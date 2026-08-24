// Unit tests for SidecarStreamDockDevice — the app-side proxy for the mirajazz Rust sidecar.
//
// Before this file the class had NO dedicated unit test (only the offscreen QML smoke target
// *compiled* it). These cover the process-free surface: construction from a real sidecar
// DeviceDescriptor, the capability-interface wiring, descriptor-derived geometry, and that the
// pre-open lifecycle (incl. the keep-alive guard) is crash-safe without a spawned sidecar.
//
// The keep-alive WIRE command (buildKeepAlive) is unit-tested in test_sidecar_protocol.cpp;
// keepAlive() short-circuits while the device is closed (no process), so here we assert it is a
// safe no-op in that state rather than re-asserting the codec.

#include "sidecar_stream_dock_device.hpp"

#include <string>
#include <utility>
#include <vector>

#include <ajazz/core/capabilities.hpp>
#include <ajazz/core/device.hpp>
#include <ajazz/streamdeck/streamdeck.hpp>
#include <catch2/catch_test_macros.hpp>

using ajazz::app::SidecarStreamDockDevice;
using ajazz::core::DeviceDescriptor;
using ajazz::core::DeviceId;

namespace {

/// Pick a sidecar descriptor that has encoders + a touch strip (the AKP05/N4 family) so the
/// encoder/touch capability assertions are meaningful.
DeviceDescriptor pickEncoderDescriptor() {
    auto const descs = ajazz::streamdeck::streamDockSidecarDescriptors();
    REQUIRE_FALSE(descs.empty());
    for (auto const& d : descs) {
        if (d.encoderCount > 0) {
            return d;
        }
    }
    return descs.front();
}

} // namespace

TEST_CASE("SidecarStreamDockDevice constructs closed and exposes its descriptor",
          "[sidecar][device]") {
    auto const desc = pickEncoderDescriptor();
    SidecarStreamDockDevice dev(desc, DeviceId{desc.vendorId, desc.productId, "TEST-SERIAL"});

    REQUIRE_FALSE(dev.isOpen()); // not open until open() spawns the sidecar
    REQUIRE(dev.descriptor().codename == desc.codename);
    REQUIRE(dev.id().vendorId == desc.vendorId);
    REQUIRE(dev.id().productId == desc.productId);
    REQUIRE(dev.firmwareVersion() == "unknown"); // placeholder until a `connected` event arrives
}

TEST_CASE("SidecarStreamDockDevice implements the display/encoder/touch-strip capabilities",
          "[sidecar][device]") {
    auto const desc = pickEncoderDescriptor();
    SidecarStreamDockDevice dev(desc, DeviceId{desc.vendorId, desc.productId, "TEST-SERIAL"});

    // The capability interfaces are reachable (the registry/services dynamic_cast to these).
    REQUIRE(dynamic_cast<ajazz::core::IDisplayCapable*>(&dev) != nullptr);
    REQUIRE(dynamic_cast<ajazz::core::IEncoderCapable*>(&dev) != nullptr);
    REQUIRE(dynamic_cast<ajazz::core::ITouchStripDisplayCapable*>(&dev) != nullptr);

    // Encoder count is descriptor-driven, not hardcoded.
    REQUIRE(dev.encoderInfo().count == static_cast<std::uint8_t>(desc.encoderCount));
}

TEST_CASE("SidecarStreamDockDevice output calls are safe no-ops while closed",
          "[sidecar][device]") {
    auto const desc = pickEncoderDescriptor();
    SidecarStreamDockDevice dev(desc, DeviceId{desc.vendorId, desc.productId, "TEST-SERIAL"});

    // None of these may crash or write when there is no spawned sidecar (isOpen()==false).
    std::vector<std::uint8_t> rgba(4, 0);
    REQUIRE_NOTHROW(dev.setBrightness(40));
    REQUIRE_NOTHROW(dev.keepAlive()); // WR-05: the formerly-no-op guard must stay crash-safe closed
    REQUIRE_NOTHROW(dev.setKeyImage(0, rgba, 1, 1));
    REQUIRE_NOTHROW(dev.flush());
    REQUIRE_FALSE(dev.isOpen());
}

// ---------------------------------------------------------------------------
// Family-aware input + key mapping (2026-08-21).
//
// Before this the AKP05's tables were applied to every family, which quietly
// broke the AKP03 / N3 in two ways that both look like "I plug it in and
// nothing happens":
//   * key images went to wire slots 10..14, past the end of its 9 surfaces;
//   * the three plain buttons and encoder 2 had no code-table entry at all,
//     and encoder 0's codes landed on encoder 2.
//
// The reference for the AKP03 codes is `4ndv/opendeck-akp03` inputs.rs, which
// matches docs/protocols/streamdeck/akp03.md "Action codes (input reports
// byte 9)".
// ---------------------------------------------------------------------------

using ajazz::app::DockFamily;
using ajazz::app::familyOf;
using ajazz::app::hwKeyForKeyIndex;
using ajazz::app::keySourcePx;
using ajazz::app::mapSidecarInput;
using Kind = ajazz::core::DeviceEvent::Kind;

namespace {

/// Look up a registered descriptor by codename.
DeviceDescriptor descriptorFor(std::string const& codename) {
    for (auto const& d : ajazz::streamdeck::streamDockSidecarDescriptors()) {
        if (d.codename == codename) {
            return d;
        }
    }
    FAIL("no descriptor registered for codename " << codename);
    return {};
}

} // namespace

TEST_CASE("descriptor geometry classifies each Stream Dock family", "[sidecar][device][family]") {
    REQUIRE(familyOf(descriptorFor("akp05e")) == DockFamily::Akp05);
    REQUIRE(familyOf(descriptorFor("akp03e")) == DockFamily::Akp03);
    REQUIRE(familyOf(descriptorFor("akp03e_rev2")) == DockFamily::Akp03);
    REQUIRE(familyOf(descriptorFor("akp153")) == DockFamily::Akp153);
}

TEST_CASE("key images address contiguous wire slots outside the AKP05",
          "[sidecar][device][image]") {
    // AKP05: the hardware-confirmed non-contiguous remap stays.
    REQUIRE(hwKeyForKeyIndex(DockFamily::Akp05, 1) == 10);
    REQUIRE(hwKeyForKeyIndex(DockFamily::Akp05, 5) == 14);
    REQUIRE(hwKeyForKeyIndex(DockFamily::Akp05, 6) == 5);
    REQUIRE(hwKeyForKeyIndex(DockFamily::Akp05, 10) == 9);

    // AKP03 has 9 surfaces; every key must land inside them (mirajazz takes a
    // 0-based index and writes key+1 on the wire).
    for (std::uint8_t k = 1; k <= 9; ++k) {
        REQUIRE(hwKeyForKeyIndex(DockFamily::Akp03, k) == k - 1);
    }
    REQUIRE(hwKeyForKeyIndex(DockFamily::Akp153, 1) == 0);
    REQUIRE(hwKeyForKeyIndex(DockFamily::Akp153, 15) == 14);
}

TEST_CASE("AKP03 encoder twists map to the right knob", "[sidecar][device][input][akp03]") {
    struct Row {
        std::uint8_t code;
        std::uint16_t encoder;
        int delta;
    };
    // opendeck-akp03 read_encoder_value: 0x90/0x91 left, 0x50/0x51 middle,
    // 0x60/0x61 right.
    constexpr Row rows[] = {
        {0x90, 0, -1},
        {0x91, 0, 1},
        {0x50, 1, -1},
        {0x51, 1, 1},
        {0x60, 2, -1},
        {0x61, 2, 1},
    };
    for (auto const& r : rows) {
        CAPTURE(r.code);
        auto const ev = mapSidecarInput(DockFamily::Akp03, r.code, 0);
        REQUIRE(ev.has_value());
        REQUIRE(ev->kind == Kind::EncoderTurned);
        REQUIRE(ev->index == r.encoder);
        REQUIRE(ev->value == r.delta);
    }
}

TEST_CASE("AKP03 encoder presses map to the right knob", "[sidecar][device][input][akp03]") {
    // opendeck-akp03 read_encoder_press: 0x33 -> 0, 0x35 -> 1, 0x34 -> 2.
    constexpr std::pair<std::uint8_t, std::uint16_t> rows[] = {{0x33, 0}, {0x35, 1}, {0x34, 2}};
    for (auto const& [code, encoder] : rows) {
        CAPTURE(code);
        auto const down = mapSidecarInput(DockFamily::Akp03, code, 1);
        REQUIRE(down.has_value());
        REQUIRE(down->kind == Kind::EncoderPressed);
        REQUIRE(down->index == encoder);

        auto const up = mapSidecarInput(DockFamily::Akp03, code, 0);
        REQUIRE(up.has_value());
        REQUIRE(up->kind == Kind::EncoderReleased);
        REQUIRE(up->index == encoder);
    }
}

TEST_CASE("AKP03 LCD keys and plain buttons are all reachable", "[sidecar][device][input][akp03]") {
    // LCD grid: codes 1..6 are the 1-based key index.
    for (std::uint8_t code = 1; code <= 6; ++code) {
        CAPTURE(code);
        auto const ev = mapSidecarInput(DockFamily::Akp03, code, 1);
        REQUIRE(ev.has_value());
        REQUIRE(ev->kind == Kind::KeyPressed);
        REQUIRE(ev->index == code);
    }
    // The three non-LCD buttons continue the numbering at 7..9.
    constexpr std::pair<std::uint8_t, std::uint16_t> buttons[] = {{0x25, 7}, {0x30, 8}, {0x31, 9}};
    for (auto const& [code, index] : buttons) {
        CAPTURE(code);
        auto const down = mapSidecarInput(DockFamily::Akp03, code, 1);
        REQUIRE(down.has_value());
        REQUIRE(down->kind == Kind::KeyPressed);
        REQUIRE(down->index == index);

        auto const up = mapSidecarInput(DockFamily::Akp03, code, 0);
        REQUIRE(up.has_value());
        REQUIRE(up->kind == Kind::KeyReleased);
    }
}

TEST_CASE("AKP03 idle NOP frames stay unmapped", "[sidecar][device][input][akp03]") {
    // Code 0x00 is the keep-alive frame; emitting it as key 0 would fire a
    // phantom press on every idle tick.
    REQUIRE_FALSE(mapSidecarInput(DockFamily::Akp03, 0x00, 0).has_value());
}

TEST_CASE("the AKP05 code table is unchanged", "[sidecar][device][input][akp05]") {
    // Regression guard for the family split: 0x90 is encoder 2 on the AKP05 and
    // encoder 0 on the AKP03 — the two tables must not be merged.
    auto const akp05 = mapSidecarInput(DockFamily::Akp05, 0x90, 0);
    REQUIRE(akp05.has_value());
    REQUIRE(akp05->kind == Kind::EncoderTurned);
    REQUIRE(akp05->index == 2);

    auto const akp03 = mapSidecarInput(DockFamily::Akp03, 0x90, 0);
    REQUIRE(akp03.has_value());
    REQUIRE(akp03->index == 0);
}

TEST_CASE("per-key render size follows the family", "[sidecar][device][image]") {
    REQUIRE(keySourcePx(DockFamily::Akp05) == 112);
    REQUIRE(keySourcePx(DockFamily::Akp03) == 64);
    REQUIRE(keySourcePx(DockFamily::Akp153) == 85);
}

TEST_CASE("AKP03 encoders have no screens and reject zone uploads",
          "[sidecar][device][encoder][akp03]") {
    auto const desc = descriptorFor("akp03e");
    SidecarStreamDockDevice dev(desc, DeviceId{desc.vendorId, desc.productId, "TEST-SERIAL"});

    REQUIRE(dev.encoderInfo().count == 3);
    // The AKP03's three knobs are bare — advertising screens made the app try to
    // paint 128x128 touch-strip zones onto a device that has none.
    REQUIRE_FALSE(dev.encoderInfo().hasScreens);
    REQUIRE(dev.touchStripInfo().zoneCount == 0);

    std::vector<std::uint8_t> rgba(4, 0);
    REQUIRE_NOTHROW(dev.setEncoderImage(0, rgba, 1, 1)); // no-op, no crash
}

TEST_CASE("AKP03 declares all nine bindable buttons, not just the six with screens",
          "[sidecar][device][geometry][akp03]") {
    // The three plain buttons under the LCD grid emit input (0x25/0x30/0x31 ->
    // KeyPressed 7/8/9 -> Profile::keys[6..8]), but the editor draws keyCount
    // cells and routes anything past keyCount to the ENCODER slots. While
    // keyCount was 6 those three buttons had nowhere to bind: confirmed dead in
    // the UI on a 0x0300:0x3002 unit whose owner could see only the six keys and
    // three dials.
    for (auto const& codename : {"akp03", "akp03e", "akp03e_rev2", "mirabox_n3"}) {
        auto const d = descriptorFor(codename);
        CAPTURE(codename);
        REQUIRE(d.keyCount == 9);       // 6 LCD + 3 plain
        REQUIRE(d.gridColumns == 3);    // 3 across
        REQUIRE(d.keyRows == 3);        // 2 LCD rows + the plain-button row
        REQUIRE(d.encoderCount == 3);   // dials stay separate (Profile::encoders)
        REQUIRE(d.touchZoneCount == 0); // no strip on this family
    }
}

TEST_CASE("every AKP03 button index maps to a wire slot that exists",
          "[sidecar][device][geometry][akp03]") {
    // Raising keyCount to 9 is only safe because the wire has nine surfaces:
    // key 9 must not address slot 9 or beyond (mirajazz writes key + 1).
    auto const d = descriptorFor("akp03e_rev2");
    for (std::uint8_t k = 1; k <= d.keyCount; ++k) {
        CAPTURE(k);
        REQUIRE(hwKeyForKeyIndex(DockFamily::Akp03, k) < 9);
    }
}

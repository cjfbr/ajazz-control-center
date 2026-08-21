// SPDX-License-Identifier: GPL-3.0-or-later
/** @file sidecar_stream_dock_device.cpp
 *  @brief Implementation of the mirajazz-sidecar-backed AKP05/N4 backend.
 */
#include "sidecar_stream_dock_device.hpp"

#include "ajazz/core/logger.hpp"
#include "sidecar_protocol.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

#include <array>
#include <stdexcept>
#include <utility>

namespace ajazz::app {

DockFamily familyOf(core::DeviceDescriptor const& d) noexcept {
    if (d.touchZoneCount > 0) {
        return DockFamily::Akp05;
    }
    if (d.encoderCount > 0) {
        return DockFamily::Akp03;
    }
    return DockFamily::Akp153;
}

/// Map our 1-based key index to the mirajazz hardware index for `set_button_image`.
///
/// mirajazz takes a 0-based index and writes `key + 1` on the wire, so for the
/// families whose surfaces are contiguous the mapping is simply "minus one".
/// The AKP05 is the exception: its BAT wire slots are not contiguous with the
/// visual grid (opendeck mappings.rs: top row 10..14, bottom 5..9, slot 5 dead),
/// hardware-confirmed on the 0x0300:0x3004 unit.
///
/// Before this was family-aware every family got the AKP05 remap, so an AKP03
/// key 1..5 was uploaded to wire slots 10..14 — past the end of its 9 surfaces.
/// The upload was accepted and rendered nowhere: the panel simply stayed dark.
std::uint8_t hwKeyForKeyIndex(DockFamily family, std::uint8_t oneBased) noexcept {
    if (family != DockFamily::Akp05) {
        return oneBased > 0 ? static_cast<std::uint8_t>(oneBased - 1) : oneBased;
    }
    if (oneBased >= 1 && oneBased <= 5) {
        return static_cast<std::uint8_t>(oneBased + 9); // 1->10 .. 5->14
    }
    if (oneBased >= 6 && oneBased <= 10) {
        return static_cast<std::uint8_t>(oneBased - 1); // 6->5 .. 10->9
    }
    return oneBased;
}

namespace {

/// AKP05 / N4 input codes (opendeck-akp05 inputs.rs, N4-derived).
///
/// PROVISIONAL: the 0x3004 demo unit emits no input, so this table is
/// calibrated from upstream rather than from local hardware.
[[nodiscard]] std::optional<core::DeviceEvent> mapAkp05Input(std::uint8_t code,
                                                             std::uint8_t state) {
    using Kind = core::DeviceEvent::Kind;
    core::DeviceEvent e{};

    switch (code) {
    // Encoder twist: low byte = -1, high byte = +1, per encoder.
    case 0xA0:
        e = {Kind::EncoderTurned, 0, -1};
        return e;
    case 0xA1:
        e = {Kind::EncoderTurned, 0, 1};
        return e;
    case 0x50:
        e = {Kind::EncoderTurned, 1, -1};
        return e;
    case 0x51:
        e = {Kind::EncoderTurned, 1, 1};
        return e;
    case 0x90:
        e = {Kind::EncoderTurned, 2, -1};
        return e;
    case 0x91:
        e = {Kind::EncoderTurned, 2, 1};
        return e;
    case 0x70:
        e = {Kind::EncoderTurned, 3, -1};
        return e;
    case 0x71:
        e = {Kind::EncoderTurned, 3, 1};
        return e;

    // Encoder press (0x37->0, 0x35->1, 0x33->2, 0x36->3).
    case 0x37:
        e = {state ? Kind::EncoderPressed : Kind::EncoderReleased, 0, state};
        return e;
    case 0x35:
        e = {state ? Kind::EncoderPressed : Kind::EncoderReleased, 1, state};
        return e;
    case 0x33:
        e = {state ? Kind::EncoderPressed : Kind::EncoderReleased, 2, state};
        return e;
    case 0x36:
        e = {state ? Kind::EncoderPressed : Kind::EncoderReleased, 3, state};
        return e;

    // Touch-zone taps (one per encoder) -> encoder press.
    case 0x40:
        e = {Kind::EncoderPressed, 0, 1};
        return e;
    case 0x41:
        e = {Kind::EncoderPressed, 1, 1};
        return e;
    case 0x42:
        e = {Kind::EncoderPressed, 2, 1};
        return e;
    case 0x43:
        e = {Kind::EncoderPressed, 3, 1};
        return e;

    default:
        // Physical LCD keys report their 1-based index directly (1..10).
        if (code >= 1 && code <= 10) {
            e = {state ? Kind::KeyPressed : Kind::KeyReleased, code, state};
            return e;
        }
        return std::nullopt;
    }
}

/// AKP03 / N3 input codes.
///
/// Transcribed from `4ndv/opendeck-akp03` `inputs.rs` (`process_input`), which
/// agrees with this repo's RE table in
/// `docs/protocols/streamdeck/akp03.md` ("Action codes (input reports byte 9)").
///
/// The AKP05 table used to be applied to this family too, which silently broke
/// most of the device: the three plain buttons (0x25/0x30/0x31) and encoder 2
/// (0x60/0x61 twist, 0x34 press) had no entry at all and were dropped, while
/// 0x90/0x91 and 0x33 landed on encoder 2 instead of encoder 0.
[[nodiscard]] std::optional<core::DeviceEvent> mapAkp03Input(std::uint8_t code,
                                                             std::uint8_t state) {
    using Kind = core::DeviceEvent::Kind;
    core::DeviceEvent e{};

    switch (code) {
    // --- Encoder twist. One frame per detent, no release edge. -----------
    case 0x90: // left / large knob
        e = {Kind::EncoderTurned, 0, -1};
        return e;
    case 0x91:
        e = {Kind::EncoderTurned, 0, 1};
        return e;
    case 0x50: // middle (top)
        e = {Kind::EncoderTurned, 1, -1};
        return e;
    case 0x51:
        e = {Kind::EncoderTurned, 1, 1};
        return e;
    case 0x60: // right
        e = {Kind::EncoderTurned, 2, -1};
        return e;
    case 0x61:
        e = {Kind::EncoderTurned, 2, 1};
        return e;

    // --- Encoder press (0x33 -> 0, 0x35 -> 1, 0x34 -> 2). ----------------
    case 0x33:
        e = {state ? Kind::EncoderPressed : Kind::EncoderReleased, 0, state};
        return e;
    case 0x35:
        e = {state ? Kind::EncoderPressed : Kind::EncoderReleased, 1, state};
        return e;
    case 0x34:
        e = {state ? Kind::EncoderPressed : Kind::EncoderReleased, 2, state};
        return e;

    // --- The three plain (non-LCD) buttons under the grid. ---------------
    // They continue the key numbering: keys 1..6 are the LCD grid, 7..9 these.
    case 0x25:
        e = {state ? Kind::KeyPressed : Kind::KeyReleased, 7, state};
        return e;
    case 0x30:
        e = {state ? Kind::KeyPressed : Kind::KeyReleased, 8, state};
        return e;
    case 0x31:
        e = {state ? Kind::KeyPressed : Kind::KeyReleased, 9, state};
        return e;

    default:
        // LCD keys report their 1-based index directly (1..6). Code 0x00 is
        // the idle/keep-alive NOP frame and must stay unmapped.
        if (code >= 1 && code <= 6) {
            e = {state ? Kind::KeyPressed : Kind::KeyReleased, code, state};
            return e;
        }
        return std::nullopt;
    }
}

/// AKP153 / HSV293S input codes — a plain key grid, 1-based index in byte 9.
[[nodiscard]] std::optional<core::DeviceEvent> mapAkp153Input(std::uint8_t code,
                                                              std::uint8_t state) {
    using Kind = core::DeviceEvent::Kind;
    if (code >= 1 && code <= 18) {
        return core::DeviceEvent{state ? Kind::KeyPressed : Kind::KeyReleased, code, state};
    }
    return std::nullopt;
}

} // namespace

/// Translate a sidecar (code,state) pair into a core::DeviceEvent for `family`.
std::optional<core::DeviceEvent>
mapSidecarInput(DockFamily family, std::uint8_t code, std::uint8_t state) {
    switch (family) {
    case DockFamily::Akp05:
        return mapAkp05Input(code, state);
    case DockFamily::Akp03:
        return mapAkp03Input(code, state);
    case DockFamily::Akp153:
        return mapAkp153Input(code, state);
    }
    return std::nullopt;
}

/// Nominal per-key source size the app renders at, per family. The sidecar
/// (mirajazz) rescales to the SKU's real wire format, so this only decides how
/// much detail the render pipeline hands over — it must not be smaller than the
/// wire size or the panel gets an upscaled key.
std::uint16_t keySourcePx(DockFamily family) noexcept {
    switch (family) {
    case DockFamily::Akp05:
        return 112;
    case DockFamily::Akp03:
        return 64; // 60x60 on pv2 silicon, 64x64 on pv3 — render at the larger.
    case DockFamily::Akp153:
        return 85;
    }
    return 112;
}

QString defaultSidecarBinary() {
    QString const env = qEnvironmentVariable("AJAZZ_STREAMDOCK_HOST");
    if (!env.isEmpty()) {
        return env;
    }
    QString const beside = QCoreApplication::applicationDirPath() + "/streamdock-host";
    if (QFileInfo::exists(beside)) {
        return beside;
    }
    QString const onPath = QStandardPaths::findExecutable("streamdock-host");
    if (!onPath.isEmpty()) {
        return onPath;
    }
    return QStringLiteral("streamdock-host");
}

SidecarStreamDockDevice::SidecarStreamDockDevice(core::DeviceDescriptor descriptor,
                                                 core::DeviceId id,
                                                 SidecarBinaryResolver resolver)
    : m_descriptor(std::move(descriptor)), m_id(std::move(id)),
      m_resolver(resolver ? std::move(resolver) : SidecarBinaryResolver(&defaultSidecarBinary)) {}

SidecarStreamDockDevice::~SidecarStreamDockDevice() {
    close();
}

core::DeviceDescriptor const& SidecarStreamDockDevice::descriptor() const noexcept {
    return m_descriptor;
}

core::DeviceId SidecarStreamDockDevice::id() const noexcept {
    return m_id;
}

std::string SidecarStreamDockDevice::firmwareVersion() const {
    std::lock_guard const lock(m_mutex);
    return m_firmwareVersion;
}

void SidecarStreamDockDevice::open() {
    if (isOpen()) {
        return;
    }
    QString const exe = m_resolver();

    m_process = std::make_unique<QProcess>();
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    // Scope the sidecar to THIS device. Without the filter it opens every
    // Stream Dock it can find and the app adopts whichever serial arrives
    // first, so with two docks attached one proxy could drive the other's
    // panel — and with none openable it still reported success.
    m_process->start(
        exe,
        QStringList{QStringLiteral("--allow-output"),
                    QStringLiteral("--vid"),
                    QStringLiteral("0x%1").arg(m_id.vendorId, 4, 16, QLatin1Char('0')),
                    QStringLiteral("--pid"),
                    QStringLiteral("0x%1").arg(m_id.productId, 4, 16, QLatin1Char('0'))});

    if (!m_process->waitForStarted(3000)) {
        QString const err = m_process->errorString();
        m_process.reset();
        throw std::runtime_error("streamdock-host failed to start (" + exe.toStdString() +
                                 "): " + err.toStdString());
    }

    // Blocking handshake: drain lines until the sidecar emits "ready".
    QElapsedTimer timer;
    timer.start();
    m_ready = false;
    m_deviceConnected = false;
    while (timer.elapsed() < 5000 && !m_ready) {
        if (m_process->state() != QProcess::Running) {
            break;
        }
        if (m_process->waitForReadyRead(500)) {
            drainStdout();
        }
    }
    if (!m_ready) {
        QString const err = m_process->errorString();
        close();
        throw std::runtime_error("streamdock-host did not become ready: " + err.toStdString());
    }

    // "ready" only means enumeration finished — it says nothing about whether
    // our device was actually opened. Treating it as success was why a device
    // that enumerates but cannot be opened (missing udev `uaccess` rule, wrong
    // protocol version, already-held handle) looked connected in the UI while
    // every command silently went to serial "" and was answered with
    // "no device". Fail loudly instead.
    if (!m_deviceConnected) {
        close();
        QString const usb = QStringLiteral("%1:%2")
                                .arg(m_id.vendorId, 4, 16, QLatin1Char('0'))
                                .arg(m_id.productId, 4, 16, QLatin1Char('0'));
        throw std::runtime_error(
            "streamdock-host enumerated but could not open " + m_descriptor.model + " (" +
            usb.toStdString() +
            "). On Linux this is usually a missing udev rule: install "
            "resources/linux/70-ajazz.rules, reload udev and replug the device. Run "
            "`streamdock-host --list` to see what the HID layer exposes.");
    }

    // Switch to async input: deliver subsequent stdout via the event loop.
    // Context object = the process, so the connection dies with it; no QObject
    // on this class, hence no MOC.
    QObject::connect(m_process.get(),
                     &QProcess::readyReadStandardOutput,
                     m_process.get(),
                     [this]() { drainStdout(); });
    drainStdout(); // flush anything buffered between the loop exit and connect

    AJAZZ_LOG_INFO("sidecar", "device opened: {} (fw {})", m_descriptor.model, m_firmwareVersion);
}

void SidecarStreamDockDevice::close() {
    if (!m_process) {
        return;
    }
    if (m_process->state() != QProcess::NotRunning) {
        // EOF on stdin makes the sidecar's command loop end and exit cleanly
        // (closing its one HID handle); kill is the fallback.
        m_process->closeWriteChannel();
        if (!m_process->waitForFinished(1000)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
    }
    m_process.reset();
    m_ready = false;
    m_deviceConnected = false;
}

bool SidecarStreamDockDevice::isOpen() const noexcept {
    return m_process && m_process->state() == QProcess::Running && m_ready;
}

void SidecarStreamDockDevice::onEvent(core::EventCallback cb) {
    std::lock_guard const lock(m_mutex);
    m_callback = std::move(cb);
}

std::size_t SidecarStreamDockDevice::poll() {
    return 0; // input arrives asynchronously via readyReadStandardOutput
}

core::DisplayInfo SidecarStreamDockDevice::displayInfo() const noexcept {
    core::DisplayInfo info{};
    // Per family, not a hardcoded AKP05 112x112: an AKP03 key is 60/64 px and
    // an AKP153 key 85 px on the wire. The sidecar rescales whatever it gets,
    // but the app sizes its render surface from this.
    auto const px = keySourcePx(familyOf(m_descriptor));
    info.widthPx = px;
    info.heightPx = px;
    info.keyRows = m_descriptor.keyRows;
    info.keyCols = static_cast<std::uint8_t>(m_descriptor.gridColumns);
    info.jpegEncoded = true;
    return info;
}

void SidecarStreamDockDevice::setKeyImage(std::uint8_t keyIndex,
                                          std::span<std::uint8_t const> rgba,
                                          std::uint16_t width,
                                          std::uint16_t height) {
    if (!isOpen()) {
        return;
    }
    writeCommand(sidecar::buildSetImage(effectiveSerial(),
                                        hwKeyForKeyIndex(familyOf(m_descriptor), keyIndex),
                                        /*touchzone=*/false,
                                        width,
                                        height,
                                        rgba));
}

void SidecarStreamDockDevice::setKeyColor(std::uint8_t keyIndex, core::Rgb color) {
    std::array<std::uint8_t, 4> const px{color.r, color.g, color.b, 255};
    setKeyImage(keyIndex, px, 1, 1); // mirajazz resizes the 1x1 to a solid key
}

void SidecarStreamDockDevice::clearKey(std::uint8_t keyIndex) {
    if (keyIndex == 0xFF) {
        for (std::uint8_t k = 1; k <= static_cast<std::uint8_t>(m_descriptor.keyCount); ++k) {
            setKeyColor(k, core::Rgb{0, 0, 0});
        }
        return;
    }
    setKeyColor(keyIndex, core::Rgb{0, 0, 0});
}

void SidecarStreamDockDevice::setMainImage(std::span<std::uint8_t const> rgba,
                                           std::uint16_t width,
                                           std::uint16_t height) {
    // AKP05's touch strip is four discrete encoder zones, not one main image;
    // callers paint it via setEncoderImage. No-op here.
    (void)rgba;
    (void)width;
    (void)height;
}

void SidecarStreamDockDevice::setBrightness(std::uint8_t percent) {
    if (!isOpen()) {
        return;
    }
    writeCommand(sidecar::buildSetBrightness(effectiveSerial(), percent));
}

void SidecarStreamDockDevice::flush() {
    // The sidecar flushes after each set_image, so there is nothing to commit.
}

void SidecarStreamDockDevice::keepAlive() {
    // WR-05 / idle-wedge guard: send keep_alive so the sidecar emits mirajazz keep_alive()
    // (CRT CONNECT) on the persistent handle. Driven by StreamDockControlService's keep-alive
    // timer. (Previously a no-op, so the timer fired into the void and the panel could wedge on
    // idle despite the persistent handle.)
    if (!isOpen()) {
        return;
    }
    writeCommand(sidecar::buildKeepAlive(effectiveSerial()));
}

core::EncoderInfo SidecarStreamDockDevice::encoderInfo() const noexcept {
    core::EncoderInfo info{};
    info.count = static_cast<std::uint8_t>(m_descriptor.encoderCount);
    info.pressable = true;
    // Only the AKP05 family backs its encoders with touch-strip zones; the
    // AKP03's three knobs have no screen at all.
    info.hasScreens = m_descriptor.touchZoneCount > 0;
    info.stepsPerRevolution = 0; // endless
    return info;
}

void SidecarStreamDockDevice::setEncoderImage(std::uint8_t index,
                                              std::span<std::uint8_t const> rgba,
                                              std::uint16_t width,
                                              std::uint16_t height) {
    // Families without touch zones (AKP03, AKP153) have no encoder screens;
    // sending a zone upload there would push a 128x128 zone-format image at a
    // device that has no such surface.
    if (!isOpen() || index >= m_descriptor.touchZoneCount) {
        return;
    }
    // Encoder touch zones are mirajazz hardware indices 0..3.
    writeCommand(
        sidecar::buildSetImage(effectiveSerial(), index, /*touchzone=*/true, width, height, rgba));
}

core::TouchStripInfo SidecarStreamDockDevice::touchStripInfo() const noexcept {
    core::TouchStripInfo info{};
    if (m_descriptor.touchZoneCount > 0) {
        info.widthPx = 800; // AKP05 strip: 800x480, 4 zones of 200x480.
        info.heightPx = 480;
        info.zoneCount = m_descriptor.touchZoneCount;
    }
    return info;
}

bool SidecarStreamDockDevice::setTouchStripImage(std::span<std::uint8_t const> rgba,
                                                 std::uint16_t srcWidth,
                                                 std::uint16_t srcHeight,
                                                 std::uint8_t location,
                                                 std::uint16_t /*x*/,
                                                 std::uint16_t /*y*/,
                                                 std::uint16_t /*rectWidth*/,
                                                 std::uint16_t /*rectHeight*/) {
    if (!isOpen() || location >= m_descriptor.touchZoneCount) {
        return false;
    }
    // Zone `location` maps to the sidecar's touch-zone set_image (index-addressed
    // render). The C++ "DRA" rect geometry is irrelevant to mirajazz's per-zone
    // discrete LCD model.
    writeCommand(sidecar::buildSetImage(
        effectiveSerial(), location, /*touchzone=*/true, srcWidth, srcHeight, rgba));
    return true;
}

bool SidecarStreamDockDevice::clearTouchStrip() {
    if (!isOpen()) {
        return false;
    }
    std::array<std::uint8_t, 4> const black{0, 0, 0, 255};
    for (std::uint8_t zone = 0; zone < m_descriptor.touchZoneCount; ++zone) {
        writeCommand(
            sidecar::buildSetImage(effectiveSerial(), zone, /*touchzone=*/true, 1, 1, black));
    }
    return true;
}

QString SidecarStreamDockDevice::effectiveSerial() const {
    if (!m_sidecarSerial.isEmpty()) {
        return m_sidecarSerial;
    }
    return QString::fromStdString(m_id.serial);
}

void SidecarStreamDockDevice::writeCommand(QByteArray const& line) {
    if (m_process && m_process->state() == QProcess::Running) {
        m_process->write(line);
    }
}

void SidecarStreamDockDevice::drainStdout() {
    if (!m_process) {
        return;
    }
    m_lineBuffer += m_process->readAllStandardOutput();
    for (qsizetype nl = m_lineBuffer.indexOf('\n'); nl >= 0; nl = m_lineBuffer.indexOf('\n')) {
        QByteArray const line = m_lineBuffer.left(nl);
        m_lineBuffer.remove(0, nl + 1);
        if (!line.trimmed().isEmpty()) {
            handleLine(line);
        }
    }
}

void SidecarStreamDockDevice::handleLine(QByteArray const& line) {
    auto const ev = sidecar::parseEvent(line);
    if (!ev) {
        return;
    }
    using Type = sidecar::SidecarEvent::Type;
    switch (ev->type) {
    case Type::Connected: {
        // Adopt the serial only when the sidecar opened OUR device. A sidecar
        // that enumerated a different dock must not have its handle proxied
        // through this object.
        if (ev->vid != m_id.vendorId || ev->pid != m_id.productId) {
            AJAZZ_LOG_WARN("sidecar",
                           "ignoring connected event for {:04x}:{:04x} (this proxy is "
                           "{:04x}:{:04x})",
                           ev->vid,
                           ev->pid,
                           m_id.vendorId,
                           m_id.productId);
            break;
        }
        {
            std::lock_guard const lock(m_mutex);
            if (!ev->firmware.isEmpty()) {
                m_firmwareVersion = ev->firmware.toStdString();
            }
        }
        m_sidecarSerial = ev->serial;
        m_deviceConnected = true;
        break;
    }
    case Type::Ready:
        m_ready = true;
        break;
    case Type::Input: {
        auto const devEv = mapSidecarInput(familyOf(m_descriptor), ev->code, ev->state);
        // Log EVERY input frame the mirajazz sidecar delivers — mapped or not.
        // The AKP05 code map is PROVISIONAL (N4-derived; the 0x3004 demo unit
        // emits nothing to calibrate against), so an unmapped code was dropped
        // silently before: on a retail/Pro unit this line is how the real code
        // map gets calibrated straight from `scripts/ajazz-debug log.tail`.
        AJAZZ_LOG_INFO("sidecar",
                       "input: code=0x{:02x} state={} -> {}",
                       ev->code,
                       ev->state,
                       devEv ? "mapped" : "UNMAPPED (calibration needed)");
        if (devEv) {
            core::EventCallback cb;
            {
                std::lock_guard const lock(m_mutex);
                cb = m_callback;
            }
            if (cb) {
                cb(*devEv);
            }
        }
        break;
    }
    case Type::Error:
        AJAZZ_LOG_WARN("sidecar", "sidecar error: {}", ev->message.toStdString());
        break;
    default:
        break;
    }
}

core::DevicePtr makeSidecarStreamDock(core::DeviceDescriptor const& d, core::DeviceId id) {
    return std::make_shared<SidecarStreamDockDevice>(d, std::move(id));
}

} // namespace ajazz::app

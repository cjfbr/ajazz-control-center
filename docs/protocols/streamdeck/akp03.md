# AJAZZ AKP03 / AKP03E / AKP03R — Mirabox N3 family

> A small desk controller built around **6 LCD keys + 3 rotary encoders + 3
> non-LCD buttons**. The N3 nickname covers a long list of rebadges sold
> under different brand names with identical firmware.
>
> ✅ **Status (2026-08-21):** the family is driven by the out-of-process
> mirajazz sidecar (`streamdock-host/`), not by in-tree C++ wire code — the
> old `akp03.cpp` was removed in the `experiment/mirajazz` Slice D. The
> per-SKU parameters (protocol version, image format) live in
> `streamdock-host/src/kind.rs`; the input code table below is implemented in
> `src/app/src/sidecar_stream_dock_device.cpp::mapAkp03Input`.

## Hardware confirmation (2026-08-24)

First physical AKP03-family unit seen by this project — everything below had
been transcribed from third-party catalogues until now.

| Field               | Observed                                      |
| ------------------- | --------------------------------------------- |
| USB                 | `0x0300:0x3002`                               |
| Product string      | `HOTSPOTEKUSB HOTSPOTEKUSB HID DEMO`          |
| Manufacturer string | `HOTSPOTEKUSB`                                |
| Control interface   | usage page `0xFFA0`, usage id `1` (`1-1:1.0`) |
| Second interface    | usage page `0x0001`, usage id `6` (`1-1:1.1`) |
| LCD keys            | **6** — confirmed by the owner, visually      |

⚠️ **The `HOTSPOTEKUSB HID DEMO` product string does NOT identify the family.**
The `0x0300:0x3004` unit carries the same white-label string and is an
**AKP05E** (10 keys + touch strip) — it sat mis-filed as a 6-key AKP03 here
until a live `CRT VER` handshake corrected it (`akp05_vendor.md` §14.1). This
`0x3002` unit really is a 6-LCD-key AKP03, but that was established from the
physical hardware, not from the string. Always confirm the family before
trusting a PID that reports this string.

Still unconfirmed on this unit: the protocol version (registered as v3 per
`opendeck-akp03`), the image format, and whether input reports are reachable —
the `0x3004` sibling's firmware ships with the input path disabled.

## Hardware

**Sources:** `[mirabox-n3]` `[ajazz-sdk]` `[opendeck-akp03]` `[companion]` — see
[`_research-sources.md`](./_research-sources.md).

| Property                    | Value                                                                                                                                                                 |
| --------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Form factor                 | Desktop macropad with detachable stand                                                                                                                                |
| Connection                  | USB-C, wired                                                                                                                                                          |
| Operating voltage / current | 5 V / 0.6 A (3.0 W typical)                                                                                                                                           |
| LCD keys                    | **6**, arranged in **2 rows × 3 columns**                                                                                                                             |
| LCD key size                | 72 × 72 px nominally, but the **image format on the wire is 60×60 JPEG** (`Rot0`, no mirror) per `[ajazz-sdk]`. The AKP03R rev. 2 uploads 64×64 with `Rot90` instead. |
| Non-LCD buttons             | **3**, positioned in a row underneath the LCD grid                                                                                                                    |
| Rotary encoders             | **3** — one larger primary knob, two smaller secondaries. All three are pressable.                                                                                    |
| Boot logo display           | One small front LCD strip, 320×240 px (uploaded JPEG at `Rot90`)                                                                                                      |
| Touch strip                 | None                                                                                                                                                                  |
| Dimensions                  | 129 × 78 × 59 mm                                                                                                                                                      |
| Weight                      | 148 g (no stand) / 200 g (with stand)                                                                                                                                 |
| Material                    | ABS body + acrylic LCD keycaps                                                                                                                                        |
| OS support                  | Windows ≥ 7, macOS ≥ 10.15, Linux (community)                                                                                                                         |
| Vendor software             | StreamDock (Mirabox), AJAZZ App                                                                                                                                       |
| Bundled features            | Hotkeys, multi-action chains, folders, scene auto-switch, animated GIF icons, drag-and-drop assignment to keys *and* knobs                                            |

### Layout sketch

From `[companion]`'s N3 mapping diagram (recreated in ASCII):

```
+------+------+------+
| LCD1 | LCD2 | LCD3 |    <- 2 rows x 3 cols of 72x72 LCD keys
+------+------+------+
| LCD4 | LCD5 | LCD6 |
+------+------+------+
|  B7  |  B8  |  B9  |    <- 3 non-LCD buttons (action codes 0x25 / 0x30 / 0x31)
+------+------+------+
|  (((( E1 ))))      |    <- "large" primary encoder (Rotary 1)
|        ((E2))((E3))|    <- 2 smaller encoders (Rotary 2 and Rotary 3)
+------+------+------+
```

### Variants and rebadges

`[ajazz-sdk]` knows the AJAZZ-branded SKUs:

| Codename in `[opendeck-akp03]` | Marketing name      | VID      | PID      | Protocol |
| ------------------------------ | ------------------- | -------- | -------- | -------- |
| `Akp03`                        | AJAZZ AKP03         | `0x0300` | `0x1001` | v2       |
| `Akp03E`                       | AJAZZ AKP03E        | `0x0300` | `0x1002` | v2       |
| `Akp03R`                       | AJAZZ AKP03R        | `0x0300` | `0x1003` | v2       |
| `Akp03Erev2`                   | AJAZZ AKP03E rev. 2 | `0x0300` | `0x3002` | v3       |
| `Akp03Rrev2`                   | AJAZZ AKP03R rev. 2 | `0x0300` | `0x3003` | v3       |

⚠️ **`0x0300:0x3002` is the rev. 2 AKP03E, not the original.** The original
AKP03E is `0x0300:0x1002`. Until 2026-08-21 this project had `0x1002`
registered as an **AKP153E** and `0x3002` as the plain AKP03E at protocol
version 2. Both were wrong in a way that made the device unusable rather than
merely degraded: the mirajazz protocol version selects the HID packet size
(512 bytes for v1, 1024 for v2+), so an AKP03E opened as a v1 AKP153E received
512-byte writes it ignored — it enumerated, the UI listed it, and nothing else
ever happened.

`[opendeck-akp03]` adds the Mirabox-branded and licensee-branded units
sharing the same firmware:

| Marketing name               | VID      | PID      |
| ---------------------------- | -------- | -------- |
| Mirabox N3                   | `0x6602` | `0x1002` |
| Mirabox N3 (rev. 3)          | `0x6603` | `0x1002` |
| Mirabox N3EN                 | `0x6603` | `0x1003` |
| Soomfon Stream Controller SE | `0x1500` | `0x3001` |
| Mars Gaming MSD-TWO          | `0x0B00` | `0x1001` |
| TreasLin N3                  | `0x5548` | `0x1001` |
| Redragon Skyrider SS-551     | `0x0200` | `0x2000` |

`register.cpp` also keeps one in-tree-only pair, `0x0300:0x3001`
(`akp03_legacy`), which appears in no upstream catalogue; it is registered at
protocol version 2 for backwards compatibility. Note that `0x0300:0x3004`,
surfaced by `[capture-2026-05-13]`, is **not** an AKP03 sibling — a live
`CRT VER` handshake proved it an AKP05E (see `akp05_vendor.md` §14.1).

### USB identifier map (canonical, post-2026-05-14)

```
protocol v2 (1024-byte packets, 60x60 Rot0 keys)
  0x0300:0x1001 AKP03
  0x0300:0x1002 AKP03E
  0x0300:0x1003 AKP03R
  0x0300:0x3001 AKP03 (legacy, in-tree only)
  0x6602:0x1000 Mirabox N3
  0x6602:0x1002 Mirabox N3
  0x6602:0x1003 Mirabox N3E (in-tree only)

protocol v3 (1024-byte packets, 64x64 Rot90 keys, both press+release edges)
  0x0300:0x3002 AKP03E rev. 2
  0x0300:0x3003 AKP03R rev. 2
  0x6603:0x1002 Mirabox N3 rev. 3
  0x6603:0x1003 Mirabox N3EN
  0x1500:0x3001 Soomfon Stream Controller SE
  0x0B00:0x1001 Mars Gaming MSD-TWO
  0x5548:0x1001 TreasLin N3
  0x0200:0x2000 Redragon Skyrider SS-551
```

Cumulatively this is **15 distinct USB identifiers** that all open the same
N3 backend; all 15 are registered in `register.cpp` and `kind.rs`.

## Features that must work

| Feature                                                                     | Required for `functional` | Required for `stable` |
| --------------------------------------------------------------------------- | ------------------------- | --------------------- |
| Open / close transport with v2 (1024-byte) framing                          | ✅                        | ✅                    |
| Read **LCD key** press / release events                                     | ✅                        | ✅                    |
| Read **non-LCD button** events (action codes `0x25`/`0x30`/`0x31`)          | ✅                        | ✅                    |
| Read **3 encoder** rotation events (CW + CCW) for all knobs                 | ✅                        | ✅                    |
| Read **3 encoder press / release** events                                   | ✅                        | ✅                    |
| Set per-key image (60×60 JPEG, `Rot0`; or 64×64 / `Rot90` on AKP03R rev. 2) | ✅                        | ✅                    |
| Set global brightness (0..100)                                              | ✅                        | ✅                    |
| Clear single / all keys                                                     | ✅                        | ✅                    |
| Set boot logo (320×240 JPEG, `Rot90`)                                       | —                         | ✅                    |
| Read firmware version (Feature Report ID `0x01`)                            | —                         | ✅                    |
| Forward animated GIF as multi-frame JPEG                                    | —                         | nice-to-have          |
| Probe sibling PIDs (`0x3004`) with capability fallback                      | —                         | ✅                    |

## Wire protocol

The AKP03 reuses the AKP153 framing **prefix** (`CRT` at bytes 0..2 +
3-byte ASCII command word at bytes 5..7) but ships **1024-byte packets**
(`is_v2_api` in `[ajazz-sdk]`). All existing AKP153 opcodes (`LIG`, `STP`,
`CLE`, `BAT`, `LOG`, `HAN`, `CONNECT`) are reused with the larger packet
size; the only new opcodes are the action codes returned in input reports.

### Action codes (input reports byte 9)

Bytes 9 carries the action code:

| Action code  | Meaning                                                                                                                   |
| ------------ | ------------------------------------------------------------------------------------------------------------------------- |
| `0x01..0x06` | LCD key 1..6 — press/release polarity not directly encoded; treat as press event with synthesised release on next reading |
| `0x25`       | Non-LCD button 7                                                                                                          |
| `0x30`       | Non-LCD button 8                                                                                                          |
| `0x31`       | Non-LCD button 9                                                                                                          |
| `0x90`       | Encoder 0 CCW (large knob)                                                                                                |
| `0x91`       | Encoder 0 CW                                                                                                              |
| `0x50`       | Encoder 1 CCW                                                                                                             |
| `0x51`       | Encoder 1 CW                                                                                                              |
| `0x60`       | Encoder 2 CCW                                                                                                             |
| `0x61`       | Encoder 2 CW                                                                                                              |
| `0x33`       | Encoder 0 press                                                                                                           |
| `0x35`       | Encoder 1 press                                                                                                           |
| `0x34`       | Encoder 2 press                                                                                                           |
| `0x00`       | NOP / keep-alive frame                                                                                                    |

ℹ️ Implemented in `mapAkp03Input`. Until 2026-08-21 the app applied the
**AKP05** code table to this family, which silently dropped buttons 7-9
(`0x25`/`0x30`/`0x31`), dropped encoder 2 entirely (`0x60`/`0x61`/`0x34`) and
routed encoder 0's codes (`0x90`/`0x91`/`0x33`) to encoder 2.

### Press / release encoding

The N3 firmware does not always emit both edges of a transition — the
constraint `[companion]` documents:

- Non-LCD buttons (`0x25/0x30/0x31`) emit only **on release**.
- Encoders emit press/release pairs, but **rotation events arrive
  unidirectionally** (one frame per detent, no "release" for a rotation).
- LCD keys emit one frame per transition (same as AKP153).

The backend must synthesise the missing edge so consumers see uniform
`KeyPressed` / `KeyReleased` events.

### Image upload

Identical structure to AKP153 but with 1024-byte chunks and the per-model
image format from `[ajazz-sdk]`:

| Model                         | Encoding | Size    | Rotation | Mirror |
| ----------------------------- | -------- | ------- | -------- | ------ |
| protocol v2 (AKP03/E/R, N3)   | JPEG     | 60 × 60 | `Rot0`   | none   |
| protocol v3 (rev. 2, N3EN, …) | JPEG     | 64 × 64 | `Rot90`  | none   |

The split is by protocol version, not by marketing name — see the USB
identifier map above and `streamdock-host/src/kind.rs::key_image_format`.

Boot logo:

| Field    | Value                              |
| -------- | ---------------------------------- |
| Encoding | JPEG                               |
| Size     | 240 × 320 (`Rot90` when displayed) |
| Opcode   | `LOG` (v2 header `4C 4F 47 00 00`) |

## Edge cases and quirks

- **Per-event press/release asymmetry** (above) is the source of the
  current `EncoderReleased → EncoderPressed value=0` workaround in
  `poll()`. The fix is the same as for AKP05: extend `core::DeviceEvent`
  with an `EncoderReleased` kind.
- The AKP03 emits *NOP frames* (`action code 0x00`) at idle for
  keep-alive — they must be silently discarded by the parser.
- Three different USB vendor IDs (`0x0300`, `0x5548`, `0x6602/3`) all map
  to the same firmware behaviour, so the geometry should be selected by
  PID alone after the VID/PID is matched.
- `0x0300:0x3004` (`HOTSPOTEKUSB HID DEMO`) registered via
  `[capture-2026-05-13]` is **not** in any third-party catalogue. Until a
  capture is annotated, treat it conservatively: open with default AKP03
  geometry and log a warning if any byte pattern in the input report
  diverges from the table above.

## Cross-references

- `[ajazz-sdk]` — VID/PID, geometry, opcode table, per-encoder action codes
- `[opendeck-akp03]` — rebadge USB IDs (full list above)
- `[companion]` — N3 layout sketch + press/release edge semantics
- `[mirabox-n3]` — vendor product page (dimensions, weight, materials)
- [`akp153.md`](./akp153.md) — sister 15-key device on the v1 API
- [`akp05.md`](./akp05.md) — full 10-key Plus-class device with touch strip

## Time sync

**Status:** scaffolded — not yet implemented.

`Akp03Device` inherits `IClockCapable` and returns
`TimeSyncResult::NotImplemented` from `setTime()`, with a WARN-once via
`s_warned_akp03` (Pitfall 14). When a wire format lands, only this
backend's body changes — UI / service / capability / glyph / Settings
toggle all stay. See
[`docs/superpowers/specs/2026-05-13-time-sync-design.md`](../../superpowers/specs/2026-05-13-time-sync-design.md).

//! streamdock-host — out-of-process Rust sidecar (mirajazz) for the Stream Dock
//! families mirajazz can drive (AKP05/N4, AKP03/N3, AKP153/HSV293S). Newline-
//! delimited JSON over stdin/stdout. See Cargo.toml for the protocol summary.
//!
//! Per-device parameters (protocol version, key/encoder count, image format)
//! come from `kind.rs`. Output commands (brightness/images) are gated behind
//! --allow-output because mirajazz `initialize()` sends `CRT DIS` (wedge risk);
//! a persistent-handle sidecar sends it once for the handle lifetime.

mod kind;

use std::{collections::HashMap, sync::Arc, time::Duration};

use base64::{engine::general_purpose::STANDARD as B64, Engine as _};
use futures_lite::StreamExt;
use image::{DynamicImage, RgbaImage};
use mirajazz::{
    device::{list_devices, new_hid_backend, Device, DeviceQuery},
    error::MirajazzError,
    types::DeviceInput,
};
use tokio::{
    io::{AsyncBufReadExt, BufReader},
    sync::Mutex,
};

use kind::{key_image_format, known_vid_pids, params_for, zone_image_format, DeviceParams, Family};

/// Build the mirajazz enumeration queries from `kind::SKUS`.
///
/// Queried at both observed vendor usage pages (0xFFA0 on the AKP05E demo unit
/// and in every opendeck consumer; 0xFF00 as a fallback) since firmware varies.
///
/// Derived from the SKU table rather than a second hand-maintained list: the
/// two drifted before (the AKP05 Pro/retail PIDs landed in the parameter table
/// but not the enumeration list, so those units were never opened at all).
fn build_queries() -> Vec<DeviceQuery> {
    let mut q = Vec::new();
    for (vid, pid) in known_vid_pids() {
        q.push(DeviceQuery::new(0xFFA0, 1, vid, pid));
        q.push(DeviceQuery::new(0xFF00, 1, vid, pid));
    }
    q
}

/// Command-line configuration.
struct Args {
    /// Allow brightness/image commands (mirajazz `initialize()` sends `CRT DIS`).
    allow_output: bool,
    /// Diagnostic mode: dump every HID interface the host can see and exit.
    list: bool,
    /// Open only this (vid, pid). The app passes its own device's identity so
    /// one sidecar instance never adopts a different Stream Dock's handle.
    only: Option<(u16, u16)>,
}

/// Parse `0x1234` / `1234` (hex) or a decimal value.
fn parse_id(raw: &str) -> Option<u16> {
    let raw = raw.trim();
    if let Some(hex) = raw.strip_prefix("0x").or_else(|| raw.strip_prefix("0X")) {
        return u16::from_str_radix(hex, 16).ok();
    }
    u16::from_str_radix(raw, 16)
        .ok()
        .or_else(|| raw.parse().ok())
}

fn parse_args() -> Args {
    let argv: Vec<String> = std::env::args().skip(1).collect();
    let mut args = Args {
        allow_output: false,
        list: false,
        only: None,
    };
    let (mut vid, mut pid) = (None, None);

    let mut i = 0;
    while i < argv.len() {
        let a = argv[i].as_str();
        // Accept both `--vid 0x0300` and `--vid=0x0300`.
        let (key, inline) = match a.split_once('=') {
            Some((k, v)) => (k, Some(v.to_string())),
            None => (a, None),
        };
        let mut take_value = || -> Option<String> {
            if let Some(v) = inline.clone() {
                return Some(v);
            }
            i += 1;
            argv.get(i).cloned()
        };
        match key {
            "--allow-output" => args.allow_output = true,
            "--list" => args.list = true,
            "--vid" => vid = take_value().as_deref().and_then(parse_id),
            "--pid" => pid = take_value().as_deref().and_then(parse_id),
            _ => {}
        }
        i += 1;
    }
    if let (Some(v), Some(p)) = (vid, pid) {
        args.only = Some((v, p));
    }
    args
}

/// Best-effort: report the OS device node behind `id` and whether this process
/// can open it read-write.
///
/// `async_hid::DeviceId` is not re-exported by mirajazz and taking a direct
/// dependency on async-hid pulls in a conflicting async runtime feature, so the
/// path is recovered from the id's `Debug` form (`DevPath("/dev/hidrawN")` on
/// Linux). This is diagnostic output only — nothing depends on it parsing.
fn node_access(debug_id: &str) -> (Option<String>, Option<bool>) {
    if !cfg!(target_os = "linux") {
        return (None, None);
    }
    let Some(open) = debug_id.find('"') else {
        return (None, None);
    };
    let rest = &debug_id[open + 1..];
    let Some(close) = rest.find('"') else {
        return (None, None);
    };
    let path = &rest[..close];
    if !path.starts_with('/') {
        return (None, None);
    }
    let writable = std::fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(path)
        .is_ok();
    (Some(path.to_string()), Some(writable))
}

/// `--list`: dump every HID interface visible to this process, flagging the
/// ones the sidecar recognises. This is the first thing to run when a device
/// "does nothing": it distinguishes "not enumerated at all" (cable/kernel),
/// "enumerated but unreadable" (missing udev `uaccess` rule) and "enumerated
/// with a VID:PID we do not know" (catalogue gap).
async fn list_all_hid() {
    let backend = new_hid_backend();
    let mut stream = match backend.enumerate().await {
        Ok(s) => s,
        Err(e) => {
            emit(serde_json::json!({"event": "error", "msg": format!("enumerate: {e}")}));
            return;
        }
    };
    let mut count = 0usize;
    while let Some(d) = stream.next().await {
        count += 1;
        let known = params_for(d.vendor_id, d.product_id);
        let (node, writable) = node_access(&format!("{:?}", d.id));
        emit(serde_json::json!({
            "event": "hid",
            "vid": format!("0x{:04x}", d.vendor_id),
            "pid": format!("0x{:04x}", d.product_id),
            "usage_page": format!("0x{:04x}", d.usage_page),
            "usage_id": d.usage_id,
            "name": d.name,
            "manufacturer": d.manufacturer,
            "serial": d.serial_number,
            "known": known.is_some(),
            "known_as": known.map(|p| p.human_name),
            "protocol_version": known.map(|p| p.protocol_version),
            "node": node,
            // Linux only: false here on a `known` row means the udev rule is
            // not applying its `uaccess` ACL, which is the single most common
            // reason a Stream Dock enumerates but "does nothing".
            "writable": writable,
        }));
    }
    emit(serde_json::json!({"event": "list_done", "interface_count": count}));
}

/// A connected device plus the parameters it was opened with. The full params
/// are retained (not just the family) because the key image format depends on
/// the SKU's protocol version, not only on its family.
struct DeviceEntry {
    device: Arc<Device>,
    params: DeviceParams,
}

type DeviceMap = Arc<Mutex<HashMap<String, DeviceEntry>>>;

/// Emit one JSON line to stdout. `println!` locks stdout, so tasks don't interleave.
fn emit(obj: serde_json::Value) {
    println!("{obj}");
}

fn noop_process(_input: u8, _state: u8) -> Result<DeviceInput, MirajazzError> {
    Ok(DeviceInput::NoData)
}

fn make_solid(w: u32, h: u32, r: u8, g: u8, b: u8) -> DynamicImage {
    let mut img = RgbaImage::new(w, h);
    for px in img.pixels_mut() {
        *px = image::Rgba([r, g, b, 255]);
    }
    DynamicImage::ImageRgba8(img)
}

#[tokio::main]
async fn main() {
    let args = parse_args();
    if args.list {
        list_all_hid().await;
        return;
    }
    let allow_output = args.allow_output;
    let devices: DeviceMap = Arc::new(Mutex::new(HashMap::new()));

    let queries = match args.only {
        // One sidecar per app-side device: query only that device's identity so
        // the handle this process holds is unambiguously the caller's.
        Some((vid, pid)) => {
            vec![
                DeviceQuery::new(0xFFA0, 1, vid, pid),
                DeviceQuery::new(0xFF00, 1, vid, pid),
            ]
        }
        None => build_queries(),
    };
    let matched = match list_devices(&queries).await {
        Ok(set) => set,
        Err(e) => {
            emit(serde_json::json!({"event": "error", "msg": format!("enumerate: {e}")}));
            return;
        }
    };

    // One control interface per physical unit (usage_id 1).
    for dev in matched.into_iter().filter(|d| d.usage_id == 1) {
        if let Some((vid, pid)) = args.only {
            if dev.vendor_id != vid || dev.product_id != pid {
                continue;
            }
        }
        let params = match params_for(dev.vendor_id, dev.product_id) {
            Some(p) => p,
            None => continue, // not a mirajazz-driven SKU
        };
        match Device::connect(
            &dev,
            params.protocol_version,
            params.key_count,
            params.encoder_count,
        )
        .await
        {
            Ok(device) => {
                let device = Arc::new(device);
                let serial = device.serial_number().clone();
                emit(serde_json::json!({
                    "event": "connected",
                    "serial": serial,
                    "vid": device.vid,
                    "pid": device.pid,
                    "firmware": device.firmware_version.clone(),
                    "family": format!("{:?}", params.family),
                    "name": params.human_name,
                }));

                spawn_input_reader(device.get_reader(noop_process), serial.clone());

                devices
                    .lock()
                    .await
                    .insert(serial, DeviceEntry { device, params });
            }
            Err(e) => emit(serde_json::json!({"event": "error", "msg": format!("connect: {e}")})),
        }
    }

    emit(serde_json::json!({
        "event": "ready",
        "device_count": devices.lock().await.len(),
        "output_allowed": allow_output,
    }));

    let mut lines = BufReader::new(tokio::io::stdin()).lines();
    while let Ok(Some(line)) = lines.next_line().await {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        let cmd: serde_json::Value = match serde_json::from_str(line) {
            Ok(v) => v,
            Err(e) => {
                emit(serde_json::json!({"event": "error", "msg": format!("bad json: {e}")}));
                continue;
            }
        };
        match cmd.get("cmd").and_then(|c| c.as_str()) {
            Some("ping") => emit(serde_json::json!({"event": "pong"})),
            Some("set_brightness") => handle_set_brightness(&devices, &cmd, allow_output).await,
            Some("set_image") => handle_set_image(&devices, &cmd, allow_output).await,
            Some("keep_alive") => handle_keep_alive(&devices, &cmd, allow_output).await,
            Some("render_test") => handle_render_test(&devices, &cmd, allow_output).await,
            other => emit(serde_json::json!({
                "event": "error",
                "msg": format!("unknown cmd: {other:?}"),
            })),
        }
    }
}

/// True if `buf` is an "ACK..OK" command-acknowledgement frame rather than an
/// input report. ACK frames begin with the ASCII bytes `A` `C` `K` (0x41 0x43
/// 0x4b) and share the input channel; they must not be decoded as input.
/// See docs/protocols/streamdeck/akp05_input_corrections.md §2.1.
fn is_ack_frame(buf: &[u8]) -> bool {
    buf.len() >= 3 && buf[0] == 0x41 && buf[1] == 0x43 && buf[2] == 0x4b
}

/// Per-device input reader. Uses raw frames (no initialize/DIS).
fn spawn_input_reader(reader: Arc<mirajazz::state::DeviceStateReader>, serial: String) {
    tokio::spawn(async move {
        loop {
            match reader
                .raw_read_data_with_timeout(512, Duration::from_millis(500))
                .await
            {
                Ok(Some(buf)) => {
                    // Discard "ACK..OK" acknowledgement frames: they ride the same
                    // input-report channel but are command acknowledgements, not
                    // input events. Emitting one as {code: buf[9]} would surface a
                    // bogus key/encoder event. See
                    // docs/protocols/streamdeck/akp05_input_corrections.md §2.1.
                    if is_ack_frame(&buf) {
                        continue;
                    }
                    let hex: String = buf.iter().take(16).map(|b| format!("{b:02x}")).collect();
                    emit(serde_json::json!({
                        "event": "input",
                        "serial": serial,
                        "code": buf.get(9).copied().unwrap_or(0),
                        "state": buf.get(10).copied().unwrap_or(0),
                        "raw": hex,
                    }));
                }
                Ok(None) => {}
                Err(e) => {
                    emit(serde_json::json!({
                        "event": "device_error", "serial": serial, "msg": format!("{e}"),
                    }));
                    break;
                }
            }
        }
    });
}

async fn handle_set_brightness(devices: &DeviceMap, cmd: &serde_json::Value, allow_output: bool) {
    if !allow_output {
        emit(serde_json::json!({"event": "error", "msg": "output disabled (--allow-output)"}));
        return;
    }
    let serial = cmd.get("serial").and_then(|s| s.as_str()).unwrap_or("");
    let percent = cmd.get("percent").and_then(|p| p.as_u64()).unwrap_or(50) as u8;
    let device = devices.lock().await.get(serial).map(|e| e.device.clone());
    match device {
        Some(device) => match device.set_brightness(percent).await {
            Ok(()) => emit(serde_json::json!({"event":"ok","cmd":"set_brightness"})),
            Err(e) => {
                emit(serde_json::json!({"event":"error","msg":format!("set_brightness: {e}")}))
            }
        },
        None => emit(serde_json::json!({"event":"error","msg":format!("no device {serial}")})),
    }
}

/// `{"cmd":"keep_alive","serial":..}` — sends mirajazz `keep_alive()` (CRT CONNECT) to hold the
/// persistent HID handle alive while idle, preventing the panel from wedging. Driven by the app's
/// StreamDockControlService keep-alive timer (whose IDisplayCapable::keepAlive() was a no-op before
/// this command existed).
async fn handle_keep_alive(devices: &DeviceMap, cmd: &serde_json::Value, allow_output: bool) {
    if !allow_output {
        emit(serde_json::json!({"event": "error", "msg": "output disabled (--allow-output)"}));
        return;
    }
    let serial = cmd.get("serial").and_then(|s| s.as_str()).unwrap_or("");
    let device = devices.lock().await.get(serial).map(|e| e.device.clone());
    match device {
        Some(device) => match device.keep_alive().await {
            Ok(()) => emit(serde_json::json!({"event":"ok","cmd":"keep_alive"})),
            Err(e) => emit(serde_json::json!({"event":"error","msg":format!("keep_alive: {e}")})),
        },
        None => emit(serde_json::json!({"event":"error","msg":format!("no device {serial}")})),
    }
}

/// `{"cmd":"set_image","serial":..,"key":N,"touchzone":bool,"width":W,"height":H,"rgba_b64":".."}`
async fn handle_set_image(devices: &DeviceMap, cmd: &serde_json::Value, allow_output: bool) {
    if !allow_output {
        emit(serde_json::json!({"event": "error", "msg": "output disabled (--allow-output)"}));
        return;
    }
    let serial = cmd.get("serial").and_then(|s| s.as_str()).unwrap_or("");
    let key = cmd.get("key").and_then(|k| k.as_u64()).unwrap_or(0) as u8;
    let touchzone = cmd
        .get("touchzone")
        .and_then(|t| t.as_bool())
        .unwrap_or(false);
    let width = cmd.get("width").and_then(|w| w.as_u64()).unwrap_or(0) as u32;
    let height = cmd.get("height").and_then(|h| h.as_u64()).unwrap_or(0) as u32;
    let rgba = match cmd
        .get("rgba_b64")
        .and_then(|s| s.as_str())
        .map(|s| B64.decode(s))
    {
        Some(Ok(bytes)) => bytes,
        _ => {
            emit(serde_json::json!({"event": "error", "msg": "missing/invalid rgba_b64"}));
            return;
        }
    };
    if rgba.len() as u32 != width * height * 4 {
        emit(serde_json::json!({"event": "error", "msg": "rgba length != width*height*4"}));
        return;
    }
    let img = match RgbaImage::from_raw(width, height, rgba) {
        Some(i) => DynamicImage::ImageRgba8(i),
        None => {
            emit(serde_json::json!({"event": "error", "msg": "RgbaImage::from_raw failed"}));
            return;
        }
    };

    let (device, fmt) = {
        let guard = devices.lock().await;
        match guard.get(serial) {
            Some(e) => {
                let fmt = if touchzone {
                    zone_image_format()
                } else {
                    key_image_format(&e.params)
                };
                (e.device.clone(), fmt)
            }
            None => {
                emit(serde_json::json!({"event":"error","msg":format!("no device {serial}")}));
                return;
            }
        }
    };

    let r = async {
        device.set_button_image(key, fmt, img).await?;
        device.flush().await
    }
    .await;
    match r {
        Ok(()) => emit(serde_json::json!({"event":"ok","cmd":"set_image","key":key})),
        Err(e) => emit(serde_json::json!({"event":"error","msg":format!("set_image: {e}")})),
    }
}

/// `{"cmd":"render_test","serial":..}` — one-shot visual test, family-aware.
async fn handle_render_test(devices: &DeviceMap, cmd: &serde_json::Value, allow_output: bool) {
    if !allow_output {
        emit(serde_json::json!({"event": "error", "msg": "output disabled (--allow-output)"}));
        return;
    }
    let serial = cmd.get("serial").and_then(|s| s.as_str()).unwrap_or("");
    let (device, params, key_count) = {
        let guard = devices.lock().await;
        match guard.get(serial) {
            Some(e) => (e.device.clone(), e.params, e.device.key_count()),
            None => {
                emit(serde_json::json!({"event":"error","msg":format!("no device {serial}")}));
                return;
            }
        }
    };

    let r = async {
        device.set_brightness(60).await?;
        for key in 0u8..key_count as u8 {
            let (r, g, b) = (
                key.wrapping_mul(17),
                key.wrapping_mul(9).wrapping_add(40),
                200u8.wrapping_sub(key.wrapping_mul(13)),
            );
            // AKP05 indices 0..3 are encoder touch zones; other families have none.
            let fmt = if params.family == Family::Akp05 && key < 4 {
                zone_image_format()
            } else {
                key_image_format(&params)
            };
            // Source the solid at the format's own size so no family renders a
            // mis-sized probe (the AKP03 uploads 60x60 / 64x64, not 112x112).
            let (w, h) = (fmt.size.0 as u32, fmt.size.1 as u32);
            device
                .set_button_image(key, fmt, make_solid(w, h, r, g, b))
                .await?;
        }
        device.flush().await
    }
    .await;
    match r {
        Ok(()) => emit(serde_json::json!({"event":"ok","cmd":"render_test","serial":serial})),
        Err(e) => emit(serde_json::json!({"event":"error","msg":format!("render_test: {e}")})),
    }
}

#[cfg(test)]
mod tests {
    use super::is_ack_frame;

    #[test]
    fn ack_frame_is_detected_and_input_is_not() {
        // "ACK" prefix (0x41 0x43 0x4b) -> acknowledgement, must be filtered.
        assert!(is_ack_frame(b"ACK,OK\0\0\0\0\0\0"));
        assert!(is_ack_frame(&[0x41, 0x43, 0x4b, 0x00]));
        // A real input report (code at byte 9) must NOT be treated as ACK.
        let mut input = [0u8; 16];
        input[9] = 0x05; // key index 5
        input[10] = 0x01; // pressed
        assert!(!is_ack_frame(&input));
        // Too-short / empty buffers are not ACK.
        assert!(!is_ack_frame(&[0x41, 0x43]));
        assert!(!is_ack_frame(&[]));
    }
}

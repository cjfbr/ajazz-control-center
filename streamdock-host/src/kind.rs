//! Per-device-family parameters for the Stream Dock families mirajazz can
//! drive: AKP05/N4, AKP03/N3, AKP153/HSV293S.
//!
//! The authority for the (vid, pid) -> {family, protocol version, image
//! format} mapping is the set of mirajazz consumers maintained against real
//! hardware — `4ndv/opendeck-akp03`, `4ndv/opendeck-akp153` and
//! `naerschhersch/opendeck-akp05`. `SKUS` below is a transcription of their
//! `mappings.rs` tables, restricted to the SKUs this project registers.
//!
//! PROVISIONAL: only AKP05E (`0x0300:0x3004`) has been verified against local
//! hardware. The AKP03 / AKP153 rows follow upstream, which is field-validated
//! by those plugins' users but not by us.

use mirajazz::types::{ImageFormat, ImageMirroring, ImageMode, ImageRotation};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Family {
    /// AKP05 / Mirabox N4 — 10 keys + 4 encoders + 4 touch-strip zones.
    Akp05,
    /// AKP03 / Mirabox N3 — 6 LCD keys + 3 plain buttons + 3 encoders.
    Akp03,
    /// AKP153 / Mirabox HSV293S — key grid, no encoders.
    Akp153,
}

/// Connection + geometry parameters for one device family member.
#[derive(Clone, Copy, Debug)]
pub struct DeviceParams {
    pub family: Family,
    /// mirajazz protocol version. Drives the HID packet size (512 for pv1,
    /// 1024 for pv2+) and whether the device reports both press AND release
    /// edges (pv3 only). Getting this wrong does not just degrade behaviour —
    /// a pv2/pv3 device addressed with pv1 framing writes 512-byte reports
    /// that the firmware ignores, so the panel stays dark and no input
    /// arrives. See mirajazz `Device::connect`.
    pub protocol_version: usize,
    pub key_count: usize,
    pub encoder_count: usize,
    pub human_name: &'static str,
}

/// One USB identity plus the parameters to open it with.
#[derive(Clone, Copy, Debug)]
pub struct Sku {
    pub vid: u16,
    pub pid: u16,
    pub params: DeviceParams,
}

const fn sku(
    vid: u16,
    pid: u16,
    family: Family,
    protocol_version: usize,
    key_count: usize,
    encoder_count: usize,
    human_name: &'static str,
) -> Sku {
    Sku {
        vid,
        pid,
        params: DeviceParams {
            family,
            protocol_version,
            key_count,
            encoder_count,
            human_name,
        },
    }
}

/// Every Stream Dock SKU the sidecar drives.
///
/// Mirrors the app's `register.cpp` Stream Dock matrix 1:1 — same SKUs, same
/// geometry — so the sidecar stays at parity with the descriptors the app
/// registers. `tests/unit/test_streamdeck_sidecar_geometry_contract.cpp`
/// enforces that in CI; edit the two tables in lockstep.
#[rustfmt::skip] // one line per SKU: the table is the documentation.
pub const SKUS: &[Sku] = &[
    // -----------------------------------------------------------------
    // AKP05 / N4 — pv3, 15 mirajazz surfaces (10 keys + 4 zones + 1 dead),
    // 4 encoders. `0x0300:0x3004` is hardware-confirmed here; the rest are
    // transcribed from opendeck-akp05's mappings.rs.
    // -----------------------------------------------------------------
    sku(0x0300, 0x3004, Family::Akp05, 3, 15, 4, "Ajazz AKP05E"),
    sku(0x0300, 0x3013, Family::Akp05, 3, 15, 4, "Ajazz AKP05E Pro"),
    sku(0x0300, 0x3014, Family::Akp05, 3, 15, 4, "Ajazz AKP05CN Pro"),
    sku(0x0300, 0x3006, Family::Akp05, 3, 15, 4, "Ajazz AKP05"),
    sku(0x0300, 0x5001, Family::Akp05, 3, 15, 4, "Ajazz AKP05 (provisional)"),
    sku(0x6603, 0x1007, Family::Akp05, 3, 15, 4, "Mirabox N4"),
    // -----------------------------------------------------------------
    // AKP03 / N3 — 9 mirajazz surfaces (6 LCD keys + 3 plain buttons),
    // 3 encoders. Protocol version is PER SKU, not per family: the
    // "rev. 2" silicon and the 0x6603 Mirabox units speak pv3 and upload
    // 64x64 Rot90, while the original units speak pv2 and upload 60x60
    // Rot0 (opendeck-akp03 `Kind::protocol_version` / `Kind::image_format`).
    // -----------------------------------------------------------------
    // pv2 — 60x60 Rot0.
    sku(0x0300, 0x1001, Family::Akp03, 2, 9, 3, "Ajazz AKP03"),
    sku(0x0300, 0x1002, Family::Akp03, 2, 9, 3, "Ajazz AKP03E"),
    sku(0x0300, 0x1003, Family::Akp03, 2, 9, 3, "Ajazz AKP03R"),
    sku(0x6602, 0x1000, Family::Akp03, 2, 9, 3, "Mirabox N3 (6602:1000)"),
    sku(0x6602, 0x1002, Family::Akp03, 2, 9, 3, "Mirabox N3 (6602:1002)"),
    // In-tree only (no upstream row): kept from the pre-2026-08 catalogue so
    // deployments already bound to them keep working. Assumed pv2.
    sku(0x0300, 0x3001, Family::Akp03, 2, 9, 3, "Ajazz AKP03 (legacy firmware)"),
    sku(0x6602, 0x1003, Family::Akp03, 2, 9, 3, "Mirabox N3E (6602:1003)"),
    // pv3 — 64x64 Rot90.
    sku(0x0300, 0x3002, Family::Akp03, 3, 9, 3, "Ajazz AKP03E (rev. 2)"),
    sku(0x0300, 0x3003, Family::Akp03, 3, 9, 3, "Ajazz AKP03R (rev. 2)"),
    sku(0x6603, 0x1002, Family::Akp03, 3, 9, 3, "Mirabox N3 (6603:1002)"),
    sku(0x6603, 0x1003, Family::Akp03, 3, 9, 3, "Mirabox N3EN"),
    sku(0x1500, 0x3001, Family::Akp03, 3, 9, 3, "Soomfon Stream Controller SE"),
    sku(0x0b00, 0x1001, Family::Akp03, 3, 9, 3, "Mars Gaming MSD-TWO"),
    sku(0x5548, 0x1001, Family::Akp03, 3, 9, 3, "TreasLin N3"),
    sku(0x0200, 0x2000, Family::Akp03, 3, 9, 3, "Redragon Skyrider SS-551"),
    // -----------------------------------------------------------------
    // AKP153 / HSV293S — pv1, 15 surfaces, no encoders.
    //
    // NOTE `0x0300:0x1001` and `0x0300:0x1002` are NOT AKP153 SKUs: both
    // opendeck-akp153 and opendeck-akp03 place them in the AKP03 family
    // (above), as does this repo's own RE (docs/protocols/streamdeck/
    // akp03.md "USB identifier map"). They were mis-filed here until
    // 2026-08-21, which opened every AKP03/AKP03E on those PIDs with pv1
    // 512-byte framing — the device enumerated but never responded.
    // -----------------------------------------------------------------
    sku(0x5548, 0x6674, Family::Akp153, 1, 15, 0, "Ajazz AKP153 (Mirabox V1)"),
    sku(0x5548, 0x6670, Family::Akp153, 1, 15, 0, "Mirabox HSV293S"),
    sku(0x0300, 0x1010, Family::Akp153, 1, 15, 0, "Ajazz AKP153E"),
    sku(0x0300, 0x1020, Family::Akp153, 1, 15, 0, "Ajazz AKP153R"),
];

/// Resolve (vid, pid) to its parameters, or None if unknown.
pub fn params_for(vid: u16, pid: u16) -> Option<DeviceParams> {
    SKUS.iter()
        .find(|s| s.vid == vid && s.pid == pid)
        .map(|s| s.params)
}

/// Every (vid, pid) the sidecar should enumerate, derived from `SKUS` so the
/// enumeration list can never drift from the parameter table. (It did: the
/// AKP05 Pro/retail PIDs were added to the parameter table but not to the
/// hand-maintained enumeration list, so those units were never opened.)
pub fn known_vid_pids() -> impl Iterator<Item = (u16, u16)> {
    SKUS.iter().map(|s| (s.vid, s.pid))
}

/// Image format for a regular key.
///
/// Per SKU, not per family — see `DeviceParams::protocol_version`.
pub fn key_image_format(params: &DeviceParams) -> ImageFormat {
    match params.family {
        Family::Akp05 => ImageFormat {
            mode: ImageMode::JPEG,
            size: (112, 112),
            rotation: ImageRotation::Rot180,
            mirror: ImageMirroring::None,
        },
        // opendeck-akp03 `Kind::image_format`: pv3 silicon uploads 64x64
        // Rot90, everything else 60x60 Rot0.
        Family::Akp03 if params.protocol_version >= 3 => ImageFormat {
            mode: ImageMode::JPEG,
            size: (64, 64),
            rotation: ImageRotation::Rot90,
            mirror: ImageMirroring::None,
        },
        Family::Akp03 => ImageFormat {
            mode: ImageMode::JPEG,
            size: (60, 60),
            rotation: ImageRotation::Rot0,
            mirror: ImageMirroring::None,
        },
        // Every AKP153 SKU the app registers is pv1 (85x85 Rot90, mirror both).
        Family::Akp153 => ImageFormat {
            mode: ImageMode::JPEG,
            size: (85, 85),
            rotation: ImageRotation::Rot90,
            mirror: ImageMirroring::Both,
        },
    }
}

/// Image format for an AKP05 encoder touch zone (other families have no zones).
pub fn zone_image_format() -> ImageFormat {
    ImageFormat {
        mode: ImageMode::JPEG,
        size: (128, 128),
        rotation: ImageRotation::Rot180,
        mirror: ImageMirroring::None,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashSet;

    #[test]
    fn akp05e_is_pv3_with_4_encoders() {
        let p = params_for(0x0300, 0x3004).expect("AKP05E known");
        assert_eq!(p.family, Family::Akp05);
        assert_eq!(p.protocol_version, 3);
        assert_eq!(p.encoder_count, 4);
    }

    #[test]
    fn n4_shares_akp05_family() {
        assert_eq!(params_for(0x6603, 0x1007).unwrap().family, Family::Akp05);
        // Pro/retail variants ride the same family params (issue #85).
        assert_eq!(params_for(0x0300, 0x3013).unwrap().family, Family::Akp05);
        assert_eq!(params_for(0x0300, 0x3014).unwrap().family, Family::Akp05);
        assert_eq!(params_for(0x0300, 0x3006).unwrap().family, Family::Akp05);
    }

    #[test]
    fn akp03_grid_has_9_surfaces_and_3_encoders() {
        for &(vid, pid) in &[(0x0300u16, 0x1002u16), (0x0300, 0x3002), (0x6603, 0x1003)] {
            let p = params_for(vid, pid).expect("AKP03 SKU known");
            assert_eq!(p.family, Family::Akp03);
            assert_eq!(p.key_count, 9);
            assert_eq!(p.encoder_count, 3);
        }
    }

    #[test]
    fn akp03e_pids_are_akp03_family_not_akp153() {
        // Regression: 0x0300:0x1001/0x1002 used to resolve to AKP153 (pv1),
        // which opens an AKP03/AKP03E with 512-byte framing -> dead device.
        assert_eq!(params_for(0x0300, 0x1001).unwrap().family, Family::Akp03);
        assert_eq!(params_for(0x0300, 0x1002).unwrap().family, Family::Akp03);
    }

    #[test]
    fn akp03_protocol_version_is_per_sku() {
        // Original silicon: pv2.
        assert_eq!(params_for(0x0300, 0x1002).unwrap().protocol_version, 2);
        assert_eq!(params_for(0x0300, 0x1001).unwrap().protocol_version, 2);
        // rev. 2 silicon and the 0x6603 Mirabox units: pv3.
        assert_eq!(params_for(0x0300, 0x3002).unwrap().protocol_version, 3);
        assert_eq!(params_for(0x0300, 0x3003).unwrap().protocol_version, 3);
        assert_eq!(params_for(0x6603, 0x1002).unwrap().protocol_version, 3);
        assert_eq!(params_for(0x6603, 0x1003).unwrap().protocol_version, 3);
    }

    #[test]
    fn akp153_is_pv1_no_encoders_15_keys() {
        let p = params_for(0x5548, 0x6674).expect("AKP153 known");
        assert_eq!(p.family, Family::Akp153);
        assert_eq!(p.protocol_version, 1);
        assert_eq!(p.encoder_count, 0);
        assert_eq!(p.key_count, 15); // parity with our descriptor, not opendeck's 18
    }

    #[test]
    fn unknown_vid_pid_is_none() {
        assert!(params_for(0xDEAD, 0xBEEF).is_none());
    }

    #[test]
    fn akp05_key_format_is_112_rot180() {
        let f = key_image_format(&params_for(0x0300, 0x3004).unwrap());
        assert_eq!(f.size, (112, 112));
        assert!(matches!(f.rotation, ImageRotation::Rot180));
    }

    #[test]
    fn akp153_is_85_rot90_mirror_both() {
        let f = key_image_format(&params_for(0x5548, 0x6674).unwrap());
        assert_eq!(f.size, (85, 85));
        assert!(matches!(f.rotation, ImageRotation::Rot90));
        assert!(matches!(f.mirror, ImageMirroring::Both));
    }

    #[test]
    fn akp03_key_format_follows_protocol_version() {
        // pv2 AKP03E -> 60x60 Rot0.
        let pv2 = key_image_format(&params_for(0x0300, 0x1002).unwrap());
        assert_eq!(pv2.size, (60, 60));
        assert!(matches!(pv2.rotation, ImageRotation::Rot0));
        // pv3 AKP03E rev. 2 -> 64x64 Rot90.
        let pv3 = key_image_format(&params_for(0x0300, 0x3002).unwrap());
        assert_eq!(pv3.size, (64, 64));
        assert!(matches!(pv3.rotation, ImageRotation::Rot90));
    }

    #[test]
    fn known_vid_pids_covers_every_sku_exactly_once() {
        let listed: Vec<(u16, u16)> = known_vid_pids().collect();
        assert_eq!(listed.len(), SKUS.len());
        let unique: HashSet<(u16, u16)> = listed.iter().copied().collect();
        assert_eq!(unique.len(), SKUS.len(), "duplicate (vid,pid) in SKUS");
        for &(vid, pid) in &listed {
            assert!(params_for(vid, pid).is_some());
        }
    }
}

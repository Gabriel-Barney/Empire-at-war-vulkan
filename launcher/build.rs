//! Build script: embeds `assets/icon.ico` in the exe as its program icon (what Explorer,
//! shortcuts and the taskbar show before the window opens). The `.res` file is written here
//! in plain Rust and handed to the MSVC linker, so no resource compiler (rc.exe) is needed.
//!
//! (Until 2026-11-13 this also copied Valve's `steam_api64.dll` next to the exe; the
//! launcher no longer uses the Steamworks SDK, see `src/workshop.rs`.)

use std::path::{Path, PathBuf};

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    let target = std::env::var("TARGET").unwrap_or_default();
    if !target.contains("windows") || !target.contains("msvc") {
        return;
    }
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap_or_default());
    embed_icon(&out_dir);
}

const ICON: &str = "assets/icon.ico";

/// Turns `assets/icon.ico` into a Win32 `.res` file (one RT_ICON per image plus an
/// RT_GROUP_ICON directory) and passes it to the linker.
fn embed_icon(out_dir: &Path) {
    println!("cargo:rerun-if-changed={ICON}");
    let manifest = std::env::var("CARGO_MANIFEST_DIR").unwrap_or_default();
    let ico = match std::fs::read(Path::new(&manifest).join(ICON)) {
        Ok(b) => b,
        Err(e) => {
            println!("cargo:warning=Could not read {ICON} ({e}); the exe will have no icon.");
            return;
        }
    };
    let Some(res) = ico_to_res(&ico) else {
        println!("cargo:warning={ICON} is not a valid .ico file; the exe will have no icon.");
        return;
    };
    let path = out_dir.join("icon.res");
    if let Err(e) = std::fs::write(&path, res) {
        println!("cargo:warning=Could not write {}: {e}", path.display());
        return;
    }
    println!("cargo:rustc-link-arg-bins={}", path.display());
}

fn ico_to_res(ico: &[u8]) -> Option<Vec<u8>> {
    const RT_ICON: u16 = 3;
    const RT_GROUP_ICON: u16 = 14;
    const LANG_EN_US: u16 = 0x0409;
    let u16_at = |o: usize| Some(u16::from_le_bytes(ico.get(o..o + 2)?.try_into().ok()?));
    let u32_at = |o: usize| Some(u32::from_le_bytes(ico.get(o..o + 4)?.try_into().ok()?));

    if u16_at(0)? != 0 || u16_at(2)? != 1 {
        return None;
    }
    let count = u16_at(4)? as usize;
    if count == 0 {
        return None;
    }

    // An empty entry first marks the file as a 32-bit .res.
    let mut res = Vec::new();
    push_resource(&mut res, 0, 0, 0, 0, &[]);

    let mut group = Vec::new();
    group.extend_from_slice(&0u16.to_le_bytes());
    group.extend_from_slice(&1u16.to_le_bytes());
    group.extend_from_slice(&(count as u16).to_le_bytes());
    for i in 0..count {
        let e = 6 + i * 16;
        let len = u32_at(e + 8)? as usize;
        let off = u32_at(e + 12)? as usize;
        let image = ico.get(off..off.checked_add(len)?)?;
        let id = (i + 1) as u16;
        push_resource(&mut res, RT_ICON, id, 0x1010, LANG_EN_US, image);
        group.extend_from_slice(ico.get(e..e + 12)?); // width .. bytes-in-resource
        group.extend_from_slice(&id.to_le_bytes());
    }
    push_resource(&mut res, RT_GROUP_ICON, 1, 0x1030, LANG_EN_US, &group);
    Some(res)
}

/// Appends one resource with an ordinal type and name (RESOURCEHEADER + data, DWORD aligned).
fn push_resource(res: &mut Vec<u8>, kind: u16, name: u16, flags: u16, lang: u16, data: &[u8]) {
    res.extend_from_slice(&(data.len() as u32).to_le_bytes());
    res.extend_from_slice(&32u32.to_le_bytes()); // header size
    for v in [0xFFFF, kind, 0xFFFF, name] {
        res.extend_from_slice(&u16::to_le_bytes(v));
    }
    res.extend_from_slice(&0u32.to_le_bytes()); // data version
    res.extend_from_slice(&flags.to_le_bytes());
    res.extend_from_slice(&lang.to_le_bytes());
    res.extend_from_slice(&0u32.to_le_bytes()); // version
    res.extend_from_slice(&0u32.to_le_bytes()); // characteristics
    res.extend_from_slice(data);
    while res.len() % 4 != 0 {
        res.push(0);
    }
}

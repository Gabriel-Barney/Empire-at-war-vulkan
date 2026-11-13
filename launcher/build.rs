//! Build script: program icon and Steam Workshop support.
//!
//! 0. Embeds `assets/icon.ico` in the exe as its program icon (what Explorer, shortcuts and
//!    the taskbar show before the window opens). The `.res` file is written here in plain
//!    Rust and handed to the MSVC linker, so no resource compiler (rc.exe) is needed.
//! 1. Copies `steam_api64.dll` (shipped inside the `steamworks-sys` crate) next to the built
//!    exe, i.e. into `target\<profile>\`. A release package must ship that DLL beside
//!    `command-center.exe`.
//! 2. Makes the DLL a *delay-loaded* import, so the launcher still starts when the DLL is
//!    missing. Only the Steam helper process (the same exe, see `workshop.rs`) loads it, by
//!    full path, before its first Steam call, and reports a plain error when it isn't there.

use std::path::{Path, PathBuf};

const DLL: &str = "steam_api64.dll";

fn main() {
    println!("cargo:rerun-if-changed=build.rs");
    println!("cargo:rerun-if-changed=Cargo.lock");

    let target = std::env::var("TARGET").unwrap_or_default();
    if !target.contains("windows") || target.contains("i686") {
        return;
    }

    // OUT_DIR is target\<profile>\build\command-center-<hash>\out on older Cargo and
    // target\<profile>\build\command-center\<hash>\out on newer Cargo.
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap_or_default());

    if target.contains("msvc") {
        // Applies to the launcher exe and to its test binary.
        println!("cargo:rustc-link-arg-bins=/DELAYLOAD:{DLL}");
        println!("cargo:rustc-link-arg-bins=delayimp.lib");
        // The test binary makes no Steam calls, so the linker would warn (LNK4199) that the
        // delay-load has nothing to delay.
        println!("cargo:rustc-link-arg-bins=/IGNORE:4199");
        embed_icon(&out_dir);
    }
    // The profile folder (where the exe goes) is the parent of the nearest `build` folder,
    // which holds for both layouts.
    let profile_dir = out_dir
        .ancestors()
        .find(|p| p.file_name().is_some_and(|n| n == "build"))
        .and_then(Path::parent)
        .map(Path::to_path_buf);
    let Some(profile_dir) = profile_dir else {
        println!("cargo:warning=Could not work out the target folder; {DLL} was not copied.");
        return;
    };

    let Some(source) = find_dll(&profile_dir) else {
        println!(
            "cargo:warning={DLL} was not found in the steamworks-sys crate. Workshop features need it next to the exe."
        );
        return;
    };
    println!("cargo:rerun-if-changed={}", source.display());

    let dest = profile_dir.join(DLL);
    // A missing destination makes cargo run this script again on the next build.
    println!("cargo:rerun-if-changed={}", dest.display());
    if same_file(&source, &dest) {
        return;
    }
    if let Err(e) = std::fs::copy(&source, &dest) {
        // The Steam helper may be running and holding the DLL: only a problem if it's absent.
        if !dest.is_file() {
            println!("cargo:warning=Could not copy {DLL} to {}: {e}", dest.display());
        }
    }
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

fn same_file(a: &Path, b: &Path) -> bool {
    match (std::fs::metadata(a), std::fs::metadata(b)) {
        (Ok(x), Ok(y)) => x.len() == y.len() && std::fs::read(a).ok() == std::fs::read(b).ok(),
        _ => false,
    }
}

/// The version of `steamworks-sys` the lock file resolved to.
fn locked_version() -> Option<String> {
    let manifest = std::env::var("CARGO_MANIFEST_DIR").ok()?;
    let lock = std::fs::read_to_string(Path::new(&manifest).join("Cargo.lock")).ok()?;
    let mut lines = lock.lines();
    while let Some(line) = lines.next() {
        if line.trim() == "name = \"steamworks-sys\"" {
            let version = lines.next()?.trim();
            return Some(version.strip_prefix("version = \"")?.strip_suffix('"')?.to_owned());
        }
    }
    None
}

fn find_dll(profile_dir: &Path) -> Option<PathBuf> {
    // 1. The crate's own redistributable folder in the cargo registry.
    let cargo_home = std::env::var_os("CARGO_HOME")
        .map(PathBuf::from)
        .or_else(|| std::env::var_os("USERPROFILE").map(|h| PathBuf::from(h).join(".cargo")));
    if let (Some(home), Some(version)) = (cargo_home, locked_version()) {
        if let Ok(indexes) = std::fs::read_dir(home.join("registry").join("src")) {
            for index in indexes.flatten() {
                let dll = index
                    .path()
                    .join(format!("steamworks-sys-{version}"))
                    .join("lib")
                    .join("steam")
                    .join("redistributable_bin")
                    .join("win64")
                    .join(DLL);
                if dll.is_file() {
                    return Some(dll);
                }
            }
        }
    }
    // 2. The copy steamworks-sys's own build script leaves in its OUT_DIR (present once that
    //    crate has been built for this profile): build\steamworks-sys-<hash>\out on older
    //    Cargo, build\steamworks-sys\<hash>\out on newer Cargo.
    let builds = std::fs::read_dir(profile_dir.join("build")).ok()?;
    builds.flatten().find_map(|e| {
        let name = e.file_name().to_string_lossy().into_owned();
        if name.starts_with("steamworks-sys-") {
            Some(e.path().join("out").join(DLL)).filter(|p| p.is_file())
        } else if name == "steamworks-sys" {
            std::fs::read_dir(e.path())
                .ok()?
                .flatten()
                .map(|h| h.path().join("out").join(DLL))
                .find(|p| p.is_file())
        } else {
            None
        }
    })
}

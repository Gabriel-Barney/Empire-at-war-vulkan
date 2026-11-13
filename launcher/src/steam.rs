//! Finds Steam, its library folders, Forces of Corruption and the Workshop folder.

use std::path::{Path, PathBuf};

pub const EAW_APP_ID: &str = "32470";

#[derive(Clone, Debug, Default)]
pub struct SteamInfo {
    pub steam_dir: Option<PathBuf>,
    pub libraries: Vec<PathBuf>,
    /// `...\steamapps\common\Star Wars Empire at War`
    pub game_root: Option<PathBuf>,
    /// `...\Star Wars Empire at War\corruption`
    pub foc_dir: Option<PathBuf>,
    /// `...\steamapps\workshop\content\32470`
    pub workshop_dir: Option<PathBuf>,
}

pub fn detect() -> SteamInfo {
    let mut info = SteamInfo { steam_dir: find_steam_dir(), ..Default::default() };
    if let Some(steam) = &info.steam_dir {
        info.libraries = library_folders(steam);
    }
    for lib in &info.libraries {
        let apps = lib.join("steamapps");
        if info.game_root.is_none() {
            let root = match install_dir_from_manifest(&apps) {
                Some(name) => apps.join("common").join(name),
                None => apps.join("common").join("Star Wars Empire at War"),
            };
            if root.is_dir() {
                let foc = root.join("corruption");
                if foc.is_dir() {
                    info.foc_dir = Some(foc);
                }
                info.game_root = Some(root);
            }
        }
        if info.workshop_dir.is_none() {
            let ws = apps.join("workshop").join("content").join(EAW_APP_ID);
            if ws.is_dir() {
                info.workshop_dir = Some(ws);
            }
        }
    }
    info
}

fn find_steam_dir() -> Option<PathBuf> {
    #[cfg(windows)]
    {
        use winreg::enums::{HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE};
        use winreg::RegKey;
        let candidates = [
            (HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath"),
            (HKEY_LOCAL_MACHINE, "SOFTWARE\\WOW6432Node\\Valve\\Steam", "InstallPath"),
            (HKEY_LOCAL_MACHINE, "SOFTWARE\\Valve\\Steam", "InstallPath"),
        ];
        for (hive, key, value) in candidates {
            if let Ok(k) = RegKey::predef(hive).open_subkey(key) {
                if let Ok(p) = k.get_value::<String, _>(value) {
                    let p = PathBuf::from(p.replace('/', "\\"));
                    if p.is_dir() {
                        return Some(p);
                    }
                }
            }
        }
    }
    let fallback = PathBuf::from("C:\\Program Files (x86)\\Steam");
    fallback.is_dir().then_some(fallback)
}

/// Reads `steamapps/libraryfolders.vdf`. The Steam folder itself is always a library.
fn library_folders(steam: &Path) -> Vec<PathBuf> {
    let mut out = vec![steam.to_path_buf()];
    let vdf = steam.join("steamapps").join("libraryfolders.vdf");
    if let Ok(text) = std::fs::read_to_string(vdf) {
        for line in text.lines() {
            let toks = vdf_tokens(line);
            if toks.len() == 2 && toks[0].eq_ignore_ascii_case("path") {
                let p = PathBuf::from(&toks[1]);
                if p.is_dir() && !out.iter().any(|o| same_path(o, &p)) {
                    out.push(p);
                }
            }
        }
    }
    out
}

fn install_dir_from_manifest(steamapps: &Path) -> Option<String> {
    let text = std::fs::read_to_string(steamapps.join(format!("appmanifest_{EAW_APP_ID}.acf"))).ok()?;
    text.lines().find_map(|l| {
        let t = vdf_tokens(l);
        (t.len() == 2 && t[0].eq_ignore_ascii_case("installdir")).then(|| t[1].clone())
    })
}

/// Quoted tokens on one VDF line, with `\\` unescaped.
fn vdf_tokens(line: &str) -> Vec<String> {
    let mut out = Vec::new();
    let mut chars = line.chars();
    while let Some(c) = chars.next() {
        if c != '"' {
            continue;
        }
        let mut tok = String::new();
        while let Some(c) = chars.next() {
            match c {
                '\\' => {
                    if let Some(n) = chars.next() {
                        tok.push(n);
                    }
                }
                '"' => break,
                _ => tok.push(c),
            }
        }
        out.push(tok);
    }
    out
}

pub fn same_path(a: &Path, b: &Path) -> bool {
    let norm = |p: &Path| {
        p.to_string_lossy().replace('/', "\\").trim_end_matches('\\').to_ascii_lowercase()
    };
    norm(a) == norm(b)
}

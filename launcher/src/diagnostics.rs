//! Pre-flight diagnostics: everything worth checking before the game is started.
//!
//! [`quick`] is the cheap part (a few path checks) and is what `launch::problems` is made
//! of, so the Launch button and the diagnostics panel can't disagree. [`run`] adds the
//! checks that read files or ask the graphics driver; call it on a worker thread.
//! Everything works from an [`Input`] of plain values, so it can be tested without an app.

use crate::launch;
use std::collections::HashMap;
use std::path::{Path, PathBuf};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Severity {
    /// The game won't start, or won't get far.
    Error,
    /// Worth a look; the game still starts.
    Warning,
    Ok,
}

/// What the button next to a finding does.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Fix {
    OpenSettings,
    OpenProfiles,
    OpenMods,
    OpenFolder(PathBuf),
}

impl Fix {
    pub fn label(&self) -> &'static str {
        match self {
            Fix::OpenSettings => "Open settings",
            Fix::OpenProfiles => "Open profiles",
            Fix::OpenMods => "Open mods",
            Fix::OpenFolder(_) => "Open folder",
        }
    }
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Finding {
    pub severity: Severity,
    pub title: String,
    pub detail: String,
    pub fix: Option<Fix>,
}

impl Finding {
    fn new(severity: Severity, title: impl Into<String>, detail: impl Into<String>, fix: Option<Fix>) -> Self {
        Self { severity, title: title.into(), detail: detail.into(), fix }
    }
    fn ok(title: impl Into<String>, detail: impl Into<String>) -> Self {
        Self::new(Severity::Ok, title, detail, None)
    }
    fn warn(title: impl Into<String>, detail: impl Into<String>, fix: Option<Fix>) -> Self {
        Self::new(Severity::Warning, title, detail, fix)
    }
    fn error(title: impl Into<String>, detail: impl Into<String>, fix: Option<Fix>) -> Self {
        Self::new(Severity::Error, title, detail, fix)
    }
}

/// "7 checks passed, 1 warning".
pub fn summary(findings: &[Finding]) -> String {
    let count = |s: Severity| findings.iter().filter(|f| f.severity == s).count();
    let (ok, warnings, errors) = (count(Severity::Ok), count(Severity::Warning), count(Severity::Error));
    let mut parts = vec![format!("{ok} check{} passed", if ok == 1 { "" } else { "s" })];
    if errors > 0 {
        parts.push(format!("{errors} error{}", if errors == 1 { "" } else { "s" }));
    }
    if warnings > 0 {
        parts.push(format!("{warnings} warning{}", if warnings == 1 { "" } else { "s" }));
    }
    parts.join(", ")
}

/// The findings as plain text, for pasting into a bug report.
pub fn as_text(findings: &[Finding]) -> String {
    let mut out = format!("Command Center diagnostics: {}\r\n", summary(findings));
    for (severity, label) in [(Severity::Error, "ERROR"), (Severity::Warning, "WARNING"), (Severity::Ok, "OK")] {
        for f in findings.iter().filter(|f| f.severity == severity) {
            out.push_str(&format!("[{label}] {}: {}\r\n", f.title, f.detail));
        }
    }
    out
}

// ------------------------------------------------------------------ the cheap checks

/// Path checks cheap enough to run every frame. The `Error` findings here are exactly
/// what stops the Launch button (see `launch::problems`).
pub fn quick(game_exe: Option<&Path>, foc: Option<&Path>, mods: &[PathBuf]) -> Vec<Finding> {
    let mut out = Vec::new();
    match game_exe {
        None => out.push(Finding::error(
            "The game hasn't been chosen",
            "alamo_game.exe hasn't been chosen yet (Settings).",
            Some(Fix::OpenSettings),
        )),
        Some(p) if !p.is_file() => out.push(Finding::error(
            "The game wasn't found",
            format!("alamo_game.exe wasn't found at {}.", p.display()),
            Some(Fix::OpenSettings),
        )),
        Some(p) => out.push(Finding::ok("The game is in place", p.display().to_string())),
    }
    match foc {
        None => out.push(Finding::error(
            "Forces of Corruption wasn't found",
            "Forces of Corruption wasn't found (Settings).",
            Some(Fix::OpenSettings),
        )),
        Some(p) if !p.join("Data").is_dir() => out.push(Finding::error(
            "Forces of Corruption has no Data folder",
            format!("The Forces of Corruption folder has no Data folder: {}", p.display()),
            Some(Fix::OpenSettings),
        )),
        Some(p) => out.push(Finding::ok("Forces of Corruption is in place", p.display().to_string())),
    }
    for m in mods {
        if !m.is_dir() {
            out.push(Finding::error(
                "A mod folder is missing",
                format!("Mod folder is missing: {}", m.display()),
                Some(Fix::OpenProfiles),
            ));
        }
    }
    out
}

// ------------------------------------------------------------------ input

/// What a live Steam connection says about a Workshop item.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct SteamItem {
    pub installed: bool,
    pub needs_update: bool,
    pub downloading: bool,
    /// Download progress in percent, when Steam knows it.
    pub percent: Option<u32>,
}

#[derive(Clone, Debug, Default)]
pub struct ModEntry {
    pub name: String,
    pub path: PathBuf,
    /// Set for mods Steam manages: the ones inside the Workshop content folder.
    pub workshop_id: Option<u64>,
    /// Only while the launcher is connected to Steam and Steam has reported the item.
    pub steam: Option<SteamItem>,
}

#[derive(Clone, Debug, Default)]
pub struct Input {
    pub game_exe: Option<PathBuf>,
    pub foc: Option<PathBuf>,
    /// The profile's mods in load order.
    pub mods: Vec<ModEntry>,
    pub steam_connected: bool,
    /// `...\steamapps\workshop\appworkshop_32470.acf`, Steam's own record of Workshop items.
    pub workshop_acf: Option<PathBuf>,
    pub saves_dir: Option<PathBuf>,
    /// The graphics preset the profile names, and the file it would be read from.
    pub video_preset: Option<(String, PathBuf)>,
    pub extra_args: String,
    /// The folder the launcher's own exe is in.
    pub launcher_dir: Option<PathBuf>,
    /// `C:\Windows\System32`.
    pub system_dir: Option<PathBuf>,
    /// Load-order warnings from the conflict check; `None` while it hasn't finished.
    pub order_warnings: Option<Vec<String>>,
}

/// `%WINDIR%\System32`.
pub fn system_dir() -> Option<PathBuf> {
    std::env::var_os("WINDIR")
        .or_else(|| std::env::var_os("SystemRoot"))
        .map(|w| PathBuf::from(w).join("System32"))
        .filter(|p| p.is_dir())
}

/// Steam's record of the game's Workshop items, next to the Workshop content folder
/// (`...\steamapps\workshop\content\32470`).
pub fn workshop_acf_path(workshop_dir: &Path) -> Option<PathBuf> {
    let workshop = workshop_dir.parent()?.parent()?;
    let app = workshop_dir.file_name()?.to_string_lossy().into_owned();
    Some(workshop.join(format!("appworkshop_{app}.acf")))
}

// ------------------------------------------------------------------ Steam's Workshop record

/// A parsed Valve KeyValues file.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Vdf {
    Str(String),
    Map(Vec<(String, Vdf)>),
}

impl Vdf {
    pub fn get(&self, key: &str) -> Option<&Vdf> {
        match self {
            Vdf::Map(items) => items.iter().find(|(k, _)| k.eq_ignore_ascii_case(key)).map(|(_, v)| v),
            Vdf::Str(_) => None,
        }
    }
    pub fn str(&self, key: &str) -> Option<&str> {
        match self.get(key) {
            Some(Vdf::Str(s)) => Some(s),
            _ => None,
        }
    }
}

/// Parses KeyValues text (`"key" "value"` and `"key" { ... }`). `None` when it isn't that.
pub fn parse_vdf(text: &str) -> Option<Vdf> {
    #[derive(PartialEq)]
    enum Tok {
        Str(String),
        Open,
        Close,
    }
    let mut toks = Vec::new();
    let mut chars = text.chars().peekable();
    while let Some(c) = chars.next() {
        match c {
            '{' => toks.push(Tok::Open),
            '}' => toks.push(Tok::Close),
            '"' => {
                let mut s = String::new();
                loop {
                    match chars.next()? {
                        '\\' => s.push(match chars.next()? {
                            'n' => '\n',
                            't' => '\t',
                            other => other,
                        }),
                        '"' => break,
                        other => s.push(other),
                    }
                }
                toks.push(Tok::Str(s));
            }
            '/' if chars.peek() == Some(&'/') => {
                for c in chars.by_ref() {
                    if c == '\n' {
                        break;
                    }
                }
            }
            c if c.is_whitespace() => {}
            _ => return None,
        }
    }
    fn map(toks: &[Tok], pos: &mut usize, depth: usize, top: bool) -> Option<Vdf> {
        let mut items = Vec::new();
        loop {
            match toks.get(*pos) {
                None if top => return Some(Vdf::Map(items)),
                None => return None,
                Some(Tok::Close) if !top => {
                    *pos += 1;
                    return Some(Vdf::Map(items));
                }
                Some(Tok::Str(key)) => {
                    *pos += 1;
                    match toks.get(*pos)? {
                        Tok::Str(value) => {
                            *pos += 1;
                            items.push((key.clone(), Vdf::Str(value.clone())));
                        }
                        Tok::Open if depth < 32 => {
                            *pos += 1;
                            items.push((key.clone(), map(toks, pos, depth + 1, false)?));
                        }
                        _ => return None,
                    }
                }
                Some(_) => return None,
            }
        }
    }
    let mut pos = 0;
    map(&toks, &mut pos, 0, true)
}

/// What `appworkshop_<app>.acf` says about one item.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct AcfItem {
    /// Listed under `WorkshopItemsInstalled`.
    pub installed: bool,
    /// The manifest (version) of the files on disk.
    pub manifest: Option<String>,
    /// The newest manifest Steam knows of (`WorkshopItemDetails` / `latest_manifest`).
    pub latest_manifest: Option<String>,
}

impl AcfItem {
    /// Steam knows a newer version than the one it has on disk.
    pub fn update_pending(&self) -> bool {
        match (&self.manifest, &self.latest_manifest) {
            (Some(have), Some(latest)) => self.installed && have != latest,
            _ => false,
        }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct WorkshopAcf {
    /// The app-wide `NeedsDownload` flag is set.
    pub needs_download: bool,
    pub items: HashMap<u64, AcfItem>,
}

/// Reads the parts of Steam's Workshop record the diagnostics use: the per-item `manifest`
/// under `WorkshopItemsInstalled`, `latest_manifest` under `WorkshopItemDetails`, and the
/// app-wide `NeedsDownload` flag.
pub fn parse_workshop_acf(text: &str) -> Option<WorkshopAcf> {
    let root = parse_vdf(text)?;
    let app = root.get("AppWorkshop")?;
    let mut out = WorkshopAcf {
        needs_download: app.str("NeedsDownload").is_some_and(|v| v.trim() != "0" && !v.trim().is_empty()),
        ..Default::default()
    };
    if let Some(Vdf::Map(items)) = app.get("WorkshopItemsInstalled") {
        for (id, item) in items {
            let Ok(id) = id.trim().parse::<u64>() else { continue };
            let entry = out.items.entry(id).or_default();
            entry.installed = true;
            entry.manifest = item.str("manifest").map(str::to_owned);
        }
    }
    if let Some(Vdf::Map(items)) = app.get("WorkshopItemDetails") {
        for (id, item) in items {
            let Ok(id) = id.trim().parse::<u64>() else { continue };
            let entry = out.items.entry(id).or_default();
            entry.latest_manifest = item.str("latest_manifest").map(str::to_owned);
        }
    }
    Some(out)
}

// ------------------------------------------------------------------ Vulkan

/// A graphics adapter the Vulkan loader offers.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Gpu {
    pub name: String,
    /// A software renderer (no graphics card behind it).
    pub software: bool,
    /// The Vulkan version the driver offers, `(major, minor, patch)`.
    pub api: Option<(u32, u32, u32)>,
}

impl Gpu {
    fn label(&self) -> String {
        match self.api {
            Some((a, b, c)) => format!("{} (Vulkan {a}.{b}.{c})", self.name),
            None => self.name.clone(),
        }
    }
}

/// Asks the Vulkan loader for its adapters. Loads the graphics driver, so call it on a
/// worker thread. `Err` when the question itself failed.
pub fn probe_vulkan() -> Result<Vec<Gpu>, String> {
    std::panic::catch_unwind(|| {
        let instance = wgpu::Instance::new(&wgpu::InstanceDescriptor {
            backends: wgpu::Backends::VULKAN,
            flags: wgpu::InstanceFlags::empty(),
            ..Default::default()
        });
        instance
            .enumerate_adapters(wgpu::Backends::VULKAN)
            .iter()
            .map(|adapter| {
                let info = adapter.get_info();
                // SAFETY: the callback only reads the properties the adapter was created with.
                let version = unsafe {
                    adapter.as_hal::<wgpu::hal::api::Vulkan, _, _>(|hal| {
                        hal.map(|a| a.physical_device_capabilities().properties().api_version)
                    })
                };
                Gpu {
                    name: info.name.trim().to_owned(),
                    software: info.device_type == wgpu::DeviceType::Cpu,
                    api: version.map(|v| (v >> 22 & 0x7f, v >> 12 & 0x3ff, v & 0xfff)),
                }
            })
            .collect()
    })
    .map_err(|_| "The graphics driver failed while being asked about Vulkan.".to_owned())
}

// ------------------------------------------------------------------ the full run

/// Every check, including the ones that read files and load the graphics driver.
pub fn run(input: &Input) -> Vec<Finding> {
    let has_loader = input.system_dir.as_ref().is_some_and(|d| d.join("vulkan-1.dll").is_file());
    let gpus = has_loader.then(probe_vulkan);
    checks(input, gpus)
}

/// [`run`] with the Vulkan adapters handed in (`None` = not asked).
pub fn checks(input: &Input, gpus: Option<Result<Vec<Gpu>, String>>) -> Vec<Finding> {
    let mod_paths: Vec<PathBuf> = input.mods.iter().map(|m| m.path.clone()).collect();
    let mut out = quick(input.game_exe.as_deref(), input.foc.as_deref(), &mod_paths);
    check_eaw(input, &mut out);
    check_mods(input, &mut out);
    check_workshop(input, &mut out);
    check_game_files(input, &mut out);
    check_launcher_files(input, &mut out);
    check_vulkan(input, gpus, &mut out);
    check_saves(input, &mut out);
    check_preset(input, &mut out);
    check_extra_args(input, &mut out);
    check_order(input, &mut out);
    out
}

fn check_eaw(input: &Input, out: &mut Vec<Finding>) {
    let Some(foc) = input.foc.as_deref().filter(|f| f.is_dir()) else { return };
    // The game looks for <FoC>\..\GameData\sweaw.exe.
    let game_data = foc.parent().map(|p| p.join("GameData"));
    match game_data {
        Some(dir) if dir.join("sweaw.exe").is_file() => {
            out.push(Finding::ok("Empire at War is in place", dir.display().to_string()));
        }
        Some(dir) => out.push(Finding::warn(
            "Empire at War wasn't found",
            format!(
                "The game expects Empire at War next to Forces of Corruption, at {}. Without it the Empire at War archives and fallback files are not used, and the game says so when it starts.",
                dir.join("sweaw.exe").display()
            ),
            Some(Fix::OpenSettings),
        )),
        None => {}
    }
}

fn check_mods(input: &Input, out: &mut Vec<Finding>) {
    if input.mods.is_empty() {
        out.push(Finding::ok("No mods", "This profile runs unmodded Forces of Corruption."));
        return;
    }
    let mut good = 0;
    for m in &input.mods {
        if !m.path.is_dir() {
            continue; // reported by `quick`
        }
        if m.path.join("Data").is_dir() {
            good += 1;
        } else {
            out.push(Finding::warn(
                format!("{} has no Data folder", m.name),
                format!("{} has no Data folder, so the game gets nothing from it.", m.path.display()),
                Some(Fix::OpenFolder(m.path.clone())),
            ));
        }
    }
    if good == input.mods.len() {
        let detail = match good {
            1 => "The mod's folder exists and has a Data folder.".to_owned(),
            n => format!("All {n} mod folders exist and have a Data folder."),
        };
        out.push(Finding::ok("Mod folders are in place", detail));
    }
}

fn check_workshop(input: &Input, out: &mut Vec<Finding>) {
    let managed: Vec<(&ModEntry, u64)> = input.mods.iter().filter_map(|m| Some((m, m.workshop_id?))).collect();
    if managed.is_empty() {
        return;
    }
    let count = match managed.len() {
        1 => "The Workshop mod is".to_owned(),
        n => format!("All {n} Workshop mods are"),
    };

    // Steam's live answer where there is one; otherwise its own record on disk. (A live
    // connection only reports on items the account is subscribed to.)
    let need_record = managed.iter().any(|(m, _)| m.steam.is_none());
    let acf = if need_record {
        input.workshop_acf.as_deref().and_then(|p| parse_workshop_acf(&std::fs::read_to_string(p).ok()?))
    } else {
        None
    };
    let mut problems = 0;
    let mut unknown = 0;
    for (m, id) in &managed {
        if let Some(s) = m.steam {
            let detail = if s.downloading {
                match s.percent {
                    Some(p) => format!("Steam is downloading {} ({p}%). Wait for it to finish before playing.", m.name),
                    None => format!("Steam is downloading {}, or has it queued. Wait for it to finish before playing.", m.name),
                }
            } else if s.needs_update {
                format!("Steam has an update for {} that isn't downloaded yet.", m.name)
            } else if !s.installed {
                format!("Steam says {} isn't installed.", m.name)
            } else {
                continue;
            };
            problems += 1;
            out.push(Finding::warn(format!("{} isn't fully downloaded", m.name), detail, Some(Fix::OpenMods)));
            continue;
        }
        let Some(acf) = &acf else {
            unknown += 1;
            continue;
        };
        let detail = match acf.items.get(id) {
            Some(item) if item.update_pending() => {
                format!("Steam knows a newer version of {} than the one on disk and hasn't downloaded it yet.", m.name)
            }
            Some(item) if item.installed => continue,
            _ => format!("Steam's record doesn't list {} (Workshop item {id}) as installed. It may still be downloading.", m.name),
        };
        problems += 1;
        out.push(Finding::warn(format!("{} may not be fully downloaded", m.name), detail, Some(Fix::OpenMods)));
    }

    if unknown > 0 {
        let detail = if input.steam_connected {
            "Steam hasn't reported on every Workshop mod in the profile, and its record of Workshop items on disk couldn't be read. Run the diagnostics again in a moment."
        } else {
            "Steam's record of Workshop items couldn't be read. Connect to Steam on the Mods page to see whether the profile's Workshop mods are fully downloaded."
        };
        out.push(Finding::warn("Workshop downloads weren't checked", detail, Some(Fix::OpenMods)));
    } else if acf.as_ref().is_some_and(|a| a.needs_download) && !input.steam_connected {
        out.push(Finding::warn(
            "Steam has Workshop downloads waiting",
            "Steam's record for the game says Workshop content is waiting to be downloaded. Let Steam finish before playing.",
            Some(Fix::OpenMods),
        ));
    } else if problems == 0 {
        let source = if !need_record {
            "says Steam.".to_owned()
        } else if input.steam_connected {
            "according to Steam and its record on disk.".to_owned()
        } else {
            "according to Steam's record on disk. Connect to Steam on the Mods page for live download status.".to_owned()
        };
        out.push(Finding::ok("Workshop mods are downloaded", format!("{count} installed and current, {source}")));
    }
}

fn check_game_files(input: &Input, out: &mut Vec<Finding>) {
    let Some(dir) = input.game_exe.as_deref().filter(|p| p.is_file()).and_then(Path::parent) else { return };
    let missing: Vec<&str> = ["dxcompiler.dll", "dxil.dll"].into_iter().filter(|f| !dir.join(f).is_file()).collect();
    if missing.is_empty() {
        out.push(Finding::ok("Shader compiler is in place", "dxcompiler.dll and dxil.dll are next to alamo_game.exe."));
    } else {
        out.push(Finding::error(
            format!("{} missing", missing.join(" and ")),
            format!(
                "The game loads {} from its own folder to build shaders. Copy {} next to alamo_game.exe (they come with the game build).",
                missing.join(" and "),
                if missing.len() == 1 { "it" } else { "them" }
            ),
            Some(Fix::OpenFolder(dir.to_path_buf())),
        ));
    }
}

fn check_launcher_files(input: &Input, out: &mut Vec<Finding>) {
    let Some(dir) = input.launcher_dir.as_deref() else { return };
    if dir.join("steam_api64.dll").is_file() {
        out.push(Finding::ok("Steam library is in place", "steam_api64.dll is next to the launcher."));
    } else {
        out.push(Finding::warn(
            "steam_api64.dll is missing",
            "Workshop features (connecting to Steam, subscribing, browsing) need steam_api64.dll next to the launcher's exe. The game itself starts without it.",
            Some(Fix::OpenFolder(dir.to_path_buf())),
        ));
    }
}

fn check_vulkan(input: &Input, gpus: Option<Result<Vec<Gpu>, String>>, out: &mut Vec<Finding>) {
    let has_loader = input.system_dir.as_ref().is_some_and(|d| d.join("vulkan-1.dll").is_file());
    if !has_loader {
        out.push(Finding::error(
            "Vulkan isn't installed",
            "vulkan-1.dll wasn't found in the Windows system folder. The game draws with Vulkan 1.3; installing the current graphics driver adds it.",
            None,
        ));
        return;
    }
    let gpus = match gpus {
        None => return,
        Some(Err(e)) => {
            out.push(Finding::warn("Vulkan couldn't be checked", e, None));
            return;
        }
        Some(Ok(g)) => g,
    };
    if gpus.is_empty() {
        out.push(Finding::error(
            "No graphics card supports Vulkan",
            "Windows has the Vulkan runtime, but no graphics adapter offers Vulkan. The game needs Vulkan 1.3: update the graphics driver.",
            None,
        ));
        return;
    }
    let list = gpus.iter().map(Gpu::label).collect::<Vec<_>>().join(", ");
    let is_13 = |g: &&Gpu| g.api.is_some_and(|(major, minor, _)| (major, minor) >= (1, 3));
    let hardware: Vec<&Gpu> = gpus.iter().filter(|g| !g.software).collect();
    if hardware.iter().any(is_13) {
        out.push(Finding::ok("Vulkan 1.3 is available", list));
    } else if hardware.iter().any(|g| g.api.is_none()) {
        out.push(Finding::warn(
            "Vulkan version couldn't be verified",
            format!("The game needs Vulkan 1.3. Vulkan adapters were found, but the driver's version couldn't be read: {list}"),
            None,
        ));
    } else if hardware.is_empty() {
        out.push(Finding::warn(
            "Only a software Vulkan renderer was found",
            format!("No graphics card offers Vulkan, only a software renderer, which is far too slow to play on: {list}"),
            None,
        ));
    } else {
        out.push(Finding::error(
            "The graphics driver is too old",
            format!("The game needs Vulkan 1.3 and the driver offers less. Update the graphics driver. Found: {list}"),
            None,
        ));
    }
}

fn check_saves(input: &Input, out: &mut Vec<Finding>) {
    let Some(dir) = input.saves_dir.as_deref() else {
        out.push(Finding::warn("The save folder is unknown", "The save folder couldn't be worked out on this system.", None));
        return;
    };
    // A folder that doesn't exist yet is created by the game: test the nearest one that does.
    let existing = dir.ancestors().find(|p| p.is_dir());
    let Some(existing) = existing else {
        out.push(Finding::error(
            "The save folder can't be created",
            format!("{} is on a drive or path that doesn't exist.", dir.display()),
            Some(Fix::OpenProfiles),
        ));
        return;
    };
    let probe = existing.join(format!(".command-center-write-test-{}", std::process::id()));
    let written = std::fs::write(&probe, b"");
    let _ = std::fs::remove_file(&probe);
    match written {
        Ok(()) if existing == dir => out.push(Finding::ok("The save folder is writable", dir.display().to_string())),
        Ok(()) => out.push(Finding::ok(
            "The save folder can be created",
            format!("{} doesn't exist yet; it is made the first time it is needed.", dir.display()),
        )),
        Err(e) => out.push(Finding::error(
            "The save folder isn't writable",
            format!("Nothing can be written in {}: {e}. Saving in the game would fail.", existing.display()),
            Some(Fix::OpenFolder(existing.to_path_buf())),
        )),
    }
}

fn check_preset(input: &Input, out: &mut Vec<Finding>) {
    let Some((name, file)) = &input.video_preset else { return };
    if file.is_file() {
        out.push(Finding::ok("The graphics preset exists", format!("\"{name}\" is passed to the game with --video-options.")));
    } else {
        out.push(Finding::warn(
            "The graphics preset is missing",
            format!("The profile names the graphics preset \"{name}\", which no longer exists. The game starts with its own video_options.txt instead."),
            Some(Fix::OpenProfiles),
        ));
    }
}

fn check_extra_args(input: &Input, out: &mut Vec<Finding>) {
    if input.extra_args.trim().is_empty() {
        return;
    }
    let issues = launch::extra_arg_issues(&input.extra_args);
    if issues.is_empty() {
        out.push(Finding::ok("Extra arguments look right", "Every flag in the extra arguments is one the game knows."));
    }
    for issue in issues {
        out.push(Finding::warn(issue.title(), issue.text(), Some(Fix::OpenProfiles)));
    }
}

fn check_order(input: &Input, out: &mut Vec<Finding>) {
    if input.mods.len() < 2 {
        return;
    }
    match &input.order_warnings {
        None => out.push(Finding::warn(
            "The load order wasn't checked",
            "The conflict check for this profile hasn't finished yet. Run the diagnostics again in a moment.",
            Some(Fix::OpenProfiles),
        )),
        Some(list) if list.is_empty() => {
            out.push(Finding::ok("The load order looks right", "The conflict check found nothing that suggests a mod is in the wrong place."));
        }
        Some(list) => {
            for text in list {
                out.push(Finding::warn("The load order looks wrong", text.clone(), Some(Fix::OpenProfiles)));
            }
        }
    }
}

// ------------------------------------------------------------------ tests

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static NEXT: AtomicUsize = AtomicUsize::new(0);

    struct Scratch(PathBuf);

    impl Scratch {
        fn new() -> Self {
            let dir = std::env::temp_dir().join(format!(
                "cc-diagnostics-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::SeqCst)
            ));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            Scratch(dir)
        }
        fn file(&self, rel: &str, text: &str) -> PathBuf {
            let path = self.0.join(rel);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(&path, text).unwrap();
            path
        }
        fn dir(&self, rel: &str) -> PathBuf {
            let path = self.0.join(rel);
            std::fs::create_dir_all(&path).unwrap();
            path
        }
        /// A complete, healthy setup with one local mod.
        fn healthy(&self) -> Input {
            let exe = self.file("game/alamo_game.exe", "");
            self.file("game/dxcompiler.dll", "");
            self.file("game/dxil.dll", "");
            self.dir("steam/corruption/Data");
            self.file("steam/GameData/sweaw.exe", "");
            self.dir("mods/Alpha/Data");
            self.file("launcher/steam_api64.dll", "");
            self.file("system/vulkan-1.dll", "");
            Input {
                game_exe: Some(exe),
                foc: Some(self.0.join("steam/corruption")),
                mods: vec![ModEntry { name: "Alpha".into(), path: self.0.join("mods/Alpha"), ..Default::default() }],
                saves_dir: Some(self.dir("saves")),
                launcher_dir: Some(self.0.join("launcher")),
                system_dir: Some(self.0.join("system")),
                order_warnings: Some(Vec::new()),
                ..Default::default()
            }
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn good_gpu() -> Option<Result<Vec<Gpu>, String>> {
        Some(Ok(vec![Gpu { name: "Test GPU".into(), software: false, api: Some((1, 3, 280)) }]))
    }

    fn titles(findings: &[Finding], severity: Severity) -> Vec<&str> {
        findings.iter().filter(|f| f.severity == severity).map(|f| f.title.as_str()).collect()
    }

    const ACF: &str = r#""AppWorkshop"
{
	"appid"		"32470"
	"SizeOnDisk"		"104430155686"
	"NeedsUpdate"		"0"
	"NeedsDownload"		"0"
	"WorkshopItemsInstalled"
	{
		"1125571106"
		{
			"size"		"26682023071"
			"timeupdated"		"1785339388"
			"manifest"		"117925265235480311"
		}
		"1770851727"
		{
			"size"		"22180234497"
			"timeupdated"		"1692486814"
			"manifest"		"4857426212674182359"
		}
	}
	"WorkshopItemDetails"
	{
		"1125571106"
		{
			"manifest"		"117925265235480311"
			"timeupdated"		"1785339388"
			"latest_timeupdated"		"1785339388"
			"latest_manifest"		"117925265235480311"
		}
		"1770851727"
		{
			"manifest"		"4857426212674182359"
			"latest_manifest"		"9999"
		}
		"3622043265"
		{
			"latest_manifest"		"4157932020408903301"
		}
	}
}
"#;

    #[test]
    fn a_healthy_setup_passes_everything() {
        let s = Scratch::new();
        let input = s.healthy();
        let f = checks(&input, good_gpu());
        assert!(titles(&f, Severity::Error).is_empty(), "{f:?}");
        assert!(titles(&f, Severity::Warning).is_empty(), "{f:?}");
        assert_eq!(summary(&f), format!("{} checks passed", f.len()));
        assert!(f.len() >= 8, "{f:?}");
        // The write test cleans up after itself.
        assert_eq!(std::fs::read_dir(input.saves_dir.unwrap()).unwrap().count(), 0);
    }

    #[test]
    fn quick_errors_are_the_launch_blockers() {
        let s = Scratch::new();
        let mut input = s.healthy();
        let errors = |i: &Input| {
            let mods: Vec<PathBuf> = i.mods.iter().map(|m| m.path.clone()).collect();
            let q = quick(i.game_exe.as_deref(), i.foc.as_deref(), &mods);
            q.into_iter().filter(|f| f.severity == Severity::Error).map(|f| f.detail).collect::<Vec<_>>()
        };
        assert!(errors(&input).is_empty());
        input.game_exe = None;
        input.foc = None;
        input.mods.push(ModEntry { name: "Gone".into(), path: s.0.join("mods/Gone"), ..Default::default() });
        let e = errors(&input);
        assert_eq!(e.len(), 3);
        assert_eq!(e[0], "alamo_game.exe hasn't been chosen yet (Settings).");
        assert_eq!(e[1], "Forces of Corruption wasn't found (Settings).");
        assert!(e[2].starts_with("Mod folder is missing: "));
        // Every blocker is in the full run too.
        let full = checks(&input, None);
        for detail in &e {
            assert!(full.iter().any(|f| f.severity == Severity::Error && &f.detail == detail));
        }
        input.game_exe = Some(s.0.join("nowhere/alamo_game.exe"));
        input.foc = Some(s.dir("empty-foc"));
        let e = errors(&input);
        assert!(e[0].starts_with("alamo_game.exe wasn't found at "));
        assert!(e[1].starts_with("The Forces of Corruption folder has no Data folder"));
    }

    #[test]
    fn missing_pieces_are_reported_with_the_right_severity() {
        let s = Scratch::new();
        let mut input = s.healthy();
        std::fs::remove_file(s.0.join("game/dxil.dll")).unwrap();
        std::fs::remove_file(s.0.join("steam/GameData/sweaw.exe")).unwrap();
        std::fs::remove_file(s.0.join("launcher/steam_api64.dll")).unwrap();
        std::fs::remove_dir_all(s.0.join("mods/Alpha/Data")).unwrap();
        input.video_preset = Some(("Cinematic".into(), s.0.join("presets/Cinematic.txt")));
        input.order_warnings = Some(vec!["B looks like a submod of A and should be above it.".into()]);
        input.mods.push(ModEntry { name: "Beta".into(), path: s.dir("mods/Beta/Data").parent().unwrap().to_path_buf(), ..Default::default() });
        let f = checks(&input, good_gpu());
        assert_eq!(titles(&f, Severity::Error), vec!["dxil.dll missing"]);
        let warnings = titles(&f, Severity::Warning);
        for expected in [
            "Empire at War wasn't found",
            "Alpha has no Data folder",
            "steam_api64.dll is missing",
            "The graphics preset is missing",
            "The load order looks wrong",
        ] {
            assert!(warnings.contains(&expected), "{expected} not in {warnings:?}");
        }
        assert_eq!(warnings.len(), 5, "{warnings:?}");
        let fix = |title: &str| f.iter().find(|x| x.title == title).unwrap().fix.clone();
        assert_eq!(fix("Alpha has no Data folder"), Some(Fix::OpenFolder(s.0.join("mods/Alpha"))));
        assert_eq!(fix("The load order looks wrong"), Some(Fix::OpenProfiles));
        assert!(summary(&f).ends_with("1 error, 5 warnings"), "{}", summary(&f));
        assert!(as_text(&f).contains("[ERROR] dxil.dll missing"));
    }

    #[test]
    fn vulkan_findings() {
        let s = Scratch::new();
        let mut input = s.healthy();
        let vulkan = |input: &Input, gpus: Option<Result<Vec<Gpu>, String>>| {
            let mut out = Vec::new();
            check_vulkan(input, gpus, &mut out);
            out.remove(0)
        };
        let gpu = |api| Gpu { name: "Card".into(), software: false, api };
        assert_eq!(vulkan(&input, good_gpu()).detail, "Test GPU (Vulkan 1.3.280)");
        assert_eq!(vulkan(&input, Some(Ok(vec![gpu(Some((1, 2, 198)))]))).severity, Severity::Error);
        assert_eq!(vulkan(&input, Some(Ok(vec![gpu(Some((1, 2, 0))), gpu(Some((1, 4, 0)))]))).severity, Severity::Ok);
        assert_eq!(vulkan(&input, Some(Ok(vec![gpu(None)]))).title, "Vulkan version couldn't be verified");
        assert_eq!(vulkan(&input, Some(Ok(Vec::new()))).severity, Severity::Error);
        assert_eq!(vulkan(&input, Some(Err("boom".into()))).severity, Severity::Warning);
        let soft = Gpu { name: "llvmpipe".into(), software: true, api: Some((1, 3, 0)) };
        assert_eq!(vulkan(&input, Some(Ok(vec![soft]))).title, "Only a software Vulkan renderer was found");
        input.system_dir = Some(s.dir("no-vulkan-here"));
        assert_eq!(vulkan(&input, good_gpu()).title, "Vulkan isn't installed");
    }

    #[test]
    fn vdf_and_the_workshop_record() {
        let acf = parse_workshop_acf(ACF).unwrap();
        assert!(!acf.needs_download);
        assert_eq!(acf.items.len(), 3);
        let a = &acf.items[&1125571106];
        assert!(a.installed && !a.update_pending());
        assert_eq!(a.manifest.as_deref(), Some("117925265235480311"));
        assert!(acf.items[&1770851727].update_pending());
        let c = &acf.items[&3622043265];
        assert!(!c.installed && !c.update_pending());

        assert!(parse_workshop_acf(&ACF.replace("\"NeedsDownload\"\t\t\"0\"", "\"NeedsDownload\"\t\t\"1\"")).unwrap().needs_download);
        assert!(parse_workshop_acf("").is_none());
        assert!(parse_workshop_acf("\"Other\" { }").is_none());
        assert!(parse_vdf("\"a\" { \"b\" \"c\"").is_none());
        assert!(parse_vdf("\"a\" \"unterminated").is_none());
        assert!(parse_vdf("garbage").is_none());
        let v = parse_vdf("// comment\n\"a\" { \"path\" \"C:\\\\x\\\\y\" \"n\" { } }").unwrap();
        assert_eq!(v.get("A").unwrap().str("path"), Some("C:\\x\\y"));
        assert_eq!(v.get("a").unwrap().get("n"), Some(&Vdf::Map(Vec::new())));
    }

    #[test]
    fn workshop_downloads_from_steams_record() {
        let s = Scratch::new();
        let mut input = s.healthy();
        let ws = |name: &str, id: u64| ModEntry {
            name: name.into(),
            path: s.dir(&format!("ws/{id}/Data")).parent().unwrap().to_path_buf(),
            workshop_id: Some(id),
            steam: None,
        };
        input.workshop_acf = Some(s.file("ws/appworkshop_32470.acf", ACF));
        input.mods = vec![ws("Current", 1125571106)];
        let workshop = |input: &Input| {
            let mut out = Vec::new();
            check_workshop(input, &mut out);
            out
        };
        let f = workshop(&input);
        assert_eq!(titles(&f, Severity::Ok), vec!["Workshop mods are downloaded"]);
        assert_eq!(f.len(), 1);

        input.mods = vec![ws("Current", 1125571106), ws("Stale", 1770851727), ws("Queued", 3622043265), ws("Unknown", 42)];
        let f = workshop(&input);
        assert_eq!(
            titles(&f, Severity::Warning),
            vec!["Stale may not be fully downloaded", "Queued may not be fully downloaded", "Unknown may not be fully downloaded"]
        );
        assert!(f[0].detail.contains("newer version"));
        assert!(f[1].detail.contains("doesn't list Queued (Workshop item 3622043265) as installed"));
        assert!(titles(&f, Severity::Ok).is_empty());

        // No record: say it wasn't checked rather than claim anything.
        input.workshop_acf = Some(s.0.join("ws/missing.acf"));
        assert_eq!(titles(&workshop(&input), Severity::Warning), vec!["Workshop downloads weren't checked"]);

        // Connected: Steam's live answer is used and the file isn't.
        input.steam_connected = true;
        input.mods = vec![ws("Current", 1125571106), ws("Busy", 7)];
        input.mods[0].steam = Some(SteamItem { installed: true, ..Default::default() });
        input.mods[1].steam = Some(SteamItem { installed: true, downloading: true, percent: Some(40), ..Default::default() });
        let f = workshop(&input);
        assert_eq!(f.len(), 1);
        assert!(f[0].detail.contains("Busy (40%)"), "{}", f[0].detail);
        // No live answer for one of them and no record either: not checked.
        input.mods[1].steam = None;
        assert_eq!(titles(&workshop(&input), Severity::Warning), vec!["Workshop downloads weren't checked"]);
        // With the record, the one Steam didn't report on is looked up there.
        input.workshop_acf = Some(s.0.join("ws/appworkshop_32470.acf"));
        assert_eq!(titles(&workshop(&input), Severity::Warning), vec!["Busy may not be fully downloaded"]);
        input.mods[1] = ws("Stale", 1770851727);
        assert!(workshop(&input)[0].detail.contains("newer version"));
        input.mods.truncate(1);
        input.mods.push(ws("Current too", 1125571106));
        assert_eq!(titles(&workshop(&input), Severity::Ok), vec!["Workshop mods are downloaded"]);

        // Local mods are never Steam's business.
        input.mods = vec![ModEntry { name: "Local".into(), path: s.dir("mods/Local/Data"), ..Default::default() }];
        assert!(workshop(&input).is_empty());

        assert_eq!(
            workshop_acf_path(Path::new("F:\\SteamLibrary\\steamapps\\workshop\\content\\32470")),
            Some(PathBuf::from("F:\\SteamLibrary\\steamapps\\workshop\\appworkshop_32470.acf"))
        );
    }

    /// Asks the real graphics driver and reads Steam's real Workshop record.
    /// `cargo test live_vulkan_and_steam_record -- --ignored --nocapture`
    #[test]
    #[ignore = "loads the graphics driver and reads Steam's files"]
    fn live_vulkan_and_steam_record() {
        let system = system_dir();
        println!("system folder: {system:?}; vulkan-1.dll there: {:?}", system.as_ref().map(|d| d.join("vulkan-1.dll").is_file()));
        let start = std::time::Instant::now();
        let gpus = probe_vulkan();
        println!("Vulkan adapters ({:?}): {gpus:#?}", start.elapsed());
        let mut out = Vec::new();
        check_vulkan(&Input { system_dir: system, ..Default::default() }, Some(gpus), &mut out);
        println!("{out:#?}");

        let steam = crate::steam::detect();
        let acf = steam.workshop_dir.as_deref().and_then(workshop_acf_path);
        println!("record: {acf:?}");
        if let Some(text) = acf.and_then(|p| std::fs::read_to_string(p).ok()) {
            let parsed = parse_workshop_acf(&text).expect("Steam's record should parse");
            println!("needs_download={} items:", parsed.needs_download);
            let mut ids: Vec<_> = parsed.items.iter().collect();
            ids.sort_by_key(|(id, _)| **id);
            for (id, item) in ids {
                println!("  {id}: installed={} update_pending={}", item.installed, item.update_pending());
            }
        }
    }

    #[test]
    fn extra_arguments_and_saves() {
        let s = Scratch::new();
        let mut input = s.healthy();
        input.extra_args = "--no-intro --windowed AI_DEBUG=1 --audio loud --seed".into();
        let mut out = Vec::new();
        check_extra_args(&input, &mut out);
        let w = titles(&out, Severity::Warning);
        assert_eq!(w.len(), 3, "{out:?}");
        assert!(out[0].detail.contains("--windowed"), "{}", out[0].detail);
        assert!(out[0].detail.contains("code 2"), "{}", out[0].detail);
        input.extra_args = "--attract AI_DEBUG=1".into();
        out.clear();
        check_extra_args(&input, &mut out);
        assert_eq!(titles(&out, Severity::Ok).len(), 1);

        // A save folder that doesn't exist yet is fine as long as it can be made.
        input.saves_dir = Some(s.0.join("saves/profile-9/deeper"));
        out.clear();
        check_saves(&input, &mut out);
        assert_eq!(out[0].title, "The save folder can be created");
        assert!(!s.0.join("saves/profile-9").exists());
        input.saves_dir = None;
        out.clear();
        check_saves(&input, &mut out);
        assert_eq!(out[0].severity, Severity::Warning);
    }
}

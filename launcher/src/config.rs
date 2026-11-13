//! Persistent launcher state: settings, profiles, mod display names.
//! Stored as JSON in `%APPDATA%\Command Center\config.json`.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

fn yes() -> bool {
    true
}

fn five() -> u32 {
    5
}

/// A note and tags the user attached to a save file.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
pub struct SaveNote {
    #[serde(default)]
    pub note: String,
    #[serde(default)]
    pub tags: Vec<String>,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Settings {
    /// Path to `alamo_game.exe`.
    #[serde(default)]
    pub game_exe: Option<PathBuf>,
    /// Overrides the auto-detected Forces of Corruption folder.
    #[serde(default)]
    pub foc_dir_override: Option<PathBuf>,
    #[serde(default)]
    pub extra_mod_dirs: Vec<PathBuf>,
    /// Draw the launcher window with the GPU (off = software rasteriser).
    #[serde(default = "yes")]
    pub gpu_rendering: bool,
    /// What the launcher window does while the game runs.
    #[serde(default)]
    pub while_running: WhileRunning,
    /// Windows notification when the game exits or crashes.
    #[serde(default = "yes")]
    pub notify_on_exit: bool,
    /// Optional second place (another disk, a cloud-synced folder) checkpoints are mirrored to.
    #[serde(default)]
    pub backup_dir: Option<PathBuf>,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum WhileRunning {
    /// Leave the launcher window as it is.
    #[default]
    Stay,
    /// Minimise while the game runs, restore when it exits.
    Minimize,
    /// Close the launcher once the game has started.
    Close,
}

impl Default for Settings {
    fn default() -> Self {
        Self {
            game_exe: None,
            foc_dir_override: None,
            extra_mod_dirs: Vec::new(),
            gpu_rendering: true,
            while_running: WhileRunning::Stay,
            notify_on_exit: true,
            backup_dir: None,
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
pub struct LaunchOptions {
    #[serde(default)]
    pub skip_intro: bool,
    #[serde(default)]
    pub frame_overlay: bool,
    /// `None` = let the game decide (CPU threads minus one).
    #[serde(default)]
    pub worker_threads: Option<u32>,
    /// `None` = use the graphics setting; `-1` = display refresh rate; `0` = no cap.
    #[serde(default)]
    pub frame_cap: Option<i32>,
    #[serde(default)]
    pub extra_args: String,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default)]
pub struct Objective {
    pub text: String,
    #[serde(default)]
    pub done: bool,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Profile {
    pub id: u64,
    pub name: String,
    /// Mod folders in priority order (first wins).
    #[serde(default)]
    pub mods: Vec<PathBuf>,
    #[serde(default)]
    pub separate_saves: bool,
    #[serde(default)]
    pub options: LaunchOptions,
    #[serde(default)]
    pub objectives: Vec<Objective>,
    #[serde(default)]
    pub notes: String,
    #[serde(default)]
    pub play_seconds: u64,
    /// Unix seconds.
    #[serde(default)]
    pub last_played: Option<i64>,
    /// Take a checkpoint of this profile's saves each time the game is launched.
    #[serde(default)]
    pub checkpoint_before_launch: bool,
    /// How many automatic checkpoints to keep (older ones are removed).
    #[serde(default = "five")]
    pub checkpoint_keep: u32,
    /// Also checkpoint every N minutes while the game runs (0 = off).
    #[serde(default)]
    pub checkpoint_every_minutes: u32,
    /// Name of the graphics preset used at launch (`None` = the game's own video_options.txt).
    #[serde(default)]
    pub video_preset: Option<String>,
}

impl Profile {
    pub fn new(id: u64, name: &str) -> Self {
        Self {
            id,
            name: name.to_owned(),
            mods: Vec::new(),
            separate_saves: false,
            options: LaunchOptions::default(),
            objectives: Vec::new(),
            notes: String::new(),
            play_seconds: 0,
            last_played: None,
            checkpoint_before_launch: false,
            checkpoint_keep: 5,
            checkpoint_every_minutes: 0,
            video_preset: None,
        }
    }
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Config {
    #[serde(default)]
    pub settings: Settings,
    #[serde(default)]
    pub profiles: Vec<Profile>,
    #[serde(default)]
    pub active_profile: u64,
    /// Display names the user gave to mods, keyed by lower-cased folder path.
    #[serde(default)]
    pub mod_names: BTreeMap<String, String>,
    /// Descriptions the user gave to mods, keyed like `mod_names`.
    #[serde(default)]
    pub mod_descriptions: BTreeMap<String, String>,
    #[serde(default)]
    pub next_id: u64,
    /// Favourite mods (keys from `mod_key`).
    #[serde(default)]
    pub mod_favourites: std::collections::BTreeSet<String>,
    /// Tags per mod (keys from `mod_key`).
    #[serde(default)]
    pub mod_tags: BTreeMap<String, Vec<String>>,
    /// When each mod was last launched with, Unix seconds (keys from `mod_key`).
    #[serde(default)]
    pub mod_last_used: BTreeMap<String, i64>,
    /// Notes and tags on saves, keyed by `mod_key(save path)`.
    #[serde(default)]
    pub save_notes: BTreeMap<String, SaveNote>,
    /// Open tabs, the split and page windows, so the launcher reopens the way it was left.
    #[serde(default)]
    pub layout: crate::layout::Layout,
}

impl Default for Config {
    fn default() -> Self {
        Self {
            settings: Settings::default(),
            profiles: vec![Profile::new(1, "Vanilla")],
            active_profile: 1,
            mod_names: BTreeMap::new(),
            mod_descriptions: BTreeMap::new(),
            next_id: 2,
            mod_favourites: Default::default(),
            mod_tags: BTreeMap::new(),
            mod_last_used: BTreeMap::new(),
            save_notes: BTreeMap::new(),
            layout: Default::default(),
        }
    }
}

/// `%APPDATA%\Command Center`, created on demand.
pub fn data_dir() -> PathBuf {
    // COMMAND_CENTER_DATA points the launcher at another folder (portable installs, testing).
    let dir = match std::env::var_os("COMMAND_CENTER_DATA") {
        Some(p) if !p.is_empty() => PathBuf::from(p),
        _ => dirs::config_dir().unwrap_or_else(|| PathBuf::from(".")).join("Command Center"),
    };
    let _ = std::fs::create_dir_all(&dir);
    dir
}

pub fn mod_key(path: &Path) -> String {
    path.to_string_lossy().replace('/', "\\").trim_end_matches('\\').to_lowercase()
}

impl Config {
    pub fn path() -> PathBuf {
        data_dir().join("config.json")
    }

    pub fn load() -> Self {
        let mut cfg: Config = match std::fs::read_to_string(Self::path()) {
            Ok(text) => serde_json::from_str(&text).unwrap_or_else(|_| {
                // Unreadable config: keep it aside rather than overwrite it with defaults.
                let stamp = chrono::Local::now().format("%Y%m%d-%H%M%S");
                let _ = std::fs::rename(Self::path(), data_dir().join(format!("config.unreadable-{stamp}.json")));
                Config::default()
            }),
            Err(_) => Config::default(),
        };
        if cfg.profiles.is_empty() {
            cfg.profiles.push(Profile::new(cfg.next_id.max(1), "Vanilla"));
        }
        let max_id = cfg.profiles.iter().map(|p| p.id).max().unwrap_or(0);
        cfg.next_id = cfg.next_id.max(max_id + 1);
        if !cfg.profiles.iter().any(|p| p.id == cfg.active_profile) {
            cfg.active_profile = cfg.profiles[0].id;
        }
        cfg
    }

    /// Writes to a temp file and renames, so a crash can't leave half a config.
    pub fn save(&self) -> Result<(), String> {
        let path = Self::path();
        let tmp = path.with_extension("json.tmp");
        let text = serde_json::to_string_pretty(self).map_err(|e| e.to_string())?;
        std::fs::write(&tmp, text).map_err(|e| format!("Couldn't write {}: {e}", tmp.display()))?;
        std::fs::rename(&tmp, &path).map_err(|e| format!("Couldn't replace {}: {e}", path.display()))
    }

    pub fn active(&self) -> &Profile {
        self.profiles.iter().find(|p| p.id == self.active_profile).unwrap_or(&self.profiles[0])
    }

    pub fn active_mut(&mut self) -> &mut Profile {
        let idx = self.profiles.iter().position(|p| p.id == self.active_profile).unwrap_or(0);
        &mut self.profiles[idx]
    }

    pub fn add_profile(&mut self, name: &str) -> u64 {
        let id = self.next_id;
        self.next_id += 1;
        self.profiles.push(Profile::new(id, name));
        id
    }

    pub fn duplicate_profile(&mut self, id: u64) -> Option<u64> {
        let mut copy = self.profiles.iter().find(|p| p.id == id)?.clone();
        copy.id = self.next_id;
        self.next_id += 1;
        copy.name = format!("{} (copy)", copy.name);
        copy.play_seconds = 0;
        copy.last_played = None;
        let new_id = copy.id;
        self.profiles.push(copy);
        Some(new_id)
    }
}

//! `video_options.txt`: one `key=value` per line, `#` comments. Lines the launcher
//! doesn't understand are kept exactly as they were.

use std::path::{Path, PathBuf};

pub const DETAIL_KEYS: [(&str, &str, &[i32]); 7] = [
    ("particle_detail", "Particles", &[0, 40, 75, 100]),
    ("mesh_detail", "Mesh detail", &[0, 40, 75, 100]),
    ("texture_detail", "Textures", &[0, 40, 100]),
    ("water_detail", "Water", &[0, 40, 100]),
    ("shader_detail", "Shaders", &[0, 40, 75, 100]),
    ("shadow_detail", "Shadows", &[0, 100]),
    ("environment_detail", "Environment", &[0, 40, 75, 100]),
];

pub const PRESETS: [(&str, i32); 4] = [("Low", 0), ("Medium", 40), ("High", 75), ("Ultra", 100)];

/// Nearest value the game actually distinguishes for this slider.
pub fn snap(levels: &[i32], v: i32) -> i32 {
    *levels.iter().min_by_key(|l| (**l - v).abs()).unwrap_or(&v)
}

pub fn default_for(key: &str) -> &'static str {
    match key {
        "vsync" | "soft_shadows" | "heat_distortion" | "bloom" => "1",
        "vsync_mode" => "fifo",
        "gamma" => "50",
        "detail" | "particle_detail" | "mesh_detail" | "texture_detail" | "water_detail"
        | "shader_detail" | "shadow_detail" | "environment_detail" => "100",
        _ => "0",
    }
}

#[derive(Clone, Debug, Default, PartialEq)]
pub struct VideoOptions {
    pub path: PathBuf,
    lines: Vec<String>,
    pub existed: bool,
}

fn split(line: &str) -> Option<(&str, &str)> {
    let t = line.trim();
    if t.starts_with('#') {
        return None;
    }
    let (k, v) = t.split_once('=')?;
    Some((k.trim(), v.trim()))
}

impl VideoOptions {
    pub fn load(path: &Path) -> Self {
        let text = std::fs::read_to_string(path);
        Self {
            path: path.to_path_buf(),
            existed: text.is_ok(),
            lines: text.unwrap_or_default().lines().map(str::to_owned).collect(),
        }
    }

    pub fn raw(&self, key: &str) -> Option<&str> {
        // The game keeps the last occurrence of a repeated key.
        self.lines.iter().rev().find_map(|l| split(l).filter(|(k, _)| *k == key).map(|(_, v)| v))
    }

    pub fn get(&self, key: &str) -> &str {
        self.raw(key).unwrap_or_else(|| default_for(key))
    }

    pub fn int(&self, key: &str) -> i32 {
        self.get(key).parse().unwrap_or_else(|_| default_for(key).parse().unwrap_or(0))
    }

    pub fn flag(&self, key: &str) -> bool {
        matches!(self.get(key), "1" | "true" | "on")
    }

    pub fn set(&mut self, key: &str, value: impl ToString) {
        let value = value.to_string();
        let mut found = false;
        for line in self.lines.iter_mut() {
            if split(line).map(|(k, _)| k == key).unwrap_or(false) {
                *line = format!("{key}={value}");
                found = true;
            }
        }
        if !found {
            self.lines.push(format!("{key}={value}"));
        }
    }

    pub fn set_flag(&mut self, key: &str, on: bool) {
        self.set(key, if on { 1 } else { 0 });
    }

    /// `detail` is the rounded mean of the seven sliders; the game recomputes it on load.
    pub fn sync_detail(&mut self) {
        let sum: i32 = DETAIL_KEYS.iter().map(|(k, _, _)| self.int(k)).sum();
        self.set("detail", (sum as f32 / 7.0).round() as i32);
    }

    pub fn apply_preset(&mut self, level: i32) {
        for (key, _, levels) in DETAIL_KEYS {
            self.set(key, snap(levels, level));
        }
        self.sync_detail();
    }

    /// The preset all seven sliders currently match, if any.
    pub fn current_preset(&self) -> Option<&'static str> {
        PRESETS
            .iter()
            .find(|(_, level)| DETAIL_KEYS.iter().all(|(k, _, levels)| self.int(k) == snap(levels, *level)))
            .map(|(name, _)| *name)
    }

    /// Lines that aren't comments, blanks, or keys the launcher edits.
    pub fn unknown_keys(&self) -> Vec<String> {
        const KNOWN: [&str; 22] = [
            "width", "height", "refresh_hz", "vsync", "vsync_mode", "hardware_mouse", "frame_cap",
            "detail", "anti_alias", "gamma", "particle_detail", "mesh_detail", "texture_detail",
            "water_detail", "shader_detail", "shadow_detail", "environment_detail", "anisotropy",
            "soft_shadows", "heat_distortion", "bloom", "ambient_occlusion",
        ];
        self.lines
            .iter()
            .filter_map(|l| split(l))
            .filter(|(k, _)| !KNOWN.contains(k))
            .map(|(k, v)| format!("{k}={v}"))
            .collect()
    }

    pub fn save(&mut self) -> Result<(), String> {
        if let Some(dir) = self.path.parent() {
            std::fs::create_dir_all(dir).map_err(|e| format!("Couldn't create {}: {e}", dir.display()))?;
        }
        if self.lines.is_empty() || !self.existed && !self.lines.iter().any(|l| l.starts_with('#')) {
            self.lines.insert(0, "# Empire at War (Alamo) video options".to_owned());
        }
        let mut text = self.lines.join("\r\n");
        text.push_str("\r\n");
        std::fs::write(&self.path, text).map_err(|e| format!("Couldn't write {}: {e}", self.path.display()))?;
        self.existed = true;
        Ok(())
    }

    /// Replaces every line with `other`'s, keeping this file's own path.
    pub fn copy_values_from(&mut self, other: &VideoOptions) {
        self.lines = other.lines.clone();
    }

    /// Writes these values to another file and returns the options bound to that file.
    pub fn save_as(&self, path: &Path) -> Result<VideoOptions, String> {
        let mut copy = self.clone();
        copy.path = path.to_path_buf();
        copy.existed = false;
        copy.save()?;
        Ok(copy)
    }
}

// ------------------------------------------------------------------ named presets
// A preset is a complete options file of its own under the launcher's data folder. A
// profile that names one is launched with `--video-options <that file>`. The game reads
// and writes the file it is given, so changes made in its own video menu land in the preset.
//
// The `_in` functions take the presets folder, so they can be tested against a scratch
// folder; the plain ones use the launcher's own.

/// Longest preset name the launcher accepts.
pub const PRESET_NAME_MAX: usize = 48;

fn preset_char_ok(c: char) -> bool {
    c.is_alphanumeric() || matches!(c, ' ' | '-' | '_')
}

pub fn presets_dir() -> PathBuf {
    let d = crate::config::data_dir().join("video-presets");
    let _ = std::fs::create_dir_all(&d);
    d
}

pub fn preset_path_in(dir: &Path, name: &str) -> PathBuf {
    let safe: String = name.trim().chars().map(|c| if preset_char_ok(c) { c } else { '_' }).collect();
    dir.join(format!("{safe}.txt"))
}

pub fn preset_path(name: &str) -> PathBuf {
    preset_path_in(&presets_dir(), name)
}

/// Preset names, sorted.
pub fn list_presets_in(dir: &Path) -> Vec<String> {
    let mut out: Vec<String> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_file() && p.extension().map(|e| e.eq_ignore_ascii_case("txt")).unwrap_or(false))
        .filter_map(|p| p.file_stem().map(|s| s.to_string_lossy().into_owned()))
        .collect();
    out.sort_by_key(|n| n.to_lowercase());
    out
}

pub fn list_presets() -> Vec<String> {
    list_presets_in(&presets_dir())
}

/// Two preset names mean the same file (Windows file names ignore case).
pub fn same_preset(a: &str, b: &str) -> bool {
    a.trim().to_lowercase() == b.trim().to_lowercase()
}

/// Checks a name for a new preset, or for renaming the preset `renaming`. Returns the name
/// as it will be stored: trimmed, with runs of spaces collapsed.
pub fn check_preset_name_in(dir: &Path, name: &str, renaming: Option<&str>) -> Result<String, String> {
    const RESERVED: [&str; 22] = [
        "con", "prn", "aux", "nul", "com1", "com2", "com3", "com4", "com5", "com6", "com7", "com8", "com9", "lpt1",
        "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9",
    ];
    let clean = name.split_whitespace().collect::<Vec<_>>().join(" ");
    if clean.is_empty() {
        return Err("Give the preset a name.".to_owned());
    }
    if clean.chars().count() > PRESET_NAME_MAX {
        return Err(format!("Keep the name to {PRESET_NAME_MAX} characters or fewer."));
    }
    if !clean.chars().all(preset_char_ok) {
        return Err("Use only letters, numbers, spaces, - and _.".to_owned());
    }
    if RESERVED.contains(&clean.to_lowercase().as_str()) {
        return Err("Windows doesn't allow a file with that name.".to_owned());
    }
    let taken = list_presets_in(dir)
        .into_iter()
        .any(|existing| same_preset(&existing, &clean) && !renaming.is_some_and(|old| same_preset(old, &existing)));
    if taken {
        return Err(format!("There is already a preset called \"{clean}\"."));
    }
    Ok(clean)
}

pub fn check_preset_name(name: &str, renaming: Option<&str>) -> Result<String, String> {
    check_preset_name_in(&presets_dir(), name, renaming)
}

/// Creates a preset holding `source`'s values. Returns the stored name and the new file.
pub fn create_preset_in(dir: &Path, name: &str, source: &VideoOptions) -> Result<(String, VideoOptions), String> {
    let clean = check_preset_name_in(dir, name, None)?;
    let saved = source.save_as(&preset_path_in(dir, &clean))?;
    Ok((clean, saved))
}

pub fn create_preset(name: &str, source: &VideoOptions) -> Result<(String, VideoOptions), String> {
    create_preset_in(&presets_dir(), name, source)
}

/// Renames a preset's file. Returns the stored new name.
pub fn rename_preset_in(dir: &Path, old: &str, new: &str) -> Result<String, String> {
    let clean = check_preset_name_in(dir, new, Some(old))?;
    let from = preset_path_in(dir, old);
    if !from.is_file() {
        return Err(format!("The preset \"{}\" no longer exists.", old.trim()));
    }
    if clean == old.trim() {
        return Ok(clean);
    }
    let to = preset_path_in(dir, &clean);
    std::fs::rename(&from, &to).map_err(|e| format!("Couldn't rename {}: {e}", from.display()))?;
    Ok(clean)
}

pub fn rename_preset(old: &str, new: &str) -> Result<String, String> {
    rename_preset_in(&presets_dir(), old, new)
}

/// Copies a preset's file as it is on disk under a free "<name> copy" name. Returns that name.
pub fn duplicate_preset_in(dir: &Path, name: &str) -> Result<String, String> {
    let from = preset_path_in(dir, name);
    if !from.is_file() {
        return Err(format!("The preset \"{}\" no longer exists.", name.trim()));
    }
    let existing = list_presets_in(dir);
    for n in 1..1000 {
        let suffix = if n == 1 { " copy".to_owned() } else { format!(" copy {n}") };
        let room = PRESET_NAME_MAX.saturating_sub(suffix.chars().count());
        let base: String = name.trim().chars().take(room).collect();
        let candidate = format!("{}{suffix}", base.trim_end());
        if existing.iter().any(|e| same_preset(e, &candidate)) {
            continue;
        }
        let clean = check_preset_name_in(dir, &candidate, None)?;
        let to = preset_path_in(dir, &clean);
        std::fs::copy(&from, &to).map_err(|e| format!("Couldn't copy {}: {e}", from.display()))?;
        return Ok(clean);
    }
    Err("Couldn't find a free name for the copy.".to_owned())
}

pub fn duplicate_preset(name: &str) -> Result<String, String> {
    duplicate_preset_in(&presets_dir(), name)
}

/// Removes a preset's file: to the Recycle Bin when `recycle` is set, else for good.
/// A preset that is already gone counts as deleted.
pub fn delete_preset_in(dir: &Path, name: &str, recycle: bool) -> Result<(), String> {
    let path = preset_path_in(dir, name);
    if !path.is_file() {
        return Ok(());
    }
    if recycle && trash::delete(&path).is_ok() {
        return Ok(());
    }
    std::fs::remove_file(&path).map_err(|e| format!("Couldn't delete {}: {e}", path.display()))
}

/// Moves a preset's file to the Recycle Bin.
pub fn delete_preset(name: &str) -> Result<(), String> {
    delete_preset_in(&presets_dir(), name, true)
}

/// Names of the profiles launched with this preset.
pub fn profiles_using(profiles: &[crate::config::Profile], preset: &str) -> Vec<String> {
    profiles
        .iter()
        .filter(|p| p.video_preset.as_deref().is_some_and(|v| same_preset(v, preset)))
        .map(|p| p.name.clone())
        .collect()
}

/// Points every profile that uses `preset` at `replacement` (`None` = the game's own file).
/// Returns how many profiles changed.
pub fn retarget_profiles(profiles: &mut [crate::config::Profile], preset: &str, replacement: Option<&str>) -> usize {
    let mut changed = 0;
    for p in profiles.iter_mut() {
        if p.video_preset.as_deref().is_some_and(|v| same_preset(v, preset)) && p.video_preset.as_deref() != replacement {
            p.video_preset = replacement.map(str::to_owned);
            changed += 1;
        }
    }
    changed
}

/// Ready-made presets offered while the user has none of their own.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Starter {
    HighQuality,
    LowPower,
}

impl Starter {
    pub const ALL: [Starter; 2] = [Starter::HighQuality, Starter::LowPower];

    pub fn name(self) -> &'static str {
        match self {
            Starter::HighQuality => "High quality",
            Starter::LowPower => "Low power",
        }
    }

    pub fn summary(self) -> &'static str {
        match self {
            Starter::HighQuality => "Every detail setting on Ultra.",
            Starter::LowPower => "Low detail, v-sync on, capped at 60 fps.",
        }
    }

    /// `base` with this starter's settings applied. Everything else is kept.
    pub fn options(self, base: &VideoOptions) -> VideoOptions {
        let mut opts = base.clone();
        match self {
            Starter::HighQuality => opts.apply_preset(100),
            Starter::LowPower => {
                opts.apply_preset(0);
                opts.set_flag("vsync", true);
                opts.set("frame_cap", 60);
            }
        }
        opts
    }
}

// ------------------------------------------------------------------ tests

#[cfg(test)]
mod tests {
    use super::*;
    use crate::config::Profile;

    /// A fresh scratch folder that is removed again when the test ends.
    struct Scratch(PathBuf);

    impl Scratch {
        fn new(tag: &str) -> Self {
            let dir = std::env::temp_dir().join(format!("command-center-test-{tag}-{}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            Scratch(dir)
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn sample(dir: &Path) -> VideoOptions {
        let path = dir.join("source.cfg");
        std::fs::write(&path, "# my settings\r\nwidth=1920\r\nheight=1080\r\nmystery=7\r\nvsync=0\r\n").unwrap();
        VideoOptions::load(&path)
    }

    #[test]
    fn preset_names_are_checked() {
        let s = Scratch::new("video-names");
        let dir = &s.0;
        assert_eq!(check_preset_name_in(dir, "  Big   screen ", None).as_deref(), Ok("Big screen"));
        assert!(check_preset_name_in(dir, "   ", None).is_err());
        assert!(check_preset_name_in(dir, "a/b", None).is_err());
        assert!(check_preset_name_in(dir, "notes.txt", None).is_err());
        assert!(check_preset_name_in(dir, "NUL", None).is_err());
        assert!(check_preset_name_in(dir, &"x".repeat(PRESET_NAME_MAX + 1), None).is_err());
        assert!(check_preset_name_in(dir, &"x".repeat(PRESET_NAME_MAX), None).is_ok());

        create_preset_in(dir, "Laptop", &sample(dir)).unwrap();
        // Unique without regard to case, except against the preset being renamed.
        assert!(check_preset_name_in(dir, "laptop", None).is_err());
        assert_eq!(check_preset_name_in(dir, "LAPTOP", Some("Laptop")).as_deref(), Ok("LAPTOP"));
        assert!(check_preset_name_in(dir, "laptop", Some("Other")).is_err());
    }

    #[test]
    fn create_copies_every_line() {
        let s = Scratch::new("video-create");
        let dir = &s.0;
        let source = sample(dir);
        let (name, saved) = create_preset_in(dir, " Couch ", &source).unwrap();
        assert_eq!(name, "Couch");
        assert_eq!(saved.path, preset_path_in(dir, "Couch"));
        assert!(saved.existed);
        // source.cfg is not a preset: only .txt files in the folder are.
        assert_eq!(list_presets_in(dir), ["Couch"]);

        let loaded = VideoOptions::load(&preset_path_in(dir, "couch"));
        assert!(loaded.existed);
        assert_eq!(loaded.int("width"), 1920);
        assert!(!loaded.flag("vsync"));
        assert_eq!(loaded.unknown_keys(), ["mystery=7"]);
        let text = std::fs::read_to_string(&loaded.path).unwrap();
        assert!(text.starts_with("# my settings\r\n"));
        // The source file itself is untouched.
        assert_eq!(VideoOptions::load(&source.path), source);
        assert!(create_preset_in(dir, "couch", &source).is_err());
    }

    #[test]
    fn rename_duplicate_delete() {
        let s = Scratch::new("video-rename");
        let dir = &s.0;
        let source = sample(dir);
        create_preset_in(dir, "One", &source).unwrap();
        create_preset_in(dir, "Two", &source).unwrap();

        assert!(rename_preset_in(dir, "One", "two").is_err());
        assert!(rename_preset_in(dir, "Missing", "Three").is_err());
        assert_eq!(rename_preset_in(dir, "One", "Uno").as_deref(), Ok("Uno"));
        assert_eq!(list_presets_in(dir), ["Two", "Uno"]);
        // Changing only the case is a rename too.
        assert_eq!(rename_preset_in(dir, "Uno", "UNO").as_deref(), Ok("UNO"));
        assert_eq!(list_presets_in(dir), ["Two", "UNO"]);

        assert_eq!(duplicate_preset_in(dir, "Two").as_deref(), Ok("Two copy"));
        assert_eq!(duplicate_preset_in(dir, "Two").as_deref(), Ok("Two copy 2"));
        assert!(duplicate_preset_in(dir, "Missing").is_err());
        assert_eq!(VideoOptions::load(&preset_path_in(dir, "Two copy 2")).int("height"), 1080);
        let long = "y".repeat(PRESET_NAME_MAX);
        create_preset_in(dir, &long, &source).unwrap();
        let copy = duplicate_preset_in(dir, &long).unwrap();
        assert_eq!(copy.chars().count(), PRESET_NAME_MAX);
        assert!(copy.ends_with(" copy"));

        delete_preset_in(dir, "two", false).unwrap();
        assert!(!preset_path_in(dir, "Two").exists());
        // Deleting what is already gone is fine.
        delete_preset_in(dir, "Two", false).unwrap();
        assert_eq!(list_presets_in(dir).len(), 5);
    }

    #[test]
    fn profiles_follow_their_preset() {
        let mut profiles = vec![Profile::new(1, "A"), Profile::new(2, "B"), Profile::new(3, "C")];
        profiles[0].video_preset = Some("Laptop".to_owned());
        profiles[1].video_preset = Some("laptop".to_owned());
        profiles[2].video_preset = Some("Desk".to_owned());
        assert_eq!(profiles_using(&profiles, "LAPTOP"), ["A", "B"]);
        assert_eq!(retarget_profiles(&mut profiles, "Laptop", Some("Travel")), 2);
        assert_eq!(profiles_using(&profiles, "Travel"), ["A", "B"]);
        assert_eq!(retarget_profiles(&mut profiles, "Travel", None), 2);
        assert!(profiles[0].video_preset.is_none() && profiles[1].video_preset.is_none());
        assert_eq!(profiles[2].video_preset.as_deref(), Some("Desk"));
        assert_eq!(retarget_profiles(&mut profiles, "Nobody", None), 0);
    }

    #[test]
    fn starters_and_copying() {
        let s = Scratch::new("video-starters");
        let base = sample(&s.0);
        let high = Starter::HighQuality.options(&base);
        assert_eq!(high.current_preset(), Some("Ultra"));
        assert_eq!(high.int("width"), 1920);
        let low = Starter::LowPower.options(&base);
        assert_eq!(low.current_preset(), Some("Low"));
        assert!(low.flag("vsync"));
        assert_eq!(low.int("frame_cap"), 60);
        assert_eq!(low.unknown_keys(), ["mystery=7"]);

        let mut other = VideoOptions::load(&s.0.join("other.txt"));
        other.copy_values_from(&low);
        assert_eq!(other.path, s.0.join("other.txt"));
        assert_eq!(other.int("frame_cap"), 60);
        assert_eq!(other.int("height"), 1080);
    }
}

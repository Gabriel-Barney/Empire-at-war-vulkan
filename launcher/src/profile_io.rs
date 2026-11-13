//! Profile export and import: a small JSON file (`*.ccprofile`) that describes a profile
//! well enough to rebuild it on another PC.
//!
//! What goes in: the name, launch options, objectives, notes, checkpoint settings, the
//! *name* of the graphics preset, and the ordered mod list. Workshop mods are written by
//! their Workshop id, other mods by their folder name. What never goes in: play time,
//! absolute paths, anything about this PC.

use crate::config::{LaunchOptions, Objective, Profile};
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

/// Marks a file as one of ours.
pub const FORMAT: &str = "command-center-profile";
/// The newest file layout this build reads and the one it writes.
pub const VERSION: u32 = 1;
pub const EXTENSION: &str = "ccprofile";

const MAX_FILE_BYTES: u64 = 2 * 1024 * 1024;
const MAX_MODS: usize = 500;
const MAX_OBJECTIVES: usize = 500;
const MAX_NAME_CHARS: usize = 120;
const MAX_TEXT_CHARS: usize = 100_000;
/// The choices the Profiles page offers for "Also checkpoint while playing".
pub const CHECKPOINT_MINUTES: [u32; 6] = [0, 5, 10, 15, 30, 60];

fn five() -> u32 {
    5
}

/// One mod in an exported profile.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
#[serde(tag = "kind", rename_all = "lowercase")]
pub enum ModRef {
    /// A Steam Workshop item, by its id. Never a path.
    Workshop {
        #[serde(deserialize_with = "id_text")]
        id: String,
        #[serde(default)]
        name: String,
    },
    /// Any other mod, by the name of its folder only.
    Local {
        folder: String,
        #[serde(default)]
        name: String,
    },
}

impl ModRef {
    pub fn name(&self) -> &str {
        match self {
            ModRef::Workshop { name, id } if name.trim().is_empty() => id,
            ModRef::Local { name, folder } if name.trim().is_empty() => folder,
            ModRef::Workshop { name, .. } | ModRef::Local { name, .. } => name,
        }
    }
}

/// Workshop ids are written as text; a bare number is accepted too.
fn id_text<'de, D: serde::Deserializer<'de>>(d: D) -> Result<String, D::Error> {
    match serde_json::Value::deserialize(d)? {
        serde_json::Value::String(s) => Ok(s.trim().to_owned()),
        serde_json::Value::Number(n) => Ok(n.to_string()),
        _ => Err(serde::de::Error::custom("a Workshop id must be a number")),
    }
}

/// The contents of a `.ccprofile` file. Fields this build doesn't know are ignored.
#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct ProfileFile {
    pub format: String,
    pub version: u32,
    pub name: String,
    #[serde(default)]
    pub options: LaunchOptions,
    #[serde(default)]
    pub separate_saves: bool,
    #[serde(default)]
    pub objectives: Vec<Objective>,
    #[serde(default)]
    pub notes: String,
    #[serde(default)]
    pub checkpoint_before_launch: bool,
    #[serde(default = "five")]
    pub checkpoint_keep: u32,
    #[serde(default)]
    pub checkpoint_every_minutes: u32,
    /// The preset's name only; its settings stay on the PC it was made on.
    #[serde(default)]
    pub video_preset: Option<String>,
    #[serde(default)]
    pub mods: Vec<ModRef>,
}

/// What the launcher knows about an installed mod, as far as matching goes.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Installed {
    pub path: PathBuf,
    pub folder: String,
    pub name: String,
    pub workshop_id: Option<String>,
    /// Lives in Steam's Workshop content folder.
    pub from_workshop: bool,
}

fn digits(s: &str) -> bool {
    !s.is_empty() && s.len() <= 20 && s.bytes().all(|b| b.is_ascii_digit())
}

/// `C:\`, `C:/`, `\\server`, `/home`: something that only means anything on one PC.
pub fn has_absolute_path(text: &str) -> bool {
    let b = text.as_bytes();
    text.contains("\\\\")
        || text.split(|c: char| c.is_whitespace() || c == '"' || c == '=').any(|t| t.starts_with('/') && t.len() > 1)
        || b.windows(3).any(|w| w[0].is_ascii_alphabetic() && w[1] == b':' && (w[2] == b'\\' || w[2] == b'/'))
}

/// Result of [`export`]: the file, and what was left out on purpose.
pub struct Exported {
    pub file: ProfileFile,
    /// The extra arguments contained a folder path and were not written.
    pub dropped_extra_args: bool,
}

/// Describes `profile` without anything that only makes sense on this PC. `lookup` finds
/// the scanned mod for a path; `workshop_dir` is Steam's Workshop content folder.
pub fn export(profile: &Profile, lookup: impl Fn(&Path) -> Option<Installed>, workshop_dir: Option<&Path>) -> Exported {
    let mods = profile
        .mods
        .iter()
        .map(|path| {
            let folder = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
            match lookup(path) {
                Some(m) => match m.workshop_id.filter(|id| digits(id)) {
                    Some(id) => ModRef::Workshop { id, name: m.name },
                    None => ModRef::Local { folder: m.folder, name: m.name },
                },
                // Not installed right now: a numbered folder in the Workshop folder is still
                // a Workshop item.
                None => {
                    let in_workshop = workshop_dir
                        .zip(path.parent())
                        .is_some_and(|(ws, parent)| crate::steam::same_path(ws, parent));
                    if in_workshop && digits(&folder) {
                        ModRef::Workshop { id: folder, name: String::new() }
                    } else {
                        ModRef::Local { name: folder.clone(), folder }
                    }
                }
            }
        })
        .collect();
    let mut options = profile.options.clone();
    let dropped_extra_args = has_absolute_path(&options.extra_args);
    if dropped_extra_args {
        options.extra_args.clear();
    }
    Exported {
        file: ProfileFile {
            format: FORMAT.to_owned(),
            version: VERSION,
            name: profile.name.clone(),
            options,
            separate_saves: profile.separate_saves,
            objectives: profile.objectives.clone(),
            notes: profile.notes.clone(),
            checkpoint_before_launch: profile.checkpoint_before_launch,
            checkpoint_keep: profile.checkpoint_keep,
            checkpoint_every_minutes: profile.checkpoint_every_minutes,
            video_preset: profile.video_preset.clone().filter(|p| !p.trim().is_empty()),
            mods,
        },
        dropped_extra_args,
    }
}

pub fn to_json(file: &ProfileFile) -> Result<String, String> {
    serde_json::to_string_pretty(file).map_err(|e| e.to_string())
}

/// A file name for the save dialog: the profile's name without characters Windows rejects.
pub fn default_file_name(profile_name: &str) -> String {
    let mut stem: String = profile_name
        .trim()
        .chars()
        .map(|c| if c.is_control() || matches!(c, '<' | '>' | ':' | '"' | '/' | '\\' | '|' | '?' | '*') { '_' } else { c })
        .take(80)
        .collect();
    stem.truncate(stem.trim_end_matches(['.', ' ']).len());
    if stem.is_empty() {
        stem = "profile".to_owned();
    }
    format!("{stem}.{EXTENSION}")
}

pub fn save(path: &Path, file: &ProfileFile) -> Result<(), String> {
    let text = to_json(file)?;
    std::fs::write(path, text).map_err(|e| format!("Couldn't write {}: {e}", path.display()))
}

pub fn load(path: &Path) -> Result<ProfileFile, String> {
    let size = std::fs::metadata(path).map_err(|e| format!("Couldn't read {}: {e}", path.display()))?.len();
    if size > MAX_FILE_BYTES {
        return Err("That file is far too big to be a profile.".to_owned());
    }
    let bytes = std::fs::read(path).map_err(|e| format!("Couldn't read {}: {e}", path.display()))?;
    let text = String::from_utf8(bytes).map_err(|_| "That file isn't a Command Center profile (it isn't text).".to_owned())?;
    parse(&text)
}

/// Reads and checks a profile file. Unknown fields are ignored; a file from a newer
/// launcher, or anything that isn't a profile, is refused with a reason.
pub fn parse(text: &str) -> Result<ProfileFile, String> {
    let not_ours = || "That file isn't a Command Center profile.".to_owned();
    let value: serde_json::Value = serde_json::from_str(text.trim_start_matches('\u{feff}')).map_err(|_| not_ours())?;
    if value.get("format").and_then(|f| f.as_str()) != Some(FORMAT) {
        return Err(not_ours());
    }
    let version = value.get("version").and_then(|v| v.as_u64()).ok_or_else(not_ours)?;
    if version == 0 {
        return Err(not_ours());
    }
    if version > VERSION as u64 {
        return Err(format!(
            "This profile was exported by a newer Command Center (file format {version}; this version reads up to {VERSION}). Update the launcher to import it."
        ));
    }
    let mut file: ProfileFile =
        serde_json::from_value(value).map_err(|e| format!("The profile file is damaged: {e}."))?;
    validate(&mut file)?;
    Ok(file)
}

fn clamp_chars(s: &mut String, max: usize) {
    if let Some((at, _)) = s.char_indices().nth(max) {
        s.truncate(at);
    }
}

/// Refuses what can't be right and tidies what can be.
fn validate(file: &mut ProfileFile) -> Result<(), String> {
    file.name = file.name.split_whitespace().collect::<Vec<_>>().join(" ");
    clamp_chars(&mut file.name, MAX_NAME_CHARS);
    if file.name.is_empty() {
        file.name = "Imported profile".to_owned();
    }
    if file.mods.len() > MAX_MODS {
        return Err(format!("The profile file lists {} mods, which can't be right.", file.mods.len()));
    }
    if file.objectives.len() > MAX_OBJECTIVES {
        return Err(format!("The profile file lists {} objectives, which can't be right.", file.objectives.len()));
    }
    for m in &mut file.mods {
        match m {
            ModRef::Workshop { id, name } => {
                if !digits(id) || id.parse::<u64>().map_or(true, |n| n == 0) {
                    return Err(format!("The profile file has a Workshop mod with an id that isn't a number: \"{id}\"."));
                }
                clamp_chars(name, 200);
            }
            ModRef::Local { folder, name } => {
                // A folder name, never a path: nothing in a file gets to point at a place on disk.
                let t = folder.trim().to_owned();
                let bad = t.is_empty() || t == "." || t == ".." || t.len() > 200 || t.contains(['\\', '/', ':', '\0']);
                if bad {
                    return Err(format!("The profile file has a mod whose folder isn't a plain folder name: \"{folder}\"."));
                }
                *folder = t;
                clamp_chars(name, 200);
            }
        }
    }
    clamp_chars(&mut file.notes, MAX_TEXT_CHARS);
    for o in &mut file.objectives {
        o.text = o.text.replace(['\r', '\n'], " ");
        clamp_chars(&mut o.text, 500);
    }
    file.options.extra_args = file.options.extra_args.replace(['\r', '\n'], " ");
    clamp_chars(&mut file.options.extra_args, 2000);
    file.options.worker_threads = file.options.worker_threads.map(|n| n.min(256));
    file.options.frame_cap = file.options.frame_cap.map(|c| c.clamp(-1, 1000));
    file.checkpoint_keep = file.checkpoint_keep.clamp(1, 50);
    if !CHECKPOINT_MINUTES.contains(&file.checkpoint_every_minutes) {
        file.checkpoint_every_minutes = 0;
    }
    file.video_preset = file.video_preset.take().map(|p| p.trim().to_owned()).filter(|p| !p.is_empty() && p.chars().count() <= 100);
    Ok(())
}

/// Where a mod from the file stands on this PC.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Status {
    Installed(PathBuf),
    /// A Workshop mod that isn't on disk. `expected` is where Steam will put it, when the
    /// Workshop folder is known.
    MissingWorkshop { id: u64, expected: Option<PathBuf> },
    /// A mod from outside the Workshop: the launcher can't fetch it.
    MissingLocal,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Resolved {
    pub entry: ModRef,
    pub status: Status,
}

/// Matches every mod in the file against the installed ones: Workshop mods by id, other
/// mods by folder name (ignoring case).
pub fn resolve(file: &ProfileFile, installed: &[Installed], workshop_dir: Option<&Path>) -> Vec<Resolved> {
    file.mods
        .iter()
        .map(|entry| {
            let status = match entry {
                ModRef::Workshop { id, .. } => {
                    let same = |m: &&Installed| m.workshop_id.as_deref() == Some(id.as_str());
                    // Steam's own copy first, then a local copy that carries the same id.
                    let found = installed.iter().filter(same).find(|m| m.from_workshop).or_else(|| installed.iter().find(same));
                    match found {
                        Some(m) => Status::Installed(m.path.clone()),
                        None => Status::MissingWorkshop {
                            id: id.parse().unwrap_or(0),
                            expected: workshop_dir.map(|d| d.join(id)),
                        },
                    }
                }
                ModRef::Local { folder, .. } => {
                    let same = |m: &&Installed| m.folder.eq_ignore_ascii_case(folder);
                    let found = installed.iter().filter(same).find(|m| !m.from_workshop).or_else(|| installed.iter().find(same));
                    match found {
                        Some(m) => Status::Installed(m.path.clone()),
                        None => Status::MissingLocal,
                    }
                }
            };
            Resolved { entry: entry.clone(), status }
        })
        .collect()
}

/// `name`, or `name (2)`, `name (3)`… when a profile is already called that.
pub fn unique_name(name: &str, existing: &[String]) -> String {
    let name = name.trim();
    let taken = |n: &str| existing.iter().any(|e| e.trim().eq_ignore_ascii_case(n));
    if !taken(name) {
        return name.to_owned();
    }
    (2..).map(|i| format!("{name} ({i})")).find(|n| !taken(n)).unwrap_or_else(|| name.to_owned())
}

/// Builds the profile. Installed mods go in, in order. A missing Workshop mod goes in at
/// its place too, as the folder Steam will install it to, so the profile is complete and
/// simply shows that mod as missing until the download is done. Missing local mods can't
/// be placed and are left out.
pub fn build_profile(id: u64, name: &str, file: &ProfileFile, resolved: &[Resolved]) -> Profile {
    let mut p = Profile::new(id, name);
    for r in resolved {
        let path = match &r.status {
            Status::Installed(path) => Some(path.clone()),
            Status::MissingWorkshop { expected, .. } => expected.clone(),
            Status::MissingLocal => None,
        };
        if let Some(path) = path {
            if !p.mods.iter().any(|m| crate::steam::same_path(m, &path)) {
                p.mods.push(path);
            }
        }
    }
    p.separate_saves = file.separate_saves;
    p.options = file.options.clone();
    p.objectives = file.objectives.clone();
    p.notes = file.notes.clone();
    p.checkpoint_before_launch = file.checkpoint_before_launch;
    p.checkpoint_keep = file.checkpoint_keep;
    p.checkpoint_every_minutes = file.checkpoint_every_minutes;
    p.video_preset = file.video_preset.clone();
    p
}

// ------------------------------------------------------------------ tests

#[cfg(test)]
mod tests {
    use super::*;

    fn installed(path: &str, id: Option<&str>, from_workshop: bool) -> Installed {
        let path = PathBuf::from(path);
        Installed {
            folder: path.file_name().unwrap().to_string_lossy().into_owned(),
            name: format!("Name of {}", path.file_name().unwrap().to_string_lossy()),
            path,
            workshop_id: id.map(str::to_owned),
            from_workshop,
        }
    }

    const WS: &str = "F:\\SteamLibrary\\steamapps\\workshop\\content\\32470";

    fn library() -> Vec<Installed> {
        vec![
            installed("F:\\SteamLibrary\\steamapps\\workshop\\content\\32470\\1125571106", Some("1125571106"), true),
            installed("D:\\Games\\EaW\\corruption\\Mods\\MyTweaks", None, false),
            installed("D:\\Dev\\RemakeLocal", Some("1770851727"), false),
        ]
    }

    fn sample() -> Profile {
        let mut p = Profile::new(7, "Thrawn's Revenge: <test>");
        p.mods = vec![
            PathBuf::from("D:\\Games\\EaW\\corruption\\Mods\\MyTweaks"),
            PathBuf::from("F:\\SteamLibrary\\steamapps\\workshop\\content\\32470\\1125571106"),
            PathBuf::from("D:\\Dev\\RemakeLocal"),
            // Not installed any more.
            PathBuf::from("F:\\SteamLibrary\\steamapps\\workshop\\content\\32470\\3622043265"),
            PathBuf::from("E:\\Old\\GoneMod"),
        ];
        p.separate_saves = true;
        p.options.skip_intro = true;
        p.options.worker_threads = Some(6);
        p.options.frame_cap = Some(-1);
        p.options.extra_args = "AI_DEBUG=1 --attract".into();
        p.objectives = vec![Objective { text: "Take Coruscant".into(), done: true }, Objective { text: "Hold Kuat".into(), done: false }];
        p.notes = "Line one\nLine two".into();
        p.play_seconds = 99_999;
        p.last_played = Some(1_700_000_000);
        p.checkpoint_before_launch = true;
        p.checkpoint_keep = 9;
        p.checkpoint_every_minutes = 15;
        p.video_preset = Some("Cinematic".into());
        p
    }

    fn export_sample() -> Exported {
        let lib = library();
        export(&sample(), |p| lib.iter().find(|m| m.path == p).cloned(), Some(Path::new(WS)))
    }

    #[test]
    fn export_names_mods_by_id_or_folder_and_nothing_personal() {
        let e = export_sample();
        assert!(!e.dropped_extra_args);
        assert_eq!(
            e.file.mods,
            vec![
                ModRef::Local { folder: "MyTweaks".into(), name: "Name of MyTweaks".into() },
                ModRef::Workshop { id: "1125571106".into(), name: "Name of 1125571106".into() },
                ModRef::Workshop { id: "1770851727".into(), name: "Name of RemakeLocal".into() },
                ModRef::Workshop { id: "3622043265".into(), name: String::new() },
                ModRef::Local { folder: "GoneMod".into(), name: "GoneMod".into() },
            ]
        );
        let json = to_json(&e.file).unwrap();
        for private in ["D:\\\\", "F:\\\\", "E:\\\\", "SteamLibrary", "Games", "play_seconds", "last_played", "99999", "1700000000", "\"id\": 7"] {
            assert!(!json.contains(private), "{private} leaked into {json}");
        }
        assert!(json.contains("\"kind\": \"workshop\""));
        assert!(json.contains("\"kind\": \"local\""));
        assert!(json.contains("\"video_preset\": \"Cinematic\""));
    }

    #[test]
    fn extra_arguments_with_a_path_are_left_out() {
        let mut p = sample();
        p.options.extra_args = "--cache-dir \"C:\\Users\\me\\cache\" AI_DEBUG=1".into();
        let e = export(&p, |_| None, None);
        assert!(e.dropped_extra_args);
        assert!(e.file.options.extra_args.is_empty());
        assert!(has_absolute_path("--png-dir D:/shots"));
        assert!(has_absolute_path("X=\\\\server\\share"));
        assert!(has_absolute_path("--out /tmp/x"));
        assert!(!has_absolute_path("AI_DEBUG=1 --frame-cap 60 RATIO=16:9 a/b"));
    }

    #[test]
    fn export_then_import_round_trips() {
        let e = export_sample();
        let file = parse(&to_json(&e.file).unwrap()).unwrap();
        assert_eq!(to_json(&file).unwrap(), to_json(&e.file).unwrap());

        // On the PC it came from, everything installed resolves to the very same folders.
        let lib = library();
        let resolved = resolve(&file, &lib, Some(Path::new(WS)));
        let original = sample();
        let name = unique_name(&file.name, &[original.name.clone()]);
        assert_eq!(name, "Thrawn's Revenge: <test> (2)");
        let p = build_profile(12, &name, &file, &resolved);
        assert_eq!(p.id, 12);
        // The missing Workshop mod keeps its place; the missing local mod can't be placed.
        assert_eq!(p.mods, original.mods[..4].to_vec());
        assert_eq!(p.options, original.options);
        assert_eq!(p.separate_saves, original.separate_saves);
        assert_eq!(p.notes, original.notes);
        assert_eq!(p.objectives.len(), 2);
        assert!(p.objectives[0].done && !p.objectives[1].done);
        assert_eq!((p.checkpoint_before_launch, p.checkpoint_keep, p.checkpoint_every_minutes), (true, 9, 15));
        assert_eq!(p.video_preset.as_deref(), Some("Cinematic"));
        assert_eq!((p.play_seconds, p.last_played), (0, None));
    }

    #[test]
    fn mods_are_matched_by_id_and_by_folder() {
        let file = parse(
            r#"{"format":"command-center-profile","version":1,"name":"X","mods":[
                {"kind":"workshop","id":"1125571106","name":"TR"},
                {"kind":"workshop","id":1770851727},
                {"kind":"workshop","id":"2858787558","name":"Not here"},
                {"kind":"local","folder":"mytweaks","name":"Tweaks"},
                {"kind":"local","folder":"Elsewhere"},
                {"kind":"local","folder":"1125571106"}
            ]}"#,
        )
        .unwrap();
        let mut lib = library();
        // A local copy that carries the same id as Steam's copy: Steam's wins.
        lib.insert(0, installed("D:\\Dev\\TR-dev", Some("1125571106"), false));
        let r = resolve(&file, &lib, Some(Path::new(WS)));
        let status: Vec<&Status> = r.iter().map(|x| &x.status).collect();
        assert_eq!(status[0], &Status::Installed(PathBuf::from(format!("{WS}\\1125571106"))));
        assert_eq!(status[1], &Status::Installed(PathBuf::from("D:\\Dev\\RemakeLocal")));
        assert_eq!(
            status[2],
            &Status::MissingWorkshop { id: 2858787558, expected: Some(PathBuf::from(format!("{WS}\\2858787558"))) }
        );
        assert_eq!(status[3], &Status::Installed(PathBuf::from("D:\\Games\\EaW\\corruption\\Mods\\MyTweaks")));
        assert_eq!(status[4], &Status::MissingLocal);
        // A local mod is never matched to a Workshop folder while a local one could exist,
        // but a folder of that name is all there is here.
        assert_eq!(status[5], &Status::Installed(PathBuf::from(format!("{WS}\\1125571106"))));
        assert_eq!(r[1].entry.name(), "1770851727");
        assert_eq!(r[4].entry.name(), "Elsewhere");

        // Without a Workshop folder a missing Workshop mod has nowhere to be expected.
        let r = resolve(&file, &lib, None);
        assert_eq!(r[2].status, Status::MissingWorkshop { id: 2858787558, expected: None });
        let p = build_profile(3, "X", &file, &r);
        assert_eq!(p.mods.len(), 3, "{:?}", p.mods);
    }

    #[test]
    fn malformed_files_are_refused_with_a_reason() {
        let not_ours = "That file isn't a Command Center profile.";
        assert_eq!(parse("").unwrap_err(), not_ours);
        assert_eq!(parse("{ not json").unwrap_err(), not_ours);
        assert_eq!(parse("[1,2,3]").unwrap_err(), not_ours);
        assert_eq!(parse(r#"{"name":"x","version":1}"#).unwrap_err(), not_ours);
        assert_eq!(parse(r#"{"format":"something-else","version":1,"name":"x"}"#).unwrap_err(), not_ours);
        assert_eq!(parse(r#"{"format":"command-center-profile","name":"x"}"#).unwrap_err(), not_ours);
        assert_eq!(parse(r#"{"format":"command-center-profile","version":0,"name":"x"}"#).unwrap_err(), not_ours);
        let newer = parse(r#"{"format":"command-center-profile","version":2,"name":"x"}"#).unwrap_err();
        assert!(newer.contains("newer Command Center") && newer.contains("format 2"), "{newer}");
        let damaged = parse(r#"{"format":"command-center-profile","version":1}"#).unwrap_err();
        assert!(damaged.starts_with("The profile file is damaged"), "{damaged}");
        let ours = |mods: &str| parse(&format!(r#"{{"format":"command-center-profile","version":1,"name":"x","mods":[{mods}]}}"#));
        assert!(ours(r#"{"kind":"workshop","id":"12ab"}"#).unwrap_err().contains("isn't a number"));
        assert!(ours(r#"{"kind":"workshop","id":"0"}"#).is_err());
        assert!(ours(r#"{"kind":"workshop","id":true}"#).is_err());
        assert!(ours(r#"{"kind":"local","folder":"..\\..\\Windows"}"#).unwrap_err().contains("plain folder name"));
        assert!(ours(r#"{"kind":"local","folder":"C:\\Mods\\X"}"#).is_err());
        assert!(ours(r#"{"kind":"local","folder":".."}"#).is_err());
        assert!(ours(r#"{"kind":"local","folder":"  "}"#).is_err());
        assert!(ours(r#"{"kind":"torrent","magnet":"x"}"#).is_err());
        assert!(ours(r#"{"kind":"local","folder":"Fine"}"#).is_ok());
    }

    #[test]
    fn unknown_fields_are_tolerated_and_odd_values_tidied() {
        let file = parse(
            "\u{feff}{\"format\":\"command-center-profile\",\"version\":1,\"name\":\"  Big \\n  name  \",\"future_field\":{\"a\":[1,2]},
              \"options\":{\"skip_intro\":true,\"worker_threads\":9000,\"frame_cap\":-50,\"extra_args\":\"A=1\\nB=2\",\"new_option\":3},
              \"checkpoint_keep\":0,\"checkpoint_every_minutes\":7,\"video_preset\":\"  \",
              \"mods\":[{\"kind\":\"local\",\"folder\":\" Spaced \",\"author\":\"someone\"}],
              \"objectives\":[{\"text\":\"a\\nb\"}]}",
        )
        .unwrap();
        assert_eq!(file.name, "Big name");
        assert!(file.options.skip_intro);
        assert_eq!(file.options.worker_threads, Some(256));
        assert_eq!(file.options.frame_cap, Some(-1));
        assert_eq!(file.options.extra_args, "A=1 B=2");
        assert_eq!(file.checkpoint_keep, 1);
        assert_eq!(file.checkpoint_every_minutes, 0);
        assert_eq!(file.video_preset, None);
        assert_eq!(file.mods, vec![ModRef::Local { folder: "Spaced".into(), name: String::new() }]);
        assert_eq!(file.objectives[0].text, "a b");
        assert!(!file.objectives[0].done);
        // Missing optional parts get the same defaults as a new profile.
        let bare = parse(r#"{"format":"command-center-profile","version":1,"name":""}"#).unwrap();
        assert_eq!(bare.name, "Imported profile");
        assert_eq!(bare.checkpoint_keep, 5);
        assert!(bare.mods.is_empty());
    }

    #[test]
    fn names_and_file_names() {
        let existing = vec!["Vanilla".to_owned(), "vanilla (2)".to_owned(), "Other".to_owned()];
        assert_eq!(unique_name("New", &existing), "New");
        assert_eq!(unique_name(" VANILLA ", &existing), "VANILLA (3)");
        assert_eq!(default_file_name("Thrawn's Revenge: <test>"), "Thrawn's Revenge_ _test_.ccprofile");
        assert_eq!(default_file_name("  ...  "), "profile.ccprofile");
        assert_eq!(default_file_name("a/b\\c."), "a_b_c.ccprofile");
    }
}

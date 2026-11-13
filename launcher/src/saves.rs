//! `.alsv` save headers, and checkpoints (verified copies of a whole save folder).
//!
//! Container (little-endian): u32 magic "ALSV", u32 format, u64 tick, then a
//! length-prefixed UTF-8 description. The description holds a label line, the marker
//! `alamo_game_save 1`, then `key=value` lines — all readable without unpacking the body.

use crate::config::{self, SaveNote};
use crate::steam::same_path;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::collections::BTreeMap;
use std::io::Read;
use std::path::{Path, PathBuf};
use std::time::SystemTime;

const MAGIC: u32 = 0x5653_4C41;

#[derive(Clone, Debug)]
pub struct SaveInfo {
    pub path: PathBuf,
    pub name: String,
    pub size: u64,
    pub modified: SystemTime,
    pub error: Option<String>,
    pub format: u32,
    pub tick: u64,
    /// "campaign" or "skirmish".
    pub kind: String,
    pub campaign: String,
    pub title: String,
    pub faction: String,
    pub difficulty: Option<u8>,
    pub week: u32,
    pub game_seconds: u64,
    pub saved_at: Option<i64>,
}

impl SaveInfo {
    pub fn is_campaign(&self) -> bool {
        self.kind == "campaign"
    }
    pub fn difficulty_label(&self) -> &'static str {
        match self.difficulty {
            Some(0) => "Easy",
            Some(1) => "Normal",
            Some(2) => "Hard",
            _ => "—",
        }
    }
    pub fn display_title(&self) -> String {
        let t = if self.title.is_empty() { &self.campaign } else { &self.title };
        if t.is_empty() {
            "Unknown".into()
        } else if self.is_campaign() {
            t.replace('_', " ")
        } else {
            format!("Skirmish · {}", t.replace('_', " "))
        }
    }
    pub fn when(&self) -> SystemTime {
        match self.saved_at {
            Some(s) if s > 0 => SystemTime::UNIX_EPOCH + std::time::Duration::from_secs(s as u64),
            _ => self.modified,
        }
    }
}

pub fn read_header(path: &Path) -> SaveInfo {
    let meta = std::fs::metadata(path).ok();
    let mut info = SaveInfo {
        path: path.to_path_buf(),
        name: path.file_stem().map(|s| s.to_string_lossy().into_owned()).unwrap_or_default(),
        size: meta.as_ref().map(|m| m.len()).unwrap_or(0),
        modified: meta.and_then(|m| m.modified().ok()).unwrap_or(SystemTime::UNIX_EPOCH),
        error: None,
        format: 0,
        tick: 0,
        kind: String::new(),
        campaign: String::new(),
        title: String::new(),
        faction: String::new(),
        difficulty: None,
        week: 0,
        game_seconds: 0,
        saved_at: None,
    };
    if let Err(e) = parse_into(path, &mut info) {
        info.error = Some(e);
    }
    info
}

fn parse_into(path: &Path, info: &mut SaveInfo) -> Result<(), String> {
    let mut f = std::fs::File::open(path).map_err(|e| e.to_string())?;
    let mut head = [0u8; 20];
    f.read_exact(&mut head).map_err(|_| "File is too short to be a save.".to_owned())?;
    let u32at = |o: usize| u32::from_le_bytes(head[o..o + 4].try_into().unwrap());
    if u32at(0) != MAGIC {
        return Err("Not an ALSV save (wrong signature).".into());
    }
    info.format = u32at(4);
    info.tick = u64::from_le_bytes(head[8..16].try_into().unwrap());
    let len = u32at(16) as usize;
    if len > 1 << 20 {
        return Err("Save header is damaged (description too long).".into());
    }
    let mut desc = vec![0u8; len];
    f.read_exact(&mut desc).map_err(|_| "Save header is cut short.".to_owned())?;
    let desc = String::from_utf8_lossy(&desc);
    let mut in_fields = false;
    for line in desc.lines() {
        if !in_fields {
            in_fields = line.trim().starts_with("alamo_game_save");
            continue;
        }
        let Some((k, v)) = line.split_once('=') else { continue };
        let v = v.trim();
        match k.trim() {
            "name" => {
                if !v.is_empty() {
                    info.name = v.to_owned()
                }
            }
            "kind" => info.kind = v.to_owned(),
            "campaign" => info.campaign = v.to_owned(),
            "title" => info.title = v.to_owned(),
            "faction" => info.faction = v.to_owned(),
            "difficulty" => info.difficulty = v.parse().ok(),
            "week" => info.week = v.parse().unwrap_or(0),
            "game_seconds" => info.game_seconds = v.parse().unwrap_or(0),
            "saved_at" => info.saved_at = v.parse().ok(),
            _ => {}
        }
    }
    if !in_fields {
        return Err("Save has no readable description.".into());
    }
    Ok(())
}

/// Every `.alsv` in the folder, newest first.
pub fn list(dir: &Path) -> Vec<SaveInfo> {
    let mut out: Vec<SaveInfo> = std::fs::read_dir(dir)
        .into_iter()
        .flatten()
        .flatten()
        .map(|e| e.path())
        .filter(|p| p.is_file() && p.extension().map(|e| e.eq_ignore_ascii_case("alsv")).unwrap_or(false))
        .map(|p| read_header(&p))
        .collect();
    out.sort_by(|a, b| b.when().cmp(&a.when()).then_with(|| a.name.to_lowercase().cmp(&b.name.to_lowercase())));
    out
}

// ------------------------------------------------------------------ notes, tags, search

/// The order the saves list is shown in.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum SaveSort {
    #[default]
    Newest,
    Oldest,
    Name,
    Week,
}

impl SaveSort {
    pub const ALL: [SaveSort; 4] = [SaveSort::Newest, SaveSort::Oldest, SaveSort::Name, SaveSort::Week];

    pub fn label(self) -> &'static str {
        match self {
            SaveSort::Newest => "Newest",
            SaveSort::Oldest => "Oldest",
            SaveSort::Name => "Name",
            SaveSort::Week => "Week",
        }
    }
}

/// Sorts saves for display. Ties fall back to newest first, then name, so the order is stable.
pub fn sort_saves(saves: &mut [&SaveInfo], sort: SaveSort) {
    let name = |s: &SaveInfo| s.name.to_lowercase();
    saves.sort_by(|a, b| {
        let newest = b.when().cmp(&a.when()).then_with(|| name(a).cmp(&name(b)));
        match sort {
            SaveSort::Newest => newest,
            SaveSort::Oldest => a.when().cmp(&b.when()).then_with(|| name(a).cmp(&name(b))),
            SaveSort::Name => name(a).cmp(&name(b)).then(newest),
            // Latest week first; skirmish saves (no week) go last.
            SaveSort::Week => b.is_campaign().cmp(&a.is_campaign()).then(b.week.cmp(&a.week)).then(newest),
        }
    });
}

/// Everything about a save the search box looks at, lower-cased.
pub fn search_text(s: &SaveInfo, note: Option<&SaveNote>) -> String {
    let mut text = format!("{}\n{}\n{}\n{}", s.name, s.display_title(), s.campaign.replace('_', " "), s.faction.replace('_', " "));
    if let Some(n) = note {
        text.push('\n');
        text.push_str(&n.note);
        for t in &n.tags {
            text.push('\n');
            text.push_str(t);
        }
    }
    text.to_lowercase()
}

/// True when every word of the query appears somewhere in the (lower-cased) text.
pub fn matches_query(text_lower: &str, query: &str) -> bool {
    query.split_whitespace().all(|w| text_lower.contains(&w.to_lowercase()))
}

/// Tidies a tag as typed: trimmed, lower-cased, inner whitespace collapsed, commas dropped.
/// `None` when nothing is left.
pub fn normalize_tag(raw: &str) -> Option<String> {
    let tag = raw.replace(',', " ").split_whitespace().collect::<Vec<_>>().join(" ").to_lowercase();
    (!tag.is_empty()).then_some(tag)
}

/// Drops notes for saves that are gone. Only entries for files directly inside `dir` are
/// considered, and one is removed only when `exists` says its file isn't there, so notes
/// for other folders (other profiles) are never touched. Returns how many were removed.
pub fn prune_notes(notes: &mut BTreeMap<String, SaveNote>, dir: &Path, exists: impl Fn(&str) -> bool) -> usize {
    let prefix = format!("{}\\", config::mod_key(dir));
    let gone: Vec<String> = notes
        .keys()
        .filter(|k| k.strip_prefix(&prefix).map(|rest| !rest.is_empty() && !rest.contains('\\')).unwrap_or(false))
        .filter(|k| !exists(k))
        .cloned()
        .collect();
    for k in &gone {
        notes.remove(k);
    }
    gone.len()
}

// ------------------------------------------------------------------ checkpoints

#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct CheckpointFile {
    /// Path relative to the save folder, with `/` separators.
    pub name: String,
    pub size: u64,
    pub sha256: String,
}

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct Manifest {
    pub label: String,
    /// Unix seconds.
    pub created: i64,
    /// Taken automatically before a restore.
    #[serde(default)]
    pub safety: bool,
    /// Taken automatically at launch or on a timer; only these are pruned.
    #[serde(default)]
    pub auto: bool,
    pub source: PathBuf,
    pub files: Vec<CheckpointFile>,
}

#[derive(Clone, Debug)]
pub struct Checkpoint {
    pub dir: PathBuf,
    pub manifest: Manifest,
    /// Set by the `*_mirrored` functions when the checkpoint itself was made but its copy
    /// in the backup folder was not. The checkpoint is fine; show this as a warning.
    pub backup_warning: Option<String>,
}

impl Checkpoint {
    pub fn total_size(&self) -> u64 {
        self.manifest.files.iter().map(|f| f.size).sum()
    }
    pub fn save_count(&self) -> usize {
        self.manifest.files.iter().filter(|f| f.name.to_ascii_lowercase().ends_with(".alsv")).count()
    }
}

/// Checkpoints are grouped per save folder so profiles with their own saves don't mix.
pub fn checkpoint_root(saves_dir: &Path) -> PathBuf {
    let key = config::mod_key(saves_dir);
    let hash = Sha256::digest(key.as_bytes());
    let short: String = hash.iter().take(6).map(|b| format!("{b:02x}")).collect();
    config::data_dir().join("checkpoints").join(short)
}

pub fn sha256_file(path: &Path) -> Result<String, String> {
    let mut f = std::fs::File::open(path).map_err(|e| format!("{}: {e}", path.display()))?;
    let mut h = Sha256::new();
    let mut buf = vec![0u8; 1 << 16];
    loop {
        let n = f.read(&mut buf).map_err(|e| format!("{}: {e}", path.display()))?;
        if n == 0 {
            break;
        }
        h.update(&buf[..n]);
    }
    Ok(h.finalize().iter().map(|b| format!("{b:02x}")).collect())
}

fn walk(root: &Path, dir: &Path, out: &mut Vec<String>) {
    let Ok(rd) = std::fs::read_dir(dir) else { return };
    for e in rd.flatten() {
        let p = e.path();
        if p.is_dir() {
            walk(root, &p, out);
        } else if p.extension().map(|x| x != "tmp").unwrap_or(true) {
            if let Ok(rel) = p.strip_prefix(root) {
                out.push(rel.to_string_lossy().replace('\\', "/"));
            }
        }
    }
}

pub fn create_checkpoint(saves_dir: &Path, label: &str, safety: bool) -> Result<Checkpoint, String> {
    create_checkpoint_in(&checkpoint_root(saves_dir), saves_dir, label, safety)
}

/// [`create_checkpoint`], then a copy into the backup folder when one is set. A backup
/// that fails never fails the checkpoint: it comes back in `backup_warning`.
pub fn create_checkpoint_mirrored(
    saves_dir: &Path,
    label: &str,
    safety: bool,
    backup_dir: Option<&Path>,
) -> Result<Checkpoint, String> {
    let mut cp = create_checkpoint(saves_dir, label, safety)?;
    if let Some(backup) = backup_dir {
        cp.backup_warning = mirror_checkpoint(&cp, backup).err();
    }
    Ok(cp)
}

/// [`create_checkpoint`] with the folder the checkpoints live in given explicitly.
fn create_checkpoint_in(root: &Path, saves_dir: &Path, label: &str, safety: bool) -> Result<Checkpoint, String> {
    let mut names = Vec::new();
    walk(saves_dir, saves_dir, &mut names);
    if names.is_empty() {
        return Err("There are no saves in this folder to checkpoint.".into());
    }
    names.sort();
    let now = chrono::Local::now();
    let mut dir = root.join(now.format("%Y%m%d-%H%M%S").to_string());
    let mut n = 1;
    while dir.exists() {
        n += 1;
        dir = root.join(format!("{}-{n}", now.format("%Y%m%d-%H%M%S")));
    }
    let files_dir = dir.join("files");
    std::fs::create_dir_all(&files_dir).map_err(|e| format!("Couldn't create {}: {e}", dir.display()))?;

    let result = (|| {
        let mut files = Vec::new();
        for name in &names {
            let src = saves_dir.join(name);
            let dst = files_dir.join(name);
            if let Some(parent) = dst.parent() {
                std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
            }
            let before = sha256_file(&src)?;
            let size = std::fs::copy(&src, &dst).map_err(|e| format!("Couldn't copy {name}: {e}"))?;
            // Verify the copy, not just the source.
            let after = sha256_file(&dst)?;
            if before != after {
                return Err(format!("{name} changed while it was being copied; nothing was kept. Try again."));
            }
            files.push(CheckpointFile { name: name.clone(), size, sha256: after });
        }
        let manifest = Manifest {
            label: label.trim().to_owned(),
            created: now.timestamp(),
            safety,
            auto: false,
            source: saves_dir.to_path_buf(),
            files,
        };
        let text = serde_json::to_string_pretty(&manifest).map_err(|e| e.to_string())?;
        std::fs::write(dir.join("manifest.json"), text).map_err(|e| e.to_string())?;
        Ok(manifest)
    })();
    match result {
        Ok(manifest) => Ok(Checkpoint { dir, manifest, backup_warning: None }),
        Err(e) => {
            let _ = std::fs::remove_dir_all(&dir);
            Err(e)
        }
    }
}

/// Takes an automatic checkpoint and keeps only the newest `keep` automatic ones.
/// Returns `Ok(None)` when the folder has nothing to checkpoint. Manual and safety
/// checkpoints are never pruned.
pub fn auto_checkpoint(saves_dir: &Path, label: &str, keep: u32) -> Result<Option<Checkpoint>, String> {
    auto_checkpoint_in(&checkpoint_root(saves_dir), saves_dir, label, keep, None)
}

/// [`auto_checkpoint`] that also copies the new checkpoint into the backup folder (when one
/// is set) and removes the backup copies of the automatic checkpoints it prunes.
///
/// A backup that fails never fails the checkpoint: the checkpoint is returned with
/// `backup_warning` set, for the caller to show.
#[allow(dead_code)] // called by the launch hook in app.rs
pub fn auto_checkpoint_mirrored(
    saves_dir: &Path,
    label: &str,
    keep: u32,
    backup_dir: Option<&Path>,
) -> Result<Option<Checkpoint>, String> {
    auto_checkpoint_in(&checkpoint_root(saves_dir), saves_dir, label, keep, backup_dir)
}

fn auto_checkpoint_in(
    root: &Path,
    saves_dir: &Path,
    label: &str,
    keep: u32,
    backup_dir: Option<&Path>,
) -> Result<Option<Checkpoint>, String> {
    let mut names = Vec::new();
    walk(saves_dir, saves_dir, &mut names);
    if names.is_empty() {
        return Ok(None);
    }
    let mut cp = create_checkpoint_in(root, saves_dir, label, false)?;
    cp.manifest.auto = true;
    let text = serde_json::to_string_pretty(&cp.manifest).map_err(|e| e.to_string())?;
    std::fs::write(cp.dir.join("manifest.json"), text).map_err(|e| e.to_string())?;
    let mut warnings = Vec::new();
    if let Some(backup) = backup_dir {
        if let Err(e) = mirror_checkpoint(&cp, backup) {
            warnings.push(e);
        }
    }
    // The one just made always stays, whatever the listing order makes of two checkpoints
    // taken within the same second.
    let older: Vec<Checkpoint> = list_checkpoints_in(root)
        .into_iter()
        .filter(|c| c.manifest.auto && !same_path(&c.dir, &cp.dir))
        .collect();
    for old in older.iter().skip(keep.max(1) as usize - 1) {
        let _ = std::fs::remove_dir_all(&old.dir);
        // The backup copy goes with it, but only one that is itself marked automatic.
        let mirror = backup_dir.and_then(|b| mirror_dir(&old.dir, b)).filter(|m| !same_path(m, &old.dir));
        if let Some(m) = mirror.filter(|m| read_manifest(m).map(|x| x.auto).unwrap_or(false)) {
            if let Err(e) = std::fs::remove_dir_all(&m) {
                warnings.push(format!("Couldn't remove the old backup copy {}: {e}", m.display()));
            }
        }
    }
    if !warnings.is_empty() {
        cp.backup_warning = Some(warnings.join(" "));
    }
    Ok(Some(cp))
}

fn read_manifest(checkpoint_dir: &Path) -> Option<Manifest> {
    let text = std::fs::read_to_string(checkpoint_dir.join("manifest.json")).ok()?;
    serde_json::from_str(text.trim_start_matches('\u{feff}')).ok()
}

pub fn list_checkpoints(saves_dir: &Path) -> Vec<Checkpoint> {
    list_checkpoints_in(&checkpoint_root(saves_dir))
}

/// Every checkpoint folder directly inside `root`, newest first.
fn list_checkpoints_in(root: &Path) -> Vec<Checkpoint> {
    let mut out: Vec<Checkpoint> = std::fs::read_dir(root)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| {
            let dir = e.path();
            let manifest = read_manifest(&dir)?;
            Some(Checkpoint { dir, manifest, backup_warning: None })
        })
        .collect();
    out.sort_by(|a, b| b.manifest.created.cmp(&a.manifest.created).then_with(|| b.dir.cmp(&a.dir)));
    out
}

/// Re-hashes every file in the checkpoint against its manifest.
pub fn verify_checkpoint(cp: &Checkpoint) -> Result<(), String> {
    for f in &cp.manifest.files {
        let p = cp.dir.join("files").join(&f.name);
        if !p.is_file() {
            return Err(format!("{} is missing from the checkpoint.", f.name));
        }
        if sha256_file(&p)? != f.sha256 {
            return Err(format!("{} no longer matches its checksum; the checkpoint is damaged.", f.name));
        }
    }
    Ok(())
}

/// Verifies the checkpoint, takes a safety checkpoint of what's there now, then replaces
/// the save folder's contents. Returns the safety checkpoint if one was taken.
#[allow(dead_code)] // the page uses the mirrored form
pub fn restore_checkpoint(cp: &Checkpoint, saves_dir: &Path) -> Result<Option<Checkpoint>, String> {
    restore_checkpoint_in(&checkpoint_root(saves_dir), cp, saves_dir, None)
}

/// [`restore_checkpoint`] that also copies the safety checkpoint into the backup folder.
/// A failed backup is reported in the safety checkpoint's `backup_warning`.
pub fn restore_checkpoint_mirrored(
    cp: &Checkpoint,
    saves_dir: &Path,
    backup_dir: Option<&Path>,
) -> Result<Option<Checkpoint>, String> {
    restore_checkpoint_in(&checkpoint_root(saves_dir), cp, saves_dir, backup_dir)
}

fn restore_checkpoint_in(
    root: &Path,
    cp: &Checkpoint,
    saves_dir: &Path,
    backup_dir: Option<&Path>,
) -> Result<Option<Checkpoint>, String> {
    verify_checkpoint(cp)?;
    let mut current = Vec::new();
    walk(saves_dir, saves_dir, &mut current);
    let safety = if current.is_empty() {
        None
    } else {
        let label = format!("Before restoring \"{}\"", display_label(cp));
        let mut safety = create_checkpoint_in(root, saves_dir, &label, true)?;
        if let Some(backup) = backup_dir {
            safety.backup_warning = mirror_checkpoint(&safety, backup).err();
        }
        Some(safety)
    };
    for name in &current {
        std::fs::remove_file(saves_dir.join(name)).map_err(|e| format!("Couldn't remove {name}: {e}"))?;
    }
    std::fs::create_dir_all(saves_dir).map_err(|e| e.to_string())?;
    for f in &cp.manifest.files {
        let dst = saves_dir.join(&f.name);
        if let Some(parent) = dst.parent() {
            std::fs::create_dir_all(parent).map_err(|e| e.to_string())?;
        }
        std::fs::copy(cp.dir.join("files").join(&f.name), &dst)
            .map_err(|e| format!("Couldn't restore {}: {e}", f.name))?;
        if sha256_file(&dst)? != f.sha256 {
            return Err(format!("{} didn't copy back correctly. Your previous saves are in the safety checkpoint.", f.name));
        }
    }
    Ok(safety)
}

pub fn display_label(cp: &Checkpoint) -> String {
    if cp.manifest.label.is_empty() {
        "Checkpoint".into()
    } else {
        cp.manifest.label.clone()
    }
}

pub fn to_recycle_bin(path: &Path) -> Result<(), String> {
    trash::delete(path).map_err(|e| format!("Couldn't move {} to the Recycle Bin: {e}", path.display()))
}

// ------------------------------------------------------------------ backup folder
//
// A backup folder holds copies of checkpoints in the same layout the launcher uses itself:
// `<backup>\<save-folder key>\<checkpoint folder>\{manifest.json, files\...}`. Every file is
// checked against the manifest's SHA-256 after it is copied, in both directions.

/// Where a checkpoint's copy lives inside a backup folder.
pub fn mirror_dir(checkpoint_dir: &Path, backup_dir: &Path) -> Option<PathBuf> {
    let name = checkpoint_dir.file_name()?;
    let key = checkpoint_dir.parent()?.file_name()?;
    Some(backup_dir.join(key).join(name))
}

/// A manifest's file name as a relative path, refusing anything that would land outside
/// the checkpoint (manifests in a backup folder are outside data).
fn safe_relative(name: &str) -> Result<PathBuf, String> {
    let rel = Path::new(name);
    let plain = rel.components().all(|c| matches!(c, std::path::Component::Normal(_)));
    if name.is_empty() || name.contains(':') || !plain {
        return Err(format!("The manifest lists a file with an unsafe name ({name})."));
    }
    Ok(rel.to_path_buf())
}

/// Copies a checkpoint folder, checking every file against the manifest once it has
/// landed. Files already there and intact are left alone. The manifest is written last,
/// so a copy that was interrupted is never mistaken for a complete one.
fn copy_verified(src_dir: &Path, manifest: &Manifest, dst_dir: &Path) -> Result<(), String> {
    let (src_files, dst_files) = (src_dir.join("files"), dst_dir.join("files"));
    std::fs::create_dir_all(&dst_files).map_err(|e| format!("Couldn't create {}: {e}", dst_dir.display()))?;
    for f in &manifest.files {
        let rel = safe_relative(&f.name)?;
        let (src, dst) = (src_files.join(&rel), dst_files.join(&rel));
        let intact = std::fs::metadata(&dst).map(|m| m.is_file() && m.len() == f.size).unwrap_or(false)
            && sha256_file(&dst).map(|h| h == f.sha256).unwrap_or(false);
        if intact {
            continue;
        }
        if !src.is_file() {
            return Err(format!("{} is missing from {}.", f.name, src_dir.display()));
        }
        if let Some(parent) = dst.parent() {
            std::fs::create_dir_all(parent).map_err(|e| format!("Couldn't create {}: {e}", parent.display()))?;
        }
        // Copy next to the target and rename, so a half-written file never has the real name.
        let mut part = dst.clone().into_os_string();
        part.push(".part");
        let part = PathBuf::from(part);
        std::fs::copy(&src, &part).map_err(|e| format!("Couldn't copy {} to {}: {e}", f.name, dst_dir.display()))?;
        match sha256_file(&part) {
            Ok(h) if h == f.sha256 => {}
            other => {
                let _ = std::fs::remove_file(&part);
                return Err(match other {
                    Err(e) => e,
                    Ok(_) => format!("{} doesn't match its checksum after copying to {}.", f.name, dst_dir.display()),
                });
            }
        }
        std::fs::rename(&part, &dst).map_err(|e| format!("Couldn't finish copying {}: {e}", f.name))?;
    }
    let text = serde_json::to_string_pretty(manifest).map_err(|e| e.to_string())?;
    let tmp = dst_dir.join("manifest.json.part");
    std::fs::write(&tmp, text).map_err(|e| format!("Couldn't write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, dst_dir.join("manifest.json")).map_err(|e| format!("Couldn't write the manifest in {}: {e}", dst_dir.display()))
}

/// Copies a checkpoint into the backup folder and verifies every file there against the
/// manifest. Safe to repeat: files that are already there and intact aren't copied again.
/// The local checkpoint is never changed, whatever happens.
pub fn mirror_checkpoint(cp: &Checkpoint, backup_dir: &Path) -> Result<(), String> {
    let label = display_label(cp);
    if !backup_dir.is_dir() {
        return Err(format!(
            "\"{label}\" wasn't copied to the backup folder: {} isn't available.",
            backup_dir.display()
        ));
    }
    let dst = mirror_dir(&cp.dir, backup_dir)
        .ok_or_else(|| format!("\"{label}\" wasn't copied to the backup folder: its own folder has no name."))?;
    if same_path(&dst, &cp.dir) {
        return Err("The backup folder is the launcher's own checkpoints folder, so there is nothing to copy. Choose another one.".into());
    }
    let existed = dst.exists();
    copy_verified(&cp.dir, &cp.manifest, &dst).map_err(|e| {
        if !existed {
            let _ = std::fs::remove_dir_all(&dst);
        }
        format!("\"{label}\" wasn't copied to the backup folder: {e}")
    })
}

/// [`mirror_checkpoint`] for a whole list. Returns how many were copied and what failed.
pub fn mirror_all(checkpoints: &[Checkpoint], backup_dir: &Path) -> (usize, Vec<String>) {
    let mut done = 0;
    let mut failed = Vec::new();
    for cp in checkpoints {
        match mirror_checkpoint(cp, backup_dir) {
            Ok(()) => done += 1,
            Err(e) => failed.push(e),
        }
    }
    (done, failed)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum BackupStatus {
    /// The backup folder has this checkpoint. `verified`: every file was re-hashed.
    BackedUp { verified: bool },
    Missing,
    /// There is a copy but it isn't the same; the text says how.
    Differs(String),
}

/// Compares a checkpoint with its copy in the backup folder. The quick form compares the
/// manifests and file sizes; `deep` also re-hashes every file of the copy.
pub fn backup_status(cp: &Checkpoint, backup_dir: &Path, deep: bool) -> BackupStatus {
    let Some(dir) = mirror_dir(&cp.dir, backup_dir) else { return BackupStatus::Missing };
    if !dir.is_dir() {
        return BackupStatus::Missing;
    }
    let Some(theirs) = read_manifest(&dir) else {
        return BackupStatus::Differs("The backup copy has no readable manifest (it may be incomplete).".into());
    };
    let ours = &cp.manifest;
    if theirs.files != ours.files || theirs.created != ours.created || theirs.label != ours.label {
        return BackupStatus::Differs("The backup copy lists different files.".into());
    }
    for f in &ours.files {
        let Ok(rel) = safe_relative(&f.name) else {
            return BackupStatus::Differs(format!("{} has an unsafe name.", f.name));
        };
        let p = dir.join("files").join(rel);
        match std::fs::metadata(&p) {
            Ok(m) if m.is_file() && m.len() == f.size => {}
            Ok(_) => return BackupStatus::Differs(format!("{} has a different size in the backup.", f.name)),
            Err(_) => return BackupStatus::Differs(format!("{} is missing from the backup.", f.name)),
        }
        if deep {
            match sha256_file(&p) {
                Ok(h) if h == f.sha256 => {}
                Ok(_) => return BackupStatus::Differs(format!("{} no longer matches its checksum in the backup.", f.name)),
                Err(e) => return BackupStatus::Differs(format!("Couldn't read the backup: {e}")),
            }
        }
    }
    BackupStatus::BackedUp { verified: deep }
}

/// Whether the backup folder has anything for this checkpoint (cheap; nothing is read).
pub fn has_mirror(cp: &Checkpoint, backup_dir: &Path) -> bool {
    mirror_dir(&cp.dir, backup_dir).map(|d| d.is_dir() && !same_path(&d, &cp.dir)).unwrap_or(false)
}

/// Removes a checkpoint's copy from the backup folder: to the Recycle Bin where the
/// folder has one, otherwise for good. `Ok(false)` when there was no copy.
pub fn delete_mirror(cp: &Checkpoint, backup_dir: &Path) -> Result<bool, String> {
    delete_mirror_with(cp, backup_dir, true)
}

fn delete_mirror_with(cp: &Checkpoint, backup_dir: &Path, recycle: bool) -> Result<bool, String> {
    let Some(dir) = mirror_dir(&cp.dir, backup_dir).filter(|d| d.is_dir() && !same_path(d, &cp.dir)) else {
        return Ok(false);
    };
    if recycle && trash::delete(&dir).is_ok() {
        return Ok(true);
    }
    // Network and some cloud folders have no Recycle Bin.
    std::fs::remove_dir_all(&dir).map_err(|e| format!("Couldn't delete the backup copy {}: {e}", dir.display()))?;
    Ok(true)
}

/// A checkpoint that is in the backup folder but not on this computer.
#[derive(Clone, Debug)]
pub struct BackupOnly {
    /// `dir` points into the backup folder.
    pub checkpoint: Checkpoint,
    /// False when it was taken from a different save folder than the one being shown
    /// (another profile, or another computer or user name).
    pub same_folder: bool,
}

/// Checkpoints that exist only in the backup folder (after a disk loss, say).
pub fn list_backup_only(saves_dir: &Path, backup_dir: &Path) -> Vec<BackupOnly> {
    list_backup_only_in(&checkpoint_root(saves_dir), backup_dir)
}

/// Lists the backup's checkpoints for this save folder that aren't in `local_root`, plus
/// those of save folders this computer keeps no checkpoints for at all: after a disk loss
/// or on a new computer the folder keys needn't match, and they should still be offered.
fn list_backup_only_in(local_root: &Path, backup_dir: &Path) -> Vec<BackupOnly> {
    let key = local_root.file_name().map(|k| k.to_os_string());
    let base = local_root.parent().map(Path::to_path_buf);
    let mut out = Vec::new();
    for group in std::fs::read_dir(backup_dir).into_iter().flatten().flatten() {
        let group_dir = group.path();
        if !group_dir.is_dir() || same_path(&group_dir, local_root) {
            continue;
        }
        let same_folder = Some(group.file_name()) == key;
        // Another save folder's checkpoints that are still on this computer belong to
        // that folder's own list, not here.
        if !same_folder && base.as_ref().map(|b| b.join(group.file_name()).is_dir()).unwrap_or(false) {
            continue;
        }
        for cp in list_checkpoints_in(&group_dir) {
            let here = cp.dir.file_name().map(|n| local_root.join(n).join("manifest.json").is_file()).unwrap_or(true);
            if !here {
                out.push(BackupOnly { checkpoint: cp, same_folder });
            }
        }
    }
    out.sort_by(|a, b| {
        b.checkpoint.manifest.created.cmp(&a.checkpoint.manifest.created).then_with(|| b.checkpoint.dir.cmp(&a.checkpoint.dir))
    });
    out
}

/// Copies a checkpoint from the backup folder back onto this computer, verified file by
/// file, so it shows up in the list and can be restored like any other.
pub fn import_checkpoint(from_backup: &Checkpoint, saves_dir: &Path) -> Result<Checkpoint, String> {
    import_checkpoint_into(&checkpoint_root(saves_dir), from_backup)
}

fn import_checkpoint_into(local_root: &Path, from_backup: &Checkpoint) -> Result<Checkpoint, String> {
    let label = display_label(from_backup);
    let name = from_backup
        .dir
        .file_name()
        .ok_or_else(|| format!("\"{label}\" couldn't be imported: its folder has no name."))?;
    let dst = local_root.join(name);
    if same_path(&dst, &from_backup.dir) {
        return Err(format!("\"{label}\" is already on this computer."));
    }
    if dst.join("manifest.json").is_file() {
        return Err(format!("\"{label}\" is already on this computer."));
    }
    let existed = dst.exists();
    copy_verified(&from_backup.dir, &from_backup.manifest, &dst).map_err(|e| {
        if !existed {
            let _ = std::fs::remove_dir_all(&dst);
        }
        format!("\"{label}\" couldn't be imported: {e}")
    })?;
    Ok(Checkpoint { dir: dst, manifest: from_backup.manifest.clone(), backup_warning: None })
}

#[cfg(test)]
mod tests {
    use super::*;

    /// A fresh folder under the system temp dir, removed when dropped.
    struct Scratch(PathBuf);

    impl Scratch {
        fn new(tag: &str) -> Self {
            static SEQ: std::sync::atomic::AtomicU32 = std::sync::atomic::AtomicU32::new(0);
            let n = SEQ.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
            let dir = std::env::temp_dir().join(format!("cc-saves-test-{}-{tag}-{n}", std::process::id()));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            Scratch(dir)
        }
        fn dir(&self, name: &str) -> PathBuf {
            let d = self.0.join(name);
            std::fs::create_dir_all(&d).unwrap();
            d
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn write(path: &Path, bytes: &[u8]) {
        if let Some(p) = path.parent() {
            std::fs::create_dir_all(p).unwrap();
        }
        std::fs::write(path, bytes).unwrap();
    }

    /// A save folder with two saves (one in a subfolder) and a checkpoint root next to it.
    fn fixture(s: &Scratch) -> (PathBuf, PathBuf, PathBuf) {
        let saves = s.dir("Saves");
        write(&saves.join("one.alsv"), b"first save");
        write(&saves.join("sub/two.alsv"), b"second save, a little longer");
        let root = s.dir("data/checkpoints/abc123");
        let backup = s.dir("backup");
        (saves, root, backup)
    }

    fn save(name: &str, kind: &str, week: u32, saved_at: i64) -> SaveInfo {
        SaveInfo {
            path: PathBuf::from(format!("C:\\Saves\\{name}.alsv")),
            name: name.to_owned(),
            size: 1,
            modified: SystemTime::UNIX_EPOCH,
            error: None,
            format: 1,
            tick: 0,
            kind: kind.to_owned(),
            campaign: "Sandbox_Equal_Footing".to_owned(),
            title: "Equal Footing".to_owned(),
            faction: "Rebel".to_owned(),
            difficulty: Some(1),
            week,
            game_seconds: 0,
            saved_at: Some(saved_at),
        }
    }

    #[test]
    fn mirror_copies_and_verifies() {
        let s = Scratch::new("mirror");
        let (saves, root, backup) = fixture(&s);
        let cp = create_checkpoint_in(&root, &saves, "Before Kuat", false).unwrap();
        assert_eq!(backup_status(&cp, &backup, true), BackupStatus::Missing);
        assert!(!has_mirror(&cp, &backup));

        mirror_checkpoint(&cp, &backup).unwrap();
        let dst = mirror_dir(&cp.dir, &backup).unwrap();
        assert_eq!(dst, backup.join("abc123").join(cp.dir.file_name().unwrap()));
        assert_eq!(std::fs::read(dst.join("files/one.alsv")).unwrap(), b"first save");
        assert_eq!(std::fs::read(dst.join("files/sub/two.alsv")).unwrap(), b"second save, a little longer");
        assert!(has_mirror(&cp, &backup));
        assert_eq!(backup_status(&cp, &backup, false), BackupStatus::BackedUp { verified: false });
        assert_eq!(backup_status(&cp, &backup, true), BackupStatus::BackedUp { verified: true });
        // The copy is a checkpoint in its own right.
        let copy = &list_checkpoints_in(&backup.join("abc123"))[0];
        assert_eq!(copy.manifest.label, "Before Kuat");
        verify_checkpoint(copy).unwrap();
        // Doing it again is harmless.
        mirror_checkpoint(&cp, &backup).unwrap();
        assert_eq!(mirror_all(&[cp.clone()], &backup), (1, Vec::new()));
        // No temporary files are left behind.
        let mut names = Vec::new();
        walk(&dst, &dst, &mut names);
        names.sort();
        assert_eq!(names, vec!["files/one.alsv", "files/sub/two.alsv", "manifest.json"]);
    }

    #[test]
    fn a_damaged_backup_is_noticed_and_repaired() {
        let s = Scratch::new("differs");
        let (saves, root, backup) = fixture(&s);
        let cp = create_checkpoint_in(&root, &saves, "", false).unwrap();
        mirror_checkpoint(&cp, &backup).unwrap();
        let dst = mirror_dir(&cp.dir, &backup).unwrap();

        // Same size, different bytes: only the deep check can tell.
        write(&dst.join("files/one.alsv"), b"FIRST SAVE");
        assert_eq!(backup_status(&cp, &backup, false), BackupStatus::BackedUp { verified: false });
        assert!(matches!(backup_status(&cp, &backup, true), BackupStatus::Differs(m) if m.contains("one.alsv")));
        // A different size shows up in the quick check already.
        write(&dst.join("files/one.alsv"), b"short");
        assert!(matches!(backup_status(&cp, &backup, false), BackupStatus::Differs(_)));
        std::fs::remove_file(dst.join("files/sub/two.alsv")).unwrap();
        assert!(matches!(backup_status(&cp, &backup, false), BackupStatus::Differs(_)));

        // Backing up again puts it right.
        mirror_checkpoint(&cp, &backup).unwrap();
        assert_eq!(backup_status(&cp, &backup, true), BackupStatus::BackedUp { verified: true });

        // A copy without its manifest isn't a backup.
        std::fs::remove_file(dst.join("manifest.json")).unwrap();
        assert!(matches!(backup_status(&cp, &backup, false), BackupStatus::Differs(_)));
    }

    #[test]
    fn a_failed_mirror_leaves_the_checkpoint_alone() {
        let s = Scratch::new("fail");
        let (saves, root, backup) = fixture(&s);
        let cp = create_checkpoint_in(&root, &saves, "Mine", false).unwrap();

        // The backup folder isn't there (unplugged disk).
        let gone = s.0.join("not-there");
        let e = mirror_checkpoint(&cp, &gone).unwrap_err();
        assert!(e.contains("Mine") && e.contains("isn't available"), "{e}");
        assert!(!gone.exists(), "a missing backup folder must not be created");
        verify_checkpoint(&cp).unwrap();

        // The local checkpoint is damaged: the copy fails its checksum and nothing is kept.
        write(&cp.dir.join("files/one.alsv"), b"first sAve");
        let e = mirror_checkpoint(&cp, &backup).unwrap_err();
        assert!(e.contains("one.alsv") && e.contains("checksum"), "{e}");
        assert!(!mirror_dir(&cp.dir, &backup).unwrap().exists());
        assert_eq!(backup_status(&cp, &backup, false), BackupStatus::Missing);
        assert_eq!(std::fs::read(cp.dir.join("files/one.alsv")).unwrap(), b"first sAve");
        assert_eq!(mirror_all(&[cp.clone()], &backup).0, 0);

        // Pointing the backup at the launcher's own checkpoints folder is refused.
        let own = root.parent().unwrap();
        assert!(mirror_checkpoint(&cp, own).is_err());
        assert!(!has_mirror(&cp, own));
    }

    #[test]
    fn auto_checkpoints_are_mirrored_and_pruned_with_their_copies() {
        let s = Scratch::new("auto");
        let (saves, root, backup) = fixture(&s);
        let manual = create_checkpoint_in(&root, &saves, "Manual", false).unwrap();
        mirror_checkpoint(&manual, &backup).unwrap();

        let mut made = Vec::new();
        for i in 0..4 {
            write(&saves.join("one.alsv"), format!("first save v{i}").as_bytes());
            let cp = auto_checkpoint_in(&root, &saves, &format!("Auto {i}"), 2, Some(&backup)).unwrap().unwrap();
            assert!(cp.manifest.auto);
            assert_eq!(cp.backup_warning, None);
            assert_eq!(backup_status(&cp, &backup, true), BackupStatus::BackedUp { verified: true });
            made.push(cp);
        }
        let autos: Vec<Checkpoint> = list_checkpoints_in(&root).into_iter().filter(|c| c.manifest.auto).collect();
        assert_eq!(autos.len(), 2);
        // The pruned ones are gone from the backup too; the kept ones and the manual one stay.
        // (Folder names are reused within one second, so go by label, not by path.)
        let mut labels: Vec<String> = autos.iter().map(|c| c.manifest.label.clone()).collect();
        labels.sort();
        assert_eq!(labels, ["Auto 2", "Auto 3"]);
        let mut copies: Vec<String> =
            list_checkpoints_in(&backup.join("abc123")).into_iter().map(|c| c.manifest.label).collect();
        copies.sort();
        assert_eq!(copies, ["Auto 2", "Auto 3", "Manual"]);
        for cp in &autos {
            assert_eq!(backup_status(cp, &backup, true), BackupStatus::BackedUp { verified: true });
        }
        assert!(manual.dir.exists());
        assert_eq!(backup_status(&manual, &backup, true), BackupStatus::BackedUp { verified: true });

        // A backup folder that has gone away is a warning, never a failed checkpoint.
        let gone = s.0.join("unplugged");
        let cp = auto_checkpoint_in(&root, &saves, "Offline", 2, Some(&gone)).unwrap().unwrap();
        assert!(cp.backup_warning.as_deref().unwrap_or("").contains("isn't available"));
        verify_checkpoint(&cp).unwrap();
        // Without a backup folder nothing is mirrored and nothing is warned about.
        let cp = auto_checkpoint_in(&root, &saves, "Plain", 2, None).unwrap().unwrap();
        assert_eq!(cp.backup_warning, None);
        // An empty save folder gives no checkpoint.
        let empty = s.dir("EmptySaves");
        assert!(auto_checkpoint_in(&root, &empty, "x", 2, Some(&backup)).unwrap().is_none());
    }

    #[test]
    fn restore_mirrors_its_safety_checkpoint() {
        let s = Scratch::new("restore");
        let (saves, root, backup) = fixture(&s);
        let cp = create_checkpoint_in(&root, &saves, "Good", false).unwrap();
        write(&saves.join("one.alsv"), b"overwritten");
        write(&saves.join("three.alsv"), b"new");
        let safety = restore_checkpoint_in(&root, &cp, &saves, Some(&backup)).unwrap().unwrap();
        assert!(safety.manifest.safety);
        assert_eq!(safety.backup_warning, None);
        assert_eq!(backup_status(&safety, &backup, true), BackupStatus::BackedUp { verified: true });
        assert_eq!(std::fs::read(saves.join("one.alsv")).unwrap(), b"first save");
        assert!(!saves.join("three.alsv").exists());
    }

    #[test]
    fn checkpoints_only_in_the_backup_can_be_imported() {
        let s = Scratch::new("import");
        let (saves, root, backup) = fixture(&s);
        let kept = create_checkpoint_in(&root, &saves, "Kept", false).unwrap();
        write(&saves.join("one.alsv"), b"first save, later");
        let lost = create_checkpoint_in(&root, &saves, "Lost", false).unwrap();
        mirror_checkpoint(&kept, &backup).unwrap();
        mirror_checkpoint(&lost, &backup).unwrap();
        assert!(list_backup_only_in(&root, &backup).is_empty());

        // The disk dies and takes one checkpoint with it.
        std::fs::remove_dir_all(&lost.dir).unwrap();
        let only = list_backup_only_in(&root, &backup);
        assert_eq!(only.len(), 1);
        assert_eq!(only[0].checkpoint.manifest.label, "Lost");
        assert!(only[0].same_folder);

        let back = import_checkpoint_into(&root, &only[0].checkpoint).unwrap();
        assert_eq!(back.dir, lost.dir);
        verify_checkpoint(&back).unwrap();
        assert_eq!(std::fs::read(back.dir.join("files/one.alsv")).unwrap(), b"first save, later");
        assert!(list_backup_only_in(&root, &backup).is_empty());
        assert_eq!(list_checkpoints_in(&root).len(), 2);
        // Importing it twice is refused rather than overwriting.
        assert!(import_checkpoint_into(&root, &only[0].checkpoint).unwrap_err().contains("already"));

        // Everything local is gone and the save folder now has another key (new user name).
        let new_root = s.0.join("data2/checkpoints/ffff00");
        let only = list_backup_only_in(&new_root, &backup);
        assert_eq!(only.len(), 2);
        assert!(only.iter().all(|o| !o.same_folder));
        assert_eq!(only[0].checkpoint.manifest.label, "Lost", "newest first");
        let back = import_checkpoint_into(&new_root, &only[1].checkpoint).unwrap();
        verify_checkpoint(&back).unwrap();
        assert_eq!(list_backup_only_in(&new_root, &backup).len(), 1);

        // Another save folder that still has its own checkpoints here keeps them to itself.
        let other_root = s.dir("data/checkpoints/999999");
        assert!(list_backup_only_in(&other_root, &backup).is_empty());
    }

    #[test]
    fn a_damaged_backup_is_not_imported() {
        let s = Scratch::new("badimport");
        let (saves, root, backup) = fixture(&s);
        let cp = create_checkpoint_in(&root, &saves, "Rotten", false).unwrap();
        mirror_checkpoint(&cp, &backup).unwrap();
        std::fs::remove_dir_all(&cp.dir).unwrap();
        let dst = mirror_dir(&cp.dir, &backup).unwrap();
        write(&dst.join("files/one.alsv"), b"first sAve");

        let only = list_backup_only_in(&root, &backup);
        let e = import_checkpoint_into(&root, &only[0].checkpoint).unwrap_err();
        assert!(e.contains("Rotten") && e.contains("checksum"), "{e}");
        assert!(!cp.dir.exists(), "a failed import leaves nothing behind");
        assert!(list_checkpoints_in(&root).is_empty());

        // A manifest that points outside the checkpoint is refused outright.
        let mut evil = only[0].checkpoint.clone();
        evil.manifest.files[0].name = "../../escape.alsv".into();
        assert!(import_checkpoint_into(&root, &evil).unwrap_err().contains("unsafe"));
        evil.manifest.files[0].name = "C:/Windows/escape.alsv".into();
        assert!(import_checkpoint_into(&root, &evil).unwrap_err().contains("unsafe"));
        assert!(!cp.dir.exists());
    }

    #[test]
    fn deleting_a_backup_copy() {
        let s = Scratch::new("delmirror");
        let (saves, root, backup) = fixture(&s);
        let cp = create_checkpoint_in(&root, &saves, "Temp", false).unwrap();
        // (Straight removal here, so the tests leave nothing in the Recycle Bin.)
        assert_eq!(delete_mirror_with(&cp, &backup, false), Ok(false));
        mirror_checkpoint(&cp, &backup).unwrap();
        assert_eq!(delete_mirror_with(&cp, &backup, false), Ok(true));
        assert!(!mirror_dir(&cp.dir, &backup).unwrap().exists());
        assert!(cp.dir.exists());
        // Never the local checkpoint, even if the backup folder is the launcher's own.
        assert_eq!(delete_mirror_with(&cp, root.parent().unwrap(), false), Ok(false));
        assert!(cp.dir.exists());
    }

    #[test]
    fn search_matches_every_word_anywhere() {
        let s = save("Before Kuat", "campaign", 12, 100);
        let note = SaveNote { note: "Fleet parked over Fondor".into(), tags: vec!["risky".into(), "ironman run".into()] };
        let text = search_text(&s, Some(&note));
        assert!(matches_query(&text, ""));
        assert!(matches_query(&text, "   "));
        assert!(matches_query(&text, "kuat"));
        assert!(matches_query(&text, "KUAT rebel"));
        assert!(matches_query(&text, "equal footing"));
        assert!(matches_query(&text, "fondor risky"));
        assert!(matches_query(&text, "ironman"));
        assert!(!matches_query(&text, "kuat empire"));
        // Without the note its words don't match.
        assert!(!matches_query(&search_text(&s, None), "fondor"));
    }

    #[test]
    fn tags_are_tidied() {
        assert_eq!(normalize_tag("  Risky "), Some("risky".into()));
        assert_eq!(normalize_tag("Iron   man,"), Some("iron man".into()));
        assert_eq!(normalize_tag(" , "), None);
        assert_eq!(normalize_tag(""), None);
    }

    #[test]
    fn sorting_saves() {
        let a = save("Alpha", "campaign", 3, 300);
        let b = save("bravo", "campaign", 9, 100);
        let c = save("Charlie", "skirmish", 0, 200);
        let order = |sort| {
            let mut v = vec![&a, &b, &c];
            sort_saves(&mut v, sort);
            v.iter().map(|s| s.name.as_str()).collect::<Vec<_>>()
        };
        assert_eq!(order(SaveSort::Newest), ["Alpha", "Charlie", "bravo"]);
        assert_eq!(order(SaveSort::Oldest), ["bravo", "Charlie", "Alpha"]);
        assert_eq!(order(SaveSort::Name), ["Alpha", "bravo", "Charlie"]);
        assert_eq!(order(SaveSort::Week), ["bravo", "Alpha", "Charlie"]);
    }

    #[test]
    fn notes_of_deleted_saves_are_pruned_only_in_their_folder() {
        let mut notes = BTreeMap::new();
        let n = || SaveNote { note: "x".into(), tags: vec![] };
        for k in [
            "c:\\saves\\here.alsv",
            "c:\\saves\\gone.alsv",
            "c:\\saves\\sub\\deeper.alsv",
            "c:\\saves2\\gone.alsv",
            "d:\\other\\gone.alsv",
        ] {
            notes.insert(k.to_owned(), n());
        }
        let removed = prune_notes(&mut notes, Path::new("C:/Saves/"), |k| k.ends_with("here.alsv"));
        assert_eq!(removed, 1);
        let left: Vec<&str> = notes.keys().map(String::as_str).collect();
        assert_eq!(
            left,
            ["c:\\saves2\\gone.alsv", "c:\\saves\\here.alsv", "c:\\saves\\sub\\deeper.alsv", "d:\\other\\gone.alsv"]
        );
        assert_eq!(prune_notes(&mut notes, Path::new("C:\\Saves"), |_| true), 0);
    }
}

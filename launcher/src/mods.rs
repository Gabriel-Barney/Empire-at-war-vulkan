//! Mod discovery: the game's Mods folder, Steam Workshop, and user-added folders.

use crate::config::{self, mod_key, Settings};
use crate::steam::{same_path, SteamInfo};
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum ModSource {
    GameFolder,
    Workshop,
    Extra,
}

impl ModSource {
    pub fn label(self) -> &'static str {
        match self {
            ModSource::GameFolder => "Mods folder",
            ModSource::Workshop => "Workshop",
            ModSource::Extra => "Extra folder",
        }
    }
}

#[derive(Clone, Debug)]
pub struct ModInfo {
    pub path: PathBuf,
    pub folder: String,
    /// Name from `modinfo.json`, or the folder name.
    pub name: String,
    pub summary: String,
    pub version: String,
    pub source: ModSource,
    pub workshop_id: Option<String>,
    pub icon: Option<PathBuf>,
    pub splash: Option<PathBuf>,
    pub has_modinfo: bool,
    pub has_data: bool,
    /// `modinfo.json` gave the mod a name. Without one, a Workshop mod is shown under the
    /// title Steam has for it (see [`apply_workshop_titles`]).
    pub own_name: bool,
    /// `modinfo.json` gave the mod a summary or description.
    pub own_summary: bool,
}

const IMAGE_EXT: [&str; 7] = ["png", "jpg", "jpeg", "bmp", "webp", "tga", "dds"];

fn ext_of(p: &Path) -> String {
    p.extension().map(|e| e.to_string_lossy().to_ascii_lowercase()).unwrap_or_default()
}

/// The FoC `Mods` folder for the detected (or overridden) install.
pub fn foc_dir(steam: &SteamInfo, settings: &Settings) -> Option<PathBuf> {
    settings.foc_dir_override.clone().filter(|p| p.is_dir()).or_else(|| steam.foc_dir.clone())
}

pub fn scan(steam: &SteamInfo, settings: &Settings) -> Vec<ModInfo> {
    let mut roots: Vec<(PathBuf, ModSource)> = Vec::new();
    if let Some(foc) = foc_dir(steam, settings) {
        roots.push((foc.join("Mods"), ModSource::GameFolder));
    }
    if let Some(ws) = &steam.workshop_dir {
        roots.push((ws.clone(), ModSource::Workshop));
    }
    for d in &settings.extra_mod_dirs {
        roots.push((d.clone(), ModSource::Extra));
    }

    let mut out: Vec<ModInfo> = Vec::new();
    for (root, source) in roots {
        // An extra folder may itself be a mod rather than a folder of mods.
        if source == ModSource::Extra && looks_like_mod(&root) {
            if !out.iter().any(|m| same_path(&m.path, &root)) {
                out.push(read_mod(&root, source));
            }
            continue;
        }
        let Ok(rd) = std::fs::read_dir(&root) else { continue };
        for entry in rd.flatten() {
            let path = entry.path();
            if !path.is_dir() || !looks_like_mod(&path) {
                continue;
            }
            if out.iter().any(|m| same_path(&m.path, &path)) {
                continue;
            }
            out.push(read_mod(&path, source));
        }
    }
    // Workshop folders are named after the item's number: use the titles Steam gave us the
    // last time the launcher was connected.
    apply_workshop_titles(&mut out, &crate::workshop::load_titles());
    out
}

/// Gives Workshop mods that don't name or describe themselves the title and description
/// Steam has for them, then re-sorts by name. Safe to call again whenever the titles change.
pub fn apply_workshop_titles(mods: &mut [ModInfo], titles: &crate::workshop::Titles) {
    for m in mods.iter_mut() {
        let Some(t) = m.workshop_id.as_ref().and_then(|id| titles.get(id)) else { continue };
        if !m.own_name && !t.title.trim().is_empty() {
            m.name = t.title.trim().to_owned();
        }
        if !m.own_summary && !t.description.trim().is_empty() {
            m.summary = t.description.trim().to_owned();
        }
    }
    mods.sort_by(|a, b| a.name.to_lowercase().cmp(&b.name.to_lowercase()));
}

fn looks_like_mod(dir: &Path) -> bool {
    dir.join("Data").is_dir() || dir.join("modinfo.json").is_file()
}

fn read_mod(path: &Path, source: ModSource) -> ModInfo {
    let folder = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
    let mut m = ModInfo {
        path: path.to_path_buf(),
        name: folder.clone(),
        folder: folder.clone(),
        summary: String::new(),
        version: String::new(),
        source,
        workshop_id: (source == ModSource::Workshop && folder.chars().all(|c| c.is_ascii_digit()))
            .then(|| folder.clone()),
        icon: None,
        splash: None,
        has_modinfo: false,
        has_data: path.join("Data").is_dir(),
        own_name: false,
        own_summary: false,
    };

    let mut icon_hint: Option<String> = None;
    if let Ok(text) = std::fs::read_to_string(path.join("modinfo.json")) {
        // Some mods ship modinfo.json with a UTF-8 BOM.
        if let Ok(v) = serde_json::from_str::<serde_json::Value>(text.trim_start_matches('\u{feff}')) {
            m.has_modinfo = true;
            let s = |k: &str| v.get(k).and_then(|x| x.as_str()).map(str::trim).unwrap_or("").to_owned();
            let name = s("name");
            if !name.is_empty() {
                m.name = name;
                m.own_name = true;
            }
            m.summary = s("summary");
            if m.summary.is_empty() {
                m.summary = s("description");
            }
            m.own_summary = !m.summary.is_empty();
            m.version = s("version");
            let icon = s("icon");
            if !icon.is_empty() {
                // Usually "<workshop id>/Icon.ico": only the file name is meaningful here.
                icon_hint = icon.replace('\\', "/").rsplit('/').next().map(str::to_owned);
            }
            if m.workshop_id.is_none() {
                m.workshop_id = v
                    .pointer("/steamdata/publishedfileid")
                    .and_then(|x| x.as_str())
                    .filter(|x| !x.is_empty())
                    .map(str::to_owned);
            }
        }
    }

    // Art: look at loose image files in the mod's root.
    let mut images: Vec<(PathBuf, u64)> = Vec::new();
    let mut icons: Vec<PathBuf> = Vec::new();
    if let Ok(rd) = std::fs::read_dir(path) {
        for e in rd.flatten() {
            let p = e.path();
            if !p.is_file() {
                continue;
            }
            let ext = ext_of(&p);
            if ext == "ico" {
                icons.push(p);
            } else if IMAGE_EXT.contains(&ext.as_str()) {
                let len = e.metadata().map(|m| m.len()).unwrap_or(0);
                images.push((p, len));
            }
        }
    }
    let stem = |p: &Path| p.file_stem().map(|s| s.to_string_lossy().to_lowercase()).unwrap_or_default();
    let find = |words: &[&str]| {
        images.iter().find(|(p, _)| words.iter().any(|w| stem(p).contains(w))).map(|(p, _)| p.clone())
    };
    m.splash = find(&["splash"])
        .or_else(|| find(&["banner", "header", "cover"]))
        .or_else(|| find(&["preview", "thumb", "logo"]))
        .or_else(|| images.iter().max_by_key(|(_, len)| *len).map(|(p, _)| p.clone()));
    m.icon = icon_hint
        .and_then(|h| {
            let p = path.join(h);
            p.is_file().then_some(p)
        })
        .or_else(|| icons.first().cloned())
        .or_else(|| find(&["icon", "logo"]))
        .or_else(|| m.splash.clone());
    m
}

// ------------------------------------------------------------------ the launcher's own notes about mods

/// `<data dir>\cache`, created on demand. Everything in it can be rebuilt.
pub fn cache_dir() -> PathBuf {
    let dir = config::data_dir().join("cache");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

fn read_json<T: serde::de::DeserializeOwned + Default>(path: &Path) -> T {
    std::fs::read_to_string(path)
        .ok()
        .and_then(|text| serde_json::from_str(text.trim_start_matches('\u{feff}')).ok())
        .unwrap_or_default()
}

fn write_json<T: Serialize>(path: &Path, value: &T) -> Result<(), String> {
    let tmp = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(value).map_err(|e| e.to_string())?;
    std::fs::write(&tmp, text).map_err(|e| format!("Couldn't write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| format!("Couldn't replace {}: {e}", path.display()))
}

fn unix_secs(time: std::io::Result<std::time::SystemTime>) -> i64 {
    time.ok()
        .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
        .map_or(0, |d| d.as_secs() as i64)
}

// ---- what a mod looked like the last time the launcher saw it

/// A mod was found changed: which version it had before and has now.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq, Eq)]
pub struct Change {
    #[serde(default)]
    pub from_version: String,
    #[serde(default)]
    pub to_version: String,
    /// Steam's `timeupdated` before and after, Unix seconds (0 = not a Workshop item).
    #[serde(default)]
    pub from_time: i64,
    #[serde(default)]
    pub to_time: i64,
}

#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq, Eq)]
pub struct Seen {
    /// `version` from `modinfo.json`.
    #[serde(default)]
    pub version: String,
    /// Steam's `timeupdated` for the installed files (0 = unknown).
    #[serde(default)]
    pub time_updated: i64,
    /// Set when the mod changed, until the user dismisses the notice.
    #[serde(default)]
    pub changed: Option<Change>,
}

/// Keyed by `config::mod_key(path)`.
pub type SeenMap = BTreeMap<String, Seen>;

pub fn seen_path() -> PathBuf {
    cache_dir().join("mods-seen.json")
}

/// Missing or unreadable file = nothing seen yet.
pub fn load_seen(path: &Path) -> SeenMap {
    read_json(path)
}

pub fn save_seen(path: &Path, seen: &SeenMap) -> Result<(), String> {
    write_json(path, seen)
}

/// Compares a mod with what was remembered about it, flags a change and remembers the new
/// state. `time_updated` 0 means "unknown" and never counts as a change. True when `seen`
/// was modified.
pub fn note_seen(seen: &mut SeenMap, key: &str, version: &str, time_updated: i64) -> bool {
    let Some(entry) = seen.get_mut(key) else {
        // First sight of a mod is not a change.
        seen.insert(key.to_owned(), Seen { version: version.to_owned(), time_updated, changed: None });
        return true;
    };
    let version_changed = entry.version != version;
    let time_changed = time_updated > 0 && entry.time_updated > 0 && entry.time_updated != time_updated;
    if !version_changed && !time_changed {
        if entry.time_updated == 0 && time_updated > 0 {
            entry.time_updated = time_updated;
            return true;
        }
        return false;
    }
    let new_time = if time_updated > 0 { time_updated } else { entry.time_updated };
    // A second change before the first was dismissed keeps the original "from".
    let mut change = entry.changed.take().unwrap_or_else(|| Change {
        from_version: entry.version.clone(),
        from_time: entry.time_updated,
        ..Default::default()
    });
    change.to_version = version.to_owned();
    change.to_time = new_time;
    // Back to what the user last knew: nothing to tell them.
    let back = change.from_version == change.to_version && change.from_time == change.to_time;
    entry.changed = (!back).then_some(change);
    entry.version = version.to_owned();
    entry.time_updated = new_time;
    true
}

/// Clears a mod's "changed" notice. True when there was one.
pub fn dismiss_change(seen: &mut SeenMap, key: &str) -> bool {
    seen.get_mut(key).is_some_and(|entry| entry.changed.take().is_some())
}

// ---- size on disk

/// What walking a mod's folder found.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Measured {
    pub bytes: u64,
    pub files: u64,
    /// Newest file modification time in the folder, Unix seconds (0 = no files).
    pub newest: i64,
}

/// What a measurement was taken against. It is reused for as long as none of these moved:
/// the folder's and its `Data` folder's own modification times, and Steam's `timeupdated`
/// for Workshop items. Edits deep inside a folder don't move them, so the user's Rescan
/// measures again regardless.
#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
struct Stamp {
    root: i64,
    data: i64,
    steam: i64,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
struct CachedSize {
    stamp: Stamp,
    measured: Measured,
}

/// Keyed by `config::mod_key(path)`.
type SizeCache = BTreeMap<String, CachedSize>;

fn stamp_of(path: &Path, steam: i64) -> Stamp {
    let mtime = |p: &Path| unix_secs(std::fs::metadata(p).and_then(|m| m.modified()));
    Stamp { root: mtime(path), data: mtime(&path.join("Data")), steam }
}

/// Adds up every file under `root`. Links to other folders are not followed, and a
/// sub-folder that can't be read is skipped. Slow for big mods (tens of thousands of
/// files): never call it on the UI thread. Setting `cancel` makes it stop early.
pub fn measure_dir(root: &Path, cancel: &AtomicBool) -> Result<Measured, String> {
    let mut out = Measured::default();
    let mut stack: Vec<PathBuf> = Vec::new();
    let mut next = Some(std::fs::read_dir(root).map_err(|e| format!("Couldn't read {}: {e}", root.display()))?);
    loop {
        let entries = match next.take() {
            Some(entries) => entries,
            None => match stack.pop() {
                Some(dir) => match std::fs::read_dir(&dir) {
                    Ok(entries) => entries,
                    Err(_) => continue,
                },
                None => break,
            },
        };
        if cancel.load(Ordering::Relaxed) {
            return Err("Stopped.".to_owned());
        }
        for entry in entries.flatten() {
            let Ok(kind) = entry.file_type() else { continue };
            if kind.is_symlink() {
                continue;
            }
            if kind.is_dir() {
                stack.push(entry.path());
            } else if let Ok(meta) = entry.metadata() {
                out.bytes += meta.len();
                out.files += 1;
                out.newest = out.newest.max(unix_secs(meta.modified()));
            }
        }
    }
    Ok(out)
}

/// The remembered measurement if it still applies, else a fresh one, which is then
/// remembered. The flag says whether `cache` changed.
fn measure_cached(
    cache: &mut SizeCache,
    key: &str,
    path: &Path,
    steam: i64,
    force: bool,
    cancel: &AtomicBool,
) -> (Result<Measured, String>, bool) {
    let stamp = stamp_of(path, steam);
    if !force && stamp.root != 0 {
        if let Some(hit) = cache.get(key).filter(|c| c.stamp == stamp) {
            return (Ok(hit.measured), false);
        }
    }
    match measure_dir(path, cancel) {
        Ok(measured) => {
            let entry = CachedSize { stamp, measured };
            let changed = cache.insert(key.to_owned(), entry) != Some(entry);
            (Ok(measured), changed)
        }
        Err(e) => (Err(e), false),
    }
}

struct SizeJob {
    key: String,
    path: PathBuf,
    steam: i64,
    force: bool,
}

enum SizeEvent {
    Done(String, Result<Measured, String>),
    /// The cache file couldn't be written. Sizes still work; they are measured again next run.
    Trouble(String),
}

fn size_worker(jobs: Receiver<SizeJob>, events: Sender<SizeEvent>, cache_file: PathBuf, cancel: Arc<AtomicBool>, ctx: egui::Context) {
    let mut cache: SizeCache = read_json(&cache_file);
    let mut complained = false;
    for job in jobs {
        if cancel.load(Ordering::Relaxed) {
            break;
        }
        let (result, changed) = measure_cached(&mut cache, &job.key, &job.path, job.steam, job.force, &cancel);
        if cancel.load(Ordering::Relaxed) {
            break;
        }
        if changed {
            if let Err(e) = write_json(&cache_file, &cache) {
                if !std::mem::replace(&mut complained, true) {
                    let _ = events.send(SizeEvent::Trouble(e));
                }
            }
        }
        if events.send(SizeEvent::Done(job.key, result)).is_err() {
            break;
        }
        ctx.request_repaint();
    }
}

/// How much disk space each mod folder takes. Folders are walked one after another on a
/// worker thread, and the results are kept in `<data dir>\cache\mod-sizes.json` so a mod
/// is only walked again when it changed. `Default` is cheap and starts nothing.
#[derive(Default)]
pub struct Sizes {
    known: HashMap<String, Measured>,
    /// Requests the worker has not answered yet, per mod.
    waiting: HashMap<String, u32>,
    link: Option<(Sender<SizeJob>, Receiver<SizeEvent>)>,
    cancel: Option<Arc<AtomicBool>>,
}

impl Sizes {
    /// Queues folders for measuring: each with Steam's `timeupdated` for it (0 when it is
    /// not a Workshop item). A folder already in the queue is not queued twice. With
    /// `force` the remembered result is ignored and the folder is walked again.
    pub fn request(
        &mut self,
        ctx: &egui::Context,
        folders: impl IntoIterator<Item = (PathBuf, i64)>,
        force: bool,
    ) -> Result<(), String> {
        for (path, steam) in folders {
            let key = mod_key(&path);
            if !force && self.waiting.contains_key(&key) {
                continue;
            }
            if self.link.is_none() {
                let (job_tx, job_rx) = mpsc::channel();
                let (event_tx, event_rx) = mpsc::channel();
                let cancel = Arc::new(AtomicBool::new(false));
                let (flag, ctx, file) = (cancel.clone(), ctx.clone(), cache_dir().join("mod-sizes.json"));
                std::thread::Builder::new()
                    .name("mod-sizes".into())
                    .spawn(move || size_worker(job_rx, event_tx, file, flag, ctx))
                    .map_err(|e| format!("Couldn't start measuring mod folders: {e}"))?;
                self.link = Some((job_tx, event_rx));
                self.cancel = Some(cancel);
            }
            let sent = self.link.as_ref().is_some_and(|(tx, _)| tx.send(SizeJob { key: key.clone(), path, steam, force }).is_ok());
            if !sent {
                // The worker is gone (it never stops by itself): start a new one next time.
                self.link = None;
                self.waiting.clear();
                return Err("Measuring mod folders stopped unexpectedly.".to_owned());
            }
            *self.waiting.entry(key).or_insert(0) += 1;
        }
        Ok(())
    }

    /// Takes in finished measurements. Returns whether anything changed, and problems to
    /// show the user.
    pub fn poll(&mut self) -> (bool, Vec<String>) {
        let mut changed = false;
        let mut problems = Vec::new();
        let Some((_, events)) = &self.link else { return (changed, problems) };
        for event in events.try_iter() {
            match event {
                SizeEvent::Trouble(e) => problems.push(e),
                SizeEvent::Done(key, result) => {
                    changed = true;
                    if let Some(n) = self.waiting.get_mut(&key) {
                        *n = n.saturating_sub(1);
                        if *n == 0 {
                            self.waiting.remove(&key);
                        }
                    }
                    match result {
                        Ok(measured) => {
                            self.known.insert(key, measured);
                        }
                        Err(e) => {
                            self.known.remove(&key);
                            problems.push(e);
                        }
                    }
                }
            }
        }
        (changed, problems)
    }

    /// The measurement for a mod, by `config::mod_key(path)`. `None` until it is known.
    pub fn get(&self, key: &str) -> Option<Measured> {
        self.known.get(key).copied()
    }

    /// A measurement for this mod is queued or running.
    pub fn is_waiting(&self, key: &str) -> bool {
        self.waiting.contains_key(key)
    }
}

impl Drop for Sizes {
    fn drop(&mut self) {
        // Stops a walk in progress; the worker then ends because its queue is closed.
        if let Some(cancel) = &self.cancel {
            cancel.store(true, Ordering::Relaxed);
        }
    }
}

// ------------------------------------------------------------------ tests

#[cfg(test)]
mod tests {
    use super::*;

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

    fn write(path: &Path, bytes: usize) {
        std::fs::create_dir_all(path.parent().unwrap()).unwrap();
        std::fs::write(path, vec![b'x'; bytes]).unwrap();
    }

    #[test]
    fn folders_are_measured() {
        let s = Scratch::new("mods-measure");
        let root = s.0.join("MyMod");
        write(&root.join("modinfo.json"), 10);
        write(&root.join("Data").join("XML").join("units.xml"), 1000);
        write(&root.join("Data").join("Art").join("Textures").join("a.dds"), 5000);
        std::fs::create_dir_all(root.join("Data").join("Empty")).unwrap();
        let never = AtomicBool::new(false);

        let m = measure_dir(&root, &never).unwrap();
        assert_eq!((m.bytes, m.files), (6010, 3));
        assert!(m.newest > 1_600_000_000);
        assert_eq!(measure_dir(&root.join("Data").join("Empty"), &never), Ok(Measured::default()));
        assert!(measure_dir(&s.0.join("nowhere"), &never).is_err());
        assert!(measure_dir(&root, &AtomicBool::new(true)).is_err());
    }

    #[test]
    fn measurements_are_reused_until_the_stamp_moves() {
        let s = Scratch::new("mods-size-cache");
        let root = s.0.join("123");
        write(&root.join("Data").join("XML").join("a.xml"), 100);
        let never = AtomicBool::new(false);
        let mut cache = SizeCache::new();

        let (first, changed) = measure_cached(&mut cache, "k", &root, 5, false, &never);
        assert_eq!(first.map(|m| m.bytes), Ok(100));
        assert!(changed);

        // Rewriting a file deep inside moves neither folder's own time: the remembered size stands.
        write(&root.join("Data").join("XML").join("a.xml"), 250);
        let (again, changed) = measure_cached(&mut cache, "k", &root, 5, false, &never);
        assert_eq!(again.map(|m| m.bytes), Ok(100));
        assert!(!changed);

        // A Steam update (new timeupdated) or a forced rescan measures again.
        let (updated, changed) = measure_cached(&mut cache, "k", &root, 6, false, &never);
        assert_eq!(updated.map(|m| m.bytes), Ok(250));
        assert!(changed);
        write(&root.join("Data").join("XML").join("a.xml"), 300);
        let (forced, changed) = measure_cached(&mut cache, "k", &root, 6, true, &never);
        assert_eq!(forced.map(|m| m.bytes), Ok(300));
        assert!(changed);

        // The cache survives a round trip through its file.
        let file = s.0.join("mod-sizes.json");
        write_json(&file, &cache).unwrap();
        let mut loaded: SizeCache = read_json(&file);
        assert_eq!(loaded, cache);
        let (reloaded, changed) = measure_cached(&mut loaded, "k", &root, 6, false, &never);
        assert_eq!(reloaded.map(|m| m.bytes), Ok(300));
        assert!(!changed);

        // A folder that is gone is an error and leaves the cache alone.
        let (missing, changed) = measure_cached(&mut loaded, "k", &s.0.join("gone"), 6, false, &never);
        assert!(missing.is_err() && !changed);
        assert_eq!(read_json::<SizeCache>(&s.0.join("no-such-file.json")), SizeCache::new());
    }

    #[test]
    fn changes_between_runs_are_noticed() {
        let mut seen = SeenMap::new();
        // First sight: remembered, not flagged.
        assert!(note_seen(&mut seen, "a", "1.0", 100));
        assert_eq!(seen["a"].changed, None);
        assert!(!note_seen(&mut seen, "a", "1.0", 100));
        // Steam's records being unreadable for a run is not a change.
        assert!(!note_seen(&mut seen, "a", "1.0", 0));
        assert_eq!(seen["a"].time_updated, 100);

        // A new version.
        assert!(note_seen(&mut seen, "a", "1.1", 100));
        let change = seen["a"].changed.clone().unwrap();
        assert_eq!((change.from_version.as_str(), change.to_version.as_str()), ("1.0", "1.1"));
        assert!(!note_seen(&mut seen, "a", "1.1", 100));
        assert!(seen["a"].changed.is_some());

        // A second change before the notice was dismissed keeps where it started.
        assert!(note_seen(&mut seen, "a", "1.1", 200));
        let change = seen["a"].changed.clone().unwrap();
        assert_eq!((change.from_version.as_str(), change.from_time, change.to_time), ("1.0", 100, 200));
        assert!(dismiss_change(&mut seen, "a"));
        assert!(!dismiss_change(&mut seen, "a"));
        assert!(!dismiss_change(&mut seen, "unknown"));
        assert!(!note_seen(&mut seen, "a", "1.1", 200));
        assert_eq!(seen["a"].changed, None);

        // Changing and changing back cancels out.
        assert!(note_seen(&mut seen, "a", "2.0", 200));
        assert!(note_seen(&mut seen, "a", "1.1", 200));
        assert_eq!(seen["a"].changed, None);

        // A Steam time learned later is stored without a notice.
        assert!(note_seen(&mut seen, "b", "", 0));
        assert!(note_seen(&mut seen, "b", "", 300));
        assert_eq!(seen["b"], Seen { version: String::new(), time_updated: 300, changed: None });
        // Only Steam's time moved (the mod has no version of its own).
        assert!(note_seen(&mut seen, "b", "", 400));
        assert_eq!(seen["b"].changed.as_ref().map(|c| (c.from_time, c.to_time)), Some((300, 400)));
    }

    #[test]
    fn seen_file_round_trip() {
        let s = Scratch::new("mods-seen");
        let file = s.0.join("mods-seen.json");
        assert!(load_seen(&file).is_empty());
        let mut seen = SeenMap::new();
        note_seen(&mut seen, "f:\\mods\\one", "1.0", 10);
        note_seen(&mut seen, "f:\\mods\\one", "1.2", 20);
        save_seen(&file, &seen).unwrap();
        assert_eq!(load_seen(&file), seen);
        // Older files without the newer fields still load.
        std::fs::write(&file, "{\"x\": {\"version\": \"3\"}}").unwrap();
        assert_eq!(load_seen(&file)["x"], Seen { version: "3".to_owned(), time_updated: 0, changed: None });
    }
}

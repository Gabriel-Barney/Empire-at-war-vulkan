//! Mod conflict and load-order checker.
//!
//! What it knows about the game (see the top of `unitdb.rs` and the port's `vfs.cpp`):
//! * mods are layers in load order and the first layer that has a file wins it, so the
//!   earlier mod in a profile replaces every file it shares with a later one;
//! * the list files (`GameObjectFiles.xml` and friends) are opened once, from the first
//!   layer that has them. They are not merged: a lower mod's list, and any XML file only
//!   that list names, is never read.
//!
//! Only loose files are compared. Only file names are listed while walking a mod's `Data`
//! tree; contents are read for the list files and for XML data files two mods share.
//! Everything here runs on a worker thread ([`Checker`]); nothing touches the disk on the
//! UI thread.

use crate::unitdb::{self, BuildInput, Layer, Progress, UnitDb};
use crate::xml;
use std::collections::{HashMap, HashSet};
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::sync::Arc;

/// The list files the game opens from the first layer that has them, with the words the
/// warnings use for them.
const LISTS: [(&str, &str); 4] = [
    ("GameObjectFiles.xml", "object list"),
    ("HardPointDataFiles.xml", "hardpoint list"),
    ("TradeRouteFiles.xml", "trade route list"),
    ("CampaignFiles.xml", "campaign list"),
];

/// Folders nested deeper than this are reported instead of walked (a link loop).
const MAX_DEPTH: usize = 48;

#[derive(Clone, Debug)]
pub struct ModInput {
    pub name: String,
    pub path: PathBuf,
    pub workshop_id: Option<String>,
    /// The mod scan found it. Flips when Steam finishes installing a mod, which is what
    /// makes the check run again for a list that otherwise didn't change.
    pub present: bool,
    /// The mod's version text, if it has one. A changed version means changed files.
    pub version: String,
}

/// A profile's mods in load order (first wins) and the game they sit on top of.
#[derive(Clone, Debug, Default)]
pub struct Input {
    pub mods: Vec<ModInput>,
    pub foc: Option<PathBuf>,
}

impl Input {
    /// Changes whenever the mods, their order, their names or the game folder change.
    pub fn signature(&self) -> u64 {
        let mut h = std::collections::hash_map::DefaultHasher::new();
        for m in &self.mods {
            cache_key(m).hash(&mut h);
            m.name.hash(&mut h);
            m.workshop_id.hash(&mut h);
        }
        self.foc.as_deref().map(key_of).hash(&mut h);
        h.finish()
    }
}

fn key_of(path: &Path) -> String {
    crate::config::mod_key(path)
}

/// What a mod's file index is remembered under: a mod that was installed or updated
/// since gets a new key, and with it a fresh walk of its folder.
fn cache_key(m: &ModInput) -> String {
    format!("{}|{}|{}", key_of(&m.path), m.present, m.version)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Severity {
    /// Worth knowing, nothing to fix.
    Info,
    Warning,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Kind {
    /// A lower mod's list file is never read because a mod above has the same list.
    ListIgnored,
    /// A mod that names another profile mod as its parent sits below it.
    SubmodOrder,
    /// Every file the mod has is also provided by mods above it.
    NoEffect,
    /// The mod folder has no `Data` folder.
    NoData,
    /// Part of a mod couldn't be read, so the comparison is incomplete.
    Unreadable,
}

#[derive(Clone, Debug)]
pub struct Warning {
    pub severity: Severity,
    pub kind: Kind,
    pub text: String,
    /// Indices into the profile's mod list.
    pub mods: Vec<usize>,
    /// Moving a mod up or down is the likely fix.
    pub order: bool,
}

/// Files two mods both have. `upper` is earlier in the list and wins all of them.
#[derive(Clone, Debug)]
pub struct PairOverlap {
    pub upper: usize,
    pub lower: usize,
    /// Paths relative to `Data`, as the upper mod spells them, sorted.
    pub shared_files: Vec<String>,
    /// How many of the shared files are `.xml`.
    pub xml_files: usize,
    /// Definitions (by name) in the lower mod's copies of shared XML data files the game
    /// loads from the upper mod, that the upper mod's copy defines too.
    pub definitions: usize,
    /// Definitions in those lower copies that the upper mod's copy doesn't have at all.
    pub lost_definitions: usize,
}

#[derive(Clone, Debug, Default)]
pub struct ModSummary {
    pub exists: bool,
    pub has_data: bool,
    /// Loose files under `Data`.
    pub files: usize,
    /// How many of them a mod higher in the list also has (and therefore wins).
    pub replaced_files: usize,
}

#[derive(Clone, Debug, Default)]
pub struct Report {
    /// Mod names in load order, as given in the input.
    pub names: Vec<String>,
    pub pairs: Vec<PairOverlap>,
    pub warnings: Vec<Warning>,
    pub per_mod: Vec<ModSummary>,
}

impl Report {
    /// Overlaps where mod `i` is the winner.
    pub fn overrides_of(&self, i: usize) -> impl Iterator<Item = &PairOverlap> {
        self.pairs.iter().filter(move |p| p.upper == i)
    }

    /// Warnings (not notes) that involve mod `i`.
    pub fn warnings_for(&self, i: usize) -> impl Iterator<Item = &Warning> {
        self.warnings.iter().filter(move |w| w.severity == Severity::Warning && w.mods.contains(&i))
    }

    /// Warnings that moving a mod would probably fix.
    pub fn order_warnings(&self) -> Vec<&Warning> {
        self.warnings.iter().filter(|w| w.severity == Severity::Warning && w.order).collect()
    }

    pub fn name(&self, i: usize) -> &str {
        self.names.get(i).map(String::as_str).unwrap_or("a mod")
    }
}

// ------------------------------------------------------------------ file index

/// Every loose file under one mod's `Data` folder.
#[derive(Debug, Default)]
pub struct ModFiles {
    pub exists: bool,
    pub has_data: bool,
    /// Lower-cased path relative to `Data` (backslashes) -> the path as it is on disk.
    pub files: HashMap<String, String>,
    /// Folders that couldn't be listed, with the reason.
    pub unreadable: Vec<String>,
}

/// File indexes by mod folder, kept between runs so reordering a profile doesn't walk
/// tens of thousands of files again.
pub type IndexCache = HashMap<String, Arc<ModFiles>>;

/// Lists a mod's `Data` tree. Names only: no file is opened.
pub fn index_mod(path: &Path) -> ModFiles {
    let mut out = ModFiles { exists: path.is_dir(), ..Default::default() };
    let data = path.join("Data");
    out.has_data = data.is_dir();
    if !out.has_data {
        return out;
    }
    // (folder, path relative to Data with a trailing backslash, depth)
    let mut stack: Vec<(PathBuf, String, usize)> = vec![(data, String::new(), 0)];
    while let Some((dir, prefix, depth)) = stack.pop() {
        let entries = match std::fs::read_dir(&dir) {
            Ok(rd) => rd,
            Err(e) => {
                out.unreadable.push(format!("{}: {e}", dir.display()));
                continue;
            }
        };
        for entry in entries {
            let entry = match entry {
                Ok(e) => e,
                Err(e) => {
                    out.unreadable.push(format!("{}: {e}", dir.display()));
                    continue;
                }
            };
            let name = entry.file_name().to_string_lossy().into_owned();
            let rel = format!("{prefix}{name}");
            // Links are followed, as the game follows them.
            let is_dir = match entry.file_type() {
                Ok(t) if t.is_symlink() => entry.path().is_dir(),
                Ok(t) => t.is_dir(),
                Err(e) => {
                    out.unreadable.push(format!("{}: {e}", entry.path().display()));
                    continue;
                }
            };
            if is_dir {
                if depth + 1 >= MAX_DEPTH {
                    out.unreadable.push(format!("{}: nested too deeply to be real", entry.path().display()));
                } else {
                    stack.push((entry.path(), format!("{rel}\\"), depth + 1));
                }
            } else {
                out.files.insert(rel.to_lowercase(), rel);
            }
        }
    }
    out
}

// ------------------------------------------------------------------ analysis

/// Runs the whole check, walking any mod that isn't in `cache` yet.
pub fn analyze(input: &Input, cache: &mut IndexCache) -> Report {
    let indexes: Vec<Arc<ModFiles>> = input
        .mods
        .iter()
        .map(|m| {
            let key = cache_key(m);
            if let Some(known) = cache.get(&key) {
                return known.clone();
            }
            let index = Arc::new(index_mod(&m.path));
            // A folder that isn't there yet is looked at again next time.
            if index.exists {
                cache.insert(key, index.clone());
            }
            index
        })
        .collect();
    analyze_indexed(input, &indexes)
}

fn analyze_indexed(input: &Input, idx: &[Arc<ModFiles>]) -> Report {
    let n = input.mods.len();
    let name = |i: usize| input.mods[i].name.as_str();
    let mut report = Report {
        names: input.mods.iter().map(|m| m.name.clone()).collect(),
        per_mod: idx
            .iter()
            .map(|f| ModSummary { exists: f.exists, has_data: f.has_data, files: f.files.len(), replaced_files: 0 })
            .collect(),
        ..Default::default()
    };

    // ---- folders that add nothing, and folders that couldn't be read
    for (i, f) in idx.iter().enumerate() {
        if f.exists && !f.has_data {
            report.warnings.push(Warning {
                severity: Severity::Warning,
                kind: Kind::NoData,
                text: format!("{} has no Data folder, so it changes nothing in the game.", name(i)),
                mods: vec![i],
                order: false,
            });
        }
        if !f.unreadable.is_empty() {
            let more = match f.unreadable.len() {
                1 => String::new(),
                k => format!(" (and {} more)", k - 1),
            };
            report.warnings.push(Warning {
                severity: Severity::Warning,
                kind: Kind::Unreadable,
                text: format!(
                    "Part of {} couldn't be read, so its conflicts may be incomplete: {}{more}",
                    name(i),
                    f.unreadable[0]
                ),
                mods: vec![i],
                order: false,
            });
        }
    }

    // ---- file level: every pair, and how much of each mod is replaced from above
    for lower in 0..n {
        let mut replaced: HashSet<&str> = HashSet::new();
        for upper in 0..lower {
            let (a, b) = (&idx[upper].files, &idx[lower].files);
            // Walk the smaller map, look up in the larger.
            let mut shared: Vec<&str> = if a.len() <= b.len() {
                a.keys().filter(|k| b.contains_key(*k)).map(String::as_str).collect()
            } else {
                b.keys().filter(|k| a.contains_key(*k)).map(String::as_str).collect()
            };
            if shared.is_empty() {
                continue;
            }
            shared.sort_unstable();
            replaced.extend(shared.iter().copied());
            report.pairs.push(PairOverlap {
                upper,
                lower,
                xml_files: shared.iter().filter(|k| k.ends_with(".xml")).count(),
                shared_files: shared.iter().map(|k| a[*k].clone()).collect(),
                definitions: 0,
                lost_definitions: 0,
            });
        }
        report.per_mod[lower].replaced_files = replaced.len();
        let files = idx[lower].files.len();
        if files > 0 && replaced.len() == files {
            let above: Vec<usize> = report.pairs.iter().filter(|p| p.lower == lower).map(|p| p.upper).collect();
            let mut mods = vec![lower];
            mods.extend(above);
            report.warnings.push(Warning {
                severity: Severity::Warning,
                kind: Kind::NoEffect,
                text: format!(
                    "{} has no effect in this order: {} also in mods above it. Move it up if it is meant to change them.",
                    name(lower),
                    if files == 1 { "its one file is".to_owned() } else { format!("all {files} of its files are") }
                ),
                mods,
                order: true,
            });
        }
    }
    report.pairs.sort_by_key(|p| (p.upper, p.lower));

    list_warnings(input, idx, &mut report);
    count_definitions(input, idx, &mut report);
    submod_warnings(input, &mut report);
    // Warnings before notes, and the ones that cost the most first: a list file that is
    // never read takes whole files of units with it.
    report.warnings.sort_by_key(|w| {
        let rank = match w.kind {
            Kind::ListIgnored => 0,
            Kind::SubmodOrder => 1,
            Kind::NoEffect => 2,
            Kind::NoData => 3,
            Kind::Unreadable => 4,
        };
        (w.severity == Severity::Info, rank)
    });
    report
}

fn read_text(path: &Path) -> Option<String> {
    std::fs::read(path).ok().map(|bytes| xml::decode(&bytes).0)
}

/// The path on disk of the file a mod has under this lower-cased `Data`-relative key.
fn disk_path(input: &Input, idx: &[Arc<ModFiles>], m: usize, key: &str) -> Option<PathBuf> {
    idx[m].files.get(key).map(|rel| input.mods[m].path.join("Data").join(rel))
}

/// Lower-cased entries of a mod's copy of a list file.
fn list_of(input: &Input, idx: &[Arc<ModFiles>], m: usize, key: &str) -> Option<Vec<(String, String)>> {
    let text = read_text(&disk_path(input, idx, m, key)?)?;
    Some(unitdb::list_entries(&text).into_iter().map(|e| (e.to_lowercase(), e)).collect())
}

fn list_warnings(input: &Input, idx: &[Arc<ModFiles>], report: &mut Report) {
    let name = |i: usize| input.mods[i].name.as_str();
    for (list, words) in LISTS {
        let key = format!("xml\\{}", list.to_lowercase());
        let have: Vec<usize> = (0..idx.len()).filter(|i| idx[*i].files.contains_key(&key)).collect();
        let Some((&winner, rest)) = have.split_first() else { continue };
        if rest.is_empty() {
            continue;
        }
        let Some(winning) = list_of(input, idx, winner, &key) else { continue };
        let winning_keys: HashSet<&str> = winning.iter().map(|(k, _)| k.as_str()).collect();
        for &lower in rest {
            let Some(ignored) = list_of(input, idx, lower, &key) else { continue };
            let ignored_keys: HashSet<&str> = ignored.iter().map(|(k, _)| k.as_str()).collect();
            // Files only the ignored list names: the game never reads these.
            let mut lost: Vec<&str> = Vec::new();
            for (k, shown) in &ignored {
                if !winning_keys.contains(k.as_str()) && !lost.iter().any(|l| l.eq_ignore_ascii_case(shown)) {
                    lost.push(shown);
                }
            }
            let (a, b) = (name(winner), name(lower));
            if lost.is_empty() {
                report.warnings.push(Warning {
                    severity: Severity::Info,
                    kind: Kind::ListIgnored,
                    text: format!(
                        "{b}'s {words} is ignored because {a} also has {list}. {a}'s list names every file {b}'s does, so nothing is lost."
                    ),
                    mods: vec![winner, lower],
                    order: false,
                });
                continue;
            }
            let examples = lost.iter().take(3).copied().collect::<Vec<_>>().join(", ");
            let more = if lost.len() > 3 { ", …" } else { "" };
            let count = if lost.len() == 1 { "1 XML file".to_owned() } else { format!("{} XML files", lost.len()) };
            // The other way round nothing would be lost: the lower mod was built on top of
            // the upper one and belongs above it.
            let swap_fixes = winning_keys.iter().all(|k| ignored_keys.contains(k));
            let advice = if swap_fixes {
                format!(" {b}'s list includes everything in {a}'s, so {b} looks like it belongs above {a}.")
            } else {
                String::new()
            };
            report.warnings.push(Warning {
                severity: Severity::Warning,
                kind: Kind::ListIgnored,
                text: format!(
                    "{b}'s {words} is ignored because {a} also has {list}. {count} only {b} lists ({examples}{more}) won't load unless {a} lists them.{advice}"
                ),
                mods: vec![winner, lower],
                order: swap_fixes,
            });
        }
    }
}

/// For XML data files the game really loads from a mod, counts what that copy takes over
/// from the same-named file in each mod below.
fn count_definitions(input: &Input, idx: &[Arc<ModFiles>], report: &mut Report) {
    if !report.pairs.iter().any(|p| p.xml_files > 0) {
        return;
    }
    // The index resolves the list files and every file they name exactly as the game does.
    let foc = input.foc.clone().unwrap_or_else(|| std::env::temp_dir().join("command-center-no-game-folder"));
    let db = UnitDb::build(
        BuildInput { mods: input.mods.iter().map(|m| (m.name.clone(), m.path.clone())).collect(), foc },
        &Progress::default(),
    );
    // (mod, key) of each XML data file loaded from a mod. A file can be named by two lists.
    let mut loaded: Vec<(usize, String)> = Vec::new();
    for f in &db.files {
        let Layer::Mod(m) = &f.layer else { continue };
        let m = *m;
        let rel = f.rel.replace('/', "\\").to_lowercase();
        let Some(key) = rel.strip_prefix("data\\") else { continue };
        if !loaded.iter().any(|(lm, lk)| *lm == m && lk == key) {
            loaded.push((m, key.to_owned()));
        }
    }
    drop(db);

    let mut names: HashMap<(usize, String), Arc<HashSet<String>>> = HashMap::new();
    let mut names_of = |m: usize, key: &str| -> Arc<HashSet<String>> {
        names
            .entry((m, key.to_owned()))
            .or_insert_with(|| {
                let text = disk_path(input, idx, m, key).and_then(|p| read_text(&p)).unwrap_or_default();
                Arc::new(unitdb::definition_names(&text).into_iter().map(|n| n.to_lowercase()).collect())
            })
            .clone()
    };
    for pair in report.pairs.iter_mut().filter(|p| p.xml_files > 0) {
        for (m, key) in &loaded {
            if *m != pair.upper || !idx[pair.lower].files.contains_key(key) {
                continue;
            }
            let upper = names_of(pair.upper, key);
            let lower = names_of(pair.lower, key);
            let taken = lower.iter().filter(|n| upper.contains(*n)).count();
            pair.definitions += taken;
            pair.lost_definitions += lower.len() - taken;
        }
    }
}

/// Workshop ids (runs of six or more digits) in the text fields of `modinfo.json` where
/// a mod names what it depends on.
pub fn mentioned_ids(modinfo: &str) -> Vec<String> {
    let Ok(v) = serde_json::from_str::<serde_json::Value>(modinfo.trim_start_matches('\u{feff}')) else {
        return Vec::new();
    };
    fn strings(v: &serde_json::Value, out: &mut Vec<String>, depth: usize) {
        match v {
            serde_json::Value::String(s) => out.push(s.clone()),
            serde_json::Value::Number(n) => out.push(n.to_string()),
            serde_json::Value::Array(a) if depth < 8 => a.iter().for_each(|x| strings(x, out, depth + 1)),
            serde_json::Value::Object(o) if depth < 8 => o.values().for_each(|x| strings(x, out, depth + 1)),
            _ => {}
        }
    }
    let mut texts = Vec::new();
    for field in ["name", "summary", "description", "dependencies"] {
        if let Some(x) = v.get(field) {
            strings(x, &mut texts, 0);
        }
    }
    let mut ids: Vec<String> = Vec::new();
    for text in texts {
        let mut run = String::new();
        for c in text.chars().chain(std::iter::once(' ')) {
            if c.is_ascii_digit() {
                run.push(c);
                continue;
            }
            if run.len() >= 6 && !ids.contains(&run) {
                ids.push(run.clone());
            }
            run.clear();
        }
    }
    ids
}

fn submod_warnings(input: &Input, report: &mut Report) {
    let n = input.mods.len();
    let ids: Vec<Option<&str>> = input.mods.iter().map(|m| m.workshop_id.as_deref().filter(|id| !id.is_empty())).collect();
    if ids.iter().flatten().count() == 0 {
        return;
    }
    // mentions[x] = the other profile mods whose Workshop id x's modinfo names.
    let mentions: Vec<Vec<usize>> = (0..n)
        .map(|x| {
            let Ok(text) = std::fs::read_to_string(input.mods[x].path.join("modinfo.json")) else { return Vec::new() };
            let named = mentioned_ids(&text);
            (0..n)
                .filter(|y| *y != x && ids[*y].is_some_and(|id| ids[x] != Some(id) && named.iter().any(|m| m == id)))
                .collect()
        })
        .collect();
    for x in 0..n {
        for &y in &mentions[x] {
            // x names y as its parent, so x has to win: it must be earlier in the list.
            if x > y && !mentions[y].contains(&x) {
                report.warnings.push(Warning {
                    severity: Severity::Warning,
                    kind: Kind::SubmodOrder,
                    text: format!(
                        "{} looks like a submod of {} and should be above it.",
                        input.mods[x].name, input.mods[y].name
                    ),
                    mods: vec![x, y],
                    order: true,
                });
            }
        }
    }
}

// ------------------------------------------------------------------ worker

struct Finished {
    signature: u64,
    report: Report,
    cache: IndexCache,
}

/// Runs [`analyze`] on a worker thread and keeps the last result per mod-list signature.
#[derive(Default)]
pub struct Checker {
    done: Option<(u64, Arc<Report>)>,
    running: Option<(u64, Receiver<Finished>)>,
    cache: IndexCache,
    /// Set when the worker thread couldn't be started or died.
    pub error: Option<String>,
}

impl Checker {
    /// Call once per frame with the current profile. Returns the report for exactly this
    /// input once it is ready; until then a check is running in the background. With
    /// `start` false a missing report is not worked out now (the game is running and
    /// shouldn't have to share the disk with a walk through every mod folder).
    pub fn poll(&mut self, input: &Input, ctx: &egui::Context, start: bool) -> Option<Arc<Report>> {
        let signature = input.signature();
        if let Some((sig, rx)) = &self.running {
            match rx.try_recv() {
                Ok(finished) => {
                    self.cache = finished.cache;
                    self.done = Some((finished.signature, Arc::new(finished.report)));
                    self.running = None;
                }
                Err(TryRecvError::Empty) => {}
                Err(TryRecvError::Disconnected) => {
                    self.error = Some("The conflict check stopped unexpectedly.".to_owned());
                    // Don't try the same list again every frame.
                    self.done = Some((*sig, Arc::new(Report::default())));
                    self.running = None;
                }
            }
        }
        if let Some((sig, report)) = &self.done {
            if *sig == signature {
                return Some(report.clone());
            }
        }
        if self.running.is_none() && start {
            if input.mods.is_empty() {
                self.done = Some((signature, Arc::new(Report::default())));
                return self.done.as_ref().map(|(_, r)| r.clone());
            }
            self.start(input.clone(), signature, ctx);
        }
        None
    }

    fn start(&mut self, input: Input, signature: u64, ctx: &egui::Context) {
        self.error = None;
        let (tx, rx) = mpsc::channel();
        let mut cache = self.cache.clone();
        // Indexes of mods that are no longer in any list asked about are let go.
        cache.retain(|key, _| input.mods.iter().any(|m| &cache_key(m) == key));
        let ctx = ctx.clone();
        let spawned = std::thread::Builder::new().name("conflict-check".into()).spawn(move || {
            let report = analyze(&input, &mut cache);
            let _ = tx.send(Finished { signature, report, cache });
            ctx.request_repaint();
        });
        match spawned {
            Ok(_) => self.running = Some((signature, rx)),
            Err(e) => {
                self.error = Some(format!("The conflict check couldn't start: {e}"));
                self.done = Some((signature, Arc::new(Report::default())));
            }
        }
    }

    /// Forgets everything, so the next [`Checker::poll`] walks the mod folders again.
    pub fn refresh(&mut self) {
        self.done = None;
        self.cache.clear();
        self.error = None;
    }
}

// ------------------------------------------------------------------ tests

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicUsize, Ordering};

    static NEXT: AtomicUsize = AtomicUsize::new(0);

    /// A scratch folder that is removed when the test ends.
    struct Scratch(PathBuf);

    impl Scratch {
        fn new() -> Self {
            let dir = std::env::temp_dir().join(format!(
                "cc-conflicts-{}-{}",
                std::process::id(),
                NEXT.fetch_add(1, Ordering::SeqCst)
            ));
            let _ = std::fs::remove_dir_all(&dir);
            std::fs::create_dir_all(&dir).unwrap();
            Scratch(dir)
        }

        fn write(&self, rel: &str, text: &str) {
            let path = self.0.join(rel);
            std::fs::create_dir_all(path.parent().unwrap()).unwrap();
            std::fs::write(path, text).unwrap();
        }

        fn input(&self, mods: &[(&str, Option<&str>)]) -> Input {
            std::fs::create_dir_all(self.0.join("game").join("corruption")).unwrap();
            Input {
                mods: mods
                    .iter()
                    .map(|(name, id)| ModInput {
                        name: (*name).to_owned(),
                        path: self.0.join(name),
                        workshop_id: id.map(str::to_owned),
                        present: true,
                        version: String::new(),
                    })
                    .collect(),
                foc: Some(self.0.join("game").join("corruption")),
            }
        }
    }

    impl Drop for Scratch {
        fn drop(&mut self) {
            let _ = std::fs::remove_dir_all(&self.0);
        }
    }

    fn list(files: &[&str]) -> String {
        let body: String = files.iter().map(|f| format!("  <File>{f}</File>\n")).collect();
        format!("<?xml version=\"1.0\"?>\n<Game_Object_Files>\n{body}</Game_Object_Files>\n")
    }

    fn units(names: &[&str]) -> String {
        let body: String = names.iter().map(|n| format!("  <SpaceUnit Name=\"{n}\"><Health>10</Health></SpaceUnit>\n")).collect();
        format!("<?xml version=\"1.0\"?>\n<SpaceUnits>\n{body}</SpaceUnits>\n")
    }

    fn run(input: &Input) -> Report {
        analyze(input, &mut IndexCache::new())
    }

    #[test]
    fn shared_files_are_case_insensitive_and_the_upper_mod_wins() {
        let s = Scratch::new();
        s.write("A/Data/Art/Textures/Ship.dds", "a");
        s.write("A/Data/XML/Units.xml", &units(&["X"]));
        s.write("A/Data/Scripts/Only_A.lua", "a");
        s.write("B/Data/ART/textures/SHIP.DDS", "b");
        s.write("B/Data/Xml/units.XML", &units(&["X"]));
        s.write("B/Data/Scripts/Only_B.lua", "b");
        let r = run(&s.input(&[("A", None), ("B", None)]));
        assert_eq!(r.pairs.len(), 1);
        let p = &r.pairs[0];
        assert_eq!((p.upper, p.lower), (0, 1));
        assert_eq!(p.shared_files, vec!["Art\\Textures\\Ship.dds".to_owned(), "XML\\Units.xml".to_owned()]);
        assert_eq!(p.xml_files, 1);
        assert_eq!(r.per_mod[0].files, 3);
        assert_eq!(r.per_mod[0].replaced_files, 0);
        assert_eq!(r.per_mod[1].replaced_files, 2);
        assert!(r.warnings.is_empty(), "{:?}", r.warnings);
        assert_eq!(r.overrides_of(0).count(), 1);
        assert_eq!(r.overrides_of(1).count(), 0);
    }

    #[test]
    fn a_lower_mods_list_file_is_ignored() {
        let s = Scratch::new();
        s.write("A/Data/XML/GameObjectFiles.xml", &list(&["Base.xml", "A_Units.xml"]));
        s.write("A/Data/XML/A_Units.xml", &units(&["A1"]));
        s.write("B/Data/XML/GameObjectFiles.xml", &list(&["Base.xml", "B_Units.xml", "B_More.xml"]));
        s.write("B/Data/XML/B_Units.xml", &units(&["B1"]));
        let r = run(&s.input(&[("A", None), ("B", None)]));
        let w: Vec<&Warning> = r.warnings.iter().filter(|w| w.kind == Kind::ListIgnored).collect();
        assert_eq!(w.len(), 1);
        assert_eq!(w[0].severity, Severity::Warning);
        assert_eq!(w[0].mods, vec![0, 1]);
        assert!(w[0].text.contains("B's object list is ignored because A also has GameObjectFiles.xml"), "{}", w[0].text);
        assert!(w[0].text.contains("2 XML files only B lists"), "{}", w[0].text);
        assert!(w[0].text.contains("B_Units.xml"), "{}", w[0].text);
        // Neither list contains the other: reordering alone doesn't fix it.
        assert!(!w[0].order);
        assert!(r.order_warnings().is_empty());
        assert_eq!(r.warnings_for(1).count(), 1);
    }

    #[test]
    fn a_submod_list_below_its_parent_is_an_order_problem() {
        let s = Scratch::new();
        s.write("Parent/Data/XML/GameObjectFiles.xml", &list(&["Base.xml", "Parent.xml"]));
        s.write("Sub/Data/XML/GameObjectFiles.xml", &list(&["Base.xml", "Parent.xml", "Sub.xml"]));
        s.write("Parent/Data/XML/Parent.xml", &units(&["P"]));
        s.write("Sub/Data/XML/Sub.xml", &units(&["S"]));
        let wrong = run(&s.input(&[("Parent", None), ("Sub", None)]));
        assert_eq!(wrong.order_warnings().len(), 1);
        assert!(wrong.order_warnings()[0].text.contains("Sub looks like it belongs above Parent"));
        // The right way round the parent's list is a subset: a note, not a warning.
        let right = run(&s.input(&[("Sub", None), ("Parent", None)]));
        assert!(right.order_warnings().is_empty());
        assert_eq!(right.warnings.len(), 1);
        assert_eq!(right.warnings[0].severity, Severity::Info);
        assert_eq!(right.warnings_for(0).count(), 0);
    }

    #[test]
    fn definitions_taken_over_are_counted_for_files_that_load() {
        let s = Scratch::new();
        s.write("A/Data/XML/GameObjectFiles.xml", &list(&["Units.xml", "Extra.xml"]));
        s.write("A/Data/XML/Units.xml", &units(&["One", "TWO", "Three"]));
        s.write("B/Data/XML/Units.xml", &units(&["one", "Two", "Four", "Five"]));
        // Shared, but no list names it: the game never opens it, so nothing is counted.
        s.write("A/Data/XML/Unlisted.xml", &units(&["Q"]));
        s.write("B/Data/XML/Unlisted.xml", &units(&["Q"]));
        let r = run(&s.input(&[("A", None), ("B", None)]));
        let p = &r.pairs[0];
        assert_eq!(p.xml_files, 2);
        assert_eq!(p.definitions, 2);
        assert_eq!(p.lost_definitions, 2);
    }

    #[test]
    fn definitions_belong_to_the_mod_the_file_is_loaded_from() {
        let s = Scratch::new();
        s.write("Top/Data/XML/GameObjectFiles.xml", &list(&["Units.xml"]));
        s.write("Top/Data/XML/Units.xml", &units(&["One"]));
        s.write("Mid/Data/XML/Units.xml", &units(&["One", "Two"]));
        s.write("Low/Data/XML/Units.xml", &units(&["One", "Two"]));
        let r = run(&s.input(&[("Top", None), ("Mid", None), ("Low", None)]));
        let pair = |u: usize, l: usize| r.pairs.iter().find(|p| p.upper == u && p.lower == l).unwrap();
        assert_eq!((pair(0, 1).definitions, pair(0, 1).lost_definitions), (1, 1));
        assert_eq!((pair(0, 2).definitions, pair(0, 2).lost_definitions), (1, 1));
        // Mid's copy is never loaded, so it takes nothing over from Low.
        assert_eq!((pair(1, 2).definitions, pair(1, 2).lost_definitions), (0, 0));
        assert_eq!(pair(1, 2).shared_files.len(), 1);
    }

    #[test]
    fn a_fully_shadowed_mod_has_no_effect() {
        let s = Scratch::new();
        s.write("A/Data/Art/a.dds", "");
        s.write("B/Data/Art/b.dds", "");
        s.write("C/Data/Art/A.DDS", "");
        s.write("C/Data/Art/B.dds", "");
        let r = run(&s.input(&[("A", None), ("B", None), ("C", None)]));
        let w: Vec<&Warning> = r.warnings.iter().filter(|w| w.kind == Kind::NoEffect).collect();
        assert_eq!(w.len(), 1);
        assert_eq!(w[0].mods, vec![2, 0, 1]);
        assert!(w[0].text.starts_with("C has no effect in this order"), "{}", w[0].text);
        assert_eq!(r.order_warnings().len(), 1);
        // Put first, it is the one doing the replacing.
        let r = run(&s.input(&[("C", None), ("A", None), ("B", None)]));
        assert!(r.warnings.iter().all(|w| w.kind != Kind::NoEffect || !w.mods.starts_with(&[0])));
    }

    #[test]
    fn folders_without_data_and_missing_folders() {
        let s = Scratch::new();
        s.write("Empty/modinfo.json", "{}");
        s.write("A/Data/x.txt", "");
        let r = run(&s.input(&[("A", None), ("Empty", None), ("Gone", None)]));
        assert!(r.per_mod[1].exists && !r.per_mod[1].has_data);
        assert!(!r.per_mod[2].exists);
        let w: Vec<&Warning> = r.warnings.iter().collect();
        assert_eq!(w.len(), 1);
        assert_eq!(w[0].kind, Kind::NoData);
        assert_eq!(w[0].mods, vec![1]);
        assert!(r.order_warnings().is_empty());
    }

    #[test]
    fn a_submod_below_the_mod_it_names_is_reported() {
        let s = Scratch::new();
        s.write("Parent/Data/a.txt", "");
        s.write("Sub/Data/b.txt", "");
        s.write(
            "Sub/modinfo.json",
            r#"{"name":"Sub","dependencies":["ResolveRecursive",{"identifier":"1125571106","modtype":1}],"steamdata":{"publishedfileid":"2222222222"}}"#,
        );
        s.write("Parent/modinfo.json", r#"{"name":"Parent","description":"Version 12345 of 2020."}"#);
        let mods = [("Parent", Some("1125571106")), ("Sub", Some("2222222222"))];
        let r = run(&s.input(&mods));
        assert_eq!(r.order_warnings().len(), 1);
        assert_eq!(r.order_warnings()[0].kind, Kind::SubmodOrder);
        assert_eq!(r.order_warnings()[0].text, "Sub looks like a submod of Parent and should be above it.");
        assert_eq!(r.order_warnings()[0].mods, vec![1, 0]);
        let r = run(&s.input(&[mods[1], mods[0]]));
        assert!(r.warnings.is_empty(), "{:?}", r.warnings);
    }

    #[test]
    fn ids_are_found_in_the_dependency_fields_only() {
        let text = r#"{"name":"X 2024","summary":"Needs https://steamcommunity.com/sharedfiles/filedetails/?id=1125571106.",
            "description":"12345 and 1976399102","dependencies":[{"identifier":"3622043265"}, 2858787558],
            "steamdata":{"publishedfileid":"9999999999"},"version":"1000000"}"#;
        assert_eq!(mentioned_ids(text), vec!["1125571106", "1976399102", "3622043265", "2858787558"]);
        assert!(mentioned_ids("not json").is_empty());
        assert!(mentioned_ids("\u{feff}{\"name\":\"1234567\"}") == vec!["1234567"]);
    }

    /// Walks the real Workshop folder on this PC (names only) and prints what it finds and
    /// how long it took. `cargo test live_workshop_conflicts -- --ignored --nocapture`
    #[test]
    #[ignore = "reads the installed Workshop mods"]
    fn live_workshop_conflicts() {
        let steam = crate::steam::detect();
        let Some(dir) = steam.workshop_dir.clone() else {
            println!("no Workshop folder on this PC");
            return;
        };
        let mods: Vec<ModInput> = std::fs::read_dir(&dir)
            .unwrap()
            .flatten()
            .filter(|e| e.path().is_dir())
            .map(|e| ModInput {
                name: e.file_name().to_string_lossy().into_owned(),
                path: e.path(),
                workshop_id: Some(e.file_name().to_string_lossy().into_owned()),
                present: true,
                version: String::new(),
            })
            .collect();
        let input = Input { mods, foc: steam.foc_dir.clone() };
        let mut cache = IndexCache::new();
        let start = std::time::Instant::now();
        let report = analyze(&input, &mut cache);
        println!("first run (walks every folder): {:?}", start.elapsed());
        let start = std::time::Instant::now();
        let again = analyze(&input, &mut cache);
        println!("second run (indexes cached): {:?}", start.elapsed());
        assert_eq!(again.pairs.len(), report.pairs.len());
        for (name, m) in report.names.iter().zip(&report.per_mod) {
            println!("  {name}: {} files, {} replaced from above, data={}", m.files, m.replaced_files, m.has_data);
        }
        for p in &report.pairs {
            println!(
                "  {} over {}: {} files, {} XML, {} definitions replaced, {} dropped",
                report.name(p.upper),
                report.name(p.lower),
                p.shared_files.len(),
                p.xml_files,
                p.definitions,
                p.lost_definitions
            );
        }
        for w in &report.warnings {
            println!("  {:?} {:?} order={}: {}", w.severity, w.kind, w.order, w.text);
        }
    }

    #[test]
    fn the_signature_follows_order_and_the_cache_is_reused() {
        let s = Scratch::new();
        s.write("A/Data/a.txt", "");
        s.write("B/Data/a.txt", "");
        let ab = s.input(&[("A", None), ("B", None)]);
        let ba = s.input(&[("B", None), ("A", None)]);
        assert_ne!(ab.signature(), ba.signature());
        assert_eq!(ab.signature(), s.input(&[("A", None), ("B", None)]).signature());
        let mut cache = IndexCache::new();
        assert_eq!(analyze(&ab, &mut cache).pairs.len(), 1);
        assert_eq!(cache.len(), 2);
        // The second run uses the indexes it has even though the files are gone.
        std::fs::remove_dir_all(s.0.join("B")).unwrap();
        let again = analyze(&ba, &mut cache);
        assert_eq!((again.pairs[0].upper, again.pairs[0].lower), (0, 1));
        // A mod that was updated or (re)installed is walked again.
        let mut updated = ba.clone();
        updated.mods[0].version = "2.0".into();
        assert_ne!(updated.signature(), ba.signature());
        assert!(analyze(&updated, &mut cache).pairs.is_empty());
        // A folder that isn't there is never remembered as missing.
        let mut cache = IndexCache::new();
        let gone = s.input(&[("Later", None)]);
        assert!(!analyze(&gone, &mut cache).per_mod[0].exists);
        assert!(cache.is_empty());
        s.write("Later/Data/x.txt", "");
        assert_eq!(analyze(&gone, &mut cache).per_mod[0].files, 1);
    }
}

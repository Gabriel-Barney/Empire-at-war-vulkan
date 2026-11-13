//! Index of every XML definition a profile loads, resolved the way the game resolves
//! files: mods in load order, then loose Forces of Corruption files, then MEG archives
//! (newest mounted first), then loose Empire at War files.
//!
//! Also: variant inheritance, in-place value edits with whole-file backups, and the
//! edit history.

use crate::config;
use crate::meg::{MegArchive, MegEntry};
use crate::xml::{self, Elem, Encoding};
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::Arc;

pub const VARIANT_TAG: &str = "Variant_Of_Existing_Type";
/// The field on a unit that lists its hardpoints by name.
pub const HARDPOINTS_TAG: &str = "HardPoints";
pub const HEALTH_TAG: &str = "Health";

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Layer {
    Mod(usize),
    Foc,
    Meg(usize),
    Eaw,
}

/// A layer as the Source filter groups them: every archive counts as one source.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum SourceGroup {
    Mod(usize),
    Foc,
    Archives,
    Eaw,
}

impl SourceGroup {
    pub fn of(layer: &Layer) -> SourceGroup {
        match layer {
            Layer::Mod(i) => SourceGroup::Mod(*i),
            Layer::Foc => SourceGroup::Foc,
            Layer::Meg(_) => SourceGroup::Archives,
            Layer::Eaw => SourceGroup::Eaw,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Category {
    Object,
    TradeRoute,
    Campaign,
    /// Listed by `HardPointDataFiles.xml`. Hardpoint names are a namespace of their own.
    HardPoint,
}

#[derive(Clone, Debug)]
pub struct SourceFile {
    /// e.g. `Data\XML\SpaceUnits.xml`
    pub rel: String,
    pub layer: Layer,
    /// Which list file named this one; every definition in it has this category.
    pub category: Category,
    /// Set for loose files — the only ones that can be edited.
    pub path: Option<PathBuf>,
    meg: Option<MegEntry>,
}

#[derive(Clone, Debug)]
pub struct Definition {
    pub name: String,
    /// The XML tag: `SpaceUnit`, `GroundCompany`, `Planet`…
    pub kind: String,
    pub category: Category,
    pub file: usize,
    pub span: (usize, usize),
    pub variant_of: Option<String>,
}

#[derive(Clone, Debug)]
pub struct Field {
    pub key: String,
    pub value: String,
    /// Has child elements; shown as a summary and not editable.
    pub nested: bool,
    /// n-th field with this key inside the definition.
    pub occurrence: usize,
    pub editable: bool,
}

#[derive(Clone, Debug)]
pub struct EffField {
    pub field: Field,
    /// Definition the value comes from.
    pub from: usize,
    pub inherited: bool,
}

struct Vfs {
    mods: Vec<PathBuf>,
    foc: PathBuf,
    eaw: Option<PathBuf>,
    megs: Vec<MegArchive>,
    /// Upper-cased archive path -> (archive, entry); filled so the newest mount wins.
    meg_index: HashMap<String, (usize, usize)>,
}

enum Found {
    Loose(PathBuf, Layer),
    Meg(usize, MegEntry),
}

impl Vfs {
    fn find_loose(&self, rel: &str, with_eaw: bool) -> Option<(PathBuf, Layer)> {
        for (i, m) in self.mods.iter().enumerate() {
            let p = m.join(rel);
            if p.is_file() {
                return Some((p, Layer::Mod(i)));
            }
        }
        let p = self.foc.join(rel);
        if p.is_file() {
            return Some((p, Layer::Foc));
        }
        if with_eaw {
            if let Some(eaw) = &self.eaw {
                let p = eaw.join(rel);
                if p.is_file() {
                    return Some((p, Layer::Eaw));
                }
            }
        }
        None
    }

    fn find(&self, rel: &str) -> Option<Found> {
        if let Some((p, l)) = self.find_loose(rel, false) {
            return Some(Found::Loose(p, l));
        }
        let key = rel.to_ascii_uppercase().replace('/', "\\");
        if let Some(&(a, e)) = self.meg_index.get(&key) {
            return Some(Found::Meg(a, self.megs[a].entries[e].clone()));
        }
        self.eaw.as_ref().map(|e| e.join(rel)).filter(|p| p.is_file()).map(|p| Found::Loose(p, Layer::Eaw))
    }

    fn read(&self, found: &Found) -> Result<Vec<u8>, String> {
        match found {
            Found::Loose(p, _) => std::fs::read(p).map_err(|e| format!("{}: {e}", p.display())),
            Found::Meg(a, entry) => self.megs[*a].read(entry),
        }
    }

    /// Mounts archives in the game's order; later mounts take priority.
    fn mount(&mut self, warnings: &mut Vec<String>) {
        let mut mount_order: Vec<PathBuf> = Vec::new();
        let list_megs = |megafiles: &Path| -> Vec<String> {
            std::fs::read(megafiles)
                .map(|b| {
                    let (text, _) = xml::decode(&b);
                    let mut out = Vec::new();
                    collect_files(&xml::parse(&text), &mut out);
                    out
                })
                .unwrap_or_default()
        };
        const PATCHES: [&str; 3] = ["Data\\Patch.meg", "Data\\patch2.meg", "Data\\64Patch.meg"];
        if let Some(eaw) = self.eaw.clone() {
            for name in list_megs(&eaw.join("Data\\MegaFiles.xml")).iter().map(String::as_str).chain(PATCHES) {
                let p = eaw.join(name);
                if p.is_file() {
                    mount_order.push(p);
                }
            }
        }
        // FoC's MegaFiles.xml and its archives are themselves looked up through mods first.
        if let Some((megafiles, _)) = self.find_loose("Data\\MegaFiles.xml", false) {
            for name in list_megs(&megafiles).iter().map(String::as_str).chain(PATCHES) {
                if let Some((p, _)) = self.find_loose(name, false) {
                    mount_order.push(p);
                }
            }
        }
        let mut seen: Vec<String> = Vec::new();
        for p in mount_order {
            let key = p.to_string_lossy().to_lowercase();
            if seen.contains(&key) {
                continue;
            }
            seen.push(key);
            match MegArchive::open(&p) {
                Ok(a) => self.megs.push(a),
                Err(e) => warnings.push(e),
            }
        }
        for (ai, a) in self.megs.iter().enumerate().rev() {
            for (ei, e) in a.entries.iter().enumerate() {
                self.meg_index.entry(e.name.clone()).or_insert((ai, ei));
            }
        }
    }
}

fn collect_files(elems: &[Elem], out: &mut Vec<String>) {
    for e in elems {
        if e.tag.eq_ignore_ascii_case("File") {
            let t = e.text.trim();
            if !t.is_empty() {
                out.push(t.replace('/', "\\"));
            }
        }
        collect_files(&e.children, out);
    }
}

/// The XML files a list file (`GameObjectFiles.xml` and friends) names, in order, with
/// backslashes. Paths are relative to `Data\XML`.
pub fn list_entries(text: &str) -> Vec<String> {
    let mut out = Vec::new();
    collect_files(&xml::parse(text), &mut out);
    out
}

/// Names of the definitions in one XML data file, in file order, as [`UnitDb::build`]
/// finds them.
pub fn definition_names(text: &str) -> Vec<String> {
    scan_defs(text, 0, Category::Object).into_iter().map(|d| d.name).collect()
}

#[derive(Default)]
pub struct Progress {
    pub done: AtomicUsize,
    pub total: AtomicUsize,
}

pub struct UnitDb {
    vfs: Vfs,
    pub mod_names: Vec<String>,
    pub files: Vec<SourceFile>,
    pub defs: Vec<Definition>,
    by_name: HashMap<String, usize>,
    /// Hardpoints by lower-cased name, kept apart from `by_name`.
    hp_by_name: HashMap<String, usize>,
    /// Lower-cased hardpoint name -> definitions whose effective `HardPoints` list
    /// names it. Built on first use.
    hp_users: Option<HashMap<String, Vec<usize>>>,
    pub kinds: Vec<(String, usize)>,
    pub warnings: Vec<String>,
    pub shadowed: usize,
    pub meg_count: usize,
    text_cache: Vec<(usize, Arc<String>, Encoding)>,
}

pub struct BuildInput {
    pub mods: Vec<(String, PathBuf)>,
    pub foc: PathBuf,
}

impl UnitDb {
    pub fn build(input: BuildInput, progress: &Progress) -> UnitDb {
        let eaw = input.foc.parent().map(|p| p.join("GameData")).filter(|p| p.is_dir());
        let mut warnings = Vec::new();
        let mut vfs = Vfs {
            mods: input.mods.iter().map(|(_, p)| p.clone()).collect(),
            foc: input.foc.clone(),
            eaw,
            megs: Vec::new(),
            meg_index: HashMap::new(),
        };
        vfs.mount(&mut warnings);

        let mut db = UnitDb {
            meg_count: vfs.megs.len(),
            vfs,
            mod_names: input.mods.iter().map(|(n, _)| n.clone()).collect(),
            files: Vec::new(),
            defs: Vec::new(),
            by_name: HashMap::new(),
            hp_by_name: HashMap::new(),
            hp_users: None,
            kinds: Vec::new(),
            warnings,
            shadowed: 0,
            text_cache: Vec::new(),
        };

        // The game opens each list file once (first match wins, no merging across layers).
        let lists = [
            ("GameObjectFiles.xml", Category::Object),
            ("TradeRouteFiles.xml", Category::TradeRoute),
            ("CampaignFiles.xml", Category::Campaign),
            ("HardPointDataFiles.xml", Category::HardPoint),
        ];
        let mut work: Vec<(String, Category)> = Vec::new();
        for (list, cat) in lists {
            let rel = format!("Data\\XML\\{list}");
            let Some(found) = db.vfs.find(&rel) else {
                db.warnings.push(format!("{list} wasn't found in any layer."));
                continue;
            };
            match db.vfs.read(&found) {
                Ok(bytes) => {
                    let (text, _) = xml::decode(&bytes);
                    let mut names = Vec::new();
                    collect_files(&xml::parse(&text), &mut names);
                    let mut seen: Vec<String> = Vec::new();
                    for n in names {
                        let key = n.to_lowercase();
                        if seen.contains(&key) {
                            continue;
                        }
                        seen.push(key);
                        work.push((format!("Data\\XML\\{n}"), cat));
                    }
                }
                Err(e) => db.warnings.push(e),
            }
        }
        progress.total.store(work.len(), Ordering::Relaxed);

        for (rel, cat) in work {
            progress.done.fetch_add(1, Ordering::Relaxed);
            let Some(found) = db.vfs.find(&rel) else {
                db.warnings.push(format!("{rel} is listed but doesn't exist."));
                continue;
            };
            let bytes = match db.vfs.read(&found) {
                Ok(b) => b,
                Err(e) => {
                    db.warnings.push(e);
                    continue;
                }
            };
            let file = db.files.len();
            db.files.push(match found {
                Found::Loose(p, layer) => SourceFile { rel: rel.clone(), layer, category: cat, path: Some(p), meg: None },
                Found::Meg(a, entry) => {
                    SourceFile { rel: rel.clone(), layer: Layer::Meg(a), category: cat, path: None, meg: Some(entry) }
                }
            });
            let (text, _) = xml::decode(&bytes);
            for def in scan_defs(&text, file, cat) {
                let key = def.name.to_lowercase();
                if cat == Category::HardPoint {
                    // Hardpoints: the last definition of a name wins.
                    match db.hp_by_name.get(&key).copied() {
                        Some(existing) => {
                            db.defs[existing] = def;
                            db.shadowed += 1;
                        }
                        None => {
                            db.hp_by_name.insert(key, db.defs.len());
                            db.defs.push(def);
                        }
                    }
                    continue;
                }
                match db.by_name.get(&key).copied() {
                    // Objects: first definition wins. Routes and campaigns: last wins.
                    Some(_) if cat == Category::Object => db.shadowed += 1,
                    Some(existing) if db.defs[existing].category == cat => db.defs[existing] = def,
                    Some(_) => db.shadowed += 1,
                    None => {
                        db.by_name.insert(key, db.defs.len());
                        db.defs.push(def);
                    }
                }
            }
        }

        let mut kinds: HashMap<String, usize> = HashMap::new();
        for d in &db.defs {
            *kinds.entry(d.kind.clone()).or_default() += 1;
        }
        db.kinds = kinds.into_iter().collect();
        db.kinds.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.cmp(&b.0)));
        db
    }

    /// Objects, trade routes and campaigns by name. Hardpoints are not in here.
    pub fn lookup(&self, name: &str) -> Option<usize> {
        self.by_name.get(&name.trim().to_lowercase()).copied()
    }

    pub fn lookup_hardpoint(&self, name: &str) -> Option<usize> {
        self.hp_by_name.get(&name.trim().to_lowercase()).copied()
    }

    /// Looks a name up in the namespace that definitions of `category` live in.
    pub fn lookup_in(&self, category: Category, name: &str) -> Option<usize> {
        match category {
            Category::HardPoint => self.lookup_hardpoint(name),
            _ => self.lookup(name),
        }
    }

    /// Sources that have files in the index, in priority order, with how many
    /// definitions each one supplies.
    pub fn source_groups(&self) -> Vec<(SourceGroup, usize)> {
        let mut out: Vec<(SourceGroup, usize)> = (0..self.mod_names.len())
            .map(SourceGroup::Mod)
            .chain([SourceGroup::Foc, SourceGroup::Archives, SourceGroup::Eaw])
            .filter(|g| self.files.iter().any(|f| SourceGroup::of(&f.layer) == *g))
            .map(|g| (g, 0))
            .collect();
        for d in &self.defs {
            let g = SourceGroup::of(&self.files[d.file].layer);
            if let Some(entry) = out.iter_mut().find(|(x, _)| *x == g) {
                entry.1 += 1;
            }
        }
        out
    }

    pub fn source_group_label(&self, group: SourceGroup) -> String {
        match group {
            SourceGroup::Mod(i) => self.layer_label(&Layer::Mod(i)),
            SourceGroup::Foc => self.layer_label(&Layer::Foc),
            SourceGroup::Archives => "Game archives (.meg)".into(),
            SourceGroup::Eaw => self.layer_label(&Layer::Eaw),
        }
    }

    pub fn layer_label(&self, layer: &Layer) -> String {
        match layer {
            Layer::Mod(i) => self.mod_names.get(*i).cloned().unwrap_or_else(|| "Mod".into()),
            Layer::Foc => "Forces of Corruption (loose)".into(),
            Layer::Meg(i) => self.vfs.megs[*i]
                .path
                .file_name()
                .map(|n| n.to_string_lossy().into_owned())
                .unwrap_or_else(|| "archive".into()),
            Layer::Eaw => "Empire at War (loose)".into(),
        }
    }

    pub fn file_editable(&self, file: usize) -> bool {
        self.files[file].path.is_some()
    }

    /// Reads and decodes a source file, bypassing the cache.
    fn read_source(&self, file: usize) -> Result<(String, Encoding), String> {
        let sf = &self.files[file];
        let bytes = match (&sf.path, &sf.meg, &sf.layer) {
            (Some(p), _, _) => std::fs::read(p).map_err(|e| format!("{}: {e}", p.display()))?,
            (None, Some(entry), Layer::Meg(a)) => self.vfs.megs[*a].read(entry)?,
            _ => return Err("Source file is unavailable.".into()),
        };
        Ok(xml::decode(&bytes))
    }

    fn file_text(&mut self, file: usize) -> Result<(Arc<String>, Encoding), String> {
        if let Some((_, t, e)) = self.text_cache.iter().find(|(f, _, _)| *f == file) {
            return Ok((t.clone(), *e));
        }
        let (text, enc) = self.read_source(file)?;
        let text = Arc::new(text);
        if self.text_cache.len() >= 12 {
            self.text_cache.remove(0);
        }
        self.text_cache.push((file, text.clone(), enc));
        Ok((text, enc))
    }

    /// The definition's own fields, as written in its file.
    pub fn fields(&mut self, def: usize) -> Result<Vec<Field>, String> {
        let d = self.defs[def].clone();
        let (text, enc) = self.file_text(d.file)?;
        let slice = text.get(d.span.0..d.span.1).ok_or("The file changed on disk; refresh the database.")?;
        let roots = xml::parse(slice);
        let root = roots.first().ok_or("Definition is empty.")?;
        let editable_file = self.files[d.file].path.is_some() && enc != Encoding::Utf16;
        Ok(fields_of(root, editable_file))
    }

    /// Own fields plus everything inherited through `Variant_Of_Existing_Type`.
    pub fn effective(&mut self, def: usize) -> Result<Vec<EffField>, String> {
        // Walk up to the root ancestor (guarding against cycles).
        let mut chain = vec![def];
        let mut cur = def;
        let category = self.defs[def].category;
        while let Some(parent) = self.defs[cur].variant_of.clone().and_then(|p| self.lookup_in(category, &p)) {
            if chain.contains(&parent) || chain.len() > 16 {
                break;
            }
            chain.push(parent);
            cur = parent;
        }
        let mut out: Vec<EffField> = Vec::new();
        for &idx in chain.iter().rev() {
            let own = self.fields(idx)?;
            let mut replaced: Vec<String> = Vec::new();
            for f in own {
                let key = f.key.to_lowercase();
                // Empty values and TBD are no-ops in the game: the parent's value stands.
                let noop = !f.nested && (f.value.is_empty() || f.value.eq_ignore_ascii_case("TBD"));
                if idx != def && key == VARIANT_TAG.to_lowercase() {
                    continue;
                }
                if noop && out.iter().any(|e| e.field.key.to_lowercase() == key) {
                    continue;
                }
                let entry = EffField { field: f, from: idx, inherited: idx != def };
                if !replaced.contains(&key) {
                    replaced.push(key.clone());
                    match out.iter().position(|e| e.field.key.to_lowercase() == key) {
                        Some(pos) => {
                            out.retain(|e| e.field.key.to_lowercase() != key);
                            out.insert(pos.min(out.len()), entry);
                        }
                        None => out.push(entry),
                    }
                } else {
                    // Repeated key inside one definition: keep them together.
                    let pos = out.iter().rposition(|e| e.field.key.to_lowercase() == key).map(|p| p + 1);
                    out.insert(pos.unwrap_or(out.len()), entry);
                }
            }
        }
        Ok(out)
    }

    /// Checks a proposed value against the old one. Errors block saving; warnings don't.
    pub fn validate(&self, old: &str, new: &str) -> Check {
        let new_t = new.trim();
        if new.contains('\n') && !old.contains('\n') {
            return Check::Error("This value is a single line.".into());
        }
        if new.chars().any(|c| c.is_control() && !matches!(c, '\n' | '\r' | '\t')) {
            return Check::Error("Contains a control character XML can't hold.".into());
        }
        let old_t = old.trim();
        let is_num = |s: &str| s.trim().trim_end_matches(['f', 'F']).parse::<f64>().is_ok();
        let is_bool = |s: &str| matches!(s.to_ascii_lowercase().as_str(), "yes" | "no" | "true" | "false");
        if !old_t.is_empty() && new_t.is_empty() {
            return Check::Warn("Empty: the game treats an empty tag as \"not set\".".into());
        }
        if is_bool(old_t) && !is_bool(new_t) {
            return Check::Error("Expected Yes/No or True/False, like the current value.".into());
        }
        if is_num(old_t) && !is_bool(old_t) {
            if !is_num(new_t) {
                return Check::Error("Expected a number, like the current value.".into());
            }
            if !old_t.contains('.') && new_t.contains('.') {
                return Check::Warn("The current value is a whole number; a fraction may be truncated.".into());
            }
            return Check::Ok;
        }
        let parts = |s: &str| s.split(',').map(|p| p.trim().to_owned()).filter(|p| !p.is_empty()).collect::<Vec<_>>();
        let (op, np) = (parts(old_t), parts(new_t));
        if op.len() > 1 && op.iter().all(|p| is_num(p)) {
            if !np.iter().all(|p| is_num(p)) {
                return Check::Error("Expected numbers separated by commas, like the current value.".into());
            }
            if np.len() != op.len() {
                return Check::Warn(format!("The current value has {} numbers; this has {}.", op.len(), np.len()));
            }
            return Check::Ok;
        }
        // Reference lists: if every current entry names a definition, new ones should too.
        if !op.is_empty() && op.iter().all(|p| self.lookup(p).is_some()) {
            if let Some(missing) = np.iter().find(|p| self.lookup(p).is_none()) {
                return Check::Warn(format!("No definition named \"{missing}\" is loaded by this profile."));
            }
        }
        Check::Ok
    }

    /// Builds the new file contents for one value change, without writing anything.
    pub fn preview_edit(&mut self, def: usize, key: &str, occurrence: usize, old: &str, new: &str) -> Result<EditPreview, String> {
        let d = self.defs[def].clone();
        let path = self.files[d.file].path.clone().ok_or("This definition lives inside a MEG archive and can't be edited in place.")?;
        // Always work from what's on disk right now, not from the index.
        let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
        let (text, enc) = xml::decode(&bytes);
        if enc == Encoding::Utf16 {
            return Err("UTF-16 files can't be edited in place.".into());
        }
        let fresh = scan_defs(&text, d.file, d.category);
        let target = find_def(&fresh, &d).ok_or_else(stale)?;
        let (va, vb) = value_span(&text, target, key, occurrence, old)?;
        let mut new_text = String::with_capacity(text.len() + new.len());
        new_text.push_str(&text[..va]);
        new_text.push_str(&xml::escape(new.trim()));
        new_text.push_str(&text[vb..]);
        let new_bytes = xml::encode(&new_text, enc)
            .ok_or("The new value has characters this file's encoding (Windows-1252) can't store.")?;
        Ok(EditPreview {
            def,
            path,
            def_name: d.name,
            key: key.to_owned(),
            old: old.trim().to_owned(),
            new: new.trim().to_owned(),
            diff: diff_lines(&text, &new_text),
            original: bytes,
            new_bytes,
        })
    }

    /// Backs the file up, writes the edit and records it in the history.
    pub fn apply_edit(&mut self, p: &EditPreview) -> Result<HistoryEntry, String> {
        let now = std::fs::read(&p.path).map_err(|e| format!("{}: {e}", p.path.display()))?;
        if now != p.original {
            return Err("The file changed on disk while you were reviewing. Nothing was written.".into());
        }
        let backup = write_backup(&p.path, &now)?;
        write_atomic(&p.path, &p.new_bytes)?;
        let entry = push_history(HistoryEntry {
            id: 0,
            when: chrono::Local::now().timestamp(),
            file: p.path.clone(),
            def: p.def_name.clone(),
            key: p.key.clone(),
            old: p.old.clone(),
            new: p.new.clone(),
            backup,
            restore_of: None,
        })?;
        self.reindex_file(self.defs[p.def].file);
        Ok(entry)
    }

    /// Re-reads one file after it changed so later definitions' positions stay right.
    pub fn reindex_file(&mut self, file: usize) {
        self.text_cache.retain(|(f, _, _)| *f != file);
        let Ok((text, _)) = self.file_text(file) else { return };
        let category = self.files[file].category;
        let fresh = scan_defs(&text, file, category);
        for d in self.defs.iter_mut().filter(|d| d.file == file) {
            if let Some(n) = find_def(&fresh, d) {
                d.span = n.span;
                d.variant_of = n.variant_of.clone();
            }
        }
        // A unit's hardpoint list or its parent may have changed.
        if category == Category::Object {
            self.hp_users = None;
        }
    }

    pub fn reindex_path(&mut self, path: &Path) {
        // The same file can be named by more than one list.
        for i in 0..self.files.len() {
            if self.files[i].path.as_deref() == Some(path) {
                self.reindex_file(i);
            }
        }
    }

    // -------------------------------------------------------------- batch edits

    /// Builds the new contents of every file touched by `edits`, without writing
    /// anything. All edits to one file are applied to its text in a single pass.
    pub fn preview_batch(&mut self, edits: &[BatchEdit], label: &BatchLabel) -> Result<BatchPreview, String> {
        if edits.is_empty() {
            return Err("There is nothing to change.".into());
        }
        let mut file_ids: Vec<usize> = edits.iter().map(|e| self.defs[e.def].file).collect();
        file_ids.sort_unstable();
        file_ids.dedup();

        let mut files = Vec::new();
        for file in file_ids {
            let path = self.files[file]
                .path
                .clone()
                .ok_or_else(|| format!("{} is inside a MEG archive and can't be edited in place.", self.files[file].rel))?;
            // Each file is rewritten once, from one set of edits.
            if files.iter().any(|f: &BatchFilePreview| f.path == path) {
                return Err(format!("{} is loaded through two list files; edit its values one at a time.", path.display()));
            }
            // Always work from what's on disk right now, not from the index.
            let bytes = std::fs::read(&path).map_err(|e| format!("{}: {e}", path.display()))?;
            let (text, enc) = xml::decode(&bytes);
            if enc == Encoding::Utf16 {
                return Err(format!("{}: UTF-16 files can't be edited in place.", path.display()));
            }
            let fresh = scan_defs(&text, file, self.files[file].category);

            // (start, end, replacement, old number, new number)
            let mut spans: Vec<(usize, usize, String, Option<f64>, Option<f64>)> = Vec::new();
            for e in edits.iter().filter(|e| self.defs[e.def].file == file) {
                let d = &self.defs[e.def];
                let target = find_def(&fresh, d).ok_or_else(stale)?;
                let (a, b) = value_span(&text, target, &e.key, e.occurrence, &e.old).map_err(|err| format!("{}: {err}", d.name))?;
                if spans.iter().any(|s| s.0 == a) {
                    return Err(format!("{} · {} is in the list twice.", d.name, e.key));
                }
                spans.push((a, b, xml::escape(e.new.trim()), parse_number(&e.old), parse_number(&e.new)));
            }
            // Back to front, so replacing one value doesn't move the ones before it.
            spans.sort_by_key(|s| s.0);
            if spans.windows(2).any(|w| w[0].1 > w[1].0) {
                return Err(format!("{}: two of the values overlap.", path.display()));
            }
            let mut new_text = text.clone();
            for (a, b, replacement, _, _) in spans.iter().rev() {
                new_text.replace_range(*a..*b, replacement);
            }
            if new_text == text {
                continue;
            }
            let new_bytes = xml::encode(&new_text, enc)
                .ok_or_else(|| format!("{}: a new value has characters this file's encoding (Windows-1252) can't store.", path.display()))?;

            let n = spans.len();
            let count = format!("{n} {}{}", label.noun, if n == 1 { "" } else { "s" });
            let sum = |pick: fn(&(usize, usize, String, Option<f64>, Option<f64>)) -> Option<f64>| {
                spans.iter().map(pick).sum::<Option<f64>>()
            };
            let (old, new) = match (sum(|s| s.3), sum(|s| s.4)) {
                (Some(before), Some(after)) => {
                    (format!("total {}", fmt_number(before)), format!("total {} ({}, {count})", fmt_number(after), label.summary))
                }
                _ => (count, label.summary.clone()),
            };
            files.push(BatchFilePreview {
                path,
                edits: n,
                old,
                new,
                diff: diff_lines(&text, &new_text),
                original: bytes,
                new_bytes,
            });
        }
        if files.is_empty() {
            return Err("The new values are the same as the current ones.".into());
        }
        Ok(BatchPreview { edits: files.iter().map(|f| f.edits).sum(), files, def_name: label.def.clone(), key: label.key.clone() })
    }

    /// Writes a previewed batch: every file is checked first, then each one is backed
    /// up, replaced and given one history entry. If any file changed since the preview,
    /// nothing is written.
    pub fn apply_batch(&mut self, p: &BatchPreview) -> Result<Vec<HistoryEntry>, String> {
        for f in &p.files {
            let now = std::fs::read(&f.path).map_err(|e| format!("{}: {e}", f.path.display()))?;
            if now != f.original {
                return Err(format!(
                    "{} changed on disk while you were reviewing. Nothing was written.",
                    f.path.display()
                ));
            }
        }
        // Every backup exists before the first file is touched.
        let mut backups = Vec::with_capacity(p.files.len());
        for f in &p.files {
            backups.push(write_backup(&f.path, &f.original)?);
        }
        for (i, f) in p.files.iter().enumerate() {
            if let Err(e) = write_atomic(&f.path, &f.new_bytes) {
                // Put back the files already replaced, so the batch is all or nothing.
                let failed: Vec<String> = p.files[..i]
                    .iter()
                    .filter(|done| write_atomic(&done.path, &done.original).is_err())
                    .map(|done| done.path.display().to_string())
                    .collect();
                for done in &p.files[..i] {
                    self.reindex_path(&done.path);
                }
                return Err(if failed.is_empty() {
                    format!("{e}. Nothing was changed.")
                } else {
                    format!("{e}. These files were already changed and couldn't be put back; restore them from the backups folder: {}", failed.join(", "))
                });
            }
        }
        let when = chrono::Local::now().timestamp();
        let mut entries = Vec::with_capacity(p.files.len());
        let mut history_error = None;
        for (f, backup) in p.files.iter().zip(backups) {
            let entry = HistoryEntry {
                id: 0,
                when,
                file: f.path.clone(),
                def: p.def_name.clone(),
                key: p.key.clone(),
                old: f.old.clone(),
                new: f.new.clone(),
                backup,
                restore_of: None,
            };
            match push_history(entry) {
                Ok(e) => entries.push(e),
                Err(e) => history_error = Some(e),
            }
            self.reindex_path(&f.path);
        }
        match history_error {
            Some(e) => Err(format!(
                "The files were changed, but the edit history couldn't be updated ({e}). The backups are in {}.",
                config::data_dir().join("backups").display()
            )),
            None => Ok(entries),
        }
    }

    // -------------------------------------------------------------- hardpoints

    /// The hardpoints a unit lists in its effective `HardPoints` field(s), each once,
    /// in the order they're first named, with where its Health value lives.
    pub fn hardpoints_of(&mut self, unit: usize) -> Result<Vec<HardpointRow>, String> {
        let mut names: Vec<(String, usize)> = Vec::new();
        for e in self.effective(unit)? {
            if e.field.nested || !e.field.key.eq_ignore_ascii_case(HARDPOINTS_TAG) {
                continue;
            }
            for n in split_names(&e.field.value) {
                match names.iter_mut().find(|(have, _)| have.eq_ignore_ascii_case(n)) {
                    Some(have) => have.1 += 1,
                    None => names.push((n.to_owned(), 1)),
                }
            }
        }
        let mut out = Vec::with_capacity(names.len());
        for (name, listed) in names {
            let mut row = HardpointRow {
                name,
                listed,
                def: None,
                key: HEALTH_TAG.to_owned(),
                occurrence: 0,
                health: None,
                health_value: None,
                inherited_from: None,
                source: String::new(),
                state: HardpointState::NotFound,
            };
            let Some(def) = self.lookup_hardpoint(&row.name) else {
                out.push(row);
                continue;
            };
            let file = self.defs[def].file;
            row.def = Some(def);
            row.source = self.layer_label(&self.files[file].layer);
            // Hardpoints can be variants too; the last Health in the chain is the one in force.
            let health = self.effective(def)?.into_iter().rev().find(|e| e.field.key.eq_ignore_ascii_case(HEALTH_TAG));
            row.state = match health {
                Some(h) if !h.field.nested && !h.field.value.is_empty() => {
                    row.key = h.field.key.clone();
                    row.occurrence = h.field.occurrence;
                    row.health_value = parse_number(&h.field.value);
                    row.health = Some(h.field.value);
                    if h.inherited {
                        let parent = self.defs[h.from].name.clone();
                        row.inherited_from = Some(parent.clone());
                        HardpointState::Inherited(parent)
                    } else if self.files[file].path.is_none() {
                        HardpointState::InArchive
                    } else if !h.field.editable {
                        HardpointState::Locked("its Health value can't be edited in place".into())
                    } else if row.health_value.is_none() {
                        HardpointState::Locked("its Health value isn't a number".into())
                    } else {
                        HardpointState::Editable
                    }
                }
                _ => HardpointState::NoHealth,
            };
            out.push(row);
        }
        Ok(out)
    }

    /// Definitions whose effective `HardPoints` list (their own, or the nearest one
    /// inherited through `Variant_Of_Existing_Type`) names this hardpoint.
    pub fn hardpoint_users(&mut self, hardpoint: &str) -> Vec<usize> {
        if self.hp_users.is_none() {
            self.hp_users = Some(self.build_hardpoint_users());
        }
        self.hp_users
            .as_ref()
            .and_then(|m| m.get(&hardpoint.trim().to_lowercase()))
            .cloned()
            .unwrap_or_default()
    }

    fn build_hardpoint_users(&self) -> HashMap<String, Vec<usize>> {
        // Each object's own list first. Files are read once each and only definitions
        // that mention the tag are parsed.
        let mut own: Vec<Option<Vec<String>>> = vec![None; self.defs.len()];
        let mut by_file: Vec<Vec<usize>> = vec![Vec::new(); self.files.len()];
        for (i, d) in self.defs.iter().enumerate() {
            if d.category == Category::Object {
                by_file[d.file].push(i);
            }
        }
        let needle = format!("<{HARDPOINTS_TAG}").into_bytes();
        for (file, defs) in by_file.iter().enumerate() {
            if defs.is_empty() {
                continue;
            }
            let Ok((text, _)) = self.read_source(file) else { continue };
            for &i in defs {
                let d = &self.defs[i];
                let Some(slice) = text.get(d.span.0..d.span.1) else { continue };
                if !slice.as_bytes().windows(needle.len()).any(|w| w.eq_ignore_ascii_case(&needle)) {
                    continue;
                }
                let roots = xml::parse(slice);
                let Some(root) = roots.first() else { continue };
                let mut names: Vec<String> = Vec::new();
                for c in root.children.iter().filter(|c| c.children.is_empty() && c.tag.eq_ignore_ascii_case(HARDPOINTS_TAG)) {
                    for n in split_names(&c.text) {
                        let n = n.to_lowercase();
                        if !names.contains(&n) {
                            names.push(n);
                        }
                    }
                }
                if !names.is_empty() {
                    own[i] = Some(names);
                }
            }
        }
        let mut users: HashMap<String, Vec<usize>> = HashMap::new();
        for (i, d) in self.defs.iter().enumerate() {
            if d.category != Category::Object {
                continue;
            }
            // A variant without a list of its own uses its parent's.
            let (mut cur, mut hops) = (i, 0);
            while own[cur].is_none() && hops < 16 {
                match self.defs[cur].variant_of.as_deref().and_then(|p| self.lookup(p)) {
                    Some(parent) if parent != i => cur = parent,
                    _ => break,
                }
                hops += 1;
            }
            for n in own[cur].iter().flatten() {
                users.entry(n.clone()).or_default().push(i);
            }
        }
        users
    }
}

/// Names in a `HardPoints` value: separated by commas and/or whitespace.
fn split_names(value: &str) -> impl Iterator<Item = &str> {
    value.split(|c: char| c == ',' || c.is_whitespace()).filter(|n| !n.is_empty())
}

/// A number as the game's XML writes them; a trailing `f` is allowed.
pub fn parse_number(s: &str) -> Option<f64> {
    let t = s.trim();
    let t = t.strip_suffix(['f', 'F']).unwrap_or(t);
    if t.is_empty() || !t.bytes().all(|b| b.is_ascii_digit() || matches!(b, b'.' | b'-' | b'+')) {
        return None;
    }
    t.parse::<f64>().ok().filter(|v| v.is_finite())
}

/// Writes `value` the way `original` was written: whole numbers stay whole (rounded
/// to nearest, never below 1) and decimals keep the number of places they had.
pub fn format_like(original: &str, value: f64) -> String {
    let t = original.trim();
    let (body, suffix) = match t.strip_suffix(['f', 'F']) {
        Some(b) => (b, &t[b.len()..]),
        None => (t, ""),
    };
    match body.split_once('.') {
        None => format!("{}{suffix}", value.round().max(1.0) as i64),
        Some((_, frac)) => {
            let places = frac.len().clamp(1, 6);
            let smallest = 10f64.powi(-(places as i32));
            format!("{:.*}{suffix}", places, value.max(smallest))
        }
    }
}

/// A total for display: whole when it is whole, otherwise two decimals at most.
pub fn fmt_number(v: f64) -> String {
    if (v - v.round()).abs() < 1e-6 {
        format!("{}", v.round() as i64)
    } else {
        let s = format!("{v:.2}");
        s.trim_end_matches('0').trim_end_matches('.').to_owned()
    }
}

/// One value change inside a batch.
#[derive(Clone, Debug)]
pub struct BatchEdit {
    pub def: usize,
    pub key: String,
    pub occurrence: usize,
    pub old: String,
    pub new: String,
}

/// How a batch is described in the edit history: one entry per file, titled
/// "`def` · `key`", reading e.g. "total 900 → total 1125 (`summary`, 9 `noun`s)".
#[derive(Clone, Debug)]
pub struct BatchLabel {
    pub def: String,
    pub key: String,
    /// e.g. "×1.25"
    pub summary: String,
    /// Singular, e.g. "hardpoint".
    pub noun: String,
}

pub struct BatchFilePreview {
    pub path: PathBuf,
    /// How many values change in this file.
    pub edits: usize,
    /// History summary of the values before and after.
    pub old: String,
    pub new: String,
    pub diff: Vec<DiffLine>,
    original: Vec<u8>,
    new_bytes: Vec<u8>,
}

pub struct BatchPreview {
    pub files: Vec<BatchFilePreview>,
    /// Values changed across all files.
    pub edits: usize,
    pub def_name: String,
    pub key: String,
}

#[derive(Clone, Debug, PartialEq)]
pub enum HardpointState {
    /// Its own Health value is in a loose file and can be changed in place.
    Editable,
    /// No loaded hardpoint file defines this name.
    NotFound,
    NoHealth,
    /// The value comes from this parent hardpoint.
    Inherited(String),
    InArchive,
    /// Any other reason, phrased to follow "can't be changed because".
    Locked(String),
}

#[derive(Clone, Debug)]
pub struct HardpointRow {
    pub name: String,
    /// How many times the unit lists it.
    pub listed: usize,
    /// The hardpoint's definition; `None` when it isn't defined anywhere.
    pub def: Option<usize>,
    /// The Health tag as written, and which one if there are several.
    pub key: String,
    pub occurrence: usize,
    /// Health in force, as shown in Details (own or inherited).
    pub health: Option<String>,
    pub health_value: Option<f64>,
    /// Set when Health comes from a parent hardpoint rather than its own field.
    pub inherited_from: Option<String>,
    /// Layer label of the hardpoint's file; empty when not found.
    pub source: String,
    pub state: HardpointState,
}

impl HardpointRow {
    pub fn editable(&self) -> bool {
        self.state == HardpointState::Editable
    }
}

fn stale() -> String {
    "The file changed on disk since it was indexed. Refresh the database and try again.".to_owned()
}

/// Finds the indexed definition `d` in a fresh scan of its file. Where one file defines
/// a name twice, objects use the first and everything else the last, as in `build`.
fn find_def<'a>(fresh: &'a [Definition], d: &Definition) -> Option<&'a Definition> {
    let same = |x: &&Definition| x.name.eq_ignore_ascii_case(&d.name) && x.kind == d.kind;
    if d.category == Category::Object {
        fresh.iter().find(same)
    } else {
        fresh.iter().rev().find(same)
    }
}

/// Byte range in `text` of one field's value inside `target`, without the whitespace
/// around it. Fails if the value isn't `old` any more.
fn value_span(text: &str, target: &Definition, key: &str, occurrence: usize, old: &str) -> Result<(usize, usize), String> {
    let slice = text.get(target.span.0..target.span.1).ok_or_else(stale)?;
    let roots = xml::parse(slice);
    let root = roots.first().ok_or_else(stale)?;
    let elem = root
        .children
        .iter()
        .filter(|c| c.tag.eq_ignore_ascii_case(key))
        .nth(occurrence)
        .ok_or_else(stale)?;
    if !elem.children.is_empty() || elem.self_closing {
        return Err("This value can't be edited in place.".into());
    }
    // Values are shown with runs of whitespace collapsed, so compare them the same way.
    let squash = |s: &str| s.split_whitespace().collect::<Vec<_>>().join(" ");
    if squash(&elem.text) != squash(old) {
        return Err(stale());
    }
    // Only the value is replaced, keeping the whitespace around it.
    let (a, b) = (target.span.0 + elem.inner.0, target.span.0 + elem.inner.1);
    let inner = &text[a..b];
    let lead = inner.len() - inner.trim_start().len();
    let trail = inner.len() - inner.trim_end().len();
    Ok(if inner.trim().is_empty() { (a, b) } else { (a + lead, b - trail) })
}

fn scan_defs(text: &str, file: usize, category: Category) -> Vec<Definition> {
    let mut out = Vec::new();
    for root in xml::parse(text) {
        for e in &root.children {
            let Some(name) = e.attr("Name").map(str::trim).filter(|n| !n.is_empty()) else { continue };
            out.push(Definition {
                name: name.to_owned(),
                kind: e.tag.clone(),
                category,
                file,
                span: e.span,
                variant_of: e.child_text(VARIANT_TAG).map(str::trim).filter(|v| !v.is_empty()).map(str::to_owned),
            });
        }
    }
    out
}

fn fields_of(root: &Elem, editable_file: bool) -> Vec<Field> {
    let mut counts: HashMap<String, usize> = HashMap::new();
    root.children
        .iter()
        .map(|c| {
            let n = counts.entry(c.tag.to_lowercase()).or_default();
            let occurrence = *n;
            *n += 1;
            let nested = !c.children.is_empty();
            let value = if nested {
                let mut parts: Vec<String> = Vec::new();
                for g in c.children.iter().take(6) {
                    parts.push(if g.text.is_empty() { g.tag.clone() } else { format!("{}: {}", g.tag, g.text) });
                }
                if c.children.len() > 6 {
                    parts.push(format!("… {} more", c.children.len() - 6));
                }
                parts.join("  ·  ")
            } else {
                c.text.split_whitespace().collect::<Vec<_>>().join(" ")
            };
            // Multi-line values are shown flattened, so only single-line ones round-trip.
            let single_line = !c.text.contains('\n');
            Field {
                key: c.tag.clone(),
                value,
                nested,
                occurrence,
                editable: editable_file && !nested && !c.self_closing && single_line,
            }
        })
        .collect()
}

#[derive(Clone, Debug, PartialEq)]
pub enum Check {
    Ok,
    Warn(String),
    Error(String),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiffKind {
    Same,
    Removed,
    Added,
    Gap,
}

#[derive(Clone, Debug)]
pub struct DiffLine {
    pub kind: DiffKind,
    pub line_no: Option<usize>,
    pub text: String,
}

pub struct EditPreview {
    pub def: usize,
    pub path: PathBuf,
    pub def_name: String,
    pub key: String,
    pub old: String,
    pub new: String,
    pub diff: Vec<DiffLine>,
    original: Vec<u8>,
    new_bytes: Vec<u8>,
}

fn diff_lines(old: &str, new: &str) -> Vec<DiffLine> {
    let diff = similar::TextDiff::from_lines(old, new);
    let mut out = Vec::new();
    for (i, group) in diff.grouped_ops(3).iter().enumerate() {
        if i > 0 {
            out.push(DiffLine { kind: DiffKind::Gap, line_no: None, text: String::new() });
        }
        for op in group {
            for change in diff.iter_changes(op) {
                let (kind, line_no) = match change.tag() {
                    similar::ChangeTag::Equal => (DiffKind::Same, change.new_index()),
                    similar::ChangeTag::Delete => (DiffKind::Removed, change.old_index()),
                    similar::ChangeTag::Insert => (DiffKind::Added, change.new_index()),
                };
                out.push(DiffLine {
                    kind,
                    line_no: line_no.map(|n| n + 1),
                    text: change.value().trim_end_matches(['\r', '\n']).replace('\t', "    "),
                });
            }
        }
    }
    out
}

// ------------------------------------------------------------------ backups & history

#[derive(Serialize, Deserialize, Clone, Debug)]
pub struct HistoryEntry {
    pub id: u64,
    /// Unix seconds.
    pub when: i64,
    pub file: PathBuf,
    pub def: String,
    pub key: String,
    pub old: String,
    pub new: String,
    /// Copy of the whole file as it was just before this entry's change.
    pub backup: PathBuf,
    /// Set when this entry is a restore of another entry's backup.
    #[serde(default)]
    pub restore_of: Option<u64>,
}

fn history_path() -> PathBuf {
    config::data_dir().join("edit_history.json")
}

pub fn load_history() -> Vec<HistoryEntry> {
    std::fs::read_to_string(history_path())
        .ok()
        .and_then(|t| serde_json::from_str(&t).ok())
        .unwrap_or_default()
}

fn push_history(mut e: HistoryEntry) -> Result<HistoryEntry, String> {
    let mut all = load_history();
    e.id = all.iter().map(|h| h.id).max().unwrap_or(0) + 1;
    all.push(e.clone());
    let text = serde_json::to_string_pretty(&all).map_err(|e| e.to_string())?;
    write_atomic(&history_path(), text.as_bytes())?;
    Ok(e)
}

fn write_backup(original: &Path, bytes: &[u8]) -> Result<PathBuf, String> {
    let dir = config::data_dir().join("backups");
    std::fs::create_dir_all(&dir).map_err(|e| e.to_string())?;
    let name = original.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| "file.xml".into());
    let stamp = chrono::Local::now().format("%Y%m%d-%H%M%S%3f");
    let mut path = dir.join(format!("{stamp}_{name}"));
    let mut n = 1;
    while path.exists() {
        n += 1;
        path = dir.join(format!("{stamp}-{n}_{name}"));
    }
    std::fs::write(&path, bytes).map_err(|e| format!("Couldn't write the backup {}: {e}", path.display()))?;
    // A backup that doesn't read back identically isn't a backup.
    if std::fs::read(&path).map(|b| b != bytes).unwrap_or(true) {
        return Err("The backup didn't read back correctly; nothing was changed.".into());
    }
    Ok(path)
}

fn write_atomic(path: &Path, bytes: &[u8]) -> Result<(), String> {
    let mut tmp = path.as_os_str().to_owned();
    tmp.push(".cc-tmp");
    let tmp = PathBuf::from(tmp);
    std::fs::write(&tmp, bytes).map_err(|e| format!("Couldn't write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, path).map_err(|e| {
        let _ = std::fs::remove_file(&tmp);
        format!("Couldn't replace {}: {e}", path.display())
    })
}

/// Puts the file back to how it was before `entry`. The current contents are backed up
/// first and recorded as a new history entry, so the restore can itself be undone.
pub fn restore(entry: &HistoryEntry) -> Result<HistoryEntry, String> {
    let wanted = std::fs::read(&entry.backup)
        .map_err(|e| format!("The backup {} can't be read: {e}", entry.backup.display()))?;
    let current = std::fs::read(&entry.file).map_err(|e| format!("{}: {e}", entry.file.display()))?;
    if current == wanted {
        return Err("The file already matches this backup.".into());
    }
    let backup = write_backup(&entry.file, &current)?;
    write_atomic(&entry.file, &wanted)?;
    push_history(HistoryEntry {
        id: 0,
        when: chrono::Local::now().timestamp(),
        file: entry.file.clone(),
        def: entry.def.clone(),
        key: entry.key.clone(),
        old: String::new(),
        new: String::new(),
        backup,
        restore_of: Some(entry.id),
    })
}

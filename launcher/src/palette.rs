//! Ctrl+K command palette: one search box over commands, profiles, mods, saves and unit
//! definitions.
//!
//! Everything that can be found is gathered once when the palette opens ([`collect`]);
//! typing only re-ranks that list ([`search`]), and only when the text changed.

use crate::app::{open_in_explorer, take_shortcut, App, Page};
use crate::steam::same_path;
use crate::theme::{self, icon};
use crate::{config, pages, runner, saves};
use egui::{
    pos2, vec2, Align, Align2, Color32, CornerRadius, FontFamily, FontId, Id, Key, Layout, Modifiers, Rect, RichText,
    Sense, Stroke, ViewportId,
};
use std::collections::BTreeMap;
use std::path::PathBuf;

const WIDTH: f32 = 620.0;
const ROW_H: f32 = 40.0;
/// Most results shown per kind.
const PER_GROUP: usize = 8;

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
pub enum Kind {
    Command,
    Profile,
    Mod,
    Save,
    Unit,
}

impl Kind {
    fn heading(self) -> &'static str {
        match self {
            Kind::Command => "Commands",
            Kind::Profile => "Profiles",
            Kind::Mod => "Mods",
            Kind::Save => "Saves",
            Kind::Unit => "Unit definitions",
        }
    }

    fn chip(self) -> (&'static str, Color32) {
        match self {
            Kind::Command => ("Command", theme::ACCENT_HI),
            Kind::Profile => ("Profile", theme::GOOD),
            Kind::Mod => ("Mod", theme::WARN),
            Kind::Save => ("Save", Color32::from_rgb(196, 150, 255)),
            Kind::Unit => ("Unit", theme::MUTED),
        }
    }
}

#[derive(Clone, Debug, PartialEq)]
enum Action {
    Launch,
    Stop,
    Go(Page),
    ToggleSplit,
    OwnWindow,
    CloseTab,
    Diagnostics,
    Checkpoint,
    OpenLogs,
    OpenData,
    OpenSaves,
    RescanMods,
    Profile(u64),
    ShowMod(PathBuf),
    AddMod(PathBuf),
    RemoveMod(PathBuf),
    ShowSaves,
    ViewMap(PathBuf),
    Unit(String),
}

/// One thing the palette can find.
#[derive(Clone, Debug)]
struct Entry {
    kind: Kind,
    title: String,
    /// Shown after the title; not searched.
    detail: String,
    /// Searched as well as the title, but counts for less.
    keywords: String,
    glyph: &'static str,
    /// Keyboard shortcut that does the same thing, if there is one.
    shortcut: String,
    action: Action,
    /// Second action (Shift+Enter): its label and what it does.
    alt: Option<(&'static str, Action)>,
}

impl Entry {
    fn new(kind: Kind, glyph: &'static str, title: impl Into<String>, detail: impl Into<String>, action: Action) -> Self {
        Self {
            kind,
            title: title.into(),
            detail: detail.into(),
            keywords: String::new(),
            glyph,
            shortcut: String::new(),
            action,
            alt: None,
        }
    }

    fn keywords(mut self, words: impl Into<String>) -> Self {
        self.keywords = words.into();
        self
    }

    fn shortcut(mut self, keys: impl Into<String>) -> Self {
        self.shortcut = keys.into();
        self
    }

    fn alt(mut self, label: &'static str, action: Action) -> Self {
        self.alt = Some((label, action));
        self
    }

    fn score(&self, query: &str) -> Option<i32> {
        let title = match_score(query, &self.title);
        if self.keywords.is_empty() {
            return title;
        }
        // A hit in the keywords is worth less than the same hit in the title.
        let extra = match_score(query, &format!("{} {}", self.title, self.keywords)).map(|s| s - 250);
        title.max(extra)
    }
}

// ------------------------------------------------------------------ matching

/// Where `token` starts at the beginning of a word of `text`: after a space or punctuation.
fn word_start(text: &str, token: &str) -> Option<usize> {
    text.match_indices(token).map(|(i, _)| i).find(|&i| text[..i].chars().next_back().is_none_or(|c| !c.is_alphanumeric()))
}

/// The first letter of every word of `text`.
fn initials(text: &str) -> String {
    text.split(|c: char| !c.is_alphanumeric()).filter_map(|word| word.chars().next()).collect()
}

/// The letters of `token` in order but not together ("thrw" in "thrawn's revenge"). The
/// first letter must start a word and the match can't be spread too thin.
fn fuzzy(token: &str, text: &str) -> Option<i32> {
    let wanted: Vec<char> = token.chars().collect();
    if wanted.len() < 3 {
        return None;
    }
    let chars: Vec<char> = text.chars().collect();
    let mut best: Option<usize> = None;
    for start in 0..chars.len() {
        if chars[start] != wanted[0] || (start > 0 && chars[start - 1].is_alphanumeric()) {
            continue;
        }
        let mut next = 1;
        let mut end = start;
        for (i, c) in chars.iter().enumerate().skip(start + 1) {
            if next == wanted.len() {
                break;
            }
            if *c == wanted[next] {
                next += 1;
                end = i;
            }
        }
        if next == wanted.len() {
            let span = end - start + 1;
            best = Some(best.map_or(span, |b| b.min(span)));
        }
    }
    let span = best?;
    let gaps = span - wanted.len();
    (span <= wanted.len() * 3).then(|| (300 - gaps as i32 * 10).max(120))
}

/// One word of the query against lower-cased text.
fn token_score(token: &str, text: &str) -> Option<i32> {
    let tier = if text == token {
        1000
    } else if text.starts_with(token) {
        900
    } else if let Some(at) = word_start(text, token) {
        // An earlier word is a slightly better hit than a later one.
        700 - (text[..at].split_whitespace().count().min(5) as i32) * 10
    } else if token.chars().count() >= 2 && initials(text).starts_with(token) {
        // "tfs" for "TIE Fighter Squadron".
        600
    } else if text.contains(token) {
        500
    } else {
        fuzzy(token, text)?
    };
    // Between equal hits the shorter text is the closer one.
    Some(tier - text.chars().count().min(80) as i32 / 4)
}

/// How well `text` answers `query`: exact > prefix > start of a word > initials > anywhere
/// > letters in order. Several words must all be found, in any order. `None` = no match.
pub fn match_score(query: &str, text: &str) -> Option<i32> {
    let query = query.trim().to_lowercase();
    let text = text.to_lowercase();
    if query.is_empty() {
        return Some(0);
    }
    let whole = token_score(&query, &text);
    let words: Vec<&str> = query.split_whitespace().collect();
    if words.len() < 2 {
        return whole;
    }
    let mut total = 0;
    for word in &words {
        total += token_score(word, &text)?;
    }
    // Words scattered about rank a little below the same words together.
    let scattered = total / words.len() as i32 - 30;
    Some(whole.map_or(scattered, |w| w.max(scattered)))
}

/// Ranks `entries` against `query`: grouped by kind, the kind with the best hit first, at
/// most [`PER_GROUP`] per kind, best first (ties keep the order the entries came in). An
/// empty query lists the commands and the profiles.
fn search(query: &str, entries: &[Entry]) -> Vec<(Kind, Vec<usize>)> {
    let query = query.trim();
    let mut found: BTreeMap<Kind, Vec<(i32, usize)>> = BTreeMap::new();
    for (index, entry) in entries.iter().enumerate() {
        let score = if query.is_empty() {
            matches!(entry.kind, Kind::Command | Kind::Profile).then_some(0)
        } else {
            entry.score(query)
        };
        if let Some(score) = score {
            found.entry(entry.kind).or_default().push((score, index));
        }
    }
    let mut groups: Vec<(i32, Kind, Vec<usize>)> = found
        .into_iter()
        .map(|(kind, mut hits)| {
            hits.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
            let best = hits[0].0;
            (best, kind, hits.into_iter().take(PER_GROUP).map(|(_, i)| i).collect())
        })
        .collect();
    groups.sort_by(|a, b| b.0.cmp(&a.0).then(a.1.cmp(&b.1)));
    groups.into_iter().map(|(_, kind, hits)| (kind, hits)).collect()
}

// ------------------------------------------------------------------ what can be found

fn plural(n: usize, one: &str, many: &str) -> String {
    if n == 1 {
        format!("1 {one}")
    } else {
        format!("{n} {many}")
    }
}

/// Everything the palette can find right now. Only commands that can actually run are
/// listed (no "Stop game" while nothing is running, and so on).
fn collect(app: &App) -> Vec<Entry> {
    let mut out = Vec::new();
    let command = |glyph: &'static str, title: &str, detail: &str, action: Action| {
        Entry::new(Kind::Command, glyph, title, detail, action)
    };
    let profile = app.cfg.active();

    // ---- commands, the most useful first (this is the order an empty search shows)
    if app.runner.is_running() {
        out.push(command(icon::STOP, "Stop game", "End the running game now", Action::Stop).keywords("quit kill exit close"));
    } else {
        let detail = format!("Start Empire at War with \"{}\"", profile.name);
        out.push(command(icon::PLAY, "Launch game", &detail, Action::Launch).keywords("play start run").shortcut("Ctrl+Enter"));
    }
    let saves_dir = app.saves_dir();
    if saves_dir.as_ref().is_some_and(|d| d.is_dir()) {
        out.push(
            command(icon::HISTORY, "Create checkpoint", "Copy and verify this profile's saves now", Action::Checkpoint)
                .keywords("backup snapshot saves"),
        );
    }
    out.push(
        command(icon::SHIELD, "Run diagnostics", "Check the game, the mods and the graphics driver", Action::Diagnostics)
            .keywords("check problems preflight troubleshoot"),
    );
    if app.layout.is_split() {
        out.push(command(icon::COMPARE, "Close split view", "Back to one page at a time", Action::ToggleSplit).keywords("side by side merge panes"));
    } else if app.layout.main_tabs() > 1 {
        out.push(command(icon::COMPARE, "Split view", "Show the current tab on the right", Action::ToggleSplit).keywords("side by side two panes"));
    }
    let current = app.layout.focused_page();
    let detail = format!("Move {} out of the main window", current.label());
    out.push(command(icon::POP_OUT, "Open page in its own window", &detail, Action::OwnWindow).keywords("pop out detach separate new window"));
    if app.layout.main_tabs() > 1 {
        let detail = format!("Close {}", current.label());
        out.push(command(icon::CLOSE, "Close tab", &detail, Action::CloseTab).shortcut("Ctrl+W"));
    }
    out.push(command(icon::FOLDER_OPEN, "Open logs folder", "Where each game session's output is kept", Action::OpenLogs).keywords("log console output"));
    out.push(command(icon::FOLDER_OPEN, "Open data folder", "The launcher's own settings and data", Action::OpenData).keywords("config settings appdata"));
    if saves_dir.as_ref().is_some_and(|d| d.is_dir()) {
        out.push(command(icon::FOLDER_OPEN, "Open save folder", "Where this profile's saves are", Action::OpenSaves).keywords("saves alsv"));
    }
    out.push(command(icon::REFRESH, "Rescan mods", "Look for mods on disk again", Action::RescanMods).keywords("refresh reload detect"));
    for (i, page) in Page::ALL.into_iter().enumerate() {
        out.push(
            command(page.glyph(), page.label(), "Go to this page", Action::Go(page))
                .keywords(format!("go open page {}", page.key()))
                .shortcut(format!("Ctrl+{}", i + 1)),
        );
    }

    // ---- profiles
    for p in &app.cfg.profiles {
        let mods = match p.mods.len() {
            0 => "no mods".to_owned(),
            n => plural(n, "mod", "mods"),
        };
        out.push(if p.id == profile.id {
            Entry::new(Kind::Profile, icon::PEOPLE, &p.name, format!("Active profile \u{b7} {mods}"), Action::Go(Page::Profiles))
        } else {
            Entry::new(Kind::Profile, icon::PEOPLE, &p.name, format!("Switch to this profile \u{b7} {mods}"), Action::Profile(p.id))
        });
    }

    // ---- mods
    for m in &app.mods {
        let in_profile = profile.mods.iter().any(|p| same_path(p, &m.path));
        let detail = if in_profile {
            format!("{} \u{b7} in \"{}\"", m.source.label(), profile.name)
        } else {
            m.source.label().to_owned()
        };
        let entry = Entry::new(Kind::Mod, icon::PUZZLE, app.mod_name(&m.path), detail, Action::ShowMod(m.path.clone()))
            .keywords(format!("{} {} {}", m.name, m.folder, m.workshop_id.as_deref().unwrap_or("")));
        out.push(if in_profile {
            entry.alt("Remove from profile", Action::RemoveMod(m.path.clone()))
        } else {
            entry.alt("Add to profile", Action::AddMod(m.path.clone()))
        });
    }

    // ---- saves of the active profile
    if let Some(dir) = saves_dir.filter(|d| d.is_dir()) {
        for s in saves::list(&dir) {
            let readable = s.error.is_none();
            let mut parts = Vec::new();
            if readable {
                parts.push(s.display_title());
                if !s.faction.is_empty() {
                    parts.push(crate::galaxy::pretty(&s.faction));
                }
                if s.is_campaign() {
                    parts.push(format!("Week {}", s.week));
                }
            } else {
                parts.push("Can't be read".to_owned());
            }
            parts.push(theme::fmt_time(s.when()));
            let entry = Entry::new(Kind::Save, icon::SAVE, &s.name, parts.join(" \u{b7} "), Action::ShowSaves)
                .keywords(format!("{} {}", s.display_title(), s.faction));
            out.push(if readable && s.is_campaign() { entry.alt("View map", Action::ViewMap(s.path.clone())) } else { entry });
        }
    }

    // ---- unit definitions, once the Unit database has an index
    for (name, kind) in app.units_ui.definition_names() {
        out.push(Entry::new(Kind::Unit, icon::DATABASE, name, kind, Action::Unit(name.to_owned())));
    }
    out
}

// ------------------------------------------------------------------ doing it

fn run(app: &mut App, ctx: &egui::Context, action: Action) {
    match action {
        Action::Launch => app.launch_or_explain(ctx),
        Action::Stop => app.runner.stop(),
        Action::Go(page) => app.navigate(page),
        Action::ToggleSplit => app.toggle_split(),
        Action::OwnWindow => app.open_in_window(app.layout.focused_page()),
        Action::CloseTab => app.close_tab(app.layout.focused_page()),
        Action::Diagnostics => {
            // The results show in the Play page's diagnostics panel.
            app.navigate(Page::Play);
            pages::play::run_diagnostics(app, ctx);
        }
        Action::Checkpoint => match app.saves_dir() {
            _ if app.runner.is_running() => {
                app.err("The game is running and may be writing saves. Close it first.");
            }
            None => app.err("The save folder couldn't be worked out."),
            Some(dir) => match saves::create_checkpoint(&dir, "", false) {
                Ok(cp) => {
                    app.saves_ui.refresh = true;
                    app.ok(format!("Checkpoint created and verified ({}).", theme::fmt_bytes(cp.total_size())));
                }
                Err(e) => app.err(e),
            },
        },
        Action::OpenLogs => open_in_explorer(&runner::logs_dir()),
        Action::OpenData => open_in_explorer(&config::data_dir()),
        Action::OpenSaves => match app.saves_dir().filter(|d| d.is_dir()) {
            Some(dir) => open_in_explorer(&dir),
            None => app.err("The save folder doesn't exist yet. It's created the first time the game saves."),
        },
        Action::RescanMods => {
            app.rescan_mods();
            let found = plural(app.mods.len(), "mod", "mods");
            app.ok(format!("Found {found}"));
        }
        Action::Profile(id) => {
            if let Some(name) = app.cfg.profiles.iter().find(|p| p.id == id).map(|p| p.name.clone()) {
                app.cfg.active_profile = id;
                app.touch();
                app.ok(format!("\"{name}\" is now the active profile."));
            }
        }
        Action::ShowMod(path) => {
            let name = app.mod_name(&path);
            app.mods_ui.focus_mod(&name);
            app.navigate(Page::Mods);
        }
        Action::AddMod(path) => {
            let (name, profile) = (app.mod_name(&path), app.cfg.active().name.clone());
            if !path.is_dir() {
                app.err(format!("Folder no longer exists: {}", path.display()));
            } else if app.cfg.active().mods.iter().any(|p| same_path(p, &path)) {
                app.ok(format!("{name} is already in \"{profile}\"."));
            } else {
                app.cfg.active_mut().mods.push(path);
                app.touch();
                app.ok(format!("Added {name} to \"{profile}\"."));
            }
        }
        Action::RemoveMod(path) => {
            let (name, profile) = (app.mod_name(&path), app.cfg.active().name.clone());
            app.cfg.active_mut().mods.retain(|p| !same_path(p, &path));
            app.touch();
            app.ok(format!("Removed {name} from \"{profile}\"."));
        }
        Action::ShowSaves => app.navigate(Page::Saves),
        Action::ViewMap(path) => {
            app.saves_ui.open_map(&path);
            app.navigate(Page::Saves);
        }
        Action::Unit(name) => {
            app.units_ui.select_by_name(&name);
            app.navigate(Page::Units);
        }
    }
}

// ------------------------------------------------------------------ the panel

/// Keys pressed since the palette was last drawn. They are taken out of the input at the
/// start of the frame so that nothing underneath reacts to them.
#[derive(Default)]
struct Keys {
    down: usize,
    up: usize,
    enter: bool,
    alt_enter: bool,
    close: bool,
}

#[derive(Default)]
pub struct Palette {
    open: bool,
    /// The window it was opened in (the main window or a page window).
    viewport: ViewportId,
    query: String,
    entries: Vec<Entry>,
    collected: bool,
    /// The query `groups` was worked out for.
    searched: Option<String>,
    groups: Vec<(Kind, Vec<usize>)>,
    count: usize,
    selected: usize,
    scroll_to_selected: bool,
    keys: Keys,
}

impl Palette {
    pub fn is_open(&self) -> bool {
        self.open
    }

    pub fn is_open_in(&self, viewport: ViewportId) -> bool {
        self.open && self.viewport == viewport
    }

    /// Opens the palette, empty, in the given window.
    pub fn open(&mut self, viewport: ViewportId) {
        *self = Palette { open: true, viewport, ..Default::default() };
    }

    pub fn close(&mut self) {
        *self = Palette::default();
    }

    /// Takes the palette's keys (arrows, Enter, Esc, Ctrl+K) out of this frame's input.
    pub fn take_keys(&mut self, ctx: &egui::Context) {
        let keys = &mut self.keys;
        ctx.input_mut(|i| {
            keys.down += i.count_and_consume_key(Modifiers::NONE, Key::ArrowDown);
            keys.up += i.count_and_consume_key(Modifiers::NONE, Key::ArrowUp);
            keys.down += i.count_and_consume_key(Modifiers::NONE, Key::PageDown) * PER_GROUP;
            keys.up += i.count_and_consume_key(Modifiers::NONE, Key::PageUp) * PER_GROUP;
            // Shift+Enter first: plain Enter would match it too.
            keys.alt_enter |= i.consume_key(Modifiers::SHIFT, Key::Enter);
            keys.enter |= i.consume_key(Modifiers::NONE, Key::Enter);
            keys.close |= i.consume_key(Modifiers::NONE, Key::Escape);
        });
        keys.close |= take_shortcut(ctx, Key::K, false, false);
    }

    /// Draws the palette. Returns what the user chose, if anything.
    fn ui(&mut self, app: &App, ctx: &egui::Context) -> Option<Action> {
        if !self.collected {
            self.entries = collect(app);
            self.collected = true;
            self.searched = None;
        }
        let keys = std::mem::take(&mut self.keys);
        let mut close = keys.close;
        let mut chosen: Option<(usize, bool)> = None;

        let screen = ctx.screen_rect();
        let width = WIDTH.min(screen.width() - 48.0).max(240.0);
        let top = (screen.height() * 0.14).max(24.0).min(130.0);
        let list_height = (screen.height() - top - 150.0).max(ROW_H * 2.0).min(440.0);
        let id = Id::new("command-palette");
        let frame = egui::Frame::new()
            .fill(Color32::from_rgb(17, 21, 29))
            .stroke(Stroke::new(1.0_f32, theme::BORDER_HI))
            .corner_radius(CornerRadius::same(14))
            .shadow(egui::Shadow { offset: [0, 18], blur: 48, spread: 0, color: Color32::from_black_alpha(180) });
        let modal = egui::Modal::new(id)
            .area(egui::Modal::default_area(id).anchor(Align2::CENTER_TOP, vec2(0.0, top)))
            .backdrop_color(Color32::from_black_alpha(150))
            .frame(frame)
            .show(ctx, |ui| {
                ui.set_width(width);
                ui.spacing_mut().item_spacing = vec2(0.0, 0.0);

                // ---- search field
                ui.allocate_ui_with_layout(vec2(width, 54.0), Layout::left_to_right(Align::Center), |ui| {
                    ui.add_space(18.0);
                    ui.label(RichText::new(icon::SEARCH).size(15.0).color(theme::MUTED));
                    ui.add_space(12.0);
                    let edit = ui.add(
                        egui::TextEdit::singleline(&mut self.query)
                            .id(id.with("query"))
                            .frame(false)
                            .desired_width(width - 120.0)
                            .font(FontId::new(16.0, FontFamily::Proportional))
                            .hint_text(RichText::new("Search commands, profiles, mods, saves and units").color(theme::FAINT)),
                    );
                    // The palette owns the keyboard for as long as it is open.
                    if !edit.has_focus() {
                        edit.request_focus();
                    }
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        ui.add_space(14.0);
                        keycap(ui, "Esc");
                    });
                });
                hairline(ui);

                // ---- results, re-ranked only when the text changed
                if self.searched.as_deref() != Some(self.query.as_str()) {
                    self.groups = search(&self.query, &self.entries);
                    self.count = self.groups.iter().map(|(_, hits)| hits.len()).sum();
                    self.selected = 0;
                    self.scroll_to_selected = true;
                    self.searched = Some(self.query.clone());
                }
                if self.count > 0 && keys.down + keys.up > 0 {
                    // Wraps around at both ends.
                    let n = self.count;
                    self.selected = (self.selected + keys.down % n + n - keys.up % n) % n;
                    self.scroll_to_selected = true;
                }
                if keys.enter || keys.alt_enter {
                    chosen = (self.count > 0).then_some((self.selected, keys.alt_enter));
                }

                if self.count == 0 {
                    ui.allocate_ui_with_layout(vec2(width, 96.0), Layout::top_down(Align::Center), |ui| {
                        ui.add_space(30.0);
                        ui.label(theme::muted("Nothing matches"));
                        ui.add_space(4.0);
                        ui.label(theme::faint("Try fewer letters, or another word."));
                    });
                } else {
                    let moved = ui.input(|i| i.pointer.delta() != egui::Vec2::ZERO);
                    let scroll = std::mem::take(&mut self.scroll_to_selected);
                    egui::ScrollArea::vertical().max_height(list_height).auto_shrink([false, true]).show(ui, |ui| {
                        ui.add_space(6.0);
                        let mut n = 0;
                        for (kind, hits) in &self.groups {
                            let (head, _) = ui.allocate_exact_size(vec2(ui.available_width(), 26.0), Sense::hover());
                            ui.painter().text(
                                head.left_bottom() + vec2(20.0, -5.0),
                                Align2::LEFT_BOTTOM,
                                kind.heading().to_uppercase(),
                                FontId::new(11.0, theme::semibold()),
                                theme::FAINT,
                            );
                            for &index in hits {
                                let selected = n == self.selected;
                                let out = result_row(ui, &self.entries[index], selected, id.with(("row", n)));
                                if out.hovered && moved {
                                    self.selected = n;
                                }
                                if out.clicked || out.alt_clicked {
                                    chosen = Some((n, out.alt_clicked));
                                }
                                if selected && scroll {
                                    ui.scroll_to_rect(out.rect.expand2(vec2(0.0, 30.0)), None);
                                }
                                n += 1;
                            }
                        }
                        ui.add_space(6.0);
                    });
                }

                // ---- footer: what the keys do
                hairline(ui);
                ui.allocate_ui_with_layout(vec2(width, 36.0), Layout::left_to_right(Align::Center), |ui| {
                    ui.spacing_mut().item_spacing.x = 6.0;
                    ui.add_space(12.0);
                    for (key, what) in [
                        ("\u{2191}\u{2193}", "move"),
                        ("Enter", "open"),
                        ("Shift+Enter", "second action"),
                        ("Esc", "close"),
                    ] {
                        keycap(ui, key);
                        ui.label(theme::faint(what));
                        ui.add_space(6.0);
                    }
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        ui.add_space(14.0);
                        ui.label(theme::faint("opens this"));
                        keycap(ui, "Ctrl+K");
                    });
                });
            });
        close |= modal.backdrop_response.clicked();

        let action = chosen.and_then(|(n, alt)| {
            let index = *self.groups.iter().flat_map(|(_, hits)| hits.iter()).nth(n)?;
            let entry = &self.entries[index];
            if alt {
                // No second action: Shift+Enter does nothing rather than something else.
                entry.alt.as_ref().map(|(_, action)| action.clone())
            } else {
                Some(entry.action.clone())
            }
        });
        if close || action.is_some() {
            self.close();
        }
        action
    }
}

/// Draws the open palette in the current window and carries out what was chosen.
pub fn show(app: &mut App, ctx: &egui::Context) {
    let mut palette = std::mem::take(&mut app.palette);
    let action = palette.ui(app, ctx);
    app.palette = palette;
    if let Some(action) = action {
        run(app, ctx, action);
    }
}

fn hairline(ui: &mut egui::Ui) {
    let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), 1.0), Sense::hover());
    ui.painter().rect_filled(rect, 0.0, theme::BORDER);
}

/// A small key label, like "Esc".
fn keycap(ui: &mut egui::Ui, text: &str) {
    let galley = ui.painter().layout_no_wrap(text.to_owned(), FontId::new(11.0, theme::semibold()), theme::MUTED);
    let (rect, _) = ui.allocate_exact_size(galley.size() + vec2(12.0, 6.0), Sense::hover());
    ui.painter().rect_filled(rect, CornerRadius::same(5), theme::CARD_HI);
    ui.painter().galley(rect.center() - galley.size() / 2.0, galley, theme::MUTED);
}

struct RowOut {
    rect: Rect,
    hovered: bool,
    clicked: bool,
    alt_clicked: bool,
}

fn result_row(ui: &mut egui::Ui, entry: &Entry, selected: bool, id: Id) -> RowOut {
    let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), ROW_H), Sense::click());
    let mut out = RowOut { rect, hovered: resp.hovered(), clicked: resp.clicked(), alt_clicked: false };
    if !ui.is_rect_visible(rect) {
        return out;
    }
    let row = rect.shrink2(vec2(8.0, 1.0));
    let painter = ui.painter().with_clip_rect(row.intersect(ui.clip_rect()));
    if selected {
        painter.rect_filled(row, CornerRadius::same(8), theme::ACCENT_DIM);
    }
    let cy = row.center().y;
    let glyph_color = if selected { theme::ACCENT_HI } else { theme::MUTED };
    painter.text(pos2(row.left() + 14.0, cy), Align2::LEFT_CENTER, entry.glyph, FontId::new(14.0, FontFamily::Proportional), glyph_color);

    // Right to left: the kind, then the shortcut or (on the selected row) the second action.
    let mut right = row.right() - 10.0;
    let (kind, kind_color) = entry.kind.chip();
    let chip = painter.layout_no_wrap(kind.to_owned(), FontId::new(11.0, theme::semibold()), kind_color);
    let chip_rect = Rect::from_min_max(pos2(right - chip.size().x - 14.0, cy - 9.5), pos2(right, cy + 9.5));
    painter.rect_filled(chip_rect, CornerRadius::same(255), kind_color.gamma_multiply(0.14));
    painter.galley(chip_rect.center() - chip.size() / 2.0, chip, kind_color);
    right = chip_rect.left() - 10.0;

    let small = FontId::new(11.5, FontFamily::Proportional);
    match (&entry.alt, selected) {
        (Some((label, _)), true) => {
            let text = painter.layout_no_wrap(format!("Shift+Enter  {label}"), small, Color32::PLACEHOLDER);
            let alt_rect = Rect::from_min_max(pos2(right - text.size().x - 14.0, cy - 11.0), pos2(right, cy + 11.0));
            let alt = ui.interact(alt_rect, id.with("alt"), Sense::click());
            let (fill, color) = if alt.hovered() {
                (Color32::from_rgb(44, 62, 104), Color32::WHITE)
            } else {
                (Color32::from_rgb(34, 48, 80), theme::ACCENT_HI)
            };
            painter.rect_filled(alt_rect, CornerRadius::same(6), fill);
            painter.galley(alt_rect.center() - text.size() / 2.0, text, color);
            out.alt_clicked = alt.clicked();
            out.hovered |= alt.hovered();
            right = alt_rect.left() - 10.0;
        }
        _ if !entry.shortcut.is_empty() => {
            let text = painter.layout_no_wrap(entry.shortcut.clone(), small, theme::FAINT);
            let w = text.size().x;
            painter.galley(pos2(right - w, cy - text.size().y / 2.0), text, theme::FAINT);
            right -= w + 10.0;
        }
        _ => {}
    }

    // Title, then the detail in what is left.
    let left = row.left() + 42.0;
    let text_clip = Rect::from_min_max(pos2(left, row.top()), pos2(right.max(left), row.bottom()));
    let text_painter = painter.with_clip_rect(text_clip.intersect(painter.clip_rect()));
    let title_color = if selected { Color32::WHITE } else { theme::TEXT };
    let title = text_painter.layout_no_wrap(entry.title.clone(), FontId::new(14.0, theme::semibold()), title_color);
    let title_w = title.size().x;
    text_painter.galley(pos2(left, cy - title.size().y / 2.0 - 0.5), title, title_color);
    if !entry.detail.is_empty() && left + title_w + 12.0 < right - 24.0 {
        let detail = text_painter.layout_no_wrap(entry.detail.clone(), FontId::new(12.5, FontFamily::Proportional), theme::FAINT);
        text_painter.galley(pos2(left + title_w + 12.0, cy - detail.size().y / 2.0), detail, theme::FAINT);
    }
    if resp.hovered() && !selected {
        painter.rect_stroke(row, CornerRadius::same(8), Stroke::new(1.0_f32, theme::BORDER_HI), egui::StrokeKind::Inside);
    }
    out
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(kind: Kind, title: &str) -> Entry {
        Entry::new(kind, "", title, "", Action::ShowSaves)
    }

    fn titles<'a>(entries: &'a [Entry], groups: &[(Kind, Vec<usize>)]) -> Vec<(Kind, Vec<&'a str>)> {
        groups.iter().map(|(k, hits)| (*k, hits.iter().map(|&i| entries[i].title.as_str()).collect())).collect()
    }

    #[test]
    fn palette_ranks_prefix_over_word_start_over_substring() {
        let s = |text: &str| match_score("star", text).unwrap();
        assert!(s("star") > s("Star_Destroyer"), "exact beats prefix");
        assert!(s("Star_Destroyer") > s("Imperial_Star_Destroyer"), "prefix beats word start");
        assert!(s("Imperial_Star_Destroyer") > s("Deathstar"), "word start beats substring");
        assert!(s("Death Star") > s("Deathstar"));
        assert!(match_score("star", "Tartan Patrol Cruiser").is_none());
        // Case and surrounding spaces don't matter.
        assert_eq!(match_score("  STAR ", "star destroyer"), match_score("star", "Star Destroyer"));
        // Between equal hits the shorter name wins.
        assert!(s("Star Base") > s("Star Destroyer Squadron"));
        // An earlier word beats a later one.
        assert!(match_score("wing", "A Wing Squadron") > match_score("wing", "Rebel Alliance X Wing"));
        assert_eq!(match_score("", "anything"), Some(0));
    }

    #[test]
    fn palette_matches_several_words_in_any_order() {
        assert!(match_score("open logs", "Open logs folder").is_some());
        assert!(match_score("logs open", "Open logs folder").is_some());
        assert!(match_score("open logs", "Open logs folder") > match_score("logs open", "Open logs folder"));
        assert!(match_score("open nothing", "Open logs folder").is_none());
        assert!(match_score("tie int", "TIE_Interceptor_Squadron").is_some());
    }

    #[test]
    fn palette_fuzzy_is_a_last_resort() {
        // Letters in order, starting at a word.
        let fuzzy_hit = match_score("thrw", "Thrawn's Revenge").unwrap();
        assert!(fuzzy_hit < match_score("venge", "Thrawn's Revenge").unwrap(), "any real substring beats fuzzy");
        assert!(match_score("tfs", "TIE Fighter Squadron").is_some());
        // Not from the middle of a word, not spread across the whole text, not for two letters.
        assert!(match_score("hrw", "Thrawn's Revenge").is_none());
        assert!(match_score("tqn", "the quick brown fox jumps over the lazy dog again").is_none());
        assert!(match_score("tr", "Thrawn's Revenge").is_none());
    }

    #[test]
    fn palette_keywords_count_for_less_than_the_title() {
        let by_title = entry(Kind::Command, "Play");
        let by_keyword = entry(Kind::Command, "Launch game").keywords("play start run");
        assert!(by_title.score("play").unwrap() > by_keyword.score("play").unwrap());
        assert!(by_keyword.score("play").is_some());
        assert!(by_keyword.score("launch").unwrap() > by_keyword.score("start").unwrap());
        assert!(by_keyword.score("zebra").is_none());
    }

    #[test]
    fn palette_groups_by_kind_with_the_best_kind_first() {
        let mut entries = vec![
            entry(Kind::Command, "Launch game").keywords("play start run"),
            entry(Kind::Command, "Play"),
            entry(Kind::Command, "Profiles"),
            entry(Kind::Profile, "Vanilla"),
            entry(Kind::Profile, "Playthrough two"),
            entry(Kind::Mod, "Republic at War"),
            entry(Kind::Mod, "Display tweaks"),
            entry(Kind::Save, "Autoplay test"),
            entry(Kind::Unit, "Play_Dummy"),
        ];
        let groups = search("play", &entries);
        assert_eq!(
            titles(&entries, &groups),
            vec![
                // Exact title first; the keyword hit after it, in the same group.
                (Kind::Command, vec!["Play", "Launch game"]),
                // Prefixes; the shorter text first.
                (Kind::Unit, vec!["Play_Dummy"]),
                (Kind::Profile, vec!["Playthrough two"]),
                // Substrings last.
                (Kind::Mod, vec!["Display tweaks"]),
                (Kind::Save, vec!["Autoplay test"]),
            ]
        );

        // No more than PER_GROUP per kind, the best ones, ties in their original order.
        for i in 0..20 {
            entries.push(entry(Kind::Unit, &format!("Play_Unit_{i:02}")));
        }
        entries.push(entry(Kind::Unit, "Play"));
        let groups = search("play", &entries);
        let units = &titles(&entries, &groups).into_iter().find(|(k, _)| *k == Kind::Unit).unwrap().1;
        assert_eq!(units.len(), PER_GROUP);
        assert_eq!(units[..3], ["Play", "Play_Dummy", "Play_Unit_00"]);
        // Two kinds with an exact hit: the earlier kind leads.
        assert_eq!(groups[0].0, Kind::Command);
        assert_eq!(groups[1].0, Kind::Unit);

        assert!(search("zzzz", &entries).is_empty());
    }

    #[test]
    fn palette_empty_search_lists_commands_and_profiles() {
        let mut entries: Vec<Entry> = (0..12).map(|i| entry(Kind::Command, &format!("Command {i}"))).collect();
        entries.push(entry(Kind::Profile, "Vanilla"));
        entries.push(entry(Kind::Mod, "Some mod"));
        entries.push(entry(Kind::Save, "Some save"));
        entries.push(entry(Kind::Unit, "Some unit"));
        let groups = search("   ", &entries);
        let shown = titles(&entries, &groups);
        assert_eq!(shown.len(), 2);
        assert_eq!(shown[0].0, Kind::Command);
        // The first few, in the order they were listed.
        assert_eq!(shown[0].1, (0..PER_GROUP).map(|i| format!("Command {i}")).collect::<Vec<_>>());
        assert_eq!(shown[1], (Kind::Profile, vec!["Vanilla"]));
    }
}

//! Unit database: browse, compare and edit the XML definitions the active profile loads.

use crate::app::{reveal_in_explorer, App, Page};
use crate::mods;
use crate::theme::{self, icon};
use crate::unitdb::{
    self, BatchEdit, BatchLabel, BatchPreview, BuildInput, Category, Check, DiffKind, DiffLine, EditPreview, HardpointRow,
    HardpointState, HistoryEntry, Progress, SourceGroup, UnitDb,
};
use egui::text::{LayoutJob, TextFormat, TextWrapping};
use egui::{pos2, vec2, Align, Align2, Color32, CornerRadius, FontId, Layout, Rect, RichText, ScrollArea, Sense, Stroke, StrokeKind, Ui, UiBuilder};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::atomic::Ordering;
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::sync::Arc;
use std::time::Duration;

const MAX_COMPARE: usize = 4;
const LIST_ROW_H: f32 = 44.0;
const FIELD_ROW_H: f32 = 28.0;
const CMP_ROW_H: f32 = 26.0;

/// What an index was built for: active profile id, its mod folders, the FoC folder.
type Sig = (u64, Vec<PathBuf>, PathBuf);

#[derive(Clone, Copy, PartialEq, Eq, Default)]
enum Tab {
    #[default]
    Details,
    Compare,
    History,
}

struct Job {
    rx: Receiver<UnitDb>,
    progress: Arc<Progress>,
    sig: Sig,
}

struct Row {
    key: String,
    value: String,
    nested: bool,
    occurrence: usize,
    editable: bool,
    inherited: bool,
    from: usize,
    from_name: String,
    /// Lower-cased key and value, for the filter box.
    hay: String,
}

struct RowsCache {
    def: usize,
    own_only: bool,
    /// The definition lists hardpoints, itself or through a parent.
    has_hardpoints: bool,
    rows: Result<Vec<Row>, String>,
    shown: Vec<usize>,
    filter: Option<String>,
}

struct CmpCache {
    set: Vec<usize>,
    keys: Vec<String>,
    /// row -> column -> value
    cells: Vec<Vec<Option<String>>>,
    differs: Vec<bool>,
    errors: Vec<String>,
    shown: Vec<usize>,
    shown_only_diff: Option<bool>,
}

struct EditState {
    def: usize,
    def_name: String,
    key: String,
    occurrence: usize,
    old: String,
    new: String,
    check: Check,
    preview: Option<EditPreview>,
    error: Option<String>,
    focus: bool,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum HpMode {
    Scale,
    SetEach,
    SetTotal,
}

struct HpRow {
    row: HardpointRow,
    include: bool,
    /// Other definitions whose hardpoint list names this one, by name.
    others: Vec<String>,
}

/// The "Adjust hardpoint health" dialog.
struct HpState {
    def_name: String,
    rows: Vec<HpRow>,
    mode: HpMode,
    percent: f64,
    each: f64,
    total: f64,
    preview: Option<BatchPreview>,
    error: Option<String>,
}

/// What the dialog's current settings would do, row by row.
struct HpPlan {
    /// New Health per row, where it differs from the current one. Also set for rows
    /// that inherit from a row being changed.
    new: Vec<Option<String>>,
    /// Rows whose own value gets written.
    edits: Vec<bool>,
    current_total: f64,
    new_total: f64,
    problem: Option<String>,
}

enum Action {
    Select(usize),
    ToggleCompare(usize),
    ClearCompare,
    Edit { key: String, occurrence: usize, old: String },
    AdjustHardpoints,
}

#[derive(Default)]
pub struct State {
    db: Option<UnitDb>,
    built_sig: Option<Sig>,
    job: Option<Job>,
    auto_tried: bool,
    /// Definition indices sorted by name; valid for `db` only.
    order: Vec<usize>,
    /// Lower-cased "name\nkind" per definition.
    hay: Vec<String>,
    file_labels: Vec<String>,

    /// Sources with files in the index, in priority order: group, label, definitions.
    sources: Vec<(SourceGroup, String, usize)>,

    query: String,
    kind: Option<String>,
    source: Option<SourceGroup>,
    editable_only: bool,
    filter_key: Option<(String, Option<String>, Option<SourceGroup>, bool)>,
    filtered: Vec<usize>,

    selected: Option<usize>,
    compare: Vec<usize>,
    tab: Tab,
    /// Inverted so the default (false) shows inherited values.
    own_only: bool,
    field_filter: String,
    rows: Option<RowsCache>,
    cmp: Option<CmpCache>,
    only_diff: bool,
    edit: Option<EditState>,
    hp: Option<HpState>,
    /// Newest first, with whether the backup file still exists.
    history: Option<Vec<(HistoryEntry, bool)>>,
    confirm_restore: Option<HistoryEntry>,
}

impl State {
    /// Name and type of every definition in the index, or nothing while there is no index.
    /// The command palette searches these.
    pub fn definition_names(&self) -> impl Iterator<Item = (&str, &str)> + '_ {
        self.db.iter().flat_map(|db| db.defs.iter().map(|d| (d.name.as_str(), d.kind.as_str())))
    }

    /// Selects the definition with this name (ignoring case) and shows its details, with
    /// the list searching for it and no other filter in the way. Returns false if the
    /// index has no such definition.
    pub fn select_by_name(&mut self, name: &str) -> bool {
        let Some(db) = self.db.as_ref() else { return false };
        let Some(index) = db
            .defs
            .iter()
            .position(|d| d.name == name)
            .or_else(|| db.defs.iter().position(|d| d.name.eq_ignore_ascii_case(name)))
        else {
            return false;
        };
        self.query = db.defs[index].name.clone();
        self.kind = None;
        self.source = None;
        self.editable_only = false;
        self.selected = Some(index);
        self.rows = None;
        self.tab = Tab::Details;
        true
    }
}

pub fn show(app: &mut App, ui: &mut Ui) {
    // Taken out for the frame so the page can use `app` and its own state side by side.
    let mut st = std::mem::take(&mut app.units_ui);
    body(app, &mut st, ui);
    app.units_ui = st;
}

fn body(app: &mut App, st: &mut State, ui: &mut Ui) {
    theme::page_header(
        ui,
        "Unit database",
        "Every XML definition the active profile loads, found the way the game finds them: mods in load order, then loose base-game XML, then the game's archives.",
    );
    let ctx = ui.ctx().clone();
    poll(app, st);

    let Some(foc) = mods::foc_dir(&app.steam, &app.cfg.settings) else {
        theme::card(ui, |ui| {
            theme::empty_state(
                ui,
                icon::FOLDER,
                "Forces of Corruption wasn't found",
                "Set the game folder in Settings to build the unit database.",
            );
            ui.vertical_centered(|ui| {
                if theme::button(ui, "Open Settings").clicked() {
                    app.page = Page::Settings;
                }
            });
            ui.add_space(12.0);
        });
        return;
    };
    let sig: Sig = (app.cfg.active().id, app.cfg.active().mods.clone(), foc);

    if st.db.is_none() && st.job.is_none() && !st.auto_tried {
        st.auto_tried = true;
        start_build(app, st, &ctx, sig.clone());
    }

    if let Some(job) = &st.job {
        let done = job.progress.done.load(Ordering::Relaxed);
        let total = job.progress.total.load(Ordering::Relaxed);
        theme::card(ui, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(36.0);
                ui.label(theme::title("Building the index"));
                if total == 0 {
                    ui.label(theme::muted("Opening the game's archives"));
                    ui.add_space(4.0);
                    ui.spinner();
                } else {
                    ui.label(theme::muted(format!("{done} of {total} files")));
                    ui.add_space(4.0);
                    ui.add(egui::ProgressBar::new(done as f32 / total as f32).desired_width(360.0).desired_height(8.0));
                }
                ui.add_space(36.0);
            });
        });
        ctx.request_repaint_after(Duration::from_millis(100));
        return;
    }

    let mut rebuild = false;
    let Some(db) = st.db.as_ref() else {
        theme::card(ui, |ui| {
            theme::empty_state(ui, icon::DATABASE, "No index yet", "Build it to browse the definitions this profile loads.");
            ui.vertical_centered(|ui| {
                rebuild = theme::primary_button(ui, "Build index").clicked();
            });
            ui.add_space(12.0);
        });
        if rebuild {
            start_build(app, st, &ctx, sig);
        }
        return;
    };

    ui.horizontal(|ui| {
        ui.label(theme::muted(format!(
            "{} definitions · {} files · {} archives · {} shadowed duplicates",
            db.defs.len(),
            db.files.len(),
            db.meg_count,
            db.shadowed
        )));
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if theme::button(ui, &format!("{}  Refresh", icon::REFRESH)).on_hover_text("Rebuild the index from disk").clicked() {
                rebuild = true;
            }
        });
    });

    if st.built_sig.as_ref() != Some(&sig) {
        egui::Frame::new()
            .fill(theme::WARN.gamma_multiply(0.10))
            .stroke(Stroke::new(1.0_f32,theme::WARN.gamma_multiply(0.45)))
            .corner_radius(CornerRadius::same(10))
            .inner_margin(egui::Margin::symmetric(14, 8))
            .show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.horizontal(|ui| {
                    ui.label(RichText::new(icon::WARNING).color(theme::WARN));
                    ui.label(theme::text("The active profile changed since this index was built."));
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if theme::primary_button(ui, "Rebuild").clicked() {
                            rebuild = true;
                        }
                    });
                });
            });
    }

    if !db.warnings.is_empty() {
        let n = db.warnings.len();
        let label = if n == 1 { "1 warning".to_owned() } else { format!("{n} warnings") };
        egui::CollapsingHeader::new(RichText::new(label).color(theme::WARN)).id_salt("unit_warnings").show(ui, |ui| {
            ScrollArea::vertical().id_salt("unit_warnings_scroll").max_height(110.0).show(ui, |ui| {
                for w in &db.warnings {
                    ui.add(egui::Label::new(theme::muted(w.clone())).wrap());
                }
            });
        });
    }

    if rebuild {
        start_build(app, st, &ctx, sig);
        return;
    }

    // Two panes filling the rest of the page; each scrolls on its own.
    let avail = ui.available_rect_before_wrap();
    let area = Rect::from_min_max(avail.min, pos2(avail.right(), (avail.bottom() - 20.0).max(avail.top() + 200.0)));
    ui.allocate_rect(area, Sense::hover());
    let left = Rect::from_min_size(area.min, vec2(330.0_f32.min(area.width() * 0.45), area.height()));
    let right = Rect::from_min_max(pos2(left.right() + 14.0, area.top()), area.max);
    for r in [left, right] {
        ui.painter().rect_filled(r, CornerRadius::same(12), theme::CARD);
        ui.painter().rect_stroke(r, CornerRadius::same(12), Stroke::new(1.0_f32,theme::BORDER), StrokeKind::Inside);
    }
    ui.scope_builder(UiBuilder::new().max_rect(left.shrink(14.0)), |ui| {
        ui.set_clip_rect(left.shrink(1.0).intersect(ui.clip_rect()));
        left_pane(app, st, ui);
    });
    ui.scope_builder(UiBuilder::new().max_rect(right.shrink(14.0)), |ui| {
        ui.set_clip_rect(right.shrink(1.0).intersect(ui.clip_rect()));
        right_pane(app, st, ui);
    });

    edit_modal(app, st, &ctx);
    hardpoint_modal(app, st, &ctx);
    restore_modal(app, st, &ctx);
}

// ------------------------------------------------------------------ building

fn start_build(app: &App, st: &mut State, ctx: &egui::Context, sig: Sig) {
    let input = BuildInput { mods: sig.1.iter().map(|p| (app.mod_name(p), p.clone())).collect(), foc: sig.2.clone() };
    let progress = Arc::new(Progress::default());
    let (tx, rx) = mpsc::channel();
    let (thread_progress, thread_ctx) = (progress.clone(), ctx.clone());
    std::thread::spawn(move || {
        let db = UnitDb::build(input, &thread_progress);
        let _ = tx.send(db);
        thread_ctx.request_repaint();
    });
    st.job = Some(Job { rx, progress, sig });
}

fn poll(app: &mut App, st: &mut State) {
    let Some(job) = st.job.as_ref() else { return };
    match job.rx.try_recv() {
        Ok(db) => {
            let sig = st.job.take().map(|j| j.sig);
            install(st, db, sig);
        }
        Err(TryRecvError::Empty) => {}
        Err(TryRecvError::Disconnected) => {
            st.job = None;
            app.err("Building the unit database stopped unexpectedly. Try Refresh.");
        }
    }
}

/// Swaps in a freshly built index. Definition indices from the old one are meaningless
/// now, so everything that holds them is cleared.
fn install(st: &mut State, db: UnitDb, sig: Option<Sig>) {
    st.hay = db.defs.iter().map(|d| format!("{}\n{}", d.name, d.kind).to_lowercase()).collect();
    st.order = (0..db.defs.len()).collect();
    let hay = &st.hay;
    st.order.sort_by(|&a, &b| hay[a].cmp(&hay[b]));
    st.file_labels = db.files.iter().map(|f| db.layer_label(&f.layer)).collect();
    st.sources = db.source_groups().into_iter().map(|(g, n)| (g, db.source_group_label(g), n)).collect();
    if let Some(k) = &st.kind {
        if !db.kinds.iter().any(|(name, _)| name == k) {
            st.kind = None;
        }
    }
    // Mod positions mean something else in a new index.
    st.source = None;
    st.db = Some(db);
    st.built_sig = sig;
    st.filter_key = None;
    st.filtered.clear();
    st.selected = None;
    st.compare.clear();
    st.rows = None;
    st.cmp = None;
    st.edit = None;
    st.hp = None;
}

fn apply(app: &mut App, st: &mut State, act: Option<Action>) {
    match act {
        None => {}
        Some(Action::Select(i)) => {
            st.selected = Some(i);
            st.rows = None;
            st.tab = Tab::Details;
        }
        Some(Action::ToggleCompare(i)) => {
            if let Some(pos) = st.compare.iter().position(|&c| c == i) {
                st.compare.remove(pos);
            } else if st.compare.len() >= MAX_COMPARE {
                app.err("You can compare up to four at a time. Remove one first.");
            } else {
                st.compare.push(i);
            }
            st.cmp = None;
        }
        Some(Action::ClearCompare) => {
            st.compare.clear();
            st.cmp = None;
        }
        Some(Action::Edit { key, occurrence, old }) => {
            let (Some(def), Some(db)) = (st.selected, st.db.as_ref()) else { return };
            st.edit = Some(EditState {
                def,
                def_name: db.defs[def].name.clone(),
                key,
                occurrence,
                new: old.clone(),
                old,
                check: Check::Ok,
                preview: None,
                error: None,
                focus: true,
            });
        }
        Some(Action::AdjustHardpoints) => {
            let (Some(def), Some(db)) = (st.selected, st.db.as_mut()) else { return };
            match db.hardpoints_of(def) {
                Ok(found) => {
                    let rows: Vec<HpRow> = found
                        .into_iter()
                        .map(|row| {
                            let mut others: Vec<String> =
                                db.hardpoint_users(&row.name).into_iter().filter(|&u| u != def).map(|u| db.defs[u].name.clone()).collect();
                            others.sort_by_key(|n| n.to_lowercase());
                            HpRow { include: row.editable(), others, row }
                        })
                        .collect();
                    let each = rows.iter().find(|r| r.row.editable()).and_then(|r| r.row.health_value).unwrap_or(100.0);
                    let total = rows.iter().filter_map(|r| r.row.health_value).sum();
                    st.hp = Some(HpState {
                        def_name: db.defs[def].name.clone(),
                        rows,
                        mode: HpMode::Scale,
                        percent: 100.0,
                        each,
                        total,
                        preview: None,
                        error: None,
                    });
                }
                Err(e) => app.err(e),
            }
        }
    }
}

// ------------------------------------------------------------------ painting helpers

fn body_font() -> FontId {
    FontId::proportional(14.0)
}
fn small_font() -> FontId {
    FontId::proportional(12.0)
}
fn mono_font() -> FontId {
    FontId::monospace(12.5)
}

/// Paints one line of text, cut off with an ellipsis at the rect's width. Returns
/// whether it was cut off.
fn paint_text(ui: &Ui, rect: Rect, text: &str, font: FontId, color: Color32, italics: bool) -> bool {
    let mut job = LayoutJob::single_section(text.to_owned(), TextFormat { font_id: font, color, italics, ..Default::default() });
    job.wrap = TextWrapping::truncate_at_width(rect.width().max(0.0));
    let galley = ui.fonts(|f| f.layout_job(job));
    let elided = galley.elided;
    ui.painter().galley(pos2(rect.left(), rect.center().y - galley.size().y / 2.0), galley, color);
    elided
}

fn paint_glyph(ui: &Ui, rect: Rect, glyph: &str, color: Color32) {
    ui.painter().text(rect.center(), Align2::CENTER_CENTER, glyph, FontId::proportional(14.0), color);
}

fn tab_button(ui: &mut Ui, label: &str, selected: bool) -> bool {
    let text = if selected { theme::strong(label) } else { theme::muted(label) };
    let fill = if selected { theme::ACCENT_DIM } else { Color32::TRANSPARENT };
    let stroke = if selected { Stroke::new(1.0_f32,theme::ACCENT.gamma_multiply(0.6)) } else { Stroke::NONE };
    ui.add(egui::Button::new(text).fill(fill).stroke(stroke).corner_radius(CornerRadius::same(8))).clicked()
}

fn category_label(c: Category) -> &'static str {
    match c {
        Category::Object => "Game object",
        Category::TradeRoute => "Trade route",
        Category::Campaign => "Campaign",
        Category::HardPoint => "Hardpoint",
    }
}

// ------------------------------------------------------------------ left: search + list

fn left_pane(app: &mut App, st: &mut State, ui: &mut Ui) {
    let Some(db) = st.db.as_ref() else { return };
    let mut act = None;

    ui.add(
        egui::TextEdit::singleline(&mut st.query)
            .hint_text(format!("{}  Search by name or type", icon::SEARCH))
            .desired_width(f32::INFINITY),
    );
    ui.horizontal(|ui| {
        let w = ((ui.available_width() - ui.spacing().item_spacing.x) / 2.0).floor();
        let current = st.kind.clone().unwrap_or_else(|| "All types".to_owned());
        egui::ComboBox::from_id_salt("unit_kind").width(w).truncate().selected_text(current).show_ui(ui, |ui| {
            if ui.selectable_label(st.kind.is_none(), "All types").clicked() {
                st.kind = None;
            }
            for (k, n) in &db.kinds {
                if ui.selectable_label(st.kind.as_deref() == Some(k.as_str()), format!("{k} ({n})")).clicked() {
                    st.kind = Some(k.clone());
                }
            }
        });
        let current = st
            .source
            .and_then(|s| st.sources.iter().find(|(g, _, _)| *g == s))
            .map(|(_, label, _)| label.clone())
            .unwrap_or_else(|| "All sources".to_owned());
        egui::ComboBox::from_id_salt("unit_source")
            .width(w)
            .truncate()
            .selected_text(current)
            .show_ui(ui, |ui| {
                if ui.selectable_label(st.source.is_none(), "All sources").clicked() {
                    st.source = None;
                }
                for (g, label, n) in &st.sources {
                    if ui.selectable_label(st.source == Some(*g), format!("{label} ({n})")).clicked() {
                        st.source = Some(*g);
                    }
                }
            })
            .response
            .on_hover_text("Where a definition's file comes from: a mod, loose game files or the game's archives");
    });

    let key = (st.query.trim().to_lowercase(), st.kind.clone(), st.source, st.editable_only);
    if st.filter_key.as_ref() != Some(&key) {
        let hay = &st.hay;
        st.filtered = st
            .order
            .iter()
            .copied()
            .filter(|&i| {
                let d = &db.defs[i];
                key.1.as_ref().is_none_or(|k| &d.kind == k)
                    && key.2.is_none_or(|s| SourceGroup::of(&db.files[d.file].layer) == s)
                    && (!key.3 || db.files[d.file].path.is_some())
                    && (key.0.is_empty() || hay[i].contains(&key.0))
            })
            .collect();
        st.filter_key = Some(key);
    }

    ui.horizontal(|ui| {
        ui.label(theme::faint(if st.filtered.len() == db.defs.len() {
            format!("{} definitions", db.defs.len())
        } else {
            format!("{} of {}", st.filtered.len(), db.defs.len())
        }));
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            theme::toggle(ui, &mut st.editable_only).on_hover_text("Only definitions in loose XML files, which can be changed in place");
            ui.label(theme::faint("Editable only"));
        });
    });

    if st.filtered.is_empty() {
        theme::empty_state(ui, icon::SEARCH, "Nothing matches", "Try a different search, type or source.");
        return;
    }

    ui.spacing_mut().item_spacing.y = 2.0;
    ScrollArea::vertical().id_salt("unit_list").auto_shrink([false, false]).show_rows(ui, LIST_ROW_H, st.filtered.len(), |ui, range| {
        for n in range {
            let idx = st.filtered[n];
            let d = &db.defs[idx];
            let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), LIST_ROW_H), Sense::hover());
            if !ui.is_rect_visible(rect) {
                continue;
            }
            let btn = Rect::from_center_size(pos2(rect.right() - 22.0, rect.center().y), vec2(28.0, 28.0));
            let main = Rect::from_min_max(rect.min, pos2(btn.left() - 4.0, rect.bottom()));
            let resp = ui.interact(main, ui.id().with(("row", idx)), Sense::click());
            let selected = st.selected == Some(idx);
            if selected {
                ui.painter().rect_filled(rect, CornerRadius::same(8), theme::ACCENT_DIM);
                ui.painter().rect_stroke(rect, CornerRadius::same(8), Stroke::new(1.0_f32,theme::ACCENT.gamma_multiply(0.6)), StrokeKind::Inside);
            } else if resp.hovered() {
                ui.painter().rect_stroke(rect, CornerRadius::same(8), Stroke::new(1.0_f32,theme::BORDER_HI), StrokeKind::Inside);
            }
            let text_w = main.width() - 14.0;
            let top = Rect::from_min_size(pos2(rect.left() + 10.0, rect.top() + 4.0), vec2(text_w, 20.0));
            let bottom = Rect::from_min_size(pos2(rect.left() + 10.0, rect.top() + 23.0), vec2(text_w, 17.0));
            paint_text(ui, top, &d.name, FontId::new(14.0, theme::semibold()), theme::TEXT, false);
            let sub = format!("{} · {}", d.kind, st.file_labels.get(d.file).map(String::as_str).unwrap_or(""));
            paint_text(ui, bottom, &sub, small_font(), theme::FAINT, false);
            if resp.clicked() {
                act = Some(Action::Select(idx));
            }

            let in_cmp = st.compare.contains(&idx);
            let b = ui.interact(btn, ui.id().with(("cmp", idx)), Sense::click());
            if in_cmp {
                ui.painter().rect_filled(btn, CornerRadius::same(6), theme::ACCENT.gamma_multiply(0.30));
            } else if b.hovered() {
                ui.painter().rect_filled(btn, CornerRadius::same(6), Color32::from_rgb(40, 48, 64));
            }
            let color = if in_cmp { theme::ACCENT_HI } else if b.hovered() { Color32::WHITE } else { theme::FAINT };
            paint_glyph(ui, btn, icon::COMPARE, color);
            if b.on_hover_text(if in_cmp { "Remove from compare" } else { "Add to compare" }).clicked() {
                act = Some(Action::ToggleCompare(idx));
            }
        }
    });

    apply(app, st, act);
}

// ------------------------------------------------------------------ right: tabs

fn right_pane(app: &mut App, st: &mut State, ui: &mut Ui) {
    let before = st.tab;
    ui.horizontal(|ui| {
        ui.spacing_mut().item_spacing.x = 6.0;
        if tab_button(ui, "Details", st.tab == Tab::Details) {
            st.tab = Tab::Details;
        }
        if tab_button(ui, &format!("Compare ({})", st.compare.len()), st.tab == Tab::Compare) {
            st.tab = Tab::Compare;
        }
        if tab_button(ui, "Edit history", st.tab == Tab::History) {
            st.tab = Tab::History;
        }
    });
    if st.tab == Tab::History && before != Tab::History {
        st.history = None;
    }
    theme::separator(ui);
    match st.tab {
        Tab::Details => details_tab(app, st, ui),
        Tab::Compare => compare_tab(app, st, ui),
        Tab::History => history_tab(st, ui),
    }
}

fn details_tab(app: &mut App, st: &mut State, ui: &mut Ui) {
    let Some(db) = st.db.as_mut() else { return };
    let Some(sel) = st.selected.filter(|&s| s < db.defs.len()) else {
        theme::empty_state(ui, icon::DATABASE, "Pick a definition", "Select one in the list to see its values.");
        return;
    };

    if st.rows.as_ref().is_none_or(|c| c.def != sel || c.own_only != st.own_only) {
        let raw = if st.own_only {
            db.fields(sel).map(|v| v.into_iter().map(|f| (f, sel, false)).collect::<Vec<_>>())
        } else {
            db.effective(sel).map(|v| v.into_iter().map(|e| (e.field, e.from, e.inherited)).collect())
        };
        let rows = raw.map(|v| {
            v.into_iter()
                .map(|(f, from, inherited)| Row {
                    hay: format!("{}\n{}", f.key, f.value).to_lowercase(),
                    from_name: if inherited { db.defs[from].name.clone() } else { String::new() },
                    key: f.key,
                    value: f.value,
                    nested: f.nested,
                    occurrence: f.occurrence,
                    editable: f.editable,
                    inherited,
                    from,
                })
                .collect::<Vec<_>>()
        });
        let is_list = |key: &str| key.eq_ignore_ascii_case(unitdb::HARDPOINTS_TAG);
        let has_hardpoints = db.defs[sel].category == Category::Object
            && match &rows {
                Ok(r) if r.iter().any(|r| is_list(&r.key)) => true,
                // The own-values view hides a list that comes from a parent.
                Ok(_) if st.own_only => db.effective(sel).is_ok_and(|v| v.iter().any(|e| is_list(&e.field.key))),
                _ => false,
            };
        st.rows = Some(RowsCache { def: sel, own_only: st.own_only, has_hardpoints, rows, shown: Vec::new(), filter: None });
    }
    let has_hardpoints = st.rows.as_ref().is_some_and(|c| c.has_hardpoints);
    let db = &*db;
    let d = &db.defs[sel];
    let file = &db.files[d.file];
    let mut act = None;

    // ---- header
    ui.add(egui::Label::new(RichText::new(&d.name).size(20.0).family(theme::semibold()).color(theme::TEXT)).truncate());
    ui.horizontal(|ui| {
        theme::chip(ui, d.kind.clone(), theme::ACCENT_HI);
        theme::chip(ui, category_label(d.category), theme::MUTED);
        if file.path.is_none() {
            theme::chip(ui, "Read-only", theme::FAINT).on_hover_text("This definition is inside a game archive, so it can't be changed in place.");
        }
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            let label = if st.compare.contains(&sel) { "Remove from compare" } else { "Add to compare" };
            if theme::button(ui, label).clicked() {
                act = Some(Action::ToggleCompare(sel));
            }
            if has_hardpoints
                && theme::button(ui, "Adjust hardpoint health")
                    .on_hover_text("Change the Health of all this unit's hardpoints in one go")
                    .clicked()
            {
                act = Some(Action::AdjustHardpoints);
            }
        });
    });
    ui.horizontal(|ui| {
        ui.label(theme::muted("Source"));
        ui.label(theme::text(st.file_labels.get(d.file).cloned().unwrap_or_default()));
        ui.add(egui::Label::new(RichText::new(&file.rel).monospace().size(12.5).color(theme::MUTED)).truncate());
    });
    if let Some(path) = &file.path {
        ui.horizontal(|ui| {
            if theme::icon_button(ui, icon::FOLDER_OPEN, "Reveal in Explorer").clicked() {
                reveal_in_explorer(path);
            }
            ui.add(egui::Label::new(theme::faint(path.display().to_string())).truncate());
        });
    }
    if let Some(parent) = &d.variant_of {
        ui.horizontal(|ui| {
            ui.label(theme::muted("Variant of"));
            match db.lookup_in(d.category, parent) {
                Some(p) => {
                    if ui.link(parent.as_str()).on_hover_text("Open the parent definition").clicked() {
                        act = Some(Action::Select(p));
                    }
                }
                None => {
                    ui.label(theme::text(parent.clone()));
                    theme::chip(ui, "parent not found", theme::WARN);
                }
            }
        });
    }
    theme::separator(ui);

    // ---- controls
    ui.horizontal(|ui| {
        let mut inherited = !st.own_only;
        if theme::toggle(ui, &mut inherited).changed() {
            st.own_only = !inherited;
        }
        ui.label(theme::text("Show inherited values"));
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            ui.add(
                egui::TextEdit::singleline(&mut st.field_filter)
                    .hint_text(format!("{}  Filter fields and values", icon::SEARCH))
                    .desired_width(240.0),
            );
        });
    });
    if d.variant_of.is_some() && !st.own_only {
        ui.add(
            egui::Label::new(theme::faint(
                "Variants show the values they inherit from their parent unit, marked with where each value comes from.",
            ))
            .wrap(),
        );
    }

    // ---- table
    let Some(RowsCache { rows, shown, filter, .. }) = st.rows.as_mut() else { return };
    let rows = match rows {
        Ok(r) => r,
        Err(e) => {
            ui.add(egui::Label::new(RichText::new(e.as_str()).color(theme::BAD)).wrap());
            apply(app, st, act);
            return;
        }
    };
    let needle = st.field_filter.trim().to_lowercase();
    if filter.as_ref() != Some(&needle) {
        *shown = (0..rows.len()).filter(|&i| needle.is_empty() || rows[i].hay.contains(&needle)).collect();
        *filter = Some(needle);
    }
    if rows.is_empty() {
        theme::empty_state(ui, icon::INFO, "No values", "This definition has no fields of its own.");
    } else if shown.is_empty() {
        theme::empty_state(ui, icon::SEARCH, "Nothing matches", "No field or value contains that text.");
    } else {
        let any_inherited = rows.iter().any(|r| r.inherited);
        let file_editable = db.file_editable(d.file);
        ui.spacing_mut().item_spacing.y = 0.0;
        ScrollArea::vertical().id_salt("unit_fields").auto_shrink([false, false]).show_rows(ui, FIELD_ROW_H, shown.len(), |ui, range| {
            for n in range {
                let ri = shown[n];
                let r = &rows[ri];
                let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), FIELD_ROW_H), Sense::hover());
                if !ui.is_rect_visible(rect) {
                    continue;
                }
                if n % 2 == 1 {
                    ui.painter().rect_filled(rect, CornerRadius::same(4), theme::CARD_HI.gamma_multiply(0.55));
                }
                let w = rect.width();
                let key_w = (w * 0.32).clamp(140.0, 280.0);
                let origin_w = if any_inherited { (w * 0.22).clamp(110.0, 200.0) } else { 0.0 };
                let edit = Rect::from_center_size(pos2(rect.right() - 18.0, rect.center().y), vec2(26.0, 24.0));
                let x_key = rect.left() + 8.0;
                let x_val = x_key + key_w;
                let x_origin = edit.left() - 6.0 - origin_w;
                let cell = |a: f32, b: f32| Rect::from_min_max(pos2(a, rect.top()), pos2(b.max(a), rect.bottom()));

                paint_text(ui, cell(x_key, x_val - 10.0), &r.key, mono_font(), theme::MUTED, false);
                let val_rect = cell(x_val, x_origin - 10.0);
                let color = if r.nested { theme::FAINT } else { theme::TEXT };
                if paint_text(ui, val_rect, &r.value, body_font(), color, r.nested) {
                    ui.interact(val_rect, ui.id().with(("val", ri)), Sense::hover()).on_hover_ui(|ui| {
                        ui.set_max_width(520.0);
                        ui.add(egui::Label::new(theme::text(r.value.clone())).wrap());
                    });
                }

                if r.inherited {
                    let o = cell(x_origin, edit.left() - 6.0);
                    paint_text(ui, o, &format!("from {}", r.from_name), small_font(), theme::ACCENT_HI, false);
                    let resp = ui
                        .interact(o, ui.id().with(("from", ri)), Sense::click())
                        .on_hover_cursor(egui::CursorIcon::PointingHand)
                        .on_hover_ui(|ui| {
                            ui.label(format!("Inherited from {0}. Open {0} to change it there.", r.from_name));
                        });
                    if resp.clicked() {
                        act = Some(Action::Select(r.from));
                    }
                } else if r.editable {
                    let resp = ui.interact(edit, ui.id().with(("edit", ri)), Sense::click());
                    if resp.hovered() {
                        ui.painter().rect_filled(edit, CornerRadius::same(6), Color32::from_rgb(40, 48, 64));
                    }
                    paint_glyph(ui, edit, icon::EDIT, if resp.hovered() { Color32::WHITE } else { theme::MUTED });
                    if resp.on_hover_text("Edit this value").clicked() {
                        act = Some(Action::Edit { key: r.key.clone(), occurrence: r.occurrence, old: r.value.clone() });
                    }
                } else {
                    paint_glyph(ui, edit, icon::EDIT, theme::FAINT.gamma_multiply(0.45));
                    let why = if !file_editable {
                        "This definition is inside a game archive, so it can't be changed in place."
                    } else if r.nested {
                        "Values that contain nested elements can't be edited here."
                    } else {
                        "This value can't be edited in place (multi-line value, empty tag or UTF-16 file)."
                    };
                    ui.interact(edit, ui.id().with(("noedit", ri)), Sense::hover()).on_hover_text(why);
                }
            }
        });
    }

    apply(app, st, act);
}

// ------------------------------------------------------------------ compare

fn build_compare(db: &mut UnitDb, set: &[usize]) -> CmpCache {
    let n = set.len();
    let mut keys: Vec<String> = Vec::new();
    let mut index: HashMap<String, usize> = HashMap::new();
    let mut cells: Vec<Vec<Option<String>>> = Vec::new();
    let mut errors = Vec::new();
    for (col, &def) in set.iter().enumerate() {
        match db.effective(def) {
            Ok(fields) => {
                for e in fields {
                    let lower = e.field.key.to_lowercase();
                    let row = match index.get(&lower) {
                        Some(&r) => r,
                        None => {
                            keys.push(e.field.key.clone());
                            cells.push(vec![None; n]);
                            index.insert(lower, keys.len() - 1);
                            keys.len() - 1
                        }
                    };
                    match &mut cells[row][col] {
                        Some(s) => {
                            s.push_str("; ");
                            s.push_str(&e.field.value);
                        }
                        empty => *empty = Some(e.field.value),
                    }
                }
            }
            Err(e) => errors.push(format!("{}: {e}", db.defs[def].name)),
        }
    }
    let differs = cells.iter().map(|row| row.iter().any(|c| c != &row[0])).collect();
    CmpCache { set: set.to_vec(), keys, cells, differs, errors, shown: Vec::new(), shown_only_diff: None }
}

fn compare_tab(app: &mut App, st: &mut State, ui: &mut Ui) {
    let Some(db) = st.db.as_mut() else { return };
    st.compare.retain(|&i| i < db.defs.len());
    if st.compare.is_empty() {
        theme::empty_state(
            ui,
            icon::COMPARE,
            "Nothing to compare yet",
            "Use the compare icon next to a definition in the list to add up to four.",
        );
        return;
    }
    if st.cmp.as_ref().is_none_or(|c| c.set != st.compare) {
        st.cmp = Some(build_compare(db, &st.compare));
    }
    let db = &*db;
    let Some(cache) = st.cmp.as_mut() else { return };
    let mut act = None;

    ui.horizontal(|ui| {
        theme::toggle(ui, &mut st.only_diff);
        ui.label(theme::text("Only differences"));
        let diff_count = cache.differs.iter().filter(|d| **d).count();
        ui.label(theme::faint(format!("{} of {} fields differ", diff_count, cache.keys.len())));
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if theme::button(ui, "Clear").clicked() {
                act = Some(Action::ClearCompare);
            }
        });
    });
    for e in &cache.errors {
        ui.add(egui::Label::new(RichText::new(e.as_str()).color(theme::BAD)).wrap());
    }
    if cache.shown_only_diff != Some(st.only_diff) {
        cache.shown = (0..cache.keys.len()).filter(|&r| !st.only_diff || cache.differs[r]).collect();
        cache.shown_only_diff = Some(st.only_diff);
    }
    let cache = &*cache;

    let n = cache.set.len();
    let key_w = 240.0;
    let col_w = ((ui.available_width() - key_w) / n as f32).max(220.0);
    let total_w = key_w + col_w * n as f32;
    ScrollArea::horizontal().id_salt("unit_cmp_h").auto_shrink([false, false]).show(ui, |ui| {
        ui.set_min_width(total_w);
        ui.spacing_mut().item_spacing.y = 0.0;

        let (head, _) = ui.allocate_exact_size(vec2(total_w, 46.0), Sense::hover());
        paint_text(
            ui,
            Rect::from_min_size(pos2(head.left() + 8.0, head.top()), vec2(key_w - 16.0, head.height())),
            "Field",
            small_font(),
            theme::FAINT,
            false,
        );
        for (col, &def) in cache.set.iter().enumerate() {
            let d = &db.defs[def];
            let x = head.left() + key_w + col_w * col as f32;
            let close = Rect::from_center_size(pos2(x + col_w - 22.0, head.center().y), vec2(26.0, 26.0));
            let text_w = col_w - 44.0;
            paint_text(ui, Rect::from_min_size(pos2(x, head.top() + 4.0), vec2(text_w, 20.0)), &d.name, FontId::new(14.0, theme::semibold()), theme::TEXT, false);
            paint_text(ui, Rect::from_min_size(pos2(x, head.top() + 24.0), vec2(text_w, 17.0)), &d.kind, small_font(), theme::FAINT, false);
            let name_rect = Rect::from_min_size(pos2(x, head.top()), vec2(text_w, head.height()));
            let open = ui
                .interact(name_rect, ui.id().with(("cmp_open", def)), Sense::click())
                .on_hover_cursor(egui::CursorIcon::PointingHand)
                .on_hover_text("Open in Details");
            if open.clicked() {
                act = Some(Action::Select(def));
            }
            let resp = ui.interact(close, ui.id().with(("cmp_x", def)), Sense::click());
            if resp.hovered() {
                ui.painter().rect_filled(close, CornerRadius::same(6), Color32::from_rgb(40, 48, 64));
            }
            paint_glyph(ui, close, icon::CLOSE, if resp.hovered() { Color32::WHITE } else { theme::MUTED });
            if resp.on_hover_text("Remove from compare").clicked() {
                act = Some(Action::ToggleCompare(def));
            }
        }
        ui.painter().hline(head.x_range(), head.bottom() - 0.5, Stroke::new(1.0_f32,theme::BORDER));

        if cache.shown.is_empty() {
            theme::empty_state(ui, icon::CHECK, "No differences", "These definitions have the same values.");
            return;
        }
        ScrollArea::vertical().id_salt("unit_cmp_v").auto_shrink([false, false]).show_rows(ui, CMP_ROW_H, cache.shown.len(), |ui, range| {
            for n in range {
                let ri = cache.shown[n];
                let differs = cache.differs[ri];
                let (rect, _) = ui.allocate_exact_size(vec2(total_w, CMP_ROW_H), Sense::hover());
                if !ui.is_rect_visible(rect) {
                    continue;
                }
                if differs {
                    ui.painter().rect_filled(rect, CornerRadius::same(4), theme::WARN.gamma_multiply(0.09));
                }
                paint_text(
                    ui,
                    Rect::from_min_size(pos2(rect.left() + 8.0, rect.top()), vec2(key_w - 18.0, rect.height())),
                    &cache.keys[ri],
                    mono_font(),
                    theme::MUTED,
                    false,
                );
                for (col, value) in cache.cells[ri].iter().enumerate() {
                    let x = rect.left() + key_w + col_w * col as f32;
                    let cell = Rect::from_min_size(pos2(x, rect.top()), vec2(col_w - 12.0, rect.height()));
                    match value {
                        Some(v) => {
                            let color = if differs { theme::TEXT } else { theme::MUTED };
                            if paint_text(ui, cell, v, body_font(), color, false) {
                                ui.interact(cell, ui.id().with(("cmp_cell", ri, col)), Sense::hover()).on_hover_ui(|ui| {
                                    ui.set_max_width(520.0);
                                    ui.add(egui::Label::new(theme::text(v.clone())).wrap());
                                });
                            }
                        }
                        None => {
                            paint_text(ui, cell, "—", body_font(), theme::FAINT, false);
                        }
                    }
                }
            }
        });
    });

    apply(app, st, act);
}

// ------------------------------------------------------------------ edit history

fn clip(s: &str, max: usize) -> String {
    if s.chars().count() <= max {
        s.to_owned()
    } else {
        let mut out: String = s.chars().take(max).collect();
        out.push('…');
        out
    }
}

fn history_tab(st: &mut State, ui: &mut Ui) {
    let list = st.history.get_or_insert_with(|| {
        let mut all = unitdb::load_history();
        all.sort_by(|a, b| b.id.cmp(&a.id));
        all.into_iter()
            .map(|e| {
                let exists = e.backup.is_file();
                (e, exists)
            })
            .collect()
    });
    if list.is_empty() {
        theme::empty_state(
            ui,
            icon::HISTORY,
            "No edits yet",
            "Changes you save from the Details tab are listed here, each with a backup of the whole file.",
        );
        return;
    }
    ui.add(egui::Label::new(theme::faint("Every change keeps a backup of the whole file. Restoring one can itself be undone from here.")).wrap());
    let mut ask = None;
    ScrollArea::vertical().id_salt("unit_history").auto_shrink([false, false]).show(ui, |ui| {
        for (e, exists) in list.iter() {
            theme::inset_frame().show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.spacing_mut().item_spacing.y = 3.0;
                let when = theme::fmt_time(std::time::UNIX_EPOCH + Duration::from_secs(e.when.max(0) as u64));
                ui.horizontal(|ui| {
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if *exists {
                            if theme::button(ui, &format!("{}  Restore this backup", icon::UNDO)).clicked() {
                                ask = Some(e.clone());
                            }
                        } else {
                            ui.label(theme::faint("backup file missing"));
                        }
                        ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                            let title = match e.restore_of {
                                Some(id) => format!("Restored the file to before edit #{id}"),
                                None => format!("{} · {}", e.def, e.key),
                            };
                            ui.add(egui::Label::new(theme::strong(title)).truncate());
                        });
                    });
                });
                if e.restore_of.is_none() {
                    let change = format!("{}  →  {}", clip(&e.old, 60), clip(&e.new, 60));
                    ui.add(egui::Label::new(RichText::new(change).monospace().size(12.5).color(theme::MUTED)).truncate());
                }
                ui.add(egui::Label::new(theme::faint(format!("#{} · {} · {}", e.id, when, e.file.display()))).truncate());
            });
        }
        ui.add_space(8.0);
    });
    if ask.is_some() {
        st.confirm_restore = ask;
    }
}

fn restore_modal(app: &mut App, st: &mut State, ctx: &egui::Context) {
    let Some(entry) = st.confirm_restore.as_ref() else { return };
    let name = entry.file.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| entry.file.display().to_string());
    let (mut cancel, mut confirm) = (false, false);
    egui::Modal::new(egui::Id::new("unit_restore_confirm")).show(ctx, |ui| {
        ui.set_width(460.0);
        ui.label(theme::title("Restore this backup?"));
        ui.add(
            egui::Label::new(theme::muted(format!(
                "Puts {name} back to how it was just before this change. The current file is backed up first, so you can undo this from the history too."
            )))
            .wrap(),
        );
        ui.add(egui::Label::new(theme::faint(entry.file.display().to_string())).wrap());
        ui.add_space(8.0);
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            confirm = theme::primary_button(ui, "Restore").clicked();
            cancel = theme::button(ui, "Cancel").clicked();
        });
    });
    if ctx.input(|i| i.key_pressed(egui::Key::Escape)) {
        cancel = true;
    }
    if confirm {
        let Some(entry) = st.confirm_restore.take() else { return };
        match unitdb::restore(&entry) {
            Ok(_) => {
                if let Some(db) = st.db.as_mut() {
                    db.reindex_path(&entry.file);
                }
                st.rows = None;
                st.cmp = None;
                app.ok(format!("Restored {name}. The previous contents are in the history."));
            }
            Err(e) => app.err(e),
        }
        st.history = None;
    } else if cancel {
        st.confirm_restore = None;
    }
}

// ------------------------------------------------------------------ editing

fn diff_view(ui: &mut Ui, diff: &[DiffLine], max_height: f32) {
    theme::inset_frame().inner_margin(egui::Margin::same(4)).show(ui, |ui| {
        ui.set_width(ui.available_width());
        ScrollArea::both().id_salt("unit_diff").max_height(max_height).auto_shrink([false, true]).show(ui, |ui| {
            ui.spacing_mut().item_spacing.y = 0.0;
            const GUTTER: f32 = 72.0;
            let galleys: Vec<_> = diff
                .iter()
                .map(|l| {
                    let color = if l.kind == DiffKind::Same { theme::MUTED } else { theme::TEXT };
                    ui.painter().layout_no_wrap(l.text.clone(), mono_font(), color)
                })
                .collect();
            let width = (galleys.iter().map(|g| g.size().x).fold(0.0, f32::max) + GUTTER + 8.0).max(ui.available_width());
            for (line, galley) in diff.iter().zip(galleys) {
                let (rect, _) = ui.allocate_exact_size(vec2(width, 19.0), Sense::hover());
                let (bg, sign, sign_color) = match line.kind {
                    DiffKind::Removed => (Some(Color32::from_rgb(62, 24, 29)), "−", theme::BAD),
                    DiffKind::Added => (Some(Color32::from_rgb(19, 52, 36)), "+", theme::GOOD),
                    DiffKind::Same => (None, "", theme::FAINT),
                    DiffKind::Gap => {
                        ui.painter().text(pos2(rect.left() + 20.0, rect.center().y), Align2::LEFT_CENTER, "···", mono_font(), theme::FAINT);
                        continue;
                    }
                };
                if let Some(bg) = bg {
                    ui.painter().rect_filled(rect, 0.0, bg);
                }
                if let Some(n) = line.line_no {
                    ui.painter().text(pos2(rect.left() + 44.0, rect.center().y), Align2::RIGHT_CENTER, n.to_string(), mono_font(), theme::FAINT);
                }
                ui.painter().text(pos2(rect.left() + 56.0, rect.center().y), Align2::CENTER_CENTER, sign, mono_font(), sign_color);
                let color = if line.kind == DiffKind::Same { theme::MUTED } else { theme::TEXT };
                ui.painter().galley(pos2(rect.left() + GUTTER, rect.center().y - galley.size().y / 2.0), galley, color);
            }
        });
    });
}

fn edit_modal(app: &mut App, st: &mut State, ctx: &egui::Context) {
    if st.db.is_none() {
        st.edit = None;
        return;
    }
    let (Some(ed), Some(db)) = (st.edit.as_mut(), st.db.as_mut()) else { return };
    let (mut close, mut saved) = (false, false);

    egui::Modal::new(egui::Id::new("unit_edit")).show(ctx, |ui| {
        ui.set_width(640.0);
        ui.label(theme::title(if ed.preview.is_some() { "Review change" } else { "Edit value" }));
        ui.horizontal(|ui| {
            ui.label(theme::text(ed.def_name.clone()));
            ui.label(theme::faint("·"));
            ui.label(RichText::new(&ed.key).monospace().color(theme::MUTED));
        });
        ui.add_space(4.0);

        if let Some(preview) = &ed.preview {
            // ---- step 2: the diff
            ui.add(egui::Label::new(theme::faint(preview.path.display().to_string())).wrap());
            diff_view(ui, &preview.diff, 320.0);
            ui.label(theme::muted("The whole file is backed up before it's changed."));
            if let Some(e) = &ed.error {
                ui.add(egui::Label::new(RichText::new(e.as_str()).color(theme::BAD)).wrap());
            }
            ui.add_space(8.0);
            let (mut back, mut save) = (false, false);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                save = theme::primary_button(ui, "Save change").clicked();
                back = theme::button(ui, "Back").clicked();
            });
            if save {
                match db.apply_edit(preview) {
                    Ok(_) => {
                        app.ok("Saved. Backup kept in edit history.");
                        saved = true;
                    }
                    Err(e) => {
                        app.err(e.clone());
                        ed.error = Some(e);
                    }
                }
            } else if back {
                ed.preview = None;
                ed.error = None;
                ed.focus = true;
            }
        } else {
            // ---- step 1: the new value, checked as it's typed
            theme::overline(ui, "Current value");
            theme::inset_frame().show(ui, |ui| {
                ui.set_width(ui.available_width());
                if ed.old.is_empty() {
                    ui.label(theme::faint("(empty)"));
                } else {
                    ui.add(egui::Label::new(theme::mono(ed.old.clone())).wrap());
                }
            });
            ui.add_space(4.0);
            theme::overline(ui, "New value");
            let resp = ui.add(egui::TextEdit::singleline(&mut ed.new).font(egui::TextStyle::Monospace).desired_width(f32::INFINITY));
            if ed.focus {
                resp.request_focus();
                ed.focus = false;
            }
            if resp.changed() {
                ed.check = db.validate(&ed.old, &ed.new);
                ed.error = None;
            }
            let unchanged = ed.new.trim() == ed.old.trim();
            let (note, color) = match &ed.check {
                _ if unchanged => ("Unchanged".to_owned(), theme::FAINT),
                Check::Ok => ("Looks valid".to_owned(), theme::GOOD),
                Check::Warn(w) => (w.clone(), theme::WARN),
                Check::Error(e) => (e.clone(), theme::BAD),
            };
            ui.add(egui::Label::new(RichText::new(note).size(12.5).color(color)).wrap());
            if let Some(e) = &ed.error {
                ui.add(egui::Label::new(RichText::new(e.as_str()).color(theme::BAD)).wrap());
            }
            ui.add_space(8.0);
            let can = !unchanged && !matches!(ed.check, Check::Error(_));
            let mut review = resp.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.add_enabled_ui(can, |ui| {
                    review |= theme::primary_button(ui, "Review change").clicked();
                });
                close |= theme::button(ui, "Cancel").clicked();
            });
            if review && can {
                match db.preview_edit(ed.def, &ed.key, ed.occurrence, &ed.old, &ed.new) {
                    Ok(p) => {
                        ed.preview = Some(p);
                        ed.error = None;
                    }
                    Err(e) => ed.error = Some(e),
                }
            }
        }
    });
    if ctx.input(|i| i.key_pressed(egui::Key::Escape)) {
        close = true;
    }

    if saved {
        st.edit = None;
        st.rows = None;
        st.cmp = None;
        st.history = None;
    } else if close {
        st.edit = None;
    }
}

// ------------------------------------------------------------------ hardpoint health

const HP_ROW_H: f32 = 26.0;

fn plural(n: usize, noun: &str) -> String {
    if n == 1 {
        format!("1 {noun}")
    } else {
        format!("{n} {noun}s")
    }
}

/// Works out the new Health of every row from the dialog's mode and number.
fn hp_plan(hp: &HpState) -> HpPlan {
    let rows = &hp.rows;
    let cur = |i: usize| rows[i].row.health_value.unwrap_or(0.0);
    let edits: Vec<bool> = rows.iter().map(|r| r.include && r.row.editable() && r.row.health_value.is_some()).collect();
    // A row that inherits its Health from a row being changed moves with it.
    let follows: Vec<Option<usize>> = rows
        .iter()
        .map(|r| match &r.row.state {
            HardpointState::Inherited(parent) => rows.iter().position(|x| x.row.name.eq_ignore_ascii_case(parent)).filter(|&p| edits[p]),
            _ => None,
        })
        .collect();
    let current_total: f64 = (0..rows.len()).map(cur).sum();

    let mut problem = None;
    let factor = match hp.mode {
        HpMode::Scale => Some(hp.percent / 100.0),
        HpMode::SetEach => None,
        HpMode::SetTotal => {
            let moving: f64 = (0..rows.len()).filter(|&i| edits[i] || follows[i].is_some()).map(cur).sum();
            let fixed = current_total - moving;
            if moving <= 0.0 {
                None
            } else if hp.total - fixed <= 0.0 {
                problem = Some(format!(
                    "The hardpoints that stay as they are already add up to {}. Enter a larger total.",
                    unitdb::fmt_number(fixed)
                ));
                None
            } else {
                Some((hp.total - fixed) / moving)
            }
        }
    };

    let mut new: Vec<Option<String>> = vec![None; rows.len()];
    for (i, r) in rows.iter().enumerate().filter(|(i, _)| edits[*i]) {
        let target = match hp.mode {
            HpMode::SetEach => Some(hp.each),
            _ => factor.map(|f| cur(i) * f),
        };
        let old = r.row.health.as_deref().unwrap_or("");
        new[i] = target.map(|v| unitdb::format_like(old, v)).filter(|n| n != old.trim());
    }
    for i in 0..rows.len() {
        if let Some(p) = follows[i] {
            new[i] = new[p].clone();
        }
    }
    let new_total = (0..rows.len()).map(|i| new[i].as_deref().and_then(unitdb::parse_number).unwrap_or_else(|| cur(i))).sum();
    HpPlan { new, edits, current_total, new_total, problem }
}

/// Why nothing in the list can be changed, and what to do about it.
fn hp_blocked_reason(rows: &[HpRow]) -> String {
    if rows.is_empty() {
        return "This unit's HardPoints list is empty.".to_owned();
    }
    let count = |test: fn(&HardpointState) -> bool| rows.iter().filter(|r| test(&r.row.state)).count();
    let reasons = [
        (
            count(|s| matches!(s, HardpointState::InArchive)),
            "live in the game's archives. Copy the hardpoint file into a mod to edit it.",
        ),
        (
            count(|s| matches!(s, HardpointState::Inherited(_))),
            "take their Health from a parent hardpoint. Open the parent and change it there.",
        ),
        (
            count(|s| matches!(s, HardpointState::NotFound)),
            "aren't defined in any file this profile loads. Check the names in the unit's HardPoints list.",
        ),
        (count(|s| matches!(s, HardpointState::NoHealth)), "have no Health value, so there is nothing to adjust."),
        (count(|s| matches!(s, HardpointState::Locked(_))), "have a Health value that can't be edited in place."),
    ];
    if let Some((_, why)) = reasons.iter().find(|(n, _)| *n == rows.len()) {
        return format!("These hardpoints {why}");
    }
    let mut out = "None of these hardpoints can be changed here.".to_owned();
    for (_, why) in reasons.iter().filter(|(n, _)| *n > 0) {
        out.push_str(" Some ");
        out.push_str(why);
    }
    out
}

fn hardpoint_modal(app: &mut App, st: &mut State, ctx: &egui::Context) {
    if st.db.is_none() {
        st.hp = None;
        return;
    }
    let (Some(hp), Some(db)) = (st.hp.as_mut(), st.db.as_mut()) else { return };
    // `wrote` is set whenever files may have changed, even if the save then failed.
    let (mut close, mut saved, mut wrote) = (false, false, false);

    egui::Modal::new(egui::Id::new("unit_hardpoints")).show(ctx, |ui| {
        ui.set_width(720.0);
        ui.label(theme::title(if hp.preview.is_some() { "Review change" } else { "Adjust hardpoint health" }));
        ui.horizontal(|ui| {
            ui.label(theme::text(hp.def_name.clone()));
            ui.label(theme::faint("·"));
            ui.label(RichText::new("HardPoints").monospace().color(theme::MUTED));
        });
        ui.add_space(4.0);

        if let Some(preview) = &hp.preview {
            // ---- step 2: one diff per file
            let files = preview.files.len();
            ui.label(theme::muted(format!("{} in {}", plural(preview.edits, "Health value"), plural(files, "file"))));
            let diff_height = if files == 1 { 320.0 } else { 190.0 };
            ScrollArea::vertical().id_salt("hp_review").max_height(430.0).auto_shrink([false, true]).show(ui, |ui| {
                for (i, f) in preview.files.iter().enumerate() {
                    ui.add(egui::Label::new(theme::faint(f.path.display().to_string())).wrap());
                    ui.push_id(i, |ui| diff_view(ui, &f.diff, diff_height));
                    ui.add_space(4.0);
                }
            });
            ui.label(theme::muted("Each file is backed up before it's changed."));
            if let Some(e) = &hp.error {
                ui.add(egui::Label::new(RichText::new(e.as_str()).color(theme::BAD)).wrap());
            }
            ui.add_space(8.0);
            let (mut back, mut save) = (false, false);
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                save = theme::primary_button(ui, "Save changes").clicked();
                back = theme::button(ui, "Back").clicked();
            });
            if save {
                wrote = true;
                match db.apply_batch(preview) {
                    Ok(_) => {
                        app.ok(format!(
                            "Adjusted {} in {}. Backups are in edit history.",
                            plural(preview.edits, "hardpoint"),
                            plural(files, "file")
                        ));
                        saved = true;
                    }
                    Err(e) => {
                        app.err(e.clone());
                        hp.error = Some(e);
                    }
                }
            } else if back {
                hp.preview = None;
                hp.error = None;
            }
            return;
        }

        // ---- step 1: how to change them
        let changeable = hp.rows.iter().filter(|r| r.row.editable()).count();
        if changeable == 0 {
            egui::Frame::new()
                .fill(theme::WARN.gamma_multiply(0.10))
                .stroke(Stroke::new(1.0_f32, theme::WARN.gamma_multiply(0.45)))
                .corner_radius(CornerRadius::same(10))
                .inner_margin(egui::Margin::symmetric(14, 8))
                .show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.add(egui::Label::new(theme::text(hp_blocked_reason(&hp.rows))).wrap());
                });
        } else {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 6.0;
                for (mode, label) in
                    [(HpMode::Scale, "Scale by percent"), (HpMode::SetEach, "Set every hardpoint to"), (HpMode::SetTotal, "Set total to")]
                {
                    if tab_button(ui, label, hp.mode == mode) {
                        hp.mode = mode;
                        hp.error = None;
                    }
                }
            });
            ui.horizontal(|ui| match hp.mode {
                HpMode::Scale => {
                    ui.add(egui::DragValue::new(&mut hp.percent).range(1.0..=10000.0).speed(1.0).max_decimals(1).suffix(" %"));
                    for p in [50.0, 75.0, 125.0, 150.0, 200.0] {
                        if theme::button(ui, &format!("{p:.0}")).clicked() {
                            hp.percent = p;
                        }
                    }
                    ui.label(theme::faint("100 changes nothing, 125 is a quarter more."));
                }
                HpMode::SetEach => {
                    ui.add(egui::DragValue::new(&mut hp.each).range(1.0..=1.0e9).speed(1.0).max_decimals(2));
                    ui.label(theme::faint("Every ticked hardpoint gets this Health."));
                }
                HpMode::SetTotal => {
                    ui.add(egui::DragValue::new(&mut hp.total).range(1.0..=1.0e12).speed(10.0).max_decimals(2));
                    ui.label(theme::faint("Shared out in proportion to what each hardpoint has now."));
                }
            });
        }
        ui.add_space(4.0);

        let plan = hp_plan(hp);
        if !hp.rows.is_empty() {
            hp_table(ui, &mut hp.rows, &plan);
        }

        let changing = (0..hp.rows.len()).filter(|&i| plan.edits[i] && plan.new[i].is_some()).count();
        ui.horizontal(|ui| {
            ui.label(theme::muted("Total health"));
            ui.label(theme::mono(unitdb::fmt_number(plan.current_total)));
            if (plan.new_total - plan.current_total).abs() > 1e-9 {
                ui.label(theme::faint("→"));
                ui.label(RichText::new(unitdb::fmt_number(plan.new_total)).monospace().size(12.5).color(theme::GOOD));
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.label(theme::muted(format!("{changing} of {} will change", hp.rows.len())));
            });
        });

        // Other units that list a hardpoint being changed get the new value too.
        let mut shared: Vec<&str> = hp
            .rows
            .iter()
            .enumerate()
            .filter(|(i, _)| plan.edits[*i] && plan.new[*i].is_some())
            .flat_map(|(_, r)| r.others.iter().map(String::as_str))
            .collect();
        shared.sort_unstable_by_key(|n| n.to_lowercase());
        shared.dedup();
        if !shared.is_empty() {
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::WARNING).color(theme::WARN));
                ui.label(RichText::new(format!("Also used by {} — they change too", plural(shared.len(), "other unit"))).color(theme::WARN))
                    .on_hover_ui(|ui| {
                        ui.set_max_width(440.0);
                        let mut names = shared.iter().take(30).copied().collect::<Vec<_>>().join(", ");
                        if shared.len() > 30 {
                            names.push_str(&format!(" and {} more", shared.len() - 30));
                        }
                        ui.add(egui::Label::new(theme::text(names)).wrap());
                        ui.add(
                            egui::Label::new(theme::faint(
                                "Counted from every definition's hardpoint list, including lists inherited from a parent unit.",
                            ))
                            .wrap(),
                        );
                    });
            });
        }
        if let Some(p) = &plan.problem {
            ui.add(egui::Label::new(RichText::new(p.as_str()).color(theme::WARN)).wrap());
        }
        if let Some(e) = &hp.error {
            ui.add(egui::Label::new(RichText::new(e.as_str()).color(theme::BAD)).wrap());
        }
        ui.add_space(8.0);

        let can = changing > 0 && plan.problem.is_none();
        let mut review = false;
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            ui.add_enabled_ui(can, |ui| {
                review = theme::primary_button(ui, "Review change").clicked();
            });
            close |= theme::button(ui, if changeable == 0 { "Close" } else { "Cancel" }).clicked();
        });
        if review && can {
            let edits: Vec<BatchEdit> = hp
                .rows
                .iter()
                .enumerate()
                .filter(|(i, _)| plan.edits[*i])
                .filter_map(|(i, r)| {
                    Some(BatchEdit {
                        def: r.row.def?,
                        key: r.row.key.clone(),
                        occurrence: r.row.occurrence,
                        old: r.row.health.clone()?,
                        new: plan.new[i].clone()?,
                    })
                })
                .collect();
            let summary = match hp.mode {
                HpMode::Scale => format!("×{}", unitdb::fmt_number(hp.percent / 100.0)),
                HpMode::SetEach => format!("each set to {}", unitdb::fmt_number(hp.each)),
                HpMode::SetTotal => format!("unit total set to {}", unitdb::fmt_number(hp.total)),
            };
            let label = BatchLabel { def: hp.def_name.clone(), key: "HardPoints health".to_owned(), summary, noun: "hardpoint".to_owned() };
            match db.preview_batch(&edits, &label) {
                Ok(p) => {
                    hp.preview = Some(p);
                    hp.error = None;
                }
                Err(e) => hp.error = Some(e),
            }
        }
    });
    if ctx.input(|i| i.key_pressed(egui::Key::Escape)) {
        close = true;
    }

    if wrote {
        st.rows = None;
        st.cmp = None;
        st.history = None;
    }
    if saved || close {
        st.hp = None;
    }
}

/// The list of hardpoints with their current and new Health and why a row is or
/// isn't changing.
fn hp_table(ui: &mut Ui, rows: &mut [HpRow], plan: &HpPlan) {
    // Fixed columns from the right; the name gets what's left.
    let columns = |rect: Rect| {
        let cell = |a: f32, b: f32| Rect::from_min_max(pos2(a, rect.top()), pos2(b.max(a), rect.bottom()));
        let status = cell(rect.right() - 178.0, rect.right() - 4.0);
        let new = cell(status.left() - 76.0, status.left() - 8.0);
        let arrow = cell(new.left() - 20.0, new.left());
        let current = cell(arrow.left() - 68.0, arrow.left());
        let source = cell(current.left() - 124.0, current.left() - 8.0);
        let name = cell(rect.left() + 32.0, source.left() - 8.0);
        (name, source, current, arrow, new, status)
    };

    let (head, _) = ui.allocate_exact_size(vec2(ui.available_width(), 22.0), Sense::hover());
    let (name, source, current, _, new, status) = columns(head);
    for (rect, label) in [(name, "Hardpoint"), (source, "Source"), (current, "Health"), (new, "New"), (status, "Status")] {
        paint_text(ui, rect, label, small_font(), theme::FAINT, false);
    }
    ui.painter().hline(head.x_range(), head.bottom() - 0.5, Stroke::new(1.0_f32, theme::BORDER));

    ScrollArea::vertical().id_salt("hp_rows").max_height(HP_ROW_H * 10.5).auto_shrink([false, true]).show(ui, |ui| {
        ui.spacing_mut().item_spacing.y = 0.0;
        for (i, r) in rows.iter_mut().enumerate() {
            let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), HP_ROW_H), Sense::hover());
            if i % 2 == 1 {
                ui.painter().rect_filled(rect, CornerRadius::same(4), theme::CARD_HI.gamma_multiply(0.55));
            }
            let (name, source, current, arrow, new, status) = columns(rect);
            let editable = r.row.editable();
            if editable {
                // Painted by hand: a placed widget would move the layout cursor and break the row grid.
                let check = Rect::from_center_size(pos2(rect.left() + 16.0, rect.center().y), vec2(18.0, 18.0));
                let resp = ui.interact(check.expand(4.0), ui.id().with(("hp_check", i)), Sense::click());
                if resp.clicked() {
                    r.include = !r.include;
                }
                let (fill, line) = match (r.include, resp.hovered()) {
                    (true, _) => (theme::ACCENT, theme::ACCENT),
                    (false, true) => (theme::FIELD, theme::BORDER_HI),
                    (false, false) => (theme::FIELD, theme::BORDER),
                };
                ui.painter().rect_filled(check, CornerRadius::same(4), fill);
                ui.painter().rect_stroke(check, CornerRadius::same(4), Stroke::new(1.0_f32, line), StrokeKind::Inside);
                if r.include {
                    ui.painter().text(check.center(), Align2::CENTER_CENTER, icon::CHECK, FontId::proportional(12.0), Color32::WHITE);
                }
                resp.on_hover_text(if r.include { "Leave this hardpoint as it is" } else { "Change this hardpoint too" });
            }
            let dim = !(editable && r.include) && plan.new[i].is_none();

            let label = if r.row.listed > 1 { format!("{}  ×{}", r.row.name, r.row.listed) } else { r.row.name.clone() };
            let cut = paint_text(ui, name, &label, body_font(), if dim { theme::FAINT } else { theme::TEXT }, false);
            if cut || r.row.listed > 1 {
                ui.interact(name, ui.id().with(("hp_name", i)), Sense::hover()).on_hover_ui(|ui| {
                    ui.label(theme::text(r.row.name.clone()));
                    if r.row.listed > 1 {
                        ui.label(theme::faint(format!("Listed {} times; its Health is changed once.", r.row.listed)));
                    }
                });
            }
            let from = if r.row.source.is_empty() { "—" } else { r.row.source.as_str() };
            paint_text(ui, source, from, small_font(), theme::FAINT, false);
            let health = r.row.health.as_deref().unwrap_or("—");
            paint_text(ui, current, health, mono_font(), if dim { theme::FAINT } else { theme::MUTED }, false);
            if let Some(n) = &plan.new[i] {
                paint_text(ui, arrow, "→", small_font(), theme::FAINT, false);
                paint_text(ui, new, n, mono_font(), theme::GOOD, false);
            }

            let (text, color, tip): (String, Color32, Option<String>) = match &r.row.state {
                HardpointState::NotFound => {
                    ("not found".into(), theme::WARN, Some("No hardpoint file this profile loads defines this name.".into()))
                }
                HardpointState::NoHealth => ("no Health value".into(), theme::FAINT, Some("This hardpoint doesn't set a Health value.".into())),
                HardpointState::Inherited(parent) if plan.new[i].is_some() => {
                    (format!("follows {parent}"), theme::MUTED, Some(format!("Takes its Health from {parent}, which is being changed.")))
                }
                HardpointState::Inherited(parent) => (
                    format!("inherited from {parent}"),
                    theme::FAINT,
                    Some(format!("Takes its Health from {parent}. Open {parent} to change it there.")),
                ),
                HardpointState::InArchive => (
                    "in a game archive".into(),
                    theme::FAINT,
                    Some("Copy the hardpoint file into a mod to edit it.".into()),
                ),
                HardpointState::Locked(why) => ("can't be edited in place".into(), theme::FAINT, Some(format!("Not changed here: {why}."))),
                HardpointState::Editable if !r.include => ("skipped".into(), theme::FAINT, None),
                HardpointState::Editable if plan.new[i].is_none() => ("no change".into(), theme::FAINT, None),
                HardpointState::Editable if !r.others.is_empty() => {
                    let mut names = r.others.iter().take(20).cloned().collect::<Vec<_>>().join(", ");
                    if r.others.len() > 20 {
                        names.push_str(&format!(" and {} more", r.others.len() - 20));
                    }
                    (format!("also used by {} more", r.others.len()), theme::WARN, Some(format!("Also used by {names}. They change too.")))
                }
                HardpointState::Editable => ("will change".into(), theme::GOOD, None),
            };
            let cut = paint_text(ui, status, &text, small_font(), color, false);
            if tip.is_some() || cut {
                ui.interact(status, ui.id().with(("hp_status", i)), Sense::hover()).on_hover_ui(|ui| {
                    ui.set_max_width(440.0);
                    if cut {
                        ui.label(theme::text(text.clone()));
                    }
                    if let Some(tip) = &tip {
                        ui.add(egui::Label::new(theme::muted(tip.clone())).wrap());
                    }
                });
            }
        }
    });
}

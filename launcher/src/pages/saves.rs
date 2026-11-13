//! Saves, checkpoints and the campaign map.

#[path = "campaign_map.rs"]
mod campaign_map;

use crate::app::{open_in_explorer, reveal_in_explorer, App};
use crate::config::{self, SaveNote};
use crate::galaxy::pretty;
use crate::saves::{self as store, BackupOnly, BackupStatus, Checkpoint, SaveInfo, SaveSort};
use crate::steam::same_path;
use crate::theme::{self, icon};
use egui::{vec2, Align, Color32, CornerRadius, Layout, Margin, RichText, Stroke, StrokeKind, Ui};
use std::collections::{BTreeMap, BTreeSet, HashMap, HashSet};
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, TryRecvError};

/// The colour of tag chips.
const TAG: Color32 = Color32::from_rgb(178, 132, 255);

#[derive(Default, Clone, Copy, PartialEq, Eq)]
enum Tab {
    #[default]
    Saves,
    Checkpoints,
    Map,
}

enum Confirm {
    DeleteSave(PathBuf, String),
    Restore(Checkpoint),
    /// The flag: also delete the copy in the backup folder. `None` when there is no copy.
    DeleteCheckpoint(Checkpoint, Option<bool>),
}

enum Action {
    Reload,
    OpenFolder,
    ViewMap(PathBuf),
    Reveal(PathBuf),
    Ask(Confirm),
    Create,
    Verify(Checkpoint),
    Confirmed(Confirm),
    EditNote(PathBuf, String),
    ChooseBackup,
    ClearBackup,
    OpenBackup,
    CheckBackups,
    BackUpAll,
    Import(Checkpoint),
}

/// The note editor's working copy.
struct NoteEdit {
    path: PathBuf,
    name: String,
    note: String,
    tags: Vec<String>,
    input: String,
    /// Put the cursor in the note on the first frame.
    focus: bool,
}

/// What a background job hands back.
enum Done {
    Created(Result<Checkpoint, String>),
    Verified(String, Result<(), String>),
    Restored(String, Result<Option<Checkpoint>, String>),
    Deleted { label: String, local: Result<(), String>, mirror: Option<Result<bool, String>> },
    Scanned { key: ScanKey, deep: bool, available: bool, status: Vec<(PathBuf, BackupStatus)>, only: Vec<BackupOnly> },
    BackedUp { done: usize, failed: Vec<String> },
    Imported(Result<Checkpoint, String>),
}

/// Checkpoint work (hashing, copying, a backup folder that may be a network share) runs
/// on a worker thread, one job at a time.
struct Job {
    what: &'static str,
    rx: Receiver<Done>,
}

/// The backup folder and save folder a scan was made for.
type ScanKey = (PathBuf, Option<PathBuf>);

/// What is known about the backup folder; refreshed off the UI thread.
#[derive(Default)]
struct BackupView {
    scanned_for: Option<ScanKey>,
    /// The checkpoint list changed since the last scan.
    stale: bool,
    available: bool,
    /// Keyed by the local checkpoint's folder.
    status: HashMap<PathBuf, BackupStatus>,
    only: Vec<BackupOnly>,
    /// When every file in the backup was last re-hashed.
    checked: Option<std::time::SystemTime>,
}

#[derive(Default)]
pub struct State {
    /// Set by the app when the game exits so the list is re-read.
    pub refresh: bool,
    pub start_tab: Option<String>,
    tab: Tab,
    loaded: bool,
    dir: Option<PathBuf>,
    dir_exists: bool,
    saves: Vec<SaveInfo>,
    checkpoints: Vec<Checkpoint>,
    new_label: String,
    confirm: Option<Confirm>,
    map: campaign_map::MapState,
    query: String,
    tag_filter: BTreeSet<String>,
    sort: SaveSort,
    note_edit: Option<NoteEdit>,
    job: Option<Job>,
    backup: BackupView,
}

impl State {
    /// Shows the campaign map of this save the next time the page is drawn. The command
    /// palette uses it for "View map".
    pub fn open_map(&mut self, save: &Path) {
        self.start_tab = Some(format!("map:{}", save.display()));
    }
}

pub fn show(app: &mut App, ui: &mut egui::Ui) {
    // The page state is moved out for the frame so it and the app can be borrowed freely.
    let refresh = app.saves_ui.refresh;
    let mut st = std::mem::take(&mut app.saves_ui);
    st.refresh = refresh;
    page(app, &mut st, ui);
    app.saves_ui = st;
}

fn reload(app: &mut App, st: &mut State, dir: &Option<PathBuf>) {
    st.saves = dir.as_deref().map(store::list).unwrap_or_default();
    st.checkpoints = dir.as_deref().map(store::list_checkpoints).unwrap_or_default();
    st.dir_exists = dir.as_deref().map(|d| d.is_dir()).unwrap_or(false);
    st.dir = dir.clone();
    st.loaded = true;
    st.refresh = false;
    st.backup.stale = true;

    // Notes for saves that are gone from this folder go quietly. Only when the folder
    // could be read: an unplugged disk must not cost anyone their notes.
    if let Some(d) = dir.as_deref().filter(|d| std::fs::read_dir(d).is_ok()) {
        let present: HashSet<String> = st.saves.iter().map(|s| config::mod_key(&s.path)).collect();
        let removed = store::prune_notes(&mut app.cfg.save_notes, d, |k| present.contains(k) || Path::new(k).exists());
        if removed > 0 {
            app.touch();
        }
    }
}

fn page(app: &mut App, st: &mut State, ui: &mut Ui) {
    theme::page_header(
        ui,
        "Saves & checkpoints",
        "Browse your saves, keep verified checkpoints, and look at a campaign's galaxy.",
    );

    let dir = app.saves_dir();
    if !st.loaded || st.refresh || st.dir != dir {
        if st.loaded && st.dir != dir {
            st.map.clear();
            st.confirm = None;
            st.note_edit = None;
        }
        reload(app, st, &dir);
    }
    // COMMAND_CENTER_PAGE=checkpoints|map opens this page on that tab (once).
    if let Some(tab) = st.start_tab.take() {
        match tab.as_str() {
            "checkpoints" => st.tab = Tab::Checkpoints,
            "map" => {
                st.tab = Tab::Map;
                if let Some(p) = st.saves.iter().find(|s| s.is_campaign() && s.error.is_none()).map(|s| s.path.clone()) {
                    st.map.request(p);
                }
            }
            // `State::open_map`: the campaign map of one particular save.
            other => {
                if let Some(path) = other.strip_prefix("map:") {
                    st.tab = Tab::Map;
                    st.map.request(PathBuf::from(path));
                }
            }
        }
    }

    poll_job(app, st, &dir);
    let backup = app.cfg.settings.backup_dir.clone();
    if st.tab == Tab::Checkpoints {
        keep_backup_view_fresh(st, ui.ctx(), &dir, backup.as_deref());
    }

    let mut action: Option<Action> = None;
    top_bar(ui, st, dir.as_deref(), &mut action);
    ui.add_space(12.0);

    match st.tab {
        Tab::Saves => saves_tab(ui, st, &app.cfg.save_notes, &mut action),
        Tab::Checkpoints => checkpoints_tab(ui, st, app.runner.is_running(), backup.as_deref(), &mut action),
        Tab::Map => campaign_map::show(&mut st.map, app, &st.saves, ui),
    }

    confirm_modal(ui.ctx(), st, &mut action);
    note_modal(ui.ctx(), app, st);

    if let Some(a) = action {
        run(app, st, ui.ctx(), dir, a);
    }
}

// ------------------------------------------------------------------ header

fn top_bar(ui: &mut Ui, st: &mut State, dir: Option<&Path>, action: &mut Option<Action>) {
    ui.horizontal(|ui| {
        egui::Frame::new()
            .fill(theme::FIELD)
            .stroke(Stroke::new(1.0_f32, theme::BORDER))
            .corner_radius(CornerRadius::same(10))
            .inner_margin(Margin::same(3))
            .show(ui, |ui| {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 2.0;
                    for (tab, label) in
                        [(Tab::Saves, "Saves"), (Tab::Checkpoints, "Checkpoints"), (Tab::Map, "Campaign map")]
                    {
                        let on = st.tab == tab;
                        let text = if on {
                            RichText::new(label).color(Color32::WHITE).family(theme::semibold())
                        } else {
                            theme::muted(label)
                        };
                        let button = egui::Button::new(text)
                            .fill(if on { theme::ACCENT_DIM } else { Color32::TRANSPARENT })
                            .stroke(Stroke::NONE)
                            .corner_radius(CornerRadius::same(8));
                        if ui.add(button).clicked() {
                            st.tab = tab;
                        }
                    }
                });
            });

        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if theme::icon_button(ui, icon::REFRESH, "Refresh").clicked() {
                *action = Some(Action::Reload);
            }
            match dir {
                Some(d) => {
                    ui.add_enabled_ui(st.dir_exists, |ui| {
                        let r = theme::button(ui, "Open")
                            .on_disabled_hover_text("This folder is created the first time the game saves.");
                        if r.clicked() {
                            *action = Some(Action::OpenFolder);
                        }
                    });
                    ui.add(egui::Label::new(theme::faint(d.display().to_string())).truncate());
                }
                None => {
                    ui.label(theme::faint("The save folder couldn't be worked out."));
                }
            }
        });
    });
}

/// A list row with trailing actions. Looks like `theme::list_row`, but without a click
/// area over the whole row, which would sit on top of the buttons inside it.
fn row(ui: &mut Ui, height: f32, actions: impl FnOnce(&mut Ui), body: impl FnOnce(&mut Ui)) {
    let out = egui::Frame::new()
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(10, 7))
        .show(ui, |ui| {
            ui.allocate_ui_with_layout(
                vec2(ui.available_width(), height),
                Layout::right_to_left(Align::Center),
                |ui| {
                    actions(ui);
                    ui.with_layout(Layout::top_down(Align::Min), |ui| {
                        ui.spacing_mut().item_spacing.y = 2.0;
                        ui.spacing_mut().interact_size.y = 20.0;
                        body(ui);
                    });
                },
            );
        });
    if ui.rect_contains_pointer(out.response.rect) {
        ui.painter().rect_stroke(
            out.response.rect,
            CornerRadius::same(8),
            Stroke::new(1.0_f32, theme::BORDER_HI),
            StrokeKind::Inside,
        );
    }
}

/// A pill that stays lit while its filter is on.
fn toggle_chip(ui: &mut Ui, on: bool, label: &str) -> egui::Response {
    let (fill, stroke, fg) = if on {
        (theme::ACCENT_DIM, theme::ACCENT.gamma_multiply(0.7), theme::ACCENT_HI)
    } else {
        (theme::CARD_HI, theme::BORDER, theme::MUTED)
    };
    ui.add(
        egui::Button::new(RichText::new(label).size(12.5).color(fg))
            .fill(fill)
            .stroke(Stroke::new(1.0_f32, stroke))
            .corner_radius(CornerRadius::same(255)),
    )
}

// ------------------------------------------------------------------ saves

fn saves_tab(ui: &mut Ui, st: &mut State, notes: &BTreeMap<String, SaveNote>, action: &mut Option<Action>) {
    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
        theme::card(ui, |ui| {
            if st.saves.is_empty() {
                let body = if st.dir_exists {
                    "Saves you make in the game show up here."
                } else {
                    "The save folder doesn't exist yet. It's created the first time the game saves."
                };
                theme::empty_state(ui, icon::SAVE, "No saves yet", body);
                return;
            }

            // Tags in use on the saves listed here, with how many saves carry each.
            let note_of = |s: &SaveInfo| notes.get(&config::mod_key(&s.path));
            let mut tag_counts: BTreeMap<String, usize> = BTreeMap::new();
            for s in &st.saves {
                for t in note_of(s).map(|n| n.tags.as_slice()).unwrap_or_default() {
                    *tag_counts.entry(t.clone()).or_default() += 1;
                }
            }
            st.tag_filter.retain(|t| tag_counts.contains_key(t));

            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::SEARCH).color(theme::FAINT));
                ui.add(
                    egui::TextEdit::singleline(&mut st.query)
                        .hint_text("Search name, campaign, faction, notes, tags")
                        .desired_width(300.0),
                );
                if !st.query.is_empty() && theme::icon_button(ui, icon::CLOSE, "Clear the search").clicked() {
                    st.query.clear();
                }
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    egui::ComboBox::from_id_salt("saves_sort").width(96.0).selected_text(st.sort.label()).show_ui(ui, |ui| {
                        for sort in SaveSort::ALL {
                            ui.selectable_value(&mut st.sort, sort, sort.label());
                        }
                    });
                    ui.label(theme::muted("Sort"));
                });
            });
            if !tag_counts.is_empty() {
                ui.horizontal_wrapped(|ui| {
                    ui.spacing_mut().item_spacing = vec2(6.0, 4.0);
                    ui.label(theme::muted("Tags"));
                    for (tag, count) in &tag_counts {
                        let on = st.tag_filter.contains(tag);
                        if toggle_chip(ui, on, &format!("{tag}  {count}")).on_hover_text("Show only saves with this tag").clicked() {
                            if on {
                                st.tag_filter.remove(tag);
                            } else {
                                st.tag_filter.insert(tag.clone());
                            }
                        }
                    }
                });
            }

            let query = st.query.trim();
            let mut shown: Vec<&SaveInfo> = st
                .saves
                .iter()
                .filter(|s| {
                    let note = note_of(s);
                    st.tag_filter.iter().all(|t| note.map(|n| n.tags.contains(t)).unwrap_or(false))
                        && (query.is_empty() || store::matches_query(&store::search_text(s, note), query))
                })
                .collect();
            store::sort_saves(&mut shown, st.sort);

            ui.add_space(2.0);
            let (n, m) = (shown.len(), st.saves.len());
            theme::overline(
                ui,
                &if n != m {
                    format!("{n} of {m} saves")
                } else if m == 1 {
                    "1 save".to_owned()
                } else {
                    format!("{m} saves")
                },
            );
            if shown.is_empty() {
                theme::empty_state(ui, icon::SEARCH, "No saves match", "Change the search or the tags to see more.");
                return;
            }
            ui.spacing_mut().item_spacing.y = 2.0;
            for s in shown {
                save_row(ui, s, note_of(s), action);
            }
        });
        ui.add_space(24.0);
    });
}

fn save_row(ui: &mut Ui, s: &SaveInfo, note: Option<&SaveNote>, action: &mut Option<Action>) {
    let ok = s.error.is_none();
    let text = note.map(|n| n.note.trim()).filter(|t| !t.is_empty());
    row(
        ui,
        if text.is_some() { 62.0 } else { 42.0 },
        |ui| {
            if theme::icon_button(ui, icon::DELETE, "Delete").clicked() {
                *action = Some(Action::Ask(Confirm::DeleteSave(s.path.clone(), s.name.clone())));
            }
            if theme::icon_button(ui, icon::FOLDER_OPEN, "Reveal in Explorer").clicked() {
                *action = Some(Action::Reveal(s.path.clone()));
            }
            let tip = if note.is_some() { "Edit note and tags" } else { "Add a note or tags" };
            if theme::icon_button(ui, icon::EDIT, tip).clicked() {
                *action = Some(Action::EditNote(s.path.clone(), s.name.clone()));
            }
            if ok && s.is_campaign() && theme::button(ui, "View map").clicked() {
                *action = Some(Action::ViewMap(s.path.clone()));
            }
        },
        |ui| {
            ui.horizontal(|ui| {
                ui.add(egui::Label::new(theme::strong(s.name.clone())).truncate());
                if ok {
                    if s.is_campaign() {
                        theme::chip(ui, "Campaign", theme::ACCENT_HI);
                    } else {
                        theme::chip(ui, "Skirmish", theme::MUTED);
                    }
                }
                for tag in note.map(|n| n.tags.as_slice()).unwrap_or_default() {
                    theme::chip(ui, tag.clone(), TAG);
                }
            });
            match &s.error {
                Some(e) => {
                    ui.add(egui::Label::new(RichText::new(e.clone()).color(theme::WARN)).truncate());
                }
                None => {
                    let mut parts = vec![s.display_title()];
                    if !s.faction.is_empty() {
                        parts.push(pretty(&s.faction));
                    }
                    if s.is_campaign() {
                        parts.push(format!("Week {}", s.week));
                    }
                    if s.difficulty.is_some() {
                        parts.push(s.difficulty_label().to_owned());
                    }
                    parts.push(theme::fmt_time(s.when()));
                    parts.push(theme::fmt_bytes(s.size));
                    ui.add(egui::Label::new(theme::muted(parts.join("  ·  "))).truncate());
                }
            }
            if let Some(text) = text {
                // One line in the list; the whole note on hover.
                let first = text.lines().next().unwrap_or_default();
                let line = if text.lines().count() > 1 { format!("{first} …") } else { first.to_owned() };
                ui.add(egui::Label::new(RichText::new(line).color(theme::TEXT).italics().size(13.0)).truncate())
                    .on_hover_text(text);
            }
        },
    );
}

/// The note editor. Saving an empty note with no tags removes the entry.
fn note_modal(ctx: &egui::Context, app: &mut App, st: &mut State) {
    let Some(edit) = st.note_edit.as_mut() else { return };
    let key = config::mod_key(&edit.path);
    // Tags used on other saves, offered as one-click suggestions.
    let mut known: BTreeSet<String> = BTreeSet::new();
    for (k, n) in &app.cfg.save_notes {
        if *k != key {
            known.extend(n.tags.iter().cloned());
        }
    }
    let add = |tags: &mut Vec<String>, raw: &str| {
        if let Some(tag) = store::normalize_tag(raw) {
            if !tags.contains(&tag) {
                tags.push(tag);
            }
        }
    };

    let (mut save, mut cancel) = (false, false);
    let resp = egui::Modal::new(egui::Id::new("save_note_edit")).show(ctx, |ui| {
        ui.set_width(460.0);
        ui.label(theme::title(format!("Note for \"{}\"", edit.name)));
        ui.add_space(2.0);
        let note = ui.add(
            egui::TextEdit::multiline(&mut edit.note)
                .hint_text("What was going on, what to try next…")
                .desired_rows(4)
                .desired_width(f32::INFINITY),
        );
        if std::mem::take(&mut edit.focus) {
            note.request_focus();
        }

        ui.add_space(6.0);
        theme::overline(ui, "Tags");
        ui.horizontal_wrapped(|ui| {
            ui.spacing_mut().item_spacing = vec2(6.0, 4.0);
            let mut remove = None;
            for (i, tag) in edit.tags.iter().enumerate() {
                let chip = egui::Button::new(RichText::new(format!("{tag}  {}", icon::CLOSE)).size(12.5).color(TAG))
                    .fill(TAG.gamma_multiply(0.14))
                    .stroke(Stroke::NONE)
                    .corner_radius(CornerRadius::same(255));
                if ui.add(chip).on_hover_text("Remove this tag").clicked() {
                    remove = Some(i);
                }
            }
            if let Some(i) = remove {
                edit.tags.remove(i);
            }
            let input = ui.add(
                egui::TextEdit::singleline(&mut edit.input).hint_text("Add a tag, then Enter").desired_width(150.0),
            );
            // A comma finishes a tag as it is typed; Enter finishes the one in the box.
            while let Some((done, rest)) = edit.input.split_once(',').map(|(a, b)| (a.to_owned(), b.to_owned())) {
                add(&mut edit.tags, &done);
                edit.input = rest;
            }
            if input.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)) {
                let typed = std::mem::take(&mut edit.input);
                add(&mut edit.tags, &typed);
                input.request_focus();
            }
        });
        let typed = edit.input.trim().to_lowercase();
        let suggestions: Vec<String> =
            known.iter().filter(|t| !edit.tags.contains(t) && t.contains(typed.as_str())).take(10).cloned().collect();
        if !suggestions.is_empty() {
            let mut picked = None;
            ui.horizontal_wrapped(|ui| {
                ui.spacing_mut().item_spacing = vec2(6.0, 4.0);
                ui.label(theme::faint("Used before:"));
                for t in &suggestions {
                    if toggle_chip(ui, false, t).on_hover_text("Add this tag").clicked() {
                        picked = Some(t.clone());
                    }
                }
            });
            if let Some(tag) = picked {
                add(&mut edit.tags, &tag);
                edit.input.clear();
            }
        }

        ui.add_space(10.0);
        ui.horizontal(|ui| {
            save = theme::primary_button(ui, "Save").clicked();
            cancel = theme::button(ui, "Cancel").clicked();
            ui.label(theme::faint("Notes are kept by the launcher. The save file isn't changed."));
        });
    });

    if save {
        if let Some(mut edit) = st.note_edit.take() {
            // A tag still sitting in the box counts.
            let typed = std::mem::take(&mut edit.input);
            add(&mut edit.tags, &typed);
            let note = SaveNote { note: edit.note.trim().to_owned(), tags: edit.tags };
            let changed = if note.note.is_empty() && note.tags.is_empty() {
                app.cfg.save_notes.remove(&key).is_some()
            } else {
                app.cfg.save_notes.insert(key, note.clone()) != Some(note)
            };
            if changed {
                app.touch();
            }
        }
    } else if cancel || resp.should_close() {
        st.note_edit = None;
    }
}

// ------------------------------------------------------------------ checkpoints

fn checkpoints_tab(ui: &mut Ui, st: &mut State, running: bool, backup: Option<&Path>, action: &mut Option<Action>) {
    let busy = st.job.as_ref().map(|j| j.what);
    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
        theme::card(ui, |ui| {
            ui.label(theme::title("New checkpoint"));
            ui.label(theme::muted(
                "Checkpoints are copies of the whole save folder, verified with SHA-256 checksums.",
            ));
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.add_enabled_ui(!running && busy.is_none() && st.dir.is_some(), |ui| {
                    let w = (ui.available_width() - 170.0).clamp(120.0, 520.0);
                    let edit = ui.add(
                        egui::TextEdit::singleline(&mut st.new_label)
                            .hint_text("Label, e.g. Before attacking Kuat")
                            .desired_width(w),
                    );
                    let enter = edit.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
                    if theme::primary_button(ui, "Create checkpoint").clicked() || enter {
                        *action = Some(Action::Create);
                    }
                });
            });
            if running {
                ui.horizontal(|ui| {
                    ui.label(RichText::new(icon::WARNING).color(theme::WARN));
                    ui.label(RichText::new(
                        "The game is running and may be writing saves. Close it to create or restore a checkpoint.",
                    )
                    .color(theme::WARN));
                });
            }
            if let Some(what) = busy {
                ui.horizontal(|ui| {
                    ui.add(egui::Spinner::new().size(16.0));
                    ui.label(theme::muted(what));
                });
            }
        });
        ui.add_space(12.0);

        backup_card(ui, st, backup, busy.is_some(), action);
        ui.add_space(12.0);

        theme::card(ui, |ui| {
            if st.checkpoints.is_empty() {
                theme::empty_state(
                    ui,
                    icon::HISTORY,
                    "No checkpoints yet",
                    "Create one before a risky move so you can come back to it.",
                );
                return;
            }
            let n = st.checkpoints.len();
            theme::overline(ui, &if n == 1 { "1 checkpoint".to_owned() } else { format!("{n} checkpoints") });
            ui.spacing_mut().item_spacing.y = 2.0;
            for cp in &st.checkpoints {
                let status = backup.and_then(|_| st.backup.status.get(&cp.dir));
                checkpoint_row(ui, cp, status, running, busy.is_some(), action);
            }
        });
        ui.add_space(24.0);
    });
}

fn backup_card(ui: &mut Ui, st: &State, backup: Option<&Path>, busy: bool, action: &mut Option<Action>) {
    theme::card(ui, |ui| {
        ui.horizontal(|ui| {
            ui.label(theme::title("Backup folder"));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                ui.add_enabled_ui(!busy, |ui| {
                    if backup.is_some() {
                        if theme::button(ui, "Clear").on_hover_text("Stop copying checkpoints there. Nothing is deleted.").clicked() {
                            *action = Some(Action::ClearBackup);
                        }
                        if theme::button(ui, "Open").clicked() {
                            *action = Some(Action::OpenBackup);
                        }
                    }
                    if theme::button(ui, "Choose…").clicked() {
                        *action = Some(Action::ChooseBackup);
                    }
                });
            });
        });
        match backup {
            Some(b) => ui.add(egui::Label::new(theme::mono(b.display().to_string())).truncate()),
            None => ui.label(theme::muted("Not set")),
        };
        ui.add(
            egui::Label::new(theme::faint(
                "Every new checkpoint is also copied here and checked file by file. Point it at a OneDrive, Dropbox or NAS folder to keep a copy that isn't on this disk.",
            ))
            .wrap(),
        );
        let Some(backup) = backup else { return };

        let view = &st.backup;
        let scanned = view.scanned_for.as_ref().map(|(b, d)| same_path(b, backup) && *d == st.dir).unwrap_or(false);
        if scanned && !view.available {
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::WARNING).color(theme::WARN));
                ui.add(
                    egui::Label::new(
                        RichText::new("This folder isn't available right now, so checkpoints aren't being copied to it.")
                            .color(theme::WARN),
                    )
                    .wrap(),
                );
            });
        }
        ui.add_space(2.0);
        ui.horizontal(|ui| {
            ui.add_enabled_ui(!busy && !st.checkpoints.is_empty(), |ui| {
                if theme::button(ui, "Check backups")
                    .on_hover_text("Re-hash every file in the backup folder against the checkpoints' checksums")
                    .clicked()
                {
                    *action = Some(Action::CheckBackups);
                }
                if theme::button(ui, "Back up all now")
                    .on_hover_text("Copy every checkpoint that is missing or different, and verify the copies")
                    .clicked()
                {
                    *action = Some(Action::BackUpAll);
                }
            });
            if scanned && view.available && !st.checkpoints.is_empty() {
                let ok = st
                    .checkpoints
                    .iter()
                    .filter(|c| matches!(view.status.get(&c.dir), Some(BackupStatus::BackedUp { .. })))
                    .count();
                let mut line = format!("{ok} of {} backed up", st.checkpoints.len());
                if let Some(t) = view.checked {
                    line.push_str(&format!("  ·  every file checked {}", theme::fmt_time(t)));
                }
                ui.label(theme::faint(line));
            }
        });

        if scanned && !view.only.is_empty() {
            ui.add_space(6.0);
            theme::separator(ui);
            ui.add_space(2.0);
            theme::overline(ui, &format!("Only in the backup folder ({})", view.only.len()));
            ui.add(
                egui::Label::new(theme::muted(
                    "These checkpoints aren't on this computer. Importing one copies it back, verified, so it can be restored.",
                ))
                .wrap(),
            );
            ui.spacing_mut().item_spacing.y = 2.0;
            for only in &view.only {
                let cp = &only.checkpoint;
                row(
                    ui,
                    42.0,
                    |ui| {
                        ui.add_enabled_ui(!busy, |ui| {
                            if theme::button(ui, "Import").clicked() {
                                *action = Some(Action::Import(cp.clone()));
                            }
                        });
                    },
                    |ui| {
                        ui.horizontal(|ui| {
                            ui.add(egui::Label::new(theme::strong(store::display_label(cp))).truncate());
                            if !only.same_folder {
                                theme::chip(ui, "Another save folder", theme::WARN)
                                    .on_hover_text(format!("Taken from {}", cp.manifest.source.display()));
                            }
                        });
                        ui.add(egui::Label::new(theme::muted(checkpoint_line(cp))).truncate());
                    },
                );
            }
        }
    });
}

fn checkpoint_line(cp: &Checkpoint) -> String {
    let created = std::time::UNIX_EPOCH + std::time::Duration::from_secs(cp.manifest.created.max(0) as u64);
    let saves = match cp.save_count() {
        1 => "1 save".to_owned(),
        n => format!("{n} saves"),
    };
    format!("{}  ·  {}  ·  {}", theme::fmt_time(created), saves, theme::fmt_bytes(cp.total_size()))
}

fn checkpoint_row(
    ui: &mut Ui,
    cp: &Checkpoint,
    status: Option<&BackupStatus>,
    running: bool,
    busy: bool,
    action: &mut Option<Action>,
) {
    row(
        ui,
        42.0,
        |ui| {
            ui.add_enabled_ui(!busy, |ui| {
                if theme::icon_button(ui, icon::DELETE, "Delete").clicked() {
                    *action = Some(Action::Ask(Confirm::DeleteCheckpoint(cp.clone(), None)));
                }
            });
            ui.add_enabled_ui(!running && !busy, |ui| {
                let r = theme::button(ui, "Restore").on_disabled_hover_text(if running {
                    "Close the game before restoring a checkpoint."
                } else {
                    "Wait for the current job to finish."
                });
                if r.clicked() {
                    *action = Some(Action::Ask(Confirm::Restore(cp.clone())));
                }
            });
            ui.add_enabled_ui(!busy, |ui| {
                if theme::button(ui, "Verify").on_hover_text("Check every file against its checksum").clicked() {
                    *action = Some(Action::Verify(cp.clone()));
                }
            });
        },
        |ui| {
            ui.horizontal(|ui| {
                ui.add(egui::Label::new(theme::strong(store::display_label(cp))).truncate());
                if cp.manifest.safety {
                    theme::chip(ui, "Safety", theme::WARN)
                        .on_hover_text("Taken automatically before a restore");
                }
                if cp.manifest.auto {
                    theme::chip(ui, "Auto", theme::MUTED)
                        .on_hover_text("Taken automatically by the launcher. Old ones are removed.");
                }
                match status {
                    Some(BackupStatus::BackedUp { verified: true }) => {
                        theme::chip(ui, format!("Backed up {}", icon::CHECK), theme::GOOD)
                            .on_hover_text("Every file in the backup folder matches its checksum.");
                    }
                    Some(BackupStatus::BackedUp { verified: false }) => {
                        theme::chip(ui, "Backed up", theme::GOOD).on_hover_text(
                            "The backup folder has this checkpoint with the same files and sizes. Check backups re-hashes every file.",
                        );
                    }
                    Some(BackupStatus::Missing) => {
                        theme::chip(ui, "Not backed up", theme::MUTED)
                            .on_hover_text("The backup folder has no copy. Back up all now makes one.");
                    }
                    Some(BackupStatus::Differs(why)) => {
                        theme::chip(ui, "Backup differs", theme::WARN)
                            .on_hover_text(format!("{why} Back up all now repairs the copy."));
                    }
                    None => {}
                }
            });
            ui.add(egui::Label::new(theme::muted(checkpoint_line(cp))).truncate());
        },
    );
}

// ------------------------------------------------------------------ background jobs

fn spawn(st: &mut State, ctx: &egui::Context, what: &'static str, work: impl FnOnce() -> Done + Send + 'static) {
    let (tx, rx) = mpsc::channel();
    let ctx = ctx.clone();
    std::thread::spawn(move || {
        let _ = tx.send(work());
        ctx.request_repaint();
    });
    st.job = Some(Job { what, rx });
}

/// Looks at the backup folder again (manifests and sizes only) whenever the checkpoint
/// list, the save folder or the backup folder changed.
fn keep_backup_view_fresh(st: &mut State, ctx: &egui::Context, dir: &Option<PathBuf>, backup: Option<&Path>) {
    let Some(backup) = backup else {
        st.backup = BackupView::default();
        return;
    };
    let key: ScanKey = (backup.to_path_buf(), dir.clone());
    if st.job.is_some() || (!st.backup.stale && st.backup.scanned_for.as_ref() == Some(&key)) {
        return;
    }
    scan_backup(st, ctx, key, false);
}

fn scan_backup(st: &mut State, ctx: &egui::Context, key: ScanKey, deep: bool) {
    st.backup.stale = false;
    let checkpoints = st.checkpoints.clone();
    let what = if deep { "Checking every file in the backup folder…" } else { "Looking at the backup folder…" };
    spawn(st, ctx, what, move || {
        let available = key.0.is_dir();
        let status = if available {
            checkpoints.iter().map(|c| (c.dir.clone(), store::backup_status(c, &key.0, deep))).collect()
        } else {
            Vec::new()
        };
        let only = match &key.1 {
            Some(d) if available => store::list_backup_only(d, &key.0),
            _ => Vec::new(),
        };
        Done::Scanned { key, deep, available, status, only }
    });
}

fn poll_job(app: &mut App, st: &mut State, dir: &Option<PathBuf>) {
    let Some(job) = &st.job else { return };
    let done = match job.rx.try_recv() {
        Ok(d) => d,
        Err(TryRecvError::Empty) => return,
        Err(TryRecvError::Disconnected) => {
            st.job = None;
            app.err("The job stopped before it finished. Try again.");
            return;
        }
    };
    st.job = None;
    let warn = |app: &mut App, cp: &Checkpoint| {
        if let Some(w) = &cp.backup_warning {
            app.err(w.clone());
        }
    };
    match done {
        Done::Scanned { key, deep, available, status, only } => {
            let view = &mut st.backup;
            let old = std::mem::take(&mut view.status);
            let same = view.scanned_for.as_ref() == Some(&key);
            for (cp_dir, new) in status {
                // A quick look doesn't take back what a full check established.
                let keep = !deep
                    && same
                    && new == BackupStatus::BackedUp { verified: false }
                    && old.get(&cp_dir) == Some(&BackupStatus::BackedUp { verified: true });
                view.status.insert(cp_dir, if keep { BackupStatus::BackedUp { verified: true } } else { new });
            }
            view.only = only;
            view.available = available;
            if deep {
                view.checked = available.then(std::time::SystemTime::now);
                if !available {
                    app.err(format!("The backup folder {} isn't available.", key.0.display()));
                } else {
                    let bad = view.status.values().filter(|s| matches!(s, BackupStatus::Differs(_))).count();
                    let missing = view.status.values().filter(|s| matches!(s, BackupStatus::Missing)).count();
                    let good = view.status.len() - bad - missing;
                    if bad == 0 && missing == 0 {
                        app.ok(format!("All {good} backup copies are intact. Every file matches its checksum."));
                    } else {
                        app.err(format!(
                            "{good} backup copies are intact, {bad} differ and {missing} are missing. Back up all now puts that right."
                        ));
                    }
                }
            } else if !same {
                view.checked = None;
            }
            view.scanned_for = Some(key);
            return;
        }
        Done::Created(Ok(cp)) => {
            let backed_up = app.cfg.settings.backup_dir.is_some() && cp.backup_warning.is_none();
            app.ok(format!(
                "Checkpoint \"{}\" created and verified ({}){}.",
                store::display_label(&cp),
                theme::fmt_bytes(cp.total_size()),
                if backed_up { ", and copied to the backup folder" } else { "" }
            ));
            warn(app, &cp);
            st.new_label.clear();
        }
        Done::Created(Err(e)) => app.err(e),
        Done::Verified(label, Ok(())) => {
            app.ok(format!("\"{label}\" is intact. Every file matches its checksum."));
            return;
        }
        Done::Verified(_, Err(e)) => {
            app.err(e);
            return;
        }
        Done::Restored(label, Ok(safety)) => {
            st.map.clear();
            match &safety {
                Some(s) => {
                    app.ok(format!(
                        "Restored \"{label}\". Your previous saves were kept as the safety checkpoint \"{}\".",
                        store::display_label(s)
                    ));
                    warn(app, s);
                }
                None => app.ok(format!("Restored \"{label}\". The folder was empty, so no safety checkpoint was needed.")),
            }
        }
        Done::Restored(_, Err(e)) => {
            // A failed restore may still have replaced some files.
            st.map.clear();
            app.err(e);
        }
        Done::Deleted { label, local, mirror } => match (local, mirror) {
            (Err(e), _) => app.err(e),
            (Ok(()), Some(Ok(true))) => {
                app.ok(format!("The checkpoint \"{label}\" was moved to the Recycle Bin and its backup copy deleted."))
            }
            (Ok(()), Some(Err(e))) => {
                app.ok(format!("The checkpoint \"{label}\" was moved to the Recycle Bin."));
                app.err(e);
            }
            (Ok(()), _) => app.ok(format!("The checkpoint \"{label}\" was moved to the Recycle Bin.")),
        },
        Done::BackedUp { done, failed } => {
            if done > 0 || failed.is_empty() {
                app.ok(match done {
                    1 => "1 checkpoint is in the backup folder, every file verified.".to_owned(),
                    n => format!("{n} checkpoints are in the backup folder, every file verified."),
                });
            }
            if let Some(first) = failed.first() {
                app.err(match failed.len() {
                    1 => first.clone(),
                    n => format!("{first} ({} more couldn't be copied either.)", n - 1),
                });
            }
        }
        Done::Imported(Ok(cp)) => app.ok(format!(
            "\"{}\" was copied back from the backup folder and verified. It can be restored like any other checkpoint.",
            store::display_label(&cp)
        )),
        Done::Imported(Err(e)) => app.err(e),
    }
    reload(app, st, dir);
}

// ------------------------------------------------------------------ confirm + actions

fn confirm_modal(ctx: &egui::Context, st: &mut State, action: &mut Option<Action>) {
    let (title, body, button, danger) = match &st.confirm {
        None => return,
        Some(Confirm::DeleteSave(_, name)) => (
            format!("Delete \"{name}\"?"),
            "Deleted saves go to the Recycle Bin. Its note and tags are removed.".to_owned(),
            "Delete",
            true,
        ),
        Some(Confirm::Restore(cp)) => (
            format!("Restore \"{}\"?", store::display_label(cp)),
            "The checkpoint is checked first. Your current saves are copied into a safety checkpoint, then replaced."
                .to_owned(),
            "Restore",
            false,
        ),
        Some(Confirm::DeleteCheckpoint(cp, _)) => (
            format!("Delete the checkpoint \"{}\"?", store::display_label(cp)),
            "The checkpoint goes to the Recycle Bin. Your current saves aren't touched.".to_owned(),
            "Delete",
            true,
        ),
    };
    let mut go = false;
    let mut cancel = false;
    let resp = egui::Modal::new(egui::Id::new("saves_confirm")).show(ctx, |ui| {
        ui.set_width(420.0);
        ui.label(theme::title(title));
        ui.add_space(2.0);
        ui.add(egui::Label::new(theme::muted(body)).wrap());
        // The copy in the backup folder is kept unless asked otherwise.
        if let Some(Confirm::DeleteCheckpoint(_, Some(also))) = &mut st.confirm {
            ui.add_space(4.0);
            ui.checkbox(also, "Also delete its copy in the backup folder");
            ui.label(theme::faint(if *also {
                "The copy is deleted too. If the backup folder has no Recycle Bin, it is gone for good."
            } else {
                "The copy in the backup folder is kept. It can be imported again later."
            }));
        }
        ui.add_space(10.0);
        ui.horizontal(|ui| {
            let r = if danger { theme::danger_button(ui, button) } else { theme::primary_button(ui, button) };
            if r.clicked() {
                go = true;
            }
            if theme::button(ui, "Cancel").clicked() {
                cancel = true;
            }
        });
    });
    if go {
        if let Some(c) = st.confirm.take() {
            *action = Some(Action::Confirmed(c));
        }
    } else if cancel || resp.should_close() {
        st.confirm = None;
    }
}

fn run(app: &mut App, st: &mut State, ctx: &egui::Context, dir: Option<PathBuf>, action: Action) {
    const BUSY: &str = "The game is running and may be writing saves. Close it first.";
    const NO_DIR: &str = "The save folder couldn't be worked out.";
    const WAIT: &str = "Another checkpoint job is still running. Try again when it has finished.";
    let backup = app.cfg.settings.backup_dir.clone();
    // The checkpoint actions each start a background job, and those run one at a time.
    let needs_worker = matches!(
        action,
        Action::Create
            | Action::Verify(_)
            | Action::CheckBackups
            | Action::BackUpAll
            | Action::Import(_)
            | Action::Confirmed(Confirm::Restore(_))
            | Action::Confirmed(Confirm::DeleteCheckpoint(..))
    );
    if needs_worker && st.job.is_some() {
        app.err(WAIT);
        return;
    }
    match action {
        Action::Reload => reload(app, st, &dir),
        Action::OpenFolder => {
            if let Some(d) = &dir {
                open_in_explorer(d);
            }
        }
        Action::Reveal(p) => reveal_in_explorer(&p),
        Action::ViewMap(p) => {
            st.tab = Tab::Map;
            st.map.request(p);
        }
        Action::Ask(Confirm::DeleteCheckpoint(cp, _)) => {
            // Offer to delete the backup copy only when there is one.
            let has_copy = backup.as_deref().map(|b| store::has_mirror(&cp, b)).unwrap_or(false);
            st.confirm = Some(Confirm::DeleteCheckpoint(cp, has_copy.then_some(false)));
        }
        Action::Ask(c) => st.confirm = Some(c),
        Action::EditNote(path, name) => {
            let existing = app.cfg.save_notes.get(&config::mod_key(&path)).cloned().unwrap_or_default();
            st.note_edit = Some(NoteEdit {
                path,
                name,
                note: existing.note,
                tags: existing.tags,
                input: String::new(),
                focus: true,
            });
        }
        Action::ChooseBackup => {
            let mut dialog = rfd::FileDialog::new().set_title("Choose a folder for checkpoint backups");
            if let Some(current) = backup.as_deref().filter(|b| b.is_dir()) {
                dialog = dialog.set_directory(current);
            }
            let Some(picked) = dialog.pick_folder() else { return };
            // The launcher's own checkpoints folder (or anything inside it) can't back itself up.
            let own = config::mod_key(&config::data_dir().join("checkpoints"));
            let key = config::mod_key(&picked);
            if key == own || key.starts_with(&format!("{own}\\")) {
                app.err("That is where the launcher keeps the checkpoints themselves. Choose a folder somewhere else, ideally on another disk.");
                return;
            }
            app.cfg.settings.backup_dir = Some(picked);
            app.touch();
            st.backup = BackupView { stale: true, ..Default::default() };
        }
        Action::ClearBackup => {
            app.cfg.settings.backup_dir = None;
            app.touch();
            st.backup = BackupView::default();
            app.ok("Checkpoints are no longer copied to a backup folder. The copies already there were left alone.");
        }
        Action::OpenBackup => match backup.as_deref() {
            Some(b) if b.is_dir() => open_in_explorer(b),
            Some(b) => app.err(format!("The backup folder {} isn't available right now.", b.display())),
            None => {}
        },
        Action::CheckBackups => {
            if let Some(b) = backup {
                scan_backup(st, ctx, (b, dir), true);
            }
        }
        Action::BackUpAll => {
            let Some(b) = backup else { return };
            let checkpoints = st.checkpoints.clone();
            spawn(st, ctx, "Copying checkpoints to the backup folder…", move || {
                let (done, failed) = store::mirror_all(&checkpoints, &b);
                Done::BackedUp { done, failed }
            });
        }
        Action::Import(cp) => match dir {
            None => app.err(NO_DIR),
            Some(d) => spawn(st, ctx, "Copying the checkpoint back from the backup folder…", move || {
                Done::Imported(store::import_checkpoint(&cp, &d))
            }),
        },
        Action::Create => match dir {
            _ if app.runner.is_running() => app.err(BUSY),
            None => app.err(NO_DIR),
            Some(d) => {
                let label = st.new_label.clone();
                spawn(st, ctx, "Creating the checkpoint…", move || {
                    Done::Created(store::create_checkpoint_mirrored(&d, &label, false, backup.as_deref()))
                });
            }
        },
        Action::Verify(cp) => spawn(st, ctx, "Checking the checkpoint…", move || {
            Done::Verified(store::display_label(&cp), store::verify_checkpoint(&cp))
        }),
        Action::Confirmed(Confirm::DeleteSave(path, name)) => {
            match store::to_recycle_bin(&path) {
                Ok(()) => {
                    st.map.forget(&path);
                    if app.cfg.save_notes.remove(&config::mod_key(&path)).is_some() {
                        app.touch();
                    }
                    app.ok(format!("\"{name}\" was moved to the Recycle Bin."));
                }
                Err(e) => app.err(e),
            }
            reload(app, st, &dir);
        }
        Action::Confirmed(Confirm::DeleteCheckpoint(cp, also)) => {
            let mirror = backup.filter(|_| also == Some(true));
            spawn(st, ctx, "Deleting the checkpoint…", move || {
                let local = store::to_recycle_bin(&cp.dir);
                // The backup copy only goes once the checkpoint itself is gone.
                let mirror = match (&local, mirror) {
                    (Ok(()), Some(b)) => Some(store::delete_mirror(&cp, &b)),
                    _ => None,
                };
                Done::Deleted { label: store::display_label(&cp), local, mirror }
            });
        }
        Action::Confirmed(Confirm::Restore(cp)) => match dir {
            _ if app.runner.is_running() => app.err(BUSY),
            None => app.err(NO_DIR),
            Some(d) => spawn(st, ctx, "Restoring the checkpoint…", move || {
                Done::Restored(store::display_label(&cp), store::restore_checkpoint_mirrored(&cp, &d, backup.as_deref()))
            }),
        },
    }
}

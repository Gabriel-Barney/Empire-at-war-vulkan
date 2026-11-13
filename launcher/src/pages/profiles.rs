//! Profiles: named sets of mods with their own load order, saves, launch options and notes.

use super::graphics::setting_row;
use super::mods::paint_icon;
use crate::app::{open_in_explorer, App};
use crate::config::Objective;
use crate::conflicts::{self, PairOverlap, Report, Severity as ConflictSeverity};
use crate::mods::{self, ModInfo, ModSource};
use crate::profile_io::{self, Installed, ProfileFile, Status as ImportStatus};
use crate::shortcut;
use crate::steam::same_path;
use crate::theme::{self, icon};
use crate::{launch, video, workshop};
use egui::{
    vec2, Align, Align2, CornerRadius, CursorIcon, FontFamily, FontId, Layout, Margin, Rect, RichText, Sense, Stroke,
};
use std::path::{Path, PathBuf};
use std::sync::Arc;

#[derive(Default)]
pub struct State {
    /// Index of the mod row being dragged.
    drag: Option<usize>,
    confirm_delete: Option<u64>,
    new_objective: String,
    /// The conflict and load-order check for the active profile. It runs on a worker
    /// thread; the Play page reads it through [`conflict_report`].
    pub conflicts: conflicts::Checker,
    /// A profile file that was opened and is waiting in the review dialog.
    import: Option<Import>,
    presets: Option<PresetCache>,
}

struct Import {
    file: ProfileFile,
    /// The name the new profile gets; editable in the dialog.
    name: String,
    source: PathBuf,
}

/// Graphics preset names, re-read from disk every couple of seconds.
struct PresetCache {
    read_at: f64,
    names: Vec<String>,
    /// The preset the profile named when this was read, and whether its file was there.
    checked: Option<String>,
    missing: bool,
}

/// Below this width the profile list moves above the editor.
const NARROW: f32 = 720.0;

const FRAME_CAPS: [(Option<i32>, &str); 10] = [
    (None, "Use graphics setting"),
    (Some(0), "No cap"),
    (Some(-1), "Display refresh rate"),
    (Some(30), "30 fps"),
    (Some(60), "60 fps"),
    (Some(90), "90 fps"),
    (Some(120), "120 fps"),
    (Some(144), "144 fps"),
    (Some(165), "165 fps"),
    (Some(240), "240 fps"),
];

pub fn show(app: &mut App, ui: &mut egui::Ui) {
    let mut st = std::mem::take(&mut app.profiles_ui);
    theme::page_header(ui, "Profiles", "Named sets of mods.");
    let report = st.conflicts.poll(&conflict_input(app), ui.ctx(), true);
    let avail = ui.available_size();
    if avail.x < NARROW {
        // A narrow pane (the page shares the window with another): the list becomes a
        // picker above the editor, and everything scrolls together.
        egui::ScrollArea::vertical().id_salt("profile_editor").auto_shrink([false, false]).show(ui, |ui| {
            profile_picker(app, &mut st, ui);
            ui.add_space(6.0);
            let id = app.cfg.active().id;
            ui.push_id(id, |ui| editor(app, &mut st, ui, report.as_deref()));
            ui.add_space(24.0);
        });
    } else {
        ui.horizontal_top(|ui| {
            ui.allocate_ui_with_layout(vec2(270.0, avail.y), Layout::top_down(Align::Min), |ui| {
                profile_list(app, &mut st, ui, avail.y);
            });
            ui.add_space(8.0);
            let w = ui.available_width();
            ui.allocate_ui_with_layout(vec2(w, avail.y), Layout::top_down(Align::Min), |ui| {
                egui::ScrollArea::vertical().id_salt("profile_editor").auto_shrink([false, false]).show(ui, |ui| {
                    let id = app.cfg.active().id;
                    ui.push_id(id, |ui| editor(app, &mut st, ui, report.as_deref()));
                    ui.add_space(24.0);
                });
            });
        });
    }
    delete_modal(app, &mut st, ui.ctx());
    import_modal(app, &mut st, ui.ctx());
    app.profiles_ui = st;
}

// ------------------------------------------------------------------ conflicts (shared with Play)

fn conflict_input(app: &App) -> conflicts::Input {
    conflicts::Input {
        mods: app
            .cfg
            .active()
            .mods
            .iter()
            .map(|path| {
                let info = app.find_mod(path);
                conflicts::ModInput {
                    name: app.mod_name(path),
                    path: path.clone(),
                    workshop_id: info.and_then(|m| m.workshop_id.clone()),
                    present: info.is_some(),
                    version: info.map(|m| m.version.clone()).unwrap_or_default(),
                }
            })
            .collect(),
        foc: mods::foc_dir(&app.steam, &app.cfg.settings),
    }
}

/// The conflict report for the active profile, or `None` while it is being worked out
/// in the background. Starts the check when the profile's mods or their order changed,
/// unless the game is running: reading every mod folder would compete with it for the disk.
pub fn conflict_report(app: &mut App, ctx: &egui::Context) -> Option<Arc<Report>> {
    let input = conflict_input(app);
    let idle = !app.runner.is_running();
    app.profiles_ui.conflicts.poll(&input, ctx, idle)
}

fn plural(n: usize, one: &str, many: &str) -> String {
    if n == 1 {
        format!("1 {one}")
    } else {
        format!("{n} {many}")
    }
}

/// "14 files (3 XML, 212 definitions replaced, 5 dropped)".
fn overlap_text(p: &PairOverlap) -> String {
    let mut text = plural(p.shared_files.len(), "file", "files");
    if p.xml_files > 0 {
        let mut parts = vec![format!("{} XML", p.xml_files)];
        if p.definitions > 0 {
            parts.push(format!("{} replaced", plural(p.definitions, "definition", "definitions")));
        }
        if p.lost_definitions > 0 {
            parts.push(format!("{} dropped", plural(p.lost_definitions, "definition", "definitions")));
        }
        text.push_str(&format!(" ({})", parts.join(", ")));
    }
    text
}

// ------------------------------------------------------------------ left: list

fn profile_list(app: &mut App, st: &mut State, ui: &mut egui::Ui, height: f32) {
    let running = app.runner.running_profile();
    let active = app.cfg.active().id;
    let mut import = false;
    let mut export = false;
    theme::card(ui, |ui| {
        theme::overline(ui, "Profiles");
        let mut select = None;
        egui::ScrollArea::vertical().id_salt("profile_list").max_height((height - 240.0).max(120.0)).show(ui, |ui| {
            ui.spacing_mut().item_spacing.y = 4.0;
            for p in &app.cfg.profiles {
                let (resp, _) = theme::list_row(ui, p.id == active, |ui| {
                    ui.spacing_mut().item_spacing.y = 1.0;
                    let name = if p.name.trim().is_empty() { "Unnamed profile".to_owned() } else { p.name.clone() };
                    ui.add(egui::Label::new(theme::strong(name)).truncate().selectable(false));
                    let mods = if p.mods.is_empty() { "No mods".to_owned() } else { plural(p.mods.len(), "mod", "mods") };
                    let mut line = format!("{mods} · {}", theme::fmt_duration(p.play_seconds));
                    if running == Some(p.id) {
                        line.push_str(" · running");
                    }
                    ui.add(egui::Label::new(theme::faint(line)).truncate().selectable(false));
                });
                if resp.clicked() {
                    select = Some(p.id);
                }
            }
        });
        if let Some(id) = select {
            select_profile(app, st, id);
        }

        ui.add_space(4.0);
        theme::separator(ui);
        ui.add_space(4.0);
        if theme::primary_button(ui, &format!("{}  New profile", icon::ADD)).clicked() {
            new_profile(app, st);
        }
        ui.horizontal(|ui| profile_buttons(app, st, ui, false));
        ui.horizontal(|ui| (import, export) = share_buttons(ui));
    });
    if import {
        start_import(app, st);
    }
    if export {
        export_active(app);
    }
}

/// The profile list for a narrow pane: a drop-down and a row of buttons.
fn profile_picker(app: &mut App, st: &mut State, ui: &mut egui::Ui) {
    let active = app.cfg.active().id;
    let mut import = false;
    let mut export = false;
    theme::card(ui, |ui| {
        theme::overline(ui, "Profile");
        let label = |name: &str| if name.trim().is_empty() { "Unnamed profile".to_owned() } else { name.to_owned() };
        let mut select = None;
        egui::ComboBox::from_id_salt("profile_picker")
            .width(ui.available_width())
            .selected_text(label(&app.cfg.active().name))
            .show_ui(ui, |ui| {
                for p in &app.cfg.profiles {
                    if ui.selectable_label(p.id == active, label(&p.name)).clicked() {
                        select = Some(p.id);
                    }
                }
            });
        if let Some(id) = select {
            select_profile(app, st, id);
        }
        ui.horizontal_wrapped(|ui| {
            profile_buttons(app, st, ui, true);
            (import, export) = share_buttons(ui);
        });
    });
    if import {
        start_import(app, st);
    }
    if export {
        export_active(app);
    }
}

fn select_profile(app: &mut App, st: &mut State, id: u64) {
    if id != app.cfg.active().id {
        app.cfg.active_profile = id;
        st.drag = None;
        st.new_objective.clear();
        app.touch();
    }
}

fn new_profile(app: &mut App, st: &mut State) {
    let id = app.cfg.add_profile("New profile");
    app.cfg.active_profile = id;
    st.drag = None;
    app.touch();
}

/// Duplicate and Delete, and New when `with_new` (the wide list has its own, bigger one).
fn profile_buttons(app: &mut App, st: &mut State, ui: &mut egui::Ui, with_new: bool) {
    let running = app.runner.running_profile();
    let active = app.cfg.active().id;
    if with_new && theme::primary_button(ui, &format!("{}  New", icon::ADD)).clicked() {
        new_profile(app, st);
        return;
    }
    if theme::button(ui, "Duplicate").on_hover_text("Copy the active profile").clicked() {
        if let Some(id) = app.cfg.duplicate_profile(active) {
            app.cfg.active_profile = id;
            st.drag = None;
            app.touch();
        }
    }
    let only = app.cfg.profiles.len() <= 1;
    let delete = ui
        .add_enabled_ui(!only, |ui| theme::danger_button(ui, "Delete"))
        .inner
        .on_disabled_hover_text("The last profile can't be deleted.");
    if delete.clicked() {
        if running == Some(active) {
            app.err("The game is running with this profile. Close the game before deleting it.");
        } else {
            st.confirm_delete = Some(active);
        }
    }
}

/// Import and Export. Returns which was pressed.
fn share_buttons(ui: &mut egui::Ui) -> (bool, bool) {
    let import = theme::button(ui, "Import").on_hover_text("Make a profile from a .ccprofile file").clicked();
    let export = theme::button(ui, "Export").on_hover_text("Save the active profile to a file you can share").clicked();
    (import, export)
}

fn delete_modal(app: &mut App, st: &mut State, ctx: &egui::Context) {
    let Some(id) = st.confirm_delete else { return };
    let Some(name) = app.cfg.profiles.iter().find(|p| p.id == id).map(|p| p.name.clone()) else {
        st.confirm_delete = None;
        return;
    };
    let mut confirm = false;
    let mut cancel = false;
    let modal = egui::Modal::new(egui::Id::new("delete_profile")).show(ctx, |ui| {
        ui.set_width(400.0);
        ui.label(theme::title(format!("Delete \"{name}\"?")));
        ui.label(theme::muted(
            "Its mod list, launch options, objectives and notes are removed. Mods and save files stay on disk.",
        ));
        ui.add_space(8.0);
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if theme::danger_button(ui, "Delete profile").clicked() {
                confirm = true;
            }
            if theme::button(ui, "Cancel").clicked() {
                cancel = true;
            }
        });
    });
    if confirm {
        st.confirm_delete = None;
        if app.cfg.profiles.len() <= 1 {
            app.err("The last profile can't be deleted.");
        } else if app.runner.running_profile() == Some(id) {
            app.err("The game is running with this profile. Close the game before deleting it.");
        } else if let Some(idx) = app.cfg.profiles.iter().position(|p| p.id == id) {
            app.cfg.profiles.remove(idx);
            if app.cfg.active_profile == id || !app.cfg.profiles.iter().any(|p| p.id == app.cfg.active_profile) {
                let next = idx.min(app.cfg.profiles.len() - 1);
                app.cfg.active_profile = app.cfg.profiles[next].id;
            }
            st.drag = None;
            app.touch();
            app.ok(format!("Deleted profile \"{name}\""));
        }
    } else if cancel || modal.should_close() {
        st.confirm_delete = None;
    }
}

// ------------------------------------------------------------------ export and import

fn installed_of(m: &ModInfo) -> Installed {
    Installed {
        path: m.path.clone(),
        folder: m.folder.clone(),
        name: m.name.clone(),
        workshop_id: m.workshop_id.clone(),
        from_workshop: m.source == ModSource::Workshop,
    }
}

fn export_active(app: &mut App) {
    let profile = app.cfg.active().clone();
    let exported =
        profile_io::export(&profile, |p| app.find_mod(p).map(installed_of), app.steam.workshop_dir.as_deref());
    let picked = rfd::FileDialog::new()
        .set_title("Export profile")
        .add_filter("Command Center profile", &[profile_io::EXTENSION])
        .set_file_name(profile_io::default_file_name(&profile.name))
        .save_file();
    let Some(mut path) = picked else { return };
    if path.extension().is_none() {
        path.set_extension(profile_io::EXTENSION);
    }
    match profile_io::save(&path, &exported.file) {
        Ok(()) => {
            let mut text = format!("Exported \"{}\" to {}", profile.name, path.display());
            if exported.dropped_extra_args {
                text.push_str(". The extra arguments were left out because they contain a folder path.");
            }
            app.ok(text);
        }
        Err(e) => app.err(e),
    }
}

fn start_import(app: &mut App, st: &mut State) {
    let picked = rfd::FileDialog::new()
        .set_title("Import profile")
        .add_filter("Command Center profile", &[profile_io::EXTENSION])
        .add_filter("All files", &["*"])
        .pick_file();
    let Some(path) = picked else { return };
    match profile_io::load(&path) {
        Ok(file) => {
            let names: Vec<String> = app.cfg.profiles.iter().map(|p| p.name.clone()).collect();
            st.import = Some(Import { name: profile_io::unique_name(&file.name, &names), file, source: path });
        }
        Err(e) => app.err(e),
    }
}

/// What the review dialog shows next to a Workshop mod that isn't on disk.
struct MissingItem {
    busy: bool,
    subscribed: bool,
    /// "Downloading 40%", when Steam is at it.
    progress: Option<String>,
}

fn import_modal(app: &mut App, st: &mut State, ctx: &egui::Context) {
    let Some(imp) = st.import.as_mut() else { return };
    let installed: Vec<Installed> = app.mods.iter().map(installed_of).collect();
    let workshop_dir = app.steam.workshop_dir.clone();
    let resolved = profile_io::resolve(&imp.file, &installed, workshop_dir.as_deref());
    let names: Vec<String> = app.cfg.profiles.iter().map(|p| p.name.clone()).collect();

    let ws = app.mods_ui.workshop();
    let connected = ws.is_connected();
    let state_of = |id: u64| {
        let status = ws.item_status(id);
        MissingItem {
            busy: ws.is_busy(id),
            subscribed: ws.is_subscribed(id),
            progress: status.downloading.then(|| match status.percent() {
                Some(p) => format!("Downloading {p}%"),
                None => "Download queued".to_owned(),
            }),
        }
    };
    let missing_ws: Vec<u64> = resolved
        .iter()
        .filter_map(|r| match r.status {
            ImportStatus::MissingWorkshop { id, .. } => Some(id),
            _ => None,
        })
        .collect();
    let missing_local = resolved.iter().filter(|r| r.status == ImportStatus::MissingLocal).count();
    let to_subscribe: Vec<u64> = missing_ws
        .iter()
        .copied()
        .filter(|id| {
            let s = state_of(*id);
            !s.busy && !s.subscribed
        })
        .collect();
    let preset_missing = imp
        .file
        .video_preset
        .as_deref()
        .is_some_and(|name| !st.presets.as_ref().is_some_and(|c| c.names.iter().any(|n| n.eq_ignore_ascii_case(name))));

    let mut create = false;
    let mut cancel = false;
    let mut subscribe: Vec<u64> = Vec::new();
    let mut open_steam: Option<u64> = None;
    let modal = egui::Modal::new(egui::Id::new("import_profile")).show(ctx, |ui| {
        ui.set_width(600.0);
        ui.label(theme::title("Import profile"));
        ui.add(
            egui::Label::new(theme::faint(format!(
                "From {}. Nothing is created until you press Create profile.",
                imp.source.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default()
            )))
            .truncate(),
        );
        ui.add_space(2.0);

        theme::overline(ui, "Name");
        ui.add(
            egui::TextEdit::singleline(&mut imp.name)
                .font(FontId::new(16.0, theme::semibold()))
                .desired_width(f32::INFINITY)
                .margin(Margin::symmetric(8, 5))
                .hint_text("Profile name"),
        );
        let typed = imp.name.trim();
        let final_name = profile_io::unique_name(if typed.is_empty() { "Imported profile" } else { typed }, &names);
        if final_name != typed {
            ui.label(theme::faint(format!("A profile with that name exists already; this one will be called \"{final_name}\".")));
        }

        // ---- what else is in the file
        let f = &imp.file;
        let mut settings: Vec<String> = Vec::new();
        if f.options.skip_intro {
            settings.push("skip intro".into());
        }
        if f.options.frame_overlay {
            settings.push("frame overlay".into());
        }
        if let Some(n) = f.options.worker_threads {
            settings.push(format!("{n} worker threads"));
        }
        if let Some(cap) = f.options.frame_cap {
            settings.push(match cap {
                0 => "no frame cap".to_owned(),
                c if c < 0 => "frame cap at the display's rate".to_owned(),
                c => format!("{c} fps cap"),
            });
        }
        if f.separate_saves {
            settings.push("its own save folder".into());
        }
        if f.checkpoint_before_launch {
            settings.push(format!("checkpoint before launch (keep {})", f.checkpoint_keep));
        }
        if f.checkpoint_every_minutes > 0 {
            settings.push(format!("checkpoint every {} min", f.checkpoint_every_minutes));
        }
        if !f.objectives.is_empty() {
            settings.push(plural(f.objectives.len(), "objective", "objectives"));
        }
        if !f.notes.trim().is_empty() {
            settings.push("notes".into());
        }
        if !settings.is_empty() {
            ui.add(egui::Label::new(theme::muted(format!("Also sets: {}.", settings.join(", ")))).wrap());
        }
        if let Some(preset) = &f.video_preset {
            let text = if preset_missing {
                format!("Graphics preset \"{preset}\": there is no preset of that name on this PC, so the game's own video options are used until you make one or pick another.")
            } else {
                format!("Graphics preset \"{preset}\": uses your preset of that name. A profile file carries the preset's name, not its settings.")
            };
            ui.add(egui::Label::new(theme::faint(text)).wrap());
        }
        if !f.options.extra_args.trim().is_empty() {
            ui.label(theme::faint("Extra arguments passed to the game:"));
            theme::inset_frame().show(ui, |ui| {
                ui.set_width(ui.available_width());
                ui.add(egui::Label::new(theme::mono(f.options.extra_args.clone())).wrap());
            });
        }

        // ---- mods
        ui.add_space(4.0);
        theme::overline(ui, "Mods, in load order");
        if resolved.is_empty() {
            ui.label(theme::muted("None: this profile runs unmodded Forces of Corruption."));
        }
        egui::ScrollArea::vertical().id_salt("import_mods").max_height(260.0).show(ui, |ui| {
            ui.spacing_mut().item_spacing.y = 4.0;
            for (i, r) in resolved.iter().enumerate() {
                egui::Frame::new()
                    .fill(theme::FIELD)
                    .stroke(Stroke::new(1.0_f32, theme::BORDER))
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::symmetric(8, 4))
                    .show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        ui.horizontal(|ui| {
                            ui.add_sized([20.0, 20.0], egui::Label::new(theme::muted((i + 1).to_string())).selectable(false));
                            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                match &r.status {
                                    ImportStatus::Installed(path) => {
                                        theme::chip(ui, "Installed", theme::GOOD).on_hover_text(path.display().to_string());
                                    }
                                    ImportStatus::MissingLocal => {
                                        theme::chip(ui, "Missing local mod", theme::BAD).on_hover_text(
                                            "This mod didn't come from the Workshop, so the launcher can't fetch it. It is left out of the profile.",
                                        );
                                    }
                                    ImportStatus::MissingWorkshop { id, .. } => {
                                        let s = state_of(*id);
                                        if s.busy {
                                            theme::chip(ui, "Subscribing…", theme::ACCENT_HI);
                                        } else if let Some(progress) = &s.progress {
                                            theme::chip(ui, progress.clone(), theme::ACCENT_HI);
                                        } else if s.subscribed {
                                            theme::chip(ui, "Subscribed, waiting for Steam", theme::ACCENT_HI);
                                        } else if connected {
                                            if theme::button(ui, "Subscribe").clicked() {
                                                subscribe.push(*id);
                                            }
                                        } else if theme::button(ui, "Open in Steam").clicked() {
                                            open_steam = Some(*id);
                                        }
                                        theme::chip(ui, "Missing Workshop mod", theme::WARN)
                                            .on_hover_text(format!("Workshop item {id} isn't installed on this PC."));
                                    }
                                }
                                ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                                    ui.add(egui::Label::new(theme::text(r.entry.name().to_owned())).truncate());
                                });
                            });
                        });
                    });
            }
        });

        // ---- what happens with the missing ones
        if !missing_ws.is_empty() {
            let text = if workshop_dir.is_none() {
                "Steam's Workshop folder wasn't found on this PC, so missing Workshop mods can't be given a place in the profile. Add them on this page once they are installed."
            } else {
                "Missing Workshop mods keep their place in the load order. They show as not downloaded, and the profile can't be launched, until Steam has finished downloading them. No need to keep this window open."
            };
            ui.add(egui::Label::new(theme::faint(text)).wrap());
            if !connected {
                ui.add(
                    egui::Label::new(theme::faint(
                        "Connect to Steam on the Mods page to subscribe to them from here. Until then, Open in Steam shows the item's page.",
                    ))
                    .wrap(),
                );
            }
        }
        if missing_local > 0 {
            ui.add(
                egui::Label::new(theme::faint(format!(
                    "{} from outside the Workshop can't be fetched and {} left out. Install {} and add {} to the profile afterwards.",
                    plural(missing_local, "mod", "mods"),
                    if missing_local == 1 { "is" } else { "are" },
                    if missing_local == 1 { "it" } else { "them" },
                    if missing_local == 1 { "it" } else { "them" },
                )))
                .wrap(),
            );
        }

        ui.add_space(8.0);
        ui.horizontal(|ui| {
            if connected && !to_subscribe.is_empty() {
                let label = format!("Subscribe to all missing ({})", to_subscribe.len());
                if theme::button(ui, &label).clicked() {
                    subscribe.extend(to_subscribe.iter().copied());
                }
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if theme::primary_button(ui, "Create profile").clicked() {
                    create = true;
                }
                if theme::button(ui, "Cancel").clicked() {
                    cancel = true;
                }
            });
        });
    });

    if let Some(id) = open_steam {
        if let Err(e) = workshop::open_item_page(id) {
            app.err(e);
        }
    }
    for id in subscribe {
        app.mods_ui.workshop_mut().subscribe(id);
    }
    if create {
        let Some(imp) = st.import.take() else { return };
        let typed = imp.name.trim();
        let name = profile_io::unique_name(if typed.is_empty() { "Imported profile" } else { typed }, &names);
        let id = app.cfg.add_profile(&name);
        let built = profile_io::build_profile(id, &name, &imp.file, &resolved);
        let placed = built.mods.len();
        if let Some(slot) = app.cfg.profiles.iter_mut().find(|p| p.id == id) {
            *slot = built;
        }
        app.cfg.active_profile = id;
        st.drag = None;
        st.new_objective.clear();
        app.touch();
        let left_out = resolved.len() - placed;
        let waiting = missing_ws.len().min(placed);
        let mut text = format!("Imported \"{name}\" with {}", plural(placed, "mod", "mods"));
        if workshop_dir.is_some() && waiting > 0 {
            text.push_str(&format!(", {waiting} still to be downloaded"));
        }
        if left_out > 0 {
            text.push_str(&format!(". {} left out", plural(left_out, "mod was", "mods were")));
        }
        app.ok(text);
    } else if cancel || modal.should_close() {
        st.import = None;
    }
}

// ------------------------------------------------------------------ right: editor

/// A card whose body can be folded away. The header shows `summary` while it is folded.
fn section(ui: &mut egui::Ui, key: &str, title: &str, summary: &str, default_open: bool, add: impl FnOnce(&mut egui::Ui)) {
    let id = egui::Id::new(("profile_section", key));
    let mut open = ui.data(|d| d.get_temp::<bool>(id)).unwrap_or(default_open);
    theme::card(ui, |ui| {
        let mut toggle = false;
        ui.horizontal(|ui| {
            let label = ui.add(egui::Label::new(theme::title(title)).selectable(false).sense(Sense::click()));
            toggle |= label.on_hover_cursor(CursorIcon::PointingHand).clicked();
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let (glyph, tip) = if open { (icon::UP, "Fold away") } else { (icon::DOWN, "Show") };
                toggle |= theme::icon_button(ui, glyph, tip).clicked();
                if !open && !summary.is_empty() {
                    ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                        ui.add(egui::Label::new(theme::faint(summary)).truncate().selectable(false));
                    });
                }
            });
        });
        if toggle {
            open = !open;
            ui.data_mut(|d| d.insert_temp(id, open));
        }
        if open {
            add(ui);
        }
    });
}

fn frame_cap_label(cap: Option<i32>) -> String {
    FRAME_CAPS
        .iter()
        .find(|(v, _)| *v == cap)
        .map(|(_, l)| (*l).to_owned())
        .unwrap_or_else(|| format!("{} fps", cap.unwrap_or(0)))
}

/// Preset names for the picker, and whether the one the profile names still exists.
fn presets<'a>(st: &'a mut State, current: Option<&str>, now: f64) -> &'a PresetCache {
    let stale = st
        .presets
        .as_ref()
        .is_none_or(|c| now - c.read_at > 2.0 || now < c.read_at || c.checked.as_deref() != current);
    if stale {
        st.presets = Some(PresetCache {
            read_at: now,
            names: video::list_presets(),
            checked: current.map(str::to_owned),
            missing: current.is_some_and(|name| !video::preset_path(name).is_file()),
        });
    }
    st.presets.as_ref().unwrap()
}

fn editor(app: &mut App, st: &mut State, ui: &mut egui::Ui, report: Option<&Report>) {
    let mut changed = false;
    let mut error: Option<String> = None;
    let mut done: Option<String> = None;

    // ---- Name
    theme::card(ui, |ui| {
        theme::overline(ui, "Name");
        let p = app.cfg.active_mut();
        changed |= ui
            .add(
                egui::TextEdit::singleline(&mut p.name)
                    .font(FontId::new(20.0, theme::semibold()))
                    .desired_width(f32::INFINITY)
                    .margin(Margin::symmetric(8, 6))
                    .hint_text("Profile name"),
            )
            .changed();
    });
    ui.add_space(6.0);

    // ---- Mods & load order
    changed |= mods_card(app, st, ui, report);
    ui.add_space(6.0);

    // ---- Launch options
    let now = ui.input(|i| i.time);
    let current_preset = app.cfg.active().video_preset.clone().filter(|p| !p.trim().is_empty());
    let (preset_names, preset_missing) = {
        let cache = presets(st, current_preset.as_deref(), now);
        (cache.names.clone(), cache.missing)
    };
    let launch_summary = {
        let p = app.cfg.active();
        let o = &p.options;
        let mut parts: Vec<String> = Vec::new();
        if o.skip_intro {
            parts.push("Skip intro".into());
        }
        if o.frame_overlay {
            parts.push("Frame overlay".into());
        }
        if let Some(n) = o.worker_threads {
            parts.push(format!("{n} worker threads"));
        }
        if o.frame_cap.is_some() {
            parts.push(format!("Frame cap: {}", frame_cap_label(o.frame_cap).to_lowercase()));
        }
        if let Some(preset) = &current_preset {
            parts.push(format!("Graphics preset: {preset}"));
        }
        if !o.extra_args.trim().is_empty() {
            parts.push("Extra arguments".into());
        }
        if parts.is_empty() {
            "The game's defaults".to_owned()
        } else {
            parts.join(" · ")
        }
    };
    section(ui, "launch", "Launch options", &launch_summary, true, |ui| {
        let p = app.cfg.active_mut();
        let o = &mut p.options;
        changed |= theme::toggle_row(ui, &mut o.skip_intro, "Skip intro", "Go straight to the main menu (--no-intro).");
        changed |= theme::toggle_row(
            ui,
            &mut o.frame_overlay,
            "Frame overlay",
            "Show frame timing on screen while playing (--frame-overlay).",
        );
        let mut auto = o.worker_threads.is_none();
        if theme::toggle_row(
            ui,
            &mut auto,
            "Automatic worker threads",
            "The game uses one fewer than the number of CPU threads.",
        ) {
            o.worker_threads = if auto {
                None
            } else {
                let cpus = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(4) as u32;
                Some(cpus.saturating_sub(1).min(64))
            };
            changed = true;
        }
        if let Some(n) = o.worker_threads.as_mut() {
            setting_row(ui, "Worker threads", "0 runs everything on one thread.", |ui| {
                changed |= ui.add(egui::Slider::new(n, 0..=64)).changed();
            });
        }
        setting_row(ui, "Frame cap", "Overrides the frame cap on the Graphics page.", |ui| {
            let current = o.frame_cap;
            egui::ComboBox::from_id_salt("profile_frame_cap")
                .width(210.0)
                .selected_text(frame_cap_label(current))
                .show_ui(ui, |ui| {
                    for (v, l) in FRAME_CAPS {
                        if ui.selectable_label(current == v, l).clicked() && current != v {
                            o.frame_cap = v;
                            changed = true;
                        }
                    }
                });
        });
        setting_row(ui, "Graphics preset", "Presets are made on the Graphics page.", |ui| {
            const DEFAULT: &str = "Game default (video_options.txt)";
            let selected = match &current_preset {
                None => DEFAULT.to_owned(),
                Some(name) if preset_missing => format!("{name} (missing)"),
                Some(name) => name.clone(),
            };
            let mut pick: Option<Option<String>> = None;
            egui::ComboBox::from_id_salt("profile_video_preset").width(210.0).selected_text(selected).show_ui(ui, |ui| {
                if ui.selectable_label(current_preset.is_none(), DEFAULT).clicked() && current_preset.is_some() {
                    pick = Some(None);
                }
                for name in &preset_names {
                    let on = !preset_missing && current_preset.as_ref().is_some_and(|c| c.eq_ignore_ascii_case(name));
                    if ui.selectable_label(on, name).clicked() && !on {
                        pick = Some(Some(name.clone()));
                    }
                }
                if let (true, Some(name)) = (preset_missing, &current_preset) {
                    let _ = ui.selectable_label(true, RichText::new(format!("{name} (missing)")).color(theme::WARN));
                }
            });
            if let Some(choice) = pick {
                p.video_preset = choice;
                changed = true;
            }
            if preset_missing {
                theme::chip(ui, "Missing", theme::WARN).on_hover_text(
                    "This preset no longer exists. The game starts with its own video_options.txt until you pick another.",
                );
            }
        });
        if preset_names.is_empty() && current_preset.is_none() {
            ui.label(theme::faint("No presets yet. A preset is a saved set of graphics settings; make one on the Graphics page."));
        }
        ui.add_space(2.0);
        ui.label(theme::text("Extra arguments"));
        changed |= ui
            .add(
                egui::TextEdit::singleline(&mut o.extra_args)
                    .font(egui::TextStyle::Monospace)
                    .desired_width(f32::INFINITY)
                    .hint_text("Added to the end of the command line"),
            )
            .changed();
        for issue in launch::extra_arg_issues(&o.extra_args) {
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::WARNING).color(theme::WARN).size(12.0));
                ui.add(egui::Label::new(RichText::new(issue.text()).color(theme::WARN).size(12.0)).wrap());
            });
        }
        ui.label(theme::faint(
            "A flag the game doesn't know makes it exit immediately with code 2. KEY=value extras become part of what a save remembers, so changing them later stops older saves from loading.",
        ));
    });
    ui.add_space(6.0);

    // ---- Saves
    let saves_summary = {
        let p = app.cfg.active();
        let mut parts = vec![if p.separate_saves { "Own save folder" } else { "Shared save folder" }.to_owned()];
        if p.checkpoint_before_launch {
            parts.push(format!("Checkpoint before launch (keep {})", p.checkpoint_keep));
        }
        if p.checkpoint_every_minutes > 0 {
            parts.push(format!("Checkpoint every {} min", p.checkpoint_every_minutes));
        }
        parts.join(" · ")
    };
    section(ui, "saves", "Saves", &saves_summary, true, |ui| {
        let p = app.cfg.active_mut();
        changed |= theme::toggle_row(
            ui,
            &mut p.separate_saves,
            "Separate save folder",
            "Keep this profile's saves in a folder of their own.",
        );
        ui.label(theme::faint(
            "A save only loads with the mod list it was made with, so a folder per profile keeps saves from different mod sets apart. Existing saves are not moved when you switch this.",
        ));
        match launch::saves_dir(p) {
            Some(dir) => {
                ui.horizontal(|ui| {
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if theme::button(ui, "Open").clicked() {
                            match std::fs::create_dir_all(&dir) {
                                Ok(()) => open_in_explorer(&dir),
                                Err(e) => error = Some(format!("Couldn't create {}: {e}", dir.display())),
                            }
                        }
                        ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                            ui.add(egui::Label::new(theme::mono(dir.display().to_string())).truncate());
                        });
                    });
                });
            }
            None => {
                ui.label(theme::faint("The save folder couldn't be worked out on this system."));
            }
        }

        ui.add_space(2.0);
        theme::separator(ui);
        ui.add_space(2.0);
        changed |= theme::toggle_row(
            ui,
            &mut p.checkpoint_before_launch,
            "Checkpoint saves before launching",
            "Copy this profile's saves to a checkpoint every time the game is started.",
        );
        setting_row(ui, "Also checkpoint while playing", "Takes another checkpoint at this interval while the game runs.", |ui| {
            let current = p.checkpoint_every_minutes;
            let label = |m: u32| if m == 0 { "Off".to_owned() } else { format!("Every {m} minutes") };
            egui::ComboBox::from_id_salt("profile_checkpoint_every").width(210.0).selected_text(label(current)).show_ui(
                ui,
                |ui| {
                    for m in profile_io::CHECKPOINT_MINUTES {
                        if ui.selectable_label(current == m, label(m)).clicked() && current != m {
                            p.checkpoint_every_minutes = m;
                            changed = true;
                        }
                    }
                    // A value from an edited config that isn't one of the choices stays visible.
                    if !profile_io::CHECKPOINT_MINUTES.contains(&current) {
                        let _ = ui.selectable_label(true, label(current));
                    }
                },
            );
        });
        if p.checkpoint_before_launch || p.checkpoint_every_minutes > 0 {
            setting_row(ui, "Keep the last", "Older automatic checkpoints are removed when a new one is taken.", |ui| {
                ui.label(theme::muted("automatic checkpoints"));
                p.checkpoint_keep = p.checkpoint_keep.clamp(1, 50);
                changed |= ui.add(egui::DragValue::new(&mut p.checkpoint_keep).range(1..=50).speed(0.1)).changed();
            });
        }
        ui.label(theme::faint(
            "A save only loads with the mod list it was made with, so a checkpoint before each launch protects a campaign from a bad mod change or a crash. Automatic checkpoints are pruned; the ones you make yourself never are.",
        ));
    });
    ui.add_space(6.0);

    // ---- Campaign objectives
    theme::card(ui, |ui| {
        let p = app.cfg.active_mut();
        let total = p.objectives.len();
        let done = p.objectives.iter().filter(|o| o.done).count();
        ui.horizontal(|ui| {
            ui.label(theme::title("Campaign objectives"));
            if total > 0 {
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.label(theme::faint(format!("{done} of {total} done")));
                    ui.add(
                        egui::ProgressBar::new(done as f32 / total as f32)
                            .desired_width(120.0)
                            .desired_height(6.0)
                            .fill(theme::GOOD),
                    );
                });
            }
        });
        if total == 0 {
            theme::empty_state(
                ui,
                icon::FLAG,
                "No objectives yet",
                "Add goals for this campaign and tick them off as you go.",
            );
        }
        let mut remove = None;
        for (i, o) in p.objectives.iter_mut().enumerate() {
            ui.horizontal(|ui| {
                changed |= ui.checkbox(&mut o.done, "").changed();
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if theme::icon_button(ui, icon::DELETE, "Delete objective").clicked() {
                        remove = Some(i);
                    }
                    ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                        let color = if o.done { theme::FAINT } else { theme::TEXT };
                        changed |= ui
                            .add(
                                egui::TextEdit::singleline(&mut o.text)
                                    .desired_width(f32::INFINITY)
                                    .frame(false)
                                    .text_color(color)
                                    .hint_text("Objective"),
                            )
                            .changed();
                    });
                });
            });
        }
        if let Some(i) = remove {
            p.objectives.remove(i);
            changed = true;
        }
        ui.add_space(2.0);
        ui.horizontal(|ui| {
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let ready = !st.new_objective.trim().is_empty();
                let clicked = ui.add_enabled_ui(ready, |ui| theme::button(ui, "Add")).inner.clicked();
                ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                    let edit = ui.add(
                        egui::TextEdit::singleline(&mut st.new_objective)
                            .desired_width(f32::INFINITY)
                            .hint_text("New objective"),
                    );
                    let enter = edit.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
                    if (clicked || enter) && ready {
                        p.objectives.push(Objective { text: st.new_objective.trim().to_owned(), done: false });
                        st.new_objective.clear();
                        changed = true;
                        edit.request_focus();
                    }
                });
            });
        });
    });
    ui.add_space(6.0);

    // ---- Notes
    theme::card(ui, |ui| {
        ui.label(theme::title("Notes"));
        let p = app.cfg.active_mut();
        changed |= ui
            .add(
                egui::TextEdit::multiline(&mut p.notes)
                    .desired_rows(6)
                    .desired_width(f32::INFINITY)
                    .hint_text("Anything worth remembering about this setup"),
            )
            .changed();
    });
    ui.add_space(6.0);

    // ---- Shortcuts and sharing
    let name = app.cfg.active().name.clone();
    let same_name = app.cfg.profiles.iter().filter(|p| p.name.trim().eq_ignore_ascii_case(name.trim())).count() > 1;
    let mut place: Option<shortcut::Place> = None;
    let mut export = false;
    let mut import = false;
    section(ui, "sharing", "Shortcuts and sharing", "Desktop and Start menu shortcuts, export to a file", false, |ui| {
        theme::overline(ui, "Shortcuts");
        ui.add_enabled_ui(!name.trim().is_empty(), |ui| {
            ui.horizontal(|ui| {
                if theme::button(ui, "Create desktop shortcut").clicked() {
                    place = Some(shortcut::Place::Desktop);
                }
                if theme::button(ui, "Add to Start menu").clicked() {
                    place = Some(shortcut::Place::StartMenu);
                }
            });
        });
        ui.add(
            egui::Label::new(theme::faint(format!(
                "The shortcut starts this profile directly: it runs the launcher with --profile \"{}\" --launch. Make it again after renaming the profile.",
                name.trim()
            )))
            .wrap(),
        );
        if name.trim().is_empty() {
            ui.label(RichText::new("Give the profile a name first.").color(theme::WARN).size(12.0));
        } else if same_name {
            ui.label(
                RichText::new("Another profile has the same name, so a shortcut can't tell them apart. Rename one of them.")
                    .color(theme::WARN)
                    .size(12.0),
            );
        }

        ui.add_space(2.0);
        theme::separator(ui);
        ui.add_space(2.0);
        theme::overline(ui, "Share");
        ui.horizontal(|ui| {
            export = theme::button(ui, "Export profile").clicked();
            import = theme::button(ui, "Import profile").clicked();
        });
        ui.add(
            egui::Label::new(theme::faint(
                "An exported profile is a small .ccprofile file: the name, launch options, objectives, notes, checkpoint settings and the mod list. Workshop mods are listed by their Workshop id and other mods by folder name, never by path. Play time is left out. The graphics preset is saved by name only, not its settings.",
            ))
            .wrap(),
        );
    });

    if changed {
        app.touch();
    }
    if let Some(place) = place {
        match shortcut::create(name.trim(), place) {
            Ok(path) => done = Some(format!("Shortcut created: {}", path.display())),
            Err(e) => error = Some(e),
        }
    }
    if export {
        export_active(app);
    }
    if import {
        start_import(app, st);
    }
    if let Some(text) = done {
        app.ok(text);
    }
    if let Some(e) = error {
        app.err(e);
    }
}

// ------------------------------------------------------------------ mods & load order

struct Row {
    path: PathBuf,
    name: String,
    icon: Option<PathBuf>,
    source: Option<&'static str>,
    missing: bool,
    /// A Workshop item Steam hasn't put on disk yet: its id and what to say about it.
    pending: Option<(u64, String)>,
    can_subscribe: bool,
    /// Faint lines under the row: what this mod overrides and what overrides it.
    lines: Vec<String>,
    /// Conflict warnings that involve this mod.
    warning: Option<String>,
}

enum Act {
    /// Take the row at the first index out and put it back at the second.
    Move(usize, usize),
    Remove(usize),
    Add(PathBuf),
    Subscribe(u64),
    OpenSteam(u64),
    Recheck,
    Copied(usize),
}

/// The id of the Workshop item a folder in Steam's Workshop content folder belongs to.
pub(super) fn workshop_item_of(path: &Path, workshop_dir: Option<&Path>) -> Option<u64> {
    let parent = path.parent()?;
    if !same_path(parent, workshop_dir?) {
        return None;
    }
    let folder = path.file_name()?.to_string_lossy();
    if folder.is_empty() || !folder.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    folder.parse().ok().filter(|id| *id > 0)
}

fn conflict_lines(report: &Report, i: usize) -> Vec<String> {
    let mut lines: Vec<String> = Vec::new();
    let overlaps: Vec<&PairOverlap> = report.overrides_of(i).collect();
    for p in overlaps.iter().take(3) {
        lines.push(format!("Overrides {} from {}", overlap_text(p), report.name(p.lower)));
    }
    if overlaps.len() > 3 {
        lines.push(format!("Overrides files from {} more below", plural(overlaps.len() - 3, "mod", "mods")));
    }
    if let Some(summary) = report.per_mod.get(i) {
        if summary.exists && !summary.has_data {
            lines.push("No Data folder: this mod changes nothing in the game".to_owned());
        }
        if summary.replaced_files > 0 {
            let all = summary.replaced_files == summary.files;
            lines.push(format!(
                "{}{} replaced by mods above",
                if all { "All " } else { "" },
                plural(summary.replaced_files, "file", "files")
            ));
        }
    }
    lines
}

fn mods_card(app: &mut App, st: &mut State, ui: &mut egui::Ui, report: Option<&Report>) -> bool {
    let workshop_dir = app.steam.workshop_dir.clone();
    let ws = app.mods_ui.workshop();
    let connected = ws.is_connected();
    let rows: Vec<Row> = app
        .cfg
        .active()
        .mods
        .iter()
        .enumerate()
        .map(|(i, path)| {
            let info = app.find_mod(path);
            let missing = !path.is_dir();
            let mut name = app.mod_name(path);
            let mut pending = None;
            let mut can_subscribe = false;
            let item = if missing { workshop_item_of(path, workshop_dir.as_deref()) } else { None };
            if let Some(id) = item {
                // Not on disk, so the scan has no name for it: use Steam's, if it is known.
                if !app.cfg.mod_names.contains_key(&crate::config::mod_key(path)) {
                    name = ws.title(id);
                }
                let status = ws.item_status(id);
                let busy = ws.is_busy(id);
                let subscribed = ws.is_subscribed(id);
                let text = if busy {
                    "Subscribing…".to_owned()
                } else if status.downloading {
                    match status.percent() {
                        Some(p) => format!("Downloading {p}%"),
                        None => "Download queued".to_owned(),
                    }
                } else if subscribed {
                    "Waiting for Steam".to_owned()
                } else {
                    "Not downloaded".to_owned()
                };
                pending = Some((id, text));
                can_subscribe = connected && !busy && !subscribed;
            }
            let warning = report
                .map(|r| r.warnings_for(i).map(|w| w.text.clone()).collect::<Vec<_>>().join("\n\n"))
                .filter(|t| !t.is_empty());
            Row {
                name,
                icon: info.and_then(|m| m.icon.clone()),
                source: info.map(|m| m.source.label()),
                missing,
                pending,
                can_subscribe,
                lines: report.map(|r| conflict_lines(r, i)).unwrap_or_default(),
                warning,
                path: path.clone(),
            }
        })
        .collect();
    let candidates: Vec<(PathBuf, String)> = app
        .mods
        .iter()
        .filter(|m| !rows.iter().any(|r| same_path(&r.path, &m.path)))
        .map(|m| (m.path.clone(), app.mod_name(&m.path)))
        .collect();
    let installed = app.mods.len();
    let n = rows.len();
    let checking = report.is_none();
    let check_error = st.conflicts.error.clone();
    let mut act: Option<Act> = None;
    let tex = &mut app.tex;

    theme::card(ui, |ui| {
        ui.horizontal(|ui| {
            ui.label(theme::title("Mods & load order"));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if checking {
                    ui.label(theme::faint("Checking for conflicts…"));
                    ui.add(egui::Spinner::new().size(14.0));
                } else if n > 0 && theme::icon_button(ui, icon::REFRESH, "Look at the mod folders again and re-check for conflicts").clicked() {
                    act = Some(Act::Recheck);
                }
            });
        });
        ui.label(theme::faint(
            "Passed to the game as MODPATH= in this order. The first mod wins when two mods change the same file — list a submod above its parent.",
        ));
        ui.add_space(2.0);

        if rows.is_empty() {
            theme::empty_state(ui, icon::PUZZLE, "No mods", "This profile runs unmodded Forces of Corruption.");
        }

        let mut rects: Vec<Rect> = Vec::with_capacity(n);
        ui.scope(|ui| {
            ui.spacing_mut().item_spacing.y = 4.0;
            for (i, row) in rows.iter().enumerate() {
                let dragging = st.drag == Some(i);
                let frame = egui::Frame::new()
                    .fill(if dragging { theme::CARD_HI } else { theme::FIELD })
                    .stroke(Stroke::new(1.0_f32, if dragging { theme::ACCENT } else { theme::BORDER }))
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::symmetric(8, 5))
                    .show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        ui.spacing_mut().item_spacing.y = 1.0;
                        let mut text_left = 0.0;
                        ui.horizontal(|ui| {
                            // Grip: the drag handle.
                            let (grip, resp) = ui.allocate_exact_size(vec2(18.0, 28.0), Sense::drag());
                            let hot = resp.hovered() || dragging;
                            ui.painter().text(
                                grip.center(),
                                Align2::CENTER_CENTER,
                                icon::GRIP,
                                FontId::new(14.0, FontFamily::Proportional),
                                if hot { theme::TEXT } else { theme::FAINT },
                            );
                            if resp.drag_started() {
                                st.drag = Some(i);
                            }
                            resp.on_hover_cursor(CursorIcon::Grab);

                            ui.add_sized(
                                [20.0, 20.0],
                                egui::Label::new(theme::muted((i + 1).to_string())).selectable(false),
                            );

                            let (ir, _) = ui.allocate_exact_size(vec2(26.0, 26.0), Sense::hover());
                            if ui.is_rect_visible(ir) {
                                let t = row.icon.as_ref().and_then(|p| tex.get(ui.ctx(), p));
                                paint_icon(ui, t, ir);
                            }
                            text_left = ir.right() + ui.spacing().item_spacing.x - ui.min_rect().left();

                            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                                ui.spacing_mut().item_spacing.x = 2.0;
                                if theme::icon_button(ui, icon::CLOSE, "Remove from profile").clicked() {
                                    act = Some(Act::Remove(i));
                                }
                                if ui
                                    .add_enabled_ui(i + 1 < n, |ui| theme::icon_button(ui, icon::DOWN, "Move down"))
                                    .inner
                                    .clicked()
                                {
                                    act = Some(Act::Move(i, i + 1));
                                }
                                if ui
                                    .add_enabled_ui(i > 0, |ui| theme::icon_button(ui, icon::UP, "Move up"))
                                    .inner
                                    .clicked()
                                {
                                    act = Some(Act::Move(i, i - 1));
                                }
                                ui.add_space(6.0);
                                if let Some((id, text)) = &row.pending {
                                    if theme::icon_button(ui, icon::GLOBE, "Open this Workshop item in Steam").clicked() {
                                        act = Some(Act::OpenSteam(*id));
                                    }
                                    if row.can_subscribe && theme::button(ui, "Subscribe").clicked() {
                                        act = Some(Act::Subscribe(*id));
                                    }
                                    ui.add_space(4.0);
                                    theme::chip(ui, text.clone(), theme::WARN).on_hover_text(format!(
                                        "Steam hasn't put Workshop item {id} on this PC yet. The profile can't be launched until it has.{}",
                                        if connected { "" } else { "\nConnect to Steam on the Mods page to subscribe from here." }
                                    ));
                                    ui.add_space(4.0);
                                } else if row.missing {
                                    theme::chip(ui, "Missing", theme::BAD)
                                        .on_hover_text(format!("This folder no longer exists:\n{}", row.path.display()));
                                    ui.add_space(4.0);
                                }
                                if let Some(warning) = &row.warning {
                                    theme::chip(ui, format!("{} Warning", icon::WARNING), theme::WARN).on_hover_text(warning.clone());
                                    ui.add_space(4.0);
                                }
                                if let Some(source) = row.source {
                                    theme::chip(ui, source, theme::MUTED);
                                    ui.add_space(4.0);
                                }
                                ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                                    ui.add(egui::Label::new(theme::text(row.name.clone())).truncate())
                                        .on_hover_text(row.path.display().to_string());
                                });
                            });
                        });
                        for line in &row.lines {
                            ui.horizontal(|ui| {
                                ui.add_space(text_left);
                                ui.add(egui::Label::new(theme::faint(line.clone())).truncate().selectable(false));
                            });
                        }
                    });
                rects.push(frame.response.rect);
            }
        });

        // Drag in progress: show where the row would land, and move it on release.
        if let Some(from) = st.drag {
            if from >= n {
                st.drag = None;
            } else {
                ui.ctx().set_cursor_icon(CursorIcon::Grabbing);
                let (pos, down) = ui.input(|i| (i.pointer.interact_pos(), i.pointer.primary_down()));
                match pos {
                    Some(pos) => {
                        let slot = rects.iter().filter(|r| r.center().y < pos.y).count();
                        let to = if slot > from { slot - 1 } else { slot };
                        if down {
                            if to != from {
                                let y = if slot == 0 {
                                    rects[0].top() - 2.0
                                } else if slot == n {
                                    rects[n - 1].bottom() + 2.0
                                } else {
                                    (rects[slot - 1].bottom() + rects[slot].top()) / 2.0
                                };
                                ui.painter().hline(rects[0].x_range(), y, Stroke::new(2.0_f32, theme::ACCENT));
                            }
                        } else {
                            if to != from {
                                act = Some(Act::Move(from, to));
                            }
                            st.drag = None;
                        }
                    }
                    None => {
                        if !down {
                            st.drag = None;
                        }
                    }
                }
            }
        }

        ui.add_space(4.0);
        ui.horizontal(|ui| {
            ui.add_enabled_ui(!candidates.is_empty(), |ui| {
                egui::ComboBox::from_id_salt("profile_add_mod")
                    .width(240.0)
                    .selected_text(format!("{}  Add mod", icon::ADD))
                    .show_ui(ui, |ui| {
                        for (path, name) in &candidates {
                            if ui.selectable_label(false, name).clicked() {
                                act = Some(Act::Add(path.clone()));
                            }
                        }
                    });
            });
            if installed == 0 {
                ui.label(theme::faint("No mods were found. See the Mods page."));
            } else if candidates.is_empty() {
                ui.label(theme::faint("Every installed mod is already in this profile."));
            }
        });

        if let Some(e) = &check_error {
            ui.label(RichText::new(e.clone()).color(theme::WARN).size(12.0));
        }
        if let Some(report) = report {
            conflicts_section(ui, report, n, &mut act);
        }
    });

    let mods = &mut app.cfg.active_mut().mods;
    match act {
        Some(Act::Move(from, to)) if from < mods.len() && to < mods.len() && from != to => {
            let m = mods.remove(from);
            mods.insert(to, m);
            true
        }
        Some(Act::Remove(i)) if i < mods.len() => {
            mods.remove(i);
            true
        }
        Some(Act::Add(path)) if !mods.iter().any(|p| same_path(p, &path)) => {
            mods.push(path);
            true
        }
        Some(Act::Subscribe(id)) => {
            app.mods_ui.workshop_mut().subscribe(id);
            false
        }
        Some(Act::OpenSteam(id)) => {
            if let Err(e) = workshop::open_item_page(id) {
                app.err(e);
            }
            false
        }
        Some(Act::Recheck) => {
            st.conflicts.refresh();
            false
        }
        Some(Act::Copied(count)) => {
            app.ok(format!("Copied {}", plural(count, "path", "paths")));
            false
        }
        _ => false,
    }
}

/// The foldable details under the mod list: every warning in full, and each pair of mods
/// that share files with the list of those files.
fn conflicts_section(ui: &mut egui::Ui, report: &Report, mods: usize, act: &mut Option<Act>) {
    let warnings = report.warnings.iter().filter(|w| w.severity == ConflictSeverity::Warning).count();
    if report.pairs.is_empty() && report.warnings.is_empty() {
        if mods >= 2 {
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::CHECK).color(theme::GOOD).size(12.0));
                ui.label(theme::faint("No conflicts: these mods don't have any loose file in common."));
            });
        }
        return;
    }
    ui.add_space(2.0);
    let mut title = format!("Conflicts  ·  {}", plural(report.pairs.len(), "overlap", "overlaps"));
    if warnings > 0 {
        title.push_str(&format!(", {}", plural(warnings, "warning", "warnings")));
    }
    let heading = RichText::new(title).color(if warnings > 0 { theme::WARN } else { theme::MUTED });
    egui::CollapsingHeader::new(heading).id_salt("profile_conflicts").default_open(false).show(ui, |ui| {
        ui.add(
            egui::Label::new(theme::faint(
                "Loose files under each mod's Data folder are compared; files inside a mod's .meg archives are not. The mod higher in the list wins every file two mods both have.",
            ))
            .wrap(),
        );
        for w in &report.warnings {
            ui.horizontal(|ui| {
                let (glyph, color) = match w.severity {
                    ConflictSeverity::Warning => (icon::WARNING, theme::WARN),
                    ConflictSeverity::Info => (icon::INFO, theme::FAINT),
                };
                ui.label(RichText::new(glyph).color(color));
                let text = match w.severity {
                    ConflictSeverity::Warning => theme::text(w.text.clone()),
                    ConflictSeverity::Info => theme::muted(w.text.clone()),
                };
                ui.add(egui::Label::new(text).wrap());
            });
        }
        for (k, p) in report.pairs.iter().enumerate() {
            let title = format!("{}  over  {}  ·  {}", report.name(p.upper), report.name(p.lower), overlap_text(p));
            egui::CollapsingHeader::new(theme::text(title)).id_salt(("profile_conflict_pair", k)).default_open(false).show(
                ui,
                |ui| {
                    ui.horizontal(|ui| {
                        ui.label(theme::faint(format!(
                            "{} that both have, relative to Data. {} is the one the game uses.",
                            plural(p.shared_files.len(), "file", "files"),
                            report.name(p.upper)
                        )));
                        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                            if theme::button(ui, "Copy list").clicked() {
                                ui.ctx().copy_text(p.shared_files.join("\r\n"));
                                *act = Some(Act::Copied(p.shared_files.len()));
                            }
                        });
                    });
                    theme::inset_frame().show(ui, |ui| {
                        ui.set_width(ui.available_width());
                        let row_h = ui.text_style_height(&egui::TextStyle::Monospace);
                        egui::ScrollArea::vertical()
                            .id_salt(("profile_conflict_files", k))
                            .max_height(row_h * 12.0 + 24.0)
                            .auto_shrink([false, true])
                            .show_rows(ui, row_h, p.shared_files.len(), |ui, range| {
                                for path in &p.shared_files[range] {
                                    ui.add(egui::Label::new(theme::mono(path.clone())).truncate());
                                }
                            });
                    });
                },
            );
        }
    });
}

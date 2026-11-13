//! Graphics: edits the port's `video_options.txt`, and the named presets (complete option
//! files of their own) that a profile can be launched with instead.

use crate::app::App;
use crate::launch;
use crate::theme::{self, icon};
use crate::video::{self, Starter, VideoOptions};
use egui::{vec2, Align, Layout, RichText, Sense, Ui};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant, SystemTime};

const DEFAULT_LABEL: &str = "Game default (video_options.txt)";
/// How often the page looks at the disk again: the preset list, and whether the file
/// being edited was rewritten (the game does that when its own video menu is used).
const DISK_RECHECK: Duration = Duration::from_secs(2);

enum Dialog {
    /// Name for a new preset made from the values on screen.
    SaveAs(String),
    /// New name for the preset being edited.
    Rename(String),
    /// Confirm deleting the preset being edited.
    Delete,
    /// Switching to this file (`None` = the game's own) would lose unsaved changes.
    Leave(Option<String>),
}

/// Things the page's controls ask for; carried out once the frame's UI is done.
enum Do {
    Switch(Option<String>),
    AskSaveAs,
    AskRename,
    AskDelete,
    Duplicate,
    /// Take every value from this file (`None` = the game's own).
    CopyFrom(Option<String>),
    /// Use the preset being edited for the active profile, or stop using it.
    Assign(bool),
    Starter(Starter),
    /// Profiles that name this preset, which no longer exists, go back to the game's own file.
    Forget(String),
    Save,
    Revert,
}

#[derive(Default)]
pub struct State {
    loaded: bool,
    /// The file being edited: `None` = the game's own video_options.txt, else a preset.
    target: Option<String>,
    presets: Vec<String>,
    opts: Option<VideoOptions>,
    pristine: Option<VideoOptions>,
    custom_res: bool,
    dialog: Option<Dialog>,
    dialog_focus: bool,
    /// Modification time of the file when it was loaded or last saved here.
    disk_stamp: Option<SystemTime>,
    /// The file was rewritten by something else while it has unsaved changes here.
    changed_on_disk: bool,
    disk_checked: Option<Instant>,
}

const RESOLUTIONS: [(i32, i32); 6] =
    [(1280, 720), (1600, 900), (1920, 1080), (2560, 1440), (3440, 1440), (3840, 2160)];

const FRAME_CAPS: [(i32, &str); 9] = [
    (0, "No cap"),
    (-1, "Display refresh rate"),
    (30, "30 fps"),
    (60, "60 fps"),
    (90, "90 fps"),
    (120, "120 fps"),
    (144, "144 fps"),
    (165, "165 fps"),
    (240, "240 fps"),
];

const AA_LEVELS: [(i32, &str); 4] = [(0, "Off"), (30, "Low"), (63, "Medium"), (100, "High")];
const ANISO_LEVELS: [(i32, &str); 5] = [(0, "Off"), (2, "2×"), (4, "4×"), (8, "8×"), (16, "16×")];

/// Title and description on the left, a control laid out from the right edge.
pub(super) fn setting_row(ui: &mut Ui, title: &str, desc: &str, add: impl FnOnce(&mut Ui)) {
    ui.horizontal(|ui| {
        ui.vertical(|ui| {
            ui.set_max_width((ui.available_width() - 270.0).max(120.0));
            ui.spacing_mut().item_spacing.y = 1.0;
            ui.label(theme::text(title));
            if !desc.is_empty() {
                ui.label(theme::faint(desc));
            }
        });
        ui.with_layout(Layout::right_to_left(Align::Center), add);
    });
}

fn combo_i32(ui: &mut Ui, id: &str, current: i32, options: &[(i32, &str)], unit: &str) -> Option<i32> {
    let label = options
        .iter()
        .find(|(v, _)| *v == current)
        .map(|(_, l)| (*l).to_owned())
        .unwrap_or_else(|| format!("{current}{unit}"));
    let mut out = None;
    egui::ComboBox::from_id_salt(id).width(210.0).selected_text(label).show_ui(ui, |ui| {
        for (v, l) in options {
            if ui.selectable_label(*v == current, *l).clicked() && *v != current {
                out = Some(*v);
            }
        }
    });
    out
}

fn flag_row(ui: &mut Ui, opts: &mut VideoOptions, key: &str, title: &str, desc: &str) {
    let mut on = opts.flag(key);
    if theme::toggle_row(ui, &mut on, title, desc) {
        opts.set_flag(key, on);
    }
}

fn nearest(options: &[(i32, &str)], v: i32) -> i32 {
    options.iter().map(|(l, _)| *l).min_by_key(|l| (*l - v).abs()).unwrap_or(v)
}

fn level_name(levels: &[i32], v: i32) -> &'static str {
    let names: &[&str] = if levels.len() >= 4 { &["Low", "Medium", "High", "Ultra"] } else { &["Low", "Medium", "High"] };
    let snapped = video::snap(levels, v);
    let idx = levels.iter().position(|l| *l == snapped).unwrap_or(0);
    names.get(idx).copied().unwrap_or("")
}

fn modified(path: &Path) -> Option<SystemTime> {
    std::fs::metadata(path).and_then(|m| m.modified()).ok()
}

/// The file behind a target: a preset's, or the game's own for `None`.
fn path_of(target: Option<&str>) -> Option<PathBuf> {
    match target {
        Some(name) => Some(video::preset_path(name)),
        None => launch::video_options_path(),
    }
}

fn label_of(target: Option<&str>) -> String {
    match target {
        Some(name) => format!("the preset \"{name}\""),
        None => "video_options.txt".to_owned(),
    }
}

fn is_custom(opts: &VideoOptions) -> bool {
    let wh = (opts.int("width"), opts.int("height"));
    wh != (0, 0) && !RESOLUTIONS.contains(&wh)
}

/// (Re)reads the file being edited, dropping anything unsaved.
fn load(st: &mut State) {
    st.loaded = true;
    st.presets = video::list_presets();
    // A preset deleted or renamed outside the launcher: fall back to the game's own file.
    st.target = st.target.take().and_then(|t| st.presets.iter().find(|p| video::same_preset(p, &t)).cloned());
    st.opts = path_of(st.target.as_deref()).map(|p| VideoOptions::load(&p));
    st.pristine = st.opts.clone();
    st.custom_res = st.opts.as_ref().is_some_and(is_custom);
    st.disk_stamp = st.opts.as_ref().and_then(|o| modified(&o.path));
    st.changed_on_disk = false;
    st.disk_checked = Some(Instant::now());
}

/// Every couple of seconds: refreshes the preset list, and notices when the file being
/// edited was rewritten by the game.
fn watch_disk(st: &mut State) {
    if st.disk_checked.is_some_and(|t| t.elapsed() < DISK_RECHECK) {
        return;
    }
    st.disk_checked = Some(Instant::now());
    let presets = video::list_presets();
    if presets != st.presets {
        st.presets = presets;
    }
    let Some(path) = st.opts.as_ref().map(|o| o.path.clone()) else { return };
    if modified(&path) == st.disk_stamp {
        return;
    }
    if st.pristine == st.opts {
        // Nothing unsaved here: simply show what is on disk now.
        load(st);
    } else {
        st.changed_on_disk = true;
    }
}

pub fn show(app: &mut App, ui: &mut egui::Ui) {
    let mut st = std::mem::take(&mut app.graphics_ui);
    body(app, &mut st, ui);
    app.graphics_ui = st;
}

/// The "Editing" card: which file the page edits, the preset actions, and who uses it.
fn target_card(app: &App, st: &State, ui: &mut Ui) -> Option<Do> {
    let mut todo = None;
    let target = st.target.as_deref();
    let is = |name: &str| target.is_some_and(|t| video::same_preset(t, name));
    theme::card(ui, |ui| {
        ui.horizontal(|ui| {
            ui.label(theme::muted("Editing"));
            egui::ComboBox::from_id_salt("gfx_target")
                .width(280.0)
                .selected_text(target.unwrap_or(DEFAULT_LABEL))
                .show_ui(ui, |ui| {
                    if ui.selectable_label(target.is_none(), DEFAULT_LABEL).clicked() && target.is_some() {
                        todo = Some(Do::Switch(None));
                    }
                    for name in &st.presets {
                        if ui.selectable_label(is(name), name.as_str()).clicked() && !is(name) {
                            todo = Some(Do::Switch(Some(name.clone())));
                        }
                    }
                });
            ui.add_enabled_ui(st.opts.is_some(), |ui| {
                if theme::button(ui, "Save as new preset…")
                    .on_hover_text("Create a preset from the values on screen.")
                    .clicked()
                {
                    todo = Some(Do::AskSaveAs);
                }
                // Every other file: the game's own, and each preset but the one on screen.
                let mut sources: Vec<Option<&str>> = Vec::new();
                if target.is_some() {
                    sources.push(None);
                }
                sources.extend(st.presets.iter().map(String::as_str).filter(|p| !is(p)).map(Some));
                ui.add_enabled_ui(!sources.is_empty(), |ui| {
                    ui.menu_button("Copy from…", |ui| {
                        for source in &sources {
                            if ui.button(source.unwrap_or(DEFAULT_LABEL)).clicked() {
                                todo = Some(Do::CopyFrom(source.map(str::to_owned)));
                                ui.close_menu();
                            }
                        }
                    })
                    .response
                    .on_hover_text("Replace the values on screen with another file's. Nothing is written until you save.");
                });
            });
            if target.is_some() {
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.spacing_mut().item_spacing.x = 2.0;
                    if theme::icon_button(ui, icon::DELETE, "Delete this preset").clicked() {
                        todo = Some(Do::AskDelete);
                    }
                    if theme::icon_button(ui, icon::RENAME, "Rename this preset").clicked() {
                        todo = Some(Do::AskRename);
                    }
                    if theme::icon_button(ui, icon::COPY, "Duplicate this preset").clicked() {
                        todo = Some(Do::Duplicate);
                    }
                });
            }
        });

        if let Some(opts) = &st.opts {
            ui.add(
                egui::Label::new(RichText::new(opts.path.display().to_string()).monospace().size(12.0).color(theme::FAINT))
                    .truncate(),
            );
            if !opts.existed {
                ui.label(theme::faint(
                    "This file doesn't exist yet. It will be created with the game's defaults when you save.",
                ));
            }
        }

        let active = app.cfg.active();
        match target {
            Some(name) => {
                let users = video::profiles_using(&app.cfg.profiles, name);
                let in_use = active.video_preset.as_deref().is_some_and(|v| video::same_preset(v, name));
                ui.horizontal_wrapped(|ui| {
                    if in_use {
                        if theme::button(ui, &format!("Stop using for {}", active.name))
                            .on_hover_text("The active profile goes back to the game's own video_options.txt.")
                            .clicked()
                        {
                            todo = Some(Do::Assign(false));
                        }
                    } else if theme::button(ui, &format!("Use for {}", active.name))
                        .on_hover_text("Launch the active profile with this preset.")
                        .clicked()
                    {
                        todo = Some(Do::Assign(true));
                    }
                    ui.label(theme::faint(if users.is_empty() {
                        "No profile uses this preset.".to_owned()
                    } else {
                        format!("Used by: {}", users.join(", "))
                    }));
                });
                ui.label(theme::faint(
                    "A profile that uses a preset starts the game with that file instead of video_options.txt. The game reads and writes the file it is started with, so settings changed in its own video menu are saved to the preset.",
                ));
            }
            None => {
                let theirs =
                    active.video_preset.as_deref().and_then(|v| st.presets.iter().find(|p| video::same_preset(p, v)));
                match theirs {
                    Some(preset) => {
                        ui.horizontal_wrapped(|ui| {
                            ui.label(theme::faint(format!(
                                "The active profile, {}, is launched with the preset \"{preset}\", not with this file.",
                                active.name
                            )));
                            if theme::button(ui, &format!("Edit {preset}")).clicked() {
                                todo = Some(Do::Switch(Some(preset.clone())));
                            }
                        });
                    }
                    None => {
                        ui.label(theme::faint("Profiles without a preset of their own start the game with this file."));
                    }
                }
                if st.presets.is_empty() && st.opts.is_some() {
                    ui.horizontal_wrapped(|ui| {
                        ui.label(theme::faint("No presets yet. Start with one made from the values on screen:"));
                        for starter in Starter::ALL {
                            if theme::button(ui, starter.name()).on_hover_text(starter.summary()).clicked() {
                                todo = Some(Do::Starter(starter));
                            }
                        }
                    });
                }
            }
        }

        // Profiles pointing at a preset that is gone are launched with the game's own file.
        let mut missing: Vec<&str> = Vec::new();
        for v in app.cfg.profiles.iter().filter_map(|p| p.video_preset.as_deref()).filter(|v| !v.trim().is_empty()) {
            let exists = st.presets.iter().any(|p| video::same_preset(p, v));
            if !exists && !missing.iter().any(|m| video::same_preset(m, v)) {
                missing.push(v);
            }
        }
        for preset in missing {
            let users = video::profiles_using(&app.cfg.profiles, preset).join(", ");
            ui.horizontal_wrapped(|ui| {
                ui.label(RichText::new(icon::WARNING).color(theme::WARN));
                ui.label(
                    RichText::new(format!(
                        "The preset \"{preset}\" no longer exists, so the game starts with video_options.txt for: {users}."
                    ))
                    .color(theme::WARN)
                    .size(12.0),
                );
                if theme::button(ui, "Stop using it").clicked() {
                    todo = Some(Do::Forget(preset.to_owned()));
                }
            });
        }
    });
    todo
}

fn body(app: &mut App, st: &mut State, ui: &mut egui::Ui) {
    theme::page_header(
        ui,
        "Graphics",
        "Edits the game's video options, and presets that a profile can be launched with instead.",
    );
    if !st.loaded {
        load(st);
    } else {
        watch_disk(st);
    }
    let mut todo = target_card(app, st, ui);
    ui.add_space(6.0);
    settings(app, st, ui, &mut todo);
    if let Some(todo) = todo {
        perform(app, st, todo);
    }
    dialogs(app, st, ui.ctx());
}

/// Writes the values on screen to their file. True when that worked.
fn save(app: &mut App, st: &mut State) -> bool {
    let Some(opts) = st.opts.as_mut() else { return false };
    opts.sync_detail();
    match opts.save() {
        Ok(()) => {
            st.pristine = Some(opts.clone());
            st.disk_stamp = modified(&opts.path);
            st.changed_on_disk = false;
            app.ok(match &st.target {
                Some(name) => format!("Preset \"{name}\" saved"),
                None => "Graphics settings saved".to_owned(),
            });
            true
        }
        Err(e) => {
            app.err(e);
            false
        }
    }
}

fn open(st: &mut State, dialog: Dialog) {
    st.dialog = Some(dialog);
    st.dialog_focus = true;
}

fn perform(app: &mut App, st: &mut State, todo: Do) {
    let dirty = st.pristine != st.opts;
    match todo {
        Do::Switch(target) => {
            if dirty {
                open(st, Dialog::Leave(target));
            } else {
                st.target = target;
                load(st);
            }
        }
        Do::AskSaveAs => open(st, Dialog::SaveAs(String::new())),
        Do::AskRename => {
            if let Some(name) = st.target.clone() {
                open(st, Dialog::Rename(name));
            }
        }
        Do::AskDelete => {
            if st.target.is_some() {
                open(st, Dialog::Delete);
            }
        }
        Do::Duplicate => {
            let Some(name) = st.target.clone() else { return };
            match video::duplicate_preset(&name) {
                Ok(copy) => {
                    st.presets = video::list_presets();
                    app.ok(if dirty {
                        format!("Created \"{copy}\" from the saved preset. The unsaved changes are not in it.")
                    } else {
                        format!("Created \"{copy}\"")
                    });
                }
                Err(e) => app.err(e),
            }
        }
        Do::CopyFrom(source) => {
            let Some(path) = path_of(source.as_deref()) else {
                app.err("Windows didn't report a local app data folder, so video_options.txt can't be located.");
                return;
            };
            let from = VideoOptions::load(&path);
            if !from.existed {
                app.err(format!("There is nothing to copy: {} doesn't exist yet.", path.display()));
                return;
            }
            if let Some(opts) = st.opts.as_mut() {
                opts.copy_values_from(&from);
                st.custom_res = is_custom(opts);
                app.ok(format!("Copied the values of {}. Save to keep them.", label_of(source.as_deref())));
            }
        }
        Do::Assign(on) => {
            let Some(name) = st.target.clone() else { return };
            app.cfg.active_mut().video_preset = on.then_some(name);
            app.touch();
        }
        Do::Starter(starter) => {
            let Some(opts) = &st.opts else { return };
            match video::create_preset(starter.name(), &starter.options(opts)) {
                Ok((name, _)) => {
                    st.presets = video::list_presets();
                    app.ok(format!("Preset \"{name}\" created"));
                }
                Err(e) => app.err(e),
            }
        }
        Do::Forget(preset) => {
            if video::retarget_profiles(&mut app.cfg.profiles, &preset, None) > 0 {
                app.touch();
            }
        }
        Do::Save => {
            save(app, st);
        }
        Do::Revert => load(st),
    }
}

#[derive(PartialEq)]
enum Outcome {
    Open,
    Confirm,
    Cancel,
}

/// A dialog asking for a preset name. `checked` is what the name check says about `text`.
#[allow(clippy::too_many_arguments)]
fn name_modal(
    ctx: &egui::Context,
    id: &str,
    title: &str,
    body: &str,
    action: &str,
    text: &mut String,
    checked: &Result<String, String>,
    focus: bool,
) -> Outcome {
    let mut out = Outcome::Open;
    let modal = egui::Modal::new(egui::Id::new(id)).show(ctx, |ui| {
        ui.set_width(440.0);
        ui.label(theme::title(title));
        ui.label(theme::muted(body));
        ui.add_space(4.0);
        let edit = ui.add(
            egui::TextEdit::singleline(text)
                .desired_width(f32::INFINITY)
                .char_limit(video::PRESET_NAME_MAX)
                .hint_text("Preset name"),
        );
        if focus {
            edit.request_focus();
        }
        let enter = edit.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
        match checked {
            Err(problem) if !text.trim().is_empty() => ui.label(RichText::new(problem).size(12.0).color(theme::BAD)),
            _ => ui.label(theme::faint("Letters, numbers, spaces, - and _.")),
        };
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let clicked = ui.add_enabled_ui(checked.is_ok(), |ui| theme::primary_button(ui, action)).inner.clicked();
                if (clicked || enter) && checked.is_ok() {
                    out = Outcome::Confirm;
                }
                if theme::button(ui, "Cancel").clicked() {
                    out = Outcome::Cancel;
                }
            });
        });
    });
    if out == Outcome::Open && modal.should_close() {
        out = Outcome::Cancel;
    }
    out
}

fn dialogs(app: &mut App, st: &mut State, ctx: &egui::Context) {
    let Some(dialog) = st.dialog.take() else { return };
    let focus = std::mem::take(&mut st.dialog_focus);
    match dialog {
        Dialog::SaveAs(mut text) => {
            let checked = video::check_preset_name(&text, None);
            let body = format!(
                "The new preset gets the values on screen, including unsaved changes. Nothing is written to {}.",
                label_of(st.target.as_deref())
            );
            match name_modal(ctx, "gfx_save_as", "Save as new preset", &body, "Create", &mut text, &checked, focus) {
                Outcome::Open => st.dialog = Some(Dialog::SaveAs(text)),
                Outcome::Cancel => {}
                Outcome::Confirm => {
                    let Some(mut source) = st.opts.clone() else { return };
                    source.sync_detail();
                    match video::create_preset(&text, &source) {
                        Ok((name, saved)) => {
                            st.target = Some(name.clone());
                            st.presets = video::list_presets();
                            st.custom_res = is_custom(&saved);
                            st.disk_stamp = modified(&saved.path);
                            st.changed_on_disk = false;
                            st.pristine = Some(saved.clone());
                            st.opts = Some(saved);
                            app.ok(format!("Preset \"{name}\" created"));
                        }
                        Err(e) => {
                            app.err(e);
                            st.dialog = Some(Dialog::SaveAs(text));
                        }
                    }
                }
            }
        }
        Dialog::Rename(mut text) => {
            let Some(old) = st.target.clone() else { return };
            let checked = video::check_preset_name(&text, Some(&old));
            let body = "Profiles that use the preset follow it to its new name.";
            match name_modal(ctx, "gfx_rename", &format!("Rename \"{old}\""), body, "Rename", &mut text, &checked, focus) {
                Outcome::Open => st.dialog = Some(Dialog::Rename(text)),
                Outcome::Cancel => {}
                Outcome::Confirm => match video::rename_preset(&old, &text) {
                    Ok(new) => {
                        let path = video::preset_path(&new);
                        for opts in [st.opts.as_mut(), st.pristine.as_mut()].into_iter().flatten() {
                            opts.path = path.clone();
                        }
                        if video::retarget_profiles(&mut app.cfg.profiles, &old, Some(&new)) > 0 {
                            app.touch();
                        }
                        st.disk_stamp = modified(&path);
                        st.presets = video::list_presets();
                        if new != old {
                            app.ok(format!("Renamed to \"{new}\""));
                        }
                        st.target = Some(new);
                    }
                    Err(e) => {
                        app.err(e);
                        st.dialog = Some(Dialog::Rename(text));
                    }
                },
            }
        }
        Dialog::Delete => {
            let Some(name) = st.target.clone() else { return };
            let users = video::profiles_using(&app.cfg.profiles, &name);
            let mut confirm = false;
            let mut cancel = false;
            let modal = egui::Modal::new(egui::Id::new("gfx_delete")).show(ctx, |ui| {
                ui.set_width(440.0);
                ui.label(theme::title(format!("Delete the preset \"{name}\"?")));
                ui.label(theme::muted("Its file is moved to the Recycle Bin."));
                if !users.is_empty() {
                    ui.label(
                        RichText::new(format!(
                            "{}  Used by: {}. They will start the game with video_options.txt again.",
                            icon::WARNING,
                            users.join(", ")
                        ))
                        .color(theme::WARN),
                    );
                }
                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if theme::danger_button(ui, "Delete").clicked() {
                            confirm = true;
                        }
                        if theme::button(ui, "Cancel").clicked() {
                            cancel = true;
                        }
                    });
                });
            });
            if confirm {
                match video::delete_preset(&name) {
                    Ok(()) => {
                        if video::retarget_profiles(&mut app.cfg.profiles, &name, None) > 0 {
                            app.touch();
                        }
                        st.target = None;
                        load(st);
                        app.ok(format!("Preset \"{name}\" deleted"));
                    }
                    Err(e) => app.err(e),
                }
            } else if !cancel && !modal.should_close() {
                st.dialog = Some(Dialog::Delete);
            }
        }
        Dialog::Leave(target) => {
            let mut save_first = false;
            let mut discard = false;
            let mut cancel = false;
            let modal = egui::Modal::new(egui::Id::new("gfx_leave")).show(ctx, |ui| {
                ui.set_width(440.0);
                ui.label(theme::title("Unsaved changes"));
                ui.label(theme::muted(format!(
                    "The changes to {} haven't been saved. Switching to {} without saving loses them.",
                    label_of(st.target.as_deref()),
                    label_of(target.as_deref())
                )));
                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        if theme::primary_button(ui, "Save and switch").clicked() {
                            save_first = true;
                        }
                        if theme::danger_button(ui, "Discard changes").clicked() {
                            discard = true;
                        }
                        if theme::button(ui, "Cancel").clicked() {
                            cancel = true;
                        }
                    });
                });
            });
            if save_first {
                if save(app, st) {
                    st.target = target;
                    load(st);
                }
            } else if discard {
                st.target = target;
                load(st);
            } else if !cancel && !modal.should_close() {
                st.dialog = Some(Dialog::Leave(target));
            }
        }
    }
}

/// The save row and the cards of settings for the file being edited.
fn settings(app: &App, st: &mut State, ui: &mut egui::Ui, todo: &mut Option<Do>) {
    let Some(opts) = st.opts.as_mut() else {
        theme::empty_state(
            ui,
            icon::DISPLAY,
            "Settings folder not found",
            "Windows didn't report a local app data folder, so video_options.txt can't be located.",
        );
        return;
    };
    let dirty = st.pristine.as_ref() != Some(&*opts);

    // Action row: stays put above the scrolling cards.
    ui.horizontal(|ui| {
        if ui.add_enabled_ui(dirty || !opts.existed, |ui| theme::primary_button(ui, "Save changes")).inner.clicked() {
            *todo = Some(Do::Save);
        }
        if theme::button(ui, "Revert").on_hover_text("Reload the file from disk").clicked() {
            *todo = Some(Do::Revert);
        }
        ui.add_space(4.0);
        let (dot, _) = ui.allocate_exact_size(vec2(8.0, 8.0), Sense::hover());
        if dirty {
            ui.painter().circle_filled(dot.center(), 4.0, theme::WARN);
            ui.label(RichText::new("Unsaved changes").color(theme::WARN).size(12.0));
        } else {
            ui.painter().circle_filled(dot.center(), 4.0, theme::FAINT);
            ui.label(theme::faint("No unsaved changes"));
        }
    });
    if st.changed_on_disk {
        ui.horizontal(|ui| {
            ui.label(RichText::new(icon::WARNING).color(theme::WARN));
            ui.label(
                RichText::new(
                    "This file was changed on disk after it was loaded here (the game rewrites it when its own video menu is used). Saving replaces those changes; Revert shows them.",
                )
                .color(theme::WARN)
                .size(12.0),
            );
        });
    }
    if app.runner.is_running() {
        ui.horizontal(|ui| {
            ui.label(RichText::new(icon::WARNING).color(theme::WARN));
            ui.label(
                RichText::new(
                    "The game is running. It reads its options file only at startup, and rewrites it when you press Apply in its own video menu.",
                )
                .color(theme::WARN)
                .size(12.0),
            );
        });
    }
    ui.add_space(6.0);

    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
        // ---- Display
        theme::card(ui, |ui| {
            ui.label(theme::title("Display"));
            let wh = (opts.int("width"), opts.int("height"));
            let selected = if st.custom_res {
                "Custom".to_owned()
            } else if wh == (0, 0) {
                "Game default (1280×720 window)".to_owned()
            } else {
                format!("{}×{}", wh.0, wh.1)
            };
            setting_row(ui, "Resolution", "", |ui| {
                egui::ComboBox::from_id_salt("gfx_resolution").width(240.0).selected_text(selected).show_ui(ui, |ui| {
                    if ui.selectable_label(!st.custom_res && wh == (0, 0), "Game default (1280×720 window)").clicked() {
                        st.custom_res = false;
                        opts.set("width", 0);
                        opts.set("height", 0);
                    }
                    for (w, h) in RESOLUTIONS {
                        if ui.selectable_label(!st.custom_res && wh == (w, h), format!("{w}×{h}")).clicked() {
                            st.custom_res = false;
                            opts.set("width", w);
                            opts.set("height", h);
                        }
                    }
                    if ui.selectable_label(st.custom_res, "Custom").clicked() && !st.custom_res {
                        st.custom_res = true;
                        if wh.0 <= 0 || wh.1 <= 0 {
                            opts.set("width", 1280);
                            opts.set("height", 720);
                        }
                    }
                });
            });
            if st.custom_res {
                setting_row(ui, "Custom size", "Width × height in pixels.", |ui| {
                    let (mut w, mut h) = (opts.int("width"), opts.int("height"));
                    if ui.add(egui::DragValue::new(&mut h).range(480..=4320).speed(2.0)).changed() {
                        opts.set("height", h);
                    }
                    ui.label(theme::muted("×"));
                    if ui.add(egui::DragValue::new(&mut w).range(640..=7680).speed(2.0)).changed() {
                        opts.set("width", w);
                    }
                });
            }
            setting_row(ui, "Refresh rate", "0 leaves the display's refresh rate as it is.", |ui| {
                let mut hz = opts.int("refresh_hz");
                if ui.add(egui::DragValue::new(&mut hz).range(0..=500).suffix(" Hz")).changed() {
                    opts.set("refresh_hz", hz);
                }
            });
            ui.label(theme::faint(
                "The game always runs in a window. A size larger than the desktop work area becomes a borderless window covering the monitor.",
            ));
            ui.add_space(4.0);
            theme::separator(ui);
            ui.add_space(4.0);

            flag_row(ui, opts, "vsync", "V-sync", "Synchronise frames with the display to avoid tearing.");
            let vsync = opts.flag("vsync");
            ui.add_enabled_ui(vsync, |ui| {
                setting_row(ui, "V-sync mode", "Used only while v-sync is on.", |ui| {
                    let current = if opts.get("vsync_mode") == "mailbox" { "mailbox" } else { "fifo" };
                    let name = |m: &str| if m == "mailbox" { "Low latency (mailbox)" } else { "Standard (FIFO)" };
                    egui::ComboBox::from_id_salt("gfx_vsync_mode").width(210.0).selected_text(name(current)).show_ui(ui, |ui| {
                        for m in ["fifo", "mailbox"] {
                            if ui.selectable_label(current == m, name(m)).clicked() && current != m {
                                opts.set("vsync_mode", m);
                            }
                        }
                    });
                });
            });
            setting_row(ui, "Frame cap", "A profile's own frame cap overrides this.", |ui| {
                if let Some(v) = combo_i32(ui, "gfx_frame_cap", opts.int("frame_cap"), &FRAME_CAPS, " fps") {
                    opts.set("frame_cap", v);
                }
            });
            flag_row(ui, opts, "hardware_mouse", "Hardware mouse cursor", "Let Windows draw the cursor instead of the game.");
        });
        ui.add_space(6.0);

        // ---- Quality
        theme::card(ui, |ui| {
            ui.label(theme::title("Quality"));
            let preset = opts.current_preset();
            ui.horizontal(|ui| {
                // Not "Preset": that word now means a named options file.
                ui.label(theme::muted("Detail level"));
                for (name, level) in video::PRESETS {
                    if super::mods::filter_chip(ui, name, preset == Some(name)).clicked() {
                        opts.apply_preset(level);
                    }
                }
                if preset.is_none() {
                    theme::chip(ui, "Custom", theme::MUTED);
                }
            });
            ui.add_space(4.0);
            for (key, label, levels) in video::DETAIL_KEYS {
                if levels.len() <= 2 {
                    let mut on = opts.int(key) > 0;
                    if theme::toggle_row(ui, &mut on, label, "") {
                        opts.set(key, if on { 100 } else { 0 });
                        opts.sync_detail();
                    }
                    continue;
                }
                setting_row(ui, label, "", |ui| {
                    let current = opts.int(key);
                    ui.add_sized([62.0, 20.0], egui::Label::new(theme::muted(level_name(levels, current))));
                    let mut v = current.clamp(0, 100);
                    if ui.add(egui::Slider::new(&mut v, 0..=100).show_value(false)).changed() {
                        // The game only tells a few levels apart, so land on a real one.
                        let snapped = video::snap(levels, v);
                        if snapped != current {
                            opts.set(key, snapped);
                            opts.sync_detail();
                        }
                    }
                });
            }
            ui.add_space(2.0);
            theme::separator(ui);
            let sum: i32 = video::DETAIL_KEYS.iter().map(|(k, _, _)| opts.int(k)).sum();
            let mean = (sum as f32 / video::DETAIL_KEYS.len() as f32).round() as i32;
            theme::kv(ui, "Overall detail", theme::text(format!("{mean} / 100")));
        });
        ui.add_space(6.0);

        // ---- Image
        theme::card(ui, |ui| {
            ui.label(theme::title("Image"));
            setting_row(ui, "Anti-aliasing (TAA)", "Smooths jagged edges over several frames.", |ui| {
                let shown = nearest(&AA_LEVELS, opts.int("anti_alias"));
                if let Some(v) = combo_i32(ui, "gfx_aa", shown, &AA_LEVELS, "") {
                    opts.set("anti_alias", v);
                }
            });
            setting_row(ui, "Anisotropic filtering", "Keeps textures sharp at shallow angles.", |ui| {
                let shown = nearest(&ANISO_LEVELS, opts.int("anisotropy"));
                if let Some(v) = combo_i32(ui, "gfx_aniso", shown, &ANISO_LEVELS, "") {
                    opts.set("anisotropy", v);
                }
            });
            let gamma = opts.int("gamma");
            let effective = 2f32.powf((gamma - 50) as f32 / 50.0);
            setting_row(ui, "Gamma", &format!("Effective gamma {effective:.2}"), |ui| {
                if ui.add_enabled_ui(gamma != 50, |ui| theme::button(ui, "Reset")).inner.clicked() {
                    opts.set("gamma", 50);
                }
                let mut g = gamma.clamp(0, 100);
                if ui.add(egui::Slider::new(&mut g, 0..=100)).changed() {
                    opts.set("gamma", g);
                }
            });
        });
        ui.add_space(6.0);

        // ---- Effects
        theme::card(ui, |ui| {
            ui.label(theme::title("Effects"));
            flag_row(ui, opts, "soft_shadows", "Soft shadows", "Smooths the edges of shadows.");
            flag_row(ui, opts, "heat_distortion", "Heat distortion", "Shimmer from engines and explosions.");
            flag_row(ui, opts, "bloom", "Bloom", "Glow around bright lights.");
            flag_row(ui, opts, "ambient_occlusion", "Ambient occlusion", "Darkens creases and contact points.");
        });

        // ---- Unknown
        let unknown = opts.unknown_keys();
        if !unknown.is_empty() {
            ui.add_space(6.0);
            theme::card(ui, |ui| {
                ui.label(theme::title("Other settings in this file"));
                ui.label(theme::faint(
                    "Settings the launcher doesn't recognise, and comments, are kept exactly as they are.",
                ));
                theme::inset_frame().show(ui, |ui| {
                    ui.set_width(ui.available_width());
                    ui.spacing_mut().item_spacing.y = 2.0;
                    for line in &unknown {
                        ui.label(theme::mono(line.clone()));
                    }
                });
            });
        }
        ui.add_space(24.0);
    });
}

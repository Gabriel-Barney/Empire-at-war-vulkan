//! Settings: what was detected, where the game is, extra mod folders, launcher options.

use crate::app::{open_in_explorer, App};
use crate::steam::{self, same_path};
use crate::theme::{self, icon};
use crate::{config, launch, mods};
use egui::{vec2, Align, Layout, RichText, Ui};
use std::path::Path;

const LABEL_W: f32 = 200.0;

pub fn show(app: &mut App, ui: &mut Ui) {
    theme::page_header(ui, "Settings", "Where the game lives and how the launcher behaves.");
    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
        detection_card(app, ui);
        ui.add_space(6.0);
        game_card(app, ui);
        ui.add_space(6.0);
        extra_dirs_card(app, ui);
        ui.add_space(6.0);
        workshop_card(app, ui);
        ui.add_space(6.0);
        launcher_card(app, ui);
        ui.add_space(24.0);
    });
}

/// The optional Steam Web API key that lets the Workshop's Browse tab search in the launcher.
fn workshop_card(app: &mut App, ui: &mut Ui) {
    let show_id = egui::Id::new("settings_show_steam_key");
    let mut show = ui.data(|d| d.get_temp::<bool>(show_id).unwrap_or(false));
    theme::card(ui, |ui| {
        theme::overline(ui, "Steam Workshop");
        ui.label(theme::muted(
            "Your subscriptions, downloads and mod names work without any setup. Searching the Workshop inside the launcher needs your own Steam Web API key, because Steam answers searches only with one. Without a key, Browse opens the same search in the Steam client.",
        ));
        ui.horizontal(|ui| {
            fixed(ui, LABEL_W, |ui| {
                ui.label(theme::muted("Steam Web API key"));
            });
            let before = app.cfg.settings.steam_web_api_key.clone();
            ui.add(
                egui::TextEdit::singleline(&mut app.cfg.settings.steam_web_api_key)
                    .password(!show)
                    .desired_width(300.0)
                    .hint_text("Not set"),
            );
            if theme::icon_button(ui, icon::EYE, if show { "Hide the key" } else { "Show the key" }).clicked() {
                show = !show;
            }
            if app.cfg.settings.steam_web_api_key != before {
                let trimmed = app.cfg.settings.steam_web_api_key.trim().to_owned();
                app.cfg.settings.steam_web_api_key = trimmed;
                app.touch();
            }
            if theme::button(ui, "Get a key").on_hover_text("steamcommunity.com/dev/apikey, signed in to Steam").clicked() {
                if let Err(e) = open_web("https://steamcommunity.com/dev/apikey") {
                    app.err(e);
                }
            }
        });
        ui.label(theme::faint(
            "The key is stored in the launcher's settings file on this PC and sent only to Steam (api.steampowered.com).",
        ));
    });
    ui.data_mut(|d| d.insert_temp(show_id, show));
}

/// Opens a web page in the default browser.
fn open_web(url: &str) -> Result<(), String> {
    std::process::Command::new("explorer.exe")
        .arg(url)
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("Couldn't open {url}: {e}"))
}

/// A fixed-width cell, so rows line up like a table.
fn fixed(ui: &mut Ui, width: f32, add: impl FnOnce(&mut Ui)) {
    ui.allocate_ui_with_layout(vec2(width, ui.spacing().interact_size.y), Layout::left_to_right(Align::Center), |ui| {
        ui.set_min_width(width);
        add(ui);
    });
}

fn found_chip(ui: &mut Ui, found: bool) {
    if found {
        theme::chip(ui, "Found", theme::GOOD);
    } else {
        theme::chip(ui, "Missing", theme::BAD);
    }
}

fn path_label(ui: &mut Ui, path: &Path) {
    let text = path.display().to_string();
    ui.add(egui::Label::new(theme::mono(text.clone())).truncate()).on_hover_text(text);
}

fn detect_row(ui: &mut Ui, label: &str, path: Option<&Path>, found: bool) {
    ui.horizontal(|ui| {
        fixed(ui, LABEL_W, |ui| {
            ui.label(theme::muted(label));
        });
        fixed(ui, 72.0, |ui| found_chip(ui, found));
        match path {
            Some(p) => path_label(ui, p),
            None => {
                ui.label(theme::faint("Not found"));
            }
        }
    });
}

// ------------------------------------------------------------------ detection

fn detection_card(app: &mut App, ui: &mut Ui) {
    let mut redetect = false;
    theme::card(ui, |ui| {
        ui.horizontal(|ui| {
            theme::overline(ui, "Detection");
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if theme::button(ui, &format!("{}  Detect again", icon::REFRESH)).clicked() {
                    redetect = true;
                }
            });
        });
        let s = &app.steam;
        detect_row(ui, "Steam folder", s.steam_dir.as_deref(), s.steam_dir.as_ref().is_some_and(|p| p.is_dir()));
        if s.libraries.is_empty() {
            detect_row(ui, "Steam library", None, false);
        }
        for (i, lib) in s.libraries.iter().enumerate() {
            let label = if s.libraries.len() == 1 { "Steam library".to_owned() } else { format!("Steam library {}", i + 1) };
            detect_row(ui, &label, Some(lib), lib.is_dir());
        }
        let foc = mods::foc_dir(s, &app.cfg.settings);
        let overridden = match (&foc, &app.cfg.settings.foc_dir_override) {
            (Some(f), Some(o)) => same_path(f, o),
            _ => false,
        };
        let foc_label = if overridden { "Forces of Corruption (your choice)" } else { "Forces of Corruption" };
        detect_row(ui, foc_label, foc.as_deref(), foc.as_ref().is_some_and(|p| p.is_dir()));
        // The base game's data sits in GameData, next to the corruption folder.
        let eaw = foc
            .as_ref()
            .and_then(|f| f.parent())
            .or(s.game_root.as_deref())
            .map(|root| root.join("GameData"));
        detect_row(ui, "Empire at War", eaw.as_deref(), eaw.as_ref().is_some_and(|p| p.is_dir()));
        detect_row(ui, "Workshop folder", s.workshop_dir.as_deref(), s.workshop_dir.as_ref().is_some_and(|p| p.is_dir()));
    });
    if redetect {
        app.steam = steam::detect();
        app.rescan_mods();
        if mods::foc_dir(&app.steam, &app.cfg.settings).is_some() {
            app.ok(format!("Detection finished. {} mods found.", app.mods.len()));
        } else {
            app.err("Forces of Corruption still wasn't found. Choose its folder under Game.");
        }
    }
}

// ------------------------------------------------------------------ game

fn game_card(app: &mut App, ui: &mut Ui) {
    theme::card(ui, |ui| {
        theme::overline(ui, "Game");

        // ---- alamo_game.exe
        let exe = app.cfg.settings.game_exe.clone();
        ui.horizontal(|ui| {
            ui.label(theme::strong("alamo_game.exe"));
            match &exe {
                Some(p) => found_chip(ui, p.is_file()),
                None => {
                    theme::chip(ui, "Not chosen", theme::BAD);
                }
            }
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let clear = ui.add_enabled_ui(exe.is_some(), |ui| theme::button(ui, "Clear")).inner;
                if clear.clicked() {
                    app.cfg.settings.game_exe = None;
                    app.touch();
                }
                if theme::button(ui, "Browse").clicked() {
                    let mut dialog =
                        rfd::FileDialog::new().set_title("Choose alamo_game.exe").add_filter("Programs", &["exe"]);
                    if let Some(dir) = exe.as_ref().and_then(|p| p.parent()).filter(|d| d.is_dir()) {
                        dialog = dialog.set_directory(dir);
                    }
                    if let Some(p) = dialog.pick_file() {
                        app.cfg.settings.game_exe = Some(p);
                        app.touch();
                    }
                }
            });
        });
        ui.add_space(-4.0);
        ui.label(theme::faint("The Vulkan port's program. The game is started from the folder it is in."));
        theme::inset_frame().show(ui, |ui| {
            ui.set_width(ui.available_width());
            match &exe {
                Some(p) => path_label(ui, p),
                None => {
                    ui.label(theme::faint("No file chosen"));
                }
            }
        });

        ui.add_space(6.0);
        theme::separator(ui);
        ui.add_space(6.0);

        // ---- Forces of Corruption override
        let over = app.cfg.settings.foc_dir_override.clone();
        let effective = mods::foc_dir(&app.steam, &app.cfg.settings);
        ui.horizontal(|ui| {
            ui.label(theme::strong("Forces of Corruption folder"));
            match (&over, &effective) {
                (Some(o), _) if !o.is_dir() => theme::chip(ui, "Your folder is missing", theme::BAD),
                (Some(_), _) => theme::chip(ui, "Your choice", theme::ACCENT_HI),
                (None, Some(_)) => theme::chip(ui, "Detected", theme::GOOD),
                (None, None) => theme::chip(ui, "Not found", theme::BAD),
            };
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let reset = ui.add_enabled_ui(over.is_some(), |ui| theme::button(ui, "Use detected")).inner;
                if reset.clicked() {
                    app.cfg.settings.foc_dir_override = None;
                    app.touch();
                    app.rescan_mods();
                }
                if theme::button(ui, "Browse").clicked() {
                    let mut dialog = rfd::FileDialog::new().set_title("Choose the Forces of Corruption folder");
                    if let Some(dir) = effective.as_ref().filter(|d| d.is_dir()) {
                        dialog = dialog.set_directory(dir);
                    }
                    if let Some(p) = dialog.pick_folder() {
                        if p.join("Data").is_dir() {
                            app.cfg.settings.foc_dir_override = Some(p);
                            app.touch();
                            app.rescan_mods();
                        } else {
                            app.err(format!(
                                "{} has no Data folder, so it isn't a Forces of Corruption folder.",
                                p.display()
                            ));
                        }
                    }
                }
            });
        });
        ui.add_space(-4.0);
        ui.label(theme::faint(
            "The folder that holds Data, usually named corruption. Only choose one if detection picks the wrong install.",
        ));
        theme::inset_frame().show(ui, |ui| {
            ui.set_width(ui.available_width());
            match &effective {
                Some(p) => path_label(ui, p),
                None => {
                    ui.label(theme::faint("Not found"));
                }
            }
        });
        if over.as_ref().is_some_and(|o| !o.is_dir()) {
            ui.label(RichText::new("The folder you chose no longer exists, so the detected one is used.").size(12.0).color(theme::WARN));
        }
    });
}

// ------------------------------------------------------------------ extra mod folders

fn extra_dirs_card(app: &mut App, ui: &mut Ui) {
    let mut remove: Option<usize> = None;
    let mut add = false;
    theme::card(ui, |ui| {
        ui.horizontal(|ui| {
            theme::overline(ui, "Extra mod folders");
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if theme::button(ui, &format!("{}  Add folder", icon::ADD)).clicked() {
                    add = true;
                }
            });
        });
        ui.label(theme::faint(
            "Looked in as well as the game's Mods folder and the Workshop. A folder can be one mod, or a folder full of mods.",
        ));
        if app.cfg.settings.extra_mod_dirs.is_empty() {
            theme::empty_state(ui, icon::FOLDER, "No extra folders", "Add one if you keep mods somewhere else.");
            return;
        }
        for (i, dir) in app.cfg.settings.extra_mod_dirs.iter().enumerate() {
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::FOLDER).color(theme::MUTED));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if theme::icon_button(ui, icon::DELETE, "Remove from the list. Nothing is deleted from disk.").clicked() {
                        remove = Some(i);
                    }
                    if theme::icon_button(ui, icon::FOLDER_OPEN, "Open the folder").clicked() {
                        open_in_explorer(dir);
                    }
                    if !dir.is_dir() {
                        theme::chip(ui, "Missing", theme::BAD);
                    }
                    ui.with_layout(Layout::left_to_right(Align::Center), |ui| path_label(ui, dir));
                });
            });
        }
    });
    if let Some(i) = remove {
        app.cfg.settings.extra_mod_dirs.remove(i);
        app.touch();
        app.rescan_mods();
    }
    if add {
        if let Some(p) = rfd::FileDialog::new().set_title("Choose a mod folder, or a folder of mods").pick_folder() {
            if app.cfg.settings.extra_mod_dirs.iter().any(|d| same_path(d, &p)) {
                app.err("That folder is already in the list.");
            } else {
                app.cfg.settings.extra_mod_dirs.push(p);
                app.touch();
                let before = app.mods.len();
                app.rescan_mods();
                match app.mods.len().saturating_sub(before) {
                    0 => app.ok("Folder added. No new mods were found in it."),
                    1 => app.ok("Folder added. 1 new mod found."),
                    n => app.ok(format!("Folder added. {n} new mods found.")),
                }
            }
        }
    }
}

// ------------------------------------------------------------------ launcher

fn info_row(ui: &mut Ui, label: &str, path: Option<&Path>, open: bool) {
    ui.horizontal(|ui| {
        fixed(ui, LABEL_W, |ui| {
            ui.label(theme::muted(label));
        });
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if let (true, Some(p)) = (open, path) {
                if theme::button(ui, "Open").clicked() {
                    open_in_explorer(p);
                }
            }
            ui.with_layout(Layout::left_to_right(Align::Center), |ui| match path {
                Some(p) => path_label(ui, p),
                None => {
                    ui.label(theme::faint("Unknown"));
                }
            });
        });
    });
}

fn launcher_card(app: &mut App, ui: &mut Ui) {
    let mut changed = false;
    theme::card(ui, |ui| {
        theme::overline(ui, "Launcher");
        changed = theme::toggle_row(
            ui,
            &mut app.cfg.settings.gpu_rendering,
            "Draw the launcher window with the GPU",
            "Off uses a software renderer so the launcher never touches the graphics card. Takes effect the next time the launcher starts.",
        );
        ui.add_space(4.0);
        theme::separator(ui);
        ui.add_space(4.0);
        info_row(ui, "Launcher data folder", Some(&config::data_dir()), true);
        info_row(ui, "Default save folder", launch::default_saves_dir().as_deref(), false);
        info_row(ui, "Video options file", launch::video_options_path().as_deref(), false);
        ui.label(theme::faint(
            "The last two belong to the game and are shown for reference. Profiles with their own saves keep them in the launcher data folder.",
        ));
    });
    if changed {
        app.touch();
    }
}

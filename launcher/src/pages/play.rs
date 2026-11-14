//! The landing page: what's about to be launched, and the button that launches it.

use super::profiles;
use crate::app::{clock, open_in_explorer, App, Page};
use crate::conflicts::Report;
use crate::diagnostics::{self, Finding, Fix, Severity};
use crate::saves::{self, SaveInfo};
use crate::theme::{self, icon};
use crate::{launch, mods, runner, video};
use egui::{
    pos2, vec2, Align, Align2, Color32, CornerRadius, FontFamily, FontId, Layout, Rect, RichText,
    Sense, Stroke, StrokeKind, Ui,
};
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::sync::{Arc, Mutex};
use std::time::{Duration, SystemTime};

const BANNER_H: f32 = 250.0;
const BANNER_RADIUS: u8 = 14;
const CARD_H: f32 = 204.0;

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

pub fn show(app: &mut App, ui: &mut Ui) {
    egui::ScrollArea::vertical().auto_shrink([false, false]).show(ui, |ui| {
        theme::page_header(ui, "Play", "Start the game with the active profile.");

        // Only these stop the Launch button. Everything else on this page is advice.
        let blockers = launch::blockers(&app.cfg, &app.steam);
        hero(app, ui, blockers.is_empty());

        // The conflict check runs in the background; its report is shared with Profiles.
        let report = profiles::conflict_report(app, ui.ctx());
        let mut run_diagnostics = false;

        if !blockers.is_empty() {
            ui.add_space(4.0);
            run_diagnostics |= problem_strip(app, ui, &blockers);
        }
        if let Some(report) = report.as_deref() {
            let order: Vec<&str> = report.order_warnings().iter().map(|w| w.text.as_str()).collect();
            if !order.is_empty() {
                ui.add_space(4.0);
                run_diagnostics |= order_strip(app, ui, &order);
            }
        }

        ui.add_space(6.0);
        ui.scope(|ui| {
            ui.spacing_mut().item_spacing.x = 14.0;
            ui.columns(3, |cols| {
                play_time_card(app, &mut cols[0]);
                saves_card(app, &mut cols[1]);
                options_card(app, &mut cols[2]);
            });
        });

        ui.add_space(6.0);
        run_diagnostics |= command_card(app, ui);
        ui.add_space(24.0);

        if run_diagnostics {
            start_diagnostics(app, ui.ctx(), report.as_deref());
        }
        diagnostics_modal(app, ui.ctx(), report.as_deref());
    });
}

// ------------------------------------------------------------------ hero banner

fn hero(app: &mut App, ui: &mut Ui, launchable: bool) {
    let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), BANNER_H), Sense::hover());
    if !ui.is_rect_visible(rect) {
        return;
    }
    let ctx = ui.ctx().clone();
    let radius = CornerRadius::same(BANNER_RADIUS);

    // Art: the first mod in load order whose picture can actually be shown.
    let splashes: Vec<PathBuf> = app
        .cfg
        .active()
        .mods
        .iter()
        .filter_map(|p| app.find_mod(p).and_then(|m| m.splash.clone()))
        .collect();
    let art = splashes.iter().find_map(|p| app.tex.get(&ctx, p));

    let painter = ui.painter().clone();
    painter.rect_filled(rect, radius, Color32::from_rgb(9, 13, 24));
    match &art {
        Some(tex) => {
            let size = tex.size_vec2();
            egui::Image::from_texture(egui::load::SizedTexture::new(tex.id(), size))
                .uv(cover_uv(size, rect.size()))
                .corner_radius(radius)
                .paint_at(ui, rect);
        }
        None => paint_fallback(&painter, rect),
    }

    // Bottom-up shade so the text stays readable on any picture.
    let strength = if art.is_some() { 1.0 } else { 0.6 };
    gradient_mesh(&painter, rect, 1, 10, |_, ty| {
        let a = (0.10 + 0.84 * ty.powf(1.7)) * strength;
        Color32::from_rgba_unmultiplied(6, 8, 13, (a * 255.0) as u8)
    });
    // The meshes are square: paint the page colour over what sticks out past the rounded corners.
    painter.rect_stroke(rect, radius, Stroke::new(7.0_f32, theme::BG), StrokeKind::Outside);
    painter.rect_stroke(rect, radius, Stroke::new(1.0_f32, Color32::from_white_alpha(20)), StrokeKind::Inside);

    let running = app.runner.is_running();
    let pad = 26.0;

    // ---- launch / stop button
    let btn = Rect::from_min_size(pos2(rect.right() - pad - 170.0, rect.bottom() - 24.0 - 48.0), vec2(170.0, 48.0));
    let enabled = running || launchable;
    let sense = if enabled { Sense::click() } else { Sense::hover() };
    let resp = ui.interact(btn, ui.id().with("launch_button"), sense);
    let hovered = enabled && resp.hovered();
    let pressed = enabled && resp.is_pointer_button_down_on();
    let (fill, fg) = if !enabled {
        (Color32::from_rgb(40, 47, 62), theme::FAINT)
    } else if running {
        let base = Color32::from_rgb(214, 70, 70);
        (if pressed { Color32::from_rgb(180, 54, 54) } else if hovered { theme::BAD } else { base }, Color32::WHITE)
    } else {
        (
            if pressed {
                Color32::from_rgb(64, 118, 224)
            } else if hovered {
                theme::lerp_color(theme::ACCENT, theme::ACCENT_HI, 0.55)
            } else {
                theme::ACCENT
            },
            Color32::WHITE,
        )
    };
    painter.rect_filled(btn.translate(vec2(0.0, 3.0)), CornerRadius::same(12), Color32::from_black_alpha(70));
    painter.rect_filled(btn, CornerRadius::same(12), fill);
    let (glyph, label) = if running { (icon::STOP, "Stop") } else { (icon::PLAY, "Launch") };
    let ig = painter.layout_no_wrap(glyph.to_owned(), FontId::new(16.0, FontFamily::Proportional), fg);
    let tg = painter.layout_no_wrap(label.to_owned(), FontId::new(17.0, theme::semibold()), fg);
    let total = ig.size().x + 10.0 + tg.size().x;
    let x = btn.center().x - total / 2.0;
    let icon_w = ig.size().x;
    painter.galley(pos2(x, btn.center().y - ig.size().y / 2.0 + 1.0), ig, fg);
    painter.galley(pos2(x + icon_w + 10.0, btn.center().y - tg.size().y / 2.0 - 1.0), tg, fg);

    let resp = if enabled {
        resp.on_hover_cursor(egui::CursorIcon::PointingHand)
    } else {
        resp.on_hover_text("Fix the problems listed below first.")
    };
    if resp.clicked() {
        if running {
            app.runner.stop();
        } else {
            app.launch(&ctx);
        }
    }

    // ---- elapsed clock next to the button while the game runs
    let mut reserved = 170.0 + pad;
    if running {
        let text_rect = painter.text(
            pos2(btn.left() - 18.0, btn.center().y),
            Align2::RIGHT_CENTER,
            format!("Running · {}", clock(app.runner.elapsed())),
            FontId::new(14.0, theme::semibold()),
            Color32::WHITE,
        );
        painter.circle_filled(pos2(text_rect.left() - 11.0, btn.center().y + 1.0), 4.0, theme::GOOD);
        reserved += text_rect.width() + 44.0;
    }

    // ---- profile name and mods, bottom-left
    let mods_line = if app.cfg.active().mods.is_empty() {
        "Unmodded Forces of Corruption".to_owned()
    } else {
        app.cfg.active().mods.iter().map(|p| app.mod_name(p)).collect::<Vec<_>>().join("  ›  ")
    };
    let left = rect.left() + pad;
    let right = (rect.right() - pad - reserved).max(left + 140.0).min(rect.right() - pad);
    let text_rect = Rect::from_min_max(pos2(left, rect.top() + pad), pos2(right, rect.bottom() - 22.0));
    let mut child = ui.new_child(
        egui::UiBuilder::new().id_salt("hero_text").max_rect(text_rect).layout(Layout::bottom_up(Align::Min)),
    );
    child.spacing_mut().item_spacing.y = 2.0;
    child.add(egui::Label::new(RichText::new(mods_line).size(14.0).color(Color32::from_rgb(208, 215, 228))).truncate());
    child.add(
        egui::Label::new(
            RichText::new(app.cfg.active().name.clone()).size(30.0).family(theme::semibold()).color(Color32::WHITE),
        )
        .truncate(),
    );
    child.label(
        RichText::new("ACTIVE PROFILE")
            .size(11.0)
            .family(theme::semibold())
            .extra_letter_spacing(1.2)
            .color(theme::ACCENT_HI),
    );
}

/// The part of a texture that fills `area` without distorting it.
fn cover_uv(tex: egui::Vec2, area: egui::Vec2) -> Rect {
    if tex.x <= 0.0 || tex.y <= 0.0 || area.x <= 0.0 || area.y <= 0.0 {
        return Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0));
    }
    let tex_aspect = tex.x / tex.y;
    let area_aspect = area.x / area.y;
    if tex_aspect > area_aspect {
        // Picture is wider than the banner: trim the sides.
        let f = area_aspect / tex_aspect;
        let x0 = (1.0 - f) / 2.0;
        Rect::from_min_max(pos2(x0, 0.0), pos2(x0 + f, 1.0))
    } else {
        // Picture is taller: trim top and bottom, keeping a little more of the top.
        let f = tex_aspect / area_aspect;
        let y0 = (1.0 - f) * 0.38;
        Rect::from_min_max(pos2(0.0, y0), pos2(1.0, y0 + f))
    }
}

/// A grid of coloured vertices; `color` gets the position as 0..1 fractions of the rect.
fn gradient_mesh(painter: &egui::Painter, rect: Rect, cols: usize, rows: usize, color: impl Fn(f32, f32) -> Color32) {
    let mut mesh = egui::Mesh::default();
    for j in 0..=rows {
        for i in 0..=cols {
            let (tx, ty) = (i as f32 / cols as f32, j as f32 / rows as f32);
            let p = pos2(rect.left() + rect.width() * tx, rect.top() + rect.height() * ty);
            mesh.colored_vertex(p, color(tx, ty));
        }
    }
    let w = cols as u32 + 1;
    for j in 0..rows as u32 {
        for i in 0..cols as u32 {
            let a = j * w + i;
            let (b, c, d) = (a + 1, a + w, a + w + 1);
            mesh.add_triangle(a, b, c);
            mesh.add_triangle(b, d, c);
        }
    }
    painter.add(egui::Shape::mesh(mesh));
}

/// Banner for profiles without art: deep blue with two soft glows and a fixed star field.
fn paint_fallback(painter: &egui::Painter, rect: Rect) {
    let aspect = rect.width() / rect.height().max(1.0);
    gradient_mesh(painter, rect, 20, 8, |tx, ty| {
        let glow = |cx: f32, cy: f32, r: f32| {
            let d = (((tx - cx) * aspect).powi(2) + (ty - cy).powi(2)).sqrt();
            (1.0 - d / r).clamp(0.0, 1.0).powi(2)
        };
        let t = (tx * 0.6 + (1.0 - ty) * 0.4).clamp(0.0, 1.0);
        let base = theme::lerp_color(Color32::from_rgb(8, 12, 26), Color32::from_rgb(17, 31, 68), t);
        let c = theme::lerp_color(base, Color32::from_rgb(46, 92, 190), glow(0.80, 0.22, 1.1) * 0.55);
        theme::lerp_color(c, Color32::from_rgb(78, 56, 150), glow(0.16, 0.95, 0.9) * 0.32)
    });
    let mut seed: u32 = 0x9E37_79B9;
    let mut next = || {
        seed = seed.wrapping_mul(1_664_525).wrapping_add(1_013_904_223);
        (seed >> 8) as f32 / (1u32 << 24) as f32
    };
    for _ in 0..80 {
        let (x, y, s, a) = (next(), next(), next(), next());
        let p = pos2(rect.left() + x * rect.width(), rect.top() + y * rect.height());
        painter.circle_filled(p, 0.5 + s * s * 1.1, Color32::from_white_alpha((28.0 + a * 150.0) as u8));
    }
}

// ------------------------------------------------------------------ problems

/// The amber strip under the banner. `buttons` draws its actions.
fn warning_strip(ui: &mut Ui, heading: &str, lines: &[&str], note: &str, buttons: impl FnOnce(&mut Ui)) {
    egui::Frame::new()
        .fill(Color32::from_rgb(44, 35, 16))
        .stroke(Stroke::new(1.0_f32, theme::WARN.gamma_multiply(0.45)))
        .corner_radius(CornerRadius::same(12))
        .inner_margin(egui::Margin::symmetric(16, 12))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::WARNING).color(theme::WARN).size(16.0));
                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 3.0;
                    ui.label(theme::strong(heading));
                    for line in lines {
                        ui.add(egui::Label::new(theme::text(*line)).wrap());
                    }
                    if !note.is_empty() {
                        ui.add(egui::Label::new(theme::faint(note)).wrap());
                    }
                    ui.add_space(5.0);
                    ui.horizontal(buttons);
                });
            });
        });
}

fn apply_fix(app: &mut App, fix: &Fix) {
    match fix {
        Fix::OpenSettings => app.page = Page::Settings,
        Fix::OpenProfiles => app.page = Page::Profiles,
        Fix::OpenMods => app.page = Page::Mods,
        Fix::OpenFolder(path) => open_in_explorer(path),
    }
}

/// What stops the game from launching. Returns true when "Run diagnostics" was pressed.
fn problem_strip(app: &mut App, ui: &mut Ui, blockers: &[Finding]) -> bool {
    let lines: Vec<&str> = blockers.iter().map(|f| f.detail.as_str()).collect();
    // One button per place a problem can be fixed.
    let mut fixes: Vec<&Fix> = Vec::new();
    for fix in blockers.iter().filter_map(|f| f.fix.as_ref()) {
        if !fixes.contains(&fix) {
            fixes.push(fix);
        }
    }
    let mut chosen: Option<Fix> = None;
    let mut run = false;
    warning_strip(ui, "The game can't be launched yet", &lines, "", |ui| {
        for fix in fixes {
            if theme::button(ui, fix.label()).clicked() {
                chosen = Some(fix.clone());
            }
        }
        run = theme::button(ui, "Run diagnostics").clicked();
    });
    if let Some(fix) = chosen {
        apply_fix(app, &fix);
    }
    run
}

/// Advice from the conflict check. It never disables Launch.
fn order_strip(app: &mut App, ui: &mut Ui, warnings: &[&str]) -> bool {
    let mut open = false;
    let mut run = false;
    warning_strip(
        ui,
        "The load order looks wrong",
        warnings,
        "The game still launches. The Profiles page shows which files each mod takes from the others.",
        |ui| {
            open = theme::button(ui, "Open profiles").clicked();
            run = theme::button(ui, "Run diagnostics").clicked();
        },
    );
    if open {
        app.page = Page::Profiles;
    }
    run
}

// ------------------------------------------------------------------ diagnostics

/// The diagnostics panel's state. The page has no state struct of its own, so this lives
/// in egui's temporary storage.
#[derive(Default)]
struct Diagnostics {
    open: bool,
    running: Option<Receiver<Vec<Finding>>>,
    results: Option<Vec<Finding>>,
    /// Name of the profile the run is for.
    profile: String,
    error: Option<String>,
}

fn diagnostics_state(ctx: &egui::Context) -> Arc<Mutex<Diagnostics>> {
    ctx.data_mut(|d| d.get_temp_mut_or_default::<Arc<Mutex<Diagnostics>>>(egui::Id::new("play_diagnostics")).clone())
}

/// Everything the checks need, gathered on the UI thread (no disk access worth the name).
fn diagnostics_input(app: &App, report: Option<&Report>) -> diagnostics::Input {
    let profile = app.cfg.active();
    let ws = app.mods_ui.workshop();
    let workshop_dir = app.steam.workshop_dir.as_deref();
    let mods = profile
        .mods
        .iter()
        .map(|path| {
            // Only folders inside Steam's Workshop content folder are Steam's to keep current.
            let id = profiles::workshop_item_of(path, workshop_dir);
            // Only while Steam is fetching it (its download folder exists): everything else
            // the diagnostics read from Steam's record themselves.
            let fetching = id.map(|id| ws.item_status(id)).filter(|s| s.downloading && s.bytes_done > 0);
            let steam = fetching.map(|s| {
                diagnostics::SteamItem {
                    installed: s.installed,
                    needs_update: s.needs_update,
                    downloading: s.downloading,
                    percent: s.percent(),
                }
            });
            diagnostics::ModEntry { name: app.mod_name(path), path: path.clone(), workshop_id: id, steam }
        })
        .collect();
    diagnostics::Input {
        game_exe: app.cfg.settings.game_exe.clone(),
        foc: mods::foc_dir(&app.steam, &app.cfg.settings),
        mods,
        workshop_acf: workshop_dir.and_then(diagnostics::workshop_acf_path),
        saves_dir: app.saves_dir(),
        video_preset: profile
            .video_preset
            .as_ref()
            .filter(|p| !p.trim().is_empty())
            .map(|p| (p.clone(), video::preset_path(p))),
        extra_args: profile.options.extra_args.clone(),
        launcher_dir: std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.to_path_buf())),
        system_dir: diagnostics::system_dir(),
        order_warnings: report.map(|r| r.order_warnings().iter().map(|w| w.text.clone()).collect()),
    }
}

/// Opens the panel and starts a run on a worker thread (some checks read files and ask
/// the graphics driver).
fn start_diagnostics(app: &mut App, ctx: &egui::Context, report: Option<&Report>) {
    let state = diagnostics_state(ctx);
    let mut d = state.lock().unwrap_or_else(|e| e.into_inner());
    d.open = true;
    if d.running.is_some() {
        return;
    }
    d.results = None;
    d.error = None;
    d.profile = app.cfg.active().name.clone();
    let input = diagnostics_input(app, report);
    let (tx, rx) = mpsc::channel();
    let repaint = ctx.clone();
    let spawned = std::thread::Builder::new().name("diagnostics".into()).spawn(move || {
        let findings = diagnostics::run(&input);
        let _ = tx.send(findings);
        repaint.request_repaint();
    });
    match spawned {
        Ok(_) => d.running = Some(rx),
        Err(e) => d.error = Some(format!("The diagnostics couldn't start: {e}")),
    }
}

/// Starts a diagnostics run and opens its panel, from outside the page (the command
/// palette's "Run diagnostics"). The results show on the Play page.
#[allow(dead_code)] // called by the command palette
pub fn run_diagnostics(app: &mut App, ctx: &egui::Context) {
    let report = profiles::conflict_report(app, ctx);
    start_diagnostics(app, ctx, report.as_deref());
}

fn finding_row(ui: &mut Ui, f: &Finding, chosen: &mut Option<Fix>) {
    let (glyph, color) = match f.severity {
        Severity::Error => (icon::CLOSE, theme::BAD),
        Severity::Warning => (icon::WARNING, theme::WARN),
        Severity::Ok => (icon::CHECK, theme::GOOD),
    };
    if f.severity == Severity::Ok {
        // Passed checks stay small: one line each.
        ui.horizontal(|ui| {
            ui.label(RichText::new(glyph).color(color).size(13.0));
            ui.label(theme::text(f.title.clone()));
            ui.add(egui::Label::new(theme::faint(f.detail.clone())).truncate()).on_hover_text(f.detail.clone());
        });
        return;
    }
    theme::inset_frame().show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.horizontal(|ui| {
            ui.label(RichText::new(glyph).color(color).size(15.0));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if let Some(fix) = &f.fix {
                    if theme::button(ui, fix.label()).clicked() {
                        *chosen = Some(fix.clone());
                    }
                }
                ui.with_layout(Layout::top_down(Align::Min), |ui| {
                    ui.spacing_mut().item_spacing.y = 2.0;
                    ui.add(egui::Label::new(theme::strong(f.title.clone())).wrap());
                    ui.add(egui::Label::new(theme::muted(f.detail.clone())).wrap());
                });
            });
        });
    });
}

fn diagnostics_modal(app: &mut App, ctx: &egui::Context, report: Option<&Report>) {
    let state = diagnostics_state(ctx);
    let mut close = false;
    let mut rerun = false;
    let mut copied = false;
    let mut chosen: Option<Fix> = None;
    {
        let mut d = state.lock().unwrap_or_else(|e| e.into_inner());
        if !d.open {
            return;
        }
        let finished = d.running.as_ref().map(|rx| rx.try_recv());
        match finished {
            Some(Ok(findings)) => {
                d.results = Some(findings);
                d.running = None;
            }
            Some(Err(TryRecvError::Disconnected)) => {
                d.error = Some("The diagnostics stopped unexpectedly.".to_owned());
                d.running = None;
            }
            Some(Err(TryRecvError::Empty)) | None => {}
        }

        let modal = egui::Modal::new(egui::Id::new("play_diagnostics_modal")).show(ctx, |ui| {
            ui.set_width(660.0);
            ui.horizontal(|ui| {
                ui.label(theme::title("Diagnostics"));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.add(egui::Label::new(theme::faint(format!("Profile: {}", d.profile))).truncate());
                });
            });
            if d.running.is_some() {
                ui.add_space(8.0);
                ui.horizontal(|ui| {
                    ui.add(egui::Spinner::new().size(16.0));
                    ui.label(theme::muted("Checking the game, the mods and the graphics driver…"));
                });
                ui.add_space(8.0);
            } else if let Some(e) = &d.error {
                ui.label(RichText::new(e.clone()).color(theme::BAD));
            } else if let Some(findings) = &d.results {
                let worst = if findings.iter().any(|f| f.severity == Severity::Error) {
                    theme::BAD
                } else if findings.iter().any(|f| f.severity == Severity::Warning) {
                    theme::WARN
                } else {
                    theme::GOOD
                };
                ui.label(RichText::new(diagnostics::summary(findings)).color(worst).family(theme::semibold()));
                ui.add_space(2.0);
                egui::ScrollArea::vertical().id_salt("play_diagnostics_list").max_height(440.0).show(ui, |ui| {
                    for (severity, heading) in [
                        (Severity::Error, "Needs fixing"),
                        (Severity::Warning, "Worth a look"),
                        (Severity::Ok, "Passed"),
                    ] {
                        let group: Vec<&Finding> = findings.iter().filter(|f| f.severity == severity).collect();
                        if group.is_empty() {
                            continue;
                        }
                        ui.add_space(4.0);
                        theme::overline(ui, heading);
                        ui.scope(|ui| {
                            ui.spacing_mut().item_spacing.y = if severity == Severity::Ok { 2.0 } else { 6.0 };
                            for f in group {
                                finding_row(ui, f, &mut chosen);
                            }
                        });
                    }
                });
            }
            ui.add_space(8.0);
            ui.horizontal(|ui| {
                let idle = d.running.is_none();
                rerun = ui.add_enabled_ui(idle, |ui| theme::button(ui, "Run again")).inner.clicked();
                if let Some(findings) = d.results.as_ref().filter(|_| idle) {
                    if theme::button(ui, "Copy report").clicked() {
                        ui.ctx().copy_text(diagnostics::as_text(findings));
                        copied = true;
                    }
                }
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    close = theme::primary_button(ui, "Close").clicked();
                });
            });
        });
        // A fix that goes to another page closes the panel; opening a folder doesn't.
        let leaves = chosen.as_ref().is_some_and(|f| !matches!(f, Fix::OpenFolder(_)));
        if close || leaves || modal.should_close() {
            d.open = false;
        }
    }
    if let Some(fix) = chosen {
        apply_fix(app, &fix);
    }
    if copied {
        app.ok("Diagnostics copied");
    }
    if rerun {
        start_diagnostics(app, ctx, report);
    }
}

// ------------------------------------------------------------------ stat cards

fn stat_card(ui: &mut Ui, add: impl FnOnce(&mut Ui)) {
    theme::card(ui, |ui| {
        ui.spacing_mut().item_spacing.x = 8.0;
        ui.set_min_height(CARD_H);
        add(ui);
    });
}

fn play_time_card(app: &mut App, ui: &mut Ui) {
    let profile = app.cfg.active();
    let live = |id: u64| {
        // Time from the session in progress isn't banked until the game exits.
        if app.runner.running_profile() == Some(id) {
            app.runner.elapsed().as_secs()
        } else {
            0
        }
    };
    let mine = profile.play_seconds + live(profile.id);
    let total: u64 = app.cfg.profiles.iter().map(|p| p.play_seconds + live(p.id)).sum();
    let last = match profile.last_played {
        Some(ts) if ts > 0 => theme::fmt_time(SystemTime::UNIX_EPOCH + Duration::from_secs(ts as u64)),
        _ => "Never".to_owned(),
    };
    let name = profile.name.clone();
    let count = app.cfg.profiles.len();

    stat_card(ui, |ui| {
        theme::overline(ui, "Play time");
        ui.add_space(4.0);
        ui.label(RichText::new(theme::fmt_duration(mine)).size(24.0).family(theme::semibold()).color(theme::TEXT));
        ui.add_space(-4.0);
        ui.add(egui::Label::new(theme::faint(format!("with {name}"))).truncate());
        ui.add_space(10.0);
        theme::separator(ui);
        ui.add_space(2.0);
        theme::kv(ui, "Last played", theme::text(last));
        let all = if count == 1 { "All profiles".to_owned() } else { format!("All {count} profiles") };
        theme::kv(ui, &all, theme::text(theme::fmt_duration(total)));
    });
}

#[derive(Clone)]
struct SavesCache {
    dir: Option<PathBuf>,
    read_at: f64,
    top: Arc<Vec<SaveInfo>>,
    total: usize,
}

fn latest_saves(app: &App, ui: &Ui) -> SavesCache {
    let id = egui::Id::new("play_latest_saves");
    let dir = app.saves_dir();
    let now = ui.input(|i| i.time);
    if let Some(c) = ui.data(|d| d.get_temp::<SavesCache>(id)) {
        if c.dir == dir && now - c.read_at < 3.0 {
            return c;
        }
    }
    let mut list = dir.as_deref().map(saves::list).unwrap_or_default();
    let total = list.len();
    list.truncate(4);
    let cache = SavesCache { dir, read_at: now, top: Arc::new(list), total };
    ui.data_mut(|d| d.insert_temp(id, cache.clone()));
    cache
}

fn saves_card(app: &mut App, ui: &mut Ui) {
    let cache = latest_saves(app, ui);
    stat_card(ui, |ui| {
        ui.horizontal(|ui| {
            theme::overline(ui, "Latest saves");
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let label = if cache.total > 4 { format!("All {} saves", cache.total) } else { "All saves".to_owned() };
                let link = ui
                    .add(egui::Button::new(RichText::new(label).size(12.0).color(theme::ACCENT_HI)).frame(false))
                    .on_hover_cursor(egui::CursorIcon::PointingHand);
                if link.clicked() {
                    app.page = Page::Saves;
                }
            });
        });
        if cache.top.is_empty() {
            small_empty(ui, icon::SAVE, "No saves yet", "Saves made in the game show up here.");
            return;
        }
        ui.spacing_mut().item_spacing.y = 5.0;
        for save in cache.top.iter() {
            save_row(ui, save);
        }
    });
}

fn save_row(ui: &mut Ui, save: &SaveInfo) {
    ui.horizontal(|ui| {
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            ui.label(theme::faint(ago(save.when()))).on_hover_text(theme::fmt_time(save.when()));
            ui.with_layout(Layout::top_down(Align::Min), |ui| {
                ui.spacing_mut().item_spacing.y = 0.0;
                ui.add(egui::Label::new(theme::text(save.name.clone())).truncate());
                let detail = if let Some(e) = &save.error {
                    RichText::new(e.clone()).size(12.0).color(theme::WARN)
                } else if save.is_campaign() {
                    let mut parts = vec![save.display_title()];
                    if !save.faction.is_empty() {
                        parts.push(save.faction.replace('_', " "));
                    }
                    parts.push(format!("week {}", save.week));
                    theme::faint(parts.join(" · "))
                } else {
                    theme::faint(save.display_title())
                };
                ui.add(egui::Label::new(detail).truncate());
            });
        });
    });
}

/// "5 min ago" for recent times, the date for older ones.
fn ago(t: SystemTime) -> String {
    let date = || {
        let dt: chrono::DateTime<chrono::Local> = t.into();
        dt.format("%d %b %Y").to_string()
    };
    let Ok(d) = SystemTime::now().duration_since(t) else { return date() };
    let s = d.as_secs();
    if s < 60 {
        "just now".to_owned()
    } else if s < 3600 {
        format!("{} min ago", s / 60)
    } else if s < 86_400 {
        format!("{} h ago", s / 3600)
    } else if s < 2 * 86_400 {
        "yesterday".to_owned()
    } else if s < 7 * 86_400 {
        format!("{} days ago", s / 86_400)
    } else {
        date()
    }
}

/// An empty state sized for a small card.
fn small_empty(ui: &mut Ui, glyph: &str, heading: &str, body: &str) {
    ui.vertical_centered(|ui| {
        ui.add_space(26.0);
        ui.label(RichText::new(glyph).size(26.0).color(theme::FAINT));
        ui.label(theme::strong(heading));
        ui.label(theme::faint(body));
    });
}

fn options_card(app: &mut App, ui: &mut Ui) {
    let mut changed = false;
    stat_card(ui, |ui| {
        let o = &mut app.cfg.active_mut().options;
        theme::overline(ui, "Quick launch options");
        ui.add_space(2.0);
        changed |= theme::toggle_row(ui, &mut o.skip_intro, "Skip intro", "Go straight to the main menu");
        ui.add_space(2.0);
        changed |= theme::toggle_row(ui, &mut o.frame_overlay, "Frame-time overlay", "F11 toggles it in game");
        ui.add_space(4.0);
        ui.label(theme::text("Frame cap"));
        ui.add_space(-4.0);
        let selected = FRAME_CAPS
            .iter()
            .find(|(v, _)| *v == o.frame_cap)
            .map(|(_, l)| (*l).to_owned())
            .unwrap_or_else(|| format!("{} fps", o.frame_cap.unwrap_or(0)));
        egui::ComboBox::from_id_salt("play_frame_cap")
            .selected_text(selected)
            .width(ui.available_width())
            .show_ui(ui, |ui| {
                for (value, label) in FRAME_CAPS {
                    if ui.selectable_label(o.frame_cap == value, label).clicked() && o.frame_cap != value {
                        o.frame_cap = value;
                        changed = true;
                    }
                }
            });
    });
    if changed {
        app.touch();
    }
}

// ------------------------------------------------------------------ command line

/// Returns true when "Run diagnostics" was pressed.
fn command_card(app: &mut App, ui: &mut Ui) -> bool {
    let args = launch::args_for(&app.cfg, &app.steam, app.cfg.active());
    let exe = app.cfg.settings.game_exe.clone();
    let exe_text = match &exe {
        Some(p) => runner::display_args(&[p.to_string_lossy().into_owned()]),
        None => "alamo_game.exe".to_owned(),
    };
    let command = format!("{exe_text} {}", runner::display_args(&args)).trim_end().to_owned();
    let folder = exe.as_ref().and_then(|p| p.parent()).map(|d| d.display().to_string());

    let mut copied = false;
    let mut run_diagnostics = false;
    theme::card(ui, |ui| {
        ui.horizontal(|ui| {
            theme::overline(ui, "Command line");
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if theme::icon_button(ui, icon::COPY, "Copy the command").clicked() {
                    ui.ctx().copy_text(command.clone());
                    copied = true;
                }
                run_diagnostics = theme::button(ui, &format!("{}  Run diagnostics", icon::SHIELD))
                    .on_hover_text("Check the game, the mods, the graphics driver and this command line before launching")
                    .clicked();
            });
        });
        ui.label(theme::faint("Exactly what the game is started with for this profile."));
        theme::inset_frame().show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.add(egui::Label::new(theme::mono(command.clone())).wrap().selectable(true));
        });
        match folder {
            Some(f) => ui.add(egui::Label::new(theme::faint(format!("Working folder: {f}"))).wrap()),
            None => ui.label(theme::faint("Working folder: the folder alamo_game.exe is in, once it's chosen in Settings.")),
        };
    });
    if copied {
        app.ok("Command copied");
    }
    run_diagnostics
}

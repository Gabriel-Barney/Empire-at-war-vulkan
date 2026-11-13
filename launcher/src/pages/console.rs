//! Game console: live output from the running game, how the last run ended, saved logs,
//! bug reports, and the hunt for a crashing mod.

use crate::app::{clock, open_in_explorer, reveal_in_explorer, App};
use crate::bisect::{self, Bisect, Outcome, Test, TestKind, Verdict};
use crate::runner::{self, ConsoleLine, LineKind, Runner};
use crate::steam::same_path;
use crate::theme::{self, icon};
use egui::{pos2, vec2, Align, Color32, CornerRadius, Layout, Rect, RichText, Sense, Stroke, TextStyle, Ui};
use std::path::PathBuf;
use std::sync::mpsc::{self, Receiver, TryRecvError};
use std::time::SystemTime;

struct LogFile {
    path: PathBuf,
    name: String,
    size: u64,
    modified: SystemTime,
}

/// A hunt for the crashing mod. It works on a copy of the profile's mod list taken when it
/// started; the profile itself is never changed while it runs.
struct Hunt {
    profile_id: u64,
    profile_name: String,
    mods: Vec<PathBuf>,
    names: Vec<String>,
    /// Dependencies being read from the mods' `modinfo.json` files on a worker thread.
    prep: Option<Receiver<Vec<Vec<usize>>>>,
    /// `None` until the user has read the caveats and started.
    machine: Option<Bisect>,
    /// `Runner::runs` of the launch made for the current step.
    launched: Option<u64>,
    /// The culprit was taken out of the profile from the result card.
    removed: bool,
}

/// What a click on this page asks for; carried out once the frame's borrows are over.
enum Act {
    CopyReport,
    SaveReport,
    OfferHunt,
    StartHunt,
    StopHunt,
    LaunchStep,
    Answer(Outcome),
    RemoveFromProfile(usize),
    OpenFolder(usize),
    CopyResult,
}

#[derive(Default)]
pub struct State {
    filter: String,
    errors_only: bool,
    /// Inverted so the default (false) means auto-scroll is on.
    manual_scroll: bool,
    jump_to_end: bool,
    /// What `filtered` was computed for: line revision, lower-cased filter, errors-only.
    cache_key: Option<(u64, String, bool)>,
    filtered: Vec<usize>,
    logs: Vec<LogFile>,
    logs_open: bool,
    logs_height: f32,
    last_pass: u64,
    was_running: bool,
    hunt: Option<Hunt>,
}

impl State {
    fn filtering(&self) -> bool {
        self.errors_only || !self.filter.trim().is_empty()
    }
}

pub fn show(app: &mut App, ui: &mut Ui) {
    theme::page_header(ui, "Game console", "What the game prints while it runs, and how the last run ended.");

    // Re-read the log folder when the page is opened or a run ends, not every frame.
    let pass = ui.ctx().cumulative_pass_nr();
    let running = app.runner.is_running();
    {
        let st = &mut app.console_ui;
        if st.last_pass == 0 || pass > st.last_pass + 1 || (st.was_running && !running) {
            st.logs = read_logs();
        }
        st.last_pass = pass.max(1);
        st.was_running = running;
    }
    if let Some(warning) = poll_hunt(&mut app.console_ui) {
        app.err(warning);
    }

    let mut act: Option<Act> = None;
    // The hunt is offered for a crash of the active profile, when there is something to halve.
    let active = app.cfg.active();
    let can_hunt = app.console_ui.hunt.is_none()
        && active.mods.len() >= 2
        && app.runner.last_run.as_ref().map(|r| r.profile_id) == Some(active.id);
    status_card(&mut app.runner, can_hunt, ui, &mut act);
    if let Some(hunt) = &app.console_ui.hunt {
        ui.add_space(4.0);
        hunt_card(hunt, &app.runner, app.cfg.active().id, ui, &mut act);
    }
    ui.add_space(4.0);

    // The output card takes whatever height the saved-logs card (measured last frame) leaves.
    let avail = ui.available_rect_before_wrap();
    let logs_h = if app.console_ui.logs_height > 0.0 { app.console_ui.logs_height } else { 62.0 };
    let bottom_pad = 20.0;
    let out_bottom = (avail.bottom() - bottom_pad - logs_h - 12.0).max(avail.top() + 140.0);
    let out_rect = Rect::from_min_max(avail.min, pos2(avail.right(), out_bottom));
    let logs_rect = Rect::from_min_max(pos2(avail.left(), out_bottom + 12.0), pos2(avail.right(), avail.bottom()));

    let mut toast: Option<String> = None;
    let mut out_ui = ui.new_child(
        egui::UiBuilder::new().id_salt("console_output").max_rect(out_rect).layout(Layout::top_down(Align::Min)),
    );
    output_card(&mut app.console_ui, &mut app.runner, &mut out_ui, &mut toast);

    let mut logs_ui = ui.new_child(
        egui::UiBuilder::new().id_salt("console_logs").max_rect(logs_rect).layout(Layout::top_down(Align::Min)),
    );
    logs_card(&mut app.console_ui, &mut logs_ui);
    let measured = logs_ui.min_rect().height();
    if (measured - app.console_ui.logs_height).abs() > 0.5 {
        app.console_ui.logs_height = measured;
        ui.ctx().request_repaint();
    }
    ui.allocate_rect(avail, Sense::hover());

    if let Some(t) = toast {
        app.ok(t);
    }
    if let Some(a) = act {
        perform(app, ui.ctx(), a);
    }
}

// ------------------------------------------------------------------ status

fn dot(ui: &mut Ui, color: Color32) {
    let (r, _) = ui.allocate_exact_size(vec2(10.0, 10.0), Sense::hover());
    ui.painter().circle_filled(r.center(), 4.5, color);
}

fn status_card(runner: &mut Runner, can_hunt: bool, ui: &mut Ui, act: &mut Option<Act>) {
    theme::card(ui, |ui| {
        if runner.is_running() {
            ui.horizontal(|ui| {
                dot(ui, theme::GOOD);
                ui.label(RichText::new("Running").family(theme::semibold()).color(theme::GOOD));
                ui.label(theme::muted(clock(runner.elapsed())));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if theme::danger_button(ui, "Stop").on_hover_text("Ends the game right away, without saving").clicked() {
                        runner.stop();
                    }
                });
            });
        } else if let Some(exit) = runner.last_exit.clone() {
            let crashed = exit.crashed();
            ui.horizontal(|ui| {
                dot(ui, if crashed { theme::BAD } else { theme::FAINT });
                let color = if crashed { theme::BAD } else { theme::TEXT };
                ui.label(RichText::new(exit.summary()).family(theme::semibold()).color(color));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if let Some(p) = &exit.log_path {
                        if theme::button(ui, "Show log file").on_hover_text(p.display().to_string()).clicked() {
                            reveal_in_explorer(p);
                        }
                    }
                    if crashed && can_hunt {
                        let tip = "Runs the game a few times with only some of the mods switched on to find the one that crashes it.";
                        if theme::primary_button(ui, "Find the crashing mod").on_hover_text(tip).clicked() {
                            *act = Some(Act::OfferHunt);
                        }
                    }
                });
            });
            ui.label(theme::faint(format!(
                "Ran for {} · ended {}",
                clock(exit.ran_for),
                theme::fmt_time(exit.when)
            )));
            let hint = match exit.code {
                _ if exit.stopped_by_user => None,
                Some(1) => Some(
                    "Code 1: startup failed or Vulkan validation reported problems. The last error lines below usually say why.",
                ),
                Some(2) => Some("Code 2: the game rejected an argument. Check the profile's extra arguments."),
                _ => None,
            };
            if let Some(h) = hint {
                ui.add(egui::Label::new(theme::muted(h)).wrap());
            }
        } else {
            ui.horizontal(|ui| {
                dot(ui, theme::FAINT);
                ui.label(theme::muted("The game hasn't been run this session. Its output appears here once it starts."));
            });
        }

        // A report describes the run this session knows about, so there must have been one.
        if runner.last_run.is_some() {
            ui.add_space(2.0);
            ui.horizontal(|ui| {
                let tip = "Launcher and Windows versions, the game file, the exact command line, the mods in load order, how the run ended and the last lines of output, as plain text.";
                if theme::button(ui, "Copy bug report").on_hover_text(tip).clicked() {
                    *act = Some(Act::CopyReport);
                }
                if theme::button(ui, "Save as…").on_hover_text("Write the same report to a text file").clicked() {
                    *act = Some(Act::SaveReport);
                }
                ui.label(theme::faint("Your Windows user name is replaced with <user> in every path."));
            });
        }
    });
}

// ------------------------------------------------------------------ bug report

fn report_text(app: &App) -> Option<String> {
    let run = app.runner.last_run.as_ref()?;
    let stamp = |t: SystemTime| {
        let dt: chrono::DateTime<chrono::Local> = t.into();
        dt.format("%Y-%m-%d %H:%M:%S").to_string()
    };
    let meta = std::fs::metadata(&run.exe).ok();
    let mods = run
        .mods
        .iter()
        .map(|path| {
            let info = app.find_mod(path);
            runner::ReportMod {
                name: app.mod_name(path),
                version: info.map(|m| m.version.clone()).unwrap_or_default(),
                source: match info {
                    Some(m) => m.source.label().to_owned(),
                    None if path.is_dir() => "not in the mod library".to_owned(),
                    None => "folder missing".to_owned(),
                },
                workshop_id: info.and_then(|m| m.workshop_id.clone()),
                path: path.display().to_string(),
            }
        })
        .collect();
    let running = app.runner.is_running();
    let exit = app.runner.last_exit.as_ref().filter(|_| !running).map(|e| runner::ReportExit {
        summary: e.summary(),
        code: e.code,
        ran_for: e.ran_for,
        ended: stamp(e.when),
    });
    let total = app.runner.lines.len();
    let input = runner::ReportInput {
        generated: stamp(SystemTime::now()),
        launcher_version: env!("CARGO_PKG_VERSION").to_owned(),
        windows: runner::windows_version(),
        exe: run.exe.display().to_string(),
        exe_size: meta.as_ref().map(|m| m.len()),
        exe_modified: meta.and_then(|m| m.modified().ok()).map(stamp),
        command_line: run.command_line(),
        work_dir: run.work_dir.display().to_string(),
        started: stamp(run.started),
        profile: run.profile_name.clone(),
        mods,
        exit,
        running_for: running.then(|| app.runner.elapsed()),
        lines: app.runner.lines.iter().skip(total.saturating_sub(runner::REPORT_LINES)).cloned().collect(),
        total_lines: total,
    };
    Some(runner::build_report(&input))
}

// ------------------------------------------------------------------ find the crashing mod

/// Picks up the dependency scan when it finishes. Returns a warning to show, if any.
fn poll_hunt(st: &mut State) -> Option<String> {
    let hunt = st.hunt.as_mut()?;
    let rx = hunt.prep.as_ref()?;
    let (parents, warning) = match rx.try_recv() {
        Ok(parents) => (parents, None),
        Err(TryRecvError::Empty) => return None,
        Err(TryRecvError::Disconnected) => (
            Vec::new(),
            Some("The mods' dependencies couldn't be read, so the hunt treats every mod as standing on its own.".to_owned()),
        ),
    };
    hunt.prep = None;
    hunt.machine = Some(Bisect::new(hunt.mods.clone(), parents));
    warning
}

fn quoted(names: &[String], i: usize) -> String {
    format!("\"{}\"", names.get(i).map(String::as_str).unwrap_or("?"))
}

/// What a step does, in a sentence or two, with the dependency notes spelled out.
fn step_text(test: &Test, suspects: usize, names: &[String]) -> Vec<String> {
    let mut out = Vec::new();
    match test.kind {
        TestKind::Baseline => out.push(
            "This run has every mod switched off. If the game still crashes, no mod is to blame.".to_owned(),
        ),
        TestKind::Split => {
            let off = names.len() - test.enabled.len();
            out.push(format!(
                "{suspects} mods are still under suspicion. This run switches half of them on, together with the mods already cleared; {off} {} off.",
                if off == 1 { "mod is" } else { "mods are" }
            ));
        }
        TestKind::Alone => {
            let c = test.enabled.iter().copied().find(|i| !test.on_for_child.iter().any(|(_, p)| p == i));
            out.push(format!(
                "One suspect is left. This run has only {} switched on, to see whether it crashes by itself.",
                c.map(|c| quoted(names, c)).unwrap_or_else(|| "it".to_owned())
            ));
        }
        TestKind::Together => out.push(
            "The last suspect works on its own. This run puts it back with the mods that were cleared, to see whether the crash needs them together.".to_owned(),
        ),
    }
    for &(d, p) in &test.off_with_parent {
        out.push(format!("{} stays off because it needs {}, which is off.", quoted(names, d), quoted(names, p)));
    }
    for &(c, p) in &test.on_for_child {
        out.push(format!("{} stays on because {} needs it.", quoted(names, p), quoted(names, c)));
    }
    out
}

fn chips(ui: &mut Ui, label: &str, items: &[&str], color: Color32) {
    ui.horizontal_wrapped(|ui| {
        ui.spacing_mut().item_spacing = vec2(6.0, 4.0);
        ui.label(theme::muted(label));
        if items.is_empty() {
            ui.label(theme::faint("none"));
        }
        for name in items {
            theme::chip(ui, *name, color);
        }
    });
}

fn hunt_card(hunt: &Hunt, runner: &Runner, active_profile: u64, ui: &mut Ui, act: &mut Option<Act>) {
    theme::card(ui, |ui| {
        ui.horizontal(|ui| {
            ui.label(RichText::new(icon::SEARCH).color(theme::ACCENT_HI));
            ui.label(theme::title("Find the crashing mod"));
            ui.label(theme::faint(format!("Profile: {}", hunt.profile_name)));
            if hunt.machine.as_ref().map(|m| m.verdict().is_none()).unwrap_or(false) {
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if theme::button(ui, "Stop").on_hover_text("End the hunt and forget its progress").clicked() {
                        *act = Some(Act::StopHunt);
                    }
                });
            }
        });

        let Some(machine) = &hunt.machine else {
            if hunt.prep.is_some() {
                ui.horizontal(|ui| {
                    ui.add(egui::Spinner::new().size(16.0));
                    ui.label(theme::muted("Reading which mods depend on which…"));
                });
            } else {
                hunt_intro(hunt, ui, act);
            }
            return;
        };

        if let Some(verdict) = machine.verdict() {
            hunt_result(hunt, machine, verdict, ui, act);
            return;
        }
        let Some(test) = machine.current() else { return };

        ui.label(theme::strong(format!("Step {} of about {}", machine.step(), machine.estimated_steps())));
        for line in step_text(test, machine.suspects().len(), &hunt.names) {
            ui.add(egui::Label::new(theme::muted(line)).wrap());
        }
        let on: Vec<&str> = test.enabled.iter().map(|&i| hunt.names[i].as_str()).collect();
        let off: Vec<&str> =
            (0..hunt.names.len()).filter(|i| !test.enabled.contains(i)).map(|i| hunt.names[i].as_str()).collect();
        egui::ScrollArea::vertical().id_salt("hunt_mods").max_height(92.0).auto_shrink([false, true]).show(ui, |ui| {
            chips(ui, &format!("On ({})", on.len()), &on, theme::GOOD);
            chips(ui, &format!("Off ({})", off.len()), &off, theme::MUTED);
        });
        ui.add_space(2.0);

        let running = runner.is_running();
        let this_run = hunt.launched.is_some() && hunt.launched == Some(runner.runs);
        let wrong_profile = active_profile != hunt.profile_id;
        if running && this_run {
            ui.horizontal(|ui| {
                ui.add(egui::Spinner::new().size(16.0));
                ui.add(
                    egui::Label::new(theme::text(
                        "The game is running with this step's mods. Do what made it crash, then close the game and come back here.",
                    ))
                    .wrap(),
                );
            });
            return;
        }
        let launch = |ui: &mut Ui, label: &str, primary: bool, act: &mut Option<Act>| {
            ui.add_enabled_ui(!running && !wrong_profile, |ui| {
                let r = if primary { theme::primary_button(ui, label) } else { theme::button(ui, label) };
                let r = r.on_disabled_hover_text(if running {
                    "The game is already running. Close it first."
                } else {
                    "The hunt was started on another profile. Switch back to it to continue."
                });
                if r.clicked() {
                    *act = Some(Act::LaunchStep);
                }
            });
        };
        if hunt.launched.is_none() {
            ui.horizontal(|ui| {
                launch(ui, "Launch this step", true, act);
                if wrong_profile {
                    ui.label(RichText::new(format!("Switch back to the profile \"{}\" to continue.", hunt.profile_name)).color(theme::WARN));
                } else if running {
                    ui.label(theme::muted("The game is running. Close it before launching this step."));
                }
            });
            return;
        }

        // The step has been run: ask how it went, with the answer the exit suggests in front.
        let exit = runner.last_exit.as_ref().filter(|_| this_run && !running);
        let suggestion = exit.and_then(|e| {
            if e.crashed() {
                Some(Outcome::Crashed)
            } else if e.code == Some(0) && !e.stopped_by_user {
                Some(Outcome::Worked)
            } else {
                None
            }
        });
        ui.horizontal(|ui| {
            ui.label(theme::strong("Did it crash?"));
            match exit {
                Some(e) => ui.label(theme::muted(format!("The game ended like this: {}", e.summary()))),
                None => ui.label(theme::muted("The game has been started again since this step, so go by what you saw.")),
            };
        });
        ui.horizontal(|ui| {
            let mut answer = |ui: &mut Ui, label: &str, outcome: Outcome| {
                let r = if suggestion == Some(outcome) { theme::primary_button(ui, label) } else { theme::button(ui, label) };
                if r.clicked() {
                    *act = Some(Act::Answer(outcome));
                }
            };
            answer(ui, "Crashed", Outcome::Crashed);
            answer(ui, "Worked", Outcome::Worked);
            launch(ui, "Launch again", false, act);
            ui.label(theme::faint(
                "The highlighted answer comes from how the game ended. Pick the other one if that isn't what happened.",
            ));
        });
    });
}

fn hunt_intro(hunt: &Hunt, ui: &mut Ui, act: &mut Option<Act>) {
    let n = hunt.mods.len();
    let runs = Bisect::new(hunt.mods.clone(), Vec::new()).estimated_steps();
    ui.add(
        egui::Label::new(theme::muted(format!(
            "The game is started about {runs} times, each time with only some of this profile's {n} mods switched on. After each run you say whether it crashed, until one mod is left."
        )))
        .wrap(),
    );
    ui.horizontal_top(|ui| {
        ui.label(RichText::new(icon::WARNING).color(theme::WARN));
        ui.add(
            egui::Label::new(
                RichText::new(
                    "A save only loads with the mod list it was made with. Test with a new game or a skirmish, not by loading a campaign save.",
                )
                .color(theme::WARN),
            )
            .wrap(),
        );
    });
    ui.add(
        egui::Label::new(theme::muted(
            "These runs use the profile's save folder, and the launcher's automatic checkpoint is skipped for them. The profile's mod list isn't changed.",
        ))
        .wrap(),
    );
    ui.add_space(2.0);
    ui.horizontal(|ui| {
        if theme::primary_button(ui, "Start").clicked() {
            *act = Some(Act::StartHunt);
        }
        if theme::button(ui, "Cancel").clicked() {
            *act = Some(Act::StopHunt);
        }
    });
}

fn hunt_result(hunt: &Hunt, machine: &Bisect, verdict: &Verdict, ui: &mut Ui, act: &mut Option<Act>) {
    let named = match verdict {
        Verdict::Culprit { culprit, .. } => Some(*culprit),
        Verdict::Combination { trigger, .. } => Some(*trigger),
        Verdict::NoModToBlame | Verdict::CantReproduce => None,
    };
    let (glyph, color) = match verdict {
        Verdict::Culprit { .. } => (icon::CHECK, theme::GOOD),
        Verdict::Combination { .. } => (icon::INFO, theme::WARN),
        Verdict::NoModToBlame | Verdict::CantReproduce => (icon::INFO, theme::MUTED),
    };
    ui.horizontal_top(|ui| {
        ui.label(RichText::new(glyph).color(color));
        ui.add(egui::Label::new(theme::text(bisect::verdict_text(verdict, &hunt.names))).wrap());
    });
    let steps = machine.history().len();
    ui.label(theme::faint(if steps == 1 { "Found in 1 run.".to_owned() } else { format!("Found in {steps} runs.") }));
    if matches!(verdict, Verdict::CantReproduce) {
        ui.add(
            egui::Label::new(theme::muted(
                "If the crash only shows up after playing for a while, run the hunt again and play each step for longer before answering.",
            ))
            .wrap(),
        );
    }
    ui.add_space(2.0);
    ui.horizontal(|ui| {
        if let Some(i) = named {
            let label = match verdict {
                Verdict::Culprit { .. } => "Remove it from the profile".to_owned(),
                _ => format!("Remove {} from the profile", quoted(&hunt.names, i)),
            };
            ui.add_enabled_ui(!hunt.removed, |ui| {
                let r = theme::button(ui, &label)
                    .on_hover_text(format!("Takes the mod out of \"{}\". The mod's files stay where they are.", hunt.profile_name))
                    .on_disabled_hover_text("Already removed from the profile.");
                if r.clicked() {
                    *act = Some(Act::RemoveFromProfile(i));
                }
            });
            if theme::button(ui, "Open its folder").clicked() {
                *act = Some(Act::OpenFolder(i));
            }
        }
        if theme::button(ui, "Copy result").on_hover_text("The steps and the result as plain text").clicked() {
            *act = Some(Act::CopyResult);
        }
        if theme::button(ui, "Close").clicked() {
            *act = Some(Act::StopHunt);
        }
    });
}

fn perform(app: &mut App, ctx: &egui::Context, act: Act) {
    match act {
        Act::CopyReport => match report_text(app) {
            Some(text) => {
                ctx.copy_text(text);
                app.ok("Bug report copied. Your Windows user name was replaced with <user>.");
            }
            None => app.err("There is no run to report yet. Launch the game first."),
        },
        Act::SaveReport => {
            let Some(text) = report_text(app) else {
                app.err("There is no run to report yet. Launch the game first.");
                return;
            };
            let name = format!("command-center-report-{}.txt", chrono::Local::now().format("%Y%m%d-%H%M%S"));
            let Some(path) = rfd::FileDialog::new().set_file_name(name).add_filter("Text", &["txt"]).save_file() else {
                return;
            };
            match std::fs::write(&path, text) {
                Ok(()) => app.ok(format!("Bug report saved to {}.", path.display())),
                Err(e) => app.err(format!("Couldn't write {}: {e}", path.display())),
            }
        }
        Act::OfferHunt => {
            let profile = app.cfg.active();
            let mods = profile.mods.clone();
            app.console_ui.hunt = Some(Hunt {
                profile_id: profile.id,
                profile_name: profile.name.clone(),
                names: mods.iter().map(|m| app.mod_name(m)).collect(),
                mods,
                prep: None,
                machine: None,
                launched: None,
                removed: false,
            });
        }
        Act::StartHunt => {
            let ids: Vec<Option<String>> = match &app.console_ui.hunt {
                Some(h) => h.mods.iter().map(|m| app.find_mod(m).and_then(|i| i.workshop_id.clone())).collect(),
                None => return,
            };
            let Some(hunt) = app.console_ui.hunt.as_mut() else { return };
            // Reading every mod's modinfo.json touches the disk, so it happens off this thread.
            let (tx, rx) = mpsc::channel();
            let (mods, ctx) = (hunt.mods.clone(), ctx.clone());
            std::thread::spawn(move || {
                let _ = tx.send(bisect::scan_parents(&mods, &ids));
                ctx.request_repaint();
            });
            hunt.prep = Some(rx);
        }
        Act::StopHunt => app.console_ui.hunt = None,
        Act::LaunchStep => {
            let Some(paths) = app.console_ui.hunt.as_ref().and_then(|h| h.machine.as_ref()).map(|m| m.enabled_paths())
            else {
                return;
            };
            let before = app.runner.runs;
            // Shows its own error if the game can't be started.
            app.launch_with(ctx, Some(paths));
            if app.runner.runs != before {
                if let Some(h) = app.console_ui.hunt.as_mut() {
                    h.launched = Some(app.runner.runs);
                }
            }
        }
        Act::Answer(outcome) => {
            if let Some(h) = app.console_ui.hunt.as_mut() {
                if let Some(m) = h.machine.as_mut() {
                    m.answer(outcome);
                }
                h.launched = None;
            }
        }
        Act::RemoveFromProfile(i) => {
            let Some((path, name, id, profile_name)) = app
                .console_ui
                .hunt
                .as_ref()
                .and_then(|h| Some((h.mods.get(i)?.clone(), h.names.get(i)?.clone(), h.profile_id, h.profile_name.clone())))
            else {
                return;
            };
            match app.cfg.profiles.iter_mut().find(|p| p.id == id) {
                Some(profile) => {
                    let before = profile.mods.len();
                    profile.mods.retain(|m| !same_path(m, &path));
                    let removed = profile.mods.len() != before;
                    let profile_name = profile.name.clone();
                    if removed {
                        app.touch();
                        app.ok(format!(
                            "\"{name}\" was removed from \"{profile_name}\". Add it back on the Profiles page if you change your mind."
                        ));
                    } else {
                        app.ok(format!("\"{name}\" is no longer in \"{profile_name}\"."));
                    }
                    if let Some(h) = app.console_ui.hunt.as_mut() {
                        h.removed = true;
                    }
                }
                None => app.err(format!("The profile \"{profile_name}\" no longer exists.")),
            }
        }
        Act::OpenFolder(i) => {
            let path = app.console_ui.hunt.as_ref().and_then(|h| h.mods.get(i).cloned());
            match path {
                Some(p) if p.is_dir() => open_in_explorer(&p),
                Some(p) => app.err(format!("The folder {} isn't there any more.", p.display())),
                None => {}
            }
        }
        Act::CopyResult => {
            let text = app.console_ui.hunt.as_ref().and_then(|h| h.machine.as_ref().map(|m| m.summary(&h.names)));
            if let Some(text) = text {
                ctx.copy_text(text);
                app.ok("Result copied.");
            }
        }
    }
}

// ------------------------------------------------------------------ live output

fn toggle_chip(ui: &mut Ui, on: &mut bool, label: &str) -> egui::Response {
    let (fill, stroke, fg) = if *on {
        (theme::ACCENT_DIM, theme::ACCENT.gamma_multiply(0.7), theme::ACCENT_HI)
    } else {
        (theme::CARD_HI, theme::BORDER, theme::MUTED)
    };
    let resp = ui.add(
        egui::Button::new(RichText::new(label).size(12.5).color(fg))
            .fill(fill)
            .stroke(Stroke::new(1.0_f32, stroke))
            .corner_radius(CornerRadius::same(255)),
    );
    if resp.clicked() {
        *on = !*on;
    }
    resp
}

fn refresh_filter(st: &mut State, runner: &Runner) {
    let needle = st.filter.trim().to_lowercase();
    let errors_only = st.errors_only;
    let key = (runner.revision, needle.clone(), errors_only);
    if st.cache_key.as_ref() == Some(&key) {
        return;
    }
    st.filtered.clear();
    if errors_only || !needle.is_empty() {
        st.filtered.extend(
            runner
                .lines
                .iter()
                .enumerate()
                .filter(|(_, l)| {
                    (!errors_only || l.kind == LineKind::Err)
                        && (needle.is_empty() || l.text.to_lowercase().contains(&needle))
                })
                .map(|(i, _)| i),
        );
    }
    st.cache_key = Some(key);
}

fn shown_count(st: &State, runner: &Runner) -> usize {
    if st.filtering() {
        st.filtered.len()
    } else {
        runner.lines.len()
    }
}

fn shown_line<'a>(st: &State, runner: &'a Runner, row: usize) -> Option<&'a ConsoleLine> {
    let idx = if st.filtering() { *st.filtered.get(row)? } else { row };
    runner.lines.get(idx)
}

fn line_color(line: &ConsoleLine) -> Color32 {
    match line.kind {
        LineKind::Out => theme::TEXT,
        LineKind::Launcher => theme::ACCENT_HI,
        LineKind::Err => {
            if line.text.to_ascii_lowercase().contains("error") {
                theme::BAD
            } else {
                theme::WARN
            }
        }
    }
}

fn output_card(st: &mut State, runner: &mut Runner, ui: &mut Ui, toast: &mut Option<String>) {
    theme::card_frame().show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.set_min_height(ui.available_height());

        ui.horizontal(|ui| {
            ui.label(RichText::new(icon::SEARCH).color(theme::FAINT));
            ui.add(egui::TextEdit::singleline(&mut st.filter).hint_text("Filter output").desired_width(220.0));
            if !st.filter.is_empty() && theme::icon_button(ui, icon::CLOSE, "Clear the filter").clicked() {
                st.filter.clear();
            }
            toggle_chip(ui, &mut st.errors_only, "Errors only")
                .on_hover_text("Show only what the game wrote to its error stream");
            let mut auto = !st.manual_scroll;
            if toggle_chip(ui, &mut auto, "Auto-scroll").on_hover_text("Follow new output as it arrives").clicked() {
                st.manual_scroll = !auto;
                st.jump_to_end = auto;
            }

            // The controls above decide what's shown, so the count to their right is current.
            refresh_filter(st, runner);

            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let count = shown_count(st, runner);
                ui.add_enabled_ui(count > 0, |ui| {
                    if theme::button(ui, "Copy all").on_hover_text("Copy the lines shown").clicked() {
                        let mut text = String::new();
                        for row in 0..count {
                            if let Some(l) = shown_line(st, runner, row) {
                                text.push_str(&format!("[{:9.3}] {}\n", l.at, l.text));
                            }
                        }
                        ui.ctx().copy_text(text);
                        *toast = Some(if count == 1 { "Copied 1 line".to_owned() } else { format!("Copied {count} lines") });
                    }
                });
                ui.add_enabled_ui(!runner.lines.is_empty(), |ui| {
                    if theme::button(ui, "Clear").on_hover_text("Empty this view. The log file keeps everything.").clicked() {
                        runner.clear();
                        refresh_filter(st, runner);
                        ui.ctx().request_repaint();
                    }
                });
                let total = runner.lines.len();
                let shown = shown_count(st, runner);
                let label = if st.filtering() {
                    format!("{shown} of {total} lines")
                } else if total == 1 {
                    "1 line".to_owned()
                } else {
                    format!("{total} lines")
                };
                ui.label(theme::faint(label));
            });
        });
        ui.add_space(2.0);

        theme::inset_frame().show(ui, |ui| {
            ui.set_width(ui.available_width());
            ui.set_min_height(ui.available_height());
            let count = shown_count(st, runner);
            if count == 0 {
                if runner.lines.is_empty() {
                    theme::empty_state(ui, icon::CONSOLE, "No output yet", "Launch the game and what it prints shows up here.");
                } else {
                    theme::empty_state(ui, icon::SEARCH, "Nothing matches", "No line fits the filter. Change or clear it to see more.");
                }
                return;
            }

            ui.spacing_mut().item_spacing.y = 1.0;
            let font = TextStyle::Monospace.resolve(ui.style());
            let row_h = ui.text_style_height(&TextStyle::Monospace);
            let mut area = egui::ScrollArea::both()
                .id_salt("console_lines")
                .auto_shrink([false, false])
                .stick_to_bottom(!st.manual_scroll);
            if st.jump_to_end {
                st.jump_to_end = false;
                area = area.vertical_scroll_offset(count as f32 * (row_h + 1.0));
                ui.ctx().request_repaint();
            }
            area.show_rows(ui, row_h, count, |ui, range| {
                ui.style_mut().wrap_mode = Some(egui::TextWrapMode::Extend);
                for row in range {
                    let Some(line) = shown_line(st, runner, row) else { continue };
                    let mut job = egui::text::LayoutJob::default();
                    job.append(
                        &format!("{:>9.3}  ", line.at),
                        0.0,
                        egui::TextFormat { font_id: font.clone(), color: theme::FAINT, ..Default::default() },
                    );
                    job.append(
                        &line.text,
                        0.0,
                        egui::TextFormat { font_id: font.clone(), color: line_color(line), ..Default::default() },
                    );
                    ui.add(egui::Label::new(job).extend());
                }
            });
        });
    });
}

// ------------------------------------------------------------------ saved logs

fn read_logs() -> Vec<LogFile> {
    let mut out: Vec<LogFile> = std::fs::read_dir(runner::logs_dir())
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|e| {
            let path = e.path();
            let meta = e.metadata().ok()?;
            if !meta.is_file() || !path.extension().map(|x| x.eq_ignore_ascii_case("log")).unwrap_or(false) {
                return None;
            }
            Some(LogFile {
                name: path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(),
                size: meta.len(),
                modified: meta.modified().unwrap_or(SystemTime::UNIX_EPOCH),
                path,
            })
        })
        .collect();
    out.sort_by(|a, b| b.modified.cmp(&a.modified).then_with(|| b.name.cmp(&a.name)));
    out
}

fn logs_card(st: &mut State, ui: &mut Ui) {
    theme::card(ui, |ui| {
        ui.horizontal(|ui| {
            ui.label(theme::strong("Saved log files"));
            let n = st.logs.len();
            ui.label(theme::faint(match n {
                0 => "None yet · every session is recorded".to_owned(),
                1 => "1 file · every session is recorded".to_owned(),
                n => format!("{n} files · every session is recorded"),
            }));
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if theme::button(ui, if st.logs_open { "Hide" } else { "Show" }).clicked() {
                    st.logs_open = !st.logs_open;
                }
                if theme::button(ui, "Open logs folder").clicked() {
                    open_in_explorer(&runner::logs_dir());
                }
                if theme::icon_button(ui, icon::REFRESH, "Refresh the list").clicked() {
                    st.logs = read_logs();
                }
            });
        });
        if !st.logs_open {
            return;
        }
        theme::separator(ui);
        if st.logs.is_empty() {
            theme::empty_state(ui, icon::HISTORY, "No log files yet", "A log is written each time the game is launched from here.");
            return;
        }
        egui::ScrollArea::vertical().id_salt("console_log_files").auto_shrink([false, true]).max_height(168.0).show(
            ui,
            |ui| {
                for f in &st.logs {
                    ui.horizontal(|ui| {
                        ui.label(theme::mono(f.name.clone()));
                        ui.label(theme::faint(format!("{} · {}", theme::fmt_bytes(f.size), theme::fmt_time(f.modified))));
                        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                            ui.add_space(10.0);
                            if theme::button(ui, "Reveal").on_hover_text("Show the file in its folder").clicked() {
                                reveal_in_explorer(&f.path);
                            }
                            if theme::button(ui, "Open").clicked() {
                                open_in_explorer(&f.path);
                            }
                        });
                    });
                }
            },
        );
    });
}

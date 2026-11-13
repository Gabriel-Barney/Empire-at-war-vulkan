//! Application state, the sidebar, the tabs / split view / page windows, keyboard
//! shortcuts, launching the game, and page dispatch.

use crate::config::{mod_key, Config, WhileRunning};
use crate::layout::{self, Place, Side};
use crate::mods::{self, ModInfo};
use crate::pages;
use crate::palette::Palette;
use crate::runner::Runner;
use crate::steam::{self, SteamInfo};
use crate::theme::{self, icon};
use crate::{launch, notify, saves, steam::same_path, Cli};
use egui::{
    pos2, vec2, Align, Align2, Color32, CornerRadius, FontFamily, FontId, Id, Layout, Margin, Rect, RichText, Sense,
    Stroke, StrokeKind, ViewportCommand, ViewportId,
};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::atomic::Ordering;
use std::sync::mpsc;
use std::time::{Duration, Instant};

#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash, PartialOrd, Ord)]
pub enum Page {
    Play,
    Profiles,
    Mods,
    Saves,
    Units,
    Graphics,
    Console,
    Settings,
}

/// Sidebar order: page, icon, label. Ctrl+1 … Ctrl+8 follow it.
const NAV: [(Page, &str, &str); 8] = [
    (Page::Play, icon::PLAY, "Play"),
    (Page::Profiles, icon::PEOPLE, "Profiles"),
    (Page::Mods, icon::PUZZLE, "Mods"),
    (Page::Saves, icon::SAVE, "Saves & checkpoints"),
    (Page::Units, icon::DATABASE, "Unit database"),
    (Page::Graphics, icon::DISPLAY, "Graphics"),
    (Page::Console, icon::CONSOLE, "Game console"),
    (Page::Settings, icon::SETTINGS, "Settings"),
];

impl Page {
    /// Every page, in sidebar order.
    pub const ALL: [Page; 8] = [
        Page::Play,
        Page::Profiles,
        Page::Mods,
        Page::Saves,
        Page::Units,
        Page::Graphics,
        Page::Console,
        Page::Settings,
    ];

    /// Position in the sidebar, from 0.
    pub fn index(self) -> usize {
        Page::ALL.iter().position(|p| *p == self).unwrap_or(0)
    }

    /// The name in the sidebar.
    pub fn label(self) -> &'static str {
        NAV[self.index()].2
    }

    /// A shorter name, for tabs.
    pub fn short(self) -> &'static str {
        match self {
            Page::Saves => "Saves",
            Page::Units => "Units",
            Page::Console => "Console",
            other => other.label(),
        }
    }

    pub fn glyph(self) -> &'static str {
        NAV[self.index()].1
    }

    /// The name used in the config file and in `COMMAND_CENTER_PAGE`.
    pub fn key(self) -> &'static str {
        match self {
            Page::Play => "play",
            Page::Profiles => "profiles",
            Page::Mods => "mods",
            Page::Saves => "saves",
            Page::Units => "units",
            Page::Graphics => "graphics",
            Page::Console => "console",
            Page::Settings => "settings",
        }
    }

    pub fn from_key(key: &str) -> Option<Page> {
        Page::ALL.into_iter().find(|p| p.key() == key)
    }
}

/// Smallest size of the main window, and the wider minimum while it shows two pages.
pub const MIN_WINDOW: [f32; 2] = [1080.0, 680.0];
const MIN_WINDOW_SPLIT: [f32; 2] = [1280.0, 680.0];
const STRIP_H: f32 = 38.0;
const DIVIDER_W: f32 = 7.0;
const MIN_PANE: f32 = 380.0;
const STRIP_BG: Color32 = Color32::from_rgb(6, 8, 12);
const HOVER_BG: Color32 = Color32::from_rgb(22, 27, 36);

pub struct Toast {
    pub text: String,
    pub error: bool,
    pub until: Instant,
}

/// Lazily decoded mod art.
#[derive(Default)]
pub struct Textures {
    map: HashMap<PathBuf, Option<egui::TextureHandle>>,
    budget: u32,
}

impl Textures {
    pub fn begin_frame(&mut self) {
        self.budget = 1;
    }

    pub fn get(&mut self, ctx: &egui::Context, path: &Path) -> Option<egui::TextureHandle> {
        if let Some(t) = self.map.get(path) {
            return t.clone();
        }
        // Decode at most one image per frame so scrolling a page of mods stays smooth.
        if self.budget == 0 {
            ctx.request_repaint();
            return None;
        }
        self.budget -= 1;
        let tex = image::open(path).ok().map(|img| {
            let img = if img.width() > 1280 || img.height() > 1280 {
                img.resize(1280, 1280, image::imageops::FilterType::Triangle)
            } else {
                img
            };
            let rgba = img.to_rgba8();
            let size = [rgba.width() as usize, rgba.height() as usize];
            let color = egui::ColorImage::from_rgba_unmultiplied(size, rgba.as_raw());
            ctx.load_texture(path.to_string_lossy(), color, egui::TextureOptions::LINEAR)
        });
        self.map.insert(path.to_path_buf(), tex.clone());
        ctx.request_repaint();
        tex
    }

    pub fn clear(&mut self) {
        self.map.clear();
    }
}

/// A change to the tabs, collected while the strips are drawn and applied afterwards.
enum Op {
    Activate(Side, usize),
    Focus(Side),
    Close(Page),
    CloseOthers(Page),
    Move(Page, Side, usize),
    Beside(Page, Side),
    OtherSide(Page),
    Pop(Page),
    ToggleSplit,
    Ratio(f32),
}

/// A tab being dragged.
struct TabDrag {
    page: Page,
    /// Last known pointer position (it is lost once the pointer leaves the window).
    pos: egui::Pos2,
}

/// Where a dragged tab would land.
#[derive(Clone, Copy, PartialEq)]
enum DropTarget {
    /// Between the tabs of a strip.
    Strip(Side, usize),
    /// Onto one side of the content area.
    Pane(Side),
    /// Outside the content area: its own window.
    Window,
    Nowhere,
}

/// One tab strip as drawn this frame, for hit-testing a drag.
struct StripGeom {
    rect: Rect,
    tabs: Vec<(Page, Rect)>,
}

/// What the launcher remembers about the game run in progress.
struct Run {
    /// Started with the profile's own mods (not a one-off list from the crash hunt).
    normal: bool,
    /// The launcher minimised itself for this run.
    minimized: bool,
    /// "Close the launcher" setting: when to do it.
    close_at: Option<Instant>,
    next_checkpoint: Option<Instant>,
    checkpoint_error_shown: bool,
    watch: Option<notify::ExitWatch>,
}

type CheckpointResult = Result<Option<saves::Checkpoint>, String>;

pub struct App {
    pub cfg: Config,
    pub steam: SteamInfo,
    /// The page in the focused pane of the main window. Pages may assign it to navigate
    /// (`app.page = Page::Settings`); [`App::navigate`] does the same thing.
    pub page: Page,
    pub mods: Vec<ModInfo>,
    pub tex: Textures,
    pub runner: Runner,
    pub toasts: Vec<Toast>,
    dirty_since: Option<Instant>,
    /// Open pages: tabs, the split, and pages in their own windows.
    pub layout: layout::Layout,
    /// The layout came from `COMMAND_CENTER_PAGE` & co., so it isn't written to the config.
    layout_from_env: bool,
    /// What `page` was last set to by the app itself, to notice when something else assigns it.
    synced_page: Page,
    /// The page being drawn right now.
    drawing: Option<Page>,
    /// Pages already drawn this frame, one bit each: a page is never drawn twice.
    drawn: u8,
    /// Set while a page window (not the main window) is being drawn.
    in_window: bool,
    focus_window: Option<Page>,
    focus_main: bool,
    drag: Option<TabDrag>,
    strips: [Option<StripGeom>; 2],
    content_rect: Rect,
    /// Where each page window was last seen, for this session.
    window_pos: HashMap<Page, egui::Pos2>,
    /// Position and size each open page window was created with. Kept fixed while it is
    /// open: egui would move the window to follow a changing value.
    window_start: HashMap<Page, (Option<egui::Pos2>, [f32; 2])>,
    min_size_split: Option<bool>,
    toast_viewport: ViewportId,
    pub palette: Palette,
    /// `--launch` on the command line: start the game as soon as the window is up.
    autolaunch: bool,
    /// The main window's Win32 handle, to bring it back when the game exits.
    hwnd: Option<isize>,
    run: Option<Run>,
    timed_checkpoint: Option<mpsc::Receiver<CheckpointResult>>,
    logo: Option<egui::TextureHandle>,
    pub profiles_ui: pages::profiles::State,
    pub mods_ui: pages::mods::State,
    pub saves_ui: pages::saves::State,
    pub units_ui: pages::units::State,
    pub graphics_ui: pages::graphics::State,
    pub console_ui: pages::console::State,
}

fn env_text(name: &str) -> Option<String> {
    std::env::var(name).ok().map(|v| v.trim().to_lowercase()).filter(|v| !v.is_empty())
}

/// The layout the screenshot variables ask for, if any of them is set:
/// * `COMMAND_CENTER_PAGE=<name>` — the page shown and focused in the main window;
/// * `COMMAND_CENTER_WINDOWS=saves,units` — pages opened in their own windows;
/// * `COMMAND_CENTER_TABS=mods,saves|units` — more tabs next to the page; anything after
///   `|` goes into a second pane (split view).
fn layout_from_env(page: Option<&str>, windows: Option<&str>, tabs: Option<&str>) -> Option<layout::Layout> {
    if page.is_none() && windows.is_none() && tabs.is_none() {
        return None;
    }
    let first = page.and_then(page_from_name).unwrap_or(Page::Play);
    let mut l = layout::Layout::single(first);
    let (left, right) = tabs.unwrap_or("").split_once('|').unwrap_or((tabs.unwrap_or(""), ""));
    for name in left.split(',') {
        if let Some(p) = page_from_name(name.trim()) {
            l.open_background(p);
        }
    }
    for name in right.split(',') {
        if let Some(p) = page_from_name(name.trim()).filter(|p| *p != first) {
            let end = l.pane(Side::Right).map_or(0, |p| p.tabs().len());
            if l.is_split() {
                l.move_tab(p, Side::Right, end);
            } else {
                l.beside(p, Side::Right);
            }
        }
    }
    // Show the first tab of the second pane, and leave the focus on the page asked for.
    if l.is_split() {
        l.activate(Side::Right, 0);
    }
    l.open(first);
    for name in windows.unwrap_or("").split(',') {
        if let Some(p) = page_from_name(name.trim()).filter(|p| *p != first) {
            l.pop(p);
        }
    }
    Some(l)
}

impl App {
    pub fn new(cc: &eframe::CreationContext<'_>, mut cfg: Config, cli: Cli) -> Self {
        theme::install(&cc.egui_ctx);
        let steam = steam::detect();
        // First run: pick up a game build sitting next to the launcher.
        if cfg.settings.game_exe.is_none() {
            if let Some(p) = std::env::current_exe().ok().and_then(|e| e.parent().map(|d| d.join("alamo_game.exe"))) {
                if p.is_file() {
                    cfg.settings.game_exe = Some(p);
                }
            }
        }
        let mods = mods::scan(&steam, &cfg.settings);

        // `--profile "Name"` (what the desktop shortcuts use) picks the profile for this run.
        let mut dirty = false;
        let mut autolaunch = cli.launch;
        let mut startup_error = None;
        if let Some(wanted) = &cli.profile {
            match crate::find_profile(&cfg.profiles, wanted) {
                Some(id) => {
                    dirty = cfg.active_profile != id;
                    cfg.active_profile = id;
                }
                None => {
                    // Don't start the wrong profile's game by accident.
                    autolaunch = false;
                    startup_error = Some(if cli.launch {
                        format!("There is no profile named \"{wanted}\", so the game wasn't started.")
                    } else {
                        format!("There is no profile named \"{wanted}\".")
                    });
                }
            }
        }

        // Screenshot variables replace the remembered layout for this run.
        let start = env_text("COMMAND_CENTER_PAGE");
        let from_env = layout_from_env(
            start.as_deref(),
            env_text("COMMAND_CENTER_WINDOWS").as_deref(),
            env_text("COMMAND_CENTER_TABS").as_deref(),
        );
        let layout_from_env = from_env.is_some();
        let layout = from_env.unwrap_or_else(|| cfg.layout.clone());
        let page = layout.focused_page();

        let mut saves_ui = pages::saves::State::default();
        // COMMAND_CENTER_PAGE=checkpoints|map opens the Saves page on that tab.
        saves_ui.start_tab = Some(start.unwrap_or_default());

        let mut app = Self {
            cfg,
            steam,
            page,
            mods,
            tex: Textures::default(),
            runner: Runner::default(),
            toasts: Vec::new(),
            dirty_since: None,
            layout,
            layout_from_env,
            synced_page: page,
            drawing: None,
            drawn: 0,
            in_window: false,
            focus_window: None,
            focus_main: false,
            drag: None,
            strips: [None, None],
            content_rect: Rect::NOTHING,
            window_pos: HashMap::new(),
            window_start: HashMap::new(),
            min_size_split: None,
            toast_viewport: ViewportId::ROOT,
            palette: Palette::default(),
            autolaunch,
            hwnd: main_window_handle(cc),
            run: None,
            timed_checkpoint: None,
            logo: load_logo(&cc.egui_ctx),
            profiles_ui: Default::default(),
            mods_ui: Default::default(),
            saves_ui,
            units_ui: Default::default(),
            graphics_ui: Default::default(),
            console_ui: Default::default(),
        };
        if dirty {
            app.touch();
        }
        if let Some(e) = startup_error {
            app.err(e);
        }
        app
    }

    /// Marks the config changed; it's written shortly after edits stop.
    pub fn touch(&mut self) {
        self.dirty_since.get_or_insert_with(Instant::now);
    }

    pub fn ok(&mut self, text: impl Into<String>) {
        self.toasts.push(Toast { text: text.into(), error: false, until: Instant::now() + Duration::from_secs(4) });
    }

    pub fn err(&mut self, text: impl Into<String>) {
        self.toasts.push(Toast { text: text.into(), error: true, until: Instant::now() + Duration::from_secs(9) });
    }

    pub fn rescan_mods(&mut self) {
        self.mods = mods::scan(&self.steam, &self.cfg.settings);
        self.tex.clear();
    }

    pub fn find_mod(&self, path: &Path) -> Option<&ModInfo> {
        self.mods.iter().find(|m| same_path(&m.path, path))
    }

    /// The user's own name for a mod, else its modinfo name, else the folder name.
    pub fn mod_name(&self, path: &Path) -> String {
        if let Some(n) = self.cfg.mod_names.get(&mod_key(path)) {
            return n.clone();
        }
        match self.find_mod(path) {
            Some(m) => m.name.clone(),
            None => path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_else(|| path.display().to_string()),
        }
    }

    pub fn saves_dir(&self) -> Option<PathBuf> {
        launch::saves_dir(self.cfg.active())
    }
}

// ------------------------------------------------------------------ navigation

impl App {
    /// Shows `page` and gives it the focus: its tab if it has one (in either pane), its
    /// window if it is in one, otherwise a new tab in the focused pane.
    pub fn navigate(&mut self, page: Page) {
        match self.layout.open(page) {
            Place::Window => self.focus_window = Some(page),
            // Asked for from a page window: bring the main window forward too.
            Place::Tab(..) => self.focus_main |= self.in_window,
        }
        self.sync_page();
    }

    /// Opens `page` as a tab without switching to it.
    pub fn open_in_background(&mut self, page: Page) {
        self.layout.open_background(page);
    }

    /// Shows `page` next to the current one, splitting the main window if needed.
    pub fn open_to_side(&mut self, page: Page) {
        if self.layout.to_other_side(page) {
            self.focus_main |= self.in_window;
        } else {
            self.err("Open a second page first. This is the only one in the window.");
        }
        self.sync_page();
    }

    /// Moves `page` into a window of its own (or brings that window forward).
    pub fn open_in_window(&mut self, page: Page) {
        if self.layout.pop(page) {
            self.focus_window = Some(page);
        } else {
            self.err("The main window needs at least one page.");
        }
        self.sync_page();
    }

    /// Closes `page`'s tab; a page in its own window comes back as a tab instead.
    pub fn close_tab(&mut self, page: Page) {
        self.layout.close(page);
        self.sync_page();
    }

    /// Splits the main window in two, or merges it back.
    pub fn toggle_split(&mut self) {
        if !self.layout.toggle_split() {
            self.err("Open a second page first, then split the view.");
        }
        self.sync_page();
    }

    /// Keeps `page` equal to the focused page, except while a page is being drawn (it may
    /// be about to assign it).
    fn sync_page(&mut self) {
        if self.drawing.is_none() {
            self.page = self.layout.focused_page();
            self.synced_page = self.page;
        }
    }

    fn apply(&mut self, op: Op) {
        match op {
            Op::Activate(side, index) => self.layout.activate(side, index),
            Op::Focus(side) => self.layout.set_focus(side),
            Op::Close(page) => {
                self.layout.close(page);
            }
            Op::CloseOthers(page) => {
                self.layout.close_others(page);
            }
            Op::Move(page, side, index) => {
                self.layout.move_tab(page, side, index);
            }
            Op::Beside(page, side) => {
                self.layout.beside(page, side);
            }
            Op::OtherSide(page) => self.open_to_side(page),
            Op::Pop(page) => self.open_in_window(page),
            Op::ToggleSplit => self.toggle_split(),
            Op::Ratio(ratio) => self.layout.set_ratio(ratio),
        }
        self.sync_page();
    }
}

// ------------------------------------------------------------------ launching

impl App {
    pub fn launch(&mut self, ctx: &egui::Context) {
        self.launch_with(ctx, None);
    }

    /// Starts the game; `mods_override` replaces the active profile's mod list for this
    /// one run (used when hunting for a crashing mod).
    pub fn launch_with(&mut self, ctx: &egui::Context, mods_override: Option<Vec<PathBuf>>) {
        let normal = mods_override.is_none();
        let plan = match launch::plan_with(&self.cfg, &self.steam, mods_override.as_deref()) {
            Ok(plan) => plan,
            Err(e) => return self.err(e),
        };
        if self.runner.is_running() {
            return self.err("The game is already running.");
        }
        let profile = self.cfg.active().clone();

        // The profile's "checkpoint before launch". A failure is reported but never stops
        // the game from starting; one-off runs with another mod list leave checkpoints alone.
        if normal && profile.checkpoint_before_launch {
            if let Some(dir) = self.saves_dir() {
                let label = format!("Before launch \u{b7} {}", chrono::Local::now().format("%d %b %Y, %H:%M"));
                match saves::auto_checkpoint(&dir, &label, profile.checkpoint_keep) {
                    Ok(Some(_)) => {
                        self.ok("Checkpoint taken before launch.");
                        self.saves_ui.refresh = true;
                    }
                    Ok(None) => {}
                    Err(e) => self.err(format!("The checkpoint before launch failed: {e}")),
                }
            }
        }

        if let Err(e) = self.runner.start(&plan.exe, &plan.args, &plan.work_dir, profile.id, &profile.name, ctx) {
            return self.err(e);
        }
        let now = chrono::Local::now().timestamp();
        self.cfg.active_mut().last_played = Some(now);
        if normal {
            for m in &profile.mods {
                self.cfg.mod_last_used.insert(mod_key(m), now);
            }
        }
        self.touch();

        let mut run = Run {
            normal,
            minimized: false,
            close_at: None,
            next_checkpoint: None,
            checkpoint_error_shown: false,
            watch: None,
        };
        match self.cfg.settings.while_running {
            WhileRunning::Stay => {}
            WhileRunning::Minimize => {
                self.minimize_windows(ctx, true);
                run.minimized = true;
            }
            // Wait a moment first: a game that fails to start should leave the launcher
            // open to say why. The crash hunt needs the launcher to see how the run ends.
            WhileRunning::Close if normal => run.close_at = Some(Instant::now() + Duration::from_secs(3)),
            WhileRunning::Close => {}
        }
        run.watch = self.runner.pid().map(|pid| {
            notify::watch(notify::WatchRequest {
                pid,
                hwnd: self.hwnd,
                restore: run.minimized,
                notify: self.cfg.settings.notify_on_exit,
                profile: profile.name.clone(),
            })
        });
        self.run = Some(run);
    }

    /// Ctrl+Enter and the palette: launch, or say why not.
    pub fn launch_or_explain(&mut self, ctx: &egui::Context) {
        if self.runner.is_running() {
            self.ok("The game is already running.");
        } else {
            self.launch(ctx);
        }
    }

    fn minimize_windows(&self, ctx: &egui::Context, minimized: bool) {
        ctx.send_viewport_cmd_to(ViewportId::ROOT, ViewportCommand::Minimized(minimized));
        for page in self.layout.windows() {
            ctx.send_viewport_cmd_to(popout_id(*page), ViewportCommand::Minimized(minimized));
        }
    }

    /// Is any of the launcher's windows the one the user is working in?
    fn has_focus(&self, ctx: &egui::Context) -> bool {
        let focused = |id: ViewportId| ctx.input_for(id, |i| i.viewport().focused) != Some(false);
        focused(ViewportId::ROOT) || self.layout.windows().iter().any(|p| ctx.input_for(popout_id(*p), |i| i.viewport().focused) == Some(true))
    }

    /// Once a frame: collect the game's output, bank play time when it exits, and do what
    /// the profile and settings ask for while it runs.
    fn game_tick(&mut self, ctx: &egui::Context) {
        if let Some(rx) = &self.timed_checkpoint {
            match rx.try_recv() {
                Ok(result) => {
                    self.timed_checkpoint = None;
                    self.timed_checkpoint_done(result);
                }
                Err(mpsc::TryRecvError::Empty) => {}
                Err(mpsc::TryRecvError::Disconnected) => self.timed_checkpoint = None,
            }
        }

        if let Some(done) = self.runner.poll() {
            let profile_name = match self.cfg.profiles.iter_mut().find(|p| p.id == done.profile_id) {
                Some(p) => {
                    p.play_seconds += done.seconds;
                    p.name.clone()
                }
                None => self.cfg.active().name.clone(),
            };
            self.touch();
            self.saves_ui.refresh = true;
            let exit = self.runner.last_exit.clone();
            let problem = exit.as_ref().filter(|e| e.crashed()).map(|e| e.summary());
            if let Some(problem) = &problem {
                self.err(format!("{problem} See Game console."));
            }

            let run = self.run.take();
            let watch = run.as_ref().and_then(|r| r.watch.as_ref());
            if let Some(watch) = watch {
                watch.handled.store(true, Ordering::SeqCst);
            }
            let focused = self.has_focus(ctx);
            if run.as_ref().is_some_and(|r| r.minimized) {
                self.minimize_windows(ctx, false);
                ctx.send_viewport_cmd_to(ViewportId::ROOT, ViewportCommand::Focus);
            }
            let stopped_here = exit.as_ref().is_some_and(|e| e.stopped_by_user);
            if self.cfg.settings.notify_on_exit && !focused && !stopped_here {
                // The watcher thread may have shown it already (see notify.rs).
                let already = watch.is_some_and(|w| w.notified.swap(true, Ordering::SeqCst));
                if !already {
                    let (title, body) = notify::exit_message(problem.as_deref(), done.seconds, &profile_name);
                    notify::show(&title, &body);
                }
            }
            return;
        }

        if !self.runner.is_running() {
            return;
        }
        ctx.request_repaint_after(Duration::from_millis(500));
        let now = Instant::now();
        let Some(run) = &mut self.run else { return };
        if run.close_at.is_some_and(|at| now >= at) {
            run.close_at = None;
            ctx.send_viewport_cmd_to(ViewportId::ROOT, ViewportCommand::Close);
        }
        if !run.normal {
            return;
        }
        // The profile's "checkpoint every N minutes", read live so a change applies at once.
        let Some(profile) = self.runner.running_profile().and_then(|id| self.cfg.profiles.iter().find(|p| p.id == id)) else {
            return;
        };
        let every = Duration::from_secs(profile.checkpoint_every_minutes as u64 * 60);
        if every.is_zero() {
            run.next_checkpoint = None;
            return;
        }
        let due = *run.next_checkpoint.get_or_insert(now + every);
        if now < due {
            return;
        }
        run.next_checkpoint = Some(now + every);
        // Still copying the previous one (a very large save folder): skip this turn.
        if self.timed_checkpoint.is_some() {
            return;
        }
        let Some(dir) = launch::saves_dir(profile) else { return };
        let keep = profile.checkpoint_keep;
        let (tx, rx) = mpsc::channel();
        let ctx = ctx.clone();
        // Copying and hashing happen off the UI thread. The game replaces its saves in one
        // step (temp file, then rename), so a copy never sees half a save.
        std::thread::spawn(move || {
            let label = format!("While playing \u{b7} {}", chrono::Local::now().format("%H:%M"));
            let _ = tx.send(saves::auto_checkpoint(&dir, &label, keep));
            ctx.request_repaint();
        });
        self.timed_checkpoint = Some(rx);
    }

    fn timed_checkpoint_done(&mut self, result: CheckpointResult) {
        match result {
            Ok(Some(_)) => self.saves_ui.refresh = true,
            Ok(None) => {}
            // The game saved in the middle of the copy: nothing was kept, try again next time.
            Err(e) if e.contains("changed while it was being copied") => {}
            Err(e) => {
                // Say so once per run, not every few minutes.
                let first = self.run.as_mut().is_none_or(|r| !std::mem::replace(&mut r.checkpoint_error_shown, true));
                if first {
                    self.err(format!("The checkpoint while playing failed: {e}"));
                }
            }
        }
    }
}

// ------------------------------------------------------------------ keyboard

/// Takes Ctrl+`key` out of this frame's input, so nothing else reacts to it. Shift must
/// match exactly and Alt must be up: AltGr is Ctrl+Alt, and typing `{` or `@` on many
/// keyboard layouts must not trigger a shortcut.
pub(crate) fn take_shortcut(ctx: &egui::Context, key: egui::Key, shift: bool, repeat_ok: bool) -> bool {
    ctx.input_mut(|i| {
        let mut hit = false;
        i.events.retain(|e| match e {
            egui::Event::Key { key: k, pressed: true, repeat, modifiers: m, .. }
                if *k == key && m.command && !m.alt && m.shift == shift =>
            {
                hit |= repeat_ok || !*repeat;
                false
            }
            _ => true,
        });
        hit
    })
}

impl App {
    /// Global shortcuts, for the main window (`window` = None) or a page window.
    fn shortcuts(&mut self, ctx: &egui::Context, window: Option<Page>) {
        use egui::Key;
        let viewport = ctx.viewport_id();
        if self.palette.is_open() {
            // The palette has the keyboard.
            if self.palette.is_open_in(viewport) {
                self.palette.take_keys(ctx);
            }
            return;
        }
        // A dialog is open: leave everything underneath it alone.
        if ctx.memory(|m| m.top_modal_layer().is_some()) {
            return;
        }
        if take_shortcut(ctx, Key::K, false, false) {
            self.palette.open(viewport);
            return;
        }
        // Ctrl+Shift+Tab / Ctrl+Tab: previous / next tab.
        for (shift, forward) in [(true, false), (false, true)] {
            if take_shortcut(ctx, Key::Tab, shift, true) && window.is_none() {
                self.layout.cycle(forward);
            }
        }
        if take_shortcut(ctx, Key::W, false, false) {
            match window {
                // Closing a page window brings the page back as a tab.
                Some(page) => {
                    self.layout.dock(page);
                    self.focus_main = true;
                }
                None => self.close_tab(self.layout.focused_page()),
            }
        }
        let digits = [Key::Num1, Key::Num2, Key::Num3, Key::Num4, Key::Num5, Key::Num6, Key::Num7, Key::Num8];
        for (key, page) in digits.into_iter().zip(Page::ALL) {
            if take_shortcut(ctx, key, false, false) {
                self.navigate(page);
            }
        }
        // Ctrl+Enter belongs to the text box while one is being edited (a new line in notes).
        if !ctx.wants_keyboard_input() && take_shortcut(ctx, Key::Enter, false, false) {
            self.launch_or_explain(ctx);
        }
        self.sync_page();
    }
}

// ------------------------------------------------------------------ sidebar

impl App {
    fn sidebar(&mut self, ctx: &egui::Context) {
        egui::SidePanel::left("nav")
            .exact_width(232.0)
            .resizable(false)
            .frame(egui::Frame::new().fill(theme::SIDEBAR).inner_margin(Margin::symmetric(12, 16)))
            .show(ctx, |ui| {
                ui.spacing_mut().item_spacing.y = 4.0;
                ui.horizontal(|ui| {
                    let (rect, _) = ui.allocate_exact_size(vec2(34.0, 34.0), Sense::hover());
                    match &self.logo {
                        Some(tex) => {
                            let uv = Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0));
                            ui.painter().image(tex.id(), rect, uv, Color32::WHITE);
                        }
                        None => {
                            ui.painter().rect_filled(rect, CornerRadius::same(9), theme::ACCENT);
                            ui.painter().text(
                                rect.center(),
                                Align2::CENTER_CENTER,
                                icon::GLOBE,
                                FontId::new(18.0, FontFamily::Proportional),
                                Color32::WHITE,
                            );
                        }
                    }
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 0.0;
                        ui.label(RichText::new("Command Center").family(theme::semibold()).size(15.0));
                        ui.label(theme::faint("Empire at War · Vulkan port"));
                    });
                });
                ui.add_space(12.0);
                self.search_button(ui);
                ui.add_space(10.0);

                let visible = self.layout.visible();
                for (page, glyph, label) in NAV {
                    let place = self.layout.find(page);
                    let selected = self.page == page;
                    let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), 38.0), Sense::click());
                    let hovered = resp.hovered();
                    if selected {
                        ui.painter().rect_filled(rect, CornerRadius::same(9), theme::ACCENT_DIM);
                        let bar = Rect::from_min_size(rect.left_center() - vec2(0.0, 9.0), vec2(3.0, 18.0));
                        ui.painter().rect_filled(bar, CornerRadius::same(2), theme::ACCENT);
                    } else if hovered {
                        ui.painter().rect_filled(rect, CornerRadius::same(9), HOVER_BG);
                    }
                    let color = if selected {
                        Color32::WHITE
                    } else if hovered {
                        theme::TEXT
                    } else {
                        theme::MUTED
                    };
                    ui.painter().text(
                        rect.left_center() + vec2(16.0, 0.0),
                        Align2::LEFT_CENTER,
                        glyph,
                        FontId::new(15.0, FontFamily::Proportional),
                        if selected { theme::ACCENT_HI } else { color },
                    );
                    let family = if selected { theme::semibold() } else { FontFamily::Proportional };
                    ui.painter().text(
                        rect.left_center() + vec2(44.0, -1.0),
                        Align2::LEFT_CENTER,
                        label,
                        FontId::new(14.0, family),
                        color,
                    );

                    // Right edge: where the page is open, if it is.
                    let mark = rect.right_center() - vec2(14.0, 0.0);
                    match place {
                        Some(Place::Window) => {
                            ui.painter().text(
                                mark,
                                Align2::CENTER_CENTER,
                                icon::POP_OUT,
                                FontId::new(11.0, FontFamily::Proportional),
                                theme::ACCENT_HI,
                            );
                        }
                        // On screen in the other pane: a brighter dot than a tab in the background.
                        Some(Place::Tab(..)) if !selected && visible.contains(&page) => {
                            ui.painter().circle_filled(mark, 3.0, theme::ACCENT_HI);
                        }
                        Some(Place::Tab(..)) if !selected => {
                            ui.painter().circle_filled(mark, 2.5, theme::FAINT);
                        }
                        _ => {}
                    }
                    if page == Page::Console && self.runner.is_running() {
                        ui.painter().circle_filled(rect.right_center() - vec2(32.0, 0.0), 4.0, theme::GOOD);
                    }

                    let in_window = place == Some(Place::Window);
                    // The main window's only tab can't go to the side of itself.
                    let can_side = !(matches!(place, Some(Place::Tab(..))) && self.layout.main_tabs() == 1);
                    #[derive(Clone, Copy)]
                    enum Choice {
                        Open,
                        Background,
                        Side,
                        Window,
                        Dock,
                    }
                    let ctrl = ui.input(|i| i.modifiers.command);
                    let mut choice = if resp.middle_clicked() || (resp.clicked() && ctrl) {
                        Some(Choice::Background)
                    } else if resp.clicked() {
                        Some(Choice::Open)
                    } else {
                        None
                    };
                    resp.context_menu(|ui| {
                        let mut item = |ui: &mut egui::Ui, enabled: bool, label: &str, pick: Choice| {
                            if ui.add_enabled(enabled, egui::Button::new(label)).clicked() {
                                choice = Some(pick);
                                ui.close_menu();
                            }
                        };
                        item(ui, true, "Open", Choice::Open);
                        item(ui, place.is_none(), "Open in new tab", Choice::Background);
                        item(ui, can_side, "Open to the side", Choice::Side);
                        if in_window {
                            item(ui, true, "Bring back into this window", Choice::Dock);
                        } else {
                            item(ui, true, "Open in new window", Choice::Window);
                        }
                    });
                    match choice {
                        Some(Choice::Open) => self.navigate(page),
                        Some(Choice::Background) => self.open_in_background(page),
                        Some(Choice::Side) => self.open_to_side(page),
                        Some(Choice::Window) => self.open_in_window(page),
                        Some(Choice::Dock) => self.close_tab(page),
                        None => {}
                    }
                }

                // Footer: active profile and game status.
                ui.with_layout(Layout::bottom_up(Align::Min), |ui| {
                    let running = self.runner.is_running();
                    egui::Frame::new()
                        .fill(theme::CARD)
                        .stroke(Stroke::new(1.0_f32, theme::BORDER))
                        .corner_radius(CornerRadius::same(10))
                        .inner_margin(Margin::same(12))
                        .show(ui, |ui| {
                            ui.set_width(ui.available_width());
                            // Fixed height: a top-down child of a bottom-up layout would
                            // otherwise claim the whole sidebar.
                            let size = vec2(ui.available_width(), 96.0);
                            ui.allocate_ui_with_layout(size, Layout::top_down(Align::Min), |ui| {
                                ui.spacing_mut().item_spacing.y = 2.0;
                                theme::overline(ui, "Active profile");
                                ui.add(egui::Label::new(theme::strong(self.cfg.active().name.clone())).truncate());
                                let n = self.cfg.active().mods.len();
                                let line = match n {
                                    0 => "No mods".to_owned(),
                                    1 => "1 mod".to_owned(),
                                    n => format!("{n} mods"),
                                };
                                ui.label(theme::faint(line));
                                ui.add_space(6.0);
                                ui.horizontal(|ui| {
                                    let (r, _) = ui.allocate_exact_size(vec2(8.0, 8.0), Sense::hover());
                                    let c = if running { theme::GOOD } else { theme::FAINT };
                                    ui.painter().circle_filled(r.center(), 4.0, c);
                                    let t = if running {
                                        format!("Running · {}", clock(self.runner.elapsed()))
                                    } else {
                                        "Game not running".to_owned()
                                    };
                                    ui.label(RichText::new(t).size(12.0).color(if running { theme::GOOD } else { theme::MUTED }));
                                });
                            });
                        });
                });
            });
    }

    /// The "Search … Ctrl K" field at the top of the sidebar; it opens the palette.
    fn search_button(&mut self, ui: &mut egui::Ui) {
        let (rect, resp) = ui.allocate_exact_size(vec2(ui.available_width(), 32.0), Sense::click());
        let hovered = resp.hovered();
        let p = ui.painter();
        p.rect_filled(rect, CornerRadius::same(8), theme::FIELD);
        let border = if hovered { theme::BORDER_HI } else { theme::BORDER };
        p.rect_stroke(rect, CornerRadius::same(8), Stroke::new(1.0_f32, border), StrokeKind::Inside);
        p.text(
            rect.left_center() + vec2(12.0, 0.0),
            Align2::LEFT_CENTER,
            icon::SEARCH,
            FontId::new(12.0, FontFamily::Proportional),
            theme::FAINT,
        );
        p.text(
            rect.left_center() + vec2(32.0, -1.0),
            Align2::LEFT_CENTER,
            "Search",
            FontId::new(13.0, FontFamily::Proportional),
            if hovered { theme::MUTED } else { theme::FAINT },
        );
        let key = Rect::from_min_max(pos2(rect.right() - 54.0, rect.top() + 6.0), pos2(rect.right() - 7.0, rect.bottom() - 6.0));
        p.rect_filled(key, CornerRadius::same(5), theme::CARD_HI);
        p.text(key.center() - vec2(0.0, 1.0), Align2::CENTER_CENTER, "Ctrl K", FontId::new(11.0, theme::semibold()), theme::MUTED);
        if resp.on_hover_text("Search commands, profiles, mods, saves and units").clicked() {
            self.palette.open(ui.ctx().viewport_id());
        }
    }

    fn draw_toasts(&mut self, ctx: &egui::Context) {
        let now = Instant::now();
        self.toasts.retain(|t| t.until > now);
        if self.toasts.is_empty() {
            return;
        }
        ctx.request_repaint_after(Duration::from_millis(250));
        let mut dismiss = None;
        egui::Area::new(Id::new("toasts"))
            .anchor(Align2::RIGHT_BOTTOM, vec2(-20.0, -20.0))
            .order(egui::Order::Foreground)
            .show(ctx, |ui| {
                ui.with_layout(Layout::bottom_up(Align::Max), |ui| {
                    for (i, t) in self.toasts.iter().enumerate().rev() {
                        let (accent, glyph) = if t.error { (theme::BAD, icon::WARNING) } else { (theme::GOOD, icon::CHECK) };
                        let resp = egui::Frame::new()
                            .fill(Color32::from_rgb(26, 31, 42))
                            .stroke(Stroke::new(1.0_f32, accent.gamma_multiply(0.6)))
                            .corner_radius(CornerRadius::same(10))
                            .inner_margin(Margin::symmetric(14, 10))
                            .shadow(egui::Shadow { offset: [0, 6], blur: 18, spread: 0, color: Color32::from_black_alpha(120) })
                            .show(ui, |ui| {
                                ui.set_max_width(440.0);
                                ui.horizontal(|ui| {
                                    ui.label(RichText::new(glyph).color(accent));
                                    ui.add(egui::Label::new(theme::text(t.text.clone())).wrap());
                                });
                            })
                            .response;
                        if resp.interact(Sense::click()).clicked() {
                            dismiss = Some(i);
                        }
                    }
                });
            });
        if let Some(i) = dismiss {
            self.toasts.remove(i);
        }
    }
}

// ------------------------------------------------------------------ tabs and panes

/// Splits the content area into the left pane, the divider and the right pane. Both panes
/// keep a minimum width as long as the window is wide enough for that.
fn split_rects(content: Rect, ratio: f32) -> (Rect, Rect, Rect) {
    // A minimised window can be narrower than the divider: shrink the divider so the right
    // pane never gets a negative width.
    let divider_w = DIVIDER_W.min(content.width().max(0.0));
    let width = (content.width() - divider_w).max(0.0);
    let min = MIN_PANE.min(width / 2.0);
    let left_w = (width * ratio).min(width - min).max(min);
    let x = content.left() + left_w;
    (
        Rect::from_min_max(content.min, pos2(x, content.bottom())),
        Rect::from_min_max(pos2(x, content.top()), pos2(x + divider_w, content.bottom())),
        Rect::from_min_max(pos2(x + divider_w, content.top()), content.max),
    )
}

/// Two panes side by side (or one, for "close split"), drawn by hand: the icon font has no
/// glyph both Windows 10 and 11 agree on.
fn paint_split_icon(painter: &egui::Painter, center: egui::Pos2, color: Color32, split: bool) {
    let r = Rect::from_center_size(center, vec2(15.0, 12.0));
    painter.rect_stroke(r, CornerRadius::same(2), Stroke::new(1.2_f32, color), StrokeKind::Inside);
    if split {
        // Already split: show the right half filled in, as the thing that goes away.
        let right = Rect::from_min_max(pos2(r.center().x, r.top() + 2.0), pos2(r.right() - 2.0, r.bottom() - 2.0));
        painter.rect_filled(right, 0.0, color.gamma_multiply(0.55));
    }
    painter.vline(r.center().x, (r.top() + 1.0)..=(r.bottom() - 1.0), Stroke::new(1.2_f32, color));
}

impl App {
    /// The content area of the main window: one pane, or two with a divider.
    fn main_area(&mut self, ctx: &egui::Context) {
        egui::CentralPanel::default().frame(egui::Frame::new().fill(theme::BG)).show(ctx, |ui| {
            let content = ui.max_rect();
            self.content_rect = content;
            self.strips = [None, None];
            // A minimised window has no room for anything.
            if content.width() < 160.0 || content.height() < 120.0 {
                return;
            }
            let mut ops = Vec::new();
            if self.layout.is_split() {
                let (left, divider, right) = split_rects(content, self.layout.ratio());
                self.pane(ui, Side::Left, left, true, &mut ops);
                self.pane(ui, Side::Right, right, true, &mut ops);
                self.divider(ui, divider, content, &mut ops);
            } else {
                // One page open: no strip, the plain single-page look.
                let strip = self.layout.main_tabs() > 1;
                self.pane(ui, Side::Left, content, strip, &mut ops);
            }
            self.tab_drag(ui, &mut ops);
            for op in ops {
                self.apply(op);
            }
        });
    }

    fn pane(&mut self, ui: &mut egui::Ui, side: Side, rect: Rect, strip: bool, ops: &mut Vec<Op>) {
        let Some(page) = self.layout.pane(side).map(|p| p.active_page()) else { return };
        // A click anywhere in a pane gives it the focus.
        if self.layout.focus() != side {
            let pressed = ui.input(|i| if i.pointer.any_pressed() { i.pointer.interact_pos() } else { None });
            if let Some(pos) = pressed {
                let on_page = ui.ctx().layer_id_at(pos).is_none_or(|l| l.order == egui::Order::Background);
                if rect.contains(pos) && on_page {
                    ops.push(Op::Focus(side));
                }
            }
        }
        let mut body = rect;
        if strip {
            let strip_rect = Rect::from_min_size(rect.min, vec2(rect.width(), STRIP_H));
            self.tab_strip(ui, side, strip_rect, ops);
            body.min.y = strip_rect.bottom();
        }
        // The same margins the single view has, in every pane.
        let top = if strip { 16.0 } else { 22.0 };
        let inner = Rect::from_min_max(pos2(body.left() + 28.0, body.top() + top), pos2(body.right() - 28.0, body.bottom()));
        if inner.width() < 80.0 || inner.height() < 80.0 {
            return;
        }
        // The page gets its own id scope (the same wherever it is shown, so scroll positions
        // survive a move) and can't draw outside its pane.
        let mut child = ui.new_child(
            egui::UiBuilder::new().id_salt(("page", page)).max_rect(inner).layout(Layout::top_down(Align::Min)),
        );
        child.set_clip_rect(body.intersect(ui.clip_rect()));
        self.draw_page(page, &mut child, Some(side));
    }

    fn tab_strip(&mut self, ui: &egui::Ui, side: Side, rect: Rect, ops: &mut Vec<Op>) {
        let Some(pane) = self.layout.pane(side).cloned() else { return };
        let split = self.layout.is_split();
        let focused = self.layout.focus() == side;
        let main_tabs = self.layout.main_tabs();
        let painter = ui.painter().with_clip_rect(rect.intersect(ui.clip_rect()));
        painter.rect_filled(rect, 0.0, STRIP_BG);
        painter.hline(rect.x_range(), rect.bottom() - 0.5, Stroke::new(1.0_f32, theme::BORDER));

        // Split button at the right end.
        let button = Rect::from_center_size(pos2(rect.right() - 24.0, rect.center().y), vec2(30.0, 26.0));
        let split_resp = ui.interact(button, Id::new(("split-button", side)), Sense::click());
        if split_resp.hovered() {
            painter.rect_filled(button, CornerRadius::same(6), Color32::from_rgb(30, 36, 48));
        }
        let color = if split_resp.hovered() { Color32::WHITE } else { theme::MUTED };
        paint_split_icon(&painter, button.center(), color, split);
        let tip = if split { "Close the split view" } else { "Split view: show this tab on the right" };
        if split_resp.on_hover_text(tip).clicked() {
            ops.push(Op::ToggleSplit);
        }

        let tabs = pane.tabs();
        let gap = 2.0;
        let left = rect.left() + 8.0;
        let room = (button.left() - 8.0 - left - gap * tabs.len().saturating_sub(1) as f32).max(40.0);
        let share = room / tabs.len().max(1) as f32;
        let pressed = ui.input(|i| i.pointer.primary_pressed());
        let mut x = left;
        let mut geom = Vec::with_capacity(tabs.len());
        for (i, &page) in tabs.iter().enumerate() {
            let active = i == pane.active();
            let family = if active { theme::semibold() } else { FontFamily::Proportional };
            let galley = painter.layout_no_wrap(page.short().to_owned(), FontId::new(13.5, family), Color32::PLACEHOLDER);
            let width = (galley.size().x + 76.0).min(share).max(34.0);
            let tab = Rect::from_min_max(pos2(x, rect.top() + 5.0), pos2(x + width, rect.bottom()));
            x += width + gap;
            geom.push((page, tab));

            let id = Id::new(("tab", page));
            let resp = ui.interact(tab, id, Sense::click_and_drag());
            let pointer_over = ui.rect_contains_pointer(tab);
            let dragged = self.drag.as_ref().is_some_and(|d| d.page == page);
            let closable = main_tabs > 1 && width >= 72.0;
            let close_rect = Rect::from_center_size(pos2(tab.right() - 16.0, tab.center().y), vec2(20.0, 20.0));
            let close = (closable && (active || pointer_over)).then(|| ui.interact(close_rect, id.with("close"), Sense::click()));

            // ---- paint
            let top_round = CornerRadius { nw: 8, ne: 8, sw: 0, se: 0 };
            if active {
                painter.rect_filled(tab, top_round, theme::BG);
                let line = Rect::from_min_max(pos2(tab.left() + 10.0, tab.bottom() - 2.0), pos2(tab.right() - 10.0, tab.bottom()));
                // The accent marks the pane that has the focus; the other pane's is grey.
                painter.rect_filled(line, CornerRadius::same(1), if focused { theme::ACCENT } else { theme::BORDER_HI });
            } else if pointer_over || dragged {
                let r = Rect::from_min_max(tab.min, pos2(tab.right(), tab.bottom() - 3.0));
                painter.rect_filled(r, CornerRadius::same(7), Color32::from_rgb(18, 22, 30));
            }
            let text_color = match (active, focused, pointer_over) {
                (true, true, _) => Color32::WHITE,
                (true, false, _) | (false, _, true) => theme::TEXT,
                _ => theme::MUTED,
            };
            let icon_color = if active && focused { theme::ACCENT_HI } else { text_color };
            let cy = tab.center().y - 1.0;
            painter.text(pos2(tab.left() + 12.0, cy), Align2::LEFT_CENTER, page.glyph(), FontId::new(13.0, FontFamily::Proportional), icon_color);
            let label_right = if close.is_some() { close_rect.left() - 2.0 } else { tab.right() - 8.0 };
            let label = Rect::from_min_max(pos2(tab.left() + 34.0, tab.top()), pos2(label_right, tab.bottom()));
            let truncated = galley.size().x > label.width();
            if label.width() > 10.0 {
                let pos = pos2(label.left(), cy - galley.size().y / 2.0);
                painter.with_clip_rect(label.intersect(painter.clip_rect())).galley(pos, galley, text_color);
            }
            if let Some(close) = &close {
                if close.hovered() {
                    painter.rect_filled(close_rect, CornerRadius::same(5), Color32::from_rgb(44, 53, 72));
                }
                let c = if close.hovered() { Color32::WHITE } else { theme::MUTED };
                painter.text(close_rect.center(), Align2::CENTER_CENTER, icon::CLOSE, FontId::new(9.0, FontFamily::Proportional), c);
            }

            // ---- interaction
            // Like a browser: the tab is shown as soon as the button goes down on it.
            if (pressed && resp.is_pointer_button_down_on()) || resp.clicked() {
                ops.push(Op::Activate(side, i));
            }
            if resp.middle_clicked() || close.is_some_and(|c| c.on_hover_text("Close (Ctrl+W)").clicked()) {
                ops.push(Op::Close(page));
            }
            if resp.drag_started_by(egui::PointerButton::Primary) {
                let pos = ui.input(|i| i.pointer.latest_pos()).unwrap_or(tab.center());
                self.drag = Some(TabDrag { page, pos });
            }
            resp.context_menu(|ui| {
                if ui.add_enabled(main_tabs > 1, egui::Button::new("Close")).clicked() {
                    ops.push(Op::Close(page));
                    ui.close_menu();
                }
                if ui.add_enabled(tabs.len() > 1, egui::Button::new("Close other tabs")).clicked() {
                    ops.push(Op::CloseOthers(page));
                    ui.close_menu();
                }
                ui.separator();
                let label = if split { "Move to the other side" } else { "Open to the side" };
                if ui.add_enabled(main_tabs > 1, egui::Button::new(label)).clicked() {
                    ops.push(Op::OtherSide(page));
                    ui.close_menu();
                }
                if ui.button("Move to new window").clicked() {
                    ops.push(Op::Pop(page));
                    ui.close_menu();
                }
            });
            if truncated && self.drag.is_none() {
                resp.on_hover_text(page.label());
            }
        }
        self.strips[side as usize] = Some(StripGeom { rect, tabs: geom });
    }

    /// The bar between the two panes: drag to resize, double-click for half and half.
    fn divider(&mut self, ui: &egui::Ui, rect: Rect, content: Rect, ops: &mut Vec<Op>) {
        let resp = ui.interact(rect.expand2(vec2(2.0, 0.0)), Id::new("pane-divider"), Sense::click_and_drag());
        let hot = resp.hovered() || resp.dragged();
        let (width, color) = if hot { (2.0, theme::ACCENT) } else { (1.0_f32, theme::BORDER) };
        ui.painter().vline(rect.center().x, rect.y_range(), Stroke::new(width, color));
        if hot {
            ui.ctx().set_cursor_icon(egui::CursorIcon::ResizeHorizontal);
        }
        if resp.double_clicked() {
            ops.push(Op::Ratio(0.5));
        } else if resp.dragged() {
            if let Some(pos) = resp.interact_pointer_pos() {
                let width = (content.width() - DIVIDER_W).max(1.0);
                let min = MIN_PANE.min(width / 2.0);
                let left = (pos.x - DIVIDER_W / 2.0 - content.left()).min(width - min).max(min);
                ops.push(Op::Ratio(left / width));
            }
        }
    }

    /// Where the tab being dragged would go if it were dropped now.
    fn drop_target(&self, drag: &TabDrag) -> DropTarget {
        let pos = drag.pos;
        for side in [Side::Left, Side::Right] {
            if let Some(strip) = &self.strips[side as usize] {
                if strip.rect.expand2(vec2(0.0, 8.0)).contains(pos) {
                    let index = strip.tabs.iter().filter(|(p, r)| *p != drag.page && r.center().x < pos.x).count();
                    return DropTarget::Strip(side, index);
                }
            }
        }
        let content = self.content_rect;
        if !content.expand(16.0).contains(pos) {
            return DropTarget::Window;
        }
        if !content.contains(pos) {
            return DropTarget::Nowhere;
        }
        let here = match self.layout.find(drag.page) {
            Some(Place::Tab(side, _)) => Some(side),
            _ => None,
        };
        if self.layout.is_split() {
            // Onto the other pane: move there.
            let (left, _, _) = split_rects(content, self.layout.ratio());
            let side = if pos.x < left.right() { Side::Left } else { Side::Right };
            return if here == Some(side) { DropTarget::Nowhere } else { DropTarget::Pane(side) };
        }
        // Near an edge: split, with the tab on that side.
        let x = (pos.x - content.left()) / content.width().max(1.0);
        if x < 0.25 {
            DropTarget::Pane(Side::Left)
        } else if x > 0.75 {
            DropTarget::Pane(Side::Right)
        } else {
            DropTarget::Nowhere
        }
    }

    /// Follows a tab drag: live reordering inside its strip, drop zones for the other pane
    /// and the edges, and a new window when it is let go outside the content area.
    fn tab_drag(&mut self, ui: &egui::Ui, ops: &mut Vec<Op>) {
        let Some(mut drag) = self.drag.take() else { return };
        let ctx = ui.ctx();
        let (down, pos, cancel) = ctx.input(|i| {
            (i.pointer.primary_down(), i.pointer.latest_pos(), i.key_pressed(egui::Key::Escape) || !i.focused)
        });
        let Some(Place::Tab(home, _)) = self.layout.find(drag.page) else { return };
        if cancel {
            return;
        }
        if let Some(pos) = pos {
            drag.pos = pos;
        }
        let target = self.drop_target(&drag);
        if !down {
            // Dropped.
            match target {
                DropTarget::Strip(side, index) => ops.push(Op::Move(drag.page, side, index)),
                DropTarget::Pane(side) => ops.push(Op::Beside(drag.page, side)),
                DropTarget::Window => {
                    // Put the new window under the pointer.
                    if let Some(inner) = ctx.input(|i| i.viewport().inner_rect) {
                        self.window_pos.insert(drag.page, inner.min + drag.pos.to_vec2() - vec2(140.0, 24.0));
                    }
                    ops.push(Op::Pop(drag.page));
                }
                DropTarget::Nowhere => {}
            }
            return;
        }

        ctx.set_cursor_icon(egui::CursorIcon::Grabbing);
        ctx.request_repaint();
        let overlay = ctx.layer_painter(egui::LayerId::new(egui::Order::Foreground, Id::new("tab-drop")));
        let mut hint = None;
        match target {
            // In its own strip the tab simply follows the pointer.
            DropTarget::Strip(side, index) if side == home => ops.push(Op::Move(drag.page, side, index)),
            DropTarget::Strip(side, index) => {
                if let Some(strip) = &self.strips[side as usize] {
                    let x = match strip.tabs.get(index) {
                        Some((_, r)) => r.left() - 1.0,
                        None => strip.tabs.last().map_or(strip.rect.left() + 8.0, |(_, r)| r.right() + 1.0),
                    };
                    overlay.vline(x, (strip.rect.top() + 6.0)..=(strip.rect.bottom() - 2.0), Stroke::new(2.0_f32, theme::ACCENT_HI));
                }
            }
            DropTarget::Pane(side) => {
                let content = self.content_rect;
                let zone = if self.layout.is_split() {
                    let (left, _, right) = split_rects(content, self.layout.ratio());
                    if side == Side::Left { left } else { right }
                } else {
                    let mid = content.center().x;
                    match side {
                        Side::Left => Rect::from_min_max(content.min, pos2(mid, content.bottom())),
                        Side::Right => Rect::from_min_max(pos2(mid, content.top()), content.max),
                    }
                };
                let zone = Rect::from_min_max(pos2(zone.left(), zone.top() + STRIP_H), zone.max).shrink(8.0);
                overlay.rect_filled(zone, CornerRadius::same(10), theme::ACCENT.gamma_multiply(0.16));
                overlay.rect_stroke(zone, CornerRadius::same(10), Stroke::new(1.5_f32, theme::ACCENT.gamma_multiply(0.8)), StrokeKind::Inside);
                hint = Some(if self.layout.is_split() { "Move here" } else { "Open side by side" });
            }
            DropTarget::Window => hint = Some("Open in its own window"),
            DropTarget::Nowhere => {}
        }

        // A small copy of the tab rides along with the pointer.
        let page = drag.page;
        egui::Area::new(Id::new("tab-ghost"))
            .order(egui::Order::Tooltip)
            .fixed_pos(drag.pos + vec2(14.0, 16.0))
            .interactable(false)
            .show(ctx, |ui| {
                egui::Frame::new()
                    .fill(theme::CARD_HI)
                    .stroke(Stroke::new(1.0_f32, theme::BORDER_HI))
                    .corner_radius(CornerRadius::same(8))
                    .inner_margin(Margin::symmetric(10, 6))
                    .shadow(egui::Shadow { offset: [0, 6], blur: 16, spread: 0, color: Color32::from_black_alpha(120) })
                    .show(ui, |ui| {
                        ui.horizontal(|ui| {
                            ui.label(RichText::new(page.glyph()).color(theme::ACCENT_HI).size(13.0));
                            ui.label(theme::strong(page.short()));
                            if let Some(hint) = hint {
                                ui.label(theme::faint(format!("\u{b7}  {hint}")));
                            }
                        });
                    });
            });
        self.drag = Some(drag);
    }
}

// ------------------------------------------------------------------ pages and their windows

fn page_from_name(name: &str) -> Option<Page> {
    match name {
        // These open the Saves page on a particular tab (see `saves::State::start_tab`).
        "checkpoints" | "map" => Some(Page::Saves),
        other => Page::from_key(other),
    }
}

fn page_frame() -> egui::Frame {
    egui::Frame::new().fill(theme::BG).inner_margin(Margin { left: 28, right: 28, top: 22, bottom: 0 })
}

fn popout_id(page: Page) -> ViewportId {
    ViewportId::from_hash_of(("page-window", page))
}

pub(crate) const ICON_PNG: &[u8] = include_bytes!("../assets/icon.png");

/// The program icon, for window title bars and the taskbar.
pub fn app_icon() -> std::sync::Arc<egui::IconData> {
    let icon = eframe::icon_data::from_png_bytes(ICON_PNG).unwrap_or_default();
    std::sync::Arc::new(icon)
}

fn load_logo(ctx: &egui::Context) -> Option<egui::TextureHandle> {
    let img = image::load_from_memory(ICON_PNG).ok()?.resize(96, 96, image::imageops::FilterType::Lanczos3).to_rgba8();
    let size = [img.width() as usize, img.height() as usize];
    let color = egui::ColorImage::from_rgba_unmultiplied(size, img.as_raw());
    Some(ctx.load_texture("app-logo", color, egui::TextureOptions::LINEAR))
}

/// The main window's `HWND`, as a number.
fn main_window_handle(cc: &eframe::CreationContext<'_>) -> Option<isize> {
    use raw_window_handle::{HasWindowHandle, RawWindowHandle};
    match cc.window_handle().ok()?.as_raw() {
        RawWindowHandle::Win32(handle) => Some(handle.hwnd.get()),
        _ => None,
    }
}

impl App {
    fn show_page(&mut self, page: Page, ui: &mut egui::Ui) {
        match page {
            Page::Play => pages::play::show(self, ui),
            Page::Profiles => pages::profiles::show(self, ui),
            Page::Mods => pages::mods::show(self, ui),
            Page::Saves => pages::saves::show(self, ui),
            Page::Units => pages::units::show(self, ui),
            Page::Graphics => pages::graphics::show(self, ui),
            Page::Console => pages::console::show(self, ui),
            Page::Settings => pages::settings::show(self, ui),
        }
    }

    /// Draws a page, at most once per frame: pages keep one state each, and their widget
    /// ids would clash if a page were on screen twice. `pane` is the pane it is drawn in
    /// (`None` for a page window).
    fn draw_page(&mut self, page: Page, ui: &mut egui::Ui, pane: Option<Side>) {
        let bit = 1u8 << page.index();
        if self.drawn & bit != 0 {
            return;
        }
        self.drawn |= bit;
        self.drawing = Some(page);
        self.page = page;
        self.show_page(page, ui);
        self.drawing = None;
        if self.page != page {
            // The page assigned `app.page`: go there, opening new tabs next to this one.
            let target = self.page;
            if let Some(side) = pane {
                self.layout.set_focus(side);
            }
            self.navigate(target);
        }
        self.sync_page();
    }

    /// Draws every page that has a window of its own. They share all state with the main
    /// window, so a change in one shows up in the others straight away.
    fn show_popouts(&mut self, ctx: &egui::Context) {
        let pages: Vec<Page> = self.layout.windows().to_vec();
        self.window_start.retain(|p, _| pages.contains(p));
        for page in pages {
            // Position and size are decided once per window (see `window_start`).
            let remembered = (self.window_pos.get(&page).copied(), self.layout.window_size(page).unwrap_or([1120.0, 800.0]));
            let (pos, size) = *self.window_start.entry(page).or_insert(remembered);
            let mut builder = egui::ViewportBuilder::default()
                .with_title(format!("{} · Command Center", page.label()))
                .with_inner_size(size)
                .with_min_inner_size([860.0, 600.0])
                .with_icon(app_icon());
            if let Some(pos) = pos {
                builder = builder.with_position(pos);
            }
            let id = popout_id(page);
            let mut closed = false;
            ctx.show_viewport_immediate(id, builder, |ctx, _class| {
                self.in_window = true;
                self.shortcuts(ctx, Some(page));
                egui::CentralPanel::default().frame(page_frame()).show(ctx, |ui| {
                    if ui.available_width() < 80.0 || ui.available_height() < 80.0 {
                        return;
                    }
                    let mut child = ui.new_child(egui::UiBuilder::new().id_salt(("page", page)));
                    self.draw_page(page, &mut child, None);
                });
                if self.palette.is_open_in(id) {
                    crate::palette::show(self, ctx);
                }
                if self.toast_viewport == id {
                    self.draw_toasts(ctx);
                }
                // Remember where the window is and how big, for the next time it opens.
                let (outer, inner, minimized, close) = ctx.input(|i| {
                    let v = i.viewport();
                    (v.outer_rect, v.inner_rect, v.minimized == Some(true), v.close_requested())
                });
                if !minimized {
                    if let Some(outer) = outer {
                        self.window_pos.insert(page, outer.min);
                    }
                    if let Some(inner) = inner {
                        self.layout.set_window_size(page, [inner.width(), inner.height()]);
                    }
                }
                closed = close;
                self.in_window = false;
            });
            if closed {
                // Closing the window brings the page back as a tab; nothing is lost.
                self.layout.dock(page);
                self.sync_page();
            }
        }
        if let Some(page) = self.focus_window.take() {
            if self.layout.windows().contains(&page) {
                let id = popout_id(page);
                if ctx.input_for(id, |i| i.viewport().minimized) == Some(true) {
                    ctx.send_viewport_cmd_to(id, ViewportCommand::Minimized(false));
                }
                ctx.send_viewport_cmd_to(id, ViewportCommand::Focus);
            }
        }
    }

    /// End of the frame: settle `page`, remember the layout, keep the window wide enough.
    fn finish_frame(&mut self, ctx: &egui::Context) {
        // Something outside a page's own drawing assigned `app.page`.
        if self.page != self.synced_page {
            let target = self.page;
            self.navigate(target);
        }
        self.sync_page();

        if std::mem::take(&mut self.focus_main) {
            if ctx.input(|i| i.viewport().minimized) == Some(true) {
                ctx.send_viewport_cmd_to(ViewportId::ROOT, ViewportCommand::Minimized(false));
            }
            ctx.send_viewport_cmd_to(ViewportId::ROOT, ViewportCommand::Focus);
        }

        if !self.layout_from_env && self.cfg.layout != self.layout {
            self.cfg.layout = self.layout.clone();
            self.touch();
        }

        // Two pages side by side need a wider window than one.
        let split = self.layout.is_split();
        if self.min_size_split != Some(split) {
            self.min_size_split = Some(split);
            let min = if split { MIN_WINDOW_SPLIT } else { MIN_WINDOW };
            ctx.send_viewport_cmd_to(ViewportId::ROOT, ViewportCommand::MinInnerSize(min.into()));
            let (inner, busy) = ctx.input(|i| {
                let v = i.viewport();
                (v.inner_rect, v.minimized == Some(true) || v.maximized == Some(true) || v.fullscreen == Some(true))
            });
            if let (Some(inner), false) = (inner, busy) {
                if split && inner.width() < min[0] {
                    ctx.send_viewport_cmd_to(ViewportId::ROOT, ViewportCommand::InnerSize(vec2(min[0], inner.height().max(min[1]))));
                }
            }
        }
    }
}

pub fn clock(d: Duration) -> String {
    let s = d.as_secs();
    if s >= 3600 {
        format!("{}:{:02}:{:02}", s / 3600, (s % 3600) / 60, s % 60)
    } else {
        format!("{}:{:02}", s / 60, s % 60)
    }
}

pub fn open_in_explorer(path: &Path) {
    let _ = std::process::Command::new("explorer").arg(path).spawn();
}

pub fn reveal_in_explorer(path: &Path) {
    let _ = std::process::Command::new("explorer").arg(format!("/select,{}", path.display())).spawn();
}

impl eframe::App for App {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        self.tex.begin_frame();
        self.drawn = 0;
        // Something assigned `app.page` between frames.
        if self.page != self.synced_page {
            let target = self.page;
            self.navigate(target);
        }

        self.game_tick(ctx);
        pages::mods::background(self, ctx);
        if std::mem::take(&mut self.autolaunch) {
            self.launch(ctx);
        }

        // Messages show in whichever of our windows the user is looking at.
        self.toast_viewport = self
            .layout
            .windows()
            .iter()
            .map(|p| popout_id(*p))
            .find(|id| ctx.input_for(*id, |i| i.viewport().focused) == Some(true))
            .unwrap_or(ViewportId::ROOT);

        self.shortcuts(ctx, None);
        self.sidebar(ctx);
        self.main_area(ctx);
        self.show_popouts(ctx);
        if self.palette.is_open_in(ViewportId::ROOT) {
            crate::palette::show(self, ctx);
        }
        if self.toast_viewport == ViewportId::ROOT {
            self.draw_toasts(ctx);
        }
        self.finish_frame(ctx);

        if let Some(since) = self.dirty_since {
            if since.elapsed() > Duration::from_millis(600) {
                self.dirty_since = None;
                if let Err(e) = self.cfg.save() {
                    self.err(e);
                }
            } else {
                ctx.request_repaint_after(Duration::from_millis(300));
            }
        }
    }

    fn on_exit(&mut self) {
        if !self.layout_from_env {
            self.cfg.layout = self.layout.clone();
        }
        let _ = self.cfg.save();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout_env_variables_build_the_screenshot_layout() {
        assert!(layout_from_env(None, None, None).is_none());

        // COMMAND_CENTER_PAGE alone: that page, nothing else.
        let l = layout_from_env(Some("mods"), None, None).unwrap();
        assert_eq!(l, layout::Layout::single(Page::Mods));
        // The Saves page's tab names are pages too.
        assert_eq!(layout_from_env(Some("map"), None, None).unwrap().focused_page(), Page::Saves);
        assert_eq!(layout_from_env(Some("nonsense"), None, None).unwrap().focused_page(), Page::Play);

        // COMMAND_CENTER_WINDOWS: own windows; the main page stays in the main window.
        let l = layout_from_env(Some("play"), Some("saves, units,play,bogus"), None).unwrap();
        assert_eq!(l.focused_page(), Page::Play);
        assert_eq!(l.windows(), [Page::Saves, Page::Units]);
        assert_eq!(l.main_tabs(), 1);
        let l = layout_from_env(None, Some("console"), None).unwrap();
        assert_eq!((l.focused_page(), l.windows()), (Page::Play, &[Page::Console][..]));

        // COMMAND_CENTER_TABS: extra tabs, and a second pane after the bar.
        let l = layout_from_env(Some("mods"), None, Some("play,saves|units,console")).unwrap();
        assert_eq!(l.pane(Side::Left).unwrap().tabs(), [Page::Mods, Page::Play, Page::Saves]);
        assert_eq!(l.pane(Side::Right).unwrap().tabs(), [Page::Units, Page::Console]);
        assert_eq!(l.focused_page(), Page::Mods);
        assert_eq!(l.visible(), [Page::Mods, Page::Units]);
        let l = layout_from_env(None, None, Some("mods")).unwrap();
        assert_eq!(l.pane(Side::Left).unwrap().tabs(), [Page::Play, Page::Mods]);
        assert!(!l.is_split());
    }

    #[test]
    fn layout_pages_have_stable_names() {
        for (i, page) in Page::ALL.into_iter().enumerate() {
            assert_eq!(page.index(), i);
            assert_eq!(NAV[i].0, page);
            assert_eq!(Page::from_key(page.key()), Some(page));
            assert!(!page.label().is_empty() && !page.short().is_empty());
        }
        assert_eq!(Page::from_key("Play"), None);
        assert_eq!(page_from_name("checkpoints"), Some(Page::Saves));
    }

    #[test]
    fn layout_split_keeps_both_panes_usable() {
        let content = Rect::from_min_size(pos2(232.0, 0.0), vec2(1048.0, 700.0));
        for ratio in [0.0, 0.2, 0.5, 0.8, 1.0] {
            let (left, divider, right) = split_rects(content, ratio);
            assert!(left.width() >= MIN_PANE && right.width() >= MIN_PANE, "ratio {ratio}");
            assert_eq!(left.left(), content.left());
            assert_eq!(right.right(), content.right());
            assert_eq!(divider.width(), DIVIDER_W);
            assert_eq!((left.right(), divider.right()), (divider.left(), right.left()));
        }
        let (left, _, right) = split_rects(content, 0.5);
        assert!((left.width() - right.width()).abs() < 1.0);
        // Too narrow for two minimum-width panes (or minimised): halves, and no panic.
        for width in [500.0, 7.0, 0.0] {
            let (left, _, right) = split_rects(Rect::from_min_size(pos2(0.0, 0.0), vec2(width, 100.0)), 0.9);
            assert!(left.width() >= 0.0 && right.width() >= -0.001);
            assert!((left.width() - right.width()).abs() < 1.0);
        }
    }
}

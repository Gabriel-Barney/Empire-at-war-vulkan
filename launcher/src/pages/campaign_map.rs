//! The campaign map tab of the saves page: a galactic save drawn as a galaxy, on its own or
//! compared with another save of the same campaign.

use crate::app::App;
use crate::galaxy::{self, pretty, Galaxy, GalaxyDiff, Planet, Stack, Tally};
use crate::saves::SaveInfo;
use crate::theme::{self, icon};
use egui::{
    pos2, vec2, Align, Align2, Color32, CornerRadius, FontFamily, FontId, Layout, Pos2, Rect, RichText,
    Sense, Shape, Stroke, StrokeKind, Ui, Vec2,
};
use std::collections::HashMap;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{self, Receiver, TryRecvError};

const SIDE_W: f32 = 300.0;
/// The side panel is wider while comparing, for the table of changes.
const SIDE_W_COMPARE: f32 = 348.0;
const MIN_ZOOM: f32 = 0.3;
const MAX_ZOOM: f32 = 12.0;
const NEUTRAL: Color32 = Color32::from_rgb(128, 136, 150);
const UNSEEN: Color32 = Color32::from_rgb(62, 69, 84);
const PALETTE: [Color32; 8] = [
    Color32::from_rgb(82, 142, 255),
    Color32::from_rgb(240, 98, 98),
    Color32::from_rgb(62, 201, 142),
    Color32::from_rgb(245, 184, 61),
    Color32::from_rgb(178, 132, 255),
    Color32::from_rgb(64, 200, 220),
    Color32::from_rgb(240, 140, 80),
    Color32::from_rgb(220, 120, 190),
];

#[derive(Clone, Copy)]
struct View {
    /// Multiplier on top of the fit-everything scale.
    zoom: f32,
    /// Screen-space offset of the galaxy's centre from the canvas centre.
    pan: Vec2,
}

impl Default for View {
    fn default() -> Self {
        Self { zoom: 1.0, pan: Vec2::ZERO }
    }
}

/// A galaxy plus everything worked out once when it loads.
struct Loaded {
    g: Galaxy,
    names: Vec<String>,
    /// False for rows that aren't planets (map decoration); those are never shown.
    real: Vec<bool>,
    world: Vec<Pos2>,
    /// Screen positions, refilled each frame (kept here to reuse the allocation).
    screen: Vec<Pos2>,
    bounds: Rect,
    by_index: HashMap<i32, usize>,
    routes: Vec<(usize, usize)>,
    colors: HashMap<i32, Color32>,
    /// Per planet: positions in `g.fleets` of the fleets in orbit.
    orbit: Vec<Vec<usize>>,
    moving: Vec<usize>,
    fleet_units: Vec<u32>,
    fleet_labels: Vec<String>,
    /// Aligned with `g.players`.
    tallies: Vec<Tally>,
}

impl Loaded {
    fn new(g: Galaxy) -> Self {
        let by_index: HashMap<i32, usize> = g.planets.iter().enumerate().map(|(i, p)| (p.index, i)).collect();
        let world: Vec<Pos2> = g.planets.iter().map(|p| pos2(p.x, p.y)).collect();
        // An export without the flag at all is treated as all planets.
        let flagged = g.planets.iter().any(|p| p.is_planet);
        let real: Vec<bool> = g.planets.iter().map(|p| p.is_planet || !flagged).collect();
        let fitted: Vec<Pos2> = world.iter().zip(&real).filter(|(_, r)| **r).map(|(w, _)| *w).collect();
        let bounds =
            if fitted.is_empty() { Rect::from_center_size(Pos2::ZERO, Vec2::splat(1.0)) } else { Rect::from_points(&fitted) };
        let routes = g
            .routes
            .iter()
            .filter_map(|r| Some((*by_index.get(&r.a)?, *by_index.get(&r.b)?)))
            .filter(|&(a, b)| real[a] && real[b])
            .collect();
        let colors = g
            .players
            .iter()
            .map(|p| {
                let c = if p.neutral {
                    NEUTRAL
                } else {
                    p.color
                        .as_deref()
                        .and_then(parse_hex)
                        .unwrap_or(PALETTE[p.index.rem_euclid(PALETTE.len() as i32) as usize])
                };
                (p.index, c)
            })
            .collect();
        let mut orbit = vec![Vec::new(); g.planets.len()];
        let mut moving = Vec::new();
        for (i, f) in g.fleets.iter().enumerate() {
            let at = if f.moving || f.planet < 0 { None } else { by_index.get(&f.planet) };
            match at {
                Some(&p) => orbit[p].push(i),
                None => moving.push(i),
            }
        }
        let fleet_units: Vec<u32> = g.fleets.iter().map(|f| f.units.iter().map(|u| u.count).sum()).collect();
        Self {
            names: g.planets.iter().map(|p| pretty(&p.name)).collect(),
            screen: Vec::with_capacity(world.len()),
            real,
            world,
            bounds,
            by_index,
            routes,
            colors,
            orbit,
            moving,
            fleet_labels: fleet_units.iter().map(|n| n.to_string()).collect(),
            fleet_units,
            tallies: g.players.iter().map(|p| g.tally(p.index)).collect(),
            g,
        }
    }

    fn color(&self, player: i32) -> Color32 {
        self.colors.get(&player).copied().unwrap_or(NEUTRAL)
    }

    /// The colour a faction is drawn in here, if this save has the faction.
    fn faction_color(&self, faction: &str) -> Option<Color32> {
        self.g.players.iter().find(|p| p.faction == faction).map(|p| self.color(p.index))
    }
}

/// The difference between the shown save (A) and the other one (B), ready for drawing.
struct DiffView {
    /// The fog setting the diff was worked out for.
    show_all: bool,
    diff: GalaxyDiff,
    /// For each of A's planets: where the planet of the same name sits in B.
    b_of: Vec<Option<usize>>,
    /// A's planet -> position in `diff.owner_changes` / `diff.stations`.
    changed: HashMap<usize, usize>,
    stations: HashMap<usize, usize>,
}

impl DiffView {
    fn new(a: &Loaded, b: &Loaded, show_all: bool) -> Self {
        let diff = galaxy::diff(&a.g, &b.g, show_all);
        let mut by_name: HashMap<&str, usize> = HashMap::new();
        for (i, p) in b.g.planets.iter().enumerate() {
            if b.real[i] {
                by_name.entry(p.name.as_str()).or_insert(i);
            }
        }
        let b_of = a
            .g
            .planets
            .iter()
            .enumerate()
            .map(|(i, p)| by_name.get(p.name.as_str()).copied().filter(|_| a.real[i]))
            .collect();
        Self {
            show_all,
            changed: diff.owner_changes.iter().enumerate().map(|(n, c)| (c.a, n)).collect(),
            stations: diff.stations.iter().enumerate().map(|(n, s)| (s.a, n)).collect(),
            b_of,
            diff,
        }
    }
}

/// The save the shown one is compared with ("B").
struct Compare {
    path: PathBuf,
    other: Option<Loaded>,
    error: Option<String>,
    view: Option<DiffView>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum Target {
    Main,
    Other,
}

/// One run of the game's exporter. They run one at a time.
struct Job {
    target: Target,
    /// The request number it was started for; a result for an older request is dropped.
    serial: u64,
    rx: Receiver<Result<Galaxy, String>>,
}

#[derive(Default)]
pub(super) struct MapState {
    save: Option<PathBuf>,
    /// Bumped for every new request, of either save.
    serial: u64,
    main_serial: u64,
    other_serial: u64,
    job: Option<Job>,
    error: Option<String>,
    loaded: Option<Loaded>,
    compare: Option<Compare>,
    show_all: bool,
    view: View,
    selected: Option<usize>,
    /// A planet to bring to the middle of the canvas on the next frame.
    center_on: Option<usize>,
}

impl MapState {
    /// Forgets the loaded map (the save folder changed under it).
    pub(super) fn clear(&mut self) {
        // A running export is left to finish; its result is dropped in `pump`.
        self.save = None;
        self.error = None;
        self.loaded = None;
        self.compare = None;
        self.selected = None;
        self.center_on = None;
        self.serial += 1;
        self.main_serial = self.serial;
        self.other_serial = self.serial;
    }

    /// Forgets the map if it shows this save, or the comparison if it uses it.
    pub(super) fn forget(&mut self, path: &Path) {
        if self.save.as_deref() == Some(path) {
            self.clear();
        } else if self.compare.as_ref().map(|c| c.path.as_path()) == Some(path) {
            self.compare = None;
        }
    }

    /// Loads a save's galaxy. Exports run one at a time, so a request made while one is
    /// running waits for it. Asking for the save already shown reads it again and keeps
    /// the comparison; another save ends it.
    pub(super) fn request(&mut self, path: PathBuf) {
        if self.save.as_deref() != Some(path.as_path()) {
            self.compare = None;
            self.view = View::default();
        } else if let Some(c) = &mut self.compare {
            c.view = None;
        }
        self.save = Some(path);
        self.error = None;
        self.loaded = None;
        self.selected = None;
        self.center_on = None;
        self.serial += 1;
        self.main_serial = self.serial;
    }

    /// Starts comparing the shown save with another one.
    fn compare_with(&mut self, path: PathBuf) {
        self.compare = Some(Compare { path, other: None, error: None, view: None });
        self.serial += 1;
        self.other_serial = self.serial;
    }

    /// Makes B the shown save and the shown save B. Both are already loaded, so nothing
    /// is read again.
    fn swap(&mut self) {
        let (Some(a), Some(c)) = (self.loaded.as_mut(), self.compare.as_mut()) else { return };
        let (Some(b), Some(save)) = (c.other.as_mut(), self.save.as_mut()) else { return };
        let selected = self.selected.and_then(|i| a.g.planets.get(i)).map(|p| p.name.clone());
        std::mem::swap(a, b);
        std::mem::swap(save, &mut c.path);
        c.view = None;
        self.selected = selected.and_then(|name| a.g.planets.iter().position(|p| p.name == name));
        self.center_on = None;
    }

    fn start(&mut self, app: &App, ctx: &egui::Context, target: Target, path: PathBuf) -> Result<(), String> {
        let exe = app
            .cfg
            .settings
            .game_exe
            .clone()
            .ok_or("alamo_game.exe hasn't been chosen yet. Pick it in Settings, then try again.")?;
        let foc = crate::mods::foc_dir(&app.steam, &app.cfg.settings)
            .ok_or("Forces of Corruption wasn't found. Set its folder in Settings, then try again.")?;
        let args = crate::launch::data_args(app.cfg.active());
        let (tx, rx) = mpsc::channel();
        let ctx = ctx.clone();
        std::thread::spawn(move || {
            let _ = tx.send(galaxy::export(&exe, &foc, &args, &path));
            ctx.request_repaint();
        });
        let serial = match target {
            Target::Main => self.main_serial,
            Target::Other => self.other_serial,
        };
        self.job = Some(Job { target, serial, rx });
        Ok(())
    }

    /// Collects a finished export and starts the next one that is wanted: the shown save
    /// first, then the one it is compared with.
    fn pump(&mut self, app: &App, ctx: &egui::Context) {
        if let Some(job) = &self.job {
            let result = match job.rx.try_recv() {
                Ok(r) => Some(r),
                Err(TryRecvError::Empty) => None,
                Err(TryRecvError::Disconnected) => {
                    Some(Err("Reading the save stopped before it finished. Try again.".to_owned()))
                }
            };
            if let Some(result) = result {
                let (target, serial) = (job.target, job.serial);
                self.job = None;
                match target {
                    Target::Main if serial == self.main_serial && self.save.is_some() => match result {
                        Ok(g) => self.loaded = Some(Loaded::new(g)),
                        Err(e) => self.error = Some(e),
                    },
                    Target::Other if serial == self.other_serial => {
                        if let Some(c) = &mut self.compare {
                            match result {
                                Ok(g) => c.other = Some(Loaded::new(g)),
                                Err(e) => c.error = Some(e),
                            }
                        }
                    }
                    _ => {}
                }
            }
        }
        if self.job.is_none() {
            if let Some(path) = self.save.clone().filter(|_| self.loaded.is_none() && self.error.is_none()) {
                if let Err(e) = self.start(app, ctx, Target::Main, path) {
                    self.error = Some(e);
                }
            } else if self.loaded.is_some() {
                let wanted = self.compare.as_ref().filter(|c| c.other.is_none() && c.error.is_none()).map(|c| c.path.clone());
                if let Some(path) = wanted {
                    if let Err(e) = self.start(app, ctx, Target::Other, path) {
                        if let Some(c) = &mut self.compare {
                            c.error = Some(e);
                        }
                    }
                }
            }
        }

        // Work out the difference once both sides are in, and again when the fog setting flips.
        if let (Some(a), Some(c)) = (&self.loaded, &mut self.compare) {
            if let Some(b) = &c.other {
                if let Err(e) = galaxy::comparable(&a.g, &b.g) {
                    c.error = Some(e);
                    c.other = None;
                    c.view = None;
                } else if c.view.as_ref().map(|v| v.show_all != self.show_all).unwrap_or(true) {
                    c.view = Some(DiffView::new(a, b, self.show_all));
                }
            }
        }
    }

    fn loading(&self) -> bool {
        self.save.is_some() && self.loaded.is_none() && self.error.is_none()
    }
}

fn save_name(saves: &[SaveInfo], path: &Path) -> String {
    saves
        .iter()
        .find(|s| s.path == path)
        .map(|s| s.name.clone())
        .or_else(|| path.file_stem().map(|s| s.to_string_lossy().into_owned()))
        .unwrap_or_default()
}

pub(super) fn show(map: &mut MapState, app: &mut App, saves: &[SaveInfo], ui: &mut Ui) {
    let ctx = ui.ctx().clone();
    map.pump(app, &ctx);

    let campaign_saves = || saves.iter().filter(|s| s.error.is_none() && s.is_campaign());
    let mut pick: Option<PathBuf> = None;
    let mut pick_other: Option<PathBuf> = None;
    ui.horizontal(|ui| {
        let current = match &map.save {
            Some(p) => save_name(saves, p),
            None => "Choose a campaign save".to_owned(),
        };
        egui::ComboBox::from_id_salt("campaign_map_save").width(240.0).selected_text(current).show_ui(ui, |ui| {
            let mut any = false;
            for s in campaign_saves() {
                any = true;
                let on = map.save.as_deref() == Some(s.path.as_path());
                let label = format!("{}  ·  Week {}", s.name, s.week);
                if ui.selectable_label(on, label).clicked() {
                    pick = Some(s.path.clone());
                }
            }
            if !any {
                ui.label(theme::muted("No campaign saves in this folder."));
            }
        });
        if map.save.is_some() && theme::icon_button(ui, icon::REFRESH, "Read this save again").clicked() {
            pick = map.save.clone();
        }
        if let (Some(l), None) = (&map.loaded, &map.compare) {
            let meta = &l.g.save;
            let title = if meta.title.is_empty() { &meta.campaign } else { &meta.title };
            if !title.is_empty() {
                ui.label(theme::strong(pretty(title)));
            }
            ui.label(theme::muted(format!("Week {}", meta.week)));
        }

        // Only saves of the campaign being shown can be put next to it.
        if let (Some(l), Some(shown)) = (&map.loaded, &map.save) {
            let campaign = l.g.save.campaign.as_str();
            let text = match &map.compare {
                Some(c) => format!("Compared with {}", save_name(saves, &c.path)),
                None => "Compare with…".to_owned(),
            };
            egui::ComboBox::from_id_salt("campaign_map_compare").width(200.0).selected_text(text).show_ui(ui, |ui| {
                let (mut same, mut others) = (0, 0);
                for s in campaign_saves().filter(|s| &s.path != shown) {
                    if !s.campaign.eq_ignore_ascii_case(campaign) {
                        others += 1;
                        continue;
                    }
                    same += 1;
                    let on = map.compare.as_ref().map(|c| c.path == s.path).unwrap_or(false);
                    if ui.selectable_label(on, format!("{}  ·  Week {}", s.name, s.week)).clicked() {
                        pick_other = Some(s.path.clone());
                    }
                }
                if same == 0 {
                    ui.label(theme::muted("No other saves of this campaign."));
                }
                if others > 0 {
                    ui.label(theme::faint(if others == 1 {
                        "1 save is from a different campaign and can't be compared.".to_owned()
                    } else {
                        format!("{others} saves are from different campaigns and can't be compared.")
                    }));
                }
            });
        }

        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            theme::toggle(ui, &mut map.show_all);
            ui.label(theme::muted("Show everything"))
                .on_hover_text("Off: the map shows only what your faction has seen.");
        });
    });
    if let Some(p) = pick {
        map.request(p);
        map.pump(app, &ctx);
    }
    if let Some(p) = pick_other {
        map.compare_with(p);
        map.pump(app, &ctx);
    }
    compare_bar(map, saves, ui);
    ui.add_space(10.0);

    if map.loading() {
        theme::card(ui, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(64.0);
                ui.add(egui::Spinner::new().size(24.0));
                ui.add_space(8.0);
                ui.label(theme::muted("Asking the game to read this save…"));
                ui.add_space(64.0);
            });
        });
        return;
    }

    if let Some(e) = map.error.clone() {
        let mut retry = false;
        theme::card(ui, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(36.0);
                ui.label(RichText::new(icon::WARNING).size(34.0).color(theme::WARN));
                ui.add_space(4.0);
                ui.label(theme::title("The map couldn't be loaded"));
                ui.add(egui::Label::new(theme::muted(e)).wrap());
                ui.add_space(8.0);
                retry = theme::primary_button(ui, "Retry").clicked();
                ui.add_space(36.0);
            });
        });
        if retry {
            if let Some(p) = map.save.clone() {
                map.request(p);
            }
        }
        return;
    }

    let MapState { loaded, compare, view, selected, show_all, center_on, .. } = map;
    let show_all = *show_all;
    let Some(l) = loaded.as_mut() else {
        theme::card(ui, |ui| {
            if saves.iter().any(|s| s.error.is_none() && s.is_campaign()) {
                theme::empty_state(ui, icon::GLOBE, "No campaign loaded", "Choose a campaign save to see its galaxy.");
            } else {
                theme::empty_state(
                    ui,
                    icon::GLOBE,
                    "No campaign saves",
                    "Save a galactic campaign in the game and it will show up here.",
                );
            }
        });
        return;
    };
    if !l.real.iter().any(|r| *r) {
        theme::card(ui, |ui| {
            theme::empty_state(ui, icon::GLOBE, "Nothing to show", "This save has no planets in it.");
        });
        return;
    }
    // The comparison is drawn once B is loaded and the difference has been worked out.
    let cmp: Option<Cmp> = compare.as_ref().and_then(|c| Some(Cmp { other: c.other.as_ref()?, view: c.view.as_ref()? }));

    let side_w = if cmp.is_some() { SIDE_W_COMPARE } else { SIDE_W };
    let avail = ui.available_size();
    let h = (avail.y - 24.0).max(320.0);
    let canvas_w = (avail.x - side_w - 12.0).max(240.0);
    ui.horizontal_top(|ui| {
        ui.spacing_mut().item_spacing.x = 12.0;
        canvas(ui, l, view, selected, center_on, show_all, cmp, vec2(canvas_w, h));
        let l = &*l;
        ui.allocate_ui_with_layout(vec2(side_w, h), Layout::top_down(Align::Min), |ui| {
            theme::card_frame().show(ui, |ui| {
                ui.set_width(side_w - 34.0);
                ui.set_height(h - 34.0);
                egui::ScrollArea::vertical().id_salt("campaign_map_side").auto_shrink([false, false]).show(ui, |ui| {
                    ui.spacing_mut().interact_size.y = 18.0;
                    ui.spacing_mut().item_spacing.y = 4.0;
                    if let Some(go) = side_panel(ui, l, *selected, show_all, cmp) {
                        *selected = Some(go);
                        *center_on = Some(go);
                        ui.ctx().request_repaint();
                    }
                });
            });
        });
    });
}

/// "A: … vs B: …" with the swap and stop buttons, shown while a comparison is set up.
fn compare_bar(map: &mut MapState, saves: &[SaveInfo], ui: &mut Ui) {
    let (Some(c), Some(shown)) = (&map.compare, &map.save) else { return };
    let week = |loaded: Option<&Loaded>, path: &Path| match loaded {
        Some(l) => Some(l.g.save.week),
        None => saves.iter().find(|s| s.path == path).map(|s| s.week as i64),
    };
    let side = |ui: &mut Ui, tag: &str, path: &Path, loaded: Option<&Loaded>| {
        ui.label(RichText::new(tag).family(theme::semibold()).color(theme::ACCENT_HI));
        let mut text = save_name(saves, path);
        if let Some(w) = week(loaded, path) {
            text.push_str(&format!(" · week {w}"));
        }
        ui.add(egui::Label::new(theme::strong(text)).truncate());
    };
    let ready = map.loaded.is_some() && c.other.is_some();
    let reading = c.other.is_none() && c.error.is_none();
    let error = c.error.clone();
    let (mut swap, mut stop, mut retry) = (false, false, false);
    ui.add_space(8.0);
    theme::inset_frame().show(ui, |ui| {
        ui.set_width(ui.available_width());
        ui.horizontal(|ui| {
            ui.spacing_mut().interact_size.y = 24.0;
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                stop = theme::button(ui, "Stop comparing").clicked();
                ui.add_enabled_ui(ready, |ui| {
                    swap = theme::icon_button(ui, icon::COMPARE, "Swap A and B").clicked();
                });
                ui.with_layout(Layout::left_to_right(Align::Center), |ui| {
                    side(ui, "A:", shown, map.loaded.as_ref());
                    ui.label(theme::faint("vs"));
                    side(ui, "B:", &c.path, c.other.as_ref());
                    if reading {
                        ui.add(egui::Spinner::new().size(14.0));
                        ui.label(theme::muted("Asking the game to read B…"));
                    } else if ready {
                        ui.label(theme::faint("Changes are counted from B to A."));
                    }
                });
            });
        });
        if let Some(e) = error {
            ui.horizontal(|ui| {
                ui.label(RichText::new(icon::WARNING).color(theme::WARN));
                ui.add(egui::Label::new(RichText::new(e).color(theme::WARN)).wrap());
            });
            retry = theme::button(ui, "Try again").clicked();
        }
    });
    if stop {
        map.compare = None;
    } else if swap {
        map.swap();
    } else if retry {
        if let Some(path) = map.compare.as_ref().map(|c| c.path.clone()) {
            map.compare_with(path);
        }
    }
}

/// What the canvas and the side panel need to draw a comparison.
#[derive(Clone, Copy)]
struct Cmp<'a> {
    other: &'a Loaded,
    view: &'a DiffView,
}

impl<'a> Cmp<'a> {
    /// B's version of one of A's planets, when the diff may show it (fog of war).
    fn then(self, a: &Planet, i: usize, show_all: bool) -> Option<&'a Planet> {
        let seen = |p: &Planet| show_all || (p.revealed && !p.hidden);
        let other: &'a Loaded = self.other;
        let b = other.g.planets.get((*self.view.b_of.get(i)?)?)?;
        (seen(a) && seen(b)).then_some(b)
    }

    /// The colour of a faction named by the diff: A's colour for it, else B's.
    fn faction_color(&self, a: &Loaded, faction: &str) -> Color32 {
        a.faction_color(faction).or_else(|| self.other.faction_color(faction)).unwrap_or(NEUTRAL)
    }
}

// ------------------------------------------------------------------ canvas

#[derive(Clone, Copy)]
struct Xf {
    /// Where the middle of the galaxy lands on screen.
    center: Pos2,
    /// The middle of the galaxy in world units.
    mid: Pos2,
    scale: f32,
}

impl Xf {
    /// World y points up; the screen's points down.
    fn to_screen(self, w: Pos2) -> Pos2 {
        pos2(self.center.x + (w.x - self.mid.x) * self.scale, self.center.y - (w.y - self.mid.y) * self.scale)
    }
}

#[allow(clippy::too_many_arguments)]
fn canvas(
    ui: &mut Ui,
    l: &mut Loaded,
    view: &mut View,
    selected: &mut Option<usize>,
    center_on: &mut Option<usize>,
    show_all: bool,
    cmp: Option<Cmp>,
    size: Vec2,
) {
    let (resp, painter) = ui.allocate_painter(size, Sense::click_and_drag());
    let rect = resp.rect;
    painter.rect_filled(rect, CornerRadius::same(12), theme::FIELD);

    // Overlay control, registered after the canvas so it takes its own clicks.
    let reset_rect = Rect::from_min_size(rect.left_bottom() + vec2(12.0, -38.0), vec2(92.0, 26.0));
    let reset = ui.interact(reset_rect, resp.id.with("reset"), Sense::click());
    if reset.clicked() {
        *view = View::default();
    }

    // ---- pan and zoom
    if resp.dragged() {
        view.pan += resp.drag_delta();
    }
    let pointer = if reset.hovered() { None } else { resp.hover_pos() };
    if let Some(cursor) = pointer {
        let (scroll, pinch) = ui.input(|i| (i.smooth_scroll_delta.y, i.zoom_delta()));
        let factor = pinch * (scroll * 0.0025).exp();
        if factor != 1.0 {
            let zoom = (view.zoom * factor).clamp(MIN_ZOOM, MAX_ZOOM);
            let k = zoom / view.zoom;
            // Keep the world point under the cursor where it is.
            let rel = cursor - rect.center();
            view.pan = rel - (rel - view.pan) * k;
            view.zoom = zoom;
        }
    }

    let pad = 48.0;
    let fit = ((rect.width() - 2.0 * pad).max(40.0) / l.bounds.width().max(1.0))
        .min((rect.height() - 2.0 * pad).max(40.0) / l.bounds.height().max(1.0));
    // A planet picked from the side panel is brought to the middle.
    if let Some(w) = center_on.take().and_then(|i| l.world.get(i).copied()) {
        let (mid, scale) = (l.bounds.center(), fit * view.zoom);
        view.pan = vec2(-(w.x - mid.x) * scale, (w.y - mid.y) * scale);
    }
    let xf = Xf { center: rect.center() + view.pan, mid: l.bounds.center(), scale: fit * view.zoom };
    l.screen.clear();
    l.screen.extend(l.world.iter().map(|w| xf.to_screen(*w)));

    let l = &*l;
    let g = &l.g;
    let screen = &l.screen;
    let zoom = view.zoom;
    let local = g.local_player;
    let r = (6.5 * zoom.powf(0.25)).clamp(5.0, 10.0);
    let shown = |i: usize| l.real[i] && (show_all || !g.planets[i].hidden);
    let known = |p: &Planet| show_all || p.revealed;

    if let Some(i) = *selected {
        if i >= g.planets.len() || !shown(i) {
            *selected = None;
        }
    }

    // ---- hover and selection
    let hovered: Option<usize> = pointer.and_then(|c| {
        let mut best = None;
        let mut best_d = (r + 6.0) * (r + 6.0);
        for i in 0..g.planets.len() {
            if shown(i) {
                let d = screen[i].distance_sq(c);
                if d <= best_d {
                    best_d = d;
                    best = Some(i);
                }
            }
        }
        best
    });
    if resp.clicked() {
        *selected = hovered;
    }
    if resp.dragged() {
        ui.ctx().set_cursor_icon(egui::CursorIcon::Grabbing);
    } else if hovered.is_some() || reset.hovered() {
        ui.ctx().set_cursor_icon(egui::CursorIcon::PointingHand);
    }

    let small = FontId::new(10.0, FontFamily::Proportional);
    let label_font = FontId::new(11.5, FontFamily::Proportional);
    let counts = zoom >= 1.8;
    // While comparing, whatever didn't change steps back so the changes stand out.
    let dim = |c: Color32| theme::lerp_color(c, theme::FIELD, 0.58);

    // ---- trade routes
    let route = stroke(1.0, theme::BORDER_HI.gamma_multiply(0.55));
    for &(a, b) in &l.routes {
        if shown(a) && shown(b) {
            painter.line_segment([screen[a], screen[b]], route);
        }
    }

    // ---- paths of fleets in transit (under the planets)
    for &fi in &l.moving {
        let f = &g.fleets[fi];
        if !(show_all || f.owner == local) {
            continue;
        }
        if let Some(&to) = l.by_index.get(&f.to).filter(|&&to| shown(to)) {
            let from = xf.to_screen(pos2(f.x, f.y));
            let stroke = stroke(1.0, l.color(f.owner).gamma_multiply(0.7));
            painter.extend(Shape::dashed_line(&[from, screen[to]], stroke, 5.0, 4.0));
        }
    }

    // ---- planets
    let visible = rect.expand(60.0);
    let labels_all = zoom >= 1.5 || g.planets.len() <= 24;
    let time = ui.input(|i| i.time);
    let mut animating = false;
    for (i, p) in g.planets.iter().enumerate() {
        let c = screen[i];
        if !shown(i) || !visible.contains(c) {
            continue;
        }
        let k = known(p);
        // The diff already leaves out anything fog of war hides, so a hit here may be drawn.
        let change = cmp.and_then(|d| d.view.changed.get(&i).map(|&n| (d, &d.view.diff.owner_changes[n])));
        let station = cmp.and_then(|d| d.view.stations.get(&i).map(|&n| &d.view.diff.stations[n]));
        let faded = cmp.is_some() && change.is_none() && station.is_none();
        let tint = |c: Color32| if faded { dim(c) } else { c };
        let col = tint(if k { l.color(p.owner) } else { UNSEEN });
        if let Some((d, ch)) = change {
            // Two colours: a ring in the previous owner's around the current owner's fill.
            let was = if ch.from.is_empty() { NEUTRAL } else { d.faction_color(l, &ch.from) };
            painter.circle_filled(c, r + 4.5, was);
            painter.circle_filled(c, r + 1.5, theme::FIELD);
        }
        if k && p.destroyed {
            painter.circle_stroke(c, r - 1.0, stroke(1.5, col.gamma_multiply(0.5)));
        } else {
            painter.circle_filled(c, r, col);
            if !k {
                painter.circle_stroke(c, r, stroke(1.0, tint(theme::BORDER_HI)));
            }
        }
        if k && p.owner == local && change.is_none() {
            painter.circle_stroke(c, r + 2.5, stroke(1.5, tint(theme::lerp_color(col, Color32::WHITE, 0.45))));
        }
        if k && p.in_battle {
            animating = true;
            let phase = ((time * 1.2) % 1.0) as f32;
            painter.circle_stroke(c, r + 3.5, stroke(1.5, theme::BAD));
            painter.circle_stroke(
                c,
                r + 3.5 + phase * 11.0,
                stroke(2.0, theme::BAD.gamma_multiply(1.0 - phase)),
            );
        }
        let halo = if change.is_some() { 3.0 } else { 0.0 };
        if *selected == Some(i) {
            painter.circle_stroke(c, r + 5.5 + halo, stroke(2.0, theme::ACCENT_HI));
        } else if hovered == Some(i) {
            painter.circle_stroke(c, r + 5.0 + halo, stroke(1.0, theme::TEXT.gamma_multiply(0.6)));
        }
        if change.is_some() {
            badge(&painter, c + vec2(r + 3.5, -(r + 3.5)));
        }
        // Space station level: a row of pips above the planet.
        let pips_y = c.y - r - 6.0 - halo;
        if k && p.starbase_level > 0 {
            let n = p.starbase_level.min(6);
            let x0 = c.x - (n - 1) as f32 * 2.25;
            for j in 0..n {
                painter.circle_filled(pos2(x0 + j as f32 * 4.5, pips_y), 1.6, tint(theme::TEXT));
            }
        }
        if let Some(s) = station {
            // An arrow beside the pips: the station grew or shrank since B.
            let n = p.starbase_level.clamp(0, 6);
            let at = pos2(c.x + (n as f32) * 2.25 + 5.0, pips_y);
            let up = s.to > s.from;
            let (color, dy) = if up { (theme::GOOD, -3.0) } else { (theme::BAD, 3.0) };
            let points = vec![at + vec2(0.0, dy), at + vec2(3.0, -dy), at + vec2(-3.0, -dy)];
            painter.add(Shape::convex_polygon(points, color, Stroke::NONE));
        }
        // Fleets in orbit, stacked to the right.
        let step = if counts { 16.0 } else { 11.0 };
        let mut slot = 0.0;
        for &fi in &l.orbit[i] {
            let f = &g.fleets[fi];
            if !(k || f.owner == local) {
                continue;
            }
            let at = c + vec2(r + 10.0 + halo + slot * step, 0.0);
            marker(&painter, at, 4.5, tint(l.color(f.owner)), false);
            if counts {
                painter.text(
                    at - vec2(0.0, 7.0),
                    Align2::CENTER_BOTTOM,
                    &l.fleet_labels[fi],
                    small.clone(),
                    theme::MUTED,
                );
            }
            slot += 1.0;
        }
        if labels_all || hovered == Some(i) || *selected == Some(i) {
            let color = if hovered == Some(i) || *selected == Some(i) {
                if k { theme::TEXT } else { theme::FAINT }
            } else if !k || faded {
                theme::FAINT
            } else if change.is_some() {
                theme::TEXT
            } else {
                theme::MUTED
            };
            painter.text(c + vec2(0.0, r + 5.0 + halo), Align2::CENTER_TOP, &l.names[i], label_font.clone(), color);
        }
    }

    // ---- fleets in transit
    for &fi in &l.moving {
        let f = &g.fleets[fi];
        if !(show_all || f.owner == local) {
            continue;
        }
        let at = xf.to_screen(pos2(f.x, f.y));
        if !visible.contains(at) {
            continue;
        }
        let color = if cmp.is_some() { dim(l.color(f.owner)) } else { l.color(f.owner) };
        marker(&painter, at, 5.5, color, true);
        if counts {
            painter.text(at - vec2(0.0, 8.0), Align2::CENTER_BOTTOM, &l.fleet_labels[fi], small.clone(), theme::MUTED);
        }
    }

    // ---- overlay: reset button, zoom readout, legend, hover label
    let fill = if reset.hovered() { Color32::from_rgb(36, 43, 58) } else { theme::CARD_HI };
    painter.rect_filled(reset_rect, CornerRadius::same(8), fill);
    painter.rect_stroke(reset_rect, CornerRadius::same(8), stroke(1.0, theme::BORDER), StrokeKind::Inside);
    painter.text(
        reset_rect.center(),
        Align2::CENTER_CENTER,
        "Reset view",
        FontId::new(12.0, FontFamily::Proportional),
        theme::TEXT,
    );
    painter.text(
        reset_rect.right_center() + vec2(10.0, 0.0),
        Align2::LEFT_CENTER,
        format!("{:.0}%", zoom * 100.0),
        FontId::new(12.0, FontFamily::Proportional),
        theme::FAINT,
    );
    if cmp.is_some() {
        legend(&painter, rect);
    }

    if let (Some(i), Some(cursor)) = (hovered, pointer) {
        let p = &g.planets[i];
        let change = cmp.and_then(|d| d.view.changed.get(&i).map(|&n| &d.view.diff.owner_changes[n]));
        let text = match (known(p), change) {
            (true, Some(ch)) => format!(
                "{}  ·  {}  ·  {} in B",
                l.names[i],
                owner_name(g, p.owner),
                if ch.from.is_empty() { "unowned".to_owned() } else { pretty(&ch.from) }
            ),
            (true, None) => format!("{}  ·  {}", l.names[i], owner_name(g, p.owner)),
            (false, _) => format!("{}  ·  Not seen", l.names[i]),
        };
        let galley = painter.layout_no_wrap(text, FontId::new(12.5, FontFamily::Proportional), theme::TEXT);
        let size = galley.size() + vec2(16.0, 10.0);
        let mut min = cursor + vec2(14.0, 16.0);
        min.x = min.x.min(rect.right() - size.x - 6.0).max(rect.left() + 6.0);
        min.y = min.y.min(rect.bottom() - size.y - 6.0).max(rect.top() + 6.0);
        let tip = Rect::from_min_size(min, size);
        painter.rect_filled(tip, CornerRadius::same(7), Color32::from_rgb(26, 31, 42));
        painter.rect_stroke(tip, CornerRadius::same(7), stroke(1.0, theme::BORDER_HI), StrokeKind::Inside);
        painter.galley(tip.min + vec2(8.0, 5.0), galley, theme::TEXT);
    }

    painter.rect_stroke(rect, CornerRadius::same(12), stroke(1.0, theme::BORDER), StrokeKind::Inside);

    if animating {
        ui.ctx().request_repaint();
    }
}

fn stroke(width: f32, color: Color32) -> Stroke {
    Stroke::new(width, color)
}

/// A fleet marker: a diamond in orbit, a triangle in transit.
fn marker(painter: &egui::Painter, at: Pos2, s: f32, color: Color32, triangle: bool) {
    let points = if triangle {
        vec![at + vec2(0.0, -s), at + vec2(s * 0.87, s * 0.5), at + vec2(-s * 0.87, s * 0.5)]
    } else {
        vec![at + vec2(0.0, -s), at + vec2(s, 0.0), at + vec2(0.0, s), at + vec2(-s, 0.0)]
    };
    painter.add(Shape::convex_polygon(points, color, stroke(1.0, theme::FIELD)));
}

/// The "changed hands" badge: a small arrow in a disc.
fn badge(painter: &egui::Painter, at: Pos2) {
    painter.circle_filled(at, 5.5, theme::TEXT);
    let ink = stroke(1.4, theme::FIELD);
    painter.line_segment([at + vec2(-3.0, 0.0), at + vec2(3.0, 0.0)], ink);
    painter.line_segment([at + vec2(0.6, -2.4), at + vec2(3.0, 0.0)], ink);
    painter.line_segment([at + vec2(0.6, 2.4), at + vec2(3.0, 0.0)], ink);
}

/// Explains the comparison's drawing, in the canvas's bottom right corner.
fn legend(painter: &egui::Painter, canvas: Rect) {
    let font = FontId::new(11.5, FontFamily::Proportional);
    let rows = ["Changed hands: ring = owner in B, fill = owner in A", "Dimmed: nothing changed here", "Space station level went up or down"];
    let size = vec2(322.0, 14.0 + rows.len() as f32 * 19.0);
    let rect = Rect::from_min_size(canvas.right_bottom() - size - vec2(12.0, 12.0), size);
    if rect.left() < canvas.left() + 200.0 {
        return; // No room next to the reset button.
    }
    painter.rect_filled(rect, CornerRadius::same(8), theme::CARD_HI.gamma_multiply(0.94));
    painter.rect_stroke(rect, CornerRadius::same(8), stroke(1.0, theme::BORDER), StrokeKind::Inside);
    for (n, text) in rows.iter().enumerate() {
        let mid = pos2(rect.left() + 20.0, rect.top() + 16.0 + n as f32 * 19.0);
        match n {
            0 => {
                painter.circle_filled(mid, 7.0, PALETTE[1]);
                painter.circle_filled(mid, 4.6, theme::CARD_HI);
                painter.circle_filled(mid, 3.6, PALETTE[0]);
            }
            1 => {
                painter.circle_filled(mid, 4.5, theme::lerp_color(PALETTE[0], theme::FIELD, 0.58));
            }
            _ => {
                let up = mid - vec2(4.0, 0.0);
                let down = mid + vec2(4.0, 0.0);
                painter.add(Shape::convex_polygon(
                    vec![up + vec2(0.0, -3.0), up + vec2(3.0, 3.0), up + vec2(-3.0, 3.0)],
                    theme::GOOD,
                    Stroke::NONE,
                ));
                painter.add(Shape::convex_polygon(
                    vec![down + vec2(0.0, 3.0), down + vec2(3.0, -3.0), down + vec2(-3.0, -3.0)],
                    theme::BAD,
                    Stroke::NONE,
                ));
            }
        }
        painter.text(mid + vec2(16.0, 0.0), Align2::LEFT_CENTER, *text, font.clone(), theme::MUTED);
    }
}

// ------------------------------------------------------------------ side panel

/// Returns a planet to select and bring into view when one was clicked in a list.
fn side_panel(ui: &mut Ui, l: &Loaded, selected: Option<usize>, show_all: bool, cmp: Option<Cmp>) -> Option<usize> {
    let g = &l.g;
    let mut go = None;
    match cmp {
        Some(d) => go = changes_block(ui, l, d, selected, show_all),
        None => factions_block(ui, l, show_all),
    }

    ui.add_space(8.0);
    theme::overline(ui, "Selected planet");
    let Some((i, p)) = selected.and_then(|i| g.planets.get(i).map(|p| (i, p))) else {
        ui.label(theme::muted("Click a planet for its details."));
        return go;
    };
    ui.label(theme::title(l.names[i].clone()));
    if !(show_all || p.revealed) {
        ui.label(theme::muted("Your faction hasn't seen this planet."));
        return go;
    }

    // While comparing, a value that differs is shown as "B → A".
    let then: Option<(&Planet, &Galaxy)> = cmp.and_then(|d| {
        let other: &Loaded = d.other;
        d.then(p, i, show_all).map(|b| (b, &other.g))
    });
    let both = |ui: &mut Ui, key: &str, now: String, was: Option<String>| match was.filter(|w| *w != now) {
        Some(was) => {
            ui.horizontal(|ui| {
                ui.label(theme::muted(key));
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    ui.label(theme::text(now));
                    ui.label(theme::faint("→"));
                    ui.label(theme::muted(was)).on_hover_text("In B");
                });
            });
            true
        }
        None => {
            theme::kv(ui, key, theme::text(now));
            false
        }
    };
    let mut differs = both(ui, "Owner", owner_name(g, p.owner), then.map(|(b, bg)| owner_name(bg, b.owner)));
    if !p.kind.is_empty() {
        theme::kv(ui, "Type", theme::text(pretty(&p.kind)));
    }
    differs |= both(ui, "Base income", thousands(p.credit_value), then.map(|(b, _)| thousands(b.credit_value)));
    differs |= both(ui, "Space station", level(p.starbase_level), then.map(|(b, _)| level(b.starbase_level)));
    differs |= both(ui, "Ground base", level(p.ground_level), then.map(|(b, _)| level(b.ground_level)));
    if p.in_battle || p.corrupted || p.destroyed {
        ui.horizontal_wrapped(|ui| {
            if p.in_battle {
                theme::chip(ui, "In battle", theme::BAD);
            }
            if p.corrupted {
                theme::chip(ui, "Corrupted", theme::WARN);
            }
            if p.destroyed {
                theme::chip(ui, "Destroyed", theme::MUTED);
            }
        });
    }
    if cmp.is_some() {
        let note = match then {
            Some((b, _)) if b.corrupted != p.corrupted || b.destroyed != p.destroyed => {
                let mut was = Vec::new();
                if b.corrupted != p.corrupted {
                    was.push(if b.corrupted { "corrupted" } else { "not corrupted" });
                }
                if b.destroyed != p.destroyed {
                    was.push(if b.destroyed { "destroyed" } else { "not destroyed" });
                }
                format!("In B it was {}.", was.join(" and "))
            }
            Some(_) if differs => "Values that differ read B → A.".to_owned(),
            Some(_) => "The same in B.".to_owned(),
            None => "Not in B, or your faction hadn't seen it there.".to_owned(),
        };
        ui.label(theme::faint(note));
    }

    ui.add_space(8.0);
    theme::overline(ui, "Fleets in orbit");
    if l.orbit[i].is_empty() {
        ui.label(theme::faint("None"));
    }
    for &fi in &l.orbit[i] {
        let f = &g.fleets[fi];
        ui.horizontal(|ui| {
            let (dot, _) = ui.allocate_exact_size(vec2(8.0, 8.0), Sense::hover());
            ui.painter().circle_filled(dot.center(), 4.0, l.color(f.owner));
            ui.label(theme::text(owner_name(g, f.owner)));
            let n = l.fleet_units[fi];
            ui.label(theme::faint(if n == 1 { "1 unit".to_owned() } else { format!("{n} units") }));
        });
        for u in &f.units {
            ui.label(theme::muted(format!("    {} × {}", u.count, pretty(&u.kind))));
        }
    }

    ui.add_space(8.0);
    theme::overline(ui, "Ground units");
    stacks(ui, g, &p.surface, p.owner);

    ui.add_space(8.0);
    theme::overline(ui, "Buildings");
    stacks(ui, g, &p.structures, p.owner);
    ui.add_space(8.0);
    go
}

fn factions_block(ui: &mut Ui, l: &Loaded, show_all: bool) {
    let g = &l.g;
    let value = |s: String| theme::text(s);
    let hidden = || theme::faint("Hidden");

    theme::overline(ui, "Factions");
    // Factions that hold planets or field anything come first, yours at the top; the
    // rest (pirates and the like with nothing on the map) are only named.
    let active = |n: usize| {
        let (pl, t) = (&g.players[n], &l.tallies[n]);
        pl.index == g.local_player || pl.planets > 0 || t.space + t.ground + t.structures + t.fleets > 0
    };
    let mut any = false;
    let mut idle = String::new();
    for pass in 0..2 {
        for (n, pl) in g.players.iter().enumerate() {
            let you = pl.index == g.local_player;
            if pl.neutral || you != (pass == 0) {
                continue;
            }
            if !active(n) {
                if !idle.is_empty() {
                    idle.push_str(", ");
                }
                idle.push_str(&pretty(&pl.faction));
                continue;
            }
            any = true;
            let open = show_all || you;
            ui.add_space(2.0);
            ui.horizontal(|ui| {
                let (dot, _) = ui.allocate_exact_size(vec2(10.0, 10.0), Sense::hover());
                ui.painter().circle_filled(dot.center(), 5.0, l.color(pl.index));
                ui.add(egui::Label::new(theme::strong(pretty(&pl.faction))).truncate());
                if you {
                    theme::chip(ui, "You", theme::ACCENT_HI);
                }
            });
            if open {
                theme::kv(ui, "Credits", value(thousands(pl.credits)));
                let tech = if pl.max_tech_level > 0 {
                    format!("{} / {}", pl.tech_level, pl.max_tech_level)
                } else {
                    pl.tech_level.to_string()
                };
                theme::kv(ui, "Tech level", value(tech));
            } else {
                theme::kv(ui, "Credits", hidden());
                theme::kv(ui, "Tech level", hidden());
            }
            theme::kv(ui, "Planets", value(pl.planets.to_string()));
            if open {
                let t = &l.tallies[n];
                theme::kv(ui, "Space units", value(t.space.to_string()));
                theme::kv(ui, "Ground units", value(t.ground.to_string()));
                theme::kv(ui, "Structures", value(t.structures.to_string()));
                theme::kv(ui, "Fleets", value(t.fleets.to_string()));
            } else {
                theme::kv(ui, "Forces", hidden());
            }
            ui.add_space(4.0);
            theme::separator(ui);
        }
    }
    if !any {
        ui.label(theme::muted("This save lists no factions."));
    }
    if !idle.is_empty() {
        ui.add(egui::Label::new(theme::faint(format!("Nothing on the map: {idle}"))).wrap());
    }
}

/// "+3" in green, "−2" in red, "0" faint.
fn signed(v: i64) -> RichText {
    let text = match v {
        0 => "0".to_owned(),
        v if v > 0 => format!("+{}", thousands(v as f64)),
        v => format!("−{}", thousands(-(v as f64))),
    };
    let color = match v {
        0 => theme::FAINT,
        v if v > 0 => theme::GOOD,
        _ => theme::BAD,
    };
    RichText::new(text).color(color).size(12.5)
}

fn plain_signed(v: i64) -> String {
    match v {
        0 => "0".to_owned(),
        v if v > 0 => format!("+{v}"),
        v => format!("−{}", -v),
    }
}

/// The comparison's share of the side panel: the table of factions and the lists of
/// planets that changed. Returns a planet clicked in one of the lists.
fn changes_block(ui: &mut Ui, l: &Loaded, d: Cmp, selected: Option<usize>, show_all: bool) -> Option<usize> {
    let g = &l.g;
    let diff = &d.view.diff;
    let mut go = None;

    theme::overline(ui, "Changes from B to A");
    let local = g.player(g.local_player).map(|p| p.faction.as_str()).unwrap_or("");
    // The same factions the plain view lists: yours first, then anyone with a stake.
    let active = |f: &galaxy::FactionDiff| {
        let on_map = |lo: &Loaded, index: Option<i32>| {
            index
                .and_then(|ix| lo.g.players.iter().position(|p| p.index == ix))
                .map(|n| {
                    let t = &lo.tallies[n];
                    lo.g.players[n].planets > 0 || t.space + t.ground + t.structures + t.fleets > 0
                })
                .unwrap_or(false)
        };
        !f.neutral && (f.faction == local || f.gained + f.lost > 0 || on_map(l, f.a) || on_map(d.other, f.b))
    };
    let mut rows: Vec<&galaxy::FactionDiff> = diff.factions.iter().filter(|f| active(f)).collect();
    rows.sort_by_key(|f| f.faction != local);
    if rows.is_empty() {
        ui.label(theme::muted("These saves list no factions."));
    } else {
        let head = |ui: &mut Ui, s: &str| {
            ui.label(RichText::new(s).color(theme::FAINT).size(11.0));
        };
        let hidden = |ui: &mut Ui| {
            ui.label(theme::faint("—")).on_hover_text("Hidden. Turn on Show everything to see other factions' books.");
        };
        egui::Grid::new("campaign_map_changes").num_columns(5).spacing(vec2(10.0, 4.0)).show(ui, |ui| {
            head(ui, "Faction");
            head(ui, "Planets");
            head(ui, "Credits");
            head(ui, "Tech");
            head(ui, "Units");
            ui.end_row();
            for f in rows {
                ui.horizontal(|ui| {
                    ui.spacing_mut().item_spacing.x = 5.0;
                    let (dot, _) = ui.allocate_exact_size(vec2(9.0, 9.0), Sense::hover());
                    ui.painter().circle_filled(dot.center(), 4.5, d.faction_color(l, &f.faction));
                    let name = RichText::new(pretty(&f.faction)).size(12.5);
                    let name = if f.faction == local { name.color(theme::TEXT).family(theme::semibold()) } else { name.color(theme::TEXT) };
                    ui.add_sized(vec2(82.0, 18.0), egui::Label::new(name).truncate()).on_hover_text(pretty(&f.faction));
                });
                ui.label(signed(f.net() as i64)).on_hover_text(format!("Gained {}, lost {}", f.gained, f.lost));
                match f.credits {
                    Some(c) => {
                        ui.label(signed(c.round() as i64));
                    }
                    None => hidden(ui),
                }
                match f.tech {
                    Some(t) => {
                        ui.label(signed(t as i64));
                    }
                    None => hidden(ui),
                }
                match f.units {
                    Some(u) => {
                        ui.label(signed(u.total())).on_hover_text(format!(
                            "Space {}, ground {}, buildings {}, fleets {}",
                            plain_signed(u.space),
                            plain_signed(u.ground),
                            plain_signed(u.structures),
                            plain_signed(u.fleets)
                        ));
                    }
                    None => hidden(ui),
                }
                ui.end_row();
            }
        });
    }
    let fleets = format!("{} → {} ({})", diff.fleets_b, diff.fleets_a, plain_signed(diff.fleets_delta()));
    theme::kv(ui, if show_all { "Fleets on the map" } else { "Your fleets" }, theme::text(fleets));
    if !show_all {
        ui.add(
            egui::Label::new(theme::faint("Only what your faction has seen in both saves is compared."))
                .wrap(),
        );
    }

    // One clickable line per planet: its name, then what changed in the factions' colours.
    let font = FontId::new(13.0, FontFamily::Proportional);
    let piece = |job: &mut egui::text::LayoutJob, text: &str, color: Color32| {
        job.append(text, 0.0, egui::TextFormat { font_id: font.clone(), color, ..Default::default() });
    };
    let faction = |name: &str| if name.is_empty() { "Unowned".to_owned() } else { pretty(name) };
    let tint = |name: &str| if name.is_empty() { NEUTRAL } else { d.faction_color(l, name) };

    ui.add_space(8.0);
    theme::overline(ui, &format!("Changed hands ({})", diff.owner_changes.len()));
    if diff.owner_changes.is_empty() {
        ui.label(theme::muted("No planet changed hands between these saves."));
    }
    for ch in &diff.owner_changes {
        let mut job = egui::text::LayoutJob::default();
        piece(&mut job, &format!("{}   ", l.names[ch.a]), theme::TEXT);
        piece(&mut job, &faction(&ch.from), tint(&ch.from));
        piece(&mut job, "  →  ", theme::FAINT);
        piece(&mut job, &faction(&ch.to), tint(&ch.to));
        if ui.selectable_label(selected == Some(ch.a), job).on_hover_text("Show this planet on the map").clicked() {
            go = Some(ch.a);
        }
    }

    if !diff.stations.is_empty() {
        ui.add_space(8.0);
        theme::overline(ui, &format!("Space stations ({})", diff.stations.len()));
        for s in &diff.stations {
            let mut job = egui::text::LayoutJob::default();
            piece(&mut job, &format!("{}   ", l.names[s.a]), theme::TEXT);
            piece(&mut job, &level(s.from), theme::MUTED);
            piece(&mut job, "  →  ", theme::FAINT);
            piece(&mut job, &level(s.to), if s.to > s.from { theme::GOOD } else { theme::BAD });
            if ui.selectable_label(selected == Some(s.a), job).on_hover_text("Show this planet on the map").clicked() {
                go = Some(s.a);
            }
        }
    }
    ui.add_space(4.0);
    theme::separator(ui);
    go
}

fn stacks(ui: &mut Ui, g: &Galaxy, list: &[Stack], planet_owner: i32) {
    if list.is_empty() {
        ui.label(theme::faint("None"));
    }
    for s in list {
        let mut line = format!("{} × {}", s.count, pretty(&s.kind));
        if s.owner != planet_owner {
            line.push_str(&format!("  ({})", owner_name(g, s.owner)));
        }
        ui.label(theme::muted(line));
    }
}

fn level(n: i32) -> String {
    if n > 0 {
        format!("Level {n}")
    } else {
        "None".to_owned()
    }
}

fn owner_name(g: &Galaxy, player: i32) -> String {
    match g.player(player) {
        Some(p) if !p.faction.is_empty() => pretty(&p.faction),
        _ => "Unowned".to_owned(),
    }
}

fn thousands(v: f64) -> String {
    let n = v.round() as i64;
    let digits = n.unsigned_abs().to_string();
    let mut out = String::with_capacity(digits.len() + digits.len() / 3 + 1);
    if n < 0 {
        out.push('-');
    }
    for (i, ch) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i) % 3 == 0 {
            out.push(',');
        }
        out.push(ch);
    }
    out
}

fn parse_hex(s: &str) -> Option<Color32> {
    let s = s.trim().trim_start_matches('#');
    if s.len() != 6 || !s.is_ascii() {
        return None;
    }
    let v = u32::from_str_radix(s, 16).ok()?;
    let c = Color32::from_rgb((v >> 16) as u8, (v >> 8) as u8, v as u8);
    // Very dark faction colours would vanish against the map; lift them.
    if c.r().max(c.g()).max(c.b()) < 70 {
        Some(theme::lerp_color(c, NEUTRAL, 0.5))
    } else {
        Some(c)
    }
}

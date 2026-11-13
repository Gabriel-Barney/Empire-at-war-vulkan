//! Mods: everything the scan found as a grid of cards, and Steam Workshop (your
//! subscriptions, and browsing for more) once the launcher is connected to Steam.

use crate::app::{open_in_explorer, App, Page, Textures};
use crate::config::{mod_key, Config};
use crate::mods::{self, ModInfo, ModSource, Sizes};
use crate::steam::same_path;
use crate::theme::{self, icon};
use crate::workshop::{self, ItemStatus, LocalState, Notice, Sort, Status, Workshop};
use egui::{
    pos2, vec2, Align, Align2, Button, Color32, CornerRadius, FontFamily, FontId, Layout, Margin, Rect, Response,
    RichText, Sense, Stroke, Ui,
};
use std::collections::{BTreeMap, HashMap, HashSet};
use std::hash::{Hash, Hasher};
use std::path::{Path, PathBuf};
use std::time::{Duration, Instant, SystemTime};

/// How often Steam's Workshop records are looked at again while the launcher is in use.
const LOCAL_RECHECK: Duration = Duration::from_secs(20);
/// Longest tag the launcher accepts.
const TAG_MAX_CHARS: usize = 24;

#[derive(Clone, Copy, Default, PartialEq, Eq)]
enum Tab {
    #[default]
    Installed,
    Workshop,
}

#[derive(Clone, Copy, Default, PartialEq, Eq)]
enum WsTab {
    #[default]
    Subscribed,
    Browse,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
enum ModSort {
    #[default]
    Name,
    Used,
    Size,
    Updated,
}

impl ModSort {
    const ALL: [ModSort; 4] = [ModSort::Name, ModSort::Used, ModSort::Size, ModSort::Updated];

    fn label(self) -> &'static str {
        match self {
            ModSort::Name => "Name",
            ModSort::Used => "Recently used",
            ModSort::Size => "Size on disk",
            ModSort::Updated => "Recently updated",
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Hash)]
enum Usage {
    #[default]
    Any,
    Used,
    Unused,
}

/// What the Installed tab lists and in which order. Kept for the session.
#[derive(Clone, PartialEq, Eq, Hash)]
struct Filters {
    sort: ModSort,
    favourites_first: bool,
    source: Option<ModSource>,
    usage: Usage,
    favourites_only: bool,
    /// A mod must carry every one of these.
    tags: Vec<String>,
}

impl Default for Filters {
    fn default() -> Self {
        Self {
            sort: ModSort::Name,
            favourites_first: true,
            source: None,
            usage: Usage::Any,
            favourites_only: false,
            tags: Vec::new(),
        }
    }
}

impl Filters {
    fn any_filter(&self) -> bool {
        self.source.is_some() || self.usage != Usage::Any || self.favourites_only || !self.tags.is_empty()
    }

    fn clear_filters(&mut self) {
        *self = Filters { sort: self.sort, favourites_first: self.favourites_first, ..Default::default() };
    }
}

/// One card on the Installed tab, with everything that decided its place in the list.
struct Entry {
    /// Index into `app.mods`.
    index: usize,
    key: String,
    name: String,
    sort_name: String,
    in_profile: bool,
    favourite: bool,
    tags: Vec<String>,
    /// Names of the profiles that list the mod, joined. Empty = unused.
    used_by: String,
    /// The Workshop item Steam keeps up to date in this folder.
    steam_id: Option<u64>,
    size: Option<u64>,
    files: Option<u64>,
    measuring: bool,
    /// When the installed files last changed, Unix seconds (0 = unknown).
    updated: i64,
    /// `updated` is Steam's publish time of the installed version, not a file time.
    updated_by_steam: bool,
    last_used: i64,
    update_available: bool,
    change: Option<mods::Change>,
}

/// The Installed tab's list after filtering and sorting. Rebuilt only when `key`, a hash
/// of everything it was made from, no longer matches.
struct Listing {
    key: u64,
    items: Vec<Entry>,
    /// Every tag in use on an installed mod, with how many mods carry it.
    tags: Vec<(String, usize)>,
}

/// The "Edit entry" dialog: the launcher's own name, description and tags for a mod.
struct EditEntry {
    path: PathBuf,
    name: String,
    description: String,
    tags: Vec<String>,
    tag_input: String,
}

#[derive(Default)]
pub struct State {
    tab: Tab,
    search: String,
    view: Filters,
    listing: Option<Listing>,
    edit: Option<EditEntry>,
    edit_focus: bool,
    /// The Steam connection. Nothing talks to Steam until the user presses Connect.
    ws: Workshop,
    ws_tab: WsTab,
    browse_text: String,
    browse_sort: Sort,
    /// Titles for installed Workshop mods have been requested on this connection.
    titles_asked: bool,
    /// Text in the "Open a Workshop item" dialog.
    open_item: Option<String>,
    open_item_focus: bool,
    /// Item waiting for the unsubscribe confirmation.
    unsubscribe: Option<u64>,

    /// Hash of `app.mods` as of the last frame, to notice rescans.
    mods_stamp: u64,
    /// The user pressed Rescan: look at everything again, and measure folders afresh.
    rescanned: bool,
    /// Folder sizes. Measuring starts the first time the Installed tab is shown.
    sizes: Sizes,
    sizes_wanted: bool,
    sizes_rev: u64,
    /// Steam's own records about installed Workshop items (read without connecting).
    local: Option<LocalState>,
    /// Modification time and length of that file when it was last read.
    local_file: Option<(Option<SystemTime>, u64)>,
    local_checked: Option<Instant>,
    local_failures: u32,
    local_rev: u64,
    /// What each mod looked like the last time the launcher ran. `None` until loaded.
    seen: Option<mods::SeenMap>,
    seen_rev: u64,
}

impl State {
    /// The Steam connection, for other pages that show Workshop status or subscribe
    /// (profile import, diagnostics). It is polled every frame by [`background`].
    pub fn workshop(&self) -> &Workshop {
        &self.ws
    }

    pub fn workshop_mut(&mut self) -> &mut Workshop {
        &mut self.ws
    }

    /// Shows the Installed tab searching for `text`, with no other filter in the way.
    /// The command palette uses it to jump to one mod.
    pub fn focus_mod(&mut self, text: &str) {
        self.tab = Tab::Installed;
        self.search = text.to_owned();
        self.view.clear_filters();
    }
}

enum Act {
    Add(PathBuf),
    Remove(PathBuf),
    Edit(PathBuf),
    Open(PathBuf),
    OpenSteam(u64),
    Subscribe(u64),
    AskUnsubscribe(u64),
    Download(u64),
    ToggleFavourite(String),
    DismissChange(String),
}

/// Paints a texture so it fills `rect`, cropping whatever doesn't fit.
pub(super) fn paint_cover(ui: &Ui, tex: &egui::TextureHandle, rect: Rect, radius: CornerRadius) {
    let size = tex.size_vec2();
    let mut uv = Rect::from_min_max(pos2(0.0, 0.0), pos2(1.0, 1.0));
    if size.x > 0.0 && size.y > 0.0 && rect.width() > 0.0 && rect.height() > 0.0 {
        let tex_aspect = size.x / size.y;
        let rect_aspect = rect.width() / rect.height();
        if tex_aspect > rect_aspect {
            let w = rect_aspect / tex_aspect;
            uv = Rect::from_min_max(pos2(0.5 - w / 2.0, 0.0), pos2(0.5 + w / 2.0, 1.0));
        } else {
            let h = tex_aspect / rect_aspect;
            uv = Rect::from_min_max(pos2(0.0, 0.5 - h / 2.0), pos2(1.0, 0.5 + h / 2.0));
        }
    }
    egui::Image::from_texture(egui::load::SizedTexture::new(tex.id(), size)).uv(uv).corner_radius(radius).paint_at(ui, rect);
}

/// Pill-shaped toggle button used for filters and segmented choices.
pub(super) fn filter_chip(ui: &mut Ui, label: &str, selected: bool) -> Response {
    let (fill, color, stroke) = if selected {
        (theme::ACCENT_DIM, theme::ACCENT_HI, theme::ACCENT.gamma_multiply(0.6))
    } else {
        (Color32::TRANSPARENT, theme::MUTED, theme::BORDER)
    };
    ui.add(
        Button::new(RichText::new(label).color(color).size(13.0))
            .fill(fill)
            .stroke(Stroke::new(1.0_f32, stroke))
            .corner_radius(CornerRadius::same(255)),
    )
}

/// Small square icon for a mod, or a neutral tile when it has none.
pub(super) fn paint_icon(ui: &Ui, tex: Option<egui::TextureHandle>, rect: Rect) {
    let radius = CornerRadius::same((rect.width() / 5.0) as u8);
    match tex {
        Some(t) => paint_cover(ui, &t, rect, radius),
        None => {
            ui.painter().rect_filled(rect, radius, theme::CARD_HI);
            ui.painter().text(
                rect.center(),
                Align2::CENTER_CENTER,
                icon::PUZZLE,
                FontId::new(rect.width() * 0.5, FontFamily::Proportional),
                theme::FAINT,
            );
        }
    }
}

fn art_placeholder(ui: &Ui, rect: Rect, name: &str, radius: CornerRadius) {
    let mut h = 0u32;
    for b in name.bytes() {
        h = h.wrapping_mul(31).wrapping_add(b as u32);
    }
    let hue = (h % 360) as f32 / 360.0;
    let top: Color32 = egui::ecolor::Hsva::new(hue, 0.45, 0.30, 1.0).into();
    let bottom: Color32 = egui::ecolor::Hsva::new((hue + 0.08).fract(), 0.50, 0.13, 1.0).into();
    // Rounded cap in the top colour, then a vertical gradient below the corners.
    ui.painter().rect_filled(rect, radius, top);
    let lower = Rect::from_min_max(pos2(rect.left(), rect.top() + 12.0), rect.max);
    let mut mesh = egui::Mesh::default();
    mesh.colored_vertex(lower.left_top(), top);
    mesh.colored_vertex(lower.right_top(), top);
    mesh.colored_vertex(lower.left_bottom(), bottom);
    mesh.colored_vertex(lower.right_bottom(), bottom);
    mesh.add_triangle(0, 1, 2);
    mesh.add_triangle(1, 2, 3);
    ui.painter().add(egui::Shape::mesh(mesh));
    let initial: String =
        name.chars().find(|c| c.is_alphanumeric()).map(|c| c.to_uppercase().collect()).unwrap_or_else(|| "?".to_owned());
    ui.painter().text(
        rect.center(),
        Align2::CENTER_CENTER,
        initial,
        FontId::new(46.0, theme::semibold()),
        Color32::from_white_alpha(70),
    );
}

/// Joined segmented control, the same look as the tabs on the Saves page.
fn segmented<T: Copy + PartialEq>(ui: &mut Ui, current: &mut T, options: &[(T, String)]) {
    egui::Frame::new()
        .fill(theme::FIELD)
        .stroke(Stroke::new(1.0_f32, theme::BORDER))
        .corner_radius(CornerRadius::same(10))
        .inner_margin(Margin::same(3))
        .show(ui, |ui| {
            ui.horizontal(|ui| {
                ui.spacing_mut().item_spacing.x = 2.0;
                for (value, label) in options {
                    let on = *current == *value;
                    let text = if on {
                        RichText::new(label).color(Color32::WHITE).family(theme::semibold())
                    } else {
                        theme::muted(label)
                    };
                    let button = Button::new(text)
                        .fill(if on { theme::ACCENT_DIM } else { Color32::TRANSPARENT })
                        .stroke(Stroke::NONE)
                        .corner_radius(CornerRadius::same(8));
                    if ui.add(button).clicked() {
                        *current = *value;
                    }
                }
            });
        });
}

/// Lays `items` out as rows of equally wide cards.
fn card_grid<T>(ui: &mut Ui, items: &[T], mut card: impl FnMut(&mut Ui, &T, f32)) {
    let gap = 14.0;
    let width = ui.available_width();
    let cols = (((width + gap) / (300.0 + gap)).floor() as usize).max(1);
    let card_w = ((width - gap * (cols as f32 - 1.0)) / cols as f32).floor();
    for chunk in items.chunks(cols) {
        ui.horizontal_top(|ui| {
            ui.spacing_mut().item_spacing.x = gap;
            for item in chunk {
                ui.allocate_ui_with_layout(vec2(card_w, 0.0), Layout::top_down(Align::Min), |ui| card(ui, item, card_w));
            }
        });
        ui.add_space(gap - 8.0);
    }
}

/// A description that is always two lines tall, so cards in a row line up.
fn summary_lines(ui: &mut Ui, text: &str, width: f32) {
    let font = FontId::new(13.0, FontFamily::Proportional);
    let line_h = ui.fonts(|f| f.row_height(&font));
    let (rect, resp) = ui.allocate_exact_size(vec2(width, line_h * 2.0), Sense::hover());
    let summary = text.split_whitespace().collect::<Vec<_>>().join(" ");
    let (shown, color) =
        if summary.is_empty() { ("No description.".to_owned(), theme::FAINT) } else { (summary.clone(), theme::MUTED) };
    let mut job = egui::text::LayoutJob::simple(shown, font, color, width);
    job.wrap.max_rows = 2;
    let galley = ui.fonts(|f| f.layout_job(job));
    let elided = galley.elided;
    ui.painter().galley(rect.min, galley, color);
    if elided {
        resp.on_hover_text(summary);
    }
}

fn fmt_count(n: u64) -> String {
    let digits = n.to_string();
    let mut out = String::with_capacity(digits.len() + digits.len() / 3);
    for (i, c) in digits.chars().enumerate() {
        if i > 0 && (digits.len() - i) % 3 == 0 {
            out.push(',');
        }
        out.push(c);
    }
    out
}

fn add_to_profile(app: &mut App, path: PathBuf) {
    let mods = &mut app.cfg.active_mut().mods;
    if !mods.iter().any(|p| same_path(p, &path)) {
        mods.push(path);
        app.touch();
    }
}

fn remove_from_profile(app: &mut App, path: &Path) {
    app.cfg.active_mut().mods.retain(|p| !same_path(p, path));
    app.touch();
}

pub fn show(app: &mut App, ui: &mut egui::Ui) {
    let mut st = std::mem::take(&mut app.mods_ui);
    tick(app, &mut st, ui.ctx());

    theme::page_header(
        ui,
        "Mods",
        "Finds mods in the game's Mods folder, Steam Workshop and any extra folders you add.",
    );
    segmented(ui, &mut st.tab, &[(Tab::Installed, "Installed".to_owned()), (Tab::Workshop, "Workshop".to_owned())]);
    ui.add_space(4.0);

    let act = match st.tab {
        Tab::Installed => installed_tab(app, &mut st, ui),
        Tab::Workshop => workshop_tab(app, &mut st, ui),
    };
    if let Some(act) = act {
        run(app, &mut st, act);
    }

    edit_modal(app, &mut st, ui.ctx());
    open_item_modal(app, &mut st, ui.ctx());
    unsubscribe_modal(app, &mut st, ui.ctx());
    app.mods_ui = st;
}

/// Called by the app every frame, so Workshop downloads finishing are noticed (and the
/// mod list rescanned) even while another page is showing.
pub fn background(app: &mut App, ctx: &egui::Context) {
    let mut st = std::mem::take(&mut app.mods_ui);
    tick(app, &mut st, ctx);
    app.mods_ui = st;
}

/// Takes in what Steam reported since the last frame.
fn tick(app: &mut App, st: &mut State, ctx: &egui::Context) {
    for notice in st.ws.poll(ctx) {
        match notice {
            Notice::Error(e) => app.err(e),
            Notice::Info(text) => app.ok(text),
            Notice::Rescan => {
                app.rescan_mods();
                // Steam just changed something on disk: read its records again right away.
                st.local_checked = None;
            }
            Notice::TitlesChanged => mods::apply_workshop_titles(&mut app.mods, st.ws.titles()),
        }
    }
    if !st.ws.is_connected() {
        st.titles_asked = false;
    } else if !st.titles_asked {
        // Installed Workshop mods that don't name themselves: ask Steam what they're called.
        st.titles_asked = true;
        let ids: Vec<u64> = app
            .mods
            .iter()
            .filter(|m| !m.own_name || !m.own_summary)
            .filter_map(|m| m.workshop_id.as_ref()?.parse().ok())
            .collect();
        st.ws.request_details(&ids);
    }
    track(app, st, ctx);
}

/// The Workshop item Steam installs and updates in this mod's folder. A copy of a Workshop
/// mod in the game's Mods folder is the user's own business, not Steam's.
fn steam_id(m: &ModInfo) -> Option<u64> {
    if m.source != ModSource::Workshop {
        return None;
    }
    m.workshop_id.as_ref()?.parse().ok()
}

/// When Steam's records say the installed version of this mod was published (0 = unknown).
fn steam_time(local: Option<&LocalState>, m: &ModInfo) -> i64 {
    steam_id(m).and_then(|id| local?.item(id)).map_or(0, |item| item.time_updated)
}

/// Whether a newer version than the installed one exists: Steam's live answer while the
/// launcher is connected, else what Steam's records on this PC say.
fn update_available(ws: &Workshop, local: Option<&LocalState>, id: u64) -> bool {
    if ws.is_connected() {
        if let Some(status) = ws.known_status(id) {
            return status.installed && status.needs_update;
        }
    }
    local.and_then(|l| l.item(id)).is_some_and(|item| item.update_available())
}

/// "6 Feb 2026" in local time.
fn fmt_date(unix: i64) -> String {
    chrono::DateTime::from_timestamp(unix, 0)
        .map(|t| t.with_timezone(&chrono::Local).format("%-d %b %Y").to_string())
        .unwrap_or_default()
}

fn hash_mods(mods: &[ModInfo]) -> u64 {
    let mut h = std::collections::hash_map::DefaultHasher::new();
    for m in mods {
        (&m.path, &m.name, &m.version, m.source, &m.workshop_id, m.has_data, m.summary.len()).hash(&mut h);
    }
    h.finish()
}

/// Keeps what the page knows beyond the scan up to date: Steam's records, the memory of
/// what each mod looked like last time, and folder sizes. Runs every frame; does nothing
/// unless the mod list or Steam's file changed.
fn track(app: &mut App, st: &mut State, ctx: &egui::Context) {
    let (measured, problems) = st.sizes.poll();
    if measured {
        st.sizes_rev += 1;
    }
    for problem in problems {
        app.err(problem);
    }

    let stamp = hash_mods(&app.mods);
    let rescanned = std::mem::take(&mut st.rescanned);
    let mods_changed = stamp != st.mods_stamp || st.seen.is_none() || rescanned;
    st.mods_stamp = stamp;

    let mut local_changed = false;
    if mods_changed || st.local_checked.is_none_or(|t| t.elapsed() >= LOCAL_RECHECK) {
        st.local_checked = Some(Instant::now());
        local_changed = reload_local(app, st);
    }
    if !mods_changed && !local_changed {
        return;
    }

    // Has any mod changed since the launcher last saw it?
    let seen_file = mods::seen_path();
    let seen = st.seen.get_or_insert_with(|| mods::load_seen(&seen_file));
    let mut noted = false;
    for m in &app.mods {
        noted |= mods::note_seen(seen, &mod_key(&m.path), &m.version, steam_time(st.local.as_ref(), m));
    }
    if noted {
        st.seen_rev += 1;
        if let Err(e) = mods::save_seen(&seen_file, seen) {
            app.err(e);
        }
    }
    if st.sizes_wanted {
        measure(app, st, ctx, rescanned);
    }
}

/// Reads Steam's Workshop records again if the file changed. True when what they say changed.
fn reload_local(app: &mut App, st: &mut State) -> bool {
    let path = app.steam.workshop_dir.as_deref().and_then(workshop::local_state_path);
    let file = path.as_ref().and_then(|p| std::fs::metadata(p).ok()).map(|m| (m.modified().ok(), m.len()));
    if file == st.local_file {
        return false;
    }
    let local = match (&path, file.is_some()) {
        (Some(path), true) => match LocalState::read(path) {
            Ok(local) => local,
            Err(e) => {
                // Steam may be in the middle of writing it: look again at the next check, and
                // only say something if it still can't be read then.
                st.local_failures += 1;
                if st.local_failures == 2 {
                    app.err(e);
                }
                return false;
            }
        },
        _ => None,
    };
    st.local_failures = 0;
    st.local_file = file;
    let changed = local != st.local;
    st.local = local;
    if changed {
        st.local_rev += 1;
    }
    changed
}

/// Queues every mod folder for measuring. Folders that didn't change come straight from
/// the cache unless `force` is set.
fn measure(app: &mut App, st: &mut State, ctx: &egui::Context, force: bool) {
    let folders: Vec<(PathBuf, i64)> =
        app.mods.iter().map(|m| (m.path.clone(), steam_time(st.local.as_ref(), m))).collect();
    if let Err(e) = st.sizes.request(ctx, folders, force) {
        app.err(e);
    }
    // Which folders are waiting to be measured is part of what the list shows.
    st.sizes_rev += 1;
}

fn run(app: &mut App, st: &mut State, act: Act) {
    match act {
        Act::Add(path) => {
            if path.is_dir() {
                add_to_profile(app, path);
            } else {
                app.err(format!("Folder no longer exists: {}", path.display()));
            }
        }
        Act::Remove(path) => remove_from_profile(app, &path),
        Act::Edit(path) => {
            let key = mod_key(&path);
            st.edit = Some(EditEntry {
                name: app.cfg.mod_names.get(&key).cloned().unwrap_or_default(),
                description: app.cfg.mod_descriptions.get(&key).cloned().unwrap_or_default(),
                tags: app.cfg.mod_tags.get(&key).cloned().unwrap_or_default(),
                tag_input: String::new(),
                path,
            });
            st.edit_focus = true;
        }
        Act::ToggleFavourite(key) => {
            if !app.cfg.mod_favourites.remove(&key) {
                app.cfg.mod_favourites.insert(key);
            }
            app.touch();
        }
        Act::DismissChange(key) => {
            if let Some(seen) = st.seen.as_mut() {
                if mods::dismiss_change(seen, &key) {
                    st.seen_rev += 1;
                    if let Err(e) = mods::save_seen(&mods::seen_path(), seen) {
                        app.err(e);
                    }
                }
            }
        }
        Act::Open(path) => {
            if path.is_dir() {
                open_in_explorer(&path);
            } else {
                app.err(format!("Folder no longer exists: {}", path.display()));
            }
        }
        Act::OpenSteam(id) => {
            if let Err(e) = workshop::open_item_page(id) {
                app.err(e);
            }
        }
        Act::Subscribe(id) => st.ws.subscribe(id),
        Act::AskUnsubscribe(id) => st.unsubscribe = Some(id),
        Act::Download(id) => st.ws.download(id),
    }
}

// ------------------------------------------------------------------ installed

/// Hash of everything the Installed tab's list is made from. Cheap next to rebuilding and
/// re-sorting the list, which only happens when this changes.
fn listing_key(app: &App, st: &State) -> u64 {
    let mut h = std::collections::hash_map::DefaultHasher::new();
    (st.mods_stamp, &st.search, &st.view, st.sizes_rev, st.local_rev, st.seen_rev).hash(&mut h);
    let cfg = &app.cfg;
    cfg.active_profile.hash(&mut h);
    for p in &cfg.profiles {
        (p.id, &p.name, &p.mods).hash(&mut h);
    }
    (&cfg.mod_names, &cfg.mod_favourites, &cfg.mod_tags, &cfg.mod_last_used).hash(&mut h);
    let connected = st.ws.is_connected();
    connected.hash(&mut h);
    if connected {
        for id in app.mods.iter().filter_map(steam_id) {
            st.ws.known_status(id).map(|s| (s.installed, s.needs_update)).hash(&mut h);
        }
    }
    h.finish()
}

fn build_listing(app: &App, st: &State, key: u64) -> Listing {
    let cfg = &app.cfg;
    let mut used: HashMap<String, Vec<&str>> = HashMap::new();
    for p in &cfg.profiles {
        for m in &p.mods {
            let names = used.entry(mod_key(m)).or_default();
            if !names.contains(&p.name.as_str()) {
                names.push(&p.name);
            }
        }
    }
    let active: HashSet<String> = cfg.active().mods.iter().map(|m| mod_key(m)).collect();
    let query = st.search.trim().to_lowercase();
    let wanted: Vec<String> = st.view.tags.iter().map(|t| t.to_lowercase()).collect();
    let local = st.local.as_ref();

    // Lower-cased tag to its spelling and the number of mods carrying it.
    let mut tag_counts: BTreeMap<String, (String, usize)> = BTreeMap::new();
    let mut items = Vec::new();
    for (index, m) in app.mods.iter().enumerate() {
        let key = mod_key(&m.path);
        let tags = cfg.mod_tags.get(&key).cloned().unwrap_or_default();
        for tag in &tags {
            tag_counts.entry(tag.to_lowercase()).or_insert_with(|| (tag.clone(), 0)).1 += 1;
        }
        let used_by = used.get(&key);
        let favourite = cfg.mod_favourites.contains(&key);
        let name = cfg.mod_names.get(&key).cloned().unwrap_or_else(|| m.name.clone());

        if st.view.source.is_some_and(|source| m.source != source)
            || (st.view.usage == Usage::Used && used_by.is_none())
            || (st.view.usage == Usage::Unused && used_by.is_some())
            || (st.view.favourites_only && !favourite)
            || !wanted.iter().all(|w| tags.iter().any(|t| t.to_lowercase() == *w))
        {
            continue;
        }
        if !query.is_empty()
            && !(name.to_lowercase().contains(&query)
                || m.name.to_lowercase().contains(&query)
                || m.folder.to_lowercase().contains(&query)
                || m.workshop_id.as_deref().is_some_and(|id| id.contains(&query))
                || tags.iter().any(|t| t.to_lowercase().contains(&query)))
        {
            continue;
        }

        let steam = steam_id(m);
        let record = steam.and_then(|id| local?.item(id));
        let measured = st.sizes.get(&key);
        // Until the folder has been walked, Steam's own count stands in for Workshop items.
        let size = measured.map(|x| x.bytes).or(record.filter(|r| r.installed && r.size > 0).map(|r| r.size));
        let steam_updated = record.map_or(0, |r| r.time_updated);
        items.push(Entry {
            index,
            sort_name: name.to_lowercase(),
            name,
            in_profile: active.contains(&key),
            favourite,
            tags,
            used_by: used_by.map(|names| names.join(", ")).unwrap_or_default(),
            steam_id: steam,
            size,
            files: measured.map(|x| x.files),
            measuring: measured.is_none() && st.sizes.is_waiting(&key),
            updated: if steam_updated > 0 { steam_updated } else { measured.map_or(0, |x| x.newest) },
            updated_by_steam: steam_updated > 0,
            last_used: cfg.mod_last_used.get(&key).copied().unwrap_or(0),
            update_available: steam.is_some_and(|id| update_available(&st.ws, local, id)),
            change: st.seen.as_ref().and_then(|seen| seen.get(&key)?.changed.clone()),
            key,
        });
    }

    let (sort, favourites_first) = (st.view.sort, st.view.favourites_first);
    items.sort_by(|a, b| {
        let favourites = if favourites_first { b.favourite.cmp(&a.favourite) } else { std::cmp::Ordering::Equal };
        // Largest / most recent first. Unknown values are 0, so they end up last.
        let chosen = match sort {
            ModSort::Name => std::cmp::Ordering::Equal,
            ModSort::Used => b.last_used.cmp(&a.last_used),
            ModSort::Size => b.size.unwrap_or(0).cmp(&a.size.unwrap_or(0)),
            ModSort::Updated => b.updated.cmp(&a.updated),
        };
        favourites.then(chosen).then_with(|| a.sort_name.cmp(&b.sort_name))
    });

    let mut tags: Vec<(String, usize)> = tag_counts.into_values().collect();
    tags.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.to_lowercase().cmp(&b.0.to_lowercase())));
    Listing { key, items, tags }
}

/// Makes sure `st.listing` matches the current state, rebuilding it only if it doesn't.
fn refresh_listing(app: &App, st: &mut State) {
    let key = listing_key(app, st);
    if st.listing.as_ref().is_none_or(|l| l.key != key) {
        st.listing = Some(build_listing(app, st, key));
    }
}

fn installed_tab(app: &mut App, st: &mut State, ui: &mut egui::Ui) -> Option<Act> {
    if app.mods.is_empty() {
        theme::card(ui, |ui| {
            theme::empty_state(
                ui,
                icon::PUZZLE,
                "No mods found",
                "Put mods in the game's Mods folder, subscribe to some on Steam Workshop, or add extra mod folders in Settings.",
            );
            ui.vertical_centered(|ui| {
                if theme::primary_button(ui, "Open Settings").clicked() {
                    app.page = Page::Settings;
                }
                if theme::button(ui, "Go to Workshop").clicked() {
                    st.tab = Tab::Workshop;
                }
                if theme::button(ui, "Rescan").clicked() {
                    app.rescan_mods();
                    st.rescanned = true;
                }
                ui.add_space(16.0);
            });
        });
        return None;
    }

    // Folder sizes are only worth measuring once somebody looks at this tab.
    if !st.sizes_wanted {
        st.sizes_wanted = true;
        measure(app, st, ui.ctx(), false);
    }
    refresh_listing(app, st);
    let all_tags: Vec<(String, usize)> = st.listing.as_ref().map(|l| l.tags.clone()).unwrap_or_default();
    // A tag nobody carries any more can't stay selected: it would hide everything.
    st.view.tags.retain(|t| all_tags.iter().any(|(known, _)| known.to_lowercase() == t.to_lowercase()));

    // ---- row 1: search, sort, rescan
    let mut rescan = false;
    let total = app.mods.len();
    ui.horizontal(|ui| {
        ui.add(
            egui::TextEdit::singleline(&mut st.search)
                .desired_width(260.0)
                .hint_text(format!("{}  Search name, folder, tag or Workshop id", icon::SEARCH)),
        );
        if !st.search.is_empty() && theme::icon_button(ui, icon::CLOSE, "Clear search").clicked() {
            st.search.clear();
        }
        ui.add_space(4.0);
        ui.label(theme::muted("Sort by"));
        egui::ComboBox::from_id_salt("mods_sort").width(150.0).selected_text(st.view.sort.label()).show_ui(ui, |ui| {
            for sort in ModSort::ALL {
                if ui.selectable_label(st.view.sort == sort, sort.label()).clicked() {
                    st.view.sort = sort;
                }
            }
        });
        if filter_chip(ui, "Favourites first", st.view.favourites_first)
            .on_hover_text("Keep favourite mods at the top, whatever the sort order.")
            .clicked()
        {
            st.view.favourites_first = !st.view.favourites_first;
        }
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if theme::button(ui, &format!("{}  Rescan", icon::REFRESH))
                .on_hover_text("Look for mods again and measure their folders afresh.")
                .clicked()
            {
                rescan = true;
            }
        });
    });

    // ---- row 2: filters
    ui.horizontal_wrapped(|ui| {
        ui.spacing_mut().item_spacing = vec2(6.0, 6.0);
        for (source, label) in [
            (None, "All sources"),
            (Some(ModSource::GameFolder), "Mods folder"),
            (Some(ModSource::Workshop), "Workshop"),
            (Some(ModSource::Extra), "Extra"),
        ] {
            if filter_chip(ui, label, st.view.source == source).clicked() {
                st.view.source = source;
            }
        }
        ui.add_space(8.0);
        if filter_chip(ui, &format!("{}  Favourites", icon::STAR_FILLED), st.view.favourites_only).clicked() {
            st.view.favourites_only = !st.view.favourites_only;
        }
        for (usage, label, tip) in [
            (Usage::Used, "In a profile", "Mods that at least one profile lists."),
            (Usage::Unused, "Unused", "Mods that no profile lists."),
        ] {
            if filter_chip(ui, label, st.view.usage == usage).on_hover_text(tip).clicked() {
                st.view.usage = if st.view.usage == usage { Usage::Any } else { usage };
            }
        }
        if !all_tags.is_empty() {
            ui.add_space(8.0);
            let selected = |tags: &[String], tag: &str| tags.iter().any(|t| t.to_lowercase() == tag.to_lowercase());
            let toggle = |tags: &mut Vec<String>, tag: &str| {
                let before = tags.len();
                tags.retain(|t| t.to_lowercase() != tag.to_lowercase());
                if tags.len() == before {
                    tags.push(tag.to_owned());
                }
            };
            if all_tags.len() <= 6 {
                for (tag, count) in &all_tags {
                    let chip = filter_chip(ui, &format!("{tag}  {count}"), selected(&st.view.tags, tag));
                    if chip.on_hover_text("Show only mods with this tag. Several tags: mods that have all of them.").clicked() {
                        toggle(&mut st.view.tags, tag);
                    }
                }
            } else {
                let text = match st.view.tags.len() {
                    0 => "Tags".to_owned(),
                    1 => format!("Tag: {}", st.view.tags[0]),
                    n => format!("{n} tags"),
                };
                egui::ComboBox::from_id_salt("mods_tag_filter")
                    .selected_text(text)
                    .close_behavior(egui::PopupCloseBehavior::CloseOnClickOutside)
                    .show_ui(ui, |ui| {
                        for (tag, count) in &all_tags {
                            let on = selected(&st.view.tags, tag);
                            if ui.selectable_label(on, format!("{tag}  ({count})")).clicked() {
                                toggle(&mut st.view.tags, tag);
                            }
                        }
                        if !st.view.tags.is_empty() {
                            ui.separator();
                            if ui.selectable_label(false, "Any tag").clicked() {
                                st.view.tags.clear();
                            }
                        }
                    })
                    .response
                    .on_hover_text("Show only mods that have every tag picked here.");
            }
        }
    });
    if rescan {
        app.rescan_mods();
        st.rescanned = true;
        // Straight away, so the list below is made from the new scan.
        track(app, st, ui.ctx());
        let n = app.mods.len();
        app.ok(if n == 1 { "Found 1 mod".to_owned() } else { format!("Found {n} mods") });
    }

    // ---- what to show (the header may just have changed it)
    refresh_listing(app, st);
    let listing = st.listing.take()?;
    let items = &listing.items;
    let profile_name = app.cfg.active().name.clone();
    let count = if items.len() == total {
        format!("{total} {}", if total == 1 { "mod" } else { "mods" })
    } else {
        format!("{} of {total} mods", items.len())
    };
    let mut line = format!("{count} · adding to profile \"{profile_name}\"");
    if st.view.usage == Usage::Unused && !items.is_empty() {
        let bytes: u64 = items.iter().filter_map(|e| e.size).sum();
        let unknown = items.iter().filter(|e| e.size.is_none()).count();
        line.push_str(&format!(" · these unused mods take {}", theme::fmt_bytes(bytes)));
        if unknown > 0 {
            line.push_str(&format!(" ({unknown} not measured yet)"));
        }
    }
    let measuring = items.iter().any(|e| e.measuring);
    let steam_pending = st.local.as_ref().is_some_and(|l| l.pending);
    ui.horizontal(|ui| {
        ui.label(theme::faint(line));
        if measuring {
            ui.add(egui::Spinner::new().size(11.0));
            ui.label(theme::faint("Measuring folders…"));
        }
        if steam_pending {
            ui.label(theme::faint("· Steam has Workshop downloads waiting"))
                .on_hover_text("Steam's records for the game say Workshop content is waiting to be downloaded or updated. Steam does that while its client runs.");
        }
    });
    ui.add_space(2.0);

    // ---- grid
    let mut act: Option<Act> = None;
    let mut clear_filters = false;
    let sort = st.view.sort;
    let filtered = st.view.any_filter() || !st.search.trim().is_empty();
    egui::ScrollArea::vertical().id_salt("mods_installed").auto_shrink([false, false]).show(ui, |ui| {
        if items.is_empty() {
            theme::empty_state(ui, icon::SEARCH, "No matching mods", "Try a different search, or fewer filters.");
            if filtered {
                ui.vertical_centered(|ui| {
                    clear_filters = theme::button(ui, "Clear search and filters").clicked();
                });
            }
            return;
        }
        card_grid(ui, items, |ui, e, card_w| {
            let Some(m) = app.mods.get(e.index) else { return };
            if let Some(a) = mod_card(ui, &mut app.tex, &mut st.ws, &app.cfg, m, e, sort, card_w) {
                act = Some(a);
            }
        });
        ui.add_space(24.0);
    });
    if clear_filters {
        st.view.clear_filters();
        st.search.clear();
    }
    st.listing = Some(listing);
    act
}

/// What the "Updated" chip says when hovered.
fn change_text(change: &mods::Change, used_by: &str) -> String {
    let mut what = Vec::new();
    if change.from_version != change.to_version {
        let show = |v: &str| if v.is_empty() { "no version".to_owned() } else { format!("version {v}") };
        what.push(format!("{} became {}", show(&change.from_version), show(&change.to_version)));
    }
    if change.from_time != change.to_time && change.to_time > 0 {
        what.push(match change.from_time {
            0 => format!("Steam installed the update of {}", fmt_date(change.to_time)),
            from => format!("Steam replaced the update of {} with the one of {}", fmt_date(from), fmt_date(change.to_time)),
        });
    }
    let mut text = format!("This mod changed since the launcher last saw it: {}.", what.join("; "));
    text.push_str("\n\nA save only loads with the mod data it was made with, so campaigns saved before the change may no longer load or may behave differently.");
    if !used_by.is_empty() {
        text.push_str(&format!("\n\nUsed by: {used_by}"));
    }
    text.push_str("\n\nClick to dismiss.");
    text
}

/// A single faint line that is cut short rather than wrapped, so cards keep their height.
fn fact_line(ui: &mut Ui, text: String) -> Response {
    ui.add(egui::Label::new(theme::faint(text)).truncate())
}

#[allow(clippy::too_many_arguments)]
fn mod_card(
    ui: &mut Ui,
    tex: &mut Textures,
    ws: &mut Workshop,
    cfg: &Config,
    m: &ModInfo,
    e: &Entry,
    sort: ModSort,
    width: f32,
) -> Option<Act> {
    let mut act = None;
    let workshop_id: Option<u64> = m.workshop_id.as_ref().and_then(|id| id.parse().ok());
    egui::Frame::new()
        .fill(theme::CARD)
        .stroke(Stroke::new(1.0_f32, theme::BORDER))
        .corner_radius(CornerRadius::same(12))
        .show(ui, |ui| {
            let w = width - 2.0;
            ui.set_width(w);
            ui.spacing_mut().item_spacing.y = 0.0;

            // Splash art. Only decoded once the card has actually been scrolled into view.
            let (art, _) = ui.allocate_exact_size(vec2(w, 130.0), Sense::hover());
            let visible = ui.is_rect_visible(art);
            if visible {
                let top = CornerRadius { nw: 11, ne: 11, sw: 0, se: 0 };
                // A Workshop mod without art of its own gets its Workshop preview.
                let splash = m.splash.clone().or_else(|| workshop_id.and_then(|id| ws.preview(ui.ctx(), id)));
                match splash.as_ref().and_then(|p| tex.get(ui.ctx(), p)) {
                    Some(t) => paint_cover(ui, &t, art, top),
                    None => art_placeholder(ui, art, &e.name, top),
                }
            }

            // Favourite star in the art's corner.
            let star = Rect::from_center_size(art.right_top() + vec2(-22.0, 22.0), vec2(30.0, 30.0));
            let star_resp = ui
                .interact(star, ui.id().with(("favourite", e.index)), Sense::click())
                .on_hover_text(if e.favourite { "Remove from favourites" } else { "Add to favourites" });
            if visible {
                let back = if star_resp.hovered() { 200 } else { 130 };
                ui.painter().circle_filled(star.center(), 14.0, Color32::from_black_alpha(back));
                let (glyph, color) = if e.favourite {
                    (icon::STAR_FILLED, theme::WARN)
                } else if star_resp.hovered() {
                    (icon::STAR, Color32::WHITE)
                } else {
                    (icon::STAR, Color32::from_white_alpha(170))
                };
                ui.painter().text(star.center(), Align2::CENTER_CENTER, glyph, FontId::new(14.0, FontFamily::Proportional), color);
            }
            if star_resp.clicked() {
                act = Some(Act::ToggleFavourite(e.key.clone()));
            }

            egui::Frame::new().inner_margin(Margin::same(14)).show(ui, |ui| {
                let inner = w - 28.0;
                ui.set_width(inner);
                ui.spacing_mut().item_spacing.y = 8.0;

                ui.horizontal(|ui| {
                    let (ir, _) = ui.allocate_exact_size(vec2(32.0, 32.0), Sense::hover());
                    if visible {
                        let t = m.icon.as_ref().and_then(|p| tex.get(ui.ctx(), p));
                        paint_icon(ui, t, ir);
                    }
                    ui.vertical(|ui| {
                        ui.spacing_mut().item_spacing.y = 1.0;
                        ui.add(egui::Label::new(theme::strong(&e.name)).truncate());
                        let mut sub = match (&m.workshop_id, m.source) {
                            (Some(id), ModSource::Workshop) => format!("Workshop {id}"),
                            _ => m.folder.clone(),
                        };
                        if !m.version.is_empty() {
                            let v = m.version.trim_start_matches(['v', 'V']);
                            sub.push_str(&format!(" · v{v}"));
                        }
                        fact_line(ui, sub).on_hover_text(m.path.display().to_string());

                        // Size on disk and when the files last changed.
                        let mut facts: Vec<String> = Vec::new();
                        let mut tip: Vec<String> = Vec::new();
                        match e.size {
                            Some(bytes) => {
                                facts.push(theme::fmt_bytes(bytes));
                                tip.push(match e.files {
                                    Some(files) => format!("{} on disk in {} files.", theme::fmt_bytes(bytes), fmt_count(files)),
                                    None => "Size as Steam recorded it. The folder is still being measured.".to_owned(),
                                });
                            }
                            None if e.measuring => facts.push("Measuring…".to_owned()),
                            None => {}
                        }
                        if e.updated > 0 {
                            if e.updated_by_steam {
                                facts.push(format!("Updated {}", fmt_date(e.updated)));
                                tip.push("Updated: when the installed version was published on the Workshop.".to_owned());
                            } else {
                                facts.push(format!("Changed {}", fmt_date(e.updated)));
                                tip.push("Changed: the newest file in the folder.".to_owned());
                            }
                        }
                        if sort == ModSort::Used {
                            facts.push(match e.last_used {
                                0 => "Never launched".to_owned(),
                                t => format!("Launched {}", fmt_date(t)),
                            });
                        }
                        let line = fact_line(ui, if facts.is_empty() { " ".to_owned() } else { facts.join(" · ") });
                        if !tip.is_empty() {
                            line.on_hover_text(tip.join("\n"));
                        }
                    });
                });

                let description = cfg.mod_descriptions.get(&e.key).unwrap_or(&m.summary);
                summary_lines(ui, description, inner);

                if e.used_by.is_empty() {
                    fact_line(ui, "Not in any profile".to_owned());
                } else {
                    fact_line(ui, format!("Used by: {}", e.used_by)).on_hover_text(format!("Profiles that list this mod: {}", e.used_by));
                }

                ui.horizontal_wrapped(|ui| {
                    ui.spacing_mut().item_spacing = vec2(6.0, 6.0);
                    theme::chip(ui, m.source.label(), theme::MUTED);
                    if e.in_profile {
                        theme::chip(ui, "In profile", theme::GOOD);
                    }
                    if !m.has_data {
                        theme::chip(ui, "No Data folder", theme::WARN).on_hover_text(
                            "This mod has no Data folder, so the game will find nothing to load from it.",
                        );
                    }
                    if let Some(id) = e.steam_id {
                        let status = ws.known_status(id).filter(|s| s.downloading).cloned();
                        if let Some(status) = status {
                            state_chip(ui, &status, false);
                        } else if e.update_available {
                            let chip = theme::chip(ui, "Update available", theme::WARN);
                            if ws.is_connected() {
                                chip.on_hover_text("Steam has a newer version of this mod than the one installed.");
                                let link =
                                    Button::new(RichText::new("Update now").size(12.0).color(theme::ACCENT_HI)).frame(false);
                                if ui.add(link).clicked() {
                                    act = Some(Act::Download(id));
                                }
                            } else {
                                chip.on_hover_text(
                                    "Steam's records on this PC show a newer version than the one installed.\nConnect to Steam on the Workshop tab to update it from here, or let the Steam client update it.",
                                );
                            }
                        }
                    }
                    if let Some(change) = &e.change {
                        let chip = theme::chip(ui, format!("Updated  {}", icon::CLOSE), theme::GOOD)
                            .interact(Sense::click())
                            .on_hover_cursor(egui::CursorIcon::PointingHand)
                            .on_hover_text(change_text(change, &e.used_by));
                        if chip.clicked() {
                            act = Some(Act::DismissChange(e.key.clone()));
                        }
                    }
                    for tag in e.tags.iter().take(4) {
                        theme::chip(ui, tag, theme::ACCENT_HI);
                    }
                    if e.tags.len() > 4 {
                        theme::chip(ui, format!("+{}", e.tags.len() - 4), theme::ACCENT_HI).on_hover_text(e.tags[4..].join(", "));
                    }
                });

                ui.horizontal(|ui| {
                    if e.in_profile {
                        if theme::button(ui, "Remove from profile").clicked() {
                            act = Some(Act::Remove(m.path.clone()));
                        }
                    } else if theme::primary_button(ui, "Add to profile").clicked() {
                        act = Some(Act::Add(m.path.clone()));
                    }
                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        ui.spacing_mut().item_spacing.x = 2.0;
                        if theme::icon_button(ui, icon::FOLDER_OPEN, "Open folder").clicked() {
                            act = Some(Act::Open(m.path.clone()));
                        }
                        if let Some(id) = workshop_id {
                            if theme::icon_button(ui, icon::GLOBE, "Open its Workshop page in Steam").clicked() {
                                act = Some(Act::OpenSteam(id));
                            }
                        }
                        if theme::icon_button(ui, icon::RENAME, "Edit name, description and tags").clicked() {
                            act = Some(Act::Edit(m.path.clone()));
                        }
                    });
                });
            });
        });
    act
}

// ------------------------------------------------------------------ workshop

fn workshop_tab(app: &mut App, st: &mut State, ui: &mut egui::Ui) -> Option<Act> {
    match st.ws.status().clone() {
        Status::Connected => connected(app, st, ui),
        status => {
            not_connected(app, st, ui, &status);
            None
        }
    }
}

/// The two things that work without a Steam connection.
fn steam_links(app: &mut App, st: &mut State, ui: &mut egui::Ui) {
    if theme::button(ui, &format!("{}  Browse the Workshop in Steam", icon::GLOBE)).clicked() {
        if let Err(e) = workshop::open_workshop_home() {
            app.err(e);
        }
    }
    if theme::button(ui, "Open a Workshop item…").clicked() {
        st.open_item = Some(String::new());
        st.open_item_focus = true;
    }
}

fn not_connected(app: &mut App, st: &mut State, ui: &mut egui::Ui, status: &Status) {
    egui::ScrollArea::vertical().id_salt("ws_offline").auto_shrink([false, false]).show(ui, |ui| {
        theme::card(ui, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(30.0);
                ui.label(RichText::new(icon::GLOBE).size(34.0).color(theme::FAINT));
                ui.add_space(4.0);
                ui.label(theme::title("Steam Workshop"));
                ui.label(theme::muted(
                    "Connect to see the mods you are subscribed to, and to find and subscribe to new ones without leaving the launcher.",
                ));
                ui.add_space(10.0);
                match status {
                    Status::Connecting => {
                        ui.add(egui::Spinner::new().size(18.0));
                        ui.label(theme::muted("Connecting to Steam…"));
                    }
                    Status::Failed(failure) => {
                        ui.label(RichText::new(format!("{}  {}", icon::WARNING, failure.reason)).color(theme::BAD));
                        if !failure.detail.is_empty() {
                            ui.label(theme::faint(&failure.detail));
                        }
                        ui.add_space(4.0);
                        if theme::primary_button(ui, "Retry").clicked() {
                            st.ws.connect(ui.ctx());
                        }
                    }
                    _ => {
                        if theme::primary_button(ui, "Connect to Steam").clicked() {
                            st.ws.connect(ui.ctx());
                        }
                    }
                }
                ui.add_space(6.0);
                ui.label(theme::faint("While the launcher is connected, Steam shows Empire at War as running."));
                ui.add_space(30.0);
            });
        });
        ui.add_space(14.0);
        theme::card(ui, |ui| {
            theme::overline(ui, "Without connecting");
            ui.label(theme::muted(
                "These open in the Steam client. Mods you subscribe to there appear under Installed after a rescan.",
            ));
            ui.add_space(2.0);
            ui.horizontal(|ui| steam_links(app, st, ui));
        });
        ui.add_space(24.0);
    });
}

fn connected(app: &mut App, st: &mut State, ui: &mut egui::Ui) -> Option<Act> {
    let subscribed_label = match st.ws.subscribed() {
        Some(ids) => format!("Subscribed  {}", ids.len()),
        None => "Subscribed".to_owned(),
    };
    let mut disconnect = false;
    ui.horizontal(|ui| {
        segmented(ui, &mut st.ws_tab, &[(WsTab::Subscribed, subscribed_label), (WsTab::Browse, "Browse".to_owned())]);
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if theme::button(ui, "Disconnect").clicked() {
                disconnect = true;
            }
            if theme::icon_button(ui, icon::GLOBE, "Browse the Workshop in Steam").clicked() {
                if let Err(e) = workshop::open_workshop_home() {
                    app.err(e);
                }
            }
            theme::chip(ui, "Connected to Steam", theme::GOOD)
                .on_hover_text("While the launcher is connected, Steam shows Empire at War as running.");
        });
    });
    if disconnect {
        st.ws.disconnect();
        return None;
    }
    match st.ws_tab {
        WsTab::Subscribed => subscribed_tab(app, st, ui),
        WsTab::Browse => browse_tab(app, st, ui),
    }
}

fn subscribed_tab(app: &mut App, st: &mut State, ui: &mut egui::Ui) -> Option<Act> {
    let Some(mut ids) = st.ws.subscribed().map(<[u64]>::to_vec) else {
        theme::card(ui, |ui| {
            ui.vertical_centered(|ui| {
                ui.add_space(36.0);
                ui.add(egui::Spinner::new().size(18.0));
                ui.label(theme::muted("Asking Steam for your subscriptions…"));
                ui.add_space(36.0);
            });
        });
        return None;
    };
    ids.sort_by_cached_key(|id| st.ws.title(*id).to_lowercase());

    let active_mods = app.cfg.active().mods.clone();
    let profile_name = app.cfg.active().name.clone();
    ui.horizontal(|ui| {
        let count = if ids.len() == 1 { "1 subscription".to_owned() } else { format!("{} subscriptions", ids.len()) };
        ui.label(theme::faint(format!("{count} · adding to profile \"{profile_name}\"")));
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if theme::icon_button(ui, icon::REFRESH, "Ask Steam again").clicked() {
                st.ws.refresh_subscribed();
            }
        });
    });

    let mut act = None;
    egui::ScrollArea::vertical().id_salt("ws_subscribed").auto_shrink([false, false]).show(ui, |ui| {
        if ids.is_empty() {
            theme::card(ui, |ui| {
                theme::empty_state(
                    ui,
                    icon::LIBRARY,
                    "No subscriptions yet",
                    "Find mods under Browse. Subscribing downloads them and adds them to Installed.",
                );
                ui.vertical_centered(|ui| {
                    if theme::primary_button(ui, "Browse the Workshop").clicked() {
                        st.ws_tab = WsTab::Browse;
                    }
                    ui.add_space(16.0);
                });
            });
            return;
        }
        let offline = Offline { local: st.local.as_ref(), sizes: &st.sizes };
        card_grid(ui, &ids, |ui, id, card_w| {
            let card = workshop_card(ui, &mut app.tex, &mut st.ws, &offline, *id, WsTab::Subscribed, &active_mods, card_w);
            if let Some(a) = card {
                act = Some(a);
            }
        });
        ui.add_space(24.0);
    });
    act
}

fn browse_tab(app: &mut App, st: &mut State, ui: &mut egui::Ui) -> Option<Act> {
    if !st.ws.browse().started {
        st.ws.search(&st.browse_text, st.browse_sort, 1);
    }
    let browse = st.ws.browse().clone();

    // ---- search row
    let mut submit: Option<u32> = None;
    ui.horizontal(|ui| {
        let edit = ui.add(
            egui::TextEdit::singleline(&mut st.browse_text)
                .desired_width(260.0)
                .hint_text(format!("{}  Search the Workshop", icon::SEARCH)),
        );
        if edit.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)) {
            submit = Some(1);
        }
        if !st.browse_text.is_empty() && theme::icon_button(ui, icon::CLOSE, "Clear search").clicked() {
            st.browse_text.clear();
            submit = Some(1);
        }
        if theme::button(ui, "Search").clicked() {
            submit = Some(1);
        }
        ui.add_space(4.0);
        egui::ComboBox::from_id_salt("ws_sort").selected_text(st.browse_sort.label()).show_ui(ui, |ui| {
            for sort in Sort::ALL {
                if ui.selectable_label(st.browse_sort == sort, sort.label()).clicked() && st.browse_sort != sort {
                    st.browse_sort = sort;
                    submit = Some(1);
                }
            }
        });
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            if let Some(page) = pager(ui, &browse) {
                submit = Some(page);
            }
        });
    });

    let summary = if browse.loading {
        "Asking Steam…".to_owned()
    } else if browse.error.is_some() {
        String::new()
    } else if browse.text.is_empty() {
        format!("{} items · {}", fmt_count(browse.total as u64), browse.sort.label().to_lowercase())
    } else {
        format!("{} results for \"{}\" · {}", fmt_count(browse.total as u64), browse.text, browse.sort.label().to_lowercase())
    };
    ui.label(theme::faint(summary));

    // ---- results
    let active_mods = app.cfg.active().mods.clone();
    let mut act = None;
    egui::ScrollArea::vertical()
        // A new page starts at the top.
        .id_salt(("ws_browse", browse.page))
        .auto_shrink([false, false])
        .show(ui, |ui| {
            if let Some(error) = &browse.error {
                theme::card(ui, |ui| {
                    theme::empty_state(ui, icon::WARNING, "The search didn't work", error);
                    ui.vertical_centered(|ui| {
                        if theme::primary_button(ui, "Try again").clicked() {
                            submit = Some(browse.page.max(1));
                        }
                        ui.add_space(16.0);
                    });
                });
                return;
            }
            if browse.items.is_empty() {
                if browse.loading {
                    ui.vertical_centered(|ui| {
                        ui.add_space(48.0);
                        ui.add(egui::Spinner::new().size(18.0));
                        ui.label(theme::muted("Asking Steam…"));
                    });
                } else {
                    theme::empty_state(ui, icon::SEARCH, "Nothing found", "Try different words, or clear the search.");
                }
                return;
            }
            let offline = Offline { local: st.local.as_ref(), sizes: &st.sizes };
            card_grid(ui, &browse.items, |ui, id, card_w| {
                let card = workshop_card(ui, &mut app.tex, &mut st.ws, &offline, *id, WsTab::Browse, &active_mods, card_w);
                if let Some(a) = card {
                    act = Some(a);
                }
            });
            ui.add_space(4.0);
            ui.horizontal(|ui| {
                ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                    if let Some(page) = pager(ui, &browse) {
                        submit = Some(page);
                    }
                });
            });
            ui.add_space(24.0);
        });

    if let Some(page) = submit {
        st.ws.search(&st.browse_text, st.browse_sort, page);
    }
    act
}

/// Previous / page number / Next, drawn in a right-to-left layout. Returns the page to load.
fn pager(ui: &mut Ui, browse: &workshop::Browse) -> Option<u32> {
    let mut go = None;
    let (page, pages) = (browse.page.max(1), browse.pages());
    let idle = !browse.loading && browse.error.is_none();
    if ui.add_enabled(idle && page < pages, Button::new("Next").corner_radius(CornerRadius::same(8))).clicked() {
        go = Some(page + 1);
    }
    ui.label(theme::muted(format!("Page {} of {}", fmt_count(page as u64), fmt_count(pages as u64))));
    if ui.add_enabled(idle && page > 1, Button::new("Previous").corner_radius(CornerRadius::same(8))).clicked() {
        go = Some(page - 1);
    }
    if browse.loading {
        ui.add(egui::Spinner::new().size(14.0));
    }
    go
}

fn state_chip(ui: &mut Ui, status: &ItemStatus, busy: bool) {
    if busy {
        theme::chip(ui, "Working…", theme::MUTED);
    } else if status.downloading {
        match status.percent() {
            Some(p) => theme::chip(ui, format!("Downloading {p}%"), theme::ACCENT_HI),
            None => theme::chip(ui, "Queued", theme::ACCENT_HI),
        };
    } else if status.installed && status.needs_update {
        theme::chip(ui, "Update available", theme::WARN)
            .on_hover_text("Steam has a newer version of this mod than the one installed.");
    } else if status.installed {
        theme::chip(ui, "Installed", theme::GOOD);
    } else if status.subscribed {
        theme::chip(ui, "Not downloaded", theme::WARN);
    }
}

fn download_progress(ui: &mut Ui, status: &ItemStatus) {
    match status.percent() {
        Some(p) => {
            let text = format!("{p}% of {}", theme::fmt_bytes(status.bytes_total));
            ui.add(
                egui::ProgressBar::new(p as f32 / 100.0)
                    .desired_width(170.0)
                    .desired_height(28.0)
                    .corner_radius(CornerRadius::same(8))
                    .fill(theme::ACCENT.gamma_multiply(0.55))
                    .text(RichText::new(text).size(12.0).color(theme::TEXT)),
            );
        }
        None => {
            ui.add_enabled(false, Button::new("Queued…").corner_radius(CornerRadius::same(8)));
        }
    }
}

/// What the launcher knows about Workshop items without asking Steam.
struct Offline<'a> {
    local: Option<&'a LocalState>,
    sizes: &'a Sizes,
}

/// One Workshop item. `mode` decides the actions: managing a subscription, or subscribing.
#[allow(clippy::too_many_arguments)]
fn workshop_card(
    ui: &mut Ui,
    tex: &mut Textures,
    ws: &mut Workshop,
    offline: &Offline,
    id: u64,
    mode: WsTab,
    active_mods: &[PathBuf],
    width: f32,
) -> Option<Act> {
    let title = ws.title(id);
    let (description, subscribers, rating) = match ws.item(id) {
        Some(item) => (item.description.clone(), item.subscribers, item.rating_percent()),
        None => (String::new(), None, None),
    };
    let known = ws.known_status(id).is_some();
    let mut status = ws.item_status(id);
    status.subscribed |= mode == WsTab::Subscribed || ws.is_subscribed(id);
    let busy = ws.is_busy(id);
    let folder = status.folder.clone().filter(|_| status.installed);
    let in_profile = folder.as_ref().is_some_and(|f| active_mods.iter().any(|p| same_path(p, f)));
    let record = offline.local.and_then(|l| l.item(id));
    // Steam hasn't said anything about the item yet: go by its records on this PC.
    if !known && record.is_some_and(|r| r.update_available()) {
        status.installed = true;
        status.needs_update = true;
    }
    // The folder as walked by the launcher, else Steam's own count. (The size the Steam
    // API reports is a 32-bit number, so it is wrong for mods over 4 GB.)
    let size = folder
        .as_ref()
        .and_then(|f| offline.sizes.get(&mod_key(f)))
        .map(|m| m.bytes)
        .or(record.filter(|r| r.installed && r.size > 0).map(|r| r.size));
    let updated = record.filter(|r| r.installed).map_or(0, |r| r.time_updated);

    let mut act = None;
    egui::Frame::new()
        .fill(theme::CARD)
        .stroke(Stroke::new(1.0_f32, theme::BORDER))
        .corner_radius(CornerRadius::same(12))
        .show(ui, |ui| {
            let w = width - 2.0;
            ui.set_width(w);
            ui.spacing_mut().item_spacing.y = 0.0;

            // Preview image: fetched and decoded only once the card is on screen.
            let (art, _) = ui.allocate_exact_size(vec2(w, 130.0), Sense::hover());
            if ui.is_rect_visible(art) {
                let top = CornerRadius { nw: 11, ne: 11, sw: 0, se: 0 };
                match ws.preview(ui.ctx(), id).and_then(|p| tex.get(ui.ctx(), &p)) {
                    Some(t) => paint_cover(ui, &t, art, top),
                    None => art_placeholder(ui, art, &title, top),
                }
            }

            egui::Frame::new().inner_margin(Margin::same(14)).show(ui, |ui| {
                let inner = w - 28.0;
                ui.set_width(inner);
                ui.spacing_mut().item_spacing.y = 8.0;

                ui.vertical(|ui| {
                    ui.spacing_mut().item_spacing.y = 1.0;
                    ui.add(egui::Label::new(theme::strong(&title)).truncate());
                    let mut facts: Vec<String> = Vec::new();
                    if let Some(n) = subscribers {
                        facts.push(format!("{} subscribers", fmt_count(n)));
                    }
                    if let Some(p) = rating {
                        facts.push(format!("{p}% positive"));
                    }
                    if mode == WsTab::Subscribed {
                        facts.clear();
                        facts.push(format!("Workshop {id}"));
                        if let Some(bytes) = size.filter(|_| status.installed) {
                            facts.push(format!("{} on disk", theme::fmt_bytes(bytes)));
                        }
                        if status.installed && updated > 0 {
                            facts.push(format!("Updated {}", fmt_date(updated)));
                        }
                    } else if facts.is_empty() {
                        facts.push(format!("Workshop {id}"));
                    }
                    let line = ui.add(egui::Label::new(theme::faint(facts.join(" · "))).truncate());
                    if let Some(f) = &folder {
                        line.on_hover_text(f.display().to_string());
                    }
                });

                summary_lines(ui, &description, inner);

                ui.horizontal_wrapped(|ui| {
                    ui.spacing_mut().item_spacing = vec2(6.0, 6.0);
                    if mode == WsTab::Browse && status.subscribed {
                        theme::chip(ui, "Subscribed", theme::ACCENT_HI);
                    }
                    state_chip(ui, &status, busy);
                    if in_profile {
                        theme::chip(ui, "In profile", theme::GOOD);
                    }
                    if status.subscribed && status.needs_update && !status.downloading && !busy {
                        let link = Button::new(RichText::new("Update now").size(12.0).color(theme::ACCENT_HI)).frame(false);
                        if ui.add(link).clicked() {
                            act = Some(Act::Download(id));
                        }
                    }
                    // Keeps the row's height when there is no chip to show.
                    ui.allocate_exact_size(vec2(0.0, 20.0), Sense::hover());
                });

                ui.horizontal(|ui| {
                    if busy {
                        ui.add_enabled(false, Button::new("Working…").corner_radius(CornerRadius::same(8)));
                    } else if !status.subscribed {
                        if theme::primary_button(ui, "Subscribe").clicked() {
                            act = Some(Act::Subscribe(id));
                        }
                    } else if status.downloading {
                        download_progress(ui, &status);
                    } else if mode == WsTab::Browse {
                        if theme::button(ui, "Unsubscribe").clicked() {
                            act = Some(Act::AskUnsubscribe(id));
                        }
                    } else if let Some(f) = &folder {
                        if in_profile {
                            if theme::button(ui, "Remove from profile").clicked() {
                                act = Some(Act::Remove(f.clone()));
                            }
                        } else if theme::primary_button(ui, "Add to profile").clicked() {
                            act = Some(Act::Add(f.clone()));
                        }
                    } else if theme::primary_button(ui, "Download").clicked() {
                        act = Some(Act::Download(id));
                    }

                    ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                        ui.spacing_mut().item_spacing.x = 2.0;
                        if mode == WsTab::Subscribed {
                            ui.add_enabled_ui(!busy, |ui| {
                                if theme::icon_button(ui, icon::DELETE, "Unsubscribe").clicked() {
                                    act = Some(Act::AskUnsubscribe(id));
                                }
                            });
                            ui.add_enabled_ui(folder.is_some(), |ui| {
                                if theme::icon_button(ui, icon::FOLDER_OPEN, "Open folder").clicked() {
                                    if let Some(f) = &folder {
                                        act = Some(Act::Open(f.clone()));
                                    }
                                }
                            });
                        }
                        if theme::icon_button(ui, icon::GLOBE, "Open in Steam").clicked() {
                            act = Some(Act::OpenSteam(id));
                        }
                    });
                });
            });
        });
    act
}

// ------------------------------------------------------------------ dialogs

/// Small "Reset" button on the right of a field's label. Returns true when pressed.
fn field_header(ui: &mut Ui, label: &str, can_reset: bool, tip: &str) -> bool {
    let mut reset = false;
    ui.horizontal(|ui| {
        theme::overline(ui, label);
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            let button = Button::new(RichText::new("Reset").size(12.0).color(theme::MUTED)).small();
            reset = ui.add_enabled(can_reset, button).on_hover_text(tip).clicked();
        });
    });
    reset
}

/// A tag as it is stored: single spaces, no commas, not too long. `None` when nothing is left.
fn clean_tag(text: &str) -> Option<String> {
    let joined = text.replace([',', ';'], " ").split_whitespace().collect::<Vec<_>>().join(" ");
    let tag: String = joined.chars().take(TAG_MAX_CHARS).collect();
    let tag = tag.trim_end().to_owned();
    (!tag.is_empty()).then_some(tag)
}

/// Adds a tag unless the mod already has it. A tag that exists on another mod keeps that
/// mod's spelling, so "Star Wars" and "star wars" don't become two tags.
fn add_tag(tags: &mut Vec<String>, text: &str, known: &[String]) {
    let Some(tag) = clean_tag(text) else { return };
    let lower = tag.to_lowercase();
    if tags.iter().any(|t| t.to_lowercase() == lower) {
        return;
    }
    tags.push(known.iter().find(|k| k.to_lowercase() == lower).cloned().unwrap_or(tag));
}

/// Every tag in use on any mod, most used first.
fn known_tags(cfg: &Config) -> Vec<String> {
    let mut counts: BTreeMap<String, (String, usize)> = BTreeMap::new();
    for tag in cfg.mod_tags.values().flatten() {
        counts.entry(tag.to_lowercase()).or_insert_with(|| (tag.clone(), 0)).1 += 1;
    }
    let mut tags: Vec<(String, usize)> = counts.into_values().collect();
    tags.sort_by(|a, b| b.1.cmp(&a.1).then_with(|| a.0.to_lowercase().cmp(&b.0.to_lowercase())));
    tags.into_iter().map(|(tag, _)| tag).collect()
}

/// Small pill button used for tags in the edit dialog.
fn tag_button(ui: &mut Ui, label: String, color: Color32) -> Response {
    ui.add(
        Button::new(RichText::new(label).size(12.0).color(color))
            .fill(color.gamma_multiply(0.14))
            .stroke(Stroke::NONE)
            .corner_radius(CornerRadius::same(255))
            .small(),
    )
}

fn edit_modal(app: &mut App, st: &mut State, ctx: &egui::Context) {
    let Some(edit) = st.edit.as_mut() else { return };
    let key = mod_key(&edit.path);
    let (own_name, own_summary) = match app.find_mod(&edit.path) {
        Some(m) => (m.name.clone(), m.summary.split_whitespace().collect::<Vec<_>>().join(" ")),
        None => (edit.path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default(), String::new()),
    };
    let summary_hint = if own_summary.is_empty() {
        "No description.".to_owned()
    } else if own_summary.chars().count() > 240 {
        format!("{}…", own_summary.chars().take(240).collect::<String>().trim_end())
    } else {
        own_summary.clone()
    };
    let known = known_tags(&app.cfg);
    let focus = std::mem::take(&mut st.edit_focus);

    let mut save = false;
    let mut cancel = false;
    let modal = egui::Modal::new(egui::Id::new("edit_mod_entry")).show(ctx, |ui| {
        ui.set_width(460.0);
        ui.label(theme::title("Edit entry"));
        ui.label(theme::faint(
            "This changes how the mod is listed in the launcher. It does not touch the mod's files.",
        ));
        ui.add_space(6.0);

        if field_header(ui, "Name", !edit.name.is_empty(), "Use the mod's own name") {
            edit.name.clear();
        }
        let name = ui.add(egui::TextEdit::singleline(&mut edit.name).desired_width(f32::INFINITY).hint_text(own_name.clone()));
        if focus {
            name.request_focus();
        }
        if name.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter)) {
            save = true;
        }
        ui.add_space(6.0);

        if field_header(ui, "Description", !edit.description.is_empty(), "Use the mod's own description") {
            edit.description.clear();
        }
        egui::ScrollArea::vertical().id_salt("edit_mod_description").max_height(150.0).show(ui, |ui| {
            ui.add(
                egui::TextEdit::multiline(&mut edit.description)
                    .desired_width(f32::INFINITY)
                    .desired_rows(4)
                    .hint_text(summary_hint.clone()),
            );
        });
        ui.label(theme::faint("Leave a field empty to use the mod's own text, shown in grey."));
        ui.add_space(6.0);

        // ---- tags
        if field_header(ui, "Tags", !edit.tags.is_empty(), "Remove every tag") {
            edit.tags.clear();
        }
        let mut remove: Option<usize> = None;
        ui.horizontal_wrapped(|ui| {
            ui.spacing_mut().item_spacing = vec2(6.0, 6.0);
            for (i, tag) in edit.tags.iter().enumerate() {
                let chip = tag_button(ui, format!("{tag}  {}", icon::CLOSE), theme::ACCENT_HI);
                if chip.on_hover_text("Remove this tag").clicked() {
                    remove = Some(i);
                }
            }
            let input = ui.add(
                egui::TextEdit::singleline(&mut edit.tag_input)
                    .desired_width(150.0)
                    .char_limit(TAG_MAX_CHARS + 1)
                    .hint_text("Add a tag"),
            );
            // Enter or a comma finishes a tag; the field keeps the focus for the next one.
            let entered = input.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
            if entered || edit.tag_input.contains([',', ';']) {
                let text = std::mem::take(&mut edit.tag_input);
                for part in text.split([',', ';']) {
                    add_tag(&mut edit.tags, part, &known);
                }
                input.request_focus();
            }
        });
        if let Some(i) = remove {
            edit.tags.remove(i);
        }
        // Tags already used on other mods, narrowed by what is being typed.
        let typed = edit.tag_input.trim().to_lowercase();
        let suggestions: Vec<&String> = known
            .iter()
            .filter(|k| !edit.tags.iter().any(|t| t.to_lowercase() == k.to_lowercase()))
            .filter(|k| typed.is_empty() || k.to_lowercase().contains(&typed))
            .take(10)
            .collect();
        if suggestions.is_empty() {
            ui.label(theme::faint("Press Enter or type a comma to add a tag. Tags can be filtered on in the mod list."));
        } else {
            let mut pick: Option<String> = None;
            ui.horizontal_wrapped(|ui| {
                ui.spacing_mut().item_spacing = vec2(6.0, 6.0);
                ui.label(theme::faint("Used on other mods:"));
                for tag in suggestions {
                    if tag_button(ui, format!("{}  {tag}", icon::ADD), theme::MUTED).clicked() {
                        pick = Some(tag.clone());
                    }
                }
            });
            if let Some(tag) = pick {
                add_tag(&mut edit.tags, &tag, &known);
                edit.tag_input.clear();
            }
        }
        ui.add_space(8.0);

        ui.horizontal(|ui| {
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if theme::primary_button(ui, "Save").clicked() {
                    save = true;
                }
                if theme::button(ui, "Cancel").clicked() {
                    cancel = true;
                }
            });
        });
    });

    if save {
        // Empty text, or the mod's own text typed back in, means "no override".
        let name = edit.name.trim();
        let description = edit.description.trim();
        let mut changed = if name.is_empty() || name == own_name {
            app.cfg.mod_names.remove(&key).is_some()
        } else {
            app.cfg.mod_names.insert(key.clone(), name.to_owned()).as_deref() != Some(name)
        };
        changed |= if description.is_empty() || description == own_summary {
            app.cfg.mod_descriptions.remove(&key).is_some()
        } else {
            app.cfg.mod_descriptions.insert(key.clone(), description.to_owned()).as_deref() != Some(description)
        };
        // A tag still sitting in the input counts too.
        let mut tags = edit.tags.clone();
        add_tag(&mut tags, &edit.tag_input, &known);
        changed |= if tags.is_empty() {
            app.cfg.mod_tags.remove(&key).is_some()
        } else {
            app.cfg.mod_tags.insert(key, tags.clone()) != Some(tags)
        };
        if changed {
            app.touch();
        }
        st.edit = None;
    } else if cancel || modal.should_close() {
        st.edit = None;
    }
}

fn open_item_modal(app: &mut App, st: &mut State, ctx: &egui::Context) {
    let Some(text) = st.open_item.as_mut() else { return };
    let focus = std::mem::take(&mut st.open_item_focus);
    let id = workshop::parse_item_id(text);

    let mut open = false;
    let mut cancel = false;
    let modal = egui::Modal::new(egui::Id::new("open_workshop_item")).show(ctx, |ui| {
        ui.set_width(460.0);
        ui.label(theme::title("Open a Workshop item"));
        ui.label(theme::muted("Paste a Workshop link or the item's number. It opens in the Steam client."));
        ui.add_space(4.0);
        let edit = ui.add(
            egui::TextEdit::singleline(text)
                .desired_width(f32::INFINITY)
                .hint_text("https://steamcommunity.com/sharedfiles/filedetails/?id=…"),
        );
        if focus {
            edit.request_focus();
        }
        let enter = edit.lost_focus() && ui.input(|i| i.key_pressed(egui::Key::Enter));
        match id {
            Some(id) => ui.label(theme::faint(format!("Workshop item {id}"))),
            None if text.trim().is_empty() => ui.label(theme::faint(" ")),
            None => ui.label(RichText::new("That doesn't look like a Workshop link or number.").size(12.0).color(theme::BAD)),
        };
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                let clicked = ui.add_enabled_ui(id.is_some(), |ui| theme::primary_button(ui, "Open in Steam")).inner.clicked();
                if (clicked || enter) && id.is_some() {
                    open = true;
                }
                if theme::button(ui, "Cancel").clicked() {
                    cancel = true;
                }
            });
        });
    });

    if open {
        if let Some(id) = id {
            if let Err(e) = workshop::open_item_page(id) {
                app.err(e);
            }
        }
        st.open_item = None;
    } else if cancel || modal.should_close() {
        st.open_item = None;
    }
}

fn unsubscribe_modal(app: &mut App, st: &mut State, ctx: &egui::Context) {
    let Some(id) = st.unsubscribe else { return };
    let title = st.ws.title(id);
    let folder = st.ws.item_status(id).folder;
    let used_by: Vec<String> = match &folder {
        Some(f) => app
            .cfg
            .profiles
            .iter()
            .filter(|p| p.mods.iter().any(|m| same_path(m, f)))
            .map(|p| p.name.clone())
            .collect(),
        None => Vec::new(),
    };

    let mut confirm = false;
    let mut cancel = false;
    let modal = egui::Modal::new(egui::Id::new("unsubscribe_workshop_item")).show(ctx, |ui| {
        ui.set_width(440.0);
        ui.label(theme::title(format!("Unsubscribe from {title}?")));
        ui.label(theme::muted(
            "Steam removes the mod's files from this PC, which can wait until the launcher disconnects. Profiles that list the mod will show it as missing.",
        ));
        if !used_by.is_empty() {
            ui.label(RichText::new(format!("{}  Used by: {}", icon::WARNING, used_by.join(", "))).color(theme::WARN));
        }
        ui.add_space(8.0);
        ui.horizontal(|ui| {
            ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
                if theme::danger_button(ui, "Unsubscribe").clicked() {
                    confirm = true;
                }
                if theme::button(ui, "Cancel").clicked() {
                    cancel = true;
                }
            });
        });
    });

    if confirm {
        if st.ws.is_connected() {
            st.ws.unsubscribe(id);
        } else {
            app.err("Not connected to Steam any more.");
        }
        st.unsubscribe = None;
    } else if cancel || modal.should_close() {
        st.unsubscribe = None;
    }
}

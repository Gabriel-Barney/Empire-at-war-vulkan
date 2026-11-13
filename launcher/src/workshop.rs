//! Steam Workshop for Empire at War through the Steamworks API.
//!
//! Everything that talks to Steam lives here, behind [`Workshop`]. Pages never see a
//! `steamworks` type. The rules this module keeps:
//!
//! * Nothing touches Steam until [`Workshop::connect`] is called. While connected Steam shows
//!   the user as playing Empire at War, so connecting is always the user's choice.
//! * The Steam client lives in a **helper process**: the launcher's own exe started again
//!   with [`HELPER_ARG`] (see `main.rs`), with no window. Steam counts the game as running
//!   for as long as a process that initialised the Steam API is alive. Shutting the API
//!   down is not enough (measured: the "Running" flag stays set until the process exits).
//!   So disconnecting means ending that process, and it must not be the launcher. It also
//!   keeps anything the Steam DLL does out of the launcher's own process.
//! * The two talk over the helper's stdin/stdout, one JSON message per line. In the launcher
//!   a reader and a writer thread sit on the pipes, so the UI thread never waits on Steam;
//!   results arrive through a channel and wake the UI with `request_repaint`.
//! * `steam_api64.dll` is a delay-loaded import (see `build.rs`). Only the helper loads it,
//!   by full path from the exe's own folder, right before the first Steam call. A missing
//!   DLL is an ordinary error message, and the launcher itself never needs it to start.
//! * Preview images come from the URL Steam returned for the item, only from Steam's own
//!   hosts, over HTTPS, size-limited, a few at a time, cached on disk.

use crate::config;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::io::{BufRead, Write};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender, TryRecvError};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};
use steamworks as sw;

/// Star Wars: Empire at War (Gold Pack) on Steam.
pub const APP_ID: u32 = 32470;

/// First command-line argument that makes the exe run as the Steam helper instead of
/// opening the launcher. `main` checks for it before anything else.
pub const HELPER_ARG: &str = "--steam-workshop-helper";
/// Starts every message the helper prints. The Steam DLL writes to stdout too.
const MESSAGE_MARK: &str = "@@workshop ";

const DLL_NAME: &str = "steam_api64.dll";
/// Steam's page size for Workshop queries.
pub const PAGE_SIZE: u32 = 50;
/// "Most popular" ranks by subscriptions gained over this many days.
const TREND_DAYS: u32 = 30;
const CONNECT_TIMEOUT: Duration = Duration::from_secs(30);
const REQUEST_TIMEOUT: Duration = Duration::from_secs(25);
const PREVIEW_MAX_BYTES: u64 = 4 * 1024 * 1024;
const PREVIEW_WORKERS: usize = 3;
const PREVIEW_EXT: [&str; 4] = ["jpg", "png", "gif", "webp"];
/// Hosts Steam serves Workshop preview images from.
const PREVIEW_HOSTS: [&str; 6] = [
    "steamusercontent.com",
    "steamusercontent-a.akamaihd.net",
    "steamuserimages-a.akamaihd.net",
    "steamstatic.com",
    "steamcommunity.com",
    "steampowered.com",
];

// ------------------------------------------------------------------ public types

/// Why connecting failed, in words for the user.
#[derive(Serialize, Deserialize, Clone, Debug, PartialEq, Eq)]
pub struct Failure {
    pub reason: String,
    /// What Steam (or Windows) actually said. May be empty.
    pub detail: String,
}

impl Failure {
    fn new(reason: impl Into<String>, detail: impl Into<String>) -> Self {
        Self { reason: reason.into(), detail: detail.into() }
    }
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub enum Status {
    #[default]
    Disconnected,
    Connecting,
    Connected,
    Failed(Failure),
}

/// One Workshop item as Steam describes it.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq)]
pub struct Item {
    pub id: u64,
    pub title: String,
    /// Plain text: markup removed, whitespace collapsed, shortened.
    pub description: String,
    pub preview_url: String,
    /// Current subscriber count, when Steam reports it.
    pub subscribers: Option<u64>,
    pub votes_up: u32,
    pub votes_down: u32,
    /// Steam's rating score, 0 to 1.
    pub score: f32,
    /// Unix seconds of the last update.
    pub updated: i64,
    pub tags: Vec<String>,
}

impl Item {
    /// Share of positive votes in percent, once there are enough votes to mean something.
    pub fn rating_percent(&self) -> Option<u32> {
        let total = self.votes_up + self.votes_down;
        (total >= 5).then(|| (self.votes_up as f32 * 100.0 / total as f32).round() as u32)
    }
}

/// What Steam has on disk for an item on this PC.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq, Eq)]
pub struct ItemStatus {
    pub subscribed: bool,
    pub installed: bool,
    pub needs_update: bool,
    /// Downloading now, or queued to.
    pub downloading: bool,
    pub bytes_done: u64,
    pub bytes_total: u64,
    pub size_on_disk: u64,
    /// The install folder: this is the mod's path once it is installed.
    pub folder: Option<PathBuf>,
}

impl ItemStatus {
    /// Download progress in percent, when Steam knows the total.
    pub fn percent(&self) -> Option<u32> {
        (self.downloading && self.bytes_total > 0)
            .then(|| ((self.bytes_done as f64 / self.bytes_total as f64) * 100.0).clamp(0.0, 100.0) as u32)
    }

    /// Installed, current, and not being changed by Steam right now.
    pub fn ready(&self) -> bool {
        self.installed && !self.needs_update && !self.downloading
    }
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum Sort {
    #[default]
    Popular,
    Subscribed,
    Updated,
    Rated,
}

impl Sort {
    pub const ALL: [Sort; 4] = [Sort::Popular, Sort::Subscribed, Sort::Updated, Sort::Rated];

    pub fn label(self) -> &'static str {
        match self {
            Sort::Popular => "Most popular",
            Sort::Subscribed => "Most subscribed",
            Sort::Updated => "Recently updated",
            Sort::Rated => "Top rated",
        }
    }

    fn query_type(self) -> sw::UGCQueryType {
        match self {
            Sort::Popular => sw::UGCQueryType::RankedByTrend,
            Sort::Subscribed => sw::UGCQueryType::RankedByTotalUniqueSubscriptions,
            Sort::Updated => sw::UGCQueryType::RankedByLastUpdatedDate,
            Sort::Rated => sw::UGCQueryType::RankedByVote,
        }
    }
}

/// The Browse tab's current search and its results.
#[derive(Clone, Debug, Default)]
pub struct Browse {
    pub text: String,
    pub sort: Sort,
    /// 1-based.
    pub page: u32,
    pub loading: bool,
    pub error: Option<String>,
    /// Item ids on this page, in Steam's order. Look them up with [`Workshop::item`].
    pub items: Vec<u64>,
    /// Matching items across all pages.
    pub total: u32,
    /// False until the first search has been sent.
    pub started: bool,
}

impl Browse {
    pub fn pages(&self) -> u32 {
        self.total.div_ceil(PAGE_SIZE).max(1)
    }
}

/// Things the page has to act on after [`Workshop::poll`].
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Notice {
    Error(String),
    Info(String),
    /// Files on disk changed (an item finished installing or was removed): rescan mods.
    Rescan,
    /// The cached titles changed: see [`Workshop::titles`].
    TitlesChanged,
}

/// Title and description Steam has for a Workshop item, kept so that installed Workshop
/// mods have real names even when the launcher is not connected.
#[derive(Serialize, Deserialize, Clone, Debug, Default, PartialEq, Eq)]
pub struct Title {
    #[serde(default)]
    pub title: String,
    #[serde(default)]
    pub description: String,
}

/// Workshop id (as text) to title.
pub type Titles = BTreeMap<String, Title>;

// ------------------------------------------------------------------ files and links

/// `<data dir>\cache\workshop`, created on demand.
pub fn cache_dir() -> PathBuf {
    let dir = config::data_dir().join("cache").join("workshop");
    let _ = std::fs::create_dir_all(&dir);
    dir
}

fn titles_path() -> PathBuf {
    cache_dir().join("titles.json")
}

/// Reads the title cache. Missing or unreadable file = no titles.
pub fn load_titles() -> Titles {
    std::fs::read_to_string(titles_path())
        .ok()
        .and_then(|text| serde_json::from_str(text.trim_start_matches('\u{feff}')).ok())
        .unwrap_or_default()
}

fn save_titles(titles: &Titles) -> Result<(), String> {
    let path = titles_path();
    let tmp = path.with_extension("json.tmp");
    let text = serde_json::to_string_pretty(titles).map_err(|e| e.to_string())?;
    std::fs::write(&tmp, text).map_err(|e| format!("Couldn't write {}: {e}", tmp.display()))?;
    std::fs::rename(&tmp, &path).map_err(|e| format!("Couldn't replace {}: {e}", path.display()))
}

fn open_url(url: &str) -> Result<(), String> {
    std::process::Command::new("explorer.exe")
        .arg(url)
        .spawn()
        .map(|_| ())
        .map_err(|e| format!("Couldn't open Steam: {e}"))
}

/// Opens the game's Workshop front page in the Steam client. Works without connecting.
pub fn open_workshop_home() -> Result<(), String> {
    open_url(&format!("steam://url/SteamWorkshopPage/{APP_ID}"))
}

/// Opens one Workshop item's page in the Steam client. Works without connecting.
pub fn open_item_page(id: u64) -> Result<(), String> {
    open_url(&format!("steam://url/CommunityFilePage/{id}"))
}

/// Finds the item id in a pasted Workshop link (`...filedetails/?id=123`,
/// `steam://url/CommunityFilePage/123`) or a bare number.
pub fn parse_item_id(text: &str) -> Option<u64> {
    let text = text.trim();
    let digits = |s: &str| -> Option<u64> {
        let d: String = s.chars().take_while(|c| c.is_ascii_digit()).collect();
        d.parse::<u64>().ok().filter(|n| *n > 0)
    };
    if !text.is_empty() && text.chars().all(|c| c.is_ascii_digit()) {
        return digits(text);
    }
    let lower = text.to_ascii_lowercase();
    for marker in ["?id=", "&id=", "communityfilepage/"] {
        if let Some(pos) = lower.find(marker) {
            if let Some(id) = digits(&text[pos + marker.len()..]) {
                return Some(id);
            }
        }
    }
    None
}

/// Workshop descriptions are BBCode. This strips the tags, drops embedded image links,
/// collapses whitespace and shortens the result to `max_chars`.
pub fn plain_text(source: &str, max_chars: usize) -> String {
    const TAGS: [&str; 22] = [
        "h1", "h2", "h3", "b", "i", "u", "url", "img", "list", "olist", "*", "quote", "code", "strike", "spoiler",
        "noparse", "hr", "table", "tr", "td", "th", "previewyoutube",
    ];
    let mut out = String::with_capacity(source.len().min(4096));
    let mut rest = source;
    while let Some(open) = rest.find('[') {
        out.push_str(&rest[..open]);
        let after = &rest[open + 1..];
        let close = after.char_indices().take(200).find(|(_, c)| *c == ']').map(|(i, _)| i);
        let Some(close) = close else {
            out.push('[');
            rest = after;
            continue;
        };
        let inner = &after[..close];
        let name: String = inner
            .trim_start_matches('/')
            .chars()
            .take_while(|c| c.is_ascii_alphanumeric() || *c == '*')
            .collect::<String>()
            .to_ascii_lowercase();
        if !TAGS.contains(&name.as_str()) {
            out.push('[');
            rest = after;
            continue;
        }
        rest = &after[close + 1..];
        out.push(' ');
        // The text between [img] and [/img] is a link, not prose.
        if (name == "img" || name == "previewyoutube") && !inner.starts_with('/') {
            let lower = rest.to_ascii_lowercase();
            if let Some(end) = lower.find(&format!("[/{name}]")) {
                rest = &rest[end + name.len() + 3..];
            }
        }
    }
    out.push_str(rest);
    // Steam's short descriptions often start with what was an image tag: a bare link.
    let is_link = |word: &&str| word.starts_with("http://") || word.starts_with("https://");
    let text = out.split_whitespace().filter(|w| !is_link(w)).collect::<Vec<_>>().join(" ");
    if text.chars().count() <= max_chars {
        return text;
    }
    let mut cut: String = text.chars().take(max_chars.saturating_sub(1)).collect();
    cut.truncate(cut.trim_end().len());
    cut.push('…');
    cut
}

// ------------------------------------------------------------------ Steam's own records
// Steam keeps a small text file per game about its Workshop content:
// `<library>\steamapps\workshop\appworkshop_<appid>.acf`. Reading it needs no connection,
// so the launcher can tell which installed items Steam knows a newer version of.

/// A value in one of Valve's KeyValues ("VDF") text files.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Vdf {
    Text(String),
    Block(Vec<(String, Vdf)>),
}

impl Vdf {
    /// Parses a whole file. The result is a block holding the file's top-level entries.
    pub fn parse(text: &str) -> Result<Vdf, String> {
        /// Deeper nesting than any real file has: stops a damaged file from growing the stack.
        const MAX_DEPTH: usize = 64;
        enum Tok {
            Text(String),
            Open,
            Close,
        }

        let mut tokens = Vec::new();
        let mut chars = text.trim_start_matches('\u{feff}').chars().peekable();
        while let Some(c) = chars.next() {
            match c {
                c if c.is_whitespace() => {}
                '{' => tokens.push(Tok::Open),
                '}' => tokens.push(Tok::Close),
                '/' if chars.peek() == Some(&'/') => {
                    for c in chars.by_ref() {
                        if c == '\n' {
                            break;
                        }
                    }
                }
                '"' => {
                    let mut s = String::new();
                    let mut closed = false;
                    while let Some(c) = chars.next() {
                        match c {
                            '"' => {
                                closed = true;
                                break;
                            }
                            '\\' => match chars.next() {
                                Some('n') => s.push('\n'),
                                Some('t') => s.push('\t'),
                                Some(other) => s.push(other),
                                None => {}
                            },
                            _ => s.push(c),
                        }
                    }
                    if !closed {
                        return Err("a quoted text is never closed".to_owned());
                    }
                    tokens.push(Tok::Text(s));
                }
                _ => {
                    let mut s = String::from(c);
                    while let Some(n) = chars.peek().copied() {
                        if n.is_whitespace() || matches!(n, '{' | '}' | '"') {
                            break;
                        }
                        s.push(n);
                        chars.next();
                    }
                    // `[$WIN32]`-style conditions after a value mean nothing here.
                    if !(s.starts_with('[') && s.ends_with(']')) {
                        tokens.push(Tok::Text(s));
                    }
                }
            }
        }

        // One frame per open block: its key, and the entries read so far.
        let mut stack: Vec<(String, Vec<(String, Vdf)>)> = vec![(String::new(), Vec::new())];
        let mut tokens = tokens.into_iter();
        while let Some(token) = tokens.next() {
            match token {
                Tok::Text(key) => match tokens.next() {
                    Some(Tok::Text(value)) => {
                        if let Some(top) = stack.last_mut() {
                            top.1.push((key, Vdf::Text(value)));
                        }
                    }
                    Some(Tok::Open) => {
                        if stack.len() >= MAX_DEPTH {
                            return Err("blocks are nested too deeply".to_owned());
                        }
                        stack.push((key, Vec::new()));
                    }
                    Some(Tok::Close) | None => return Err(format!("\"{key}\" has no value")),
                },
                Tok::Open => return Err("a block has no name".to_owned()),
                Tok::Close => {
                    if stack.len() < 2 {
                        return Err("a block is closed that was never opened".to_owned());
                    }
                    if let Some((key, entries)) = stack.pop() {
                        if let Some(top) = stack.last_mut() {
                            top.1.push((key, Vdf::Block(entries)));
                        }
                    }
                }
            }
        }
        if stack.len() != 1 {
            return Err("a block is never closed".to_owned());
        }
        Ok(Vdf::Block(stack.pop().map(|(_, entries)| entries).unwrap_or_default()))
    }

    /// The entries of a block, in file order. Empty for a text value.
    pub fn entries(&self) -> &[(String, Vdf)] {
        match self {
            Vdf::Block(entries) => entries,
            Vdf::Text(_) => &[],
        }
    }

    /// The entry called `key` (Steam is not consistent about case, so neither is this).
    pub fn get(&self, key: &str) -> Option<&Vdf> {
        self.entries().iter().find(|(k, _)| k.eq_ignore_ascii_case(key)).map(|(_, v)| v)
    }

    /// The text value of the entry called `key`.
    pub fn text(&self, key: &str) -> Option<&str> {
        match self.get(key) {
            Some(Vdf::Text(s)) => Some(s),
            _ => None,
        }
    }

    fn number<T: std::str::FromStr + Default>(&self, key: &str) -> T {
        self.text(key).and_then(|s| s.trim().parse().ok()).unwrap_or_default()
    }
}

/// What Steam's records on this PC say about one Workshop item.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LocalItem {
    /// Listed under `WorkshopItemsInstalled`: the files are on disk.
    pub installed: bool,
    /// Size of the installed files as Steam counted them, in bytes.
    pub size: u64,
    /// When the installed version was published, Unix seconds (0 = not recorded).
    pub time_updated: i64,
    /// Content id of the installed version.
    pub manifest: String,
    /// When the newest version Steam has seen was published (0 = not recorded).
    pub latest_time_updated: i64,
    /// Content id of the newest version Steam has seen.
    pub latest_manifest: String,
}

impl LocalItem {
    /// Steam has seen a version newer than the installed one. This is as current as Steam's
    /// own last look at the Workshop, which happens while the Steam client runs.
    pub fn update_available(&self) -> bool {
        if !self.installed {
            return false;
        }
        let other_content =
            !self.manifest.is_empty() && !self.latest_manifest.is_empty() && self.manifest != self.latest_manifest;
        let newer = self.time_updated > 0 && self.latest_time_updated > self.time_updated;
        other_content || newer
    }
}

/// The parts of `appworkshop_<appid>.acf` the launcher uses.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub struct LocalState {
    pub items: HashMap<u64, LocalItem>,
    /// Steam's own flags for the game's Workshop content as a whole: something is waiting
    /// to be downloaded or updated.
    pub pending: bool,
}

impl LocalState {
    pub fn parse(text: &str) -> Result<LocalState, String> {
        let root = Vdf::parse(text)?;
        let app = root.get("AppWorkshop").ok_or("there is no AppWorkshop block")?;
        let mut state = LocalState {
            items: HashMap::new(),
            pending: app.number::<u32>("NeedsUpdate") != 0 || app.number::<u32>("NeedsDownload") != 0,
        };
        if let Some(installed) = app.get("WorkshopItemsInstalled") {
            for (id, entry) in installed.entries() {
                let Ok(id) = id.trim().parse::<u64>() else { continue };
                let item = state.items.entry(id).or_default();
                item.installed = true;
                item.size = entry.number("size");
                item.time_updated = entry.number("timeupdated");
                item.manifest = entry.text("manifest").unwrap_or_default().trim().to_owned();
            }
        }
        if let Some(details) = app.get("WorkshopItemDetails") {
            for (id, entry) in details.entries() {
                let Ok(id) = id.trim().parse::<u64>() else { continue };
                let item = state.items.entry(id).or_default();
                // The details repeat the installed version; use them where the first list was silent.
                if item.manifest.is_empty() {
                    item.manifest = entry.text("manifest").unwrap_or_default().trim().to_owned();
                }
                if item.time_updated == 0 {
                    item.time_updated = entry.number("timeupdated");
                }
                item.latest_time_updated = entry.number("latest_timeupdated");
                item.latest_manifest = entry.text("latest_manifest").unwrap_or_default().trim().to_owned();
            }
        }
        Ok(state)
    }

    /// Reads the file. `Ok(None)` when it doesn't exist (no Workshop content on this PC).
    pub fn read(path: &Path) -> Result<Option<LocalState>, String> {
        let bytes = match std::fs::read(path) {
            Ok(bytes) => bytes,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
            Err(e) => return Err(format!("Couldn't read {}: {e}", path.display())),
        };
        LocalState::parse(&String::from_utf8_lossy(&bytes))
            .map(Some)
            .map_err(|e| format!("Couldn't understand {}: {e}", path.display()))
    }

    pub fn item(&self, id: u64) -> Option<&LocalItem> {
        self.items.get(&id)
    }
}

/// Where Steam keeps its Workshop records for the game, given the game's Workshop content
/// folder (`...\steamapps\workshop\content\<appid>`).
pub fn local_state_path(workshop_content_dir: &Path) -> Option<PathBuf> {
    Some(workshop_content_dir.parent()?.parent()?.join(format!("appworkshop_{APP_ID}.acf")))
}

// ------------------------------------------------------------------ launcher <-> helper messages

#[derive(Serialize, Deserialize)]
enum Cmd {
    Refresh,
    Details(Vec<u64>),
    Query { req: u64, text: String, sort: Sort, page: u32 },
    Subscribe(u64),
    Unsubscribe(u64),
    Download(u64),
    Disconnect,
}

#[derive(Serialize, Deserialize, Clone, Copy, Debug, PartialEq, Eq)]
enum Action {
    Subscribe,
    Unsubscribe,
}

#[derive(Serialize, Deserialize)]
enum Event {
    Connected,
    Failed(Failure),
    Subscribed(Vec<u64>),
    States(Vec<(u64, ItemStatus)>),
    Details(Vec<Item>),
    Page { req: u64, result: Result<(Vec<Item>, u32), String> },
    Action { id: u64, action: Action, result: Result<(), String> },
    Installed(u64),
    Removed,
    Error(String),
}

struct Link {
    tx: Sender<Cmd>,
    rx: Receiver<Event>,
}

// ------------------------------------------------------------------ the handle pages use

/// The launcher's connection to Steam Workshop. `Default` is cheap and does nothing.
#[derive(Default)]
pub struct Workshop {
    status: Status,
    link: Option<Link>,
    connect_started: Option<Instant>,
    items: HashMap<u64, Item>,
    states: HashMap<u64, ItemStatus>,
    subscribed: Option<Vec<u64>>,
    browse: Browse,
    browse_req: u64,
    browse_sent: Option<Instant>,
    /// Subscribe / unsubscribe requests Steam has not answered yet.
    busy: HashMap<u64, Instant>,
    titles: Option<Titles>,
    previews: Previews,
}

impl Workshop {
    pub fn status(&self) -> &Status {
        &self.status
    }

    pub fn is_connected(&self) -> bool {
        self.status == Status::Connected
    }

    /// Starts connecting. The result shows up in [`Workshop::status`] a moment later.
    pub fn connect(&mut self, ctx: &egui::Context) {
        if matches!(self.status, Status::Connecting | Status::Connected) {
            return;
        }
        self.reset_session();
        match start_helper(ctx) {
            Ok(link) => {
                self.link = Some(link);
                self.connect_started = Some(Instant::now());
                self.status = Status::Connecting;
            }
            Err(failure) => self.status = Status::Failed(failure),
        }
    }

    /// Ends the helper process. Steam stops showing the game as running a few seconds later.
    pub fn disconnect(&mut self) {
        if let Some(link) = self.link.take() {
            let _ = link.tx.send(Cmd::Disconnect);
        }
        self.reset_session();
        self.status = Status::Disconnected;
    }

    fn reset_session(&mut self) {
        self.link = None;
        self.connect_started = None;
        self.states.clear();
        self.subscribed = None;
        self.browse = Browse::default();
        self.browse_sent = None;
        self.busy.clear();
    }

    fn send(&mut self, cmd: Cmd) -> bool {
        self.link.as_ref().is_some_and(|l| l.tx.send(cmd).is_ok())
    }

    fn fail(&mut self, failure: Failure) {
        self.reset_session();
        self.status = Status::Failed(failure);
    }

    /// Collects what Steam reported since the last call. Call once per frame while the page
    /// that owns this is on screen. Cheap when nothing happened.
    pub fn poll(&mut self, ctx: &egui::Context) -> Vec<Notice> {
        let mut notices = Vec::new();
        self.previews.collect();

        let Some(link) = &self.link else { return notices };
        let mut events = Vec::new();
        let mut lost = false;
        loop {
            match link.rx.try_recv() {
                Ok(event) => events.push(event),
                Err(TryRecvError::Empty) => break,
                Err(TryRecvError::Disconnected) => {
                    // The helper is gone. Fine if it said why; otherwise it died.
                    lost = true;
                    break;
                }
            }
        }

        let mut titles_changed = false;
        let mut rescan = false;
        for event in events {
            match event {
                Event::Connected => {
                    self.status = Status::Connected;
                    self.connect_started = None;
                }
                Event::Failed(failure) => {
                    self.fail(failure);
                    return notices;
                }
                Event::Subscribed(ids) => self.subscribed = Some(ids),
                Event::States(list) => self.states.extend(list),
                Event::Details(items) => {
                    for item in items {
                        titles_changed |= self.remember_title(&item);
                        self.items.insert(item.id, item);
                    }
                }
                Event::Page { req, result } => {
                    if req != self.browse_req {
                        continue;
                    }
                    self.browse.loading = false;
                    self.browse_sent = None;
                    match result {
                        Ok((items, total)) => {
                            self.browse.error = None;
                            self.browse.total = total;
                            self.browse.items = items.iter().map(|i| i.id).collect();
                            for item in items {
                                self.items.insert(item.id, item);
                            }
                        }
                        Err(e) => {
                            self.browse.items.clear();
                            self.browse.total = 0;
                            self.browse.error = Some(e);
                        }
                    }
                }
                Event::Action { id, action, result } => {
                    self.busy.remove(&id);
                    let title = self.title(id);
                    match (action, result) {
                        (Action::Subscribe, Ok(())) => {
                            if let Some(item) = self.items.get(&id).cloned() {
                                titles_changed |= self.remember_title(&item);
                            }
                        }
                        (Action::Unsubscribe, Ok(())) => notices.push(Notice::Info(format!("Unsubscribed from {title}"))),
                        (Action::Subscribe, Err(e)) => {
                            notices.push(Notice::Error(format!("Couldn't subscribe to {title}: {e}")))
                        }
                        (Action::Unsubscribe, Err(e)) => {
                            notices.push(Notice::Error(format!("Couldn't unsubscribe from {title}: {e}")))
                        }
                    }
                }
                Event::Installed(id) => {
                    notices.push(Notice::Info(format!("{} is installed", self.title(id))));
                    rescan = true;
                }
                Event::Removed => rescan = true,
                Event::Error(e) => notices.push(Notice::Error(e)),
            }
        }

        if lost {
            self.fail(Failure::new("The connection to Steam stopped unexpectedly.", ""));
            return notices;
        }

        // Nothing here may wait forever on Steam.
        let now = Instant::now();
        if self.connect_started.is_some_and(|t| now.duration_since(t) > CONNECT_TIMEOUT) {
            self.fail(Failure::new("Steam didn't answer. Check that Steam is running and signed in, then try again.", ""));
            return notices;
        }
        if self.browse.loading && self.browse_sent.is_some_and(|t| now.duration_since(t) > REQUEST_TIMEOUT) {
            self.browse.loading = false;
            self.browse_sent = None;
            self.browse_req += 1;
            self.browse.items.clear();
            self.browse.total = 0;
            self.browse.error = Some("Steam didn't answer in time.".to_owned());
        }
        let late: Vec<u64> =
            self.busy.iter().filter(|(_, t)| now.duration_since(**t) > REQUEST_TIMEOUT).map(|(id, _)| *id).collect();
        for id in late {
            self.busy.remove(&id);
            notices.push(Notice::Error(format!("Steam didn't answer about {}.", self.title(id))));
        }
        if self.connect_started.is_some() || self.browse.loading || !self.busy.is_empty() {
            ctx.request_repaint_after(Duration::from_secs(1));
        }

        if titles_changed {
            if let Some(titles) = &self.titles {
                if let Err(e) = save_titles(titles) {
                    notices.push(Notice::Error(e));
                }
            }
            notices.push(Notice::TitlesChanged);
        }
        if rescan {
            notices.push(Notice::Rescan);
        }
        notices
    }

    /// Stores the item's title for offline use. True when the cache changed.
    fn remember_title(&mut self, item: &Item) -> bool {
        if item.title.trim().is_empty() {
            return false;
        }
        let entry = Title { title: item.title.trim().to_owned(), description: item.description.clone() };
        let titles = self.titles.get_or_insert_with(load_titles);
        let key = item.id.to_string();
        if titles.get(&key) == Some(&entry) {
            return false;
        }
        titles.insert(key, entry);
        true
    }

    /// Titles and descriptions known for Workshop items, including ones cached on disk.
    pub fn titles(&mut self) -> &Titles {
        self.titles.get_or_insert_with(load_titles)
    }

    /// Ids of the items the account is subscribed to. `None` until Steam has answered.
    pub fn subscribed(&self) -> Option<&[u64]> {
        self.subscribed.as_deref()
    }

    pub fn is_subscribed(&self, id: u64) -> bool {
        self.states.get(&id).is_some_and(|s| s.subscribed) || self.subscribed.as_ref().is_some_and(|s| s.contains(&id))
    }

    /// Asks Steam again for the subscription list, item details and install states.
    pub fn refresh_subscribed(&mut self) {
        self.send(Cmd::Refresh);
    }

    /// Asks Steam for the details (title, description, preview) of these items.
    pub fn request_details(&mut self, ids: &[u64]) {
        if !ids.is_empty() {
            self.send(Cmd::Details(ids.to_vec()));
        }
    }

    pub fn item(&self, id: u64) -> Option<&Item> {
        self.items.get(&id)
    }

    /// The best name known for an item: Steam's, else the cached one, else its number.
    pub fn title(&self, id: u64) -> String {
        if let Some(item) = self.items.get(&id).filter(|i| !i.title.trim().is_empty()) {
            return item.title.trim().to_owned();
        }
        if let Some(t) = self.titles.as_ref().and_then(|t| t.get(&id.to_string())) {
            return t.title.clone();
        }
        format!("Workshop item {id}")
    }

    /// Install state and download progress. Refreshed about once a second while Steam is
    /// downloading something, every few seconds otherwise.
    pub fn item_status(&self, id: u64) -> ItemStatus {
        self.states.get(&id).cloned().unwrap_or_default()
    }

    /// Like [`Workshop::item_status`], but `None` while Steam has said nothing about the
    /// item (not connected, or not asked about it yet).
    pub fn known_status(&self, id: u64) -> Option<&ItemStatus> {
        self.states.get(&id)
    }

    /// A subscribe or unsubscribe for this item is waiting for Steam's answer.
    pub fn is_busy(&self, id: u64) -> bool {
        self.busy.contains_key(&id)
    }

    /// Runs a Workshop search. `page` is 1-based. Results arrive in [`Workshop::browse`].
    pub fn search(&mut self, text: &str, sort: Sort, page: u32) {
        self.browse_req += 1;
        let page = page.max(1);
        let text = text.trim().to_owned();
        self.browse.text = text.clone();
        self.browse.sort = sort;
        self.browse.page = page;
        self.browse.started = true;
        self.browse.error = None;
        if self.send(Cmd::Query { req: self.browse_req, text, sort, page }) {
            self.browse.loading = true;
            self.browse_sent = Some(Instant::now());
        } else {
            self.browse.loading = false;
            self.browse.error = Some("Not connected to Steam.".to_owned());
        }
    }

    pub fn browse(&self) -> &Browse {
        &self.browse
    }

    /// Subscribes and starts the download straight away.
    pub fn subscribe(&mut self, id: u64) {
        if self.send(Cmd::Subscribe(id)) {
            self.busy.insert(id, Instant::now());
        }
    }

    /// Unsubscribes. Steam deletes the item's files on its own schedule.
    pub fn unsubscribe(&mut self, id: u64) {
        if self.send(Cmd::Unsubscribe(id)) {
            self.busy.insert(id, Instant::now());
        }
    }

    /// Asks Steam to download (or update) a subscribed item now.
    pub fn download(&mut self, id: u64) {
        self.send(Cmd::Download(id));
    }

    /// Path of the item's preview image once it is in the cache. The first call for an item
    /// queues the download; a later frame gets the path. `None` also when it has no preview.
    pub fn preview(&mut self, ctx: &egui::Context, id: u64) -> Option<PathBuf> {
        let item = self.items.get(&id);
        self.previews.get(ctx, id, item)
    }
}

// ------------------------------------------------------------------ preview images

enum Preview {
    Loading,
    Ready(PathBuf),
    Missing,
}

#[derive(Default)]
struct Previews {
    state: HashMap<u64, Preview>,
    queue: Arc<Mutex<VecDeque<(u64, String)>>>,
    workers: Arc<AtomicUsize>,
    channel: Option<(Sender<(u64, Option<PathBuf>)>, Receiver<(u64, Option<PathBuf>)>)>,
}

impl Previews {
    fn collect(&mut self) {
        if let Some((_, rx)) = &self.channel {
            for (id, path) in rx.try_iter() {
                self.state.insert(id, path.map_or(Preview::Missing, Preview::Ready));
            }
        }
    }

    fn get(&mut self, ctx: &egui::Context, id: u64, item: Option<&Item>) -> Option<PathBuf> {
        match self.state.get(&id) {
            Some(Preview::Ready(p)) => return Some(p.clone()),
            Some(_) => return None,
            None => {}
        }
        let dir = cache_dir();
        let cached = PREVIEW_EXT.iter().map(|ext| dir.join(format!("{id}.{ext}"))).find(|p| p.is_file());
        if let Some(path) = &cached {
            // Use the cached file unless the item has been updated since it was fetched.
            let fetched = std::fs::metadata(path)
                .and_then(|m| m.modified())
                .ok()
                .and_then(|t| t.duration_since(std::time::UNIX_EPOCH).ok())
                .map_or(i64::MAX, |d| d.as_secs() as i64);
            if item.is_none_or(|i| i.updated <= fetched) {
                self.state.insert(id, Preview::Ready(path.clone()));
                return cached;
            }
        }
        let Some(url) = item.and_then(|i| checked_url(&i.preview_url)) else {
            // No item yet (details still on their way) is not final; no usable URL is.
            if item.is_some() {
                self.state.insert(id, cached.clone().map_or(Preview::Missing, Preview::Ready));
            }
            return cached;
        };
        self.state.insert(id, Preview::Loading);
        if let Ok(mut q) = self.queue.lock() {
            q.push_back((id, url));
        }
        let tx = self.channel.get_or_insert_with(mpsc::channel).0.clone();
        if self.workers.load(Ordering::SeqCst) < PREVIEW_WORKERS {
            self.workers.fetch_add(1, Ordering::SeqCst);
            let (queue, workers, ctx) = (self.queue.clone(), self.workers.clone(), ctx.clone());
            let spawned = std::thread::Builder::new().name("workshop-preview".into()).spawn(move || {
                loop {
                    let job = queue.lock().ok().and_then(|mut q| q.pop_front());
                    let Some((id, url)) = job else { break };
                    let path = download_preview(id, &url).ok();
                    let _ = tx.send((id, path));
                    ctx.request_repaint();
                }
                workers.fetch_sub(1, Ordering::SeqCst);
            });
            if spawned.is_err() {
                self.workers.fetch_sub(1, Ordering::SeqCst);
            }
        }
        cached
    }
}

/// Returns the URL as HTTPS if it points at one of Steam's image hosts.
fn checked_url(url: &str) -> Option<String> {
    let url = url.trim();
    let rest = url.strip_prefix("https://").or_else(|| url.strip_prefix("http://"))?;
    let authority = rest.split(['/', '?', '#']).next()?;
    if authority.is_empty() || authority.contains('@') || url.chars().any(|c| c.is_control() || c == ' ') {
        return None;
    }
    let host = authority.strip_suffix(":443").unwrap_or(authority).to_ascii_lowercase();
    if host.contains(':') {
        return None;
    }
    PREVIEW_HOSTS
        .iter()
        .any(|allowed| host == *allowed || host.ends_with(&format!(".{allowed}")))
        .then(|| format!("https://{rest}"))
}

fn http_agent() -> &'static ureq::Agent {
    static AGENT: OnceLock<ureq::Agent> = OnceLock::new();
    AGENT.get_or_init(|| {
        let tls = ureq::tls::TlsConfig::builder()
            .provider(ureq::tls::TlsProvider::NativeTls)
            .root_certs(ureq::tls::RootCerts::PlatformVerifier)
            .build();
        ureq::Agent::config_builder()
            .tls_config(tls)
            .https_only(true)
            // Redirects are followed by hand so every hop is checked against the host list.
            .max_redirects(0)
            .timeout_global(Some(Duration::from_secs(30)))
            .build()
            .new_agent()
    })
}

fn image_ext(bytes: &[u8]) -> Option<&'static str> {
    if bytes.starts_with(&[0xFF, 0xD8, 0xFF]) {
        Some("jpg")
    } else if bytes.starts_with(&[0x89, b'P', b'N', b'G']) {
        Some("png")
    } else if bytes.starts_with(b"GIF8") {
        Some("gif")
    } else if bytes.len() > 12 && bytes.starts_with(b"RIFF") && &bytes[8..12] == b"WEBP" {
        Some("webp")
    } else {
        None
    }
}

fn download_preview(id: u64, url: &str) -> Result<PathBuf, String> {
    let mut url = url.to_owned();
    for _ in 0..4 {
        let mut response = http_agent().get(&url).call().map_err(|e| e.to_string())?;
        let status = response.status().as_u16();
        if (300..400).contains(&status) {
            let next = response.headers().get("location").and_then(|v| v.to_str().ok()).and_then(checked_url);
            url = next.ok_or("redirected away from Steam")?;
            continue;
        }
        if status != 200 {
            return Err(format!("HTTP {status}"));
        }
        let bytes =
            response.body_mut().with_config().limit(PREVIEW_MAX_BYTES).read_to_vec().map_err(|e| e.to_string())?;
        let ext = image_ext(&bytes).ok_or("not an image")?;
        let dir = cache_dir();
        for old in PREVIEW_EXT {
            let _ = std::fs::remove_file(dir.join(format!("{id}.{old}")));
        }
        let path = dir.join(format!("{id}.{ext}"));
        let tmp = dir.join(format!("{id}.{ext}.part"));
        std::fs::write(&tmp, &bytes).map_err(|e| e.to_string())?;
        std::fs::rename(&tmp, &path).map_err(|e| e.to_string())?;
        return Ok(path);
    }
    Err("too many redirects".to_owned())
}

// ------------------------------------------------------------------ launcher side of the pipe

/// The exe to start as the helper: the launcher itself.
fn helper_exe() -> Result<PathBuf, Failure> {
    let exe = std::env::current_exe()
        .map_err(|e| Failure::new("The launcher couldn't work out where its own exe is.", e.to_string()))?;
    // A test binary can't be its own helper: use the launcher exe built next to it.
    #[cfg(test)]
    let exe = std::env::var_os("COMMAND_CENTER_HELPER_EXE")
        .map(PathBuf::from)
        .or_else(|| exe.ancestors().map(|d| d.join("command-center.exe")).find(|p| p.is_file()))
        .unwrap_or(exe);
    Ok(exe)
}

/// Starts the helper process and the two threads that sit on its pipes.
fn start_helper(ctx: &egui::Context) -> Result<Link, Failure> {
    use std::os::windows::process::CommandExt;
    use std::process::{Command, Stdio};
    const CREATE_NO_WINDOW: u32 = 0x0800_0000;

    let exe = helper_exe()?;
    let mut child = Command::new(&exe)
        .arg(HELPER_ARG)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .creation_flags(CREATE_NO_WINDOW)
        .spawn()
        .map_err(|e| Failure::new("The launcher couldn't start its Steam helper.", format!("{}: {e}", exe.display())))?;
    let (Some(mut stdin), Some(stdout)) = (child.stdin.take(), child.stdout.take()) else {
        let _ = child.kill();
        return Err(Failure::new("The launcher couldn't talk to its Steam helper.", ""));
    };

    let (cmd_tx, cmd_rx) = mpsc::channel::<Cmd>();
    let (ev_tx, ev_rx) = mpsc::channel::<Event>();

    // Writer: passes commands on, and owns the process so it can make sure it ends.
    let writer = std::thread::Builder::new().name("steam-helper-in".into()).spawn(move || {
        // Ends on Disconnect, or when the `Workshop` lets go of the link.
        for cmd in cmd_rx {
            let last = matches!(cmd, Cmd::Disconnect);
            let sent = serde_json::to_string(&cmd)
                .ok()
                .is_some_and(|json| writeln!(stdin, "{json}").and_then(|_| stdin.flush()).is_ok());
            if last || !sent {
                break;
            }
        }
        // Closing its stdin tells the helper to stop even if no message got through.
        drop(stdin);
        for _ in 0..50 {
            if !matches!(child.try_wait(), Ok(None)) {
                return;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        let _ = child.kill();
        let _ = child.wait();
    });
    if let Err(e) = writer {
        // The closure (and with it the helper's stdin) was dropped, so the helper exits by itself.
        return Err(Failure::new("The launcher couldn't start its Steam helper.", e.to_string()));
    }

    // Reader: one message per marked line. Anything else is the Steam DLL talking.
    let ctx = ctx.clone();
    let reader = std::thread::Builder::new().name("steam-helper-out".into()).spawn(move || {
        let mut lines = std::io::BufReader::new(stdout);
        let mut line = Vec::new();
        loop {
            line.clear();
            match lines.read_until(b'\n', &mut line) {
                Ok(0) | Err(_) => break,
                Ok(_) => {}
            }
            let text = String::from_utf8_lossy(&line);
            let Some(json) = text.trim().strip_prefix(MESSAGE_MARK.trim_end()) else { continue };
            let Ok(event) = serde_json::from_str::<Event>(json.trim()) else { continue };
            if ev_tx.send(event).is_err() {
                break;
            }
            ctx.request_repaint();
        }
        // The helper is gone: dropping the sender is how the UI finds out.
        drop(ev_tx);
        ctx.request_repaint();
    });
    if let Err(e) = reader {
        let _ = cmd_tx.send(Cmd::Disconnect);
        return Err(Failure::new("The launcher couldn't start its Steam helper.", e.to_string()));
    }
    Ok(Link { tx: cmd_tx, rx: ev_rx })
}

// ------------------------------------------------------------------ the helper process

/// Runs the exe as the Steam helper: reads commands from stdin, prints events to stdout,
/// and returns (so the process exits) when told to disconnect or when stdin closes.
pub fn helper_main() {
    let (tx, rx) = mpsc::channel::<Cmd>();
    std::thread::spawn(move || {
        for line in std::io::stdin().lock().lines() {
            let Ok(line) = line else { break };
            if let Ok(cmd) = serde_json::from_str::<Cmd>(line.trim()) {
                if tx.send(cmd).is_err() {
                    return;
                }
            }
        }
        // The launcher closed the pipe or is gone. Stop, and don't let a stuck Steam call
        // keep this process (and Steam's "running" state) alive.
        let _ = tx.send(Cmd::Disconnect);
        std::thread::sleep(Duration::from_secs(3));
        std::process::exit(0);
    });
    worker(rx, Out);
}

/// Where the helper's events go: its stdout, one marked JSON line each.
#[derive(Clone)]
struct Out;

impl Out {
    fn send(&self, event: Event) {
        let Ok(json) = serde_json::to_string(&event) else { return };
        // The leading newline ends whatever the Steam DLL may have left on the line.
        let message = format!("\n{MESSAGE_MARK}{json}\n");
        let mut stdout = std::io::stdout().lock();
        let _ = stdout.write_all(message.as_bytes());
        let _ = stdout.flush();
    }
}

/// Loads `steam_api64.dll` from the exe's own folder, so the delay-loaded imports resolve.
/// Without this the first Steam call would crash the helper when the DLL is missing.
fn load_steam_api() -> Result<(), Failure> {
    #[link(name = "kernel32")]
    extern "system" {
        fn LoadLibraryW(name: *const u16) -> *mut std::ffi::c_void;
    }
    use std::os::windows::ffi::OsStrExt;

    let dir = std::env::current_exe()
        .ok()
        .and_then(|exe| exe.parent().map(Path::to_path_buf))
        .ok_or_else(|| Failure::new("The launcher couldn't work out its own folder.", ""))?;
    let dll = dir.join(DLL_NAME);
    if !dll.is_file() {
        return Err(Failure::new(
            format!("{DLL_NAME} is missing. It has to sit next to the launcher's exe."),
            format!("Looked in {}", dir.display()),
        ));
    }
    let wide: Vec<u16> = dll.as_os_str().encode_wide().chain(std::iter::once(0)).collect();
    // SAFETY: `wide` is a NUL-terminated UTF-16 path that outlives the call.
    let module = unsafe { LoadLibraryW(wide.as_ptr()) };
    if module.is_null() {
        return Err(Failure::new(
            format!("{DLL_NAME} couldn't be loaded. The file next to the launcher may be damaged."),
            std::io::Error::last_os_error().to_string(),
        ));
    }
    Ok(())
}

fn init_failure(error: sw::SteamAPIInitError) -> Failure {
    match error {
        sw::SteamAPIInitError::NoSteamClient(detail) => {
            Failure::new("Steam isn't running. Start Steam, sign in, then try again.", detail)
        }
        sw::SteamAPIInitError::VersionMismatch(detail) => {
            Failure::new("Steam is out of date. Let Steam update itself, then try again.", detail)
        }
        sw::SteamAPIInitError::FailedGeneric(detail) => Failure::new(
            "Steam refused the connection. Usually that means the signed-in account doesn't own Empire at War, or Steam is still starting.",
            detail,
        ),
    }
}

fn steam_error(error: sw::SteamError) -> String {
    use sw::SteamError as E;
    match error {
        E::NoConnection | E::ServiceUnavailable => "Steam is offline or can't reach the Workshop.".to_owned(),
        E::NotLoggedOn => "Steam isn't signed in.".to_owned(),
        E::Timeout | E::IOFailure => "Steam didn't answer. It may be offline.".to_owned(),
        E::AccessDenied | E::InsufficientPrivilege => "Steam says this account isn't allowed to do that.".to_owned(),
        E::Banned => "Steam says this item or account is banned.".to_owned(),
        E::FileNotFound => "The item no longer exists on the Workshop.".to_owned(),
        E::LimitExceeded | E::RateLimitExceeded => "Steam says too many requests. Try again in a moment.".to_owned(),
        E::DiskFull => "The disk is full.".to_owned(),
        E::Busy | E::Pending => "Steam is busy. Try again in a moment.".to_owned(),
        other => format!("Steam error: {other}."),
    }
}

/// Converts one page of query results. Each item is read on its own so one odd entry
/// (the bindings assert on values they don't know) can't lose the page.
fn read_results(results: &sw::QueryResults<'_>) -> Vec<Item> {
    let mut items = Vec::new();
    for index in 0..results.returned_results() {
        let Ok(Some(r)) = catch_unwind(AssertUnwindSafe(|| results.get(index))) else { continue };
        let preview_url = catch_unwind(AssertUnwindSafe(|| results.preview_url(index))).ok().flatten().unwrap_or_default();
        let subscribers =
            catch_unwind(AssertUnwindSafe(|| results.statistic(index, sw::UGCStatisticType::Subscriptions))).ok().flatten();
        items.push(Item {
            id: r.published_file_id.0,
            title: r.title.trim().to_owned(),
            description: plain_text(&r.description, 600),
            preview_url,
            subscribers,
            votes_up: r.num_upvotes,
            votes_down: r.num_downvotes,
            score: r.score,
            updated: r.time_updated as i64,
            tags: r.tags.into_iter().filter(|t| !t.trim().is_empty()).collect(),
        });
    }
    items
}

struct Known {
    bits: u32,
    status: ItemStatus,
}

struct Worker {
    ugc: sw::UGC,
    out: Out,
    /// Set by callbacks when install states should be re-read right away.
    poke: Arc<AtomicBool>,
    /// Items to start downloading (subscribing alone waits until the "game" exits).
    downloads: Arc<Mutex<Vec<u64>>>,
    /// Ids on the Browse page currently shown.
    page_ids: Arc<Mutex<Vec<u64>>>,
    /// Other ids worth watching: ones the user acted on or asked details for.
    extra: HashSet<u64>,
    subscribed: Vec<u64>,
    sent_subscribed: bool,
    known: HashMap<u64, Known>,
    downloading: bool,
    /// Until then, poll every second: the user just started something.
    watch_closely_until: Instant,
    last_poll: Instant,
}

fn worker(rx: Receiver<Cmd>, out: Out) {
    if let Err(failure) = load_steam_api() {
        out.send(Event::Failed(failure));
        return;
    }
    // Sets SteamAppId for this process only, so no steam_appid.txt is needed.
    let client = match catch_unwind(|| sw::Client::init_app(sw::AppId(APP_ID))) {
        Ok(Ok(client)) => client,
        Ok(Err(e)) => {
            out.send(Event::Failed(init_failure(e)));
            return;
        }
        Err(_) => {
            out.send(Event::Failed(Failure::new("Steam answered in a way the launcher didn't understand.", "")));
            return;
        }
    };

    let session = catch_unwind(AssertUnwindSafe(|| run_session(&client, &rx, &out)));
    drop(client);
    match session {
        Ok(Ok(())) => {}
        Ok(Err(failure)) => out.send(Event::Failed(failure)),
        Err(_) => out.send(Event::Failed(Failure::new("The connection to Steam stopped unexpectedly.", ""))),
    }
}

fn run_session(client: &sw::Client, rx: &Receiver<Cmd>, out: &Out) -> Result<(), Failure> {
    if !client.apps().is_subscribed() {
        return Err(Failure::new("The Steam account that is signed in doesn't own Empire at War.", ""));
    }

    let poke = Arc::new(AtomicBool::new(false));
    let _download_done = {
        let (poke, out) = (poke.clone(), out.clone());
        client.register_callback(move |result: sw::DownloadItemResult| {
            if result.app_id.0 != APP_ID {
                return;
            }
            poke.store(true, Ordering::SeqCst);
            if let Some(e) = result.error {
                out.send(Event::Error(format!(
                    "Steam couldn't download Workshop item {}: {}",
                    result.published_file_id.0,
                    steam_error(e)
                )));
            }
        })
    };

    let mut w = Worker {
        ugc: client.ugc(),
        out: out.clone(),
        poke,
        downloads: Arc::default(),
        page_ids: Arc::default(),
        extra: HashSet::new(),
        subscribed: Vec::new(),
        sent_subscribed: false,
        known: HashMap::new(),
        downloading: false,
        watch_closely_until: Instant::now(),
        last_poll: Instant::now(),
    };
    out.send(Event::Connected);
    w.poll_states(true);
    w.fetch_details(&w.subscribed.clone());

    loop {
        match rx.recv_timeout(Duration::from_millis(50)) {
            Ok(Cmd::Disconnect) | Err(RecvTimeoutError::Disconnected) => return Ok(()),
            Ok(cmd) => w.handle(cmd),
            Err(RecvTimeoutError::Timeout) => {}
        }
        client.run_callbacks();

        let queued: Vec<u64> = w.downloads.lock().map(|mut d| std::mem::take(&mut *d)).unwrap_or_default();
        for id in queued {
            w.ugc.download_item(sw::PublishedFileId(id), true);
            w.watch_closely();
        }

        // About once a second while something is downloading, otherwise now and then to
        // notice changes made in the Steam client itself.
        let busy = w.downloading || Instant::now() < w.watch_closely_until;
        let every = if busy { Duration::from_secs(1) } else { Duration::from_secs(4) };
        if w.poke.swap(false, Ordering::SeqCst) || w.last_poll.elapsed() >= every {
            w.poll_states(false);
        }
    }
}

impl Worker {
    fn watch_closely(&mut self) {
        self.watch_closely_until = Instant::now() + Duration::from_secs(20);
        self.poke.store(true, Ordering::SeqCst);
    }

    fn handle(&mut self, cmd: Cmd) {
        match cmd {
            Cmd::Disconnect => {}
            Cmd::Refresh => {
                self.sent_subscribed = false;
                self.poll_states(true);
                self.fetch_details(&self.subscribed.clone());
            }
            Cmd::Details(ids) => {
                self.extra.extend(ids.iter().copied());
                self.fetch_details(&ids);
                self.poke.store(true, Ordering::SeqCst);
            }
            Cmd::Query { req, text, sort, page } => self.query(req, &text, sort, page),
            Cmd::Subscribe(id) => {
                self.extra.insert(id);
                self.watch_closely();
                let (out, poke, downloads) = (self.out.clone(), self.poke.clone(), self.downloads.clone());
                self.ugc.subscribe_item(sw::PublishedFileId(id), move |result| {
                    if result.is_ok() {
                        if let Ok(mut d) = downloads.lock() {
                            d.push(id);
                        }
                    }
                    poke.store(true, Ordering::SeqCst);
                    out.send(Event::Action { id, action: Action::Subscribe, result: result.map_err(steam_error) });
                });
            }
            Cmd::Unsubscribe(id) => {
                self.extra.insert(id);
                self.watch_closely();
                let (out, poke) = (self.out.clone(), self.poke.clone());
                self.ugc.unsubscribe_item(sw::PublishedFileId(id), move |result| {
                    poke.store(true, Ordering::SeqCst);
                    out.send(Event::Action { id, action: Action::Unsubscribe, result: result.map_err(steam_error) });
                });
            }
            Cmd::Download(id) => {
                self.extra.insert(id);
                if !self.ugc.download_item(sw::PublishedFileId(id), true) {
                    self.out.send(Event::Error(format!(
                        "Steam didn't start the download of Workshop item {id}. It may already be up to date."
                    )));
                }
                self.watch_closely();
            }
        }
    }

    fn query(&mut self, req: u64, text: &str, sort: Sort, page: u32) {
        let text = text.replace('\0', " ");
        let text = text.trim();
        let app = sw::AppId(APP_ID);
        // The query builder asserts on a `false` from Steam, hence the guard.
        let built = catch_unwind(AssertUnwindSafe(|| {
            let mut query = self
                .ugc
                .query_all(sort.query_type(), sw::UGCType::Items, sw::AppIDs::Both { creator: app, consumer: app }, page.max(1))
                .ok()?;
            if !text.is_empty() {
                query = query.set_search_text(text);
            }
            if sort == Sort::Popular {
                query = query.set_ranked_by_trend_days(TREND_DAYS);
            }
            Some(query)
        }));
        let Ok(Some(query)) = built else {
            self.out.send(Event::Page { req, result: Err("Steam wouldn't accept the search.".to_owned()) });
            return;
        };
        let (out, page_ids) = (self.out.clone(), self.page_ids.clone());
        let poke = self.poke.clone();
        query.fetch(move |result: Result<sw::QueryResults<'_>, sw::SteamError>| {
            let result = match result {
                Ok(results) => {
                    let total = results.total_results();
                    let items = read_results(&results);
                    if let Ok(mut ids) = page_ids.lock() {
                        *ids = items.iter().map(|i| i.id).collect();
                    }
                    poke.store(true, Ordering::SeqCst);
                    Ok((items, total))
                }
                Err(e) => Err(steam_error(e)),
            };
            out.send(Event::Page { req, result });
        });
    }

    fn fetch_details(&self, ids: &[u64]) {
        for chunk in ids.chunks(PAGE_SIZE as usize) {
            let list: Vec<sw::PublishedFileId> = chunk.iter().map(|id| sw::PublishedFileId(*id)).collect();
            let Ok(query) = self.ugc.query_items(list) else {
                self.out.send(Event::Error("Steam wouldn't accept the request for item details.".to_owned()));
                continue;
            };
            let out = self.out.clone();
            query.fetch(move |result: Result<sw::QueryResults<'_>, sw::SteamError>| match result {
                Ok(results) => out.send(Event::Details(read_results(&results))),
                Err(e) => out.send(Event::Error(format!("Couldn't get Workshop item details. {}", steam_error(e)))),
            });
        }
    }

    fn status_of(&self, id: u64, state: sw::ItemState) -> ItemStatus {
        let file = sw::PublishedFileId(id);
        let mut status = ItemStatus {
            subscribed: state.contains(sw::ItemState::SUBSCRIBED),
            installed: state.contains(sw::ItemState::INSTALLED),
            needs_update: state.contains(sw::ItemState::NEEDS_UPDATE),
            downloading: state.intersects(sw::ItemState::DOWNLOADING | sw::ItemState::DOWNLOAD_PENDING),
            ..Default::default()
        };
        if status.installed {
            if let Some(info) = self.ugc.item_install_info(file) {
                status.size_on_disk = info.size_on_disk;
                if !info.folder.is_empty() {
                    status.folder = Some(PathBuf::from(info.folder));
                }
            }
        }
        if status.downloading || status.needs_update {
            if let Some((done, total)) = self.ugc.item_download_info(file) {
                status.bytes_done = done;
                status.bytes_total = total;
            }
        }
        status
    }

    /// Re-reads the subscription list and the state of every watched item, and reports
    /// what changed. All of these are local calls to the Steam client.
    fn poll_states(&mut self, force: bool) {
        self.last_poll = Instant::now();

        let mut subscribed: Vec<u64> = catch_unwind(AssertUnwindSafe(|| self.ugc.subscribed_items(false)))
            .unwrap_or_default()
            .into_iter()
            .map(|id| id.0)
            .collect();
        subscribed.sort_unstable();
        subscribed.dedup();
        if subscribed != self.subscribed || !self.sent_subscribed {
            let new: Vec<u64> = subscribed.iter().copied().filter(|id| !self.subscribed.contains(id)).collect();
            let first = !self.sent_subscribed;
            self.subscribed = subscribed;
            self.sent_subscribed = true;
            self.out.send(Event::Subscribed(self.subscribed.clone()));
            // The first list is followed by a full details request from the caller.
            if !first {
                self.fetch_details(&new);
            }
        }

        let mut ids: Vec<u64> = self.subscribed.clone();
        ids.extend(self.page_ids.lock().map(|p| p.clone()).unwrap_or_default());
        ids.extend(self.extra.iter().copied());
        ids.sort_unstable();
        ids.dedup();

        let mut changed = Vec::new();
        let mut downloading = false;
        for id in ids {
            let state = self.ugc.item_state(sw::PublishedFileId(id));
            let active = state.intersects(sw::ItemState::DOWNLOADING | sw::ItemState::DOWNLOAD_PENDING);
            downloading |= active;
            let previous = self.known.get(&id);
            if !force && !active && previous.is_some_and(|k| k.bits == state.bits()) {
                continue;
            }
            let status = self.status_of(id, state);
            if let Some(previous) = previous {
                if previous.status == status {
                    continue;
                }
                if status.ready() && !previous.status.ready() {
                    self.out.send(Event::Installed(id));
                } else if previous.status.installed && !status.installed {
                    self.out.send(Event::Removed);
                }
            }
            changed.push((id, status.clone()));
            self.known.insert(id, Known { bits: state.bits(), status });
        }
        self.downloading = downloading;
        if !changed.is_empty() {
            self.out.send(Event::States(changed));
        }
    }
}

// ------------------------------------------------------------------ tests

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn item_ids_from_links() {
        assert_eq!(parse_item_id("1125571106"), Some(1125571106));
        assert_eq!(parse_item_id(" https://steamcommunity.com/sharedfiles/filedetails/?id=1125571106&searchtext= "), Some(1125571106));
        assert_eq!(parse_item_id("https://steamcommunity.com/workshop/filedetails/?foo=1&id=42"), Some(42));
        assert_eq!(parse_item_id("steam://url/CommunityFilePage/77"), Some(77));
        assert_eq!(parse_item_id("thrawn"), None);
        assert_eq!(parse_item_id(""), None);
        assert_eq!(parse_item_id("0"), None);
    }

    #[test]
    fn bbcode_is_stripped() {
        let s = "[h1]Big mod[/h1]\n[b]Adds[/b] ships. [img]https://x/y.png[/img] See [url=https://a.b]here[/url] [1] ok";
        assert_eq!(plain_text(s, 200), "Big mod Adds ships. See here [1] ok");
        assert_eq!(plain_text("one two three four", 9), "one two…");
        assert_eq!(plain_text("https://i.imgur.com/9xgj4Z5.png Palpatine's plan", 80), "Palpatine's plan");
    }

    #[test]
    fn only_steam_hosts() {
        assert_eq!(
            checked_url("https://images.steamusercontent.com/ugc/1/ABC/").as_deref(),
            Some("https://images.steamusercontent.com/ugc/1/ABC/")
        );
        assert!(checked_url("http://steamuserimages-a.akamaihd.net/ugc/1/ABC/").is_some_and(|u| u.starts_with("https://")));
        assert_eq!(checked_url("https://example.com/a.png"), None);
        assert_eq!(checked_url("https://steamusercontent.com.evil.example/a.png"), None);
        assert_eq!(checked_url("https://user@images.steamusercontent.com/a.png"), None);
        assert_eq!(checked_url("ftp://images.steamusercontent.com/a.png"), None);
    }

    /// The shape of a real `appworkshop_32470.acf`, trimmed to three items: one current, one
    /// Steam has seen a newer version of, and one subscribed but not downloaded.
    const ACF: &str = "\"AppWorkshop\"\n{\n\t\"appid\"\t\t\"32470\"\n\t\"SizeOnDisk\"\t\t\"104430155686\"\n\t\"NeedsUpdate\"\t\t\"1\"\n\t\"NeedsDownload\"\t\t\"0\"\n\t\"TimeLastUpdated\"\t\t\"1794518213\"\n\t\"TimeLastAppRan\"\t\t\"1794496663\"\n\t\"LastBuildID\"\t\t\"15987880\"\n\t\"WorkshopItemsInstalled\"\n\t{\n\t\t\"1125571106\"\n\t\t{\n\t\t\t\"size\"\t\t\"26682023071\"\n\t\t\t\"timeupdated\"\t\t\"1785339388\"\n\t\t\t\"manifest\"\t\t\"117925265235480311\"\n\t\t}\n\t\t\"1770851727\"\n\t\t{\n\t\t\t\"size\"\t\t\"22180234497\"\n\t\t\t\"timeupdated\"\t\t\"1692486814\"\n\t\t\t\"manifest\"\t\t\"4857426212674182359\"\n\t\t}\n\t}\n\t\"WorkshopItemDetails\"\n\t{\n\t\t\"1125571106\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"117925265235480311\"\n\t\t\t\"timeupdated\"\t\t\"1785339388\"\n\t\t\t\"timetouched\"\t\t\"1794495325\"\n\t\t\t\"subscribedby\"\t\t\"1271133577\"\n\t\t\t\"latest_timeupdated\"\t\t\"1785339388\"\n\t\t\t\"latest_manifest\"\t\t\"117925265235480311\"\n\t\t}\n\t\t\"1770851727\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"4857426212674182359\"\n\t\t\t\"timeupdated\"\t\t\"1692486814\"\n\t\t\t\"timetouched\"\t\t\"1794495325\"\n\t\t\t\"subscribedby\"\t\t\"1271133577\"\n\t\t\t\"latest_timeupdated\"\t\t\"1791648357\"\n\t\t\t\"latest_manifest\"\t\t\"588262808355129871\"\n\t\t}\n\t\t\"3830298393\"\n\t\t{\n\t\t\t\"manifest\"\t\t\"0\"\n\t\t\t\"timeupdated\"\t\t\"0\"\n\t\t\t\"timetouched\"\t\t\"1793383758\"\n\t\t\t\"latest_timeupdated\"\t\t\"1793383737\"\n\t\t\t\"latest_manifest\"\t\t\"8671042864565479444\"\n\t\t}\n\t}\n}\n";

    #[test]
    fn vdf_blocks_and_text() {
        let root = Vdf::parse(ACF).unwrap();
        let app = root.get("appworkshop").unwrap();
        assert_eq!(app.text("appid"), Some("32470"));
        assert_eq!(app.text("SizeOnDisk"), Some("104430155686"));
        let installed = app.get("WorkshopItemsInstalled").unwrap();
        assert_eq!(installed.entries().len(), 2);
        assert_eq!(installed.get("1770851727").and_then(|i| i.text("manifest")), Some("4857426212674182359"));
        // A block is not a text and the other way round.
        assert_eq!(app.text("WorkshopItemsInstalled"), None);
        assert!(app.get("appid").is_some_and(|v| v.entries().is_empty()));

        let odd = Vdf::parse("\u{feff}// comment\nroot { \"a\\\\b\" \"say \\\"hi\\\"\" [$WIN32]\n bare 12 // tail\n empty {} }").unwrap();
        let block = odd.get("root").unwrap();
        assert_eq!(block.text("a\\b"), Some("say \"hi\""));
        assert_eq!(block.text("bare"), Some("12"));
        assert_eq!(block.get("empty"), Some(&Vdf::Block(Vec::new())));

        assert!(Vdf::parse("\"a\" {").is_err());
        assert!(Vdf::parse("\"a\" { } }").is_err());
        assert!(Vdf::parse("\"a\" \"unclosed").is_err());
        assert!(Vdf::parse("\"key\"").is_err());
        assert!(Vdf::parse(&"a { ".repeat(500)).is_err());
        assert_eq!(Vdf::parse("").unwrap(), Vdf::Block(Vec::new()));
    }

    #[test]
    fn updates_from_steams_records() {
        let state = LocalState::parse(ACF).unwrap();
        assert!(state.pending);
        assert_eq!(state.items.len(), 3);

        let current = state.item(1125571106).unwrap();
        assert!(current.installed);
        assert_eq!(current.size, 26_682_023_071);
        assert_eq!(current.time_updated, 1_785_339_388);
        assert!(!current.update_available());

        let stale = state.item(1770851727).unwrap();
        assert_eq!(stale.latest_time_updated, 1_791_648_357);
        assert!(stale.update_available());

        // Known to Steam but not on disk: nothing installed, so nothing to update.
        let absent = state.item(3830298393).unwrap();
        assert!(!absent.installed);
        assert!(!absent.update_available());

        // Same publish time but different content still counts.
        let mut item = current.clone();
        item.latest_manifest = "1".to_owned();
        assert!(item.update_available());
        // Without a recorded newest version there is nothing to compare.
        let bare = LocalItem { installed: true, time_updated: 5, manifest: "7".to_owned(), ..Default::default() };
        assert!(!bare.update_available());

        assert!(LocalState::parse("\"Something\" { }").is_err());
        assert!(!LocalState::parse("\"AppWorkshop\" { \"NeedsUpdate\" \"0\" }").unwrap().pending);
        assert_eq!(
            local_state_path(Path::new("F:\\Lib\\steamapps\\workshop\\content\\32470")),
            Some(PathBuf::from("F:\\Lib\\steamapps\\workshop\\appworkshop_32470.acf"))
        );
    }

    /// What Steam itself says about the game running (it sets this while a client is connected).
    fn steam_says_running() -> Option<u32> {
        use winreg::enums::HKEY_CURRENT_USER;
        winreg::RegKey::predef(HKEY_CURRENT_USER)
            .open_subkey(format!("Software\\Valve\\Steam\\Apps\\{APP_ID}"))
            .ok()?
            .get_value::<u32, _>("Running")
            .ok()
    }

    fn pump(ws: &mut Workshop, ctx: &egui::Context, seconds: u64, mut done: impl FnMut(&mut Workshop) -> bool) -> bool {
        let start = Instant::now();
        while start.elapsed() < Duration::from_secs(seconds) {
            for notice in ws.poll(ctx) {
                println!("  notice: {notice:?}");
            }
            if done(ws) {
                return true;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        false
    }

    /// Waits for Steam to stop counting the game as running. Returns the seconds it took.
    fn wait_until_not_running(limit: u64) -> Option<u64> {
        (0..=limit).find(|_| {
            let stopped = steam_says_running() == Some(0);
            if !stopped {
                std::thread::sleep(Duration::from_secs(1));
            }
            stopped
        })
    }

    /// Talks to the real Steam client through the real helper process (the launcher exe
    /// built by `cargo build`, so build first). Read-only: connects, lists subscriptions,
    /// runs four searches, fetches one preview image, disconnects and connects again.
    /// `cargo test workshop -- --ignored --nocapture`
    #[test]
    #[ignore = "needs a running Steam client that owns Empire at War"]
    fn live_steam_read_only() {
        let ctx = egui::Context::default();
        let mut ws = Workshop::default();
        println!("Steam's Running flag before connecting: {:?}", steam_says_running());

        ws.connect(&ctx);
        pump(&mut ws, &ctx, 40, |ws| !matches!(ws.status(), Status::Connecting));
        println!("status after connect: {:?}", ws.status());
        if !ws.is_connected() {
            return;
        }
        println!("Steam's Running flag while connected: {:?}", steam_says_running());

        let got = pump(&mut ws, &ctx, 20, |ws| {
            ws.subscribed().is_some_and(|ids| ids.iter().all(|id| ws.item(*id).is_some()))
        });
        let ids = ws.subscribed().map(<[u64]>::to_vec).unwrap_or_default();
        println!("subscribed items: {} (details for all: {got})", ids.len());
        for id in &ids {
            let s = ws.item_status(*id);
            println!(
                "  {id}  {:<48}  installed={} update={} downloading={} size={} folder={:?}",
                ws.title(*id),
                s.installed,
                s.needs_update,
                s.downloading,
                s.size_on_disk,
                s.folder
            );
        }

        for (text, sort) in [("", Sort::Popular), ("thrawn", Sort::Subscribed), ("", Sort::Updated), ("", Sort::Rated)] {
            ws.search(text, sort, 1);
            pump(&mut ws, &ctx, 30, |ws| !ws.browse().loading);
            let b = ws.browse().clone();
            println!(
                "search {text:?} sorted by {:?}: total={} pages={} on this page={} error={:?}",
                sort,
                b.total,
                b.pages(),
                b.items.len(),
                b.error
            );
            for id in b.items.iter().take(5) {
                let item = ws.item(*id).cloned().unwrap_or_default();
                let s = ws.item_status(*id);
                println!(
                    "  {id}  {:<48}  subscribers={:?} rating={:?}% subscribed={} installed={}",
                    item.title,
                    item.subscribers,
                    item.rating_percent(),
                    s.subscribed,
                    s.installed
                );
                println!("      {}", plain_text(&item.description, 100));
                println!("      preview: {}", item.preview_url);
            }
        }

        if let Some(id) = ws.browse().items.first().copied() {
            let mut path = None;
            pump(&mut ws, &ctx, 30, |ws| {
                path = ws.preview(&ctx, id);
                path.is_some() || matches!(ws.previews.state.get(&id), Some(Preview::Missing))
            });
            let size = path.as_ref().and_then(|p| std::fs::metadata(p).ok()).map(|m| m.len());
            println!("preview of {id}: {path:?} ({size:?} bytes)");
        }

        ws.disconnect();
        println!(
            "status after disconnect: {:?}; Steam stopped counting the game as running after {:?} s",
            ws.status(),
            wait_until_not_running(30)
        );
        println!("SteamAppId in the launcher's own environment: {:?}", std::env::var_os("SteamAppId"));

        ws.connect(&ctx);
        pump(&mut ws, &ctx, 40, |ws| !matches!(ws.status(), Status::Connecting));
        println!("status after connecting a second time: {:?}; Running flag: {:?}", ws.status(), steam_says_running());
        pump(&mut ws, &ctx, 10, |ws| ws.subscribed().is_some());
        println!("subscribed items on the second connection: {:?}", ws.subscribed().map(<[u64]>::len));
        // Letting go without disconnecting (what happens when the launcher just closes).
        drop(ws);
        println!("after dropping the connection: not running after {:?} s", wait_until_not_running(30));
    }

    /// A helper exe without the DLL beside it must report that, not crash.
    /// Set COMMAND_CENTER_HELPER_EXE to a copy of the launcher exe in a folder without the DLL.
    #[test]
    #[ignore = "needs COMMAND_CENTER_HELPER_EXE"]
    fn live_missing_dll() {
        let ctx = egui::Context::default();
        let mut ws = Workshop::default();
        ws.connect(&ctx);
        pump(&mut ws, &ctx, 40, |ws| !matches!(ws.status(), Status::Connecting));
        println!("status with no DLL next to the helper: {:?}", ws.status());
        assert!(matches!(ws.status(), Status::Failed(f) if f.reason.contains(DLL_NAME)));
    }
}

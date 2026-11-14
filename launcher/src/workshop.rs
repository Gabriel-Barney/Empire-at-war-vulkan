//! Steam Workshop for Empire at War, without any Valve code in the launcher.
//!
//! The launcher ships no Steamworks DLL (Gabriel, 2026-11-13). Where each thing comes from:
//!
//! * Subscriptions and install state: Steam's own records on this PC,
//!   `appworkshop_<appid>.acf`, and the item folders under `workshop\content\<appid>`. An
//!   item Steam is fetching has a folder under `workshop\downloads\<appid>`; its size
//!   against the item's size is the download progress.
//! * Titles, descriptions, preview images and sizes: Steam's public web API
//!   (`ISteamRemoteStorage/GetPublishedFileDetails`), which needs no key.
//! * Searching the Workshop: `IPublishedFileService/QueryFiles`, which Steam only answers
//!   with a Steam Web API key. With the user's own key (Settings) the Browse tab searches
//!   inside the launcher; without one it opens the same search in the Steam client.
//! * Subscribing, unsubscribing and updating: the Steam client does those. The launcher
//!   opens the item's page (`steam://url/CommunityFilePage/<id>`) or Steam's Downloads page,
//!   then watches Steam's records for the result. Several subscriptions (a profile import)
//!   open one after the other, each once the previous one shows up in Steam's records.
//! * Web requests run on one worker thread; results arrive through a channel and wake the
//!   UI with `request_repaint`. Preview images come only from Steam's own hosts, over
//!   HTTPS, size-limited, a few at a time, cached on disk.

use crate::config;
use serde::{Deserialize, Serialize};
use std::collections::{BTreeMap, HashMap, HashSet, VecDeque};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::mpsc::{self, Receiver, Sender, TryRecvError};
use std::sync::{Arc, Mutex, OnceLock};
use std::time::{Duration, Instant};

/// Star Wars: Empire at War (Gold Pack) on Steam.
pub const APP_ID: u32 = 32470;

/// Workshop items per search page.
pub const PAGE_SIZE: u32 = 50;
/// "Most popular" ranks by subscriptions gained over this many days.
const TREND_DAYS: u32 = 30;
const REQUEST_TIMEOUT: Duration = Duration::from_secs(35);
/// How long a subscribe or unsubscribe handed to the Steam client is waited for.
const STEAM_WAIT: Duration = Duration::from_secs(180);
/// How often Steam's records are looked at: normally, and while something is changing.
const LOCAL_EVERY: Duration = Duration::from_secs(5);
const LOCAL_EVERY_BUSY: Duration = Duration::from_secs(1);
/// After a web request failed, quiet time before the next failure is reported.
const ERROR_QUIET: Duration = Duration::from_secs(60);
const DETAILS_URL: &str = "https://api.steampowered.com/ISteamRemoteStorage/GetPublishedFileDetails/v1/";
const QUERY_URL: &str = "https://api.steampowered.com/IPublishedFileService/QueryFiles/v1/";
const JSON_MAX_BYTES: u64 = 16 * 1024 * 1024;
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
    /// Size of the item's files in bytes (0 = not reported).
    #[serde(default)]
    pub file_size: u64,
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
    /// Download progress in percent, when the total is known.
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

    /// `query_type` for `QueryFiles` (Steam's EPublishedFileQueryType). A search ranked by
    /// popularity is ranked by how well the text matches instead, as on the Workshop site.
    fn query_type(self, searching: bool) -> u32 {
        match self {
            Sort::Popular if searching => 12, // RankedByTextSearch
            Sort::Popular => 3,               // RankedByTrend
            Sort::Subscribed => 9,            // RankedByTotalUniqueSubscriptions
            Sort::Updated => 21,              // RankedByLastUpdatedDate
            Sort::Rated => 0,                 // RankedByVote
        }
    }

    /// `browsesort` on the Workshop site.
    fn site_key(self) -> &'static str {
        match self {
            Sort::Popular => "trend",
            Sort::Subscribed => "totaluniquesubscribers",
            Sort::Updated => "lastupdated",
            Sort::Rated => "toprated",
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
/// mods have real names without asking Steam every time.
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

/// Opens the game's Workshop front page in the Steam client.
pub fn open_workshop_home() -> Result<(), String> {
    open_url(&format!("steam://url/SteamWorkshopPage/{APP_ID}"))
}

/// Opens one Workshop item's page in the Steam client: where Subscribe and Unsubscribe are.
pub fn open_item_page(id: u64) -> Result<(), String> {
    open_url(&format!("steam://url/CommunityFilePage/{id}"))
}

/// Opens Steam's Downloads page, where Workshop updates are queued and can be started.
pub fn open_downloads() -> Result<(), String> {
    open_url("steam://open/downloads")
}

/// The Workshop site's search for the game, as a URL.
pub fn search_url(text: &str, sort: Sort) -> String {
    let mut url = format!(
        "https://steamcommunity.com/workshop/browse/?appid={APP_ID}&browsesort={}&section=readytouseitems",
        sort.site_key()
    );
    if sort == Sort::Popular {
        url.push_str(&format!("&days={TREND_DAYS}"));
    }
    let text = text.trim();
    if !text.is_empty() {
        url.push_str("&searchtext=");
        url.push_str(&encode_component(text));
    }
    url
}

/// Opens a Workshop search in the Steam client's own browser.
pub fn open_search_in_steam(text: &str, sort: Sort) -> Result<(), String> {
    open_url(&format!("steam://openurl/{}", search_url(text, sort)))
}

/// Percent-encodes everything but unreserved characters (RFC 3986), for a URL query value.
fn encode_component(text: &str) -> String {
    let mut out = String::with_capacity(text.len());
    for b in text.bytes() {
        if b.is_ascii_alphanumeric() || matches!(b, b'-' | b'_' | b'.' | b'~') {
            out.push(b as char);
        } else {
            out.push_str(&format!("%{b:02X}"));
        }
    }
    out
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

/// Steam's download folder for the game's Workshop items (`...\workshop\downloads\<appid>`),
/// given its content folder (`...\workshop\content\<appid>`).
pub fn downloads_dir(workshop_content_dir: &Path) -> Option<PathBuf> {
    Some(workshop_content_dir.parent()?.parent()?.join("downloads").join(APP_ID.to_string()))
}

/// Builds what the launcher shows for one item from Steam's records and the folders on disk.
/// `downloading` is the byte count in its download folder, when it has one.
fn local_status(
    record: Option<&LocalItem>,
    pending: bool,
    folder: Option<PathBuf>,
    downloading: Option<u64>,
    file_size: u64,
) -> ItemStatus {
    let Some(record) = record else {
        // Not in Steam's records (yet), but Steam may already be fetching it.
        return ItemStatus {
            subscribed: downloading.is_some(),
            downloading: downloading.is_some(),
            bytes_done: downloading.unwrap_or(0),
            bytes_total: file_size,
            ..Default::default()
        };
    };
    let installed = record.installed && folder.is_some();
    let needs_update = installed && record.update_available();
    // Recorded but not on disk: Steam has it queued (or is about to, when `pending`).
    let queued = !installed && (pending || record.latest_time_updated > 0 || !record.latest_manifest.is_empty());
    ItemStatus {
        subscribed: true,
        installed,
        needs_update,
        downloading: downloading.is_some() || queued,
        bytes_done: downloading.unwrap_or(0),
        bytes_total: file_size,
        size_on_disk: if installed { record.size } else { 0 },
        folder: folder.filter(|_| installed),
    }
}

// ------------------------------------------------------------------ web requests

enum Job {
    Details(Vec<u64>),
    Query { req: u64, key: String, text: String, sort: Sort, page: u32 },
}

enum Done {
    Details(Result<Vec<Item>, String>),
    Page { req: u64, result: Result<(Vec<Item>, u32), String> },
}

struct Web {
    tx: Sender<Job>,
    rx: Receiver<Done>,
}

/// Starts the worker thread that makes the web requests, one at a time.
fn start_web(ctx: &egui::Context) -> Option<Web> {
    let (job_tx, job_rx) = mpsc::channel::<Job>();
    let (done_tx, done_rx) = mpsc::channel::<Done>();
    let ctx = ctx.clone();
    std::thread::Builder::new()
        .name("workshop-web".into())
        .spawn(move || {
            for job in job_rx {
                let done = match job {
                    Job::Details(ids) => Done::Details(fetch_details(&ids)),
                    Job::Query { req, key, text, sort, page } => {
                        Done::Page { req, result: fetch_page(&key, &text, sort, page) }
                    }
                };
                if done_tx.send(done).is_err() {
                    break;
                }
                ctx.request_repaint();
            }
        })
        .ok()?;
    Some(Web { tx: job_tx, rx: done_rx })
}

/// A request that went wrong, in words for the user.
fn web_error(error: ureq::Error) -> String {
    match error {
        ureq::Error::StatusCode(401 | 403) => {
            "Steam refused the request. Check the Steam Web API key in Settings.".to_owned()
        }
        ureq::Error::StatusCode(429) => "Steam says too many requests. Try again in a moment.".to_owned(),
        ureq::Error::StatusCode(code) if code >= 500 => format!("Steam's web service had a problem (HTTP {code})."),
        ureq::Error::StatusCode(code) => format!("Steam answered HTTP {code}."),
        ureq::Error::Timeout(_) => "Steam didn't answer in time.".to_owned(),
        other => format!("Couldn't reach Steam: {other}"),
    }
}

fn read_json(response: &mut ureq::http::Response<ureq::Body>) -> Result<serde_json::Value, String> {
    let text = response
        .body_mut()
        .with_config()
        .limit(JSON_MAX_BYTES)
        .read_to_string()
        .map_err(|e| format!("Couldn't read Steam's answer: {e}"))?;
    serde_json::from_str(&text).map_err(|_| "Steam's answer wasn't readable.".to_owned())
}

/// A number Steam sends either as a JSON number or as text.
fn number(value: &serde_json::Value) -> Option<u64> {
    value.as_u64().or_else(|| value.as_str().and_then(|s| s.trim().parse().ok()))
}

/// One entry of `publishedfiledetails` (both web APIs use this shape). `None` for entries
/// Steam marks as failed (`result` other than 1: deleted, hidden, unknown id).
fn parse_item(value: &serde_json::Value) -> Option<Item> {
    if value.get("result").and_then(number).is_some_and(|r| r != 1) {
        return None;
    }
    let id = value.get("publishedfileid").and_then(number).filter(|id| *id > 0)?;
    let text = |key: &str| value.get(key).and_then(|v| v.as_str()).unwrap_or_default();
    let description = ["description", "file_description", "short_description"]
        .into_iter()
        .map(text)
        .find(|d| !d.trim().is_empty())
        .unwrap_or_default();
    let votes = value.get("vote_data");
    let vote = |key: &str| votes.and_then(|v| v.get(key)).and_then(number).unwrap_or(0);
    let tags = value
        .get("tags")
        .and_then(|t| t.as_array())
        .map(|tags| {
            tags.iter()
                .filter_map(|t| t.get("tag").and_then(|s| s.as_str()).or_else(|| t.as_str()))
                .map(|s| s.trim().to_owned())
                .filter(|s| !s.is_empty())
                .collect()
        })
        .unwrap_or_default();
    Some(Item {
        id,
        title: text("title").trim().to_owned(),
        description: plain_text(description, 600),
        preview_url: text("preview_url").trim().to_owned(),
        subscribers: value.get("subscriptions").and_then(number),
        votes_up: u32::try_from(vote("votes_up")).unwrap_or(u32::MAX),
        votes_down: u32::try_from(vote("votes_down")).unwrap_or(u32::MAX),
        score: votes.and_then(|v| v.get("score")).and_then(|s| s.as_f64()).unwrap_or(0.0) as f32,
        updated: value.get("time_updated").and_then(number).map_or(0, |t| i64::try_from(t).unwrap_or(0)),
        tags,
        file_size: value.get("file_size").and_then(number).unwrap_or(0),
    })
}

/// The items of an answer from either web API.
fn parse_items(json: &serde_json::Value) -> Vec<Item> {
    json.pointer("/response/publishedfiledetails")
        .and_then(|list| list.as_array())
        .map(|list| list.iter().filter_map(parse_item).collect())
        .unwrap_or_default()
}

/// Title, description, preview and size of these items. Needs no key.
fn fetch_details(ids: &[u64]) -> Result<Vec<Item>, String> {
    let mut form: Vec<(String, String)> = vec![("itemcount".to_owned(), ids.len().to_string())];
    for (i, id) in ids.iter().enumerate() {
        form.push((format!("publishedfileids[{i}]"), id.to_string()));
    }
    let mut response = http_agent()
        .post(DETAILS_URL)
        .send_form(form.iter().map(|(k, v)| (k.as_str(), v.as_str())))
        .map_err(web_error)?;
    Ok(parse_items(&read_json(&mut response)?))
}

/// The `QueryFiles` request for one search page, without the key.
fn query_url(text: &str, sort: Sort, page: u32) -> String {
    let text = text.trim();
    let mut url = format!(
        "{QUERY_URL}?appid={APP_ID}&creator_appid={APP_ID}&filetype=0&query_type={}&page={}&numperpage={PAGE_SIZE}&return_vote_data=true&return_tags=true",
        sort.query_type(!text.is_empty()),
        page.max(1)
    );
    if sort == Sort::Popular && text.is_empty() {
        url.push_str(&format!("&days={TREND_DAYS}"));
    }
    if !text.is_empty() {
        url.push_str("&search_text=");
        url.push_str(&encode_component(text));
    }
    url
}

/// One page of a Workshop search, and the number of matches. Needs a Steam Web API key.
fn fetch_page(key: &str, text: &str, sort: Sort, page: u32) -> Result<(Vec<Item>, u32), String> {
    let url = format!("{}&key={}", query_url(text, sort, page), encode_component(key.trim()));
    let mut response = http_agent().get(&url).call().map_err(web_error)?;
    let json = read_json(&mut response)?;
    let total = json.pointer("/response/total").and_then(number).unwrap_or(0);
    Ok((parse_items(&json), u32::try_from(total).unwrap_or(u32::MAX)))
}

// ------------------------------------------------------------------ the handle pages use

/// A subscribe or unsubscribe handed to the Steam client, waiting to show up in its records.
struct Pending {
    since: Instant,
    subscribe: bool,
}

/// The launcher's view of the game's Workshop. `Default` is cheap and does nothing until
/// [`Workshop::configure`] and [`Workshop::poll`] are called.
#[derive(Default)]
pub struct Workshop {
    ctx: Option<egui::Context>,
    /// `...\steamapps\workshop\content\<appid>`.
    content_dir: Option<PathBuf>,
    api_key: String,
    web: Option<Web>,
    last_web_error: Option<Instant>,
    items: HashMap<u64, Item>,
    /// Ids whose details were asked for in this session.
    asked: HashSet<u64>,
    titles: Option<Titles>,
    previews: Previews,
    browse: Browse,
    browse_req: u64,
    browse_sent: Option<Instant>,

    /// Steam's records as last read, and the file's time and size then.
    local: Option<LocalState>,
    local_file: Option<(Option<std::time::SystemTime>, u64)>,
    local_read: bool,
    local_checked: Option<Instant>,
    local_failures: u32,
    /// What Steam's records and folders say about each item they mention.
    states: HashMap<u64, ItemStatus>,
    subscribed: Vec<u64>,
    /// Bytes in each download folder, measured on a worker thread.
    downloads: HashMap<u64, u64>,
    measuring: Arc<AtomicBool>,
    measured: Option<(Sender<HashMap<u64, u64>>, Receiver<HashMap<u64, u64>>)>,

    busy: HashMap<u64, Pending>,
    /// Subscriptions still to be opened in Steam, one after the other.
    queue: VecDeque<u64>,
    /// Until then, read Steam's records every second.
    watch_until: Option<Instant>,
    notices: Vec<Notice>,
}

impl Workshop {
    /// Tells the handle where Steam keeps the game's Workshop items and which key (if any)
    /// to search with. Cheap; call every frame before [`Workshop::poll`].
    pub fn configure(&mut self, ctx: &egui::Context, content_dir: Option<&Path>, api_key: &str) {
        if self.ctx.is_none() {
            self.ctx = Some(ctx.clone());
        }
        if self.content_dir.as_deref() != content_dir {
            self.content_dir = content_dir.map(Path::to_path_buf);
            self.local = None;
            self.local_file = None;
            self.local_read = false;
            self.local_checked = None;
            self.states.clear();
            self.subscribed.clear();
        }
        if self.api_key != api_key.trim() {
            self.api_key = api_key.trim().to_owned();
            // A search made without the key (or with another one) is out of date.
            self.browse.started = false;
        }
    }

    /// True when searches run inside the launcher (a Steam Web API key is set).
    pub fn can_search(&self) -> bool {
        !self.api_key.is_empty()
    }

    fn send(&mut self, job: Job) -> bool {
        if self.web.is_none() {
            self.web = self.ctx.as_ref().and_then(start_web);
        }
        self.web.as_ref().is_some_and(|w| w.tx.send(job).is_ok())
    }

    fn report_web_error(&mut self, error: String) {
        if self.last_web_error.is_some_and(|t| t.elapsed() < ERROR_QUIET) {
            return;
        }
        self.last_web_error = Some(Instant::now());
        self.notices.push(Notice::Error(format!("Couldn't get Workshop details from Steam. {error}")));
    }

    fn watch_closely(&mut self) {
        self.watch_until = Some(Instant::now() + STEAM_WAIT);
        self.local_checked = None;
    }

    /// Collects web answers, reads Steam's records when they changed and follows what was
    /// handed to the Steam client. Call once per frame. Cheap when nothing happened.
    pub fn poll(&mut self, ctx: &egui::Context) -> Vec<Notice> {
        self.previews.collect();
        let mut titles_changed = false;

        let mut done = Vec::new();
        if let Some(web) = &self.web {
            loop {
                match web.rx.try_recv() {
                    Ok(d) => done.push(d),
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => {
                        self.web = None;
                        break;
                    }
                }
            }
        }
        for d in done {
            match d {
                Done::Details(Ok(items)) => {
                    for item in items {
                        titles_changed |= self.remember_title(&item);
                        self.items.insert(item.id, item);
                    }
                }
                Done::Details(Err(e)) => self.report_web_error(e),
                Done::Page { req, result } => {
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
            }
        }

        if let Some((_, rx)) = &self.measured {
            if let Some(sizes) = rx.try_iter().last() {
                self.downloads = sizes;
                self.local_checked = None;
            }
        }

        let now = Instant::now();
        let watching = self.watch_until.is_some_and(|t| now < t);
        let busy = watching || !self.busy.is_empty() || !self.downloads.is_empty();
        let every = if busy { LOCAL_EVERY_BUSY } else { LOCAL_EVERY };
        if self.local_checked.is_none_or(|t| t.elapsed() >= every) {
            self.local_checked = Some(now);
            self.refresh_local(ctx);
        }

        // Follow what was handed to the Steam client.
        let mut finished = Vec::new();
        for (id, p) in &self.busy {
            let subscribed = self.is_recorded(*id);
            if p.subscribe == subscribed {
                finished.push((*id, true, p.subscribe));
            } else if now.duration_since(p.since) > STEAM_WAIT {
                finished.push((*id, false, p.subscribe));
            }
        }
        for (id, ok, subscribe) in finished {
            self.busy.remove(&id);
            let title = self.title(id);
            self.notices.push(Notice::Info(match (ok, subscribe) {
                (true, true) => format!("Subscribed to {title}. Steam downloads it next."),
                (true, false) => format!("Unsubscribed from {title}."),
                (false, true) => format!("Steam hasn't recorded a subscription to {title}."),
                (false, false) => format!("Steam still lists {title} as subscribed."),
            }));
            if ok && subscribe {
                if let Some(item) = self.items.get(&id).cloned() {
                    titles_changed |= self.remember_title(&item);
                }
            }
        }
        self.open_next_subscription();

        // Names for subscriptions the launcher hasn't seen yet.
        let unnamed: Vec<u64> = self.subscribed.iter().copied().filter(|id| !self.items.contains_key(id)).collect();
        self.request_details(&unnamed);

        if self.browse.loading && self.browse_sent.is_some_and(|t| now.duration_since(t) > REQUEST_TIMEOUT) {
            self.browse.loading = false;
            self.browse_sent = None;
            self.browse_req += 1;
            self.browse.items.clear();
            self.browse.total = 0;
            self.browse.error = Some("Steam didn't answer in time.".to_owned());
        }
        if busy || self.browse.loading {
            ctx.request_repaint_after(Duration::from_secs(1));
        }

        if titles_changed {
            if let Some(titles) = &self.titles {
                if let Err(e) = save_titles(titles) {
                    self.notices.push(Notice::Error(e));
                }
            }
            self.notices.push(Notice::TitlesChanged);
        }
        std::mem::take(&mut self.notices)
    }

    /// Reads Steam's records again if the file changed, looks at the download folders, and
    /// works out each item's state.
    fn refresh_local(&mut self, ctx: &egui::Context) {
        let Some(content) = self.content_dir.clone() else {
            self.local_read = false;
            return;
        };
        if let Some(path) = local_state_path(&content) {
            let file = std::fs::metadata(&path).ok().map(|m| (m.modified().ok(), m.len()));
            if file != self.local_file || !self.local_read {
                match LocalState::read(&path) {
                    Ok(local) => {
                        self.local = local;
                        self.local_file = file;
                        self.local_read = true;
                        self.local_failures = 0;
                    }
                    Err(e) => {
                        // Steam may be in the middle of writing it: only report a lasting problem.
                        self.local_failures += 1;
                        if self.local_failures == 3 {
                            self.notices.push(Notice::Error(e));
                        }
                        if !self.local_read {
                            return;
                        }
                    }
                }
            }
        }

        // Items Steam is fetching right now have a folder in its download area.
        let fetching: Vec<(u64, PathBuf)> = downloads_dir(&content)
            .and_then(|dir| std::fs::read_dir(dir).ok())
            .map(|entries| {
                entries
                    .flatten()
                    .filter(|e| e.file_type().is_ok_and(|t| t.is_dir()))
                    .filter_map(|e| Some((e.file_name().to_str()?.parse::<u64>().ok()?, e.path())))
                    .collect()
            })
            .unwrap_or_default();
        self.downloads.retain(|id, _| fetching.iter().any(|(f, _)| f == id));
        for (id, _) in &fetching {
            self.downloads.entry(*id).or_insert(0);
        }
        if !fetching.is_empty() && !self.measuring.swap(true, Ordering::SeqCst) {
            let tx = self.measured.get_or_insert_with(mpsc::channel).0.clone();
            let (flag, ctx) = (self.measuring.clone(), ctx.clone());
            let spawned = std::thread::Builder::new().name("workshop-downloads".into()).spawn(move || {
                let sizes: HashMap<u64, u64> = fetching.into_iter().map(|(id, dir)| (id, folder_bytes(&dir))).collect();
                let _ = tx.send(sizes);
                flag.store(false, Ordering::SeqCst);
                ctx.request_repaint();
            });
            if spawned.is_err() {
                self.measuring.store(false, Ordering::SeqCst);
            }
        }

        let pending = self.local.as_ref().is_some_and(|l| l.pending);
        let mut ids: Vec<u64> = self.local.as_ref().map(|l| l.items.keys().copied().collect()).unwrap_or_default();
        ids.extend(self.downloads.keys().copied());
        ids.sort_unstable();
        ids.dedup();
        let mut states = HashMap::new();
        for id in &ids {
            let record = self.local.as_ref().and_then(|l| l.item(*id));
            let folder = Some(content.join(id.to_string())).filter(|f| f.is_dir());
            let size = self.items.get(id).map_or(0, |i| i.file_size);
            states.insert(*id, local_status(record, pending, folder, self.downloads.get(id).copied(), size));
        }

        // Say what finished, and have the mod list rescanned when files appeared or went.
        let mut rescan = false;
        for (id, new) in &states {
            match self.states.get(id) {
                Some(old) if new.ready() && !old.ready() => {
                    self.notices.push(Notice::Info(format!("{} is installed", self.title(*id))));
                    rescan = true;
                }
                Some(old) if old.installed != new.installed => rescan = true,
                None if self.local_read && new.installed && !self.states.is_empty() => rescan = true,
                _ => {}
            }
        }
        if self.states.iter().any(|(id, old)| old.installed && !states.contains_key(id)) {
            rescan = true;
        }
        if rescan {
            self.notices.push(Notice::Rescan);
        }
        self.subscribed = ids;
        self.states = states;
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

    /// Ids of the items Steam's records on this PC list for the game: what the account is
    /// subscribed to here. `None` while Steam's Workshop folder or records aren't known.
    pub fn subscribed(&self) -> Option<&[u64]> {
        (self.content_dir.is_some() && self.local_read).then_some(self.subscribed.as_slice())
    }

    fn is_recorded(&self, id: u64) -> bool {
        self.states.get(&id).is_some_and(|s| s.subscribed)
    }

    pub fn is_subscribed(&self, id: u64) -> bool {
        self.is_recorded(id)
    }

    /// Reads Steam's records again and asks for the subscriptions' details again.
    pub fn refresh_subscribed(&mut self) {
        self.local_file = None;
        self.local_read = false;
        self.local_checked = None;
        let ids = self.subscribed.clone();
        for id in &ids {
            self.asked.remove(id);
        }
        self.request_details(&ids);
    }

    /// Asks Steam's web API for the details (title, description, preview, size) of these
    /// items, once per session each.
    pub fn request_details(&mut self, ids: &[u64]) {
        let new: Vec<u64> = ids.iter().copied().filter(|id| *id > 0 && !self.asked.contains(id)).collect();
        for chunk in new.chunks(PAGE_SIZE as usize) {
            if self.send(Job::Details(chunk.to_vec())) {
                self.asked.extend(chunk.iter().copied());
            }
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

    /// Install state and download progress, from Steam's records and folders.
    pub fn item_status(&self, id: u64) -> ItemStatus {
        self.states.get(&id).cloned().unwrap_or_default()
    }

    /// Like [`Workshop::item_status`], but `None` for items Steam's records don't mention.
    pub fn known_status(&self, id: u64) -> Option<&ItemStatus> {
        self.states.get(&id)
    }

    /// A subscribe or unsubscribe for this item is with the Steam client, or waiting its turn.
    pub fn is_busy(&self, id: u64) -> bool {
        self.busy.contains_key(&id) || self.queue.contains(&id)
    }

    /// Runs a Workshop search inside the launcher (needs a key: see [`Workshop::can_search`]).
    /// `page` is 1-based. Results arrive in [`Workshop::browse`].
    pub fn search(&mut self, text: &str, sort: Sort, page: u32) {
        self.browse_req += 1;
        let page = page.max(1);
        let text = text.trim().to_owned();
        self.browse.text = text.clone();
        self.browse.sort = sort;
        self.browse.page = page;
        self.browse.started = true;
        self.browse.error = None;
        if self.api_key.is_empty() {
            self.browse.loading = false;
            self.browse.items.clear();
            self.browse.total = 0;
            return;
        }
        let job = Job::Query { req: self.browse_req, key: self.api_key.clone(), text, sort, page };
        if self.send(job) {
            self.browse.loading = true;
            self.browse_sent = Some(Instant::now());
        } else {
            self.browse.loading = false;
            self.browse.error = Some("The launcher couldn't start its web requests.".to_owned());
        }
    }

    pub fn browse(&self) -> &Browse {
        &self.browse
    }

    /// Opens the item's page in Steam to subscribe there, and watches for the result. Several
    /// calls open the pages one after the other.
    pub fn subscribe(&mut self, id: u64) {
        if self.is_recorded(id) || self.is_busy(id) {
            return;
        }
        self.queue.push_back(id);
        self.open_next_subscription();
    }

    fn open_next_subscription(&mut self) {
        if self.busy.values().any(|p| p.subscribe) {
            return;
        }
        while let Some(id) = self.queue.pop_front() {
            if self.is_recorded(id) {
                continue;
            }
            match open_item_page(id) {
                Ok(()) => {
                    self.busy.insert(id, Pending { since: Instant::now(), subscribe: true });
                    self.watch_closely();
                    let more = match self.queue.len() {
                        0 => String::new(),
                        1 => " The next one opens once Steam has it.".to_owned(),
                        n => format!(" The next of {n} opens once Steam has it."),
                    };
                    self.notices.push(Notice::Info(format!(
                        "{} is open in Steam: press Subscribe there.{more}",
                        self.title(id)
                    )));
                    return;
                }
                Err(e) => self.notices.push(Notice::Error(e)),
            }
        }
    }

    /// Opens the item's page in Steam to unsubscribe there, and watches for the result.
    pub fn unsubscribe(&mut self, id: u64) {
        if self.busy.contains_key(&id) {
            return;
        }
        self.queue.retain(|q| *q != id);
        match open_item_page(id) {
            Ok(()) => {
                self.busy.insert(id, Pending { since: Instant::now(), subscribe: false });
                self.watch_closely();
                self.notices.push(Notice::Info(format!("{} is open in Steam: press Unsubscribe there.", self.title(id))));
            }
            Err(e) => self.notices.push(Notice::Error(e)),
        }
    }

    /// Opens Steam's Downloads page, where the item's download or update can be started.
    pub fn download(&mut self, id: u64) {
        match open_downloads() {
            Ok(()) => {
                self.watch_closely();
                self.notices.push(Notice::Info(format!(
                    "Steam's Downloads page is open: start {} there if it isn't already queued. Steam also updates Workshop items on its own while the game isn't running.",
                    self.title(id)
                )));
            }
            Err(e) => self.notices.push(Notice::Error(e)),
        }
    }

    /// Path of the item's preview image once it is in the cache. The first call for an item
    /// queues the download; a later frame gets the path. `None` also when it has no preview.
    pub fn preview(&mut self, ctx: &egui::Context, id: u64) -> Option<PathBuf> {
        if !self.items.contains_key(&id) {
            self.request_details(&[id]);
        }
        let item = self.items.get(&id);
        self.previews.get(ctx, id, item)
    }
}

/// Total size of the files under `dir`, in bytes.
fn folder_bytes(dir: &Path) -> u64 {
    let mut total = 0u64;
    let mut stack = vec![dir.to_path_buf()];
    while let Some(d) = stack.pop() {
        let Ok(entries) = std::fs::read_dir(&d) else { continue };
        for entry in entries.flatten() {
            match entry.file_type() {
                Ok(t) if t.is_dir() => stack.push(entry.path()),
                Ok(_) => total += entry.metadata().map_or(0, |m| m.len()),
                Err(_) => {}
            }
        }
    }
    total
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

    /// Trimmed from a real `GetPublishedFileDetails` answer, plus a failed entry and a
    /// `QueryFiles`-shaped one (numbers as numbers, `file_description`, vote data, tags).
    const DETAILS_JSON: &str = r#"{"response":{"result":1,"resultcount":3,"total":1234,"publishedfiledetails":[
        {"publishedfileid":"1125571106","result":1,"creator_app_id":32470,"consumer_app_id":32470,"file_size":"26682023071",
         "preview_url":"https://images.steamusercontent.com/ugc/999180650299891967/53EBDB8E/",
         "title":"EaWX: Thrawn's Revenge 3.5.1","description":"[img]https://media.example/x.png[/img]\r\n\r\nThe death of the [b]Emperor[/b]",
         "time_created":1504034045,"time_updated":1785339388,"subscriptions":512345,"favorited":1000,
         "tags":[{"tag":"Mod"},{"tag":"FoC"}]},
        {"publishedfileid":"42","result":9},
        {"publishedfileid":7,"title":" Small mod ","file_description":"Adds a ship.","time_updated":5,"subscriptions":"12",
         "vote_data":{"score":0.75,"votes_up":30,"votes_down":10},"tags":[{"tag":"Units","display_name":"Units"}]}
    ]}}"#;

    #[test]
    fn web_answers_become_items() {
        let json: serde_json::Value = serde_json::from_str(DETAILS_JSON).unwrap();
        let items = parse_items(&json);
        assert_eq!(items.len(), 2, "the failed entry is left out");
        let tr = &items[0];
        assert_eq!(tr.id, 1125571106);
        assert_eq!(tr.title, "EaWX: Thrawn's Revenge 3.5.1");
        assert_eq!(tr.description, "The death of the Emperor");
        assert_eq!(tr.file_size, 26_682_023_071);
        assert_eq!(tr.subscribers, Some(512_345));
        assert_eq!(tr.updated, 1_785_339_388);
        assert_eq!(tr.tags, ["Mod", "FoC"]);
        assert_eq!(tr.rating_percent(), None);
        assert!(checked_url(&tr.preview_url).is_some());

        let small = &items[1];
        assert_eq!((small.id, small.title.as_str(), small.description.as_str()), (7, "Small mod", "Adds a ship."));
        assert_eq!(small.subscribers, Some(12));
        assert_eq!((small.votes_up, small.votes_down), (30, 10));
        assert_eq!(small.rating_percent(), Some(75));
        assert_eq!(json.pointer("/response/total").and_then(number), Some(1234));

        assert!(parse_items(&serde_json::json!({"response": {}})).is_empty());
        assert!(parse_items(&serde_json::json!([])).is_empty());
    }

    #[test]
    fn search_links_and_queries() {
        assert_eq!(encode_component("Thrawn's Revenge & co/2"), "Thrawn%27s%20Revenge%20%26%20co%2F2");
        assert_eq!(encode_component("ü"), "%C3%BC");
        assert_eq!(
            search_url("  thrawn  ", Sort::Popular),
            "https://steamcommunity.com/workshop/browse/?appid=32470&browsesort=trend&section=readytouseitems&days=30&searchtext=thrawn"
        );
        assert_eq!(
            search_url("", Sort::Updated),
            "https://steamcommunity.com/workshop/browse/?appid=32470&browsesort=lastupdated&section=readytouseitems"
        );
        let q = query_url("star destroyer", Sort::Popular, 0);
        assert!(q.starts_with(QUERY_URL));
        assert!(q.contains("&query_type=12&page=1&numperpage=50"), "{q}");
        assert!(q.ends_with("&search_text=star%20destroyer"), "{q}");
        assert!(!q.contains("key="));
        let q = query_url("", Sort::Popular, 3);
        assert!(q.contains("&query_type=3&page=3") && q.contains(&format!("&days={TREND_DAYS}")), "{q}");
        assert!(query_url("", Sort::Subscribed, 1).contains("&query_type=9&"));
    }

    #[test]
    fn item_state_from_records_and_folders() {
        let state = LocalState::parse(ACF).unwrap();
        let folder = || Some(PathBuf::from("F:\\ws\\1"));

        let current = local_status(state.item(1125571106), state.pending, folder(), None, 0);
        assert!(current.subscribed && current.installed && current.ready());
        assert_eq!(current.size_on_disk, 26_682_023_071);
        assert_eq!(current.folder, folder());

        let stale = local_status(state.item(1770851727), state.pending, folder(), None, 0);
        assert!(stale.installed && stale.needs_update && !stale.ready());

        // Recorded as installed, but the folder is gone: not installed.
        let gone = local_status(state.item(1125571106), false, None, None, 0);
        assert!(gone.subscribed && !gone.installed && gone.folder.is_none());

        // Subscribed, not on disk yet: queued; with a download folder, progress.
        let queued = local_status(state.item(3830298393), state.pending, None, None, 0);
        assert!(queued.subscribed && queued.downloading && queued.percent().is_none());
        let fetching = local_status(state.item(3830298393), false, None, Some(250), 1000);
        assert_eq!(fetching.percent(), Some(25));

        // Not in the records yet, but Steam is already fetching it.
        let new = local_status(None, false, None, Some(10), 0);
        assert!(new.subscribed && new.downloading && !new.installed);
        assert_eq!(local_status(None, false, None, None, 0), ItemStatus::default());

        assert_eq!(
            downloads_dir(Path::new("F:\\Lib\\steamapps\\workshop\\content\\32470")),
            Some(PathBuf::from("F:\\Lib\\steamapps\\workshop\\downloads\\32470"))
        );
    }

    /// Reads this PC's real Steam records through the handle the pages use, and fetches the
    /// subscriptions' names. Read-only. `cargo test live_local_records -- --ignored --nocapture`
    #[test]
    #[ignore = "needs Steam's Workshop folder for the game and the internet"]
    fn live_local_records() {
        let steam = crate::steam::detect();
        let Some(dir) = steam.workshop_dir.as_deref() else {
            println!("no Workshop folder on this PC");
            return;
        };
        let ctx = egui::Context::default();
        let mut ws = Workshop::default();
        let start = Instant::now();
        while start.elapsed() < Duration::from_secs(20) {
            ws.configure(&ctx, Some(dir), "");
            for notice in ws.poll(&ctx) {
                println!("  notice: {notice:?}");
            }
            if ws.subscribed().is_some_and(|ids| ids.iter().all(|id| ws.item(*id).is_some())) {
                break;
            }
            std::thread::sleep(Duration::from_millis(100));
        }
        let ids = ws.subscribed().expect("Steam's records were read").to_vec();
        println!("{} items in Steam's records under {}", ids.len(), dir.display());
        for id in &ids {
            let s = ws.item_status(*id);
            println!(
                "  {id}  {:<50} installed={} update={} downloading={} {:?}% on disk={}",
                ws.title(*id),
                s.installed,
                s.needs_update,
                s.downloading,
                s.percent(),
                s.size_on_disk
            );
        }
        assert!(ids.iter().all(|id| ws.known_status(*id).is_some_and(|s| s.subscribed)));
    }

    /// Asks Steam's real web API for one item's details (no key needed).
    /// `cargo test live_web_details -- --ignored --nocapture`
    #[test]
    #[ignore = "needs the internet"]
    fn live_web_details() {
        let items = fetch_details(&[1125571106, 1]).expect("details");
        for item in &items {
            println!("{} {:?} size={} subscribers={:?} tags={:?}", item.id, item.title, item.file_size, item.subscribers, item.tags);
        }
        assert!(items.iter().any(|i| i.id == 1125571106 && !i.title.is_empty() && i.file_size > 0));
        // Searching without a key is refused, and says so in words.
        let error = fetch_page("", "", Sort::Popular, 1).expect_err("QueryFiles without a key");
        println!("search without a key: {error}");
        assert!(error.contains("key"), "{error}");
    }
}

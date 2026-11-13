//! Where every open page lives: a tab in one of (at most) two panes of the main window, or
//! a window of its own.
//!
//! The model keeps three promises the UI relies on:
//! * a page is in exactly one place, so it is only ever drawn once per frame;
//! * the main window always has at least one tab;
//! * a pane that exists is never empty (closing its last tab closes the pane).

use crate::app::Page;
use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;

const MIN_RATIO: f32 = 0.2;
const MAX_RATIO: f32 = 0.8;

#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum Side {
    Left,
    Right,
}

impl Side {
    pub fn other(self) -> Side {
        match self {
            Side::Left => Side::Right,
            Side::Right => Side::Left,
        }
    }
}

/// Where a page is.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Place {
    /// Pane and position in its tab strip.
    Tab(Side, usize),
    Window,
}

#[derive(Clone, Debug, PartialEq, Default)]
pub struct Pane {
    tabs: Vec<Page>,
    active: usize,
}

impl Pane {
    fn of(page: Page) -> Self {
        Self { tabs: vec![page], active: 0 }
    }

    pub fn tabs(&self) -> &[Page] {
        &self.tabs
    }

    pub fn active(&self) -> usize {
        self.active
    }

    /// The page this pane shows. Panes are never empty.
    pub fn active_page(&self) -> Page {
        self.tabs.get(self.active).or(self.tabs.last()).copied().unwrap_or(Page::Play)
    }

    fn position(&self, page: Page) -> Option<usize> {
        self.tabs.iter().position(|p| *p == page)
    }

    /// Removes a tab. Closing the active tab shows its right-hand neighbour, like a browser.
    fn remove(&mut self, page: Page) -> bool {
        let Some(i) = self.position(page) else { return false };
        self.tabs.remove(i);
        if i < self.active {
            self.active -= 1;
        }
        self.active = self.active.min(self.tabs.len().saturating_sub(1));
        true
    }

    fn insert(&mut self, index: usize, page: Page, activate: bool) -> usize {
        let i = index.min(self.tabs.len());
        let was_empty = self.tabs.is_empty();
        self.tabs.insert(i, page);
        if activate || was_empty {
            self.active = i;
        } else if i <= self.active {
            self.active += 1;
        }
        i
    }

    fn show(&mut self, page: Page) {
        if let Some(i) = self.position(page) {
            self.active = i;
        }
    }
}

#[derive(Clone, Debug, PartialEq, Serialize, Deserialize)]
#[serde(from = "Saved", into = "Saved")]
pub struct Layout {
    left: Pane,
    right: Option<Pane>,
    /// Share of the content width the left pane gets while split.
    ratio: f32,
    focus: Side,
    windows: Vec<Page>,
    /// Last known inner size of each page's own window, in points.
    window_sizes: BTreeMap<Page, [f32; 2]>,
}

impl Default for Layout {
    fn default() -> Self {
        Self::single(Page::Play)
    }
}

impl Layout {
    /// One page, no tabs: the simple look.
    pub fn single(page: Page) -> Self {
        Self {
            left: Pane::of(page),
            right: None,
            ratio: 0.5,
            focus: Side::Left,
            windows: Vec::new(),
            window_sizes: BTreeMap::new(),
        }
    }

    // ---------------------------------------------------------------- reading

    pub fn is_split(&self) -> bool {
        self.right.is_some()
    }

    pub fn pane(&self, side: Side) -> Option<&Pane> {
        match side {
            Side::Left => Some(&self.left),
            Side::Right => self.right.as_ref(),
        }
    }

    /// `side` if that pane exists, else the left pane.
    fn existing(&self, side: Side) -> Side {
        if side == Side::Right && self.right.is_none() {
            Side::Left
        } else {
            side
        }
    }

    fn pane_mut(&mut self, side: Side) -> &mut Pane {
        match (side, self.right.as_mut()) {
            (Side::Right, Some(right)) => right,
            _ => &mut self.left,
        }
    }

    pub fn focus(&self) -> Side {
        self.existing(self.focus)
    }

    pub fn ratio(&self) -> f32 {
        self.ratio
    }

    pub fn windows(&self) -> &[Page] {
        &self.windows
    }

    /// Tabs in the main window, both panes together.
    pub fn main_tabs(&self) -> usize {
        self.left.tabs.len() + self.right.as_ref().map_or(0, |p| p.tabs.len())
    }

    pub fn find(&self, page: Page) -> Option<Place> {
        if let Some(i) = self.left.position(page) {
            return Some(Place::Tab(Side::Left, i));
        }
        if let Some(i) = self.right.as_ref().and_then(|p| p.position(page)) {
            return Some(Place::Tab(Side::Right, i));
        }
        self.windows.contains(&page).then_some(Place::Window)
    }

    pub fn is_open(&self, page: Page) -> bool {
        self.find(page).is_some()
    }

    /// The page shown in the focused pane of the main window.
    pub fn focused_page(&self) -> Page {
        self.pane(self.focus()).unwrap_or(&self.left).active_page()
    }

    /// Every page that is on screen right now (each pane's active tab, and every window).
    pub fn visible(&self) -> Vec<Page> {
        let mut out = vec![self.left.active_page()];
        out.extend(self.right.as_ref().map(Pane::active_page));
        out.extend(self.windows.iter().copied());
        out
    }

    pub fn window_size(&self, page: Page) -> Option<[f32; 2]> {
        self.window_sizes.get(&page).copied()
    }

    // ---------------------------------------------------------------- changing

    pub fn set_focus(&mut self, side: Side) {
        self.focus = self.existing(side);
    }

    pub fn set_ratio(&mut self, ratio: f32) {
        self.ratio = if ratio.is_finite() { ratio.clamp(MIN_RATIO, MAX_RATIO) } else { 0.5 };
    }

    pub fn set_window_size(&mut self, page: Page, size: [f32; 2]) {
        if let Some(size) = sane_size(size) {
            self.window_sizes.insert(page, size);
        }
    }

    /// Shows `page` and gives it the focus: its existing tab if it has one, otherwise a new
    /// tab in the focused pane. For a page in its own window nothing changes here; the
    /// caller brings that window forward.
    pub fn open(&mut self, page: Page) -> Place {
        self.open_in(page, self.focus())
    }

    /// Like [`Layout::open`], but a new tab goes into the pane on `side`.
    pub fn open_in(&mut self, page: Page, side: Side) -> Place {
        match self.find(page) {
            Some(Place::Tab(side, i)) => {
                self.pane_mut(side).active = i;
                self.focus = side;
                Place::Tab(side, i)
            }
            Some(Place::Window) => Place::Window,
            None => {
                let side = self.existing(side);
                let i = self.pane_mut(side).insert(usize::MAX, page, true);
                self.focus = side;
                Place::Tab(side, i)
            }
        }
    }

    /// Opens `page` as a tab without switching to it. Returns false if it was open already.
    pub fn open_background(&mut self, page: Page) -> bool {
        if self.is_open(page) {
            return false;
        }
        let side = self.focus();
        self.pane_mut(side).insert(usize::MAX, page, false);
        true
    }

    pub fn activate(&mut self, side: Side, index: usize) {
        let side = self.existing(side);
        let pane = self.pane_mut(side);
        if index < pane.tabs.len() {
            pane.active = index;
        }
        self.focus = side;
    }

    /// Closes a tab. The last tab of the main window can't be closed. For a page in its own
    /// window, "closing" brings it back as a tab.
    pub fn close(&mut self, page: Page) -> bool {
        match self.find(page) {
            None => false,
            Some(Place::Window) => self.dock(page),
            Some(Place::Tab(side, _)) => {
                if self.main_tabs() <= 1 {
                    return false;
                }
                self.pane_mut(side).remove(page);
                self.normalize();
                true
            }
        }
    }

    /// Closes every other tab in `page`'s pane.
    pub fn close_others(&mut self, page: Page) -> bool {
        let Some(Place::Tab(side, _)) = self.find(page) else { return false };
        let pane = self.pane_mut(side);
        let changed = pane.tabs.len() > 1;
        *pane = Pane::of(page);
        self.focus = side;
        changed
    }

    /// Moves `page` to position `index` of the pane on `side` (reordering, moving between
    /// panes, or bringing a window back as a tab). `index` counts the pane's other tabs.
    pub fn move_tab(&mut self, page: Page, side: Side, index: usize) -> bool {
        match self.find(page) {
            Some(Place::Tab(from, i)) if from == self.existing(side) => {
                let pane = self.pane_mut(from);
                let shown = pane.active_page();
                let at = index.min(pane.tabs.len() - 1);
                if at == i {
                    return false;
                }
                pane.tabs.remove(i);
                pane.tabs.insert(at, page);
                pane.show(shown);
                self.focus = from;
                true
            }
            Some(Place::Tab(from, _)) => {
                // Another pane, so the window is split and both panes exist.
                self.pane_mut(from).remove(page);
                self.pane_mut(side).insert(index, page, true);
                self.focus = side;
                self.normalize();
                true
            }
            Some(Place::Window) | None => {
                self.windows.retain(|p| *p != page);
                let side = self.existing(side);
                self.pane_mut(side).insert(index, page, true);
                self.focus = side;
                true
            }
        }
    }

    /// Puts `page` in the pane on `side`, splitting the main window if it isn't split yet.
    /// Fails only when `page` is the main window's one and only tab.
    pub fn beside(&mut self, page: Page, side: Side) -> bool {
        if self.is_split() {
            if let Some(Place::Tab(at, i)) = self.find(page) {
                if at == side {
                    self.activate(side, i);
                    return true;
                }
            }
            return self.move_tab(page, side, usize::MAX);
        }
        match self.find(page) {
            Some(Place::Tab(..)) => {
                if self.left.tabs.len() < 2 {
                    return false;
                }
                self.left.remove(page);
            }
            Some(Place::Window) => self.windows.retain(|p| *p != page),
            None => {}
        }
        let solo = Pane::of(page);
        match side {
            Side::Right => self.right = Some(solo),
            Side::Left => self.right = Some(std::mem::replace(&mut self.left, solo)),
        }
        self.focus = side;
        true
    }

    /// "Open to the side": the pane the page isn't in, creating it if needed.
    pub fn to_other_side(&mut self, page: Page) -> bool {
        let side = match self.find(page) {
            Some(Place::Tab(at, _)) if self.is_split() => at.other(),
            _ if self.is_split() => self.focus().other(),
            _ => Side::Right,
        };
        self.beside(page, side)
    }

    /// Splits the main window (the active tab moves to the right), or merges the two panes
    /// back into one. Returns false when there is only one tab, so nothing to split.
    pub fn toggle_split(&mut self) -> bool {
        let shown = self.focused_page();
        if let Some(right) = self.right.take() {
            self.left.tabs.extend(right.tabs);
            self.left.show(shown);
            self.focus = Side::Left;
            true
        } else if self.left.tabs.len() >= 2 {
            self.beside(shown, Side::Right)
        } else {
            false
        }
    }

    /// Moves `page` into a window of its own (opening it if it wasn't open). If it was the
    /// main window's only tab, the first page that isn't open takes its place.
    pub fn pop(&mut self, page: Page) -> bool {
        match self.find(page) {
            Some(Place::Window) => return true,
            Some(Place::Tab(side, _)) => {
                if self.main_tabs() <= 1 {
                    let Some(fill) = Page::ALL.iter().copied().find(|p| *p != page && !self.is_open(*p)) else {
                        return false;
                    };
                    self.pane_mut(side).insert(usize::MAX, fill, false);
                }
                self.pane_mut(side).remove(page);
                self.normalize();
            }
            None => {}
        }
        self.windows.push(page);
        true
    }

    /// Brings a page back from its own window as a tab in the focused pane.
    pub fn dock(&mut self, page: Page) -> bool {
        if !self.windows.contains(&page) {
            return false;
        }
        self.windows.retain(|p| *p != page);
        let side = self.focus();
        self.pane_mut(side).insert(usize::MAX, page, true);
        true
    }

    /// Next / previous tab in the focused pane.
    pub fn cycle(&mut self, forward: bool) {
        let side = self.focus();
        let pane = self.pane_mut(side);
        let n = pane.tabs.len();
        if n > 1 {
            pane.active = (pane.active + if forward { 1 } else { n - 1 }) % n;
        }
    }

    /// Drops panes that ran out of tabs.
    fn normalize(&mut self) {
        if self.right.as_ref().is_some_and(|p| p.tabs.is_empty()) {
            self.right = None;
        }
        if self.left.tabs.is_empty() {
            if let Some(right) = self.right.take() {
                self.left = right;
            }
            self.focus = Side::Left;
        }
        self.focus = self.existing(self.focus);
    }

    /// Repairs anything a hand-edited or older config could get wrong.
    fn sanitize(mut self) -> Self {
        let shown_left = self.left.tabs.get(self.left.active).copied();
        let shown_right = self.right.as_ref().and_then(|p| p.tabs.get(p.active).copied());
        let mut seen: Vec<Page> = Vec::new();
        let mut unique = |tabs: &mut Vec<Page>| {
            tabs.retain(|p| {
                let new = !seen.contains(p);
                if new {
                    seen.push(*p);
                }
                new
            });
        };
        unique(&mut self.left.tabs);
        if let Some(right) = &mut self.right {
            unique(&mut right.tabs);
        }
        unique(&mut self.windows);
        // Removing duplicates moves tabs along; keep the same pages in view.
        if let Some(page) = shown_left {
            self.left.show(page);
        }
        if let (Some(page), Some(right)) = (shown_right, self.right.as_mut()) {
            right.show(page);
        }
        self.normalize();
        if self.left.tabs.is_empty() {
            // Everything was in windows (or nothing was open): the main window still needs a page.
            let page = Page::ALL.iter().copied().find(|p| !self.windows.contains(p)).unwrap_or(Page::Play);
            self.windows.retain(|p| *p != page);
            self.left = Pane::of(page);
        }
        self.left.active = self.left.active.min(self.left.tabs.len() - 1);
        if let Some(right) = &mut self.right {
            right.active = right.active.min(right.tabs.len() - 1);
        }
        self.focus = self.existing(self.focus);
        let ratio = self.ratio;
        self.set_ratio(ratio);
        self.window_sizes = self.window_sizes.into_iter().filter_map(|(p, s)| Some((p, sane_size(s)?))).collect();
        self
    }

    /// Checks the promises in the module docs. Used by the tests.
    #[cfg(test)]
    fn check(&self) -> Result<(), String> {
        if self.left.tabs.is_empty() {
            return Err("the main window has no tabs".into());
        }
        if self.right.as_ref().is_some_and(|p| p.tabs.is_empty()) {
            return Err("the right pane exists but is empty".into());
        }
        for (name, pane) in [("left", Some(&self.left)), ("right", self.right.as_ref())] {
            if let Some(pane) = pane {
                if pane.active >= pane.tabs.len() {
                    return Err(format!("{name} pane: active tab {} of {}", pane.active, pane.tabs.len()));
                }
            }
        }
        if self.focus == Side::Right && self.right.is_none() {
            return Err("focus is on a pane that doesn't exist".into());
        }
        let mut all: Vec<Page> = self.left.tabs.clone();
        all.extend(self.right.iter().flat_map(|p| p.tabs.iter().copied()));
        all.extend(self.windows.iter().copied());
        for page in Page::ALL {
            let n = all.iter().filter(|p| **p == page).count();
            if n > 1 {
                return Err(format!("{page:?} is open {n} times"));
            }
        }
        let visible = self.visible();
        for page in &visible {
            if visible.iter().filter(|p| *p == page).count() != 1 {
                return Err(format!("{page:?} would be drawn twice"));
            }
        }
        Ok(())
    }
}

fn sane_size(size: [f32; 2]) -> Option<[f32; 2]> {
    let ok = |v: f32| v.is_finite() && (200.0..=20_000.0).contains(&v);
    (ok(size[0]) && ok(size[1])).then(|| [size[0].round(), size[1].round()])
}

// ------------------------------------------------------------------ persistence

fn half() -> f32 {
    0.5
}

/// The layout as it is written to the config. Pages are stored by name and everything has a
/// default, so an unknown page or a missing field never makes the whole config unreadable.
#[derive(Serialize, Deserialize, Clone, Debug, Default)]
struct Saved {
    #[serde(default)]
    left: Vec<String>,
    #[serde(default)]
    left_active: usize,
    #[serde(default)]
    right: Vec<String>,
    #[serde(default)]
    right_active: usize,
    #[serde(default = "half")]
    ratio: f32,
    #[serde(default)]
    focus_right: bool,
    #[serde(default)]
    windows: Vec<String>,
    #[serde(default)]
    window_sizes: BTreeMap<String, [f32; 2]>,
}

fn names(pages: &[Page]) -> Vec<String> {
    pages.iter().map(|p| p.key().to_owned()).collect()
}

fn pages(names: &[String]) -> Vec<Page> {
    names.iter().filter_map(|n| Page::from_key(n)).collect()
}

impl From<Layout> for Saved {
    fn from(l: Layout) -> Self {
        let right = l.right.unwrap_or_default();
        Saved {
            left: names(&l.left.tabs),
            left_active: l.left.active,
            right: names(&right.tabs),
            right_active: right.active,
            ratio: l.ratio,
            focus_right: l.focus == Side::Right,
            windows: names(&l.windows),
            window_sizes: l.window_sizes.iter().map(|(p, s)| (p.key().to_owned(), *s)).collect(),
        }
    }
}

impl From<Saved> for Layout {
    fn from(s: Saved) -> Self {
        // The active tab is remembered by position; find the page first so dropping an
        // unknown name before it doesn't shift the selection.
        let pane = |tabs: &[String], active: usize| {
            let shown = tabs.get(active).and_then(|n| Page::from_key(n));
            let mut pane = Pane { tabs: pages(tabs), active: 0 };
            if let Some(page) = shown {
                pane.show(page);
            }
            pane
        };
        let right = pane(&s.right, s.right_active);
        Layout {
            left: pane(&s.left, s.left_active),
            right: (!right.tabs.is_empty()).then_some(right),
            ratio: s.ratio,
            focus: if s.focus_right { Side::Right } else { Side::Left },
            windows: pages(&s.windows),
            window_sizes: s.window_sizes.iter().filter_map(|(n, size)| Some((Page::from_key(n)?, *size))).collect(),
        }
        .sanitize()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tabs(l: &Layout, side: Side) -> Vec<Page> {
        l.pane(side).map(|p| p.tabs().to_vec()).unwrap_or_default()
    }

    #[test]
    fn layout_opening_focuses_an_existing_tab() {
        let mut l = Layout::single(Page::Play);
        assert_eq!(l.open(Page::Mods), Place::Tab(Side::Left, 1));
        assert_eq!(l.open(Page::Saves), Place::Tab(Side::Left, 2));
        assert_eq!(l.focused_page(), Page::Saves);
        // Already open: no new tab, just focus.
        assert_eq!(l.open(Page::Mods), Place::Tab(Side::Left, 1));
        assert_eq!(tabs(&l, Side::Left), [Page::Play, Page::Mods, Page::Saves]);
        assert_eq!(l.focused_page(), Page::Mods);
        l.check().unwrap();
    }

    #[test]
    fn layout_background_tabs_do_not_steal_the_view() {
        let mut l = Layout::single(Page::Play);
        assert!(l.open_background(Page::Units));
        assert!(!l.open_background(Page::Units));
        assert_eq!(l.focused_page(), Page::Play);
        assert_eq!(tabs(&l, Side::Left), [Page::Play, Page::Units]);
    }

    #[test]
    fn layout_last_tab_cannot_be_closed() {
        let mut l = Layout::single(Page::Play);
        assert!(!l.close(Page::Play));
        l.open(Page::Mods);
        assert!(l.close(Page::Play));
        assert!(!l.close(Page::Mods));
        assert_eq!(tabs(&l, Side::Left), [Page::Mods]);
        // A page that isn't open can't be closed either.
        assert!(!l.close(Page::Console));
        l.check().unwrap();
    }

    #[test]
    fn layout_closing_the_active_tab_shows_its_neighbour() {
        let mut l = Layout::single(Page::Play);
        l.open(Page::Mods);
        l.open(Page::Saves);
        l.open(Page::Mods);
        assert!(l.close(Page::Mods));
        assert_eq!(l.focused_page(), Page::Saves);
        assert!(l.close(Page::Saves));
        assert_eq!(l.focused_page(), Page::Play);
    }

    #[test]
    fn layout_split_and_merge() {
        let mut l = Layout::single(Page::Play);
        // One tab: nothing to split.
        assert!(!l.toggle_split());
        assert!(!l.to_other_side(Page::Play));
        l.open(Page::Mods);
        assert!(l.toggle_split());
        assert_eq!(tabs(&l, Side::Left), [Page::Play]);
        assert_eq!(tabs(&l, Side::Right), [Page::Mods]);
        assert_eq!(l.focus(), Side::Right);
        assert_eq!(l.visible(), [Page::Play, Page::Mods]);
        // A new page opens in the focused pane.
        l.open(Page::Saves);
        assert_eq!(tabs(&l, Side::Right), [Page::Mods, Page::Saves]);
        // Closing the last tab of a pane closes the pane.
        assert!(l.close(Page::Play));
        assert!(!l.is_split());
        assert_eq!(tabs(&l, Side::Left), [Page::Mods, Page::Saves]);
        assert_eq!(l.focus(), Side::Left);
        l.check().unwrap();
        // Merge keeps the page that was focused in view.
        assert!(l.toggle_split());
        l.set_focus(Side::Left);
        assert!(l.toggle_split());
        assert!(!l.is_split());
        assert_eq!(l.focused_page(), Page::Mods);
    }

    #[test]
    fn layout_dropping_on_the_left_edge_puts_the_page_on_the_left() {
        let mut l = Layout::single(Page::Play);
        l.open(Page::Mods);
        l.open(Page::Saves);
        assert!(l.beside(Page::Saves, Side::Left));
        assert_eq!(tabs(&l, Side::Left), [Page::Saves]);
        assert_eq!(tabs(&l, Side::Right), [Page::Play, Page::Mods]);
        assert_eq!(l.focus(), Side::Left);
        // Moving the only tab of a pane to the other side closes that pane.
        assert!(l.to_other_side(Page::Saves));
        assert!(!l.is_split());
        assert_eq!(tabs(&l, Side::Left), [Page::Play, Page::Mods, Page::Saves]);
        l.check().unwrap();
    }

    #[test]
    fn layout_reordering_keeps_the_same_page_in_view() {
        let mut l = Layout::single(Page::Play);
        l.open(Page::Mods);
        l.open(Page::Saves);
        l.open(Page::Play);
        assert!(l.move_tab(Page::Saves, Side::Left, 0));
        assert_eq!(tabs(&l, Side::Left), [Page::Saves, Page::Play, Page::Mods]);
        assert_eq!(l.focused_page(), Page::Play);
        assert!(!l.move_tab(Page::Saves, Side::Left, 0));
        assert!(l.move_tab(Page::Saves, Side::Left, 99));
        assert_eq!(tabs(&l, Side::Left), [Page::Play, Page::Mods, Page::Saves]);
    }

    #[test]
    fn layout_windows_come_back_as_tabs() {
        let mut l = Layout::single(Page::Play);
        l.open(Page::Mods);
        assert!(l.pop(Page::Mods));
        assert_eq!(l.find(Page::Mods), Some(Place::Window));
        assert_eq!(tabs(&l, Side::Left), [Page::Play]);
        // Opening a windowed page changes nothing in the main window.
        assert_eq!(l.open(Page::Mods), Place::Window);
        assert_eq!(l.visible(), [Page::Play, Page::Mods]);
        // Closing the window docks it.
        assert!(l.close(Page::Mods));
        assert_eq!(tabs(&l, Side::Left), [Page::Play, Page::Mods]);
        assert_eq!(l.focused_page(), Page::Mods);
        assert!(l.windows().is_empty());
        l.check().unwrap();
    }

    #[test]
    fn layout_popping_the_only_tab_leaves_a_page_behind() {
        let mut l = Layout::single(Page::Mods);
        assert!(l.pop(Page::Mods));
        assert_eq!(tabs(&l, Side::Left), [Page::Play]);
        assert_eq!(l.windows(), [Page::Mods]);
        // Pop everything else too: the main window keeps one page.
        for page in Page::ALL {
            l.pop(page);
            l.check().unwrap();
        }
        assert_eq!(l.main_tabs(), 1);
        assert_eq!(l.windows().len(), Page::ALL.len() - 1);
        let last = l.focused_page();
        assert!(!l.pop(last));
    }

    #[test]
    fn layout_every_page_is_in_exactly_one_place() {
        // A long pseudo-random walk over every operation.
        let mut seed = 0x2545_F491_4F6C_DD1D_u64;
        let mut next = |n: u64| {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            (seed % n) as usize
        };
        let mut l = Layout::default();
        for step in 0..20_000 {
            let page = Page::ALL[next(8)];
            let side = if next(2) == 0 { Side::Left } else { Side::Right };
            let was_open = l.is_open(page);
            let before = l.clone();
            let op = next(13);
            match op {
                0 => {
                    l.open(page);
                    assert!(l.is_open(page));
                }
                1 => {
                    l.open_background(page);
                    assert!(l.is_open(page));
                }
                2 => {
                    let only = l.main_tabs() == 1 && matches!(l.find(page), Some(Place::Tab(..)));
                    assert_eq!(l.close(page), was_open && !only, "step {step}");
                }
                3 => {
                    l.move_tab(page, side, next(9));
                    assert!(matches!(l.find(page), Some(Place::Tab(..))));
                }
                4 => {
                    l.beside(page, side);
                }
                5 => {
                    l.to_other_side(page);
                }
                6 => {
                    l.toggle_split();
                }
                7 => {
                    if l.pop(page) {
                        assert_eq!(l.find(page), Some(Place::Window));
                    }
                }
                8 => {
                    l.dock(page);
                }
                9 => l.cycle(next(2) == 0),
                10 => l.activate(side, next(9)),
                11 => {
                    l.close_others(page);
                }
                _ => {
                    l.set_focus(side);
                    l.set_ratio(next(100) as f32 / 50.0 - 0.5);
                }
            }
            if let Err(e) = l.check() {
                panic!("step {step}: {e}\nbefore: {before:?}\nafter: {l:?}");
            }
            // Only closing makes an open page disappear: "close" its own page, "close
            // others" the rest of that pane. Moving, splitting and windows never lose one.
            for p in Page::ALL {
                if before.is_open(p) && !l.is_open(p) {
                    assert!((op == 2 && p == page) || (op == 11 && p != page), "step {step}: {p:?} was lost by op {op}");
                }
            }
            assert!(l.main_tabs() >= 1);
            assert!((MIN_RATIO..=MAX_RATIO).contains(&l.ratio()));
        }
    }

    #[test]
    fn layout_survives_a_round_trip_through_the_config() {
        let mut l = Layout::single(Page::Play);
        l.open(Page::Mods);
        l.open(Page::Saves);
        l.open(Page::Units);
        l.beside(Page::Saves, Side::Right);
        l.open(Page::Console);
        l.pop(Page::Units);
        l.set_ratio(0.62);
        l.set_window_size(Page::Units, [1000.4, 700.0]);
        l.set_focus(Side::Left);
        l.check().unwrap();
        let json = serde_json::to_string(&l).unwrap();
        let back: Layout = serde_json::from_str(&json).unwrap();
        assert_eq!(back, l);
        assert_eq!(back.window_size(Page::Units), Some([1000.0, 700.0]));
        assert_eq!(back.focused_page(), Page::Mods);

        // The whole config too, and a config written before layouts existed.
        let mut cfg = crate::config::Config::default();
        cfg.layout = l.clone();
        let text = serde_json::to_string_pretty(&cfg).unwrap();
        let cfg2: crate::config::Config = serde_json::from_str(&text).unwrap();
        assert_eq!(cfg2.layout, l);
        let old: crate::config::Config = serde_json::from_str("{}").unwrap();
        assert_eq!(old.layout, Layout::single(Page::Play));
    }

    #[test]
    fn layout_repairs_a_damaged_config() {
        // Unknown pages, duplicates, an active index past the end, an empty main window.
        let json = r#"{
            "left": ["nonsense", "mods", "mods", "saves"], "left_active": 3,
            "right": ["saves", "mods"], "right_active": 7,
            "ratio": 9.5, "focus_right": true,
            "windows": ["units", "units", "play", "mods"],
            "window_sizes": { "units": [900, 700], "play": [1, 1], "bogus": [900, 700] }
        }"#;
        let l: Layout = serde_json::from_str(json).unwrap();
        l.check().unwrap();
        assert_eq!(tabs(&l, Side::Left), [Page::Mods, Page::Saves]);
        // "saves" was the active entry even though a bad name sat before it.
        assert_eq!(l.pane(Side::Left).unwrap().active_page(), Page::Saves);
        assert!(!l.is_split());
        assert_eq!(l.focus(), Side::Left);
        assert_eq!(l.windows(), [Page::Units, Page::Play]);
        assert_eq!(l.ratio(), MAX_RATIO);
        assert_eq!(l.window_size(Page::Units), Some([900.0, 700.0]));
        assert_eq!(l.window_size(Page::Play), None);

        let empty: Layout = serde_json::from_str(r#"{ "windows": ["play", "mods"] }"#).unwrap();
        empty.check().unwrap();
        assert_eq!(tabs(&empty, Side::Left), [Page::Profiles]);
        assert_eq!(empty.windows(), [Page::Play, Page::Mods]);

        let nothing: Layout = serde_json::from_str("{}").unwrap();
        assert_eq!(nothing, Layout::single(Page::Play));
    }
}

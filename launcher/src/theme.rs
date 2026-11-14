//! Look and feel: palette, fonts, and the small set of shared widgets every page uses.

use egui::{
    vec2, Align, Align2, Button, Color32, CornerRadius, FontData, FontDefinitions, FontFamily,
    FontId, Frame, Layout, Margin, Response, RichText, Sense, Stroke, StrokeKind, TextStyle, Ui,
    Vec2, WidgetText,
};
use std::sync::Arc;

pub const BG: Color32 = Color32::from_rgb(10, 12, 17);
pub const SIDEBAR: Color32 = Color32::from_rgb(13, 16, 22);
pub const CARD: Color32 = Color32::from_rgb(20, 24, 32);
pub const CARD_HI: Color32 = Color32::from_rgb(27, 32, 43);
pub const FIELD: Color32 = Color32::from_rgb(14, 17, 24);
pub const BORDER: Color32 = Color32::from_rgb(37, 44, 58);
pub const BORDER_HI: Color32 = Color32::from_rgb(58, 68, 88);
pub const TEXT: Color32 = Color32::from_rgb(231, 235, 242);
pub const MUTED: Color32 = Color32::from_rgb(142, 152, 170);
pub const FAINT: Color32 = Color32::from_rgb(94, 104, 122);
pub const ACCENT: Color32 = Color32::from_rgb(82, 142, 255);
pub const ACCENT_HI: Color32 = Color32::from_rgb(122, 170, 255);
pub const ACCENT_DIM: Color32 = Color32::from_rgb(28, 44, 78);
pub const GOOD: Color32 = Color32::from_rgb(62, 201, 142);
pub const WARN: Color32 = Color32::from_rgb(245, 184, 61);
pub const BAD: Color32 = Color32::from_rgb(240, 98, 98);

/// Glyphs from Segoe Fluent Icons / Segoe MDL2 Assets (both ship with Windows).
/// A palette to pick from: not every glyph is used at any one time.
#[allow(dead_code)]
pub mod icon {
    pub const PLAY: &str = "\u{E768}";
    pub const STOP: &str = "\u{E71A}";
    pub const SETTINGS: &str = "\u{E713}";
    pub const SAVE: &str = "\u{E74E}";
    pub const FOLDER: &str = "\u{E8B7}";
    pub const FOLDER_OPEN: &str = "\u{E838}";
    pub const SEARCH: &str = "\u{E721}";
    pub const DELETE: &str = "\u{E74D}";
    pub const ADD: &str = "\u{E710}";
    pub const EDIT: &str = "\u{E70F}";
    pub const REFRESH: &str = "\u{E72C}";
    pub const PEOPLE: &str = "\u{E716}";
    pub const LIBRARY: &str = "\u{E8F1}";
    pub const GLOBE: &str = "\u{E774}";
    pub const DISPLAY: &str = "\u{E7F4}";
    pub const CONSOLE: &str = "\u{E756}";
    pub const COPY: &str = "\u{E8C8}";
    pub const UP: &str = "\u{E74A}";
    pub const DOWN: &str = "\u{E74B}";
    pub const HISTORY: &str = "\u{E81C}";
    pub const CHECK: &str = "\u{E73E}";
    pub const CLOSE: &str = "\u{E711}";
    pub const WARNING: &str = "\u{E7BA}";
    pub const PUZZLE: &str = "\u{EA86}";
    pub const RENAME: &str = "\u{E8AC}";
    pub const FLAG: &str = "\u{E7C1}";
    pub const UNDO: &str = "\u{E7A7}";
    pub const SHIELD: &str = "\u{EA18}";
    pub const CLOCK: &str = "\u{E823}";
    pub const GRIP: &str = "\u{E76F}";
    pub const DATABASE: &str = "\u{E8FD}";
    pub const HOME: &str = "\u{E80F}";
    pub const STAR: &str = "\u{E734}";
    pub const STAR_FILLED: &str = "\u{E735}";
    pub const COMPARE: &str = "\u{E8AB}";
    pub const BACK: &str = "\u{E72B}";
    pub const EYE: &str = "\u{E890}";
    pub const INFO: &str = "\u{E946}";
    pub const REMOVE: &str = "\u{E738}";
    pub const POP_OUT: &str = "\u{E8A7}";
}

pub fn semibold() -> FontFamily {
    FontFamily::Name("semibold".into())
}

fn read_font(names: &[&str]) -> Option<Vec<u8>> {
    let windir = std::env::var("WINDIR").unwrap_or_else(|_| "C:\\Windows".into());
    names
        .iter()
        .find_map(|n| std::fs::read(format!("{windir}\\Fonts\\{n}")).ok())
}

pub fn install(ctx: &egui::Context) {
    // ---- fonts: Segoe UI for text, Segoe icon font as fallback for glyphs.
    let mut fonts = FontDefinitions::default();
    let mut prop = Vec::new();
    let mut semi = Vec::new();
    let mut mono = Vec::new();
    if let Some(b) = read_font(&["segoeui.ttf"]) {
        fonts.font_data.insert("ui".into(), Arc::new(FontData::from_owned(b)));
        prop.push("ui".to_owned());
    }
    if let Some(b) = read_font(&["seguisb.ttf", "segoeuib.ttf"]) {
        fonts.font_data.insert("ui-semi".into(), Arc::new(FontData::from_owned(b)));
        semi.push("ui-semi".to_owned());
    }
    if let Some(b) = read_font(&["CascadiaMono.ttf", "consola.ttf"]) {
        fonts.font_data.insert("ui-mono".into(), Arc::new(FontData::from_owned(b)));
        mono.push("ui-mono".to_owned());
    }
    let has_icons = if let Some(b) = read_font(&["SegoeIcons.ttf", "segmdl2.ttf"]) {
        fonts.font_data.insert("icons".into(), Arc::new(FontData::from_owned(b)));
        true
    } else {
        false
    };
    let defaults = fonts.families.get(&FontFamily::Proportional).cloned().unwrap_or_default();
    let mono_defaults = fonts.families.get(&FontFamily::Monospace).cloned().unwrap_or_default();
    for list in [&mut prop, &mut semi, &mut mono] {
        if has_icons {
            list.push("icons".to_owned());
        }
    }
    semi.extend(prop.iter().filter(|n| *n != "icons").cloned());
    prop.extend(defaults.clone());
    semi.extend(defaults);
    mono.extend(mono_defaults);
    fonts.families.insert(FontFamily::Proportional, prop);
    fonts.families.insert(FontFamily::Monospace, mono);
    fonts.families.insert(semibold(), semi);
    ctx.set_fonts(fonts);

    // ---- style
    let mut style = (*ctx.style()).clone();
    style.text_styles = [
        (TextStyle::Heading, FontId::new(24.0, semibold())),
        (TextStyle::Body, FontId::new(14.0, FontFamily::Proportional)),
        (TextStyle::Button, FontId::new(14.0, FontFamily::Proportional)),
        (TextStyle::Small, FontId::new(12.0, FontFamily::Proportional)),
        (TextStyle::Monospace, FontId::new(13.0, FontFamily::Monospace)),
    ]
    .into();
    style.spacing.item_spacing = vec2(8.0, 8.0);
    style.spacing.button_padding = vec2(12.0, 6.0);
    style.spacing.interact_size = vec2(40.0, 30.0);
    style.spacing.scroll.bar_width = 8.0;
    style.spacing.scroll.floating = true;
    style.spacing.menu_margin = Margin::same(8);
    style.spacing.combo_height = 320.0;
    style.spacing.slider_width = 180.0;

    let v = &mut style.visuals;
    v.dark_mode = true;
    v.override_text_color = Some(TEXT);
    v.panel_fill = BG;
    v.window_fill = CARD;
    v.window_stroke = Stroke::new(1.0_f32, BORDER_HI);
    v.window_corner_radius = CornerRadius::same(12);
    v.menu_corner_radius = CornerRadius::same(10);
    v.extreme_bg_color = FIELD;
    v.faint_bg_color = CARD_HI;
    v.hyperlink_color = ACCENT_HI;
    v.selection.bg_fill = Color32::from_rgb(40, 70, 130);
    v.selection.stroke = Stroke::new(1.0_f32, ACCENT_HI);
    v.popup_shadow = egui::Shadow { offset: [0, 8], blur: 24, spread: 0, color: Color32::from_black_alpha(140) };
    v.window_shadow = egui::Shadow { offset: [0, 12], blur: 40, spread: 0, color: Color32::from_black_alpha(160) };
    v.widgets.noninteractive.bg_fill = CARD;
    v.widgets.noninteractive.bg_stroke = Stroke::new(1.0_f32, BORDER);
    v.widgets.noninteractive.fg_stroke = Stroke::new(1.0_f32, TEXT);
    v.widgets.noninteractive.corner_radius = CornerRadius::same(8);
    for (w, fill, stroke, fg) in [
        (&mut v.widgets.inactive, CARD_HI, BORDER, TEXT),
        (&mut v.widgets.hovered, Color32::from_rgb(36, 43, 58), BORDER_HI, Color32::WHITE),
        (&mut v.widgets.active, Color32::from_rgb(44, 53, 72), ACCENT, Color32::WHITE),
        (&mut v.widgets.open, Color32::from_rgb(36, 43, 58), ACCENT, Color32::WHITE),
    ] {
        w.bg_fill = fill;
        w.weak_bg_fill = fill;
        w.bg_stroke = Stroke::new(1.0_f32, stroke);
        w.fg_stroke = Stroke::new(1.0_f32, fg);
        w.corner_radius = CornerRadius::same(8);
        w.expansion = 0.0;
    }
    ctx.set_style(style);
}

// ------------------------------------------------------------------ text

pub fn text(s: impl Into<String>) -> RichText {
    RichText::new(s).color(TEXT)
}
pub fn muted(s: impl Into<String>) -> RichText {
    RichText::new(s).color(MUTED)
}
pub fn faint(s: impl Into<String>) -> RichText {
    RichText::new(s).color(FAINT).size(12.0)
}
pub fn strong(s: impl Into<String>) -> RichText {
    RichText::new(s).color(TEXT).family(semibold())
}
pub fn title(s: impl Into<String>) -> RichText {
    RichText::new(s).color(TEXT).family(semibold()).size(16.0)
}
pub fn mono(s: impl Into<String>) -> RichText {
    RichText::new(s).monospace().color(TEXT)
}
/// Small all-caps label used above groups of controls.
pub fn overline(ui: &mut Ui, s: &str) {
    ui.label(RichText::new(s.to_uppercase()).color(FAINT).size(11.0).family(semibold()));
}

pub fn page_header(ui: &mut Ui, title: &str, subtitle: &str) {
    ui.label(RichText::new(title).size(26.0).family(semibold()).color(TEXT));
    if !subtitle.is_empty() {
        ui.add_space(-4.0);
        ui.label(muted(subtitle));
    }
    ui.add_space(10.0);
}

// ------------------------------------------------------------------ containers

pub fn card_frame() -> Frame {
    Frame::new()
        .fill(CARD)
        .stroke(Stroke::new(1.0_f32, BORDER))
        .corner_radius(CornerRadius::same(12))
        .inner_margin(Margin::same(16))
}

pub fn card<R>(ui: &mut Ui, add: impl FnOnce(&mut Ui) -> R) -> R {
    card_frame()
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            add(ui)
        })
        .inner
}

pub fn inset_frame() -> Frame {
    Frame::new()
        .fill(FIELD)
        .stroke(Stroke::new(1.0_f32, BORDER))
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::same(10))
}

// ------------------------------------------------------------------ buttons

pub fn primary_button(ui: &mut Ui, label: &str) -> Response {
    ui.add(
        Button::new(RichText::new(label).color(Color32::WHITE).family(semibold()))
            .fill(ACCENT)
            .stroke(Stroke::NONE)
            .corner_radius(CornerRadius::same(8)),
    )
}

pub fn button(ui: &mut Ui, label: &str) -> Response {
    ui.add(Button::new(RichText::new(label).color(TEXT)).corner_radius(CornerRadius::same(8)))
}

pub fn danger_button(ui: &mut Ui, label: &str) -> Response {
    ui.add(
        Button::new(RichText::new(label).color(BAD))
            .fill(Color32::from_rgb(52, 24, 28))
            .stroke(Stroke::new(1.0_f32, Color32::from_rgb(110, 44, 50)))
            .corner_radius(CornerRadius::same(8)),
    )
}

/// Square, borderless icon button with a tooltip.
pub fn icon_button(ui: &mut Ui, glyph: &str, tip: &str) -> Response {
    let (rect, resp) = ui.allocate_exact_size(Vec2::splat(28.0), Sense::click());
    if ui.is_rect_visible(rect) {
        let hovered = resp.hovered() && ui.is_enabled();
        if hovered {
            ui.painter().rect_filled(rect, CornerRadius::same(6), Color32::from_rgb(40, 48, 64));
        }
        let color = if !ui.is_enabled() {
            FAINT.gamma_multiply(0.5)
        } else if hovered {
            Color32::WHITE
        } else {
            MUTED
        };
        ui.painter().text(
            rect.center(),
            Align2::CENTER_CENTER,
            glyph,
            FontId::new(14.0, FontFamily::Proportional),
            color,
        );
    }
    resp.on_hover_text(tip)
}

pub fn chip(ui: &mut Ui, label: impl Into<String>, color: Color32) -> Response {
    let label = label.into();
    let galley = ui.painter().layout_no_wrap(label, FontId::new(11.5, semibold()), color);
    let size = galley.size() + vec2(16.0, 6.0);
    let (rect, resp) = ui.allocate_exact_size(size, Sense::hover());
    if ui.is_rect_visible(rect) {
        ui.painter().rect_filled(rect, CornerRadius::same(255), color.gamma_multiply(0.14));
        ui.painter().galley(rect.center() - galley.size() / 2.0, galley, color);
    }
    resp
}

/// iOS-style switch.
pub fn toggle(ui: &mut Ui, on: &mut bool) -> Response {
    let size = vec2(38.0, 20.0);
    let (rect, mut resp) = ui.allocate_exact_size(size, Sense::click());
    if resp.clicked() {
        *on = !*on;
        resp.mark_changed();
    }
    if ui.is_rect_visible(rect) {
        let t = ui.ctx().animate_bool_with_time(resp.id, *on, 0.12);
        let bg = lerp_color(Color32::from_rgb(48, 56, 72), ACCENT, t);
        ui.painter().rect_filled(rect, CornerRadius::same(255), bg);
        let x = egui::lerp((rect.left() + 10.0)..=(rect.right() - 10.0), t);
        ui.painter().circle_filled(egui::pos2(x, rect.center().y), 7.0, Color32::WHITE);
    }
    resp
}

/// A labelled switch row: title + description on the left, switch on the right.
pub fn toggle_row(ui: &mut Ui, on: &mut bool, title: &str, desc: &str) -> bool {
    let mut changed = false;
    ui.horizontal(|ui| {
        ui.vertical(|ui| {
            ui.spacing_mut().item_spacing.y = 1.0;
            ui.label(text(title));
            if !desc.is_empty() {
                ui.label(faint(desc));
            }
        });
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            changed = toggle(ui, on).changed();
        });
    });
    changed
}

pub fn lerp_color(a: Color32, b: Color32, t: f32) -> Color32 {
    let l = |x: u8, y: u8| (x as f32 + (y as f32 - x as f32) * t).round() as u8;
    Color32::from_rgb(l(a.r(), b.r()), l(a.g(), b.g()), l(a.b(), b.b()))
}

pub fn empty_state(ui: &mut Ui, glyph: &str, heading: &str, body: &str) {
    ui.vertical_centered(|ui| {
        ui.add_space(36.0);
        ui.label(RichText::new(glyph).size(34.0).color(FAINT));
        ui.add_space(4.0);
        ui.label(title(heading));
        ui.label(muted(body));
        ui.add_space(36.0);
    });
}

pub fn separator(ui: &mut Ui) {
    let (rect, _) = ui.allocate_exact_size(vec2(ui.available_width(), 1.0), Sense::hover());
    ui.painter().rect_filled(rect, 0.0, BORDER);
}

/// Selectable row background used by lists. Returns the response for the whole row.
pub fn list_row<R>(
    ui: &mut Ui,
    selected: bool,
    add: impl FnOnce(&mut Ui) -> R,
) -> (Response, R) {
    let fill = if selected { ACCENT_DIM } else { Color32::TRANSPARENT };
    let stroke = if selected { Stroke::new(1.0_f32, ACCENT.gamma_multiply(0.6)) } else { Stroke::NONE };
    // The row's click area is registered *before* its contents (using last frame's
    // rect) so buttons inside the row sit on top of it and still get their clicks.
    let id = ui.next_auto_id().with("list_row");
    let last_rect: Option<egui::Rect> = ui.ctx().data(|d| d.get_temp(id));
    let early = last_rect.map(|r| ui.interact(r, id, Sense::click()));
    let out = Frame::new()
        .fill(fill)
        .stroke(stroke)
        .corner_radius(CornerRadius::same(8))
        .inner_margin(Margin::symmetric(10, 7))
        .show(ui, |ui| {
            ui.set_width(ui.available_width());
            add(ui)
        });
    ui.ctx().data_mut(|d| d.insert_temp(id, out.response.rect));
    let resp = early.unwrap_or_else(|| ui.interact(out.response.rect, id, Sense::click()));
    if resp.hovered() && !selected {
        ui.painter().rect_stroke(
            out.response.rect,
            CornerRadius::same(8),
            Stroke::new(1.0_f32, BORDER_HI),
            StrokeKind::Inside,
        );
    }
    (resp, out.inner)
}

pub fn kv(ui: &mut Ui, key: &str, value: impl Into<WidgetText>) {
    ui.horizontal(|ui| {
        ui.label(muted(key));
        ui.with_layout(Layout::right_to_left(Align::Center), |ui| {
            ui.label(value);
        });
    });
}

pub fn fmt_duration(secs: u64) -> String {
    let h = secs / 3600;
    let m = (secs % 3600) / 60;
    if h > 0 {
        format!("{h} h {m:02} min")
    } else if m > 0 {
        format!("{m} min")
    } else if secs > 0 {
        "under a minute".into()
    } else {
        "not played yet".into()
    }
}

pub fn fmt_bytes(n: u64) -> String {
    if n >= 1 << 30 {
        format!("{:.1} GB", n as f64 / (1u64 << 30) as f64)
    } else if n >= 1 << 20 {
        format!("{:.1} MB", n as f64 / (1u64 << 20) as f64)
    } else if n >= 1 << 10 {
        format!("{:.0} KB", n as f64 / 1024.0)
    } else {
        format!("{n} B")
    }
}

pub fn fmt_time(t: std::time::SystemTime) -> String {
    let dt: chrono::DateTime<chrono::Local> = t.into();
    dt.format("%d %b %Y, %H:%M").to_string()
}

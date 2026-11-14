// P4 GUI dialog manager (docs/re/UI.md §3, §4.1-4.2): runtime dialogs built from GUIDialogs.rc (gui_resource.hpp),
// skinned from GUIDialogs.xml, drawn as 2D scene #1 (HudRenderer::record's `dialogs` list, frame step Dialogs).
//
// Behaviour taken from UI.md:
//   * input order: dialogs come after the overlay / targeting controller and before chat, HUD and world (InputRouter
//     already has the slot: install() fills RouteHooks::dialog_hit / dialog_focus); a press is offered to every
//     "always receives input" dialog and to the topmost one, consumed if any accepts (§4.1 0x1c3660);
//   * WM_CHAR goes only to dialogs; key downs reach dialogs while one has focus; the wheel goes to dialogs first only
//     while one is active (§4.1-4.2);
//   * captions are MasterTextFile keys; other captions are runtime placeholders (§3.2) - a caption with no text entry
//     is shown as written [port];
//   * UI sounds: GUI_Common_Dialog_Button_Hilight (button hover), GUI_Common_Dialog_Button_Press (click),
//     GUI_Common_Dialog_Listbox_Entry_Select (list selection) through a HudSfxHook (§3.6).
//
// [port] choices (UI.md §3.3 / §6 leave them open):
//   * dialog unit -> canvas: 1 dialog unit = 1 HUD canvas unit (the 1024 x 768 Hor+ canvas, hud_canvas.hpp), so the
//     on-screen pixel size is units / units-per-pixel - at 1024 x 768 one unit is one pixel; widescreen widens the
//     canvas and keeps 768 units of height;
//   * placement: a dialog taller than the canvas (the 512 x 1024 menu columns) is anchored to the top edge and the screen's
//     right edge (base 20.jpg); its IDC_MENU_PETRO_LOGO hotspot follows the stretched MENUBACK_OVERLAY backdrop
//     (backdrop_hotspots); every other dialog is centred (DS_CENTER or not); set_position() overrides;
//   * modality: a dialog opened modal (default) takes every press / wheel / key while it is the topmost modal dialog,
//     even outside its rectangle; non-modal dialogs take only presses inside them;
//   * keyboard: Tab / Shift+Tab cycle the WS_TABSTOP controls (texts, images, frames, progress bars never), Enter
//     clicks the focused button (else the DEFPUSHBUTTON; in an edit box it is Submitted), Space activates the focused
//     button / check box / radio, Up / Down move list / combo selections and radio groups, Left / Right / PageUp /
//     PageDown / Home / End move sliders (line 1, page = range / 5), Esc closes an open combo, else posts Cancel;
//   * a check / radio / button click fires on release over the pressed control; list double click = 0.5 s;
//   * sliders 0..100 and progress bars 0..100 by default (Win32 defaults), list rows = font line height;
//   * drawing: frame background stretched over the whole dialog, 16-piece frame with the transition pieces next to
//     the corners and the edges stretched between; buttons = left cap / stretched middle / right cap, caps scaled to
//     the button height; check / radio icons at their atlas size (clamped to the control height) left of the caption;
//     list / edit / combo inside the 9-piece small frame; slider = Dial_Left / Middle / Right track + Dial_Tab thumb;
//     progress = Progress_Bar_Left / Middle_On (filled part) / Middle_Off / Right; Scanlines not drawn; textures
//     missing from the atlas and from the image resolver draw [port] flat stand-ins only for the frame background,
//     buttons, tracks and bars (GuiStyle::fallback_art).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/gui_resource.hpp"
#include "alamo/ui/hud_buttons.hpp"
#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_draw.hpp"
#include "alamo/ui/hud_input.hpp"
#include "alamo/ui/input.hpp"

namespace alamo::ui {

class FontCache;

// GuiImage / GuiImageResolver: hud_draw.hpp.

// Assigns image pages to texture names (kHudImagePages slots). The host uploads assigned names (entries with
// width 0 and not failed) with HudRenderer::set_image_page and marks them loaded / failed. When every slot is in use
// a new name takes the least recently requested slot (failed slots first) and the host re-uploads it; a name the
// host could not load is remembered and never takes a slot again [port] (animated cursors alone use more than 16
// textures).
class GuiImagePages {
public:
    // Page for a texture (assigned on first request, reusing the least recently used slot when full); -1 for a name
    // the host failed to load.
    int32_t page(std::string_view texture);
    struct Entry {
        std::string name;  // as first requested
        uint16_t page = 0;
        uint32_t width = 0, height = 0;  // texture size once uploaded (0 = not yet)
        bool failed = false;             // the host could not load it
        uint64_t last_use = 0;           // request clock (LRU)
    };
    std::span<Entry> entries() { return entries_; }
    std::span<const Entry> entries() const { return entries_; }
    void set_loaded(uint16_t page, uint32_t width, uint32_t height);
    void set_failed(uint16_t page);
    void clear() {
        entries_.clear();
        failed_.clear();
    }
    // Resolver over these pages (valid once loaded).
    GuiImage resolve(std::string_view texture);

private:
    std::vector<Entry> entries_;
    std::vector<std::string> failed_;  // atlas keys the host could not load
    uint64_t clock_ = 0;
};

struct GuiStyle {
    bool fallback_art = true;                 // flat stand-ins for missing frame / button / track textures
    // [port] scroll bar arrow / track auto-repeat while the button is held: first repeat after scroll_repeat_delay,
    // then every scroll_repeat_rate seconds (the Win32 scroll bar timer pattern).
    double scroll_repeat_delay = 0.4, scroll_repeat_rate = 0.05;
    Rgba8 edit_selection{80, 120, 200, 160};  // [port] edit box selected-text highlight
    Rgba8 fallback_background{18, 22, 30, 235};
    Rgba8 fallback_button{60, 66, 80, 255}, fallback_button_hot{90, 100, 125, 255}, fallback_button_down{40, 44, 55, 255};
    Rgba8 selection{255, 255, 255, 60};       // list selected-row highlight
    Rgba8 focus{255, 255, 255, 40};           // keyboard focus highlight
    Rgba8 disabled_text{128, 128, 128, 200};
    float tooltip_delay = 0.330f;             // GameConstants Tooltip_Delay
    double double_click = 0.5;
    float edit_caret_blink = 0.5f;
};

// A text label drawn over a custom image (the skirmish start numbers, UI.md 9.6.1): `pos` is a 0..1 fraction of the
// drawing area (x right, y down), the label is centred on that point in the control's font in `color`. The drawing area
// is the control rectangle inset by 5 % of its size on each side.
struct GuiMarker {
    Vec2f pos{};
    std::u32string label;
    Rgba8 color{255, 255, 255, 255};
};

struct GuiControl {
    const RcControl* def = nullptr;
    GuiGadgetType type = GuiGadgetType::Unknown;
    bool visible = true, enabled = true;
    bool checked = false;              // check box / radio
    std::u32string text;               // caption (resolved) / edit text
    int32_t value = 0, range_min = 0, range_max = 100;  // slider / progress
    std::vector<std::u32string> items; // list / combo
    // Per-item colours (list / combo): item k with a colour has its TEXT drawn flat in that colour (both gradient ends,
    // control font; kept when the control is disabled) - UI.md §3.7.1. The skirmish staging colour combos fill each item
    // with one glyph (U+25A0), which the per-control font (IDC_PLAYER_COLOR_n: Arial Unicode MS 35, Stretch_Factor
    // 0.25) turns into a flat wide bar: the colour "swatch".
    std::vector<Rgba8> item_colors;
    // [port] per-item text colours (list): item k is drawn in item_text_colors[k] (flat, no font gradient) when that
    // colour's alpha is > 0, else in the skin's font colours. Story dialog scripts' TEXTCOLOR lines use it.
    std::vector<Rgba8> item_text_colors;
    // [port: Gabriel] per-item icon and indent (list): item k draws item_icons[k] (a texture name; "" or a missing texture =
    // no icon, the text keeps its place) in a square one row high, item_indent[k] row-heights in from the left edge. The
    // saved games tree (FrontEnd) uses them.
    std::vector<std::string> item_icons;
    std::vector<uint8_t> item_indent;
    int32_t selected = -1;             // list / combo selection
    int32_t top = 0;                   // first visible list row
    bool open = false;                 // combo drop-down shown
    bool left_justified = false;       // combo: closed-field text starts at the left edge + inset instead of centred (UI.md §3.7.1)
    size_t caret = 0;                  // edit caret (code points)
    size_t sel_anchor = 0;             // edit: other end of the selection (== caret: nothing selected)
    size_t scroll_x = 0;               // edit: first code point drawn (ES_AUTOHSCROLL keeps the caret in view)
    float field_height = 15.0f;        // combo: closed field height (canvas units; the skin's pop-down button height)
    size_t max_chars = 256;            // edit limit [port]
    bool read_only = false;            // edit: keys are not typed into it (key-capture fields)
    bool key_capture = false;          // edit: a key-binding field - Tab is a key to bind there, not a focus move (UI5 t05)
    bool text_outline = false;         // [port] force an outline under the caption (readability over bright art)
    bool ui_font = false;              // [port] QA D8: text in the dialog's L_Text font instead of the gadget's skin font
    // [port] QA U2: a static with fit_lines > 0 is never narrowed: word-wrapped to the control width in at most fit_lines
    // lines (the last one cut with "..."), the block centred on the rectangle's height.
    int32_t fit_lines = 0;
    size_t fit_keep_tail = 0;          // [port] t47: code points at the end of the text the "..." may not hide (fit_text_lines)
    std::string image;                 // custom image texture ("" = none)
    std::vector<GuiMarker> markers;    // custom image: labels over the image (skirmish start numbers, UI.md 9.6.1)
    Rgba8 tint{255, 255, 255, 255};
    int32_t radio_group = -1;          // radio buttons: group index (WS_GROUP rule)
    Vec2f offset{};                    // [port] canvas-unit shift of the control rectangle (backdrop hotspots, +Y up)
    Vec2f scale{1.0f, 1.0f};           // [port] factor on the rectangle's width / height (backdrop hotspots follow the stretch)
    std::string_view id() const { return def ? std::string_view(def->id) : std::string_view(); }
};

// Canvas-space rectangle (+Y up).
struct GuiRect {
    Vec2f min{}, max{};
    bool contains(Vec2f p) const { return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y; }
    float width() const { return max.x - min.x; }
    float height() const { return max.y - min.y; }
};

struct GuiOpenOptions {
    bool modal = true;
    bool always_input = false;  // offered every press even when not topmost (§4.1 flag +0x108)
    bool opaque_backdrop = false;  // [port] black over the whole canvas under this dialog (load screens: no scene around it)
};

class GuiDialog {
public:
    GuiDialog(const RcDialog& def, GuiOpenOptions opt) : def_(&def), opt_(opt) {}
    const RcDialog& def() const { return *def_; }
    std::string_view name() const { return def_->name; }
    const GuiOpenOptions& options() const { return opt_; }

    size_t size() const { return controls_.size(); }
    GuiControl& operator[](size_t i) { return controls_[i]; }
    const GuiControl& operator[](size_t i) const { return controls_[i]; }
    int32_t find(std::string_view id) const { return def_->find(id); }
    GuiControl* get(std::string_view id);
    const GuiControl* get(std::string_view id) const;

    // Convenience setters by control id (false when the dialog has no such control).
    bool set_text(std::string_view id, std::u32string text);
    bool show(std::string_view id, bool visible);
    bool enable(std::string_view id, bool enabled);
    bool set_checked(std::string_view id, bool checked);  // radios uncheck the rest of their group
    bool set_value(std::string_view id, int32_t value);
    bool set_range(std::string_view id, int32_t lo, int32_t hi);
    bool set_items(std::string_view id, std::vector<std::u32string> items, int32_t selected = -1);
    bool set_selected(std::string_view id, int32_t index);
    bool set_image(std::string_view id, std::string texture);
    bool focus(std::string_view id);
    int32_t focus_index() const { return focus_; }

    // Placement (canvas units): top-left corner of the dialog.
    Vec2f origin() const { return origin_; }
    void set_position(Vec2f top_left) {
        origin_ = top_left;
        placed_ = true;
    }
    GuiRect rect() const;
    GuiRect control_rect(size_t i) const;
    // Dialog height in canvas units: the .rc height less GuiManager::trim_dialog_height.
    float height() const { return static_cast<float>(def_->h) - height_trim_; }
    // Topmost visible control at a canvas point, -1 if none.
    int32_t control_at(Vec2f p) const;

private:
    friend class GuiManager;
    const RcDialog* def_;
    GuiOpenOptions opt_;
    std::vector<GuiControl> controls_;
    Vec2f origin_{};
    float height_trim_ = 0.0f;
    bool placed_ = false;
    int32_t focus_ = -1;
    bool keyboard_focus_ = false;
};

struct GuiEvent {
    enum class Kind : uint8_t {
        Clicked,           // push button (or pushlike radio) clicked
        Checked,           // check box toggled / radio selected (value = checked)
        SelectionChanged,  // list / combo selection (value = index)
        DoubleClicked,     // list row double click (value = index)
        ValueChanged,      // slider (value)
        TextChanged,       // edit box text
        Submitted,         // Enter in an edit box
        Cancel,            // Esc on the topmost dialog
        Opened,
        Closed,
        RowClicked,        // [port] list row pressed with the mouse (value = row; after its SelectionChanged; the second press of a
                           // double click sends DoubleClicked instead)
    };
    Kind kind = Kind::Clicked;
    std::string dialog;
    std::string control;   // IDC_* ("" for dialog events)
    int32_t control_index = -1;
    int32_t value = 0;
    std::u32string text;
};
// "\n" / "\r\n" escape sequences of MasterTextFile strings -> line breaks, "\t" -> space [port].
std::u32string gui_unescape(std::u32string s);
// QA U1: "" for a text that is an asset name or path (".ted" and other file extensions, path separators); players never
// see those. Any other text comes back unchanged.
std::u32string gui_no_asset_name(std::u32string s);
// QA U2: `text` wrapped to max_width_px in at most max_lines lines; when it does not fit, the last line ends in "...".
// t47: the last keep_tail code points (a " x12" count) are never cut: the last line then ends in "..." plus those.
std::vector<std::u32string> fit_text_lines(FontCache& fonts, int32_t font, std::u32string_view text, float max_width_px,
                                           int32_t max_lines, size_t keep_tail = 0);
// QA D4 / U3: a 1024 x 768 Frame_Background (loadmenuback, battle_summary, victory, ...) is full-screen art, not a
// dialog-sized panel. Its painted panels line up with the GUIDialogs.rc controls only when it is drawn 1:1 over the centred
// 1024 x 768 base area (IDC_PLANET_OVERLAY, x 442..924 of the 975-wide dialog = x 466..948 of the art, whose right panel
// is 465..950); the dialog's frame pieces are then not drawn [inferred from the art and the .rc coordinates].
// [port] IDC_LIST_* of those dialogs are 400 units wide from x = 51, but the art's left panel ends at x = 411.
inline constexpr float kPlanetListWidth = 358.0f;
// [port] IDC_LIST_TACTICAL (y 175) starts inside its own header (y 167..186): its rows start this much lower.
inline constexpr float kPlanetTacticalListDrop = 12.0f;
inline bool gui_full_screen_art(const GuiImage& im) {
    return im.valid && im.size.x == kHudBaseWidth && im.size.y == kHudBaseHeight;
}

class GuiManager {
public:
    // Tables must outlive the manager. `text` may be null (captions shown as written).
    void init(const DialogScript& script, const GuiSkin& skin, const TextureAtlas& atlas, FontCache& fonts,
              const TextDatabase* text = nullptr);
    void resize(uint32_t width, uint32_t height);
    // [port: Gabriel] DPI-style UI scale (hud_canvas.hpp; 1 = 100 %, 0 = the original fitted canvas): takes effect on the
    // next resize().
    void set_ui_scale(float s) { canvas_.ui_scale = s > 0.0f ? s : 0.0f; }
    const HudCanvas& canvas() const { return canvas_; }
    GuiStyle& style() { return style_; }
    void set_sfx_hook(HudSfxHook hook) { sfx_ = std::move(hook); }
    void play_sfx(std::string_view event) const { sfx(event, -1); }
    void set_image_resolver(GuiImageResolver r) { images_ = std::move(r); }
    // Full-window 2D backdrop under every dialog (the front end's MENUBACK_OVERLAY: top logo + LucasArts / Petroglyph
    // logos, GALAXY_MAP_PRESENTATION §5): one quad stretched over the whole canvas at any aspect. "" = none.
    // Controls named in backdrop_hotspots() (default IDC_MENU_PETRO_LOGO, a 118 x 118 hotspot over the overlay's painted
    // Petroglyph logo) follow the stretch: their 4:3 base-area rectangle (left / width by canvas width / 1024, and for a
    // column dialog top / height by canvas height / 768) is mapped through it, so the drawn art stays on the painted logo
    // at any aspect (playtest 3 t13: the unstretched 118-unit button sat up to 30 px off the stretched logo at 16:9).
    void set_backdrop(std::string texture) { backdrop_ = std::move(texture); }
    const std::string& backdrop() const { return backdrop_; }
    std::vector<std::string>& backdrop_hotspots() { return backdrop_hotspots_; }
    // GALAXY_MAP_PRESENTATION §5: the version line "V <version> <build>" drawn with the backdrop, bottom-left 2 pixels in,
    // colour (40, 160, 255, 128) [port: Arial 8]. "" = none. Host: e.g. U"V 1.121 <build>".
    void set_version_text(std::u32string text) { version_text_ = std::move(text); }
    const TextDatabase* text_db() const { return text_; }
    // Edit box clipboard (Ctrl+C / Ctrl+X / Ctrl+V, Ctrl+Insert / Shift+Delete / Shift+Insert). Unset: a clipboard
    // private to the manager [port]; the game host wires the Windows clipboard.
    void set_clipboard(std::function<std::u32string()> get, std::function<void(std::u32string_view)> set) {
        clip_get_ = std::move(get);
        clip_set_ = std::move(set);
    }
    // Caption -> display text: the MasterTextFile entry for the key, else the caption as written [port].
    std::u32string caption_text(std::string_view caption) const;
    // [port] t47: English the port adds in code, on the same text path (TextDatabase::port_text).
    std::u32string port_text(std::u32string_view english) const;
    // [port] How many rows the open list of combo box `i` shows: all its items when the control's height (the RC height holds
    // the drop-down) has room for them, else as many as fit (the list then scrolls).
    int32_t combo_rows(const GuiDialog& d, size_t i) const;

    // Opens a dialog on top of the stack (an open one is brought to the top). Null when the script has no such dialog.
    GuiDialog* open(std::string_view name, GuiOpenOptions opt = {});
    void close(std::string_view name);
    void close_all();
    // The script has this dialog (open or not).
    bool has_dialog(std::string_view name) const { return script_ && script_->find(name) != nullptr; }
    GuiDialog* find(std::string_view name);
    const GuiDialog* find(std::string_view name) const;
    GuiDialog* top() { return stack_.empty() ? nullptr : stack_.back().get(); }
    size_t open_count() const { return stack_.size(); }
    std::vector<std::string> open_names() const;  // bottom -> top

    // Routing (UI.md §4.1): a press at the pixel goes to the dialogs (inside one, or anywhere under a modal top).
    bool hit(int32_t px, int32_t py) const;
    bool has_focus() const;  // keys / wheel go to the dialogs (a modal dialog is open)
    void install(RouteHooks& hooks) const;

    // One frame: `raw` = the window snapshot, `routed` = InputRouter's result (presses owned by PointerTarget::Dialog),
    // `typed` = WM_CHAR text this frame (empty: letters / digits / space derived from key presses [port]).
    void update(const InputFrame& raw, const RoutedInput& routed, double now, std::vector<GuiEvent>& events,
                std::u32string_view typed = {});
    // Draws the stack bottom -> top: dialog k on layer min(k, 11), open combo lists and tooltips on layer 12.
    void build(HudDrawList& dl, double now);

    // Scrolls a list box so its last row is visible (chat logs). False when the dialog has no such list.
    bool scroll_to_end(GuiDialog& d, std::string_view control);
    // Scrolls a list so `row` is its first visible row, or as near as the list's length allows (no empty rows at the end).
    bool scroll_list_to(GuiDialog& d, std::string_view control, int32_t row);
    // The canvas point a mouse press on list row `row` of `control` hits (the row's centre, left of the scroll column),
    // false when there is no such list box or the row is not visible (scroll it into view first). Scripted input uses it.
    bool list_row_point(const GuiDialog& d, std::string_view control, int32_t row, Vec2f& out) const;
    // [port] Takes `units` off the dialog's bottom edge and centres it again (controls keep their .rc positions).
    void trim_dialog_height(GuiDialog& d, float units);
    // [port] word-wraps a list box's items to its text width (the control's font; the scroll column is reserved):
    // an item wider than the list becomes several rows (its text / swatch colour repeated). Win32 list boxes do not
    // wrap; the story dialog's paragraphs (one item per paragraph) would otherwise run out of the dialog. False when
    // the dialog has no such list.
    bool wrap_list_items(GuiDialog& d, std::string_view control);
    // [port] the planet text lists (IDD_PLANET_INFO, the load screens): one item per line of `paragraphs` in `color`
    // (alpha 0 = the skin's), word-wrapped, scrolled to the top. max_width > 0 narrows the list to that many canvas units.
    bool set_paragraphs(GuiDialog& d, std::string_view control, const std::vector<std::u32string>& paragraphs, Rgba8 color,
                        float max_width = 0.0f);

    // Image of a texture name: atlas entry, else the resolver.
    GuiImage image(std::string_view texture) const;
    // Font id for a control (GuiSkin lookup, point -> pixel rule of hud_canvas.hpp, Stretch_Factor).
    int32_t control_font(const GuiDialog& d, size_t i) const;

    // Scroll bar of a list box, or of an open combo's drop-down list (canvas units). active = there are more rows than fit.
    struct ScrollBar {
        bool active = false;
        GuiRect col, up, down, thumb;
        float track_top = 0, track_len = 0;  // below the up arrow, downwards
        int32_t max_top = 0, vis = 1;
    };
    ScrollBar scroll_bar(const GuiDialog& d, size_t i) const;
    // Closed-field height of combo i: the skin's Combo_Box_Popdown_Button height (25 units, UI.md §3.7.1), else 15 [port].
    float combo_field_height(const GuiDialog& d, size_t i) const;

private:
    enum class ScrollPart : uint8_t { None, Up, Down, PageUp, PageDown, Thumb };
    ScrollPart scroll_part_at(const ScrollBar& sb, Vec2f p) const;
    void scroll_press(GuiDialog& d, size_t i, ScrollPart part, double now);
    void scroll_held(double now);
    // Edit helpers.
    GuiRect edit_text_rect(const GuiDialog& d, size_t i) const;
    float edit_measure(const GuiDialog& d, size_t i, std::u32string_view t) const;
    size_t edit_index_at(const GuiDialog& d, size_t i, float x) const;
    void edit_keep_caret_visible(GuiDialog& d, size_t i) const;
    bool edit_insert(GuiDialog& d, size_t i, std::u32string_view t) const;
    void place(GuiDialog& d) const;
    void click(GuiDialog& d, size_t i, std::vector<GuiEvent>& ev, bool keyboard);
    void move_focus(GuiDialog& d, int32_t dir);
    void key_input(GuiDialog& d, const InputFrame& raw, std::u32string_view typed, std::vector<GuiEvent>& ev);
    void list_select(GuiDialog& d, size_t i, int32_t index, std::vector<GuiEvent>& ev);
    void slider_set(GuiDialog& d, size_t i, int32_t v, std::vector<GuiEvent>& ev);
    void scroll_list(GuiControl& c, size_t i, const GuiDialog& d, int32_t rows);
    int32_t scroll_rows(const GuiDialog& d, size_t i) const;  // visible rows of a list / open combo list
    float scroll_width(const GuiDialog& d, size_t i) const;  // scroll bar column width: Scroll_Up_Button art, else 12 [port]
    void draw_scroll_bar(HudDrawList& dl, uint32_t layer, const GuiDialog& d, size_t i, const ScrollBar& sb) const;
    const GuiFontSpec& font_spec(const GuiDialog& d, size_t i) const;
    int32_t visible_rows(const GuiDialog& d, size_t i) const;
    float row_height(const GuiDialog& d, size_t i) const;
    GuiRect combo_field(const GuiDialog& d, size_t i) const;
    GuiRect combo_list(const GuiDialog& d, size_t i) const;
    GuiDialog* modal_top();
    const GuiDialog* modal_top() const;
    GuiEvent make_event(GuiEvent::Kind k, const GuiDialog& d, int32_t control, int32_t value = 0) const;
    void sfx(std::string_view e, int32_t control) const;

    // Drawing helpers.
    bool quad(HudDrawList& dl, uint32_t layer, std::string_view tex, GuiRect r, Rgba8 color = {255, 255, 255, 255}) const;
    void fill(HudDrawList& dl, uint32_t layer, GuiRect r, Rgba8 color) const;
    void frame16(HudDrawList& dl, uint32_t layer, const GuiDialog& d) const;
    void frame9(HudDrawList& dl, uint32_t layer, const GuiDialog& d, std::string_view control, GuiRect r, bool background) const;
    void three_piece(HudDrawList& dl, uint32_t layer, const GuiDialog& d, std::string_view control, std::string_view left,
                     std::string_view middle, std::string_view right, GuiRect r, float fraction = 1.0f) const;
    void draw_control(HudDrawList& dl, uint32_t layer, const GuiDialog& d, size_t i, double now) const;
    void draw_text(HudDrawList& dl, uint32_t layer, const GuiDialog& d, size_t i, std::u32string_view text, GuiRect r,
                   TextAlign align, bool wrap, bool vcenter, bool disabled = false, const Rgba8* color = nullptr) const;
    void draw_fit_text(HudDrawList& dl, uint32_t layer, const GuiDialog& d, size_t i, GuiRect r) const;
    HudTextStyle text_style(const GuiDialog& d, size_t i, bool disabled) const;

    const DialogScript* script_ = nullptr;
    const GuiSkin* skin_ = nullptr;
    const TextureAtlas* atlas_ = nullptr;
    FontCache* fonts_ = nullptr;
    const TextDatabase* text_ = nullptr;
    HudCanvas canvas_ = HudCanvas::make(1024, 768);
    GuiStyle style_;
    HudSfxHook sfx_;
    GuiImageResolver images_;
    std::string backdrop_;
    std::vector<std::string> backdrop_hotspots_{"IDC_MENU_PETRO_LOGO"};
    std::u32string version_text_;
    std::vector<std::unique_ptr<GuiDialog>> stack_;  // bottom -> top
    // Pointer state.
    GuiDialog* hover_dialog_ = nullptr;
    int32_t hover_control_ = -1;
    double hover_since_ = 0.0;
    GuiDialog* press_dialog_ = nullptr;
    int32_t press_control_ = -1;
    bool slider_drag_ = false;
    ScrollPart scroll_part_ = ScrollPart::None;  // scroll bar part held (press_dialog_ / press_control_)
    double scroll_repeat_at_ = 0.0;
    float scroll_grab_ = 0.0f;                   // thumb drag: cursor distance below the thumb's top edge
    bool edit_drag_ = false;                     // edit: mouse selection while the button is held
    std::function<std::u32string()> clip_get_;
    std::function<void(std::u32string_view)> clip_set_;
    std::u32string clipboard_;
    int32_t last_list_click_ = -1;
    int32_t last_list_row_ = -1;
    double last_list_time_ = -1e30;
    Vec2f cursor_{};
};

}  // namespace alamo::ui

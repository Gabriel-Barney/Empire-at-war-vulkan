// P4 HUD panels (docs/re/UI.md Â§2.2 shells, Â§2.3 tooltips / encyclopedia, CommandBarComponents.xml): the encyclopedia
// pop-up, the hero roster and the FoC black-market / sabotage / corruption shells. Same host API as the galactic HUD:
// plain data in (a frame struct), a HudDrawList + hit boxes out, clicks -> HudAction out. Nothing
// here reads or writes sim state.
//
// Data (CommandBarComponents.xml):
//   encyclopedia     encyclopedia_back (e_background.tga, faction frame list, Base_Layer 9 = the panel's layer, Color
//                    alpha 128, Size 262 x 14 = width x row height, Offset 5 2 = padding), encyclopedia_header_text (Arial
//                    Bold 7), encyclopedia_text (Arial 7, rows + description), encyclopedia_right_text / _cost_text (right
//                    justified), encyclopedia_center_text, encyclopedia_icon (Size x = unit icon scale 0.75);
//                    zoomed_back / zoomed_* = the same panel on layer 4 at Default_Offset (-132, 30) [port: "zoomed"
//                    variant]; GameConstants Encyclopedia_Delay 750 ms / Long_Encyclopedia_Delay 4000 ms (TooltipConfig)
//   hero roster      good_hero_frame (i_main_hero_frame_rebel.alo) / evil_hero_frame (i_main_hero_frame_empire.alo),
//                    hero_slot_{good,evil}00..10 (ring Blank_Texture, Build_Texture dial), hero_hb_{good,evil}00..10 bars
//   black market     market_frame (i_black_market.alo, Dialog_Scene): market_00..04 item buttons, market_tech_level00..04,
//                    market_title00..13 texts, market_close, i_blackmarket_encycl
//   sabotage         bm_frame (i_sabotage.alo): bm_RR0C target buttons (rows 00..04 x columns 0..1), bm_title_R0C prices,
//                    bm_title_402..4011 names, bm_title / bm_title01 header, bm_tech_level, bm_close, bm_text_steal
//   corruption       corruption_shell (i_corrupt_choice.alo): choices a / b / c - corrupt_button_icon_x, corrupt_title_x,
//                    corrupt_cost_x, corrupt_text00x..04x, corrupt_icon_00x..07x (requirements); corrupt_planet_title00/01,
//                    corrupt_planet_defiler_00/01, corrupt_close, i_corrupt_encycl
// Clicks: hero slot -> HudAction::HeroSelect{unit}; black-market item -> HudAction::BlackMarketBuy{unit = item id, ids =
// {planet}, value = row} (the host answers with the SpecialAbility kind-4 purchase command, docs/re/P4_SIM_SYSTEMS.md
// §1.4); sabotage target -> HudAction::SabotageTarget{unit = target, ids = {planet}}; corruption choice ->
// HudAction::CorruptionChoice{unit = planet, value = corruption type}; *_close -> HudAction::PanelClose{name = shell}.
// Right click on a panel = PanelClose [port].
//
// Host wiring: load the shells with ShellSet (names below) and apply() them to each panel's layout(); keep
// ShellSet::set_visible(shell, panel.open()) so ShellDrawer draws the open ones; route with hit_test() before the
// galactic / tactical HUD's (RouteHooks::hud_hit) and hand the RoutedInput to the panel that owns() the hovered or
// pressed component. Without shell bones the components sit at [port] fallback positions.
// [port] layout / content choices: market_title00 = title, 01 = credits / subtitle, 02 = hovered item description, 03 =
// hovered item cost, item k's name / price on market_title(4 + 2k) / (5 + 2k); sabotage name k on bm_title_40(k + 2);
// hero frame good = the local player's bar, evil = the enemy bar (FIXPASS_GALACTIC §2.1); the encyclopedia
// (HUD_GAPS §2.2) appears Encyclopedia_Delay (Long_Encyclopedia_Delay with IDC_DELAY_TOOLTIPS) after the hovered subject
// last changed, fades in / out at Encyclopedia_Fade_Rate, stays at least Encyclopedia_Min_Display_Time, and sits on the
// active command-bar shell's hint bone (not the cursor), kept inside the canvas.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/ui/hud_buttons.hpp"
#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"
#include "alamo/ui/hud_input.hpp"
#include "alamo/ui/tactical_hud.hpp"

namespace alamo::ui {

class FontCache;

// ---- Encyclopedia ---------------------------------------------------------------------------------------------
struct EncyclopediaRow {
    std::u32string left, right;  // label / value (encyclopedia_text / encyclopedia_right_text)
    std::u32string center;       // a centred row instead (encyclopedia_center_text) when not empty
};
// t40 (HUD_GAPS §2.2, captures C3_*tooltip*): a unit shows name, class + ability icons, description, then the Strong / Weak
// icon rows; `rows` are for what is not a unit (a planet's income) [port].
struct EncyclopediaEntry {
    std::u32string header;       // unit / planet name
    std::string icon;            // atlas texture (unit icon), "" none
    std::u32string cost;         // encyclopedia_cost_text, "" none
    int32_t population = 0;      // Population_Value, shown in e_topbar's circle; 0: e_topbar2, no circle
    std::u32string unit_class;   // Encyclopedia_Unit_Class ("Class: Fighter"), "" none
    std::vector<std::string> abilities;  // ability button art (i_sa_*.tga), list order
    std::vector<EncyclopediaRow> rows;
    std::u32string description;  // long form (Encyclopedia_Text), shown after the long delay
    std::u32string strong_label, weak_label;  // TEXT_GOOD_AGAINST / TEXT_VULNERABLE_TO
    std::vector<std::string> strong, weak;    // unit icons (Encyclopedia_Good_Against / _Vulnerable_To): both empty = no rows
};

class EncyclopediaPanel {
public:
    enum class Stage : uint8_t { Hidden, Shown };
    void init(const ComponentTable& table, const TextureAtlas& atlas, FontCache& fonts, const TextDatabase* text = nullptr);
    void set_config(const TooltipConfig& c) { cfg_ = c; }
    const TooltipConfig& config() const { return cfg_; }
    // What the cursor is over this frame (a HUD component and / or a world object; -1 / 0 = nothing), called once per
    // frame. HUD_GAPS §2.2: the countdown (cfg.encyclopedia_wait(): 750 ms, 4000 ms with delay_tooltips) restarts
    // whenever the subject changes or is lost; when it expires the pop-up is created for that subject (shown_*) and
    // fades in at encyclopedia_fade_rate / s. Once the subject is lost (or replaced) it stays until
    // encyclopedia_min_display s after it appeared, then fades out at the same rate and is destroyed (Stage::Hidden).
    // [port] a new subject's pop-up replaces a fading one and starts again from alpha 0.
    void update(int32_t component, uint32_t object, double now);
    Stage stage() const { return stage_; }
    float alpha() const { return alpha_; }
    // The hovered subject (countdown target).
    int32_t component() const { return component_; }
    uint32_t object() const { return object_; }
    // The subject the pop-up shows (kept while it fades out after the hover is lost): the host builds the entry for it.
    int32_t shown_component() const { return shown_component_; }
    uint32_t shown_object() const { return shown_object_; }
    // Draws the panel for `e` (nothing while Hidden), every colour's alpha scaled by alpha(). `anchor` = top-left corner
    // in canvas units; nullopt = near the cursor pixel. `zoomed`: the zoomed_* variant. Returns the panel rectangle size
    // (0 when nothing was drawn).
    Vec2f build(HudDrawList& dl, const HudCanvas& canvas, const EncyclopediaEntry& e, Vec2f cursor_px,
                std::optional<Vec2f> anchor = std::nullopt, int32_t faction_alt = 1, bool zoomed = false) const;
    // HUD_GAPS §2.2 anchor: the panel's bottom-left corner at hint_anchor(layout, canvas) (the hint bone of the active
    // command-bar shell + encyclopedia_back / zoomed_back Default_Offset "offset from hint bone"); it does not follow the
    // cursor. `layout` = the HUD whose shells are on screen (GalacticHud / TacticalHud::layout()), same canvas size.
    Vec2f build(HudDrawList& dl, const HudCanvas& canvas, const EncyclopediaEntry& e, const HudLayout& layout,
                int32_t faction_alt = 1, bool zoomed = false) const;
    // Hint bone [M: the shell's help-text bone, the only text area bone of each command bar]: the first of
    // Text_adv_help (map_shell, planetary_mode.alo), Text_galactic_help (i_galactic_controls.alo), Text_tactical_help
    // (i_tactical_controls.alo) found on an ACTIVE shell of `layout`, + Default_Offset (+Y up). [port] without one:
    // the galactic bone's place on the base canvas (65, 309 from the bottom-left corner).
    Vec2f hint_anchor(const HudLayout& layout, const HudCanvas& canvas, bool zoomed = false) const;
    static constexpr const char* kHintBones[3] = {"Text_adv_help", "Text_galactic_help", "Text_tactical_help"};

private:
    Vec2f build_at(HudDrawList& dl, const HudCanvas& canvas, const EncyclopediaEntry& e, Vec2f cursor_px,
                   std::optional<Vec2f> top_left, std::optional<Vec2f> bottom_left, int32_t faction_alt, bool zoomed) const;
    const ComponentTable* table_ = nullptr;
    const TextureAtlas* atlas_ = nullptr;
    FontCache* fonts_ = nullptr;
    const TextDatabase* text_ = nullptr;
    TooltipConfig cfg_;
    int32_t component_ = -1;
    uint32_t object_ = 0;
    double since_ = 0.0;
    int32_t shown_component_ = -1;
    uint32_t shown_object_ = 0;
    double shown_at_ = 0.0, last_ = 0.0;
    bool has_last_ = false;
    float alpha_ = 0.0f;
    Stage stage_ = Stage::Hidden;
};

// ---- Shell panels (common part) ---------------------------------------------------------------------------------
struct PanelSlot {
    int32_t component = -1;
    std::u32string text, text2;
    std::string icon;          // icon override ("" = data)
    bool disabled = false, selected = false, flash = false;
    float dial = -1.0f;        // Build_Texture dial 0..1 (< 0 none)
    float bar = -1.0f;         // Bar components: fill 0..1 (< 0 hidden)
    int32_t alt = -1;          // alternate texture index (-1: the frame's faction)
    uint32_t id = 0;           // object / type the slot stands for
    int32_t value = -1;        // panel-specific
};

struct PanelFrame {
    bool open = false;
    uint8_t faction_alt = 1;
    bool shells_loaded = false;
    double time = 0.0;
    Vec2f cursor_px{};
};

class HudPanel {
public:
    virtual ~HudPanel() = default;
    // false when none of the panel's shells is in the table.
    bool init(const ComponentTable& table, const TextureAtlas& atlas, FontCache& fonts, const TextDatabase* text = nullptr);
    void resize(uint32_t width, uint32_t height);
    // [port: Gabriel] DPI-style UI scale (hud_canvas.hpp; 1 = 100 %, 0 = the original fitted canvas): takes effect on the
    // next resize().
    void set_ui_scale(float s) { canvas_.ui_scale = s > 0.0f ? s : 0.0f; }
    const HudCanvas& canvas() const { return canvas_; }
    HudLayout& layout() { return layout_; }
    HudButtons& buttons() { return buttons_; }
    void set_sfx_hook(HudSfxHook hook) { buttons_.set_sfx_hook(std::move(hook)); }
    void set_tooltip_config(const TooltipConfig& c) { tooltip_cfg_ = c; }

    bool open() const { return open_; }
    // Shell component indices this panel uses (for ShellSet::load / set_visible), and the one shown now (-1: closed).
    const std::vector<int32_t>& shells() const { return shells_; }
    int32_t active_shell() const { return open_ ? active_shell_ : -1; }
    virtual std::vector<std::string> shell_names() const = 0;
    // Shell clicks of this frame -> `actions`. `wheel` = the frame's wheel notches (InputFrame::wheel), read by SlicePanel only.
    virtual void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<HudAction>* actions = nullptr,
                              float wheel = 0.0f) = 0;

    const HudDrawList& draw_list() const { return dl_; }
    const std::vector<HudHitBox>& hit_boxes() const { return hits_; }
    int32_t hit_test(int32_t px, int32_t py) const;
    bool owns(int32_t component) const;  // a slot of the last build
    const std::vector<PanelSlot>& slots() const { return slots_; }

protected:
    // Subclasses fill slots_ then call draw_slots(). `shell` = index into shells_ to activate.
    void begin(const PanelFrame& f, int32_t shell);
    void draw_slots(const PanelFrame& f);
    void end(const PanelFrame& f);
    std::vector<ButtonClick> clicks(const RoutedInput& routed, bool shift, bool ctrl, double now);
    // Shared by build() and update_input() of the shell panels.
    void add_text_slot(std::string_view name, std::u32string text);  // a text-only slot (text role)
    void add_close(std::string_view name);                           // the shell's close button slot
    HudAction click_action(const ButtonClick& k, HudAction::Kind kind) const;  // kind + component + component name
    template <class Accept, class Fill>
    void shell_clicks(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<HudAction>* actions,
                      HudAction::Kind kind, Accept accept, Fill fill);
    const PanelSlot* slot_of(int32_t component) const;
    int32_t find(std::string_view name) const { return table_ ? table_->find(name) : -1; }
    virtual void fallback_layout() = 0;  // [port] positions without shell bones
    void put(std::string_view name, float x, float y);  // fallback position (canvas units, +Y up)

    const ComponentTable* table_ = nullptr;
    const TextureAtlas* atlas_ = nullptr;
    FontCache* fonts_ = nullptr;
    const TextDatabase* text_ = nullptr;
    HudCanvas canvas_ = HudCanvas::make(1024, 768);
    HudLayout layout_;
    HudButtons buttons_;
    TooltipState tooltip_;
    TooltipConfig tooltip_cfg_;
    HudDrawList dl_;
    std::vector<HudHitBox> hits_;
    std::vector<PanelSlot> slots_;
    std::vector<int32_t> shells_;
    int32_t active_shell_ = -1;
    bool open_ = false;
};

// ---- Hero roster ----------------------------------------------------------------------------------------------
struct HeroEntry {
    uint32_t object = 0;       // the hero object; 0 / kNoObject while it is being built or respawning (clicks do nothing)
    std::string icon;
    float health = 1.0f;       // 0..1
    float respawn = -1.0f;     // 0..1 while dead and returning (dial), < 0 alive
    bool selected = false;
    // GALAXY_MAP_PRESENTATION §4 sort keys (host: the hero type's Ranking_In_Category and its type name as written in the
    // XML, e.g. "Boba_Fett_Team"; hero_tie_key applies the case rule).
    int32_t ranking = 0;
    std::string type_name;
    // FIXPASS_GALACTIC §2 click targets (host feeds):
    uint32_t planet = commands::kNoObject;     // galactic: the planet found by walking up the hero's parent chain
                                               // (kNoObject = in hyperspace / no planet -> the click does nothing)
    uint32_t container = commands::kNoObject;  // tactical: the hero's parent / container (kNoObject = none)
    std::u32string tooltip;                    // the hero type's tooltip text (display name); "" = no tooltip
};
// FIXPASS_GALACTIC §2.4 tie key. FoC (comparator 0x3202e0) breaks equal rankings by type +0x11C = CRC-32 of the
// UPPER-CASED type name. Capture C3 (FoC 1.121): the name is the hero's bar type (game::hero_bar_type), not the *_Team
// type: Emperor_Palpatine, Boba_Fett, General_Veers, Grand_Moff_Tarkin, Accuser_Star_Destroyer, right to left.
uint32_t hero_tie_key(std::string_view type_name);
// Comparator (stable insertion sort): a before b when both rankings are > 0 and a's is lower; equal rankings ->
// hero_tie_key ascending (unsigned); a ranking <= 0 on either side = equal (gathering order kept). Slot 00 (the frame's
// first slot: rightmost on the friendly bar, which grows left; leftmost on the enemy bar) gets the first entry.
// HeroPanel::build sorts its copy of the frame's heroes.
void sort_hero_entries(std::vector<HeroEntry>& heroes);
struct HeroPanelFrame : PanelFrame {
    // FIXPASS_GALACTIC §2.1: good_hero_frame = the LOCAL player's bar (self / allied heroes; the only slots with click
    // callbacks), evil_hero_frame = the enemy bar (no callbacks: clicks do nothing). Host: good = true for the local
    // player's heroes whatever the faction.
    bool good = true;
    bool tactical = false;     // false: galactic map clicks (§2.2), true: tactical clicks (§2.3)
    std::span<const HeroEntry> heroes;  // up to 11
};
// Input (FIXPASS_GALACTIC §2), good slots only, left button only (right click: nothing). A double click = a second
// left click on the same slot within InputConfig::double_click_time (0.5 s) / 4 px; the second click repeats the
// single-click action, then adds the double-click one:
//   galactic single  HeroFocusPlanet {unit = hero, ids = {planet}}: centre the galactic camera on the planet and
//                    select / focus it (the fleet is not selected, no panel opens). Nothing while the hero has no
//                    object (building / respawning) or no planet (hyperspace).
//   galactic double  + HeroZoomPlanet {unit = hero, ids = {planet}}: with a planet selected, camera zoom state 0 / 2 ->
//                    zoom into it, 1 / 3 -> zoom out (or close the advanced planet map when open); story control lock
//                    gates both.
//   tactical single  HeroSelect {unit = hero, ids = {hero}, shift / ctrl}: replace the selection (modifier: add [M]).
//                    Nothing without an object.
//   tactical double  + HeroSelectGroup {unit = hero, ids = {container} or {} = every object of the hero's type}, then
//                    centre the tactical camera on the hero.
// Hover: hover_hero() / hover_component() name the hovered good slot's hero for the host's EncyclopediaPanel::update;
// the panel draws HeroEntry::tooltip after Tooltip_Delay itself.
class HeroPanel : public HudPanel {
public:
    std::vector<std::string> shell_names() const override { return {"good_hero_frame", "evil_hero_frame"}; }
    void build(const HeroPanelFrame& f);
    void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<HudAction>* actions = nullptr,
                      float wheel = 0.0f) override;
    uint32_t hover_hero() const;        // hovered good slot's hero object (0 = none)
    int32_t hover_component() const;    // its slot component (-1 = none)

private:
    void fallback_layout() override;
    bool good_ = true, tactical_ = false;
    std::vector<HeroEntry> shown_;      // sorted entries of the last build (index = slot number)
    int32_t last_click_component_ = -1;
    double last_click_time_ = -1e30;
    int32_t last_click_x_ = 0, last_click_y_ = 0;
};

// ---- Black market (FoC) ---------------------------------------------------------------------------------------
struct BlackMarketItem {
    uint32_t item = 0;         // the host's item id (alamo_game: SpecialRules::items index), reported by the click
    std::string icon;
    std::u32string name, description;
    int32_t cost = -1;
    bool enabled = true;       // affordable / allowed
    // modc t20: the item faction's Tech_Level_Icon_Name (FoC i_button_r_tech_upgrade_ / _e_): market_tech_levelNN shows the
    // first of its Icon_Alternate_Texture_Name entries that starts with it (any case); "" or no match: hidden.
    std::string tech_icon;
};
struct BlackMarketFrame : PanelFrame {
    uint32_t planet = commands::kNoObject;  // the market's planet (reported by the click)
    std::u32string title, subtitle;
    std::span<const BlackMarketItem> items;  // up to 5
};
class BlackMarketPanel : public HudPanel {
public:
    std::vector<std::string> shell_names() const override { return {"market_frame"}; }
    void build(const BlackMarketFrame& f);
    void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<HudAction>* actions = nullptr,
                      float wheel = 0.0f) override;

private:
    void fallback_layout() override;
    uint32_t planet_ = commands::kNoObject;
};

// ---- Slice dialog (Rebel tech theft, REMAINING_GAPS §1.3 step 5) ---------------------------------------------------
// The retail dialog (0x3c95d0: TEXT_SLICE_DIALOG_TITLE / _DESCRIPTION, one row per slice entry, Cancel) is a GUI dialog
// whose resource is not identified. [port] stand-in: the market_frame shell (i_black_market.alo) with the black-market
// slot mapping - market_title00 title, 01 description, row k name / price on market_title(4 + 2k) / (5 + 2k), hovered
// row's description (or "Chance n%") / price on market_title02 / 03; 5 rows visible, the mouse wheel over the panel
// scrolls one row per notch (rows beyond 5); market_tech_level icons hidden. Row click (enabled rows) ->
// HudAction::SliceSelect{unit = type, ids = {planet, slicer}, value = row} - the host sends SpecialAbility event 13 kind 2
// (§1.3.1); market_close / right click -> PanelClose (Cancel: sends nothing [M]). Only one of BlackMarketPanel /
// SlicePanel should be open at a time (same shell).
struct SliceRow {
    uint32_t type = 0;          // GameObjectType index of the slice entry (reported by the click)
    std::string icon;
    std::u32string name, description;  // description "" = "Chance n%" from `chance`
    int32_t cost = -1;          // the entry's price (rounded), < 0 none
    float chance = -1.0f;       // 0..1 success chance, < 0 not shown
    bool enabled = true;        // affordable (price <= credits)
};
struct SliceFrame : PanelFrame {
    uint32_t planet = commands::kNoObject;  // the slicer's planet
    uint32_t slicer = 0;                    // the slicer (R2D2) object id
    std::u32string title, description;      // TEXT_SLICE_DIALOG_TITLE / _DESCRIPTION
    std::span<const SliceRow> rows;         // every slice entry, candidate order
};
class SlicePanel : public HudPanel {
public:
    static constexpr int32_t kVisibleRows = 5;
    std::vector<std::string> shell_names() const override { return {"market_frame"}; }
    void build(const SliceFrame& f);
    // `wheel` = this frame's wheel notches (InputFrame::wheel); used while routed.wheel_target is the HUD and the cursor
    // is over one of the panel's components.
    void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<HudAction>* actions = nullptr,
                      float wheel = 0.0f) override;
    int32_t first_row() const { return first_; }

private:
    void fallback_layout() override;
    uint32_t planet_ = commands::kNoObject, slicer_ = 0;
    int32_t first_ = 0, rows_ = 0;
    bool was_open_ = false;
};

// ---- Sabotage (FoC) -------------------------------------------------------------------------------------------
struct SabotageTarget {
    uint32_t object = 0;
    std::string icon;
    std::u32string name;
    int32_t cost = -1;
    bool enabled = true;
};
struct SabotageFrame : PanelFrame {
    uint32_t planet = commands::kNoObject;
    std::u32string title, subtitle, steal_text;
    std::span<const SabotageTarget> targets;  // up to 10 (5 rows x 2 columns, row-major)
};
class SabotagePanel : public HudPanel {
public:
    std::vector<std::string> shell_names() const override { return {"bm_frame"}; }
    void build(const SabotageFrame& f);
    void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<HudAction>* actions = nullptr,
                      float wheel = 0.0f) override;

private:
    void fallback_layout() override;
    uint32_t planet_ = commands::kNoObject;
};

// ---- Corruption choice (FoC) ----------------------------------------------------------------------------------
struct CorruptionOption {
    int32_t corruption_type = -1;  // CorruptionTypeEnum (OBJECT_TYPES.md: Intimidation 0 .. Bonded_Citizens 7)
    std::string icon;
    std::u32string title;
    int32_t cost = -1;
    std::vector<std::u32string> lines;      // up to 5 (corrupt_text00x..04x)
    std::vector<std::string> requirements;  // up to 8 icons (corrupt_icon_00x..07x)
    bool enabled = true;
};
struct CorruptionFrame : PanelFrame {
    uint32_t planet = commands::kNoObject;
    std::u32string planet_title, planet_subtitle;
    std::u32string defiler_text, defiler_text2;
    std::span<const CorruptionOption> options;  // up to 3 (a, b, c)
};
class CorruptionPanel : public HudPanel {
public:
    std::vector<std::string> shell_names() const override { return {"corruption_shell"}; }
    void build(const CorruptionFrame& f);
    void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<HudAction>* actions = nullptr,
                      float wheel = 0.0f) override;

private:
    void fallback_layout() override;
    uint32_t planet_ = commands::kNoObject;
};

// ---- Hack (FoC, i_hack.alo) -----------------------------------------------------------------------------------
// hack_shell: hack_title01 (title), hack_text_00..06 (Hack_Desc lines), hack_cost, hack_hack (confirm button, its caption
// = confirm_text), hack_close. hack_hack -> HudAction::HackConfirm{unit = target, ids = {planet}} (disabled when
// !enabled); close / right click -> PanelClose.
struct HackFrame : PanelFrame {
    uint32_t target = 0;                    // object to hack
    uint32_t planet = commands::kNoObject;
    std::u32string title, cost, confirm_text = U"Hack";
    std::vector<std::u32string> lines;      // up to 7
    bool enabled = true;
};
class HackPanel : public HudPanel {
public:
    std::vector<std::string> shell_names() const override { return {"hack_shell"}; }
    void build(const HackFrame& f);
    void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<HudAction>* actions = nullptr,
                      float wheel = 0.0f) override;

private:
    void fallback_layout() override;
    uint32_t target_ = 0, planet_ = commands::kNoObject;
};

}  // namespace alamo::ui

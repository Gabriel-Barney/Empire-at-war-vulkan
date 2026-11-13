// P2 tactical HUD (docs/re/UI.md §1, §2): owns the canvas, the CommandBarComponents-driven layout, the
// world-space layer (hud_world.hpp), the selected-unit panel (s_select_NN / s_health_NN / s_shield_NN), the
// ability buttons (special_button_NN + special_border_NN), the tactical build queue (tqueueNN), the land
// reinforcement pad (r_RRCC), the command / game buttons, tooltips, the radar, order feedback (waypoint lines,
// move acknowledgements, hard-point reticles), the top-bar credits text, the HUD hit boxes and the HUD actions.
// Each frame:
//   resize(w, h) on a resolution change (canvas + three ortho cameras rebuilt, fonts re-rasterised);
//   route input with InputRouter (hud_input.hpp) using hit_test() as RouteHooks::hud_hit, then
//   update_input(routed, ...) -> commands::EventData (Select / SetUnitAbilityMode / SetAbilityAutofire /
//   Production cancel / Retreat / MoveToPosition) + HudAction host requests (targeting modes, camera jumps,
//   menu buttons); then
//   build(frame, units, types) -> draw_list() (13 layers) + selection_blobs() (3D selection circles).
// Read-only towards the sim: selection / health / abilities / queues come in as copies or through the
// HudProviders callbacks (until the S9 ability state lands); actions leave as commands.
//
// Shells (stage 2): load the .alo shells with ShellSet (hud_shell.hpp), apply() them to layout(), set
// TacticalHudFrame::shells_loaded and draw them with alamo_ui_draw ShellDrawer at the ShellModels step. Without
// them a [port] 2D frame stands in for the radar and the command bar and components sit at the [port]
// fallback positions (apply_tactical_fallback_layout).
#pragma once
#include <cstdint>
#include <array>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/ui/hud_buttons.hpp"
#include "alamo/ui/hud_canvas.hpp"
#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/hud_draw.hpp"
#include "alamo/ui/hud_input.hpp"
#include "alamo/ui/hud_markers.hpp"
#include "alamo/ui/hud_radar.hpp"
#include "alamo/ui/hud_world.hpp"
#include "alamo/ui/orders.hpp"

namespace alamo::ui {

class FontCache;

struct SelectedGroup {
    uint32_t type = 0;
    std::vector<uint32_t> ids;  // ascending
    float health = 1.0f;        // mean health fraction
    float shield = -1.0f;       // mean shield fraction (< 0: none)
};
// Selected units grouped by type, groups ordered by their lowest id [port], at most `max_groups`.
std::vector<SelectedGroup> group_selection(std::span<const HudUnit> units, size_t max_groups = 48);

// Ability state of a selected group: read-only S9 queries (hud_abilities.hpp abilities_from_s9 fills it from
// tactical::abilities::AbilitySystem) or any host callback.
struct HudAbility {
    uint8_t ability_type = 0;   // 1..76 (COMMANDS.md §2 TacticalSpecialAbility)
    std::string icon;           // atlas texture (e.g. "i_sa_*.tga")
    float recharge = 1.0f;      // 0..1, 1 = ready (the dial shows recharge x 100 while < 1)
    bool active = false;        // mode on (selected look)
    bool enabled = true;
    bool targeted = false;      // needs a target: a click starts the targeting mode (HudAction)
    uint8_t target_kind = 0;    // targeting.hpp TargetKind (0 = by ability_target_kind); BeginAbilityTargeting `value`
    bool autofire = false;      // autofire on (overlay shown); right click toggles
    std::string tooltip_key;    // MasterTextFile key for the tooltip ("" = the component's)
};
struct HudQueueEntry {
    uint16_t type_index = 0;
    std::string icon;
    float progress = 0.0f;      // 0..1 (Build_Texture_Name dial)
    uint32_t count = 1;
    uint32_t location = commands::kNoObject;  // Production event location
    uint8_t queue = 0, index = 0;
    bool land = false;          // galactic: ground queue (queue05..09, Build_Queue_Land) instead of space (queue00..04)
};
// One tactical buy button (Build_00..05, group "Tactical Build Buttons"): a type the selected builder (space station,
// build pad) can produce now. Host feed per frame from the builder's Tactical_Buildable_Objects_* list.
struct HudBuildOption {
    enum class Block : uint8_t {
        None,           // buyable
        QueueFull,      // Tooltip_Text[0] TEXT_PRODUCTION_QUEUE_FULL
        PopCap,         // Tooltip_Text[1] TEXT_PRODUCTION_POP_CAP_REACHED
        Funds,          // Tooltip_Text[2] TEXT_PRODUCTION_INSUFFICIENT_FUNDS
        Prerequisite,   // tech / prerequisite missing [port: disabled, no tooltip]
        Limit,          // build limit reached [port: disabled, no tooltip]
    };
    uint16_t type_index = 0;    // Production event type
    std::string icon;
    int32_t cost = -1;          // credits (text2, gold, under the button); < 0 none
    uint32_t queued = 0;        // entries of this type in the builder's queue (text, > 1 shown)
    Block block = Block::None;  // != None: disabled
    std::string tooltip_key;    // ui3 t07: the type's text (Text_ID) shown as the tooltip of a buyable option
};
struct HudReinforcement {
    uint32_t unit = 0;          // reinforcement-pool object (Reinforce event "unit")
    std::string icon;
    uint32_t count = 1;
    bool available = true;
};
struct HudProviders {
    std::function<void(const SelectedGroup& group, std::vector<HudAbility>& out)> abilities;
    std::function<void(std::vector<HudQueueEntry>& out)> build_queue;
    std::function<void(std::vector<HudReinforcement>& out)> reinforcements;
};

// A request the HUD cannot turn into a command by itself (the host / targeting controller / P4 handle it).
struct HudAction {
    enum class Kind : uint8_t {
        Button,                 // a game button (component name): menu, camera, beacon, special weapon, speed, ...
        BeginAbilityTargeting,  // targeted ability chosen: ability_type, ids = users, value = TargetKind (-1 by type)
        BeginReinforcement,     // reinforcement chosen: unit (drop point picked in the world -> reinforce_event)
        RadarCameraJump,        // look at world (x, y)
        RadarPing,              // radar event to show (name) at (x, y), e.g. Default_Click
        // P4 galactic HUD (galactic_hud.hpp):
        GalacticSelect,         // fleet / land-force pad clicked: ids = objects to select (host -> Select command)
        PlanetInfo,             // planet_info / Planet name clicked: unit = planet object id
        // P4 panels (hud_panels.hpp), appended 2026-10-27:
        HeroSelect,             // hero roster slot: unit = hero object (host -> Select / camera)
        SabotageTarget,         // sabotage target: unit = target object, ids = {planet}
        CorruptionChoice,       // corruption option: unit = planet, value = corruption type
        PanelClose,             // *_close button / right click: name = the panel's shell component
        HackConfirm,            // hack_shell hack_hack button: unit = the hacked target, ids = {planet}
        BlackMarketBuy,         // P4 host, appended: black-market item: unit = item id, ids = {planet}, value = row
        SliceSelect,            // OI 2026-10-28, appended: slice dialog row (hud_panels.hpp SlicePanel): unit = type,
                                // ids = {planet, slicer}, value = row index in SliceFrame::rows
        ResumeGame,             // OI 2026-10-28, appended: "Resume Game" on the tactical Game Paused banner (pause_shell,
                                // attack_button): the host unpauses (pause_event(false)); name = "attack_button"
        BattleBegin,            // GALACTIC_DIALOGS §1.5, appended: "Begin" on the battle-pending prompt (choice_button_left /
                                // attack_button_single): host -> conflict state 3 (start the tactical battle), release pause
        BattleAutoResolve,      // "Auto-Resolve" (choice_button_right): host -> conflict state 6 with force, release pause
        // FIXPASS_GALACTIC §2 hero bar (hud_panels.hpp HeroPanel), appended 2026-10-28:
        HeroFocusPlanet,        // galactic left click: unit = hero, ids = {planet}: centre the galactic camera on the
                                // planet and select / focus it (fleet not selected, no panel)
        HeroZoomPlanet,         // galactic double click (after HeroFocusPlanet): unit = hero, ids = {planet}: zoom state
                                // 0 / 2 -> zoom into the planet; 1 / 3 -> zoom out (or close the advanced map)
        HeroSelectGroup,        // tactical double click (after HeroSelect): unit = hero, ids = {container} or {} = all
                                // objects of the hero's type; then centre the tactical camera on the hero
        // Tactical production (TacticalHud), appended 2026-10-28:
        TacticalBuy,            // Build_NN left click on an enabled option: unit = builder (TacticalHudFrame::builder),
                                // value = type_index, shift: host -> Production {type_index, location = builder}
        TacticalCancel,         // tqueueNN left click: unit = location, value = type_index, ids = {queue, index}: host ->
                                // Production {type_index, location, cancel = 1, queue, index}
        // ui4 t02, appended 2026-11-13: a left click on a greyed galactic build pad (GalacticBuildOption::block != None):
        // value = the Block, name = the pad component; the host plays the GUI negative feedback, no command is sent.
        BuildRefused,
    };
    Kind kind = Kind::Button;
    int32_t component = -1;
    std::string name;           // component name / radar event name
    uint8_t ability_type = 0;
    int32_t value = -1;         // panel-specific (CorruptionChoice type)
    uint32_t unit = 0;
    bool queued = false;        // [port: Gabriel] issued with the queue modifier (BeginAbilityTargeting: queue the order)
    std::vector<uint32_t> ids;
    float x = 0, y = 0;
    Button button = Button::Left;
    bool shift = false, ctrl = false;
};

// A 3D model the HUD wants drawn through an ortho camera (radar event models, UI.md §1.7: RadarMap.xml
// Event_Model_Name, e.g. I_minimap_*.alo). Model space -> canvas: translate(center) x uniform `scale` (canvas units per
// model unit); drawn by alamo_ui_draw HudModelDrawer at the Models2 step (HUD camera kHudCameraModels2).
struct HudModelDraw {
    std::string model;     // .alo name
    Vec2f center{};        // canvas units
    float scale = 1.0f;    // canvas units per model unit = Event_Model_Scale x canvas width / 2 [M: RadarMap.xml's logical
                           // screen space; the vanilla models are one 20-60 unit quad]
    float age = 0.0f;      // seconds since the event started (animation time)
    uint32_t event = 0;    // RadarEvent id
};

// One objective line (tactical Mission Objectives box, galactic objectives panel).
struct HudObjective {
    enum class State : uint8_t { Active, Complete, Failed };
    std::string text_key;               // MasterTextFile key
    std::u32string text;                // overrides the key when not empty
    State state = State::Active;
};

// Tactical order buttons c_button00..05 (attack, attack-move, move, waypoint, full stop, guard; docs/re/HUD_GAPS.md §5):
// never hidden - visible whenever the tactical command bar is - only enabled / disabled every frame from the local
// player's OWN selected units (enemy / allied units in the selection are ignored):
//   attack      disabled when no own selected unit can attack (attack behaviour)
//   attack-move disabled when none can attack AND none can move
//   move, waypoint, full stop   disabled when none can move (locomotor)
//   guard       disabled when none can attack AND none can move
// controls_locked (story LOCK_CONTROLS / Lua Lock_Controls) disables all six. A Lua GUI_Component_Visibility /
// _Enable call on them (mods only) overrides this (ButtonRuntime::script_visible / script_enabled).
struct HudOrderButtons {
    bool can_attack = false;     // some own selected unit has an attack behaviour
    bool can_move = false;       // some own selected unit has a locomotor
    bool controls_locked = false;
};
bool order_button_enabled(uint32_t button, const HudOrderButtons& s);  // button 0..5 = c_button00..05

// Land b_special_weapon = the bombing-run button (HUD_GAPS §9.1 - §9.3, cu9g t03). Never hidden in land, only disabled
// every frame (controls locked, no bombing-run record, run not available); alternate icon 3, tooltip 3
// (TEXT_TOOLTIP_BOMBRUN_BUTTON); the Build_Texture_Name dial is the cooldown. `present` false (no tactical HUD feed): the
// component keeps its data state. ui1 t04: in space the same component is the special-weapon button (HUD_GAPS §9.1,
// 0x317d00: icon 0..2 = the weapon's Special_Weapon_Index, a station 2; tooltip 0..2, or 4 TEXT_TOOLTIP_REQUIRES_COMM_ARRAY
// while special weapons are not allowed); `hidden` [port] when the local player has no space special weapon.
struct HudBombingRunButton {
    bool present = false;       // a battle: the HUD drives the button
    bool hidden = false;        // ui1 t04: space without a special weapon
    int32_t icon = 3;           // Icon_Alternate_Texture_Name index (land 3)
    int32_t tooltip = 3;        // Tooltip_Text index (land 3)
    bool disabled = true;       // !ready (0x312570)
    float dial = -1.0f;         // 0..1 radial dial (Build_Texture_Name); < 0: none. [port] none when ready (the 0x495250 value
                                // 1.0 draws nothing, as the ability buttons; HUD_GAPS §9.9 item 1 is a probe question)
};

// ui3 t03 / t04: the other command-bar buttons the land / space HUD update enables in the same pass as c_button00..05
// (HUD_GAPS §5.1: "reinforcement availability, retreat allowed") and the reinforcement window's texts. `present` false
// (no battle feed): the components keep their data state. Lua GUI_Component_Visibility / _Enable override (mods).
struct HudCommandButtons {
    bool present = false;
    bool controls_locked = false;    // story LOCK_CONTROLS: all of them disabled
    // b_retreat: enabled when the retreat request would be accepted (BATTLE_TRANSITIONS §3.4 vf9 reason 0). Its
    // Build_Texture_Name dial [port]: the retreat-allowed countdown while it runs (0..1), then the running retreat's
    // countdown; < 0 none.
    bool retreat_enabled = false;
    float retreat_dial = -1.0f;
    // b_reinforcement: enabled when some reinforcement-pad entry can be called (reserve_available); selected look while a
    // drop point is being picked.
    bool reinforce_enabled = false;
    bool reinforce_selected = false;
    // b_special_weapon2 (TEXT_PLACE_BEACON) / b_beacon_t: enabled when no beacon was placed within BeaconPlaceDelay;
    // selected while the beacon point is being picked.
    bool beacon_enabled = false;
    bool beacon_selected = false;
    bool movie_mode = false;         // b_camera_t (TEXT_MOVIE_MODE) selected look
    // ui3 t04: reinforcement_counter = number of pad entries (hidden at 0); r_pop_text = "used/cap" population (hidden
    // when there is no cap: land); r_pop_icon shown with it.
    uint32_t reinforce_count = 0;
    int32_t pop_used = -1, pop_cap = -1;
};

struct TacticalHudFrame {
    WorldHudContext world;          // canvas is filled by TacticalHud
    // Radar mapping: world rectangle shown by the radar (the map's playable box).
    float map_min_x = -5000, map_min_y = -5000, map_max_x = 5000, map_max_y = 5000;
    int64_t credits = -1;           // < 0: no credits text ("13,000": thousands separator [port: original screenshot])
    // [port: Gabriel, original screenshot] planet panel above the radar: Text_Planet_tactical (battle planet name),
    // Text_Tactical_Tech ("Tech: 3", TEXT_TECH), Text_Planetary_Pop ("13/20"; Text_Pop_Cap without it); empty / < 0 = blank.
    std::u32string planet_name;
    std::u32string cohesion_text;  // [port: Gabriel 2026-11-13] fleet cohesion line above the planet name ("" = none)
    int32_t tech = -1, pop = -1, pop_cap = -1;
    // FIXPASS_GALACTIC §4.2 bottom-left advisor frame (i_main_skirmish = i_tactical_controls.alo bones: Movie_tactical
    // (50, 288), b_droid_help_tactical (19, 255), Text_tactical_help (65, 309), Text_Planet_tactical (179, 239); the
    // faction frame model b_Help_Droid_{Empire,Rebel,Underworld}_ALT{0,1,2} sits at (45.7, 287.5) behind the movie and is
    // drawn by the shell drawer). Movie_tactical = an Icon, Size 200 x 200 with the bone at its bottom-left corner (capture
    // C2), showing the advisor *_LOOP Bink (a talking head, not a static image). Host feed: the image page holding the loop's current frame, only while an advisor loop
    // plays (-1 = none: the movie component stays hidden [M]), and Movies.xml Alpha (true: straight alpha, PrimAlpha).
    int32_t advisor_page = -1;
    bool advisor_alpha = true;
    uint8_t faction_alt = 1;        // index into per-faction component lists (Blank_Texture_Name: 0 rebel, 1 empire) [port]
    bool shells_loaded = false;     // shell models drawn by alamo_ui_draw: no [port] 2D placeholder frames
    double time = 0.0;              // wall seconds (tooltip delay, blink, flash)
    HudProviders providers;
    // Radar (stage 2): the dynamic picture (terrain + FOW, uploaded by the renderer as page kHudPageRadar; null =
    // backdrop only), pings, camera footprint (ground plane z).
    const RadarImage* radar_image = nullptr;
    const RadarEvents* radar_events = nullptr;
    const RadarSettings* radar_settings = nullptr;
    bool radar_footprint = true;
    // true: the host draws radar_models() (HudModelDrawer) - pings whose event type has an Event_Model_Name then get no
    // [port] dot-ring stand-in; false: every ping draws the ring.
    bool radar_event_models = false;
    float ground_z = 0.0f;
    bool radar_colorize_selected = true;          // GameConstants Radar_Colorize_Selected_Units
    Rgba8 radar_selected_color{209, 255, 209, 255};
    // Order feedback.
    std::span<const HudWaypointLine> waypoints;
    // Tactical buy buttons (Build_00..05): the selected builder (kNoObject = none: no buttons) and its options (first
    // six). Placement: the i_build_buttons.alo bone grid (Build_00..02 at y -67, Build_03..05 at y -120; x -54 / -0.5 /
    // 52, canvas units) around build_anchor_px (the builder's projected pixel, e.g. a build pad), or, with no anchor,
    // around the bottom-centre [port] point (0, -H/2 + 430) above the build queue.
    uint32_t builder = commands::kNoObject;
    std::span<const HudBuildOption> build_options;
    bool has_build_anchor = false;
    Vec2f build_anchor_px{};
    // Hard-point reticles (HUD_GAPS §1): the hard points of the hovered ship, or of every capital ship with
    // HudHardPoint::parent set - only the hovered one's are drawn (filter_hardpoints). `images` resolves the stand-alone I_Hard_Point_Reticle_* DDS textures
    // to image pages (the GUI's resolver; null = [port] corner outlines).
    std::span<const HudHardPoint> hardpoints;
    const ReticleTable* reticles = nullptr;
    const GuiImageResolver* images = nullptr;
    const MoveAcks* move_acks = nullptr;
    // Tooltips: cursor pixel for placement.
    Vec2f cursor_px{};
    // [port: Gabriel] sim paused (the pause button's selected look; a click emits pause_event(!paused), orders.hpp) and
    // the queued orders of the units to mark (sim queued_orders(unit), hud_markers.hpp add_queue_markers).
    bool paused = false;
    std::span<const HudOrderQueue> order_queues;
    HudOrderButtons order_buttons;  // c_button00..05 enable state (HUD_GAPS §5)
    HudBombingRunButton bombing_run;  // b_special_weapon in land (HUD_GAPS §9)
    HudCommandButtons command_buttons;  // ui3 t03 / t04: b_retreat, b_reinforcement, beacon, movie mode, reinforcement texts
    bool objectives_hidden = false;     // ui3 t01 [port]: the Mission Holocron key (Tab) toggles the objectives box
    // [port: Gabriel, original screenshot] Game Paused banner (pause_shell: yellow TEXT_GAME_PAUSED bar at the top
    // centre, red TEXT_BUTTON_RESUME_GAME button under it - a click emits HudAction::ResumeGame) while `paused`.
    bool pause_banner = true;
    // [port: Gabriel, original screenshot] Mission Objectives box at the bottom right (objective_back / _header_text /
    // _text / _icon components): header (TEXT_TEST_OBJECTIVE_HEADER "Mission Objectives"), a rule, then the numbered
    // objectives word-wrapped to the box. Empty = no box.
    std::span<const HudObjective> objectives;
    std::u32string objectives_header;   // "" = TEXT_TEST_OBJECTIVE_HEADER
    // Drag-select box (UI.md §1.1 step 5h "drag rectangle"; Playtest 3): while the world owns a left drag past the drag
    // threshold (InputState ButtonState::dragging), the press -> cursor rectangle in pixels (ButtonState::drag). Drawn
    // by add_drag_box over the same bounds box_select uses on release (drag_select_bounds). The corner marker of the
    // original is the POINTER_DRAG_SELECT_MODE cursor (DragSelectPointer_00), drawn by the cursor pass, not here.
    bool drag_select = false;
    DragRect drag_rect;
};

struct TacticalHudStats {
    WorldHudStats world;
    uint32_t panel_slots = 0, radar_blips = 0, hit_boxes = 0, quads = 0;
    uint32_t ability_buttons = 0, queue_entries = 0, reinforcements = 0, buttons = 0, radar_events = 0;
    uint32_t build_options = 0;                  // Build_NN buttons drawn
    uint32_t waypoint_dots = 0, reticles = 0, ack_dots = 0, queue_dots = 0, queue_markers = 0;
    uint32_t radar_icons = 0, radar_models = 0;  // blips drawn with Radar_Icon_Name; pings handed out as models
    bool advisor_movie = false;                  // Movie_tactical drew the advisor loop this frame
    bool tooltip = false;
    bool pause_banner = false;          // Game Paused banner drawn
    uint32_t objectives = 0, objective_lines = 0;
    bool drag_box = false;              // drag-select box drawn
};

class TacticalHud {
public:
    // Tables must outlive the HUD. `text` may be null (keys shown as text).
    void init(const ComponentTable& table, const TextureAtlas& atlas, FontCache& fonts, const TextDatabase* text = nullptr);
    void resize(uint32_t width, uint32_t height);
    const HudCanvas& canvas() const { return canvas_; }
    HudLayout& layout() { return layout_; }
    const HudLayout& layout() const { return layout_; }
    const ComponentTable& components() const { return *table_; }
    HudButtons& buttons() { return buttons_; }
    const HudButtons& buttons() const { return buttons_; }
    void set_sfx_hook(HudSfxHook hook) { buttons_.set_sfx_hook(std::move(hook)); }
    void set_tooltip_config(const TooltipConfig& c) { tooltip_cfg_ = c; }

    void build(const TacticalHudFrame& frame, std::span<const HudUnit> units, std::span<const HudTypeInfo> types,
               const FlyTextList* flytext = nullptr);
    const HudDrawList& draw_list() const { return dl_; }
    const std::vector<SelectionBlob>& selection_blobs() const { return blobs_; }
    const std::vector<SelectedGroup>& panel_groups() const { return groups_; }
    const TacticalHudStats& stats() const { return stats_; }
    const RadarView& radar_view() const { return radar_view_; }
    // Radar event models of the last build (pings inside the radar whose RadarMap.xml type names a model), in event order.
    const std::vector<HudModelDraw>& radar_models() const { return radar_models_; }
    const TooltipState& tooltip() const { return tooltip_; }

    // Topmost HUD component under a pixel (-1: none) - RouteHooks::hud_hit.
    int32_t hit_test(int32_t px, int32_t py) const;
    const std::vector<HudHitBox>& hit_boxes() const { return hits_; }

    // One frame of HUD input (stage 2): hover / press visuals, SFX hooks, clicks -> commands + host actions. Uses the
    // layout of the previous build() (what the player sees). `routed.world` gives the cursor for radar drags.
    void update_input(const RoutedInput& routed, bool shift, bool ctrl, double now, std::vector<commands::EventData>& out,
                      std::vector<HudAction>* actions = nullptr);
    // ui3 t02: an ability hotkey = a left click on the first ability button (selection panel order) showing
    // `ability_type`: the same command / BeginAbilityTargeting action as the click (`queued` = the queue modifier).
    // False when no button shows it or it is disabled (nothing emitted). Uses the slots of the previous build().
    bool activate_ability(uint8_t ability_type, bool queued, std::vector<commands::EventData>& out,
                          std::vector<HudAction>* actions = nullptr) const;
    // ui3 t01: c_button00..05 enabled after the previous build() (HUD_GAPS §5.1 rules, Lua overrides); false when the
    // component is missing or hidden. The order hotkeys (Attack, Move, ...) act only while their button would.
    bool order_button_live(uint32_t button) const;
    // Stage-1 entry point: left click on a selection portrait -> Select{that group's ids} (no state change).
    void on_pointer(std::span<const HudPointerEvent> events, std::vector<commands::EventData>& out) const;

private:
    void build_panel(const TacticalHudFrame& f, std::span<const HudTypeInfo> types);
    void build_abilities(const TacticalHudFrame& f);
    void build_queue(const TacticalHudFrame& f);
    void build_reinforcements(const TacticalHudFrame& f);
    void build_buy_buttons(const TacticalHudFrame& f);
    void build_buttons(const TacticalHudFrame& f);
    void build_radar(const TacticalHudFrame& f, std::span<const HudUnit> units, std::span<const HudTypeInfo> types);
    void build_skeleton(const TacticalHudFrame& f);
    void build_markers(const TacticalHudFrame& f, std::span<const HudUnit> units);
    void apply_order_buttons(const HudOrderButtons& s);
    struct AbilitySlot;
    static void ability_click(const AbilitySlot& a, const std::string& name, Button button, bool queued,
                              std::vector<commands::EventData>& out, std::vector<HudAction>* actions);
    void apply_bombing_run_button(const HudBombingRunButton& s);
    void apply_command_buttons(const HudCommandButtons& s);
    void build_pause_banner(const TacticalHudFrame& f);
    void build_objectives(const TacticalHudFrame& f);
    int32_t font_of(int32_t component, float fallback_points) const;
    void hit(int32_t component, uint32_t layer, Vec2f center, Vec2f size, uint32_t slot = 0);
    Vec2f component_size(uint32_t component, const ComponentPlacement& p) const;
    std::vector<uint32_t> selection_ids() const;

    const ComponentTable* table_ = nullptr;
    const TextureAtlas* atlas_ = nullptr;
    FontCache* fonts_ = nullptr;
    const TextDatabase* text_ = nullptr;
    HudCanvas canvas_ = HudCanvas::make(1024, 768);
    HudLayout layout_;
    WorldHud world_;
    HudButtons buttons_;
    TooltipState tooltip_;
    TooltipConfig tooltip_cfg_;
    HudDrawList dl_;
    std::vector<SelectionBlob> blobs_;
    std::vector<SelectedGroup> groups_;
    std::vector<HudHitBox> hits_;
    std::vector<int32_t> slot_component_, health_component_, shield_component_;
    std::vector<int32_t> special_button_, special_border_, queue_component_, reinforce_component_;
    struct AbilitySlot {
        int32_t component = -1;
        HudAbility ability;
        std::vector<uint32_t> users;
    };
    std::vector<AbilitySlot> ability_slots_;
    struct QueueSlot {
        int32_t component = -1;
        HudQueueEntry entry;
    };
    std::vector<QueueSlot> queue_slots_;
    struct ReinforceSlot {
        int32_t component = -1;
        HudReinforcement entry;
    };
    std::vector<ReinforceSlot> reinforce_slots_;
    struct BuySlot {
        int32_t component = -1;
        HudBuildOption option;
    };
    std::vector<BuySlot> buy_slots_;
    std::array<int32_t, 6> buy_component_{-1, -1, -1, -1, -1, -1};
    uint32_t builder_ = 0;
    std::vector<uint8_t> dynamic_;  // per component: drawn by a panel (not by build_buttons)
    int32_t radar_ = -1, radar_blip_ = -1, credits_ = -1, shell_ = -1, retreat_ = -1;
    int32_t order_button_[6] = {-1, -1, -1, -1, -1, -1};  // c_button00..05
    int32_t special_weapon_ = -1;                         // b_special_weapon (land: the bombing-run button)
    int32_t reinforce_button_ = -1, beacon_button_ = -1, beacon_t_ = -1, camera_t_ = -1;  // ui3 t03
    int32_t reinforce_counter_ = -1, r_pop_icon_ = -1, r_pop_text_ = -1;                 // ui3 t04
    int32_t planet_text_ = -1, tech_text_ = -1, pop_text_ = -1, movie_ = -1;
    int32_t pause_ = -1, pause_g_ = -1;  // [port: Gabriel] b_play_pause_t / b_play_pause
    bool paused_ = false;
    int32_t pause_text_ = -1, resume_ = -1;  // pause_shell text_attack / attack_button
    int32_t obj_back_ = -1, obj_header_ = -1, obj_text_ = -1, obj_icon_ = -1;
    RadarView radar_view_;
    std::vector<HudModelDraw> radar_models_;
    bool radar_drag_ = false;
    float radar_ground_z_ = 0.0f;
    uint8_t faction_alt_ = 1;
    TacticalHudStats stats_;
};

// Drag-select box (TacticalHudFrame::drag_select). Colours / outline width are not in docs/re: [inferred] from the
// base-game screenshot (Playtest 3, 41.jpg): a faint grey translucent fill and a 1-pixel light-grey outline, both
// inside drag_select_bounds(rect) (the outline covers the press / cursor pixel rows and columns).
inline constexpr Rgba8 kDragBoxFill{128, 128, 128, 64};       // [inferred]
inline constexpr Rgba8 kDragBoxOutline{176, 176, 176, 255};   // [inferred]
inline constexpr uint32_t kDragBoxLayer = 9;                  // [inferred] over the shells / panels, under tooltips (10)
// Appends the fill and the four outline quads (Alpha list, white page) to `dl` on `layer`; returns the quads added
// (0 for an empty canvas). Fill first, then top, bottom, left, right edges; edges collapse into the fill when the box
// is narrower than two pixels.
uint32_t add_drag_box(HudDrawList& dl, const HudCanvas& canvas, const DragRect& rect, uint32_t layer = kDragBoxLayer);

// Decimal with thousands separators ("13,000", "-1,250") [port: original screenshots].
std::u32string hud_thousands(int64_t v);
// Reinforce event (COMMANDS.md §2 id 12) for a reinforcement unit dropped at a world point.
commands::EventData reinforce_event(uint32_t unit, Vec3 point);
// SetUnitAbilityMode (id 43) toggling `ability` on `units` (max 127).
commands::EventData ability_mode_event(std::span<const uint32_t> units, uint8_t ability, bool on);
// SetAbilityAutofire (id 58).
commands::EventData ability_autofire_event(std::span<const uint32_t> units, uint8_t ability, bool on);

}  // namespace alamo::ui

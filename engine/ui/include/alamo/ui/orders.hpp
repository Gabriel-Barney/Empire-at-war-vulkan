// P1 click -> order translation. Selection is synced sim state (COMMANDS.md §2.4, §6.2): Select,
// SelectAll and ControlGroup are SCHEDULED events, so the UI never changes a selection itself - it emits
// commands::EventData and the host submits them (CommandPipeline::submit with CommandSource::Player).
// The translator keeps a local mirror of the selection it last requested (for shift-add and attack
// lists); the host should overwrite it with set_selection() from the sim's synced selection after the
// commands execute (SelectAll / control-group recall can only be known there).
//
// Mapping (tactical; [port] where the docs give no UI rule). Right button = the order button of the default mouse scheme
// (IDC_ALTERNATE_MOUSE_CONTROLS, orders on the left button, is not implemented). Right-button orders follow
// P1_DOUBLE_CLICK_MOVE.md: the order is resolved at the RELEASE from the object under the cursor there; a press that is a
// Win32 double click (ButtonState::double_press) acts at the press and its release is swallowed.
//   left click on a selectable object      Select{id}            (shift: toggle id in the mirror -> Select{list})
//   left click on nothing (no object hit)  Select{} (P1_CAMERA_INPUT.md §5; [port] not with Shift)
//   left double click on selectable        Select{every selectable of the same type on screen} (needs type_of)
//   left drag (>= drag threshold)          Select{box}           (shift: union) - own player tree, ascending id
//   right click on hostile                 Attack{target, attackers = selection, flag1 = Ctrl}
//   right click on any other object        MoveToObject{target, movers = {} (= Sel)}
//   right click on ground, land            MoveToPosition{x, y, z (q1..q3), flags}
//   right drag on ground, land             MoveToPositionFacing{anchor point, angle, flags, units = {} (= Sel)}, or
//                                          Facing{angle, units = {}} when the press was on a selected object
//   right click on ground, space           MoveToRay{a = camera ray origin, b = point on z = ground_z, flags}
//   right drag on ground, space            MoveToRayFacing{anchor ray origin, anchor plane point, angle, flags}
//     "drag" = max(|dx|, |dy|) from the press > InputConfig::drag_threshold_px (MinimumDragDistance 4); angle = degrees
//     of the anchor -> release ground direction (atan2, 0 = +x, the sim's dir_from_deg).
//   right double click on ground           a second, full MoveToPosition (land) / MoveToRay (space) at the press point with
//                                          flags | 0x80, never a facing variant; no voice; double-click ack (MoveAck)
//   right double click on hostile          Attack{..., flag0 ("double") = 1}
//   right double click on anything else    nothing (the first click's order stands)
//   SelectAll / Stop action                SelectAll / StopMovement
//   ControlGroupN (+Ctrl assign, +Ctrl+Shift add)  ControlGroup{N, assign, add}
// Move flags (move_flag; spec §2.1): 0x04 always; Ctrl or the attack-move toggle 0x01; Ctrl+Alt or the guard toggle 0x02
//   (wins over 0x01); 0x80 double click; 0x08 from the second order of an Alt (alone) / waypoint-toggle run (the first
//   starts a fresh path; any order without Alt ends the run) and [port: Gabriel] whenever the queue modifier is held.
//   A move order is queued (UiOrder::queued) exactly when it carries 0x08.
// Galactic: left click on an own fleet -> Select{fleet}; on nothing -> Select{} + take_planet_deselect(); left double
//   click on a planet -> zoom request; right click on a planet with fleets selected -> MoveObjectToObject{fleet, planet}
//   per fleet (asc. id). No double-click move in galactic.
// Lists are capped at 127 ids (Select is list7), lowest ids kept.
#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/tactical/tree_set.hpp"
#include "alamo/ui/input.hpp"
#include "alamo/ui/picking.hpp"

namespace alamo::commands {
class CommandPipeline;
}

namespace alamo::ui {

enum class UiMode : uint8_t { Land, Space, Galactic };

// [port: Gabriel] pause / resume as a command (docs/DECISIONS.md "Camera and controls": pausing goes through the command
// stream so replays and lockstep stay valid). The original has no such event (ResumeGame 51 is the MP battle vote), so it
// is the port extension event 63 with subtype kPortPauseSubtype and one data byte (1 pause, 0 resume). The host submits
// it like any order; the sim host stops ticking from the scheduled tick on.
inline constexpr uint16_t kPortPauseSubtype = 0x5041;  // "PA"
commands::EventData pause_event(bool pause);
// True when `e` is a pause event (pause = its state).
bool read_pause_event(const commands::EventData& e, bool& pause);

// Move-order flag byte = the movement flag word (P1_DOUBLE_CLICK_MOVE.md §3.3; Execute copies it unchanged).
namespace move_flag {
inline constexpr uint8_t kAttackMove = 0x01, kGuardMove = 0x02, kPlayerOrder = 0x04, kWaypoint = 0x08, kDoubleClick = 0x80;
}
// One-shot HUD toggles (attack-move / guard / waypoint buttons): armed until the next move order is sent.
enum class MoveToggle : uint8_t { AttackMove, GuardMove, Waypoint };

// Move acknowledgement (spec §2.3, `0x418b0`), one per ground move order the translator emits; the host spawns it.
enum class AckEffect : uint8_t { Move, DoubleClickMove, AttackMove, GuardMove };
enum class AckRadar : uint8_t { Click, DoubleClick, AttackClick };
struct MoveAck {
    Vec3 point{};             // destination (land: ground point / drag anchor; space: plane point)
    bool space = false;       // scale GUI_Move_Acknowledge_Scale_Space (5.0), else _Land (1.0)
    bool double_click = false;
    bool attack = false;      // flags 0x01
    bool guard = false;       // flags 0x02
    // Particle by priority attack > guard > double > normal.
    AckEffect effect() const;
    // Double (not attack / guard): first kill the nearest live GUI_Move_Command_Ack_Effect object silently (no radius).
    bool kill_nearest_move_ack() const { return effect() == AckEffect::DoubleClickMove; }
    // attack -> Default_Attack_Click; double -> remove the playing Default_Click radar event, play Default_Double_Click.
    AckRadar radar() const;
    bool remove_click_radar() const { return radar() == AckRadar::DoubleClick; }
};
// GameConstants tag of the particle / radar event name (GUI_Double_Click_Move_Command_Ack_Effect, ...).
std::string_view ack_effect_constant(AckEffect e);
std::string_view ack_radar_constant(AckRadar r);

// [port: Gabriel] queueable orders: an order with its queued flag (the queue modifier, default Shift, held when it was
// issued; for moves: the 0x08 flag). queued = false replaces the unit's order queue, true appends to it. The flag travels
// next to the EventData until the command stream carries it (the host copies it into the command).
struct UiOrder {
    commands::EventData event;
    bool queued = false;
    bool voice = true;          // false: do not play the unit response (the double-click order, spec §1.4)
    bool double_click = false;  // issued by a double click (move 0x80 / Attack flag0)
};

struct OrderContext {
    UiMode mode = UiMode::Space;
    int32_t local_player = 0;
    View view;
    // tactical
    const tactical::TreeSet* trees = nullptr;
    tactical::QueryFilter pick_filter = tactical::kSkipDead;
    // refixf t08: the world pick's model / sphere tests (HUD_GAPS §2.2, pick_world); null: tree boxes only (pick).
    const WorldPickTests* pick_tests = nullptr;
    float ground_z = 0.0f;  // move-order plane (space: z = 0)
    // Optional land terrain height (x, y) -> z, refined by fixed-point iteration from ground_z [port].
    std::function<float(float, float)> ground_height;
    // galactic
    std::span<const PickSphere> galactic_items;
    // Optional overrides. Defaults: selectable = owner == local_player (TreeSet membership / sphere owner);
    // hostile = owner >= 0 && owner != local_player; type_of = none (double click acts as a click).
    std::function<bool(uint32_t)> selectable;
    std::function<bool(uint32_t)> hostile;
    std::function<uint32_t(uint32_t)> type_of;
};

class OrderTranslator {
public:
    static constexpr size_t kMaxSelect = 127;

    // Reads this frame's clicks/drags/actions; appends orders to pending().
    void update(const InputState& in, const Bindings& keys, const OrderContext& ctx);

    const std::vector<commands::EventData>& pending() const { return pending_; }
    // [port: Gabriel] queued flag per pending() entry (1 = queued: Attack / MoveToObject / MoveToPosition / MoveToRay
    // issued with Bindings::queue_held; selection and keyboard orders are never queued).
    const std::vector<uint8_t>& pending_queued() const { return pending_queued_; }
    // false per pending() entry: no unit voice for that order (the double-click move / attack).
    const std::vector<uint8_t>& pending_voice() const { return pending_voice_; }
    std::vector<commands::EventData> take() {
        pending_queued_.clear();
        pending_voice_.clear();
        pending_double_.clear();
        return std::exchange(pending_, {});
    }
    // Acknowledgements of this frame's move orders, in order (cleared by the call).
    std::vector<MoveAck> take_move_acks() { return std::exchange(acks_, {}); }
    void arm_move_toggle(MoveToggle t);
    bool waypoint_mode() const { return waypoint_mode_; }
    // Orders with their queued flags (clears pending()).
    std::vector<UiOrder> take_orders();
    // Submits and clears pending(); returns how many were accepted.
    size_t submit(commands::CommandPipeline& pipe, uint8_t player);

    const std::vector<uint32_t>& selection() const { return sel_; }
    void set_selection(std::vector<uint32_t> ids);  // sorted + deduplicated

    // Galactic local UI request: planet id to zoom onto (double click), consumed by the host.
    std::optional<uint32_t> take_planet_zoom() { return std::exchange(planet_zoom_, std::nullopt); }
    // Galactic left click on nothing: besides Select{}, the host clears its planet selection (when not zoomed in).
    bool take_planet_deselect() { return std::exchange(planet_deselect_, false); }
    // false: the keyboard orders above (SelectAll / Stop / ControlGroupN) are left to HotkeyDispatcher (hotkeys.hpp,
    // the original 153-slot table: Ctrl+n assign, Shift+n add); mouse orders are unchanged. Default true.
    void set_keyboard_orders(bool on) { keyboard_orders_ = on; }
    // Local camera request (UI.md §4.4): the same control group recalled twice within 1 s (InputFrame::time) - the
    // host looks the camera at the group (TacticalCameraController::jump_to(focus_point(member positions))).
    std::optional<uint8_t> take_camera_jump() { return std::exchange(camera_jump_, std::nullopt); }
    static constexpr double kDoubleRecallSeconds = 1.0;

private:
    void emit_select(std::vector<uint32_t> ids);
    void push_order(commands::EventData e, bool queued, bool voice = true, bool dbl = false);
    void sync_meta();
    uint8_t move_flags(const InputState& in, bool dbl);
    void right_release(const InputState& in, const OrderContext& ctx);
    void right_double(const InputState& in, const OrderContext& ctx);
    void ground_move(const InputState& in, const OrderContext& ctx, int32_t px, int32_t py, bool dbl);
    void tactical(const InputState& in, const OrderContext& ctx);
    void galactic(const InputState& in, const OrderContext& ctx);
    bool ground_point(const OrderContext& ctx, int32_t px, int32_t py, Vec3& out) const;
    bool is_selectable(const OrderContext& ctx, uint32_t id) const;
    bool is_hostile(const OrderContext& ctx, uint32_t id) const;

    std::vector<commands::EventData> pending_;
    std::vector<uint8_t> pending_queued_;
    std::vector<uint8_t> pending_voice_;
    std::vector<uint8_t> pending_double_;
    std::vector<MoveAck> acks_;
    bool waypoint_mode_ = false;  // GameModeManager +0xb4: an Alt run has started
    bool toggle_attack_ = false, toggle_guard_ = false, toggle_waypoint_ = false;
    bool queue_ = false;  // queue modifier held during this update
    std::vector<uint32_t> sel_;
    std::optional<uint32_t> planet_zoom_;
    bool planet_deselect_ = false;
    tactical::QueryContext qctx_;
    bool keyboard_orders_ = true;
    std::optional<uint8_t> camera_jump_;
    int16_t last_recall_group_ = -1;
    double last_recall_time_ = -1e30;
};

}  // namespace alamo::ui

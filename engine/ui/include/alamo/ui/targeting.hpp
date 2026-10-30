// P2: consumers of the tactical HUD's host actions (docs/re/UI.md §4.1 "modal targeting controller"):
//   BeginAbilityTargeting -> TargetingController (ability mode: the next world click picks the target)
//   BeginReinforcement    -> TargetingController (drop mode: the next world click picks the landing point)
//   RadarCameraJump       -> apply_radar_jump (the tactical camera looks at the clicked radar point)
// Presentation only: the picked target leaves as a command (COMMANDS.md §2: 40 TacticalSpecialAbility on an object,
// 57 TacticalSpecialAbilityWithDummyTarget on a ground point - the host / sim spawns the dummy target -, 43
// SetUnitAbilityMode with the point for the position-via-43 types, 12 Reinforce at the drop point).
//
// Ability families (ABILITIES_HARDPOINTS.md §2.9 table [H for the lists]): ability_target_kind(). [port] HUNT / LURE /
// AVOID_DANGER (optional target) and TARGETED_INVULNERABILITY take an object; JET_PACK a ground point (57).
//
// Host wiring: RouteHooks::targeting_active = [&]{ return targeting.active(); } (the router then gives the targeting
// mode the presses: hud_input.hpp), then per frame after TacticalHud::update_input: targeting.begin(action) for every
// action, targeting.update(world input, OrderContext, orders) BEFORE OrderTranslator::update; when update() returns
// true the frame's clicks belonged to the targeting mode and OrderTranslator must not see them (skip its update or
// feed it an input without those clicks). Queued: the action's `queued` (Shift at the HUD click) or the queue modifier
// held at the target click [port: Gabriel].
// alamo_game (battle_input): BeginAbilityTargeting -> begin(); the router's presses of a running pick are NOT in the
// world's InputState, so the host keeps a second InputState fed with the raw frame for update(); Esc during a pick only
// cancels it (no pause menu); the pause menu / a modal dialog cancels it; ability_pick_cursor() picks the pointer.
// BeginReinforcement stays with the host's own drop (apps/game reinforce_unit_), which the smoke goldens pin.
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/ui/camera.hpp"
#include "alamo/ui/mouse_cursor.hpp"
#include "alamo/ui/orders.hpp"
#include "alamo/ui/tactical_hud.hpp"

namespace alamo::ui {

enum class TargetKind : uint8_t {
    None = 0,       // mode toggle (command 43 on / off)
    Object = 1,     // command 40 on the picked object
    Point = 2,      // command 57 at the picked ground point (dummy target)
    PointMode = 3,  // command 43 (on) carrying the picked ground point
};
TargetKind ability_target_kind(uint8_t ability_type);

// The cursor of a running ability pick (CursorContext::targeting): an object pick shows AbilityEnemy, or AbilityFriendly
// over a friendly hit (the host sets CursorContext::target_valid = something under the cursor); ground picks show
// Barrage (5) / AbilityBlast (70) for those types, else AbilitySpace or AbilityTerrain by the battle kind [port: the
// original's per-ability cursor table is not specified].
CursorContext::Targeting ability_pick_cursor(TargetKind kind, uint8_t ability_type, bool space, bool friendly_hit);

// Command 57 (users list7, ability, ground point, player); `target` = the dummy object (kNoObject: the sim spawns it).
commands::EventData ability_point_event(std::span<const uint32_t> users, uint8_t ability, Vec3 point, int32_t player,
                                        uint32_t target = commands::kNoObject);
// Command 43 turning `ability` on for `users` with a target point (position-via-43 types).
commands::EventData ability_mode_point_event(std::span<const uint32_t> users, uint8_t ability, Vec3 point);

class TargetingController {
public:
    enum class Mode : uint8_t { None, Ability, Reinforcement };

    // Starts the mode of a BeginAbilityTargeting / BeginReinforcement action (replacing a running one); false for any
    // other action kind (left for the host).
    bool begin(const HudAction& a);
    void cancel() { mode_ = Mode::None; }
    bool active() const { return mode_ != Mode::None; }
    Mode mode() const { return mode_; }
    uint8_t ability_type() const { return ability_; }
    TargetKind target_kind() const { return kind_; }
    uint32_t unit() const { return unit_; }
    const std::vector<uint32_t>& users() const { return users_; }

    // One frame of world input (see the header). Left click: ability -> 40 on the object under the cursor (Object; no
    // object = keep waiting), 57 / 43 at the ground point (Point / PointMode); reinforcement -> Reinforce at the ground
    // point. Right click or Esc cancels. The mode ends after a command. Returns true when a click / Esc of this frame
    // was used by the targeting mode.
    bool update(const InputState& in, const Bindings& keys, const OrderContext& ctx, std::vector<UiOrder>& out);

    uint64_t issued() const { return issued_; }
    uint64_t cancelled() const { return cancelled_; }

private:
    Mode mode_ = Mode::None;
    uint8_t ability_ = 0;
    TargetKind kind_ = TargetKind::None;
    uint32_t unit_ = 0;
    bool queued_ = false;
    std::vector<uint32_t> users_;
    tactical::QueryContext qctx_;
    uint64_t issued_ = 0, cancelled_ = 0;
};

// RadarCameraJump: looks the tactical camera at (a.x, a.y) (terrain-snapped, clamped; TacticalCameraController::jump_to)
// and stops its scroll drift. False for other actions.
bool apply_radar_jump(const HudAction& a, TacticalCameraController& camera);

}  // namespace alamo::ui

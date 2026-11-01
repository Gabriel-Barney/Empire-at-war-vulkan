#include "alamo/ui/targeting.hpp"

#include <algorithm>

namespace alamo::ui {

TargetKind ability_target_kind(uint8_t t) {
    // ABILITIES_HARDPOINTS.md §2.9 "Argument" table.
    switch (t) {
        // target object (command 40)
        case 13: case 15: case 16: case 18: case 20: case 21: case 22: case 27: case 30: case 31: case 34: case 37:
        case 41: case 45: case 51: case 53: case 55: case 58: case 59: case 64: case 66:
        // special: 6 / 10 / 12 HUNT / LURE / AVOID_DANGER (optional target), 19 TARGETED_INVULNERABILITY (object) [port]
        case 6: case 10: case 12: case 19:
            return TargetKind::Object;
        // target position (command 57, Dummy_Barrage_Target); 17 JET_PACK (position) [port: 57]
        case 14: case 23: case 24: case 25: case 35: case 36: case 38: case 44: case 46: case 49: case 57: case 61:
        case 72: case 17:
            return TargetKind::Point;
        // position via command 43
        case 5: case 26: case 28: case 42:
            return TargetKind::PointMode;
        default:
            return TargetKind::None;
    }
}

CursorContext::Targeting ability_pick_cursor(TargetKind kind, uint8_t ability_type, bool space, bool friendly_hit) {
    using T = CursorContext::Targeting;
    if (kind == TargetKind::Object) return friendly_hit ? T::AbilityFriendly : T::AbilityEnemy;
    if (ability_type == 70) return T::AbilityBlast;  // BLAST
    if (ability_type == 5) return T::Barrage;        // BARRAGE
    return space ? T::AbilitySpace : T::AbilityTerrain;
}

namespace {
std::vector<uint32_t> capped_users(std::span<const uint32_t> users) {
    std::vector<uint32_t> v(users.begin(), users.end());
    if (v.size() > 127) v.resize(127);  // list7
    return v;
}

bool ground_point(const OrderContext& ctx, int32_t px, int32_t py, Vec3& out) {
    const tactical::Ray r = pixel_ray(ctx.view, px, py);
    if (!ray_plane_z(r, ctx.ground_z, out)) return false;
    if (ctx.ground_height) {
        for (int i = 0; i < 4; ++i) {  // as OrderTranslator [port]
            Vec3 p;
            if (!ray_plane_z(r, ctx.ground_height(out.x, out.y), p)) break;
            out = p;
        }
    }
    return true;
}
}  // namespace

commands::EventData ability_point_event(std::span<const uint32_t> users, uint8_t ability, Vec3 p, int32_t player,
                                        uint32_t target) {
    commands::EventData e(commands::EventType::TacticalSpecialAbilityWithDummyTarget);
    e.set_ids("users", capped_users(users)).set("ability", ability).set("target", target);
    e.set_f("x", p.x).set_f("y", p.y).set_f("z", p.z).set("player", static_cast<uint32_t>(std::max(player, 0)));
    return e;
}

commands::EventData ability_mode_point_event(std::span<const uint32_t> users, uint8_t ability, Vec3 p) {
    commands::EventData e = ability_mode_event(users, ability, true);
    e.set_f("x", p.x).set_f("y", p.y).set_f("z", p.z);
    return e;
}

bool TargetingController::begin(const HudAction& a) {
    if (a.kind == HudAction::Kind::BeginAbilityTargeting) {
        mode_ = Mode::Ability;
        ability_ = a.ability_type;
        kind_ = a.value > 0 && a.value <= 3 ? static_cast<TargetKind>(a.value) : ability_target_kind(a.ability_type);
        if (kind_ == TargetKind::None) kind_ = TargetKind::Object;  // a targeted button without a family [port]
        users_ = a.ids;
        unit_ = 0;
        queued_ = a.queued;
        return true;
    }
    if (a.kind == HudAction::Kind::BeginReinforcement) {
        mode_ = Mode::Reinforcement;
        unit_ = a.unit;
        ability_ = 0;
        kind_ = TargetKind::Point;
        users_.clear();
        queued_ = false;
        return true;
    }
    return false;
}

bool TargetingController::update(const InputState& in, const Bindings& keys, const OrderContext& ctx, std::vector<UiOrder>& out) {
    if (mode_ == Mode::None) return false;
    const ButtonState& lb = in.button(Button::Left);
    const ButtonState& rb = in.button(Button::Right);
    if (rb.click || in.key_pressed(vk::kEscape)) {
        mode_ = Mode::None;
        ++cancelled_;
        return true;
    }
    // the presses / releases of the coming click belong to the mode too
    if (!lb.click) return lb.pressed || lb.released || rb.pressed || rb.released;
    const bool queued = queued_ || keys.queue_held(in);
    Vec3 p{};
    if (mode_ == Mode::Reinforcement) {
        if (ground_point(ctx, in.mouse_x(), in.mouse_y(), p)) {
            out.push_back({reinforce_event(unit_, p), false});
            ++issued_;
        }
        mode_ = Mode::None;  // a click outside the ground plane ends the drop as well
        return true;
    }
    switch (kind_) {
        case TargetKind::Object: {
            if (!ctx.trees) return true;
            const auto hit = pick_world(ctx.trees->all(), ctx.view, pixel_ray(ctx.view, in.mouse_x(), in.mouse_y()), ctx.pick_filter, qctx_, ctx.pick_tests);
            if (!hit) return true;  // keep waiting for an object
            commands::EventData e = commands::ev::ability(capped_users(users_), ability_, hit->id);
            out.push_back({std::move(e), queued});
            break;
        }
        case TargetKind::Point:
        case TargetKind::PointMode: {
            if (!ground_point(ctx, in.mouse_x(), in.mouse_y(), p)) return true;
            if (kind_ == TargetKind::Point) out.push_back({ability_point_event(users_, ability_, p, ctx.local_player), queued});
            else out.push_back({ability_mode_point_event(users_, ability_, p), queued});
            break;
        }
        case TargetKind::None: break;
    }
    ++issued_;
    mode_ = Mode::None;
    return true;
}

bool apply_radar_jump(const HudAction& a, TacticalCameraController& camera) {
    if (a.kind != HudAction::Kind::RadarCameraJump) return false;
    camera.jump_to({a.x, a.y, camera.look_at().z});
    camera.stop_scroll();
    return true;
}

}  // namespace alamo::ui

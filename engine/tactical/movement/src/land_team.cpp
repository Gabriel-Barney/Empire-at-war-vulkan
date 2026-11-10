// s78n t03: land squad containers (TACTICAL_COMBAT §5.4.4 item 2, MOVEMENT §3.7, FIGHTER_FLYING §7). The container is a
// unit of the system (UnitDesc::team_container) that walks its own path; its members steer to their slots around it
// (LandTeamInfantry 0x62d620). See MovementSystem::set_land_team in movement_system.hpp.
#include <algorithm>
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/tactical/movement/movement_system.hpp"

namespace alamo::tactical::movement {

namespace {

constexpr float kRecoveryDistance = 4.0f;  // 0x140a4431c (FIGHTER_FLYING §7 item 9, MOVEMENT §3.7)
constexpr uint32_t kRecoveryBlockedFrames = 10;  // [port] = the walk's kReplanAfterBlocked

// [port] The member's move must pass its movement class (the thin cell walk of the Walk locomotor). A member standing on
// a blocked cell may leave it (as the walk's blocked-start escape).
bool steer_move_ok(const PassabilityGrid& grid, V2 a, V2 b, uint32_t mask) {
    if (!grid.passable_world(a, mask)) return true;
    return walk_cells(a, b, [&](int32_t cx, int32_t cy) { return grid.passable_cell(cx, cy, mask); });
}

// [port] Wall slide: the full step, else its x part, else its y part. False: the member stays.
bool steer_commit(const PassabilityGrid& grid, LandMotion& m, V2 to, uint32_t mask) {
    if (steer_move_ok(grid, m.pos, to, mask)) {
        m.pos = to;
        return true;
    }
    const V2 tx{to.x, m.pos.y}, ty{m.pos.x, to.y};
    if (tx.x != m.pos.x && steer_move_ok(grid, m.pos, tx, mask)) {
        m.pos = tx;
        return true;
    }
    if (ty.y != m.pos.y && steer_move_ok(grid, m.pos, ty, mask)) {
        m.pos = ty;
        return true;
    }
    return false;
}

}  // namespace

bool MovementSystem::set_land_team(ObjectId container, std::span<const ObjectId> members, std::span<const V2> slots) {
    if (cfg_.mode != MoveMode::Land || members.size() != slots.size()) return false;
    const Unit* c = unit_ptr(container);
    if (!c || !c->d.team_container) return false;
    for (const ObjectId id : members) {
        const Unit* u = unit_ptr(id);
        if (!u || u->d.team_container || u->d.locomotor != LocomotorKind::Default || u->d.uses_multiple_locomotors) return false;
    }
    for (Unit& u : units_)
        if (u.anchor == container) u.anchor = -1;  // a re-call replaces the member list
    for (size_t i = 0; i < members.size(); ++i) {
        Unit& u = *unit_ptr(members[i]);
        u.anchor = container;
        u.tslot = slots[i];
        u.tindex = static_cast<uint32_t>(i);
        u.recovering = false;
        u.steer_blocked = 0;
        // The member's own order ends: its container holds the squad's (the formation unit).
        u.has_goal = false;
        u.need_path = false;
        u.arrived = false;
        u.failed = false;
        u.pend = 0;
        if (u.start_frame == kStartPending) u.start_frame = 0;
        u.adopted = false;
        u.in_formation = false;
        u.formation_speed = 3.0e38f;
        u.m.path.clear();
        u.m.next = 0;
        u.m.speed = 0.0f;
        u.m.state = LocoState::WalkStopped;
        end_push(u);
        arrival_reset(u);
    }
    return true;
}

bool MovementSystem::team_slot(ObjectId member, ObjectId& container, V2& slot) const {
    container = -1;
    const Unit* u = unit_ptr(member);
    if (!u || u->anchor < 0) return false;
    container = u->anchor;
    slot = u->tslot;
    return true;
}

std::vector<std::pair<ObjectId, V2>> MovementSystem::land_team_slots(ObjectId container) const {
    std::vector<std::pair<uint32_t, std::pair<ObjectId, V2>>> v;
    for (const Unit& u : units_)
        if (u.anchor == container) v.push_back({u.tindex, {u.d.id, u.tslot}});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first || (a.first == b.first && a.second.first < b.second.first); });
    std::vector<std::pair<ObjectId, V2>> out;
    out.reserve(v.size());
    for (const auto& e : v) out.push_back(e.second);
    return out;
}

ObjectId MovementSystem::order_id(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    if (!u || u->anchor < 0 || !unit_ptr(u->anchor)) return id;
    return u->anchor;
}

// One member frame (FIGHTER_FLYING §7, MOVEMENT §3.7). Reads only `k` (the container's state at the start of the tick)
// and writes only `u`, so it runs in the parallel walk.
void MovementSystem::steer_member(Unit& u, const TeamSnap& k) const {
    const float vmax = loco_max_speed(u);
    const float accel = u.lp.accel > 0.0f ? u.lp.accel : vmax;
    const float decel = u.lp.decel > 0.0f ? u.lp.decel : vmax;
    const V2 cs = dir_from_deg(k.yaw);  // (c, s): step 1 normalises (cos, sin) - already unit length
    const float c = cs.x, s = cs.y;
    const V2 off{u.tslot.x + u.jiggle.x, u.tslot.y + u.jiggle.y};
    if (!k.has_goal) {
        // The team is idle: recovery (0x29 / 0x2A) when the member is farther than 4.0 from its slot [port: the recovery
        // states' steering is untraced - turn in place, then a straight walk to the slot point, then a turn in place to
        // the team's facing, so a later team move starts with the member facing along it: a member's max speed is the team
        // speed (MOVEMENT §3.4), so a member that falls behind its slot while the team cruises cannot catch up].
        const V2 target{k.pos.x + c * off.x - s * off.y, k.pos.y + s * off.x + c * off.y};
        const V2 to = target - u.m.pos;
        const float dist = length(to);
        if (!u.recovering && dist > kRecoveryDistance) {
            u.recovering = true;
            u.steer_blocked = 0;
        }
        if (!u.recovering) {
            u.m.speed = 0.0f;
            u.m.state = LocoState::WalkStopped;
            return;
        }
        if (!(dist > 0.0f)) {  // on the slot point: face the team's facing
            u.m.speed = 0.0f;
            if (u.lp.turn_rate > 0.0f && wrap_deg(k.yaw - u.m.yaw) != 0.0f) {
                u.m.yaw = turn_toward(u.m.yaw, k.yaw, u.lp.turn_rate);
                u.m.state = LocoState::WalkStartTurn;
                return;
            }
            u.recovering = false;
            u.m.state = LocoState::WalkStopped;
            return;
        }
        const float desired = heading_deg(to);
        const float err = std::fabs(wrap_deg(desired - u.m.yaw));
        if (err > u.lp.turn_rate && err > u.lp.close_enough_angle) {  // 0x29 Recovery Turning
            u.m.yaw = turn_toward(u.m.yaw, desired, u.lp.turn_rate);
            u.m.speed = 0.0f;
            u.m.state = LocoState::WalkStartTurn;
            return;
        }
        // 0x2A Recovery Moving
        u.m.speed = std::min(vmax, u.m.speed + accel);
        u.m.yaw = turn_toward(u.m.yaw, desired, u.lp.turn_rate);
        if (!(u.m.speed > 0.0f)) {
            u.m.state = LocoState::WalkStopped;
            return;
        }
        const bool reach = dist <= u.m.speed;
        const V2 np = reach ? target : u.m.pos + normalize_retail(to) * u.m.speed;
        if (!steer_commit(grid_, u.m, np, u.lp.mask)) {
            u.m.speed = 0.0f;
            if (++u.steer_blocked >= kRecoveryBlockedFrames) {
                u.recovering = false;
                u.m.state = LocoState::WalkStopped;
            }
            return;
        }
        u.steer_blocked = 0;
        if (reach && u.m.pos == target) {
            u.m.speed = 0.0f;
            u.recovering = u.lp.turn_rate > 0.0f && wrap_deg(k.yaw - u.m.yaw) != 0.0f;  // the facing turn follows
            u.m.state = LocoState::WalkStopped;
        } else {
            u.m.state = LocoState::WalkMoving;
        }
        return;
    }
    u.recovering = false;
    u.steer_blocked = 0;
    // Step 2-3: the errors in the team frame.
    const V2 d = u.m.pos - k.pos;
    const float along = off.x - (c * d.x + s * d.y);
    const float side = off.y - (-s * d.x + c * d.y);
    // Step 4: speed by the along error.
    const float vk = k.speed;
    float v = vk;
    if (along > 1.0f) {
        const float t = std::clamp(along / 10.0f, 0.0f, 1.0f);
        v = (vmax - vk) * t + vk;
    } else if (along < -1.0f) {
        const float t = std::clamp(-along / 10.0f, 0.0f, 1.0f);
        v = (0.0f - vk) * t * t + vk;
    }
    // The member state 0x10 (copied from the container, MOVEMENT §3.7): v = 0. [port] The port's Walk turns in place in
    // WalkStartTurn (retail 0x0F -> 0x10); both count.
    if (k.state == LocoState::WalkStartTurn || k.state == LocoState::WalkTurning) v = 0.0f;
    // Step 5 (0x62d520): accelerate / decelerate toward v [port: the step stops at v], clamped to [0, vmax].
    float sp = u.m.speed;
    if (sp < v) sp = std::min(v, sp + accel);
    else sp = std::max(v, sp - decel);
    sp = std::clamp(sp, 0.0f, std::max(vmax, 0.0f));
    u.m.speed = sp;
    // Step 6: heading = the team facing pulled toward the side error (w <= 0.5).
    const float w = std::clamp(std::fabs(side) / 10.0f, 0.0f, 0.5f);
    const V2 perp = side > 0.0f ? V2{-s, c} : V2{s, -c};
    const V2 dir{c + w * (perp.x - c), (perp.y - s) * w + s};
    // Step 7: turn by at most the turn rate.
    u.m.yaw = turn_toward(u.m.yaw, heading_deg(dir), u.lp.turn_rate);
    if (!(sp > 0.0f)) {
        u.m.state = LocoState::WalkStopped;
        return;
    }
    // Step 8: move along the new yaw.
    const V2 f = dir_from_deg(u.m.yaw);
    if (!steer_commit(grid_, u.m, V2{f.x * sp + u.m.pos.x, f.y * sp + u.m.pos.y}, u.lp.mask)) {
        u.m.speed = 0.0f;
        u.m.state = LocoState::WalkStopped;
        return;
    }
    u.m.state = LocoState::WalkMoving;
}

}  // namespace alamo::tactical::movement

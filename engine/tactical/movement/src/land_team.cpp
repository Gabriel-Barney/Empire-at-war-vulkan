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

void MovementSystem::attach_member(Unit& u, ObjectId container, V2 slot, uint32_t tindex) {
    u.anchor = container;
    u.tslot = slot;
    u.tindex = tindex;
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
    std::erase_if(parked_, [container](const LandTeamSlot& p) { return p.container == container; });
    for (size_t i = 0; i < members.size(); ++i) attach_member(*unit_ptr(members[i]), container, slots[i], static_cast<uint32_t>(i));
    team_ix_rebuild();
    return true;
}

void MovementSystem::team_ix_rebuild() {
    team_ix_.clear();
    for (const Unit& u : units_)
        if (u.anchor >= 0) team_ix_.push_back({u.anchor, u.tindex, u.d.id, u.tslot});
    std::sort(team_ix_.begin(), team_ix_.end(), [](const LandTeamSlot& a, const LandTeamSlot& b) {
        if (a.container != b.container) return a.container < b.container;
        if (a.tindex != b.tindex) return a.tindex < b.tindex;
        return a.member < b.member;
    });
}

bool MovementSystem::team_slot(ObjectId member, ObjectId& container, V2& slot) const {
    container = -1;
    const Unit* u = unit_ptr(member);
    if (!u || u->anchor < 0) return false;
    container = u->anchor;
    slot = u->tslot;
    return true;
}

std::span<const MovementSystem::LandTeamSlot> MovementSystem::land_team(ObjectId container) const {
    const auto lo = std::lower_bound(team_ix_.begin(), team_ix_.end(), container,
                                     [](const LandTeamSlot& e, ObjectId c) { return e.container < c; });
    auto hi = lo;
    while (hi != team_ix_.end() && hi->container == container) ++hi;
    return {lo, hi};
}

std::vector<std::pair<ObjectId, V2>> MovementSystem::land_team_slots(ObjectId container) const {
    std::vector<std::pair<ObjectId, V2>> out;
    for (const LandTeamSlot& e : land_team(container)) out.push_back({e.member, e.slot});
    return out;
}

bool MovementSystem::is_land_team_container(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u && u->d.team_container;
}

void MovementSystem::park_record(const Unit& u) {
    const ObjectId member = u.d.id;
    const auto it = std::lower_bound(parked_.begin(), parked_.end(), member,
                                     [](const LandTeamSlot& e, ObjectId m) { return e.member < m; });
    const LandTeamSlot p{u.anchor, u.tindex, member, u.tslot};
    if (it != parked_.end() && it->member == member) *it = p;
    else parked_.insert(it, p);
}

bool MovementSystem::park_team_member(ObjectId member) {
    const Unit* u = unit_ptr(member);
    if (!u || u->anchor < 0) return false;
    park_record(*u);
    return remove_impl(member);
}

bool MovementSystem::suspend_team_member(ObjectId member) {
    Unit* u = unit_ptr(member);
    if (!u || u->anchor < 0) return false;
    park_record(*u);
    u->anchor = -1;
    u->recovering = false;
    u->steer_blocked = 0;
    u->m.speed = 0.0f;
    u->m.state = LocoState::WalkStopped;
    team_ix_rebuild();
    return true;
}

bool MovementSystem::team_member_parked(ObjectId member) const {
    const auto it = std::lower_bound(parked_.begin(), parked_.end(), member,
                                     [](const LandTeamSlot& e, ObjectId m) { return e.member < m; });
    return it != parked_.end() && it->member == member;
}

bool MovementSystem::rejoin_land_team(ObjectId member) {
    const auto it = std::lower_bound(parked_.begin(), parked_.end(), member,
                                     [](const LandTeamSlot& e, ObjectId m) { return e.member < m; });
    if (it == parked_.end() || it->member != member) return false;
    const LandTeamSlot p = *it;
    parked_.erase(it);
    Unit* u = unit_ptr(member);
    const Unit* c = unit_ptr(p.container);
    if (cfg_.mode != MoveMode::Land || !u || !c || !c->d.team_container || u->d.team_container ||
        u->d.locomotor != LocomotorKind::Default || u->d.uses_multiple_locomotors)
        return false;
    attach_member(*u, p.container, p.slot, p.tindex);
    team_ix_rebuild();
    return true;
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
    // Step 1: (c, s) = normalise(cos yaw, sin yaw) by sqrtf and 1 / len (FIGHTER_FLYING §7 item 1).
    const V2 cs = dir_from_deg(k.yaw);
    const float inv = 1.0f / std::sqrt(cs.x * cs.x + cs.y * cs.y);
    const float c = cs.x * inv, s = cs.y * inv;
    // Step 7 / MOVEMENT §3.7: [port] the turn rate x Fleeing_Infantry_Speed_Bonus while the member's (or its team's) path
    // is kind 7 (0x39a5b0) is not applied - the port has no fleeing path kind (no flee order), so the factor is always 1.
    const float rate = u.lp.turn_rate;
    const V2 off{u.tslot.x + u.jiggle.x, u.tslot.y + u.jiggle.y};
    if (!k.has_goal) {
        // The team is idle: recovery (0x29 / 0x2A) when the member is farther than 4.0 from its slot [port: the recovery
        // states' steering is untraced - turn in place, then a straight walk to the slot point at the member's own max
        // speed, then a turn in place to the team's facing, so a later team move starts with the member facing along it.
        // While the team moves, step 4 lets a member behind its slot run up to its own max speed (lp.max_speed, not the
        // team speed), so it catches up when it is faster than the team].
        const V2 target{k.pos.x + c * off.x - s * off.y, k.pos.y + s * off.x + c * off.y};
        const V2 to = target - u.m.pos;
        const float dist = length(to);
        // s78n review [port]: a recovery that gave up (kRecoveryBlockedFrames blocked frames: steer_blocked stays at the
        // limit) is not restarted until the container has a goal again or is teleported (both reset steer_blocked), so an
        // unreachable slot does not keep the member reporting has_goal (kUnitMoving) every frame.
        if (!u.recovering && dist > kRecoveryDistance && u.steer_blocked < kRecoveryBlockedFrames) {
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
            if (rate > 0.0f && wrap_deg(k.yaw - u.m.yaw) != 0.0f) {
                u.m.yaw = turn_toward(u.m.yaw, k.yaw, rate);
                u.m.state = LocoState::WalkStartTurn;
                return;
            }
            u.recovering = false;
            u.m.state = LocoState::WalkStopped;
            return;
        }
        const float desired = heading_deg(to);
        const float err = std::fabs(wrap_deg(desired - u.m.yaw));
        if (err > rate && err > u.lp.close_enough_angle) {  // 0x29 Recovery Turning
            u.m.yaw = turn_toward(u.m.yaw, desired, rate);
            u.m.speed = 0.0f;
            u.m.state = LocoState::WalkStartTurn;
            return;
        }
        // 0x2A Recovery Moving
        u.m.speed = std::min(vmax, u.m.speed + accel);
        u.m.yaw = turn_toward(u.m.yaw, desired, rate);
        if (!(u.m.speed > 0.0f)) {
            u.m.state = LocoState::WalkStopped;
            return;
        }
        const bool reach = dist <= u.m.speed;
        const V2 np = reach ? target : u.m.pos + normalize_retail(to) * u.m.speed;
        if (!steer_commit(grid_, u.m, np, u.lp.mask)) {
            u.m.speed = 0.0f;
            if (++u.steer_blocked >= kRecoveryBlockedFrames) {  // give up (kept until the team moves, see above)
                u.steer_blocked = kRecoveryBlockedFrames;
                u.recovering = false;
                u.m.state = LocoState::WalkStopped;
            }
            return;
        }
        u.steer_blocked = 0;
        if (reach && u.m.pos == target) {
            u.m.speed = 0.0f;
            u.recovering = rate > 0.0f && wrap_deg(k.yaw - u.m.yaw) != 0.0f;  // the facing turn follows
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
    u.m.yaw = turn_toward(u.m.yaw, heading_deg(dir), rate);
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

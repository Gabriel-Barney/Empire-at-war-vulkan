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
    u.recovery = 0;
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
        if (u.anchor >= 0) team_ix_.push_back({u.anchor, u.tindex, u.d.id, u.tslot, V2{}});
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
    // s78n t14 (FIGHTER_FLYING §7.4): the offset from the container is kept for the garrison leave.
    const Unit* k = unit_ptr(u.anchor);
    const LandTeamSlot p{u.anchor, u.tindex, member, u.tslot, k ? u.m.pos - k->m.pos : V2{}};
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
    u->recovery = 0;
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

// ---- s78n t14 (FIGHTER_FLYING §7.2): slot layout and re-lay ----

float team_radical_inverse(uint32_t index, uint32_t base) {
    float f = 0.0f;
    const float b = static_cast<float>(base);
    float bk = b;
    for (uint32_t i = index; i != 0; i /= base) {
        f += static_cast<float>(i % base) / bk;
        bk *= b;
    }
    return f;
}

namespace {

// ceil(sqrt(n)) by the float rule of 0x5abc00: (uint)sqrtf(n), + 1 unless its square is n.
uint32_t team_ceil_sqrt(uint32_t n) {
    uint32_t k = static_cast<uint32_t>(std::sqrt(static_cast<float>(n)));
    if (k * k != n) ++k;
    return k;
}

}  // namespace

bool MovementSystem::lay_out_rows(const Unit& k, V2 dir, std::vector<std::pair<Unit*, V2>>& out) {
    // Rows 0x5ac970: rows of ceil(sqrt(n)) members, front-most first, each row from the most negative side to the most
    // positive, every member with its own half-extents and FormationSpacing x 20 x f; then the rows are centred.
    out.clear();
    std::vector<Unit*> mem;
    for (const LandTeamSlot& e : land_team(k.d.id))
        if (Unit* m = unit_ptr(e.member)) mem.push_back(m);
    const uint32_t n = static_cast<uint32_t>(mem.size());
    const uint32_t kk = team_ceil_sqrt(n);
    if (kk == 0) return false;
    const float f = k.spread ? cfg_.constants.spread_out_spacing_coefficient : 1.0f;
    const auto proj = [&k](const Unit* m, V2 v) { return dot(m->m.pos - k.m.pos, v); };
    // MSVC std::sort on <= 32 entries is an insertion sort: ties keep their order (stable_sort).
    std::vector<uint32_t> a(n);
    for (uint32_t i = 0; i < n; ++i) a[i] = i;
    std::stable_sort(a.begin(), a.end(), [&](uint32_t x, uint32_t y) { return proj(mem[x], dir) > proj(mem[y], dir); });
    const V2 neg_side{dir.y, -dir.x};
    const auto gap = [&](uint32_t m) { return mem[m]->d.formation_spacing * 20.0f * f; };
    std::vector<V2> slot(n);
    float along = 0.0f, hx0 = 0.0f, dd = 0.0f;
    uint32_t pos = 0;
    while (pos < n) {
        const uint32_t cnt = std::min(kk, n - pos);
        std::vector<uint32_t> row(a.begin() + pos, a.begin() + pos + cnt);
        const bool first_row = pos == 0;
        pos += cnt;
        std::stable_sort(row.begin(), row.end(),
                         [&](uint32_t x, uint32_t y) { return proj(mem[x], neg_side) > proj(mem[y], neg_side); });
        float hx = 0.0f, g = 0.0f, w = 0.0f;
        for (const uint32_t m : row) {
            hx = std::max(hx, mem[m]->team_hx);
            g = std::max(g, gap(m));
        }
        if (cnt == 1) {
            w = 2.0f * mem[row[0]]->team_hy;
        } else {
            for (uint32_t j = 0; j < cnt; ++j) {
                const float hy = mem[row[j]]->team_hy, gm = gap(row[j]);
                w += (j == 0 || j + 1 == cnt) ? gm / 2.0f + 2.0f * hy : 2.0f * hy + gm;
            }
        }
        if (first_row) hx0 = hx;
        else along -= g / 2.0f + hx;
        if (pos == n) dd = hx0 - (along - hx);
        float side = -w / 2.0f;
        for (uint32_t j = 0; j < cnt; ++j) {
            const uint32_t m = row[j];
            const float hy = mem[m]->team_hy, gm = gap(m);
            if (cnt == 1) side = 0.0f;
            else if (j == 0) side += hy;
            else side += gm / 2.0f + hy;
            slot[m] = {along, side};
            side += gm / 2.0f + hy;
        }
        along -= g / 2.0f + hx;
    }
    for (uint32_t i = 0; i < n; ++i) {  // index order: rows centred, row 0 at the front (+along)
        slot[i].x += dd / 2.0f - hx0;
        out.push_back({mem[i], slot[i]});
    }
    return true;
}

bool MovementSystem::lay_out_dartboard(const Unit& k, V2 dir, uint32_t frame, std::vector<std::pair<Unit*, V2>>& out) {
    // Dartboard 0x5ac2a0: a Halton scatter (bases 2 / 3) over L x Wd, points closer than 5.0 + r rejected, <= 1000 tries
    // (else Rows); points and members matched front to back, then side to side within groups.
    out.clear();
    std::vector<Unit*> mem;
    for (const LandTeamSlot& e : land_team(k.d.id))
        if (Unit* m = unit_ptr(e.member)) mem.push_back(m);
    const uint32_t n = static_cast<uint32_t>(mem.size());
    if (n == 0) return false;
    const Unit& fm = *mem[0];  // F = the type of member 0
    const uint32_t kk = team_ceil_sqrt(n);
    const uint32_t mm = n / kk + (n % kk != 0 ? 1u : 0u);
    const float hx = fm.team_thx, hy = fm.team_thy;
    float r = std::sqrt(hx * hx + hy * hy);
    const float g = fm.d.formation_spacing;
    float len_l = static_cast<float>(kk - 1) * g * 20.0f + static_cast<float>(kk) * hx;
    float len_w = static_cast<float>(mm - 1) * g * 20.0f + static_cast<float>(mm) * hy;
    if (k.spread) {
        const float c = cfg_.constants.spread_out_spacing_coefficient;
        len_l *= c;
        len_w *= c;
        r *= c;
    }
    // Both generators start at (int32) container id x NOW, wrapping uint32 (port: the system's id x the tick frame).
    uint32_t i2 = static_cast<uint32_t>(k.d.id) * frame, i3 = i2;
    std::vector<V2> pts;
    pts.reserve(n);
    for (uint32_t t = 0; t < 1000 && pts.size() < n; ++t) {
        const float x = (team_radical_inverse(i2++, 2) - 0.5f) * len_l;
        const float y = (team_radical_inverse(i3++, 3) - 0.5f) * len_w;
        bool ok = true;
        for (const V2& p : pts)
            if (std::sqrt((y - p.y) * (y - p.y) + (x - p.x) * (x - p.x)) < 5.0f + r) {
                ok = false;
                break;
            }
        if (ok) pts.push_back({x, y});
    }
    if (pts.size() < n) return lay_out_rows(k, dir, out);  // too few points: Rows with the same dir
    const auto proj = [&k](const Unit* m, V2 v) { return dot(m->m.pos - k.m.pos, v); };
    std::stable_sort(pts.begin(), pts.end(), [](V2 p, V2 q) { return p.x > q.x; });
    std::vector<Unit*> a = mem;
    std::stable_sort(a.begin(), a.end(), [&](const Unit* x, const Unit* y) { return proj(x, dir) > proj(y, dir); });
    const V2 side_v{-dir.y, dir.x};
    const float band = len_w / static_cast<float>(mm);
    uint32_t i = 0;
    do {
        uint32_t j = i;
        while (j < n && pts[i].x - pts[j].x <= band) ++j;
        std::stable_sort(a.begin() + i, a.begin() + j,
                         [&](const Unit* x, const Unit* y) { return proj(x, side_v) > proj(y, side_v); });
        std::stable_sort(pts.begin() + i, pts.begin() + j, [](V2 p, V2 q) { return p.y > q.y; });
        i = j + 1;  // retail quirk (copied): index j joins no group sort
    } while (i < n);
    for (uint32_t q = 0; q < n; ++q) out.push_back({a[q], pts[q]});
    return true;
}

bool MovementSystem::lay_out_land_team(ObjectId container, V2 dir, uint32_t frame) {
    // 0x5ac120(K, teleport 0, dir).
    Unit* k = unit_ptr(container);
    if (cfg_.mode != MoveMode::Land || !k || !k->d.team_container) return false;
    const float len = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (len > 0.0f) {
        const float inv = 1.0f / len;
        dir = {dir.x * inv, dir.y * inv};
    }
    if (dot(dir, k->lay_dir) > 0.99f && k->laid_out) return false;  // within ~8.11 degrees: the slots stay
    std::vector<std::pair<Unit*, V2>> out;
    bool ok = false;
    if (k->d.container_arrangement == 0) ok = lay_out_rows(*k, dir, out);
    else if (k->d.container_arrangement == 1) ok = lay_out_dartboard(*k, dir, frame, out);
    if (!ok) return false;
    for (const auto& [m, sl] : out) m->tslot = sl;
    k->lay_dir = dir;
    k->laid_out = true;
    team_ix_rebuild();  // land_team() / squad_joint_slots read the slot copies there
    return true;
}

void MovementSystem::team_relay_pass(uint32_t frame) {
    // FIGHTER_FLYING §7.2 callers 1-4 (this tick's requests, in order), then caller 2: a container whose flag is clear lays
    // out with its facing (its first tick, after a SPREAD_OUT toggle).
    for (Unit& u : units_) {
        if (!u.d.team_container) continue;
        const std::vector<V2> req = std::move(u.relay);
        u.relay.clear();
        for (const V2& d : req) (void)lay_out_land_team(u.d.id, d, frame);
        if (!u.laid_out) (void)lay_out_land_team(u.d.id, dir_from_deg(u.m.yaw), frame);
    }
}

bool MovementSystem::land_team_layout(ObjectId container, bool& laid_out, V2& dir) const {
    const Unit* k = unit_ptr(container);
    if (!k || !k->d.team_container) return false;
    laid_out = k->laid_out;
    dir = k->lay_dir;
    return true;
}

bool MovementSystem::set_team_member_shape(ObjectId member, float hx, float hy, float type_hx, float type_hy) {
    Unit* u = unit_ptr(member);
    if (!u) return false;
    u->team_hx = hx;
    u->team_hy = hy;
    u->team_thx = type_hx;
    u->team_thy = type_hy;
    return true;
}

bool MovementSystem::set_team_spread_out(ObjectId container, bool on) {
    Unit* k = unit_ptr(container);
    if (!k || !k->d.team_container) return false;
    if (k->spread != on) {
        k->spread = on;
        k->laid_out = false;  // 0x3a3500 clears +0x2a4: the next tick re-lays with the container's facing
    }
    return true;
}

bool MovementSystem::set_team_target(ObjectId container, bool has_target) {
    Unit* k = unit_ptr(container);
    if (!k || !k->d.team_container) return false;
    k->team_target = has_target;
    return true;
}

uint32_t MovementSystem::team_start_delay(ObjectId member) const {
    const Unit* u = unit_ptr(member);
    return u && u->anchor >= 0 ? u->start_delay : 0u;
}

bool MovementSystem::parked_offset(ObjectId member, V2& off) const {
    const auto it = std::lower_bound(parked_.begin(), parked_.end(), member,
                                     [](const LandTeamSlot& e, ObjectId m) { return e.member < m; });
    if (it == parked_.end() || it->member != member) return false;
    off = it->off;
    return true;
}

// One member frame (FIGHTER_FLYING §7 / §7.3, MOVEMENT §3.7). Reads only `k` (the container's state at the start of the
// tick) and writes only `u`, so it runs in the parallel walk.
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
    // s78n t14 (FIGHTER_FLYING §7.3): the container's state copied to the member. Port Walk states: WalkStartTurn /
    // WalkTurning = retail 0x10 (the port's Walk turns in place in 0x0F), WalkMoving = 0x13, WalkEndMove = 0x14; WalkStopped
    // / None = 0 / 0x16 (the recovery decision); any other = 0 (idle).
    const bool turning = k.state == LocoState::WalkStartTurn || k.state == LocoState::WalkTurning;
    const bool steering = turning || k.state == LocoState::WalkMoving || k.state == LocoState::WalkEndMove;
    if (!steering) {
        // Recovery decision 0x62bbf0 (item 6): only while the container is laid out and has no target. s78n review [port]:
        // a recovery that gave up (kRecoveryBlockedFrames blocked frames: steer_blocked stays at the limit) is not
        // restarted until the container moves or is teleported (both reset steer_blocked).
        const bool decide = (k.state == LocoState::WalkStopped || k.state == LocoState::None) && k.laid_out &&
                            !k.has_target && u.steer_blocked < kRecoveryBlockedFrames;
        const V2 target{k.pos.x + c * off.x - s * off.y, k.pos.y + s * off.x + c * off.y};
        const V2 to = target - u.m.pos;
        const float dist = length(to);
        const float desired = heading_deg(to);
        uint8_t rs = 0;
        if (decide) {
            if (dist > kRecoveryDistance) {
                rs = std::fabs(wrap_deg(desired - u.m.yaw)) > 0.5f * rate ? uint8_t{0x29} : uint8_t{0x2A};
            } else if (dist == kRecoveryDistance) {
                rs = 0x2A;
            } else if (u.recovery == 0x2A) {  // closing in: keep walking while the next step gets closer
                const V2 fw = dir_from_deg(u.m.yaw);
                const V2 nx{u.m.pos.x + fw.x * u.m.speed, u.m.pos.y + fw.y * u.m.speed};
                if (length(nx - target) <= dist && decel <= u.m.speed) rs = 0x2A;
            }
        }
        u.recovery = rs;
        if (rs == 0) {  // state 0: speed 0, no movement
            u.m.speed = 0.0f;
            u.m.state = LocoState::WalkStopped;
            return;
        }
        u.m.yaw = turn_toward(u.m.yaw, desired, rate);
        if (rs == 0x29) {  // 0x29 Recovery Turning
            u.m.speed = 0.0f;
            u.m.state = LocoState::WalkStartTurn;
            return;
        }
        // 0x2A Recovery Moving (0x62d190): speed step toward vmax, or toward 0 inside v^2 / (2 decel); move along the yaw.
        const float v0 = u.m.speed;
        const float want = dist < v0 * v0 / (2.0f * decel) ? 0.0f : vmax;
        float sp = v0;
        if (sp < want) sp = std::min(want, sp + accel);
        else sp = std::max(want, sp - decel);
        sp = std::clamp(sp, 0.0f, std::max(vmax, 0.0f));
        u.m.speed = sp;
        if (!(sp > 0.0f)) {
            u.m.state = LocoState::WalkStopped;
            return;
        }
        const V2 fw = dir_from_deg(u.m.yaw);
        if (!steer_commit(grid_, u.m, V2{fw.x * sp + u.m.pos.x, fw.y * sp + u.m.pos.y}, u.lp.mask)) {
            u.m.speed = 0.0f;
            if (++u.steer_blocked >= kRecoveryBlockedFrames) {  // give up (kept until the team moves, see above)
                u.steer_blocked = kRecoveryBlockedFrames;
                u.recovery = 0;
                u.m.state = LocoState::WalkStopped;
            }
            return;
        }
        u.steer_blocked = 0;
        u.m.state = LocoState::WalkMoving;
        return;
    }
    u.recovery = 0;
    u.steer_blocked = 0;
    // §7.3 items 3-4: while the container turns in place or moves, a member with a start delay counts it down and stands.
    if (k.state != LocoState::WalkEndMove && u.start_delay > 0) {
        --u.start_delay;
        u.m.state = LocoState::WalkStopped;
        return;
    }
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
    // The member state 0x10 (copied from the container): v = 0 - the member only turns toward its heading.
    if (turning) v = 0.0f;
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
    const V2 fw = dir_from_deg(u.m.yaw);
    if (!steer_commit(grid_, u.m, V2{fw.x * sp + u.m.pos.x, fw.y * sp + u.m.pos.y}, u.lp.mask)) {
        u.m.speed = 0.0f;
        u.m.state = LocoState::WalkStopped;
        return;
    }
    u.m.state = LocoState::WalkMoving;
}

}  // namespace alamo::tactical::movement

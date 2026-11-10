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

// s78n t14 part B: the passability probe 0x5a4c10 of the steering vector for a movement class: every cell the segment
// touches passes the mask (the start cell included).
bool steer_probe_ok(const PassabilityGrid& grid, V2 a, V2 b, uint32_t mask) {
    return walk_cells(a, b, [&](int32_t cx, int32_t cy) { return grid.passable_cell(cx, cy, mask); });
}

}  // namespace

void MovementSystem::attach_member(Unit& u, ObjectId container, V2 slot, uint32_t tindex) {
    u.anchor = container;
    u.tslot = slot;
    u.tindex = tindex;
    u.recovery = 0;
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

bool MovementSystem::set_collision_exempt(ObjectId container, ObjectId other) {
    Unit* k = unit_ptr(container);
    if (!k || !k->d.team_container) return false;
    k->exempt = other;
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

// ---- s78n t14 part B (FIGHTER_FLYING §7.1 / §7.5): fleeing and the member collision steering vector 0x509810 ----

bool MovementSystem::team_fleeing(ObjectId container) const {
    const Unit* k = unit_ptr(container);
    return k && k->d.team_container && k->pushed && k->push_flee;
}

float MovementSystem::team_hard_radius(const Unit& u) const {
    // 0x397780 (hard radius): sqrt(hx^2 + hy^2) of the unit's half-extents - a steering member's shape from the host
    // (set_team_member_shape); [port] otherwise the Custom_Hard extents, else the soft radius (the push's footprint).
    if (u.team_hx > 0.0f || u.team_hy > 0.0f) return std::sqrt(u.team_hx * u.team_hx + u.team_hy * u.team_hy);
    if (u.d.hard_x > 0.0f || u.d.hard_y > 0.0f) return std::sqrt(u.d.hard_x * u.d.hard_x + u.d.hard_y * u.d.hard_y);
    return u.d.radius;
}

void MovementSystem::team_steer_recs(std::vector<SteerRec>& out) const {
    // The id-keyed collision hash (+0x80) gives a squad container's active collision objects in list order (the port's
    // collision list is insertion order). vf +0x30 weights: MovingVsMoving 1, MovingVsStopped 2, StoppedVsStopped 3. vf
    // +0x38 for a member (never participant +0x7c): MovingVsStopped always; StoppedVsStopped never (state 1 and obj = +0x7c
    // only); MovingVsMoving only in the Repulse (3) / 11 states, which the port's MovingVsMoving stand-in does not model, so
    // [port] never. [port] The port's resolutions end a MovingVsStopped object at once (Resolved: dropped after the driver
    // pass) where retail keeps it through its Path Around / Existing Path states [M], so the members read the container's
    // live objects plus those this tick's driver resolved (coll_done_), all in creation (seq) order.
    out.clear();
    std::vector<const CollisionObject*> all;
    all.reserve(coll_.list().size() + coll_done_.size());
    for (const CollisionObject& c : coll_.list()) all.push_back(&c);
    for (const CollisionObject& c : coll_done_) all.push_back(&c);
    std::sort(all.begin(), all.end(), [](const CollisionObject* x, const CollisionObject* y) { return x->seq < y->seq; });
    for (const CollisionObject* cp : all) {
        const CollisionObject& c = *cp;
        const int32_t ia = find(c.a), ib = find(c.b);
        if (ia < 0 || ib < 0) continue;
        const float w = c.kind == CollisionKind::MovingVsMoving ? 1.0f : c.kind == CollisionKind::MovingVsStopped ? 2.0f : 3.0f;
        const bool vec = c.kind == CollisionKind::MovingVsStopped;
        if (units_[static_cast<size_t>(ia)].d.team_container) out.push_back({c.a, w, vec, static_cast<uint32_t>(ib)});
        if (units_[static_cast<size_t>(ib)].d.team_container) out.push_back({c.b, w, vec, static_cast<uint32_t>(ia)});
    }
    std::stable_sort(out.begin(), out.end(), [](const SteerRec& x, const SteerRec& y) { return x.container < y.container; });
}

V2 MovementSystem::member_side_steps(const Unit& a, V2 pa, V2 dir, uint32_t other, std::span<const V2> pos) const {
    // 0x5c6d40(a, dir, O, 1.0) without its speed ratio: O a land team container -> the sum over O's members m != a of
    // 0x5c71a0(a, m) in team index order; else 0x5c71a0(a, O). 0x5c71a0(a, b): 0 when a and b share a container; t =
    // clamp(1 - |a - b| / ((r_a + r_b) x 1.3), 0, 1); t = 0 -> 0; b on a's right (cross(dir, b - a) < 0) and the left side
    // step free -> t (-dir.y, dir.x); else the right step free -> t (dir.y, -dir.x); else 0. [port] the side-step probe
    // (0x5c66a0) is the segment a -> a + (r_a + r_b) x 1.3 x normalise(side) for a's movement class; the footprint-overlap t
    // of related pairs (0x4261d0 / 0x41cf50 [M]) is not modelled.
    const float ra = team_hard_radius(a);
    const auto side_step = [&](const Unit& b, V2 pb) -> V2 {
        if (b.anchor >= 0 && b.anchor == a.anchor) return {};
        const float reach = (ra + team_hard_radius(b)) * 1.3f;
        if (!(reach > 0.0f)) return {};
        const float t = std::clamp(1.0f - length(pa - pb) / reach, 0.0f, 1.0f);
        if (!(t > 0.0f)) return {};
        const V2 left{-dir.y, dir.x}, right{dir.y, -dir.x};
        const auto free = [&](V2 side) { return steer_probe_ok(grid_, pa, pa + normalize_retail(side) * reach, a.lp.mask); };
        if (cross(dir, pb - pa) < 0.0f && free(left)) return left * t;
        if (free(right)) return right * t;
        return {};
    };
    const Unit& o = units_[other];
    if (!o.d.team_container) return side_step(o, pos[other]);
    V2 sum{};
    for (const LandTeamSlot& e : land_team(o.d.id)) {
        if (e.member == a.d.id) continue;
        const int32_t mi = find(e.member);
        if (mi < 0) continue;
        sum = sum + side_step(units_[static_cast<size_t>(mi)], pos[static_cast<size_t>(mi)]);
    }
    return sum;
}

V2 MovementSystem::member_avoid(const Unit& a, V2 pa, float speed, float vmax, V2 dir, std::span<const SteerRec> recs,
                                std::span<const V2> pos) const {
    // 0x509810(system, out, dir, obj) for a team member (key = its container): see team_member_avoid in the header.
    if (recs.empty() || a.d.avoidance_disabled) return {};  // no key entry; 0x5c6ca0 needs Avoidance_Disabled false
    const float ratio = vmax > 0.0f ? std::clamp(speed / vmax, 0.0f, 1.0f) : 0.0f;  // 0x5c6d40's speed scale
    std::vector<V2> kept;
    kept.reserve(recs.size());
    size_t best_rec = 0;
    bool have = false;
    V2 best{};
    float best_w = 0.0f;
    for (size_t i = 0; i < recs.size(); ++i) {
        const SteerRec& r = recs[i];
        V2 v{};
        if (r.vec) v = member_side_steps(a, pa, dir, r.other, pos) * ratio;
        // kept only if P -> P + 25 normalise(v) and P -> P + 25 (v + dir) pass a's movement class (0x5a4c10)
        if (!steer_probe_ok(grid_, pa, pa + normalize_retail(v) * 25.0f, a.lp.mask) ||
            !steer_probe_ok(grid_, pa, pa + (v + dir) * 25.0f, a.lp.mask))
            continue;
        kept.push_back(v);
        if (!have || r.w > best_w || (r.w == best_w && length_sq(v) > length_sq(best))) {
            have = true;
            best_rec = i;  // the RECORD loop index (retail compares it with the kept-list index below)
            best = v;
            best_w = r.w;
        }
    }
    if (!have) return {};
    V2 out = best;
    for (size_t j = 0; j < kept.size(); ++j) {
        if (j == best_rec) continue;  // retail quirk (copied): after a dropped record this skips the wrong entry
        if (dot(best, kept[j]) >= -0.5f) out = out + kept[j];
    }
    // + 0x5c6d40(obj, dir, its container, 1.0): every term is 0x5c71a0 of two members of one container = 0.
    return out;
}

V2 MovementSystem::team_member_avoid(ObjectId member, V2 dir) const {
    const Unit* u = unit_ptr(member);
    if (!u || u->anchor < 0 || !unit_ptr(u->anchor)) return {};
    std::vector<SteerRec> recs;
    team_steer_recs(recs);
    const auto lo = std::lower_bound(recs.begin(), recs.end(), u->anchor,
                                     [](const SteerRec& e, ObjectId c) { return e.container < c; });
    auto hi = lo;
    while (hi != recs.end() && hi->container == u->anchor) ++hi;
    std::vector<V2> pos;
    pos.reserve(units_.size());
    for (const Unit& o : units_) pos.push_back(o.m.pos);
    const float flee = team_fleeing(u->anchor) ? cfg_.constants.fleeing_infantry_speed_bonus : 1.0f;
    return member_avoid(*u, u->m.pos, u->m.speed, loco_max_speed(*u) * flee, dir,
                        std::span<const SteerRec>(recs).subspan(static_cast<size_t>(lo - recs.begin()),
                                                                static_cast<size_t>(hi - lo)),
                        pos);
}

// s78n t16 (FIGHTER_FLYING §7.7): the inputs of the squad containers' speed step 0x5aaf70, serial, before the parallel walk
// (start-of-tick positions and slots). Minima only, so the member order does not matter (retail: team index order).
void MovementSystem::team_speed_steps() {
    const float mult = mode_mult();
    for (Unit& k : units_) {
        k.team_accel = 0.0f;
        k.straggle = 1.0f;
        if (!k.d.team_container) continue;
        const std::span<const LandTeamSlot> team = land_team(k.d.id);
        if (team.empty()) continue;  // 0x5ab540: no member -> the full-speed fallback; 0x404450 fails -> no cap
        // The members' smallest raw Max_Speed (0x4049e0), as team_caps takes it.
        float raw = 3.0e38f;
        for (const LandTeamSlot& e : team) {
            const int32_t mi = find(e.member);
            if (mi >= 0) raw = std::min(raw, std::max(units_[static_cast<size_t>(mi)].d.max_speed, 0.0f));
        }
        // Item 1, 0x5ab540: min over the members of 0x373080(member type, K) = OverrideDeceleration x E / Max_Speed, E =
        // 0x370f00(member type, K) = min(member Max_Speed, the members' minimum) x K's chain (mode multiplier, container
        // factor; the fleeing factor is applied by the walk); the bare OverrideDeceleration when Max_Speed is 0; a type
        // without the tag gives E (MOVEMENT §3.4: no override = the effective max speed), so an untagged member does not
        // lower the minimum. The first value is taken, then the minimum; not > 0 -> K's effective max speed (team_accel 0).
        // Item 2, 0x404450: the joint box starts at 0 and holds every slot x +/- the member's half-extent.
        bool first = true;
        float a = 0.0f, lx = 0.0f, ux = 0.0f;
        for (const LandTeamSlot& e : team) {
            const int32_t mi = find(e.member);
            if (mi < 0) continue;
            const Unit& m = units_[static_cast<size_t>(mi)];
            const float ms = m.d.max_speed;
            const float eff = std::min(ms, raw) * mult * k.team_factor;
            float v = m.d.decel;
            if (!(v > 0.0f)) v = eff;
            else if (ms != 0.0f) v = v * eff / ms;
            a = first ? v : std::min(a, v);
            first = false;
            lx = std::min(lx, m.tslot.x - m.team_hx);
            ux = std::max(ux, m.tslot.x + m.team_hx);
        }
        if (first) continue;
        if (a > 0.0f) k.team_accel = a;
        // Item 2, 0x5ab640: a member more than HX + 2 behind the container along its facing caps the target speed at
        // (1 - clamp(((-HX - 2) - p) / 10, 0, 1)) x the team speed.
        const float edge = -((ux - lx) * 0.5f + k.d.max_jiggle_distance) - 2.0f;
        const V2 f = dir_from_deg(k.m.yaw);
        float fac = 1.0f;
        for (const LandTeamSlot& e : team) {
            const int32_t mi = find(e.member);
            if (mi < 0) continue;
            const Unit& m = units_[static_cast<size_t>(mi)];
            const float p = f.x * (m.m.pos.x - k.m.pos.x) + f.y * (m.m.pos.y - k.m.pos.y);
            if (!(p < edge)) continue;  // every member in the list counts (0x5ab640 tests nothing else)
            fac = std::min(fac, 1.0f - std::clamp((edge - p) / 10.0f, 0.0f, 1.0f));
        }
        k.straggle = fac;
    }
}

// One member frame (FIGHTER_FLYING §7 / §7.3, MOVEMENT §3.7). Reads only `k` (the container's state at the start of the
// tick) and writes only `u`, so it runs in the parallel walk.
void MovementSystem::steer_member(Unit& u, const TeamSnap& k, std::span<const SteerRec> recs, std::span<const V2> pos0) const {
    // s78n t14 part B (FIGHTER_FLYING §7.1): while the container's current path is a crush-flee push (0x39a5b0 reads the
    // container's path for a member) Fleeing_Infantry_Speed_Bonus multiplies the member's max speed 0x370f00 (the catch-up
    // and recovery speeds), so its acceleration / deceleration (0x372fe0 / 0x373080: x effective / Max_Speed), and its turn
    // rate 0x62c3d0 (steering, recovery turn / move, the 0.5 x rate recovery threshold). x 1 is exact otherwise.
    // FIGHTER_FLYING §7.3 "Recovery in full" item 1: the whole member service is skipped while the member's locomotor is
    // suspended (pack +0x260; the port's speed factor 0: a stun).
    if (!(u.speed_factor > 0.0f)) return;
    const float flee = k.fleeing ? cfg_.constants.fleeing_infantry_speed_bonus : 1.0f;
    const float vmax = loco_max_speed(u) * flee;
    const float accel = u.lp.accel > 0.0f ? u.lp.accel * flee : vmax;
    const float decel = u.lp.decel > 0.0f ? u.lp.decel * flee : vmax;
    // Step 1: (c, s) = normalise(cos yaw, sin yaw) by sqrtf and 1 / len (FIGHTER_FLYING §7 item 1).
    const V2 cs = dir_from_deg(k.yaw);
    const float inv = 1.0f / std::sqrt(cs.x * cs.x + cs.y * cs.y);
    const float c = cs.x * inv, s = cs.y * inv;
    // Step 7: the member's own turn rate (0x62c3d0 = 0x372560 x the fleeing factor).
    const float rate = u.lp.turn_rate * flee;
    const V2 off{u.tslot.x + u.jiggle.x, u.tslot.y + u.jiggle.y};
    // s78n t14 (FIGHTER_FLYING §7.3): the container's state copied to the member. Port Walk states: WalkStartTurn /
    // WalkTurning = retail 0x10 (the port's Walk turns in place in 0x0F), WalkMoving = 0x13, WalkEndMove = 0x14; WalkStopped
    // / None = 0 / 0x16 (the recovery decision); any other = 0 (idle).
    const bool turning = k.state == LocoState::WalkStartTurn || k.state == LocoState::WalkTurning;
    const bool steering = turning || k.state == LocoState::WalkMoving || k.state == LocoState::WalkEndMove;
    if (!steering) {
        // Recovery decision 0x62bbf0 (item 6): only while the container is laid out and has no target. No distance limit,
        // no timer, no give-up (FIGHTER_FLYING §7.3 "Recovery in full" item 7).
        const bool decide = (k.state == LocoState::WalkStopped || k.state == LocoState::None) && k.laid_out && !k.has_target;
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
        // the brake distance is 3-D with the slot point at the container's height (§7.3 item 6 [quirk])
        const float dz = k.z - u.z;
        const float d3 = std::sqrt(dist * dist + dz * dz);
        const float want = d3 < v0 * v0 / (2.0f * decel) ? 0.0f : vmax;
        float sp = v0;
        if (sp < want) sp = std::min(want, sp + accel);
        else sp = std::max(want, sp - decel);
        sp = std::clamp(sp, 0.0f, std::max(vmax, 0.0f));
        u.m.speed = sp;
        if (!(sp > 0.0f)) {
            u.m.state = LocoState::WalkStopped;
            return;
        }
        // The step is committed as it is (vf +0x2f8 = 0x42d6a0: terrain conform and set position): no passability test, no
        // collision test - a straight walk at the slot, through anything (§7.3 item 7).
        const V2 fw = dir_from_deg(u.m.yaw);
        u.m.pos = V2{fw.x * sp + u.m.pos.x, fw.y * sp + u.m.pos.y};
        u.m.state = LocoState::WalkMoving;
        return;
    }
    u.recovery = 0;
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
    V2 dir{c + w * (perp.x - c), (perp.y - s) * w + s};
    // s78n t14 part B (FIGHTER_FLYING §7.5): + the collision steering vector 0x509810 of the container's collision objects
    // (start-of-tick positions; the member's new speed for the 0x5c6d40 ratio).
    if (k.rec_end > k.rec_begin)
        dir = dir + member_avoid(u, u.m.pos, sp, vmax, dir, recs.subspan(k.rec_begin, k.rec_end - k.rec_begin), pos0);
    // Step 7: turn by at most the turn rate.
    u.m.yaw = turn_toward(u.m.yaw, heading_deg(dir), rate);
    if (!(sp > 0.0f)) {
        u.m.state = LocoState::WalkStopped;
        return;
    }
    // Step 8: move along the new yaw. Retail commits the step with no passability test (the same vf +0x2f8 as the
    // recovery step), so nothing can refuse it: that is what bounds a member's lag together with the straggler cap.
    const V2 fw = dir_from_deg(u.m.yaw);
    u.m.pos = V2{fw.x * sp + u.m.pos.x, fw.y * sp + u.m.pos.y};
    u.m.state = LocoState::WalkMoving;
}

}  // namespace alamo::tactical::movement

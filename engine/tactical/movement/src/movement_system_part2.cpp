// movement_system: part 2 (bo3 t03 split; shared code in movement_system_internal.hpp).
#include "movement_system_internal.hpp"

namespace alamo::tactical::movement {
void MovementSystem::resolve_collision(CollisionObject& c, bool full, uint32_t frame, SimRng& rng) {
    if (c.kind == CollisionKind::Terrain) {  // s78n t24 (MOVEMENT §5.4.7): one participant
        terrain_service(c, full, frame);
        return;
    }
    Unit& a = units_[static_cast<size_t>(find(c.a))];
    Unit& b = units_[static_cast<size_t>(find(c.b))];
    if (c.kind != CollisionKind::MovingVsMoving) {  // each service answers anew (0x67f080): max speed, no flag
        c.speed_a = c.speed_b = kInf;
        c.now_a = c.now_b = false;
    }
    switch (c.kind) {
        case CollisionKind::MovingVsMoving:  // s78n t23 (MOVEMENT §5.4.2 / §5.4.6): every pair
            mvm_service(c, a, b, full, frame, rng);
            return;
        case CollisionKind::MovingVsStopped:
            mvs_service(c, a, b, full, frame);  // s78n t18 (MOVEMENT §5.4.1)
            return;
        case CollisionKind::StoppedVsStopped:
            svs_service(c, a, b, frame);  // s78n t20 (MOVEMENT §5.4.3)
            return;
        case CollisionKind::Terrain: return;  // (above)
    }
}

// ---- s78n t18 (MOVEMENT §5.4.1 / §5.4.2): record life, the MovingVsStopped decision and the MovingVsMoving states ----

// A record is kept while both participants are layer objects; it then ends only by its state (s78n t23: MovingVsMoving
// too - its validity 0x680650, "either one not Moving", runs in its service).
bool MovementSystem::collision_valid(const CollisionObject& c) const {
    // s78n t24 (MOVEMENT §5.4.7 item 2): a Terrain record drops its unit when it is destroyed (vf +0x08) and is finished
    // with no participant (decision step 2 / service: state 3)
    if (c.kind == CollisionKind::Terrain) return find(c.a) >= 0 && land_layer_object(units_[static_cast<size_t>(find(c.a))]);
    const int32_t ia = find(c.a), ib = find(c.b);
    if (ia < 0 || ib < 0) return false;
    return land_layer_object(units_[static_cast<size_t>(ia)]) && land_layer_object(units_[static_cast<size_t>(ib)]);
}

float MovementSystem::record_answer(const CollisionObject& c, bool for_a) {
    if (c.kind == CollisionKind::MovingVsMoving && c.state == CollisionState::Active) return kInf;  // 0x680d00, state 0
    return for_a ? c.speed_a : c.speed_b;
}

bool MovementSystem::unit_moving(const Unit& u) const {
    // vf +0x1a8 0x42cc60: not suspended (pack +0x260; the port's speed factor 0) and (speed > 0 or a path record on its
    // stack); vf +0x1b0 0x42cd30: not suspended and turning in place (the start turn, the arrival turn, a container's
    // final-facing turn)
    if (!(u.speed_factor > 0.0f) || u.d.no_locomotor) return false;  // s78n t20 batch 1: no locomotor, never Moving
    return u.m.speed > 0.0f || u.pushed || (u.has_goal && !u.need_path) || in_place_turn(u);
}

namespace bo3split {

uint8_t record_weight(CollisionKind k) {  // vf +0x30 (s78n t24: Terrain 4.0, the highest, MOVEMENT §5.4.7 item 2)
    switch (k) {
        case CollisionKind::MovingVsMoving: return 1;
        case CollisionKind::MovingVsStopped: return 2;
        case CollisionKind::StoppedVsStopped: return 3;
        case CollisionKind::Terrain: return 4;
    }
    return 0;
}
}  // namespace bo3split

namespace bo3split {
// s78n t20 batch 1 (MOVEMENT §5.4.4 item 5, 0x5c5910): a path record "on its last waypoint" after a cut - its index is past
// the end (a one-node or over-cut list), or it heads for its last point and the unit is already past that point's line
// (the line through it, normal = its segment's direction), so the index steps on. Then `next` = the point count.
bool record_finished(const std::vector<V2>& pts, uint32_t& next, V2 pos) {
    const size_t n = pts.size();
    if (static_cast<size_t>(next) + 1 == n && n >= 2) {
        const V2 nrm = normalize_retail(pts[n - 1] - pts[n - 2]);
        if (length_sq(nrm) > 0.0f && dot(pos - pts[n - 1], nrm) >= 0.0f) next = static_cast<uint32_t>(n);
    }
    if (static_cast<size_t>(next) < n) return false;
    next = static_cast<uint32_t>(n);
    return true;
}
}  // namespace bo3split

bool MovementSystem::claim(Unit& u, const CollisionObject& c, bool force, uint32_t frame) {
    const bool other = u.pushed && u.push_owner != kNoDetour && u.push_owner != c.seq;
    if (other && !force && !(record_weight(c.kind) > u.push_weight)) return false;
    if (other) {  // 0x559410: a claim by a new owner removes every collision-owned record of the unit
        end_push(u);
        u.around.clear();
        u.path_change = frame;
    }
    return true;
}

void MovementSystem::push_top(Unit& u, std::vector<V2> path, uint8_t kind, bool flee, const CollisionObject& c, uint32_t frame) {
    end_push(u);
    u.pushed = true;
    u.push_kind = kind;
    u.push_flee = flee;  // kind 7 (a crush-flee path), else kind 4
    u.push_owner = c.seq;
    u.push_weight = record_weight(c.kind);
    u.push_path = std::move(path);
    u.push_next = 1;
    u.around.clear();
    u.path_change = frame;  // s78n t18: pack +0x288 (a path record is pushed on its stack)
    // s78n t23 (MOVEMENT §5.4.1 "The stamp in full" item 1): the push stamps the unit's held records (0x50a020). The
    // record that pushes is never stamped by its own detour - by the role test, the detour lands on its detourer.
    stamp_held(u, frame);
    if (u.d.team_container) {
        // s78n t14 (FIGHTER_FLYING §7.2 caller 4, 0x5ad140): a squad container's path that is not a formation path and
        // has >= 2 points re-lays toward wp1 - wp0. s78n t18 (MOVEMENT §5.4.1 "Push"): the path is marked started - a
        // standing container starts it at once, a moving one walks on along it (no braking coast, no arrival braking
        // carried over from the path below)
        if (u.push_path.size() >= 2) u.relay.push_back(u.push_path[1] - u.push_path[0]);
        u.coast = false;
        if (u.m.state == LocoState::WalkEndMove) u.m.state = LocoState::WalkMoving;
        else if (u.m.state != LocoState::WalkMoving) u.m.state = LocoState::WalkStopped;
    }
}

void MovementSystem::stamp_held(const Unit& u, uint32_t frame) {
    for (CollisionObject& o : coll_.list_mut()) {
        if (o.kind != CollisionKind::MovingVsMoving || (o.a != u.d.id && o.b != u.d.id)) continue;
        const ObjectId m = o.master == 0 ? o.a : o.b;
        if ((o.state == CollisionState::PathAround && m == u.d.id) || (o.state == CollisionState::Pass && m != u.d.id))
            o.redecide = frame + 2;
    }
}

bool MovementSystem::pop_top(Unit& u, bool walked, uint32_t frame) {
    const uint8_t kind = u.push_kind;
    const uint32_t resume = u.push_resume;
    end_push(u);
    u.around.clear();
    u.path_change = frame;  // s78n t18: the record is popped (pack +0x288)
    LandMotion& m = u.m;
    const size_t n = m.path.size();
    if (!walked || !u.has_goal || u.need_path) return false;
    // s78n t20 batch 1: an order path cut to its end underneath (0x54e780 leaves its index; MOVEMENT §5.4.4 item 5) has
    // no waypoint left - arrived where it stands
    if (n < 2 || m.next >= n) return true;
    // A detour rejoins the order path on the segment that ends at `resume` (the path size: it ended past the path end).
    if (kind == 2) m.next = std::max(m.next, std::min(resume, static_cast<uint32_t>(n)));
    if (m.next == 0) m.next = 1;
    // 0x620820: the waypoint index steps past every waypoint line the unit has already crossed (the line through the
    // waypoint, normal = the bisector of its two segments; the last waypoint: its segment's direction)
    while (static_cast<size_t>(m.next) + 1 < n) {
        const V2 w = m.path[m.next];
        const V2 nrm = normalize_retail(normalize_retail(w - m.path[m.next - 1]) + normalize_retail(m.path[m.next + 1] - w));
        if (!(length_sq(nrm) > 0.0f) || dot(m.pos - w, nrm) < 0.0f) break;
        ++m.next;
    }
    if (static_cast<size_t>(m.next) + 1 == n) {
        const V2 nrm = normalize_retail(m.path[n - 1] - m.path[n - 2]);
        if (length_sq(nrm) > 0.0f && dot(m.pos - m.path[n - 1], nrm) >= 0.0f) m.next = static_cast<uint32_t>(n);
    }
    return m.next >= n;
}

MovementSystem::StackPoly MovementSystem::stack_poly(const Unit& u, std::vector<V2>& scratch) const {
    const bool order = u.has_goal && !u.need_path && u.m.next < u.m.path.size();
    if (!u.pushed) return {u.m.path, order ? u.m.next : static_cast<uint32_t>(u.m.path.size())};
    scratch.clear();
    for (size_t i = u.push_next; i < u.push_path.size(); ++i) scratch.push_back(u.push_path[i]);
    if (order) {
        size_t from = u.m.next;
        if (u.push_kind == 2) from = std::max(from, static_cast<size_t>(u.push_resume));
        for (size_t i = from; i < u.m.path.size(); ++i) scratch.push_back(u.m.path[i]);
    }
    return {scratch, 0u};
}

void MovementSystem::stack_points(const Unit& u, std::vector<V2>& out) const {
    std::vector<V2> scratch;
    const StackPoly sp = stack_poly(u, scratch);
    std::vector<V2> pts{u.m.pos};
    for (size_t i = sp.next; i < sp.pts.size(); ++i)
        if (!(pts.back() == sp.pts[i])) pts.push_back(sp.pts[i]);
    out = std::move(pts);
}

V2 MovementSystem::type_half(const Unit& u) const {
    if (u.d.type_hx >= 0.0f && u.d.type_hy >= 0.0f) return {u.d.type_hx, u.d.type_hy};
    if (u.d.hard_x > 0.0f || u.d.hard_y > 0.0f) return {std::max(u.d.hard_x, 0.0f), std::max(u.d.hard_y, 0.0f)};
    return {u.d.radius, u.d.radius};  // [port] not given and no hard extents: the soft radius
}

// 0x396b70 (MOVEMENT §5.3 "When the deployed pair is used"): a squad container's joint slot box; else, deploying or
// deployed, the deployed Y alone when it is set (the per-axis 0x372100: no X test); else the normal half extents' y.
// Undeploying (0x1A) uses the normal extents.
float MovementSystem::half_width(const Unit& u) const {
    if (u.d.team_container && u.team_boxed) return u.team_box_hy;
    if (deployed_extents(u) && u.d.hard_y_deployed > 0.0f) return u.d.hard_y_deployed;
    if (u.d.hard_x > 0.0f || u.d.hard_y > 0.0f) return std::max(u.d.hard_y, 0.0f);
    return u.d.radius;
}

bool MovementSystem::set_collision_info(ObjectId id, const CollisionInfo& info) {
    Unit* u = unit_ptr(id);
    if (!u) return false;
    u->ci = info;
    return true;
}

bool MovementSystem::collision_info(ObjectId id, CollisionInfo& out) const {
    const Unit* u = unit_ptr(id);
    if (!u) return false;
    out = u->ci;
    return true;
}

std::span<const V2> MovementSystem::top_path(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    if (!u || !u->pushed) return {};
    return u->push_path;
}

std::span<const HoldTag> MovementSystem::top_tags(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    if (!u || !u->pushed) return {};
    return u->push_tags;
}

float MovementSystem::walk_max_speed(const Unit& u) const {
    if (u.d.team_container && u.team_cap < kInf) return u.team_cap * (u.pushed && u.push_flee ? cfg_.constants.fleeing_infantry_speed_bonus : 1.0f);
    return loco_max_speed(u);
}

V2 MovementSystem::wanted_dir(const Unit& u) const {
    // the locomotor's wanted facing (vf +0x1f8): the direction to its current waypoint (s78n t20: of the top of its path
    // stack), else its facing
    std::vector<V2> scratch;
    const StackPoly sp = stack_poly(u, scratch);
    if (sp.next < sp.pts.size()) {
        const V2 d = normalize_retail(sp.pts[sp.next] - u.m.pos);
        if (length_sq(d) > 0.0f) return d;
    }
    return dir_from_deg(u.m.yaw);
}

float MovementSystem::record_cap(const Unit& u) const {
    // 0x509ea0: the lowest speed its collision records answer it (as last serviced)
    float cap = kInf;
    for (const CollisionObject& c : coll_.list()) {
        if (c.kind == CollisionKind::StoppedVsStopped) continue;  // answers max speed
        if (c.a == u.d.id) cap = std::min(cap, record_answer(c, true));
        if (c.b == u.d.id) cap = std::min(cap, record_answer(c, false));
    }
    return cap;
}

// 0x683ee0 with r = 0x3969f0: b lies behind a's facing by more than the two footprint radii.
bool MovementSystem::mvs_behind(const Unit& a, const Unit& b) const {
    return dot(b.m.pos - a.m.pos, dir_from_deg(a.m.yaw)) < -(foot_radius(a) + foot_radius(b));
}

// 0x67eb90 with r = 0x397780: either one lies behind the other's facing by more than 1.3 x (r0 + r1).
bool MovementSystem::mvm_passed(const Unit& a, const Unit& b) const {
    const float r = 1.3f * (steer_radius(a) + steer_radius(b));
    return dot(b.m.pos - a.m.pos, dir_from_deg(a.m.yaw)) < -r || dot(a.m.pos - b.m.pos, dir_from_deg(b.m.yaw)) < -r;
}

// Slave speed 0x67f590(x, y) -> 0x67f130(x, y) (MOVEMENT §5.4.2). `touch`: the two footprints touch (the "at once" flag).
float MovementSystem::slave_speed(const Unit& x, const Unit& y, bool& touch) const {
    touch = false;
    const float vmax = walk_max_speed(x);
    const V2 fx = dir_from_deg(x.m.yaw), fy = dir_from_deg(y.m.yaw);
    const V2 rel = y.m.pos - x.m.pos;
    // separate lanes: the facings within 10 degrees of parallel or anti-parallel and y at least hy_x + hy_y to the side
    // (s78n t23: the half widths 0x396b70, with their deployed switch)
    if (std::fabs(dot(fx, fy)) >= 0.9848f && std::fabs(cross(fx, rel)) >= half_width(x) + half_width(y)) return vmax;
    return closing_speed(x, y, touch);
}

// 0x67f130(x, y): the speed at which x closes on y without running into it (MOVEMENT §5.4.2), no lane test.
float MovementSystem::closing_speed(const Unit& x, const Unit& y, bool& touch) const {
    touch = false;
    const float vmax = walk_max_speed(x);
    const V2 fx = dir_from_deg(x.m.yaw), fy = dir_from_deg(y.m.yaw);
    const V2 rel = y.m.pos - x.m.pos;
    const float v = x.m.speed;
    const float vmin = std::min(x.sp.min_speed, vmax);
    const V2 d = normalize_retail(rel);
    const float cy = y.m.speed * dot(fy, d), cx = -vmax * dot(fx, d);
    if (cx + cy >= 0.0f) return vmax;  // x is not closing even at full speed
    const float gap = foot_gap(foot_of(x), foot_of(y));
    if (gap == 0.0f) touch = true;
    float tau = gap / -(cx + cy);
    const float follow = cx == 0.0f ? 0.0f : std::clamp(std::fabs(cy / cx) * vmax * 0.8f, vmin, vmax);
    // s78n t18 review: a squad container's rate 0x5ab540 taken here (team_rate), not Unit::team_accel - that is the
    // walk's copy of the tick before (the driver runs first) and is not saved, so a loaded battle had 0 in its first tick
    const float rate = x.d.team_container ? team_rate(x) : 0.0f;
    const float dec = x.d.team_container ? (rate > 0.0f ? rate : vmax) : walk_decel(x, vmax);
    if (follow < v && dec > 0.0f) tau = std::max(0.0f, tau - (v - follow) / dec);
    const float k = std::clamp(tau / (30.0f * cfg_.constants.moving_vs_moving_look_ahead_time), 0.0f, 1.0f);
    if (cy > 0.0f) return gap <= 0.0f ? 0.0f : follow + (vmax - follow) * k;  // y moves away along the line
    return vmin + (vmax - vmin) * k;
}

// s78n t20 (MOVEMENT §5.4.6, 0x5c69f0; the MovingVsStopped answer 0x684660 in state 1).
float MovementSystem::follow_speed(const Unit& s, const CollisionObject& c) const {
    const float vf = s.formation_speed;
    if (!holds_path(s, c) || !s.in_formation || !(vf > 0.0f) || !(vf < kInf) || s.push_path.size() < 2) return kInf;
    float len = 0.0f;
    for (size_t i = 1; i < s.push_path.size(); ++i) len += distance(s.push_path[i - 1], s.push_path[i]);
    const float straight = distance(s.push_path.front(), s.push_path.back());
    if (!(straight > 0.0f)) return kInf;
    return std::min(walk_max_speed(s), vf * len / straight);
}

bool MovementSystem::on_blocked_cell(const Unit& u) const {
    if (u.d.no_locomotor) return false;  // s78n t20 batch 1: the test needs a locomotor (a clone stands on its own stamp)
    const bool order = u.has_goal && !u.pushed;  // the order path is the top record (or will be, once planned)
    if (order && (u.need_path || u.forced || u.m.next < u.m.escape_until)) return false;  // forced ([port] see the header)
    return !grid_.passable_world(u.m.pos, u.lp.mask);
}

void MovementSystem::collision_stop(Unit& u, const CollisionObject& c, uint32_t frame) {
    const bool own = u.push_owner == kNoDetour || u.push_owner == c.seq;  // 0x42cba0: the pack's path owner is 0 or c
    // 0x554480(pack, unit, id != 0): every record of the stack is destroyed
    end_push(u);
    // bkm1 t07 (b0055, MOVEMENT §3.5.2 G, §4.6.2): a new order that is not planned yet lives in its formation's block, not
    // on the path stack, so the Stop of the old path's records leaves it: the unit stands and plans it when its turn comes
    if (!(u.has_goal && u.need_path)) {
        if (u.has_goal) u.arrived = true;
        u.has_goal = false;
        u.need_path = false;
        if (u.start_frame == kStartPending) u.start_frame = 0;
    }
    u.m.path.clear();
    u.m.next = 0;
    u.m.escape_until = 0;
    u.around.clear();
    u.path_change = frame;  // pack +0x288: the stack changed
    u.fturn = u.path_started = false;
    if (own) {  // state 0x16, pack +0xec / +0xf0 := 0 at once
        u.m.speed = 0.0f;
        u.m.state = LocoState::WalkStopped;
        u.coast = false;
    } else {
        brake_to_stop(u, frame);
    }
}

// ---- s78n t24 (MOVEMENT §5.4.7): TerrainCollision ----

// The failed-step report 0x509090 (serial commit pass, ascending unit index): the step was not committed; the speed is
// zeroed at once and a one-participant record is made - every failed step makes one (no count, no duplicate rule).
// [port] it is decided by the next tick's driver pass (the frame order of the steps against the driver is not read).
void MovementSystem::terrain_report(Unit& u, uint32_t frame) {
    u.m.speed = 0.0f;
    coll_.add(CollisionKind::Terrain, u.d.id, kNoParticipant, frame);
    ++stats_.collisions_created;
    ++stats_.terrain_records;
}

// Decision 0x687ff0 (quota pass, full) / service 0x687ea0. The record answers max speed and no "at once" flag in every
// state (the steering vector is zero: team_steer_recs). [port] a failed Path Around stops the unit and the record is
// dropped at once (retail keeps it in its state 4 for ever, with no effect but its zero steering vector of weight 4).
void MovementSystem::terrain_service(CollisionObject& c, bool full, uint32_t frame) {
    using S = CollisionState;
    c.speed_a = c.speed_b = kInf;
    c.now_a = c.now_b = false;
    const int32_t iu = find(c.a);
    if (iu < 0) {  // no participant
        c.state = S::Resolved;
        return;
    }
    Unit& u = units_[static_cast<size_t>(iu)];
    if (u.d.no_locomotor) {  // state 2
        c.state = S::Resolved;
        return;
    }
    if (c.state == S::Resolving) {  // state 1: ends when no path record of the unit is owned by it
        if (!holds_path(u, c)) c.state = S::Resolved;
        return;
    }
    if (c.state != S::Active) return;
    // state 0: the own position impassable -> Stop (every path record destroyed), state 2
    if (on_blocked_cell(u)) {
        collision_stop(u, c, frame);
        ++stats_.terrain_stops_cell;
        c.state = S::Resolved;
        return;
    }
    if (!full) return;  // waits for the quota
    // Path Around (no obstacle, flag 0, kind 3, flags 0): D = r_u + 40, no last-point drop
    std::vector<V2> scratch;
    std::span<const V2> base;
    uint32_t cur = 0;
    const bool has_base = around_base(u, scratch, base, cur);
    if (path_around(c, u, nullptr, false, 3, 0, frame)) {
        // waypoint 0 := (1.0, 0.0). s78n t26.2 (MOVEMENT §5.4.7 item 6, §3.5.2 A / B.4): the detour record's speed fraction
        // 0 on waypoint 0 - the corner cap sends a moving unit to End Move, and the start rule (fraction 0 fails its
        // close-enough clause) turns it in place onto the first leg before it moves; a squad container: its start turn
        // (FIGHTER_FLYING §7.7 item 7)
        u.push_frac = {0.0f};
        if (u.d.team_container) u.m.state = LocoState::WalkStopped;
        ++stats_.terrain_detours;
        c.state = S::Resolving;
        return;
    }
    collision_stop(u, c, frame);  // failed: Stop, state 4 ([port] dropped)
    if (has_base) {
        ++stats_.terrain_stops_failed;
    } else {
        ++stats_.terrain_stops_nobase;
    }
    c.state = S::Resolved;
}

// Destination blocked 0x684710(a, b): a's path approximation reaches the end of its path stack and a's rectangle placed
// at the path end overlaps b's or that of a layer object standing within the 160 x 160 box around the end. [port] "the
// approximation reaches the path end" = near_path_end: the path end is no farther from the mover (straight line) than
// the contact test's look-ahead distance + the two footprint radii, so a contact with a unit on the destination is
// decided as blocked from its first frame (the port keeps no approximation; its cell paths have many short legs, so "on
// the last waypoint" would be too narrow).
bool MovementSystem::near_path_end(const Unit& a, const Unit& b) const {
    std::vector<V2> scratch;
    const StackPoly sp = stack_poly(a, scratch);  // s78n t20: the end of the path stack
    return sp.next < sp.pts.size() &&
           distance(a.m.pos, sp.pts.back()) <= mvs_look_distance(a, b) + foot_radius(a) + foot_radius(b);
}

bool MovementSystem::destination_blocked(const Unit& a, const Unit& b) const {
    // s78n t20 batch 1: E = the end of the path stack (a collision-owned top record included), at or beyond the top
    // record's current waypoint line: n . (E - W) >= -0.1 (W = its current waypoint, n = the bisector of its two segments;
    // the last waypoint: its segment's direction)
    std::vector<V2> scratch;
    const StackPoly sp = stack_poly(a, scratch);
    if (sp.next >= sp.pts.size() || !near_path_end(a, b)) return false;
    const V2 e = sp.pts.back();
    const std::vector<V2>& top = a.pushed ? a.push_path : a.m.path;
    const uint32_t ti = a.pushed ? a.push_next : a.m.next;
    if (ti < top.size()) {
        const V2 w = top[ti];
        V2 nrm = normalize_retail(w - (ti > 0 ? top[ti - 1] : a.m.pos));
        if (static_cast<size_t>(ti) + 1 < top.size()) nrm = normalize_retail(nrm + normalize_retail(top[ti + 1] - w));
        if (length_sq(nrm) > 0.0f && dot(e - w, nrm) < -0.1f) return false;
    }
    // s78n t20 batch 1 (MOVEMENT §5.4.5 item 2): only objects with a locomotor count - a death clone never blocks the
    // destination
    const FootRect ra = foot_at(a, e);
    if (!b.d.no_locomotor && foot_overlap(ra, foot_of(b))) return true;
    std::vector<uint32_t> cand;
    layer_box_query(e, 0.5f * kCollisionCrowdBox, cand);
    for (uint32_t ci : cand) {
        const Unit& o = units_[ci];
        if (o.d.id == a.d.id || o.d.id == b.d.id || o.d.no_locomotor || unit_moving(o)) continue;
        if (foot_overlap(ra, foot_of(o))) return true;
    }
    return false;
}

// 0x6835c0: a holds no other record that is a StoppedVsStopped or a MovingVsStopped in which a is the obstacle, and the
// pair is not a Vehicle / Large / Giant mover against Infantry / Bike.
bool MovementSystem::side_step_ok(const Unit& a, const Unit& b, const CollisionObject& self) const {
    const int32_t ca = a.d.collision_class, cb = b.d.collision_class;
    if (ca >= kUccVehicle && ca <= kUccGiantVehicle && (cb == kUccInfantry || cb == kUccBike)) return false;
    for (const CollisionObject& c : coll_.list()) {
        if (c.seq == self.seq) continue;
        if (c.kind == CollisionKind::StoppedVsStopped && (c.a == a.d.id || c.b == a.d.id)) return false;
        if (c.kind == CollisionKind::MovingVsStopped && c.b == a.d.id) return false;
    }
    return true;
}

// 0x684100 on a unit's order path (its kind-1 record): 0x54e780 with keep = the whole list's length (from its first point,
// the part already walked included) less c; the waypoint index is not touched (s78n t20 batch 1, MOVEMENT §5.4.4 item 5).
bool MovementSystem::cut_order_path(Unit& u, float c, uint32_t frame) {
    LandMotion& m = u.m;
    if (!u.has_goal || u.need_path || m.path.size() < 2) return false;
    float total = 0.0f;
    for (size_t i = 1; i < m.path.size(); ++i) total += distance(m.path[i - 1], m.path[i]);
    (void)cut_points(m.path, total - c);
    u.goal = m.path.back();
    u.path_change = frame;
    return true;
}

// 0x684100 on the collision-owned top record (a's top path is no formation path): the same cut; the hold-line tags past
// the new end (and on an appended point, which carries no id) are dropped.
void MovementSystem::cut_top_path(Unit& u, float c, uint32_t frame) {
    float total = 0.0f;
    for (size_t i = 1; i < u.push_path.size(); ++i) total += distance(u.push_path[i - 1], u.push_path[i]);
    const bool added = cut_points(u.push_path, total - c);
    const size_t n = u.push_path.size();
    std::erase_if(u.push_tags, [&](const HoldTag& h) { return h.index >= n || (added && static_cast<size_t>(h.index) + 1 == n); });
    u.path_change = frame;
}

// s78n t20 batch 1 (MOVEMENT §5.4.4 item 5 "What the unit then does", the Walk step 0x620820 / container step 0x5adfb0):
// a top record left on its last waypoint by a cut is finished. A collision-owned one over a started order path (the unit
// not in End Move) is popped and the unit walks on along the order path; otherwise the unit goes to End Move and brakes
// from where it is (brake_to_stop; [port] a finished order path ends as arrived at once, the braking follows).
// s78n t23 step 0 (t21 review items 1 and 4): run at the walk step (tick_land, after the collision driver, for the units
// a cut marked), not during the decision: a decision that goes on after the cut (early arrival false) sees the finished
// top record, on which Path Around fails (§1.5.1 item 2). An order path with nothing left to walk under the popped record
// (an over-cut follower, keep <= 0) ends as arrived, braking or not.
void MovementSystem::cut_finished(Unit& u, uint32_t frame) {
    if (u.pushed) {
        if (!record_finished(u.push_path, u.push_next, u.m.pos)) return;
        const bool below = u.has_goal && !u.need_path && frame >= u.start_frame && u.m.next < u.m.path.size() &&
                           (!u.d.team_container || u.path_started);
        if (!below || u.m.state == LocoState::WalkEndMove) {
            const bool used_up = u.has_goal && !u.need_path && u.m.next >= u.m.path.size();
            end_push(u);
            u.around.clear();
            u.path_change = frame;
            if (used_up) {
                u.has_goal = false;
                u.arrived = true;
                u.exit_host = -1;
                land_arrival_turn(u);
                arrival_turn_rest(u);
            }
            brake_to_stop(u, frame);
            return;
        }
        if (!pop_top(u, true, frame)) return;  // walks on along the order path
    } else if (!u.has_goal || u.need_path || !record_finished(u.m.path, u.m.next, u.m.pos)) {
        return;
    }
    u.has_goal = false;  // the order path has no waypoint left: arrived where it stands
    u.arrived = true;
    walk_out_done(u);
    u.around.clear();
    u.path_change = frame;
    land_arrival_turn(u);
    arrival_turn_rest(u);
    brake_to_stop(u, frame);
}

// Early arrival 0x685e10 with the cut 0x684100 (MOVEMENT §5.4.4; called on the mover's final leg with a blocked
// destination). A = the mover's path stack from its position (length L, end E). Obstacles: the layer objects whose box
// meets the 160 x 160 square on E, plus b, without every Moving one (the mover itself drops out as Moving). The box: the
// mover TYPE's half extents 0x371be0 (a squad container: (0, 0), not its joint slot box) with hx + 5, turned along the path.
// Scan s = L, L - 5, ..., the last test at s = 0 (its own position): the first point P whose box overlaps no listed
// footprint (touching counts) and, with a target, lies within the attack range (+ the target's radius) of it. With a
// spare point v: P farther from E than v -> fail, nothing changed. Else the path is cut by c = (L - s) + 3.0 (0x54e780,
// keep measured from each record's first point): a collision-owned top record alone; on a formation path (the order
// path on top) a's order path and those of the formation's members whose slot is behind a's and within the two half
// widths sideways, by the same c. A record the cut leaves on its last waypoint is finished (cut_finished). True iff b is
// at or ahead of P: (b - P) . n >= -0.001. [port] the target's radius is added whenever the host reports a target (the
// weapon-list test is [M]).
bool MovementSystem::early_arrival(Unit& a, const Unit& b, bool have_v, V2 v, uint32_t frame) {
    std::vector<V2> pts;
    stack_points(a, pts);
    if (pts.size() < 2) return false;
    float total = 0.0f;
    for (size_t i = 1; i < pts.size(); ++i) total += distance(pts[i - 1], pts[i]);
    const V2 end = pts.back();
    const V2 th = type_half(a);
    std::vector<uint32_t> cand;
    layer_box_query(end, 0.5f * kCollisionCrowdBox, cand);
    std::vector<FootRect> blockers;
    // s78n t20 batch 1 (MOVEMENT §5.4.4 item 2): every object without a locomotor and every Moving one is dropped
    if (!b.d.no_locomotor && !unit_moving(b)) blockers.push_back(foot_of(b));
    for (uint32_t ci : cand) {
        const Unit& o = units_[ci];
        if (o.d.id == a.d.id || o.d.id == b.d.id || o.d.no_locomotor || unit_moving(o)) continue;
        blockers.push_back(foot_of(o));
    }
    const bool has_target = a.ci.target >= 0;
    const float range = has_target ? a.ci.attack_range + a.ci.target_radius : 0.0f;
    // the point of A at arc length s and the unit direction of its segment there
    const auto at_arc = [&](float s, V2& n) {
        float acc = 0.0f;
        for (size_t i = 1; i < pts.size(); ++i) {
            const float l = distance(pts[i - 1], pts[i]);
            if (acc + l >= s || i + 1 == pts.size()) {
                n = normalize_retail(pts[i] - pts[i - 1]);
                return l > 0.0f ? lerp(pts[i - 1], pts[i], std::clamp((s - acc) / l, 0.0f, 1.0f)) : pts[i - 1];
            }
            acc += l;
        }
        n = V2{};
        return pts.back();
    };
    for (float s = total;; s = std::max(0.0f, s - 5.0f)) {
        V2 n{};
        const V2 p = at_arc(s, n);
        if (!(length_sq(n) > 0.0f)) n = dir_from_deg(a.m.yaw);
        const FootRect box{p, n, th.x + 5.0f, th.y, false};
        bool clear = true;
        for (const FootRect& o : blockers) clear = clear && !foot_overlap(box, o);
        if (clear && has_target && distance(p, a.ci.target_pos) > range) clear = false;
        if (clear) {
            if (have_v && distance(p, end) > distance(v, end)) return false;  // the spare point is nearer the end
            const float c = (total - s) + 3.0f;
            if (a.pushed) {  // the top path is no formation path: a's top record only (s78n t20 batch 1)
                cut_top_path(a, c, frame);
                a.cut_pending = true;  // s78n t23 step 0: finished at the walk step (cut_finished)
                return dot(b.m.pos - p, n) >= -0.001f;
            }
            const float ax = a.form_slot.x, ay = a.form_slot.y, hwa = half_width(a);
            if (a.form_id != 0) {  // a formation path: the kind-1 records of the members behind a's slot in its lane too
                for (Unit& m : units_) {
                    if (m.form_id != a.form_id || m.d.id == a.d.id) continue;
                    if (!(m.form_slot.x < ax) || std::fabs(m.form_slot.y - ay) > half_width(m) + hwa) continue;
                    if (cut_order_path(m, c, frame)) m.cut_pending = true;
                }
            }
            if (cut_order_path(a, c, frame)) a.cut_pending = true;
            return dot(b.m.pos - p, n) >= -0.001f;
        }
        if (s == 0.0f) return false;  // even its own position fails: nothing changed
    }
}

// ---- s78n t20 (MOVEMENT §1.5.1): the Path Around call of the dynamic land pathfinder ----

bool MovementSystem::shares_open_record(const Unit& u, const Unit& o) const {
    // record vf +0x58: MovingVsStopped state 0 / 1, MovingVsMoving state 0 / 1 / 4 (Pass)
    for (const CollisionObject& c : coll_.list()) {
        if (!((c.a == u.d.id && c.b == o.d.id) || (c.a == o.d.id && c.b == u.d.id))) continue;
        if (c.kind == CollisionKind::StoppedVsStopped) return false;
        return c.state == CollisionState::Active || c.state == CollisionState::PathAround || c.state == CollisionState::Pass;
    }
    return false;
}

uint8_t MovementSystem::unit_relation(const Unit& p0, const Unit& p1, bool own_clause) const {
    RelationInput in;
    in.p0_moving = unit_moving(p0);
    if (!in.p0_moving) return kRelStanding;
    in.p0_cap = record_cap(p0);
    in.h0 = wanted_dir(p0);
    in.h1 = wanted_dir(p1);
    const auto speed = [this](const Unit& x) { return x.in_formation && x.formation_speed < kInf ? x.formation_speed : walk_max_speed(x); };
    in.v0 = speed(p0);
    in.v1 = speed(p1);
    in.rel = p1.m.pos - p0.m.pos;
    in.hw1 = half_width(p1);
    for (const CollisionObject& c : coll_.list()) {
        if (!((c.a == p0.d.id && c.b == p1.d.id) || (c.a == p1.d.id && c.b == p0.d.id))) continue;
        in.own_record = own_clause && c.kind == CollisionKind::MovingVsMoving &&
                        (c.state == CollisionState::Active || c.state == CollisionState::PathAround || c.state == CollisionState::Pass);
        break;
    }
    return relation_code(in);
}

FootRect MovementSystem::unit_swept_box(const Unit& o, const Unit& u, uint8_t rel) const {
    SweptInput in;
    in.pos = o.m.pos;
    in.yaw = o.m.yaw;
    const V2 h = foot_half(o);
    in.hx = h.x;
    in.hy = h.y;
    std::vector<V2> poly;
    if (unit_moving(o)) {
        // turning in place by more than 10 degrees: a square along its wanted facing
        V2 wanted{};
        if (o.m.state == LocoState::WalkStartTurn) wanted = wanted_dir(o);
        else if (o.aturn != LocoState::None) wanted = dir_from_deg(o.turn_to);
        else if (o.fturn) wanted = dir_from_deg(land_wanted_yaw(o));
        if (length_sq(wanted) > 0.0f && std::fabs(wrap_deg(heading_deg(wanted) - o.m.yaw)) > 10.0f) {
            in.turning = true;
            in.wanted = wanted;
        } else {
            // closing on the pathing unit: swept along its path by L = min(max(0, (d - r_u - r_o) / c), 30 x
            // MovingVsMovingLookAheadTime) frames x its max speed; in relation 0x10 / 0x20 on its final leg: the whole path
            const float vo = walk_max_speed(o), vu = walk_max_speed(u);
            const V2 to_u = u.m.pos - o.m.pos;
            const V2 rel_v = dir_from_deg(o.m.yaw) * vo - dir_from_deg(u.m.yaw) * vu;
            const float closing = dot(rel_v, normalize_retail(to_u));
            stack_points(o, poly);
            if ((rel == kRelSameWay || rel == kRelSameWaySlower) && near_path_end(o, u)) {
                in.sweep = path_length(poly);
            } else if (vo > 0.0f && vu > 0.0f && closing > 0.0f) {
                const float frames = std::min(std::max(0.0f, (length(to_u) - foot_radius(u) - foot_radius(o)) / closing),
                                              30.0f * cfg_.constants.moving_vs_moving_look_ahead_time);
                in.sweep = frames * vo;
            }
            in.poly = poly;
        }
    }
    return swept_box(in);
}

// Obstacle set 0x67d580 (MOVEMENT §1.5.1 item 3) over the layer objects around u, ascending id. Never: u itself, anything
// u can crush, a pair that fails the class table, [port] a gated pair (its own garrison host). Always: the forced id.
// Mask bit 0 (u Infantry or Giant): Infantry-class objects not in a hero clash are no obstacles. Otherwise by the
// relation code of (object, u): standing -> an obstacle, subject to `flags` (1: a non-enemy only when it shares an
// undecided / detouring record with u; 2: the same for every standing unit; 4: the obstacle classes 2 - 4 are ignored;
// a deployed / deploying unit always is one); held, head-on, crossing, same way and slower -> an obstacle; same way and
// not slower -> ignored, unless it is on its final leg, not behind u's facing and the pair has such a record. s78n t23
// (MOVEMENT §5.4.6): the slaves of u's MovingVsMoving records in state 4 (Pass) where u is the master are always obstacles.
void MovementSystem::around_obstacles(const Unit& u, const Unit* forced, uint8_t flags, float half, std::vector<AroundObstacle>& out,
                                      std::vector<uint32_t>& ids) const {
    out.clear();
    ids.clear();
    std::vector<uint32_t> cand;
    layer_box_query(u.m.pos, half, cand);
    const int32_t cu = u.d.collision_class;
    const bool skip_infantry = cu == kUccInfantry || cu == kUccGiantVehicle;
    const V2 fu = dir_from_deg(u.m.yaw);
    for (uint32_t ci : cand) {
        const Unit& o = units_[ci];
        if (o.d.id == u.d.id) continue;
        const uint8_t rel = unit_relation(o, u);
        bool is_forced = forced && forced->d.id == o.d.id;
        if (!is_forced) {
            for (const CollisionObject& c : coll_.list()) {
                if (c.kind != CollisionKind::MovingVsMoving || c.state != CollisionState::Pass) continue;
                const ObjectId mid = c.master == 0 ? c.a : c.b, sid = c.master == 0 ? c.b : c.a;
                if (mid == u.d.id && sid == o.d.id) {
                    is_forced = true;
                    break;
                }
            }
        }
        if (!is_forced) {
            if (crushes(u, o) || !collision_classes_interact(cu, o.d.collision_class) || pair_gated(u, o)) continue;
            if (skip_infantry && o.d.collision_class == kUccInfantry && !o.ci.hero_clash) continue;
            if (rel == kRelStanding) {
                const bool deployed = o.deploy == LocoState::WalkDeploying || o.deploy == LocoState::WalkDeployed;
                if (!deployed) {
                    const int32_t co = o.d.collision_class;
                    if ((flags & 4u) && co >= kUccLowObstacle && co <= kUccHighObstacle) continue;
                    const bool open_only = (flags & 2u) || ((flags & 1u) && !hostile(u.d.owner, o.d.owner));
                    if (open_only && !shares_open_record(u, o)) continue;
                }
            } else if (rel == kRelSameWay) {
                if (!(near_path_end(o, u) && !(dot(o.m.pos - u.m.pos, fu) < 0.0f) && shares_open_record(u, o))) continue;
            }
        }
        AroundObstacle ob;
        ob.id = o.d.id;
        ob.shape = unit_swept_box(o, u, rel);
        ob.foot_hx = foot_half(o).x;
        ob.front_free = rel == kRelHeld || rel == kRelHeadOn || rel == kRelCrossing || rel == kRelSameWaySlower;
        out.push_back(ob);
        ids.push_back(ci);
    }
}

// bkm1 t07 (b0055, MOVEMENT §3.5.2 G, §4.6.2): a new order that is not planned yet (need_path) leaves the old order path as
// the stack's kind-1 record until the plan's stack reset, so it is the base too.
bool MovementSystem::around_base(const Unit& u, std::vector<V2>& scratch, std::span<const V2>& base, uint32_t& cur) const {
    if (!u.has_goal || u.m.next == 0 || u.m.next >= u.m.path.size()) return false;  // a kind-1 record
    if (!u.pushed) {
        base = u.m.path;
        cur = u.m.next - 1;
    } else {
        // s78n t23 step 0 (§1.5.1 item 2, t21 review item 4): a top record left on its last waypoint (a cut, finished at
        // the walk step) fails
        if (u.push_next == 0 || u.push_path.empty() || u.push_next >= u.push_path.size()) return false;
        scratch = u.push_path;
        const V2 dir = normalize_retail(u.m.path[u.m.next] - u.m.path[u.m.next - 1]);
        scratch.push_back(scratch.back() + dir);  // 1 unit past its end along the order path's current segment
        base = scratch;
        cur = u.push_next - 1;
    }
    return static_cast<size_t>(cur) + 1 < base.size();
}

// 0x67d580(request, object, 1) - the goal-spot filter of 0x679aa0 (s78n t24, MOVEMENT §5.4.7 "Rejoin candidates of a
// kind-3 Path Around" item 2; relation mask 0x10): never an obstacle - u, anything u crushes, a class-exempt or gated
// pair; always - u's MovingVsMoving partners; then by the relation (object, u): standing, held, head-on, crossing, same
// way slower -> an obstacle; same way not slower -> not, unless it is on its final approach, not behind u's facing and
// the pair has an undecided / detouring record. Infantry are obstacles here even for an Infantry / Giant u (the mask
// has no bit 0).
bool MovementSystem::goal_obstacle(const Unit& u, const Unit& o, const Unit* forced) const {
    if (o.d.id == u.d.id) return false;
    if (crushes(u, o) || !collision_classes_interact(u.d.collision_class, o.d.collision_class) || pair_gated(u, o)) return false;
    // s78o (b0065, MOVEMENT §5.4.7 "Rejoin candidates" item 2): the forced object (0x67d580's object argument, a flag-1
    // detour's obstacle) is an obstacle before the partner and relation tests (today only MvM detours pass flag 1, and
    // their obstacle is a MovingVsMoving partner already)
    if (forced && o.d.id == forced->d.id) return true;
    for (const CollisionObject& c : coll_.list())
        if (c.kind == CollisionKind::MovingVsMoving && ((c.a == u.d.id && c.b == o.d.id) || (c.a == o.d.id && c.b == u.d.id)))
            return true;
    if (unit_relation(o, u) != kRelSameWay) return true;
    return near_path_end(o, u) && !(dot(o.m.pos - u.m.pos, dir_from_deg(u.m.yaw)) < 0.0f) && shares_open_record(u, o);
}

// 0x679aa0: the candidate is taken at the nearest passable point Q of the unit's region within LandDestinationProximity
// (60), when the unit's rectangle placed at Q, axis along Q - E (its facing when zero), overlaps no layer object that
// the goal-spot filter counts as an obstacle (s78n t24: goal_obstacle). [port] the tracker argument of goals 2 - 11
// (the previous window's grown shapes, [M] in the spec) is not modelled: every goal queries the layer.
bool MovementSystem::rejoin_accept(const Unit& u, V2 cand, V2 path_end, V2& q, const Unit* forced) const {
    q = cand;
    const int32_t comp = regions_.component_at(u.slot, world_to_cell(u.m.pos.x), world_to_cell(u.m.pos.y));
    const int32_t cx = world_to_cell(cand.x), cy = world_to_cell(cand.y);
    if (!grid_.passable_cell(cx, cy, u.lp.mask) || (comp >= 0 && regions_.component_at(u.slot, cx, cy) != comp)) {
        const float reach = cfg_.constants.land_destination_proximity;
        if (comp < 0 || !snap_to_component(grid_, regions_, cand, u.lp.mask, comp, static_cast<int32_t>(reach / kCellSize), q)) return false;
        if (distance(q, cand) > reach) return false;
    }
    FootRect r = foot_at(u, q);
    const V2 axis = normalize_retail(q - path_end);
    if (length_sq(axis) > 0.0f) r.a = axis;
    std::vector<uint32_t> near_units;
    layer_box_query(q, foot_radius(u), near_units);
    for (uint32_t ci : near_units) {
        const Unit& o = units_[ci];
        if (o.d.id != u.d.id && foot_overlap(r, foot_of(o)) && goal_obstacle(u, o, forced)) return false;
    }
    return true;
}

bool MovementSystem::rejoin_search(const Unit& u, std::span<const V2> base, uint32_t cur, float d,
                                   const std::function<int(const Rejoin&)>& fn, const Unit* forced) const {
    // 0x675ed0: at most 11 accepted goals (0x676020 cmp r12d, 0xa); the generator itself has no count
    constexpr uint32_t kMaxGoals = 11;
    uint32_t goals = 0;
    int verdict = kRejoinNext;
    const auto offer = [&](const Rejoin& r) {
        if (goals++ >= kMaxGoals) {
            verdict = kRejoinFail;
            return true;
        }
        verdict = fn(r);
        return verdict != kRejoinNext;
    };
    const V2 end = base.back();
    // (a) along the base path, from the unit's progress + D + 2.0 in steps of 10 to the path end (no count limit)
    std::vector<RejoinPoint> pts;
    rejoin_on_path(base, cur, u.m.pos, d, UINT32_MAX, pts);
    for (const RejoinPoint& p : pts) {
        Rejoin r;
        r.on_path = !u.pushed;
        r.resume = u.pushed ? u.m.next : p.next;  // (a detour off an older collision path resumes the order path by its lines)
        if (rejoin_accept(u, p.p, end, r.q, forced) && offer(r)) return verdict == kRejoinTaken;
    }
    // 0x679e20: a land path from the end E to the spot no longer than 2 x the straight line
    LandPathScratch sc;
    const auto reachable = [&](V2 spot) {
        if (line_passable(grid_, end, spot, u.lp.mask)) return true;
        LandPathRequest q;
        q.start = end;
        q.goal = spot;
        q.mask = u.lp.mask;
        q.snap_goal = false;
        LandPath lp;
        find_land_path(grid_, regions_, q, sc, lp);
        return lp.ok && path_length(lp.points) <= 2.0f * distance(end, spot);
    };
    // (b) past the end E: toward the unit's target up to its distance, else along the last segment up to
    // LandTemporaryDestinationProximity (20), in steps of 10; each step 0x679aa0 and 0x679e20 - a failed 0x679e20 ends (b)
    V2 dir = normalize_retail(base[base.size() - 1] - base[base.size() - 2]);
    float limit = cfg_.constants.land_temporary_destination_proximity;
    if (u.ci.target >= 0 && length_sq(u.ci.target_pos - end) > 0.0f) {
        dir = normalize_retail(u.ci.target_pos - end);
        limit = distance(u.ci.target_pos, end);
    }
    if (!(length_sq(dir) > 0.0f)) dir = dir_from_deg(u.m.yaw);
    const uint32_t past = u.pushed ? u.m.next : static_cast<uint32_t>(u.m.path.size());
    for (float s = 10.0f; s <= limit; s += 10.0f) {
        const V2 spot = end + dir * s;
        Rejoin r;
        r.resume = past;
        if (!rejoin_accept(u, spot, end, r.q, forced)) continue;
        if (!reachable(spot)) break;
        if (offer(r)) return verdict == kRejoinTaken;
    }
    // (c) the ring search 0x440f90 around E (full circle from dir; occupancy margin max(6 x FormationSpacing, 0.25 r_u);
    // any layer object but u occupies): each free reachable spot not behind E along dir is a goal, in ring order (the
    // search resumes after a goal that fails); 0x679e20 only, not 0x679aa0. The rings exhausted fail the call.
    const float ru = foot_radius(u);
    const float reach = ru + std::max(6.0f * u.d.formation_spacing, 0.25f * ru);
    const int32_t comp = regions_.component_at(u.slot, world_to_cell(u.m.pos.x), world_to_cell(u.m.pos.y));
    std::vector<uint32_t> occ;
    RingSetup ring;
    ring.centre = end;
    ring.dir = dir;
    ring.r = ru;
    V2 spot;
    const bool stopped = ring_search(
        ring,
        [&](V2 p, uint32_t) {
            const int32_t cx = world_to_cell(p.x), cy = world_to_cell(p.y);
            if (!grid_.in_range(cx, cy) || !grid_.passable_cell(cx, cy, u.lp.mask)) return false;
            if (comp >= 0 && regions_.component_at(u.slot, cx, cy) != comp) return false;
            if (dot(p - end, dir) < 0.0f) return false;
            layer_box_query(p, reach + 1.0f, occ);
            for (uint32_t ci : occ)
                if (units_[ci].d.id != u.d.id && ring_blocked(p, reach, foot_of(units_[ci]))) return false;
            if (!reachable(p)) return false;
            Rejoin r;
            r.q = p;
            r.resume = past;
            return offer(r);
        },
        spot);
    return stopped && verdict == kRejoinTaken;
}

bool MovementSystem::spare_point(const Unit& a, const Unit& b, V2& v) const {
    v = V2{};
    std::vector<V2> scratch;
    std::span<const V2> base;
    uint32_t cur = 0;
    if (!around_base(a, scratch, base, cur)) return false;
    const V2 dd = a.m.pos - b.m.pos;
    const float dz = a.z - b.z;
    const float d = std::sqrt(dd.x * dd.x + dd.y * dd.y + dz * dz) + foot_radius(a) + foot_radius(b);
    return rejoin_search(a, base, cur, d, [&](const Rejoin& r) {
        v = r.q;
        return kRejoinTaken;
    });
}

// Path Around 0x5c80e0 (MOVEMENT §1.5.1). The detour goes on top of the unit's path stack; the ordered path stays
// underneath, untouched, and resumes at the first waypoint line not yet crossed when the detour ends (pop_top). The
// complexity (the vertex count of every A* pass) goes to the quota counter. [port] not modelled: the escape vertices of
// a start inside a same-way obstacle, the formation-path refusal 0x41cf40, concave terrain corners and the corner
// pruning masks. s78n t24: kind 3 (TerrainCollision, no obstacle) adds 40.0 to D and skips the last-point drop.
// ("Crosses an obstacle" = the segment meets its tracker rectangle, touching counts: §1.5.1 item 4a, path_around.cpp.)
bool MovementSystem::path_around(const CollisionObject& c, Unit& u, const Unit* o, bool flag, uint8_t kind, uint8_t flags,
                                 uint32_t frame) {
    const MovementConstants& k = cfg_.constants;
    if (!(walk_max_speed(u) > 0.0f)) return false;
    std::vector<V2> scratch;
    std::span<const V2> base;
    uint32_t cur = 0;
    if (!around_base(u, scratch, base, cur)) return false;
    // Distance D past which the detour rejoins the path
    const float ru = foot_radius(u);
    float d = ru;
    FootRect obox{};
    if (o) {
        obox = unit_swept_box(*o, u, unit_relation(*o, u));
        const float ro = foot_radius(*o);
        if (!flag) {
            V2 exit;
            if (ray_leaves_rect(obox, u.m.pos, wanted_dir(u), exit)) d = distance(exit, u.m.pos) + ru;
            else d = distance(u.m.pos, o->m.pos) + ru + ro;
        } else {
            const V2 front = obox.c + obox.a * obox.hx;
            d = std::max(distance(u.m.pos, o->m.pos), distance(front, u.m.pos)) + ru + ro;
        }
    }
    if (kind == 3) d += 40.0f;  // s78n t24 (§1.5.1 item 1, §5.4.7 item 3.5): TerrainCollision, D = r_u + 40
    // The obstacle set, once for the call: every layer object the largest window can reach (the rejoin point lies within
    // D + the candidates' reach of the unit; the window adds its bound, its growth and the retries)
    std::vector<AroundObstacle> obs;
    std::vector<uint32_t> obs_units;
    around_obstacles(u, flag ? o : nullptr, flags, d + 160.0f + 40.0f * kCellSize, obs, obs_units);
    const V2 hu = foot_half(u);
    // s78n t23 (MOVEMENT §5.4.6, record vf +0x80 0x67ee60): a MovingVsMoving record in state 4 (Pass) refuses a detour
    // whose first segment makes more than 120 degrees with u's facing; the call then fails
    const bool refuse_back = c.kind == CollisionKind::MovingVsMoving && c.state == CollisionState::Pass;
    bool searched = false, done = false;
    rejoin_search(u, base, cur, d, [&](const Rejoin& rj) {
        AroundRequest q;
        q.start = u.m.pos;
        q.goal = rj.q;
        q.mask = u.lp.mask;
        q.giant = u.d.collision_class == kUccGiantVehicle;
        q.f = std::max(u.d.box_expansion, 0.0f);
        q.grow = half_width(u) + 0.5f;
        q.tracker = std::min(hu.x, hu.y) + 0.5f + q.f;
        q.mover_hx = hu.x;
        q.obstacles = obs;
        if (height_) q.height = height_;
        AroundResult res;
        searched = true;
        const bool ok = around_search(grid_, k, q, res);
        quota_used_ += res.complexity;
        // s78n t24 (§5.4.7 "Rejoin candidates" item 3): an A* failure fails the call; an earlier failure tries the next goal
        if (!ok || res.points.size() < 2) return res.graph ? kRejoinFail : kRejoinNext;
        // Last-point drop: the last corner sees the next original waypoint N -> the rejoin point is removed
        if (kind != 3 && res.points.size() > 2 && rj.on_path && rj.resume < u.m.path.size()) {
            const V2 p = res.points[res.points.size() - 2];
            bool drop = line_passable(grid_, p, u.m.path[rj.resume], u.lp.mask);
            if (drop && flag && o) {
                const V2 front = obox.c + obox.a * obox.hx;
                drop = dot(p - (front - obox.a * (hu.x + foot_half(*o).x)), obox.a) > 60.0f;
            }
            if (drop) res.points.pop_back();
        }
        if (refuse_back && dot(normalize_retail(res.points[1] - res.points[0]), dir_from_deg(u.m.yaw)) < -0.5f)
            return kRejoinFail;  // refused: the search ends, nothing changed

        // 0x6744a0: the path's id list - every obstacle of the window that is not moving or lies behind the detour's end
        const V2 last = res.points.back();
        const V2 last_dir = normalize_retail(last - res.points[res.points.size() - 2]);
        std::vector<ObjectId> around;
        for (uint32_t wi : res.window) {
            const Unit& w = units_[obs_units[wi]];
            if (!unit_moving(w) || dot(w.m.pos - last, last_dir) < 0.0f) around.push_back(w.d.id);
        }
        (void)claim(u, c, true, frame);  // forced: an owner of higher weight is released
        const uint32_t npts = static_cast<uint32_t>(res.points.size());
        push_top(u, std::move(res.points), 2, kind == 7, c, frame);
        u.push_resume = rj.resume;
        for (const HoldTag& t : res.tags)
            if (t.index < npts) u.push_tags.push_back(t);
        u.around = std::move(around);
        u.around_frame = frame;
        done = true;
        return kRejoinTaken;
    }, flag ? o : nullptr);  // s78o (b0065): a flag-1 call's obstacle is the goal-spot filter's forced object
    if (searched) ++stats_.dynamic_repaths;
    return done;
}

bool MovementSystem::mvs_path_around(const CollisionObject& c, Unit& a, const Unit& b, uint8_t p, uint32_t frame) {
    return path_around(c, a, &b, false, 4, p, frame) || path_around(c, a, &b, false, 4, static_cast<uint8_t>(p | 4u), frame);
}

// MovingVsStopped decision 0x684f90 (MOVEMENT §5.4.1; a = mover, b = obstacle), first match wins. s78n t20 batch 1: the
// death-clone obstacle clauses of steps 2 and 8 (§5.4.5 item 2). Not modelled ([port]): step 10's units-ignoring detour
// 0x685490 (state 8) is a stand-in (see there). s78n t20: the spare point 0x5c6ba0 and the Path Around calls are §1.5.1's; §5.4.5 - the target
// clause of step 4, the hero-clash test of step 8 and the crusher's record (set_collision_info gives the target, the
// range, "short-range" and the clash state).
void MovementSystem::mvs_decide(CollisionObject& c, Unit& a, Unit& b, uint32_t frame) {
    using S = CollisionState;
    // 1. the contact test no longer holds
    const float contact = mvs_contact(a, b);
    if (contact < 0.0f) {
        c.state = S::Resolved;
        return;
    }
    const int32_t ca = a.d.collision_class, cb = b.d.collision_class;
    const bool inf_a = ca == kUccInfantry, inf_b = cb == kUccInfantry;
    const bool crush = crushes(a, b);
    // 2. a forced top path (flag 0x20: garrison exit, unload; a collision-owned record on top is not forced), against b with
    // a locomotor or a death clone (s78n t20 batch 1, MOVEMENT §5.4.5 item 2)
    if (a.forced && !a.pushed && a.has_goal && (!b.d.no_locomotor || b.d.death_clone_obstacle)) {
        if (inf_a && inf_b) c.state = S::Repulsion;
        else c.state = push_aside(c, a, b, crush, frame) ? S::Resolved : S::TryAgainLater;
        return;
    }
    // 3. the path was already planned around b after b last changed paths
    if (b.path_change < a.around_frame && std::find(a.around.begin(), a.around.end(), b.d.id) != a.around.end()) {
        c.state = S::Resolved;
        return;
    }
    // 4. tgt := a's current target is b and (a's attack range < b's footprint radius, or a is short-range). An Infantry /
    // Bike mover takes Repulsion at once against Infantry when it is no squad container and (b is none either, or tgt) -
    // or, being Infantry, whenever tgt (a melee squad closing on its target)
    const bool tgt = a.ci.target == b.d.id && (a.ci.attack_range < foot_radius(b) || a.ci.short_range);
    if ((inf_a || ca == kUccBike) && ((inf_b && !a.d.team_container && (!b.d.team_container || tgt)) || (inf_a && tgt))) {
        c.state = S::Repulsion;
        return;
    }
    // 5. a Giant Vehicle that cannot crush pushes infantry / bikes (and anything while it turns in place)
    const bool scat = crush && scatters(b);
    bool push_tried = false;
    if (ca == kUccGiantVehicle && !crush && (inf_b || cb == kUccBike || in_place_turn(a))) {  // s78n t26.2: vf +0x1b0
        push_tried = true;
        if (push_aside(c, a, b, false, frame)) {
            c.state = S::Resolved;
            return;
        }
    }
    // 6. destination blocked: the early-arrival cut, then Path Around; destination free: Repulsion for two Infantry
    // classes, else Path Around
    bool alt = false;
    if (!crush && destination_blocked(a, b)) {
        bool to_push = false;
        if (side_step_ok(a, b, c)) {
            V2 spare;
            alt = spare_point(a, b, spare);  // 0x5c6ba0: the first accepted rejoin point
            if (early_arrival(a, b, alt, spare, frame)) {
                a.coll_now = true;  // +0x84: the re-routed mover takes a lower target speed at once
                ++stats_.early_arrivals;
                c.state = S::Resolved;
                return;
            }
            to_push = !alt;  // no spare point: straight to step 8
        }
        if (!to_push && mvs_path_around(c, a, b, 0, frame)) {
            c.state = S::PathAround;
            return;
        }
    } else {
        if (inf_a && inf_b) {
            c.state = S::Repulsion;
            return;
        }
        if (!crush && mvs_path_around(c, a, b, 0, frame)) {
            c.state = S::PathAround;
            return;
        }
    }
    // 8. a non-hostile blocker (or one that scatters from the crusher) is pushed - not by a single infantry mover, nor by
    // a bike against infantry, and never a blocker in a hero clash (+0x3b4; only this step tests it). s78n t20 batch 1
    // (MOVEMENT §5.4.5 item 2): any mover pushes a death clone (hostile or not, single infantry included) - it is killed
    const bool friendly = !hostile(a.d.owner, b.d.owner) || scat;
    const bool shove_ok = friendly && !(inf_a && !a.d.team_container) && !(ca == kUccBike && inf_b);
    if ((shove_ok || b.d.death_clone_obstacle) && (!crush || scat) && !b.ci.hero_clash && !push_tried &&
        push_aside(c, a, b, crush, frame)) {
        c.state = S::Resolved;
        return;
    }
    // 9. the crusher drives on (one push attempt per record)
    if (crush) {
        c.state = S::Crusher;
        return;
    }
    // 10. a pair with a class above Bike: a second Path Around, else 0x685490 - a detour that ignores units (state 8,
    // Resolved at the next service). [port] The mover keeps its path and b counts as planned around (step 3) until b
    // changes paths: it walks on through b's footprint; the attack block retail gives the mover's formation on a hostile
    // unit in the way is not issued.
    if (ca > kUccBike || cb > kUccBike) {
        if (alt && mvs_path_around(c, a, b, 1, frame)) {  // 0x685920(a, b, 1): standing friends are no obstacles
            c.state = S::PathAround;
            return;
        }
        // s78n t18 review: only b is stamped with this frame. The list has one stamp, so an older entry that step 3 no
        // longer accepts (its unit changed paths at or after the old stamp, or is gone) is dropped first - the new stamp
        // would make it count as planned around again.
        std::erase_if(a.around, [&](ObjectId id) {
            if (id == b.d.id) return false;
            const Unit* o = unit_ptr(id);
            return !o || !(o->path_change < a.around_frame);
        });
        if (std::find(a.around.begin(), a.around.end(), b.d.id) == a.around.end()) a.around.push_back(b.d.id);
        a.around_frame = frame;
        c.state = S::Resolved;
        return;
    }
    // 11.
    c.state = S::TryAgainLater;
}

// Per-state service 0x684cf0 (every frame) and the speed answer 0x684660.
void MovementSystem::mvs_service(CollisionObject& c, Unit& a, Unit& b, bool full, uint32_t frame) {
    using S = CollisionState;
    // s78n t23 step 0 (MOVEMENT §5.4.1 driver item 2, t21 review item 5): a state-0 record without quota is not serviced
    // (a state-0 MvS service does nothing - no validity either); it answers max speed meanwhile
    if (c.state == S::Active && !full) {
        ++stats_.quota_deferrals;
        return;
    }
    // s78n t20 (MOVEMENT §5.4.1 "The impassable-cell rule in full" item 1; validity, first in every state): a first, b
    // only when a passed; the unit on an impassable cell is stopped and the record ends (6)
    if (Unit* const s = on_blocked_cell(a) ? &a : on_blocked_cell(b) ? &b : nullptr) {
        collision_stop(*s, c, frame);
        c.state = S::Resolved;
        return;
    }
    if (c.state == S::TryAgainLater) {
        if (mvs_behind(a, b)) {
            c.state = S::Resolved;
            return;
        }
        if (frame >= c.retry) {  // re-decided at most every 30 frames (the first time at once)
            c.retry = frame + 30;
            c.state = S::Active;
        }
    }
    if (c.state == S::Active) {
        if (!full) {  // a retry that came due without quota: decided on a later frame
            ++stats_.quota_deferrals;
            return;
        }
        mvs_decide(c, a, b, frame);
    }
    switch (c.state) {
        case S::PathAround:  // a no longer holds the path made for this record (vf +0x1c0(record id))
            if (!holds_path(a, c)) c.state = S::Resolved;
            else if (!crushes(a, b)) c.speed_a = follow_speed(a, c);  // s78n t20: 0x684660 state 1 -> 0x5c69f0
            break;
        case S::Repulsion:  // the obstacle is behind, or the mover has no path left; the contact is not re-tested
            if (mvs_behind(a, b) || !(a.has_goal || a.pushed)) c.state = S::Resolved;
            break;
        case S::Crusher:  // never re-decided: ends when the contact test fails
            if (mvs_contact(a, b) < 0.0f) c.state = S::Resolved;
            break;
        case S::TryAgainLater:  // speed 0 with no "at once" flag: the mover brakes at its rate
            if (!crushes(a, b)) c.speed_a = 0.0f;
            break;
        default: break;
    }
}

// ---- s78n t23 (MOVEMENT §5.4.2 / §5.4.6): MovingVsMoving for every pair ----

// vf +0x1f8 as the chooser reads it: the unit's facing, or its wanted facing while it turns in place.
V2 MovementSystem::mvm_facing(const Unit& u) const {
    if (u.m.state == LocoState::WalkStartTurn) return wanted_dir(u);
    if (u.aturn != LocoState::None) return dir_from_deg(u.turn_to);
    if (u.fturn) return dir_from_deg(land_wanted_yaw(u));
    return dir_from_deg(u.m.yaw);
}

// 0x680510 (§5.4.6 state 1): the last hold tag of u's top path naming `obstacle` whose waypoint u has not passed yet.
// s78n t23 (t21 review item 9): a same-frame cut may leave push_next at the path size - such tags are not pending.
const HoldTag* MovementSystem::pending_tag(const Unit& u, ObjectId obstacle) const {
    if (!u.pushed) return nullptr;
    const HoldTag* r = nullptr;
    for (const HoldTag& t : u.push_tags)
        if (t.obstacle == obstacle && t.index >= u.push_next && t.index < u.push_path.size()) r = &t;
    return r;
}

// 0x372fe0 (MOVEMENT §3.4): the unit's own type's acceleration per frame, OverrideAcceleration scaled like the speed
// (x the mode multiplier x the speed factor, Unit::lp.accel); not set (<= 0) -> 0.05, not the locomotor's "full speed
// in one frame". A squad container reads its own type too (s78n t23 review fix 3: not the members' rate 0x5ab540).
float MovementSystem::accel_per_frame(const Unit& u) const {
    return u.d.accel > 0.0f && u.lp.accel > 0.0f ? u.lp.accel : 0.05f;
}

// s78n t26 closing review (MOVEMENT §3.4 "Units with Walk_Transition" [H], §3.5.2 F): the clip data of a Walk_Transition
// Walk / Bike unit from the model (set_deploy_clip_fn's hook: frames, fps; [M] variant 0 - the hook answers the drawn
// variant once that clip plays). Refreshed each land tick before the plan and the walk (not saved: derived from the
// model); no hook -> off (the unit keeps the OverrideAcceleration / OverrideDeceleration getters).
void MovementSystem::walk_transition_fill(Unit& u) const {
    u.wt = WalkTransition{};
    if (!u.d.walk_transition || !deploy_clip_ || cfg_.mode != MoveMode::Land) return;
    if (u.d.locomotor != LocomotorKind::Default && u.d.locomotor != LocomotorKind::Bike) return;
    u.wt.on = true;
    u.wt.states = u.d.locomotor == LocomotorKind::Default;
    u.wt.s = u.d.anim_scale_speed;
    for (size_t i = 0; i < 6; ++i) {
        uint32_t n = 0;
        float fps = 0.0f;
        if (deploy_clip_(u.d.id, kWtClips[i], n, fps) && fps > 0.0f) {
            u.wt.count[i] = static_cast<float>(n);
            u.wt.step[i] = fps / 30.0f;
        }
    }
}

// vf +0x2e0: the Walk deceleration - a Walk_Transition unit's from its MOVE_ENDONE clip (V = `vmax`), else lp.decel.
float MovementSystem::walk_decel(const Unit& u, float vmax) const {
    return u.wt.on ? walk_transition_decel(u.wt, vmax, u.m.speed, u.m.wt_f0) : u.lp.decel;
}

namespace bo3split {

bool infantry_class(int32_t k) { return k == kUccInfantry || k == kUccBike; }
}  // namespace bo3split

// Chooser 0x67f7b0 (MOVEMENT §5.4.2 steps 1 - 7 for two Infantry / Bike movers, §5.4.6 for every other pair). p0 / p1 =
// the record's participants in list order (a, b); the result starts as M = p0, mode 0, swap_ok 1. `rerun`: the re-runs
// of states 2 and 6 (the relation code's own-record clause is false). [port] pack +0x110 (the corridor-squeeze layout
// index) is always 0; the head-on "formation extent" is the formation's member count (the port keeps no extent); the
// overtake test's object flag +0x3a0 bit 2 and same-coordinator exception are not modelled.
MovementSystem::MvmChoice MovementSystem::mvm_choose(const CollisionObject& c, const Unit& p0, const Unit& p1, bool rerun) const {
    using S = CollisionState;
    MvmChoice r;
    // 1. the predictor 0x442540 fails
    if (!mvm_contact(p0, p1)) {
        r.state = S::Resolved;
        return r;
    }
    // 2. starvation (state 2's countdown ran out): the starved unit becomes M of state 15
    if (c.starve == 0 && c.starve_next != 2) {
        r.state = S::Starved;
        r.master = c.starve_next;
        return r;
    }
    const V2 f0 = mvm_facing(p0), f1 = mvm_facing(p1);
    const float ahead0 = dot(p1.m.pos - p0.m.pos, f0), ahead1 = dot(p0.m.pos - p1.m.pos, f1);
    const bool inf = infantry_class(p0.d.collision_class) && infantry_class(p1.d.collision_class);
    const uint8_t farther = ahead0 < ahead1 ? uint8_t{0} : uint8_t{1};  // the one farther ahead
    // 3. same formation: the unit with the front slot behind the other -> 11 (Infantry / Bike), 13 (interchangeable:
    // 0x67ea40), else Pass (pack +0x110 = 0, mode 1), M = the front-slot unit; otherwise Speed Slave Only
    if (p0.form_id != 0 && p0.form_id == p1.form_id) {
        const float s0 = p0.form_slot.x, s1 = p1.form_slot.x;
        int32_t f = -1;
        if (s1 > s0 && ahead0 < -0.001f) f = 1;
        else if (s1 < s0 && ahead1 < -0.001f) f = 0;
        if (f < 0) {
            r.state = S::SpeedSlave;
            r.master = farther;
            return r;
        }
        r.master = static_cast<uint8_t>(f);
        const Unit& fu = f == 0 ? p0 : p1;
        const Unit& bu = f == 0 ? p1 : p0;
        if (inf) {
            r.state = S::FormationFollow;
        } else {
            const V2 hf = foot_half(fu), hb = foot_half(bu);
            const float tol = 10.0f * fu.d.formation_spacing;
            const bool same_type = !fu.d.type_name.empty() && fu.d.type_name == bu.d.type_name;
            const bool alike = fu.d.formation_order == bu.d.formation_order && std::fabs(hf.x - hb.x) <= tol &&
                               std::fabs(hf.y - hb.y) <= tol;
            if (same_type || alike) {
                r.state = S::SwapSlots;
            } else {
                r.state = S::Pass;
                r.mode = 1;
            }
        }
        return r;
    }
    // 4. exactly one on a forced top path: the other detours, never swapped
    const bool fz0 = p0.forced && !p0.pushed && p0.has_goal, fz1 = p1.forced && !p1.pushed && p1.has_goal;
    if (fz0 != fz1) {
        r.state = S::PathAround;
        r.master = fz0 ? uint8_t{0} : uint8_t{1};
        r.mode = 2;
        r.swap_ok = false;
        return r;
    }
    // 5. the relation code 0x679750(p0, p1)
    const uint8_t rel = unit_relation(p0, p1, !rerun);
    if (inf) {  // §5.4.2 step 7: headings within 60 degrees -> Speed Slave Only, else Repulse (M = p1)
        if (rel == kRelSameWay || rel == kRelSameWaySlower) {
            r.state = S::SpeedSlave;
            r.master = farther;
        } else {
            r.state = S::Repulsion;
            r.master = 1;
        }
        return r;
    }
    const bool c0 = crushes(p0, p1), sc0 = c0 && scatters(p1);
    const bool c1 = crushes(p1, p0), sc1 = c1 && scatters(p0);
    // 6. same direction: the rear one that cannot crush the front one passes it (overtake test) or follows it (6)
    if (dot(f0, f1) > 0.0f) {
        if (ahead0 < -0.001f) {  // p1 is the rear one; with c1 the Giant step follows (the other order is not tried)
            if (!c1) {
                if (mvm_overtake(p1, p0)) {
                    r.state = S::Pass;
                    r.master = 1;
                    r.mode = 1;
                } else {
                    r.state = S::MovingWithSpeed;
                    r.master = 0;
                }
                return r;
            }
        } else if (ahead1 < -0.001f && !c0) {
            if (mvm_overtake(p0, p1)) {
                r.state = S::Pass;
                r.master = 0;
                r.mode = 1;
            } else {
                r.state = S::MovingWithSpeed;
                r.master = 1;
            }
            return r;
        }
    }
    // 7. a Giant against a non-Giant it cannot crush, or one that scatters: the other detours, never swapped
    const int32_t k0 = p0.d.collision_class, k1 = p1.d.collision_class;
    if (k0 == kUccGiantVehicle && k1 != kUccGiantVehicle && (!c0 || sc0)) {
        r.state = S::PathAround;
        r.master = 0;
        r.mode = 2;
        r.swap_ok = false;
        return r;
    }
    if (k1 == kUccGiantVehicle && k0 != kUccGiantVehicle && (!c1 || sc1)) {
        r.state = S::PathAround;
        r.master = 1;
        r.mode = 2;
        r.swap_ok = false;
        return r;
    }
    // 8. by the relation code
    switch (rel) {
        case kRelStanding:
        case kRelHeld:  // p0 standing or held: p0 detours (both run on when p1 can crush p0)
            r.master = 1;
            if (c1) {
                r.state = S::Crusher;
            } else {
                r.state = S::PathAround;
                r.mode = 2;
                r.swap_ok = !c0;
            }
            return r;
        case kRelHeadOn: {
            if (c0 && !sc0) {
                r.state = S::Crusher;
                r.master = 0;
                return r;
            }
            if (c1 && !sc1) {
                r.state = S::Crusher;
                r.master = 1;
                return r;
            }
            r.state = S::PathAround;
            r.mode = 2;
            r.swap_ok = !c0 && !c1;
            // M = the higher class; equal class, both in formations: the larger formation (member count), equal -> p0
            // unless formation 0's first member id is lower (then p1); else the lower max speed (tie p0)
            if (k0 != k1) {
                r.master = k0 > k1 ? uint8_t{0} : uint8_t{1};
            } else if (p0.form_id != 0 && p1.form_id != 0) {
                uint32_t n0 = 0, n1 = 0;
                ObjectId l0 = std::numeric_limits<ObjectId>::max(), l1 = std::numeric_limits<ObjectId>::max();
                for (const Unit& x : units_) {
                    if (x.form_id == p0.form_id) {
                        ++n0;
                        l0 = std::min(l0, x.d.id);
                    } else if (x.form_id == p1.form_id) {
                        ++n1;
                        l1 = std::min(l1, x.d.id);
                    }
                }
                if (n0 != n1) r.master = n0 > n1 ? uint8_t{0} : uint8_t{1};
                else r.master = l0 < l1 ? uint8_t{1} : uint8_t{0};
            } else {
                r.master = walk_max_speed(p1) < walk_max_speed(p0) ? uint8_t{1} : uint8_t{0};
            }
            return r;
        }
        case kRelCrossing:
            if (c0 && sc0) {
                r.state = S::PathAround;
                r.master = 0;
                r.mode = 2;
                r.swap_ok = false;
            } else if (c0) {
                if (c1 && sc1) {
                    r.state = S::PathAround;
                    r.master = 1;
                    r.mode = 2;
                    r.swap_ok = false;
                } else {
                    r.state = S::Crusher;
                    r.master = 0;
                }
            } else if (c1) {
                if (sc1) {
                    r.state = S::PathAround;
                    r.master = 1;
                    r.mode = 2;
                    r.swap_ok = false;
                } else {
                    r.state = S::Crusher;
                    r.master = 0;
                }
            } else {  // the one less far ahead detours
                r.state = S::PathAround;
                r.mode = 2;
                r.swap_ok = true;
                r.master = ahead0 > ahead1 ? uint8_t{1} : uint8_t{0};
            }
            return r;
        case kRelSameWay:
        case kRelSameWaySlower:  // headings within 60 degrees (step 6 did not decide): slave the rear one
            r.state = S::MovingWithSpeed;
            r.master = ahead1 > ahead0 ? uint8_t{0} : uint8_t{1};
            return r;
        default:
            r.state = S::Active;  // decided again next frame
            return r;
    }
}

// Overtake test 0x683260(a = rear, b = front): a can get past b, with b slowed to vs, before its own path ends.
bool MovementSystem::mvm_overtake(const Unit& a, const Unit& b) const {
    if (a.d.collision_class == kUccGiantVehicle) return false;
    const auto speed = [this](const Unit& x) { return x.in_formation && x.formation_speed < kInf ? x.formation_speed : walk_max_speed(x); };
    const float va = speed(a), vb = speed(b);
    if (!(vb <= va)) return false;
    const float vs = std::min(vb, 0.5f * va);
    if (va == 0.0f || va - vs == 0.0f || near_path_end(a, b)) return false;
    std::vector<V2> pts;
    stack_points(a, pts);
    const float d = path_length(pts);
    const V2 dd = a.m.pos - b.m.pos;
    const float dz = a.z - b.z;
    const float dist = std::sqrt(dd.x * dd.x + dd.y * dd.y + dz * dz);
    return (steer_radius(a) + steer_radius(b) + dist) / (va - vs) <= d / va;
}

// 0x680c00: Try Again Later with a retry frame NOW + 30 + one synced draw in [0, 15].
void MovementSystem::mvm_fail(CollisionObject& c, uint32_t frame, SimRng& rng) {
    c.state = CollisionState::TryAgainLater;
    ++c.failures;
    c.retry = frame + 30u + static_cast<uint32_t>(rng.range_int(0, 15));
}

// 0x682e80 (mode 1): M path-arounds S (flag 1, kind 4; the record is in state 4 already, so a backward detour is
// refused), then the two hold lines from S's swept box: A = its rear centre + E a, B = its front centre - E a,
// E = hx_S + hx_M.
bool MovementSystem::mvm_pass_setup(CollisionObject& c, Unit& m, const Unit& s, uint32_t frame) {
    if (!path_around(c, m, &s, true, 4, 0, frame)) return false;
    const FootRect box = unit_swept_box(s, m, unit_relation(s, m));
    const float e = foot_half(s).x + foot_half(m).x;
    c.line_a = box.c - box.a * box.hx + box.a * e;
    c.line_b = box.c + box.a * box.hx - box.a * e;
    c.line_dir = box.a;
    return true;
}

// 0x683140 (mode 2), one attempt: S path-arounds M (kind 7 when M can crush S and S scatters, else 4); the early-pop
// gate +0xbc := rel(M, S) is 4, 8 or 0x10 after it.
bool MovementSystem::mvm_detour(CollisionObject& c, Unit& m, Unit& s, bool flag, uint32_t frame) {
    const uint8_t kind = crushes(m, s) && scatters(s) ? uint8_t{7} : uint8_t{4};
    if (!path_around(c, s, &m, flag, kind, 0, frame)) return false;
    const uint8_t rel = unit_relation(m, s);
    c.early_pop = rel == kRelHeadOn || rel == kRelCrossing || rel == kRelSameWay;
    return true;
}

// Decision 0x682d00 (state 0, under quota). False: no service this frame.
bool MovementSystem::mvm_decide(CollisionObject& c, Unit& a, Unit& b, uint32_t frame, SimRng& rng) {
    using S = CollisionState;
    const MvmChoice ch = mvm_choose(c, a, b, false);
    c.state = ch.state;
    c.set_master(ch.master);  // s78n t24: the role slots' answers follow the roles (RE f8afef8d)
    if (ch.state == S::Active || ch.state == S::Resolved) return false;
    Unit& m = c.master == 0 ? a : b;
    Unit& s = c.master == 0 ? b : a;
    if (ch.mode == 1) {  // Pass: failure -> 0x680c00, M and S swap, state 12, serviced at once
        if (mvm_pass_setup(c, m, s, frame)) return true;
        mvm_fail(c, frame, rng);
        c.swap_roles();
        c.state = S::PassFailed;
        return true;
    }
    if (ch.mode == 2) {  // Path Around: S around M; then swapped, swapped back with flag 1, swapped with flag 1
        // s78n t23 (MOVEMENT §5.4.1 "The stamp in full" item 3): the roles (+0x7c / +0x80) are swapped before each
        // retry, so every detour lands on the record's S of that moment
        if (mvm_detour(c, m, s, false, frame)) return true;
        if (!ch.swap_ok) {
            c.state = S::Crusher;
            return true;
        }
        c.swap_roles();
        if (mvm_detour(c, s, m, false, frame)) return true;
        c.swap_roles();
        if (mvm_detour(c, m, s, true, frame)) return true;
        c.swap_roles();
        if (mvm_detour(c, s, m, true, frame)) return true;
        mvm_fail(c, frame, rng);  // all four failed: the roles are left swapped, no service this frame
        return false;
    }
    return true;
}

// Validity 0x680650 (start of the service): a participant without a locomotor -> 9; a participant whose top path is
// not forced on an impassable cell (a first, b only when a passed) is stopped -> 9; either one not Moving -> 9.
bool MovementSystem::mvm_valid(CollisionObject& c, Unit& a, Unit& b, uint32_t frame) {
    if (a.d.no_locomotor || b.d.no_locomotor) {
        c.state = CollisionState::Resolved;
        return false;
    }
    if (Unit* const s = on_blocked_cell(a) ? &a : on_blocked_cell(b) ? &b : nullptr) {
        collision_stop(*s, c, frame);
        c.state = CollisionState::Resolved;
        return false;
    }
    if (!unit_moving(a) || !unit_moving(b)) {
        c.state = CollisionState::Resolved;
        return false;
    }
    return true;
}

// Early detour pop 0x6807e0 (state 1 with +0xbc): S is behind M by hx_S + hx_M, S's path owner is none or this record
// and the rectangle from S to its detour's last point (S's half extents; M and S excluded) meets no layer object S has
// a collision record with. Then S's collision-owned path is removed: back on its ordered path at once. [port] (M:
// the query's field roles) the rectangle is S's footprint stretched along that segment.
bool MovementSystem::mvm_early_pop(CollisionObject& c, const Unit& m, Unit& s, uint32_t frame) {
    const V2 hs = foot_half(s);
    if (!(dot(s.m.pos - m.m.pos, mvm_facing(m)) < -(hs.x + foot_half(m).x))) return false;
    if (!s.pushed || s.push_path.empty() || !(s.push_owner == kNoDetour || s.push_owner == c.seq)) return false;
    const V2 e = s.push_path.back();
    FootRect r = foot_of(s);
    const V2 d = e - s.m.pos;
    const float len = length(d);
    if (len > 0.0f) r.a = d * (1.0f / len);
    r.c = s.m.pos + d * 0.5f;
    r.hx = hs.x + 0.5f * len;
    r.hy = hs.y;
    r.round = false;
    std::vector<uint32_t> cand;
    layer_box_query(r.c, r.hx + r.hy, cand);
    for (uint32_t ci : cand) {
        const Unit& o = units_[ci];
        if (o.d.id == s.d.id || o.d.id == m.d.id || !foot_overlap(r, foot_of(o))) continue;
        for (const CollisionObject& k : coll_.list())
            if ((k.a == s.d.id && k.b == o.d.id) || (k.a == o.d.id && k.b == s.d.id)) return false;
    }
    if (pop_top(s, true, frame)) {  // the order path has no waypoint left: arrived where it stands
        s.has_goal = false;
        s.arrived = true;
        s.exit_host = -1;
        land_arrival_turn(s);
        arrival_turn_rest(s);
        brake_to_stop(s, frame);
    }
    return true;
}

// Starvation 0x681d30 (state 2, after 0x67eb90). True: the countdown has run out (state 0; the chooser gives 15).
bool MovementSystem::mvm_starve(CollisionObject& c, const Unit& m, const Unit& s) const {
    if (c.starve == 0) return true;
    c.starve_next = 2;
    const float am = accel_per_frame(m), as = accel_per_frame(s);
    const float vm = c.master == 0 ? c.speed_a : c.speed_b, vs = c.master == 0 ? c.speed_b : c.speed_a;
    if (vm < am && record_cap(s) < as) {
        --c.starve;
        c.starve_next = c.master;
    } else if (vs < as && record_cap(m) < am) {
        --c.starve;
        c.starve_next = static_cast<uint8_t>(1u - c.master);
    }
    return false;
}

// State 13 (0x554660): the two members swap their formation slots. [port] the goals, slots and exact slot points are
// swapped and both re-plan from the next frame.
void MovementSystem::mvm_swap_slots(Unit& f, Unit& b, uint32_t frame) {
    std::swap(f.goal, b.goal);
    std::swap(f.form_slot, b.form_slot);
    std::swap(f.self_goal, b.self_goal);
    for (Unit* u : {&f, &b}) {
        if (!u->has_goal) continue;
        u->need_path = true;
        u->start_frame = frame + 1;
    }
}

// MovingVsMoving service 0x6814f0 (every frame; the decision 0x682d00 in state 0 under quota).
void MovementSystem::mvm_service(CollisionObject& c, Unit& a, Unit& b, bool full, uint32_t frame, SimRng& rng) {
    using S = CollisionState;
    // the re-decide stamp (§5.4.1 "The stamp in full" item 5): each participant gives up the record id (the detour
    // stays on its stack), state 0, and case 0 runs at once - no decision in this frame (s78n t23 review fix 2: the
    // next frame decides, a set-up under quota)
    bool fired = false;
    if (c.redecide != 0 && c.redecide <= frame) {
        c.redecide = 0;
        for (Unit* u : {&a, &b}) {
            if (u->pushed && u->push_owner == c.seq) {
                u->push_owner = kNoDetour;
                u->push_weight = 0;
            }
        }
        c.state = S::Active;
        fired = true;
    }
    if (c.state == S::Active && (!full || fired)) {  // case 0: M = p0, speeds 0, both flags 1 (the answer is max)
        if (!fired) ++stats_.quota_deferrals;
        c.master = 0;
        c.speed_a = c.speed_b = 0.0f;
        c.now_a = c.now_b = true;
        return;
    }
    if (!mvm_valid(c, a, b, frame)) return;
    if (c.state == S::Active && !mvm_decide(c, a, b, frame, rng)) return;
    Unit& m = c.master == 0 ? a : b;
    Unit& s = c.master == 0 ? b : a;
    float& vm = c.master == 0 ? c.speed_a : c.speed_b;
    float& vs = c.master == 0 ? c.speed_b : c.speed_a;
    bool& nm = c.master == 0 ? c.now_a : c.now_b;
    bool& ns = c.master == 0 ? c.now_b : c.now_a;
    const auto reset = [&] {  // 0x67f080
        c.speed_a = c.speed_b = kInf;
        c.now_a = c.now_b = false;
    };
    bool touch = false;
    switch (c.state) {
        case S::PathAround: {  // 1: S walks its detour around M; M stops at once on its hold line
            reset();
            if (c.early_pop && mvm_early_pop(c, m, s, frame)) {
                c.state = S::Resolved;
                break;
            }
            const HoldTag* tag = pending_tag(s, m.d.id);
            if (!tag && !holds_path(s, c)) {
                c.state = S::Resolved;
                break;
            }
            const bool mc = crushes(m, s);
            if (!mc) {
                vm = slave_speed(m, s, touch);
                if (touch) nm = true;
            }
            vs = follow_speed(s, c);
            if (tag && !mc && dot(m.m.pos - tag->pt, tag->dir) >= -0.001f) {
                vm = 0.0f;
                nm = true;
            }
            break;
        }
        case S::SpeedSlave: {  // 2: the chooser must give 2 again (roles not refreshed)
            // s78n t23 follow-up (MOVEMENT §5.4.6 "State 2 service", RE 2): the re-run runs before the reset, with the state
            // still 2, so record_cap folds this record's own stored answer for p0 (no self-exclusion: retail). Stored 0 (the
            // constructor, case 0) -> a relation-path pair of Infantry / Bike movers bails to state 0 writing nothing and
            // re-decides every frame (answered max speed); only the same-formation state 2 (decided before the relation)
            // slaves the rear unit. Not "fixed", as retail.
            if (mvm_choose(c, a, b, true).state != S::SpeedSlave) {
                c.state = S::Active;
                break;
            }
            reset();
            vs = slave_speed(s, m, touch);
            if (touch || vs == 0.0f) ns = true;
            if (mvm_passed(a, b)) c.state = S::Resolved;
            else if (mvm_starve(c, m, s)) c.state = S::Active;
            break;
        }
        case S::Repulsion:  // 3: both max speed, the flags keep their value
            c.speed_a = c.speed_b = kInf;
            if (mvm_passed(a, b)) c.state = S::Resolved;
            break;
        case S::Pass: {  // 4: M passes S; S runs free up to line A, slows, and stops at once at line B
            reset();
            const HoldTag* tag = pending_tag(m, s.d.id);
            if ((!tag && !holds_path(m, c)) ||
                dot(s.m.pos - m.m.pos, dir_from_deg(m.m.yaw)) <= -(foot_radius(m) + foot_radius(s))) {
                c.state = S::Resolved;
                break;
            }
            const bool sc = crushes(s, m);
            if (sc || dot(s.m.pos - c.line_a, c.line_dir) < -0.001f) {
                vs = kInf;
            } else if (dot(s.m.pos - c.line_b, c.line_dir) >= -0.001f) {
                vs = 0.0f;
                ns = true;
            } else {
                vs = std::min(closing_speed(s, m, touch), 0.5f * m.m.speed);
                if (touch) ns = true;
            }
            if (tag && !sc && dot(s.m.pos - tag->pt, tag->dir) >= -0.001f) {
                vs = 0.0f;
                ns = true;
            }
            break;
        }
        case S::MovingWithSpeed:  // 6: the chooser must give 6 again (roles not refreshed); S follows M
            // s78n t23 follow-up (MOVEMENT §5.4.6 "State 2 service" item 5, 0x681939): case 6 resets before its re-run, so
            // the re-run reads both answers as max speeds and a bail leaves (max, max, flags 0)
            reset();
            if (mvm_choose(c, a, b, true).state != S::MovingWithSpeed) {
                c.state = S::Active;
                break;
            }
            if (!crushes(s, m)) {
                vs = closing_speed(s, m, touch);
                if (touch) ns = true;
            }
            if (!mvm_contact(m, s)) c.state = S::Resolved;
            break;
        case S::Crusher:  // 7: both max speed, validity only
            reset();
            break;
        case S::TryAgainLater: {  // 10: both brake to 0, except the one behind the other runs; retry -> state 0
            reset();
            vm = vs = 0.0f;
            if (dot(m.m.pos - s.m.pos, mvm_facing(s)) < -0.001f) vs = kInf;
            else if (dot(s.m.pos - m.m.pos, mvm_facing(m)) < -0.001f) vm = kInf;
            if (c.retry <= frame) {
                c.state = S::Active;
                c.retry = frame + 30u + static_cast<uint32_t>(rng.range_int(0, 15));
            }
            break;
        }
        case S::FormationFollow:  // 11: no reset; S follows M until it has fallen back behind it
            vm = kInf;
            vs = closing_speed(s, m, touch);
            if (touch) ns = true;
            if (dot(s.m.pos - m.m.pos, mvm_facing(m)) < -(foot_radius(s) + foot_radius(m))) c.state = S::Resolved;
            break;
        case S::PassFailed:  // 12: S follows M; retry -> state 0
            reset();
            vs = closing_speed(s, m, touch);
            if (touch) ns = true;
            if (c.retry <= frame) {
                c.retry = frame + 30u + static_cast<uint32_t>(rng.range_int(0, 15));
                c.state = S::Active;
            } else if (!mvm_contact(m, s)) {
                c.state = S::Resolved;
            }
            break;
        case S::SwapSlots:  // 13
            mvm_swap_slots(m, s, frame);
            c.state = S::Resolved;
            break;
        case S::Starved: {  // 15: M stops; while M has exactly one record it path-arounds S
            if (c.starve == 0) c.starve = 30;
            reset();
            vm = 0.0f;
            uint32_t n = 0;
            for (const CollisionObject& k : coll_.list())
                if (k.a == m.d.id || k.b == m.d.id) ++n;
            if (n == 1 && path_around(c, m, &s, dot(wanted_dir(m), wanted_dir(s)) > 0.0f, 4, 0, frame)) c.state = S::StarvedDetour;
            break;
        }
        case S::StarvedDetour:  // 16: M walks its detour, S stands
            reset();
            vs = 0.0f;
            if (!pending_tag(m, s.d.id) && !holds_path(m, c)) c.state = S::Resolved;
            break;
        default: break;
    }
}

// Push 0x6859a0 (A1_READINGS §36.4, §40): direction = a's heading turned 45 degrees toward b's side; the free-position
// search (push_spot) and a land path to the spot; b walks it as a pushed move over its order move (§40.3: no return
// move). A deploying / deployed b is not pushed when its Max_Dist_Walk_While_Deployed (M) is <= 0 (s78n t26.1, MOVEMENT
// §3.2.1 item 5: a deployed Destroyer Droid is pushed and walks the push deployed). [port] A b already on a pushed move
// counts as pushed (the original
// pushes a blocker that is moving again, decision 0x684f90 step 2).
bool MovementSystem::push_aside(const CollisionObject& c, const Unit& a, Unit& b, bool flee, uint32_t frame) {
    // s78n t20 batch 1 (MOVEMENT §5.4.5 item 2, 0x6859a0 first test): a b without a locomotor is not pushed - a death-clone
    // obstacle is killed (0x391310(b, 1), the normal death: the host does it after the tick, push_kills) and the push
    // succeeds; any other locomotor-less b fails the push. No spot search, no path.
    if (b.d.no_locomotor) {
        if (!b.d.death_clone_obstacle) return false;
        if (std::find(push_kills_.begin(), push_kills_.end(), b.d.id) == push_kills_.end()) push_kills_.push_back(b.d.id);
        return true;
    }
    // [port] a unit still walking the path this record pushed it on keeps it, path kind (crush-flee or not) included.
    // (s78n t20: a push by another record replaces the older collision path - the claim is forced, A1_READINGS §40.3.)
    if (holds_path(b, c)) return true;
    if (b.anchor >= 0) return false;  // s78n t03 [port]: a steering member is not pushed (it steers back to its slot)
    if (!(b.lp.max_speed > 0.0f) || deploy_immobile(b)) {
        ++stats_.push_failures;  // s78n t18: an attempt on a blocker that cannot walk counts as a failed push too
        return false;
    }
    std::vector<V2> scratch;
    const StackPoly sp = stack_poly(a, scratch);  // s78n t20: the pusher's heading along the top of its path stack
    if (sp.next >= sp.pts.size()) return false;
    const V2 h = normalize_retail(sp.pts[sp.next] - a.m.pos);
    if (!(length_sq(h) > 0.0f)) return false;
    constexpr float kCos45 = 0.70710678f;
    const V2 rel = b.m.pos - a.m.pos;
    const float sin45 = cross(h, rel) <= 0.0f ? -kCos45 : kCos45;  // -45 degrees when b is on a's right
    const V2 dir{h.x * kCos45 - h.y * sin45, h.x * sin45 + h.y * kCos45};
    V2 spot;
    LandPath out;
    if (push_spot(b, dir, 0.78539819f, 0.33f, spot)) {  // the push's cone pi / 4 and outside radius 0.33
        // 0x67b730: b's land path to the spot (synchronous); no path fails the push.
        LandPathRequest q;
        q.start = b.m.pos;
        q.goal = spot;
        q.mask = b.lp.mask;
        // s78n t14 part B: a squad container plans without clearance (as plan_land)
        q.clearance_radius = b.d.radius > kCellSize && !b.d.team_container ? b.d.radius : 0.0f;
        LandPathScratch sc;
        find_land_path(grid_, regions_, q, sc, out);
    }
    if (!out.ok || out.points.size() < 2) {
        ++stats_.push_failures;
        return false;
    }
    // Ownership (A1_READINGS §40.3): the claim is forced - an owner of higher weight is released. The path is the top
    // record of b's stack: kind 7 (0x5c8900 / 0x5c8890 by the caller) when `flee`, else kind 4.
    (void)claim(b, c, true, frame);
    push_top(b, std::move(out.points), 1, flee, c, frame);
    ++stats_.pushes;
    return true;
}

// Free-position search 0x440f90 around b itself (A1_READINGS §40.1-40.2, path_around.hpp ring_search): rings from radius
// 0 (step 1, +0.8 per ring up to min(1.5 r, 10), at most 20 rings), n = trunc(R 2pi / r) + 1 samples (at least 4 after
// ring 0) at theta = 2pi i / n counter-clockwise from `dir`; only theta <= `cone` lies on the ring, the other samples at
// `outside` from b. The first sample that is on the map, reachable, unoccupied and off the paths of the movers heading
// into b is the spot. r = b's footprint radius 0x3969f0 (foot_radius: a squad container's joint box). [port]
// reachability = b's mask and region component. s78n t20 (MOVEMENT §5.4.3 "Occupancy, settled"): a layer object other than
// b blocks the sample when its centre comes within (r + 5 x FormationSpacing) + its directional radius of the sample
// swept one unit along world +x (ring_blocked; a squad member is never a layer object, a squad container is). The
// movers' path stacks stand for their path approximations (clearance = the mover's half width + r + 4).
bool MovementSystem::push_spot(const Unit& b, V2 dir, float cone, float outside, V2& out) const {
    const float r = foot_radius(b);
    const float reach = r + b.d.formation_spacing * 20.0f * 0.25f;  // + the margin +0x48
    // 0x505490: the other participants of b's MovingVsStopped collisions (kind 3), in collision-list order.
    struct Avoid {
        std::vector<V2> pts;  // s78n t20: the mover's path stack from its position
        float clearance;
    };
    std::vector<Avoid> avoid;
    for (const CollisionObject& c : coll_.list()) {
        if (c.kind != CollisionKind::MovingVsStopped || c.b != b.d.id) continue;
        const Unit* m = unit_ptr(c.a);
        if (!m) continue;
        Avoid av;
        stack_points(*m, av.pts);
        if (av.pts.size() < 2) continue;
        av.clearance = half_width(*m) + r + 4.0f;
        avoid.push_back(std::move(av));
    }
    // 0x50ecd0: both cells pass b's mask and lie in one connected region of it.
    const int32_t bcx = world_to_cell(b.m.pos.x), bcy = world_to_cell(b.m.pos.y);
    if (!grid_.passable_cell(bcx, bcy, b.lp.mask)) return false;
    const int32_t comp = regions_built_ ? regions_.component_at(b.slot, bcx, bcy) : -1;
    std::vector<uint32_t> occ;
    RingSetup ring;
    ring.centre = b.m.pos;
    ring.dir = dir;
    ring.r = r;
    ring.cone = cone;
    ring.outside = outside;
    return ring_search(
        ring,
        [&](V2 p, uint32_t) {
            const int32_t cx = world_to_cell(p.x), cyy = world_to_cell(p.y);
            if (!grid_.in_range(cx, cyy) || !grid_.passable_cell(cx, cyy, b.lp.mask)) return false;
            if (comp >= 0 && regions_.component_at(b.slot, cx, cyy) != comp) return false;
            layer_box_query(p, reach + 1.0f, occ);
            for (uint32_t ci : occ)
                if (units_[ci].d.id != b.d.id && ring_blocked(p, reach, foot_of(units_[ci]))) return false;  // occupied
            for (const Avoid& av : avoid) {
                V2 s0 = av.pts[0];
                for (size_t k = 1; k < av.pts.size(); ++k) {
                    const V2 s1 = av.pts[k];
                    const V2 seg = s1 - s0;
                    const float l2 = length_sq(seg);
                    const float t = l2 > 0.0f ? std::clamp(dot(p - s0, seg) / l2, 0.0f, 1.0f) : 0.0f;
                    if (length_sq(p - (s0 + seg * t)) < av.clearance * av.clearance) return false;  // on a mover's path
                    s0 = s1;
                }
            }
            return true;
        },
        out);
}

// ---- s78n t20 (MOVEMENT §5.4.3): StoppedVsStopped ----

// Escape direction 0x5c74b0 of X against the other participant O: toward X's target when it has one; X's own facing for
// a Vehicle / Large / Giant unit facing away from a unit of a higher class; else straight away from O.
V2 MovementSystem::svs_escape_dir(const Unit& x, const Unit& o) const {
    const V2 fx = dir_from_deg(x.m.yaw);
    if (x.ci.target >= 0) {
        const V2 d = normalize_retail(x.ci.target_pos - x.m.pos);
        if (length_sq(d) > 0.0f) return d;
    }
    const int32_t cx = x.d.collision_class;
    if (cx >= kUccVehicle && cx <= kUccGiantVehicle && o.d.collision_class > cx && dot(x.m.pos - o.m.pos, fx) > 0.0f) return fx;
    const V2 away = normalize_retail(x.m.pos - o.m.pos);
    return length_sq(away) > 0.0f ? away : fx;
}

// Cost 0x686b20(x, y, d): the time to turn to the escape direction d plus the time to clear y along it. v = x's effective
// max speed, D = y - x, r = sqrt(hx^2 + hy^2) of each TYPE's extents 0x371df0, R = r_x + r_y.
float MovementSystem::svs_cost(const Unit& x, const Unit& y, V2 d) const {
    const float v = walk_max_speed(x);
    const V2 delta = y.m.pos - x.m.pos;
    const V2 hx = type_half(x), hy = type_half(y);
    const float big_r = std::sqrt(hx.x * hx.x + hx.y * hx.y) + std::sqrt(hy.x * hy.x + hy.y * hy.y);
    const float along = v * dot(d, delta), side = v * (d.y * delta.x - d.x * delta.y);
    const float dist = length(delta);
    bool have = false;
    float t = 0.0f;
    if (along > 0.0f) {
        t = (dist + big_r) / along;
        have = true;
    } else if (along < 0.0f) {
        t = dist > big_r ? 0.0f : (big_r - dist) / -along;
        have = true;
    }
    if (side != 0.0f) {
        const float ts = big_r / side;  // signed, as coded
        if (!have || ts < t) {
            t = ts;
            have = true;
        }
    }
    if (!have || t < 0.0f) return 0.0f;
    const float rate = loco_turn_rate(x);
    const float turn = std::fabs(wrap_deg(heading_deg(d) - x.m.yaw));
    return (rate > 0.0f ? turn / rate : 0.0f) + t;
}

// Which unit moves, 0x687010 (first match): a Landing Transport -> the other; a lowering hover unit -> the other;
// deployed / deploying units with Max_Dist_Walk_While_Deployed <= 0 never move (both: nobody; s78n t26.1: the droid, M > 0,
// can be the one moved); exactly one has a target -> the one without; two Infantry-class
// units with targets -> nobody; against a Giant of a higher class only the smaller one is a candidate; else both are,
// the cheaper first (cost(p1) <= cost(p0) -> p1 first). Of the candidates the first whose locomotor would accept the
// claim moves (no owner, this record, or an owner of lower weight). A participant without a locomotor (a death clone, s78n t20
// batch 1) makes the other one move (rule 1). [port] A unit whose max speed is 0 counts as immobile too (retail has no
// such case: it would pick it and wait for good).
uint8_t MovementSystem::svs_pick_mover(const CollisionObject& c, const Unit& p0, const Unit& p1) const {
    if (p0.d.collision_class == kUccLandingTransport || p0.d.no_locomotor) return 1;  // (s78n t20 batch 1: no locomotor)
    if (p1.d.collision_class == kUccLandingTransport || p1.d.no_locomotor) return 0;
    if (p0.hover == 3) return 1;
    if (p1.hover == 3) return 0;
    const auto imm = [this](const Unit& x) { return deploy_immobile(x) || !(walk_max_speed(x) > 0.0f); };
    const bool i0 = imm(p0), i1 = imm(p1);
    if (i0 && i1) return 2;
    if (i0) return 1;
    if (i1) return 0;
    const bool t0 = p0.ci.target >= 0, t1 = p1.ci.target >= 0;
    if (t0 != t1) return t0 ? uint8_t{1} : uint8_t{0};
    const int32_t c0 = p0.d.collision_class, c1 = p1.d.collision_class;
    const bool both_inf = c0 == kUccInfantry && c1 == kUccInfantry;
    if (both_inf && t0 && t1) return 2;
    uint8_t cand[2] = {2, 2};
    if (!both_inf && c0 != c1 && std::max(c0, c1) == kUccGiantVehicle) {
        cand[0] = c0 < c1 ? uint8_t{0} : uint8_t{1};
    } else {
        const bool p1_first = svs_cost(p1, p0, svs_escape_dir(p1, p0)) <= svs_cost(p0, p1, svs_escape_dir(p0, p1));
        cand[0] = p1_first ? uint8_t{1} : uint8_t{0};
        cand[1] = p1_first ? uint8_t{0} : uint8_t{1};
    }
    for (const uint8_t k : cand) {
        if (k > 1) continue;
        const Unit& x = k == 0 ? p0 : p1;
        if (!x.pushed || x.push_owner == kNoDetour || x.push_owner == c.seq || x.push_weight < 3) return k;  // vf +0x168
    }
    return 2;
}

// Service 0x687570 with the resolution 0x687720 (MOVEMENT §5.4.3). No instant displacement: the unit chosen by
// svs_pick_mover walks a land path to the first free spot of the ring search around itself (cone toward its escape
// direction), pushed above its own paths as a collision path; the record is Resolving while it holds that path and
// Resolved when the path is gone. No mover, no spot (or its own position: ring 0), no path or a refused claim -> Try Again
// Later, re-tried every 30 frames. [port] retail never re-tests the contact, so a record whose resolution failed would
// cycle 4 -> 0 -> 4 for as long as both units live; the port ends it at a retry when the two footprints no longer overlap
// by more than MinimumStoppedVsStoppedOverlapCoefficient or either unit is Moving (the next check makes whatever record
// then applies).
void MovementSystem::svs_service(CollisionObject& c, Unit& a, Unit& b, uint32_t frame) {
    using S = CollisionState;
    // Validity 0x686e80: a participant standing on a cell that is impassable for it -> Unresolvable
    // (s78n t20 batch 1: only a participant with a locomotor is tested)
    const auto off_cell = [this](const Unit& u) { return !u.d.no_locomotor && !grid_.passable_world(u.m.pos, u.lp.mask); };
    if (off_cell(a) || off_cell(b)) {
        for (Unit* u : {&a, &b})
            if (holds_path(*u, c)) {
                end_push(*u);
                u->path_change = frame;
            }
        c.state = S::Resolved;
        return;
    }
    if (c.state == S::Active) {  // the resolution (the first service after the record was made runs it at once)
        c.state = S::TryAgainLater;
        c.master = svs_pick_mover(c, a, b);
        if (c.master > 1) return;
        Unit& x = c.master == 0 ? a : b;
        const Unit& o = c.master == 0 ? b : a;
        const bool inf = x.d.collision_class == kUccInfantry;
        V2 spot;
        if (!push_spot(x, svs_escape_dir(x, o), inf ? 3.1415927f : 2.3561945f, inf ? 1.0f : 0.75f, spot)) return;
        if (spot == x.m.pos) return;  // ring 0: nothing is near any more
        // 0x5a2830: a land path from the main pathfinder ([port] the same request as the push's; its flags are not decoded)
        LandPathRequest q;
        q.start = x.m.pos;
        q.goal = spot;
        q.mask = x.lp.mask;
        q.clearance_radius = x.d.radius > kCellSize && !x.d.team_container ? x.d.radius : 0.0f;
        LandPathScratch sc;
        LandPath out;
        find_land_path(grid_, regions_, q, sc, out);
        if (!out.ok || out.points.size() < 2) return;
        if (!claim(x, c, false, frame)) return;  // an owner of equal or higher weight refuses
        const float len = path_length_3d(out.points);
        push_top(x, std::move(out.points), 3, false, c, frame);
        // s78n t26.1 (MOVEMENT §3.2.1 item 5): the path starts through Walk vf +0x148 0x61e570 - a deployed / deploying
        // mover walks it deployed when it is shorter than M, else it undeploys
        deploy_path_start(x, len, frame);
        c.state = S::Resolving;
        return;
    }
    if (c.state == S::Resolving) {
        const Unit& x = c.master == 0 ? a : b;
        if (c.master > 1) c.state = S::Resolved;
        else if (x.pushed && x.push_owner != kNoDetour && x.push_owner != c.seq) c.state = S::TryAgainLater;  // owned by another record
        else if (!holds_path(x, c)) c.state = S::Resolved;  // walked to its end and popped, or dropped
        return;
    }
    if (c.state == S::TryAgainLater && frame >= c.retry) {
        c.retry = frame + 30;
        const bool apart = foot_overlap_coefficient(foot_of(a), foot_of(b)) <= cfg_.constants.minimum_stopped_vs_stopped_overlap;
        c.state = apart || unit_moving(a) || unit_moving(b) ? S::Resolved : S::Active;  // ([port] the end rule above)
    }
}

void MovementSystem::end_push(Unit& u) {
    u.pushed = false;
    u.push_flee = false;  // s78n t14 part B: the crush-flee path ends with the push (popped)
    u.push_path.clear();
    u.push_next = 0;
    u.push_frac.clear();  // s78n t26.2
    u.push_turn_done = false;
    u.push_owner = kNoDetour;  // s78n t20: the locomotor gives up the record id (pack +0x248)
    u.push_weight = 0;
    u.push_kind = 0;
    u.push_resume = 0;
    u.push_tags.clear();
}

void MovementSystem::tick_land(uint32_t frame, SimRng& rng, const ParallelFor& pf) {
    // s78n t26.4 (MOVEMENT §3.5.2 C.4, s78n t26 RE 2): the object service's snapshot before the behaviours - the End Move
    // coast vector is the whole previous frame's 2-D displacement P(end of N - 1) - P(end of N - 2)
    for (Unit& u : units_) {
        u.m.last_dir = u.m.pos - u.prev_pos;
        u.prev_pos = u.m.pos;
        walk_transition_fill(u);  // s78n t26 closing review: the Walk_Transition clip data (serial: the hook)
    }
    rebuild_regions_if_dirty(pf);
    tree_.maintain(static_cast<double>(frame) / 30.0);  // shape only; query results do not depend on it
    plan_land(frame, pf);
    detect_collisions(frame);  // S7: checks create collision objects
    drive_collisions(frame, rng);  // S7: cancel / priority sort / resolve (TACTICAL_GAPS §6.7)
    // s78n t23 step 0 (MOVEMENT §5.4.4 item 5): a record a cut left on its last waypoint is finished at the walk step
    // (serial, ascending index, before the parallel walk: a pop walks on along the order path in this tick's walk)
    for (Unit& u : units_) {
        if (!u.cut_pending) continue;
        u.cut_pending = false;
        cut_finished(u, frame);
    }
    team_caps();  // s78k t03: serial, before the parallel walk reads Unit::team_cap
    team_speed_steps();  // s78n t16: the squad containers' acceleration and straggler cap (serial, start-of-tick positions)
    // s78n t03: each squad container's state at the start of the tick, for its steering members (anchor_at: index into
    // snaps, -1 none).
    std::vector<int32_t> anchor_at(units_.size(), -1);
    std::vector<TeamSnap> snaps;
    // s78n t14 part B (FIGHTER_FLYING §7.5): the squad containers' collision objects after this tick's driver, and the
    // start-of-tick positions (the members' steering vectors read other units, which the parallel walk moves).
    std::vector<SteerRec> recs;
    team_steer_recs(recs);
    std::vector<SteerPos> pos0;  // s78n t18 review: with the heights (the side steps' 3-D distance)
    if (!recs.empty()) {
        pos0.reserve(units_.size());
        for (const Unit& u : units_) pos0.push_back({u.m.pos, u.z, u.m.yaw});
    }
    // s78n t18 (MOVEMENT §5.4.1): each unit's own records - a Walk unit that is no squad member adds the vector to its
    // heading (a container uses none; its members read its records through their TeamSnap).
    std::vector<std::pair<uint32_t, uint32_t>> own(units_.size(), std::pair<uint32_t, uint32_t>{0u, 0u});
    for (size_t r0 = 0; r0 < recs.size();) {
        size_t r1 = r0;
        while (r1 < recs.size() && recs[r1].container == recs[r0].container) ++r1;
        if (const int32_t ui = find(recs[r0].container); ui >= 0)
            own[static_cast<size_t>(ui)] = {static_cast<uint32_t>(r0), static_cast<uint32_t>(r1)};
        r0 = r1;
    }
    for (size_t i = 0; i < units_.size(); ++i) {
        const Unit& u = units_[i];
        if (u.anchor < 0) continue;
        const int32_t ci = find(u.anchor);
        if (ci < 0) continue;
        const Unit& k = units_[static_cast<size_t>(ci)];
        TeamSnap t;
        t.pos = k.m.pos;
        t.yaw = k.m.yaw;
        t.speed = k.m.speed;
        t.state = k.m.state;
        t.has_goal = k.has_goal;
        t.laid_out = k.laid_out;  // s78n t14
        t.has_target = k.team_target;
        t.z = k.z;
        t.fleeing = k.pushed && k.push_flee;  // s78n t14 part B (0x39a5b0)
        const auto lo = std::lower_bound(recs.begin(), recs.end(), u.anchor,
                                         [](const SteerRec& e, ObjectId c) { return e.container < c; });
        auto hi = lo;
        while (hi != recs.end() && hi->container == u.anchor) ++hi;
        t.rec_begin = static_cast<uint32_t>(lo - recs.begin());
        t.rec_end = static_cast<uint32_t>(hi - recs.begin());
        anchor_at[i] = static_cast<int32_t>(snaps.size());
        snaps.push_back(t);
    }
    // OI tactical: formation jiggle (0x42c270, §3.5 step 6) - serial, ascending id, before the parallel walk. s78n t03: a
    // steering member refreshes its jiggle while its team moves (0x62d620 calls 0x42c270 before reading it).
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (anchor_at[i] >= 0) {
            // s78n t14 (FIGHTER_FLYING §7.3): 0x62d620 (which refreshes the jiggle) runs while the container turns in place,
            // moves or brakes, and not in a start-delay frame.
            const TeamSnap& k = snaps[static_cast<size_t>(anchor_at[i])];
            const bool turn_or_move = k.state == LocoState::WalkStartTurn || k.state == LocoState::WalkTurning ||
                                      k.state == LocoState::WalkMoving;
            if (!turn_or_move && k.state != LocoState::WalkEndMove) continue;
            if (turn_or_move && u.start_delay > 0) continue;
        } else if (!u.in_formation || !u.has_goal || u.need_path || frame < u.start_frame || is_air(u)) {
            continue;
        }
        if (!(u.m.speed > 0.0f) || !(u.lp.max_speed > 0.0f)) continue;  // a moving frame
        const float rag = u.d.formation_raggedness, mx = u.d.max_jiggle_distance;
        if (!(rag > 0.0f) || !(mx > 0.0f)) continue;
        const float frac = u.m.speed / u.lp.max_speed;
        u.jiggle.x = std::clamp(u.jiggle.x + rng.range_float(-1.0f, 1.0f) * frac * rag, -mx, mx);
        u.jiggle.y = std::clamp(u.jiggle.y + rng.range_float(-1.0f, 1.0f) * frac * rag, -mx, mx);
    }
    std::vector<WalkResult> res(units_.size());
    // s78n t18 (MOVEMENT §5.4.1 "How Walk combines the vector"): a Walk unit with collision records adds the steering vector
    // 0x509810 to its wanted direction (start-of-tick positions; its new speed for the 0x5c6d40 ratio).
    struct AvoidCtx {
        const MovementSystem* sys;
        const Unit* u;
        float vmax;
        std::span<const SteerRec> recs;
        std::span<const SteerPos> pos;
    };
    // s78n t26.4 (MOVEMENT §5.4.1 "How Walk combines" items 2 - 3, §3.5.2 C): the extras of unit i's walk on its top record -
    // the lane term ([port] on every record of a unit with an order: the formation update 0x41dbb0 writes it for a member
    // with a path; the jiggle and the formation speed only on the order path of a formation member), the steering vector,
    // the "at once" flag, the forced flag, Movement_Directions_Count.
    const auto walk_extras = [&](uint32_t i, bool order_rec, WalkExtras& x, AvoidCtx& actx) {
        const Unit& u = units_[i];
        actx = AvoidCtx{this, &u, u.lp.max_speed, {}, pos0};
        if (own[i].second > own[i].first) {
            actx.recs = std::span<const SteerRec>(recs).subspan(own[i].first, own[i].second - own[i].first);
            x.avoid = [](const void* c, V2 dir) -> V2 {
                const AvoidCtx& k = *static_cast<const AvoidCtx*>(c);
                return k.sys->member_avoid(*k.u, k.u->m.pos, k.u->m.speed, k.vmax, dir, k.recs, k.pos);
            };
            x.avoid_ctx = &actx;
        }
        x.cap_now = u.coll_now;  // s78n t20 (MOVEMENT §3.5 step 2): the collision cap's "at once" flag
        if (u.wt.on) x.wt = &u.wt;  // s78n t26 closing review: Walk_Transition (MOVEMENT §3.4, §3.5.2 F)
        x.lane = u.has_goal;
        x.directions = u.d.movement_directions;
        if (order_rec) {
            x.formation_path = u.in_formation;
            if (u.in_formation) x.jiggle = u.jiggle;  // OI tactical: formation members
            x.formation = u.formation_speed;
            x.forced = u.forced;
        }
    };
    // The walk of one unit that is no squad member / container (the parallel pass; s78n t26.2: also the serial start-turn
    // pass below, for a start turn that ends in Start Move). Writes only units_[i] and res[i].
    const auto walk_one = [&](uint32_t i) {
        Unit& u = units_[i];
        const bool bike = u.d.locomotor == LocomotorKind::Bike;
        const float yaw0 = u.m.yaw;
        AvoidCtx actx{this, &u, u.lp.max_speed, {}, pos0};
        // a1k t03 (A1_READINGS §40.3): the collision-owned record is the top of the path stack; the order path waits
        // under it. s78n t20 (MOVEMENT §1.5.1 item 5): it is walked as the order path is - the collision records'
        // speed answer, the steering vector, the team cap - without the formation speed or jiggle.
        // s78n t26.1 (MOVEMENT §3.2.1 item 5): a deployed unit with M > 0 walks a push deployed (the 0x19 branch first)
        if (u.pushed && (u.deploy == LocoState::None || deploy_walks(u))) {
            if (deploy_branch(u)) return;
            WalkExtras x;
            walk_extras(i, false, x, actx);
            std::swap(u.m.path, u.push_path);
            std::swap(u.m.next, u.push_next);
            std::swap(u.m.frac, u.push_frac);  // s78n t26.2: the record's fractions and turn-done byte
            std::swap(u.m.turn_done, u.push_turn_done);
            const bool order_started = u.m.started;
            u.m.started = true;  // a collision-owned record is started (MOVEMENT §5.4.1 "Push")
            const uint32_t escape = u.m.escape_until;
            u.m.escape_until = 0;
            // s78n t26.4 (MOVEMENT §3.5.2 C.4 / C.11): a started order path under the record (Q) - no End Move for the
            // record's end, and the walk pops the record and goes on along Q in the same call
            LandMotion q;
            const bool below = u.has_goal && !u.need_path && frame >= u.start_frame && order_started && !u.push_path.empty();
            if (below) {
                q.path = std::move(u.push_path);
                q.next = u.push_next;
                q.frac = std::move(u.push_frac);
                q.turn_done = u.push_turn_done;
                q.started = true;
                x.below = &q;
                x.below_kind = u.push_kind;
                x.below_resume = u.push_resume;
                x.below_forced = u.forced;
            }
            // s78n t14 part B: on a crush-flee path (0x39a5b0) the speed (0x370f00) and with it the acceleration /
            // deceleration (0x372fe0 / 0x373080: x effective / Max_Speed) x Fleeing_Infantry_Speed_Bonus.
            LocomotorParams plp = u.lp;
            if (u.team_cap < kInf && u.speed_factor > 0.0f) plp.max_speed = u.team_cap;
            if (u.push_flee) {
                const float fb = cfg_.constants.fleeing_infantry_speed_bonus;
                plp.max_speed *= fb;
                plp.accel *= fb;
                plp.decel *= fb;
            }
            x.vmax_u = plp.max_speed;  // C.1: never the deployed speed
            if (u.walk_deployed) plp.max_speed = u.d.deployed_walk_speed;  // s78n t26.1
            actx.vmax = plp.max_speed;
            res[i] = walk_step(u.m, plp, u.coll_speed, &grid_, &x);
            if (below) {
                u.push_path = std::move(q.path);
                u.push_next = q.next;
                u.push_frac = std::move(q.frac);
                u.push_turn_done = q.turn_done;
            }
            u.m.escape_until = escape;
            if (res[i].popped) {
                // u.m is the order path now (walked on in this call); the finished record is popped (0x557b10)
                end_push(u);
                u.around.clear();
                u.path_change = frame;
            } else {
                u.m.started = order_started;
                std::swap(u.m.path, u.push_path);
                std::swap(u.m.next, u.push_next);
                std::swap(u.m.frac, u.push_frac);
                std::swap(u.m.turn_done, u.push_turn_done);
            }
            start_turn_mark(u, res[i]);  // s78n t26.2
            if (bike) bike_frame(u, yaw0, true);
            return;
        }
        // s78n t18 / t20 batch 2 (MOVEMENT §5.3 "Hover in full", Walk service 0x61e930): while rising (2) / lowering
        // (3) the Walk state machine does not run (no turn, no speed step, no horizontal move). T = the ground height
        // at (x, y). Rising: z >= T + Hover_Height on entry -> z := that, raised (1), speed 0; else z += the effective
        // max speed, and z > T + Hover_Height -> z := that, raised, speed 0 (landing exactly costs one more frame;
        // speed 0: the rise never ends). Lowering: z <= T on entry -> grounded (0); else z -= the speed, clamped at
        // T with state 0 (it lands on the frame it reaches T: the spec gives no extra frame here). hover_off = z - T,
        // the height above the ground (hashed, saved).
        const bool hovers = u.d.hover_height > 0.0f && u.d.locomotor == LocomotorKind::Default;
        if (hovers && u.hover >= 2) {
            const float step = u.lp.max_speed;
            const float ground = height_ ? height_(u.m.pos.x, u.m.pos.y) : 0.0f;
            const bool rising = u.hover == 2;
            const float target = rising ? ground + u.d.hover_height : ground;
            const auto reached = [&](float z) { return rising ? z >= target : z <= target; };
            bool done = reached(u.z);
            if (!done) {
                u.z = rising ? u.z + step : u.z - step;
                done = rising ? u.z > target : u.z <= target;
            }
            if (done) {
                u.z = target;
                u.hover = rising ? uint8_t{1} : uint8_t{0};
                u.m.speed = 0.0f;
            }
            u.hover_off = u.z - ground;
            return;
        }
        // s78k t01 / s78l t02 (MOVEMENT §3.5.1): the arrival turn runs in the serial pass below (it reads the clip
        // data hook).
        if (u.aturn != LocoState::None && !u.has_goal && u.d.locomotor == LocomotorKind::Default) return;
        // S8 t11: a Walk in a deploy state does not move (MovementSystem::deploy). s78n t26.1 (MOVEMENT §3.2.1 item 3):
        // but 0x19 with M > 0 and a path walks deployed.
        if (u.deploy != LocoState::None && !deploy_walks(u)) {
            if (bike) bike_frame(u, yaw0, false);
            return;
        }
        // s78k t03 / s78l t01: a team member walks at the team speed (team_caps) - the container's effective max speed
        // replaces its own (no member factor enters), in a formation or not; a stunned member (factor 0) stays put.
        // s78n t26.1 / t26.2 (MOVEMENT §3.2.1 item 3, §3.5.2 B.1): walking deployed, the speed controller's vmax is
        // Deployed_Walk_Speed (raw); the acceleration / deceleration are the unit's own
        LocomotorParams lp = u.lp;
        if (u.team_cap < kInf && u.speed_factor > 0.0f) lp.max_speed = u.team_cap;
        const float vmax_u = lp.max_speed;
        if (u.walk_deployed) lp.max_speed = u.d.deployed_walk_speed;
        if (!u.has_goal || u.need_path || frame < u.start_frame) {
            // s78n t26.4 (MOVEMENT §3.5.2 C.1 / C.4, G): no started path (no order - a stop on a record owned by another,
            // a cut, a halt -, a new order not planned yet, a start delay): a moving unit goes to End Move at T = a and
            // coasts along its last frame's displacement to 0x15 (then idle); a standing one does nothing
            // s78n t26.4 fix (MOVEMENT §3.5.2 G, §4.6.2): a new order that is not planned yet (its composition waits for
            // the next coordinator update, or the re-plan for the next plan pass) leaves the old order path on top of the
            // stack - retail's stack reset 0x554480 runs only in the plan member loop, together with the path set - so a
            // moving unit walks on along it until then, as before the order. [port] its arrival on that path is not the
            // new order's (r.arrived is dropped); a standing unit does nothing (the start delay of a new path applies at
            // speed 0 only).
            if (u.has_goal && u.m.speed > 0.0f && !u.m.path.empty() && u.m.next < u.m.path.size()) {
                WalkExtras x;
                walk_extras(i, false, x, actx);
                x.vmax_u = vmax_u;
                actx.vmax = lp.max_speed;
                res[i] = walk_step(u.m, lp, u.coll_speed, &grid_, &x);
                res[i].arrived = false;
                start_turn_mark(u, res[i]);
                if (bike) bike_frame(u, yaw0, true);
                return;
            }
            if (u.m.speed > 0.0f) {
                LandMotion c;
                c.pos = u.m.pos;
                c.yaw = u.m.yaw;
                c.speed = u.m.speed;
                c.state = u.m.state;
                c.last_dir = u.m.last_dir;
                c.started = false;
                c.wt_clip = u.m.wt_clip;  // s78n t26 closing review: the Walk_Transition clip state runs on
                c.wt_pos = u.m.wt_pos;
                c.wt_rate = u.m.wt_rate;
                c.wt_f4 = u.m.wt_f4;
                c.wt_f0 = u.m.wt_f0;
                WalkExtras x;
                walk_extras(i, false, x, actx);
                x.lane = false;
                x.vmax_u = vmax_u;
                actx.vmax = lp.max_speed;
                res[i] = walk_step(c, lp, u.coll_speed, &grid_, &x);
                u.m.pos = c.pos;
                u.m.yaw = c.yaw;
                u.m.speed = c.speed;
                u.m.state = c.state;
                u.m.wt_clip = c.wt_clip;
                u.m.wt_pos = c.wt_pos;
                u.m.wt_rate = c.wt_rate;
                u.m.wt_f4 = c.wt_f4;
                u.m.wt_f0 = c.wt_f0;
                if (bike) bike_frame(u, yaw0, true);
                return;
            }
            if (bike) bike_frame(u, yaw0, false);
            return;
        }
        if (deploy_branch(u)) return;
        WalkExtras x;
        walk_extras(i, true, x, actx);
        x.vmax_u = vmax_u;
        // s78n t18 / t20 batch 2 (Walk Start Move 0x61ff40): a Hover_Height unit that is not raised rises first; the
        // frame ends. A turn in place (0xf-0x11) comes before Start Move, so it runs on the ground: the start rule is
        // tried on a copy, and a frame that only turns is kept (the walk's own result), one that would move rises.
        if (hovers && u.hover != 1) {
            LandMotion probe = u.m;
            const WalkResult pr = walk_step(probe, u.lp, kInf, &grid_, &x);
            if (pr.turn) {
                u.m = std::move(probe);
                res[i] = pr;
                start_turn_mark(u, pr);  // s78n t26.2
            } else {
                u.hover = 2;
                u.m.speed = 0.0f;
            }
            return;
        }
        // s78n t18 (0x509ea0): the collision records' speed answer is a target the walk brakes toward at its rate. s78n
        // t26.2 (MOVEMENT §3.5.2 B.2 / B.6): the formation speed is the order path's (a formation path) target, apart from
        // the collision cap
        actx.vmax = lp.max_speed;
        res[i] = walk_step(u.m, lp, u.coll_speed, &grid_, &x);
        start_turn_mark(u, res[i]);  // s78n t26.2
        if (bike) bike_frame(u, yaw0, true);  // §4.5: states 0x20-0x25 and the lean (writes only u)
    };
    pf(static_cast<uint32_t>(units_.size()), 64, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            Unit& u = units_[i];
            if (is_air(u) || u.d.no_locomotor) continue;  // §4.3-4.6: serial pass below; s78n t20 batch 1: no locomotor
            if (anchor_at[i] >= 0) {  // s78n t03: a squad member steers to its slot around its container
                steer_member(u, snaps[static_cast<size_t>(anchor_at[i])], recs, pos0);
                continue;
            }
            // s78n t18 (FIGHTER_FLYING §7.7 items 3 - 6): a squad container's own walk - its pushed path (the team speed,
            // x Fleeing_Infantry_Speed_Bonus with the rate and the turn rate on a crush-flee path) or its order path
            if (u.d.team_container) {
                res[i] = team_walk(u, frame);
                if (res[i].popped) {  // s78n t26.5 (MOVEMENT §3.5.2 C.4 / E): the pushed record ended, walked on along the order path
                    end_push(u);
                    u.around.clear();
                    u.path_change = frame;
                }
                continue;
            }
            // s78n t26.2 (MOVEMENT §3.5.2 A): a start turn in place runs in the serial pass below (it reads the clip data
            // hook), the move step that follows it too
            if (u.aturn != LocoState::None && u.aturn_start && u.d.locomotor == LocomotorKind::Default) continue;
            walk_one(i);
        }
    });
    team_relay_pass(frame);  // s78n t14: the squad containers' re-lays (serial, ascending id)
    // s78l t02 (MOVEMENT §3.5.1 states 0x0F-0x15): the arrival turns, serial in ascending id; their clip plays become
    // this tick's turn clip events. s78n t26.2 (MOVEMENT §3.5.2 A): the start turns in place too (the wanted yaw re-read
    // from the top path every frame; nothing left to walk drops the turn); one that ends goes to Start Move and runs its
    // move step here, in the same tick ([port] placement: retail runs 0x12 in the same frame, inside the Walk service).
    for (uint32_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (u.aturn == LocoState::None || u.d.locomotor != LocomotorKind::Default || is_air(u)) continue;
        if (u.aturn_start) {
            float want = 0.0f;
            WalkExtras sx;
            AvoidCtx sctx{this, &u, u.lp.max_speed, {}, pos0};
            walk_extras(i, !u.pushed, sx, sctx);
            if (!start_turn_want(u, frame, sx, want)) {
                arrival_reset(u);
                if (u.m.state == LocoState::WalkStartTurn || u.m.state == LocoState::WalkTurning || u.m.state == LocoState::WalkEndTurn)
                    u.m.state = LocoState::WalkStopped;
                continue;
            }
            u.turn_to = want;
            arrival_turn_frame(u, frame);
            if (u.tplay_set) {
                u.tplay_set = false;
                tevents_.push_back(u.tplay);
            }
            if (u.aturn == LocoState::None && u.m.state == LocoState::WalkStartMove) walk_one(i);
            continue;
        }
        if (u.has_goal) continue;
        if (u.pushed && (u.deploy == LocoState::None || deploy_walks(u))) continue;  // the pushed walk runs (parallel pass)
        arrival_turn_frame(u, frame);
        if (u.tplay_set) {
            u.tplay_set = false;
            tevents_.push_back(u.tplay);
        }
    }
    tick_special(frame, rng);  // §4.3, 4.4, 4.6: serial, ascending id
    tick_deploy(frame);        // S8 t11: Walk deploy states (serial, ascending id)
    tick_turrets(frame);       // S8 t24: TURRET service of the Deploys units (serial, ascending id)
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (is_air(u)) {
            update_tree(u);
            continue;
        }
        if (u.d.no_locomotor) continue;  // s78n t20 batch 1: a death clone stands where it was placed
        const WalkResult& r = res[i];
        bool popped_arrival = false;
        if (u.pushed) {
            // a1k t03: the pushed path ended -> the move underneath continues (an idle unit stays at the spot). s78n t24
            // (MOVEMENT §5.4.7): a failed step makes a TerrainCollision record (the blocked-frames cut of a1k is gone)
            if (r.blocked) {
                ++stats_.blocked_steps;
                terrain_report(u, frame);
            } else if (r.arrived) {
                // s78n t20 (MOVEMENT §1.5.1 item 5): popped; the order path resumes past the waypoint lines already
                // crossed, and a unit that is past its last waypoint's line has arrived where it stands
                popped_arrival = pop_top(u, true, frame);
                if (popped_arrival) {
                    u.has_goal = false;
                    u.arrived = true;
                    walk_out_done(u);
                    if (!u.d.team_container) {
                        u.m.speed = 0.0f;
                        u.m.state = LocoState::WalkStopped;
                    }
                    land_arrival_turn(u);
                }
            }
        } else {
            // s78n t26.4 (MOVEMENT §3.5.1 "Arrival turn, details" item 1): the walk's own 0x15 reports the arrival - End Move at
            // a speed <= the deceleration on the last waypoint, or within 5 before its line (the t20 commit-loop rule is
            // gone). s78n t18: a squad container's arrival is its own state 0x15 (team_walk)
            if (r.arrived && u.has_goal) {
                u.has_goal = false;
                u.arrived = true;
                u.around.clear();  // s78n t18: the path is popped
                u.path_change = frame;
                walk_out_done(u);  // (MOVEMENT §5.3: the walk-out move arrived)
                land_arrival_turn(u);  // s78k t01: state 0x15
            }
            // s78n t24 (MOVEMENT §5.4.7): a failed step makes a TerrainCollision record; there is no count of blocked frames
            // and no re-plan of the order path (the a1k blocked-frames re-plan is gone)
            if (r.blocked) {
                ++stats_.blocked_steps;
                terrain_report(u, frame);
            }
        }
        // s78n t26.4 (MOVEMENT §3.5.2 C): the walk's counters
        if (r.coast) ++stats_.walk_coasts;
        if (r.restart) ++stats_.walk_restarts;
        stats_.walk_tau_skips += r.tau_skips;
        stats_.walk_degen_skips += r.degen_skips;
        // s78n t20 batch 2 (MOVEMENT §5.3 "Hover in full", commit 0x42d6a0): a raised hover unit stands at G + Hover_Height;
        // a rising / lowering one keeps the z its hover step wrote
        if (height_ && u.hover < 2) u.z = height_(u.m.pos.x, u.m.pos.y) + (u.hover == 1 ? u.d.hover_height : 0.0f);
        update_tree(u);
    }
    crush_pass(res);  // OI tactical
    for (ObjectId id : despawned_) remove_unit(id);  // §4.6: bombers at path end (reported, then dropped)
}
}  // namespace alamo::tactical::movement

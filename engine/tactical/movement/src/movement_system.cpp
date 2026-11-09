#include "alamo/tactical/movement/movement_system.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo::tactical::movement {

namespace {

constexpr float kInf = 3.0e38f;
constexpr uint32_t kReplanAfterBlocked = 10;
constexpr uint32_t kMaxReplans = 30;
constexpr uint32_t kWaitsBeforePush = 8;

uint64_t cell_key(int32_t x, int32_t y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32) | static_cast<uint32_t>(x);
}

bool contains_key(const std::vector<uint64_t>& v, uint64_t k) { return std::binary_search(v.begin(), v.end(), k); }
void insert_key(std::vector<uint64_t>& v, uint64_t k) {
    auto it = std::lower_bound(v.begin(), v.end(), k);
    if (it == v.end() || *it != k) v.insert(it, k);
}

// Point at arc length s along pos -> path[next..]; seg = index of the waypoint the point precedes.
V2 point_along(V2 pos, const std::vector<V2>& path, uint32_t next, float s, size_t& seg) {
    V2 a = pos;
    float acc = 0.0f;
    for (size_t i = next; i < path.size(); ++i) {
        const float l = distance(a, path[i]);
        if (acc + l >= s) {
            seg = i;
            const float t = l > 0.0f ? (s - acc) / l : 0.0f;
            return lerp(a, path[i], std::clamp(t, 0.0f, 1.0f));
        }
        acc += l;
        a = path[i];
    }
    seg = path.size();
    return path.empty() ? pos : path.back();
}

float slowdown_for_layer(const MovementConstants& k, uint32_t layer) {
    if (layer & (kLayerCapital | kLayerSuperCapital)) return k.turn_in_place_slowdown_capital;
    if (layer & kLayerFrigate) return k.turn_in_place_slowdown_frigate;
    if (layer & kLayerCorvette) return k.turn_in_place_slowdown_corvette;
    return 1.0f;
}

}  // namespace

MovementSystem::MovementSystem(MovementConfig cfg) : cfg_(std::move(cfg)) {
    space_.set_window(cfg_.constants.space_object_tracking_tree_count, cfg_.constants.space_object_tracking_interval);
}

void MovementSystem::set_grid(PassabilityGrid grid, const ParallelFor& pf) {
    grid_ = std::move(grid);
    regions_built_ = false;
    rebuild_regions_if_dirty(pf);
}

void MovementSystem::rebuild_regions_if_dirty(const ParallelFor& pf) {
    if (regions_built_ && regions_.grid_version() == grid_.version()) return;
    const std::vector<uint32_t> masks = cfg_.classes.distinct_masks();
    regions_.build(grid_, masks, pf);
    regions_built_ = true;
    for (Unit& u : units_) u.slot = regions_.mask_slot(u.lp.mask);
}

// cu8i t02 (CU8G_T02_ENV_PICK_RE §7 P1 results): FormationClass ctor 0x40d0d0, one synced int(1, FPS).
int32_t MovementSystem::formation_ctor_draw(SimRng& rng) { return rng.range_int(1, kFormationFps); }

bool MovementSystem::has_coordinator(ObjectId id) const {
    // cu8i t02 [port]: a unit has a movement coordinator (retail locomotor pack +0x1f0 formation) while it has an order
    // (a goal) or a formation; the coordinator-less rest is what 0x39bcb0 adopts at Load_Map's tail.
    // cu8j: also while it waits for its order's composition (pend) or holds in an adopt's coordinator (adopted).
    const Unit* u = unit_ptr(id);
    return u && (u->has_goal || u->in_formation || u->adopted || u->pend != 0);
}

// ---------------------------------------------------------------- cu8j t01 / t02: coordinator update, adopt
uint32_t MovementSystem::compose_register(bool space, bool moves, V2 goal) {
    if (++pend_serial_ == 0) pend_serial_ = 1;  // 0 = none (a wrap after 2^32 orders)
    PendingComposition pc;
    pc.serial = pend_serial_;
    pc.space = space;
    pc.moves = moves;
    pc.goal = goal;
    pending_.push_back(std::move(pc));
    return pend_serial_;
}

// MOVEMENT §4.6.2: the coordinators in creation order; a space composition stopped by a seed without speed stays.
void MovementSystem::coordinator_update(uint32_t frame, uint64_t global_frame, SimRng& rng) {
    if (pending_.empty()) return;
    std::vector<PendingComposition> work;
    work.swap(pending_);
    std::vector<PendingComposition> keep;
    for (const PendingComposition& pc : work) {
        if (!pc.space) {
            compose_land(pc, frame, global_frame, rng);
        } else if (!compose_space(pc, frame, rng)) {
            keep.push_back(pc);
        }
    }
    // Nothing composes while the update runs, so `pending_` is still empty here [port: no re-entry].
    pending_ = std::move(keep);
}

// Land: 0x43c1b0 (the stagger draws r', descriptor order) then 0x43e5b0 (one ctor draw per descriptor), §1.6 / §4.6.1.
void MovementSystem::compose_land(const PendingComposition& pc, uint32_t frame, uint64_t global_frame, SimRng& rng) {
    std::vector<std::vector<Unit*>> forms;
    for (const std::vector<ObjectId>& f : pc.forms) {
        std::vector<Unit*> v;
        for (const ObjectId id : f)
            if (Unit* u = unit_ptr(id); u && u->pend == pc.serial) v.push_back(u);
        if (!v.empty()) forms.push_back(std::move(v));  // [port] a seed that left: the next member seeds
    }
    if (forms.empty()) return;
    // Unstaggered: F +0x1dc = 0, so the plan waits for 2 FPS < now: frame 61 when created at frame <= 60 [M].
    const uint32_t gate = global_frame <= 2u * kFormationFps ? frame + static_cast<uint32_t>(2u * kFormationFps + 1u - global_frame) : frame;
    std::vector<uint32_t> start(forms.size(), gate);
    uint32_t acc = 0;
    for (size_t i = 0; i < forms.size(); ++i) {
        const int32_t pfd = forms[i].front()->d.path_frame_delay;
        // cu8l t01: a birth coordinator (hold_from_birth: moves false) is the team container's own formation, and no
        // vanilla container type has PathFrameDelay (CU8K_T02_T06_RE §1.3): no r'. [port] Its members' PathFrameDelay is
        // not read for it.
        if (pfd <= 0 || !pc.moves) continue;  // [port] a negative PathFrameDelay draws nothing (vanilla: 0 or 4)
        const int32_t r = rng.range_int(0, pfd);
        // F +0x1dc = now + acc + r' - 2 FPS: the first plan frame is the first frame > now + acc + r'.
        start[i] = frame + acc + static_cast<uint32_t>(r) + 1u;
        acc += static_cast<uint32_t>(pfd);
    }
    for (size_t i = 0; i < forms.size(); ++i) {
        (void)formation_ctor_draw(rng);  // the next-check timer; no land reader in the port
        for (Unit* u : forms[i]) {
            if (pc.moves) u->start_frame = start[i];  // a birth hold has no order to start
            u->pend = 0;
        }
    }
}

// Space: 0x5d58f0 (§4.6.4). Candidates in the order's unit order, a squadron's fighters as its container (key).
bool MovementSystem::compose_space(const PendingComposition& pc, uint32_t frame, SimRng& rng) {
    struct Cand {
        ObjectId key = -1;
        std::vector<std::pair<Unit*, uint32_t>> units;  // (unit, start offset)
        V2 pos;
        float ex = 0.0f, ey = 0.0f;
        uint32_t role = 0;
        float vmax = 0.0f, vmin = 0.0f;
    };
    std::vector<Cand> cands;
    const std::vector<ObjectId> none;
    const std::vector<ObjectId>& ids = pc.forms.empty() ? none : pc.forms[0];
    for (size_t i = 0; i < ids.size(); ++i) {
        Unit* u = unit_ptr(ids[i]);
        if (!u || u->pend != pc.serial) continue;
        const uint32_t off = i < pc.offsets.size() ? pc.offsets[i] : 0u;
        const bool squad = is_fighter(*u) && u->fs.squad >= 0 && squad_ptr(u->fs.squad) != nullptr;
        const ObjectId key = squad ? u->fs.squad : u->d.id;
        auto it = std::find_if(cands.begin(), cands.end(), [key](const Cand& c) { return c.key == key; });
        if (it != cands.end()) {
            it->units.emplace_back(u, off);
            continue;
        }
        Cand c;
        c.key = key;
        c.units.emplace_back(u, off);
        F3 p{};
        if (fobject_pos(key, p)) c.pos = {p.x, p.y};
        else c.pos = u->m.pos;
        // [M: which extent] the hard extents (Custom_Hard_X/YExtent x Scale_Factor), else the radius; a squadron: its
        // first fighter's [port].
        c.ex = u->d.hard_x > 0.0f ? u->d.hard_x : u->d.radius;
        c.ey = u->d.hard_y > 0.0f ? u->d.hard_y : u->d.radius;
        // Get_Space_Layer 0x397900(seed, 0): Stationary_Space_Layer when not moving and set, else Space_Layer; a squadron
        // container (TEAM, no layer) is role 0. [port] moving = following a space path; no multi-locomotor space unit.
        if (!squad) c.role = !u->following && u->d.stationary_space_layer != 0 ? u->d.stationary_space_layer : u->d.space_layer_type;
        c.vmax = squad ? fform_unit_speed(key) : u->lp.max_speed;
        c.vmin = u->d.min_speed * mode_mult() * u->speed_factor;  // 0x3728f0: Min_Speed x the multipliers
        cands.push_back(std::move(c));
    }
    const auto finish = [&](const Cand& c, int32_t r) {
        for (const auto& [u, off] : c.units) {
            if (pc.moves || u->start_frame == kStartPending) u->start_frame = frame + off;  // [port] a superseded move starts
            u->pend = 0;
            if (is_fighter(*u)) {  // FIGHTER_FLYING §4.11: the formation's first 0x4192b0 / 0x410a60 check
                FighterFormation* f = fform_ptr(fkey(*u));
                if (f && f->next == kStartPending) f->next = frame + static_cast<uint32_t>(kFormationFps + r);
            }
        }
    };
    while (!cands.empty()) {
        size_t si = 0;
        float bd = 3.0e38f;
        for (size_t i = 0; i < cands.size(); ++i) {
            const float d = length_sq(cands[i].pos - pc.goal);
            if (d < bd) {
                bd = d;
                si = i;
            }
        }
        if (!(cands[si].vmax > 0.0f)) return false;  // no speed: the pass stops, the rest waits for the next update
        const Cand seed = std::move(cands[si]);
        cands.erase(cands.begin() + static_cast<std::ptrdiff_t>(si));
        const int32_t r = formation_ctor_draw(rng);  // 0x4f6fb0: the new formation
        finish(seed, r);
        if (seed.role != 0) continue;  // a ship with a role: one formation per ship
        // Role 0 (fighters, squadron containers, objects without a layer): recruit in candidate order. [port] never
        // locked (0x39ad40); every space locomotor is kind 2.
        const float reach = 0.33f * length(pc.goal - seed.pos);
        for (size_t i = 0; i < cands.size();) {
            const Cand& c = cands[i];
            const float dx = std::fabs(c.pos.x - seed.pos.x), dy = std::fabs(c.pos.y - seed.pos.y);
            const bool box = dx <= std::max(2.0f * seed.ex, reach) + c.ex && dy <= std::max(2.0f * seed.ey, reach) + c.ey;
            const bool ok = box && c.role == 0 && c.vmax != 0.0f && seed.vmax != 0.0f && c.vmin / seed.vmax <= 0.9f &&
                            seed.vmin / c.vmax <= 0.9f;
            if (ok) {
                finish(c, r);
                cands.erase(cands.begin() + static_cast<std::ptrdiff_t>(i));
            } else {
                ++i;
            }
        }
    }
    return true;
}

bool MovementSystem::adopt(ObjectId id, SimRng& rng) {
    Unit* u = unit_ptr(id);
    if (!u || has_coordinator(id)) return false;
    u->adopted = true;  // Issue_Degenerate_Move: a move to its own position, so it holds there [port: no motion]
    (void)formation_ctor_draw(rng);
    return true;
}

bool MovementSystem::hold_from_birth(std::span<const ObjectId> ids) {
    std::vector<ObjectId> held;
    for (const ObjectId id : ids)
        if (unit_ptr(id)) held.push_back(id);
    if (held.empty()) return false;
    const uint32_t serial = compose_register(cfg_.mode == MoveMode::Space, false, unit_ptr(held.front())->m.pos);
    PendingComposition& pc = pending_.back();
    pc.offsets.assign(held.size(), 0u);
    for (const ObjectId id : held) {
        Unit* u = unit_ptr(id);
        u->pend = serial;
        u->adopted = false;
    }
    pc.forms.assign(1, std::move(held));
    return true;
}

bool MovementSystem::stop_and_leave(ObjectId id) {
    if (!unit_ptr(id)) return false;
    const ObjectId one[] = {id};
    stop(one);
    Unit* u = unit_ptr(id);
    u->adopted = false;  // 0x426d50: it leaves every coordinator
    u->in_formation = false;
    u->formation_speed = kInf;
    u->pend = 0;
    return true;
}

bool MovementSystem::stop_and_hold(ObjectId id, SimRng& rng) {
    if (!stop_and_leave(id)) return false;
    return adopt(id, rng);
}

MovementSystem::CoordinatorHold MovementSystem::coordinator_hold(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    // cu8l t02 [port]: "in a formation" = a member of a formation of > 1 unit, or a unit whose composed move (not waiting
    // for its composition, not an adopt's degenerate hold) is still running. A unit waiting for its composition or held
    // by an adopt is in a coordinator without a formation of its own.
    if (!u) return CoordinatorHold::None;
    if (u->adopted || u->pend != 0) return CoordinatorHold::NoFormation;
    if (u->in_formation || u->has_goal) return CoordinatorHold::Formation;
    return CoordinatorHold::None;
}

bool MovementSystem::split_from_formation(ObjectId id, SimRng& rng) {
    if (coordinator_hold(id) != CoordinatorHold::Formation) return false;
    Unit* u = unit_ptr(id);
    (void)formation_ctor_draw(rng);  // 0x4f84e0: the new formation (ctor return 0x4f8518)
    u->in_formation = false;         // alone now; it keeps its current move (the copied move list)
    u->formation_speed = kInf;
    u->adopted = true;               // [port] still in the coordinator once the move ends
    return true;
}

int32_t MovementSystem::find(ObjectId id) const {
    auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, ObjectId v) { return u.d.id < v; });
    if (it == units_.end() || it->d.id != id) return -1;
    return static_cast<int32_t>(it - units_.begin());
}

void MovementSystem::configure(Unit& u) const {
    const MovementConstants& k = cfg_.constants;
    const float mult = mode_mult();
    // S10-int: host speed factor (MOVEMENT 3.4 items 4/5/7: Mod_Multiplier Speed x Engines_Disabled x (1 + list 5);
    // 0 = immobilised). Acceleration / deceleration scale with the effective speed (3.4). x 1 is exact, so a
    // factor of 1 keeps every value bit-identical.
    const float sf = u.speed_factor;
    // S7 t14: the normal (kind 1) values; the jetpack's airborne-kind alternates are loco_max_speed / loco_turn_rate.
    const float base = u.speed_override >= 0.0f ? u.speed_override : u.d.max_speed;  // OI abilities: Berserker +0x2a0
    u.lp.max_speed = std::max(base, 0.0f) * mult * sf;
    u.lp.turn_rate = std::max(u.d.turn_rate, 0.0f) * mult;
    u.lp.accel = u.d.accel > 0.0f ? u.d.accel * mult * sf : u.lp.max_speed;
    u.lp.decel = u.d.decel > 0.0f ? u.d.decel * mult * sf : u.lp.max_speed;
    u.lp.mask = cfg_.classes.mask(u.d.movement_class);
    u.lp.close_enough_angle = u.d.close_enough_angle > 0.0f ? u.d.close_enough_angle : k.close_enough_angle_for_move_start;
    u.slot = regions_built_ ? regions_.mask_slot(u.lp.mask) : -1;
    // REMAINING_GAPS §4.3-4.7: special locomotor parameters (getters 0x372440 / 0x3724d0 / 0x370fd0 apply the mode
    // multiplier).
    u.sp.hover_offset = u.d.hover_offset;
    u.sp.max_lift = std::max(u.d.max_lift, 0.0f) * mult;
    u.sp.roll_rate = std::max(u.d.max_rate_of_roll, 0.0f) * mult;
    u.sp.bank_turn_angle = std::fabs(u.d.bank_turn_angle);
    u.sp.min_speed = std::max(u.d.min_speed, 0.0f) * mult * sf;
    u.sp.strafe_distance = u.d.strafe_distance;
    u.sp.prepare_strafe_height = u.d.prepare_strafe_height;
    u.sp.has_pre_turn_anim = u.d.has_pre_turn_anim;
}

bool MovementSystem::add_unit(const UnitDesc& d) {
    if (find(d.id) >= 0 || d.id < 0 || static_cast<uint32_t>(d.id) == kNoEntity) return false;
    Unit u;
    u.d = d;
    configure(u);
    u.m.pos = d.pos;
    u.m.yaw = wrap_deg(d.yaw_deg);
    u.z = d.z;
    u.primary_locomotor = d.locomotor;  // S7 t06
    for (size_t k = 0; k < 3; ++k) u.tcur[k] = u.ttgt[k] = d.turret_rest[k];  // S8 t24 [port: spawn pose = rest]
    special_idle(u);
    if (d.locomotor == LocomotorKind::Fighter) {  // FIGHTER_FLYING §1.5: P, O (yaw wrap360), D = P, stored O = O
        u.fs.b.p = {d.pos.x, d.pos.y, d.z};
        u.fs.b.o.yaw = fighter_wrap360(d.yaw_deg);
        u.fs.dest = u.fs.b.p;
        u.fs.stored = u.fs.b.o;
    }
    auto it = std::lower_bound(units_.begin(), units_.end(), d.id, [](const Unit& a, ObjectId v) { return a.d.id < v; });
    it = units_.insert(it, std::move(u));
    if (cfg_.mode == MoveMode::Land) {
        const float r = std::max(it->d.radius, 1.0f);
        tree_.insert(static_cast<EntityHandle>(d.id), Aabb{{d.pos.x - r, d.pos.y - r, -1.0f}, {d.pos.x + r, d.pos.y + r, 1.0f}}, 1u);
    } else {
        park_space(*it, 0);
    }
    return true;
}

bool MovementSystem::remove_unit(ObjectId id) {
    const int32_t i = find(id);
    if (i < 0) return false;
    if (cfg_.mode == MoveMode::Land) tree_.remove(static_cast<EntityHandle>(id));
    else space_.clear_track(id);
    fighter_removed(id);  // FIGHTER_FLYING: squadrons, cells, locks, targets naming it
    units_.erase(units_.begin() + i);
    return true;
}

std::vector<ObjectId> MovementSystem::unit_ids() const {
    std::vector<ObjectId> v;
    v.reserve(units_.size());
    for (const Unit& u : units_) v.push_back(u.d.id);
    return v;
}

UnitView MovementSystem::view(ObjectId id) const {
    UnitView v;
    const Unit* u = unit_ptr(id);
    if (!u) return v;
    v.id = id;
    v.pos = u->m.pos;
    v.z = u->z;
    v.yaw = u->m.yaw;
    v.speed = u->m.speed;
    v.max_speed = loco_max_speed(*u);  // S10-int; S7 t14: the jetpack's Alternate_Max_Speed in its airborne kind
    v.has_goal = u->has_goal;
    v.arrived = u->arrived;
    v.failed = u->failed;
    v.pushed = u->pushed;  // a1k t03
    v.goal = u->goal;
    v.replans = u->replans;
    v.space_fallback = u->fallback;
    v.start_frame = u->start_frame;  // OI tactical
    v.formation_speed = u->formation_speed;
    v.team_speed = u->team_cap;  // s78k t03
    v.arrival_turn = u->aturn != LocoState::None;  // s78l t02
    v.arrival_turn_ended = u->aturn_ended;
    v.large_hover_index = u->large_hover_index;
    v.jiggle = u->jiggle;
    v.locomotor = u->d.locomotor;  // §4.3-4.7
    v.secondary_locomotor = u->on_secondary;  // S7 t06
    v.loco_state = u->sm.state;
    v.anim = u->sm.anim.id;
    v.airborne = u->sm.airborne;
    attitude(id, v.roll, v.pitch);
    if (is_fighter(*u)) v.fighter_phase = u->fs.phase;
    v.deploy_state = u->deploy;  // S8 t11
    v.deploy_clip = u->dclip;
    v.turret_elevation = u->tcur[1];  // S8 t24
    v.turret_yaw = u->tcur[2];
    if (u->d.turret && u->taim) {  // s78f t03: the aim state (0x386170 equality: negatives wrapped by +360, within 0.0001)
        const bool dep = u->deploy == LocoState::WalkDeploying || u->deploy == LocoState::WalkDeployed;
        const float ext = dep ? u->d.turret_deployed_extent : u->d.turret_extent;
        const auto w = [](float a) { return a < 0.0f ? a + 360.0f : a; };
        bool eq = true;
        for (size_t k = 0; k < 3; ++k) eq = eq && std::fabs(w(u->tcur[k]) - w(u->ttgt[k])) <= 0.0001f;
        v.turret_aim = std::fabs(u->ttgt[2]) > ext ? int8_t{-2} : eq ? int8_t{1} : int8_t{0};  // s78f t08: -2 cannot aim
    }
    return v;
}

bool MovementSystem::attitude(ObjectId id, float& roll_deg, float& pitch_deg) const {
    roll_deg = 0.0f;
    pitch_deg = 0.0f;
    const Unit* u = unit_ptr(id);
    if (!u) return false;
    if (is_fighter(*u)) {  // FIGHTER_FLYING §1.1
        roll_deg = fighter_wrap180(u->fs.b.o.roll);
        pitch_deg = fighter_wrap180(u->fs.b.o.pitch);
    } else {
        roll_deg = u->sm.lean;  // bike lean / flying roll O +0x84 (0 for the other locomotors)
        if (u->d.locomotor == LocomotorKind::Flying) pitch_deg = u->sm.pitch;  // flying pitch O +0x88 (§6.3)
    }
    return true;
}

std::span<const V2> MovementSystem::land_path(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    if (!u) return {};
    return u->m.path;
}

std::span<const SpaceWaypoint> MovementSystem::space_path(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    if (!u) return {};
    return u->wps;
}

uint32_t MovementSystem::mask_of(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u ? u->lp.mask : 0u;
}

void MovementSystem::update_tree(const Unit& u) {
    const float r = std::max(u.d.radius, 1.0f);
    tree_.update(static_cast<EntityHandle>(u.d.id),
                 Aabb{{u.m.pos.x - r, u.m.pos.y - r, -1.0f}, {u.m.pos.x + r, u.m.pos.y + r, 1.0f}});
}

void MovementSystem::park_space(Unit& u, uint32_t frame) {
    SpaceWaypoint w;
    w.pos = u.m.pos;
    w.heading = u.m.yaw;
    space_.set_track(u.d.id, u.d.space_layer, u.d.radius, frame, {w});
}

// ---------------------------------------------------------------- orders
V2 MovementSystem::pick_land_slot(const Unit& u, V2 p, std::vector<uint64_t>& taken) const {
    const int32_t cx = world_to_cell(p.x), cy = world_to_cell(p.y);
    const int32_t ucx = world_to_cell(u.m.pos.x), ucy = world_to_cell(u.m.pos.y);
    const int32_t comp = regions_.component_at(u.slot, ucx, ucy);
    float best = kInf;
    int32_t bx = cx, by = cy, found_ring = -1;
    const int32_t max_r = 64;
    for (int32_t r = 0; r <= max_r; ++r) {
        if (found_ring >= 0 && r > found_ring + 1) break;
        for (int32_t y = cy - r; y <= cy + r; ++y) {
            for (int32_t x = cx - r; x <= cx + r; ++x) {
                if (std::max(std::abs(x - cx), std::abs(y - cy)) != r) continue;
                if (!grid_.passable_cell(x, y, u.lp.mask)) continue;
                if (comp >= 0 && regions_.component_at(u.slot, x, y) != comp) continue;
                if (contains_key(taken, cell_key(x, y))) continue;
                const float d = distance(p, cell_center(x, y));
                if (d < best) {
                    best = d;
                    bx = x;
                    by = y;
                    if (found_ring < 0) found_ring = r;
                }
            }
        }
    }
    if (found_ring < 0) return p;
    const int32_t rr = static_cast<int32_t>(u.d.radius / kCellSize);
    for (int32_t y = by - rr; y <= by + rr; ++y)
        for (int32_t x = bx - rr; x <= bx + rr; ++x) insert_key(taken, cell_key(x, y));
    return cell_center(bx, by);
}

void MovementSystem::move(const MoveOrder& order, SimRng& rng, uint32_t frame) {
    std::vector<uint32_t> idx = order_indices(order);
    if (!fform_internal_) fform_waypoint(order, idx);  // s78f t02: fighters with a formation append the block instead
    if (idx.empty()) return;
    if (order.switch_locomotor)  // MOVEMENT §6.2: multi-locomotor units swap first (others ignore the bit)
        for (uint32_t i : idx) swap_locomotor(units_[i], true);
    std::vector<uint32_t> delays(idx.size(), 0);
    const bool queued = (order.flags & kMoveFlagQueue) != 0;
    if (queued)
        for (uint32_t& dl : delays) dl = static_cast<uint32_t>(rng.range_int(0, cfg_.constants.max_move_frame_delay));
    // cu8j t01 (MOVEMENT §4.6.2): a new coordinator whose formations come at the next coordinator update. Not for a queued
    // order (it adds a block to the existing formations [M]) or the formation tick's own re-paths (fform_internal_).
    uint32_t serial = 0;
    if (!queued && !fform_internal_) {
        V2 goal = order.destination;
        if (cfg_.mode == MoveMode::Space && space_.has_box()) {
            goal.x = std::clamp(goal.x, space_.box_min().x, space_.box_max().x);
            goal.y = std::clamp(goal.y, space_.box_min().y, space_.box_max().y);
        }
        serial = compose_register(cfg_.mode == MoveMode::Space, true, goal);
    }
    if (cfg_.mode == MoveMode::Land) move_land(order, idx, frame, delays, serial);
    else move_space(order, idx, frame, delays, serial);
    if (!fform_internal_) fform_order(order, idx, frame, serial);  // s78f t02: a new formation holding the order's block
}

void MovementSystem::move_land(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame,
                               const std::vector<uint32_t>& delays, uint32_t serial) {
    rebuild_regions_if_dirty();
    const MovementConstants& k = cfg_.constants;
    std::vector<FormationMember> mem;
    mem.reserve(idx.size());
    for (uint32_t i : idx) {
        const Unit& u = units_[i];
        FormationMember f;
        f.id = u.d.id;
        f.pos = u.m.pos;
        f.max_speed = loco_max_speed(u);  // 0x41b2a0 reads vf74: S7 t14, the jetpack's alternate while airborne-kind
        f.radius = u.d.radius;
        f.spacing = u.d.formation_spacing;
        f.hero = u.d.hero;
        f.type_name = u.d.type_name;
        f.grouping = u.d.grouping;
        f.order = u.d.formation_order;
        f.collision_class = u.d.collision_class;
        f.team_member = u.d.team_member;
        mem.push_back(f);
    }
    const auto compatible = [&](uint32_t a, uint32_t b) {
        const Unit& ua = units_[idx[a]];
        const Unit& ub = units_[idx[b]];
        if (ua.lp.mask != ub.lp.mask) return false;
        const int32_t ca = regions_.component_at(ua.slot, world_to_cell(ua.m.pos.x), world_to_cell(ua.m.pos.y));
        const int32_t cb = regions_.component_at(ub.slot, world_to_cell(ub.m.pos.x), world_to_cell(ub.m.pos.y));
        return ca == cb;
    };
    std::vector<uint64_t> taken;
    PendingComposition* pc = serial != 0 ? &pending_.back() : nullptr;
    // P1 double-click (0x80): speed-band recruitment; LargeHover formations indexed fastest-first (0x439360).
    const bool dbl = (order.flags & kMoveFlagDoubleClick) != 0;
    const float speed_ratio = dbl ? k.double_click_move_max_speed_ratio : 0.0f;
    std::vector<FormationKey> hover_keys;
    std::vector<std::vector<uint32_t>> hover_members;  // indices into idx
    V2 group_c{};
    for (const FormationMember& f : mem) group_c = group_c + f.pos;
    group_c = group_c * (1.0f / static_cast<float>(mem.size()));
    for (const std::vector<uint32_t>& bucket : kmeans_buckets(mem, k.max_formation_area)) {
        for (const std::vector<uint32_t>& f : split_into_formations(mem, bucket, k, compatible, speed_ratio)) {
            // cu8j t01 (MOVEMENT §1.6 / §4.6, correcting cu8i): the formation, its PathFrameDelay stagger (0x43c1b0) and
            // its FormationClass ctor draw come at the next coordinator update (compose_land); [port] the split itself is
            // made here, with the order frame's positions. A queued order adds a block to the existing formations [M]:
            // no new formation, no draw, the per-unit delays.
            if (pc) {
                pc->forms.emplace_back();
                for (uint32_t m : f) pc->forms.back().push_back(units_[idx[m]].d.id);  // seed first
            }
            V2 c{};
            float fspeed = kInf;
            for (uint32_t m : f) {
                c = c + mem[m].pos;
                fspeed = std::min(fspeed, mem[m].max_speed);
            }
            c = c * (1.0f / static_cast<float>(f.size()));
            if (units_[idx[f[0]]].d.movement_class == static_cast<int32_t>(MovementClass::LargeHover)) {  // seed's class
                hover_keys.push_back({fspeed, c});
                hover_members.push_back(f);
            }
            V2 fwd = order.facing_deg != kNoFacing ? dir_from_deg(order.facing_deg) : normalize_retail(order.destination - c);
            if (length_sq(fwd) == 0.0f) fwd = {1.0f, 0.0f};
            for (const Slot& s : layout_formation(mem, f, fwd, k)) {
                Unit& u = units_[idx[s.member]];
                u.large_hover_index = -1;
                V2 target = slot_world(order.destination, fwd, s);
                if (!(order.flags & kMoveFlagSkipValidation)) target = pick_land_slot(u, target, taken);
                u.goal = target;
                u.has_goal = true;
                u.need_path = true;
                u.arrived = false;
                u.failed = false;
                u.facing = order.facing_deg;
                // s78k t01 (MOVEMENT §6.1, §3.5.1): the final facing and the flag-0x20 straight path of this move; a new
                // order drops a running arrival turn.
                u.final_facing = order.final_facing_deg;
                u.forced = (order.flags & kMoveFlagSkipValidation) != 0;
                arrival_reset(u);  // s78l t02: the arrival turn state too
                u.start_frame = pc ? kStartPending : frame + delays[s.member];
                if (pc) {
                    u.pend = serial;
                    u.adopted = false;  // cu8j t02: it leaves the adopt's coordinator for the order's
                }
                u.formation_speed = f.size() > 1 ? fspeed : kInf;
                u.in_formation = f.size() > 1;  // OI tactical
                u.jiggle = V2{};
                u.blocked_frames = 0;
                end_push(u);  // a1k t03 [port]: a new move ends a pushed move
                u.replans = 0;
                u.waits = 0;
                u.hold_until = 0;
                u.speed_cap = kInf;
                u.threat = order.threat_threshold;  // §4.2
                if (u.d.locomotor != LocomotorKind::Default) {  // §4.3-4.6
                    SpecialMotion& sm = u.sm;
                    sm.dest = target;
                    sm.circling = false;
                    sm.idle_frames = 0;
                    sm.turn_back = false;
                    if (u.d.locomotor == LocomotorKind::LandBomber) {
                        // §4.6 state 0x16 (0x646990): a pass to the formation destination + 2 x XYExpansionDistanceLand
                        // ([M] along the approach direction) at Prepare_Strafe_Height + the target z.
                        V2 dir = normalize_retail(target - u.m.pos);
                        if (length_sq(dir) == 0.0f) dir = dir_from_deg(u.m.yaw);
                        u.goal = target + dir * (2.0f * k.xy_expansion_distance_land);
                        sm.pass_z = u.sp.prepare_strafe_height + terrain(target);
                        sm.state = LocoState::WalkStopped;
                    } else if (u.d.locomotor == LocomotorKind::Jetpack && !sm.airborne) {
                        sm.state = LocoState::None;
                    }
                }
            }
        }
    }
    // 0x439360 (skipped with 0x20): index the LargeHover formations. [port] move direction = group centroid ->
    // destination; the retail layout around the centroid and the consumer of the index are not modelled.
    if (!hover_keys.empty() && !(order.flags & kMoveFlagSkipValidation)) {
        V2 dir = normalize_retail(order.destination - group_c);
        if (length_sq(dir) == 0.0f) dir = {1.0f, 0.0f};
        const std::vector<uint32_t> ord = large_hover_formation_order(hover_keys, dir, dbl);
        for (size_t n = 0; n < ord.size(); ++n)
            for (uint32_t m : hover_members[ord[n]]) units_[idx[m]].large_hover_index = static_cast<int32_t>(n);
    }
}

void MovementSystem::move_space(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame,
                                const std::vector<uint32_t>& delays, uint32_t serial) {
    const MovementConstants& k = cfg_.constants;
    // cu8j t01 (MOVEMENT §4.6.4): the order's units wait for the space composition at the next coordinator update, which
    // draws the formations' ctor int(1, FPS) and starts each unit at the update frame + its offset below. [port] The
    // port's k-means / slot layout still picks the destinations, here at the order.
    PendingComposition* pc = serial != 0 ? &pending_.back() : nullptr;
    if (pc) pc->forms.assign(1, {});
    std::vector<FormationMember> mem;
    for (uint32_t i : idx) {
        const Unit& u = units_[i];
        FormationMember f;
        f.id = u.d.id;
        f.pos = u.m.pos;
        f.max_speed = u.lp.max_speed;
        f.radius = u.d.radius;
        f.spacing = u.d.formation_spacing;
        f.hero = u.d.hero;
        f.type_name = u.d.type_name;
        f.grouping = u.d.grouping;
        f.order = u.d.formation_order;
        mem.push_back(f);
    }
    std::vector<ReservedSpot> reserved;
    V2 dest = order.destination;
    if (space_.has_box()) {
        dest.x = std::clamp(dest.x, space_.box_min().x, space_.box_max().x);
        dest.y = std::clamp(dest.y, space_.box_min().y, space_.box_max().y);
    }
    for (const std::vector<uint32_t>& bucket : kmeans_buckets(mem, k.max_formation_area)) {
        for (const std::vector<uint32_t>& f : split_into_formations(mem, bucket, k)) {
            V2 c{};
            for (uint32_t m : f) c = c + mem[m].pos;
            c = c * (1.0f / static_cast<float>(f.size()));
            V2 fwd = order.facing_deg != kNoFacing ? dir_from_deg(order.facing_deg) : normalize_retail(dest - c);
            if (length_sq(fwd) == 0.0f) fwd = {1.0f, 0.0f};
            uint32_t n = 0;
            for (const Slot& s : layout_formation(mem, f, fwd, k)) {
                Unit& u = units_[idx[s.member]];
                V2 target = slot_world(dest, fwd, s);
                if (!(order.flags & kMoveFlagSkipValidation))
                    target = find_space_destination(space_, k, target, u.m.pos, u.d.radius, u.d.space_layer, u.d.id, frame, reserved);
                reserved.push_back({target, u.d.radius * k.occupation_radius_coefficient_space});
                u.goal = target;
                u.has_goal = true;
                u.need_path = true;
                u.arrived = false;
                u.failed = false;
                u.turning = false;
                u.facing = order.facing_deg;
                const uint32_t offset = delays[s.member] + n * k.space_pathfind_frame_delay_delta;
                u.start_frame = pc ? kStartPending : frame + offset;
                if (pc) {
                    pc->forms[0].push_back(u.d.id);
                    pc->offsets.push_back(offset);
                    u.pend = serial;
                    u.adopted = false;
                }
                u.threat = order.threat_threshold;  // §4.2
                if (u.d.locomotor == LocomotorKind::Fighter) fighter_order(u);  // FIGHTER_FLYING §2 step 6
                ++n;
            }
        }
    }
}

void MovementSystem::stop(std::span<const ObjectId> ids) {
    fform_stop(ids);  // S7 t37 / s78f t02: Stop drops the fighter formations
    for (ObjectId id : ids) {
        Unit* u = unit_ptr(id);
        if (!u) continue;
        if (is_air(*u) && u->d.locomotor == LocomotorKind::LandBomber) continue;  // §4.6 [port]: a bombing pass runs out
        u->has_goal = false;
        u->need_path = false;
        u->pend = 0;  // cu8j t01 [port]: a stop leaves a composition still waiting
        if (u->start_frame == kStartPending) u->start_frame = 0;
        u->m.path.clear();
        u->m.next = 0;
        u->m.speed = 0.0f;
        u->m.state = LocoState::WalkStopped;
        end_push(*u);  // a1k t03 [port]: a stop ends a pushed move
        u->following = false;
        arrival_reset(*u);  // s78l t02
        u->wps.clear();
        if (cfg_.mode == MoveMode::Space) park_space(*u, u->path_start);
        switch (u->d.locomotor) {  // §4.3-4.7 [port]: where a stop leaves the special locomotors
            case LocomotorKind::Jetpack: u->sm.state = u->sm.airborne ? LocoState::WalkEndMove : LocoState::WalkStopped; break;
            case LocomotorKind::Flying:
                u->sm.state = LocoState::FlyNoPath;
                u->sm.idle_frames = 0;
                u->sm.dest = u->m.pos;
                break;
            case LocomotorKind::Bike: u->sm.state = LocoState::BikePreStopped; break;
            case LocomotorKind::Fighter:
                u->goal = u->m.pos;
                if (is_fighter(*u)) {
                    fighter_stop(*u);
                } else {
                    u->sm.state = LocoState::FighterIdle;
                }
                break;
            default: break;
        }
    }
}

// L1 systems: Teleport (0x3a8f90): stop, place, re-register (collision tree / stationary space track).
bool MovementSystem::teleport(ObjectId id, V2 pos, float z, float yaw_deg, uint32_t frame) {
    Unit* u = unit_ptr(id);
    if (!u) return false;
    u->has_goal = false;
    u->need_path = false;
    u->arrived = false;
    u->pend = 0;  // cu8j t01 [port]: the order is dropped, so is its waiting composition
    if (u->start_frame == kStartPending) u->start_frame = 0;
    u->m.path.clear();
    u->m.next = 0;
    u->m.speed = 0.0f;
    u->m.state = LocoState::WalkStopped;
    u->following = false;
    arrival_reset(*u);  // s78l t02
    u->wps.clear();
    u->blocked_frames = 0;
    u->hold_until = 0;
    u->nudge_frames = 0;
    end_push(*u);  // a1k t03 [port]: a teleport ends a pushed move
    u->m.pos = pos;
    u->m.yaw = wrap_deg(yaw_deg);
    u->z = z;
    if (u->d.locomotor != LocomotorKind::Default) {  // §4.3-4.7: idle where placed
        u->sm.anim = AnimPlay{};
        u->sm.dest = pos;
        u->goal = pos;
        if (u->d.locomotor == LocomotorKind::Jetpack) {
            u->sm.state = LocoState::WalkStopped;
            u->sm.airborne = false;
        } else if (u->d.locomotor == LocomotorKind::Flying) {
            u->sm.state = LocoState::FlyNoPath;
            u->sm.idle_frames = 0;
        } else if (u->d.locomotor == LocomotorKind::Bike) {
            u->sm.state = LocoState::BikeStopped;
        } else if (u->d.locomotor == LocomotorKind::Fighter) {
            u->sm.state = LocoState::FighterIdle;
            if (is_fighter(*u)) {  // [port] placed at rest: V = 0, D = P, path dropped
                u->fs.b.p = {pos.x, pos.y, z};
                u->fs.b.v = F3{};
                u->fs.b.o.yaw = fighter_wrap360(yaw_deg);
                u->fs.dest = u->fs.b.p;
                u->fs.idx = 0;
                u->fs.hold = 0;
            }
        }
    }
    if (cfg_.mode == MoveMode::Land) update_tree(*u);
    else park_space(*u, frame);
    return true;
}

// ---------------------------------------------------------------- tick
void MovementSystem::tick(uint32_t frame, SimRng& rng, const ParallelFor& pf) {
    const uint32_t interval = cfg_.constants.dynamic_land_quota_reset_interval;
    if (interval < 2 || frame % interval == 0) quota_used_ = 0;
    crush_.clear();
    despawned_.clear();
    fevents_.clear();
    tevents_.clear();  // s78l t02: the host takes them right after the tick (TacticalBattle::play_turn_clip_events)
    for (Unit& u : units_) {  // s78l t02: arrival_turn_ended reports this tick's ends (and drops since the last tick)
        u.aturn_ended = u.aturn_end_pending;
        u.aturn_end_pending = false;
    }
    if (!deferred_compose_) coordinator_update(frame, frame, rng);  // cu8j t01: a host without its own step-10 call
    if (cfg_.mode == MoveMode::Land) tick_land(frame, rng, pf);  // jiggle draws (see header)
    else {
        fform_tick(frame, rng);  // s78f t02: the fighter formations' step (plan, arrival, 0x4192b0, 0x410a60)
        tick_space(frame, pf);
    }
}

bool MovementSystem::set_owner(ObjectId id, int32_t owner) {
    Unit* u = unit_ptr(id);
    if (!u) return false;
    u->d.owner = owner;
    return true;
}

void MovementSystem::crush_pass(const std::vector<WalkResult>& res) {
    // OI tactical: SquashBehavior (vf6 0x629fa0, §5.5), serial, crushers in ascending id.
    std::vector<uint32_t> cand;
    std::vector<uint8_t> crushed(units_.size(), 0);
    for (size_t ci = 0; ci < units_.size(); ++ci) {
        Unit& cr = units_[ci];
        if (!cr.d.crusher || !res[ci].moved || !ground_collider(cr)) continue;  // crush runs only while the crusher moves
        const float r = std::max(cr.d.radius, 1.0f);
        const Box2 cbox{{cr.m.pos.x - r, cr.m.pos.y - r}, {cr.m.pos.x + r, cr.m.pos.y + r}};
        const tactical::Aabb q{{cbox.min.x, cbox.min.y, -1.0f}, {cbox.max.x, cbox.max.y, 1.0f}};
        cand.clear();
        for (EntityHandle h : tree_.query_aabb(q, QueryFilter{}, qctx_)) {
            const int32_t vi = find(static_cast<ObjectId>(h));
            if (vi >= 0 && static_cast<size_t>(vi) != ci) cand.push_back(static_cast<uint32_t>(vi));
        }
        // Players in index order, then ascending id within a player.
        std::sort(cand.begin(), cand.end(), [this](uint32_t a, uint32_t b) {
            const int32_t oa = units_[a].d.owner, ob = units_[b].d.owner;
            return oa != ob ? oa < ob : units_[a].d.id < units_[b].d.id;
        });
        for (uint32_t vi : cand) {
            Unit& v = units_[vi];
            if (crushed[vi] || !crushes(cr, v) || !ground_collider(v)) continue;
            const float vr = std::max(v.d.radius, 1.0f);
            const Box2 vbox{{v.m.pos.x - vr, v.m.pos.y - vr}, {v.m.pos.x + vr, v.m.pos.y + vr}};
            if (find_crush_victims(std::span<const Box2>(&cbox, 1), std::span<const Box2>(&vbox, 1)).empty()) continue;
            crushed[vi] = 1;
            const V2 away = v.m.pos - cr.m.pos;
            if (length_sq(away) > 0.0f) v.m.yaw = wrap_deg(heading_deg(away));  // faces away from the crusher
            crush_.push_back({cr.d.id, v.d.id});
        }
    }
}

void MovementSystem::fill_air_request(const Unit& u, uint32_t frame, SpaceRequest& q) const {
    q.self = u.d.id;
    q.start = u.m.pos;
    q.goal = u.goal;
    q.start_heading = u.m.yaw;
    // FIGHTER_FLYING §6: the formation speed caps the plan ([port] a formation speed <= 0, e.g. the loiter's
    // Min_Speed of a type without one, keeps max speed). S7 t14 (MOVEMENT §3.1.1): the request record takes
    // vf74 / vf75, so a jetpack in its airborne kind (0 / 0xF..0x13) plans with Alternate_Max_Speed /
    // _Rate_Of_Turn. S7 t15 (§3.1.1 [H]): acceleration / deceleration are vf91 / vf92 = the common getters:
    // the override x (effective max speed / Max_Speed), else the NORMAL effective max speed (configure's
    // lp.accel / lp.decel), never the alternate one, airborne or not.
    const float vmax = loco_max_speed(u);
    q.speed = u.formation_speed > 0.0f ? std::min(vmax, u.formation_speed) : vmax;
    q.turn_rate = loco_turn_rate(u);
    q.radius = u.d.radius;
    q.layer = u.d.space_layer;
    q.now = frame;
    q.accel = u.lp.accel;
    q.decel = u.lp.decel;
    q.goal_facing = u.facing;
    q.threat_threshold = u.threat;
    q.space_layer_type = u.d.space_layer_type;
}

bool MovementSystem::air_request(ObjectId id, uint32_t frame, SpaceRequest& out) const {
    const Unit* u = unit_ptr(id);
    if (!u || !is_air(*u)) return false;
    out = SpaceRequest{};
    fill_air_request(*u, frame, out);
    return true;
}

void MovementSystem::plan_land(uint32_t frame, const ParallelFor& pf) {
    std::vector<uint32_t> req;
    for (uint32_t i = 0; i < units_.size(); ++i)
        if (units_[i].need_path && frame >= units_[i].start_frame) req.push_back(i);
    if (req.empty()) return;
    std::vector<LandPath> out(req.size());
    std::vector<SpacePath> air_out(req.size());  // §4.3-4.6: timed paths of land air units
    MovementConstants air_k = cfg_.constants;     // the planner's land branches (MOVEMENT §2.1)
    air_k.xy_expansion_distance_space = air_k.xy_expansion_distance_land;
    air_k.wait_operator_speed_coefficient = air_k.land_wait_operator_speed_coefficient;
    air_k.min_obstacle_cost_space = air_k.min_obstacle_cost_land;
    pf(static_cast<uint32_t>(req.size()), 1, [&](uint32_t b, uint32_t e) {
        LandPathScratch sc;
        SpaceScratch ssc;
        for (uint32_t r = b; r < e; ++r) {
            const Unit& u = units_[req[r]];
            const ThreatFn tf = owner_threat(u);
            if (is_air(u)) {
                SpaceRequest q;
                fill_air_request(u, frame, q);
                q.threat = tf ? &tf : nullptr;
                plan_space_path(air_, air_k, q, ssc, air_out[r]);
                continue;
            }
            if (u.forced) continue;  // s78k t01: the straight path is built below
            LandPathRequest q;
            q.start = u.m.pos;
            q.goal = u.goal;
            q.mask = u.lp.mask;
            q.clearance_radius = u.d.radius > kCellSize ? u.d.radius : 0.0f;
            if (u.threat != kThreatOff && u.d.space_layer_type == 0) q.threat = ThreatCost::make(tf ? &tf : nullptr, u.threat);
            find_land_path(grid_, regions_, q, sc, out[r]);
        }
    });
    for (size_t r = 0; r < req.size(); ++r) {
        Unit& u = units_[req[r]];
        u.need_path = false;
        ++stats_.paths_planned;
        if (is_air(u)) {
            if (air_out[r].fallback) ++stats_.space_fallbacks;
            if (!air_out[r].ok || air_out[r].wps.empty()) {
                ++stats_.path_failures;
                u.has_goal = false;
                u.failed = true;
                continue;
            }
            u.wps = std::move(air_out[r].wps);
            on_air_path(u, frame);
            continue;
        }
        if (u.forced) {
            // s78k t01 (MOVEMENT §6.1 "Flag 0x20 path (land)" [H]): no route - the path is start -> destination, and its
            // skip-passability flag (path +0x5c) holds for the whole path (escape_until past the last waypoint).
            // s78l t02 c ("Forced-path destination check and fallback"): the destination's cell must pass the movement
            // class (0x50e180: a point test, outside the grid = blocked); else the ring search 0x440f90 places it; no
            // spot -> no path, the unit does not move.
            if (!grid_.passable_world(u.goal, u.lp.mask)) {
                V2 spot;
                if (!forced_spot(u, u.goal, spot)) {
                    ++stats_.path_failures;
                    u.has_goal = false;
                    u.failed = true;
                    u.m.path.clear();
                    u.m.next = 0;
                    u.m.speed = 0.0f;
                    continue;
                }
                u.goal = spot;
            }
            u.m.path = {u.m.pos, u.goal};
            u.m.next = 1;
            u.m.escape_until = 2u;
            u.blocked_frames = 0;
            continue;
        }
        if (out[r].ok) {
            u.m.path = std::move(out[r].points);
            u.m.next = 1;
            u.m.escape_until = out[r].start_blocked ? 2u : 0u;
            u.goal = out[r].goal_used;
            u.blocked_frames = 0;
        } else {
            ++stats_.path_failures;
            u.has_goal = false;
            u.failed = true;
            u.m.path.clear();
            u.m.next = 0;
            u.m.speed = 0.0f;
        }
    }
}

// ---------------------------------------------------------------- S7 collision layer (TACTICAL_GAPS §6.7)
// Candidates of unit `ai`: ground colliders in the 160 x 160 square around it, ascending id (index).
void MovementSystem::land_candidates(size_t ai, std::vector<uint32_t>& out) const {
    const Unit& a = units_[ai];
    const tactical::Aabb box{{a.m.pos.x - 80.0f, a.m.pos.y - 80.0f, -1.0f}, {a.m.pos.x + 80.0f, a.m.pos.y + 80.0f, 1.0f}};
    out.clear();
    for (EntityHandle h : tree_.query_aabb(box, QueryFilter{}, qctx_)) {
        const int32_t ci = find(static_cast<ObjectId>(h));
        if (ci >= 0 && static_cast<size_t>(ci) != ai && ground_collider(units_[static_cast<size_t>(ci)]))
            out.push_back(static_cast<uint32_t>(ci));
    }
}

// "Moving" for the checking unit (it has started its order) and for the other participant (as the pre-layer port).
bool MovementSystem::checker_moving(const Unit& u, uint32_t frame) const {
    return u.has_goal && !u.need_path && u.m.next < u.m.path.size() && frame >= u.start_frame;
}
bool MovementSystem::other_moving(const Unit& u) { return u.has_goal && !u.need_path && u.m.next < u.m.path.size(); }

// MovingVsMoving contact test from a's side: closest approach of the straight-line extrapolations within
// MovingVsMovingLookAheadTime (a at max(speed, half its max speed), b at its speed).
bool MovementSystem::mvm_contact(const Unit& a, const Unit& b) const {
    const float look_frames = cfg_.constants.moving_vs_moving_look_ahead_time * 30.0f;
    const V2 dir_a = normalize_retail(a.m.path[a.m.next] - a.m.pos);
    const V2 dir_b = normalize_retail(b.m.path[b.m.next] - b.m.pos);
    const float va_s = std::max(a.m.speed, 0.5f * a.lp.max_speed);
    float t = 0.0f;
    return predict_moving_contact(a.m.pos, dir_a * va_s, b.m.pos, dir_b * b.m.speed, a.d.radius + b.d.radius, look_frames, t);
}

// MovingVsStopped contact: distance along a's remaining path to touching b within the look-ahead, or -1.
float MovementSystem::mvs_contact(const Unit& a, const Unit& b) const {
    const float look_frames = cfg_.constants.moving_vs_moving_look_ahead_time * 30.0f;
    const float rs = a.d.radius + b.d.radius;
    return path_contact_distance(a.m.pos, a.m.path, a.m.next, b.m.pos, rs, a.lp.max_speed * look_frames + rs);
}

// Keep test (vf +0x50) [port]: both participants still exist and collide, and the classification that created the
// collision still holds this frame.
bool MovementSystem::collision_applies(const CollisionObject& c, uint32_t frame) const {
    const int32_t ia = find(c.a), ib = find(c.b);
    if (ia < 0 || ib < 0) return false;
    const Unit& a = units_[static_cast<size_t>(ia)];
    const Unit& b = units_[static_cast<size_t>(ib)];
    if (!ground_collider(a) || !ground_collider(b)) return false;
    if (!collision_classes_interact(a.d.collision_class, b.d.collision_class)) return false;
    switch (c.kind) {
        case CollisionKind::MovingVsMoving:
            return !crushes(a, b) && checker_moving(a, frame) && other_moving(b) && mvm_contact(a, b);
        case CollisionKind::MovingVsStopped:
            return !crushes(a, b) && checker_moving(a, frame) && !other_moving(b) && mvs_contact(a, b) >= 0.0f;
        case CollisionKind::StoppedVsStopped:
            return !a.has_goal && !b.has_goal && !a.pushed && !b.pushed &&
                   overlap_ratio(a.m.pos, a.d.radius, b.m.pos, b.d.radius) > cfg_.constants.minimum_stopped_vs_stopped_overlap;
    }
    return false;
}

// Collision checks (0x508170 side, §5.2): every FramesPerCollisionCheck frames per unit, ascending id; a qualifying
// pair without a collision object gets one (pair filter 0x5c7900), appended in list order.
void MovementSystem::detect_collisions(uint32_t frame) {
    const MovementConstants& k = cfg_.constants;
    std::vector<uint32_t> cand;
    for (size_t ai = 0; ai < units_.size(); ++ai) {
        Unit& a = units_[ai];
        if (!ground_collider(a)) continue;  // §4.3-4.6: air units take no part in land collision
        if (a.check_countdown > 0) {
            --a.check_countdown;
            continue;
        }
        a.check_countdown = static_cast<int32_t>(k.frames_per_collision_check) - 1;
        ++stats_.collision_checks;
        land_candidates(ai, cand);
        if (checker_moving(a, frame)) {
            for (uint32_t bi : cand) {
                const Unit& b = units_[bi];
                if (!collision_classes_interact(a.d.collision_class, b.d.collision_class)) continue;
                // OI tactical: a crusher drives through units it can crush (§5.4 case 3 "push/crush attempt" [port]);
                // the crush pass (§5.5) kills them.
                if (crushes(a, b)) continue;
                if (coll_.has_pair(a.d.id, b.d.id)) continue;
                CollisionKind kind = CollisionKind::MovingVsMoving;
                if (other_moving(b)) {
                    if (!mvm_contact(a, b)) continue;
                    kind = CollisionKind::MovingVsMoving;
                } else {
                    if (mvs_contact(a, b) < 0.0f) continue;
                    kind = CollisionKind::MovingVsStopped;
                }
                coll_.add(kind, a.d.id, b.d.id, frame);
                ++stats_.collisions_created;
            }
        } else if (!a.has_goal && !a.pushed) {
            for (uint32_t bi : cand) {
                const Unit& b = units_[bi];
                // a1k t03: a pushed unit's locomotor moves, so it is not stopped.
                if (b.has_goal || b.pushed || !collision_classes_interact(a.d.collision_class, b.d.collision_class)) continue;
                if (overlap_ratio(a.m.pos, a.d.radius, b.m.pos, b.d.radius) <= k.minimum_stopped_vs_stopped_overlap) continue;
                if (coll_.has_pair(a.d.id, b.d.id)) continue;
                coll_.add(CollisionKind::StoppedVsStopped, a.d.id, b.d.id, frame);
                ++stats_.collisions_created;
            }
        }
    }
}

// Driver 0x50a780: keep / cancel, priority sort, resolution under the dynamic quota.
void MovementSystem::drive_collisions(uint32_t frame) {
    // Speed slaving is re-applied by the live MovingVsMoving collisions on each unit's check frames.
    const int32_t check_phase = static_cast<int32_t>(cfg_.constants.frames_per_collision_check) - 1;
    for (Unit& u : units_)
        if (u.check_countdown == check_phase) u.speed_cap = kInf;
    // Per unit this frame: 1 = resolved a MovingVsStopped collision as the mover, 2 = pushed by a StoppedVsStopped one.
    std::vector<uint8_t> acted(units_.size(), 0);
    const float half = 0.5f * kCollisionCrowdBox;
    coll_.run(
        frame, [&](CollisionObject& c) { return collision_applies(c, frame); },
        [&](const CollisionObject& c) {
            // 5 x the land object-grid objects in the 160 x 160 box on the participants' XY midpoint ([port]: the
            // collision tree's ground colliders, AABB overlap - the candidate query's semantics; both participants
            // count). The keep pass ran first, so both participants exist.
            const V2 pa = units_[static_cast<size_t>(find(c.a))].m.pos, pb = units_[static_cast<size_t>(find(c.b))].m.pos;
            const V2 mid = (pa + pb) * 0.5f;
            const tactical::Aabb box{{mid.x - half, mid.y - half, -1.0f}, {mid.x + half, mid.y + half, 1.0f}};
            uint32_t n = 0;
            for (EntityHandle h : tree_.query_aabb(box, QueryFilter{}, qctx_)) {
                const Unit* o = unit_ptr(static_cast<ObjectId>(h));
                if (o && ground_collider(*o)) ++n;
            }
            return n;
        },
        [&] { return quota_used_ < cfg_.constants.dynamic_land_complexity_quota; },
        [&](CollisionObject& c, bool full) { resolve_collision(c, full, frame, acted); });
    stats_.collisions_cancelled += coll_.last_cancelled();
    stats_.collisions_resolved += coll_.last_resolved();
}

// MovingVsMoving response of participant x to y (the pre-layer port behaviour; retail 0x677650... untraced [L]):
// speed slave behind a slower mover ahead, side-step on a head-on course, otherwise the higher id yields.
void MovementSystem::mvm_response(Unit& x, const Unit& y, uint32_t frame) {
    const MovementConstants& k = cfg_.constants;
    const V2 dir_x = normalize_retail(x.m.path[x.m.next] - x.m.pos);
    const V2 dir_y = normalize_retail(y.m.path[y.m.next] - y.m.pos);
    const float cosd = dot(dir_x, dir_y);
    const bool ahead = dot(dir_x, y.m.pos - x.m.pos) > 0.0f;
    if (cosd > 0.5f) {
        // A held mover (speed 0) is not followed, so slaving chains can never deadlock.
        if (ahead && y.m.speed > 0.0f) x.speed_cap = std::min(x.speed_cap, y.m.speed);
    } else if (cosd < -0.5f) {
        x.nudge = formation_right(dir_x) * std::min(1.0f, 0.5f * x.lp.max_speed);  // side-step
        x.nudge_frames = k.frames_per_collision_check;
    } else if (x.d.id > y.d.id && ahead) {
        x.hold_until = frame + k.frames_per_collision_check;  // yield to the lower id
    }
}

void MovementSystem::resolve_collision(CollisionObject& c, bool full, uint32_t frame, std::vector<uint8_t>& acted) {
    const MovementConstants& k = cfg_.constants;
    const size_t ai = static_cast<size_t>(find(c.a));
    Unit& a = units_[ai];
    const uint32_t bi = static_cast<uint32_t>(find(c.b));
    Unit& b = units_[bi];
    // check_countdown value right after a unit's collision check this frame (detect_collisions).
    const int32_t check_phase = static_cast<int32_t>(k.frames_per_collision_check) - 1;
    switch (c.kind) {
        case CollisionKind::MovingVsMoving:
            // [port] one object per pair: each participant responds from its own side (its own contact test) on its
            // own collision-check frames; the speed cap / hold / side-step it sets lasts until its next check, as the
            // pre-layer per-unit checks did. Full and fallback are the same (no dynamic pathfinder use).
            if (a.check_countdown == check_phase) mvm_response(a, b, frame);
            if (b.check_countdown == check_phase && !crushes(b, a) && checker_moving(b, frame) && other_moving(a) &&
                mvm_contact(b, a))
                mvm_response(b, a, frame);
            return;
        case CollisionKind::MovingVsStopped: {
            if (c.state == CollisionState::TryAgainLater && frame < c.retry) return;
            // An earlier resolution this frame may have re-routed the mover.
            const float d = mvs_contact(a, b);
            if (d < 0.0f) {
                c.state = CollisionState::Resolved;
                return;
            }
            // [port] a mover resolves its blockers on its own collision-check frames (the FramesPerCollisionCheck
            // cadence of §5.2), one per check, the first in priority order; the others wait for its next check.
            if ((acted[ai] & 1u) || a.check_countdown != check_phase) return;
            acted[ai] |= 1u;
            if (resolve_blocker(a, bi, d, frame, full)) {
                c.state = CollisionState::Resolved;
            } else {
                c.state = CollisionState::TryAgainLater;
                c.retry = frame + k.frames_per_collision_check;
            }
            return;
        }
        case CollisionKind::StoppedVsStopped: {
            // [port] each participant is pushed apart on its own collision-check frames (one push per
            // FramesPerCollisionCheck frames, as the pre-layer port nudged each unit from its own check), at most once
            // per frame across its collisions.
            const bool push_a = !(acted[ai] & 2u) && a.check_countdown == check_phase;
            const bool push_b = !(acted[bi] & 2u) && b.check_countdown == check_phase;
            if (!push_a && !push_b) return;
            const V2 pa = a.m.pos, pb = b.m.pos;
            const float step = std::min(2.0f, std::max(0.5f, 0.5f * (a.d.radius + b.d.radius - distance(pa, pb))));
            V2 away_a = pa - pb;
            if (length_sq(away_a) == 0.0f) away_a = a.d.id < b.d.id ? V2{-1.0f, 0.0f} : V2{1.0f, 0.0f};
            V2 away_b = pb - pa;
            if (length_sq(away_b) == 0.0f) away_b = b.d.id < a.d.id ? V2{-1.0f, 0.0f} : V2{1.0f, 0.0f};
            if (push_a) {
                acted[ai] |= 2u;
                if (try_nudge(a.m, normalize_retail(away_a) * step, a.lp.mask, &grid_)) update_tree(a);
            }
            if (push_b) {
                acted[bi] |= 2u;
                if (try_nudge(b.m, normalize_retail(away_b) * step, b.lp.mask, &grid_)) update_tree(b);
            }
            return;
        }
    }
}

// MovingVsStopped resolution (decision 0x684f90 reduced to the port's cases): arrival at a blocked destination;
// full (vf +0x20): path around through the dynamic pathfinder (adds its complexity to the quota counter); fallback
// (vf +0x18) or a failed search: try again later, and after repeated waits push the blocker aside. True = resolved.
bool MovementSystem::resolve_blocker(Unit& a, uint32_t blocker, float contact, uint32_t frame, bool full) {
    const MovementConstants& k = cfg_.constants;
    Unit& b = units_[blocker];
    const float rs = a.d.radius + b.d.radius;
    // A stopped unit sits on the destination: arrive here (LandDestinationProximity).
    const float remaining = remaining_length(a.m);
    if (distance(b.m.pos, a.goal) <= rs + k.land_destination_proximity &&
        remaining <= contact + rs + k.land_destination_proximity) {
        // The path now ends where the unit touches the blocker; it arrives there.
        size_t seg = 0;
        const V2 stop_at = point_along(a.m.pos, a.m.path, a.m.next, contact, seg);
        a.m.path = {a.m.pos, stop_at};
        a.m.next = 1;
        a.m.escape_until = 0;
        a.goal = stop_at;
        ++stats_.early_arrivals;
        return true;
    }
    // A Giant Vehicle pushes a stopped blocker it does not crush aside and keeps its path (decision 0x684f90, push
    // 0x6859a0; A1_READINGS §36.4).
    if (a.d.collision_class == kUccGiantVehicle && push_aside(a, b)) {
        a.waits = 0;
        return true;
    }
    if (full) {
        const size_t ai = static_cast<size_t>(&a - units_.data());
        std::vector<uint32_t> cand;
        land_candidates(ai, cand);
        std::vector<DynamicObstacle> obs;
        for (uint32_t c : cand) {
            const Unit& o = units_[c];
            if (o.has_goal) continue;
            if (!collision_classes_interact(a.d.collision_class, o.d.collision_class)) continue;
            if (crushes(a, o)) continue;  // OI tactical
            obs.push_back({o.m.pos, o.d.radius});
        }
        // Target: the first path point past the blockers.
        float s = contact + 2.0f * rs + kCellSize;
        size_t seg = 0;
        V2 target = point_along(a.m.pos, a.m.path, a.m.next, s, seg);
        for (int guard = 0; guard < 64 && seg < a.m.path.size(); ++guard) {
            bool inside = false;
            for (const DynamicObstacle& o : obs) {
                const float rr = o.radius + a.d.radius;
                if (length_sq(target - o.pos) < rr * rr) inside = true;
            }
            if (!inside) break;
            s += kCellSize;
            target = point_along(a.m.pos, a.m.path, a.m.next, s, seg);
        }
        std::vector<V2> local;
        uint32_t complexity = 0;
        const bool ok = dynamic_repath(grid_, a.m.pos, target, a.lp.mask, a.d.radius, a.d.collision_class == kUccGiantVehicle,
                                       obs, k, local, complexity);
        quota_used_ += complexity;
        ++stats_.dynamic_repaths;
        if (ok && local.size() >= 2) {
            std::vector<V2> np = std::move(local);
            for (size_t i = seg; i < a.m.path.size(); ++i)
                if (!(np.back() == a.m.path[i])) np.push_back(a.m.path[i]);
            a.m.path = std::move(np);
            a.m.next = 1;
            a.m.escape_until = 0;
            a.waits = 0;
            return true;
        }
    } else {
        ++stats_.quota_deferrals;
    }
    // Try again later; after repeated waits push the blocker aside.
    a.hold_until = frame + k.frames_per_collision_check;
    if (++a.waits > kWaitsBeforePush) {
        a.waits = 0;
        const V2 dir = a.m.next < a.m.path.size() ? normalize_retail(a.m.path[a.m.next] - a.m.pos) : V2{1.0f, 0.0f};
        const V2 rel = b.m.pos - a.m.pos;
        V2 side = perp_left(dir);
        if (cross(dir, rel) < 0.0f) side = side * -1.0f;
        if (try_nudge(b.m, side * (rs + 2.0f), b.lp.mask, &grid_)) update_tree(b);
        a.hold_until = 0;
    }
    return false;
}

// Push 0x6859a0 (A1_READINGS §36.4, §40): direction = a's heading turned 45 degrees toward b's side; the free-position
// search (push_spot) and a land path to the spot; b walks it as a pushed move over its order move (§40.3: no return
// move). A deploying / deployed b is not pushed. [port] A b already on a pushed move counts as pushed (the original
// pushes a blocker that is moving again, decision 0x684f90 step 2).
bool MovementSystem::push_aside(const Unit& a, Unit& b) {
    if (b.pushed) return true;
    if (!(b.lp.max_speed > 0.0f)) return false;
    if (b.deploy == LocoState::WalkDeploying || b.deploy == LocoState::WalkDeployed) return false;
    if (a.m.next >= a.m.path.size()) return false;
    const V2 h = normalize_retail(a.m.path[a.m.next] - a.m.pos);
    if (!(length_sq(h) > 0.0f)) return false;
    constexpr float kCos45 = 0.70710678f;
    const V2 rel = b.m.pos - a.m.pos;
    const float sin45 = cross(h, rel) <= 0.0f ? -kCos45 : kCos45;  // -45 degrees when b is on a's right
    const V2 dir{h.x * kCos45 - h.y * sin45, h.x * sin45 + h.y * kCos45};
    V2 spot;
    LandPath out;
    if (push_spot(b, dir, spot)) {
        // 0x67b730: b's land path to the spot (synchronous); no path fails the push.
        LandPathRequest q;
        q.start = b.m.pos;
        q.goal = spot;
        q.mask = b.lp.mask;
        q.clearance_radius = b.d.radius > kCellSize ? b.d.radius : 0.0f;
        LandPathScratch sc;
        find_land_path(grid_, regions_, q, sc, out);
    }
    if (!out.ok || out.points.size() < 2) {
        ++stats_.push_failures;
        return false;
    }
    b.pushed = true;
    b.push_path = std::move(out.points);
    b.push_next = 1;
    b.push_blocked = 0;
    b.nudge_frames = 0;
    ++stats_.pushes;
    return true;
}

// a1k t03: the push search's footprint radius 0x3969f0 = the length of the hard extents (A1_READINGS §40.2) and the
// mover's extent 0x372040 (§40.1). [port] A type without hard extents uses its soft radius (the model fallback of
// 0x371be0 is not loaded).
static float hard_footprint(const UnitDesc& d) {
    return d.hard_x > 0.0f || d.hard_y > 0.0f ? std::sqrt(d.hard_x * d.hard_x + d.hard_y * d.hard_y) : d.radius;
}
static float hard_y_extent(const UnitDesc& d) { return d.hard_y > 0.0f ? d.hard_y : d.radius; }

// Free-position search 0x440f90 with the push's set-up 0x440be0 (A1_READINGS §40.1-40.2): rings around b from radius 0
// (step 1, +0.8 per ring up to min(1.5 r, 10), at most 20 rings), n = trunc(R 2pi / r) + 1 samples (at least 4 after
// ring 0) at theta = 2pi i / n counter-clockwise from `dir`; mode 2: only theta <= pi/4 lies on the ring, the other
// samples 0.33 from b. The first sample that is on the map, reachable, unoccupied and off the paths of the movers
// heading into b is the spot. r = b's hard footprint radius. [port] reachability = b's mask and region component;
// occupancy = the collision tree (ground colliders other than b whose hard footprint box overlaps the box
// r + FormationSpacing x 5); the movers' remaining paths stand for their path approximations (clearance = mover's Y
// extent + r + 4).
bool MovementSystem::push_spot(const Unit& b, V2 dir, V2& out) const {
    constexpr float kPi = 3.1415927f;
    constexpr float kCone = 0.78539819f;  // pi / 4 (+0x3c)
    constexpr float kOutsideCone = 0.33f;  // +0x40
    constexpr uint32_t kRings = 20;
    const float r = hard_footprint(b.d);
    const float half = r + b.d.formation_spacing * 20.0f * 0.25f;  // + the margin +0x48
    float max_step = r * 1.5f;
    if (max_step == 0.0f) max_step = 5.0f;
    max_step = std::min(max_step, 10.0f);
    // 0x505490: the other participants of b's MovingVsStopped collisions (kind 3), in collision-list order.
    struct Avoid {
        const Unit* u;
        float clearance;
    };
    std::vector<Avoid> avoid;
    for (const CollisionObject& c : coll_.list()) {
        if (c.kind != CollisionKind::MovingVsStopped || c.b != b.d.id) continue;
        const Unit* m = unit_ptr(c.a);
        if (!m) continue;
        if (m->m.next < m->m.path.size()) avoid.push_back({m, hard_y_extent(m->d) + r + 4.0f});
    }
    // Orientation (0x20acd0): yaw in degrees in [0, 360), then radians as the search reads it.
    float yaw_deg = 0.0f;
    if (dir.x != 0.0f || dir.y != 0.0f) {
        yaw_deg = dmath::atan2(dir.y, dir.x) * 57.295776f;
        if (yaw_deg < 0.0f) yaw_deg += 360.0f;
    }
    const float yaw = yaw_deg * 0.017453292f;
    const float sy = fighter_sin(yaw), cy = fighter_cos(yaw);
    // 0x50ecd0: both cells pass b's mask and lie in one connected region of it.
    const int32_t bcx = world_to_cell(b.m.pos.x), bcy = world_to_cell(b.m.pos.y);
    if (!grid_.passable_cell(bcx, bcy, b.lp.mask)) return false;
    const int32_t comp = regions_built_ ? regions_.component_at(b.slot, bcx, bcy) : -1;
    // The tree holds each unit's soft-radius box (update_tree); widen the query so a hard footprint larger than it is
    // still found.
    float widen = 0.0f;
    for (const Unit& o : units_) widen = std::max(widen, hard_footprint(o.d) - std::max(o.d.radius, 1.0f));
    float ring_r = 0.0f, step = 1.0f;
    for (uint32_t ring = 0; ring < kRings; ++ring) {
        uint32_t n = 1;
        if (r > 0.0f) n = static_cast<uint32_t>(static_cast<int64_t>(ring_r * 2.0f * kPi / r)) + 1u;
        if (ring != 0) n = std::max(4u, n);
        for (uint32_t i = 0; i < n; ++i) {
            const float th = (static_cast<float>(i) / static_cast<float>(n)) * kPi * 2.0f;
            const float rho = th > kCone ? kOutsideCone : ring_r;
            const float st = fighter_sin(th), ct = fighter_cos(th);
            const V2 p{b.m.pos.x + (ct * cy - st * sy) * rho, b.m.pos.y + (st * cy + ct * sy) * rho};
            const int32_t cx = world_to_cell(p.x), cyy = world_to_cell(p.y);
            if (!grid_.in_range(cx, cyy) || !grid_.passable_cell(cx, cyy, b.lp.mask)) continue;
            if (comp >= 0 && regions_.component_at(b.slot, cx, cyy) != comp) continue;
            bool occupied = false;
            const float q = half + widen;
            const tactical::Aabb box{{p.x - q, p.y - q, -1.0f}, {p.x + q, p.y + q, 1.0f}};
            for (EntityHandle eh : tree_.query_aabb(box, QueryFilter{}, qctx_)) {
                const int32_t oi = find(static_cast<ObjectId>(eh));
                if (oi < 0) continue;
                const Unit& o = units_[static_cast<size_t>(oi)];
                if (o.d.id == b.d.id || !ground_collider(o)) continue;
                const float ro = hard_footprint(o.d);
                if (std::abs(o.m.pos.x - p.x) <= ro + half && std::abs(o.m.pos.y - p.y) <= ro + half) {
                    occupied = true;
                    break;
                }
            }
            if (occupied) continue;
            bool on_path = false;
            for (const Avoid& av : avoid) {
                V2 s0 = av.u->m.pos;
                for (size_t k = av.u->m.next; k < av.u->m.path.size() && !on_path; ++k) {
                    const V2 s1 = av.u->m.path[k];
                    const V2 seg = s1 - s0;
                    const float l2 = length_sq(seg);
                    const float t = l2 > 0.0f ? std::clamp(dot(p - s0, seg) / l2, 0.0f, 1.0f) : 0.0f;
                    on_path = length_sq(p - (s0 + seg * t)) < av.clearance * av.clearance;
                    s0 = s1;
                }
                if (on_path) break;
            }
            if (on_path) continue;
            out = p;
            return true;
        }
        ring_r += step;
        step = std::min(step + 0.8f, max_step);
    }
    return false;
}

void MovementSystem::end_push(Unit& u) {
    u.pushed = false;
    u.push_path.clear();
    u.push_next = 0;
    u.push_blocked = 0;
}

void MovementSystem::tick_land(uint32_t frame, SimRng& rng, const ParallelFor& pf) {
    rebuild_regions_if_dirty(pf);
    tree_.maintain(static_cast<double>(frame) / 30.0);  // shape only; query results do not depend on it
    plan_land(frame, pf);
    detect_collisions(frame);  // S7: checks create collision objects
    drive_collisions(frame);   // S7: cancel / priority sort / resolve (TACTICAL_GAPS §6.7)
    team_caps();  // s78k t03: serial, before the parallel walk reads Unit::team_cap
    // OI tactical: formation jiggle (0x42c270, §3.5 step 6) - serial, ascending id, before the parallel walk.
    for (Unit& u : units_) {
        if (!u.in_formation || !u.has_goal || u.need_path || frame < u.start_frame || is_air(u)) continue;
        if (!(u.m.speed > 0.0f) || !(u.lp.max_speed > 0.0f)) continue;  // a moving frame
        const float rag = u.d.formation_raggedness, mx = u.d.max_jiggle_distance;
        if (!(rag > 0.0f) || !(mx > 0.0f)) continue;
        const float frac = u.m.speed / u.lp.max_speed;
        u.jiggle.x = std::clamp(u.jiggle.x + rng.range_float(-1.0f, 1.0f) * frac * rag, -mx, mx);
        u.jiggle.y = std::clamp(u.jiggle.y + rng.range_float(-1.0f, 1.0f) * frac * rag, -mx, mx);
    }
    std::vector<WalkResult> res(units_.size());
    pf(static_cast<uint32_t>(units_.size()), 64, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            Unit& u = units_[i];
            if (is_air(u)) continue;  // §4.3-4.6: serial pass below
            const bool bike = u.d.locomotor == LocomotorKind::Bike;
            const float yaw0 = u.m.yaw;
            // a1k t03 (A1_READINGS §40.3): the pushed move is the top of the path stack; the order path waits under it.
            if (u.pushed && u.deploy == LocoState::None) {
                std::swap(u.m.path, u.push_path);
                std::swap(u.m.next, u.push_next);
                const uint32_t escape = u.m.escape_until;
                u.m.escape_until = 0;
                res[i] = walk_step(u.m, u.lp, kInf, &grid_, nullptr);
                u.m.escape_until = escape;
                std::swap(u.m.path, u.push_path);
                std::swap(u.m.next, u.push_next);
                if (bike) bike_frame(u, yaw0, true);
                continue;
            }
            // s78k t01 / s78l t02 (MOVEMENT §3.5.1): the arrival turn runs in the serial pass below (it reads the clip
            // data hook).
            if (u.aturn != LocoState::None && !u.has_goal && u.d.locomotor == LocomotorKind::Default) continue;
            // S8 t11: a Walk in a deploy state does not move (MovementSystem::deploy).
            if (!u.has_goal || u.need_path || frame < u.start_frame || u.deploy != LocoState::None) {
                if (bike) bike_frame(u, yaw0, false);
                continue;
            }
            // s78k t03 / s78l t01: a team member walks at the team speed (team_caps) - the container's effective max speed
            // replaces its own (no member factor enters), in a formation or not; a stunned member (factor 0) stays put.
            float cap = std::min(u.formation_speed, u.speed_cap);
            LocomotorParams lp = u.lp;
            if (u.team_cap < kInf && u.speed_factor > 0.0f) lp.max_speed = u.team_cap;
            if (frame < u.hold_until) cap = 0.0f;
            WalkExtras x;  // OI tactical: formation members (jiggle, side-error steering)
            x.jiggle = u.jiggle;
            x.side_error = true;
            res[i] = walk_step(u.m, lp, cap, &grid_, u.in_formation ? &x : nullptr);
            if (bike) bike_frame(u, yaw0, true);  // §4.5: states 0x20-0x25 and the lean (writes only u)
        }
    });
    // s78l t02 (MOVEMENT §3.5.1 states 0x0F-0x15): the arrival turns, serial in ascending id; their clip plays become
    // this tick's turn clip events.
    for (Unit& u : units_) {
        if (u.aturn == LocoState::None || u.has_goal || u.d.locomotor != LocomotorKind::Default || is_air(u)) continue;
        if (u.pushed && u.deploy == LocoState::None) continue;  // the pushed walk runs (parallel pass)
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
        const WalkResult& r = res[i];
        if (u.pushed) {
            // a1k t03: the pushed path ended -> the move underneath continues (an idle unit stays at the spot).
            // [port] a pushed walk blocked for kReplanAfterBlocked frames ends where it is.
            if (r.blocked) ++stats_.blocked_steps;
            if (r.arrived || (r.blocked && ++u.push_blocked >= kReplanAfterBlocked)) {
                end_push(u);
            } else if (r.moved) {
                u.push_blocked = 0;
            }
        } else {
            // s78l t02 (MOVEMENT §3.5.1 step 1): End Move brought to a stop short of the last point (0x14 -> 0x15: the walk
            // did not move this frame - blocked, or held at speed 0 by a cap) on the last segment within 5 units of the
            // last point, measured along the segment, counts as arrived (the index steps on; the unit stays where it is).
            // [port] Only frames the walk ran (no re-plan pending, not deploying, started, the locomotor not suspended), and
            // only frames it did not move: retail also enters 0x15 at a speed in (0, decel], which the port's walker
            // covers by walking on to the point; the blocked-frame case (no 0x14 speed test) is [M].
            const bool close = !r.arrived && !r.moved && u.has_goal && !u.need_path && frame >= u.start_frame &&
                               u.deploy == LocoState::None && u.speed_factor > 0.0f && u.m.state == LocoState::WalkEndMove &&
                               u.m.path.size() >= 2 && static_cast<size_t>(u.m.next) + 1 == u.m.path.size() &&
                               dot(u.m.pos - u.m.path.back(),
                                   normalize_retail(u.m.path.back() - u.m.path[u.m.path.size() - 2])) > -5.0f;
            if (close) {
                u.m.next = static_cast<uint32_t>(u.m.path.size());
                u.m.speed = 0.0f;
                u.m.state = LocoState::WalkStopped;
                u.blocked_frames = 0;
            }
            if (r.arrived || close) {
                u.has_goal = false;
                u.arrived = true;
                land_arrival_turn(u);  // s78k t01: state 0x15
            }
            if (r.blocked && !close) {
                ++stats_.blocked_steps;
                if (++u.blocked_frames >= kReplanAfterBlocked) {
                    u.blocked_frames = 0;
                    if (++u.replans > kMaxReplans) {
                        u.has_goal = false;
                        u.failed = true;
                    } else {
                        u.need_path = true;
                        u.start_frame = frame + 1;
                        ++stats_.replans;
                    }
                }
            } else if (r.moved) {
                u.blocked_frames = 0;
            }
        }
        if (u.nudge_frames > 0) {
            --u.nudge_frames;
            try_nudge(u.m, u.nudge, u.lp.mask, &grid_);
        }
        if (height_) u.z = height_(u.m.pos.x, u.m.pos.y);
        update_tree(u);
    }
    crush_pass(res);  // OI tactical
    for (ObjectId id : despawned_) remove_unit(id);  // §4.6: bombers at path end (reported, then dropped)
}

void MovementSystem::tick_space(uint32_t frame, const ParallelFor& pf) {
    const MovementConstants& k = cfg_.constants;
    std::vector<uint32_t> req;
    for (uint32_t i = 0; i < units_.size(); ++i)
        if (units_[i].need_path && frame >= units_[i].start_frame) req.push_back(i);
    if (!req.empty()) {
        space_.build_index();  // OPT3 t12: the parallel planners' occupancy queries visit only nearby entries
        std::vector<SpacePath> out(req.size());
        pf(static_cast<uint32_t>(req.size()), 1, [&](uint32_t b, uint32_t e) {
            SpaceScratch sc;
            for (uint32_t r = b; r < e; ++r) {
                const Unit& u = units_[req[r]];
                SpaceRequest q;
                q.self = u.d.id;
                q.start = u.m.pos;
                q.goal = u.goal;
                q.start_heading = u.m.yaw;
                q.speed = u.lp.max_speed;
                q.turn_rate = u.lp.turn_rate;
                q.radius = u.d.radius;
                q.layer = u.d.space_layer;
                q.now = frame;
                q.accel = u.lp.accel;      // OI tactical: speed-change operators
                q.decel = u.lp.decel;
                q.goal_facing = u.facing;  // OI tactical: facing mode (kNoFacing = none)
                const ThreatFn tf = owner_threat(u);  // §4.2: threat mode
                q.threat_threshold = u.threat;
                q.space_layer_type = u.d.space_layer_type;
                q.threat = tf ? &tf : nullptr;
                plan_space_path(space_, k, q, sc, out[r]);
            }
        });
        for (size_t r = 0; r < req.size(); ++r) {
            Unit& u = units_[req[r]];
            u.need_path = false;
            ++stats_.paths_planned;
            if (out[r].fallback) ++stats_.space_fallbacks;
            u.fallback = out[r].fallback;
            if (!out[r].ok || out[r].wps.empty()) {
                ++stats_.path_failures;
                u.has_goal = false;
                u.failed = true;
                if (is_fighter(u)) {  // [port] no path: idle where it is (§4.3 step 5 solo entry) instead of coasting
                    u.fs.dest = f3_add(u.fs.b.p, u.fs.b.v);
                    u.fs.stored = u.fs.b.o;
                    if (u.sm.state == LocoState::FighterMoving) f_enter_idle(u);
                }
                continue;
            }
            u.wps = std::move(out[r].wps);
            u.fs.idx = 0;  // fighters: path +0x60
            u.path_start = frame;
            u.path_lag = 0.0f;  // S10-int
            u.following = true;
            space_.set_track(u.d.id, u.d.space_layer, u.d.radius, frame, u.wps);
        }
    }
    std::vector<SpacePose> pose(units_.size());
    pf(static_cast<uint32_t>(units_.size()), 64, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            Unit& u = units_[i];
            if (is_fighter(u)) continue;  // FIGHTER_FLYING: serial pass below
            if (u.following) {
                // S10-int: playback time runs at the speed factor (lag += factor - 1 per frame; 0 at factor 1).
                u.path_lag += u.speed_factor - 1.0f;
                pose[i] = eval_space_path(u.wps, static_cast<float>(frame - u.path_start + 1) + u.path_lag);
                u.m.pos = pose[i].pos;
                u.m.yaw = wrap_deg(pose[i].heading);
                u.m.speed = pose[i].speed;
            } else if (u.turning) {
                const float t = u.turn_frames > 0
                                    ? std::min(1.0f, static_cast<float>(frame - u.turn_start + 1) / static_cast<float>(u.turn_frames))
                                    : 1.0f;
                u.m.yaw = wrap_deg(u.turn_from + wrap_deg(u.turn_to - u.turn_from) * t);
                pose[i].done = t >= 1.0f;
            }
        }
    });
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (is_fighter(u)) continue;  // FIGHTER_FLYING: tick_fighters
        if (u.following && pose[i].done) {
            u.following = false;
            u.m.speed = 0.0f;
            if (u.facing != kNoFacing && std::fabs(wrap_deg(u.facing - u.m.yaw)) > 0.01f && u.lp.turn_rate > 0.0f) {
                u.turning = true;
                u.turn_from = u.m.yaw;
                u.turn_to = wrap_deg(u.facing);
                u.turn_start = frame + 1;
                const float frames = std::fabs(wrap_deg(u.turn_to - u.turn_from)) / u.lp.turn_rate *
                                     slowdown_for_layer(k, u.d.space_layer);
                u.turn_frames = static_cast<uint32_t>(std::ceil(frames));
            } else {
                u.has_goal = false;
                u.arrived = true;
            }
            park_space(u, frame);
        } else if (u.turning && pose[i].done) {
            u.turning = false;
            u.has_goal = false;
            u.arrived = true;
            park_space(u, frame);
        }
    }
    tick_fighters(frame);  // FIGHTER_FLYING §2-§5 (serial, ascending id)
}

void MovementSystem::commit(objects::ObjectStore& store) const {
    for (const Unit& u : units_) {
        const objects::ObjectHandle h = store.find(u.d.id);
        if (!store.alive(h)) continue;
        store.set_position(h, alamo::Vec3{u.m.pos.x, u.m.pos.y, u.z});
        // The store facing is a unit direction vector (map spawn, Lua, combat, anim and render all read it so);
        // ground units and ships keep a level heading (the yaw, no pitch).
        const V2 d = dir_from_deg(u.m.yaw);
        store.set_facing(h, alamo::Vec3{d.x, d.y, 0.0f});
    }
}

// ---------------------------------------------------------------- CRC
void MovementSystem::hash_units(StateHasher& h) const {
    h.u32(quota_used_);
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.i32(u.d.id);
        h.f32(u.m.pos.x);
        h.f32(u.m.pos.y);
        h.f32(u.z);
        h.f32(u.m.yaw);
        h.f32(u.m.speed);
        h.u8(static_cast<uint8_t>(u.m.state));
        h.u32(u.m.next);
        h.u32(static_cast<uint32_t>(u.m.path.size()));
        for (const V2& p : u.m.path) {
            h.f32(p.x);
            h.f32(p.y);
        }
        h.boolean(u.has_goal);
        h.boolean(u.need_path);
        h.boolean(u.arrived);
        h.f32(u.goal.x);
        h.f32(u.goal.y);
        h.u32(u.start_frame);
        h.u32(u.pend);            // cu8j t01
        h.boolean(u.adopted);     // cu8j t02
        h.u32(u.blocked_frames);
        h.u32(u.replans);
        h.u32(u.waits);
        h.i32(u.check_countdown);
        h.f32(u.speed_cap);
        h.u32(u.hold_until);
        h.u32(u.nudge_frames);
        h.boolean(u.following);
        h.u32(u.path_start);
        h.u32(static_cast<uint32_t>(u.wps.size()));
        for (const SpaceWaypoint& w : u.wps) {
            h.f32(w.pos.x);
            h.f32(w.pos.y);
            h.f32(w.heading);
            h.f32(w.time);
        }
        h.boolean(u.turning);
        if (u.aturn != LocoState::None) {  // s78l t02: the arrival turn state, hashed only while a turn runs
            h.u8(static_cast<uint8_t>(u.aturn));
            h.f32(u.aturn_rate);
            h.i32(u.tclip);
            h.u32(u.tclip_start);
            h.f32(u.tclip_speed);
        }
        h.f32(u.jiggle.x);  // OI tactical
        h.f32(u.jiggle.y);
        h.boolean(u.in_formation);
        if (u.pushed) {  // a1k t03: hashed only while a pushed move is active
            h.u32(u.push_next);
            h.u32(u.push_blocked);
            h.u32(static_cast<uint32_t>(u.push_path.size()));
            for (const V2& p : u.push_path) {
                h.f32(p.x);
                h.f32(p.y);
            }
        }
    }
    h.u32(static_cast<uint32_t>(crush_.size()));  // OI tactical
    for (const CrushEvent& e : crush_) {
        h.i32(e.crusher);
        h.i32(e.victim);
    }
    if (cfg_.mode == MoveMode::Land) coll_.hash(h);  // S7 collision layer (land only: the space format is unchanged)
    hash_special(h);  // §4.2-4.7: appended only when used (the format is unchanged otherwise)
    hash_fighters(h);  // FIGHTER_FLYING: appended only with space fighters / squadrons
    hash_deploy(h);    // S8 t11: appended only while a Walk is in a deploy state / has a deploy clip
    hash_turrets(h);   // S8 t24: appended only when a Deploys unit has a TURRET
}

void MovementSystem::register_crc(CrcRegistry& registry, const std::string& name) const {
    registry.add(name, [this](StateHasher& h) { hash_units(h); });
    registry.add(name + ".grid", [this](StateHasher& h) { grid_.hash(h); });
    registry.add(name + ".space", [this](StateHasher& h) {
        space_.hash(h);
        if (fgrids_.built) fgrids_.hash(h);  // FIGHTER_FLYING §5: cell membership (only once the grids exist)
    });
}

// ---------------------------------------------------------------- S10-int: speed factor
bool MovementSystem::set_speed_factor(ObjectId id, float factor) {
    Unit* u = unit_ptr(id);
    if (!u) return false;
    if (!(factor >= 0.0f)) factor = 0.0f;  // NaN / negative -> stopped
    if (u->speed_factor == factor) return true;
    u->speed_factor = factor;
    configure(*u);
    return true;
}

// ---------------------------------------------------------------- s78k t01: land arrival turn
// MOVEMENT §3.5.1 (0x61d890 -> 0x61ca90): the wanted orientation on the last waypoint = its facing (the move's final
// facing), through Movement_Directions_Count: 1 -> that yaw; <= 0 -> the current yaw; N -> of the N yaws spaced 360 / N
// from the wanted one, the closest to the current yaw (first on ties).
float MovementSystem::land_wanted_yaw(const Unit& u) const {
    const int32_t n = u.d.movement_directions;
    if (n <= 0) return u.m.yaw;
    const float want = wrap_deg(u.final_facing);
    if (n == 1) return want;
    float best = want, best_d = 1.0e30f;
    for (int32_t k = 0; k < n; ++k) {
        const float y = wrap_deg(want + 360.0f * static_cast<float>(k) / static_cast<float>(n));
        const float dlt = std::fabs(wrap_deg(y - u.m.yaw));
        if (dlt < best_d) {
            best_d = dlt;
            best = y;
        }
    }
    return best;
}

// State 0x15 on the last waypoint: a move with a final facing turns when the yaw is more than the threshold off (state
// 0xf / 0x10), else it keeps its arrival yaw. [port] Walk locomotor (LocomotorKind::Default) only. Every port land path
// is an ordinary one (no TerrainCollision kind-3 path, every waypoint cap > 0), so the threshold is 10 degrees
// (arrival_turn_threshold(R, false)). A move without a final facing ends on the arrival heading (0x61d570's 1e18 triple:
// no turn). The turn itself starts on the next tick (arrival_turn_frame).
void MovementSystem::land_arrival_turn(Unit& u) const {
    arrival_reset(u);
    if (u.final_facing == kNoFacing || u.d.locomotor != LocomotorKind::Default || is_air(u)) return;
    const float want = land_wanted_yaw(u);
    if (std::fabs(wrap_deg(want - u.m.yaw)) <= arrival_turn_threshold(u.lp.turn_rate, false)) return;
    u.turning = true;
    u.turn_to = want;
    u.aturn = LocoState::WalkStartTurn;
}

void MovementSystem::arrival_reset(Unit& u) {
    if (u.aturn != LocoState::None) u.aturn_end_pending = true;  // a running turn dropped: reported next tick
    u.turning = false;
    u.aturn = LocoState::None;
    u.aturn_rate = 0.0f;
    u.tclip = kAnimNone;
    u.tplay_set = false;
}

float MovementSystem::stopped_rate(const Unit& u) const {
    return u.d.stopped_turn_rate > 0.0f ? u.d.stopped_turn_rate : u.lp.turn_rate;
}

float MovementSystem::turn_clip_speed(const Unit& u) const {
    const float s = stopped_rate(u), ras = u.d.rotation_animation_speed;
    return s > 0.0f && ras > 0.0f ? s / ras : 1.0f;
}

bool MovementSystem::turn_clip_data(const Unit& u, int32_t anim, DeployClip& c) const {
    // The clip data hook (set_deploy_clip_fn) for any anim id; no data = the model has no such clip.
    uint32_t n = 0;
    float fps = 0.0f;
    if (anim == kAnimNone || !deploy_clip_ || !deploy_clip_(u.d.id, anim, n, fps) || !(fps > 0.0f) || n == 0) return false;
    c.count = static_cast<float>(n);
    c.step = fps / 30.0f;
    return true;
}

bool MovementSystem::turn_clip_near_end(const Unit& u, uint32_t frame) const {
    // 0x39a180: the clip is within its last 2 frames (or finished). No clip / no data: done.
    DeployClip c;
    if (!turn_clip_data(u, u.tclip, c)) return true;
    const float pos = static_cast<float>(frame - u.tclip_start) * c.step * u.tclip_speed;
    return pos >= c.count - 2.0f;
}

bool MovementSystem::turn_clip_play(Unit& u, int32_t anim, float speed, float blend, uint32_t frame) const {
    DeployClip c;
    if (!turn_clip_data(u, anim, c)) return false;  // the model has no such clip: the play fails, the clip stays
    u.tclip = anim;
    u.tclip_start = frame;
    u.tclip_speed = speed;
    u.tplay = TurnClipEvent{u.d.id, anim, speed, blend, false};
    u.tplay_set = true;
    return true;
}

// s78l t02 (MOVEMENT §3.5.1 "Arrival turn, details" steps 3-6): one frame of the Walk arrival turn. Every handler
// either ends the frame or runs the next state at once (0x0F -> 0x10 -> 0x11 -> 0x12 -> 0x15 in one frame).
// [port] "the current clip" A is the clip this turn last played (kAnimNone before the first play: idle-class, not a
// turn clip); the band threshold T is 10 (ordinary path); UseSecondaryFacing is not modelled.
void MovementSystem::arrival_turn_frame(Unit& u, uint32_t frame) const {
    const bool pre = u.d.has_pre_turn_anim;
    const float s = stopped_rate(u);  // 0x374310 at speed 0
    DeployClip cd;
    for (int guard = 0; guard < 8; ++guard) {
        switch (u.aturn) {
            case LocoState::WalkStartTurn: {  // 0x0F: the begin clip holds the yaw
                u.m.state = LocoState::WalkStartTurn;
                const float d = wrap_deg(u.turn_to - u.m.yaw);
                const int32_t b = d > 0.0f && d < 180.0f ? kAnimTurnLeftBegin : kAnimTurnRightBegin;
                if (pre && turn_clip_data(u, b, cd)) {
                    if (u.tclip != b) {
                        turn_clip_play(u, b, turn_clip_speed(u), -1.0f, frame);
                        return;
                    }
                    if (!turn_clip_near_end(u, frame)) return;
                }
                u.aturn_rate = 0.0f;
                u.aturn = LocoState::WalkTurning;
                break;
            }
            case LocoState::WalkTurning: {  // 0x10
                u.m.state = LocoState::WalkTurning;
                const float r = u.aturn_rate > 0.0f ? u.aturn_rate : s;
                const float e0 = wrap_deg(u.turn_to - u.m.yaw);
                const float ae0 = std::fabs(e0);
                u.m.yaw = turn_toward(u.m.yaw, u.turn_to, r);  // 0x61e730: lands exactly when |e| <= r
                const bool reached = ae0 > r ? false : r - ae0 > 0.0f;
                const int32_t a = u.tclip;
                if (anim_turn_clip(a) && !turn_clip_near_end(u, frame)) return;  // keep the band until the clip ends
                if (reached) {
                    u.aturn = LocoState::WalkEndTurn;
                    break;
                }
                if (!pre) {  // TURNL / TURNR once, the rate stays S
                    if (!anim_turn_clip(a) && turn_clip_data(u, kAnimTurnLeft, cd))
                        turn_clip_play(u, e0 >= 0.0f ? kAnimTurnLeft : kAnimTurnRight, 1.0f,
                                       std::min(r > 0.0f ? ae0 / r * 0.03333f : 0.0f, kArrivalTurnBlendCapPort), frame);
                    return;
                }
                // 0x6204f0: the band from the error after this frame's step; its rate applies from the next frame.
                const TurnAnim band = arrival_turn_band(wrap_deg(u.turn_to - u.m.yaw), s, 10.0f, a);
                u.aturn_rate = band.rate;
                if (band.anim != kAnimNone && !anim_idle_class(band.anim)) {
                    turn_clip_play(u, band.anim, turn_clip_speed(u), -1.0f, frame);
                    return;
                }
                u.aturn = LocoState::WalkEndTurn;  // |e| < T with an idle clip: the turn ends early
                break;
            }
            case LocoState::WalkEndTurn: {  // 0x11: the end clip, then 0x12 -> 0x15
                u.m.state = LocoState::WalkEndTurn;
                const int32_t ec = pre ? arrival_turn_end_clip(u.tclip) : kAnimNone;
                if (ec != kAnimNone && turn_clip_data(u, ec, cd)) {
                    if (u.tclip != ec) {
                        turn_clip_play(u, ec, turn_clip_speed(u), -1.0f, frame);
                        return;
                    }
                    if (!turn_clip_near_end(u, frame)) return;
                }
                if (std::fabs(wrap_deg(u.turn_to - u.m.yaw)) > arrival_turn_threshold(u.lp.turn_rate, false)) {
                    u.aturn = LocoState::WalkStartTurn;  // [H] not reached on the paths the port builds
                    break;
                }
                // Pop (0x557b10); a non-idle clip -> the default idle (the host tests burning / +0x2b3).
                if (u.tclip != kAnimNone && !anim_idle_class(u.tclip)) {
                    u.tplay = TurnClipEvent{u.d.id, kAnimNone, 1.0f, -1.0f, true};
                    u.tplay_set = true;
                }
                u.aturn = LocoState::None;
                u.aturn_rate = 0.0f;
                u.turning = false;
                u.aturn_ended = true;
                u.m.state = LocoState::WalkStopped;
                return;
            }
            default:
                arrival_reset(u);
                return;
        }
    }
}

std::vector<MovementSystem::TurnClipEvent> MovementSystem::take_turn_clip_events() {
    std::vector<TurnClipEvent> out;
    out.swap(tevents_);
    return out;
}

// s78l t02 c (MOVEMENT §6.1 "Forced-path destination check and fallback" step 2; A1_READINGS §40.2): the free-position
// search 0x440f90 in mode 1 (full circle) around `d` with +0x5e set: rings R = 0, 1, 2.8, 5.4, ... (step 1, +0.8 per ring
// up to min(1.5 r, 10); r = the hard footprint radius, 5 when 0), at most 20 rings, n = trunc(R 2pi / r) + 1 samples (at
// least 4 after ring 0) at the unit's yaw + 2pi i / n counter-clockwise. The first sample on the map (the grid) whose cell
// passes the movement class is taken (class -1: no point test); no occupancy or path test, no draws.
bool MovementSystem::forced_spot(const Unit& u, V2 d, V2& out) const {
    if (cfg_.mode != MoveMode::Land) return false;
    constexpr float kPi = 3.1415927f;
    constexpr uint32_t kRings = 20;
    float r = hard_footprint(u.d);
    if (!(r > 0.0f)) r = 5.0f;  // MOVEMENT §6.1 step 2: footprint radius 5 when 0 (step cap 1.5 r, n = trunc(R 2pi / r) + 1)
    const float max_step = std::min(r * 1.5f, 10.0f);
    float yaw_deg = wrap_deg(u.m.yaw);
    if (yaw_deg < 0.0f) yaw_deg += 360.0f;
    const float yaw = yaw_deg * 0.017453292f;
    const float sy = fighter_sin(yaw), cy = fighter_cos(yaw);
    float ring_r = 0.0f, step = 1.0f;
    for (uint32_t ring = 0; ring < kRings; ++ring) {
        uint32_t n = 1;
        if (r > 0.0f) n = static_cast<uint32_t>(static_cast<int64_t>(ring_r * 2.0f * kPi / r)) + 1u;
        if (ring != 0) n = std::max(4u, n);
        for (uint32_t i = 0; i < n; ++i) {
            const float th = (static_cast<float>(i) / static_cast<float>(n)) * kPi * 2.0f;
            const float st = fighter_sin(th), ct = fighter_cos(th);
            const V2 p{d.x + (ct * cy - st * sy) * ring_r, d.y + (st * cy + ct * sy) * ring_r};
            const int32_t cx = world_to_cell(p.x), cyy = world_to_cell(p.y);
            if (!grid_.in_range(cx, cyy)) continue;
            // [M] the effective cell (dynamic stamps included) where the spec reads 0x50e180's cell value; the destination
            // check in plan_land (passable_world) does the same.
            if (u.d.movement_class >= 0 && !grid_.passable_cell(cx, cyy, u.lp.mask)) continue;
            out = p;
            return true;
        }
        ring_r += step;
        step = std::min(step + 0.8f, max_step);
    }
    return false;
}

// ---------------------------------------------------------------- s78k t03: team locomotor
bool MovementSystem::set_team(ObjectId id, ObjectId team, float container_factor, float container_max_speed) {
    Unit* u = unit_ptr(id);
    if (!u) return false;
    if (!(container_factor >= 0.0f)) container_factor = 0.0f;  // NaN / negative -> stopped
    u->team = team < 0 ? -1 : team;
    u->team_factor = u->team < 0 ? 1.0f : container_factor;
    u->team_max = u->team < 0 || !(container_max_speed >= 0.0f) ? -1.0f : container_max_speed;  // s78l t01
    return true;
}

float MovementSystem::team_base_speed(const Unit& u) const {
    // 0x4049e0: the members' raw Max_Speed (+0x37c), not their own speed overrides or factors; x the mode multiplier as
    // configure() applies it, so a team of one type with factor-1 members gets exactly their lp.max_speed. s78l t01: the
    // container's own Max_Speed (set_team) caps it: min(b, team_max x mult).
    const float mult = mode_mult();
    const float b = std::max(u.d.max_speed, 0.0f) * mult;
    return u.team_max >= 0.0f ? std::min(b, u.team_max * mult) : b;
}

void MovementSystem::team_caps() {
    // MOVEMENT §3.4 (0x370f00 / 0x376990, s78l t01 "Team cap, exact"): a team container's effective max speed =
    // min(container Max_Speed, min over its members of their raw Max_Speed (0x4049e0)) x the mode multiplier x the
    // container's own factors (here: set_team's container factor). Sorted (team, minimum) pairs built serially in units_
    // order (no hashed container); a min is order-independent anyway.
    std::vector<std::pair<ObjectId, float>> mins;
    for (const Unit& u : units_) {
        if (u.team < 0) continue;
        const float b = team_base_speed(u);  // the container's own Max_Speed included
        auto it = std::lower_bound(mins.begin(), mins.end(), u.team,
                                   [](const std::pair<ObjectId, float>& e, ObjectId t) { return e.first < t; });
        if (it == mins.end() || it->first != u.team) mins.insert(it, {u.team, b});
        else it->second = std::min(it->second, b);
    }
    for (Unit& u : units_) {
        u.team_cap = kInf;
        if (u.team < 0 || jet_alternates(u)) continue;  // 0x371060: the airborne jetpack chain has no team cap
        const auto it = std::lower_bound(mins.begin(), mins.end(), u.team,
                                         [](const std::pair<ObjectId, float>& e, ObjectId t) { return e.first < t; });
        if (it != mins.end() && it->first == u.team) u.team_cap = it->second * u.team_factor;
    }
}

float MovementSystem::speed_factor(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u ? u->speed_factor : 1.0f;
}

bool MovementSystem::set_speed_override(ObjectId id, float speed) {
    Unit* u = unit_ptr(id);
    if (!u) return false;
    if (!(speed >= 0.0f)) speed = -1.0f;  // negative / NaN -> cleared
    if (u->speed_override == speed) return true;
    u->speed_override = speed;
    configure(*u);
    return true;
}

float MovementSystem::speed_override(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u ? u->speed_override : -1.0f;
}

void MovementSystem::hash_speed(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(units_.size()));
    for (const Unit& u : units_) {
        h.i32(u.d.id);
        h.f32(u.speed_factor);
        h.f32(u.path_lag);
        if (u.speed_override >= 0.0f) {  // OI abilities: only while overridden
            h.u8(0x29);
            h.f32(u.speed_override);
        }
    }
}

void MovementSystem::register_speed_crc(CrcRegistry& registry, const std::string& name) const {
    registry.add(name, [this](StateHasher& h) { hash_speed(h); });
}

// ================================================================ space fighters (FIGHTER_FLYING.md §2-§5)
// Serial in ascending id (tick_fighters). Getters (§1.4): turn / roll / lift = the configured rates (mode multiplier
// applied), thrust = FighterParams::max_thrust (raw), minspeed = SpecialParams::min_speed, maxspeed = the effective
// max speed. s78f t02: avoid_speed() / formation_speed_or_max() read the fighter formation (fform_speed_read: F +0x21c
// under a guard top with an object, FIGHTER_FLYING §4.11 F), else minspeed / maxspeed; the squadron's "container path" is its
// leader's path; the land clamp §3.7 is not modelled (space only: it is the identity there); the modifier events
// (Out_Of_Combat_Defense_Adjustment, §4.1 step 1) belong to combat and are not raised here.
MovementSystem::Unit* MovementSystem::unit_ptr(ObjectId id) {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &units_[static_cast<size_t>(i)];
}

const MovementSystem::Unit* MovementSystem::unit_ptr(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &units_[static_cast<size_t>(i)];
}

// The units_ indices of an order's known units, in order.units order, each once.
std::vector<uint32_t> MovementSystem::order_indices(const MoveOrder& order) const {
    std::vector<uint32_t> idx;
    for (const ObjectId id : order.units) {
        const int32_t i = find(id);
        if (i < 0 || std::find(idx.begin(), idx.end(), static_cast<uint32_t>(i)) != idx.end()) continue;
        idx.push_back(static_cast<uint32_t>(i));
    }
    return idx;
}

FighterSquad* MovementSystem::squad_ptr(ObjectId id) {
    auto it = std::lower_bound(squads_.begin(), squads_.end(), id, [](const FighterSquad& s, ObjectId v) { return s.id < v; });
    return it != squads_.end() && it->id == id ? &*it : nullptr;
}

FighterSquad* MovementSystem::fighter_squad(ObjectId id) { return squad_ptr(id); }

MovementSystem::Unit& MovementSystem::fleader(Unit& u) {
    FighterSquad* c = fsquad(u);
    if (c && !c->members.empty())
        if (Unit* l = unit_ptr(c->members.front())) return *l;
    return u;
}

FighterKin MovementSystem::fkin(const Unit& u) const {
    FighterKin k;
    k.turn = u.lp.turn_rate;
    k.roll = u.sp.roll_rate;
    k.lift = u.sp.max_lift;
    k.bank = u.sp.bank_turn_angle;
    k.thrust = u.fp.max_thrust;
    k.min_speed = u.sp.min_speed;
    k.max_speed = u.lp.max_speed;
    return k;
}

F3 MovementSystem::fslot(const FighterSquad* c, ObjectId id) {
    if (!c) return {};
    for (size_t n = 0; n < c->members.size(); ++n)
        if (c->members[n] == id) return n < c->slots.size() ? c->slots[n] : F3{};
    return {};
}

FighterTarget* MovementSystem::ftarget_slot(Unit& u) {
    FighterSquad* c = fsquad(u);
    return c ? &c->target : &u.fs.target;
}

// "The formation of (C's pack, else the own pack) has an attack target" (0x41a2e0): the target of set_fighter_target.
bool MovementSystem::ftarget_get(Unit& u, FighterTarget& out) {
    const FighterTarget* t = ftarget_slot(u);
    if (!t->set()) return false;
    out = *t;
    return true;
}

MovementSystem::FTgt MovementSystem::fresolve(const FighterTarget& t) {
    FTgt r;
    r.id = t.point ? -1 : t.id;
    r.p = t.pos;
    r.radius = t.radius > 0.0f ? t.radius : 0.0f;
    r.dying = t.dying;
    r.hidden = t.hidden;
    if (t.point) return r;
    if (FighterSquad* s = squad_ptr(t.id)) {  // a team container: its position is its first member's [port]
        r.team = true;
        const Unit* l = s->members.empty() ? nullptr : unit_ptr(s->members.front());
        if (!l) {
            r.dying = true;
            return r;
        }
        if (t.hard_point < 0) r.p = l->fs.b.p;
        return r;
    }
    if (const Unit* x = unit_ptr(t.id)) {
        if (t.hard_point < 0) r.p = is_fighter(*x) ? x->fs.b.p : F3{x->m.pos.x, x->m.pos.y, x->z};
        if (!(t.radius > 0.0f)) r.radius = x->d.radius;
    }
    return r;
}

FighterCell MovementSystem::fcell_of_target(const FTgt& t) {
    if (t.id < 0) return {};
    if (FighterSquad* s = squad_ptr(t.id)) return s->dog;
    if (Unit* x = unit_ptr(t.id); x && is_fighter(*x)) {
        if (FighterSquad* c = fsquad(*x)) return c->dog;
        return x->fs.dog;
    }
    return {};
}

bool MovementSystem::fpath_open(Unit& u) {
    const Unit& f = fsquad(u) ? fleader(u) : u;
    return !f.wps.empty() && static_cast<size_t>(f.fs.idx) + 1 < f.wps.size();
}

// Path-stack reset (0x554480): the stack is cleared [port: and a pending plan is dropped].
void MovementSystem::freset_path(Unit& u) {
    u.wps.clear();
    u.following = false;
    u.fs.idx = 0;
    // cu8j t01 (MOVEMENT §4.6): an order waiting for its composition has not reached the locomotor yet (retail: the
    // formation's Set_Move comes at the next coordinator update, after this reset), so its goal stays.
    if (u.start_frame == kStartPending) return;
    u.need_path = false;
    u.has_goal = false;
}

void MovementSystem::fleave(FighterCellGrid& g, Unit& u, bool dogfight) {
    if (FighterSquad* c = fsquad(u)) g.leave(c->id, dogfight ? c->dog : c->park);
    else g.leave(u.d.id, dogfight ? u.fs.dog : u.fs.park);
}

// [port] estimates of the team list's formation error E (+0x88) and spread S (+0x8c), whose writers are untraced:
// E = the largest 3-D distance of a member from its slot (L.P + (slot(m) - slot(L))), S = the per-axis largest
// |m.P - L.P|. Computed once per frame before the fighter pass, squadrons in ascending id, members in index order.
void MovementSystem::fighter_squad_estimates() {
    for (FighterSquad& s : squads_) {
        s.error = 1.0e18f;
        s.spread = F3{};
        const Unit* l = s.members.empty() ? nullptr : unit_ptr(s.members.front());
        if (!l) continue;
        const F3 lp = l->fs.b.p;
        const F3 sl = fslot(&s, l->d.id);
        float e = 0.0f;
        F3 sp{};
        for (ObjectId m : s.members) {
            const Unit* mu = unit_ptr(m);
            if (!mu) continue;
            const F3 sm = fslot(&s, m);
            const F3 at{lp.x + (sm.x - sl.x), lp.y + (sm.y - sl.y), lp.z + (sm.z - sl.z)};
            e = std::max(e, f3_length(f3_sub(mu->fs.b.p, at)));
            sp.x = std::max(sp.x, std::fabs(mu->fs.b.p.x - lp.x));
            sp.y = std::max(sp.y, std::fabs(mu->fs.b.p.y - lp.y));
            sp.z = std::max(sp.z, std::fabs(mu->fs.b.p.z - lp.z));
        }
        s.error = e;
        s.spread = sp;
    }
}

void MovementSystem::tick_fighters(uint32_t frame) {
    bool any = false;
    for (const Unit& u : units_) any = any || is_fighter(u);
    if (!any) return;
    if (!fgrids_.built && space_.has_box()) fgrids_.build(space_.box_min(), space_.box_max());  // §5 build (0x4faf40)
    fighter_squad_estimates();
    for (Unit& u : units_)
        if (is_fighter(u)) fighter_step(u, frame);
    for (Unit& u : units_)
        if (is_fighter(u)) park_space(u, frame);  // [port] the occupancy track = the current position
}

// §2 per-frame driver (vf6 0x5cb830); the hyperspace states (§4.8) are not run here.
void MovementSystem::fighter_step(Unit& u, uint32_t frame) {
    FighterState& f = u.fs;
    f.b.p = {u.m.pos.x, u.m.pos.y, u.z};
    f.b.o = fighter_fold_pitch360(f.b.o, fkin(u).lift);  // step 4
    const LocoState st = u.sm.state;
    if (FighterSquad* c = fsquad(u)) {  // step 5 (C list +0xbc / +0xbd flags [M] not modelled)
        if (st != LocoState::FighterIdle && st != LocoState::FighterIdleCombat) fgrids_.parking.leave(c->id, c->park);
        if (st != LocoState::FighterIdleCombat && st != LocoState::FighterDirectedCombat)
            fgrids_.dogfight.leave(c->id, c->dog);
    }
    FighterTarget t;
    switch (st) {
        case LocoState::FighterMoving:
            if (ftarget_get(u, t)) {
                u.sm.state = LocoState::FighterDirectedCombat;
                f_combat(u, frame);
            } else {
                f_moving(u);
            }
            break;
        case LocoState::FighterIdle: f_idle(u); break;
        case LocoState::FighterIdleCombat: break;
        case LocoState::FighterDirectedCombat: f_combat(u, frame); break;
        case LocoState::FighterDeadStop: f_dead_stop(u); break;
        default:  // 0 (and any foreign state [port])
            if (ftarget_get(u, t)) {
                u.sm.state = LocoState::FighterDirectedCombat;
                f_combat(u, frame);
            } else {
                u.sm.state = LocoState::FighterMoving;
                f_moving(u);
            }
            break;
    }
    u.m.pos = {f.b.p.x, f.b.p.y};
    u.z = f.b.p.z;
    u.m.yaw = fighter_wrap180(f.b.o.yaw);
    u.m.speed = f3_length(f.b.v);
}

void MovementSystem::f_steer(Unit& u, F3 aim, float v, float& tb, float& thb, float& lb) {
    fighter_steer_to(u.fs.b, fkin(u), aim, v, tb, thb, lb, u.sm.state == LocoState::FighterIdle);
}

// §3.5 avoid (0x5c8b70). Without the host hook (headless: no tactical tree) it returns false at once.
bool MovementSystem::f_avoid(Unit& u, F3& aim, float& tb, float& thb, float& lb) {
    if (!favoid_) return false;
    FighterState& f = u.fs;
    const FighterKin k = fkin(u);
    float tl = 2.0f / (k.turn * kFighterDegToRad);
    if (fsquad(u) && &fleader(u) == &u) tl *= 2.0f;
    const float s = f3_length(f.b.v);
    const F3 p = f.b.p;
    const F3 lo{p.x - s, p.y - s, p.z - s}, hi{p.x + s, p.y + s, p.z + s};
    const F3 centre{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
    const F3 half{std::fabs(hi.x - lo.x) * 0.5f, std::fabs(hi.y - lo.y) * 0.5f, std::fabs(hi.z - lo.z) * 0.5f};
    const F3 e{tl * f.b.v.x + p.x, tl * f.b.v.y + p.y, tl * f.b.v.z + p.z};
    F3 hit;
    if (favoid_(u.d.id, centre, half, p, e, hit)) {
        F3 away = f3_sub(p, hit);
        if (away == F3{}) away = {1.0f, 0.0f, 0.0f};
        f_steer(u, f3_add(away, p), favoid_speed(u), tb, thb, lb);  // avoid_speed()
        f.hold = kFighterAvoidHold;
        return true;
    }
    if (f.hold > 0) {  // keep flying straight for 15 frames after an evasion
        --f.hold;
        aim = {f.b.v.x * 10.0f + p.x, f.b.v.y * 10.0f + p.y, f.b.v.z * 10.0f + p.z};
    }
    return false;
}

float MovementSystem::f_station_speed(Unit& u, float v, float lo, float hi) {
    FighterSquad* c = fsquad(u);
    if (!c) return v;
    const Unit& l = fleader(u);
    StationInput in;
    in.v = v;
    in.lo = lo;
    in.hi = hi;
    in.p = u.fs.b.p;
    in.leader_p = l.fs.b.p;
    in.leader_v = l.fs.b.v;
    in.slot_leader = fslot(c, l.d.id);
    in.slot_self = fslot(c, u.d.id);
    in.speed = f3_length(u.fs.b.v);
    in.thrust = u.fp.max_thrust;
    in.tolerance = c->tolerance;
    in.error = c->error;
    return fighter_station_speed(in);
}

// §3.6 follow (0x5c9e90).
void MovementSystem::f_follow(Unit& u, float v_req, float z_goal) {
    FighterState& f = u.fs;
    const FighterKin k = fkin(u);
    FighterSquad* c = fsquad(u);
    const Unit& l = fleader(u);
    const float v = f_station_speed(u, v_req, 0.5f * v_req, 1.5f * v_req);
    float tb = k.turn, thb = k.thrust, lb = k.lift;
    if (k.max_speed < v) {
        const float s = v / k.max_speed;
        tb *= s;
        thb *= s;
        lb *= s;
    }
    F3 dir = l.fs.b.v;
    if (dir == F3{}) dir = {fighter_cos(l.fs.b.o.yaw * kFighterDegToRad), fighter_sin(l.fs.b.o.yaw * kFighterDegToRad), 0.0f};
    F3 off{};
    if (c) off = f3_sub(fslot(c, u.d.id), fslot(c, l.d.id));
    const float la = std::max(v, f3_length(f.b.v));
    std::span<const SpaceWaypoint> path;
    if (&l == &u) path = u.wps;  // the leader (or a solo fighter): the container's path = its own [port]
    const F3 a = fighter_lookahead(path, f.idx, l.fs.b.p, dir, c == nullptr, la, k.turn);
    z_goal = fighter_glide_z(z_goal, a, f.b.p, f.b.o.pitch, lb, la);
    F3 aim{a.x + off.x, a.y + off.y, off.z + z_goal};
    if (!f_avoid(u, aim, tb, thb, lb)) f_steer(u, aim, v, tb, thb, lb);
}

// §4.7 enter_idle (0x5cb750).
void MovementSystem::f_enter_idle(Unit& u) {
    FighterSquad* c = fsquad(u);
    if (!c) {
        u.sm.state = LocoState::FighterIdle;
        freset_path(u);
        return;
    }
    fgrids_.parking.leave(c->id, c->park);
    c->target = FighterTarget{};  // the team's two target slots (0x4061f0 / 0x4061d0) [M]
    u.sm.state = LocoState::FighterIdle;
    freset_path(u);
    // Register a parking cell (0x4fafd0): D (own and the container's) = the cell centre. ref = the own D [M].
    const FighterAlliedFn allied = [this](int32_t a, int32_t b) { return fallied(a, b); };
    const FighterCellGrid::Lookup r = fgrids_.parking.lookup(c->id, u.d.owner, u.fs.dest, u.d.layer_z, c->park, true, allied);
    if (r.set_dest) {
        const F3 ctr = fgrids_.parking.centre(r.cell, u.d.layer_z);
        u.fs.dest = ctr;
        c->dest = ctr;
    }
}

// §4.1 Moving 0x1B (0x5ce010).
void MovementSystem::f_moving(Unit& u) {
    FighterState& f = u.fs;
    f.phase = FighterPhase::None;
    if (u.wps.empty()) {  // no path: coast
        f_commit(u);
        return;
    }
    FighterSquad* c = fsquad(u);
    Unit& l = fleader(u);
    const bool passed = fighter_waypoint_passed(u.wps, f.idx, f.b.p);
    const auto complete = [](const Unit& x) { return static_cast<size_t>(x.fs.idx) + 1 >= x.wps.size(); };
    if (c && &l == &u) {
        if (passed)  // 0x5c4920 on every member with a current path, team index order
            for (ObjectId m : c->members)
                if (Unit* mu = unit_ptr(m); mu && !mu->wps.empty()) ++mu->fs.idx;
        if (complete(u) && u.sm.state != LocoState::FighterDirectedCombat) {
            const V2 w = u.wps.back().pos;
            const float lz = u.d.layer_z;
            const FAttitude so = u.fs.stored;
            const std::vector<ObjectId> members = c->members;
            for (ObjectId m : members) {
                Unit* mu = unit_ptr(m);
                if (!mu) continue;
                mu->fs.dest = {w.x, w.y, lz};
                mu->fs.stored = so;
                mu->arrived = true;
                f_enter_idle(*mu);  // [port] every member (the retail call sits under "m has a path" [M])
                fevents_.push_back({FighterEvent::EnterIdle, m, -1});  // S7 t25 (§4.10 D)
            }
            f_commit(u);
            return;
        }
    } else if (!c) {
        if (passed) ++f.idx;
        if (complete(u)) {
            const V2 w = u.wps.back().pos;
            f.dest = {w.x, w.y, u.d.layer_z};
            u.arrived = true;
            f_enter_idle(u);
            fevents_.push_back({FighterEvent::EnterIdle, u.d.id, -1});  // S7 t25 (§4.10 D)
            f_commit(u);
            return;
        }
    }
    // formation_speed_or_max() (s78f t02, FIGHTER_FLYING §4.11 F: F +0x21c of the container's / own formation while its
    // top is a guard with an object and it has a path, so a solo fighter is capped too); path kind 1 is not produced
    f_follow(u, fformation_speed(u), u.d.layer_z);
    f_commit(u);
}

// §4.2 Idle 0x1C (0x5cd8e0).
void MovementSystem::f_idle(Unit& u) {
    FighterState& f = u.fs;
    f.phase = FighterPhase::None;
    const FighterKin k = fkin(u);
    float tb = k.turn, thb = k.thrust, lb = k.lift;
    FighterSquad* c = fsquad(u);
    if (!c) {
        float v = kSpaceIdleMovementSpeed;
        if (space_.has_box() && !space_.inside_box({f.b.p.x, f.b.p.y})) v = 0.0f;  // 0x59e5b0 [port: 2-D box]
        fighter_yaw_bank_step(f.b.o, k, 0.0f, tb);                                  // unwinds the bank
        fighter_pitch_step(f.b.o, -fighter_wrap180(f.b.o.pitch), lb);               // levels
        fighter_speed_step(f.b, v, thb);
    } else {
        const Unit& l = fleader(u);
        const bool leader = &l == &u;
        F3 d = l.fs.dest;  // a local copy of L's D
        const float v = fighter_squad_idle(f.b.p, l.fs.b.p, d, leader, favoid_speed(u));  // avoid_speed()
        F3 aim = d;
        if (!leader) {
            const F3 sl = fslot(c, l.d.id), ss = fslot(c, u.d.id);
            aim = {(l.fs.b.v.x * 40.0f + l.fs.b.p.x) + (ss.x - sl.x) * 0.75f,
                   (l.fs.b.v.y * 40.0f + l.fs.b.p.y) + (ss.y - sl.y) * 0.75f,
                   (l.fs.b.v.z * 40.0f + l.fs.b.p.z) + (ss.z - sl.z) * 0.75f};
        }
        if (!f_avoid(u, aim, tb, thb, lb)) f_steer(u, aim, v, tb, thb, lb);
    }
    f_commit(u);
    FighterTarget t;
    if (ftarget_get(u, t) && !fresolve(t).dying) u.sm.state = LocoState::FighterDirectedCombat;  // runs next frame
}

// §4.3 Directed_Combat 0x1E (0x5cc220).
void MovementSystem::f_combat(Unit& u, uint32_t frame) {
    FighterState& f = u.fs;
    FighterSquad* c = fsquad(u);
    Unit& l = fleader(u);
    const bool leader = &l == &u;
    const auto no_target = [&] {
        f.phase = FighterPhase::None;
        if (fpath_open(u)) {
            f_moving(u);
            return;
        }
        if (!c) {
            f.dest = f3_add(f.b.p, f.b.v);
            f.stored = f.b.o;
            f_enter_idle(u);
            fevents_.push_back({FighterEvent::EnterIdle, u.d.id, -1});  // S7 t25 (§4.10 D)
        } else {
            fgrids_.dogfight.leave(c->id, c->dog);
            if (leader) {
                c->target = FighterTarget{};  // 0x4061d0 [M]
                const F3 lp = l.fs.b.p, lv = l.fs.b.v;
                const FAttitude lo = l.fs.b.o;  // "m's stored orientation = L's" [M: L's current orientation]
                const std::vector<ObjectId> members = c->members;
                for (ObjectId m : members) {
                    Unit* mu = unit_ptr(m);
                    if (!mu) continue;
                    mu->fs.dest = {lp.x + lv.x, lp.y + lv.y, u.d.layer_z};
                    mu->fs.stored = lo;
                    f_enter_idle(*mu);
                    fevents_.push_back({FighterEvent::EnterIdle, m, -1});  // S7 t25 (§4.10 D)
                }
            }
        }
        f_commit(u);
    };
    // 2. target resolution
    FighterTarget ft;
    bool have = ftarget_get(u, ft);
    FTgt t;
    if (have) t = fresolve(ft);
    if (!have || t.dying) {  // retarget from C's dogfight cell, join order
        have = false;
        if (c && c->dog.valid()) {
            const auto es = fgrids_.dogfight.entries(c->dog);
            const std::vector<FighterCellGrid::Entry> list(es.begin(), es.end());
            for (const FighterCellGrid::Entry& e : list) {
                if (!hostile(u.d.owner, e.owner)) continue;  // 0x39a450
                FighterTarget nt;
                nt.id = e.id;
                const FTgt cand = fresolve(nt);
                if (cand.dying) continue;
                c->target = nt;  // order C to attack o (0x38d730); tgt = C's current attack target
                fevents_.push_back({FighterEvent::Retarget, c->id, e.id});
                t = cand;
                have = true;
                break;
            }
        }
        if (!have) return no_target();
    }
    if (t.dying || t.hidden) return no_target();
    // 3. approach test
    const FighterCell fcell = c ? c->dog : f.dog;
    const FighterCell tcell = fcell_of_target(t);
    if (!(fcell.i != -1 && fcell == tcell)) {
        const F3 x = l.fs.b.p;
        const float rs = u.sp.strafe_distance + t.radius;
        const float dx = t.p.x - x.x, dy = t.p.y - x.y;
        if (rs * rs < dx * dx + dy * dy) {
            f.phase = FighterPhase::Approach;
            fleave(fgrids_.dogfight, u, true);
            if (fpath_open(u)) {
                f_moving(u);
                f.phase = FighterPhase::Approach;
                return;
            }
            const FighterKin k = fkin(u);
            float tb = k.turn, thb = k.thrust, lb = k.lift;
            F3 a = t.p;
            if (!f_avoid(u, a, tb, thb, lb)) f_steer(u, a, fformation_speed(u), tb, thb, lb);  // formation_speed_or_max()
            f_commit(u);
            return;
        }
    }
    // 4. attack
    if (c) {
        if (leader)
            for (ObjectId m : c->members)
                if (Unit* mu = unit_ptr(m)) freset_path(*mu);
    } else {
        freset_path(u);
    }
    if (!t.team) {
        fleave(fgrids_.dogfight, u, true);
        f_attack_run(u, t);
    } else {
        f_dogfight(u, t, frame);
    }
    f_commit(u);
}

// §4.4 attack run vs a ship, structure or hard point (0x5cedf0).
void MovementSystem::f_attack_run(Unit& u, const FTgt& t) {
    FighterState& f = u.fs;
    Unit& l = fleader(u);
    const F3 tp = t.p;
    if (&l != &u) {  // members fly the formation slot at the target's height
        f.phase = l.fs.phase;
        f_follow(u, u.lp.max_speed, tp.z);
        return;
    }
    const FighterKin k = fkin(u);
    const float vmax = k.max_speed, vmin = k.min_speed;
    const float v = f_station_speed(u, vmax, vmin, vmax * 1.5f);
    float tb = k.turn, thb = k.thrust, lb = k.lift;
    if (vmax < v) {
        const float s = v / vmax;
        tb *= s;
        thb *= s;
        lb *= s;
    }
    const float rs = u.sp.strafe_distance + t.radius;
    const float r2 = rs * rs;
    const F3 x = l.fs.b.p;
    const float dx = tp.x - x.x, dy = tp.y - x.y;
    F3 a = tp;
    if (dx * dx + dy * dy <= r2) {  // inside the strafe circle (2-D)
        RelAngles rel;
        if (fighter_cone(tp, x, l.fs.b.o, 45.0f, 45.0f, r2, rel)) {  // firing pass
            f.phase = FighterPhase::Pass;
            if (f_avoid(u, a, tb, thb, lb)) return;
            f_steer(u, a, v, tb, thb, lb);
        } else {  // break-off: straight on (the path stack is cleared: one turn radius along V)
            f.phase = FighterPhase::BreakOff;
            f_follow(u, vmax, tp.z);
        }
    } else {  // approach / re-attack
        f.phase = FighterPhase::Approach;
        if (f_avoid(u, a, tb, thb, lb)) return;
        f_steer(u, a, v, tb, thb, lb);
    }
}

// §4.5 chase(x) with the cone angles: no loop rule, no avoidance.
void MovementSystem::f_chase(Unit& u, const Unit& x, RelAngles rel, float& tb, float& thb, float& lb) {
    if (thb == 0.0f || tb == 0.0f) return;
    const FighterKin k = fkin(u);
    fighter_yaw_bank_step(u.fs.b.o, k, rel.yaw, tb);
    fighter_pitch_step(u.fs.b.o, rel.pitch, lb);
    const F3 d = f3_sub(x.fs.b.p, u.fs.b.p);
    const float d2 = (d.x * d.x + d.y * d.y) + d.z * d.z;
    const float mfd = u.fp.min_follow_distance;
    const float v = mfd * mfd <= d2 ? f3_length(x.fs.b.v) : favoid_speed(u);  // match its speed, else avoid_speed()
    fighter_speed_step(u.fs.b, v, thb);
}

// §4.5 dogfight vs a squadron (0x5ce4f0).
void MovementSystem::f_dogfight(Unit& u, const FTgt& t, uint32_t frame) {
    FighterState& f = u.fs;
    FighterSquad* c = fsquad(u);
    Unit& l = fleader(u);
    const bool leader = &l == &u;
    const FighterKin k = fkin(u);
    float tb = k.turn, thb = k.thrust, lb = k.lift;
    FighterSquad* ts = squad_ptr(t.id);
    if (!ts) return;
    const FighterAlliedFn allied = [this](int32_t a, int32_t b) { return fallied(a, b); };
    FighterCellGrid& gd = fgrids_.dogfight;
    const ObjectId entry = c ? c->id : u.d.id;
    FighterCell& own = c ? c->dog : f.dog;
    F3 a = t.p;
    FighterCell cell = ts->dog;
    f.phase = FighterPhase::Dogfight;
    if (cell.i < 0 && cell.j < 0) {  // 1. the target is not in a cell
        const Unit* tl = ts->members.empty() ? nullptr : unit_ptr(ts->members.front());
        FighterCell none;
        cell = gd.lookup(ts->id, tl ? tl->d.owner : -1, t.p, tl ? tl->d.layer_z : 0.0f, none, false, allied).cell;
        if (!leader) {
            f_follow(u, k.max_speed, t.p.z);
            return;
        }
        gd.leave(entry, own);
        own = cell;  // written without joining
    } else {         // 2. the target is in a cell
        a = gd.centre(cell, u.d.layer_z);
        const float dx = f.b.p.x - a.x, dy = f.b.p.y - a.y;
        if (80000.0f < dx * dx + dy * dy) {  // farther than ~282.8: travel to the cell
            gd.leave(entry, own);
            gd.join(entry, u.d.owner, cell, own, allied);
            if (!leader) {
                f_follow(u, k.max_speed, u.d.layer_z);
                return;
            }
        } else {  // at the fight
            gd.leave(entry, own);
            own = cell;
            // Provocation [M for the flags]: the target squadron is ordered to attack us (C, or L when solo). The
            // unit abilities 0x29 / 0x22 test is the host's (it receives the order as a Provoke event).
            const Unit* e = ts->members.empty() ? nullptr : unit_ptr(ts->members.front());
            const FighterTarget& tt = ts->target;
            const bool tt_team = tt.set() && !tt.point && squad_ptr(tt.id) != nullptr;
            if (e && e->sm.state != LocoState::FighterMoving && (!tt.set() || !tt_team)) {
                FighterTarget nt;
                nt.id = c ? c->id : l.d.id;
                ts->target = nt;
                fevents_.push_back({FighterEvent::Provoke, ts->id, nt.id});
            }
            if (ts->dog.i != -1) gd.join(entry, u.d.owner, ts->dog, own, allied);
            // Lock (pack +0x1e0, timer +0x1d8 / +0x1dc).
            const float range2 = u.fp.attack_range * u.fp.attack_range;
            Unit* o = f.lock >= 0 ? unit_ptr(f.lock) : nullptr;
            const bool expired = f.lock_start + f.lock_frames <= frame;
            if (o && !expired) {
                RelAngles rel;
                if (fighter_cone(o->fs.b.p, f.b.p, f.b.o, 90.0f, 90.0f, range2, rel)) {
                    f.phase = FighterPhase::Chase;
                    f_chase(u, *o, rel, tb, thb, lb);
                    return;
                }
                F3 aa = o->fs.b.p;
                if (f_avoid(u, aa, tb, thb, lb)) return;
                f_steer(u, aa, fformation_speed(u), tb, thb, lb);  // formation_speed_or_max()
                return;
            }
            if (expired) {  // scan the cell in join order (0x39a4e0 hostility)
                const auto es = gd.entries(cell);
                const std::vector<FighterCellGrid::Entry> list(es.begin(), es.end());
                for (const FighterCellGrid::Entry& en : list) {
                    if (!hostile(u.d.owner, en.owner)) continue;
                    std::vector<ObjectId> cands;
                    if (const FighterSquad* es2 = squad_ptr(en.id)) cands = es2->members;
                    else cands.push_back(en.id);
                    for (ObjectId cid : cands) {
                        Unit* x = unit_ptr(cid);
                        if (!x || x == &u || !is_fighter(*x)) continue;
                        if (x->fs.lock_start + x->fs.lock_frames > frame) continue;  // its own lock timer is running
                        RelAngles rel;
                        if (!fighter_cone(x->fs.b.p, f.b.p, f.b.o, 90.0f, 90.0f, range2, rel)) continue;
                        f.lock = x->d.id;  // lock(x): 300 frames; the victim cannot lock back meanwhile
                        f.lock_start = frame;
                        f.lock_frames = kFighterLockFrames;
                        x->fs.lock = -1;
                        x->fs.lock_start = frame;
                        x->fs.lock_frames = kFighterLockFrames;
                        f.phase = FighterPhase::Chase;
                        f_chase(u, *x, rel, tb, thb, lb);
                        return;
                    }
                }
            }
        }
    }
    // 3. tail (nothing chased)
    if (!leader) f_follow(u, k.max_speed, u.d.layer_z);
    else f_steer(u, a, k.max_speed, tb, thb, lb);
}

// §4.6 Dead_Stop 0x1F (0x5cbea0).
void MovementSystem::f_dead_stop(Unit& u) {
    FighterState& f = u.fs;
    f.phase = FighterPhase::None;
    const FighterKin k = fkin(u);
    float tb = k.turn, thb = k.thrust, lb = k.lift;
    FighterSquad* c = fsquad(u);
    bool settled = c == nullptr;
    if (c) {  // Rf (0x404cd0) = the longest slot offset + the first member's radius [M]
        float longest = 0.0f;
        for (const F3& s : c->slots) longest = std::max(longest, f3_length(s));
        const Unit* m0 = c->members.empty() ? nullptr : unit_ptr(c->members.front());
        const float rf = longest + (m0 ? m0->d.radius : 0.0f);
        settled = std::fabs(c->error) <= c->tolerance && f3_length(c->spread) <= 1.33f * rf;
    }
    if (settled) {  // turn to the stored orientation and brake by Max_Thrust per frame
        fighter_yaw_bank_step(f.b.o, k, fighter_wrap180(f.stored.yaw - f.b.o.yaw), tb);
        fighter_pitch_step(f.b.o, fighter_wrap180(f.stored.pitch - f.b.o.pitch), lb);
        fighter_speed_step(f.b, 0.0f, thb);
    } else {  // regroup
        const float vmin = k.min_speed;
        if (&fleader(u) == &u) {
            const float sy = f.stored.yaw * kFighterDegToRad;
            F3 a{(2.0f * vmin) * fighter_cos(sy) + f.b.p.x, (2.0f * vmin) * fighter_sin(sy) + f.b.p.y,
                 (2.0f * vmin) * 0.0f + f.b.p.z};
            if (!f_avoid(u, a, tb, thb, lb)) f_steer(u, a, vmin, tb, thb, lb);
        } else {
            f_follow(u, vmin, u.d.layer_z);
        }
    }
    f_commit(u);
}

// [port] Stop order: the NoTarget idle entry of §4.3 step 5 (the retail Stop path is untraced): a fighter without a
// squadron idles at D = P + V with stored O = O; a squadron's members all idle at D = L.P + L.V with L's orientation
// (and register one parking cell).
void MovementSystem::fighter_stop(Unit& u) {
    FighterSquad* c = fsquad(u);
    if (!c) {
        u.fs.dest = f3_add(u.fs.b.p, u.fs.b.v);
        u.fs.stored = u.fs.b.o;
        f_enter_idle(u);
        return;
    }
    const Unit& l = fleader(u);
    const F3 lp = l.fs.b.p, lv = l.fs.b.v;
    const FAttitude lo = l.fs.b.o;
    const float lz = l.d.layer_z;
    const std::vector<ObjectId> members = c->members;
    for (ObjectId m : members) {
        Unit* mu = unit_ptr(m);
        if (!mu) continue;
        mu->fs.dest = {lp.x + lv.x, lp.y + lv.y, lz};
        mu->fs.stored = lo;
        f_enter_idle(*mu);
    }
}

// move_space: §2 step 6 state 0 -> 0x1E with a target, else 0x1B; the new path starts at waypoint 0.
// [port] S7 t12: always 0x1B here. The new order replaces the formation's block, but the host feeds that block's attack
// target (set_fighter_target, TacticalBattle::feed_fighter_target) only before the next fighter frame, so a target read
// now is the OLD order's. 0x1B with a target turns into 0x1E and runs §4.3 in the same frame (§2 step 6), so a target
// that is still valid behaves as before; a stale one (a squadron in an attack run or a dogfight given a Move_To) no longer
// sends §4.3 into NoTarget (enter_idle reset the new path: the order was lost) or a Retarget from its dogfight cell.
void MovementSystem::fighter_order(Unit& u) {
    u.sm.state = LocoState::FighterMoving;
    u.fs.idx = 0;
}

// S7 t28 / t35 (A1_READINGS §33.4.1 step 3, 0x40dbc0, FIGHTER_FLYING §4.10 E): the guard's path copy. The whole path is
// copied and every member's waypoint index is set to the object's, so the escort joins at the object's progress.
// [port] the object's index = the start of its current segment by playback time (a space ship's path has no pass index;
// FighterState::idx is the last passed waypoint, the segment start). S7 t37: t35 used the segment end, so a two-point
// path was complete at once (the escort went idle instead of flying it).
bool MovementSystem::fighter_copy_path(ObjectId squad, ObjectId from, uint32_t frame) {
    if (cfg_.mode != MoveMode::Space) return false;
    const Unit* s = unit_ptr(from);
    if (!s || is_fighter(*s) || !s->following || s->wps.empty()) return false;
    const float t = static_cast<float>(frame - s->path_start) + s->path_lag;  // from's playback time (tick_space)
    uint32_t at = 0;
    while (at < s->wps.size() && !(s->wps[at].time > t)) ++at;
    if (at >= s->wps.size()) return false;  // the object's path is complete: not moving
    const std::vector<SpaceWaypoint> path = s->wps;
    std::vector<ObjectId> ids;
    if (const FighterSquad* c = squad_ptr(squad)) {
        ids = c->members;
    } else if (const Unit* x = unit_ptr(squad); x && is_fighter(*x) && x->fs.squad < 0) {
        ids.push_back(squad);
    }
    bool any = false;
    for (ObjectId m : ids) {
        Unit* u = unit_ptr(m);
        if (!u || !is_fighter(*u)) continue;
        u->wps = path;
        u->fs.idx = at > 0 ? at - 1 : 0;
        u->goal = path.back().pos;
        u->has_goal = true;
        u->need_path = false;
        u->arrived = false;
        u->failed = false;
        u->fallback = false;
        u->following = true;
        u->path_start = frame;
        u->path_lag = 0.0f;
        u->facing = kNoFacing;
        const LocoState st = u->sm.state;
        if (st == LocoState::FighterIdle || st == LocoState::FighterMoving || st == LocoState::None)
            u->sm.state = LocoState::FighterMoving;
        any = true;
    }
    return any;
}

// A unit (any kind) or squadron leaves: fighters drop it from their squadron, the grids and their locks; targets that
// name it become dying ([port]: the retail death signals, 0x220e90 / 0x220eb0).
void MovementSystem::fighter_removed(ObjectId id) {
    fform_removed(id);  // S7 t37 / s78f t02: the formation's own end, or its objects' (listener 0x4f77e0)
    if (Unit* u = unit_ptr(id); u && is_fighter(*u)) {
        if (FighterSquad* c = fsquad(*u)) {
            for (size_t n = 0; n < c->members.size(); ++n)
                if (c->members[n] == id) {
                    c->members.erase(c->members.begin() + static_cast<std::ptrdiff_t>(n));
                    if (n < c->slots.size()) c->slots.erase(c->slots.begin() + static_cast<std::ptrdiff_t>(n));
                    break;
                }
            u->fs.squad = -1;
            if (c->members.empty()) {
                const ObjectId cid = c->id;
                remove_fighter_squad(cid);
            }
        } else {
            fgrids_.dogfight.leave(id, u->fs.dog);
            fgrids_.parking.leave(id, u->fs.park);
        }
    }
    for (Unit& o : units_) {
        if (o.d.locomotor != LocomotorKind::Fighter) continue;
        if (o.fs.lock == id) o.fs.lock = -1;
        if (!o.fs.target.point && o.fs.target.id == id) o.fs.target.dying = true;
    }
    for (FighterSquad& s : squads_)
        if (!s.target.point && s.target.id == id) s.target.dying = true;
}

bool MovementSystem::set_fighter_squad(const FighterSquadDesc& desc) {
    if (cfg_.mode != MoveMode::Space || desc.id < 0) return false;
    std::vector<ObjectId> mem;
    std::vector<F3> slots;
    for (size_t n = 0; n < desc.members.size(); ++n) {
        const ObjectId m = desc.members[n];
        const Unit* u = unit_ptr(m);
        if (!u || !is_fighter(*u) || std::find(mem.begin(), mem.end(), m) != mem.end()) continue;
        mem.push_back(m);
        slots.push_back(n < desc.slots.size() ? desc.slots[n] : F3{});
    }
    if (mem.empty()) return false;
    // Members of another squadron leave it (an emptied squadron is removed).
    for (ObjectId m : mem) {
        Unit* u = unit_ptr(m);
        if (u->fs.squad >= 0 && u->fs.squad != desc.id) {
            if (FighterSquad* o = squad_ptr(u->fs.squad)) {
                for (size_t n = 0; n < o->members.size(); ++n)
                    if (o->members[n] == m) {
                        o->members.erase(o->members.begin() + static_cast<std::ptrdiff_t>(n));
                        if (n < o->slots.size()) o->slots.erase(o->slots.begin() + static_cast<std::ptrdiff_t>(n));
                        break;
                    }
                u->fs.squad = -1;
                if (o->members.empty()) {
                    const ObjectId oid = o->id;
                    remove_fighter_squad(oid);
                }
            }
        } else if (u->fs.squad < 0) {  // a solo fighter's own cells
            fgrids_.dogfight.leave(m, u->fs.dog);
            fgrids_.parking.leave(m, u->fs.park);
        }
    }
    FighterSquad* s = squad_ptr(desc.id);
    if (!s) {
        auto it = std::lower_bound(squads_.begin(), squads_.end(), desc.id,
                                   [](const FighterSquad& q, ObjectId v) { return q.id < v; });
        FighterSquad ns;
        ns.id = desc.id;
        if (const Unit* l = unit_ptr(mem.front())) ns.dest = l->fs.dest;
        s = &*squads_.insert(it, std::move(ns));
    }
    for (ObjectId m : s->members)
        if (std::find(mem.begin(), mem.end(), m) == mem.end())
            if (Unit* u = unit_ptr(m)) u->fs.squad = -1;
    s->members = std::move(mem);
    s->slots = std::move(slots);
    s->tolerance = desc.tolerance;
    s->guard_follow = desc.guard_follow;  // S7 t37
    for (ObjectId m : s->members) unit_ptr(m)->fs.squad = desc.id;
    s->chase = desc.chase;              // s78f t02
    // S7 t37 / s78f t02: a formation given to the members before their squadron was registered (a hangar launch orders
    // the spawned fighters in the same frame) is the squadron's: the first member's (team index order) moves to the
    // container; the members' own formations go.
    if (!fforms_.empty()) {
        FighterFormation moved;
        bool have = false;
        for (ObjectId m : s->members)
            if (const FighterFormation* g = fform_ptr(m); g && !have) {
                moved = *g;
                have = true;
            }
        const std::vector<ObjectId> now_members = s->members;
        std::erase_if(fforms_, [&now_members](const FighterFormation& g) {
            return std::find(now_members.begin(), now_members.end(), g.key) != now_members.end();
        });
        if (have && !fform_ptr(desc.id)) {
            moved.key = desc.id;
            moved.follow = s->guard_follow;
            auto it = std::lower_bound(fforms_.begin(), fforms_.end(), desc.id,
                                       [](const FighterFormation& x, ObjectId v) { return x.key < v; });
            fforms_.insert(it, std::move(moved));
        }
    }
    return true;
}

bool MovementSystem::remove_fighter_squad(ObjectId id) {
    FighterSquad* s = squad_ptr(id);
    if (!s) return false;
    for (ObjectId m : s->members)
        if (Unit* u = unit_ptr(m)) u->fs.squad = -1;
    fgrids_.dogfight.leave(id, s->dog);
    fgrids_.parking.leave(id, s->park);
    squads_.erase(squads_.begin() + (s - squads_.data()));
    fighter_removed(id);  // targets naming the container become dying
    return true;
}

bool MovementSystem::set_fighter_target(ObjectId id, const FighterTarget& t) {
    if (FighterSquad* s = squad_ptr(id)) {
        s->target = t;
        return true;
    }
    Unit* u = unit_ptr(id);
    if (!u || u->d.locomotor != LocomotorKind::Fighter) return false;
    *ftarget_slot(*u) = t;
    return true;
}

bool MovementSystem::set_fighter_params(ObjectId id, const FighterParams& p) {
    Unit* u = unit_ptr(id);
    if (!u || u->d.locomotor != LocomotorKind::Fighter) return false;
    u->fp = p;
    return true;
}

bool MovementSystem::fighter_dead_stop(ObjectId id) {
    Unit* u = unit_ptr(id);
    if (!u || !is_fighter(*u)) return false;
    u->sm.state = LocoState::FighterDeadStop;
    u->fs.stored = u->fs.b.o;
    freset_path(*u);
    return true;
}

const FighterState* MovementSystem::fighter_state(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    if (!u) return nullptr;
    return u->d.locomotor == LocomotorKind::Fighter ? &u->fs : nullptr;
}

FighterState* MovementSystem::fighter_state(ObjectId id) {
    Unit* u = unit_ptr(id);
    return u && u->d.locomotor == LocomotorKind::Fighter ? &u->fs : nullptr;
}

namespace {
void hash_f3(StateHasher& h, F3 v) {
    h.f32(v.x);
    h.f32(v.y);
    h.f32(v.z);
}
void hash_att(StateHasher& h, FAttitude o) {
    h.f32(o.roll);
    h.f32(o.pitch);
    h.f32(o.yaw);
}
void hash_cell(StateHasher& h, FighterCell c) {
    h.i32(c.i);
    h.i32(c.j);
}
void hash_target(StateHasher& h, const FighterTarget& t) {
    h.i32(t.id);
    h.boolean(t.point);
    h.i32(t.hard_point);
    hash_f3(h, t.pos);
    h.f32(t.radius);
    h.boolean(t.dying);
    h.boolean(t.hidden);
}
}  // namespace

void MovementSystem::hash_fighters(StateHasher& h) const {
    uint32_t n = 0;
    for (const Unit& u : units_)
        if (is_fighter(u)) ++n;
    if (n == 0 && squads_.empty() && fevents_.empty()) return;  // unchanged format without fighters
    h.u32(0x46474854u);                                         // "FGHT"
    h.u32(n);
    for (const Unit& u : units_) {
        if (!is_fighter(u)) continue;
        const FighterState& f = u.fs;
        h.i32(u.d.id);
        hash_f3(h, f.b.p);
        hash_f3(h, f.b.v);
        hash_att(h, f.b.o);
        h.boolean(f.b.loop);
        hash_f3(h, f.dest);
        hash_att(h, f.stored);
        h.u32(f.idx);
        hash_cell(h, f.dog);
        hash_cell(h, f.park);
        h.i32(f.lock);
        h.u32(f.lock_start);
        h.u32(f.lock_frames);
        h.i32(f.hold);
        hash_target(h, f.target);
        h.i32(f.squad);
    }
    h.u32(static_cast<uint32_t>(squads_.size()));
    for (const FighterSquad& s : squads_) {
        h.i32(s.id);
        h.u32(static_cast<uint32_t>(s.members.size()));
        for (ObjectId m : s.members) h.i32(m);
        for (const F3& sl : s.slots) hash_f3(h, sl);
        h.f32(s.tolerance);
        h.f32(s.error);
        hash_f3(h, s.spread);
        hash_cell(h, s.dog);
        hash_cell(h, s.park);
        hash_f3(h, s.dest);
        hash_target(h, s.target);
        if (!s.guard_follow) h.u8(0x47);  // S7 t37: only when not following (format unchanged otherwise)
    }
    h.u32(static_cast<uint32_t>(fevents_.size()));
    for (const FighterEvent& e : fevents_) {
        h.u8(static_cast<uint8_t>(e.kind));
        h.i32(e.who);
        h.i32(e.target);
    }
    if (!fforms_.empty()) {  // s78f t02 (replaces S7 t37's "GRDD")
        h.u32(0x46464F52u);  // "FFOR"
        h.u32(static_cast<uint32_t>(fforms_.size()));
        for (const FighterFormation& g : fforms_) {
            h.i32(g.key);
            h.u32(static_cast<uint32_t>(g.blocks.size()));
            for (const FighterBlock& b : g.blocks) {
                h.u8(b.kind);
                h.i32(b.object);
                h.i32(b.hard_point);
                h.f32(b.point.x);
                h.f32(b.point.y);
                h.f32(b.facing);
                h.f32(b.threat);
                h.u8(b.flags);
                h.boolean(b.pushed);
                h.boolean(b.own_speed);
                h.boolean(b.loop);
            }
            h.u8(g.state);
            h.f32(g.goal.x);
            h.f32(g.goal.y);
            h.u32(g.next);
            h.f32(g.speed);
            h.boolean(g.single_arrived);
            h.boolean(g.follow);
        }
    }
}

}  // namespace alamo::tactical::movement

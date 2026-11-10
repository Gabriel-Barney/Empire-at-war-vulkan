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

uint64_t cell_key(int32_t x, int32_t y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32) | static_cast<uint32_t>(x);
}

bool contains_key(const std::vector<uint64_t>& v, uint64_t k) { return std::binary_search(v.begin(), v.end(), k); }
void insert_key(std::vector<uint64_t>& v, uint64_t k) {
    auto it = std::lower_bound(v.begin(), v.end(), k);
    if (it == v.end() || *it != k) v.insert(it, k);
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
    id = order_id(id);  // s78n t03: a steering member's coordinator is its container's
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
        V2 pos;  // the object position: the seed pick and D
        // s78n t10 (TACTICAL_COMBAT §5.4.6 port spec item 4): the x / y centre and half size of the world AABB 0x3982b0.
        V2 bc;
        float ex = 0.0f, ey = 0.0f;
        uint32_t role = 0;
        float vmax = 0.0f, vmin = 0.0f;
    };
    std::vector<Cand> cands;
    // s78n t10: one unit's world box (SPACE_AVOIDANCE §3.1) from the host's model data - the collision bound through the
    // unit's position, yaw and Scale_Factor; no model: [P, P + 0.1]. [port] yaw only (a fighter's pitch / roll are not
    // applied). False: no host data.
    const auto unit_box = [this](const Unit& x, AvoidObstacle& o) {
        const AvoidModel* model = nullptr;
        float scale = 1.0f;
        if (!model_bound_ || !model_bound_(x.d.id, model, scale)) return false;
        o.id = x.d.id;
        o.pos = is_fighter(x) ? x.fs.b.p : F3{x.m.pos.x, x.m.pos.y, x.z};
        o.facing = dir_from_deg(x.m.yaw);
        o.scale = scale;
        o.model = model;
        avoid_place(o);
        return true;
    };
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
        // s78n t10: the world AABB 0x3982b0 (Custom_Hard extents are not read there). A ship: its model box. A squadron
        // container: the team box = the min / max union of its fighters' boxes, team index order [M: the union rule].
        // [port] without host data (no hook, or a model that does not load): the object position with the hard extents
        // (Custom_Hard_X/YExtent x Scale_Factor), else the radius; a squadron: its first fighter's.
        bool boxed = false;
        if (squad) {
            V2 lo{}, hi{};
            for (const ObjectId mid : squad_ptr(key)->members) {
                const Unit* m = unit_ptr(mid);
                if (!m) continue;
                AvoidObstacle o;
                if (!unit_box(*m, o)) {
                    boxed = false;
                    break;
                }
                const V2 l{o.box_c.x - o.box_h.x, o.box_c.y - o.box_h.y}, h{o.box_c.x + o.box_h.x, o.box_c.y + o.box_h.y};
                if (!boxed) {
                    lo = l;
                    hi = h;
                    boxed = true;
                } else {
                    lo = {std::min(lo.x, l.x), std::min(lo.y, l.y)};
                    hi = {std::max(hi.x, h.x), std::max(hi.y, h.y)};
                }
            }
            if (boxed) {
                c.bc = {(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f};
                c.ex = (hi.x - lo.x) * 0.5f;
                c.ey = (hi.y - lo.y) * 0.5f;
            }
        } else {
            AvoidObstacle o;
            boxed = unit_box(*u, o);
            if (boxed) {
                c.bc = {o.box_c.x, o.box_c.y};
                c.ex = o.box_h.x;
                c.ey = o.box_h.y;
            }
        }
        if (!boxed) {
            c.bc = c.pos;
            c.ex = u->d.hard_x > 0.0f ? u->d.hard_x : u->d.radius;
            c.ey = u->d.hard_y > 0.0f ? u->d.hard_y : u->d.radius;
        }
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
        // cu8l t03 (CU8K_T03_T08_T09_RE §3 item 1): a locked seed (0x39ad40) recruits nobody; candidates are not tested.
        if (locked_ && locked_(seed.key)) continue;
        // Role 0 (fighters, squadron containers, objects without a layer): recruit in candidate order. [port] every space
        // locomotor is kind 2.
        // s78n t10: |cc - cs| between the box centres; the reach term keeps D = |goal - seed position|.
        const float reach = 0.33f * length(pc.goal - seed.pos);
        for (size_t i = 0; i < cands.size();) {
            const Cand& c = cands[i];
            const float dx = std::fabs(c.bc.x - seed.bc.x), dy = std::fabs(c.bc.y - seed.bc.y);
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
    const Unit* u = unit_ptr(order_id(id));  // s78n t03: a steering member answers for its container
    // cu8l t02 [port]: "in a formation" = a member of a formation of > 1 unit, or a unit whose composed move (not waiting
    // for its composition, not an adopt's degenerate hold) is still running. A unit waiting for its composition or held
    // by an adopt is in a coordinator without a formation of its own.
    if (!u) return CoordinatorHold::None;
    if (u->adopted || u->pend != 0) return CoordinatorHold::NoFormation;
    if (u->in_formation || u->has_goal) return CoordinatorHold::Formation;
    return CoordinatorHold::None;
}

bool MovementSystem::split_from_formation(ObjectId id, SimRng& rng) {
    id = order_id(id);  // s78n t03: a steering member's formation is its container's
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
    if (cfg_.mode == MoveMode::Land && it->d.team_container) {
        // s78n t03 (TACTICAL_COMBAT §5.4.4 item 2.1): the team helper moves the new container (at its members' mean) by the
        // free-position search 0x440f90 [M: the mode and the container type's +0x1688 movement class; port: the ring
        // search of forced_spot with the container's class], so it can sit beside a blocked centroid.
        V2 spot;
        if (forced_spot(*it, it->m.pos, spot)) it->m.pos = spot;
    }
    if (cfg_.mode == MoveMode::Land) {
        const float r = tree_half(*it);
        const V2 p = it->m.pos;
        tree_.insert(static_cast<EntityHandle>(d.id), Aabb{{p.x - r, p.y - r, -1.0f}, {p.x + r, p.y + r, 1.0f}}, 1u);
    } else {
        park_space(*it, 0);
    }
    return true;
}

bool MovementSystem::remove_unit(ObjectId id) {
    // s78n review: a parked squad member removed for good (it died inside its garrison) gives up its team place.
    const size_t dropped = std::erase_if(parked_, [id](const LandTeamSlot& p) { return p.member == id; });
    const bool removed = remove_impl(id);
    // s78n t18 review: the record was a TEAM entry (a member suspended for a hero clash, or absent in a garrison - neither
    // is a steering member, so remove_impl did not rebuild): the entry list, the joint box and the container's radius
    // follow at once, as a load would rebuild them.
    if (dropped > 0) team_ix_rebuild();
    return removed;
}

bool MovementSystem::remove_impl(ObjectId id) {
    const int32_t i = find(id);
    if (i < 0) return false;
    if (cfg_.mode == MoveMode::Land) tree_.remove(static_cast<EntityHandle>(id));
    else space_.clear_track(id);
    fighter_removed(id);  // FIGHTER_FLYING: squadrons, cells, locks, targets naming it
    const bool steering = units_[static_cast<size_t>(i)].anchor >= 0 || units_[static_cast<size_t>(i)].d.team_container;
    units_.erase(units_.begin() + i);
    for (Unit& m : units_)  // s78n t03: a removed container releases its members
        if (m.anchor == id) {
            m.anchor = -1;
            m.recovery = 0;
            m.start_delay = 0;
        }
    std::erase_if(parked_, [id](const LandTeamSlot& p) { return p.container == id; });  // s78n review: and its parked ones
    if (steering) team_ix_rebuild();
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
    v.push_kind = u->push_kind;  // s78n t20
    v.push_flee = u->pushed && u->push_flee;
    v.form_id = u->form_id;
    v.goal = u->goal;
    if (u->anchor >= 0) {  // s78n t03: a steering member moves with its container's order (and while recovering)
        if (const Unit* k = unit_ptr(u->anchor)) {
            v.has_goal = k->has_goal || u->recovery != 0;
            v.arrived = k->arrived;
            v.failed = k->failed;
            v.goal = k->goal;
        }
    }
    v.replans = u->replans;
    v.space_fallback = u->fallback;
    v.start_frame = u->start_frame;  // OI tactical
    v.formation_speed = u->formation_speed;
    v.team_speed = u->team_cap;  // s78k t03
    v.team_coast = u->d.team_container && u->coast;  // s78n t18 review
    v.team_final_turn = u->d.team_container && u->fturn;
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
    const float r = tree_half(u);
    tree_.update(static_cast<EntityHandle>(u.d.id),
                 Aabb{{u.m.pos.x - r, u.m.pos.y - r, -1.0f}, {u.m.pos.x + r, u.m.pos.y + r, 1.0f}});
}

// ---- s78n t18 (MOVEMENT §5.3.1 / §5.3.2): footprints and the layer-0x10 box query
V2 MovementSystem::foot_half(const Unit& u) const {
    if (u.d.team_container && u.team_boxed) return {u.team_box_hx, u.team_box_hy};
    if (u.d.hard_x > 0.0f || u.d.hard_y > 0.0f) return {std::max(u.d.hard_x, 0.0f), std::max(u.d.hard_y, 0.0f)};
    return {u.d.radius, u.d.radius};  // [port] no hard extents (the model-box fallback of 0x371be0 is not loaded)
}

float MovementSystem::foot_radius(const Unit& u) const {
    if ((u.d.team_container && u.team_boxed) || u.d.hard_x > 0.0f || u.d.hard_y > 0.0f) {
        const V2 h = foot_half(u);
        return std::sqrt(h.x * h.x + h.y * h.y);
    }
    return u.d.radius;  // [port] no hard extents: the soft radius (as the push search took it)
}

FootRect MovementSystem::foot_of(const Unit& u) const { return foot_at(u, u.m.pos); }

FootRect MovementSystem::foot_at(const Unit& u, V2 p) const {
    const V2 h = foot_half(u);
    const bool boxed = (u.d.team_container && u.team_boxed) || u.d.hard_x > 0.0f || u.d.hard_y > 0.0f;
    return boxed ? foot_rect(p, u.m.yaw, h.x, h.y) : foot_circle(p, u.m.yaw, u.d.radius);  // [port] no hard extents: a circle
}

float MovementSystem::tree_half(const Unit& u) const { return std::max(std::max(foot_radius(u), u.d.radius), 1.0f); }

void MovementSystem::layer_box_query(V2 centre, float half, std::vector<uint32_t>& out) const {
    out.clear();
    // the tree holds every unit's box of at least its footprint radius (tree_half), so no layer object is missed
    const tactical::Aabb box{{centre.x - half, centre.y - half, -1.0f}, {centre.x + half, centre.y + half, 1.0f}};
    for (EntityHandle h : tree_.query_aabb(box, QueryFilter{}, qctx_)) {
        const int32_t ci = find(static_cast<ObjectId>(h));
        if (ci < 0) continue;
        const Unit& o = units_[static_cast<size_t>(ci)];
        if (!land_layer_object(o)) continue;
        const float r = foot_radius(o);
        if (std::fabs(o.m.pos.x - centre.x) < half + r && std::fabs(o.m.pos.y - centre.y) < half + r)
            out.push_back(static_cast<uint32_t>(ci));
    }
    std::sort(out.begin(), out.end());
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
    // s78n t18 [port]: a hidden unit (a squad container garrisoned with its whole squad) takes no move order
    std::erase_if(idx, [this](uint32_t i) { return units_[i].hidden; });
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
    if (cfg_.mode == MoveMode::Land) move_land(order, idx, frame, delays, serial, rng);
    else move_space(order, idx, frame, delays, serial);
    if (!fform_internal_) fform_order(order, idx, frame, serial);  // s78f t02: a new formation holding the order's block
}

void MovementSystem::move_land(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame,
                               const std::vector<uint32_t>& delays, uint32_t serial, SimRng& rng) {
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
        if (u.d.team_container && u.team_cap < kInf) f.max_speed = u.team_cap;  // s78n t03: a container walks at the team speed
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
    std::vector<LandForm> forms;  // s78n t20: every formation of the order, laid out before any goal is given
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
            LandForm lf;
            lf.members = f;
            lf.centroid = c;
            lf.speed = fspeed;
            lf.fwd = fwd;
            lf.slots = layout_formation(mem, f, fwd, k);
            lf.centre = order.destination;
            forms.push_back(std::move(lf));
        }
    }
    // s78n t20 (MOVEMENT §4.3.1 item 2): the destination check - not for a forced-path order (flag 0x20), an object goal
    // (the host's skip_destination_check) or a queued order (no composition pass). It never refuses the order: it only
    // moves the centre the formations' slots are laid around.
    const bool queued = (order.flags & kMoveFlagQueue) != 0;
    const bool checked = !(order.flags & kMoveFlagSkipValidation) && !order.skip_destination_check && !queued;
    if (checked) destination_check(order, idx, forms, group_c);
    {
        for (const LandForm& lf : forms) {
            const std::vector<uint32_t>& f = lf.members;
            const V2 fwd = lf.fwd;
            const float fspeed = lf.speed;
            const uint32_t form_id = ++form_serial_;
            for (const Slot& s : lf.slots) {
                Unit& u = units_[idx[s.member]];
                u.large_hover_index = -1;
                V2 target = slot_world(lf.centre, fwd, s);
                if (!(order.flags & kMoveFlagSkipValidation)) target = pick_land_slot(u, target, taken);
                // s78n t20 (MOVEMENT §5.4.4 item 5, §4.3.1 item 6): the formation identity and the slot in its frame;
                // the single-member shortcut of the plan (no flag 0x20, no object goal; [port] flag 0x10 has no sender)
                u.form_id = form_id;
                u.form_slot = {-s.back, s.lateral};
                u.self_arrive = f.size() == 1 && !(order.flags & kMoveFlagSkipValidation) && !order.skip_destination_check;
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
                u.exit_host = -1;  // s78n t18 (MOVEMENT §5.3): a new order ends the garrison-host gate of a unit that left
                if (u.d.team_container) {
                    // s78n t18 (FIGHTER_FLYING §7.7 item 5): a new order while moving is not taken at once - the
                    // container brakes at its rate (0x14), coasting straight on, then starts the new path from speed 0
                    u.coast = u.m.speed > 0.0f;
                    u.fturn = false;
                    u.path_started = false;
                    if (!u.coast) u.m.state = LocoState::WalkStopped;
                }
                u.jiggle = V2{};
                u.blocked_frames = 0;
                end_push(u);  // a1k t03 [port]: a new move ends a pushed move
                u.replans = 0;
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
    // s78n t14 (FIGHTER_FLYING §7.3 item 2, the path-set hook 0x5ad140 for a kind-1 path): a squad container standing still
    // (speed 0) that gets this move draws every steering member's start delay, synced int [0, Max_Move_Frame_Delay], in team
    // index order. [port] drawn here at the order (the order's units in index order, after the formation layout), not at
    // the path's arrival from the planner.
    for (uint32_t i : idx) {
        const Unit& tc = units_[i];
        if (!tc.d.team_container || tc.m.speed != 0.0f) continue;
        for (const LandTeamSlot& e : land_team(tc.d.id))
            if (Unit* m = unit_ptr(e.member))
                m->start_delay = static_cast<uint32_t>(rng.range_int(0, cfg_.constants.max_move_frame_delay));
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

// ---- s78n t20 (MOVEMENT §4.3.1 items 3 - 4): the destination check of a land move order ----
// 0x439360: every formation whose lead (its seed) is a land layer object, nearest first to the group centroid C. The
// first one is test-planned to the ordered goal D; fewer than 2 path points end the pass with nothing changed. Axis a =
// the order's facing, else the direction d1 of that path's last segment; phi = the signed angle from its first to its
// last segment. Passes n = 0 .. 40 over the centre Dn (D, then 20 x ceil(n / 8) out at (n mod 8) x 45 degrees; Dn must
// pass the lead's class, be zone-connected to D and reachable from it by a straight line or a path <= 1.8 x the
// distance): each formation is placed by place_formation with the step direction v = normalise(rotate(centroid - C,
// phi)), or -d1 for the only formation or a zero offset; a formation that cannot be placed sends the pass to the next n.
// After n = 40 every formation keeps D. The order is never refused. [port] an ordered goal that does not pass the lead's
// class is left to the slot search (pick_land_slot), unchecked; the taken rectangles of a failed pass are dropped.
void MovementSystem::destination_check(const MoveOrder& order, const std::vector<uint32_t>& idx, std::vector<LandForm>& forms,
                                       V2 group_c) {
    const V2 dest = order.destination;
    std::vector<uint32_t> ord;
    for (uint32_t i = 0; i < forms.size(); ++i)
        if (land_layer_object(units_[idx[forms[i].members[0]]])) ord.push_back(i);
    if (ord.empty()) return;
    std::stable_sort(ord.begin(), ord.end(), [&](uint32_t x, uint32_t y) {
        return length_sq(forms[x].centroid - group_c) < length_sq(forms[y].centroid - group_c);
    });
    const Unit& lead = units_[idx[forms[ord[0]].members[0]]];
    if (!grid_.passable_world(dest, lead.lp.mask)) return;
    LandPathScratch sc;
    LandPath test;
    {
        LandPathRequest q;
        q.start = lead.m.pos;
        q.goal = dest;
        q.mask = lead.lp.mask;
        q.clearance_radius = lead.d.radius > kCellSize && !lead.d.team_container ? lead.d.radius : 0.0f;  // as plan_land
        find_land_path(grid_, regions_, q, sc, test);
    }
    if (!test.ok || test.points.size() < 2) return;
    const size_t np = test.points.size();
    const V2 d0 = normalize_retail(test.points[1] - test.points[0]);
    const V2 d1 = normalize_retail(test.points[np - 1] - test.points[np - 2]);
    if (!(length_sq(d1) > 0.0f)) return;
    const V2 axis = order.facing_deg != kNoFacing ? dir_from_deg(order.facing_deg) : d1;
    const float cphi = length_sq(d0) > 0.0f ? dot(d0, d1) : 1.0f, sphi = length_sq(d0) > 0.0f ? cross(d0, d1) : 0.0f;
    const int32_t comp = regions_.component_at(lead.slot, world_to_cell(dest.x), world_to_cell(dest.y));
    const bool several = ord.size() > 1;
    constexpr float kC = 0.70710678f;
    static constexpr V2 kRing[8] = {{1.0f, 0.0f}, {kC, kC}, {0.0f, 1.0f}, {-kC, kC}, {-1.0f, 0.0f}, {-kC, -kC}, {0.0f, -1.0f}, {kC, -kC}};
    std::vector<FootRect> taken;
    std::vector<V2> centres(ord.size());
    for (uint32_t n = 0; n <= 40; ++n) {
        V2 dn = dest;
        if (n > 0) {
            dn = dest + kRing[n % 8u] * (20.0f * static_cast<float>((n + 7u) / 8u));
            const int32_t cx = world_to_cell(dn.x), cy = world_to_cell(dn.y);
            if (!grid_.passable_cell(cx, cy, lead.lp.mask) || regions_.component_at(lead.slot, cx, cy) != comp) continue;
            if (!line_passable(grid_, dest, dn, lead.lp.mask)) {
                LandPathRequest q;
                q.start = dest;
                q.goal = dn;
                q.mask = lead.lp.mask;
                q.snap_goal = false;
                LandPath lp;
                find_land_path(grid_, regions_, q, sc, lp);
                if (!lp.ok || path_length(lp.points) > 1.8f * distance(dest, dn)) continue;
            }
        }
        taken.clear();
        bool ok = true;
        for (size_t r = 0; r < ord.size() && ok; ++r) {
            const LandForm& lf = forms[ord[r]];
            V2 v = d1 * -1.0f;
            if (several) {
                const V2 off = lf.centroid - group_c;
                const V2 rot = normalize_retail(V2{off.x * cphi - off.y * sphi, off.x * sphi + off.y * cphi});
                if (length_sq(rot) > 0.0f) v = rot;
            }
            ok = place_formation(idx, lf, dn, v, axis, r == 0, several, comp, taken, centres[r]);
        }
        if (!ok) continue;
        for (size_t r = 0; r < ord.size(); ++r) forms[ord[r]].centre = centres[r];
        return;
    }
}

// 0x43d6a0, one formation: S = BetweenFormationSpacing, E = the destination query extension; (HX, HY) = the half extents
// of the union of its members' slot rectangles (slot +- the member's footprint half extents 0x396850; a lone squad
// container: its joint slot box); R = the rectangle (HX + S, HY + S) on `axis`. Up to 40 tries (destination_try_offset).
// A try is rejected when: several formations and R contains Dn; R overlaps a rectangle taken in this pass; its centre
// cell or R is impassable ([port] R's corners; this sets "impassable ground met"); after that, it is not zone-connected
// to D or its path from Dn is > 1.8 x the straight line; or the object test fails. Object test: the layer objects in R
// grown by E, without the order's own units and, for a formation that is all Infantry / Bike or all Giant and not all
// squad containers, without single Infantry-class units; no owner test and no moving test. Each counts where it is
// going - its footprint at the end of its path with the final direction when it has one - and is a blocker when a member's
// slot rectangle (its half extents + S) overlaps it, the two classes interact and the member cannot crush it. Rejected
// iff the sum of the blockers' hx x hy > 0.15 x (HX + S)(HY + S). Accepted: `centre` and R joins the taken list.
// [port] the union box is centred where the slots put it (the port's layout is not centred on the destination).
bool MovementSystem::place_formation(const std::vector<uint32_t>& idx, const LandForm& f, V2 dn, V2 v, V2 axis, bool first,
                                     bool several, int32_t comp, std::vector<FootRect>& taken, V2& centre) const {
    const float sp = cfg_.constants.between_formation_spacing, ext = cfg_.constants.destination_query_extension;
    const Unit& lead = units_[idx[f.members[0]]];
    float x0 = 3.0e38f, x1 = -3.0e38f, y0 = 3.0e38f, y1 = -3.0e38f;
    bool all_inf = true, all_giant = true, all_containers = true;
    for (const Slot& s : f.slots) {
        const Unit& m = units_[idx[s.member]];
        const V2 h = foot_half(m);
        x0 = std::min(x0, -s.back - h.x);
        x1 = std::max(x1, -s.back + h.x);
        y0 = std::min(y0, s.lateral - h.y);
        y1 = std::max(y1, s.lateral + h.y);
        const int32_t cls = m.d.collision_class;
        all_inf = all_inf && (cls == kUccInfantry || cls == kUccBike);
        all_giant = all_giant && cls == kUccGiantVehicle;
        all_containers = all_containers && m.d.team_container;
    }
    if (f.slots.empty()) return false;
    const float hx = 0.5f * (x1 - x0) + sp, hy = 0.5f * (y1 - y0) + sp;
    const V2 box_off = axis * (0.5f * (x1 + x0)) + formation_right(axis) * (0.5f * (y1 + y0));
    const float m_side = std::min(hx, hy);
    const bool skip_infantry = (all_inf || all_giant) && !all_containers;
    const auto inside = [](const FootRect& r, V2 p) {
        const V2 d = p - r.c;
        return std::fabs(dot(d, r.a)) <= r.hx && std::fabs(dot(d, perp_left(r.a))) <= r.hy;
    };
    std::vector<uint32_t> cand;
    LandPathScratch sc;
    bool blocked_seen = false;
    for (uint32_t k = 0; k < 40; ++k) {
        const V2 c = dn + destination_try_offset(k, first && !blocked_seen, v, m_side);
        const FootRect rect{c + box_off, axis, hx, hy, false};
        if (several && inside(rect, dn)) continue;
        bool hit = false;
        for (const FootRect& t : taken) hit = hit || foot_overlap(rect, t);
        if (hit) continue;
        bool pass = grid_.passable_world(c, lead.lp.mask) && grid_.passable_world(rect.c, lead.lp.mask);
        const V2 b = perp_left(axis);
        for (int corner = 0; corner < 4 && pass; ++corner) {
            const V2 p = rect.c + axis * ((corner & 1) ? hx : -hx) + b * ((corner & 2) ? hy : -hy);
            pass = grid_.passable_world(p, lead.lp.mask);
        }
        if (!pass) {
            blocked_seen = true;
            continue;
        }
        if (blocked_seen) {
            if (regions_.component_at(lead.slot, world_to_cell(c.x), world_to_cell(c.y)) != comp) continue;
            if (!line_passable(grid_, dn, c, lead.lp.mask)) {
                LandPathRequest q;
                q.start = dn;
                q.goal = c;
                q.mask = lead.lp.mask;
                q.snap_goal = false;
                LandPath lp;
                find_land_path(grid_, regions_, q, sc, lp);
                if (!lp.ok || path_length(lp.points) > 1.8f * distance(dn, c)) continue;
            }
        }
        // ---- the object test
        layer_box_query(rect.c, std::sqrt(hx * hx + hy * hy) + ext, cand);
        float sum = 0.0f;
        for (uint32_t ci : cand) {
            if (std::find(idx.begin(), idx.end(), ci) != idx.end()) continue;  // the order's own units
            const Unit& o = units_[ci];
            if (skip_infantry && o.d.collision_class == kUccInfantry && !o.d.team_container) continue;
            // where it is going (0x43a7f0): the end of its path with the final direction when it has a path
            FootRect ro = foot_of(o);
            V2 end = o.m.pos, dir{};
            bool has_path = false;
            if (o.has_goal) {
                has_path = true;
                end = o.goal;
                const size_t n = o.m.path.size();
                dir = !o.need_path && n >= 2 ? o.m.path[n - 1] - o.m.path[n - 2] : o.goal - o.m.pos;
            } else if (o.pushed && o.push_path.size() >= 2) {
                has_path = true;
                const size_t n = o.push_path.size();
                end = o.push_path[n - 1];
                dir = o.push_path[n - 1] - o.push_path[n - 2];
            }
            if (has_path) {
                ro.c = end;
                const V2 dnorm = normalize_retail(dir);
                if (length_sq(dnorm) > 0.0f) ro.a = dnorm;
            }
            bool blocker = false;
            for (const Slot& s : f.slots) {
                const Unit& m = units_[idx[s.member]];
                if (!collision_classes_interact(m.d.collision_class, o.d.collision_class) || crushes(m, o)) continue;
                const V2 h = foot_half(m);
                const FootRect mr{slot_world(c, f.fwd, s), f.fwd, h.x + sp, h.y + sp, false};
                if (foot_overlap(mr, ro)) {
                    blocker = true;
                    break;
                }
            }
            if (blocker) {
                const V2 h = foot_half(o);
                sum += h.x * h.y;
            }
        }
        if (sum > 0.15f * hx * hy) continue;
        centre = c;
        taken.push_back(rect);
        return true;
    }
    return false;
}

float MovementSystem::collision_radius(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u ? steer_radius(*u) : 0.0f;
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
        Unit* u = unit_ptr(order_id(id));  // s78n t03: a steering member's stop stops its container
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
        u->around.clear();  // s78n t18: the path stack is cleared
        u->form_id = 0;     // s78n t20: it leaves its formation
        u->self_arrive = false;
        u->coast = u->fturn = u->path_started = false;  // s78n t18 (0x5ad320): state 0x16, the speed zeroed at once
        u->exit_host = -1;  // s78n t18 [port]: Stop counts as the unit's next order for the garrison-host gate
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
    u->around.clear();  // s78n t18
    u->form_id = 0;     // s78n t20
    u->self_arrive = false;
    if (cfg_.mode == MoveMode::Land) u->path_change = frame;
    u->coast = u->fturn = u->path_started = false;
    end_push(*u);  // a1k t03 [port]: a teleport ends a pushed move
    const V2 old = u->m.pos;
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
    if (cfg_.mode == MoveMode::Land && u->d.team_container) {
        // s78n t18 (FIGHTER_FLYING §7.4, 0x3abb80(K, pos)): every member of the container's TEAM list is put at pos +
        // (m.P - K.P), its offset from the container kept, with its locomotor idle; member facings are not written. The
        // steering members and the members suspended for a hero clash ([port] they are TEAM entries too); a parked
        // (garrisoned) member is not in the system and keeps its stored offset. Start delays, jiggle, the slots and the
        // laid-out flag are untouched.
        const ObjectId k = u->d.id;
        const auto place = [&](Unit& m) {
            m.m.pos = pos + (m.m.pos - old);
            m.m.speed = 0.0f;
            m.m.state = LocoState::WalkStopped;
            m.recovery = 0;
            if (height_) m.z = height_(m.m.pos.x, m.m.pos.y);
            update_tree(m);
        };
        for (Unit& m : units_)
            if (m.anchor == k) place(m);
        for (const LandTeamSlot& p : parked_)
            if (p.container == k)
                if (Unit* m = unit_ptr(p.member)) place(*m);
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
            // s78n t14 part B: a squad container is no victim (its members are; the container is pushed, see resolve_blocker)
            if (crushed[vi] || !crushes(cr, v) || !ground_collider(v) || v.d.team_container) continue;
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
    for (uint32_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (!u.need_path || frame < u.start_frame) continue;
        // s78n t20 (MOVEMENT §4.3.1 item 6, the plan 0x41c1a0): a one-member formation whose goal lies inside its own
        // footprint radius 0x3969f0 (2-D) signals "arrived" and stops: a unit ordered onto its own footprint does not move
        if (u.self_arrive && !is_air(u) && u.d.locomotor == LocomotorKind::Default && distance(u.m.pos, u.goal) < foot_radius(u)) {
            u.need_path = false;
            u.has_goal = false;
            u.arrived = true;
            u.m.path.clear();
            u.m.next = 0;
            u.around.clear();
            u.path_change = frame;
            u.exit_host = -1;
            if (!u.d.team_container) {  // (a moving container brakes to a stop at its rate, team_walk)
                u.m.speed = 0.0f;
                u.m.state = LocoState::WalkStopped;
            }
            continue;
        }
        req.push_back(i);
    }
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
            // s78n t03 [port]: a squad container plans without clearance (its planner footprint is untraced; its radius is
            // the joint box's, for the formation layout).
            q.clearance_radius = u.d.radius > kCellSize && !u.d.team_container ? u.d.radius : 0.0f;
            if (u.threat != kThreatOff && u.d.space_layer_type == 0) q.threat = ThreatCost::make(tf ? &tf : nullptr, u.threat);
            find_land_path(grid_, regions_, q, sc, out[r]);
        }
    });
    for (size_t r = 0; r < req.size(); ++r) {
        Unit& u = units_[req[r]];
        u.need_path = false;
        ++stats_.paths_planned;
        u.around.clear();  // s78n t18: a new path (pack +0x288 := NOW), planned around nobody
        u.path_change = frame;
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
// Candidates of unit `ai` (§5.2 item 4): the layer-0x10 objects in the 160 x 160 square around it, ascending id (index).
void MovementSystem::land_candidates(size_t ai, std::vector<uint32_t>& out) const {
    layer_box_query(units_[ai].m.pos, 0.5f * kCollisionCrowdBox, out);
    std::erase(out, static_cast<uint32_t>(ai));
}

// [port] "Moving" as the port read it before s78n t20 (the checker has started its order; the other participant has an
// order path): kept only for the MovingVsMoving stand-in of pairs that are not two Infantry / Bike movers.
bool MovementSystem::checker_moving(const Unit& u, uint32_t frame) const {
    return u.has_goal && !u.need_path && u.m.next < u.m.path.size() && frame >= u.start_frame;
}
bool MovementSystem::other_moving(const Unit& u) { return u.has_goal && !u.need_path && u.m.next < u.m.path.size(); }

// s78n t18 (MOVEMENT §5.3.1 item 3): the MovingVsMoving predictor 0x442540 - both movers along their remaining paths at
// their assumed speeds, directional radii of their footprint rectangles with the lane rule, inside trunc(fps x
// MovingVsMovingLookAheadTime) frames (45), + 1.5 s when either can crush the other and that other scatters.
bool MovementSystem::mvm_contact(const Unit& a, const Unit& b) const {
    // Gates (MOVEMENT §5.3): garrison host either side; MovingVsMoving only between equal raised states
    if (pair_gated(a, b) || raised(a) != raised(b)) return false;
    const float fps_t = 30.0f * cfg_.constants.moving_vs_moving_look_ahead_time;
    int32_t frames = static_cast<int32_t>(fps_t);
    if ((crushes(a, b) && scatters(b)) || (crushes(b, a) && scatters(a))) frames += 45;
    std::vector<V2> sa, sb;
    const auto track = [this](const Unit& u, std::vector<V2>& scratch) {
        FootTrack t;
        t.pos = u.m.pos;
        t.yaw = u.m.yaw;
        const StackPoly sp = stack_poly(u, scratch);  // s78n t20: along the path stack
        t.path = sp.pts;
        t.next = sp.next;
        t.speed = u.m.speed;
        t.max_speed = u.d.team_container && u.team_cap < kInf ? u.team_cap : u.lp.max_speed;
        const V2 h = foot_half(u);
        t.hx = h.x;
        t.hy = h.y;
        return t;
    };
    return foot_moving_contact(track(a, sa), track(b, sb), frames);
}

// MovingVsStopped contact: distance along a's remaining path to touching b within the look-ahead, or -1. s78n t14 part B
// (MOVEMENT §5.3 0x683790): the look-ahead is max(MovingVsMovingLookAheadTime, the type's MovementPredictionInterval) -
// s78n t18 review: of every mover type (FoC: AT-AT and Veers 5.0, Pod Walker 12, SPMA-T 4.0, Juggernaut 3.0, T4-B 3.0),
// not only of squad containers - plus 1.5 s when the mover can push the obstacle (0x5c8900 crushes, 0x5c8890 scatters).
// s78n t18 (MOVEMENT §5.3.1 item 2, 0x441b60): the swept-point test of the mover's path against the obstacle's
// rectangle centre with the directional radii rho(mover, cos to the segment direction) + rho(obstacle, cos to its
// facing); hit when the path length d to the contact has d / max speed < the look-ahead frames. While the mover turns in
// place (the port's start turn) the test is one zero-length segment at its position with its facing as the axis and its
// box swollen toward a square by clamp(|heading error| / 90 degrees).
float MovementSystem::mvs_look_distance(const Unit& a, const Unit& b) const {
    // 0x683790: d / max speed < fps x max(MovingVsMovingLookAheadTime, the type's MovementPredictionInterval), + 1.5 s
    // when the mover can push the obstacle (it crushes it and the obstacle scatters)
    float secs = cfg_.constants.moving_vs_moving_look_ahead_time;
    secs = std::max(secs, a.d.movement_prediction_interval);
    if (crushes(a, b) && scatters(b)) secs += 1.5f;
    const float vmax = a.d.team_container && a.team_cap < kInf ? a.team_cap : a.lp.max_speed;
    return vmax * secs * 30.0f;
}

float MovementSystem::mvs_contact(const Unit& a, const Unit& b) const {
    if (!unit_moving(a)) return -1.0f;
    // Gates (MOVEMENT §5.3): garrison host either side; a raised mover takes only a raised obstacle
    if (pair_gated(a, b) || (raised(a) && !raised(b))) return -1.0f;
    const float look = mvs_look_distance(a, b);
    std::vector<V2> scratch;
    const StackPoly sp = stack_poly(a, scratch);  // s78n t20: the path stack (the top record, then the order path)
    if (!(look > 0.0f) || sp.next >= sp.pts.size()) return -1.0f;
    V2 ha = foot_half(a);
    float turn_yaw = kNoFacing;
    if (a.m.state == LocoState::WalkStartTurn) {
        const float err = std::fabs(wrap_deg(heading_deg(sp.pts[sp.next] - a.m.pos) - a.m.yaw));
        const float k = std::clamp(err / 90.0f, 0.0f, 1.0f), mx = std::max(ha.x, ha.y);
        ha = {ha.x + (mx - ha.x) * k, ha.y + (mx - ha.y) * k};
        turn_yaw = a.m.yaw;
    }
    return foot_path_contact(a.m.pos, sp.pts, sp.next, ha, foot_of(b), look, turn_yaw);
}

// StoppedVsStopped contact 0x686500 (MOVEMENT §5.3 / §5.3.1 item 1): the garrison-host gate (no hover test), then the
// overlap coefficient of the two footprint rectangles > MinimumStoppedVsStoppedOverlapCoefficient.
bool MovementSystem::svs_overlap(const Unit& a, const Unit& b) const {
    return !pair_gated(a, b) && foot_overlap_coefficient(foot_of(a), foot_of(b)) > cfg_.constants.minimum_stopped_vs_stopped_overlap;
}

// [port] Keep test of the stand-in kinds (StoppedVsStopped, MovingVsMoving of pairs that are not Infantry / Bike): both
// participants still exist and collide, and the classification that created the collision still holds this frame.
bool MovementSystem::collision_applies(const CollisionObject& c, uint32_t frame) const {
    const int32_t ia = find(c.a), ib = find(c.b);
    if (ia < 0 || ib < 0) return false;
    const Unit& a = units_[static_cast<size_t>(ia)];
    const Unit& b = units_[static_cast<size_t>(ib)];
    if (!land_layer_object(a) || !land_layer_object(b)) return false;  // FIGHTER_FLYING §7.8: never a squad member
    if (!collision_classes_interact(a.d.collision_class, b.d.collision_class)) return false;
    switch (c.kind) {
        case CollisionKind::MovingVsMoving:
            return !drives_through(a, b) && checker_moving(a, frame) && other_moving(b) && mvm_contact(a, b);
        case CollisionKind::MovingVsStopped:
            return !drives_through(a, b) && checker_moving(a, frame) && !other_moving(b) && mvs_contact(a, b) >= 0.0f;
        case CollisionKind::StoppedVsStopped: return true;  // (ends by its state: collision_valid)
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
        // §4.3-4.6: air units take no part in land collision; FIGHTER_FLYING §7.5 item 1: a squad member is never a
        // collision checker (it is not registered)
        if (!land_layer_object(a)) continue;
        if (a.check_countdown > 0) {
            --a.check_countdown;
            continue;
        }
        a.check_countdown = static_cast<int32_t>(k.frames_per_collision_check) - 1;
        ++stats_.collision_checks;
        land_candidates(ai, cand);
        // s78n t20 (MOVEMENT §5.2 item 7): the pair is classified from "Moving" alone (unit_moving: no order test) - a
        // unit on a pushed path, a StoppedVsStopped path or a detour is a checker and the moving side
        if (unit_moving(a)) {
            for (uint32_t bi : cand) {
                const Unit& b = units_[bi];
                // (the candidates are layer objects: FIGHTER_FLYING §7.8, a squad member is never one)
                if (!collision_classes_interact(a.d.collision_class, b.d.collision_class)) continue;
                if (coll_.has_pair(a.d.id, b.d.id)) continue;
                CollisionKind kind = CollisionKind::MovingVsMoving;
                if (unit_moving(b)) {
                    // [port] until the retail states of pairs that are not two Infantry / Bike movers are ported
                    // (MOVEMENT §5.4.6): such a pair gets its stand-in record only under the older predicates (both walk
                    // an order path, the checker no pushed one), and a crusher takes none with a single unit it crushes
                    if (!mvm_infantry_pair(a, b) &&
                        (drives_through(a, b) || a.pushed || !checker_moving(a, frame) || !other_moving(b)))
                        continue;
                    if (!mvm_contact(a, b)) continue;
                    kind = CollisionKind::MovingVsMoving;
                } else {
                    // s78n t20 (MOVEMENT §5.4.5 item 4): a crusher takes a record with a standing unit it crushes like any
                    // pair (the detection has no crush test; the look-ahead adds 1.5 s when the victim scatters). The
                    // decision pushes a scattering victim once (a crush-flee path), then - or otherwise - the crusher
                    // drives on at full speed (state 4); the crush pass (§5.5) kills what it runs over.
                    if (mvs_contact(a, b) < 0.0f) continue;
                    kind = CollisionKind::MovingVsStopped;
                }
                coll_.add(kind, a.d.id, b.d.id, frame);
                ++stats_.collisions_created;
            }
        } else {
            for (uint32_t bi : cand) {
                const Unit& b = units_[bi];
                // a moving candidate is skipped: its own iteration finds the pair
                if (unit_moving(b) || !collision_classes_interact(a.d.collision_class, b.d.collision_class)) continue;
                // s78n t18 (MOVEMENT §5.3.1 item 1, 0x686500): the overlap coefficient of the two footprint rectangles
                if (!svs_overlap(a, b)) continue;
                if (coll_.has_pair(a.d.id, b.d.id)) continue;
                coll_.add(CollisionKind::StoppedVsStopped, a.d.id, b.d.id, frame);
                ++stats_.collisions_created;
            }
        }
    }
}

// Driver 0x50a780 (MOVEMENT §5.4.1): every record is serviced every frame. Validity first: a record whose participants
// are gone or left the layer is dropped. Then (s78n t20) the records that do not take the quota pass - the decided
// MovingVsMoving ones and every StoppedVsStopped - are serviced in creation order; then the others in priority order, each
// record's state service - a state-None record is decided only while the dynamic pathfinder's quota allows, else it
// waits. `rng` = the battle's synced stream ([port] no draw yet: the MovingVsMoving retry draws of §5.4.6 are not ported). Last, the
// records' speed answers (vf +0x40, 0x509ea0) are folded per unit in list order: the lowest speed, and the "at once" flag
// of the last record that writes one (MovingVsStopped, and MovingVsMoving of two Infantry / Bike movers).
void MovementSystem::drive_collisions(uint32_t frame, SimRng& rng) {
    // [port] The MovingVsMoving stand-in of the other pairs re-applies its speed slaving on each unit's check frames.
    const int32_t check_phase = static_cast<int32_t>(cfg_.constants.frames_per_collision_check) - 1;
    for (Unit& u : units_) {
        if (u.check_countdown == check_phase) u.speed_cap = kInf;
        u.coll_speed = kInf;
        u.coll_now = false;
    }
    const float half = 0.5f * kCollisionCrowdBox;
    std::vector<uint32_t> crowd;
    coll_.run(
        frame, [&](CollisionObject& c) { return collision_valid(c, frame); },
        // vf +0x50 "takes the quota pass": MovingVsStopped always, MovingVsMoving only undecided (the stand-in's records
        // stay undecided), StoppedVsStopped never
        [](const CollisionObject& c) {
            return c.kind == CollisionKind::MovingVsStopped ||
                   (c.kind == CollisionKind::MovingVsMoving && c.state == CollisionState::Active);
        },
        [&](const CollisionObject& c) {
            // 5 x the land object-grid objects in the 160 x 160 box on the participants' XY midpoint (s78n t18: the
            // layer-0x10 box query - a squad counts once, as its container; both participants count). The keep pass
            // ran first, so both participants exist.
            const V2 pa = units_[static_cast<size_t>(find(c.a))].m.pos, pb = units_[static_cast<size_t>(find(c.b))].m.pos;
            layer_box_query((pa + pb) * 0.5f, half, crowd);
            return static_cast<uint32_t>(crowd.size());
        },
        [&] { return quota_used_ < cfg_.constants.dynamic_land_complexity_quota; },
        [&](CollisionObject& c, bool full) { resolve_collision(c, full, frame, rng); });
    stats_.collisions_cancelled += coll_.last_cancelled();
    stats_.collisions_resolved += coll_.last_resolved();
    for (const CollisionObject& c : coll_.list()) {
        Unit* a = unit_ptr(c.a);
        Unit* b = unit_ptr(c.b);
        if (!a || !b) continue;
        if (c.kind != CollisionKind::MovingVsStopped && !(c.kind == CollisionKind::MovingVsMoving && mvm_infantry_pair(*a, *b)))
            continue;  // StoppedVsStopped (and the stand-in): max speed, flag untouched
        a->coll_speed = std::min(a->coll_speed, c.speed_a);
        a->coll_now = c.now_a;
        b->coll_speed = std::min(b->coll_speed, c.speed_b);
        b->coll_now = c.now_b;
    }
}

// [port] MovingVsMoving stand-in for pairs that are not two Infantry / Bike movers (the retail states of such pairs,
// chooser 0x67f7b0 step 8, are read only once [M]): the response of participant x to y - speed slave behind a slower mover
// ahead, side-step on a head-on course, otherwise the higher id yields. s78n t18: a squad container never side-steps (no
// LandTeamContainer code leaves its path).
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
        if (!x.d.team_container) {
            x.nudge = formation_right(dir_x) * std::min(1.0f, 0.5f * x.lp.max_speed);  // side-step
            x.nudge_frames = k.frames_per_collision_check;
        }
    } else if (x.d.id > y.d.id && ahead) {
        x.hold_until = frame + k.frames_per_collision_check;  // yield to the lower id
    }
}

void MovementSystem::resolve_collision(CollisionObject& c, bool full, uint32_t frame, SimRng& rng) {
    const MovementConstants& k = cfg_.constants;
    Unit& a = units_[static_cast<size_t>(find(c.a))];
    Unit& b = units_[static_cast<size_t>(find(c.b))];
    c.speed_a = c.speed_b = kInf;  // each service answers anew (0x67f080): max speed, no flag
    c.now_a = c.now_b = false;
    // check_countdown value right after a unit's collision check this frame (detect_collisions).
    const int32_t check_phase = static_cast<int32_t>(k.frames_per_collision_check) - 1;
    switch (c.kind) {
        case CollisionKind::MovingVsMoving: {
            // s78n t14 part B (FIGHTER_FLYING §7.1, 0x683140): a crusher and a moving squad container it crushes - the
            // container gets the collision path away from the crusher (push_aside), crush-flee (kind 7) while its owner's
            // faction Scatters_From_Crushers and its SPREAD_OUT is off, else kind 4; the crusher keeps going. [port] either
            // participant order (the port's check order is ascending id), on the crusher's collision-check frames; the
            // push stands for the 0x683140 collision path (direction and spot of 0x6859a0).
            Unit* cr = b.d.team_container && crushes(a, b) ? &a : a.d.team_container && crushes(b, a) ? &b : nullptr;
            if (cr) {
                Unit& sq = cr == &a ? b : a;
                if (cr->check_countdown == check_phase) (void)push_aside(c, *cr, sq, scatters(sq), frame);
                return;
            }
            if (mvm_infantry_pair(a, b)) {  // s78n t18 (MOVEMENT §5.4.2): the retail states
                mvm_service(c, a, b, full, frame, rng);
                return;
            }
            // [port] one object per pair: each participant responds from its own side (its own contact test) on its
            // own collision-check frames; the speed cap / hold / side-step it sets lasts until its next check, as the
            // pre-layer per-unit checks did. Full and fallback are the same (no dynamic pathfinder use).
            if (a.check_countdown == check_phase) mvm_response(a, b, frame);
            if (b.check_countdown == check_phase && !crushes(b, a) && checker_moving(b, frame) && other_moving(a) &&
                mvm_contact(b, a))
                mvm_response(b, a, frame);
            return;
        }
        case CollisionKind::MovingVsStopped:
            mvs_service(c, a, b, full, frame);  // s78n t18 (MOVEMENT §5.4.1)
            return;
        case CollisionKind::StoppedVsStopped:
            svs_service(c, a, b, frame);  // s78n t20 (MOVEMENT §5.4.3)
            return;
    }
}

// ---- s78n t18 (MOVEMENT §5.4.1 / §5.4.2): record life, the MovingVsStopped decision and the MovingVsMoving states ----

// A record is kept while both participants are layer objects; a MovingVsStopped or StoppedVsStopped record then ends only
// by its state, a MovingVsMoving record of two Infantry / Bike movers also the frame either has neither speed nor path
// (validity 0x680650). The MovingVsMoving records of the other pairs ([port] a stand-in) keep the older test: the
// classification still holds.
bool MovementSystem::collision_valid(const CollisionObject& c, uint32_t frame) const {
    const int32_t ia = find(c.a), ib = find(c.b);
    if (ia < 0 || ib < 0) return false;
    const Unit& a = units_[static_cast<size_t>(ia)];
    const Unit& b = units_[static_cast<size_t>(ib)];
    if (!land_layer_object(a) || !land_layer_object(b)) return false;
    if (c.kind == CollisionKind::MovingVsStopped || c.kind == CollisionKind::StoppedVsStopped) return true;  // by their states
    if (c.kind == CollisionKind::MovingVsMoving && mvm_infantry_pair(a, b)) return unit_moving(a) && unit_moving(b);
    return collision_applies(c, frame);
}

bool MovementSystem::mvm_infantry_pair(const Unit& a, const Unit& b) const {
    const auto inf = [](const Unit& u) { return u.d.collision_class == kUccInfantry || u.d.collision_class == kUccBike; };
    return inf(a) && inf(b);
}

bool MovementSystem::unit_moving(const Unit& u) const {
    // vf +0x1a8 0x42cc60: not suspended (pack +0x260; the port's speed factor 0) and (speed > 0 or a path record on its
    // stack); vf +0x1b0 0x42cd30: not suspended and turning in place (the start turn, the arrival turn, a container's
    // final-facing turn)
    if (!(u.speed_factor > 0.0f)) return false;
    return u.m.speed > 0.0f || u.pushed || (u.has_goal && !u.need_path) || u.m.state == LocoState::WalkStartTurn ||
           u.aturn != LocoState::None || u.fturn;
}

// ---- s78n t20 (MOVEMENT §1.5.1 item 5): the collision-owned record on top of the path stack ----
namespace {
uint8_t record_weight(CollisionKind k) {  // vf +0x30
    return k == CollisionKind::MovingVsMoving ? uint8_t{1} : k == CollisionKind::MovingVsStopped ? uint8_t{2} : uint8_t{3};
}
}  // namespace

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
    u.nudge_frames = 0;
    u.path_change = frame;  // s78n t18: pack +0x288 (a path record is pushed on its stack)
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

bool MovementSystem::pop_top(Unit& u, bool walked, uint32_t frame) {
    const uint8_t kind = u.push_kind;
    const uint32_t resume = u.push_resume;
    end_push(u);
    u.around.clear();
    u.path_change = frame;  // s78n t18: the record is popped (pack +0x288)
    LandMotion& m = u.m;
    const size_t n = m.path.size();
    if (!walked || !u.has_goal || u.need_path || n < 2 || m.next >= n) return false;
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

float MovementSystem::half_width(const Unit& u) const { return foot_half(u).y; }

bool MovementSystem::set_collision_info(ObjectId id, const CollisionInfo& info) {
    Unit* u = unit_ptr(id);
    if (!u) return false;
    u->ci = info;
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
        if (c.a == u.d.id) cap = std::min(cap, c.speed_a);
        if (c.b == u.d.id) cap = std::min(cap, c.speed_b);
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
    if (std::fabs(dot(fx, fy)) >= 0.9848f && std::fabs(cross(fx, rel)) >= foot_half(x).y + foot_half(y).y) return vmax;
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
    const float dec = x.d.team_container ? (rate > 0.0f ? rate : vmax) : x.lp.decel;
    if (follow < v && dec > 0.0f) tau = std::max(0.0f, tau - (v - follow) / dec);
    const float k = std::clamp(tau / (30.0f * cfg_.constants.moving_vs_moving_look_ahead_time), 0.0f, 1.0f);
    if (cy > 0.0f) return gap <= 0.0f ? 0.0f : follow + (vmax - follow) * k;  // y moves away along the line
    return vmin + (vmax - vmin) * k;
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
    // ([port] a mover on a collision-owned top record never counts as blocked: no cut of such a record)
    if (a.pushed || !a.has_goal || a.need_path || a.m.next >= a.m.path.size() || !near_path_end(a, b)) return false;
    const V2 e = a.m.path.back();
    const FootRect ra = foot_at(a, e);
    if (foot_overlap(ra, foot_of(b))) return true;
    std::vector<uint32_t> cand;
    layer_box_query(e, 0.5f * kCollisionCrowdBox, cand);
    for (uint32_t ci : cand) {
        const Unit& o = units_[ci];
        if (o.d.id == a.d.id || o.d.id == b.d.id || unit_moving(o)) continue;
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

// 0x54e780 on a unit's order path (the kind-1 record): its length is reduced by c from its end. A remaining length <= 0:
// [port] the path ends at the unit's position (it arrives where it stands).
void MovementSystem::cut_order_path(Unit& u, float c, uint32_t frame) {
    LandMotion& m = u.m;
    if (!u.has_goal || u.need_path || m.next == 0 || m.next >= m.path.size()) return;
    // the record's remaining points from where the unit is on it (under a collision-owned top record: its current waypoint)
    const V2 from = u.pushed ? m.path[m.next - 1] : m.pos;
    float total = distance(from, m.path[m.next]);
    for (size_t i = m.next + 1; i < m.path.size(); ++i) total += distance(m.path[i - 1], m.path[i]);
    const float keep = total - c;
    V2 stop = from;
    size_t seg = m.next;
    if (keep > 0.0f) {
        float acc = 0.0f;
        V2 at = from;
        for (size_t i = m.next; i < m.path.size(); ++i) {
            const float l = distance(at, m.path[i]);
            if (acc + l >= keep) {
                seg = i;
                stop = l > 0.0f ? lerp(at, m.path[i], std::clamp((keep - acc) / l, 0.0f, 1.0f)) : at;
                break;
            }
            acc += l;
            at = m.path[i];
        }
    }
    m.path.resize(seg);
    m.path.push_back(stop);
    if (u.pushed && u.push_kind == 2 && u.push_resume > seg) u.push_resume = static_cast<uint32_t>(seg);
    u.goal = stop;
    u.path_change = frame;
}

// Early arrival 0x685e10 with the cut 0x684100 (MOVEMENT §5.4.4; called on the mover's final leg with a blocked
// destination). A = the mover's path stack from its position (length L, end E). Obstacles: the layer objects whose box
// meets the 160 x 160 square on E, plus b, without every Moving one (the mover itself drops out as Moving). The box: the
// mover TYPE's half extents 0x371be0 (a squad container: (0, 0), not its joint slot box) with hx + 5, turned along the path.
// Scan s = L, L - 5, ..., the last test at s = 0 (its own position): the first point P whose box overlaps no listed
// footprint (touching counts) and, with a target, lies within the attack range (+ the target's radius) of it. With a
// spare point v: P farther from E than v -> fail, nothing changed. Else the path is cut by c = (L - s) + 3.0, so it ends
// 3.0 before P; on a formation path the order paths of the formation's members whose slot is behind a's and within the
// two half widths sideways are cut by the same c. True iff b is at or ahead of P: (b - P) . n >= -0.001. [port] the cut
// applies to the order path (destination_blocked refuses a mover on a collision-owned top record); the target's radius is
// added whenever the host reports a target (the weapon-list test is [M]).
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
    if (!unit_moving(b)) blockers.push_back(foot_of(b));
    for (uint32_t ci : cand) {
        const Unit& o = units_[ci];
        if (o.d.id == a.d.id || o.d.id == b.d.id || unit_moving(o)) continue;
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
            const float ax = a.form_slot.x, ay = a.form_slot.y, hwa = half_width(a);
            if (a.form_id != 0) {
                for (Unit& m : units_) {
                    if (m.form_id != a.form_id || m.d.id == a.d.id) continue;
                    if (!(m.form_slot.x < ax) || std::fabs(m.form_slot.y - ay) > half_width(m) + hwa) continue;
                    cut_order_path(m, c, frame);
                }
            }
            cut_order_path(a, c, frame);
            return dot(b.m.pos - p, n) >= -0.001f;
        }
        if (s == 0.0f) return false;  // even its own position fails: nothing changed
    }
}

// ---- s78n t20 (MOVEMENT §1.5.1): the Path Around call of the dynamic land pathfinder ----

bool MovementSystem::shares_open_record(const Unit& u, const Unit& o) const {
    // record vf +0x58: MovingVsStopped state 0 / 1, MovingVsMoving state 0 / 1 (/ 4 Pass: not ported yet)
    for (const CollisionObject& c : coll_.list()) {
        if (!((c.a == u.d.id && c.b == o.d.id) || (c.a == o.d.id && c.b == u.d.id))) continue;
        if (c.kind == CollisionKind::StoppedVsStopped) return false;
        return c.state == CollisionState::Active || c.state == CollisionState::PathAround;
    }
    return false;
}

uint8_t MovementSystem::unit_relation(const Unit& p0, const Unit& p1) const {
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
        in.own_record = c.kind == CollisionKind::MovingVsMoving && (c.state == CollisionState::Active || c.state == CollisionState::PathAround);
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
// not slower -> ignored, unless it is on its final leg, not behind u's facing and the pair has such a record.
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
        const bool is_forced = forced && forced->d.id == o.d.id;
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

bool MovementSystem::around_base(const Unit& u, std::vector<V2>& scratch, std::span<const V2>& base, uint32_t& cur) const {
    if (!u.has_goal || u.need_path || u.m.next == 0 || u.m.next >= u.m.path.size()) return false;  // a kind-1 record
    if (!u.pushed) {
        base = u.m.path;
        cur = u.m.next - 1;
    } else {
        if (u.push_next == 0 || u.push_path.empty()) return false;
        scratch = u.push_path;
        const V2 dir = normalize_retail(u.m.path[u.m.next] - u.m.path[u.m.next - 1]);
        scratch.push_back(scratch.back() + dir);  // 1 unit past its end along the order path's current segment
        base = scratch;
        cur = u.push_next - 1;
    }
    return static_cast<size_t>(cur) + 1 < base.size();
}

// 0x679aa0: the candidate is taken at the nearest passable point Q of the unit's region within LandDestinationProximity
// (60), when the unit's rectangle placed at Q, axis along Q - E (its facing when zero), overlaps no layer object.
bool MovementSystem::rejoin_accept(const Unit& u, V2 cand, V2 path_end, V2& q) const {
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
        if (o.d.id != u.d.id && foot_overlap(r, foot_of(o))) return false;
    }
    return true;
}

bool MovementSystem::rejoin_search(const Unit& u, std::span<const V2> base, uint32_t cur, float d,
                                   const std::function<bool(const Rejoin&)>& fn) const {
    constexpr uint32_t kMaxTried = 11;  // 0x675ed0
    uint32_t tried = 0;
    const V2 end = base.back();
    // (a) along the base path, from the unit's progress + D + 2.0 in steps of 10
    std::vector<RejoinPoint> pts;
    rejoin_on_path(base, cur, u.m.pos, d, kMaxTried, pts);
    for (const RejoinPoint& p : pts) {
        if (tried++ >= kMaxTried) return false;
        Rejoin r;
        r.on_path = !u.pushed;
        r.resume = u.pushed ? u.m.next : p.next;  // (a detour off an older collision path resumes the order path by its lines)
        if (rejoin_accept(u, p.p, end, r.q) && fn(r)) return true;
    }
    // A spot past the path end needs a straight passable line from the end, or a land path no longer than 2 x the line.
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
    // LandTemporaryDestinationProximity (20), in steps of 10
    V2 dir = normalize_retail(base[base.size() - 1] - base[base.size() - 2]);
    float limit = cfg_.constants.land_temporary_destination_proximity;
    if (u.ci.target >= 0 && length_sq(u.ci.target_pos - end) > 0.0f) {
        dir = normalize_retail(u.ci.target_pos - end);
        limit = distance(u.ci.target_pos, end);
    }
    if (!(length_sq(dir) > 0.0f)) dir = dir_from_deg(u.m.yaw);
    const uint32_t past = u.pushed ? u.m.next : static_cast<uint32_t>(u.m.path.size());
    for (float s = 10.0f; s <= limit; s += 10.0f) {
        if (tried++ >= kMaxTried) return false;
        const V2 spot = end + dir * s;
        Rejoin r;
        r.resume = past;
        if (grid_.passable_world(spot, u.lp.mask) && reachable(spot) && rejoin_accept(u, spot, end, r.q) && fn(r)) return true;
    }
    // (c) the ring search 0x440f90 around E (full circle from dir; occupancy margin max(6 x FormationSpacing, 0.25 r_u)):
    // the first free reachable spot not behind E along dir
    if (tried++ >= kMaxTried) return false;
    const float ru = foot_radius(u);
    const float reach = ru + std::max(6.0f * u.d.formation_spacing, 0.25f * ru);
    const int32_t comp = regions_.component_at(u.slot, world_to_cell(u.m.pos.x), world_to_cell(u.m.pos.y));
    std::vector<uint32_t> occ;
    RingSetup ring;
    ring.centre = end;
    ring.dir = dir;
    ring.r = ru;
    V2 spot;
    const bool found = ring_search(
        ring,
        [&](V2 p, uint32_t) {
            const int32_t cx = world_to_cell(p.x), cy = world_to_cell(p.y);
            if (!grid_.in_range(cx, cy) || !grid_.passable_cell(cx, cy, u.lp.mask)) return false;
            if (comp >= 0 && regions_.component_at(u.slot, cx, cy) != comp) return false;
            if (dot(p - end, dir) < 0.0f) return false;
            layer_box_query(p, reach + 1.0f, occ);
            for (uint32_t ci : occ)
                if (units_[ci].d.id != u.d.id && ring_blocked(p, reach, foot_of(units_[ci]))) return false;
            return reachable(p);
        },
        spot);
    if (!found) return false;
    Rejoin r;
    r.resume = past;
    return rejoin_accept(u, spot, end, r.q) && fn(r);
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
        return true;
    });
}

// Path Around 0x5c80e0 (MOVEMENT §1.5.1). The detour goes on top of the unit's path stack; the ordered path stays
// underneath, untouched, and resumes at the first waypoint line not yet crossed when the detour ends (pop_top). The
// complexity (the vertex count of every A* pass) goes to the quota counter. [port] not modelled: the escape vertices of
// a start inside a same-way obstacle, the formation-path refusal 0x41cf40, concave terrain corners and the corner
// pruning masks; "crosses an obstacle" = the segment meets the interior of its grown rectangle; kind 3 (TerrainCollision)
// has no caller.
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
    // The obstacle set, once for the call: every layer object the largest window can reach (the rejoin point lies within
    // D + the candidates' reach of the unit; the window adds its bound, its growth and the retries)
    std::vector<AroundObstacle> obs;
    std::vector<uint32_t> obs_units;
    around_obstacles(u, flag ? o : nullptr, flags, d + 160.0f + 40.0f * kCellSize, obs, obs_units);
    const V2 hu = foot_half(u);
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
        if (!ok || res.points.size() < 2) return false;  // the next candidate
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
        return true;
    });
    if (searched) ++stats_.dynamic_repaths;
    return done;
}

bool MovementSystem::mvs_path_around(const CollisionObject& c, Unit& a, const Unit& b, uint8_t p, uint32_t frame) {
    return path_around(c, a, &b, false, 4, p, frame) || path_around(c, a, &b, false, 4, static_cast<uint8_t>(p | 4u), frame);
}

// MovingVsStopped decision 0x684f90 (MOVEMENT §5.4.1; a = mover, b = obstacle), first match wins. Not modelled ([port]):
// the death-clone obstacle branch (no such movement unit yet); step 10's units-ignoring detour 0x685490 (state 8) is a
// stand-in (see there). s78n t20: the spare point 0x5c6ba0 and the Path Around calls are §1.5.1's; §5.4.5 - the target
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
    // 2. a forced top path (flag 0x20: garrison exit, unload; a collision-owned record on top is not forced)
    if (a.forced && !a.pushed && a.has_goal) {
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
    if (ca == kUccGiantVehicle && !crush && (inf_b || cb == kUccBike || a.m.state == LocoState::WalkStartTurn)) {
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
    // a bike against infantry, and never a blocker in a hero clash (+0x3b4; only this step tests it)
    const bool friendly = !hostile(a.d.owner, b.d.owner) || scat;
    const bool shove_ok = friendly && !(inf_a && !a.d.team_container) && !(ca == kUccBike && inf_b);
    if (shove_ok && (!crush || scat) && !b.ci.hero_clash && !push_tried && push_aside(c, a, b, crush, frame)) {
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
        if (!full) {  // a state-None record waits for the dynamic quota; it answers max speed meanwhile
            ++stats_.quota_deferrals;
            return;
        }
        mvs_decide(c, a, b, frame);
    }
    switch (c.state) {
        case S::PathAround:  // a no longer holds the path made for this record (vf +0x1c0(record id))
            if (!holds_path(a, c)) c.state = S::Resolved;
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

// Chooser 0x67f7b0 for two Infantry / Bike movers (MOVEMENT §5.4.2). [port] not modelled: the same-formation branch
// (states 11 / 13 / Pass; the port keeps no formation identity), the starvation states 15 / 16 and the (1, 1) flags of
// an undecided record.
CollisionState MovementSystem::mvm_choose(const CollisionObject& c, const Unit& a, const Unit& b, bool& master_a) const {
    using S = CollisionState;
    master_a = false;
    if (!mvm_contact(a, b)) return S::Resolved;
    const bool fa = a.forced && !a.pushed && a.has_goal, fb = b.forced && !b.pushed && b.has_goal;
    if (fa != fb) {  // exactly one on a forced (top) path: it is the master, the other one path-arounds
        master_a = fa;
        return S::PathAround;
    }
    const V2 f0 = dir_from_deg(a.m.yaw), f1 = dir_from_deg(b.m.yaw);
    const float ahead0 = dot(b.m.pos - a.m.pos, f0), ahead1 = dot(a.m.pos - b.m.pos, f1);
    // Relation code 0x679750(p0, p1): 2 when p0 is held by its records; headings within 60 degrees -> 0x10 / 0x20; else
    // 4 / 8 at the decision, and in state 2 only once p1 is at least its half width ahead of p0 (else 0x10).
    bool slave = false;
    if (record_cap(a) != 0.0f) {
        const V2 h0 = wanted_dir(a), h1 = wanted_dir(b);
        if (dot(h0, h1) > 0.5f) slave = true;
        else if (c.state != S::Active) slave = !(dot(b.m.pos - a.m.pos, h0) >= foot_half(b).y);
    }
    if (!slave) return S::Repulsion;  // both at max speed, vectors for both sides
    master_a = ahead0 < ahead1;       // the one farther ahead
    return S::SpeedSlave;
}

// MovingVsMoving states 0x6814f0 for two Infantry / Bike movers.
void MovementSystem::mvm_service(CollisionObject& c, Unit& a, Unit& b, bool full, uint32_t frame, SimRng& rng) {
    using S = CollisionState;
    (void)rng;  // [port] no draw yet (the retry draws of MOVEMENT §5.4.6 come with the states of the other pairs)
    if (c.state == S::Active) {
        if (!full) {
            ++stats_.quota_deferrals;
            return;
        }
        bool ma = false;
        const S s = mvm_choose(c, a, b, ma);
        c.master = ma ? 0 : 1;
        if (s == S::PathAround) {  // mode 2 (0x683140): the slave path-arounds the master; failure -> state 7
            Unit& sl = ma ? b : a;
            const Unit& ms = ma ? a : b;
            // (kind 7, a crush-flee path, when the master can crush the slave and the slave scatters)
            c.state = path_around(c, sl, &ms, false, crushes(ms, sl) && scatters(sl) ? uint8_t{7} : uint8_t{4}, 0, frame)
                          ? S::PathAround
                          : S::Crusher;
        } else {
            c.state = s;
        }
    }
    const Unit& m = c.master == 0 ? a : b;
    const Unit& s = c.master == 0 ? b : a;
    switch (c.state) {
        case S::SpeedSlave: {  // state 2: the chooser must give it again; the slave follows by the slave-speed rule
            bool ma = false;
            if (mvm_choose(c, a, b, ma) != S::SpeedSlave) {
                c.state = S::Active;
                break;
            }
            c.master = ma ? 0 : 1;
            const Unit& mm = ma ? a : b;
            const Unit& ss = ma ? b : a;
            bool touch = false;
            const float v = slave_speed(ss, mm, touch);
            (ma ? c.speed_b : c.speed_a) = v;
            (ma ? c.now_b : c.now_a) = touch || v == 0.0f;
            if (mvm_passed(a, b)) c.state = S::Resolved;
            break;
        }
        case S::Repulsion:  // state 3 Repulse: both at max speed, no hold; ends when one is behind the other
            if (mvm_passed(a, b)) c.state = S::Resolved;
            break;
        case S::PathAround: {  // state 1: the slave walks its detour, the master follows by the slave-speed rule
            if (!holds_path(s, c)) {
                c.state = S::Resolved;
                break;
            }
            if (!crushes(m, s)) {
                bool touch = false;
                (c.master == 0 ? c.speed_a : c.speed_b) = slave_speed(m, s, touch);
                (c.master == 0 ? c.now_a : c.now_b) = touch;
            }
            break;
        }
        default: break;  // Crusher (state 7): both at max speed, validity only
    }
}

// Push 0x6859a0 (A1_READINGS §36.4, §40): direction = a's heading turned 45 degrees toward b's side; the free-position
// search (push_spot) and a land path to the spot; b walks it as a pushed move over its order move (§40.3: no return
// move). A deploying / deployed b is not pushed. [port] A b already on a pushed move counts as pushed (the original
// pushes a blocker that is moving again, decision 0x684f90 step 2).
bool MovementSystem::push_aside(const CollisionObject& c, const Unit& a, Unit& b, bool flee, uint32_t frame) {
    // [port] a unit still walking the path this record pushed it on keeps it, path kind (crush-flee or not) included.
    // (s78n t20: a push by another record replaces the older collision path - the claim is forced, A1_READINGS §40.3.)
    if (holds_path(b, c)) return true;
    if (b.anchor >= 0) return false;  // s78n t03 [port]: a steering member is not pushed (it steers back to its slot)
    if (!(b.lp.max_speed > 0.0f) || b.deploy == LocoState::WalkDeploying || b.deploy == LocoState::WalkDeployed) {
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
// deployed / deploying units never move (both: nobody); exactly one has a target -> the one without; two Infantry-class
// units with targets -> nobody; against a Giant of a higher class only the smaller one is a candidate; else both are,
// the cheaper first (cost(p1) <= cost(p0) -> p1 first). Of the candidates the first whose locomotor would accept the
// claim moves (no owner, this record, or an owner of lower weight). [port] no movement unit is without a locomotor
// (death clones: not registered yet); a deployed unit never walks (Max_Dist_Walk_While_Deployed is not modelled), and a
// unit whose max speed is 0 counts as immobile too (retail has no such case: it would pick it and wait for good).
uint8_t MovementSystem::svs_pick_mover(const CollisionObject& c, const Unit& p0, const Unit& p1) const {
    if (p0.d.collision_class == kUccLandingTransport) return 1;
    if (p1.d.collision_class == kUccLandingTransport) return 0;
    if (p0.hover == 3) return 1;
    if (p1.hover == 3) return 0;
    const auto imm = [this](const Unit& x) {
        return x.deploy == LocoState::WalkDeploying || x.deploy == LocoState::WalkDeployed || !(walk_max_speed(x) > 0.0f);
    };
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
    if (!grid_.passable_world(a.m.pos, a.lp.mask) || !grid_.passable_world(b.m.pos, b.lp.mask)) {
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
        push_top(x, std::move(out.points), 3, false, c, frame);
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
    u.push_blocked = 0;
    u.push_owner = kNoDetour;  // s78n t20: the locomotor gives up the record id (pack +0x248)
    u.push_weight = 0;
    u.push_kind = 0;
    u.push_resume = 0;
    u.push_tags.clear();
}

void MovementSystem::tick_land(uint32_t frame, SimRng& rng, const ParallelFor& pf) {
    rebuild_regions_if_dirty(pf);
    tree_.maintain(static_cast<double>(frame) / 30.0);  // shape only; query results do not depend on it
    plan_land(frame, pf);
    detect_collisions(frame);  // S7: checks create collision objects
    drive_collisions(frame, rng);  // S7: cancel / priority sort / resolve (TACTICAL_GAPS §6.7)
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
        for (const Unit& u : units_) pos0.push_back({u.m.pos, u.z});
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
    pf(static_cast<uint32_t>(units_.size()), 64, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            Unit& u = units_[i];
            if (is_air(u)) continue;  // §4.3-4.6: serial pass below
            if (anchor_at[i] >= 0) {  // s78n t03: a squad member steers to its slot around its container
                steer_member(u, snaps[static_cast<size_t>(anchor_at[i])], recs, pos0);
                continue;
            }
            // s78n t18 (FIGHTER_FLYING §7.7 items 3 - 6): a squad container's own walk - its pushed path (the team speed,
            // x Fleeing_Infantry_Speed_Bonus with the rate and the turn rate on a crush-flee path) or its order path
            if (u.d.team_container) {
                res[i] = team_walk(u, frame);
                continue;
            }
            const bool bike = u.d.locomotor == LocomotorKind::Bike;
            const float yaw0 = u.m.yaw;
            // s78n t18 (MOVEMENT §5.4.1 "How Walk combines the vector"): a Walk unit with collision records adds the
            // steering vector 0x509810 to its heading (start-of-tick positions; its new speed for the 0x5c6d40 ratio).
            struct AvoidCtx {
                const MovementSystem* sys;
                const Unit* u;
                float vmax;
                std::span<const SteerRec> recs;
                std::span<const SteerPos> pos;
            } actx{this, &u, u.lp.max_speed, {}, pos0};
            WalkExtras x;
            if (own[i].second > own[i].first) {
                actx.recs = std::span<const SteerRec>(recs).subspan(own[i].first, own[i].second - own[i].first);
                x.avoid = [](const void* c, V2 dir) -> V2 {
                    const AvoidCtx& k = *static_cast<const AvoidCtx*>(c);
                    return k.sys->member_avoid(*k.u, k.u->m.pos, k.u->m.speed, k.vmax, dir, k.recs, k.pos);
                };
                x.avoid_ctx = &actx;
            }
            x.cap_now = u.coll_now;  // s78n t20 (MOVEMENT §3.5 step 2): the collision cap's "at once" flag
            // a1k t03 (A1_READINGS §40.3): the collision-owned record is the top of the path stack; the order path waits
            // under it. s78n t20 (MOVEMENT §1.5.1 item 5): it is walked as the order path is - the collision records'
            // speed answer, the steering vector, the team cap - without the formation speed, jiggle or side error.
            if (u.pushed && u.deploy == LocoState::None) {
                // a started order path goes on under the record: no braking for the record's end
                if (u.has_goal && !u.need_path && frame >= u.start_frame && !u.push_path.empty()) {
                    size_t from = u.m.next;
                    if (u.push_kind == 2) from = std::max(from, static_cast<size_t>(u.push_resume));
                    if (from < u.m.path.size()) {
                        x.tail = distance(u.push_path.back(), u.m.path[from]);
                        for (size_t p = from + 1; p < u.m.path.size(); ++p) x.tail += distance(u.m.path[p - 1], u.m.path[p]);
                        if (!(x.tail > 0.0f)) x.tail = 1e-3f;
                    }
                }
                std::swap(u.m.path, u.push_path);
                std::swap(u.m.next, u.push_next);
                const uint32_t escape = u.m.escape_until;
                u.m.escape_until = 0;
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
                actx.vmax = plp.max_speed;
                res[i] = walk_step(u.m, plp, u.coll_speed, &grid_, &x);
                u.m.escape_until = escape;
                std::swap(u.m.path, u.push_path);
                std::swap(u.m.next, u.push_next);
                if (bike) bike_frame(u, yaw0, true);
                continue;
            }
            // s78n t18 (MOVEMENT §5.3 "Hover state", Walk service 0x61e930): while rising (2) / lowering (3) the Walk
            // state machine does not run - the height changes by the max speed per frame until Hover_Height / the
            // ground, then the state is raised (1) / grounded (0) and the speed 0. [port] z itself is not written.
            const bool hovers = u.d.hover_height > 0.0f && u.d.locomotor == LocomotorKind::Default;
            if (hovers && u.hover >= 2) {
                const float step = u.lp.max_speed;
                u.hover_off = u.hover == 2 ? u.hover_off + step : u.hover_off - step;
                if (!(step > 0.0f) || (u.hover == 2 ? u.hover_off >= u.d.hover_height : u.hover_off <= 0.0f)) {
                    u.hover_off = u.hover == 2 ? u.d.hover_height : 0.0f;
                    u.hover = u.hover == 2 ? uint8_t{1} : uint8_t{0};
                    u.m.speed = 0.0f;
                }
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
            // s78n t18 (Walk Start Move 0x61ff40): a Hover_Height unit that is not raised rises first; the frame ends
            if (hovers && u.hover != 1) {
                u.hover = 2;
                u.m.speed = 0.0f;
                continue;
            }
            // s78k t03 / s78l t01: a team member walks at the team speed (team_caps) - the container's effective max speed
            // replaces its own (no member factor enters), in a formation or not; a stunned member (factor 0) stays put.
            // s78n t18 (0x509ea0): the collision records' speed answer is a target the walk brakes toward at its rate
            float cap = std::min(std::min(u.formation_speed, u.speed_cap), u.coll_speed);
            LocomotorParams lp = u.lp;
            if (u.team_cap < kInf && u.speed_factor > 0.0f) lp.max_speed = u.team_cap;
            if (frame < u.hold_until) cap = 0.0f;
            if (u.in_formation) {  // OI tactical: formation members (jiggle, side-error steering)
                x.jiggle = u.jiggle;
                x.side_error = true;
            }
            actx.vmax = lp.max_speed;
            res[i] = walk_step(u.m, lp, cap, &grid_, &x);
            if (bike) bike_frame(u, yaw0, true);  // §4.5: states 0x20-0x25 and the lean (writes only u)
        }
    });
    team_relay_pass(frame);  // s78n t14: the squad containers' re-lays (serial, ascending id)
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
        bool popped_arrival = false;
        if (u.pushed) {
            // a1k t03: the pushed path ended -> the move underneath continues (an idle unit stays at the spot).
            // [port] a pushed walk blocked for kReplanAfterBlocked frames ends where it is.
            if (r.blocked) ++stats_.blocked_steps;
            if (r.arrived || (r.blocked && ++u.push_blocked >= kReplanAfterBlocked)) {
                const bool cut = !r.arrived;
                // s78n t20 (MOVEMENT §1.5.1 item 5): popped; the order path resumes past the waypoint lines already
                // crossed, and a unit that is past its last waypoint's line has arrived where it stands
                popped_arrival = pop_top(u, !cut, frame);
                // s78n review: a pushed walk cut short on a unit with no order leaves a stopped locomotor (the walk only
                // stops on its last waypoint): a squad's members then run the recovery decision, and the next order starts
                // with the start turn and the move-start re-lay.
                if (cut && !u.has_goal) {
                    u.m.speed = 0.0f;
                    u.m.state = LocoState::WalkStopped;
                }
                if (popped_arrival) {
                    u.has_goal = false;
                    u.arrived = true;
                    u.exit_host = -1;
                    if (!u.d.team_container) {
                        u.m.speed = 0.0f;
                        u.m.state = LocoState::WalkStopped;
                    }
                    land_arrival_turn(u);
                }
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
            // s78n t18: a squad container's arrival is its own state 0x15 (team_walk)
            const bool close = !u.d.team_container && !r.arrived && !r.moved && u.has_goal && !u.need_path &&
                               frame >= u.start_frame &&
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
                u.around.clear();  // s78n t18: the path is popped
                u.path_change = frame;
                u.exit_host = -1;  // (MOVEMENT §5.3: the walk-out move arrived)
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
        h.u32(u.path_change);  // s78n t18 (in the place of the dropped Unit::waits; 0 in space: the format is unchanged)
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
        if (u.d.team_container) {  // s78n t03: appended only for squad containers and their members
            h.u8(0xC7);
            h.boolean(u.laid_out);  // s78n t14
            h.f32(u.lay_dir.x);
            h.f32(u.lay_dir.y);
            h.boolean(u.spread);
            h.boolean(u.team_target);
            h.boolean(u.coast);  // s78n t18
            h.boolean(u.fturn);
            h.boolean(u.path_started);
        }
        if (u.anchor >= 0) {
            h.i32(u.anchor);
            h.f32(u.tslot.x);
            h.f32(u.tslot.y);
            h.u32(u.tindex);
            h.u8(u.recovery);  // s78n t14: the recovery state (was a bool)
            h.u32(u.start_delay);  // s78n t14
        }
        if (u.exit_host >= 0) {  // s78n t18: the garrison-host gate of a unit that left, only while set
            h.u8(0xE7);
            h.i32(u.exit_host);
        }
        if (u.hover != 0 || u.hover_off != 0.0f) {  // s78n t18: the hover state, only once it left the ground
            h.u8(0xE8);
            h.u8(u.hover);
            h.f32(u.hover_off);
        }
        if (!u.around.empty()) {  // s78n t18: the top path's id list (decision step 3), only while any
            h.u8(0xD7);
            h.u32(u.around_frame);
            h.u32(static_cast<uint32_t>(u.around.size()));
            for (const ObjectId id : u.around) h.i32(id);
        }
        if (u.form_id != 0 || u.self_arrive) {  // s78n t20: the formation of the last land order (land only)
            h.u8(0xF7);
            h.u32(u.form_id);
            h.f32(u.form_slot.x);
            h.f32(u.form_slot.y);
            h.boolean(u.self_arrive);
        }
        if (u.pushed) {  // a1k t03: hashed only while a collision-owned record is on top of the stack
            if (u.push_flee) h.u8(0x07);  // s78n t14 part B: a crush-flee path (kind 7)
            h.u32(u.push_next);
            h.u32(u.push_blocked);
            h.u32(static_cast<uint32_t>(u.push_path.size()));
            for (const V2& p : u.push_path) {
                h.f32(p.x);
                h.f32(p.y);
            }
            h.u8(u.push_kind);  // s78n t20: the record's kind, owner, weight, resume index and hold tags
            h.u8(u.push_weight);
            h.u32(u.push_owner);
            h.u32(u.push_resume);
            h.u32(static_cast<uint32_t>(u.push_tags.size()));
            for (const HoldTag& t : u.push_tags) {
                h.u32(t.index);
                h.i32(t.obstacle);
                h.f32(t.pt.x);
                h.f32(t.pt.y);
                h.f32(t.dir.x);
                h.f32(t.dir.y);
            }
        }
    }
    h.u32(static_cast<uint32_t>(crush_.size()));  // OI tactical
    for (const CrushEvent& e : crush_) {
        h.i32(e.crusher);
        h.i32(e.victim);
    }
    if (!parked_.empty()) {  // s78n review: parked squad members (garrisoned), appended only while any exists
        h.u8(0xC8);
        h.u32(static_cast<uint32_t>(parked_.size()));
        for (const LandTeamSlot& p : parked_) {
            h.i32(p.member);
            h.i32(p.container);
            h.u32(p.tindex);
            h.f32(p.slot.x);
            h.f32(p.slot.y);
            h.f32(p.off.x);  // s78n t14
            h.f32(p.off.y);
            h.f32(p.hx);  // s78n t18 review: the absent member's entry data (the joint box and the rate read them)
            h.f32(p.hy);
            h.f32(p.max_speed);
            h.f32(p.decel);
        }
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
    if (u.final_facing == kNoFacing || u.d.locomotor != LocomotorKind::Default || is_air(u) || u.d.team_container) return;
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
    float r = foot_radius(u);
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
        // s78n t03: a squad container (set_team on itself) takes its members' minimum; it is not one of them.
        if (u.team < 0 || u.d.team_container) continue;
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
        const int32_t i = find(order_id(id));  // s78n t03: a steering member's order goes to its container
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

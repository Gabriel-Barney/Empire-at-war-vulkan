// movement_system: part 1 (bo3 t03 split; shared code in movement_system_internal.hpp).
#include "movement_system_internal.hpp"

namespace alamo::tactical::movement {

namespace bo3split {

uint64_t cell_key(int32_t x, int32_t y) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(y)) << 32) | static_cast<uint32_t>(x);
}
}  // namespace bo3split

namespace bo3split {

bool contains_key(const std::vector<uint64_t>& v, uint64_t k) { return std::binary_search(v.begin(), v.end(), k); }
}  // namespace bo3split

namespace bo3split {
void insert_key(std::vector<uint64_t>& v, uint64_t k) {
    auto it = std::lower_bound(v.begin(), v.end(), k);
    if (it == v.end() || *it != k) v.insert(it, k);
}
}  // namespace bo3split

namespace bo3split {

float slowdown_for_layer(const MovementConstants& k, uint32_t layer) {
    if (layer & (kLayerCapital | kLayerSuperCapital)) return k.turn_in_place_slowdown_capital;
    if (layer & kLayerFrigate) return k.turn_in_place_slowdown_frigate;
    if (layer & kLayerCorvette) return k.turn_in_place_slowdown_corvette;
    return 1.0f;
}
}  // namespace bo3split

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
    u.lp.secondary = u.d.use_secondary_facing && u.d.locomotor == LocomotorKind::Default;  // bkm1 t08: a Walk flag
    u.lp.sec_angle = u.d.secondary_angle;
    u.lp.sec_min = u.d.secondary_min;
    u.lp.sec_max = u.d.secondary_max;
    u.lp.sec_lookahead = u.d.secondary_lookahead;
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
    u.m.yaw2 = u.m.yaw;  // bkm1 t08 (MOVEMENT §3.5.3 item 3): the Walk's Initialise copies the orientation into Y2
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
        it->prev_pos = it->m.pos;  // s78n t26.4: no displacement before the first frame
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
    std::erase_if(deploy_kept_, [id](const DeployKeep& k) { return k.id == id; });  // s78n t26.1 review
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
    v.atk_theta = u->atk_theta;  // s78n t25.2
    v.atk_rho = u->atk_rho;
    v.atk_form = u->atk_form;
    v.atk_target = u->atk_target;
    v.goal = u->goal;
    if (u->anchor >= 0) {  // s78n t03: a steering member moves with its container's order (and while recovering)
        if (const Unit* k = unit_ptr(u->anchor)) {
            v.has_goal = k->has_goal || u->recovery != 0;
            v.arrived = k->arrived;
            v.failed = k->failed;
            v.goal = k->goal;
        }
    }
    v.space_fallback = u->fallback;
    v.start_frame = u->start_frame;  // OI tactical
    v.formation_speed = u->formation_speed;
    v.cohesion_speed = u->cohesion_speed;  // [port] fleet cohesion
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
    v.walk_state = u->m.state;  // s78n t20
    v.anim = u->sm.anim.id;
    v.airborne = u->sm.airborne;
    attitude(id, v.roll, v.pitch);
    if (is_fighter(*u)) v.fighter_phase = u->fs.phase;
    v.deploy_state = u->deploy;  // S8 t11
    v.deploy_clip = u->dclip;
    v.walk_deployed = u->walk_deployed;  // s78n t26.1
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
    // s78n t23 (MOVEMENT §5.3 "When the deployed pair is used", 0x371d20): the deployed pair only when its X is set (the Y
    // branch tests the X field too): X set with Y unset gives hy = 0
    if (deployed_extents(u) && u.d.hard_x_deployed > 0.0f) return {u.d.hard_x_deployed, std::max(u.d.hard_y_deployed, 0.0f)};
    if (u.d.hard_x > 0.0f || u.d.hard_y > 0.0f) return {std::max(u.d.hard_x, 0.0f), std::max(u.d.hard_y, 0.0f)};
    return {u.d.radius, u.d.radius};  // [port] no hard extents (the model-box fallback of 0x371be0 is not loaded)
}

bool MovementSystem::foot_boxed(const Unit& u) const {
    return (u.d.team_container && u.team_boxed) || u.d.hard_x > 0.0f || u.d.hard_y > 0.0f ||
           (deployed_extents(u) && u.d.hard_x_deployed > 0.0f);
}

V2 MovementSystem::collision_half(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u ? foot_half(*u) : V2{};
}

float MovementSystem::collision_half_width(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u ? half_width(*u) : 0.0f;
}

float MovementSystem::foot_radius(const Unit& u) const {
    if (foot_boxed(u)) {
        const V2 h = foot_half(u);
        return std::sqrt(h.x * h.x + h.y * h.y);
    }
    return u.d.radius;  // [port] no hard extents: the soft radius (as the push search took it)
}

FootRect MovementSystem::foot_of(const Unit& u) const { return foot_at(u, u.m.pos); }

FootRect MovementSystem::foot_at(const Unit& u, V2 p) const { return foot_at(u, p, u.m.yaw); }

FootRect MovementSystem::foot_at(const Unit& u, V2 p, float yaw) const {
    const V2 h = foot_half(u);
    return foot_boxed(u) ? foot_rect(p, yaw, h.x, h.y) : foot_circle(p, yaw, u.d.radius);  // [port] no hard extents: a circle
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
    // s78n t26.1 (MOVEMENT §3.2.1 item 2, the group move 0x2a7b80 / waypoint queue 0x2a8240): a deployed `Deploys` unit
    // (0x39a390) that is not an auto-deployer (0x39a320) is skipped entirely - not moved, not stopped, its path and orders
    // kept; the rest of the selection moves, and an all-deployed selection gets no move.
    if (cfg_.mode == MoveMode::Land)
        std::erase_if(idx, [this](uint32_t i) { return deployed_d(units_[i]) && !auto_deployer(units_[i]); });
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
    // s78n t25.2 (MOVEMENT §4.3.2 item 8, §4.6.2): a land attack re-plan is not a composition - no new coordinator, no
    // FormationClass ctor draw; its formations persist (Unit::atk_form)
    const bool atk_replan = cfg_.mode == MoveMode::Land && order.attack.target >= 0 && order.attack.replan;
    if (!queued && !fform_internal_ && !atk_replan) {
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
        // s78n t26 review round 2 (MOVEMENT §4.3 correction [H]): the layout's rank key, the attack range 0x3749d0
        f.attack_range = attack_range_fn_ ? attack_range_fn_(u.d.id) : 0.0f;
        // bkm1 t04 (b0036, MOVEMENT §4.2 step 2 item 7 test 9 [H]): E = 0x4f6e40 (a move: the attack range; an attack
        // order: the entry's engage, set below), K = min(E, Targeting_Min_Attack_Distance), 0 under a guard block
        f.engage = f.attack_range;
        f.min_engage = min_attack_fn_ ? min_attack_fn_(u.d.id) : 0.0f;
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
    // s78n t25.2 (MOVEMENT §4.3.2): a land attack order (an object goal, block kind 3); `replan` = a re-plan of its
    // formations (no split, no slot test)
    const bool attack = order.attack.target >= 0;
    const bool replan = attack && order.attack.replan;
    for (size_t n = 0; n < idx.size(); ++n) {  // bkm1 t04: E and K of the recruitment's test 9
        if (attack)
            if (const LandAttackUnit* e = attack_entry(order, units_[idx[n]].d.id)) mem[n].engage = e->engage;
        mem[n].min_engage = attack && order.attack.guard ? 0.0f : std::min(mem[n].engage, mem[n].min_engage);
    }
    if (attack && !replan && !order.attack.guard) {
        // s78n t25.2 (MOVEMENT §4.2 item 6 [H]): a seed goes solo when it is already in max range of the target (slot 44),
        // or melee (0x372210), or its 3-D distance to the target d < r_seed (0x3969f0) + E_r + Solo_Attack_Range. The flag
        // is set on every member and read only for the seed (members are not seeds until picked). Otherwise the seed is
        // short-range when d < E_r x Short_Range_Attack_Formation_Coefficient (1.6), and a short-range seed recruits
        // only units of its own exact type (bkm1 t04: §4.2 step 2 item 7 test 3, correcting "recruits nobody").
        for (size_t n = 0; n < idx.size(); ++n) {
            const Unit& u = units_[idx[n]];
            const LandAttackUnit* e = attack_entry(order, u.d.id);
            if (!e) continue;
            const float dx = u.m.pos.x - order.attack.c.x, dy = u.m.pos.y - order.attack.c.y, dz = u.z - order.attack.cz;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            mem[n].solo_attack = e->in_range || e->melee || d < foot_radius(u) + e->engage + k.solo_attack_range;
            mem[n].short_range_attack = !mem[n].solo_attack && d < e->engage * k.short_range_attack_formation_coefficient;
        }
    }
    // s78n t26 review round 2 (MOVEMENT §4.2 step 2 item 4 [H]): a candidate joins a seed with the same locomotor kind
    // (vf +0x240 and vf +0x238 agree; FIGHTER_FLYING §4.10 B: kind 1 = Walk, Bike, LandTeamContainer, LandTeamInfantry,
    // kind 2 = Flying, LandBomber, JetPack, Fighter; vf +0x240 true only for JetPack, whose kind is 2 in pack state 0 or
    // 0x0F - 0x13, MOVEMENT §2 "Layers") when both are zone-connected (0x50ee10) to the destination, each for its own
    // class. Movement masks need not agree: infantry and vehicles share a formation (its class: formation_class_mask).
    // The destination is the seed's zone-validated point (§4.2 step 2 item 2: 0x50f480(P, the seed, its class, 30); P
    // itself with flag 0x20, for an air seed or a seed off passable ground, or when the validation fails) - [port] [M]
    // for an object goal too, whose block then drops the validated point; P is the order point, for an attack the
    // object's position. [port] With no grid (unit tests without terrain) every unit is connected.
    const auto loco_kind = [](const Unit& u) -> int32_t {
        switch (u.d.locomotor) {
            case LocomotorKind::Jetpack: {
                const auto st = static_cast<uint8_t>(u.sm.state);
                return st == 0 || (st >= 0x0F && st <= 0x13) ? 0x102 : 0x101;  // vf +0x240 true, kind 2 / 1
            }
            case LocomotorKind::Flying:
            case LocomotorKind::LandBomber:
            case LocomotorKind::Fighter: return 2;
            default: return 1;
        }
    };
    const V2 order_p = attack ? order.attack.c : order.destination;
    std::vector<V2> seed_dest(idx.size());
    std::vector<uint8_t> seed_dest_set(idx.size(), 0);
    const auto dest_of = [&](uint32_t sd) -> V2 {
        if (!seed_dest_set[sd]) {
            seed_dest_set[sd] = 1;
            seed_dest[sd] = order_p;
            const Unit& su = units_[idx[sd]];
            V2 v;
            if (!(order.flags & kMoveFlagSkipValidation) && !is_air(su) && grid_.passable_world(su.m.pos, su.lp.mask) &&
                zone_validate(regions_, order_p, su.m.pos, su.lp.mask, v))
                seed_dest[sd] = v;
        }
        return seed_dest[sd];
    };
    const bool no_grid = grid_.width() == 0 || grid_.height() == 0;
    const auto compatible = [&](uint32_t a, uint32_t b) {
        const Unit& ua = units_[idx[a]];
        const Unit& ub = units_[idx[b]];
        if (loco_kind(ua) != loco_kind(ub)) return false;
        if (no_grid) return true;
        const V2 d = dest_of(a);
        return zone_connected(regions_, ua.lp.mask, ua.m.pos, d) && zone_connected(regions_, ub.lp.mask, ub.m.pos, d);
    };
    // §4.2 step 2 item 4 [H]: a straight passable line (0x5a4c10) from the nearest member to the candidate, or a path
    // (0x5a2830) no longer than 1.4 x their distance ([port] [M]: the candidate's class, the line from the member)
    LandPathScratch reach_sc;
    const auto reach = [&](uint32_t nm, uint32_t cand) {
        if (no_grid) return true;
        const Unit& um = units_[idx[nm]];
        const Unit& uc = units_[idx[cand]];
        if (line_passable(grid_, um.m.pos, uc.m.pos, uc.lp.mask)) return true;
        LandPathRequest q;
        q.start = uc.m.pos;
        q.goal = um.m.pos;
        q.mask = uc.lp.mask;
        q.snap_goal = false;
        LandPath lp;
        find_land_path(grid_, regions_, q, reach_sc, lp);
        return lp.ok && !(path_length(lp.points) > k.formation_recruit_path_ratio * distance(um.m.pos, uc.m.pos));
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
    // s78n t20 batch 2 (MOVEMENT §4.2 step 2, §4.3.1 item 7): each formation's destination is the ordered point validated
    // against its seed's zone (0x50f480(P, the seed's position, its class, 30)) - a position order without flag 0x20 only
    // (the host's skip_destination_check marks the port's object-goal moves; a queued order has no composition pass).
    // [port] a seed standing on a cell its class cannot enter keeps P (the port keeps its blocked-start escape, as the
    // impassable-cell rule's exemption of batch 1 item 1.3); air units use retail's position finder (not modelled): P.
    // A validation that fails ends the composition pass: this formation and every later one of the order get no move
    // ([port] their units are stopped and report `failed`).
    const bool queued = (order.flags & kMoveFlagQueue) != 0;
    const bool position_order =
        !(order.flags & kMoveFlagSkipValidation) && !order.skip_destination_check && !attack && !queued;  // s78n t25.2: + attack
    // bkm3 t01 (b0076, MOVEMENT §4.2 step 2 "Destination"): an attack order's point P (the target's goal point C, 0x583150)
    // is validated per seed as well - its result is used only for position goals, but a failure still ends the pass: this
    // formation and every later one get no move. Not for a re-plan (no composition pass) or a queued order.
    const bool validate_only = attack && !replan && !(order.flags & kMoveFlagSkipValidation) && !queued;
    bool zone_failed = false;
    std::vector<uint32_t> no_move;  // indices into idx: the units of the formations the failed validation left out
    if (replan) {
        // s78n t25.2 (MOVEMENT §4.3.2 item 8, §4.6.2 [port]): a re-plan keeps the formations of its attack order - the
        // order's units grouped by Unit::atk_form (0: a group of its own) in order position, lead = the first; each keeps
        // its slot (form_slot), the formation its id; no split, no stagger, no coordinator
        std::vector<uint32_t> keys;
        std::vector<std::vector<uint32_t>> groups;
        for (uint32_t n = 0; n < idx.size(); ++n) {
            const uint32_t key = units_[idx[n]].atk_form;
            size_t gi = groups.size();
            if (key != 0)
                for (size_t j = 0; j < keys.size(); ++j)
                    if (keys[j] == key) gi = j;
            if (gi == groups.size()) {
                keys.push_back(key);
                groups.emplace_back();
            }
            groups[gi].push_back(n);
        }
        for (size_t gi = 0; gi < groups.size(); ++gi) {
            LandForm lf;
            lf.members = groups[gi];
            V2 c{};
            float fspeed = kInf;
            for (const uint32_t m : lf.members) {
                c = c + mem[m].pos;
                fspeed = std::min(fspeed, mem[m].max_speed);
                const Unit& u = units_[idx[m]];
                Slot sl;
                sl.member = m;
                if (keys[gi] != 0) {
                    sl.back = -u.form_slot.x;
                    sl.lateral = u.form_slot.y;
                }
                lf.slots.push_back(sl);
            }
            lf.centroid = c * (1.0f / static_cast<float>(lf.members.size()));
            lf.speed = fspeed;
            lf.centre = order.attack.g;
            lf.fwd = normalize_retail(lf.centre - lf.centroid);
            if (length_sq(lf.fwd) == 0.0f) lf.fwd = {1.0f, 0.0f};
            lf.keep_id = keys[gi];
            set_form_mask(idx, lf, lf.centre);  // s78n t26 review round 2 (0x4779e0); bkm1 t03: 0x414720
            forms.push_back(std::move(lf));
        }
    }
    for (const std::vector<uint32_t>& bucket : replan ? std::vector<std::vector<uint32_t>>{} : kmeans_buckets(mem, k.max_formation_area)) {
        for (const std::vector<uint32_t>& f : split_into_formations(mem, bucket, k, compatible, speed_ratio, reach)) {
            V2 centre = order.destination;
            bool fresh = false;
            if ((position_order || validate_only) && !zone_failed) {
                const Unit& seed = units_[idx[f[0]]];
                if (!is_air(seed) && grid_.passable_world(seed.m.pos, seed.lp.mask)) {
                    V2 v;
                    const V2 p = validate_only ? order.attack.c : order.destination;
                    if (!zone_validate(regions_, p, seed.m.pos, seed.lp.mask, v)) {
                        zone_failed = true;
                    } else if (position_order && !(v == order.destination)) {
                        centre = v;
                        fresh = true;
                    }
                }
            }
            if (zone_failed) {
                no_move.insert(no_move.end(), f.begin(), f.end());
                continue;
            }
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
            // s78n t20 batch 2: a fresh block (V != P) carries no formation facing
            V2 fwd = order.facing_deg != kNoFacing && !fresh ? dir_from_deg(order.facing_deg) : normalize_retail(centre - c);
            if (length_sq(fwd) == 0.0f) fwd = {1.0f, 0.0f};
            LandForm lf;
            lf.members = f;
            lf.centroid = c;
            lf.speed = fspeed;
            lf.fwd = fwd;
            lf.slots = layout_formation(mem, f, fwd, k);
            lf.centre = centre;
            lf.fresh = fresh;
            set_form_mask(idx, lf, attack ? order.attack.g : centre);  // s78n t26 review round 2 (0x4779e0); bkm1 t03: 0x414720
            forms.push_back(std::move(lf));
        }
    }
    // s78n t20 batch 2: the units a failed zone validation left without a formation do not move
    if (!no_move.empty()) {
        std::vector<ObjectId> ids;
        for (const uint32_t m : no_move) ids.push_back(units_[idx[m]].d.id);
        stop(ids);
        for (const uint32_t m : no_move) {
            Unit& u = units_[idx[m]];
            u.failed = true;
            u.arrived = false;
            u.in_formation = false;
            u.formation_speed = kInf;
        }
    }
    // s78n t20 (MOVEMENT §4.3.1 item 2): the destination check - not for a forced-path order (flag 0x20), an object goal
    // (the host's skip_destination_check) or a queued order (no composition pass). It never refuses the order: it only
    // moves the centre the formations' slots are laid around.
    if (position_order && !forms.empty()) {
        destination_check(order, idx, forms, group_c);
        for (LandForm& lf : forms) set_form_mask(idx, lf, lf.centre);  // bkm1 t03: 0x414720 runs again at the plan, for the moved goal
    }
    // s78n t26 review round 3 (FIGHTER_FLYING §4.11 G 5 [H]): a re-path of a moving formation on a non-guard block leaves
    // a member that is in range (slots 44 / 45 from where it stands, the host's in_range) and not moving (no goal, speed 0:
    // locomotor vf +0x1c0(0) false) on its own path - no new goal, no path, its slot kept
    std::vector<uint8_t> atk_keep(idx.size(), 0);
    if (replan && order.attack.repath && !order.attack.guard)
        for (size_t n = 0; n < idx.size(); ++n) {
            const Unit& u = units_[idx[n]];
            const LandAttackUnit* e = attack_entry(order, u.d.id);
            atk_keep[n] = e && e->in_range && !u.has_goal && !(u.m.speed > 0.0f) ? 1 : 0;
        }
    {
        for (const LandForm& lf : forms) {
            const std::vector<uint32_t>& f = lf.members;
            const V2 fwd = lf.fwd;
            const float fspeed = lf.speed;
            const uint32_t form_id = lf.keep_id != 0 ? lf.keep_id : ++form_serial_;  // s78n t25.2: a re-plan keeps it
            for (const Slot& s : lf.slots) {
                if (atk_keep[s.member]) continue;  // s78n t26 review round 3: G 5
                Unit& u = units_[idx[s.member]];
                u.large_hover_index = -1;
                const V2 exact = slot_world(lf.centre, fwd, s);
                V2 target = exact;
                // s78n t20 F5 (MOVEMENT §4.3.1: the block goal := the accepted centre): the cell snap only for slots the
                // destination check did not place ([port] unchecked orders keep the older slot search)
                // s78n t25.2: not for an attack order (its goals come from the attack plan, exact)
                if (!(order.flags & kMoveFlagSkipValidation) && !lf.checked && !attack) target = pick_land_slot(u, target, taken);
                // s78n t20 (MOVEMENT §5.4.4 item 5, §4.3.1 item 6): the formation identity and the slot in its frame;
                // the single-member shortcut of the plan (no flag 0x20, no object goal; [port] flag 0x10 has no sender)
                u.form_id = form_id;
                u.form_slot = {-s.back, s.lateral};
                u.self_arrive =
                    f.size() == 1 && !(order.flags & kMoveFlagSkipValidation) && !order.skip_destination_check && !attack;
                // s78n t25.2 (MOVEMENT §4.3.2 item 8): the attack slot survives only an attack re-plan; every unit of an
                // attack order joins its formation's block
                if (!replan) u.atk_theta = u.atk_rho = 0.0f;
                u.atk_form = attack ? form_id : 0u;
                u.atk_target = attack ? order.attack.target : -1;
                u.self_goal = exact;  // s78n t20 F5: the shortcut measures to the exact goal, not a snapped cell
                u.goal = target;
                u.lane.clear();  // s78p: set below for a formation of 2+
                u.has_goal = true;
                u.need_path = true;
                u.arrived = false;
                u.failed = false;
                u.facing = lf.fresh ? kNoFacing : order.facing_deg;  // s78n t20 batch 2: a fresh block has no facings
                // s78k t01 (MOVEMENT §6.1, §3.5.1): the final facing and the flag-0x20 straight path of this move; a new
                // order drops a running arrival turn.
                u.final_facing = lf.fresh ? kNoFacing : order.final_facing_deg;
                u.forced = (order.flags & kMoveFlagSkipValidation) != 0;
                arrival_reset(u);  // s78l t02: the arrival turn state too
                // s78n t25.2: a re-plan starts at once, unless the unit still waits for its composition
                u.start_frame = pc ? kStartPending : replan && u.pend != 0 ? u.start_frame : frame + delays[s.member];
                if (pc) {
                    u.pend = serial;
                    u.adopted = false;  // cu8j t02: it leaves the adopt's coordinator for the order's
                }
                u.formation_speed = f.size() > 1 ? fspeed : kInf;
                u.in_formation = f.size() > 1;  // OI tactical
                u.exit_host = -1;  // s78n t18 (MOVEMENT §5.3): a new order ends the garrison-host gate of a unit that left
                if (u.d.team_container) {
                    // s78n t18 (FIGHTER_FLYING §7.7 item 5): a new order while moving is not taken at once - the
                    // container brakes at its rate (0x14), coasting straight on, then starts the new path from speed 0.
                    // s78n t26.5 (MOVEMENT §3.5.2 G): the coast starts at the path set (set_path_started); until the plan
                    // a moving container walks on along its old order path (team_walk)
                    u.fturn = false;
                    if (!(u.m.speed > 0.0f)) {
                        u.path_started = false;
                        u.m.state = LocoState::WalkStopped;
                    }
                }
                u.jiggle = V2{};
                // s78n t23 review fix 6 (MOVEMENT §5.4.1 "The stamp in full" item 7): the order path goes through the push
                // and stamps the unit's held records only when no collision record owns the unit; with an owner every
                // path record (the detour too) is destroyed with no stamp, and that record ends at its next service
                if (u.push_owner == kNoDetour) stamp_held(u, frame);
                end_push(u);  // a1k t03 [port]: a new move ends a pushed move
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
    // s78p (b0049, MOVEMENT §3.5.2 D / G, s78n t26 RE 2): a land formation of 2+ plans one formation path (0x417520: from
    // the members' centroid when the lead reaches it, else from the lead, to the formation's destination, for the
    // formation's class - attack_path) and every member follows its lane on it (0x413340, set_lanes), ending at its slot.
    // [port] built here at the order, with the order frame's positions (retail: at the plan, after the composition); not
    // for a forced-path order (flag 0x20: the straight path) or air members; a failed formation path leaves the members to
    // plan singly to their slots, as before.
    // The layout 0x40eb90 runs after the path (the form-up box oriented at the path's start, §4.3.1 item 1): [port] the
    // slots are laid out again along the formation path's first segment, so the members' left / right order is the
    // lanes' order (the layout keeps it, §4.3 item 2), and each member's slot is updated.
    if (!attack && !(order.flags & kMoveFlagSkipValidation) && !no_grid)
        for (LandForm& lf : forms) {
            if (lf.members.size() < 2) continue;
            const Unit& lead = units_[idx[lf.members[0]]];
            if (is_air(lead)) continue;
            V2 g = lf.centre;
            std::vector<V2> fpath;
            if (!attack_path(lead, lf.mask, lf.centroid, g, fpath) || fpath.size() < 2) {
                ++stats_.formation_path_failures;
                continue;
            }
            ++stats_.formation_paths;
            const V2 fwd0 = lane_dir(fpath, 0);
            if (length_sq(fwd0) > 0.0f) {
                lf.slots = layout_formation(mem, lf.members, fwd0, k);
                for (const Slot& s : lf.slots) units_[idx[s.member]].form_slot = {-s.back, s.lateral};
            }
            set_lanes(idx, lf, fpath, atk_keep, mem);
        }
    // s78n t25.2 (MOVEMENT §4.3.2 items 2 and 6 - 7): the attack slots of a new order (0x438c10; not at a re-plan;
    // s78n t25.4: guard blocks too, by the guard L rule), then the attack plan of every formation (the "every member can
    // fire" stop, the slot leg, the fire trim, the member goals), before the squad start-delay draws below (a stopped
    // container gets no path, so no draw).
    if (attack) {
        ++(replan ? stats_.attack_replans : stats_.attack_orders);
        if (!replan) attack_slots(order, idx, forms);
        for (const LandForm& lf : forms) attack_plan(order, idx, lf, frame, atk_keep, mem);
    }
    // s78n t14 (FIGHTER_FLYING §7.3 item 2, the path-set hook 0x5ad140 for a kind-1 path): a squad container standing still
    // (speed 0) that gets this move draws every steering member's start delay, synced int [0, Max_Move_Frame_Delay], in team
    // index order. [port] drawn here at the order (the order's units in index order, after the formation layout), not at
    // the path's arrival from the planner.
    for (uint32_t i : idx) {
        const Unit& tc = units_[i];
        // (s78n t20 batch 2: a container the failed zone validation left without a formation gets no path: no draw)
        if (!tc.d.team_container || tc.m.speed != 0.0f || !tc.has_goal) continue;
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

/// ---- s78n t20 (MOVEMENT §4.3.1 items 3 - 4): the destination check of a land move order ----
// 0x439360: every formation whose lead (its seed) is a land layer object, nearest first to the group centroid C. The
// first one, R, is test-planned to D = R's own block goal (s78n t20 batch 2: for a position order the point validated
// for R's seed, §4.2 step 2 - not the raw order point); fewer than 2 path points end the pass with nothing changed (a
// lead on ground its class cannot enter: retail's region search needs the start region to pass the class mask). Axis a
// = the order's facing (not for a fresh block, which carries none), else the direction d1 of that path's last segment;
// phi = the signed angle from its first to its last segment. Passes n = 0 .. 40 over the centre Dn (D, then 20 x
// ceil(n / 8) out at (n mod 8) x 45 degrees; pass 0 tests nothing about D; for n >= 1 Dn must pass the lead's class, be
// zone-connected to D (0x50ee10) and be reachable from it by a straight line or a path <= 1.8 x the distance): each
// formation is placed by place_formation with the step direction v = normalise(rotate(centroid - C, phi)), or -d1 for
// the only formation or a zero offset; a formation that cannot be placed sends the pass to the next n. After n = 40 one
// forced pass at D accepts everything. The order is never refused. An impassable D (only when R's seed stands on ground
// its class cannot enter, or is airborne) is left as it is: retail's tries all fail there and the forced pass keeps D
// (§4.3.1 item 7, "What reaches the destination check"); [port] the port then keeps its slot search (pick_land_slot).
// [port] the taken rectangles of a failed pass are dropped.
// s78n t26 review round 2 (MOVEMENT §4.3.1 item 7 (c), §4.3.2 item 5 [H]): 0x4779e0 - of the movement classes whose
// mask lies inside the intersection of the members' masks, the one with the most mask bits ([port] [L] ties: the lowest
// class index; none inside: the intersection itself). One class for a single-class formation. bkm1 t03 (b0035):
// 0x414720's switch to the class without the Shield bit is applied where the formation's goal is known
// (shield_free_mask, at the layout and again after the destination check).
uint32_t MovementSystem::best_class_within(uint32_t inter, bool& found) const {
    uint32_t best = inter;
    int32_t best_bits = -1;
    for (uint32_t c = 0; c < kMovementClassCount; ++c) {
        const uint32_t cm = cfg_.classes.mask(static_cast<int32_t>(c));
        if ((cm & ~inter) != 0u) continue;
        const auto bits = static_cast<int32_t>(std::popcount(cm));
        if (bits > best_bits) {
            best_bits = bits;
            best = cm;
        }
    }
    found = best_bits >= 0;
    return best;
}

void MovementSystem::set_form_mask(const std::vector<uint32_t>& idx, LandForm& lf, V2 goal) const {
    lf.inter = ~0u;
    for (const uint32_t m : lf.members) lf.inter &= units_[idx[m]].lp.mask;
    bool found = false;
    lf.best = best_class_within(lf.inter, found);
    lf.mask = lf.members.empty() ? lf.best : shield_free_mask(lf.inter, lf.best, units_[idx[lf.members[0]]].m.pos, goal);
}

// bkm1 t03 (b0035, MOVEMENT §4.3.1 item 7 (c) [H]): at every plan 0x414720 takes 0x4779e0's class c; when the goal G is
// zone-connected to the lead for c (or 0x50f480 validates it, G := the validated point) and G is also zone-connected to
// the lead for the best class without the Shield bit 0x40 (0x477b00: of the classes whose mask lies inside the members'
// intersection with bit 0x40 cleared, the one with the most bits), the formation plans for that class instead - so it
// keeps out of base-shield cells whenever the goal is reachable without them. [port] [L] ties as 0x4779e0 (the lowest
// class index); no grid: c.
uint32_t MovementSystem::shield_free_mask(uint32_t inter, uint32_t best, V2 lead, V2 goal) const {
    if (grid_.width() == 0 || grid_.height() == 0 || (best & kPassShield) == 0u) return best;
    V2 g = goal;
    if (!zone_connected(regions_, best, lead, g) && !zone_validate(regions_, goal, lead, best, g)) return best;
    bool found = false;
    const uint32_t ns = best_class_within(inter & ~kPassShield, found);
    if (!found || ns == best) return best;
    return zone_connected(regions_, ns, lead, g) ? ns : best;
}

uint32_t MovementSystem::plan_mask(const Unit& u) const {
    if (cfg_.mode != MoveMode::Land || !u.has_goal) return u.lp.mask;
    bool found = false;
    const uint32_t best = best_class_within(u.lp.mask, found);
    return shield_free_mask(u.lp.mask, found ? best : u.lp.mask, u.m.pos, u.goal);
}

void MovementSystem::destination_check(const MoveOrder& order, const std::vector<uint32_t>& idx, std::vector<LandForm>& forms,
                                       V2 group_c) {
    std::vector<uint32_t> ord;
    for (uint32_t i = 0; i < forms.size(); ++i)
        if (land_layer_object(units_[idx[forms[i].members[0]]])) ord.push_back(i);
    if (ord.empty()) return;
    std::stable_sort(ord.begin(), ord.end(), [&](uint32_t x, uint32_t y) {
        return length_sq(forms[x].centroid - group_c) < length_sq(forms[y].centroid - group_c);
    });
    const LandForm& rf = forms[ord[0]];
    const Unit& lead = units_[idx[rf.members[0]]];
    const V2 dest = rf.centre;
    if (!grid_.passable_world(lead.m.pos, rf.mask) || !grid_.passable_world(dest, rf.mask)) return;
    LandPathScratch sc;
    LandPath test;
    {
        LandPathRequest q;
        q.start = lead.m.pos;
        q.goal = dest;
        q.mask = rf.mask;
        q.clearance_radius = lead.d.radius > kCellSize && !lead.d.team_container ? lead.d.radius : 0.0f;  // as plan_land
        find_land_path(grid_, regions_, q, sc, test);
    }
    if (!test.ok || test.points.size() < 2) return;
    const size_t np = test.points.size();
    const V2 d0 = normalize_retail(test.points[1] - test.points[0]);
    const V2 d1 = normalize_retail(test.points[np - 1] - test.points[np - 2]);
    if (!(length_sq(d1) > 0.0f)) return;
    const V2 axis = order.facing_deg != kNoFacing && !rf.fresh ? dir_from_deg(order.facing_deg) : d1;
    const float cphi = length_sq(d0) > 0.0f ? dot(d0, d1) : 1.0f, sphi = length_sq(d0) > 0.0f ? cross(d0, d1) : 0.0f;
    const bool several = ord.size() > 1;
    constexpr float kC = 0.70710678f;
    static constexpr V2 kRing[8] = {{1.0f, 0.0f}, {kC, kC}, {0.0f, 1.0f}, {-kC, kC}, {-1.0f, 0.0f}, {-kC, -kC}, {0.0f, -1.0f}, {kC, -kC}};
    std::vector<FootRect> taken;
    std::vector<V2> centres(ord.size());
    for (uint32_t n = 0; n <= 41; ++n) {
        const bool forced = n == 41;  // the forced pass at D after n = 40
        V2 dn = dest;
        if (n > 0 && !forced) {
            dn = dest + kRing[n % 8u] * (20.0f * static_cast<float>((n + 7u) / 8u));
            // bkm1 t03: the pass tests keep 0x4779e0's class (the test plan above is a plan: 0x414720's class)
            if (!grid_.passable_world(dn, rf.best) || !zone_connected(regions_, rf.best, dest, dn)) continue;
            if (!line_passable(grid_, dest, dn, rf.best)) {
                LandPathRequest q;
                q.start = dest;
                q.goal = dn;
                q.mask = rf.best;
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
            ok = place_formation(idx, lf, dn, v, axis, r == 0, several, forced, taken, centres[r]);
        }
        if (!ok) continue;
        for (size_t r = 0; r < ord.size(); ++r) {
            forms[ord[r]].centre = centres[r];
            forms[ord[r]].checked = true;  // s78n t20 F5
        }
        return;
    }
}

// 0x43d6a0, one formation: S = BetweenFormationSpacing, E = the destination query extension; (HX, HY) = the half extents
// of the union of its members' slot rectangles (slot +- the member's footprint half extents 0x396850; a lone squad
// container: its joint slot box); R = the rectangle (HX + S, HY + S) on `axis`. Up to 40 tries (destination_try_offset).
// A try is rejected when: several formations and R contains Dn; R overlaps a rectangle taken in this pass; its centre
// cell or R is impassable ([port] R's centre and corners); while "crossed", it is not zone-connected to Dn (0x50ee10) or
// its path from Dn is > 1.8 x the straight line; or the object test fails. Ground state (s78n t20 batch 2): "met" at the
// first impassable try, "crossed" at the next passable one; a try that fails after the ground tests while crossed,
// followed later by another impassable try, ends the tries at once. Before "met" the first formation uses the halved,
// alternating offsets. Object test: the layer objects in R grown by E, without the order's own units and, for a
// formation that is all Infantry / Bike or all Giant and not all squad containers, without single Infantry-class units;
// no owner test and no moving test. Each counts where it is going - its footprint at the end of its path with the final
// direction when it has one - and is a blocker when a member's slot rectangle (its half extents + S) overlaps it, the
// two classes interact and the member cannot crush it. Rejected iff the sum of the blockers' hx x hy > 0.15 x (HX + S)
// (HY + S). Accepted: `centre` and R joins the taken list. Forced pass: when no try is accepted, `centre` := the centre
// of the last try that reached the object test, or Dn when none did, and that rectangle joins the taken list.
// [port] the union box is centred where the slots put it (the port's layout is not centred on the destination).
bool MovementSystem::place_formation(const std::vector<uint32_t>& idx, const LandForm& f, V2 dn, V2 v, V2 axis, bool first,
                                     bool several, bool forced, std::vector<FootRect>& taken, V2& centre) const {
    const float sp = cfg_.constants.between_formation_spacing, ext = cfg_.constants.destination_query_extension;
    float x0 = 3.0e38f, x1 = -3.0e38f, y0 = 3.0e38f, y1 = -3.0e38f;
    bool all_inf = true, all_giant = true, all_containers = true;
    // s78n t20 F8 [port]: the slots are laid along the layout's forward axis f.fwd, R along `axis`: each slot rectangle's
    // centre and extents are taken in the axis frame (the same box when the two agree); s78n t25.2: formation_box
    const V2 right = formation_right(axis);
    if (!formation_box(idx, f, axis, x0, x1, y0, y1)) return false;
    for (const Slot& s : f.slots) {
        const Unit& m = units_[idx[s.member]];
        const int32_t cls = m.d.collision_class;
        all_inf = all_inf && (cls == kUccInfantry || cls == kUccBike);
        all_giant = all_giant && cls == kUccGiantVehicle;
        all_containers = all_containers && m.d.team_container;
    }
    const float hx = 0.5f * (x1 - x0) + sp, hy = 0.5f * (y1 - y0) + sp;
    const V2 box_off = axis * (0.5f * (x1 + x0)) + right * (0.5f * (y1 + y0));
    const float m_side = std::min(hx, hy);
    const bool skip_infantry = (all_inf || all_giant) && !all_containers;
    const auto inside = [](const FootRect& r, V2 p) {
        const V2 d = p - r.c;
        return std::fabs(dot(d, r.a)) <= r.hx && std::fabs(dot(d, perp_left(r.a))) <= r.hy;
    };
    std::vector<uint32_t> cand;
    LandPathScratch sc;
    enum class Ground : uint8_t { None, Met, Crossed };
    Ground ground = Ground::None;
    bool crossed_fail = false;  // a try failed after the ground tests while crossed
    bool reached = false;       // a try reached the object test (forced pass: the last one is taken)
    V2 last_c = dn;
    FootRect last_rect{dn + box_off, axis, hx, hy, false};
    for (uint32_t k = 0; k < 40; ++k) {
        const V2 c = dn + destination_try_offset(k, first && ground == Ground::None, v, m_side);
        const FootRect rect{c + box_off, axis, hx, hy, false};
        if (several && inside(rect, dn)) continue;
        bool hit = false;
        for (const FootRect& t : taken) hit = hit || foot_overlap(rect, t);
        if (hit) continue;
        bool pass = grid_.passable_world(c, f.mask) && grid_.passable_world(rect.c, f.mask);
        const V2 b = perp_left(axis);
        for (int corner = 0; corner < 4 && pass; ++corner) {
            const V2 p = rect.c + axis * ((corner & 1) ? hx : -hx) + b * ((corner & 2) ? hy : -hy);
            pass = grid_.passable_world(p, f.mask);
        }
        if (!pass) {
            if (crossed_fail) break;  // the early end
            if (ground == Ground::None) ground = Ground::Met;
            continue;
        }
        if (ground == Ground::Met) ground = Ground::Crossed;
        if (ground == Ground::Crossed) {
            if (!zone_connected(regions_, f.mask, dn, c)) {
                crossed_fail = true;
                continue;
            }
            if (!line_passable(grid_, dn, c, f.mask)) {
                LandPathRequest q;
                q.start = dn;
                q.goal = c;
                q.mask = f.mask;
                q.snap_goal = false;
                LandPath lp;
                find_land_path(grid_, regions_, q, sc, lp);
                if (!lp.ok || path_length(lp.points) > 1.8f * distance(dn, c)) {
                    crossed_fail = true;
                    continue;
                }
            }
        }
        // ---- the object test
        reached = true;
        last_c = c;
        last_rect = rect;
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
        if (sum > 0.15f * hx * hy) {
            if (ground == Ground::Crossed) crossed_fail = true;
            continue;
        }
        centre = c;
        taken.push_back(rect);
        return true;
    }
    if (!forced) return false;
    centre = reached ? last_c : dn;
    taken.push_back(last_rect);
    return true;
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
        // bkm1 t05 (b0037, MOVEMENT §4.3 item 1 with the s78n t19 correction [H]): 0x41a080 ranks by the attack range
        // 0x3749d0 in space too (the speed key was a [port] stand-in)
        f.attack_range = attack_range_fn_ ? attack_range_fn_(u.d.id) : 0.0f;
        f.radius = u.d.radius;
        f.spacing = u.d.formation_spacing;
        f.hero = u.d.hero;
        f.type_name = u.d.type_name;
        f.grouping = u.d.grouping;
        f.order = u.d.formation_order;
        mem.push_back(f);
    }
    // [port: Gabriel 2026-11-13] fleet cohesion (MoveOrder::cohesion_tight): every ship of the order that is not a
    // fighter and can move plans at the slowest such ship's max speed, so the fleet keeps its spacing in flight. A loose
    // order (or a tight one with fewer than two such ships) clears the cap: retail behaviour.
    float cohesion = kInf;
    if (order.cohesion_tight) {
        uint32_t ships = 0;
        for (uint32_t i : idx) {
            const Unit& u = units_[i];
            if (u.d.locomotor == LocomotorKind::Fighter || !(u.lp.max_speed > 0.0f)) continue;
            cohesion = std::min(cohesion, u.lp.max_speed);
            ++ships;
        }
        if (ships < 2) cohesion = kInf;
    }
    for (uint32_t i : idx) {
        Unit& u = units_[i];
        u.cohesion_speed = u.d.locomotor == LocomotorKind::Fighter ? kInf : cohesion;
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
            for (const Slot& s : layout_formation(mem, f, fwd, k)) {  // bkm1 t05: the attack-range key
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
        u->cohesion_speed = kInf;  // [port] fleet cohesion: a stop ends the move's cap
        u->self_arrive = false;
        u->atk_theta = u->atk_rho = 0.0f;  // s78n t25.2: a stop drops the attack block (halt() keeps it)
        u->atk_form = 0;
        u->atk_target = -1;
        u->lane.clear();  // s78p: a lane not yet taken goes with the order
        u->lane_d = 0.0f;
        u->lane_z = 0.0f;
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

void MovementSystem::halt(std::span<const ObjectId> ids, uint32_t frame) {
    for (const ObjectId id : ids) {
        const ObjectId one[] = {id};
        Unit* u = unit_ptr(order_id(id));
        if (!u || cfg_.mode != MoveMode::Land || u->d.locomotor != LocomotorKind::Default || u->deploy != LocoState::None) {
            const float th = u ? u->atk_theta : 0.0f, rh = u ? u->atk_rho : 0.0f;  // s78n t25.2: the block is kept
            const uint32_t af = u ? u->atk_form : 0u;
            const ObjectId at = u ? u->atk_target : -1;
            const uint32_t fid = u ? u->form_id : 0u;  // s78o (b0066): and the formation identity
            stop(one);
            if (u) {
                u->atk_theta = th;
                u->atk_rho = rh;
                u->atk_form = af;
                u->atk_target = at;
                u->form_id = fid;
            }
            continue;
        }
        const float v = u->m.speed;
        const LocoState s = u->m.state;
        // s78n t25.2 (MOVEMENT §4.3.2 item 8): the land stop and item 6's stop end the walk, not the attack block
        // s78o (b0066, FIGHTER_FLYING §7.5 item 4): nor the formation - form_id (form_slot is not touched by stop()) stays,
        // so halted mates side-step by the footprint overlap coefficient, not the distance ramp, and the deploy gate keeps
        // the formation together
        const float th = u->atk_theta, rh = u->atk_rho;
        const uint32_t af = u->atk_form;
        const ObjectId at = u->atk_target;
        const uint32_t fid = u->form_id;
        stop(one);
        u->atk_theta = th;
        u->atk_rho = rh;
        u->atk_form = af;
        u->atk_target = at;
        u->form_id = fid;
        if (!(v > 0.0f)) continue;
        u->m.speed = v;  // the speed is kept: the unit brakes through End Move
        u->m.state = s;
        brake_to_stop(*u, frame);
    }
}

void MovementSystem::clear_attack_block(std::span<const ObjectId> ids) {
    for (const ObjectId id : ids) {
        Unit* u = unit_ptr(order_id(id));
        if (!u) continue;
        u->atk_theta = u->atk_rho = 0.0f;
        u->atk_form = 0;
        u->atk_target = -1;
    }
}

void MovementSystem::brake_to_stop(Unit& u, uint32_t frame) {
    const float v = u.m.speed, dec = u.lp.decel;
    if (u.d.team_container && v > 0.0f) {
        u.coast = true;  // 0x14 with no top path: the coast brakes at the team rate (team_walk)
        return;
    }
    if (u.d.team_container || !(v > 0.0f) || !(dec > 0.0f)) {
        u.m.speed = 0.0f;
        u.m.state = LocoState::WalkStopped;
        u.coast = false;
        return;
    }
    // s78n t26.4 (MOVEMENT §3.5.2 C.1 / C.4, §5.4.1 "The impassable-cell rule" item 3): a Walk unit keeps its speed and
    // state; with no started path its walk goes to End Move at T = a and coasts along its last frame's displacement to
    // 0x15 (the t20 braking stub record is gone)
    (void)frame;
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
    u->around.clear();  // s78n t18
    u->form_id = 0;     // s78n t20
    u->self_arrive = false;
    u->atk_theta = u->atk_rho = 0.0f;  // s78n t25.2
    u->atk_form = 0;
    u->atk_target = -1;
    u->lane.clear();  // s78p
    u->lane_d = 0.0f;
    u->lane_z = 0.0f;
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
    push_kills_.clear();  // s78n t20 batch 1
    deploy_pose_.clear();  // s78n t26.1 review
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
        // s78n t26.1 (MOVEMENT §3.2.1 item 2, the plan 0x41c1a0 gate): a formation whose every member is a `Deploys`
        // type that is deployed or deploying and not an auto-deployer gets no path - no state change; the plan is tried
        // again ([port] every tick, retail at every formation update), so the formation waits (the group move admits
        // such a unit only while it deploys; once deployed the gate still holds until a DEPLOY-off or garrison undeploy).
        if (deploy_gate_holds(u)) continue;
        // s78n t20 (MOVEMENT §4.3.1 item 6, the plan 0x41c1a0): a one-member formation whose goal lies inside its own
        // footprint radius 0x3969f0 (2-D) signals "arrived" and stops: a unit ordered onto its own footprint does not move
        if (u.self_arrive && !is_air(u) && u.d.locomotor == LocomotorKind::Default && distance(u.m.pos, u.self_goal) < foot_radius(u)) {
            u.need_path = false;
            u.has_goal = false;
            u.arrived = true;
            u.m.path.clear();
            u.m.next = 0;
            u.around.clear();
            u.path_change = frame;
            walk_out_done(u);
            if (!u.d.team_container) {  // (a moving container brakes to a stop at its rate, team_walk)
                u.m.speed = 0.0f;
                u.m.state = LocoState::WalkStopped;
            }
            continue;
        }
        req.push_back(i);
    }
    if (req.empty()) return;
    // s78p (b0049): a member with a lane takes it as its path (no search of its own) while the lane still ends at its goal
    // and both its position and that goal pass its class ([port] else it plans singly, as before: the search's start escape
    // and goal snap)
    std::vector<uint8_t> use_lane(req.size(), 0);
    for (size_t r = 0; r < req.size(); ++r) {
        const Unit& u = units_[req[r]];
        use_lane[r] = !u.lane.empty() && u.lane.size() >= 2 && !u.forced && !is_air(u) && u.lane.back() == u.goal &&
                              grid_.passable_world(u.m.pos, u.lp.mask) && grid_.passable_world(u.goal, u.lp.mask)
                          ? 1
                          : 0;
    }
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
            if (u.forced || use_lane[r]) continue;  // s78k t01: the straight path is built below; s78p: the lane
            LandPathRequest q;
            q.start = u.m.pos;
            q.goal = u.goal;
            q.mask = plan_mask(u);  // bkm1 t03 (b0035): 0x414720 - the class without the Shield bit when the goal allows
            // s78n t03 [port]: a squad container plans without clearance (its planner footprint is untraced; its radius is
            // the joint box's, for the formation layout).
            q.clearance_radius = u.d.radius > kCellSize && !u.d.team_container ? u.d.radius : 0.0f;
            if (u.threat != kThreatOff && u.d.space_layer_type == 0) q.threat = ThreatCost::make(tf ? &tf : nullptr, u.threat);
            find_land_path(grid_, regions_, q, sc, out[r]);
        }
    });
    // s78n t26.3 (MOVEMENT §5.3.2 "Freshness"): the corridor test reads the layer flags of the last tracker update, so a unit
    // is Static for every corner pass of this plan when it was not moving before the plan (members planned earlier in the
    // loop do not turn Moving for the later ones)
    corner_static_.assign(units_.size(), 0);
    if (cfg_.mode == MoveMode::Land)
        for (size_t i = 0; i < units_.size(); ++i) corner_static_[i] = unit_moving(units_[i]) ? 0 : 1;
    for (size_t r = 0; r < req.size(); ++r) {
        Unit& u = units_[req[r]];
        u.need_path = false;
        ++stats_.paths_planned;
        // bkm1 t07 (b0055): the path set resets the stack (0x554480): a record pushed onto the old order path after the
        // order (a TerrainCollision detour while the new order waited) goes with it; a failed plan sets no path (review)
        if (u.pushed && !is_air(u) && (u.forced || use_lane[r] || out[r].ok)) end_push(u);
        std::vector<V2> lane;
        lane.swap(u.lane);  // s78p: a lane is taken by this plan or dropped
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
            u.m.frac.clear();  // s78n t26.2: a new record - fractions (1.0, 1.0), turn-done clear
            u.m.turn_done = false;
            // bkm1 t06 (MOVEMENT §3.5.2 "waypoint z" item 3): 0x40ed70 stores both ends with their heights ([port] the unit's
            // height and the terrain's at the destination); only the 3-D length reads them (C.11 never sums its one segment)
            u.m.end_z = 0.0f;
            corner_pass(u);  // s78n t26.3
            plan_deploy_cap(u, frame, path_length_3d(u.m.path, u.z, terrain(u.goal)));  // s78n t26.1
            set_path_started(u);  // s78n t26.4
            continue;
        }
        if (use_lane[r]) {
            // s78p (b0049, MOVEMENT §3.5.2 G): the path set of the member's lane - from where it stands now; the corner pass
            // runs on the lane (§3.5.2 D) and the walk-deployed cap's d is the formation path's 3-D length (§3.2.1 item 2)
            u.m.path = std::move(lane);
            lane_start(u.m.path, u.m.pos);  // where it stands now (it may have walked on since the order)
            u.m.end_z = u.lane_z;  // bkm1 t06: a slot-leg lane keeps G's height on its last point
            u.m.end_at = u.m.path.back();
            u.lane_z = 0.0f;
            u.m.next = 1;
            u.m.escape_until = 0;
            u.m.frac.clear();
            u.m.turn_done = false;
            ++stats_.lanes_taken;
            corner_pass(u);
            plan_deploy_cap(u, frame, u.lane_d);
            set_path_started(u);
            continue;
        }
        if (out[r].ok) {
            u.m.path = std::move(out[r].points);
            u.m.end_z = 0.0f;  // bkm1 t06: the pathfinder stores z = 0 at every point
            u.m.next = 1;
            u.m.escape_until = out[r].start_blocked ? 2u : 0u;
            u.m.frac.clear();  // s78n t26.2: a new record - fractions (1.0, 1.0), turn-done clear
            u.m.turn_done = false;
            u.goal = out[r].goal_used;
            corner_pass(u);  // s78n t26.3
            plan_deploy_cap(u, frame);  // s78n t26.1
            set_path_started(u);  // s78n t26.4
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

// s78n t26.1 (MOVEMENT §3.2.1 item 1, s78n t26 RE 2): a move that finishes ends the garrison-host gate; a unit that walked
// out of a garrison host is reported (GarrisonUnitBehavior 0x443d10 on the move-finished signal outside states 1 / 2 calls
// the host's vf +0xf8, 0x6360b0: the cap test with d = 0; [M] the exit).
void MovementSystem::walk_out_done(Unit& u) {
    if (u.exit_host >= 0) exit_arrivals_.push_back({u.d.id, u.exit_host});
    u.exit_host = -1;
}

std::vector<std::pair<ObjectId, ObjectId>> MovementSystem::take_exit_arrivals() {
    std::vector<std::pair<ObjectId, ObjectId>> out;
    out.swap(exit_arrivals_);
    return out;
}

// s78n t26.1 (MOVEMENT §3.2.1 item 5, the push 0x6859a0 and the StoppedVsStopped chooser 0x687010 rule 3): deployed or
// deploying with M <= 0. (Path Around's obstacle set 0x67d580 has no M clause: around_obstacles.)
bool MovementSystem::deploy_immobile(const Unit& x) {
    return x.d.max_walk_deployed <= 0.0f && (x.deploy == LocoState::WalkDeploying || x.deploy == LocoState::WalkDeployed);
}

// s78n t26.1 (MOVEMENT §3.2.1 item 3, Walk 0x19 with M > 0 and a top path): speed 0, the path started, state 0x12 and the
// flag set, with no length test and no start turn; the frame ends (true). Retail 0x19 = deploy WalkDeployed with the walk
// stopped; a unit already walking deployed (a Walk move state) is not in it.
bool MovementSystem::deploy_branch(Unit& u) {
    if (u.deploy != LocoState::WalkDeployed || u.m.state != LocoState::WalkStopped) return false;
    u.m.speed = 0.0f;
    u.m.started = true;  // bkm3 review: path +0x6d := 1 (an unstarted order path would otherwise cycle 0x12 <-> 0x16 forever)
    u.walk_deployed = true;
    u.m.state = LocoState::WalkStartMove;
    return true;
}

// s78n t26.1 (MOVEMENT §3.2.1 item 2): the plan's gate - every member of u's formation (form_id; a unit without one alone)
// is a `Deploys` type, deployed (0x39a390) or deploying (0x18), and not an auto-deployer.
bool MovementSystem::deploy_gate_holds(const Unit& u) const {
    if (cfg_.mode != MoveMode::Land) return false;
    const auto manual_dep = [](const Unit& x) {
        return x.d.deploys && !auto_deployer(x) && (deployed_d(x) || x.deploy == LocoState::WalkDeploying);
    };
    if (!manual_dep(u)) return false;
    if (u.form_id == 0) return true;
    for (const Unit& o : units_)
        if (o.form_id == u.form_id && !manual_dep(o)) return false;
    return true;
}

// s78n t26.1 (MOVEMENT §3.2.1 item 2, the plan's member loop): an auto-deployer takes the cap test 0x3ac100 with d = the
// summed 3-D length of the formation path when the path is set. s78p (b0049): `d` >= 0 is that length for a member on a
// lane (Unit::lane_d); else ([port] a unit alone, or one that planned singly) its own planned path's.
void MovementSystem::plan_deploy_cap(Unit& u, uint32_t frame, float d) {
    if (!auto_deployer(u) || u.d.locomotor != LocomotorKind::Default || is_air(u)) return;
    deploy_cap_unit(u, d >= 0.0f ? d : path_length_3d(u.m.path), frame);
}

// s78n t26.4 (MOVEMENT §3.5.2 G, s78n t26 RE 2): the path set (0x42c680 / push 0x554310) does not set the started byte
// +0x6d; 0x410170 sets it when the unit is aligned - waypoint 0's speed fraction > 0 and |delta| <= R or <= CE (delta from
// the facing to the wanted yaw of path index 0: [port] the first leg's heading, the lane term is 0 at its start point and
// the collision vector is not asked at the plan) - and keeps the locomotor state and speed. Only a unit moving in 0x12 -
// 0x14 is affected: a standing one starts its path at its walk's 0x16 either way (then start rule A). A moving unit that is
// not aligned coasts in End Move (C.1 / C.4), stops at 0x15 and starts the path from 0x16. [port] A squad container keeps
// its own path_started (team_walk); the Walk byte is set for it and for every non-Walk locomotor. s78n t26.5 (FIGHTER_FLYING
// §7.7 item 5, MOVEMENT §3.5.2 G / E): a squad container's new path is never started at the path set (0x5ad140 has no
// aligned test) - a moving one brakes at its rate in End Move along its facing (Unit::coast), a standing one starts it
// from 0x16.
void MovementSystem::set_path_started(Unit& u) const {
    LandMotion& m = u.m;
    m.started = true;
    if (u.d.team_container) {
        u.path_started = false;
        u.coast = m.speed > 0.0f;
        if (!u.coast) m.state = LocoState::WalkStopped;
        return;
    }
    if (is_air(u) || (u.d.locomotor != LocomotorKind::Default && u.d.locomotor != LocomotorKind::Bike)) return;
    const bool moving = m.speed > 0.0f && (m.state == LocoState::WalkStartMove || m.state == LocoState::WalkMoving ||
                                           m.state == LocoState::WalkEndMove);
    if (!moving) return;
    bool aligned = false;
    if (m.path.size() >= 2 && wp_frac(m, 0) > 0.0f) {
        const V2 d = m.path[1] - m.path[0];
        const float err = length_sq(d) > 0.0f ? std::fabs(wrap_deg(heading_deg(d) - m.yaw)) : 0.0f;
        aligned = err <= u.lp.turn_rate || err <= u.lp.close_enough_angle;
    }
    m.started = aligned;
}

// s78n t26.3 (MOVEMENT §3.5.2 D, s78n t26 RE 2): the corner-speed pass 0x5070b0 of the Walk path set vf +0x190 0x42c680
// and of the squad container's 0x5ad140 - land mode, a formation path (every land order composes a formation: form_id;
// [port] the flag-0x20 straight path too, it is the formation's path) of a Walk unit or a squad container. The input is
// the unit's own path: s78p (b0049) for a member of a formation of 2+ its lane 0x413340 (set_lanes / member_lane, with the
// pull-in); a unit alone (or one that planned singly) its own planned path. vmax = the effective max speed (a container: its
// team speed), vmin = Min_Speed x mode, R, f_min, a = the deceleration (a container: its rate 0x5ab540, team_rate); probe
// segment max(2, 10 x 0x397830) = max(2, the soft radius) ([port] UnitDesc::radius stands for Scale_Factor x the
// radius), a container max(2, max(HX, HY) / 2). Line test = 0x5a4c10 for the unit's movement class; corridor 0x5093c0 =
// per probe segment a zero-width rectangle (centre the midpoint, half length |seg| / 2), failed by any land layer object
// other than the unit whose footprint rectangle overlaps it and that was Static (not moving, unit_moving) before the plan
// (corner_static_: the flags of the last tracker update, §5.3.2). The splice
// (item 1) is not reachable: the plan resets the stack before the path set (§3.5.2 G), as plan_land replaces the path.
void MovementSystem::corner_pass(Unit& u) {
    if (cfg_.mode != MoveMode::Land || u.form_id == 0 || is_air(u) || u.m.path.size() < 2) return;
    if (u.d.locomotor != LocomotorKind::Default && !u.d.team_container) return;
    CornerPassIn in;
    in.yaw0 = u.m.yaw;
    in.rate = u.lp.turn_rate;
    in.vmin = std::max(u.d.min_speed, 0.0f) * mode_mult();
    in.f_min = u.d.min_speed_fraction_for_turn;
    float hx = 0.0f, hy = 0.0f;
    if (u.d.team_container) {
        const float cap = u.team_cap < kInf ? u.team_cap : u.lp.max_speed;
        in.vmax = u.speed_factor > 0.0f ? cap : 0.0f;
        const float rate = team_rate(u);
        in.decel = rate > 0.0f ? rate : cap;
        in.seg = team_joint_box(u.d.id, hx, hy) ? std::max(2.0f, 0.5f * std::max(hx, hy)) : std::max(2.0f, u.d.radius);
    } else {
        in.vmax = u.team_cap < kInf && u.speed_factor > 0.0f ? u.team_cap : u.lp.max_speed;
        in.decel = walk_decel(u, in.vmax);  // s78n t26 closing review: a Walk_Transition unit's vf +0x2e0
        in.seg = std::max(2.0f, u.d.radius);
    }
    struct Ctx {
        const MovementSystem* sys;
        const Unit* u;
        mutable std::vector<uint32_t> cand;
    } ctx{this, &u, {}};
    in.ctx = &ctx;
    in.line_ok = [](const void* c, V2 a, V2 b) {
        const Ctx& x = *static_cast<const Ctx*>(c);
        return line_passable(x.sys->grid_, a, b, x.u->lp.mask);
    };
    in.corridor_ok = [](const void* c, const std::vector<V2>& probe) {
        const Ctx& x = *static_cast<const Ctx*>(c);
        for (size_t k = 0; k + 1 < probe.size(); ++k) {
            const V2 a = probe[k], b = probe[k + 1];
            const float len = distance(a, b);
            FootRect seg;
            seg.c = (a + b) * 0.5f;
            seg.a = len > 0.0f ? V2{(b.x - a.x) / len, (b.y - a.y) / len} : V2{1.0f, 0.0f};
            seg.hx = 0.5f * len;
            seg.hy = 0.0f;
            x.sys->layer_box_query(seg.c, seg.hx, x.cand);
            for (const uint32_t oi : x.cand) {
                const Unit& o = x.sys->units_[oi];
                if (o.d.id == x.u->d.id || oi >= x.sys->corner_static_.size() || !x.sys->corner_static_[oi]) continue;
                if (foot_overlap(seg, x.sys->foot_of(o))) return false;  // a Static record (flag 2)
            }
        }
        return true;
    };
    CornerPassStats st;
    // s78p: corner 0 is the unit's own position (a lane path starts at its foot on the lane, lane_start)
    std::vector<float> f;
    if (u.m.path[0] == u.m.pos) {
        f = corner_fractions(u.m.path, in, &st);
    } else {
        std::vector<V2> poly = u.m.path;
        poly[0] = u.m.pos;
        f = corner_fractions(poly, in, &st);
    }
    stats_.corner_zero += st.zero;
    stats_.corner_slowed += st.slowed;
    stats_.corner_stepdowns += st.stepdowns;
    // every entry 1.0 is the default record (an empty vector)
    if (std::all_of(f.begin(), f.end(), [](float v) { return v == 1.0f; })) f.clear();
    u.m.frac = std::move(f);
}

// ---------------------------------------------------------------- S7 collision layer (TACTICAL_GAPS §6.7)
// Candidates of unit `ai` (§5.2 item 4): the layer-0x10 objects in the 160 x 160 square around it, ascending id (index).
void MovementSystem::land_candidates(size_t ai, std::vector<uint32_t>& out) const {
    layer_box_query(units_[ai].m.pos, 0.5f * kCollisionCrowdBox, out);
    std::erase(out, static_cast<uint32_t>(ai));
}

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
    if (in_place_turn(a)) {  // s78n t26.2: vf +0x1b0 (states 0xF / 0x10 / 0x11)
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
        // s78n t20 batch 1 (MOVEMENT §5.2 items 2 / 5 / 7): the check runs on the locomotor's countdown, so an object without
        // a locomotor (a death-clone obstacle) is never a checker - only a candidate (and the pair filter's "neither side
        // has a locomotor" then never applies: the checker has one)
        if (a.d.no_locomotor) continue;
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
                    // s78n t23 (MOVEMENT §5.4.6): every Moving / Moving pair the predictor finds gets a record (crusher
                    // pairs and squad containers included)
                    if (!mvm_contact(a, b)) continue;
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
// waits. `rng` = the battle's synced stream: s78n t23, the MovingVsMoving retry draws of §5.4.6 (mvm_fail and the passed
// retry frames of states 10 / 12), in service order. Last, the records' speed answers (vf +0x40, 0x509ea0) are folded per
// unit in list order: the lowest speed, and the "at once" flag of the last record that writes one (MovingVsStopped and
// MovingVsMoving; a StoppedVsStopped record answers max speed and leaves the flag).
void MovementSystem::drive_collisions(uint32_t frame, SimRng& rng) {
    for (Unit& u : units_) {
        u.coll_speed = kInf;
        u.coll_now = false;
    }
    const float half = 0.5f * kCollisionCrowdBox;
    std::vector<uint32_t> crowd;
    coll_.run(
        frame, [&](CollisionObject& c) { return collision_valid(c); },
        // vf +0x50 "takes the quota pass": MovingVsStopped always, StoppedVsStopped never, MovingVsMoving only in state 0
        // when the chooser asks for a set-up (s78n t23 review fix 2, MOVEMENT §5.4.1 "The stamp in full" item 5,
        // 0x6811c0: the chooser without side effects, mode != 0 - a Pass or a Path Around; a mode-0 result is decided
        // at once in the first pass); s78n t24 (MOVEMENT §5.4.7 item 2): TerrainCollision only in state 0
        [&](const CollisionObject& c) {
            if (c.kind == CollisionKind::Terrain) return c.state == CollisionState::Active;
            if (c.kind != CollisionKind::MovingVsMoving) return c.kind == CollisionKind::MovingVsStopped;
            if (c.state != CollisionState::Active) return false;
            const int32_t ia = find(c.a), ib = find(c.b);
            return mvm_choose(c, units_[static_cast<size_t>(ia)], units_[static_cast<size_t>(ib)], false).mode != 0;
        },
        [&](const CollisionObject& c) {
            // 5 x the land object-grid objects in the 160 x 160 box on the participants' XY midpoint (s78n t18: the
            // layer-0x10 box query - a squad counts once, as its container; both participants count). The keep pass
            // ran first, so both participants exist. (Not called for a one-participant Terrain record: its crowd is 0.)
            const V2 pa = units_[static_cast<size_t>(find(c.a))].m.pos, pb = units_[static_cast<size_t>(find(c.b))].m.pos;
            layer_box_query((pa + pb) * 0.5f, half, crowd);
            return static_cast<uint32_t>(crowd.size());
        },
        [&] { return quota_used_ < cfg_.constants.dynamic_land_complexity_quota; },
        [&](CollisionObject& c, bool full) { resolve_collision(c, full, frame, rng); });
    stats_.collisions_cancelled += coll_.last_cancelled();
    stats_.collisions_resolved += coll_.last_resolved();
    // s78n t20 F6: the early arrival (resolve, record removed) set its mover's "at once" flag directly; [port] it is kept
    // through the fold below (before, the flag of any later record of that unit overwrote it)
    std::vector<uint32_t> early_now;
    for (uint32_t i = 0; i < units_.size(); ++i)
        if (units_[i].coll_now) early_now.push_back(i);
    for (const CollisionObject& c : coll_.list()) {
        Unit* a = unit_ptr(c.a);
        Unit* b = unit_ptr(c.b);
        if (!a || !b) continue;
        // max speed, flag untouched; s78n t23: a finished record is destroyed before the walk (driver step 3)
        if (c.kind == CollisionKind::StoppedVsStopped || c.state == CollisionState::Resolved) continue;
        a->coll_speed = std::min(a->coll_speed, record_answer(c, true));
        a->coll_now = c.now_a;
        b->coll_speed = std::min(b->coll_speed, record_answer(c, false));
        b->coll_now = c.now_b;
    }
    for (const uint32_t i : early_now) units_[i].coll_now = true;
}
}  // namespace alamo::tactical::movement

#include "alamo/tactical/movement/movement_system.hpp"

#include <algorithm>

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

void MovementSystem::set_grid(PassabilityGrid grid) {
    grid_ = std::move(grid);
    regions_built_ = false;
    rebuild_regions_if_dirty();
}

void MovementSystem::rebuild_regions_if_dirty() {
    if (regions_built_ && regions_.grid_version() == grid_.version()) return;
    const std::vector<uint32_t> masks = cfg_.classes.distinct_masks();
    regions_.build(grid_, masks);
    regions_built_ = true;
    for (Unit& u : units_) u.slot = regions_.mask_slot(u.lp.mask);
}

int32_t MovementSystem::find(ObjectId id) const {
    auto it = std::lower_bound(units_.begin(), units_.end(), id, [](const Unit& u, ObjectId v) { return u.d.id < v; });
    if (it == units_.end() || it->d.id != id) return -1;
    return static_cast<int32_t>(it - units_.begin());
}

void MovementSystem::configure(Unit& u) const {
    const MovementConstants& k = cfg_.constants;
    const float mult = cfg_.mode == MoveMode::Land ? k.land_speed_multiplier : k.space_speed_multiplier;
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
    const int32_t i = find(id);
    if (i < 0) return v;
    const Unit& u = units_[static_cast<size_t>(i)];
    v.id = id;
    v.pos = u.m.pos;
    v.z = u.z;
    v.yaw = u.m.yaw;
    v.speed = u.m.speed;
    v.max_speed = loco_max_speed(u);  // S10-int; S7 t14: the jetpack's Alternate_Max_Speed in its airborne kind
    v.has_goal = u.has_goal;
    v.arrived = u.arrived;
    v.failed = u.failed;
    v.goal = u.goal;
    v.replans = u.replans;
    v.space_fallback = u.fallback;
    v.start_frame = u.start_frame;  // OI tactical
    v.formation_speed = u.formation_speed;
    v.large_hover_index = u.large_hover_index;
    v.jiggle = u.jiggle;
    v.locomotor = u.d.locomotor;  // §4.3-4.7
    v.secondary_locomotor = u.on_secondary;  // S7 t06
    v.loco_state = u.sm.state;
    v.anim = u.sm.anim.id;
    v.airborne = u.sm.airborne;
    attitude(id, v.roll, v.pitch);
    if (is_fighter(u)) v.fighter_phase = u.fs.phase;
    return v;
}

bool MovementSystem::attitude(ObjectId id, float& roll_deg, float& pitch_deg) const {
    roll_deg = 0.0f;
    pitch_deg = 0.0f;
    const int32_t i = find(id);
    if (i < 0) return false;
    const Unit& u = units_[static_cast<size_t>(i)];
    if (is_fighter(u)) {  // FIGHTER_FLYING §1.1
        roll_deg = fighter_wrap180(u.fs.b.o.roll);
        pitch_deg = fighter_wrap180(u.fs.b.o.pitch);
    } else {
        roll_deg = u.sm.lean;  // bike lean / flying roll O +0x84 (0 for the other locomotors)
        if (u.d.locomotor == LocomotorKind::Flying) pitch_deg = u.sm.pitch;  // flying pitch O +0x88 (§6.3)
    }
    return true;
}

std::span<const V2> MovementSystem::land_path(ObjectId id) const {
    const int32_t i = find(id);
    if (i < 0) return {};
    return units_[static_cast<size_t>(i)].m.path;
}

std::span<const SpaceWaypoint> MovementSystem::space_path(ObjectId id) const {
    const int32_t i = find(id);
    if (i < 0) return {};
    return units_[static_cast<size_t>(i)].wps;
}

uint32_t MovementSystem::mask_of(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? 0u : units_[static_cast<size_t>(i)].lp.mask;
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
    std::vector<uint32_t> idx;
    for (ObjectId id : order.units) {
        const int32_t i = find(id);
        if (i < 0) continue;
        if (std::find(idx.begin(), idx.end(), static_cast<uint32_t>(i)) != idx.end()) continue;
        idx.push_back(static_cast<uint32_t>(i));
    }
    if (idx.empty()) return;
    if (order.switch_locomotor)  // MOVEMENT §6.2: multi-locomotor units swap first (others ignore the bit)
        for (uint32_t i : idx) swap_locomotor(units_[i], true);
    std::vector<uint32_t> delays(idx.size(), 0);
    if (order.flags & kMoveFlagQueue)
        for (uint32_t& dl : delays) dl = static_cast<uint32_t>(rng.range_int(0, cfg_.constants.max_move_frame_delay));
    if (cfg_.mode == MoveMode::Land) move_land(order, idx, frame, delays, rng);
    else move_space(order, idx, frame, delays);
}

void MovementSystem::move_land(const MoveOrder& order, const std::vector<uint32_t>& idx, uint32_t frame,
                               const std::vector<uint32_t>& delays, SimRng& rng) {
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
    const bool queued = (order.flags & kMoveFlagQueue) != 0;
    uint32_t stagger_acc = 0;  // OI tactical: accumulated PathFrameDelay (§1.6)
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
            // OI tactical: PathFrameDelay stagger (0x43c1b0) for group moves; queued moves keep the per-unit delay.
            uint32_t fstart = frame;
            if (!queued) {
                fstart = frame + stagger_acc + static_cast<uint32_t>(rng.range_int(0, std::max(0, k.max_move_frame_delay)));
                for (uint32_t m : f) stagger_acc += static_cast<uint32_t>(std::max(0, units_[idx[m]].d.path_frame_delay));
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
                u.start_frame = queued ? frame + delays[s.member] : fstart;
                u.formation_speed = f.size() > 1 ? fspeed : kInf;
                u.in_formation = f.size() > 1;  // OI tactical
                u.jiggle = V2{};
                u.blocked_frames = 0;
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
                                const std::vector<uint32_t>& delays) {
    const MovementConstants& k = cfg_.constants;
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
                u.start_frame = frame + delays[s.member] + n * k.space_pathfind_frame_delay_delta;
                u.threat = order.threat_threshold;  // §4.2
                if (u.d.locomotor == LocomotorKind::Fighter) fighter_order(u);  // FIGHTER_FLYING §2 step 6
                ++n;
            }
        }
    }
}

void MovementSystem::stop(std::span<const ObjectId> ids) {
    for (ObjectId id : ids) {
        const int32_t i = find(id);
        if (i < 0) continue;
        Unit& u = units_[static_cast<size_t>(i)];
        if (is_air(u) && u.d.locomotor == LocomotorKind::LandBomber) continue;  // §4.6 [port]: a bombing pass runs out
        u.has_goal = false;
        u.need_path = false;
        u.m.path.clear();
        u.m.next = 0;
        u.m.speed = 0.0f;
        u.m.state = LocoState::WalkStopped;
        u.following = false;
        u.turning = false;
        u.wps.clear();
        if (cfg_.mode == MoveMode::Space) park_space(u, u.path_start);
        switch (u.d.locomotor) {  // §4.3-4.7 [port]: where a stop leaves the special locomotors
            case LocomotorKind::Jetpack: u.sm.state = u.sm.airborne ? LocoState::WalkEndMove : LocoState::WalkStopped; break;
            case LocomotorKind::Flying:
                u.sm.state = LocoState::FlyNoPath;
                u.sm.idle_frames = 0;
                u.sm.dest = u.m.pos;
                break;
            case LocomotorKind::Bike: u.sm.state = LocoState::BikePreStopped; break;
            case LocomotorKind::Fighter:
                u.goal = u.m.pos;
                if (is_fighter(u)) {
                    fighter_stop(u);
                } else {
                    u.sm.state = LocoState::FighterIdle;
                }
                break;
            default: break;
        }
    }
}

// L1 systems: Teleport (0x3a8f90): stop, place, re-register (collision tree / stationary space track).
bool MovementSystem::teleport(ObjectId id, V2 pos, float z, float yaw_deg, uint32_t frame) {
    const int32_t i = find(id);
    if (i < 0) return false;
    Unit& u = units_[static_cast<size_t>(i)];
    u.has_goal = false;
    u.need_path = false;
    u.arrived = false;
    u.m.path.clear();
    u.m.next = 0;
    u.m.speed = 0.0f;
    u.m.state = LocoState::WalkStopped;
    u.following = false;
    u.turning = false;
    u.wps.clear();
    u.blocked_frames = 0;
    u.hold_until = 0;
    u.nudge_frames = 0;
    u.m.pos = pos;
    u.m.yaw = wrap_deg(yaw_deg);
    u.z = z;
    if (u.d.locomotor != LocomotorKind::Default) {  // §4.3-4.7: idle where placed
        u.sm.anim = AnimPlay{};
        u.sm.dest = pos;
        u.goal = pos;
        if (u.d.locomotor == LocomotorKind::Jetpack) {
            u.sm.state = LocoState::WalkStopped;
            u.sm.airborne = false;
        } else if (u.d.locomotor == LocomotorKind::Flying) {
            u.sm.state = LocoState::FlyNoPath;
            u.sm.idle_frames = 0;
        } else if (u.d.locomotor == LocomotorKind::Bike) {
            u.sm.state = LocoState::BikeStopped;
        } else if (u.d.locomotor == LocomotorKind::Fighter) {
            u.sm.state = LocoState::FighterIdle;
            if (is_fighter(u)) {  // [port] placed at rest: V = 0, D = P, path dropped
                u.fs.b.p = {pos.x, pos.y, z};
                u.fs.b.v = F3{};
                u.fs.b.o.yaw = fighter_wrap360(yaw_deg);
                u.fs.dest = u.fs.b.p;
                u.fs.idx = 0;
                u.fs.hold = 0;
            }
        }
    }
    if (cfg_.mode == MoveMode::Land) update_tree(u);
    else park_space(u, frame);
    return true;
}

// ---------------------------------------------------------------- tick
void MovementSystem::tick(uint32_t frame, SimRng& rng, const ParallelFor& pf) {
    const uint32_t interval = cfg_.constants.dynamic_land_quota_reset_interval;
    if (interval < 2 || frame % interval == 0) quota_used_ = 0;
    crush_.clear();
    despawned_.clear();
    fevents_.clear();
    if (cfg_.mode == MoveMode::Land) tick_land(frame, rng, pf);  // jiggle draws (see header)
    else tick_space(frame, pf);
}

bool MovementSystem::set_owner(ObjectId id, int32_t owner) {
    const int32_t i = find(id);
    if (i < 0) return false;
    units_[static_cast<size_t>(i)].d.owner = owner;
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
    const int32_t i = find(id);
    if (i < 0 || !is_air(units_[static_cast<size_t>(i)])) return false;
    out = SpaceRequest{};
    fill_air_request(units_[static_cast<size_t>(i)], frame, out);
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
            return !a.has_goal && !b.has_goal &&
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
        } else if (!a.has_goal) {
            for (uint32_t bi : cand) {
                const Unit& b = units_[bi];
                if (b.has_goal || !collision_classes_interact(a.d.collision_class, b.d.collision_class)) continue;
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
                const int32_t i = find(static_cast<ObjectId>(h));
                if (i >= 0 && ground_collider(units_[static_cast<size_t>(i)])) ++n;
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

void MovementSystem::tick_land(uint32_t frame, SimRng& rng, const ParallelFor& pf) {
    rebuild_regions_if_dirty();
    tree_.maintain(static_cast<double>(frame) / 30.0);  // shape only; query results do not depend on it
    plan_land(frame, pf);
    detect_collisions(frame);  // S7: checks create collision objects
    drive_collisions(frame);   // S7: cancel / priority sort / resolve (TACTICAL_GAPS §6.7)
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
            if (!u.has_goal || u.need_path || frame < u.start_frame) {
                if (bike) bike_frame(u, yaw0, false);
                continue;
            }
            float cap = std::min(u.formation_speed, u.speed_cap);
            if (frame < u.hold_until) cap = 0.0f;
            WalkExtras x;  // OI tactical: formation members (jiggle, side-error steering)
            x.jiggle = u.jiggle;
            x.side_error = true;
            res[i] = walk_step(u.m, u.lp, cap, &grid_, u.in_formation ? &x : nullptr);
            if (bike) bike_frame(u, yaw0, true);  // §4.5: states 0x20-0x25 and the lean (writes only u)
        }
    });
    tick_special(frame, rng);  // §4.3, 4.4, 4.6: serial, ascending id
    for (size_t i = 0; i < units_.size(); ++i) {
        Unit& u = units_[i];
        if (is_air(u)) {
            update_tree(u);
            continue;
        }
        const WalkResult& r = res[i];
        if (r.arrived) {
            u.has_goal = false;
            u.arrived = true;
        }
        if (r.blocked) {
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
        h.f32(u.jiggle.x);  // OI tactical
        h.f32(u.jiggle.y);
        h.boolean(u.in_formation);
    }
    h.u32(static_cast<uint32_t>(crush_.size()));  // OI tactical
    for (const CrushEvent& e : crush_) {
        h.i32(e.crusher);
        h.i32(e.victim);
    }
    if (cfg_.mode == MoveMode::Land) coll_.hash(h);  // S7 collision layer (land only: the space format is unchanged)
    hash_special(h);  // §4.2-4.7: appended only when used (the format is unchanged otherwise)
    hash_fighters(h);  // FIGHTER_FLYING: appended only with space fighters / squadrons
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
    const int32_t i = find(id);
    if (i < 0) return false;
    Unit& u = units_[static_cast<size_t>(i)];
    if (!(factor >= 0.0f)) factor = 0.0f;  // NaN / negative -> stopped
    if (u.speed_factor == factor) return true;
    u.speed_factor = factor;
    configure(u);
    return true;
}

float MovementSystem::speed_factor(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? 1.0f : units_[static_cast<size_t>(i)].speed_factor;
}

bool MovementSystem::set_speed_override(ObjectId id, float speed) {
    const int32_t i = find(id);
    if (i < 0) return false;
    Unit& u = units_[static_cast<size_t>(i)];
    if (!(speed >= 0.0f)) speed = -1.0f;  // negative / NaN -> cleared
    if (u.speed_override == speed) return true;
    u.speed_override = speed;
    configure(u);
    return true;
}

float MovementSystem::speed_override(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? -1.0f : units_[static_cast<size_t>(i)].speed_override;
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
// max speed. [port] choices: avoid_speed() = minspeed and formation_speed_or_max() = maxspeed (the port has no space
// formation object: the 0x5ca430 / 0x5ca390 formation branches never apply); the squadron's "container path" is its
// leader's path; the land clamp §3.7 is not modelled (space only: it is the identity there); the modifier events
// (Out_Of_Combat_Defense_Adjustment, §4.1 step 1) belong to combat and are not raised here.
MovementSystem::Unit* MovementSystem::unit_ptr(ObjectId id) {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &units_[static_cast<size_t>(i)];
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

// "The formation of (C's pack, else the own pack) has an attack target" (0x41a2e0). [port] fallback: the legacy point
// target of set_attack_target (the leader's for a squadron).
bool MovementSystem::ftarget_get(Unit& u, FighterTarget& out) {
    const FighterTarget* t = ftarget_slot(u);
    if (t->set()) {
        out = *t;
        return true;
    }
    const Unit& l = fleader(u);
    if (!l.sm.has_target) return false;
    out = FighterTarget{};
    out.point = true;
    out.pos = {l.sm.target.x, l.sm.target.y, l.sm.target_z};
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
    u.need_path = false;
    u.has_goal = false;
    u.fs.idx = 0;
}

void MovementSystem::fleave(FighterCellGrid& g, Unit& u, bool dogfight) {
    if (FighterSquad* c = fsquad(u)) g.leave(c->id, dogfight ? c->dog : c->park);
    else g.leave(u.d.id, dogfight ? u.fs.dog : u.fs.park);
}

bool MovementSystem::fjoin(FighterCellGrid& g, Unit& u, FighterCell cell, bool dogfight) {
    const FighterAlliedFn allied = [this](int32_t a, int32_t b) { return fallied(a, b); };
    if (FighterSquad* c = fsquad(u)) return g.join(c->id, u.d.owner, cell, dogfight ? c->dog : c->park, allied);
    return g.join(u.d.id, u.d.owner, cell, dogfight ? u.fs.dog : u.fs.park, allied);
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
        f_steer(u, f3_add(away, p), k.min_speed, tb, thb, lb);  // avoid_speed()
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
    f_follow(u, u.lp.max_speed, u.d.layer_z);  // formation_speed_or_max(); path kind 1 is not produced by the port
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
        const float v = fighter_squad_idle(f.b.p, l.fs.b.p, d, leader, k.min_speed);
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
            if (!f_avoid(u, a, tb, thb, lb)) f_steer(u, a, k.max_speed, tb, thb, lb);  // formation_speed_or_max()
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
    const float v = mfd * mfd <= d2 ? f3_length(x.fs.b.v) : k.min_speed;  // match its speed, else avoid_speed()
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
                f_steer(u, aa, k.max_speed, tb, thb, lb);  // formation_speed_or_max()
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

// A unit (any kind) or squadron leaves: fighters drop it from their squadron, the grids and their locks; targets that
// name it become dying ([port]: the retail death signals, 0x220e90 / 0x220eb0).
void MovementSystem::fighter_removed(ObjectId id) {
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
    for (ObjectId m : s->members) unit_ptr(m)->fs.squad = desc.id;
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

void MovementSystem::build_fighter_grids() {
    fgrids_ = FighterCellGrids{};
    if (space_.has_box()) fgrids_.build(space_.box_min(), space_.box_max());
    for (Unit& u : units_) {
        u.fs.dog = FighterCell{};
        u.fs.park = FighterCell{};
    }
    for (FighterSquad& s : squads_) {
        s.dog = FighterCell{};
        s.park = FighterCell{};
    }
}

const FighterState* MovementSystem::fighter_state(ObjectId id) const {
    const int32_t i = find(id);
    if (i < 0) return nullptr;
    const Unit& u = units_[static_cast<size_t>(i)];
    return u.d.locomotor == LocomotorKind::Fighter ? &u.fs : nullptr;
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
    }
    h.u32(static_cast<uint32_t>(fevents_.size()));
    for (const FighterEvent& e : fevents_) {
        h.u8(static_cast<uint8_t>(e.kind));
        h.i32(e.who);
        h.i32(e.target);
    }
}

}  // namespace alamo::tactical::movement

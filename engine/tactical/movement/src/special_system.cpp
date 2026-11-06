// MovementSystem: special locomotors (docs/re/REMAINING_GAPS.md §4.2-4.7). See movement_system.hpp and
// special_locomotors.hpp for the contract.
#include <algorithm>

#include "alamo/tactical/movement/movement_system.hpp"

namespace alamo::tactical::movement {

bool MovementSystem::is_air(const Unit& u) const {
    if (cfg_.mode != MoveMode::Land) return false;
    const LocomotorKind k = u.d.locomotor;
    return k == LocomotorKind::Jetpack || k == LocomotorKind::Flying || k == LocomotorKind::LandBomber;
}

bool MovementSystem::ground_collider(const Unit& u) const {
    switch (u.d.locomotor) {
        case LocomotorKind::Flying:
        case LocomotorKind::LandBomber: return cfg_.mode != MoveMode::Land;
        case LocomotorKind::Jetpack:
            return !u.sm.airborne && u.sm.state != LocoState::WalkMoving && u.sm.state != LocoState::WalkEndMove;
        default: return true;
    }
}

void MovementSystem::special_idle(Unit& u) {
    u.sm = SpecialMotion{};
    const LocomotorKind k = u.d.locomotor;
    if (k == LocomotorKind::Default) return;
    u.sm.state = k == LocomotorKind::Bike      ? LocoState::BikeStopped
                 : k == LocomotorKind::Flying  ? LocoState::FlyNoPath
                 : k == LocomotorKind::Fighter ? LocoState::FighterIdle
                                               : LocoState::WalkStopped;
    u.sm.dest = u.m.pos;
    u.goal = u.m.pos;
    u.sm.airborne = k == LocomotorKind::Flying || k == LocomotorKind::LandBomber;
}

// S7 t06 / t14: the Uses_Multiple_Locomotors swap 0x3a9910 (MOVEMENT §3.1.1) - a set, not a toggle. [port] land only
// (every vanilla user is a land unit; the jetpack is land-only); JETPACK secondaries only (UnitDesc::
// has_secondary_locomotor), so "on the secondary" (retail: the active locomotor is a JetPack, vf96 = 0) is on_secondary.
// Engine audio loops off (STATION_HANGARS_SOUND §3.3) and the weapon behaviours (step 6) are the host's.
bool MovementSystem::swap_locomotor(Unit& u, bool secondary) {
    if (cfg_.mode != MoveMode::Land || !u.d.uses_multiple_locomotors || !u.d.has_secondary_locomotor) return false;
    if (u.on_secondary == secondary) return true;  // step 2: already there
    // S8 t12 (MOVEMENT §3.2 "A locomotor swap undeploys"): the Walk is replaced, so its deploy state goes with it - JetPack
    // answers "not deployed" (vf +0x120 0x13b7d0), and the Walk rebuilt on landing starts in 0x16. No signal.
    u.dclip = kAnimNone;
    deploy_set(u, LocoState::None, false);
    // Both directions: the new locomotor is built and attached; the path stack's content belongs to the old one
    // ([port] the jetpack's timed path is not a land path and the reverse, so a kept goal is re-planned).
    u.m.path.clear();
    u.m.next = 0;
    u.m.escape_until = 0;
    u.m.speed = 0.0f;
    u.m.state = LocoState::WalkStopped;
    u.following = false;
    arrival_reset(u);  // s78l t02
    u.wps.clear();
    u.path_lag = 0.0f;
    u.on_secondary = secondary;
    if (secondary) {
        // Step 4, before the locomotor changes: the unit leaves its coordinator and Move_To re-issues its current order
        // block to it alone (no coordinator: a move to its own position, i.e. hold). [port] no stagger draw for the
        // one-unit Move_To ([M] in the spec); a pending start frame is kept.
        u.in_formation = false;
        u.jiggle = V2{};
        u.formation_speed = 3.0e38f;  // alone
        u.large_hover_index = -1;
        u.speed_cap = 3.0e38f;
        u.hold_until = 0;
        u.nudge_frames = 0;
        end_push(u);  // a1k t03 [port]
        u.blocked_frames = 0;
        u.replans = 0;
        u.waits = 0;
        if (u.has_goal) {
            u.need_path = true;
        } else {
            u.need_path = false;
            u.goal = u.m.pos;
        }
        // Steps 5-6: JetPack Initialise (0x640ef0): conform / commit at the current position, pack state := 0 (None).
        u.d.locomotor = u.d.secondary_locomotor;
        u.sm = SpecialMotion{};
        u.sm.state = LocoState::None;
        u.sm.dest = u.goal;
    } else {
        // Step 7: the order is left alone (goal, facing, formation, start frame); [port] a kept goal is re-planned as a
        // land path. Walk Initialise (0x61dea0): conform / commit at the current position, pack state := 0x16.
        u.d.locomotor = u.primary_locomotor;
        const V2 goal = u.goal;
        special_idle(u);  // the primary's stopped state (Walk 0x16; a Bike primary: 0x25)
        u.goal = goal;
        if (u.has_goal) {
            u.need_path = true;
            u.sm.dest = goal;
        }
    }
    if (height_) u.z = terrain(u.m.pos);  // conform
    return true;
}

bool MovementSystem::jet_alternates(const Unit& u) const {
    return cfg_.mode == MoveMode::Land && u.d.locomotor == LocomotorKind::Jetpack && locomotor_airborne_kind(u.d.locomotor, u.sm.state);
}

float MovementSystem::loco_max_speed(const Unit& u) const {
    if (!jet_alternates(u)) return u.lp.max_speed;
    // 0x371060: the §3.4 chain 0x376990 on Alternate_Max_Speed (no team-minimum cap); [port] the speed override
    // replaces the base as it does for Max_Speed (configure).
    const float base = u.speed_override >= 0.0f ? u.speed_override : u.d.alt_max_speed;
    return std::max(base, 0.0f) * cfg_.constants.land_speed_multiplier * u.speed_factor;
}

float MovementSystem::loco_turn_rate(const Unit& u) const {
    if (!jet_alternates(u)) return u.lp.turn_rate;
    return std::max(u.d.alt_turn_rate, 0.0f) * cfg_.constants.land_speed_multiplier;  // 0x370fd0
}

bool MovementSystem::use_secondary_locomotor(ObjectId id, bool secondary) {
    const int32_t i = find(id);
    return i >= 0 && swap_locomotor(units_[static_cast<size_t>(i)], secondary);
}

ThreatFn MovementSystem::owner_threat(const Unit& u) const {
    if (!threat_) return {};
    const int32_t owner = u.d.owner;
    return [this, owner](V2 p) { return threat_(owner, p); };
}

void MovementSystem::anim_play(Unit& u, int32_t anim, bool loop) const {
    AnimPlay a;
    a.id = anim;
    a.loop = loop;
    a.length = anim_len_ ? anim_len_(u.d.id, anim) : 0u;
    u.sm.anim = a;
}

void MovementSystem::on_air_path(Unit& u, uint32_t frame) {
    u.path_start = frame;
    u.path_lag = 0.0f;
    u.sm.clock = 0.0f;
    switch (u.d.locomotor) {
        case LocomotorKind::Jetpack:  // §4.3: airborne -> keep flying; on the ground -> 0 -> 0xF (turn, jump)
            if (u.sm.airborne) {
                u.sm.state = LocoState::WalkMoving;
                u.following = true;
            } else {
                u.sm.state = LocoState::None;
                u.following = false;
            }
            break;
        case LocomotorKind::Flying:  // §4.4: 0 -> 0x13
            u.sm.state = LocoState::WalkMoving;
            u.sm.circling = false;
            u.sm.circle_target = -1;
            u.sm.path_idx = 0;
            u.following = true;
            break;
        case LocomotorKind::LandBomber:  // §4.6: the pass
            u.sm.state = LocoState::WalkMoving;
            u.following = true;
            break;
        default: break;
    }
}

SpacePose MovementSystem::air_follow(Unit& u, uint32_t frame) const {
    u.path_lag += u.speed_factor - 1.0f;  // S10-int: playback runs at the speed factor (0 at factor 1)
    const float t = static_cast<float>(frame - u.path_start + 1) + u.path_lag;
    const SpacePose p = eval_space_path(u.wps, t);
    u.m.pos = p.pos;
    u.m.yaw = wrap_deg(p.heading);
    u.m.speed = p.speed;
    return p;
}

float MovementSystem::air_altitude(const Unit& u, uint32_t frame) const {
    const float t0 = static_cast<float>(frame - u.path_start + 1) + u.path_lag;
    const bool on_path = u.following && !u.wps.empty();
    const V2 fwd = dir_from_deg(u.m.yaw) * u.m.speed;
    const TrackFn track = [&](uint32_t i) {
        return on_path ? eval_space_path(u.wps, t0 + static_cast<float>(i)).pos : u.m.pos + fwd * static_cast<float>(i);
    };
    const TerrainFn ter = [this](V2 p) { return terrain(p); };
    const InsideFn inside = [this](V2 p) { return air_.inside_box(p); };
    const float target = jetpack_altitude_target(track, ter, inside, u.sp.hover_offset, map_floor_, u.z);
    return step_altitude(u.z, target, u.sp.max_lift, terrain(u.m.pos), map_floor_);
}

bool MovementSystem::land_spot_free(const Unit& u, V2 p) const {
    if (!grid_.passable_world(p, u.lp.mask)) return false;  // movement-class test (0x50e280)
    const float r = std::max(u.d.radius, 1.0f);
    const tactical::Aabb q{{p.x - r - 80.0f, p.y - r - 80.0f, -1.0f}, {p.x + r + 80.0f, p.y + r + 80.0f, 1.0f}};
    for (EntityHandle h : tree_.query_aabb(q, QueryFilter{}, qctx_)) {  // occupancy (0x538200) [port: ground units]
        const int32_t oi = find(static_cast<ObjectId>(h));
        if (oi < 0) continue;
        const Unit& o = units_[static_cast<size_t>(oi)];
        if (o.d.id == u.d.id || !ground_collider(o)) continue;
        const float rr = r + o.d.radius;
        if (length_sq(o.m.pos - p) < rr * rr) return false;
    }
    return true;
}

// ---------------------------------------------------------------- §4.3 JETPACK_LOCOMOTOR (vf6 0x641910)
void MovementSystem::step_jetpack(Unit& u, uint32_t frame) {
    SpecialMotion& s = u.sm;
    // §3.1.1: the take-off turn (0x10, 0x642740) turns at pack +0xe8 (the band rate) when > 0, else R = Max_Rate_Of_Turn x
    // the mode multiplier, never the alternate turn rate.
    const float rate = u.lp.turn_rate;
    bool stopped = false;  // in 0x16 at the end of this service (0x16 itself, or 0x13 no path / 0x14 re-dispatching)
    auto aim_heading = [&]() {
        V2 aim = u.wps.empty() ? u.goal : u.wps.back().pos;
        for (const SpaceWaypoint& w : u.wps)
            if (!(w.pos == u.m.pos)) {
                aim = w.pos;
                break;
            }
        const V2 d = aim - u.m.pos;
        return length_sq(d) > 0.0f ? heading_deg(d) : u.m.yaw;
    };
    switch (s.state) {
        case LocoState::None:  // vf6 case 0: no current path -> 0x16 and return (the swap back is one service later)
            if (u.has_goal && !u.need_path && !u.wps.empty()) s.state = LocoState::WalkStartTurn;
            else if (!u.has_goal) s.state = LocoState::WalkStopped;
            break;
        case LocoState::WalkStartTurn: {  // 0xF
            const float err = wrap_deg(aim_heading() - u.m.yaw);
            s.turn_left = err > 0.0f;
            if (u.sp.has_pre_turn_anim && std::fabs(err) > 0.0f)
                anim_play(u, err > 0.0f && err < 180.0f ? kAnimTurnLeftBegin : kAnimTurnRightBegin, false);
            s.turn_band_rate = 0.0f;  // S7 t15: 0xF clears +0xe8 right before 0x10 (the first turn frame turns at R)
            s.state = LocoState::WalkTurning;
            break;
        }
        case LocoState::WalkTurning: {  // 0x10 (0x642740), S7 t15 (MOVEMENT §3.1.1 "Take-off turn")
            // 1-2. r = +0xe8 when > 0, else R; turn by min(|e|, r); r_left = r - |e| when |e| <= r, else 0.
            const float desired = aim_heading();
            const float r = s.turn_band_rate > 0.0f ? s.turn_band_rate : rate;
            const float e0 = wrap_deg(desired - u.m.yaw);
            const float r_left = std::fabs(e0) <= r ? r - std::fabs(e0) : 0.0f;
            if (e0 != 0.0f) s.turn_left = e0 > 0.0f;
            u.m.yaw = turn_toward(u.m.yaw, desired, r);
            // 3. c = the animation is in its last 2 frames (port: frames + 2 >= length; a clip of unknown length is always
            // ending); the port's path is ready whenever 0x10 runs (0xF needs it), so "no path or path +0x6d" holds and
            // step 4 never fires.
            const bool c = s.anim.loop ? false : s.anim.frames + 2u >= s.anim.length;
            const bool turn_anim = s.anim.id == kAnimTurnLeft || s.anim.id == kAnimTurnRight || s.anim.id == kAnimTurnLeftHalf ||
                                   s.anim.id == kAnimTurnRightHalf || s.anim.id == kAnimTurnLeftQuarter ||
                                   s.anim.id == kAnimTurnRightQuarter;
            if (turn_anim && !c) break;  // 5. keep turning at the band rate until the clip ends
            if (r_left <= 0.0f) {        // 6. not aligned this frame: pick a turn animation
                if (u.sp.has_pre_turn_anim) {
                    // Band on the error left after this frame's turn: the band value is the yaw rate of the next frames.
                    const float e = wrap_deg(desired - u.m.yaw);
                    const TurnAnim ta = jetpack_turn_anim(e, rate, true);
                    if (ta.anim == kAnimNone) {  // |e| < R: no animation, +0xe8 := R, End Turn
                        s.turn_band_rate = rate;
                        s.state = LocoState::WalkEndTurn;
                        break;
                    }
                    anim_play(u, ta.anim, false);
                    s.anim.rate = 1.0f;  // [M] the runtime constant speed DAT_140b39958 (presentation only)
                    s.turn_band_rate = ta.rate;
                } else if (!turn_anim && anim_len_ && anim_len_(u.d.id, kAnimTurnLeft) > 0u) {
                    // Without Has_Pre_Turn_Anim: anim 3 / 4 by the sign of the frame-start error, only when anim 3 exists
                    // and no turn animation plays; +0xe8 is not written.
                    const TurnAnim ta = jetpack_turn_anim(e0, rate, false);
                    anim_play(u, ta.anim, false);
                    s.anim.rate = ta.rate;
                }
                break;
            }
            s.state = LocoState::WalkEndTurn;  // 7.
            break;
        }
        case LocoState::WalkEndTurn:  // 0x11 (0x640b30)
            if (u.sp.has_pre_turn_anim) {
                const int32_t end = s.turn_left ? kAnimTurnLeftEnd : kAnimTurnRightEnd;
                if (!s.anim.playing(end)) anim_play(u, end, false);
                if (!s.anim.done()) break;
            }
            s.state = LocoState::WalkStartMove;
            break;
        case LocoState::WalkStartMove:  // 0x12 (0x642530): Jump, then shift the path to now and fly
            if (!s.anim.playing(kAnimJump)) anim_play(u, kAnimJump, false);
            if (!s.anim.done()) break;
            u.path_start = frame;  // every waypoint time += now - first waypoint time
            u.path_lag = 0.0f;
            u.following = true;
            anim_play(u, kAnimFlyIdle, true);
            s.airborne = true;
            s.state = LocoState::WalkMoving;
            break;
        case LocoState::WalkMoving: {  // 0x13 (0x6420e0)
            if (!u.following || u.wps.size() < 2) {  // no current path: 0x16, swap back in this service
                s.state = LocoState::WalkStopped;
                stopped = true;
                break;
            }
            const V2 pos0 = u.m.pos;
            const float yaw0 = u.m.yaw;
            const SpacePose p = air_follow(u, frame);
            u.z = air_altitude(u, frame);
            const float now = static_cast<float>(frame - u.path_start + 1) + u.path_lag;
            if (!s.anim.playing(kAnimFlyLand) && !u.need_path && u.wps.back().time - now <= kJetpackLandingFrames) {
                const V2 last = u.wps.back().pos;
                if (land_spot_free(u, last)) {
                    anim_play(u, kAnimFlyLand, false);
                    s.airborne = false;
                } else {
                    // Re-issue a move to the same point (0x640880, radius 50) [port: nearest free cell centre within
                    // 50 of it, ring scan order; the point itself when none].
                    V2 best = last;
                    float bd = 3.0e38f;
                    const int32_t cx = world_to_cell(last.x), cy = world_to_cell(last.y);
                    const auto rings = static_cast<int32_t>(kJetpackRelandRadius / kCellSize) + 1;
                    for (int32_t y = cy - rings; y <= cy + rings; ++y)
                        for (int32_t x = cx - rings; x <= cx + rings; ++x) {
                            const V2 c = cell_center(x, y);
                            const float d = distance(last, c);
                            if (d <= kJetpackRelandRadius && d < bd && land_spot_free(u, c)) {
                                bd = d;
                                best = c;
                            }
                        }
                    u.goal = best;
                    u.need_path = true;
                    u.start_frame = frame + 1;
                }
            }
            if (p.done) s.state = LocoState::WalkEndMove;
            // §3.1.1: a frame whose follower step left position and orientation unchanged -> 0x16 (the path is not done);
            // the swap back follows one service later, possibly in the air [M whether it occurs in play]. [port] not at
            // speed factor 0, the port's stand-in for a suspended locomotor (stun / immobilise: retail skips the service).
            else if (u.m.pos == pos0 && u.m.yaw == yaw0 && u.speed_factor > 0.0f) {
                s.state = LocoState::WalkStopped;
                ++stats_.jetpack_still_frames;
            }
            break;
        }
        case LocoState::WalkEndMove: {  // 0x14 (0x641df0): landing sequence
            u.m.speed = 0.0f;
            u.following = false;
            const float ground = std::max(terrain(u.m.pos), map_floor_);
            if (s.anim.playing(kAnimFlyLand)) {
                if (s.anim.done()) anim_play(u, kAnimFlyLandIdle, true);
            } else if (s.anim.playing(kAnimFlyLandIdle)) {
                u.z = std::max(ground, u.z - u.sp.max_lift);  // descend by at most Max_Lift
                if (std::fabs(u.z - ground) <= 0.01f) anim_play(u, kAnimFlyLandDrop, false);
            } else if (s.anim.playing(kAnimFlyLandDrop)) {
                if (s.anim.done()) {
                    s.state = LocoState::WalkStopped;
                    s.airborne = false;
                    s.anim = AnimPlay{};
                    u.z = terrain(u.m.pos);  // snap to the terrain
                    u.has_goal = false;
                    u.arrived = true;
                    u.wps.clear();
                    stopped = true;  // FlyLandDrop finished: 0x16 and the swap back in this service
                }
            } else {
                anim_play(u, kAnimFlyLand, false);  // FlyIdle (or nothing) playing -> FlyLand
                s.airborne = false;
            }
            break;
        }
        case LocoState::WalkStopped:  // 0x16 (every service): speed 0, idle, then the swap back
        default:
            u.m.speed = 0.0f;
            if (height_) u.z = terrain(u.m.pos);
            stopped = s.state == LocoState::WalkStopped;
            break;
    }
    // S7 t14 (§3.1.1): JetPack vf6 in state 0x16 calls 0x3a9910(obj, system, 1) with no other condition. [port] the
    // need_path guard: not while a re-plan is pending (the port plans in plan_land after the order, so a re-issued move
    // - the re-land move 0x640880, a host order - has no path yet).
    if (stopped && u.on_secondary && !u.need_path) swap_locomotor(u, false);
}

// ---------------------------------------------------------------- FLYING_LOCOMOTOR (vf6 0x63d7f0; FIGHTER_FLYING §6)
// Unit mapping: P = (m.pos, z), O = (sm.lean roll, sm.pitch, m.yaw), V = (sm.vel, sm.vel_z), s = m.speed (pack +0xec).
// The formation destination is sm.dest (z = the terrain there [port]: orders carry planar points).
void MovementSystem::step_flying(Unit& u, uint32_t frame, SimRng& rng) {
    SpecialMotion& s = u.sm;
    FlyRates r;
    r.turn = u.lp.turn_rate;
    r.roll = u.sp.roll_rate;
    r.lift = u.sp.max_lift;
    r.bank = u.sp.bank_turn_angle;
    r.max_speed = u.lp.max_speed;
    r.hover_offset = u.sp.hover_offset;
    r.prepare_strafe_height = u.sp.prepare_strafe_height;
    FlyWorld w;
    w.terrain = [this](V2 p) { return terrain(p); };
    w.has_box = air_.has_box();
    w.box_min = air_.box_min();
    w.box_max = air_.box_max();
    w.floor = map_floor_;
    const ObjectId id = u.d.id;
    if (fly_hooks_.clearance)
        w.clearance = [this, id](FlyVec3 a, FlyVec3 b, float m) { return fly_hooks_.clearance(id, a, b, m); };
    FlyBody b;
    b.pos = {u.m.pos.x, u.m.pos.y, u.z};
    b.o = {s.lean, s.pitch, u.m.yaw};
    b.vel = {s.vel.x, s.vel.y, s.vel_z};
    b.speed = u.m.speed;
    auto store = [&]() {
        u.m.pos = {b.pos.x, b.pos.y};
        u.z = b.pos.z;
        s.lean = b.o.roll;
        s.pitch = b.o.pitch;
        u.m.yaw = b.o.yaw;
        s.vel = {b.vel.x, b.vel.y};
        s.vel_z = b.vel.z;
        u.m.speed = b.speed;
    };
    const FlyVec3 dest{s.dest.x, s.dest.y, terrain(s.dest)};
    if (s.state == LocoState::None) s.state = LocoState::WalkMoving;  // 0 -> 0x13 ([M] runs 0x13 this frame)
    switch (s.state) {
        case LocoState::WalkMoving: {  // 0x13: timed follower (§6.1), then altitude (§6.3) with the tracker
            if (!u.following || u.wps.empty()) {
                s.state = LocoState::FlyNoPath;
                s.idle_frames = 0;
                break;
            }
            u.path_lag += u.speed_factor - 1.0f;  // S10-int: playback runs at the speed factor (0 at factor 1)
            const float T = static_cast<float>(frame - u.path_start + 1) + u.path_lag;
            const FlyFollow fo = flying_follow(u.wps, s.path_idx, T, b, r);
            FlyVec3 p1 = fo.pos;
            FlyOrient o1 = fo.o;
            if (fly_hooks_.track) {
                w.track = [this, id, frame](uint32_t i, FlyVec3& p, V2& f) { return fly_hooks_.track(id, frame + i, p, f); };
            } else {  // [port] the unit's own reservation (linear in time), z = the current height
                const float zt = p1.z;
                w.track = [&u, T, zt](uint32_t i, FlyVec3& p, V2& f) {
                    return flying_track_linear(u.wps, T + static_cast<float>(i), zt, p, f);
                };
            }
            flying_altitude_step(p1, o1, fo.speed, true, &dest, r, w);
            if (!(p1 == b.pos)) b.vel = p1 - b.pos;
            b.pos = p1;
            b.o = o1;
            b.speed = fo.speed;
            store();
            if (fo.complete) {  // pop the path (0x557b10): no path left -> 0x27
                s.state = LocoState::FlyNoPath;
                s.idle_frames = 0;
                u.following = false;
                u.has_goal = false;
                u.arrived = true;
            }
            break;
        }
        case LocoState::WalkStopped: break;  // 0x16: nothing
        case LocoState::FlyCircling: {       // 0x26 (0x63db30)
            FlyVec3 c;
            const FlyVec3* cp = nullptr;
            if (s.circle_target >= 0) {  // the target object: a mover of this system, else the host
                const int32_t ti = find(s.circle_target);
                if (ti >= 0) {
                    const Unit& t = units_[static_cast<size_t>(ti)];
                    c = {t.m.pos.x, t.m.pos.y, t.z};
                    cp = &c;
                } else if (fly_hooks_.object_position && fly_hooks_.object_position(s.circle_target, c)) {
                    cp = &c;
                }
            } else if (s.circling) {  // [port] circle(): a fixed point
                c = {s.circle_c.x, s.circle_c.y, s.circle_z};
                cp = &c;
            }
            flying_circle_step(b, cp, s.circle_rmin, s.circle_rmax, r, w);
            store();
            break;
        }
        case LocoState::FlyNoPath: {  // 0x27 (0x63e2b0): loiter
            flying_loiter_move(b, &dest, r, w);
            store();
            // Re-issue when the formation is not busy and has been idle for > 2 FPS + 1 frames ([port]: a frame
            // counter for the +0x1dc stamp; signal 10 on +0x240 is not modelled [M]).
            if (++s.idle_frames > kLoiterIdleFrames && !u.need_path) {
                V2 point;
                if (s.has_target) {
                    point = flying_strafe_aim(u.m.pos, s.target, u.m.yaw, s.target_radius, u.sp.strafe_distance,
                                              s.turn_back);
                } else {
                    const float L = kLoiterDistanceCoefficient * cfg_.constants.xy_expansion_distance_land;
                    // [port] the issued move (0x41fd30 kind 4) does not replace the formation destination sm.dest
                    // (the destination kinds of 0x583680 are open), so the loiter stays around the ordered point.
                    point = flying_loiter_reissue(u.m.pos, s.dest, u.m.yaw, L, rng, s.turn_back);
                }
                u.goal = point;
                u.has_goal = true;
                u.need_path = true;
                u.arrived = false;
                u.start_frame = frame + 1;
                u.formation_speed = u.sp.min_speed;  // 0x4205a0 (the air planner caps its plan speed with it)
                s.idle_frames = 0;
            }
            break;
        }
        default: break;  // other states: the driver returns an error (no movement)
    }
}

// ---------------------------------------------------------------- §4.6 LANDBOMBER_LOCOMOTOR (vf6 0x646800)
void MovementSystem::step_bomber(Unit& u) {
    SpecialMotion& s = u.sm;
    if (s.state != LocoState::WalkMoving || !u.following) return;  // 0x16: waiting for the pass
    s.clock += kLandBomberClockStep * u.speed_factor;
    const SpacePose p = eval_space_path(u.wps, s.clock);
    u.m.pos = p.pos;
    u.m.yaw = wrap_deg(p.heading);
    u.m.speed = p.speed;
    u.z = s.pass_z;
    if (p.done) {  // killed silently (0x391310 flag 1)
        u.following = false;
        u.has_goal = false;
        despawned_.push_back(u.d.id);
    }
}

void MovementSystem::tick_special(uint32_t frame, SimRng& rng) {
    for (Unit& u : units_) {
        if (!is_air(u)) continue;
        if (u.sm.anim.id != kAnimNone) ++u.sm.anim.frames;
        switch (u.d.locomotor) {
            case LocomotorKind::Jetpack: step_jetpack(u, frame); break;
            case LocomotorKind::Flying: step_flying(u, frame, rng); break;
            case LocomotorKind::LandBomber: step_bomber(u); break;
            default: break;
        }
    }
}

// ---------------------------------------------------------------- §4.5 BIKE_LOCOMOTOR (vf6 0x639e90)
void MovementSystem::bike_frame(Unit& u, float yaw0, bool walked) const {
    SpecialMotion& s = u.sm;
    if (s.anim.id != kAnimNone) ++s.anim.frames;
    const float dy = wrap_deg(u.m.yaw - yaw0);
    const int32_t turn = dy > 0.0f ? kAnimTurnLeft : (dy < 0.0f ? kAnimTurnRight : kAnimNone);
    s.lean = bike_lean(s.lean, turn, u.sp.roll_rate, u.sp.bank_turn_angle);
    const LocoState prev = s.state;
    const bool was_moving =
        prev == LocoState::BikeStartMove || prev == LocoState::BikeMoving || prev == LocoState::BikeEndMove;
    if (walked && u.m.state == LocoState::WalkStartTurn) {
        s.state = LocoState::BikeTurningInPlace;
    } else if (walked && u.m.speed > 0.0f && u.m.state != LocoState::WalkStopped) {
        switch (prev) {
            case LocoState::BikeStartMove:
                if (u.m.state == LocoState::WalkEndMove) {
                    s.state = LocoState::BikeEndMove;
                    anim_play(u, kAnimMoveEnd, false);
                } else if (s.anim.done()) {
                    s.state = LocoState::BikeMoving;
                    anim_play(u, kAnimMove, true);
                }
                break;
            case LocoState::BikeMoving:
                if (u.m.state == LocoState::WalkEndMove) {
                    s.state = LocoState::BikeEndMove;
                    anim_play(u, kAnimMoveEnd, false);
                }
                break;
            case LocoState::BikeEndMove:
                if (u.m.speed <= u.sp.min_speed + 0.0001f) s.state = LocoState::BikePreStopped;
                break;
            default:
                s.state = LocoState::BikeStartMove;
                anim_play(u, kAnimMoveStart, false);
                break;
        }
    } else if (was_moving) {
        s.state = LocoState::BikePreStopped;  // 0x24 (0x63a8e0)
    } else if (prev == LocoState::BikePreStopped || prev == LocoState::None ||
               (prev == LocoState::BikeTurningInPlace && !u.has_goal)) {
        s.state = LocoState::BikeStopped;
        s.anim = AnimPlay{};
    }
}

// ---------------------------------------------------------------- API
bool MovementSystem::set_attack_target(ObjectId id, bool has, V2 pos, float z) {
    const int32_t i = find(id);
    if (i < 0) return false;
    SpecialMotion& s = units_[static_cast<size_t>(i)].sm;
    s.has_target = has;
    s.target = has ? pos : V2{};
    s.target_z = has ? z : 0.0f;
    return true;
}

bool MovementSystem::circle(ObjectId id, V2 centre, float z, float rmin, float rmax) {
    const int32_t i = find(id);
    if (i < 0) return false;
    Unit& u = units_[static_cast<size_t>(i)];
    if (u.d.locomotor != LocomotorKind::Flying || cfg_.mode != MoveMode::Land) return false;
    u.sm.state = LocoState::FlyCircling;
    u.sm.circling = true;
    u.sm.circle_target = -1;
    u.sm.circle_c = centre;
    u.sm.circle_z = z;
    u.sm.circle_rmin = rmin;
    u.sm.circle_rmax = std::max(rmin, rmax);
    u.following = false;
    u.wps.clear();
    u.has_goal = false;
    u.need_path = false;
    return true;
}

// FIGHTER_FLYING §6.5: Start_Circling (vf +0x278 = 0x63bf20), called by CableAttackAbilityClass (0x6f34d0) with the
// cable target and the ability record's +0x178 / +0x17c (Activation_Min_Range / Activation_Max_Range [M]).
bool MovementSystem::start_circling(ObjectId id, ObjectId target, float rmin, float rmax) {
    const int32_t i = find(id);
    if (i < 0 || target < 0 || !(rmin > 0.0f) || !(rmax > 0.0f)) return false;  // a target and both radii > 0
    Unit& u = units_[static_cast<size_t>(i)];
    if (u.d.locomotor != LocomotorKind::Flying || cfg_.mode != MoveMode::Land) return false;
    u.sm.state = LocoState::FlyCircling;
    u.sm.circling = false;
    u.sm.circle_target = target;
    u.sm.circle_rmin = rmin;
    u.sm.circle_rmax = rmax;
    u.following = false;  // [port] as circle(): the order is dropped (how 0x26 is left is untraced)
    u.wps.clear();
    u.has_goal = false;
    u.need_path = false;
    return true;
}

float MovementSystem::clearance_query(ObjectId flyer, FlyVec3 from, FlyVec3 to, float M, const ClearanceInfoFn& info) const {
    if (!info) return M;
    ClearanceInfo fi;
    if (!info(flyer, fi)) return M;
    const V2 a{from.x, from.y}, b{to.x, to.y};
    const float qx = fi.half.x, qy = fi.half.y;
    for (const Unit& o : units_) {  // ascending id
        ClearanceInfo ci;
        if (!info(o.d.id, ci)) continue;
        const bool moving = o.m.speed > 0.0f || o.has_goal;
        const uint32_t layer = object_tracking_layer(ci.uses_multiple_locomotors, locomotor_airborne_kind(o.d.locomotor, o.sm.state),
                                                     moving, o.d.space_layer_type, ci.stationary_space_layer);
        if ((layer & kLayerCorvette) != kLayerCorvette) continue;
        ClearanceEntry e;
        e.s = e.e = o.m.pos;
        e.facing = dir_from_deg(o.m.yaw);
        e.ex = ci.half.x;
        e.ey = ci.half.y;
        if (!clearance_candidate(a, b, qx, qy, e) || !clearance_hit(a, b, qx, qy, e, 0.0f)) continue;
        if (!ci.has_box) continue;  // no model: no value
        const float v = clearance_top(ci.box_lo_z, ci.box_hi_z, ci.scale_factor, o.z);
        if (v >= M) M = v;
    }
    return M;
}

void MovementSystem::hash_special(StateHasher& h) const {
    uint32_t count = 0;
    const auto listed = [](const Unit& u) {
        return u.d.locomotor != LocomotorKind::Default || u.threat != kThreatOff || u.on_secondary;
    };
    for (const Unit& u : units_)
        if (listed(u)) ++count;
    if (count == 0 && despawned_.empty()) return;  // unchanged format for scenarios without §4.2-4.7 state
    h.u32(count);
    for (const Unit& u : units_) {
        if (!listed(u)) continue;
        const SpecialMotion& s = u.sm;
        h.i32(u.d.id);
        h.u8(static_cast<uint8_t>(u.d.locomotor));
        if (u.on_secondary) h.u8(0xA5);  // S7 t06: secondary locomotor active (only then: other formats unchanged)
        h.f32(u.threat);
        h.u8(static_cast<uint8_t>(s.state));
        h.i32(s.anim.id);
        h.u32(s.anim.frames);
        h.u32(s.anim.length);
        h.boolean(s.anim.loop);
        h.boolean(s.airborne);
        h.boolean(s.turn_left);
        if (s.turn_band_rate != 0.0f) h.f32(s.turn_band_rate);  // S7 t15: only during a Has_Pre_Turn_Anim take-off turn
        h.f32(s.lean);
        h.u32(s.idle_frames);
        h.boolean(s.turn_back);
        h.f32(s.dest.x);
        h.f32(s.dest.y);
        h.boolean(s.circling);
        h.boolean(s.has_target);
        h.f32(s.clock);
        h.f32(s.pass_z);
        h.f32(u.path_lag);
        if (u.d.locomotor == LocomotorKind::Flying) {  // FIGHTER_FLYING §6 (flying units only: other formats unchanged)
            h.f32(s.pitch);
            h.f32(s.vel.x);
            h.f32(s.vel.y);
            h.f32(s.vel_z);
            h.u32(s.path_idx);
            h.i32(s.circle_target);
        }
    }
    h.u32(static_cast<uint32_t>(despawned_.size()));
    for (ObjectId id : despawned_) h.i32(id);
}

}  // namespace alamo::tactical::movement

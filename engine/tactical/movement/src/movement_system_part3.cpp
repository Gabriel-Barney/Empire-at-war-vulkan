// movement_system: part 3 (bo3 t03 split; shared code in movement_system_internal.hpp).
#include "movement_system_internal.hpp"

namespace alamo::tactical::movement {
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
                q.speed = std::min(u.lp.max_speed, u.cohesion_speed);  // [port] fleet cohesion cap (3e38: none)
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
                plan_space_path(space_, k, q, sc, out[r], &pf);  // sp2: retries in parallel, same result
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
        if (u.d.no_locomotor) continue;  // s78n t20 batch 1: the host's object keeps its own transform
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
    // SIMP sp6: tree hash, blocks of kCrcBlockItems hashed in parallel (same digest for any worker count)
    hash_blocks(h, static_cast<uint32_t>(units_.size()), [&](StateHasher& bh, uint32_t blk_first, uint32_t blk_last) {
        for (uint32_t blk_i = blk_first; blk_i < blk_last; ++blk_i) {
            const Unit& u = units_[blk_i];
            bh.i32(u.d.id);
            bh.f32(u.m.pos.x);
            bh.f32(u.m.pos.y);
            bh.f32(u.z);
            bh.f32(u.m.yaw);
            bh.f32(u.m.speed);
            bh.u8(static_cast<uint8_t>(u.m.state));
            bh.u32(u.m.next);
            bh.u32(static_cast<uint32_t>(u.m.path.size()));
            for (const V2& p : u.m.path) {
                bh.f32(p.x);
                bh.f32(p.y);
            }
            bh.boolean(u.has_goal);
            bh.boolean(u.need_path);
            bh.boolean(u.arrived);
            bh.f32(u.goal.x);
            bh.f32(u.goal.y);
            bh.u32(u.start_frame);
            bh.u32(u.pend);            // cu8j t01
            bh.boolean(u.adopted);     // cu8j t02
            // s78n t24: Unit::blocked_frames / replans are gone (the blocked-frames re-plan); their constants keep the format
            bh.u32(0);
            bh.u32(0);
            bh.u32(u.path_change);  // s78n t18 (in the place of the dropped Unit::waits; 0 in space: the format is unchanged)
            bh.i32(u.check_countdown);
            // s78n t23: Unit::speed_cap / hold_until / nudge_frames are gone (the MovingVsMoving stand-in); their constants
            // keep the hash format (the space goldens do not move)
            bh.f32(3.0e38f);
            bh.u32(0);
            bh.u32(0);
            bh.boolean(u.following);
            bh.u32(u.path_start);
            bh.u32(static_cast<uint32_t>(u.wps.size()));
            for (const SpaceWaypoint& w : u.wps) {
                bh.f32(w.pos.x);
                bh.f32(w.pos.y);
                bh.f32(w.heading);
                bh.f32(w.time);
            }
            bh.boolean(u.turning);
            if (u.aturn != LocoState::None) {  // s78l t02: the arrival turn state, hashed only while a turn runs
                bh.u8(static_cast<uint8_t>(u.aturn));
                if (u.aturn_start) bh.u8(0xE9);  // s78n t26.2: a start turn in place
                bh.f32(u.aturn_rate);
                bh.i32(u.tclip);
                bh.u32(u.tclip_start);
                bh.f32(u.tclip_speed);
            }
            bh.f32(u.jiggle.x);  // OI tactical
            bh.f32(u.jiggle.y);
            bh.boolean(u.in_formation);
            if (u.d.team_container) {  // s78n t03: appended only for squad containers and their members
                bh.u8(0xC7);
                bh.boolean(u.laid_out);  // s78n t14
                bh.f32(u.lay_dir.x);
                bh.f32(u.lay_dir.y);
                bh.boolean(u.spread);
                bh.boolean(u.team_target);
                bh.boolean(u.coast);  // s78n t18
                bh.boolean(u.fturn);
                bh.boolean(u.path_started);
            }
            if (u.anchor >= 0) {
                bh.i32(u.anchor);
                bh.f32(u.tslot.x);
                bh.f32(u.tslot.y);
                bh.u32(u.tindex);
                bh.u8(u.recovery);  // s78n t14: the recovery state (was a bool)
                bh.u32(u.start_delay);  // s78n t14
            }
            if (u.exit_host >= 0) {  // s78n t18: the garrison-host gate of a unit that left, only while set
                bh.u8(0xE7);
                bh.i32(u.exit_host);
            }
            if (u.d.no_locomotor) {  // s78n t20 batch 1: an object without a locomotor (a death clone), only for such a unit
                bh.u8(0xD1);
                bh.boolean(u.d.death_clone_obstacle);
            }
            if (!u.m.frac.empty() || u.m.turn_done) {  // s78n t26.2: the order path's fractions / turn-done byte (land only)
                bh.u8(0xEA);
                bh.boolean(u.m.turn_done);
                bh.u32(static_cast<uint32_t>(u.m.frac.size()));
                for (const float f : u.m.frac) bh.f32(f);
            }
            // s78n t26 closing review (land only): a Walk_Transition unit's clip state, only while it is not the default
            if (u.m.wt_clip != -1 || u.m.wt_pos != 0.0f || u.m.wt_rate != 0.0f || u.m.wt_f4 != 0.0f || u.m.wt_f0 != 0.0f) {
                bh.u8(0x35);
                bh.i32(u.m.wt_clip);
                bh.f32(u.m.wt_pos);
                bh.f32(u.m.wt_rate);
                bh.f32(u.m.wt_f4);
                bh.f32(u.m.wt_f0);
            }
            // s78n t26.4 (land only): the coast vector, the previous tick's position and an unstarted order path
            if (u.m.last_dir.x != 0.0f || u.m.last_dir.y != 0.0f || u.prev_pos.x != 0.0f || u.prev_pos.y != 0.0f || !u.m.started) {
                bh.u8(0xEB);
                bh.f32(u.m.last_dir.x);
                bh.f32(u.m.last_dir.y);
                bh.f32(u.prev_pos.x);
                bh.f32(u.prev_pos.y);
                bh.boolean(u.m.started);
            }
            if (u.hover != 0 || u.hover_off != 0.0f) {  // s78n t18: the hover state, only once it left the ground
                bh.u8(0xE8);
                bh.u8(u.hover);
                bh.f32(u.hover_off);
            }
            if (!u.around.empty()) {  // s78n t18: the top path's id list (decision step 3), only while any
                bh.u8(0xD7);
                bh.u32(u.around_frame);
                bh.u32(static_cast<uint32_t>(u.around.size()));
                for (const ObjectId id : u.around) bh.i32(id);
            }
            if (u.form_id != 0 || u.self_arrive) {  // s78n t20: the formation of the last land order (land only)
                bh.u8(0xF7);
                bh.u32(u.form_id);
                bh.f32(u.form_slot.x);
                bh.f32(u.form_slot.y);
                bh.boolean(u.self_arrive);
                if (u.self_arrive) {  // s78n t20 F5
                    bh.f32(u.self_goal.x);
                    bh.f32(u.self_goal.y);
                }
            }
            if (!u.lane.empty()) {  // s78p: a lane the next plan takes
                bh.u8(0x1A);
                bh.u32(static_cast<uint32_t>(u.lane.size()));
                for (const V2 p : u.lane) {
                    bh.f32(p.x);
                    bh.f32(p.y);
                }
                bh.f32(u.lane_d);
                if (u.lane_z != 0.0f) bh.f32(u.lane_z);  // bkm1 t06
            }
            if (u.lp.secondary) {  // bkm1 t08 (land only): the secondary yaw of a UseSecondaryFacing unit
                bh.u8(0xED);
                bh.f32(u.m.yaw2);
            }
            if (u.m.end_z != 0.0f) {  // bkm1 t06 (land only): a stored last-waypoint height
                bh.u8(0xEC);
                bh.f32(u.m.end_z);
                bh.f32(u.m.end_at.x);
                bh.f32(u.m.end_at.y);
            }
            if (u.atk_rho > 0.0f || u.atk_form != 0) {  // s78n t25.2: the attack block of a land attack order (land only)
                bh.u8(0xA5);
                bh.f32(u.atk_theta);
                bh.f32(u.atk_rho);
                bh.u32(u.atk_form);
                bh.i32(u.atk_target);
            }
            if (u.pushed) {  // a1k t03: hashed only while a collision-owned record is on top of the stack
                if (u.push_flee) bh.u8(0x07);  // s78n t14 part B: a crush-flee path (kind 7)
                bh.u32(u.push_next);
                bh.u32(u.push_turn_done ? 1u : 0u);  // s78n t26.2: the record's turn-done byte (in the place of push_turn_first)
                bh.u32(static_cast<uint32_t>(u.push_path.size()));
                for (const V2& p : u.push_path) {
                    bh.f32(p.x);
                    bh.f32(p.y);
                }
                bh.u32(static_cast<uint32_t>(u.push_frac.size()));  // s78n t26.2: the record's speed fractions
                for (const float f : u.push_frac) bh.f32(f);
                bh.u8(u.push_kind);  // s78n t20: the record's kind, owner, weight, resume index and hold tags
                bh.u8(u.push_weight);
                bh.u32(u.push_owner);
                bh.u32(u.push_resume);
                bh.u32(static_cast<uint32_t>(u.push_tags.size()));
                for (const HoldTag& t : u.push_tags) {
                    bh.u32(t.index);
                    bh.i32(t.obstacle);
                    bh.f32(t.pt.x);
                    bh.f32(t.pt.y);
                    bh.f32(t.dir.x);
                    bh.f32(t.dir.y);
                }
            }
        }
    });
    h.u32(static_cast<uint32_t>(crush_.size()));  // OI tactical
    for (const CrushEvent& e : crush_) {
        h.i32(e.crusher);
        h.i32(e.victim);
    }
    if (!push_kills_.empty()) {  // s78n t20 batch 1: the death clones a push killed this tick, only while any
        h.u8(0xD2);
        h.u32(static_cast<uint32_t>(push_kills_.size()));
        for (const ObjectId id : push_kills_) h.i32(id);
    }
    if (!deploy_kept_.empty()) {  // s78n t26.1 review: the kept deploy states of garrisoned units, only while any exists
        h.u8(0xDC);
        h.u32(static_cast<uint32_t>(deploy_kept_.size()));
        for (const DeployKeep& k : deploy_kept_) {
            h.i32(k.id);
            h.u8(static_cast<uint8_t>(k.deploy));
            h.u8(k.walk_deployed ? 1 : 0);
            h.i32(k.dclip);
            h.f32(k.dclip_pos);
        }
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
// 0xf / 0x10), else it keeps its arrival yaw. [port] Walk locomotor (LocomotorKind::Default) only. The threshold is 10
// degrees, or R when R <= 10 and the current waypoint's cap is 0 (s78n t26.2: cap_zero reads the path's fractions; the
// waypoint behind an arrived unit is the last, (1, 1), so an order arrival uses 10). A move without a final facing ends on the arrival heading (0x61d570's 1e18 triple:
// no turn). The turn itself starts on the next tick (arrival_turn_frame).
void MovementSystem::land_arrival_turn(Unit& u) const {
    arrival_reset(u);
    if (u.final_facing == kNoFacing || is_air(u)) return;
    if (u.d.team_container) {
        // s78n t23 step 0 (t21 review item 3, FIGHTER_FLYING §7.7 item 6): a squad container that arrives outside its own
        // 0x15 (an early-arrival cut, a popped record past its end) runs the same final-facing test: the turn in place
        // (team_walk, after any braking coast)
        if (std::fabs(wrap_deg(land_wanted_yaw(u) - u.m.yaw)) > arrival_turn_threshold(u.lp.turn_rate, cap_zero(u))) u.fturn = true;
        return;
    }
    if (u.d.locomotor != LocomotorKind::Default) return;
    const float want = land_wanted_yaw(u);
    if (std::fabs(wrap_deg(want - u.m.yaw)) <= arrival_turn_threshold(u.lp.turn_rate, cap_zero(u))) return;
    u.turning = true;
    u.turn_to = want;
    u.aturn = LocoState::WalkStartTurn;
}

// bkm3 review (MOVEMENT §3.5.1 item 1 / step 4): an early arrival (the cut 0x684100, the MvM early pop) that starts the
// arrival turn turns from rest - retail coasts in End Move to 0x15 first, whose entry zeroes the speed (+0xec), so 0x10
// runs at Stopped_Rate_Of_Turn; the port starts the turn at once, so the speed is zeroed here as the popped arrival does.
void MovementSystem::arrival_turn_rest(Unit& u) {
    if (u.aturn != LocoState::None && !u.d.team_container) u.m.speed = 0.0f;
}

void MovementSystem::arrival_reset(Unit& u) {
    if (u.aturn != LocoState::None) u.aturn_end_pending = true;  // a running turn dropped: reported next tick
    u.turning = false;
    u.aturn = LocoState::None;
    u.aturn_start = false;  // s78n t26.2
    u.aturn_rate = 0.0f;
    u.tclip = kAnimNone;
    u.tplay_set = false;
}

// s78n t26.2 (MOVEMENT §3.5.2 A): the start turn's wanted yaw - the direction to the current waypoint of the top path the
// walk would take this frame (the collision-owned record, else the order path; a formation member's order path with its
// jiggle, as walk_step aims).
bool MovementSystem::start_turn_want(const Unit& u, uint32_t frame, const WalkExtras& x, float& want) const {
    if (u.deploy != LocoState::None && !deploy_walks(u)) return false;
    std::span<const V2> path;
    uint32_t next = 0;
    if (u.pushed) {
        path = u.push_path;
        next = u.push_next;
    } else if (u.has_goal && !u.need_path && frame >= u.start_frame && u.m.started) {
        path = u.m.path;
        next = u.m.next;
    } else {
        return false;
    }
    V2 dir;
    if (!walk_wanted_dir(path, next, u.m.pos, &x, dir)) return false;
    want = length_sq(dir) > 0.0f ? heading_deg(dir) : u.m.yaw;
    return true;
}

bool MovementSystem::cap_zero(const Unit& u) {
    if (u.pushed) {
        const size_t i = u.push_next >= 1 ? u.push_next - 1u : 0u;
        return u.push_next >= 1 && i < u.push_frac.size() && i < u.push_path.size() && u.push_frac[i] == 0.0f;
    }
    return u.m.next >= 1 && wp_frac(u.m, u.m.next - 1) == 0.0f;
}

void MovementSystem::start_turn_mark(Unit& u, const WalkResult& r) {
    if (!r.turn) return;
    if (u.d.locomotor != LocomotorKind::Default) {
        u.m.yaw = turn_toward(u.m.yaw, r.want, u.lp.turn_rate);
        return;
    }
    u.aturn = LocoState::WalkStartTurn;
    u.aturn_start = true;
    u.aturn_rate = 0.0f;
    u.turn_to = r.want;
    u.turning = true;
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
// turn clip); the band threshold T is 10 (ordinary path). bkm1 t08 (b0050, MOVEMENT §3.5.3 items 2 / 5): with
// UseSecondaryFacing, 0x10 also steps Y2 toward the same w at r x SecondaryTurnInPlaceROTCoefficient, and the arrival turn
// (not a start turn) is reached when Y2 lands with rate left; 0x15's re-test reads the primary yaw only.
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
                // bkm3 t03 (b0052 item 1, §3.5.1 step 4): pack +0xe8 when > 0; else Stopped_Rate_Of_Turn only at speed
                // <= 0, else R (a start turn entered moving: the stale v after an undeploy)
                const float r = u.aturn_rate > 0.0f ? u.aturn_rate : (u.m.speed > 0.0f ? u.lp.turn_rate : s);
                const float e0 = wrap_deg(u.turn_to - u.m.yaw);
                const float ae0 = std::fabs(e0);
                u.m.yaw = turn_toward(u.m.yaw, u.turn_to, r);  // 0x61e730: lands exactly when |e| <= r
                bool reached = ae0 > r ? false : r - ae0 > 0.0f;
                if (u.lp.secondary) {
                    const float r2 = r * u.d.secondary_in_place;
                    const float ae2 = std::fabs(wrap_deg(u.turn_to - u.m.yaw2));
                    u.m.yaw2 = turn_toward(u.m.yaw2, u.turn_to, r2);
                    if (!u.aturn_start) reached = ae2 > r2 ? false : r2 - ae2 > 0.0f;  // the last waypoint: Y2's leftover
                }
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
                // 0x6204f0: the band from the error after this frame's step; its rate applies from the next frame. s78n t26.2
                // (MOVEMENT §3.5.1 item 4): T = S on a start turn whose path is kind 3 ([port] the TerrainCollision detour,
                // the only record with fractions) or whose current waypoint cap is 0, else 10
                const bool tight = u.aturn_start && ((u.pushed && !u.push_frac.empty()) || cap_zero(u));
                const TurnAnim band = arrival_turn_band(wrap_deg(u.turn_to - u.m.yaw), s, tight ? s : 10.0f, a);
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
                if (u.aturn_start) {
                    // s78n t26.2 (MOVEMENT §3.5.2 A): the start turn ends in Start Move 0x12 (then 0x13 and the move step,
                    // run by the caller in the same tick)
                    u.aturn = LocoState::None;
                    u.aturn_start = false;
                    u.aturn_rate = 0.0f;
                    u.turning = false;
                    u.m.state = LocoState::WalkStartMove;
                    return;
                }
                if (std::fabs(wrap_deg(u.turn_to - u.m.yaw)) > arrival_turn_threshold(u.lp.turn_rate, cap_zero(u))) {
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
        if (u.cohesion_speed < kInf) {  // [port] fleet cohesion: only while capped (loose moves hash as before)
            h.u8(0x43);
            h.f32(u.cohesion_speed);
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

namespace bo3split {

void hash_f3(StateHasher& h, F3 v) {
    h.f32(v.x);
    h.f32(v.y);
    h.f32(v.z);
}
}  // namespace bo3split

namespace bo3split {
void hash_att(StateHasher& h, FAttitude o) {
    h.f32(o.roll);
    h.f32(o.pitch);
    h.f32(o.yaw);
}
}  // namespace bo3split

namespace bo3split {
void hash_cell(StateHasher& h, FighterCell c) {
    h.i32(c.i);
    h.i32(c.j);
}
}  // namespace bo3split

namespace bo3split {
void hash_target(StateHasher& h, const FighterTarget& t) {
    h.i32(t.id);
    h.boolean(t.point);
    h.i32(t.hard_point);
    hash_f3(h, t.pos);
    h.f32(t.radius);
    h.boolean(t.dying);
    h.boolean(t.hidden);
}
}  // namespace bo3split

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

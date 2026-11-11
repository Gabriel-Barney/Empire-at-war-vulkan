// MovementSystem: the Walk locomotor's deploy states (docs/re/MOVEMENT.md §3.2 "Deploy states", "A locomotor swap
// undeploys"; S8 t11 / t12). Contract: movement_system.hpp ("S8 t11 / t12").
#include <algorithm>
#include <cmath>

#include "alamo/core/dmath.hpp"

#include "alamo/tactical/movement/movement_system.hpp"

namespace alamo::tactical::movement {

namespace {
constexpr int32_t kAnimDeploy = 0x19;    // ModelAnimType 25 (Deploy)
constexpr int32_t kAnimUndeploy = 0x1A;  // 26 (Undeploy)

// 0x386170: every component equal within 0.0001, negative values wrapped by +360 first.
bool turret_angles_equal(const float* a, const float* b) {
    const auto w = [](float v) { return v < 0.0f ? v + 360.0f : v; };
    for (size_t k = 0; k < 3; ++k)
        if (std::fabs(w(a[k]) - w(b[k])) > 0.0001f) return false;
    return true;
}
}  // namespace

bool MovementSystem::turret_at_rest(const Unit& u) const {
    if (turret_rest_) return turret_rest_(u.d.id);
    // S8 t24: always against the UNDEPLOYED Turret_Rest_Angle, also in 0x18 (MOVEMENT §3.2 "At rest").
    return !u.d.turret || turret_angles_equal(u.tcur, u.d.turret_rest);
}

const float* MovementSystem::turret_rest_angle(const Unit& u) const {
    // Turret_Deployed_Rest_Angle when the type Deploys (only a Deploys type reaches these states) and the unit is deployed (0x39a390: state
    // 0x19 or the walking-deployed flag, which keeps 0x19 here: s78n t26.1) or Deploying (0x39a400: 0x18); else Turret_Rest_Angle.
    const bool dep = u.deploy == LocoState::WalkDeploying || u.deploy == LocoState::WalkDeployed;
    return dep ? u.d.turret_deployed_rest : u.d.turret_rest;
}

void MovementSystem::turret_recentre(Unit& u, const float* rest) {
    // 0x6319d0(turret, 0): not aiming, target := R, idle time 0 (the 2-s delay is skipped). The hold byte is untouched: when
    // it was clear, the next service still freezes the target for that frame [M].
    if (!u.d.turret) return;
    u.taim = false;
    for (size_t k = 0; k < 3; ++k) u.ttgt[k] = rest[k];
    u.tidle = 0.0f;
}

void MovementSystem::tick_turrets(uint32_t frame) {
    // TURRET service 0x632270 + motion 0x631210, every frame (behaviour interval 1). [port] after the Walk deploy pass.
    const float now = static_cast<float>(frame) / 30.0f;  // seconds (frame / FPS; the sim runs at 30)
    if (cfg_.mode != MoveMode::Land) return;  // s78f t08: [port] the space TURRET aim stays unmodelled (aimed at once)
    for (Unit& u : units_) {
        if (!u.d.turret) continue;
        const float* rest = turret_rest_angle(u);
        // Aim (0x632050): the host's aim point. [inferred] Nothing aims during a deploy transition (0x18 / 0x1A: the
        // weapon holds fire, kUnitDeployTransition), so the forced recentre of the deploy pass is not undone here.
        const bool transition = u.deploy == LocoState::WalkDeploying || u.deploy == LocoState::WalkUndeploying;
        float ax = 0.0f, ay = 0.0f, az = 0.0f;
        if (!transition && turret_aim_ && turret_aim_(u.d.id, ax, ay, az)) {
            // [inferred] target angles relative to the body: yaw toward the point (atan2 convention of the unit yaw),
            // elevation the geometric one with up negative (LOB_SOLVER §3.6 sign).
            const float dx = ax - u.m.pos.x, dy = ay - u.m.pos.y, dz = az - u.z;
            if (dx != 0.0f || dy != 0.0f) u.ttgt[2] = wrap_deg(dmath::atan2(dy, dx) * (180.0f / dmath::kPiF) - u.m.yaw);
            u.ttgt[1] = -dmath::atan2(dz, std::sqrt(dx * dx + dy * dy)) * (180.0f / dmath::kPiF);
            u.taim = true;
        } else {
            u.taim = false;
        }
        if (!u.taim) {  // idle return
            if (!turret_angles_equal(u.tcur, rest)) {
                if (!u.thold) {  // first idle frame: hold where it is
                    u.thold = true;
                    for (size_t k = 0; k < 3; ++k) u.ttgt[k] = u.tcur[k];
                    u.tidle = now;
                } else if (now - u.tidle > u.d.turret_rest_delay) {
                    for (size_t k = 0; k < 3; ++k) u.ttgt[k] = rest[k];
                    u.tidle = 0.0f;
                }
            } else {
                u.thold = false;
            }
        }
        // Motion: at most Turret_Rotate_Speed degrees per frame (no frame-time scaling); x never moves.
        const float s = u.d.turret_speed;
        const float de = u.ttgt[1] - u.tcur[1];
        u.tcur[1] = std::fabs(de) <= s ? u.ttgt[1] : u.tcur[1] + (de > 0.0f ? s : -s);
        const float dyaw = wrap_deg(u.ttgt[2] - u.tcur[2]);
        const float yaw = wrap_deg(std::fabs(dyaw) <= s ? u.ttgt[2] : u.tcur[2] + (dyaw > 0.0f ? s : -s));
        const bool deployed = u.deploy == LocoState::WalkDeploying || u.deploy == LocoState::WalkDeployed;
        const float ext = deployed ? u.d.turret_deployed_extent : u.d.turret_extent;
        u.tcur[2] = std::clamp(yaw, -ext, ext);
    }
}

MovementSystem::DeployClip MovementSystem::deploy_clip(const Unit& u, int32_t anim) const {
    // ABILITIES_HARDPOINTS §4.13 convention: a clip advances fps / 30 clip frames per sim frame (the sim animation's
    // clock) times the play speed, here Deployment_Anim_Rate. No clip data: count 0, rate 1.
    DeployClip c;
    const float rate = u.d.deployment_anim_rate;
    uint32_t n = 0;
    float fps = 0.0f;
    if (deploy_clip_ && deploy_clip_(u.d.id, anim, n, fps) && fps > 0.0f) {
        c.count = static_cast<float>(n);
        c.step = fps / 30.0f * rate;
    } else {
        c.step = rate;
    }
    return c;
}

float MovementSystem::deploy_clip_pos(const Unit& u, uint32_t frame) const {
    if (u.dclip < 0) return 0.0f;
    const DeployClip c = deploy_clip(u, u.dclip);
    return u.dclip_pos0 + static_cast<float>(frame - u.dclip_start) * c.step;
}

void MovementSystem::deploy_start_clip(Unit& u, int32_t anim, float pos0, uint32_t frame) {
    u.dclip = anim;
    u.dclip_start = frame;
    u.dclip_pos0 = pos0;
}

void MovementSystem::deploy_begin_clip(Unit& u, int32_t anim, uint32_t frame) {
    if (u.dclip == anim) return;  // already the current clip: it keeps running
    const DeployClip c = deploy_clip(u, anim);
    float pos0 = 0.0f;
    if (u.dclip >= 0) {
        // Reversing the other clip (cur = its position, clamped to its frames): count - cur - 1, and one step less out of
        // the Deploy clip into the Undeploy clip; min 0.
        const DeployClip o = deploy_clip(u, u.dclip);
        const float cur = std::clamp(deploy_clip_pos(u, frame), 0.0f, std::max(o.count - 1.0f, 0.0f));
        pos0 = std::max(c.count - cur - 1.0f - (anim == kAnimUndeploy ? c.step : 0.0f), 0.0f);
    }
    deploy_start_clip(u, anim, pos0, frame);
}

void MovementSystem::deploy_set(Unit& u, LocoState to, bool finished) {
    if (u.deploy == to && !finished) return;
    DeployEvent e;
    e.id = u.d.id;
    e.from = u.deploy;
    e.to = to;
    e.finished = finished;
    devents_.push_back(e);
    u.deploy = to;
}

MovementSystem::DeployResult MovementSystem::deploy(ObjectId id, bool on, uint32_t frame) {
    const int32_t i = find(id);
    if (i < 0 || cfg_.mode != MoveMode::Land) return DeployResult::NoWalk;
    Unit& u = units_[static_cast<size_t>(i)];
    if (u.d.locomotor != LocomotorKind::Default) return DeployResult::NoWalk;  // JetPack / Bike / ...: 0x1fa90
    if (on) {
        // Deploy(true) 0x61d300: nothing in 0x18 / 0x19 or with the walking-deployed flag +0x2b3 set (s78n t26.1; [port]
        // obj +0x3a1 & 0x40 not modelled).
        if (u.deploy == LocoState::WalkDeploying || u.deploy == LocoState::WalkDeployed || u.walk_deployed) return DeployResult::Ignored;
        if (u.deploy == LocoState::WalkUndeploying && !turret_at_rest(u)) {
            // Out of 0x1A with the turret off rest: straight to 0x19, the turret reset, signal 0x29 at once. [inferred]
            // the reset is the forced recentre 0x6319d0 (toward the deployed rest now).
            u.dclip = kAnimNone;
            deploy_set(u, LocoState::WalkDeployed, true);
            turret_recentre(u, turret_rest_angle(u));
            return DeployResult::ReverseBlocked;
        }
        const ObjectId ids[] = {id};
        stop(ids);  // vf +0x140(0)
        deploy_set(u, LocoState::WalkDeploying, false);
        // s78n t26.1 (MOVEMENT §5.3 "Hover in full"): a rising / lowering hover unit runs no Walk state; its Deploy clip is
        // started by the 0x18 pass once it is down.
        if (u.hover >= 2) {
            u.dclip = kAnimNone;
            return DeployResult::Deploying;
        }
        // S8 t24 (MOVEMENT §3.2 "Where it gates"): the Deploy clip starts at once, with no turret wait (from 0 / 0x16; out
        // of 0x1A the turret is at rest here); reversing out of the Undeploy clip starts at count - cur - 1 (min 0).
        deploy_begin_clip(u, kAnimDeploy, frame);
        return DeployResult::Deploying;
    }
    // Deploy(false): 0x18 / 0x19 / the walking-deployed flag -> 0x1A ([port] the slot +0x35b blocker 0x42e3b0 / 0x42e3d0 is
    // not modelled). The Undeploy clip starts in the per-frame pass (turret wait). s78n t26.1 (MOVEMENT §3.2.1 item 3): only
    // the state is written - no stop, no speed or path change; a unit walking deployed freezes where it is (the walk does
    // not run in 0x1A) with its speed kept.
    if (u.deploy != LocoState::WalkDeploying && u.deploy != LocoState::WalkDeployed && !u.walk_deployed) return DeployResult::Ignored;
    deploy_set(u, LocoState::WalkUndeploying, false);
    return DeployResult::Undeploying;
}

// ---- s78n t26.1 (MOVEMENT §3.2.1): walking while deployed ----

float MovementSystem::path_length_3d(std::span<const V2> path) const {
    // 0x54f610: the summed 3-D length of every segment ([port] the land path is 2-D; the heights come from the height hook
    // at its points, 0 without one).
    float l = 0.0f;
    for (size_t k = 1; k < path.size(); ++k) {
        const V2 a = path[k - 1], b = path[k];
        const float dz = height_ ? height_(b.x, b.y) - height_(a.x, a.y) : 0.0f;
        const float dx = b.x - a.x, dy = b.y - a.y;
        l += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    return l;
}

void MovementSystem::deploy_cap_unit(Unit& u, float d, uint32_t frame) {
    // 0x3ac100(x, d): x `Deploys` with a Walk locomotor. Deployed (D) or deploying, d >= 0, M > 0 and d < M -> the flag
    // only (no state change, no clip); every other case -> Deploy(false).
    if (cfg_.mode != MoveMode::Land || u.d.locomotor != LocomotorKind::Default || !u.d.deploys) return;
    const bool dep = deployed_d(u) || u.deploy == LocoState::WalkDeploying;
    if (dep && d >= 0.0f && u.d.max_walk_deployed > 0.0f && d < u.d.max_walk_deployed) {
        u.walk_deployed = true;
        return;
    }
    (void)deploy(u.d.id, false, frame);
}

void MovementSystem::deploy_cap(ObjectId id, float d, uint32_t frame) {
    if (Unit* u = unit_ptr(id)) deploy_cap_unit(*u, d, frame);
}

void MovementSystem::deploy_path_start(Unit& u, float remaining, uint32_t frame) {
    // Walk vf +0x148 0x61e570 on a new top path: outside 0x18 / 0x19 / 0x1A with the flag clear -> the plain start (the
    // port's walk starts it). Otherwise L (the remaining length of the top path) < M -> the flag and the plain start
    // 0x42ced0, which acts in 0x19 when M > 0 (idle, 0x42c890): state 0 [port: m.state None; the walk runs on the next
    // tick]; else Deploy(false).
    if (cfg_.mode != MoveMode::Land || u.d.locomotor != LocomotorKind::Default || !u.d.deploys) return;
    if (u.deploy == LocoState::None && !u.walk_deployed) return;
    if (u.deploy == LocoState::WalkUndeploying) return;  // Deploy(false) changes nothing in 0x1A
    if (u.d.max_walk_deployed > 0.0f && remaining < u.d.max_walk_deployed) {
        u.walk_deployed = true;
        if (u.deploy == LocoState::WalkDeployed) u.m.state = LocoState::None;
        return;
    }
    (void)deploy(u.d.id, false, frame);
}

bool MovementSystem::deploy_walks(const Unit& u) const {
    // §3.2.1 item 3: a 0x19 unit with M > 0 walks a top path (a started order path or a collision record) it holds.
    return u.deploy == LocoState::WalkDeployed && u.d.max_walk_deployed > 0.0f && u.d.locomotor == LocomotorKind::Default &&
           cfg_.mode == MoveMode::Land;
}

void MovementSystem::damage_deploy(ObjectId id, uint32_t frame) {
    // Take_Damage 0x3a9e30: M > 0, Walk state 0x16, `Deploys`, the flag clear -> Deploy(true).
    Unit* u = unit_ptr(id);
    if (!u || cfg_.mode != MoveMode::Land || u->d.locomotor != LocomotorKind::Default || !u->d.deploys) return;
    if (!(u->d.max_walk_deployed > 0.0f) || u->walk_deployed || u->deploy != LocoState::None) return;
    if (u->has_goal || u->need_path || u->pushed || u->aturn != LocoState::None) return;
    if (u->m.state != LocoState::WalkStopped && u->m.state != LocoState::None) return;
    (void)deploy(id, true, frame);
}

bool MovementSystem::move_skips(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u && cfg_.mode == MoveMode::Land && deployed_d(*u) && !auto_deployer(*u);
}

bool MovementSystem::garrison_remove(ObjectId id, uint32_t frame) {
    const Unit* u = unit_ptr(id);
    if (!u) return false;
    std::erase_if(deploy_kept_, [id](const DeployKeep& k) { return k.id == id; });
    if (cfg_.mode == MoveMode::Land && u->d.locomotor == LocomotorKind::Default && u->d.deploys &&
        (u->deploy != LocoState::None || u->walk_deployed)) {
        DeployKeep k;
        k.id = id;
        k.deploy = u->deploy;
        k.walk_deployed = u->walk_deployed;
        k.dclip = u->dclip;
        k.dclip_pos = u->dclip >= 0 ? deploy_clip_pos(*u, frame) : 0.0f;
        const auto it = std::lower_bound(deploy_kept_.begin(), deploy_kept_.end(), id,
                                         [](const DeployKeep& e, ObjectId v) { return e.id < v; });
        deploy_kept_.insert(it, k);
    }
    if (!park_team_member(id)) (void)remove_impl(id);
    return true;
}

bool MovementSystem::restore_deploy(ObjectId id, uint32_t frame) {
    const auto it = std::lower_bound(deploy_kept_.begin(), deploy_kept_.end(), id,
                                     [](const DeployKeep& e, ObjectId v) { return e.id < v; });
    if (it == deploy_kept_.end() || it->id != id) return false;
    const DeployKeep k = *it;
    deploy_kept_.erase(it);
    Unit* u = unit_ptr(id);
    if (!u || u->d.locomotor != LocomotorKind::Default) return false;
    // The state as it was at the entry (no deploy event: the host's ability / combat flags never changed); a clip that
    // was timing resumes at its kept position ([port] retail's hidden unit keeps running its locomotor inside [M]).
    u->deploy = k.deploy;
    u->walk_deployed = k.walk_deployed;
    u->dclip = k.dclip;
    u->dclip_start = frame;
    u->dclip_pos0 = k.dclip >= 0 ? k.dclip_pos : 0.0f;
    return true;
}

bool MovementSystem::deploy_kept(ObjectId id) const {
    const auto it = std::lower_bound(deploy_kept_.begin(), deploy_kept_.end(), id,
                                     [](const DeployKeep& e, ObjectId v) { return e.id < v; });
    return it != deploy_kept_.end() && it->id == id;
}

bool MovementSystem::walking_deployed(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u && u->walk_deployed;
}

LocoState MovementSystem::deploy_state(ObjectId id) const {
    const Unit* u = unit_ptr(id);
    return u ? u->deploy : LocoState::None;
}

std::vector<MovementSystem::DeployEvent> MovementSystem::take_deploy_events() {
    std::vector<DeployEvent> out;
    out.swap(devents_);
    return out;
}

bool MovementSystem::snap_yaw(ObjectId id, float yaw_deg) {
    Unit* u = unit_ptr(id);
    if (!u) return false;
    u->m.yaw = wrap_deg(yaw_deg);
    if (is_fighter(*u)) u->fs.b.o.yaw = fighter_wrap360(yaw_deg);
    return true;
}

void MovementSystem::tick_deploy(uint32_t frame) {
    // Walk vf6 0x61e930, states 0x16 (s78n t26.1), 0x18 / 0x1A (serial, ascending id; each unit writes only itself and the
    // event list).
    for (Unit& u : units_) {
        if (u.d.locomotor != LocomotorKind::Default) continue;
        // s78n t26.1 (MOVEMENT §5.3 "Hover in full"): a rising / lowering unit runs no Walk state at all (an HTT that
        // deploys as it lowers times its Deploy clip only once it is down).
        if (u.hover >= 2) continue;
        if (u.deploy == LocoState::WalkUndeploying) u.walk_deployed = false;  // §3.2.1 item 3: 0x1A clears the flag
        // s78n t26.1 (§3.2.1 item 3): 0x16, idle with no path (after the arrival pop or a stop; the arrival turn first).
        // The flag set -> clear it, 0x19 at once with the Deploy clip on its last frame, no signal 0x29 (no event);
        // else a `Deploys` type with M > 0 and no DEPLOY ability deploys (Deploy(true)): the droid deploys at every stop.
        const bool idle = !u.has_goal && !u.need_path && !u.pushed && u.aturn == LocoState::None && u.anchor < 0 &&
                          !u.d.team_container && (u.m.state == LocoState::WalkStopped || u.m.state == LocoState::None);
        if (idle && u.d.deploys && cfg_.mode == MoveMode::Land) {
            if (u.walk_deployed && u.deploy == LocoState::WalkDeployed) {
                u.walk_deployed = false;
                u.m.speed = 0.0f;  // the 0x19 handler runs in the same frame (speed := 0)
                const DeployClip c = deploy_clip(u, kAnimDeploy);
                deploy_start_clip(u, kAnimDeploy, std::max(c.count - 1.0f, 0.0f), frame);
                deploy_pose_.push_back(u.d.id);  // the host's Deploy clip at its last frame (units_ ascending)
                continue;
            }
            if (u.deploy == LocoState::None && !u.walk_deployed && u.d.max_walk_deployed > 0.0f && !u.d.deploy_ability) {
                (void)deploy(u.d.id, true, frame);
                continue;
            }
        }
        if (u.deploy != LocoState::WalkDeploying && u.deploy != LocoState::WalkUndeploying) continue;
        const bool dep = u.deploy == LocoState::WalkDeploying;
        const int32_t anim = dep ? kAnimDeploy : kAnimUndeploy;
        if (u.dclip != anim) {
            // Not playing its clip (0x1A before its Undeploy clip; 0x18 only when the Deploy clip was overridden): (re)start
            // it once the turret is at rest; until then 0x6319d0 recentres it every frame. [M] (MOVEMENT §3.2): an SPMAT
            // in 0x18 recentres toward its deployed rest (elevation -45) but is tested against Turret_Rest_Angle, so it
            // would wait until undeployed; modelled as read (the port's 0x18 always plays its clip).
            if (!turret_at_rest(u)) {
                turret_recentre(u, turret_rest_angle(u));
                continue;
            }
            deploy_begin_clip(u, anim, frame);
        }
        // 0x39a180: the clip has ended when its frame >= count - 2 - step.
        const DeployClip c = deploy_clip(u, anim);
        if (deploy_clip_pos(u, frame) < c.count - 2.0f - c.step) continue;
        if (dep) {
            deploy_set(u, LocoState::WalkDeployed, true);  // 0x19, signal 0x29 (the clip stays the current animation)
        } else {
            u.dclip = kAnimNone;
            u.m.state = LocoState::WalkStopped;
            deploy_set(u, LocoState::None, true);  // state 0, signal 0x29
        }
    }
}

void MovementSystem::hash_deploy(StateHasher& h) const {
    uint32_t n = 0;
    for (const Unit& u : units_)
        if (u.deploy != LocoState::None || u.dclip >= 0) ++n;
    if (n == 0) return;  // unchanged format without deploying units
    h.u32(0xDE910Bu);
    h.u32(n);
    for (const Unit& u : units_) {
        if (u.deploy == LocoState::None && u.dclip < 0) continue;
        h.i32(u.d.id);
        h.u8(static_cast<uint8_t>(u.deploy));
        h.i32(u.dclip);
        h.u32(u.dclip_start);
        h.f32(u.dclip_pos0);
        if (u.walk_deployed) h.u8(0xB3);  // s78n t26.1: the walking-deployed flag (only when set)
    }
}

void MovementSystem::hash_turrets(StateHasher& h) const {
    // S8 t24: the TURRET state of the Deploys units (appended only when there are any).
    uint32_t nt = 0;
    for (const Unit& u : units_) nt += u.d.turret ? 1u : 0u;
    if (nt == 0) return;
    h.u32(0x7E44E7u);
    h.u32(nt);
    for (const Unit& u : units_) {
        if (!u.d.turret) continue;
        h.i32(u.d.id);
        for (size_t k = 0; k < 3; ++k) {
            h.f32(u.tcur[k]);
            h.f32(u.ttgt[k]);
        }
        h.f32(u.tidle);
        h.u8(static_cast<uint8_t>((u.thold ? 1 : 0) | (u.taim ? 2 : 0)));
    }
}

}  // namespace alamo::tactical::movement

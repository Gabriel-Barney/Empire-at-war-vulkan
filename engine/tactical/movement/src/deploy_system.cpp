// MovementSystem: the Walk locomotor's deploy states (docs/re/MOVEMENT.md §3.2 "Deploy states", "A locomotor swap
// undeploys"; S8 t11 / t12). Contract: movement_system.hpp ("S8 t11 / t12").
#include <algorithm>

#include "alamo/tactical/movement/movement_system.hpp"

namespace alamo::tactical::movement {

namespace {
constexpr int32_t kAnimDeploy = 0x19;    // ModelAnimType 25 (Deploy)
constexpr int32_t kAnimUndeploy = 0x1A;  // 26 (Undeploy)
}  // namespace

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
        // Deploy(true) 0x61d300: nothing in 0x18 / 0x19 ([port] pending flag +0x2b3 and obj +0x3a1 & 0x40 not modelled).
        if (u.deploy == LocoState::WalkDeploying || u.deploy == LocoState::WalkDeployed) return DeployResult::Ignored;
        if (u.deploy == LocoState::WalkUndeploying && !turret_at_rest(u)) {
            // Out of 0x1A with the turret off rest: straight to 0x19, the turret reset (the host's), signal 0x29 at once.
            u.dclip = kAnimNone;
            deploy_set(u, LocoState::WalkDeployed, true);
            return DeployResult::ReverseBlocked;
        }
        const ObjectId ids[] = {id};
        stop(ids);  // vf +0x140(0)
        deploy_set(u, LocoState::WalkDeploying, false);
        // [M] the clip plays only once the turret is at rest (the per-frame rule; MOVEMENT §3.2 window length); reversing
        // out of the Undeploy clip starts at count - cur - 1 (min 0).
        if (turret_at_rest(u)) deploy_begin_clip(u, kAnimDeploy, frame);
        return DeployResult::Deploying;
    }
    // Deploy(false): 0x18 / 0x19 -> 0x1A ([port] the slot +0x35b blocker 0x42e3b0 / 0x42e3d0 is not modelled). The
    // Undeploy clip starts in the per-frame pass (turret wait).
    if (u.deploy != LocoState::WalkDeploying && u.deploy != LocoState::WalkDeployed) return DeployResult::Ignored;
    deploy_set(u, LocoState::WalkUndeploying, false);
    return DeployResult::Undeploying;
}

LocoState MovementSystem::deploy_state(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? LocoState::None : units_[static_cast<size_t>(i)].deploy;
}

std::vector<MovementSystem::DeployEvent> MovementSystem::take_deploy_events() {
    std::vector<DeployEvent> out;
    out.swap(devents_);
    return out;
}

bool MovementSystem::snap_yaw(ObjectId id, float yaw_deg) {
    const int32_t i = find(id);
    if (i < 0) return false;
    Unit& u = units_[static_cast<size_t>(i)];
    u.m.yaw = wrap_deg(yaw_deg);
    if (is_fighter(u)) u.fs.b.o.yaw = fighter_wrap360(yaw_deg);
    return true;
}

void MovementSystem::tick_deploy(uint32_t frame) {
    // Walk vf6 0x61e930, states 0x18 / 0x1A (serial, ascending id; each unit writes only itself and the event list).
    for (Unit& u : units_) {
        if (u.deploy != LocoState::WalkDeploying && u.deploy != LocoState::WalkUndeploying) continue;
        const bool dep = u.deploy == LocoState::WalkDeploying;
        const int32_t anim = dep ? kAnimDeploy : kAnimUndeploy;
        if (u.dclip != anim) {
            // Not playing its clip yet: (re)start it once the turret is at rest (until then the turret recentres).
            if (!turret_at_rest(u)) continue;
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
    }
}

}  // namespace alamo::tactical::movement

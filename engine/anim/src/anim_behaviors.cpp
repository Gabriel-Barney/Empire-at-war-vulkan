#include "alamo/anim/anim_behaviors.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"

namespace alamo::anim {

uint64_t seconds_to_frames(float seconds) {
    if (!(seconds > 0.0f)) return 0;
    const float f = seconds * static_cast<float>(kSimTicksPerSecond);
    return f >= 1.0e18f ? static_cast<uint64_t>(1000000000000000000ull) : static_cast<uint64_t>(f);
}

int32_t flinch_type(float fx, float fy, float dx, float dy, bool attacking) {
    // cos^2 45 deg = 0.5: compare 2 dot^2 with |f|^2 |d|^2 (no trig, no sqrt).
    const float dot = fx * dx + fy * dy;
    const float cross = fx * dy - fy * dx;
    const float lf = fx * fx + fy * fy, ld = dx * dx + dy * dy;
    int32_t base = kAnimFlinchFront;
    if (lf > 0.0f && ld > 0.0f) {
        const bool steep = 2.0f * dot * dot > lf * ld;
        if (steep && dot > 0.0f) base = kAnimFlinchBack;        // travelling along the facing: hit from behind
        else if (steep && dot < 0.0f) base = kAnimFlinchFront;  // travelling against the facing: from the front
        else base = cross > 0.0f ? kAnimFlinchRight : kAnimFlinchLeft;
    }
    return attacking ? base + (kAnimAttackFlinchLeft - kAnimFlinchLeft) : base;
}

float explosion_flinch_intensity(float intensity, float dx, float dy, float range) {
    float v = intensity;
    if (range > 0.0f) {
        const float d2 = dx * dx + dy * dy;
        const float r2 = range * range;
        v = intensity * (1.0f - 0.5f * d2 / r2);
    }
    if (!(v >= kMinExplosionFlinchIntensity)) v = kMinExplosionFlinchIntensity;  // also NaN
    if (v > 1.0f) v = 1.0f;
    return v;
}

bool flinch_allowed_from(int32_t c) { return c != kAnimAttackIdle && (is_idle_like(c) || is_attack_like(c)); }

float Corpse::alpha(uint64_t frame) const {
    if (phase != CorpsePhase::Fading) return 1.0f;
    if (fade_frames == 0 || frame >= phase_end) return 0.0f;
    return static_cast<float>(phase_end - frame) / static_cast<float>(fade_frames);
}

// ---- state lookup ------------------------------------------------------------------------------------

AnimBehaviors::Entry* AnimBehaviors::find(ObjectId id) {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), id, [](const Entry& e, ObjectId v) { return e.id < v; });
    return (it != entries_.end() && it->id == id) ? &*it : nullptr;
}

const AnimBehaviors::Entry* AnimBehaviors::find(ObjectId id) const {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), id, [](const Entry& e, ObjectId v) { return e.id < v; });
    return (it != entries_.end() && it->id == id) ? &*it : nullptr;
}

AnimBehaviors::Entry& AnimBehaviors::entry(ObjectId id) {
    const auto it = std::lower_bound(entries_.begin(), entries_.end(), id, [](const Entry& e, ObjectId v) { return e.id < v; });
    if (it != entries_.end() && it->id == id) return *it;
    Entry e;
    e.id = id;
    return *entries_.insert(it, e);
}

Corpse* AnimBehaviors::find_corpse(ObjectId id) {
    const auto it = std::lower_bound(corpses_.begin(), corpses_.end(), id, [](const Corpse& c, ObjectId v) { return c.id < v; });
    return (it != corpses_.end() && it->id == id) ? &*it : nullptr;
}

const Corpse* AnimBehaviors::corpse(ObjectId id) const {
    const auto it = std::lower_bound(corpses_.begin(), corpses_.end(), id, [](const Corpse& c, ObjectId v) { return c.id < v; });
    return (it != corpses_.end() && it->id == id) ? &*it : nullptr;
}

bool AnimBehaviors::is_corpse(ObjectId id) const { return corpse(id) != nullptr; }

void AnimBehaviors::detach(ObjectId id) {
    if (Corpse* c = find_corpse(id)) c->detached = true;
}

void AnimBehaviors::remove(ObjectId id) {
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [id](const Entry& e) { return e.id == id; }), entries_.end());
    corpses_.erase(std::remove_if(corpses_.begin(), corpses_.end(), [id](const Corpse& c) { return c.id == id; }), corpses_.end());
}

// ---- flinch ------------------------------------------------------------------------------------------

bool AnimBehaviors::on_hit(AnimWorld& w, ObjectId id, const FlinchParams& p, const IdleParams& /*idle*/, float fx, float fy, float dx,
                           float dy, SimRng& rng, uint64_t frame, float intensity) {
    if (is_corpse(id) || !w.contains(id) || p.deploys) return false;  // §A.12: Deploys types never flinch
    const Entry* old = find(id);
    // §A.12: timer expired and +0x58 = 0; [port] BlendIn blocks too (I = 0).
    if (old && (frame < old->flinch_timer || old->flinch_intensity != 0.0f || old->phase == FlinchPhase::BlendIn)) return false;
    const AnimModel* m = w.model(id);
    if (!m) return false;
    const int32_t cur = w.current_type(id);
    if (!flinch_allowed_from(cur)) return false;  // §A.12: Attack Idle, none, moving, turning, death, ... never
    const bool attacking = is_attack_like(cur);
    float level = intensity;  // [port] clamped to [0, 1]
    if (!(level > 0.0f)) level = 0.0f;
    if (level > 1.0f) level = 1.0f;
    const int32_t type = flinch_type(fx, fy, dx, dy, attacking);
    if (m->variant_count(type) == 0) return false;  // no clip: nothing (no draw)
    PlayRequest r;
    r.type = type;
    r.index = kRandomVariant;  // synced
    r.loop = true;
    r.blend_seconds = p.blend_seconds;
    r.blend_mode = 2;
    r.speed = 1.0f;
    if (w.play(id, r, &rng, frame) != PlayResult::Played) return false;
    Entry& e = entry(id);
    e.flinch = type;
    e.phase = FlinchPhase::BlendIn;
    e.flinch_intensity = level;
    e.ret_type = cur;
    e.flinch_blend = p.blend_seconds;
    e.hold_frames = seconds_to_frames(p.hold_seconds * level);
    e.cooldown_frames = seconds_to_frames((attacking || cur == kAnimAttackIdle) ? p.attack_interval_seconds : p.idle_interval_seconds);
    return true;
}

AnimBehaviors::FlinchPhase AnimBehaviors::flinch_phase(ObjectId id) const {
    const Entry* e = find(id);
    return e ? e->phase : FlinchPhase::None;
}

bool AnimBehaviors::flinching(ObjectId id) const {
    const FlinchPhase ph = flinch_phase(id);
    return ph == FlinchPhase::BlendIn || ph == FlinchPhase::Hold;
}

uint64_t AnimBehaviors::next_flinch_frame(ObjectId id) const {
    const Entry* e = find(id);
    return e ? e->flinch_timer : 0;
}

int32_t AnimBehaviors::flinch_return_type(ObjectId id) const {
    const Entry* e = find(id);
    return e ? e->ret_type : kNoAnimType;
}

namespace {
template <class E>
void end_flinch(E& e, uint64_t frame) {  // cooldown from now, +0x5c = -1
    e.flinch_timer = frame + e.cooldown_frames;
    e.phase = AnimBehaviors::FlinchPhase::None;
    e.flinch = kNoAnimType;
    e.flinch_intensity = 0.0f;
    e.ret_type = kNoAnimType;
}
}  // namespace

// ---- idle --------------------------------------------------------------------------------------------

bool AnimBehaviors::service_idle(AnimWorld& w, ObjectId id, const IdleParams& idle, SimRng& rng, uint64_t frame, bool crouch_changed) {
    if (is_corpse(id) || !w.contains(id)) return false;
    const SlotView cur = w.slot(id);
    // §A.11: T (idle.idle_type) != C and a different crouch class -> the idle restarts at once, even mid-clip.
    if (crouch_changed || idle_class_restart(cur.type, idle.idle_type)) return w.service_idle(id, idle.idle_type, true, rng, frame);
    if (cur.clip && cur.mode != SlotMode::Finished) return false;
    const AnimModel* m = w.model(id);
    if (m && m->variant_count(kAnimAttackIdle) > 0) {
        const bool from_attack = cur.clip && (is_attack_like(cur.type) || cur.type == kAnimAttackIdle);
        if (from_attack) {
            Entry& e = entry(id);
            if (cur.type != kAnimAttackIdle) e.combat_timer = frame + kEngagedBaseFrames + static_cast<uint64_t>(rng.range_int(0, kEngagedRandomFrames));
            if (cur.type != kAnimAttackIdle || frame < e.combat_timer) {
                PlayRequest r;
                r.type = kAnimAttackIdle;
                r.index = 0;
                r.loop = false;
                r.blend_seconds = 0.0f;
                r.speed = 1.0f;
                return w.play(id, r, &rng, frame) == PlayResult::Played;
            }
        } else if (cur.clip && is_idle_like(cur.type)) {
            const Entry* e = find(id);
            if (e && frame < e->combat_timer) {
                PlayRequest r;  // back from an idle while still engaged
                r.type = kAnimAttackIdle;
                r.index = 0;
                r.loop = false;
                r.blend_seconds = rng.range_float(0.2f, 0.75f);
                r.speed = 1.0f;
                return w.play(id, r, &rng, frame) == PlayResult::Played;
            }
        }
    }
    return w.service_idle(id, idle.idle_type, false, rng, frame);
}

uint64_t AnimBehaviors::combat_timer(ObjectId id) const {
    const Entry* e = find(id);
    return e ? e->combat_timer : 0;
}

uint32_t AnimBehaviors::combat_countdown(ObjectId id) const {
    const Entry* e = find(id);
    return e ? e->combat_countdown : 0;
}

// ---- locomotor / deploy ------------------------------------------------------------------------------

bool is_move_family(int32_t t) {
    return t == kAnimMove || t == kAnimFireMove || t == kAnimWalkMove || t == kAnimCrouchMove || t == kAnimDeployedWalk;
}

int32_t default_idle_type(const AnimModel* m, const LocomotorInput& in, const IdleParams& idle) {
    // 0x395920 (§A.11): Idle Crouch while SPREAD_OUT, else the presence-induced idle, else the idle. No clip fallback.
    (void)m;
    return in.crouched ? kAnimIdleCrouch : (in.idle_type != kNoAnimType ? in.idle_type : idle.idle_type);
}

int32_t death_type_for_kind(uint8_t k) {
    switch (k) {
        case 2: return kAnimFwDie;
        case 3: return kAnimFtkDie;
        case 4: return kAnimFlDie;
        case 7: return kAnimFireDie;
        case 8: return kAnimCaDie;
        case 11: return kAnimDeployedCaDie;
        case 12: return kAnimDeployedDie;
        case 14: return kAnimCrushed;
        case 15: return kAnimEatenDie;
        default: return kNoAnimType;
    }
}

uint32_t AnimBehaviors::service_locomotor(AnimWorld& w, ObjectId id, const LocomotorParams& p, const LocomotorInput& in, SimRng& rng,
                                          uint64_t frame) {
    if (is_corpse(id) || !w.contains(id)) return 0;
    const AnimModel* m = w.model(id);
    if (!m) return 0;
    Entry& e = entry(id);
    float dyaw = 0.0f;
    if (e.have_yaw) {
        dyaw = in.yaw_deg - e.last_yaw;
        while (dyaw > 180.0f) dyaw -= 360.0f;
        while (dyaw <= -180.0f) dyaw += 360.0f;
    }
    const bool crouch_changed = e.have_yaw && e.last_crouch != in.crouched;
    e.last_yaw = in.yaw_deg;
    e.have_yaw = true;
    e.last_crouch = in.crouched;
    const float move_blend = w.tunables().move_blend_seconds, idle_blend = w.tunables().idle_blend_seconds;
    const int32_t cur = w.current_type(id);
    // R3f t02/t03: the IDLE service's gates (§A.11 "Exact order", LocomotorInput comment), before the locomotor's plays
    // [port: the two behaviours' relative order inside one object service].
    const bool idle_gated = in.idle_target >= 0;
    bool idle_play = true;  // the idle part may play this frame
    bool engaged = false;   // 5a: TARGETING has a target
    if (idle_gated) {
        if (in.idle_moving) {
            // 1. +0x58 := 0: a flinch still blending in loses that stage (its timer had expired at the hit, so the hold
            // stage ends at the next flinch step); Hold / Return keep their timers. Nothing else of the IDLE service runs.
            if (e.phase == FlinchPhase::BlendIn) {
                e.flinch_intensity = 0.0f;
                e.phase = FlinchPhase::Hold;
            }
            idle_play = false;
        } else if (in.idle_hold) {
            idle_play = false;  // 2. Deploys busy
        } else if (in.idle_target > 0) {
            // 5a: the combat timer is drawn every frame while the target persists.
            e.combat_countdown = kEngagedBaseFrames + static_cast<uint32_t>(rng.range_int(0, kEngagedRandomFrames));
            engaged = true;
        } else if (e.combat_countdown > 0) {
            --e.combat_countdown;  // 5b
        }
        if (e.phase != FlinchPhase::None) idle_play = false;  // 3. "flinch busy": no idle play
    }
    const bool moving = in.movable && in.speed > 0.0f;
    if (moving) {
        const float ratio = in.max_speed > 0.0f ? in.speed / in.max_speed : 1.0f;
        int32_t want = kAnimMove;
        if (in.deployed) want = kAnimDeployedWalk;
        else if (in.crouched) want = kAnimCrouchMove;
        else if (in.burning) want = kAnimFireMove;
        else if (m->variant_count(kAnimWalkMove) > 0 && ratio < p.walk_speed_ratio) want = kAnimWalkMove;
        if (m->variant_count(want) == 0) want = kAnimMove;
        if (m->variant_count(want) == 0) return 0;
        const float base = want == kAnimDeployedWalk ? p.deployed_walk_anim_speed : p.movement_anim_speed;
        const float speed = base * std::max(ratio, p.min_move_ratio);
        if (cur == want) {
            w.set_speed(id, speed);
            return 0;
        }
        if (is_move_family(cur)) {
            w.switch_preserving_phase(id, want, 0, true, move_blend, speed, frame);
            return 1;
        }
        PlayRequest r;
        r.type = want;
        r.index = 0;
        r.loop = true;
        r.blend_seconds = move_blend;
        r.speed = speed;
        const ClipRef* c = m->clip(want, 0);
        const uint32_t count = c && c->clip ? c->clip->frame_count : 0u;
        if (count > 0) r.start_frame = static_cast<float>(rng.range_int(0, static_cast<int32_t>(count) - 1));  // synced
        w.play(id, r, &rng, frame);
        return 1;
    }
    const int32_t idle_type = default_idle_type(m, in, p.idle);
    if (in.movable && (dyaw > 0.01f || dyaw < -0.01f)) {
        // Turning in place: TURNL for a counter-clockwise (positive) yaw change [port sign].
        const int32_t turn = dyaw > 0.0f ? (in.crouched ? kAnimTurnLeftCrouch : kAnimTurnLeft) : (in.crouched ? kAnimTurnRightCrouch : kAnimTurnRight);
        const bool interruptible = cur == kNoAnimType || is_idle_like(cur) || is_turn(cur) || w.current_mode(id) == SlotMode::Finished;
        if (m->variant_count(turn) > 0 && interruptible && (cur != turn || w.current_mode(id) == SlotMode::Finished)) {
            PlayRequest r;
            r.type = turn;
            r.index = 0;
            r.loop = false;
            r.blend_seconds = move_blend;
            r.speed = 1.0f;
            return w.play(id, r, &rng, frame) == PlayResult::Played ? 1u : 0u;
        }
    }
    if (is_move_family(cur)) {
        PlayRequest r;  // stopped: back to the idle (then the idle behaviour); [port] no clip of T: the plain idle
        r.type = m->variant_count(idle_type) > 0 ? idle_type : p.idle.base_idle_type;
        r.loop = p.idle.loop_idle;
        r.blend_seconds = idle_blend;
        r.speed = p.idle.idle_rate;
        w.play(id, r, &rng, frame);
        return 1;
    }
    if (idle_gated) {
        // R3f t02/t03: the idle part of the IDLE service (§A.11 "Exact order" 5a / 5b).
        if (!idle_play) return 0;
        const bool finished = w.current_mode(id) == SlotMode::Finished && cur != kNoAnimType;
        const bool attack_c = cur == kAnimAttack || cur == kAnimAttackIdle || is_attack_like(cur);
        if (engaged) {
            if (!(attack_c && finished && m->variant_count(kAnimAttackIdle) > 0)) return 0;
            PlayRequest r;  // 5a: Attack Idle, index 0, one-shot, blend 0
            r.type = kAnimAttackIdle;
            r.index = 0;
            r.loop = false;
            r.blend_seconds = 0.0f;
            r.speed = 1.0f;
            return w.play(id, r, &rng, frame) == PlayResult::Played ? 1u : 0u;
        }
        if (attack_c && !crouch_changed) {
            if (!finished) return 0;
            PlayRequest r;  // 5b after an attack: the default idle once the combat timer ran out, else Attack Idle again
            r.index = 0;
            r.loop = false;
            r.speed = 1.0f;
            // [port] a model without an Attack Idle clip goes to T at once instead of holding the attack pose.
            if (e.combat_countdown < 1 || m->variant_count(kAnimAttackIdle) == 0) {
                r.type = idle_type;
                r.blend_seconds = idle_blend;
            } else {
                r.type = kAnimAttackIdle;
                r.blend_seconds = rng.range_float(0.2f, 0.75f);
            }
            return w.play(id, r, &rng, frame) == PlayResult::Played ? 1u : 0u;
        }
        if (crouch_changed || idle_class_restart(cur, idle_type) || w.current_mode(id) == SlotMode::Finished || cur == kNoAnimType)
            return w.service_idle(id, idle_type, crouch_changed, rng, frame) ? 1u : 0u;
        return 0;
    }
    if (crouch_changed || idle_class_restart(cur, idle_type) || w.current_mode(id) == SlotMode::Finished || cur == kNoAnimType) {
        IdleParams ip = p.idle;
        ip.idle_type = idle_type;
        return service_idle(w, id, ip, rng, frame, crouch_changed) ? 1u : 0u;
    }
    return 0;
}

bool AnimBehaviors::deploy(AnimWorld& w, ObjectId id, bool undeploy, float rate, SimRng& rng, uint64_t frame) {
    if (is_corpse(id) || !w.contains(id)) return false;
    PlayRequest r;
    r.type = undeploy ? kAnimUndeploy : kAnimDeploy;
    r.index = kRandomVariant;
    r.loop = false;
    r.blend_seconds = kDeployBlendSeconds;
    r.speed = rate;
    return w.play(id, r, &rng, frame) == PlayResult::Played;
}

// ---- death / corpse ----------------------------------------------------------------------------------

uint64_t one_shot_frames(uint32_t frame_count, float fps, float speed) {
    // AnimWorld::advance_row of a one-shot slot started at frame 0: frame += fps x dt x speed per sim frame, finished once
    // it reaches max(count - 2, 0). [port] dt = float(1 / 30) every frame (the world's dt is float(t - last) of double
    // times, equal to it but for a rounding at large t; the sim schedule does not depend on the world).
    if (frame_count <= 2) return 0;  // last frame 0: finished by the (dt = 0) advance of the start frame itself
    const float dt = static_cast<float>(1.0 / kSimTicksPerSecond);
    const float step = fps * dt * speed;
    float f = 0.0f;
    constexpr uint64_t kCap = 1ull << 24;  // ~6 days of sim time: treated as never
    for (uint64_t n = 1; n <= kCap; ++n) {
        bool finished = false;
        f = advance_one_shot(f, step, frame_count, finished);
        if (finished) return n;
        if (!(step > 0.0f)) break;
    }
    return kNeverFrames;
}

DeathPlan plan_death(const AnimModel* m, const DeathParams& p, uint8_t damage_kind, SimRng& rng) {
    DeathPlan d;
    d.spin = p.spin;  // the caller's decision (TACTICAL_GAPS §1.9): no draw here
    if (d.spin) {
        d.lifetime = seconds_to_frames(p.spin_away_seconds);
        return d;
    }
    int32_t type = p.type;
    if (!p.type_explicit && m) {
        const int32_t k = death_type_for_kind(damage_kind);
        if (k != kNoAnimType && m->variant_count(k) > 0) type = k;
    }
    uint32_t index = p.index;
    const uint32_t count = m ? m->variant_count(type) : 0u;
    if (index == kRandomVariant) index = count > 0 ? static_cast<uint32_t>(rng.range_int(0, static_cast<int32_t>(count) - 1)) : 0u;
    const ClipRef* c = m ? m->clip(type, index) : nullptr;
    uint64_t clip_frames = 0;
    if (c && c->clip) {
        d.type = type;
        d.variant = index;
        clip_frames = one_shot_frames(c->clip->frame_count, c->clip->fps, 1.0f);
    } else if (p.remove_upon_death) {
        d.remove_now = true;
        return d;
    }
    if (p.persistence_seconds < 0.0f || clip_frames == kNeverFrames) {
        d.forever = true;
        return d;
    }
    d.lifetime = (d.type != kNoAnimType ? clip_frames + 1 : 0) + seconds_to_frames(p.persistence_seconds) + seconds_to_frames(p.fade_seconds);
    return d;
}

DeathResult AnimBehaviors::begin_death(AnimWorld& w, ObjectId id, const DeathParams& p, const CorpseInfo& info, SimRng& rng, uint64_t frame,
                                       uint8_t damage_kind) {
    // The draws of plan_death are AnimWorld::play's (random variant of the row's model), so planning first and then
    // playing the planned variant draws exactly what playing with a random index did.
    const DeathPlan plan = plan_death(w.contains(id) ? w.model(id) : nullptr, p, damage_kind, rng);
    return begin_death(w, id, p, plan, info, frame, damage_kind);
}

DeathResult AnimBehaviors::begin_death(AnimWorld& w, ObjectId id, const DeathParams& p, const DeathPlan& plan, const CorpseInfo& info,
                                       uint64_t frame, uint8_t damage_kind) {
    // Die step 5: animation states reset (overlay cleared, behaviour state dropped, berserker off).
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [id](const Entry& e) { return e.id == id; }), entries_.end());
    const bool has_row = w.contains(id);
    const bool spin = plan.spin;
    const int32_t type = plan.type;
    bool played = false;
    if (has_row) {
        w.clear_overlay(id);
        w.set_berserker(id, false);
    }
    if (has_row && !spin && type != kNoAnimType) {
        PlayRequest r;
        r.type = type;
        r.index = plan.variant;
        r.loop = false;
        r.blend_seconds = kDeathBlendSeconds;
        r.speed = 1.0f;
        r.lock = true;  // [port]
        played = w.play(id, r, nullptr, frame) == PlayResult::Played;
    }
    if (!played && !spin && p.remove_upon_death) {
        if (has_row) w.remove(id);
        corpses_.erase(std::remove_if(corpses_.begin(), corpses_.end(), [id](const Corpse& c) { return c.id == id; }), corpses_.end());
        return DeathResult::RemoveNow;
    }
    Corpse c;
    c.id = id;
    c.phase = spin ? CorpsePhase::SpinAway : CorpsePhase::Dying;
    c.death_type = played ? type : kNoAnimType;
    c.forever = !spin && p.persistence_seconds < 0.0f;
    c.spin_away = spin;
    c.damage_kind = damage_kind;
    c.death_frame = frame;
    c.persist_frames = seconds_to_frames(p.persistence_seconds);
    c.fade_frames = seconds_to_frames(p.fade_seconds);
    if (spin) c.phase_end = frame + seconds_to_frames(p.spin_away_seconds);
    c.info = info;
    const auto it = std::lower_bound(corpses_.begin(), corpses_.end(), id, [](const Corpse& a, ObjectId v) { return a.id < v; });
    if (it != corpses_.end() && it->id == id) *it = c;
    else corpses_.insert(it, c);
    return has_row ? DeathResult::Corpse : DeathResult::NoRow;
}

void AnimBehaviors::service(AnimWorld& w, uint64_t frame, std::vector<ObjectId>& removed, SimRng* rng) {
    // Flinch progress (§A.12, one step per service), ascending id.
    for (Entry& e : entries_) {
        if (e.phase == FlinchPhase::None) continue;
        const int32_t cur = w.contains(e.id) ? w.current_type(e.id) : kNoAnimType;
        switch (e.phase) {
            case FlinchPhase::BlendIn:
                if (cur != e.flinch) {
                    end_flinch(e, frame);  // [port] replaced by another play
                } else if (!w.crossfading(e.id) || w.crossfade_weight(e.id) >= e.flinch_intensity) {
                    w.set_speed(e.id, kFlinchFrozenSpeed);  // 0x3a8120
                    e.flinch_timer = frame + e.hold_frames;
                    e.flinch_intensity = 0.0f;
                    e.phase = FlinchPhase::Hold;
                }
                break;
            case FlinchPhase::Hold:
                if (cur != e.flinch) {
                    end_flinch(e, frame);
                } else if (frame >= e.flinch_timer) {
                    PlayRequest r;  // replay C: synced random variant, one-shot, flinch blend, blend mode 3
                    r.type = e.ret_type;
                    r.index = kRandomVariant;
                    r.loop = false;
                    r.blend_seconds = e.flinch_blend;
                    r.blend_mode = 3;
                    r.speed = 1.0f;
                    if (w.play(e.id, r, rng, frame) == PlayResult::Played) e.phase = FlinchPhase::Return;
                    else end_flinch(e, frame);
                }
                break;
            case FlinchPhase::Return:
                if (cur != e.ret_type || !w.crossfading(e.id)) end_flinch(e, frame);  // back on C, fade complete (or replaced)
                break;
            case FlinchPhase::None:
                break;
        }
    }
    // Corpse countdown (DeathBehavior::Service), ascending id.
    size_t keep = 0;
    for (size_t i = 0; i < corpses_.size(); ++i) {
        Corpse& c = corpses_[i];
        if (c.phase == CorpsePhase::Dying) {
            const bool clip_over = c.death_type == kNoAnimType || !w.contains(c.id) || w.current_type(c.id) != c.death_type ||
                                   w.current_mode(c.id) == SlotMode::Finished;
            if (clip_over) {
                c.phase = CorpsePhase::Persisting;
                c.phase_end = frame + c.persist_frames;
            }
        }
        if (c.phase == CorpsePhase::Persisting && !c.forever && frame >= c.phase_end) {
            c.phase = CorpsePhase::Fading;
            c.phase_end = frame + c.fade_frames;
        }
        if ((c.phase == CorpsePhase::Fading || c.phase == CorpsePhase::SpinAway) && frame >= c.phase_end) {
            w.remove(c.id);
            removed.push_back(c.id);
            continue;
        }
        if (keep != i) corpses_[keep] = c;
        ++keep;
    }
    corpses_.resize(keep);
}

void AnimBehaviors::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(entries_.size()));
    for (const Entry& e : entries_) {
        h.i32(e.id);
        h.i32(e.flinch);
        h.u8(static_cast<uint8_t>(e.phase));
        h.u64(e.flinch_timer);
        h.f32(e.flinch_intensity);
        h.i32(e.ret_type);
        h.f32(e.flinch_blend);
        h.u64(e.hold_frames);
        h.u64(e.cooldown_frames);
        h.u64(e.combat_timer);
        // R3f t03: hashed only when running, so state that never used the IDLE gates (idle_target -1) hashes as before.
        if (e.combat_countdown != 0) h.u32(e.combat_countdown);
        h.f32(e.last_yaw);
        h.boolean(e.have_yaw);
        h.boolean(e.last_crouch);
    }
    h.u32(static_cast<uint32_t>(corpses_.size()));
    for (const Corpse& c : corpses_) {
        h.i32(c.id);
        h.u8(static_cast<uint8_t>(c.phase));
        h.i32(c.death_type);
        h.boolean(c.forever);
        h.boolean(c.detached);
        h.boolean(c.spin_away);
        h.u8(c.damage_kind);
        h.u64(c.death_frame);
        h.u64(c.phase_end);
    }
}

}  // namespace alamo::anim

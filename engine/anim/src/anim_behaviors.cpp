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

bool AnimBehaviors::on_hit(AnimWorld& w, ObjectId id, const FlinchParams& p, const IdleParams& idle, float fx, float fy, float dx, float dy,
                           SimRng& rng, uint64_t frame) {
    if (is_corpse(id) || !w.contains(id)) return false;
    const Entry* old = find(id);
    if (old && frame < old->flinch_next) return false;  // holding or cooling down
    const AnimModel* m = w.model(id);
    if (!m) return false;
    const int32_t cur = w.current_type(id);
    const bool attacking = is_attack_like(cur) || cur == kAnimAttackIdle;
    if (!(cur == kNoAnimType || is_idle_like(cur) || attacking)) return false;  // [port] moving / turning / other
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
    e.flinch_blend = p.blend_seconds;
    e.flinch_end = frame + seconds_to_frames(p.hold_seconds);
    e.flinch_next = e.flinch_end + seconds_to_frames(attacking ? p.attack_interval_seconds : p.idle_interval_seconds);
    if (attacking && m->variant_count(kAnimAttackIdle) > 0) {
        e.ret_type = kAnimAttackIdle;
        e.ret_loop = false;
        e.ret_speed = 1.0f;
    } else {
        e.ret_type = idle.idle_type;
        e.ret_loop = idle.loop_idle;
        e.ret_speed = idle.idle_rate;
    }
    return true;
}

bool AnimBehaviors::flinching(ObjectId id) const {
    const Entry* e = find(id);
    return e && e->flinch != kNoAnimType;
}

uint64_t AnimBehaviors::next_flinch_frame(ObjectId id) const {
    const Entry* e = find(id);
    return e ? e->flinch_next : 0;
}

// ---- idle --------------------------------------------------------------------------------------------

bool AnimBehaviors::service_idle(AnimWorld& w, ObjectId id, const IdleParams& idle, IdleTarget target, SimRng& rng, uint64_t frame,
                                 bool crouch_changed) {
    // IdleBehavior service 0x62e570 from step 3 (s78k t02; the moving / deploying early returns are the caller's).
    if (is_corpse(id) || !w.contains(id)) return false;
    const SlotView cur = w.slot(id);
    const AnimModel* m = w.model(id);
    const bool from_attack = cur.clip && (is_attack_like(cur.type) || cur.type == kAnimAttack || cur.type == kAnimAttackIdle);
    const bool finished = cur.clip && cur.mode == SlotMode::Finished;
    PlayRequest r;
    r.type = kAnimAttackIdle;
    r.index = 0;
    r.loop = false;
    r.blend_seconds = 0.0f;
    r.speed = 1.0f;
    if (target == IdleTarget::Target) {
        // Step 4: one synced draw on every service, countdown = r + 90; Attack Idle again only after a finished
        // attack-like / Attack / Attack Idle clip (no draw for that play). Nothing else.
        Entry& e = entry(id);
        e.idle_countdown = kEngagedBaseFrames + static_cast<uint64_t>(rng.range_int(0, kEngagedRandomFrames));
        if (from_attack && finished && m && m->variant_count(kAnimAttackIdle) > 0) return w.play(id, r, nullptr, frame) == PlayResult::Played;
        return false;
    }
    // Step 5: no target. The countdown ages; nothing more during a flinch or while the clip plays.
    Entry* e = find(id);
    if (e && e->idle_countdown > 0) --e->idle_countdown;
    if (flinching(id)) return false;
    if (cur.clip && !finished && !crouch_changed) return false;
    if (from_attack && target != IdleTarget::None) {
        if (e && e->idle_countdown > 0) {
            r.blend_seconds = rng.range_float(0.2f, 0.75f);  // drawn even without the clip [M]
            return w.play(id, r, nullptr, frame) == PlayResult::Played;
        }
        r.type = idle.idle_type;  // the default idle, idle blend
        r.blend_seconds = w.tunables().idle_blend_seconds;
        return w.play(id, r, nullptr, frame) == PlayResult::Played;
    }
    return w.service_idle(id, idle.idle_type, crouch_changed, rng, frame);
}

uint64_t AnimBehaviors::idle_countdown(ObjectId id) const {
    const Entry* e = find(id);
    return e ? e->idle_countdown : 0;
}

// ---- locomotor / deploy ------------------------------------------------------------------------------

bool is_move_family(int32_t t) {
    return t == kAnimMove || t == kAnimFireMove || t == kAnimWalkMove || t == kAnimCrouchMove || t == kAnimDeployedWalk;
}

int32_t default_idle_type(const AnimModel* m, const LocomotorInput& in, const IdleParams& idle) {
    // 0x395920: Idle Crouch when crouched, else the container's mapping when garrisoned, else the idle. [port] a
    // type without clips falls back to the plain idle.
    int32_t t = in.crouched ? kAnimIdleCrouch : (in.idle_type != kNoAnimType ? in.idle_type : idle.idle_type);
    if (m && m->variant_count(t) == 0) t = idle.idle_type;
    return t;
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
    const bool moving = in.movable && in.speed > 0.0f;
    IdleParams ip = p.idle;
    ip.idle_type = default_idle_type(m, in, p.idle);
    // s78k t02: the IDLE service (0x62e570) after the locomotor's play, every call while not moving (step 1: a moving unit
    // is skipped unless Allow_Idle_When_Moving). Plays started: the locomotor's + the idle's. A crouch change forces the
    // idle clip only when the locomotor started none this call ([port]: its stop / turn clip already follows the state).
    const auto then_idle = [&](uint32_t plays) -> uint32_t {
        if (moving && !in.allow_idle_when_moving) return plays;
        return plays + (service_idle(w, id, ip, in.target, rng, frame, crouch_changed && plays == 0) ? 1u : 0u);
    };
    if (moving) {
        const float ratio = in.max_speed > 0.0f ? in.speed / in.max_speed : 1.0f;
        int32_t want = kAnimMove;
        if (in.deployed) want = kAnimDeployedWalk;
        else if (in.crouched) want = kAnimCrouchMove;
        else if (in.burning) want = kAnimFireMove;
        else if (m->variant_count(kAnimWalkMove) > 0 && ratio < p.walk_speed_ratio) want = kAnimWalkMove;
        if (m->variant_count(want) == 0) want = kAnimMove;
        if (m->variant_count(want) == 0) return then_idle(0);
        const float base = want == kAnimDeployedWalk ? p.deployed_walk_anim_speed : p.movement_anim_speed;
        const float speed = base * std::max(ratio, p.min_move_ratio);
        if (cur == want) {
            w.set_speed(id, speed);
            return then_idle(0);
        }
        if (is_move_family(cur)) {
            w.switch_preserving_phase(id, want, 0, true, move_blend, speed, frame);
            return then_idle(1);
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
        return then_idle(1);
    }
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
            return then_idle(w.play(id, r, &rng, frame) == PlayResult::Played ? 1u : 0u);
        }
    }
    if (is_move_family(cur)) {
        PlayRequest r;  // stopped: back to the idle (then the idle behaviour)
        r.type = ip.idle_type;
        r.loop = p.idle.loop_idle;
        r.blend_seconds = idle_blend;
        r.speed = p.idle.idle_rate;
        w.play(id, r, &rng, frame);
        return then_idle(1);
    }
    return then_idle(0);
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
    if (p.spin_away) {
        if (p.spin_away_chance > 0.0f && p.spin_away_chance < 1.0f) d.spin = rng.range_float(0.0f, 1.0f) < p.spin_away_chance;
        else d.spin = true;  // [port] chance >= 1 or unset
    }
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

void AnimBehaviors::service(AnimWorld& w, uint64_t frame, std::vector<ObjectId>& removed) {
    // Flinch hold ends.
    for (Entry& e : entries_) {
        if (e.flinch == kNoAnimType || frame < e.flinch_end) continue;
        if (w.contains(e.id) && w.current_type(e.id) == e.flinch) {
            PlayRequest r;
            r.type = e.ret_type;
            r.index = 0;
            r.loop = e.ret_loop;
            r.blend_seconds = e.flinch_blend;
            r.blend_mode = 2;
            r.speed = e.ret_speed;
            w.play(e.id, r, nullptr, frame);
        }
        e.flinch = kNoAnimType;
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
        h.u64(e.flinch_end);
        h.u64(e.flinch_next);
        h.f32(e.flinch_blend);
        h.i32(e.ret_type);
        h.boolean(e.ret_loop);
        h.f32(e.ret_speed);
        h.u64(e.idle_countdown);
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

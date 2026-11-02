// OI abilities - special-ability class bodies of docs/re/TACTICAL_GAPS.md §2.3 (the classes FoC / Thrawn's Revenge
// use beyond the S9 set): accepts-target templates, apply / update / remove per class. Victim scans walk the combat
// units in ascending id (port rule for GOM / query order).
//
// Animation-driven timings (OI t03, ABILITIES_HARDPOINTS.md §4.13 [port]): the sim never reads a playing clock. A clip
// position is a pure function of the sim frames n since the clip started: n x rate x speed, rate = clip fps / 30 (the
// sim animation's clock, anim_state.hpp) and speed = Attack_Animation_Speedup (1 for the other clips). "At animation
// frame N" (Damage_Frame_Number, Begin_Applying_Damage_Frame, Attachment_Frame, Target_Destruction_Frame, Spawn_Frame,
// Fire_Frame) = the first update whose position >= N. "At the animation end" = position >= max(count - 2, 0), where a
// one-shot finishes. Frame count and fps come from the model's clip data through AbilityHost::anim_clip (clip_timing);
// without a clip (no host, no model, no such clip) rate is 1 and each end falls back: melee / grenade end at their last
// frame-numbered event, Hack / Repair after kHackFrames, the Berserker damage window is always open.
//
// Synced draws (exactly these, in apply order): one per apply attempt that passed the range test for the chance
// classes (Arc_Sweep / Generic / Eat: draw <= chance; Earthquake / Telekinesis / Vehicle_Thief / Whirlwind: draw <
// chance; a chance of 0 refuses without drawing for the melee classes), two per mine (angle, radius), two per Confuse
// victim (CONFUSE destination).
#include <algorithm>
#include <array>
#include <cmath>

#include "alamo/core/dmath.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"

namespace alamo::tactical::abilities {

using combat::DamageRequest;
using combat::HardpointKind;
using combat::ModList;

namespace {
using C = SpecialClass;
constexpr float kDegToRad = 0.017453292519943295f;
constexpr uint8_t kKind2 = 2, kKind3 = 3, kKind4 = 4, kKind5 = 5, kKind16 = 0x16;
constexpr int32_t kHackFrames = 60;  // [port] Hack / Repair animation 0x5a length without a clip (§4.13 fallback)
constexpr int32_t kAnimHacking = 0x5a;  // Hack / Repair (TACTICAL_GAPS §2.3.10)
constexpr int32_t kAnimBerserk = 0x68;  // Berserker (TACTICAL_GAPS §2.3.2)
constexpr int32_t kAnimAttack = 5;      // Super_Laser (TACTICAL_GAPS §2.3.9), weapon shots, the Arc / Eat gates
// §4.14: begin / hold / release clips (anim_types.cpp)
constexpr int32_t kAnimFtkAttack = 0x21, kAnimFtkRelease = 0x23;  // Telekinesis
constexpr int32_t kAnimFbAttack = 0x25, kAnimFbRelease = 0x27;    // Earthquake
constexpr int32_t kAnimFlAttack = 0x28;                           // Force_Lightning (Force_Sight's: ability_system.cpp)

int32_t frames_of(float secs, uint32_t fps) { return static_cast<int32_t>(static_cast<float>(fps) * secs); }

// |angle between the unit's facing and the direction to p| <= deg (XY), via cosines (no acos).
bool facing_within(Vec3 f, Vec3 from, Vec3 p, float deg) {
    const float dx = p.x - from.x, dy = p.y - from.y;
    const float lf = dmath::sqrt(f.x * f.x + f.y * f.y), ld = dmath::sqrt(dx * dx + dy * dy);
    if (!(lf > 0.0f) || !(ld > 0.0f)) return true;
    return (f.x * dx + f.y * dy) / (lf * ld) >= dmath::cos(deg * kDegToRad);
}

// Signed XY bearing (degrees, -180..180) of p relative to the unit's facing.
float bearing_deg(Vec3 f, Vec3 from, Vec3 p) {
    const float a = dmath::atan2(p.y - from.y, p.x - from.x) - dmath::atan2(f.y, f.x);
    float d = a / kDegToRad;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    return d;
}
}  // namespace

AbilitySystem::ClipTiming AbilitySystem::clip_timing(ObjectId id, int32_t anim_type, std::string_view anim_name,
                                                     uint32_t variant) const {
    ClipTiming t;
    if (!host_.anim_clip || (anim_type < 0 && anim_name.empty())) return t;
    uint32_t count = 0;
    float fps = 0.0f;
    if (!host_.anim_clip(id, anim_type, anim_name, variant, count, fps) || count == 0 || !(fps > 0.0f)) return t;
    t.rate = fps / 30.0f;
    t.count = static_cast<float>(count);
    t.end = count >= 2 ? static_cast<float>(count - 2) : 0.0f;
    return t;
}

// OI follow-up t01 (ABILITIES_HARDPOINTS §4.14 [port]): the owner's primary slot as far as the sim knows it. Only the
// ability system's own plays (and non-overlay weapon shots of melee owners) are recorded; a play without clip data leaves
// the record unchanged, so headless runs without models keep "none" and every gate passes.
int32_t AbilitySystem::anim_type_of(std::string_view anim_name) const {
    return (host_.anim_type && !anim_name.empty()) ? host_.anim_type(anim_name) : -1;
}

void AbilitySystem::play_anim(Obj& o, int32_t type, std::string_view anim_name, uint32_t variant, bool loop, float speed,
                              uint8_t then, uint32_t frame) {
    if (type < 0) type = anim_type_of(anim_name);
    if (type < 0) return;
    const ClipTiming ct = clip_timing(o.id, type, {}, variant);
    if (ct.count <= 0.0f) return;  // the model lacks the clip: the play returns 0, nothing changes
    o.anim_type = type;
    o.anim_start = frame;
    o.anim_rate = ct.rate * speed;
    o.anim_end = ct.end;
    o.anim_loop = loop ? 1 : 0;
    o.anim_then = then;
}

AbilitySystem::AnimView AbilitySystem::anim_view(const Obj& o, uint32_t frame) const {
    AnimView v;
    if (o.anim_type < 0) return v;
    v.type = o.anim_type;
    if (o.anim_loop) return v;  // a loop never finishes
    // One-shot: finished from the first advance with position >= count - 2 (a clip does not move in its play frame).
    const float n = frame > o.anim_start ? static_cast<float>(frame - o.anim_start) : 0.0f;
    if (!(n * o.anim_rate >= o.anim_end)) return v;
    switch (o.anim_then) {
        case kAnimIdle: v.type = -1; break;           // the update that saw it finish played the idle
        case kAnimHoldLoop: v.type = o.anim_type + 1; break;  // begin -> hold loop
        default: v.finished = true; break;
    }
    return v;
}

bool AbilitySystem::class_state(const Obj& o, SpecialClass cls) const {
    for (const SpecialState& sp : o.specials)
        if (sp.applied && data_->specials[sp.desc].cls == cls) return true;
    return false;
}

bool AbilitySystem::anim_busy(const Obj& o, uint32_t frame) const {
    // 0x39a350: OR of the owner's abilities' type-only tests (the mode is ignored). Earthquake / Telekinesis / Lightning /
    // Generic / Eat count in land only; Arc_Sweep by its applied state in any mode. [port] Eat's Target_Grabbed_Animation
    // is not parsed (only Attack_Animation counts).
    if (class_state(o, C::ArcSweep)) return true;
    if (cfg_.mode != GameMode::Land) return false;
    const int32_t type = anim_before(o, frame).type;
    if (type < 0) return false;
    for (const SpecialState& sp : o.specials) {
        const SpecialAbilityDesc& s = data_->specials[sp.desc];
        switch (s.cls) {
            case C::Earthquake:
                if (type >= kAnimFbAttack && type <= kAnimFbRelease) return true;
                break;
            case C::Telekinesis:
                if (type >= kAnimFtkAttack && type <= kAnimFtkRelease) return true;
                break;
            case C::ForceLightning:
                if (type == kAnimFlAttack) return true;
                break;
            case C::GenericAttack:
            case C::EatAttack:
                if (type == anim_type_of(s.attack_anim)) return true;
                break;
            default: break;
        }
    }
    return false;
}

bool AbilitySystem::locked(const Obj& o) const {
    return o.immobilize > 0 || (host_.movement_locked && host_.movement_locked(o.id));
}

void AbilitySystem::snap_face(Obj& o, ObjectId target) {
    // S8 t10 (LOB_SOLVER §3.5 "Facing at apply", Grenade_Attack 0x700860 / Remote_Bomb 0x719c90): angles 0x20acd0 from the
    // unit to the target's RAW position (no Ranged_Target_Z_Adjust; roll and pitch 0), snapped at once by 0x3a8710 (no turn
    // rate). Skipped while obj +0x3a0 & 0x10 (weapons disabled / stunned). Not re-faced at the throw, not restored after.
    if (o.weapons_disabled || !host_.snap_yaw || target == kNoObject || !combat_->has_unit(target)) return;
    const Vec3 a = position(o), b = combat_->position(target);
    const float dx = b.x - a.x, dy = b.y - a.y;
    if (dx == 0.0f && dy == 0.0f) return;  // [port] no direction: the facing is kept
    const float yaw = dmath::atan2(dy, dx) * (180.0f / dmath::kPiF);
    host_.snap_yaw(o.id, yaw);
    if (o.combat_unit && combat_->has_unit(o.id)) {  // the combat facing at once (the host re-pushes it every tick)
        float s = 0.0f, c = 1.0f;
        dmath::sincos(yaw * (dmath::kPiF / 180.0f), s, c);
        combat_->set_facing(o.id, Vec3{c, s, 0.0f});
    }
}

void AbilitySystem::lock_pair(Obj& o, uint8_t flag, bool lock, uint32_t frame) {
    // OI follow-up t04 (A1_READINGS §28.9 table): one caller's lock / unlock on the shared counter. [port] the flag keeps
    // each pair balanced (the original's unconditional releases are hidden by the clamp at 0).
    if (lock == ((o.lock_flags & flag) != 0)) return;
    o.lock_flags = static_cast<uint8_t>(lock ? (o.lock_flags | flag) : (o.lock_flags & ~flag));
    immobilize(o, lock ? +1 : -1, frame);
}

// ================================================================================================
// accepts-target templates (vfunc +0x28)
// ================================================================================================

bool AbilitySystem::class_accepts(const Obj& o, const SpecialAbilityDesc& s, ObjectId target, uint32_t frame) const {
    (void)frame;
    const bool live = alive_unit(target);
    const bool dummy = target != kNoObject && combat_->has_unit(target) && (combat_->flags(target) & combat::kUnitSkipTargeting);
    const bool foe = live && !dummy && enemy(o.owner, combat_->owner(target));
    switch (s.cls) {
        // 0x6eef00 (Arc_Sweep, Concentrate_Fire, Generic_Attack) and the other enemy-target classes: enemy, applicable
        // filter (0x48e290) [port: the model / layer tests are not modelled].
        case C::ArcSweep:
        case C::GenericAttack:
        case C::EatAttack:
        case C::Earthquake:
        case C::Telekinesis:
        case C::GrenadeAttack:
        case C::ForceLightning:
        case C::DrainLife:
        case C::LeechShields:
        case C::SuperLaser:
        case C::Hack:
        case C::VehicleThief:
        case C::TacticalBribe:
        case C::Infection:
        case C::SaberThrow:
        case C::FlameThrower:
        case C::Cable:
        case C::MaximumFirepower: return foe && target_filter(s, target);
        case C::Berserker:  // enemy, not a projectile, has a locomotor, filter
            return foe && combat_->data().units[combat_->desc_index(target)].has_locomotor && target_filter(s, target);
        case C::Repair: return live && !dummy && allied(o.owner, combat_->owner(target)) && target_filter(s, target);  // [inferred]
        // 0x712770 (Proximity_Mines, Buzz_Droids, Corrupt_Systems, Summon, Radioactive): the MARKER dummy point object
        // [port: or a live target / none - positions come with the command-57 dummy].
        case C::ProximityMines:
        case C::BuzzDroids:
        case C::Summon:
        case C::Spawn:
        case C::Radioactive:
        case C::RemoteBomb:
        case C::CorruptSystems: return target == kNoObject || dummy || live;
        // 0x592e30: self or no target (Whirlwind / Confuse / Stun waves also take the auto-use target [port]).
        case C::ForceWhirlwind:
        case C::ForceConfuse:
        case C::StunWave: return target == kNoObject || target == o.id || dummy || live;
        case C::ClusterBomb:
        case C::ShieldFlare:
        case C::Blast:
        case C::ForceCloak: return target == kNoObject || target == o.id || dummy;
        default: return true;
    }
}

bool AbilitySystem::range_ok(const Obj& o, const SpecialAbilityDesc& s, ObjectId target) const {
    if (target == kNoObject || target == o.id || !combat_->has_unit(target)) return true;
    return in_range(o, s, target);
}

// ================================================================================================
// apply (vf30)
// ================================================================================================

bool AbilitySystem::class_apply(Obj& o, SpecialState& sp, ObjectId target, int32_t hp, uint32_t frame) {
    (void)hp;
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    const uint32_t fps = cfg_.fps;
    const Vec3 me = position(o);
    auto chance_roll_le = [&](float chance) {  // proceed when draw <= chance (Arc / Generic / Eat)
        if (!(chance > 0.0f)) return false;   // a chance of 0 refuses without drawing
        return draw_unit() <= chance;
    };
    auto chance_roll_lt = [&](float chance) { return draw_unit() < chance; };  // Earthquake / Telekinesis / Thief / Whirlwind
    switch (s.cls) {
        case C::CombatBonus:
        case C::EnhanceDefense:
        case C::BattlefieldModifier:
        case C::BasePower:
        case C::Weatherproof:
        case C::RetreatPrevention:
        case C::EnableAbility:
            aura_on(o, sp, frame);
            return true;

        case C::ForceHealing:  // 2.3.1: apply is a no-op; the update does the work
            sp.applied = 1;
            return true;

        case C::StarbaseUpgrade:  // STATION_HANGARS_SOUND.md §1.1: armed at creation; the swap runs on the first service
            sp.applied = 1;
            sp.phase = 0;
            return true;

        case C::Berserker: {  // 2.3.2; the speed override (locomotor +0x2a0) is read through speed_override() while applied
            if (!alive_unit(target) || !range_ok(o, s, target)) return false;
            sp.applied = 1;
            sp.target = target;
            sp.counter = frames_of(s.berserker_duration, fps);
            start_recharge(o, kBerserker);
            event(AbilityEventKind::EffectOn, kBerserker, o.id, target, frame);
            return true;
        }

        case C::ProximityMines: {  // 2.3.3 apply 0x71a4d0 + spawn 0x71a650
            if (target != kNoObject && combat_->has_unit(target) && !range_ok(o, s, target)) return false;
            const Vec3 at = (target != kNoObject && combat_->has_unit(target)) ? combat_->position(target) : me;
            stop_order(o.id, frame);
            start_recharge(o, kProximityMines);
            for (ObjectId m : o.members)
                if (Obj* mo = obj(m)) start_recharge(*mo, kProximityMines);
            const UnitAbilityRecord* r67 = record_of(o, kProximityMines);
            const float R = r67 ? r67->area_effect_decal_distance : 0.0f;
            for (int32_t i = 0; i < s.spawn_count; ++i) {
                const float ang = draw_float(0.0f, 360.0f) * kDegToRad;  // angle Get_Float(0, 360) degrees
                const float rad = draw_float(-R, R);                      // radius Get_Float(-R, R)
                float sn = 0.0f, cs = 1.0f;
                dmath::sincos(ang, sn, cs);
                Vec3 p{at.x + cs * rad, at.y + sn * rad, at.z};
                if (host_.ground_height) p.z = host_.ground_height(p.x, p.y);
                if (s.spawn_type < 0) continue;  // [port] unknown Mine_Type: draws consumed, nothing created
                SpawnRequest q;
                q.purpose = SpawnPurpose::Mine;
                q.object_type = s.spawn_type;
                q.position = p;
                q.facing = combat_->has_unit(o.id) ? combat_->facing(o.id) : Vec3{1.0f, 0.0f, 0.0f};
                q.owner = o.owner;
                q.source = o.id;
                q.ability_type = kProximityMines;
                Mine mine;
                mine.serial = request_spawn(q);
                mine.owner = o.owner;
                mine.pos = p;
                mine.trigger = s.trigger_radius;
                mine.arm = frame + static_cast<uint32_t>(frames_of(s.activation_time, fps)) + 1u;
                mines_.push_back(mine);
            }
            event(AbilityEventKind::EffectOn, kProximityMines, o.id, target, frame);
            return true;
        }

        case C::Earthquake:
        case C::Telekinesis: {  // 2.3.4
            if (!alive_unit(target)) return false;
            if (sp.applied) class_remove(o, sp, frame);  // remove the previous effect (plays the release clip)
            // §4.14: Earthquake refuses while the owner shows FB_Release (any phase; also the one the removal just
            // started); Telekinesis has no animation gate.
            if (s.cls == C::Earthquake && anim_before(o, frame).type == kAnimFbRelease) return false;
            if (moving(o) || !range_ok(o, s, target)) return false;
            if (!chance_roll_lt(s.activation_chance)) return false;  // one synced draw < Activation_Chance
            if (s.cls == C::Earthquake) {  // also ends the owner's own Force_Cloak (stealth breaks, 0.3 s)
                if (o.stealthed) break_stealth(o, frame);
            }
            stop_order(o.id, frame);
            sp.applied = 1;
            sp.target = target;
            sp.value = max_health_and_shield(target) * s.damage_percentage + s.damage_amount;  // D
            sp.next = frame + static_cast<uint32_t>(std::max(1, frames_of(s.damage_interval, fps)));
            if (s.cls == C::Telekinesis) start_recharge(o, kForceTelekinesis);
            // §4.14: FB_Attack / FTK_Attack one-shot, then the hold clip loops
            play_anim(o, s.cls == C::Earthquake ? kAnimFbAttack : kAnimFtkAttack, {}, 0, false, 1.0f, kAnimHoldLoop, frame);
            event(AbilityEventKind::EffectOn, sp.trigger, o.id, target, frame);
            return true;
        }

        case C::GrenadeAttack: {  // 2.3.5
            if (!alive_unit(target)) return false;
            if (s.requires_direct_command && !direct_ctx_ && combat_->ai_player(o.owner)) return false;
            if (!range_ok(o, s, target) || s.projectile < 0) return false;
            // S8 t04 (LOB_SOLVER §3.4): Grenade_Type is a category-3 projectile with a lob solution from the unit's
            // position to the target's position (no z adjust); otherwise refused (the ability is not consumed).
            if (!combat_->lob_solution(static_cast<uint32_t>(s.projectile), me, combat_->position(target))) return false;
            // S8 t10 follow-up (LOB_SOLVER §3.5 "Facing at apply", last item): with ABILITY_COUNTDOWN and unit ability 22's
            // counter > 0 the apply returns 1 WITHOUT the face / stop / lock / pending block: the request is consumed (a
            // pending order is not retried) and nothing is thrown [M on the meaning; the port applies the flow as read].
            if (desc(o).countdown && o.cd.counter[kStickyBomb] > 0) return true;
            snap_face(o, target);  // S8 t10: yaw snapped to the target's raw position
            stop_order(o.id, frame);
            lock_pair(o, kLockGrenade, true, frame);  // stop, lock (0x3a8e50 1; A1_READINGS §28.9)
            sp.applied = 1;
            sp.target = target;
            sp.phase = 0;
            sp.next = frame;  // toss starts once not moving / turning
            if (o.combat_unit) combat_->clear_targets(o.id);  // targeting suspended
            return true;
        }

        case C::ForceLightning: {  // 2.3.6 apply 0x6fd010
            if (!alive_unit(target) || moving(o)) return false;
            if (anim_before(o, frame).type == kAnimFlAttack) return false;  // §4.14: refused while FL_Attack runs
            ObjectId primary = target;
            if (const Obj* to = obj(target); to && to->container != kNoObject && !alive_unit(primary)) primary = to->container;
            if (!range_ok(o, s, primary)) return false;
            stop_order(o.id, frame);
            lightning_targets(o, s, primary, sp.victims);
            for (ObjectId v : sp.victims) {  // each target takes its current health (kill-all, damage multipliers only)
                if (!alive_unit(v)) continue;
                DamageRequest r;
                r.victim = v;
                r.amount = combat_->health(v);
                r.kind = kKind4;
                r.source = o.id;
                combat_->take_damage(r, frame);
            }
            start_recharge(o, kForceLightning);
            play_anim(o, kAnimFlAttack, {}, 0, false, 1.0f, kAnimIdle, frame);  // §4.14: the cast lasts the clip
            event(AbilityEventKind::EffectOn, kForceLightning, o.id, primary, frame);
            return true;  // update is visuals only
        }

        case C::ForceWhirlwind:
        case C::StunWave:
        case C::ForceConfuse: {  // 2.3.7
            const bool direct = direct_ctx_ != 0 || target == kNoObject || target == o.id;
            const float range = s.cls == C::ForceWhirlwind ? s.damage_range : s.wave_range;
            if (sp.applied) return true;  // "true at once when a wave runs"
            if (!direct) {  // auto-use pre-check: distance band, >= 1 eligible victim in range, Whirlwind chance roll
                if (alive_unit(target)) {
                    const float d2 = combat::distance2_xy(me, combat_->position(target));
                    if (d2 < s.min_range * s.min_range || d2 > s.max_range * s.max_range) return false;
                }
                sp.victims.clear();
                const uint32_t n = wave_victims(o, sp, range, scratch_);
                if (n == 0) return false;
                if (s.cls == C::ForceWhirlwind) {
                    const float chance = s.chance_curve.empty() ? s.activation_chance : s.chance_curve.eval(static_cast<float>(n));
                    if (!chance_roll_lt(chance)) return false;  // [M: curve argument = the victim count]
                }
            }
            sp.applied = 1;
            sp.victims.clear();
            sp.target = target;
            const int32_t interval = std::max(1, interval_of(s));
            sp.value = (s.travel_time > 0.0f) ? (static_cast<float>(interval) / static_cast<float>(fps)) / s.travel_time * range : range;
            sp.radius = sp.value;
            start_recharge(o, s.cls == C::ForceWhirlwind ? kForceWhirlwind : (s.cls == C::StunWave ? kStun : kForceConfuse));
            event(AbilityEventKind::EffectOn, sp.trigger, o.id, target, frame);
            class_update(o, sp, frame);  // the first ring
            return true;
        }

        case C::ArcSweep:
        case C::GenericAttack:
        case C::EatAttack: {  // 2.3.8
            if (!alive_unit(target) || moving(o)) return false;
            {  // §4.14 gates (owner's primary slot as of the last advance)
                const AnimView v = anim_before(o, frame);
                if (s.cls == C::GenericAttack) {  // its own Attack_Animation in any phase, then busy
                    if ((v.type >= 0 && v.type == anim_type_of(s.attack_anim)) || anim_busy(o, frame)) return false;
                } else {  // Arc / Eat: the per-owner class state, the literal Attack (5) unless finished; Eat also busy
                    if (class_state(o, s.cls) || (v.type == kAnimAttack && !v.finished)) return false;
                    if (s.cls == C::EatAttack && anim_busy(o, frame)) return false;
                }
            }
            if (!range_ok(o, s, target)) return false;
            if (s.cls != C::EatAttack && o.combat_unit) {
                const float lim = s.cls == C::ArcSweep ? 15.0f : 5.0f;
                if (!facing_within(combat_->facing(o.id), me, combat_->position(target), lim)) return false;
            }
            if (!chance_roll_le(s.activation_chance)) return false;
            stop_order(o.id, frame);
            sp.applied = 1;
            sp.target = target;
            sp.next = frame;  // animation start
            sp.phase = 0;
            sp.radius = s.right_to_left ? 0.5f * s.arc_width : -0.5f * s.arc_width;  // sweep start angle
            sp.victims.clear();
            // §4.14: Attack_Animation one-shot at Speedup, then the default idle (Arc: the explicit Subindex variant;
            // [port] Generic / Eat use variant 0, no draw; Eat plays at the apply)
            play_anim(o, -1, s.attack_anim, s.cls == C::ArcSweep ? static_cast<uint32_t>(std::max(0, s.anim_sub_index)) : 0u,
                      false, s.anim_speedup, kAnimIdle, frame);
            event(AbilityEventKind::EffectOn, sp.trigger, o.id, target, frame);
            return true;
        }

        case C::SuperLaser: {  // 2.3.9
            if (!alive_unit(target) || !range_ok(o, s, target) || !has(o, kSuperLaser)) return false;
            sp.applied = 1;
            sp.target = target;
            sp.phase = 0;
            sp.next = frame + static_cast<uint32_t>(std::max(0, frames_of(s.fire_delay, fps)));  // GMM frame + int(FPS x delay)
            o.active[kSuperLaser] = 1;
            sync_combat(o);
            event(AbilityEventKind::EffectOn, kSuperLaser, o.id, target, frame);
            return true;
        }

        case C::DrainLife: {  // 2.3.10
            if (!alive_unit(target) || !range_ok(o, s, target)) return false;
            if (s.charging_time > 0.0f && combat::distance_xy(me, combat_->position(target)) > s.targets_radius) return false;
            lightning_targets(o, s, target, sp.victims);
            stop_order(o.id, frame);
            sp.applied = 1;
            sp.target = target;
            sp.counter = frames_of(s.duration_secs, fps);
            start_recharge(o, kDrainLife);
            event(AbilityEventKind::EffectOn, kDrainLife, o.id, target, frame);
            return true;
        }

        case C::LeechShields: {  // vf30 0x7177a0 (REMAINING_GAPS §5.6): needs 58 (a live WEAPON_SPECIAL hardpoint), in range
            if (!alive_unit(target) || !range_ok(o, s, target) || !can(o, kLeechShields, false, true, frame)) return false;
            sp.applied = 1;
            sp.target = target;
            o.active[kLeechShields] = 1;  // 58 on
            // Damage_Multiplier: a PERMANENT entry (replace by source 0x3fe810) in the owner's OWN set, list 1 "damage
            // dealt", stacking category 0, keyed by the ability [M: source key = the ability name CRC]: all damage the
            // owner deals is x (1 + m) while draining.
            if (o.combat_unit)
                if (combat::ModifierSet* m = combat_->unit_modifiers(o.id))
                    m->set_by_source(ModList::Damage, 0, s.damage_multiplier, combat::kPermanent, s.name_crc, o.id);
            sp.counter = frames_of(s.duration_secs, fps);
            start_recharge(o, kLeechShields);
            event(AbilityEventKind::EffectOn, kLeechShields, o.id, target, frame);
            return true;
        }

        case C::Blast: {  // apply 0x71c0b0 (REMAINING_GAPS §5.5): counter := int(FPS x Charge_Up_Seconds), Fire_At gated
            sp.applied = 1;
            sp.counter = frames_of(s.charging_time, fps);
            sp.phase = 0;
            start_recharge(o, kBlast);
            if (o.combat_unit) combat_->set_flag(o.id, combat::kUnitSkipTargeting, true);  // obj +0x3a1 |= 0x10
            event(AbilityEventKind::EffectOn, kBlast, o.id, kNoObject, frame);
            return true;
        }

        case C::ForceCloak: {  // stealth as Stealth_Ability with unit ability 54's Expiration_Seconds; cancels Earthquake
            for (SpecialState& q : o.specials)
                if (q.applied && data_->specials[q.desc].cls == C::Earthquake) class_remove(o, q, frame);
            if (!sp.trigger) sp.trigger = kForceCloak;
            const UnitAbilityRecord* r = record_of(o, kForceCloak);
            const float e = r ? r->expiration_seconds : 0.0f;
            float f = 1.0f + player_reduce(o.owner, static_cast<uint8_t>(PlayerList::StealthDuration));
            if (o.owner >= 0 && static_cast<size_t>(o.owner) < stealth_bonus_.size()) f += stealth_bonus_[static_cast<size_t>(o.owner)];
            if (f <= 0.0f) f = 1.0f;
            o.stealth_frames = static_cast<int32_t>(static_cast<float>(fps) * f * e);
            o.stealthed = 1;
            o.last_detected = 0;
            if (o.combat_unit) combat_->clear_targets(o.id);
            sp.applied = 1;
            sp.target = o.id;
            refresh_stealth_flag(o, frame);
            event(AbilityEventKind::StealthOn, kForceCloak, o.id, kNoObject, frame);
            return true;
        }

        case C::Hack:
        case C::Repair:
        case C::TacticalBribe: {  // the acting member (Team_Member_With_Ability) faces the target; animation 0x5a
            if (!alive_unit(target) || !range_ok(o, s, target)) return false;
            sp.applied = 1;
            sp.target = target;
            uint32_t len = 0;  // animation 0x5a: its end on sim frames, else kHackFrames (§4.13)
            if (s.cls != C::TacticalBribe) {
                const ClipTiming ct = clip_timing(o.id, kAnimHacking, {});
                len = ct.count > 0.0f ? static_cast<uint32_t>(std::ceil(ct.end / ct.rate)) : static_cast<uint32_t>(kHackFrames);
            }
            sp.next = frame + len;
            start_recharge(o, s.cls == C::Hack ? kTargetedHack : (s.cls == C::Repair ? kTargetedRepair : kTacticalBribe));
            stop_order(o.id, frame);
            event(AbilityEventKind::EffectOn, sp.trigger, o.id, target, frame);
            if (s.cls == C::TacticalBribe) class_update(o, sp, frame);  // changes the owner at once
            return true;
        }

        case C::VehicleThief: {  // in [min, max] (2-D - radius) and one synced draw < chance
            if (!alive_unit(target)) return false;
            const float r = combat_->data().units[combat_->desc_index(target)].radius;
            const float d = combat::distance_xy(me, combat_->position(target)) - r;
            if (d < s.min_range || d > s.max_range) return false;
            if (!chance_roll_lt(s.activation_chance)) return false;
            // the victim's capture variant (+0xda8; the host resolves it), at the victim with its owner (one id); the
            // victim is removed [port: dies without a death clone]; VEHICLE_THIEF boards the thief, CAPTURE_VEHICLE on.
            SpawnRequest q;
            q.purpose = SpawnPurpose::CaptureVariant;
            q.position = combat_->position(target);
            q.facing = combat_->facing(target);
            q.owner = combat_->owner(target);
            q.source = o.id;
            q.target = target;
            q.ability_type = kCaptureVehicle;
            request_spawn(q);
            combat_->kill_unit(target, o.id, combat::kKindNoDeathClone, frame);
            if (has(o, kCaptureVehicle)) set_active(o, kCaptureVehicle, true, frame, kNoObject);
            event(AbilityEventKind::EffectOn, kCaptureVehicle, o.id, target, frame);
            return true;
        }

        case C::MaximumFirepower: {  // 45-driven pulses until int(FPS x 45's Expiration_Seconds) or the target is lost [M]
            if (!alive_unit(target)) return false;
            sp.applied = 1;
            sp.target = target;
            o.active[kMaximumFirepower] = 1;
            start_active(o, kMaximumFirepower);
            sync_combat(o);
            if (o.combat_unit) combat_->order_attack(o.id, target);
            event(AbilityEventKind::EffectOn, kMaximumFirepower, o.id, target, frame);
            return true;
        }

        case C::CorruptSystems: {  // attaches CORRUPT_SYSTEMS (0x58) to the victim; recharge 0x4a
            if (!alive_unit(target) || !range_ok(o, s, target)) return false;
            Obj* v = obj(target);
            if (!v) return false;
            v->corrupted = 1;
            sp.applied = 1;
            sp.target = target;
            sp.counter = frames_of(s.duration_secs, fps);
            start_recharge(o, kCorruptSystems);
            event(AbilityEventKind::EffectOn, kCorruptSystems, o.id, target, frame);
            return true;
        }

        case C::BuzzDroids:
        case C::Summon:
        case C::Spawn:
        case C::Radioactive: {  // create Object_Type (one id) after Creation_Delay / Spray_Time / Activation_Time
            if (target != kNoObject && combat_->has_unit(target) && !range_ok(o, s, target)) return false;
            sp.applied = 1;
            sp.point = (target != kNoObject && combat_->has_unit(target)) ? combat_->position(target) : me;
            const float delay = s.cls == C::BuzzDroids ? s.activation_time : s.creation_delay;
            sp.next = frame + static_cast<uint32_t>(std::max(0, frames_of(delay, fps)));
            const uint8_t t = s.cls == C::BuzzDroids ? kBuzzDroids : (s.cls == C::Radioactive ? kRadioactiveContaminate : kSummon);
            start_recharge(o, t);
            event(AbilityEventKind::EffectOn, t, o.id, target, frame);
            return true;
        }

        case C::ClusterBomb: {  // 12 x Bomb_Type armed with Detonation_Time_In_Secs; recharge 0x3d
            if (s.projectile < 0) return false;
            const int32_t n = s.spawn_count > 0 ? s.spawn_count : 12;
            for (int32_t i = 0; i < n; ++i) {  // [port] scatter of the bombs not traced: all at the user
                combat::ProjectileLaunch l;
                l.shooter = o.combat_unit ? o.id : kNoObject;
                l.owner = o.owner;
                l.projectile = static_cast<uint32_t>(s.projectile);
                l.from = me;
                l.point = me;
                l.fuse_seconds = s.fuse_time;
                combat_->launch_projectile(l, frame);
            }
            start_recharge(o, kClusterBomb);
            event(AbilityEventKind::EffectOn, kClusterBomb, o.id, kNoObject, frame);
            return true;
        }

        case C::RemoteBomb: {  // apply 0x719c90 (LOB_SOLVER §3.5); DETONATE_REMOTE_BOMB (65) sets the bombs off (0x280ec0)
            if (s.projectile < 0) return false;
            if (sp.applied) return false;  // [port] a toss is running (retail would overwrite the pending pack)
            const bool has_target = target != kNoObject && combat_->has_unit(target);
            // Range tests 0x48d820 / 0x48e100 against the target's garrison host when it is garrisoned (0x39b1a0); the solve
            // uses the target itself: Bomb_Type needs a lob solution from the unit's position to the target's position +
            // Ranged_Target_Z_Adjust (0x398440).
            if (has_target && !range_ok(o, s, remote_bomb_range_target(target))) return false;
            if (has_target && !combat_->lob_solution(static_cast<uint32_t>(s.projectile), me, combat_->target_point(target)))
                return false;
            // S8 t06 / t10: face (the yaw snapped to the target's raw position, LOB_SOLVER §3.5 "Facing at apply"), stop,
            // movement lock, TARGETING disabled [port: its targets cleared, as Grenade_Attack]; pending "waiting to stop" with
            // the target. No toss and no recharge yet (class_update). [port] Without a target object the bomb is placed at the
            // unit at the toss.
            if (has_target) snap_face(o, target);
            stop_order(o.id, frame);
            lock_pair(o, kLockRemoteBomb, true, frame);
            if (o.combat_unit) combat_->clear_targets(o.id);
            sp.applied = 1;
            sp.target = has_target ? target : kNoObject;
            sp.phase = 0;
            sp.next = frame;
            sp.counter = 0;
            event(AbilityEventKind::EffectOn, kPlaceRemoteBomb, o.id, target, frame);  // the presentation (0x48e4d0)
            return true;
        }

        case C::FlameThrower: {  // cone Damage_Arc, Damage_Amount over Fire_Time after Damage_Delay; recharge 16
            if (!alive_unit(target) || !range_ok(o, s, target)) return false;
            sp.applied = 1;
            sp.target = target;
            sp.next = frame + static_cast<uint32_t>(std::max(0, frames_of(s.fire_delay > 0.0f ? s.fire_delay : 0.0f, fps)));
            sp.counter = std::max(1, frames_of(s.duration_secs, fps));
            start_recharge(o, kFlameThrower);
            event(AbilityEventKind::EffectOn, kFlameThrower, o.id, target, frame);
            return true;
        }

        case C::ShieldFlare: {  // after Charge_Up_Seconds a shield-scaled blast capped by Max_Damage_Per_Victim
            sp.applied = 1;
            sp.next = frame + static_cast<uint32_t>(std::max(0, frames_of(s.charging_time, fps)));
            start_recharge(o, kShieldFlare);
            event(AbilityEventKind::EffectOn, kShieldFlare, o.id, kNoObject, frame);
            return true;
        }

        case C::Infection: {  // throws Infection_Projectile (Causes_Infection: combat attaches INFECTION on the hit, refix t02)
            if (!alive_unit(target) || !range_ok(o, s, target) || s.projectile < 0) return false;
            combat::ProjectileLaunch l;
            l.shooter = o.combat_unit ? o.id : kNoObject;
            l.owner = o.owner;
            l.projectile = static_cast<uint32_t>(s.projectile);
            l.from = me;
            l.target = target;
            combat_->launch_projectile(l, frame);
            start_recharge(o, kInfection);
            event(AbilityEventKind::EffectOn, kInfection, o.id, target, frame);
            return true;
        }

        case C::SaberThrow:
        case C::Cable: {  // recharge (+ the cable circling start) [port: the thrown saber / cable phases are not modelled]
            if (!alive_unit(target) || !range_ok(o, s, target)) return false;
            start_recharge(o, s.cls == C::SaberThrow ? kSaberThrow : kCableAttack);
            if (s.cls == C::Cable && host_.cable_attack) host_.cable_attack(o.id, target, s.min_range, s.max_range);  // §6.5
            event(AbilityEventKind::Unhandled, sp.trigger, o.id, target, frame);
            return true;
        }

        case C::WeaponSwap:  // Permanent_Weapon_Swap (TACTICAL_GAPS 2.3): obj +0x324 := Weapon_Index [port: team -> members]
            sp.applied = 1;
            if (o.combat_unit) {
                combat_->set_weapon_index(o.id, s.weapon_index);
            } else {
                for (ObjectId m : o.members)
                    if (combat_->has_unit(m)) combat_->set_weapon_index(m, s.weapon_index);
            }
            event(AbilityEventKind::EffectOn, sp.trigger, o.id, kNoObject, frame);
            return true;

        case C::CancelSpecial:  // cancels another named special of the owner [M]
            for (SpecialState& q : o.specials)
                if (&q != &sp && q.applied && data_->specials[q.desc].name_crc == s.ability_name_crc) remove_special(o, q, frame);
            return true;

        default: return false;
    }
}

// ================================================================================================
// update (vf4, at the class interval)
// ================================================================================================

void AbilitySystem::class_update(Obj& o, SpecialState& sp, uint32_t frame) {
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    const uint32_t fps = cfg_.fps;
    const Vec3 me = position(o);
    switch (s.cls) {
        case C::ForceHealing: heal_pass(o, s, frame); return;

        case C::StarbaseUpgrade: {  // 0x70ec20: once (SAP +0x230), then Kill(upgrade object, flag 1) whatever happened
            if (sp.phase != 0) return;
            sp.phase = 1;
            // §1.2 step 1: the object itself when it is a station, else its parent container (+0x2b0) when that is one.
            auto is_station = [&](ObjectId id) {
                return id != kNoObject && alive_unit(id) && combat_->data().units[combat_->desc_index(id)].repairs_hardpoints;
            };
            ObjectId st = kNoObject;
            if (is_station(o.id)) {
                st = o.id;
            } else if (host_.upgrade_container) {
                st = host_.upgrade_container(o.id);
            } else if (is_station(o.container)) {
                st = o.container;
            } else {  // [port] no host query: the owner's lowest-id live station
                for (size_t i = 0; i < combat_->unit_count() && st == kNoObject; ++i) {
                    const ObjectId u = combat_->unit_id_at(i);
                    if (is_station(u) && combat_->owner(u) == o.owner) st = u;
                }
            }
            if (is_station(st)) {  // steps 2-11 are the host's (SpawnPurpose::StationUpgrade)
                SpawnRequest q;
                q.purpose = SpawnPurpose::StationUpgrade;
                q.position = combat_->position(st);
                q.facing = combat_->facing(st);
                q.owner = combat_->owner(st);
                q.source = st;
                q.target = o.id;
                request_spawn(q);
                sp.target = st;
            }
            event(AbilityEventKind::RemoveSilently, 0, o.id, kNoObject, frame);
            return;
        }

        case C::Berserker: {  // 2.3.2 update
            sp.counter -= 1;
            if (sp.counter < 1) {
                remove_special(o, sp, frame);
                return;
            }
            // Damage pass every 5th update while the looping animation 0x68 is strictly inside 20 %-80 % of the clip
            // (§4.13 [port]: loops wrap at count - 1; position from the frames since the apply; no clip: always open).
            bool window = true;
            if (const ClipTiming ct = clip_timing(o.id, kAnimBerserk, {}); ct.count > 1.0f) {
                const float n = static_cast<float>(frames_of(s.berserker_duration, fps) - sp.counter);
                const float pos = std::fmod(n * ct.rate, ct.count - 1.0f);
                window = pos > 0.2f * ct.count && pos < 0.8f * ct.count;
            }
            if (sp.counter % 5 == 0 && window) {
                for (size_t i = 0; i < combat_->unit_count(); ++i) {
                    const ObjectId u = combat_->unit_id_at(i);
                    if (!alive_unit(u) || !enemy(o.owner, combat_->owner(u))) continue;
                    const combat::UnitDesc& ud = combat_->data().units[combat_->desc_index(u)];
                    if (!ud.has_locomotor || (combat_->flags(u) & combat::kUnitSkipTargeting)) continue;
                    if (combat::distance3(me, combat_->position(u)) - ud.radius > s.berserker_damage_range) continue;
                    DamageRequest r;
                    r.victim = u;
                    r.amount = s.berserker_damage;
                    r.kind = kKind16;
                    r.source = o.id;
                    combat_->take_damage(r, frame);
                    if (o.dead) return;
                }
            }
            // target upkeep: drop it when not moving and beyond Damage_Range; pick the nearest acceptable enemy with
            // (squared distance - radius) < (1.3 x Activation_Max_Range)^2 (mixed units - the quirk is kept) and move to it.
            bool keep = alive_unit(sp.target);
            if (keep && !moving(o) && combat::distance3(me, combat_->position(sp.target)) > s.berserker_damage_range) keep = false;
            if (!keep) {
                ObjectId best = kNoObject;
                float best_d = 0.0f;
                const float lim = 1.3f * s.max_range;
                for (size_t i = 0; i < combat_->unit_count(); ++i) {
                    const ObjectId u = combat_->unit_id_at(i);
                    if (!class_accepts(o, s, u, frame)) continue;
                    const float d2 = combat::distance2_xy(me, combat_->position(u)) - combat_->data().units[combat_->desc_index(u)].radius;
                    if (!(d2 < lim * lim)) continue;
                    if (best == kNoObject || d2 < best_d) {
                        best = u;
                        best_d = d2;
                    }
                }
                if (best == kNoObject) {
                    remove_special(o, sp, frame);
                    return;
                }
                sp.target = best;
                move_order(o.id, combat_->position(best), frame);
            }
            return;
        }

        case C::Earthquake:
        case C::Telekinesis: {  // the target behaviour: Take_Damage(kind 3, D) every interval [port: no ramp state]
            if (!alive_unit(sp.target) || !range_ok(o, s, sp.target)) {
                remove_special(o, sp, frame);
                return;
            }
            if (frame >= sp.next) {
                sp.next = frame + static_cast<uint32_t>(std::max(1, frames_of(s.damage_interval, fps)));
                DamageRequest r;
                r.victim = sp.target;
                r.amount = sp.value;
                r.kind = kKind3;
                r.source = o.id;
                combat_->take_damage(r, frame);
            }
            return;
        }

        case C::GrenadeAttack: {  // 2.3.5 update: toss once not moving, throw at Spawn_Frame (one projectile)
            if (sp.phase == 0) {
                if (moving(o)) return;
                sp.phase = 1;
                sp.next = frame;  // toss animation start (-1 = at once)
            }
            const ClipTiming ct = clip_timing(o.id, -1, s.toss_anim);
            const float anim = static_cast<float>(frame - sp.next) * ct.rate;
            if (sp.phase == 1 && anim >= s.spawn_frame) {
                sp.phase = 2;  // thrown
                bool spawned = false;
                if (alive_unit(sp.target) && s.projectile >= 0) {
                    combat::ProjectileLaunch l;
                    l.shooter = o.combat_unit ? o.id : kNoObject;
                    l.owner = o.owner;
                    l.projectile = static_cast<uint32_t>(s.projectile);
                    // S8 t04 (TACTICAL_GAPS §2.3.5, LOB_SOLVER §5): thrown from the unit through the spawner (the
                    // grenade branch: solve, arc, nothing without an arc) with its BOMB armed with the timer. [port] No
                    // Spawn_Bone: the launch point is the unit's position + its Ranged_Target_Z_Adjust.
                    l.from = o.combat_unit ? combat_->target_point(o.id) : position(o);
                    l.target = sp.target;
                    l.point = combat_->position(sp.target);
                    l.fuse_seconds = s.fuse_time > 0.0f ? s.fuse_time : -1.0f;  // [port] no timer: impact only
                    l.toss = true;
                    spawned = combat_->launch_projectile(l, frame);
                }
                // S8 t06 (LOB_SOLVER §5.2): the throw 0x700a70 returns 0 on a null spawn - the ability is not set active
                // and no recharge starts; the animation still ends the ability as usual.
                if (spawned) {
                    o.active[kStickyBomb] = 1;
                    start_recharge(o, kStickyBomb);
                }
            }
            // animation end (no Toss_Anim clip: at the throw): targeting / locomotion resume, unlock
            if (sp.phase == 2 && (ct.count <= 0.0f || anim >= ct.end)) remove_special(o, sp, frame);
            return;
        }

        case C::RemoteBomb: {  // update 0x719ee0 (LOB_SOLVER §3.5); phase 0 waiting to stop, 1 Toss_Anim, 2 tossed
            if (sp.phase == 0) {  // 1. wait while the locomotor still moves, then start the toss
                if (moving(o)) return;
                sp.phase = 1;
                sp.next = frame;
                if (s.toss_anim.empty()) {  // Toss_Anim -1: toss at once; no animation runs, so the same service ends it
                    remote_bomb_toss(o, s, sp.target, frame);
                    sp.phase = 2;
                    remove_special(o, sp, frame);
                    return;
                }
                // Toss_Anim one-shot (0x3a92f0, speed 1). sp.counter = the play was recorded (clip data): later plays that
                // replace it are seen as an interruption.
                play_anim(o, -1, s.toss_anim, 0, false, 1.0f, kAnimIdle, frame);
                const int32_t tt = anim_type_of(s.toss_anim);
                sp.counter = (tt >= 0 && o.anim_type == tt && o.anim_start == frame) ? 1 : 0;
            }
            // 3. the animation was replaced: the same cleanup, no toss.
            if (sp.counter != 0 && (o.anim_type != anim_type_of(s.toss_anim) || o.anim_start != sp.next)) {
                remove_special(o, sp, frame);
                return;
            }
            // 2. animation frame >= Spawn_Frame (n x rate, §4.13; [port] no clip: rate 1): toss once (0x71a200, the
            // recharge only when a bomb spawned); at its end (no clip: at the toss) unlock and end the ability.
            const ClipTiming ct = clip_timing(o.id, -1, s.toss_anim);
            const float anim = static_cast<float>(frame - sp.next) * ct.rate;
            if (sp.phase == 1 && anim >= s.spawn_frame) {
                sp.phase = 2;
                remote_bomb_toss(o, s, sp.target, frame);
            }
            if (ct.count > 0.0f ? anim >= ct.end : sp.phase == 2) remove_special(o, sp, frame);
            return;
        }

        case C::ForceWhirlwind:
        case C::StunWave:
        case C::ForceConfuse: {  // the wave grows by its step per update up to the range
            const float range = s.cls == C::ForceWhirlwind ? s.damage_range : s.wave_range;
            wave_victims(o, sp, sp.radius, scratch_);
            for (ObjectId v : scratch_) {
                sp.victims.push_back(v);
                if (s.cls == C::ForceWhirlwind) {
                    DamageRequest r;
                    r.victim = v;
                    r.amount = s.damage_amount;
                    r.kind = kKind2;
                    r.source = o.id;
                    combat_->take_damage(r, frame);
                } else if (s.cls == C::StunWave) {
                    combat_->stun_unit(v, static_cast<int32_t>(s.wave_time * 30.0f), frame);  // literal 30.0
                } else if (Obj* vo = obj(v)) {
                    confuse(*vo, s.wave_time, frame);  // 2 synced draws per victim
                }
                if (o.dead) return;
            }
            if (!(sp.radius < range)) {
                remove_special(o, sp, frame);
                return;
            }
            sp.radius = std::min(range, sp.radius + sp.value);
            return;
        }

        case C::ArcSweep:
        case C::GenericAttack:
        case C::EatAttack: {  // Attack_Animation on sim frames x rate x Attack_Animation_Speedup (§4.13)
            const ClipTiming ct =
                clip_timing(o.id, -1, s.attack_anim, s.cls == C::ArcSweep ? static_cast<uint32_t>(std::max(0, s.anim_sub_index)) : 0u);
            const float anim = static_cast<float>(frame - sp.next) * ct.rate * s.anim_speedup;
            // the hold ends at the animation end, or with the last frame-numbered event when there is no clip
            const bool ended = ct.count <= 0.0f || anim >= ct.end;
            if (s.cls == C::GenericAttack) {
                if (sp.phase == 0 && anim >= static_cast<float>(s.damage_frame)) {
                    sp.phase = 1;  // hit
                    if (alive_unit(sp.target)) {
                        DamageRequest r;
                        r.victim = sp.target;
                        r.amount = max_health_and_shield(sp.target) * s.damage_percentage + s.damage_amount;
                        r.damage_type = s.damage_type;
                        r.source = o.id;
                        combat_->take_damage(r, frame);
                    }
                }
                if (sp.phase == 1 && ended && !o.dead) remove_special(o, sp, frame);
                return;
            }
            if (s.cls == C::EatAttack) {
                if (sp.phase == 0 && anim >= static_cast<float>(s.attachment_frame)) {
                    sp.phase = 1;  // grab: the victim is held (its targeting off) [port: no bone attachment / re-own]
                    if (alive_unit(sp.target)) stop_order(sp.target, frame);
                }
                if (sp.phase == 1 && anim >= static_cast<float>(s.destruction_frame)) {
                    sp.phase = 2;  // destroyed
                    if (alive_unit(sp.target)) {
                        DamageRequest r;
                        r.victim = sp.target;
                        r.amount = 1000000.0f;
                        r.damage_type = s.damage_type;
                        r.source = o.id;
                        combat_->take_damage(r, frame);
                    }
                    if (o.combat_unit && !o.dead) {
                        const float mh = combat_->max_health(o.id);
                        combat_->set_health(o.id, std::min(mh, combat_->health(o.id) + s.health_per_victim));
                    }
                }
                if (sp.phase == 2 && ended && !o.dead) remove_special(o, sp, frame);
                return;
            }
            // Arc_Sweep: from Begin_Applying_Damage_Frame each update damages the accepted objects in this update's
            // slice of the arc whose (2-D distance - radius) is in [Arc_Min, Arc_Max] (within 1.5 x Arc_Max).
            if (sp.phase == 1) {  // swept: held until the animation end
                if (ended) remove_special(o, sp, frame);
                return;
            }
            if (anim < static_cast<float>(s.begin_damage_frame)) return;
            const float step = s.sweep_time > 0.0f
                                   ? static_cast<float>(std::max(1, interval_of(s))) * (s.arc_width / s.sweep_time) /
                                         static_cast<float>(fps) * s.anim_speedup
                                   : s.arc_width;
            const float a0 = sp.radius;
            const float a1 = s.right_to_left ? a0 - step : a0 + step;
            const float lo = std::min(a0, a1), hi = std::max(a0, a1);
            const Vec3 f = o.combat_unit ? combat_->facing(o.id) : Vec3{1.0f, 0.0f, 0.0f};
            for (size_t i = 0; i < combat_->unit_count(); ++i) {
                const ObjectId u = combat_->unit_id_at(i);
                if (!class_accepts(o, s, u, frame)) continue;
                if (std::find(sp.victims.begin(), sp.victims.end(), u) != sp.victims.end()) continue;
                const Vec3 up = combat_->position(u);
                const float d = combat::distance_xy(me, up) - combat_->data().units[combat_->desc_index(u)].radius;
                if (d > 1.5f * s.arc_max || d < s.arc_min || d > s.arc_max) continue;
                const float b = bearing_deg(f, me, up);
                if (b < lo || b > hi) continue;
                sp.victims.push_back(u);
                DamageRequest r;
                r.victim = u;
                r.amount = max_health_and_shield(u) * s.damage_percentage + s.damage_amount;
                r.damage_type = s.damage_type;
                r.source = o.id;
                combat_->take_damage(r, frame);
                if (o.dead) return;
            }
            sp.radius = a1;
            if (std::abs(a1) >= 0.5f * s.arc_width || step <= 0.0f) {
                sp.phase = 1;
                if (ended) remove_special(o, sp, frame);
            }
            return;
        }

        case C::SuperLaser: {  // 2.3.9 update
            if (!alive_unit(sp.target) || !range_ok(o, s, sp.target)) {
                remove_special(o, sp, frame);
                return;
            }
            if (sp.phase == 0) {
                if (frame < sp.next) return;
                sp.phase = 1;  // inside the firing cone: the attack animation starts
                sp.next = frame;
                // §4.14: type 5, index Attack_Anim_Sub_Index, one-shot (parks at its end)
                play_anim(o, kAnimAttack, {}, static_cast<uint32_t>(std::max(0, s.anim_sub_index)), false, 1.0f, kAnimStay, frame);
            }
            if (sp.phase == 1 &&
                static_cast<float>(frame - sp.next) *
                        clip_timing(o.id, kAnimAttack, {}, static_cast<uint32_t>(std::max(0, s.anim_sub_index))).rate >=
                    static_cast<float>(s.fire_frame)) {  // Fire_Frame on the sub-index variant's clip (§4.13 / §4.14)
                sp.phase = 2;
                start_active(o, kSuperLaser);  // 55's active countdown
            }
            if (sp.phase == 2) {
                const bool running = o.cd.phase[kSuperLaser] == 1 && o.cd.counter[kSuperLaser] < o.cd.maximum[kSuperLaser];
                if (!running && desc(o).countdown && o.cd.maximum[kSuperLaser] > 0) {
                    remove_special(o, sp, frame);
                    return;
                }
                DamageRequest r;  // every frame while it runs
                r.victim = sp.target;
                r.amount = s.damage_per_frame;
                r.kind = combat::kKindNormal;
                r.source = o.id;
                combat_->take_damage(r, frame);
            }
            return;
        }

        case C::DrainLife: {  // per frame each victim takes DPS / FPS; Should_Heal heals the owner by the same
            sp.counter -= 1;
            const float per = s.damage_per_second / static_cast<float>(fps);
            bool any = false;
            for (ObjectId v : sp.victims) {
                if (!alive_unit(v)) continue;
                if (s.charging_time > 0.0f && combat::distance_xy(me, combat_->position(v)) > s.targets_radius) {
                    sp.counter = 0;  // a victim leaving the radius ends it (charging variant)
                    break;
                }
                any = true;
                DamageRequest r;
                r.victim = v;
                r.amount = per;
                r.kind = s.charging_time > 0.0f ? kKind4 : kKind16;
                r.source = o.id;
                combat_->take_damage(r, frame);
                if (o.dead) return;
                if (s.should_heal && o.combat_unit) {
                    const float mh = combat_->max_health(o.id);
                    if (combat_->health(o.id) < mh) combat_->set_health(o.id, std::min(mh, combat_->health(o.id) + per));
                }
            }
            if (sp.counter <= 0 || !any) remove_special(o, sp, frame);
            return;
        }

        case C::LeechShields: {  // vf4 0x717d60: shield drained by SDPS / FPS while Can_Activate(58), in range, accepted
            sp.counter -= 1;
            if (!can(o, kLeechShields, false, true, frame) || !alive_unit(sp.target) || !range_ok(o, s, sp.target) ||
                !accepts(o, s, sp.target, frame) || sp.counter <= 0) {
                remove_special(o, sp, frame);
                return;
            }
            // [port] the drain sets the shield directly; 0x549ac0's depleted-shield lockout stamp is not applied.
            const float sh = combat_->shield(sp.target) - s.damage_per_second / static_cast<float>(fps);
            combat_->set_shield(sp.target, sh > 0.0f ? sh : 0.0f);
            if (!(sh > 0.0f)) remove_special(o, sp, frame);
            return;
        }

        case C::Blast: {
            if (sp.phase == 0) {
                sp.counter -= 1;
                if (sp.counter > 0) return;
                // update 0x71c190 at 0: charging particle destroyed, "charged" mesh + particle, counter := -K (K = the
                // WEAPON_SPECIAL hard points, 0x397c10). Fire_At then lets only those fire, x Damage_Multiplier, each
                // shot counting up to 0 (fire_gate / fire_done).
                sp.phase = 1;
                sp.counter = o.combat_unit ? -static_cast<int32_t>(combat_->live_hardpoints(o.id, HardpointKind::WeaponSpecial)) : 0;
                if (o.combat_unit) combat_->set_flag(o.id, combat::kUnitSkipTargeting, false);  // [port] +0x3a1 cleared here
                event(AbilityEventKind::EffectOn, kBlast, o.id, kNoObject, frame);
                if (sp.counter == 0) remove_special(o, sp, frame);  // no special hard point: normal fire resumes at once
            }
            return;
        }

        case C::ForceCloak:  // as Stealth (0x592e80)
            if (!o.stealthed) {
                sp.applied = 0;
                return;
            }
            o.stealth_frames -= 1;
            if (o.stealth_frames <= 0) {
                remove_special(o, sp, frame);
                return;
            }
            detect(o, frame);
            refresh_stealth_flag(o, frame);
            return;

        case C::Hack:
        case C::Repair:
        case C::TacticalBribe: {  // at the animation end: Hack / Bribe change the target's owner (and its container's)
            if (frame < sp.next) return;
            if (alive_unit(sp.target)) {
                if (s.cls == C::Repair) {
                    combat_->set_health(sp.target, combat_->max_health(sp.target));  // [M] the repair effect
                } else if (Obj* t = obj(sp.target)) {
                    change_owner(*t, o.owner, frame);
                }
            }
            remove_special(o, sp, frame);
            return;
        }

        case C::MaximumFirepower: {
            const bool running = o.cd.phase[kMaximumFirepower] == 1 && o.cd.counter[kMaximumFirepower] < o.cd.maximum[kMaximumFirepower];
            if (!alive_unit(sp.target) || (!running && o.cd.maximum[kMaximumFirepower] > 0)) remove_special(o, sp, frame);
            return;
        }

        case C::CorruptSystems: {  // the victim's CORRUPT_SYSTEMS behaviour: shield drain per second, expiry
            sp.counter -= 1;
            if (alive_unit(sp.target)) {
                const float sh = combat_->shield(sp.target) - s.damage_per_second / static_cast<float>(fps);
                combat_->set_shield(sp.target, sh > 0.0f ? sh : 0.0f);
            }
            if (sp.counter <= 0 || !alive_unit(sp.target)) remove_special(o, sp, frame);
            return;
        }

        case C::BuzzDroids:
        case C::Summon:
        case C::Spawn:
        case C::Radioactive: {
            if (frame < sp.next) return;
            if (s.spawn_type >= 0) {
                SpawnRequest q;
                q.purpose = SpawnPurpose::Summon;
                q.object_type = s.spawn_type;
                q.position = sp.point;
                q.facing = o.combat_unit ? combat_->facing(o.id) : Vec3{1.0f, 0.0f, 0.0f};
                q.owner = o.owner;
                q.source = o.id;
                q.ability_type = sp.trigger;
                request_spawn(q);
            }
            remove_special(o, sp, frame);
            return;
        }

        case C::FlameThrower: {  // after the delay: per frame each enemy in range inside the cone [M]
            if (frame < sp.next) return;
            sp.counter -= 1;
            const Vec3 f = o.combat_unit ? combat_->facing(o.id) : Vec3{1.0f, 0.0f, 0.0f};
            const float per = s.damage_amount / static_cast<float>(std::max(1, frames_of(s.duration_secs, fps)));
            for (size_t i = 0; i < combat_->unit_count(); ++i) {
                const ObjectId u = combat_->unit_id_at(i);
                if (!alive_unit(u) || !enemy(o.owner, combat_->owner(u))) continue;
                const Vec3 up = combat_->position(u);
                if (combat::distance_xy(me, up) > s.max_range) continue;
                if (!facing_within(f, me, up, 0.5f * s.damage_arc)) continue;
                DamageRequest r;
                r.victim = u;
                r.amount = per;
                r.kind = combat::kKindFire;
                r.source = o.id;
                combat_->take_damage(r, frame);
                if (o.dead) return;
            }
            if (sp.counter <= 0) remove_special(o, sp, frame);
            return;
        }

        case C::ShieldFlare: {  // shield-scaled Take_Damage(kind 1) capped by Max_Damage_Per_Victim within Damage_Radius
            if (frame < sp.next) return;
            const float frac = o.combat_unit ? combat_->shield_fraction(o.id) : 0.0f;
            const float amount = s.max_damage_per_victim * frac;  // [inferred: scaled by the owner's shield fraction]
            for (size_t i = 0; i < combat_->unit_count(); ++i) {
                const ObjectId u = combat_->unit_id_at(i);
                if (!alive_unit(u) || !enemy(o.owner, combat_->owner(u))) continue;
                if (combat::distance3(me, combat_->position(u)) > s.damage_range) continue;
                DamageRequest r;
                r.victim = u;
                r.amount = amount;
                r.kind = combat::kKindNormal;
                r.source = o.id;
                combat_->take_damage(r, frame);
                if (o.dead) return;
            }
            if (o.combat_unit) combat_->set_shield(o.id, 0.0f);
            remove_special(o, sp, frame);
            return;
        }
        default: return;
    }
}

// ================================================================================================
// remove (vf31)
// ================================================================================================

void AbilitySystem::class_remove(Obj& o, SpecialState& sp, uint32_t frame) {
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    switch (s.cls) {
        case C::CombatBonus:
        case C::EnhanceDefense:
        case C::BattlefieldModifier:
        case C::BasePower:
        case C::Weatherproof:
        case C::RetreatPrevention:
        case C::EnableAbility: aura_off(o, sp, frame); return;
        case C::SuperLaser:
            o.active[kSuperLaser] = 0;
            start_recharge(o, kSuperLaser);  // recharge at the end (0x42fb70, §1.11)
            sync_combat(o);
            break;
        case C::LeechShields:  // remove 0x717f70: 58 off, the list-1 entries of this source deleted, effect destroyed
            o.active[kLeechShields] = 0;
            if (o.combat_unit)
                if (combat::ModifierSet* m = combat_->unit_modifiers(o.id)) m->remove_source_in_list(ModList::Damage, o.id, s.name_crc);
            sync_combat(o);
            break;
        case C::MaximumFirepower:
            o.active[kMaximumFirepower] = 0;
            start_recharge(o, kMaximumFirepower);
            sync_combat(o);
            break;
        case C::CorruptSystems:
            if (Obj* v = obj(sp.target)) v->corrupted = 0;
            break;
        case C::ForceCloak:
            if (o.stealthed) break_stealth(o, frame);
            break;
        case C::Blast:
            if (o.combat_unit && combat_->has_unit(o.id)) combat_->set_flag(o.id, combat::kUnitSkipTargeting, false);
            break;
        case C::GrenadeAttack:  // vf4 0x700d00: targeting / locomotor re-enabled, unlock (A1_READINGS §28.9)
            lock_pair(o, kLockGrenade, false, frame);
            break;
        case C::RemoteBomb:  // S8 t06 (0x719ee0 end / interruption): TARGETING and the locomotor enabled, unlock
            lock_pair(o, kLockRemoteBomb, false, frame);
            break;
        case C::Earthquake:  // §4.14 remove: FB_Release / FTK_Release one-shot, then the idle loop
        case C::Telekinesis:
            play_anim(o, s.cls == C::Earthquake ? kAnimFbRelease : kAnimFtkRelease, {}, 0, false, 1.0f, kAnimIdle, frame);
            break;
        default: break;
    }
    sp.applied = 0;
    sp.phase = 0;
    sp.counter = 0;
    sp.victims.clear();
}

// ================================================================================================
// Remote_Bomb toss (S8 t06, docs/re/LOB_SOLVER.md §3.5, §5.1, §5.2)
// ================================================================================================

ObjectId AbilitySystem::remote_bomb_range_target(ObjectId target) const {
    // 0x39b1a0 / obj +0x2b0: a garrisoned target is range-tested through its host.
    if (target == kNoObject || !combat_->has_unit(target) || !(combat_->flags(target) & combat::kUnitGarrisoned)) return target;
    if (!host_.garrison_holder) return target;  // [port] no host query: the target itself
    const ObjectId h = host_.garrison_holder(target);
    return h != kNoObject ? h : target;
}

Vec3 AbilitySystem::remote_bomb_launch(Obj& o, const SpecialAbilityDesc& s, ObjectId target) {
    // Spawn_Projectile launch point L (§5.1). A garrisoned thrower whose host lists gun bones: a synced pick
    // (Get_Int(0, n - 1), one draw) among the bones within 60 degrees (horizontal) of the host -> target direction [M: the
    // angle measured from the host position]; none within -> the normal muzzle [M].
    if (o.combat_unit && combat_->has_unit(o.id) && (combat_->flags(o.id) & combat::kUnitGarrisoned) && host_.garrison_holder &&
        host_.garrison_fire_bones && combat_->has_unit(target)) {
        const ObjectId holder = host_.garrison_holder(o.id);
        Vec3 hp;
        std::vector<Vec3> bones;
        if (holder != kNoObject && host_.garrison_fire_bones(holder, hp, bones) && !bones.empty()) {
            const Vec3 tp = combat_->position(target);
            const float tx = tp.x - hp.x, ty = tp.y - hp.y;
            const float tl = dmath::sqrt(tx * tx + ty * ty);
            std::vector<Vec3> within;
            for (const Vec3& b : bones) {
                const float bx = b.x - hp.x, by = b.y - hp.y;
                const float bl = dmath::sqrt(bx * bx + by * by);
                // cos(angle) >= cos 60 = 0.5; a bone (or target) on the host's axis counts as within [port]
                if (!(bl > 0.0f) || !(tl > 0.0f) || (bx * tx + by * ty) >= 0.5f * (bl * tl)) within.push_back(b);
            }
            if (!within.empty()) return within[static_cast<size_t>(draw_int(0, static_cast<int32_t>(within.size()) - 1))];
        }
    }
    // The request's bone: Spawn_Bone on the unit's model (0x12d2a0); no such bone -> the shooter's position + its own
    // Ranged_Target_Z_Adjust (0x398440).
    if (!s.spawn_bone.empty() && host_.bone_position) {
        Vec3 p;
        if (host_.bone_position(o.id, s.spawn_bone, p) > 0) return p;
    }
    return o.combat_unit ? combat_->target_point(o.id) : position(o);
}

bool AbilitySystem::remote_bomb_toss(Obj& o, const SpecialAbilityDesc& s, ObjectId target, uint32_t frame) {
    // 0x71a200: Bomb_Type through the spawner (shooter = the unit, target = the target object, Spawn_Bone). Only when a
    // bomb spawned does the PLACE_REMOTE_BOMB recharge start (§5.2: a null spawn starts none and is not retried). The bomb
    // gets no BOMB timer (§6.4): only DETONATE_REMOTE_BOMB sets it off. [M] A target that died before the toss: no spawn.
    if (s.projectile < 0) return false;
    combat::ProjectileLaunch l;
    l.shooter = o.combat_unit ? o.id : kNoObject;
    l.owner = o.owner;
    l.projectile = static_cast<uint32_t>(s.projectile);
    if (target != kNoObject) {
        if (!alive_unit(target)) return false;
        l.from = remote_bomb_launch(o, s, target);
        l.target = target;
        l.point = combat_->position(target);
        l.toss = true;  // fuse_seconds stays < 0: never armed
    } else {  // [port] no target object (no command-57 point here): placed at the unit, armed until detonated
        l.from = position(o);
        l.point = l.from;
        l.fuse_seconds = 1.0e9f;
    }
    const bool spawned = combat_->launch_projectile(l, frame) && combat_->projectile_count() > 0;
    if (spawned) {
        remote_bombs_.push_back(RemoteBomb{o.owner, combat_->projectile(combat_->projectile_count() - 1).serial});
        start_recharge(o, kPlaceRemoteBomb);
    }
    // S8 t10 (LOB_SOLVER §3.5 "MARKER removal"): after the spawn attempt, whether or not a bomb spawned, a MARKER target
    // is killed (0x391310(marker, 1)); the host knows which objects are markers (its Dummy_Barrage_Targets).
    if (target != kNoObject) event(AbilityEventKind::MarkerTossed, kPlaceRemoteBomb, o.id, target, frame);
    return spawned;
}

// ================================================================================================
// Force_Healing (2.3.1), victim scans
// ================================================================================================

void AbilitySystem::heal_pass(Obj& o, const SpecialAbilityDesc& s, uint32_t frame) {
    // update 0x6fbb20: skipped when garrisoned (+0x3a7) [port: garrison-style objects]; fires when frame >= the
    // object's due frame (+0x25c, shared by its healing abilities); due := now + int(FPS x interval + 0.5).
    if (!o.combat_unit || o.dead) return;
    if (combat_->flags(o.id) & combat::kUnitGarrisoned) return;
    if (frame < o.heal_due) return;
    o.heal_due = frame + static_cast<uint32_t>(static_cast<int32_t>(static_cast<float>(cfg_.fps) * s.heal_interval + 0.5f));
    const bool heal = s.heal_amount > 0.0f || s.heal_percent > 0.0f;
    const bool harm = s.heal_amount < 0.0f && s.heal_percent <= 0.0f;
    if (!heal && !harm) return;
    const Vec3 me = position(o);
    auto fraction = [this](ObjectId id) { return combat_->script_hull(id); };  // combined fraction (0x396e60) [M]
    auto heal_one = [&](ObjectId id) {
        // 0x6fb940: the damaged targetable hardpoint with the LOWEST fraction, else the hull: max x percent + amount + cur.
        int32_t best = -1;
        float best_f = 2.0f;
        for (uint32_t k = 0; k < combat_->hardpoint_count(id); ++k) {
            if (!combat_->hardpoint_targetable(id, k)) continue;
            const float mx = combat_->hardpoint_max(id, k);
            if (!(mx > 0.0f)) continue;
            const float f = combat_->hardpoint_health(id, k) / mx;
            if (f < 1.0f && f < best_f) {
                best = static_cast<int32_t>(k);
                best_f = f;
            }
        }
        if (best >= 0) {
            const auto k = static_cast<uint32_t>(best);
            const float mx = combat_->hardpoint_max(id, k);
            combat_->set_hardpoint_health(id, k, mx * s.heal_percent + s.heal_amount + combat_->hardpoint_health(id, k));
        } else {
            const float mx = combat_->max_health(id);
            const float h = mx * s.heal_percent + s.heal_amount + combat_->health(id);
            combat_->set_health(id, h < mx ? h : mx);
        }
    };
    if (heal) {
        if (!(s.heal_range > 0.0f)) {
            if (fraction(o.id) < 1.0f) heal_one(o.id);
            return;
        }
        ObjectId single = kNoObject;
        float single_f = 0.0f;
        scratch_.clear();
        for (size_t i = 0; i < combat_->unit_count(); ++i) {
            const ObjectId u = combat_->unit_id_at(i);
            if (!alive_unit(u) || !allied(o.owner, combat_->owner(u))) continue;
            if (combat_->flags(u) & (combat::kUnitConverted | combat::kUnitSkipTargeting)) continue;
            if (combat::distance_xy(me, combat_->position(u)) > s.heal_range) continue;
            if (!(s.applicable_categories == 0 && s.applicable_types.empty()) && !target_filter(s, u)) continue;
            const float f = fraction(u);
            if (!(f < 1.0f)) continue;
            if (s.single_target_heal) {
                if (single == kNoObject || f < single_f) {  // strict <, first wins
                    single = u;
                    single_f = f;
                }
            } else {
                scratch_.push_back(u);
            }
        }
        if (s.single_target_heal) {
            if (single != kNoObject) heal_one(single);
        } else {
            for (ObjectId u : scratch_) heal_one(u);
        }
        return;
    }
    // harm (vf33 0x6fbc20): enemies in range with health > 0, not garrisoned: Take_Damage(kind 5, -amount); single =
    // the lowest hull fraction.
    ObjectId single = kNoObject;
    float single_f = 0.0f;
    scratch_.clear();
    for (size_t i = 0; i < combat_->unit_count(); ++i) {
        const ObjectId u = combat_->unit_id_at(i);
        if (!alive_unit(u) || !enemy(o.owner, combat_->owner(u))) continue;
        if (combat_->flags(u) & (combat::kUnitGarrisoned | combat::kUnitSkipTargeting)) continue;
        if (!(s.heal_range > 0.0f) || combat::distance_xy(me, combat_->position(u)) > s.heal_range) continue;
        if (!(s.applicable_categories == 0 && s.applicable_types.empty()) && !target_filter(s, u)) continue;
        const float mh = combat_->max_health(u);
        const float f = mh > 0.0f ? combat_->health(u) / mh : 0.0f;
        if (s.single_target_heal) {
            if (single == kNoObject || f < single_f) {
                single = u;
                single_f = f;
            }
        } else {
            scratch_.push_back(u);
        }
    }
    auto harm_one = [&](ObjectId u) {
        DamageRequest r;
        r.victim = u;
        r.amount = -s.heal_amount;
        r.kind = kKind5;
        r.source = o.id;
        combat_->take_damage(r, frame);
    };
    if (s.single_target_heal) {
        if (single != kNoObject) harm_one(single);
    } else {
        for (ObjectId u : scratch_) {
            harm_one(u);
            if (o.dead) return;
        }
    }
}

uint32_t AbilitySystem::wave_victims(Obj& o, SpecialState& sp, float radius, std::vector<ObjectId>& out) {
    // 0x2a9ff0 (mode 2) around the owner: enemy, applicable filter, alive, not 0x80 / 0x02, not garrisoned, not already
    // in the per-cast id list; ascending id.
    out.clear();
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    const Vec3 me = position(o);
    for (size_t i = 0; i < combat_->unit_count(); ++i) {
        const ObjectId u = combat_->unit_id_at(i);
        if (!alive_unit(u) || !enemy(o.owner, combat_->owner(u))) continue;
        if (combat_->flags(u) & (combat::kUnitGarrisoned | combat::kUnitSkipTargeting)) continue;
        if (!target_filter(s, u)) continue;
        if (std::find(sp.victims.begin(), sp.victims.end(), u) != sp.victims.end()) continue;
        if (combat::distance_xy(me, combat_->position(u)) > radius) continue;
        out.push_back(u);
    }
    return static_cast<uint32_t>(out.size());
}

void AbilitySystem::lightning_targets(Obj& o, const SpecialAbilityDesc& s, ObjectId primary, std::vector<ObjectId>& out) {
    // 0x6fcd60: the primary, then per enemy player (list order) that player's objects within the radius of the primary
    // (ascending id) passing accepts, up to Max_Targets.
    out.clear();
    const auto cap = static_cast<size_t>(s.max_targets > 0 ? s.max_targets : 1);
    if (alive_unit(primary)) out.push_back(primary);
    if (!alive_unit(primary)) return;
    const Vec3 c = combat_->position(primary);
    const auto np = static_cast<int32_t>(combat_->player_count());
    for (int32_t p = 0; p < np && out.size() < cap; ++p) {
        if (!enemy(o.owner, p)) continue;
        for (size_t i = 0; i < combat_->unit_count() && out.size() < cap; ++i) {
            const ObjectId u = combat_->unit_id_at(i);
            if (u == primary || combat_->owner(u) != p) continue;
            if (combat::distance_xy(c, combat_->position(u)) > s.targets_radius) continue;
            if (!class_accepts(o, s, u, 0)) continue;
            out.push_back(u);
        }
    }
}

}  // namespace alamo::tactical::abilities

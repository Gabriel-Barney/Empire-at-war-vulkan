#include "alamo/tactical/abilities/special_weapons.hpp"

#include <algorithm>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::abilities {

namespace {
float length(Vec3 v) { return dmath::sqrt(dot(v, v)); }
}  // namespace

SpecialWeaponSystem::SpecialWeaponSystem(std::vector<SpecialWeaponDesc> descs, SpecialWeaponConfig config)
    : descs_(std::move(descs)), cfg_(config) {}

uint32_t SpecialWeaponSystem::add_desc(const SpecialWeaponDesc& d) {
    descs_.push_back(d);
    return static_cast<uint32_t>(descs_.size() - 1);
}

int32_t SpecialWeaponSystem::find(ObjectId id) const {
    const auto it = std::lower_bound(packs_.begin(), packs_.end(), id, [](const Pack& p, ObjectId v) { return p.id < v; });
    return (it == packs_.end() || it->id != id) ? -1 : static_cast<int32_t>(it - packs_.begin());
}
SpecialWeaponSystem::Pack* SpecialWeaponSystem::pack(ObjectId id) {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &packs_[static_cast<size_t>(i)];
}
const SpecialWeaponSystem::Pack* SpecialWeaponSystem::pack(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &packs_[static_cast<size_t>(i)];
}

uint32_t SpecialWeaponSystem::recharge_frames(const SpecialWeaponDesc& d) const {
    // int(fps x recharge) (0x4ea3f0 [H]; 0x491770 uses the same getter [M]).
    const float f = static_cast<float>(cfg_.fps) * d.recharge_seconds;
    return f > 0.0f ? static_cast<uint32_t>(f) : 0u;
}

void SpecialWeaponSystem::event(SwEventKind k, const Pack& p, uint32_t frame, int32_t value, ObjectId target, Vec3 point) {
    SpecialWeaponEvent e;
    e.kind = k;
    e.id = p.id;
    e.target = target;
    e.value = value;
    e.frame = frame;
    e.point = point;
    events_.push_back(e);
}

bool SpecialWeaponSystem::add(ObjectId id, uint32_t desc, int32_t owner, uint32_t frame) {
    if (desc >= descs_.size() || find(id) >= 0) return false;
    const SpecialWeaponDesc& d = descs_[desc];
    Pack p;
    p.id = id;
    p.desc = desc;
    p.owner = owner;
    p.start = frame;
    p.ready = cfg_.start_charged ? frame : frame + recharge_frames(d);
    p.ready_signalled = cfg_.start_charged ? 1 : 0;
    if (d.kinds & kSwTsw) p.tsw = kTswIdle;
    const auto it = std::lower_bound(packs_.begin(), packs_.end(), id, [](const Pack& q, ObjectId v) { return q.id < v; });
    packs_.insert(it, p);
    return true;
}

void SpecialWeaponSystem::remove(ObjectId id) {
    const int32_t i = find(id);
    if (i >= 0) packs_.erase(packs_.begin() + i);
}

size_t SpecialWeaponSystem::prune(const std::function<bool(ObjectId id)>& keep) {
    const size_t before = packs_.size();
    std::erase_if(packs_, [&keep](const Pack& p) { return !keep(p.id); });
    return before - packs_.size();
}

bool SpecialWeaponSystem::set_owner(ObjectId id, int32_t owner) {
    Pack* p = pack(id);
    if (!p) return false;
    p->owner = owner;
    return true;
}

uint8_t SpecialWeaponSystem::kinds(ObjectId id) const {
    const Pack* p = pack(id);
    return p ? descs_[p->desc].kinds : uint8_t{0};
}

// ---- pack (SCRIPT_GAPS conventions / §1.6) ----

float SpecialWeaponSystem::readiness_of(const Pack& p, uint32_t frame) const {
    // 0x4914e0 [H]: 1.0 when now >= ready, else clamp((now - start) / (ready - start), 0, 1).
    if (frame >= p.ready) return 1.0f;
    const int64_t span = static_cast<int64_t>(p.ready) - static_cast<int64_t>(p.start);
    if (span <= 0) return 1.0f;
    const float r = static_cast<float>(static_cast<int64_t>(frame) - static_cast<int64_t>(p.start)) / static_cast<float>(span);
    return std::clamp(r, 0.0f, 1.0f);
}

float SpecialWeaponSystem::readiness(ObjectId id, uint32_t frame) const {
    const Pack* p = pack(id);
    if (!p || !(descs_[p->desc].kinds & (kSwSpecialWeapon | kSwLobbing))) return -1.0f;
    return readiness_of(*p, frame);
}

bool SpecialWeaponSystem::targeting(ObjectId id) const {
    const Pack* p = pack(id);
    return p && p->targeting_flag != 0;
}

uint32_t SpecialWeaponSystem::ready_frame(ObjectId id) const {
    const Pack* p = pack(id);
    return p ? p->ready : 0u;
}

bool SpecialWeaponSystem::begin_targeting(ObjectId id, uint32_t frame) {
    Pack* p = pack(id);
    if (!p || !(descs_[p->desc].kinds & kSwSpecialWeapon)) return false;
    if (p->targeting_flag) {  // a second call toggles off and fails [H]
        p->targeting_flag = 0;
        event(SwEventKind::TargetingOff, *p, frame);
        return false;
    }
    if (cfg_.space)  // space: one targeting SPECIAL_WEAPON per owner (the local-player "already targeting" sound)
        for (const Pack& q : packs_)
            if (&q != p && q.owner == p->owner && q.targeting_flag && (descs_[q.desc].kinds & kSwSpecialWeapon)) return false;
    p->targeting_flag = 1;
    event(SwEventKind::TargetingOn, *p, frame);
    return true;
}

void SpecialWeaponSystem::queue_volley(Pack& p, const SpecialWeaponDesc& d, ObjectId target, Vec3 point, bool at_point, uint32_t frame) {
    p.pulses_left = std::max(1, d.pulse_count);
    p.pulse_index = 0;
    p.next_pulse = frame;
    p.shot_target = target;
    p.shot_point = point;
    p.at_point = at_point ? 1 : 0;
}

bool SpecialWeaponSystem::fire_at_object(ObjectId id, ObjectId target, uint32_t frame, const SpecialWeaponWorld& w) {
    // 0x491cd0 [H]: ready, target not flagged, not allied, categories in Special_Weapon_Valid_Targets, not self.
    Pack* p = pack(id);
    if (!p || target == kNoObject || target == id) return false;
    const SpecialWeaponDesc& d = descs_[p->desc];
    if (!(d.kinds & kSwSpecialWeapon) || readiness_of(*p, frame) < 1.0f) return false;
    if (w.target_flagged && w.target_flagged(target)) return false;
    const bool allied = w.allied ? w.allied(p->owner, target) : (w.owner_of && w.owner_of(target) == p->owner);
    if (allied) return false;
    const uint64_t cats = w.category_mask ? w.category_mask(target) : ~uint64_t{0};
    if (!(cats & d.valid_targets)) return false;
    // 0x491770: restart (start = now, ready = now + recharge), targeting off, the weapon event (the volley).
    p->start = frame;
    p->ready = frame + recharge_frames(d);
    p->ready_signalled = 0;
    p->targeting_flag = 0;
    queue_volley(*p, d, target, {}, false, frame);
    event(SwEventKind::Fired, *p, frame, d.target_action_index, target);
    return true;
}

bool SpecialWeaponSystem::fire_lobbing_at(ObjectId id, Vec3 point, uint32_t frame, const SpecialWeaponWorld& w) {
    // 0x4ea3f0 [H]: the owner's tactical data, base power when Requires_Base_Power; no range / ally / fog check.
    Pack* p = pack(id);
    if (!p) return false;
    const SpecialWeaponDesc& d = descs_[p->desc];
    if (!(d.kinds & kSwLobbing)) return false;
    if (p->owner < 0) return false;
    if (d.requires_base_power && w.base_power && !w.base_power(p->owner)) return false;
    p->targeting_flag = 0;
    p->start = frame;
    p->ready = frame + recharge_frames(d);
    p->ready_signalled = 0;
    // S8 t30: the WeaponBehavior charges and fires the order (TACTICAL_COMBAT §3.1.1); [port] the point volley only for an
    // object without one.
    if (!(w.fire_at_position && w.fire_at_position(id, point))) queue_volley(*p, d, kNoObject, point, true, frame);
    event(SwEventKind::Fired, *p, frame, d.target_action_index, kNoObject, point);
    return true;
}

bool SpecialWeaponSystem::restart(ObjectId id, uint32_t frame, float seconds) {
    Pack* p = pack(id);
    if (!p) return false;
    const SpecialWeaponDesc& d = descs_[p->desc];
    p->start = frame;
    const float f = static_cast<float>(cfg_.fps) * seconds;
    p->ready = seconds < 0.0f ? frame + recharge_frames(d) : frame + (f > 0.0f ? static_cast<uint32_t>(f) : 0u);
    p->ready_signalled = p->ready <= frame ? 1 : 0;
    return true;
}

// ---- TSW (§1.2) ----

int32_t SpecialWeaponSystem::tsw_state(ObjectId id) const {
    const Pack* p = pack(id);
    return p ? p->tsw : -1;
}

ObjectId SpecialWeaponSystem::tsw_target(ObjectId id) const {
    const Pack* p = pack(id);
    return p ? p->tsw_target : kNoObject;
}

void SpecialWeaponSystem::set_tsw_state(Pack& p, int32_t s, uint32_t frame) {
    if (p.tsw == s) return;
    p.tsw = s;
    event(SwEventKind::TswState, p, frame, s, p.tsw_target);
}

bool SpecialWeaponSystem::tsw_fire(ObjectId id, bool planet_ok, uint32_t frame) {
    Pack* p = pack(id);
    if (!p || p->tsw != kTswReady || p->tsw_target == kNoObject || !planet_ok) return false;
    const SpecialWeaponDesc& d = descs_[p->desc];
    set_tsw_state(*p, kTswPowerUp, frame);
    const float f = 30.0f * d.tsw_power_up_seconds;  // int(30 x seconds) [H]
    p->countdown = f > 0.0f ? static_cast<int32_t>(f) : 0;
    event(SwEventKind::TswFiring, *p, frame, 0, p->tsw_target);
    return true;
}

void SpecialWeaponSystem::tsw_service(Pack& p, const SpecialWeaponDesc& d, uint32_t frame, SimRng& rng, const SpecialWeaponWorld& w) {
    auto find_target = [&]() {
        ObjectId t = kNoObject;
        Vec3 tp;
        if (w.find_tsw_target && w.find_tsw_target(p.id, t, tp) && t != kNoObject) {
            p.tsw_target = t;
            p.tsw_target_pos = tp;
            return true;
        }
        return false;
    };
    // Placement toward the target at distance `dist` (z per TSW_Start_Pos_Match_Targets_Z), facing it.
    auto place_at = [&](float dist) {
        const Vec3 cur = w.position ? w.position(p.id) : Vec3{};
        Vec3 dir = cur - p.tsw_target_pos;
        dir.z = 0.0f;
        float len = length(dir);
        if (!(len > 0.0f)) {
            dir = Vec3{-1.0f, 0.0f, 0.0f};  // [port] coincident: approach from -x
            len = 1.0f;
        }
        dir = dir * (1.0f / len);
        Vec3 pos = p.tsw_target_pos + dir * dist;
        pos.z = d.tsw_match_target_z ? p.tsw_target_pos.z + d.tsw_z_adjust : cur.z;
        const Vec3 facing{-dir.x, -dir.y, 0.0f};
        if (w.place) w.place(p.id, pos, facing);
        return pos;
    };
    if (p.tsw_init == 2) return;  // spent: the dummy is dead, it never re-arms (§3)
    if (!p.tsw_init) {
        // First service (§1.2): with a target dummy, placed toward it; the battle attacker starts ready.
        if (!find_target()) {
            set_tsw_state(p, kTswSearch, frame);
            return;  // retried next service (initialised once a target exists [port])
        }
        p.tsw_init = 1;
        const bool attacker = w.is_battle_attacker && w.is_battle_attacker(p.owner);
        const Vec3 at = place_at(attacker ? d.tsw_attack_distance : d.tsw_start_distance);
        event(SwEventKind::TswPlaced, p, frame, attacker ? 1 : 0, p.tsw_target, at);
        set_tsw_state(p, attacker ? kTswReady : kTswAcquired, frame);
        if (attacker) event(SwEventKind::TswReady, p, frame, 0, p.tsw_target);
        return;
    }
    switch (p.tsw) {
        case kTswIdle:
        case kTswSearch:
            if (find_target()) set_tsw_state(p, kTswAcquired, frame);
            break;
        case kTswAcquired: set_tsw_state(p, kTswArming, frame); break;
        case kTswArming: {
            // [port] approach at Max_Speed per frame along the line to the target until within the attack distance.
            const Vec3 cur = w.position ? w.position(p.id) : Vec3{};
            Vec3 dv = cur - p.tsw_target_pos;
            dv.z = 0.0f;
            const float dist = length(dv);
            if (!(d.max_speed > 0.0f) || dist - d.max_speed <= d.tsw_attack_distance) {
                place_at(d.tsw_attack_distance);
                set_tsw_state(p, kTswReady, frame);
                event(SwEventKind::TswReady, p, frame, 0, p.tsw_target);
            } else {
                place_at(dist - d.max_speed);
            }
            break;
        }
        case kTswReady: break;  // waits for tsw_fire
        case kTswPowerUp:
            if (--p.countdown <= 0) set_tsw_state(p, kTswFire, frame);
            break;
        case kTswFire: set_tsw_state(p, kTswAttackAnim, frame); break;  // state 6: one service (TSW_PLANET_DESTROY §3)
        case kTswAttackAnim: {  // state 7: attack animation (anim 5) + SFXEvent_Fire, countdown, one service
            event(SwEventKind::TswAttackAnim, p, frame, 5, p.tsw_target);
            const float f = 30.0f * d.tsw_attack_anim_seconds;
            p.countdown = f > 0.0f ? static_cast<int32_t>(f) : 0;
            set_tsw_state(p, kTswAttackWait, frame);
            break;
        }
        case kTswAttackWait:
            if (--p.countdown <= 0) set_tsw_state(p, kTswDestruction, frame);
            break;
        case kTswDestruction: {
            // TSW_PLANET_DESTROY §4: state := 10 first, countdown := the debris delay; then the Death_Explosions pick
            // (synced 0x1ffb40; no draw for one entry) and the destruction (the host: explosion, dummy kill, galaxy).
            set_tsw_state(p, kTswDebrisWait, frame);
            p.countdown = d.tsw_debris_delay_frames;
            const uint32_t n = w.debris_choices ? w.debris_choices(p.tsw_target) : 0u;
            p.debris_pick = n > 1 ? rng.range_int(0, static_cast<int32_t>(n) - 1) : (n == 1 ? 0 : -1);
            p.debris_pending = 1;
            p.debris_frame = frame;
            event(SwEventKind::TswPlanetDestroyed, p, frame, p.debris_pick, p.tsw_target, p.tsw_target_pos);
            break;
        }
        case kTswDebrisWait:  // §3: countdown; at 0 every Death_By_TSW_Replacements entry, then the post-destruction wait
            if (--p.countdown <= 0) {
                p.debris_pending = 0;
                event(SwEventKind::TswDebris, p, frame, p.debris_pick, p.tsw_target, p.tsw_target_pos);
                p.countdown = d.tsw_post_destruction_frames;
                set_tsw_state(p, kTswPostWait, frame);
            }
            break;
        case kTswPostWait:
            if (--p.countdown <= 0) set_tsw_state(p, kTswRelease, frame);
            break;
        case kTswRelease:  // 0x42a7b0 state 13: state 0, target cleared, screen effect off, hold released with replay
            p.tsw_init = 2;
            set_tsw_state(p, kTswIdle, frame);
            event(SwEventKind::TswDone, p, frame, 0, p.tsw_target);
            p.tsw_target = kNoObject;
            break;
        default: break;
    }
}

uint32_t SpecialWeaponSystem::victory_holds() const {
    uint32_t n = 0;
    for (const Pack& p : packs_) n += tsw_holds_victory(p.tsw) ? 1u : 0u;
    return n;
}

bool SpecialWeaponSystem::tsw_spent(ObjectId id) const {
    const Pack* p = pack(id);
    return p && p->tsw_init == 2;
}

// ---- tick ----

void SpecialWeaponSystem::tick(uint32_t frame, SimRng& rng, const SpecialWeaponWorld& w) {
    clear_outputs();
    for (size_t i = 0; i < packs_.size(); ++i) {
        Pack& p = packs_[i];
        const SpecialWeaponDesc& d = descs_[p.desc];
        // Volley pulses due this frame (Projectile_Fire_Pulse_Delay_Seconds apart [M]).
        while (p.pulses_left > 0 && frame >= p.next_pulse) {
            SpecialWeaponShot s;
            s.weapon = p.id;
            s.owner = p.owner;
            s.projectile = d.projectile;
            s.target = p.shot_target;
            s.point = p.shot_point;
            s.at_point = p.at_point != 0;
            s.pulse = p.pulse_index++;
            shots_.push_back(s);
            --p.pulses_left;
            const float f = static_cast<float>(cfg_.fps) * d.pulse_delay_seconds;
            const uint32_t gap = f > 0.0f ? static_cast<uint32_t>(f) : 0u;
            p.next_pulse = p.next_pulse + gap;  // no delay: the whole volley this frame
        }
        if ((d.kinds & (kSwSpecialWeapon | kSwLobbing)) && !p.ready_signalled && frame >= p.ready) {
            p.ready_signalled = 1;
            event(SwEventKind::Ready, p, frame);
        }
        if (p.tsw >= 0) tsw_service(p, d, frame, rng, w);
    }
}

void SpecialWeaponSystem::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(packs_.size()));
    for (const Pack& p : packs_) {
        h.i32(p.id);
        h.u32(p.desc);
        h.i32(p.owner);
        h.u32(p.start);
        h.u32(p.ready);
        h.u8(p.targeting_flag);
        h.u8(p.ready_signalled);
        h.i32(p.pulses_left);
        h.u32(p.pulse_index);
        h.u32(p.next_pulse);
        h.i32(p.shot_target);
        h.f32(p.shot_point.x);
        h.f32(p.shot_point.y);
        h.f32(p.shot_point.z);
        h.u8(p.at_point);
        h.i32(p.tsw);
        h.u8(p.tsw_init);
        h.i32(p.tsw_target);
        h.f32(p.tsw_target_pos.x);
        h.f32(p.tsw_target_pos.y);
        h.f32(p.tsw_target_pos.z);
        h.i32(p.countdown);
        h.i32(p.debris_pick);
        h.u32(p.debris_frame);
        h.u8(p.debris_pending);
    }
}

}  // namespace alamo::tactical::abilities

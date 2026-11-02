#include "alamo/tactical/abilities/ability_system.hpp"

#include <algorithm>

#include "alamo/core/dmath.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::abilities {

using combat::CombatSystem;
using combat::HardpointKind;
using combat::ModKind;
using combat::ModList;
using combat::ProjectileCategory;

namespace {

constexpr float kDegToRad = 0.017453292519943295f;
constexpr int32_t kAnimRevealBegin = 0x69, kAnimRevealLoop = 0x6a, kAnimRevealEnd = 0x6b;  // Force_Sight (§4.14)

bool forwards_to_special(uint32_t t) {
    // 2.5: re-check the gates, then forward to the special ability named by the record.
    switch (t) {
        case kForceWhirlwind:  // OI abilities: 0x3a1fb0 gates, then the special named by +0x88 (§2.1)
        case kStun:
        case kForceSight:
        case kLaserDefense:
        case kForceConfuse:
        case kClusterBomb:
        case kSensorJamming:
        case kDrainLife:
        case kBlast:
        case kShieldFlare: return true;
        default: return false;
    }
}

bool byte_only(uint32_t t) {
    // 2.5 "none": the work is done by the forwarded special ability; pure toggles of other modules.
    switch (t) {
        case kStickyBomb:
        case kTractorBeam:
        case kEnergyWeapon:
        case kLuckyShot:
        case kConcentrateFire:
        case kIonCannonShot:
        case kMaximumFirepower:
        case kSuperLaser: return true;
        default: return false;
    }
}

}  // namespace

// ================================================================================================
// construction / objects
// ================================================================================================

AbilitySystem::AbilitySystem(CombatSystem& combat, const AbilityData& data, AbilityConfig config)
    : combat_(&combat), data_(&data), cfg_(config) {
    if (cfg_.fps == 0) cfg_.fps = combat::kDefaultFps;
    combat_->set_hooks(this);
    stealth_bonus_.assign(combat_->player_count(), 0.0f);
}

AbilitySystem::~AbilitySystem() { combat_->set_hooks(nullptr); }

void AbilitySystem::reserve(uint32_t objects, uint32_t events) {
    objs_.reserve(objects);
    events_.reserve(events);
}

int32_t AbilitySystem::find(ObjectId id) const {
    const auto it = std::lower_bound(objs_.begin(), objs_.end(), id, [](const Obj& o, ObjectId v) { return o.id < v; });
    if (it == objs_.end() || it->id != id) return -1;
    return static_cast<int32_t>(it - objs_.begin());
}
AbilitySystem::Obj* AbilitySystem::obj(ObjectId id) {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &objs_[static_cast<size_t>(i)];
}
const AbilitySystem::Obj* AbilitySystem::obj(ObjectId id) const {
    const int32_t i = find(id);
    return i < 0 ? nullptr : &objs_[static_cast<size_t>(i)];
}

bool AbilitySystem::add_unit(const combat::UnitSpawn& s, uint32_t ability_desc, uint32_t frame, SimRng& rng,
                             ObjectId container) {
    if (ability_desc >= data_->objects.size()) return false;
    if (!objs_.empty() && s.id <= objs_.back().id) return false;
    const AbilityObjectDesc& d = data_->objects[ability_desc];
    Obj o;
    o.id = s.id;
    o.desc = ability_desc;
    o.owner = s.owner;
    o.container = container;
    o.combat_unit = 1;
    // 1. unit-ability slots, XML order; a later entry of the same Type replaces an earlier one.
    o.record.fill(-1);
    for (size_t i = 0; i < d.unit_abilities.size(); ++i) {
        const uint8_t t = d.unit_abilities[i].type;
        if (t >= 1 && t <= kMaxAbilityType) o.record[t] = static_cast<int16_t>(i);
    }
    o.cd.last_update = frame;
    // 2. hardpoints (combat unit): one synced Get_Int per hardpoint, HardPoints order.
    if (!combat_->add_unit(s, frame, rng)) return false;
    // 3. special-ability pack
    o.specials.reserve(d.specials.size());
    for (uint32_t si : d.specials) {
        SpecialState sp;
        sp.desc = si;
        o.specials.push_back(sp);
    }
    objs_.push_back(std::move(o));
    Obj& n = objs_.back();
    if (container != kNoObject) {
        if (Obj* c = obj(container)) {
            if (std::find(c->members.begin(), c->members.end(), n.id) == c->members.end()) c->members.push_back(n.id);
        }
    }
    // OI abilities: SAP init (first-due stagger draws, after the hardpoint draws [M]), automatic / event-driven
    // classes (auras give to the new unit), Active_By_Default (0x3a82a0, first two entries, §1.2 [H]).
    sap_init(n, frame, &rng);
    update_emitter(n);
    const ObjectId nid = n.id;
    if (Obj* p = obj(nid)) auras_unit_added(*p, frame);
    if (Obj* p = obj(nid)) apply_automatic(*p, frame);
    if (Obj* p = obj(nid)) active_by_default(*p, frame);
    if (Obj* p = obj(nid)) sync_combat(*p);
    return true;
}

bool AbilitySystem::add_container(ObjectId id, uint32_t ability_desc, int32_t owner, std::span<const ObjectId> members,
                                  uint32_t frame, SimRng* rng) {
    if (ability_desc >= data_->objects.size()) return false;
    if (!objs_.empty() && id <= objs_.back().id) return false;
    const AbilityObjectDesc& d = data_->objects[ability_desc];
    Obj o;
    o.id = id;
    o.desc = ability_desc;
    o.owner = owner;
    o.is_container = 1;
    o.record.fill(-1);
    for (size_t i = 0; i < d.unit_abilities.size(); ++i) {
        const uint8_t t = d.unit_abilities[i].type;
        if (t >= 1 && t <= kMaxAbilityType) o.record[t] = static_cast<int16_t>(i);
    }
    o.cd.last_update = frame;
    o.members.assign(members.begin(), members.end());
    o.members.reserve(members.size() + 4);
    for (uint32_t si : d.specials) {
        SpecialState sp;
        sp.desc = si;
        o.specials.push_back(sp);
    }
    objs_.push_back(std::move(o));
    Obj& c = objs_.back();
    for (ObjectId m : c.members)
        if (Obj* mo = obj(m)) mo->container = id;
    sap_init(c, frame, rng);  // OI abilities
    apply_automatic(c, frame);
    if (Obj* p = obj(id)) active_by_default(*p, frame);  // team / company spawn 0x2a5a60 (§1.2)
    return true;
}

bool AbilitySystem::set_owner(ObjectId id, int32_t owner) {
    // S10-int: Change_Owner.
    Obj* o = obj(id);
    if (!o || o->dead) return false;
    o->owner = owner;
    if (o->combat_unit) combat_->set_owner(id, owner);
    return true;
}

void AbilitySystem::remove(ObjectId id) {
    const int32_t i = find(id);
    if (i < 0) return;
    Obj& o = objs_[static_cast<size_t>(i)];
    for (SpecialState& sp : o.specials)
        if (sp.applied) remove_special(o, sp, 0);
    const bool unit = o.combat_unit != 0;
    objs_.erase(objs_.begin() + i);
    for (Obj& c : objs_) {
        auto it = std::find(c.members.begin(), c.members.end(), id);
        if (it != c.members.end()) c.members.erase(it);
    }
    if (unit) combat_->remove_unit(id);
}

// ================================================================================================
// state helpers
// ================================================================================================

const UnitAbilityRecord* AbilitySystem::own_record(const Obj& o, uint32_t t) const {
    if (t >= kAbilitySlots) return nullptr;
    const int16_t r = o.record[t];
    return r < 0 ? nullptr : &desc(o).unit_abilities[static_cast<size_t>(r)];
}

const UnitAbilityRecord* AbilitySystem::record_of(const Obj& o, uint32_t t) const {
    // 0x397d90: own slot when U = 1 or not a team container; otherwise the member's record.
    if (info(t).per_unit || !o.is_container) return own_record(o, t);
    for (ObjectId m : o.members)
        if (const Obj* mo = obj(m))
            if (!mo->dead)
                if (const UnitAbilityRecord* r = own_record(*mo, t)) return r;
    return own_record(o, t);
}

bool AbilitySystem::has(const Obj& o, uint32_t t) const {
    if (t < 1 || t > kMaxAbilityType) return false;
    if (o.is_container && !info(t).per_unit) {  // 0x3986b0: any member has it
        for (ObjectId m : o.members)
            if (const Obj* mo = obj(m))
                if (!mo->dead && has(*mo, t)) return true;
        return false;
    }
    return o.record[t] >= 0;
}

bool AbilitySystem::active_state(const Obj& o, uint32_t t) const {
    // Is_Unit_Ability_Active (0x39b950) [H]
    if (t < 1 || t > kMaxAbilityType) return false;
    const AbilityTypeInfo& in = info(t);
    if (o.is_container && !in.per_unit) {
        bool any = false;
        for (ObjectId m : o.members) {
            const Obj* mo = obj(m);
            if (!mo || mo->dead || !has(*mo, t)) continue;
            any = true;
            if (!active_state(*mo, t)) return false;
        }
        return any;
    }
    if (!has(o, t) || !record_of(o, t)) return false;
    if (in.behavior != kBehaviorNone && in.mode == 0) return o.behavior[t] != 0;  // DEPLOY_TROOPERS (HUNT/LURE: M = 1)
    if (t == kDeploy) return o.deployed != 0;                     // OI abilities: the deployed state
    if (t == kBarrage) return o.active[t] != 0 || o.barrage != 0;  // ... or a barrage in progress
    return o.active[t] != 0;  // JET_PACK's locomotor jump query is not modelled (slot byte, never written: M = 0)
}

bool AbilitySystem::in_nebula_lockout(const Obj& o, uint32_t frame) const {
    if (o.nebula_frame == kNever) return false;
    return static_cast<float>(frame - o.nebula_frame) / static_cast<float>(cfg_.fps) < cfg_.nebula_ability_disable_time;
}

bool AbilitySystem::alive_unit(ObjectId id) const { return combat_->alive(id); }

Vec3 AbilitySystem::position(const Obj& o) const {
    if (o.combat_unit) return combat_->position(o.id);
    for (ObjectId m : o.members)  // container: its first live member [port]
        if (alive_unit(m)) return combat_->position(m);
    return Vec3{};
}

bool AbilitySystem::can(const Obj& o, uint32_t t, bool check_recharge, bool check_nebula, uint32_t frame) const {
    // Can_Activate_Unit_Ability (0x39b480), gates in order [H].
    if (t < 1 || t > kMaxAbilityType) return false;                                   // 1
    if (o.dead) return false;
    if (check_nebula && in_nebula_lockout(o, frame)) return false;                    // 2
    const AbilityTypeInfo& in = info(t);
    if (in.force && o.force_frame != kNever) {                                        // 3
        float elapsed = static_cast<float>(frame - o.force_frame) / static_cast<float>(cfg_.fps);
        if (elapsed < 0.1f) elapsed = 0.0f;
        if (elapsed <= cfg_.force_ability_disable_time) return false;
    }
    if (desc(o).garrison_style && o.garrison_count == 0) return false;               // 4
    if (o.sap_disabled) return false;                                                 // 5
    if (o.corrupted) return false;                                                    // 6
    if (check_recharge && desc(o).countdown && o.cd.blocks_activation(t)) return false;  // 7
    if (o.is_container) {                                                             // 8
        for (ObjectId m : o.members) {
            const Obj* mo = obj(m);
            if (!mo || mo->dead) continue;
            if (!can(*mo, t, true, check_nebula, frame)) return false;
        }
    }
    if (o.locomotor_busy) return false;                                               // 9
    // 10 per-type checks (combat-side ones only for combat units)
    const ObjectId id = o.id;
    switch (t) {
        case kDefend:
            if (!o.combat_unit) break;
            if (combat_->ion_stunned(id, frame)) return false;
            if (!(combat_->data().units[combat_->desc_index(id)].shield_points > 0.0f)) return false;
            if (o.ion_storm_frame != kNever &&
                static_cast<float>(frame - o.ion_storm_frame) / static_cast<float>(cfg_.fps) < cfg_.ion_storm_shield_disable_time)
                return false;
            if (combat_->shield_lockout(id, frame)) return false;
            if (combat_->shield_regen_blocked(id)) return false;  // 0x549460 reads +0x131 (HARDPOINT_DISABLED §9.2)
            break;
        case kInterdict:
            if (o.gravity_well_destroyed) return false;
            break;
        case kHunt:
            if (locked(o)) return false;  // movement locked (0x39ad40, A1_READINGS §28.9)
            break;
        case kCaptureVehicle:
        case kTargetedInvulnerability:
        case kAreaEffectStun:
        case kAreaEffectHeal:
        case kAreaEffectConvert:
        case kCorruptSystems:
            if (desc(o).garrison_style) return false;
            break;
        case kTractorBeam:
            if (o.combat_unit && combat_->live_hardpoints(id, HardpointKind::TractorBeam) == 0) return false;
            if (o.weapons_disabled) return false;
            break;
        case kEnergyWeapon:
        case kLuckyShot:
        case kSuperLaser:
            if (o.weapons_disabled) return false;
            break;
        case kFireLobbingSuperweapon:
            return false;  // player super-weapon state not modelled [port]
        case kForceSight:  // case 0x34 (§4.14; the §2.4 row read "SABER_THROW"): no ability of the owner busy (0x39a350)
            if (anim_busy(o, frame)) return false;
            break;
        case kLeechShields:
        case kBlast:
            if (o.combat_unit && combat_->live_hardpoints(id, HardpointKind::WeaponSpecial) == 0) return false;
            break;
        case kShieldFlare:
            if (o.combat_unit && !(combat_->shield_fraction(id) > 0.0f)) return false;
            break;
        case kDeploySquad:  // the SPAWN_SQUADRON state is 4 (host query, STATION_HANGARS_SOUND §2.5) and >= 1 live bay
            if (o.combat_unit && combat_->live_hardpoints(id, HardpointKind::FighterBay) == 0) return false;
            if (host_.squadron_deploy_ready && !host_.squadron_deploy_ready(id)) return false;
            break;
        default: break;
    }
    return true;
}

float AbilitySystem::own_recharge_bonus(const Obj& o) const {
    // list 9 of the object's own ModifierSet only (owner player's set not merged), with its clamps.
    if (!o.combat_unit) return 0.0f;
    const combat::ModifierSet* set = combat_->unit_modifiers(o.id);
    return set ? combat::reduce_list(nullptr, set, ModList::AbilityRecharge) : 0.0f;
}

void AbilitySystem::start_active(Obj& o, uint32_t t) {
    const UnitAbilityRecord* r = record_of(o, t);
    if (r && desc(o).countdown) o.cd.start_active(t, r->expiration_seconds, cfg_.fps);
}

void AbilitySystem::start_recharge(Obj& o, uint32_t t) {
    const UnitAbilityRecord* r = record_of(o, t);
    if (r && desc(o).countdown) o.cd.start_recharge(t, r->recharge_seconds, own_recharge_bonus(o), cfg_.fps);
}

void AbilitySystem::deactivate_early(Obj& o, uint32_t t, uint32_t frame) {
    const UnitAbilityRecord* r = record_of(o, t);
    if (!r || !desc(o).countdown) return;
    if (o.cd.deactivate_early(t, r->recharge_seconds, own_recharge_bonus(o), cfg_.fps))
        event(AbilityEventKind::Ready, static_cast<uint8_t>(t), o.id, kNoObject, frame);
}

void AbilitySystem::event(AbilityEventKind k, uint8_t type, ObjectId id, ObjectId target, uint32_t frame) {
    AbilityEvent e;
    e.kind = k;
    e.type = type;
    e.id = id;
    e.target = target;
    e.frame = frame;
    events_.push_back(e);
}

void AbilitySystem::sync_combat(Obj& o) {
    // Mod_Multiplier (system B) reads the first two Unit_Abilities_Data entries where Is_Active.
    if (!o.combat_unit || o.dead) return;
    const combat::UnitDesc& ud = combat_->data().units[combat_->desc_index(o.id)];
    for (int s = 0; s < 2; ++s) {
        const int32_t a = ud.abilities[static_cast<size_t>(s)];
        if (a < 0 || static_cast<size_t>(a) >= combat_->data().abilities.size()) continue;
        const bool on = active_state(o, combat_->data().abilities[static_cast<size_t>(a)].ability_type);
        if (combat_->ability_active(o.id, s) != on) combat_->set_ability_active(o.id, s, on);
    }
}

void AbilitySystem::update_emitter(Obj& o) {
    // 4.3: radius = passive, overridden by unit ability 32's Effective_Radius, overridden by 63's
    // while jamming; an emitter while 32 is active, the passive radius is > 0 or jamming runs.
    if (!o.combat_unit || o.dead) return;
    const float passive = combat_->data().units[combat_->desc_index(o.id)].passive_missile_shield_radius;
    float r = passive;
    if (const UnitAbilityRecord* r32 = own_record(o, kMissileShield))
        if (r32->effective_radius > 0.0f) r = r32->effective_radius;
    bool jamming = false;
    for (const SpecialState& sp : o.specials)
        if (sp.applied && data_->specials[sp.desc].cls == SpecialClass::SensorJamming) jamming = true;
    if (jamming)
        if (const UnitAbilityRecord* r63 = own_record(o, kSensorJamming))
            if (r63->effective_radius > 0.0f) r = r63->effective_radius;
    const bool emitter = active_state(o, kMissileShield) || passive > 0.0f || jamming;
    combat_->set_emitter_radius(o.id, emitter ? r : 0.0f);
}

void AbilitySystem::immobilize(Obj& o, int32_t delta, uint32_t frame) {
    // 0x3a8e50 (A1_READINGS §28.8): +1 / -1 clamped at 0.
    const int32_t before = o.immobilize;
    o.immobilize = std::max<int32_t>(0, before + delta);
    if (before == 0 && o.immobilize > 0) event(AbilityEventKind::Immobilised, 0, o.id, kNoObject, frame);
    if (before > 0 && o.immobilize == 0) event(AbilityEventKind::Released, 0, o.id, kNoObject, frame);
}

// ================================================================================================
// Set_Unit_Ability_Active (0x38a350) and the per-type handlers (2.5)
// ================================================================================================

bool AbilitySystem::set_active(Obj& o, uint32_t t, bool on, uint32_t frame, ObjectId target) {
    if (o.dead || !has(o, t)) return false;
    const AbilityTypeInfo& in = info(t);
    if (on) {
        if (!can(o, t, false, true, frame)) return false;
        if (active_state(o, t) && t != kBarrage) return false;
    }
    // REMAINING_GAPS §5.3 [M]: JET_PACK (on) runs on the team itself with its target point - 0x3a38d0 reads the member's
    // record and moves the whole team; forwarding it to the members without a target would move nobody.
    if (o.is_container && !in.per_unit && !(t == kJetPack && on)) {
        // forwarded to every member that has it (target none); the container itself only goes on
        // for SPREAD_OUT.
        for (ObjectId m : o.members) {
            Obj* mo = obj(m);
            if (mo && !mo->dead && has(*mo, t)) set_active(*mo, t, on, frame, kNoObject);
        }
        if (t != kSpreadOut) return true;
    }
    handler(o, t, on, frame, target);
    if (in.mode == 1) o.active[t] = on ? 1 : 0;
    update_emitter(o);
    sync_combat(o);
    event(on ? AbilityEventKind::Activated : AbilityEventKind::Deactivated, static_cast<uint8_t>(t), o.id, target, frame);
    return true;
}

void AbilitySystem::handler(Obj& o, uint32_t t, bool on, uint32_t frame, ObjectId target) {
    const UnitAbilityRecord* rec = record_of(o, t);
    switch (t) {
        case kDefend:
        case kTurbo:
        case kPowerToWeapons:
        case kSpoilerLock:
        case kLeechShields:  // 0x3a4250
            if (t == kDefend && o.combat_unit) combat_->force_regen_service(o.id);
            if (on) {
                start_active(o, t);
            } else if (!o.cd.recharging(t)) {
                deactivate_early(o, t, frame);
            }
            return;
        case kInterdict:
            interdict(o, on, frame);
            return;
        case kHunt:
        case kLure:
        case kAvoidDanger:
        case kDeployTroopers:
        case kSelfDestruct:  // 0x3a4070: the implementing behaviour on / off (OI abilities: HUNT, SELF_DESTRUCT, ...)
            if (on && locked(o)) return;  // switching on is refused while movement is locked (A1_READINGS §28.9)
            h_behavior(o, t, on, frame);
            return;
        case kMissileShield:
            if (on) {
                stop_order(o.id, frame);  // 0x4261f0 always, then the lock with Disable_Movement (A1_READINGS §28.9)
                if (rec && rec->disable_movement) immobilize(o, +1, frame);
                start_active(o, t);
            } else {
                if (rec && rec->disable_movement) immobilize(o, -1, frame);
                start_recharge(o, t);
            }
            return;
        case kInvulnerability:  // countdown on / off only; effect = TAKE_DAMAGE_MULTIPLIER 0
            if (on) {
                start_active(o, t);
            } else {
                deactivate_early(o, t, frame);
            }
            return;
        case kSprint:
        case kStimPack:
            if (on && t == kStimPack && rec && o.combat_unit && rec->damage_percent_when_activated > 0.0f) {
                combat::DamageRequest r;
                r.victim = o.id;
                r.kind = combat::kKindNormal;
                r.amount = rec->damage_percent_when_activated * combat_->max_health(o.id);
                combat_->take_damage(r, frame);
                event(AbilityEventKind::SelfDamage, static_cast<uint8_t>(t), o.id, kNoObject, frame);
            }
            if (on) {
                start_active(o, t);
            } else {
                deactivate_early(o, t, frame);
            }
            return;
        case kStealth:
        case kForceCloak:
            if (on) {
                start_active(o, t);
                if (rec && can(o, t, false, true, frame)) activate_by_name(o, rec->gui_name_crc, o.id, -1, frame, static_cast<uint8_t>(t));
            } else {
                start_recharge(o, t);
                if (o.stealthed) break_stealth(o, frame);
            }
            return;
        case kFullSalvo:
            if (on) {
                start_active(o, t);
            } else {
                start_recharge(o, t);
            }
            return;
        case kFowRevealPing:  // fog follow-up (FOG_OF_WAR.md §3.3): the host spawns Spawned_Object_Type [M]
        case kTargetedInvulnerability:  // OI abilities: the instant-effect handler 0x3a3e20 (§2.1)
        case kAreaEffectStun:
        case kAreaEffectHeal:
        case kAreaEffectConvert:
        case kDistract:
        case kHarmonicBomb:
        case kUntargetedStickyBomb:
        case kWeakenEnemy:
            if (on) h_instant(o, t, target, frame);
            return;
        case kSpreadOut: h_spread_out(o, on, frame); return;   // OI abilities: 0x3a3500
        case kDeploy: h_deploy(o, on, frame); return;          // 0x3a1ce0
        case kBarrage: h_barrage(o, on, target, frame); return;  // 0x3a1930
        case kJetPack:                                          // 0x3a38d0
            if (on) h_jet_pack(o, target, frame);
            return;
        case kEjectVehicleThief:  // REMAINING_GAPS §5.1 (inline in 0x38a350)
            if (on) {
                start_recharge(o, t);  // with the countdown behaviour; the slot byte is written by set_active (M = 1)
                return;
            }
            h_eject_thief(o, frame);
            return;
        case kCaptureVehicle:  // §5.1: 18 off shares the eject code; 18 on has no handler here (generic below)
            if (!on) {
                h_eject_thief(o, frame);
                return;
            }
            break;
        case kRocketAttack:  // 0x3a3260: slot 11's active byte (M = 1) and the countdown [M: the rocket reader]
            if (on) {
                start_active(o, t);
            } else {
                deactivate_early(o, t, frame);
            }
            return;
        case kReplenishWingmen:  // 0x3a2970
            if (on) h_replenish(o, frame);
            return;
        case kDetonateRemoteBomb:  // 0x280ec0: the player's remote bombs detonate (serial order [port] for list order)
            if (on) {
                // LOB_SOLVER §3.5 (S8 t06): every live bomb of the player whose type has Friendly_Damage_Amount F > 0. One
                // stuck to a garrisoned parent is skipped (stays armed); one stuck to an own unit first damages it (kind 1)
                // and the other members of its team container (kind 0x15) by F; then the detonation at the bomb.
                const auto find_bomb = [&](uint32_t serial) -> size_t {
                    for (size_t i = 0; i < combat_->projectile_count(); ++i)
                        if (combat_->projectile(i).serial == serial && combat_->projectile(i).alive) return i;
                    return combat_->projectile_count();
                };
                std::vector<RemoteBomb> keep;
                for (const RemoteBomb& b : remote_bombs_) {
                    if (b.player != o.owner) {
                        keep.push_back(b);
                        continue;
                    }
                    size_t i = find_bomb(b.serial);
                    if (i >= combat_->projectile_count()) continue;  // gone: dropped from the list
                    const combat::ProjectileView pv = combat_->projectile(i);
                    const float F = combat_->data().projectiles[pv.desc].friendly_damage_amount;
                    if (!(F > 0.0f)) {  // not a remote bomb type: never set off by this ability
                        keep.push_back(b);
                        continue;
                    }
                    const ObjectId parent = pv.stuck ? pv.stuck_to : kNoObject;
                    if (parent != kNoObject && combat_->has_unit(parent)) {
                        if (combat_->flags(parent) & combat::kUnitGarrisoned) {
                            keep.push_back(b);
                            continue;
                        }
                        if (alive_unit(parent) && combat_->owner(parent) == o.owner) {
                            auto hurt = [&](ObjectId v, uint8_t kind) {
                                combat::DamageRequest r;
                                r.victim = v;
                                r.amount = F;
                                r.kind = kind;
                                r.source = pv.shooter;
                                r.source_is_projectile = true;
                                r.projectile_desc = static_cast<int32_t>(pv.desc);
                                combat_->take_damage(r, frame);
                            };
                            hurt(parent, 1);
                            const Obj* po = obj(parent);
                            if (const Obj* team = po ? obj(po->container) : nullptr) {
                                const std::vector<ObjectId> members = team->members;  // copy: damage can change the list
                                for (const ObjectId m : members)
                                    if (m != parent && alive_unit(m)) hurt(m, 0x15);
                            }
                        }
                    }
                    i = find_bomb(b.serial);  // the damage may have spawned projectiles (death projectiles)
                    if (i < combat_->projectile_count()) combat_->detonate_projectile(i, frame);
                }
                remote_bombs_ = std::move(keep);
                start_recharge(o, t);
            }
            return;
        case kSwapWeapons: {
            // ABILITIES_HARDPOINTS 2.5 (inline): flips the weapon index obj +0x324 when the type has more than one weapon
            // set (Projectile_Types entries, combat UnitDesc::weapon_projectiles), on both on and off. [port] flip =
            // 0 <-> 1 (index != 0 -> 0); a team flips each combat member (the container has no weapon).
            bool any = false;
            auto flip = [&](ObjectId id) {
                if (!combat_->has_unit(id) || combat_->weapon_set_count(id) < 2) return;
                combat_->set_weapon_index(id, combat_->weapon_index(id) == 0 ? 1 : 0);
                any = true;
            };
            if (o.combat_unit) flip(o.id);
            else
                for (ObjectId m : o.members) flip(m);
            if (any) event(AbilityEventKind::EffectOn, static_cast<uint8_t>(t), o.id, target, frame);
            return;
        }
        case kDeploySquad:  // inline in 0x38a350 (ABILITIES_HARDPOINTS §2.5): reset SPAWN_SQUADRON, next-ready, recharge
            if (on && host_.squadron_deploy) {
                host_.squadron_deploy(o.id, frame, rec ? rec->recharge_seconds : 0.0f);
                start_recharge(o, t);
                return;
            }
            break;  // unset: the generic path below
        default: break;
    }
    if (byte_only(t)) return;
    if (forwards_to_special(t)) {
        if (on) {
            const bool ok = rec && can(o, t, false, true, frame) &&
                            activate_by_name(o, rec->gui_name_crc, target != kNoObject ? target : o.id, -1, frame,
                                             static_cast<uint8_t>(t));
            if (!ok) event(AbilityEventKind::Unhandled, static_cast<uint8_t>(t), o.id, target, frame);
        } else if (t == kSensorJamming) {
            start_recharge(o, t);
            for (SpecialState& sp : o.specials)
                if (sp.applied && data_->specials[sp.desc].cls == SpecialClass::SensorJamming) remove_special(o, sp, frame);
        }
        return;
    }
    // Remaining types (land instant effects 0x3a3e20, jet pack, rockets, ...): no behaviour here.
    // [port] the countdown still runs so the button recharges: active phase if any, else recharge.
    event(AbilityEventKind::Unhandled, static_cast<uint8_t>(t), o.id, target, frame);
    if (on && rec) {
        if (rec->expiration_seconds > 0.0f) {
            start_active(o, t);
        } else {
            start_recharge(o, t);
        }
    }
}

void AbilitySystem::interdict(Obj& o, bool on, uint32_t frame) {
    // 0x3a2240 [M]
    const UnitAbilityRecord* rec = record_of(o, kInterdict);
    if (on) {
        if (o.gravity_well_destroyed) return;
        start_active(o, kInterdict);
        if (rec && rec->disable_movement) {  // stop (0x4261f0), then lock: the lock alone freezes nothing (§28.9)
            stop_order(o.id, frame);
            immobilize(o, +1, frame);
        }
        event(AbilityEventKind::InterdictOn, kInterdict, o.id, kNoObject, frame);
        if (cfg_.mode == GameMode::Space && !o.interdicting) {
            // OI abilities (TACTICAL_GAPS §1.5 [H]; relation test 0x2824d0 [M: enemies]): a permanent entry keyed by
            // (owner type name, "INTERDICT") in the list +0x180 of every enemy player, player-list order.
            o.interdicting = 1;
            for (int32_t p = 0; p < static_cast<int32_t>(combat_->player_count()); ++p)
                if (combat_->is_enemy(o.owner, p)) source_entries_.push_back(SourceEntry{p, o.id, desc(o).name_crc, 0});
        }
    } else {
        if (o.interdicting || active_state(o, kInterdict)) {
            o.interdicting = 0;
            const ObjectId id = o.id;
            source_entries_.erase(std::remove_if(source_entries_.begin(), source_entries_.end(),
                                                 [id](const SourceEntry& e) { return e.source == id && e.kind == 0; }),
                                  source_entries_.end());
            if (rec && rec->disable_movement) immobilize(o, -1, frame);  // [M]
            event(AbilityEventKind::InterdictOff, kInterdict, o.id, kNoObject, frame);
        }
    }
}

// ================================================================================================
// countdown (2.6)
// ================================================================================================

void AbilitySystem::update_countdown(Obj& o, uint32_t frame) {
    if (!desc(o).countdown || o.dead) return;
    const auto delta = static_cast<int32_t>(frame - o.cd.last_update);
    o.cd.last_update = frame;
    for (uint32_t t = 0; t < kAbilitySlots; ++t) {
        if (o.cd.phase[t] == 0) {
            if (o.cd.counter[t] > 0) {
                o.cd.counter[t] = std::max<int32_t>(0, o.cd.counter[t] - delta);
                if (o.cd.counter[t] == 0 && can(o, t, true, true, frame))
                    event(AbilityEventKind::Ready, static_cast<uint8_t>(t), o.id, kNoObject, frame);
            }
        } else if (o.cd.counter[t] < o.cd.maximum[t]) {  // [port] expiry fires once (X = 0 types stay at max)
            o.cd.counter[t] = std::min<int32_t>(o.cd.counter[t] + delta, o.cd.maximum[t]);
            if (o.cd.counter[t] == o.cd.maximum[t]) expire(o, t, frame);
            if (o.dead) return;
        }
    }
}

void AbilitySystem::expire(Obj& o, uint32_t t, uint32_t frame) {
    // 0x42f460: X = 1 -> Set_Unit_Ability_Active(team leader or self, off), phase 0, recharge.
    if (info(t).auto_deactivate) {
        Obj* target = &o;
        if (o.container != kNoObject && !info(t).per_unit)
            if (Obj* c = obj(o.container)) target = c;
        const ObjectId oid = o.id;
        set_active(*target, t, false, frame, kNoObject);
        Obj* self = obj(oid);
        if (!self) return;
        self->cd.phase[t] = 0;
        start_recharge(*self, t);
        event(AbilityEventKind::Ended, static_cast<uint8_t>(t), oid, kNoObject, frame);
        return;
    }
    event(AbilityEventKind::Ended, static_cast<uint8_t>(t), o.id, kNoObject, frame);
}

// ================================================================================================
// special abilities (3)
// ================================================================================================

AbilitySystem::SpecialState* AbilitySystem::special_by_crc(Obj& o, uint32_t crc) {
    if (crc == 0) return nullptr;
    for (SpecialState& sp : o.specials)
        if (data_->specials[sp.desc].name_crc == crc) return &sp;
    return nullptr;
}

bool AbilitySystem::allowed(const SpecialAbilityDesc& s) const {
    return cfg_.abilities_enabled && style_allowed(s.style, cfg_.mode, cfg_.skirmish);
}

bool AbilitySystem::bm_blocked(int32_t player, bool must_be_bought, uint32_t crc) const {
    // P4 tactical: the campaign Black Market gate (0x39bbd0 / 0x39b0b0); skirmish always passes.
    if (cfg_.skirmish || !must_be_bought || player < 0 || static_cast<size_t>(player) >= bm_blocked_.size()) return false;
    const std::vector<uint32_t>& v = bm_blocked_[static_cast<size_t>(player)];
    return std::binary_search(v.begin(), v.end(), crc);
}

bool AbilitySystem::bm_ok(const Obj& o, uint32_t t) const {
    // P4 tactical: slot t's record (team containers: the first live member's, 0x397d90).
    const UnitAbilityRecord* r = record_of(o, t);
    return !r || !bm_blocked(o.owner, r->must_be_bought_on_black_market, r->gui_name_crc);
}

bool AbilitySystem::enabled(const Obj& o, const SpecialAbilityDesc& s) const {
    if (bm_blocked(o.owner, s.must_be_bought_on_black_market, s.name_crc)) return false;
    for (const TypeOverride& t : type_overrides_)
        if (t.player == o.owner && t.desc == o.desc && t.crc == s.name_crc) return t.enabled != 0;
    return enabled(static_cast<int32_t>(o.owner), s);
}

bool AbilitySystem::enabled(int32_t player, const SpecialAbilityDesc& s) const {
    if (bm_blocked(player, s.must_be_bought_on_black_market, s.name_crc)) return false;  // P4 tactical (0x39b0b0)
    for (const PlayerOverride& p : overrides_)
        if (p.player == player && p.crc == s.name_crc) return p.enabled != 0;
    return s.initially_enabled;
}

bool AbilitySystem::activate_by_name(Obj& o, uint32_t crc, ObjectId target, int32_t hp, uint32_t frame, uint8_t trigger) {
    // SAP_Activate_By_Name (0x37b8a0): allowed in mode, enabled, apply.
    SpecialState* sp = special_by_crc(o, crc);
    if (!sp) return false;
    const SpecialAbilityDesc& s = data_->specials[sp->desc];
    if (!allowed(s) || !enabled(o, s)) return false;
    sp->trigger = trigger;
    return apply(o, *sp, target, hp, frame);
}

bool AbilitySystem::target_filter(const SpecialAbilityDesc& s, ObjectId target) const {
    // 0x48e290: CategoryMask hits Applicable_Unit_Categories or the type is listed; not excluded.
    const combat::UnitDesc& td = combat_->data().units[combat_->desc_index(target)];
    for (int32_t x : s.excluded_types)
        if (x >= 0 && x == td.object_type) return false;
    if (s.applicable_categories & td.category_mask) return true;
    for (int32_t a : s.applicable_types)
        if (a >= 0 && a == td.object_type) return true;
    return false;
}

bool AbilitySystem::in_range(const Obj& o, const SpecialAbilityDesc& s, ObjectId target) const {
    // Target_In_Range (0x48d820) [M]: Activation_Min/Max_Range when given (XY), else the unit's
    // attack range + target radius. A container is in range when any live member is [port].
    const Vec3 tp = combat_->position(target);
    const float tr = combat_->data().units[combat_->desc_index(target)].radius;
    auto check = [&](ObjectId u) {
        const float d = combat::distance_xy(combat_->position(u), tp);
        if (s.max_range > 0.0f) return d >= s.min_range && d <= s.max_range;
        return d <= combat_->attack_range(u) + tr;
    };
    if (o.combat_unit) return check(o.id);
    for (ObjectId m : o.members)
        if (alive_unit(m) && check(m)) return true;
    return false;
}

bool AbilitySystem::accepts(const Obj& o, const SpecialAbilityDesc& s, ObjectId target, uint32_t frame) const {
    // vfunc +0x28 per class
    switch (s.cls) {
        case SpecialClass::TractorBeam: {
            if (!alive_unit(target) || !combat_->is_enemy(o.owner, combat_->owner(target))) return false;
            if (!target_filter(s, target)) return false;
            if (combat_->flags(target) & combat::kUnitLocomotorBusy) return false;
            if (!can(o, kTractorBeam, false, true, frame)) return false;
            if (const Obj* to = obj(target))
                if (to->locomotor_busy || in_nebula_lockout(*to, frame)) return false;
            return true;
        }
        case SpecialClass::IonCannonShot:
        case SpecialClass::LuckyShot:
        case SpecialClass::ConcentrateFire:
        case SpecialClass::EnergyWeapon:
            return alive_unit(target) && combat_->is_enemy(o.owner, combat_->owner(target)) && target_filter(s, target);
        case SpecialClass::Unsupported: return false;
        case SpecialClass::Stealth:
        case SpecialClass::SensorJamming:
        case SpecialClass::LaserDefense:
        case SpecialClass::RedirectBlaster:
        case SpecialClass::AbsorbBlaster:
        case SpecialClass::ForceSight: return true;
        default: return class_accepts(o, s, target, frame);  // OI abilities (ability_classes.cpp)
    }
}

void AbilitySystem::shot_units(Obj& o, std::array<Obj*, 64>& out, uint32_t& n) {
    n = 0;
    if (o.combat_unit) out[n++] = &o;
    for (ObjectId m : o.members) {
        Obj* mo = obj(m);
        if (mo && !mo->dead && n < out.size()) out[n++] = mo;
    }
}

bool AbilitySystem::apply(Obj& o, SpecialState& sp, ObjectId target, int32_t hp, uint32_t frame) {
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    const uint32_t crc = desc(o).name_crc;
    switch (s.cls) {
        case SpecialClass::TractorBeam: {  // 0x710130 [H]
            if (!alive_unit(target) || !in_range(o, s, target)) return false;
            if (sp.applied) remove_special(o, sp, frame);
            if (!set_active(o, kTractorBeam, true, frame, target)) return false;
            float v = -s.target_speed_decrease;
            const Obj* to = obj(target);
            const combat::UnitDesc& td = combat_->data().units[combat_->desc_index(target)];
            const bool hero = to && desc(*to).hero;
            if ((td.category_mask & cfg_.corvette_category) && !hero) v = v * 2.0f;  // corvettes x2
            if (combat::ModifierSet* m = combat_->unit_modifiers(target))
                m->set_by_source(ModList::MaxSpeed, s.stacking_category, v, combat::kPermanent, crc, o.id);
            sp.applied = 1;
            sp.target = target;
            sp.hp = -1;
            event(AbilityEventKind::EffectOn, kTractorBeam, o.id, target, frame);
            return true;
        }
        case SpecialClass::IonCannonShot:
        case SpecialClass::LuckyShot: {  // 0x704980 / 0x706320
            const bool ion = s.cls == SpecialClass::IonCannonShot;
            const uint32_t t = ion ? kIonCannonShot : kLuckyShot;
            const size_t k = ion ? 0u : 1u;
            std::array<Obj*, 64> units{};
            uint32_t n = 0;
            shot_units(o, units, n);
            if (o.active[t]) return false;
            for (uint32_t i = 0; i < n; ++i)
                if (units[i]->active[t]) return false;
            if (!alive_unit(target) || !in_range(o, s, target)) return false;
            sp.applied = 1;
            sp.target = target;
            sp.hp = hp;
            sp.fired = 0;
            o.active[t] = 1;
            o.shot_target[k] = target;
            o.shot_hp[k] = hp;
            o.lucky_fired = 0;
            for (uint32_t i = 0; i < n; ++i) {
                Obj& u = *units[i];
                u.active[t] = 1;
                u.shot_target[k] = target;
                u.shot_hp[k] = hp;
                u.shot_proj[k] = -1;
                const UnitAbilityRecord* r = own_record(u, t);
                if (!r) r = record_of(o, t);
                if (r)
                    for (int32_t p : r->projectile_override)
                        if (p >= 0) {
                            u.shot_proj[k] = p;
                            break;
                        }
                u.lucky_fired = 0;
                combat_->reset_weapon_burst(u.id);
                combat_->order_attack(u.id, target, hp);
                sync_combat(u);
            }
            event(AbilityEventKind::EffectOn, static_cast<uint8_t>(t), o.id, target, frame);
            return true;
        }
        case SpecialClass::ConcentrateFire: {  // 0x6f5550
            const UnitAbilityRecord* r37 = record_of(o, kConcentrateFire);
            if (!r37 || !(r37->expiration_seconds > 0.0f) || !desc(o).countdown) return false;
            if (!alive_unit(target) || !in_range(o, s, target)) return false;
            const Vec3 c = position(o);
            const size_t count = combat_->unit_count();
            for (size_t i = 0; i < count; ++i) {  // owner's units within Effective_Radius, ascending id
                const ObjectId u = combat_->unit_id_at(i);
                if (combat_->owner(u) != o.owner || !combat_->can_attack(u)) continue;
                if (const Obj* uo = obj(u))
                    if (active_state(*uo, kHunt)) continue;
                if (combat::distance_xy(c, combat_->position(u)) > r37->effective_radius) continue;
                combat_->order_attack(u, target);  // Attack command (6) executed directly
            }
            if (!set_active(o, kConcentrateFire, true, frame, target)) return false;
            start_active(o, kConcentrateFire);
            if (combat::ModifierSet* m = combat_->unit_modifiers(target))
                m->set_by_source(ModList::MaxSpeed, s.stacking_category, -s.target_speed_decrease, combat::kPermanent, crc, o.id);
            sp.applied = 1;
            sp.target = target;
            event(AbilityEventKind::EffectOn, kConcentrateFire, o.id, target, frame);
            return true;
        }
        case SpecialClass::EnergyWeapon: {  // 0x6f9ad0 [M]
            if (!alive_unit(target) || !in_range(o, s, target)) return false;
            if (!set_active(o, kEnergyWeapon, true, frame, target)) return false;
            start_active(o, kEnergyWeapon);
            sp.applied = 1;
            sp.target = target;
            event(AbilityEventKind::EffectOn, kEnergyWeapon, o.id, target, frame);
            return true;
        }
        case SpecialClass::Stealth: {  // 0x5926e0 [H]
            if (!o.combat_unit) return false;
            const uint8_t t = sp.trigger ? sp.trigger : static_cast<uint8_t>(kStealth);
            const UnitAbilityRecord* r = record_of(o, t);
            const float e = r ? r->expiration_seconds : 0.0f;
            // (1 + s), s = the owner player's stealth-duration list +0x268 (OI abilities: Battlefield_Modifier entries,
            // plus the host-set value)
            float f = 1.0f + player_reduce(o.owner, static_cast<uint8_t>(PlayerList::StealthDuration));
            if (o.owner >= 0 && static_cast<size_t>(o.owner) < stealth_bonus_.size()) f = f + stealth_bonus_[static_cast<size_t>(o.owner)];
            if (f <= 0.0f) f = 1.0f;
            o.stealth_frames = static_cast<int32_t>(static_cast<float>(cfg_.fps) * f * e);
            o.stealthed = 1;
            o.last_detected = 0;
            combat_->clear_targets(o.id);
            sp.applied = 1;
            sp.target = o.id;
            refresh_stealth_flag(o, frame);
            event(AbilityEventKind::StealthOn, t, o.id, kNoObject, frame);
            return true;
        }
        case SpecialClass::SensorJamming: {  // 0x719000 [H]
            const UnitAbilityRecord* r63 = own_record(o, kSensorJamming);
            sp.frames = r63 ? static_cast<int32_t>(static_cast<float>(cfg_.fps) * r63->expiration_seconds)
                            : static_cast<int32_t>(static_cast<float>(cfg_.fps) * s.duration_secs);
            sp.applied = 1;
            sp.target = o.id;
            if (o.combat_unit) combat_->add_emitter_entry(o.id);  // OI abilities: Add_Emitter on every apply (§1.7)
            update_emitter(o);
            event(AbilityEventKind::EffectOn, kSensorJamming, o.id, kNoObject, frame);
            return true;
        }
        case SpecialClass::LaserDefense: {  // 0x7161b0 [H]
            start_recharge(o, kLaserDefense);  // "recharge bookkeeping" [M]
            sp.frames = static_cast<int32_t>(static_cast<float>(cfg_.fps) * s.defense_duration);
            sp.cooldown = 0;
            sp.applied = 1;
            sp.target = o.id;
            event(AbilityEventKind::EffectOn, kLaserDefense, o.id, kNoObject, frame);
            return true;
        }
        case SpecialClass::RedirectBlaster:
        case SpecialClass::AbsorbBlaster: return false;  // only through the Take_Damage trigger
        default: return class_apply(o, sp, target, hp, frame);  // OI abilities (ability_classes.cpp)
        case SpecialClass::ForceSight: {  // fog follow-up: ForceSightAbilityClass 0x713ba0 (FOG_OF_WAR.md §3.3)
            // While channelling (Force_Sight_Duration_In_Secs [M]): Reveal_All for the owner; the recharge starts at
            // activation [port].
            {  // §4.14 gates: busy -> refused; Force_Revel_Begin / Loop -> already running; Force_Revel_End -> refused
                const AnimView v = anim_before(o, frame);
                if (anim_busy(o, frame) || v.type == kAnimRevealEnd) return false;
                if (v.type == kAnimRevealBegin || v.type == kAnimRevealLoop) return true;
            }
            if (sp.applied) return false;
            stop_order(o.id, frame);  // OI abilities: the apply stops the unit (animation 0x69, §2.3.10)
            play_anim(o, kAnimRevealBegin, {}, 0, false, 1.0f, kAnimHoldLoop, frame);  // §4.14: Begin, then the Loop
            sp.frames = static_cast<int32_t>(static_cast<float>(cfg_.fps) * s.force_sight_duration);
            sp.applied = 1;
            sp.target = o.id;
            sp.player = o.owner;
            start_recharge(o, sp.trigger ? sp.trigger : static_cast<uint8_t>(kForceSight));
            if (fog_reveal_all_ && sp.player >= 0) fog_reveal_all_(sp.player, true);
            event(AbilityEventKind::EffectOn, kForceSight, o.id, kNoObject, frame);
            return true;
        }
        case SpecialClass::Unsupported: return false;
    }
    return false;
}

void AbilitySystem::remove_special(Obj& o, SpecialState& sp, uint32_t frame) {
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    const uint32_t crc = desc(o).name_crc;
    const ObjectId target = sp.target;
    sp.applied = 0;
    switch (s.cls) {
        case SpecialClass::TractorBeam:  // 0x710880
            set_active(o, kTractorBeam, false, frame, kNoObject);
            if (combat::ModifierSet* m = combat_->unit_modifiers(target)) m->remove_source_in_list(ModList::MaxSpeed, o.id, crc);
            if (Obj* to = obj(target)) to->tractored = 0;
            deactivate_early(o, kTractorBeam, frame);
            break;
        case SpecialClass::IonCannonShot:
        case SpecialClass::LuckyShot: {  // 0x705250 / 0x7069e0
            const bool ion = s.cls == SpecialClass::IonCannonShot;
            const uint32_t t = ion ? kIonCannonShot : kLuckyShot;
            const size_t k = ion ? 0u : 1u;
            std::array<Obj*, 64> units{};
            uint32_t n = 0;
            shot_units(o, units, n);
            bool fired = o.lucky_fired != 0;
            uint32_t still = 0, members = 0;  // OI abilities: k members still active of n (0x705250)
            for (uint32_t i = 0; i < n; ++i)
                if (units[i] != &o) {
                    ++members;
                    if (units[i]->active[t]) ++still;
                }
            for (uint32_t i = 0; i < n; ++i) {
                Obj& u = *units[i];
                fired = fired || u.lucky_fired;
                u.active[t] = 0;
                u.shot_target[k] = kNoObject;
                u.shot_hp[k] = -1;
                u.shot_proj[k] = -1;
                sync_combat(u);
            }
            o.active[t] = 0;
            o.shot_target[k] = kNoObject;
            o.shot_hp[k] = -1;
            // Recharge (TACTICAL_GAPS §1.11 [H]): ion -> only a team-container owner with some member that fired (k < n);
            // a non-team owner never recharges on this path. Lucky -> only if the shot fired.
            if (ion ? (o.is_container && still < members) : fired) start_recharge(o, t);
            o.lucky_fired = 0;
            break;
        }
        case SpecialClass::ConcentrateFire:  // 0x6f5d50
            if (active_state(o, kConcentrateFire)) set_active(o, kConcentrateFire, false, frame, kNoObject);
            if (combat::ModifierSet* m = combat_->unit_modifiers(target)) m->remove_source_in_list(ModList::MaxSpeed, o.id, crc);
            break;
        case SpecialClass::EnergyWeapon:
            if (active_state(o, kEnergyWeapon)) {
                set_active(o, kEnergyWeapon, false, frame, kNoObject);
                deactivate_early(o, kEnergyWeapon, frame);
            }
            break;
        case SpecialClass::Stealth:
            if (o.stealthed) break_stealth(o, frame);
            break;
        case SpecialClass::SensorJamming:
            sp.frames = 0;
            update_emitter(o);
            break;
        case SpecialClass::LaserDefense:
            sp.frames = 0;
            break;
        case SpecialClass::ForceSight:  // fog follow-up: 0x713d20 Undo Reveal_All for the player it was applied for
            if (fog_reveal_all_ && sp.player >= 0) fog_reveal_all_(sp.player, false);
            sp.player = -1;
            sp.frames = 0;
            play_anim(o, kAnimRevealEnd, {}, 0, false, 1.0f, kAnimIdle, frame);  // §4.14: End one-shot, then the idle
            break;
        default: class_remove(o, sp, frame); break;  // OI abilities
    }
    sp.target = kNoObject;
    sp.hp = -1;
    event(AbilityEventKind::Ended, sp.trigger, o.id, target, frame);
}

void AbilitySystem::update_special(Obj& o, SpecialState& sp, uint32_t frame) {
    const SpecialAbilityDesc& s = data_->specials[sp.desc];
    switch (s.cls) {
        case SpecialClass::TractorBeam: {  // 0x710710
            const bool keep = active_state(o, kTractorBeam) && can(o, kTractorBeam, false, true, frame) &&
                              alive_unit(sp.target) && in_range(o, s, sp.target) &&
                              !(obj(sp.target) && in_nebula_lockout(*obj(sp.target), frame));
            if (!keep) {
                remove_special(o, sp, frame);
                return;
            }
            if (Obj* to = obj(sp.target)) to->tractored = 1;
            return;
        }
        case SpecialClass::IonCannonShot:
        case SpecialClass::LuckyShot: {  // 0x704f50 / 0x706850
            const bool ion = s.cls == SpecialClass::IonCannonShot;
            const uint32_t t = ion ? kIonCannonShot : kLuckyShot;
            if (!alive_unit(sp.target) || (obj(sp.target) && in_nebula_lockout(*obj(sp.target), frame))) {
                remove_special(o, sp, frame);
                return;
            }
            std::array<Obj*, 64> units{};
            uint32_t n = 0;
            shot_units(o, units, n);
            bool any = false;
            for (uint32_t i = 0; i < n; ++i)
                if (units[i]->active[t] && can(*units[i], t, false, true, frame)) any = true;
            if (!any) {
                remove_special(o, sp, frame);
                return;
            }
            const size_t k = ion ? 0u : 1u;
            if (sp.hp >= 0 && !combat_->hardpoint_targetable(sp.target, static_cast<uint32_t>(sp.hp))) {
                sp.hp = -1;  // chosen hardpoint died / untargetable
                o.shot_hp[k] = -1;
                for (uint32_t i = 0; i < n; ++i) units[i]->shot_hp[k] = -1;
            }
            for (uint32_t i = 0; i < n; ++i) {
                Obj& u = *units[i];
                if (!u.active[t]) continue;
                if (combat_->current_target(u.id) != sp.target || combat_->ordered_hardpoint(u.id) != sp.hp)
                    combat_->order_attack(u.id, sp.target, sp.hp);
            }
            return;
        }
        case SpecialClass::ConcentrateFire:  // 0x6f5cc0
            if (!active_state(o, kConcentrateFire)) remove_special(o, sp, frame);
            return;
        case SpecialClass::EnergyWeapon: {  // 0x6f9e60 [M]
            const bool keep = active_state(o, kEnergyWeapon) && can(o, kEnergyWeapon, false, true, frame) &&
                              alive_unit(sp.target) && in_range(o, s, sp.target) &&
                              !(obj(sp.target) && in_nebula_lockout(*obj(sp.target), frame));
            if (!keep) {
                remove_special(o, sp, frame);
                return;
            }
            combat::DamageRequest r;
            r.victim = sp.target;
            r.amount = s.damage_per_frame;
            r.kind = combat::kKindNormal;
            r.source = o.id;
            r.hardpoint = combat_->choose_target_hardpoint(o.id, sp.target);
            combat_->take_damage(r, frame);
            return;
        }
        case SpecialClass::Stealth:  // 0x592e80
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
        case SpecialClass::SensorJamming: {  // 0x719620
            if (s.style == ActivationStyle::CombatAutomatic) sp.frames = 10;
            if (in_nebula_lockout(o, frame)) {
                remove_special(o, sp, frame);
                return;
            }
            float r = combat_->data().units[combat_->desc_index(o.id)].passive_missile_shield_radius;
            if (const UnitAbilityRecord* r63 = own_record(o, kSensorJamming)) r = r63->effective_radius;
            if (o.combat_unit) combat_->apply_jamming(o.owner, position(o), r, frame);
            sp.frames -= 1;
            if (sp.frames <= 0) remove_special(o, sp, frame);
            return;
        }
        case SpecialClass::LaserDefense: {  // 0x716730
            if (sp.cooldown <= 0) {
                // 0x716230: first enemy projectile of a targeted category that flew far enough and is
                // within Protection_Radius (3D) is destroyed; serial order (= creation order).
                const Vec3 c = position(o);
                const size_t np = combat_->projectile_count();
                for (size_t i = 0; i < np; ++i) {
                    const combat::ProjectileView v = combat_->projectile(i);
                    if (!v.alive || !combat_->is_enemy(o.owner, v.owner)) continue;
                    if (!(s.targeted_categories & (1u << static_cast<uint32_t>(v.category)))) continue;
                    if (!(v.travelled > s.dist_travelled)) continue;
                    if (combat::distance3(v.pos, c) > s.protection_radius) continue;
                    combat_->detonate_projectile(i, frame);
                    event(AbilityEventKind::Zap, kLaserDefense, o.id, v.shooter, frame);
                    break;
                }
                sp.cooldown = static_cast<int32_t>(static_cast<float>(cfg_.fps) * s.zap_recharge);
            } else {
                sp.cooldown -= 1;
            }
            sp.frames -= 1;
            if (sp.frames <= 0) remove_special(o, sp, frame);
            return;
        }
        case SpecialClass::ForceSight:  // fog follow-up: channelling countdown
            sp.frames -= 1;
            if (sp.frames <= 0) remove_special(o, sp, frame);
            return;
        default: class_update(o, sp, frame); return;  // OI abilities
    }
}

void AbilitySystem::sap_update(Obj& o, uint32_t frame) {
    // 0x37da80: Abilities in list order; due ones updated, then rescheduled.
    for (SpecialState& sp : o.specials) {
        if (o.dead) return;
        const SpecialAbilityDesc& s = data_->specials[sp.desc];
        if (interval_of(s) <= 0 || !enabled(o, s)) continue;
        if (sp.due > frame) continue;
        if (sp.applied) update_special(o, sp, frame);
        sp.due = frame + static_cast<uint32_t>(interval_of(s));
    }
}

void AbilitySystem::try_pending(Obj& o, uint32_t frame) {
    // Targeting_Try_Pending_Ability (0x565de0): retried every targeting service until it applies.
    if (o.pending_type < 0 || o.dead) return;
    if (!alive_unit(o.pending_target)) {  // [port] target gone: the order is finished
        o.pending_type = -1;
        o.pending_target = kNoObject;
        return;
    }
    direct_ctx_ = o.pending_direct;  // OI abilities: autofire-issued pendings run the auto-use pre-checks
    const bool fired = activate_by_name(o, o.pending_crc, o.pending_target, o.pending_hp, frame, static_cast<uint8_t>(o.pending_type));
    direct_ctx_ = 1;
    if (fired) {
        event(AbilityEventKind::PendingFired, static_cast<uint8_t>(o.pending_type), o.id, o.pending_target, frame);
        o.pending_type = -1;
        o.pending_target = kNoObject;
        o.pending_hp = -1;
        o.pending_crc = 0;
    }
}

void AbilitySystem::apply_automatic(Obj& o, uint32_t frame) {
    // Automatic styles on unit add (3.1 event triggers) [M]; target = self.
    for (SpecialState& sp : o.specials) {
        const SpecialAbilityDesc& s = data_->specials[sp.desc];
        if (!style_automatic(s.style) || !allowed(s) || !enabled(o, s)) continue;
        switch (s.cls) {
            case SpecialClass::SensorJamming:
            case SpecialClass::Stealth:
            case SpecialClass::LaserDefense:
            // OI abilities: the event-driven classes (auras, TACTICAL_COMBAT §5.4) and the always-on Force_Healing
            case SpecialClass::CombatBonus:
            case SpecialClass::EnhanceDefense:
            case SpecialClass::BattlefieldModifier:
            case SpecialClass::BasePower:
            case SpecialClass::Weatherproof:
            case SpecialClass::RetreatPrevention:
            case SpecialClass::EnableAbility:
            case SpecialClass::WeaponSwap:  // OI abilities: Permanent_Weapon_Swap (Ground_Automatic) [port: at unit add]
            case SpecialClass::StarbaseUpgrade:  // Skirmish_Automatic on the built upgrade object (HARDPOINT_DISABLED §3)
            case SpecialClass::ForceHealing: apply(o, sp, o.id, -1, frame); break;
            default: break;
        }
    }
}

// ================================================================================================
// stealth (3.7)
// ================================================================================================

bool AbilitySystem::stealthed_now(const Obj& o, uint32_t frame) const {
    if (!o.stealthed) return false;
    const auto d = static_cast<uint32_t>(static_cast<int32_t>(static_cast<float>(cfg_.fps) * cfg_.stealth_detection_time));
    return o.last_detected + d < frame;
}

void AbilitySystem::refresh_stealth_flag(const Obj& o, uint32_t frame) {
    if (o.combat_unit && !o.dead) combat_->set_flag(o.id, combat::kUnitStealthed, stealthed_now(o, frame));
}

void AbilitySystem::break_stealth(Obj& o, uint32_t frame) {
    // 0x592d00
    o.stealth_frames = 0;
    o.stealthed = 0;
    for (SpecialState& sp : o.specials)
        if (sp.applied && data_->specials[sp.desc].cls == SpecialClass::Stealth) {
            sp.applied = 0;
            sp.target = kNoObject;
        }
    refresh_stealth_flag(o, frame);
    event(AbilityEventKind::StealthBroken, kStealth, o.id, kNoObject, frame);
}

void AbilitySystem::detect(Obj& o, uint32_t frame) {
    // 0x592ae0: the first live enemy (ascending id) within 3D distance < its detection range.
    const Vec3 p = position(o);
    const size_t count = combat_->unit_count();
    for (size_t i = 0; i < count; ++i) {
        const ObjectId u = combat_->unit_id_at(i);
        if (!alive_unit(u) || !combat_->is_enemy(o.owner, combat_->owner(u))) continue;
        const Obj* uo = obj(u);
        const float range = uo ? desc(*uo).stealth_detection_range : 0.0f;
        if (!(range > 0.0f)) continue;
        if (combat::distance3(combat_->position(u), p) < range) {
            if (o.last_detected != frame) event(AbilityEventKind::StealthDetected, kStealth, o.id, u, frame);
            o.last_detected = frame;
            return;
        }
    }
}

// ================================================================================================
// public API
// ================================================================================================

ObjectId AbilitySystem::container_of(ObjectId member) const {
    const Obj* o = obj(member);
    return (o && o->container != kNoObject && obj(o->container)) ? o->container : kNoObject;  // removed team: none
}
std::span<const ObjectId> AbilitySystem::members_of(ObjectId container) const {
    const Obj* o = obj(container);
    if (!o || !o->is_container) return {};
    return o->members;
}

bool AbilitySystem::has_ability(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    return o && has(*o, type);
}
bool AbilitySystem::is_active(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    return o && active_state(*o, type);
}
bool AbilitySystem::can_activate(ObjectId id, uint8_t type, bool check_recharge, bool check_nebula, uint32_t frame) const {
    const Obj* o = obj(id);
    return o && can(*o, type, check_recharge, check_nebula, frame) && bm_ok(*o, type);  // P4 tactical: BM gate
}
bool AbilitySystem::is_ready(ObjectId id, uint8_t type, uint32_t frame) const {
    const Obj* o = obj(id);
    if (!o || !has(*o, type)) return false;
    if (!bm_ok(*o, type)) return false;  // P4 tactical: BM gate (Lua Is_Ability_Ready 0x57b580)
    return can(*o, type, true, true, frame) || o->pending_type == type;
}
bool AbilitySystem::black_market_allows(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    return !o || bm_ok(*o, type);
}
bool AbilitySystem::is_autofire(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    return o && type < kAbilitySlots && o->autofire[type] != 0;
}
const UnitAbilityRecord* AbilitySystem::record(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    return o ? record_of(*o, type) : nullptr;
}
int32_t AbilitySystem::countdown_counter(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    return (o && type < kAbilitySlots) ? o->cd.counter[type] : 0;
}
int32_t AbilitySystem::countdown_maximum(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    return (o && type < kAbilitySlots) ? o->cd.maximum[type] : 0;
}
uint8_t AbilitySystem::countdown_phase(ObjectId id, uint8_t type) const {
    const Obj* o = obj(id);
    return (o && type < kAbilitySlots) ? o->cd.phase[type] : uint8_t{0};
}
int32_t AbilitySystem::troopers_deployed(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? o->troopers_deployed : 0;
}
std::span<const ObjectId> AbilitySystem::troopers(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? std::span<const ObjectId>(o->troopers) : std::span<const ObjectId>();
}
int32_t AbilitySystem::blast_counter(ObjectId id) const {
    const Obj* o = obj(id);
    if (!o) return 0;
    for (const SpecialState& sp : o->specials)
        if (sp.applied && data_->specials[sp.desc].cls == SpecialClass::Blast) return sp.counter;
    return 0;
}
float AbilitySystem::speed_override(ObjectId id) const {
    const Obj* o = obj(id);
    if (!o || o->dead) return -1.0f;
    for (const SpecialState& sp : o->specials)
        if (sp.applied && data_->specials[sp.desc].cls == SpecialClass::Berserker) return data_->specials[sp.desc].berserker_speed;
    return -1.0f;
}
int32_t AbilitySystem::pending_type(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? o->pending_type : -1;
}
ObjectId AbilitySystem::pending_target(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? o->pending_target : kNoObject;
}
bool AbilitySystem::is_stealthed(ObjectId id, uint32_t frame) const {
    const Obj* o = obj(id);
    return o && stealthed_now(*o, frame);
}
bool AbilitySystem::tractored(ObjectId id) const {
    const Obj* o = obj(id);
    return o && o->tractored;
}
bool AbilitySystem::interdicting(ObjectId id) const {
    const Obj* o = obj(id);
    return o && o->interdicting;
}
bool AbilitySystem::interdicted(int32_t player) const {
    for (const SourceEntry& e : source_entries_)  // OI abilities: the player's interdict list (+0x180)
        if (e.player == player && e.kind == 0) return true;
    return false;
}
int32_t AbilitySystem::immobilize_count(ObjectId id) const {
    const Obj* o = obj(id);
    return o ? o->immobilize : 0;
}
int32_t AbilitySystem::anim_type_at(ObjectId id, uint32_t frame) const {
    const Obj* o = obj(id);
    return o ? anim_view(*o, frame).type : -1;
}
bool AbilitySystem::lock_movement(ObjectId id, bool lock, uint32_t frame) {
    Obj* o = obj(id);
    if (!o) return false;
    immobilize(*o, lock ? +1 : -1, frame);
    return true;
}
bool AbilitySystem::special_applied(ObjectId id, uint32_t crc) const {
    const Obj* o = obj(id);
    if (!o) return false;
    for (const SpecialState& sp : o->specials)
        if (data_->specials[sp.desc].name_crc == crc) return sp.applied != 0;
    return false;
}
ObjectId AbilitySystem::special_target(ObjectId id, uint32_t crc) const {
    const Obj* o = obj(id);
    if (!o) return kNoObject;
    for (const SpecialState& sp : o->specials)
        if (data_->specials[sp.desc].name_crc == crc) return sp.target;
    return kNoObject;
}
// R5 jamming visual: presentation-only const read (no state, hash or RNG involved).
const SpecialAbilityDesc* AbilitySystem::jamming_special(ObjectId id, float& radius) const {
    const Obj* o = obj(id);
    if (!o || o->dead) return nullptr;
    for (const SpecialState& sp : o->specials) {
        if (!sp.applied || data_->specials[sp.desc].cls != SpecialClass::SensorJamming) continue;
        // The radius the update (0x719620) jams with: unit ability 63's Effective_Radius, else Passive_Missile_Shield_Radius.
        float r = 0.0f;
        if (o->combat_unit) r = combat_->data().units[combat_->desc_index(o->id)].passive_missile_shield_radius;
        if (const UnitAbilityRecord* r63 = own_record(*o, kSensorJamming)) r = r63->effective_radius;
        radius = r;
        return &data_->specials[sp.desc];
    }
    return nullptr;
}

uint32_t AbilitySystem::set_unit_ability_mode(std::span<const ObjectId> objects, uint8_t type, bool on, uint32_t frame,
                                              ObjectId target) {
    uint32_t n = 0;
    for (ObjectId id : objects) {
        Obj* o = obj(id);
        if (!o) continue;
        if (!can(*o, type, on, on, frame)) continue;                              // 1
        if (on && !bm_ok(*o, type)) continue;  // P4 tactical: BM gate (GUI / Lua Activate_Ability check it first)
        if (active_state(*o, type) == on) continue;                               // 2
        if (on && desc(*o).countdown && !o->cd.counter_zero(type)) continue;      // 3
        if (set_active(*o, type, on, frame, target)) ++n;                         // 4
    }
    return n;
}

uint32_t AbilitySystem::targeted_ability(std::span<const ObjectId> users, uint8_t type, ObjectId target, int32_t hardpoint,
                                         uint32_t frame) {
    if (type < 1 || type > kMaxAbilityType + 1) return 0;
    if (!alive_unit(target)) return 0;  // target must exist and not be dying
    uint32_t n = 0;
    for (ObjectId id : users) {
        Obj* o = obj(id);
        if (!o || o->dead || o->container != kNoObject) continue;  // squad members are skipped
        if (!can(*o, type, true, true, frame)) continue;
        if (!bm_ok(*o, type)) continue;  // P4 tactical: BM gate
        const UnitAbilityRecord* r = record_of(*o, type);
        if (!r) continue;
        SpecialState* sp = special_by_crc(*o, r->gui_name_crc);
        if (!sp) continue;
        const SpecialAbilityDesc& s = data_->specials[sp->desc];
        if (!allowed(s) || !enabled(*o, s) || !accepts(*o, s, target, frame)) continue;
        // Issue_Auto_Target (0x38d730): attack order + pending ability.
        o->pending_target = target;
        o->pending_hp = hardpoint;
        o->pending_crc = r->gui_name_crc;
        o->pending_type = type;
        o->pending_direct = 1;  // OI abilities: a player / Lua order (pending context flag)
        if (o->combat_unit) combat_->order_attack(o->id, target, hardpoint);
        for (ObjectId m : o->members)
            if (alive_unit(m)) combat_->order_attack(m, target, hardpoint);
        event(AbilityEventKind::TargetIssued, type, o->id, target, frame);
        ++n;
    }
    return n;
}

void AbilitySystem::set_autofire(std::span<const ObjectId> objects, uint8_t type, bool on) {
    // Command 58 (0x4b3860) on listed non-member objects that have it, through the setter 0x3a78b0 (OI abilities: slot
    // byte + map entry, Supports_Autofire gate).
    if (type >= kAbilitySlots) return;
    for (ObjectId id : objects) {
        Obj* o = obj(id);
        if (o && o->container == kNoObject && has(*o, type)) set_ability_autofire(id, type, on);
    }
}

bool AbilitySystem::set_unit_ability_active(ObjectId id, uint8_t type, bool on, uint32_t frame, ObjectId target) {
    Obj* o = obj(id);
    return o && set_active(*o, type, on, frame, target);
}

bool AbilitySystem::force_recharge(ObjectId id, uint8_t type, float seconds) {
    Obj* o = obj(id);
    if (!o || type >= kAbilitySlots) return false;
    if (seconds < 0.0f) {
        start_recharge(*o, type);
    } else {
        o->cd.set_duration(type, seconds, cfg_.fps);  // phase unchanged
    }
    return true;
}

bool AbilitySystem::reset_counter(ObjectId id, uint8_t type) {
    Obj* o = obj(id);
    if (!o || type >= kAbilitySlots || !o->cd.counter_zero(type)) return false;
    o->cd.counter[type] = 1;
    o->cd.maximum[type] = 1;
    return true;
}

void AbilitySystem::set_nebula(ObjectId id, uint32_t frame) {
    if (Obj* o = obj(id)) o->nebula_frame = frame;
}
void AbilitySystem::set_force_disabled(ObjectId id, uint32_t frame) {
    if (Obj* o = obj(id)) o->force_frame = frame;
}
void AbilitySystem::set_ion_storm(ObjectId id, uint32_t frame) {
    if (Obj* o = obj(id)) o->ion_storm_frame = frame;
}
// ---- L1 systems: space fields (L1_S4_REMAINING.md §3.3) ----
void AbilitySystem::clear_nebula(ObjectId id) {
    if (Obj* o = obj(id)) o->nebula_frame = kNever;
}
void AbilitySystem::clear_ion_storm(ObjectId id) {
    if (Obj* o = obj(id)) o->ion_storm_frame = kNever;
}
bool AbilitySystem::nebula_stamped(ObjectId id) const {
    const Obj* o = obj(id);
    return o && o->nebula_frame != kNever;
}
bool AbilitySystem::nebula_lockout(ObjectId id, uint32_t frame) const {
    const Obj* o = obj(id);
    return o && in_nebula_lockout(*o, frame);
}
bool AbilitySystem::ion_storm_lockout(ObjectId id, uint32_t frame) const {
    const Obj* o = obj(id);
    return o && o->ion_storm_frame != kNever &&
           static_cast<float>(frame - o->ion_storm_frame) / static_cast<float>(cfg_.fps) < cfg_.ion_storm_shield_disable_time;
}
uint32_t AbilitySystem::cancel_active_abilities(ObjectId id, uint32_t frame) {
    Obj* o = obj(id);
    if (!o) return 0;
    uint32_t n = 0;
    for (uint32_t t = 0; t < kAbilitySlots; ++t)
        if (has(*o, t) && active_state(*o, t) && set_active(*o, t, false, frame, kNoObject)) ++n;
    return n;
}
bool AbilitySystem::ion_storm_shields_down(ObjectId victim, uint32_t frame) const { return ion_storm_lockout(victim, frame); }
void AbilitySystem::set_locomotor_busy(ObjectId id, bool busy) {
    if (Obj* o = obj(id)) o->locomotor_busy = busy ? 1 : 0;
    combat_->set_flag(id, combat::kUnitLocomotorBusy, busy);
}
void AbilitySystem::set_abilities_disabled(ObjectId id, bool off) {
    if (Obj* o = obj(id)) o->sap_disabled = off ? 1 : 0;
}
void AbilitySystem::set_corrupted(ObjectId id, bool on) {
    if (Obj* o = obj(id)) o->corrupted = on ? 1 : 0;
}
void AbilitySystem::set_weapons_disabled(ObjectId id, bool off) {
    if (Obj* o = obj(id)) o->weapons_disabled = off ? 1 : 0;
}
void AbilitySystem::set_garrison_count(ObjectId id, int32_t n) {
    if (Obj* o = obj(id)) o->garrison_count = n;
}
void AbilitySystem::set_special_enabled(int32_t player, uint32_t crc, bool on) {
    for (PlayerOverride& p : overrides_)
        if (p.player == player && p.crc == crc) {
            p.enabled = on ? 1 : 0;
            return;
        }
    overrides_.push_back(PlayerOverride{player, crc, static_cast<uint8_t>(on ? 1 : 0)});
}
void AbilitySystem::set_player_stealth_bonus(int32_t player, float s) {
    if (player < 0) return;
    if (static_cast<size_t>(player) >= stealth_bonus_.size()) stealth_bonus_.resize(static_cast<size_t>(player) + 1, 0.0f);
    stealth_bonus_[static_cast<size_t>(player)] = s;
}
void AbilitySystem::set_black_market_blocked(int32_t player, std::vector<uint32_t> crcs) {
    // P4 tactical: per-player blocked list, kept sorted / unique (lookup by binary search; order-independent input).
    if (player < 0) return;
    std::sort(crcs.begin(), crcs.end());
    crcs.erase(std::unique(crcs.begin(), crcs.end()), crcs.end());
    if (static_cast<size_t>(player) >= bm_blocked_.size()) bm_blocked_.resize(static_cast<size_t>(player) + 1);
    bm_blocked_[static_cast<size_t>(player)] = std::move(crcs);
}

// ================================================================================================
// hooks
// ================================================================================================

void AbilitySystem::begin_tick(uint32_t frame) {
    for (const Obj& o : objs_) refresh_stealth_flag(o, frame);
}

void AbilitySystem::before_units(uint32_t frame, SimRng& rng) {
    rng_ = &rng;
    mines_update(frame);  // OI abilities: mine objects' ProximityMineBehavior [port: serviced before the units]
    for (size_t i = objs_.size(); i-- > 0;) {  // team containers, newest first [port]
        if (!objs_[i].is_container) continue;
        const ObjectId id = objs_[i].id;
        update_countdown(objs_[i], frame);
        if (Obj* o = obj(id)) try_pending(*o, frame);
        if (Obj* o = obj(id)) behaviors_service(*o, frame);  // OI abilities
        if (Obj* o = obj(id)) sap_update(*o, frame);
    }
}

void AbilitySystem::unit_begin(ObjectId id, uint32_t frame, SimRng& rng) {
    // Per unit (ABILITIES_HARDPOINTS 6.2): countdown -> TARGETING (OI abilities: the pending targeted ability first,
    // 0x565de0, then the scan in combat) ; behaviours (HUNT, SELF_DESTRUCT, CONFUSE) [M: after the pending retry].
    rng_ = &rng;
    if (Obj* o = obj(id)) update_countdown(*o, frame);
    if (Obj* o = obj(id)) try_pending(*o, frame);
    if (Obj* o = obj(id)) behaviors_service(*o, frame);
}

void AbilitySystem::unit_after_targeting(ObjectId id, uint32_t frame, SimRng& rng) {
    rng_ = &rng;
    // OI abilities: Issue_Auto_Target for a target the scan just acquired -> the autofire driver (§1.3); a squad
    // member's acquisition also drives its container's abilities (team targeting) [port: the first live member].
    if (combat_->acquired_target(id)) {
        const ObjectId tgt = combat_->auto_target(id);
        if (Obj* o = obj(id)) autofire_driver(*o, tgt, frame);
        if (Obj* o = obj(id); o && o->container != kNoObject) {
            const ObjectId cid = o->container;
            if (Obj* c = obj(cid)) {
                ObjectId first = kNoObject;
                for (ObjectId m : c->members)
                    if (alive_unit(m)) {
                        first = m;
                        break;
                    }
                if (first == id) autofire_driver(*c, tgt, frame);
            }
        }
    }
    if (Obj* o = obj(id)) sap_update(*o, frame);
}

bool AbilitySystem::shot_allowed(ObjectId shooter, ObjectId target, int32_t target_hp, int32_t& projectile) {
    // 0x397e00 / 0x394eb0: with 41 / 34 active a unit fires only at the ability target, with the override.
    const Obj* u = obj(shooter);
    if (!u) return true;
    for (size_t k = 0; k < 2; ++k) {
        const uint32_t t = k == 0 ? kIonCannonShot : kLuckyShot;
        if (!u->active[t]) continue;
        const Obj* to = obj(target);
        const bool tmatch = target == u->shot_target[k] || (to && to->container != kNoObject && to->container == u->shot_target[k]);
        const bool hmatch = u->shot_hp[k] < 0 || target_hp == u->shot_hp[k];
        if (!tmatch || !hmatch) return false;
        if (u->shot_proj[k] >= 0) projectile = u->shot_proj[k];
        return true;
    }
    return true;
}

void AbilitySystem::shot_fired(ObjectId shooter, ObjectId target, int32_t /*target_hp*/, int32_t projectile, uint32_t frame) {
    Obj* u = obj(shooter);
    if (!u) return;
    // One volley (0x3a06a0): ion shot at the ion target with the override projectile turns 41 off.
    if (u->active[kIonCannonShot] && target == u->shot_target[0] && projectile == u->shot_proj[0] && projectile >= 0) {
        u->active[kIonCannonShot] = 0;
        sync_combat(*u);
        event(AbilityEventKind::Deactivated, kIonCannonShot, u->id, target, frame);
    }
    if (u->stealthed) break_stealth(*u, frame);  // firing breaks stealth (fade 0.3 s, render)
    // OI follow-up t01 (§4.14): WeaponBehavior 0x54d090 plays Attack (5) in the primary slot unless the type sets
    // Attack_Animation_Is_Overlay. Recorded only for owners whose melee gates read it (Arc / Generic / Eat) and only over
    // no running ability clip (the hold suspends the weapon) [port: variant 0, no draw; the idle replaces it at its end].
    if (host_.attack_overlay && !host_.attack_overlay(shooter) && anim_view(*u, frame).type < 0 &&
        (class_has(*u, SpecialClass::ArcSweep) || class_has(*u, SpecialClass::GenericAttack) ||
         class_has(*u, SpecialClass::EatAttack)))
        play_anim(*u, 5, {}, 0, false, 1.0f, kAnimIdle, frame);
}

bool AbilitySystem::class_has(const Obj& o, SpecialClass cls) const {
    for (const SpecialState& sp : o.specials)
        if (data_->specials[sp.desc].cls == cls) return true;
    return false;
}

AbilitySystem::SpecialState* AbilitySystem::blast_state(Obj& o) {
    for (SpecialState& sp : o.specials)
        if (sp.applied && data_->specials[sp.desc].cls == SpecialClass::Blast) return &sp;
    return nullptr;
}

bool AbilitySystem::fire_gate(ObjectId shooter, const combat::HardpointDesc* hp, int32_t& projectile, float& damage_scale) {
    // REMAINING_GAPS §5.5 [H] (Fire_At 0x3825b0 steps 1 / 4): the special-weapon state counter (SAP pack +0x394).
    //   > 0 charging: every hard point and weapon of the unit refuses;
    //   < 0 charged: only WEAPON_SPECIAL (type 10) hard points fire, with Blast_Ability_Fire_Projectile_Type (+0x220)
    //       when set (else Fire_Projectile_Type +0x210), damage x Damage_Multiplier (special +0x17c).
    Obj* u = obj(shooter);
    if (!u || u->specials.empty()) return true;
    const SpecialState* sp = blast_state(*u);
    if (!sp || sp->counter == 0) return true;
    if (sp->counter > 0) return false;
    if (!hp || hp->kind != HardpointKind::WeaponSpecial) return false;
    if (hp->blast_projectile >= 0) projectile = hp->blast_projectile;
    damage_scale = data_->specials[sp->desc].damage_multiplier;
    return true;
}

void AbilitySystem::fire_done(ObjectId shooter, const combat::HardpointDesc* hp, uint32_t frame) {
    // §5.5: each charged shot does counter += 1; at 0 the charged state ends (meshes hidden, charged particle killed,
    // +0x398 := 0x3fffff) and normal fire resumes.
    Obj* u = obj(shooter);
    if (!u || u->specials.empty() || !hp || hp->kind != HardpointKind::WeaponSpecial) return;
    SpecialState* sp = blast_state(*u);
    if (!sp || sp->counter >= 0) return;
    sp->counter += 1;
    if (sp->counter == 0) remove_special(*u, *sp, frame);
}

float AbilitySystem::lucky_shot(ObjectId source, ObjectId victim, int32_t& hardpoint, Vec3 impact, bool has_impact) {
    // Take_Damage step 11 (3.4) [H].
    Obj* s = obj(source);
    if (!s) return 1.0f;
    Obj* c = s->container != kNoObject ? obj(s->container) : nullptr;
    const bool own = s->active[kLuckyShot] != 0;
    if (!own && !(c && c->active[kLuckyShot])) return 1.0f;
    const Obj* holder = own ? s : c;
    const Obj* vo = obj(victim);
    const ObjectId lt = holder->shot_target[1];
    const bool tmatch = victim == lt || (vo && vo->container != kNoObject && vo->container == lt);
    const bool hmatch = holder->shot_hp[1] < 0 || hardpoint == holder->shot_hp[1];
    if (!tmatch || !hmatch) return 1.0f;
    s->lucky_fired = 1;
    if (c) c->lucky_fired = 1;
    s->active[kLuckyShot] = 0;
    sync_combat(*s);
    const UnitAbilityRecord* r = own_record(*s, kLuckyShot);
    if (!r && c) r = record_of(*c, kLuckyShot);
    const float m = r ? r->combat.mult[static_cast<size_t>(ModKind::CauseDamage) - 1] : 1.0f;
    if (hardpoint < 0)  // hull hit: the hardpoint closest to the impact (0x394a80)
        hardpoint = combat_->closest_hardpoint(victim, has_impact ? impact : combat_->position(source), false);
    event(AbilityEventKind::Deactivated, kLuckyShot, source, victim, 0);
    return m;
}

bool AbilitySystem::damage_listener(ObjectId victim, ObjectId source, int32_t projectile_desc, uint32_t frame, SimRng* rng) {
    // 4.6: style Take_Damage abilities of the victim, list order; the first that applies cancels.
    Obj* o = obj(victim);
    if (!o || o->specials.empty() || projectile_desc < 0 || !rng) return false;
    const combat::ProjectileDesc& pd = combat_->data().projectiles[static_cast<size_t>(projectile_desc)];
    for (SpecialState& sp : o->specials) {
        const SpecialAbilityDesc& s = data_->specials[sp.desc];
        if (s.style != ActivationStyle::TakeDamage || !allowed(s) || !enabled(*o, s)) continue;
        if (s.cls != SpecialClass::RedirectBlaster && s.cls != SpecialClass::AbsorbBlaster) continue;
        if (alive_unit(source) && !target_filter(s, source)) continue;  // shots from these categories / types
        if (s.cls == SpecialClass::RedirectBlaster) {
            // TACTICAL 3.8: categories 0, 5, 7 inside Reaction_Arc; redirect roll, then block roll.
            const ProjectileCategory c = pd.category;
            if (c != ProjectileCategory::Default && c != ProjectileCategory::StaticBomb && c != ProjectileCategory::Laser) continue;
            if (s.reaction_arc_deg < 360.0f && alive_unit(source)) {
                const Vec3 f = combat_->facing(victim);
                const Vec3 d = combat_->position(source) - combat_->position(victim);
                const float lf = dmath::sqrt(f.x * f.x + f.y * f.y), ld = dmath::sqrt(d.x * d.x + d.y * d.y);
                if (lf > 0.0f && ld > 0.0f) {
                    const float cosang = (f.x * d.x + f.y * d.y) / (lf * ld);
                    if (cosang < dmath::cos(s.reaction_arc_deg * 0.5f * kDegToRad)) continue;
                }
            }
            if (rng->range_float(0.0f, 1.0f) < pd.redirect_chance_modifier * s.redirect_chance) {
                event(AbilityEventKind::Redirected, 0, victim, source, frame);
                return true;
            }
            if (rng->range_float(0.0f, 1.0f) < pd.block_chance_modifier * s.block_chance) {
                event(AbilityEventKind::Blocked, 0, victim, source, frame);
                return true;
            }
        } else if (rng->range_float(0.0f, 1.0f) < s.absorb_chance) {  // [L] one Absorb_Chance roll
            event(AbilityEventKind::Blocked, 0, victim, source, frame);
            return true;
        }
    }
    return false;
}

void AbilitySystem::hardpoint_destroyed(ObjectId owner, uint32_t index, HardpointKind kind, bool last, uint32_t frame) {
    hardpoint_offline(owner, index, kind, last, true, frame);
}

void AbilitySystem::set_type_special_enabled(int32_t player, uint32_t desc, uint32_t crc, bool on) {
    for (TypeOverride& t : type_overrides_)
        if (t.player == player && t.desc == desc && t.crc == crc) {
            t.enabled = on ? 1 : 0;
            return;
        }
    type_overrides_.push_back(TypeOverride{player, desc, crc, static_cast<uint8_t>(on ? 1 : 0)});
}

bool AbilitySystem::special_enabled(ObjectId id, uint32_t crc) const {
    const Obj* o = obj(id);
    if (!o) return false;
    for (const SpecialState& sp : o->specials) {
        const SpecialAbilityDesc& s = data_->specials[sp.desc];
        if (s.name_crc == crc) return enabled(*o, s);
    }
    return false;
}

void AbilitySystem::hardpoint_online(ObjectId owner, uint32_t index, HardpointKind kind, uint32_t frame) {
    // Set_System_Online(on) (0x384d00), ability side (HARDPOINT_DISABLED.md §9). No live-count check.
    Obj* o = obj(owner);
    if (!o || o->dead) return;
    switch (kind) {
        case HardpointKind::Engine:  // §9.1: host engines back on; TURBO / SPOILER_LOCK not re-activated
            event(AbilityEventKind::EnginesOnline, 0, owner, kNoObject, frame);
            break;
        case HardpointKind::GravityWell:  // §9.3 (0x3a89c0): +0x3ca := 0; INTERDICT activatable, not re-activated
            o->gravity_well_destroyed = 0;
            break;
        case HardpointKind::EnableSpecialAbility: {  // §9.6: enable on all the player's objects of the owner's type
            const uint32_t crc = combat_ ? combat_->hardpoint_special_ability(owner, index) : 0u;
            if (crc != 0) set_type_special_enabled(o->owner, o->desc, crc, true);
            break;
        }
        default: break;  // shield (combat side clears the regen block; DEFEND not re-activated), weapons, bays, tractor
    }
}

void AbilitySystem::hardpoint_offline(ObjectId owner, uint32_t index, HardpointKind kind, bool last, bool destroyed,
                                      uint32_t frame) {
    // Set_System_Online(off) (0x384d00), ability side.
    Obj* o = obj(owner);
    if (!o || o->dead) return;
    switch (kind) {
        case HardpointKind::Engine:
            if (!last) break;
            if (active_state(*o, kTurbo)) set_active(*o, kTurbo, false, frame, kNoObject);
            if (active_state(*o, kSpoilerLock)) set_active(*o, kSpoilerLock, false, frame, kNoObject);
            event(AbilityEventKind::EnginesOffline, 0, owner, kNoObject, frame);
            events_.back().value = destroyed ? 1 : 0;
            break;
        case HardpointKind::ShieldGenerator:
            if (last && active_state(*o, kDefend)) set_active(*o, kDefend, false, frame, kNoObject);
            break;
        case HardpointKind::GravityWell:
            if (active_state(*o, kInterdict)) set_active(*o, kInterdict, false, frame, kNoObject);
            o->gravity_well_destroyed = 1;
            break;
        case HardpointKind::EnableSpecialAbility: {  // §9.6: disable on all the player's objects of the owner's type
            const uint32_t crc = combat_ ? combat_->hardpoint_special_ability(owner, index) : 0u;
            if (crc != 0) set_type_special_enabled(o->owner, o->desc, crc, false);
            break;
        }
        default: break;
    }
}

void AbilitySystem::unit_dying(ObjectId id, uint32_t frame) {
    Obj* o = obj(id);
    if (!o) return;
    for (SpecialState& sp : o->specials)
        if (sp.applied) remove_special(*o, sp, frame);
    if (o->interdicting) interdict(*o, false, frame);
    o->stealthed = 0;
    o->dead = 1;
}

void AbilitySystem::ion_stunned(ObjectId id, uint32_t frame) {
    Obj* o = obj(id);
    if (o && !o->dead && active_state(*o, kDefend)) set_active(*o, kDefend, false, frame, kNoObject);
}

void AbilitySystem::units_removed() {
    size_t w = 0;
    for (size_t i = 0; i < objs_.size(); ++i) {
        if (objs_[i].combat_unit && !combat_->has_unit(objs_[i].id)) continue;
        if (w != i) objs_[w] = std::move(objs_[i]);
        ++w;
    }
    objs_.erase(objs_.begin() + static_cast<std::ptrdiff_t>(w), objs_.end());
    for (Obj& c : objs_) {
        if (!c.is_container) continue;
        c.members.erase(std::remove_if(c.members.begin(), c.members.end(), [this](ObjectId m) { return find(m) < 0; }),
                        c.members.end());
    }
}

// ================================================================================================
// CRC
// ================================================================================================

void AbilitySystem::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(objs_.size()));
    for (const Obj& o : objs_) {
        h.i32(o.id);
        h.u32(o.desc);
        h.i32(o.owner);
        h.i32(o.container);
        h.u8(o.is_container);
        h.u8(o.dead);
        h.u32(static_cast<uint32_t>(o.members.size()));
        for (ObjectId m : o.members) h.i32(m);
        for (uint32_t t = 0; t < kAbilitySlots; ++t) {
            const uint8_t bits = static_cast<uint8_t>(o.active[t] | (o.autofire[t] << 1) | (o.behavior[t] << 2));
            if (bits) {
                h.u32(t);
                h.u8(bits);
            }
        }
        o.cd.hash(h);
        h.i32(o.pending_target);
        h.i32(o.pending_hp);
        h.u32(o.pending_crc);
        h.i32(o.pending_type);
        h.u32(o.nebula_frame);
        h.u32(o.force_frame);
        h.u32(o.ion_storm_frame);
        h.u8(o.locomotor_busy);
        h.u8(o.sap_disabled);
        h.u8(o.corrupted);
        h.u8(o.weapons_disabled);
        h.i32(o.garrison_count);
        h.u8(o.gravity_well_destroyed);
        h.u8(o.tractored);
        h.u8(o.interdicting);
        h.i32(o.immobilize);
        h.u8(o.stealthed);
        h.i32(o.stealth_frames);
        h.u32(o.last_detected);
        for (size_t k = 0; k < 2; ++k) {
            h.i32(o.shot_target[k]);
            h.i32(o.shot_hp[k]);
            h.i32(o.shot_proj[k]);
        }
        h.u8(o.lucky_fired);
        h.u32(static_cast<uint32_t>(o.specials.size()));
        for (const SpecialState& sp : o.specials) {
            h.u32(sp.due);
            h.u8(sp.applied);
            h.u8(sp.fired);
            h.u8(sp.trigger);
            h.i32(sp.target);
            h.i32(sp.hp);
            h.i32(sp.frames);
            h.i32(sp.cooldown);
            // OI abilities: class state
            h.u32(sp.next);
            h.i32(sp.counter);
            h.u8(sp.phase);
            h.u8(sp.direct);
            h.u8(sp.aura);
            h.f32(sp.radius);
            h.f32(sp.value);
            h.f32(sp.point.x);
            h.f32(sp.point.y);
            h.f32(sp.point.z);
            h.u32(static_cast<uint32_t>(sp.victims.size()));
            for (ObjectId v : sp.victims) h.i32(v);
        }
        // OI abilities: object state
        h.u32(o.heal_due);
        h.u32(static_cast<uint32_t>(o.autofire_map.size()));
        for (const auto& [c, v] : o.autofire_map) {
            h.u32(c);
            h.u8(v);
        }
        h.u32(o.hunt_due);
        h.u8(static_cast<uint8_t>(o.deployed | (o.spread << 1) | (o.self_destruct << 2) | (o.confused << 3) | (o.barrage << 4) |
                                  (o.stunned << 5) | (o.pending_direct << 6)));
        h.u32(o.confuse_end);
        h.i32(o.barrage_dummy);
        // REMAINING_GAPS §5.2: DEPLOY_TROOPERS data, only once used (keeps the digests of trooper-less states)
        if (o.troopers_deployed != 0 || !o.troopers.empty()) {
            h.u32(0x54525053u);  // "TRPS"
            h.i32(o.troopers_deployed);
            h.u32(static_cast<uint32_t>(o.troopers.size()));
            for (ObjectId t : o.troopers) h.i32(t);
        }
        // OI follow-up t01 / t04: the animation record and the lock pairs, only once set (keeps record-less digests)
        if (o.anim_type >= 0 || o.lock_flags != 0) {
            h.u32(0x4D494E41u);  // "ANIM"
            h.i32(o.anim_type);
            h.u32(o.anim_start);
            h.f32(o.anim_rate);
            h.f32(o.anim_end);
            h.u8(o.anim_loop);
            h.u8(o.anim_then);
            h.u8(o.lock_flags);
        }
    }
    // OI abilities: mines, player / source lists, remote bombs, the spawn serial
    h.u32(static_cast<uint32_t>(mines_.size()));
    for (const Mine& m : mines_) {
        h.u32(m.serial);
        h.i32(m.id);
        h.i32(m.owner);
        h.f32(m.pos.x);
        h.f32(m.pos.y);
        h.f32(m.pos.z);
        h.f32(m.trigger);
        h.u32(m.arm);
    }
    h.u32(static_cast<uint32_t>(player_entries_.size()));
    for (const PlayerEntry& e : player_entries_) {
        h.i32(e.player);
        h.u8(e.list);
        h.i32(e.category);
        h.f32(e.value);
        h.i32(e.source);
        h.u32(e.crc);
    }
    h.u32(static_cast<uint32_t>(source_entries_.size()));
    for (const SourceEntry& e : source_entries_) {
        h.i32(e.player);
        h.i32(e.source);
        h.u32(e.crc);
        h.u8(e.kind);
    }
    h.u32(static_cast<uint32_t>(remote_bombs_.size()));
    for (const RemoteBomb& b : remote_bombs_) {
        h.i32(b.player);
        h.u32(b.serial);
    }
    h.u32(spawn_serial_);
    h.u32(static_cast<uint32_t>(overrides_.size()));
    for (const PlayerOverride& p : overrides_) {
        h.i32(p.player);
        h.u32(p.crc);
        h.u8(p.enabled);
    }
    if (!type_overrides_.empty()) {  // §9.6 type overrides: only once one exists (older battles keep their bytes)
        h.u32(0x54594f56u);
        h.u32(static_cast<uint32_t>(type_overrides_.size()));
        for (const TypeOverride& t : type_overrides_) {
            h.i32(t.player);
            h.u32(t.desc);
            h.u32(t.crc);
            h.u8(t.enabled);
        }
    }
    // P4 tactical: bm_blocked_ is not hashed. It is a battle-start input, a pure function of the galactic purchases
    // (galactic CRC / save) and the battle tables, rebuilt by the host at setup; its effects are in the state above.
}

void AbilitySystem::register_crc(CrcRegistry& registry, const std::string& prefix, const std::string& combat_prefix) const {
    registry.add(prefix + ".objects", [this](StateHasher& h) { hash(h); });
    combat_->register_extended_crc(registry, combat_prefix);
}

}  // namespace alamo::tactical::abilities

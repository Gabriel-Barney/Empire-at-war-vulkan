#include "alamo/ui/hud_abilities.hpp"

#include <algorithm>

#include "alamo/core/ascii.hpp"
#include "alamo/ui/targeting.hpp"

#ifdef ALAMO_UI_HAVE_ABILITIES
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/abilities/ability_types.hpp"
#endif

namespace alamo::ui {

std::string ability_icon(uint8_t t) {
    if (t == 0) return {};
#ifdef ALAMO_UI_HAVE_ABILITIES
    using namespace tactical::abilities;
    // [inferred] The original's per-type table holds the icon names; only i_sa_lure / i_sa_stun are confirmed
    // (ABILITIES_HARDPOINTS 2.1). The rest pair each type with the closest i_sa_* name in MT_CommandBar.
    switch (t) {
        case kDefend: return "i_sa_defend_mode.tga";
        case kHarass: return "i_sa_strike.tga";
        case kBarrage: return "i_sa_barrage_area.tga";
        case kAfterburner:
        case kTurbo: return "i_sa_power_to_engines.tga";
        case kAvoidDanger: return "i_sa_evasive_maneuvers.tga";
        case kForceTelekinesis: return "i_sa_force_push.tga";
        case kForceWhirlwind: return "i_sa_force_crush.tga";
        case kForceLightning: return "i_sa_force_lighting.tga";  // spelled so in the atlas
        case kJetPack: return "i_sa_jetpack_jump.tga";
        case kCaptureVehicle: return "i_sa_capture_vehicles.tga";
        case kTargetedInvulnerability:
        case kInvulnerability: return "i_sa_force_protect.tga";
        case kTargetedHack: return "i_sa_hack_turret.tga";
        case kTargetedRepair: return "i_sa_repair_vehicle.tga";
        case kAreaEffectStun: return "i_sa_stun.tga";
        case kAreaEffectHeal: return "i_sa_area_heal.tga";
        case kAreaEffectConvert: return "i_sa_force_confuse.tga";
        case kDistract: return "i_sa_cover_me.tga";
        case kCableAttack: return "i_sa_tow_cable_attack.tga";
        case kFowRevealPing: return "i_sa_sensor_ping.tga";
        case kEnergyWeapon:
        case kSuperLaser: return "i_sa_fire_energy_weapon.tga";
        case kMissileShield: return "i_sa_missile_jamming.tga";
        case kUntargetedStickyBomb: return "i_sa_sticky_bomb.tga";
        case kConcentrateFire: return "i_sa_all_ships_concentrate_fire.tga";
        case kDeployTroopers: return "i_sa_deploy_stormtroopers.tga";
        case kSpoilerLock: return "i_sa_s_foil_mode.tga";
        case kReplenishWingmen: return "i_sa_join_me.tga";
        case kEjectVehicleThief: return "i_sa_undeploy.tga";
        case kFireLobbingSuperweapon: return "i_sa_ion_cannon_shot.tga";
        case kRadioactiveContaminate: return "i_sa_contaminate.tga";
        default: break;
    }
    std::string n = ability_type_info(t).name;
    if (n.empty()) return {};
    ascii_lower_in_place(n);
    return "i_sa_" + n + ".tga";
#else
    return {};
#endif
}

#ifdef ALAMO_UI_HAVE_ABILITIES
void abilities_from_s9(const tactical::abilities::AbilitySystem& sys, const SelectedGroup& g, uint32_t frame,
                       std::vector<HudAbility>& out) {
    using tactical::abilities::kMaxAbilityType;
    for (uint32_t t = 1; t <= kMaxAbilityType; ++t) {
        const auto type = static_cast<uint8_t>(t);
        bool any = false;
        std::string alternate;  // t42: Alternate_Icon_Name of the first member's record (hud_abilities.hpp)
        HudAbility a;
        a.ability_type = type;
        a.recharge = 0.0f;
        a.enabled = false;
        for (const uint32_t uid : g.ids) {
            const auto id = static_cast<tactical::combat::ObjectId>(uid);
            if (!sys.has_ability(id, type)) continue;
            any = true;
            if (const tactical::abilities::UnitAbilityRecord* rec = alternate.empty() ? sys.record(id, type) : nullptr)
                alternate = rec->alternate_icon_name;
            const bool active = sys.is_active(id, type);
            a.active = a.active || active;
            a.autofire = a.autofire || sys.is_autofire(id, type);
            a.enabled = a.enabled || active || sys.can_activate(id, type, false, true, frame);
            float r = 1.0f;
            const int32_t counter = sys.countdown_counter(id, type), maximum = sys.countdown_maximum(id, type);
            if (sys.countdown_phase(id, type) == 0 && counter > 0 && maximum > 0)
                r = std::clamp(1.0f - static_cast<float>(counter) / static_cast<float>(maximum), 0.0f, 1.0f);
            a.recharge = std::max(a.recharge, r);
        }
        if (!any) continue;
        const TargetKind k = ability_target_kind(type);
        a.targeted = k != TargetKind::None;
        a.target_kind = static_cast<uint8_t>(k);
        a.icon = alternate.empty() ? ability_icon(type) : std::move(alternate);
        out.push_back(std::move(a));
    }
}

std::function<void(const SelectedGroup&, std::vector<HudAbility>&)> s9_ability_provider(
    const tactical::abilities::AbilitySystem& system, std::function<uint32_t()> frame) {
    return [&system, frame = std::move(frame)](const SelectedGroup& g, std::vector<HudAbility>& out) {
        abilities_from_s9(system, g, frame ? frame() : 0u, out);
    };
}
#endif

}  // namespace alamo::ui

#include "alamo/ui/hotkeys.hpp"

#include <algorithm>
#include <cstdio>

namespace alamo::ui {

namespace {

constexpr int32_t K(char c) { return static_cast<int32_t>(c); }
// Win32 virtual keys not in vk:: (UI.md §4.3 defaults).
constexpr int32_t kVkNumpad1 = 0x61, kVkNumpad2 = 0x62, kVkNumpad3 = 0x63, kVkNumpadSub = 0x6D, kVkF5 = 0x74,
                  kVkF6 = 0x75, kVkF12 = 0x7B, kVkScrollLock = 0x91, kVkOem1 = 0xBA /* ; */, kVkOemComma = 0xBC,
                  kVkOemMinus = 0xBD, kVkOemPeriod = 0xBE, kVkOem2 = 0xBF /* / */, kVkOem4 = 0xDB /* [ */,
                  kVkOem6 = 0xDD /* ] */, kVkOem7 = 0xDE /* ' */, kVkOemPlus = 0xBB /* = */, kVkOem5 = 0xDC /* backslash */,
                  kVkOem3 = 0xC0 /* ` */;

constexpr HotkeyMod N = HotkeyMod::None, S = HotkeyMod::Shift, C = HotkeyMod::Ctrl, A = HotkeyMod::Alt;
using HK = HotkeyKind;

constexpr HotkeySlotInfo kSlots[kHotkeySlots] = {
    // 0..9 select control group 1..9, 0
    {"SelectControlGroup1", HK::SelectGroup, {K('1'), N}}, {"SelectControlGroup2", HK::SelectGroup, {K('2'), N}},
    {"SelectControlGroup3", HK::SelectGroup, {K('3'), N}}, {"SelectControlGroup4", HK::SelectGroup, {K('4'), N}},
    {"SelectControlGroup5", HK::SelectGroup, {K('5'), N}}, {"SelectControlGroup6", HK::SelectGroup, {K('6'), N}},
    {"SelectControlGroup7", HK::SelectGroup, {K('7'), N}}, {"SelectControlGroup8", HK::SelectGroup, {K('8'), N}},
    {"SelectControlGroup9", HK::SelectGroup, {K('9'), N}}, {"SelectControlGroup0", HK::SelectGroup, {K('0'), N}},
    // 10..23 orders
    {"SelectAll", HK::Order, {K('Y'), C}} /* [port: Gabriel] Ctrl+A -> Ctrl+Y */, {"SelectLike", HK::Order, {K('Q'), C}}, {"NextUnit", HK::Order, {K('F'), N}},
    {"PreviousUnit", HK::Order, {K('V'), N}} /* [port: Gabriel] D -> V */, {"Guard", HK::Order, {K('G'), N}}, {"Attack", HK::Order, {K('X'), N}} /* [port: Gabriel] A -> X */,
    {"Stop", HK::Order, {K('H'), N}} /* [port: Gabriel] S -> H */, {"Move", HK::Order, {K('M'), N}}, {"Waypoint", HK::Order, {K('L'), N}} /* [port: Gabriel] W -> L */,
    {"Retreat", HK::Order, {K('E'), N}}, {"Reinforce", HK::Order, {K('R'), N}}, {"PlaceBeacon", HK::Order, {K('B'), N}},
    {"SuperWeapon", HK::Order, {kVkOem1, C}} /* [port: Gabriel] Ctrl+W -> Ctrl+; */, {"AttackMove", HK::Order, {K('T'), N}},
    // 24..45 land abilities
    {"Ability_Deploy", HK::Ability, {kVkOemComma, C}} /* [port: Gabriel] Ctrl+D -> Ctrl+, */, {"Ability_SpreadOut", HK::Ability, {K('Z'), C}},
    {"Ability_JetPack", HK::Ability, {K('J'), C}}, {"Ability_StickyBomb", HK::Ability, {K('K'), N}},
    {"Ability_UntargetedStickyBomb", HK::Ability, {K('K'), C}}, {"Ability_RocketAttack", HK::Ability, {K('G'), C}},
    {"Ability_CableAttack", HK::Ability, {K('F'), C}}, {"Ability_FowRevealPing", HK::Ability, {K('O'), C}},
    {"Ability_PowerToWeapons", HK::Ability, {K('B'), C}}, {"Ability_SelfDestruct", HK::Ability, {K('X'), C}},
    {"Ability_DeployTroopers", HK::Ability, {K('H'), C}}, {"Ability_MaximumFirepower", HK::Ability, {K('M'), C}},
    {"Ability_Sprint", HK::Ability, {K('N'), C}}, {"Ability_SystemControl", HK::Ability, {K('C'), C}},
    {"Ability_PlaceRemoteBomb", HK::Ability, {K('E'), C}}, {"Ability_DetonateRemoteBomb", HK::Ability, {K('R'), C}},
    {"Ability_FireLobbingSuperweapon", HK::Ability, {K('U'), C}}, {"Ability_LaserDefense", HK::Ability, {K('L'), C}},
    {"Ability_StimPack", HK::Ability, {kVkOemPeriod, C}} /* [port: Gabriel] Ctrl+S -> Ctrl+. */, {"Ability_SwapWeapons", HK::Ability, {K('I'), C}},
    {"Ability_MinorDrainLife", HK::Ability, {K('T'), C}}, {"Ability_ProximityMines", HK::Ability, {K('P'), C}},
    // 46..65 space abilities
    {"Ability_Defend", HK::Ability, {K('O'), S}}, {"Ability_TractorBeam", HK::Ability, {K('T'), S}},
    {"Ability_Interdict", HK::Ability, {K('G'), S}}, {"Ability_Barrage", HK::Ability, {K('B'), S}},
    {"Ability_Hunt", HK::Ability, {K('H'), S}}, {"Ability_Turbo", HK::Ability, {K('E'), S}},
    {"Ability_Lure", HK::Ability, {K('L'), S}}, {"Ability_MissileShield", HK::Ability, {K('M'), S}},
    {"Ability_SpoilerLock", HK::Ability, {K('Q'), S}} /* [port: Gabriel] Shift+W -> Shift+Q */, {"Ability_IonCannonShot", HK::Ability, {K('I'), S}},
    {"Ability_DestroyPlanet", HK::Ability, {K('R'), S}} /* [port: Gabriel] Shift+D -> Shift+R */, {"Ability_SensorJamming", HK::Ability, {K('J'), S}},
    {"Ability_Stealth", HK::Ability, {K('C'), S}}, {"Ability_LeechShields", HK::Ability, {K('N'), S}},
    {"Ability_BuzzDroids", HK::Ability, {K('Z'), S}}, {"Ability_SuperLaser", HK::Ability, {K('U'), S}} /* [port: Gabriel] Shift+A -> Shift+U */,
    {"Ability_ClusterBomb", HK::Ability, {K('K'), S}}, {"Ability_SpaceLaserDefense", HK::Ability, {K('P'), S}},
    {"Ability_SpaceSelfDestruct", HK::Ability, {kVkOem4, S}} /* [port: Gabriel] Shift+S -> Shift+[ */, {"Ability_FullSalvo", HK::Ability, {K('F'), S}},
    // 66..89
    {"DeploySquad", HK::Order, {K('Q'), N}}, {"DroidAdvisor", HK::Interface, {kVkNumpadSub, N}},
    {"InitiateChat", HK::Interface, {vk::kReturn, N}}, {"ScrollUp", HK::Camera, {K('W'), N}} /* [port: Gabriel] W A S D (were the arrows) */,
    {"ScrollDown", HK::Camera, {K('S'), N}}, {"ScrollLeft", HK::Camera, {K('A'), N}},
    {"ScrollRight", HK::Camera, {K('D'), N}}, {"CameraReset", HK::Camera, {vk::kEnd, N}},
    {"CameraCentre", HK::Camera, {vk::kPageUp, N}}, {"CameraHome", HK::Camera, {vk::kHome, N}},
    {"TacticalOverview", HK::Camera, {vk::kInsert, N}}, {"CameraTether", HK::Camera, {kVkScrollLock, N}},
    {"MovieMode", HK::Camera, {vk::kSpace, N}}, {"CinematicView", HK::Camera, {K('C'), N}},
    {"Screenshot", HK::Interface, {kVkF12, N}}, {"QuickSave", HK::Interface, {kVkF6, N}},
    {"QuickLoad", HK::Interface, {kVkF5, N}}, {"MissionHolocron", HK::Interface, {vk::kTab, N}},
    {"Taunt1", HK::Taunt, {kVkNumpad1, N}}, {"Taunt2", HK::Taunt, {kVkNumpad2, N}}, {"Taunt3", HK::Taunt, {kVkNumpad3, N}},
    {"GiveCredits", HK::Interface, {K('Y'), S}}, {"ToggleInterface", HK::Interface, {K('I'), N}},
    {"TogglePlayerList", HK::Interface, {K('P'), N}},
    // 90..122 FoC abilities (all Alt)
    {"Ability_LuckyShot", HK::Ability, {K('K'), A}}, {"Ability_ReplenishWingmen", HK::Ability, {kVkOem7, A}} /* [port: Gabriel] Alt+W -> Alt+' */,
    {"Ability_Telekinesis", HK::Ability, {K('H'), A}}, {"Ability_Whirlwind", HK::Ability, {K('P'), A}},
    {"Ability_Lightning", HK::Ability, {K('L'), A}}, {"Ability_AreaConvert", HK::Ability, {K('O'), A}},
    {"Ability_TargetedInvulnerability", HK::Ability, {K('Z'), A}}, {"Ability_CaptureVehicle", HK::Ability, {K('V'), A}},
    {"Ability_EjectVehicleThief", HK::Ability, {K('Y'), A}}, {"Ability_EnergyWeapon", HK::Ability, {K('X'), A}},
    {"Ability_HarmonicBomb", HK::Ability, {K('M'), A}}, {"Ability_WeakenEnemy", HK::Ability, {kVkOem4, A}},
    {"Ability_Invulnerability", HK::Ability, {kVkOem6, A}}, {"Ability_AreaStun", HK::Ability, {kVkOem2, A}},
    {"Ability_TargetedHack", HK::Ability, {kVkOemComma, A}}, {"Ability_TargetedRepair", HK::Ability, {kVkOemPeriod, A}},
    {"Ability_FlameThrower", HK::Ability, {K('J'), A}}, {"Ability_HomeOne", HK::Ability, {kVkOem1, A}},
    {"Ability_SaberThrow", HK::Ability, {K('T'), A}}, {"Ability_ForceCloak", HK::Ability, {K('C'), A}},
    {"Ability_ForceConfuse", HK::Ability, {K('U'), A}}, {"Ability_ForceSight", HK::Ability, {kVkOemPlus, A}} /* [port: Gabriel] Alt+S -> Alt+= */,
    {"Ability_Berserker", HK::Ability, {K('E'), A}}, {"Ability_RadioactiveContaminate", HK::Ability, {kVkOemMinus, A}},
    {"Ability_Infection", HK::Ability, {K('I'), A}}, {"Ability_DrainLife", HK::Ability, {kVkOem5, A}} /* [port: Gabriel] Alt+D -> Alt+backslash */,
    {"Ability_Summon", HK::Ability, {K('R'), A}}, {"Ability_Blast", HK::Ability, {K('G'), A}},
    {"Ability_ShieldFlare", HK::Ability, {K('F'), A}}, {"Ability_Stun", HK::Ability, {K('N'), A}},
    {"Ability_TacticalBribe", HK::Ability, {K('B'), A}}, {"Ability_FoCFullSalvo", HK::Ability, {kVkOem3, A}} /* [port: Gabriel] Alt+A -> Alt+` */,
    {"Ability_FoCSensorJamming", HK::Ability, {K('Q'), A}},
    // 123..132 assign group 1..0 (Ctrl)
    {"AssignControlGroup1", HK::AssignGroup, {K('1'), C}}, {"AssignControlGroup2", HK::AssignGroup, {K('2'), C}},
    {"AssignControlGroup3", HK::AssignGroup, {K('3'), C}}, {"AssignControlGroup4", HK::AssignGroup, {K('4'), C}},
    {"AssignControlGroup5", HK::AssignGroup, {K('5'), C}}, {"AssignControlGroup6", HK::AssignGroup, {K('6'), C}},
    {"AssignControlGroup7", HK::AssignGroup, {K('7'), C}}, {"AssignControlGroup8", HK::AssignGroup, {K('8'), C}},
    {"AssignControlGroup9", HK::AssignGroup, {K('9'), C}}, {"AssignControlGroup0", HK::AssignGroup, {K('0'), C}},
    // 133..142 add to group 1..0 (Shift)
    {"AddToControlGroup1", HK::AddToGroup, {K('1'), S}}, {"AddToControlGroup2", HK::AddToGroup, {K('2'), S}},
    {"AddToControlGroup3", HK::AddToGroup, {K('3'), S}}, {"AddToControlGroup4", HK::AddToGroup, {K('4'), S}},
    {"AddToControlGroup5", HK::AddToGroup, {K('5'), S}}, {"AddToControlGroup6", HK::AddToGroup, {K('6'), S}},
    {"AddToControlGroup7", HK::AddToGroup, {K('7'), S}}, {"AddToControlGroup8", HK::AddToGroup, {K('8'), S}},
    {"AddToControlGroup9", HK::AddToGroup, {K('9'), S}}, {"AddToControlGroup0", HK::AddToGroup, {K('0'), S}},
    // 143..152 third group action [M: Alt, camera jump]
    {"JumpToControlGroup1", HK::GroupThird, {K('1'), A}}, {"JumpToControlGroup2", HK::GroupThird, {K('2'), A}},
    {"JumpToControlGroup3", HK::GroupThird, {K('3'), A}}, {"JumpToControlGroup4", HK::GroupThird, {K('4'), A}},
    {"JumpToControlGroup5", HK::GroupThird, {K('5'), A}}, {"JumpToControlGroup6", HK::GroupThird, {K('6'), A}},
    {"JumpToControlGroup7", HK::GroupThird, {K('7'), A}}, {"JumpToControlGroup8", HK::GroupThird, {K('8'), A}},
    {"JumpToControlGroup9", HK::GroupThird, {K('9'), A}}, {"JumpToControlGroup0", HK::GroupThird, {K('0'), A}},
    // 153..159 [port: Gabriel] (docs/DECISIONS.md "Camera and controls"): pause, free tactical camera axes (the arrows
    // freed by W A S D, Numpad 4 / 6 for roll).
    {"Pause", HK::Interface, {vk::kPause, N}}, {"CameraYawLeft", HK::Camera, {vk::kLeft, N}},
    {"CameraYawRight", HK::Camera, {vk::kRight, N}}, {"CameraPitchUp", HK::Camera, {vk::kUp, N}},
    {"CameraPitchDown", HK::Camera, {vk::kDown, N}}, {"CameraRollLeft", HK::Camera, {vk::kNumpad4, N}},
    {"CameraRollRight", HK::Camera, {vk::kNumpad6, N}},
    // 160, 161 [port: Gabriel] keyboard zoom (was Home / End in ui::Bindings, which collided with CameraHome / CameraReset):
    // = and - on the main keyboard (Alt+= / Alt+- stay Force Sight / Radioactive Contaminate: exact modifiers).
    {"CameraZoomIn", HK::Camera, {kVkOemPlus, N}}, {"CameraZoomOut", HK::Camera, {kVkOemMinus, N}},
};

}  // namespace

std::span<const HotkeySlotInfo, kHotkeySlots> hotkey_slots() { return std::span<const HotkeySlotInfo, kHotkeySlots>(kSlots); }

int32_t hotkey_slot(std::string_view name) {
    for (uint16_t i = 0; i < kHotkeySlots; ++i)
        if (name == kSlots[i].name) return i;
    return -1;
}

std::string_view hotkey_ability_name(uint16_t slot) {
    struct Row {
        const char* slot;
        const char* ability;
    };
    static constexpr Row kRows[] = {
        {"Ability_Deploy", "DEPLOY"}, {"Ability_SpreadOut", "SPREAD_OUT"}, {"Ability_JetPack", "JET_PACK"},
        {"Ability_StickyBomb", "STICKY_BOMB"}, {"Ability_UntargetedStickyBomb", "UNTARGETED_STICKY_BOMB"},
        {"Ability_RocketAttack", "ROCKET_ATTACK"}, {"Ability_CableAttack", "CABLE_ATTACK"}, {"Ability_FowRevealPing", "FOW_REVEAL_PING"},
        {"Ability_PowerToWeapons", "POWER_TO_WEAPONS"}, {"Ability_SelfDestruct", "SELF_DESTRUCT"},
        {"Ability_DeployTroopers", "DEPLOY_TROOPERS"}, {"Ability_MaximumFirepower", "MAXIMUM_FIREPOWER"}, {"Ability_Sprint", "SPRINT"},
        {"Ability_PlaceRemoteBomb", "PLACE_REMOTE_BOMB"}, {"Ability_DetonateRemoteBomb", "DETONATE_REMOTE_BOMB"},
        {"Ability_FireLobbingSuperweapon", "FIRE_LOBBING_SUPERWEAPON"}, {"Ability_LaserDefense", "LASER_DEFENSE"},
        {"Ability_StimPack", "STIM_PACK"}, {"Ability_SwapWeapons", "SWAP_WEAPONS"}, {"Ability_MinorDrainLife", "DRAIN_LIFE"},
        {"Ability_ProximityMines", "PROXIMITY_MINES"}, {"Ability_Defend", "DEFEND"}, {"Ability_TractorBeam", "TRACTOR_BEAM"},
        {"Ability_Interdict", "INTERDICT"}, {"Ability_Barrage", "BARRAGE"}, {"Ability_Hunt", "HUNT"}, {"Ability_Turbo", "TURBO"},
        {"Ability_Lure", "LURE"}, {"Ability_MissileShield", "MISSILE_SHIELD"}, {"Ability_SpoilerLock", "SPOILER_LOCK"},
        {"Ability_IonCannonShot", "ION_CANNON_SHOT"}, {"Ability_SensorJamming", "SENSOR_JAMMING"}, {"Ability_Stealth", "STEALTH"},
        {"Ability_LeechShields", "LEECH_SHIELDS"}, {"Ability_BuzzDroids", "BUZZ_DROIDS"}, {"Ability_SuperLaser", "SUPER_LASER"},
        {"Ability_ClusterBomb", "CLUSTER_BOMB"}, {"Ability_SpaceLaserDefense", "LASER_DEFENSE"},
        {"Ability_SpaceSelfDestruct", "SELF_DESTRUCT"}, {"Ability_FullSalvo", "FULL_SALVO"}, {"DeploySquad", "DEPLOY_SQUAD"},
        {"Ability_LuckyShot", "LUCKY_SHOT"}, {"Ability_ReplenishWingmen", "REPLENISH_WINGMEN"},
        {"Ability_Telekinesis", "FORCE_TELEKINESIS"}, {"Ability_Whirlwind", "FORCE_WHIRLWIND"}, {"Ability_Lightning", "FORCE_LIGHTNING"},
        {"Ability_AreaConvert", "AREA_EFFECT_CONVERT"}, {"Ability_TargetedInvulnerability", "TARGETED_INVULNERABILITY"},
        {"Ability_CaptureVehicle", "CAPTURE_VEHICLE"}, {"Ability_EjectVehicleThief", "EJECT_VEHICLE_THIEF"},
        {"Ability_EnergyWeapon", "ENERGY_WEAPON"}, {"Ability_HarmonicBomb", "HARMONIC_BOMB"}, {"Ability_WeakenEnemy", "WEAKEN_ENEMY"},
        {"Ability_Invulnerability", "INVULNERABILITY"}, {"Ability_AreaStun", "AREA_EFFECT_STUN"},
        {"Ability_TargetedHack", "TARGETED_HACK"}, {"Ability_TargetedRepair", "TARGETED_REPAIR"}, {"Ability_FlameThrower", "FLAME_THROWER"},
        {"Ability_HomeOne", "CONCENTRATE_FIRE"}, {"Ability_SaberThrow", "SABER_THROW"}, {"Ability_ForceCloak", "FORCE_CLOAK"},
        {"Ability_ForceConfuse", "FORCE_CONFUSE"}, {"Ability_ForceSight", "FORCE_SIGHT"}, {"Ability_Berserker", "BERSERKER"},
        {"Ability_RadioactiveContaminate", "RADIOACTIVE_CONTAMINATE"}, {"Ability_Infection", "INFECTION"},
        {"Ability_DrainLife", "DRAIN_LIFE"}, {"Ability_Summon", "SUMMON"}, {"Ability_Blast", "BLAST"},
        {"Ability_ShieldFlare", "SHIELD_FLARE"}, {"Ability_Stun", "STUN"}, {"Ability_TacticalBribe", "TACTICAL_BRIBE"},
        {"Ability_FoCFullSalvo", "FULL_SALVO"}, {"Ability_FoCSensorJamming", "SENSOR_JAMMING"},
    };
    if (slot >= kHotkeySlots) return {};
    const std::string_view name = kSlots[slot].name;
    for (const Row& r : kRows)
        if (name == r.slot) return r.ability;
    return {};
}

int8_t hotkey_group(uint16_t slot) {
    uint16_t n = 0xFFFF;
    if (slot < 10) n = slot;
    else if (slot >= hk::kAssignBase && slot < hk::kAssignBase + 30) n = static_cast<uint16_t>((slot - hk::kAssignBase) % 10);
    if (n == 0xFFFF) return -1;
    return static_cast<int8_t>(n == 9 ? 0 : n + 1);  // slot 0 = key '1' = group 1 ... slot 9 = key '0' = group 0
}

void HotkeyTable::reset_defaults() {
    for (uint16_t i = 0; i < kHotkeySlots; ++i) slots_[i] = kSlots[i].def;
}

void HotkeyTable::bind(uint16_t slot, HotkeyBinding b) {
    if (slot >= kHotkeySlots) return;
    auto set = [&](uint16_t s, HotkeyBinding v) {
        if (v.key >= 0)
            for (uint16_t o = 0; o < kHotkeySlots; ++o)
                if (o != s && slots_[o] == v) slots_[o] = HotkeyBinding{};
        slots_[s] = v;
    };
    set(slot, b);
    if (slot < 10) {  // derived slots follow the recall key
        const HotkeyMod mods[3] = {HotkeyMod::Ctrl, HotkeyMod::Shift, HotkeyMod::Alt};
        const uint16_t bases[3] = {hk::kAssignBase, hk::kAddBase, hk::kThirdBase};
        for (int k = 0; k < 3; ++k)
            set(static_cast<uint16_t>(bases[k] + slot), b.key < 0 ? HotkeyBinding{} : HotkeyBinding{b.key, mods[k]});
    }
}

void HotkeyTable::save(HotkeyProfile& out) const {
    out["KeySettingsCount"] = kHotkeySlots;
    char name[64];
    for (uint16_t i = 0; i < kHotkeySlots; ++i) {
        std::snprintf(name, sizeof name, "KeySettingKeycode%u", static_cast<unsigned>(i));
        out[name] = slots_[i].key;
        std::snprintf(name, sizeof name, "KeySettingModifier%u", static_cast<unsigned>(i));
        out[name] = static_cast<int32_t>(slots_[i].mod);
    }
}

bool HotkeyTable::load(const HotkeyProfile& in) {
    const auto cnt = in.find("KeySettingsCount");
    if (cnt == in.end()) return false;
    const int32_t n = std::clamp<int32_t>(cnt->second, 0, kHotkeySlots);
    char name[64];
    for (int32_t i = 0; i < n; ++i) {
        std::snprintf(name, sizeof name, "KeySettingKeycode%d", i);
        const auto k = in.find(name);
        if (k == in.end()) continue;
        std::snprintf(name, sizeof name, "KeySettingModifier%d", i);
        const auto m = in.find(name);
        HotkeyBinding b;
        b.key = k->second >= 0 && k->second < 256 ? k->second : -1;
        const int32_t mv = m == in.end() ? 0 : m->second;
        b.mod = mv >= 0 && mv <= 3 ? static_cast<HotkeyMod>(mv) : HotkeyMod::None;
        slots_[static_cast<size_t>(i)] = b;
    }
    // [port: Gabriel] an original-format profile (<= 153 slots): W A S D are for camera movement only.
    //  1. scroll slots still on the original default arrows become the new W / S / A / D defaults;
    //  2. any other slot on W / A / S / D (any modifier) takes its port default when that is free, else it is unbound.
    if (n <= static_cast<int32_t>(kOriginalHotkeySlots)) {
        const uint16_t scroll[4] = {hk::kScrollUp, hk::kScrollDown, hk::kScrollLeft, hk::kScrollRight};
        const int32_t arrows[4] = {vk::kUp, vk::kDown, vk::kLeft, vk::kRight};
        for (int k = 0; k < 4; ++k)
            if (slots_[scroll[k]] == (HotkeyBinding{arrows[k], HotkeyMod::None})) slots_[scroll[k]] = kSlots[scroll[k]].def;
        auto wasd = [](const HotkeyBinding& b) { return b.key == 'W' || b.key == 'A' || b.key == 'S' || b.key == 'D'; };
        for (uint16_t o = 0; o < kOriginalHotkeySlots; ++o) {
            if (o == hk::kScrollUp || o == hk::kScrollDown || o == hk::kScrollLeft || o == hk::kScrollRight || !wasd(slots_[o])) continue;
            const HotkeyBinding def = kSlots[o].def;
            bool taken = wasd(def);
            for (uint16_t x = 0; x < kHotkeySlots && !taken; ++x) taken = x != o && slots_[x] == def;
            slots_[o] = taken ? HotkeyBinding{} : def;
        }
    }
    // [port: Gabriel] a port slot whose default collides with a loaded binding (an original 153-slot profile keeps the
    // arrows on scrolling) is unbound rather than shadowing it.
    for (uint16_t p = kOriginalHotkeySlots; p < kHotkeySlots; ++p) {
        if (p < static_cast<uint16_t>(n) || slots_[p].key < 0) continue;
        for (uint16_t o = 0; o < kOriginalHotkeySlots; ++o)
            if (slots_[o] == slots_[p]) {
                slots_[p] = HotkeyBinding{};
                break;
            }
    }
    return true;
}

namespace {
// `0x2127f0` (P1_CAMERA_INPUT.md §6): modifier 0 matches only with Shift, Ctrl and Alt all up; 1 / 2 / 3 when Shift /
// Ctrl / Alt is held (the others are ignored).
bool matches(const InputState& in, const HotkeyBinding& b) {
    if (b.key < 0 || !in.key_pressed(static_cast<uint32_t>(b.key))) return false;
    switch (b.mod) {
        case HotkeyMod::None: return !in.shift() && !in.ctrl() && !in.alt();
        case HotkeyMod::Shift: return in.shift();
        case HotkeyMod::Ctrl: return in.ctrl();
        case HotkeyMod::Alt: return in.alt();
    }
    return false;
}
}  // namespace

// The first matching slot in index order wins its key.
bool HotkeyTable::pressed(const InputState& in, uint16_t slot) const {
    if (slot >= kHotkeySlots || !matches(in, slots_[slot])) return false;
    for (uint16_t i = 0; i < slot; ++i)
        if (slots_[i].key == slots_[slot].key && matches(in, slots_[i])) return false;
    return true;
}

void HotkeyTable::pressed(const InputState& in, std::vector<uint16_t>& out) const {
    out.clear();
    std::array<bool, 256> taken{};
    for (uint16_t i = 0; i < kHotkeySlots; ++i) {
        if (!matches(in, slots_[i])) continue;
        const auto k = static_cast<size_t>(slots_[i].key);
        if (taken[k]) continue;
        taken[k] = true;
        out.push_back(i);
    }
}

void HotkeyTable::apply_to_bindings(Bindings& b) const {
    auto kb = [](const HotkeyBinding& h, bool any) {
        KeyBinding k;
        if (h.key < 0) return k;
        k.key = static_cast<uint16_t>(h.key);
        if (any) {
            k.mods = kModAny;
        } else {
            k.mods = h.mod == HotkeyMod::Shift ? kModShift : h.mod == HotkeyMod::Ctrl ? kModCtrl : h.mod == HotkeyMod::Alt ? kModAlt : kModNone;
        }
        return k;
    };
    b.clear(Action::ResetCamera);
    b.bind(Action::ResetCamera, 0, kb(slots_[hk::kCameraReset], true));
    // [port: Gabriel] camera movement (W A S D by default) matches whatever modifier is held (W A S D are used for nothing
    // else); the free camera axes and pause keep their exact modifier.
    const std::pair<Action, uint16_t> any[] = {{Action::ScrollUp, hk::kScrollUp},     {Action::ScrollDown, hk::kScrollDown},
                                               {Action::ScrollLeft, hk::kScrollLeft}, {Action::ScrollRight, hk::kScrollRight}};
    for (const auto& [a, s] : any) {
        b.clear(a);
        b.bind(a, 0, kb(slots_[s], true));
    }
    const std::pair<Action, uint16_t> exact[] = {{Action::RotateLeft, hk::kYawLeft}, {Action::RotateRight, hk::kYawRight},
                                                 {Action::PitchUp, hk::kPitchUp},    {Action::PitchDown, hk::kPitchDown},
                                                 {Action::RollLeft, hk::kRollLeft},  {Action::RollRight, hk::kRollRight},
                                                 {Action::Pause, hk::kPause},        {Action::ZoomIn, hk::kZoomIn},
                                                 {Action::ZoomOut, hk::kZoomOut}};
    for (const auto& [a, s] : exact) {
        b.clear(a);
        b.bind(a, 0, kb(slots_[s], false));
    }
    b.clear(Action::SelectAll);
    b.bind(Action::SelectAll, 0, kb(slots_[hk::kSelectAll], false));
    b.clear(Action::Stop);
    b.bind(Action::Stop, 0, kb(slots_[hk::kStop], false));
    for (uint16_t s = 0; s < 10; ++s) {
        const int8_t g = hotkey_group(s);
        const auto a = static_cast<Action>(static_cast<uint8_t>(Action::ControlGroup0) + static_cast<uint8_t>(g));
        b.clear(a);
        b.bind(a, 0, kb(slots_[s], true));  // OrderTranslator reads Ctrl / Shift itself
    }
}

void HotkeyDispatcher::update(const InputState& in, const HotkeyTable& table, std::span<const uint32_t> selection,
                              double now, std::vector<commands::EventData>& cmds, std::vector<HotkeyEvent>& events) {
    table.pressed(in, fired_);
    const auto slots = hotkey_slots();
    for (uint16_t s : fired_) {
        HotkeyEvent e;
        e.slot = s;
        e.kind = slots[s].kind;
        e.group = hotkey_group(s);
        switch (e.kind) {
            case HotkeyKind::SelectGroup: {
                const auto g = static_cast<uint8_t>(e.group);
                cmds.push_back(commands::ev::control_group(g, false));
                e.camera_jump = last_group_ == e.group && now - last_recall_ <= kDoubleRecallSeconds;
                last_group_ = e.group;
                last_recall_ = now;
                if (e.camera_jump) events.push_back(e);
                continue;
            }
            case HotkeyKind::AssignGroup:
                cmds.push_back(commands::ev::control_group(static_cast<uint8_t>(e.group), true, false));
                continue;
            case HotkeyKind::AddToGroup:
                cmds.push_back(commands::ev::control_group(static_cast<uint8_t>(e.group), true, true));
                continue;
            case HotkeyKind::GroupThird:
                e.camera_jump = true;
                events.push_back(e);
                continue;
            default:
                break;
        }
        if (s == hk::kSelectAll) {
            cmds.push_back(commands::ev::select_all());
            continue;
        }
        if (s == hk::kStop) {
            if (!selection.empty()) cmds.push_back(commands::ev::stop());
            continue;
        }
        if (s == hk::kPause) {  // [port: Gabriel] pause / resume through the command stream
            cmds.push_back(pause_event(!paused_));
            paused_ = !paused_;
            continue;
        }
        if (s == hk::kRetreat) {
            cmds.push_back(commands::ev::retreat(true));
            continue;
        }
        events.push_back(e);
    }
}

std::vector<uint16_t> apply_camera_hotkeys(std::span<const HotkeyEvent> events, TacticalCameraController& cam,
                                           const CameraFocus& focus) {
    std::vector<uint16_t> handled;
    for (const HotkeyEvent& e : events) {
        std::optional<Vec3> p;
        bool done = true;
        if (e.camera_jump && e.group >= 0) {
            if (focus.group) p = focus.group(e.group);
        } else if (e.slot == hk::kTacticalOverview) {
            cam.toggle_overview();
        } else if (e.slot == hk::kCameraReset) {
            cam.reset();
        } else if (e.slot == hk::kCameraCentre) {
            if (focus.selection) p = focus.selection();
        } else if (e.slot == hk::kCameraHome) {
            if (focus.home) p = focus.home();
        } else {
            done = false;
        }
        if (p) cam.jump_to(*p);
        if (done) handled.push_back(e.slot);
    }
    return handled;
}

}  // namespace alamo::ui

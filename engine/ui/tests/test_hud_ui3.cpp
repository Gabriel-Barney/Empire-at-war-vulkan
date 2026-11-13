// alamo_ui_hud_ui3_tests (ui3, UIART "Tactical HUD controls"): the hotkey slot -> ability mapping and the keyboard
// dialog's edit-box -> slot mapping (UI.md §4.3: the retail edit-name table is in slot order), the ability hotkey as a
// click on the matching ability button, the order-button enable query, the b_retreat / b_reinforcement / beacon / movie
// mode state and the reinforcement texts (HUD_GAPS §5.1), the land Tooltip_Text entry 1 and the ability tooltip keys
// (HUD_GAPS §2.1), and the objectives box toggle.
#include <cstdio>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/data/xml.hpp"
#include "alamo/test/support.hpp"
#include "alamo/ui/gui_menus.hpp"
#include "alamo/ui/hotkeys.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/orders.hpp"
#include "alamo/ui/tactical_hud.hpp"
#include "hud_test_data.hpp"
#ifdef ALAMO_UI_HAVE_ABILITIES
#include "alamo/tactical/abilities/ability_types.hpp"
#endif

using namespace alamo;
using namespace alamo::ui;
using ::alamo::test::g_fail;

namespace {

// The keyboard dialogs' IDC_EDIT_* names in the order of the retail name table (strings 0x8063d0 .. 0x807310, stride
// 0x20; docs/re/ghidra/strings_xrefs.csv): entry i is hotkey slot i.
constexpr const char* kRetailEditOrder[] = {
    "IDC_EDIT_GROUP_1_SELECT", "IDC_EDIT_GROUP_2_SELECT", "IDC_EDIT_GROUP_3_SELECT", "IDC_EDIT_GROUP_4_SELECT",
    "IDC_EDIT_GROUP_5_SELECT", "IDC_EDIT_GROUP_6_SELECT", "IDC_EDIT_GROUP_7_SELECT", "IDC_EDIT_GROUP_8_SELECT",
    "IDC_EDIT_GROUP_9_SELECT", "IDC_EDIT_GROUP_0_SELECT", "IDC_EDIT_SELECT_ALL", "IDC_EDIT_SELECT_LIKE",
    "IDC_EDIT_NEXT_UNIT", "IDC_EDIT_PREV_UNIT", "IDC_EDIT_GUARD", "IDC_EDIT_ATTACK", "IDC_EDIT_STOP",
    "IDC_EDIT_MOVE", "IDC_EDIT_WAYPOINT", "IDC_EDIT_RETREAT", "IDC_EDIT_REINFORCE", "IDC_EDIT_PLACE_BEACON",
    "IDC_EDIT_SUPER_WEAPON", "IDC_EDIT_ATTACK_MOVE", "IDC_EDIT_DEPLOY", "IDC_EDIT_SPREAD_OUT",
    "IDC_EDIT_JET_PACK", "IDC_EDIT_THERMAL_DETONATOR", "IDC_EDIT_THERMAL_DETONATOR_DROP",
    "IDC_EDIT_ROCKET_ATTACK", "IDC_EDIT_CABLE_ATTACK", "IDC_EDIT_SENSOR_PING", "IDC_EDIT_BOOST_WEAPON_POWER",
    "IDC_EDIT_SELF_DESTRUCT", "IDC_EDIT_DEPLOY_STORMTROOPERS", "IDC_EDIT_MAXIMUM_FIREPOWER", "IDC_EDIT_SPRINT",
    "IDC_EDIT_SYSTEM_CONTROL", "IDC_EDIT_REMOTE_EXPLOSIVE", "IDC_EDIT_DETONATE_REMOTE",
    "IDC_EDIT_MAGNEPULSE_SHOT", "IDC_EDIT_LASER_DEFENSE", "IDC_EDIT_STIM_PACK", "IDC_EDIT_SWAP_WEAPONS",
    "IDC_EDIT_MINOR_LIFE_DRAIN", "IDC_EDIT_PROXIMITY_MINES", "IDC_EDIT_DEFEND", "IDC_EDIT_TRACTOR_BEAM",
    "IDC_EDIT_GRAVITY_WELL", "IDC_EDIT_BARRAGE_AREA", "IDC_EDIT_HUNT_ENEMIES", "IDC_EDIT_BOOST_ENGINE_POWER",
    "IDC_EDIT_LURE_FIGHTERS", "IDC_EDIT_MISSILE_JAMMING_FIELD", "IDC_EDIT_LOCK_WINGS", "IDC_EDIT_ION_CANON_SHOT",
    "IDC_EDIT_DESTROY_PLANET", "IDC_EDIT_SENSOR_JAMMING", "IDC_EDIT_STEALTH", "IDC_EDIT_LEECH_SHIELDS",
    "IDC_EDIT_BUZZ_DROIDS", "IDC_EDIT_SUPER_LASER", "IDC_EDIT_CLUSTER_BOMB", "IDC_EDIT_SPACE_LASER_DEFENSE",
    "IDC_EDIT_SPACE_SELF_DESTRUCT", "IDC_EDIT_FULL_SALVO", "IDC_EDIT_DEPLOY_SQUADRON", "IDC_EDIT_DROID_ADVISOR",
    "IDC_EDIT_INITIATE_CHAT", "IDC_EDIT_SCROLL_UP", "IDC_EDIT_SCROLL_DOWN", "IDC_EDIT_SCROLL_LEFT",
    "IDC_EDIT_SCROLL_RIGHT", "IDC_EDIT_CAMERA_RESET", "IDC_EDIT_CAMERA_CENTER", "IDC_EDIT_CAMERA_HOME",
    "IDC_EDIT_TACTICAL_OVERVIEW", "IDC_EDIT_TETHER_CAMERA", "IDC_EDIT_BATTLE_CAM", "IDC_EDIT_CINEMATIC_PREVIEW",
    "IDC_EDIT_SCREEN_SHOT", "IDC_EDIT_QUICK_SAVE", "IDC_EDIT_QUICK_LOAD", "IDC_EDIT_MISSION_HOLOCRON",
    "IDC_EDIT_TAUNT_ONE", "IDC_EDIT_TAUNT_TWO", "IDC_EDIT_TAUNT_THREE", "IDC_EDIT_GIVE_CREDITS",
    "IDC_EDIT_TOGGLE_INTERFACE", "IDC_EDIT_TOGGLE_PLAYER_LIST", "IDC_EDIT_LUCKY_SHOT", "IDC_EDIT_CALL_WINGMEN",
    "IDC_EDIT_FORCE_CRUSH", "IDC_EDIT_FORCE_PUSH", "IDC_EDIT_FORCE_LIGHTNING", "IDC_EDIT_FORCE_CORRUPT",
    "IDC_EDIT_FORCE_PROTECT", "IDC_EDIT_CAPTURE_VEHICLE", "IDC_EDIT_EJECT_VEHICLE_THIEF", "IDC_EDIT_PROTON_BEAM",
    "IDC_EDIT_SEISMIC_CHARGE", "IDC_EDIT_WEAKEN", "IDC_EDIT_INVULNERABILITY", "IDC_EDIT_AOE_STUN",
    "IDC_EDIT_COMMAND_TURRET", "IDC_EDIT_REPAIR_VEHICLE", "IDC_EDIT_FLAME_THROWER",
    "IDC_EDIT_REDIRECT_FIREPOWER", "IDC_EDIT_LIGHTSABER_THROW", "IDC_EDIT_HERO_CLOAK", "IDC_EDIT_FORCE_CONFUSE",
    "IDC_EDIT_FORCE_SIGHT", "IDC_EDIT_BERSERKER", "IDC_EDIT_CONTAMINATE", "IDC_EDIT_INFECTION",
    "IDC_EDIT_DRAIN_LIFE", "IDC_EDIT_SUMMON_RANCOR", "IDC_EDIT_BLAST", "IDC_EDIT_SHIELD_FLARE",
    "IDC_EDIT_SONIC_STUN", "IDC_EDIT_BRIBE", "IDC_EDIT_ASSAULT", "IDC_EDIT_SENSOR_JAM",
};

void test_edit_box_order() {
    CHECK(std::size(kRetailEditOrder) == 123);
    for (size_t i = 0; i < std::size(kRetailEditOrder); ++i) {
        const int32_t s = keyboard_edit_slot(kRetailEditOrder[i]);
        if (s != static_cast<int32_t>(i)) std::printf("  edit box %s -> slot %d, retail order %zu\n", kRetailEditOrder[i], s, i);
        CHECK(s == static_cast<int32_t>(i));
    }
}

void test_ability_slots() {
    const auto slots = hotkey_slots();
    uint32_t mapped = 0;
    for (uint16_t s = 0; s < kHotkeySlots; ++s) {
        const std::string_view n = hotkey_ability_name(s);
        if (slots[s].kind != HotkeyKind::Ability && s != hk::kDeploySquad) {
            CHECK(n.empty());
            continue;
        }
        const std::string_view slot = slots[s].name;
        if (slot == "Ability_SystemControl" || slot == "Ability_DestroyPlanet") {
            CHECK(n.empty());
            continue;
        }
        CHECK(!n.empty());
        ++mapped;
#ifdef ALAMO_UI_HAVE_ABILITIES
        const uint8_t type = tactical::abilities::ability_type_from_name(n);
        if (type == 0) std::printf("  slot %s: no ability type %.*s\n", slots[s].name, static_cast<int>(n.size()), n.data());
        CHECK(type != 0);
#endif
    }
    CHECK(mapped == 74);  // 42 + 33 ability slots, minus SystemControl / DestroyPlanet, plus DeploySquad
    CHECK(hotkey_ability_name(hk::kDeploySquad) == "DEPLOY_SQUAD");
    CHECK(hotkey_ability_name(static_cast<uint16_t>(hotkey_slot("Ability_HomeOne"))) == "CONCENTRATE_FIRE");
    CHECK(hotkey_ability_name(hk::kAttack).empty() && hotkey_ability_name(kHotkeySlots).empty());
}

const char* kUi3Xml = R"(<?xml version="1.0"?>
<CommandBarComponents>
  <CommandBarComponent Name="s_select_00"><Type>TextButton</Type><Base_Layer>4</Base_Layer>
    <Blank_Texture_Name>i_button_pad_rebel.tga i_button_pad_empire.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="special_button_00"><Type>TextButton</Type><Base_Layer>4</Base_Layer>
    <Tooltip_Text>TEXT_GENERIC_ABILITY</Tooltip_Text></CommandBarComponent>
  <CommandBarComponent Name="special_button_01"><Type>TextButton</Type><Base_Layer>4</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="c_button00"><Type>Button</Type><Tooltip_Text>TEXT_TOOLTIP_ATTACK TEXT_TOOLTIP_ATTACK_01</Tooltip_Text></CommandBarComponent>
  <CommandBarComponent Name="c_button01"><Type>Button</Type></CommandBarComponent>
  <CommandBarComponent Name="c_button02"><Type>Button</Type></CommandBarComponent>
  <CommandBarComponent Name="c_button03"><Type>Button</Type></CommandBarComponent>
  <CommandBarComponent Name="c_button04"><Type>Button</Type></CommandBarComponent>
  <CommandBarComponent Name="c_button05"><Type>Button</Type></CommandBarComponent>
  <CommandBarComponent Name="b_retreat"><Type>Button</Type><Build_Texture_Name>i_button_build.tga</Build_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="b_reinforcement"><Type>Button</Type><Disabled>True</Disabled></CommandBarComponent>
  <CommandBarComponent Name="b_special_weapon2"><Type>Button</Type><Disabled>True</Disabled></CommandBarComponent>
  <CommandBarComponent Name="b_beacon_t"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="b_camera_t"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="reinforcement_counter"><Type>TextButton</Type></CommandBarComponent>
  <CommandBarComponent Name="r_pop_icon"><Type>Button</Type></CommandBarComponent>
  <CommandBarComponent Name="r_pop_text"><Type>TextButton</Type></CommandBarComponent>
</CommandBarComponents>)";

void test_tactical_controls() {
    ComponentTable t;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kUi3Xml), "ui3.xml");
    CHECK(doc.ok());
    t.parse(doc.root());
    const TextureAtlas atlas = hud_test::make_atlas();
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    TacticalHud hud;
    hud.init(t, atlas, fonts);
    hud.resize(1280, 720);
    std::vector<HudTypeInfo> types(1);
    std::vector<HudUnit> units(2);
    for (uint32_t i = 0; i < 2; ++i) {
        units[i].id = 40 + i;
        units[i].owner = 0;
        units[i].selected = true;
        units[i].position = {static_cast<float>(i) * 50.0f, 0.0f, 0.0f};
    }
    bool disabled = false;
    TacticalHudFrame fr;
    fr.providers.abilities = [&disabled](const SelectedGroup&, std::vector<HudAbility>& out) {
        HudAbility a;
        a.ability_type = 7;
        a.icon = "i_icon_unit.tga";
        a.tooltip_key = "TEXT_TOOLTIP_ABILITY_SPREAD_OUT_NAME TEXT_TOOLTIP_ABILITY_SPREAD_OUT_DESCRIPTION";
        a.enabled = !disabled;
        out.push_back(a);
        HudAbility b;
        b.ability_type = 12;
        b.targeted = true;
        b.icon = "i_icon_unit.tga";
        out.push_back(b);
    };
    fr.order_buttons.can_move = true;
    hud.build(fr, units, types);
    auto state = [&](const char* n) { return hud.buttons().find_state(static_cast<uint32_t>(t.find(n))); };
    // t02: an ability hotkey = a left click on the button of that type.
    CHECK(hud.stats().ability_buttons == 2);
    {
        std::vector<commands::EventData> cmds;
        std::vector<HudAction> acts;
        CHECK(hud.activate_ability(7, false, cmds, &acts));
        const uint32_t users[2] = {40, 41};
        CHECK(cmds.size() == 1 && acts.empty() && !cmds.empty() && cmds[0] == ability_mode_event(users, 7, true));
        cmds.clear();
        CHECK(hud.activate_ability(12, true, cmds, &acts));
        CHECK(cmds.empty() && acts.size() == 1 && !acts.empty() && acts[0].kind == HudAction::Kind::BeginAbilityTargeting &&
              acts[0].ability_type == 12 && acts[0].queued && acts[0].name == "special_button_01");
        acts.clear();
        CHECK(!hud.activate_ability(9, false, cmds, &acts) && cmds.empty() && acts.empty());
        disabled = true;
        hud.build(fr, units, types);
        CHECK(!hud.activate_ability(7, false, cmds, &acts) && cmds.empty());
        disabled = false;
    }
    // t07: the ability's own tooltip keys; space -> entry 0, land -> entry 1 of a two-entry Tooltip_Text.
    hud.build(fr, units, types);
    CHECK(state("special_button_00") && state("special_button_00")->tooltip_key.rfind("TEXT_TOOLTIP_ABILITY_SPREAD_OUT_NAME", 0) == 0);
    CHECK(state("c_button00") && state("c_button00")->tooltip_index == 0);
    fr.world.land = true;
    hud.build(fr, units, types);
    CHECK(state("c_button00") && state("c_button00")->tooltip_index == 1);
    // t01: the order hotkeys follow the buttons (can move, cannot attack).
    CHECK(!hud.order_button_live(0) && hud.order_button_live(2) && hud.order_button_live(4) && !hud.order_button_live(6));
    // t03 / t04: the command-bar buttons and the reinforcement texts from the feed.
    fr.command_buttons.present = true;
    fr.command_buttons.retreat_enabled = false;
    fr.command_buttons.retreat_dial = 0.25f;
    fr.command_buttons.reinforce_enabled = true;
    fr.command_buttons.reinforce_selected = true;
    fr.command_buttons.beacon_enabled = true;
    fr.command_buttons.movie_mode = true;
    fr.command_buttons.reinforce_count = 3;
    fr.command_buttons.pop_used = 12;
    fr.command_buttons.pop_cap = 20;
    hud.build(fr, units, types);
    const ButtonRuntime* retreat = state("b_retreat");
    const ButtonRuntime* reinforce = state("b_reinforcement");
    const ButtonRuntime* beacon = state("b_special_weapon2");
    const ButtonRuntime* beacon_t = state("b_beacon_t");
    const ButtonRuntime* camera = state("b_camera_t");
    const ButtonRuntime* counter = state("reinforcement_counter");
    const ButtonRuntime* pop = state("r_pop_text");
    const ButtonRuntime* pop_icon = state("r_pop_icon");
    CHECK(retreat && reinforce && beacon && beacon_t && camera && counter && pop && pop_icon);
    if (!(retreat && reinforce && beacon && beacon_t && camera && counter && pop && pop_icon)) return;
    CHECK(retreat->disabled && retreat->build_fill == 25.0f);
    CHECK(!reinforce->disabled && reinforce->selected);
    CHECK(!beacon->disabled && !beacon_t->disabled && camera->selected);
    CHECK(!counter->hidden && counter->text == U"3");
    CHECK(!pop->hidden && pop->text == U"12/20" && !pop_icon->hidden);
    fr.command_buttons.retreat_enabled = true;
    fr.command_buttons.retreat_dial = -1.0f;
    fr.command_buttons.reinforce_enabled = false;
    fr.command_buttons.reinforce_count = 0;
    fr.command_buttons.pop_cap = -1;  // land: no cap
    hud.build(fr, units, types);
    CHECK(!retreat->disabled && retreat->build_fill < 0.0f && reinforce->disabled);
    CHECK(counter->hidden && pop->hidden && pop_icon->hidden);
    fr.command_buttons.controls_locked = true;  // story LOCK_CONTROLS: all disabled
    hud.build(fr, units, types);
    CHECK(retreat->disabled && beacon->disabled);
    // t01: the Mission Holocron key hides the objectives box.
    HudObjective o;
    o.text = U"Destroy all enemy space forces to win.";
    const std::vector<HudObjective> objs{o};
    fr.objectives = objs;
    hud.build(fr, units, types);
    const uint32_t shown = hud.stats().objectives;
    fr.objectives_hidden = true;
    hud.build(fr, units, types);
    CHECK(shown == 1 && hud.stats().objectives == 0);
}

}  // namespace

int main() {
    test_edit_box_order();
    test_ability_slots();
    test_tactical_controls();
    if (g_fail) {
        std::printf("alamo_ui_hud_ui3_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_hud_ui3_tests: all passed\n");
    return 0;
}

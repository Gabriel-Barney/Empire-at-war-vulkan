// alamo_ui_hud_tests, P2 stage 2 part (CPU): shell .alo bones / placeholder rects / _ALT faction meshes (synthetic
// model, and the real i_tactical_controls.alo when ALAMO_FOC_DATA is set), the 153-slot hotkey table + dispatch,
// Max_Text_Width wrapping, button states / SFX hooks / tooltips, radar settings / image / events / mapping,
// waypoint lines / move acknowledgements / hard-point reticles, and the TacticalHud stage-2 panels + actions.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <span>
#include <utility>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "alamo/data/xml.hpp"
#include "alamo/ui/camera.hpp"
#include "alamo/ui/hotkeys.hpp"
#include "alamo/ui/hud_buttons.hpp"
#include "alamo/ui/hud_font.hpp"
#include "alamo/ui/hud_markers.hpp"
#include "alamo/ui/hud_radar.hpp"
#include "alamo/ui/gui_script.hpp"
#include "alamo/ui/orders.hpp"
#include "alamo/ui/picking.hpp"
#include "alamo/ui/tactical_hud.hpp"
#include "hud_test_data.hpp"
#ifdef ALAMO_UI_HAVE_ASSETS
#include "alamo/assets/model.hpp"
#include "alamo/ui/hud_shell.hpp"
#endif
#ifdef ALAMO_UI_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::ui;

namespace {

int g_fail2 = 0;
#define CHECK2(c)                                                    \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c); \
            ++g_fail2;                                               \
        }                                                            \
    } while (0)

bool near2(float a, float b, float eps = 1e-3f) { return std::fabs(a - b) <= eps; }

View top_view2(uint32_t w, uint32_t h, float height = 1000.0f) {
    CameraParams p;
    p.eye = {0, -0.001f, height};
    p.target = {0, 0, 0};
    p.fov_x = 0.9599311f;
    p.znear = 10.0f;
    p.zfar = 7000.0f;
    return make_view(p, w, h);
}

const char* kStage2Xml = R"(<?xml version="1.0"?>
<CommandBarComponents>
  <CommandBarComponent Name="i_main_skirmish"><Model_Name>i_tactical_controls.alo</Model_Name><Type>Shell</Type>
    <Model_Offset_X>True</Model_Offset_X><Model_Offset_Y>True</Model_Offset_Y><Hidden>True</Hidden></CommandBarComponent>
  <CommandBarComponent Name="s_select_00"><Type>TextButton</Type><Base_Layer>4</Base_Layer>
    <Blank_Texture_Name>i_button_pad_rebel.tga i_button_pad_empire.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="s_select_01"><Type>TextButton</Type><Base_Layer>4</Base_Layer>
    <Blank_Texture_Name>i_button_pad_rebel.tga i_button_pad_empire.tga</Blank_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="special_button_00"><Type>TextButton</Type><Base_Layer>4</Base_Layer>
    <Blank_Texture_Name>i_special_border_button.tga</Blank_Texture_Name><Build_Texture_Name>i_button_sa_recharge.tga</Build_Texture_Name>
    <Overlay_Texture_Name>i_sa_ami_box.tga</Overlay_Texture_Name><Click_SFX>GUI_Button_Press_2_SFX</Click_SFX>
    <Disabled_Texture_Name>i_special_border_button.tga</Disabled_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="special_button_01"><Type>TextButton</Type><Base_Layer>4</Base_Layer>
    <Blank_Texture_Name>i_special_border_button.tga</Blank_Texture_Name><Build_Texture_Name>i_button_sa_recharge.tga</Build_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="special_border_00"><Type>TextButton</Type><Base_Layer>2</Base_Layer>
    <Icon_Alternate_Texture_Name>i_special_border_full.tga i_special_border_left.tga i_special_border_center.tga i_special_border_right.tga</Icon_Alternate_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="tqueue00"><Type>TextButton</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name>
    <Build_Texture_Name>i_button_build.tga</Build_Texture_Name><Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>6</Font_Point_Size></CommandBarComponent>
  <CommandBarComponent Name="Build_00"><Type>TextButton</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name>
    <Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>7</Font_Point_Size><Text_Offset2>0 -20</Text_Offset2>
    <Tooltip_Text>TEXT_PRODUCTION_QUEUE_FULL TEXT_PRODUCTION_POP_CAP_REACHED TEXT_PRODUCTION_INSUFFICIENT_FUNDS</Tooltip_Text></CommandBarComponent>
  <CommandBarComponent Name="Build_01"><Type>TextButton</Type><Blank_Texture_Name>i_button_hologram.tga</Blank_Texture_Name>
    <Font_Name>EmpireAtWar-Medium</Font_Name><Font_Point_Size>7</Font_Point_Size><Text_Offset2>0 -20</Text_Offset2>
    <Tooltip_Text>TEXT_PRODUCTION_QUEUE_FULL TEXT_PRODUCTION_POP_CAP_REACHED TEXT_PRODUCTION_INSUFFICIENT_FUNDS</Tooltip_Text></CommandBarComponent>
  <CommandBarComponent Name="r_0000"><Type>TextButton</Type><Mouse_Over_Texture_Name>i_button_unit_frame_select.tga</Mouse_Over_Texture_Name>
    <Disabled_Texture_Name>i_button_reinforce_disabled.tga</Disabled_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="b_retreat"><Type>Button</Type><Blank_Texture_Name>i_button_back_new.tga</Blank_Texture_Name>
    <Icon_Alternate_Texture_Name>i_button_command_bar_retreat.tga i_button_command_bar_retreat.tga</Icon_Alternate_Texture_Name>
    <Mouse_Over_Texture_Name>i_button_heros_over.tga</Mouse_Over_Texture_Name><Tooltip_Text>TEXT_TOOLTIP_RETREAT_BUTTON</Tooltip_Text></CommandBarComponent>
  <CommandBarComponent Name="b_option_t"><Type>TextButton</Type>
    <Icon_Texture_Name>i_button_command_bar_options.tga</Icon_Texture_Name>
    <Mouse_Over_Texture_Name>i_button_command_bar_options_over.tga</Mouse_Over_Texture_Name>
    <Selected_Texture_Name>i_button_command_bar_options_press.tga</Selected_Texture_Name>
    <Disabled_Texture_Name>i_button_command_bar_options_disabled.tga</Disabled_Texture_Name>
    <Tooltip_Text>TEXT_BUTTON_MAIN_MENU TEXT_SECOND</Tooltip_Text><Toggle>True</Toggle>
    <Mouse_Over_SFX>GUI_Highlight_Mouse_Over</Mouse_Over_SFX><Click_SFX>GUI_Button_Press_2_SFX</Click_SFX>
    <Disabled_Darken>True</Disabled_Darken><Blink_Duration>3.0</Blink_Duration><Blink_Rate>0.15</Blink_Rate></CommandBarComponent>
  <CommandBarComponent Name="b_hidden_t"><Type>Button</Type><Hidden>True</Hidden><Icon_Texture_Name>i_button_heros_over.tga</Icon_Texture_Name></CommandBarComponent>
  <CommandBarComponent Name="radar"><Type>Button</Type><Size>171 175</Size></CommandBarComponent>
  <CommandBarComponent Name="radar_blip"><Type>Button</Type><Base_Layer>5</Base_Layer><No_Shell>True</No_Shell></CommandBarComponent>
  <CommandBarComponent Name="tooltip_back"><Type>TextButton</Type><Blank_Texture_Name>i_tooltip_back.tga</Blank_Texture_Name>
    <No_Shell>True</No_Shell><Base_Layer>10</Base_Layer></CommandBarComponent>
  <CommandBarComponent Name="tooltip_left_text"><Type>TextButton</Type><Font_Name>Arial</Font_Name><Font_Point_Size>7</Font_Point_Size>
    <No_Shell>True</No_Shell><Base_Layer>10</Base_Layer><Max_Text_Width>110</Max_Text_Width></CommandBarComponent>
</CommandBarComponents>)";

ComponentTable stage2_table() {
    ComponentTable t;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(kStage2Xml), "stage2.xml");
    if (doc.ok()) t.parse(doc.root());
    return t;
}

TextureAtlas stage2_atlas() {
    TextureAtlas a = hud_test::make_atlas();
    a.add({"i_special_border_button.tga", 0, 100, 24, 25, true});
    a.add({"i_button_sa_recharge.tga", 30, 100, 24, 25, true});
    a.add({"i_sa_ami_box.tga", 60, 100, 10, 10, true});
    a.add({"i_special_border_full.tga", 0, 130, 54, 119, true});
    a.add({"i_special_border_left.tga", 0, 130, 54, 119, true});
    a.add({"i_button_hologram.tga", 160, 0, 54, 54, true});
    a.add({"i_button_build.tga", 160, 60, 54, 54, true});
    a.add({"i_button_back_new.tga", 200, 100, 36, 36, true});
    a.add({"i_button_command_bar_retreat.tga", 200, 140, 30, 30, true});
    a.add({"i_button_heros_over.tga", 200, 180, 36, 36, true});
    a.add({"i_button_command_bar_options.tga", 100, 200, 24, 24, true});
    a.add({"i_button_command_bar_options_over.tga", 130, 200, 24, 24, true});
    a.add({"i_button_command_bar_options_press.tga", 160, 200, 24, 24, true});
    a.add({"i_button_command_bar_options_disabled.tga", 190, 200, 24, 24, true});
    a.add({"i_tooltip_back.tga", 220, 220, 32, 32, true});
    a.add({"i_icon_unit.tga", 64, 160, 30, 30, true});
    return a;
}

InputState keys_frame(std::initializer_list<uint16_t> pressed, bool shift = false, bool ctrl = false, bool alt = false) {
    InputState in;
    InputFrame f;
    in.update(f);  // previous frame: nothing held
    for (uint16_t k : pressed) {
        f.keys_down.set(k);
        f.keys_pressed.set(k);
    }
    if (shift) f.keys_down.set(vk::kShift);
    if (ctrl) f.keys_down.set(vk::kControl);
    if (alt) f.keys_down.set(vk::kAlt);
    in.update(f);
    return in;
}

// ------------------------------------------------------------------------------------------- shells
#ifdef ALAMO_UI_HAVE_ASSETS
std::shared_ptr<assets::ModelAsset> synthetic_shell() {
    auto m = std::make_shared<assets::ModelAsset>();
    m->name = "I_TACTICAL_CONTROLS.ALO";
    assets::Skeleton& sk = m->skeleton;
    auto bone = [&](const char* n, int32_t parent, float x, float y, float z) {
        sk.names.emplace_back(n);
        sk.parents.push_back(parent);
        sk.visible.push_back(1);
        sk.billboard_mode.push_back(0);
        assets::Mat3x4 l;
        l.m[0][3] = x;
        l.m[1][3] = y;
        l.m[2][3] = z;
        sk.bind_local.push_back(l);
    };
    bone("Root", -1, 0, 0, 0);
    bone("s_select_00", 0, 380, 93, 4);
    bone("radar", 0, 102, 98, 0);
    bone("special_border_11", 0, 985, 70.5f, 1);
    bone("special_button_22", 3, 0, 61.5f, 1);
    bone("Empire_Faceplate_ALT0", 0, 511, 127, -1);
    bone("Rebel_Faceplate_ALT1", 0, 511, 127, -1);
    sk.finalize();
    auto mesh = [&](const char* n, uint32_t b, bool hidden, float hx, float hy, float cx = 0, float cy = 0) {
        assets::MeshInfo mi;
        mi.name = n;
        mi.attach_bone = b;
        mi.hidden = hidden;
        mi.file_bounds = {{cx - hx, cy - hy, 0}, {cx + hx, cy + hy, 0}};
        m->meshes.push_back(mi);
    };
    mesh("s_select_00", 1, true, 25, 25);
    mesh("radar", 2, true, 87.5f, 87.5f);
    mesh("special_button_22", 4, true, 12, 12.5f);
    mesh("Empire_Faceplate_ALT0", 5, false, 539, 142, 27, 14);
    mesh("Rebel_Faceplate_ALT1", 6, false, 539, 142, 27, 14);
    return m;
}

void test_shell_models() {
    CHECK2(shell_alt_index("Empire_Faceplate_ALT0") == 0 && shell_alt_index("b_Help_Droid_Rebel_alt1") == 1 &&
           shell_alt_index("x_ALT12") == 12 && shell_alt_index("a_ALT") == -1 && shell_alt_index("a_ALTx") == -1 &&
           shell_alt_index("radar") == -1 && shell_alt_index("reinforcement_window_ALT4") == 4);
    const auto model = synthetic_shell();
    const ShellModelInfo info = shell_model_info(*model);
    CHECK2(info.bones.size() == 6 && info.meshes.size() == 5);
    const ShellBone* b22 = nullptr;
    for (const ShellBone& b : info.bones)
        if (b.name == "special_button_22") b22 = &b;
    CHECK2(b22 && near2(b22->x, 985) && near2(b22->y, 132) && near2(b22->z, 2) && near2(b22->half_size.x, 12));
    CHECK2(info.bones[0].name == "s_select_00" && near2(info.bones[0].half_size.x, 25) && near2(info.bones[0].z, 4));
    CHECK2(info.bones[4].name == "Empire_Faceplate_ALT0" && near2(info.bones[4].rect_center.x, 27));
    CHECK2(info.meshes[3].alt == 0 && info.meshes[4].alt == 1 && info.meshes[0].alt == -1 && info.meshes[0].hidden);
    const std::vector<uint8_t> m0 = shell_mesh_hidden(info, 0), m1 = shell_mesh_hidden(info, 1);
    CHECK2((m0 == std::vector<uint8_t>{1, 1, 1, 0, 1}) && (m1 == std::vector<uint8_t>{1, 1, 1, 1, 0}));

    const ComponentTable t = stage2_table();
    ShellSet set;
    std::vector<std::string> missing;
    const size_t n = set.load(
        t, [&](std::string_view name) -> std::shared_ptr<const assets::ModelAsset> {
            return name == "i_tactical_controls.alo" ? model : nullptr;
        },
        {}, &missing);
    CHECK2(n == 1 && missing.empty() && set.shells()[0].component == static_cast<uint32_t>(t.find("i_main_skirmish")));
    set.set_alt(set.shells()[0].component, 1);
    CHECK2(set.shells()[0].mesh_hidden == m1);
    HudLayout layout;
    set.apply(layout);
    for (auto [w, h] : {std::pair{1024u, 768u}, std::pair{1920u, 1080u}, std::pair{1280u, 1024u}}) {
        const HudCanvas cv = HudCanvas::make(w, h);
        layout.update(cv, t);
        const ComponentPlacement& p = layout.placement(static_cast<uint32_t>(t.find("s_select_00")));
        CHECK2(p.placed && p.from_bone && near2(p.center.x, -0.5f * cv.width + 380) && near2(p.center.y, -0.5f * cv.height + 93) &&
               near2(p.size.x, 50) && near2(p.size.y, 50));
        const ComponentPlacement& r = layout.placement(static_cast<uint32_t>(t.find("radar")));
        CHECK2(r.placed && near2(r.size.x, 175) && near2(r.rect_center.x, r.center.x));
    }
    set.set_visible(set.shells()[0].component, false);
    set.apply(layout);
    layout.update(HudCanvas::make(1024, 768), t);
    CHECK2(!layout.placement(static_cast<uint32_t>(t.find("radar"))).placed);
}

#ifdef ALAMO_UI_HAVE_VFS
void test_real_shell() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real shell: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK2(files != nullptr);
    if (!files) return;
    ComponentTable table;
    CHECK2(table.load(*files, &err));
    ShellSet set;
    std::vector<std::string> missing;
    const std::string names[] = {"i_main_skirmish", "i_main_reinforce", "i_main_commandbar"};
    set.load(
        table,
        [&](std::string_view name) -> std::shared_ptr<const assets::ModelAsset> {
            auto m = std::make_shared<assets::ModelAsset>();
            std::string e;
            if (!assets::load_model(*files, name, *m, &e)) return nullptr;
            return m;
        },
        names, &missing);
    for (const std::string& m : missing) std::printf("real shell: missing %s\n", m.c_str());
    CHECK2(set.shells().size() == 3);
    const ShellSet::Shell* tac = set.find(static_cast<uint32_t>(table.find("i_main_skirmish")));
    CHECK2(tac != nullptr);
    if (!tac) return;
    // Model-space convention (the stage-1 [port] assumption corrected): canvas units, +Y up, origin bottom-left.
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    uint32_t alts[3] = {};
    for (const ShellBone& b : tac->info.bones) {
        minx = std::min(minx, b.x);
        maxx = std::max(maxx, b.x);
        miny = std::min(miny, b.y);
        maxy = std::max(maxy, b.y);
    }
    for (const ShellMesh& m : tac->info.meshes)
        if (m.alt >= 0 && m.alt < 3 && !m.hidden) ++alts[m.alt];
    std::printf("real i_tactical_controls.alo: %zu bones, %zu meshes, bones x %.1f..%.1f y %.1f..%.1f, ALT meshes %u/%u/%u\n",
                tac->info.bones.size(), tac->info.meshes.size(), minx, maxx, miny, maxy, alts[0], alts[1], alts[2]);
    CHECK2(tac->info.bones.size() >= 150 && minx >= -1.0f && maxx <= 1100.0f && miny >= -150.0f && maxy <= 768.0f);
    CHECK2(alts[0] >= 1 && alts[1] >= 1 && alts[2] >= 1);
    // FIXPASS_GALACTIC §4.2 / capture C2: the bottom-left advisor bones, model units from the bottom-left corner.
    for (const ShellBone& b : tac->info.bones) {
        const auto is = [&b](const char* n) { return _stricmp(b.name.c_str(), n) == 0; };
        if (is("Movie_tactical") || is("b_droid_help_tactical") || is("Text_Planet_tactical"))
            std::printf("real advisor bone %s (%.1f, %.1f) mesh half %.1f x %.1f, rect centre +%.1f +%.1f\n", b.name.c_str(),
                        static_cast<double>(b.x), static_cast<double>(b.y), static_cast<double>(b.half_size.x),
                        static_cast<double>(b.half_size.y), static_cast<double>(b.rect_center.x),
                        static_cast<double>(b.rect_center.y));
        if (is("Movie_tactical")) CHECK2(near2(b.x, 50, 1.0f) && near2(b.y, 288, 1.0f));
        if (is("b_droid_help_tactical")) CHECK2(near2(b.x, 19, 1.0f) && near2(b.y, 255, 1.0f));
        if (is("Text_Planet_tactical")) CHECK2(near2(b.x, 179, 1.0f) && near2(b.y, 239, 1.0f));
    }
    // FIXPASS_GALACTIC §4.2 / capture C2: Movie_galactic on the galactic bar.
    if (const ShellSet::Shell* gal = set.find(static_cast<uint32_t>(table.find("i_main_commandbar")))) {
        bool movie_bone = false;
        for (const ShellBone& b : gal->info.bones)
            if (_stricmp(b.name.c_str(), "Movie_galactic") == 0) {
                std::printf("real advisor bone %s (%.1f, %.1f)\n", b.name.c_str(), static_cast<double>(b.x), static_cast<double>(b.y));
                movie_bone = near2(b.x, 51, 1.0f) && near2(b.y, 288, 1.0f);
            }
        CHECK2(movie_bone);
    }
    HudLayout layout;
    set.apply(layout);
    for (auto [w, h] : {std::pair{1024u, 768u}, std::pair{1920u, 1080u}}) {
        const HudCanvas cv = HudCanvas::make(w, h);
        layout.update(cv, table);
        const ComponentPlacement& s0 = layout.placement(static_cast<uint32_t>(table.find("s_select_00")));
        const ComponentPlacement& rd = layout.placement(static_cast<uint32_t>(table.find("radar")));
        const ComponentPlacement& r0 = layout.placement(static_cast<uint32_t>(table.find("r_0000")));
        std::printf("real layout %ux%u: s_select_00 (%.1f, %.1f) %gx%g, radar (%.1f, %.1f) %gx%g, r_0000 (%.1f, %.1f)\n", w, h,
                    s0.center.x, s0.center.y, s0.size.x, s0.size.y, rd.center.x, rd.center.y, rd.size.x, rd.size.y, r0.center.x,
                    r0.center.y);
        CHECK2(s0.placed && s0.from_bone && near2(s0.size.x, 50) && s0.center.y < -0.5f * cv.height + 120.0f);
        CHECK2(rd.placed && near2(rd.size.x, 175, 1.0f) && rd.center.x < -0.5f * cv.width + 120.0f);
        // Land reinforcement window: top-left (bones y <= 0 -> origin on the top edge).
        CHECK2(r0.placed && r0.center.y > 0.5f * cv.height - 80.0f && r0.center.x < -0.5f * cv.width + 60.0f);
    }
}
#endif
#endif

// ------------------------------------------------------------------------------------------- hotkeys
uint16_t slot_of(const char* name) { return static_cast<uint16_t>(hotkey_slot(name)); }

constexpr uint16_t kOem4 = 0xDB, kOemPeriod = 0xBE, kOemPlus = 0xBB;  // [ . =

void test_hotkeys() {
    const auto slots = hotkey_slots();
    bool names_ok = true;
    for (uint16_t i = 0; i < kHotkeySlots; ++i) {
        names_ok = names_ok && slots[i].name != nullptr && hotkey_slot(slots[i].name) == i;
    }
    CHECK2(names_ok);
    HotkeyTable t;
    CHECK2(t.binding(0) == (HotkeyBinding{'1', HotkeyMod::None}) && t.binding(9) == (HotkeyBinding{'0', HotkeyMod::None}));
    CHECK2(t.binding(hk::kSelectAll) == (HotkeyBinding{'Y', HotkeyMod::Ctrl}) && t.binding(hk::kStop) == (HotkeyBinding{'H', HotkeyMod::None}));  // [port: Gabriel] S -> H
    CHECK2(t.binding(123) == (HotkeyBinding{'1', HotkeyMod::Ctrl}) && t.binding(142) == (HotkeyBinding{'0', HotkeyMod::Shift}) &&
           t.binding(152) == (HotkeyBinding{'0', HotkeyMod::Alt}));
    CHECK2(t.binding(hk::kScrollUp).key == 'W' && t.binding(hk::kYawLeft).key == vk::kLeft && t.binding(hk::kScreenshot).key == 0x7B && t.binding(slot_of("Ability_HomeOne")).key == 0xBA);
    CHECK2(hotkey_slot("Stop") == 16 && hotkey_slot("Ability_ProximityMines") == 45 && hotkey_slot("Ability_FullSalvo") == 65 &&
           hotkey_slot("TogglePlayerList") == 89 && hotkey_slot("Ability_FoCSensorJamming") == 122);
    CHECK2(hotkey_group(0) == 1 && hotkey_group(9) == 0 && hotkey_group(124) == 2 && hotkey_group(142) == 0 && hotkey_group(10) == -1);
    // Rebinding the recall key moves the derived slots and frees the keys they now use.
    t.bind(0, {'Z', HotkeyMod::None});
    CHECK2(t.binding(123) == (HotkeyBinding{'Z', HotkeyMod::Ctrl}) && t.binding(133) == (HotkeyBinding{'Z', HotkeyMod::Shift}) &&
           t.binding(143) == (HotkeyBinding{'Z', HotkeyMod::Alt}));
    CHECK2(t.binding(slot_of("Ability_SpreadOut")).key == -1 && t.binding(slot_of("Ability_BuzzDroids")).key == -1 &&
           t.binding(slot_of("Ability_TargetedInvulnerability")).key == -1);
    t.bind(hk::kStop, {'G', HotkeyMod::None});  // takes G from Guard
    CHECK2(t.binding(hk::kGuard).key == -1 && t.binding(hk::kStop).key == 'G');
    // Profile round trip.
    HotkeyProfile prof;
    t.save(prof);
    CHECK2(prof.at("KeySettingsCount") == kHotkeySlots && prof.at("KeySettingKeycode0") == 'Z' && prof.at("KeySettingModifier123") == 2 &&
           prof.at("KeySettingKeycode14") == -1);
    HotkeyTable u;
    CHECK2(u.load(prof));
    bool same = true;
    for (uint16_t i = 0; i < kHotkeySlots; ++i) same = same && u.binding(i) == t.binding(i);
    CHECK2(same);
    HotkeyProfile partial{{"KeySettingsCount", 1}, {"KeySettingKeycode0", 'P'}, {"KeySettingModifier0", 7}};
    HotkeyTable p;
    CHECK2(p.load(partial) && p.binding(0) == (HotkeyBinding{'P', HotkeyMod::None}) && p.binding(1).key == '2');
    CHECK2(!p.load(HotkeyProfile{}));
    // Exact modifier matching.
    HotkeyTable d;
    std::vector<uint16_t> fired;
    d.pressed(keys_frame({'S'}), fired);
    CHECK2(fired == (std::vector<uint16_t>{hk::kScrollDown}));  // [port: Gabriel] S scrolls
    d.pressed(keys_frame({kOem4}, true), fired);
    CHECK2(fired == (std::vector<uint16_t>{static_cast<uint16_t>(hotkey_slot("Ability_SpaceSelfDestruct"))}));
    d.pressed(keys_frame({'S'}, true), fired);  // [port: Gabriel] W A S D only move the camera
    CHECK2(fired.empty());
    d.pressed(keys_frame({kOemPeriod}, false, true), fired);
    CHECK2(fired == (std::vector<uint16_t>{static_cast<uint16_t>(hotkey_slot("Ability_StimPack"))}));
    d.pressed(keys_frame({kOemPlus}, false, false, true), fired);
    CHECK2(fired == (std::vector<uint16_t>{static_cast<uint16_t>(hotkey_slot("Ability_ForceSight"))}));
    // P1_CAMERA_INPUT.md §6: a modified slot fires when its modifier is held, others ignored; the first match wins.
    d.pressed(keys_frame({kOemPeriod}, true, true), fired);  // Shift+Ctrl+. -> Ctrl+. (stim pack)
    CHECK2(fired == (std::vector<uint16_t>{static_cast<uint16_t>(hotkey_slot("Ability_StimPack"))}));
    d.pressed(keys_frame({'1'}, true, true), fired);  // Shift+Ctrl+1 -> assign (slot 123 before add 133)
    CHECK2(fired == (std::vector<uint16_t>{hk::kAssignBase}));
    d.pressed(keys_frame({'H'}, true), fired);  // Shift+H: Hunt, never the unmodified Stop
    CHECK2(fired == (std::vector<uint16_t>{static_cast<uint16_t>(hotkey_slot("Ability_Hunt"))}));
    // Dispatch: control groups (recall / Ctrl assign / Shift add), double recall camera jump, stop needs a selection.
    HotkeyDispatcher disp;
    std::vector<commands::EventData> cmds;
    std::vector<HotkeyEvent> evs;
    const uint32_t sel[2] = {5, 9};
    disp.update(keys_frame({'1'}), d, sel, 10.0, cmds, evs);
    CHECK2(cmds.size() == 1 && cmds[0] == commands::ev::control_group(1, false) && evs.empty());
    disp.update(keys_frame({'1'}), d, sel, 10.6, cmds, evs);
    CHECK2(cmds.size() == 2 && evs.size() == 1 && evs[0].camera_jump && evs[0].group == 1);
    disp.update(keys_frame({'3'}, false, true), d, sel, 12.0, cmds, evs);
    disp.update(keys_frame({'0'}, true), d, sel, 12.1, cmds, evs);
    CHECK2(cmds.size() == 4 && cmds[2] == commands::ev::control_group(3, true, false) && cmds[3] == commands::ev::control_group(0, true, true));
    disp.update(keys_frame({'H'}), d, {}, 13.0, cmds, evs);
    CHECK2(cmds.size() == 4);
    disp.update(keys_frame({'H'}), d, sel, 13.1, cmds, evs);
    disp.update(keys_frame({'Y'}, false, true), d, sel, 13.2, cmds, evs);
    disp.update(keys_frame({'E'}), d, sel, 13.3, cmds, evs);
    CHECK2(cmds.size() == 7 && cmds[4] == commands::ev::stop() && cmds[5] == commands::ev::select_all() && cmds[6] == commands::ev::retreat(true));
    const size_t e0 = evs.size();
    disp.update(keys_frame({'G'}), d, sel, 14.0, cmds, evs);
    disp.update(keys_frame({'K'}, false, false, true), d, sel, 14.1, cmds, evs);
    CHECK2(evs.size() == e0 + 2 && evs[e0].slot == hk::kGuard && evs[e0 + 1].kind == HotkeyKind::Ability &&
           evs[e0 + 1].slot == hotkey_slot("Ability_LuckyShot"));
    // P1 Bindings follow the table.
    Bindings b;
    t.apply_to_bindings(b);
    CHECK2(b.binding(Action::Stop, 0).key == 'G' && b.binding(Action::Stop, 0).mods == kModNone);
    CHECK2(b.binding(Action::ControlGroup1, 0).key == 'Z' && b.binding(Action::SelectAll, 0).mods == kModCtrl &&
           b.binding(Action::ScrollUp, 0).mods == kModAny && b.binding(Action::ScrollUp, 0).key == 'W');
    // OrderTranslator leaves the keyboard to the dispatcher when asked.
    OrderTranslator ot;
    ot.set_keyboard_orders(false);
    OrderContext ctx;
    ot.update(keys_frame({'Y'}, false, true), Bindings{}, ctx);
    CHECK2(ot.pending().empty());
    OrderTranslator ot2;
    ot2.update(keys_frame({'Y'}, false, true), Bindings{}, ctx);
    CHECK2(ot2.pending().size() == 1);
}

// ------------------------------------------------------------------------------------------- text wrap
void test_wrap() {
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    const int32_t f = fonts.font("EmpireAtWar-Medium", 9, 768);  // 15 px em, 9 px advance
    CHECK2(fonts.measure(f, U"a") == 9);
    CHECK2(wrap_text(fonts, f, U"aaa bbb ccc", 70) == (std::vector<std::u32string>{U"aaa bbb", U"ccc"}));
    CHECK2(wrap_text(fonts, f, U"aaa bbb ccc", 0) == (std::vector<std::u32string>{U"aaa bbb ccc"}));
    CHECK2(wrap_text(fonts, f, U"abcdefghij", 40) == (std::vector<std::u32string>{U"abcd", U"efgh", U"ij"}));
    CHECK2(wrap_text(fonts, f, U"ab\ncd ef", 1000) == (std::vector<std::u32string>{U"ab", U"cd ef"}));
    CHECK2(wrap_text(fonts, f, U"aa   bb", 30) == (std::vector<std::u32string>{U"aa", U"bb"}));
    const HudCanvas c800 = HudCanvas::make(800, 600), c1024 = HudCanvas::make(1024, 768);
    CHECK2(near2(wrap_width_px(c800, 128), 95.0f) && near2(wrap_width_px(c1024, 110), 110.0f) && wrap_width_px(c1024, 0) == 0.0f);
    HudDrawList dl;
    HudTextStyle st;
    st.font = f;
    const TextBlock tb = add_text_block(dl, 3, HudList::Text, fonts, c1024, U"aaa bbb ccc", {0, 0}, 70, st);
    CHECK2(tb.lines == 2 && near2(tb.width, 63) && dl.items(3, HudList::Text).size() == 9);
    const TextBlock mb = measure_text_block(fonts, c1024, U"aaa bbb ccc", 70, st);
    CHECK2(mb.lines == 2 && near2(mb.height, tb.height));
}

// ------------------------------------------------------------------------------------------- buttons
void test_buttons() {
    const ComponentTable t = stage2_table();
    const TextureAtlas atlas = stage2_atlas();
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    const HudCanvas cv = HudCanvas::make(1024, 768);
    HudButtons b;
    b.bind(t);
    std::vector<std::string> sfx;
    b.set_sfx_hook([&](std::string_view e, int32_t) { sfx.emplace_back(e); });
    const auto opt = static_cast<uint32_t>(t.find("b_option_t"));
    const auto hidden = static_cast<uint32_t>(t.find("b_hidden_t"));
    CHECK2(b.visual(opt) == ButtonVisual::Normal && b.state(hidden).hidden && !b.state(opt).hidden);
    std::vector<ButtonClick> clicks;
    b.update(static_cast<int32_t>(opt), {}, false, false, 1.0, clicks);
    b.update(static_cast<int32_t>(opt), {}, false, false, 1.1, clicks);  // no second hover SFX
    CHECK2(sfx == (std::vector<std::string>{"GUI_Highlight_Mouse_Over"}) && b.visual(opt) == ButtonVisual::MouseOver);
    // Full-art mouse-over replaces the icon: still one icon quad.
    HudDrawList dl;
    b.draw(dl, atlas, &fonts, cv, opt, {0, 0}, {}, 1, 1.1);
    CHECK2(dl.items(0, HudList::Alpha).size() == 1 && near2(dl.items(0, HudList::Alpha)[0].uv0.x, 130.0f / 256.0f));
    const HudPointerEvent press{HudPointerEvent::Kind::Press, Button::Left, static_cast<int32_t>(opt), 5, 5};
    b.update(static_cast<int32_t>(opt), std::span<const HudPointerEvent>(&press, 1), false, false, 1.2, clicks);
    CHECK2(b.visual(opt) == ButtonVisual::Pressed && b.pressed() == static_cast<int32_t>(opt));
    const HudPointerEvent rel[2] = {{HudPointerEvent::Kind::Release, Button::Left, static_cast<int32_t>(opt), 5, 5},
                                    {HudPointerEvent::Kind::Click, Button::Left, static_cast<int32_t>(opt), 5, 5}};
    b.update(static_cast<int32_t>(opt), rel, true, false, 1.3, clicks);
    CHECK2(clicks.size() == 1 && clicks[0].component == static_cast<int32_t>(opt) && clicks[0].shift && b.state(opt).selected &&
           b.visual(opt) == ButtonVisual::Selected && sfx.size() == 2 && sfx[1] == "GUI_Button_Press_2_SFX");
    // Disabled: click swallowed, darkened icon replaced by the disabled art.
    b.state(opt).disabled = true;
    b.update(static_cast<int32_t>(opt), std::span<const HudPointerEvent>(&rel[1], 1), false, false, 1.4, clicks);
    CHECK2(clicks.size() == 1 && b.visual(opt) == ButtonVisual::Disabled);
    // Frame-style mouse-over (different size than the icon): drawn on top.
    const auto ret = static_cast<uint32_t>(t.find("b_retreat"));
    b.update(static_cast<int32_t>(ret), {}, false, false, 2.0, clicks);
    HudDrawList dl2;
    b.draw(dl2, atlas, &fonts, cv, ret, {0, 0}, {}, 1, 2.0, 2);
    CHECK2(dl2.items(2, HudList::Alpha).size() == 3);
    {  // No-alpha hover frame (r_NN, i_button_unit_frame_select, QA D11): additive over the icon, also for a same-size portrait.
        TextureAtlas fa = stage2_atlas();
        fa.add({"i_button_unit_frame_select.tga", 300, 0, 48, 48, false});
        fa.add({"i_button_sd_test.tga", 300, 60, 50, 50, true});
        fa.add({"i_button_hero_test.tga", 300, 120, 48, 48, true});
        const auto rb = static_cast<uint32_t>(t.find("r_0000"));
        b.update(static_cast<int32_t>(rb), {}, false, false, 2.5, clicks);
        for (const char* icon : {"i_button_sd_test.tga", "i_button_hero_test.tga"}) {
            b.state(rb).icon_override = icon;
            HudDrawList dlf;
            b.draw(dlf, fa, &fonts, cv, rb, {0, 0}, {}, 1, 2.5);
            CHECK2(dlf.items(0, HudList::Alpha).size() == 1 && dlf.items(0, HudList::Additive).size() == 1 &&
                   near2(dlf.items(0, HudList::Additive)[0].uv0.x, 300.0f / 256.0f));
        }
        b.state(rb).icon_override.clear();
        b.update(static_cast<int32_t>(ret), {}, false, false, 2.6, clicks);
    }
    // Dials, overlay, text.
    const auto sb = static_cast<uint32_t>(t.find("special_button_00"));
    ButtonRuntime& r = b.state(sb);
    r.build_fill = 50.0f;
    r.overlay = true;
    r.text = U"3";
    HudDrawList dl3;
    b.draw(dl3, atlas, &fonts, cv, sb, {0, 0}, {}, 1, 2.0);
    {  // one PrimAlpha list (STRATEGIC_GAPS §7.2): ... dial (fill 50) then the overlay over it
        const auto items = dl3.items(4, HudList::Alpha);
        size_t dials = 0, dial_at = 0;
        for (size_t i = 0; i < items.size(); ++i)
            if (items[i].fill < 100.0f) {
                ++dials;
                dial_at = i;
            }
        CHECK2(dials == 1 && near2(items[dial_at].fill, 50.0f) && dial_at + 1 < items.size() &&
               dl3.items(4, HudList::Text).size() == 1);
    }
    CHECK2(b.draw(dl3, atlas, &fonts, cv, hidden, {0, 0}, {}, 1, 2.0).x == 0.0f);  // Hidden data
    // Blink: Blink_Rate = half period, Blink_Duration seconds.
    const CommandBarComponent& oc = t[opt];
    CHECK2(blink_alpha(oc, -1.0, 5.0) == 1.0f && blink_alpha(oc, 0.0, 0.1) == 1.0f && blink_alpha(oc, 0.0, 0.2) == 0.0f &&
           blink_alpha(oc, 0.0, 3.5) == 1.0f);
    // Tooltip after Tooltip_Delay, key per tooltip index, drawn on layer 10.
    b.state(opt).disabled = false;
    b.state(opt).tooltip_index = 1;
    b.update(-1, {}, false, false, 9.0, clicks);
    b.update(static_cast<int32_t>(opt), {}, false, false, 10.0, clicks);
    TooltipState tip;
    const TooltipConfig cfg;
    tip.update(b, t, cfg, 10.2);
    CHECK2(!tip.active());
    tip.update(b, t, cfg, 10.34);
    CHECK2(tip.active() && tip.key(b, t) == "TEXT_SECOND");
    TextDatabase text;
    text.add("TEXT_SECOND", u"Opens the in-game menu with options, save and load");
    HudDrawList dl4;
    CHECK2(tip.draw(dl4, atlas, fonts, cv, t, b, &text, {500, 400}, 1, cfg));
    CHECK2(dl4.items(10, HudList::Alpha).size() >= 1 && dl4.items(10, HudList::Text).size() > 20);
    for (const HudQuad& q : dl4.items(10, HudList::Text))
        CHECK2(q.max.x <= 0.5f * cv.width + 1.0f && q.max.y <= 0.5f * cv.height + 1.0f);
    // HUD_GAPS §2.1 anchor: below-right of the cursor (+8 px, -24 px), flipped at the right / bottom edges.
    auto tip_box = [&](Vec2f px) {
        HudDrawList d;
        tip.draw(d, atlas, fonts, cv, t, b, &text, px, 1, cfg);
        const HudQuad& q = d.items(10, HudList::Alpha).back();  // the back (the faction frame goes under it)
        return std::pair<Vec2f, Vec2f>{q.min, q.max};
    };
    {
        const Vec2f c0 = cv.pixel_to_canvas(300, 300), c8 = cv.pixel_to_canvas(308, 324);
        const auto [mn, mx] = tip_box({300, 300});
        CHECK2(near2(mn.x, c8.x) && near2(mx.y, c8.y) && mn.x > c0.x && mx.y < c0.y);
        const Vec2f rc = cv.pixel_to_canvas(static_cast<float>(cv.screen_width) - 2.0f, static_cast<float>(cv.screen_height) - 2.0f);
        const auto [fmn, fmx] = tip_box({static_cast<float>(cv.screen_width) - 2.0f, static_cast<float>(cv.screen_height) - 2.0f});
        CHECK2(fmx.x < rc.x && fmn.y > rc.y);  // flipped to above-left
    }
    // GameConstants delays and GUIDialogs tooltips.
    const data::XmlDocument gc = data::XmlDocument::parse(std::string_view(
        "<GameConstants><Tooltip_Delay>500</Tooltip_Delay><Encyclopedia_Delay>900</Encyclopedia_Delay></GameConstants>"));
    const TooltipConfig c2 = tooltip_config_from(gc.root());
    CHECK2(near2(static_cast<float>(c2.delay), 0.5f) && near2(static_cast<float>(c2.encyclopedia_delay), 0.9f) &&
           near2(static_cast<float>(c2.long_encyclopedia_delay), 4.0f));
    const data::XmlDocument gd = data::XmlDocument::parse(std::string_view(
        "<GUIDialogs><Tooltips><IDC_COMBAT_EFF_TIP><TextID>TEXT_COMBAT_EFF_TOOLTIP</TextID></IDC_COMBAT_EFF_TIP></Tooltips></GUIDialogs>"));
    const auto dt = parse_dialog_tooltips(gd.root());
    CHECK2(dt.size() == 1 && dt.at("IDC_COMBAT_EFF_TIP") == "TEXT_COMBAT_EFF_TOOLTIP");
}

// ------------------------------------------------------------------------------------------- radar
void test_radar() {
    const char* xml = R"(<RadarMap><RadarMapEvents>
      <Radar_Map_Event name="Default_Click"><Event_Model_Name>I_minimap_default_click.alo</Event_Model_Name>
        <Event_Model_Scale>0.001</Event_Model_Scale><Event_Duration>.8</Event_Duration><Event_Single_Instance>No</Event_Single_Instance></Radar_Map_Event>
      <Radar_Map_Event name="Interdict_Activated"><Event_Model_Name>I_minimap_beacon.alo</Event_Model_Name>
        <Event_Single_Instance>Yes</Event_Single_Instance><Event_Persistent>Yes</Event_Persistent></Radar_Map_Event>
      </RadarMapEvents><RadarMapSettings>
      <Space_FOW_Color>25, 66, 120, 100 </Space_FOW_Color><Land_FOW_Color>10, 10, 10, 100</Land_FOW_Color>
      <Land_Backdrop_Texture_Name>i_radar_map_grid.tga</Land_Backdrop_Texture_Name >
      <Passability_Color_Settings name="Default"><Color name="passability_water">140,176,200,255</Color>
        <Color name="passability_clear">228,228,228,255</Color></Passability_Color_Settings>
      <Passability_Color_Settings name="Temperate"><Color name="passability_clear">152,203,95,255</Color></Passability_Color_Settings>
      </RadarMapSettings></RadarMap>)";
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(xml));
    RadarSettings s;
    CHECK2(s.parse(doc.root()));
    const RadarEventType* click = s.event("default_click");
    CHECK2(click && near2(click->duration, 0.8f) && click->model == "I_minimap_default_click.alo" && !click->single_instance);
    const RadarEventType* inter = s.event("Interdict_Activated");
    CHECK2(inter && inter->persistent && inter->single_instance);
    CHECK2(s.space_fow == (Rgba8{25, 66, 120, 100}) && s.land_fow == (Rgba8{10, 10, 10, 100}));
    const PassabilityPalette temp = s.palette("temperate"), def = s.palette("Volcanic");
    CHECK2(temp[static_cast<size_t>(Passability::Clear)] == (Rgba8{152, 203, 95, 255}) &&
           temp[static_cast<size_t>(Passability::Water)] == (Rgba8{140, 176, 200, 255}) &&
           def[static_cast<size_t>(Passability::Clear)] == (Rgba8{228, 228, 228, 255}));
    // Image: 2x2 terrain (row 0 = south), fog 2x1 (west fogged, east visible), out 4x4 (row 0 = north).
    const uint8_t cls[4] = {static_cast<uint8_t>(Passability::Clear), static_cast<uint8_t>(Passability::Water),
                            static_cast<uint8_t>(Passability::Clear), static_cast<uint8_t>(Passability::Water)};
    const std::vector<uint8_t> terrain = passability_rgba(cls, temp);
    const uint8_t fog[2] = {0, 255};
    RadarImage img;
    img.build(4, terrain, 2, 2, fog, 2, 1, s.land_fow);
    CHECK2(img.width() == 4 && img.rgba().size() == 64 && img.generation() == 1);
    const uint8_t* west = &img.rgba()[0];        // north-west: clear, fogged
    const uint8_t* east = &img.rgba()[3 * 4];    // north-east: water, visible
    const float a = 100.0f / 255.0f;
    CHECK2(west[0] == static_cast<uint8_t>(std::lround(152 + (10 - 152) * a)) && east[0] == 140 && east[3] == 255);
    img.build(4, terrain, 2, 2, fog, 2, 1, s.land_fow);
    CHECK2(img.generation() == 1);  // unchanged inputs: no re-upload
    const uint8_t fog2[2] = {255, 255};
    img.build(4, {}, 0, 0, fog2, 2, 1, s.space_fow);
    CHECK2(img.generation() == 2 && img.rgba()[3] == 0);  // space: visible = transparent
    // Asteroid fields (Space_Asteroid_Field_Color / _Border_Color): a disc of radius 900 at the map centre on an 8 x 8
    // space radar over a 2000 x 2000 box (250 world units per texel): centre texels = fill, the outer texel ring
    // (650 < d <= 900) = border, the corners untouched.
    {
        const RadarField field{0.0f, 0.0f, 900.0f};
        RadarFieldPaint paint;
        paint.fields = std::span<const RadarField>(&field, 1);
        paint.min_x = paint.min_y = -1000;
        paint.max_x = paint.max_y = 1000;
        paint.fill = s.space_asteroid;
        paint.border = s.space_asteroid_border;
        RadarImage af;
        af.build(8, {}, 0, 0, {}, 0, 0, s.space_fow, paint);
        const auto texel = [&](uint32_t x, uint32_t y) { return &af.rgba()[(static_cast<size_t>(y) * 8 + x) * 4]; };
        // output texel (3, 3) = world (-125, 125): d 177 -> fill; (0, 0) = (-875, 875): d 1237 -> empty; (0, 3) =
        // (-875, 125): d 884 -> border
        CHECK2(texel(3, 3)[0] == s.space_asteroid.r && texel(3, 3)[3] == s.space_asteroid.a && texel(0, 0)[3] == 0);
        CHECK2(texel(0, 3)[0] == s.space_asteroid_border.r && texel(0, 3)[3] == s.space_asteroid_border.a);
    }
    // Events.
    RadarEvents ev;
    ev.add(s, "Default_Click", 10, 20);
    ev.add(s, "Interdict_Activated", 0, 0);
    ev.add(s, "Interdict_Activated", 5, 5);  // single instance: replaces
    CHECK2(ev.items().size() == 2);
    ev.update(0.5f);
    CHECK2(ev.items().size() == 2);
    ev.update(0.5f);
    CHECK2(ev.items().size() == 1 && ev.items()[0].type == "Interdict_Activated" && near2(ev.items()[0].x, 5));
    ev.remove(ev.items()[0].id);
    CHECK2(ev.items().empty());
    // Story blips (Lua Add_Radar_Blip / Remove_Radar_Blip): persistent named markers, upserted by case-insensitive name,
    // never expired by update(), untouched by single-instance event replacement.
    {
        RadarEvents mk;
        const uint32_t m1 = mk.set_marker("starbase_01_blip", 1, 2);
        mk.set_marker("objective_blip", 3, 4);
        CHECK2(mk.set_marker("STARBASE_01_BLIP", 10, 20) == m1 && mk.items().size() == 2);  // upsert: same id, new place
        CHECK2(near2(mk.items()[0].x, 10) && near2(mk.items()[0].y, 20) && mk.items()[0].marker && mk.items()[0].persistent);
        mk.add(s, "Interdict_Activated", 0, 0);
        mk.add(s, "Interdict_Activated", 5, 5);  // replaces the event, not a marker
        CHECK2(mk.items().size() == 3);
        mk.update(100.0f);
        CHECK2(mk.items().size() == 3 && mk.items()[0].age >= 100.0f);  // the event is persistent too; markers stay
        CHECK2(mk.marker_names().size() == 2 && mk.marker_names()[1] == "objective_blip");
        CHECK2(mk.remove_marker("Objective_Blip") && !mk.remove_marker("objective_blip") && mk.items().size() == 2);
        CHECK2(!mk.remove_marker("Interdict_Activated") && mk.items().size() == 2);  // events are not markers
    }
    // Mapping.
    RadarView rv;
    rv.center = {-400, -290};
    rv.size = {175, 175};
    rv.min_x = -1000;
    rv.max_x = 1000;
    rv.min_y = -500;
    rv.max_y = 500;
    const Vec2f c = rv.to_canvas(500, 250);
    CHECK2(near2(c.x, -400 + 0.25f * 175) && near2(c.y, -290 + 0.25f * 175) && rv.inside(c));
    const Vec2f w = rv.to_world(c);
    CHECK2(near2(w.x, 500, 0.1f) && near2(w.y, 250, 0.1f));
    const Vec2f clamped = rv.to_world({1000, 1000});
    CHECK2(near2(clamped.x, 1000) && near2(clamped.y, 500) && !rv.inside({1000, 1000}));
    const auto fp = camera_footprint(top_view2(1024, 768), 0.0f);
    CHECK2(fp[0].x < 0 && fp[0].y > 0 && fp[2].x > 0 && fp[2].y < 0 && near2(fp[0].x, -fp[1].x, 1.0f));
}

// ------------------------------------------------------------------------------------------- markers
void test_markers() {
    const HudCanvas cv = HudCanvas::make(1024, 768);
    const View v = top_view2(1024, 768);
    HudDrawList dl;
    HudWaypointLine line;
    line.points = {{0, 0, 0}, {200, 0, 0}, {200, 200, 0}};
    const uint32_t dots = add_waypoint_lines(dl, cv, v, std::span<const HudWaypointLine>(&line, 1));
    CHECK2(dots > 10 && dl.items(0, HudList::Alpha).size() == dots);
    const data::XmlDocument gc = data::XmlDocument::parse(std::string_view(R"(<GameConstants>
      <GUI_Move_Command_Ack_Effect> GUI_Move_Command_Particle </GUI_Move_Command_Ack_Effect>
      <GUI_Attack_Move_Command_Ack_Effect> GUI_Attack_Move_Command_Particle </GUI_Attack_Move_Command_Ack_Effect>
      <GUI_Movement_Double_Click_Radar_Event_Name> Default_Double_Click </GUI_Movement_Double_Click_Radar_Event_Name>
      <GUI_Move_Acknowledge_Scale_Land> 1.0 </GUI_Move_Acknowledge_Scale_Land><GUI_Move_Acknowledge_Scale_Space> 5.0 </GUI_Move_Acknowledge_Scale_Space>
      <HardPoint_Target_Reticle_Enemy_Screen_Size> 0.03 </HardPoint_Target_Reticle_Enemy_Screen_Size>
      <HardPoint_Target_Reticle_Enemy_Texture> HARD_POINT_ENGINE, I_Hard_Point_Reticle_Engines </HardPoint_Target_Reticle_Enemy_Texture>
      <HardPoint_Target_Reticle_Enemy_Tracked_Texture> HARD_POINT_ENGINE, I_Hard_Point_Reticle_Engines_Tracked </HardPoint_Target_Reticle_Enemy_Tracked_Texture>
      <HardPoint_Target_Reticle_Friendly_Repairing_Texture> HARD_POINT_ENGINE, I_Hard_Point_Reticle_Engines_Repair </HardPoint_Target_Reticle_Friendly_Repairing_Texture>
      <HardPoint_Target_Reticle_Enemy_Texture> HARD_POINT_WEAPON_LASER, I_Hard_Point_Reticle_Weapons </HardPoint_Target_Reticle_Enemy_Texture>
      </GameConstants>)"));
    MoveAckConfig mc;
    mc.parse(gc.root());
    MoveAcks acks;
    const MoveAckRequest req = acks.add(mc, MoveAckKind::AttackMove, {50, 50, 0}, false);
    CHECK2(req.effect == "GUI_Attack_Move_Command_Particle" && near2(req.scale, 5.0f) && req.radar_event == "Default_Attack_Click");
    const MoveAckRequest r2 = acks.add(mc, MoveAckKind::DoubleClick, {0, 0, 0}, true);
    CHECK2(r2.radar_event == "Default_Double_Click" && near2(r2.scale, 1.0f) && acks.size() == 2);
    HudDrawList dl2;
    CHECK2(acks.draw(dl2, cv, v) == 24);
    acks.update(1.0f);
    CHECK2(acks.size() == 0);
    ReticleTable rt;
    rt.parse(gc.root());
    CHECK2(rt.size() == 4 && near2(rt.enemy_screen_size, 0.03f));
    HudHardPoint hp;
    hp.position = {0, 0, 0};
    hp.type = "Hard_Point_Engine";
    CHECK2(rt.texture(hp) == "I_Hard_Point_Reticle_Engines");
    hp.tracked = true;
    CHECK2(rt.texture(hp) == "I_Hard_Point_Reticle_Engines_Tracked");
    hp.enemy = false;
    hp.repairing = true;
    CHECK2(rt.texture(hp) == "I_Hard_Point_Reticle_Engines_Repair");
    TextureAtlas atlas;
    atlas.texture_width = atlas.texture_height = 256;
    atlas.add({"I_Hard_Point_Reticle_Weapons.tga", 0, 0, 32, 32, true});
    const HudHardPoint pts[2] = {{{0, 0, 0}, "HARD_POINT_WEAPON_LASER", true, false, false, false},
                                 {{100, 0, 0}, "HARD_POINT_UNKNOWN", true, false, false, false}};
    HudDrawList dl3;
    CHECK2(add_hardpoint_reticles(dl3, atlas, cv, v, rt, pts) == 2);
    const auto q = dl3.items(1, HudList::Alpha);
    CHECK2(q.size() == 1 + 8 && near2(q[0].max.x - q[0].min.x, 0.03f * 1024.0f) && near2(q[0].max.y - q[0].min.y, 0.04f * 768.0f) &&
           q[0].page == kHudPageAtlas);
    // Playtest: the vanilla reticle textures are stand-alone DDS files - the image-page resolver draws them ("<name>.tga"
    // first, then the bare name); unresolved ones keep the corner outline.
    {
        ReticleTable rt2;
        rt2.set("ENEMY", "HARD_POINT_SHIELD_GENERATOR", "I_Hard_Point_Reticle_Shield_Gen");
        rt2.set("ENEMY", "HARD_POINT_ENGINE", "I_Hard_Point_Reticle_Engines");
        std::vector<std::string> asked;
        const GuiImageResolver images = [&](std::string_view n) {
            asked.emplace_back(n);
            GuiImage g;
            if (n == "I_Hard_Point_Reticle_Shield_Gen.tga") {
                g.valid = true;
                g.page = static_cast<uint16_t>(kHudPageImageBase + 3);
                g.size = {64, 64};
            }
            return g;
        };
        HudHardPoint a, b;
        a.position = {0, 0, 0};
        a.type = "HARD_POINT_SHIELD_GENERATOR";
        b.position = {100, 0, 0};
        b.type = "HARD_POINT_ENGINE";
        const HudHardPoint p2[2] = {a, b};
        HudDrawList dl4;
        CHECK2(add_hardpoint_reticles(dl4, atlas, cv, v, rt2, p2, 1, &images) == 2);
        const auto q4 = dl4.items(1, HudList::Alpha);
        CHECK2(q4.size() == 1 + 8 && q4[0].page == kHudPageImageBase + 3 && near2(q4[0].max.x - q4[0].min.x, 0.03f * 1024.0f));
        CHECK2(q4[0].color == (Rgba8{32, 255, 32, 255}));  // full health: green
        CHECK2(asked.size() == 3 && asked[0] == "I_Hard_Point_Reticle_Shield_Gen.tga" &&
               asked[1] == "I_Hard_Point_Reticle_Engines.tga" && asked[2] == "I_Hard_Point_Reticle_Engines");
        // Hover filter: parent 0 always, otherwise only the hovered ship's points (a selected one gets none).
        HudHardPoint c = a, d = a, e = a;
        c.parent = 7;
        d.parent = 8;
        e.parent = 9;
        const HudHardPoint all[4] = {a, c, d, e};
        HudUnit u7, u8, u9;
        u7.id = 7;
        u7.hovered = true;
        u8.id = 8;
        u8.selected = true;
        u9.id = 9;
        const HudUnit units[3] = {u9, u8, u7};
        const std::vector<HudHardPoint> shown = filter_hardpoints(all, units);
        CHECK2(shown.size() == 2 && shown[0].parent == 0 && shown[1].parent == 7);  // hover only (HUD_GAPS §1.1)
        CHECK2(filter_hardpoints(all, {}).size() == 1);
        {  // R5 stealth (ABILITY_VISUALS 1.4): no reticles on a hovered hostile stealthed ship
            HudUnit s7 = u7;
            s7.stealthed = true;
            const HudUnit su[3] = {u9, u8, s7};
            const std::vector<HudHardPoint> hidden = filter_hardpoints(all, su);
            CHECK2(hidden.size() == 1 && hidden[0].parent == 0);
        }
        // Health tint, flash (half size on the "on" phase: frames 0-2, 6-8, ... of 60 at 30 fps) and the hit test.
        HudHardPoint t = a;
        t.health = 0.5f;
        CHECK2(reticle_tint(t) == (Rgba8{255, 255, 32, 255}));
        t.health = 0.2f;
        CHECK2(reticle_tint(t) == (Rgba8{255, 32, 32, 255}));
        t.disabled = true;
        CHECK2(reticle_tint(t) == (Rgba8{128, 128, 128, 255}));
        t.flash_start = 10.0;
        CHECK2(reticle_flash_on(t, 10.01) && !reticle_flash_on(t, 10.0 + 4.0 / 30.0) && reticle_flash_on(t, 10.0 + 6.5 / 30.0) &&
               !reticle_flash_on(t, 12.5) && !reticle_flash_on(t, 9.0));
        CHECK2(near2(reticle_size(t, rt2, cv, 10.01).x, 0.015f * 1024.0f) && near2(reticle_size(t, rt2, cv, 13.0).x, 0.03f * 1024.0f));
        float rx = 0, ry = 0;
        CHECK2(project(v, b.position, rx, ry));
        CHECK2(reticle_at(cv, v, rt2, p2, rx + 2.0f, ry - 2.0f) == 1 && reticle_at(cv, v, rt2, p2, 0.0f, 0.0f) == -1);
    }
    // Hover pick by bounding sphere (pick_hud_unit): the first sphere the cursor ray enters; neutral units optional.
    {
        float sx = 0, sy = 0;
        CHECK2(project(v, Vec3{0, 0, 0}, sx, sy));
        HudUnit n0, n1, n2;
        n0.id = 3;
        n0.position = {0, 0, 0};
        n0.radius = 40.0f;
        n0.owner = -1;
        n1.id = 4;
        n1.position = {2000, 0, 0};
        n1.radius = 40.0f;
        n1.owner = 1;
        n2.id = 5;
        n2.position = {0, 0, 0};
        n2.radius = 20.0f;
        n2.owner = 1;
        const HudUnit us[3] = {n1, n0, n2};
        const auto px = static_cast<int32_t>(sx), py = static_cast<int32_t>(sy);
        const std::optional<size_t> h1 = pick_hud_unit(us, v, px, py);
        CHECK2(h1 && us[*h1].id == 3);  // the larger sphere is entered first
        const std::optional<size_t> h2 = pick_hud_unit(us, v, px, py, true);
        CHECK2(h2 && us[*h2].id == 5);
        CHECK2(!pick_hud_unit(us, v, 0, 0));
    }
}

// HUD_GAPS §5: c_button00..05 are never hidden, only enabled / disabled from the own selection's capabilities; a Lua
// GUI_Component_* call (mods) overrides the engine state.
void test_order_buttons() {
    std::string xml = "<CommandBarComponents>";
    for (int i = 0; i < 6; ++i)
        xml += "<CommandBarComponent Name=\"c_button0" + std::to_string(i) +
               "\"><Type>Button</Type><Blank_Texture_Name>i_button_back_new.tga</Blank_Texture_Name>"
               "<Disabled_Texture_Name>i_button_disabled.tga</Disabled_Texture_Name></CommandBarComponent>";
    xml += "</CommandBarComponents>";
    ComponentTable t;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view(xml), "orders.xml");
    CHECK2(doc.ok());
    t.parse(doc.root());
    const TextureAtlas atlas = stage2_atlas();
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    TacticalHud hud;
    hud.init(t, atlas, fonts);
    hud.resize(1024, 768);
    TacticalHudFrame fr;
    fr.world.view = top_view2(1024, 768);
    auto state = [&](int i) { return hud.buttons().find_state(static_cast<uint32_t>(t.find("c_button0" + std::to_string(i)))); };
    auto enabled = [&](int i) {
        const ButtonRuntime* r = state(i);
        return r && !r->hidden && !r->disabled;
    };
    auto shown = [&](int i) { return state(i) && !state(i)->hidden; };
    // Empty selection: all six shown and disabled.
    hud.build(fr, {}, {});
    for (int i = 0; i < 6; ++i) CHECK2(shown(i) && !enabled(i));
    CHECK2(hud.stats().buttons == 6);
    // Can move but not attack (e.g. a transport): attack off, the rest on.
    fr.order_buttons.can_move = true;
    hud.build(fr, {}, {});
    CHECK2(!enabled(0) && enabled(1) && enabled(2) && enabled(3) && enabled(4) && enabled(5));
    // Can attack but not move (e.g. a turret): attack, attack-move, guard on; move / waypoint / stop off.
    fr.order_buttons = {};
    fr.order_buttons.can_attack = true;
    hud.build(fr, {}, {});
    CHECK2(enabled(0) && enabled(1) && !enabled(2) && !enabled(3) && !enabled(4) && enabled(5));
    // Locked controls: all disabled, still shown.
    fr.order_buttons.can_move = true;
    fr.order_buttons.controls_locked = true;
    hud.build(fr, {}, {});
    for (int i = 0; i < 6; ++i) CHECK2(shown(i) && !enabled(i));
    for (uint32_t k = 0; k < 6; ++k) CHECK2(!order_button_enabled(k, fr.order_buttons));
    // A stray per-frame hidden flag no longer sticks; a mod's script override does.
    fr.order_buttons.controls_locked = false;
    hud.buttons().state(static_cast<uint32_t>(t.find("c_button02"))).hidden = true;
    hud.build(fr, {}, {});
    CHECK2(shown(2) && enabled(2));
    GuiScriptCall hide;
    hide.op = GuiScriptOp::ComponentVisibility;
    hide.args = {GuiScriptArg::of(std::string("c_button02")), GuiScriptArg::of(false)};
    GuiScriptCall dis;
    dis.op = GuiScriptOp::ComponentEnable;
    dis.args = {GuiScriptArg::of(std::string("c_button03")), GuiScriptArg::of(false)};
    CHECK2(apply_gui_script(hide, t, hud.buttons(), nullptr, 0.0) && apply_gui_script(dis, t, hud.buttons(), nullptr, 0.0));
    hud.build(fr, {}, {});
    CHECK2(!shown(2) && shown(3) && !enabled(3) && enabled(4) && hud.stats().buttons == 5);
}

// Drag-select box (Playtest 3): drawn from the drag threshold on, over exactly the bounds box_select uses on release.
void test_drag_box() {
    ComponentTable t;
    const data::XmlDocument doc = data::XmlDocument::parse(std::string_view("<CommandBarComponents></CommandBarComponents>"), "e.xml");
    CHECK2(doc.ok());
    t.parse(doc.root());
    const TextureAtlas atlas = stage2_atlas();
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    TacticalHud hud;
    hud.init(t, atlas, fonts);
    hud.resize(1024, 768);
    TacticalHudFrame fr;
    fr.world.view = top_view2(1024, 768);
    const uint8_t lb = button_bit(Button::Left);
    InputState in;
    InputFrame f;
    f.width = 1024;
    f.height = 768;
    f.dt = 1.0f / 30.0f;
    auto step = [&](int32_t x, int32_t y, uint8_t down, uint8_t pressed, uint8_t released) {
        f.time += f.dt;
        f.mouse_x = x;
        f.mouse_y = y;
        f.buttons_down = down;
        f.buttons_pressed = pressed;
        f.buttons_released = released;
        in.update(f);
        fr.drag_select = in.button(Button::Left).dragging;  // the host's feed (game_main tactical HUD frame)
        fr.drag_rect = in.button(Button::Left).drag;
        hud.build(fr, {}, {});
    };
    auto drag_quads = [&] { return hud.draw_list().items(kDragBoxLayer, HudList::Alpha); };
    // Press, then 3 px (3.6 px from the press): below the 4 px threshold -> no box, a release here would be a click.
    step(200, 150, lb, lb, 0);
    CHECK2(!hud.stats().drag_box && drag_quads().empty());
    step(203, 152, lb, 0, 0);
    CHECK2(!in.button(Button::Left).dragging && !hud.stats().drag_box && drag_quads().empty());
    // Past the threshold: the box, press pixel -> cursor pixel inclusive (drag_select_bounds), resizing with the cursor.
    step(204, 153, lb, 0, 0);
    CHECK2(in.button(Button::Left).dragging && hud.stats().drag_box);
    step(520, 400, lb, 0, 0);
    CHECK2(hud.stats().drag_box);
    const PixelBounds pb = drag_select_bounds(in.button(Button::Left).drag);
    CHECK2(pb.x0 == 200.0f && pb.y0 == 150.0f && pb.x1 == 521.0f && pb.y1 == 401.0f);
    const auto q = drag_quads();
    CHECK2(q.size() == 5);
    if (q.size() == 5) {
        const HudCanvas& c = hud.canvas();
        float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f;
        for (const HudQuad& e : q) {
            const Vec2f a = c.canvas_to_pixel(e.min.x, e.max.y), b = c.canvas_to_pixel(e.max.x, e.min.y);  // top-left, bottom-right
            lo_x = std::min(lo_x, a.x);
            lo_y = std::min(lo_y, a.y);
            hi_x = std::max(hi_x, b.x);
            hi_y = std::max(hi_y, b.y);
            CHECK2(e.page == kHudPageWhite);
        }
        CHECK2(near2(lo_x, pb.x0, 0.01f) && near2(lo_y, pb.y0, 0.01f) && near2(hi_x, pb.x1, 0.01f) && near2(hi_y, pb.y1, 0.01f));
        // Fill first (inset by the 1-pixel outline), then the four outline edges.
        CHECK2(q[0].color == kDragBoxFill);
        const Vec2f fa = c.canvas_to_pixel(q[0].min.x, q[0].max.y), fb = c.canvas_to_pixel(q[0].max.x, q[0].min.y);
        CHECK2(near2(fa.x, pb.x0 + 1, 0.01f) && near2(fa.y, pb.y0 + 1, 0.01f) && near2(fb.x, pb.x1 - 1, 0.01f) &&
               near2(fb.y, pb.y1 - 1, 0.01f));
        for (size_t i = 1; i < 5; ++i) CHECK2(q[i].color == kDragBoxOutline);
    }
    // The release selects what the drawn box covers: a unit projected in the box's last pixel column is in, one pixel
    // further right is out (box_select over the same DragRect).
    tactical::TreeSet trees(1);
    const View& v = fr.world.view;
    auto put = [&](uint32_t id, float px, float py) {
        Vec3 g;
        CHECK2(ray_plane_z(screen_ray(v, px, py), 0.0f, g));
        trees.add(id, {{g.x - 0.01f, g.y - 0.01f, -0.01f}, {g.x + 0.01f, g.y + 0.01f, 0.01f}}, 1, {0});
    };
    put(1, 520.5f, 300.5f);  // last column inside
    put(2, 521.5f, 300.5f);  // first column outside
    put(3, 200.5f, 150.5f);  // the press pixel
    put(4, 360.0f, 149.5f);  // above the top row
    step(520, 400, 0, 0, lb);
    CHECK2(in.button(Button::Left).drag_ended && !hud.stats().drag_box && drag_quads().empty());  // gone on release
    tactical::QueryContext qc;
    const std::vector<uint32_t> sel = box_select(trees.player(0), v, in.button(Button::Left).drag, tactical::kSkipDead, qc);
    CHECK2((sel == std::vector<uint32_t>{1, 3}));
    // A single-pixel-wide drag (vertical line): no fill, top + bottom + left only, still inside the bounds.
    HudDrawList thin;
    CHECK2(add_drag_box(thin, hud.canvas(), DragRect{10, 10, 10, 40}) == 3);
}

// ------------------------------------------------------------------------------------------- tactical HUD stage 2
void test_tactical_stage2() {
    const ComponentTable t = stage2_table();
    const TextureAtlas atlas = stage2_atlas();
    auto box = make_box_rasterizer();
    FontCache fonts(*box);
    TacticalHud hud;
    hud.init(t, atlas, fonts);
    hud.resize(1280, 720);
    std::vector<HudTypeInfo> types(2);
    std::vector<HudUnit> units(3);
    for (uint32_t i = 0; i < 3; ++i) {
        units[i].id = 20 + i;
        units[i].type = i == 2 ? 1u : 0u;
        units[i].owner = 0;
        units[i].selected = true;
        units[i].position = {static_cast<float>(i) * 100.0f - 100.0f, 50.0f, 0};
    }
    TacticalHudFrame fr;
    fr.world.view = top_view2(1280, 720);
    fr.map_min_x = fr.map_min_y = -1000;
    fr.map_max_x = fr.map_max_y = 1000;
    fr.time = 1.0;
    fr.providers.abilities = [](const SelectedGroup& g, std::vector<HudAbility>& out) {
        HudAbility a;
        a.ability_type = 7;  // shared by both groups
        a.icon = "i_icon_unit.tga";
        a.recharge = g.type == 0 ? 0.25f : 0.5f;
        out.push_back(a);
        if (g.type == 1) {
            HudAbility b;
            b.ability_type = 12;
            b.targeted = true;
            b.icon = "i_icon_unit.tga";
            out.push_back(b);
        }
    };
    fr.providers.build_queue = [](std::vector<HudQueueEntry>& out) {
        HudQueueEntry e;
        e.type_index = 33;
        e.location = 900;
        e.progress = 0.4f;
        e.count = 2;
        e.index = 1;
        out.push_back(e);
    };
    fr.providers.reinforcements = [](std::vector<HudReinforcement>& out) {
        HudReinforcement r;
        r.unit = 555;
        r.icon = "i_icon_unit.tga";
        out.push_back(r);
    };
    RadarEvents pings;
    RadarSettings rs;
    pings.add(rs, "Default_Click", 0, 0);
    fr.radar_events = &pings;
    hud.build(fr, units, types);
    const TacticalHudStats& st = hud.stats();
    CHECK2(st.panel_slots == 2 && st.ability_buttons == 2 && st.queue_entries == 1 && st.reinforcements == 1 && st.radar_events == 1);
    CHECK2(st.buttons >= 2);  // b_retreat, b_option_t (b_hidden_t stays hidden)
    CHECK2(st.radar_blips == 3 && st.radar_icons == 0 && hud.radar_models().empty());  // no settings: the ring stand-in
    {  // Radar_Icon_Name per type, Is_Visible_On_Enemy_Radar, radar event models (UI.md §1.7)
        std::vector<HudTypeInfo> rt = types;
        rt[1].radar_icon = "i_icon_unit.tga";
        rt[1].radar_icon_size = {0.1f, 0.1f};
        rt[0].visible_on_enemy_radar = false;
        std::vector<HudUnit> ru = units;
        ru[0].owner = 1;  // hostile, type 0: hidden on the radar
        const data::XmlDocument rd = data::XmlDocument::parse(std::string_view(
            R"(<RadarMap><RadarMapEvents><Radar_Map_Event name="Default_Click"><Event_Model_Name>I_minimap_default_click.alo</Event_Model_Name>
               <Event_Model_Scale>0.002</Event_Model_Scale><Event_Duration>.8</Event_Duration></Radar_Map_Event></RadarMapEvents></RadarMap>)"));
        RadarSettings rs2;
        CHECK2(rs2.parse(rd.root()));
        TacticalHudFrame f2 = fr;
        f2.radar_settings = &rs2;
        f2.radar_event_models = true;
        hud.build(f2, ru, rt);
        const TacticalHudStats& s2 = hud.stats();
        CHECK2(s2.radar_blips == 2 && s2.radar_icons == 1 && s2.radar_models == 1 && s2.radar_events == 1);
        const auto& models = hud.radar_models();
        CHECK2(models.size() == 1 && models[0].model == "I_minimap_default_click.alo" &&
               near2(models[0].scale, 0.002f * 0.5f * hud.canvas().width) && near2(models[0].center.x, hud.radar_view().center.x));
        bool icon_sized = false;  // the icon quad: 0.1 x radar size / 2
        for (uint32_t l = 0; l < kHudLayers; ++l)
            for (const HudQuad& q : hud.draw_list().items(l, HudList::Alpha))
                icon_sized = icon_sized || near2(q.max.x - q.min.x, 0.05f * hud.radar_view().size.x, 0.01f);
        CHECK2(icon_sized);
        {  // R5 sensor jamming (ABILITY_VISUALS 3): hostile jammed units drop off the radar; own / allied jammed ones stay
            std::vector<HudUnit> ju = units;
            ju[0].owner = 1;
            ju[0].jammed = true;   // hostile + jammed: no blip
            ju[1].jammed = true;   // own + jammed: still shown
            ju[2].owner = 1;       // hostile, not jammed: shown
            hud.build(fr, ju, types);
            CHECK2(hud.stats().radar_blips == 2);
            ju[0].jammed = false;
            hud.build(fr, ju, types);
            CHECK2(hud.stats().radar_blips == 3);
            // R5 stealth (ABILITY_VISUALS 1.4, same filter 0x35b4d0): hostile stealthed units drop off the radar, own ones stay
            ju[0].stealthed = true;  // hostile + stealthed: no blip
            ju[1].stealthed = true;  // own + stealthed: still shown
            hud.build(fr, ju, types);
            CHECK2(hud.stats().radar_blips == 2);
            ju[0].stealthed = false;
            hud.build(fr, ju, types);
            CHECK2(hud.stats().radar_blips == 3);
        }
        {  // a story blip draws a gold ring + centre dot (no event model: the Lua name is not a RadarMap.xml type)
            RadarEvents blips;
            const RadarView& rvw = hud.radar_view();
            blips.set_marker("objective_blip", 0.5f * (rvw.min_x + rvw.max_x), 0.5f * (rvw.min_y + rvw.max_y));
            TacticalHudFrame f3 = f2;
            f3.radar_events = &blips;
            hud.build(f3, ru, rt);
            const TacticalHudStats& s3 = hud.stats();
            CHECK2(s3.radar_events == 1 && s3.radar_models == 0 && hud.radar_models().empty());
            bool gold = false;
            for (uint32_t l = 0; l < kHudLayers; ++l)
                for (const HudQuad& q : hud.draw_list().items(l, HudList::Alpha))
                    gold = gold || (q.color.r == 255 && q.color.g == 214 && q.color.b == 64 && q.color.a == 255);
            CHECK2(gold);
        }
        hud.build(fr, units, types);  // back to the plain frame
    }
    // The shared ability merges both groups' users and the best recharge.
    auto center_px = [&](const char* name) {
        const ComponentPlacement& p = hud.layout().placement(static_cast<uint32_t>(t.find(name)));
        const Vec2f px = hud.canvas().canvas_to_pixel(p.center.x, p.center.y);
        return std::pair<int32_t, int32_t>{static_cast<int32_t>(px.x), static_cast<int32_t>(px.y)};
    };
    auto click = [&](const char* name, Button b, bool shift = false) {
        const auto [x, y] = center_px(name);
        const int32_t comp = hud.hit_test(x, y);
        RoutedInput ri;
        ri.hover_component = comp;
        ri.hud = {{HudPointerEvent::Kind::Press, b, comp, x, y},
                  {HudPointerEvent::Kind::Release, b, comp, x, y},
                  {HudPointerEvent::Kind::Click, b, comp, x, y}};
        ri.world.mouse_x = x;
        ri.world.mouse_y = y;
        std::vector<commands::EventData> cmds;
        std::vector<HudAction> acts;
        hud.update_input(ri, shift, false, 2.0, cmds, &acts);
        return std::pair{std::move(cmds), std::move(acts)};
    };
    CHECK2(hud.hit_test(center_px("special_button_00").first, center_px("special_button_00").second) == t.find("special_button_00"));
    {
        auto [cmds, acts] = click("special_button_00", Button::Left);
        CHECK2(cmds.size() == 1 && acts.empty());
        if (cmds.size() == 1) {
            const uint32_t users[3] = {20, 21, 22};
            CHECK2(cmds[0] == ability_mode_event(users, 7, true));
            CHECK2(cmds[0].u("count") == 3 && cmds[0].ids("units") == (std::vector<uint32_t>{20, 21, 22}));
        }
    }
    {
        auto [cmds, acts] = click("special_button_00", Button::Right);
        const uint32_t users[3] = {20, 21, 22};
        CHECK2(cmds.size() == 1 && cmds[0] == ability_autofire_event(users, 7, true));
    }
    {
        auto [cmds, acts] = click("special_button_01", Button::Left);
        CHECK2(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::BeginAbilityTargeting &&
               acts[0].ability_type == 12 && acts[0].ids == (std::vector<uint32_t>{22}));
    }
    {
        // QA D5: a disabled ability keeps its icon, dimmed, over the dark back (one quad; none while it is enabled).
        const auto dimmed_icons = [&hud] {
            size_t n = 0;
            for (uint32_t l = 0; l < kHudLayers; ++l)
                for (const HudQuad& q : hud.draw_list().items(l, HudList::Alpha))
                    if (q.color == (Rgba8{128, 128, 128, 255})) ++n;
            return n;
        };
        CHECK2(dimmed_icons() == 0);
        TacticalHudFrame off = fr;
        off.providers.abilities = [](const SelectedGroup&, std::vector<HudAbility>& out) {
            HudAbility a;
            a.ability_type = 7;
            a.icon = "i_icon_unit.tga";
            a.enabled = false;
            out.push_back(a);
        };
        hud.build(off, units, types);
        CHECK2(hud.stats().ability_buttons == 1 && dimmed_icons() == 1);
        hud.build(fr, units, types);  // back to the enabled buttons for the clicks below
    }
    {
        auto [cmds, acts] = click("tqueue00", Button::Left);
        CHECK2(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::TacticalCancel && acts[0].unit == 900 &&
               acts[0].value == 33 && acts[0].ids == (std::vector<uint32_t>{0, 1}));
    }
    {  // Tactical buy buttons: Build_00 enabled (cost text2), Build_01 blocked by funds (disabled, tooltip index 2).
        const HudBuildOption opts[2] = {{41, "i_button_hologram.tga", 1200, 0, HudBuildOption::Block::None},
                                        {42, "i_button_hologram.tga", 3500, 2, HudBuildOption::Block::Funds}};
        TacticalHudFrame f3 = fr;
        f3.builder = 777;
        f3.build_options = opts;
        hud.build(f3, units, types);
        CHECK2(hud.stats().build_options == 2);
        const ButtonRuntime* b0 = hud.buttons().find_state(static_cast<uint32_t>(t.find("Build_00")));
        const ButtonRuntime* b1 = hud.buttons().find_state(static_cast<uint32_t>(t.find("Build_01")));
        CHECK2(b0 && b1 && b0->text2 == U"1200" && !b0->disabled && b0->tooltip_index == -1 && b1->disabled &&
               b1->tooltip_index == 2 && b1->text == U"2");
        auto buy_click = [&](const char* name, Button b) {
            const int32_t ci = t.find(name);
            Vec2f c{-1e9f, -1e9f};
            for (const HudHitBox& h : hud.hit_boxes())
                if (h.component == ci) c = {0.5f * (h.min.x + h.max.x), 0.5f * (h.min.y + h.max.y)};
            const Vec2f px = hud.canvas().canvas_to_pixel(c.x, c.y);
            const auto x = static_cast<int32_t>(px.x), y = static_cast<int32_t>(px.y);
            CHECK2(hud.hit_test(x, y) == ci);
            RoutedInput ri;
            ri.hover_component = ci;
            ri.hud = {{HudPointerEvent::Kind::Press, b, ci, x, y}, {HudPointerEvent::Kind::Release, b, ci, x, y},
                      {HudPointerEvent::Kind::Click, b, ci, x, y}};
            std::vector<commands::EventData> cmds;
            std::vector<HudAction> acts;
            hud.update_input(ri, false, false, 3.0, cmds, &acts);
            CHECK2(cmds.empty());
            return acts;
        };
        std::vector<HudAction> a = buy_click("Build_00", Button::Left);
        CHECK2(a.size() == 1 && a[0].kind == HudAction::Kind::TacticalBuy && a[0].unit == 777 && a[0].value == 41);
        CHECK2(buy_click("Build_01", Button::Left).empty());   // disabled
        CHECK2(buy_click("Build_00", Button::Right).empty());
        // Anchored on the builder's pixel: Build_00 at anchor + (-54, -67).
        f3.has_build_anchor = true;
        f3.build_anchor_px = hud.canvas().canvas_to_pixel(100.0f, 200.0f);
        hud.build(f3, units, types);
        bool at = false;
        for (const HudHitBox& h : hud.hit_boxes())
            if (h.component == t.find("Build_00"))
                at = near2(0.5f * (h.min.x + h.max.x), 100.0f - 54.0f + 1.0f, 0.01f) && near2(0.5f * (h.min.y + h.max.y), 200.0f - 67.0f - 4.0f, 0.01f);
        CHECK2(at);
        hud.build(fr, units, types);  // no builder: no buttons
        CHECK2(hud.stats().build_options == 0);
    }
    {
        auto [cmds, acts] = click("r_0000", Button::Left);
        CHECK2(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::BeginReinforcement && acts[0].unit == 555);
        const commands::EventData re = reinforce_event(555, {10, 20, 0});
        CHECK2(re.type() == commands::EventType::Reinforce && re.u("unit") == 555 && re.valid());
    }
    {
        auto [cmds, acts] = click("b_retreat", Button::Left);
        CHECK2(cmds.size() == 1 && cmds[0] == commands::ev::retreat(true));
    }
    {
        auto [cmds, acts] = click("b_option_t", Button::Left);
        CHECK2(cmds.empty() && acts.size() == 1 && acts[0].kind == HudAction::Kind::Button && acts[0].name == "b_option_t");
    }
    {
        auto [cmds, acts] = click("s_select_00", Button::Left, true);  // shift: drop group 0 (ids 20, 21)
        CHECK2(cmds.size() == 1 && cmds[0] == commands::ev::select({22}));
    }
    {
        // Radar: left press = camera jump to the point under the cursor; right click = move there + ping.
        const ComponentPlacement& rp = hud.layout().placement(static_cast<uint32_t>(t.find("radar")));
        const RadarView& rv = hud.radar_view();
        CHECK2(rv.valid() && near2(rv.center.x, rp.center.x));
        const Vec2f target = rv.to_canvas(250, -500);
        const Vec2f px = hud.canvas().canvas_to_pixel(target.x, target.y);
        const auto x = static_cast<int32_t>(px.x), y = static_cast<int32_t>(px.y);
        RoutedInput ri;
        ri.hover_component = hud.hit_test(x, y);
        CHECK2(ri.hover_component == t.find("radar"));
        ri.hud = {{HudPointerEvent::Kind::Press, Button::Left, ri.hover_component, x, y}};
        ri.owner[static_cast<size_t>(Button::Left)] = PointerTarget::Hud;
        ri.world.mouse_x = x;
        ri.world.mouse_y = y;
        std::vector<commands::EventData> cmds;
        std::vector<HudAction> acts;
        hud.update_input(ri, false, false, 3.0, cmds, &acts);
        CHECK2(acts.size() == 1 && acts[0].kind == HudAction::Kind::RadarCameraJump && near2(acts[0].x, 250, 15) &&
               near2(acts[0].y, -500, 15));
        // Drag: still held, cursor moved -> another jump; release ends it.
        ri.hud.clear();
        ri.world.mouse_x = x + 10;
        acts.clear();
        hud.update_input(ri, false, false, 3.1, cmds, &acts);
        CHECK2(acts.size() == 1 && acts[0].x > 250);
        ri.hud = {{HudPointerEvent::Kind::Release, Button::Left, ri.hover_component, x, y}};
        ri.owner[static_cast<size_t>(Button::Left)] = PointerTarget::None;
        acts.clear();
        hud.update_input(ri, false, false, 3.2, cmds, &acts);
        CHECK2(acts.empty() && cmds.empty());
        ri.hud = {{HudPointerEvent::Kind::Press, Button::Right, ri.hover_component, x, y},
                  {HudPointerEvent::Kind::Release, Button::Right, ri.hover_component, x, y},
                  {HudPointerEvent::Kind::Click, Button::Right, ri.hover_component, x, y}};
        hud.update_input(ri, false, false, 3.3, cmds, &acts);
        CHECK2(cmds.size() == 1 && cmds[0].type() == commands::EventType::MoveToPosition && acts.size() == 1 &&
               acts[0].kind == HudAction::Kind::RadarPing && acts[0].name == "Default_Click");
    }
    // Tooltip: hover b_retreat past the delay -> drawn on the next build.
    {
        const auto [x, y] = center_px("b_retreat");
        RoutedInput ri;
        ri.hover_component = hud.hit_test(x, y);
        std::vector<commands::EventData> cmds;
        hud.update_input(ri, false, false, 10.0, cmds);
        hud.update_input(ri, false, false, 10.5, cmds);
        fr.time = 10.5;
        fr.cursor_px = {static_cast<float>(x), static_cast<float>(y)};
        hud.build(fr, units, types);
        CHECK2(hud.stats().tooltip && hud.tooltip().component() == t.find("b_retreat"));
    }
    // Determinism of the full stage-2 draw list.
    const uint64_t d1 = hud.draw_list().digest();
    hud.build(fr, units, types);
    CHECK2(hud.draw_list().digest() == d1);
}

}  // namespace

int run_stage2_tests() {
#ifdef ALAMO_UI_HAVE_ASSETS
    test_shell_models();
#ifdef ALAMO_UI_HAVE_VFS
    test_real_shell();
#endif
#else
    std::printf("stage 2: alamo_assets not configured, shell tests skipped\n");
#endif
    test_hotkeys();
    test_wrap();
    test_buttons();
    test_radar();
    test_markers();
    test_tactical_stage2();
    test_order_buttons();
    test_drag_box();
    return g_fail2;
}

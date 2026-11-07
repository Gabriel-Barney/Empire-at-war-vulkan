// test_gui_screens: main and the tests that stayed (see test_gui_screens_common.hpp; the others are in test_gui_screens_*.cpp).
#include "test_gui_screens_common.hpp"

namespace test_gui_screens_t {

#ifdef ALAMO_UI_HAVE_VFS
void test_real_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("real screens: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(cfg, &err);
    CHECK(files != nullptr);
    if (!files) return;
    GuiData gd;
    CHECK(load_gui_data(*files, gd, &err));
    CHECK(gd.script.merge(kPortDialogsRc) && gd.script.find(kPortControlsDialog) != nullptr && gd.script.warnings.empty());
    TextDatabase text;
    load_master_text(*files, "ENGLISH", text);
    TextureAtlas atlas;
    auto raster = make_box_rasterizer();
    FontCache fonts(*raster);
    GuiManager gui;
    gui.init(gd.script, gd.skin, atlas, fonts, &text);
    gui.resize(1920, 1080);
    {  // [port: Gabriel] the camera and controls dialog with the shipped skin; every key edit maps to a slot
        GuiDialog* pd = gui.open(kPortControlsDialog);
        size_t edits = 0, mapped = 0;
        for (size_t i = 0; pd && i < pd->size(); ++i)
            if ((*pd)[i].type == GuiGadgetType::EditBox) {
                ++edits;
                mapped += keyboard_edit_slot((*pd)[i].id()) >= 0 ? 1u : 0u;
            }
        CHECK(pd && edits == 13 && mapped == 13);  // + zoom in / out (slots 160 / 161)
        gui.close(kPortControlsDialog);
    }
    // Every in-game screen opens and fills on the shipped script.
    GameScreens gs;
    gs.init(gui);
    for (size_t n = 0; n <= 3; ++n) {
        MessageBoxSpec m;
        m.caption = U"Caption";
        for (size_t k = 0; k < n; ++k) m.buttons.push_back(U"B");
        CHECK(gs.show_message(m));
        m.centered = true;
        CHECK(gs.show_message(m));
        std::printf("real screens: %zu-button box -> %s\n", n, gs.message_dialog().c_str());
    }
    MessageBoxSpec small;
    small.small = true;
    CHECK(gs.show_message(small) && gs.message_dialog() == "IDD_DIALOG_BOX_SMALL");
    BattleEndInfo be;
    for (int i = 0; i < 14; ++i) be.your_losses.push_back({"i_button_tie.tga", U"TIE", 2});
    be.your_heroes = {"i_button_vader.tga"};
    CHECK(gs.show_battle_end(be));
    CHECK(gui.find(dlg::kBattleEnd)->get("IDC_YOUR_UNIT_NAME11")->visible && gui.find(dlg::kBattleEnd)->get("IDC_YOUR_HERO0")->visible &&
          !gui.find(dlg::kBattleEnd)->get("IDC_YOUR_HERO1")->visible);
    CampaignScoreInfo cs;
    cs.military = 87;
    cs.list = {U"Captured Coruscant"};
    CHECK(gs.show_campaign_score(cs) && gui.find(dlg::kCampaignScore)->get("IDC_MILITARY_EFFICIENCY_STATIC")->text == U"87%");
    CHECK(gs.show_retreat_confirm() && gs.open_chat());
    PlanetInfoData pi;
    pi.name = U"Tatooine";
    pi.history = {U"Outer Rim desert world."};
    CHECK(gs.show_planet_info(pi));
    AutoResolveInfo ar;
    ar.invaders = {U"Star Destroyer"};
    CHECK(gs.show_auto_resolve(ar));
    gs.set_auto_resolve_progress(0.4f, 0.6f);
    CHECK(gui.find(dlg::kAutoResolve)->get("IDC_AUTO_RESOLVE_PROGRESS_DEFENDER")->value == 60);
    const SkirmishMapInfo maps[1] = {{"_SPACE_PLANET_KUAT_01", U"Kuat", 2}};
    CHECK(gs.show_select_map(maps));
    std::printf("real screens: open %zu dialogs\n", gui.open_count());
    gs.close_all();
    CHECK(gui.open_count() == 0);
    // Multiplayer screens on the shipped script.
    LobbyScreens lob;
    lob.init(gui);
    lob.show_multiplayer_menu();
    CHECK(lob.top() == dlg::kMultiplayer);
    lob.show_lan_lobby();
    CHECK(lob.top() == dlg::kLanLobby);
    StagingState st;
    st.host = true;
    st.slots.resize(9);
    st.slots[0].state = SlotState::Human;
    st.slots[0].local = true;
    st.slots[0].name = U"Gabriel";
    CHECK(lob.show_staging(st) && lob.top() == dlg::kLanStaging);
    st.internet = true;
    CHECK(lob.show_staging(st) && lob.top() == dlg::kInternetStaging);
    lob.show_internet_home();
    CHECK(lob.top() == dlg::kInternetHome);
    lob.set_internet_stats(12, 3, 4);
    lob.show_internet_lobby();
    CHECK(lob.top() == dlg::kInternetLobby);
    lob.show_direct_connect({U"192.168.1.20"});
    lob.show_password_prompt("g");
    lob.show_connecting();
    const ConnectionRow rows[1] = {{U"Host", 0.5f, true}};
    lob.show_disconnect(rows);
    lob.show_vote({});
    for (const char* n : {dlg::kDirectConnect, dlg::kPassword, dlg::kConnect, dlg::kDisconnect, dlg::kVote}) CHECK(gui.find(n) != nullptr);
    lob.close_all();
    CHECK(gui.open_count() == 0);
    // P4 completion 2: the remaining screens on the shipped script.
    {
        InfoScreens is;
        is.init(gui);
        CHECK(is.show_buy_units({1000, {{"X", U"X", 10, 0, "", true}}}) && is.show_story({}) && is.show_story_summary({}));
        // GALACTIC_DIALOGS Â§2.2: chapter TITLE in the IDC_STATIC above the recap, newest chapter preselected, Completed
        // disabled with nothing completed, Abandon hidden.
        {
            StorySummaryInfo ss;
            ss.goals = {U"Chapter one", U"Heart of the Maw"};
            ss.recap = {U"Body", U"Empire Will Achieve Victory If:"};
            ss.recap_colors = {Rgba8{255, 255, 255, 176}, Rgba8{255, 24, 39, 255}};
            ss.title = U"Incoming Transmission from Darth Vader";
            ss.has_completed = false;
            ss.goal_colors = {Rgba8{128, 128, 255, 255}, Rgba8{84, 84, 168, 255}};  // q6 t14: per-row colours (read: x 0.66)
            CHECK(is.show_story_summary(ss));
            if (const GuiDialog* sd = gui.find(dlg::kStorySummary))
                if (const GuiControl* gl = sd->get("IDC_GOAL_LIST"))
                    CHECK(gl->item_text_colors.size() == 2 && gl->item_text_colors[1].r == 84 && gl->item_text_colors[1].b == 168);
            const GuiDialog* d = gui.find(dlg::kStorySummary);
            CHECK(d != nullptr);
            if (d) {
                bool title = false;
                for (size_t i = 0; i < d->size(); ++i)
                    title = title || ((*d)[i].def && (*d)[i].def->id == "IDC_STATIC" && (*d)[i].text == ss.title);
                const GuiControl* goals = d->get("IDC_GOAL_LIST");
                const GuiControl* done = d->get("IDC_RADIO_COMPLETED");
                const GuiControl* ab = d->get("IDC_SUMMARY_DIALOG_ABANDON");
                const GuiControl* recap = d->get("IDC_STORY_RECAP");
                CHECK(title && goals && goals->selected == 1 && done && !done->enabled && (!ab || !ab->visible) && recap &&
                      !recap->item_text_colors.empty() && recap->item_text_colors.back().r == 255 &&
                      recap->item_text_colors.back().g == 24);
            }
        }
        size_t tech_items = 0;
        TechTreeInfo tt;
        tt.item = [&](std::string_view, TechTreeItem& o) {
            ++tech_items;
            o.icon = "i_x.tga";
            return true;
        };
        for (const char* f : {"IDD_TECH_TREE_REBEL", "IDD_TECH_TREE_EMPIRE", "IDD_TECH_TREE_BLACKMARKET"})
            for (bool space : {false, true}) {
                tt.dialog = f;
                tt.space = space;
                is.set_tech_tree(tt);
                CHECK(is.show_info(InfoPage::TechTree) && is.info_dialog() == tech_tree_dialog(f, space));
            }
        for (InfoPage p : {InfoPage::DroidLog, InfoPage::DataChart, InfoPage::SummaryProduction, InfoPage::SummaryFleets,
                           InfoPage::SummaryEconomy})
            CHECK(is.show_info(p));
        std::printf("real info screens: %zu tech tree items over the 6 tech tree dialogs\n", tech_items);
        CHECK(tech_items > 100);
        is.close_all();
        CHECK(gui.open_count() == 0);
    }
    // Mouse pointers: MousePointers.xml + frame textures in the game data.
    {
        std::vector<uint8_t> bytes;
        CHECK(files->read("Data\\XML\\MousePointers.xml", bytes));
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, "MousePointers.xml");
        CursorTable ct;
        const size_t n = ct.parse(doc.root());
        size_t known = 0;
        for (const CursorDef& d : ct.defs()) {
            Pointer p;
            known += pointer_from_name(d.name, p) ? 1u : 0u;
        }
        const size_t frames = ct.resolve_frames([&](std::string_view t) {
            const std::string s(t);
            return files->exists("Data\\Art\\Textures\\" + s) ||
                   files->exists("Data\\Art\\Textures\\" + s.substr(0, s.find_last_of('.')) + ".dds");
        });
        std::printf("real cursor: %zu pointers (%zu known), %zu frames\n", n, known, frames);
        CHECK(n == kPointerCount && known == kPointerCount && frames > n);
    }
    // Every panel component resolves in the shipped CommandBarComponents.
    ComponentTable table;
    CHECK(table.load(*files, &err));
    HeroPanel hp;
    BlackMarketPanel bp;
    SabotagePanel sp;
    CorruptionPanel cp;
    CHECK(hp.init(table, atlas, fonts) && bp.init(table, atlas, fonts) && sp.init(table, atlas, fonts) && cp.init(table, atlas, fonts));
    size_t missing = 0;
    auto need = [&](const std::string& n) {
        if (table.find(n) < 0) {
            ++missing;
            std::printf("  missing component %s\n", n.c_str());
        }
    };
    for (const char* s : {"good", "evil"})
        for (int k = 0; k <= 10; ++k) {
            char b[40];
            std::snprintf(b, sizeof b, "hero_slot_%s%02d", s, k);
            need(b);
            std::snprintf(b, sizeof b, "hero_hb_%s%02d", s, k);
            need(b);
        }
    for (int k = 0; k < 5; ++k) {
        char b[40];
        std::snprintf(b, sizeof b, "market_%02d", k);
        need(b);
        std::snprintf(b, sizeof b, "market_tech_level%02d", k);
        need(b);
    }
    for (int k = 0; k < 14; ++k) {
        char b[40];
        std::snprintf(b, sizeof b, "market_title%02d", k);
        need(b);
    }
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 2; ++c) {
            char b[40];
            std::snprintf(b, sizeof b, "bm_%02d0%d", r, c);
            need(b);
            std::snprintf(b, sizeof b, "bm_title_%d0%d", r, c);
            need(b);
            std::snprintf(b, sizeof b, "bm_title_40%d", r * 2 + c + 2);
            need(b);
        }
    for (const char* x : {"a", "b", "c"}) {
        need(std::string("corrupt_button_icon_") + x);
        need(std::string("corrupt_title_") + x);
        need(std::string("corrupt_cost_") + x);
        for (int n = 0; n < 5; ++n) need("corrupt_text0" + std::to_string(n) + x);
        for (int n = 0; n < 8; ++n) need("corrupt_icon_0" + std::to_string(n) + x);
    }
    for (const char* n : {"encyclopedia_back", "encyclopedia_header_text", "encyclopedia_text", "encyclopedia_right_text",
                          "encyclopedia_center_text", "encyclopedia_cost_text", "encyclopedia_icon", "zoomed_back", "zoomed_text",
                          "market_close", "bm_close", "corrupt_close", "hack_shell", "hack_title01", "hack_cost", "hack_hack",
                          "hack_close", "q1_fleet", "q2_fleet", "q3_fleet"})
        need(n);
    for (int k = 0; k < 7; ++k) need("hack_text_0" + std::to_string(k));
    HackPanel hk;
    CHECK(hk.init(table, atlas, fonts));
    size_t animated = 0;
    for (const CommandBarComponent& c : table.items()) animated += c.can_animate && !c.icon_alternate_texture.empty() ? 1u : 0u;
    std::printf("real panels: %zu Can_Animate components with frames\n", animated);
    CHECK(animated >= 60);
    std::printf("real panels: %zu components, %zu panel components missing\n", table.size(), missing);
    CHECK(missing == 0);
}
#else
void test_real_data() { std::printf("real screens: alamo_vfs not configured, skipped\n"); }
#endif

}  // namespace test_gui_screens_t

using namespace test_gui_screens_t;


// P2 completion: targeting controller (BeginAbilityTargeting / BeginReinforcement), radar jump, the galactic
// g_special_ability rows, and the S9 ability feed.
InputFrame mouse_frame(int32_t x, int32_t y, uint8_t down, uint8_t pressed, uint8_t released, double time) {
    InputFrame f;
    f.time = time;
    f.dt = 1.0f / 30.0f;
    f.width = 800;
    f.height = 600;
    f.mouse_x = x;
    f.mouse_y = y;
    f.buttons_down = down;
    f.buttons_pressed = pressed;
    f.buttons_released = released;
    return f;
}

void test_targeting() {
    CHECK(ability_target_kind(30) == TargetKind::Object && ability_target_kind(14) == TargetKind::Point &&
          ability_target_kind(26) == TargetKind::PointMode && ability_target_kind(1) == TargetKind::None &&
          ability_target_kind(17) == TargetKind::Point && ability_target_kind(6) == TargetKind::Object);
    TacticalCameraController cam;  // space defaults: the screen centre looks at the origin
    OrderContext ctx;
    ctx.mode = UiMode::Space;
    ctx.local_player = 0;
    ctx.view = make_view(cam.params(800.0f / 600.0f), 800, 600);
    tactical::TreeSet trees(2);
    tactical::TreeMembership m;
    m.owner = 1;
    CHECK(trees.add(static_cast<tactical::EntityHandle>(77), {{-20, -20, -20}, {20, 20, 20}}, 1u, m));
    ctx.trees = &trees;
    const uint8_t L = button_bit(Button::Left), R = button_bit(Button::Right);
    Bindings keys;
    auto click = [&](TargetingController& tc, InputState& in, int32_t x, int32_t y, uint8_t bit, double t, std::vector<UiOrder>& out) {
        in.update(mouse_frame(x, y, bit, bit, 0, t));
        const bool a = tc.update(in, keys, ctx, out);
        in.update(mouse_frame(x, y, 0, 0, bit, t + 0.05));
        const bool b = tc.update(in, keys, ctx, out);
        return a && b;
    };
    // Object ability (TRACTOR_BEAM 30): the click on the enemy at the origin -> command 40, queued from the HUD click.
    {
        TargetingController tc;
        InputState in;
        HudAction a;
        a.kind = HudAction::Kind::BeginAbilityTargeting;
        a.ability_type = 30;
        a.ids = {5, 6};
        a.queued = true;
        CHECK(tc.begin(a) && tc.active() && tc.target_kind() == TargetKind::Object);
        std::vector<UiOrder> out;
        CHECK(click(tc, in, 400, 300, L, 1.0, out));
        CHECK(out.size() == 1 && out[0].queued && out[0].event == commands::ev::ability({5, 6}, 30, 77) && !tc.active() && tc.issued() == 1);
        HudAction other;
        other.kind = HudAction::Kind::RadarPing;
        CHECK(!tc.begin(other) && !tc.active());
    }
    // Point ability (14 Force Whirlwind): command 57 at the ground point; a click on empty sky keeps waiting.
    {
        TargetingController tc;
        InputState in;
        HudAction a;
        a.kind = HudAction::Kind::BeginAbilityTargeting;
        a.ability_type = 14;
        a.ids = {5};
        CHECK(tc.begin(a));
        std::vector<UiOrder> out;
        CHECK(click(tc, in, 400, 300, L, 1.0, out));
        CHECK(out.size() == 1 && !out[0].queued && out[0].event.type() == commands::EventType::TacticalSpecialAbilityWithDummyTarget &&
              out[0].event.u("ability") == 14 && std::fabs(out[0].event.f("x")) < 1.0f && std::fabs(out[0].event.f("y")) < 1.0f &&
              out[0].event.ids("users") == std::vector<uint32_t>{5});
        // position via 43 (26 Distract): SetUnitAbilityMode on, with the point
        a.ability_type = 26;
        CHECK(tc.begin(a) && tc.target_kind() == TargetKind::PointMode);
        out.clear();
        CHECK(click(tc, in, 400, 300, L, 2.0, out));
        CHECK(out.size() == 1 && out[0].event.type() == commands::EventType::SetUnitAbilityMode && out[0].event.u("on") == 1 &&
              out[0].event.u("ability") == 26);
        // right click cancels
        CHECK(tc.begin(a));
        out.clear();
        CHECK(click(tc, in, 400, 300, R, 3.0, out));
        CHECK(out.empty() && !tc.active() && tc.cancelled() == 1);
        // not active: the world input is not taken
        in.update(mouse_frame(400, 300, L, L, 0, 4.0));
        CHECK(!tc.update(in, keys, ctx, out));
    }
    // Reinforcement drop: Reinforce at the ground point.
    {
        TargetingController tc;
        InputState in;
        HudAction a;
        a.kind = HudAction::Kind::BeginReinforcement;
        a.unit = 4242;
        CHECK(tc.begin(a) && tc.mode() == TargetingController::Mode::Reinforcement);
        std::vector<UiOrder> out;
        CHECK(click(tc, in, 400, 300, L, 1.0, out));
        CHECK(out.size() == 1 && out[0].event.type() == commands::EventType::Reinforce && out[0].event.u("unit") == 4242 && !tc.active());
    }
    // Host flow (apps/game battle_input): the router gives a running pick the presses (the HUD and the world see none of
    // them), the controller reads the RAW frame through its own InputState; the next press is the world's again.
    {
        TargetingController tc;
        InputRouter router;
        RouteHooks hooks;
        hooks.targeting_active = [&] { return tc.active(); };
        hooks.hud_hit = [](int32_t x, int32_t) { return x < 100 ? 7 : -1; };  // a HUD strip at the left edge
        RoutedInput routed;
        InputState raw_in, world_in;
        std::vector<UiOrder> out;
        bool world_click = false;
        auto frame = [&](int32_t x, int32_t y, uint8_t down, uint8_t pressed, uint8_t released, double t, bool esc = false) {
            InputFrame f = mouse_frame(x, y, down, pressed, released, t);
            if (esc) {
                f.keys_down.set(vk::kEscape);
                f.keys_pressed.set(vk::kEscape);
            }
            router.route(f, hooks, routed);
            raw_in.update(f);
            world_in.update(routed.world);
            world_click = world_click || world_in.button(Button::Left).click || world_in.button(Button::Right).click;
            return tc.update(raw_in, keys, ctx, out);
        };
        // no pick running: a click on the strip belongs to the HUD
        frame(50, 300, L, L, 0, 1.0);
        frame(50, 300, 0, 0, L, 1.05);
        CHECK(routed.hud.size() == 2 && routed.hud[1].kind == HudPointerEvent::Kind::Click && !tc.active());
        HudAction a;
        a.kind = HudAction::Kind::BeginAbilityTargeting;
        a.ability_type = 30;
        a.value = -1;
        a.ids = {5};
        CHECK(tc.begin(a));
        // the pick runs: a click on the strip is the pick's (nothing under it: it keeps waiting), the HUD hears nothing
        CHECK(frame(50, 300, L, L, 0, 2.0) && routed.hud.empty() && routed.owner[0] == PointerTarget::Targeting);
        CHECK(frame(50, 300, 0, 0, L, 2.05) && routed.hud.empty() && tc.active() && out.empty());
        // the click on the enemy: command 40 through the order list, the world saw no click
        CHECK(frame(400, 300, L, L, 0, 3.0));
        CHECK(frame(400, 300, 0, 0, L, 3.05));
        CHECK(out.size() == 1 && out[0].event == commands::ev::ability({5}, 30, 77) && !tc.active() && !world_click);
        // the pick is over: the next press is the world's
        frame(400, 300, L, L, 0, 4.0);
        CHECK(routed.owner[0] == PointerTarget::World && !tc.update(raw_in, keys, ctx, out));
        frame(400, 300, 0, 0, L, 4.05);
        CHECK(world_click);
        // a right click cancels the pick without a world click; so does Esc
        out.clear();
        world_click = false;
        CHECK(tc.begin(a));
        frame(400, 300, R, R, 0, 5.0);
        frame(400, 300, 0, 0, R, 5.05);
        CHECK(!tc.active() && tc.cancelled() >= 1 && out.empty() && !world_click);
        CHECK(tc.begin(a));
        frame(400, 300, 0, 0, 0, 6.0, true);
        CHECK(!tc.active() && out.empty());
    }
    // The pick cursor.
    {
        using T = CursorContext::Targeting;
        CHECK(ability_pick_cursor(TargetKind::Object, 30, true, false) == T::AbilityEnemy &&
              ability_pick_cursor(TargetKind::Object, 30, true, true) == T::AbilityFriendly &&
              ability_pick_cursor(TargetKind::Point, 14, true, false) == T::AbilitySpace &&
              ability_pick_cursor(TargetKind::Point, 14, false, false) == T::AbilityTerrain &&
              ability_pick_cursor(TargetKind::Point, 70, false, false) == T::AbilityBlast &&
              ability_pick_cursor(TargetKind::PointMode, 5, true, false) == T::Barrage);
        CursorContext cc;
        cc.targeting = ability_pick_cursor(TargetKind::Object, 30, true, false);
        cc.target_valid = false;
        CHECK(choose_pointer(cc) == Pointer::AbilityEnemyInvalid);
        cc.target_valid = true;
        CHECK(choose_pointer(cc) == Pointer::AbilityEnemy);
    }
    // Radar camera jump.
    {
        TacticalCameraController c;
        HudAction a;
        a.kind = HudAction::Kind::RadarCameraJump;
        a.x = 1200;
        a.y = -800;
        CHECK(apply_radar_jump(a, c) && c.look_at().x == 1200.0f && c.look_at().y == -800.0f);
        a.kind = HudAction::Kind::RadarPing;
        CHECK(!apply_radar_jump(a, c));
    }

    // Galactic g_special_ability rows: one per provider entry beside the selected planet; a click -> the host action.
    {
        const char* xml = R"(<CommandBarComponents>
  <CommandBarComponent Name="g_special_ability"><Type>TextButton</Type><Blank_Texture_Name>i_row.tga</Blank_Texture_Name>
    <Icon_Alternate_Texture_Name>i_sym_r.tga i_sym_e.tga</Icon_Alternate_Texture_Name><No_Shell>True</No_Shell><Base_Layer>0</Base_Layer>
    <Text_Offset>-18 0</Text_Offset><Offset>-48 -20</Offset><Right_Justified>True</Right_Justified></CommandBarComponent>
</CommandBarComponents>)";
        ComponentTable t;
        const data::XmlDocument d = data::XmlDocument::parse(std::string_view(xml), "s.xml");
        t.parse(d.root());
        TextureAtlas atlas;
        atlas.texture_width = atlas.texture_height = 64;
        atlas.add({"i_row.tga", 0, 0, 16, 16, true});
        atlas.add({"i_sym_r.tga", 16, 0, 12, 12, true});
        atlas.add({"i_sym_e.tga", 32, 0, 12, 12, true});
        auto raster = make_box_rasterizer();
        FontCache fonts(*raster);
        GalacticHud g;
        g.init(t, atlas, fonts);
        g.resize(1024, 768);
        GalacticCameraController gc;
        std::vector<GalacticPlanetInfo> planets(1);
        planets[0].planet = 300;
        planets[0].selected = true;
        GalacticHudFrame f;
        f.view = make_view(gc.params(4.0f / 3.0f), 1024, 768);
        f.planets = planets;
        f.providers.special_abilities = [](uint32_t planet, std::vector<GalacticSpecialAbility>& out) {
            if (planet != 300) return;
            out.push_back({901, U"Black market", "", "", true});
            out.push_back({902, U"Sabotage", "", "", true});
            out.push_back({903, U"Corrupt", "", "", false});
        };
        g.build(f);
        CHECK(g.stats().ability_rows == 3);
        const int32_t sc = t.find("g_special_ability");
        float sx = 0, sy = 0;
        CHECK(project(f.view, planets[0].position, sx, sy));
        const Vec2f at = g.canvas().pixel_to_canvas(sx, sy);
        auto row_px = [&](int i) { return g.canvas().canvas_to_pixel(at.x - 48.0f, at.y - 20.0f * static_cast<float>(i + 1)); };
        auto click_row = [&](int i, std::vector<HudAction>& acts) {
            const Vec2f p = row_px(i);
            const auto x = static_cast<int32_t>(p.x), y = static_cast<int32_t>(p.y);
            CHECK(g.hit_test(x, y) == sc);
            RoutedInput r;
            r.hover_component = sc;
            r.hud = {{HudPointerEvent::Kind::Press, Button::Left, sc, x, y}, {HudPointerEvent::Kind::Release, Button::Left, sc, x, y},
                     {HudPointerEvent::Kind::Click, Button::Left, sc, x, y}};
            std::vector<commands::EventData> cmds;
            g.update_input(r, false, false, 1.0, cmds, &acts);
            CHECK(cmds.empty());
        };
        std::vector<HudAction> acts;
        click_row(1, acts);
        CHECK(acts.size() == 1 && acts[0].kind == HudAction::Kind::Button && acts[0].name == "g_special_ability" && acts[0].unit == 902 &&
              acts[0].value == 1 && acts[0].ids == std::vector<uint32_t>{300});
        acts.clear();
        click_row(2, acts);  // disabled row: no action
        CHECK(acts.empty());
        // frame + faction symbol per row (6 quads) and the texts; nothing without a selected planet
        CHECK(g.draw_list().items(0, HudList::Alpha).size() == 6 && g.draw_list().items(0, HudList::Text).size() > 3);
        planets[0].selected = false;
        g.build(f);
        CHECK(g.stats().ability_rows == 0);
    }

#ifdef ALAMO_UI_HAVE_ABILITIES
    // S9 ability feed: read-only AbilitySystem queries -> HudAbility rows.
    {
        using namespace alamo::tactical::abilities;
        tactical::combat::CombatData cd;
        AbilityData ad;
        tactical::combat::UnitDesc u;
        u.name = "Ship";
        u.tactical_health = 1000.0f;
        u.shield_points = 500.0f;  // DEFEND needs shields (Can_Activate gate 10)
        u.shield_refresh_rate = 10.0f;
        u.radius = 50.0f;
        AbilityObjectDesc od;
        UnitAbilityRecord defend;
        defend.type = kDefend;
        defend.combat.ability_type = kDefend;
        defend.expiration_seconds = 15.0f;
        defend.recharge_seconds = 20.0f;
        UnitAbilityRecord tractor;
        tractor.type = kTractorBeam;
        tractor.combat.ability_type = kTractorBeam;
        od.unit_abilities = {defend, tractor};
        cd.units.push_back(u);
        od.name = u.name;
        link_combat_abilities(od, cd, cd.units.back());
        ad.objects.push_back(od);
        tactical::combat::CombatSystem cs(cd, tactical::combat::CombatConfig{},
                                          std::vector<tactical::combat::PlayerInfo>{tactical::combat::PlayerInfo{}, tactical::combat::PlayerInfo{}});
        AbilitySystem as(cs, ad, AbilityConfig{});
        SimRng rng{77u};
        cs.bind_rng(&rng);
        tactical::combat::UnitSpawn s;
        s.id = 1;
        s.desc = 0;
        s.owner = 0;
        s.facing = {1, 0, 0};
        CHECK(as.add_unit(s, 0, 1, rng));
        tactical::TreeSet strees(2);
        tactical::TreeMembership sm;
        sm.owner = 0;
        CHECK(strees.add(static_cast<tactical::EntityHandle>(1), {{-50, -50, -50}, {50, 50, 50}}, 1u, sm));
        as.tick(1, strees, rng);  // first update (countdown deltas are 1 from here on, as in the S9 tests)
        SelectedGroup grp;
        grp.ids = {1};
        std::vector<HudAbility> out;
        abilities_from_s9(as, grp, 2, out);
        CHECK(out.size() == 2 && out[0].ability_type == kDefend && out[1].ability_type == kTractorBeam);
        CHECK(out.size() == 2 && !out[0].targeted && out[1].targeted && out[1].target_kind == static_cast<uint8_t>(TargetKind::Object) &&
              out[0].icon == "i_sa_defend_mode.tga" && out[0].recharge == 1.0f && !out[0].active && out[0].enabled);
        const tactical::combat::ObjectId ids[] = {1};
        CHECK(as.set_unit_ability_mode(ids, kDefend, true, 2) == 1);
        out.clear();
        abilities_from_s9(as, grp, 2, out);
        CHECK(!out.empty() && out[0].active && out[0].enabled);
        for (uint32_t fr = 2; fr < 32; ++fr) as.tick(fr, strees, rng);  // 30 active frames
        CHECK(as.set_unit_ability_mode(ids, kDefend, false, 32) == 1);  // early off: recharging
        out.clear();
        auto provider = s9_ability_provider(as, [] { return 32u; });
        provider(grp, out);
        CHECK(!out.empty() && !out[0].active && out[0].recharge < 1.0f);
        // t42: a record's Alternate_Icon_Name replaces the type's art on the button (the parse: test_abilities).
        CHECK(ad.objects[0].unit_abilities[0].type == kDefend);
        ad.objects[0].unit_abilities[0].alternate_icon_name = "i_sa_strike.tga";
        out.clear();
        abilities_from_s9(as, grp, 32, out);
        CHECK(!out.empty() && out[0].icon == "i_sa_strike.tga" && out.size() == 2 && out[1].icon == "i_sa_tractor_beam.tga");
        // QA D5: the i_sa_* art of MT_CommandBar, by mapping or by type name.
        CHECK(ability_icon(0).empty() && ability_icon(kDefend) == "i_sa_defend_mode.tga" &&
              ability_icon(kTractorBeam) == "i_sa_tractor_beam.tga" && ability_icon(kLure) == "i_sa_lure.tga" &&
              ability_icon(kStun) == "i_sa_stun.tga");
        std::printf("targeting: S9 feed %zu abilities for the test ship\n", out.size());
    }
#endif
}

namespace test_gui_screens_t {
void test_skirmish_maps();  // test_gui_skirmish_maps.cpp (p4ui t06)
void test_skirmish_preview();  // p4uim t03 / t04
void test_skirmish_colours();  // p4uia t01 / t02
}

int main() {
    test_skirmish_maps();
    test_skirmish_preview();
    test_skirmish_colours();
    test_screens();
    test_advanced_video();
    test_video_vsync_mode();
    test_video_port_text();
    test_resolution_confirm();
    test_options_polish();
    test_advanced_video_port();
    test_lobby();
    test_panels();
    test_encyclopedia_fade_anchor();
    test_camera_hotkeys();
    test_info_screens();
    test_cursor();
    test_anim_hack_drag();
    test_controls();
    test_targeting();
    test_real_data();
    if (g_fail) {
        std::printf("alamo_ui_screens_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_ui_screens_tests: all passed\n");
    return 0;
}

// test_gui_screens: tests split from test_gui_screens.cpp (PAF t08).
#include "test_gui_screens_common.hpp"

namespace test_gui_screens_t {

// ---- in-game screens ------------------------------------------------------------------------------------------
void test_screens() {
    Env env;
    GameScreens gs;
    gs.init(env.gui);
    std::vector<ScreenAction> acts;
    std::vector<commands::EventData> cmds;

    // Message box: 2 buttons -> IDD_DIALOG_BOX2, real click on the right button -> result 1.
    MessageBoxSpec m;
    m.tag = "quit?";
    m.caption = U"Leave the battle?";
    m.lines = {U"Unsaved progress will be lost."};
    m.buttons = {U"Yes", U"No"};
    CHECK(gs.show_message(m));
    CHECK(gs.message_dialog() == "IDD_DIALOG_BOX2");
    const GuiDialog* mb = env.gui.find("IDD_DIALOG_BOX2");
    CHECK(mb && mb->get("IDC_CAPTION")->text == U"Leave the battle?" && mb->get("IDC_DIALOG_BOX_BUTTON_RIGHT")->text == U"No" &&
          mb->get("IDC_USER_TEXT_STRING_1")->visible && !mb->get("IDC_USER_TEXT_STRING_2")->visible);
    gs.update(env.click("IDD_DIALOG_BOX2", "IDC_DIALOG_BOX_BUTTON_RIGHT"), acts, cmds);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::MessageBoxResult && acts[0].tag == "quit?" && acts[0].button == 1);
    CHECK(env.gui.find("IDD_DIALOG_BOX2") == nullptr && gs.message_dialog().empty());
    // Esc -> -1; a 3-button box missing from this script -> false.
    CHECK(gs.show_message(m));
    acts.clear();
    gs.update(env.frame({5, 5}, false, {vk::kEscape}), acts, cmds);
    CHECK(acts.size() == 1 && acts[0].button == -1);
    m.buttons.push_back(U"Cancel");
    CHECK(!gs.show_message(m));

    // Battle end: title from TEXT_VICTORY, losses with counts, invade hidden, countdown, scrolling, continue.
    BattleEndInfo be;
    be.victory = true;
    be.planet = U"Kuat";
    be.time = U"12:34";
    be.bounty = 1500;
    for (int i = 0; i < 15; ++i) be.your_losses.push_back({"i_button_tie.tga", U"TIE " + std::u32string(1, static_cast<char32_t>(U'A' + static_cast<char32_t>(i))), static_cast<uint32_t>(i % 3 + 1)});
    be.enemy_losses = {{"i_button_xwing.tga", U"X-Wing", 4}};
    be.countdown = 9.2f;
    CHECK(gs.show_battle_end(be));
    GuiDialog* bd = env.gui.find(dlg::kBattleEnd);
    if (bd) {  // QA U2 / U3: names wrap in a box as tall as the icon; without heroes the rows sit right under the header
        const size_t icon0 = static_cast<size_t>(bd->find("IDC_YOUR_UNIT0")), name0 = static_cast<size_t>(bd->find("IDC_YOUR_UNIT_NAME0"));
        const GuiRect ir = bd->control_rect(icon0), nr = bd->control_rect(name0);
        CHECK((*bd)[name0].fit_lines == 2 && std::abs(nr.max.y - ir.max.y) < 0.01f && std::abs(nr.height() - ir.height()) < 0.01f);
        const float top = bd->origin().y - static_cast<float>((*bd)[icon0].def->y);
        CHECK(std::abs(ir.max.y - (top + kBattleEndRowsUp)) < 0.01f);
    }
    {
        BattleEndInfo raw = be;
        raw.planet = U"_Space_Planet_Felucia_01.ted";
        raw.your_heroes = {"i_button_vader.tga"};
        CHECK(gs.show_battle_end(raw));
        GuiDialog* rd = env.gui.find(dlg::kBattleEnd);
        CHECK(rd && rd->get("IDC_BATTLE_END_PLANET_STATIC")->text.empty() && rd->get("IDC_YOUR_UNIT0")->offset.y == 0.0f &&
              rd->get("IDC_ENEMY_UNIT0")->offset.y == kBattleEndRowsUp);
        CHECK(gs.show_battle_end(be));
    }
    CHECK(bd && bd->get("IDC_TITLE_STATIC")->text == U"Victory!" && bd->get("IDC_YOUR_UNIT_NAME0")->text == U"TIE A" &&
          bd->get("IDC_YOUR_UNIT_NAME1")->text == U"TIE B x2" && bd->get("IDC_ENEMY_UNIT_NAME0")->text == U"X-Wing x4" &&
          bd->get("IDC_YOUR_UNIT_NAME0")->fit_keep_tail == 0 && bd->get("IDC_YOUR_UNIT_NAME1")->fit_keep_tail == 3 &&  // t47
          bd->get("IDC_YOUR_UNIT0")->image == "i_button_tie.tga" && !bd->get("IDC_LAND_INVADE_BUTTON")->visible &&
          bd->get("IDC_COUNTDOWN_TIMER")->text == U"10" && bd->get("IDC_STATIC_BOUNTY")->text == U"1500" &&
          bd->get("IDC_YOUR_SCROLLBAR")->visible);
    acts.clear();
    gs.update(std::vector<GuiEvent>{event(GuiEvent::Kind::ValueChanged, dlg::kBattleEnd, "IDC_YOUR_SCROLLBAR", 100)}, acts, cmds);
    CHECK(gs.battle_end_offset(false) == 3 && bd->get("IDC_YOUR_UNIT_NAME0")->text == U"TIE D");
    gs.set_countdown(-1.0f);
    CHECK(!bd->get("IDC_COUNTDOWN_TIMER")->visible);
    gs.update(env.click(dlg::kBattleEnd, "IDC_SAVE_REPLAY_BUTTON"), acts, cmds);
    gs.update(env.click(dlg::kBattleEnd, "IDC_CONTINUE_BUTTON"), acts, cmds);

    // Playtest 2 t13: auto-resolve results figures. Battle time from seconds (TEXT_BATTLE_TIME without a number is followed by
    // hh:mm:ss; a pattern with %02d takes the parts), bounty through TEXT_SUMMARY_BOUNTY_COLLECTED, loss values and
    // efficiencies in their own controls where the template has them (yours) and appended to the loss header where not (enemy).
    {
        BattleEndInfo ar;
        ar.victory = true;
        ar.planet = U"Kuat";
        ar.battle_seconds = 3725;
        ar.bounty = 80;
        ar.your_loss_value = 200;
        ar.enemy_loss_value = 500;
        ar.your_efficiency = 71;
        ar.enemy_efficiency = 29;
        CHECK(gs.show_battle_end(ar));
        GuiDialog* d = env.gui.find(dlg::kBattleEnd);
        CHECK(d && d->get("IDC_TIME_STATIC")->text == U"Battle Time: 01:02:05" && d->get("IDC_STATIC_BOUNTY")->text == U"80");
        CHECK(d->get("IDC_YOUR_LOSS_VAL_STATIC")->text == U"$ 200" && d->get("IDC_EFFICIENCY_STATIC")->text == U"71%");
        CHECK(d->get("IDC_STATIC_LOSS1")->text == U"TEXT_YOUR_LOSSES");  // yours: own controls, header untouched
        CHECK(d->get("IDC_STATIC_LOSS2")->text == U"TEXT_ENEMY_LOSSES   $ 500   29%");
        env.text.add("TEXT_BATTLE_TIME", u"Time %02d:%02d:%02d");
        env.text.add("TEXT_SUMMARY_BOUNTY_COLLECTED", u"Salvage collected: $ %d");
        CHECK(gs.show_battle_end(ar));  // shown again without closing: the appended header text is replaced, not stacked
        d = env.gui.find(dlg::kBattleEnd);
        CHECK(d->get("IDC_TIME_STATIC")->text == U"Time 01:02:05" && d->get("IDC_STATIC_BOUNTY")->text == U"Salvage collected: $ 80");
        CHECK(d->get("IDC_STATIC_LOSS2")->text == U"TEXT_ENEMY_LOSSES   $ 500   29%");
        ar.battle_seconds = 59;
        ar.time = U"explicit";  // an explicit time text wins
        CHECK(gs.show_battle_end(ar) && env.gui.find(dlg::kBattleEnd)->get("IDC_TIME_STATIC")->text == U"explicit");
        BattleEndInfo plain;  // nothing computed: the old behaviour (bounty hidden, header untouched)
        plain.time = U"12:34";
        CHECK(gs.show_battle_end(plain));
        d = env.gui.find(dlg::kBattleEnd);
        CHECK(!d->get("IDC_STATIC_BOUNTY")->visible && d->get("IDC_STATIC_LOSS2")->text == U"TEXT_ENEMY_LOSSES");
        gs.close_all();
    }
    CHECK(acts.size() == 2 && acts[0].kind == ScreenAction::Kind::SaveReplay && acts[1].kind == ScreenAction::Kind::BattleEndContinue);
    CHECK(env.gui.find(dlg::kBattleEnd) == nullptr && cmds.empty());

    // Retreat: the only screen click that becomes a command.
    CHECK(gs.show_retreat_confirm());
    acts.clear();
    gs.update(env.click(dlg::kRetreat, "IDC_RETREAT"), acts, cmds);
    CHECK(cmds.size() == 1 && cmds[0] == commands::ev::retreat(true) && acts.size() == 1 &&
          acts[0].kind == ScreenAction::Kind::RetreatConfirmed);
    CHECK(gs.show_retreat_confirm());
    gs.update(env.click(dlg::kRetreat, "IDC_AUTORESOLVE"), acts, cmds);
    CHECK(acts.size() == 2 && acts[1].kind == ScreenAction::Kind::AutoResolve && cmds.size() == 1);

    // Chat: typed text + Enter -> SendChat (team only checked), Esc -> ChatClosed.
    CHECK(gs.open_chat(true) && gs.chat_open());
    acts.clear();
    gs.update(env.frame({5, 5}, false, {}, U"gg wp"), acts, cmds);
    gs.update(env.frame({5, 5}, false, {vk::kReturn}), acts, cmds);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::SendChat && acts[0].text == U"gg wp" && acts[0].team_only);
    CHECK(!gs.chat_open());
    CHECK(gs.open_chat());
    gs.update(env.frame({5, 5}, false, {vk::kEscape}), acts, cmds);
    CHECK(acts.size() == 2 && acts[1].kind == ScreenAction::Kind::ChatClosed);

    // Map selection: double click -> MapSelected.
    const SkirmishMapInfo maps[2] = {{"_SPACE_PLANET_KUAT_01", U"Kuat", 2}, {"_LAND_PLANET_TATOOINE_01", U"Tatooine", 4}};
    CHECK(gs.show_select_map(maps));
    acts.clear();
    gs.update(std::vector<GuiEvent>{event(GuiEvent::Kind::DoubleClicked, dlg::kSelectMap, "IDC_SAVED_MAPS_LIST", 1)}, acts, cmds);
    // the selection is the list control's state (row 0 preselected; the manager selects the row before a double click)
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::MapSelected && acts[0].map == "_SPACE_PLANET_KUAT_01");
    acts.clear();
    CHECK(gs.show_select_map(maps));
    env.gui.find(dlg::kSelectMap)->set_selected("IDC_SAVED_MAPS_LIST", 1);
    gs.update(env.click(dlg::kSelectMap, "IDC_LOAD_MAP_BUTTON"), acts, cmds);
    CHECK(acts.size() == 1 && acts[0].kind == ScreenAction::Kind::MapSelected && acts[0].map == "_LAND_PLANET_TATOOINE_01");
    // Screens not in this script fail cleanly.
    CHECK(!gs.show_auto_resolve({}) && !gs.show_campaign_score({}));
    for (int k = 0; k < 14; ++k) std::printf("%s%s", k ? " " : "screen actions: ", screen_action_name(static_cast<ScreenAction::Kind>(k)));
    std::printf("\n");
}

// ---- advanced video options in the options flow -------------------------------------------------------------------
void test_advanced_video() {
    Env env;
    FrontEnd fe;
    MenuProviders p;
    AdvancedVideoSettings cur;
    cur.particle = 40;
    cur.bloom = false;
    p.advanced_video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    env.gui.open("IDD_VIDEO_OPTIONS_DIALOG");
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    CHECK(fe.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
    GuiDialog* d = env.gui.top();
    CHECK(d && d->get("IDC_PARTICLE_DETAIL_SLIDER")->value == 40 && !d->get("IDC_BLOOM")->checked);
    d->set_value("IDC_PARTICLE_DETAIL_SLIDER", 75);
    d->set_checked("IDC_BLOOM", true);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_OK")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyAdvancedVideo && acts[0].advanced.particle == 75 &&
          acts[0].advanced.bloom && acts[0].advanced.mesh == 100);
    CHECK(fe.top() == "IDD_VIDEO_OPTIONS_DIALOG");
    CHECK(std::string(menu_action_name(MenuAction::Kind::AutoDetectVideo)) == "AutoDetectVideo");
}

// ---- t12 (playtest 3): "keep these settings?" after a resolution change ------------------------------------------
void test_resolution_confirm() {
    Env env;
    env.text.add("TEXT_RESOLUTION_CHANGED_CONFIRMATION", u"Your screen resolution has been changed.  Do you want to keep these settings?");
    env.text.add("TEXT_REVERTING_IN_X_SECONDS", u"Reverting in %d seconds.");
    env.text.add("TEXT_BUTTON_YES", u"Yes");
    env.text.add("TEXT_BUTTON_NO", u"No");
    FrontEnd fe;
    MenuProviders p;
    VideoSettings cur;  // what the host reports: the mode in use
    cur.resolutions = {U"800 x 600 (60Hz)", U"1280 x 720 (60Hz)", U"1920 x 1080 (60Hz)", U"2560 x 1440 (144Hz)"};
    cur.resolution = 1;
    p.video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    InputFrame raw;
    raw.time = 100.0;
    auto open_video = [&] {
        env.gui.close_all();
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO")}, raw, acts);
        return env.gui.top();
    };
    auto click_video = [&](const char* id, int32_t pick) {
        GuiDialog* v = env.gui.find("IDD_VIDEO_OPTIONS_DIALOG");
        if (v) v->set_selected("IDC_RESOLUTION_LIST", pick);
        acts.clear();
        fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", id)}, raw, acts);
    };
    auto line2 = [&]() -> std::u32string {
        const GuiDialog* c = env.gui.find("IDD_DIALOG_BOX2_CENTERED");
        return c ? c->get("IDC_USER_TEXT_STRING_2")->text : std::u32string();
    };

    GuiDialog* d = open_video();
    CHECK(fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" && d && d->get("IDC_RESOLUTION_LIST")->selected == 1);
    // Applying the mode already in use asks nothing.
    click_video("IDC_APPLY", 1);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyVideo && !acts[0].video.unconfirmed && !fe.resolution_confirm_open());

    // A new resolution: applied tentatively, the box opens with the sentence, the buttons and a 15 s countdown.
    click_video("IDC_APPLY", 3);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 3 && acts[0].video.unconfirmed);
    CHECK(fe.resolution_confirm_open() && fe.top() == "IDD_DIALOG_BOX2_CENTERED");
    const GuiDialog* c = env.gui.top();
    CHECK(c->get("IDC_USER_TEXT_STRING_1")->text == U"Your screen resolution has been changed.  Do you want to keep these settings?" &&
          c->get("IDC_DIALOG_BOX_BUTTON_LEFT")->text == U"Yes" && c->get("IDC_DIALOG_BOX_BUTTON_RIGHT")->text == U"No" &&
          !c->get("IDC_USER_TEXT_STRING_3")->visible);
    CHECK(line2() == U"Reverting in 15 seconds." && std::fabs(fe.resolution_confirm_remaining() - kResolutionConfirmSeconds) < 1e-9);
    // The countdown follows the frame clock and sends nothing while it runs.
    acts.clear();
    raw.time = 105.2;
    fe.update({}, raw, acts);
    CHECK(acts.empty() && line2() == U"Reverting in 10 seconds." && fe.resolution_confirm_open());
    raw.time = 114.9;
    fe.update({}, raw, acts);
    CHECK(acts.empty() && line2() == U"Reverting in 1 seconds.");
    // Timeout: the previous resolution is applied again (confirmed, so the host stores it); the Apply dialog shows it selected.
    raw.time = 115.0;
    fe.update({}, raw, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyVideo && acts[0].video.resolution == 1 && !acts[0].video.unconfirmed);
    CHECK(!fe.resolution_confirm_open() && fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" &&
          env.gui.top()->get("IDC_RESOLUTION_LIST")->selected == 1);

    // Yes keeps: the same settings again, confirmed; the dialog closes and stays closed.
    raw.time = 200.0;
    click_video("IDC_APPLY", 2);
    CHECK(acts.size() == 1 && acts[0].video.unconfirmed && fe.resolution_confirm_open());
    acts.clear();
    raw.time = 201.0;
    fe.update(env.click("IDD_DIALOG_BOX2_CENTERED", "IDC_DIALOG_BOX_BUTTON_LEFT"), raw, acts);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 2 && !acts[0].video.unconfirmed && !fe.resolution_confirm_open() &&
          fe.top() == "IDD_VIDEO_OPTIONS_DIALOG");
    cur.resolution = 2;  // the host now reports the kept mode
    acts.clear();
    raw.time = 230.0;
    fe.update({}, raw, acts);
    CHECK(acts.empty());  // the old deadline is gone

    // No reverts at once. OK closes the video dialog first; the box then sits over the options.
    open_video();
    raw.time = 300.0;
    click_video("IDC_OK", 0);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 0 && acts[0].video.unconfirmed && fe.top() == "IDD_DIALOG_BOX2_CENTERED" &&
          env.gui.find("IDD_VIDEO_OPTIONS_DIALOG") == nullptr);
    acts.clear();
    raw.time = 302.0;
    fe.update(env.click("IDD_DIALOG_BOX2_CENTERED", "IDC_DIALOG_BOX_BUTTON_RIGHT"), raw, acts);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 2 && !acts[0].video.unconfirmed && !fe.resolution_confirm_open() &&
          env.gui.find("IDD_DIALOG_BOX2_CENTERED") == nullptr);

    // Esc is No.
    open_video();
    raw.time = 400.0;
    click_video("IDC_APPLY", 3);
    CHECK(fe.resolution_confirm_open());
    acts.clear();
    raw.time = 401.0;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Cancel, "IDD_DIALOG_BOX2_CENTERED", "")}, raw, acts);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 2 && !acts[0].video.unconfirmed && !fe.resolution_confirm_open() &&
          fe.top() == "IDD_VIDEO_OPTIONS_DIALOG");
    // Other settings travel with the answer: only the resolution is rolled back.
    cur.vsync = true;
    raw.time = 500.0;
    GuiDialog* v = env.gui.top();
    v->set_checked("IDC_VSYNC_ENABLED", true);
    click_video("IDC_APPLY", 0);
    CHECK(acts.size() == 1 && acts[0].video.vsync);
    acts.clear();
    raw.time = 600.0;
    fe.update({}, raw, acts);
    CHECK(acts.size() == 1 && acts[0].video.resolution == 2 && acts[0].video.vsync);
}

// ---- t47: the captions stop at the value column; port strings take the pseudo-localization path ----------------------
void test_video_port_text() {
    Env env;
    env.text.pseudo_localize_all();
    CHECK(env.script.extend(kPortDialogExtensionsRc));
    CHECK(env.script.extend(R"RC(
IDD_VIDEO_OPTIONS_DIALOG DIALOGEX 0, 0, 570, 454
BEGIN
    LTEXT           "TEXT_ANTI_ALIASING",IDC_ANTI_ALIAS_STATIC,41,245,146,21
    LTEXT           "TEXT_DETAIL_LEVEL",IDC_STATIC,41,271,146,21
    LTEXT           "TEXT_OFF",IDC_ANTI_ALIAS_OFF_STATIC,190,246,63,21
END
)RC"));
    FrontEnd fe;
    MenuProviders p;
    VideoSettings cur;
    cur.display_info = U"160 Hz";
    p.video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO")}, InputFrame{}, acts);
    GuiDialog* d = fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d != nullptr);
    if (!d) return;
    auto pseudo = [](const char32_t* s) {
        std::u16string u;
        for (; *s; ++s) u.push_back(static_cast<char16_t>(*s));
        return utf16_to_utf32(pseudo_localize(u));
    };
    CHECK(d->get("IDC_PORT_VSYNC_MODE_TEXT")->text == pseudo(U"V-Sync Mode") && d->get("IDC_PORT_FRAME_CAP_TEXT")->text == pseudo(U"Frame Rate Cap"));
    CHECK(d->get("IDC_PORT_VSYNC_MODE")->items[1] == pseudo(U"Standard (FIFO)") && d->get("IDC_PORT_FRAME_CAP")->items[1] == pseudo(U"Display refresh rate"));
    CHECK(d->get("IDC_PORT_DISPLAY_INFO")->text == pseudo(U"160 Hz"));
    const auto rect = [&](const GuiControl* c) { return d->control_rect(static_cast<size_t>(c - &(*d)[0])); };
    const float value_left = rect(d->get("IDC_PORT_ANTI_ALIAS_VALUE")).min.x;
    CHECK(rect(d->get("IDC_ANTI_ALIAS_STATIC")).max.x <= value_left && rect(d->get("IDC_ANTI_ALIAS_STATIC")).min.x < value_left);
    CHECK(d->get("IDC_ANTI_ALIAS_OFF_STATIC")->scale.x == 1.0f);  // not a caption under the value
    bool detail_seen = false;
    for (size_t i = 0; i < d->size(); ++i)
        if ((*d)[i].def && (*d)[i].def->y == 271 && (*d)[i].def->x == 41) {
            detail_seen = true;
            CHECK(d->control_rect(i).max.x <= rect(d->get("IDC_PORT_DETAIL_VALUE")).min.x);
        }
    CHECK(detail_seen);
}

// ---- [port: Gabriel] V-sync mode combo and display line added to the video dialog ----------------------------------
void test_video_vsync_mode() {
    Env env;
    const size_t before = env.script.find("IDD_VIDEO_OPTIONS_DIALOG")->controls.size();
    CHECK(env.script.extend(kPortDialogExtensionsRc) && env.script.warnings.empty());
    const RcDialog* rd = env.script.find("IDD_VIDEO_OPTIONS_DIALOG");
    CHECK(rd && rd->controls.size() == before + 8 && rd->find("IDC_ADVANCED") >= 0 && rd->find("IDC_PORT_VSYNC_MODE") >= 0 &&
          rd->w == 570 && rd->has_style("DS_CENTER"));  // the shipped header is kept
    FrontEnd fe;
    MenuProviders p;
    VideoSettings cur;
    cur.vsync_mode = VsyncMode::Mailbox;
    cur.display_info = U"160 Hz, Mailbox";
    cur.frame_cap = 144;
    p.video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO")}, InputFrame{}, acts);
    GuiDialog* d = fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d && d->get("IDC_PORT_VSYNC_MODE")->selected == 2 && d->get("IDC_PORT_VSYNC_MODE")->items.size() == kVsyncModeCount + 1 &&
          d->get("IDC_PORT_DISPLAY_INFO")->text == U"160 Hz, Mailbox" && d->get("IDC_PORT_FRAME_CAP")->selected == 6 &&
          d->get("IDC_PORT_FRAME_CAP")->items[1] == U"Display refresh rate" && d->get("IDC_PORT_FRAME_CAP")->items[3] == U"60 fps");
    if (!d) return;
    // U9: the check box and the mode list are one setting, whichever is used.
    auto changed = [&](GuiEvent::Kind k, const char* id, int32_t value) {
        GuiEvent e = event(k, "IDD_VIDEO_OPTIONS_DIALOG", id);
        e.value = value;
        fe.update(std::vector<GuiEvent>{e}, InputFrame{}, acts);
    };
    CHECK(d->get("IDC_PORT_VSYNC_MODE")->items[0] == U"Off (no v-sync)" && d->get("IDC_VSYNC_ENABLED")->checked);
    d->set_checked("IDC_VSYNC_ENABLED", false);
    changed(GuiEvent::Kind::Checked, "IDC_VSYNC_ENABLED", 0);
    CHECK(d->get("IDC_PORT_VSYNC_MODE")->selected == 0);
    d->set_checked("IDC_VSYNC_ENABLED", true);
    changed(GuiEvent::Kind::Checked, "IDC_VSYNC_ENABLED", 1);
    CHECK(d->get("IDC_PORT_VSYNC_MODE")->selected == 2);  // back to the remembered Mailbox
    d->set_selected("IDC_PORT_VSYNC_MODE", 0);
    changed(GuiEvent::Kind::SelectionChanged, "IDC_PORT_VSYNC_MODE", 0);
    CHECK(!d->get("IDC_VSYNC_ENABLED")->checked);
    d->set_selected("IDC_PORT_VSYNC_MODE", 1);
    changed(GuiEvent::Kind::SelectionChanged, "IDC_PORT_VSYNC_MODE", 1);
    CHECK(d->get("IDC_VSYNC_ENABLED")->checked && acts.empty());
    // U8: the value beside each slider follows it; the shipped end captions stay.
    CHECK(d->get("IDC_PORT_DETAIL_VALUE")->text == U"100" && d->get("IDC_PORT_GAMMA_VALUE")->text == U"50" && d->get("IDC_PORT_ANTI_ALIAS_VALUE")->text == U"0");
    changed(GuiEvent::Kind::ValueChanged, "IDC_DETAIL_LEVEL_SLIDER", 25);
    changed(GuiEvent::Kind::ValueChanged, "IDC_GAMMA_SLIDER", 80);
    changed(GuiEvent::Kind::ValueChanged, "IDC_ANTI_ALIAS_SLIDER", 40);
    CHECK(d->get("IDC_PORT_DETAIL_VALUE")->text == U"25" && d->get("IDC_PORT_GAMMA_VALUE")->text == U"80" && d->get("IDC_PORT_ANTI_ALIAS_VALUE")->text == U"40");
    // Apply leaves the dialog open: the status line picks up the mode the host switched to on the next frame.
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].video.vsync && acts[0].video.vsync_mode == VsyncMode::Fifo);
    cur.display_info = U"160 Hz, FIFO";
    acts.clear();
    fe.update({}, InputFrame{}, acts);
    CHECK(acts.empty() && d->get("IDC_PORT_DISPLAY_INFO")->text == U"160 Hz, FIFO");
    // Off: v-sync goes off and the mode stays remembered.
    d->set_selected("IDC_PORT_VSYNC_MODE", 0);
    changed(GuiEvent::Kind::SelectionChanged, "IDC_PORT_VSYNC_MODE", 0);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && !acts[0].video.vsync && acts[0].video.vsync_mode == VsyncMode::Mailbox);
    acts.clear();
    d->set_selected("IDC_PORT_VSYNC_MODE", 1);
    changed(GuiEvent::Kind::SelectionChanged, "IDC_PORT_VSYNC_MODE", 1);
    d->set_selected("IDC_PORT_FRAME_CAP", 1);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_OK")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyVideo && acts[0].video.vsync && acts[0].video.vsync_mode == VsyncMode::Fifo &&
          acts[0].video.frame_cap == kFrameCapDisplay);
}

// ---- QA U10 / D8 / D10: list scrolling, the port combos' font, one check box size, the port dialogs' layout ---------
void test_options_polish() {
    Env env;
    CHECK(env.script.merge(kPortDialogsRc) && env.script.extend(kPortDialogExtensionsRc) && env.script.warnings.empty());
    FrontEnd fe;
    MenuProviders p;
    VideoSettings cur;
    for (int32_t i = 0; i < 40; ++i) cur.resolutions.push_back(std::u32string(U"mode ") + static_cast<char32_t>(0x61 + i % 26));
    cur.resolution = 38;
    p.video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO")}, InputFrame{}, acts);
    GuiDialog* v = env.gui.find("IDD_VIDEO_OPTIONS_DIALOG");
    CHECK(v != nullptr);
    if (!v) return;
    // A mode at the end of the list opens with a full list, not with empty rows under the last mode, and is visible.
    const GuiControl* list = v->get("IDC_RESOLUTION_LIST");
    CHECK(list->top > 0 && list->top <= list->selected && list->top < 38 - 3);
    CHECK(env.gui.scroll_list_to(*v, "IDC_RESOLUTION_LIST", 1000) && list->top < 40 - 1 && list->top > 0);
    const int32_t last_top = list->top;
    CHECK(env.gui.scroll_list_to(*v, "IDC_RESOLUTION_LIST", -5) && list->top == 0);
    CHECK(env.gui.scroll_list_to(*v, "IDC_RESOLUTION_LIST", last_top - 1) && list->top == last_top - 1);
    CHECK(!env.gui.scroll_list_to(*v, "IDC_ADVANCED", 1));
    // The combos the port adds take the caption font; a shipped combo would keep the skin's.
    CHECK(v->get("IDC_PORT_VSYNC_MODE")->ui_font && v->get("IDC_PORT_FRAME_CAP")->ui_font);
    CHECK(env.gui.control_font(*v, static_cast<size_t>(v->find("IDC_PORT_VSYNC_MODE"))) ==
          env.gui.control_font(*v, static_cast<size_t>(v->find("IDC_PORT_VSYNC_MODE_TEXT"))));
    // Advanced: check rows run up to the combo column, captions clear of their combos, Title Case.
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    GuiDialog* a = env.gui.find("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
    CHECK(a != nullptr);
    if (!a) return;
    for (const char* id : {"IDC_BLOOM", "IDC_PORT_AMBIENT_OCCLUSION"}) {  // the checks this test script has
        const int32_t k = a->find(id);
        CHECK(k >= 0);
        if (k >= 0) CHECK(std::fabs(a->control_rect(static_cast<size_t>(k)).width() - 215.0f) < 0.01f);
    }
    CHECK(a->get("IDC_PORT_AMBIENT_OCCLUSION")->text == U"Ambient Occlusion");
    CHECK(a->get("IDC_PORT_ANTI_ALIASING")->ui_font && a->get("IDC_PORT_ANISOTROPY")->ui_font);
    for (const auto& [caption, combo] : {std::pair{"IDC_PORT_ANTI_ALIASING_TEXT", "IDC_PORT_ANTI_ALIASING"}, std::pair{"IDC_PORT_ANISOTROPY_TEXT", "IDC_PORT_ANISOTROPY"}}) {
        const GuiRect cap = a->control_rect(static_cast<size_t>(a->find(caption)));
        const GuiRect box = a->control_rect(static_cast<size_t>(a->find(combo)));
        CHECK(cap.min.y >= box.max.y && cap.height() == 18.0f);
    }
    // Camera and Controls: the shipped title control (gold header font), both key columns the same width.
    env.gui.close_all();
    const GuiDialog* c = env.gui.open(kPortControlsDialog);
    CHECK(c != nullptr);
    if (!c) return;
    const GuiControl* title = c->get("IDC_STATIC_MEDIUM");
    CHECK(title && title->text == U"Camera and Controls");
    const auto column = [&](const char* id) { return c->control_rect(static_cast<size_t>(c->find(id))); };
    CHECK(column("IDC_EDIT_PORT_PAUSE").width() == column("IDC_EDIT_PORT_SCROLL_UP").width() &&
          column("IDC_EDIT_PORT_PAUSE").min.x == column("IDC_EDIT_PORT_SCROLL_UP").min.x);
}

// ---- [port: Gabriel] anti-aliasing / anisotropic filtering combos and ambient occlusion check (advanced video) ------
void test_advanced_video_port() {
    Env env;
    const size_t before = env.script.find("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG")->controls.size();
    CHECK(env.script.extend(kPortDialogExtensionsRc) && env.script.warnings.empty());
    const RcDialog* rd = env.script.find("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
    CHECK(rd && rd->controls.size() == before + 5 && rd->find("IDC_BLOOM") >= 0 && rd->find("IDC_PORT_AMBIENT_OCCLUSION") >= 0);
    FrontEnd fe;
    MenuProviders p;
    AdvancedVideoSettings cur;
    cur.anti_aliasing = AntiAliasing::Taa;
    cur.anisotropy = 8;
    p.advanced_video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    GuiDialog* video = env.gui.open("IDD_VIDEO_OPTIONS_DIALOG");
    if (!video) return;
    video->set_value("IDC_ANTI_ALIAS_SLIDER", 40);  // t19 / D9: the combo follows the slider
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    GuiDialog* d = fe.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d && d->get("IDC_PORT_ANTI_ALIASING")->selected == 1 && d->get("IDC_PORT_ANISOTROPY")->selected == 3 &&
          d->get("IDC_PORT_ANISOTROPY")->items.size() == std::size(kAnisotropyChoices) &&
          d->get("IDC_PORT_ANISOTROPY")->items[4] == U"16x" && !d->get("IDC_PORT_AMBIENT_OCCLUSION")->checked);
    if (!d) return;
    d->set_selected("IDC_PORT_ANTI_ALIASING", 0);
    d->set_selected("IDC_PORT_ANISOTROPY", 4);
    d->set_checked("IDC_PORT_AMBIENT_OCCLUSION", true);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyAdvancedVideo &&
          acts[0].advanced.anti_aliasing == AntiAliasing::Off && acts[0].advanced.anisotropy == 16 &&
          acts[0].advanced.ambient_occlusion && acts[0].advanced.bloom);
    // t19 / D9: Apply in Advanced moves the slider underneath, both ways (Off -> 0, TAA from 0 -> the default position).
    CHECK(video->get("IDC_ANTI_ALIAS_SLIDER")->value == 0 && video->get("IDC_PORT_ANTI_ALIAS_VALUE")->text == U"0");
    d->set_selected("IDC_PORT_ANTI_ALIASING", 1);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(video->get("IDC_ANTI_ALIAS_SLIDER")->value == kAntiAliasDefaultSlider && video->get("IDC_PORT_ANTI_ALIAS_VALUE")->text == U"63");
    // A slider already above 0 keeps its strength.
    video->set_value("IDC_ANTI_ALIAS_SLIDER", 20);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(video->get("IDC_ANTI_ALIAS_SLIDER")->value == 20);
    CHECK(anti_aliasing_from_slider(0) == AntiAliasing::Off && anti_aliasing_from_slider(1) == AntiAliasing::Taa);
    // t43: Detail and the seven agree both ways. Apply in Advanced moves the Detail slider to their level...
    d->set_value("IDC_PARTICLE_DETAIL_SLIDER", 30);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY")}, InputFrame{}, acts);
    CHECK(acts.back().advanced.particle == 30 && video->get("IDC_DETAIL_LEVEL_SLIDER")->value == 90 &&
          video->get("IDC_PORT_DETAIL_VALUE")->text == U"90");
    // ... and a Detail slider moved in Video, not yet applied, opens Advanced as its preset; unmoved keeps a custom mix.
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_CANCEL")}, InputFrame{}, acts);
    VideoSettings applied;
    applied.detail = 90;
    MenuProviders p2 = p;
    p2.video = [&] { return applied; };
    cur.particle = 30;
    FrontEnd fe2;
    fe2.init(env.gui, {}, p2);
    video->set_value("IDC_DETAIL_LEVEL_SLIDER", 90);
    fe2.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    CHECK(env.gui.top() && env.gui.top()->get("IDC_PARTICLE_DETAIL_SLIDER")->value == 30);
    fe2.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_CANCEL")}, InputFrame{}, acts);
    video->set_value("IDC_DETAIL_LEVEL_SLIDER", 45);
    fe2.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    CHECK(env.gui.top() && env.gui.top()->get("IDC_PARTICLE_DETAIL_SLIDER")->value == 45);
    constexpr AdvancedVideoSettings all100;
    CHECK(advanced_detail_level(all100) == 100);
}

// ---- multiplayer screens (UI only) --------------------------------------------------------------------------------
void test_lobby() {
    Env env;
    LobbyScreens lob;
    lob.init(env.gui);
    std::vector<LobbyEvent> ev;
    lob.show_multiplayer_menu();
    CHECK(lob.top() == dlg::kMultiplayer);
    lob.update(env.click(dlg::kMultiplayer, "IDC_MULTIPLAYER_LAN_BUTTON"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::EnterLanLobby && lob.top() == dlg::kLanLobby);
    // Network layer pushes games / players / colours / chat.
    lob.set_local_name(U"Gabriel");
    lob.set_colors({U"Red", U"Blue"}, 1);
    lob.set_games({{"g1", U"Fleet battle", U"Kuat", U"Space", 1, 2, 30, false}, {"g2", U"Locked", U"Hoth", U"Land", 3, 4, -1, true}});
    lob.set_players({{U"Gabriel", 1}, {U"Pilot", 0}});
    for (int i = 0; i < 205; ++i) lob.add_chat(U"line");
    GuiDialog* d = env.gui.find(dlg::kLanLobby);
    CHECK(d && d->get("IDC_GAME_LIST")->items.size() == 2 && d->get("IDC_GAME_LIST")->items[0] == U"Fleet battle  Space  1/2  Kuat  30 ms" &&
          d->get("IDC_GAME_LIST")->items[1].back() == U'*' && d->get("IDC_PLAYER_LIST")->items.size() == 2 &&
          d->get("IDC_LAN_CHAT_LIST")->items.size() == kLobbyChatLines && d->get("IDC_LAN_CHAT_LIST")->top > 0 &&
          d->get("IDC_LAN_NAME_EDIT")->text == U"Gabriel" && d->get("IDC_LAN_COLOR_SELECT")->selected == 1 &&
          !d->get("IDC_GAME_LIST2")->visible && !d->get("IDC_LAN_JOIN_GAME_BUTTON")->enabled);
    ev.clear();
    lob.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, dlg::kLanLobby, "IDC_GAME_LIST", 0),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanLobby, "IDC_LAN_COLOR_SELECT", 0),
                                     event(GuiEvent::Kind::Clicked, dlg::kLanLobby, "IDC_INTERNET_PING_BUTTON")},
               ev);
    CHECK(ev.size() == 3 && ev[0].kind == LobbyEvent::Kind::SelectGame && ev[0].game == "g1" && ev[1].kind == LobbyEvent::Kind::SetPlayerColor &&
          ev[1].value == 0 && ev[2].kind == LobbyEvent::Kind::SortGames && ev[2].value == 4);
    // Chat: type + Enter in the chat edit (real input path).
    d->focus("IDC_LAN_CHAT_EDIT");
    ev.clear();
    lob.update(env.frame({5, 5}, false, {}, U"hello"), ev);
    lob.update(env.frame({5, 5}, false, {vk::kReturn}), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::Chat && ev[0].text == U"hello" && d->get("IDC_LAN_CHAT_EDIT")->text.empty());
    // Join a password game -> prompt -> JoinGame with the password.
    d->set_selected("IDC_GAME_LIST", 1);
    ev.clear();
    lob.update(std::vector<GuiEvent>{event(GuiEvent::Kind::DoubleClicked, dlg::kLanLobby, "IDC_GAME_LIST", 1)}, ev);
    CHECK(ev.empty() && env.gui.find(dlg::kPassword) != nullptr);
    env.gui.find(dlg::kPassword)->set_text("IDC_INTERNET_GAME_PASSWORD_EDIT", U"sesame");
    lob.update(env.click(dlg::kPassword, "IDC_OK"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::JoinGame && ev[0].game == "g2" && ev[0].text == U"sesame" &&
          env.gui.find(dlg::kPassword) == nullptr);
    // Host -> the network layer answers with the staging state.
    ev.clear();
    lob.update(env.click(dlg::kLanLobby, "IDC_LAN_HOST_GAME_BUTTON"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::HostGame);
    StagingState st;
    st.host = true;
    st.factions = {U"Rebel", U"Empire", U"Underworld"};
    st.colors = {U"Red", U"Blue"};
    st.color_values = {{255, 0, 0, 255}, {0, 0, 255, 255}};
    st.teams = {U"1", U"2"};
    st.maps = {U"Kuat", U"Hoth"};
    st.map = 0;
    StagingSlot me;
    me.state = SlotState::Human;
    me.name = U"Gabriel";
    me.local = true;
    me.faction = 1;
    StagingSlot ai;
    ai.state = SlotState::AI;
    ai.faction = 2;
    st.slots = {me, ai};
    st.options = {{U"Credits", U"10000"}, {U"Pop cap", U"20"}};
    CHECK(lob.show_staging(st));
    GuiDialog* s = env.gui.find(dlg::kLanStaging);
    CHECK(s && lob.top() == dlg::kLanStaging && s->get("IDC_PLAYER_NAME_0")->items[0] == U"Gabriel" && !s->get("IDC_PLAYER_NAME_0")->enabled &&
          s->get("IDC_PLAYER_NAME_1")->selected == 2 && s->get("IDC_PLAYER_NAME_1")->enabled && s->get("IDC_PLAYER_FACTION_1")->selected == 2 &&
          s->get("IDC_PLAYER_FACTION_0")->enabled && s->get("IDC_LAN_START_GAME_BUTTON")->enabled &&
          s->get("IDC_STAGING_MAP_LIST")->selected == 0 && s->get("IDC_ACCEPT_PLAYER_0")->image == "i_dialogue_check_on.tga" &&
          s->get("IDC_PLAYER_COLOR_0")->item_colors.size() == 2);
    {  // the colour combo draws a swatch (no glyphs) for its selected item
        HudDrawList sdl;
        env.gui.build(sdl, env.now);
        bool red = false;
        for (uint32_t layer = 0; layer < kHudLayers; ++layer)
            for (const HudQuad& q : sdl.items(layer, HudList::Alpha)) red = red || (q.color == Rgba8{255, 0, 0, 255});
        CHECK(red);
    }
    ev.clear();
    lob.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_PLAYER_FACTION_1", 0),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_PLAYER_NAME_1", 1),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_PLAYER_TEAM_0", 1),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_STAGING_MAP_LIST", 1)},
               ev);
    CHECK(ev.size() == 4 && ev[0].kind == LobbyEvent::Kind::SetSlotFaction && ev[0].slot == 1 && ev[0].value == 0 &&
          ev[1].kind == LobbyEvent::Kind::SetSlotState && ev[1].slot == 1 && ev[1].value == static_cast<int32_t>(SlotState::Closed) &&
          ev[2].kind == LobbyEvent::Kind::SetSlotTeam && ev[2].slot == 0 && ev[3].kind == LobbyEvent::Kind::SelectMap && ev[3].value == 1);
    ev.clear();
    lob.update(env.click(dlg::kLanStaging, "IDC_LAN_START_GAME_BUTTON"), ev);
    lob.update(env.click(dlg::kLanStaging, "IDC_LOAD_GAME"), ev);
    CHECK(ev.size() == 2 && ev[0].kind == LobbyEvent::Kind::StartGame && ev[1].kind == LobbyEvent::Kind::LoadGame);
    // A client: own slot only; another human not yet accepted blocks the host's start.
    st.host = false;
    st.slots[0].local = false;
    st.slots[1].state = SlotState::Human;
    st.slots[1].name = U"Me";
    st.slots[1].local = true;
    lob.set_staging(st);
    CHECK(!s->get("IDC_LAN_START_GAME_BUTTON")->visible && !s->get("IDC_PLAYER_FACTION_0")->enabled && s->get("IDC_PLAYER_FACTION_1")->enabled);
    ev.clear();
    lob.update(std::vector<GuiEvent>{event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_PLAYER_FACTION_0", 0),
                                     event(GuiEvent::Kind::SelectionChanged, dlg::kLanStaging, "IDC_STAGING_MAP_LIST", 0)},
               ev);
    CHECK(ev.empty());
    st.host = true;
    st.slots[0].local = true;
    st.slots[1].local = false;
    st.slots[1].accepted = false;
    lob.set_staging(st);
    CHECK(s->get("IDC_LAN_START_GAME_BUTTON")->visible && !s->get("IDC_LAN_START_GAME_BUTTON")->enabled);
    // Esc leaves the game -> back to the LAN lobby.
    ev.clear();
    lob.update(env.frame({5, 5}, false, {vk::kEscape}), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::LeaveGame && lob.top() == dlg::kLanLobby);
    // In-game: disconnect (kick) and vote.
    const ConnectionRow rows[2] = {{U"Host", 1.0f, false}, {U"Lagger", 0.25f, true}};
    lob.show_disconnect(rows);
    GuiDialog* dc = env.gui.find(dlg::kDisconnect);
    CHECK(dc && !dc->get("IDC_BUTTON_KICK_PLAYER0")->visible && dc->get("IDC_BUTTON_KICK_PLAYER1")->visible &&
          dc->get("IDC_CONNECTION_QUALITY1")->value == 25 && dc->get("IDC_PLAYER_NAME_1")->text == U"Lagger");
    ev.clear();
    lob.update(env.click(dlg::kDisconnect, "IDC_BUTTON_KICK_PLAYER1"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::KickPlayer && ev[0].slot == 1);
    env.gui.close(dlg::kDisconnect);
    VoteState v;
    v.planet = U"Coruscant";
    v.names = {U"Gabriel"};
    v.results = {U"Waiting"};
    v.countdown = 4.5f;
    lob.show_vote(v);
    GuiDialog* vd = env.gui.find(dlg::kVote);
    CHECK(vd && vd->get("IDC_TEXT_MP_VOTE_PLANET_NAME")->text == U"Coruscant" && vd->get("IDC_COUNTDOWN_TIMER")->text == U"5" &&
          vd->get("IDC_DIALOG_BOX_BUTTON_LEFT")->text == U"Yes");
    ev.clear();
    lob.update(env.click(dlg::kVote, "IDC_DIALOG_BOX_BUTTON_LEFT"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::Vote && ev[0].value == 1 && !vd->get("IDC_DIALOG_BOX_BUTTON_RIGHT")->enabled);
    env.gui.close(dlg::kVote);
    // Back from the lobby, then from the multiplayer menu.
    ev.clear();
    lob.update(env.click(dlg::kLanLobby, "IDC_LAN_CANCEL_BUTTON"), ev);
    CHECK(ev.size() == 1 && ev[0].kind == LobbyEvent::Kind::LeaveLobby && lob.top() == dlg::kMultiplayer);
    lob.update(env.click(dlg::kMultiplayer, "IDC_OK_MAIN"), ev);
    CHECK(ev.size() == 2 && ev[1].kind == LobbyEvent::Kind::BackToMainMenu && env.gui.open_count() == 0);
    CHECK(std::string(lobby_event_name(LobbyEvent::Kind::SelectClan)) == "SelectClan");
}

}  // namespace test_gui_screens_t

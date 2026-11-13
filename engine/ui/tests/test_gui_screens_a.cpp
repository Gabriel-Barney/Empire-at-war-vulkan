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
    CHECK(!gs.show_campaign_score({}));
    for (int k = 0; k < 13; ++k) std::printf("%s%s", k ? " " : "screen actions: ", screen_action_name(static_cast<ScreenAction::Kind>(k)));
    std::printf("\n");
}

// ---- advanced video options in the options flow -------------------------------------------------------------------
void test_advanced_video() {
    Env env;
    CHECK(env.script.extend(kPortDialogExtensionsRc));
    FrontEnd fe;
    MenuProviders p;
    AdvancedVideoSettings cur;
    cur.particle = 40;
    cur.mesh = 55;  // an older profile's slider value: High's range, not High's own value
    cur.bloom = false;
    p.advanced_video = [&] { return cur; };
    fe.init(env.gui, {}, p);
    env.gui.open("IDD_VIDEO_OPTIONS_DIALOG");
    std::vector<MenuAction> acts;
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED")}, InputFrame{}, acts);
    CHECK(fe.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
    GuiDialog* d = env.gui.top();
    CHECK(d && d->get("IDC_PORT_PARTICLE_DETAIL")->selected == 1 && d->get("IDC_PORT_MESH_DETAIL")->selected == 2 && !d->get("IDC_BLOOM")->checked);
    if (!d) return;
    CHECK(!d->get("IDC_PARTICLE_DETAIL_SLIDER")->visible);  // the list took the slider's place
    d->set_selected("IDC_PORT_PARTICLE_DETAIL", 2);
    d->set_checked("IDC_BLOOM", true);
    fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_OK")}, InputFrame{}, acts);
    CHECK(acts.size() == 1 && acts[0].kind == MenuAction::Kind::ApplyAdvancedVideo && acts[0].advanced.particle == kLevels4[2].value &&
          acts[0].advanced.bloom && acts[0].advanced.shader == 100);
    CHECK(acts[0].advanced.mesh == 55);  // a list left on its level keeps the stored value
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

// ---- t47: port strings take the pseudo-localization path; the shipped sliders and their end captions give way to the lists ------
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
    LTEXT           "TEXT_MAX",IDC_STATIC,517,271,47,21
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
    CHECK(d->get("IDC_PORT_ANTI_ALIASING")->items[1] == pseudo(kAntiAliasChoices[1].label) && d->get("IDC_PORT_DETAIL_LEVEL")->items[0] == pseudo(U"Low"));
    // The sliders of those two rows and the captions at their ends are gone; the rows' titles stay (and Gamma is untouched).
    CHECK(!d->get("IDC_ANTI_ALIAS_SLIDER")->visible && !d->get("IDC_DETAIL_LEVEL_SLIDER")->visible && !d->get("IDC_ANTI_ALIAS_OFF_STATIC")->visible);
    CHECK(d->get("IDC_GAMMA_SLIDER")->visible && d->get("IDC_ANTI_ALIAS_STATIC")->visible);
    size_t titles = 0, ends = 0;
    for (size_t i = 0; i < d->size(); ++i) {
        const GuiControl& c = (*d)[i];
        if (c.type != GuiGadgetType::Text || !c.def || c.def->y != 271) continue;
        if (c.visible) ++(c.def->x == 41 ? titles : ends);
    }
    CHECK(titles == 1 && ends == 0);
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
    // U8: the value beside the Gamma slider follows it. Anti-Aliasing and Detail Level are lists of named levels now (Off, Ultra).
    CHECK(d->get("IDC_PORT_GAMMA_VALUE")->text == U"50");
    changed(GuiEvent::Kind::ValueChanged, "IDC_GAMMA_SLIDER", 80);
    CHECK(d->get("IDC_PORT_GAMMA_VALUE")->text == U"80");
    CHECK(d->get("IDC_PORT_ANTI_ALIASING")->items.size() == std::size(kAntiAliasChoices) && d->get("IDC_PORT_ANTI_ALIASING")->selected == 0 &&
          d->get("IDC_PORT_DETAIL_LEVEL")->items.size() == kQualityCount + 1 && d->get("IDC_PORT_DETAIL_LEVEL")->selected == 3);
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
    CHECK(a->get("IDC_PORT_PARTICLE_DETAIL")->ui_font && a->get("IDC_PORT_ANISOTROPY")->ui_font && v->get("IDC_PORT_ANTI_ALIASING")->ui_font &&
          v->get("IDC_PORT_DETAIL_LEVEL")->ui_font);
    for (const auto& [caption, combo] : {std::pair{"IDC_PORT_ANISOTROPY_TEXT", "IDC_PORT_ANISOTROPY"}}) {
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

// ---- [port: Gabriel] quality lists, anisotropic filtering combo and ambient occlusion check (video + advanced video) -----
void test_advanced_video_port() {
    Env env;
    const size_t before = env.script.find("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG")->controls.size();
    CHECK(env.script.extend(kPortDialogExtensionsRc) && env.script.warnings.empty());
    const RcDialog* rd = env.script.find("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG");
    CHECK(rd && rd->controls.size() == before + 12 && rd->find("IDC_BLOOM") >= 0 && rd->find("IDC_PORT_AMBIENT_OCCLUSION") >= 0 &&
          rd->find("IDC_PORT_WATER_DETAIL") >= 0);
    CHECK(rd->find("IDC_PORT_ANTI_ALIASING") < 0);  // anti-aliasing is one list, in the Video dialog
    FrontEnd fe;
    MenuProviders p;
    AdvancedVideoSettings cur;
    cur.anisotropy = 8;
    cur.mesh = 55;
    VideoSettings applied;
    applied.anti_alias = 40;  // an older profile's slider position: inside Low's range
    applied.detail = advanced_detail_level(cur);
    p.advanced_video = [&] { return cur; };
    p.video = [&] { return applied; };
    fe.init(env.gui, {}, p);
    std::vector<MenuAction> acts;
    const auto click = [&](const char* dialog, const char* id) { fe.update(std::vector<GuiEvent>{event(GuiEvent::Kind::Clicked, dialog, id)}, InputFrame{}, acts); };
    click("IDD_MAIN_OPTIONS_DIALOG", "IDC_MAIN_OPTIONS_VIDEO");
    GuiDialog* video = fe.top() == "IDD_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(video != nullptr);
    if (!video) return;
    // Video: one anti-aliasing list (an old strength of 40 reads as Low), and the quality the options make (mesh 55 is High's range).
    GuiControl* aa = video->get("IDC_PORT_ANTI_ALIASING");
    GuiControl* quality = video->get("IDC_PORT_DETAIL_LEVEL");
    CHECK(aa->selected == 1 && aa->items.size() == 4 && quality->selected == static_cast<int32_t>(kQualityCustom));
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(acts.back().video.anti_alias == 40 && acts.back().video.detail == applied.detail && !acts.back().video.detail_picked);  // untouched lists change nothing
    video->set_selected("IDC_PORT_ANTI_ALIASING", 3);
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(acts.back().video.anti_alias == 100 && !acts.back().video.detail_picked);
    video->set_selected("IDC_PORT_ANTI_ALIASING", 0);
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(acts.back().video.anti_alias == 0);
    // Advanced: a list per option on the level the stored value is in, the shipped sliders hidden, anisotropy and AO as before.
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED");
    GuiDialog* d = fe.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d != nullptr);
    if (!d) return;
    const struct {
        const char* id;
        size_t items;
        int32_t selected;
    } expect[] = {{"IDC_PORT_PARTICLE_DETAIL", 4, 3}, {"IDC_PORT_MESH_DETAIL", 4, 2},   {"IDC_PORT_TEXTURE_DETAIL", 3, 2},
                  {"IDC_PORT_WATER_DETAIL", 3, 2},    {"IDC_PORT_SHADER_DETAIL", 4, 3}, {"IDC_PORT_SHADOW_DETAIL", 2, 1},
                  {"IDC_PORT_ENVIRONMENT_DETAIL", 4, 3}};
    for (const auto& e : expect) CHECK(d->get(e.id)->items.size() == e.items && d->get(e.id)->selected == e.selected);
    CHECK(d->get("IDC_PORT_TEXTURE_DETAIL")->items[0] == U"Low - quarter size" && d->get("IDC_PORT_SHADOW_DETAIL")->items[0] == U"Off");
    CHECK(d->get("IDC_PORT_ANISOTROPY")->selected == 3 && d->get("IDC_PORT_ANISOTROPY")->items.size() == std::size(kAnisotropyChoices) &&
          d->get("IDC_PORT_ANISOTROPY")->items[4] == U"16x" && !d->get("IDC_PORT_AMBIENT_OCCLUSION")->checked);
    CHECK(!d->get("IDC_PARTICLE_DETAIL_SLIDER")->visible);
    // [port: Gabriel] Render Scale: native by default, the listed percentages, 75 picked.
    const GuiControl* rs = d->get("IDC_PORT_RENDER_SCALE");
    CHECK(rs && rs->items.size() == std::size(kRenderScaleChoices) && rs->selected == static_cast<int32_t>(std::size(kRenderScaleChoices) - 1) &&
          rs->items.back() == U"100% (native)" && rs->items[3] == U"75%" && rs->ui_font);
    d->set_selected("IDC_PORT_PARTICLE_DETAIL", 0);
    d->set_selected("IDC_PORT_ANISOTROPY", 4);
    d->set_selected("IDC_PORT_RENDER_SCALE", 3);
    d->set_checked("IDC_PORT_AMBIENT_OCCLUSION", true);
    click("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(acts.back().kind == MenuAction::Kind::ApplyAdvancedVideo && acts.back().advanced.particle == 0 && acts.back().advanced.mesh == 55 &&
          acts.back().advanced.anisotropy == 16 && acts.back().advanced.ambient_occlusion && acts.back().advanced.bloom &&
          acts.back().advanced.render_scale == 75);
    // t43: Apply in Advanced moves the Detail Level list underneath to the level the seven make (Low amid Ultra: Custom) ...
    CHECK(quality->selected == static_cast<int32_t>(kQualityCustom));
    for (const char* id : {"IDC_PORT_PARTICLE_DETAIL", "IDC_PORT_MESH_DETAIL", "IDC_PORT_TEXTURE_DETAIL", "IDC_PORT_WATER_DETAIL", "IDC_PORT_SHADER_DETAIL",
                           "IDC_PORT_SHADOW_DETAIL", "IDC_PORT_ENVIRONMENT_DETAIL"})
        d->set_selected(id, 0);
    d->set_selected("IDC_PORT_SHADOW_DETAIL", 1);  // Low keeps shadows on: the preset's 0 is "Off" only for the shadow list's own row 0
    click("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(quality->selected == static_cast<int32_t>(kQualityCustom) && acts.back().advanced.shader == kLevels4[0].value &&
          acts.back().advanced.texture == kTextureChoices[0].value && acts.back().advanced.shadow == kShadowChoices[1].value);
    d->set_selected("IDC_PORT_SHADOW_DETAIL", 0);  // everything on its first row: the Low preset
    click("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(quality->selected == 0);
    // ... and a quality picked in Video, not yet applied, opens Advanced as its preset.
    click("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_CANCEL");
    video->set_selected("IDC_PORT_DETAIL_LEVEL", 1);
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED");
    d = fe.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d != nullptr);
    if (!d) return;
    for (const auto& e : std::initializer_list<std::pair<const char*, int32_t>>{{"IDC_PORT_PARTICLE_DETAIL", 1}, {"IDC_PORT_TEXTURE_DETAIL", 1}, {"IDC_PORT_WATER_DETAIL", 1},
                                                                                  {"IDC_PORT_SHADER_DETAIL", 1}, {"IDC_PORT_SHADOW_DETAIL", 1}})
        CHECK(d->get(e.first)->selected == e.second);
    click("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_OK");
    CHECK(acts.back().advanced.particle == kLevels4[1].value && acts.back().advanced.water == kLevels3[1].value && acts.back().advanced.mesh == kLevels4[1].value);
    // Video Apply: a level picked that differs from what the options make asks for the preset; Custom and the level in force do not.
    video->set_selected("IDC_PORT_DETAIL_LEVEL", 2);
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(acts.back().kind == MenuAction::Kind::ApplyVideo && acts.back().video.detail_picked && acts.back().video.detail == kQualityPresets[2]);
    video->set_selected("IDC_PORT_DETAIL_LEVEL", static_cast<int32_t>(kQualityCustom));
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(!acts.back().video.detail_picked && acts.back().video.detail == applied.detail);
    set_advanced_detail(cur, 100);  // the options now sit on the Ultra row: picking Ultra is no change
    video->set_selected("IDC_PORT_DETAIL_LEVEL", 3);
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_APPLY");
    CHECK(!acts.back().video.detail_picked);
    // Auto Detect (gfxf t02): the button only asks the host; refresh_video_dialogs shows what the host then set, in the Video
    // dialog and in an open Advanced dialog, and leaves the rest of the Video dialog (here the gamma slider) as the player had it.
    video->set_value("IDC_GAMMA_SLIDER", 77);
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_AUTO_DETECT");
    CHECK(acts.back().kind == MenuAction::Kind::AutoDetectVideo);
    set_advanced_detail(cur, kQualityPresets[1]);
    cur.anisotropy = 4;
    cur.ambient_occlusion = true;
    applied.anti_alias = kAntiAliasChoices[1].value;
    applied.detail = kQualityPresets[1];
    fe.refresh_video_dialogs();
    CHECK(video->get("IDC_PORT_DETAIL_LEVEL")->selected == 1 && video->get("IDC_PORT_ANTI_ALIASING")->selected == 1 &&
          video->get("IDC_GAMMA_SLIDER")->value == 77);
    click("IDD_VIDEO_OPTIONS_DIALOG", "IDC_ADVANCED");
    d = fe.top() == "IDD_ADVANCED_VIDEO_OPTIONS_DIALOG" ? env.gui.top() : nullptr;
    CHECK(d != nullptr);
    if (!d) return;
    CHECK(d->get("IDC_PORT_MESH_DETAIL")->selected == 1 && d->get("IDC_PORT_ANISOTROPY")->selected == 2 && d->get("IDC_PORT_AMBIENT_OCCLUSION")->checked);
    click("IDD_ADVANCED_VIDEO_OPTIONS_DIALOG", "IDC_AUTO_DETECT");  // the same button inside Advanced
    CHECK(acts.back().kind == MenuAction::Kind::AutoDetectVideo);
    set_advanced_detail(cur, kQualityPresets[3]);
    cur.anisotropy = 16;
    cur.ambient_occlusion = false;
    applied.detail = kQualityPresets[3];
    fe.refresh_video_dialogs();
    CHECK(d->get("IDC_PORT_MESH_DETAIL")->selected == 3 && d->get("IDC_PORT_ANISOTROPY")->selected == 4 && !d->get("IDC_PORT_AMBIENT_OCCLUSION")->checked &&
          video->get("IDC_PORT_DETAIL_LEVEL")->selected == 3);
    constexpr AdvancedVideoSettings all100;
    CHECK(advanced_detail_level(all100) == 100 && quality_level(all100) == 3);
    // The levels are consistent: each preset reads back as itself, a mix is Custom, and the anti-aliasing ranges are where they say.
    for (size_t i = 0; i < kQualityCount; ++i) {
        AdvancedVideoSettings a;
        set_advanced_detail(a, kQualityPresets[i]);
        CHECK(quality_level(a) == i);
    }
    AdvancedVideoSettings mix;
    mix.water = 0;
    CHECK(quality_level(mix) == kQualityCustom);
    CHECK(level_index(kAntiAliasChoices, 0) == 0 && level_index(kAntiAliasChoices, 1) == 1 && level_index(kAntiAliasChoices, 46) == 1 &&
          level_index(kAntiAliasChoices, kAntiAliasDefaultSlider) == 2 && level_index(kAntiAliasChoices, 81) == 2 && level_index(kAntiAliasChoices, 100) == 3);
}

}  // namespace test_gui_screens_t

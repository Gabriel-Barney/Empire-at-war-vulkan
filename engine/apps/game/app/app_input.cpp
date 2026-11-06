// alamo_game App: special systems, sim stepping, command routing, HUD / galactic / battle / menu input (split from game_main.cpp).
#include "game_app.hpp"

namespace alamo_game_app {

uint32_t App::panel_kind() const {
    if (screen_ != Screen::Galactic || !session_) return 0;
    if (const SpecialDialog* d = session_->special_dialog()) return d->kind;
    return panel_preview_;
}

// The local player's units at planet `pi` whose type has a Ground_Activated special ability, in g_special_ability row
// order [port]: orbit fleets in orbit order, members in order, then the surface.
std::vector<objects::ObjectHandle> App::special_units(const galactic::GalacticSim& g, PlanetIndex pi) const {
    std::vector<objects::ObjectHandle> out;
    if (pi == kInvalidPlanet) return out;
    const objects::ObjectStore& st = g.store();
    const galactic::SpecialRules& sp = g.rules().special;
    auto usable = [&](objects::ObjectHandle h) {
        if (!st.alive(h) || st.owner(h) != local_player()) return false;
        for (const uint32_t a : sp.type(static_cast<uint32_t>(st.type(h))).abilities)
            if (sp.abilities[a].style == galactic::ActivationStyle::GroundActivated) return true;
        return false;
    };
    for (const uint32_t fi : g.fleets_at(pi, local_player()))
        for (const objects::ObjectHandle h : g.fleets()[fi].contents)
            if (usable(h)) out.push_back(h);
    for (const objects::ObjectHandle h : g.planets()[pi].surface)
        if (usable(h)) out.push_back(h);
    return out;
}

// SpecialAbility kind 1 (activate) at the selected planet by the g_special_ability row's hero (GalacticHud rows from
// special_units); hero_id 0 = the first of them. False when none / refused.
bool App::activate_special_here(uint32_t hero_id) {
    const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
    if (!g) return false;
    const objects::ObjectStore& st = g->store();
    for (const objects::ObjectHandle h : special_units(*g, planet_index(*g, selected_planet_)))
        if ((hero_id == 0 || static_cast<uint32_t>(st.id(h)) == hero_id) &&
            session_->activate_special(static_cast<uint32_t>(st.id(h)), selected_planet_)) {
            ++activations_;
            return true;
        }
    return false;
}

// SFX / messages of the special events (local player only, P4_SIM_SYSTEMS §1.4, §2.3, §2.4, §3.3, §6.2). The "local error
// SFX" (0x140b301c8) is not identified: [port] the GUI negative feedback.
void App::special_feedback() {
    if (!session_) return;
    using K = galactic::GalacticEventKind;
    auto type_name = [this](uint32_t t) { return t < data_.rules.types.size() ? data_.rules.types[t].name.c_str() : "?"; };
    bool unlock_sfx = false;
    for (const galactic::GalacticEvent& e : session_->take_special_events()) {
        const bool local = e.player == local_player();
        switch (e.kind) {
            case K::SpecialError:
                if (local) audio_.negative_feedback();
                break;
            case K::BlackMarketPurchase:
                if (local && e.other_player == 1) {
                    audio_.faction_sfx("SFXEvent_New_Construction_Options_Available");
                    audio_.faction_sfx("SFXEvent_Slice_Success");
                    ++special_sfx_;
                }
                break;
            case K::GalacticSabotage:
                if (local) {
                    audio_.faction_sfx("SFXEvent_Sabotage_Success");
                    ++special_sfx_;
                }
                break;
            case K::SuperWeaponHacked:
                if (local) {
                    audio_.faction_sfx("SFXEvent_Hack_Success");
                    ++special_sfx_;
                }
                break;
            case K::HeroRespawned:
                if (local && e.type < data_.types.size()) {
                    audio_.ui(data_.types.get_string(static_cast<objects::TypeIndex>(e.type), "SFXEvent_Hero_Respawned"));
                    ++special_sfx_;
                }
                break;
            case K::SabotageVigilance:  // TEXT_SABOTAGE_FAILURE_VIGILANCE to the saboteur's owner, _PREVENTED_ to the planet's
                if (local || e.other_player == local_player()) {
                    const char* id = local ? "TEXT_SABOTAGE_FAILURE_VIGILANCE" : "TEXT_SABOTAGE_PREVENTED_VIGILANCE";
                    std::printf("message: %s%s\n", id, ui_.text.find(id) ? "" : " (no text)");  // [port] no message log UI
                    ++special_msgs_;
                }
                break;
            case K::TypeUnlocked:  // REMAINING_GAPS 1.4 / 1.5: the local player's new construction options (one SFX per batch)
                if (local) {
                    std::printf("unlocked: %s (%s)\n", type_name(e.type),
                                e.value == 1 ? "tech sweep" : e.value == 2 ? "slice" : e.value == 4 ? "corruption" : "story / Lua");
                    if (!unlock_sfx) {
                        audio_.faction_sfx("SFXEvent_New_Construction_Options_Available");
                        ++special_sfx_;
                        unlock_sfx = true;
                    }
                }
                break;
            case K::Slice:  // REMAINING_GAPS 1.3 step 7: outcome messages (slicer owner; vigilance also to the planet owner)
                if (local || (e.value == 4 && e.other_player == local_player())) {
                    const char* id = e.value == 0   ? "TEXT_SLICE_SUCCESS"
                                     : e.value == 1 ? "TEXT_SLICE_FAILURE_CAPTURED"
                                     : e.value == 3 ? "TEXT_SLICE_FAILURE_CHEATED"
                                     : e.value == 4 ? (local ? "TEXT_SLICE_FAILURE_VIGILANCE" : "TEXT_SLICE_PREVENTED_VIGILANCE")
                                                    : nullptr;  // 2 lost effort: no message [port: none identified]
                    std::printf("slice: %s %s%s%s\n", type_name(e.type), e.value == 0 ? "unlocked" : e.value == 2 ? "lost effort" : "failed",
                                id ? ", message " : "", id ? id : "");
                    if (id) ++special_msgs_;
                    if (local && e.value == 0) {
                        audio_.faction_sfx("SFXEvent_Slice_Success");
                        ++special_sfx_;
                    }
                }
                break;
            default: break;
        }
    }
}
// --- end P4 host ---

void App::post_tick() {
    audio_.sim_tick();
    if (session_->take_reveal_sfx()) {  // GRC 1.3: a planet was revealed by the galactic pre-step (frame > 0)
        audio_.ui("SFXEvent_Planet_Revealed");
        ++reveal_sfx_;
    }
    special_feedback();  // P4 host
    if (session_->in_battle()) {
        if (!bview_.active()) {
            begin_battle();
            return;
        }
        bview_.capture(*session_);
        if (const sim::TacticalBattle* b = session_->battle()) {
            const objects::ObjectStore& st = session_->host().modes().top_tactical()->primary();
            const game::GameSession* gs = session_.get();
            audio_.battle_tick(*b, st, data_.types, local_player(),
                               [gs](objects::ObjectId id) { return gs->script_hidden(id); });  // q6 t19 §10.3
            for (const TswCue& c : tsw_cues(b->special_weapon_events(), st)) {  // refixf t05: TSW_PLANET_DESTROY.md §11
                if (c.kind == TswCue::Kind::Firing) tsw_shot_begin(c, *b, st);              // 0x2ccf30: the firing shot
                else if (c.kind == TswCue::Kind::PlanetDestroyed) tsw_shake(c, *b);        // 0x2cdbb0 at the dummy
            }
            {  // radar pings (deaths, capture-point transitions) and the story's blips: read-only presentation
                const lua::StoryModel* tac_story = nullptr;
#ifdef ALAMO_SIM_HAVE_STORY
                if (const sim::StorySession* sst = session_->host().story()) tac_story = sst->tactical_model();
#endif
                radar_feed_.battle_tick(*b, st, data_.types, ui_.hud_types, local_player(), tac_story, radar_settings_, radar_events_);
            }
        }
        // Selection mirror <- the synced selection once the local orders were drained.
        if (session_->host().frame() > last_submit_frame_) orders_.set_selection(synced_selection());
        std::vector<std::pair<uint64_t, Vec3>> pos;
        for (const render::SnapshotObject& o : bview_.frame().objects) pos.emplace_back(static_cast<uint64_t>(o.id), o.position);
        audio_.set_object_positions(std::move(pos));
    } else {
        if (bview_.active()) end_battle();
        if (session_) gview_.capture(session_->host());
    }
}

void App::step_sim() {
    if (!session_ || hold_sim_ || script_hold_ || paused_ || battle_end_open_ || story_summary_open_) return;
    if (load_intro_frames_ > 0) return;  // QA t11: the load screen reaches the screen before the push's blocking load
    const bool demo = screen_ == Screen::Menu && demo_session();  // OI host: the attract demo behind the main menu
    if ((screen_ == Screen::Menu && !demo) || screen_ == Screen::BattleLoad || screen_ == Screen::Quit) return;
    const bool smoke = !opt_.smoke.empty();
    if (demo && attract_.idle_paused()) return;  // the attract idle pause (rendering continues)
    if (demo && !bview_.active()) {  // its hidden skirmish galaxy runs fast until the battle is pushed
        for (int i = 0; i < 200 && demo_session() && !bview_.active(); ++i)
            if (session_->tick()) post_tick();
        return;
    }
    if (screen_ == Screen::SessionLoad) {  // skirmish: run the hidden galaxy fast until the battle is pushed
        for (int i = 0; i < 200 && session_ && screen_ == Screen::SessionLoad; ++i)
            if (session_->tick()) post_tick();
        return;
    }
    if (bench_active()) {  // BM2a: the benchmark's own schedule (app_benchmark.cpp)
        bench_step_sim();
        return;
    }
    if (smoke) {
        const int n = screen_ == Screen::Galactic ? 16 : 4;
        const Screen at = screen_;  // a battle start / end ends the frame's ticks (the presentation catches up)
        for (int i = 0; i < n && session_ && screen_ == at; ++i)
            if (session_->tick()) post_tick();
        return;
    }
    const auto now = static_cast<uint32_t>(ms_since(t0_));
    if (session_->loop_iteration(now)) {
        prev_tick_ms_ = last_tick_ms_;
        last_tick_ms_ = now;
        post_tick();
    }
}

std::vector<uint32_t> App::synced_selection() const {
    std::vector<uint32_t> v;
    if (!session_) return v;
    for (const objects::ObjectId id : session_->host().selection(static_cast<uint8_t>(local_player()))) v.push_back(static_cast<uint32_t>(id));
    std::sort(v.begin(), v.end());
    return v;
}

// Unit responses (AUDIO.md 3.3): the representative unit = the lowest id of the order's units [port].
void App::respond(const commands::EventData& ev) {
    if (!session_) return;
    using T = commands::EventType;
    UnitResponse kind;
    std::vector<uint32_t> ids;
    switch (ev.type()) {
        case T::Select: kind = UnitResponse::Select; ids = ev.ids("ids"); break;
        case T::MoveToPosition: case T::MoveToObject: case T::MoveToRay: case T::MoveToPositionFacing: case T::MoveToRayFacing:
        case T::Facing:  // [port] a facing drag answers like a move
            kind = UnitResponse::Move;
            ids = (ev.type() == T::MoveToPositionFacing || ev.type() == T::Facing) && !ev.ids("units").empty() ? ev.ids("units")
                                                                                                                : orders_.selection();
            break;
        case T::Attack: kind = UnitResponse::Attack; ids = ev.ids("attackers").empty() ? orders_.selection() : ev.ids("attackers"); break;
        case T::Escort: kind = UnitResponse::Guard; ids = orders_.selection(); break;
        case T::StopMovement: kind = UnitResponse::Stop; ids = orders_.selection(); break;
        case T::MoveObjectToObject: kind = UnitResponse::FleetMove; ids = {ev.u("object")}; break;
        default: return;
    }
    if (ids.empty()) return;
    const uint32_t id = *std::min_element(ids.begin(), ids.end());
    const sim::GameMode* top = session_->host().modes().top();
    if (!top) return;
    const objects::ObjectStore& st = top->primary();
    objects::ObjectHandle h = st.find(static_cast<objects::ObjectId>(id));
    if (!st.active(h)) return;
    if (kind == UnitResponse::FleetMove || (!top->tactical() && kind == UnitResponse::Select)) {
        // A galactic fleet answers with its first member.
        if (const galactic::GalacticSim* g = session_->galaxy())
            for (const galactic::FleetState& f : g->fleets())
                if (f.alive && f.object == h && !f.contents.empty() && st.alive(f.contents.front())) {
                    h = f.contents.front();
                    break;
                }
    }
    audio_.unit_response(data_.types, st.type(h), static_cast<uint64_t>(st.id(h)), kind, false, top->tactical() ? st.owner(h) : -1);
}

void App::submit(commands::EventData ev, bool voice) {
    if (!session_) return;
    if (voice) respond(ev);  // UiOrder::voice false: the double-click order replays no unit response (spec §1.4)
    const std::string name(ev.def().name);
    std::string detail;  // galactic moves: which fleet / planet / player (the reason alone hid the playtest failure)
    if (ev.type() == commands::EventType::MoveObjectToObject)
        detail = " (object " + std::to_string(ev.u("object")) + " -> target " + std::to_string(ev.u("target")) + ", player " +
                 std::to_string(local_player()) + ")";
    const commands::SubmitResult r = session_->submit(std::move(ev));
    if (!r.ok()) {
        std::fprintf(stderr, "submit %s refused: %s%s\n", name.c_str(), std::string(commands::reject_reason_name(r.status)).c_str(), detail.c_str());
        ++refused_;
    }
    if (std::getenv("ALAMO_GAME_TRACE")) std::printf("trace: submit %s at tick %" PRIu64 "\n", name.c_str(), session_->host().frame());
    last_submit_frame_ = session_->host().frame() + 1;
}

// Commands of this frame (translator, HUD, hotkeys, panels, screens) -> the session. Pause entries toggle the pause
// (a command-stream entry, GameSession::set_paused); a HUD Retreat asks for confirmation first (IDD_RETREAT_DIALOG).
void App::route_commands(std::vector<commands::EventData>& cmds, std::span<const uint8_t> voice) {
    for (size_t ci = 0; ci < cmds.size(); ++ci) {
        commands::EventData& e = cmds[ci];
        bool p = false;
        if (ui::read_pause_event(e, p)) {
            if (session_ && p != session_->pause_requested()) toggle_pause();
            continue;
        }
        // GRC 1.1: no fleet orders to a planet whose raw revealed byte is clear (the human hop gate would refuse them):
        // click, drag-to-move and the HUD fleet drag all end here.
        if (screen_ == Screen::Galactic && session_ &&
            (e.type() == commands::EventType::MoveToObject || e.type() == commands::EventType::MoveObjectToObject) &&
            !session_->move_target_revealed(e.u("target"))) {
            ++hidden_moves_dropped_;
            continue;
        }
        if (e.type() == commands::EventType::Retreat && screen_ == Screen::Battle && !ui_.gui.find(ui::dlg::kRetreat)) {
            screens_.show_retreat_confirm();
            continue;
        }
        submit(std::move(e), ci >= voice.size() || voice[ci] != 0);
    }
    cmds.clear();
    // OI host: the Pause hotkey flips its own idea of the state; keep it on the session's (a refused or ignored toggle).
    if (session_) hotkeys_.set_paused(session_->pause_requested());
}

void App::toggle_pause() {
    if (!session_) return;
    const bool want = !session_->pause_requested();
    if (session_->set_paused(want)) {
        ++pause_toggles_;
        hotkeys_.set_paused(want);
        std::printf("pause: %s at tick %" PRIu64 "\n", want ? "requested" : "resumed", session_->host().frame());
    }
}

void App::handle_hud_actions(const std::vector<ui::HudAction>& acts) {
    for (const ui::HudAction& a : acts) {
        switch (a.kind) {
            case ui::HudAction::Kind::GalacticSelect: {
                // A planet fleet button selects its fleet; the land-forces button names the planet: select the planet
                // (no fleet selection).
                bool planet = false;
                if (const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr; g && a.ids.size() == 1)
                    for (const galactic::PlanetState& ps : g->planets())
                        if (g->store().alive(ps.object) && static_cast<uint32_t>(g->store().id(ps.object)) == a.ids.front()) planet = true;
                if (planet) {
                    selected_planet_ = a.ids.front();
                    orders_.set_selection({});
                } else {
                    orders_.set_selection(a.ids);
                }
                break;
            }
            case ui::HudAction::Kind::BattleBegin: answer_battle_prompt(false); break;  // GALACTIC_DIALOGS §1.5
            case ui::HudAction::Kind::BattleAutoResolve: answer_battle_prompt(true); break;
            case ui::HudAction::Kind::ResumeGame:  // the Game Paused banner's Resume Game (pause_event(false))
                if (session_ && session_->pause_requested()) toggle_pause();
                break;
            case ui::HudAction::Kind::RadarCameraJump: bview_.camera().set_look_at({a.x, a.y, 0.0f}); break;
            case ui::HudAction::Kind::PlanetInfo: {  // GALACTIC_DIALOGS §9: the organiser's planet_info button -> IDD_PLANET_INFO
                // Single instance: a click while it is open does nothing (the first planet is kept). No pause, no §6.3 gates.
                if (screens_.planet_info_open()) break;
                const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
                const PlanetIndex row = g ? planet_index(*g, a.unit) : kInvalidPlanet;
                if (row == kInvalidPlanet) break;
                ui::PlanetInfoData d = planet_info_data(*g, row);
                d.planet = a.unit;
                if (!screens_.show_planet_info(d)) notes_.push_back("planet info dialog missing");
                break;
            }
            case ui::HudAction::Kind::BeginReinforcement:  // the drop point is the next left click in the world
                reinforce_unit_ = a.unit;
                targeting_.cancel();  // one modal pick at a time
                break;
            case ui::HudAction::Kind::BeginAbilityTargeting:  // the target is the next world click (TargetingController)
                if (screen_ == Screen::Battle && targeting_.begin(a)) reinforce_unit_ = 0;
                break;
            case ui::HudAction::Kind::TacticalBuy:  // Build_NN: Production {type_index, location = builder}
                if (screen_ == Screen::Battle && a.unit != 0 && a.value >= 0)
                    submit(commands::ev::production(static_cast<uint16_t>(a.value), a.unit));
                break;
            case ui::HudAction::Kind::TacticalCancel:  // tqueueNN: Production {type_index, location, cancel, queue, index}
                if (screen_ == Screen::Battle && a.value >= 0 && a.ids.size() >= 2)
                    submit(commands::ev::production(static_cast<uint16_t>(a.value), a.unit, true,
                                                    static_cast<uint8_t>(a.ids[0]), static_cast<uint8_t>(a.ids[1])));
                break;
            case ui::HudAction::Kind::HeroSelect:  // FIXPASS_GALACTIC §2.3 tactical single click: replace the selection
                if (a.unit && screen_ == Screen::Battle) submit(commands::ev::select({a.unit}));
                break;
            case ui::HudAction::Kind::HeroSelectGroup: {  // §2.3 tactical double click: the container, else every own object
                if (screen_ != Screen::Battle || !bview_.active() || a.unit == 0) break;  // of the hero's type; then centre
                std::vector<uint32_t> ids = a.ids;
                const render::SnapshotObject* hero = nullptr;
                for (const render::SnapshotObject& o : bview_.frame().objects)
                    if (static_cast<uint32_t>(o.id) == a.unit) hero = &o;
                if (!hero) break;
                if (ids.empty() || ids.front() == commands::kNoObject) {
                    ids.clear();
                    for (const render::SnapshotObject& o : bview_.frame().objects)
                        if (o.type == hero->type && o.owner == local_player()) ids.push_back(static_cast<uint32_t>(o.id));
                }
                if (!ids.empty()) submit(commands::ev::select(ids));
                bview_.camera().jump_to({hero->position.x, hero->position.y, hero->position.z});
                break;
            }
            case ui::HudAction::Kind::HeroFocusPlanet:  // §2.2 galactic single click: centre on the hero's planet, select it
            case ui::HudAction::Kind::HeroZoomPlanet: {  // §2.2 double click: zoom in (state 0 / 2) / out (1 / 3)
                const galactic::GalacticSim* g = session_ ? session_->galaxy() : nullptr;
                if (!g || screen_ != Screen::Galactic || a.ids.empty() || story_controls_locked()) break;
                const PlanetIndex pi = planet_index(*g, a.ids.front());
                if (pi == kInvalidPlanet) break;
                if (a.kind == ui::HudAction::Kind::HeroFocusPlanet) {
                    const Vec3 c = g->planets()[pi].position;
                    gview_.camera().set_look_at({c.x, c.y, 0.0f});
                    selected_planet_ = a.ids.front();  // the hero's fleet is not selected, no panel opens
                    break;
                }
                if (selected_planet_ == 0) break;
                if (!gview_.camera().zoomed()) {
                    for (const ui::PickSphere& s : gview_.pick_items())
                        if (s.kind == ui::PickKind::Planet && s.id == selected_planet_) {
                            gview_.camera().zoom_to_planet(s.center, s.radius);
                            zoom_planet_ = s.id;  // L1/S4 wave 1: ZOOM_INTO_PLANET's planet
                        }
                } else if (planet_map_open_) {
                    planet_map_open_ = false;  // the advanced planet map closes instead of zooming out
                } else {
                    gview_.camera().zoom_out();
                }
                break;
            }
            case ui::HudAction::Kind::PanelClose:
                for (ui::HudPanel* p : panels())
                    for (const std::string& n : p->shell_names())
                        if (n == a.name && session_) {
                            session_->close_special_dialog();
                            panel_preview_ = 0;
                        }
                break;
            case ui::HudAction::Kind::Button:
                // OI host: game speed (UI.md §2.3 "play/pause and fast forward = game speed"): b_fast_forward[_t] toggles
                // single-player fast mode (GameSession::set_fast_forward; wall-clock pacing only).
                // The HUD's options button (b_option_t / b_option_g) opens the in-game menu, which pauses the sim.
                if (a.name == "b_option_t" || a.name == "b_option_g") {
                    if (session_ && !ui_.front.pause_menu_open()) {
                        paused_ = true;
                        ui_.front.show_pause_menu();
                    }
                    break;
                }
                // P1_DOUBLE_CLICK_MOVE.md §2.1: the attack-move / waypoint / guard order buttons (c_button01 / 03 / 05) arm the
                // one-shot move toggles of the next move order.
                if (screen_ == Screen::Battle && (iequal(a.name, "c_button01") || iequal(a.name, "c_button03") || iequal(a.name, "c_button05"))) {
                    orders_.arm_move_toggle(iequal(a.name, "c_button01")   ? ui::MoveToggle::AttackMove
                                            : iequal(a.name, "c_button03") ? ui::MoveToggle::Waypoint
                                                                           : ui::MoveToggle::GuardMove);
                    break;
                }
                if (a.name == "advanced_planetary") {  // GALACTIC_DIALOGS §4: "Base Layout" enters / leaves the planet map
                    planet_map_open_ = !planet_map_open_ && selected_planet_ != 0 && gview_.camera().zoomed();
                    break;
                }
                if (a.name == "b_story_arc_g") {  // GALACTIC_DIALOGS §2.1: the holocron button opens the story summary
                    open_story_summary();
                    break;
                }
                if (a.name == "b_fast_forward" || a.name == "b_fast_forward_t") {
                    if (session_) {
                        session_->set_fast_forward(!session_->fast_forward());
                        ++fast_toggles_;
                    }
                    break;
                }
                // OI host: the retail per-planet ability button is CommandBarComponents' g_special_ability (TextButton
                // beside the planet, one row per hero ability, GalacticHud): its HudAction carries the hero id in `unit`
                // -> SpecialAbility kind 1 for that hero at the selected planet; the sim's SpecialDialog then opens the
                // panel. (The P4 stand-in buttons - any component named market / sabotage / corrupt / hack activating the
                // first special unit - are gone: the retail rows are the only way in.)
                if (a.name == "g_special_ability") activate_special_here(a.unit);
                break;
            // P4 host: the panels' answers (SpecialAbility kinds 2 / 4 / 5 / 6 / 7 through the command stream); the preview
            // panel of the smoke sends nothing. OI host: slice rows come from the SlicePanel (unit = type).
            case ui::HudAction::Kind::BlackMarketBuy:
                if (session_ && !panel_preview_) session_->buy_black_market(a.unit);
                break;
            case ui::HudAction::Kind::SliceSelect:
                if (session_ && !panel_preview_) session_->slice_tech(a.unit);
                break;
            case ui::HudAction::Kind::SabotageTarget:
                if (session_ && !panel_preview_) session_->sabotage_structure(a.unit);
                break;
            case ui::HudAction::Kind::HackConfirm:
                if (session_ && !panel_preview_) session_->hack_super_weapon();
                break;
            case ui::HudAction::Kind::CorruptionChoice:
                if (session_ && !panel_preview_ && a.value >= 0 && a.value < 8) session_->corrupt_planet(static_cast<int8_t>(a.value));
                break;
            default: break;  // targeting modes, reinforcement drops
        }
    }
}

void App::galactic_input() {
    const uint32_t w = targets_.width, h = targets_.height;
    {  // P1_CAMERA_INPUT.md §1: the scroll clamp box = the union of the planets' spheres (centre +- radius)
        std::vector<ui::Vec3> centres;
        std::vector<float> half;
        for (const ui::PickSphere& s : gview_.pick_items())
            if (s.kind == ui::PickKind::Planet) {
                centres.push_back(s.center);
                half.push_back(s.radius);
            }
        if (!centres.empty()) gview_.set_camera_bounds(ui::galactic_scroll_box(centres, half));
    }
    if (!opt_.hidden || !opt_.smoke.empty()) gview_.camera().update(input_, ui_.bindings, dt_);
    gview_.camera().step(dt_);
    const ui::CameraParams cp = gview_.camera_params(w, h);
    ui::OrderContext ctx;
    ctx.mode = ui::UiMode::Galactic;
    ctx.local_player = local_player();
    ctx.view = ui::make_view(cp, w, h);
    const std::vector<ui::PickSphere> items = gview_.pick_items();
    // P1_CAMERA_INPUT.md §5 wheel up: the hovered planet (or the planet of the hovered fleet) becomes the selected one,
    // then the camera zooms onto the selected planet (the planet panel opens with the zoom).
    if (gview_.camera().take_zoom_in_request() && !story_controls_locked()) {
        if (const auto hov = ui::pick_spheres(items, ui::pixel_ray(ctx.view, input_.mouse_x(), input_.mouse_y()))) {
            const ui::PickSphere& s = items[*hov];
            if (s.kind == ui::PickKind::Planet) selected_planet_ = s.id;
            else if (const uint32_t at = gview_.fleet_planet_id(s.id); at != 0) selected_planet_ = at;
        }
        if (selected_planet_ != 0 && !gview_.camera().zoomed())
            for (const ui::PickSphere& s : items)
                if (s.kind == ui::PickKind::Planet && s.id == selected_planet_) {
                    gview_.camera().zoom_to_planet(s.center, s.radius);
                    zoom_planet_ = s.id;  // L1/S4 wave 1: ZOOM_INTO_PLANET's planet
                }
    }
    // OI host (playtest "fleets can't move between planets") [port]: the translator picks the nearest sphere, so a fleet
    // orbiting inside / in front of its planet's sphere lost the left click to the planet, and a right click on the fleet
    // icons around the destination hit a fleet (no order). The host hands it a per-click list instead: left click - the
    // fleets alone when the ray hits one; right click - the planets plus every orbiting fleet re-labelled as its planet.
    // Left drag from an own fleet onto a planet (the original drag-to-move) is a select + move as well.
    const ui::ButtonState& lb = input_.button(ui::Button::Left);
    const ui::ButtonState& rb = input_.button(ui::Button::Right);
    std::vector<ui::PickSphere> fleets_only, as_planets;
    for (const ui::PickSphere& s : items) {
        if (s.kind == ui::PickKind::Fleet) fleets_only.push_back(s);
        // GRC 1.1: only planets with the raw revealed byte are move targets (right click, drag-to-move).
        if (s.kind == ui::PickKind::Planet && !session_->move_target_revealed(s.id)) continue;
        if (s.kind == ui::PickKind::Planet) {
            as_planets.push_back(s);
        } else if (const uint32_t at = gview_.fleet_planet_id(s.id); at != 0 && session_->move_target_revealed(at)) {
            ui::PickSphere p = s;
            p.id = at;
            p.kind = ui::PickKind::Planet;
            for (const ui::PickSphere& q : items)
                if (q.kind == ui::PickKind::Planet && q.id == at) p.owner = q.owner;
            as_planets.push_back(p);
        }
    }
    const auto mouse_ray = ui::pixel_ray(ctx.view, input_.mouse_x(), input_.mouse_y());
    if (rb.click) ctx.galactic_items = as_planets;
    else if (lb.click && ui::pick_spheres(fleets_only, mouse_ray)) ctx.galactic_items = fleets_only;
    else ctx.galactic_items = items;
    orders_.update(input_, ui_.bindings, ctx);
    if (lb.drag_ended) {
        const auto from = ui::pick_spheres(fleets_only, ui::pixel_ray(ctx.view, lb.drag.x0, lb.drag.y0));
        const auto to = ui::pick_spheres(as_planets, ui::pixel_ray(ctx.view, lb.drag.x1, lb.drag.y1));
        if (from && to && fleets_only[*from].owner == ctx.local_player && as_planets[*to].id != gview_.fleet_planet_id(fleets_only[*from].id)) {
            orders_.set_selection({fleets_only[*from].id});
            drag_moves_.push_back({fleets_only[*from].id, as_planets[*to].id});
        }
    }
    // Planet selection (a left click on a planet; the fleet organiser opens for it) [port].
    if (input_.button(ui::Button::Left).click) {
        const auto hit = ui::pick_spheres(items, ui::pixel_ray(ctx.view, input_.mouse_x(), input_.mouse_y()));
        if (hit && items[*hit].kind == ui::PickKind::Planet) selected_planet_ = items[*hit].id;
    }
    // A left click on nothing (P1_CAMERA_INPUT.md §5): besides Select{}, the planet selection clears while not zoomed in.
    if (orders_.take_planet_deselect() && gview_.camera().zoom_state() == 0) selected_planet_ = 0;
    orders_.take_move_acks();  // no ground moves in galactic
    // A fleet selection selects the planet it orbits (the organiser's planet).
    {
        const uint32_t sel = orders_.selection().empty() ? 0 : orders_.selection().front();
        if (sel != last_fleet_sel_)
            if (const uint32_t at = sel ? gview_.fleet_planet_id(sel) : 0; at != 0) selected_planet_ = at;
        last_fleet_sel_ = sel;
    }
    if (const auto z = orders_.take_planet_zoom())
        for (const ui::PickSphere& s : items)
            if (s.id == *z) {
                gview_.camera().zoom_to_planet(s.center, s.radius);
                zoom_planet_ = s.id;  // L1/S4 wave 1: ZOOM_INTO_PLANET's planet
            }
    std::vector<commands::EventData> cmds;
    for (ui::UiOrder& o : orders_.take_orders()) cmds.push_back(std::move(o.event));
    for (const auto& [fleet, planet] : drag_moves_) {
        cmds.push_back(commands::ev::select({fleet}));
        cmds.push_back(commands::ev::move_object_to_object(fleet, planet));
    }
    drag_moves_.clear();
    std::vector<ui::HotkeyEvent> hk;
    hotkeys_.update(input_, ui_.hotkeys, orders_.selection(), now_s_, cmds, hk);
    std::vector<ui::HudAction> acts;
    {
        const size_t before = cmds.size();
        const bool was_dragging = fleet_drag_active_;
        ui_.galactic_hud.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
        // Fleet drag sounds: GUI_Drag_Fleet when a fleet / unit drag starts, GUI_Drop_Fleet when its drop issued an order.
        const ui::GalacticDrag& d = ui_.galactic_hud.drag();
        fleet_drag_active_ = d.active;
        if (d.active && !was_dragging) audio_.ui("GUI_Drag_Fleet");
        if (!d.active && was_dragging && cmds.size() > before) audio_.ui("GUI_Drop_Fleet");
    }
    // L1/S4 wave 1: STORY_CLICK_GUI - a left click on a HUD component that is not disabled (the HudButtons rule: hidden /
    // disabled components never click), name = its CommandBarComponents name (b_Create_NN, b_zoom, b_planet_tab, ...).
    for (const ui::HudPointerEvent& e : routed_.hud)
        if (e.kind == ui::HudPointerEvent::Kind::Click && e.button == ui::Button::Left && e.component >= 0 &&
            static_cast<size_t>(e.component) < ui_.components.size() &&
            ui_.galactic_hud.buttons().visual(static_cast<uint32_t>(e.component)) != ui::ButtonVisual::Disabled)
            story_ui_.click_gui(ui_.components[static_cast<size_t>(e.component)].name);
    for (ui::HudPanel* p : panels())
        if (p->open()) {
            if (p == &market_) market_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
            else if (p == &slice_) slice_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts, input_.wheel());
            else if (p == &sabotage_) sabotage_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
            else if (p == &corruption_) corruption_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
            else if (p == &hack_) hack_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
        }
    for (const ui::HotkeyEvent& e : hk) {
        // FIXPASS_GALACTIC §4.2 (capture C2): the advisor key (Numpad -) opens the Droid Log, as the dome and "?" do.
        if (e.slot == ui::hk::kHolocron || e.slot == ui::hk::kDroidAdvisor) open_holocron(ui::InfoPage::DroidLog);
    }
    handle_hud_actions(acts);
    route_commands(cmds);
}

const ui::WorldPickTests* App::world_pick_tests() {
    const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
    if (!tm) return nullptr;
    if (pick_radii_.size() != data_.types.size()) pick_radii_ = game::mouse_sphere_radii(data_.types);
    const game::GameSession* gs = session_.get();
    pick_tests_ = game::tactical_pick_tests(*tm, session_->battle(), pick_radii_,
                                            [gs](objects::ObjectId id) { return gs->script_hidden(id); });  // q6 t19 §10.3
    return &pick_tests_;
}

void App::battle_input() {
    const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
    if (!tm || !bview_.active()) return;
    const uint32_t w = targets_.width, h = targets_.height;
    if (!opt_.hidden || !opt_.smoke.empty()) bview_.camera().update(input_, ui_.bindings, dt_);
    const ui::CameraParams cp = bview_.camera_params(w, h);
    ui::OrderContext ctx;
    ctx.mode = bview_.space() ? ui::UiMode::Space : ui::UiMode::Land;
    ctx.local_player = local_player();
    ctx.view = ui::make_view(cp, w, h);
    ctx.trees = &tm->trees();  // const reads between ticks (P1 picking contract)
    ctx.pick_tests = world_pick_tests();  // refixf t08: precise model test, then the override sphere (HUD_GAPS §2.2)
    if (!bview_.space() && bview_.scene()) {
        const terrain::MapData* m = &bview_.scene()->map();
        ctx.ground_height = [m](float x, float y) { return m->terrain.sample_height(x, y); };
    }
    const objects::ObjectStore* st = &tm->primary();
    ctx.type_of = [st](uint32_t id) {
        const objects::ObjectHandle hh = st->find(static_cast<objects::ObjectId>(id));
        return st->exists(hh) ? static_cast<uint32_t>(st->type(hh)) : 0xFFFFFFFFu;
    };
    std::vector<commands::EventData> cmds;
    // story t13 (UI.md §4, flag 0x140a28c91): while a linked battle's P11 input block holds the world gets no mouse input -
    // no drop point, no targeted pick, no selection / orders. The HUD, the camera and the keys are not blocked.
    const bool world_blocked = linked_ui_.input_blocked();
    // Reinforcement drop (TacticalHud BeginReinforcement): the next world left click is the landing point (ground plane
    // z = 0, land: the terrain height there [port]); a right click cancels. The click is not an order.
    bool drop_click = false;
    if (reinforce_unit_ != 0 && !world_blocked) {
        // The release after a right double press is swallowed (P1_DOUBLE_CLICK_MOVE.md §1.1): it does not cancel.
        if (input_.button(ui::Button::Right).click && !input_.button(ui::Button::Right).in_double_press) {
            reinforce_unit_ = 0;
            drop_click = true;
        } else if (input_.button(ui::Button::Left).click) {
            ui::Vec3 p{};
            if (ui::ray_plane_z(ui::pixel_ray(ctx.view, input_.mouse_x(), input_.mouse_y()), 0.0f, p)) {
                if (ctx.ground_height) p.z = ctx.ground_height(p.x, p.y);
                cmds.push_back(ui::reinforce_event(reinforce_unit_, {p.x, p.y, p.z}));
                ++reinforce_drops_;
            }
            reinforce_unit_ = 0;
            drop_click = true;
        }
    }
    // Int-final: the space reinforcement overlay (R5 fog palette 3) while a space drop point is being picked.
    bview_.set_space_reinforcement_overlay(bview_.space() && reinforce_unit_ != 0);
    // Targeted-ability pick (TargetingController, before the order translator: a frame whose click it used is not an
    // order). The click becomes the ability command through the command stream (UI.md §4.1; cmds -> route_commands).
    bool targeted = false;
    if (!drop_click && !world_blocked && targeting_.active()) {
        std::vector<ui::UiOrder> picks;
        targeted = targeting_.update(target_input_, ui_.bindings, ctx, picks);
        for (ui::UiOrder& o : picks) {
            o.event.set_queued(o.queued);
            cmds.push_back(std::move(o.event));
        }
    }
    if (!drop_click && !targeted && !world_blocked) orders_.update(input_, ui_.bindings, ctx);
    // Orders carry their queued flag (shift: queueable actions) into the command stream; voice per command (UiOrder::voice).
    std::vector<uint8_t> voice(cmds.size(), 1);
    for (ui::UiOrder& o : orders_.take_orders()) {
        o.event.set_queued(o.queued);
        cmds.push_back(std::move(o.event));
        voice.push_back(o.voice ? 1 : 0);
    }
    for (const ui::MoveAck& a : orders_.take_move_acks()) move_ack(a);  // P1_DOUBLE_CLICK_MOVE.md §2.3
    if (!drop_click && reticle_orders(ctx, cmds)) voice.assign(cmds.size(), 1);  // HUD_GAPS §1.6 (orders rewritten)
    if (hud_hit_debug_) {
        const int32_t px = hud_hit_px_ >= 0 ? hud_hit_px_ : input_.mouse_x(), py = hud_hit_px_ >= 0 ? hud_hit_py_ : input_.mouse_y();
        hud_hit_dump("tactical", ui_.tactical_hud.hit_test(px, py), px, py);
    }
    std::vector<ui::HotkeyEvent> hk;
    hotkeys_.update(input_, ui_.hotkeys, orders_.selection(), now_s_, cmds, hk);
    // Camera hotkeys (P1): group / selection / home focus points from the battle snapshot.
    const render::InterpolatedFrame& fr = bview_.frame();
    auto centroid = [&fr](const std::function<bool(const render::SnapshotObject&)>& want) -> std::optional<ui::Vec3> {
        double x = 0, y = 0;
        uint32_t n = 0;
        for (const render::SnapshotObject& o : fr.objects)
            if (want(o)) {
                x += o.position.x;
                y += o.position.y;
                ++n;
            }
        if (!n) return std::nullopt;
        return ui::Vec3{static_cast<float>(x / n), static_cast<float>(y / n), 0.0f};
    };
    ui::CameraFocus focus;
    const std::vector<uint32_t> sel = orders_.selection();
    const int32_t me = local_player();
    focus.selection = [&] { return centroid([&](const render::SnapshotObject& o) { return std::binary_search(sel.begin(), sel.end(), static_cast<uint32_t>(o.id)); }); };
    focus.home = [&] { return centroid([me](const render::SnapshotObject& o) { return o.owner == me; }); };
    // Control groups live in the sim (SimHost::control_group, assigned / recalled by ControlGroup commands): const read.
    const sim::SimHost& host = session_->host();
    focus.group = [&](int8_t g) -> std::optional<ui::Vec3> {
        if (g < 0 || g > 9) return std::nullopt;
        const std::vector<objects::ObjectId>& ids = host.control_group(static_cast<uint8_t>(me), static_cast<uint8_t>(g));
        return centroid([&ids](const render::SnapshotObject& o) { return std::binary_search(ids.begin(), ids.end(), o.id); });
    };
    ui::apply_camera_hotkeys(hk, bview_.camera(), focus);
    // P1_DOUBLE_CLICK_MOVE.md §2.1: the attack-move (T), guard (G) and waypoint hotkeys arm the next move order's toggle.
    for (const ui::HotkeyEvent& e : hk) {
        if (e.slot == ui::hk::kAttackMove) orders_.arm_move_toggle(ui::MoveToggle::AttackMove);
        else if (e.slot == ui::hk::kGuard) orders_.arm_move_toggle(ui::MoveToggle::GuardMove);
        else if (e.slot == ui::hk::kWaypoint) orders_.arm_move_toggle(ui::MoveToggle::Waypoint);
        else if (e.slot == ui::hk::kDroidAdvisor) open_holocron(ui::InfoPage::DroidLog);  // capture C2 (no galaxy in a skirmish: nothing)
    }
    std::vector<ui::HudAction> acts;
    ui_.tactical_hud.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
    // L1/S4 wave 1 (tactical): STORY_CLICK_GUI to the tactical plot set - the galactic rule (left click on a component that
    // is not disabled, name = its CommandBarComponents name: b_reinforcement, b_special_weapon, ...) on the tactical HUD.
    story_ui_.set_battle(tactical_story());
    if (story_ui_.battle())
        for (const ui::HudPointerEvent& e : routed_.hud)
            if (e.kind == ui::HudPointerEvent::Kind::Click && e.button == ui::Button::Left && e.component >= 0 &&
                static_cast<size_t>(e.component) < ui_.components.size() &&
                ui_.tactical_hud.buttons().visual(static_cast<uint32_t>(e.component)) != ui::ButtonVisual::Disabled)
                story_ui_.click_gui(ui_.components[static_cast<size_t>(e.component)].name);
    if (hero_panel_.open()) hero_panel_.update_input(routed_, input_.shift(), input_.ctrl(), now_s_, cmds, &acts);
    handle_hud_actions(acts);
    voice.resize(cmds.size(), 1);
    route_commands(cmds, voice);
}

// P1_DOUBLE_CLICK_MOVE.md §2.3 (`0x418b0`): the particle by priority attack > guard > double > normal (a double first kills
// the nearest live normal ack silently), scaled GUI_Move_Acknowledge_Scale_Land / _Space; the radar ping
// Default_Attack_Click / Default_Click, a double removing the playing Default_Click and playing Default_Double_Click.
void App::move_ack(const ui::MoveAck& a) {
    ++move_acks_;
    if (a.kill_nearest_move_ack() && bview_.kill_nearest_move_ack(a.point)) ++move_ack_kills_;
    const ui::AckEffect fx = a.effect();
    if (bview_.spawn_move_ack(ack_fx_[static_cast<size_t>(fx)], a.point, a.space ? ack_scale_space_ : ack_scale_land_,
                              fx == ui::AckEffect::Move))
        ++move_acks_drawn_;
    if (a.remove_click_radar()) {
        const std::string& click = ack_radar_[static_cast<size_t>(ui::AckRadar::Click)];
        std::vector<uint32_t> gone;
        for (const ui::RadarEvent& e : radar_events_.items())
            if (iequal(e.type, click)) gone.push_back(e.id);
        for (const uint32_t id : gone) radar_events_.remove(id);
    }
    const std::string& radar = ack_radar_[static_cast<size_t>(a.radar())];
    if (!radar.empty()) radar_events_.add(radar_settings_, radar, a.point.x, a.point.y);
}

ui::CameraTuning App::option_tuning(ui::CameraTuning t) const {
    if (!live_input()) return t;  // smoke / replay runs: the defaults (machine and profile independent)
    t.scroll_speed_option = ui::scroll_speed_option_from_slider(gameplay_.scroll_speed);  // IDC_SCROLL_SPEED_SLIDER
    t.windowed = true;  // [port] the game window is always windowed (no exclusive full screen)
    t.absolute_scrolling = gameplay_.absolute_scroll;
    t.right_button_scroll = gameplay_.right_button_scroll;
    t.both_buttons_rotate = gameplay_.both_buttons_rotate;
    return t;
}

void App::apply_camera_tuning() {
    gview_.camera().set_tuning(option_tuning(gview_.camera().tuning()));
    if (bview_.active()) bview_.camera().set_tuning(option_tuning(bview_.camera().tuning()));
}
void App::handle_menu_actions(const std::vector<ui::MenuAction>& acts) {
    using K = ui::MenuAction::Kind;
    for (const ui::MenuAction& a : acts) {
        std::printf("menu: %s %s %s\n", ui::menu_action_name(a.kind), a.campaign.c_str(), a.map.c_str());
        switch (a.kind) {
            case K::StartCampaign: {
                SessionSpec s;
                s.kind = SessionKind::Campaign;
                s.campaign = a.campaign;
                s.faction = a.faction;
                s.ask_battle_choice = true;  // human conflicts: Fight / Auto-resolve / Retreat message box
                s.difficulty = a.difficulty;
                s.seed = opt_.seed;
                s.battle_max_frames = opt_.battle_max_frames;
                s.battle_end_hold_frames = kBattleEndHoldFrames;
                if (!start_session(s)) {
                    notes_.push_back("campaign " + a.campaign + " did not start");
                    open_main_menu();
                }
                break;
            }
            case K::StartSkirmish: {
                SessionSpec s;
                s.kind = SessionKind::Skirmish;
                s.skirmish_map = a.map;
                s.skirmish.human_faction = a.factions.size() > 0 && !a.factions[0].empty() ? a.factions[0] : "Rebel";
                s.skirmish.ai_faction = a.factions.size() > 1 && !a.factions[1].empty() ? a.factions[1] : "Empire";
                if (iequal(s.skirmish.ai_faction, s.skirmish.human_faction))  // [port] the AI needs a hostile faction
                    s.skirmish.ai_faction = iequal(s.skirmish.human_faction, "Empire") ? "Rebel" : "Empire";
                for (const int32_t c : a.colors) s.skirmish.colors.push_back(static_cast<uint8_t>(std::clamp(c, 0, 8)));
                s.seed = opt_.seed;
                s.battle_max_frames = opt_.battle_max_frames;
                s.battle_end_hold_frames = kBattleEndHoldFrames;
                open_buy_units(s);  // the skirmish starts from the buy-units screen
                break;
            }
            case K::ApplyCamera:  // [port: Gabriel] FOV settings + queue modifier
                // The tactical FOV replaces TacticalCameras.xml's only once the player moved it off the default.
                if (a.camera.tactical_fov_deg != camera_.tactical_fov_deg) tactical_fov_set_ = true;
                camera_ = a.camera;
                gview_.camera().set_fov_deg(camera_.galactic_fov_deg);
                bview_.camera().set_fov_override(tactical_fov_set_ ? camera_.tactical_fov_deg : 0.0f);
                sync_control_bindings(ui_.hotkeys, camera_, ui_.bindings);  // OI host: hotkeys + queue modifier
                break;
            case K::ApplyGameplay:  // HUD_GAPS §2.2: IDC_DELAY_TOOLTIPS -> Long_Encyclopedia_Delay
                gameplay_ = a.gameplay;
                ency_cfg_.delay_tooltips = gameplay_.delay_tooltips;
                ency_.set_config(ency_cfg_);
                apply_camera_tuning();  // P1_CAMERA_INPUT.md §3.1 mouse / scroll options
                break;
            case K::SaveGame: save_game_as(a.save); break;          // N1 / t09
            case K::LoadGame: load_saved_game(a.save); break;
            case K::DeleteSave: delete_saved_game(a.save); break;
            case K::ResumeGame: paused_ = false; break;
            case K::QuitToMainMenu:
                paused_ = false;
                end_session();
                screen_ = Screen::Menu;
                open_main_menu();
                break;
            case K::ExitGame: screen_ = Screen::Quit; break;
            case K::PreviewAudio:
            case K::ApplyAudio: audio_.set_volumes(a.audio.master, a.audio.sfx, a.audio.speech, a.audio.music); break;
            case K::ApplyVideo:  // the chosen resolution resizes the window (the resize path recreates swapchain + targets)
                ++video_applies_;
                hardware_mouse_ = a.video.hardware_mouse;
                vsync_mode_ = a.video.vsync_mode;
                set_present_mode(!a.video.vsync ? rhi::PresentMode::Immediate
                                 : vsync_mode_ == ui::VsyncMode::Mailbox ? rhi::PresentMode::Mailbox : rhi::PresentMode::Fifo);
                opt_.frame_cap = a.video.frame_cap;
                if (a.video.resolution >= 0 && static_cast<size_t>(a.video.resolution) < display_modes_.size()) {
                    // The list item is a full mode: the window size and the monitor's refresh rate (game_display.hpp).
                    const DisplayMode m = display_modes_[static_cast<size_t>(a.video.resolution)];
                    if (m.width != win_.width() || m.height != win_.height() || (m.hz && m.hz != refresh_hz_)) {
                        std::string line;
                        apply_display_mode(win_.native_handle(), m, &line);
                        std::printf("video: %s\n", line.c_str());
                    }
                }
                update_display_rate(true);
                // t12: tried now, stored on the Yes answer of "keep these settings?" (or as the old mode on No / timeout).
                if (update_video_profile(video_profile_, a.video, display_modes_)) {
                    // t11 [M]: a quality level picked in the Detail Level list is a preset for the seven advanced detail options.
                    if (a.video.detail_picked) {
                        adv_video_ = detail_preset(adv_video_, video_profile_.detail);
                        video_profile_.advanced = adv_video_;
                    }
                    std::printf("video: anti-aliasing %s (strength %d), detail level %d\n", taa_active(video_profile_.anti_alias) ? "TAA" : "off",
                                video_profile_.anti_alias, video_profile_.detail);
                    save_video_options();
                }
                break;
            case K::ApplyAdvancedVideo:  // AO goes into the next frame's RecordOptions; anisotropy latches next frame
                adv_video_ = a.advanced;
                gpu_.renderer.set_anisotropy(static_cast<uint32_t>(adv_video_.anisotropy));
                std::printf("video: ambient occlusion %s, anisotropy %dx requested\n", adv_video_.ambient_occlusion ? "on" : "off",
                            adv_video_.anisotropy);
                video_profile_.advanced = adv_video_;
                sync_detail(video_profile_);  // t43: the Detail value follows
                save_video_options();
                break;
            case K::AutoDetectVideo: {  // gfxf t02: a quality level from the GPU; the open dialogs show it
                const rhi::DeviceCaps& caps = ctx_.caps();
                GpuClass gpu;
                gpu.kind = caps.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU     ? GpuKind::Discrete
                           : caps.type == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? GpuKind::Integrated
                                                                                 : GpuKind::Other;
                gpu.memory_bytes = caps.device_local_bytes;
                gpu.sampler_anisotropy = caps.sampler_anisotropy;
                gpu.max_anisotropy = caps.max_anisotropy;
                static constexpr const char* kQualityNames[] = {"low", "medium", "high", "ultra"};
                const AutoVideoChoice choice = recommend_video(gpu);
                apply_auto_video(video_profile_, choice);
                adv_video_ = video_profile_.advanced;
                gpu_.renderer.set_anisotropy(static_cast<uint32_t>(adv_video_.anisotropy));
                std::printf("video: auto detect (%s, %llu MiB): %s detail, anti-aliasing strength %d, anisotropy %dx, ambient occlusion %s\n",
                            caps.name.c_str(), static_cast<unsigned long long>(caps.device_local_bytes >> 20),
                            kQualityNames[choice.quality], video_profile_.anti_alias, adv_video_.anisotropy, adv_video_.ambient_occlusion ? "on" : "off");
                save_video_options();
                ui_.front.refresh_video_dialogs();
                break;
            }
            default: break;
        }
    }
}

}  // namespace alamo_game_app

// alamo_game App: screens, panels, saves, battle prompts, holocron / encyclopedia, cursor (split from game_main.cpp).
#include "game_app.hpp"
#include "alamo/vfs/vfs.hpp"  // q6: the mod layer decides the buy list's [port interim rule]

namespace alamo_game_app {

// ------------------------------------------------------------------------------------------------ screens / panels
uint32_t App::display_planet_type(const galactic::GalacticSim& g, PlanetIndex p) const {
    const uint32_t own = p < g.planets().size() ? g.planets()[p].type : galactic::kNoIndex;
    if (!session_ || session_->spec().kind != SessionKind::Skirmish) return own;
    const uint32_t t = g.rules().find_type(skirmish_map_planet_type(g.rules(), session_->spec().skirmish_map));
    return t != galactic::kNoIndex ? t : own;
}

// [port: Gabriel] the window's monitor refresh rate (re-read every 2 s at 60 Hz: the window may move to another
// monitor) and the frame rate cap it sets for "Display refresh rate". Live play only: smoke / hidden runs are uncapped.
void App::update_display_rate(bool force) {
    if (!force && frames_ - refresh_checked_frame_ < 120) return;
    refresh_checked_frame_ = frames_;
    const uint32_t hz = monitor_refresh_hz(win_.native_handle());
    if (hz != refresh_hz_) std::printf("display: %u Hz\n", hz);
    refresh_hz_ = hz;
    const int32_t cap = opt_.frame_cap == ui::kFrameCapDisplay ? static_cast<int32_t>(refresh_hz_) : opt_.frame_cap;
    // BM2a: a real-time benchmark run uses its preset's cap (the fixed-frames mode never caps).
    const bool capped = live_input() || (bench_active() && !bench_fixed());
    limiter_.set_fps(capped && cap > 0 ? static_cast<uint32_t>(cap) : 0u);
}

// A new swap mode takes effect on a swapchain recreate (same size).
void App::set_present_mode(rhi::PresentMode m) {
    if (m == opt_.mode) return;
    opt_.mode = m;
    sc_.set_present_mode(m);
    gpu_.ring.wait_idle();
    sc_.recreate(win_.width(), win_.height());
    const VkPresentModeKHR pm = sc_.present_mode();
    std::printf("video: present mode %s\n", pm == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : pm == VK_PRESENT_MODE_IMMEDIATE_KHR ? "immediate" : "fifo");
}

// [port: Gabriel] render interpolation between the last two sim snapshots (InterpMode::Smooth), so motion stays smooth
// at any refresh rate / frame rate above the tick rate: alpha = time since the last tick over the last tick interval
// (clamped to 1..2 pacing periods). Smoke and hidden runs draw the current snapshot (alpha 1, the original behaviour).
float App::render_alpha() {
    if (bench_fixed()) return bench_alpha();  // BM2a: (counted frame within the tick + 1) / frames per tick
    // BM2a: a real-time benchmark interpolates like live play.
    if ((!live_input() && !bench_active()) || !session_ || last_tick_ms_ == prev_tick_ms_) return 1.0f;
    const uint32_t pace = session_->host().pacing().period_ms();
    const uint32_t period = std::clamp(last_tick_ms_ - prev_tick_ms_, pace, 2u * pace);
    return render::tick_alpha(static_cast<uint32_t>(ms_since(t0_)), last_tick_ms_, period);
}

bool App::skirmish_map_is_land(const std::string& map) const {
    if (skirmish_catalog_)
        if (const game::SkirmishMapEntry* e = game::find_skirmish_map(*skirmish_catalog_, map); e && (e->kind == 1 || e->kind == 2)) return e->kind == 1;
    return map.find("_LAND") != std::string::npos;
}

// Skirmish buy-units screen (IDD_SKIRMISH_BUY_UNITS_DIALOG). [port] a pre-battle screen (the original single-player
// skirmish has none, SKIRMISH_SETUP §2.7): 10000 credits; bought units replace the default fleet, none bought = the
// default fleet. The list is the original dialog's (game::skirmish_buy_list: cost > 0, Affiliation, Available_In_Skirmish
// or the map kind's build tab, not cinematic-only, named heroes hidden, most expensive first, no cap: the list box scrolls).
void App::open_buy_units(const SessionSpec& spec) {
    pending_skirmish_ = spec;
    bought_.clear();
    buy_ = {};
    buy_.credits = opt_.smoke == "skirmish" ? 20000 : 10000;  // [smoke] room for a fleet larger than the space unit cap
    const bool land = skirmish_map_is_land(spec.skirmish_map);
    const uint32_t fac = data_.rules.find_faction(spec.skirmish.human_faction);
    game::SkirmishBuyOptions bo;
    bo.heroes = false;  // [port] the staging screen has no heroes option (0x140b154bc bit 0): named heroes stay hidden
    // [port interim rule, Gabriel 2026-11-04: mod-only port rules keep vanilla behaviour] with a mod layer (MODPATH=) only the
    // types some Tactical_Buildable_Objects_Multiplayer list names (drops Thrawn's Revenge's option / cheat / marker rows).
    bo.multiplayer_list_only = data_.vfs && !data_.vfs->mod_dirs().empty();
    for (const uint32_t t : game::skirmish_buy_list(data_.rules, data_.types, fac, land, bo)) {
        const galactic::TypeInfo& ti = data_.rules.types[t];
        ui::BuyUnitEntry e;
        e.type = ti.name;
        e.name = type_label(data_, ui_.text, t);
        e.cost = ti.build_cost;
        e.icon = type_icon(t);
        buy_.units.push_back(std::move(e));
    }
    std::printf("skirmish buy list: %zu rows (%s, %s)%s\n", buy_.units.size(), spec.skirmish.human_faction.c_str(), land ? "land" : "space",
                bo.multiplayer_list_only ? " [mod: Tactical_Buildable_Objects_Multiplayer types only]" : "");
    if (!info_.show_buy_units(buy_)) {  // no dialog in the script: start with the defaults
        if (!start_session(spec)) open_main_menu();
    }
}

void App::handle_screen_actions(const std::vector<ui::ScreenAction>& acts) {
    using K = ui::ScreenAction::Kind;
    for (const ui::ScreenAction& a : acts) {
        switch (a.kind) {
            case K::BuyUnit:
            case K::SellUnit:
                for (ui::BuyUnitEntry& u : buy_.units)
                    if (u.type == a.map) {
                        if (a.kind == K::BuyUnit && buy_.credits >= u.cost) {
                            buy_.credits -= u.cost;
                            ++u.owned;
                            ++bought_[u.type];
                        } else if (a.kind == K::SellUnit && u.owned > 0) {
                            buy_.credits += u.cost;
                            --u.owned;
                            if (--bought_[u.type] == 0) bought_.erase(u.type);
                        }
                    }
                for (ui::BuyUnitEntry& u : buy_.units) u.can_buy = buy_.credits >= u.cost;
                info_.set_buy_units(buy_);
                break;
            case K::SellAllUnits:
                for (ui::BuyUnitEntry& u : buy_.units) {
                    buy_.credits += static_cast<int64_t>(u.cost) * u.owned;
                    u.owned = 0;
                    u.can_buy = true;
                }
                bought_.clear();
                info_.set_buy_units(buy_);
                break;
            case K::BuyUnitsDone: {
                SessionSpec s = pending_skirmish_;
                std::vector<std::string> units;
                for (const auto& [type, n] : bought_)
                    for (uint32_t k = 0; k < n; ++k) units.push_back(type);
                const bool land = skirmish_map_is_land(s.skirmish_map);
                if (!units.empty()) (land ? s.skirmish.ground_human : s.skirmish.units_human) = units;
                if (!start_session(s)) {
                    notes_.push_back("skirmish on " + s.skirmish_map + " did not start");
                    open_main_menu();
                }
                break;
            }
            case K::BuyUnitsBack: open_main_menu(); break;
            case K::StorySummaryClosed:
                story_summary_open_ = false;
                break;
            case K::LandInvade:  // ui3 t05: "Invade System" - the Invade command at the battle's planet, then as Continue
                if (session_ && invade_planet_ != 0) {
                    commands::EventData ev(commands::EventType::Invade);
                    ev.set("planet", invade_planet_);
                    submit(std::move(ev));
                }
                invade_planet_ = 0;
                [[fallthrough]];
            case K::BattleEndContinue:
                battle_end_open_ = false;
                story_ui_.generic("battle_end_closed");  // L1/S4 wave 1: STORY.md 4.1 step 6, GENERIC "battle_end_closed"
                if (battle_end_auto_) audio_.galactic_resume();  // the win / lose music ends with the screen
                battle_end_auto_ = false;
                if (session_ && session_->spec().kind == SessionKind::Skirmish) {
                    skirmish_ended_ = true;
                    screen_ = Screen::Menu;
                    if (opt_.smoke.empty()) session_.reset();  // smoke runs keep it for the CRC check
                    open_main_menu();
                }
                break;
            case K::MessageBoxResult:
                if (a.tag == "battle_choice") {  // [port] a scripted message-box prompt: button 1 = Auto-Resolve
                    answer_battle_prompt(a.button == 1);
                } else if (a.tag == "retry") {  // Lua Activate_Retry_Dialog: Retry restarts the session, Quit leaves it
                    if (a.button == 0 && session_) {
                        const SessionSpec spec = session_->spec();
                        if (!start_session(spec)) open_main_menu();
                    } else {
                        end_session();
                        screen_ = Screen::Menu;
                        open_main_menu();
                    }
                }
                break;
            case K::CampaignScoreClosed:  // the campaign is over: back to the main menu (smoke runs keep the session)
                screen_ = Screen::Menu;
                if (opt_.smoke.empty()) end_session();
                ui_.front.set_in_game(false);
                open_main_menu();
                break;
            case K::StorySummaryGoal:  // GALACTIC_DIALOGS §2.2: a record chosen in the goal list -> refill with it
                if (a.button >= 0 && static_cast<size_t>(a.button) < summary_rows_.size()) summary_entry_ = summary_rows_[static_cast<size_t>(a.button)];
                open_story_summary();
                break;
            case K::StorySummaryFilter:
                summary_completed_ = a.button == 1;
                summary_entry_ = 0;    // the tab's first row
                open_story_summary();  // refill
                break;
            // P4 host: the answer goes through the command stream; its result comes at the command phase
            // (GameSession::port_event). The story summary's Abandon is permanently disabled (P4_SIM_SYSTEMS §9.1).
            case K::AutoResolve:  // the retreat dialog's Autoresolve (tactical)
                if (session_ && session_->auto_resolve_battle()) ++tactical_auto_resolves_;
                std::printf("battle: auto-resolve requested\n");
                break;
            default: break;  // retreat (its command was sent), planet info / story / holocron closes: nothing to do
        }
    }
}

void App::show_battle_end(bool dialog) {
    ui::BattleEndInfo info;
    // ui3 t05 (GALACTIC_DIALOGS §3.1, the same IDD_BATTLE_END_DIALOG as after an auto-resolve): the winner is the ended
    // battle's result (CampaignBattles::last_battle_end, after an auto-resolve from tactical too), not a log string.
    const galactic::GalacticSim* g = galaxy();
    const sim::CampaignBattles* cb = session_ ? session_->battles() : nullptr;
    const sim::CampaignBattles::LastBattleEnd end = cb ? cb->last_battle_end() : sim::CampaignBattles::LastBattleEnd{};
    const int32_t me = local_player();
    std::string winner;  // the winner's faction name (the lost-battle music names it)
    if (end.valid) {
        info.victory = end.winner == me;
        if (g && end.winner >= 0 && static_cast<size_t>(end.winner) < g->players().size()) {
            const uint32_t f = g->players()[static_cast<size_t>(end.winner)].faction;
            if (f < g->rules().factions.size()) winner = g->rules().factions[f].name;
        }
    }
    info.title = info.victory ? txt("TEXT_WIN_BATTLE", U"You have won the battle!") : txt("TEXT_LOST_BATTLE", U"You have lost the battle!");
    // L1/S4 wave 1 (SCRIPT_GAPS.md 4.2): the local player lost a battle that ran a tactical plot set -> STORY_MISSION_LOST
    // to the galactic story, name = that plots file (the shipped Param1 values) [inferred: the original's "monitor
    // MISSION_LOST name" source is not specified].
    if (!info.victory && !battle_plots_.empty()) story_ui_.mission_lost(battle_plots_);
    battle_plots_.clear();
    info.planet = battle_place_;  // QA U1: the localized place, never the log's map file name
    // Playtest QA D1 / V1: the ended battle's own records (the old per-frame map also kept the attract demo's losses and
    // missed the final tick's deaths).
    std::vector<game::BattleLossRecord> dead;
    if (session_ && session_->battles())
        for (const sim::TacticalBattle::RemovalRecord& r : session_->battles()->last_battle_losses()) dead.push_back({r.owner, r.object_type});
    const game::BattleKillCount kills = game::count_battle_kills(dead, local_player());  // t41: campaign score, final tick included
    tally_.kills += kills.kills;
    tally_.losses += kills.losses;
    // ui3 t05: the battle time (TEXT_BATTLE_TIME, the battle's frames / 30), salvage (the bounty of the units the local
    // player's enemies lost: TEXT_SUMMARY_BOUNTY_COLLECTED), each side's lost credit value, the efficiency and the lost
    // named heroes' portraits (5 per side), by the auto-resolve screen's rules (battle_summary.hpp); types costing 0 are
    // not listed (§3.1).
    const game::BountyTable bounty_table = game::load_bounty_table(data_.db.constants(), data_.types);
    std::vector<game::ArUnitRecord> records;
    game::BattleLosses lost;
    for (const game::BattleLossRecord& d : dead) {
        if (d.owner < 0) continue;
        game::ArUnitRecord r;
        r.owner = d.owner;
        r.destroyed = true;
        r.cost = d.type < data_.rules.types.size() ? static_cast<int64_t>(data_.rules.types[d.type].build_cost) : 0;
        r.bounty = game::type_bounty(bounty_table, data_.types, static_cast<objects::TypeIndex>(d.type));
        records.push_back(r);
        if (g && is_named_hero(*g, d.type)) {
            std::vector<std::string>& heroes = d.owner == me ? info.your_heroes : info.enemy_heroes;
            const std::string icon = type_icon(d.type);
            if (!icon.empty() && heroes.size() < 5 && std::find(heroes.begin(), heroes.end(), icon) == heroes.end()) heroes.push_back(icon);
        }
        if (r.cost > 0) ++lost[d.owner][d.type];
    }
    add_loss_rows(info, lost, me);
    const game::ArSummary sum = game::summarize_auto_resolve(records, me, info.victory);
    info.battle_seconds = end.valid ? static_cast<int64_t>(end.frames / 30u) : -1;
    info.bounty = sum.bounty;
    info.your_loss_value = sum.loss_value[0];
    info.enemy_loss_value = sum.loss_value[1];
    info.your_efficiency = sum.efficiency[0];
    info.enemy_efficiency = sum.efficiency[1];
    // §3.1 IDC_LAND_INVADE_BUTTON "Invade System": a won space battle with an invasion possible [port: a campaign, the
    // planet not the local player's and accessible, and a stopped own fleet there carrying a ground unit - the
    // GalacticSim::invade conditions; the click sends the Invade command for every such unit].
    invade_planet_ = 0;
    if (g && end.valid && end.space && info.victory && session_->spec().kind != SessionKind::Skirmish && end.planet < g->planets().size()) {
        const auto pi = static_cast<PlanetIndex>(end.planet);
        const galactic::PlanetState& ps = g->planets()[pi];
        bool ground = false;
        for (const uint32_t fi : g->fleets_at(pi, me))
            if (!g->fleets()[fi].moving)
                for (const objects::ObjectHandle h : g->fleets()[fi].contents)
                    ground = ground || (g->store().alive(h) && g->rules().types[g->store().type(h)].kind == galactic::UnitKind::GroundUnit);
        info.can_invade = ground && g->surface_accessible(pi) && g->store().owner(ps.object) != me;
        if (info.can_invade) invade_planet_ = static_cast<uint32_t>(g->store().id(ps.object));
    }
    info.can_save_replay = false;  // N1
    if (!dialog) return;  // story t13: a linked battle's skip - the story and score bookkeeping above, no screen, no music
    // Battle-end win / lose music, mode 16 (STRATEGIC_GAPS §7.4); a lost battle names the winner as the enemy faction.
    audio_.battle_end(info.victory, info.victory ? std::string_view() : std::string_view(winner));
    battle_end_open_ = screens_.show_battle_end(info);
}

// GALACTIC_DIALOGS §3.1 step 2: IDD_BATTLE_END_DIALOG after the local player's auto-resolve (pauses: ticks wait for
// Continue). Title TEXT_WIN_BATTLE / TEXT_LOST_BATTLE, the planet, the battle time, salvage (TEXT_SUMMARY_BOUNTY_COLLECTED),
// both sides' lost units by type (the forces recorded at the choice that are gone now; types costing 0 skipped) with their
// credit values, the lost named heroes' icons (5 per side) and the combat efficiency. Playtest 2 t13: time, bounty and
// efficiency come from game::summarize_auto_resolve (battle_summary.hpp; display-only [M] rules, no sim state touched).
void App::show_auto_resolve_end(const galactic::ConflictLogEntry& e) {
    const galactic::GalacticSim* g = galaxy();
    if (!g) return;
    const int32_t me = local_player();
    ui::BattleEndInfo info;
    info.victory = e.winner == me;
    info.title = info.victory ? txt("TEXT_WIN_BATTLE", U"You have won the battle!") : txt("TEXT_LOST_BATTLE", U"You have lost the battle!");
    info.planet = planet_label(data_, ui_.text, *g, e.planet);
    const game::BountyTable bounty_table = game::load_bounty_table(data_.db.constants(), data_.types);
    std::vector<game::ArUnitRecord> records;
    records.reserve(auto_units_.size());
    game::BattleLosses lost;
    for (const AutoUnit& u : auto_units_) {
        const uint32_t t = static_cast<uint32_t>(u.type);
        const bool gone = !g->store().alive(u.h);
        game::ArUnitRecord r;
        r.owner = u.owner;
        r.destroyed = gone;
        r.cost = t < data_.rules.types.size() ? static_cast<int64_t>(data_.rules.types[t].build_cost) : 0;
        r.bounty = game::type_bounty(bounty_table, data_.types, u.type);
        records.push_back(r);
        if (!gone) continue;
        if (is_named_hero(*g, t)) {
            std::vector<std::string>& heroes = u.owner == me ? info.your_heroes : info.enemy_heroes;
            const std::string icon = type_icon(t);
            if (!icon.empty() && heroes.size() < 5) heroes.push_back(icon);
        }
        if (r.cost <= 0) continue;  // §3.1: types costing 0 are not listed
        ++lost[u.owner][u.type];
    }
    auto_units_.clear();
    add_loss_rows(info, lost, me);
    const game::ArSummary sum = game::summarize_auto_resolve(records, me, info.victory);
    info.battle_seconds = sum.battle_seconds;
    info.bounty = sum.bounty;
    info.your_loss_value = sum.loss_value[0];
    info.enemy_loss_value = sum.loss_value[1];
    info.your_efficiency = sum.efficiency[0];
    info.enemy_efficiency = sum.efficiency[1];
    info.can_save_replay = false;
    audio_.battle_end(info.victory, std::string_view());
    battle_end_open_ = screens_.show_battle_end(info);
    battle_end_auto_ = battle_end_open_;
    if (battle_end_open_) ++auto_results_;
    const bool attacker = e.attacker == me;
    std::printf("auto-resolve end: %s at %s, time %02lld:%02lld:%02lld, salvage %lld, units lost yours %u ($%lld) / enemy %u ($%lld), "
                "efficiency %d%% / %d%%, heroes lost %zu / %zu (sim combatants yours %u / enemy %u)\n",
                info.victory ? "victory" : "defeat", g->map().planets.name[e.planet].c_str(),
                static_cast<long long>(sum.battle_seconds / 3600), static_cast<long long>((sum.battle_seconds / 60) % 60),
                static_cast<long long>(sum.battle_seconds % 60), static_cast<long long>(sum.bounty), sum.units_lost[0],
                static_cast<long long>(sum.loss_value[0]), sum.units_lost[1], static_cast<long long>(sum.loss_value[1]),
                sum.efficiency[0], sum.efficiency[1], info.your_heroes.size(), info.enemy_heroes.size(),
                attacker ? e.attacker_losses : e.defender_losses, attacker ? e.defender_losses : e.attacker_losses);
}

void App::add_loss_rows(ui::BattleEndInfo& info, const game::BattleLosses& lost, int32_t me) const {
    for (const auto& [owner, types] : lost)
        for (const auto& [t, n] : types) {
            ui::BattleEndUnit u;
            u.name = type_label(data_, ui_.text, t);
            u.icon = type_icon(t);
            u.count = n;
            (owner == me ? info.your_losses : info.enemy_losses).push_back(std::move(u));
        }
}

std::u32string App::txt(std::string_view id, std::u32string_view fallback) const {
    if (const std::u16string* s = ui_.text.find(id)) return ui::utf16_to_utf32(*s);
    return std::u32string(fallback);
}

// [port] the faction's Text_ID (Factions.xml <Text_ID>; modc t20: read, not built from the name), else its name.
std::u32string App::faction_label(int32_t player) const {
    const galactic::GalacticSim* g = galaxy();
    if (!g || player < 0 || static_cast<size_t>(player) >= g->players().size()) return {};
    const uint32_t f = g->players()[static_cast<size_t>(player)].faction;
    if (f >= g->rules().factions.size()) return {};
    const galactic::FactionRules& fr = g->rules().factions[f];
    if (fr.text_id.empty()) return ui::utf8_to_utf32(fr.name);
    return txt(fr.text_id, ui::utf8_to_utf32(fr.name));
}

void App::screen_line(std::u32string text, ui::Rgba8 color) {
    std::printf("screen text: %s\n", utf8_of(text).c_str());
    ++screen_line_count_;
    screen_lines_.push_back({std::move(text), color, now_s_});
    if (screen_lines_.size() > 8) screen_lines_.erase(screen_lines_.begin());
}

// ------------------------------------------------------------------------------------------------ N1 / t09 saves
std::string App::session_title() const {
    if (!session_) return {};
    const SessionSpec& s = session_->spec();
    if (s.kind == SessionKind::Skirmish) return s.skirmish_map;
    if (const data::CampaignDef* c = data_.db.find_campaign(s.campaign); c && !c->text_id.empty() && ui_.text.find(c->text_id))
        return utf8_of(txt(c->text_id, {}));
    return s.campaign;
}

// A message box with up to three lines of `detail` (the reason a save / load failed), one OK button.
void App::save_message(std::u32string caption, const std::string& detail) {
    std::printf("saves: %s: %s\n", utf8_of(caption).c_str(), detail.c_str());
    ui::MessageBoxSpec m;
    m.tag = "save_message";
    m.caption = std::move(caption);
    std::string rest = detail;
    while (!rest.empty() && m.lines.size() < 3) {
        size_t cut = rest.size();
        if (cut > 64 && m.lines.size() < 2) {
            cut = rest.rfind(' ', 64);
            if (cut == std::string::npos || cut == 0) cut = 64;
        }
        m.lines.push_back(ui::utf8_to_utf32(rest.substr(0, cut)));
        rest.erase(0, std::min(rest.size(), cut + (cut < rest.size() && rest[cut] == ' ' ? 1u : 0u)));
    }
    m.buttons = {txt("TEXT_BUTTON_OK", U"OK")};
    screens_.show_message(m);
}

void App::save_game_as(const std::string& typed) {
    if (!session_ || demo_session()) return save_message(U"The game cannot be saved now", "no game is running");
    if (saves_dir_.empty()) return save_message(txt("TEXT_SAVE_FAILED", U"Failed to save game."), "no saved games folder");
    std::string path, err;
    if (!save_game(*session_, saves_dir_, typed, session_title(), unix_now(), &path, &err)) {
        const bool refused = !session_->check_save().empty();  // cached: no second save
        save_message(refused ? std::u32string(U"The game cannot be saved now") : txt("TEXT_SAVE_FAILED", U"Failed to save game."), err);
    } else {
        ++saves_written_;
        std::printf("saves: saved %s at tick %" PRIu64 "\n", path.c_str(), session_->host().frame());
        screen_line(txt("TEXT_SAVED_OK", U"Game Saved"), ui::Rgba8{255, 255, 255, 255});
    }
    ui_.front.refresh_saves();
}

// Load from the main menu or the in-game menu: the save is rebuilt and loaded into a new session first; only when that
// worked does it replace the running session (a failed load leaves the current game / menu as it was).
bool App::load_saved_game(const std::string& name) { return load_save_file(save_path(saves_dir_, name)); }

bool App::load_save_file(const std::string& path, const commands::CommandRecording* replay) {
    std::string err;
    std::unique_ptr<GameSession> s = path.empty() ? nullptr : load_game(data_, *js_, path, &err, replay);
    if (!s) {
        save_message(U"The saved game could not be loaded", path.empty() ? std::string("no save name") : err);
        return false;
    }
    attract_.stop();  // as start_session: a real game ends the attract demo
    end_session();
    session_ = std::move(s);
    recording_ = commands::CommandRecording{};
    if (replay) recording_ = *replay;  // bm2: a replayed stream is the run's stream (crc_check replays it again)
    else session_->record_to(&recording_);  // record_after_load: the stream continues the save
    loaded_save_ = path;
    ++saves_loaded_;
    paused_ = false;
    session_started({});
    hotkeys_.set_paused(session_->pause_requested());  // a game saved paused comes back paused
    return true;
}

void App::delete_saved_game(const std::string& name) {
    std::string err;
    if (!delete_save(saves_dir_, name, &err)) save_message(U"The saved game could not be deleted", err);
    else std::printf("saves: deleted %s\n", name.c_str());
    ui_.front.refresh_saves();
}

// GALACTIC_DIALOGS §3.1 / BATTLE_TRANSITIONS §4.1 step 4: an auto-resolved conflict (log how = 1) the local player took
// part in, or at a planet whose fleets it can see [port: PlanetInfo::FleetPresence], posts the red result line
// TEXT_AR_{SPACE,LAND}_{WIN,LOSS}_{MAJOR,REGULAR,MINOR} (##0 = the viewpoint's faction, ##1 = planet, ##2 = its recorded
// losses; viewpoint = the local player, else the winner) and TEXT_AR_HERO_LOSS per dead named hero [port: known for the
// local player's own auto-resolve only]. The battle-end screen follows the local player's Auto-Resolve choice (never an
// overrun or an AI battle, which are not asked).
void App::auto_resolve_result(const galactic::GalacticSim& g, const galactic::ConflictLogEntry& e) {
    const int32_t me = local_player();
    const bool involved = e.attacker == me || e.defender == me;
    const bool chosen = auto_pending_ && involved && e.planet == auto_planet_;
    if (chosen) auto_pending_ = false;
    if (!involved && !g.can_see_planet_info(me, e.planet, galactic::GalacticSim::PlanetInfo::FleetPresence)) return;
    const int32_t view = involved ? me : e.winner;
    const bool view_attacker = view == e.attacker;
    const uint32_t n_own = view_attacker ? e.attacker_losses : e.defender_losses;
    const uint32_t n_other = view_attacker ? e.defender_losses : e.attacker_losses;
    const bool land = e.kind == galactic::ConflictKind::Land;
    const bool win = e.winner == view;
    const std::u32string planet = planet_label(data_, ui_.text, g, e.planet);
    screen_line(ui::auto_resolve_text(&ui_.text, !land, win, ar_result_magnitude(win, n_own, n_other), faction_label(view), planet, n_own),
                ui::kAutoResolveTextColor);
    if (chosen)
        for (const AutoUnit& u : auto_units_)
            if (!g.store().alive(u.h) && is_named_hero(g, static_cast<uint32_t>(u.type)))
                screen_line(ui::auto_resolve_hero_loss_text(&ui_.text, type_label(data_, ui_.text, u.type), planet), ui::kAutoResolveTextColor);
    if (chosen) show_auto_resolve_end(e);
}

// GALACTIC_DIALOGS §1: the battle-pending prompt for the head conflict (paused: the conflict manager holds the galactic
// pause in Vote; no timeout in single player). Feed: the planet, space / land, the title, Begin-only when the story lock
// forbids auto-resolve (§1.4; [port] no tutorial lock / player flag source yet), each side's commander icon and stack
// depth (Ships_Per_Stack); the faction pending SFX and music.
// §1.5: Begin -> Fight = conflict state 3; Auto-Resolve (two-button prompt only) -> AutoResolve = state 6. No Retreat. SP
// auto-resolve has no progress dialog (§3.1): its result comes from the conflict log (auto_resolve_result).
void App::answer_battle_prompt(bool auto_resolve) {
    if (!choice_open_) return;
    choice_open_ = false;
    if (prompt_.linked) {  // story t13: the P13 banner has Begin only; the battle (and its load screen) starts at the next hook
        const bool sent = session_ && session_->begin_linked();
        std::printf("battle choice: begin (story link%s)\n", sent ? "" : ", not sent");
        return;
    }
    const bool ar = auto_resolve && !prompt_.story_pending && !prompt_.no_autoresolve;
    const galactic::BattleChoice c = ar ? galactic::BattleChoice::AutoResolve : galactic::BattleChoice::Fight;
    const galactic::GalacticSim* g = galaxy();
    const galactic::ConflictInfo* head = session_ ? session_->conflict_head() : nullptr;
    const bool sent = session_ && session_->choose_battle(c);
    if (sent && !ar && head) open_battle_load(head->planet, head->kind == galactic::ConflictKind::Space);  // QA t11
    if (sent && ar && g && head) {
        ++auto_resolves_;
        auto_pending_ = true;
        auto_planet_ = head->planet;
        auto_units_.clear();  // the forces as they stand when the auto-resolve starts (losses = the ones gone after)
        for (const int32_t side : {head->attacker, head->defender})
            for (const objects::ObjectHandle m : conflict_units(*g, *head, side)) auto_units_.push_back({side, g->store().type(m), m});
    }
    std::printf("battle choice: %s\n", ar ? "auto-resolve" : "begin");
}

void App::open_battle_prompt(const galactic::GalacticSim& g, const galactic::ConflictInfo& c) {
    prompt_ = {};
    prompt_.planet = c.planet;
    prompt_.land = c.kind == galactic::ConflictKind::Land;
    prompt_.story_pending = session_->autoresolve_locked(c.planet);
    const int32_t me = local_player();
    const int32_t enemy = c.attacker == me ? c.defender : c.attacker;
    auto icon = [this](uint32_t t) { return type_icon(t); };
    const uint32_t per = static_cast<uint32_t>(std::max(1, data_.db.constants().get_int("Ships_Per_Stack", 5)));
    const std::vector<objects::ObjectHandle> mine = conflict_units(g, c, me), theirs = conflict_units(g, c, enemy);
    prompt_.left = battle_force_summary(g, mine, icon, per);
    prompt_.right = battle_force_summary(g, theirs, icon, per);
    prompt_.title = planet_label(data_, ui_.text, g, c.planet) +
                    (prompt_.land ? txt("TEXT_TACTICAL_LAND_BATTLE_IS_PENDING", U": PLANETARY BATTLE IMMINENT!")
                                  : txt("TEXT_TACTICAL_SPACE_BATTLE_IS_PENDING", U": SPACE BATTLE IMMINENT!"));
    prompt_.planet_name = planet_label(data_, ui_.text, g, c.planet);
    audio_.battle_pending(prompt_.land);
    if (prompt_.land && c.attacker == me) {  // ui4 t08 [port]: the local player's land invasion begins (the battle prompt)
        audio_.faction_sfx("SFXEvent_Land_Invasion_Commencing");
        ++announcements_;
    }
    choice_open_ = true;  // the banner (GalacticHudFrame::battle) until Begin / Auto-Resolve
    ++choices_;
    if (prompt_.story_pending) ++prompts_story_;
    // ui1 t06 (BATTLE_TRANSITIONS §1.6 step 4): with the advanced options' auto-resolve option, "always" (0) resolves a
    // battle that is not story-locked without asking; "never" (1) [port: the original's GUI hook 0x140b15638 is not
    // traced] offers Begin only; "ask" (2) and "overwhelming odds" (3) [port] ask.
    if (session_ && session_->spec().game_options && !prompt_.story_pending && !prompt_.linked) {
        if (session_->spec().auto_resolve_option == ui::kAutoResolveAlways) answer_battle_prompt(true);
        else if (session_->spec().auto_resolve_option == ui::kAutoResolveNever) prompt_.no_autoresolve = true;
    }
    std::printf("battle prompt: %s (%s, %s), yours %u units / %u stacks [%s], enemy %u units / %u stacks [%s]\n",
                utf8_of(prompt_.title).c_str(), prompt_.land ? "land" : "space", prompt_.story_pending ? "begin only" : "begin / auto-resolve",
                prompt_.left.units, prompt_.left.stacks, prompt_.left.icon.c_str(), prompt_.right.units, prompt_.right.stacks,
                prompt_.right.icon.c_str());
}

// story t13 (BT §2.9.3 step 2): a LINK_TACTICAL record with P13 = 1 waits behind the one-button pending banner (the
// story_pending_battle_shell, chosen because the linked-battle flag 0x140b30545 is set) with the galaxy paused; Begin
// (answer_battle_prompt) sends ResumeGame 51 and the record starts at the next hook. No auto-resolve, no force graphs (the
// single shell has none; a non-sandbox link takes no galactic units anyway).
void App::open_linked_banner(const galactic::GalacticSim& g, const sim::CampaignBattles::LinkedRecord& r) {
    prompt_ = {};
    prompt_.planet = r.planet;
    prompt_.land = r.kind == galactic::ConflictKind::Land;
    prompt_.story_pending = true;
    prompt_.linked = true;
    prompt_.planet_name = planet_label(data_, ui_.text, g, r.planet);
    prompt_.title = prompt_.planet_name + (prompt_.land ? txt("TEXT_TACTICAL_LAND_BATTLE_IS_PENDING", U": PLANETARY BATTLE IMMINENT!")
                                                        : txt("TEXT_TACTICAL_SPACE_BATTLE_IS_PENDING", U": SPACE BATTLE IMMINENT!"));
    audio_.battle_pending(prompt_.land);
    choice_open_ = true;
    ++choices_;
    ++prompts_story_;
    ++linked_banners_;
    std::printf("battle prompt: %s (%s, story link, begin only, map %s)\n", utf8_of(prompt_.title).c_str(), prompt_.land ? "land" : "space",
                r.map.c_str());
}

// STORY.md §1.6 step 3 / GALACTIC_DIALOGS §2.1: a Story_Dialog event fired (its record is already in the sim's holocron
// list). Popup false (every shipped event): b_story_arc_g flashes, the faction's Default_Transmission_Message
// (TEXT_ENCYCLOPEDIA_HOLOCRON_UPDATED for every shipped faction [port: the port reads no faction field for it]) is a white
// screen-text line, and SFXEvent_Mission_Added plays only when the event has Story_Dialog_SFX (no shipped event does).
// Popup true: the summary opens at this record instead.
void App::add_holocron_chapter(const StoryDialogMeta& meta, size_t lines) {
    HolocronChapter ch;
    ch.dialog = meta.dialog;
    ch.tag = meta.tag;
    ch.chapter = meta.chapter;
    ch.id = meta.holocron_id;
    holocron_chapters_.push_back(std::move(ch));
    ++holocron_updates_;
    summary_entry_ = 0;  // the summary opens at the first (newest) row
    std::printf("holocron: chapter %d of %s added (%zu lines), tag %s%s%s\n", meta.chapter, meta.dialog.c_str(), lines, meta.tag.c_str(),
                meta.popup ? ", popup" : "", meta.sfx ? ", sfx" : "");
    if (meta.popup) {
        summary_completed_ = false;
        summary_entry_ = meta.holocron_id;
        open_story_summary();
        return;
    }
    holocron_flash_ = true;
    screen_line(txt("TEXT_ENCYCLOPEDIA_HOLOCRON_UPDATED", U"The mission holocron has been updated."), ui::Rgba8{255, 255, 255, 255});
    if (meta.sfx) audio_.faction_sfx("SFXEvent_Mission_Added");
}

void App::update_radar() {
    const render::FogRenderer& fog = bview_.fog();
    if (!bview_.active()) return;
    // Space asteroid fields (RadarMap.xml Space_Asteroid_Field_Color): the battle's SpaceFieldIndex, read once per battle.
    if (!radar_fields_built_) {
        if (const sim::TacticalBattle* b = battle()) {
            radar_fields_built_ = true;
            if (bview_.space()) radar_fields_ = radar_asteroid_fields(b->space_fields().fields());
            radar_fields_dirty_ = !radar_fields_.empty();
        }
    }
    const bool fog_new = !fog.processed().empty() && fog.stats().uploads != radar_fog_uploads_;
    if (!fog_new && !radar_fields_dirty_) return;
    radar_fog_uploads_ = fog.stats().uploads;
    radar_fields_dirty_ = false;
    uint32_t w = 0, h = 0;
    std::span<const uint8_t> vis;
    if (!fog.processed().empty()) {
        w = fog.grid().width();
        h = fog.grid().height();
        if (static_cast<size_t>(w) * h != fog.processed().size()) return;
        vis = fog.processed();
    }
    ui::RadarFieldPaint paint;  // the radar's world box (build_tactical_hud's map_min / max)
    paint.fields = radar_fields_;
    if (const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr)
        if (tm->bounds().max.x - tm->bounds().min.x < 1.0e5f) {
            paint.min_x = tm->bounds().min.x;
            paint.min_y = tm->bounds().min.y;
            paint.max_x = tm->bounds().max.x;
            paint.max_y = tm->bounds().max.y;
        }
    paint.fill = radar_settings_.space_asteroid;
    paint.border = radar_settings_.space_asteroid_border;
    radar_.build(128, {}, 0, 0, vis, w, h, bview_.space() ? radar_settings_.space_fow : radar_settings_.land_fow, paint);
    if (!radar_.empty()) gpu_.hud.set_dynamic_page(ui::kHudPageRadar, radar_.width(), radar_.height(), radar_.rgba(), radar_.generation());
}

// ---- information screens fed from the sim (game_info.hpp) ----

// Galactic-map screens each frame: data-chart samples, story dialogs fired since the last frame (GALACTIC_DIALOGS §2:
// the mission holocron; IDD_STORY_DIALOG only for random-plot offers), the battle prompt (§1), auto-resolve results (§3)
// and the campaign end (campaign score).
void App::galactic_screens() {
    const galactic::GalacticSim* g = galaxy();
    if (!g) return;
    holo_.sample(*g);
    std::vector<StoryMovie> movies;
    std::vector<StoryDialogMeta> meta;
    sync_story_scripts(*session_, data_, story_scripts_, story_loads_);  // GALACTIC_DIALOGS §2.8: scripts load with their plots
    std::vector<ui::StoryDialogInfo> dialogs = new_story_dialogs(*session_, story_scripts_, ui_.text, story_cursor_, &movies, &meta);
    bool offer = false;  // a random-plot offer opened (its talking head plays in the dialog's portrait)
    for (size_t k = 0; k < dialogs.size(); ++k) {
        ui::StoryDialogInfo& i = dialogs[k];
        const StoryDialogMeta md = k < meta.size() ? meta[k] : StoryDialogMeta{};
        if (!md.random_plot) {
            add_holocron_chapter(md, i.lines.size());
            continue;
        }
        // ui ask 11: the talking head (MOVIE looped / MOVIE_ONCE) plays in the dialog's portrait: a GUI image page named
        // after the movie, filled with the movie's frames (StoryDialogInfo::image = that name).
        if (!movies.empty() && i.image.empty()) i.image = movies.front().name;
        if (info_.show_story(i)) {
            ++story_dialogs_;
            offer = true;
        }
    }
    for (const StoryMovie& m : movies) {
        if (!offer) break;  // [port] holocron chapters play no movie of their own; MULTIMEDIA rewards drive Movie_galactic
        std::string err;
        if (advisor_movie_.open(data_, audio_, m.name, m.loop, &err)) {
            story_movie_ = true;
            story_page_ = ui_.pages.page(m.name);
            // Marked loaded at once (the movie's size) so the page loader never looks for a texture of that name.
            if (story_page_ >= 0) ui_.pages.set_loaded(static_cast<uint16_t>(story_page_), advisor_movie_.width(), advisor_movie_.height());
        } else {
            std::fprintf(stderr, "story movie %s: %s\n", m.name.c_str(), err.c_str());
        }
    }
    // The open story dialog's script clock (wall time, presentation only): lines appear at their WAITs, SFX / DIALOG cues
    // play once.
    std::vector<ui::StoryDialogCue> cues;
    info_.update_story(now_s_, &cues);
    for (const ui::StoryDialogCue& c : cues) {
        if (c.kind == ui::StoryDialogCue::Kind::Speech) audio_.speech(c.event);
        else audio_.ui(c.event);
        ++story_cues_;
    }
    if (story_movie_ && !ui_.gui.find(ui::dlg::kStory)) {  // the talking head ends with its dialog
        advisor_movie_.stop(audio_);
        gpu_.hud.clear_image_page(kAdvisorPage);
        story_movie_ = false;
        story_page_ = -1;
    }
    if (story_summary_open_ && !ui_.gui.find(ui::dlg::kStorySummary)) story_summary_open_ = false;  // closed by Esc
    if (session_->battle_choice_pending() && !choice_open_)
        if (const galactic::ConflictInfo* c = session_->conflict_head()) open_battle_prompt(*g, *c);
    // story t13: the linked record's banner (it can also go down without Begin, e.g. a loaded save shows it again).
    if (choice_open_ && prompt_.linked && !(session_->battles() && session_->battles()->linked_banner_up())) choice_open_ = false;
    if (!choice_open_)
        if (const sim::CampaignBattles::LinkedRecord* r = session_->linked_banner_record()) open_linked_banner(*g, *r);
    if (const sim::CampaignBattles* b = session_->battles()) {
        const std::vector<galactic::ConflictLogEntry>& log = b->conflicts().log();
        while (ar_log_seen_ < log.size()) {
            const galactic::ConflictLogEntry& e = log[ar_log_seen_++];
            if (e.how == 1) auto_resolve_result(*g, e);
        }
    }
    if (session_->spec().kind == SessionKind::Campaign && !campaign_over_)
        if (const int32_t o = galactic::victory_outcome(*g, local_player()); o != 0) show_campaign_end(o > 0);  // Int-verify
}

void App::show_campaign_end(bool victory) {
    const galactic::GalacticSim* g = galaxy();
    if (!g) return;
    campaign_over_ = true;
    // ui1 t05 (GAME_SCORING.md §7): the dialog's values come from the GameScoring script (galactic stats, tactical = false).
    ScoringStats stats;
    {
        sim::SimHost& host = session_->host();
        double v = 0.0;
        std::string t;
        bool num = false;
        auto stat = [&](const char* id, double& out) {
            if (host.scoring_game_stat(local_player(), id, false, v, t, num) && num) {
                out = v;
                return true;
            }
            return false;
        };
        stats.ok = stat("IDC_MILITARY_EFFICIENCY_STATIC", stats.military) && stat("IDC_CONQUEST_EFFICIENCY_STATIC", stats.conquest) &&
                   stat("IDC_KILL_EFFICIENCY_STATIC", stats.kill);
        if (stats.ok && host.scoring_game_stat(local_player(), "IDC_TITLE_STATIC", false, v, t, num) && !num) stats.title_key = t;
    }
    if (!screens_.show_campaign_score(campaign_score(*session_, *g, ui_.text, victory, tally_, stats))) notes_.push_back("campaign score dialog missing");
    std::printf("campaign end: %s\n", victory ? "victory" : "defeat");
}

// The holocron (IDD_DROID_LOG / DATA_CHART / TECH_TREE_* / SUMMARY_SCREEN_*): every page's data from the galaxy, then
// the page; switching pages inside keeps the data (InfoScreens).
void App::open_holocron(ui::InfoPage page) {
    const galactic::GalacticSim* g = galaxy();
    if (!g) return;
    info_.set_droid_log(holo_.droid_log(*session_, data_, ui_.text));
    info_.set_data_chart(holo_.chart(*g, data_, ui_.text));
    info_.set_tech_tree(holo_.tech_tree(*g, data_, ui_.hud_types, local_player(), false));
    info_.set_summary(holo_.summary(*g, data_, ui_.text, ui_.hud_types, local_player()));
    if (!info_.show_info(page)) notes_.push_back("holocron page missing");
}

// GALACTIC_DIALOGS §2.2 / §2.8: IDD_STORY_SUMMARY_DIALOG from the sim's holocron list (holocron_summary): the tab's
// records newest first, labelled by their chapter's first TEXT line, the selected record's chapter as the recap. Showing a
// record marks it read (0x1089e0; its row dims from now on). Opening stops the holocron flash and holds the ticks until it
// closes (the summary's galactic pause in single player).
void App::open_story_summary() {
    if (!session_) return;
    sync_story_scripts(*session_, data_, story_scripts_, story_loads_);
    HolocronSummaryView v = holocron_summary(*session_, story_scripts_, ui_.text, summary_completed_, summary_entry_);
    summary_rows_ = std::move(v.row_ids);
    if (v.info.selected >= 0 && static_cast<size_t>(v.info.selected) < summary_rows_.size())
        summary_entry_ = summary_rows_[static_cast<size_t>(v.info.selected)];
#ifdef ALAMO_SIM_HAVE_STORY
    if (v.selected_index != SIZE_MAX)
        if (sim::StorySession* story = session_->host().story()) story->mark_holocron_read(v.selected_index);
#endif
    if (!info_.show_story_summary(v.info)) {
        notes_.push_back("story summary dialog missing");
        return;
    }
    holocron_flash_ = false;
    story_summary_open_ = true;
}

bool App::story_controls_locked() const {
#ifdef ALAMO_SIM_HAVE_STORY
    if (sim::StorySession* story = session_ ? session_->host().story() : nullptr)
        if (const lua::PresentationLog* log = story->presentation()) return log->state().controls_locked;
#endif
    return false;
}

// Encyclopedia pop-up for what the cursor is over (presentation only): galactic = a planet (its type), battle = the
// nearest object whose screen position is within 24 px of the cursor [port]. Delays from the panel's TooltipConfig.
void App::build_encyclopedia(ui::HudDrawList& dl, const ui::HudCanvas& canvas) {
    const int32_t mx = input_.mouse_x(), my = input_.mouse_y();
    uint32_t object = 0;
    objects::TypeIndex type = objects::kNone;
    const uint32_t w = targets_.width, h = targets_.height;
    uint32_t stack_fleet = 0;  // ui1 t03: a fleet under the cursor (map fleet in transit or a planet's fleet button)
    bool stack_land = false;
    if (screen_ == Screen::Galactic && gview_.attached() && !ui_.gui.hit(mx, my)) {  // HUD_GAPS §6: not under a dialog
        const ui::View v = ui::make_view(gview_.camera_params(w, h), w, h);
        const std::vector<ui::PickSphere> items = gview_.pick_items();
        stack_fleet = ui_.galactic_hud.stack_under(static_cast<float>(mx), static_cast<float>(my), stack_land);
        const auto fhit = ui::pick_spheres(items, ui::pixel_ray(v, mx, my));
        if (stack_fleet == 0 && routed_.hover_component < 0 && fhit && items[*fhit].kind == ui::PickKind::Fleet) stack_fleet = items[*fhit].id;
        if (const auto hit = fhit; hit && items[*hit].kind == ui::PickKind::Planet) {
            object = items[*hit].id;
            for (const render::GalacticPlanetSnap& p : gview_.frame().planets)
                if (static_cast<uint32_t>(p.id) == object) type = p.type;
        }
    } else if (screen_ == Screen::Battle && bview_.active()) {
        // HUD_GAPS §2.2: the hover pick of build_tactical_hud (the world pick, refixf t08; reticle owner kept).
        for (const ui::HudUnit& u : hud_units_)
            if (u.id == hover_ship_ && hover_ship_ != 0 && u.type < data_.types.size()) {
                object = u.id;
                type = u.type;
            }
    }
    if (routed_.hover_component >= 0) object = 0;  // over the HUD: no world pop-up
    // ui1 t03 (HUD_GAPS §2.2: fleets use a fleet summary and fire the story trigger fleet_tooltip, land forces land_tooltip;
    // [port] the summary layout is not recovered: the pop-up of the fleet's representative unit, the unit with the highest
    // Tactical_Health as on the stack button; the trigger fires once per hovered fleet / land stack).
    if (stack_fleet != 0 && !stack_land)
        if (const galactic::GalacticSim* g = galaxy()) {
            const objects::ObjectStore& st = g->store();
            float best = -1.0f;
            for (const auto& fl : g->fleets()) {
                if (!fl.alive || !st.alive(fl.object) || static_cast<uint32_t>(st.id(fl.object)) != stack_fleet) continue;
                for (const objects::ObjectHandle m : fl.contents) {
                    if (!st.alive(m)) continue;
                    const objects::TypeIndex t = st.type(m);
                    const float hp = t < data_.types.size() ? data_.types.get_float(t, "Tactical_Health") : 0.0f;
                    if (hp > best) {
                        best = hp;
                        type = t;
                        object = stack_fleet;
                    }
                }
            }
        }
    {
        const uint32_t key = stack_fleet == 0 ? 0u : (stack_fleet ^ (stack_land ? 0x80000000u : 0u));
        if (key != 0 && key != tooltip_trigger_fleet_) story_ui_.generic(stack_land ? "land_tooltip" : "fleet_tooltip");
        tooltip_trigger_fleet_ = key;
    }
    if (type == objects::kNone) object = 0;
    int32_t component = -1;
    // FIXPASS_GALACTIC §2.2 hover: a good hero slot shows its hero's pop-up.
    if (hero_panel_.open() && hero_panel_.hover_component() >= 0 && hero_panel_.hover_hero() != 0)
        for (const auto& [id, t] : hero_types_)
            if (id == hero_panel_.hover_hero()) {
                component = hero_panel_.hover_component();
                object = id;
                type = t;
            }
    ency_.update(component, object, now_s_);  // every frame: the countdown / fade run on the panel
    // Remember the hovered object's type so the pop-up of shown_object() still draws while it fades out; keep only the
    // shown and the hovered subjects.
    if (object != 0) {
        bool known = false;
        for (auto& [id, t] : ency_types_)
            if (id == object) {
                t = type;
                known = true;
            }
        if (!known) ency_types_.emplace_back(object, type);
    }
    const uint32_t shown = ency_.shown_object();
    std::erase_if(ency_types_, [&](const auto& p) { return p.first != shown && p.first != object; });
    if (shown == 0 || ency_.stage() == ui::EncyclopediaPanel::Stage::Hidden) return;
    objects::TypeIndex shown_type = objects::kNone;
    for (const auto& [id, t] : ency_types_)
        if (id == shown) shown_type = t;
    if (shown_type == objects::kNone) return;
    ui::EncyclopediaEntry e = encyclopedia_entry(data_, ui_.text, ui_.hud_types, shown_type);
    // GRC 2 / REMAINING_GAPS §6.3: a planet's income row only with the local player's CreditIncome visibility.
    if (screen_ == Screen::Galactic)
        if (const galactic::GalacticSim* g = galaxy())
            if (const PlanetIndex pi = planet_index(*g, shown); pi != kInvalidPlanet &&
                !g->can_see_planet_info(local_player(), pi, galactic::GalacticSim::PlanetInfo::CreditIncome))
                std::erase_if(e.rows, [](const ui::EncyclopediaRow& r) { return r.left == U"Income"; });
    // HUD_GAPS §2.2: anchored at the hint bone of the HUD on screen (not the cursor).
    const ui::HudLayout& layout = screen_ == Screen::Battle ? ui_.tactical_hud.layout() : ui_.galactic_hud.layout();
    if (ency_.build(dl, canvas, e, layout, ui_.faction_alt()).x > 0.0f) ++ency_shown_;
}

// ui1 t01 (docs/re/UI.md §3.1 gives the pointer set, not the rules; [port] docs/PHASE4.md "UI1 cursor"): the object under the
// cursor (the order translator's world pick) sets the hover kind - own (SELECT), enemy of the local player (ATTACK),
// other owned objects friendly, unowned neutral - and, with a selection: can_attack = a selected unit can hit the
// target at all (combat can_hit is not NoTarget), in_range = one of those can hit it from where it stands or can move
// (a mobile unit walks into range), garrisonable = a selected unit may enter it (Can_Garrison), passable = the ground
// point under the cursor is passable for a selected land unit (any; space: always).
void App::fill_cursor_hover(ui::CursorContext& c) {
    using H = ui::CursorContext::Hover;
    const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
    const sim::TacticalBattle* tb = battle();
    if (!tm || !tb) return;
    const uint32_t w = targets_.width, h = targets_.height;
    const ui::View v = ui::make_view(bview_.camera_params(w, h), w, h);
    const tactical::Ray ray = ui::pixel_ray(v, input_.mouse_x(), input_.mouse_y());
    tactical::QueryContext qc;
    const auto hit = ui::pick_world(tm->trees().all(), v, ray, tactical::kSkipDead, qc, world_pick_tests());
    const auto& cb = tb->combat();
    const std::vector<uint32_t>& sel = orders_.selection();
    if (!hit) {
        c.hover = H::Ground;
        if (!c.selection || bview_.space()) return;
        // The ground point: the ray against the terrain (fixed-point refine from z = 0, as the order translator does).
        const terrain::MapData* m = bview_.scene() ? &bview_.scene()->map() : nullptr;
        if (!m) return;
        float z = 0.0f;
        tactical::Vec3 p{};
        for (int i = 0; i < 4; ++i) {
            if (!ui::ray_plane_z(ray, z, p)) return;
            z = m->terrain.sample_height(p.x, p.y);
        }
        const auto& mv = tb->movement();
        bool any_land = false;
        for (const uint32_t u : sel) {
            const auto id = static_cast<objects::ObjectId>(u);
            if (!mv.has_unit(id)) continue;
            any_land = true;
            if (mv.grid().passable_world({p.x, p.y}, mv.mask_of(id))) return;  // passable stays true
        }
        c.passable = !any_land;
        return;
    }
    const auto target = static_cast<tactical::combat::ObjectId>(hit->id);
    const int32_t owner = cb.owner(target);
    const int32_t me = local_player();
    if (owner == me) c.hover = H::Own;
    else if (owner >= 0 && cb.is_enemy(me, owner)) c.hover = H::Enemy;
    else if (owner >= 0) c.hover = H::Friendly;
    else c.hover = H::Neutral;
    if (!c.selection) return;
    if (c.hover == H::Enemy) {
        bool attack = false, range = false;
        for (const uint32_t u : sel) {
            const auto uid = static_cast<tactical::combat::ObjectId>(u);
            if (cb.owner(uid) != me) continue;
            const tactical::combat::CanHit r = cb.can_hit(uid, target);
            if (r == tactical::combat::CanHit::NoTarget) continue;
            attack = true;
            if (r == tactical::combat::CanHit::Yes || r == tactical::combat::CanHit::LineBlocked ||
                tb->movement().has_unit(static_cast<objects::ObjectId>(u)))
                range = true;
        }
        c.can_attack = attack;
        c.in_range = range;
    } else {
        for (const uint32_t u : sel)
            if (u != hit->id && tb->can_garrison(static_cast<objects::ObjectId>(u), static_cast<objects::ObjectId>(hit->id))) {
                c.garrisonable = true;
                break;
            }
    }
}

void App::update_cursor(ui::HudDrawList& dl) {
    // ui ask 1: one cursor - the OS arrow is hidden over the window while the game draws its own.
    if (cinematic_hides_cursor()) {  // refixf t05: cinematic mode on hides the cursor (0x262490(..., 0))
        set_os_cursor_hidden(true);
        return;
    }
    set_os_cursor_hidden(cursor_ok_ && !opt_.hidden);
    if (!cursor_ok_) return;
    ui::CursorContext c;
    c.movie = screen_ == Screen::Movie;
    c.busy = screen_ == Screen::SessionLoad;
    c.over_ui = routed_.hover_component >= 0 || ui_.gui.hit(input_.mouse_x(), input_.mouse_y());
    c.drag_selecting = input_.button(ui::Button::Left).dragging && screen_ == Screen::Battle;
    c.selection = !orders_.selection().empty();
    if (screen_ == Screen::Galactic) {
        c.scrolling = gview_.camera().dragging();  // ui1 t01: POINTER_MAP_SCROLLING_MODE while the map is dragged
        const ui::GalacticDrag& d = ui_.galactic_hud.drag();
        c.fleet_drag = d.active;
        c.drop_on_fleet = d.over_fleet;
        c.drop_on_planet = d.over_planet >= 0;
        c.drop_valid = d.valid;
        if (d.over_planet >= 0 && static_cast<size_t>(d.over_planet) < hud_planet_ids_.size() && session_ &&
            !session_->move_target_revealed(hud_planet_ids_[static_cast<size_t>(d.over_planet)]))
            c.drop_valid = false;  // GRC 1.1: an unrevealed planet is no drop target
    } else if (screen_ == Screen::Battle) {
        c.rotating = bview_.camera().drag() == ui::TacticalCameraController::Drag::Rotate;
        c.scrolling = bview_.camera().drag() == ui::TacticalCameraController::Drag::Pan;
        // ui1 t01: the hovered object decides the plain cursor (ATTACK / out of range / CANT_ATTACK, SELECT, GARRISON_IN,
        // MOVE / CANT_MOVE). [port] rules (docs/PHASE4.md "UI1 cursor"): see fill_cursor_hover.
        if (!c.over_ui && bview_.active()) fill_cursor_hover(c);
        // A running pick: the targeted-ability cursor (valid on a hit object / any ground point) or the landing-point one.
        if (!c.over_ui && bview_.active()) {
            const sim::TacticalMode* tm = session_ ? session_->host().modes().top_tactical() : nullptr;
            const sim::TacticalBattle* tb = battle();
            if (targeting_.active() && tm && tb) {
                bool hit_friendly = false;
                if (targeting_.target_kind() == ui::TargetKind::Object) {
                    const ui::View v = ui::make_view(bview_.camera_params(targets_.width, targets_.height), targets_.width, targets_.height);
                    tactical::QueryContext qc;
                    const auto hit = ui::pick_world(tm->trees().all(), v, ui::pixel_ray(v, input_.mouse_x(), input_.mouse_y()),
                                                    tactical::kSkipDead, qc, world_pick_tests());
                    c.target_valid = hit.has_value();  // no object under the cursor: the pick keeps waiting
                    hit_friendly = hit && !tb->combat().is_enemy(local_player(), tb->combat().owner(static_cast<tactical::combat::ObjectId>(hit->id)));
                }
                c.targeting = ui::ability_pick_cursor(targeting_.target_kind(), targeting_.ability_type(), bview_.space(), hit_friendly);
            } else if (reinforce_unit_ != 0) {
                c.targeting = ui::CursorContext::Targeting::Reinforcement;
            } else if (bombing_targeting_) {
                c.targeting = ui::CursorContext::Targeting::BombingRun;  // POINTER_BOMBING_RUN_TARGET (HUD_GAPS §9.5)
            } else if (sw_weapon_ != objects::kNoObjectId) {  // ui1 t04: POINTER_ATTACK_WITH_SPECIAL_WEAPON_n / _INVALID
                c.targeting = ui::CursorContext::Targeting::SpecialWeapon;
                c.special_weapon = sw_icon_;
                c.target_valid = c.hover == ui::CursorContext::Hover::Enemy;
            }
        }
        // HUD_GAPS §1.4: the repair cursor over an own / allied damaged hard point not being repaired, when the selection
        // can repair hard points.
        if (!c.over_ui && c.selection && c.targeting == ui::CursorContext::Targeting::None && !hud_hardpoints_.empty() && bview_.active()) {
            const ui::View v = ui::make_view(bview_.camera_params(targets_.width, targets_.height), targets_.width, targets_.height);
            const int32_t r = ui::reticle_at(ui_.tactical_hud.canvas(), v, reticles_, hud_hardpoints_,
                                             static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y()), now_s_);
            if (r >= 0) {
                const ui::HudHardPoint& hp = hud_hardpoints_[static_cast<size_t>(r)];
                if (!hp.enemy && hp.health < 1.0f && !hp.repairing && selection_can_repair())
                    c.targeting = ui::CursorContext::Targeting::Repair;
            }
        }
    }
    cursor_.update(ui::choose_pointer(c), now_s_);
    cursor_.build(dl, ui_.gui.canvas(), {static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y())}, now_s_);
}

// Panels of the current screen into `lists` (one draw list per open panel; their shells follow open()).
void App::build_panels(std::vector<ui::HudDrawList>& lists) {
    const ui::Vec2f cur{static_cast<float>(input_.mouse_x()), static_cast<float>(input_.mouse_y())};
    auto frame_of = [&](ui::PanelFrame& f, bool open) {
        f.open = open;
        f.faction_alt = ui_.faction_alt();
        f.shells_loaded = panel_shells_ok_;
        f.time = now_s_;
        f.cursor_px = cur;
    };
    // Hero roster (battle): the local player's named heroes on the map.
    std::vector<ui::HeroEntry> heroes;
    hero_types_.clear();
    if (screen_ == Screen::Battle && bview_.active())
        for (const render::SnapshotObject& o : bview_.frame().objects)
            if (o.owner == local_player() && o.type < data_.types.size() && data_.types.get_bool(o.type, "Is_Named_Hero") && heroes.size() < 11) {
                ui::HeroEntry e;
                e.object = static_cast<uint32_t>(o.id);
                e.icon = type_icon(o.type);
                e.health = o.health_fraction;
                e.selected = std::binary_search(orders_.selection().begin(), orders_.selection().end(), static_cast<uint32_t>(o.id));
                // FIXPASS_GALACTIC §2.3: container = the hero's team container (AbilitySystem team membership); none ->
                // kNoObject (a double click then selects every object of the hero's type).
                e.container = commands::kNoObject;
                if (const sim::TacticalBattle* b = battle())
                    if (const tactical::combat::ObjectId c = b->abilities().container_of(static_cast<tactical::combat::ObjectId>(o.id));
                        c != tactical::combat::kNoObject)
                        e.container = static_cast<uint32_t>(c);
                e.tooltip = type_label(data_, ui_.text, o.type);
                hero_types_.emplace_back(e.object, o.type);
                heroes.push_back(e);
            }
    // HUD_GAPS §3.2: the galactic roster (good set) every frame - the local player's named heroes on the map, then the
    // ones being built / awaiting respawn, max 11.
    if (screen_ == Screen::Galactic)
        if (const galactic::GalacticSim* gs = galaxy()) {
            const std::vector<uint32_t> sel = orders_.selection();
            galactic_heroes_ = galactic_hero_entries(
                *gs, local_player(),
                [this](uint32_t t) { return type_icon(t); }, sel, 11,
                [this](uint32_t t) { return t < data_.types.size() ? data_.types.get_int(static_cast<objects::TypeIndex>(t), "Ranking_In_Category") : 0; });
            heroes = galactic_heroes_;
            for (ui::HeroEntry& e : heroes)
                if (const objects::TypeIndex t = data_.types.resolve_name(e.type_name); t != objects::kNone) {
                    e.tooltip = type_label(data_, ui_.text, t);
                    if (e.object != 0 && e.object != commands::kNoObject) hero_types_.emplace_back(e.object, t);
                }
        }
    ui::HeroPanelFrame hf;
    frame_of(hf, !heroes.empty());
    // FIXPASS_GALACTIC §2.1: the good bar is the local player's own (self / allied heroes), whatever the faction.
    hf.good = true;
    hf.tactical = screen_ == Screen::Battle;
    hf.heroes = heroes;
    hero_panel_.build(hf);
    const bool galactic = screen_ == Screen::Galactic;
    // P4 host: the open special dialog (GameSession::special_dialog, else the smoke's preview) feeds its panel from the
    // GalacticSim getters (game_info.cpp special_panel).
    const uint32_t kind = panel_kind();
    const SpecialDialog* dlg = session_ ? session_->special_dialog() : nullptr;
    const galactic::GalacticSim* g = galaxy();
    PlanetIndex pi = kInvalidPlanet;
    uint32_t planet_id = commands::kNoObject, hero_id = 0;
    if (g && kind) {
        planet_id = dlg ? dlg->planet_id : preview_planet_;
        hero_id = dlg ? dlg->hero_id : 0;
        pi = dlg ? dlg->planet : planet_index(*g, preview_planet_);
    }
    SpecialPanelData pd;
    if (g && pi != kInvalidPlanet) pd = special_panel(*g, data_, ui_.text, ui_.hud_types, local_player(), kind, pi, hero_id);
    const bool show = galactic && pi != kInvalidPlanet;
    ui::BlackMarketFrame mf;
    frame_of(mf, show && kind == galactic::kSpecialBlackMarket);
    mf.planet = planet_id;
    mf.title = pd.title;
    mf.subtitle = pd.subtitle;
    mf.items = pd.market;
    market_.build(mf);
    // OI host: the slice dialog (kind 2): SlicePanel on the same market shell (only one of the two is open).
    ui::SliceFrame lf;
    frame_of(lf, show && kind == galactic::kSpecialSlice);
    lf.planet = planet_id;
    lf.slicer = hero_id;
    lf.title = pd.title;
    lf.description = pd.subtitle;
    lf.rows = pd.slice;
    slice_.build(lf);
    ui::SabotageFrame sf;
    frame_of(sf, show && kind == galactic::kSpecialSabotage);
    sf.planet = planet_id;
    sf.title = pd.title;
    sf.subtitle = pd.subtitle;
    sf.steal_text = pd.hero;
    sf.targets = pd.sabotage;
    sabotage_.build(sf);
    ui::CorruptionFrame cf;
    frame_of(cf, show && kind == galactic::kSpecialCorruption);
    cf.planet = planet_id;
    cf.planet_title = pd.subtitle;
    cf.planet_subtitle = pd.title;
    cf.defiler_text = pd.hero;
    cf.options = pd.corruption;  // P4 host: requirement n/m rows, enabled by corruption_requirements (all four checks)
    corruption_.build(cf);
    ui::HackFrame kf;
    frame_of(kf, show && kind == galactic::kSpecialHack);
    kf.planet = planet_id;
    kf.target = pd.hack_target;
    kf.title = pd.title;
    kf.cost = pd.hack_cost;
    kf.lines = pd.hack_lines;
    kf.enabled = pd.hack_enabled;
    hack_.build(kf);
    // Panels may share a shell (market / slice): hide every panel shell, then show the open panels' active ones.
    for (ui::HudPanel* p : panels())
        for (const int32_t s : p->shells())
            if (s >= 0) panel_shells_.set_visible(static_cast<uint32_t>(s), false);
    for (ui::HudPanel* p : panels()) {
        if (const int32_t s = p->active_shell(); s >= 0) panel_shells_.set_visible(static_cast<uint32_t>(s), true);
        if (p->open()) lists.push_back(p->draw_list());
    }
}

}  // namespace alamo_game_app

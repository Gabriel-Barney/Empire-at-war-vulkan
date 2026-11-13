#include "alamo/game/game_session.hpp"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <span>
#include <string_view>

#include "alamo/game/battle_orders.hpp"
#include "alamo/game/story_ui.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/economy.hpp"
#include "alamo/sim/save_session.hpp"
#include "alamo/sim/tactical_battle.hpp"
#include "alamo/terrain/ted.hpp"
#include "alamo/vfs/vfs.hpp"  // map candidates per layer
#ifdef ALAMO_GAME_HAVE_UI
#include "alamo/ui/orders.hpp"  // pause_event / read_pause_event
#endif
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
#include "alamo/ai/galactic_ai.hpp"
#endif
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/lua/presentation.hpp"
#include "alamo/lua/story_model.hpp"
#include "alamo/sim/story_session.hpp"
#if defined(ALAMO_LUA_PLAYERS)
#include "alamo/lua/players.hpp"  // q6: the story player world's perception source
#endif
#endif

namespace alamo::game {

const char* difficulty_name(int32_t difficulty) {
    return difficulty <= 0 ? "Easy_Default" : difficulty >= 2 ? "Hard_Default" : "Normal_Default";
}

GameSession::GameSession(const GameData& data, jobs::JobSystem& js, SessionSpec spec) : data_(&data), js_(&js), spec_(std::move(spec)) {}

GameSession::~GameSession() {
    // The galactic AI lets go of the story's player world first; then CampaignBattles (it clears its host hooks), the
    // host (its controllers and CRC hashers reference the AI and the animation), then the AI and the animation.
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    if (galactic_ai_) galactic_ai_->attach_story_world(nullptr);
#endif
    battles_.reset();
    host_.reset();
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    galactic_ai_.reset();
#endif
}

bool GameSession::galactic_ai_running() const {
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    return galactic_ai_ != nullptr;
#else
    return false;
#endif
}

uint64_t GameSession::galactic_ai_orders() const {
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    return galactic_ai_ ? galactic_ai_->orders_issued() : 0;
#else
    return 0;
#endif
}

const galactic::GalacticSim* GameSession::galaxy() const {
    if (!host_) return nullptr;
    const sim::GameModeManager& m = host_->modes();
    if (m.empty()) return nullptr;
    const auto* g = dynamic_cast<const sim::GalacticMode*>(&m.at(m.depth() - 1));
    return g ? g->sim() : nullptr;
}

bool GameSession::take_reveal_sfx() {
    if (!host_) return false;
    sim::GameModeManager& m = host_->modes();
    for (size_t i = 0; i < m.depth(); ++i)
        if (auto* g = dynamic_cast<sim::GalacticMode*>(&m.at(i)); g && g->sim()) return g->sim()->consume_reveal_sfx();
    return false;
}

bool GameSession::move_target_revealed(uint32_t planet_id) const {
    const galactic::GalacticSim* g = galaxy();
    if (!g || g->local_player() == galactic::kNoPlayer) return true;
    const objects::ObjectStore& st = g->store();
    for (PlanetIndex p = 0; p < g->planets().size(); ++p) {
        const galactic::PlanetState& ps = g->planets()[p];
        if (ps.is_planet && st.exists(ps.object) && static_cast<uint32_t>(st.id(ps.object)) == planet_id) return g->planet_revealed_raw(p);
    }
    return true;
}

bool GameSession::skirmish_over() const {
    return spec_.kind == SessionKind::Skirmish && battles_ && battles_->battles_fought() > 0 && !battles_->battle();
}

bool GameSession::start(std::vector<std::string>* diag, const commands::CommandRecording* replay, commands::CommandRecording* record) {
    auto fail = [&](const std::string& m) {
        if (diag) diag->push_back(m);
        return false;
    };
    if (started_) return fail("session already started");
    const GameData& d = *data_;
    // 1. The galaxy.
    if (spec_.kind == SessionKind::Campaign) {
        galactic::CampaignOptions co;
        co.human_faction = spec_.faction;
        co.ai_difficulty = difficulty_name(spec_.difficulty);
        setup_ = galactic::build_campaign_setup(d.db, d.rules, spec_.campaign, co, diag);
        if (setup_.map.planets.size() == 0) return fail("campaign '" + spec_.campaign + "' has no planets");
    } else {
        std::string err;
        if (spec_.skirmish_map_data) skirmish_map_ = *spec_.skirmish_map_data;
        else if (!terrain::load_map(*d.files, spec_.skirmish_map, skirmish_map_, &err))
            return fail("skirmish map " + spec_.skirmish_map + ": " + err);
        spec_.skirmish.land = !skirmish_map_.is_space();  // the map decides: space battle or land invasion
        if (spec_.attract && spec_.skirmish.land) return fail("attract demo: " + spec_.skirmish_map + " is not a space map");
        SkirmishSetup sk = spec_.skirmish;
        // modc t19 [port]: the galaxy's planet is the map's own planet type when the data has it and it forbids no unit,
        // else the first such PLANET type (skirmish_galaxy_planet); never a planet by name.
        if (sk.planet_type.empty())
            if (const uint32_t pt = skirmish_galaxy_planet(d.rules, spec_.skirmish_map, sk.land); pt != galactic::kNoIndex)
                sk.planet_type = d.rules.types[pt].name;
        sk.no_forces = spec_.attract;  // q6 t02: the demo's forces are cleared below; a mod faction without a fleet must not fail it
        if (!make_skirmish_galaxy(d, sk, setup_, &err)) return fail(err);
        // GALAXY_MAP_PRESENTATION §6: the attract demo is a plain tactical space battle on the map. The map's object owners
        // are player indices in faction order (Factions.xml then Expansion_Factions.xml: Rebel 0, Empire 1, ..., Neutral 3,
        // Underworld 6), so every faction becomes the player of its index; AI on the Rebel and Underworld players (0x22f10's
        // players 0 and 6), no human (player 0 stays the viewpoint), no starting forces: every unit comes from the map. The
        // skirmish galaxy is scaffolding only (no hop, no conflict): the battle starts directly (start_map_battle).
        if (spec_.attract) {
            // modc t03: by index as 0x22f10 does (players 0 and 6 = faction indices 0 and 6; FoC: Rebel, Underworld), not
            // by name. [port] data with fewer than 7 factions (synthetic tests): the skirmish's two slot factions.
            if (d.rules.factions.size() > 6) {
                attract_sides_ = {0, 6};
            } else {
                const SkirmishSetup slots = resolve_skirmish_factions(d.rules, spec_.skirmish);
                attract_sides_ = {d.rules.find_faction(slots.human_faction), d.rules.find_faction(slots.ai_faction)};
            }
            setup_.players.clear();
            for (uint32_t f = 0; f < d.rules.factions.size(); ++f) {
                galactic::PlayerSetup ps;
                ps.faction = f;
                ps.ai = f == attract_sides_[0] || f == attract_sides_[1];
                setup_.players.push_back(ps);
            }
            setup_.forces.clear();
            setup_.leaning.clear();
        }
    }
    // A2: every non-human playable faction with an AIPlayerType becomes an AI player (difficulty from the menu).
    bool use_ai = false;
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    ai::GalacticAiConfig gcfg;
    if (spec_.galactic_ai && d.ai && !spec_.attract) {  // OI host: the attract demo's galaxy only stages the battle
        gcfg.difficulties = d.ai->difficulties;
        gcfg.contrast = &d.crules.contrast;
        gcfg.story_campaign = spec_.kind == SessionKind::Campaign && spec_.story;
        gcfg.ai_uses_fog_galactic = d.db.constants().get_bool("AIUsesFogOfWarGalactic", gcfg.ai_uses_fog_galactic);  // shipped False
        const uint32_t diff = d.rules.find_difficulty(difficulty_name(spec_.difficulty));
        // q6 t08: the type from the data (resolve_ai_type): campaigns read the campaign's AI_Player_Control pairs, a
        // skirmish gives only its AI slot (ps.ai) the faction's Basic_AI (SKIRMISH_SETUP §2.1). modc t17: base FoC too.
        const std::vector<std::string>* control = nullptr;
        if (spec_.kind == SessionKind::Campaign)
            if (const data::CampaignDef* c = d.db.find_campaign(spec_.campaign)) control = &c->ai_player_control;
        const std::vector<std::string> no_pairs;
        if (spec_.kind == SessionKind::Campaign && !control) control = &no_pairs;
        for (size_t p = 0; p < setup_.players.size(); ++p) {
            galactic::PlayerSetup& ps = setup_.players[p];
            if (ps.human || ps.faction >= d.rules.factions.size() || d.rules.factions[ps.faction].is_neutral) continue;
            if (spec_.kind == SessionKind::Skirmish && !ps.ai) continue;
            const std::string type = resolve_ai_type(d.rules.factions[ps.faction].name, d.basic_ai, control, &d.ai->defs);
            if (type.empty()) continue;
            ps.ai = true;
            ps.difficulty = diff;
            gcfg.players.push_back({static_cast<int32_t>(p), type, std::clamp(spec_.difficulty, 0, 2)});
            ai_types_.resize(setup_.players.size());
            ai_types_[p] = type;
        }
        use_ai = !gcfg.players.empty();
    }
#endif
    local_player_ = 0;
    for (size_t p = 0; p < setup_.players.size(); ++p)
        if (setup_.players[p].human) {
            local_player_ = static_cast<int32_t>(p);
            break;
        }
    // 2. The host (a replay rebuilds the pipeline and takes the recorded seed before the session starts).
    sim::SimConfig cfg;
    cfg.seed = spec_.seed;
    cfg.players = sim::session_players(setup_.players.size());
    // q6 (docs/re/OBJECT_SCRIPTS.md "Port status"): game-object Lua scripts, in every session that has the game's files
    // (vanilla, mods and the attract demo alike). q6 t23 (2026-11-13): the retail probe (Q6_T15_RETAIL_RUN.md row 2b)
    // showed vanilla objects run their Lua_Script, so the 2026-11-04 interim rule (on only with a mod layer) is gone.
    cfg.object_scripts = d.files != nullptr;
    host_ = std::make_unique<sim::SimHost>(d.types, *js_, cfg);
    if (cfg.object_scripts) host_->set_object_script_files(d.files.get());
    if (replay) host_->replay_from(replay);
    // q6 t06: the GameScoring script, re-created by every new session before the players and plots (GAME_SCORING.md).
    if (const auto scoring = d.db.constants().get("Game_Scoring_Script_Name"))
        host_->set_scoring_script(d.files.get(), std::string(*scoring));
    galactic::GalacticSim* gs = host_->start_galactic(d.rules, setup_, diag);
    if (!gs) return fail("galactic session did not start");
    gs->set_emit_events(true);
#ifdef ALAMO_SIM_HAVE_ANIM
    if (spec_.animation) {
        anim_ = std::make_unique<sim::SimAnimation>(d.types, *d.files);
        anim_->attach(*host_, false);
    }
#endif
    // 3. Battles: human conflicts fought as tactical battles, AI-vs-AI auto-resolved.
    sim::CampaignBattleOptions bo;
    bo.conflict.human_choice = galactic::BattleChoice::Fight;  // without ask_battle_choice: every human conflict is fought
    bo.conflict.fight_ai_battles = spec_.attract;  // OI host: the attract demo's AI-vs-AI battle is fought
    bo.battle.max_frames = spec_.battle_max_frames;
    bo.battle.end_hold_frames = spec_.battle_end_hold_frames;
    bo.battle.order_interval = spec_.battle_order_interval;
    bo.battle.skirmish = spec_.kind == SessionKind::Skirmish;  // SpecialWeaponsAllowed ANDs the hard-point flag
    // ccpgt t02: the Capture Control Points game type (land maps only; SKIRMISH_SETUP §4.2).
    bo.battle.control_point_domination = spec_.kind == SessionKind::Skirmish && spec_.skirmish.land && spec_.skirmish.control_points;
    bo.battle.skip_human_units = true;  // the human's units take orders only from commands (stand-ins skip them)
    sim::load_tactical_systems(*d.files, d.rules, &d.db.constants(), bo);  // Int-final: weather, retreat, strikes data
    if (d.vfs) {
        // cu8f t03: the random map picker's candidate table (CU8F_T03_T05_RE §1.1), built on the first pick [port].
        bo.map_candidate_source = sim::map_candidate_source(*d.files, [v = d.vfs](std::string_view g, bool mod) {
            return v->list(g, mod ? vfs::Vfs::kLayerMod : vfs::Vfs::kLayerBase | vfs::Vfs::kLayerMeg);
        });
    }
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    if (spec_.tactical_ai && d.ai && d.ai->tactical) {
        auto ts = std::make_shared<sim::TacticalAiSetup>(*d.ai->tactical);
        ts->difficulty = std::clamp(spec_.difficulty, 0, 2);
        bo.tactical_ai = std::move(ts);
    }
#endif
#ifdef ALAMO_SIM_HAVE_ANIM
    bo.animation = anim_.get();
#endif
#if defined(ALAMO_GAME_HAVE_GALACTIC_AI) && defined(ALAMO_SIM_HAVE_AI_TACTICAL)
    // A2 -> A1: the AI players' galactic budget categories for the land-battle credit transfer.
    budget_rows_.assign(gs->players().size(), {});
    bo.tactical_budget = [this](int32_t p) -> std::span<ai::tactical::TacticalBudgetCategory> {
        if (!galactic_ai_ || p < 0 || static_cast<size_t>(p) >= budget_rows_.size()) return {};
        auto& rows = budget_rows_[static_cast<size_t>(p)];
        rows.clear();
        for (const auto& r : galactic_ai_->tactical_budget_rows(p)) rows.push_back({r.allowance, r.available, r.pool});
        return rows;
    };
#endif
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    // battle host: battle objects leaving the mode -> the galactic AI's survival learning (0x3a8380), as alamo_headless
    // ai_run does. No galactic AI (CampaignAggressor / AI data missing): nothing is recorded.
    bo.object_removed = [this](int32_t owner, uint32_t type, uint32_t original, bool survived) {
        if (galactic_ai_) galactic_ai_->record_object_removal(owner, type, original, survived);
    };
    // a1j t07 (A1_READINGS §38): Evaluate_In_Galactic_Context / Release_Credits_For_Tactical in a land battle read the galactic
    // AI's world (0x69e1b0).
    bo.galactic_resolver = [this]() -> const ai::TokenResolver* { return galactic_ai_ ? &galactic_ai_->resolver() : nullptr; };
#endif
    sim::CampaignBattles::MapLoader loader;
    if (spec_.kind == SessionKind::Skirmish) {
        const terrain::MapData* mp = &skirmish_map_;
        loader = [mp](const std::string&, terrain::MapData& m) {
            m = *mp;
            return true;
        };
    } else {
        loader = sim::CampaignBattles::file_map_loader(*d.files);
    }
    battles_ = std::make_unique<sim::CampaignBattles>(*host_, d.crules, *d.catalog, std::move(loader), bo);
    if (!battles_->attach(diag)) return fail("battles: attach failed");
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    // battle host: the auto-resolve survival roll -> the galactic AI's survival learning (STRATEGIC_GAPS 1.1).
    battles_->conflicts().on_survival_roll = [this](int32_t owner, uint32_t type, bool survived) {
        if (galactic_ai_) galactic_ai_->record_auto_resolve_survival(owner, type, survived);
    };
#endif
    // The pending-battle choice: the conflict waits in Vote until the battle_choice_event drains (replays: the recorded
    // event does the same at the same tick).
    if (spec_.ask_battle_choice) battles_->conflicts().decide = [](const galactic::ConflictInfo&) { return galactic::BattleChoice::Pending; };
    // 4. Human orders into the running battle (P host hook in SimHost::apply).
    host_->set_order_hook([this](sim::SimHost& h, uint8_t player, const commands::EventData& ev, const std::vector<objects::ObjectId>& sel) {
        return route_battle_order(*battles_, h, player, ev, sel, &order_stats_);
    });
    // Pause / resume (docs/DECISIONS.md "Camera and controls"): a port-only command; a drained pause stops the host from
    // ticking (replays tick through it: a pause changes when ticks run, never what they compute).
    host_->set_port_event_hook([this](sim::SimHost&, uint8_t player, const commands::EventData& ev) { return port_event(player, ev); });
    // P4 host: the galactic special events (dialogs, SFX) are tapped before the story drains the stream at T9.
    host_->set_before_story_hook([this](sim::SimHost& h) { tap_events(h.story() != nullptr); });
    // OI host: the Lua presentation's cinematic rate lock -> pacing + parity, right after T9 (see cinematic_rate_lock()).
    host_->set_after_story_hook([this](sim::SimHost& h) { after_story(h); });
    // 5. Story (campaigns with plots): StorySession at T9; its AI requests reach the galactic AI (attached in 6).
    // A2i t01 (A2I_T01_FRAME_ORDER.md §6): started BEFORE the galactic AI, as alamo_headless does. start_story ends with the
    // load-time STORY_TECH_LEVEL pass, whose hero spawns (HotM: the Accuser, Home_One, ...) must be starting units for the
    // AI: GalacticAiWorld::start takes the players' units present then as known (no On_Unit_Added at its first service).
#ifdef ALAMO_SIM_HAVE_STORY
    if (spec_.kind == SessionKind::Campaign && spec_.story && sim::SimHost::story_supported())
        if (const data::CampaignDef* camp = d.db.find_campaign(spec_.campaign)) {
            sim::StoryStart st;
            st.files = d.files.get();
            st.plot_sets = sim::campaign_plot_sets(*camp, *gs);
            if (!st.plot_sets.empty() && host_->start_story(st, diag)) {
                // OI host: events raised inside T9 (Lua Activate_Ability, story rewards) reach the presentation tap
                // from the story's later drain rounds (round 0 = what the before-story tap already saw).
                host_->story()->set_drain_tap([this](int round, const std::vector<galactic::GalacticEvent>& evs) {
                    const galactic::GalacticSim* g = round > 0 ? galaxy() : nullptr;
                    if (g)
                        for (const galactic::GalacticEvent& e : evs) tap_event(*g, e);
                });
            }
        }
#endif
    // 6. AI players: A2's galactic AI (T7, player index order); without it the S10 stand-in (campaign only).
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    if (use_ai) {
        sim::SimHost* h = host_.get();
        gcfg.lua_gc_pf = host_->parallel_for();  // OPT2 t09: scheduled Lua collections on the host's job system
        gcfg.background = jobs::make_background_start(host_->jobs());  // OPT2 t02: background path recomputes
        galactic_ai_ =std::make_unique<ai::GalacticAiWorld>(
            *gs, host_->galactic_mode()->primary(), &d.types, d.files.get(), host_->random(), d.ai->defs, d.ai->equations, d.ai->type_info,
            [h](uint8_t p, const commands::EventData& ev) { return h->execute_direct(p, commands::CommandSource::AI, ev); }, gcfg);
        std::vector<std::string> wd;
        if (!galactic_ai_->load_plans(d.ai->plan_files, &wd) || !galactic_ai_->start(&wd)) {
            if (diag)
                for (const std::string& s : wd) diag->push_back("galactic AI: " + s);
            galactic_ai_.reset();
            use_ai = false;
            ai_types_.clear();
        } else {
            galactic_ai_->register_crc(host_->crc_registry());
            galactic_ai_->set_global_values(host_->global_values());  // Int-final: session-wide GlobalValue table
            galactic_ai_->set_conflicts(&battles_->conflicts());  // q6 t03: TimeSince*Defender/Attacker stamps
#ifdef ALAMO_SIM_HAVE_STORY
            if (host_->story()) {
                galactic_ai_->attach_story_world(host_->story()->player_world());  // story Find_Path
#if defined(ALAMO_LUA_PLAYERS)
                // q6: the plot scripts' EvaluatePerception answers from the galactic AI's equations and token resolver,
                // as in alamo_headless (Thrawn's Revenge's VictoryHandler / StoryUtil read perceptions every service).
                if (lua::LuaPlayerWorld* sw = host_->story()->player_world())
                    sw->set_perception({&d.ai->equations, &galactic_ai_->resolver()},
                                       {&d.ai->equations, &galactic_ai_->resolver()});
#endif
            }
#endif
            ai::GalacticAiWorld* w = galactic_ai_.get();
            sim::CampaignBattles* b = battles_.get();
            // q6: every player but the human and the neutral player gets the T7 controller, because the story reward
            // SWITCH_CONTROL (below) can give a player an AI later; a player without an AI does nothing there (as before:
            // no update_player call for it).
            for (size_t pi = 0; pi < setup_.players.size(); ++pi) {
                if (setup_.players[pi].human || static_cast<int32_t>(pi) == gs->neutral_player()) continue;
                host_->set_controller(static_cast<uint8_t>(pi), [w, b](sim::SimHost& hh, uint8_t p, uint64_t frame) {
                    if (!w->is_ai_player(p)) return;
                    const sim::GameMode* top = hh.modes().top();
                    const bool galactic_top = top && !top->tactical();
                    const bool busy = b->conflicts().state() == galactic::ConflictState::AutoRunning;
                    w->update_player(p, static_cast<int64_t>(frame), galactic_top, busy);
                });
            }
            // q6: story reward SWITCH_CONTROL (AI_GALACTIC.md §1.2: Player::Set_AI_Type, then the difficulty) -> the
            // galactic AI, while the reward applies in T9: the new AI takes its first service at the next T7. Difficulty =
            // the session's [M: the original reads the session difficulty global]; the human keeps its control [port].
            host_->set_switch_control_hook([this](const sim::StorySwitchControl& sw) {
                if (!galactic_ai_ || sw.player < 0 || static_cast<size_t>(sw.player) >= setup_.players.size()) return;
                if (setup_.players[static_cast<size_t>(sw.player)].human) return;
                galactic_ai_->set_ai_type(sw.player, sw.ai_type, std::clamp(spec_.difficulty, 0, 2));
            });
            // N1 / t09: the galactic AI's chunk, as alamo_headless save_run registers it.
            sim::SaveParticipant ai_part;
            ai_part.name = "ai";
            ai_part.write = [w](ByteWriter& bw) { sim::write_system(*w, bw); };
            ai_part.read = [w](ByteReader& br) { return sim::read_system(*w, br); };
            host_->add_save_participant(std::move(ai_part));
        }
    }
#endif
    if (!use_ai && spec_.kind == SessionKind::Campaign && spec_.aggressor_period > 0)
        for (size_t p = 0; p < gs->players().size() && p < host_->players(); ++p) {
            const galactic::PlayerState& ps = gs->players()[p];
            if (ps.human || static_cast<int32_t>(p) == gs->neutral_player()) continue;
            host_->set_controller(static_cast<uint8_t>(p), sim::CampaignAggressor{spec_.aggressor_period});
        }
    if (record) host_->record_to(record);
    replay_ = replay != nullptr;
    // Land skirmish: the human's ground units board its fleet at the start (GalacticSim's direct operation, part of the
    // session setup like the starting forces, so a replay does the same).
    if (spec_.kind == SessionKind::Skirmish && spec_.skirmish.land) {
        std::vector<objects::ObjectHandle> ground;
        for (const objects::ObjectHandle u : gs->planets()[kSkirmishHumanPlanet].surface)
            if (gs->store().alive(u) && gs->store().owner(u) == local_player_) ground.push_back(u);
        if (ground.empty() || gs->launch_units(local_player_, kSkirmishHumanPlanet, ground) != commands::RejectReason::None)
            return fail("land skirmish: the human's ground units could not board");
    }
    // GRC 1.1 [port]: the skirmish galaxy is host scaffolding; its opening hop is ordered before the first galactic
    // pre-step, and the human hop gate refuses an unrevealed target - the battle planet is revealed with the setup (as
    // the home planet is), for recorded and replayed sessions alike.
    if (spec_.kind == SessionKind::Skirmish) gs->set_planet_revealed(kSkirmishBattlePlanet, true);
    // 7. Skirmish opening: the human fleet hops to the AI's planet (an ordinary order in the stream). OI host: the attract
    // demo has no human to issue it - the hop is a direct GalacticSim operation of the setup (a replay does the same).
    if (spec_.kind == SessionKind::Skirmish && spec_.attract) {
        galactic::BattleRequest r;
        r.info.kind = galactic::ConflictKind::Space;
        r.info.planet = kSkirmishBattlePlanet;
        r.info.attacker = static_cast<int32_t>(attract_sides_[0]);
        r.info.defender = static_cast<int32_t>(attract_sides_[1]);
        r.info.map = r.map = spec_.skirmish_map;
        if (r.info.attacker < 0 || r.info.defender < 0 || r.info.attacker == r.info.defender || !battles_ ||
            !battles_->start_map_battle(std::move(r)))
            return fail("attract demo: map battle on " + spec_.skirmish_map + " refused");
    } else if (spec_.kind == SessionKind::Skirmish && !replay && !loading_) {  // a load: the saved queue holds it
        const std::vector<uint32_t> fleets = gs->fleets_at(kSkirmishHumanPlanet, local_player_);
        const galactic::PlanetState& target = gs->planets()[kSkirmishBattlePlanet];
        if (fleets.empty() || !gs->store().exists(target.object)) return fail("skirmish: human fleet / battle planet missing");
        const auto fleet_id = static_cast<uint32_t>(gs->store().id(gs->fleets()[fleets.front()].object));
        const auto planet_id = static_cast<uint32_t>(gs->store().id(target.object));
        if (!submit(commands::ev::move_object_to_object(fleet_id, planet_id)).ok()) return fail("skirmish: opening order refused");
    }
    // N1 / t09: this session's own chunk (last participant) and the session descriptor.
    sim::SaveParticipant game_part;
    game_part.name = "game";
    game_part.write = [this](ByteWriter& w) { write_game_state(w); };
    game_part.read = [this](ByteReader& r) { return read_game_state(r); };
    host_->add_save_participant(std::move(game_part));
    if (!spec_.attract) describe_session();
    started_ = true;
    return true;
}

bool GameSession::set_paused(bool pause) {
#ifdef ALAMO_GAME_HAVE_UI
    if (!host_ || pause == (paused_ || pause_pending_)) return false;
    set_fast_forward(false);  // OI host: every pause / unpause clears fast mode (MAIN_LOOP.md §1.5)
    if (!pause) {
        paused_ = pause_pending_ = false;  // ticks resume now; the resume entry drains on the next tick (stream record)
        return submit(ui::pause_event(false)).ok();
    }
    pause_pending_ = submit(ui::pause_event(true)).ok();
    return pause_pending_;
#else
    (void)pause;
    return false;
#endif
}

bool GameSession::tick() {
    if (paused_ && !replay_) return false;
    if (!host_ || !host_->tick_once()) return false;
    if (paused_) pause_pending_ = false;
    ++ticks_;
    after_tick();
    return true;
}

// Land skirmish: once the human fleet stopped at the AI's planet, the Invade order (an ordinary recorded command; a
// replay carries it in its stream instead).
void GameSession::land_skirmish_invade() {
    if (invaded_ || replay_ || spec_.kind != SessionKind::Skirmish || !spec_.skirmish.land) return;
    const galactic::GalacticSim* gs = galaxy();
    if (!gs || host_->modes().top_tactical()) return;
    bool there = false;
    for (const uint32_t fi : gs->fleets_at(kSkirmishBattlePlanet, local_player_)) there = there || !gs->fleets()[fi].moving;
    if (!there) return;
    commands::EventData ev(commands::EventType::Invade);
    ev.set("planet", static_cast<uint32_t>(gs->store().id(gs->planets()[kSkirmishBattlePlanet].object)));
    invaded_ = submit(std::move(ev)).ok();
}

bool GameSession::loop_iteration(uint32_t now_ms) {
    if (!host_ || (paused_ && !replay_)) return false;
    const bool t = host_->loop_iteration(now_ms);
    if (paused_) pause_pending_ = false;
    if (t) {
        ++ticks_;
        after_tick();
    }
    return t;
}

commands::EventData battle_choice_event(galactic::BattleChoice choice) {
    const auto b = static_cast<uint8_t>(choice);
    return commands::ev::port_extension(kBattleChoiceSubtype, std::span<const uint8_t>(&b, 1));
}

const galactic::ConflictInfo* GameSession::conflict_head() const {
    if (!battles_) return nullptr;
    const galactic::ConflictManager& cm = battles_->conflicts();
    if (!cm.space_queue().empty()) return &cm.space_queue().front();  // [port] space conflicts are serviced first
    if (!cm.land_queue().empty()) return &cm.land_queue().front();
    return nullptr;
}

bool GameSession::local_in_head_conflict() const {
    const galactic::ConflictInfo* c = conflict_head();
    return c && (c->attacker == local_player_ || c->defender == local_player_);
}

bool GameSession::battle_choice_pending() const {
    if (!battles_ || battles_->conflicts().state() != galactic::ConflictState::Vote || choice_sent_) return false;
    return local_in_head_conflict();
}

bool GameSession::choose_battle(galactic::BattleChoice choice) {
    if (!battle_choice_pending()) return false;
    choice_sent_ = submit(battle_choice_event(choice)).ok();
    return choice_sent_;
}

bool GameSession::linked_banner_pending() const { return battles_ && battles_->linked_banner_up() && !banner_sent_; }

const sim::CampaignBattles::LinkedRecord* GameSession::linked_banner_record() const {
    return linked_banner_pending() ? battles_->linked_record() : nullptr;
}

bool GameSession::begin_linked() {
    if (!linked_banner_pending()) return false;
    commands::EventData ev(commands::EventType::ResumeGame);
    ev.set("flag", 0u);  // Begin (CampaignBattles::resume_game -> linked_begin while the banner is up)
    banner_sent_ = submit(std::move(ev)).ok();
    return banner_sent_;
}

bool GameSession::autoresolve_locked(PlanetIndex planet) const {
#ifdef ALAMO_SIM_HAVE_STORY
    if (const sim::StorySession* s = host_ ? host_->story() : nullptr) return s->autoresolve_locked(planet);
#else
    (void)planet;
#endif
    return false;
}

bool GameSession::auto_resolving() const {
    if (!battles_ || battles_->conflicts().state() != galactic::ConflictState::AutoRunning) return false;
    return local_in_head_conflict();
}

// --- P4 host: Withdraw (retail's multiplayer-only auto-resolve dialog; no UI caller in the port) and the retreat
// dialog's Autoresolve, both through the command stream (recorded, replayed, drained at the command phase like
// battle_choice_event) ---
bool GameSession::withdraw() { return host_ && submit(commands::EventData(commands::EventType::Withdrawl)).ok(); }

commands::EventData tactical_auto_resolve_event() {
    const uint8_t b = 1;
    return commands::ev::port_extension(kTacticalAutoResolveSubtype, std::span<const uint8_t>(&b, 1));
}
bool GameSession::auto_resolve_battle() { return host_ && submit(tactical_auto_resolve_event()).ok(); }
// L1/S4 wave 1: story UI notification through the command stream (port_event applies it).
bool GameSession::notify_story_ui(const sim::StoryUiNotification& n) {
    return host_ && host_->story() && submit(story_ui_event(n)).ok();
}

bool GameSession::story_esc() {
    sim::StorySession* story = host_ ? host_->story() : nullptr;
    return story && story->esc_goes_to_story() && submit(story_esc_event()).ok();
}

// --- P4 host: galactic special abilities (SpecialAbility 13) and their dialogs ---
bool GameSession::submit_special(commands::EventData ev) {
    if (!host_ || !submit(std::move(ev)).ok()) return false;
    dialog_open_ = false;
    return true;
}

bool GameSession::activate_special(uint32_t unit_id, uint32_t planet_id) {
    return host_ && submit(galactic::special_activate_event(planet_id, unit_id)).ok();
}

uint32_t slicer_ability(const galactic::GalacticSim& g, objects::ObjectHandle hero) {
    const objects::ObjectStore& st = g.store();
    if (!st.alive(hero)) return 0xFFFFFFFFu;
    const galactic::SpecialRules& sp = g.rules().special;
    for (const uint32_t a : sp.type(static_cast<uint32_t>(st.type(hero))).abilities)
        if (a < sp.abilities.size() && sp.abilities[a].cls == galactic::SpecialAbilityClass::Slicer) return a;
    return 0xFFFFFFFFu;
}

std::vector<galactic::SliceEntry> slice_rows(const galactic::GalacticSim& g, int32_t player, PlanetIndex planet, uint32_t hero_id) {
    const uint32_t ability = slicer_ability(g, g.store().find(static_cast<objects::ObjectId>(hero_id)));
    if (ability == 0xFFFFFFFFu) return {};
    return g.slice_entries(player, planet, ability);
}

bool GameSession::slice_tech(uint32_t type) {
    const galactic::GalacticSim* g = galaxy();
    if (!g || !dialog_open_ || dialog_.kind != galactic::kSpecialSlice) return false;
    const std::vector<galactic::SliceEntry> rows = slice_rows(*g, local_player_, dialog_.planet, dialog_.hero_id);
    for (const galactic::SliceEntry& e : rows)
        if (e.type == type)
            return submit_special(galactic::slice_event(dialog_.planet_id, dialog_.hero_id, type, e.price, e.chance, dialog_.respawn_seconds,
                                                        rows.size() == 1));
    return false;
}

bool black_market_row_enabled(const galactic::BlackMarketOffer& o, float credits) {
    return !o.purchased && static_cast<int32_t>(o.price + 0.5f) <= static_cast<int32_t>(credits + 0.5f);
}

bool GameSession::buy_black_market(uint32_t item) {
    const galactic::GalacticSim* g = galaxy();
    if (!g || !dialog_open_ || dialog_.kind != galactic::kSpecialBlackMarket || item >= g->rules().special.items.size()) return false;
    const objects::ObjectHandle hero = g->store().find(static_cast<objects::ObjectId>(dialog_.hero_id));
    for (const galactic::BlackMarketOffer& o : g->bm_offers(local_player_, dialog_.planet, hero)) {
        if (o.item != item) continue;
        if (!black_market_row_enabled(o, g->credits(local_player_))) return false;
        // 2.1 / 2.4: STORY_BUY_BLACK_MARKET at click time with the item Name, ahead of the purchase command and
        // whatever its outcome (story-UI command path, every plot set).
        sim::StoryUiNotification n;
        n.kind = sim::StoryUiEvent::BuyBlackMarket;
        n.player = local_player_;
        n.name = g->rules().special.items[item].name;
        notify_story_ui(n);  // no story session: nothing to tell
        return submit_special(galactic::black_market_purchase_event(dialog_.planet_id, dialog_.hero_id, g->rules().special.items[item].name_crc,
                                                                    o.price, dialog_.respawn_seconds));
    }
    return false;
}

bool GameSession::sabotage_structure(uint32_t structure_id) {
    const galactic::GalacticSim* g = galaxy();
    if (!g || !dialog_open_ || dialog_.kind != galactic::kSpecialSabotage) return false;
    const objects::ObjectHandle saboteur = g->store().find(static_cast<objects::ObjectId>(dialog_.hero_id));
    const objects::ObjectHandle structure = g->store().find(static_cast<objects::ObjectId>(structure_id));
    const int32_t cost = g->sabotage_cost(local_player_, dialog_.planet, structure, saboteur);
    if (cost < 1) return false;  // the dialog's rows: cost >= 1 (P4_SIM_SYSTEMS §2.4)
    return submit_special(galactic::sabotage_structure_event(dialog_.planet_id, dialog_.hero_id, structure_id, static_cast<float>(cost), 0.0f));
}

bool GameSession::hack_super_weapon() {
    const galactic::GalacticSim* g = galaxy();
    if (!g || !dialog_open_ || dialog_.kind != galactic::kSpecialHack) return false;
    return submit_special(galactic::hack_super_weapon_event(dialog_.planet_id, dialog_.hero_id, g->hack_cost()));
}

bool GameSession::corrupt_planet(int8_t type) {
    if (!dialog_open_ || dialog_.kind != galactic::kSpecialCorruption || type < 0 || type >= 8) return false;
    return submit_special(sim::corruption_command(dialog_.hero_id, dialog_.planet_id, type));
}

std::vector<galactic::GalacticEvent> GameSession::take_special_events() {
    std::vector<galactic::GalacticEvent> out;
    out.swap(special_events_);
    return out;
}

std::vector<galactic::GalacticEvent> GameSession::take_announce_events() {
    std::vector<galactic::GalacticEvent> out;
    out.swap(announce_events_);
    return out;
}

// One galactic event for the presentation tap (special events only; everything else is ignored).
void GameSession::tap_event(const galactic::GalacticSim& gs, const galactic::GalacticEvent& e) {
    using K = galactic::GalacticEventKind;
    if (e.kind == K::PlanetOwnerChanged || e.kind == K::CorruptionChanged || e.kind == K::FleetDeparted) {
        if (announce_events_.size() < 256) announce_events_.push_back(e);  // ui4 t08 (bounded when nobody takes them)
        return;
    }
    if (e.kind < K::SpecialDialog || e.kind >= K::Count) return;
    special_events_.push_back(e);
    const bool local = e.player == local_player_;
    switch (e.kind) {
        case K::SpecialDialog:
            ++special_stats_.dialogs;
            if (local && e.planet < gs.planets().size()) {
                dialog_ = SpecialDialog{};
                dialog_.kind = static_cast<uint32_t>(e.other_player);
                dialog_.planet = e.planet;
                dialog_.planet_id = static_cast<uint32_t>(gs.store().id(gs.planets()[e.planet].object));
                dialog_.hero_id = static_cast<uint32_t>(e.object);
                dialog_.respawn_seconds = commands::dequantize(galactic::kQuantDuration, static_cast<uint32_t>(e.value));
                dialog_.frame = host_->frame();
                dialog_open_ = true;
            }
            break;
        case K::SpecialError: ++special_stats_.errors; break;
        case K::SpecialApplied: ++special_stats_.applied; break;
        case K::BlackMarketPurchase: ++special_stats_.purchases; break;
        case K::GalacticSabotage: ++special_stats_.sabotages; break;
        case K::SabotageVigilance: ++special_stats_.vigilance; break;
        case K::PlanetDestroyed: ++special_stats_.planets_destroyed; break;
        case K::SuperWeaponHacked: ++special_stats_.hacks; break;
        case K::HeroRespawnScheduled: ++special_stats_.respawns_scheduled; break;
        case K::HeroRespawned: ++special_stats_.respawned; break;
        case K::TypeUnlocked: ++special_stats_.types_unlocked; break;
        case K::Slice:
            ++special_stats_.slices;
            if (e.value == 0) ++special_stats_.slice_successes;
            break;
        default: break;
    }
}

// The galactic event stream since the last tap (the sim appends in stream order; StorySession drains it at T9, a session
// without a story drains it here after the tick). Presentation only: reads events, never sim state changes. Events
// raised inside T9 are tapped from the story's later drain rounds (start(): StorySession::set_drain_tap).
void GameSession::tap_events(bool story_drains) {
    sim::GalacticMode* gm = host_ ? host_->galactic_mode() : nullptr;
    galactic::GalacticSim* gs = gm ? gm->sim() : nullptr;
    if (!gs) return;
    const std::vector<galactic::GalacticEvent>& ev = gs->events();
    if (events_seen_ > ev.size()) events_seen_ = 0;
    for (size_t i = events_seen_; i < ev.size(); ++i) tap_event(*gs, ev[i]);
    events_seen_ = ev.size();
    if (story_drains) events_seen_ = 0;  // T9 drains the stream right after this tap
    else if (!host_->story()) {          // no story session: the stream is drained here (bounded growth)
        std::vector<galactic::GalacticEvent> drop;
        gs->drain_events(drop);
        events_seen_ = 0;
    }
}

void GameSession::after_tick() {
    save_cache_.reset();  // N1 / t09: a checked save is stale once the session ticked
    save_checked_ = false;
    tap_events(false);
    land_skirmish_invade();
    if (banner_sent_ && (!battles_ || !battles_->linked_banner_up())) banner_sent_ = false;  // story t13
}

// --- OI host: Lua presentation -> frame pacing and the parity CRC (docs/re/MAIN_LOOP.md §1.5, COMMANDS.md §7.1 step 6) ---
// Runs inside the tick right after T9, so the lock the story VMs left this tick decides this tick's parity capture (T13)
// and the next pacing decisions. The presentation state is computed by the story VMs on the sim tick, so a replay (or the
// presentation-off check) reads the same lock. [port] ParityFrameOptions is written whole (no other owner sets it yet:
// scoring / mode byte / land condition are not modelled) and only when the lock changes, so runs that never enter a
// cinematic keep the default options.
void GameSession::after_story(sim::SimHost& h) {
    sync_rate_lock(h);
    if (rate_lock_) ++rate_lock_ticks_;
}

void GameSession::sync_rate_lock(sim::SimHost& h) {
    bool lock = false;
    if (rate_lock_source_) {
        lock = rate_lock_source_();
    } else {
#ifdef ALAMO_SIM_HAVE_STORY
        if (sim::StorySession* st = h.story())
            if (const lua::PresentationLog* log = st->presentation()) {
                lock = log->state().cinematic_rate_lock();
                if (!log->state().fast_forward && h.pacing().fast()) h.pacing().set_fast(false);  // Lua Cancel_Fast_Forward
            }
#endif
    }
    if (lock != rate_lock_) {
        rate_lock_ = lock;
        h.pacing().set_cinematic_lock(lock || pres_lock_);
        ParityFrameOptions po;
        po.invert = !lock;
        h.crc_registry().set_parity_options(po);
    }
}

void GameSession::set_presentation_rate_lock(bool on) {
    pres_lock_ = on;
    if (host_) host_->pacing().set_cinematic_lock(rate_lock_ || pres_lock_);
}

void GameSession::set_fast_forward(bool on) {
    if (!host_) return;
    save_checked_ = false;  // N1 / t09: the story's presentation state changes
    host_->pacing().set_fast(on);
#ifdef ALAMO_SIM_HAVE_STORY
    if (sim::StorySession* st = host_->story())
        if (lua::PresentationLog* log = st->presentation()) log->set_fast_forward(on);
#endif
}
// --- end OI host ---

// SimHost port hook: PortExtension events of the game host, and original events no sim system handled. Every answer is
// decided from sim state at the command phase, so a replay gets the same results.
commands::RejectReason GameSession::port_event(uint8_t player, const commands::EventData& ev) {
    using R = commands::RejectReason;
    const bool local = static_cast<int32_t>(player) == local_player_;  // [port] SP: the local player only
    if (ev.type() == commands::EventType::Withdrawl) {
        // COMMANDS.md id 49 (no payload; the player = the header's) -> AutoResolve Set_Retreating(player)
        // (P4_SIM_SYSTEMS §9.2): ConflictManager::set_retreating. The executor ignores its result, so an early, late or
        // second Withdraw is a silent drop: the command itself succeeds.
        ++port_stats_.withdraw;
        if (battles_ && battles_->conflicts().set_retreating(static_cast<int32_t>(player))) ++port_stats_.withdraw_accepted;
        else ++port_stats_.withdraw_dropped;
        return R::None;
    }
    if (ev.type() != commands::EventType::PortExtension) return R::Unhandled;
    const uint32_t sub = ev.u("subtype");
    if (sub == kBattleChoiceSubtype) {  // the pending-battle choice (local player, Vote state only)
        const std::span<const uint8_t> b = ev.bytes_view("data");
        if (!local || b.size() != 1) return R::NotAllowed;
        if (battles_->conflicts().state() != galactic::ConflictState::Vote) return R::NotAllowed;
        const auto c = static_cast<galactic::BattleChoice>(b[0]);
        if (c != galactic::BattleChoice::Fight && c != galactic::BattleChoice::AutoResolve && c != galactic::BattleChoice::Retreat)
            return R::NotAllowed;
        battles_->conflicts().choose(c);
        choice_sent_ = false;
        return R::None;
    }
    if (sub == kTacticalAutoResolveSubtype) {
        // P4_SIM_SYSTEMS §9.3 / BT §4.5: the tactical IDC_AUTORESOLVE for the issuer -> CampaignBattles::auto_resolve_battle
        // (gate = the retreat coordinator's can-retreat check; the battle ends at the next battle update, resolved by the
        // galaxy from tactical). Refused when the gate refuses or no battle runs.
        ++port_stats_.tactical_auto_resolve;
        const std::span<const uint8_t> b = ev.bytes_view("data");
        if (!local || b.size() != 1 || !battles_ || !battles_->auto_resolve_battle(static_cast<int32_t>(player))) {
            ++port_stats_.tactical_auto_resolve_refused;
            return R::NotAllowed;
        }
        return R::None;
    }
    if (sub == kStoryEscSubtype) {
        // bkc1 t01 (story_ui.hpp): Story_Handle_Esc for every active plot of the current mode. The cinematic camera was
        // on when the key was sent; the call is not gated again here (retail calls it at once, before any tick).
        sim::StorySession* story = host_ ? host_->story() : nullptr;
        if (!local) return R::NotAllowed;
        if (story) story->handle_esc();
        ++port_stats_.story_esc;
        return R::None;
    }
    if (sub == kStoryUiSubtype) {
        // L1/S4 wave 1 (story_ui.hpp): the local player's UI notification -> StorySession::notify_ui (queued there, routed
        // at the next T9; `tactical` ones to the attached tactical plot set). Dropped (the command still succeeds) without
        // a story session.
        sim::StoryUiNotification n;
        if (!local || !read_story_ui_event(ev, n)) {
            ++port_stats_.story_ui_dropped;
            return R::NotAllowed;
        }
        n.player = static_cast<int32_t>(player);
        sim::StorySession* story = host_ ? host_->story() : nullptr;
        if (!story) {
            ++port_stats_.story_ui_dropped;
            return R::None;
        }
        story->notify_ui(n);
        ++port_stats_.story_ui;
        return R::None;
    }
#ifdef ALAMO_GAME_HAVE_UI
    bool p = false;
    if (!ui::read_pause_event(ev, p)) return R::Unhandled;
    if (!local) return R::NotAllowed;
    if (p && !replay_) paused_ = true;
    ++pause_events_;
    return R::None;
#else
    return R::Unhandled;
#endif
}
// --- end P4 host ---

commands::SubmitResult GameSession::submit(commands::EventData ev) {
    ++submits_;
    return host_->submit(static_cast<uint8_t>(local_player_), std::move(ev));
}


// --- N1 / t09: save / load (game_session.hpp file comment) ---
namespace {

constexpr uint32_t kGameChunkVersion = 1;

std::string join_names(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& n : v) {
        if (!s.empty()) s += ',';
        s += n;
    }
    return s;
}

std::vector<std::string> split_names(std::string_view s) {
    std::vector<std::string> out;
    while (!s.empty()) {
        const size_t c = s.find(',');
        out.emplace_back(s.substr(0, c));
        if (c == std::string_view::npos) break;
        s.remove_prefix(c + 1);
    }
    return out;
}

const char* flag(bool b) { return b ? "1" : "0"; }

sim::SessionPlayer session_player(const galactic::GalacticRules& rules, const galactic::PlayerSetup& ps, std::string ai_type) {
    sim::SessionPlayer p;
    p.faction = ps.faction < rules.factions.size() ? rules.factions[ps.faction].name : std::string();
    p.human = ps.human;
    p.ai = ps.ai;
    p.ai_type = std::move(ai_type);
    p.difficulty = ps.difficulty < rules.difficulties.size() ? rules.difficulties[ps.difficulty].name : std::string();
    return p;
}

// SessionDescriptor option -> SessionSpec field (spec_from_save). Every key must be present: a save without one was
// not written by this build's describe_session.
struct OptionReader {
    const sim::SessionDescriptor& d;
    std::string missing;
    std::string_view text(const char* key) {
        for (const auto& [k, v] : d.options)
            if (k == key) return v;
        if (missing.empty()) missing = key;
        return {};
    }
    bool boolean(const char* key) { return text(key) == "1"; }
    bool has(const char* key) const {
        for (const auto& [k, v] : d.options)
            if (k == key) return true;
        return false;
    }
    int64_t integer(const char* key) {
        const std::string_view v = text(key);
        int64_t n = 0;
        const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), n);
        if ((ec != std::errc() || p != v.data() + v.size()) && missing.empty()) missing = std::string(key) + " (not a number)";
        return n;
    }
    uint32_t u32(const char* key) { return static_cast<uint32_t>(std::clamp<int64_t>(integer(key), 0, 0xFFFFFFFFll)); }
};

}  // namespace

void GameSession::describe_session() {
    const GameData& dd = *data_;
    sim::SessionDescriptor d;
    const bool campaign = spec_.kind == SessionKind::Campaign;
    d.kind = campaign ? kCampaignKind : kSkirmishKind;
    d.campaign = campaign ? spec_.campaign : spec_.skirmish_map;
    d.data_root = dd.root;
    d.data_args = dd.args;
    for (size_t p = 0; p < setup_.players.size(); ++p)
        d.setup.push_back(session_player(dd.rules, setup_.players[p], p < ai_types_.size() ? ai_types_[p] : std::string()));
    d.difficulty = difficulty_name(spec_.difficulty);
    // Every SessionSpec field that changes what a tick computes (the seed is SimConfig's).
    d.set_option("spec.faction", spec_.faction);
    d.set_option("spec.difficulty", std::to_string(spec_.difficulty));
    d.set_option("spec.aggressor_period", std::to_string(spec_.aggressor_period));
    d.set_option("spec.battle_max_frames", std::to_string(spec_.battle_max_frames));
    d.set_option("spec.battle_end_hold_frames", std::to_string(spec_.battle_end_hold_frames));
    d.set_option("spec.battle_order_interval", std::to_string(spec_.battle_order_interval));
    d.set_option("spec.tactical_ai", flag(spec_.tactical_ai));
    d.set_option("spec.animation", flag(spec_.animation));
    d.set_option("spec.galactic_ai", flag(spec_.galactic_ai));
    d.set_option("spec.story", flag(spec_.story));
    d.set_option("spec.ask_battle_choice", flag(spec_.ask_battle_choice));
    if (!campaign) {
        const SkirmishSetup& s = spec_.skirmish;
        d.set_option("skirmish.human_faction", s.human_faction);
        d.set_option("skirmish.ai_faction", s.ai_faction);
        d.set_option("skirmish.units_human", join_names(s.units_human));
        d.set_option("skirmish.units_ai", join_names(s.units_ai));
        d.set_option("skirmish.ground_human", join_names(s.ground_human));
        d.set_option("skirmish.ground_ai", join_names(s.ground_ai));
        d.set_option("skirmish.planet_type", s.planet_type);
        d.set_option("skirmish.land", flag(s.land));
        if (s.control_points) d.set_option("skirmish.control_points", "1");  // ccpgt t02 (absent in older saves: off)
    }
    // What this build attached (a save from a build without the AI / story / animation is not this session).
    d.set_option("run.galactic_ai", flag(galactic_ai_running()));
    d.set_option("run.story", flag(story_running()));
#ifdef ALAMO_SIM_HAVE_ANIM
    d.set_option("run.animation", flag(anim_ != nullptr));
#else
    d.set_option("run.animation", "0");
#endif
    // Data fingerprint: object types, galactic rules + this galaxy, and the AI inputs (alamo_headless save_run).
    StateHasher h;
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    if (galactic_ai_ && dd.ai) {
        h.u32(static_cast<uint32_t>(dd.ai->plan_files.size()));
        for (const std::string& f : dd.ai->plan_files) h.str(f);
        h.u32(static_cast<uint32_t>(dd.ai->defs.goals.size()));
        h.u32(static_cast<uint32_t>(dd.ai->equations.size()));
    }
#endif
    h.u32(static_cast<uint32_t>(ai_types_.size()));
    for (const std::string& t : ai_types_) h.str(t);
    d.data_fingerprint = sim::combine_fingerprints({dd.types_fingerprint(), sim::fingerprint_galactic(dd.rules, setup_), h.digest()});
    d.writer = "alamo_game (N1 t09)";
    host_->set_session(std::move(d));
}

void GameSession::write_game_state(ByteWriter& w) const {
    w.u32(kGameChunkVersion);
    w.boolean(paused_).boolean(pause_pending_).boolean(invaded_).boolean(choice_sent_);
    w.u64(static_cast<uint64_t>(events_seen_));
}

bool GameSession::read_game_state(ByteReader& r) {
    if (r.u32() != kGameChunkVersion) return false;
    const bool paused = r.boolean(), pending = r.boolean(), invaded = r.boolean(), choice = r.boolean();
    const uint64_t seen = r.u64();
    if (!r.ok()) return false;
    paused_ = paused;
    pause_pending_ = pending;
    invaded_ = invaded;
    choice_sent_ = choice;
    events_seen_ = static_cast<size_t>(std::min<uint64_t>(seen, 0xFFFFFFFFull));  // tap_events clamps to the stream
    return true;
}

std::string GameSession::save_blocker() const {
    if (!started_ || !host_) return "no game is running";
    if (spec_.attract) return "the attract demo cannot be saved";
    if (replay_) return "a replay cannot be saved";
    std::string why;
    if (!host_->save_allowed(&why)) return why.empty() ? std::string("the game cannot be saved now") : why;
    return {};
}

std::string GameSession::check_save() {
    std::string why = save_blocker();
    if (!why.empty()) {
        save_cache_.reset();
        save_checked_ = false;
        return why;
    }
    const uint64_t frame = host_->frame();
    if (save_checked_ && save_check_frame_ == frame && save_check_submits_ == submits_) return save_refusal_;
    save_cache_.reset();
    SaveFile f;
    std::string err;
    if (!host_->save(f, &err)) {
        why = err.empty() ? std::string("the save failed") : err;
    } else {
        // save_file.hpp contract: every real chunk starts with its u32 version, so an empty chunk is a participant that
        // wrote nothing (a host-added SaveParticipant with an empty writer): its read fails, the save could not be loaded.
        for (const SaveChunk& c : f.chunks())
            if (c.data.empty()) {
                why = c.name + ": pending (this system cannot be saved yet)";
                break;
            }
    }
    if (why.empty()) save_cache_ = std::move(f);
    save_refusal_ = why;
    save_checked_ = true;
    save_check_frame_ = frame;
    save_check_submits_ = submits_;
    return why;
}

bool GameSession::save(SaveFile& out, const SaveMeta& meta, std::string* err) {
    const std::string why = check_save();
    if (!why.empty() || !save_cache_) {
        if (err) *err = why.empty() ? std::string("the save failed") : why;
        return false;
    }
    out = std::move(*save_cache_);
    save_cache_.reset();
    save_checked_ = false;  // handed out: a second save at this frame saves again
    out.description = encode_description(meta);
    return true;
}

SaveMeta GameSession::save_meta(std::string name, int64_t saved_at) const {
    SaveMeta m;
    m.name = std::move(name);
    const bool campaign = spec_.kind == SessionKind::Campaign;
    m.kind = campaign ? "campaign" : "skirmish";
    m.campaign = campaign ? spec_.campaign : spec_.skirmish_map;
    m.difficulty = std::clamp(spec_.difficulty, 0, 2);
    m.saved_at = saved_at;
    if (host_) m.tick = host_->frame();
    if (const galactic::GalacticSim* g = galaxy()) {
        const int32_t lp = local_player_;
        if (lp >= 0 && static_cast<size_t>(lp) < g->players().size()) {
            const uint32_t f = g->players()[static_cast<size_t>(lp)].faction;
            if (f < g->rules().factions.size()) m.faction = g->rules().factions[f].name;
        }
        const int32_t frame = std::max(g->frame(), 0);
        m.game_seconds = static_cast<uint64_t>(frame) / 30u;
        const int32_t period = galactic::econ::fiscal_period_frames(g->rules().constants.fiscal_cycle_seconds);
        if (campaign && period > 0) m.week = static_cast<uint32_t>(frame / period) + 1u;
        // A skirmish is its battle: its clock is the battle's (0:00 while the fleet is still on its way), not the galaxy's.
        if (!campaign) {
            const sim::TacticalBattle* b = battle();
            m.game_seconds = b ? b->frame() / 30u : 0u;
        }
    }
    return m;
}

bool GameSession::spec_from_save(const SaveFile& save, SessionSpec& out, std::string* err) {
    auto fail = [err](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };
    sim::SessionDescriptor d;
    std::string e;
    if (!sim::read_session(save, d, &e)) return fail(e.empty() ? std::string("no session descriptor") : e);
    SessionSpec s;
    if (d.kind == kCampaignKind) {
        s.kind = SessionKind::Campaign;
        s.campaign = d.campaign;
    } else if (d.kind == kSkirmishKind) {
        s.kind = SessionKind::Skirmish;
        s.skirmish_map = d.campaign;
    } else {
        return fail("not an alamo_game save (session kind '" + d.kind + "')");
    }
    s.seed = d.seed;
    OptionReader o{d, {}};
    s.faction = std::string(o.text("spec.faction"));
    s.difficulty = static_cast<int32_t>(std::clamp<int64_t>(o.integer("spec.difficulty"), 0, 2));
    s.aggressor_period = o.u32("spec.aggressor_period");
    s.battle_max_frames = o.u32("spec.battle_max_frames");
    s.battle_end_hold_frames = o.u32("spec.battle_end_hold_frames");
    s.battle_order_interval = o.u32("spec.battle_order_interval");
    s.tactical_ai = o.boolean("spec.tactical_ai");
    s.animation = o.boolean("spec.animation");
    s.galactic_ai = o.boolean("spec.galactic_ai");
    s.story = o.boolean("spec.story");
    s.ask_battle_choice = o.boolean("spec.ask_battle_choice");
    if (s.kind == SessionKind::Skirmish) {
        s.skirmish.human_faction = std::string(o.text("skirmish.human_faction"));
        s.skirmish.ai_faction = std::string(o.text("skirmish.ai_faction"));
        s.skirmish.units_human = split_names(o.text("skirmish.units_human"));
        s.skirmish.units_ai = split_names(o.text("skirmish.units_ai"));
        s.skirmish.ground_human = split_names(o.text("skirmish.ground_human"));
        s.skirmish.ground_ai = split_names(o.text("skirmish.ground_ai"));
        s.skirmish.planet_type = std::string(o.text("skirmish.planet_type"));
        s.skirmish.land = o.boolean("skirmish.land");
        s.skirmish.control_points = o.has("skirmish.control_points") && o.boolean("skirmish.control_points");
    }
    if (!o.missing.empty()) return fail("save option " + o.missing + " missing");
    out = std::move(s);
    return true;
}

bool GameSession::start_from_save(const SaveFile& save, std::string* err, const commands::CommandRecording* replay,
                                  commands::CommandRecording* record) {
    auto fail = [err](std::string m) {
        if (err) *err = std::move(m);
        return false;
    };
    if (started_) return fail("session already started");
    std::vector<std::string> diag;
    loading_ = true;
    const bool built = start(&diag, nullptr, nullptr);
    loading_ = false;
    if (!built) return fail("rebuild: " + (diag.empty() ? std::string("the session did not start") : diag.back()));
    std::string e;
    load_report_ = {};
    if (!host_->load(save, &e, &load_report_)) {
        if (e.empty()) e = load_report_.text();
        if (load_report_.descriptor_error.empty())
            for (const std::string& c : load_report_.failed)
                if (const SaveChunk* ch = save.find(c); ch && ch->data.empty()) {
                    e = c + ": pending (the save holds no state for this system) - " + e;
                    break;
                }
        return fail(e);
    }
    sync_rate_lock(*host_);  // the loaded story's cinematic lock -> pacing + parity, as after T9
    load_point_ = {save.tick, commands::save_digest(save.encode())};
    if (replay) {
        if (!host_->replay_after_load(replay, load_point_, &e)) return fail("replay: " + e);
        replay_ = true;
        paused_ = pause_pending_ = false;  // a replay never pauses (port_event)
    } else if (record) {
        host_->record_after_load(record, load_point_);
    }
    loaded_ = true;
    return true;
}

void GameSession::record_to(commands::CommandRecording* rec) {
    if (!host_) return;
    if (loaded_) host_->record_after_load(rec, load_point_);
    else host_->record_to(rec);
}


bool save_game(GameSession& session, const std::string& dir, std::string_view name, const std::string& title, int64_t saved_at,
               std::string* path_out, std::string* err) {
    const std::string path = save_path(dir, name);
    if (path.empty()) {
        if (err) *err = "no save name";
        return false;
    }
    SaveMeta meta = session.save_meta(sanitize_save_name(name), saved_at);
    meta.title = title;
    SaveFile f;
    if (!session.save(f, meta, err) || !write_save_file(f, path, err)) return false;
    if (path_out) *path_out = path;
    return true;
}

std::unique_ptr<GameSession> load_game(const GameData& data, jobs::JobSystem& js, const std::string& path, std::string* err,
                                       const commands::CommandRecording* replay, commands::CommandRecording* record, SaveFile* file_out) {
    SaveFile f;
    std::string e;
    if (!SaveFile::read_file(path, f, &e)) {
        if (err) *err = e.empty() ? std::string("not a readable save") : e;
        return nullptr;
    }
    SessionSpec spec;
    if (!GameSession::spec_from_save(f, spec, &e)) {
        if (err) *err = e;
        return nullptr;
    }
    auto s = std::make_unique<GameSession>(data, js, std::move(spec));
    if (!s->start_from_save(f, &e, replay, record)) {
        if (err) *err = e;
        return nullptr;  // the half-loaded session is discarded here
    }
    if (file_out) *file_out = std::move(f);
    return s;
}

}  // namespace alamo::game

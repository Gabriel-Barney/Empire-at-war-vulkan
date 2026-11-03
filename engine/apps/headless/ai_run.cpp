// alamo_headless --economy --galactic-ai: see ai_run.hpp.
#include "ai_run.hpp"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>

#if defined(ALAMO_HEADLESS_HAVE_GALACTIC_AI) && defined(ALAMO_SIM_HAVE_BATTLES)
#include "alamo/ai/ai_context.hpp"
#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/galactic_ai.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/jobs/crc_recorder.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/campaign_battles.hpp"
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/sim/story_session.hpp"
#endif
#include "alamo/vfs/vfs.hpp"
#include "port_trace.hpp"

namespace {

std::string env_text(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}


// --human-script: the scripted human's recorded command stream. One command per line ('#' starts a comment):
//   <second> build <Type> <Planet>      Production at Planet (the human's own planet)
//   <second> move <From> <To>           every stationary fleet of the human orbiting From -> To (one order each)
//   <second> invade <Planet>            the human's ground units in orbit of Planet land (Invade)
//   <second> launch <Planet>            the human's surface units at Planet go to its orbit fleet (FleetManagement
//                                       launch, one order per unit, surface order)
// <second> = galactic seconds; the command is submitted on the first T7 update at or after frame second x 30
// through SimHost::submit (the scheduled pipeline a real player's orders take). Names resolve when the command
// is due; a command whose names do not resolve is logged and skipped.
struct HumanCommand {
    uint64_t frame = 0;
    std::string op, a, b;
    uint32_t line = 0;
};

bool load_human_script(const std::string& path, std::vector<HumanCommand>& out, std::string& err) {
    std::ifstream in(alamo::long_path(alamo::path_from_utf8(path)), std::ios::binary);
    if (!in) {
        err = "cannot read " + path;
        return false;
    }
    std::string text;
    uint32_t n = 0;
    while (std::getline(in, text)) {
        ++n;
        const size_t hash = text.find('#');
        if (hash != std::string::npos) text.erase(hash);
        std::vector<std::string> w;
        size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r')) ++i;
            size_t e = i;
            while (e < text.size() && text[e] != ' ' && text[e] != '\t' && text[e] != '\r') ++e;
            if (e > i) w.push_back(text.substr(i, e - i));
            i = e;
        }
        if (w.empty()) continue;
        HumanCommand c;
        c.line = n;
        c.frame = std::strtoull(w[0].c_str(), nullptr, 10) * 30u;
        if (w.size() >= 2) c.op = w[1];
        if (w.size() >= 3) c.a = w[2];
        if (w.size() >= 4) c.b = w[3];
        const bool ok = (c.op == "build" && w.size() == 4) || (c.op == "move" && w.size() == 4) || ((c.op == "invade" || c.op == "launch") && w.size() == 3);
        if (!ok) {
            err = path + ":" + std::to_string(n) + ": bad command";
            return false;
        }
        out.push_back(std::move(c));
    }
    std::stable_sort(out.begin(), out.end(), [](const HumanCommand& x, const HumanCommand& y) { return x.frame < y.frame; });
    return true;
}

#ifdef ALAMO_SIM_HAVE_STORY
// q6 t07: a fired story reward "SWITCH_CONTROL[<player>|<AI type>]" (the story log's reward text) -> its two
// parameters. Reward_Param1 = the player (faction name, or "#player:N" for a player object), Reward_Param2 = the
// AIPlayerType name. Thrawn's Revenge fires it from Lua (StoryUtil.ChangeAIPlayer: Set_Reward_Parameter + Story_Event).
bool parse_switch_control(const std::string& reward, std::string& player, std::string& type) {
    static constexpr std::string_view kHead = "SWITCH_CONTROL[";
    if (reward.size() < kHead.size() + 1 || reward.compare(0, kHead.size(), kHead) != 0 || reward.back() != ']') return false;
    const std::string body = reward.substr(kHead.size(), reward.size() - kHead.size() - 1);
    const size_t bar = body.find('|');
    if (bar == std::string::npos) return false;
    player = body.substr(0, bar);
    const size_t bar2 = body.find('|', bar + 1);
    type = body.substr(bar + 1, bar2 == std::string::npos ? std::string::npos : bar2 - bar - 1);
    return !player.empty() && !type.empty();
}
#endif

}  // namespace

int ai_campaign_run(const MapRunOptions& opt, const AiRunOptions& aopt) {
    using namespace alamo;
    const std::string root = opt.game_dir.empty() ? env_text("ALAMO_FOC_DATA") : opt.game_dir;
    if (root.empty()) {
        std::printf("--galactic-ai: no --game <corruption dir> and ALAMO_FOC_DATA unset: skipped\n");
        return 0;
    }
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    // Text-only data (extracted/foc_data_text.zip unzipped) has no sibling GameData to auto-detect, so the EaW tier would be
    // missing and the AI definitions (ai.goals CRC: the EaW-only Ewok / intervention / sandbox sets) would differ from the
    // goldens, which come from a full install. ALAMO_EAW_DATA (a folder with Data\XML: the install's GameData, or
    // eaw_data_text.zip unzipped) names the EaW tier explicitly. Only on the environment route; --game decides for itself.
    if (opt.game_dir.empty()) {
        const std::string eaw = env_text("ALAMO_EAW_DATA");
        if (!eaw.empty()) vcfg.eaw_root = eaw;
    }
    vfs::parse_command_line(opt.game_args, vcfg);
    std::string err;
    std::unique_ptr<vfs::Vfs> files = vfs::Vfs::create(vcfg, &err);
    if (!files) {
        std::fprintf(stderr, "VFS: %s\n", err.c_str());
        return 2;
    }
    std::vector<std::string> diag;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::ConflictRules crules;
    if (!db.load(*files, {}, &diag) || !raw.load(*files, &diag) || !types.build(raw, files.get(), &diag) ||
        !galactic::load_galactic_rules(*files, db, types, rules, &diag)) {
        std::fprintf(stderr, "galactic-ai: game data incomplete (%zu diagnostics)\n", diag.size());
        return 2;
    }
    std::vector<std::string> cdiag;
    galactic::load_conflict_rules(*files, db, types, rules, crules, &cdiag);
    const sim::BattleCatalog catalog(types, files.get(), &db.constants(), &cdiag);
    // AI data, perception equations, plan scripts.
    ai::AiDefinitions defs;
    const auto lister = [&files](std::string_view g) { return files->list(g); };
    if (!ai::load_ai_definitions(*files, ai::list_ai_data(lister), defs)) {
        std::fprintf(stderr, "galactic-ai: no AI definitions found\n");
        return 2;
    }
    ai::EquationSet eqs;
    ai::load_equation_files(*files, files->list(ai::kPerceptionEquationGlob), eqs);
    const std::vector<std::string> plan_files = ai::list_plan_files(lister);
    const std::vector<ai::GalacticAiDifficulty> difficulties = ai::load_ai_difficulties(*files);
    const std::vector<ai::GalacticAiTypeInfo> tinfo = ai::make_galactic_ai_type_info(types);

    const std::string name = !opt.map.empty() ? opt.map : std::string("Sandbox_Heart_Of_The_Maw_Underworld");
    galactic::GalacticSetup setup = galactic::build_campaign_setup(db, rules, name, {}, &diag);
    if (setup.map.planets.size() == 0) {
        std::fprintf(stderr, "galactic-ai: campaign '%s' has no planets\n", name.c_str());
        return 2;
    }
    // AI types from the campaign's AI_Player_Control pairs (AI_GALACTIC §1.2); the unassigned (human) slot plays its
    // faction's Basic_AI [port: AI-vs-AI], except the --human faction (a scripted human). Difficulty Normal [port].
    const uint32_t normal = rules.find_difficulty("Normal_Default");
    ai::GalacticAiConfig gcfg;
    gcfg.difficulties = difficulties;
    gcfg.contrast = &crules.contrast;  // A2 stage 2: the PGAICommands contrast table (Contrast marshaller)
    gcfg.ai_uses_fog_galactic = db.constants().get_bool("AIUsesFogOfWarGalactic", gcfg.ai_uses_fog_galactic);  // shipped False
    const data::CampaignDef* cdef = db.find_campaign(name);
    const std::vector<std::string> no_pairs;
    const std::vector<std::string>& control = cdef ? cdef->ai_player_control : no_pairs;
    const std::map<std::string, std::string> basic_ai = ai::load_faction_basic_ai(*files);
    int32_t human = -1;
    for (size_t p = 0; p < setup.players.size(); ++p) {
        galactic::PlayerSetup& ps = setup.players[p];
        const std::string fac = ps.faction < rules.factions.size() ? rules.factions[ps.faction].name : std::string();
        std::string type = ai::campaign_ai_type(control, basic_ai, fac, true);
        for (const std::string& o : aopt.ai_types) {  // q6 t07 [port] --ai-type FACTION=TYPE (last one wins)
            const size_t eq = o.find('=');
            if (eq != std::string::npos && ai::ai_iequal(ai::ai_trim(std::string_view(o).substr(0, eq)), fac))
                type = ai::ai_iequal(ai::ai_trim(std::string_view(o).substr(eq + 1)), "Human") ? std::string()
                                                                                                 : std::string(ai::ai_trim(std::string_view(o).substr(eq + 1)));
        }
        if (!type.empty() && !defs.find_player(type)) {
            std::printf("galactic-ai: player %zu (%s): Unrecognized AI player type %s\n", p, fac.c_str(), type.c_str());
            type.clear();
        }  // unknown type: no AI ("Unrecognized AI player type")
        if (type.empty() || (ps.faction < rules.factions.size() && rules.factions[ps.faction].is_neutral)) continue;
        if (!aopt.human.empty() && human < 0 && ai::ai_iequal(fac, aopt.human)) {
            ps.human = true;
            ps.ai = false;
            human = static_cast<int32_t>(p);
            continue;
        }
        ps.human = false;
        ps.ai = true;
        ps.difficulty = normal;
        gcfg.players.push_back({static_cast<int32_t>(p), type, 1});
    }
    if (!aopt.human.empty() && human < 0) {
        std::fprintf(stderr, "galactic-ai: --human %s: no playable player of that faction\n", aopt.human.c_str());
        return 2;
    }
    std::vector<HumanCommand> human_cmds;
    if (!aopt.human_script.empty()) {
        if (human < 0) {
            std::fprintf(stderr, "galactic-ai: --human-script needs --human\n");
            return 2;
        }
        if (!load_human_script(aopt.human_script, human_cmds, err)) {
            std::fprintf(stderr, "galactic-ai: %s\n", err.c_str());
            return 2;
        }
    }
    std::printf("galactic-ai: campaign %s, %zu planets, %zu AI players, human %d (%zu scripted commands), %zu goal types, %zu plan files, "
                "%zu equations\n",
                name.c_str(), setup.map.planets.size(), gcfg.players.size(), human, human_cmds.size(), defs.goals.size(), plan_files.size(),
                eqs.size());
    if (aopt.verbose) {  // q6 t07: which AIPlayerType each AI player got
        std::string types_line = "galactic-ai: AI types:";
        for (const ai::GalacticAiPlayerConfig& pc : gcfg.players) types_line += " p" + std::to_string(pc.player) + "=" + pc.ai_type;
        std::printf("%s\n", types_line.c_str());
    }

    struct Out {
        CrcLog crc;
        std::string ai_text;
        std::string summary;
    };
    auto run_one = [&](int32_t workers, Out& out, bool print) -> bool {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        // Declared before the host: the host's CRC registry captures the world (hashers may still run while
        // the host shuts down), so the world must outlive it.
        std::unique_ptr<ai::GalacticAiWorld> world_holder;
        sim::SimConfig cfg;
        cfg.seed = opt.seed;
        cfg.story_crc = aopt.story;
        cfg.players = sim::session_players(setup.players.size());
        sim::SimHost host(types, js, cfg);
        if (opt.scoring)  // q6 t06
            if (const auto scoring = db.constants().get("Game_Scoring_Script_Name"))
                host.set_scoring_script(files.get(), std::string(*scoring));
        galactic::GalacticSim* gs = host.start_galactic(rules, setup);
        if (!gs) return false;
        if (print)
            for (const auto& m : host.scoring_messages()) std::printf("scoring: %s\n", m.c_str());
        if (aopt.story) {
            if (!cdef) return false;
#ifdef ALAMO_SIM_HAVE_STORY
            // q2f t03: the campaign's story plot sets (as map_run.cpp story_run). The retail sandbox runs them, and their
            // start-of-game hero spawns are part of its tick-1 RNG draws (docs/re/GALACTIC_RNG_DRAWS.md).
            sim::StoryStart st;
            st.files = files.get();
            st.plot_sets = sim::campaign_plot_sets(*cdef, *gs);
            std::vector<std::string> sd;
            if (!host.start_story(st, &sd) || !host.story()) {
                for (const auto& d : sd) std::fprintf(stderr, "galactic-ai: story: %s\n", d.c_str());
                return false;
            }
#else
            std::fprintf(stderr, "galactic-ai: --story: this build has no story session\n");
            return false;
#endif
        }
        // q2f t05: the state trace reads the sim after each tick (never writes it); the first run only.
        PortStateTrace trace;
        if (print && !aopt.trace_out.empty()) {
            std::string terr;
            const std::vector<std::pair<std::string, std::string>> meta = {
                {"reader", "alamo_headless"}, {"scenario", "galactic"}, {"game", "foc"}, {"campaign", name},
                {"label", "alamo_headless --economy --galactic-ai" + std::string(aopt.story ? " --story" : "") +
                              (aopt.human.empty() ? std::string() : " --human " + aopt.human)},
                {"game_type", "0"}, {"mode_type", "0"}, {"target_ticks", std::to_string(opt.ticks)}};
            if (!trace.open(aopt.trace_out, meta, cfg.seed, terr)) {
                std::fprintf(stderr, "galactic-ai: %s\n", terr.c_str());
                return false;
            }
        }
        sim::CampaignBattleOptions bo;
        bo.conflict.human_choice = aopt.battles == "auto" ? galactic::BattleChoice::AutoResolve : galactic::BattleChoice::Fight;
        bo.conflict.fight_ai_battles = aopt.battles == "fight-all";
        bo.tactical = aopt.battles != "auto";
        sim::load_tactical_systems(*files, rules, &db.constants(), bo);  // Int-final: weather, retreat, strikes data
        ai::GalacticAiWorld* world_ptr = nullptr;
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        // --- A2 hook: the AI's galactic budget categories for the A1 land-battle credit transfer ---
        std::vector<std::vector<ai::tactical::TacticalBudgetCategory>> budget_rows(gs->players().size());
        bo.tactical_budget = [&world_ptr, &budget_rows](int32_t p) -> std::span<ai::tactical::TacticalBudgetCategory> {
            if (!world_ptr || p < 0 || static_cast<size_t>(p) >= budget_rows.size()) return {};
            auto& rows = budget_rows[static_cast<size_t>(p)];
            rows.clear();
            for (const auto& r : world_ptr->tactical_budget_rows(p)) rows.push_back({r.allowance, r.available, r.pool});
            return rows;
        };
#endif
        // battle host: battle objects leaving the mode -> the galactic AI's survival learning (0x3a8380).
        bo.object_removed = [&world_ptr](int32_t owner, uint32_t type, uint32_t original, bool survived) {
            if (world_ptr) world_ptr->record_object_removal(owner, type, original, survived);
        };
        sim::CampaignBattles battles(host, crules, catalog, sim::CampaignBattles::file_map_loader(*files), bo);
        std::vector<std::string> ad;
        if (!battles.attach(&ad)) return false;
        // battle host: the auto-resolve survival roll -> the galactic AI's survival learning (STRATEGIC_GAPS 1.1).
        battles.conflicts().on_survival_roll = [&world_ptr](int32_t owner, uint32_t type, bool survived) {
            if (world_ptr) world_ptr->record_auto_resolve_survival(owner, type, survived);
        };
        ai::GalacticAiConfig wcfg = gcfg;
        wcfg.lua_gc_pf = host.parallel_for();  // OPT2 t04: scheduled Lua collections on the host's job system
        wcfg.background = jobs::make_background_start(host.jobs());  // OPT2 t02: background path recomputes
        if (opt.ai_latency >= 0) wcfg.path_latency = static_cast<uint32_t>(opt.ai_latency);  // parity knobs
        wcfg.stagger = opt.ai_stagger;
        world_holder = std::make_unique<ai::GalacticAiWorld>(
            *gs, host.galactic_mode()->primary(), &types, files.get(), host.random(), defs, eqs, tinfo,
            [&host](uint8_t p, const commands::EventData& ev) { return host.execute_direct(p, commands::CommandSource::AI, ev); },
            wcfg);
        ai::GalacticAiWorld& world = *world_holder;
        world_ptr = &world;
        std::vector<std::string> wd;
        if (!world.load_plans(plan_files, &wd) || !world.start(&wd)) {
            for (const auto& d : wd) std::fprintf(stderr, "  %s\n", d.c_str());
            return false;
        }
        world.register_crc(host.crc_registry());
        world.set_global_values(host.global_values());  // Int-final: session-wide GlobalValue table
        // T7: the AI players' updates, player index order (SimHost calls controllers in index order). q6 t07: every player
        // but the scripted human gets one, because a SWITCH_CONTROL reward can give a player an AI later; a player
        // without an AI does nothing there (as before: no update_player call).
        for (size_t pi = 0; pi < setup.players.size(); ++pi) {
            if (static_cast<int32_t>(pi) == human) continue;
            host.set_controller(static_cast<uint8_t>(pi), [&world, &battles](sim::SimHost& h, uint8_t p, uint64_t frame) {
                if (!world.is_ai_player(p)) return;
                const sim::GameMode* top = h.modes().top();
                const bool galactic_top = top && !top->tactical();
                const bool busy = battles.conflicts().state() == galactic::ConflictState::AutoRunning;
                world.update_player(p, static_cast<int64_t>(frame), galactic_top, busy);
            });
        }
        // The scripted human (--human): its recorded commands go through the scheduled pipeline (SimHost::submit).
        std::vector<std::string> human_log;
        size_t next_cmd = 0;
        if (human >= 0)
            host.set_controller(static_cast<uint8_t>(human), [&](sim::SimHost& h, uint8_t p, uint64_t frame) {
                const sim::GameMode* top = h.modes().top();
                if (!top || top->tactical()) return;  // galactic orders only while the galaxy is on top
                const auto& names = gs->map().planets.name;
                auto planet = [&](const std::string& n) -> PlanetIndex {
                    for (size_t i = 0; i < names.size(); ++i)
                        if (ai::ai_iequal(names[i], n)) return static_cast<PlanetIndex>(i);
                    return kInvalidPlanet;
                };
                auto id_of = [&](objects::ObjectHandle o) { return static_cast<uint32_t>(gs->store().id(o)); };
                while (next_cmd < human_cmds.size() && human_cmds[next_cmd].frame <= frame) {
                    const HumanCommand& c = human_cmds[next_cmd++];
                    std::string what = c.op + " " + c.a + (c.b.empty() ? "" : " " + c.b);
                    std::vector<commands::EventData> evs;
                    const PlanetIndex pa = planet(c.a);
                    if (c.op == "build") {
                        const PlanetIndex pb = planet(c.b);
                        const uint32_t t = rules.find_type(c.a);
                        if (pb != kInvalidPlanet && t != galactic::kNoIndex) {
                            commands::EventData ev(commands::EventType::Production);
                            ev.set("location", id_of(gs->planets()[pb].object));
                            ev.set("type_index", t);
                            evs.push_back(std::move(ev));
                        }
                    } else if (c.op == "move") {
                        const PlanetIndex pb = planet(c.b);
                        if (pa != kInvalidPlanet && pb != kInvalidPlanet)
                            for (const uint32_t f : gs->fleets_at(pa, p)) {
                                commands::EventData ev(commands::EventType::MoveObjectToObject);
                                ev.set("object", id_of(gs->fleets()[f].object));
                                ev.set("target", id_of(gs->planets()[pb].object));
                                evs.push_back(std::move(ev));
                            }
                    } else if (c.op == "launch" && pa != kInvalidPlanet) {
                        for (const objects::ObjectHandle u : gs->planets()[pa].surface)
                            if (gs->store().alive(u) && gs->store().owner(u) == p)
                                evs.push_back(galactic::fleet_management_event(galactic::kFleetLaunch, commands::kNoObject,
                                                                               commands::kNoObject, id_of(u)));
                    } else if (c.op == "invade" && pa != kInvalidPlanet) {
                        commands::EventData ev(commands::EventType::Invade);
                        ev.set("planet", id_of(gs->planets()[pa].object));
                        evs.push_back(std::move(ev));
                    }
                    if (evs.empty()) what += " (nothing to order)";
                    for (commands::EventData& ev : evs)
                        if (const commands::SubmitResult r = h.submit(p, std::move(ev)); !r.ok())
                            what += " (not accepted: " + std::string(commands::reject_reason_name(r.status)) + ")";
                    human_log.push_back(std::to_string(frame) + " human p" + std::to_string(p) + " " + what + " x" + std::to_string(evs.size()));
                }
            });
        // Players without an AI still get a perception refresh through the first AI player's call.
        const auto t0 = std::chrono::steady_clock::now();
        uint64_t n = 0;
        std::string& t = out.ai_text;
        std::vector<int32_t> owner_prev;
        for (const galactic::PlanetState& ps : gs->planets()) owner_prev.push_back(ps.owner);
        std::string owner_log;
        uint32_t owner_changes = 0;
        if (trace.is_open()) trace.start(*gs, cfg.seed);
        size_t story_seen = 0;  // q6 t07: fired story log entries already scanned for SWITCH_CONTROL
        while (n < opt.ticks && host.tick_once()) {
            ++n;
#ifdef ALAMO_SIM_HAVE_STORY
            // q6 t07: story reward SWITCH_CONTROL (AI_GALACTIC §1.2: Set_AI_Type, then the difficulty). [port] The story
            // session leaves this reward to the host and has no hook for it, so the host reads the fired log after the
            // tick: the new AI is in place before the next T7, as when the reward runs in T9; only this tick's end-of-tick
            // AI CRC still shows the old one. Difficulty = the run's Normal [port: the original reads the session
            // difficulty global]. The scripted human keeps its control [port].
            if (const sim::StorySession* story = host.story()) {
                const auto& fired = story->log();
                for (; story_seen < fired.size(); ++story_seen) {
                    std::string who, type;
                    if (!parse_switch_control(fired[story_seen].reward, who, type)) continue;
                    int32_t player = -1;
                    if (who.rfind("#player:", 0) == 0) player = std::atoi(who.c_str() + 8);
                    else player = gs->player_of_faction(who);
                    std::string note = std::to_string(fired[story_seen].frame) + " SWITCH_CONTROL " + who + " " + type + ": ";
                    if (player < 0 || static_cast<size_t>(player) >= setup.players.size()) {
                        note += "no such player";
                    } else if (player == human) {
                        note += "scripted human, ignored";
                    } else {
                        std::vector<std::string> sd;
                        const bool changed = world.set_ai_type(player, type, 1, &sd);
                        note += changed ? "p" + std::to_string(player) + " is now " + (world.ai_type(player).empty() ? "Human" : world.ai_type(player))
                                        : (sd.empty() ? "no change" : sd.front());
                    }
                    if (print && aopt.verbose) std::printf("galactic-ai: %s\n", note.c_str());
                }
            }
#endif
            if (trace.is_open()) {
                const sim::GameMode* top = host.modes().top();
                PortStateTrace::Sample ts;
                ts.global_frame = host.frame();
                ts.mode_type = top ? static_cast<uint32_t>(top->kind()) : 0u;
                ts.rng_state = host.random().synced().state();
                ts.stack_depth = static_cast<uint32_t>(host.modes().depth());
                trace.sample(*gs, ts);
            }
            for (size_t i = 0; i < owner_prev.size() && i < gs->planets().size(); ++i) {  // planets changing hands
                const int32_t o = gs->planets()[i].owner;
                if (o == owner_prev[i]) continue;
                owner_log += std::to_string(gs->frame()) + " owner " + gs->map().planets.name[i] + " " + std::to_string(owner_prev[i]) +
                             " -> " + std::to_string(o) + "\n";
                owner_prev[i] = o;
                ++owner_changes;
            }
            if (n % 1800 == 0 || n == opt.ticks) {
                char line[256];
                int k = std::snprintf(line, sizeof line, "# t=%" PRIu64 "s planets", n / 30);
                for (size_t p = 0; p < gs->players().size(); ++p)
                    k += std::snprintf(line + k, sizeof line - static_cast<size_t>(k), " %zu=%d", p, gs->planets_owned(static_cast<int32_t>(p)));
                std::snprintf(line + k, sizeof line - static_cast<size_t>(k), " plans %zu conflicts %zu crc %016" PRIx64 "\n",
                              world.running_plans(), battles.conflicts().log().size(), host.crc_log().records.back().combined);
                t += line;
            }
        }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (trace.is_open()) {
            std::string terr;
            if (!trace.finish("ticks_reached", terr)) {
                std::fprintf(stderr, "galactic-ai: %s\n", terr.c_str());
                return false;
            }
            std::printf("wrote %s\n", aopt.trace_out.c_str());
        }
        out.crc = host.crc_log();
        for (const ai::AiDecision& d : world.decision_log())
            t += std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text + "\n";
        for (const std::string& l : human_log) t += l + "\n";
        t += owner_log;
        for (const auto& e : battles.conflicts().log()) t += "conflict " + e.text(*gs) + "\n";
        const std::vector<std::string> le = world.lua_errors();
        char line[896];
        int k = std::snprintf(line, sizeof line,
                              "galactic-ai %s workers %d: %" PRIu64 " ticks (%.1f ms), galactic frame %d, decisions %zu, orders %" PRIu64
                              " (rejected %" PRIu64 "), conflicts %zu, battles %u, planet owner changes %u, running plans %zu, Lua errors %zu,",
                              name.c_str(), workers, n, ms, gs->frame(), world.decision_log().size(), world.orders_issued(),
                              world.orders_rejected(), battles.conflicts().log().size(), battles.battles_fought(), owner_changes,
                              world.running_plans(), le.size());
        if (human >= 0)
            k += std::snprintf(line + k, sizeof line - static_cast<size_t>(k), " human p%d: %zu commands, pipeline applied %" PRIu64
                               " rejected %" PRIu64 ",", human, human_log.size(), host.commands_applied(), host.commands_rejected());
        k += std::snprintf(line + k, sizeof line - static_cast<size_t>(k), " planets:");
        for (size_t p = 0; p < gs->players().size(); ++p)
            k += std::snprintf(line + k, sizeof line - static_cast<size_t>(k), " %zu=%d/%.0f", p, gs->planets_owned(static_cast<int32_t>(p)),
                               static_cast<double>(gs->credits(static_cast<int32_t>(p))));
        std::snprintf(line + k, sizeof line - static_cast<size_t>(k), ", final CRC %016" PRIx64 "\n",
                      out.crc.records.empty() ? 0 : out.crc.records.back().combined);
        out.summary = line;
        std::printf("%s", line);
        if (print && aopt.verbose) {
            std::map<std::string, uint32_t> errs;
            for (std::string e : le) {
                const size_t a = e.find("thread "), b = e.find(" (", a == std::string::npos ? 0 : a);
                if (a != std::string::npos && b != std::string::npos) e.erase(a, b - a);  // group by message
                ++errs[e.substr(0, 220)];
            }
            for (const auto& [e, c] : errs) std::printf("  lua x%u: %s\n", c, e.c_str());
            for (const auto& [s, c] : world.proposal_stats()) std::printf("  proposals x%u: %s\n", c, s.c_str());
        }
        return true;
    };
    const int32_t w0 = opt.check_workers ? jobs::determinism_worker_counts().front() : opt.workers;
    Out first;
    if (!run_one(w0, first, true)) {
        std::fprintf(stderr, "galactic-ai: campaign start failed\n");
        return 1;
    }
    bool ok = true;
    if (opt.check_workers) {
        const auto counts = jobs::determinism_worker_counts();
        for (size_t i = 1; i < counts.size(); ++i) {
            Out o;
            run_one(counts[i], o, false);
            const auto d = first_divergence(first.crc, o.crc);
            std::printf("  workers %d vs %d: CRC %s, AI log %s\n", counts.front(), counts[i], d.message.c_str(),
                        o.ai_text == first.ai_text ? "identical" : "DIFFERS");
            if (d.diverged || o.ai_text != first.ai_text) ok = false;
        }
    }
    const std::string header = "# galactic-ai campaign " + name + " battles " + aopt.battles + " seed " + std::to_string(opt.seed) +
                               " ticks " + std::to_string(opt.ticks) + (aopt.human.empty() ? "" : " human " + aopt.human) + "\n# " +
                               first.summary;
    first.crc.comments = {header.substr(2, header.find('\n') - 2)};
    if (!opt.crc_out.empty() && first.crc.save(opt.crc_out, &err)) std::printf("wrote %s\n", opt.crc_out.c_str());
    if (!aopt.ai_log.empty()) {
        std::ofstream(long_path(path_from_utf8(aopt.ai_log)), std::ios::binary) << header << first.ai_text;
        std::printf("wrote %s\n", aopt.ai_log.c_str());
    }
    if ((!aopt.ai_golden.empty() || !opt.golden.empty()) && files->eaw_root().empty())
        std::printf("note: no EaW tier in the VFS (no sibling GameData, no ALAMO_EAW_DATA): the AI definitions lack the EaW-only "
                    "files, so goldens made from a full install will differ (ai.goals)\n");
    if (!aopt.ai_golden.empty()) {
        std::ifstream in(long_path(path_from_utf8(aopt.ai_golden)), std::ios::binary);
        const std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        // The '# t=' lines carry the sampled CRCs: compare everything except the header lines.
        auto body = [](const std::string& s) {
            std::string out;
            size_t i = 0;
            while (i < s.size()) {
                const size_t e = std::min(s.find('\n', i), s.size());
                const std::string_view l(s.data() + i, e - i);
                if (!(l.size() > 1 && l[0] == '#' && l.substr(0, 4) != "# t=")) out.append(l).push_back('\n');
                i = e + 1;
            }
            out.erase(std::remove(out.begin(), out.end(), '\r'), out.end());
            return out;
        };
        const bool same = !want.empty() && body(want) == body(header + first.ai_text);
        std::printf("AI golden %s: %s\n", aopt.ai_golden.c_str(), same ? "MATCH" : "DIFFERS");
        if (!same) ok = false;
    }
    if (!opt.golden.empty()) {
        CrcLog golden;
        if (!CrcLog::load(opt.golden, golden, &err)) {
            std::fprintf(stderr, "golden: %s\n", err.c_str());
            return 2;
        }
        const auto d = first_divergence(golden, first.crc);
        std::printf("golden: %s\n", d.message.c_str());
        if (d.diverged) ok = false;
    }
    std::printf(ok ? "galactic-ai run: OK\n" : "galactic-ai run: FAILED\n");
    return ok ? 0 : 1;
}

#else

int ai_campaign_run(const MapRunOptions&, const AiRunOptions&) {
    std::fprintf(stderr, "--galactic-ai: this build has no galactic AI (engine/ai + engine/lua) or no S10 battles\n");
    return 2;
}

#endif

// alamo_sim_battle_story_tests: S10 battle transitions, host side (docs/re/BATTLE_TRANSITIONS.md): story battles (L1/S4 wave 2a), linked battles, story victory rewards, Invade() of scripted fleets, S4 story battles.
// Split out of test_battles.cpp (WF1b t01); the fixture (synthetic data, maps, run_campaign) is battle_test_fixture.hpp.

#include "battle_test_fixture.hpp"
#include "alamo/core/host_fs.hpp"

namespace {

#ifdef ALAMO_SIM_HAVE_STORY
// L1/S4 wave 2a story hookup: a galactic plot whose STORY_SPACE_TACTICAL names tactical plots; CampaignBattles
// attaches them at the push (StorySession::begin_battle with the battle's bridge), feeds deaths, ends with
// VICTORY. The tactical script activates POWER_TO_WEAPONS through the battle's AbilitySystem (no command 43 is
// sent here, so every activation counted by the battle comes from Lua). Logs identical for 0 / 4 workers.
struct StoryRun {
    std::string log;
    std::vector<std::string> lua;
    uint32_t activations = 0;
    bool fog_on_seen = false, fog_off_seen = false;  // fog follow-up: the battle's FOW-active flag over the run
};
StoryRun run_story_battle(const Data& d, const FileProvider& files, int32_t workers) {
    StoryRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    cfg.story_crc = true;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;  // a provider created after the story files were written
    st.plot_sets = {{0, "Story_Plots_S10.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& m : diag) std::printf("  story diag: %s\n", m.c_str());
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    for (uint64_t t = 0; t < 2500; ++t) {
        host.tick_once();
        if (battles.battle()) out.activations = battles.battle()->stats().activations;
        if (battles.battle())
            if (const tactical::fog::FogOfWar* f = battles.battle()->fog()) {
                out.fog_on_seen = out.fog_on_seen || f->fow_active();
                out.fog_off_seen = out.fog_off_seen || !f->fow_active();
            }
    }
    if (host.story()) {
        out.log = host.story()->log_text("S10 story battle");
        out.lua = host.story()->lua_messages();
    }
    return out;
}

// BT §2.9 / REMAINING_GAPS §6.4: a galactic plot's LINK_TACTICAL (STORY_ELAPSED 1 s) starts its battle at the battle
// hook right after the reward (ConflictManager::start_linked: queued at the front, no validation / decide). The story flag
// (GalacticSim::linked_battle_pending; paused() refuses FleetManagement while the local player is in a queued conflict)
// is set by the record and cleared at the start, so no tick ends with it set. Cases: space at S10_B (an Underworld
// frigate already orbits it); land at S10_B with no Underworld unit anywhere near (the record has no units: the validation used to drop it); land sandbox (Param8 1: the
// human defender's surface units start, the AI attacker gets nothing).
struct LinkedRun {
    bool started = false, land = false, flag_after_tick = false;
    uint64_t reward_tick = 0, start_tick = 0;
    size_t forces = 0, def_start = 0, att_start = 0, att_reserve = 0;
    uint32_t fought = 0;
    std::string log, conflicts;
    // story t07: the record's P9-P14 as the battle sees them, and the P13 banner (ask = the battle dialog answers Pending).
    CampaignBattles::LinkedPresentation presentation;
    galactic::LinkedResolution resolution = galactic::LinkedResolution::None, resolution_after = galactic::LinkedResolution::None;
    bool skipped_dialog = false;
    uint64_t banner_tick = 0, begin_tick = 0;
    bool paused_in_banner = true, begin_refused_early = false;
    // story t10: `post` puts an Underworld (attacker) S10_Trooper (Is_Branched_Map_Discardable by default) on the land map;
    // -1 = not checked, else whether it is in the battle at its first frame; and the request's linked flag.
    int post_kept = -1;
    bool request_linked = false;
};

// `params9`: Reward_Param9.. XML appended after Param8 (story t07). `ask`: human_choice Pending (the game's battle dialog), so
// a P13 banner waits for ResumeGame, sent 20 ticks after it went up. `post`: see LinkedRun::post_kept.
LinkedRun run_linked(const Data& d, bool space, bool sandbox, const std::string& params9 = {}, bool ask = false, bool post = false) {
    LinkedRun out;
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_Link.xml", "<Story_Mode_Plots><Active_Plot>Story_Link.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_Link.xml",
               std::string("<Story>\n"
                           " <Event Name=\"Link\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>1</Event_Param1>"
                           "<Reward_Type>LINK_TACTICAL</Reward_Type><Reward_Param1>S10_B</Reward_Param1><Reward_Param2>") +
                   (space ? "SPACE" : "LAND") + "</Reward_Param2><Reward_Param3>Underworld</Reward_Param3><Reward_Param4> " +
                   (space ? "S10_Space_B" : "S10_Land_B") + " </Reward_Param4><Reward_Param5>Empire</Reward_Param5>"
                   "<Reward_Param6></Reward_Param6><Reward_Param7></Reward_Param7><Reward_Param8>" + (sandbox ? "1" : "0") +
                   "</Reward_Param8>" + params9 + "</Event>\n</Story>\n");
    DirectoryProvider files(path_to_utf8(d.root));
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, !space);
    if (space) setup.forces.push_back({1, 1, type_of(d, "S10_Frigate")});  // an Underworld frigate already orbits S10_B
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{0, "Story_Plots_Link.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    CampaignBattleOptions opt;
    opt.conflict.human_choice = ask ? galactic::BattleChoice::Pending : galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [post](const std::string& name, terrain::MapData& m) {
                                if (name == "S10_Space_B") m = space_map();
                                else if (name == "S10_Land_B") m = land_map();
                                else return false;
                                if (post && name == "S10_Land_B") {
                                    terrain::ObjectPlacement pl;
                                    pl.type_crc = name_crc("S10_Trooper");
                                    pl.owner = 1;  // Underworld (faction index 1)
                                    pl.position[0] = 700.0f;
                                    pl.position[1] = 1800.0f;
                                    m.objects.push_back(pl);
                                }
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    for (uint64_t t = 0; t < 400 && battles.battles_fought() == 0; ++t) {
        const size_t fired = host.story() ? host.story()->linked_tactical().size() : 0;
        host.tick_once();
        // story t07: the record waits while its banner is up (the flag stays set then, so it is only tracked outside it)
        out.flag_after_tick = out.flag_after_tick || (gs->linked_battle_pending() && !battles.linked_banner_up() && !battles.linked_record());
        if (host.story() && fired == 0 && !host.story()->linked_tactical().empty()) out.reward_tick = host.frame();
        if (battles.linked_banner_up()) {
            if (out.banner_tick == 0) out.banner_tick = host.frame();
            out.paused_in_banner = out.paused_in_banner && gs->paused();
            if (host.frame() == out.banner_tick + 20 && out.begin_tick == 0) {
                commands::EventData ev(commands::EventType::ResumeGame);
                ev.set("flag", 0);  // Begin
                host.submit(0, ev);
                out.begin_tick = host.frame() + 1;  // executed at the next tick's command phase
            }
        }
        if (battles.battle() && !out.started) {
            out.started = true;
            out.start_tick = host.frame();
            out.presentation = battles.linked_presentation();
            out.resolution = battles.linked_resolution();
            out.land = battles.battle()->ground_height(0.0f, 0.0f) > -1e30f;
            const galactic::BattleRequest& r = battles.conflicts().active_request();
            out.forces = r.forces.size();
            if (r.forces.size() == 2) {
                out.def_start = r.forces[0].start.size();
                out.att_start = r.forces[1].start.size();
                out.att_reserve = r.forces[1].reserve.size();
            }
            out.request_linked = r.linked;
        }
    }
    for (int t = 0; t < 400 && battles.battle(); ++t) {  // run the battle out
        host.tick_once();
        if (post && out.post_kept < 0 && battles.battle() && host.tactical() && battles.battle()->frame() >= 1) {
            const objects::ObjectStore& ts = host.tactical()->primary();
            const objects::TypeIndex pt = d.types.resolve_name("S10_Trooper");
            out.post_kept = 0;
            for (const objects::ObjectHandle h : ts.all_objects())
                if (ts.alive(h) && ts.type(h) == pt && ts.owner(h) == battles.battle()->request().info.attacker) out.post_kept = 1;
        }
    }
    out.fought = battles.battles_fought();
    out.resolution_after = battles.linked_resolution();
    out.skipped_dialog = battles.linked_presentation().last_battle_skipped_dialog;
    for (const std::string& l : battles.log()) out.log += l + "; ";
    for (const auto& e : battles.conflicts().log()) out.conflicts += e.text(*gs) + "; ";
    return out;
}

// a1f t08: the S10 PROBE A story sequence (tools/probe_mod/s10_map_owners/s10_story.lua, the shipped
// Story_Campaign_Underworld.lua attack pattern) on the synthetic galaxy: a galactic story script of the human's plot
// assembles the Underworld squads in orbit of S10_C into a fleet, BlockOnCommand(fleet.Move_To(S10_B)) (the Empire's
// planet, an Empire squad on the surface), Sleep(1), fleet.Invade(). The block must last until the fleet is in orbit
// of S10_B (StorySession's order_finished); before a1f t08 it finished at once, Invade() ran in hyperspace and was
// dropped, so the fleet sat in orbit with either human. The squads are spawned at S10_C with Spawn_Unit (the probe's
// SpawnList; a1g t06: registered with the GalacticSim, so Assemble_Fleet flies them). Two runs: Underworld human (the
// probe's fallback: the original invaded at once) and Empire human with an Underworld without galactic AI (the probe's
// preferred variant, AI_Player_Control None: the original's fleet stayed in orbit and never invaded). a1g t05
// (A1_READINGS §31): fleet.Invade() is issued by the LOCAL player with its own ground units only, so with a human
// Empire nothing lands, silently.
struct ProbeInvadeRun {
    std::string conflicts, story;
    std::vector<std::string> lua;
    size_t land_queued = 0;  // most land conflicts queued at once
    uint32_t fought = 0;
    bool fleet_at_target = false;  // an Underworld fleet with ground units in orbit of S10_B at the end
};
ProbeInvadeRun run_probe_invade(const Data& d, const FileProvider& files, bool human_empire) {
    ProbeInvadeRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    setup.players[0].human = human_empire;
    setup.players[0].ai = !human_empire;
    setup.players[1].human = !human_empire;
    setup.players[1].ai = false;  // human, or the probe's AI_Player_Control None (no galactic AI)
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{human_empire ? 0 : 1, "Story_Plots_Probe.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& m : diag) std::printf("  story diag: %s\n", m.c_str());
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    for (uint64_t t = 0; t < 900; ++t) {
        host.tick_once();
        out.land_queued = std::max(out.land_queued, battles.conflicts().land_queue().size());
    }
    out.fought = battles.battles_fought();
    for (const uint32_t f : gs->fleets_at(1, 1))
        for (const objects::ObjectHandle m : gs->fleets()[f].contents)
            if (gs->store().alive(m) && gs->rules().types[gs->store().type(m)].kind == galactic::UnitKind::GroundUnit)
                out.fleet_at_target = true;
    if (host.story()) {
        out.lua = host.story()->lua_messages();
        out.story = host.story()->log_text("probe");
    }
    for (const auto& e : battles.conflicts().log()) out.conflicts += e.text(*gs) + "; ";
    return out;
}

void test_probe_invade(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_Probe.xml",
               "<Story_Mode_Plots><Lua_Script>Story_Probe</Lua_Script><Active_Plot>Story_Probe.xml</Active_Plot></Story_Mode_Plots>\n");
    std::string plot = "<Story>\n";
    for (const char* s : {"FLEET", "ARRIVED", "NOT_THERE", "INVADED"})
        plot += std::string(" <Event Name=\"P_") + s + "\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>" + s +
                "</Event_Param2></Event>\n";
    plot += "</Story>\n";
    write_file(x / "Story_Probe.xml", plot);
    // PGBase's BlockOnCommand / Sleep inlined as polling stages (no PG library in the synthetic data).
    write_file(d.root / "Data/Scripts/Story/Story_Probe.lua",
               "function Base_Definitions() ServiceRate = 0.1 stage = 0 end\n"
               "function main() while true do\n"
               "  if stage == 0 and GetCurrentTime() >= 1 then\n"
               "    uw = Find_Player('Underworld') tgt = FindPlanet('S10_B') src = FindPlanet('S10_C') units = {}\n"
               "    for _, t in ipairs({'S10_Squad', 'S10_Squad'}) do\n"
               "      for _, u in pairs(Spawn_Unit(Find_Object_Type(t), src, uw)) do table.insert(units, u) end end\n"
               "    fleet = Assemble_Fleet(units)\n"
               "    if fleet then Story_Event('FLEET') block = fleet.Move_To(tgt) stage = 1 else stage = 9 end\n"
               "  elseif stage == 1 and block.IsFinished() then\n"
               "    if fleet.Get_Planet_Location() == tgt then Story_Event('ARRIVED') else Story_Event('NOT_THERE') end\n"
               "    t1 = GetCurrentTime() stage = 2\n"
               "  elseif stage == 2 and GetCurrentTime() >= t1 + 1 then\n"
               "    fleet.Invade() Story_Event('INVADED') stage = 3\n"
               "  end\n"
               "  coroutine.yield(true) end end\n");
    DirectoryProvider files(path_to_utf8(d.root));
    for (const bool human_empire : {false, true}) {
        const ProbeInvadeRun r = run_probe_invade(d, files, human_empire);
        auto fired = [&r](const char* ev) { return r.story.find(std::string(" P_") + ev + " ") != std::string::npos; };
        std::printf("  human %s: fleet %d, block ended in orbit %d (elsewhere %d), invade issued %d, land conflicts queued %zu, "
                    "fought %u, fleet in orbit of S10_B at the end %d\n    %s\n",
                    human_empire ? "Empire (Underworld AI None)" : "Underworld", fired("FLEET") ? 1 : 0, fired("ARRIVED") ? 1 : 0,
                    fired("NOT_THERE") ? 1 : 0, fired("INVADED") ? 1 : 0, r.land_queued, r.fought, r.fleet_at_target ? 1 : 0,
                    r.conflicts.c_str());
        for (const std::string& m : r.lua) std::printf("    lua: %s\n", m.c_str());
        CHECK(r.lua.empty());
        CHECK(fired("FLEET") && fired("ARRIVED") && !fired("NOT_THERE") && fired("INVADED"));
        if (human_empire) {
            // The original (probe 2026-10-31, A1_READINGS §31): Invade() issued by the local Empire keeps no Underworld
            // unit, nothing lands, the fleet stays in orbit.
            CHECK(r.land_queued == 0 && r.fought == 0 && r.fleet_at_target);
        } else {
            CHECK(r.land_queued == 1 && r.fought == 1);
            CHECK(r.conflicts.find(" land S10_B Underworld vs Empire") != std::string::npos);
        }
    }
}

// a1h t04 (A1_READINGS §31.6): story reward INVADE_PLANET. A galactic story script brings two Underworld squads into orbit
// of S10_B (Empire) and raises GO; the plot's GO event has the reward INVADE_PLANET S10_B [company]. The reward picks the
// first ground unit of a fleet in orbit friendly to the plot's player (Underworld) that matches the company filter and
// issues ONE Invade event with that unit as the LOCAL player: with a human Underworld one squad lands (a land conflict
// with one unit); with a human Empire (an AI plot) the executor keeps no unit and nothing happens, silently; a filter
// that matches nothing logs the mismatch and issues nothing.
struct InvadeRewardRun {
    size_t land_queued = 0, landing_units = 0;  // most land conflicts queued at once; units of the first one seen
    std::vector<std::string> lua;
    std::string story;
};
InvadeRewardRun run_invade_reward(const Data& d, const FileProvider& files, bool human_empire) {
    InvadeRewardRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    setup.players[0].human = human_empire;
    setup.players[0].ai = false;
    setup.players[1].human = !human_empire;
    setup.players[1].ai = false;
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{1, "Story_Plots_Reward.xml"}};  // the Underworld's plot
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& m : diag) std::printf("  story diag: %s\n", m.c_str());
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    for (uint64_t t = 0; t < 900; ++t) {
        host.tick_once();
        const auto& q = battles.conflicts().land_queue();
        out.land_queued = std::max(out.land_queued, q.size());
        if (!q.empty() && out.landing_units == 0) out.landing_units = q[0].units.size();
    }
    if (host.story()) {
        out.lua = host.story()->lua_messages();
        out.story = host.story()->log_text("reward");
    }
    return out;
}

void test_invade_planet_reward(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_Reward.xml",
               "<Story_Mode_Plots><Lua_Script>Story_Reward</Lua_Script><Active_Plot>Story_Reward.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(d.root / "Data/Scripts/Story/Story_Reward.lua",
               "function Base_Definitions() ServiceRate = 0.1 stage = 0 end\n"
               "function main() while true do\n"
               "  if stage == 0 and GetCurrentTime() >= 1 then\n"
               "    uw = Find_Player('Underworld') units = {}\n"
               "    for _, t in ipairs({'S10_Squad', 'S10_Squad'}) do\n"
               "      for _, u in pairs(Spawn_Unit(Find_Object_Type(t), FindPlanet('S10_C'), uw)) do table.insert(units, u) end end\n"
               "    fleet = Assemble_Fleet(units) block = fleet.Move_To(FindPlanet('S10_B')) stage = 1\n"
               "  elseif stage == 1 and block.IsFinished() then Story_Event('GO') stage = 2 end\n"
               "  coroutine.yield(true) end end\n");
    struct Case {
        bool human_empire;
        const char* company;
        size_t land_queued, landing_units;
        bool mismatch_logged;
    };
    for (const Case& c : {Case{false, "S10_SQUAD", 1, 1, false}, Case{false, "", 1, 1, false}, Case{true, "S10_SQUAD", 0, 0, false},
                          Case{false, "S10_Squad", 0, 0, true}}) {
        write_file(x / "Story_Reward.xml",
                   std::string("<Story>\n <Event Name=\"P_GO\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>GO</Event_Param2>"
                               "<Reward_Type>INVADE_PLANET</Reward_Type><Reward_Param1>S10_B</Reward_Param1>") +
                       (*c.company ? std::string("<Reward_Param2>") + c.company + "</Reward_Param2>" : std::string()) + "</Event>\n</Story>\n");
        DirectoryProvider files(path_to_utf8(d.root));  // after the plot file is written
        const InvadeRewardRun r = run_invade_reward(d, files, c.human_empire);
        bool mismatch = false;
        for (const std::string& m : r.lua) mismatch = mismatch || m.find("INVADE PLANET - S10_SQUAD does not match S10_Squad") != std::string::npos;
        std::printf("  INVADE_PLANET human %s, company '%s': land conflicts %zu with %zu unit(s), mismatch logged %d\n",
                    c.human_empire ? "Empire" : "Underworld", c.company, r.land_queued, r.landing_units, mismatch ? 1 : 0);
        CHECK(r.story.find(" P_GO ") != std::string::npos);
        CHECK(r.land_queued == c.land_queued && r.landing_units == c.landing_units && mismatch == c.mismatch_logged);
        CHECK(r.lua.size() == (c.mismatch_logged ? 2u : 0u));  // both squads mismatch
    }
}

void test_linked_battle(const Data& d) {
    for (int c = 0; c < 3; ++c) {
        const bool space = c == 0, sandbox = c == 2;
        const LinkedRun r = run_linked(d, space, sandbox);
        std::printf("  linked %s%s: reward tick %" PRIu64 ", battle tick %" PRIu64 " (%s), flag after a tick %d, forces %zu (defender start %zu, "
                    "attacker start %zu reserve %zu), fought %u\n    battles: %s\n    conflicts: %s\n",
                    space ? "space" : "land", sandbox ? " sandbox" : "", r.reward_tick, r.start_tick, r.land ? "land" : "space",
                    r.flag_after_tick ? 1 : 0, r.forces, r.def_start, r.att_start, r.att_reserve, r.fought, r.log.c_str(), r.conflicts.c_str());
        CHECK(r.started && r.land == !space);
        // §2.9.3 (Param13 0): started by the reward itself. [port] the reward is recorded in the story service, which runs
        // after the battle hook, so the battle starts at the next hook (the retail load also spans frames).
        CHECK(r.reward_tick != 0 && r.start_tick == r.reward_tick + 1);
        CHECK(!r.flag_after_tick);
        CHECK(r.forces == 2 && r.att_start == 0 && r.att_reserve == 0);
        CHECK(r.def_start == (sandbox ? 1u : 0u));  // not sandbox: no galactic unit; sandbox: the human defender's squad
        CHECK(r.fought == 1 && r.log.find("LINK_TACTICAL S10_B") != std::string::npos && r.log.find(" started") != std::string::npos);
        CHECK(r.conflicts.find(space ? " space S10_B " : " land S10_B ") != std::string::npos);
        // story t07: defaults (P9 1, P12 DESTROY_AI, P13 on but answered at once headless); the dialog skip is taken at the end.
        CHECK(r.resolution == galactic::LinkedResolution::DestroyAi && r.resolution_after == galactic::LinkedResolution::None);
        CHECK(r.presentation.arrival == 1 && r.presentation.arrival_cinematic && !r.presentation.hyperspace_hold);
        CHECK(!r.presentation.presentation_a && !r.presentation.presentation_b && r.skipped_dialog && r.banner_tick == 0);
    }
}

// story t07 (BT §2.9.1 / §2.9.3): LINK_TACTICAL Param9-14. (1) P9 2 / P10 1 / P11 1 / P12 RETREAT_PLAYER / P13 0 / P14 0: the
// arrival hold and presentation flags are set at the start, GOM+0x61c = RETREAT_PLAYER during the battle and cleared by its
// end; a bad P12 keyword is logged and falls back to DESTROY_AI. (2) The P13 banner with the battle dialog (Pending): the
// record waits with the galaxy paused until ResumeGame (Begin), then starts at the next hook; before it, no battle.
void test_linked_params(const Data& d) {
    const auto p = [](int n, const char* v) { return "<Reward_Param" + std::to_string(n) + ">" + v + "</Reward_Param" + std::to_string(n) + ">"; };
    const LinkedRun a = run_linked(d, true, false, p(9, "2") + p(10, "1") + p(11, "1") + p(12, "RETREAT_PLAYER") + p(13, "0") + p(14, "0"));
    std::printf("  P9-P14: started tick %" PRIu64 " (reward %" PRIu64 "), arrival %d hold %d A %d letterbox %d B %d, resolution %d -> %d, "
                "dialog skipped %d\n",
                a.start_tick, a.reward_tick, a.presentation.arrival, a.presentation.hyperspace_hold ? 1 : 0, a.presentation.presentation_a ? 1 : 0,
                a.presentation.letterbox ? 1 : 0, a.presentation.presentation_b ? 1 : 0, static_cast<int>(a.resolution),
                static_cast<int>(a.resolution_after), a.skipped_dialog ? 1 : 0);
    CHECK(a.started && a.start_tick == a.reward_tick + 1 && a.fought == 1);
    CHECK(a.presentation.arrival == 2 && a.presentation.arrival_cinematic && a.presentation.hyperspace_hold);
    CHECK(a.presentation.presentation_a && a.presentation.letterbox && a.presentation.presentation_b);
    CHECK(a.resolution == galactic::LinkedResolution::RetreatPlayer && a.resolution_after == galactic::LinkedResolution::None);
    CHECK(a.skipped_dialog);
    const LinkedRun u = run_linked(d, false, false, p(9, "0") + p(12, "RETREAT_EVERYONE"));
    CHECK(u.started && u.resolution == galactic::LinkedResolution::DestroyAi && !u.presentation.arrival_cinematic && !u.presentation.hyperspace_hold);
    CHECK(u.log.find("Unknown resolution RETREAT_EVERYONE") != std::string::npos);
    const LinkedRun b = run_linked(d, true, false, {}, true);  // P13 default on, the dialog answers Pending
    std::printf("  P13 banner: reward tick %" PRIu64 ", banner %" PRIu64 ", Begin %" PRIu64 ", battle %" PRIu64 ", paused %d\n    battles: %s\n",
                b.reward_tick, b.banner_tick, b.begin_tick, b.start_tick, b.paused_in_banner ? 1 : 0, b.log.c_str());
    CHECK(b.banner_tick == b.reward_tick + 1 && b.paused_in_banner && b.begin_tick == b.banner_tick + 21);
    CHECK(b.started && b.start_tick == b.begin_tick && b.fought == 1);  // Begin at the command phase, pushed at that tick's hook
    CHECK(b.log.find("LINK_TACTICAL S10_B space banner") != std::string::npos && !b.flag_after_tick);
}

// story t10 (REMAINING_GAPS §3.1 item 4, [M] global 0x140b155d8; BT §3.1a item 4): a linked battle keeps the attacker's
// Is_Branched_Map_Discardable map objects (Kessel UM01's jailed UMP_TYBER_ZANN): an Underworld S10_Trooper placed on the
// map, which a normal invade drops (as the S4_Post of the map owners test: owner = attacker), is in the linked land battle.
void test_linked_map_objects(const Data& d) {
    const LinkedRun r = run_linked(d, false, false, {}, false, true);
    std::printf("  linked land: attacker's branched-discardable trooper kept %d, request linked %d\n", r.post_kept, r.request_linked ? 1 : 0);
    CHECK(r.started && r.request_linked && r.post_kept == 1);
}

// story t09 / t12: a linked space battle (LINK_TACTICAL P7 = tactical plots, P9 = 2; no galactic units, so nothing ever
// dies) whose tactical plot holds the victory monitor at its start (ENABLE_VICTORY 0), names an unknown faction at 1 s
// (logged, nothing) and ends with VICTORY Underworld (Param2 1: 1 s, silent) at 12 s, past the 300-frame time limit, which
// does not run while the monitor is held. The tactical script calls Resume_Hyperspace_In after 20 services: the P9 = 2
// reinforcement hold is up at the start (TacticalBattle::arrival_held) and cleared by the script.
struct StoryRewardRun {
    bool started = false, held_at_start = false, hold_at_start = false, resumed = false, ended = false;
    int32_t hold = -1;
    uint32_t resume_frame = 0, end_frame = 0, win_frame = 0;
    uint8_t declared_type = 0;
    int32_t declared_player = -2;
    bool silent = false;
    std::string summary, log;
};
StoryRewardRun run_story_rewards(const Data& d) {
    StoryRewardRun out;
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_VLink.xml", "<Story_Mode_Plots><Active_Plot>Story_VLink.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_VLink.xml",
               "<Story>\n"
               " <Event Name=\"Link\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>1</Event_Param1>"
               "<Reward_Type>LINK_TACTICAL</Reward_Type><Reward_Param1>S10_B</Reward_Param1><Reward_Param2>SPACE</Reward_Param2>"
               "<Reward_Param3>Underworld</Reward_Param3><Reward_Param4>S10_Space_B</Reward_Param4><Reward_Param5>Empire</Reward_Param5>"
               "<Reward_Param6></Reward_Param6><Reward_Param7>Story_Plots_VTac.xml</Reward_Param7><Reward_Param8>0</Reward_Param8>"
               "<Reward_Param9>2</Reward_Param9></Event>\n"
               "</Story>\n");
    write_file(x / "Story_Plots_VTac.xml",
               "<Story_Mode_Plots><Lua_Script>Story_VTac</Lua_Script><Active_Plot>Story_VTac.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_VTac.xml",
               "<Story>\n"
               " <Event Name=\"Hold\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1>"
               "<Reward_Type>ENABLE_VICTORY</Reward_Type><Reward_Param1>0</Reward_Param1></Event>\n"
               " <Event Name=\"Bad\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>1</Event_Param1>"
               "<Reward_Type>VICTORY</Reward_Type><Reward_Param1>Nobody</Reward_Param1></Event>\n"
               " <Event Name=\"Win\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>12</Event_Param1>"
               "<Reward_Type>VICTORY</Reward_Type><Reward_Param1>Underworld</Reward_Param1><Reward_Param2>1</Reward_Param2></Event>\n"
               "</Story>\n");
    write_file(d.root / "Data/Scripts/Story/Story_VTac.lua",
               "function Base_Definitions() ServiceRate = 0.1 n = 0 end\n"
               "function main() while true do n = n + 1 if n == 20 then Resume_Hyperspace_In() end coroutine.yield(true) end end\n");
    DirectoryProvider files(path_to_utf8(d.root));
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{0, "Story_Plots_VLink.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    uint32_t last_frame = 0;
    for (uint64_t t = 0; t < 1500; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        if (!b) {
            if (!out.started) continue;
            out.ended = true;
            out.end_frame = last_frame + 1;
            out.summary = battles.log().empty() ? std::string() : battles.log().back();
            break;
        }
        last_frame = b->frame();
        if (!out.started) {
            out.started = true;
            out.held_at_start = b->arrival_held();
            out.hold_at_start = battles.linked_presentation().hyperspace_hold;
        }
        if (out.hold < 0 && b->victory_hold() > 0) out.hold = b->victory_hold();
        if (!out.resumed && !battles.linked_presentation().hyperspace_hold) {
            out.resumed = true;
            out.resume_frame = b->frame();
            CHECK(!b->arrival_held());
        }
        if (out.win_frame == 0 && b->victory_declared()) {
            out.win_frame = b->frame();
            out.declared_type = b->victory_declared_type();
            out.declared_player = b->victory_declared_player();
            out.silent = b->victory_declared_silent();
        }
    }
    for (const std::string& l : battles.log()) out.log += l + "; ";
    if (host.story())
        for (const std::string& m : host.story()->lua_messages()) out.log += "lua: " + m + "; ";
    return out;
}

// story t12: the P9 = 2 hold in a normal space battle (the arrival source installed by hand): three extra Underworld
// corvettes stay in the pool (unit cap 6); while held the Reinforce order is refused NotAllowed and the stand-in deploy
// waits even for an empty side; released, the order goes through the usual rules (cap: QueueFull, or deployed).
void test_arrival_hold(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, false);
    for (int i = 0; i < 3; ++i) setup.forces.push_back({1, 2, type_of(d, "S10_Corvette")});
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool held = true, checked = false;
    uint32_t deployed_held = 0, frames_held = 0;
    commands::RejectReason in_hold = commands::RejectReason::None, after = commands::RejectReason::NotAllowed;
    size_t pool = 0;
    for (uint64_t t = 0; t < 3000 && !checked; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        if (b->frame() == 1) {
            b->set_arrival_hold([&held] { return held; });
            deployed_held = b->stats().reinforcements;
            const auto r = b->reserves(1);
            pool = r.size();
            if (!r.empty()) in_hold = b->order_reinforce(*tm, 1, r[0].galactic_id, 0.0f, 0.0f, host.random().synced());
        }
        if (b->frame() >= 1 && held) {
            ++frames_held;
            CHECK(b->stats().reinforcements == deployed_held);
        }
        if (b->frame() == 500 || (b->finished() && held)) {
            held = false;
            const auto r = b->reserves(1);
            if (!r.empty()) after = b->order_reinforce(*tm, 1, r[0].galactic_id, 0.0f, 0.0f, host.random().synced());
            checked = true;
        }
    }
    std::printf("  arrival hold: pool %zu, Reinforce held %d / released %d, %u frames held with %u deployments\n", pool,
                static_cast<int>(in_hold), static_cast<int>(after), frames_held, deployed_held);
    CHECK(pool > 0 && in_hold == commands::RejectReason::NotAllowed && after != commands::RejectReason::NotAllowed);
}

void test_story_victory_rewards(const Data& d) {
    const StoryRewardRun r = run_story_rewards(d);
    std::printf("  story rewards: held %d / %d at start, monitor hold %d, resumed at %u, declared 0x%x for %d (silent %d) at %u, "
                "end %u\n    %s\n    %s\n",
                r.held_at_start ? 1 : 0, r.hold_at_start ? 1 : 0, r.hold, r.resume_frame, static_cast<unsigned>(r.declared_type),
                r.declared_player, r.silent ? 1 : 0, r.win_frame, r.end_frame, r.summary.c_str(), r.log.c_str());
    CHECK(r.started && r.held_at_start && r.hold_at_start && r.hold == 1);
    CHECK(r.resumed && r.resume_frame > 30 && r.resume_frame < 120);
    CHECK(r.declared_type == galactic::kVictoryStory && r.declared_player == 1 && r.silent);
    CHECK(r.win_frame > 300 && r.ended && r.end_frame == r.win_frame + 30);
    CHECK(r.summary.find("winner Underworld, victory 16, retreated Empire") != std::string::npos);
    CHECK(r.log.find("Unable to find faction Nobody to force victory") != std::string::npos);
    test_arrival_hold(d);
}

// story t11: the galactic plot set's ENABLE_VICTORY / VICTORY act on the galactic monitor (GalacticSim::victory_hold,
// galactic::check_victory): two holds at 0 s, one release and a Param1-less no-op at 1 s, an unknown faction at 2 s
// (logged), VICTORY Underworld at 3 s: the hold is released and the campaign ends for the Underworld 210 frames later. The
// human Empire's Credits_Accrued 0 would win at its next credit change without the hold (q6 t21: tested in add_credits);
// under the hold the test is deferred, and VICTORY releases every level without replaying it. A prereq-less ELAPSED 0
// fires at its second T9 (tick 2, StoryModel::tick_timer, A2I_T01_FRAME_ORDER.md §6).
void test_galactic_story_victory(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_GVic.xml", "<Story_Mode_Plots><Active_Plot>Story_GVic.xml</Active_Plot></Story_Mode_Plots>\n");
    const auto ev = [](const char* name, const char* sec, const char* reward, const char* p1) {
        std::string s = std::string(" <Event Name=\"") + name + "\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>" + sec +
                        "</Event_Param1><Reward_Type>" + reward + "</Reward_Type>";
        if (p1) s += std::string("<Reward_Param1>") + p1 + "</Reward_Param1>";
        return s + "</Event>\n";
    };
    write_file(x / "Story_GVic.xml", "<Story>\n" + ev("H1", "0", "ENABLE_VICTORY", "0") + ev("H2", "0", "ENABLE_VICTORY", "0") +
                                         ev("R1", "1", "ENABLE_VICTORY", "1") + ev("N1", "1", "ENABLE_VICTORY", nullptr) +
                                         ev("Bad", "2", "VICTORY", "Nobody") + ev("Win", "3", "VICTORY", "Underworld") + "</Story>\n");
    DirectoryProvider files(path_to_utf8(d.root));
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    galactic::VictoryRules rules;
    rules.human.conditions = {galactic::VictoryCondition::CreditsAccrued};
    gs->set_victory_rules(rules);
    CHECK(!galactic::check_victory(*gs).finished());  // q6 t21: nothing is polled; no credit change yet
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{0, "Story_Plots_GVic.xml"}};
    CHECK(host.start_story(st, nullptr));
    int32_t max_hold = 0, hold_after_release = -1;
    int32_t victory_frame = -1, end_frame = -1;
    bool won_early = false, deferred = false;
    for (uint64_t t = 0; t < 600 && end_frame < 0; ++t) {
        host.tick_once();
        max_hold = std::max(max_hold, gs->victory_hold());
        if (max_hold == 2 && hold_after_release < 0 && gs->victory_hold() == 1) hold_after_release = 1;
        if (max_hold == 0) {
            CHECK(t < 2);  // the 0 s holds are up by tick 2
            continue;
        }
        if (!deferred) {  // a credit change under the hold: Credits_Accrued is queued, nothing is declared
            gs->add_credits(0, 0.0f, false);
            deferred = gs->victory_deferred().size() == 1 && gs->victory_claims().empty();
            CHECK(deferred);
        }
        const galactic::VictoryResult v = galactic::check_victory(*gs);
        if (victory_frame < 0 && !gs->victory_claims().empty()) victory_frame = gs->victory_claims()[0].frame;
        if (v.finished()) {
            if (v.winner != 1 || v.how != galactic::VictoryCondition::StoryTriggered) won_early = true;
            end_frame = gs->frame();
        }
    }
    std::string lua;
    if (host.story())
        for (const std::string& m : host.story()->lua_messages()) lua += m + "; ";
    std::printf("  galactic story victory: max hold %d, after release %d, claim at %d, campaign end at %d (hold %d)\n    %s\n", max_hold,
                hold_after_release, victory_frame, end_frame, gs->victory_hold(), lua.c_str());
    CHECK(max_hold == 2 && hold_after_release == 1 && !won_early && gs->victory_hold() == 0);
    CHECK(victory_frame > 0 && end_frame == victory_frame + 210 && gs->victory_claims().size() == 1 &&
          gs->victory_claims()[0].type == galactic::GalacticSim::kVictoryStoryClaim && gs->victory_claims()[0].player == 1);
    CHECK(galactic::victory_outcome(*gs, 0) == -1 && galactic::victory_outcome(*gs, 1) == 1);
    CHECK(gs->victory_deferred().size() == 1);  // VICTORY released the holds without the replay
    CHECK(lua.find("Unable to find faction Nobody to force victory") != std::string::npos);
}

// a1j t05 (A1_READINGS §31.7, BT §1.4 step 2): 0x2be640 skips the space/land state machines while the story block holds
// (a linked battle record waits and the local player is in a queued conflict); the paused query 0x4b2360 freezes the
// galactic frame. The record is raised by hand here (no story), the Underworld fleet flies C -> B against the human
// Empire: its conflict stays queued, nothing is voted, fought or auto-resolved and the frame stands still until the flag
// is cleared; then the conflict runs as usual.
void test_story_block_host(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (uf.empty()) return;
    CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    uint64_t t = 0;
    // [port] the host releases a flag that has no conflict to wait for at its battle hook, so it is raised again before
    // every tick until the fleet's conflict holds it.
    for (; t < 2000 && !gs->story_block(); ++t) {
        if (!gs->linked_battle_pending()) gs->queue_linked_battle();
        host.tick_once();
    }
    CHECK(gs->story_block() && battles.conflicts().space_queue().size() == 1);
    const int32_t frozen = gs->frame();
    for (int i = 0; i < 200; ++i) host.tick_once();
    CHECK(gs->frame() == frozen && gs->paused());
    CHECK(battles.conflicts().state() == galactic::ConflictState::Idle && battles.conflicts().space_queue().size() == 1);
    CHECK(!battles.battle() && battles.battles_fought() == 0 && battles.conflicts().log().empty());
    gs->clear_linked_battle();
    bool started = false;
    for (int i = 0; i < 600 && !started; ++i) {
        host.tick_once();
        started = battles.battle() != nullptr;
    }
    CHECK(started && gs->frame() > frozen);
    std::printf("  story block: queued at tick %" PRIu64 ", frame %d frozen for 200 ticks, battle started after the flag cleared\n",
                t, frozen);
}

void test_story_battle(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_S10.xml", "<Story_Mode_Plots><Active_Plot>Story_S10.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S10.xml",
               "<Story>\n"
               " <Event Name=\"Go_Space\"><Event_Type>STORY_SPACE_TACTICAL</Event_Type><Event_Param1>Story_Plots_S10_Tac.xml</Event_Param1></Event>\n"
               " <Event Name=\"Won_Space\"><Event_Type>STORY_WIN_BATTLES</Event_Type><Event_Param1>1</Event_Param1><Event_Param2>SPACE</Event_Param2></Event>\n"
               " <Event Name=\"Lost_Space\"><Event_Type>STORY_LOSE_BATTLES</Event_Type><Event_Param1>1</Event_Param1><Event_Param2>SPACE</Event_Param2></Event>\n"
               "</Story>\n");
    write_file(x / "Story_Plots_S10_Tac.xml",
               "<Story_Mode_Plots><Lua_Script>Story_S10_Tac</Lua_Script><Active_Plot>Story_S10_Tac.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S10_Tac.xml",
               "<Story>\n"
               // L1/S4 wave 2b (SCRIPT_GAPS.md §4.2 / §4.5): VICTORY names one faction (empty never fires);
               // TACTICAL_DESTROY needs the dead type in Param1.
               " <Event Name=\"Victory_E\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Empire</Event_Param1></Event>\n"
               " <Event Name=\"Victory_U\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Underworld</Event_Param1></Event>\n"
               " <Event Name=\"Victory_Never\"><Event_Type>STORY_VICTORY</Event_Type></Event>\n"
               " <Event Name=\"First_Kill\"><Event_Type>STORY_TACTICAL_DESTROY</Event_Type>"
               "<Event_Param1>S10_Frigate, S10_Corvette, S10_Star_Base_1</Event_Param1></Event>\n"
               " <Event Name=\"Script_Saw\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>TAC_SCRIPT</Event_Param2></Event>\n"
               // fog follow-up (FOG_OF_WAR.md §6): the map centre is fogged for the Empire (no REVEAL units in this
               // data) until the script's FogOfWar.Reveal; that fires Fog_Point, whose ENABLE_FOW 0 reward switches
               // the battle's fog off. Units without TEAM / HIDE_WHEN_FOGGED are never hidden: Fog_Object fires at once.
               " <Event Name=\"Script_Fog\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>FOG_CELLS</Event_Param2></Event>\n"
               " <Event Name=\"Fog_Point\"><Event_Type>STORY_FOG_POSITION_REVEAL</Event_Type><Event_Param1>0, 0, 0</Event_Param1>"
               "<Reward_Type>ENABLE_FOW</Reward_Type><Reward_Param1>0</Reward_Param1></Event>\n"
               " <Event Name=\"Fog_Object\"><Event_Type>STORY_FOG_OBJECT_REVEAL</Event_Type><Event_Param1>S10_Corvette</Event_Param1></Event>\n"
               "</Story>\n");
    write_file(d.root / "Data/Scripts/Story/Story_S10_Tac.lua",
               // q6: main's first resume is at load (STORY.md §8 item 9), so the reveal waits a few services: the
               // battle's fog must be seen on before ENABLE_FOW 0 switches it off.
               "function Base_Definitions() ServiceRate = 0.1 done = false revealed = false waits = 0 end\n"
               "function main() while true do\n"
               "  if not done then local f = Find_First_Object('S10_Frigate')\n"
               "    if f then local h = f.Get_Hull() if h > 0 and h <= 1 then Story_Event('TAC_SCRIPT') end\n"
               "      f.Activate_Ability('POWER_TO_WEAPONS', true) done = true end\n"
               "  elseif waits < 3 then waits = waits + 1\n"
               "  elseif not revealed then local f = Find_First_Object('S10_Frigate') local p = Find_Player('Empire')\n"
               "    if f and p then cells = FogOfWar.Reveal(p, f, 20000) revealed = true\n"
               "      if cells then Story_Event('FOG_CELLS') end end end\n"
               "  coroutine.yield(true) end end\n");
    DirectoryProvider files(path_to_utf8(d.root));
    const StoryRun a = run_story_battle(d, files, 0);
    const StoryRun b = run_story_battle(d, files, 4);
    for (const auto& m : a.lua) std::printf("  lua: %s\n", m.c_str());
    auto fired = [&a](const char* ev) { return a.log.find(std::string(" ") + ev + " ") != std::string::npos; };
    std::printf("  story battle: %zu log bytes, Lua activations %u, identical for 0 / 4 workers: %s\n", a.log.size(), a.activations,
                a.log == b.log ? "yes" : "NO");
    CHECK(a.lua.empty() && a.activations > 0);
    // L1/S4 wave 2b feeds: movement transitions (UNIT_ARRIVED / FULL_STOP) and reserve deployments (REINFORCE).
    auto count = [&a](const char* k) {
        size_t n = 0;
        for (size_t p = a.log.find(k); p != std::string::npos; p = a.log.find(k, p + 1)) ++n;
        return n;
    };
    std::printf("  wave 2b feeds: TacUnitArrived %zu, TacFullStop %zu, TacReinforced %zu, TacBaseDestroyed %zu\n", count("TacUnitArrived"),
                count("TacFullStop"), count("TacReinforced"), count("TacBaseDestroyed"));
    CHECK(count("TacUnitArrived") + count("TacFullStop") > 0);
    CHECK(fired("Go_Space") && (fired("Victory_E") != fired("Victory_U")) && !fired("Victory_Never") && fired("First_Kill") &&
          fired("Script_Saw") && (fired("Won_Space") || fired("Lost_Space")));
    CHECK(a.log.find("BattleStarted") != std::string::npos && a.log.find("BattleEnded") != std::string::npos &&
          a.log.find("TacDestroyed") != std::string::npos);
    CHECK(!a.log.empty() && a.log == b.log);
#ifdef ALAMO_LUA_HAVE_FOG
    // fog follow-up: FogOfWar.Reveal from the tactical script -> STORY_FOG_POSITION_REVEAL -> ENABLE_FOW 0.
    std::printf("  fog: Script_Fog %d, Fog_Point %d, Fog_Object %d, FOW active seen %d / off seen %d\n", fired("Script_Fog") ? 1 : 0,
                fired("Fog_Point") ? 1 : 0, fired("Fog_Object") ? 1 : 0, a.fog_on_seen ? 1 : 0, a.fog_off_seen ? 1 : 0);
    CHECK(fired("Script_Fog") && fired("Fog_Point") && fired("Fog_Object"));
    CHECK(a.fog_on_seen && a.fog_off_seen);
#endif
}

// --- S4 (owner: S4 agent): the remaining S4 event sources in story land battles (s4_types data; docs/re/
// L1_S4_REMAINING.md §3.5, §4). The human Empire defends S10_B (its galactic plot's STORY_LAND_TACTICAL loads
// Story_Plots_S4_Tac.xml, so the Empire is the tactical plot player = the "local player"); commands go through SimHost's
// order hook (a stand-in for apps/game's router):
//   battle-load screen: STORY_LOAD_TACTICAL_MAP (galactic, planet S10_B, land) -> SET_TACTICAL_MAP S4_Land_Alt replaces
//                    the battle's map (the loader records the name it was asked for)
//   battle frame 5   Select the Empire troopers + MoveToPosition onto the capture post -> STORY_COMMAND_UNIT
//   battle frame 40  Select one trooper + Escort another                              -> STORY_GUARD_UNIT
//   battle frame 20  Garrison (52) of the last trooper (placed nearest the bunker; s78n t26.3: the first one, at the back
//                    of the column, now turns in place before its move - the corner pass's corridor test - and reached
//                    the bunker too late for the post's capture window; s78n t26.4: frame 20, not 60 - stopped beside
//                    the post facing away, the trooper turns on the move (CloseEnoughAngleForMoveStart 180, MOVEMENT
//                    §3.5.2 A / C.7) in a loop, and from frame 60 it took the bunker 10 frames before the Underworld came
//                    within 300 of the post, too late for its 30-frame capture) into the neutral bunker (also a capture
//                    point) -> approach, 30-frame
//                    entering, STORY_GARRISON_UNIT; rule (b) makes the bunker the Empire's -> STORY_CAPTURE_STRUCTURE
//   60 frames inside: Garrison (52) with garrison = 0x3FFFFF (leave)
//   battle frame 80  battles.change_owner on the Underworld hero: STORY_CAPTURE_HERO still never fires (§4.3)
//   the troopers on the post capture it (1 s, countdown 0)                -> STORY_CAPTURE_STRUCTURE (Param2 Empire)
// The corruption runs make the Underworld human (a mission needs a non-AI corrupter) and start its Kidnapping corruption
// at S10_B (CampaignBattles::start_corruption with its orbit ground units, or the hero alone): a land battle with
// Story_Plots_S4_Mission.xml -> STORY_CORRUPTION_TACTICAL_COMPLETE / _FAILED by the winner; the Piracy option of S10_A
// (no mission) then applies at once. In the plain run the AI Underworld's Piracy at S10_A applies directly too.
struct S4Run {
    std::string log;
    std::vector<std::string> lua, battles, maps;
    CrcLog crc;
    bool garrison_flag = false, left = false, readded = false, bunker_taken = false, post_taken = false;
    bool fire_query = false, can_query = false, hero_changed = false, contained = false;
    // a1f t07 (attack_sensor runs): the neutral sensor is a combat unit and a capture point, untouched by the target scan;
    // S7 t30: the attack order on it is refused (relation 2), so it stays alive and a capture point.
    bool sensor_combat = false, sensor_point = false, sensor_untouched = false, sensor_killed = false, sensor_point_removed = false;
    int attack_order = -1;
    float sensor_health = -1.0f;  // last health fraction seen
    uint32_t garrisoned_frames = 0;
    int mission = -1, piracy = -1;
    int32_t winner = -2;
    size_t mission_units = 99;          // story t08: galactic units in the mission battle's request (start + reserve)
    uint64_t mission_tick = 0, battle_tick = 0;
    int32_t corruption_b = -2, corruption_a = -2;
    float credits_before = -1.0f, credits_after = -1.0f;
};

terrain::MapData s4_map(bool sensor = false) {
    terrain::MapData m = land_map();
    auto place = [&m](const char* type, float x, float y, uint32_t owner, float yaw = 0.0f) {
        terrain::ObjectPlacement p;
        p.type_crc = name_crc(type);
        p.owner = owner;
        p.position[0] = x;
        p.position[1] = y;
        p.rotation_deg[2] = yaw;
        m.objects.push_back(p);
    };
    // REMAINING_GAPS §3.3: placements keep their map owner (player id): the post and the bunker belong to the neutral
    // player (2) of this galaxy.
    // Yaw 90 = facing +x (a placement's facing is Rz(yaw) x the model nose -Y): the facings this scenario was laid out
    // with (defenders' row along y, the bunker's door side).
    place("Defending Forces Position", 900.0f, 660.0f, 0, 90.0f);  // the defenders' anchor (marker, no type needed)
    place("S4_Post", 900.0f, 660.0f, 2, 90.0f);                    // on the defender anchor
    place("S4_Bunker", 900.0f, 700.0f, 2, 90.0f);                  // among the defenders' first row
    if (sensor) place("S4_Sensor", 940.0f, 660.0f, 2, 90.0f);      // a1f t07: next to the defenders, neutral
    return m;
}

// corruption: 0 none, 1 mission, 2 hero alone, 3 (story t08) mission with no Underworld unit near S10_B; attack_sensor (a1f t07): the map gets a neutral S4_Sensor and the Empire
// troopers only attack it (frame 60), no other scripted orders.
S4Run run_s4_battle(const Data& d, const FileProvider& files, int32_t workers, int corruption, bool attack_sensor = false) {
    using R = commands::RejectReason;
    using T = commands::EventType;
    S4Run out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    cfg.story_crc = true;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    setup.forces.push_back({1, 2, type_of(d, "S4_Hero")});  // an Underworld named hero rides along
    if (corruption) {  // a corruption mission needs a non-AI corrupting player (§4.5)
        setup.players[1].human = true;
        setup.players[1].ai = false;
    }
    galactic::GalacticSim* gs = host.start_galactic(d.rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return out;
    StoryStart st;
    st.files = &files;
    st.plot_sets = {{0, "Story_Plots_S4.xml"}};
    std::vector<std::string> diag;
    CHECK(host.start_story(st, &diag));
    for (const auto& m : diag) std::printf("  story diag: %s\n", m.c_str());
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    // story t08 + t06: a corruption mission brings no galactic unit and the victory monitor tests only when a
    // victory-relevant object leaves a player, so the synthetic mission (no plot VICTORY) ends at the time limit, where
    // the defender holds [port]; kept short so the galaxy is back before the piracy step at tick 2700.
    opt.battle.max_frames = corruption ? 1200 : 6000;
    opt.battle.skip_human_units = corruption == 0;  // the corruption runs let the stand-in fight for both sides
    opt.battle.land_capture_countdown = 0.0f;
    std::vector<std::string>* maps = &out.maps;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [maps, attack_sensor](const std::string& name, terrain::MapData& m) {
                                maps->push_back(name);
                                if (name != "S10_Land_B" && name != "S4_Land_Alt") return false;
                                m = s4_map(attack_sensor);
                                return true;
                            },
                            opt);
    CHECK(battles.attach());
    // apps/game battle_orders.cpp stand-in: MoveToPosition / Escort of the issuer's selection into the battle.
    host.set_order_hook([&battles](SimHost& h, uint8_t p, const commands::EventData& ev, const std::vector<objects::ObjectId>& sel) {
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = h.tactical();
        if (!b || !tm) return R::Unhandled;
        SimRng& rng = h.random().synced();
        if (ev.type() == T::MoveToPosition) return b->order_move(*tm, p, sel, ev.f("x"), ev.f("y"), rng);
        if (ev.type() == T::Escort) return b->order_move_to_object(*tm, p, sel, static_cast<objects::ObjectId>(ev.u("target")), rng);
        return R::Unhandled;
    });
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == R::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    // GRC 1.1: a human's hop needs the target's raw revealed byte - set by the galactic pre-step of the first frame
    // (auto-reveal spread from the held planets).
    if (gs->players()[1].human) {  // the hop gate reads the local player's (0) bytes; S10_B is held by player 0
        for (int i = 0; i < 4 && !gs->planet_revealed_raw(1); ++i) host.tick_once();
        CHECK(gs->planet_revealed_raw(1));
    }
    if (!uf.empty() && corruption != 3) CHECK(gs->move_fleet(1, uf[0], 1) == R::None);
    bool invaded = false;
    std::vector<objects::ObjectId> troopers;
    objects::ObjectId bunker = objects::kNoObjectId, post = objects::kNoObjectId, sensor = objects::kNoObjectId;
    const objects::TypeIndex trooper_t = d.types.resolve_name("S10_Trooper");
    for (uint64_t t = 0; t < 3000; ++t) {
        host.tick_once();
        // GRC 4.3: early enough for the 5 s gain (150 galactic frames) to complete and raise CORRUPTION_CHANGED.
        if (t >= 2700 && host.modes().depth() == 1 && out.piracy < 0) {
            out.corruption_b = gs->planets()[1].corruption;
            // S10_A's Piracy option has no mission: applied directly (AI or human corrupter) as a 5 s timed gain (GRC 4.3).
            out.piracy = static_cast<int>(battles.start_corruption(0, 1, 1));
            out.corruption_a = gs->corruption_transition_active(0) ? gs->effective_corruption_type(0) : int8_t{-2};
        }
        if (corruption == 3 && !invaded && host.modes().depth() == 1) {  // story t08: the linked start needs no unit in orbit
            CHECK(gs->fleets_at(1, 1).empty());
            out.credits_before = gs->credits(1);
            out.mission = static_cast<int>(battles.start_corruption(1, 1, 2));
            out.credits_after = gs->credits(1);
            out.mission_tick = host.frame();
            invaded = true;
        }
        if (!out.battle_tick && battles.battle()) {
            out.battle_tick = host.frame();
            out.mission_units = 0;
            for (const galactic::BattleForces& bf : battles.conflicts().active_request().forces) out.mission_units += bf.start.size() + bf.reserve.size();
        }
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) {
                if (corruption) {
                    std::vector<objects::ObjectHandle> ground;
                    for (objects::ObjectHandle h : gs->fleets()[f[0]].contents)
                        if (gs->store().alive(h) && gs->rules().types[gs->store().type(h)].kind == galactic::UnitKind::GroundUnit)
                            ground.push_back(h);
                    if (corruption == 2) {  // only the hero goes in: the defenders should win
                        std::vector<objects::ObjectHandle> hero;
                        for (objects::ObjectHandle h : ground)
                            if (gs->store().type(h) == d.types.resolve_name("S4_Hero")) hero.push_back(h);
                        ground = hero;
                    }
                    CHECK(battles.start_corruption(1, 1, 5) == CampaignBattles::CorruptionStart::Refused);  // no Bribery option
                    CHECK(battles.start_corruption(1, 0, 2) == CampaignBattles::CorruptionStart::Refused);  // own planet
                    out.credits_before = gs->credits(1);
                    out.mission = static_cast<int>(battles.start_corruption(1, 1, 2, ground));
                    out.credits_after = gs->credits(1);
                } else {
                    CHECK(gs->invade(1, 1, {}) == R::None);
                }
                invaded = true;
            }
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (b && tm) {
            const objects::ObjectStore& s = tm->primary();
            if (troopers.empty()) {
                for (objects::ObjectHandle h : s.owned_by(0))
                    if (s.alive(h) && s.type(h) == trooper_t && b->movement().has_unit(s.id(h))) troopers.push_back(s.id(h));
                for (objects::ObjectHandle h : s.all_objects()) {
                    if (s.type(h) == d.types.resolve_name("S4_Bunker")) bunker = s.id(h);
                    if (s.type(h) == d.types.resolve_name("S4_Post")) post = s.id(h);
                    if (s.type(h) == d.types.resolve_name("S4_Sensor")) sensor = s.id(h);
                }
            }
            const uint32_t f = b->frame();
            if (attack_sensor) {  // a1f t07
                if (troopers.empty() || sensor == objects::kNoObjectId) continue;
                const tactical::combat::CombatSystem& c = b->combat();
                if (f == 2) {
                    out.sensor_combat = c.has_unit(sensor) && c.alive(sensor) && c.max_health(sensor) > 0.0f;
                    out.sensor_point = b->captures() && b->captures()->is_point(sensor);
                }
                if (f == 60) {  // the troopers stood next to it for 60 frames: the target scan never picked it
                    out.sensor_untouched = out.sensor_combat && c.health(sensor) == c.max_health(sensor);
                    out.attack_order = static_cast<int>(b->order_attack(*tm, 0, troopers, sensor, host.random().synced()));
                }
                if (out.sensor_combat && c.alive(sensor)) out.sensor_health = c.health(sensor) / c.max_health(sensor);
                if (out.sensor_combat && !out.sensor_killed && !c.alive(sensor)) out.sensor_killed = true;
                if (out.sensor_killed && b->captures() && !b->captures()->is_point(sensor)) out.sensor_point_removed = true;
                continue;
            }
            if (std::getenv("S4_DEBUG") && (f % 50) == 1 && !troopers.empty()) {
                for (const objects::ObjectId id : troopers) {
                    const objects::ObjectHandle h = s.find(id);
                    if (!s.alive(h)) continue;
                    const Vec3 p = s.position(h);
                    std::printf("   f%u trooper %d at %.0f,%.0f goal %d holder %d pending %d\n", f, id, static_cast<double>(p.x),
                                static_cast<double>(p.y), b->movement().has_unit(id) ? b->movement().view(id).has_goal : -1,
                                b->garrison_holder_of(id), b->garrison().pending_holder(id));
                }
                if (bunker != objects::kNoObjectId && post != objects::kNoObjectId)
                    std::printf("   f%u bunker owner %d; post owner %d progress %.2f\n", f, s.owner(s.find(bunker)), s.owner(s.find(post)),
                                b->captures() ? static_cast<double>(b->captures()->progress(post)) : 0.0);
            }
            auto ids = [](std::initializer_list<objects::ObjectId> l) {
                std::vector<uint32_t> w;
                for (const objects::ObjectId id : l) w.push_back(static_cast<uint32_t>(id));
                return w;
            };
            if (!corruption && troopers.size() >= 4) {
                if (f == 5) {
                    std::vector<uint32_t> all;
                    for (const objects::ObjectId id : troopers) all.push_back(static_cast<uint32_t>(id));
                    commands::EventData sel(T::Select);
                    sel.set_ids("ids", all);
                    host.submit(0, sel);
                    commands::EventData mv(T::MoveToPosition);
                    mv.set_f("x", 900.0f).set_f("y", 660.0f).set_f("z", 0.0f);
                    host.submit(0, mv);
                } else if (f == 40) {
                    commands::EventData sel(T::Select);
                    sel.set_ids("ids", ids({troopers[1]}));
                    host.submit(0, sel);
                    commands::EventData es(T::Escort);
                    es.set("target", static_cast<uint32_t>(troopers[2]));
                    host.submit(0, es);
                } else if (f == 20) {
                    out.can_query = b->can_garrison(troopers.back(), bunker) && !b->can_garrison(troopers.back(), post) &&
                                    b->can_garrison_fire(bunker) && b->contained_by(troopers.back()) == objects::kNoObjectId;
                    commands::EventData g(T::Garrison);
                    g.set("unit", static_cast<uint32_t>(troopers.back())).set("garrison", static_cast<uint32_t>(bunker));
                    host.submit(0, g);
                }
                // battle host: the hero is no longer the attacker's lead unit (TACTICAL_GAPS §4.2 land split: the lead is the
                // first non-hero, the hero waits in the reserve) - capture it once it is on the map (frame >= 80).
                if (f >= 80 && !out.hero_changed)
                    for (objects::ObjectHandle h : s.owned_by(1))
                        if (s.alive(h) && s.type(h) == d.types.resolve_name("S4_Hero") && !out.hero_changed) out.hero_changed = battles.change_owner(h, 0);
                if (b->garrison_holder_of(troopers.back()) == bunker && bunker != objects::kNoObjectId) {
                    ++out.garrisoned_frames;
                    out.garrison_flag = (b->combat().flags(troopers.back()) & tactical::combat::kUnitGarrisoned) != 0 &&
                                        !b->movement().has_unit(troopers.back());
                    out.fire_query = b->can_garrison_fire(troopers.back()) && b->garrisoned_units(bunker).size() == 1;
                    out.contained = b->contained_by(troopers.back()) == bunker;
                    out.bunker_taken = out.bunker_taken || s.owner(s.find(bunker)) == 0;
                    if (out.garrisoned_frames == 60) {
                        commands::EventData g(T::Garrison);
                        g.set("unit", static_cast<uint32_t>(troopers.back())).set("garrison", commands::kNoObject);
                        host.submit(0, g);
                    }
                } else if (out.garrisoned_frames >= 60 && !out.left) {
                    out.left = true;
                    out.readded = b->movement().has_unit(troopers.back()) &&
                                  (b->combat().flags(troopers.back()) & tactical::combat::kUnitGarrisoned) == 0 &&
                                  b->contained_by(troopers.back()) == objects::kNoObjectId;
                }
                if (post != objects::kNoObjectId) out.post_taken = out.post_taken || s.owner(s.find(post)) == 0;
            }
        }
    }
    if (corruption)
        for (const galactic::ConflictLogEntry& e : battles.conflicts().log())
            if (e.how == 2) out.winner = e.winner;
    if (host.story()) {
        out.log = host.story()->log_text("S4 story battle");
        out.lua = host.story()->lua_messages();
    }
    out.battles = battles.log();
    out.crc = host.crc_log();
    return out;
}

void test_s4_battle(const Data& d) {
    const fs::path x = d.root / "Data/XML";
    write_file(x / "Story_Plots_S4.xml", "<Story_Mode_Plots><Active_Plot>Story_S4.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S4.xml",
               "<Story>\n"
               " <Event Name=\"Go_Land\"><Event_Type>STORY_LAND_TACTICAL</Event_Type><Event_Param1>Story_Plots_S4_Tac.xml</Event_Param1></Event>\n"
               // L1_S4_REMAINING.md §4.4: Param1 planets, Param3 GROUND / SPACE; the reward fills the land map slot.
               " <Event Name=\"Map_Hook\"><Event_Type>STORY_LOAD_TACTICAL_MAP</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param3>GROUND</Event_Param3><Reward_Type>SET_TACTICAL_MAP</Reward_Type><Reward_Param1>S4_Land_Alt</Reward_Param1>"
               "<Reward_Param2>GROUND</Reward_Param2></Event>\n"
               " <Event Name=\"Map_Space_Never\"><Event_Type>STORY_LOAD_TACTICAL_MAP</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param3>SPACE</Event_Param3></Event>\n"
               // Param2 entry 0: the first S4_Hero object of the galactic store must be at the battle planet (it rides in the
               // invading fleet); no S10_Corvette exists in the land setup.
               " <Event Name=\"Map_Hero\"><Event_Type>STORY_LOAD_TACTICAL_MAP</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param2>S4_Hero</Event_Param2></Event>\n"
               " <Event Name=\"Map_Hero_Never\"><Event_Type>STORY_LOAD_TACTICAL_MAP</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param2>S10_Corvette</Event_Param2></Event>\n"
               // SCRIPT_GAPS.md §4.7: Param1 planets, Param2 the corruption keyword (default any).
               " <Event Name=\"Corrupt_Done\"><Event_Type>STORY_CORRUPTION_TACTICAL_COMPLETE</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param2>KIDNAPPING</Event_Param2></Event>\n"
               " <Event Name=\"Corrupt_Failed\"><Event_Type>STORY_CORRUPTION_TACTICAL_FAILED</Event_Type><Event_Param1>S10_B</Event_Param1></Event>\n"
               " <Event Name=\"Corrupt_Wrong_Type\"><Event_Type>STORY_CORRUPTION_TACTICAL_COMPLETE</Event_Type><Event_Param1>S10_B</Event_Param1>"
               "<Event_Param2>BRIBERY</Event_Param2></Event>\n"
               " <Event Name=\"Pirated\"><Event_Type>STORY_CORRUPTION_CHANGED</Event_Type><Event_Param1>S10_A</Event_Param1>"
               "<Event_Param2>PIRACY</Event_Param2></Event>\n"
               "</Story>\n");
    write_file(x / "Story_Plots_S4_Tac.xml", "<Story_Mode_Plots><Active_Plot>Story_S4_Tac.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S4_Tac.xml",
               "<Story>\n"
               " <Event Name=\"Commanded\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1></Event>\n"
               " <Event Name=\"Commanded_Near_Post\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param2>S4_Post</Event_Param2><Event_Param3>300</Event_Param3></Event>\n"
               " <Event Name=\"Commanded_Far_Never\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param2>S4_Bunker</Event_Param2><Event_Param3>10</Event_Param3></Event>\n"
               " <Event Name=\"Commanded_Frigate_Never\"><Event_Type>STORY_COMMAND_UNIT</Event_Type><Event_Param1>S10_Frigate</Event_Param1></Event>\n"
               " <Event Name=\"Guarded\"><Event_Type>STORY_GUARD_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param2>S10_Trooper</Event_Param2></Event>\n"
               " <Event Name=\"Guard_Bunker_Never\"><Event_Type>STORY_GUARD_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param2>S4_Bunker</Event_Param2></Event>\n"
               " <Event Name=\"Garrisoned\"><Event_Type>STORY_GARRISON_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1></Event>\n"
               " <Event Name=\"Garrison_Enemy_Never\"><Event_Type>STORY_GARRISON_UNIT</Event_Type><Event_Param1>S10_Trooper</Event_Param1>"
               "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3></Event>\n"
               // §4.2: Param1 structure types AND Param2 the new owner's faction; an empty Param2 never fires.
               " <Event Name=\"Post_Captured\"><Event_Type>STORY_CAPTURE_STRUCTURE</Event_Type><Event_Param1>S4_Post</Event_Param1>"
               "<Event_Param2>Empire</Event_Param2></Event>\n"
               " <Event Name=\"Bunker_Captured\"><Event_Type>STORY_CAPTURE_STRUCTURE</Event_Type><Event_Param1>S4_Bunker</Event_Param1>"
               "<Event_Param2>Empire</Event_Param2></Event>\n"
               " <Event Name=\"Capture_No_Faction_Never\"><Event_Type>STORY_CAPTURE_STRUCTURE</Event_Type><Event_Param1>S4_Post</Event_Param1></Event>\n"
               " <Event Name=\"Capture_Wrong_Faction_Never\"><Event_Type>STORY_CAPTURE_STRUCTURE</Event_Type><Event_Param1>S4_Post</Event_Param1>"
               "<Event_Param2>Underworld</Event_Param2></Event>\n"
               // §4.3: no dispatcher in 1.121.
               " <Event Name=\"Hero_Captured_Never\"><Event_Type>STORY_CAPTURE_HERO</Event_Type><Event_Param1>S4_Hero</Event_Param1>"
               "<Event_Param3>FILTER_NONE</Event_Param3></Event>\n"
               "</Story>\n");
    write_file(x / "Story_Plots_S4_Mission.xml", "<Story_Mode_Plots><Active_Plot>Story_S4_Mission.xml</Active_Plot></Story_Mode_Plots>\n");
    write_file(x / "Story_S4_Mission.xml",
               "<Story>\n"
               " <Event Name=\"Mission_Won_U\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Underworld</Event_Param1></Event>\n"
               " <Event Name=\"Mission_Won_E\"><Event_Type>STORY_VICTORY</Event_Type><Event_Param1>Empire</Event_Param1></Event>\n"
               "</Story>\n");
    DirectoryProvider files(path_to_utf8(d.root));
    const S4Run a = run_s4_battle(d, files, 0, 0);
    for (const auto& m : a.lua) std::printf("  lua: %s\n", m.c_str());
    for (const auto& m : a.battles) std::printf("  %s\n", m.c_str());
    auto fired_in = [](const std::string& log, const char* ev) { return log.find(std::string(" ") + ev + " ") != std::string::npos; };
    auto fired = [&a, &fired_in](const char* ev) { return fired_in(a.log, ev); };
    auto count = [&a](const char* k) {
        size_t n = 0;
        for (size_t p = a.log.find(k); p != std::string::npos; p = a.log.find(k, p + 1)) ++n;
        return n;
    };
    std::printf("  S4 feeds: TacUnitCommanded %zu, TacUnitGuarding %zu, TacGarrisoned %zu, TacStructureCaptured %zu, LoadTacticalMap %zu; maps",
                count("TacUnitCommanded"), count("TacUnitGuarding"), count("TacGarrisoned"), count("TacStructureCaptured"),
                count("LoadTacticalMap"));
    for (const std::string& m : a.maps) std::printf(" %s", m.c_str());
    std::printf("\n  garrison: inside %u frames, flag %d, fire %d, queries %d, contained %d, left %d, re-added %d, bunker taken %d, post taken %d\n",
                a.garrisoned_frames, a.garrison_flag ? 1 : 0, a.fire_query ? 1 : 0, a.can_query ? 1 : 0, a.contained ? 1 : 0,
                a.left ? 1 : 0, a.readded ? 1 : 0, a.bunker_taken ? 1 : 0, a.post_taken ? 1 : 0);
    CHECK(a.lua.empty());
    CHECK(fired("Go_Land") && fired("Map_Hook") && fired("Map_Hero") && !fired("Map_Space_Never") && !fired("Map_Hero_Never"));
    CHECK(a.maps.size() == 1 && a.maps[0] == "S4_Land_Alt");  // SET_TACTICAL_MAP replaced S10_Land_B
    CHECK(fired("Commanded") && fired("Commanded_Near_Post") && !fired("Commanded_Far_Never") && !fired("Commanded_Frigate_Never"));
    CHECK(fired("Guarded") && !fired("Guard_Bunker_Never"));
    CHECK(fired("Garrisoned") && !fired("Garrison_Enemy_Never"));
    CHECK(fired("Post_Captured") && fired("Bunker_Captured") && !fired("Capture_No_Faction_Never") && !fired("Capture_Wrong_Faction_Never"));
    CHECK(a.hero_changed && !fired("Hero_Captured_Never"));
    CHECK(a.garrisoned_frames >= 60 && a.garrison_flag && a.fire_query && a.can_query && a.contained && a.left && a.readded &&
          a.bunker_taken && a.post_taken);
    // The AI Underworld's corruption applies directly (no mission option at S10_A anyway).
    CHECK(a.piracy == static_cast<int>(CampaignBattles::CorruptionStart::Applied) && a.corruption_a == 1 && fired("Pirated"));
    // Determinism: the story log and the per-tick CRCs (story system included) for 0 / 1 / 4 / 15 workers.
    for (const int32_t w : {1, 4, 15}) {
        const S4Run b = run_s4_battle(d, files, w, 0);
        CHECK(b.log == a.log && !first_divergence(a.crc, b.crc).diverged);
    }
    // a1f t07: a map-placed neutral SELECTABLE capture structure (Secondary_Abandoned_Sensor stand-in) is a battle combat
    // unit as well as a capture point; the target scan skips the Is_Neutral player's objects (TACTICAL_COMBAT §2.2), so
    // the Empire troopers next to it leave it alone. S7 t30 (TACTICAL_COMBAT §2.10): the Neutral player is relation 2, so
    // the human attack order is refused too (the attack cursor needs relation 1): the sensor is never damaged and stays a
    // capture point (taken by presence, not by attack).
    {
        const S4Run s = run_s4_battle(d, files, 0, 0, true);
        std::printf("  neutral sensor: combat %d, capture point %d, untouched %d, attack order %d, health %.2f, killed %d, point removed %d\n",
                    s.sensor_combat ? 1 : 0, s.sensor_point ? 1 : 0, s.sensor_untouched ? 1 : 0, s.attack_order,
                    static_cast<double>(s.sensor_health), s.sensor_killed ? 1 : 0, s.sensor_point_removed ? 1 : 0);
        for (const auto& m : s.battles) std::printf("  %s\n", m.c_str());
        CHECK(s.sensor_combat && s.sensor_point && s.sensor_untouched);
        CHECK(s.attack_order == static_cast<int>(commands::RejectReason::InvalidTarget) && !s.sensor_killed && s.sensor_health == 1.0f &&
              !s.sensor_point_removed);
        const S4Run s4 = run_s4_battle(d, files, 4, 0, true);
        CHECK(!first_divergence(s.crc, s4.crc).diverged);
    }
    // Corruption mission (human Underworld).
    const S4Run c = run_s4_battle(d, files, 0, 1);
    for (const auto& m : c.battles) std::printf("  %s\n", m.c_str());
    std::printf("  corruption: mission %d (credits %.0f -> %.0f), winner %d, B corruption %d, A piracy %d; Corrupt_Done %d, Corrupt_Failed %d\n",
                c.mission, static_cast<double>(c.credits_before), static_cast<double>(c.credits_after), c.winner, c.corruption_b, c.piracy,
                fired_in(c.log, "Corrupt_Done") ? 1 : 0, fired_in(c.log, "Corrupt_Failed") ? 1 : 0);
    CHECK(c.mission == static_cast<int>(CampaignBattles::CorruptionStart::MissionQueued) && c.credits_after == c.credits_before - 500.0f);
    CHECK(c.winner == 0 || c.winner == 1);
    CHECK(fired_in(c.log, c.winner == 1 ? "Mission_Won_U" : "Mission_Won_E"));
    CHECK(fired_in(c.log, "Corrupt_Done") == (c.winner == 1) && fired_in(c.log, "Corrupt_Failed") == (c.winner != 1));
    CHECK(!fired_in(c.log, "Corrupt_Wrong_Type"));
    if (c.winner == 1) CHECK(c.corruption_b == 2);
    CHECK(c.log.find("CorruptionMission") != std::string::npos);
    CHECK(c.piracy == static_cast<int>(CampaignBattles::CorruptionStart::Applied) && fired_in(c.log, "Pirated"));
    CHECK(c.mission_units == 0);  // story t08: not sandbox, the map and the mission plots supply every unit
    // story t08: no Underworld unit anywhere near S10_B - the mission starts at once through the linked record (BT §2.9.3,
    // L1_S4_REMAINING §4.5), its battle is pushed at the next hook and the mission's story events follow its winner.
    const S4Run n = run_s4_battle(d, files, 0, 3);
    for (const auto& m : n.battles) std::printf("  %s\n", m.c_str());
    std::printf("  corruption, no units in orbit: mission %d (tick %" PRIu64 ", battle tick %" PRIu64 ", galactic units %zu), winner %d\n",
                n.mission, n.mission_tick, n.battle_tick, n.mission_units, n.winner);
    CHECK(n.mission == static_cast<int>(CampaignBattles::CorruptionStart::MissionQueued) && n.credits_after == n.credits_before - 500.0f);
    CHECK(n.battle_tick == n.mission_tick + 1 && n.mission_units == 0 && (n.winner == 0 || n.winner == 1));
    CHECK(n.log.find("CorruptionMission") != std::string::npos);
    CHECK(fired_in(n.log, "Corrupt_Done") == (n.winner == 1) && fired_in(n.log, "Corrupt_Failed") == (n.winner != 1));
    bool started_line = false;
    for (const std::string& m : n.battles) started_line = started_line || m == "corruption: mission land started";
    CHECK(started_line);
    const S4Run c4 = run_s4_battle(d, files, 4, 1);
    CHECK(c4.log == c.log && !first_divergence(c.crc, c4.crc).diverged);
    // The mission with the hero alone: the defenders win -> _FAILED, no corruption.
    const S4Run f = run_s4_battle(d, files, 0, 2);
    std::printf("  corruption, hero alone: mission %d, winner %d, B corruption %d; Corrupt_Done %d, Corrupt_Failed %d\n", f.mission,
                f.winner, f.corruption_b, fired_in(f.log, "Corrupt_Done") ? 1 : 0, fired_in(f.log, "Corrupt_Failed") ? 1 : 0);
    // story t08: the mission starts through the linked record (not sandbox): no galactic unit enters, so the units the
    // test picks no longer matter; the story events follow the winner.
    CHECK(f.mission == static_cast<int>(CampaignBattles::CorruptionStart::MissionQueued) && (f.winner == 0 || f.winner == 1));
    CHECK(fired_in(f.log, "Corrupt_Failed") == (f.winner != 1) && fired_in(f.log, "Corrupt_Done") == (f.winner == 1) &&
          (f.winner == 1 ? f.corruption_b == 2 : f.corruption_b == -1));
    // Int-final: the human corruption command (SpecialAbility kind 6) through the command stream -> start_corruption.
    {
        uint32_t dd = 0, pp = 0;
        int8_t tt = -1;
        CHECK(read_corruption_command(corruption_command(0x12345u, 0x2468u, 6), dd, pp, tt) && dd == 0x12345u && pp == 0x2468u && tt == 6);
        jobs::JobSystem js(jobs::JobSystemConfig{0});
        SimHost host(d.types, js, SimConfig{});
        galactic::GalacticSetup setup = make_setup(d, true);
        setup.forces.push_back({1, 2, type_of(d, "S4_Hero")});
        galactic::GalacticSim* gs = host.start_galactic(d.rules, setup);
        CHECK(gs != nullptr);
        if (gs) {
            CampaignBattleOptions opt;
            CampaignBattles battles(host, d.crules, *d.catalog, [](const std::string&, terrain::MapData&) { return false; }, opt);
            CHECK(battles.attach());
            objects::ObjectHandle hero{};
            for (objects::ObjectHandle h : gs->planets()[2].surface)
                if (gs->store().alive(h) && gs->store().type(h) == d.types.resolve_name("S4_Hero")) hero = h;
            CHECK(gs->store().alive(hero));
            const auto hid = static_cast<uint32_t>(gs->store().id(hero));
            const auto planet_a = static_cast<uint32_t>(gs->store().id(gs->planets()[0].object));
            const uint64_t rejected0 = host.commands_rejected();
            host.submit(0, corruption_command(hid, planet_a, 1));  // not the defiler's owner: refused
            host.tick_once();
            host.tick_once();
            CHECK(host.commands_rejected() == rejected0 + 1 && gs->planets()[0].corruption == galactic::kCorruptionNone);
            host.submit(1, corruption_command(hid, planet_a, 1));  // Piracy at S10_A (no mission): direct timed gain
            host.tick_once();
            host.tick_once();
            std::printf("  corruption command: S10_A corruption %d, result %d\n", gs->planets()[0].corruption,
                        static_cast<int>(battles.last_corruption_start()));
            CHECK(gs->effective_corruption_type(0) == 1 && gs->corruption_transition_active(0) &&
                  battles.last_corruption_start() == CampaignBattles::CorruptionStart::Applied);
            for (int t = 0; t < 2000 && gs->corruption_transition_active(0); ++t) host.tick_once();  // GRC 4.3: 5 s gain
            CHECK(gs->planets()[0].corruption == 1 && !gs->corruption_transition_active(0));
        }
    }
}
// --- end S4 ---
#endif

}  // namespace


int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_temp_prefix = "alamo_sim_battle_story_tests";
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--regen") g_regen = true;
    auto d = make_data();
    std::printf("story block freezes the conflict state machines and the galactic frame (a1j t05)\n");
    test_story_block_host(*d);
#ifdef ALAMO_SIM_HAVE_STORY
    std::printf("story battle (L1/S4 wave 2a)\n");
    test_story_battle(*d);
    std::printf("linked battle flag (REMAINING_GAPS 6.4)\n");
    test_linked_battle(*d);
    test_linked_params(*d);  // story t07
    test_linked_map_objects(*d);  // story t10
    std::printf("story t09 / t11 / t12: victory rewards (tactical, galactic) and the P9 hyperspace hold\n");
    test_story_victory_rewards(*d);
    test_galactic_story_victory(*d);
    std::printf("story Invade() of a scripted fleet (a1f t08, S10 PROBE A)\n");
    test_probe_invade(*d);
    test_invade_planet_reward(*d);
    std::printf("S4 story battles: orders, garrisons, capture, map load, corruption missions\n");
    {
        auto ds4 = make_data(false, false, true);  // S4
        test_s4_battle(*ds4);
    }
#endif
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_battle_story_tests: all passed\n");
    return 0;
}

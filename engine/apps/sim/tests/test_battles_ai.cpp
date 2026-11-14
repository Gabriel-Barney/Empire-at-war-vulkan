// test_battles_ai: main() (bo3 t03: tests live in test_battles_ai_partN.cpp, shared code in test_battles_ai_common.hpp).
#include "test_battles_ai_common.hpp"


// S7 t22 / t25 (FIGHTER_FLYING §4.10 A, TACTICAL_COMBAT §2.8 "Targets of opportunity"): the response-range test 0x4160a0.

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_temp_prefix = "alamo_sim_battle_ai_tests";
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--regen") g_regen = true;
        if (std::string(argv[i]) == "--hero-clash") return hero_clash_worker_check(argc, argv);  // s78n t06 review
    }
    auto d = make_data();
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    std::printf("OI host: BattleAi MoveToObject, hijack_unit, AI Change_Owner\n");
    test_battle_ai_orders(*d);
    std::printf("refixf t02: SPACE_OBSTACLE station as a static entry, per-unit Lua orders as one group move\n");
    test_space_obstacle_orders();
    test_neutral_attack_move();  // S7 t29 / t30
    test_battle_ai_station_level(false);
    test_battle_ai_station_level(true);
    test_battle_ai_land_mode(false);
    test_battle_ai_land_mode(true);
    test_battle_ai_land_mode(true, true);
    test_battle_ai_land_mode(false, true);
    std::printf("refixf t03: Find_All_Objects_Of_Type('S10_Squad') finds the TEAM containers (representative type hook)\n");
    test_battle_ai_find_team();
    std::printf("q6 t28: map hint names (.ted 0x482) reach Find_Hint / Get_Hint / Find_All_Objects_With_Hint\n");
    test_battle_map_hints();
    std::printf("q6: an attack ordered on a TEAM container (AI host and human order; nearest member, order ends with the container)\n");
    test_battle_attack_team();
    std::printf("a1g t04 follow-up: BattleAi::land_drop_ok (impassable / fogged / movers / enabling radius)\n");
    test_battle_ai_land_drop(*d);
    std::printf("a2h t05: the battle hosts load the tactical AI constants from GameConstants\n");
    test_tactical_ai_constants_load();
#endif
    std::printf("s78f t07: land team members registered with combat (TeamTargeting copy in a battle)\n");
    test_land_team_targeting();
    std::printf("s78j t03 / t04: land team SPEED = the container's value; Change_Owner spreads through the team; reserve drop facing\n");
    test_team_speed_and_owner();
    test_reserve_drop_facing(true);
    std::printf("s78k t04 / t05: convert spreads to the team container; reserve drop facing != the side's start facing\n");
    test_team_convert();
    test_reserve_drop_facing(false);
    std::printf("s78l t04: Hack / Tactical_Bribe spread to the whole team; a vehicle's landing exit ends on the landing yaw\n");
    test_team_hack_bribe(false);
    test_team_hack_bribe(true);
    for (const int variant : {0, 1, 2}) test_vehicle_thief(variant);  // s78n t05; s78n t12: without a clone type
    test_drop_extents_no_model();  // s78n t10
    {  // s78n t06 (+ review): hero clashes in a land battle (the worker-count check is its own ctest entries, --hero-clash)
        auto hd = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
        test_hero_clash_battle(*hd);
        for (const int how : {0, 1}) test_thief_release(*hd, how);                // s78n t12 review
        for (const bool n : {false, true}) test_thief_save_load(*hd, n);          // s78n t12 review
        for (const int how : {0, 1}) test_command_kill_save_load(*hd, how);       // s78n t19
        test_thief_dies_same_tick(*hd);                                           // s78n t12 review
        test_hero_clash_garrison(*hd);
    }
    test_land_squad_container();  // s78n t03 / t09
    test_land_squad_garrison();   // s78n review
    std::printf("s78n t18 review: a squad's garrison approach, entry, stay and walk-out through a save / load; a hostile host\n");
    test_land_squad_garrison_save_load();
    std::printf("s78n t20 batch 2: a squad's approach to a garrison vehicle that drives off (HostStop, corrective move)\n");
    test_land_squad_garrison_host_stop();
    test_land_hover_garrison_death();  // s78n t20 batch 2 item 2.4
    std::printf("s78n t20 stage A: the land attack re-check (displaced squad, friendly squads on the line, save / load)\n");
    test_land_attack_blocks();
    test_land_attack_order();
    test_land_guard();  // s78n t25.4
    test_land_move_to_object();  // s78n t26 review round 2
    test_land_attack_formation_stop();  // s78n t26 review round 2
    test_land_attack_unseen();          // s78n t26 review round 3
    test_land_attack_reach();  // s78n t20 review follow-ups
    test_land_death_clone_obstacle();  // s78n t20 batch 1 item 1.5
    test_vehicle_landing_exit();
    std::printf("a1i t04 / t05: land Reinforce from a landing object (spawn, carried type, unload offsets, take-off)\n");
    test_landing_drop(false);
    test_landing_drop(true);
    test_landing_drop(true, 90);  // a1j t01: destroyed with team 0 out
    test_landing_drop(true, 30);  // a1j t01: destroyed before any unit is out
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_battle_ai_tests: all passed\n");
    return 0;
}

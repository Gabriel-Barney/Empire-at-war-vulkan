// test_movement_gaps: main() (bo3 t03: tests live in test_movement_gaps_partN.cpp, shared code in test_movement_gaps_common.hpp).
#include "test_movement_gaps_common.hpp"


int main() {
    test_corridor();
    test_lanes();  // s78p (b0049)
    test_threat();
    test_rules();
    test_flying_rules();
    test_jetpack();
    test_flying();
    test_flying_clearance();
    test_multi_locomotor();
    test_jetpack_alternates();     // S7 t14
    test_jetpack_orders();         // S7 t14
    test_multi_locomotor_types();  // S7 t14
    test_jetpack_accel_fallback();   // S7 t15
    test_jetpack_turn_bands();       // S7 t15
    test_jetpack_no_repeated_pose(); // S8 t09 (PR #19 review)
    test_deploy();                   // S8 t11 / t12
    test_turret();                   // S8 t24
    test_bike();
    test_bomber();
    test_fighter_space();
    test_fighter_guard();
    test_fighter_formation_stack();  // s78f t02
    test_space_avoid();
    test_speed_override();
    test_arrival_turn();  // s78k t01
    test_forced_path();   // s78k t01
    test_team_cap();      // s78k t03
    test_squad_team_speed();  // s78l t04
    test_team_speed_exact();     // s78l t01
    test_arrival_turn_rules();   // s78l t02
    test_arrival_turn_clips();   // s78l t02
    test_arrival_close();        // s78l t02
    test_forced_destination();   // s78l t02
    test_land_squad();           // s78n t03
    test_land_squad_review();    // s78n review
    test_land_squad_layout();    // s78n t14
    test_land_squad_walk_states();  // s78n t14
    test_land_squad_part_b();       // s78n t14 part B
    test_land_squad_speed_step();   // s78n t16
    test_land_squad_review_fixes(); // s78n review
    test_land_squad_teleport();     // s78n t18 item 8
    test_land_squad_footprint();    // s78n t18 items 2 / 6
    test_land_squad_repulsion();    // s78n t18 items 1 / 7
    test_land_squad_speed_order();  // s78n t18 item 3
    test_land_squad_gates();        // s78n t18 items 4 / 5
    test_land_squad_t18_review();   // s78n t18 review
    test_t20_destination_check();   // s78n t20 item 6
    test_t20_path_around();         // s78n t20 item 3
    test_t20_unsnapped_goal();       // s78n t20 F5
    test_t20_detour_start_inside();  // s78n t20 F3
    test_t20_early_arrival();       // s78n t20 item 4
    test_t20_decision_leftovers();  // s78n t20 item 5
    test_t20_stopped_vs_stopped();  // s78n t20 item 1
    test_t20_death_clone();         // s78n t20 batch 1 item 1.5
    test_determinism();
    if (g_fail) {
        std::printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    std::printf("all movement gap tests passed\n");
    return 0;
}

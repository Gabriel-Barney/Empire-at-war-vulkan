// test_combat: main() (bo3 t03: tests live in test_combat_partN.cpp, shared code in test_combat_common.hpp).
#include "test_combat_common.hpp"

void* operator new(std::size_t n) {
    if (g_count.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }

int main(int argc, char** argv) {
    const char* golden = nullptr;
    const char* write_golden = nullptr;
    bool bench = true;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--golden") && i + 1 < argc) golden = argv[++i];
        else if (!std::strcmp(argv[i], "--write-golden") && i + 1 < argc) write_golden = argv[++i];
        else if (!std::strcmp(argv[i], "--no-bench")) bench = false;
    }
    test_scan_cadence();
    test_recharge();
    test_inaccuracy_formula();
    test_range_lead_area();
    test_modifier_stacking();
    test_modifier_expiry();
    test_reduce_all_matches();
    test_mod_multiplier();
    test_priority_sets();
    test_choose_target_hardpoint_by_type();
    test_damage_table();
    test_inaccuracy_system();
    test_straight_ground_test();
    test_targeting_rules();
    test_circular_lead();  // OI tactical
    test_lob_solver();
    test_lob_targeting();
    test_saber_projectile();  // cu8k t01
    test_lob_weapon_flight();
    test_lob_ground_and_expiry();
    test_lob_sticky();
    test_ground_segment();
    test_lob_object_ground_tie();
    test_lob_fuse_rounding();
    test_lob_hardpoint();
    test_lob_save();
    test_turn_toward();  // S9 t05
    test_team_targeting();
    test_gaps_constants();
    test_land_obstacle_and_weapon_index();
    test_s78n_min_range_slot46();  // s78n t02
    test_s78n_can_hit();  // s78n t20 stage A
    test_fog_filter();
    test_attack_move_object();
    test_hardpoint_fire_routing();
    test_precise_hit_hook();
    test_take_damage();
    test_shielded_only_when_deployed();  // s78n t26.1
    test_shield_regen_system();
    test_hull_hardpoint_coupling();
    test_hardpoint_disabled();
    test_station_upgrade_copy();
    test_modifier_cache();
    test_battle(golden, write_golden);
    test_allocations();
    test_save_load();  // N1 stage 2
    test_save_rejects();
    test_stickiness_override();  // cu8f t04a
    test_weather_save();
    test_rocket_spline();  // refixf t01
    test_rocket_leg();
    test_rocket_plans();
    test_rocket_replan();
    test_rocket_flight_system();
    test_weapon_sources();  // S8 t08
    test_hardpoint_state_gate();
    test_weapon_suspend_elapse();  // S7 t15 A2
    test_muzzle_bones();  // S8 t06 B4
    test_own_weapon_targeting_state();  // S8 t10 - t12
    test_own_weapon_gate_steps();
    test_hardpoints_fire_while_suspended();
    test_first_service();
    test_own_player_sweep();
    test_garrison_fire_bones();
    test_hardpoint_mode_default();
    test_own_weapon_aim_point();
    test_own_weapon_target_bones();  // S8 t27
    test_fire_at_position();         // S8 t26
    test_targeting_states();         // s78f t03
    test_targeting_t08();            // s78f t08
    test_team_targeting_copy();      // s78f t03
    test_team_handover_and_damage();  // s78g t06
    test_squad_value();               // s78h t03
    test_convert_team();              // s78h t04
    test_convert_victim_test();       // s78n t05
    test_s78n_review_combat();
    test_incognito_targeting();       // s78n t05
    test_hover_raised_melee();        // s78n t20 batch 2
    test_energy_drain_engines();      // cu8f t02
    test_team_container_slot();       // s78h t06
    test_team_head_alone();           // s78i t03
    test_convert_container_unit();    // s78i t04
    test_scan_player_order();         // s78g t04
    test_charge_up_slew();           // s78f t04
    test_hardpoint_aim_target_bones();  // S8 t27
    test_scan_range_query();
    test_shared_team_target();
    test_foc_data();
    test_hero_clash_parse_and_rules();  // s78n t06
    test_hero_clash_start_rule();
    test_hero_clash_fight();
    test_hero_clash_end_and_save();
    test_hero_clash_pairing();
    test_hero_clash_fire_refused();
    test_hero_clash_foc_data();
    if (bench) bench_modifiers();
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("all combat tests passed\n");
    return 0;
}

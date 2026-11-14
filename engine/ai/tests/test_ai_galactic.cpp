// test_ai_galactic: main() (bo3 t03: tests live in test_ai_galactic_partN.cpp, shared code in test_ai_galactic_common.hpp).
#include "test_ai_galactic_common.hpp"


int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the progress lines when a test crashes
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--write-golden") == 0) g_write_golden = true;
    test_cadence();
    test_goal_pass_cadence();  // a2m r2
    test_desire_and_hysteresis();
    test_goal_reservations();
    test_queue_slot_lifetime();
    test_goal_batched_values();
    test_goal_batched_accept();  // OPT2 t10
    test_plan_pick_and_solver();
    test_definitions_and_templates();
    test_budget_membership();  // q6 contrast/budget (§9.5)
    test_contrast_rules();     // q6 contrast/budget (§9.8)
    test_plan_load_parallel();
    test_synthetic_campaign();
    test_path_finder_world();
    test_galactic_clock_phase();  // a2m r4
    test_sim_hooks();
    test_starbase_force_split();
    test_leaf_cache();
    test_force_members();
    test_escort_flag();
    test_category_masks_from_enum_files();  // A2k t06
    test_type_info_mod_categories();        // A2k t06
    test_parity_rules();
    test_free_store_supply();
    test_ai_clock_precision();
    test_stage_and_setup_a();
    test_null_stage_and_plain_produce();
    test_plain_produce_hands_back();
    test_move_to_fleet_object();
    test_fleet_object_reelection();
    test_produce_force_stage_launch();
    test_land_units_owner_rule();
    test_no_units_remaining_counted_entries();
    test_ended_production_cancel();  // a2m r2
    test_structure_eta_queue_wait();  // a2m r2
    test_production_disabled_and_delivery();
    test_ground_structure_slots();
    test_budget_fraction_before_init();
    test_production_unbuildable();
    test_goal_time_drop();
    test_goal_allowed_time_base();  // A2f t09
    test_btime_none_speed_factor();    // a2g t03
    test_goal_nominal_time_resolve();  // a2g t05 (a)
    test_goal_resolve_time_ok();       // a2g t05 (b)
    test_active_goals_category();      // a2g F1, a2h t04
    test_maintenance_tokens();         // a2h t03
    test_force_maintenance_tokens();   // a2i t04
    test_budget_fraction_to_build();   // a2h t07
    test_budget_fraction_in_progress();  // a2i t06
    test_budget_reset_on_template_switch();  // a2g F2
    test_free_store_path_rules();
    test_free_store_world();
    test_prefer_ground_auto_land();  // cu8f t04b
    test_underworld_special();
    test_save_synthetic();
    test_planet_type_is_type();
    test_player_conflict_tokens();   // q6 t03
    test_budget_allocation_token();  // q6 t03
    test_loaded_category_enum();     // q6 t03
    test_ai_type_switch();
#ifdef ALAMO_AI_HAVE_VFS
    test_foc_data();
    test_save_foc();
#endif
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

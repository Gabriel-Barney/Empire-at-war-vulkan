// test_abilities: main() (bo3 t03: tests live in test_abilities_partN.cpp, shared code in test_abilities_common.hpp).
#include "test_abilities_common.hpp"

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
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--golden") && i + 1 < argc) golden = argv[++i];
        if (!std::strcmp(argv[i], "--write-golden") && i + 1 < argc) write_golden = argv[++i];
    }
    if (std::getenv("ALAMO_S9_BATTLE_ONLY")) {
        Battle b;
        b.run(900, serial_for, false);
        return 0;
    }
    test_info_table();
    test_countdown();
    test_parse();
    test_modes();
    test_gates();
    test_squad();
    test_black_market_gate();
    test_hardpoints();
    test_last_hardpoint_kills();
    test_death_clone_remap();
    test_infection();
    test_lob_abilities();
    test_remote_bomb_sequence();
    test_remote_bomb_detonate();
    test_s8_apply_facing_marker_deploy();  // S8 t10 / t11
    test_fire_and_decay();
    test_energy();
    test_ion_stun();
    test_missile_shield();
    test_opportunity_fire();
    test_tractor();
    test_ion_and_lucky();
    test_concentrate_energy();
    test_stealth_jamming_laser();
    test_jamming_visual_fields();
    test_fog_abilities();
    test_black_market_specials();
    // OI abilities
    test_oi_tables();
    test_oi_sap_init_and_defaults();
    test_oi_autofire();
    test_oi_hunt();
    test_oi_waves_and_confuse();
    test_oi_melee_and_chance();
    test_oi_mines();
    test_oi_death_draws();
    test_oi_auras();
    test_s78m_aura_accept();
    test_oi_repair_priorities();
    test_hardpoint_online_effects();
    test_oi_detonations();
    test_oi_shields_jamming_ion();
    test_oi_class_bodies();
    // REMAINING_GAPS §5
    test_gaps_eject_thief();
    test_gaps_deploy_troopers();
    test_cu8f_parse_defaults();
    test_cu8f_lure();
    test_cu8f_deploy_cap();
    test_cu8l_stop_hold();  // cu8l t02
    test_cu8f_enable_gates();
    test_cu8f_super_laser();
    test_gaps_jet_pack();
    test_gaps_stun_release();
    test_gaps_blast();
    test_s78n_t12_thief_apply();
    test_s78n_review_stim_pack();
    test_s78n_review_blast_battle();
    test_gaps_leech();
    test_oi_weapon_swap();
    test_oi_berserker_speed();
    test_oi_stun_over_water();
    test_oi_anim_timings();
    test_oi_anim_gates_and_locks();
    test_bribe_hack_accept();
    test_maximum_firepower_incognito();  // s78n t05
    test_bribe_range();  // s78m t08
    test_hack_repair_range();  // s78n t07 / t01
    test_s78n_allied_users();  // s78n t07
    test_s78n_min_range();  // s78n t02
    test_s78n_t11_ability_range();  // s78n t11 review
    test_s78n_combat_bonus_and_enhance_defense();  // s78n t04
    test_cu8f_abilities_disabled();
    test_cu8j_saber_flight();
    test_hero_clash_accepts();  // s78n t06
    test_battle(golden, write_golden);
    test_allocations();
    test_save_load_battle();  // N1 stage 2
    test_save_load_mines();
    test_foc_data();
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("all abilities tests passed\n");
    return 0;
}

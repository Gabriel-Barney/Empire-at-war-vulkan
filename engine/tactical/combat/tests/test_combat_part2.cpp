// Part 2 of test_combat (bo3 t03: split by position; see test_combat_common.hpp).
#include "test_combat_common.hpp"

namespace bo3split {

void test_shield_regen_system() {
    World w;
    UnitDesc s;
    s.tactical_health = 100.0f;
    s.shield_points = 100.0f;
    s.shield_refresh_rate = 10.0f;
    const uint32_t ds = push_unit(w.data, s);
    w.make();
    w.add(1, ds, 0, Vec3{});
    DamageRequest r;
    r.victim = 1;
    r.amount = 100.0f;
    w.sys->take_damage(r, 1);  // depleted at frame 1
    CHECK_F(w.sys->shield(1), 0.0f);
    // Regeneration every int(30 x 1 x 3 + 0.5) = 90 frames: frame 90 is inside the 5 s lockout
    // (cap 0.25 absolute points), frame 180 is not.
    while (w.frame <= 90) w.tick();
    CHECK_F(w.sys->shield(1), 0.25f);
    while (w.frame <= 179) w.tick();
    CHECK_F(w.sys->shield(1), 0.25f);
    w.tick();
    CHECK_F(w.sys->shield(1), 0.25f + 10.0f);
}
}  // namespace bo3split

namespace bo3split {

void test_hull_hardpoint_coupling() {
    World w;
    HardpointDesc hd;
    hd.kind = HardpointKind::Engine;
    hd.health = 100.0f;
    hd.destroyable = true;
    w.data.hardpoints.push_back(hd);
    UnitDesc u;
    u.tactical_health = 1000.0f;
    u.hardpoints = {0, 0};
    u.destroy_when_all_hardpoints_destroyed = true;
    const uint32_t du = push_unit(w.data, u);
    CombatConfig cfg;
    cfg.object_max_health_multiplier = 1.0f;  // hardpoints keep the space x1.5 -> 150 each, T = 300
    w.make(cfg);
    w.add(1, du, 0, Vec3{});
    w.add(2, du, 0, Vec3{100, 0, 0});
    CHECK_F(w.sys->hardpoint_health(1, 0), 150.0f);
    // Unit 1: hull at 20% -> hardpoint cap: excess = (1 - min(1, 0.2 + 0.2)) x 300.
    w.sys->set_health(1, 200.0f);
    // Unit 2: hardpoints at 15/150 each -> hull capped at (0.1 + 0.2) x max.
    DamageRequest r;
    r.victim = 2;
    r.amount = 135.0f;
    r.hardpoint = 0;
    w.sys->take_damage(r, 1);
    r.hardpoint = 1;
    w.sys->take_damage(r, 1);
    w.tick();
    const float excess = (1.0f - (200.0f / 1000.0f + 0.2f)) * 300.0f;
    CHECK_F(w.sys->hardpoint_health(1, 0), 150.0f - 150.0f / 300.0f * excess);
    CHECK_F(w.sys->hardpoint_health(1, 1), 150.0f - 150.0f / 300.0f * excess);
    CHECK_F(w.sys->health(2), (30.0f / 300.0f + 0.2f) * 1000.0f);
}
}  // namespace bo3split

namespace bo3split {

// refixf t06: Choose_Target_Hardpoint (TACTICAL_COMBAT 2.6 [H]) ranks the target's live destroyable hardpoints by the
// position of their TYPE in the attacker's Hard_Point_Priorities (Lua override first, then the type's set), skips the
// Hard_Point_Exclusions, unlisted types rank 1e18 with ties to the closest in 3D; without a set, the closest targetable.
void test_choose_target_hardpoint_by_type() {
    World w;
    auto hp = [&w](HardpointKind k, float x, bool targetable) {
        HardpointDesc hd;
        hd.name = "HP_" + std::to_string(w.data.hardpoints.size());
        hd.name_crc = name_crc(hd.name);
        hd.kind = k;
        hd.health = 100.0f;
        hd.destroyable = true;
        hd.targetable = targetable;
        hd.attach_offset = Vec3{x, 0, 0};
        w.data.hardpoints.push_back(hd);
        return static_cast<uint32_t>(w.data.hardpoints.size() - 1);
    };
    // Station hardpoints, nearest to the attacker (on -x) first: fighter bay, laser, engine, shield generator, missile.
    const uint32_t bay = hp(HardpointKind::FighterBay, -200.0f, true), laser = hp(HardpointKind::WeaponLaser, -150.0f, true),
                   engine = hp(HardpointKind::Engine, 100.0f, true), shield = hp(HardpointKind::ShieldGenerator, 200.0f, true),
                   missile = hp(HardpointKind::WeaponMissile, 250.0f, false);
    const auto hk = [](HardpointKind k) { return static_cast<int32_t>(k); };
    PrioritySet cam;  // as Capital_Attack_Move: Shield_Generator, Engine, FIGHTER_BAY, ...
    cam.name = "Capital_Attack_Move";
    cam.hardpoint_priorities = {hk(HardpointKind::ShieldGenerator), hk(HardpointKind::Engine), hk(HardpointKind::FighterBay)};
    PrioritySet excl;  // nothing listed, the fighter bay and the laser excluded
    excl.name = "No_Bays";
    excl.hardpoint_exclusions = {hk(HardpointKind::FighterBay), hk(HardpointKind::WeaponLaser)};
    w.data.priority_sets = {cam, excl};
    UnitDesc station;
    station.tactical_health = 5000.0f;
    station.radius = 300.0f;
    station.hardpoints = {bay, laser, engine, shield, missile};
    const uint32_t ds = push_unit(w.data, station);
    UnitDesc ship;
    ship.tactical_health = 1000.0f;
    ship.radius = 50.0f;
    ship.priority_set = 0;
    const uint32_t dset = push_unit(w.data, ship);
    ship.priority_set = -1;
    const uint32_t dnone = push_unit(w.data, ship);
    CombatConfig cfg;
    cfg.hardpoint_health_multiplier = 1.0f;
    w.make(cfg);
    w.add(1, ds, 0, Vec3{0, 0, 0});
    w.add(2, dset, 1, Vec3{-800, 0, 0});
    w.add(3, dnone, 1, Vec3{-800, 0, 0});
    CHECK(w.sys->choose_target_hardpoint(2, 1) == 3);  // the shield generator (rank 1), though the farthest listed
    CHECK(w.sys->choose_target_hardpoint(3, 1) == 0);  // no set: the closest targetable (the fighter bay)
    DamageRequest kill;  // shield generator destroyed -> the engine (rank 2)
    kill.victim = 1;
    kill.amount = 10000.0f;
    kill.hardpoint = 3;
    w.sys->take_damage(kill, w.frame);
    w.tick();
    CHECK(w.sys->alive(1) && w.sys->hardpoint_health(1, 3) <= 0.0f);
    CHECK(w.sys->choose_target_hardpoint(2, 1) == 2);
    CHECK(w.sys->set_priority_override(2, 1));         // Lua Set_Targeting_Priorities: the override wins over the type's set
    CHECK(w.sys->choose_target_hardpoint(2, 1) == 2);  // bay and laser excluded, the rest 1e18: the closest of engine / missile
    CHECK(w.sys->set_priority_override(2, -1));
    CHECK(w.sys->choose_target_hardpoint(2, 1) == 2);
}
}  // namespace bo3split

namespace bo3split {

// Hardpoint disabled flag (hp +0x6e): Update_Weapon returns before the cooldown countdown and Fire_At (TACTICAL_COMBAT
// 3.1 step 1); a repair to max health clears it (TACTICAL_GAPS §1.12 step 4). Repair-capable = DUMMY_STAR_BASE.
void test_hardpoint_disabled() {
    World w;
    w.data.projectiles.push_back(ProjectileDesc{});
    w.data.projectiles[0].max_speed = 10.0f;
    w.data.projectiles[0].damage = 1.0f;
    HardpointDesc hd;
    hd.kind = HardpointKind::WeaponLaser;
    hd.health = 100.0f;
    hd.destroyable = true;
    hd.targetable = true;
    hd.projectile = 0;
    hd.randomize_between_fire_bones = false;
    hd.min_recharge = hd.max_recharge = 1.0f;
    hd.fire_range = 1000.0f;
    hd.repair_amount_per_frame = 10.0f;
    w.data.hardpoints.push_back(hd);
    UnitDesc shooter;
    shooter.tactical_health = 100.0f;
    shooter.max_attack_distance = 1000.0f;
    shooter.hardpoints = {0};
    UnitDesc station = shooter;
    station.repairs_hardpoints = true;
    UnitDesc target;
    target.tactical_health = 1e6f;
    target.radius = 10.0f;
    const uint32_t ds = push_unit(w.data, shooter), dst = push_unit(w.data, station), dt = push_unit(w.data, target);
    CombatConfig cfg;
    cfg.object_max_health_multiplier = 1.0f;
    cfg.hardpoint_health_multiplier = 1.0f;
    w.make(cfg);
    w.add(1, ds, 0, Vec3{0, 0, 0});
    w.add(2, dt, 1, Vec3{500, 0, 0});
    w.add(3, dst, 0, Vec3{0, 500, 0});
    CHECK(!w.sys->can_repair_hardpoints(1) && w.sys->can_repair_hardpoints(3) && !w.sys->can_repair_hardpoints(99));
    CHECK(!w.sys->hardpoint_disabled(1, 0) && !w.sys->hardpoint_disabled(1, 5) && !w.sys->hardpoint_disabled(99, 0));
    w.sys->set_hardpoint_disabled(1, 0, true);
    w.sys->set_hardpoint_disabled(3, 0, true);
    CHECK(w.sys->hardpoint_disabled(1, 0));
    w.sys->order_attack(1, 2);
    const int32_t cd0 = w.sys->hardpoint_cooldown(1, 0);
    for (int i = 0; i < 60; ++i) w.tick();
    CHECK(w.sys->projectile_count() == 0);              // disabled: never fires
    CHECK(w.sys->hardpoint_cooldown(1, 0) == cd0);      // and does not count down
    // Repair: below max nothing changes; at max the flag clears and the weapon fires again.
    w.sys->set_hardpoint_health(1, 0, 85.0f);
    CHECK(w.sys->add_hardpoint_repairer(1, 0, 0) && w.sys->hardpoint_repaired_by(1, 0, 0) &&
          !w.sys->hardpoint_repaired_by(1, 0, 1));
    w.tick();  // 95
    CHECK(w.sys->hardpoint_disabled(1, 0) && w.sys->hardpoint_repaired_by(1, 0, 0));
    w.tick();  // 100: list cleared, flag cleared
    CHECK(!w.sys->hardpoint_disabled(1, 0) && !w.sys->hardpoint_repaired_by(1, 0, 0));
    CHECK(w.sys->hardpoint_disabled(3, 0));  // untouched
    for (int i = 0; i < 60 && w.sys->projectile_count() == 0; ++i) w.tick();
    CHECK(w.sys->projectile_count() > 0);
}
}  // namespace bo3split

namespace bo3split {

// HARDPOINT_DISABLED.md §3-§5: station-upgrade copy, functioning counts, offline effects of a disabled hardpoint,
// the per-player special-weapon flag (Allows_Special_Weapon_Use), re-enable on repair to max.
void test_station_upgrade_copy() {
    World w(3);
    HardpointDesc wpn;
    wpn.kind = HardpointKind::WeaponLaser;
    wpn.health = 100.0f;
    wpn.destroyable = wpn.targetable = true;
    wpn.min_recharge = wpn.max_recharge = 1.0f;
    wpn.fire_range = 1000.0f;
    wpn.repair_amount_per_frame = 10.0f;
    HardpointDesc sw = wpn;
    sw.kind = HardpointKind::WeaponSpecial;
    sw.allows_special_weapon_use = true;
    HardpointDesc gen = wpn;
    gen.kind = HardpointKind::ShieldGenerator;
    HardpointDesc fixed = wpn;  // not destroyable: the 0.1 health write is ignored
    fixed.destroyable = false;
    w.data.hardpoints = {wpn, sw, gen, fixed};
    UnitDesc st1;
    st1.tactical_health = 1000.0f;
    st1.shield_points = 500.0f;
    st1.hardpoints = {0, 0, 0, 1, 3, 0};  // 6: one more than the new station (ignored)
    UnitDesc st2 = st1;
    st2.hardpoints = {0, 0, 0, 1, 3};
    UnitDesc gens = st1;
    gens.hardpoints = {2, 0};
    const uint32_t d1 = push_unit(w.data, st1), d2 = push_unit(w.data, st2), dg = push_unit(w.data, gens);
    CombatConfig cfg;
    cfg.object_max_health_multiplier = 1.0f;
    cfg.hardpoint_health_multiplier = 1.0f;
    w.make(cfg, {PlayerInfo{}, PlayerInfo{}, PlayerInfo{}});
    w.sys->set_enemy(0, 2, false);  // player 2 allied to 0
    w.sys->set_enemy(2, 0, false);
    w.add(1, d1, 0, Vec3{0, 0, 0});
    w.add(3, dg, 0, Vec3{0, 3000, 0});
    for (int32_t p = 0; p < 3; ++p) CHECK(w.sys->special_weapon_hp_enabled(p));
    CHECK(w.sys->special_weapon_hp_enabled(-1) && w.sys->special_weapon_hp_enabled(7));

    // Destroying an Allows_Special_Weapon_Use hardpoint: Set_System_Online(0, destroyed) -> owner + allies := 0.
    DamageRequest r;
    r.victim = 1;
    r.amount = 1000.0f;
    r.hardpoint = 3;
    w.sys->take_damage(r, w.frame);
    CHECK(w.sys->hardpoint_destroyed(1, 3) && !w.sys->hardpoint_disabled(1, 3));  // destruction does not disable
    CHECK(!w.sys->special_weapon_hp_enabled(0) && w.sys->special_weapon_hp_enabled(1) && !w.sys->special_weapon_hp_enabled(2));
    // Old station: [0] damaged and being repaired by 0 then 2, [1] destroyed, [2] disabled, [3] SW destroyed,
    // [4] non-destroyable disabled, [5] beyond the new list.
    w.sys->set_hardpoint_health(1, 0, 40.0f);
    CHECK(w.sys->add_hardpoint_repairer(1, 0, 0) && w.sys->add_hardpoint_repairer(1, 0, 2));
    r.hardpoint = 1;
    w.sys->take_damage(r, w.frame);
    w.sys->set_hardpoint_disabled(1, 2, true);
    w.sys->set_hardpoint_disabled(1, 4, true);
    CHECK(w.sys->hardpoint_disabled(1, 2) && w.sys->hardpoint_health(1, 2) == 100.0f);  // keeps its health
    CHECK(w.sys->live_hardpoints(1, HardpointKind::WeaponLaser) == 2);                // [0] and [5]

    w.add(4, d2, 0, Vec3{0, 0, 0});  // Next_Level_Base, spawned by the host
    CHECK(w.sys->special_weapon_hp_enabled(0) == false);
    CHECK(!w.sys->copy_upgrade_hardpoints(1, 99) && !w.sys->copy_upgrade_hardpoints(99, 4));
    CHECK(w.sys->copy_upgrade_hardpoints(1, 4));
    // [0] being repaired: disabled, old health, repairers re-added in list order.
    CHECK(w.sys->hardpoint_disabled(4, 0) && w.sys->hardpoint_health(4, 0) == 40.0f);
    CHECK(w.sys->hardpoint_repairers(4, 0) == 2 && w.sys->hardpoint_repaired_by(4, 0, 0) && w.sys->hardpoint_repaired_by(4, 0, 2));
    // [1] destroyed / [2] disabled: disabled at 0.1; [3] SW destroyed: disabled at 0.1, flag := 0 for allies.
    for (uint32_t i = 1; i <= 3; ++i) {
        CHECK(w.sys->hardpoint_disabled(4, i) && !w.sys->hardpoint_destroyed(4, i));
        CHECK_F(w.sys->hardpoint_health(4, i), 0.1f);
    }
    CHECK(!w.sys->special_weapon_hp_enabled(0) && w.sys->special_weapon_hp_enabled(1) && !w.sys->special_weapon_hp_enabled(2));
    // [4] non-destroyable: disabled, health untouched.
    CHECK(w.sys->hardpoint_disabled(4, 4) && w.sys->hardpoint_health(4, 4) == 100.0f);
    CHECK(w.sys->live_hardpoints(4, HardpointKind::WeaponLaser) == 0 && w.sys->live_hardpoints(4, HardpointKind::WeaponSpecial) == 0);
    CHECK(!w.sys->can_attack(4) && w.sys->special_weapon_readiness(4) == 0.0f);

    // Repair to max re-enables (clear + Set_System_Online(1)): [0] from 40 at 10 / frame / repairer (2 repairers).
    w.sys->remove_unit(1);
    w.tick();  // 60
    w.tick();  // 80
    CHECK(w.sys->hardpoint_disabled(4, 0));
    w.tick();  // 100
    CHECK(!w.sys->hardpoint_disabled(4, 0) && w.sys->hardpoint_repairers(4, 0) == 0);
    CHECK(w.sys->live_hardpoints(4, HardpointKind::WeaponLaser) == 1 && w.sys->can_attack(4));
    // The SW hardpoint: repair from 0.1 -> flag back to 1 for the owner and allies when it reaches max.
    CHECK(w.sys->add_hardpoint_repairer(4, 3, 1));  // any player may pay (no ownership check)
    for (int i = 0; i < 9; ++i) w.tick();
    CHECK(w.sys->hardpoint_disabled(4, 3) && !w.sys->special_weapon_hp_enabled(0));
    w.tick();
    CHECK(!w.sys->hardpoint_disabled(4, 3) && w.sys->special_weapon_hp_enabled(0) && w.sys->special_weapon_hp_enabled(2));
    CHECK(w.sys->live_hardpoints(4, HardpointKind::WeaponSpecial) == 1);

    // Disabling the only shield generator runs the offline effects (shield zeroed, last of kind).
    CHECK(w.sys->shield(3) > 0.0f);
    w.sys->set_hardpoint_disabled(3, 0, true);
    CHECK(w.sys->shield(3) == 0.0f && w.sys->live_hardpoints(3, HardpointKind::ShieldGenerator) == 0);
    // A disabled hardpoint can still be shot down; it then stays destroyed and disabled (repair refused).
    r.victim = 3;
    r.hardpoint = 0;
    w.sys->take_damage(r, w.frame);
    CHECK(w.sys->hardpoint_destroyed(3, 0) && w.sys->hardpoint_disabled(3, 0));
    CHECK(w.sys->add_hardpoint_repairer(3, 0, 0));
    w.tick();
    CHECK(w.sys->hardpoint_repairers(3, 0) == 0 && w.sys->hardpoint_disabled(3, 0));
}
}  // namespace bo3split

namespace bo3split {

void test_modifier_cache() {
    World w;
    UnitDesc u;
    u.tactical_health = 100.0f;
    u.max_attack_distance = 800.0f;
    u.weapon_projectile = 0;
    w.data.projectiles.push_back(ProjectileDesc{});
    const uint32_t du = push_unit(w.data, u);
    w.make();
    w.add(1, du, 0, Vec3{});
    CHECK_F(w.sys->attack_range(1), 800.0f);
    w.sys->player_modifiers(0).insert(ModList::FireRange, 1, 0.25f, kPermanent);
    CHECK_F(w.sys->modifier(1, ModList::FireRange), 0.25f);  // player-set change invalidates the cache
    CHECK_F(w.sys->attack_range(1), 1000.0f);
    w.sys->unit_modifiers(1)->insert(ModList::FireRange, 2, 0.25f, expiry_frame(w.frame, 1.0f, 30));
    CHECK_F(w.sys->attack_range(1), 1200.0f);
    for (int i = 0; i < 32; ++i) w.tick();  // expiry frame 31 < frame 32: purged in that object update
    CHECK_F(w.sys->modifier(1, ModList::FireRange), 0.25f);
    CHECK_F(w.sys->max_health(1), 100.0f * 1.5f);
    w.sys->unit_modifiers(1)->insert(ModList::MaxHealth, 1, 0.2f, kPermanent);
    w.tick();
    CHECK_F(w.sys->max_health(1), 100.0f * 1.5f * 1.0f * 1.2f);
}
}  // namespace bo3split

namespace bo3split {

void test_battle(const char* golden, const char* write_golden) {
    const uint32_t kTicks = 900;
    Battle a;
    const CrcLog serial = a.run(kTicks, serial_for);
    const CombatStats& st = a.w.sys->stats();
    std::printf("battle: %u ticks, scans %llu, shots %llu, hits %llu, expired %llu, deaths %llu, resweeps %llu, units left %zu\n",
                kTicks, static_cast<unsigned long long>(st.scans), static_cast<unsigned long long>(st.shots),
                static_cast<unsigned long long>(st.hits), static_cast<unsigned long long>(st.expired),
                static_cast<unsigned long long>(st.deaths), static_cast<unsigned long long>(st.resweeps), a.w.sys->unit_count());
    CHECK(st.shots > 100);
    CHECK(a.shot_events == st.shots && a.bad_shot_events == 0);  // OI fog
    CHECK(st.hits > 50);
    CHECK(st.deaths > 0);
    CHECK(a.w.sys->unit_count() < 20);

    const unsigned hw = std::max(2u, std::thread::hardware_concurrency());
    for (unsigned threads : {1u, 4u, hw}) {
        Battle b;
        const CrcLog par = b.run(kTicks, alamo::test::make_thread_parallel_for(threads));
        const CrcDivergence dv = first_divergence(serial, par);
        if (dv.diverged) std::printf("threads %u: %s\n", threads, dv.message.c_str());
        CHECK(!dv.diverged);
    }
    if (write_golden) {
        std::string err;
        CHECK(serial.save(write_golden, &err));
        std::printf("golden written: %s\n", write_golden);
    }
    if (golden) {
        CrcLog g;
        std::string err;
        if (!CrcLog::load(golden, g, &err)) {
            std::printf("FAIL golden %s: %s (generate with --write-golden)\n", golden, err.c_str());
            ++g_fail;
        } else {
            const CrcDivergence dv = first_divergence(g, serial);
            if (dv.diverged) std::printf("golden: %s\n", dv.message.c_str());
            CHECK(!dv.diverged);
        }
    }
    std::printf("battle final combined crc %016llx\n",
                static_cast<unsigned long long>(serial.records.empty() ? 0 : serial.records.back().combined));
}
}  // namespace bo3split

namespace bo3split {

void test_allocations() {
    Battle b;
    b.run(60, serial_for, false);  // warm-up (first queries, tree rebuild scratch, contexts)
    uint64_t in_script = 0, in_tick = 0, in_trees = 0;
    const ParallelFor pf = serial_for;
    for (uint32_t i = 0; i < 540; ++i) {
        const uint32_t f = b.w.frame;
        g_allocs = 0;
        g_count = true;
        b.script(f);  // host side: positions, tree refit / maintain
        g_count = false;
        in_script += g_allocs.load();
        g_allocs = 0;
        g_count = true;
        b.w.sys->tick(f, b.w.trees, b.w.rng, pf);
        g_count = false;
        if (g_allocs.load()) std::printf("  tick %u: %llu allocation(s)\n", f, static_cast<unsigned long long>(g_allocs.load()));
        in_tick += g_allocs.load();
        g_allocs = 0;
        g_count = true;
        for (const DeathEvent& d : b.w.sys->deaths()) b.w.trees.remove(static_cast<EntityHandle>(d.id));
        g_count = false;
        in_trees += g_allocs.load();
        ++b.w.frame;
    }
    std::printf("allocations in 540 steady-state ticks: combat tick %llu (host script %llu, tree removals %llu)\n",
                static_cast<unsigned long long>(in_tick), static_cast<unsigned long long>(in_script),
                static_cast<unsigned long long>(in_trees));
    CHECK(in_tick == 0);
    CHECK(in_trees == 0);  // OI tactical: TreeSet::remove no longer allocates (SpatialTree free list pre-sized)
}
}  // namespace bo3split

namespace bo3split {

// ================================================================================================
// N1 stage 2: save / load in the middle of a battle (CombatSystem / ModifierSet / WeatherSystem write + read)
// ================================================================================================

// Every combat hash (incl. the S9 extension) and the synced RNG.
uint64_t combat_digest(const CombatSystem& s, const SimRng& rng) {
    StateHasher h;
    s.hash_units(h);
    s.hash_projectiles(h);
    s.hash_modifiers(h);
    s.hash_delayed(h);
    s.hash_extended(h);
    rng.hash(h);
    return h.digest();
}
}  // namespace bo3split

namespace bo3split {

// A fresh battle `b` (built exactly like the saved one) takes the saved combat state, RNG and frame. The host side
// (trees) drops the units the saved run removed; positions are pushed by script() before every tick, as in the run.
bool load_battle(Battle& b, const std::vector<uint8_t>& combat, const std::vector<uint8_t>& rng, uint32_t frame) {
    ByteReader r(combat);
    if (!b.w.sys->read(r)) return false;
    ByteReader rr(rng);
    if (!b.w.rng.read(rr)) return false;
    b.w.frame = frame;
    for (const Battle::Mover& m : b.movers)
        if (!b.w.sys->has_unit(m.id)) b.w.trees.remove(static_cast<EntityHandle>(m.id));
    return true;
}
}  // namespace bo3split

namespace bo3split {

void test_save_load() {
    const uint32_t kBefore = 330, kAfter = 420;
    Battle a;
    a.run(kBefore, serial_for, false);
    // Mid-battle: projectiles in flight, deaths, the timed fire-rate modifier (frame 100, 10 s) and POWER_TO_WEAPONS.
    CHECK(a.w.sys->projectile_count() > 0);
    CHECK(a.w.sys->unit_count() > 0 && a.w.sys->unit_count() < 20);
    if (a.w.sys->has_unit(1)) CHECK(a.w.sys->ability_active(1, 0));
    const std::vector<uint8_t> saved = combat_bytes(*a.w.sys);
    ByteWriter rng_w;
    a.w.rng.write(rng_w);

    Battle b;
    CHECK(load_battle(b, saved, rng_w.bytes(), a.w.frame));
    CHECK(combat_bytes(*b.w.sys) == saved);  // write -> read -> write: identical bytes
    CHECK(combat_digest(*a.w.sys, a.w.rng) == combat_digest(*b.w.sys, b.w.rng));
    CHECK(a.registry.capture(a.w.frame).combined == b.registry.capture(b.w.frame).combined);
    CHECK(a.w.sys->modifier(11, ModList::FireRate) == b.w.sys->modifier(11, ModList::FireRate));

    // Both continue (the loaded one on 4 threads): identical state every tick.
    const ParallelFor pf4 = alamo::test::make_thread_parallel_for(4);
    uint32_t diverged = 0, shots = 0, deaths = 0;
    for (uint32_t i = 0; i < kAfter; ++i) {
        const uint32_t f = a.w.frame;
        a.script(f);
        a.w.tick();
        b.script(f);
        b.w.tick(pf4);
        shots += static_cast<uint32_t>(a.w.sys->shot_events().size());
        deaths += static_cast<uint32_t>(a.w.sys->deaths().size());
        if (combat_digest(*a.w.sys, a.w.rng) != combat_digest(*b.w.sys, b.w.rng) ||
            a.w.sys->deaths().size() != b.w.sys->deaths().size() ||
            a.w.sys->damage_events().size() != b.w.sys->damage_events().size()) {
            if (diverged == 0) std::printf("save/load: diverged at frame %u\n", f);
            ++diverged;
        }
    }
    CHECK(diverged == 0);
    CHECK(shots > 0 && deaths > 0);  // the continuation really fought
    CHECK(combat_bytes(*a.w.sys) == combat_bytes(*b.w.sys));
    std::printf("combat save: %zu bytes at frame %u, %u ticks continued identically (%u shots, %u deaths)\n", saved.size(),
                kBefore, kAfter, shots, deaths);
}
}  // namespace bo3split

namespace bo3split {

void put_u32_at(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
}
}  // namespace bo3split

namespace bo3split {

void test_save_rejects() {
    Battle a;
    a.run(200, serial_for, false);
    const std::vector<uint8_t> saved = combat_bytes(*a.w.sys);
    Battle c;
    c.run(20, serial_for, false);
    const std::vector<uint8_t> before = combat_bytes(*c.w.sys);
    const auto rejected = [&c](const std::vector<uint8_t>& bytes) {
        ByteReader r(bytes);
        return !c.w.sys->read(r);
    };
    // truncated anywhere
    uint32_t cuts = 0, accepted = 0;
    for (size_t len = 0; len < saved.size(); len += (len < 256 ? 1 : 61)) {
        ++cuts;
        if (!rejected(std::vector<uint8_t>(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(len)))) ++accepted;
    }
    CHECK(rejected(std::vector<uint8_t>(saved.begin(), saved.end() - 1)));
    CHECK(cuts > 100 && accepted == 0);
    // trailing data, version, definition counts
    std::vector<uint8_t> bad = saved;
    bad.push_back(0);
    CHECK(rejected(bad));
    bad = saved;
    put_u32_at(bad, 0, 99);
    CHECK(rejected(bad));
    for (const uint32_t v : {14u, 16u}) {  // only the current version (15: s78n t12 / t11) is read (cu6)
        bad = saved;
        put_u32_at(bad, 0, v);
        CHECK(rejected(bad));
    }
    bad = saved;
    put_u32_at(bad, 4, 3);  // player count
    CHECK(rejected(bad));
    bad = saved;
    put_u32_at(bad, 8, 7);  // unit descriptor count
    CHECK(rejected(bad));
    // the first unit's descriptor index out of range: header (version, 6 counts, frame, serial), relations P x P,
    // special-weapon flags P, the player modifier sets (version, count, 21 bytes per entry), weather row, unit count, id
    size_t off = 4 + 24 + 8 + 4 + 2;
    for (int32_t p = 0; p < 2; ++p) off += 8 + 21 * a.w.sys->player_modifiers(p).entries().size();
    off += 40 + 4 + 4;
    bad = saved;
    put_u32_at(bad, off, 1000);
    CHECK(rejected(bad));
    bad = saved;
    // a valid descriptor with another hardpoint count (destroyer <-> fighter)
    put_u32_at(bad, off, a.w.sys->desc_index(a.w.sys->unit_id_at(0)) == 0 ? 1u : 0u);
    CHECK(rejected(bad));
    CHECK(combat_bytes(*c.w.sys) == before);  // every rejected read left the system unchanged
    ByteReader ok(saved);
    CHECK(c.w.sys->read(ok));  // and the intact bytes still load
    CHECK(combat_bytes(*c.w.sys) == saved);
}
}  // namespace bo3split

namespace bo3split {

// cu8f t04a (CU8F_T04_T06_RE §1): the targeting stickiness threshold TD +0x158. Spawn value 0.0 (not the type's value);
// a negative override reads the type's Targeting_Stickiness_Time_Threshold at every test (a TEAM owner: its first member's
// type); a task-force release writes the type's value. Time to kill = the destroyable hard points' health when the target
// has any (destroyed ones at 0), else the hull, + shield, / the damage rate (1e18 at rate 0); a TEAM container target sums
// its members'. Sticky <=> time to kill <= threshold. Saved (combat v13; only the current version is read), hashed when non-zero.
void add_member(World& w, ObjectId id, uint32_t desc, int32_t owner, Vec3 p, uint32_t team) {
    UnitSpawn s;
    s.id = id;
    s.desc = desc;
    s.owner = owner;
    s.position = p;
    s.team = team;
    CHECK(w.sys->add_unit(s, w.frame, w.rng));
    TreeMembership m;
    m.owner = owner;
    CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(p, w.data.units[desc].radius), 1u, m));
}
}  // namespace bo3split

namespace bo3split {

bool near_rel(float a, float b) { return std::fabs(a - b) <= 1e-5f * std::fabs(b); }
}  // namespace bo3split

namespace bo3split {

void test_stickiness_override() {
    World w;
    HardpointDesc gen;
    gen.name = "HP_Gen";
    gen.name_crc = name_crc(gen.name);
    gen.kind = HardpointKind::ShieldGenerator;
    gen.health = 10.0f;
    gen.destroyable = gen.targetable = true;
    w.data.hardpoints.push_back(gen);
    UnitDesc a;
    a.name = "Sticky5";
    a.tactical_health = 100.0f;
    a.radius = 10.0f;
    a.stickiness_threshold = 5.0f;
    const uint32_t d5 = push_unit(w.data, a);
    a.name = "Sticky025";
    a.stickiness_threshold = 0.25f;
    const uint32_t d025 = push_unit(w.data, a);
    a.name = "Never";
    a.stickiness_threshold = -1.0f;
    const uint32_t dneg = push_unit(w.data, a);
    UnitDesc t;
    t.name = "Target";
    t.tactical_health = 100.0f;
    t.radius = 10.0f;
    const uint32_t dt = push_unit(w.data, t);
    t.name = "Gunship";
    t.tactical_health = 1000.0f;
    t.hardpoints = {0};
    const uint32_t dh = push_unit(w.data, t);
    CombatConfig cfg;
    cfg.hardpoint_health_multiplier = 1.0f;
    w.make(cfg);
    w.add(1, d5, 0, Vec3{0, 0, 0});
    w.add(2, dt, 1, Vec3{500, 0, 0});
    w.add(3, dh, 1, Vec3{600, 0, 0});
    w.add(4, d025, 0, Vec3{0, 100, 0});
    w.add(5, dneg, 0, Vec3{0, -100, 0});
    add_member(w, 10, dt, 1, Vec3{500, 300, 0}, 0);   // a TEAM container that is itself a combat unit
    add_member(w, 11, dt, 1, Vec3{500, 320, 0}, 10);  // its members
    add_member(w, 12, dt, 1, Vec3{500, 340, 0}, 10);
    add_member(w, 20, d025, 0, Vec3{0, 300, 0}, 0);   // an attacking container (own type 0.25 s)
    add_member(w, 21, d5, 0, Vec3{0, 320, 0}, 20);    // its first member (5 s)
    w.tick();  // phase A: the team member list

    // Spawn value 0.0, not the type's 5 s; no damage yet: time to kill 1e18 (rate 0), sticky only for a threshold >= 1e18.
    CHECK(w.sys->stickiness_override(1) == 0.0f && w.sys->stickiness_override(99) == 0.0f);
    CHECK(w.sys->time_to_kill(2) == 1.0e18f && !w.sys->target_sticky(1, 2));
    CHECK(w.sys->set_stickiness_override(1, 1.0e30f) && w.sys->target_sticky(1, 2));
    CHECK(!w.sys->set_stickiness_override(99, 1.0f) && !w.sys->target_sticky(99, 2) && !w.sys->target_sticky(1, 99));

    // 90 hull damage: rate = 90 / 5 per second, time to kill = hull / rate (~0.56 s).
    DamageRequest d;
    d.victim = 2;
    d.amount = 90.0f;
    const DamageResult r2 = w.sys->take_damage(d, w.frame);
    w.tick();
    const float ttk2 = w.sys->time_to_kill(2);
    std::printf("stickiness: target hull %.3f, tracked %.3f, time to kill %.4f s\n", static_cast<double>(w.sys->health(2)),
                static_cast<double>(r2.absorbed + r2.to_hull), static_cast<double>(ttk2));
    CHECK(near_rel(ttk2, (w.sys->health(2) + w.sys->shield(2)) / ((r2.absorbed + r2.to_hull) / 5.0f)));
    CHECK(ttk2 > 0.25f && ttk2 < 5.0f);  // between the two type values below
    const auto sticky_at = [&w](ObjectId u, float v, ObjectId target) {
        CHECK(w.sys->set_stickiness_override(u, v));
        return w.sys->target_sticky(u, target);
    };
    CHECK(!sticky_at(1, 0.0f, 2));  // the spawn value: nothing with a positive time to kill is sticky
    CHECK(sticky_at(1, ttk2 * 1.5f, 2) && !sticky_at(1, ttk2 * 0.5f, 2) && sticky_at(1, ttk2, 2));
    // A negative override reads the type's value at every test: 5 s sticky, 0.25 s not, a negative type value never.
    CHECK(sticky_at(1, -1.0f, 2) && !sticky_at(4, -2.0f, 2) && !sticky_at(5, -1.0f, 2));
    CHECK(w.sys->stickiness_override(1) == -1.0f);
    // TEAM owner: the first member's type (5 s), not the container's own (0.25 s).
    CHECK(sticky_at(20, -1.0f, 2) && !sticky_at(21, 0.25f, 2));
    // Task-force release: the type's value (a TEAM owner: its first member's type).
    CHECK(w.sys->reset_stickiness(1) && w.sys->stickiness_override(1) == 5.0f);
    CHECK(w.sys->reset_stickiness(4) && w.sys->stickiness_override(4) == 0.25f);
    CHECK(w.sys->reset_stickiness(5) && w.sys->stickiness_override(5) == -1.0f && !w.sys->target_sticky(5, 2));
    CHECK(w.sys->reset_stickiness(20) && w.sys->stickiness_override(20) == 5.0f);
    CHECK(!w.sys->reset_stickiness(99));

    // Destroyable hard points: their health (not the hull's) whenever the target has any.
    DamageRequest h;
    h.victim = 3;
    h.amount = 900.0f;
    const DamageResult r3 = w.sys->take_damage(h, w.frame);
    w.tick();
    const float ttk3 = w.sys->time_to_kill(3);
    const float rate3 = (r3.absorbed + r3.to_hull) / 5.0f;
    std::printf("stickiness: gunship hull %.1f, hard point %.1f, time to kill %.4f s (hull would give %.4f s)\n",
                static_cast<double>(w.sys->health(3)), static_cast<double>(w.sys->hardpoint_health(3, 0)),
                static_cast<double>(ttk3), static_cast<double>(w.sys->health(3) / rate3));
    CHECK(near_rel(ttk3, (std::max(0.0f, w.sys->hardpoint_health(3, 0)) + w.sys->shield(3)) / rate3));
    CHECK(ttk3 < w.sys->health(3) / rate3);
    CHECK(!sticky_at(1, 0.0f, 3));
    DamageRequest k;  // the only destroyable hard point destroyed: health sum 0 -> time to kill 0, sticky at 0.0
    k.victim = 3;
    k.amount = 1000.0f;
    k.hardpoint = 0;
    w.sys->take_damage(k, w.frame);
    w.tick();
    CHECK(w.sys->alive(3) && w.sys->hardpoint_health(3, 0) <= 0.0f && w.sys->time_to_kill(3) == 0.0f);
    CHECK(sticky_at(1, 0.0f, 3));

    // A TEAM container target: the sum of its members' own times (ascending id), not its own.
    DamageRequest m1;
    m1.victim = 11;
    m1.amount = 90.0f;
    m1.no_team = true;  // the member itself
    w.sys->take_damage(m1, w.frame);
    m1.victim = 12;
    m1.amount = 50.0f;
    w.sys->take_damage(m1, w.frame);
    w.tick();
    const float t11 = w.sys->time_to_kill(11), t12 = w.sys->time_to_kill(12);
    CHECK(t11 > 0.0f && t11 < t12 && t12 < 1.0e17f);
    CHECK(w.sys->time_to_kill(10) == t11 + t12);
    CHECK(sticky_at(1, t11 + t12, 10) && !sticky_at(1, t12, 10));

    // Hash: the field enters hash_extended only when non-zero.
    {
        StateHasher a0, a1;
        CHECK(w.sys->set_stickiness_override(4, 0.0f));
        w.sys->hash_extended(a0);
        CHECK(w.sys->set_stickiness_override(4, 0.25f));
        w.sys->hash_extended(a1);
        CHECK(a0.digest() != a1.digest());
    }

    // Save: write -> read -> write identical, the values restored; a v12 image (no field) is refused (cu6).
    {
        CHECK(w.sys->set_stickiness_override(1, 2.5f));
        const std::vector<uint8_t> bytes = combat_bytes(*w.sys);
        World b;
        b.data = w.data;
        b.make(cfg);
        ByteReader rd(bytes);
        CHECK(b.sys->read(rd));
        CHECK(b.sys->stickiness_override(1) == 2.5f && b.sys->stickiness_override(4) == 0.25f &&
              b.sys->stickiness_override(5) == -1.0f && combat_bytes(*b.sys) == bytes);
    }
    {
        World o;
        o.data = w.data;
        o.make(cfg);
        o.add(1, d5, 0, Vec3{0, 0, 0});
        o.tick();
        const float mark = 1234.5f;  // a pattern found once in the image
        CHECK(o.sys->set_stickiness_override(1, mark));
        std::vector<uint8_t> img = combat_bytes(*o.sys);
        uint32_t bits = 0;
        std::memcpy(&bits, &mark, 4);
        const uint8_t pat[4] = {static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8), static_cast<uint8_t>(bits >> 16),
                                static_cast<uint8_t>(bits >> 24)};
        std::vector<size_t> at;
        for (size_t i = 0; i + 4 <= img.size(); ++i)
            if (std::memcmp(&img[i], pat, 4) == 0) at.push_back(i);
        CHECK(at.size() == 1u && img[0] == 15u);
        if (at.size() == 1u) {
            img.erase(img.begin() + static_cast<std::ptrdiff_t>(at[0]), img.begin() + static_cast<std::ptrdiff_t>(at[0]) + 4);
            img[0] = 13;
            World b;
            b.data = w.data;
            b.make(cfg);
            ByteReader rd(img);
            CHECK(!b.sys->read(rd));
        }
    }
}
}  // namespace bo3split

namespace bo3split {

void test_weather_save() {
    WeatherData data;
    WeatherScenario other;
    other.name = "Calm";
    other.phases.resize(1);
    WeatherScenario storm;
    storm.name = "Storm";
    storm.phases.resize(3);
    storm.phases[0] = WeatherPhase{0.2f, 0.0f, 0.1f, WeatherType::Clear, 4, 2};
    storm.phases[1] = WeatherPhase{0.9f, 0.5f, 0.6f, WeatherType::Rain, 5, 3};
    storm.phases[2] = WeatherPhase{0.7f, 0.2f, 0.3f, WeatherType::Sandstorm, 3, 2};
    data.scenarios = {other, storm};
    data.modifiers[1].accuracy[0] = 0.5f;
    data.modifiers[3].accuracy[7] = 0.25f;

    WeatherSystem a;
    SimRng ra{77u};
    a.start(&data.scenarios[1], &data, 1, ra);
    for (uint32_t f = 2; f <= 333; ++f) {
        a.clear_events();
        a.service(f, ra);
    }
    ByteWriter wa;
    a.write(wa);
    const std::vector<uint8_t> saved = wa.bytes();

    WeatherSystem b;
    SimRng rb{77u};
    b.start(&data.scenarios[1], &data, 1, rb);
    ByteReader r(saved);
    CHECK(b.read(r));
    ByteWriter rng_w;
    ra.write(rng_w);
    ByteReader rng_r(rng_w.bytes());
    CHECK(rb.read(rng_r));
    ByteWriter wb;
    b.write(wb);
    CHECK(wb.bytes() == saved);
    const auto digest = [](const WeatherSystem& s) {
        StateHasher h;
        s.hash(h);
        return h.digest();
    };
    CHECK(digest(a) == digest(b) && a.events().size() == b.events().size());
    uint32_t diverged = 0, advances = 0;
    for (uint32_t f = 334; f <= 1500; ++f) {
        a.clear_events();
        b.clear_events();
        const uint32_t phase = a.phase_index();
        a.service(f, ra);
        b.service(f, rb);
        if (a.phase_index() != phase) ++advances;
        if (digest(a) != digest(b) || a.accuracy_row() != b.accuracy_row() || ra.state() != rb.state() ||
            a.events().size() != b.events().size())
            ++diverged;
    }
    CHECK(diverged == 0 && advances > 2);

    // rejects: truncation, an unknown scenario (other data), a phase index out of range; the system stays unchanged
    WeatherSystem c;
    SimRng rc{5u};
    c.start(&data.scenarios[0], &data, 1, rc);
    ByteWriter wc0;
    c.write(wc0);
    uint32_t accepted = 0;
    for (size_t len = 0; len < saved.size(); ++len) {
        ByteReader rt(std::span<const uint8_t>(saved.data(), len));
        if (c.read(rt)) ++accepted;
    }
    CHECK(accepted == 0);
    WeatherData renamed = data;
    renamed.scenarios[1].name = "Hail";
    WeatherSystem d;
    SimRng rd{5u};
    d.start(&renamed.scenarios[1], &renamed, 1, rd);
    ByteWriter wd0;
    d.write(wd0);
    ByteReader rn(saved);
    CHECK(!d.read(rn));
    ByteWriter wd1;
    d.write(wd1);
    CHECK(wd1.bytes() == wd0.bytes());
    ByteWriter wc1;
    c.write(wc1);
    CHECK(wc1.bytes() == wc0.bytes());
}
}  // namespace bo3split

namespace bo3split {

// ================================================================================================
// FoC data (ALAMO_FOC_DATA)
// ================================================================================================

// ================================================================================================
// refixf t01: ROCKET / MPTL_ROCKET spline flight (docs/re/S9_DEATH_CLONE_SHIELDS.md §2.4)
// ================================================================================================

float dist3(Vec3 a, Vec3 b) {
    const Vec3 d = a - b;
    return dmath::sqrt(dot(d, d));
}
}  // namespace bo3split

namespace bo3split {

RocketParams rocket_params(float cd, float co, float sd, float mfd, bool on_terrain, bool explode) {
    RocketParams p;
    p.curve_distance = cd;
    p.curve_offset = co;
    p.straight_distance = sd;
    p.max_flight_distance = mfd;
    p.target_on_terrain = on_terrain;
    p.explode_at_target = explode;
    return p;
}
}  // namespace bo3split

namespace bo3split {

void test_rocket_spline() {
    // §2.4.3: never built -> no sample; fewer than 3 control points -> Build changes nothing.
    RocketSpline s;
    Vec3 out;
    CHECK(!s.sample(0.0f, out));
    s.append({0, 0, 0});
    s.append({10, 0, 0});
    s.build(10);
    CHECK(!s.built() && s.total() == 0.0f && s.polyline().empty());
    s.append({20, 0, 0});
    s.append({30, 0, 0});
    s.build(10);
    // Evenly spaced collinear points: every K is 10, so each segment is the straight line (c = d = 0), length 10.
    CHECK(s.built() && s.segments().size() == 3u);
    for (const RocketSpline::Segment& sg : s.segments()) {
        CHECK_NEAR(sg.length, 10.0, 1e-4);
        CHECK_NEAR(sg.k[1], 10.0, 1e-4);
        CHECK_NEAR(sg.k[2], 0.0, 1e-4);
        CHECK_NEAR(sg.k[3], 0.0, 1e-4);
    }
    CHECK_NEAR(s.total(), 30.0, 1e-4);
    uint32_t seg = 99;
    CHECK(s.sample(15.0f, out, &seg) && seg == 1u);
    CHECK_NEAR(out.x, 15.0, 1e-4);
    CHECK(out.y == 0.0f && out.z == 0.0f);
    CHECK(!s.sample(s.total(), out) && s.sample(29.9f, out));  // s >= total: past the end
    CHECK(s.sample(-2.0f, out, &seg) && seg == 0u);           // negative s: segment 0, u < 0
    CHECK_NEAR(out.x, -2.0, 1e-4);
    CHECK(s.polyline().size() == 4u);  // entry 0 + ceil(10 / 10) per segment
    s.build(3);
    CHECK(s.polyline().size() == 13u);  // entry 0 + ceil(10 / 3) = 4 per segment
    CHECK_NEAR(s.polyline()[5].x, 12.5, 1e-4);
    // Erasing down to 2 points and building again keeps the old coefficients (the truncation quirk of §2.4.7).
    const float total = s.total();
    s.erase_from(2);
    s.build(10);
    CHECK(s.count() == 2u && s.segments().size() == 3u && s.total() == total);

    // Curved data: a = y[i] exactly, interpolation at the next point, C1 joins, natural ends (y'' = 0), arc lengths.
    RocketSpline c;
    const Vec3 pts[] = {{0, 0, 0}, {50, 20, 5}, {100, -10, 10}, {160, 0, 0}, {200, 40, -5}};
    for (const Vec3& p : pts) c.append(p);
    c.build(10);
    CHECK(c.segments().size() == 4u);
    for (uint32_t i = 0; i < 4; ++i) {
        CHECK(same(c.eval(i, 0.0f), pts[i]));
        CHECK(dist3(c.eval(i, 1.0f), pts[i + 1]) < 1e-3f);
        CHECK(c.segments()[i].length >= dist3(pts[i], pts[i + 1]) - 1e-3f);
        if (i < 3) {
            const auto& a = c.segments()[i].k;
            const auto& b = c.segments()[i + 1].k;
            for (size_t ax = 0; ax < 3; ++ax) CHECK_NEAR(a[ax * 4 + 1] + 2.0f * a[ax * 4 + 2] + 3.0f * a[ax * 4 + 3], b[ax * 4 + 1], 1e-2);
        }
    }
    const auto& k0 = c.segments()[0].k;
    const auto& k3 = c.segments()[3].k;
    for (size_t ax = 0; ax < 3; ++ax) {
        CHECK_NEAR(k0[ax * 4 + 2], 0.0, 1e-3);
        CHECK_NEAR(2.0 * k3[ax * 4 + 2] + 6.0 * k3[ax * 4 + 3], 0.0, 1e-2);
    }
    float cum = 0.0f;
    for (const RocketSpline::Segment& sg : c.segments()) cum = cum + sg.length;
    CHECK(cum == c.total());
    CHECK(c.sample(c.segments()[0].length, out, &seg) && seg == 1u && dist3(out, pts[1]) < 1e-3f);
    double fine = 0.0;
    Vec3 prev = c.eval(1, 0.0f);
    for (int j = 1; j <= 4000; ++j) {
        const Vec3 q = c.eval(1, static_cast<float>(j) / 4000.0f);
        fine += static_cast<double>(dist3(q, prev));
        prev = q;
    }
    CHECK_NEAR(c.segments()[1].length, fine, 2e-2);
}
}  // namespace bo3split

namespace bo3split {

void test_rocket_leg() {
    RocketKit k;
    k.rp = rocket_params(50.0f, 10.0f, 0.0f, 1000.0f, false, false);
    const Vec3 from{0, 0, 0}, to{300, 0, 0};
    {  // mode 0: n = ceil(300 / (1.1 x 50)) = 6 points; every point but the last jittered (2 draws each, theta then a)
        RocketSpline p;
        p.append(from);
        const uint64_t d0 = k.rng.draws();
        rocket_leg(p, from, to, 1.0f, 1.1f, 0, false, true, k.ctx(from));
        CHECK(p.count() == 7u && k.rng.draws() - d0 == 10u);
        for (size_t i = 1; i < p.count(); ++i) {
            const Vec3 q = p.points()[i];
            CHECK_NEAR(q.x, 50.0 * static_cast<double>(i), 1e-3);  // jitter is perpendicular to the leg
            CHECK(dmath::sqrt(q.y * q.y + q.z * q.z) <= 10.0f + 1e-4f);  // |offset| = a <= amp x CO
        }
        CHECK(dist3(p.points().back(), to) < 1e-3f);
        // the draw order: replay the first point with a copy of the stream
        SimRng r(4242u);
        const float theta = r.range_float(0.0f, 6.2831855f);
        const float a = r.range_float(0.0f, 10.0f);
        const Vec3 q1 = p.points()[1];
        CHECK_NEAR(dmath::sqrt(q1.y * q1.y + q1.z * q1.z), a, 1e-4);
        (void)theta;
    }
    {  // amp 0 still draws twice per jittered point; the points stay on the line
        RocketSpline p;
        p.append(from);
        const uint64_t d0 = k.rng.draws();
        rocket_leg(p, from, to, 0.0f, 1.1f, 0, false, true, k.ctx(from));
        CHECK(p.count() == 7u && k.rng.draws() - d0 == 10u);
        for (const Vec3& q : p.points()) CHECK(q.y == 0.0f && q.z == 0.0f);
    }
    {  // CD = 0: nothing (retail's (int) of an infinite count is negative), no draws
        RocketKit z;
        z.rp = rocket_params(0.0f, 10.0f, 0.0f, 1000.0f, false, false);
        RocketSpline p;
        p.append(from);
        rocket_leg(p, from, to, 1.0f, 1.1f, 0, false, true, z.ctx(from));
        CHECK(p.count() == 1u && z.rng.draws() == 0u);
    }
    {  // mode 1: box |x| <= 200 - (10 + 1); x = 200, 250, 300 are outside (not appended, no draws); the height clamp
        RocketKit m;
        m.rp = k.rp;
        m.tr.height = [](float x, float) { return 20.0f + 0.1f * x; };
        m.tr.has_box = true;
        m.tr.hx = 200.0f;
        m.tr.hy = 1000.0f;
        const Vec3 l{0, 0, 25};  // launch altitude 5 above the ground
        RocketSpline p;
        p.append(l);
        rocket_leg(p, l, Vec3{300, 0, 25}, 1.0f, 1.1f, 1, true, true, m.ctx(l));
        CHECK(p.count() == 4u && m.rng.draws() == 6u);
        float zp = 25.0f;
        for (size_t i = 1; i < p.count(); ++i) {
            const Vec3 q = p.points()[i];
            CHECK(q.z >= 20.0f + 0.1f * q.x + 5.0f - 1e-4f);
            CHECK(q.z >= 0.7f * zp - 1e-4f);
            zp = q.z;
        }
        // whole leg on the map: the plain last point is not jittered and lands on the ground (groundEnd)
        m.tr.hx = 5000.0f;
        RocketSpline g;
        g.append(l);
        const uint64_t d0 = m.rng.draws();
        rocket_leg(g, l, Vec3{300, 0, 25}, 1.0f, 1.1f, 1, true, true, m.ctx(l));
        CHECK(g.count() == 7u && m.rng.draws() - d0 == 10u);
        CHECK_NEAR(g.points().back().x, 300.0, 1e-3);
        CHECK(g.points().back().z == m.tr.h(g.points().back().x, g.points().back().y));
        // plainEnd off (initial leg 1): the last point is jittered and clamped too
        RocketSpline j;
        j.append(l);
        const uint64_t d1 = m.rng.draws();
        rocket_leg(j, l, Vec3{300, 0, 25}, 1.0f, 1.1f, 1, false, false, m.ctx(l));
        CHECK(j.count() == 7u && m.rng.draws() - d1 == 12u);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_rocket_plans() {
    {  // §2.4.10 step 4 example: Proj_Dark_Trooper_Missile (CD 50, CO 10, MFD 330), land, target 300 away on the map:
       // leg 1 (150) 3 points x 2 draws (last included), leg 2 (~150) 3 points -> 4, leg 3 (~30) 1 point -> 0: 10 draws
        RocketKit k;
        k.rp = rocket_params(50.0f, 10.0f, 0.0f, 330.0f, false, false);
        k.tr.has_box = true;
        k.tr.hx = k.tr.hy = 5000.0f;
        RocketSpline path;
        float budget = 0.0f;
        const Vec3 P{0, 0, 0}, T{300, 0, 0};
        rocket_plan_land(path, P, T, Vec3{1, 0, 0}, budget, k.ctx(P));
        CHECK(k.rng.draws() == 10u);
        CHECK(budget == 330.0f);  // no Explode flag: the budget stays MFD
        CHECK(path.built() && same(path.points().front(), P));
        for (const Vec3& q : path.points()) CHECK(q.z >= 0.0f);  // never below the launch altitude above flat ground
        // Leg 2 ends at T; the truncation at MFD erases the start of S's segment - here T itself - so the final path
        // passes within the jitter of T instead of through it.
        float near_t = 1e9f;
        for (int j = 0; j <= 400; ++j) {
            Vec3 o;
            if (path.sample(path.total() * static_cast<float>(j) / 400.0f, o)) near_t = std::min(near_t, dist3(o, T));
        }
        CHECK(near_t < 12.0f);
        CHECK(path.total() < 330.0f * 1.05f);  // cut at MFD
    }
    {  // Explode_When_Reached_Target_Radius on land: budget = the launch -> target length (Build(10) after leg 2)
        RocketKit k;
        k.rp = rocket_params(50.0f, 10.0f, 0.0f, 330.0f, false, true);
        RocketSpline path;
        float budget = 0.0f;
        rocket_plan_land(path, Vec3{0, 0, 0}, Vec3{300, 0, 0}, Vec3{1, 0, 0}, budget, k.ctx(Vec3{0, 0, 0}));
        CHECK(budget >= 300.0f && budget < 330.0f);
        CHECK(k.rng.draws() == 10u);
    }
    {  // Target_Point_On_Terrain: no leg 3; the path ends on the ground at the target (flat ground at 7 here)
        RocketKit k;
        k.rp = rocket_params(50.0f, 10.0f, 0.0f, 330.0f, true, false);
        k.tr.height = [](float, float) { return 7.0f; };
        RocketSpline path;
        float budget = 0.0f;
        rocket_plan_land(path, Vec3{0, 0, 7}, Vec3{300, 0, 7}, Vec3{1, 0, 0}, budget, k.ctx(Vec3{0, 0, 7}));
        CHECK(k.rng.draws() == 10u);
        CHECK(path.points().back().z == 7.0f && dist3(path.points().back(), Vec3{300, 0, 7}) < 1e-2f);
    }
    {  // space, Explode flag: no leg 1 without Projectile_Rocket_Straight_Distance, leg 2 ceil(300 / 55) = 6 points ->
       // 10 draws, no extension, the budget stays MFD
        RocketKit k;
        k.rp = rocket_params(50.0f, 10.0f, 0.0f, 330.0f, false, true);
        RocketSpline path;
        float budget = 0.0f;
        rocket_plan_space(path, Vec3{0, 0, 0}, Vec3{300, 0, 0}, Vec3{1, 0, 0}, budget, k.ctx(Vec3{0, 0, 0}));
        CHECK(k.rng.draws() == 10u && budget == 330.0f && path.count() == 7u);
        CHECK(dist3(path.points().back(), Vec3{300, 0, 0}) < 1e-3f);
    }
    {  // space leg 1 = min(dh / 2, Straight) along the facing; positive pitch is nose DOWN, so an upward facing climbs
        RocketKit k;
        k.rp = rocket_params(50.0f, 10.0f, 100.0f, 330.0f, false, true);
        RocketSpline path;
        float budget = 0.0f;
        const float h = 0.70710677f;
        rocket_plan_space(path, Vec3{0, 0, 0}, Vec3{300, 0, 0}, Vec3{h, 0, h}, budget, k.ctx(Vec3{0, 0, 0}));
        // leg 1: ceil(100 / 55) = 2 points (2 draws), its end K = P + f x 100 is not jittered
        CHECK(dist3(path.points()[2], Vec3{70.710678f, 0, 70.710678f}) < 1e-2f);
        const uint64_t leg2 = 2u * (static_cast<uint64_t>(std::ceil(dist3(path.points()[2], Vec3{300, 0, 0}) / 55.0f)) - 1u);
        CHECK(k.rng.draws() == 2u + leg2);
    }
    {  // space without the flag: the extension leg runs on along the last polyline step (draws even, path past T)
        RocketKit k;
        k.rp = rocket_params(50.0f, 10.0f, 0.0f, 400.0f, false, false);
        RocketSpline path;
        float budget = 0.0f;
        rocket_plan_space(path, Vec3{0, 0, 0}, Vec3{300, 0, 0}, Vec3{1, 0, 0}, budget, k.ctx(Vec3{0, 0, 0}));
        CHECK(k.rng.draws() > 10u && k.rng.draws() % 2u == 0u);
        CHECK(path.total() > 300.0f && path.total() < 400.0f * 1.05f);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_rocket_replan() {
    RocketKit k;
    k.rp = rocket_params(50.0f, 10.0f, 0.0f, 1000.0f, true, true);
    const Vec3 E{0, 0, 0};
    const Vec3 P{-320, 30, 0};
    RocketSpline old;
    for (int i = 0; i < 5; ++i) old.append(Vec3{-400.0f + 100.0f * static_cast<float>(i), 30.0f, 0.0f});
    old.build(10);
    const auto unchanged = [&old](const RocketSpline& q) { return q.count() == old.count() && q.total() == old.total(); };
    {  // bail-outs: emitter behind, no move yet (D = 0), centre less than ~2k + 0.25 ahead -> nothing changes, no draws
        RocketSpline q = old;
        float s = 200.0f, b = 1000.0f;
        CHECK(!rocket_replan_land(q, P, E, Vec3{-1, 0, 0}, s, b, k.ctx(P)));
        CHECK(!rocket_replan_land(q, P, E, Vec3{0, 0, 0}, s, b, k.ctx(P)));
        CHECK(!rocket_replan_land(q, Vec3{-30, 0, 0}, E, Vec3{1, 0, 0}, s, b, k.ctx(P)));
        CHECK(!rocket_replan_space(q, Vec3{-50, 0, 0}, E, Vec3{1, 0, 0}, s, b, k.ctx(P)));
        CHECK(unchanged(q) && s == 200.0f && b == 1000.0f && k.rng.draws() == 0u);
    }
    const float r = dmath::sqrt(280.0f * 280.0f + 30.0f * 30.0f);  // |B - E|, B = P + 2k D (land k = 20)
    {  // land, Target_Point_On_Terrain: leg 1 is one point (CD 50 >= k) and nothing else draws; the arc lies on the
       // sphere through B, on the side the rocket passes; the path ends 5 below the ground; s and the budget reset
        RocketSpline q = old;
        float s = 200.0f, b = 1000.0f;
        CHECK(rocket_replan_land(q, P, E, Vec3{0.25f, 0, 0}, s, b, k.ctx(P)));
        CHECK(k.rng.draws() == 0u);
        CHECK(s == 0.0f && b == 800.0f);
        CHECK(same(q.points()[0], P) && dist3(q.points()[1], Vec3{-300, 30, 0}) < 1e-3f);  // A = P + k D
        CHECK(q.count() >= 5u);
        for (size_t i = 2; i + 1 < q.count(); ++i) {
            CHECK_NEAR(dist3(q.points()[i], E), r, 1e-2);
            CHECK(q.points()[i].y > 0.0f);
        }
        CHECK(q.points().back().z == -5.0f);
        CHECK(q.built());
        Vec3 o;
        CHECK(q.sample(0.0f, o) && dist3(o, P) < 1e-3f);
    }
    {  // land without it: leg 2 of length (MFD - s0) - 20 in CD steps, 2 draws per point but the last; truncated at
       // MFD - s0 and dropped to the ground (- 5) outside the sphere
        k.rp.target_on_terrain = false;
        RocketSpline q = old;
        float s = 100.0f, b = 1000.0f;
        const uint64_t d0 = k.rng.draws();
        CHECK(rocket_replan_land(q, P, E, Vec3{1, 0, 0}, s, b, k.ctx(P)));
        CHECK(k.rng.draws() - d0 == 2u * (18u - 1u));  // n = ceil(880 / 50) = 18
        CHECK(q.points().back().z == -5.0f);
        CHECK(s == 0.0f && b == 900.0f);
        CHECK(q.total() < 900.0f * 1.05f);
    }
    {  // space (k = 30): leg 2 draws 2 x (n - 1) even with CO = 0; no ground drop; arc on the sphere
        k.rp = rocket_params(50.0f, 0.0f, 0.0f, 1000.0f, false, false);
        RocketSpline q = old;
        float s = 100.0f, b = 1000.0f;
        const uint64_t d0 = k.rng.draws();
        CHECK(rocket_replan_space(q, P, E, Vec3{1, 0, 0}, s, b, k.ctx(P)));
        CHECK(k.rng.draws() - d0 == 2u * (18u - 1u));  // n = ceil(870 / 50) = 18
        CHECK(s == 0.0f && b == 1000.0f);  // no Explode flag: budget untouched
        const float rs = dmath::sqrt(260.0f * 260.0f + 30.0f * 30.0f);
        bool on_sphere = false;
        for (const Vec3& v : q.points()) {
            CHECK(v.z == 0.0f);
            on_sphere = on_sphere || std::fabs(static_cast<double>(dist3(v, E) - rs)) < 1e-2;
        }
        CHECK(on_sphere);
    }
    {  // [port] degenerate: aimed exactly at the centre (V = -U) -> the arc passes on the horizontal side, finite points
        k.rp = rocket_params(50.0f, 10.0f, 0.0f, 1000.0f, true, false);
        RocketSpline q = old;
        float s = 100.0f, b = 1000.0f;
        CHECK(rocket_replan_land(q, Vec3{-320, 0, 0}, E, Vec3{1, 0, 0}, s, b, k.ctx(P)));
        for (const Vec3& v : q.points()) CHECK(is_finite(v.x) && is_finite(v.y) && is_finite(v.z));
        CHECK(q.count() >= 5u && q.points()[2].y > 100.0f);
        CHECK_NEAR(dist3(q.points()[2], E), 280.0, 1e-2);
    }
    {  // the subdivision: a dot rounding above 1 is clamped (retail NaN never stops); a half circle at thr gives
       // 2^d - 1 midpoints, all on the unit sphere
        std::vector<Vec3> out;
        const Vec3 a{1.0000001f, 0.0f, 0.0f};  // a . a = 1.0000002
        rocket_arc_subdivide(a, a, 1000.0f, 0.0f, out);
        CHECK(out.empty());
        rocket_arc_subdivide(Vec3{1, 0, 0}, Vec3{0, 1, 0}, 100.0f, 50.0f, out);  // 90 deg x 100 = 157 -> 3 mids (<= 39)
        CHECK(out.size() == 3u);
        for (const Vec3& v : out) CHECK_NEAR(dot(v, v), 1.0, 1e-5);
        out.clear();
        rocket_arc_subdivide(Vec3{1, 0, 0}, Vec3{0, 1, 0}, 1.0f, -1.0f, out);  // never converges: capped depth
        CHECK(out.size() == (1u << kRocketArcMaxDepth) - 1u);
    }
}
}  // namespace bo3split

namespace bo3split {

uint64_t rocket_digest(const CombatSystem& s, const SimRng& rng) {
    StateHasher h;
    s.hash_projectiles(h);
    s.hash_extended(h);
    s.hash_units(h);
    rng.hash(h);
    return h.digest();
}
}  // namespace bo3split

namespace bo3split {

void test_rocket_flight_system() {
    {  // one launch, nothing else in the world: the first flight step takes exactly the plan's draws (land example)
        CombatData d;
        ProjectileDesc rk;
        rk.name = "Proj_Dark_Trooper_Missile";
        rk.max_speed = 10.0f;
        rk.category = ProjectileCategory::Rocket;
        rk.max_flight_distance = 330.0f;
        rk.rocket_curve_distance = 50.0f;
        rk.rocket_curve_offset = 10.0f;
        d.projectiles = {rk};
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        CombatSystem sys(d, cfg, {PlayerInfo{}, PlayerInfo{}});
        TreeSet trees(2);
        SimRng rng(99u);
        ProjectileLaunch l;
        l.owner = 0;
        l.projectile = 0;
        l.from = Vec3{0, 0, 0};
        l.point = Vec3{300, 0, 0};
        CHECK(sys.launch_projectile(l, 1));
        CHECK(sys.projectile(0).max_travel == 330.0f);  // MFD + 0 radius + |dz|
        sys.tick(1, trees, rng);
        CHECK(rng.draws() == 0u);  // spawned this frame: first stepped next tick
        sys.tick(2, trees, rng);
        CHECK(rng.draws() == 10u);
        RocketView v;
        CHECK(sys.rocket(0, v) && v.planned && v.budget == 330.0f && v.s == 10.0f);
        Vec3 n;
        CHECK(v.path->sample(10.0f, n) && same(sys.projectile(0).pos, n));  // the position comes from the spline
        uint32_t frame = 3;
        while (sys.projectile_count() > 0 && frame < 200) sys.tick(frame++, trees, rng);
        CHECK(sys.projectile_count() == 0u && rng.draws() == 10u);  // no other draws; it expired at the travel limit
        CHECK(sys.stats().expired == 1u);
    }
    {  // Explode_When_Reached_Target_Radius + Target_Point_On_Terrain (land): detonates at the launch -> target length
        CombatData d;
        ProjectileDesc rk;
        rk.name = "Proj_MAL_Concussion_Missile";
        rk.max_speed = 10.0f;
        rk.category = ProjectileCategory::MptlRocket;
        rk.max_flight_distance = 600.0f;
        rk.rocket_curve_distance = 50.0f;
        rk.rocket_curve_offset = 10.0f;
        rk.target_point_on_terrain = true;
        rk.explode_when_reached_target_radius = true;
        d.projectiles = {rk};
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        CombatSystem sys(d, cfg, {PlayerInfo{}, PlayerInfo{}});
        TreeSet trees(2);
        SimRng rng(5u);
        ProjectileLaunch l;
        l.owner = 0;
        l.from = Vec3{0, 0, 0};
        l.point = Vec3{300, 0, 0};
        CHECK(sys.launch_projectile(l, 1));
        Vec3 at;
        bool detonated = false;
        for (uint32_t f = 1; f < 200 && !detonated; ++f) {
            sys.tick(f, trees, rng);
            for (const HitEvent& e : sys.hit_events()) {
                detonated = true;
                at = e.position;
            }
        }
        CHECK(detonated && dist3(at, Vec3{300, 0, 0}) < 15.0f);
    }
    {  // a shooter's rockets hit their target in both modes (path through the aim point, sweep P -> N)
        for (GameMode m : {GameMode::Space, GameMode::Land}) {
            RocketWorld rw(m);
            for (int i = 0; i < 400; ++i) rw.w.tick();
            CHECK(rw.w.sys->stats().shots > 3u && rw.w.sys->stats().hits > 0u);
            CHECK(rw.w.sys->health(2) < rw.w.sys->max_health(2));
        }
    }
    {  // missile shield (space): a crossing re-plans - that step still moves to the OLD path's N and resets s to 0 -
       // and the rockets pass around the emitter instead of flying through it
        RocketWorld rw(GameMode::Space, false, true);
        uint32_t replans = 0, bad_move = 0;
        float closest = 1e9f;
        for (int i = 0; i < 400; ++i) {
            std::vector<std::pair<uint32_t, Vec3>> next;  // serial -> N on the current path
            for (size_t k = 0; k < rw.w.sys->projectile_count(); ++k) {
                RocketView v;
                const ProjectileView pv = rw.w.sys->projectile(k);
                Vec3 n;
                if (rw.w.sys->rocket(k, v) && v.planned && v.path->sample(v.s + pv.speed, n)) next.emplace_back(pv.serial, n);
            }
            rw.w.tick();
            for (size_t k = 0; k < rw.w.sys->projectile_count(); ++k) {
                RocketView v;
                const ProjectileView pv = rw.w.sys->projectile(k);
                if (!pv.alive || !rw.w.sys->rocket(k, v)) continue;
                closest = std::min(closest, dist3(pv.pos, Vec3{-300, -40, 0}));
                if (v.planned && v.s == 0.0f) {
                    ++replans;
                    bool found = false;
                    for (const auto& [serial, n] : next)
                        if (serial == pv.serial) found = same(n, pv.pos);
                    if (!found) ++bad_move;
                }
            }
        }
        CHECK(replans > 0u && bad_move == 0u);
        CHECK(closest > 60.0f);
        std::printf("rockets vs missile shield (space): %u re-plans, closest approach to the emitter %.1f\n", replans,
                    static_cast<double>(closest));
        CHECK(rw.w.sys->health(3) == rw.w.sys->max_health(3));
    }
    {  // land with Target_Point_On_Terrain: the re-plan draws nothing (CD 50 >= k), so the synced stream is the same
       // with and without the emitter; without the flag the leg-2 draws shift it
        RocketWorld a(GameMode::Land, true, true), b(GameMode::Land, true, false);
        RocketWorld c(GameMode::Land, false, true), d(GameMode::Land, false, false);
        uint32_t replans = 0;
        for (int i = 0; i < 300; ++i) {
            a.w.tick();
            b.w.tick();
            c.w.tick();
            d.w.tick();
            for (size_t k = 0; k < a.w.sys->projectile_count(); ++k) {
                RocketView v;
                if (a.w.sys->rocket(k, v) && v.planned && v.s == 0.0f) ++replans;
            }
        }
        CHECK(replans > 0u);
        CHECK(a.w.rng.draws() == b.w.rng.draws());
        CHECK(c.w.rng.draws() > d.w.rng.draws());
    }
    {  // worker counts: identical rocket state, hashes and draws every tick (shield re-plans included)
        RocketWorld s(GameMode::Space, false, true), p4(GameMode::Space, false, true), p8(GameMode::Space, false, true);
        const ParallelFor pf4 = alamo::test::make_thread_parallel_for(4);
        const ParallelFor pf8 = alamo::test::make_thread_parallel_for(8);
        uint32_t diverged = 0;
        for (int i = 0; i < 300; ++i) {
            s.w.tick();
            p4.w.tick(pf4);
            p8.w.tick(pf8);
            const uint64_t h = rocket_digest(*s.w.sys, s.w.rng);
            if (h != rocket_digest(*p4.w.sys, p4.w.rng) || h != rocket_digest(*p8.w.sys, p8.w.rng)) ++diverged;
        }
        CHECK(diverged == 0u);
    }
    {  // save / load mid-flight (planned paths, re-planned paths, unplanned launches): identical bytes and continuation
        RocketWorld a(GameMode::Land, false, true);
        uint32_t in_flight = 0;
        for (int i = 0; i < 400 && in_flight < 2; ++i) {
            a.w.tick();
            in_flight = 0;
            for (size_t k = 0; k < a.w.sys->projectile_count(); ++k) {
                RocketView v;
                if (a.w.sys->rocket(k, v) && v.planned) ++in_flight;
            }
        }
        CHECK(in_flight >= 2u);
        const std::vector<uint8_t> saved = combat_bytes(*a.w.sys);
        ByteWriter rng_w;
        a.w.rng.write(rng_w);
        RocketWorld b(GameMode::Land, false, true);
        ByteReader r(saved);
        CHECK(b.w.sys->read(r));
        ByteReader rr(rng_w.bytes());
        CHECK(b.w.rng.read(rr));
        b.w.frame = a.w.frame;
        CHECK(combat_bytes(*b.w.sys) == saved);
        CHECK(combat_digest(*a.w.sys, a.w.rng) == combat_digest(*b.w.sys, b.w.rng));
        const ParallelFor pf4 = alamo::test::make_thread_parallel_for(4);
        uint32_t diverged = 0;
        for (int i = 0; i < 300; ++i) {
            a.w.tick();
            b.w.tick(pf4);
            if (combat_digest(*a.w.sys, a.w.rng) != combat_digest(*b.w.sys, b.w.rng)) ++diverged;
        }
        CHECK(diverged == 0u);
        CHECK(combat_bytes(*a.w.sys) == combat_bytes(*b.w.sys));
    }
    {  // a rocket type without curve data (Projectile_Rocket_Curve_Distance absent = 0): spline flight, but no legs and no
       // draws; the path is not buildable, so the rocket expires on its first flight step (refixf t01: no straight fallback)
        CombatData d;
        ProjectileDesc rk;
        rk.max_speed = 10.0f;
        rk.category = ProjectileCategory::Rocket;
        rk.max_flight_distance = 300.0f;
        d.projectiles = {rk};
        CombatSystem sys(d, CombatConfig{}, {PlayerInfo{}, PlayerInfo{}});
        TreeSet trees(2);
        SimRng rng(1u);
        ProjectileLaunch l;
        l.owner = 0;
        l.from = Vec3{0, 0, 0};
        l.point = Vec3{200, 0, 0};
        CHECK(sys.launch_projectile(l, 1));
        sys.tick(1, trees, rng);
        sys.tick(2, trees, rng);
        CHECK(sys.projectile_count() == 0u && rng.draws() == 0u);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_weapon_sources() {
    // TACTICAL_COMBAT §3.1.1 table: the flags make the own weapon and the weapon hardpoints alternate.
    struct Case {
        const char* name;
        SourceFlags own, hp;
        bool grenade;
        bool deployed, rocket;
        bool own_fires, hp_fires;
    };
    const SourceFlags any{};
    const SourceFlags spmat_own{true, false, true, true}, spmat_hp{false, true, true, true};
    const SourceFlags t4b_own{true, true, false, true}, t4b_hp{true, true, true, false};
    const SourceFlags dt3_own{false, true, true, true}, dt3_hp{true, false, true, true};
    const SourceFlags dd_own{true, false, false, true}, dd_hp{true, false, true, false};
    const Case cases[] = {
        {"SPMAT undeployed", spmat_own, spmat_hp, true, false, false, false, true},
        {"SPMAT deployed", spmat_own, spmat_hp, true, true, false, true, false},
        {"T4B normal", t4b_own, t4b_hp, false, false, false, true, false},
        {"T4B rocket", t4b_own, t4b_hp, false, false, true, false, true},
        {"DT3 undeployed", dt3_own, dt3_hp, false, false, false, true, false},
        {"DT3 deployed", dt3_own, dt3_hp, false, true, false, false, true},
        {"Destroyer undeployed", dd_own, dd_hp, false, false, false, false, false},
        {"Destroyer deployed", dd_own, dd_hp, false, true, false, true, false},
        {"Destroyer deployed rocket", dd_own, dd_hp, false, true, true, false, true},
        {"defaults", any, any, false, false, false, true, true},  // both sources fire side by side (separate cooldowns)
    };
    for (const Case& c : cases) {
        SourceWorld sw(c.own, c.hp, c.grenade);
        if (c.deployed) sw.w.sys->set_flag(1, kUnitDeploying, true);
        if (c.rocket) sw.w.sys->set_ability_active(1, 0, true);
        const std::array<uint32_t, 2> n = sw.run(120);
        if ((n[0] > 0) != c.own_fires || (n[1] > 0) != c.hp_fires)
            std::printf("FAIL weapon sources %s: own %u hardpoint %u\n", c.name, n[0], n[1]);
        CHECK((n[0] > 0) == c.own_fires && (n[1] > 0) == c.hp_fires);
    }
    {  // A deploy transition (0x39a400 / 0x39b2a0): neither source fires; the hardpoints do not count down.
        SourceWorld sw(spmat_own, spmat_hp, true);
        sw.w.sys->set_flag(1, kUnitDeploying | kUnitDeployTransition, true);
        const int32_t c0 = sw.w.sys->hardpoint_cooldown(1, 0);
        const std::array<uint32_t, 2> n = sw.run(60);
        CHECK(n[0] == 0 && n[1] == 0 && sw.w.sys->hardpoint_cooldown(1, 0) == c0);
        sw.w.sys->set_flag(1, kUnitDeployTransition, false);  // deployed: the grenade only
        const std::array<uint32_t, 2> m = sw.run(120);
        CHECK(m[0] > 0 && m[1] == 0);
    }
    {  // Not a Deploys type: the deploy flags never apply (0x39a390 needs Deploys and a locomotor).
        SourceWorld sw(spmat_own, spmat_hp, true);
        sw.w.data.units[0].deploys = false;
        sw.w.sys->set_flag(1, kUnitDeploying | kUnitDeployTransition, true);
        const std::array<uint32_t, 2> n = sw.run(120);
        CHECK(n[0] == 0 && n[1] > 0);  // read as undeployed
    }
}
}  // namespace bo3split

namespace bo3split {

void test_hardpoint_state_gate() {
    {  // Filter 7 uses the gate: an Air target is never acquired, the ground one is (and shot).
        GateWorld g(false);
        CHECK(g.run(60) > 0);
        CHECK(g.w.sys->auto_target(1) == 3);
    }
    {  // A primary target the gate refuses (ordered Air target): no Fire_At and NO opportunity fire at the ground unit.
        GateWorld g(false);
        g.w.sys->order_attack(1, 2);
        CHECK(g.run(90) == 0);
        CHECK(g.w.sys->hardpoint_opportunity_target(1, 0) == kNoObject);
    }
    {  // DEFEND active and Fire_When_In_Defend_Mode No: no target is acquired, nothing fires (opportunity fire included).
        GateWorld g(true);
        CHECK(g.run(90) == 0);
        CHECK(g.w.sys->auto_target(1) == kNoObject);
        g.w.sys->set_ability_active(1, 0, false);
        CHECK(g.run(90) > 0);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_weapon_suspend_elapse() {
    SuspendWorld a;
    const uint32_t f1 = a.until_shot(400);
    CHECK(f1 != 0);
    const uint32_t f2 = a.until_shot(400);  // the normal recharge: 150..160 frames
    CHECK(f2 >= f1 + 150 && f2 <= f1 + 161);
    // Suspended for 200 frames right after a shot: nothing fires; the first frame back fires (the recharge ran out in
    // the air).
    a.w.sys->set_flag(1, kUnitWeaponsSuspended, true);
    CHECK(a.until_shot(100) == 0);
    // Save / load in the middle of the suspension keeps the last-service frame.
    ByteWriter sw;
    a.w.sys->write(sw);
    ByteWriter rw;
    a.w.rng.write(rw);
    SuspendWorld b;
    {
        ByteReader r(sw.bytes());
        CHECK(b.w.sys->read(r));
        ByteReader rr(rw.bytes());
        CHECK(b.w.rng.read(rr));
        b.w.frame = a.w.frame;
    }
    for (SuspendWorld* x : {&a, &b}) {
        CHECK(x->until_shot(100) == 0);
        x->w.sys->set_flag(1, kUnitWeaponsSuspended, false);
        const uint32_t back = x->w.frame;
        CHECK(x->until_shot(5) == back);
    }
    CHECK(combat_bytes(*a.w.sys) == combat_bytes(*b.w.sys));
    {  // A short suspension subtracts its frames too: the shot comes at the normal time, not later.
        SuspendWorld c, d;
        const uint32_t c1 = c.until_shot(400), d1 = d.until_shot(400);
        CHECK(c1 == d1 && c1 != 0);
        c.w.sys->set_flag(1, kUnitWeaponsSuspended, true);
        for (int i = 0; i < 50; ++i) c.w.tick();
        for (int i = 0; i < 50; ++i) d.w.tick();
        c.w.sys->set_flag(1, kUnitWeaponsSuspended, false);
        CHECK(c.until_shot(400) == d.until_shot(400));
    }
}
}  // namespace bo3split

namespace bo3split {

void test_muzzle_bones() {
    const Vec3 A{2, 0, 5}, B{2, 1, 5}, C{0, -1, 3};
    const Vec3 wa{10, 22, 5}, wb{9, 22, 5}, wc{11, 20, 3};  // turned by the facing +y, + the position
    {  // Round robin per shot.
        MuzzleWorld m({A, B, C});
        const std::vector<Vec3> l = m.launches(90);
        CHECK(l.size() >= 6);
        const Vec3 expect[3] = {wa, wb, wc};
        bool ok = true;
        for (size_t i = 0; i < l.size(); ++i) ok = ok && same(l[i], expect[i % 3]);
        CHECK(ok);
    }
    {  // More than one Projectile_Types entry and more than one bone: the bone of the weapon index.
        MuzzleWorld m({A, B, C}, true, false, 2);
        CHECK(m.w.sys->set_weapon_index(1, 1));
        const std::vector<Vec3> l = m.launches(60);
        CHECK(!l.empty());
        bool ok = true;
        for (const Vec3& p : l) ok = ok && same(p, wb);
        CHECK(ok);
        // Outside the bone list: no fire bone - the position + the shooter's own z adjust ([port] index 5 fires
        // weapon_projectile, LOB_SOLVER §5.1 request bone -1).
        CHECK(m.w.sys->set_weapon_index(1, 5));
        const std::vector<Vec3> l2 = m.launches(30);
        CHECK(!l2.empty() && same(l2.back(), Vec3{10, 20, 7}));
    }
    {  // Model known, no MUZZLEA_00: the weapon never fires.
        MuzzleWorld m({});
        CHECK(m.launches(90).empty() && m.w.sys->stats().shots == 0u);
    }
    {  // [port] No model data: the position (straight shots), as before.
        MuzzleWorld m({}, false);
        const std::vector<Vec3> l = m.launches(30);
        CHECK(!l.empty() && same(l.front(), Vec3{10, 20, 0}));
    }
    {  // Grenades leave from the bones too; a null spawn (no arc) changes no cooldown: the shot follows on the frame the
       // target comes into reach.
        MuzzleWorld m({A, B}, true, true);
        m.w.sys->order_attack(1, 2);
        m.w.sys->set_position(2, Vec3{10, 900, 0});
        m.w.trees.update(2, box_of(Vec3{10, 900, 0}, 5.0f));
        CHECK(m.launches(60).empty());
        m.w.sys->set_position(2, Vec3{10, 120, 0});
        m.w.trees.update(2, box_of(Vec3{10, 120, 0}, 5.0f));
        const std::vector<Vec3> l = m.launches(1);
        CHECK(l.size() == 1 && (same(l[0], wa) || same(l[0], wb)));
        CHECK(m.w.sys->projectile_count() == 1 && m.w.sys->projectile(0).lob);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_own_weapon_targeting_state() {
    // S8 t10 (e) / t11 step 7 (TACTICAL_COMBAT §3.1.1 "Gate details", LOB_SOLVER §8 item 22): the own weapon fires only in
    // targeting state 4 (slot 44 && !slot 45, re-tested every service); otherwise the burst restarts.
    {  // in range: fires; attacking() holds
        OwnWorld o(OwnOpt{}, Vec3{100, 0, 0});
        CHECK(o.run(60) > 0);
        CHECK(o.w.sys->attacking(1));
    }
    {  // ordered onto a target out of range: never fires; brought into range: fires at once (the recharge ran out)
        OwnWorld o(OwnOpt{}, Vec3{600, 0, 0});
        CHECK(o.run(60) == 0);
        CHECK(!o.w.sys->attacking(1) && o.w.sys->current_target(1) == 2);
        o.move(2, Vec3{100, 0, 0});
        const std::vector<uint32_t> f = o.shot_frames(1);
        CHECK(f.size() == 1);
        CHECK(o.w.sys->attacking(1));
    }
    {  // inside Targeting_Min_Attack_Distance (slot 45): no fire; outside it: fire
        OwnOpt opt;
        opt.min_range = 50.0f;
        OwnWorld o(opt, Vec3{30, 0, 0});
        CHECK(o.run(60) == 0 && !o.w.sys->attacking(1));
        o.move(2, Vec3{150, 0, 0});
        CHECK(o.run(30) > 0 && o.w.sys->attacking(1));
    }
    {  // a 3-pulse burst (pulse delay 3 frames, recharge 5 s): a service out of range restarts it (3 more pulses)
        OwnOpt opt;
        opt.recharge = 5.0f;
        opt.pulses = 3;
        opt.pulse_delay = 0.1f;
        OwnWorld b(opt, Vec3{100, 0, 0});
        // control: the first burst gives 3 pulses 3 frames apart
        const std::vector<uint32_t> fb = b.shot_frames(200);
        CHECK(fb.size() >= 3 && fb[1] == fb[0] + 3 && fb[2] == fb[1] + 3);
        // stopped right after the first pulse
        OwnWorld d(opt, Vec3{100, 0, 0});
        uint32_t first = 0;
        for (int i = 0; i < 400 && first == 0; ++i) {
            const std::vector<uint32_t> s = d.shot_frames(1);
            if (!s.empty()) first = s[0];
        }
        CHECK(first != 0);
        d.move(2, Vec3{900, 0, 0});  // out of range for 10 services (the pulse delay runs out: pulses := 3)
        CHECK(d.run(10) == 0);
        d.move(2, Vec3{100, 0, 0});
        const std::vector<uint32_t> after = d.shot_frames(10);  // 3 pulses, not the 2 left of the first burst
        CHECK(after.size() == 3);
        CHECK(after.size() == 3 && after[1] == after[0] + 3 && after[2] == after[1] + 3);
    }
    {  // no live target: nothing fires, attacking() false
        OwnWorld o(OwnOpt{}, Vec3{100, 0, 0});
        o.w.sys->kill_unit(2, 1, kKindNormal, o.w.frame);
        o.w.trees.remove(2);
        CHECK(o.run(30) == 0 && !o.w.sys->attacking(1));
    }
}
}  // namespace bo3split

namespace bo3split {

void test_own_weapon_gate_steps() {
    // S8 t11 (TACTICAL_COMBAT §3.1.1 "Gate details"): steps 5 - 7 and the step-3 correction.
    {  // step 5: an active mode whose WEAPONS_ENABLED flag is 0 refuses; inactive or flag 1: fires
        OwnOpt opt;
        opt.weapons_enabled_flag = 0;
        OwnWorld o(opt, Vec3{100, 0, 0});
        o.w.sys->set_ability_active(1, 0, true);
        CHECK(o.run(60) == 0);
        o.w.sys->set_ability_active(1, 0, false);
        CHECK(o.run(30) > 0);
        opt.weapons_enabled_flag = 1;
        OwnWorld e(opt, Vec3{100, 0, 0});
        e.w.sys->set_ability_active(1, 0, true);
        CHECK(e.run(60) > 0);
    }
    {  // step 6: fighter Idle (0x1C) refuses without Can_Fighter_Fire_When_Idle
        OwnWorld o(OwnOpt{}, Vec3{100, 0, 0});
        o.w.sys->set_flag(1, kUnitFighterIdle, true);
        CHECK(o.run(60) == 0);
        o.w.sys->set_flag(1, kUnitFighterIdle, false);
        CHECK(o.run(30) > 0);
        OwnOpt opt;
        opt.idle_fire = true;
        OwnWorld f(opt, Vec3{100, 0, 0});
        f.w.sys->set_flag(1, kUnitFighterIdle, true);
        CHECK(f.run(60) > 0);
    }
    {  // step 6: state 0x16 on an air locomotor (vf +0x240) refuses, Can_Fighter_Fire_When_Idle or not
        OwnOpt opt;
        opt.idle_fire = true;
        OwnWorld o(opt, Vec3{100, 0, 0});
        o.w.sys->set_flag(1, kUnitHoverRest, true);
        CHECK(o.run(60) == 0);
        o.w.sys->set_flag(1, kUnitHoverRest, false);
        CHECK(o.run(30) > 0);
    }
    {  // step 7: LOBBING_SUPERWEAPON never fires its own weapon at a target (fire-at-position orders only)
        OwnOpt opt;
        opt.lobbing = true;
        OwnWorld o(opt, Vec3{100, 0, 0});
        CHECK(o.run(90) == 0);
    }
    {  // the host-mirror flags are not hashed (the CRC follows the behaviour, not the mirror)
        OwnWorld a(OwnOpt{}, Vec3{100, 0, 0}), b(OwnOpt{}, Vec3{100, 0, 0});
        b.w.sys->set_flag(1, kUnitRocketAttack, true);
        StateHasher ha, hb;
        a.w.sys->hash_units(ha);
        b.w.sys->hash_units(hb);
        CHECK(ha.digest() == hb.digest());
    }
    {  // step 4: ROCKET_ATTACK active in any Unit_Abilities_Data entry (the host mirror kUnitRocketAttack): T4B's own
       // weapon (Fire_Weapon_When_In_Rocket_Attack_Mode No) stops, its hardpoints fire
        const SourceFlags t4b_own{true, true, false, true}, t4b_hp{true, true, true, false};
        SourceWorld sw(t4b_own, t4b_hp, false);
        sw.w.sys->set_flag(1, kUnitRocketAttack, true);
        const std::array<uint32_t, 2> n = sw.run(120);
        CHECK(n[0] == 0 && n[1] > 0);
    }
    {  // step 3 correction: deploying / undeploying (the transition) is not "deployed" for the death-clone remap either
        World w;
        UnitDesc walker;
        walker.tactical_health = 100.0f;
        walker.radius = 5.0f;
        walker.deploys = true;
        walker.has_locomotor = true;
        walker.death_clones = {{1, 50}, {12, 53}};
        push_unit(w.data, walker);
        w.make();
        w.add(1, 0, 1, Vec3{0, 0, 0});
        w.add(2, 0, 1, Vec3{100, 0, 0});
        w.sys->set_flag(1, kUnitDeploying, true);
        w.sys->set_flag(2, kUnitDeploying | kUnitDeployTransition, true);
        w.sys->kill_unit(1, kNoObject, kKindNormal, w.frame);
        CHECK(!w.sys->deaths().empty() && w.sys->deaths().back().death_clone == 53);
        w.sys->kill_unit(2, kNoObject, kKindNormal, w.frame);
        CHECK(!w.sys->deaths().empty() && w.sys->deaths().back().death_clone == 50);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_hardpoints_fire_while_suspended() {
    // S8 t12 (e) (MOVEMENT §3.2): a locomotor swap suspends TARGETING / WEAPON only; the hardpoints keep firing.
    const SourceFlags any{};
    SourceWorld sw(any, any, false);
    const std::array<uint32_t, 2> n = sw.run(60);
    CHECK(n[0] > 0 && n[1] > 0);
    sw.w.sys->set_flag(1, kUnitWeaponsSuspended, true);
    const std::array<uint32_t, 2> m = sw.run(60);
    CHECK(m[0] == 0 && m[1] > 0);
}
}  // namespace bo3split

namespace bo3split {

void test_first_service() {
    // S8 t12 (c) (TACTICAL_COMBAT §3.1.1 "First service after spawn"): the initial delay is Get_Int(0, FPS x recharge)
    // and the first service subtracts the whole frame number (last service 0).
    OwnOpt opt;
    opt.recharge = 5.0f;
    {  // spawned at frame 1: the first shot comes on frame max(delay, 1)
        SimRng r(12345u);
        const int32_t delay = r.range_int(0, 150);
        OwnWorld o(opt, Vec3{100, 0, 0});
        const std::vector<uint32_t> f = o.shot_frames(200);
        CHECK(!f.empty() && f[0] == static_cast<uint32_t>(std::max(delay, 1)));
    }
    {  // spawned at frame 1000 (past any drawn delay): fires on its first service
        OwnWorld o(opt, Vec3{100, 0, 0}, 1000);
        const std::vector<uint32_t> f = o.shot_frames(5);
        CHECK(!f.empty() && f[0] == 1000u);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_own_player_sweep() {
    // S8 t10 (d) (TACTICAL_COMBAT §3.4 "Own-player variant 0x2acb90"): with Friendly_Damage_Amount > 0 and no hostile hit,
    // the owner's tree yields its first candidate (the projectile alone is skipped); the hit stands only when it is the
    // projectile's own target without TRANSPORT_LANDING.
    struct Sweep {
        World w;
        explicit Sweep(bool landing) {
            ProjectileDesc p;
            p.max_speed = 20.0f;
            p.damage = 10.0f;
            p.friendly_damage_amount = 10.0f;
            p.max_flight_distance = 3000.0f;
            w.data.projectiles.push_back(p);
            UnitDesc u;
            u.tactical_health = 1000.0f;
            u.radius = 5.0f;
            push_unit(w.data, u);
            UnitDesc l = u;
            l.transport_landing = landing;
            push_unit(w.data, l);
            CombatConfig cfg;
            cfg.mode = GameMode::Land;
            cfg.inaccuracy_enabled = false;
            w.make(cfg);
            w.add(1, 0, 0, Vec3{-500, 0, 0});  // the shooter, away from the path
            w.add(3, 0, 0, Vec3{100, 0, 0});   // a friendly in the path
            w.add(4, 1, 0, Vec3{200, 0, 0});   // friendly target behind it
            w.add(5, 0, 1, Vec3{200, 300, 0}); // an enemy (off the path)
        }
        void fire(ObjectId target) {
            ProjectileLaunch l;
            l.shooter = 1;
            l.owner = 0;
            l.projectile = 0;
            l.from = Vec3{0, 0, 0};
            l.target = target;
            CHECK(w.sys->launch_projectile(l, w.frame));
            for (int i = 0; i < 40; ++i) w.tick();
        }
    };
    {  // aimed at the friendly target 4: the friendly 3 in front is the first candidate but not the target - no hit
       // there, the flight goes on and the target is hit
        Sweep s(false);
        s.fire(4);
        CHECK(s.w.sys->health(3) == s.w.sys->max_health(3));
        CHECK(s.w.sys->health(4) < s.w.sys->max_health(4) && s.w.sys->stats().hits == 1u);
    }
    {  // the same target with TRANSPORT_LANDING: never an own-player hit
        Sweep s(true);
        s.fire(4);
        CHECK(s.w.sys->health(3) == s.w.sys->max_health(3) && s.w.sys->health(4) == s.w.sys->max_health(4) && s.w.sys->stats().hits == 0u);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_garrison_fire_bones() {
    // S8 t10 (a) (LOB_SOLVER §5.1 "Garrison fire bones in detail"): a garrisoned shooter launches from its host's gun bones.
    struct Garrison {
        World w;
        uint32_t last = 0;
        Garrison(std::vector<std::vector<GarrisonBone>> bones, std::vector<std::pair<float, int32_t>> alts, bool garrisoned) {
            ProjectileDesc p;
            p.max_speed = 20.0f;
            w.data.projectiles.push_back(p);
            UnitDesc s;
            s.tactical_health = 100.0f;
            s.max_attack_distance = 300.0f;
            s.weapon_projectile = 0;
            s.weapon_recharge = 0.2f;
            push_unit(w.data, s);
            UnitDesc h;
            h.tactical_health = 1000.0f;
            h.radius = 5.0f;
            h.num_garrison_guns = 2;
            h.garrison_bones = std::move(bones);
            h.damage_alternates = std::move(alts);
            push_unit(w.data, h);
            UnitDesc t;
            t.tactical_health = 1.0e7f;
            t.radius = 5.0f;
            push_unit(w.data, t);
            CombatConfig cfg;
            cfg.mode = GameMode::Land;
            cfg.inaccuracy_enabled = false;
            w.make(cfg);
            w.add(1, 0, 0, Vec3{0, 0, 0});
            w.add(2, 2, 1, Vec3{0, 100, 0});                // north of the host
            w.add(3, 1, 0, Vec3{0, 0, 0}, Vec3{0, 1, 0});  // host facing +y: local (x, y, z) -> world (-y, x, z)
            if (garrisoned) w.sys->set_flag(1, kUnitGarrisoned, true);
            w.sys->set_garrison_host_query([](ObjectId id) { return id == 1 ? ObjectId{3} : kNoObject; });
            w.sys->order_attack(1, 2);
            d0 = w.rng.draws();
        }
        uint64_t d0 = 0;
        uint64_t draws() const { return w.rng.draws() - d0; }  // synced draws while ticking
        std::vector<Vec3> launches(int ticks) {
            std::vector<Vec3> out;
            for (int i = 0; i < ticks; ++i) {
                w.tick();
                for (size_t k = 0; k < w.sys->projectile_count(); ++k) {
                    const ProjectileView pv = w.sys->projectile(k);
                    if (pv.serial <= last) continue;
                    last = pv.serial;
                    out.push_back(pv.pos);
                }
            }
            return out;
        }
    };
    // ALT 0: bone 0 at local (5, 0, 3) pointing local +x (world +y: the target), bone 1 at (-5, 0, 3) pointing local -x.
    GarrisonBone fwd{true, Vec3{5, 0, 3}, 0.0f}, back{true, Vec3{-5, 0, 3}, 180.0f}, fwd2{true, Vec3{6, 1, 4}, 30.0f};
    const Vec3 wf{0, 5, 3}, wf2{-1, 6, 4};
    {  // one candidate: that bone, no draw (the synced stream matches the ungarrisoned run)
        Garrison g({{fwd, back}}, {}, true), n({{fwd, back}}, {}, false);
        const std::vector<Vec3> l = g.launches(60), ln = n.launches(60);
        CHECK(!l.empty() && l.size() == ln.size());
        bool ok = true;
        for (const Vec3& p : l) ok = ok && same(p, wf);
        CHECK(ok);
        CHECK(!ln.empty() && same(ln.front(), Vec3{0, 0, 0}));  // not garrisoned: the shooter's own launch point
        CHECK(n.draws() == g.draws());
    }
    {  // two candidates: one synced Get_Int(0, 1) per shot, both bones in use. Up to the first shot both runs draw the
       // same; the shot's tick draws one more in the garrisoned run.
        Garrison g({{fwd, back, fwd2}}, {}, true), n({{fwd, back, fwd2}}, {}, false);
        bool shot = false;
        for (int i = 0; i < 200 && !shot; ++i) {
            const uint64_t g0 = g.draws(), n0 = n.draws();
            const size_t gl = g.launches(1).size(), nl = n.launches(1).size();
            CHECK(gl == nl);
            shot = gl > 0;
            CHECK(g.draws() - g0 == n.draws() - n0 + (shot ? 1u : 0u));
        }
        CHECK(shot);
        const std::vector<Vec3> l = g.launches(120);
        CHECK(l.size() >= 4);
        bool ok = true, any_f = false, any_f2 = false;
        for (const Vec3& p : l) {
            ok = ok && (same(p, wf) || same(p, wf2));
            any_f = any_f || same(p, wf);
            any_f2 = any_f2 || same(p, wf2);
        }
        CHECK(ok && any_f && any_f2);
    }
    {  // no bone within 60 degrees: the shooter's own launch point
        Garrison g({{back}}, {}, true);
        const std::vector<Vec3> l = g.launches(30);
        CHECK(!l.empty() && same(l.front(), Vec3{0, 0, 0}));
    }
    {  // the host's ALT (Land_Damage_Thresholds / _Alternates) picks the entry; ALT >= M: the own launch point
        Garrison g({{back}, {fwd}}, {{1.0f, 0}, {0.66f, 1}, {0.33f, 2}}, true);
        CHECK(g.w.sys->model_alt(3) == 0);
        g.w.sys->set_health(3, 500.0f);  // 0.5: ALT 1
        CHECK(g.w.sys->model_alt(3) == 1);
        std::vector<Vec3> l = g.launches(30);
        CHECK(!l.empty() && same(l.back(), wf));
        Vec3 hp;
        std::vector<GarrisonFireBone> bones;
        // s78f t03: the world position and the x-axis yaw of the current pose (host facing +y: local yaw 0 -> world 90)
        CHECK(g.w.sys->garrison_fire_bones(3, hp, bones) && bones.size() == 1 && same(bones[0].pos, wf) && same(hp, Vec3{0, 0, 0}));
        CHECK(bones.size() == 1 && std::fabs(bones[0].yaw_deg - 90.0f) < 1e-4f);
        g.w.sys->set_health(3, 200.0f);  // 0.2: ALT 2 = M -> no garrison bones
        CHECK(g.w.sys->model_alt(3) == 2);
        CHECK(!g.w.sys->garrison_fire_bones(3, hp, bones));
        l = g.launches(30);
        CHECK(!l.empty() && same(l.back(), Vec3{0, 0, 0}));
    }
    {  // s78f t03 (LOB_SOLVER §5.1 "Garrison pick inputs"): the bone's x-axis yaw is tested, not the angle of its position
       // around the host: a bone behind the host (local -x) pointing forward (yaw 0) is a candidate; one in front pointing
       // backward is not.
        const GarrisonBone behind_fwd{true, Vec3{-5, 0, 3}, 0.0f}, front_back{true, Vec3{5, 0, 3}, 180.0f};
        Garrison g({{front_back, behind_fwd}}, {}, true);
        const std::vector<Vec3> l = g.launches(30);
        CHECK(!l.empty() && same(l.front(), Vec3{0, -5, 3}));
        // the free test: 60 degrees inclusive, the difference wrapped through 360
        CHECK(garrison_bone_within(350.0f, Vec3{0, 0, 0}, Vec3{10, 0, 0}) && garrison_bone_within(60.0f, Vec3{0, 0, 0}, Vec3{10, 0, 0}));
        CHECK(!garrison_bone_within(61.0f, Vec3{0, 0, 0}, Vec3{10, 0, 0}) && garrison_bone_within(180.0f, Vec3{0, 0, 0}, Vec3{-10, -1e-3f, 0}));
    }
}
}  // namespace bo3split

namespace bo3split {

void test_hardpoint_mode_default() {
    // S8 t12 (b): Fire_When_In_<Mode>_Mode defaults to 1 - without the No the hardpoint fires in DEFEND.
    GateWorld g(true);
    g.w.data.hardpoints[0].no_fire_modes.clear();
    CHECK(g.run(90) > 0);
}
}  // namespace bo3split

namespace bo3split {

void test_own_weapon_aim_point() {
    // S8 t17 / t27 [H] (0x54a270 step 4): the own weapon aims at the target position + its Ranged_Target_Z_Adjust
    // (0x398440), so a level shot at a hover hull (collision box above its origin) climbs to the hull's centre.
    OwnOpt opt;
    opt.target_z_adjust = 12.0f;
    OwnWorld o(opt, Vec3{100, 0, 0});
    const std::vector<uint32_t> f = o.shot_frames(60);
    CHECK(!f.empty());
    bool seen = false;
    for (size_t i = 0; i < o.w.sys->projectile_count(); ++i) {
        const ProjectileView v = o.w.sys->projectile(i);
        if (v.shooter != 1) continue;
        seen = true;
        const float want = 12.0f / std::sqrt(100.0f * 100.0f + 12.0f * 12.0f);
        CHECK(std::fabs(v.dir.z - want) < 1.0e-4f);
    }
    CHECK(seen);
}
}  // namespace bo3split

namespace bo3split {

bool same_dir(Vec3 d, Vec3 to) {
    const float len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
    return std::fabs(d.x - to.x / len) < 1.0e-4f && std::fabs(d.y - to.y / len) < 1.0e-4f && std::fabs(d.z - to.z / len) < 1.0e-4f;
}
}  // namespace bo3split

namespace bo3split {

void test_own_weapon_target_bones() {
    // S8 t27 (TACTICAL_COMBAT §3.1.1 "Own-weapon aim point, Target_Bones", 0x54a270) [H]: per shot, a target type with
    // Target_Bones gets one synced Get_Int(0, n - 1) (none for n = 1) and the shot aims at that bone's world translation;
    // the shooter's target hardpoint wins over the bones (its bone; not targetable -> the bare position, no z adjust).
    // Target at (100, 0, 0) facing +y: local (x, y, z) -> world (100 - y, x, z). Shooter at the origin, no fire bones.
    const std::vector<Vec3> bones = {Vec3{0, 10, 20}, Vec3{0, -10, 20}, Vec3{5, 0, 30}};
    auto world_of = [](Vec3 l) { return Vec3{100.0f - l.y, l.x, l.z}; };
    {  // three bones: every shot at one of them, more than one seen, one draw per shot more than without bones
        OwnOpt opt;
        opt.target_z_adjust = 12.0f;
        opt.recharge = 0.1f;
        opt.edit = [&](CombatData& d) { d.units[1].target_bones = bones; };
        OwnWorld o(opt, Vec3{100, 0, 0});
        o.w.sys->set_facing(2, Vec3{0, 1, 0});
        OwnOpt plain;
        plain.target_z_adjust = 12.0f;
        plain.recharge = 0.1f;
        OwnWorld p(plain, Vec3{100, 0, 0});
        p.w.sys->set_facing(2, Vec3{0, 1, 0});
        std::array<int, 3> hits{0, 0, 0};
        uint64_t shots = 0;
        for (int i = 0; i < 300; ++i) {
            const uint64_t d0 = o.w.rng.draws(), q0 = p.w.rng.draws();
            const uint64_t s0 = o.w.sys->stats().shots, t0 = p.w.sys->stats().shots;
            o.w.tick();
            p.w.tick();
            const uint64_t fired = o.w.sys->stats().shots - s0;
            if (shots == 0) {  // identical up to the first shot (later recharge draws differ): + the pick's Get_Int
                CHECK(fired == p.w.sys->stats().shots - t0);
                CHECK(o.w.rng.draws() - d0 > p.w.rng.draws() - q0 || fired == 0);
                if (fired == 0) CHECK(o.w.rng.draws() - d0 == p.w.rng.draws() - q0);
            }
            if (fired == 0) continue;
            shots += fired;
            const std::vector<Vec3> dirs = own_shot_dirs(o);
            CHECK(!dirs.empty());
            if (dirs.empty()) continue;
            int match = -1;
            for (size_t b = 0; b < bones.size(); ++b)
                if (same_dir(dirs.back(), world_of(bones[b]))) match = static_cast<int>(b);
            CHECK(match >= 0);
            if (match >= 0) ++hits[static_cast<size_t>(match)];
        }
        CHECK(shots >= 5);
        CHECK((hits[0] > 0 ? 1 : 0) + (hits[1] > 0 ? 1 : 0) + (hits[2] > 0 ? 1 : 0) >= 2);
    }
    {  // one bone: no draw (the same draws as a bone-less target), aimed at the bone
        OwnOpt opt;
        opt.target_z_adjust = 12.0f;
        opt.edit = [&](CombatData& d) { d.units[1].target_bones = {bones[2]}; };
        OwnWorld o(opt, Vec3{100, 0, 0});
        o.w.sys->set_facing(2, Vec3{0, 1, 0});
        OwnOpt plain;
        plain.target_z_adjust = 12.0f;
        OwnWorld p(plain, Vec3{100, 0, 0});
        p.w.sys->set_facing(2, Vec3{0, 1, 0});
        bool checked = false;
        for (int i = 0; i < 60; ++i) {
            const uint64_t s0 = o.w.sys->stats().shots;
            o.w.tick();
            p.w.tick();
            if (o.w.sys->stats().shots == s0 || checked) continue;
            const std::vector<Vec3> dirs = own_shot_dirs(o);
            CHECK(!dirs.empty() && same_dir(dirs.back(), world_of(bones[2])));
            checked = true;
        }
        CHECK(checked);
        CHECK(o.w.rng.draws() == p.w.rng.draws());
    }
    for (const bool targetable : {true, false}) {  // the ordered hardpoint: its bone (no draw), else the bare position
        OwnOpt opt;
        opt.target_z_adjust = 12.0f;
        opt.edit = [&](CombatData& d) {
            HardpointDesc h;
            h.kind = HardpointKind::DummyArt;
            h.health = 100.0f;
            h.targetable = targetable;
            h.attach_offset = Vec3{0, 0, 40};
            d.hardpoints.push_back(h);
            d.units[1].hardpoints = {0};
            d.units[1].target_bones = bones;
        };
        OwnWorld o(opt, Vec3{100, 0, 0});
        o.w.sys->order_attack(1, 2, 0);
        CHECK(o.w.sys->ordered_hardpoint(1) == 0);
        bool checked = false;
        for (int i = 0; i < 60 && !checked; ++i) {
            const uint64_t s0 = o.w.sys->stats().shots;
            o.w.tick();
            if (o.w.sys->stats().shots == s0) continue;
            const std::vector<Vec3> dirs = own_shot_dirs(o);
            CHECK(!dirs.empty() && same_dir(dirs.back(), targetable ? Vec3{100, 0, 40} : Vec3{100, 0, 0}));
            checked = true;
        }
        CHECK(checked);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_hardpoint_aim_target_bones() {
    // S8 t27 (TACTICAL_COMBAT §3.1.1 "Hardpoint aim for comparison", 0x383f70): the object aim tries the target's
    // Target_Bones in list order first (no draw; the first within range and arc); its last fallback is the position +
    // Ranged_Target_Z_Adjust (was the bare position).
    for (const int mode : {0, 1, 2}) {  // 0 no bones; 1 bones (first in range); 2 the first bone out of range
        World w;
        ProjectileDesc p;
        p.max_speed = 20.0f;
        w.data.projectiles.push_back(p);
        HardpointDesc hd;
        hd.kind = HardpointKind::WeaponLaser;
        hd.projectile = 0;
        hd.fire_range = 500.0f;
        hd.max_recharge = 1.0f;
        hd.bone_a = Vec3{0, 0, 10};
        w.data.hardpoints.push_back(hd);
        UnitDesc a;
        a.tactical_health = 100.0f;
        a.max_attack_distance = 500.0f;
        a.hardpoints = {0};
        const uint32_t ad = push_unit(w.data, a);
        UnitDesc t;
        t.tactical_health = 1000.0f;
        t.radius = 2.0f;
        t.ranged_target_z_adjust = 9.0f;
        if (mode == 1) t.target_bones = {Vec3{0, 0, 50}, Vec3{0, 0, 60}};
        if (mode == 2) t.target_bones = {Vec3{0, 0, 900}, Vec3{0, 0, 60}};
        const uint32_t td = push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
        w.add(1, ad, 0, Vec3{0, 0, 0});
        w.add(2, td, 1, Vec3{100, 0, 0});
        for (int i = 0; i < 60 && w.sys->projectile_count() == 0; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 1u);
        if (w.sys->projectile_count() == 0) continue;
        const Vec3 want = mode == 0 ? Vec3{100, 0, 9} : mode == 1 ? Vec3{100, 0, 50} : Vec3{100, 0, 60};
        CHECK(same_dir(w.sys->projectile(0).dir, want - Vec3{0, 0, 10}));
    }
}
}  // namespace bo3split

namespace bo3split {

void test_fire_at_position() {
    // S8 t26 (TACTICAL_COMBAT §3.1.1 "Fire-at-position order"): a LOBBING_SUPERWEAPON's own weapon fires only for an order.
    // The lobbing service (after WEAPON) aims the turret, stamps the frame, and Lobbing_Superweapon_Chargeup_Frames later
    // sets "charged"; the next WEAPON service skips its recharge wait, fires once at the point with no target object (no
    // lead, no z adjust) and clears the order.
    auto make = [](bool turret, uint32_t chargeup, float recharge) {
        OwnOpt opt;
        opt.lobbing = true;
        opt.recharge = recharge;
        opt.edit = [turret, chargeup](CombatData& d) {
            d.units[0].turret = turret;
            d.units[0].lobbing_chargeup_frames = chargeup;
        };
        return std::make_unique<OwnWorld>(opt, Vec3{100, 0, 0});
    };
    {  // the charge-up and the single shot
        auto o = make(true, 90, 60.0f);
        CHECK(o->run(30) == 0);  // no order: never fires (step 7)
        CHECK(o->w.sys->fire_at_position_state(1) == 0);
        CHECK(!o->w.sys->order_fire_at_position(2, Vec3{1, 1, 0}));  // not a lobbing superweapon
        const Vec3 at{150, 40, 3};
        CHECK(o->w.sys->order_fire_at_position(1, Vec3{10, 10, 0}));
        CHECK(o->w.sys->order_fire_at_position(1, at));  // a second order overwrites the point
        Vec3 got;
        CHECK(o->w.sys->fire_at_position_state(1, &got) == 1 && got.x == at.x && got.y == at.y && got.z == at.z);
        const uint32_t f0 = o->w.frame;
        bool seen = false;
        std::vector<uint32_t> shots;
        for (int i = 0; i < 200; ++i) {
            const uint64_t s0 = o->w.sys->stats().shots;
            const uint32_t f = o->w.frame;
            o->w.tick();
            if (o->w.sys->stats().shots == s0) continue;
            shots.push_back(f);
            for (size_t k = 0; k < o->w.sys->projectile_count(); ++k) {
                const ProjectileView v = o->w.sys->projectile(k);
                if (v.shooter != 1) continue;
                seen = true;
                CHECK(v.target == kNoObject);
                CHECK(same_dir(v.dir, at));  // the point as given: no lead, no z adjust
            }
        }
        CHECK(seen);
        CHECK(shots.size() == 1);
        if (!shots.empty()) CHECK(shots[0] == f0 + 91);  // stamped at f0, charged at f0 + 90, fired at f0 + 91
        CHECK(o->w.sys->fire_at_position_state(1) == 0);
        // the 60 s recharge was skipped for the shot; the next order fires again after its own charge-up
        CHECK(o->w.sys->order_fire_at_position(1, at));
        const uint32_t f1 = o->w.frame;
        const std::vector<uint32_t> again = o->shot_frames(200);
        CHECK(again.size() == 1 && !again.empty() && again[0] == f1 + 91);
    }
    {  // S8 t30: the presentation events - chargeup when the turret is aimed (stamp), charged at the end of the charge-up
        auto o = make(true, 90, 0.2f);
        const Vec3 at{150, 40, 0};
        CHECK(o->w.sys->order_fire_at_position(1, at));
        const uint32_t f0 = o->w.frame;
        std::vector<std::pair<uint32_t, bool>> seen;
        for (int i = 0; i < 120; ++i) {
            o->w.tick();
            for (const LobEvent& e : o->w.sys->lob_events()) {
                CHECK(e.id == 1 && e.point.x == at.x && e.point.y == at.y);
                seen.emplace_back(e.frame, e.charged);
            }
        }
        CHECK(seen == (std::vector<std::pair<uint32_t, bool>>{{f0, false}, {f0 + 90, true}}));
    }
    {  // no TURRET: the turret aim never succeeds, the order waits (no timeout)
        auto o = make(false, 90, 0.2f);
        CHECK(o->w.sys->order_fire_at_position(1, Vec3{150, 0, 0}));
        CHECK(o->run(400) == 0);
        CHECK(o->w.sys->fire_at_position_state(1) == 1);
    }
    {  // a grenade without a lob solution to the point: not aimed, no shot; a reachable point then charges and fires
        OwnOpt opt;
        opt.lobbing = true;
        opt.edit = [](CombatData& d) {
            d.projectiles[0] = spmat_grenade();  // R = 550
            d.units[0].turret = true;
            d.units[0].lobbing_chargeup_frames = 10;
        };
        OwnWorld o(opt, Vec3{100, 0, 0});
        CHECK(o.w.sys->order_fire_at_position(1, Vec3{2000, 0, 0}));
        CHECK(o.run(100) == 0);
        CHECK(o.w.sys->fire_at_position_state(1) == 1);
        CHECK(o.w.sys->order_fire_at_position(1, Vec3{300, 0, 0}));
        const uint32_t f0 = o.w.frame;
        const std::vector<uint32_t> s = o.shot_frames(60);
        CHECK(s.size() == 1 && !s.empty() && s[0] == f0 + 11);
        CHECK(o.w.sys->fire_at_position_state(1) == 0);
    }
    {  // N1: a charging order survives write -> read and fires on the same frame; the hash carries it
        auto a = make(true, 90, 0.2f);
        CHECK(a->w.sys->order_fire_at_position(1, Vec3{150, 40, 0}));
        a->run(40);
        ByteWriter wa;
        a->w.sys->write(wa);
        auto b = make(true, 90, 0.2f);
        ByteReader r(wa.bytes());
        CHECK(b->w.sys->read(r));
        b->w.frame = a->w.frame;
        b->w.rng = a->w.rng;
        CHECK(b->w.sys->fire_at_position_state(1) == 1);
        StateHasher ha, hb, hc;
        a->w.sys->hash_extended(ha);
        b->w.sys->hash_extended(hb);
        CHECK(ha.digest() == hb.digest());
        auto c = make(true, 90, 0.2f);
        c->w.sys->hash_extended(hc);
        CHECK(hc.digest() != ha.digest());
        const std::vector<uint32_t> sa = a->shot_frames(100), sb = b->shot_frames(100);
        CHECK(sa == sb && sa.size() == 1);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_scan_range_query() {
    // S8 t17 (FIGHTER_FLYING §4.10 A, Get_Scan_Range 0x566f50): a kUnitScanQuery unit whose formation answers
    // 0x4160a0(F, u, none) != 0 scans the cube R + Rr; a candidate beyond R is accepted only when 0x4160a0(F, u, c) != 0.
    // Without the flag the query is never asked and the cube is R.
    for (const int mode : {0, 1, 2}) {  // 0 no flag; 1 flag, c accepted; 2 flag, c refused
        TargetWorld t;  // attacker R 1000
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{1500, 0, 0});  // beyond R, inside R + Rr (1800)
        t.w.add(3, t.capital, 1, Vec3{0, 2500, 0});  // outside the cube
        std::vector<ObjectId> asked;
        t.w.sys->set_scan_range_query([&](ObjectId u, ObjectId c, float range, float& rr) -> uint8_t {
            CHECK(u == 1 && range == 1000.0f);
            asked.push_back(c);
            rr = 800.0f;
            return (c == kNoObject || mode == 1) ? 1 : 0;
        });
        if (mode != 0) t.w.sys->set_flag(1, kUnitScanQuery, true);
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == (mode == 1 ? 2 : kNoObject));
        CHECK(std::find(asked.begin(), asked.end(), 3) == asked.end());
        if (mode == 0) CHECK(asked.empty());
        if (mode != 0) CHECK(!asked.empty() && asked.front() == kNoObject && std::find(asked.begin(), asked.end(), 2) != asked.end());
    }
    {  // the formation does not answer (c = none -> 0): cube R, the far candidate is not even queried
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{1500, 0, 0});
        std::vector<ObjectId> asked;
        t.w.sys->set_scan_range_query([&](ObjectId, ObjectId c, float, float& rr) -> uint8_t {
            asked.push_back(c);
            rr = 800.0f;
            return 0;
        });
        t.w.sys->set_flag(1, kUnitScanQuery, true);
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == kNoObject);
        CHECK(asked.size() == 1 && asked[0] == kNoObject);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_shared_team_target() {
    // S8 t17 (FIGHTER_FLYING §4.10 B, 0x406370): members 11 / 12 / 30 of squadron 50. 11 and 12 scanned before any enemy
    // was there (their next scans are not due); 30 then scans with an empty target and picks 20: every member without a
    // live ordered target gets 20 (12 keeps its order on 21). A unit outside the squadron (13) is not touched.
    TargetWorld t;
    t.w.add(11, t.attacker, 0, Vec3{0, 0, 0});
    t.w.add(12, t.attacker, 0, Vec3{0, 0, 0});
    t.w.add(13, t.attacker, 0, Vec3{0, 0, 0});
    for (const ObjectId m : {11, 12}) t.w.sys->set_share_group(m, 50);
    t.w.tick();
    CHECK(t.w.sys->auto_target(11) == kNoObject && t.w.sys->next_scan(11) > t.w.frame + 1);
    t.w.add(20, t.capital, 1, Vec3{500, 0, 0});
    t.w.add(21, t.capital, 1, Vec3{-500, 0, 0});
    t.w.add(30, t.attacker, 0, Vec3{0, 0, 0});
    t.w.sys->set_share_group(30, 50);
    t.w.sys->order_attack(12, 21);  // also orders 11 (same squadron); 30 joined later in this test: re-cleared below
    CHECK(t.w.sys->ordered_target(11) == 21 && t.w.sys->ordered_target(12) == 21 && t.w.sys->ordered_target(13) == kNoObject);
    t.w.sys->clear_order(11);
    t.w.sys->clear_order(30);
    t.w.tick();
    CHECK(t.w.sys->auto_target(30) != kNoObject);
    const ObjectId pick = t.w.sys->auto_target(30);
    CHECK(t.w.sys->auto_target(11) == pick);           // shared
    CHECK(t.w.sys->ordered_target(12) == 21);          // ordered member kept
    CHECK(t.w.sys->auto_target(12) == kNoObject);
    CHECK(t.w.sys->auto_target(13) == kNoObject);  // 13 is not a member (its scan is not due)
    // Order_Attack on one member orders the squadron.
    t.w.sys->order_attack(30, 20);
    for (const ObjectId m : {11, 12, 30}) CHECK(t.w.sys->ordered_target(m) == 20);
}
}  // namespace bo3split

namespace bo3split {

void test_foc_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("foc data: skipped (ALAMO_FOC_DATA unset)\n");
        return;
    }
#ifdef ALAMO_COMBAT_TESTS_HAVE_VFS
    // The shipped game keeps Data\XML inside Config.meg: read through the VFS (MEGs + loose overrides).
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    std::string verr;
    const std::unique_ptr<vfs::Vfs> vfs_ptr = vfs::Vfs::create(vcfg, &verr);
    if (!vfs_ptr) {
        std::printf("FAIL foc data: VFS on %s: %s\n", root, verr.c_str());
        ++g_fail;
        return;
    }
    const FileProvider& fs = *vfs_ptr;
#else
    DirectoryProvider fs(root);
#endif
    std::vector<uint8_t> bytes;
    if (!fs.read("Data\\XML\\GameConstants.xml", bytes)) {
        std::printf("FAIL foc data: GameConstants.xml not found under %s\n", root);
        ++g_fail;
        return;
    }
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, "GameConstants.xml");
    data::GameConstants gc;
    for (data::XmlNode c : doc.root().children()) gc.fields.emplace_back(c.name(), c.text_trimmed());
    std::vector<std::string> diag;
    const DamageArmorTable t = DamageArmorTable::from_constants(gc, &diag);
    std::printf("foc data: %u damage types, %u armor types, %zu diagnostics\n", t.damage_count(), t.armor_count(), diag.size());
    CHECK(diag.empty());
    CHECK(t.damage_count() > 20 && t.armor_count() > 20);
    CHECK_F(t.factor(t.damage_index("Damage_Turbolaser"), t.armor_index("Armor_Fighter")), 0.3f);
    CHECK_F(t.factor(t.damage_index("Damage_Turbolaser"), t.armor_index("Shield_Capital")), 0.75f);
    CHECK_F(t.factor(t.damage_index("Damage_Default"), t.armor_index("Armor_Default")), 1.0f);

    objects::EnumRegistry enums;
    enums.load_dynamic(fs, nullptr);
    if (!fs.read("Data\\XML\\SpaceUnitTargetingPriorities.xml", bytes)) {
        std::printf("FAIL foc data: SpaceUnitTargetingPriorities.xml missing\n");
        ++g_fail;
        return;
    }
    const data::XmlDocument pdoc = data::XmlDocument::parse(bytes, "SpaceUnitTargetingPriorities.xml");
    diag.clear();
    const std::vector<PrioritySet> sets = load_priority_sets(pdoc.root(), enums, nullptr, &diag);
    std::printf("foc data: %zu space priority sets, %zu diagnostics (unit-exclusion type names need a resolver)\n", sets.size(),
                diag.size());
    const PrioritySet* fighter = nullptr;
    for (const PrioritySet& s : sets)
        if (iequals(s.name, "Fighter")) fighter = &s;
    CHECK(fighter != nullptr);
    if (fighter) {
        CHECK(fighter->attack.size() == 3);
        if (fighter->attack.size() == 3) {
            CHECK(fighter->attack[0].kind == PriorityEntry::Kind::Category);
            CHECK_F(fighter->attack[0].weight, 1.0f);
            CHECK_F(fighter->attack[2].weight, 3.0f);
            const auto transport = enums.get(objects::EnumId::GameObjectCategory).lookup("Transport");
            CHECK(transport && fighter->attack[0].mask == static_cast<uint64_t>(*transport));
        }
        CHECK(fighter->property_exclusions != 0);
    }
    // refixf t06: Capital_Attack_Move's Hard_Point_Priorities "Shield_Generator, Engine, FIGHTER_BAY, WEAPON_LASER,
    // WEAPON_ION_CANNON, TRACTOR_BEAM" are HardPointType values in list order; no hard-point name is unknown.
    const PrioritySet* cam = nullptr;
    for (const PrioritySet& s : sets)
        if (iequals(s.name, "Capital_Attack_Move")) cam = &s;
    CHECK(cam != nullptr);
    if (cam) {
        const auto hk = [](HardpointKind k) { return static_cast<int32_t>(k); };
        const std::vector<int32_t> want{hk(HardpointKind::ShieldGenerator), hk(HardpointKind::Engine),         hk(HardpointKind::FighterBay),
                                        hk(HardpointKind::WeaponLaser),     hk(HardpointKind::WeaponIonCannon), hk(HardpointKind::TractorBeam)};
        CHECK(cam->hardpoint_priorities == want && cam->hardpoint_exclusions.empty());
        CHECK_F(hardpoint_rank(*cam, hk(HardpointKind::FighterBay)), 3.0f);
        CHECK_F(hardpoint_rank(*cam, hk(HardpointKind::WeaponMissile)), 1e18f);
    }
    for (const std::string& m : diag) CHECK(m.find("hard point type") == std::string::npos);
}
}  // namespace bo3split

namespace bo3split {

// ================================================================================================
// benchmark: modifier aggregation
// ================================================================================================

void bench_modifiers() {
    for (const uint32_t n : {1000u, 10000u}) {
        uint32_t st = 7;
        auto rnd = [&st]() {
            st = st * 1103515245u + 12345u;
            return (st >> 8) & 0xFFFFu;
        };
        std::vector<ModifierSet> objs(n);
        ModifierSet players[2];
        for (ModifierSet& p : players)
            for (int i = 0; i < 6; ++i)
                p.insert(static_cast<ModList>(rnd() % kModListCount), static_cast<int32_t>(rnd() % 6),
                         static_cast<float>(rnd() % 100) / 100.0f - 0.5f, kPermanent);
        for (ModifierSet& o : objs)
            for (int i = 0; i < 4; ++i)
                o.insert(static_cast<ModList>(rnd() % kModListCount), static_cast<int32_t>(rnd() % 6),
                         static_cast<float>(rnd() % 100) / 100.0f - 0.5f, kPermanent);
        std::vector<std::array<float, kModListCount>> cache(n);
        std::vector<uint32_t> ver(n, 0xFFFFFFFFu);
        volatile float sink = 0.0f;
        using clk = std::chrono::steady_clock;
        const int reps = n >= 10000 ? 20 : 200;

        auto t0 = clk::now();
        for (int r = 0; r < reps; ++r)
            for (uint32_t i = 0; i < n; ++i) {
                float s = 0.0f;
                for (uint32_t l = 0; l < kModListCount; ++l) s += reduce_list(&players[i & 1], &objs[i], static_cast<ModList>(l));
                sink = sink + s;
            }
        const double per_query = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / (reps * double(n));

        t0 = clk::now();
        for (int r = 0; r < reps; ++r)
            for (uint32_t i = 0; i < n; ++i) {
                reduce_all(&players[i & 1], &objs[i], default_list_clamps(), cache[i]);
                sink = sink + cache[i][1];
            }
        const double per_all = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / (reps * double(n));

        // Steady state: version check + cached loads (11 lists), 2% of the objects dirty per tick.
        t0 = clk::now();
        for (int r = 0; r < reps; ++r) {
            for (uint32_t i = static_cast<uint32_t>(r % 50); i < n; i += 50) objs[i].insert(ModList::Damage, 9, 0.01f, kPermanent);
            for (uint32_t i = 0; i < n; ++i) {
                if (ver[i] != objs[i].version()) {
                    reduce_all(&players[i & 1], &objs[i], default_list_clamps(), cache[i]);
                    ver[i] = objs[i].version();
                }
                float s = 0.0f;
                for (uint32_t l = 0; l < kModListCount; ++l) s += cache[i][l];
                sink = sink + s;
            }
        }
        const double per_cached = std::chrono::duration<double, std::nano>(clk::now() - t0).count() / (reps * double(n));
        std::printf("bench modifiers N=%u: 11 lists/object  per-query reduce_list %.1f ns  reduce_all %.1f ns  "
                    "cached tick (2%% dirty) %.1f ns  -> %.2f ms/tick uncached vs %.3f ms/tick cached\n",
                    n, per_query, per_all, per_cached, per_query * n / 1e6, per_cached * n / 1e6);
        (void)sink;
    }
}
}  // namespace bo3split

namespace bo3split {

float yaw_of(Vec3 f) { return std::atan2(f.y, f.x) * 57.29578f; }
}  // namespace bo3split

namespace bo3split {

void test_targeting_states() {
    // s78f t03 (TACTICAL_COMBAT §3.1.1 "Targeting states, slots 47 / 48 and team copies").
    const auto turner = [](float rate) {
        OwnOpt opt;
        opt.edit = [rate](CombatData& d) { d.units[0].turn_rate = rate; };
        return opt;
    };
    {  // slot 48: target 90 degrees off, Max_Rate_Of_Turn 10 (x the land multiplier 1): state 3 and a 10-degree turn step
       // per service; the 9th service turns onto the target -> 4 at once (slot 48 re-tested), and the weapon fires.
        OwnWorld o(turner(10.0f), Vec3{0, 100, 0});
        StateHooks h;
        o.w.sys->set_hooks(&h);
        CHECK(o.w.sys->needs_turn(1));
        for (int i = 0; i < 8; ++i) {
            o.w.tick();
            CHECK(o.w.sys->targeting_state(1) == 3 && !o.w.sys->attacking(1));
        }
        CHECK(o.w.sys->stats().shots == 0);
        CHECK(std::fabs(yaw_of(o.w.sys->facing(1)) - 80.0f) < 1e-3f);
        CHECK(h.faced.size() == 8 && !h.idle.empty() && h.idle.front() == std::make_pair(ObjectId{1}, true));
        o.w.tick();
        CHECK(o.w.sys->targeting_state(1) == 4 && o.w.sys->attacking(1) && !o.w.sys->needs_turn(1));
        CHECK(std::fabs(yaw_of(o.w.sys->facing(1)) - 90.0f) < 1e-3f);
        CHECK(o.run(60) > 0);
    }
    {  // within half a frame of turn (tol 5): the facing is snapped onto the target, state 4 on the first service
        OwnWorld o(turner(10.0f), Vec3{100, 4, 0});
        StateHooks h;
        o.w.sys->set_hooks(&h);
        o.w.tick();
        CHECK(o.w.sys->targeting_state(1) == 4 && h.faced.size() == 1);
        CHECK(std::fabs(yaw_of(o.w.sys->facing(1)) - std::atan2(4.0f, 100.0f) * 57.29578f) < 1e-3f);
    }
    {  // half a frame >= 90: tolerance 0.01 degrees (a 1-degree miss needs the turn step, which lands on the target)
        OwnWorld o(turner(200.0f), Vec3{100, 1.75f, 0});
        o.w.tick();
        CHECK(o.w.sys->targeting_state(1) == 4);
        CHECK(std::fabs(yaw_of(o.w.sys->facing(1)) - std::atan2(1.75f, 100.0f) * 57.29578f) < 1e-3f);
    }
    {  // no locomotor (s78f t08 [H]): slot 48 is still tested - off the cone the structure holds state 3 (nothing turns
       // it, no shot); within the cone it is snapped onto the target and attacks
        OwnOpt opt = turner(10.0f);
        opt.edit = [](CombatData& d) {
            d.units[0].turn_rate = 10.0f;
            d.units[0].has_locomotor = false;
        };
        OwnWorld o(opt, Vec3{0, 100, 0});
        CHECK(o.run(30) == 0 && o.w.sys->targeting_state(1) == 3 && std::fabs(yaw_of(o.w.sys->facing(1))) < 1e-3f);
        OwnWorld c(opt, Vec3{100, 4, 0});
        c.w.tick();
        CHECK(c.w.sys->targeting_state(1) == 4);
        CHECK(std::fabs(yaw_of(c.w.sys->facing(1)) - std::atan2(4.0f, 100.0f) * 57.29578f) < 1e-3f);
    }
    {  // moving: state 4 without an Attack clip (fires on the move), 2 with one (no fire)
        OwnWorld a(turner(10.0f), Vec3{0, 100, 0}), b(turner(10.0f), Vec3{0, 100, 0});
        a.w.sys->set_flag(1, kUnitMoving, true);
        b.w.sys->set_flag(1, kUnitMoving, true);
        b.w.sys->set_flag(1, kUnitAttackClip, true);
        CHECK(a.run(60) > 0 && a.w.sys->targeting_state(1) == 4);
        CHECK(b.run(60) == 0 && b.w.sys->targeting_state(1) == 2);
        CHECK(std::fabs(yaw_of(a.w.sys->facing(1))) < 1e-3f);  // no turn while moving
    }
    {  // Is_Stationary_When_Attacking: state 4 is not written while moving (the old state stays: 1 for a new target)
        OwnOpt opt;
        opt.edit = [](CombatData& d) { d.units[0].stationary_when_attacking = true; };
        OwnWorld o(opt, Vec3{100, 0, 0});
        o.w.sys->set_flag(1, kUnitMoving, true);
        CHECK(o.run(30) == 0 && o.w.sys->targeting_state(1) == 1);
        o.w.sys->set_flag(1, kUnitMoving, false);
        CHECK(o.run(30) > 0 && o.w.sys->targeting_state(1) == 4);
    }
    {  // TURRET: the host's aim state - slewing (0) -> 3, reached (1) or not modelled (-1) -> 4
        OwnOpt opt;
        opt.edit = [](CombatData& d) { d.units[0].turret = true; };
        OwnWorld o(opt, Vec3{100, 0, 0});
        StateHooks h;
        h.aim = 0;
        o.w.sys->set_hooks(&h);
        CHECK(o.run(30) == 0 && o.w.sys->targeting_state(1) == 3);
        h.aim = 1;
        CHECK(o.run(30) > 0 && o.w.sys->targeting_state(1) == 4);
    }
    {  // out of range: 2 (target kept); a deployed unit drops it unless Max_Dist_Walk_While_Deployed > 0
        for (const float walk : {0.0f, 5.0f}) {
            OwnOpt opt;
            opt.edit = [walk](CombatData& d) {
                d.units[0].deploys = true;
                d.units[0].max_dist_walk_while_deployed = walk;
            };
            OwnWorld o(opt, Vec3{600, 0, 0}), u(opt, Vec3{600, 0, 0});
            o.w.sys->set_flag(1, kUnitDeploying, true);
            o.w.tick();
            u.w.tick();
            CHECK(u.w.sys->targeting_state(1) == 2 && u.w.sys->current_target(1) == 2);
            if (walk > 0.0f) CHECK(o.w.sys->targeting_state(1) == 2 && o.w.sys->current_target(1) == 2);
            else CHECK(o.w.sys->targeting_state(1) == 0 && o.w.sys->current_target(1) == kNoObject);
        }
    }
    {  // slot 47 at state 4 every service (the host draws only when the pose starts); a Set_Target to another object
       // reads as state 1 until the next service; the state survives write -> read and is hashed
        OwnWorld o(OwnOpt{}, Vec3{100, 0, 0});
        StateHooks h;
        h.draw = true;
        o.w.sys->set_hooks(&h);
        o.w.tick();
        o.w.tick();
        CHECK(h.idle.size() == 2 && o.w.sys->targeting_state(1) == 4);
        ByteWriter wa;
        o.w.sys->write(wa);
        OwnWorld c(OwnOpt{}, Vec3{100, 0, 0});
        ByteReader r(wa.bytes());
        CHECK(c.w.sys->read(r) && c.w.sys->targeting_state(1) == 4);
        StateHasher ha, hc;
        o.w.sys->hash_extended(ha);
        c.w.sys->hash_extended(hc);
        CHECK(ha.digest() == hc.digest());
        o.w.add(3, 1, 1, Vec3{120, 0, 0});
        o.w.sys->order_attack(1, 3);
        CHECK(o.w.sys->targeting_state(1) == 1 && !o.w.sys->attacking(1));
    }
}
}  // namespace bo3split

namespace bo3split {

void test_targeting_t08() {
    // s78f t08 (TACTICAL_COMBAT §3.1.1 "s78f t08 answers").
    {  // signal 0x1E: one AttackStateEvent on the first entry to 4 (cleared each tick, not raised again while it stays 4)
        OwnWorld o(OwnOpt{}, Vec3{100, 0, 0});
        o.w.tick();
        const auto ev = o.w.sys->attack_state_events();
        CHECK(ev.size() == 1 && ev[0].unit == 1 && ev[0].target == 2);
        o.w.tick();
        CHECK(o.w.sys->targeting_state(1) == 4 && o.w.sys->attack_state_events().empty());
    }
    {  // pending ability on the current target: state 4 is not written (Set_Target's 1 stays, no shot, no 0x1E) until the
       // ability is gone
        OwnWorld o(OwnOpt{}, Vec3{100, 0, 0});
        StateHooks h;
        h.pending = 2;
        o.w.sys->set_hooks(&h);
        CHECK(o.run(30) == 0 && o.w.sys->targeting_state(1) == 1 && o.w.sys->attack_state_events().empty());
        h.pending = kNoObject;
        CHECK(o.run(30) > 0 && o.w.sys->targeting_state(1) == 4);
    }
    {  // hold fire: a unit already attacking keeps 4 through the pending rule, but a target type with
       // Should_Attacker_Hold_Fire_For_Special_Ability turns it into 2 (no shots)
        for (const bool flag : {false, true}) {
            OwnOpt opt;
            opt.edit = [flag](CombatData& d) { d.units[1].hold_fire_for_special_ability = flag; };
            OwnWorld o(opt, Vec3{100, 0, 0});
            StateHooks h;
            o.w.sys->set_hooks(&h);
            o.w.tick();
            CHECK(o.w.sys->targeting_state(1) == 4);
            h.pending = 2;
            const uint64_t shots = o.run(30);
            if (flag) CHECK(shots == 0 && o.w.sys->targeting_state(1) == 2);
            else CHECK(shots > 0 && o.w.sys->targeting_state(1) == 4);
        }
    }
    {  // the auto-deploy check 0x568e60 + slot 47's deploy branch: an undeployed Auto_Deploys + Deploys unit with an ordered
       // target requests the deploy; Fire_Weapon_When_Undeployed (or no Auto_Deploys) never does
        for (int k = 0; k < 3; ++k) {
            OwnOpt opt;
            opt.edit = [k](CombatData& d) {
                d.units[0].deploys = true;
                d.units[0].auto_deploys = k != 2;
                d.units[0].fire_weapon_when_undeployed = k == 1;
            };
            OwnWorld o(opt, Vec3{100, 0, 0});
            StateHooks h;
            o.w.sys->set_hooks(&h);
            o.w.tick();
            if (k == 0) CHECK(h.deploy_requests > 0);
            else CHECK(h.deploy_requests == 0);
        }
    }
    {  // TURRET cannot aim (-2: beyond the rotate extent): the target is dropped; with the target the formation's attack
       // target and Should_Cause_Limited_Turrets_To_Turn the body turns instead (state 3, one turn step)
        for (const bool turn : {false, true}) {
            OwnOpt opt;
            opt.edit = [turn](CombatData& d) {
                d.units[0].turret = true;
                d.units[0].turn_rate = 10.0f;
                d.units[1].limited_turrets_turn = turn;
            };
            OwnWorld o(opt, Vec3{0, 100, 0});
            StateHooks h;
            h.aim = -2;
            o.w.sys->set_hooks(&h);
            o.w.sys->set_attack_goal(1, 2);
            o.w.tick();
            if (turn) {
                CHECK(o.w.sys->targeting_state(1) == 3 && o.w.sys->current_target(1) == 2);
                CHECK(std::fabs(yaw_of(o.w.sys->facing(1)) - 10.0f) < 1e-3f);
            } else {
                CHECK(o.w.sys->targeting_state(1) == 0 && o.w.sys->ordered_target(1) == kNoObject);
            }
        }
    }
}
}  // namespace bo3split

namespace bo3split {

void test_scan_player_order() {
    // s78g t04 (TACTICAL_COMBAT §2.4 correction, §2.10 item 4): Scan_Player restarts the local best at each visited
    // player, so the pick is the best of the LAST visited player (cyclic from the random start) with a candidate. Player 1
    // has a capital (priority 1, closer), player 2 a fighter (priority 3, farther): one shared best would always take the
    // capital; per player, the fighter wins whenever player 2 is visited after player 1 (start 0 or 1 of 3).
    uint32_t capital = 0, fighter = 0, same = 0;
    for (uint32_t seed = 1; seed <= 24; ++seed) {
        for (const bool split : {true, false}) {
            World w(3);
            w.rng = SimRng(seed);
            PrioritySet ps;
            ps.attack.push_back({PriorityEntry::Kind::Category, -1, kCatCapital, 1.0f});
            ps.attack.push_back({PriorityEntry::Kind::Category, -1, kCatFighter, 3.0f});
            w.data.priority_sets.push_back(ps);
            w.data.projectiles.push_back(ProjectileDesc{});
            UnitDesc a;
            a.tactical_health = 100.0f;
            a.max_attack_distance = 1000.0f;
            a.weapon_projectile = 0;
            a.weapon_recharge = 100.0f;
            a.priority_set = 0;
            const uint32_t ad = push_unit(w.data, a);
            UnitDesc c;
            c.tactical_health = 100.0f;
            c.radius = 10.0f;
            c.category_mask = kCatCapital;
            const uint32_t cd = push_unit(w.data, c);
            c.category_mask = kCatFighter;
            const uint32_t fd = push_unit(w.data, c);
            w.make({}, {PlayerInfo{}, PlayerInfo{}, PlayerInfo{}});
            w.add(1, ad, 0, Vec3{0, 0, 0});
            w.add(2, cd, 1, Vec3{100, 0, 0});
            w.add(3, fd, split ? 2 : 1, Vec3{500, 0, 0});
            w.tick();
            const ObjectId t = w.sys->auto_target(1);
            CHECK(t == 2 || t == 3);
            if (!split) {
                same += t == 2 ? 1u : 0u;  // one player: the ordinary rule (lower score) picks the capital
            } else {
                (t == 2 ? capital : fighter) += 1;
            }
        }
    }
    CHECK(same == 24);
    CHECK(capital > 0 && fighter > 0 && fighter > capital);
    std::printf("scan player order: capital %u, fighter %u of 24 split scans\n", capital, fighter);
}
}  // namespace bo3split

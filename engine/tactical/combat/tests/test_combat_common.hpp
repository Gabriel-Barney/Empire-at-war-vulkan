#pragma once
// Split out of test_combat.cpp (bo3 t03): shared includes, types and helpers; the tests live in test_combat_partN.cpp.
// S8 combat tests: hand-computed reference formulas (docs/re/TACTICAL_COMBAT.md 2-4, incl. the four
// disassembly-verified rules), modifier stacking, priority sets, damage pipeline, a scripted
// 2-sided space engagement whose per-tick CRC log must be identical for 1/4/N threads and match the
// golden file on every compiler, a per-tick allocation count and a modifier-aggregation benchmark.
//   alamo_tactical_combat_tests [--golden <file>] [--write-golden <file>] [--no-bench]
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/enums.hpp"
#include "alamo/tactical/combat/combat_system.hpp"
#include "alamo/tactical/combat/hero_clash.hpp"
#include "alamo/tactical/combat/weather.hpp"
#include "alamo/test/thread_parallel_for.hpp"
#include "alamo/test/support.hpp"
#ifdef ALAMO_COMBAT_TESTS_HAVE_MOVEMENT
#include "alamo/tactical/movement/passability.hpp"
#endif
#ifdef ALAMO_COMBAT_TESTS_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

// ---- allocation counter (counts only while enabled) -------------------------------------------
namespace bo3split {
inline std::atomic<uint64_t> g_allocs{0};
inline std::atomic<bool> g_count{false};
}  // namespace
using namespace bo3split;
// GCC flags free() inside a replaced operator delete (-Wmismatched-new-delete) although the replaced
// operator new allocates with malloc(); the pairing is correct by construction (as in test_render).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

using namespace alamo;
using namespace alamo::tactical;
using namespace alamo::tactical::combat;

namespace bo3split {

using alamo::tactical::Vec3;  // hides alamo::Vec3 (galaxy.hpp) inside this namespace

using ::alamo::test::g_fail;
#define CHECK_F(a, b)                                                                                     \
    do {                                                                                                  \
        const float va_ = (a), vb_ = (b);                                                                 \
        if (!(va_ == vb_)) {                                                                              \
            std::printf("FAIL %s:%d: %s == %s (%.9g vs %.9g)\n", __FILE__, __LINE__, #a, #b, static_cast<double>(va_), \
                        static_cast<double>(vb_));                                                        \
            ++g_fail;                                                                                     \
        }                                                                                                 \
    } while (0)
#define CHECK_NEAR(a, b, eps)                                                                             \
    do {                                                                                                  \
        const double va_ = (a), vb_ = (b);                                                                \
        if (!(std::fabs(va_ - vb_) <= (eps))) {                                                           \
            std::printf("FAIL %s:%d: %s ~ %s (%.9g vs %.9g)\n", __FILE__, __LINE__, #a, #b, va_, vb_);  \
            ++g_fail;                                                                                     \
        }                                                                                                 \
    } while (0)
Aabb box_of(Vec3 p, float r);
Vec3 norm(Vec3 v);
bool same(Vec3 a, Vec3 b);

// A battle-free sandbox: data, system, trees and the synced RNG.
struct World {
    CombatData data;
    std::unique_ptr<CombatSystem> sys;
    TreeSet trees;
    SimRng rng{12345u};
    uint32_t frame = 1;

    explicit World(uint32_t players = 2) : trees(players) {}
    void make(CombatConfig cfg = {}, std::vector<PlayerInfo> players = {PlayerInfo{}, PlayerInfo{}}) {
        // The S8 tests and golden pin the S8 behaviour: opportunity fire (added in S9, default on)
        // stays off here; the S9 tests (engine/tactical/abilities) cover it.
        cfg.opportunity_fire = false;
        sys = std::make_unique<CombatSystem>(data, std::move(cfg), std::move(players));
    }
    void add(ObjectId id, uint32_t desc, int32_t owner, Vec3 pos, Vec3 facing = {1.0f, 0.0f, 0.0f}, float health = 1.0f) {
        UnitSpawn s;
        s.id = id;
        s.desc = desc;
        s.owner = owner;
        s.position = pos;
        s.facing = facing;
        s.health_fraction = health;
        CHECK(sys->add_unit(s, frame, rng));
        TreeMembership m;
        m.owner = owner;
        CHECK(trees.add(static_cast<EntityHandle>(id), box_of(pos, data.units[desc].radius), 1u, m));
    }
    void tick(const ParallelFor& pf = serial_for) {
        sys->tick(frame, trees, rng, pf);
        for (const DeathEvent& d : sys->deaths()) trees.remove(static_cast<EntityHandle>(d.id));
        ++frame;
    }
};
uint32_t push_unit(CombatData& d, const UnitDesc& u);
void test_scan_cadence();
void test_recharge();
void test_inaccuracy_formula();
void test_range_lead_area();
void test_modifier_stacking();
void test_modifier_expiry();
void test_reduce_all_matches();
void test_mod_multiplier();
void test_priority_sets();
void test_damage_table();

// ================================================================================================
// system-level rules
// ================================================================================================

inline constexpr uint64_t kCatFighter = 0x1, kCatCapital = 0x8;
void inaccuracy_case(GameMode mode, float accuracy, float fixed_radius, float override_radius, bool use_table, float expect_r,
                     uint64_t target_category = kCatCapital);
void own_inaccuracy_case(GameMode mode, float fixed_radius, bool use_table, float expect_r, int xyz_draws);
void test_inaccuracy_system();

struct TargetWorld {
    World w;
    uint32_t attacker = 0, capital = 0, fighter = 0, hp_attacker = 0, hp_target = 0;
    explicit TargetWorld(float low_threshold = 0.0f) {
        w.data.projectiles.push_back(ProjectileDesc{});
        w.data.projectiles[0].max_speed = 10.0f;
        PrioritySet ps;
        ps.attack.push_back({PriorityEntry::Kind::Category, -1, kCatCapital, 1.0f});
        ps.attack.push_back({PriorityEntry::Kind::Category, -1, kCatFighter, 3.0f});
        w.data.priority_sets.push_back(ps);
        HardpointDesc cone;  // forward-only weapon, never fires (no projectile)
        cone.kind = HardpointKind::WeaponLaser;
        cone.fire_cone_width = 90.0f;
        cone.fire_cone_height = 180.0f;
        w.data.hardpoints.push_back(cone);
        HardpointDesc tgt_hp;
        tgt_hp.kind = HardpointKind::DummyArt;
        tgt_hp.targetable = true;
        tgt_hp.health = 10.0f;
        w.data.hardpoints.push_back(tgt_hp);
        UnitDesc a;
        a.tactical_health = 100.0f;
        a.max_attack_distance = 1000.0f;
        a.weapon_projectile = 0;
        a.weapon_recharge = 100.0f;  // never fires during the test
        a.priority_set = 0;
        attacker = push_unit(w.data, a);
        UnitDesc ah = a;
        ah.weapon_projectile = -1;
        ah.hardpoints = {0};
        hp_attacker = push_unit(w.data, ah);
        UnitDesc c;
        c.tactical_health = 100.0f;
        c.radius = 10.0f;
        c.category_mask = kCatCapital;
        capital = push_unit(w.data, c);
        UnitDesc f = c;
        f.category_mask = kCatFighter;
        fighter = push_unit(w.data, f);
        UnitDesc ct = c;
        ct.hardpoints = {1, 1};
        hp_target = push_unit(w.data, ct);
        CombatConfig cfg;
        cfg.low_health_threshold = low_threshold;
        w.make(cfg);
    }
};
void test_targeting_rules();
void test_circular_lead();
void test_turn_toward();

// ---- S8 t04: lob solver and grenade flight (docs/re/LOB_SOLVER.md) ------------------------------------------------

inline constexpr float kDeg = 57.29577951308232f;
float lob_dist(Vec3 a, Vec3 b);
ProjectileDesc spmat_grenade();
void test_lob_solver();

// A land (or space) sandbox with an SPMAT-like grenade thrower (id 1, player 0) and a target (id 2, player 1).
struct LobWorld {
    World w;
    uint32_t thrower = 0, target = 0;
    explicit LobWorld(GameMode mode = GameMode::Land, ProjectileDesc g = spmat_grenade()) {
        w.data.projectiles.push_back(g);
        UnitDesc t;
        t.tactical_health = 100.0f;
        t.weapon_projectile = 0;
        t.weapon_recharge = 5.0f;          // the first shot after 150 + 0..10 frames, then one per 150 + 0..10
        t.max_attack_distance = 100.0f;    // not a grenade weapon's range (§4)
        t.ranged_target_z_adjust = 0.0f;   // launch point at the unit's position
        thrower = push_unit(w.data, t);
        UnitDesc v;
        v.tactical_health = 1000.0f;
        v.radius = 2.0f;
        v.ranged_target_z_adjust = 0.0f;
        target = push_unit(w.data, v);
        CombatConfig cfg;
        cfg.mode = mode;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
    }
    void move(ObjectId id, Vec3 p) {
        w.sys->set_position(id, p);
        w.trees.update(static_cast<EntityHandle>(id), box_of(p, w.data.units[w.sys->desc_index(id)].radius));
    }
};
void test_saber_projectile();
void test_lob_targeting();
void test_lob_weapon_flight();

// One grenade launched by launch_projectile from (0, 0, 0) at `point` (no target) in a bare system.
struct LobLaunch {
    CombatData d;
    std::unique_ptr<CombatSystem> sys;
    TreeSet trees{2};
    SimRng rng{7u};
    uint32_t frame = 1;
    LobLaunch(GameMode mode, ProjectileDesc g, Vec3 point, const GroundGrid* grid = nullptr) {
        d.projectiles = {g};
        CombatConfig cfg;
        cfg.mode = mode;
        sys = std::make_unique<CombatSystem>(d, cfg, std::vector<PlayerInfo>{PlayerInfo{}, PlayerInfo{}});
        if (grid) {
            RocketTerrain rt;
            rt.grid = *grid;
            sys->set_rocket_terrain(std::move(rt));
        }
        ProjectileLaunch l;
        l.owner = 0;
        l.from = Vec3{0, 0, 0};
        l.point = point;
        CHECK(sys->launch_projectile(l, frame));
    }
    void tick() { sys->tick(frame++, trees, rng); }
};
GroundGrid flat_grid(uint32_t w, uint32_t h, int16_t level);
void test_straight_ground_test();
void test_lob_ground_and_expiry();
void test_lob_sticky();
void test_ground_segment();
void test_lob_object_ground_tie();
void test_lob_fuse_rounding();
void test_lob_hardpoint();
void test_lob_save();
void add_team_unit(TargetWorld& t, ObjectId id, uint32_t desc, int32_t owner, Vec3 p, uint32_t team, bool leader);
void test_team_targeting();
// OI tactical: GameConstants fields, the diminishing-firepower spline, targeting slots 44 / 45.
struct OneFileProvider final : FileProvider {  // serves Data\XML\GameConstants.xml from memory
    std::string text;
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        if (!exists(path)) return false;
        out.assign(text.begin(), text.end());
        return true;
    }
    bool exists(std::string_view path) const override { return iequals(path, "Data\\XML\\GameConstants.xml"); }
    std::string origin(std::string_view path) const override { return exists(path) ? "memory" : ""; }
};
void test_gaps_constants();
void test_s78n_can_hit();
void test_s78n_min_range_slot46();
void test_land_obstacle_and_weapon_index();

// Fog follow-up: targeting filter step 4 and the space-station special weapon through CombatHooks::target_hidden.
struct FogHooks final : CombatHooks {
    std::vector<ObjectId> hidden;  // hidden from player 0 (force 0) / from everyone (force 1)
    bool human0 = true;            // player 0 human: fog applies at force 0 (the fog's AI gate otherwise)
    mutable std::atomic<uint32_t> calls{0};
    bool target_hidden(int32_t player, ObjectId target, bool force) const override {
        calls.fetch_add(1, std::memory_order_relaxed);
        if (!force && !(player == 0 && human0)) return false;
        return std::find(hidden.begin(), hidden.end(), target) != hidden.end();
    }
};
void test_fog_filter();
void test_attack_move_object();
void test_hardpoint_fire_routing();
void test_precise_hit_hook();

struct DamageWorld {
    World w;
    uint32_t shielded = 0, plain = 0, attacker = 0, hp_unit = 0;
    uint16_t laser = 0;
    DamageWorld() {
        laser = w.data.damage.add_damage_type("Laser");
        const uint16_t hull = w.data.damage.add_armor_type("Hull");
        const uint16_t sh = w.data.damage.add_armor_type("ShieldArmor");
        w.data.damage.set(laser, hull, 2.0f);
        w.data.damage.set(laser, sh, 0.5f);
        ProjectileDesc p;  // 0: normal
        p.damage = 10.0f;
        w.data.projectiles.push_back(p);
        p.does_hitpoint_damage = false;  // 1
        w.data.projectiles.push_back(p);
        p.does_hitpoint_damage = true;
        p.does_shield_damage = false;  // 2
        w.data.projectiles.push_back(p);
        UnitAbilityDesc cause, take;
        cause.ability_type = kAbilityPowerToWeapons;
        cause.mult[static_cast<size_t>(ModKind::CauseDamage) - 1] = 2.0f;
        take.ability_type = kAbilityDefend;
        take.mult[static_cast<size_t>(ModKind::TakeDamage) - 1] = 1.5f;
        w.data.abilities = {cause, take};
        HardpointDesc hd;
        hd.kind = HardpointKind::Engine;
        hd.health = 100.0f;
        hd.destroyable = true;
        w.data.hardpoints.push_back(hd);
        UnitDesc s;
        s.tactical_health = 1000.0f;
        s.shield_points = 20.0f;
        s.armor_type = hull;
        s.shield_armor_type = sh;
        shielded = push_unit(w.data, s);
        UnitDesc pl;
        pl.tactical_health = 1000.0f;
        pl.abilities = {1, -1};
        plain = push_unit(w.data, pl);
        UnitDesc a;
        a.tactical_health = 100.0f;
        a.abilities = {0, -1};
        attacker = push_unit(w.data, a);
        UnitDesc h;
        h.tactical_health = 500.0f;
        h.hardpoints = {0, 0};
        h.destroy_when_all_hardpoints_destroyed = true;
        hp_unit = push_unit(w.data, h);
        CombatConfig cfg;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        w.make(cfg);
    }
    DamageRequest req(ObjectId v, float amount, uint8_t kind = kKindNormal) {
        DamageRequest r;
        r.victim = v;
        r.amount = amount;
        r.damage_type = laser;
        r.kind = kind;
        return r;
    }
};
void test_take_damage();
void test_shielded_only_when_deployed();
void test_shield_regen_system();
void test_hull_hardpoint_coupling();
void test_choose_target_hardpoint_by_type();
void test_hardpoint_disabled();
void test_station_upgrade_copy();
void test_modifier_cache();

// ================================================================================================
// scripted 2-sided space engagement
// ================================================================================================

struct Battle {
    World w;
    struct Mover {
        ObjectId id;
        Vec3 start, dir;
        float speed;
        uint32_t frames;
        float radius;
    };
    std::vector<Mover> movers;
    CrcRegistry registry;

    Battle() {
        CombatData& d = w.data;
        const uint16_t laser = d.damage.add_damage_type("Damage_Laser");
        const uint16_t missile = d.damage.add_damage_type("Damage_Missile");
        const uint16_t a_cap = d.damage.add_armor_type("Armor_Capital");
        const uint16_t s_cap = d.damage.add_armor_type("Shield_Capital");
        const uint16_t a_fig = d.damage.add_armor_type("Armor_Fighter");
        d.damage.set(laser, a_cap, 0.5f);
        d.damage.set(laser, s_cap, 1.0f);
        d.damage.set(missile, a_cap, 1.5f);
        d.damage.set(missile, s_cap, 0.75f);
        d.damage.set(missile, a_fig, 0.25f);
        ProjectileDesc turbo;
        turbo.name = "Turbolaser";
        turbo.max_speed = 25.0f;
        turbo.damage = 15.0f;
        turbo.damage_type = laser;
        turbo.max_flight_distance = 2200.0f;
        ProjectileDesc mis;
        mis.name = "Missile";
        mis.max_speed = 15.0f;
        mis.damage = 40.0f;
        mis.damage_type = missile;
        mis.blast_area_damage = 20.0f;
        mis.blast_area_range = 120.0f;
        mis.blast_dropoff = true;
        mis.blast_dropoff_tiers = 2;
        mis.blast_max_victims = 3;
        mis.damages_random_hardpoints = true;
        mis.homing = true;
        ProjectileDesc flak;
        flak.name = "FighterLaser";
        flak.max_speed = 30.0f;
        flak.damage = 4.0f;
        flak.damage_type = laser;
        flak.damage_delay_secs = 0.2f;
        d.projectiles = {turbo, mis, flak};

        HardpointDesc lz;
        lz.name = "HP_Laser";
        lz.name_crc = name_crc(lz.name);
        lz.kind = HardpointKind::WeaponLaser;
        lz.health = 300.0f;
        lz.destroyable = lz.targetable = true;
        lz.attach_offset = Vec3{40, 0, 0};
        lz.bone_a = Vec3{40, 10, 0};
        lz.bone_b = Vec3{40, -10, 0};
        lz.has_bone_b = true;
        lz.fire_cone_width = 160.0f;
        lz.fire_cone_height = 130.0f;
        lz.projectile = 0;
        lz.min_recharge = 3.0f;
        lz.max_recharge = 4.0f;
        lz.pulse_count = 4;
        lz.pulse_delay = 0.2f;
        lz.fire_range = 1500.0f;
        lz.inaccuracy = {{kCatFighter, 70.0f}, {kCatCapital, 10.0f}};
        lz.weapon_delay = {{kAbilityPowerToWeapons, 0.75f}};
        HardpointDesc ml = lz;
        ml.name = "HP_Missile";
        ml.name_crc = name_crc(ml.name);
        ml.kind = HardpointKind::WeaponMissile;
        ml.health = 200.0f;
        ml.has_bone_b = false;
        ml.randomize_between_fire_bones = false;
        ml.projectile = 1;
        ml.min_recharge = 5.0f;
        ml.max_recharge = 6.0f;
        ml.pulse_count = 2;
        ml.pulse_delay = 0.5f;
        ml.fire_range = 1800.0f;
        ml.fire_category_restrictions = kCatFighter;  // missiles never fire at fighters
        HardpointDesc sg;
        sg.name = "HP_Shield";
        sg.name_crc = name_crc(sg.name);
        sg.kind = HardpointKind::ShieldGenerator;
        sg.health = 250.0f;
        sg.destroyable = sg.targetable = true;
        sg.attach_offset = Vec3{-20, 0, 10};
        HardpointDesc en = sg;
        en.name = "HP_Engine";
        en.name_crc = name_crc(en.name);
        en.kind = HardpointKind::Engine;
        en.health = 200.0f;
        en.attach_offset = Vec3{-60, 0, 0};
        d.hardpoints = {lz, ml, sg, en};

        PrioritySet cap;
        cap.name = "Capital";
        cap.attack = {{PriorityEntry::Kind::Category, -1, kCatCapital, 1.0f}, {PriorityEntry::Kind::Category, -1, kCatFighter, 3.0f}};
        cap.hardpoint_priorities = {static_cast<int32_t>(HardpointKind::ShieldGenerator), static_cast<int32_t>(HardpointKind::WeaponLaser)};
        PrioritySet fig;
        fig.name = "Fighter";
        fig.attack = {{PriorityEntry::Kind::Category, -1, kCatFighter, 1.0f}, {PriorityEntry::Kind::Category, -1, kCatCapital, 2.0f}};
        d.priority_sets = {cap, fig};

        UnitAbilityDesc ptw;
        ptw.ability_type = kAbilityPowerToWeapons;
        ptw.mult[static_cast<size_t>(ModKind::FireRate) - 1] = 1.5f;
        ptw.mult[static_cast<size_t>(ModKind::WeaponDelay) - 1] = 0.75f;
        d.abilities = {ptw};

        UnitDesc ship;
        ship.name = "Destroyer";
        ship.tactical_health = 2000.0f;
        ship.shield_points = 600.0f;
        ship.shield_refresh_rate = 30.0f;
        ship.armor_type = a_cap;
        ship.shield_armor_type = s_cap;
        ship.radius = 80.0f;
        ship.max_attack_distance = 1500.0f;
        ship.category_mask = kCatCapital;
        ship.priority_set = 0;
        ship.stickiness_threshold = 4.0f;
        ship.hardpoints = {0, 0, 1, 2, 3};
        ship.abilities = {0, -1};
        ship.hit_particle_count = 3;
        ship.shield_hit_particle_count = 2;
        UnitDesc fighter;
        fighter.name = "Fighter";
        fighter.tactical_health = 60.0f;
        fighter.armor_type = a_fig;
        fighter.radius = 8.0f;
        fighter.max_attack_distance = 400.0f;
        fighter.fire_inaccuracy_fixed_radius = 3.0f;
        fighter.category_mask = kCatFighter;
        fighter.priority_set = 1;
        fighter.weapon_projectile = 2;
        fighter.weapon_recharge = 1.0f;
        fighter.weapon_pulse_count = 3;
        fighter.weapon_pulse_delay = 0.15f;
        fighter.hit_particle_count = 2;
        d.units = {ship, fighter};

        CombatConfig cfg;
        cfg.grain = 2;  // small ranges so 4/N threads really split the work
        cfg.max_secs_for_ae_delayed_damage = 0.5f;
        cfg.low_health_threshold = 0.25f;
        w.make(cfg);
        w.sys->reserve(64, 4096, 64, 1024);

        // Player 0 on the left facing +x, player 1 on the right facing -x.
        ObjectId id = 1;
        for (int side = 0; side < 2; ++side) {
            const float sx = side == 0 ? -1.0f : 1.0f;
            const Vec3 facing{-sx, 0.0f, 0.0f};
            for (int i = 0; i < 2; ++i) {
                const Vec3 p{sx * 1000.0f, (i == 0 ? -300.0f : 300.0f), 0.0f};
                spawn(id++, 0, side, p, facing, Vec3{-sx, 0, 0}, 0.5f, 600);
            }
            for (int i = 0; i < 8; ++i) {
                const Vec3 p{sx * 900.0f, -210.0f + 60.0f * static_cast<float>(i), static_cast<float>(i % 3) * 10.0f};
                const Vec3 dir = norm(Vec3{-sx * 750.0f, -p.y * 0.5f, 0.0f});
                spawn(id++, 1, side, p, dir, dir, 4.0f, 190);
            }
        }
        w.sys->player_modifiers(0).insert(ModList::Damage, 1, 0.2f, kPermanent, name_crc("AURA_A"), 1);
        w.sys->player_modifiers(0).insert(ModList::Damage, 1, 0.1f, kPermanent, name_crc("AURA_A"), 2);
        w.sys->player_modifiers(0).insert(ModList::Damage, 2, 0.05f, kPermanent);
        w.sys->player_modifiers(1).insert(ModList::Defense, 3, 0.15f, kPermanent);
        w.sys->register_crc(registry, "combat");
        registry.add("rng", [this](StateHasher& h) { w.rng.hash(h); });
    }
    void spawn(ObjectId id, uint32_t desc, int32_t owner, Vec3 p, Vec3 facing, Vec3 dir, float speed, uint32_t frames) {
        w.add(id, desc, owner, p, facing);
        movers.push_back(Mover{id, p, dir, speed, frames, w.data.units[desc].radius});
    }
    void script(uint32_t f) {
        if (f == 50) w.sys->order_attack(13, 1);  // a player-1 fighter is ordered onto destroyer 1
        if (f == 100) {
            if (ModifierSet* m = w.sys->unit_modifiers(11))
                m->insert(ModList::FireRate, 4, 0.3f, expiry_frame(f, 10.0f, 30));
        }
        if (f == 300) w.sys->set_ability_active(1, 0, true);  // POWER_TO_WEAPONS on destroyer 1
        for (const Mover& m : movers) {
            if (!w.sys->alive(m.id)) continue;
            const float t = static_cast<float>(f < m.frames ? f : m.frames);
            const Vec3 p = m.start + m.dir * (m.speed * t);
            w.sys->set_position(m.id, p);
            w.trees.update(static_cast<EntityHandle>(m.id), box_of(p, m.radius));
        }
        w.trees.maintain(static_cast<double>(f) / 30.0);
    }
    CrcLog run(uint32_t ticks, const ParallelFor& pf, bool capture = true) {
        CrcLog log;
        if (capture) {
            log.systems = registry.names();
            log.comments.push_back("S8 scripted space engagement, 2 players x (2 destroyers + 8 fighters), seed 12345");
        }
        for (uint32_t i = 0; i < ticks; ++i) {
            const uint32_t f = w.frame;
            script(f);
            w.tick(pf);
            // OI fog: one shot event per fired shot, target owner = an enemy of the shooter.
            for (const ShotEvent& e : w.sys->shot_events()) {
                ++shot_events;
                if (e.shooter == kNoObject || e.target == kNoObject || e.target_owner < 0) ++bad_shot_events;
            }
            if (capture) log.records.push_back(registry.capture(f));
        }
        return log;
    }
    uint64_t shot_events = 0, bad_shot_events = 0;  // OI fog
};
void test_battle(const char* golden, const char* write_golden);
void test_allocations();
uint64_t combat_digest(const CombatSystem& s, const SimRng& rng);

inline std::vector<uint8_t> combat_bytes(const CombatSystem& s) {
    ByteWriter w;
    s.write(w);
    return w.bytes();
}
bool load_battle(Battle& b, const std::vector<uint8_t>& combat, const std::vector<uint8_t>& rng, uint32_t frame);
void test_save_load();
void put_u32_at(std::vector<uint8_t>& v, size_t off, uint32_t x);
void test_save_rejects();
void add_member(World& w, ObjectId id, uint32_t desc, int32_t owner, Vec3 p, uint32_t team);
bool near_rel(float a, float b);
void test_stickiness_override();
void test_weather_save();
float dist3(Vec3 a, Vec3 b);

struct RocketKit {
    RocketParams rp;
    RocketTerrain tr;
    SimRng rng{4242u};
    RocketContext ctx(Vec3 launch) {
        RocketContext c;
        c.params = &rp;
        c.terrain = &tr;
        c.rng = &rng;
        c.launch = launch;
        return c;
    }
};
RocketParams rocket_params(float cd, float co, float sd, float mfd, bool on_terrain, bool explode);
void test_rocket_spline();
void test_rocket_leg();
void test_rocket_plans();
void test_rocket_replan();

// A combat world with one rocket shooter (player 0) and a target (player 1), optionally a missile-shield emitter.
struct RocketWorld {
    World w;
    explicit RocketWorld(GameMode mode, bool on_terrain = false, bool emitter = false, Vec3 emitter_pos = {-300, -40, 0},
                         bool explode = false) {
        ProjectileDesc rk;
        rk.name = "Rocket";
        rk.max_speed = 12.0f;
        rk.damage = 5.0f;
        rk.category = ProjectileCategory::Rocket;
        rk.max_flight_distance = 1500.0f;
        rk.rocket_curve_distance = 50.0f;
        rk.rocket_curve_offset = 10.0f;
        rk.rocket_straight_distance = 0.0f;
        rk.target_point_on_terrain = on_terrain;
        rk.explode_when_reached_target_radius = explode;
        w.data.projectiles = {rk};
        UnitDesc sh;
        sh.name = "Shooter";
        sh.tactical_health = 1e6f;
        sh.radius = 5.0f;
        sh.max_attack_distance = 1200.0f;
        sh.weapon_projectile = 0;
        sh.weapon_recharge = 1.0f;
        const uint32_t s = push_unit(w.data, sh);
        UnitDesc t;
        t.name = "Target";
        t.tactical_health = 1e6f;
        t.radius = 20.0f;
        const uint32_t ti = push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = mode;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
        RocketTerrain tr;
        tr.height = [](float x, float) { return 0.02f * x; };
        tr.has_box = true;
        tr.hx = tr.hy = 4000.0f;
        w.sys->set_rocket_terrain(tr);
        w.add(1, s, 0, Vec3{-800, 0, mode == GameMode::Land ? -16.0f : 0.0f});
        w.add(2, ti, 1, Vec3{0, 0, 0});
        w.add(3, ti, 1, emitter_pos);
        if (emitter) {
            w.sys->set_emitter_radius(3, 200.0f);
            w.sys->add_emitter_entry(3);
        }
        w.sys->order_attack(1, 2);
    }
};
uint64_t rocket_digest(const CombatSystem& s, const SimRng& rng);
void test_rocket_flight_system();

// ================================================================================================
// S8 t08 / S7 t15 A2 / S8 t06 B4: units with both weapon sources, the recharge while suspended, fire bones
// ================================================================================================

// A land shooter (id 1, player 0) with its own weapon (projectile 0) and two weapon hardpoints (projectile 1) facing a
// tough target (id 2, player 1) 100 away. Slot 0 of the shooter is ROCKET_ATTACK (11).
struct SourceFlags {
    bool dep = true, undep = true, rocket = true, normal = true;
};
struct SourceWorld {
    World w;
    uint32_t last_serial = 0;
    SourceWorld(SourceFlags own, SourceFlags hp, bool grenade_own) {
        ProjectileDesc op;
        op.name = "own";
        op.max_speed = 20.0f;
        w.data.projectiles.push_back(grenade_own ? spmat_grenade() : op);
        ProjectileDesc hpp;
        hpp.name = "hp";
        hpp.max_speed = 20.0f;
        w.data.projectiles.push_back(hpp);
        HardpointDesc hd;
        hd.kind = HardpointKind::WeaponLaser;
        hd.projectile = 1;
        hd.min_recharge = hd.max_recharge = 0.2f;
        hd.fire_range = 150.0f;
        hd.fire_when_deployed = hp.dep;
        hd.fire_when_undeployed = hp.undep;
        hd.fire_when_rocket_attack = hp.rocket;
        hd.fire_when_normal_attack = hp.normal;
        w.data.hardpoints.push_back(hd);
        UnitAbilityDesc rocket;
        rocket.ability_type = 11;
        w.data.abilities.push_back(rocket);
        UnitDesc s;
        s.tactical_health = 100.0f;
        s.max_attack_distance = 300.0f;
        s.weapon_projectile = 0;
        s.weapon_recharge = 0.2f;
        s.deploys = true;
        s.hardpoints = {0, 0};
        s.abilities = {0, -1};
        s.fire_weapon_when_deployed = own.dep;
        s.fire_weapon_when_undeployed = own.undep;
        s.fire_weapon_when_rocket_attack = own.rocket;
        s.fire_weapon_when_normal_attack = own.normal;
        push_unit(w.data, s);
        UnitDesc t;
        t.tactical_health = 1.0e7f;
        t.radius = 5.0f;
        push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
        w.add(1, 0, 0, Vec3{0, 0, 0});
        w.add(2, 1, 1, Vec3{100, 0, 0});
    }
    // New projectiles per descriptor over `ticks` ticks.
    std::array<uint32_t, 2> run(int ticks) {
        std::array<uint32_t, 2> n{0, 0};
        for (int i = 0; i < ticks; ++i) {
            w.tick();
            for (size_t k = 0; k < w.sys->projectile_count(); ++k) {
                const ProjectileView pv = w.sys->projectile(k);
                if (pv.serial <= last_serial) continue;
                last_serial = pv.serial;
                if (pv.desc < 2) ++n[pv.desc];
            }
        }
        return n;
    }
};
void test_weapon_sources();

// A hardpoint-only shooter (id 1) whose weapon may not fire in DEFEND (slot 0) or at Air; an Air target (id 2) and a
// ground target (id 3), both in range. Opportunity fire on.
struct GateWorld {
    World w;
    explicit GateWorld(bool defend_active) {
        ProjectileDesc p;
        p.max_speed = 20.0f;
        w.data.projectiles.push_back(p);
        HardpointDesc hd;
        hd.kind = HardpointKind::WeaponLaser;
        hd.projectile = 0;
        hd.min_recharge = hd.max_recharge = 0.2f;
        hd.fire_range = 300.0f;
        hd.fire_category_restrictions = kCatFighter;  // "not vs Air"
        hd.no_fire_modes = {1};                        // Fire_When_In_Defend_Mode No
        w.data.hardpoints.push_back(hd);
        UnitAbilityDesc defend;
        defend.ability_type = 1;
        w.data.abilities.push_back(defend);
        UnitDesc s;
        s.tactical_health = 100.0f;
        s.max_attack_distance = 300.0f;
        s.hardpoints = {0};
        s.abilities = {0, -1};
        push_unit(w.data, s);
        UnitDesc air;
        air.tactical_health = 1.0e7f;
        air.radius = 5.0f;
        air.category_mask = kCatFighter;
        push_unit(w.data, air);
        UnitDesc ground = air;
        ground.category_mask = kCatCapital;
        push_unit(w.data, ground);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        cfg.inaccuracy_enabled = false;
        w.sys = std::make_unique<CombatSystem>(w.data, cfg, std::vector<PlayerInfo>{PlayerInfo{}, PlayerInfo{}});  // opportunity on
        w.add(1, 0, 0, Vec3{0, 0, 0});
        w.add(2, 1, 1, Vec3{100, 0, 0});
        w.add(3, 2, 1, Vec3{0, 120, 0});
        if (defend_active) w.sys->set_ability_active(1, 0, true);
    }
    uint64_t run(int ticks) {
        for (int i = 0; i < ticks; ++i) w.tick();
        return w.sys->stats().shots;
    }
};
void test_hardpoint_state_gate();

// S7 t15 A2 (MOVEMENT §3.1.1): a WeaponBehavior recharge keeps elapsing while the weapon is suspended (jetpack).
struct SuspendWorld {
    World w;
    SuspendWorld() {
        ProjectileDesc p;
        p.max_speed = 20.0f;
        w.data.projectiles.push_back(p);
        UnitDesc s;
        s.tactical_health = 100.0f;
        s.max_attack_distance = 300.0f;
        s.weapon_projectile = 0;
        s.weapon_recharge = 5.0f;  // 150 + Get_Int(0, 10) frames
        push_unit(w.data, s);
        UnitDesc t;
        t.tactical_health = 1.0e7f;
        t.radius = 5.0f;
        push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
        w.add(1, 0, 0, Vec3{0, 0, 0});
        w.add(2, 1, 1, Vec3{100, 0, 0});
    }
    // Ticks until the next shot (at most `limit`); returns the frame of the shot, 0 when none.
    uint32_t until_shot(int limit) {
        for (int i = 0; i < limit; ++i) {
            const uint64_t s0 = w.sys->stats().shots;
            const uint32_t f = w.frame;
            w.tick();
            if (w.sys->stats().shots != s0) return f;
        }
        return 0;
    }
};
void test_weapon_suspend_elapse();

// S8 t06 B4 (LOB_SOLVER §5.1): WeaponBehavior shots leave from the fire bones in turn.
struct MuzzleWorld {
    World w;
    explicit MuzzleWorld(std::vector<Vec3> bones, bool model = true, bool grenade_weapon = false, size_t weapon_set = 0) {
        ProjectileDesc p;
        p.max_speed = 20.0f;
        w.data.projectiles.push_back(grenade_weapon ? spmat_grenade() : p);
        w.data.projectiles.push_back(p);
        UnitDesc s;
        s.tactical_health = 100.0f;
        s.max_attack_distance = 300.0f;
        s.weapon_projectile = 0;
        s.weapon_recharge = 0.2f;
        s.ranged_target_z_adjust = 7.0f;
        s.muzzle_model = model;
        s.muzzle_bones = std::move(bones);
        for (size_t i = 0; i < weapon_set; ++i) s.weapon_projectiles.push_back(static_cast<int32_t>(i % 2));
        push_unit(w.data, s);
        UnitDesc t;
        t.tactical_health = 1.0e7f;
        t.radius = 5.0f;
        push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
        w.add(1, 0, 0, Vec3{10, 20, 0}, Vec3{0, 1, 0});  // facing +y: local (x, y, z) -> world (-y, x, z)
        w.add(2, 1, 1, Vec3{10, 120, 0});
    }
    // Launch points of the shots fired over `ticks` ticks (spawn position: not yet stepped).
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
    uint32_t last = 0;
};
void test_muzzle_bones();

// ================================================================================================
// S8 t10 - t12: own-weapon gate steps 5-7, first service, own-player sweep, garrison fire bones
// ================================================================================================

// An own-weapon shooter (id 1, player 0, type 0) at the origin and a tough target (id 2, player 1) at `at`; the shooter
// is ordered onto the target. `opt` edits the shooter type before the system is made; `start` is the spawn frame.
struct OwnOpt {
    float recharge = 0.2f;
    int32_t pulses = 1;
    float pulse_delay = 0.0f;
    float min_range = 0.0f;
    bool idle_fire = false, lobbing = false;
    int8_t weapons_enabled_flag = -1;  // ability slot 0's WEAPONS_ENABLED flag (-1: no ability)
    float target_z_adjust = 0.0f;      // the target type's Ranged_Target_Z_Adjust
    std::function<void(CombatData&)> edit;  // S8 t26 / t27: last edits before the system is made (units 0 / 1)
};
struct OwnWorld {
    World w;
    OwnWorld(OwnOpt o, Vec3 at, uint32_t start = 1) {
        ProjectileDesc p;
        p.max_speed = 20.0f;
        w.data.projectiles.push_back(p);
        if (o.weapons_enabled_flag >= 0) {
            UnitAbilityDesc a;
            a.ability_type = 1;
            a.flag[1] = o.weapons_enabled_flag;  // Mod_Flag index 1 = WEAPONS_ENABLED
            w.data.abilities.push_back(a);
        }
        UnitDesc s;
        s.tactical_health = 100.0f;
        s.max_attack_distance = 300.0f;
        s.min_attack_distance = o.min_range;
        s.weapon_projectile = 0;
        s.weapon_recharge = o.recharge;
        s.weapon_pulse_count = o.pulses;
        s.weapon_pulse_delay = o.pulse_delay;
        s.can_fighter_fire_when_idle = o.idle_fire;
        s.lobbing_superweapon = o.lobbing;
        if (o.weapons_enabled_flag >= 0) s.abilities = {0, -1};
        push_unit(w.data, s);
        UnitDesc t;
        t.tactical_health = 1.0e7f;
        t.radius = 5.0f;
        t.ranged_target_z_adjust = o.target_z_adjust;
        push_unit(w.data, t);
        if (o.edit) o.edit(w.data);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
        w.frame = start;
        w.add(1, 0, 0, Vec3{0, 0, 0});
        w.add(2, 1, 1, at);
        w.sys->order_attack(1, 2);
    }
    void move(ObjectId id, Vec3 p) {
        w.sys->set_position(id, p);
        w.trees.update(static_cast<EntityHandle>(id), box_of(p, w.data.units[w.sys->desc_index(id)].radius));
    }
    uint64_t run(int ticks) {
        const uint64_t s0 = w.sys->stats().shots;
        for (int i = 0; i < ticks; ++i) w.tick();
        return w.sys->stats().shots - s0;
    }
    // Frames of the shots fired over `ticks` ticks.
    std::vector<uint32_t> shot_frames(int ticks) {
        std::vector<uint32_t> out;
        for (int i = 0; i < ticks; ++i) {
            const uint64_t s0 = w.sys->stats().shots;
            const uint32_t f = w.frame;
            w.tick();
            if (w.sys->stats().shots != s0) out.push_back(f);
        }
        return out;
    }
};
void test_own_weapon_targeting_state();
void test_own_weapon_gate_steps();
void test_hardpoints_fire_while_suspended();
void test_first_service();
void test_own_player_sweep();
void test_garrison_fire_bones();
void test_hardpoint_mode_default();
void test_own_weapon_aim_point();

// The directions of shooter 1's projectiles spawned this tick (not stepped yet), serial order.
inline std::vector<Vec3> own_shot_dirs(const OwnWorld& o) {
    std::vector<Vec3> out;
    for (size_t i = 0; i < o.w.sys->projectile_count(); ++i) {
        const ProjectileView v = o.w.sys->projectile(i);
        if (v.shooter == 1) out.push_back(v.dir);
    }
    return out;
}
bool same_dir(Vec3 d, Vec3 to);
void test_own_weapon_target_bones();
void test_hardpoint_aim_target_bones();
void test_fire_at_position();
void test_scan_range_query();
void test_shared_team_target();
void test_foc_data();
void bench_modifiers();

// ================================================================================================
// s78f t03 / t04: targeting states 0-4, slots 47 / 48, TeamTargeting copy, charge-up turret slew
// ================================================================================================

struct StateHooks final : CombatHooks {
    std::vector<std::pair<ObjectId, bool>> idle;  // slot 47 calls (id, arg)
    std::vector<ObjectId> faced;                  // facing_set calls
    int8_t aim = -1;                              // turret_aim_state answer
    bool draw = false;                            // attack_idle draws Get_Float(0.2, 0.75) like the host
    void attack_idle(ObjectId id, bool arg, uint32_t, SimRng& rng) override {
        idle.emplace_back(id, arg);
        if (draw) (void)rng.range_float(0.2f, 0.75f);
    }
    void facing_set(ObjectId id, Vec3) override { faced.push_back(id); }
    int8_t turret_aim_state(ObjectId) const override { return aim; }
    ObjectId pending = kNoObject;  // s78f t08: pending_ability_target answer
    uint32_t deploy_requests = 0;  // s78f t08: request_deploy calls
    ObjectId pending_ability_target(ObjectId) const override { return pending; }
    void request_deploy(ObjectId, uint32_t) override { ++deploy_requests; }
};
float yaw_of(Vec3 f);
void test_targeting_states();
void test_targeting_t08();
void test_scan_player_order();
void test_team_handover_and_damage();
void test_team_targeting_copy();
void test_squad_value();
void test_convert_team();
void test_convert_victim_test();
void test_s78n_review_combat();
void test_hover_raised_melee();
void test_incognito_targeting();
void test_team_container_slot();
void test_team_head_alone();
void test_convert_container_unit();
void test_charge_up_slew();

// ================================================================================================
// s78n t06: hero clashes (docs/re/HERO_CLASH.md §1-§6)
// ================================================================================================

inline constexpr const char* kClashXml = R"(<?xml version="1.0"?>
<Hero_Clashes>
  <Hero_Clash Name="Duel_A_B">
    <Clash_Type>Melee_Duel</Clash_Type>
    <Clash_Range>100.0</Clash_Range>
    <Involved_Hero_Types>Hero_A, Hero_B</Involved_Hero_Types>
    <Play_Conversation_Events>No</Play_Conversation_Events>
    <Combat_Distance>17.65</Combat_Distance>
    <Damage_Amount>0.0</Damage_Amount>
    <Damage_Percentage>0.15</Damage_Percentage>
    <First_Hero_Win_Exchange_Chance>0.40</First_Hero_Win_Exchange_Chance>
    <Second_Hero_Win_Exchange_Chance>0.30</Second_Hero_Win_Exchange_Chance>
    <First_Hero_Damage_Multiplier>2.5</First_Hero_Damage_Multiplier>
    <Second_Hero_Damage_Multiplier>1.0</Second_Hero_Damage_Multiplier>
    <First_Hero_Type>Hero_A</First_Hero_Type>
    <Second_Hero_Type>Hero_B</Second_Hero_Type>
    <First_Hero_Win_Anim_Type>HC_WIN</First_Hero_Win_Anim_Type>
    <First_Hero_Lose_Anim_Type>HC_LOSE</First_Hero_Lose_Anim_Type>
    <First_Hero_Draw_Anim_Type>HC_DRAW</First_Hero_Draw_Anim_Type>
    <First_Hero_Conversation_Anim_Type>IDLE</First_Hero_Conversation_Anim_Type>
    <Second_Hero_Win_Anim_Type>HC_WIN</Second_Hero_Win_Anim_Type>
    <Second_Hero_Lose_Anim_Type>HC_LOSE</Second_Hero_Lose_Anim_Type>
    <Second_Hero_Draw_Anim_Type>HC_DRAW</Second_Hero_Draw_Anim_Type>
    <Second_Hero_Conversation_Anim_Type>IDLE</Second_Hero_Conversation_Anim_Type>
    <First_Hero_Win_Speech>Speech_A</First_Hero_Win_Speech>
    <Second_Hero_Win_Speech>Speech_B</Second_Hero_Win_Speech>
  </Hero_Clash>
  <Hero_Clash Name="Action_B_C">
    <Clash_Type>Action_Driven</Clash_Type>
    <Clash_Range>0.0</Clash_Range>
    <First_Hero_Type>Hero_B</First_Hero_Type>
    <Second_Hero_Type>Hero_C</Second_Hero_Type>
    <First_Hero_Win_Exchange_Chance>0.8</First_Hero_Win_Exchange_Chance>
    <Second_Hero_Win_Exchange_Chance>0.8</Second_Hero_Win_Exchange_Chance>
    <Clash_Actions><Attack_Action/><Move_Action/></Clash_Actions>
  </Hero_Clash>
  <Hero_Clash Name="Action_B_C">
    <Clash_Type>Something</Clash_Type>
  </Hero_Clash>
</Hero_Clashes>
)";

inline std::span<const uint8_t> clash_bytes(std::string_view x) { return {reinterpret_cast<const uint8_t*>(x.data()), x.size()}; }
HeroClashData clash_data();

// The host side for the manager tests: units with type / owner / position / health; Take_Damage subtracts at once.
struct ClashStub final : HeroClashHost {
    struct U {
        ObjectId id = kNoObject;
        int32_t type = -1, owner = -1;
        Vec3 pos;
        float yaw = 0.0f, health = 500.0f;
        bool alive = true, jet = false, walking = false, flag = false, held = false, hero = true;
        ObjectId target = kNoObject;
        uint32_t moves = 0, places = 0;
        Vec3 spot;
    };
    std::vector<U> units;
    std::vector<std::pair<int32_t, int32_t>> allies;  // owner pairs that are not enemies
    bool model = false;  // clip(): model data (variants / frames below) instead of the fallback
    uint32_t model_variants = 1, model_frames = 101;
    float model_fps = 30.0f;
    float last_damage = 0.0f;
    uint32_t damage_calls = 0;

    U* get(ObjectId id) {
        for (U& u : units)
            if (u.id == id) return &u;
        return nullptr;
    }
    U& add(ObjectId id, int32_t type, int32_t owner, Vec3 pos) {
        U u;
        u.id = id;
        u.type = type;
        u.owner = owner;
        u.pos = pos;
        units.push_back(u);
        return units.back();
    }
    void candidates(int32_t type, std::vector<ObjectId>& out) override {
        for (auto it = units.rbegin(); it != units.rend(); ++it)  // reverse: the manager sorts
            if (it->type == type && it->alive) out.push_back(it->id);
    }
    bool alive(ObjectId id) override { return get(id) && get(id)->alive; }
    int32_t type_of(ObjectId id) override { return get(id) ? get(id)->type : -1; }
    int32_t owner(ObjectId id) override { return get(id) ? get(id)->owner : -1; }
    bool hero_type(int32_t type) override {
        for (const U& u : units)
            if (u.type == type) return u.hero;
        return false;
    }
    bool enemies(int32_t a, int32_t b) override {
        for (const auto& [x, y] : allies)
            if ((x == a && y == b) || (x == b && y == a)) return false;
        return a != b;
    }
    Vec3 position(ObjectId id) override { return get(id)->pos; }
    float yaw_deg(ObjectId id) override { return get(id)->yaw; }
    float health(ObjectId id) override { return get(id)->health; }
    bool jetpacking(ObjectId id) override { return get(id)->jet; }
    ObjectId attack_target(ObjectId id) override { return get(id)->target; }
    bool walking(ObjectId id) override { return get(id)->walking; }
    bool in_clash(ObjectId id) override { return get(id)->flag; }
    void set_in_clash(ObjectId id, bool on) override { get(id)->flag = on; }
    void move_to(ObjectId id, Vec3 spot) override {
        ++get(id)->moves;
        get(id)->spot = spot;
    }
    void place(ObjectId id, Vec3 pos, float yaw) override {
        U* u = get(id);
        u->pos = pos;
        u->yaw = yaw;
        ++u->places;
    }
    void face(ObjectId id, float yaw) override { get(id)->yaw = yaw; }
    void hold(ObjectId id, bool on) override { get(id)->held = on; }
    bool clip(ObjectId, std::string_view, uint32_t, uint32_t& variants, uint32_t& frames, float& fps) override {
        if (!model) return false;
        variants = model_variants;
        frames = model_frames;
        fps = model_fps;
        return true;
    }
    bool speech_idle() override { return true; }
    void damage(ObjectId victim, float amount, ObjectId) override {
        U* u = get(victim);
        u->health -= amount;
        if (u->health <= 0.0f) u->alive = false;
        last_damage = amount;
        ++damage_calls;
    }
};

inline constexpr int32_t kTypeA = 10, kTypeB = 11, kTypeC = 12, kTypeX = 13;
void test_hero_clash_parse_and_rules();

// Two enemy heroes 50 apart on world X (within Clash_Range 100), entry 0 resolved to kTypeA / kTypeB.
struct ClashRig {
    HeroClashData data = clash_data();
    HeroClashManager m;
    ClashStub host;
    SimRng rng{99u};
    uint32_t frame = 15;
    ClashRig() {
        m.set_data(&data, {{kTypeA, kTypeB}, {kTypeB, kTypeC}, {-1, -1}});
        host.add(1, kTypeA, 0, Vec3{0.0f, 0.0f, 0.0f});
        host.add(2, kTypeB, 1, Vec3{50.0f, 10.0f, 0.0f});
    }
    void step() { m.update(frame++, host, rng); }
};
void test_hero_clash_start_rule();
void test_hero_clash_fight();
void test_hero_clash_end_and_save();
void test_hero_clash_pairing();
void test_hero_clash_fire_refused();
void test_hero_clash_foc_data();

}  // namespace
using namespace bo3split;

inline void test_energy_drain_engines() {
    // cu8f t02 (CU8F_T01_T02_T07_RE.md §2.4): after the shield steps, EnergyPool::Absorb takes min(E, x) from the pool of a
    // projectile with Projectile_Does_Energy_Damage; ANY positive absorption with Projectile_Disables_Engines_When_Power_
    // Drained calls CombatHooks::engines_drained(victim, Projectile_Disable_Engines_Duration). Shields absorb first.
    struct DrainHooks final : CombatHooks {
        std::vector<std::pair<ObjectId, float>> calls;
        void engines_drained(ObjectId id, float seconds, uint32_t) override { calls.emplace_back(id, seconds); }
    };
    World w;
    DrainHooks hk;
    ProjectileDesc ion;
    ion.name = "Proj_Ion";
    ion.max_speed = 10.0f;
    ion.does_energy_damage = true;
    ion.disables_engines_when_drained = true;
    ion.disable_engines_duration = 20.0f;
    ion.does_shield_damage = false;  // the shield does not absorb it
    w.data.projectiles.push_back(ion);
    ProjectileDesc plain = ion;  // energy damage without the engine flag
    plain.name = "Proj_Energy";
    plain.disables_engines_when_drained = false;
    w.data.projectiles.push_back(plain);
    ProjectileDesc shield = ion;  // also does shield damage: the shield absorbs first
    shield.name = "Proj_Ion_Shield";
    shield.does_shield_damage = true;
    w.data.projectiles.push_back(shield);
    UnitDesc d;
    d.tactical_health = 1000.0f;
    d.energy_capacity = 100.0f;
    d.shield_points = 50.0f;
    const uint32_t ud = push_unit(w.data, d);
    w.make();
    w.sys->set_hooks(&hk);
    w.add(1, ud, 1, alamo::tactical::Vec3{});
    w.sys->set_shield(1, 50.0f);
    auto hit = [&](int32_t proj, float amount) {
        DamageRequest r;
        r.victim = 1;
        r.amount = amount;
        r.kind = kKindNormal;
        r.damage_type = kNoTypeId;
        r.source_is_projectile = true;
        r.projectile_desc = proj;
        return w.sys->take_damage(r, w.frame);
    };
    DamageResult res = hit(1, 30.0f);  // energy, no engine flag
    CHECK_F(res.energy_absorbed, 30.0f);
    CHECK(hk.calls.empty());
    res = hit(2, 40.0f);  // the shield (50) takes all of it: nothing reaches the pool
    CHECK_F(res.absorbed, 40.0f);
    CHECK_F(res.energy_absorbed, 0.0f);
    CHECK(hk.calls.empty());
    res = hit(0, 10.0f);  // any positive absorption, the pool far from empty
    CHECK_F(res.energy_absorbed, 10.0f);
    CHECK(hk.calls.size() == 1 && hk.calls[0].first == 1 && hk.calls[0].second == 20.0f);
    res = hit(0, 500.0f);  // takes the last 60
    CHECK_F(res.energy_absorbed, 60.0f);
    CHECK(hk.calls.size() == 2);
    res = hit(0, 10.0f);  // empty pool: no absorption, no call
    CHECK_F(res.energy_absorbed, 0.0f);
    CHECK(hk.calls.size() == 2);
    w.sys->set_hooks(nullptr);
}

// Test functions (defined in the part files)

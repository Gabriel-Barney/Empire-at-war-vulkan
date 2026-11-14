#pragma once
// Split out of test_abilities.cpp (bo3 t03): shared includes, types and helpers; the tests live in test_abilities_partN.cpp.
// S9 tests: hardpoints & abilities (docs/re/ABILITIES_HARDPOINTS.md). Hand-computed checks per rule,
// a scripted space battle with abilities and hardpoint kills (CRC log identical for 1/4/N threads,
// golden file shared by MSVC / clang / gcc), a per-tick allocation count and FoC data checks.
//   alamo_tactical_abilities_tests [--golden <file>] [--write-golden <file>]
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <functional>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/test/thread_parallel_for.hpp"
#include "alamo/test/support.hpp"
#ifdef ALAMO_ABILITIES_TESTS_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

namespace bo3split {
inline std::atomic<uint64_t> g_allocs{0};
inline std::atomic<bool> g_count{false};
}  // namespace
using namespace bo3split;
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
using namespace alamo::tactical::abilities;

namespace bo3split {

using alamo::tactical::Vec3;
using alamo::tactical::combat::ObjectId;
using alamo::tactical::combat::kNoObject;

using ::alamo::test::g_fail;
#define CHECK_EQ(a, b)                                                                                              \
    do {                                                                                                            \
        const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);                                    \
        if (!(va_ == vb_)) {                                                                                        \
            std::printf("FAIL %s:%d: %s == %s (%.9g vs %.9g)\n", __FILE__, __LINE__, #a, #b, va_, vb_);             \
            ++g_fail;                                                                                               \
        }                                                                                                           \
    } while (0)

inline constexpr uint64_t kCatFighter = 1, kCatCorvette = 2, kCatFrigate = 4, kCatCapital = 8;
Aabb box_of(Vec3 p, float r);

// A sandbox: combat + ability data, both systems, trees and the synced RNG.
struct World {
    CombatData cd;
    AbilityData ad;
    std::unique_ptr<CombatSystem> sys;
    std::unique_ptr<AbilitySystem> ab;
    TreeSet trees;
    SimRng rng{4242u};
    uint32_t frame = 1;

    World() : trees(2) {}
    void make(CombatConfig cfg = {}, AbilityConfig acfg = {}) {
        acfg.corvette_category = kCatCorvette;
        ab.reset();  // the old AbilitySystem unhooks itself from the old CombatSystem in its destructor, so it goes first
        sys = std::make_unique<CombatSystem>(cd, cfg, std::vector<PlayerInfo>{PlayerInfo{}, PlayerInfo{}});
        ab = std::make_unique<AbilitySystem>(*sys, ad, acfg);
        sys->bind_rng(&rng);
    }
    // Adds a unit whose ability descriptor has the same index as its UnitDesc.
    void add(ObjectId id, uint32_t desc, int32_t owner, Vec3 pos, Vec3 facing = {1, 0, 0}, ObjectId container = kNoObject) {
        UnitSpawn s;
        s.id = id;
        s.desc = desc;
        s.owner = owner;
        s.position = pos;
        s.facing = facing;
        if (container != kNoObject) s.team = static_cast<uint32_t>(container);
        CHECK(ab->add_unit(s, desc, frame, rng, container));
        TreeMembership m;
        m.owner = owner;
        CHECK(trees.add(static_cast<EntityHandle>(id), box_of(pos, cd.units[desc].radius), 1u, m));
    }
    void move(ObjectId id, Vec3 p) {
        sys->set_position(id, p);
        trees.update(static_cast<EntityHandle>(id), box_of(p, cd.units[sys->desc_index(id)].radius));
    }
    void tick(const ParallelFor& pf = serial_for) {  // events accumulate until clear_events()
        ab->tick(frame, trees, rng, pf);
        for (const DeathEvent& d : sys->deaths()) trees.remove(static_cast<EntityHandle>(d.id));
        ++frame;
    }
    bool has_event(AbilityEventKind k, ObjectId id = kNoObject) const {
        for (const AbilityEvent& e : ab->events())
            if (e.kind == k && (id == kNoObject || e.id == id)) return true;
        return false;
    }
};
uint32_t push_container(CombatData& cd, AbilityData& ad, AbilityObjectDesc a);
uint32_t push(CombatData& cd, AbilityData& ad, const UnitDesc& u, AbilityObjectDesc a);
UnitAbilityRecord rec(uint8_t type, float exp, float rech);
uint32_t add_special(AbilityData& ad, SpecialAbilityDesc s);
void test_info_table();
void test_countdown();
void test_parse();

// ================================================================================================
// unit-ability core: commands, countdown, Mod_Multiplier, gates, squads
// ================================================================================================

struct ModeWorld : World {
    uint32_t ship = 0, fighter = 0, squad = 0;
    ModeWorld() {
        UnitDesc s;
        s.name = "Ship";
        s.tactical_health = 1000.0f;
        s.shield_points = 500.0f;
        s.shield_refresh_rate = 10.0f;
        s.radius = 50.0f;
        s.category_mask = kCatCapital;
        AbilityObjectDesc a;
        UnitAbilityRecord defend = rec(kDefend, 15.0f, 20.0f);
        defend.combat.mult[static_cast<size_t>(ModKind::ShieldRegenInterval) - 1] = 0.1f;
        defend.combat.mult[static_cast<size_t>(ModKind::Speed) - 1] = 0.8f;
        UnitAbilityRecord ptw = rec(kPowerToWeapons, 6.0f, 50.0f);
        ptw.combat.mult[static_cast<size_t>(ModKind::WeaponDelay) - 1] = 0.5f;
        a.unit_abilities = {defend, ptw, rec(kSpoilerLock, 0.0f, 0.0f), rec(kSprint, 2.0f, 4.0f)};
        ship = push(cd, ad, s, a);
        UnitDesc f;
        f.name = "Fighter";
        f.tactical_health = 50.0f;
        f.radius = 5.0f;
        f.category_mask = kCatFighter;
        AbilityObjectDesc fa;
        fa.unit_abilities = {rec(kSprint, 2.0f, 4.0f)};
        fighter = push(cd, ad, f, fa);
        AbilityObjectDesc sq;  // squadron container: SPRINT forwarded to members
        sq.name = "Squad";
        sq.team_container = true;
        sq.unit_abilities = {rec(kSprint, 2.0f, 4.0f)};
        squad = push_container(cd, ad, sq);
        make();
        add(1, ship, 0, {0, 0, 0});
        add(2, fighter, 0, {100, 0, 0});
        add(3, fighter, 0, {110, 0, 0});
        add(4, fighter, 1, {5000, 0, 0});
        const ObjectId members[] = {2, 3};
        CHECK(ab->add_container(5, squad, 0, members, frame));
        tick();  // first update: countdown deltas are 1 from here on
    }
};
void test_modes();
void test_gates();
void test_squad();
uint64_t ability_hash(const AbilitySystem& a);
void test_black_market_gate();

// ================================================================================================
// hardpoints (1)
// ================================================================================================

struct HpWorld : World {
    uint32_t cap = 0, small = 0;
    HpWorld(bool all_hp_rule = true) {
        ProjectileDesc boom;
        boom.name = "Death_Boom";
        boom.max_speed = 1.0f;
        boom.max_lifetime = 0.1f;
        boom.blast_area_damage = 10.0f;
        boom.blast_area_range = 50.0f;
        cd.projectiles = {boom, boom, boom};
        HardpointDesc w1;
        w1.name = "HP_W1";
        w1.kind = HardpointKind::WeaponLaser;
        w1.health = 100.0f;
        w1.destroyable = w1.targetable = true;
        w1.max_recharge = 2.0f;
        w1.min_recharge = 1.0f;
        w1.projectile = 0;
        w1.attach_offset = {30, 0, 0};
        w1.death_explosion_type = 77;
        HardpointDesc sh = w1;
        sh.name = "HP_Shield";
        sh.kind = HardpointKind::ShieldGenerator;
        sh.max_recharge = 0.0f;
        sh.min_recharge = 0.0f;
        sh.health = 200.0f;
        sh.attach_offset = {-30, 0, 0};
        HardpointDesc w2 = w1;
        w2.name = "HP_W2";
        w2.max_recharge = 1.0f;
        w2.attach_offset = {0, 30, 0};
        HardpointDesc art = w1;
        art.name = "HP_Art";
        art.kind = HardpointKind::DummyArt;
        art.destroyable = false;
        art.max_recharge = 0.0f;
        cd.hardpoints = {w1, sh, w2, art};
        UnitDesc c;
        c.name = "Cap";
        c.tactical_health = 1000.0f;
        c.shield_points = 100.0f;
        c.radius = 50.0f;
        c.category_mask = kCatCapital;
        c.hardpoints = {0, 1, 2, 3};
        c.destroy_when_all_hardpoints_destroyed = all_hp_rule;
        c.damaged_smoke = true;
        c.death_clones = {{1, 55}, {7, 56}};
        c.death_projectiles = {0, 1, 2};
        c.ranged_target_z_adjust = 20.0f;
        cap = push(cd, ad, c, AbilityObjectDesc{});
        UnitDesc s;
        s.name = "Small";
        s.tactical_health = 60.0f;
        s.radius = 5.0f;
        s.category_mask = kCatFighter;
        s.combustible = true;
        s.burning_damage_per_second = 4.0f;
        s.converted_die_time = 10.0f;
        small = push(cd, ad, s, AbilityObjectDesc{});
        CombatConfig cfg;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        make(cfg);
    }
};
void test_hardpoints();
void test_last_hardpoint_kills();
void test_death_clone_remap();
void test_fire_and_decay();
void test_energy();
void test_ion_stun();

struct ShieldWorld : World {
    // S9 t05: `turn_rate` = the projectile's Max_Rate_Of_Turn, `turn_mult` = Object_Max_Speed_Multiplier_Space.
    ShieldWorld(ProjectileCategory cat, bool passive_only, float turn_rate, float turn_mult = 1.0f) {
        ProjectileDesc p;
        p.name = "P";
        p.max_speed = 20.0f;
        p.damage = 5.0f;
        p.category = cat;
        p.max_flight_distance = 3000.0f;
        p.max_rate_of_turn = turn_rate;
        if (cat == ProjectileCategory::Rocket) {  // refixf t01: spline flight (S9_DEATH_CLONE_SHIELDS.md §2.4)
            p.rocket_curve_distance = 50.0f;
            p.rocket_curve_offset = 10.0f;
        }
        cd.projectiles = {p};
        UnitDesc sh;
        sh.name = "Shooter";
        sh.tactical_health = 1e6f;
        sh.radius = 5.0f;
        sh.max_attack_distance = 2000.0f;
        sh.weapon_projectile = 0;
        sh.weapon_recharge = 0.5f;
        const uint32_t s = push(cd, ad, sh, AbilityObjectDesc{});
        UnitDesc em;
        em.name = "Emitter";
        em.tactical_health = 1e6f;
        em.radius = 20.0f;
        if (passive_only) em.passive_missile_shield_radius = 300.0f;
        AbilityObjectDesc ea;
        UnitAbilityRecord ms = rec(kMissileShield, 0.0f, 0.0f);
        ms.effective_radius = 300.0f;
        ms.disable_movement = true;
        ms.active_by_default = !passive_only;
        ea.unit_abilities = {ms};
        const uint32_t e = push(cd, ad, em, ea);
        CombatConfig cfg;
        cfg.inaccuracy_enabled = false;
        cfg.turn_multiplier_space = turn_mult;
        make(cfg);
        add(1, s, 0, {-1000, 0, 0});
        add(2, e, 1, {0, 0, 0});
        sys->order_attack(1, 2);
    }
    // Runs `ticks` frames. seen_inside: a projectile came within 290 of the emitter; max_turn: the largest heading
    // change of one projectile between two consecutive ticks (degrees); changed: some heading changed at all (bits).
    bool seen_inside = false, changed = false;
    float max_turn = 0.0f;
    float closest = 1e9f;  // refixf t01: smallest distance of a projectile from the emitter centre
    void run(int ticks) {
        std::map<uint32_t, Vec3> prev;
        for (int i = 0; i < ticks; ++i) {
            tick();
            std::map<uint32_t, Vec3> cur;
            for (size_t k = 0; k < sys->projectile_count(); ++k) {
                const ProjectileView v = sys->projectile(k);
                if (!v.alive) continue;
                if (dot(v.pos, v.pos) < 290.0f * 290.0f) seen_inside = true;
                closest = std::min(closest, dmath::sqrt(dot(v.pos, v.pos)));
                cur[v.serial] = v.dir;
                const auto it = prev.find(v.serial);
                if (it == prev.end()) continue;
                const Vec3 a = it->second, b = v.dir;
                if (a.x != b.x || a.y != b.y || a.z != b.z) changed = true;
                float c = dot(a, b) / dmath::sqrt(dot(a, a) * dot(b, b));
                c = c > 1.0f ? 1.0f : (c < -1.0f ? -1.0f : c);
                const float deg = dmath::acos(c) * 180.0f / dmath::kPiF;
                if (deg > max_turn) max_turn = deg;
            }
            prev = std::move(cur);
        }
    }
};
void test_missile_shield();
void test_opportunity_fire();

// ================================================================================================
// special abilities: tractor, ion shot, lucky shot, concentrate fire, energy weapon, stealth,
// jamming, laser defense, listeners
// ================================================================================================

struct SpecialWorld : World {
    uint32_t sd = 0, corvette = 0, frigate = 0, ywing = 0, ysq = 0, xwing = 0, jammer = 0, ld = 0, rb = 0;
    uint32_t tractor_sp = 0, ion_sp = 0, lucky_sp = 0, cf_sp = 0, ew_sp = 0, st_sp = 0, jam_sp = 0, ld_sp = 0, rb_sp = 0;
    SpecialWorld() {
        ProjectileDesc laser;
        laser.name = "Laser";
        laser.max_speed = 60.0f;
        laser.damage = 2.0f;
        laser.category = ProjectileCategory::Laser;
        ProjectileDesc ion = laser;
        ion.name = "Proj_Ion";
        ion.damage = 3.0f;
        ion.does_hitpoint_damage = false;
        ion.ion_stun = true;
        ion.ion_stun_duration = 1.0f;
        ion.ion_stun_speed_reduction = 0.5f;
        ion.ion_stun_shot_rate_reduction = 0.5f;
        ProjectileDesc lucky = laser;
        lucky.name = "Proj_Lucky";
        ProjectileDesc missile = laser;
        missile.name = "Missile";
        missile.category = ProjectileCategory::Missile;
        missile.max_speed = 5.0f;
        missile.max_flight_distance = 5000.0f;
        cd.projectiles = {laser, ion, lucky, missile};
        HardpointDesc tb;
        tb.name = "HP_Tractor";
        tb.kind = HardpointKind::TractorBeam;
        tb.health = 100.0f;
        tb.destroyable = tb.targetable = true;
        HardpointDesc gen = tb;
        gen.name = "HP_Gen";
        gen.kind = HardpointKind::ShieldGenerator;
        gen.attach_offset = {40, 0, 0};
        cd.hardpoints = {tb, gen};

        SpecialAbilityDesc s;
        s.style = ActivationStyle::UserInput;
        s.applicable_categories = ~uint64_t{0};
        s.name = "Tractor";
        s.cls = SpecialClass::TractorBeam;
        s.applicable_categories = kCatCorvette | kCatFrigate;
        s.max_range = 1000.0f;
        s.target_speed_decrease = 0.75f;
        tractor_sp = add_special(ad, s);
        s = SpecialAbilityDesc{};
        s.style = ActivationStyle::UserInput;
        s.applicable_categories = ~uint64_t{0};
        s.name = "Ion";
        s.cls = SpecialClass::IonCannonShot;
        ion_sp = add_special(ad, s);
        s.name = "Lucky";
        s.cls = SpecialClass::LuckyShot;
        lucky_sp = add_special(ad, s);
        s.name = "Concentrate";
        s.cls = SpecialClass::ConcentrateFire;
        s.target_speed_decrease = 0.1f;
        cf_sp = add_special(ad, s);
        s = SpecialAbilityDesc{};
        s.style = ActivationStyle::UserInput;
        s.applicable_categories = ~uint64_t{0};
        s.name = "EnergyWeapon";
        s.cls = SpecialClass::EnergyWeapon;
        s.max_range = 800.0f;
        s.damage_per_frame = 50.0f;
        ew_sp = add_special(ad, s);
        s = SpecialAbilityDesc{};
        s.style = ActivationStyle::UserInput;
        s.name = "Stealth";
        s.cls = SpecialClass::Stealth;
        st_sp = add_special(ad, s);
        s = SpecialAbilityDesc{};
        s.style = ActivationStyle::CombatAutomatic;
        s.name = "Jam";
        s.cls = SpecialClass::SensorJamming;
        jam_sp = add_special(ad, s);
        s = SpecialAbilityDesc{};
        s.style = ActivationStyle::UserInput;
        s.name = "LaserDef";
        s.cls = SpecialClass::LaserDefense;
        s.defense_duration = 10.0f;
        s.protection_radius = 400.0f;
        s.zap_recharge = 0.25f;
        s.dist_travelled = 40.0f;
        s.targeted_categories = 1u << 1;
        ld_sp = add_special(ad, s);
        s = SpecialAbilityDesc{};
        s.style = ActivationStyle::TakeDamage;
        s.name = "Redirect";
        s.cls = SpecialClass::RedirectBlaster;
        s.applicable_categories = ~uint64_t{0};
        s.redirect_chance = 0.0f;
        s.block_chance = 1.0f;
        s.reaction_arc_deg = 360.0f;
        rb_sp = add_special(ad, s);

        auto gui = [](uint8_t t, float exp, float rch, const char* name) {
            UnitAbilityRecord r = rec(t, exp, rch);
            r.gui_activated_ability_name = name;
            r.gui_name_crc = name_crc(name);
            return r;
        };
        UnitDesc u;
        u.name = "SD";
        u.tactical_health = 5000.0f;
        u.radius = 60.0f;
        u.category_mask = kCatCapital;
        u.hardpoints = {0};
        u.max_attack_distance = 1500.0f;
        AbilityObjectDesc a;
        UnitAbilityRecord tr = gui(kTractorBeam, 0.0f, 5.0f, "Tractor");
        tr.combat.mult[static_cast<size_t>(ModKind::Speed) - 1] = 0.6f;
        a.unit_abilities = {tr, gui(kEnergyWeapon, 2.0f, 10.0f, "EnergyWeapon"), gui(kConcentrateFire, 3.0f, 30.0f, "Concentrate")};
        a.unit_abilities.back().effective_radius = 3000.0f;
        a.specials = {tractor_sp, ew_sp, cf_sp};
        sd = push(cd, ad, u, a);
        UnitDesc cv;
        cv.name = "Corvette";
        cv.tactical_health = 400.0f;
        cv.radius = 20.0f;
        cv.category_mask = kCatCorvette;
        cv.hardpoints = {1};
        AbilityObjectDesc ca;
        ca.unit_abilities = {gui(kStealth, 5.0f, 20.0f, "Stealth"), gui(kLaserDefense, 0.0f, 30.0f, "LaserDef")};
        ca.specials = {st_sp, ld_sp, rb_sp};
        corvette = push(cd, ad, cv, ca);
        UnitDesc fr = cv;
        fr.name = "Frigate";
        fr.category_mask = kCatFrigate;
        fr.hardpoints = {1, 1};
        AbilityObjectDesc fa;
        fa.stealth_detection_range = 600.0f;
        frigate = push(cd, ad, fr, fa);
        UnitDesc yw;
        yw.name = "YWing";
        yw.tactical_health = 50.0f;
        yw.radius = 5.0f;
        yw.category_mask = kCatFighter;
        yw.max_attack_distance = 600.0f;
        yw.weapon_projectile = 0;
        yw.weapon_recharge = 0.5f;
        yw.weapon_pulse_count = 2;
        yw.weapon_pulse_delay = 0.1f;
        AbilityObjectDesc ya;
        UnitAbilityRecord ir = gui(kIonCannonShot, 0.0f, 20.0f, "Ion");
        ir.projectile_override = {1};
        ya.unit_abilities = {ir};
        ywing = push(cd, ad, yw, ya);
        AbilityObjectDesc ys = ya;
        ys.name = "YSquad";
        ys.team_container = true;
        ys.specials = {ion_sp};
        ysq = push_container(cd, ad, ys);
        UnitDesc xw = yw;
        xw.name = "XWing";
        AbilityObjectDesc xa;
        UnitAbilityRecord lr = gui(kLuckyShot, 0.0f, 60.0f, "Lucky");
        lr.projectile_override = {2};
        lr.combat.mult[static_cast<size_t>(ModKind::CauseDamage) - 1] = 600.0f;
        xa.unit_abilities = {lr};
        xa.specials = {lucky_sp};
        xwing = push(cd, ad, xw, xa);
        UnitDesc jm;
        jm.name = "Jammer";
        jm.tactical_health = 500.0f;
        jm.radius = 20.0f;
        jm.passive_missile_shield_radius = 0.0f;
        AbilityObjectDesc ja;
        UnitAbilityRecord jr = rec(kSensorJamming, 0.0f, 0.0f);
        jr.effective_radius = 1000.0f;
        ja.unit_abilities = {jr};
        ja.specials = {jam_sp};
        jammer = push(cd, ad, jm, ja);
        UnitDesc mf = yw;
        mf.name = "MissileBoat";
        mf.weapon_projectile = 3;
        mf.max_attack_distance = 3000.0f;
        mf.weapon_recharge = 1.0f;
        mf.weapon_pulse_count = 1;
        ld = push(cd, ad, mf, AbilityObjectDesc{});
        CombatConfig cfg;
        cfg.inaccuracy_enabled = false;
        cfg.opportunity_fire = false;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        make(cfg);
    }
};
void test_tractor();
void test_ion_and_lucky();
void test_concentrate_energy();
void test_fog_abilities();
void test_jamming_visual_fields();
void test_stealth_jamming_laser();
void test_black_market_specials();

// ================================================================================================
// OI abilities (docs/re/TACTICAL_GAPS.md §1, §2, §7): hand-computed rules and draws
// ================================================================================================

// A sandbox with configurable players / mode, a host map rectangle and an event log.
struct OiWorld {
    CombatData cd;
    AbilityData ad;
    std::unique_ptr<CombatSystem> sys;
    std::unique_ptr<AbilitySystem> ab;
    TreeSet trees;
    SimRng rng{777u};
    uint32_t frame = 1;
    std::vector<AbilityEvent> log;  // every event of every tick (and of host calls between ticks)

    OiWorld() : trees(3) {}
    void make(GameMode mode = GameMode::Space, std::vector<PlayerInfo> players = {PlayerInfo{}, PlayerInfo{}, PlayerInfo{}},
              CombatConfig cfg = {}) {
        cfg.mode = mode;
        cfg.inaccuracy_enabled = false;
        cfg.opportunity_fire = false;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        AbilityConfig acfg;
        acfg.mode = mode;
        acfg.corvette_category = kCatCorvette;
        ab.reset();
        sys = std::make_unique<CombatSystem>(cd, cfg, std::move(players));
        ab = std::make_unique<AbilitySystem>(*sys, ad, acfg);
        sys->bind_rng(&rng);
        ab->bind_rng(&rng);
        AbilityHost h;
        h.map_rect = [](Vec3& lo, Vec3& hi) {
            lo = Vec3{-1000.0f, -1000.0f, 0.0f};
            hi = Vec3{1000.0f, 1000.0f, 0.0f};
            return true;
        };
        ab->set_host(std::move(h));
    }
    void add(ObjectId id, uint32_t desc, int32_t owner, Vec3 pos, Vec3 facing = {1, 0, 0}, ObjectId container = kNoObject,
             uint32_t flags = 0) {
        UnitSpawn s;
        s.id = id;
        s.desc = desc;
        s.owner = owner;
        s.position = pos;
        s.facing = facing;
        s.flags = flags;
        if (container != kNoObject) s.team = static_cast<uint32_t>(container);
        CHECK(ab->add_unit(s, desc, frame, rng, container));
        TreeMembership m;
        m.owner = owner;
        CHECK(trees.add(static_cast<EntityHandle>(id), box_of(pos, cd.units[desc].radius), 1u, m));
    }
    void move(ObjectId id, Vec3 p) {
        sys->set_position(id, p);
        trees.update(static_cast<EntityHandle>(id), box_of(p, cd.units[sys->desc_index(id)].radius));
    }
    void collect() {
        for (const AbilityEvent& e : ab->events()) log.push_back(e);
        ab->clear_events();
    }
    void tick() {
        collect();
        ab->tick(frame, trees, rng, serial_for);
        for (const DeathEvent& d : sys->deaths()) trees.remove(static_cast<EntityHandle>(d.id));
        collect();
        ++frame;
    }
    size_t count(AbilityEventKind k, ObjectId id = kNoObject) const {
        size_t n = 0;
        for (const AbilityEvent& e : log)
            if (e.kind == k && (id == kNoObject || e.id == id)) ++n;
        return n;
    }
    const AbilityEvent* last(AbilityEventKind k, ObjectId id = kNoObject) const {
        for (size_t i = log.size(); i-- > 0;)
            if (log[i].kind == k && (id == kNoObject || log[i].id == id)) return &log[i];
        return nullptr;
    }
};
UnitDesc oi_unit(const char* name, float health, float radius = 5.0f, uint64_t cats = kCatFighter);
SpecialAbilityDesc oi_special(const char* name, SpecialClass c, ActivationStyle st);
UnitAbilityRecord oi_gui(uint8_t t, float exp, float rch, const char* name);
void test_oi_tables();
void test_oi_sap_init_and_defaults();
void test_oi_autofire();
void test_oi_hunt();
void test_oi_waves_and_confuse();
void test_oi_melee_and_chance();
void test_oi_mines();
void test_oi_death_draws();
void test_oi_auras();
void test_s78m_aura_accept();
void test_oi_repair_priorities();
void test_oi_detonations();
void test_oi_shields_jamming_ion();
void test_oi_class_bodies();
uint32_t special_hardpoint(CombatData& cd, int32_t blast_projectile);
void test_gaps_eject_thief();
void test_gaps_deploy_troopers();
void test_gaps_jet_pack();
void test_gaps_stun_release();
void test_gaps_blast();
void test_s78n_t12_thief_apply();
void test_s78n_review_stim_pack();
void blast_battle(bool target_dies, int wake);
void test_s78n_review_blast_battle();
void test_gaps_leech();
void test_oi_weapon_swap();
void test_oi_berserker_speed();
void test_oi_stun_over_water();

// OI t03: ticks from the order to the Generic_Attack hit and to its removal (Damage_Frame_Number 10); count > 0: the
// host reports a POUND_ATTACK clip of `count` frames at `fps` for the attacker.
inline std::array<int, 2> oi_generic_timing(uint32_t count, float fps, float speedup) {
    OiWorld w;
    SpecialAbilityDesc g = oi_special("Pound", SpecialClass::GenericAttack, ActivationStyle::UserInput);
    g.max_range = 100.0f;
    g.damage_amount = 30.0f;
    g.damage_frame = 10;
    g.anim_speedup = speedup;
    g.attack_anim = "POUND_ATTACK";
    const uint32_t gs = add_special(w.ad, g);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kTargetedHack, 0.0f, 0.0f, "Pound")};
    a.specials = {gs};
    const uint32_t ud = push(w.cd, w.ad, oi_unit("Rancor", 1000.0f), a);
    const uint32_t td = push(w.cd, w.ad, oi_unit("Victim", 100.0f), AbilityObjectDesc{});
    w.make(GameMode::Land);
    if (count > 0) {
        AbilityHost h;
        h.anim_clip = [count, fps](ObjectId id, int32_t type, std::string_view name, uint32_t, uint32_t& n, float& f) {
            if (id != 1 || type >= 0 || name != "POUND_ATTACK") return false;
            n = count;
            f = fps;
            return true;
        };
        w.ab->set_host(std::move(h));
    }
    w.add(1, ud, 0, {0, 0, 0});
    w.add(2, td, 1, {50, 0, 0});
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kTargetedHack, 2, -1, w.frame) == 1);
    std::array<int, 2> out{-1, -1};
    for (int i = 1; i <= 100 && out[1] < 0; ++i) {
        w.tick();
        if (out[0] < 0 && w.sys->health(2) < 100.0f) out[0] = i;
        if (!w.ab->special_applied(1, name_crc("Pound"))) out[1] = i;
    }
    CHECK(w.sys->health(2) == 70.0f);  // one hit
    return out;
}
int oi_hack_timing(uint32_t count, float fps);
float oi_berserker_damage(uint32_t count);
void test_oi_anim_timings();

// ================================================================================================
// scripted space battle (golden)
// ================================================================================================

struct Battle {
    SpecialWorld w;
    CrcRegistry registry;
    struct Mover {
        ObjectId id;
        Vec3 start, dir;
        float speed;
        uint32_t frames;
    };
    std::vector<Mover> movers;
    uint64_t hp_kills = 0, ability_events = 0, activations = 0;

    Battle() {
        w.cd.units[w.sd].hardpoints = {0, 1, 1};
        w.cd.units[w.sd].weapon_projectile = 0;
        w.cd.units[w.sd].weapon_recharge = 0.4f;
        w.cd.units[w.sd].weapon_pulse_count = 3;
        w.cd.units[w.sd].weapon_pulse_delay = 0.1f;
        w.cd.units[w.sd].damaged_smoke = true;
        w.cd.units[w.sd].death_projectiles = {3, 0};
        w.cd.units[w.frigate].weapon_projectile = 0;
        w.cd.units[w.frigate].weapon_recharge = 0.6f;
        w.cd.units[w.frigate].max_attack_distance = 1200.0f;
        w.cd.units[w.corvette].weapon_projectile = 0;
        w.cd.units[w.corvette].weapon_recharge = 0.5f;
        w.cd.units[w.corvette].max_attack_distance = 900.0f;
        w.cd.units[w.ld].weapon_recharge = 1.5f;
        CombatConfig cfg;
        cfg.grain = 2;
        cfg.opportunity_fire = true;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        w.make(cfg);
        w.sys->reserve(64, 4096, 64, 1024);
        w.ab->reserve(64, 1024);
        w.cd.projectiles[0].damage = 5.0f;
        w.cd.units[w.ywing].max_attack_distance = 900.0f;
        w.cd.units[w.xwing].max_attack_distance = 900.0f;
        w.ad.specials[w.rb_sp].block_chance = 0.3f;
        w.ad.specials[w.ew_sp].max_range = 1200.0f;
        // player 0: SD, Y-wing squadron (3), stealth corvette, missile boat
        spawn(1, w.sd, 0, {-600, 0, 0}, {1, 0, 0}, 0.3f, 300);
        spawn(2, w.ywing, 0, {-500, 200, 0}, {1, 0, 0}, 2.0f, 150, 6);
        spawn(3, w.ywing, 0, {-500, 220, 10}, {1, 0, 0}, 2.0f, 150, 6);
        spawn(4, w.ywing, 0, {-500, 240, 0}, {1, 0, 0}, 2.0f, 150, 6);
        spawn(5, w.corvette, 0, {-500, -300, 0}, {1, 0, 0}, 1.0f, 200);
        const ObjectId ym[] = {2, 3, 4};
        CHECK(w.ab->add_container(6, w.ysq, 0, ym, w.frame));
        // player 1: SD, corvette, two frigates, X-wing (lucky), jammer; then player 0's missile boat
        spawn(7, w.sd, 1, {600, 0, 0}, {-1, 0, 0}, 0.3f, 300);
        spawn(8, w.corvette, 1, {500, 300, 0}, {-1, 0, 0}, 1.0f, 200);
        spawn(9, w.frigate, 1, {550, -250, 0}, {-1, 0, 0}, 0.5f, 300);
        spawn(10, w.frigate, 1, {650, 150, 0}, {-1, 0, 0}, 0.5f, 300);
        spawn(11, w.xwing, 1, {400, -100, 0}, {-1, 0, 0}, 2.0f, 150);
        spawn(12, w.jammer, 1, {900, -600, 0}, {-1, 0, 0}, 0.0f, 0);
        spawn(13, w.ld, 0, {-800, -500, 0}, {1, 0, 0}, 0.3f, 300);        w.ab->register_crc(registry, "abilities", "combat");
        w.sys->register_crc(registry, "combat");
        registry.add("rng", [this](StateHasher& h) { w.rng.hash(h); });
    }
    void spawn(ObjectId id, uint32_t desc, int32_t owner, Vec3 p, Vec3 dir, float speed, uint32_t frames,
               ObjectId container = kNoObject) {
        w.add(id, desc, owner, p, dir, container);
        movers.push_back(Mover{id, p, dir, speed, frames});
    }
    void script(uint32_t f) {
        const ObjectId sd0[] = {1}, sd1[] = {7}, ysq[] = {6}, cv0[] = {5}, cv1[] = {8}, xw[] = {11};
        if (f == 10) w.ab->set_autofire(ysq, kIonCannonShot, true);  // command 58 (state only)
        if (f == 20) w.ab->targeted_ability(sd0, kTractorBeam, 8, -1, f);
        if (f == 30) w.ab->targeted_ability(ysq, kIonCannonShot, 7, 0, f);
        if (f == 40) w.ab->set_unit_ability_mode(cv0, kStealth, true, f);
        if (f == 60) w.ab->targeted_ability(xw, kLuckyShot, 1, 1, f);
        if (f == 90) w.ab->targeted_ability(sd1, kConcentrateFire, 1, -1, f);
        if (f == 120) w.ab->set_unit_ability_mode(cv1, kLaserDefense, true, f);
        if (f == 150) w.ab->targeted_ability(sd0, kEnergyWeapon, 9, -1, f);
        if (f == 200) w.sys->order_attack(1, 7, 2);  // hardpoint-targeted attack order (command 40/45 index)
        if (f == 260) w.ab->targeted_ability(sd1, kTractorBeam, 5, -1, f);
        if (f == 400) w.ab->targeted_ability(ysq, kIonCannonShot, 7, -1, f);
        if (f == 450) w.ab->set_unit_ability_mode(cv1, kStealth, true, f);
        for (const Mover& m : movers) {
            if (!w.sys->alive(m.id)) continue;
            const float t = static_cast<float>(f < m.frames ? f : m.frames);
            w.move(m.id, m.start + m.dir * (m.speed * t));
        }
        w.trees.maintain(static_cast<double>(f) / 30.0);
    }
    CrcLog run(uint32_t ticks, const ParallelFor& pf, bool capture = true) {
        CrcLog log;
        if (capture) {
            log.systems = registry.names();
            log.comments.push_back("S9 scripted space battle with abilities and hardpoint kills, seed 4242");
        }
        for (uint32_t i = 0; i < ticks; ++i) {
            const uint32_t f = w.frame;
            script(f);
            w.tick(pf);
            hp_kills += w.sys->hardpoint_events().size();
            ability_events += w.ab->events().size();
            for (const AbilityEvent& e : w.ab->events()) {
                if (e.kind == AbilityEventKind::Activated) ++activations;
                if (std::getenv("ALAMO_S9_DEBUG"))
                    std::printf("  f%u ev %d type %u id %d target %d\n", f, static_cast<int>(e.kind), e.type, e.id, e.target);
            }
            if (std::getenv("ALAMO_S9_DEBUG"))
                for (const HardpointEvent& e : w.sys->hardpoint_events()) std::printf("  f%u hp kill %d #%u\n", f, e.owner, e.index);
            w.ab->clear_events();
            if (capture) log.records.push_back(registry.capture(f));
        }
        return log;
    }
};
void test_battle(const char* golden, const char* write_golden);
void test_allocations();

// ================================================================================================
// N1 stage 2: save / load in the middle of a battle (AbilitySystem + CombatSystem write / read)
// ================================================================================================

struct SavedBattle {
    std::vector<uint8_t> combat, abilities, rng;
};
SavedBattle save_battle(const CombatSystem& c, const AbilitySystem& a, const SimRng& rng);
bool load_battle(CombatSystem& c, AbilitySystem& a, SimRng& rng, const SavedBattle& s);
uint64_t battle_digest(const CombatSystem& c, const AbilitySystem& a, const SimRng& rng);

inline std::vector<uint8_t> ability_bytes(const AbilitySystem& a) {
    ByteWriter w;
    a.write(w);
    return w.bytes();
}
void test_save_load_battle();
void oi_gate_world(OiWorld& w, bool clips, const bool* script_lock);
void test_oi_anim_gates_and_locks();

// Land mines (spawn requests, mines, pending events) saved between the apply tick and the host's spawn step.
struct MineScene {
    OiWorld w;
    MineScene() {
        SpecialAbilityDesc m = oi_special("Mines", SpecialClass::ProximityMines, ActivationStyle::UserInput);
        m.max_range = 200.0f;
        m.spawn_count = 3;
        m.spawn_type = 9;
        m.trigger_radius = 3.0f;
        m.activation_time = 0.5f;
        const uint32_t ms = add_special(w.ad, m);
        AbilityObjectDesc a;
        UnitAbilityRecord r = oi_gui(kProximityMines, 0.0f, 30.0f, "Mines");
        r.area_effect_decal_distance = 20.0f;
        a.unit_abilities = {r};
        a.specials = {ms};
        const uint32_t ud = push(w.cd, w.ad, oi_unit("Sapper", 100.0f), a);
        const uint32_t dd = push(w.cd, w.ad, oi_unit("Dummy", 1.0f), AbilityObjectDesc{});
        const uint32_t td = push(w.cd, w.ad, oi_unit("Tank", 100.0f), AbilityObjectDesc{});
        w.make(GameMode::Land);
        w.add(1, ud, 0, {0, 0, 0});
        w.add(2, dd, 1, {100, 0, 0}, {1, 0, 0}, kNoObject, kUnitSkipTargeting | kUnitInvulnerable);
        w.add(3, td, 1, {800, 0, 0});
    }
};
void test_save_load_mines();
void walk(const data::XmlNode& n, const std::function<void(const data::XmlNode&)>& fn);
void test_foc_data();
void test_hero_clash_accepts();

}  // namespace
using namespace bo3split;

// HARDPOINT_DISABLED.md §9: Set_System_Online offline (disabled vs destroyed) and online (repair to max) per type.
inline void test_hardpoint_online_effects() {
    OiWorld w;
    HardpointDesc hp;
    hp.health = 100.0f;
    hp.destroyable = true;
    hp.targetable = true;
    hp.repair_amount_per_frame = 50.0f;
    HardpointDesc eng = hp, gen = hp, gw = hp, esa = hp;
    eng.kind = HardpointKind::Engine;
    gen.kind = HardpointKind::ShieldGenerator;
    gw.kind = HardpointKind::GravityWell;
    esa.kind = HardpointKind::EnableSpecialAbility;
    esa.special_ability_name_crc = name_crc("Hp_Special");
    w.cd.hardpoints = {eng, gen, gw, esa};
    UnitDesc st = oi_unit("Interdictor", 1000.0f, 50.0f, kCatCapital);
    st.shield_points = 200.0f;
    st.shield_refresh_rate = 20.0f;
    st.hardpoints = {0, 1, 2, 3};
    AbilityObjectDesc a;
    a.unit_abilities = {rec(kInterdict, 10.0f, 10.0f), rec(kDefend, 10.0f, 10.0f)};
    a.specials = {add_special(w.ad, oi_special("Hp_Special", SpecialClass::Stealth, ActivationStyle::UserInput))};
    const uint32_t sd = push(w.cd, w.ad, st, a);
    w.make();
    w.add(1, sd, 0, {0, 0, 0});
    w.add(2, sd, 0, {500, 0, 0});    // same player and type: the type-11 toggle reaches it
    w.add(3, sd, 1, {-500, 0, 0});   // other player: untouched
    const uint32_t crc = name_crc("Hp_Special");
    CHECK(w.ab->special_enabled(1, crc) && w.ab->special_enabled(2, crc) && w.ab->special_enabled(3, crc));
    CHECK(!w.ab->special_enabled(1, 12345u) && !w.ab->special_enabled(99, crc));
    CHECK(w.ab->can_activate(1, kInterdict, true, true, w.frame) && w.ab->can_activate(1, kDefend, true, true, w.frame));
    // Offline by disabling (station-upgrade copy path): destroyed = 0.
    for (uint32_t i = 0; i < 4; ++i) w.sys->set_hardpoint_disabled(1, i, true);
    w.collect();
    const AbilityEvent* off = w.last(AbilityEventKind::EnginesOffline, 1);
    CHECK(off && off->value == 0);
    CHECK(w.sys->shield(1) == 0.0f && w.sys->shield_regen_blocked(1));
    CHECK(!w.ab->can_activate(1, kInterdict, true, true, w.frame) && !w.ab->can_activate(1, kDefend, true, true, w.frame));
    CHECK(!w.ab->special_enabled(1, crc) && !w.ab->special_enabled(2, crc) && w.ab->special_enabled(3, crc));
    // Destroyed engine: value 1 (permanent for the host's EngineControl).
    DamageRequest r;
    r.victim = 2;
    r.amount = 1000.0f;
    r.hardpoint = 0;
    w.sys->take_damage(r, w.frame);
    w.collect();
    off = w.last(AbilityEventKind::EnginesOffline, 2);
    CHECK(off && off->value == 1);
    for (int i = 0; i < 160; ++i) w.tick();
    CHECK(w.sys->shield(1) == 0.0f);  // blocked: no regeneration (and past the 5 s depleted lockout, so DEFEND is gated by +0x131 only)
    CHECK(!w.sys->shield_lockout(1, w.frame) && !w.ab->can_activate(1, kDefend, true, true, w.frame));
    // Online: repair each to max (health is full, so one service each).
    for (uint32_t i = 0; i < 4; ++i) CHECK(w.sys->add_hardpoint_repairer(1, i, 0));
    w.tick();
    for (uint32_t i = 0; i < 4; ++i) CHECK(!w.sys->hardpoint_disabled(1, i));
    CHECK(w.count(AbilityEventKind::EnginesOnline, 1) == 1);
    CHECK(!w.sys->shield_regen_blocked(1));
    CHECK(w.ab->can_activate(1, kInterdict, true, true, w.frame) && !w.ab->is_active(1, kInterdict));  // not re-activated
    CHECK(w.ab->can_activate(1, kDefend, true, true, w.frame) && !w.ab->is_active(1, kDefend));
    CHECK(w.ab->special_enabled(1, crc) && w.ab->special_enabled(2, crc));
    for (int i = 0; i < 120 && !(w.sys->shield(1) > 0.0f); ++i) w.tick();
    CHECK(w.sys->shield(1) > 0.0f && w.sys->shield(1) < 200.0f);  // regen resumes from 0
}

// refix t02 (S9_DEATH_CLONE_SHIELDS.md §1.5): the INFECTION behaviour - Infection_Ability fields parsed and linked, the
// hit attaches it with the shooter's parameters, spread after Seconds_To_Infect x FPS consecutive frames in range (enemy
// of the infecting player, Infection_CategoryMask, Distance_To_Infect), Damage_Per_Second once a second, expiry after
// Duration_Of_Infection (suspend), resume by a spread / the API, the death-clone key 20 only while active, save round trip.
// S8 t04 (docs/re/LOB_SOLVER.md §3.4 / §3.5, TACTICAL_GAPS §2.3.5): Grenade_Attack and Remote_Bomb need a lob solution
// to apply, and both toss their (sticky) projectile from the unit through the spawner's grenade branch.
inline ProjectileDesc lob_sticky_bomb() {
    ProjectileDesc g;
    g.name = "Proj_Sticky_Bomb";
    g.max_speed = 15.0f;
    g.lobbed = true;
    g.category = ProjectileCategory::Grenade;
    g.grenade_gravity = 1.0f;
    g.lob_mod = 1.0f;  // G = 1, reach 225 on flat ground
    g.can_lob_slower = true;
    g.sticks_on_collision = true;
    g.max_lifetime = 900.0f;
    g.blast_area_damage = 30.0f;
    g.blast_area_range = 20.0f;
    return g;
}

inline void test_lob_abilities() {
    for (const bool reachable : {true, false}) {  // Grenade_Attack
        OiWorld w;
        w.cd.projectiles = {lob_sticky_bomb()};
        SpecialAbilityDesc g = oi_special("Grenade", SpecialClass::GrenadeAttack, ActivationStyle::UserInput);
        g.max_range = 200.0f;
        g.projectile = 0;
        g.fuse_time = 2.0f;
        const uint32_t gs = add_special(w.ad, g);
        AbilityObjectDesc a;
        a.unit_abilities = {oi_gui(kStickyBomb, 0.0f, 30.0f, "Grenade")};
        a.specials = {gs};
        const uint32_t ud = push(w.cd, w.ad, oi_unit("Trooper", 100.0f), a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("Tank", 100.0f), AbilityObjectDesc{});
        w.make(GameMode::Land);
        w.add(1, ud, 0, {0, 0, 0});
        // In range either way (80 / 100 XY); the second sits 300 up: v^2 < G h, no arc -> the apply refuses.
        w.add(2, td, 1, reachable ? Vec3{80, 0, 0} : Vec3{100, 0, 300});
        const ObjectId one[] = {1};
        CHECK(w.ab->targeted_ability(one, kStickyBomb, 2, -1, w.frame) == 1);
        for (int i = 0; i < 5 && w.sys->projectile_count() == 0; ++i) w.tick();  // apply, then the throw (Spawn_Frame 0)
        if (!reachable) {
            for (int i = 0; i < 30; ++i) w.tick();
            CHECK(w.sys->projectile_count() == 0u && w.ab->countdown_counter(1, kStickyBomb) == 0);
            continue;
        }
        CHECK(w.sys->projectile_count() == 1u);
        if (w.sys->projectile_count() != 1u) continue;
        const ProjectileView pv = w.sys->projectile(0);
        CHECK(pv.lob && pv.target == 2 && pv.pos.x == 0.0f && pv.pos.z == 0.0f);  // tossed from the unit
        int guard = 0;
        while (w.sys->projectile_count() > 0 && !w.sys->projectile(0).stuck && guard++ < 100) w.tick();
        CHECK(w.sys->projectile_count() == 1u && w.sys->projectile(0).stuck_to == 2);
        CHECK(w.sys->health(2) == w.sys->max_health(2));  // sticking does no damage
        while (w.sys->projectile_count() > 0 && guard++ < 200) w.tick();
        CHECK(w.sys->health(2) < w.sys->max_health(2));  // the 2 s timer set it off
    }
    {  // S8 t06 (LOB_SOLVER §5.2): a null spawn (the arc is gone at the throw) - not set active, no recharge
        OiWorld w;
        w.cd.projectiles = {lob_sticky_bomb()};
        SpecialAbilityDesc g = oi_special("Grenade", SpecialClass::GrenadeAttack, ActivationStyle::UserInput);
        g.max_range = 200.0f;
        g.projectile = 0;
        g.fuse_time = 2.0f;
        g.spawn_frame = 5.0f;
        const uint32_t gs = add_special(w.ad, g);
        AbilityObjectDesc a;
        a.unit_abilities = {oi_gui(kStickyBomb, 0.0f, 30.0f, "Grenade")};
        a.specials = {gs};
        const uint32_t ud = push(w.cd, w.ad, oi_unit("Trooper", 100.0f), a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("Tank", 100.0f), AbilityObjectDesc{});
        w.make(GameMode::Land);
        w.add(1, ud, 0, {0, 0, 0});
        w.add(2, td, 1, {80, 0, 0});
        const ObjectId one[] = {1};
        CHECK(w.ab->targeted_ability(one, kStickyBomb, 2, -1, w.frame) == 1);
        w.tick();  // the apply (the pending ability); the throw comes 5 frames later
        CHECK(w.ab->special_applied(1, name_crc("Grenade")));
        w.move(2, Vec3{80, 0, 300});  // out of reach before the throw
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 0u && w.ab->countdown_counter(1, kStickyBomb) == 0);
        CHECK(!w.ab->is_active(1, kStickyBomb));
        CHECK(w.count(AbilityEventKind::Ended, 1) == 1);
    }
    {  // S8 t10 follow-up (LOB_SOLVER §3.5 "Facing at apply", last item): a pending request whose countdown started before
       // the apply is consumed (apply returns 1) without the face / stop / lock block - nothing thrown, not retried
        OiWorld w;
        w.cd.projectiles = {lob_sticky_bomb()};
        SpecialAbilityDesc g = oi_special("Grenade", SpecialClass::GrenadeAttack, ActivationStyle::UserInput);
        g.max_range = 200.0f;
        g.projectile = 0;
        g.fuse_time = 2.0f;
        const uint32_t gs = add_special(w.ad, g);
        AbilityObjectDesc a;
        a.unit_abilities = {oi_gui(kStickyBomb, 0.0f, 30.0f, "Grenade")};
        a.specials = {gs};
        const uint32_t ud = push(w.cd, w.ad, oi_unit("Trooper", 100.0f), a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("Tank", 100.0f), AbilityObjectDesc{});
        w.make(GameMode::Land);
        w.add(1, ud, 0, {0, 0, 0});
        w.add(2, td, 1, {80, 0, 0});
        const ObjectId one[] = {1};
        CHECK(w.ab->targeted_ability(one, kStickyBomb, 2, -1, w.frame) == 1 && w.ab->pending_type(1) >= 0);
        CHECK(w.ab->force_recharge(1, kStickyBomb) && w.ab->countdown_counter(1, kStickyBomb) > 0);
        w.tick();
        CHECK(w.ab->pending_type(1) < 0 && !w.ab->special_applied(1, name_crc("Grenade")));  // consumed, not applied
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 0u && !w.ab->is_active(1, kStickyBomb) && w.ab->pending_type(1) < 0);
    }
}

// S8 t06 (docs/re/LOB_SOLVER.md §3.5): Remote_Bomb world - bomber 1 (player 0; Remote_Bomb with Toss_Anim BOMBTOSS,
// Spawn_Frame 12, Spawn_Bone MuzzleB_00; Force_Lightning), the bomb target 2 (player 1, health 1000) at `at`, trooper 3
// (player 1) for the lightning. `clips`: BOMBTOSS (id 77) 30 frames and FL_Attack (0x28) 32 frames at 30 fps, and the
// bone MuzzleB_00 at the bomber + (1, 2, 8). Two warm-up ticks align the services (the update runs every frame).
inline void rb_world(OiWorld& w, Vec3 at, bool clips) {
    ProjectileDesc b = lob_sticky_bomb();
    b.name = "Proj_Remote_Bomb";
    b.friendly_damage_amount = 200.0f;
    w.cd.projectiles = {b};
    SpecialAbilityDesc r = oi_special("Remote", SpecialClass::RemoteBomb, ActivationStyle::UserInput);
    r.max_range = 200.0f;
    r.projectile = 0;
    r.toss_anim = "BOMBTOSS";
    r.spawn_frame = 12.0f;
    r.spawn_bone = "MuzzleB_00";
    SpecialAbilityDesc l = oi_special("Lightning", SpecialClass::ForceLightning, ActivationStyle::UserInput);
    l.max_range = 300.0f;
    l.max_targets = 1;
    l.targets_radius = 10.0f;
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kPlaceRemoteBomb, 0.0f, 30.0f, "Remote"), rec(kDetonateRemoteBomb, 0.0f, 0.0f),
                        oi_gui(kForceLightning, 0.0f, 30.0f, "Lightning")};
    a.specials = {add_special(w.ad, r), add_special(w.ad, l)};
    UnitDesc bd = oi_unit("Bomber", 100.0f);
    bd.ranged_target_z_adjust = 4.0f;
    const uint32_t ud = push(w.cd, w.ad, bd, a);
    UnitDesc tdesc = oi_unit("Tank", 1000.0f);
    tdesc.ranged_target_z_adjust = 6.0f;
    const uint32_t td = push(w.cd, w.ad, tdesc, AbilityObjectDesc{});
    w.make(GameMode::Land);
    if (clips) {
        AbilityHost h;
        h.anim_type = [](std::string_view n) { return n == "BOMBTOSS" ? 77 : -1; };
        h.anim_clip = [](ObjectId, int32_t type, std::string_view name, uint32_t, uint32_t& n, float& f) {
            if (type == 0x28) n = 32;
            else if (type == 77 || name == "BOMBTOSS") n = 30;
            else return false;
            f = 30.0f;
            return true;
        };
        CombatSystem* sys = w.sys.get();
        h.bone_position = [sys](ObjectId id, std::string_view bone, Vec3& p) {
            if (bone != "MuzzleB_00") return 0;
            p = sys->position(id) + Vec3{1, 2, 8};
            return 1;
        };
        w.ab->set_host(std::move(h));
    }
    w.add(1, ud, 0, {0, 0, 0});
    w.add(2, td, 1, at);
    w.add(3, td, 1, {150, 100, 0});
    w.tick();
    w.tick();
}

inline void test_remote_bomb_sequence() {
    const ObjectId one[] = {1};
    {  // unreachable (v^2 < G h): the apply refuses
        OiWorld w;
        rb_world(w, Vec3{100, 0, 300}, false);
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);  // queued as the pending ability
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 0u && w.ab->immobilize_count(1) == 0);
    }
    {  // no clips (headless): apply = stop + lock, nothing spawned, no recharge; the toss 12 frames after the unit
       // stopped (rate 1), from the unit + its Ranged_Target_Z_Adjust (no bone host); recharge at the toss; the ability
       // ends at the toss (no clip): unlocked, Ended
        OiWorld w;
        rb_world(w, Vec3{120, 0, 0}, false);
        const uint32_t f0 = w.frame;
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);  // pending: applied in the next tick
        w.tick();
        CHECK(w.sys->projectile_count() == 0u && w.ab->immobilize_count(1) == 1);
        CHECK(w.ab->countdown_counter(1, kPlaceRemoteBomb) == 0);
        uint32_t toss = 0;
        for (int i = 0; i < 40 && toss == 0; ++i) {
            w.tick();
            if (w.sys->projectile_count() == 1u) toss = w.frame - 1;
            if (toss == 0) CHECK(w.ab->countdown_counter(1, kPlaceRemoteBomb) == 0 && w.ab->immobilize_count(1) == 1);
        }
        CHECK(toss == f0 + 12);
        CHECK(w.ab->countdown_counter(1, kPlaceRemoteBomb) > 0 && w.ab->immobilize_count(1) == 0);
        CHECK(w.count(AbilityEventKind::Ended, 1) == 1);
        if (w.sys->projectile_count() != 1u) return;
        const ProjectileView pv = w.sys->projectile(0);
        CHECK(pv.lob && pv.pos.x == 0.0f && pv.pos.y == 0.0f && pv.pos.z == 4.0f);
        int guard = 0;
        while (!w.sys->projectile(0).stuck && guard++ < 100) w.tick();
        CHECK(w.sys->projectile(0).stuck && w.sys->projectile(0).stuck_to == 2);
        for (int i = 0; i < 60; ++i) w.tick();
        // never armed: still waiting for the trigger
        CHECK(w.sys->projectile_count() == 1u && w.sys->health(2) == w.sys->max_health(2));
        CHECK(w.ab->set_unit_ability_active(1, kDetonateRemoteBomb, true, w.frame));
        CHECK(w.sys->projectile_count() == 1u && !w.sys->projectile(0).alive);  // detonated (flushed next tick)
        CHECK(w.sys->health(2) < w.sys->max_health(2));                         // area damage on the enemy
    }
    {  // clips: Toss_Anim from the frame the unit stopped; the toss at its frame 12 from Spawn_Bone; the lock is held to
       // the animation's end (frame 28 = 30 - 2), then released
        OiWorld w;
        rb_world(w, Vec3{120, 0, 0}, true);
        const uint32_t f0 = w.frame;
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);
        uint32_t toss = 0, unlock = 0;
        for (int i = 0; i < 60 && unlock == 0; ++i) {
            w.tick();
            if (toss == 0 && w.sys->projectile_count() == 1u) toss = w.frame - 1;
            if (w.ab->immobilize_count(1) == 0) unlock = w.frame - 1;
        }
        CHECK(toss == f0 + 12 && unlock == f0 + 28);
        CHECK(w.ab->countdown_counter(1, kPlaceRemoteBomb) > 0 && w.count(AbilityEventKind::Ended, 1) == 1);
        if (w.sys->projectile_count() >= 1u) CHECK(w.sys->projectile(0).spawn_frame == toss);
    }
    {  // clips + the launch point: the bomb leaves from the bone (bomber + (1, 2, 8))
        OiWorld w;
        rb_world(w, Vec3{120, 0, 0}, true);
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);
        for (int i = 0; i < 20 && w.sys->projectile_count() == 0u; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 1u);
        if (w.sys->projectile_count() == 1u) {
            const ProjectileView pv = w.sys->projectile(0);
            CHECK(pv.pos.x == 1.0f && pv.pos.y == 2.0f && pv.pos.z == 8.0f);
        }
    }
    {  // interrupted: Force_Lightning replaces Toss_Anim before frame 12 - the same cleanup, no toss, no recharge
        OiWorld w;
        rb_world(w, Vec3{120, 0, 0}, true);
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);
        for (int i = 0; i < 5; ++i) w.tick();
        CHECK(w.ab->anim_type_at(1, w.frame) == 77);
        CHECK(w.ab->targeted_ability(one, kForceLightning, 3, -1, w.frame) == 1);
        for (int i = 0; i < 40; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 0u && w.ab->countdown_counter(1, kPlaceRemoteBomb) == 0);
        CHECK(w.ab->immobilize_count(1) == 0 && w.count(AbilityEventKind::Ended, 1) == 1);
    }
    {  // null spawn (§5.2): the target rose out of reach before the toss - no bomb, no recharge, no retry; the ability
       // still ends
        OiWorld w;
        rb_world(w, Vec3{120, 0, 0}, false);
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);
        for (int i = 0; i < 3; ++i) w.tick();
        w.move(2, Vec3{120, 0, 400});
        for (int i = 0; i < 40; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 0u && w.ab->countdown_counter(1, kPlaceRemoteBomb) == 0);
        CHECK(w.ab->immobilize_count(1) == 0 && w.count(AbilityEventKind::Ended, 1) == 1);
    }
}

inline void test_remote_bomb_detonate() {
    // S8 t06 (LOB_SOLVER §3.5, TACTICAL_COMBAT §3.4 step 3): Friendly_Damage_Amount 200 lets the sweep see own units, so
    // the bomb sticks to own unit 4 (team 10 = {4, 5}). DETONATE_REMOTE_BOMB skips it while 4 is garrisoned; then 4 takes
    // kind 1 x 200 and its team-mate 5 kind 0x15 x 200 before the detonation; 6 (own, no team) is untouched.
    OiWorld w;
    rb_world(w, Vec3{-500, -500, 0}, false);
    AbilityObjectDesc sq;
    sq.name = "Squad";
    const uint32_t sqd = push_container(w.cd, w.ad, sq);
    const uint32_t md = push(w.cd, w.ad, oi_unit("Member", 1000.0f), AbilityObjectDesc{});
    w.add(4, md, 0, {120, 0, 0}, {1, 0, 0}, 10);
    w.add(5, md, 0, {-300, 0, 0}, {1, 0, 0}, 10);
    w.add(6, md, 0, {-300, 100, 0});
    const ObjectId mem[] = {4, 5};
    CHECK(w.ab->add_container(10, sqd, 0, mem, w.frame, &w.rng));
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 4, -1, w.frame) == 1);
    int guard = 0;
    while ((w.sys->projectile_count() == 0u || !w.sys->projectile(0).stuck) && guard++ < 200) w.tick();
    CHECK(w.sys->projectile_count() == 1u && w.sys->projectile(0).stuck && w.sys->projectile(0).stuck_to == 4);
    CHECK(w.sys->health(4) == 1000.0f);  // sticking does no damage
    w.sys->set_flag(4, kUnitGarrisoned, true);
    CHECK(w.ab->set_unit_ability_active(1, kDetonateRemoteBomb, true, w.frame));
    CHECK(w.sys->projectile(0).alive && w.sys->health(4) == 1000.0f);  // skipped: stays armed
    w.sys->set_flag(4, kUnitGarrisoned, false);
    CHECK(w.ab->force_recharge(1, kDetonateRemoteBomb, 0.0f));
    CHECK(w.ab->set_unit_ability_active(1, kDetonateRemoteBomb, true, w.frame));
    CHECK(!w.sys->projectile(0).alive);
    CHECK(w.sys->health(4) == 800.0f && w.sys->health(5) == 800.0f && w.sys->health(6) == 1000.0f);
}

inline void test_infection() {
    {  // XML -> SpecialAbilityDesc -> UnitDesc::infection
        const data::XmlDocument doc = data::XmlDocument::parse(
            "<Infection_Ability Name=\"IG88_Infection\"><Activation_Style> User_Input </Activation_Style>"
            "<Damage_Per_Second>2</Damage_Per_Second><Duration_Of_Infection>30.0</Duration_Of_Infection>"
            "<Infection_CategoryMask>Infantry</Infection_CategoryMask><Distance_To_Infect>70.0</Distance_To_Infect>"
            "<Projectile_Type>Infection_Projectile</Projectile_Type><Seconds_To_Infect>2.0</Seconds_To_Infect></Infection_Ability>",
            "inf.xml");
        AbilityResolvers r;
        r.category = [](std::string_view n) -> uint64_t { return iequals(n, "Infantry") ? 16u : 0u; };
        std::vector<SpecialAbilityDesc> specials{SpecialAbilityDesc{}, parse_special_ability(doc.root(), r, nullptr)};
        const SpecialAbilityDesc& s = specials[1];
        CHECK(s.cls == SpecialClass::Infection && s.distance_to_infect == 70.0f && s.seconds_to_infect == 2.0f);
        CHECK(s.duration_secs == 30.0f && s.damage_per_second == 2.0f && s.infection_categories == 16u);
        AbilityObjectDesc a;
        a.specials = {0, 1};
        UnitDesc ud;
        link_infection(a, specials, ud);
        CHECK(ud.infection.has && ud.infection.distance_to_infect == 70.0f && ud.infection.seconds_to_infect == 2.0f &&
              ud.infection.duration_secs == 30.0f && ud.infection.damage_per_second == 2.0f && ud.infection.category_mask == 16u);
        a.specials = {0};
        link_infection(a, specials, ud);
        CHECK(!ud.infection.has);
    }
    constexpr uint64_t kInf = 16;
    World w;
    ProjectileDesc inf;
    inf.name = "Infection_Projectile";
    inf.max_speed = 20.0f;
    inf.damage = 1.0f;
    inf.max_flight_distance = 3000.0f;
    inf.causes_infection = true;
    w.cd.projectiles = {inf};
    UnitDesc ig;
    ig.name = "IG88";
    ig.tactical_health = 500.0f;
    ig.radius = 5.0f;
    ig.category_mask = kInf;
    ig.infection.has = true;
    ig.infection.distance_to_infect = 70.0f;
    ig.infection.seconds_to_infect = 2.0f;
    ig.infection.duration_secs = 5.0f;
    ig.infection.damage_per_second = 2.0f;
    ig.infection.category_mask = kInf;
    UnitDesc trooper;
    trooper.name = "Trooper";
    trooper.object_type = 41;
    trooper.tactical_health = 100.0f;
    trooper.radius = 5.0f;
    trooper.category_mask = kInf;
    trooper.death_clones = {{1, 60}, {20, 63}};
    UnitDesc tank = trooper;
    tank.name = "Tank";
    tank.category_mask = 2;
    const uint32_t igd = push(w.cd, w.ad, ig, AbilityObjectDesc{});
    const uint32_t trd = push(w.cd, w.ad, trooper, AbilityObjectDesc{});
    const uint32_t tkd = push(w.cd, w.ad, tank, AbilityObjectDesc{});
    w.make();
    w.add(1, igd, 0, {-1000, 0, 0});
    w.add(10, trd, 1, {0, 0, 0});    // A: hit by the projectile
    w.add(11, trd, 1, {50, 0, 0});   // B: in range of A
    w.add(12, tkd, 1, {0, 30, 0});   // in range, not in Infection_CategoryMask (the spread's only filter on type)
    w.add(13, trd, 0, {0, -40, 0});  // in range, the infecting player's own: spreading covers every player
    w.add(14, trd, 1, {200, 0, 0});  // out of range (150 from B)
    w.add(15, trd, 1, {0, 60, 0});   // D: in range of A only (78 from B), leaves for one frame
    auto infected = [&w](ObjectId id) { return (w.sys->flags(id) & kUnitInfected) != 0; };
    auto tick_to = [&w](uint32_t f) {  // tick until frame f has been ticked
        while (w.frame <= f) w.tick();
    };
    ProjectileLaunch l;
    l.shooter = 1;
    l.owner = 0;
    l.projectile = 0;
    l.from = {-100, 0, 0};
    l.target = 10;
    CHECK(w.sys->launch_projectile(l, w.frame));
    for (int i = 0; i < 20 && !w.sys->infection_attached(10); ++i) w.tick();
    CHECK(w.sys->infection_attached(10) && infected(10) && w.sys->infection_suspend_count(10) == 0);
    // Frames left = int(FPS x Duration_Of_Infection) = 150 at the hit, - 1 per service: s0 = A's first service frame.
    const uint32_t fa = w.frame - 1;
    const int32_t left0 = w.sys->infection_frames_left(10);
    CHECK(left0 == 150 || left0 == 149);
    const uint32_t s0 = left0 == 149 ? fa : fa + 1;
    tick_to(s0);
    CHECK(w.sys->infection_frames_left(10) == 149);
    CHECK(w.sys->infection_spread_count(11) == 0 && w.sys->infection_spread_count(15) == 0);
    CHECK(w.sys->infection_spread_count(12) == -1 && w.sys->infection_spread_count(14) == -1);  // filtered, never counted
    const float ha = w.sys->health(10);
    // The coughing draw: one synced Get_Int(0, 4) per serviced infection on frames with frame % 60 == 0.
    uint64_t expect_draws = 0;
    const uint64_t d0 = w.rng.draws();
    for (uint32_t f = s0 + 1; f <= s0 + 29; ++f) {
        if (f % 60u == 0u) ++expect_draws;
        w.tick();
    }
    CHECK(w.rng.draws() - d0 == expect_draws);
    // Damage_Per_Second / FPS on every service frame (s0 + 1 .. s0 + 29: 29 frames).
    CHECK(std::abs(w.sys->health(10) - (ha - 29.0f * (2.0f / 30.0f))) < 1e-3f);
    w.move(15, {0, 300, 0});
    tick_to(s0 + 30);  // D out of range for one frame: its consecutive count restarts
    w.move(15, {0, 60, 0});
    tick_to(s0 + 59);
    CHECK(w.sys->infection_spread_count(11) == 59 && w.sys->infection_spread_count(15) == 28);
    CHECK(!infected(11) && !infected(13) && !infected(15));
    tick_to(s0 + 60);  // count 60 >= FPS x Seconds_To_Infect: B and the infector's own unit 13 are infected
    CHECK(infected(11) && w.sys->infection_attached(11) && infected(13) && !infected(15));
    // full duration; B (id 11) was serviced before A this frame (the per-unit walk runs in descending slot order)
    CHECK(w.sys->infection_spread_count(11) == 0 && w.sys->infection_frames_left(11) == 150);
    CHECK(!w.sys->infection_attached(12) && !w.sys->infection_attached(14));
    tick_to(s0 + 90);
    CHECK(!infected(15));
    tick_to(s0 + 91);  // D, counted again from s0 + 31
    CHECK(infected(15));
    // Expiry: A's frames left reach 0 at its service on s0 + 149 -> suspended (flag cleared, no more damage).
    tick_to(s0 + 148);
    CHECK(infected(10) && w.sys->infection_frames_left(10) == 1);
    tick_to(s0 + 149);
    CHECK(!infected(10) && w.sys->infection_attached(10) && w.sys->infection_suspend_count(10) == 1);
    const float h1 = w.sys->health(10);
    tick_to(s0 + 150);
    CHECK(w.sys->health(10) == h1);
    // A suspended is a spread candidate again (B, 13 and D are in range): resumed after 60 counted frames, with a full
    // pack again.
    for (int i = 0; i < 80 && !infected(10); ++i) w.tick();
    CHECK(infected(10) && w.sys->infection_suspend_count(10) == 0 && w.frame - 1 <= s0 + 211);
    CHECK(w.sys->infection_frames_left(10) >= 149);
    // vfunc +0xc0 / +0xb8 through the API: counter +1 / -1 (floored), the flag follows counter 0; the pack is kept.
    const int32_t keep = w.sys->infection_frames_left(10);
    w.sys->suspend_infection(10);
    w.sys->suspend_infection(10);
    CHECK(w.sys->infection_suspend_count(10) == 2 && !infected(10));
    w.sys->resume_infection(10, w.frame);
    CHECK(w.sys->infection_suspend_count(10) == 1 && !infected(10));
    w.sys->resume_infection(10, w.frame);
    w.sys->resume_infection(10, w.frame);
    CHECK(w.sys->infection_suspend_count(10) == 0 && infected(10) && w.sys->infection_frames_left(10) == keep);
    w.sys->suspend_infection(12);  // no behaviour: nothing
    CHECK(!w.sys->infection_attached(12) && w.sys->infection_suspend_count(12) == 0);
    // Save round trip (the packs and the spread counters).
    {
        ByteWriter a;
        w.sys->write(a);
        const uint64_t dg = battle_digest(*w.sys, *w.ab, w.rng);
        ByteReader r(a.bytes());
        CHECK(w.sys->read(r));
        ByteWriter b;
        w.sys->write(b);
        CHECK(a.bytes() == b.bytes() && battle_digest(*w.sys, *w.ab, w.rng) == dg);
        CHECK(infected(10) && w.sys->infection_suspend_count(11) == 1 && w.sys->infection_attached(15));
        CHECK(w.sys->infection_frames_left(10) == keep);
    }
    // Die: the active INFECTION remaps the key to 20; a suspended one does not.
    w.sys->kill_unit(10, 1, kKindNormal, w.frame);
    CHECK(!w.sys->deaths().empty() && w.sys->deaths().back().death_clone == 63 && w.sys->deaths().back().clone_key == 20);
    w.sys->kill_unit(11, 1, kKindNormal, w.frame);
    CHECK(!w.sys->deaths().empty() && w.sys->deaths().back().death_clone == 60 && w.sys->deaths().back().clone_key == 1);
    // set_flag mirrors the behaviour (host / tests): on attaches an active one, off suspends it.
    w.sys->set_flag(14, kUnitInfected, true);
    CHECK(w.sys->infection_attached(14) && infected(14) && w.sys->infection_suspend_count(14) == 0);
    w.sys->set_flag(14, kUnitInfected, false);
    CHECK(w.sys->infection_attached(14) && !infected(14) && w.sys->infection_suspend_count(14) == 1);
    // A direct Causes_Infection hit is not filtered by Infection_CategoryMask: the tank (category 2) is infected.
    l.target = 12;
    l.from = {-100, 30, 0};
    CHECK(w.sys->launch_projectile(l, w.frame));
    for (int i = 0; i < 20 && !w.sys->infection_attached(12); ++i) w.tick();
    CHECK(w.sys->infection_attached(12) && w.sys->infection_suspend_count(12) == 0);
    // Infection_CategoryMask 0: the spread takes nobody (CategoryMask & 0 == 0).
    {
        World z;
        z.cd.projectiles = {inf};
        UnitDesc g0 = ig;
        g0.infection.category_mask = 0;
        const uint32_t gd = push(z.cd, z.ad, g0, AbilityObjectDesc{});
        const uint32_t td = push(z.cd, z.ad, trooper, AbilityObjectDesc{});
        z.make();
        z.add(1, gd, 0, {-1000, 0, 0});
        z.add(10, td, 1, {0, 0, 0});
        z.add(11, td, 1, {20, 0, 0});
        ProjectileLaunch zl = l;
        zl.target = 10;
        zl.from = {-100, 0, 0};
        CHECK(z.sys->launch_projectile(zl, z.frame));
        for (int i = 0; i < 100; ++i) z.tick();
        CHECK(z.sys->infection_attached(10) && !z.sys->infection_attached(11) && z.sys->infection_spread_count(11) == -1);
    }
    // cu8g t05: a candidate without a model (obj +0x2a0 null: UnitDesc::has_model false) is never counted; a parameterless
    // infection (the flag only: no pack, frames left 0) is serviced and expires at its first service (suspended).
    {
        World z;
        z.cd.projectiles = {inf};
        UnitDesc bare = trooper;
        bare.name = "NoModel";
        bare.has_model = false;
        const uint32_t gd = push(z.cd, z.ad, ig, AbilityObjectDesc{});
        const uint32_t td = push(z.cd, z.ad, trooper, AbilityObjectDesc{});
        const uint32_t bd = push(z.cd, z.ad, bare, AbilityObjectDesc{});
        z.make();
        z.add(1, gd, 0, {-1000, 0, 0});
        z.add(10, td, 1, {0, 0, 0});
        z.add(11, bd, 1, {20, 0, 0});    // in range, no model
        z.add(12, td, 1, {0, 30, 0});    // in range, a model: counted (off the projectile's path)
        z.add(13, td, 1, {500, 500, 0});  // far away: the parameterless one
        ProjectileLaunch zl = l;
        zl.target = 10;
        zl.from = {-100, 0, 0};
        CHECK(z.sys->launch_projectile(zl, z.frame));
        for (int i = 0; i < 20 && !z.sys->infection_attached(10); ++i) z.tick();
        CHECK(z.sys->infection_attached(10));
        for (int i = 0; i < 10; ++i) z.tick();
        CHECK(z.sys->infection_spread_count(11) == -1 && z.sys->infection_spread_count(12) > 0);
        z.sys->set_flag(13, kUnitInfected, true);
        CHECK(z.sys->infection_attached(13) && (z.sys->flags(13) & kUnitInfected) != 0 && z.sys->infection_frames_left(13) == 0);
        const float h13 = z.sys->health(13);
        z.tick();
        CHECK(z.sys->infection_attached(13) && (z.sys->flags(13) & kUnitInfected) == 0 && z.sys->infection_suspend_count(13) == 1);
        CHECK(z.sys->health(13) == h13);
    }
}

// S8 t10 (b)(c) (LOB_SOLVER §3.5 "Facing at apply", "MARKER removal") and S8 t11 (DEPLOY through the Walk locomotor host).
struct SnapLog {
    std::vector<std::pair<ObjectId, float>> calls;
};
inline void snap_host(OiWorld& w, SnapLog& log) {
    AbilityHost h;
    h.map_rect = [](Vec3& lo, Vec3& hi) {
        lo = Vec3{-1000.0f, -1000.0f, 0.0f};
        hi = Vec3{1000.0f, 1000.0f, 0.0f};
        return true;
    };
    h.snap_yaw = [&log](ObjectId id, float yaw) { log.calls.emplace_back(id, yaw); };
    w.ab->set_host(std::move(h));
}

inline void test_s8_apply_facing_marker_deploy() {
    const ObjectId one[] = {1};
    {  // Remote_Bomb: yaw snapped at the apply toward the raw target position (90 degrees), stop + lock, the toss, release
       // without restoring the facing, then MarkerTossed for the target (the host kills it when it is a marker)
        OiWorld w;
        rb_world(w, Vec3{0, 120, 0}, false);
        SnapLog log;
        snap_host(w, log);
        const uint32_t f0 = w.frame;
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);
        w.tick();
        CHECK(log.calls.size() == 1 && log.calls[0].first == 1 && std::fabs(log.calls[0].second - 90.0f) < 1e-3f);
        const Vec3 fc = w.sys->facing(1);
        CHECK(std::fabs(fc.x) < 1e-5f && std::fabs(fc.y - 1.0f) < 1e-5f && w.ab->immobilize_count(1) == 1);
        CHECK(w.count(AbilityEventKind::MarkerTossed) == 0);
        for (int i = 0; i < 40 && w.ab->immobilize_count(1) != 0; ++i) w.tick();
        CHECK(w.frame - 1 == f0 + 12 && w.ab->immobilize_count(1) == 0 && w.count(AbilityEventKind::Ended, 1) == 1);
        const AbilityEvent* m = w.last(AbilityEventKind::MarkerTossed, 1);
        CHECK(w.count(AbilityEventKind::MarkerTossed) == 1 && m && m->target == 2 && m->type == kPlaceRemoteBomb && m->frame == f0 + 12);
        CHECK(log.calls.size() == 1 && w.sys->facing(1).y == fc.y);  // not re-faced at the throw, not restored at release
    }
    {  // a null spawn (out of reach at the toss): no recharge, but the marker is still removed
        OiWorld w;
        rb_world(w, Vec3{0, 120, 0}, false);
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);
        w.tick();
        w.move(2, Vec3{0, 120, 300});
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 0u && w.ab->countdown_counter(1, kPlaceRemoteBomb) == 0);
        CHECK(w.count(AbilityEventKind::MarkerTossed, 1) == 1);
    }
    {  // the toss never happens (the bomber dies first): no MarkerTossed, the marker persists
        OiWorld w;
        rb_world(w, Vec3{0, 120, 0}, false);
        CHECK(w.ab->targeted_ability(one, kPlaceRemoteBomb, 2, -1, w.frame) == 1);
        w.tick();
        DamageRequest r;
        r.victim = 1;
        r.amount = 1.0e6f;
        w.sys->take_damage(r, w.frame);
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(!w.sys->alive(1) && w.count(AbilityEventKind::MarkerTossed) == 0);
    }
    {  // Grenade_Attack: the same snap; none while weapons are disabled (stunned)
        for (const bool disabled : {false, true}) {
            OiWorld w;
            w.cd.projectiles = {lob_sticky_bomb()};
            SpecialAbilityDesc g = oi_special("Grenade", SpecialClass::GrenadeAttack, ActivationStyle::UserInput);
            g.max_range = 200.0f;
            g.projectile = 0;
            g.fuse_time = 2.0f;
            const uint32_t gs = add_special(w.ad, g);
            AbilityObjectDesc a;
            a.unit_abilities = {oi_gui(kStickyBomb, 0.0f, 30.0f, "Grenade")};
            a.specials = {gs};
            const uint32_t ud = push(w.cd, w.ad, oi_unit("Trooper", 100.0f), a);
            const uint32_t td = push(w.cd, w.ad, oi_unit("Tank", 100.0f), AbilityObjectDesc{});
            w.make(GameMode::Land);
            SnapLog log;
            snap_host(w, log);
            w.add(1, ud, 0, {0, 0, 0});
            w.add(2, td, 1, {-60, -60, 0});
            if (disabled) w.ab->set_weapons_disabled(1, true);
            w.ab->targeted_ability(one, kStickyBomb, 2, -1, w.frame);
            w.tick();
            if (disabled) {
                CHECK(log.calls.empty());
            } else {
                CHECK(log.calls.size() == 1 && std::fabs(log.calls[0].second + 135.0f) < 1e-3f);
                CHECK(w.ab->special_applied(1, name_crc("Grenade")) && w.ab->immobilize_count(1) == 1);
            }
        }
    }
    {  // DEPLOY through the locomotor host: the request goes to the host, the state comes back through set_deploy_state
        OiWorld w;
        AbilityObjectDesc a;
        a.unit_abilities = {rec(kDeploy, 0.0f, 0.0f)};
        const uint32_t ud = push(w.cd, w.ad, oi_unit("Walker", 100.0f), a);
        w.make(GameMode::Land);
        std::vector<std::pair<ObjectId, bool>> calls;
        bool accept = true;
        AbilityHost h;
        h.deploy = [&](ObjectId id, bool on, uint32_t) {
            calls.emplace_back(id, on);
            return accept;
        };
        w.ab->set_host(std::move(h));
        w.add(1, ud, 0, {0, 0, 0});
        CHECK(w.ab->set_unit_ability_mode(one, kDeploy, true, w.frame) == 1);
        CHECK(calls.size() == 1 && calls[0].first == 1 && calls[0].second && !w.ab->deployed(1));  // not instant
        w.collect();
        CHECK(w.count(AbilityEventKind::Deployed, 1) == 1 && w.count(AbilityEventKind::StopOrder, 1) == 1);
        const uint32_t both = kUnitDeploying | kUnitDeployTransition;
        w.ab->set_deploy_state(1, 0x18);  // Deploying: both weapon sources hold fire (the transition gate)
        CHECK((w.sys->flags(1) & both) == kUnitDeployTransition && !w.ab->deployed(1) && !w.ab->is_active(1, kDeploy));
        CHECK((w.sys->flags(1) & kUnitDeployingIn) != 0u);  // s78n t11: 0x18 alone (the _Deployed range extents)
        w.ab->set_deploy_state(1, 0x19);  // Deployed
        CHECK((w.sys->flags(1) & both) == kUnitDeploying && w.ab->deployed(1) && w.ab->is_active(1, kDeploy));
        CHECK((w.sys->flags(1) & kUnitDeployingIn) == 0u);
        w.ab->set_deploy_state(1, 0x1A);  // Undeploying: transition again, no longer deployed
        CHECK((w.sys->flags(1) & both) == kUnitDeployTransition && !w.ab->deployed(1));
        CHECK((w.sys->flags(1) & kUnitDeployingIn) == 0u);
        w.ab->set_deploy_state(1, 0x18);
        w.ab->set_deploy_state(1, 0);
        CHECK((w.sys->flags(1) & (both | kUnitDeployingIn)) == 0u);
        accept = false;  // the locomotor refused (no Walk, or nothing to do): no Deployed event
        CHECK(w.ab->set_unit_ability_mode(one, kDeploy, true, w.frame) <= 1);
        w.collect();
        CHECK(calls.size() == 2 && w.count(AbilityEventKind::Deployed, 1) == 1);
    }
}

// s78l t05 (TACTICAL_COMBAT §5.4.1): Tactical_Bribe (0x5b0f70) never reads the target filter (the shipped
// Tyber_Tactical_Bribe has no Applicable_* tags): enemy, cost Tactical_Bribe_Cost x the user's modifier > 0 (c <= 1
// rejects), not a named hero, Friendly_Damage_Amount <= 0, no PROXIMITY_MINE, CategoryMask != 0; credits at apply. Hack
// (0x6fd610) uses the filter 0x48e290: a listed type passes first, an empty categories mask rejects the rest.
struct BribeRun {
    uint32_t accepted = 0;
    int32_t owner_after = -1;
    size_t refused = 0, changed = 0;
    float paid = -1.0f;
};
inline BribeRun oi_bribe(const AbilityObjectDesc& target, int32_t target_owner, uint64_t target_cats, float credits = -1.0f,
                  bool corrupted = false, uint32_t target_flags = 0, int pad = -1) {
    OiWorld w;
    SpecialAbilityDesc b = oi_special("Bribe", SpecialClass::TacticalBribe, ActivationStyle::UserInput);
    b.applicable_categories = 0;  // no Applicable_Unit_Categories / _Types (as shipped)
    b.max_range = 1000.0f;
    const uint32_t bs = add_special(w.ad, b);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kTacticalBribe, 0.0f, 10.0f, "Bribe")};
    a.specials = {bs};
    a.bribe_modifier_uncorrupted = 1.5f;
    a.bribe_modifier_corrupted = 3.0f;
    const uint32_t ad = push(w.cd, w.ad, oi_unit("Tyber", 100.0f), a);
    const uint32_t td = push(w.cd, w.ad, oi_unit("Trooper", 100.0f, 5.0f, target_cats), target);
    w.make(GameMode::Land);
    BribeRun out;
    float bank = credits;
    if (credits >= 0.0f || corrupted || pad >= 0) {
        AbilityHost h;
        h.battle_planet_corrupted = [corrupted] { return corrupted; };
        h.pay_credits = [&bank, &out](int32_t player, float cost) {
            if (player != 0 || bank < cost) return false;
            bank -= cost;
            out.paid = cost;
            return true;
        };
        // s78n t05: the host's occupied-build-pad query (pad 1: the target is a pad holding a live object)
        if (pad >= 0) h.occupied_build_pad = [pad](ObjectId id) { return pad == 1 && id == 2; };
        w.ab->set_host(std::move(h));
    }
    w.add(1, ad, 0, {0, 0, 0});
    w.add(2, td, target_owner, {300, 0, 0});
    if (target_flags != 0) w.sys->set_flag(2, target_flags, true);
    const ObjectId one[] = {1};
    out.accepted = w.ab->targeted_ability(one, kTacticalBribe, 2, -1, w.frame);
    for (int i = 0; i < 5; ++i) w.tick();
    out.owner_after = w.sys->owner(2);
    out.refused = w.count(AbilityEventKind::BribeRefused, 1);
    out.changed = w.count(AbilityEventKind::OwnerChanged, 2);
    return out;
}

inline void test_cu8f_abilities_disabled() {
    // cu8f t01 (CU8F_T01_T02_T07_RE.md §1): Cancel_Special_Ability disables the TARGET's special-ability manager (SAP +0x28)
    // on apply and enables it on remove; set cancels the target's automatic effects, clear re-staggers interval specials
    // with one synced draw each (list order) and restores the cancelled effects; every entry point and gate 5 are blocked.
    OiWorld w;
    const uint32_t cs = add_special(w.ad, oi_special("Cancel", SpecialClass::CancelSpecial, ActivationStyle::UserInput));
    SpecialAbilityDesc aura = oi_special("Aura", SpecialClass::CombatBonus, ActivationStyle::SpaceAutomatic);
    aura.bonus[static_cast<size_t>(ModList::Damage)] = 0.25f;
    const uint32_t as = add_special(w.ad, aura);
    const uint32_t ld = add_special(w.ad, oi_special("LD", SpecialClass::LaserDefense, ActivationStyle::UserInput));  // interval 1
    const uint32_t eq = add_special(w.ad, oi_special("EQ", SpecialClass::Earthquake, ActivationStyle::SpecialAttack));  // interval 3
    AbilityObjectDesc user;
    user.unit_abilities = {oi_gui(kTargetedHack, 0.0f, 0.0f, "Cancel")};
    user.specials = {cs};
    const uint32_t ud = push(w.cd, w.ad, oi_unit("User", 100.0f), user);
    AbilityObjectDesc hero;
    hero.unit_abilities = {rec(kSprint, 2.0f, 4.0f)};
    hero.specials = {as, ld, eq};
    hero.apply_self_on_creation = true;  // s78n t04: the aura reaches its own source only with this type flag
    const uint32_t hd = push(w.cd, w.ad, oi_unit("Hero", 100.0f), hero);
    w.make();
    w.add(1, ud, 0, {0, 0, 0});
    w.add(2, hd, 1, {300, 0, 0});
    w.add(3, hd, 0, {-300, 0, 0});  // the user's own side: refused (friendly)
    const uint32_t cancel = name_crc("Cancel");
    CHECK_EQ(w.sys->modifier(2, ModList::Damage), 0.25f);
    CHECK(!w.ab->abilities_disabled(2) && w.ab->can_activate(2, kSprint, true, true, w.frame));
    const ObjectId users[] = {1};
    CHECK(w.ab->targeted_ability(users, kTargetedHack, 3, -1, w.frame) == 0);  // accepts-target 0x6f4060: not friendly
    CHECK(w.ab->targeted_ability(users, kTargetedHack, 2, -1, w.frame) == 1);
    w.tick();
    CHECK(w.ab->special_applied(1, cancel) && w.ab->special_target(1, cancel) == 2);
    CHECK(w.ab->abilities_disabled(2) && !w.ab->abilities_disabled(1) && !w.ab->abilities_disabled(3));
    CHECK_EQ(w.sys->modifier(2, ModList::Damage), 0.0f);           // the automatic aura was cancelled
    CHECK(!w.ab->can_activate(2, kSprint, true, true, w.frame));   // gate 5
    const ObjectId two[] = {2};
    CHECK(w.ab->set_unit_ability_mode(two, kSprint, true, w.frame) == 0);
    for (int i = 0; i < 5; ++i) w.tick();
    CHECK(w.ab->abilities_disabled(2) && w.sys->modifier(2, ModList::Damage) == 0.0f);
    // A second set is a no-op; a direct clear (the remove's SAP_Enable_All) makes one synced draw per interval special,
    // list order (LD: Get_Int(now, now + 1), EQ: Get_Int(now, now + 3); the aura has interval 0), then restores the aura.
    w.ab->set_abilities_disabled(2, true, w.frame);
    SimRng pred = w.rng;
    const auto now = static_cast<int32_t>(w.frame);
    pred.range_int(now, now + 1);
    pred.range_int(now, now + 3);
    {  // save / load while the restore waits (version 4 carries the restore target)
        ByteWriter bw;
        w.ab->write(bw);
        ByteReader br(bw.bytes());
        CHECK(w.ab->read(br));
    }
    w.ab->set_abilities_disabled(2, false, w.frame);
    CHECK(w.rng.state() == pred.state());
    CHECK(!w.ab->abilities_disabled(2) && w.ab->can_activate(2, kSprint, true, true, w.frame));
    CHECK_EQ(w.sys->modifier(2, ModList::Damage), 0.25f);
    w.ab->set_abilities_disabled(2, false, w.frame);  // already clear: no draw
    CHECK(w.rng.state() == pred.state());
    // The user's death removes the Cancel effect: SAP_Enable_All on its target.
    w.ab->set_abilities_disabled(2, true, w.frame);
    CHECK_EQ(w.sys->modifier(2, ModList::Damage), 0.0f);
    w.sys->kill_unit(1, kNoObject, kKindNormal, w.frame);
    CHECK(!w.ab->abilities_disabled(2) && w.sys->modifier(2, ModList::Damage) == 0.25f);

    // cu8f t07: the Force-disable lockout (gate 3 / 0x380b10) with Force_Ability_Disable_Time 0.0 at 30 FPS: locked on the
    // stamp frame and the two after it, usable again on the 3rd frame after the last stamp.
    OiWorld f;
    AbilityObjectDesc jedi;
    jedi.unit_abilities = {rec(kForceSight, 0.0f, 0.0f)};
    const uint32_t jd = push(f.cd, f.ad, oi_unit("Jedi", 100.0f), jedi);
    f.make(GameMode::Land);
    f.add(1, jd, 0, {0, 0, 0});
    CHECK(f.ab->has_force_ability(1));
    const uint32_t s = 100;
    f.ab->set_force_disabled(1, s);
    CHECK(!f.ab->can_activate(1, kForceSight, true, true, s));
    CHECK(!f.ab->can_activate(1, kForceSight, true, true, s + 2));
    CHECK(f.ab->can_activate(1, kForceSight, true, true, s + 3));
}

inline void test_bribe_hack_accept() {
    constexpr uint64_t kCatInfantry = 16;
    {  // no filter tags, no cost data ([port] free): an enemy infantry is bribed at once
        const BribeRun r = oi_bribe(AbilityObjectDesc{}, 1, kCatInfantry);
        CHECK(r.accepted == 1 && r.owner_after == 0 && r.changed == 1 && r.refused == 0);
    }
    AbilityObjectDesc priced;
    priced.tactical_bribe_cost = 100;
    {  // cost 100 x Bribe_Modifier_On_Uncorrupted_Planets 1.5 = 150 paid
        const BribeRun r = oi_bribe(priced, 1, kCatInfantry, 1000.0f);
        CHECK(r.accepted == 1 && r.owner_after == 0 && r.paid == 150.0f && r.refused == 0);
    }
    {  // corrupted planet: x 3
        const BribeRun r = oi_bribe(priced, 1, kCatInfantry, 1000.0f, true);
        CHECK(r.accepted == 1 && r.owner_after == 0 && r.paid == 300.0f);
    }
    {  // too few credits: accepted as a target, the apply plays the "cannot" event, no conversion
        const BribeRun r = oi_bribe(priced, 1, kCatInfantry, 100.0f);
        CHECK(r.accepted == 1 && r.owner_after == 1 && r.changed == 0 && r.refused == 1 && r.paid < 0.0f);
    }
    {  // a named hero is never bribed
        AbilityObjectDesc hero = priced;
        hero.named_hero = true;
        const BribeRun r = oi_bribe(hero, 1, kCatInfantry, 1000.0f);
        CHECK(r.accepted == 0 && r.owner_after == 1 && r.changed == 0);
        AbilityObjectDesc generic = priced;
        generic.hero = true;  // Is_Generic_Hero alone does not block
        CHECK(oi_bribe(generic, 1, kCatInfantry, 1000.0f).owner_after == 0);
    }
    {  // an ally (same owner) is not a target
        const BribeRun r = oi_bribe(AbilityObjectDesc{}, 0, kCatInfantry);
        CHECK(r.accepted == 0 && r.changed == 0);
    }
    for (int32_t c : {0, 1}) {  // Tactical_Bribe_Cost <= 1: cost 0, rejected
        AbilityObjectDesc cheap;
        cheap.tactical_bribe_cost = c;
        const BribeRun r = oi_bribe(cheap, 1, kCatInfantry, 1000.0f);
        CHECK(r.accepted == 0 && r.owner_after == 1);
    }
    {  // Friendly_Damage_Amount > 0, PROXIMITY_MINE, CategoryMask 0: rejected
        AbilityObjectDesc fd = priced;
        fd.friendly_damage_amount = 5.0f;
        CHECK(oi_bribe(fd, 1, kCatInfantry, 1000.0f).accepted == 0);
        AbilityObjectDesc mine = priced;
        mine.proximity_mine = true;
        CHECK(oi_bribe(mine, 1, kCatInfantry, 1000.0f).accepted == 0);
        CHECK(oi_bribe(priced, 1, 0, 1000.0f).accepted == 0);
    }
    {  // s78n t05 (HERO_CLASH §7 / §8, 0x395c10 steps 2 and 6): a vehicle taken over by a vehicle thief and a build pad
       // holding a live object are refused; the incognito flag alone and an empty pad are not
        CHECK(oi_bribe(priced, 1, kCatInfantry, 1000.0f, false, kUnitStolenVehicle).accepted == 0);
        CHECK(oi_bribe(priced, 1, kCatInfantry, 1000.0f, false, kUnitStolenVehicle | kUnitIncognito).owner_after == 1);
        CHECK(oi_bribe(priced, 1, kCatInfantry, 1000.0f, false, kUnitIncognito).owner_after == 0);
        CHECK(oi_bribe(priced, 1, kCatInfantry, 1000.0f, false, 0, 1).accepted == 0);
        CHECK(oi_bribe(priced, 1, kCatInfantry, 1000.0f, false, 0, 0).owner_after == 0);
    }
    // Hack: Applicable_Unit_Types only (as R2D2_Hack_Ability): a listed type passes (step 1), even when also excluded; an
    // unlisted type fails on the empty categories mask; no tags at all: nothing passes.
    auto hack = [](std::vector<int32_t> types, std::vector<int32_t> excluded, uint64_t cats, int32_t target_type) {
        OiWorld w;
        SpecialAbilityDesc hk = oi_special("Hack", SpecialClass::Hack, ActivationStyle::UserInput);
        hk.applicable_categories = cats;
        hk.applicable_types = std::move(types);
        hk.excluded_types = std::move(excluded);
        hk.max_range = 1000.0f;
        const uint32_t hs = add_special(w.ad, hk);
        AbilityObjectDesc a;
        a.unit_abilities = {oi_gui(kTargetedHack, 0.0f, 10.0f, "Hack")};
        a.specials = {hs};
        const uint32_t ad = push(w.cd, w.ad, oi_unit("R2", 100.0f), a);
        UnitDesc t = oi_unit("Turret", 100.0f, 5.0f, kCatInfantry);
        t.object_type = target_type;
        const uint32_t td = push(w.cd, w.ad, t, AbilityObjectDesc{});
        w.make(GameMode::Land);
        w.add(1, ad, 0, {0, 0, 0});
        w.add(2, td, 1, {300, 0, 0});
        const ObjectId one[] = {1};
        return w.ab->targeted_ability(one, kTargetedHack, 2, -1, w.frame);
    };
    CHECK(hack({7, 9}, {}, 0, 9) == 1);
    CHECK(hack({7, 9}, {9}, 0, 9) == 1);       // the type list is tested before Excluded_Unit_Types
    CHECK(hack({7, 9}, {}, 0, 8) == 0);        // unlisted, empty categories mask
    CHECK(hack({}, {}, 0, 9) == 0);            // no filter tags
    CHECK(hack({}, {}, kCatInfantry, 9) == 1); // categories hit
    CHECK(hack({}, {9}, kCatInfantry, 9) == 0);  // categories hit, then excluded
}

inline void test_maximum_firepower_incognito() {
    // s78n t05 (HERO_CLASH §8.2): MaximumFirepower's end 0x707390 clears the incognito flag (+0x3a1 & 0x10) of the user's
    // team ([port] the user itself; a non-team user here). The stolen-vehicle flag stays.
    OiWorld w;
    SpecialAbilityDesc mf = oi_special("MF", SpecialClass::MaximumFirepower, ActivationStyle::UserInput);
    mf.max_range = 1000.0f;
    const uint32_t ms = add_special(w.ad, mf);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kMaximumFirepower, 5.0f, 10.0f, "MF")};
    a.specials = {ms};
    const uint32_t ad = push(w.cd, w.ad, oi_unit("User", 100.0f), a);
    const uint32_t td = push(w.cd, w.ad, oi_unit("Target", 100.0f), AbilityObjectDesc{});
    w.make();
    w.add(1, ad, 0, {0, 0, 0});
    w.add(2, td, 1, {300, 0, 0});
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kMaximumFirepower, 2, -1, w.frame) == 1);
    w.tick();
    CHECK(w.ab->special_applied(1, name_crc("MF")));
    w.sys->set_flag(1, kUnitIncognito | kUnitStolenVehicle, true);
    w.tick();
    CHECK(w.sys->flags(1) & kUnitIncognito);  // still running
    w.sys->kill_unit(2, kNoObject, kKindNormal, w.frame);
    for (int i = 0; i < 3; ++i) w.tick();
    CHECK(!w.ab->special_applied(1, name_crc("MF")));
    CHECK(!(w.sys->flags(1) & kUnitIncognito) && (w.sys->flags(1) & kUnitStolenVehicle));
}

// s78m t08 (TACTICAL_COMBAT §5.4.2): Tactical_Bribe's range. Tyber (Activation_Max_Range 100, radius 8: not in the formula)
// walks `path` (one position per tick); the target X (id 2, Custom_Hard X / Y extents, facing +x) stands at `xp`. With
// `squad` X is a member of container 25 (a second member, id 3, at `mate`); with `hook` the host reports 25 as a land
// squad container at `tp`. Returns the path index of the tick that converted X, -1 when none did.
struct BribeRangeCase {
    std::vector<Vec3> path;
    Vec3 xp{0, 0, 0}, tp{0, 0, 0}, mate{900, 900, 0};
    float ex = 3.0f, ey = 3.0f, min_range = 0.0f, user_min = 0.0f;
    bool squad = false, hook = false;
    bool melee = false;  // s78n t01: Tyber's Targeting_Max_Attack_Distance 40 (<= Melee_Cutoff_Range 51), else 90
    // s78n t02: the host reports container 25's slot layout (slots along +/-6, member half-extents 3, MaxJiggleDistance
    // 1, facing +x): joint box half-extents X 10, Y 4.
    bool slots = false;
};
inline int bribe_range_tick(const BribeRangeCase& c) {
    OiWorld w;
    SpecialAbilityDesc b = oi_special("Bribe", SpecialClass::TacticalBribe, ActivationStyle::UserInput);
    b.applicable_categories = 0;
    b.max_range = 100.0f;
    b.min_range = c.min_range;
    const uint32_t bs = add_special(w.ad, b);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kTacticalBribe, 0.0f, 10.0f, "Bribe")};
    a.specials = {bs};
    UnitDesc tu = oi_unit("Tyber", 100.0f, 8.0f);
    tu.min_attack_distance = c.user_min;
    tu.max_attack_distance = c.melee ? 40.0f : 90.0f;
    const uint32_t ad = push(w.cd, w.ad, tu, a);
    UnitDesc xu = oi_unit("Squad_Stormtrooper", 100.0f, 5.0f, 16);
    xu.hard_x_extent = c.ex;
    xu.hard_y_extent = c.ey;
    const uint32_t xd = push(w.cd, w.ad, xu, AbilityObjectDesc{});
    AbilityObjectDesc sq;
    sq.name = "Stormtrooper_Team";
    const uint32_t sqd = push_container(w.cd, w.ad, sq);
    CombatConfig cfg;
    cfg.short_range_cutoff = 51.0f;  // Melee_Cutoff_Range
    w.make(GameMode::Land, std::vector<PlayerInfo>(3), cfg);
    if (c.hook) {
        AbilityHost h;
        const Vec3 tp = c.tp;
        h.land_squad_container = [tp](ObjectId id, Vec3& pos) {
            if (id != 25) return false;
            pos = tp;
            return true;
        };
        if (c.slots)
            h.land_squad_slots = [](ObjectId id, std::vector<SquadSlot>& sl, float& jiggle, Vec3& facing) {
                if (id != 25) return false;
                sl = {SquadSlot{-6.0f, 0.0f, 3.0f, 3.0f}, SquadSlot{6.0f, 0.0f, 3.0f, 3.0f}};
                jiggle = 1.0f;
                facing = Vec3{2.0f, 0.0f, 0.0f};  // not normalised: the host's facing vector
                return true;
            };
        w.ab->set_host(std::move(h));
    }
    w.add(1, ad, 0, c.path.front());
    const ObjectId team = c.squad ? ObjectId{25} : kNoObject;
    w.add(2, xd, 1, c.xp, {1, 0, 0}, team);
    if (c.squad) {
        w.add(3, xd, 1, c.mate, {1, 0, 0}, team);
        const ObjectId mem[] = {2, 3};
        CHECK(w.ab->add_container(25, sqd, 1, mem, w.frame, &w.rng));
    }
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kTacticalBribe, 2, -1, w.frame) == 1);
    for (size_t i = 0; i < c.path.size(); ++i) {
        w.move(1, c.path[i]);
        w.tick();
        if (w.sys->owner(2) == 0) return static_cast<int>(i);
    }
    return -1;
}

inline void test_bribe_range() {
    auto at = [](float x, float y) { return std::vector<Vec3>{Vec3{x, y, 0.0f}}; };
    {  // a single unit: 100 + its X extent along its facing, 100 + its Y extent across it (non-strict, XY only)
        BribeRangeCase c;
        c.ex = 4.0f;
        c.ey = 2.0f;
        c.path = at(-104.0f, 0.0f);
        CHECK(bribe_range_tick(c) == 0);
        c.path = at(-104.5f, 0.0f);
        CHECK(bribe_range_tick(c) == -1);
        c.path = at(0.0f, 102.0f);
        CHECK(bribe_range_tick(c) == 0);
        c.path = at(0.0f, 102.5f);
        CHECK(bribe_range_tick(c) == -1);
        c.path = {Vec3{-104.0f, 0.0f, 500.0f}};  // z is ignored
        CHECK(bribe_range_tick(c) == 0);
    }
    BribeRangeCase sq;  // Stormtrooper_Team: Custom_Hard_X/YExtent 2.0 x Scale_Factor 1.5 = 3.0 per member
    sq.squad = true;
    sq.hook = true;
    {  // T and X at one point: the container test, <= 110 (+10, no footprint term)
        sq.path = at(-110.0f, 0.0f);
        CHECK(bribe_range_tick(sq) == 0);
        sq.path = at(-110.5f, 0.0f);
        CHECK(bribe_range_tick(sq) == -1);
    }
    {  // T far away: the member alone, <= 103
        sq.tp = Vec3{500.0f, 0.0f, 0.0f};
        sq.path = at(-103.0f, 0.0f);
        CHECK(bribe_range_tick(sq) == 0);
        sq.path = at(-103.5f, 0.0f);
        CHECK(bribe_range_tick(sq) == -1);
        sq.hook = false;  // no land squad container from the host: the member alone
        sq.tp = Vec3{0.0f, 0.0f, 0.0f};
        sq.path = at(-104.0f, 0.0f);
        CHECK(bribe_range_tick(sq) == -1);
        sq.hook = true;
    }
    {  // retail run s78m t06 trial 2 (S78M_T06_BRIBE_RUN Q6): T and X at (4010, 1200), Tyber on y = 1200 at centre
       // distances 113.5, 111.9, 110.4, 108.8: converted at 108.8 (the T test); the X test alone (103) not yet
        sq.xp = sq.tp = Vec3{4010.0f, 1200.0f, 0.0f};
        sq.mate = Vec3{4030.0f, 1210.0f, 0.0f};
        for (float d : {113.5f, 111.9f, 110.4f, 108.8f, 107.2f, 105.7f, 104.1f, 102.5f})
            sq.path.push_back(Vec3{4010.0f - d, 1200.0f, 0.0f});
        sq.path.erase(sq.path.begin());  // drop the earlier single position
        CHECK(bribe_range_tick(sq) == 3);
        sq.hook = false;
        CHECK(bribe_range_tick(sq) == 7);
        sq.hook = true;
    }
    {  // Activation_Min_Range 20: too close only when both T (d <= 20, [port] E 0) and X (d <= 20 + 3) are
        BribeRangeCase m;
        m.squad = true;
        m.hook = true;
        m.min_range = 20.0f;
        m.tp = Vec3{10.0f, 0.0f, 0.0f};
        m.path = at(-21.0f, 0.0f);  // X 21 (too close), T 31 (not): converted
        CHECK(bribe_range_tick(m) == 0);
        m.tp = Vec3{0.0f, 0.0f, 0.0f};
        m.path = at(-19.0f, 0.0f);  // both too close: refused, the order stays
        CHECK(bribe_range_tick(m) == -1);
        m.squad = m.hook = false;  // a single unit: d <= 20 + E
        m.path = at(-23.0f, 0.0f);
        CHECK(bribe_range_tick(m) == -1);
        m.path = at(-23.5f, 0.0f);
        CHECK(bribe_range_tick(m) == 0);
    }
    {  // Activation_Min_Range 0: the user's Targeting_Min_Attack_Distance; a member is retested against the live member
       // of its container nearest the user
        BribeRangeCase u;
        u.user_min = 30.0f;
        u.path = at(-33.0f, 0.0f);
        CHECK(bribe_range_tick(u) == -1);
        u.path = at(-33.5f, 0.0f);
        CHECK(bribe_range_tick(u) == 0);
        u.squad = true;
        u.xp = Vec3{50.0f, 0.0f, 0.0f};
        u.mate = Vec3{-10.0f, 0.0f, 0.0f};
        u.path = at(-30.0f, 0.0f);  // X at 80 (not too close), the nearest member at 20 (too close): refused
        CHECK(bribe_range_tick(u) == -1);
        u.mate = Vec3{0.0f, 300.0f, 0.0f};
        CHECK(bribe_range_tick(u) == 0);
    }
    {  // s78n t01 (§5.4.2 step 1): a melee user tests the container T against T's live member nearest it (+ that
       // member's footprint, no +10). T and X at the origin, the other member 60 closer to Tyber.
        BribeRangeCase me;
        me.squad = true;
        me.hook = true;
        me.mate = Vec3{-60.0f, 0.0f, 0.0f};
        me.path = at(-162.0f, 0.0f);  // the near member at 102 <= 103; T 162 > 110 and X 162 > 103
        CHECK(bribe_range_tick(me) == -1);
        me.melee = true;
        CHECK(bribe_range_tick(me) == 0);
        me.path = at(-163.5f, 0.0f);
        CHECK(bribe_range_tick(me) == -1);
    }
}

// s78n t07 / t01 (TACTICAL_COMBAT §5.4.2): Hack and Repair use the shared ability range (AbilitySystem::in_range). The
// user (radius 8: not in the formula) stands at `user`; X (id 2, Custom_Hard X / Y extents, facing +x; an enemy for Hack,
// own for Repair) at `xp`. With `grenade` the user's weapon is a grenade (v 10, G 0.56: reach v^2 / G = 178.57 on flat
// ground). Returns whether the apply ran on the first tick.
struct HackRangeCase {
    SpecialClass cls = SpecialClass::Hack;
    Vec3 user{0, 0, 0}, xp{0, 0, 0};
    float max_range = 100.0f, min_range = 0.0f, user_min = 0.0f, ex = 4.0f, ey = 2.0f;
    bool grenade = false;
    bool obstacle = false;  // s78n t02: X has LAND_OBSTACLE
    // s78n t11: X's Custom_Hard_YExtent_Deployed x Scale_Factor, whether its type Deploys, and its combat flags
    float dep_y = 0.0f;
    bool deploys = false;
    uint32_t x_flags = 0;
};
inline bool hack_range_applies(const HackRangeCase& c) {
    OiWorld w;
    const bool repair = c.cls == SpecialClass::Repair;
    const char* nm = repair ? "Repair" : "Hack";
    const uint8_t type = repair ? kTargetedRepair : kTargetedHack;
    SpecialAbilityDesc hk = oi_special(nm, c.cls, ActivationStyle::UserInput);
    hk.max_range = c.max_range;
    hk.min_range = c.min_range;
    const uint32_t hs = add_special(w.ad, hk);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(type, 0.0f, 10.0f, nm)};
    a.specials = {hs};
    UnitDesc u = oi_unit("R2", 100.0f, 8.0f);
    u.max_attack_distance = 90.0f;  // not melee
    u.min_attack_distance = c.user_min;
    if (c.grenade) {  // Proj_Merc_Concussion_Grenade numbers (LOB_SOLVER §1.2)
        ProjectileDesc g;
        g.name = "Grenade";
        g.lobbed = true;
        g.max_speed = 10.0f;
        g.grenade_gravity = 1.0f;
        g.lob_mod = 0.56f;
        g.can_lob_slower = true;
        g.damage = 0.0f;
        w.cd.projectiles = {g};
        u.weapon_projectile = 0;
        u.max_attack_distance = 240.0f;
    }
    const uint32_t ud = push(w.cd, w.ad, u, a);
    UnitDesc xu = oi_unit("Turret", 100.0f, 5.0f, 16);  // infantry
    xu.hard_x_extent = c.ex;
    xu.hard_y_extent = c.ey;
    xu.hard_y_extent_deployed = c.dep_y;
    xu.deploys = c.deploys;
    xu.land_obstacle = c.obstacle;
    const uint32_t xd = push(w.cd, w.ad, xu, AbilityObjectDesc{});
    CombatConfig cfg;
    cfg.short_range_cutoff = 51.0f;
    w.make(GameMode::Land, std::vector<PlayerInfo>(3), cfg);
    w.add(1, ud, 0, c.user);
    w.add(2, xd, repair ? 0 : 1, c.xp, {1, 0, 0});
    if (c.x_flags) w.sys->set_flag(2, c.x_flags, true);
    const ObjectId one[] = {1};
    (void)w.ab->targeted_ability(one, type, 2, -1, w.frame);
    w.tick();
    return w.ab->special_applied(1, name_crc(nm));
}

// s78n t11 review (TACTICAL_COMBAT §5.4.6 item 6, §5.4.2 slot 43): the ability range Target_In_Range reads the same
// extents - an MPTL-shaped target (X 33.0, Y 16.5, deployed Y 26.4) is reached from its flank at R + 26.4 while it is
// deploying (0x18) or deployed (0x19), at R + 16.5 otherwise (undeploying 0x1A, not deployed, a type that does not deploy).
inline void test_s78n_t11_ability_range() {
    struct Case {
        float y;
        uint32_t flags;
        bool deploys, in;
    };
    const uint32_t s18 = kUnitDeployingIn | kUnitDeployTransition, s19 = kUnitDeploying, s1a = kUnitDeployTransition;
    for (const Case k : {Case{125.0f, 0u, true, false}, Case{125.0f, s18, true, true}, Case{125.0f, s19, true, true},
                         Case{125.0f, s1a, true, false}, Case{125.0f, s19, false, false}, Case{116.0f, 0u, true, true},
                         Case{127.0f, s19, true, false}}) {
        HackRangeCase c;  // Activation_Max_Range 100, X at the origin facing +x
        c.ex = 33.0f;
        c.ey = 16.5f;
        c.dep_y = 26.4f;
        c.deploys = k.deploys;
        c.x_flags = k.flags;
        c.user = Vec3{0.0f, k.y, 0.0f};  // on X's flank: the Y extent
        CHECK(hack_range_applies(c) == k.in);
        c.user = Vec3{-132.5f, 0.0f, 0.0f};  // along X's facing: the X extent 33.0 in every state
        CHECK(hack_range_applies(c));
        c.user = Vec3{-133.5f, 0.0f, 0.0f};
        CHECK(!hack_range_applies(c));
    }
}

inline void test_hack_repair_range() {
    for (SpecialClass cls : {SpecialClass::Hack, SpecialClass::Repair}) {
        HackRangeCase c;
        c.cls = cls;
        // R + the X extent along X's facing, R + the Y extent across it (XY, non-strict)
        c.user = Vec3{-104.0f, 0.0f, 0.0f};
        CHECK(hack_range_applies(c));
        c.user = Vec3{-104.5f, 0.0f, 0.0f};
        CHECK(!hack_range_applies(c));
        c.user = Vec3{0.0f, 102.0f, 300.0f};  // z is ignored
        CHECK(hack_range_applies(c));
        c.user = Vec3{0.0f, 102.5f, 0.0f};
        CHECK(!hack_range_applies(c));
        // Activation_Min_Range 20: too close <=> d <= 20 + E
        c.min_range = 20.0f;
        c.user = Vec3{-24.0f, 0.0f, 0.0f};
        CHECK(!hack_range_applies(c));
        c.user = Vec3{-24.5f, 0.0f, 0.0f};
        CHECK(hack_range_applies(c));
        // Activation_Min_Range 0: the user's Targeting_Min_Attack_Distance
        c.min_range = 0.0f;
        c.user_min = 30.0f;
        c.user = Vec3{-34.0f, 0.0f, 0.0f};
        CHECK(!hack_range_applies(c));
        c.user = Vec3{-34.5f, 0.0f, 0.0f};
        CHECK(hack_range_applies(c));
    }
    // s78n t01 (§5.4.2 slot 44, LOB_SOLVER §3.1): a grenade-weapon user: the lob solve alone, the ability range unused
    HackRangeCase g;
    g.grenade = true;
    g.user = Vec3{-170.0f, 0.0f, 0.0f};  // 170 > 100 + 4, but an arc exists
    CHECK(hack_range_applies(g));
    g.user = Vec3{-185.0f, 0.0f, 0.0f};  // beyond v^2 / G even with Activation_Max_Range 1000
    g.max_range = 1000.0f;
    CHECK(!hack_range_applies(g));
    g.user = Vec3{-170.0f, 0.0f, 0.0f};
    g.min_range = 175.0f;  // slot 46 is unchanged: too close <=> d <= 175 + 4
    CHECK(!hack_range_applies(g));
}

// s78n t07: the per-player writers Base_Power (TACTICAL_GAPS §7.3, 0x39a2c0), Weatherproof (§7.4), Enable_Ability (§2
// table [M]) and Battlefield_Modifier (CU8F_T04_T06_RE §3.3, 0x6f1130) take the owner and the players allied with it by
// the relation-0 test (the host's allied_players): a neutral player (relation 2: not an enemy, not allied) gets nothing.
// Cancel_Special (0x6f4060) accepts a neutral target. Without the hook the fallback "not an enemy" counts the neutral.
// s78n t02 (TACTICAL_COMBAT §5.4.4 item 1): the too-close half of the ability range is slot 46 in full - E is the X
// half-extent, or the Y one only when U's unnormalised offset along X's facing is <= 0.7071; a LAND_OBSTACLE target has
// no E; the weapon-min-range retest picks the member nearest in 3-D; Tactical_Bribe's container T is too close when
// dt <= r + E_T, E_T the members' joint box (0x404450).
inline void test_s78n_min_range() {
    {  // the joint box: starts at the container's own point, half the span + MaxJiggleDistance
        float hx = -1.0f, hy = -1.0f;
        squad_joint_half_extents({}, 2.0f, hx, hy);
        CHECK(hx == 2.0f && hy == 2.0f);
        const SquadSlot one[] = {SquadSlot{5.0f, 0.0f, 1.0f, 1.0f}};  // x [0, 6] (the origin is in the box), y [-1, 1]
        squad_joint_half_extents(one, 0.5f, hx, hy);
        CHECK(hx == 3.5f && hy == 1.5f);
        const SquadSlot two[] = {SquadSlot{-6.0f, -2.0f, 3.0f, 3.0f}, SquadSlot{6.0f, 4.0f, 3.0f, 2.0f}};
        squad_joint_half_extents(two, 0.0f, hx, hy);  // x [-9, 9], y [-5, 6]
        CHECK(hx == 9.0f && hy == 5.5f);
    }
    for (SpecialClass cls : {SpecialClass::Hack, SpecialClass::Repair}) {
        HackRangeCase c;  // X at the origin facing +x, extents X 4 / Y 2; Activation_Min_Range 20
        c.cls = cls;
        c.min_range = 20.0f;
        c.user = Vec3{-5.0f, 22.0f, 0.0f};  // D 22.56; offset along the facing 5 > 0.7071 -> E = 4: too close
        CHECK(!hack_range_applies(c));      // (slot 43's normalised test, 0.22, would have picked Y: 22.56 > 22)
        c.user = Vec3{-0.5f, 22.5f, 0.0f};  // offset 0.5 -> E = 2: 22.5056 > 22
        CHECK(hack_range_applies(c));
        c.obstacle = true;  // LAND_OBSTACLE: D <= r on the centres, no E
        c.user = Vec3{-22.0f, 0.0f, 0.0f};
        CHECK(hack_range_applies(c));  // 22 > 20 (before: 22 <= 20 + 4)
        c.user = Vec3{-20.0f, 0.0f, 0.0f};
        CHECK(!hack_range_applies(c));
    }
    {  // the weapon min range's nearest member is the nearest in 3-D (0x404fc0): a mate 20 away in XY but 500 above is
       // farther than X (80): X is tested on its own and is not too close
        BribeRangeCase u;
        u.user_min = 30.0f;
        u.squad = true;
        u.xp = Vec3{50.0f, 0.0f, 0.0f};
        u.mate = Vec3{-10.0f, 0.0f, 500.0f};
        u.path = std::vector<Vec3>{Vec3{-30.0f, 0.0f, 0.0f}};
        CHECK(bribe_range_tick(u) == 0);
        u.mate = Vec3{-10.0f, 0.0f, 0.0f};  // in the plane it is the nearest (20 <= 33): refused
        CHECK(bribe_range_tick(u) == -1);
    }
    {  // T's too-close term: X too close (5 <= 20 + 3) in each case, so T decides; T at the origin facing +x
        BribeRangeCase m;
        m.squad = true;
        m.hook = true;
        m.min_range = 20.0f;
        m.xp = Vec3{-20.0f, 0.0f, 0.0f};
        m.path = std::vector<Vec3>{Vec3{-25.0f, 0.0f, 0.0f}};
        CHECK(bribe_range_tick(m) == 0);  // [port] no slot layout from the host: E_T = 0, 25 > 20
        m.slots = true;                   // along T's facing: E_T = the X half-extent 6 + 3 + 1 = 10
        CHECK(bribe_range_tick(m) == -1);  // 25 <= 30: both too close, refused
        m.path = std::vector<Vec3>{Vec3{-30.5f, 0.0f, 0.0f}};
        CHECK(bribe_range_tick(m) == 0);  // 30.5 > 30
        m.xp = Vec3{0.0f, -20.0f, 0.0f};  // across T's facing (offset 0): E_T = the Y half-extent 3 + 1 = 4
        m.path = std::vector<Vec3>{Vec3{0.0f, -25.0f, 0.0f}};
        CHECK(bribe_range_tick(m) == 0);  // 25 > 24
        m.path = std::vector<Vec3>{Vec3{0.0f, -24.0f, 0.0f}};
        CHECK(bribe_range_tick(m) == -1);
    }
}

// s78n t04 (TACTICAL_COMBAT §5.4.4 items 3a - 3c): a Combat_Bonus source does not buff itself (unless its type has
// Apply_Special_Ability_To_Self_On_Creation); Specific_Faction replaces the ally test by "the target owner's faction is
// that faction" (an unknown name: unset, with the validate's warning); Enhance_Defense appends one permanent list-0
// entry per source to the owner's (or every ally's) player set, read by every unit of that player, never removed.
inline void test_s78n_combat_bonus_and_enhance_defense() {
    {  // the source itself
        for (const bool self : {false, true}) {
            OiWorld w;
            SpecialAbilityDesc cb = oi_special("CB", SpecialClass::CombatBonus, ActivationStyle::SpaceAutomatic);
            cb.bonus[static_cast<size_t>(ModList::Damage)] = 0.25f;
            const uint32_t cs = add_special(w.ad, cb);
            AbilityObjectDesc hero;
            hero.specials = {cs};
            hero.apply_self_on_creation = self;
            const uint32_t hd = push(w.cd, w.ad, oi_unit("Hero", 100.0f), hero);
            const uint32_t sd = push(w.cd, w.ad, oi_unit("Ship", 100.0f), AbilityObjectDesc{});
            w.make();
            w.add(1, sd, 0, {0, 0, 0});
            w.add(2, hd, 0, {100, 0, 0});
            CHECK_EQ(w.sys->modifier(1, ModList::Damage), 0.25f);
            CHECK_EQ(w.sys->modifier(2, ModList::Damage), self ? 0.25f : 0.0f);
        }
    }
    {  // Specific_Faction: factions Empire 0 / Rebel 1; players 0 Empire (the source), 1 Rebel (enemy), 2 Rebel (enemy)
        std::vector<std::string> warnings;
        alamo::log::set_sink([&warnings](alamo::log::Level l, std::string_view, std::string_view msg) {
            if (l == alamo::log::Level::Warn) warnings.emplace_back(msg);
        });
        auto run = [](const char* faction, std::array<float, 3>& out) {
            OiWorld w;
            SpecialAbilityDesc cb = oi_special("FactionBonus", SpecialClass::CombatBonus, ActivationStyle::SpaceAutomatic);
            cb.bonus[static_cast<size_t>(ModList::Damage)] = 0.25f;
            cb.specific_faction = faction;
            const uint32_t cs = add_special(w.ad, cb);
            AbilityObjectDesc prop;
            prop.specials = {cs};
            const uint32_t pd = push(w.cd, w.ad, oi_unit("Prop", 100.0f), prop);
            const uint32_t sd = push(w.cd, w.ad, oi_unit("Ship", 100.0f), AbilityObjectDesc{});
            w.make();
            AbilityHost h;
            h.faction_index = [](std::string_view n) -> int32_t {
                if (n == "Empire") return 0;
                if (n == "Rebel") return 1;
                return -1;
            };
            h.player_faction = [](int32_t p) -> int32_t { return p == 0 ? 0 : 1; };
            h.allied_players = [](int32_t a, int32_t b) { return a == b; };
            w.ab->set_host(std::move(h));
            for (int32_t p = 0; p < 3; ++p) w.add(1 + p, sd, p, {100.0f * static_cast<float>(p), 0, 0});
            w.add(10, pd, 0, {0, 500, 0});
            for (size_t i = 0; i < 3; ++i) out[i] = w.sys->modifier(static_cast<ObjectId>(1 + i), ModList::Damage);
        };
        std::array<float, 3> r{};
        run("Rebel", r);  // the faction test alone: the Rebel players' units, not the source owner's own
        CHECK(r[0] == 0.0f && r[1] == 0.25f && r[2] == 0.25f);
        CHECK(warnings.empty());
        run("Hutts", r);  // unknown: treated as unset (the ally test), with the validate's error line
        CHECK(r[0] == 0.25f && r[1] == 0.0f && r[2] == 0.0f);
        CHECK(warnings.size() == 1 && warnings[0].find("Unrecognized faction given for Specific_Faction") != std::string::npos &&
              warnings[0].find("FactionBonus") != std::string::npos);
        alamo::log::set_sink(nullptr);
    }
    // Enhance_Defense: players 0 the owner, 1 enemy (the defender), 2 ally, 3 neutral
    struct EdCase {
        bool all_allies = false;
        int32_t owner = 0;
        float value = 0.25f;
    };
    auto ed_world = [](OiWorld& w, const EdCase& c, uint32_t& cd, uint32_t& sd) {
        SpecialAbilityDesc ed = oi_special("ED", SpecialClass::EnhanceDefense, ActivationStyle::GroundAutomatic);
        ed.bonus[static_cast<size_t>(ModList::Defense)] = c.value;
        ed.stacking_category = 0;
        ed.apply_to_all_allies = c.all_allies;
        const uint32_t es = add_special(w.ad, ed);
        AbilityObjectDesc cmd;
        cmd.specials = {es};
        cd = push(w.cd, w.ad, oi_unit("Field_Commander", 100.0f, 5.0f, 16), cmd);
        sd = push(w.cd, w.ad, oi_unit("Tank", 100.0f, 5.0f, 2), AbilityObjectDesc{});
        w.make(GameMode::Land, std::vector<PlayerInfo>(4));
        AbilityHost h;
        h.allied_players = [](int32_t a, int32_t b) { return a == b || (a == 0 && b == 2) || (a == 2 && b == 0); };
        h.defense_player = [](int32_t owner) { return owner == 3 ? 1 : owner; };  // neutral -> the defender
        w.ab->set_host(std::move(h));
        for (int32_t p : {2, 3}) {
            w.sys->set_enemy(0, p, false);
            w.sys->set_enemy(p, 0, false);
        }
    };
    auto entries = [](OiWorld& w, int32_t p) { return w.sys->player_modifiers(p).entries().size(); };
    {
        OiWorld w;
        uint32_t cd = 0, sd = 0;
        ed_world(w, EdCase{}, cd, sd);
        w.add(1, sd, 0, {0, 0, 0});    // own, before the source
        w.add(2, cd, 0, {50, 0, 0});   // the source (Ground_Automatic: applied at its creation)
        w.add(3, sd, 2, {100, 0, 0});  // ally: not without Apply_To_All_Allies
        w.add(4, sd, 0, {150, 0, 0});  // own, after the source
        CHECK(entries(w, 0) == 1 && entries(w, 1) == 0 && entries(w, 2) == 0 && entries(w, 3) == 0);
        const ModifierEntry& e = w.sys->player_modifiers(0).entries()[0];
        CHECK(e.list == static_cast<uint8_t>(ModList::Defense) && e.category == 0 && e.value == 0.25f &&
              e.expiry == kPermanent && e.source_id == 2);
        CHECK(w.sys->unit_modifiers(1)->entries().empty() && w.sys->unit_modifiers(4)->entries().empty());  // no unit aura
        CHECK_EQ(w.sys->modifier(1, ModList::Defense), 0.25f);
        CHECK_EQ(w.sys->modifier(2, ModList::Defense), 0.25f);  // every object of the player, the source included
        CHECK_EQ(w.sys->modifier(4, ModList::Defense), 0.25f);
        CHECK_EQ(w.sys->modifier(3, ModList::Defense), 0.0f);
        w.add(5, cd, 0, {200, 0, 0});  // a second source appends another entry; one category counts once
        CHECK(entries(w, 0) == 2);
        CHECK_EQ(w.sys->modifier(1, ModList::Defense), 0.25f);
        w.sys->unit_modifiers(1)->insert(ModList::Defense, 0, 0.3f, kPermanent);  // same category: the larger
        CHECK_EQ(w.sys->modifier(1, ModList::Defense), 0.3f);
        w.sys->unit_modifiers(1)->insert(ModList::Defense, 1, 0.6f, kPermanent);  // another category adds, capped
        CHECK_EQ(w.sys->modifier(1, ModList::Defense), 0.75f);
        // never removed: not at the source's death, not at its removal
        w.sys->kill_unit(2, kNoObject, kKindNormal, w.frame);
        w.ab->remove(5);
        CHECK(entries(w, 0) == 2);
        CHECK_EQ(w.sys->modifier(4, ModList::Defense), 0.25f);
        // it follows ownership: a unit the player loses drops it, a unit it gains gets it
        w.sys->set_owner(4, 1);
        CHECK_EQ(w.sys->modifier(4, ModList::Defense), 0.0f);
        w.sys->set_owner(3, 0);
        CHECK_EQ(w.sys->modifier(3, ModList::Defense), 0.25f);
    }
    {  // Apply_To_All_Allies: every player with relation 0 to the owner (own included), not the neutral or the enemy
        OiWorld w;
        uint32_t cd = 0, sd = 0;
        EdCase c;
        c.all_allies = true;
        ed_world(w, c, cd, sd);
        w.add(1, cd, 0, {0, 0, 0});
        CHECK(entries(w, 0) == 1 && entries(w, 1) == 0 && entries(w, 2) == 1 && entries(w, 3) == 0);
    }
    {  // a neutral owner stands for the battle's defender (the host's defense_player); a negative value is 0
        OiWorld w;
        uint32_t cd = 0, sd = 0;
        EdCase c;
        c.owner = 3;
        c.value = -0.5f;
        ed_world(w, c, cd, sd);
        w.add(1, cd, 3, {0, 0, 0});
        CHECK(entries(w, 0) == 0 && entries(w, 1) == 1 && entries(w, 3) == 0);
        CHECK(w.sys->player_modifiers(1).entries()[0].value == 0.0f);
    }
}

inline void test_s78n_allied_users() {
    struct R {
        std::array<bool, 4> power{}, weather{}, enabled{};
        std::array<uint32_t, 4> gate{};
        std::array<float, 4> stealth{};
        uint32_t cancel_neutral = 0;
    };
    auto run = [](bool hook) {
        OiWorld w;
        SpecialAbilityDesc tgt = oi_special("Target", SpecialClass::LaserDefense, ActivationStyle::UserInput);
        tgt.initially_enabled = false;
        const uint32_t ts = add_special(w.ad, tgt);
        const uint32_t ps = add_special(w.ad, oi_special("Power", SpecialClass::BasePower, ActivationStyle::GroundAutomatic));
        SpecialAbilityDesc wp = oi_special("Weatherproof", SpecialClass::Weatherproof, ActivationStyle::GroundAutomatic);
        wp.apply_to_all_allies = true;
        const uint32_t ws = add_special(w.ad, wp);
        SpecialAbilityDesc en = oi_special("Enable", SpecialClass::EnableAbility, ActivationStyle::GroundAutomatic);
        en.ability_name_crc = name_crc("Target");
        en.apply_to_all_allies = true;
        const uint32_t es = add_special(w.ad, en);
        SpecialAbilityDesc bm = oi_special("BM", SpecialClass::BattlefieldModifier, ActivationStyle::GroundAutomatic);
        bm.enable_bombing_runs = true;
        bm.stealth_duration_multiplier = 1.25f;
        bm.apply_to_all_allies = true;
        const uint32_t bs = add_special(w.ad, bm);
        const uint32_t cs = add_special(w.ad, oi_special("Cancel", SpecialClass::CancelSpecial, ActivationStyle::UserInput));
        AbilityObjectDesc src, holder, canceller;
        src.specials = {ps, ws, es, bs};
        holder.specials = {ts};
        canceller.unit_abilities = {oi_gui(kTargetedHack, 0.0f, 0.0f, "Cancel")};
        canceller.specials = {cs};
        const uint32_t sd = push(w.cd, w.ad, oi_unit("Source", 100.0f), src);
        const uint32_t hd = push(w.cd, w.ad, oi_unit("Holder", 100.0f), holder);
        const uint32_t cd = push(w.cd, w.ad, oi_unit("Canceller", 100.0f), canceller);
        // players: 0 the source, 1 enemy, 2 ally, 3 neutral (not an enemy in the combat table, relation 2)
        w.make(GameMode::Land, std::vector<PlayerInfo>(4));
        if (hook) {
            AbilityHost h;
            h.allied_players = [](int32_t a, int32_t b) { return a == b || (a == 0 && b == 2) || (a == 2 && b == 0); };
            w.ab->set_host(std::move(h));
        }
        for (int32_t p : {2, 3}) {
            w.sys->set_enemy(0, p, false);
            w.sys->set_enemy(p, 0, false);
        }
        w.add(1, sd, 0, {0, 0, 0});
        w.add(2, cd, 0, {0, -100.0f, 0});
        for (int32_t p = 0; p < 4; ++p) w.add(10 + p, hd, p, {100.0f * static_cast<float>(p), 500.0f, 0.0f});
        R r;
        for (int32_t p = 0; p < 4; ++p) {
            const auto i = static_cast<size_t>(p);
            r.power[i] = w.sys->has_base_power(p);
            r.weather[i] = w.ab->weather_affected(p);
            r.enabled[i] = w.ab->special_enabled(10 + p, name_crc("Target"));
            bool all = false;
            r.gate[i] = w.ab->player_gate(p, PlayerGate::BombingRuns, all);
            r.stealth[i] = w.ab->player_list(p, PlayerList::StealthDuration);
        }
        const ObjectId users[] = {2};
        r.cancel_neutral = w.ab->targeted_ability(users, kTargetedHack, 13, -1, w.frame);
        return r;
    };
    const R h = run(true);
    CHECK(h.power[0] && !h.power[1] && h.power[2] && !h.power[3]);
    CHECK(!h.weather[0] && h.weather[1] && !h.weather[2] && h.weather[3]);
    CHECK(h.enabled[0] && !h.enabled[1] && h.enabled[2] && !h.enabled[3]);
    CHECK(h.gate[0] == 1 && h.gate[1] == 0 && h.gate[2] == 1 && h.gate[3] == 0);
    CHECK(h.stealth[0] == 0.25f && h.stealth[1] == 0.0f && h.stealth[2] == 0.25f && h.stealth[3] == 0.0f);
    CHECK(h.cancel_neutral == 1);
    const R f = run(false);
    CHECK(f.power[3] && !f.weather[3] && f.enabled[3] && f.gate[3] == 1 && f.stealth[3] == 0.25f);
    CHECK(f.cancel_neutral == 0);
}

// ================================================================================================
// cu8f t06 (CU8F_T04_T06_RE §3): the parsed-unread ability fields
// ================================================================================================

inline void test_cu8f_parse_defaults() {
    // Super_Laser ctor defaults Turn_Rate 10 / Firing_Cone 45; an absent Max_Num_Spawned_Objects is -1 (no cap, [port]);
    // Is_Pulsing / Target_Types / Particle_Effect parsed; Enable_* flags parsed.
    const data::XmlDocument doc = data::XmlDocument::parse(
        "<R>"
        "<Unit_Ability><Type>LURE</Type><Is_Pulsing>Yes</Is_Pulsing><Pulse_Frequency_Secs>0.5</Pulse_Frequency_Secs>"
        "<Max_Number_Of_Pulses>40</Max_Number_Of_Pulses><Effective_Radius>350</Effective_Radius>"
        "<Target_Types>T_Fighter, T_Bomber</Target_Types><Particle_Effect>Lure_FX</Particle_Effect></Unit_Ability>"
        "<Unit_Ability><Type>DEPLOY_TROOPERS</Type><Max_Num_Spawned_Objects>2</Max_Num_Spawned_Objects></Unit_Ability>"
        "<Unit_Ability><Type>DEPLOY_TROOPERS</Type></Unit_Ability>"
        "<Super_Laser_Ability Name=\"SL\"><Activation_Style>User_Input</Activation_Style><Face_Target>Yes</Face_Target>"
        "<Fire_Continuation>1.5</Fire_Continuation></Super_Laser_Ability>"
        "<Super_Laser_Ability Name=\"SL2\"><Turn_Rate>0</Turn_Rate><Firing_Cone>90</Firing_Cone></Super_Laser_Ability>"
        "<Battlefield_Modifier_Ability Name=\"BM\"><Enable_Bombing_Runs>Yes</Enable_Bombing_Runs>"
        "<Enable_Special_Weapons>Yes</Enable_Special_Weapons><Enable_Planetary_Bombardment>Yes</Enable_Planetary_Bombardment>"
        "<Reverse_Application_Logic>Yes</Reverse_Application_Logic></Battlefield_Modifier_Ability>"
        "</R>",
        "cu8f.xml");
    AbilityResolvers r;
    r.object_type = [](std::string_view n) { return iequals(n, "T_Fighter") ? 7 : iequals(n, "T_Bomber") ? 9 : -1; };
    std::vector<data::XmlNode> e;
    for (data::XmlNode c = doc.root().first_child(); c; c = c.next_sibling()) e.push_back(c);
    CHECK(e.size() == 6);
    if (e.size() != 6) return;
    UnitAbilityRecord lure, cap, nocap;
    CHECK(parse_unit_ability(e[0], lure, r) && parse_unit_ability(e[1], cap, r) && parse_unit_ability(e[2], nocap, r));
    CHECK(lure.type == kLure && lure.is_pulsing && lure.pulse_frequency_secs == 0.5f && lure.max_pulses == 40);
    CHECK(lure.target_types == std::vector<int32_t>({7, 9}) && lure.particle_effect == "Lure_FX" && lure.effective_radius == 350.0f);
    CHECK(cap.max_spawned_objects == 2 && nocap.max_spawned_objects == -1);
    const SpecialAbilityDesc sl = parse_special_ability(e[3], r, nullptr);
    CHECK(sl.cls == SpecialClass::SuperLaser && sl.face_target && sl.turn_rate == 10.0f && sl.firing_cone == 45.0f &&
          sl.fire_continuation == 1.5f);
    const SpecialAbilityDesc sl2 = parse_special_ability(e[4], r, nullptr);
    CHECK(sl2.turn_rate == 0.0f && sl2.firing_cone == 90.0f && !sl2.face_target);
    const SpecialAbilityDesc bm = parse_special_ability(e[5], r, nullptr);
    CHECK(bm.cls == SpecialClass::BattlefieldModifier && bm.enable_bombing_runs && bm.enable_special_weapons &&
          bm.enable_bombardment && bm.reverse_logic);
}

inline void test_cu8f_lure() {
    // §3.1: LURE every frame while on; activation resets the count and makes the next service pulse; a pulse every
    // int(FPS x Pulse_Frequency_Secs) frames up to Max_Number_Of_Pulses, then it suspends. Each pulse: per Target_Types
    // entry, the objects of exactly that type within the 3-D Effective_Radius, ascending id; a team member stands for its
    // container; enemies only; each is ordered to attack the lure plus a move toward it (Lured). Is_Pulsing is not read.
    OiWorld w;
    AbilityObjectDesc la;
    UnitAbilityRecord lr = rec(kLure, 16.0f, 0.0f);
    lr.effective_radius = 350.0f;
    lr.pulse_frequency_secs = 0.5f;
    lr.max_pulses = 3;
    lr.target_types = {7, 9};
    lr.particle_effect = "Lure_FX";
    lr.is_pulsing = false;  // unread: the lure pulses all the same
    la.unit_abilities = {lr};
    UnitDesc lure = oi_unit("AWing", 100.0f);
    lure.object_type = 5;
    const uint32_t ld = push(w.cd, w.ad, lure, la);
    UnitDesc t7 = oi_unit("Fighter7", 100.0f);
    t7.object_type = 7;
    UnitDesc t8 = t7;
    t8.name = "Other8";
    t8.object_type = 8;
    UnitDesc t9 = t7;
    t9.name = "Bomber9";
    t9.object_type = 9;
    const uint32_t d7 = push(w.cd, w.ad, t7, AbilityObjectDesc{});
    const uint32_t d8 = push(w.cd, w.ad, t8, AbilityObjectDesc{});
    const uint32_t d9 = push(w.cd, w.ad, t9, AbilityObjectDesc{});
    AbilityObjectDesc sq;
    sq.name = "Bomber_Squad";
    const uint32_t sqd = push_container(w.cd, w.ad, sq);
    w.make();
    w.sys->set_enemy(0, 2, false);  // player 2 is allied to 0
    w.sys->set_enemy(2, 0, false);
    w.add(1, ld, 0, {0, 0, 0});
    w.add(2, d7, 1, {100, 0, 0});    // type 7, enemy, in range
    w.add(3, d9, 1, {0, 200, 100});  // type 9, 3-D 224
    w.add(4, d7, 1, {300, 0, 200});  // 3-D 361 > 350 (XY 300): out
    w.add(5, d8, 1, {50, 0, 0});     // not a target type
    w.add(6, d7, 0, {60, 0, 0});     // own
    w.add(7, d7, 2, {70, 0, 0});     // ally
    w.add(21, d9, 1, {150, 0, 0}, {1, 0, 0}, 25);
    w.add(22, d9, 1, {150, 20, 0}, {1, 0, 0}, 25);
    const ObjectId mem[] = {21, 22};
    CHECK(w.ab->add_container(25, sqd, 1, mem, w.frame, &w.rng));
    const ObjectId one[] = {1};
    const uint32_t f0 = w.frame;
    CHECK(w.ab->set_unit_ability_mode(one, kLure, true, w.frame) == 1 && w.ab->lure_running(1));
    CHECK(w.ab->lure_pulses(1) == 0 && w.ab->lure_next(1) == f0);
    // Particle_Effect: one object, the owner is in no team.
    const std::span<const SpawnRequest> q = w.ab->spawn_requests();
    CHECK(q.size() == 1);
    if (q.size() == 1) {
        CHECK(q[0].purpose == SpawnPurpose::LureEffect && q[0].source == 1 && q[0].owner == 0);
        CHECK(w.ab->bind_spawn(q[0].serial, 90));
    }
    CHECK(w.ab->lure_effect(1) == 90);
    w.collect();
    w.tick();  // f0: the first pulse
    CHECK(w.ab->lure_pulses(1) == 1 && w.ab->lure_next(1) == f0 + 15);
    CHECK(w.count(AbilityEventKind::Lured, 2) == 1 && w.count(AbilityEventKind::Lured, 3) == 1);
    CHECK(w.count(AbilityEventKind::Lured, 21) == 1 && w.count(AbilityEventKind::Lured, 22) == 1);  // the team, member by member
    for (ObjectId id : {4, 5, 6, 7, 25}) CHECK(w.count(AbilityEventKind::Lured, id) == 0);
    if (const AbilityEvent* e = w.last(AbilityEventKind::Lured, 2)) CHECK(e->target == 1 && e->point.x == 0.0f && e->type == kLure);
    CHECK(w.sys->current_target(21) == 1);  // the team took the ordered attack on the lure
    // Save round trip mid-run.
    {
        const std::vector<uint8_t> a = ability_bytes(*w.ab);
        const uint64_t dg = battle_digest(*w.sys, *w.ab, w.rng);
        ByteReader rd(a);
        CHECK(w.ab->read(rd));
        CHECK(ability_bytes(*w.ab) == a && battle_digest(*w.sys, *w.ab, w.rng) == dg);
        CHECK(w.ab->lure_running(1) && w.ab->lure_pulses(1) == 1 && w.ab->lure_next(1) == f0 + 15 && w.ab->lure_effect(1) == 90);
    }
    while (w.frame < f0 + 15) w.tick();
    CHECK(w.ab->lure_pulses(1) == 1);
    w.tick();  // f0 + 15: the second pulse; the team already targets the lure (skipped), the singles are pulled again
    CHECK(w.ab->lure_pulses(1) == 2 && w.count(AbilityEventKind::Lured, 21) == 1);
    while (w.frame <= f0 + 30) w.tick();
    CHECK(w.ab->lure_pulses(1) == 3 && w.ab->lure_running(1));
    while (w.frame <= f0 + 45) w.tick();  // count == Max_Number_Of_Pulses: suspend
    CHECK(!w.ab->lure_running(1) && w.ab->lure_pulses(1) == 3);
    CHECK(w.ab->is_active(1, kLure));  // the ability slot runs on to Expiration_Seconds [M]
    const size_t lured = w.count(AbilityEventKind::Lured);
    for (int i = 0; i < 30; ++i) w.tick();
    CHECK(w.count(AbilityEventKind::Lured) == lured);
    // Off: the effect object goes (RemoveSilently).
    CHECK(w.ab->set_unit_ability_mode(one, kLure, false, w.frame) == 1);
    w.collect();
    CHECK(w.count(AbilityEventKind::RemoveSilently, 90) == 1 && w.ab->lure_effect(1) == kNoObject);
    // A lure team: only the first member spawns the effect; the member's death removes it.
    OiWorld t;
    AbilityObjectDesc ma;
    ma.unit_abilities = {lr};
    const uint32_t md = push(t.cd, t.ad, lure, ma);
    AbilityObjectDesc ls;
    ls.name = "Lure_Squad";
    ls.unit_abilities = {lr};
    const uint32_t lsd = push_container(t.cd, t.ad, ls);
    t.make();
    t.add(31, md, 0, {0, 0, 0}, {1, 0, 0}, 35);
    t.add(32, md, 0, {0, 20, 0}, {1, 0, 0}, 35);
    const ObjectId lm[] = {31, 32}, lsq[] = {35};
    CHECK(t.ab->add_container(35, lsd, 0, lm, t.frame, &t.rng));
    CHECK(t.ab->set_unit_ability_mode(lsq, kLure, true, t.frame) >= 1 && t.ab->lure_running(31) && t.ab->lure_running(32));
    size_t fx = 0;
    for (const SpawnRequest& s : t.ab->spawn_requests())
        if (s.purpose == SpawnPurpose::LureEffect) {
            ++fx;
            CHECK(s.source == 31);
            CHECK(t.ab->bind_spawn(s.serial, 95));
        }
    CHECK(fx == 1 && t.ab->lure_effect(31) == 95 && t.ab->lure_effect(32) == kNoObject);
    t.collect();
    t.sys->kill_unit(31, kNoObject, kKindNormal, t.frame);
    t.collect();
    CHECK(t.count(AbilityEventKind::RemoveSilently, 95) == 1 && !t.ab->lure_running(31));
    // cu8g t05: switched off (or its owner gone) before the host bound the effect: bind_spawn refuses it (the host then
    // removes the new object silently) and the lure keeps no effect.
    for (const bool kill : {false, true}) {
        OiWorld u;
        const uint32_t ud = push(u.cd, u.ad, lure, la);
        u.make();
        u.add(1, ud, 0, {0, 0, 0});
        CHECK(u.ab->set_unit_ability_mode(one, kLure, true, u.frame) == 1);
        if (kill) u.sys->kill_unit(1, kNoObject, kKindNormal, u.frame);
        else CHECK(u.ab->set_unit_ability_mode(one, kLure, false, u.frame) == 1);
        size_t n = 0;
        for (const SpawnRequest& s : u.ab->spawn_requests())
            if (s.purpose == SpawnPurpose::LureEffect) {
                ++n;
                CHECK(s.cancelled && !u.ab->bind_spawn(s.serial, 97));
            }
        CHECK(n == 1 && u.ab->lure_effect(1) == kNoObject && !u.ab->lure_running(1));
    }
}

inline void test_cu8l_stop_hold() {
    // cu8l t02 (CU8K_T02_T06_RE §2 rows 0x3a4138 / 0x3a41e6, rules 5 / 6): the behaviour toggle stops and holds the unit
    // (StopOrder kStopHold) on every LURE toggle and on HUNT off, not on HUNT on; turning a running LURE off holds it a
    // second time. MISSILE_SHIELD with Disable_Movement uses the coordinator-system stop (kStopLeave) and then the first
    // lock (Immobilised, which the host turns into the split / adopt).
    OiWorld w;
    AbilityObjectDesc a;
    UnitAbilityRecord lr = rec(kLure, 16.0f, 0.0f);
    lr.pulse_frequency_secs = 0.5f;
    lr.max_pulses = 3;
    UnitAbilityRecord ms = rec(kMissileShield, 10.0f, 0.0f);
    ms.disable_movement = true;
    a.unit_abilities = {lr, rec(kHunt, 0.0f, 0.0f), ms};
    const uint32_t d = push(w.cd, w.ad, oi_unit("Holder", 100.0f), a);
    w.make();
    w.add(1, d, 0, {0, 0, 0});
    const ObjectId one[] = {1};
    const auto stops = [&w](int32_t value) {
        size_t n = 0;
        for (const AbilityEvent& e : w.log)
            if (e.kind == AbilityEventKind::StopOrder && e.id == 1 && e.value == value) ++n;
        return n;
    };
    CHECK(w.ab->set_unit_ability_mode(one, kLure, true, w.frame) == 1 && w.ab->lure_running(1));
    w.collect();
    CHECK(stops(kStopHold) == 1);
    CHECK(w.ab->set_unit_ability_mode(one, kLure, false, w.frame) == 1 && !w.ab->lure_running(1));
    w.collect();
    CHECK(stops(kStopHold) == 3);  // off + the running behaviour's second hold
    CHECK(w.ab->set_unit_ability_mode(one, kHunt, true, w.frame) == 1);
    w.collect();
    CHECK(stops(kStopHold) == 3);  // HUNT on: none
    CHECK(w.ab->set_unit_ability_mode(one, kHunt, false, w.frame) == 1);
    w.collect();
    CHECK(stops(kStopHold) == 4);  // HUNT off: one
    CHECK(w.ab->set_unit_ability_mode(one, kMissileShield, true, w.frame) == 1);
    w.collect();
    CHECK(stops(kStopLeave) == 1 && stops(kStopHold) == 4 && w.count(AbilityEventKind::Immobilised, 1) == 1);
    if (const AbilityEvent* e = w.last(AbilityEventKind::StopOrder, 1)) CHECK(e->value == kStopLeave);
}

inline void test_cu8f_deploy_cap() {
    // §3.2: Max_Num_Spawned_Objects caps the live deployed squads per unit (Can_Activate 38 and the activation); a squad
    // that is gone frees its slot. One squad per deploy.
    OiWorld w;
    AbilityObjectDesc a;
    UnitAbilityRecord dt = rec(kDeployTroopers, 0.0f, 20.0f);
    dt.spawned_object_type = "Squad";
    dt.spawned_type = 7;
    dt.max_spawned_objects = 2;
    dt.owner_attachment_bones = {"B_0", "B_1"};
    a.unit_abilities = {dt};
    const uint32_t ud = push(w.cd, w.ad, oi_unit("AT_AT", 1000.0f), a);
    const uint32_t td = push(w.cd, w.ad, oi_unit("Trooper", 10.0f), AbilityObjectDesc{});
    w.make(GameMode::Land);
    AbilityHost h;
    h.company_members = [](int32_t type, std::vector<int32_t>& m) {
        if (type != 7) return false;
        m = {100, 101, 102, 103, 104, 105};
        return true;
    };
    h.bone_position = [](ObjectId, std::string_view bone, Vec3& p) {
        p = Vec3{bone == "B_0" ? 10.0f : 30.0f, 5.0f, 0.0f};
        return 1;
    };
    w.ab->set_host(std::move(h));
    w.add(1, ud, 0, {0, 0, 0});
    const ObjectId one[] = {1};
    ObjectId next_id = 50;
    auto deploy = [&]() {
        CHECK(w.ab->set_unit_ability_mode(one, kDeployTroopers, true, w.frame) == 1);
        std::vector<SpawnRequest> q(w.ab->spawn_requests().begin(), w.ab->spawn_requests().end());
        CHECK(q.size() == 2);
        CHECK(w.ab->trooper_squads(1) == static_cast<int32_t>(next_id == 50 ? 1 : 2));  // a pending squad counts
        for (const SpawnRequest& r : q) {
            w.add(next_id, td, 0, r.position);
            CHECK(w.ab->bind_spawn(r.serial, next_id));
            ++next_id;
        }
        w.collect();
        CHECK(w.ab->force_recharge(1, kDeployTroopers, 0.0f));
    };
    CHECK(w.ab->trooper_squads(1) == 0 && w.ab->can_activate(1, kDeployTroopers, false, false, w.frame));
    deploy();  // squad 0: 50, 51
    CHECK(w.ab->trooper_squads(1) == 1 && w.ab->can_activate(1, kDeployTroopers, false, false, w.frame));
    deploy();  // squad 1: 52, 53
    CHECK(w.ab->trooper_squads(1) == 2 && !w.ab->can_activate(1, kDeployTroopers, false, false, w.frame));
    CHECK(w.ab->set_unit_ability_mode(one, kDeployTroopers, true, w.frame) == 0 && w.ab->spawn_requests().empty());
    w.sys->kill_unit(50, kNoObject, kKindNormal, w.frame);
    w.tick();
    CHECK(w.ab->trooper_squads(1) == 2);  // 51 keeps squad 0 alive
    w.sys->kill_unit(51, kNoObject, kKindNormal, w.frame);
    w.tick();
    CHECK(w.ab->trooper_squads(1) == 1 && w.ab->can_activate(1, kDeployTroopers, false, false, w.frame));
    // Save round trip (squad numbers, squads formed).
    {
        const std::vector<uint8_t> b = ability_bytes(*w.ab);
        ByteReader rd(b);
        CHECK(w.ab->read(rd) && ability_bytes(*w.ab) == b && w.ab->trooper_squads(1) == 1);
    }
    // No cap (-1, the absent tag): never refused for the count.
    OiWorld v;
    AbilityObjectDesc va = a;
    va.unit_abilities[0].max_spawned_objects = -1;
    const uint32_t vd = push(v.cd, v.ad, oi_unit("AT_AT", 1000.0f), va);
    v.make(GameMode::Land);
    AbilityHost hv;
    hv.company_members = [](int32_t, std::vector<int32_t>& m) {
        m = {100, 101, 102, 103, 104, 105, 106, 107};
        return true;
    };
    hv.bone_position = [](ObjectId, std::string_view, Vec3& p) {
        p = Vec3{10.0f, 0.0f, 0.0f};
        return 1;
    };
    v.ab->set_host(std::move(hv));
    v.add(1, vd, 0, {0, 0, 0});
    for (int i = 0; i < 3; ++i) {
        CHECK(v.ab->can_activate(1, kDeployTroopers, false, false, v.frame));
        CHECK(v.ab->set_unit_ability_mode(one, kDeployTroopers, true, v.frame) == 1 && v.ab->spawn_requests().size() == 2);
        v.collect();
        CHECK(v.ab->force_recharge(1, kDeployTroopers, 0.0f));
    }
}

inline void test_cu8f_enable_gates() {
    // §3.3: Battlefield_Modifier Enable_* push one `true` entry per affected player (keyed by the source) on apply and
    // remove them by source; Reverse_Application_Logic swaps the two; Apply_To_All_Allies covers the allies. The Lua
    // Disable_* entry is one keyed entry. A gate is the AND of its list (the host adds the empty-list rule).
    OiWorld w;
    SpecialAbilityDesc bm = oi_special("Enable", SpecialClass::BattlefieldModifier, ActivationStyle::SpaceAutomatic);
    bm.enable_bombing_runs = true;
    bm.enable_special_weapons = true;
    bm.apply_to_all_allies = true;
    const uint32_t bs = add_special(w.ad, bm);
    SpecialAbilityDesc rv = oi_special("Reverse", SpecialClass::BattlefieldModifier, ActivationStyle::SpaceAutomatic);
    rv.enable_bombardment = true;
    rv.reverse_logic = true;
    rv.reinforcement_time_multiplier = 2.0f;  // cu8g t05: the float lists swap too (Slower_Reinforcements)
    const uint32_t rs = add_special(w.ad, rv);
    AbilityObjectDesc ha, ra;
    ha.specials = {bs};
    ra.specials = {rs};
    const uint32_t hd = push(w.cd, w.ad, oi_unit("Comm_Array", 100.0f), ha);
    const uint32_t rd = push(w.cd, w.ad, oi_unit("Reverse_Array", 100.0f), ra);
    w.make();
    w.sys->set_enemy(0, 2, false);  // player 2 is allied to 0
    w.sys->set_enemy(2, 0, false);
    auto gate = [&w](int32_t p, PlayerGate g, bool& all) { return w.ab->player_gate(p, g, all); };
    bool all = false;
    CHECK(gate(0, PlayerGate::BombingRuns, all) == 0 && all);
    w.add(1, hd, 0, {0, 0, 0});
    CHECK(gate(0, PlayerGate::BombingRuns, all) == 1 && all);
    CHECK(gate(2, PlayerGate::BombingRuns, all) == 1 && all);  // the ally
    CHECK(gate(1, PlayerGate::BombingRuns, all) == 0);
    CHECK(gate(0, PlayerGate::SpecialWeapons, all) == 1 && all && gate(0, PlayerGate::Bombardment, all) == 0);
    // the Lua entry: AND with the modifier's
    w.ab->set_player_gate_entry(0, PlayerGate::BombingRuns, 0x1234u, false);
    CHECK(gate(0, PlayerGate::BombingRuns, all) == 2 && !all);
    w.ab->set_player_gate_entry(0, PlayerGate::BombingRuns, 0x1234u, true);  // replaced, not appended
    CHECK(gate(0, PlayerGate::BombingRuns, all) == 2 && all);
    w.ab->set_player_gate_entry(0, PlayerGate::BombingRuns, 0x1234u, false);
    // Save round trip with the gate entries.
    {
        const std::vector<uint8_t> b = ability_bytes(*w.ab);
        ByteReader r(b);
        CHECK(w.ab->read(r) && ability_bytes(*w.ab) == b);
        CHECK(gate(0, PlayerGate::BombingRuns, all) == 2 && !all);
    }
    // the source goes: its entries go, the Lua entry stays
    w.sys->kill_unit(1, kNoObject, kKindNormal, w.frame);
    CHECK(gate(0, PlayerGate::BombingRuns, all) == 1 && !all);
    CHECK(gate(2, PlayerGate::BombingRuns, all) == 0 && gate(0, PlayerGate::SpecialWeapons, all) == 0);
    // Reverse_Application_Logic: nothing on apply, the entry on remove (the owner only: no Apply_To_All_Allies)
    w.add(2, rd, 0, {0, 0, 0});
    CHECK(gate(0, PlayerGate::Bombardment, all) == 0);
    // cu8g t05 (§3.3): the float lists follow the same swap, for the owner (not the enemies): nothing while it lives.
    CHECK(w.ab->player_list(0, PlayerList::ReinforcementTime) == 0.0f && w.ab->player_list(1, PlayerList::ReinforcementTime) == 0.0f);
    w.sys->kill_unit(2, kNoObject, kKindNormal, w.frame);
    CHECK(gate(0, PlayerGate::Bombardment, all) == 1 && all && gate(2, PlayerGate::Bombardment, all) == 0);
    CHECK(w.ab->player_list(0, PlayerList::ReinforcementTime) == 1.0f && w.ab->player_list(1, PlayerList::ReinforcementTime) == 0.0f);
}

inline void test_cu8f_super_laser() {
    // §3.4: Face_Target on a TACTICAL_SUPER_WEAPON: yaw-only turn clamped to +-Turn_Rate per step, the clip (and the
    // firing) waits for |dyaw| <= Firing_Cone, then the turning stops. Firing_Cone >= 90 reads as 0.01. Fire_Continuation:
    // the beam stays int(FPS x value) frames after the target is gone, no damage. Without TSW / Face_Target: no turn.
    struct Run {
        OiWorld w;
        SnapLog snaps;
        uint32_t sl = 0;
    };
    auto build = [](Run& r, bool tsw, float rate, float cone, float cont) {
        OiWorld& w = r.w;
        SpecialAbilityDesc s = oi_special("SuperLaser", SpecialClass::SuperLaser, ActivationStyle::UserInput);
        s.max_range = 5000.0f;
        s.face_target = true;
        s.turn_rate = rate;
        s.firing_cone = cone;
        s.fire_continuation = cont;
        s.damage_per_frame = 5.0f;
        const uint32_t ss = add_special(w.ad, s);
        r.sl = w.ad.specials[ss].name_crc;
        AbilityObjectDesc a;
        a.unit_abilities = {oi_gui(kSuperLaser, 10.0f, 30.0f, "SuperLaser")};
        a.specials = {ss};
        a.tactical_super_weapon = tsw;
        UnitDesc ud = oi_unit("Eclipse", 10000.0f, 50.0f, kCatCapital);
        ud.turn_rate = 2.0f;
        const uint32_t ed = push(w.cd, w.ad, ud, a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("Target", 1e6f, 10.0f, kCatCapital), AbilityObjectDesc{});
        w.make();
        snap_host(w, r.snaps);
        w.add(1, ed, 0, {0, 0, 0}, {1, 0, 0});
        w.add(2, td, 1, {0, 1000, 0});  // yaw 90
        const ObjectId one[] = {1};
        CHECK(w.ab->targeted_ability(one, kSuperLaser, 2, -1, w.frame) == 1);
    };
    auto yaw_of = [](OiWorld& w) {
        const Vec3 f = w.sys->facing(1);
        return dmath::atan2(f.y, f.x) * (180.0f / dmath::kPiF);
    };
    {  // TSW, rate 10, cone 25: 10 degrees a step; fires once within 25 of the target (at 70); then no more turning
        Run r;
        build(r, true, 10.0f, 25.0f, 0.0f);
        OiWorld& w = r.w;
        float last = 0.0f;
        bool fired = false;
        for (int i = 0; i < 30 && !fired; ++i) {
            w.tick();
            const float y = yaw_of(w);
            CHECK(y - last <= 10.0f + 1e-3f);
            fired = w.sys->health(2) < 1e6f;
            if (!fired) CHECK(90.0f - y > 25.0f);
            last = y;
        }
        CHECK(fired && std::abs(last - 70.0f) < 1e-3f && !r.snaps.calls.empty());
        for (int i = 0; i < 5; ++i) w.tick();
        CHECK(std::abs(yaw_of(w) - last) < 1e-4f);  // inside the cone: the turning stopped
        CHECK(w.ab->special_applied(1, r.sl));
    }
    {  // cone 90 -> 0.01: the first step (5 degrees) is not enough; it fires only when facing the target
        Run r;
        build(r, true, 5.0f, 90.0f, 0.0f);
        OiWorld& w = r.w;
        for (int i = 0; i < 5; ++i) w.tick();
        CHECK(w.sys->health(2) == 1e6f);
        for (int i = 0; i < 20; ++i) w.tick();
        CHECK(w.sys->health(2) < 1e6f && std::abs(yaw_of(w) - 90.0f) < 0.01f);
    }
    {  // Turn_Rate <= 0: the type's turn rate (Max_Rate_Of_Turn x the mode multiplier)
        Run r;
        build(r, true, 0.0f, 20.0f, 0.0f);
        OiWorld& w = r.w;
        const float per = 2.0f * w.sys->config().turn_multiplier_space;  // Max_Rate_Of_Turn 2
        for (int i = 0; i < 3 && yaw_of(w) == 0.0f; ++i) w.tick();  // the first update (the apply tick may come first)
        CHECK(per > 0.0f && std::abs(yaw_of(w) - per) < 1e-3f);
        w.tick();
        CHECK(std::abs(yaw_of(w) - 2.0f * per) < 1e-3f);
    }
    {  // not a TACTICAL_SUPER_WEAPON: no turn, the clip at once, fires off-cone
        Run r;
        build(r, false, 10.0f, 20.0f, 0.0f);
        OiWorld& w = r.w;
        for (int i = 0; i < 3; ++i) w.tick();
        CHECK(yaw_of(w) == 0.0f && w.sys->health(2) < 1e6f && r.snaps.calls.empty());
    }
    {  // Fire_Continuation 1 s: the target dies after the shot; the special stays 30 frames (beam, no damage), then ends
        Run r;
        build(r, false, 10.0f, 20.0f, 1.0f);
        OiWorld& w = r.w;
        for (int i = 0; i < 3; ++i) w.tick();
        CHECK(w.sys->health(2) < 1e6f);
        w.sys->kill_unit(2, kNoObject, kKindNormal, w.frame);
        const uint32_t fk = w.frame;
        w.tick();
        CHECK(w.ab->special_applied(1, r.sl));
        while (w.frame < fk + 30) {
            w.tick();
            CHECK(w.ab->special_applied(1, r.sl));
        }
        w.tick();  // fk + 30: the continuation is over
        CHECK(!w.ab->special_applied(1, r.sl));
    }
    {  // no continuation: the special ends when the target is gone
        Run r;
        build(r, false, 10.0f, 20.0f, 0.0f);
        OiWorld& w = r.w;
        for (int i = 0; i < 3; ++i) w.tick();
        w.sys->kill_unit(2, kNoObject, kKindNormal, w.frame);
        w.tick();
        CHECK(!w.ab->special_applied(1, r.sl));
    }
}

// cu8j t03 (CU8F_T01_T02_T07_RE §2.7 item 4): the jitter-free saber flight stand-in. Luke (v 8, Catch_Dist 15, MFD 250):
// at 150 units the max travel (250 + R + dz, here 275) stops the saber before it gets back: no catch, end 35 (retail run 1:
// 35); at 120 units it gets back: caught at step 29, the ability ends 11 frames later (40; retail 43 with the jitter).
inline void test_cu8j_saber_flight() {
    SaberFlight f = saber_flight(150.0f, 150.0f, 275.0f, 8.0f, 15.0f);
    CHECK(f.catch_step == 0 && f.end == 35);
    f = saber_flight(120.0f, 120.0f, 275.0f, 8.0f, 15.0f);
    CHECK(f.catch_step == 29 && f.end == 40);
    f = saber_flight(120.0f, 120.0f, 0.0f, 8.0f, 15.0f);  // no max travel: the same catch
    CHECK(f.catch_step == 29 && f.end == 40);
    f = saber_flight(400.0f, 400.0f, 0.0f, 8.0f, 15.0f);  // unlimited: always caught
    CHECK(f.catch_step == 99 && f.end == 110);
    f = saber_flight(5.0f, 5.0f, 275.0f, 8.0f, 15.0f);  // within Catch_Dist at once: caught at the first step
    CHECK(f.catch_step == 1 && f.end == 12);
    f = saber_flight(150.0f, 150.0f, 275.0f, 0.0f, 15.0f);  // unknown speed: no flight
    CHECK(f.catch_step == 0 && f.end == 0);
    CHECK(kSaberThrowDelay == 7);
}

// Test functions (defined in the part files)

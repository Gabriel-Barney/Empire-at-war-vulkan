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
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/objects/enums.hpp"
#include "alamo/tactical/combat/combat_system.hpp"
#include "alamo/tactical/combat/weather.hpp"
#include "thread_parallel_for.hpp"
#ifdef ALAMO_COMBAT_TESTS_HAVE_MOVEMENT
#include "alamo/tactical/movement/passability.hpp"
#endif
#ifdef ALAMO_COMBAT_TESTS_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

// ---- allocation counter (counts only while enabled) -------------------------------------------
namespace {
std::atomic<uint64_t> g_allocs{0};
std::atomic<bool> g_count{false};
}  // namespace
// GCC flags free() inside a replaced operator delete (-Wmismatched-new-delete) although the replaced
// operator new allocates with malloc(); the pairing is correct by construction (as in test_render).
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void* operator new(std::size_t n) {
    if (g_count.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

using namespace alamo;
using namespace alamo::tactical;
using namespace alamo::tactical::combat;

namespace {

using alamo::tactical::Vec3;  // hides alamo::Vec3 (galaxy.hpp) inside this namespace

int g_fail = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++g_fail;                                                    \
        }                                                                \
    } while (0)
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

Aabb box_of(Vec3 p, float r) { return {{p.x - r, p.y - r, p.z - r}, {p.x + r, p.y + r, p.z + r}}; }

Vec3 norm(Vec3 v) {  // same arithmetic as the system's normalize
    const float l2 = dot(v, v);
    const float inv = 1.0f / dmath::sqrt(l2);
    return v * inv;
}

bool same(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

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

uint32_t push_unit(CombatData& d, const UnitDesc& u) {
    d.units.push_back(u);
    return static_cast<uint32_t>(d.units.size() - 1);
}

// ================================================================================================
// formulas
// ================================================================================================

void test_scan_cadence() {
    SimRng r(777u), c(777u);
    const uint32_t n = next_scan_frame(100, 30, r);
    CHECK(n == 130u + static_cast<uint32_t>(c.range_int(0, 15)));
    CHECK(r.draws() == 1);
    for (int i = 0; i < 200; ++i) {
        const uint32_t f = next_scan_frame(1000, 30, r);
        CHECK(f >= 1030 && f <= 1045);
    }
}

void test_recharge() {
    // Pulse branch: int(30 x 0.2f) = 6; W unconditional: int(6 x 0.75 + 0.5) = 5; int(5 / 1.25 + 0.5) = 4.
    Recharge p = hp_pulse_recharge(0.2f, 3, 0.75f, 1.25f, 30);
    CHECK(p.frames == 4 && p.pulses == 3);
    p = hp_pulse_recharge(0.2f, 3, 2.0f, 1.0f, 30);  // W >= 1 still applied on this branch: int(12.5) = 12
    CHECK(p.frames == 12 && p.pulses == 3);
    p = hp_pulse_recharge(0.2f, 3, 1.0f, 0.0f, 30);  // F <= 0: frames and pulses 0
    CHECK(p.frames == 0 && p.pulses == 0);

    // Full recharge: Get_Int(300, 400) x 30 / 100, unsigned divide; W >= 1 NOT applied; F = 1 not applied.
    SimRng r(99u), c(99u);
    Recharge f = hp_full_recharge(3.0f, 4.0f, 7, 1.5f, 1.0f, 30, r);
    const int32_t v = c.range_int(300, 400);
    CHECK(f.frames == v * 30 / 100);
    CHECK(f.pulses == 7);
    // Truncated bounds: 0.337 x 100 = 33.7 -> 33 (rounding would give 34); 0.339 -> 33: lo == hi, no draw.
    const uint64_t d0 = r.draws();
    f = hp_full_recharge(0.337f, 0.339f, 7, 1.0f, 1.0f, 30, r);
    CHECK(r.draws() == d0);
    CHECK(f.frames == 9);  // 33 x 30 = 990, / 100 = 9 (34 would give 10)
    // W < 1 applied: 200 x 30 / 100 = 60 -> int(60 x 0.5 + 0.5) = 30; 0 < F < 1: int(30 / 0.8 + 0.5) = 38.
    f = hp_full_recharge(2.0f, 2.0f, 7, 0.5f, 0.8f, 30, r);
    CHECK(f.frames == 38);
    CHECK(f.pulses == static_cast<int32_t>(7.0f * 0.8f + 0.5f));  // int(6.1) = 6
    CHECK(f.pulses == 6);
    // F > 1 does not shorten the full recharge; pulses scale: int(7 x 2 + 0.5) = 14.
    f = hp_full_recharge(2.0f, 2.0f, 7, 1.0f, 2.0f, 30, r);
    CHECK(f.frames == 60 && f.pulses == 14);
    // First cooldown: Get_Int(0, int(30 x 4.0)).
    SimRng a(5u), b(5u);
    CHECK(hp_first_cooldown(4.0f, 30, a) == b.range_int(0, 120));

    // WeaponBehavior reset: int(1.0 x 30) + Get_Int(0, 10); frames = trunc(frames / F), pulses = trunc(F x n).
    SimRng w(31u), wc(31u);
    const Recharge wr = weapon_reset(1.0f, 3, 1.0f, 1.5f, 30, w);
    const int32_t extra = wc.range_int(0, 10);
    CHECK(wr.frames == static_cast<int32_t>(static_cast<float>(30 + extra) / 1.5f));
    CHECK(wr.pulses == 4);  // trunc(4.5): no +0.5 rounding
    CHECK(weapon_pulse_delay(0.15f, 2.0f, 30) == 2);  // int(4.5) = 4 -> trunc(4 / 2) = 2
}

void test_inaccuracy_formula() {
    CHECK_F(inaccuracy_f(GameMode::Space, 0.7f), 1.0f);
    CHECK_F(scale_inaccuracy(10.0f, inaccuracy_f(GameMode::Space, 0.7f)), 20.0f);  // doubled in space
    const float fl = inaccuracy_f(GameMode::Land, 0.7f);
    CHECK_F(scale_inaccuracy(10.0f, fl), 10.0f * (1.0f - 0.7f) + 10.0f);  // (2 - accuracy) x
    CHECK_F(scale_inaccuracy(10.0f, inaccuracy_f(GameMode::Land, 1.0f)), 10.0f);
    CHECK_F(inaccuracy_spread(14.0f, 500.0f, 1000.0f), 7.0f);
    SimRng r(3u), c(3u);
    const Vec3 o = inaccuracy_offset(10.0f, r);
    CHECK(r.draws() == 3);
    CHECK_F(o.x, c.range_float(-10.0f, 10.0f));
    CHECK_F(o.y, c.range_float(-10.0f, 10.0f));
    CHECK_F(o.z, c.range_float(-10.0f, 10.0f));
}

void test_range_lead_area() {
    // Fire range is XY only: (100, 0, 500) is in range 100 although 3D distance is ~510.
    const float d = distance_xy(Vec3{0, 0, 0}, Vec3{100, 0, 500});
    CHECK_F(d, 100.0f);
    CHECK(in_fire_range(d, 0.0f, 100.0f, 0.0f));
    CHECK(!in_fire_range(d, 101.0f, 1000.0f, 0.0f));         // below the min range
    CHECK(in_fire_range(105.0f, 0.0f, 100.0f, 5.0f));         // + target radius
    CHECK(!in_fire_range(105.5f, 0.0f, 100.0f, 5.0f));
    CHECK_F(fire_range(1000.0f, 0.25f), 1250.0f);             // x (1 + list 8)
    CHECK_F(lob_range(10.0f, 2.0f, 0.5f), 100.0f);            // Max_Speed^2 / (gravity x lob mod)

    // Lead: |(100, 10 t)| = 20 t -> t = 100 / sqrt(300) = 5.7735; aim (100, 57.735, 0).
    const Vec3 aim = linear_lead(Vec3{0, 0, 0}, Vec3{100, 0, 0}, Vec3{0, 10, 0}, 20.0f);
    CHECK_NEAR(aim.x, 100.0, 1e-4);
    CHECK_NEAR(aim.y, 57.73502691896258, 1e-3);
    // Unreachable (target faster, moving away): aim at the target.
    const Vec3 aim2 = linear_lead(Vec3{0, 0, 0}, Vec3{100, 0, 0}, Vec3{50, 0, 0}, 20.0f);
    CHECK(same(aim2, Vec3{100, 0, 0}));

    // Area falloff: 1 - floor(d x tiers / R) / (tiers + 1)
    CHECK_F(area_falloff(0.0f, 300.0f, 3), 1.0f);
    CHECK_F(area_falloff(150.0f, 300.0f, 3), 0.75f);  // floor(1.5) = 1
    CHECK_F(area_falloff(299.0f, 300.0f, 3), 0.5f);   // floor(2.99) = 2
    CHECK_F(area_falloff(300.0f, 300.0f, 3), 0.25f);  // floor(3) = 3
    CHECK_F(area_falloff(250.0f, 300.0f, 0), 1.0f);   // no tiers
    CHECK_F(area_falloff(10.0f, 0.0f, 3), 1.0f);
    CHECK_F(area_delay_seconds(150.0f, 300.0f, 2.0f), 1.0f);
    CHECK_F(area_delay_seconds(0.5f, 300.0f, 2.0f), 0.0f);  // d < 1
    CHECK(delay_frames(0.01f, 30) == 1);                    // max(1, int(FPS x delay))
    CHECK(delay_frames(0.5f, 30) == 15);

    // Shields: absolute regen cap during the lockout [H, disasm].
    CHECK_F(shield_regen_step(0.1f, 5.0f, 100.0f, true, 0.25f), 0.25f);
    CHECK_F(shield_regen_step(0.1f, 5.0f, 100.0f, false, 0.25f), 5.1f);
    CHECK_F(shield_regen_step(98.0f, 5.0f, 100.0f, false, 0.25f), 100.0f);
    CHECK_F(shield_regen_step(0.0f, 5.0f, 1000.0f, true, 0.25f), 0.25f);  // not 25% of the maximum
    CHECK(shield_lockout_active(249, 100, 30, 5.0f));                       // 149 / 30 < 5
    CHECK(!shield_lockout_active(250, 100, 30, 5.0f));                      // 150 / 30 = 5
    CHECK(shield_regen_interval_frames(30, 1.0f, 3.0f) == 90);
    CHECK(shield_regen_interval_frames(30, 0.5f, 3.0f) == 45);

    // Maxima / damage multiplier
    CHECK_F(max_hull(1000.0f, 1.5f, 1.0f, 0.2f), 1000.0f * 1.5f * 1.0f * 1.2f);
    CHECK_F(damage_multiplier(0.25f, 0.75f, 1.0f, 1.0f), 0.3125f);
}

// ================================================================================================
// modifiers
// ================================================================================================

void test_modifier_stacking() {
    ModifierSet s;
    // Same stacking category: maximum; two debuffs give the LESS negative one.
    s.insert(ModList::Damage, 1, 0.2f, kPermanent);
    s.insert(ModList::Damage, 1, 0.5f, kPermanent);
    s.insert(ModList::Damage, 2, -0.3f, kPermanent);
    s.insert(ModList::Damage, 2, -0.1f, kPermanent);
    CHECK_F(reduce_list(nullptr, &s, ModList::Damage), 0.5f + -0.1f);
    // Two heroes with the same aura (same category) count once.
    ModifierSet hero;
    hero.insert(ModList::Defense, 7, 0.25f, kPermanent, name_crc("AURA"), 10);
    hero.insert(ModList::Defense, 7, 0.25f, kPermanent, name_crc("AURA"), 11);
    CHECK_F(reduce_list(nullptr, &hero, ModList::Defense), 0.25f);
    // Categories are summed in ASCENDING id order, whatever the insertion order:
    // cat 1: 1e8, cat 2: -1e8, cat 3: 1 -> (1e8 - 1e8) + 1 = 1 (insertion order would give 0).
    ModifierSet o;
    o.insert(ModList::FireRange, 3, 1.0f, kPermanent);
    o.insert(ModList::FireRange, 1, 1e8f, kPermanent);
    o.insert(ModList::FireRange, 2, -1e8f, kPermanent);
    CHECK_F(reduce_list(nullptr, &o, ModList::FireRange), 1.0f);
    // Clamps: defense max 0.75, damage min -1.
    ModifierSet c;
    c.insert(ModList::Defense, 1, 0.5f, kPermanent);
    c.insert(ModList::Defense, 2, 0.5f, kPermanent);
    c.insert(ModList::Damage, 1, -0.7f, kPermanent);
    c.insert(ModList::Damage, 2, -0.6f, kPermanent);
    CHECK_F(reduce_list(nullptr, &c, ModList::Defense), 0.75f);
    CHECK_F(reduce_list(nullptr, &c, ModList::Damage), -1.0f);
    CHECK_F(reduce_list(nullptr, &c, ModList::MaxHealth), 0.0f);  // empty list
    CHECK_F(defense_factor(0.75f), 0.25f);
    // First of equal values kept (strictly greater replaces): -0.0 then +0.0 keeps -0.0.
    ModifierSet z;
    z.insert(ModList::FireRange, 4, -0.0f, kPermanent);
    z.insert(ModList::FireRange, 4, 0.0f, kPermanent);
    CHECK(std::signbit(reduce_list(nullptr, &z, ModList::FireRange)));
    // Gather = player set + object set, per-category max across both.
    ModifierSet pl, ob;
    pl.insert(ModList::Damage, 1, 0.3f, kPermanent);
    ob.insert(ModList::Damage, 1, 0.2f, kPermanent);
    ob.insert(ModList::Damage, 2, 0.1f, kPermanent);
    CHECK_F(reduce_list(&pl, &ob, ModList::Damage), 0.3f + 0.1f);
    // MostExtreme (player reveal list): largest |value| per category.
    ModifierSet me;
    me.insert(ModList::RevealRange, 1, 0.3f, kPermanent);
    me.insert(ModList::RevealRange, 1, -0.5f, kPermanent);
    me.insert(ModList::RevealRange, 2, 0.1f, kPermanent);
    CHECK_F(reduce_most_extreme(me.entries(), ModList::RevealRange), -0.5f + 0.1f);
}

void test_modifier_expiry() {
    CHECK(expiry_frame(10, 2.0f, 30) == 70u);
    CHECK(expiry_frame(10, 0.0f, 30) == 0u);
    CHECK(expiry_frame(10, -1.0f, 30) == 0u);
    CHECK(expiry_frame(10, 0.01f, 30) == 10u);  // int(0.3 + 0.5) = 0
    ModifierSet s;
    s.insert(ModList::Damage, 1, 0.5f, 100);         // expires after frame 100
    s.insert(ModList::Damage, 2, 0.1f, kPermanent);  // -1: never
    s.insert(ModList::Damage, 3, 0.2f, 0);           // duration <= 0: next purge
    // Reads between purges still count expired entries.
    CHECK_F(reduce_list(nullptr, &s, ModList::Damage), 0.5f + 0.1f + 0.2f);
    CHECK(s.purge_expired(100) == 1);  // 0 < 100 erased; 100 < 100 false: kept
    CHECK_F(reduce_list(nullptr, &s, ModList::Damage), 0.5f + 0.1f);
    CHECK(s.purge_expired(101) == 1);
    CHECK_F(reduce_list(nullptr, &s, ModList::Damage), 0.1f);
    CHECK(s.purge_expired(0xFFFFFFF0u) == 0);  // unsigned compare: permanent survives
    const uint32_t v = s.version();
    s.insert(ModList::Damage, 5, 0.3f, kPermanent, 0, 42);
    CHECK(s.version() != v);
    CHECK(s.remove_source(42) == 1);
    CHECK(s.entries().size() == 1);
}

void test_reduce_all_matches() {
    uint32_t st = 1;
    auto rnd = [&st]() {
        st = st * 1103515245u + 12345u;
        return (st >> 8) & 0xFFFFu;
    };
    for (int trial = 0; trial < 200; ++trial) {
        ModifierSet p, o;
        const uint32_t np = rnd() % 8, no = trial == 7 ? 90 : rnd() % 12;  // trial 7: > 64 entries (slow path)
        for (uint32_t i = 0; i < np; ++i)
            p.insert(static_cast<ModList>(rnd() % kModListCount), static_cast<int32_t>(rnd() % 5) - 1,
                     static_cast<float>(rnd() % 200) / 100.0f - 1.0f, kPermanent);
        for (uint32_t i = 0; i < no; ++i)
            o.insert(static_cast<ModList>(rnd() % kModListCount), static_cast<int32_t>(rnd() % 7),
                     static_cast<float>(rnd() % 200) / 100.0f - 1.0f, kPermanent);
        std::array<float, kModListCount> all{};
        reduce_all(&p, &o, default_list_clamps(), all);
        for (uint32_t l = 0; l < kModListCount; ++l) {
            const float one = reduce_list(&p, &o, static_cast<ModList>(l));
            CHECK(std::memcmp(&one, &all[l], sizeof(float)) == 0);
        }
    }
}

void test_mod_multiplier() {
    UnitAbilityDesc a, b, lucky;
    a.ability_type = kAbilityPowerToWeapons;
    a.mult[static_cast<size_t>(ModKind::FireRate) - 1] = 1.5f;
    a.flag[kFlagAllowDiminishingFirepower] = 0;
    b.ability_type = kAbilityDefend;
    b.mult[static_cast<size_t>(ModKind::FireRate) - 1] = 2.0f;
    lucky.ability_type = kAbilityLuckyShot;
    lucky.mult[static_cast<size_t>(ModKind::FireRate) - 1] = 10.0f;
    const UnitAbilityDesc* ab[2] = {&a, &b};
    bool both[2] = {true, true}, first[2] = {true, false}, none[2] = {false, false};
    CHECK_F(ability_product(ab, both, ModKind::FireRate), 3.0f);  // product over active slots
    CHECK_F(ability_product(ab, first, ModKind::FireRate), 1.5f);
    CHECK_F(ability_product(ab, none, ModKind::FireRate), 1.0f);
    CHECK_F(ability_product(ab, both, ModKind::Speed), 1.0f);
    const UnitAbilityDesc* al[2] = {&lucky, &a};
    CHECK_F(ability_product(al, both, ModKind::FireRate), 1.5f);  // LUCKY_SHOT skipped
    const float members[3] = {0.8f, 0.5f, 0.9f};
    CHECK_F(squad_multiplier(1.0f, members), 0.5f);  // leader at 1 -> member minimum
    CHECK_F(squad_multiplier(1.2f, members), 1.2f);
    CHECK_F(squad_multiplier(1.0f, {}), 1.0f);
    CHECK(!ability_flag(ab, both, kFlagAllowDiminishingFirepower));
    CHECK(ability_flag(ab, none, kFlagAllowDiminishingFirepower));  // default true
}

void test_priority_sets() {
    PrioritySet s;
    s.attack.push_back({PriorityEntry::Kind::Type, 7, 0, 0.5f});
    s.attack.push_back({PriorityEntry::Kind::Category, -1, 0x1, 2.0f});
    s.attack.push_back({PriorityEntry::Kind::Category, -1, 0x2, 1.5f});
    s.attack.push_back({PriorityEntry::Kind::Property, -1, 0x10, 4.0f});
    s.attack.push_back({PriorityEntry::Kind::Property, -1, 0x20, 3.0f});
    s.unit_exclusions = {9};
    s.category_exclusions = 0x100;
    s.property_exclusions = 0x1000;
    CHECK_F(priority_score(s, 7, 0x100, 0x1000), 0.5f);  // exact type returns at once
    CHECK_F(priority_score(s, 3, 0x3, 0), 1.5f);          // min over categories
    CHECK_F(priority_score(s, 3, 0x1, 0x10), 4.0f);       // first property hit OVERWRITES (worse than 2)
    CHECK_F(priority_score(s, 3, 0x1, 0x30), 3.0f);       // later property hits take the min
    CHECK_F(priority_score(s, 3, 0x100, 0), -1.0f);       // category exclusion
    CHECK_F(priority_score(s, 3, 0x100, 0x10), 4.0f);     // ... unless a property entry matched
    CHECK_F(priority_score(s, 3, 0, 0x1000), -1.0f);      // property exclusion
    CHECK_F(priority_score(s, 9, 0x1, 0), -1.0f);         // unit exclusion
    CHECK_F(priority_score(s, 3, 0, 0), 1e18f);           // nothing matched
    // refixf t06: the hard-point lists hold HardPointType values (Shield_Generator, Engine, ...), not hardpoint names.
    const auto hk = [](HardpointKind k) { return static_cast<int32_t>(k); };
    s.hardpoint_priorities = {hk(HardpointKind::ShieldGenerator), hk(HardpointKind::Engine)};
    s.hardpoint_exclusions = {hk(HardpointKind::FighterBay)};
    CHECK_F(hardpoint_rank(s, hk(HardpointKind::ShieldGenerator)), 1.0f);
    CHECK_F(hardpoint_rank(s, hk(HardpointKind::Engine)), 2.0f);
    CHECK_F(hardpoint_rank(s, hk(HardpointKind::FighterBay)), -1.0f);
    CHECK_F(hardpoint_rank(s, hk(HardpointKind::WeaponLaser)), 1e18f);
    // HardPointType names (TACTICAL_COMBAT §2.4, refixf t06): short names only, trimmed, case-insensitive; value 11 has
    // no name; the HardPoint XML's HARD_POINT_ spelling does not resolve.
    CHECK(hardpoint_type_value("Shield_Generator") == hk(HardpointKind::ShieldGenerator));
    CHECK(hardpoint_type_value(" Engine ") == hk(HardpointKind::Engine));
    CHECK(hardpoint_type_value("HARD_POINT_SHIELD_GENERATOR") == -1);
    CHECK(hardpoint_type_value("FIGHTER_BAY") == hk(HardpointKind::FighterBay));
    CHECK(hardpoint_type_value("weapon_ion_cannon") == hk(HardpointKind::WeaponIonCannon));
    CHECK(hardpoint_type_value("Weapon_Special") == hk(HardpointKind::WeaponSpecial));
    CHECK(hardpoint_type_value("Dummy_Art") == hk(HardpointKind::DummyArt));
    CHECK(hardpoint_type_value("Enable_Special_Ability") == -1);
    CHECK(hardpoint_type_value("HARD_POINT_") == -1 && hardpoint_type_value("HP_Shield") == -1 && hardpoint_type_value("") == -1);
}

void test_damage_table() {
    DamageArmorTable t;
    const uint16_t laser = t.add_damage_type("Damage_Laser");
    const uint16_t hull = t.add_armor_type("Armor_Hull");
    const uint16_t sh = t.add_armor_type("Shield_Hull");
    t.set(laser, hull, 2.0f);
    t.set(laser, sh, -0.5f);  // negative cell -> 1.0
    for (int i = 0; i < 40; ++i) t.add_armor_type("Armor_" + std::to_string(i));  // grow the stride
    CHECK_F(t.factor(laser, hull), 2.0f);
    CHECK_F(t.factor(laser, sh), 1.0f);
    CHECK_F(t.factor(kNoTypeId, hull), 1.0f);
    CHECK_F(t.factor(laser, t.armor_index("Armor_5")), 1.0f);  // unset cell
    CHECK(t.damage_index("damage_laser") == laser);            // case-insensitive
    CHECK(t.armor_index("nope") == kNoTypeId);
}

// ================================================================================================
// system-level rules
// ================================================================================================

constexpr uint64_t kCatFighter = 0x1, kCatCapital = 0x8;

// Shooter with one weapon hardpoint at its centre (no bone B, no muzzle draw) and a stationary target;
// returns the first projectile's direction and fills `expected` from a copy of the RNG.
void inaccuracy_case(GameMode mode, float accuracy, float fixed_radius, float override_radius, bool use_table, float expect_r,
                     uint64_t target_category = kCatCapital) {
    World w;
    w.data.projectiles.push_back(ProjectileDesc{});
    w.data.projectiles[0].max_speed = 10.0f;
    w.data.projectiles[0].damage = 1.0f;
    HardpointDesc hd;
    hd.kind = HardpointKind::WeaponLaser;
    hd.health = 100.0f;
    hd.projectile = 0;
    hd.randomize_between_fire_bones = false;
    hd.min_recharge = hd.max_recharge = 1.0f;
    hd.fire_range = 1000.0f;
    if (use_table) hd.inaccuracy.push_back({kCatCapital, 7.0f});
    w.data.hardpoints.push_back(hd);
    UnitAbilityDesc ab;
    ab.ability_type = kAbilityPowerToWeapons;
    ab.inaccuracy_override = override_radius;
    w.data.abilities.push_back(ab);
    UnitDesc shooter;
    shooter.tactical_health = 100.0f;
    shooter.max_attack_distance = 1000.0f;
    shooter.hardpoints = {0};
    shooter.fire_inaccuracy_fixed_radius = fixed_radius;
    shooter.abilities = {override_radius > 0.0f ? 0 : -1, -1};
    UnitDesc target;
    target.tactical_health = 1e6f;
    target.radius = 10.0f;
    target.category_mask = target_category;
    const uint32_t ds = push_unit(w.data, shooter), dt = push_unit(w.data, target);
    CombatConfig cfg;
    cfg.mode = mode;
    PlayerInfo p0;
    p0.ai_accuracy = accuracy;
    w.make(cfg, {p0, PlayerInfo{}});
    w.add(1, ds, 0, Vec3{0, 0, 0});
    w.add(2, dt, 1, Vec3{500, 0, 0});
    if (override_radius > 0.0f) w.sys->set_ability_active(1, 0, true);
    w.sys->order_attack(1, 2);
    for (int i = 0; i < 40 && w.sys->projectile_count() == 0; ++i) {
        SimRng copy = w.rng;
        w.tick();
        if (w.sys->projectile_count() == 1) {
            const uint64_t tick_draws = w.rng.draws() - copy.draws();
            if (expect_r > 0.0f) {
                Vec3 off;
                off.x = copy.range_float(-expect_r, expect_r);
                off.y = copy.range_float(-expect_r, expect_r);
                off.z = copy.range_float(-expect_r, expect_r);
                const Vec3 expect = norm(Vec3{500, 0, 0} + off);
                CHECK(same(w.sys->projectile(0).dir, expect));
                CHECK(tick_draws == 3);  // 3 inaccuracy; the full recharge Get_Int(100, 100) has no draw
            } else {
                // A1j t03 (0x381dc0): a table distance of 0 (no entry for the target's category) makes no draws.
                CHECK(same(w.sys->projectile(0).dir, Vec3{1, 0, 0}));
                CHECK(tick_draws == 0);
            }
        }
    }
    CHECK(w.sys->projectile_count() == 1);
}

// A1j t03: own-weapon inaccuracy 0x549f80 (TACTICAL_COMBAT §3.1.2). A shooter with its own weapon (no hardpoints, no
// muzzle bones) at the origin and a stationary capital 500 away; the first shot's direction must be the target plus
// the first `xyz_draws` draws of the firing tick in (-expect_r, expect_r) (2: X and Y only, 0: no offset). [The tick's
// draw count is not checked: the recharge's range_int(0, 10) takes a state-dependent number of draws.]
void own_inaccuracy_case(GameMode mode, float fixed_radius, bool use_table, float expect_r, int xyz_draws) {
    World w;
    w.data.projectiles.push_back(ProjectileDesc{});
    w.data.projectiles[0].max_speed = 10.0f;
    w.data.projectiles[0].damage = 1.0f;
    UnitDesc shooter;
    shooter.tactical_health = 100.0f;
    shooter.max_attack_distance = 1000.0f;
    shooter.weapon_projectile = 0;
    shooter.weapon_recharge = 1.0f;
    shooter.fire_inaccuracy_fixed_radius = fixed_radius;
    if (use_table) shooter.fire_inaccuracy = {{kCatFighter, 70.0f}, {kCatCapital, 7.0f}, {kCatCapital, 99.0f}};  // first hit
    UnitDesc target;
    target.tactical_health = 1e6f;
    target.radius = 10.0f;
    target.category_mask = kCatCapital;
    const uint32_t ds = push_unit(w.data, shooter), dt = push_unit(w.data, target);
    CombatConfig cfg;
    cfg.mode = mode;
    PlayerInfo p0;
    p0.ai_accuracy = 1.0f;
    w.make(cfg, {p0, PlayerInfo{}});
    w.add(1, ds, 0, Vec3{0, 0, 0});
    w.add(2, dt, 1, Vec3{500, 0, 0});
    w.sys->order_attack(1, 2);
    for (int i = 0; i < 40 && w.sys->projectile_count() == 0; ++i) {
        SimRng copy = w.rng;
        w.tick();
        if (w.sys->projectile_count() == 1) {
            Vec3 off;
            if (xyz_draws >= 2) {
                off.x = copy.range_float(-expect_r, expect_r);
                off.y = copy.range_float(-expect_r, expect_r);
            }
            if (xyz_draws == 3) off.z = copy.range_float(-expect_r, expect_r);
            CHECK(same(w.sys->projectile(0).dir, norm(Vec3{500, 0, 0} + off)));
        }
    }
    CHECK(w.sys->projectile_count() == 1);
}

void test_inaccuracy_system() {
    inaccuracy_case(GameMode::Space, 1.0f, 5.0f, 0.0f, false, 10.0f);  // fixed radius doubled in space
    inaccuracy_case(GameMode::Space, 1.0f, 0.0f, 0.0f, true, 7.0f);    // table: 7 x 2 x 500 / 1000
    inaccuracy_case(GameMode::Space, 1.0f, 5.0f, 5.0f, false, 5.0f);   // ability override: unscaled
    inaccuracy_case(GameMode::Land, 0.6f, 5.0f, 0.0f, false, 5.0f * (1.0f - 0.6f) + 5.0f);  // (2 - acc) x
    inaccuracy_case(GameMode::Space, 1.0f, 0.0f, 0.0f, true, 0.0f, kCatFighter);           // table distance 0: no draws
    // A1j t03 own weapon: the fixed radius draws X and Y, Z in space only; the type table s = d x L / max distance.
    own_inaccuracy_case(GameMode::Land, 5.0f, false, 5.0f, 2);
    own_inaccuracy_case(GameMode::Space, 5.0f, false, 10.0f, 3);
    own_inaccuracy_case(GameMode::Land, 0.0f, true, 7.0f * 500.0f / 1000.0f, 3);
    own_inaccuracy_case(GameMode::Space, 0.0f, true, 14.0f * 500.0f / 1000.0f, 3);
    own_inaccuracy_case(GameMode::Space, 0.0f, false, 0.0f, 0);  // no table entry: no offset
}

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

void test_targeting_rules() {
    {  // No equality guard: a WORSE score that is closer in XY replaces the best.
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{500, 0, 0});  // score 1.0, first in id order
        t.w.add(3, t.fighter, 1, Vec3{200, 0, 0});  // score 3.0, closer
        const uint64_t d0 = t.w.rng.draws();
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 3);
        CHECK(t.w.rng.draws() - d0 == 2);  // reschedule + start player
        CHECK(t.w.sys->next_scan(1) >= 31 && t.w.sys->next_scan(1) <= 46);
    }
    {  // Same scene, ids swapped: the strictly lower score wins when it comes later.
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.fighter, 1, Vec3{200, 0, 0});
        t.w.add(3, t.capital, 1, Vec3{500, 0, 0});
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 3);
    }
    {  // Equal scores: the XY-closer one, whatever the order.
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{500, 0, 900});  // far in XY, Z ignored
        t.w.add(3, t.capital, 1, Vec3{200, 0, -900});
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 3);
    }
    {  // Attacker with hardpoints: the aimable candidate wins over a closer one behind the cone.
        TargetWorld t;
        t.w.add(1, t.hp_attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{-200, 0, 0});  // behind, closer
        t.w.add(3, t.capital, 1, Vec3{600, 0, 0});   // in the forward cone
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 3);
    }
    {  // Low-health rule first: a damaged fighter beats a healthy, closer, better-scored capital.
        TargetWorld t(0.5f);
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{200, 0, 0});
        t.w.add(3, t.fighter, 1, Vec3{800, 0, 0}, {1, 0, 0}, 0.3f);
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 3);
    }
    {  // Aim-point draws: one Get_Int per candidate with hardpoints; then the early exit keeps a 1.0 target.
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.hp_target, 1, Vec3{300, 0, 0});
        t.w.add(3, t.hp_target, 1, Vec3{400, 0, 0});
        uint64_t d0 = t.w.rng.draws();
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 2);
        const uint64_t scan_draws = t.w.rng.draws() - d0;
        CHECK(scan_draws >= 4);  // 2 + one Get_Int(0, 1) per candidate (each may redraw)
        // Next scan: the current target is valid and scored exactly 1.0 -> no rescan, 1 draw only.
        while (t.w.frame < t.w.sys->next_scan(1)) t.w.tick();
        d0 = t.w.rng.draws();
        t.w.tick();
        CHECK(t.w.rng.draws() - d0 == 1);
        CHECK(t.w.sys->auto_target(1) == 2);
    }
    {  // An attack order suppresses auto scans and is the fire target; a dead order is cleared.
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{300, 0, 0});
        t.w.add(3, t.fighter, 1, Vec3{100, 0, 0});
        t.w.sys->order_attack(1, 2);
        const uint64_t d0 = t.w.rng.draws();
        t.w.tick();
        CHECK(t.w.rng.draws() == d0);
        CHECK(t.w.sys->current_target(1) == 2);
        DamageRequest kill;
        kill.victim = 2;
        kill.amount = 1e9f;
        t.w.sys->take_damage(kill, t.w.frame);
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 3);
    }
}

// ---- OI tactical: circular lead, team targeting ------------------------------------------------

void test_circular_lead() {
    const Vec3 m{0, 0, 0};
    // Straight track (collinear history): exactly linear lead with V = (p2 - p1) x FPS.
    {
        const Vec3 p0{100, -2, 5}, p1{100, -1, 5}, p2{100, 0, 5};
        const Vec3 a = circular_lead(m, p0, p1, p2, Vec3{}, 20.0f, 30);
        const Vec3 b = linear_lead(m, p2, (p2 - p1) * 30.0f, 20.0f);
        CHECK(same(a, b));
        CHECK(same(circular_lead(m, p2, p2, p2, Vec3{}, 20.0f, 30), p2));  // stationary
    }
    // Target on a circle of radius 200 about (500, 0), 1 degree per frame (30 deg/s) counter-clockwise; shot 300 u/s.
    {
        const float r = 200.0f;
        auto on = [&](float deg) {
            float s = 0.0f, c = 0.0f;
            dmath::sincos(deg * 0.017453292519943295f, s, c);
            return Vec3{500.0f + r * c, r * s, 0.0f};
        };
        const Vec3 p0 = on(-2.0f), p1 = on(-1.0f), p2 = on(0.0f);
        const Vec3 a = circular_lead(m, p0, p1, p2, Vec3{}, 300.0f, 30);
        // The aim point lies on the circle (within float error) ...
        const float dr = dmath::sqrt((a.x - 500.0f) * (a.x - 500.0f) + a.y * a.y);
        CHECK_NEAR(dr, 200.0, 0.05);
        // ... ahead of the target in the turn direction (positive y), and |aim| = S t for t = swept angle / 30 deg/s.
        CHECK(a.y > 0.0f);
        const float ang = dmath::atan2(a.y, a.x - 500.0f) * 57.29577951308232f;
        const float t = ang / 30.0f;
        CHECK_NEAR(dmath::sqrt(dot(a, a)), 300.0 * static_cast<double>(t), 0.5);
        // Differs from the linear (tangent) lead.
        CHECK(!same(a, linear_lead(m, p2, (p2 - p1) * 30.0f, 300.0f)));
        // A hardpoint offset is added unrotated.
        const Vec3 off{0, 0, 7};
        const Vec3 ao = circular_lead(m, p0, p1, p2, off, 300.0f, 30);
        CHECK_NEAR(ao.z, 7.0, 0.02);
        // Radius > 1000: linear.
        auto big = [&](float deg) {
            float s = 0.0f, c = 0.0f;
            dmath::sincos(deg * 0.017453292519943295f, s, c);
            return Vec3{5000.0f + 1500.0f * c, 1500.0f * s, 0.0f};
        };
        const Vec3 q0 = big(-2.0f), q1 = big(-1.0f), q2 = big(0.0f);
        CHECK(same(circular_lead(m, q0, q1, q2, Vec3{}, 300.0f, 30), linear_lead(m, q2, (q2 - q1) * 30.0f, 300.0f)));
    }
}

void test_turn_toward() {
    // S9 t05 (S9_DEATH_CLONE_SHIELDS.md §2.3): Turn_Toward clamps the yaw and pitch deltas separately; rate 0 keeps
    // the heading bit for bit (vanilla lasers vs missile shields).
    const Vec3 fwd{1, 0, 0};
    const Vec3 a = turn_toward(fwd, Vec3{-1, 0, 0}, 0.0f);
    CHECK(a.x == fwd.x && a.y == fwd.y && a.z == fwd.z);
    const Vec3 z = turn_toward(fwd, Vec3{0, 0, 0}, 10.0f);  // no goal: unchanged
    CHECK(z.x == fwd.x && z.y == fwd.y && z.z == fwd.z);
    // goal behind (yaw delta 180): +3.6 degrees of yaw, no pitch
    const Vec3 b = turn_toward(fwd, Vec3{-1, 0, 0}, 3.6f);
    CHECK_NEAR(dmath::atan2(b.y, b.x) * 180.0f / dmath::kPiF, 3.6, 1e-4);
    CHECK_NEAR(b.z, 0.0, 1e-7);
    // from yaw 180 to yaw -10: delta -190 wraps to +170 (the short side), clamped to +5 -> yaw 185 = -175
    const Vec3 c = turn_toward(Vec3{-1, 0, 0}, Vec3{1, -0.1763269f, 0}, 5.0f);
    CHECK_NEAR(dmath::atan2(c.y, c.x) * 180.0f / dmath::kPiF, -175.0, 1e-3);
    // pitch and yaw clamped separately: goal up-left (yaw 90, pitch 45) -> yaw 2, pitch 2
    const Vec3 d = turn_toward(fwd, Vec3{0, 1, 1.4142135f}, 2.0f);
    CHECK_NEAR(dmath::atan2(d.y, d.x) * 180.0f / dmath::kPiF, 2.0, 1e-4);
    CHECK_NEAR(dmath::asin(d.z) * 180.0f / dmath::kPiF, 2.0, 1e-3);
    // within the rate: the goal heading (unit length)
    const Vec3 e = turn_toward(fwd, Vec3{10, 1, 0}, 30.0f);
    CHECK_NEAR(e.y / e.x, 0.1, 1e-5);
    CHECK_NEAR(dot(e, e), 1.0, 1e-6);
}

// ---- S8 t04: lob solver and grenade flight (docs/re/LOB_SOLVER.md) ------------------------------------------------

constexpr float kDeg = 57.29577951308232f;
float lob_dist(Vec3 a, Vec3 b) { return dmath::sqrt(dot(a - b, a - b)); }

// SPMAT_Walker's Proj_Ground_SPMAT_Grenade (§1.2): v = 30, G = 1.636363636 (R = v^2 / G = 550), Can_Lob_Slower.
ProjectileDesc spmat_grenade() {
    ProjectileDesc g;
    g.name = "Proj_Ground_SPMAT_Grenade";
    g.max_speed = 30.0f;
    g.damage = 10.0f;
    g.lobbed = true;
    g.category = ProjectileCategory::Grenade;
    g.grenade_gravity = 1.0f;
    g.lob_mod = 900.0f / 550.0f;
    g.can_lob_slower = true;
    g.max_lifetime = 900.0f;
    return g;
}

void test_lob_solver() {
    const float v = 30.0f, G = 900.0f / 550.0f;
    LobSolution s;
    // §2.3 table, SPMAT at 275 on flat ground: 75.0 deg / 35.4 frames at Max_Speed; with the flag 0.8 v, 64.3 deg /
    // 26.4 frames (what retail does).
    CHECK(lob_solve(Vec3{0, 0, 0}, Vec3{275, 0, 0}, v, G, false, s));
    CHECK(s.speed == 30.0f);
    CHECK_NEAR(s.angle * kDeg, 75.0, 0.01);
    CHECK_NEAR(275.0f / (s.speed * dmath::cos(s.angle)), 35.42, 0.02);
    CHECK(lob_solve(Vec3{0, 0, 0}, Vec3{275, 0, 0}, v, G, true, s));
    CHECK(s.speed == 30.0f * 0.8f);
    CHECK_NEAR(s.angle * kDeg, 64.31, 0.01);
    CHECK_NEAR(275.0f / (s.speed * dmath::cos(s.angle)), 26.43, 0.02);
    // d / R = 0.25: 82.8 deg without the flag; 0.512 v, 53.8 deg with it. d / R = 0.9: v, 57.9 deg either way.
    CHECK(lob_solve(Vec3{0, 0, 0}, Vec3{0, 137.5f, 0}, v, G, false, s));
    CHECK_NEAR(s.angle * kDeg, 82.76, 0.01);
    CHECK(lob_solve(Vec3{0, 0, 0}, Vec3{0, 137.5f, 0}, v, G, true, s));
    CHECK(s.speed == 30.0f * 0.8f * 0.8f * 0.8f);
    CHECK_NEAR(s.angle * kDeg, 53.75, 0.01);
    CHECK(lob_solve(Vec3{10, 10, 0}, Vec3{10 - 495.0f, 10, 0}, v, G, true, s));
    CHECK(s.speed == 30.0f);
    CHECK_NEAR(s.angle * kDeg, 57.92, 0.01);
    // §4: maximum range v^2 / G = 550 at equal heights; + Ranged_Target_Z_Adjust 6 on the target: (v / G) sqrt(v^2 - 12 G)
    // = 543.97 - between the two the unit is "in scan range" but there is no shot.
    CHECK(lob_solve(Vec3{0, 0, 0}, Vec3{549.0f, 0, 0}, v, G, false, s));
    CHECK(!lob_solve(Vec3{0, 0, 0}, Vec3{551.0f, 0, 0}, v, G, false, s));
    CHECK(lob_solve(Vec3{0, 0, 0}, Vec3{543.5f, 0, 6.0f}, v, G, true, s));
    CHECK(!lob_solve(Vec3{0, 0, 0}, Vec3{544.5f, 0, 6.0f}, v, G, true, s));
    CHECK(s.angle == 0.0f);  // no solution: angle 0, speed untouched
    CHECK_F(lob_range(30.0f, 1.0f, 900.0f / 550.0f), 550.0f);
    // Too high: v^2 < G h is out of range even right next to it.
    CHECK(!lob_solve(Vec3{0, 0, 0}, Vec3{10, 0, 600}, v, G, false, s));
    // d = 0 -> 0.0001: straight up (~90 deg); with the flag the speed steps down to about sqrt(G x 0.0001).
    CHECK(lob_solve(Vec3{5, 5, 0}, Vec3{5, 5, 0}, v, G, false, s));
    CHECK(s.angle * kDeg > 89.99f && s.speed == 30.0f);
    CHECK(lob_solve(Vec3{5, 5, 0}, Vec3{5, 5, 0}, v, G, true, s));
    CHECK(s.speed < 0.02f && s.speed > 0.01f);
    // §8 item 13 [port]: G <= 0 is "no solution" (retail never ends the 0.8 loop at G = 0).
    CHECK(!lob_solve(Vec3{0, 0, 0}, Vec3{100, 0, 0}, v, 0.0f, true, s));
    CHECK(!lob_solve(Vec3{0, 0, 0}, Vec3{100, 0, 0}, v, -1.0f, false, s));
    // §1.1 defaults: gravity 1.05, lob mod 5.02 (G = 5.271).
    const ProjectileDesc def;
    CHECK(def.grenade_gravity == 1.05f && def.lob_mod == 5.02f && !def.can_lob_slower && !def.sticks_on_collision);
    // §6.1: the closed form passes through Q at t = d / (s cos theta).
    CHECK(lob_solve(Vec3{0, 0, 6}, Vec3{200, 100, 6}, v, G, true, s));
    const Vec3 f = lob_forward(Vec3{2, 1, -7}, s.angle);  // yaw from the heading's XY part only
    CHECK_NEAR(dot(f, f), 1.0, 1e-6);
    CHECK_NEAR(dmath::asin(f.z) * kDeg, s.angle * kDeg, 1e-3);
    const float tq = dmath::sqrt(200.0f * 200.0f + 100.0f * 100.0f) / (s.speed * dmath::cos(s.angle));
    CHECK(lob_dist(lob_position(Vec3{0, 0, 6}, f, s.speed, G, tq), Vec3{200, 100, 6}) < 0.01f);
    CHECK(same(lob_position(Vec3{1, 2, 3}, f, s.speed, G, 0.0f), Vec3{1, 2, 3}));
}

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

void test_lob_targeting() {
    // Slot 44 (§3.1): the solve from the attacker to the target + Ranged_Target_Z_Adjust; the scan box / attack range is
    // v^2 / G x (1 + list 8) = 550 - Targeting_Max_Attack_Distance (100) is not used. Slot 45 is unchanged.
    for (const float x : {300.0f, 543.5f, 544.5f, 549.0f}) {
        LobWorld lw;
        lw.w.data.units[lw.target].ranged_target_z_adjust = 6.0f;
        lw.w.add(1, lw.thrower, 0, Vec3{0, 0, 0});
        lw.w.add(2, lw.target, 1, Vec3{x, 0, 0});
        CHECK_F(lw.w.sys->attack_range(1), 550.0f);
        CHECK_F(lw.w.sys->base_attack_range_of(1), 550.0f);
        lw.w.tick();
        CHECK(lw.w.sys->auto_target(1) == (x < 544.0f ? 2 : kNoObject));
    }
    {  // Targeting_Min_Attack_Distance 220 still refuses closer targets (slot 45).
        LobWorld lw;
        lw.w.data.units[lw.thrower].min_attack_distance = 220.0f;
        lw.w.add(1, lw.thrower, 0, Vec3{0, 0, 0});
        lw.w.add(2, lw.target, 1, Vec3{200, 0, 0});
        lw.w.tick();
        CHECK(lw.w.sys->auto_target(1) == kNoObject);
    }
    {  // No arc = no shot (§5.2): an ordered target out of reach is never fired at (and no draw is spent on it).
        LobWorld lw;
        lw.w.add(1, lw.thrower, 0, Vec3{0, 0, 0});
        lw.w.add(2, lw.target, 1, Vec3{600, 0, 0});
        lw.w.sys->order_attack(1, 2);
        for (int i = 0; i < 400; ++i) lw.w.tick();
        CHECK(lw.w.sys->stats().shots == 0u && lw.w.sys->projectile_count() == 0u);
    }
}

void test_lob_weapon_flight() {
    // SPMAT at 275 (land, flat ground at z 0): the WeaponBehavior shot launches at 0.8 v = 24 with a 64.3 deg pitch, flies
    // N(t) = L + s t f - G t^2 / 2 z exactly and hits the target (radius 2 box) on the way down between t = 26 and 27.
    LobWorld lw;
    lw.w.add(1, lw.thrower, 0, Vec3{0, 0, 0});
    lw.w.add(2, lw.target, 1, Vec3{275, 0, 0});
    const float G = 900.0f / 550.0f;
    uint32_t spawn = 0, hit_frame = 0;
    Vec3 launch, f;
    bool exact = true;
    for (int i = 0; i < 400 && hit_frame == 0; ++i) {
        lw.w.tick();
        if (lw.w.sys->stats().hits > 0) hit_frame = lw.w.frame - 1;
        if (lw.w.sys->projectile_count() == 0) continue;
        const ProjectileView pv = lw.w.sys->projectile(0);
        if (spawn == 0) {
            spawn = pv.spawn_frame;
            launch = pv.pos;
            f = pv.dir;
            CHECK(pv.lob && pv.speed == 24.0f);
            CHECK_NEAR(dmath::asin(f.z) * kDeg, 64.31, 0.01);
            CHECK(f.y == 0.0f && f.x > 0.0f);  // yaw toward the target
            continue;
        }
        const float t = static_cast<float>(lw.w.frame - 1 - spawn);
        if (!same(pv.pos, lob_position(launch, f, 24.0f, G, t))) exact = false;
    }
    CHECK(spawn != 0 && exact);
    CHECK(hit_frame == spawn + 27);
    CHECK(lw.w.sys->health(2) == lw.w.sys->max_health(2) - 10.0f);  // direct damage (Process_Hit)
}

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

// S8 t06: a heightmap grid (20-unit cells, height scale 1/16) of w x h vertices at `level` raw units everywhere.
GroundGrid flat_grid(uint32_t w, uint32_t h, int16_t level) {
    GroundGrid g;
    g.cell = 20.0f;
    g.width = w;
    g.height = h;
    g.height_scale = 0.0625f;
    g.heights.assign(static_cast<size_t>(w) * h, level);
    return g;
}

// A1j t03 (TACTICAL_COMBAT §3.4, 0x4d0630): the land ground test of straight shots. A plateau at z 5 (vertices x >= 160)
// lies between the launch at (0, 0, 1) and the point / target at x 300.
void test_straight_ground_test() {
    GroundGrid g = flat_grid(20, 4, 0);
    for (uint32_t y = 0; y < g.height; ++y)
        for (uint32_t x = 8; x < g.width; ++x) g.heights[y * g.width + x] = 80;
    ProjectileDesc shot;
    shot.max_speed = 10.0f;
    shot.damage = 10.0f;
    // No target: tested on every step in land (detonation on the ramp x 140..160 below z 1), never in space.
    for (const GameMode mode : {GameMode::Land, GameMode::Space}) {
        LobLaunch ll(mode, shot, Vec3{300, 0, 1}, &g);
        Vec3 at;
        bool done = false;
        for (int i = 0; i < 100 && !done; ++i) {
            ll.tick();
            for (const HitEvent& e : ll.sys->hit_events()) {
                done = true;
                at = e.position;
            }
        }
        CHECK(done);
        if (mode == GameMode::Land)
            CHECK(at.x > 140.0f && at.x < 144.0f && std::fabs(at.z - (at.x - 140.0f) * 0.25f) < 0.01f);
        else
            CHECK(at.x > 290.0f);
    }
    // A target ahead on all three axes: a DEFAULT shot skips the test and hits; a LASER is tested and detonates.
    for (const ProjectileCategory cat : {ProjectileCategory::Default, ProjectileCategory::Laser}) {
        World w;
        shot.category = cat;
        w.data.projectiles = {shot};
        UnitDesc t;
        t.tactical_health = 1e6f;
        t.radius = 10.0f;
        const uint32_t td = push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        w.make(cfg);
        RocketTerrain rt;
        rt.grid = g;
        w.sys->set_rocket_terrain(std::move(rt));
        w.add(2, td, 1, Vec3{300, 0, 0});
        ProjectileLaunch l;
        l.owner = 0;
        l.from = Vec3{0, 0, 1};
        l.target = 2;
        CHECK(w.sys->launch_projectile(l, w.frame));
        for (int i = 0; i < 60 && w.sys->projectile_count() != 0; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 0u);
        CHECK(w.sys->stats().hits == (cat == ProjectileCategory::Default ? 1u : 0u));
        CHECK(w.sys->stats().expired == (cat == ProjectileCategory::Default ? 0u : 1u));
    }
}

void test_lob_ground_and_expiry() {
    {  // Land: the terrain stops the arc (ground test) - On_Detonate at the ground hit, not at the aim height. A plateau
       // at z 5 (vertices x >= 160; 80 raw x 1/16) is struck before the arc comes down to the aim point (z 0) at x 200.
        GroundGrid g = flat_grid(16, 4, 0);
        for (uint32_t y = 0; y < g.height; ++y)
            for (uint32_t x = 8; x < g.width; ++x) g.heights[y * g.width + x] = 80;
        LobLaunch ll(GameMode::Land, spmat_grenade(), Vec3{200, 0, 0}, &g);
        bool done = false;
        Vec3 at;
        for (int i = 0; i < 100 && !done; ++i) {
            ll.tick();
            for (const HitEvent& e : ll.sys->hit_events()) {
                done = true;
                at = e.position;
                CHECK(e.victim == kNoObject);
            }
        }
        CHECK(done && std::fabs(at.z - 5.0f) < 0.001f && at.x < 200.0f && at.x > 180.0f);
        CHECK(ll.sys->stats().expired == 1u && ll.sys->stats().hits == 0u && ll.sys->projectile_count() == 0u);
    }
    {  // §6.6 water: a water plane at z -3 over ground at z -5 - the grenade detonates on the surface.
        GroundGrid g = flat_grid(16, 4, -80);
        g.water_type = 1;
        g.water_height = -3.0f;
        LobLaunch lw(GameMode::Land, spmat_grenade(), Vec3{200, 0, 10}, &g);
        Vec3 at;
        bool done = false;
        for (int i = 0; i < 100 && !done; ++i) {
            lw.tick();
            for (const HitEvent& e : lw.sys->hit_events()) {
                done = true;
                at = e.position;
            }
        }
        CHECK(done && std::fabs(at.z + 3.0f) < 0.001f && at.x > 200.0f);
    }
    {  // Space: no ground test - the grenade falls on past Q until its lifetime (> 2 s: the 61st frame) runs out.
        ProjectileDesc g = spmat_grenade();
        g.max_lifetime = 2.0f;
        LobLaunch ll(GameMode::Space, g, Vec3{200, 0, 0});
        int steps = 0;
        while (ll.sys->projectile_count() > 0 && steps < 200) {
            ll.tick();
            ++steps;
        }
        CHECK(steps == 62);  // launched at frame 1, first stepped at 2, expires at frame 62 (61 frames > 2 s)
        CHECK(ll.sys->stats().expired == 1u);
    }
    {  // §6.5: the lifetime replaces the travel budget only with Max_Lifetime > 0. Max_Flight_Distance 50, no lifetime:
       // DistanceTravelled += s (24 per step) reaches 50 on the third step.
        ProjectileDesc g = spmat_grenade();
        g.max_lifetime = 0.0f;
        g.max_flight_distance = 50.0f;
        LobLaunch ll(GameMode::Space, g, Vec3{275, 0, 0});
        CHECK(ll.sys->projectile(0).max_travel == 50.0f && ll.sys->projectile(0).speed == 24.0f);
        for (int i = 0; i < 3; ++i) ll.tick();  // frame 1 (spawn), 2, 3: two steps
        CHECK(ll.sys->projectile_count() == 1u);
        ll.tick();
        CHECK(ll.sys->projectile_count() == 0u);
        g.max_lifetime = 900.0f;  // with a lifetime the budget never applies
        LobLaunch lt(GameMode::Space, g, Vec3{275, 0, 0});
        for (int i = 0; i < 10; ++i) lt.tick();
        CHECK(lt.sys->projectile_count() == 1u && lt.sys->projectile(0).travelled == 24.0f * 9.0f);
    }
    {  // No solution: nothing is spawned.
        CombatData d;
        d.projectiles = {spmat_grenade()};
        CombatSystem sys(d, CombatConfig{}, {PlayerInfo{}, PlayerInfo{}});
        ProjectileLaunch l;
        l.from = Vec3{0, 0, 0};
        l.point = Vec3{600, 0, 0};
        CHECK(!sys.launch_projectile(l, 1) && sys.projectile_count() == 0u);
        CHECK(!sys.lob_solution(0, l.from, l.point) && sys.lob_solution(0, l.from, Vec3{500, 0, 0}));
        d.projectiles[0].grenade_gravity = 0.0f;  // an XML 0 is kept: G = 0 -> no arc [port]
        CHECK(!sys.lob_solution(0, l.from, Vec3{50, 0, 0}));
    }
}

void test_lob_sticky() {
    // §6.4: a sticky grenade that hits an object attaches to it (no contact damage), rides along, and waits for its
    // BOMB timer; one that hits the ground rests there.
    ProjectileDesc g = spmat_grenade();
    g.name = "Proj_Sticky_Bomb";
    g.max_speed = 15.0f;
    g.lob_mod = 1.0f;  // G = 1, R = 225
    g.sticks_on_collision = true;
    g.blast_area_damage = 40.0f;
    g.blast_area_range = 30.0f;
    LobWorld lw(GameMode::Land, g);
    lw.w.data.units[lw.thrower].weapon_projectile = -1;  // no weapon: the toss comes from the host (ability)
    lw.w.add(1, lw.thrower, 0, Vec3{0, 0, 0});
    lw.w.add(2, lw.target, 1, Vec3{100, 0, 0});
    ProjectileLaunch l;
    l.shooter = 1;
    l.owner = 0;
    l.from = Vec3{0, 0, 0};
    l.target = 2;
    l.fuse_seconds = 3.0f;
    l.toss = true;
    const uint32_t f0 = lw.w.frame;
    CHECK(lw.w.sys->launch_projectile(l, f0));
    CHECK(lw.w.sys->projectile(0).lob && !lw.w.sys->projectile(0).stuck);
    int guard = 0;
    while (!lw.w.sys->projectile(0).stuck && guard++ < 200) lw.w.tick();
    ProjectileView pv = lw.w.sys->projectile(0);
    CHECK(pv.stuck && pv.stuck_to == 2 && pv.speed == 0.0f);
    CHECK(lw.w.sys->health(2) == lw.w.sys->max_health(2) && lw.w.sys->stats().hits == 0u);  // no contact damage
    const Vec3 offset = pv.pos - Vec3{100, 0, 0};
    CHECK(offset.x < 0.0f);  // struck on the near side
    lw.move(2, Vec3{100, 40, 0});
    lw.w.tick();
    CHECK(same(lw.w.sys->projectile(0).pos, Vec3{100, 40, 0} + offset));
    // S8 t06 (§6.4, §8 item 18): the stuck point is kept in the parent's frame - the parent turns 90 degrees left and the
    // bomb turns with it: (ox, oy) -> (-oy, ox).
    lw.w.sys->set_facing(2, Vec3{0, 1, 0});
    lw.w.tick();
    const Vec3 q = lw.w.sys->projectile(0).pos;
    CHECK(std::fabs(q.x - (100.0f - offset.y)) < 1e-4f && std::fabs(q.y - (40.0f + offset.x)) < 1e-4f &&
          std::fabs(q.z - offset.z) < 1e-4f);
    while (lw.w.sys->projectile_count() > 0 && guard++ < 400) lw.w.tick();
    CHECK(lw.w.frame - 1 == f0 + 90);  // the 3 s timer: detonation frame int(30 x 3 + f0 + 0.5)
    CHECK(lw.w.sys->health(2) < lw.w.sys->max_health(2));  // area damage at the bomb
    {  // ground: rests where it came down (flat ground at z 0: no grid -> the [port] plane), not at the aim height; no
       // lifetime once stopped (Max_Lifetime 2 s here, still resting after 10 s)
        ProjectileDesc gl = g;
        gl.max_lifetime = 2.0f;
        LobLaunch ll(GameMode::Land, gl, Vec3{100, 0, 20});
        for (int i = 0; i < 200 && !ll.sys->projectile(0).stuck; ++i) ll.tick();
        const ProjectileView gv = ll.sys->projectile(0);
        CHECK(gv.stuck && gv.stuck_to == kNoObject && std::fabs(gv.pos.z) < 0.001f && gv.pos.x > 100.0f);
        for (int i = 0; i < 300; ++i) ll.tick();
        CHECK(ll.sys->projectile_count() == 1u && ll.sys->projectile(0).alive && ll.sys->stats().expired == 0u);
    }
}

void test_ground_segment() {
    // S8 t06 (§6.6, 0x135840): 16-unit cells so the Moeller-Trumbore arithmetic is exact in float.
    GroundGrid g = flat_grid(2, 2, 0);
    g.cell = 16.0f;
    g.height_scale = 1.0f;
    g.heights[1 * 2 + 0] = 8;  // v01 (x 0, y 1) at z 8
    GroundHit h;
    // Diagonal (x, y)-(x+1, y+1): below it T1 = (v00, v10, v11) is flat z 0 ...
    CHECK(ground_segment(g, Vec3{12, 4, 8}, Vec3{12, 4, -8}, h) && h.t == 0.5f && h.point.z == 0.0f && !h.water);
    // ... above it T2 = (v00, v11, v01): z = 8 (y - x) / 16 = 4 at (4, 12) (the other diagonal would give 6)
    CHECK(ground_segment(g, Vec3{4, 12, 8}, Vec3{4, 12, -8}, h) && h.t == 0.25f && h.point.z == 4.0f);
    CHECK(ground_segment(g, Vec3{4, 12, 0}, Vec3{4, 12, 16}, h) && h.t == 0.25f && h.point.z == 4.0f);  // from below
    float best = 1.0f;
    CHECK(ray_triangle(Vec3{4, 12, 8}, Vec3{0, 0, -16}, Vec3{0, 0, 0}, Vec3{16, 16, 0}, Vec3{0, 16, 8}, best) && best == 0.25f);
    CHECK(!ray_triangle(Vec3{12, 4, 8}, Vec3{0, 0, -16}, Vec3{0, 0, 0}, Vec3{16, 16, 0}, Vec3{0, 16, 8}, best));  // u + v > 1
    // t = 0 and t = 1 are not hits; outside the grid nothing is hit (no plane fallback with a grid)
    CHECK(!ground_segment(g, Vec3{12, 4, 0}, Vec3{12, 4, -16}, h));
    CHECK(!ground_segment(g, Vec3{12, 4, 16}, Vec3{12, 4, 0}, h));
    CHECK(!ground_segment(g, Vec3{40, 4, 8}, Vec3{40, 4, -8}, h));
    {  // water plane: nearest of triangles and plane; water type 0 = none; a plane below the ground loses
        GroundGrid w = flat_grid(2, 2, 0);
        w.cell = 16.0f;
        w.water_type = 1;
        w.water_height = 4.0f;
        CHECK(ground_segment(w, Vec3{12, 4, 8}, Vec3{12, 4, -8}, h) && h.water && h.t == 0.25f && h.point.z == 4.0f);
        CHECK(ground_segment(w, Vec3{12, 4, 0}, Vec3{12, 4, 16}, h) && h.water && h.t == 0.25f);  // both directions
        w.water_height = -4.0f;
        CHECK(ground_segment(w, Vec3{12, 4, 8}, Vec3{12, 4, -8}, h) && !h.water && h.t == 0.5f);
        w.water_height = 4.0f;
        w.water_type = 0;
        CHECK(ground_segment(w, Vec3{12, 4, 8}, Vec3{12, 4, -8}, h) && !h.water && h.t == 0.5f);
        w.water_type = 3;  // ice: a plane too
        CHECK(ground_segment(w, Vec3{12, 4, 8}, Vec3{12, 4, -8}, h) && h.water);
    }
    {  // a slope a few cells along a level segment: the nearest triangle hit over every cell the segment's box touches
        GroundGrid s = flat_grid(6, 2, 0);
        s.cell = 16.0f;
        s.height_scale = 1.0f;
        for (uint32_t y = 0; y < 2; ++y)
            for (uint32_t x = 3; x < 6; ++x) s.heights[y * 6 + x] = 16;  // cell 2 (32..48) rises 0 -> 16
        CHECK(ground_segment(s, Vec3{4, 4, 4}, Vec3{76, 4, 4}, h) && std::fabs(h.point.x - 36.0f) < 1e-4f &&
              std::fabs(h.t - 32.0f / 72.0f) < 1e-6f);
    }
    {  // no grid: [port] the plane z 0
        CHECK(ground_segment(GroundGrid{}, Vec3{5, 5, 8}, Vec3{5, 5, -8}, h) && h.t == 0.5f && !h.water);
    }
}

void test_lob_object_ground_tie() {
    // §6.2 step 3: an object hit wins when its fraction <= the ground fraction. The precise hook reports the ground test's
    // own t for the step (a tie: Process_Hit on the unit), then the next float above it (On_Detonate on the ground).
    for (const bool tie : {true, false}) {
        LobWorld lw(GameMode::Land);
        lw.w.data.units[lw.target].radius = 30.0f;
        RocketTerrain rt;
        rt.grid = flat_grid(32, 4, 0);
        const GroundGrid grid = rt.grid;
        lw.w.sys->set_rocket_terrain(rt);
        lw.w.add(2, lw.target, 1, Vec3{210, 10, 0});
        ProjectileLaunch l;
        l.owner = 0;
        l.from = Vec3{10, 10, 1};
        l.point = Vec3{210, 10, 0};
        CHECK(lw.w.sys->launch_projectile(l, lw.w.frame));
        const ProjectileView pv = lw.w.sys->projectile(0);
        const float G = spmat_grenade().grenade_gravity * spmat_grenade().lob_mod;
        uint32_t now = 0;
        int ties = 0;
        lw.w.sys->set_precise_hit([&](const PreciseQuery& q, PreciseResult& r) {
            const Vec3 N = lob_position(pv.pos, pv.dir, pv.speed, G, static_cast<float>(now - pv.spawn_frame));
            GroundHit gh;
            if (!ground_segment(grid, q.a, N, gh)) return PreciseOutcome::Miss;
            ++ties;
            r.t = tie ? gh.t : std::nextafter(gh.t, 2.0f);
            return PreciseOutcome::Hit;
        });
        ObjectId victim = 1;
        for (int i = 0; i < 100 && lw.w.sys->projectile_count() > 0; ++i) {
            now = lw.w.frame;
            lw.w.tick();
            for (const HitEvent& e : lw.w.sys->hit_events()) victim = e.victim;
        }
        CHECK(ties == 1);
        CHECK(victim == (tie ? 2 : kNoObject));
        CHECK(lw.w.sys->stats().hits == (tie ? 1u : 0u));
        CHECK((lw.w.sys->health(2) < lw.w.sys->max_health(2)) == tie);  // direct damage only on the object hit
    }
}

void test_lob_fuse_rounding() {
    // S8 t06 (§6.4): BOMB arm 0x58f030 - detonation frame int(30 x secs + spawn frame + 0.5), fired at frame >= it, the
    // countdown running in flight. 0.11 s: 3.3 + 0.5 -> 3 frames (age >= 0.11 s would need 4); 0.12 s: 4.1 -> 4.
    for (const auto& [secs, frames] : {std::pair<float, uint32_t>{0.11f, 3u}, {0.12f, 4u}, {0.1f, 3u}}) {
        for (const bool toss : {true, false}) {  // a tossed grenade (flying) and a placed bomb
            CombatData d;
            ProjectileDesc pd = toss ? spmat_grenade() : ProjectileDesc{};
            pd.blast_area_damage = 1.0f;
            d.projectiles = {pd};
            CombatConfig cfg;
            cfg.mode = GameMode::Space;
            CombatSystem sys(d, cfg, {PlayerInfo{}, PlayerInfo{}});
            TreeSet trees{2};
            SimRng rng{3u};
            ProjectileLaunch l;
            l.owner = 0;
            l.from = Vec3{0, 0, 0};
            l.point = Vec3{275, 0, 0};
            l.fuse_seconds = secs;
            l.toss = toss;
            const uint32_t f0 = 10;
            CHECK(sys.launch_projectile(l, f0));
            uint32_t f = f0;
            while (sys.projectile_count() > 0 && f < f0 + 20) sys.tick(++f, trees, rng);
            CHECK(sys.stats().expired == 1u);
            CHECK(f == f0 + frames);
        }
    }
}

void test_lob_hardpoint() {
    // §5.4: hardpoint shots do not use the solver - straight at the aim point at Max_Speed - and the parabola's gravity
    // pulls them below that line.
    World w;
    w.data.projectiles.push_back(spmat_grenade());
    HardpointDesc hd;
    hd.kind = HardpointKind::WeaponMissile;
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
    const uint32_t td = push_unit(w.data, t);
    CombatConfig cfg;
    cfg.mode = GameMode::Land;
    cfg.inaccuracy_enabled = false;
    w.make(cfg);
    w.add(1, ad, 0, Vec3{0, 0, 0});
    w.add(2, td, 1, Vec3{100, 0, 0});
    for (int i = 0; i < 60 && w.sys->projectile_count() == 0; ++i) w.tick();
    CHECK(w.sys->projectile_count() == 1u);
    const ProjectileView pv = w.sys->projectile(0);
    CHECK(pv.lob && pv.speed == 30.0f);
    CHECK(same(pv.dir, norm(Vec3{100, 0, -10})));  // straight at the aim point (the target's position)
    w.tick();
    const Vec3 n = w.sys->projectile(0).pos;
    CHECK(same(n, lob_position(Vec3{0, 0, 10}, pv.dir, 30.0f, 900.0f / 550.0f, 1.0f)));
    CHECK(n.z < 10.0f + pv.dir.z * 30.0f);  // below the straight line
}

void test_lob_save() {
    // N1: a grenade in flight and a stuck one survive write -> read -> write byte for byte and fly on identically.
    ProjectileDesc g = spmat_grenade();
    g.sticks_on_collision = true;
    LobLaunch a(GameMode::Land, g, Vec3{150, 0, 0});
    ProjectileLaunch l2;
    l2.from = Vec3{0, 0, 0};
    l2.point = Vec3{80, 20, 0};
    CHECK(a.sys->launch_projectile(l2, a.frame));
    for (int i = 0; i < 20; ++i) a.tick();
    CHECK(a.sys->projectile_count() == 2u && a.sys->projectile(1).stuck != a.sys->projectile(0).stuck);
    ByteWriter wa;
    a.sys->write(wa);
    LobLaunch b(GameMode::Land, g, Vec3{150, 0, 0});
    ByteReader r(wa.bytes());
    CHECK(b.sys->read(r));
    b.frame = a.frame;
    b.rng = a.rng;
    ByteWriter wb;
    b.sys->write(wb);
    CHECK(wa.bytes() == wb.bytes());
    for (int i = 0; i < 40; ++i) {
        a.tick();
        b.tick();
    }
    StateHasher ha, hb;
    a.sys->hash_projectiles(ha);
    a.sys->hash_extended(ha);
    b.sys->hash_projectiles(hb);
    b.sys->hash_extended(hb);
    CHECK(ha.digest() == hb.digest());
}

// Adds a squad unit (team id / team_leader) to a TargetWorld.
void add_team_unit(TargetWorld& t, ObjectId id, uint32_t desc, int32_t owner, Vec3 p, uint32_t team, bool leader) {
    UnitSpawn s;
    s.id = id;
    s.desc = desc;
    s.owner = owner;
    s.position = p;
    s.team = team;
    s.team_leader = leader;
    CHECK(t.w.sys->add_unit(s, t.w.frame, t.w.rng));
    TreeMembership m;
    m.owner = owner;
    CHECK(t.w.trees.add(static_cast<EntityHandle>(id), box_of(p, t.w.data.units[desc].radius), 1u, m));
}

void test_team_targeting() {
    // 2.4: a team member scores with its leader's priority set. The member's own type has no set (every score 1.0:
    // the closer fighter wins); with the leader's set (capital 1, fighter 3) the later, strictly better capital wins.
    for (const bool in_team : {true, false}) {
        TargetWorld t;
        UnitDesc noset = t.w.data.units[t.attacker];
        noset.priority_set = -1;
        const uint32_t md = push_unit(t.w.data, noset);
        t.w.make();
        add_team_unit(t, 2, t.fighter, 1, Vec3{200, 0, 0}, 0, false);
        add_team_unit(t, 3, t.capital, 1, Vec3{500, 0, 0}, 0, false);
        add_team_unit(t, 10, t.attacker, 0, Vec3{0, 300, 0}, 7, true);  // leader (far from both: its pick is irrelevant)
        add_team_unit(t, 11, md, 0, Vec3{0, 0, 0}, in_team ? 7u : 0u, false);
        t.w.tick();
        CHECK(t.w.sys->auto_target(11) == (in_team ? 3 : 2));
    }
    // 2.3 filter 9: the member's current target stays valid out of its own range while it is the head's target.
    for (const bool in_team : {true, false}) {
        TargetWorld t;
        UnitDesc shortr = t.w.data.units[t.attacker];
        shortr.priority_set = -1;
        shortr.max_attack_distance = 300.0f;
        const uint32_t md = push_unit(t.w.data, shortr);
        t.w.make();
        add_team_unit(t, 2, t.capital, 1, Vec3{900, 0, 0}, 0, false);
        add_team_unit(t, 10, t.attacker, 0, Vec3{0, 0, 0}, 7, true);  // range 1000: acquires the capital
        add_team_unit(t, 11, md, 0, Vec3{700, 0, 0}, in_team ? 7u : 0u, false);
        t.w.tick();
        CHECK(t.w.sys->auto_target(10) == 2);
        CHECK(t.w.sys->auto_target(11) == 2);
        t.w.sys->set_position(11, Vec3{300, 0, 0});  // 600 from the capital: out of its 300 range
        t.w.trees.update(11, box_of(Vec3{300, 0, 0}, t.w.data.units[md].radius));
        const uint32_t due = t.w.sys->next_scan(11);
        while (t.w.frame <= due) t.w.tick();
        CHECK(t.w.sys->auto_target(11) == (in_team ? 2 : kNoObject));
    }
    // s78f t07: a dead team_leader hands the role to the team's first live member (ascending id). Leader 10 has no set;
    // after it is removed, member 11 (capital first) leads, so member 12 (no set of its own) scores with 11's set and
    // takes the farther capital. Without the hand-over 12 would have no head and take the closer fighter (all 1.0).
    for (const bool remove_leader : {true, false}) {
        TargetWorld t;
        UnitDesc noset = t.w.data.units[t.attacker];
        noset.priority_set = -1;
        const uint32_t md = push_unit(t.w.data, noset);
        t.w.make();
        add_team_unit(t, 10, md, 0, Vec3{0, 300, 0}, 7, true);
        add_team_unit(t, 11, t.attacker, 0, Vec3{0, -300, 0}, 7, false);
        add_team_unit(t, 12, md, 0, Vec3{0, 0, 0}, 7, false);
        t.w.tick();
        if (remove_leader) {
            CHECK(t.w.sys->remove_unit(10));
            t.w.trees.remove(10);
        }
        t.w.tick();
        add_team_unit(t, 20, t.fighter, 1, Vec3{200, 0, 0}, 0, false);
        add_team_unit(t, 21, t.capital, 1, Vec3{500, 0, 0}, 0, false);
        const uint32_t due = t.w.sys->next_scan(12);
        while (t.w.frame <= due) t.w.tick();
        CHECK(t.w.sys->auto_target(12) == (remove_leader ? 21 : 20));
    }
    // Squad head = the container object when it is a combat unit (team id = its object id): the member scores with
    // the container's set (capital first) although its own type has none.
    {
        TargetWorld t;
        UnitDesc noset = t.w.data.units[t.attacker];
        noset.priority_set = -1;
        const uint32_t md = push_unit(t.w.data, noset);
        t.w.make();
        add_team_unit(t, 2, t.fighter, 1, Vec3{200, 0, 0}, 0, false);
        add_team_unit(t, 3, t.capital, 1, Vec3{500, 0, 0}, 0, false);
        add_team_unit(t, 9, t.attacker, 0, Vec3{0, -300, 0}, 0, false);  // the container
        add_team_unit(t, 11, md, 0, Vec3{0, 0, 0}, 9, false);
        t.w.tick();
        CHECK(t.w.sys->auto_target(11) == 3);
    }
}
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

void test_gaps_constants() {
    OneFileProvider fp;
    fp.text = "<GameConstants><Health_Low_Percent_Threshold>0.33</Health_Low_Percent_Threshold>"
              "<Melee_Cutoff_Range>51.0</Melee_Cutoff_Range>"
              "<Hardpoint_Recharge_Cutoff_For_Opportunity_Fire>5.0</Hardpoint_Recharge_Cutoff_For_Opportunity_Fire>"
              "<Object_Max_Speed_Multiplier_Space> 1.2 </Object_Max_Speed_Multiplier_Space>"
              "<Object_Max_Speed_Multiplier_Land> 1.56 </Object_Max_Speed_Multiplier_Land>"
              "<Diminishing_Firepower>0, 0.6, 0.3, 0.7, 0.9, 0.9, 1, 1, 2, 1</Diminishing_Firepower></GameConstants>";
    data::GameDatabase db;
    std::vector<std::string> ignored;
    db.load(fp, {}, &ignored);  // only GameConstants exists: the other loaders report missing files
    CombatConfig cfg;
    apply_game_constants(cfg, db.constants());
    CHECK_F(cfg.low_health_threshold, 0.33f);
    CHECK_F(cfg.short_range_cutoff, 51.0f);
    CHECK_F(cfg.opportunity_recharge_cutoff, 5.0f);
    CHECK_F(cfg.turn_multiplier_space, 1.2f);  // S9 t05
    CHECK_F(cfg.turn_multiplier_land, 1.56f);
    CHECK(cfg.diminishing_firepower.size() == 5);
    // Spline through the FoC points: knots reproduced, smooth in between, end values outside.
    World w;
    w.data.damage.add_damage_type("Laser");
    UnitDesc v;
    v.tactical_health = 1e6f;
    const uint32_t vd = push_unit(w.data, v);
    w.make(cfg);
    w.add(1, vd, 0, Vec3{0, 0, 0});
    auto hit = [&](uint32_t frame) {
        DamageRequest r;
        r.victim = 1;
        r.amount = 100.0f;
        return w.sys->take_damage(r, frame).after_multipliers;
    };
    CHECK_NEAR(hit(1000), 100.0, 1e-3);          // first hit: 999999 frames -> last y (1)
    CHECK_NEAR(hit(1000), 60.0, 1e-3);           // 0 s -> 0.6
    CHECK_NEAR(hit(1009), 70.0, 1e-2);           // 0.3 s -> 0.7 (knot)
    const float mid = hit(1009 + 18);            // 0.6 s: between 0.7 and 0.9
    CHECK(mid > 70.0f && mid < 90.0f);
    CHECK_NEAR(hit(1027 + 90), 100.0, 1e-3);     // 3 s: past the last knot
    // Slots 44 / 45: min range rejection and the footprint half-extent along the line of sight.
    TargetWorld t;
    UnitDesc mr = t.w.data.units[t.attacker];
    mr.min_attack_distance = 300.0f;
    const uint32_t mrd = push_unit(t.w.data, mr);
    UnitDesc wide = t.w.data.units[t.capital];
    wide.hard_x_extent = 200.0f;  // long ship: X extent along its facing
    wide.hard_y_extent = 20.0f;
    wide.radius = 200.0f;  // tree box (the query cube must reach it); E comes from the hard extents
    const uint32_t wd = push_unit(t.w.data, wide);
    t.w.make();
    t.w.add(1, mrd, 0, Vec3{0, 0, 0});
    t.w.add(2, t.capital, 1, Vec3{250, 0, 0});    // inside the min range (300 + radius 10)
    t.w.add(3, t.capital, 1, Vec3{0, 600, 0});    // outside: taken
    t.w.add(4, wd, 1, Vec3{1150, 0, 0}, {1, 0, 0});  // 1150 > 1000 + 20, but facing along the LOS: E = 200
    t.w.tick();
    CHECK(t.w.sys->auto_target(1) == 3 || t.w.sys->auto_target(1) == 4);
    CHECK(t.w.sys->auto_target(1) != 2);
    TargetWorld u;
    UnitDesc wide2 = u.w.data.units[u.capital];
    wide2.hard_x_extent = 200.0f;
    wide2.hard_y_extent = 20.0f;
    wide2.radius = 200.0f;
    const uint32_t wd2 = push_unit(u.w.data, wide2);
    u.w.make();
    u.w.add(1, u.attacker, 0, Vec3{0, 0, 0});
    u.w.add(2, wd2, 1, Vec3{1150, 0, 0}, {0, 1, 0});  // broadside: E = 20 -> out of range
    u.w.tick();
    CHECK(u.w.sys->auto_target(1) == kNoObject);
    TargetWorld v2;
    const uint32_t wd3 = push_unit(v2.w.data, wide2);
    v2.w.make();
    v2.w.add(1, v2.attacker, 0, Vec3{0, 0, 0});
    v2.w.add(2, wd3, 1, Vec3{1150, 0, 0}, {1, 0, 0});  // end-on: E = 200 -> in range
    v2.w.tick();
    CHECK(v2.w.sys->auto_target(1) == 2);
    // A1j t04 (0x371e70 / 0x372040): without Custom_Hard extents E falls back to |model bbox max.x / max.y| x Scale_Factor.
    for (const bool end_on : {true, false}) {
        TargetWorld m;
        UnitDesc boxed = m.w.data.units[m.capital];
        boxed.model_x_extent = 200.0f;
        boxed.model_y_extent = 20.0f;
        boxed.radius = 200.0f;
        const uint32_t bd = push_unit(m.w.data, boxed);
        m.w.make();
        m.w.add(1, m.attacker, 0, Vec3{0, 0, 0});
        m.w.add(2, bd, 1, Vec3{1150, 0, 0}, end_on ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
        m.w.tick();
        CHECK(m.w.sys->auto_target(1) == (end_on ? ObjectId{2} : kNoObject));
    }
}

// OI tactical: TACTICAL_GAPS 3.1 slot 43 with a LAND_OBSTACLE target (aim walk toward the attacker over the cells the
// target occupies, cell-centre snap, E = sqrt(200)); OI abilities: the weapon index obj +0x324 into Projectile_Types.
void test_land_obstacle_and_weapon_index() {
    auto building = [](TargetWorld& t) {
        UnitDesc b = t.w.data.units[t.capital];
        b.land_obstacle = true;
        b.radius = 300.0f;  // tree box; without the query E = radius
        return push_unit(t.w.data, b);
    };
    // cells of 20: the building occupies |x - 1250| < 200 on the x axis
    auto query = [](ObjectId, Vec3 p, Vec3& c) {
        c = Vec3{std::floor(p.x / 20.0f) * 20.0f + 10.0f, std::floor(p.y / 20.0f) * 20.0f + 10.0f, p.z};
        return std::fabs(p.x - 1250.0f) < 200.0f && std::fabs(p.y) < 200.0f ? ObstacleCell::Occupied : ObstacleCell::Free;
    };
    {  // S8 t01: the host reports no footprint (not stamped): the half-extent rule, E = radius 300 -> taken
        TargetWorld t;
        const uint32_t bd = building(t);
        t.w.make();
        std::atomic<int> calls{0};  // the scan may run on worker threads
        t.w.sys->set_obstacle_cell_query([&calls](ObjectId, Vec3 p, Vec3& c) {
            calls.fetch_add(1);
            c = p;
            return ObstacleCell::NoFootprint;
        });
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, bd, 1, Vec3{1250, 0, 0});
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 2 && calls.load() > 0);
    }
    {  // no query: E = radius 300 -> 1250 <= 1000 + 300, taken
        TargetWorld t;
        const uint32_t bd = building(t);
        t.w.make();
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, bd, 1, Vec3{1250, 0, 0});
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 2);
    }
    {  // walk 1250 -> 1050 (first free cell), centre 1050: 1050 > 1000 + 14.14 -> out of range
        TargetWorld t;
        const uint32_t bd = building(t);
        t.w.make();
        t.w.sys->set_obstacle_cell_query(query);
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, bd, 1, Vec3{1250, 0, 0});
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == kNoObject);
    }
    {  // attacker at x = 40: the free cell at 1050 is 1010 away -> in range
        TargetWorld t;
        const uint32_t bd = building(t);
        t.w.make();
        t.w.sys->set_obstacle_cell_query(query);
        t.w.add(1, t.attacker, 0, Vec3{40, 0, 0});
        t.w.add(2, bd, 1, Vec3{1250, 0, 0});
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 2);
    }
#ifdef ALAMO_COMBAT_TESTS_HAVE_MOVEMENT
    {  // S8 t01: the walk over a real PassabilityGrid stamp (the battle host's query: PassabilityGrid::occupies)
        namespace mv = alamo::tactical::movement;
        mv::PassabilityGrid g;
        g.init(96, 24);
        // 400 x 200 footprint centred on (1250, 100): cells x 53..73 (centres 1060..1460), y 0..10.
        CHECK(g.stamp(-1, mv::make_footprint({1250.0f, 100.0f}, 0.0f, 400.0f, 200.0f, -200.0f, -100.0f, 0, 0, 1.0f, 0)) > 0);
        CHECK(g.occupies(-1, 53, 5) && g.occupies(-1, 73, 5) && !g.occupies(-1, 52, 5) && !g.occupies(-1, 74, 5));
        auto host_query = [&g](ObjectId id, Vec3 p, Vec3& c) {
            if (!g.stamped(id)) return ObstacleCell::NoFootprint;
            const int32_t cx = mv::world_to_cell(p.x), cy = mv::world_to_cell(p.y);
            const mv::V2 cc = mv::cell_center(cx, cy);
            c = Vec3{cc.x, cc.y, p.z};
            return g.occupies(id, cx, cy) ? ObstacleCell::Occupied : ObstacleCell::Free;
        };
        auto run = [&](float ax, float min_dist, bool& taken, bool& inside_min) {
            TargetWorld t;
            const uint32_t bd = building(t);
            UnitDesc a = t.w.data.units[t.attacker];
            a.min_attack_distance = min_dist;
            const uint32_t ad = push_unit(t.w.data, a);
            t.w.make();
            t.w.sys->set_obstacle_cell_query(host_query);
            t.w.add(1, ad, 0, Vec3{ax, 100, 0});
            t.w.add(2, bd, 1, Vec3{1250, 100, 0});
            t.w.tick();
            taken = t.w.sys->auto_target(1) == 2;
            inside_min = t.w.sys->inside_min_range(1, 2);
        };
        bool taken = false, inside = false;
        // Map stamp id -1 (not the object's id): NoFootprint -> E = radius 300: 1250 <= 1300 taken, 1250 <= 1000 + 300
        // inside the min range.
        run(0.0f, 1000.0f, taken, inside);
        CHECK(!taken && inside);  // min range 1000 rejects it in the scan too
        run(0.0f, 0.0f, taken, inside);
        CHECK(taken && !inside);
        // The stamp handed to object 2 (rekey): walk 1250 -> 1050 (cell 53, occupied) -> 1030 (cell 52, free, centre 1040).
        CHECK(g.rekey(-1, 2));
        run(0.0f, 0.0f, taken, inside);
        CHECK(!taken);  // 1040 > 1000 + sqrt(200)
        run(30.0f, 0.0f, taken, inside);
        CHECK(taken);  // 1010 <= 1014.14
        run(20.0f, 0.0f, taken, inside);
        CHECK(!taken);  // 1020 > 1014.14
        run(40.0f, 1000.0f, taken, inside);
        CHECK(!taken && inside);  // max: 1000 in range; min: 1000 <= 1000 + 14.14 inside -> rejected
        run(60.0f, 950.0f, taken, inside);
        CHECK(taken && !inside);  // d = 980: in max range, outside 950 + 14.14
        // Attacker on the far side (x = 1500): the walk goes +x to cell 74 (centre 1480), d = 20.
        run(1500.0f, 10.0f, taken, inside);
        CHECK(!taken && inside);  // 20 <= 10 + 14.14: inside a min range of 10
        // [port] attacker inside the footprint (x = 1300, len 50): steps 1250 / 1270 / 1290 / 1310, then walked 60 > 50
        // stops on an occupied cell (66, centre 1320): d = 20.
        run(1300.0f, 10.0f, taken, inside);
        CHECK(!taken && inside);
        run(1300.0f, 0.0f, taken, inside);
        CHECK(taken && !inside);
    }
#endif
    {  // weapon index: Projectile_Types[obj +0x324]; a lobbed second entry changes Get_Max_Attack_Range
        TargetWorld t;
        ProjectileDesc lob;
        lob.lobbed = true;
        lob.max_speed = 20.0f;
        lob.grenade_gravity = 1.0f;
        t.w.data.projectiles.push_back(lob);
        UnitDesc a = t.w.data.units[t.attacker];
        a.weapon_projectiles = {0, 1};
        const uint32_t ad = push_unit(t.w.data, a);
        t.w.make();
        t.w.add(1, ad, 0, Vec3{0, 0, 0});
        t.w.add(2, t.attacker, 0, Vec3{100, 0, 0});
        const float r0 = t.w.sys->attack_range(1);
        CHECK_F(r0, 1000.0f);
        CHECK(t.w.sys->weapon_index(1) == 0 && t.w.sys->current_weapon_projectile(1) == 0);
        StateHasher h0;
        t.w.sys->hash_extended(h0);
        CHECK(t.w.sys->set_weapon_index(1, 1) && t.w.sys->current_weapon_projectile(1) == 1);
        CHECK(t.w.sys->attack_range(1) != r0);  // lob range
        StateHasher h1;
        t.w.sys->hash_extended(h1);
        CHECK(h0.digest() != h1.digest());
        CHECK(t.w.sys->set_weapon_index(1, 5) && t.w.sys->current_weapon_projectile(1) == -1);
        CHECK_F(t.w.sys->attack_range(1), 1000.0f);  // [port] outside the list: weapon_projectile
        CHECK(t.w.sys->set_weapon_index(2, 1) && t.w.sys->current_weapon_projectile(2) == -1 && t.w.sys->weapon_set_count(2) == 0);
        CHECK(!t.w.sys->set_weapon_index(1, -1) && !t.w.sys->set_weapon_index(99, 0));
    }
}

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

void test_fog_filter() {
    {  // A hidden candidate is skipped by a human shooter; the next one is taken.
        TargetWorld t;
        FogHooks fh;
        fh.hidden = {3};
        t.w.sys->set_hooks(&fh);
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{500, 0, 0});
        t.w.add(3, t.fighter, 1, Vec3{200, 0, 0});  // would win without fog (test_targeting_rules case 1)
        t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 2);
        CHECK(fh.calls.load() > 0);
        // The current target goes into the fog: dropped at the next scan, the revealed fighter is taken; both
        // hidden: no target.
        fh.hidden = {2};
        uint32_t due = t.w.sys->next_scan(1);
        while (t.w.frame <= due) t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 3);
        fh.hidden = {2, 3};
        due = t.w.sys->next_scan(1);
        while (t.w.frame <= due) t.w.tick();
        CHECK(t.w.sys->auto_target(1) == kNoObject);
        t.w.sys->set_hooks(nullptr);
    }
    {  // OI tactical (TACTICAL_GAPS 5.8): the targeting FOW test uses force 1 - an AI shooter skips the hidden fighter too;
       // same for the parallel gather (4 workers).
        TargetWorld t;
        FogHooks fh;
        fh.hidden = {3};
        fh.human0 = false;
        t.w.sys->set_hooks(&fh);
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{500, 0, 0});
        t.w.add(3, t.fighter, 1, Vec3{200, 0, 0});
        t.w.tick(alamo::tactical::testing::make_thread_parallel_for(4));
        CHECK(t.w.sys->auto_target(1) == 2);
        t.w.sys->set_hooks(nullptr);
    }
    {  // Space-station special weapon: first WEAPON_SPECIAL hard point; force 1 refuses hidden targets for AIs too.
        World w;
        w.data.projectiles.push_back(ProjectileDesc{});
        HardpointDesc laser;
        laser.kind = HardpointKind::WeaponLaser;
        w.data.hardpoints.push_back(laser);
        HardpointDesc sw;
        sw.kind = HardpointKind::WeaponSpecial;
        sw.fire_range = 1000.0f;
        sw.fire_min_range = 100.0f;
        sw.max_recharge = 10.0f;
        sw.bone_a = {50.0f, 0.0f, 0.0f};
        w.data.hardpoints.push_back(sw);
        UnitDesc st;
        st.tactical_health = 1000.0f;
        st.hardpoints = {0, 1};
        const uint32_t station = push_unit(w.data, st);
        UnitDesc plain;
        plain.tactical_health = 100.0f;
        plain.radius = 10.0f;
        const uint32_t ship = push_unit(w.data, plain);
        w.make();
        FogHooks fh;
        fh.human0 = false;
        w.sys->set_hooks(&fh);
        w.add(1, station, 0, Vec3{0, 0, 0});
        w.add(2, ship, 1, Vec3{500, 0, 0});
        w.add(3, ship, 1, Vec3{1100, 0, 0});  // 1050 from the bone: beyond 1000 + 10
        w.add(4, ship, 1, Vec3{120, 0, 0});   // 70 from the bone: inside the minimum range
        w.add(5, ship, 0, Vec3{400, 0, 0});   // own unit
        CHECK(w.sys->special_weapon_hardpoint(1) == 1 && w.sys->special_weapon_hardpoint(2) == -1);
        CHECK(w.sys->special_weapon_readiness(2) < 0.0f);
        CHECK(w.sys->special_weapon_can_fire(1, 2));
        CHECK(!w.sys->special_weapon_can_fire(1, 3) && !w.sys->special_weapon_can_fire(1, 4) && !w.sys->special_weapon_can_fire(1, 5));
        fh.hidden = {2};
        CHECK(!w.sys->special_weapon_can_fire(1, 2) && !w.sys->special_weapon_fire(1, 2));
        CHECK(w.sys->ordered_target(1) == kNoObject);
        fh.hidden.clear();
        CHECK(w.sys->special_weapon_fire(1, 2) && w.sys->ordered_target(1) == 2);
        const float r = w.sys->special_weapon_readiness(1);
        CHECK(r >= 0.0f && r <= 1.0f);
        w.sys->set_hooks(nullptr);
    }
}

// refixf t06 (TACTICAL_COMBAT §2.8): Attack_Move on an object. Path B locks the formation goal in weapon range with no
// score / exclusion / fog test; the lock drops on stealth / fog / garrison; path A (Order_Attack) aims the weapon hardpoints
// or, for BARRAGE / team units, locks at once; refusals.
void test_attack_move_object() {
    World w;
    w.data.projectiles.push_back(ProjectileDesc{});
    PrioritySet ps;  // a Frigate_Attack_Move-like set: Capital excluded
    ps.attack.push_back({PriorityEntry::Kind::Category, -1, kCatFighter, 1.0f});
    ps.category_exclusions = kCatCapital;
    w.data.priority_sets.push_back(ps);
    HardpointDesc laser;  // never fires (no projectile)
    laser.kind = HardpointKind::WeaponLaser;
    w.data.hardpoints.push_back(laser);
    UnitAbilityDesc barrage;
    barrage.ability_type = 5;
    w.data.abilities.push_back(barrage);
    UnitDesc a;
    a.tactical_health = 100.0f;
    a.max_attack_distance = 1000.0f;
    a.weapon_projectile = 0;
    a.weapon_recharge = 100.0f;
    a.priority_set = 0;
    const uint32_t attacker = push_unit(w.data, a);
    UnitDesc ah = a;
    ah.weapon_projectile = -1;
    ah.hardpoints = {0};
    ah.abilities = {0, -1};
    const uint32_t hp_attacker = push_unit(w.data, ah);
    UnitDesc am = a;
    am.min_attack_distance = 200.0f;
    const uint32_t min_attacker = push_unit(w.data, am);
    UnitDesc c;
    c.tactical_health = 100.0f;
    c.radius = 10.0f;
    c.category_mask = kCatCapital;
    const uint32_t station = push_unit(w.data, c);
    UnitDesc f = c;
    f.category_mask = kCatFighter;
    const uint32_t fighter = push_unit(w.data, f);
    w.make();
    FogHooks fh;
    fh.human0 = false;
    w.sys->set_hooks(&fh);
    w.add(1, attacker, 0, Vec3{0, 0, 0});
    w.add(2, station, 1, Vec3{1500, 0, 0});
    w.add(3, fighter, 1, Vec3{300, 0, 0});
    w.sys->set_attack_goal(1, 2);
    w.tick();
    // Out of weapon range: the scan picks the fighter; the excluded station is never scored in.
    CHECK(w.sys->auto_target(1) == 3 && w.sys->ordered_target(1) == kNoObject && w.sys->attack_goal(1) == 2);
    // In range (XY 800 <= 1000 + 10): path B locks it although the set excludes Capital; it replaces the auto target.
    w.sys->set_position(2, Vec3{800, 0, 0});
    w.tick();
    CHECK(w.sys->ordered_target(1) == 2 && w.sys->order_locked(1) && w.sys->current_target(1) == 2 && w.sys->auto_target(1) == kNoObject);
    // The lock drops when the target is stealthed (no Target_Stealth_Units) and, at the next service, B locks it again.
    w.sys->set_flag(2, kUnitStealthed, true);
    w.tick();
    CHECK(w.sys->ordered_target(1) == kNoObject && !w.sys->order_locked(1));
    w.sys->set_flag(2, kUnitStealthed, false);
    w.tick();
    CHECK(w.sys->ordered_target(1) == 2 && w.sys->order_locked(1));
    // Hidden by fog for the attacker's player (force 1): dropped.
    fh.hidden = {2};
    w.tick();
    CHECK(w.sys->ordered_target(1) == kNoObject);
    fh.hidden.clear();
    w.tick();
    CHECK(w.sys->ordered_target(1) == 2);
    // A plain order (no lock) is not dropped by fog; B is idle while any ordered target is held.
    w.sys->order_attack(1, 3);
    CHECK(!w.sys->order_locked(1));
    fh.hidden = {3};
    w.tick();
    CHECK(w.sys->ordered_target(1) == 3);
    fh.hidden.clear();
    // The goal dies: the lock and the goal are gone.
    w.sys->clear_order(1);
    w.tick();
    CHECK(w.sys->ordered_target(1) == 2);
    DamageRequest kill;
    kill.victim = 2;
    kill.amount = 1e9f;
    w.sys->take_damage(kill, w.frame);
    w.tick();
    w.tick();
    CHECK(w.sys->ordered_target(1) != 2 && w.sys->attack_goal(1) == kNoObject);

    // Min range (slot 45): inside Targeting_Min_Attack_Distance + extent no promotion; outside it, promotion.
    w.add(4, min_attacker, 0, Vec3{0, 500, 0});
    w.add(5, station, 1, Vec3{100, 500, 0});
    w.sys->set_attack_goal(4, 5);
    CHECK(w.sys->inside_min_range(4, 5) && w.sys->min_attack_distance(4) == 200.0f && w.sys->base_attack_range_of(4) == 1000.0f);
    w.tick();
    CHECK(w.sys->ordered_target(4) == kNoObject);
    w.sys->set_position(5, Vec3{600, 500, 0});
    CHECK(!w.sys->inside_min_range(4, 5));
    w.tick();
    CHECK(w.sys->ordered_target(4) == 5);

    // Path A (Order_Attack ordered 1): refusals - own unit, stealthed target; then the weapon path aims the hardpoints and
    // leaves the unit's own target alone.
    w.add(6, hp_attacker, 0, Vec3{0, -4000, 0});
    w.add(7, station, 1, Vec3{3000, -4000, 0});
    CHECK(!w.sys->order_attack_issue(6, 4));  // own
    w.sys->set_flag(7, kUnitStealthed, true);
    CHECK(!w.sys->order_attack_issue(6, 7));
    w.sys->set_flag(7, kUnitStealthed, false);
    CHECK(w.sys->order_attack_issue(6, 7));
    CHECK(w.sys->weapon_aim(6) == 7 && w.sys->ordered_target(6) == kNoObject);
    w.tick();
    CHECK(w.sys->weapon_aim(6) == 7);  // nothing in range: the scan picks nothing, the aim holds
    // BARRAGE active in space: the set path locks the target at once.
    w.sys->set_ability_active(6, 0, true);
    CHECK(w.sys->order_attack_issue(6, 7));
    CHECK(w.sys->ordered_target(6) == 7 && w.sys->order_locked(6) && w.sys->weapon_aim(6) == kNoObject);
    w.sys->set_hooks(nullptr);
}

// refixf t06 round 3 (TACTICAL_COMBAT §2.9): damage to hardpointed targets and close-range fire.
void test_hardpoint_fire_routing() {
    auto build = [](World& w, GameMode mode) {
        ProjectileDesc pj;
        pj.max_speed = 50.0f;
        pj.damage = 5.0f;
        w.data.projectiles.push_back(pj);  // 0
        pj.damages_random_hardpoints = true;
        w.data.projectiles.push_back(pj);  // 1
        HardpointDesc gun;
        gun.kind = HardpointKind::WeaponLaser;
        gun.health = 100.0f;
        gun.destroyable = gun.targetable = true;
        gun.projectile = 0;
        gun.randomize_between_fire_bones = false;
        gun.min_recharge = gun.max_recharge = 0.2f;
        gun.fire_range = 2000.0f;
        w.data.hardpoints.push_back(gun);  // 0
        HardpointDesc gen;
        gen.kind = HardpointKind::ShieldGenerator;
        gen.health = 20.0f;
        gen.destroyable = gen.targetable = true;
        gen.attach_offset = Vec3{0.0f, 40.0f, 0.0f};
        w.data.hardpoints.push_back(gen);  // 1
        HardpointDesc dummy;  // DUMMY_ART: not targetable, not destroyable, closest to the shooter
        dummy.kind = HardpointKind::DummyArt;
        dummy.attach_offset = Vec3{-40.0f, 0.0f, 0.0f};
        w.data.hardpoints.push_back(dummy);  // 2
        HardpointDesc eng = gen;
        eng.kind = HardpointKind::Engine;
        eng.attach_offset = Vec3{0.0f, -40.0f, 0.0f};
        w.data.hardpoints.push_back(eng);  // 3
        HardpointDesc gun_r = gun;
        gun_r.projectile = 1;
        w.data.hardpoints.push_back(gun_r);  // 4
        HardpointDesc gun_c = gun;  // a model fire frame along +x, 90 x 90 cone
        gun_c.has_fire_frame = true;
        gun_c.fire_x = Vec3{1.0f, 0.0f, 0.0f};
        gun_c.fire_y = Vec3{0.0f, 1.0f, 0.0f};
        gun_c.fire_z = Vec3{0.0f, 0.0f, 1.0f};
        gun_c.fire_cone_width = gun_c.fire_cone_height = 90.0f;
        w.data.hardpoints.push_back(gun_c);  // 5
        UnitDesc sh;
        sh.tactical_health = 100.0f;
        sh.max_attack_distance = 2000.0f;
        sh.hardpoints = {0};
        push_unit(w.data, sh);  // 0
        sh.hardpoints = {4};
        push_unit(w.data, sh);  // 1
        sh.hardpoints = {5};
        push_unit(w.data, sh);  // 2
        UnitDesc st;
        st.tactical_health = 1000.0f;
        st.radius = 50.0f;
        st.hardpoints = {1, 2, 3};
        st.destroy_when_all_hardpoints_destroyed = true;
        push_unit(w.data, st);  // 3
        CombatConfig cfg;
        cfg.mode = mode;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
    };
    {  // Ordered hardpoint (Engine) takes every hit; after it dies the per-shot fallback skips the non-targetable dummy,
       // the hull follows the hardpoint fraction + 0.2, and the station dies with its last destroyable hardpoint.
        World w;
        build(w, GameMode::Space);
        CHECK(w.sys->closest_hardpoint(ObjectId{0}, Vec3{}, true) == -1);  // unknown id
        w.add(1, 0, 0, Vec3{-600.0f, 0.0f, 0.0f});
        w.add(2, 3, 1, Vec3{0.0f, 0.0f, 0.0f});
        CHECK(w.sys->closest_hardpoint(2, Vec3{-600.0f, 0.0f, 0.0f}, false) == 1);  // the dummy
        CHECK(w.sys->closest_hardpoint(2, Vec3{-600.0f, 0.0f, 0.0f}, true) == 0);   // targetable only: the generator
        w.sys->order_attack(1, 2, 2);
        for (int i = 0; i < 20 && w.sys->hardpoint_health(2, 2) == 20.0f; ++i) w.tick();
        CHECK(w.sys->hardpoint_health(2, 2) < 20.0f && w.sys->hardpoint_health(2, 0) == 20.0f);
        for (int i = 0; i < 200 && !w.sys->hardpoint_destroyed(2, 2); ++i) w.tick();
        CHECK(w.sys->hardpoint_destroyed(2, 2) && w.sys->hardpoint_health(2, 0) == 20.0f);
        w.tick();
        CHECK(w.sys->health(2) <= (0.5f + 0.2f) * 1000.0f + 0.01f);  // hull <= hardpoint fraction + 0.2
        for (int i = 0; i < 400 && w.sys->alive(2); ++i) w.tick();
        CHECK(!w.sys->alive(2) && w.sys->hardpoint_destroyed(2, 0));
    }
    {  // Damages_Random_Hard_Points: the targeted hardpoint still wins (the draw is made, the pick is overridden).
        World w;
        build(w, GameMode::Space);
        w.add(1, 1, 0, Vec3{-600.0f, 0.0f, 0.0f});
        w.add(2, 3, 1, Vec3{0.0f, 0.0f, 0.0f});
        w.sys->order_attack(1, 2, 2);
        for (int i = 0; i < 60 && !w.sys->hardpoint_destroyed(2, 2); ++i) w.tick();
        CHECK(w.sys->hardpoint_destroyed(2, 2) && w.sys->hardpoint_health(2, 0) == 20.0f);
    }
    {  // Fire-bone frame cone: a target behind the bone's +x axis gets no shot; in front it does.
        World w;
        build(w, GameMode::Space);
        w.add(1, 2, 0, Vec3{-600.0f, 0.0f, 0.0f}, Vec3{-1.0f, 0.0f, 0.0f});
        w.add(2, 3, 1, Vec3{0.0f, 0.0f, 0.0f});
        w.sys->order_attack(1, 2);
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(w.sys->projectile_count() == 0 && w.sys->hardpoint_health(2, 0) == 20.0f && w.sys->hardpoint_health(2, 2) == 20.0f);
        w.sys->set_facing(1, Vec3{1.0f, 0.0f, 0.0f});
        for (int i = 0; i < 30; ++i) w.tick();
        CHECK(w.sys->hardpoint_health(2, 0) < 20.0f || w.sys->hardpoint_health(2, 2) < 20.0f);
    }
    {  // Service_Space: a formation unit (attack goal) promoted without a goal hardpoint gets one by
       // Choose_Target_Hardpoint (no set: the closest live targetable = the generator); land has no such step.
        for (const GameMode mode : {GameMode::Space, GameMode::Land}) {
            World w;
            build(w, mode);
            w.add(1, 0, 0, Vec3{-600.0f, 0.0f, 0.0f});
            w.add(2, 3, 1, Vec3{0.0f, 0.0f, 0.0f});
            w.sys->set_attack_goal(1, 2);
            w.tick();
            CHECK(w.sys->ordered_target(1) == 2);
            CHECK(w.sys->ordered_hardpoint(1) == (mode == GameMode::Space ? 0 : -1));
        }
    }
}

// refixf t07: the host's precise test decides each Query_Ray entry (TACTICAL_COMBAT §3.4); a hit's mesh name routes the
// damage to the hardpoint whose Collision_Mesh it is (§2.9 Q1 step 4), no name -> the hull.
void test_precise_hit_hook() {
    World w;
    ProjectileDesc pj;
    pj.max_speed = 50.0f;
    pj.damage = 5.0f;
    w.data.projectiles.push_back(pj);  // 0 (Projectile_Does_Shield_Damage: mask 7)
    pj.does_shield_damage = false;
    w.data.projectiles.push_back(pj);  // 1 (mask 5)
    HardpointDesc gun;
    gun.kind = HardpointKind::WeaponLaser;
    gun.health = 100.0f;
    gun.destroyable = gun.targetable = true;
    gun.projectile = 0;
    gun.randomize_between_fire_bones = false;
    gun.min_recharge = gun.max_recharge = 0.2f;
    gun.fire_range = 2000.0f;
    w.data.hardpoints.push_back(gun);  // 0
    HardpointDesc plate;  // destroyable, not targetable: shots carry no target hardpoint
    plate.kind = HardpointKind::DummyArt;
    plate.health = 100.0f;
    plate.destroyable = true;
    plate.collision_mesh = "PLATE";
    w.data.hardpoints.push_back(plate);  // 1
    gun.projectile = 1;
    w.data.hardpoints.push_back(gun);  // 2
    UnitDesc sh;
    sh.tactical_health = 100.0f;
    sh.max_attack_distance = 2000.0f;
    sh.hardpoints = {0};
    push_unit(w.data, sh);  // 0
    sh.hardpoints = {2};
    push_unit(w.data, sh);  // 1
    UnitDesc st;
    st.tactical_health = 1000.0f;
    st.radius = 50.0f;
    st.hardpoints = {1};
    push_unit(w.data, st);  // 2
    CombatConfig cfg;
    cfg.mode = GameMode::Space;
    cfg.object_max_health_multiplier = 1.0f;
    cfg.hardpoint_health_multiplier = 1.0f;
    cfg.inaccuracy_enabled = false;
    w.make(cfg);
    w.add(1, 0, 0, Vec3{-600.0f, 0.0f, 0.0f});
    w.add(2, 2, 1, Vec3{0.0f, 0.0f, 0.0f});
    w.add(3, 1, 0, Vec3{-600.0f, 100.0f, 0.0f});
    int calls = 0;
    uint32_t masks = 0;
    w.sys->set_precise_hit([&](const PreciseQuery& q, PreciseResult&) {
        ++calls;
        masks |= 1u << q.mask;
        CHECK(q.victim == 2 && q.collide);
        return PreciseOutcome::Miss;
    });
    w.sys->order_attack(1, 2);
    w.sys->order_attack(3, 2);
    for (int i = 0; i < 60; ++i) w.tick();
    CHECK(calls > 0 && masks == ((1u << 7) | (1u << 5)));  // both query masks seen
    CHECK(w.sys->health(2) == 1000.0f && w.sys->hardpoint_health(2, 0) == 100.0f);  // every entry rejected
    static const std::string kPlate = "PLATE", kHull = "HULL";
    w.sys->set_precise_hit([](const PreciseQuery&, PreciseResult& r) {
        r.t = 0.5f;
        r.mesh = &kPlate;
        return PreciseOutcome::Hit;
    });
    for (int i = 0; i < 60 && w.sys->hardpoint_health(2, 0) == 100.0f; ++i) w.tick();
    CHECK(w.sys->hardpoint_health(2, 0) < 100.0f && w.sys->health(2) == 1000.0f);  // the named hardpoint takes it all
    const float hp = w.sys->hardpoint_health(2, 0);
    w.sys->set_precise_hit([](const PreciseQuery&, PreciseResult& r) {
        r.t = 0.5f;
        r.mesh = &kHull;  // names no hardpoint
        return PreciseOutcome::Hit;
    });
    for (int i = 0; i < 60 && w.sys->health(2) == 1000.0f; ++i) w.tick();
    CHECK(w.sys->health(2) < 1000.0f && w.sys->hardpoint_health(2, 0) <= hp);
    // NotModelled: the tree-box hit stands; the mesh_hardpoint stand-in (bone within 40 of the impact) misses -> hull.
    const float hull = w.sys->health(2);
    w.sys->set_precise_hit([](const PreciseQuery&, PreciseResult&) { return PreciseOutcome::NotModelled; });
    for (int i = 0; i < 60 && w.sys->health(2) == hull; ++i) w.tick();
    CHECK(w.sys->health(2) < hull);
}

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

void test_take_damage() {
    {  // Shields absorb in shield-armor space, the rest converts to hull-armor space.
        DamageWorld d;
        d.w.add(1, d.shielded, 1, Vec3{});
        CHECK_F(d.w.sys->shield(1), 20.0f);
        DamageResult r = d.w.sys->take_damage(d.req(1, 100.0f), 10);
        CHECK_F(r.absorbed, 20.0f);   // min(20, 100 x 0.5)
        CHECK_F(r.to_hull, 120.0f);   // (50 - 20) / 0.5 x 2
        CHECK_F(d.w.sys->health(1), 880.0f);
        CHECK_F(d.w.sys->shield(1), 0.0f);
        // Depleted lockout: no absorption for 5 s even with shield points back.
        d.w.sys->set_shield(1, 10.0f);
        r = d.w.sys->take_damage(d.req(1, 100.0f), 10 + 149);
        CHECK_F(r.absorbed, 0.0f);
        CHECK_F(r.to_hull, 200.0f);
        r = d.w.sys->take_damage(d.req(1, 100.0f), 10 + 150);
        CHECK_F(r.absorbed, 10.0f);
        CHECK_F(r.to_hull, 160.0f);  // (50 - 10) / 0.5 x 2
        // Kind 22 bypasses shields.
        d.w.sys->set_shield(1, 20.0f);
        r = d.w.sys->take_damage(d.req(1, 10.0f, kKindBypassShield), 1000);
        CHECK_F(r.absorbed, 0.0f);
        CHECK_F(r.to_hull, 20.0f);
        // Projectile without Projectile_Does_Shield_Damage: shields do not absorb.
        DamageRequest pr = d.req(1, 10.0f);
        pr.source_is_projectile = true;
        pr.projectile_desc = 2;
        r = d.w.sys->take_damage(pr, 1000);
        CHECK_F(r.absorbed, 0.0f);
        CHECK_F(r.to_hull, 20.0f);
        // Projectile_Does_Hitpoint_Damage false: shields absorb, hull takes nothing.
        pr.projectile_desc = 1;
        d.w.sys->set_shield(1, 1.0f);
        r = d.w.sys->take_damage(pr, 1000);
        CHECK_F(r.absorbed, 1.0f);
        CHECK_F(r.to_hull, 0.0f);
    }
    {  // Multiplier order: TAKE (victim) x CAUSE (projectile shooter) x Get_Damage_Multiplier.
        DamageWorld d;
        d.w.add(1, d.attacker, 0, Vec3{});
        d.w.add(2, d.plain, 1, Vec3{});
        d.w.sys->set_ability_active(1, 0, true);
        d.w.sys->set_ability_active(2, 0, true);
        d.w.sys->player_modifiers(0).insert(ModList::Damage, 1, 0.25f, kPermanent);
        d.w.sys->unit_modifiers(2)->insert(ModList::Defense, 1, 0.9f, kPermanent);  // clamped to 0.75
        d.w.tick();  // refresh caches
        DamageRequest r = d.req(2, 100.0f);
        r.damage_type = kNoTypeId;
        r.source = 1;
        r.source_is_projectile = true;
        r.projectile_desc = 0;
        DamageResult res = d.w.sys->take_damage(r, d.w.frame);
        CHECK_F(res.after_multipliers, 93.75f);  // 100 x 1.5 x 2 x (1.25 x 0.25)
        CHECK_F(res.to_hull, 93.75f);
        // Non-projectile source without the +0x3a1 bit: CAUSE does not apply.
        r.source_is_projectile = false;
        r.projectile_desc = -1;
        res = d.w.sys->take_damage(r, d.w.frame);
        CHECK_F(res.after_multipliers, 46.875f);
    }
    {  // AI damage multiplier and the gates.
        DamageWorld d;
        PlayerInfo ai;
        ai.ai = true;
        ai.ai_damage_multiplier = 0.5f;
        d.w.make(d.w.sys->config(), {ai, PlayerInfo{}});
        d.w.add(1, d.attacker, 0, Vec3{});
        d.w.add(2, d.plain, 1, Vec3{});
        DamageRequest r = d.req(2, 100.0f);
        r.damage_type = kNoTypeId;
        r.source = 1;
        CHECK_F(d.w.sys->take_damage(r, 5).to_hull, 50.0f);
        {  // A1_READINGS §30.2: hull_lost = hullBefore - max(0, hullAfter); a 50-hull victim hit for 200 loses 50
            DamageWorld k;
            k.w.make(k.w.sys->config(), {ai, PlayerInfo{}});
            k.w.add(1, k.attacker, 0, Vec3{});
            k.w.add(2, k.plain, 1, Vec3{});
            k.w.sys->set_health(2, 50.0f);
            DamageRequest kr = r;
            kr.amount = 400.0f;  // x 0.5 AI multiplier = 200
            const DamageResult kres = k.w.sys->take_damage(kr, 5);
            CHECK_F(kres.to_hull, 200.0f);
            CHECK_F(kres.hull_lost, 50.0f);
            CHECK(kres.killed);
            CHECK(!k.w.sys->damage_events().empty());
            if (!k.w.sys->damage_events().empty()) CHECK_F(k.w.sys->damage_events().back().hull_lost, 50.0f);
            k.w.add(3, k.plain, 1, Vec3{});
            k.w.sys->set_health(3, 50.0f);
            kr.victim = 3;
            kr.amount = 40.0f;  // not lethal: the full 20
            CHECK_F(k.w.sys->take_damage(kr, 5).hull_lost, 20.0f);
            k.w.sys->set_flag(3, kUnitCannotDie, true);  // cannot die: the restore ran, no hull loss
            kr.amount = 1000.0f;
            CHECK_F(k.w.sys->take_damage(kr, 5).hull_lost, 0.0f);
        }
        d.w.sys->set_flag(2, kUnitInvulnerable, true);
        CHECK(!d.w.sys->take_damage(r, 5).applied);
        r.kind = kKindDebug;
        CHECK(d.w.sys->take_damage(r, 5).applied);
        d.w.sys->set_flag(2, kUnitInvulnerable, false);
        // Cannot die: health stays at max(1, old).
        d.w.sys->set_flag(2, kUnitCannotDie, true);
        d.w.sys->set_health(2, 30.0f);
        r.kind = kKindNormal;
        r.amount = 1000.0f;
        d.w.sys->take_damage(r, 5);
        CHECK_F(d.w.sys->health(2), 30.0f);
        d.w.sys->set_health(2, 0.5f);
        d.w.sys->take_damage(r, 5);
        CHECK_F(d.w.sys->health(2), 1.0f);
        d.w.sys->set_flag(2, kUnitCannotDie, false);
        const DamageResult k = d.w.sys->take_damage(r, 5);
        CHECK(k.killed);
        CHECK(!d.w.sys->alive(2));
        CHECK(d.w.sys->deaths().size() == 1 && d.w.sys->deaths()[0].id == 2 && d.w.sys->deaths()[0].killer == 1);
    }
    {  // Hardpoint routing: a named live destroyable hardpoint takes ALL the damage; no clamp;
       // killing the last one of a type with Should_Be_Destroyed_When_All_Hardpoints_Destroyed kills the hull.
        DamageWorld d;
        d.w.add(1, d.hp_unit, 1, Vec3{});
        DamageRequest r = d.req(1, 60.0f);
        r.damage_type = kNoTypeId;
        r.hardpoint = 0;
        DamageResult res = d.w.sys->take_damage(r, 5);
        CHECK_F(res.to_hardpoint, 60.0f);
        CHECK_F(res.to_hull, 0.0f);
        CHECK_F(d.w.sys->hardpoint_health(1, 0), 40.0f);
        CHECK_F(d.w.sys->health(1), 500.0f);
        r.amount = 50.0f;
        d.w.sys->take_damage(r, 5);
        CHECK_F(d.w.sys->hardpoint_health(1, 0), -10.0f);  // no clamp
        CHECK(d.w.sys->hardpoint_destroyed(1, 0));
        res = d.w.sys->take_damage(r, 5);  // refixf t06 (§2.9 Q1 step 5): a dead destroyable hardpoint swallows the hit
        CHECK_F(res.to_hull, 0.0f);
        CHECK_F(res.to_hardpoint, 0.0f);
        CHECK_F(d.w.sys->health(1), 500.0f);
        r.hardpoint = 1;
        r.amount = 100.0f;
        res = d.w.sys->take_damage(r, 5);
        CHECK(d.w.sys->hardpoint_destroyed(1, 1));
        CHECK(res.killed);
        CHECK(!d.w.sys->alive(1));
    }
}

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
        const CrcLog par = b.run(kTicks, alamo::tactical::testing::make_thread_parallel_for(threads));
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

std::vector<uint8_t> combat_bytes(const CombatSystem& s) {
    ByteWriter w;
    s.write(w);
    return w.bytes();
}

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
    const ParallelFor pf4 = alamo::tactical::testing::make_thread_parallel_for(4);
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

void put_u32_at(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    for (int i = 0; i < 4; ++i) v[off + static_cast<size_t>(i)] = static_cast<uint8_t>(x >> (8 * i));
}

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
    for (const uint32_t v : {11u, 13u}) {  // s78h t03: the version before (damage_team = 11; squad_seen dropped = 12) and after
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

uint64_t rocket_digest(const CombatSystem& s, const SimRng& rng) {
    StateHasher h;
    s.hash_projectiles(h);
    s.hash_extended(h);
    s.hash_units(h);
    rng.hash(h);
    return h.digest();
}

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
        const ParallelFor pf4 = alamo::tactical::testing::make_thread_parallel_for(4);
        const ParallelFor pf8 = alamo::tactical::testing::make_thread_parallel_for(8);
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
        const ParallelFor pf4 = alamo::tactical::testing::make_thread_parallel_for(4);
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

void test_hardpoint_mode_default() {
    // S8 t12 (b): Fire_When_In_<Mode>_Mode defaults to 1 - without the No the hardpoint fires in DEFEND.
    GateWorld g(true);
    g.w.data.hardpoints[0].no_fire_modes.clear();
    CHECK(g.run(90) > 0);
}

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

// The directions of shooter 1's projectiles spawned this tick (not stepped yet), serial order.
std::vector<Vec3> own_shot_dirs(const OwnWorld& o) {
    std::vector<Vec3> out;
    for (size_t i = 0; i < o.w.sys->projectile_count(); ++i) {
        const ProjectileView v = o.w.sys->projectile(i);
        if (v.shooter == 1) out.push_back(v.dir);
    }
    return out;
}

bool same_dir(Vec3 d, Vec3 to) {
    const float len = std::sqrt(to.x * to.x + to.y * to.y + to.z * to.z);
    return std::fabs(d.x - to.x / len) < 1.0e-4f && std::fabs(d.y - to.y / len) < 1.0e-4f && std::fabs(d.z - to.z / len) < 1.0e-4f;
}

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

float yaw_of(Vec3 f) { return std::atan2(f.y, f.x) * 57.29578f; }

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

void test_team_handover_and_damage() {
    // s78g t06 (TACTICAL_COMBAT §3.1.1 "Team targeting copies", A1_READINGS §29.6): the original's team head is the
    // container, which a member's death does not touch. The port's stand-in team_leader hands over at the death itself:
    // the next member (ascending id) leads at once and keeps the team's target and state (no headless rest of the tick).
    {
        World w;
        ProjectileDesc p;
        p.max_speed = 20.0f;
        w.data.projectiles.push_back(p);
        UnitDesc s;
        s.tactical_health = 100.0f;
        s.max_attack_distance = 300.0f;
        s.weapon_projectile = 0;
        s.weapon_recharge = 0.2f;
        s.turn_rate = 10.0f;
        push_unit(w.data, s);
        UnitDesc t;
        t.tactical_health = 1.0e7f;
        t.radius = 5.0f;
        push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
        w.add(2, 1, 1, Vec3{200, 5, 0});
        for (const ObjectId id : {ObjectId{10}, ObjectId{11}, ObjectId{12}}) {
            UnitSpawn sp;
            sp.id = id;
            sp.desc = 0;
            sp.owner = 0;
            sp.position = Vec3{0, static_cast<float>(id - 10) * 10.0f, 0};
            sp.team = 7;
            sp.team_leader = id == 10;
            CHECK(w.sys->add_unit(sp, w.frame, w.rng));
            TreeMembership m;
            m.owner = 0;
            CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(sp.position, 1.0f), 1u, m));
        }
        w.tick();
        CHECK(w.sys->auto_target(10) == 2 && w.sys->current_target(12) == 2 && w.sys->targeting_state(10) == 4);
        const uint32_t due = w.sys->next_scan(10);
        w.sys->kill_unit(10, kNoObject, kKindNormal, w.frame);  // between ticks, not flushed yet
        // At once: 11 leads with the team's target, state and scan timer; 12 reads 11's target.
        CHECK(w.sys->auto_target(11) == 2 && w.sys->current_target(11) == 2 && w.sys->current_target(12) == 2);
        CHECK(w.sys->targeting_state(11) == 4 && w.sys->next_scan(11) == due);
        w.tick();
        CHECK(!w.sys->alive(10) && w.sys->current_target(11) == 2 && w.sys->current_target(12) == 2);
        CHECK(w.sys->auto_target(12) == kNoObject && w.sys->targeting_state(12) == 4);
    }
    // §3.6 step 2 has no mode gate: a space squadron member (team 0, damage_team = its squadron) with
    // Redirect_Damage_To_Teammates passes amount / (n - 1) to each other live member and takes none itself.
    {
        World w;
        UnitDesc v;
        v.tactical_health = 1000.0f;
        const uint32_t plain = push_unit(w.data, v);
        v.redirect_damage_to_teammates = true;
        const uint32_t redirect = push_unit(w.data, v);
        w.make();
        auto add = [&](ObjectId id, uint32_t desc, uint32_t damage_team) {
            UnitSpawn sp;
            sp.id = id;
            sp.desc = desc;
            sp.owner = 0;
            sp.position = Vec3{static_cast<float>(id) * 50.0f, 0, 0};
            sp.damage_team = damage_team;
            CHECK(w.sys->add_unit(sp, w.frame, w.rng));
        };
        add(10, redirect, 9);
        add(11, plain, 9);
        add(12, plain, 9);
        add(13, plain, 0);   // control: no team, takes 50 itself
        add(14, redirect, 0);  // no team: the rule does not apply
        const float h0 = w.sys->health(10);
        auto hit = [&](ObjectId id, float amount) {
            DamageRequest r;
            r.victim = id;
            r.amount = amount;
            w.sys->take_damage(r, 100);
        };
        hit(13, 50.0f);
        const float loss = h0 - w.sys->health(13);
        CHECK(loss > 0.0f);
        hit(10, 100.0f);
        CHECK_F(w.sys->health(10), h0);
        CHECK_F(w.sys->health(11), h0 - loss);
        CHECK_F(w.sys->health(12), h0 - loss);
        hit(14, 50.0f);
        CHECK_F(w.sys->health(14), h0 - loss);
        // Save round trip (combat save v11+): damage_team survives a load; the same redirect hit then gives the same
        // result in the saved and the loaded system.
        World b;
        b.data = w.data;
        b.make();
        const std::vector<uint8_t> saved = combat_bytes(*w.sys);
        ByteReader rd(saved);
        CHECK(b.sys->read(rd));
        CHECK(combat_bytes(*b.sys) == saved);
        DamageRequest r;
        r.victim = 10;
        r.amount = 60.0f;
        w.sys->take_damage(r, 200);
        b.sys->take_damage(r, 200);
        CHECK_F(w.sys->health(10), h0);
        CHECK_F(b.sys->health(10), h0);
        CHECK(w.sys->health(11) < h0 - loss);
        for (const ObjectId id : {ObjectId{10}, ObjectId{11}, ObjectId{12}}) CHECK_F(b.sys->health(id), w.sys->health(id));
        CHECK(combat_bytes(*b.sys) == combat_bytes(*w.sys));
    }
    // Review (s78g t06): a phase-D death. Leader 10's due scan would keep its target 2 (scored 1.0); a closer fighter 40
    // (0.5, better) appeared since. Ally 30 (higher index, serviced first) kills 10 in its unit_begin in the tick the
    // team's scan is due. The heir 11 is re-gathered on the copied record, so it runs the keep test and keeps 2, as the
    // container would; the member's own gather (always due, no target to keep) would have rescanned and taken 40.
    {
        struct KillHooks final : CombatHooks {
            CombatSystem* sys = nullptr;
            bool armed = false;
            void unit_begin(ObjectId id, uint32_t frame, SimRng&) override {
                if (armed && id == 30) {
                    armed = false;
                    sys->kill_unit(10, kNoObject, kKindNormal, frame);
                }
            }
        };
        // The same tick's synced draws with and without the hand-over: equal (one reschedule draw, no rescan's
        // start-player draw).
        auto run = [](bool kill) -> uint64_t {
            World w;
            KillHooks hk;
            ProjectileDesc p;
            p.max_speed = 20.0f;
            w.data.projectiles.push_back(p);
            PrioritySet ps;
            ps.attack.push_back({PriorityEntry::Kind::Category, -1, kCatCapital, 1.0f});
            ps.attack.push_back({PriorityEntry::Kind::Category, -1, kCatFighter, 0.5f});
            w.data.priority_sets.push_back(ps);
            UnitDesc s;
            s.tactical_health = 100.0f;
            s.max_attack_distance = 300.0f;
            s.weapon_projectile = 0;
            s.weapon_recharge = 100.0f;  // never fires during the test
            s.turn_rate = 10.0f;
            s.priority_set = 0;
            const uint32_t sd = push_unit(w.data, s);
            UnitDesc t;
            t.tactical_health = 1.0e7f;
            t.radius = 5.0f;
            t.category_mask = kCatCapital;
            const uint32_t cd = push_unit(w.data, t);
            t.category_mask = kCatFighter;
            const uint32_t fd = push_unit(w.data, t);
            UnitDesc ally;
            ally.tactical_health = 100.0f;
            const uint32_t ad = push_unit(w.data, ally);
            CombatConfig cfg;
            cfg.mode = GameMode::Land;
            cfg.inaccuracy_enabled = false;
            w.make(cfg);
            hk.sys = w.sys.get();
            w.sys->set_hooks(&hk);
            w.add(2, cd, 1, Vec3{250, 5, 0});
            for (const ObjectId id : {ObjectId{10}, ObjectId{11}}) {
                UnitSpawn sp;
                sp.id = id;
                sp.desc = sd;
                sp.owner = 0;
                sp.position = Vec3{0, static_cast<float>(id - 10) * 10.0f, 0};
                sp.team = 7;
                sp.team_leader = id == 10;
                CHECK(w.sys->add_unit(sp, w.frame, w.rng));
                TreeMembership m;
                m.owner = 0;
                CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(sp.position, 1.0f), 1u, m));
            }
            w.add(30, ad, 0, Vec3{0, -50, 0});
            w.tick();
            CHECK(w.sys->auto_target(10) == 2);  // scored 1.0 (capital), so the due scan keeps it
            const uint32_t due = w.sys->next_scan(10);
            CHECK(due > w.frame);
            w.add(40, fd, 1, Vec3{100, 0, 0});
            while (w.frame < due) w.tick();
            CHECK(w.sys->auto_target(10) == 2);  // no scan of the leader before its due frame
            hk.armed = kill;
            const uint64_t d0 = w.rng.draws();
            w.tick();  // frame == due: phase C keeps 2 for 10; 30 kills 10 in phase D before 11 is serviced
            const ObjectId head = kill ? 11 : 10;
            CHECK(w.sys->alive(10) == !kill);
            CHECK(w.sys->auto_target(head) == 2 && w.sys->current_target(11) == 2);
            CHECK(w.sys->next_scan(head) > due);  // the team's scan ran (keep test), its timer redrawn
            return w.rng.draws() - d0;
        };
        CHECK(run(true) == run(false));
    }
}

void test_team_targeting_copy() {
    // s78f t03 (§3.1.1 "Team targeting copies"), land: the members of a ground team have no TARGETING. The head (here the
    // team_leader member standing in for the container) scans; the members read its target, get its state raw, any
    // member off the target makes the team state 3, and each member runs slot 47.
    struct Team {
        World w;
        StateHooks h;
        Team(Vec3 target, Vec3 member_facing) {
            ProjectileDesc p;
            p.max_speed = 20.0f;
            w.data.projectiles.push_back(p);
            UnitDesc s;
            s.tactical_health = 100.0f;
            s.max_attack_distance = 300.0f;
            s.weapon_projectile = 0;
            s.weapon_recharge = 0.2f;
            s.turn_rate = 10.0f;
            push_unit(w.data, s);
            UnitDesc t;
            t.tactical_health = 1.0e7f;
            t.radius = 5.0f;
            push_unit(w.data, t);
            CombatConfig cfg;
            cfg.mode = GameMode::Land;
            cfg.inaccuracy_enabled = false;
            w.make(cfg);
            w.sys->set_hooks(&h);
            w.add(2, 1, 1, target);
            for (const ObjectId id : {ObjectId{10}, ObjectId{11}}) {
                UnitSpawn sp;
                sp.id = id;
                sp.desc = 0;
                sp.owner = 0;
                sp.position = Vec3{0, id == 10 ? 0.0f : 10.0f, 0};
                sp.facing = id == 11 ? member_facing : Vec3{1, 0, 0};
                sp.team = 7;
                sp.team_leader = id == 10;
                CHECK(w.sys->add_unit(sp, w.frame, w.rng));
                TreeMembership m;
                m.owner = 0;
                CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(sp.position, 1.0f), 1u, m));
            }
        }
    };
    {  // every member on the target: the head's scan target, state 4 copied, both fire, one slot 47 per member
        Team t(Vec3{200, 5, 0}, Vec3{1, 0, 0});
        t.w.tick();
        CHECK(t.w.sys->auto_target(10) == 2 && t.w.sys->auto_target(11) == kNoObject && t.w.sys->current_target(11) == 2);
        CHECK(t.w.sys->targeting_state(10) == 4 && t.w.sys->targeting_state(11) == 4);
        size_t n10 = 0, n11 = 0;
        for (const auto& [id, arg] : t.h.idle) (id == 10 ? n10 : n11) += arg ? 1u : 0u;
        CHECK(n10 == 1 && n11 == 1);
        bool shot10 = false, shot11 = false;
        for (int i = 0; i < 60; ++i) {
            t.w.tick();
            for (size_t k = 0; k < t.w.sys->projectile_count(); ++k) {
                shot10 = shot10 || t.w.sys->projectile(k).shooter == 10;
                shot11 = shot11 || t.w.sys->projectile(k).shooter == 11;
            }
        }
        CHECK(shot10 && shot11);
    }
    {  // the member faces away: the team is in state 3 (slot 47(0) on the members) until it has turned, then 4 for all
        Team t(Vec3{200, 5, 0}, Vec3{-1, 0, 0});
        t.w.tick();
        CHECK(t.w.sys->targeting_state(10) == 3 && t.w.sys->targeting_state(11) == 3);
        CHECK(!t.h.idle.empty() && t.h.idle.front() == std::make_pair(ObjectId{11}, false));
        int ticks = 1;
        while (t.w.sys->targeting_state(11) != 4 && ticks < 40) {
            t.w.tick();
            ++ticks;
        }
        CHECK(ticks == 18 && t.w.sys->targeting_state(10) == 4);  // 178 degrees at 10 per service
    }
    {  // s78f review: a member that picked its own target before its head existed drops it when it becomes a member, so
       // it reads the head's target and never keeps a dead id
        World w;
        ProjectileDesc p;
        p.max_speed = 20.0f;
        w.data.projectiles.push_back(p);
        UnitDesc s;
        s.tactical_health = 100.0f;
        s.max_attack_distance = 300.0f;
        s.weapon_projectile = 0;
        s.weapon_recharge = 0.2f;
        push_unit(w.data, s);
        UnitDesc t;
        t.tactical_health = 1.0e7f;
        t.radius = 5.0f;
        push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = GameMode::Land;
        cfg.inaccuracy_enabled = false;
        w.make(cfg);
        w.add(2, 1, 1, Vec3{200, 5, 0});
        const auto add = [&](ObjectId id, bool leader, Vec3 at) {
            UnitSpawn sp;
            sp.id = id;
            sp.desc = 0;
            sp.owner = 0;
            sp.position = at;
            sp.facing = Vec3{1, 0, 0};
            sp.team = 7;
            sp.team_leader = leader;
            CHECK(w.sys->add_unit(sp, w.frame, w.rng));
            TreeMembership m;
            m.owner = 0;
            CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(sp.position, 1.0f), 1u, m));
        };
        add(11, false, Vec3{0, 10, 0});
        w.tick();
        const bool own = w.sys->auto_target(11) == 2;  // no head yet: its own scan picked the target
        add(12, true, Vec3{0, 0, 0});  // ids ascend: the head joins after the member
        w.tick();
        const bool joined = w.sys->auto_target(11) == kNoObject && w.sys->current_target(11) == w.sys->current_target(12);
        // The target dies: the member holds no own id (the old bug kept returning it forever) and reads the head's slot,
        // which the head's own scan replaces.
        w.sys->kill_unit(2, kNoObject, 0, w.frame);
        for (int i = 0; i < 90; ++i) w.tick();
        const bool dropped = w.sys->auto_target(11) == kNoObject && w.sys->current_target(11) == w.sys->current_target(12) &&
                             w.sys->current_target(12) != 2;
        std::printf("team member join: own target %d, cleared on join %d, dead target dropped %d\n", own ? 1 : 0, joined ? 1 : 0,
                    dropped ? 1 : 0);
        CHECK(own && joined && dropped);
    }
}

void test_squad_value() {
    // s78h t03 (TACTICAL_COMBAT §4.4 "Who the squad leader is"): the squad value belongs to the TEAM container. Every unit,
    // the team_leader stand-in included, keeps its own Mod_Multiplier; team_mod_multiplier(container id) = the container's
    // own product if != 1, else the minimum over ALL live members (stand-in and 1.0-valued members included), 1.0 if none.
    const float members[2] = {1.0f, 0.8f};
    CHECK_F(squad_multiplier(1.0f, members), 0.8f);
    const float high[2] = {1.5f, 1.5f};
    CHECK_F(squad_multiplier(1.0f, high), 1.5f);  // a minimum > 1 is used as is
    const auto run = [](GameMode mode) {
        World w;
        UnitAbilityDesc on;
        on.ability_type = kAbilityPowerToWeapons;
        on.mult[static_cast<size_t>(ModKind::FireRate) - 1] = 0.5f;
        on.mult[static_cast<size_t>(ModKind::Speed) - 1] = 1.5f;
        w.data.abilities.push_back(on);
        UnitAbilityDesc mid = on;
        mid.mult[static_cast<size_t>(ModKind::FireRate) - 1] = 0.8f;
        w.data.abilities.push_back(mid);
        UnitDesc d;
        d.tactical_health = 100.0f;
        d.abilities = {0, -1};
        const uint32_t d_half = push_unit(w.data, d);
        d.abilities = {1, -1};
        const uint32_t d_mid = push_unit(w.data, d);
        CombatConfig cfg;
        cfg.mode = mode;
        w.make(cfg);
        const bool land = mode == GameMode::Land;
        auto add = [&](ObjectId id, uint32_t desc, bool leader) {
            UnitSpawn sp;
            sp.id = id;
            sp.desc = desc;
            sp.owner = 0;
            sp.position = Vec3{static_cast<float>(id) * 10.0f, 0, 0};
            if (land) {
                sp.team = 7;
                sp.team_leader = leader;
            } else {
                sp.damage_team = 7;  // a space squadron member
            }
            CHECK(w.sys->add_unit(sp, w.frame, w.rng));
        };
        add(10, d_half, true);  // the stand-in leader on land; ability inactive: own 1.0
        add(11, d_mid, false);
        add(12, d_half, false);
        w.sys->set_ability_active(11, 0, true);
        w.tick();
        // The leader keeps its own 1.0 (the old phase-A rule gave it the members' 0.8).
        CHECK_F(w.sys->mod_multiplier(10, ModKind::FireRate), 1.0f);
        CHECK_F(w.sys->mod_multiplier(11, ModKind::FireRate), 0.8f);
        CHECK_F(w.sys->mod_multiplier(12, ModKind::FireRate), 1.0f);
        CHECK_F(w.sys->team_mod_multiplier(7, ModKind::FireRate), 0.8f);
        CHECK_F(w.sys->team_mod_multiplier(7, ModKind::Speed), 1.0f);  // 10 and 12 at 1.0 cap the team
        // The stand-in counts in the minimum and keeps its own value.
        w.sys->set_ability_active(10, 0, true);
        w.tick();
        CHECK_F(w.sys->mod_multiplier(10, ModKind::FireRate), 0.5f);
        CHECK_F(w.sys->team_mod_multiplier(7, ModKind::FireRate), 0.5f);
        w.sys->set_ability_active(12, 0, true);
        w.tick();
        CHECK_F(w.sys->team_mod_multiplier(7, ModKind::Speed), 1.5f);  // every member at 1.5
        CHECK_F(w.sys->mod_multiplier(11, ModKind::Speed), 1.5f);
        // Dead members leave the minimum; no live member -> 1.0; an unknown team -> 1.0.
        w.sys->kill_unit(10, kNoObject, kKindNormal, w.frame);
        w.sys->kill_unit(12, kNoObject, kKindNormal, w.frame);
        w.tick();
        CHECK_F(w.sys->team_mod_multiplier(7, ModKind::FireRate), 0.8f);
        w.sys->kill_unit(11, kNoObject, kKindNormal, w.frame);
        w.tick();
        CHECK_F(w.sys->team_mod_multiplier(7, ModKind::FireRate), 1.0f);
        CHECK_F(w.sys->team_mod_multiplier(99, ModKind::FireRate), 1.0f);
    };
    run(GameMode::Land);
    run(GameMode::Space);
}

void test_convert_team() {
    // s78h t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule"): the team is the container (damage_team), in space too,
    // so a converted squadron member converts every mate, wherever it is; a victim already owned by the new owner is
    // skipped, so each object converts (and raises UnitEffect::Converted) once.
    struct ConvertHooks final : CombatHooks {
        std::vector<ObjectId> converted;
        void unit_effect(ObjectId id, UnitEffect e, int32_t, Vec3, uint32_t) override {
            if (e == UnitEffect::Converted) converted.push_back(id);
        }
    };
    for (const GameMode mode : {GameMode::Space, GameMode::Land}) {
        World w;
        ConvertHooks hk;
        ProjectileDesc cv;
        cv.name = "Proj_Convert";
        cv.max_speed = 10.0f;
        cv.convert = true;
        cv.convert_radius = 100.0f;
        w.data.projectiles.push_back(cv);
        UnitDesc d;
        d.tactical_health = 100.0f;
        const uint32_t ud = push_unit(w.data, d);
        CombatConfig cfg;
        cfg.mode = mode;
        w.make(cfg);
        w.sys->set_hooks(&hk);
        auto add = [&](ObjectId id, Vec3 at, uint32_t team, bool leader) {
            UnitSpawn sp;
            sp.id = id;
            sp.desc = ud;
            sp.owner = 1;
            sp.position = at;
            if (mode == GameMode::Land) {
                sp.team = team;
                sp.team_leader = leader;
            } else {
                sp.damage_team = team;
            }
            CHECK(w.sys->add_unit(sp, w.frame, w.rng));
            TreeMembership m;
            m.owner = 1;
            CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(at, 1.0f), 1u, m));
        };
        add(10, Vec3{10, 0, 0}, 9, true);
        add(11, Vec3{20, 0, 0}, 9, false);
        add(12, Vec3{30, 0, 0}, 9, false);
        add(13, Vec3{900, 0, 0}, 9, false);  // out of the radius: converts with its team
        add(14, Vec3{0, 20, 0}, 0, false);   // no team, in the radius
        add(15, Vec3{950, 0, 0}, 0, false);  // no team, out of the radius
        ProjectileLaunch l;
        l.owner = 0;
        l.projectile = 0;
        l.from = Vec3{0, 0, 0};
        l.point = l.from;
        l.fuse_seconds = 0.0f;
        CHECK(w.sys->launch_projectile(l, w.frame));
        w.tick();
        w.tick();
        for (const ObjectId id : {ObjectId{10}, ObjectId{11}, ObjectId{12}, ObjectId{13}, ObjectId{14}}) {
            CHECK(w.sys->owner(id) == 0 && (w.sys->flags(id) & kUnitConverted));
            CHECK(std::count(hk.converted.begin(), hk.converted.end(), id) == 1);
        }
        CHECK(w.sys->owner(15) == 1 && !(w.sys->flags(15) & kUnitConverted));
        CHECK(hk.converted.size() == 5);
    }
}

void test_team_container_slot() {
    // s78h t06 (TACTICAL_COMBAT §3.1.1 "When the container is serviced, and a member's death in the same frame"): a land
    // team's TeamTargeting runs once per tick at the container slot, before every member's own service (the descending
    // walk reaches the team's highest index first), and still runs when the stand-in leader dies inside phase D.
    struct OrderHooks final : CombatHooks {
        CombatSystem* sys = nullptr;
        ObjectId kill_at = kNoObject;  // unit_begin of this unit kills `victim`
        ObjectId victim = kNoObject;
        std::vector<std::pair<char, ObjectId>> log;  // 'b' = unit_begin, 'i' = slot 47 (team service)
        void unit_begin(ObjectId id, uint32_t frame, SimRng&) override {
            log.emplace_back('b', id);
            if (id == kill_at) {
                kill_at = kNoObject;
                sys->kill_unit(victim, kNoObject, kKindNormal, frame);
            }
        }
        void attack_idle(ObjectId id, bool, uint32_t, SimRng&) override { log.emplace_back('i', id); }
    };
    struct TeamWorld {
        World w;
        OrderHooks h;
        TeamWorld() {
            ProjectileDesc p;
            p.max_speed = 20.0f;
            w.data.projectiles.push_back(p);
            UnitDesc s;
            s.tactical_health = 100.0f;
            s.max_attack_distance = 300.0f;
            s.weapon_projectile = 0;
            s.weapon_recharge = 100.0f;  // never fires during the test
            s.turn_rate = 10.0f;
            push_unit(w.data, s);
            UnitDesc t;
            t.tactical_health = 1.0e7f;
            t.radius = 5.0f;
            push_unit(w.data, t);
            UnitDesc ally;
            ally.tactical_health = 100.0f;
            push_unit(w.data, ally);
            CombatConfig cfg;
            cfg.mode = GameMode::Land;
            cfg.inaccuracy_enabled = false;
            w.make(cfg);
            h.sys = w.sys.get();
            w.sys->set_hooks(&h);
            w.add(2, 1, 1, Vec3{200, 5, 0});
            for (const ObjectId id : {ObjectId{10}, ObjectId{11}, ObjectId{12}}) {
                UnitSpawn sp;
                sp.id = id;
                sp.desc = 0;
                sp.owner = 0;
                sp.position = Vec3{0, static_cast<float>(id - 10) * 10.0f, 0};
                sp.team = 7;
                sp.team_leader = id == 10;
                CHECK(w.sys->add_unit(sp, w.frame, w.rng));
                TreeMembership m;
                m.owner = 0;
                CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(sp.position, 1.0f), 1u, m));
            }
            w.add(30, 2, 0, Vec3{0, -50, 0});  // highest index: serviced before the team slot
        }
        size_t services() const {  // slot-47 calls this tick: one per live team unit per team service at state 4
            size_t n = 0;
            for (const auto& e : h.log) n += e.first == 'i' ? 1u : 0u;
            return n;
        }
        size_t pos(char c, ObjectId id) const {
            for (size_t i = 0; i < h.log.size(); ++i)
                if (h.log[i] == std::make_pair(c, id)) return i;
            return h.log.size();
        }
    };
    {  // the first tick: the team state and the members' slot 47 come before any member's own service
        TeamWorld t;
        t.w.tick();
        CHECK(t.w.sys->targeting_state(10) == 4 && t.w.sys->targeting_state(12) == 4);
        CHECK(t.services() == 3);
        CHECK(t.pos('b', 30) < t.pos('i', 10));
        for (const ObjectId id : {ObjectId{10}, ObjectId{11}, ObjectId{12}})
            CHECK(t.pos('i', id) < t.pos('b', 12) && t.pos('b', 12) < t.h.log.size());
    }
    // The leader dies at its own slot (old port: the heir's slot had passed, so the team got no service that tick), or at
    // the top member's slot (after the team slot: no second service for the heir); or the top member dies before the slot;
    // or the leader dies before the slot (ally 30's slot): team_head_of gives the heir 11 at the slot, which decides on
    // its re-gathered candidates, and the heir's own slot (now in land_heads_) runs no second service.
    const std::array<std::pair<ObjectId, ObjectId>, 4> cases{{{10, 10}, {12, 10}, {30, 12}, {30, 10}}};
    for (const auto& [at, victim] : cases) {
        TeamWorld t;
        t.w.tick();
        t.h.log.clear();
        t.h.kill_at = at;
        t.h.victim = victim;
        t.w.tick();
        CHECK(!t.w.sys->alive(victim));
        const bool before_slot = at == 30;  // the victim died before the team slot: 2 live team units at the service
        CHECK(t.services() == (before_slot ? 2u : 3u));  // one team service this tick
        CHECK(t.pos('i', 11) < t.pos('b', 11));
        for (size_t i = t.pos('b', 12); i < t.h.log.size(); ++i) CHECK(t.h.log[i].first != 'i');  // all at the slot
        if (victim == 10) CHECK(t.w.sys->auto_target(11) == 2 && t.w.sys->targeting_state(11) == 4);  // the heir leads
        t.h.log.clear();
        t.w.tick();  // the next tick: the team (the heir's after a hand-over) is serviced once
        CHECK(t.services() == 2);
        CHECK(t.w.sys->current_target(11) == 2 && t.w.sys->targeting_state(11) == 4);
    }
}

void test_charge_up_slew() {
    // s78f t04 (LOB_SOLVER §3.6 "Charge-up aim"): the lobbing TURRET slews Turret_Rotate_Speed degrees per service from
    // Turret_Rest_Angle; the charge-up starts on the service where the angles are reached; the turret then holds its aim.
    const auto make = [](float extent) {
        OwnOpt opt;
        opt.lobbing = true;
        opt.recharge = 0.2f;
        opt.edit = [extent](CombatData& d) {
            d.units[0].turret = true;
            d.units[0].lobbing_chargeup_frames = 90;
            d.units[0].turret_rotate_speed = 4.0f;
            d.units[0].turret_extent = extent;
            d.units[0].turret_elevate_extent = 85.0f;
            d.units[0].turret_rest_pitch = -45.0f;
        };
        return std::make_unique<OwnWorld>(opt, Vec3{100, 0, 0});
    };
    {  // a point behind the cannon: 45 services of yaw slew (180 / 4), then 90 frames of charge-up
        auto o = make(360.0f);
        const Vec3 at{-150, 0, 0};
        CHECK(o->w.sys->order_fire_at_position(1, at));
        const uint32_t f0 = o->w.frame;
        std::vector<uint32_t> charge;
        std::vector<uint32_t> shots;
        for (int i = 0; i < 300; ++i) {
            const uint64_t s0 = o->w.sys->stats().shots;
            const uint32_t f = o->w.frame;
            o->w.tick();
            for (const LobEvent& e : o->w.sys->lob_events())
                if (!e.charged) charge.push_back(e.frame);
            if (o->w.sys->stats().shots != s0) shots.push_back(f);
        }
        CHECK(charge.size() == 1 && !charge.empty() && charge[0] == f0 + 44);
        CHECK(shots.size() == 1 && !shots.empty() && shots[0] == f0 + 44 + 91);
        // the turret held its aim: the next order at the same point charges on its first service
        CHECK(o->w.sys->order_fire_at_position(1, at));
        const uint32_t f1 = o->w.frame;
        const std::vector<uint32_t> again = o->shot_frames(200);
        CHECK(again.size() == 1 && !again.empty() && again[0] == f1 + 91);
    }
    {  // outside Turret_Rotate_Extent_Degrees: never aimed, the order waits
        auto o = make(90.0f);
        CHECK(o->w.sys->order_fire_at_position(1, Vec3{-150, 0, 0}));
        CHECK(o->run(300) == 0 && o->w.sys->fire_at_position_state(1) == 1);
    }
}

}  // namespace

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
    test_fog_filter();
    test_attack_move_object();
    test_hardpoint_fire_routing();
    test_precise_hit_hook();
    test_take_damage();
    test_shield_regen_system();
    test_hull_hardpoint_coupling();
    test_hardpoint_disabled();
    test_station_upgrade_copy();
    test_modifier_cache();
    test_battle(golden, write_golden);
    test_allocations();
    test_save_load();  // N1 stage 2
    test_save_rejects();
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
    test_team_container_slot();       // s78h t06
    test_scan_player_order();         // s78g t04
    test_charge_up_slew();           // s78f t04
    test_hardpoint_aim_target_bones();  // S8 t27
    test_scan_range_query();
    test_shared_team_target();
    test_foc_data();
    if (bench) bench_modifiers();
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("all combat tests passed\n");
    return 0;
}

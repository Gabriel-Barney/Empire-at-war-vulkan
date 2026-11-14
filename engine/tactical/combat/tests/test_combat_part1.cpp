// Part 1 of test_combat (bo3 t03: split by position; see test_combat_common.hpp).
#include "test_combat_common.hpp"

namespace bo3split {

Aabb box_of(Vec3 p, float r) { return {{p.x - r, p.y - r, p.z - r}, {p.x + r, p.y + r, p.z + r}}; }
}  // namespace bo3split

namespace bo3split {

Vec3 norm(Vec3 v) {  // same arithmetic as the system's normalize
    const float l2 = dot(v, v);
    const float inv = 1.0f / dmath::sqrt(l2);
    return v * inv;
}
}  // namespace bo3split

namespace bo3split {

bool same(Vec3 a, Vec3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
}  // namespace bo3split

namespace bo3split {

uint32_t push_unit(CombatData& d, const UnitDesc& u) {
    d.units.push_back(u);
    return static_cast<uint32_t>(d.units.size() - 1);
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

// Shooter with one weapon hardpoint at its centre (no bone B, no muzzle draw) and a stationary target;
// returns the first projectile's direction and fills `expected` from a copy of the RNG.
void inaccuracy_case(GameMode mode, float accuracy, float fixed_radius, float override_radius, bool use_table, float expect_r,
                     uint64_t target_category) {
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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {
float lob_dist(Vec3 a, Vec3 b) { return dmath::sqrt(dot(a - b, a - b)); }
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

// cu8k t01 (CU8F_T01_T02_T07_RE.md §2.7 item 3): a thrown saber (category 8, Max_Speed 8, MFD 250, Projectile_Damage 13,
// Projectile_Stun_Radius 23). Thrower at the origin (Ranged_Target_Z_Adjust 7), target 150 units east (radius 5.25,
// as Rebel_Trooper): max travel = 250 + 5.25 + |7 - 7| = 255.25, so the saber dies at step ceil(255.25 / 8) = 32, on its
// way back. Not stepped on its spawn frame; the first step plans the two jittered legs (n1 = n2 = ceil(150 / 55) = 3:
// 2 x 2 + 2 x 2 = 8 Get_Float draws); every step damages every hostile unit within 23 (3D), no once-per-victim memory;
// friends are spared; no sweep (a hostile on the line takes no direct hit).
void test_saber_projectile() {
    World w;
    ProjectileDesc sd;
    sd.name = "Lukes_Light_Saber";
    sd.category = ProjectileCategory::LightSaber;
    sd.max_speed = 8.0f;
    sd.max_flight_distance = 250.0f;
    sd.damage = 13.0f;
    sd.stun_radius = 23.0f;
    sd.rocket_curve_distance = 50.0f;  // the type ctor defaults (the catalog fills them for LIGHT_SABER)
    sd.rocket_curve_offset = 30.0f;
    w.data.projectiles.push_back(sd);
    UnitDesc luke;
    luke.tactical_health = 1000.0f;
    luke.ranged_target_z_adjust = 7.0f;
    const uint32_t lt = push_unit(w.data, luke);
    UnitDesc trooper;
    trooper.tactical_health = 1000.0f;
    trooper.radius = 5.25f;
    trooper.ranged_target_z_adjust = 7.0f;
    const uint32_t tt = push_unit(w.data, trooper);
    CombatConfig cfg;
    cfg.mode = GameMode::Land;
    cfg.inaccuracy_enabled = false;
    w.make(cfg);
    w.add(1, lt, 0, Vec3{0, 0, 0});
    w.add(2, tt, 1, Vec3{150, 0, 0});    // the target
    w.add(3, tt, 1, Vec3{140, 12, 0});   // a hostile near the turn
    w.add(4, tt, 0, Vec3{145, -10, 0});  // a friend near the turn
    w.add(5, tt, 1, Vec3{1000, 1000, 0});  // far away
    SaberLaunch l;
    l.thrower = 1;
    l.owner = 0;
    l.projectile = 0;
    l.from = w.sys->target_point(1);
    l.fire_at = w.sys->target_point(2);
    l.target = 2;
    CHECK(l.from.z == 7.0f && l.fire_at.x == 150.0f && l.fire_at.z == 7.0f);
    l.projectile = 9;
    CHECK(w.sys->launch_saber(l, w.frame) == 0u);  // invalid projectile index
    l.projectile = 0;
    const uint32_t f0 = w.frame;
    const uint32_t serial = w.sys->launch_saber(l, f0);
    CHECK(serial != 0u);
    CHECK(w.sys->projectile(0).max_travel == 255.25f && w.sys->projectile(0).speed == 8.0f);
    Vec3 p;
    w.tick();  // spawn frame: not stepped
    CHECK(w.sys->saber_position(serial, p) && same(p, l.from));
    const uint64_t d0 = w.rng.draws();
    w.tick();  // first step: the plan (8 draws), then s = 8
    CHECK(w.rng.draws() - d0 >= 8u);
    CHECK(w.sys->saber_position(serial, p) && !same(p, l.from) && w.sys->projectile(0).travelled == 8.0f);
    uint32_t died = 0;
    for (int i = 0; i < 60 && !died; ++i) {
        w.tick();
        if (!w.sys->saber_position(serial, p)) died = w.frame - 1;
    }
    CHECK(died == f0 + 32u);
    const float h0 = w.sys->health(5);  // the untouched start health (the far unit)
    const float lost2 = h0 - w.sys->health(2), lost3 = h0 - w.sys->health(3);
    CHECK(lost2 >= 26.0f && std::fmod(lost2, 13.0f) == 0.0f);  // several steps within 23 of the target
    CHECK(lost3 >= 13.0f && std::fmod(lost3, 13.0f) == 0.0f);
    CHECK(w.sys->health(4) == h0 && w.sys->health(1) == h0);
    CHECK(w.sys->stats().hits == 0u);  // no direct hit
    // Same flight with a different RNG seed: still dies at the max travel (the jitter changes the path, not the limit).
    {
        World w2;
        w2.data = w.data;
        for (int k = 0; k < 5; ++k) (void)w2.rng.range_float(0.0f, 1.0f);  // another stream position: other jitter
        w2.make(cfg);
        w2.add(1, lt, 0, Vec3{0, 0, 0});
        w2.add(2, tt, 1, Vec3{150, 0, 0});
        const uint32_t s2 = w2.sys->launch_saber(l, w2.frame);
        const uint32_t g0 = w2.frame;
        uint32_t d2 = 0;
        for (int i = 0; i < 60 && !d2; ++i) {
            w2.tick();
            if (!w2.sys->saber_position(s2, p)) d2 = w2.frame - 1;
        }
        CHECK(d2 == g0 + 32u);
    }
    // A near target (40 units): the path (about 80 + jitter) ends before the max travel: the saber expires at the path's
    // end, within a few units of the thrower's target point.
    {
        World w3;
        w3.data = w.data;
        w3.make(cfg);
        w3.add(1, lt, 0, Vec3{0, 0, 0});
        w3.add(2, tt, 1, Vec3{40, 0, 0});
        SaberLaunch l3 = l;
        l3.fire_at = w3.sys->target_point(2);
        const uint32_t s3 = w3.sys->launch_saber(l3, w3.frame);
        const uint32_t g0 = w3.frame;
        Vec3 last = l3.from;
        uint32_t d3 = 0;
        for (int i = 0; i < 60 && !d3; ++i) {
            w3.tick();
            if (!w3.sys->saber_position(s3, p)) d3 = w3.frame - 1;
            else last = p;
        }
        CHECK(d3 > g0 + 8u && d3 < g0 + 20u);
        CHECK(std::fabs(last.x) < 16.0f && std::fabs(last.y) < 16.0f);
    }
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

void test_gaps_constants() {
    OneFileProvider fp;
    fp.text = "<GameConstants><Health_Low_Percent_Threshold>0.33</Health_Low_Percent_Threshold>"
              "<Melee_Cutoff_Range>51.0</Melee_Cutoff_Range>"
              "<Hardpoint_Recharge_Cutoff_For_Opportunity_Fire>5.0</Hardpoint_Recharge_Cutoff_For_Opportunity_Fire>"
              "<Object_Max_Speed_Multiplier_Space> 1.2 </Object_Max_Speed_Multiplier_Space>"
              "<Object_Max_Speed_Multiplier_Land> 1.56 </Object_Max_Speed_Multiplier_Land>"
              "<Diminishing_Firepower>0, 0.6, 0.3, 0.7, 0.9, 0.9, 1, 1, 2, 1</Diminishing_Firepower>"
              // cu8 t03: the remaining data-backed knobs (FoC writes the vulnerability as "2.0f")
              "<Base_Shield_Vulnerability_Modifier>2.0f</Base_Shield_Vulnerability_Modifier>"
              "<Hull_Vs_Hard_Points_Health_Constraint>0.3</Hull_Vs_Hard_Points_Health_Constraint>"
              "<Under_Construction_Damage_Multiplier>2.5</Under_Construction_Damage_Multiplier>"
              "<Depleted_Shield_Regen_Cap>0.5</Depleted_Shield_Regen_Cap><Depleted_Shield_Disable_Time>4.0</Depleted_Shield_Disable_Time>"
              "<ShieldRechargeIntervalInSecs>5.0</ShieldRechargeIntervalInSecs><EnergyRechargeIntervalInSecs>6.0</EnergyRechargeIntervalInSecs>"
              "<EnergyToShieldExchangeRate>1.0</EnergyToShieldExchangeRate><Sensor_Jamming_Time>.75</Sensor_Jamming_Time>"
              "<Max_Secs_For_AE_Delayed_Damage>0.5</Max_Secs_For_AE_Delayed_Damage></GameConstants>";
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
    CHECK_F(cfg.base_shield_vulnerability, 2.0f);  // cu8 t03
    CHECK_F(cfg.hull_vs_hardpoints_constraint, 0.3f);
    CHECK_F(cfg.under_construction_damage_multiplier, 2.5f);
    CHECK_F(cfg.depleted_shield_regen_cap, 0.5f);
    CHECK_F(cfg.depleted_shield_disable_time, 4.0f);
    CHECK_F(cfg.shield_recharge_interval_secs, 5.0f);
    CHECK_F(cfg.energy_recharge_interval_secs, 6.0f);
    CHECK_F(cfg.energy_to_shield_exchange_rate, 1.0f);
    CHECK_F(cfg.sensor_jamming_time, 0.75f);
    CHECK_F(cfg.max_secs_for_ae_delayed_damage, 0.5f);
    {  // the checks below run with the default values of these knobs
        const CombatConfig d;
        cfg.base_shield_vulnerability = d.base_shield_vulnerability;
        cfg.hull_vs_hardpoints_constraint = d.hull_vs_hardpoints_constraint;
        cfg.under_construction_damage_multiplier = d.under_construction_damage_multiplier;
        cfg.depleted_shield_regen_cap = d.depleted_shield_regen_cap;
        cfg.depleted_shield_disable_time = d.depleted_shield_disable_time;
        cfg.shield_recharge_interval_secs = d.shield_recharge_interval_secs;
        cfg.energy_recharge_interval_secs = d.energy_recharge_interval_secs;
        cfg.energy_to_shield_exchange_rate = d.energy_to_shield_exchange_rate;
        cfg.sensor_jamming_time = d.sensor_jamming_time;
        cfg.max_secs_for_ae_delayed_damage = d.max_secs_for_ae_delayed_damage;
    }
    // Spline through the FoC points: knots reproduced, smooth in between, end values outside.
    World w;
    w.data.damage.add_damage_type("Laser");
    UnitDesc v;
    v.tactical_health = 1e6f;
    const uint32_t vd = push_unit(w.data, v);
    UnitAbilityDesc nodim;
    nodim.ability_type = kAbilityPowerToWeapons;
    nodim.flag[kFlagAllowDiminishingFirepower] = 0;
    w.data.abilities = {nodim};
    UnitDesc nodim_unit = v;
    nodim_unit.abilities = {0, -1};
    const uint32_t ndd = push_unit(w.data, nodim_unit);
    w.make(cfg);
    // s78n t13 (HERO_CLASH §8.4.1 steps 10 - 11): the curve applies, and the last-hit stamp is written, only for a kind-1
    // hit whose allow flag step 10 set - a projectile with a found shooter, or an incognito non-projectile source - and
    // whose source's ALLOW_TARGET_DIMINISHING_FIREPOWER is true.
    w.add(1, vd, 0, Vec3{0, 0, 0});
    w.add(2, vd, 1, Vec3{500, 0, 0});     // a live shooter
    w.add(3, vd, 1, Vec3{500, 500, 0});   // the incognito non-projectile source
    w.add(4, ndd, 1, Vec3{500, 1000, 0}); // a shooter whose active ability has ALLOW_TARGET_DIMINISHING_FIREPOWER false
    w.sys->set_ability_active(4, 0, true);
    for (ObjectId id = 5; id <= 9; ++id) w.add(id, vd, 0, Vec3{0, static_cast<float>(id) * 100.0f, 0});
    enum class Src { None, Plain, Projectile };
    auto hit = [&](ObjectId victim, uint32_t frame, Src s, ObjectId source = 2, uint8_t kind = kKindNormal) {
        DamageRequest r;
        r.victim = victim;
        r.amount = 100.0f;
        r.kind = kind;
        if (s != Src::None) r.source = source;
        r.source_is_projectile = s == Src::Projectile;
        return w.sys->take_damage(r, frame).after_multipliers;
    };
    CHECK_NEAR(hit(1, 1000, Src::Projectile), 100.0, 1e-3);       // first hit: 999999 frames -> last y (1)
    CHECK_NEAR(hit(1, 1000, Src::Projectile), 60.0, 1e-3);        // 0 s -> 0.6
    CHECK_NEAR(hit(1, 1009, Src::Projectile), 70.0, 1e-2);        // 0.3 s -> 0.7 (knot)
    const float mid = hit(1, 1009 + 18, Src::Projectile);         // 0.6 s: between 0.7 and 0.9
    CHECK(mid > 70.0f && mid < 90.0f);
    CHECK_NEAR(hit(1, 1027 + 90, Src::Projectile), 100.0, 1e-3);  // 3 s: past the last knot
    CHECK_NEAR(hit(1, 1117, Src::Plain), 100.0, 1e-3);            // a plain non-projectile source right after a stamp
    CHECK_NEAR(hit(5, 1000, Src::None), 100.0, 1e-3);             // no source: never diminished, never stamps
    CHECK_NEAR(hit(5, 1000, Src::None), 100.0, 1e-3);
    CHECK_NEAR(hit(5, 1000, Src::Projectile), 100.0, 1e-3);       // so this is still the victim's first stamped hit
    // the stamp is written only by a diminishing hit: non-projectile hits in between do not move it
    CHECK_NEAR(hit(6, 2000, Src::Projectile), 100.0, 1e-3);
    CHECK_NEAR(hit(6, 2000, Src::Plain), 100.0, 1e-3);
    CHECK_NEAR(hit(6, 2003, Src::Plain), 100.0, 1e-3);
    CHECK_NEAR(hit(6, 2009, Src::Projectile), 70.0, 1e-2);        // 0.3 s after 2000, not 0.2 s after 2003
    // a projectile whose shooter is gone: no curve, no stamp (the next live-shooter hit measures from 2009)
    CHECK_NEAR(hit(6, 2012, Src::Projectile, 77), 100.0, 1e-3);
    CHECK_NEAR(hit(6, 2018, Src::Projectile), 70.0, 1e-2);
    // an incognito non-projectile source: the curve applies once, with its bit; then it is a plain source
    CHECK_NEAR(hit(7, 3000, Src::Projectile), 100.0, 1e-3);
    w.sys->set_flag(3, kUnitIncognito, true);
    CHECK_NEAR(hit(7, 3000, Src::Plain, 3), 60.0, 1e-3);
    CHECK(!(w.sys->flags(3) & kUnitIncognito));
    CHECK_NEAR(hit(7, 3000, Src::Plain, 3), 100.0, 1e-3);
    // ALLOW_TARGET_DIMINISHING_FIREPOWER false on the shooter: no curve and no stamp (0.6 s after 4000 at 4018)
    CHECK_NEAR(hit(8, 4000, Src::Projectile), 100.0, 1e-3);
    CHECK_NEAR(hit(8, 4000, Src::Projectile, 4), 100.0, 1e-3);
    CHECK_NEAR(hit(8, 4009, Src::Projectile, 4), 100.0, 1e-3);
    const float late = hit(8, 4018, Src::Projectile);
    CHECK(late > 71.0f && late < 90.0f);
    // another damage kind: no curve and no stamp
    CHECK_NEAR(hit(9, 5000, Src::Projectile), 100.0, 1e-3);
    CHECK_NEAR(hit(9, 5000, Src::Projectile, 2, uint8_t{7}), 100.0, 1e-3);
    CHECK_NEAR(hit(9, 5009, Src::Projectile, 2, uint8_t{7}), 100.0, 1e-3);
    const float late7 = hit(9, 5018, Src::Projectile);
    CHECK(late7 > 71.0f && late7 < 90.0f);
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
    // A1j t04 / s78n t08f (0x371e70 / 0x372040): without Custom_Hard extents E falls back to the model extents
    // (model_x_extent along the facing, model_y_extent across; the catalog fills them from the collision bound).
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
    // s78n t08f (TACTICAL_COMBAT §5.4.5 item 2): Nebulon_B_Frigate, Scale_Factor 0.7, collision-bound half (33.54, 157.07,
    // 95.52) -> X extent (along, model half y) 0.7 x 157.07 = 109.95, Y extent (across, model half x) 0.7 x 33.54 = 23.48.
    // Weapon range 1000: in range up to 1109.95 nose-on (|f.u| > 0.7071) and up to 1023.48 broadside.
    struct NebCase {
        float d;
        bool end_on, in;
    };
    for (const NebCase c : {NebCase{1109.5f, true, true}, NebCase{1110.5f, true, false}, NebCase{1023.0f, false, true},
                            NebCase{1024.0f, false, false}}) {
        TargetWorld m;
        UnitDesc neb = m.w.data.units[m.capital];
        neb.model_x_extent = 157.07f * 0.7f;
        neb.model_y_extent = 33.54f * 0.7f;
        neb.radius = 200.0f;  // tree box only
        const uint32_t nd = push_unit(m.w.data, neb);
        m.w.make();
        m.w.add(1, m.attacker, 0, Vec3{0, 0, 0});
        m.w.add(2, nd, 1, Vec3{c.d, 0, 0}, c.end_on ? Vec3{-1, 0, 0} : Vec3{0, 1, 0});  // nose toward U, or broadside
        m.w.tick();
        CHECK(m.w.sys->auto_target(1) == (c.in ? ObjectId{2} : kNoObject));
    }
    // s78n t11 (TACTICAL_COMBAT §5.4.6 items 3 / 4 / 6): MPTL - Custom_Hard_XExtent 30, _YExtent 15, _YExtent_Deployed 24,
    // Scale_Factor 1.1 -> X 33.0, Y 16.5, deployed Y 26.4. The deployed Y applies in Walk state 0x18 (kUnitDeployingIn with
    // kUnitDeployTransition) and 0x19 (kUnitDeploying), not in 0x1A (kUnitDeployTransition alone) and not for a type that
    // does not deploy; the X extent has no deployed value and stays 33.0.
    struct DepCase {
        float d;
        bool end_on;
        uint32_t flags;
        bool deploys, in;
    };
    const uint32_t s18 = kUnitDeployingIn | kUnitDeployTransition, s19 = kUnitDeploying, s1a = kUnitDeployTransition;
    for (const DepCase c : {DepCase{1020.0f, false, 0u, true, false}, DepCase{1020.0f, false, s18, true, true},
                            DepCase{1020.0f, false, s19, true, true}, DepCase{1020.0f, false, s1a, true, false},
                            DepCase{1027.0f, false, 0u, true, false}, DepCase{1027.0f, false, s18, true, false},
                            DepCase{1027.0f, false, s19, true, false}, DepCase{1027.0f, false, s1a, true, false},
                            DepCase{1032.0f, true, 0u, true, true}, DepCase{1032.0f, true, s18, true, true},
                            DepCase{1032.0f, true, s19, true, true}, DepCase{1032.0f, true, s1a, true, true},
                            DepCase{1034.0f, true, 0u, true, false}, DepCase{1034.0f, true, s19, true, false},
                            DepCase{1020.0f, false, s19, false, false}, DepCase{1016.0f, false, s19, false, true}}) {
        TargetWorld m;
        UnitDesc mptl = m.w.data.units[m.capital];
        mptl.hard_x_extent = 33.0f;
        mptl.hard_y_extent = 16.5f;
        mptl.hard_y_extent_deployed = 26.4f;
        mptl.deploys = c.deploys;
        mptl.has_locomotor = true;
        mptl.radius = 200.0f;  // tree box only
        const uint32_t md = push_unit(m.w.data, mptl);
        m.w.make();
        m.w.add(1, m.attacker, 0, Vec3{0, 0, 0});
        m.w.add(2, md, 1, Vec3{c.d, 0, 0}, c.end_on ? Vec3{-1, 0, 0} : Vec3{0, 1, 0});
        if (c.flags) m.w.sys->set_flag(2, c.flags, true);
        m.w.tick();
        CHECK(m.w.sys->auto_target(1) == (c.in ? ObjectId{2} : kNoObject));
        // slot 46 (min_range_geometry_of) reads the same extents: from the flank (U at the origin) E is the Y extent
        float D = 0.0f, E = 0.0f;
        CHECK(m.w.sys->min_range_geometry_of(2, Vec3{0, 0, 0}, D, E));
        if (!c.end_on) {
            const bool dep = c.deploys && (c.flags & (kUnitDeploying | kUnitDeployingIn)) != 0;
            CHECK_F(E, dep ? 26.4f : 16.5f);
        }
    }
    {  // each axis on its own: a deployed X alone widens only the along extent; has_locomotor false -> the normal extents
        TargetWorld m;
        UnitDesc dxo = m.w.data.units[m.capital];
        dxo.hard_x_extent = 33.0f;
        dxo.hard_y_extent = 16.5f;
        dxo.hard_x_extent_deployed = 50.0f;
        dxo.deploys = true;
        dxo.has_locomotor = true;
        const uint32_t d1 = push_unit(m.w.data, dxo);
        dxo.has_locomotor = false;
        const uint32_t d2 = push_unit(m.w.data, dxo);
        m.w.make();
        m.w.add(2, d1, 1, Vec3{0, 0, 0});
        m.w.add(3, d2, 1, Vec3{0, 500, 0});
        m.w.sys->set_flag(2, kUnitDeploying, true);
        m.w.sys->set_flag(3, kUnitDeploying, true);
        float D = 0.0f, E = 0.0f;
        CHECK(m.w.sys->min_range_geometry_of(2, Vec3{100, 0, 0}, D, E));  // along the facing (+x)
        CHECK_F(E, 50.0f);
        CHECK(m.w.sys->min_range_geometry_of(2, Vec3{0, 100, 0}, D, E));  // across
        CHECK_F(E, 16.5f);
        CHECK(m.w.sys->min_range_geometry_of(3, Vec3{100, 500, 0}, D, E));
        CHECK_F(E, 33.0f);
    }
}
}  // namespace bo3split

namespace bo3split {

// s78n t20 stage A (FIGHTER_FLYING §4.11 E "c", 0x40edd0; MOVEMENT §4.3.2 item 7): can_hit runs outside max range (slot
// 43), inside min range (slot 46), then on land the line of fire 0x3b5ad0 (the host's base-shield query: both points
// raised by their type's Ranged_Target_Z_Adjust, the projectile shield-damage gate) - the first failing test answers. Unknown
// ids answer NoTarget; space has no line test.
void test_s78n_can_hit() {
    for (const bool land : {true, false}) {
        World w;
        w.data.projectiles.push_back(ProjectileDesc{});
        w.data.projectiles[0].max_speed = 10.0f;
        w.data.projectiles.push_back(ProjectileDesc{});
        w.data.projectiles[1].max_speed = 10.0f;
        w.data.projectiles[1].does_shield_damage = false;
        UnitDesc a;
        a.tactical_health = 100.0f;
        a.max_attack_distance = 200.0f;
        a.min_attack_distance = 30.0f;
        a.weapon_projectile = 0;
        a.weapon_recharge = 100.0f;  // never fires during the test
        a.ranged_target_z_adjust = 5.0f;
        const uint32_t ad = push_unit(w.data, a);
        a.weapon_projectile = 1;  // a projectile without shield damage
        const uint32_t nd = push_unit(w.data, a);
        UnitDesc t;
        t.tactical_health = 100.0f;
        t.ranged_target_z_adjust = 2.0f;
        const uint32_t td = push_unit(w.data, t);
        CombatConfig cfg;
        cfg.mode = land ? GameMode::Land : GameMode::Space;
        w.make(cfg);
        w.add(1, ad, 0, Vec3{0, 0, 0});
        w.add(2, td, 1, Vec3{150, 0, 0});
        w.add(3, nd, 0, Vec3{0, 10, 0});
        CHECK(w.sys->can_hit(1, 2) == CanHit::Yes);
        w.sys->set_position(1, Vec3{-100, 0, 0});  // 250 from the target
        CHECK(w.sys->can_hit(1, 2) == CanHit::OutOfMaxRange);
        w.sys->set_position(1, Vec3{135, 0, 0});  // 15 from it, the min range is 30
        CHECK(w.sys->can_hit(1, 2) == CanHit::InsideMinRange);
        CHECK(w.sys->can_hit_from(1, Vec3{0, 0, 0}, 2) == CanHit::Yes);  // the position given, not the unit's
        CHECK_F(w.sys->can_hit_origin(1).x, 135.0f);
        w.sys->set_position(1, Vec3{0, 0, 0});
        int calls = 0;
        Vec3 qa{}, qb{};
        bool qd = true;
        w.sys->set_shield_sides_query([&](Vec3 f, Vec3 to, bool shield_damage) {
            ++calls;
            qa = f;
            qb = to;
            qd = shield_damage;
            return f.x < 50.0f && to.x > 50.0f;  // a shield edge at x = 50
        });
        CHECK(w.sys->can_hit(1, 2) == (land ? CanHit::LineBlocked : CanHit::Yes));
        CHECK(calls == (land ? 1 : 0));
        if (land) CHECK(qa.z == 5.0f && qb.z == 2.0f && qd);
        CHECK(w.sys->can_hit_from(1, Vec3{100, 0, 0}, 2) == CanHit::Yes);  // both points on one side
        // bkt1 t06 (b0079, MOVEMENT §4.3.2 item 7): an end point G given - the line of fire ends there (raised by T's
        // Ranged_Target_Z_Adjust), the range tests still measure to T
        const Vec3 g{40.0f, 0.0f, 7.0f};
        CHECK(w.sys->can_hit_from(1, Vec3{0, 0, 0}, 2, &g) == CanHit::Yes);  // G on the attacker's side of the edge
        if (land) CHECK(qb.x == 40.0f && qb.z == 9.0f);
        const Vec3 g2{60.0f, 0.0f, 0.0f};
        CHECK(w.sys->line_of_fire_from(1, Vec3{0, 0, 0}, 2, &g2) == (land ? CanHit::LineBlocked : CanHit::Yes));
        CHECK(w.sys->can_hit_from(1, Vec3{-100, 0, 0}, 2, &g) == CanHit::OutOfMaxRange);  // range to T, not to G
        w.sys->set_position(1, Vec3{-100, 0, 0});
        const int before = calls;
        CHECK(w.sys->can_hit(1, 2) == CanHit::OutOfMaxRange && calls == before);  // the range tests come first
        CHECK(w.sys->can_hit(3, 2) == (land ? CanHit::LineBlocked : CanHit::Yes));
        if (land) CHECK(!qd);  // its projectile does no shield damage
        CHECK(w.sys->can_hit(1, 99) == CanHit::NoTarget && w.sys->can_hit(99, 2) == CanHit::NoTarget);
    }
}
}  // namespace bo3split

namespace bo3split {

// s78n t02 (TACTICAL_COMBAT §5.4.4 item 1): slot 46 "inside min range" in full - E is the X half-extent, or the Y one when
// U's UNNORMALISED offset along X's facing is <= 0.7071 (slot 43 normalises); a LAND_OBSTACLE target has no E (centre
// distance, no aim walk); a team-member target is retested against its team's live member nearest U (3-D squared
// distance, strict <), its own position and E. The weapon min range of the targeting scan is the same test.
void test_s78n_min_range_slot46() {
    auto world = [](TargetWorld& t, uint32_t& td, uint32_t& od, uint32_t& ad) {
        UnitDesc a = t.w.data.units[t.attacker];
        a.min_attack_distance = 20.0f;
        ad = push_unit(t.w.data, a);
        UnitDesc x = t.w.data.units[t.capital];
        x.hard_x_extent = 4.0f;  // along the facing
        x.hard_y_extent = 2.0f;
        td = push_unit(t.w.data, x);
        x.land_obstacle = true;
        od = push_unit(t.w.data, x);
        t.w.make();
    };
    {  // the facing test on the unnormalised offset
        TargetWorld t;
        uint32_t td = 0, od = 0, ad = 0;
        world(t, td, od, ad);
        t.w.add(1, ad, 0, Vec3{-5.0f, 22.0f, 0.0f});
        t.w.add(2, td, 1, Vec3{0, 0, 0}, {1, 0, 0});
        // D = 22.56; offset along the facing 5 > 0.7071 -> E = 4: 22.56 <= 24 (slot 43's normalised 0.22 would pick Y: 22)
        CHECK(t.w.sys->inside_min_range(1, 2));
        t.w.sys->set_position(1, Vec3{-0.5f, 22.5f, 0.0f});  // offset 0.5 <= 0.7071 -> E = 2: 22.5056 > 22
        CHECK(!t.w.sys->inside_min_range(1, 2));
        t.w.sys->set_position(1, Vec3{-0.5f, 21.9f, 0.0f});  // 21.906 <= 22
        CHECK(t.w.sys->inside_min_range(1, 2));
        float D = 0.0f, E = 0.0f;
        CHECK(t.w.sys->min_range_geometry_of(2, Vec3{-30.0f, 0.0f, 0.0f}, D, E) && D == 30.0f && E == 4.0f);
        CHECK(!t.w.sys->min_range_geometry_of(99, Vec3{}, D, E) && D == 0.0f && E == 0.0f);
    }
    {  // LAND_OBSTACLE: D <= r on the centres (slot 43 would add the half-extent here: no obstacle query)
        TargetWorld t;
        uint32_t td = 0, od = 0, ad = 0;
        world(t, td, od, ad);
        t.w.add(1, ad, 0, Vec3{-22.0f, 0.0f, 0.0f});
        t.w.add(2, od, 1, Vec3{0, 0, 0}, {1, 0, 0});
        CHECK(!t.w.sys->inside_min_range(1, 2));  // 22 > 20 (before: 22 <= 20 + 4)
        t.w.sys->set_position(1, Vec3{-20.0f, 0.0f, 0.0f});
        CHECK(t.w.sys->inside_min_range(1, 2));
        float D = 0.0f, E = 0.0f;
        CHECK(t.w.sys->min_range_geometry_of(2, Vec3{-20.0f, 0.0f, 0.0f}, D, E) && D == 20.0f && E == 0.0f);
    }
    {  // team members: the member nearest the attacker decides, in inside_min_range and in the scan
        TargetWorld t;
        uint32_t td = 0, od = 0, ad = 0;
        world(t, td, od, ad);
        t.w.add(1, ad, 0, Vec3{0, 0, 0});
        add_team_unit(t, 2, td, 1, Vec3{100.0f, 0.0f, 0.0f}, 50, true);
        add_team_unit(t, 3, td, 1, Vec3{15.0f, 0.0f, 0.0f}, 50, false);
        t.w.tick();  // phase A: the team member lists
        CHECK(t.w.sys->inside_min_range(1, 3));
        CHECK(t.w.sys->inside_min_range(1, 2));  // 2 is 100 away, but its squad mate 3 is 15 <= 20 + 4
        CHECK(t.w.sys->auto_target(1) == kNoObject);  // neither member can be acquired
        // 3-D: the mate high above (z 400) is no longer the nearest member; 2 is tested on its own (100 > 24)
        t.w.sys->set_position(3, Vec3{15.0f, 0.0f, 400.0f});
        CHECK(!t.w.sys->inside_min_range(1, 2));
        CHECK(!t.w.sys->inside_min_range(1, 3));  // 3 is retested against 2, now the nearer member in 3-D
    }
    {  // s78n review: a team member added after the tick (a squadron launched after phase A) is in the member lists at
       // once, as after a load (combat_save rebuilds them): before, inside_min_range missed it until the next phase A
        TargetWorld t;
        uint32_t td = 0, od = 0, ad = 0;
        world(t, td, od, ad);
        t.w.add(1, ad, 0, Vec3{0, 0, 0});
        add_team_unit(t, 2, td, 1, Vec3{100.0f, 0.0f, 0.0f}, 50, true);
        t.w.tick();  // phase A: team 50 = {2}
        CHECK(!t.w.sys->inside_min_range(1, 2));
        add_team_unit(t, 3, td, 1, Vec3{15.0f, 0.0f, 0.0f}, 50, false);  // between ticks
        CHECK(t.w.sys->inside_min_range(1, 2));  // 3 is the nearer member: 15 <= 20 + 4
        ByteWriter w;
        t.w.sys->write(w);
        TargetWorld l;
        world(l, td, od, ad);
        ByteReader r(w.bytes());
        CHECK(l.w.sys->read(r));
        CHECK(l.w.sys->inside_min_range(1, 2));  // the loaded system agrees
    }
}
}  // namespace bo3split

namespace bo3split {

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
        // Map stamp id -1 (not the object's id): NoFootprint -> E = radius 300: 1250 <= 1300 taken. s78n t02 (§5.4.4 item
        // 1, slot 46 step 7): the min range of a LAND_OBSTACLE target is the bare centre distance D <= r (no E, no walk).
        run(0.0f, 1250.0f, taken, inside);
        CHECK(!taken && inside);  // 1250 <= 1250: the min range rejects it in the scan too
        run(0.0f, 1000.0f, taken, inside);
        CHECK(taken && !inside);  // 1250 > 1000 (slot 43's E of 300 is not added)
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
        run(40.0f, 1210.0f, taken, inside);
        CHECK(!taken && inside);  // max: 1000 in range; min: centre 1210 <= 1210 inside -> rejected
        run(40.0f, 1000.0f, taken, inside);
        CHECK(taken && !inside);  // s78n t02: centre 1210 > 1000 (the walked cell and sqrt(200) are slot 43 only)
        run(60.0f, 950.0f, taken, inside);
        CHECK(taken && !inside);  // centre 1190: in max range (980 walked), outside 950
        // Attacker on the far side (x = 1500): the walk goes +x to cell 74 (centre 1480), d = 20; the centre is 250 away.
        run(1500.0f, 250.0f, taken, inside);
        CHECK(!taken && inside);  // 250 <= 250
        run(1500.0f, 10.0f, taken, inside);
        CHECK(taken && !inside);  // s78n t02: 250 > 10 (before: walked 20 <= 10 + 14.14)
        // [port] attacker inside the footprint (x = 1300, len 50): steps 1250 / 1270 / 1290 / 1310, then walked 60 > 50
        // stops on an occupied cell (66, centre 1320): d = 20. The min range reads the centre, 50 away.
        run(1300.0f, 50.0f, taken, inside);
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
}  // namespace bo3split

namespace bo3split {

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
        t.w.tick(alamo::test::make_thread_parallel_for(4));
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
}  // namespace bo3split

namespace bo3split {

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
    UnitDesc nt = c;  // q6 t19: fails the type half of 0x5680b0 (e.g. no Collidable_By_Projectile_Living)
    nt.target_test_type = false;
    const uint32_t not_target = push_unit(w.data, nt);
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
    // q6 t19 (A1_READINGS §33.2.1): a goal that fails 0x5680b0's type test is never promoted, even in range (ids ascend).
    w.add(8, attacker, 0, Vec3{0, 8000, 0});
    w.add(9, not_target, 1, Vec3{500, 8000, 0});
    w.sys->set_attack_goal(8, 9);
    w.tick();
    w.tick();
    CHECK(w.sys->ordered_target(8) == kNoObject && w.sys->attack_goal(8) == 9);
    w.sys->set_hooks(nullptr);
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

// s78n t26.1 (MOVEMENT §3.2.1 item 6): Shielded_Only_When_Deployed (the Destroyer Droid) - the absorb runs only while
// deployed (0x39a390: Walk 0x19, walking deployed included); not undeployed, deploying (0x18) or undeploying (0x1A).
// s78n t26 closing review (§3.2.1 Helpers D = +0x2b3 set or 0x19, item 4 row "deploying (0x18), d < 150", item 6): a
// droid in 0x18 whose walking-deployed flag an order set (the host's kUnitDeploying in 0x18) absorbs; with it clear not.
void test_shielded_only_when_deployed() {
    const uint32_t s18 = kUnitDeployingIn | kUnitDeployTransition, s19 = kUnitDeploying, s1a = kUnitDeployTransition;
    const uint32_t s18f = s18 | kUnitDeploying;
    for (const uint32_t flags : {0u, s18, s19, s1a, s18f}) {
        const bool d = flags == s19 || flags == s18f;
        DamageWorld dw;
        UnitDesc& u = dw.w.data.units[dw.shielded];
        u.shielded_only_when_deployed = true;
        u.deploys = true;
        u.has_locomotor = true;
        CombatConfig cfg;
        cfg.object_max_health_multiplier = 1.0f;
        cfg.hardpoint_health_multiplier = 1.0f;
        dw.w.make(cfg);
        dw.w.add(1, dw.shielded, 1, Vec3{});
        if (flags) dw.w.sys->set_flag(1, flags, true);
        const DamageResult r = dw.w.sys->take_damage(dw.req(1, 100.0f), 10);
        CHECK_F(r.absorbed, d ? 20.0f : 0.0f);
        CHECK_F(r.to_hull, d ? 120.0f : 200.0f);
        CHECK_F(dw.w.sys->shield(1), d ? 0.0f : 20.0f);
    }
}
}  // namespace bo3split

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
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "thread_parallel_for.hpp"
#ifdef ALAMO_ABILITIES_TESTS_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

namespace {
std::atomic<uint64_t> g_allocs{0};
std::atomic<bool> g_count{false};
}  // namespace
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
using namespace alamo::tactical::abilities;

namespace {

using alamo::tactical::Vec3;
using alamo::tactical::combat::ObjectId;
using alamo::tactical::combat::kNoObject;

int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                                              \
    do {                                                                                                            \
        const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);                                    \
        if (!(va_ == vb_)) {                                                                                        \
            std::printf("FAIL %s:%d: %s == %s (%.9g vs %.9g)\n", __FILE__, __LINE__, #a, #b, va_, vb_);             \
            ++g_fail;                                                                                               \
        }                                                                                                           \
    } while (0)

constexpr uint64_t kCatFighter = 1, kCatCorvette = 2, kCatFrigate = 4, kCatCapital = 8;

Aabb box_of(Vec3 p, float r) { return {{p.x - r, p.y - r, p.z - r}, {p.x + r, p.y + r, p.z + r}}; }

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

// Team containers are not combat units: a placeholder UnitDesc keeps the two descriptor lists aligned.
uint32_t push_container(CombatData& cd, AbilityData& ad, AbilityObjectDesc a) {
    UnitDesc placeholder;
    placeholder.name = a.name;
    cd.units.push_back(placeholder);
    a.team_container = true;
    ad.objects.push_back(std::move(a));
    return static_cast<uint32_t>(cd.units.size() - 1);
}

uint32_t push(CombatData& cd, AbilityData& ad, const UnitDesc& u, AbilityObjectDesc a) {
    cd.units.push_back(u);
    a.name = u.name;
    a.name_crc = name_crc(u.name);
    a.category_mask = u.category_mask;
    link_combat_abilities(a, cd, cd.units.back());
    ad.objects.push_back(std::move(a));
    return static_cast<uint32_t>(cd.units.size() - 1);
}

UnitAbilityRecord rec(uint8_t type, float exp, float rech) {
    UnitAbilityRecord r;
    r.type = type;
    r.combat.ability_type = type;
    r.expiration_seconds = exp;
    r.recharge_seconds = rech;
    return r;
}

uint32_t add_special(AbilityData& ad, SpecialAbilityDesc s) {
    s.name_crc = name_crc(s.name);
    ad.specials.push_back(std::move(s));
    return static_cast<uint32_t>(ad.specials.size() - 1);
}

// ================================================================================================
// static data
// ================================================================================================

void test_info_table() {
    CHECK(std::strcmp(ability_type_info(kDefend).name, "DEFEND") == 0);
    CHECK(std::strcmp(ability_type_info(kLure).name, "LURE") == 0);
    CHECK(std::strcmp(ability_type_info(kStun).name, "STUN") == 0);
    CHECK(std::strcmp(ability_type_info(kDeploySquad).name, "DEPLOY_SQUAD") == 0);
    CHECK(std::strcmp(ability_type_info(76).name, "") == 0);
    CHECK(ability_type_from_name("power_to_weapons") == kPowerToWeapons);
    CHECK(ability_type_from_name("ION_CANNON_SHOT") == kIonCannonShot);
    CHECK(ability_type_from_name("NOPE") == 0);
    for (uint32_t t : {20u, 21u, 34u, 41u, 71u}) CHECK(ability_type_info(t).per_unit == 1);
    for (uint32_t t : {13u, 14u, 15u, 25u, 51u, 52u, 53u, 54u, 57u, 69u}) CHECK(ability_type_info(t).force == 1);
    CHECK(ability_type_info(kSelfDestruct).auto_deactivate == 0 && ability_type_info(kSelfDestruct).behavior == 0x43);
    CHECK(ability_type_info(kHunt).behavior == 0x21 && ability_type_info(kDeployTroopers).mode == 0);
    CHECK(ability_type_info(kTractorBeam).mode == 1 && ability_type_info(kLaserDefense).mode == 0);
    uint32_t defaults = 0;
    for (uint32_t t = 1; t <= kMaxAbilityType; ++t) defaults += ability_type_info(t).defaulted;
    CHECK(defaults == 14);  // the 12 open-item rows + DEPLOY_TROOPERS, SELF_DESTRUCT (partial rows)
    CHECK(style_allowed(ActivationStyle::UserInput, GameMode::Space, false));
    CHECK(!style_allowed(ActivationStyle::GroundAutomatic, GameMode::Space, true));
    CHECK(style_allowed(ActivationStyle::SpaceAutomatic, GameMode::Space, false));
    CHECK(!style_allowed(ActivationStyle::SkirmishAutomatic, GameMode::Space, false));
    CHECK(style_allowed(ActivationStyle::SkirmishAutomatic, GameMode::Land, true));
    CHECK(!style_allowed(ActivationStyle::GalacticAutomatic, GameMode::Land, true));
    CHECK(activation_style_from_name("COMBAT_AUTOMATIC") == ActivationStyle::CombatAutomatic);
    CHECK(mod_kind_from_name("WEAPON_DELAY_MULTIPLIER") == 1 && mod_kind_from_name("speed") == 6);
    CHECK(mod_kind_from_name("TARGET_TAKE_DAMAGE_MULTIPLIER") == 12);
    CHECK(mod_flag_from_name("ALLOW_TARGET_DIMINISHING_FIREPOWER") == 3);
    CHECK(special_class_from_tag("tractor_beam_attack_ability") == SpecialClass::TractorBeam);
    CHECK(parse_projectile_categories("MISSILE, ROCKET, MPTL_ROCKET") == ((1u << 1) | (1u << 4) | (1u << 6)));
}

void test_countdown() {
    Countdown c;
    CHECK(c.start_active(1, 10.0f, 30));
    CHECK(c.counter[1] == 0 && c.maximum[1] == 300 && c.phase[1] == 1);
    CHECK(!c.start_active(2, 0.0f, 30));  // no active phase
    CHECK(!c.blocks_activation(1));        // counter 0
    c.counter[1] = 90;
    CHECK(!c.blocks_activation(1));  // active, below its maximum
    // early deactivation: f = 1 - 90/300 = 0.7; R = 20 x (1 - 0.25) = 15 s -> 450; counter = int(450 x 0.3 + 0.5) = 135
    CHECK(!c.deactivate_early(1, 20.0f, -0.25f, 30));
    CHECK(c.phase[1] == 0 && c.maximum[1] == 450 && c.counter[1] == 135);
    CHECK(c.blocks_activation(1) && c.recharging(1));
    // truncation: int(30 x 0.99) = 29
    c.set_duration(3, 0.99f, 30);
    CHECK(c.counter[3] == 29 && c.maximum[3] == 29);
    // R <= 0 changes nothing (ready at once)
    Countdown d;
    CHECK(d.start_active(4, 2.0f, 30));
    CHECK(!d.start_recharge(4, 0.0f, 0.0f, 30));
    CHECK(d.phase[4] == 1 && d.maximum[4] == 60);
    // phase-1 counter 0 -> f = 1 -> counter 0; recharge maximum 0 -> "ready" signal
    Countdown e;
    e.start_active(5, 1.0f, 30);
    CHECK(!e.deactivate_early(5, 4.0f, 0.0f, 30));
    CHECK(e.counter[5] == 0 && e.maximum[5] == 120 && e.phase[5] == 0);
    Countdown g;
    g.phase[6] = 1;
    CHECK(g.deactivate_early(6, 0.0f, 0.0f, 30));  // maximum stays 0 -> ready signal
    // list-9 clamp is the caller's: R = 10 x (1 - 0.5) = 5 -> 150
    Countdown h;
    CHECK(h.start_recharge(7, 10.0f, -0.5f, 30) && h.counter[7] == 150);
}

void test_parse() {
    const std::string xml =
        "<SpaceUnit Name=\"T\">"
        "<Unit_Abilities_Data SubObjectList=\"Yes\">"
        "<Unit_Ability><Type>power_to_weapons</Type>"
        "<Mod_Multiplier>WEAPON_DELAY_MULTIPLIER, 0.5f</Mod_Multiplier>"
        "<Mod_Multiplier>SHIELD_REGEN_MULTIPLIER, -25</Mod_Multiplier>"
        "<Mod_Flag>ALLOW_TARGET_DIMINISHING_FIREPOWER, false</Mod_Flag>"
        "<Expiration_Seconds>12</Expiration_Seconds><Recharge_Seconds>50.0</Recharge_Seconds>"
        "<GUI_Activated_Ability_Name> Foo_Ability </GUI_Activated_Ability_Name>"
        "<Alternate_Icon_Name> i_sa_strike.tga </Alternate_Icon_Name>"
        "<Projectile_Types_Override>Proj_A, Proj_B</Projectile_Types_Override>"
        "<Supports_Autofire>True</Supports_Autofire></Unit_Ability>"
        "<Unit_Ability><Type>BOGUS</Type></Unit_Ability>"
        "</Unit_Abilities_Data>"
        "<Abilities SubObjectList=\"Yes\">"
        "<Tractor_Beam_Attack_Ability Name=\"Foo_Ability\"><Activation_Style> User_Input </Activation_Style>"
        "<Applicable_Unit_Categories>Corvette | Frigate</Applicable_Unit_Categories>"
        "<Activation_Max_Range>2000</Activation_Max_Range><Target_Speed_Decrease_Percent>0.75</Target_Speed_Decrease_Percent>"
        "<Stacking_Category>2</Stacking_Category></Tractor_Beam_Attack_Ability>"
        "<Combat_Bonus_Ability Name=\"Aura\"><Activation_Style>SPACE_AUTOMATIC</Activation_Style></Combat_Bonus_Ability>"
        "</Abilities></SpaceUnit>";
    const data::XmlDocument doc = data::XmlDocument::parse(xml, "t.xml");
    AbilityResolvers r;
    r.projectile = [](std::string_view n) { return iequals(n, "Proj_B") ? 7 : -1; };
    r.category = [](std::string_view n) -> uint64_t {
        if (iequals(n, "Corvette")) return kCatCorvette;
        if (iequals(n, "Frigate")) return kCatFrigate;
        return 0;
    };
    std::vector<std::string> diag;
    const std::vector<UnitAbilityRecord> recs = parse_unit_abilities(doc.root(), r, &diag);
    CHECK(recs.size() == 1 && diag.size() == 1);  // BOGUS reported
    if (recs.size() == 1) {
        const UnitAbilityRecord& u = recs[0];
        CHECK(u.type == kPowerToWeapons);
        CHECK(u.alternate_icon_name == "i_sa_strike.tga");  // t42
        CHECK_EQ(u.combat.mult[0], 0.5f);
        CHECK_EQ(u.combat.mult[1], -25.0f);
        CHECK(u.combat.flag[3] == 0);
        CHECK_EQ(u.expiration_seconds, 12.0f);
        CHECK_EQ(u.recharge_seconds, 50.0f);
        CHECK(u.gui_name_crc == name_crc("Foo_Ability"));
        CHECK(u.projectile_override.size() == 2 && u.projectile_override[1] == 7);
        CHECK(u.supports_autofire);
    }
    AbilityData ad;
    diag.clear();
    const std::vector<uint32_t> idx = parse_special_abilities(doc.root(), ad, r, &diag);
    CHECK(idx.size() == 2 && diag.empty());
    if (idx.size() == 2) {
        const SpecialAbilityDesc& s = ad.specials[idx[0]];
        CHECK(s.cls == SpecialClass::TractorBeam && s.name_crc == name_crc("Foo_Ability"));
        CHECK(s.applicable_categories == (kCatCorvette | kCatFrigate));
        CHECK_EQ(s.max_range, 2000.0f);
        CHECK(s.stacking_category == 2 && s.style == ActivationStyle::UserInput);
        // OI abilities: Combat_Bonus is a class now (an aura, service interval 0)
        CHECK(ad.specials[idx[1]].cls == SpecialClass::CombatBonus && ad.specials[idx[1]].style == ActivationStyle::SpaceAutomatic);
        CHECK(ad.specials[idx[1]].service_interval == -1 && special_service_interval(ad.specials[idx[1]].cls, 30) == 0);
        CHECK(special_service_interval(s.cls, 30) == 1 && special_service_interval(SpecialClass::Earthquake, 30) == 3);
    }
}

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

void test_modes() {
    ModeWorld w;
    AbilitySystem& a = *w.ab;
    const ObjectId ship[] = {1};
    CHECK(a.has_ability(1, kDefend) && !a.has_ability(1, kTurbo));
    CHECK_EQ(w.sys->mod_multiplier(1, ModKind::ShieldRegenInterval), 1.0f);
    // command 43 DEFEND on at frame 1
    CHECK(a.set_unit_ability_mode(ship, kDefend, true, w.frame) == 1);
    CHECK(a.is_active(1, kDefend));
    CHECK(a.countdown_maximum(1, kDefend) == 450 && a.countdown_counter(1, kDefend) == 0 && a.countdown_phase(1, kDefend) == 1);
    CHECK_EQ(w.sys->mod_multiplier(1, ModKind::ShieldRegenInterval), 0.1f);  // system B, slot 0 active
    CHECK_EQ(w.sys->mod_multiplier(1, ModKind::Speed), 0.8f);
    CHECK(a.set_unit_ability_mode(ship, kDefend, true, w.frame) == 0);  // already on
    // P2W too (slot 1): the product of both slots
    CHECK(a.set_unit_ability_mode(ship, kPowerToWeapons, true, w.frame) == 1);
    CHECK_EQ(w.sys->mod_multiplier(1, ModKind::WeaponDelay), 0.5f);
    // 449 updates later the DEFEND counter is 449; the 450th expires it (delta = 1 per frame)
    for (int i = 0; i < 449; ++i) w.tick();
    CHECK(a.countdown_counter(1, kDefend) == 449 && a.is_active(1, kDefend));
    w.tick();
    CHECK(!a.is_active(1, kDefend));
    CHECK(a.countdown_phase(1, kDefend) == 0 && a.countdown_maximum(1, kDefend) == 600 && a.countdown_counter(1, kDefend) == 600);
    CHECK_EQ(w.sys->mod_multiplier(1, ModKind::ShieldRegenInterval), 1.0f);
    CHECK(!a.can_activate(1, kDefend, true, true, w.frame));        // recharging (gate 7)
    CHECK(a.set_unit_ability_mode(ship, kDefend, true, w.frame) == 0);
    for (int i = 0; i < 599; ++i) w.tick();
    CHECK(a.countdown_counter(1, kDefend) == 1);
    w.tick();
    CHECK(a.countdown_counter(1, kDefend) == 0 && w.has_event(AbilityEventKind::Ready, 1));
    CHECK(a.can_activate(1, kDefend, true, true, w.frame));
    // early cancel after 150 of 450 frames: f = 1 - 150/450; counter = int(600 x (150/450) + 0.5) = 200
    CHECK(a.set_unit_ability_mode(ship, kDefend, true, w.frame) == 1);
    for (int i = 0; i < 150; ++i) w.tick();
    CHECK(a.countdown_counter(1, kDefend) == 150);
    CHECK(a.set_unit_ability_mode(ship, kDefend, false, w.frame) == 1);
    CHECK(a.countdown_phase(1, kDefend) == 0 && a.countdown_counter(1, kDefend) == 200);
    // recharge with the object's own list 9 (-0.5): 20 x 0.5 = 10 s -> 300; the player's set is ignored
    w.sys->player_modifiers(0).insert(ModList::AbilityRecharge, 0, -0.9f, kPermanent);
    w.sys->unit_modifiers(1)->insert(ModList::AbilityRecharge, 0, -0.5f, kPermanent);
    CHECK(a.force_recharge(1, kDefend));
    CHECK(a.countdown_counter(1, kDefend) == 300 && a.countdown_maximum(1, kDefend) == 300);
    CHECK(a.force_recharge(1, kDefend, 2.0f) && a.countdown_counter(1, kDefend) == 60);
    // SPOILER_LOCK: no timers -> a permanent toggle
    CHECK(a.set_unit_ability_mode(ship, kSpoilerLock, true, w.frame) == 1);
    for (int i = 0; i < 100; ++i) w.tick();
    CHECK(a.is_active(1, kSpoilerLock));
    CHECK(a.set_unit_ability_mode(ship, kSpoilerLock, false, w.frame) == 1);
    CHECK(!a.is_active(1, kSpoilerLock) && a.can_activate(1, kSpoilerLock, true, true, w.frame));
}

void test_gates() {
    ModeWorld w;
    AbilitySystem& a = *w.ab;
    const uint32_t f = w.frame;
    CHECK(a.can_activate(1, kDefend, true, true, f));
    CHECK(!a.can_activate(1, 0, true, true, f) && !a.can_activate(1, 76, true, true, f));  // gate 1
    a.set_nebula(1, f);  // gate 2: 5 s lockout
    CHECK(!a.can_activate(1, kDefend, true, true, f + 149));
    CHECK(a.can_activate(1, kDefend, true, false, f + 149));
    CHECK(a.can_activate(1, kDefend, true, true, f + 150));
    a.set_corrupted(1, true);  // gate 6
    CHECK(!a.can_activate(1, kDefend, true, true, f + 200));
    a.set_corrupted(1, false);
    a.set_abilities_disabled(1, true);  // gate 5
    CHECK(!a.can_activate(1, kDefend, true, true, f + 200));
    a.set_abilities_disabled(1, false);
    a.set_locomotor_busy(1, true);  // gate 9
    CHECK(!a.can_activate(1, kDefend, true, true, f + 200));
    a.set_locomotor_busy(1, false);
    // DEFEND: depleted-shield lockout and ion storm
    a.set_ion_storm(1, f + 200);
    CHECK(!a.can_activate(1, kDefend, true, true, f + 300) && a.can_activate(1, kDefend, true, true, f + 350));
    // team container: every member must pass (gate 8)
    CHECK(a.has_ability(5, kSprint) && a.can_activate(5, kSprint, true, true, f));
    a.set_corrupted(3, true);
    CHECK(!a.can_activate(5, kSprint, true, true, f));
    a.set_corrupted(3, false);
    // Force gate: type 13 with F = 1 on a unit without the record still runs the gate order
    CHECK(!a.can_activate(1, kFireLobbingSuperweapon, true, true, f));  // [port] super weapon not modelled
}

void test_squad() {
    ModeWorld w;
    AbilitySystem& a = *w.ab;
    const ObjectId sq[] = {5};
    CHECK(!a.is_active(5, kSprint));
    CHECK(a.set_unit_ability_mode(sq, kSprint, true, w.frame) == 1);  // forwarded to members 2 and 3
    CHECK(a.is_active(2, kSprint) && a.is_active(3, kSprint) && a.is_active(5, kSprint));
    CHECK(a.countdown_maximum(2, kSprint) == 60 && a.countdown_maximum(5, kSprint) == 0);  // members' countdowns
    CHECK(a.set_unit_ability_active(3, kSprint, false, w.frame));
    CHECK(!a.is_active(5, kSprint));  // every member that has it must be active
    // member expiry -> Set(container, off) -> all members off and recharging
    for (int i = 0; i < 60; ++i) w.tick();
    CHECK(!a.is_active(2, kSprint) && !a.is_active(5, kSprint));
    CHECK(a.countdown_phase(2, kSprint) == 0 && a.countdown_counter(2, kSprint) == 120);
    // the container is a user that is not a member; a member is skipped by command 40 (no special here)
    CHECK(a.record(5, kSprint) == a.record(2, kSprint));  // U = 0: the member's record
}

// P4 tactical: the campaign Black Market gate (ABILITIES_HARDPOINTS 2.4, P4_SIM_SYSTEMS 1.5).
uint64_t ability_hash(const AbilitySystem& a) {
    StateHasher h;
    a.hash(h);
    return h.digest();
}

void test_black_market_gate() {
    const uint32_t bm = name_crc("BM_Defend");
    auto make_world = [&](World& w, bool skirmish, bool gate = true) {
        UnitDesc s;
        s.name = "Ship";
        s.tactical_health = 1000.0f;
        s.shield_points = 500.0f;
        s.radius = 50.0f;
        s.category_mask = kCatCapital;
        AbilityObjectDesc a;
        UnitAbilityRecord defend = rec(kDefend, 15.0f, 20.0f);
        defend.gui_activated_ability_name = "BM_Defend";
        defend.gui_name_crc = bm;
        defend.must_be_bought_on_black_market = true;
        a.unit_abilities = {defend, rec(kPowerToWeapons, 6.0f, 50.0f)};
        const uint32_t ship = push(w.cd, w.ad, s, a);
        AbilityConfig acfg;
        acfg.skirmish = skirmish;
        w.make({}, acfg);
        if (gate) w.ab->set_black_market_blocked(0, {bm, bm});  // player 0 has not bought it
        w.add(1, ship, 0, {0, 0, 0});
        w.add(2, ship, 1, {3000, 0, 0});
        w.tick();
    };
    {
        World w;
        make_world(w, false);
        AbilitySystem& a = *w.ab;
        const ObjectId one[] = {1}, two[] = {2};
        // Gated for player 0 only: Can_Activate / readiness / the HUD query / command 43 all refuse it.
        CHECK(a.has_ability(1, kDefend) && !a.black_market_allows(1, kDefend));
        CHECK(!a.can_activate(1, kDefend, true, true, w.frame) && !a.is_ready(1, kDefend, w.frame));
        CHECK(a.set_unit_ability_mode(one, kDefend, true, w.frame) == 0 && !a.is_active(1, kDefend));
        CHECK(a.black_market_allows(1, kPowerToWeapons) && a.can_activate(1, kPowerToWeapons, true, true, w.frame));
        CHECK(a.set_unit_ability_mode(one, kPowerToWeapons, true, w.frame) == 1);  // unmarked slot unaffected
        CHECK(a.black_market_allows(2, kDefend) && a.is_ready(2, kDefend, w.frame));
        CHECK(a.set_unit_ability_mode(two, kDefend, true, w.frame) == 1);  // player 1's list is empty
        const uint64_t gated = ability_hash(a);
        // Bought (host replaces the list): allowed from then on. The list itself is a battle-start input, not state.
        a.set_black_market_blocked(0, {});
        CHECK(ability_hash(a) == gated);
        CHECK(a.black_market_allows(1, kDefend) && a.is_ready(1, kDefend, w.frame));
        CHECK(a.set_unit_ability_mode(one, kDefend, true, w.frame) == 1 && a.is_active(1, kDefend));
        // Turning it off is never gated.
        a.set_black_market_blocked(0, {bm});
        CHECK(a.set_unit_ability_mode(one, kDefend, false, w.frame) == 1);
    }
    {
        World w;  // skirmish: the gate always passes
        make_world(w, true);
        const ObjectId one[] = {1};
        CHECK(w.ab->black_market_allows(1, kDefend) && w.ab->is_ready(1, kDefend, w.frame));
        CHECK(w.ab->set_unit_ability_mode(one, kDefend, true, w.frame) == 1);
    }
    {
        // Same run with and without the (unused) gate: identical state hash; a gated activation attempt changes nothing.
        World a, b;
        make_world(a, false, false);
        make_world(b, false, true);
        CHECK(ability_hash(*a.ab) == ability_hash(*b.ab));
        const ObjectId one[] = {1};
        CHECK(b.ab->set_unit_ability_mode(one, kDefend, true, b.frame) == 0);
        for (int i = 0; i < 30; ++i) {
            a.tick();
            b.tick();
        }
        CHECK(ability_hash(*a.ab) == ability_hash(*b.ab));
        CHECK(a.ab->set_unit_ability_mode(one, kDefend, true, a.frame) == 1);
        a.tick();
        b.tick();
        CHECK(ability_hash(*a.ab) != ability_hash(*b.ab));
    }
}

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

void test_hardpoints() {
    HpWorld w;
    // creation: one Get_Int(0, int(FPS x Fire_Max_Recharge_Seconds)) per hardpoint, list order; none for span 0
    SimRng pred = w.rng;
    const int32_t c0 = pred.range_int(0, 60);
    const int32_t c2 = pred.range_int(0, 30);
    w.add(1, w.cap, 0, {0, 0, 0});
    CHECK(w.sys->hardpoint_cooldown(1, 0) == c0 && w.sys->hardpoint_cooldown(1, 2) == c2);
    CHECK(w.rng.state() == pred.state());
    CHECK(w.sys->hardpoint_count(1) == 4 && w.sys->hardpoint_kind(1, 1) == HardpointKind::ShieldGenerator);
    // Get_Hull: min(hull, sum cur / sum max) for the all-hardpoints rule
    w.sys->set_health(1, 800.0f);
    CHECK_EQ(w.ab->get_hull(1), 0.8f);
    DamageRequest r;
    r.victim = 1;
    r.kind = kKindBypassShield;
    r.amount = 200.0f;
    r.hardpoint = 1;  // the shield generator dies (200)
    w.sys->take_damage(r, w.frame);
    CHECK(w.sys->hardpoint_destroyed(1, 1));
    CHECK_EQ(w.ab->get_hull(1), 0.5f);  // (100 + 100) / 400
    CHECK(w.sys->hardpoint_events().size() == 1 && w.sys->hardpoint_events()[0].index == 1 &&
          w.sys->hardpoint_events()[0].last_of_kind && w.sys->shield(1) == 0.0f);
    // smoke at 75 % / 50 % crossed downward in one hit
    r.hardpoint = -1;
    r.amount = 100.0f;  // 800 -> 700: crosses 750
    w.sys->take_damage(r, w.frame);
    CHECK(w.sys->smoke_events().size() == 1 && w.sys->smoke_events()[0].percent == 75);
    r.amount = 10.0f;
    w.sys->take_damage(r, w.frame);
    CHECK(w.sys->smoke_events().size() == 1);
    // the damage event list (A1 AI): one record per applied hit
    CHECK(w.sys->damage_events().size() == 3 && w.sys->damage_events()[0].hardpoint == 1 &&
          w.sys->damage_events()[1].to_hull == 100.0f);
    // Die: death signal, then live destroyable hardpoints in list order (0, 2; 3 is not destroyable),
    // death clone for kind 7, one Get_Int(0, 2) for the death projectile.
    SimRng p2 = w.rng;
    const int32_t pick = p2.range_int(0, 2);
    const size_t hp_before = w.sys->hardpoint_events().size();
    const size_t proj_before = w.sys->projectile_count();
    w.sys->kill_unit(1, 9, kKindFire, w.frame);
    CHECK(!w.sys->alive(1));
    CHECK(w.sys->hardpoint_events().size() == hp_before + 2);
    if (w.sys->hardpoint_events().size() == hp_before + 2) {
        CHECK(w.sys->hardpoint_events()[hp_before].index == 0 && w.sys->hardpoint_events()[hp_before].owner_dying);
        CHECK(w.sys->hardpoint_events()[hp_before + 1].index == 2 && w.sys->hardpoint_events()[hp_before].explosion_type == 77);
    }
    CHECK(w.sys->deaths().size() == 1 && w.sys->deaths()[0].death_clone == 56 && w.sys->deaths()[0].kind == kKindFire);
    CHECK(w.sys->deaths()[0].death_projectile == pick && w.rng.state() == p2.state());
    CHECK(w.sys->projectile_count() == proj_before + 1);
    CHECK_EQ(w.sys->projectile(proj_before).pos.z, 20.0f);
}

void test_last_hardpoint_kills() {
    HpWorld w;
    w.add(1, w.cap, 0, {0, 0, 0});
    DamageRequest r;
    r.victim = 1;
    r.kind = kKindBypassShield;
    for (int32_t i : {0, 1, 2}) {
        r.hardpoint = i;
        r.amount = 500.0f;
        const DamageResult res = w.sys->take_damage(r, w.frame);
        CHECK(res.killed == (i == 2));
    }
    CHECK(!w.sys->alive(1) && w.sys->deaths().size() == 1);
    CHECK(w.sys->deaths()[0].death_clone == 55);  // the hull path kills with kind 1
}

void test_fire_and_decay() {
    HpWorld w;
    w.add(1, w.small, 0, {0, 0, 0});
    DamageRequest r;
    r.victim = 1;
    r.kind = kKindFire;
    r.amount = 10.0f;
    w.sys->take_damage(r, w.frame);
    CHECK(w.sys->burning(1) && w.sys->health(1) == 60.0f);  // ignites instead of taking the hit
    w.sys->take_damage(r, w.frame);
    CHECK_EQ(w.sys->health(1), 50.0f);  // later fire hits deal normal damage
    for (int i = 0; i < 30; ++i) w.tick();
    CHECK_EQ(w.sys->health(1), 46.0f);  // BurningBehavior: 4 per second
    // converted decay: d = 60 / 10 / 30 = 0.2 per frame, one Get_Float(-0.02, 0.02) per frame
    HpWorld v;
    v.add(1, v.small, 0, {0, 0, 0});
    v.sys->set_flag(1, kUnitConverted, true);
    SimRng pred = v.rng;
    const float d = 60.0f / 10.0f / 30.0f;
    const float draw = pred.range_float(-(d / 10.0f), d / 10.0f);
    v.tick();
    CHECK_EQ(v.sys->health(1), 60.0f - (d + draw));
    CHECK(v.rng.state() == pred.state());
}

// ================================================================================================
// combat features: energy pool, ion stun, missile shield, jamming, opportunity fire
// ================================================================================================

void test_energy() {
    World w;
    ProjectileDesc beam;
    beam.name = "Beam";
    beam.max_speed = 50.0f;
    beam.damage = 1.0f;
    beam.energy_per_shot = 30.0f;
    w.cd.projectiles = {beam};
    UnitDesc u;
    u.name = "E";
    u.tactical_health = 100.0f;
    u.radius = 5.0f;
    u.energy_capacity = 100.0f;
    u.energy_refresh_rate = 10.0f;
    u.max_attack_distance = 500.0f;
    u.weapon_projectile = 0;
    u.weapon_recharge = 0.0f;
    u.weapon_pulse_count = 1;
    u.shield_points = 50.0f;
    u.shield_refresh_rate = 10.0f;
    const uint32_t e = push(w.cd, w.ad, u, AbilityObjectDesc{});
    UnitDesc t;
    t.name = "T";
    t.tactical_health = 1e6f;
    t.radius = 5.0f;
    const uint32_t tt = push(w.cd, w.ad, t, AbilityObjectDesc{});
    CombatConfig cfg;
    cfg.inaccuracy_enabled = false;
    w.make(cfg);
    w.add(1, e, 0, {0, 0, 0});
    w.add(2, tt, 1, {100, 0, 0});
    CHECK_EQ(w.sys->energy(1), 100.0f);
    w.sys->order_attack(1, 2);
    w.sys->set_shield(1, 50.0f);
    for (int i = 0; i < 40; ++i) w.tick();
    // three 30-point shots, then energy 10 < 30 blocks the weapon; no regen before frame 150
    CHECK_EQ(w.sys->energy(1), 10.0f);
    CHECK(w.sys->stats().shots == 3);
    // shield exchange: 10 points of regeneration cost 50 energy, all or nothing -> nothing (energy 10)
    w.sys->set_shield(1, 40.0f);
    for (int i = 0; i < 110; ++i) w.tick();  // the 150th upkeep regenerates +10 energy (interval int(30x5+0.5))
    CHECK(w.sys->stats().shots == 3);
    CHECK_EQ(w.sys->shield(1), 40.0f);
    CHECK_EQ(w.sys->energy(1), 20.0f);
}

void test_ion_stun() {
    ModeWorld w;
    ProjectileDesc ion;
    ion.name = "Proj_Ion";
    ion.ion_stun = true;
    ion.ion_stun_duration = 1.0f;
    ion.ion_stun_speed_reduction = 0.5f;
    ion.ion_stun_shot_rate_reduction = 0.5f;
    ion.ion_stun_stack_duration = true;
    w.cd.projectiles.push_back(ion);
    const uint32_t pi = static_cast<uint32_t>(w.cd.projectiles.size() - 1);
    const ObjectId ship[] = {1};
    CHECK(w.ab->set_unit_ability_mode(ship, kDefend, true, w.frame) == 1);
    const uint32_t f = w.frame;
    CHECK(w.sys->apply_ion_stun(1, pi, f));
    CHECK(w.sys->ion_stun_end(1) == f + 30 && w.sys->ion_stunned(1, f + 29) && !w.sys->ion_stunned(1, f + 30));
    CHECK(!w.ab->is_active(1, kDefend));  // DEFEND switched off
    CHECK(!w.ab->can_activate(1, kDefend, false, true, f));  // blocked while stunned
    CHECK_EQ(w.sys->modifier(1, ModList::MaxSpeed), -0.5f);
    CHECK_EQ(w.sys->modifier(1, ModList::FireRate), -0.5f);
    // stacking: end += 30; replace-by-source keeps one entry per list
    CHECK(w.sys->apply_ion_stun(1, pi, f + 10));
    CHECK(w.sys->ion_stun_end(1) == f + 60);
    CHECK(w.sys->unit_modifiers(1)->entries().size() == 2);
    // duration (end - now + 1) / FPS = 51 / 30 s -> expiry now + int(30 x 1.7 + 0.5) = now + 51
    CHECK(w.sys->unit_modifiers(1)->entries()[0].expiry == f + 10 + 51);
    w.sys->set_flag(1, kUnitLocomotorBusy, true);
    CHECK(!w.sys->apply_ion_stun(1, pi, f + 20));  // locomotor busy: nothing
}

struct ShieldWorld : World {
    ShieldWorld(ProjectileCategory cat, bool passive_only) {
        ProjectileDesc p;
        p.name = "P";
        p.max_speed = 20.0f;
        p.damage = 5.0f;
        p.category = cat;
        p.max_flight_distance = 3000.0f;
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
        make(cfg);
        add(1, s, 0, {-1000, 0, 0});
        add(2, e, 1, {0, 0, 0});
        sys->order_attack(1, 2);
    }
};

void test_missile_shield() {
    {  // an active MISSILE_SHIELD turns projectiles away (lasers too: category 7 goes to 0x4cec80)
        ShieldWorld w(ProjectileCategory::Laser, false);
        CHECK(w.ab->is_active(2, kMissileShield) && w.ab->immobilize_count(2) == 1);
        CHECK_EQ(w.sys->emitter_radius(2), 300.0f);
        bool seen_inside = false;
        for (int i = 0; i < 150; ++i) {
            w.tick();
            for (size_t k = 0; k < w.sys->projectile_count(); ++k) {
                const ProjectileView v = w.sys->projectile(k);
                const Vec3 d = v.pos - Vec3{0, 0, 0};
                if (dot(d, d) < 290.0f * 290.0f) seen_inside = true;
            }
        }
        CHECK(seen_inside);
        CHECK_EQ(w.sys->health(2), w.sys->max_health(2));  // never hit
        // off: immobilise released, emitter gone
        const ObjectId e[] = {2};
        CHECK(w.ab->set_unit_ability_mode(e, kMissileShield, false, w.frame) == 1);
        CHECK(w.ab->immobilize_count(2) == 0 && w.sys->emitter_radius(2) == 0.0f);
    }
    {  // bombs are never deflected
        ShieldWorld w(ProjectileCategory::Bomb, false);
        for (int i = 0; i < 150; ++i) w.tick();
        CHECK(w.sys->health(2) < w.sys->max_health(2));
    }
    {  // rockets: only the passive radius (here the passive emitter also has the ability record)
        ShieldWorld w(ProjectileCategory::Rocket, true);
        CHECK_EQ(w.sys->emitter_radius(2), 300.0f);  // passive emitter, radius overridden by ability 32
        for (int i = 0; i < 150; ++i) w.tick();
        CHECK_EQ(w.sys->health(2), w.sys->max_health(2));
    }
}

void test_opportunity_fire() {
    World w;
    ProjectileDesc p;
    p.name = "P";
    p.max_speed = 40.0f;
    p.damage = 1.0f;
    w.cd.projectiles = {p};
    HardpointDesc t;
    t.name = "HP_T";
    t.kind = HardpointKind::WeaponLaser;
    t.health = 100.0f;
    t.projectile = 0;
    t.min_recharge = 1.0f;
    t.max_recharge = 2.0f;  // <= 3.0 cutoff
    t.fire_range = 800.0f;
    t.allow_opportunity_fire_when_targeting = false;
    t.allow_opportunity_fire_when_idle = false;  // the cutoff alone allows it (every player)
    w.cd.hardpoints = {t};
    UnitDesc c;
    c.name = "C";
    c.tactical_health = 1e6f;
    c.radius = 20.0f;
    c.hardpoints = {0};
    c.max_attack_distance = 800.0f;
    const uint32_t ci = push(w.cd, w.ad, c, AbilityObjectDesc{});
    UnitDesc f;
    f.name = "F";
    f.tactical_health = 1e6f;
    f.radius = 5.0f;
    f.is_valid_target = true;
    const uint32_t fi = push(w.cd, w.ad, f, AbilityObjectDesc{});
    CombatConfig cfg;
    cfg.inaccuracy_enabled = false;
    w.make(cfg);
    w.add(1, ci, 0, {0, 0, 0});
    w.add(2, fi, 1, {300, 0, 0});
    w.add(3, fi, 1, {5000, 0, 0});
    w.sys->order_attack(1, 3);  // out of range: the hardpoint has no shot at the owner's target ...
    for (int i = 0; i < 80; ++i) w.tick();
    // ... the owner is not idle and Allow_Opportunity_Fire_When_Targeting is off -> no opportunity fire
    CHECK(w.sys->stats().shots == 0);
    w.cd.hardpoints[0].allow_opportunity_fire_when_targeting = true;
    for (int i = 0; i < 80; ++i) w.tick();
    CHECK(w.sys->stats().shots > 0);
    CHECK(w.sys->hardpoint_opportunity_target(1, 0) == 2);
    CHECK(w.sys->current_target(1) == 3);  // the owner's order is unchanged
}

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

void test_tractor() {
    SpecialWorld w;
    w.add(1, w.sd, 0, {0, 0, 0});
    w.add(2, w.corvette, 1, {500, 0, 0});
    w.add(3, w.frigate, 1, {0, 500, 0});
    w.add(4, w.frigate, 1, {0, 3000, 0});
    const ObjectId sd[] = {1};
    CHECK(w.ab->targeted_ability(sd, kTractorBeam, 2, -1, w.frame) == 1);
    CHECK(w.ab->pending_type(1) == kTractorBeam && w.ab->is_ready(1, kTractorBeam, w.frame));
    w.tick();
    CHECK(w.ab->pending_type(1) == -1 && w.ab->is_active(1, kTractorBeam));
    w.tick();  // OI abilities: the SAP update is staggered by the creation draw (due = now or now + 1)
    CHECK(w.ab->tractored(2));
    CHECK_EQ(w.sys->modifier(2, ModList::MaxSpeed), -0.99f);  // corvette: -2 x 0.75 clamped at -0.99
    CHECK_EQ(w.sys->mod_multiplier(1, ModKind::Speed), 0.6f);  // the user's Mod_Multiplier
    // re-target: the previous target's entry is removed (one target per ability)
    CHECK(w.ab->set_unit_ability_mode(sd, kTractorBeam, false, w.frame) == 1);
    w.tick();  // update sees 30 off -> remove, recharge 150
    CHECK(!w.ab->tractored(2) && w.sys->modifier(2, ModList::MaxSpeed) == 0.0f);
    CHECK(w.ab->countdown_counter(1, kTractorBeam) == 150);
    for (int i = 0; i < 150; ++i) w.tick();
    CHECK(w.ab->targeted_ability(sd, kTractorBeam, 3, -1, w.frame) == 1);
    w.tick();
    CHECK_EQ(w.sys->modifier(3, ModList::MaxSpeed), -0.75f);  // frigate: single
    // out of range targets stay pending (retried every targeting service)
    CHECK(w.ab->targeted_ability(sd, kTractorBeam, 4, -1, w.frame) == 1);  // accepted; stays pending while out of range
    w.tick();
    CHECK(w.ab->pending_type(1) == kTractorBeam && w.ab->tractored(3));
    // destroying the last tractor hardpoint ends it at the next update
    DamageRequest r;
    r.victim = 1;
    r.hardpoint = 0;
    r.amount = 1000.0f;
    w.sys->take_damage(r, w.frame);
    w.tick();
    CHECK(!w.ab->is_active(1, kTractorBeam) && w.sys->modifier(3, ModList::MaxSpeed) == 0.0f);
    CHECK(!w.ab->can_activate(1, kTractorBeam, false, true, w.frame));
}

void test_ion_and_lucky() {
    SpecialWorld w;
    w.add(1, w.frigate, 1, {0, 0, 0});  // target with two hardpoints (generators)
    w.add(2, w.ywing, 0, {-300, 0, 0}, {1, 0, 0}, 5);
    w.add(3, w.ywing, 0, {-300, 20, 0}, {1, 0, 0}, 5);
    const ObjectId mem[] = {2, 3};
    CHECK(w.ab->add_container(5, w.ysq, 0, mem, w.frame));
    w.add(6, w.xwing, 0, {-300, -40, 0});
    const ObjectId sq[] = {5};
    CHECK(w.ab->targeted_ability(sq, kIonCannonShot, 1, 1, w.frame) == 1);
    const ObjectId member[] = {2};
    CHECK(w.ab->targeted_ability(member, kIonCannonShot, 1, 1, w.frame) == 0);  // squad members skipped
    w.tick();
    CHECK(w.has_event(AbilityEventKind::EffectOn, 5) && w.has_event(AbilityEventKind::PendingFired, 5));
    CHECK(w.sys->ordered_hardpoint(2) == 1 && w.sys->ordered_target(3) == 1);
    uint32_t ion_shots = 0;
    for (size_t k = 0; k < w.sys->projectile_count(); ++k)
        if (w.sys->projectile(k).desc == 1) {
            ++ion_shots;
            CHECK(w.sys->projectile(k).target_hp == 1);
        }
    CHECK(ion_shots >= 1);  // the burst reset lets them fire the override projectile at once
    for (int i = 0; i < 60; ++i) w.tick();
    // one volley each with the override projectile: both members turned 41 off, the container's
    // update removed the effect and started the recharge (port rule, open item 14)
    CHECK(!w.ab->is_active(2, kIonCannonShot) && !w.ab->is_active(3, kIonCannonShot));
    CHECK(!w.ab->special_applied(5, name_crc("Ion")));
    CHECK(w.ab->countdown_phase(5, kIonCannonShot) == 0 && w.ab->countdown_counter(5, kIonCannonShot) > 500);
    CHECK(w.sys->ion_stunned(1, w.frame - 1) || w.sys->ion_stun_end(1) > 0);
    // lucky shot: x600 on the hit, consumed, recharge only because it fired
    const ObjectId x[] = {6};
    CHECK(w.ab->targeted_ability(x, kLuckyShot, 1, -1, w.frame) == 1);
    for (int i = 0; i < 60 && !w.sys->hardpoint_destroyed(1, 0) && !w.sys->hardpoint_destroyed(1, 1); ++i) w.tick();
    // 2 damage x 600 = 1200 on the hardpoint closest to the impact: destroyed
    CHECK(w.sys->hardpoint_destroyed(1, 0) || w.sys->hardpoint_destroyed(1, 1));
    w.tick();
    CHECK(!w.ab->is_active(6, kLuckyShot));
    CHECK(w.ab->countdown_counter(6, kLuckyShot) > 1700);  // 60 s recharge started
}

void test_concentrate_energy() {
    SpecialWorld w;
    w.add(1, w.sd, 0, {0, 0, 0});
    w.add(2, w.ywing, 0, {200, 0, 0});
    w.add(3, w.frigate, 1, {600, 0, 0});
    const ObjectId sd[] = {1};
    CHECK(w.ab->targeted_ability(sd, kConcentrateFire, 3, -1, w.frame) == 1);
    w.tick();
    CHECK(w.ab->is_active(1, kConcentrateFire) && w.sys->ordered_target(2) == 3);
    CHECK_EQ(w.sys->modifier(3, ModList::MaxSpeed), -0.1f);
    for (int i = 0; i < 90; ++i) w.tick();  // 3 s expiry
    CHECK(!w.ab->is_active(1, kConcentrateFire) && w.sys->modifier(3, ModList::MaxSpeed) == 0.0f);
    // energy weapon: 50 per update while active (2 s) and in range
    const float h0 = w.sys->health(3);
    CHECK(w.ab->targeted_ability(sd, kEnergyWeapon, 3, -1, w.frame) == 1);
    w.tick();
    CHECK(w.ab->is_active(1, kEnergyWeapon));
    for (int i = 0; i < 3; ++i) w.tick();
    CHECK(w.sys->health(3) < h0 || w.sys->hardpoint_destroyed(3, 0) || !w.sys->alive(3));
}

// Fog follow-up (FOG_OF_WAR.md §3.3): Force Sight = Reveal_All for the owner while channelling (undone at the end
// or on death) through set_fog_reveal_all; FOW_REVEAL_PING asks the host to spawn its Spawned_Object_Type.
void test_fog_abilities() {
    World w;
    UnitDesc yoda;
    yoda.name = "Yoda";
    yoda.tactical_health = 100.0f;
    AbilityObjectDesc ya;
    UnitAbilityRecord fsr = rec(kForceSight, 0.0f, 50.0f);
    fsr.gui_activated_ability_name = "Yoda_Force_Sight";
    fsr.gui_name_crc = name_crc("Yoda_Force_Sight");
    ya.unit_abilities.push_back(fsr);
    UnitAbilityRecord pr = rec(kFowRevealPing, 0.0f, 30.0f);
    pr.spawned_object_type = "Proj_Special_Land_FOW_Reveal_Ping_Blast";
    ya.unit_abilities.push_back(pr);
    SpecialAbilityDesc s;
    s.name = "Yoda_Force_Sight";
    s.class_tag = "Force_Sight_Ability";
    s.cls = SpecialClass::ForceSight;
    s.style = ActivationStyle::UserInput;
    s.force_sight_duration = 10.0f;
    ya.specials.push_back(add_special(w.ad, s));
    const uint32_t yd = push(w.cd, w.ad, yoda, ya);
    UnitDesc plain;
    plain.name = "Target";
    plain.tactical_health = 100.0f;
    const uint32_t pd = push(w.cd, w.ad, plain, AbilityObjectDesc{});
    AbilityConfig acfg;
    acfg.mode = GameMode::Land;
    CombatConfig ccfg;
    ccfg.mode = GameMode::Land;
    w.make(ccfg, acfg);
    std::vector<std::pair<int32_t, bool>> calls;
    w.ab->set_fog_reveal_all([&calls](int32_t p, bool on) { calls.emplace_back(p, on); });
    w.add(1, yd, 0, {0, 0, 0});
    w.add(2, pd, 1, {500, 0, 0});
    w.add(3, yd, 1, {0, 900, 0});
    const ObjectId y1[] = {1};
    // Force Sight: 10 s of Reveal_All for player 0, then undone; the 50 s recharge starts at activation.
    CHECK(w.ab->set_unit_ability_mode(y1, kForceSight, true, w.frame) == 1);
    CHECK(calls.size() == 1 && calls[0] == std::make_pair(int32_t{0}, true));
    CHECK(w.ab->special_applied(1, name_crc("Yoda_Force_Sight")) && w.has_event(AbilityEventKind::EffectOn, 1));
    CHECK(w.ab->countdown_counter(1, kForceSight) == 1500);
    for (int i = 0; i < 299; ++i) w.tick();
    CHECK(calls.size() == 1 && w.ab->special_applied(1, name_crc("Yoda_Force_Sight")));
    w.tick();
    // OI abilities: the first SAP update may come one frame later (creation stagger draw Get_Int(now, now + 1))
    if (calls.size() == 1) w.tick();
    CHECK(calls.size() == 2 && calls[1] == std::make_pair(int32_t{0}, false));
    CHECK(!w.ab->special_applied(1, name_crc("Yoda_Force_Sight")));
    CHECK(w.ab->set_unit_ability_mode(y1, kForceSight, true, w.frame) == 0);  // recharging
    CHECK(calls.size() == 2);
    // The user dies while channelling: undone for its owner.
    const ObjectId y3[] = {3};
    CHECK(w.ab->set_unit_ability_mode(y3, kForceSight, true, w.frame) == 1);
    CHECK(calls.size() == 3 && calls[2] == std::make_pair(int32_t{1}, true));
    w.sys->kill_unit(3, kNoObject, kKindNormal, w.frame);
    w.tick();
    CHECK(calls.size() == 4 && calls[3] == std::make_pair(int32_t{1}, false));
    // FOW_REVEAL_PING: a SpawnObject event for the host (target = the given object, else the user), recharge 30 s.
    w.ab->clear_events();
    CHECK(w.ab->set_unit_ability_mode(y1, kFowRevealPing, true, w.frame, 2) == 1);
    bool spawn = false;
    for (const AbilityEvent& e : w.ab->events())
        if (e.kind == AbilityEventKind::SpawnObject) spawn = e.id == 1 && e.target == 2 && e.type == kFowRevealPing;
    CHECK(spawn && !w.has_event(AbilityEventKind::Unhandled, 1));
    CHECK(w.ab->record(1, kFowRevealPing) && w.ab->record(1, kFowRevealPing)->spawned_object_type == pr.spawned_object_type);
    CHECK(w.ab->countdown_counter(1, kFowRevealPing) == 900);
    CHECK(w.ab->set_unit_ability_mode(y1, kFowRevealPing, true, w.frame) == 0);  // recharging
    // XML: the class tag and its duration field.
    const data::XmlDocument doc = data::XmlDocument::parse(
        "<Force_Sight_Ability Name=\"Yoda_Force_Sight\"><Activation_Style> User_Input </Activation_Style>"
        "<Force_Sight_Duration_In_Secs>10.0</Force_Sight_Duration_In_Secs></Force_Sight_Ability>",
        "fs.xml");
    const SpecialAbilityDesc ps = parse_special_ability(doc.root(), AbilityResolvers{}, nullptr);
    CHECK(ps.cls == SpecialClass::ForceSight && ps.force_sight_duration == 10.0f && ps.style == ActivationStyle::UserInput);
    CHECK(std::strcmp(special_class_name(SpecialClass::ForceSight), "Force_Sight_Ability") == 0);
}

// R5 jamming visual (ABILITY_VISUALS 3): Blob_* / Owner_Particle_* of Sensor_Jamming_Ability parse into the presentation fields.
void test_jamming_visual_fields() {
    const std::string xml =
        "<Abilities SubObjectList=\"Yes\">"
        "<Sensor_Jamming_Ability Name=\"UMDU_Sensor_Jamming\"><Activation_Style> COMBAT_AUTOMATIC </Activation_Style>"
        "<Duration_In_Secs>15.0</Duration_In_Secs><Blob_Color>200, 50, 200, 128</Blob_Color>"
        "<Blob_Material_Name>Sensor_Jamming_Blob</Blob_Material_Name><Show_Blob_To_Enemies>Yes</Show_Blob_To_Enemies>"
        "</Sensor_Jamming_Ability>"
        "<Sensor_Jamming_Ability Name=\"Skipray_Sensor_Jamming\"><Activation_Style> User_Input </Activation_Style>"
        "<Owner_Particle_Effect>Sensor_Jamming_Effect_Skipray</Owner_Particle_Effect>"
        "<Owner_Particle_Bone_Name>HP_Jam, ROOT</Owner_Particle_Bone_Name></Sensor_Jamming_Ability>"
        "<Tractor_Beam_Attack_Ability Name=\"T\"><Blob_Material_Name>Ignored</Blob_Material_Name></Tractor_Beam_Attack_Ability>"
        "</Abilities>";
    const data::XmlDocument doc = data::XmlDocument::parse(xml, "j.xml");
    AbilityResolvers r;
    const data::XmlNode block = doc.root();
    const SpecialAbilityDesc blob = parse_special_ability(block.first_child(), r, nullptr);
    CHECK(blob.cls == SpecialClass::SensorJamming && blob.blob_material == "Sensor_Jamming_Blob" && blob.show_blob_to_enemies);
    CHECK_EQ(blob.blob_color[0], 200.0f / 255.0f);
    CHECK_EQ(blob.blob_color[1], 50.0f / 255.0f);
    CHECK_EQ(blob.blob_color[3], 128.0f / 255.0f);
    CHECK(blob.owner_particle_effect.empty() && blob.owner_particle_bone.empty());
    const SpecialAbilityDesc part = parse_special_ability(block.first_child().next_sibling(), r, nullptr);
    CHECK(part.owner_particle_effect == "Sensor_Jamming_Effect_Skipray" && part.owner_particle_bone == "HP_Jam");
    CHECK(part.blob_material.empty() && !part.show_blob_to_enemies && part.blob_color[0] == 1.0f && part.blob_color[3] == 1.0f);
    const SpecialAbilityDesc other = parse_special_ability(block.first_child().next_sibling().next_sibling(), r, nullptr);
    CHECK(other.blob_material.empty());  // only the jammer class reads the Blob_* fields
}

void test_stealth_jamming_laser() {
    SpecialWorld w;
    w.add(1, w.corvette, 0, {0, 0, 0});
    w.add(2, w.frigate, 1, {2000, 0, 0});  // detector, range 600
    w.add(3, w.jammer, 1, {0, 3000, 0});
    w.add(4, w.ld, 1, {0, -1500, 0});  // missile boat
    const ObjectId c[] = {1};
    CHECK(w.ab->set_unit_ability_mode(c, kStealth, true, w.frame) == 1);
    CHECK(w.ab->is_stealthed(1, w.frame + 1));  // detection time 0: last detected 0 + 0 < now
    w.tick();
    CHECK(w.sys->flags(1) & kUnitStealthed);
    w.move(2, {500, 0, 0});  // within 600 -> detected
    w.tick();
    CHECK(!(w.sys->flags(1) & kUnitStealthed) && w.has_event(AbilityEventKind::StealthDetected, 1));
    w.move(2, {2000, 0, 0});
    w.tick();
    CHECK(w.sys->flags(1) & kUnitStealthed);
    for (int i = 0; i < 150; ++i) w.tick();  // 5 s duration
    CHECK(!w.ab->is_stealthed(1, w.frame));
    // jammer (Combat_Automatic, applied at spawn): OI abilities (TACTICAL_GAPS §1.5) - objects NOT hostile to the jammer's
    // owner within 1000 are jammed (it protects friendlies from homing missiles); enemies are not.
    w.move(1, {0, 2500, 0});
    w.move(2, {0, 2600, 0});
    w.tick();
    CHECK(!w.sys->is_jammed(1, w.frame - 1) && w.sys->is_jammed(2, w.frame - 1));
    w.move(2, {2000, 0, 0});
    CHECK(w.sys->emitter_radius(3) == 1000.0f && w.sys->emitter_entries(3) >= 1);  // a jammer registers on every apply
    {  // R5 jamming visual accessor (presentation only): the applied jammer special and the radius its update jams with
        float jam_r = 0.0f;
        const SpecialAbilityDesc* jd = w.ab->jamming_special(3, jam_r);
        CHECK(jd && jd->cls == SpecialClass::SensorJamming && jam_r == 1000.0f);
        CHECK(!w.ab->jamming_special(1, jam_r) && !w.ab->jamming_special(999, jam_r));
    }
    // laser defense on the corvette: zaps missiles of the boat within 400
    w.move(1, {0, -1000, 0});
    w.sys->order_attack(4, 1);
    CHECK(w.ab->set_unit_ability_mode(c, kLaserDefense, true, w.frame) == 1);
    bool zapped = false;
    for (int i = 0; i < 200; ++i) {
        w.tick();
        if (w.has_event(AbilityEventKind::Zap, 1)) zapped = true;
    }
    CHECK(zapped);
    // redirect blaster: block chance 1 -> laser hits from unit 4 are cancelled (one Get_Float pair)
    DamageRequest r;
    r.victim = 1;
    r.amount = 10.0f;
    r.source = 4;
    r.source_is_projectile = true;
    r.projectile_desc = 0;
    const float h = w.sys->health(1);
    SimRng pred = w.rng;
    pred.range_float(0.0f, 1.0f);
    pred.range_float(0.0f, 1.0f);
    const DamageResult res = w.sys->take_damage(r, w.frame);
    CHECK(res.cancelled && w.sys->health(1) == h && w.rng.state() == pred.state());
}

// P4 tactical: the Black Market gate on special abilities (0x39b0b0, part of the generic trigger) and on a
// targeted unit ability (command 40).
void test_black_market_specials() {
    SpecialWorld w;
    w.ad.specials[w.jam_sp].must_be_bought_on_black_market = true;
    w.ad.objects[w.sd].unit_abilities[0].must_be_bought_on_black_market = true;  // TRACTOR_BEAM record
    CombatConfig cfg;
    cfg.inaccuracy_enabled = false;
    cfg.opportunity_fire = false;
    cfg.object_max_health_multiplier = 1.0f;
    cfg.hardpoint_health_multiplier = 1.0f;
    AbilityConfig acfg;
    acfg.skirmish = false;
    w.make(cfg, acfg);
    w.ab->set_black_market_blocked(0, {name_crc("Tractor"), name_crc("Jam")});
    w.add(1, w.jammer, 0, {0, 5000, 0});
    w.add(2, w.jammer, 1, {0, -5000, 0});
    w.add(3, w.sd, 0, {0, 0, 0});
    w.add(4, w.corvette, 1, {500, 0, 0});
    // Combat_Automatic jamming applies at spawn only where the gate passes.
    CHECK(!w.ab->special_applied(1, name_crc("Jam")) && w.ab->special_applied(2, name_crc("Jam")));
    const ObjectId sd[] = {3};
    CHECK(!w.ab->is_ready(3, kTractorBeam, w.frame));
    CHECK(w.ab->targeted_ability(sd, kTractorBeam, 4, -1, w.frame) == 0 && w.ab->pending_type(3) == -1);
    w.ab->set_black_market_blocked(0, {name_crc("Jam")});  // the tractor item bought
    CHECK(w.ab->targeted_ability(sd, kTractorBeam, 4, -1, w.frame) == 1);
    w.tick();
    w.tick();  // OI abilities: SAP update stagger
    CHECK(w.ab->is_active(3, kTractorBeam) && w.ab->tractored(4));
}

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

UnitDesc oi_unit(const char* name, float health, float radius = 5.0f, uint64_t cats = kCatFighter) {
    UnitDesc u;
    u.name = name;
    u.tactical_health = health;
    u.radius = radius;
    u.category_mask = cats;
    return u;
}

SpecialAbilityDesc oi_special(const char* name, SpecialClass c, ActivationStyle st) {
    SpecialAbilityDesc s;
    s.name = name;
    s.cls = c;
    s.style = st;
    s.applicable_categories = ~uint64_t{0};
    return s;
}

UnitAbilityRecord oi_gui(uint8_t t, float exp, float rch, const char* name) {
    UnitAbilityRecord r = rec(t, exp, rch);
    r.gui_activated_ability_name = name;
    r.gui_name_crc = name_crc(name);
    return r;
}

void test_oi_tables() {
    // §1.1: HUNT / LURE M = 1 (slot byte), behaviours 0x21 / 0x23; AVOID_DANGER B = -1.
    CHECK(ability_type_info(kHunt).mode == 1 && ability_type_info(kHunt).behavior == 0x21);
    CHECK(ability_type_info(kLure).mode == 1 && ability_type_info(kLure).behavior == 0x23);
    CHECK(ability_type_info(kAvoidDanger).behavior == kBehaviorNone && ability_type_info(kDeployTroopers).mode == 0);
    // §1.4 service intervals
    using S = SpecialClass;
    for (S c : {S::CombatBonus, S::BattlefieldModifier, S::EnhanceDefense, S::RedirectBlaster, S::AbsorbBlaster, S::EnableAbility,
                S::ClusterBomb, S::VehicleThief, S::WeaponSwap, S::BasePower, S::CancelSpecial, S::Unsupported})
        CHECK(special_service_interval(c, 30) == 0);
    for (S c : {S::Berserker, S::ForceHealing, S::ArcSweep, S::SuperLaser, S::DrainLife, S::Stealth, S::LaserDefense, S::Summon})
        CHECK(special_service_interval(c, 30) == 1);
    CHECK(special_service_interval(S::Earthquake, 30) == 3 && special_service_interval(S::Telekinesis, 31) == 3);
    CHECK(special_class_from_tag("force_healing_ability") == S::ForceHealing && special_class_from_tag("Stun_Ability") == S::StunWave);
    // CubicInterpolator (§3.3): the shipped Whirlwind chance 0,0 1,0 5,1 500,1
    CubicCurve c;
    c.set({{5.0f, 1.0f}, {0.0f, 0.0f}, {500.0f, 1.0f}, {1.0f, 0.0f}});
    CHECK(c.eval(-3.0f) == 0.0f && c.eval(900.0f) == 1.0f && c.eval(1.0f) == 0.0f && c.eval(5.0f) == 1.0f);
    CHECK(c.eval(3.0f) > 0.0f && c.eval(3.0f) < 1.0f);
    // XML class fields
    const data::XmlDocument doc = data::XmlDocument::parse(
        "<Abilities>"
        "<Force_Whirlwind_Ability Name=\"WW\"><Activation_Style>User_Input</Activation_Style>"
        "<Activation_Chance>0,0.0, 1,0.0, 5,1.0, 500,1.0</Activation_Chance><Damage_Amount>45.0</Damage_Amount>"
        "<Damage_Range>75.0</Damage_Range><Travel_Time_In_Secs>1.0</Travel_Time_In_Secs></Force_Whirlwind_Ability>"
        "<Combat_Bonus_Ability Name=\"CB\"><Activation_Style>Space_Automatic</Activation_Style>"
        "<Damage_Bonus_Percentage>0.15</Damage_Bonus_Percentage><Movement_Speed_Bonus_Percentage>0.06</Movement_Speed_Bonus_Percentage>"
        "<Stacking_Category>3</Stacking_Category></Combat_Bonus_Ability>"
        "<Proximity_Mines_Ability Name=\"PM\"><Mine_Type>Mine_X</Mine_Type><Number_Of_Mines>6</Number_Of_Mines>"
        "<Trigger_Radius>10</Trigger_Radius><Activation_Time_In_Seconds>0.5</Activation_Time_In_Seconds></Proximity_Mines_Ability>"
        "<Earthquake_Attack_Ability Name=\"EQ\"><Activation_Chance>1.0</Activation_Chance><Damage_Percentage>0.02</Damage_Percentage>"
        "<Damage_Interval_In_Secs>0.5</Damage_Interval_In_Secs></Earthquake_Attack_Ability>"
        "</Abilities>",
        "oi.xml");
    AbilityResolvers r;
    r.object_type = [](std::string_view n) { return iequals(n, "Mine_X") ? 42 : -1; };
    std::vector<SpecialAbilityDesc> v;
    for (data::XmlNode e = doc.root().first_child(); e; e = e.next_sibling()) v.push_back(parse_special_ability(e, r, nullptr));
    CHECK(v.size() == 4);
    if (v.size() == 4) {
        CHECK(v[0].cls == S::ForceWhirlwind && !v[0].chance_curve.empty() && v[0].damage_amount == 45.0f && v[0].damage_range == 75.0f &&
              v[0].travel_time == 1.0f);
        CHECK(v[1].cls == S::CombatBonus && v[1].bonus[static_cast<size_t>(ModList::Damage)] == 0.15f &&
              v[1].bonus[static_cast<size_t>(ModList::MaxSpeed)] == 0.06f && v[1].stacking_category == 3);
        CHECK(v[2].spawn_type == 42 && v[2].spawn_count == 6 && v[2].trigger_radius == 10.0f && v[2].activation_time == 0.5f);
        CHECK(v[3].activation_chance == 1.0f && v[3].damage_percentage == 0.02f && v[3].damage_interval == 0.5f);
    }
}

void test_oi_sap_init_and_defaults() {
    // §4.3: one synced Get_Int(now, now + interval) per special with interval > 0 allowed in the mode, list order,
    // after the hardpoint draws. §1.2: Active_By_Default for the FIRST TWO Unit_Abilities_Data entries.
    OiWorld w;
    const uint32_t eq = add_special(w.ad, oi_special("EQ", SpecialClass::Earthquake, ActivationStyle::SpecialAttack));       // 3
    const uint32_t cb = add_special(w.ad, oi_special("CB", SpecialClass::CombatBonus, ActivationStyle::SpaceAutomatic));     // 0
    const uint32_t fh = add_special(w.ad, oi_special("FH", SpecialClass::ForceHealing, ActivationStyle::GroundAutomatic));   // land only
    const uint32_t ld = add_special(w.ad, oi_special("LD", SpecialClass::LaserDefense, ActivationStyle::UserInput));         // 1
    AbilityObjectDesc a;
    UnitAbilityRecord turbo = rec(kTurbo, 0.0f, 0.0f), sprint = rec(kSprint, 2.0f, 4.0f), p2w = rec(kPowerToWeapons, 0.0f, 0.0f);
    turbo.active_by_default = true;
    p2w.active_by_default = true;  // third entry: not applied (0x3a82a0 reads entries 0 and 1)
    a.unit_abilities = {turbo, sprint, p2w};
    a.specials = {eq, cb, fh, ld};
    const uint32_t u = push(w.cd, w.ad, oi_unit("U", 100.0f), a);
    w.make();
    SimRng pred = w.rng;
    pred.range_int(1, 4);  // EQ: Get_Int(1, 1 + 3)
    pred.range_int(1, 2);  // LD: Get_Int(1, 1 + 1); CB interval 0, FH not allowed in space: no draw
    w.add(1, u, 0, {0, 0, 0});
    CHECK(w.rng.state() == pred.state());
    CHECK(w.ab->is_active(1, kTurbo) && !w.ab->is_active(1, kPowerToWeapons));
    const ObjectId one[] = {1};
    CHECK(w.ab->set_unit_ability_mode(one, kTurbo, false, w.frame) == 1 && !w.ab->is_active(1, kTurbo));
    w.ab->apply_active_by_default(1, w.frame);  // AI free-store / task-force release
    CHECK(w.ab->is_active(1, kTurbo) && !w.ab->is_active(1, kPowerToWeapons));
    // order cancel (0x38f5d0): HUNT goes off on a new order
    OiWorld h;
    AbilityObjectDesc ha;
    ha.unit_abilities = {rec(kHunt, 0.0f, 0.0f)};
    const uint32_t hu = push(h.cd, h.ad, oi_unit("H", 100.0f), ha);
    h.make();
    h.add(1, hu, 0, {0, 0, 0});
    CHECK(h.ab->set_unit_ability_mode(one, kHunt, true, h.frame) == 1 && h.ab->is_active(1, kHunt) && h.ab->hunting(1));
    h.ab->order_cancel(1, h.frame);
    CHECK(!h.ab->is_active(1, kHunt) && !h.ab->hunting(1));
}

void test_oi_autofire() {
    // §1.3: autofire on = the map entry, else AI-controlled owner; the setter refuses records without
    // Supports_Autofire unless the owner is AI; the driver makes acquired targets pending abilities.
    auto build = [](OiWorld& w, bool two, bool ai) {
        ProjectileDesc p;
        p.name = "Laser";
        p.max_speed = 40.0f;
        p.damage = 1.0f;
        w.cd.projectiles = {p};
        const uint32_t hk = add_special(w.ad, oi_special("Hack", SpecialClass::Hack, ActivationStyle::UserInput));
        const uint32_t br = add_special(w.ad, oi_special("Bribe", SpecialClass::TacticalBribe, ActivationStyle::UserInput));
        w.ad.specials[hk].max_range = 1000.0f;
        w.ad.specials[br].max_range = 1000.0f;
        UnitDesc u = oi_unit("A", 100.0f);
        u.max_attack_distance = 800.0f;
        u.weapon_projectile = 0;
        u.weapon_recharge = 1.0f;
        AbilityObjectDesc a;
        a.unit_abilities = {oi_gui(kTargetedHack, 0.0f, 10.0f, "Hack")};
        a.specials = {hk};
        if (two) {
            a.unit_abilities.push_back(oi_gui(kTacticalBribe, 0.0f, 10.0f, "Bribe"));
            a.specials.push_back(br);
        }
        const uint32_t ad = push(w.cd, w.ad, u, a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("T", 100.0f), AbilityObjectDesc{});
        std::vector<PlayerInfo> pl(3);
        pl[0].ai = ai;
        w.make(GameMode::Space, pl);
        w.add(1, ad, 0, {0, 0, 0});
        w.add(2, td, 1, {300, 0, 0});
    };
    OiWorld human;
    build(human, false, false);
    CHECK(!human.ab->autofire_on(1, kTargetedHack));
    CHECK(!human.ab->set_ability_autofire(1, kTargetedHack, true));  // no Supports_Autofire, human owner
    human.tick();
    CHECK(human.ab->pending_type(1) == -1);  // User_Input with autofire off: no candidate
    OiWorld ai1;
    build(ai1, false, true);
    CHECK(ai1.ab->autofire_on(1, kTargetedHack));
    ai1.tick();  // scan acquires 2 -> Issue_Auto_Target -> the single candidate becomes the pending ability
    CHECK(ai1.ab->pending_type(1) == kTargetedHack && ai1.ab->pending_target(1) == 2);
    for (int i = 0; i < 70; ++i) ai1.tick();  // pending fires, the hack ends after its animation [port 60 frames]
    CHECK(ai1.sys->owner(2) == 0 && ai1.count(AbilityEventKind::OwnerChanged, 2) == 1);
    // the map entry overrides the AI default
    OiWorld ai2;
    build(ai2, true, true);
    CHECK(ai2.ab->set_ability_autofire(1, kTargetedHack, false) && !ai2.ab->autofire_on(1, kTargetedHack));
    ai2.tick();
    CHECK(ai2.ab->pending_type(1) == kTacticalBribe);  // the only candidate left
    // two candidates: one synced Get_Int(0, 1) pick; identical for identical seeds
    OiWorld a3, b3;
    build(a3, true, true);
    build(b3, true, true);
    a3.tick();
    b3.tick();
    CHECK((a3.ab->pending_type(1) == kTargetedHack || a3.ab->pending_type(1) == kTacticalBribe) &&
          a3.ab->pending_type(1) == b3.ab->pending_type(1) && a3.rng.state() == b3.rng.state());
}

void test_oi_hunt() {
    // §2.2 HUNT: interval 30; the decision draws u, (u > 0.5: 99 x 2 point draws, the fogged ones), then the object pick
    // (no hidden objects -> any object), then 2 jitter draws Get_Float(30, 400); a move order. Weaponless units: these
    // are the only draws of the tick.
    OiWorld w;
    AbilityObjectDesc ha;
    ha.unit_abilities = {rec(kHunt, 0.0f, 0.0f)};
    const uint32_t hu = push(w.cd, w.ad, oi_unit("H", 100.0f), ha);
    const uint32_t td = push(w.cd, w.ad, oi_unit("T", 100.0f), AbilityObjectDesc{});
    w.make();
    w.add(1, hu, 0, {0, 0, 0});
    w.add(2, td, 1, {400, 100, 0});
    w.add(3, td, 1, {-200, 600, 0});
    const ObjectId one[] = {1};
    CHECK(w.ab->set_unit_ability_mode(one, kHunt, true, w.frame) == 1);
    SimRng pred = w.rng;
    const float u = pred.range_float(0.0f, 1.0f);
    if (u > 0.5f)
        for (int i = 0; i < 99; ++i) {
            pred.range_float(-1000.0f, 1000.0f);
            pred.range_float(-1000.0f, 1000.0f);
        }
    const int32_t k = pred.range_int(0, 1);  // objects 2, 3 (ascending id)
    Vec3 dest = k == 0 ? Vec3{400, 100, 0} : Vec3{-200, 600, 0};
    dest.x += pred.range_float(30.0f, 400.0f);
    dest.y += pred.range_float(30.0f, 400.0f);
    w.tick();
    CHECK(w.rng.state() == pred.state());
    const AbilityEvent* mv = w.last(AbilityEventKind::MoveOrder, 1);
    CHECK(mv && mv->point.x == dest.x && mv->point.y == dest.y);
    const uint64_t d0 = w.rng.draws();
    for (int i = 0; i < 29; ++i) w.tick();
    CHECK(w.rng.draws() == d0);  // next decision 30 frames later
    w.tick();
    CHECK(w.rng.draws() > d0 && w.count(AbilityEventKind::MoveOrder, 1) == 2);
}

void test_oi_waves_and_confuse() {
    // §2.3.7 (land): Stun - STUNNABLE victims for int(Stun_Time x 30.0) frames; Confuse - 2 synced draws per victim.
    OiWorld w;
    SpecialAbilityDesc st = oi_special("Stun", SpecialClass::StunWave, ActivationStyle::UserInput);
    st.wave_range = 100.0f;
    st.wave_time = 2.0f;
    const uint32_t sts = add_special(w.ad, st);
    SpecialAbilityDesc cf = oi_special("Confuse", SpecialClass::ForceConfuse, ActivationStyle::UserInput);
    cf.wave_range = 100.0f;
    cf.wave_time = 4.0f;
    const uint32_t cfs = add_special(w.ad, cf);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kStun, 0.0f, 20.0f, "Stun"), oi_gui(kForceConfuse, 0.0f, 20.0f, "Confuse")};
    a.specials = {sts, cfs};
    const uint32_t ud = push(w.cd, w.ad, oi_unit("Jedi", 100.0f), a);
    UnitDesc vs = oi_unit("Trooper", 100.0f);
    vs.stunnable = true;
    const uint32_t vd = push(w.cd, w.ad, vs, AbilityObjectDesc{});
    const uint32_t nd = push(w.cd, w.ad, oi_unit("Droid", 100.0f), AbilityObjectDesc{});
    w.make(GameMode::Land);
    w.add(1, ud, 0, {0, 0, 0});
    w.add(2, vd, 1, {50, 0, 0});
    w.add(3, vd, 1, {300, 0, 0});  // out of range
    w.add(4, nd, 1, {0, 60, 0});   // not STUNNABLE
    const ObjectId one[] = {1};
    CHECK(w.ab->set_unit_ability_mode(one, kStun, true, w.frame) == 1);
    CHECK(w.sys->stunned(2, w.frame) && !w.sys->stunned(3, w.frame) && !w.sys->stunned(4, w.frame));
    CHECK(w.ab->countdown_counter(1, kStun) == 600);  // recharge started at apply
    for (int i = 0; i < 61; ++i) w.tick();  // end frame = activation + 60: released at that frame's update
    CHECK(!w.sys->stunned(2, w.frame) && w.count(AbilityEventKind::Stunned, 2) == 2);  // stunned, then released
    // Confuse: victims 2 and 4 (ascending id), 2 draws each in the map rectangle
    SimRng pred = w.rng;
    const float x2 = pred.range_float(-1000.0f, 1000.0f), y2 = pred.range_float(-1000.0f, 1000.0f);
    const float x4 = pred.range_float(-1000.0f, 1000.0f), y4 = pred.range_float(-1000.0f, 1000.0f);
    CHECK(w.ab->set_unit_ability_mode(one, kForceConfuse, true, w.frame) == 1);
    CHECK(w.rng.state() == pred.state());
    w.collect();
    const AbilityEvent* m2 = w.last(AbilityEventKind::MoveOrder, 2);
    const AbilityEvent* m4 = w.last(AbilityEventKind::MoveOrder, 4);
    CHECK(m2 && m4 && m2->point.x == x2 && m2->point.y == y2 && m4->point.x == x4 && m4->point.y == y4);
}

void test_oi_melee_and_chance() {
    // §2.3.8 / §2.3.4: one synced draw per apply attempt that passed the range test; a chance of 0 refuses without a draw.
    OiWorld w;
    SpecialAbilityDesc g = oi_special("Pound", SpecialClass::GenericAttack, ActivationStyle::UserInput);
    g.max_range = 100.0f;
    g.activation_chance = 0.0f;
    g.damage_amount = 30.0f;
    g.damage_frame = 10;
    const uint32_t gs = add_special(w.ad, g);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kTargetedHack, 0.0f, 0.0f, "Pound")};  // any targeted slot forwarding to the class
    a.specials = {gs};
    const uint32_t ud = push(w.cd, w.ad, oi_unit("Rancor", 1000.0f), a);
    const uint32_t td = push(w.cd, w.ad, oi_unit("Victim", 100.0f), AbilityObjectDesc{});
    w.make(GameMode::Land);
    w.add(1, ud, 0, {0, 0, 0});
    w.add(2, td, 1, {50, 0, 0});
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kTargetedHack, 2, -1, w.frame) == 1);
    const uint64_t d0 = w.rng.draws();
    for (int i = 0; i < 5; ++i) w.tick();
    CHECK(w.rng.draws() == d0 && w.ab->pending_type(1) == kTargetedHack);  // chance 0: no draw, stays pending
    w.ad.specials[gs].activation_chance = 1.0f;
    SimRng pred = w.rng;
    CHECK(pred.inline_unit_float() <= 1.0f);
    w.tick();  // one draw <= 1.0, applies; the hit at animation frame 10
    CHECK(w.rng.state() == pred.state() && w.ab->pending_type(1) == -1);
    for (int i = 0; i < 9; ++i) w.tick();
    CHECK(w.sys->health(2) == 100.0f);
    w.tick();
    CHECK_EQ(w.sys->health(2), 70.0f);
}

void test_oi_mines() {
    // §2.3.3: per mine 2 synced draws (angle Get_Float(0, 360) degrees, radius Get_Float(-R, R)), R = unit ability 67's
    // Area_Effect_Decal_Distance; arm frame = now + int(FPS x activation) + 1; trigger checks on frames = 0 (mod 5).
    OiWorld w;
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
    w.add(2, dd, 1, {100, 0, 0}, {1, 0, 0}, kNoObject, kUnitSkipTargeting | kUnitInvulnerable);  // the command-57 dummy
    w.add(3, td, 1, {800, 0, 0});
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kProximityMines, 2, -1, w.frame) == 1);
    SimRng pred = w.rng;
    std::vector<Vec3> want;
    for (int i = 0; i < 3; ++i) {
        const float ang = pred.range_float(0.0f, 360.0f) * 0.017453292519943295f;
        const float rad = pred.range_float(-20.0f, 20.0f);
        float sn = 0.0f, cs = 1.0f;
        dmath::sincos(ang, sn, cs);
        want.push_back(Vec3{100.0f + cs * rad, sn * rad, 0.0f});
    }
    const uint32_t f0 = w.frame;
    w.ab->tick(w.frame, w.trees, w.rng, serial_for);  // the pending ability applies; spawn requests of this tick
    CHECK(w.rng.state() == pred.state());
    CHECK(w.ab->spawn_requests().size() == 3 && w.ab->mine_count() == 3);
    for (size_t i = 0; i < w.ab->spawn_requests().size() && i < 3; ++i) {
        const SpawnRequest& q = w.ab->spawn_requests()[i];
        CHECK(q.purpose == SpawnPurpose::Mine && q.object_type == 9 && q.position.x == want[i].x && q.position.y == want[i].y);
    }
    w.collect();
    ++w.frame;
    // arm = f0 + int(30 x 0.5) + 1 = f0 + 16; the tank drives onto the first mine
    w.move(3, Vec3{want[0].x + 1.0f, want[0].y, 0.0f});
    while (w.frame < f0 + 16) w.tick();
    CHECK(w.count(AbilityEventKind::MineTriggered) == 0);
    while (w.frame % 5u != 0u) w.tick();
    w.tick();
    CHECK(w.count(AbilityEventKind::MineTriggered) == 1 && w.ab->mine_count() == 2);
}

void test_oi_death_draws() {
    // §1.9: Die step 7 - Destruction_Survivors count Get_Int(min, max), two inline draws per survivor (u >= 1 - total,
    // v x total picks the entry); then the DeathBehavior spin-away Get_Float(0, 1) <= chance. Respawn first.
    OiWorld w;
    UnitDesc u = oi_unit("Bike", 100.0f);
    u.object_type = 11;
    u.destruction_survivors = {{21, 0.5f}, {22, 1.0f}};
    u.min_survivors = 2;
    u.max_survivors = 3;
    u.spin_away_on_death = true;
    u.spin_away_chance = 0.5f;
    u.tactical_respawn_secs = 2.0f;
    const uint32_t ud = push(w.cd, w.ad, u, AbilityObjectDesc{});
    w.make(GameMode::Land);
    w.add(1, ud, 0, {10, 20, 0});
    SimRng pred = w.rng;
    const int32_t n = pred.range_int(2, 3);
    std::vector<int32_t> types;
    for (int32_t i = 0; i < n; ++i) {
        const float a = pred.inline_unit_float();
        const float v = pred.inline_unit_float() * 1.0f;
        if (a >= 0.0f) types.push_back(v <= 0.5f ? 21 : 22);
    }
    const bool spin = pred.range_float(0.0f, 1.0f) <= 0.5f;
    w.sys->kill_unit(1, kNoObject, kKindNormal, w.frame);
    CHECK(w.rng.state() == pred.state());
    const auto ev = w.sys->survivor_events();
    CHECK(ev.size() == types.size() + 1 && !ev.empty() && ev[0].respawn && ev[0].type == 11 && ev[0].due_frame == w.frame + 60);
    for (size_t i = 0; i < types.size() && i + 1 < ev.size(); ++i) CHECK(ev[i + 1].type == types[i] && !ev[i + 1].respawn);
    CHECK(w.sys->deaths().size() == 1 && w.sys->deaths()[0].spin_away == spin);
    // §1.8 burning: interval 30, first due = ignition + Get_Int(0, 29)
    OiWorld b;
    UnitDesc c = oi_unit("Hut", 100.0f);
    c.combustible = true;
    c.burning_damage_per_second = 5.0f;
    const uint32_t cd = push(b.cd, b.ad, c, AbilityObjectDesc{});
    b.make(GameMode::Land);
    b.add(1, cd, 0, {0, 0, 0});
    SimRng bp = b.rng;
    const int32_t first = bp.range_int(0, 29);
    DamageRequest r;
    r.victim = 1;
    r.kind = kKindFire;
    r.amount = 10.0f;
    b.sys->take_damage(r, b.frame);
    CHECK(b.sys->burning(1) && b.sys->health(1) == 100.0f && b.rng.state() == bp.state());
    const uint32_t due = b.frame + static_cast<uint32_t>(first);
    while (b.frame < due) b.tick();
    CHECK(b.sys->health(1) == 100.0f);
    b.tick();
    CHECK_EQ(b.sys->health(1), 95.0f);
    for (int i = 0; i < 29; ++i) b.tick();
    CHECK_EQ(b.sys->health(1), 95.0f);
    b.tick();
    CHECK_EQ(b.sys->health(1), 90.0f);
}

void test_oi_auras() {
    // TACTICAL_COMBAT §4.3: Combat_Bonus inserts one entry per non-zero field into each matching unit's set (append,
    // permanent, source = the owner); event-driven: new units get it, the source's death removes it. Two sources in one
    // stacking category count once. §2.3.11 Battlefield_Modifier: the fog list (+0x1f8, multiplier) per affected player
    // and the stealth list (+0x268, m - 1). §7.3 Base_Power: Requires_Base_Power units do not act without it.
    OiWorld w;
    SpecialAbilityDesc s = oi_special("Aura", SpecialClass::CombatBonus, ActivationStyle::SpaceAutomatic);
    s.bonus[static_cast<size_t>(ModList::Damage)] = 0.25f;
    s.bonus[static_cast<size_t>(ModList::MaxHealth)] = 0.5f;
    s.stacking_category = 1;
    const uint32_t as = add_special(w.ad, s);
    SpecialAbilityDesc bm = oi_special("BM", SpecialClass::BattlefieldModifier, ActivationStyle::SpaceAutomatic);
    bm.fow_reveal_multiplier = 1.5f;
    bm.stealth_duration_multiplier = 1.25f;
    bm.apply_to_all_allies = true;
    const uint32_t bs = add_special(w.ad, bm);
    AbilityObjectDesc hero;
    hero.specials = {as, bs};
    const uint32_t hd = push(w.cd, w.ad, oi_unit("Hero", 100.0f), hero);
    const uint32_t ud = push(w.cd, w.ad, oi_unit("Ship", 200.0f), AbilityObjectDesc{});
    w.make();
    std::vector<std::pair<int32_t, float>> fog;
    std::vector<int32_t> fog_off;
    AbilityHost h;
    h.fow_reveal_add = [&fog](int32_t p, int32_t, uint64_t, float m) { fog.emplace_back(p, m); };
    h.fow_reveal_remove = [&fog_off](int32_t p, uint64_t) { fog_off.push_back(p); };
    w.ab->set_host(std::move(h));
    w.sys->set_enemy(0, 2, false);  // player 2 is allied to 0
    w.sys->set_enemy(2, 0, false);
    w.add(1, ud, 0, {0, 0, 0});
    w.add(2, hd, 0, {100, 0, 0});
    w.add(3, ud, 0, {200, 0, 0});
    w.add(4, ud, 1, {900, 0, 0});
    w.add(5, hd, 0, {300, 0, 0});  // a second source, same stacking category
    CHECK_EQ(w.sys->modifier(1, ModList::Damage), 0.25f);
    CHECK_EQ(w.sys->modifier(3, ModList::Damage), 0.25f);  // added after the source
    CHECK_EQ(w.sys->modifier(2, ModList::Damage), 0.25f);  // the source itself matches
    CHECK_EQ(w.sys->modifier(4, ModList::Damage), 0.0f);   // other owner
    CHECK_EQ(w.sys->max_health(1), 300.0f);
    CHECK_EQ(w.sys->health(1), 300.0f);  // current += new max - old max
    CHECK(fog.size() == 4 && fog[0] == std::make_pair(int32_t{0}, 1.5f) && fog[1] == std::make_pair(int32_t{2}, 1.5f));
    CHECK_EQ(w.ab->player_list(0, PlayerList::StealthDuration), 0.25f);  // two sources, one category: max
    CHECK_EQ(w.ab->player_list(1, PlayerList::StealthDuration), 0.0f);
    w.sys->kill_unit(2, kNoObject, kKindNormal, w.frame);
    CHECK_EQ(w.sys->modifier(1, ModList::Damage), 0.25f);  // the second source remains
    w.sys->kill_unit(5, kNoObject, kKindNormal, w.frame);
    CHECK_EQ(w.sys->modifier(1, ModList::Damage), 0.0f);
    CHECK_EQ(w.sys->max_health(1), 200.0f);
    CHECK_EQ(w.sys->health(1), 200.0f);
    CHECK(fog_off.size() == 6 && w.ab->player_list(0, PlayerList::StealthDuration) == 0.0f);
    // Base power
    OiWorld p;
    ProjectileDesc laser;
    laser.name = "L";
    laser.max_speed = 50.0f;
    laser.damage = 1.0f;
    p.cd.projectiles = {laser};
    UnitDesc tower = oi_unit("Tower", 500.0f, 10.0f);
    tower.requires_base_power = true;
    tower.max_attack_distance = 1000.0f;
    tower.weapon_projectile = 0;
    tower.weapon_recharge = 0.1f;
    const uint32_t tw = push(p.cd, p.ad, tower, AbilityObjectDesc{});
    const uint32_t gs = add_special(p.ad, oi_special("Power", SpecialClass::BasePower, ActivationStyle::GroundAutomatic));
    AbilityObjectDesc ga;
    ga.specials = {gs};
    const uint32_t gd = push(p.cd, p.ad, oi_unit("Generator", 500.0f), ga);
    const uint32_t td = push(p.cd, p.ad, oi_unit("Target", 1e6f), AbilityObjectDesc{});
    p.make(GameMode::Land);
    p.add(1, tw, 0, {0, 0, 0});
    p.add(2, td, 1, {300, 0, 0});
    for (int i = 0; i < 30; ++i) p.tick();
    CHECK(!p.sys->has_base_power(0) && p.sys->stats().shots == 0);  // an empty list is no power: the tower is dead
    p.add(3, gd, 0, {-500, 0, 0});
    CHECK(p.sys->has_base_power(0) && !p.sys->has_base_power(1));
    for (int i = 0; i < 30; ++i) p.tick();
    CHECK(p.sys->stats().shots > 0);
    p.sys->kill_unit(3, kNoObject, kKindNormal, p.frame);
    CHECK(!p.sys->has_base_power(0));
}

void test_oi_repair_priorities() {
    // §1.12: each repairer pays Repair_Cost_Per_Frame and adds Repair_Amount_Per_Frame per frame, list order; one who
    // cannot pay is dropped; full health clears the list.
    OiWorld w;
    HardpointDesc gen;
    gen.name = "HP_Gen";
    gen.kind = HardpointKind::ShieldGenerator;
    gen.health = 100.0f;
    gen.destroyable = true;
    gen.targetable = true;
    gen.repair_cost_per_frame = 1.0f;
    gen.repair_amount_per_frame = 2.0f;
    w.cd.hardpoints = {gen};
    UnitDesc st = oi_unit("Station", 1000.0f, 50.0f, kCatCapital);
    st.hardpoints = {0};
    const uint32_t sd = push(w.cd, w.ad, st, AbilityObjectDesc{});
    w.make();
    std::array<float, 3> credits{3.0f, 0.0f, 0.0f};
    w.sys->set_credit_hooks([&credits](int32_t p) { return credits[static_cast<size_t>(p)]; },
                            [&credits](int32_t p, float d) { credits[static_cast<size_t>(p)] += d; });
    w.add(1, sd, 0, {0, 0, 0});
    w.sys->set_hardpoint_health(1, 0, 90.0f);
    CHECK(w.ab->repair_hardpoint(1, 0, 1) && w.ab->repair_hardpoint(1, 0, 0) && w.ab->repair_hardpoint(1, 0, 0));
    CHECK(w.sys->hardpoint_repairers(1, 0) == 2);  // no duplicates
    w.tick();
    CHECK(w.sys->hardpoint_repairers(1, 0) == 1 && w.sys->hardpoint_health(1, 0) == 92.0f && credits[0] == 2.0f);
    w.tick();
    w.tick();
    CHECK(w.sys->hardpoint_health(1, 0) == 96.0f && credits[0] == 0.0f);
    w.tick();  // cannot pay: dropped, no repair
    CHECK(w.sys->hardpoint_repairers(1, 0) == 0 && w.sys->hardpoint_health(1, 0) == 96.0f);
    credits[2] = 10.0f;
    CHECK(w.ab->repair_hardpoint(1, 0, 2));
    w.tick();
    w.tick();
    CHECK(w.sys->hardpoint_health(1, 0) == 100.0f && w.sys->hardpoint_repairers(1, 0) == 0 && credits[2] == 8.0f);
    // Set_Targeting_Priorities (TACTICAL_COMBAT 2.4 Lua override, data pack +0x160): lower score wins.
    auto pick = [](bool override_set) {
        OiWorld v;
        ProjectileDesc p;
        p.name = "L";
        p.max_speed = 40.0f;
        p.damage = 0.0f;
        v.cd.projectiles = {p};
        PrioritySet prefer_capital;
        prefer_capital.name = "Prefer_Capital";
        PriorityEntry e;
        e.kind = PriorityEntry::Kind::Category;
        e.mask = kCatCapital;
        e.weight = 0.5f;
        prefer_capital.attack = {e};
        v.cd.priority_sets = {prefer_capital};
        UnitDesc a = oi_unit("A", 100.0f);
        a.max_attack_distance = 2000.0f;
        a.weapon_projectile = 0;
        a.weapon_recharge = 1.0f;
        const uint32_t ad = push(v.cd, v.ad, a, AbilityObjectDesc{});
        const uint32_t fd = push(v.cd, v.ad, oi_unit("Fighter", 100.0f), AbilityObjectDesc{});
        const uint32_t cd = push(v.cd, v.ad, oi_unit("Capital", 100.0f, 5.0f, kCatCapital), AbilityObjectDesc{});
        v.make();
        v.add(1, ad, 0, {0, 0, 0});
        v.add(2, fd, 1, {300, 0, 0});
        v.add(3, cd, 1, {900, 0, 0});
        if (override_set) CHECK(v.ab->set_targeting_priorities(1, "prefer_capital"));
        CHECK(!v.ab->set_targeting_priorities(1, "Nope"));
        v.tick();
        return v.sys->auto_target(1);
    };
    CHECK(pick(false) == 2 && pick(true) == 3);  // no set: score 1, nearest; the override prefers the capital (0.5)
}

void test_oi_detonations() {
    // §1.10 (land): one point detonation with stun, heal, convert, invulnerability, combat mod and weaken.
    OiWorld w;
    ProjectileDesc d;
    d.name = "Proj_Special_Blast";
    d.max_speed = 10.0f;
    d.stun = true;
    d.stun_frames = 90;
    d.stun_radius = 100.0f;
    d.heal = true;
    d.heal_radius = 100.0f;
    d.heal_amount = 25;
    d.invulnerability = true;
    d.invulnerability_frames = 60;
    d.invulnerability_radius = 100.0f;
    d.invulnerability_max_targets = 1;
    d.combat_mod = 3;  // HEALTH -> list 2
    d.combat_mod_value = -0.5f;
    d.combat_mod_seconds = 10.0f;
    d.blast_area_range = 100.0f;
    d.weaken = true;
    d.weaken_radius = 100.0f;
    d.weaken_seconds = 5.0f;
    d.weaken_take_increase = 0.2f;
    d.weaken_cause_reduction = 0.3f;
    w.cd.projectiles = {d};
    ProjectileDesc cv = d;
    cv.name = "Proj_Convert";
    cv.stun = cv.heal = cv.invulnerability = cv.weaken = false;
    cv.combat_mod = 0;
    cv.convert = true;
    cv.convert_radius = 100.0f;
    w.cd.projectiles.push_back(cv);
    UnitDesc trooper = oi_unit("Trooper", 100.0f);
    trooper.stunnable = true;
    trooper.converted_die_time = 60.0f;
    const uint32_t td = push(w.cd, w.ad, trooper, AbilityObjectDesc{});
    w.make(GameMode::Land);
    w.add(1, td, 0, {0, 0, 0});    // the owner's unit (heal, invulnerability)
    w.add(2, td, 0, {20, 0, 0});   // second ally: Max_Targets 1 -> no invulnerability
    w.add(3, td, 1, {30, 0, 0});   // enemy: stun, combat mod, weaken
    w.add(4, td, 1, {500, 0, 0});  // out of range
    w.sys->set_health(1, 50.0f);
    ProjectileLaunch l;
    l.owner = 0;
    l.projectile = 0;
    l.from = Vec3{0, 0, 0};
    l.point = l.from;
    l.fuse_seconds = 0.0f;
    CHECK(w.sys->launch_projectile(l, w.frame));
    w.tick();  // spawned this frame
    w.tick();  // detonates (fuse 0)
    CHECK(w.sys->stunned(3, w.frame) && !w.sys->stunned(1, w.frame) && !w.sys->stunned(4, w.frame));
    CHECK_EQ(w.sys->health(1), 75.0f);
    CHECK(w.sys->timed_invulnerable(1, w.frame) && !w.sys->timed_invulnerable(2, w.frame));
    CHECK_EQ(w.sys->modifier(3, ModList::MaxHealth), -0.5f);
    CHECK_EQ(w.sys->modifier(3, ModList::Defense), -0.2f);
    CHECK_EQ(w.sys->modifier(3, ModList::Damage), -0.3f);
    CHECK_EQ(w.sys->modifier(4, ModList::Defense), 0.0f);
    DamageRequest r;
    r.victim = 1;
    r.amount = 10.0f;
    w.sys->take_damage(r, w.frame);
    CHECK_EQ(w.sys->health(1), 75.0f);  // invulnerable
    l.projectile = 1;
    l.from = Vec3{500, 0, 0};
    l.point = l.from;
    CHECK(w.sys->launch_projectile(l, w.frame));
    w.tick();
    w.tick();
    CHECK(w.sys->owner(4) == 0 && (w.sys->flags(4) & kUnitConverted) && w.ab->set_owner(4, 0));
    CHECK(w.count(AbilityEventKind::OwnerChanged, 4) == 1);
}

void test_oi_shields_jamming_ion() {
    // §1.7: emitters register at creation (MISSILE_SHIELD in the first two entries; + a passive radius); activation does
    // not register. §1.11: a non-team ION_CANNON_SHOT owner never recharges on the removal path.
    OiWorld w;
    AbilityObjectDesc early, late;
    UnitAbilityRecord ms = rec(kMissileShield, 0.0f, 0.0f);
    ms.effective_radius = 300.0f;
    early.unit_abilities = {ms};
    late.unit_abilities = {rec(kTurbo, 0.0f, 0.0f), rec(kSprint, 1.0f, 1.0f), ms};
    UnitDesc pas = oi_unit("Passive", 100.0f);
    pas.passive_missile_shield_radius = 200.0f;
    const uint32_t ed = push(w.cd, w.ad, oi_unit("Early", 100.0f), early);
    const uint32_t ldd = push(w.cd, w.ad, oi_unit("Late", 100.0f), late);
    const uint32_t pd = push(w.cd, w.ad, pas, early);
    w.make();
    w.add(1, ed, 0, {0, 0, 0});
    w.add(2, ldd, 0, {0, 500, 0});
    w.add(3, pd, 0, {0, -500, 0});
    CHECK(w.sys->emitter_entries(1) == 1 && w.sys->emitter_entries(2) == 0 && w.sys->emitter_entries(3) == 2);
    const ObjectId two[] = {2};
    CHECK(w.ab->set_unit_ability_mode(two, kMissileShield, true, w.frame) == 1);
    CHECK(w.sys->emitter_entries(2) == 0);  // activation does not register
    w.sys->kill_unit(3, kNoObject, kKindNormal, w.frame);
    CHECK(w.sys->emitter_entries(3) == 1);  // Die removes ONE entry
    // single-unit ion cannon shot: no recharge on removal
    OiWorld v;
    ProjectileDesc p;
    p.name = "Laser";
    p.max_speed = 50.0f;
    p.damage = 1.0f;
    ProjectileDesc ion = p;
    ion.name = "Ion";
    v.cd.projectiles = {p, ion};
    const uint32_t is = add_special(v.ad, oi_special("Ion", SpecialClass::IonCannonShot, ActivationStyle::UserInput));
    UnitDesc yw = oi_unit("Y", 100.0f);
    yw.max_attack_distance = 600.0f;
    yw.weapon_projectile = 0;
    yw.weapon_recharge = 0.5f;
    AbilityObjectDesc ya;
    UnitAbilityRecord ir = oi_gui(kIonCannonShot, 0.0f, 20.0f, "Ion");
    ir.projectile_override = {1};
    ya.unit_abilities = {ir};
    ya.specials = {is};
    const uint32_t yd = push(v.cd, v.ad, yw, ya);
    const uint32_t td = push(v.cd, v.ad, oi_unit("T", 1e6f), AbilityObjectDesc{});
    v.make();
    v.add(1, yd, 0, {0, 0, 0});
    v.add(2, td, 1, {300, 0, 0});
    const ObjectId one[] = {1};
    CHECK(v.ab->targeted_ability(one, kIonCannonShot, 2, -1, v.frame) == 1);
    for (int i = 0; i < 60; ++i) v.tick();
    CHECK(!v.ab->special_applied(1, name_crc("Ion")) && v.ab->countdown_counter(1, kIonCannonShot) == 0);
    // §1.5 jamming: a homing missile at a jammed (friendly-to-the-jammer) target flies straight
    OiWorld j;
    ProjectileDesc mis;
    mis.name = "Missile";
    mis.max_speed = 5.0f;
    mis.category = ProjectileCategory::Missile;
    mis.homing = true;
    mis.max_flight_distance = 2000.0f;
    j.cd.projectiles = {mis};
    const uint32_t js = add_special(j.ad, oi_special("Jam", SpecialClass::SensorJamming, ActivationStyle::CombatAutomatic));
    AbilityObjectDesc ja;
    UnitAbilityRecord jr = rec(kSensorJamming, 0.0f, 0.0f);
    jr.effective_radius = 300.0f;
    ja.unit_abilities = {jr};
    ja.specials = {js};
    const uint32_t jd = push(j.cd, j.ad, oi_unit("Jammer", 100.0f), ja);
    const uint32_t ud = push(j.cd, j.ad, oi_unit("U", 1e6f), AbilityObjectDesc{});
    j.make();
    j.add(1, jd, 0, {0, 0, 0});
    j.add(2, ud, 0, {100, 0, 0});     // protected (friendly to the jammer)
    j.add(3, ud, 1, {-200, 0, 0});    // an enemy inside the radius: not jammed
    for (int i = 0; i < 3; ++i) j.tick();
    CHECK(j.sys->is_jammed(2, j.frame - 1) && !j.sys->is_jammed(3, j.frame - 1));
    ProjectileLaunch l;
    l.owner = 1;
    l.shooter = 3;
    l.projectile = 0;
    l.from = Vec3{100, -2000, 0};  // outside the jammer's (missile-shield emitter) radius
    l.target = 2;
    CHECK(j.sys->launch_projectile(l, j.frame));
    const Vec3 d0 = j.sys->projectile(j.sys->projectile_count() - 1).dir;
    j.move(2, {250, 0, 0});
    j.tick();
    j.tick();
    const Vec3 d1 = j.sys->projectile(j.sys->projectile_count() - 1).dir;
    CHECK(d1.x == d0.x && d1.y == d0.y);  // no re-aim at the moved target
}

void test_oi_class_bodies() {
    // Force_Healing (§2.3.1): due := now + int(FPS x interval + 0.5); all damaged allies in range get amount added.
    {
        OiWorld w;
        SpecialAbilityDesc h = oi_special("Heal", SpecialClass::ForceHealing, ActivationStyle::GroundAutomatic);
        h.heal_range = 100.0f;
        h.heal_amount = 10.0f;
        h.heal_interval = 1.0f;
        h.service_interval = 1;
        const uint32_t hs = add_special(w.ad, h);
        AbilityObjectDesc a;
        a.specials = {hs};
        const uint32_t hd = push(w.cd, w.ad, oi_unit("Healer", 100.0f), a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("Trooper", 100.0f), AbilityObjectDesc{});
        w.make(GameMode::Land);
        w.add(1, hd, 0, {0, 0, 0});
        w.add(2, td, 0, {50, 0, 0});
        w.add(3, td, 0, {500, 0, 0});
        w.add(4, td, 1, {60, 0, 0});
        for (ObjectId id : {2, 3, 4}) w.sys->set_health(id, 50.0f);
        w.tick();
        w.tick();  // the SAP update stagger (now or now + 1)
        CHECK(w.sys->health(2) == 60.0f && w.sys->health(3) == 50.0f && w.sys->health(4) == 50.0f);
        for (int i = 0; i < 30; ++i) w.tick();  // the next pass int(30 x 1.0 + 0.5) = 30 frames after the first
        CHECK_EQ(w.sys->health(2), 70.0f);
    }
    // Force_Lightning (§2.3.6): the primary, then the target player's objects within the radius, up to Max_Targets; each
    // takes its current health at once.
    {
        OiWorld w;
        SpecialAbilityDesc l = oi_special("Lightning", SpecialClass::ForceLightning, ActivationStyle::UserInput);
        l.max_range = 200.0f;
        l.max_targets = 2;
        l.targets_radius = 100.0f;
        const uint32_t ls = add_special(w.ad, l);
        AbilityObjectDesc a;
        a.unit_abilities = {oi_gui(kForceLightning, 0.0f, 30.0f, "Lightning")};
        a.specials = {ls};
        const uint32_t ud = push(w.cd, w.ad, oi_unit("Sith", 100.0f), a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("Trooper", 100.0f), AbilityObjectDesc{});
        w.make(GameMode::Land);
        w.add(1, ud, 0, {0, 0, 0});
        w.add(2, td, 1, {150, 0, 0});
        w.add(3, td, 1, {150, 50, 0});
        w.add(4, td, 1, {150, -50, 0});
        const ObjectId one[] = {1};
        CHECK(w.ab->targeted_ability(one, kForceLightning, 3, -1, w.frame) == 1);
        w.tick();
        CHECK(!w.sys->alive(3) && !w.sys->alive(2) && w.sys->alive(4));  // primary 3, then 2 (ascending id), cap 2
        CHECK(w.ab->countdown_counter(1, kForceLightning) == 900);
    }
    // DEPLOY / SPREAD_OUT / instant effect / SELF_DESTRUCT (§2.1, §2.2)
    {
        OiWorld w;
        ProjectileDesc blast;
        blast.name = "Proj_Blast";
        blast.max_speed = 1.0f;
        blast.blast_area_damage = 40.0f;
        blast.blast_area_range = 100.0f;
        w.cd.projectiles = {blast};
        AbilityObjectDesc a;
        UnitAbilityRecord dep = rec(kDeploy, 0.0f, 0.0f), sd = rec(kSelfDestruct, 3.0f, 0.0f), aes = rec(kAreaEffectStun, 0.0f, 10.0f);
        sd.spawned_projectile = 0;
        aes.spawned_projectile = 0;
        a.unit_abilities = {dep, sd, aes, rec(kSpreadOut, 0.0f, 0.0f)};
        const uint32_t ud = push(w.cd, w.ad, oi_unit("Walker", 100.0f), a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("Trooper", 100.0f), AbilityObjectDesc{});
        AbilityObjectDesc sq;
        sq.name = "Squad";
        sq.unit_abilities = {rec(kSpreadOut, 0.0f, 0.0f)};
        const uint32_t sqd = push_container(w.cd, w.ad, sq);
        AbilityObjectDesc ma;
        ma.unit_abilities = {rec(kSpreadOut, 0.0f, 0.0f)};
        const uint32_t md = push(w.cd, w.ad, oi_unit("Member", 50.0f), ma);
        w.make(GameMode::Land);
        w.add(1, ud, 0, {0, 0, 0});
        w.add(2, td, 1, {50, 0, 0});
        w.add(3, md, 0, {-300, 0, 0}, {1, 0, 0}, 5);
        w.add(4, md, 0, {-300, 20, 0}, {1, 0, 0}, 5);
        const ObjectId mem[] = {3, 4};
        CHECK(w.ab->add_container(5, sqd, 0, mem, w.frame, &w.rng));
        const ObjectId one[] = {1}, squad[] = {5};
        CHECK(w.ab->set_unit_ability_mode(one, kDeploy, true, w.frame) == 1 && w.ab->deployed(1) && w.ab->is_active(1, kDeploy));
        w.collect();
        CHECK(w.count(AbilityEventKind::Deployed, 1) == 1 && w.count(AbilityEventKind::StopOrder, 1) == 1);
        CHECK(w.ab->set_unit_ability_mode(one, kDeploy, false, w.frame) == 1 && !w.ab->deployed(1));
        CHECK(w.ab->set_unit_ability_mode(squad, kSpreadOut, true, w.frame) >= 1 && w.ab->spread_out(3) && w.ab->spread_out(4));
        const size_t p0 = w.sys->projectile_count();
        CHECK(w.ab->set_unit_ability_mode(one, kAreaEffectStun, true, w.frame, 2) == 1);  // M = 0: no slot byte
        CHECK(w.sys->projectile_count() == p0 + 1 && w.ab->countdown_counter(1, kAreaEffectStun) == 300);
        w.tick();
        w.tick();
        CHECK(w.sys->health(2) < 100.0f);  // the blast detonated at the target
        CHECK(w.ab->set_unit_ability_mode(one, kSelfDestruct, true, w.frame) == 1);
        for (int i = 0; i < 89 && w.sys->alive(1); ++i) w.tick();
        CHECK(w.sys->alive(1));
        w.tick();
        w.tick();
        CHECK(!w.sys->alive(1));  // countdown done: the blast spawned, Die(kind 1, self)
    }
}

// ================================================================================================
// REMAINING_GAPS §5.1-5.6
// ================================================================================================

// A WEAPON_SPECIAL hard point (Blast / Leech need a live one: Can_Activate 70 / 58).
uint32_t special_hardpoint(CombatData& cd, int32_t blast_projectile) {
    HardpointDesc h;
    h.name = "HP_SPECIAL";
    h.kind = HardpointKind::WeaponSpecial;
    h.health = 100.0f;
    h.destroyable = true;
    h.targetable = true;
    h.projectile = 0;
    h.blast_projectile = blast_projectile;
    cd.hardpoints.push_back(h);
    return static_cast<uint32_t>(cd.hardpoints.size() - 1);
}

void test_gaps_eject_thief() {
    // §5.1: 46 on -> recharge (10 s) only; 46 off with an occupant -> Take_Damage(0xD, 1e6, source = thief) kills the
    // vehicle, the thief's 18 recharges; no occupant -> nothing. 18 off restarts 18's recharge when it was active.
    OiWorld w;
    AbilityObjectDesc va, ta;
    va.unit_abilities = {rec(kEjectVehicleThief, 0.0f, 10.0f)};
    ta.unit_abilities = {rec(kCaptureVehicle, 0.0f, 20.0f)};
    const uint32_t vd = push(w.cd, w.ad, oi_unit("AT_ST_Captured", 500.0f), va);
    const uint32_t td = push(w.cd, w.ad, oi_unit("Thief", 100.0f), ta);
    w.make(GameMode::Land);
    AbilityHost h;
    h.first_occupant = [](ObjectId v) { return v == 1 ? ObjectId{2} : kNoObject; };
    w.ab->set_host(std::move(h));
    w.add(1, vd, 0, {0, 0, 0});
    w.add(2, td, 0, {0, 0, 0});
    w.add(3, vd, 0, {200, 0, 0});  // no occupant
    const ObjectId v1[] = {1}, v3[] = {3}, th[] = {2};
    const SimRng before = w.rng;
    CHECK(w.ab->set_unit_ability_mode(v1, kEjectVehicleThief, true, w.frame) == 1 && w.ab->is_active(1, kEjectVehicleThief));
    CHECK(w.ab->countdown_counter(1, kEjectVehicleThief) == 300 && w.sys->alive(1));
    CHECK(w.ab->set_unit_ability_mode(v3, kEjectVehicleThief, true, w.frame) == 1);
    CHECK(w.ab->set_unit_ability_mode(v3, kEjectVehicleThief, false, w.frame) == 1 && w.sys->alive(3));  // empty list
    CHECK(w.ab->countdown_counter(2, kCaptureVehicle) == 0);
    CHECK(w.ab->set_unit_ability_mode(v1, kEjectVehicleThief, false, w.frame) == 1);
    CHECK(!w.sys->alive(1) && !w.ab->is_active(1, kEjectVehicleThief));      // the eject kills the stolen vehicle
    CHECK(w.ab->countdown_counter(2, kCaptureVehicle) == 600);               // the thief's 18 recharges
    CHECK(w.rng.state() == before.state());                                  // no draw (no death clone / projectiles here)
    // 18 off on the thief: 18 active -> its recharge restarts
    CHECK(w.ab->force_recharge(2, kCaptureVehicle, 0.0f));
    CHECK(w.ab->set_unit_ability_active(2, kCaptureVehicle, true, w.frame) && w.ab->is_active(2, kCaptureVehicle));
    CHECK(w.ab->force_recharge(2, kCaptureVehicle, 0.0f));
    CHECK(w.ab->set_unit_ability_mode(th, kCaptureVehicle, false, w.frame) == 1 && !w.ab->is_active(2, kCaptureVehicle));
    CHECK(w.ab->countdown_counter(2, kCaptureVehicle) == 600 && w.ab->countdown_phase(2, kCaptureVehicle) == 0);
}

void test_gaps_deploy_troopers() {
    // §5.2: member i of the spawned type's company at Owner_Attachment_Bone i; bones the model lacks are skipped (their
    // member index too); N = members - deployed (+0x40); no draws; Max_Num_Spawned_Objects unread.
    OiWorld w;
    AbilityObjectDesc a;
    UnitAbilityRecord dt = rec(kDeployTroopers, 0.0f, 20.0f);
    dt.spawned_object_type = "Squad";
    dt.spawned_type = 7;
    dt.max_spawned_objects = 9;
    dt.owner_attachment_bones = {"B_Trooper_00", "B_Trooper_01", "B_Trooper_02"};
    a.unit_abilities = {dt};
    const uint32_t ud = push(w.cd, w.ad, oi_unit("AT_AT", 1000.0f), a);
    w.make(GameMode::Land);
    AbilityHost h;
    h.company_members = [](int32_t type, std::vector<int32_t>& m) {
        if (type != 7) return false;
        m = {100, 101, 102, 103};
        return true;
    };
    h.bone_position = [](ObjectId, std::string_view bone, Vec3& p) {
        if (bone == "B_Trooper_01") return 0;  // the render model lacks it
        p = Vec3{bone == "B_Trooper_00" ? 10.0f : 30.0f, 5.0f, 2.0f};
        return 1;
    };
    w.ab->set_host(std::move(h));
    w.add(1, ud, 0, {0, 0, 0}, {0, 1, 0});
    const ObjectId one[] = {1};
    const SimRng before = w.rng;
    CHECK(w.ab->set_unit_ability_mode(one, kDeployTroopers, true, w.frame) == 1);
    const std::span<const SpawnRequest> q = w.ab->spawn_requests();
    CHECK(q.size() == 2);
    if (q.size() == 2) {
        CHECK(q[0].purpose == SpawnPurpose::Troopers && q[0].object_type == 100 && q[0].bone == 0 && q[0].position.x == 10.0f);
        CHECK(q[1].object_type == 102 && q[1].bone == 2 && q[1].position.x == 30.0f && q[1].facing.y == 1.0f && q[1].owner == 0);
        CHECK(w.ab->bind_spawn(q[0].serial, 50) && w.ab->bind_spawn(q[1].serial, 51));
    }
    CHECK(w.ab->troopers_deployed(1) == 2 && w.ab->troopers(1).size() == 2 && w.ab->troopers(1)[1] == 51);
    CHECK(w.ab->countdown_counter(1, kDeployTroopers) == 600 && !w.ab->is_active(1, kDeployTroopers));
    w.collect();
    // second deploy: N = 4 - 2 = 2 -> two more; third: N = 0 -> none
    CHECK(w.ab->force_recharge(1, kDeployTroopers, 0.0f));
    CHECK(w.ab->set_unit_ability_mode(one, kDeployTroopers, true, w.frame) == 1 && w.ab->spawn_requests().size() == 2);
    CHECK(w.ab->troopers_deployed(1) == 4);
    w.collect();
    CHECK(w.ab->force_recharge(1, kDeployTroopers, 0.0f));
    CHECK(w.ab->set_unit_ability_mode(one, kDeployTroopers, true, w.frame) == 1 && w.ab->spawn_requests().empty());
    // no model -> state 10 (nothing)
    OiWorld v;
    const uint32_t vd = push(v.cd, v.ad, oi_unit("AT_AT", 1000.0f), a);
    v.make(GameMode::Land);
    AbilityHost hv;
    hv.company_members = [](int32_t, std::vector<int32_t>& m) {
        m = {100, 101, 102};
        return true;
    };
    hv.bone_position = [](ObjectId, std::string_view, Vec3&) { return -1; };
    v.ab->set_host(std::move(hv));
    v.add(1, vd, 0, {0, 0, 0});
    CHECK(v.ab->set_unit_ability_mode(one, kDeployTroopers, true, v.frame) == 1 && v.ab->spawn_requests().empty());
    CHECK(v.ab->troopers_deployed(1) == 0);
    CHECK(w.rng.state() == before.state());
}

void test_gaps_jet_pack() {
    // §5.3: a squad's JET_PACK moves the whole team (MoveOrder type 17) when the point is within Effective_Radius (< 0:
    // unlimited); Expiration 0 -> recharge at once, never "active".
    OiWorld w;
    UnitAbilityRecord jp = rec(kJetPack, 0.0f, 28.0f);
    jp.effective_radius = -1.0f;
    AbilityObjectDesc ma;
    ma.unit_abilities = {jp};
    const uint32_t md = push(w.cd, w.ad, oi_unit("Rocketeer", 100.0f), ma);
    AbilityObjectDesc sq;
    sq.name = "Rocketeer_Squad";
    const uint32_t sqd = push_container(w.cd, w.ad, sq);
    UnitAbilityRecord jb = jp;
    jb.effective_radius = 100.0f;
    AbilityObjectDesc ba;
    ba.unit_abilities = {jb};
    const uint32_t bd = push(w.cd, w.ad, oi_unit("Boba", 100.0f), ba);
    const uint32_t xd = push(w.cd, w.ad, oi_unit("Marker", 10.0f), AbilityObjectDesc{});
    w.make(GameMode::Land);
    w.add(3, md, 0, {0, 0, 0}, {1, 0, 0}, 5);
    w.add(4, md, 0, {0, 20, 0}, {1, 0, 0}, 5);
    const ObjectId mem[] = {3, 4};
    CHECK(w.ab->add_container(5, sqd, 0, mem, w.frame, &w.rng));
    w.add(6, bd, 0, {0, -500, 0});
    w.add(9, xd, 1, {800, 0, 0});
    w.add(10, xd, 1, {50, -500, 0});
    const ObjectId squad[] = {5}, boba[] = {6};
    CHECK(w.ab->set_unit_ability_mode(squad, kJetPack, true, w.frame, 9) == 1);
    w.collect();
    const AbilityEvent* m3 = w.last(AbilityEventKind::MoveOrder, 3);
    const AbilityEvent* m4 = w.last(AbilityEventKind::MoveOrder, 4);
    CHECK(m3 && m4 && m3->type == kJetPack && m3->point.x == 800.0f && m4->point.x == 800.0f);
    CHECK(w.ab->countdown_counter(5, kJetPack) == 840 && !w.ab->is_active(5, kJetPack));
    CHECK(w.ab->set_unit_ability_mode(squad, kJetPack, true, w.frame, 9) == 0);  // recharging
    // Effective_Radius 100: 800 away refused (no move, no countdown); 50 away accepted
    CHECK(w.ab->set_unit_ability_mode(boba, kJetPack, true, w.frame, 9) == 1);
    w.collect();
    CHECK(!w.last(AbilityEventKind::MoveOrder, 6) && w.ab->countdown_counter(6, kJetPack) == 0);
    CHECK(w.ab->set_unit_ability_mode(boba, kJetPack, true, w.frame, 10) == 1);
    w.collect();
    CHECK(w.last(AbilityEventKind::MoveOrder, 6) && w.ab->countdown_counter(6, kJetPack) == 840);
}

void test_gaps_stun_release() {
    // §5.4: the stun sets obj +0x3a0 bit 0x10 (weapons-disabled gates: LUCKY_SHOT etc. refuse) when clear; the next
    // service := now + frames (a second stun can SHORTEN it); release at the first service at or after it clears the bit.
    OiWorld w;
    AbilityObjectDesc a;
    a.unit_abilities = {rec(kLuckyShot, 0.0f, 0.0f)};
    UnitDesc u = oi_unit("Trooper", 100.0f);
    u.stunnable = true;
    const uint32_t ud = push(w.cd, w.ad, u, a);
    w.make(GameMode::Land);
    w.add(1, ud, 0, {0, 0, 0});
    CHECK(w.ab->can_activate(1, kLuckyShot, false, true, w.frame));
    CHECK(w.sys->stun_unit(1, 60, w.frame) && !w.ab->can_activate(1, kLuckyShot, false, true, w.frame));
    CHECK(w.sys->stun_unit(1, 5, w.frame));  // plain assignment: now + 5
    for (int i = 0; i < 4; ++i) w.tick();
    CHECK(w.sys->stunned(1, w.frame) && !w.ab->can_activate(1, kLuckyShot, false, true, w.frame));
    w.tick();
    w.tick();
    CHECK(!w.sys->stunned(1, w.frame) && w.ab->can_activate(1, kLuckyShot, false, true, w.frame));
    CHECK(w.count(AbilityEventKind::Stunned, 1) == 3);  // two stuns, one release
}

void test_gaps_blast() {
    // §5.5: charging (counter > 0) refuses every hard point and weapon; charged (counter = -K) lets only WEAPON_SPECIAL hard
    // points fire with Blast_Ability_Fire_Projectile_Type and x Damage_Multiplier; each shot counts up; at 0 normal fire.
    OiWorld w;
    ProjectileDesc p;
    p.name = "Plasma";
    p.max_speed = 50.0f;
    p.damage = 10.0f;
    ProjectileDesc pb = p;
    pb.name = "Plasma_Blast";
    w.cd.projectiles = {p, pb};
    const uint32_t hs = special_hardpoint(w.cd, 1);
    HardpointDesc laser;
    laser.kind = HardpointKind::WeaponLaser;
    laser.projectile = 0;
    SpecialAbilityDesc bl = oi_special("Peacebringer_Blast", SpecialClass::Blast, ActivationStyle::UserInput);
    bl.charging_time = 1.0f;  // 30 frames
    bl.damage_multiplier = 5.0f;
    const uint32_t bs = add_special(w.ad, bl);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kBlast, 0.0f, 60.0f, "Peacebringer_Blast")};
    a.specials = {bs};
    UnitDesc ship = oi_unit("Peacebringer", 1000.0f, 50.0f, kCatFrigate);
    ship.hardpoints = {hs};
    const uint32_t sd = push(w.cd, w.ad, ship, a);
    w.make();
    w.add(1, sd, 0, {0, 0, 0});
    const HardpointDesc& special = w.cd.hardpoints[hs];
    const ObjectId one[] = {1};
    int32_t proj = 0;
    float scale = 1.0f;
    CHECK(w.ab->fire_gate(1, &laser, proj, scale) && scale == 1.0f);  // no Blast: untouched
    CHECK(w.ab->set_unit_ability_mode(one, kBlast, true, w.frame) == 1);
    CHECK(w.ab->blast_counter(1) == 30 && w.ab->countdown_counter(1, kBlast) == 1800);
    CHECK(!w.ab->fire_gate(1, &special, proj, scale) && !w.ab->fire_gate(1, &laser, proj, scale) &&
          !w.ab->fire_gate(1, nullptr, proj, scale));
    for (int i = 0; i < 40 && w.ab->blast_counter(1) > 0; ++i) w.tick();
    CHECK(w.ab->blast_counter(1) == -1);  // K = 1 special hard point
    CHECK(!w.ab->fire_gate(1, &laser, proj, scale) && !w.ab->fire_gate(1, nullptr, proj, scale));
    proj = 0;
    CHECK(w.ab->fire_gate(1, &special, proj, scale) && proj == 1 && scale == 5.0f);
    w.ab->fire_done(1, &laser, w.frame);  // a non-special shot does not count
    CHECK(w.ab->blast_counter(1) == -1);
    w.ab->fire_done(1, &special, w.frame);
    CHECK(w.ab->blast_counter(1) == 0 && !w.ab->special_applied(1, name_crc("Peacebringer_Blast")));
    proj = 0;
    scale = 1.0f;
    CHECK(w.ab->fire_gate(1, &laser, proj, scale) && w.ab->fire_gate(1, &special, proj, scale) && proj == 0 && scale == 1.0f);
}

void test_gaps_leech() {
    // §5.6: apply -> 58 on, a permanent list-1 ("damage dealt") entry m on the owner (x (1 + m) on everything it deals);
    // update drains Shield_Damage_Per_Second / FPS; ends at shield 0; remove deletes the entry.
    OiWorld w;
    const uint32_t hs = special_hardpoint(w.cd, -1);
    SpecialAbilityDesc le = oi_special("Kadalbe_Leech_Shields", SpecialClass::LeechShields, ActivationStyle::UserInput);
    le.min_range = 10.0f;
    le.max_range = 2000.0f;
    le.duration_secs = 25.0f;
    le.damage_per_second = 300.0f;  // 10 per frame
    le.damage_multiplier = 3.0f;
    const uint32_t ls = add_special(w.ad, le);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kLeechShields, 0.0f, 60.0f, "Kadalbe_Leech_Shields")};
    a.specials = {ls};
    UnitDesc ship = oi_unit("Kadalbe", 1000.0f, 50.0f, kCatCapital);
    ship.hardpoints = {hs};
    const uint32_t kd = push(w.cd, w.ad, ship, a);
    UnitDesc tgt = oi_unit("Target", 1000.0f, 20.0f, kCatFrigate);
    tgt.shield_points = 100.0f;
    const uint32_t td = push(w.cd, w.ad, tgt, AbilityObjectDesc{});
    const uint32_t vd = push(w.cd, w.ad, oi_unit("Victim", 1e6f, 20.0f, kCatFrigate), AbilityObjectDesc{});
    w.make();
    w.add(1, kd, 0, {0, 0, 0});
    w.add(2, td, 1, {500, 0, 0});
    w.add(3, vd, 1, {-500, 0, 0});
    DamageRequest r;
    r.victim = 3;
    r.source = 1;
    r.amount = 10.0f;
    const float h0 = w.sys->health(3);
    w.sys->take_damage(r, w.frame);
    const float d0 = h0 - w.sys->health(3);
    CHECK(d0 > 0.0f);
    const float shield0 = w.sys->shield(2);
    CHECK(shield0 > 0.0f);
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kLeechShields, 2, -1, w.frame) == 1);
    w.tick();
    const uint32_t crc = name_crc("Kadalbe_Leech_Shields");
    CHECK(w.ab->special_applied(1, crc) && w.ab->countdown_counter(1, kLeechShields) == 1800);
    CHECK_EQ(w.sys->modifier(1, ModList::Damage), 3.0f);
    const float h1 = w.sys->health(3);
    w.sys->take_damage(r, w.frame);
    CHECK_EQ(h1 - w.sys->health(3), 4.0f * d0);  // x (1 + 3)
    for (int i = 0; i < 200 && w.ab->special_applied(1, crc); ++i) w.tick();
    CHECK(!w.ab->special_applied(1, crc) && w.sys->shield(2) == 0.0f);
    CHECK_EQ(w.sys->modifier(1, ModList::Damage), 0.0f);
}

void test_oi_weapon_swap() {
    // ABILITIES_HARDPOINTS 2.5: SWAP_WEAPONS flips obj +0x324 (on and off) when the type has more than one weapon set
    // (Projectile_Types); TACTICAL_GAPS 2.3 Permanent_Weapon_Swap (Ground_Automatic) sets it to Weapon_Index at unit add.
    // The unit weapon then fires Projectile_Types[index].
    OiWorld w;
    ProjectileDesc pa, pb;
    pa.damage = 10.0f;
    pb.damage = 30.0f;
    w.cd.projectiles.push_back(pa);
    w.cd.projectiles.push_back(pb);
    AbilityObjectDesc sa;
    sa.unit_abilities = {rec(kSwapWeapons, 0.0f, 0.0f)};
    UnitDesc mal = oi_unit("MAL", 500.0f);
    mal.weapon_projectile = 0;
    mal.weapon_projectiles = {0, 1};
    const uint32_t md = push(w.cd, w.ad, mal, sa);
    UnitDesc one = oi_unit("Single", 500.0f);
    one.weapon_projectile = 0;
    one.weapon_projectiles = {0};
    const uint32_t od = push(w.cd, w.ad, one, sa);
    SpecialAbilityDesc ps = oi_special("Disruptor_Permanent_Swap", SpecialClass::WeaponSwap, ActivationStyle::GroundAutomatic);
    ps.weapon_index = 1;
    const uint32_t psi = add_special(w.ad, ps);
    AbilityObjectDesc pa_desc;
    pa_desc.specials = {psi};
    UnitDesc merc = oi_unit("Disruptor_Merc", 100.0f);
    merc.weapon_projectile = 0;
    merc.weapon_projectiles = {0, 1};
    const uint32_t dd = push(w.cd, w.ad, merc, pa_desc);
    w.make(GameMode::Land);
    w.add(1, md, 0, {0, 0, 0});
    w.add(2, od, 0, {50, 0, 0});
    w.add(3, dd, 0, {100, 0, 0});
    CHECK(w.sys->weapon_index(1) == 0 && w.sys->current_weapon_projectile(1) == 0 && w.sys->weapon_set_count(1) == 2);
    CHECK(w.sys->weapon_index(3) == 1 && w.sys->current_weapon_projectile(3) == 1);  // permanent swap at add
    CHECK(w.ab->special_applied(3, name_crc("Disruptor_Permanent_Swap")));
    const ObjectId u1[] = {1}, u2[] = {2};
    CHECK(w.ab->set_unit_ability_mode(u1, kSwapWeapons, true, w.frame) == 1);
    CHECK(w.sys->weapon_index(1) == 1 && w.sys->current_weapon_projectile(1) == 1);
    CHECK(w.ab->set_unit_ability_mode(u1, kSwapWeapons, false, w.frame) == 1 && w.sys->weapon_index(1) == 0);
    w.ab->set_unit_ability_mode(u2, kSwapWeapons, true, w.frame);
    CHECK(w.sys->weapon_index(2) == 0);  // one weapon set: no flip
    // a Space mode world: Ground_Automatic is not applied
    OiWorld s;
    s.cd = w.cd;
    s.ad = w.ad;
    s.make(GameMode::Space);
    s.add(3, dd, 0, {0, 0, 0});
    CHECK(s.sys->weapon_index(3) == 0);
}

void test_oi_berserker_speed() {
    // TACTICAL_GAPS 2.3.2: the apply sets the locomotor speed override (+0x2a0 := Berserker_Speed, flag +0x29c); it is
    // read while the special is applied and cleared with it [inferred].
    OiWorld w;
    SpecialAbilityDesc b = oi_special("Berserk", SpecialClass::Berserker, ActivationStyle::UserInput);
    b.max_range = 500.0f;
    b.berserker_speed = 7.5f;
    b.berserker_damage = 1.0f;
    b.berserker_damage_range = 1000.0f;
    b.berserker_duration = 1.0f;  // 30 frames
    const uint32_t bs = add_special(w.ad, b);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kBerserker, 0.0f, 60.0f, "Berserk")};
    a.specials = {bs};
    const uint32_t bd = push(w.cd, w.ad, oi_unit("Berserker", 1000.0f), a);
    const uint32_t td = push(w.cd, w.ad, oi_unit("Enemy", 1e6f), AbilityObjectDesc{});
    w.make(GameMode::Land);
    w.add(1, bd, 0, {0, 0, 0});
    w.add(2, td, 1, {100, 0, 0});
    CHECK(w.ab->speed_override(1) == -1.0f);
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kBerserker, 2, -1, w.frame) == 1);
    w.tick();
    CHECK(w.ab->special_applied(1, name_crc("Berserk")) && w.ab->speed_override(1) == 7.5f);
    CHECK(w.ab->speed_override(2) == -1.0f && w.ab->speed_override(99) == -1.0f);
    for (int i = 0; i < 60 && w.ab->special_applied(1, name_crc("Berserk")); ++i) w.tick();
    CHECK(!w.ab->special_applied(1, name_crc("Berserk")) && w.ab->speed_override(1) == -1.0f);
}

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

void test_battle(const char* golden, const char* write_golden) {
    const uint32_t kTicks = 900;
    Battle a;
    const CrcLog serial = a.run(kTicks, serial_for);
    const CombatStats& st = a.w.sys->stats();
    std::printf("battle: %u ticks, shots %llu, hits %llu, deaths %llu, hardpoint kills %llu, ability events %llu, "
                "activations %llu, units left %zu\n",
                kTicks, static_cast<unsigned long long>(st.shots), static_cast<unsigned long long>(st.hits),
                static_cast<unsigned long long>(st.deaths), static_cast<unsigned long long>(a.hp_kills),
                static_cast<unsigned long long>(a.ability_events), static_cast<unsigned long long>(a.activations),
                a.w.sys->unit_count());
    CHECK(st.shots > 100 && st.hits > 50);
    CHECK(a.hp_kills > 0);  // hardpoints were targeted and destroyed
    CHECK(a.activations > 5);
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
    b.run(60, serial_for, false);
    uint64_t in_tick = 0;
    for (uint32_t i = 0; i < 540; ++i) {
        const uint32_t f = b.w.frame;
        b.script(f);
        g_allocs = 0;
        g_count = true;
        b.w.sys->tick(f, b.w.trees, b.w.rng, serial_for);
        g_count = false;
        if (g_allocs.load()) std::printf("  tick %u: %llu allocation(s)\n", f, static_cast<unsigned long long>(g_allocs.load()));
        in_tick += g_allocs.load();
        for (const DeathEvent& d : b.w.sys->deaths()) b.w.trees.remove(static_cast<EntityHandle>(d.id));
        b.w.ab->clear_events();
        ++b.w.frame;
    }
    std::printf("allocations in 540 steady-state ticks (combat + abilities): %llu\n", static_cast<unsigned long long>(in_tick));
    CHECK(in_tick == 0);
}

// ================================================================================================
// N1 stage 2: save / load in the middle of a battle (AbilitySystem + CombatSystem write / read)
// ================================================================================================

struct SavedBattle {
    std::vector<uint8_t> combat, abilities, rng;
};

SavedBattle save_battle(const CombatSystem& c, const AbilitySystem& a, const SimRng& rng) {
    SavedBattle s;
    ByteWriter wc, wa, wr;
    c.write(wc);
    a.write(wa);
    rng.write(wr);
    s.combat = wc.bytes();
    s.abilities = wa.bytes();
    s.rng = wr.bytes();
    return s;
}

bool load_battle(CombatSystem& c, AbilitySystem& a, SimRng& rng, const SavedBattle& s) {
    ByteReader rc(s.combat), ra(s.abilities), rr(s.rng);
    return c.read(rc) && a.read(ra) && rng.read(rr);
}

// Every hash of both systems (abilities, combat incl. the S9 extension) and the synced RNG.
uint64_t battle_digest(const CombatSystem& c, const AbilitySystem& a, const SimRng& rng) {
    StateHasher h;
    a.hash(h);
    c.hash_units(h);
    c.hash_projectiles(h);
    c.hash_modifiers(h);
    c.hash_delayed(h);
    c.hash_extended(h);
    rng.hash(h);
    return h.digest();
}

std::vector<uint8_t> ability_bytes(const AbilitySystem& a) {
    ByteWriter w;
    a.write(w);
    return w.bytes();
}

void test_save_load_battle() {
    const uint32_t kBefore = 270, kAfter = 630;
    Battle a;
    a.run(kBefore, serial_for, false);
    // Mid-battle: abilities active / recharging (tractor beams, ion shot, stealth, laser defense, concentrate fire ...).
    uint32_t counting = 0, active = 0;
    for (ObjectId id = 1; id <= 13; ++id)
        for (uint8_t t = 1; t <= kMaxAbilityType; ++t) {
            if (a.w.ab->countdown_counter(id, t) > 0) ++counting;
            if (a.w.ab->is_active(id, t)) ++active;
        }
    CHECK(counting > 0);
    CHECK(a.w.sys->projectile_count() > 0);
    const SavedBattle saved = save_battle(*a.w.sys, *a.w.ab, a.w.rng);

    Battle b;
    CHECK(load_battle(*b.w.sys, *b.w.ab, b.w.rng, saved));
    b.w.frame = a.w.frame;
    for (const Battle::Mover& m : b.movers)
        if (!b.w.sys->has_unit(m.id)) b.w.trees.remove(static_cast<EntityHandle>(m.id));
    const SavedBattle again = save_battle(*b.w.sys, *b.w.ab, b.w.rng);
    CHECK(again.combat == saved.combat && again.abilities == saved.abilities);  // write -> read -> write
    CHECK(battle_digest(*a.w.sys, *a.w.ab, a.w.rng) == battle_digest(*b.w.sys, *b.w.ab, b.w.rng));
    CHECK(a.registry.capture(a.w.frame).combined == b.registry.capture(b.w.frame).combined);

    const ParallelFor pf4 = alamo::tactical::testing::make_thread_parallel_for(4);
    uint32_t diverged = 0, events = 0, activations = 0;
    for (uint32_t i = 0; i < kAfter; ++i) {
        const uint32_t f = a.w.frame;
        a.script(f);
        a.w.tick();
        b.script(f);
        b.w.tick(pf4);
        events += static_cast<uint32_t>(a.w.ab->events().size());
        bool same = a.w.ab->events().size() == b.w.ab->events().size();
        for (size_t k = 0; same && k < a.w.ab->events().size(); ++k) {
            const AbilityEvent& x = a.w.ab->events()[k];
            const AbilityEvent& y = b.w.ab->events()[k];
            same = x.kind == y.kind && x.type == y.type && x.id == y.id && x.target == y.target && x.frame == y.frame;
            if (x.kind == AbilityEventKind::Activated) ++activations;
        }
        a.w.ab->clear_events();
        b.w.ab->clear_events();
        if (!same || battle_digest(*a.w.sys, *a.w.ab, a.w.rng) != battle_digest(*b.w.sys, *b.w.ab, b.w.rng)) {
            if (diverged == 0) std::printf("abilities save/load: diverged at frame %u\n", f);
            ++diverged;
        }
    }
    CHECK(diverged == 0);
    CHECK(events > 0 && activations > 0);  // the continuation used abilities (frames 400 / 450)
    const SavedBattle end_a = save_battle(*a.w.sys, *a.w.ab, a.w.rng), end_b = save_battle(*b.w.sys, *b.w.ab, b.w.rng);
    CHECK(end_a.combat == end_b.combat && end_a.abilities == end_b.abilities && end_a.rng == end_b.rng);
    std::printf("abilities save: %zu + %zu bytes at frame %u (%u counting slots, %u active), %u ticks continued identically "
                "(%u events)\n",
                saved.abilities.size(), saved.combat.size(), kBefore, counting, active, kAfter, events);
}

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

void test_save_load_mines() {
    MineScene a;
    const ObjectId one[] = {1};
    CHECK(a.w.ab->targeted_ability(one, kProximityMines, 2, -1, a.w.frame) == 1);
    a.w.ab->tick(a.w.frame, a.w.trees, a.w.rng, serial_for);  // spawn requests + mines of this tick, not consumed yet
    ++a.w.frame;
    CHECK(a.w.ab->spawn_requests().size() == 3 && a.w.ab->mine_count() == 3 && !a.w.ab->events().empty());
    const SavedBattle saved = save_battle(*a.w.sys, *a.w.ab, a.w.rng);

    MineScene b;
    CHECK(load_battle(*b.w.sys, *b.w.ab, b.w.rng, saved));
    b.w.frame = a.w.frame;
    CHECK(ability_bytes(*b.w.ab) == saved.abilities);
    CHECK(b.w.ab->spawn_requests().size() == 3 && b.w.ab->mine_count() == 3);
    for (size_t i = 0; i < 3 && i < b.w.ab->spawn_requests().size(); ++i) {
        const SpawnRequest& x = a.w.ab->spawn_requests()[i];
        const SpawnRequest& y = b.w.ab->spawn_requests()[i];
        CHECK(x.serial == y.serial && x.purpose == y.purpose && x.position.x == y.position.x && x.position.y == y.position.y);
    }
    // Both drive the tank onto the first mine and run until it triggered.
    const Vec3 mine = a.w.ab->spawn_requests()[0].position;
    for (MineScene* s : {&a, &b}) {
        s->w.collect();
        s->w.move(3, Vec3{mine.x + 1.0f, mine.y, 0.0f});
    }
    uint32_t diverged = 0;
    for (uint32_t i = 0; i < 40; ++i) {
        a.w.tick();
        b.w.tick();
        if (battle_digest(*a.w.sys, *a.w.ab, a.w.rng) != battle_digest(*b.w.sys, *b.w.ab, b.w.rng)) ++diverged;
    }
    CHECK(diverged == 0);
    CHECK(a.w.count(AbilityEventKind::MineTriggered) == 1 && b.w.count(AbilityEventKind::MineTriggered) == 1);
    CHECK(a.w.ab->mine_count() == 2 && b.w.ab->mine_count() == 2);
    CHECK(a.w.log.size() == b.w.log.size());

    // Rejected data leaves the system unchanged: truncation, trailing bytes, version, descriptor counts, a bad slot record.
    MineScene c;
    const std::vector<uint8_t> before = ability_bytes(*c.w.ab);
    const auto rejected = [&c](const std::vector<uint8_t>& bytes) {
        ByteReader r(bytes);
        return !c.w.ab->read(r);
    };
    uint32_t accepted = 0;
    for (size_t len = 0; len < saved.abilities.size(); len += (len < 300 ? 1 : 37))
        if (!rejected(std::vector<uint8_t>(saved.abilities.begin(), saved.abilities.begin() + static_cast<std::ptrdiff_t>(len))))
            ++accepted;
    CHECK(accepted == 0);
    std::vector<uint8_t> bad = saved.abilities;
    bad.push_back(1);
    CHECK(rejected(bad));
    bad = saved.abilities;
    bad[0] = 9;  // version
    CHECK(rejected(bad));
    bad = saved.abilities;
    bad[4] = static_cast<uint8_t>(bad[4] + 1);  // AbilityData object count
    CHECK(rejected(bad));
    // The first object's slot record 0: version, 3 counts, spawn serial, context byte, object count, id / desc / owner /
    // container, 3 flag bytes, member count (0).
    const size_t rec0 = 4 + 12 + 4 + 1 + 4 + 16 + 3 + 4;
    bad = saved.abilities;
    bad[rec0] = 0x7F;  // record index 127: past the type's Unit_Abilities_Data
    bad[rec0 + 1] = 0;
    CHECK(rejected(bad));
    CHECK(ability_bytes(*c.w.ab) == before);
    ByteReader ok(saved.abilities);
    CHECK(c.w.ab->read(ok));
    CHECK(ability_bytes(*c.w.ab) == saved.abilities);
}

// ================================================================================================
// FoC data (ALAMO_FOC_DATA)
// ================================================================================================

void walk(const data::XmlNode& n, const std::function<void(const data::XmlNode&)>& fn) {
    for (data::XmlNode c = n.first_child(); c; c = c.next_sibling()) {
        fn(c);
        walk(c, fn);
    }
}

void test_foc_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("foc data: skipped (ALAMO_FOC_DATA unset)\n");
        return;
    }
#ifdef ALAMO_ABILITIES_TESTS_HAVE_VFS
    // The shipped game keeps Data\XML inside Config.meg: every FoC XML through the VFS (FoC MEGs + loose files,
    // no EaW layer, so the counts below are FoC's own).
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    vcfg.eaw_root = std::string();
    std::string verr;
    const std::unique_ptr<vfs::Vfs> vfs_ptr = vfs::Vfs::create(vcfg, &verr);
    if (!vfs_ptr) {
        std::printf("FAIL foc data: VFS on %s: %s\n", root, verr.c_str());
        ++g_fail;
        return;
    }
    const std::vector<std::string> files = vfs_ptr->list("Data\\XML\\*.XML");
    auto read_file = [&vfs_ptr](const std::string& p, std::string& out) {
        std::vector<uint8_t> b;
        if (!vfs_ptr->read(p, b)) return false;
        out.assign(b.begin(), b.end());
        return true;
    };
    auto file_name = [](const std::string& p) { return p.substr(p.find_last_of('\\') + 1); };
#else
    namespace fs = std::filesystem;
    fs::path xml_dir;
    for (const char* d : {"Data", "DATA", "data"})
        for (const char* x : {"XML", "xml"})
            if (xml_dir.empty() && fs::is_directory(fs::path(root) / d / x)) xml_dir = fs::path(root) / d / x;
    if (xml_dir.empty()) {
        std::printf("FAIL foc data: no Data\\XML under %s\n", root);
        ++g_fail;
        return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(xml_dir))
        if (e.is_regular_file()) {
            std::string ext = e.path().extension().string();
            for (char& ch : ext) ch = static_cast<char>(ch >= 'a' && ch <= 'z' ? ch - 32 : ch);
            if (ext == ".XML") files.push_back(e.path());
        }
    std::sort(files.begin(), files.end());
    auto read_file = [](const fs::path& p, std::string& out) {
        std::ifstream in(p, std::ios::binary);  // fs::path: wide-character open (long \\?\ roots work)
        if (!in) return false;
        out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        return true;
    };
    auto file_name = [](const fs::path& p) { return p.filename().string(); };
#endif
    CHECK(!files.empty());
    std::array<uint32_t, kAbilitySlots> per_type{};
    uint32_t records = 0, bad = 0, gui = 0, specials = 0, unknown_style = 0, deploy_bad = 0;
    std::map<std::string, uint32_t> classes;
    std::vector<std::string> diag;
    AbilityResolvers r;
    AbilityData ad;
    for (const auto& p : files) {
        std::string bytes;
        if (!read_file(p, bytes)) continue;
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, file_name(p));
        if (!doc.ok()) continue;
        walk(doc.root(), [&](const data::XmlNode& e) {
            if (iequals(e.name(), "Unit_Ability")) {
                UnitAbilityRecord u;
                if (parse_unit_ability(e, u, r, &diag)) {
                    ++records;
                    ++per_type[u.type];
                    if (u.gui_name_crc) ++gui;
                    // REMAINING_GAPS §5.2: every DEPLOY_TROOPERS user lists B_Trooper_00..02
                    if (u.type == kDeployTroopers &&
                        !(u.owner_attachment_bones.size() == 3 && u.owner_attachment_bones[0] == "B_Trooper_00" &&
                          u.owner_attachment_bones[2] == "B_Trooper_02"))
                        ++deploy_bad;
                } else {
                    ++bad;
                }
            } else if (iequals(e.name(), "Abilities")) {
                for (data::XmlNode c = e.first_child(); c; c = c.next_sibling()) {
                    const SpecialAbilityDesc s = parse_special_ability(c, r, nullptr);
                    ++specials;
                    ++classes[s.class_tag];
                    if (s.style == ActivationStyle::None && !c.child_text("Activation_Style").empty()) ++unknown_style;
                }
            }
        });
    }
    uint32_t mod_diag = 0;
    for (const std::string& d : diag)
        if (d.find("Mod_") != std::string::npos) ++mod_diag;
    std::printf("foc data: %zu xml files, %u Unit_Ability (%u unknown Type), %u with GUI_Activated_Ability_Name, %u special "
                "abilities in %zu classes, %u with an unknown Activation_Style, %u Mod_* diagnostics\n",
                files.size(), records, bad, gui, specials, classes.size(), unknown_style, mod_diag);
    std::printf("foc data: per type DEFEND %u P2W %u SPREAD_OUT %u HUNT %u TRACTOR %u ION %u STEALTH %u LUCKY %u MISSILE_SHIELD %u\n",
                per_type[kDefend], per_type[kPowerToWeapons], per_type[kSpreadOut], per_type[kHunt], per_type[kTractorBeam],
                per_type[kIonCannonShot], per_type[kStealth], per_type[kLuckyShot], per_type[kMissileShield]);
    CHECK(bad == 0);
    CHECK(mod_diag == 0);
    CHECK(records == 329);
    // Vanilla FoC counts of ABILITIES_HARDPOINTS.md 2.1, except DEFEND / TRACTOR_BEAM: the doc says 9 / 10,
    // the vanilla XML has 8 / 8 <Unit_Ability> elements (outside comments) of these types.
    CHECK(per_type[kDefend] == 8 && per_type[kPowerToWeapons] == 21 && per_type[kSpreadOut] == 29);
    CHECK(per_type[kHunt] == 25 && per_type[kTractorBeam] == 8 && per_type[kIonCannonShot] == 10);
    CHECK(per_type[kStealth] == 14 && per_type[kLuckyShot] == 7 && per_type[kMissileShield] == 5);
    CHECK(classes["Tractor_Beam_Attack_Ability"] > 0 && classes["Stealth_Ability"] > 0 && classes["Laser_Defense_Ability"] > 0);
    CHECK(unknown_style == 0);  // every written Activation_Style parses (incl. COMBAT_AUTOMATIC)
    std::printf("foc data: DEPLOY_TROOPERS %u, JET_PACK %u, EJECT_VEHICLE_THIEF %u\n", per_type[kDeployTroopers],
                per_type[kJetPack], per_type[kEjectVehicleThief]);
    CHECK(per_type[kDeployTroopers] > 0 && deploy_bad == 0);
}

}  // namespace

// HARDPOINT_DISABLED.md §9: Set_System_Online offline (disabled vs destroyed) and online (repair to max) per type.
void test_hardpoint_online_effects() {
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
    test_oi_repair_priorities();
    test_hardpoint_online_effects();
    test_oi_detonations();
    test_oi_shields_jamming_ion();
    test_oi_class_bodies();
    // REMAINING_GAPS §5
    test_gaps_eject_thief();
    test_gaps_deploy_troopers();
    test_gaps_jet_pack();
    test_gaps_stun_release();
    test_gaps_blast();
    test_gaps_leech();
    test_oi_weapon_swap();
    test_oi_berserker_speed();
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

// L1 systems tests: special weapons, lobbing superweapons, the tactical superweapon state machine
// (special_weapons.hpp, docs/re/SCRIPT_GAPS.md Â§1) and the combat pieces they use (launch_projectile, the
// WEAPON_SPECIAL force-1 fog gate). Hand-computed frames; repeated runs hash identically.
#include <cstddef>
#include <cstdio>
#include <memory>
#include <span>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/abilities/space_fields.hpp"
#include "alamo/tactical/combat/weather.hpp"
#include "alamo/tactical/abilities/special_weapons.hpp"
#include "thread_parallel_for.hpp"

using namespace alamo;
using namespace alamo::tactical;
using namespace alamo::tactical::combat;
using namespace alamo::tactical::abilities;
using alamo::tactical::Vec3;

namespace {

int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)
#define CHECK_EQ(a, b)                                                                                  \
    do {                                                                                                \
        const double va_ = static_cast<double>(a), vb_ = static_cast<double>(b);                        \
        if (!(va_ == vb_)) {                                                                            \
            std::printf("FAIL %s:%d: %s == %s (%.9g vs %.9g)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
            ++g_fail;                                                                                   \
        }                                                                                               \
    } while (0)

constexpr uint64_t kCatFighter = 1, kCatCapital = 8;

size_t count(std::span<const SpecialWeaponEvent> ev, SwEventKind k) {
    size_t n = 0;
    for (const SpecialWeaponEvent& e : ev) n += e.kind == k ? 1u : 0u;
    return n;
}

SpecialWeaponDesc ion_cannon() {  // Ground_Ion_Cannon in space (SpecialStructures.xml)
    SpecialWeaponDesc d;
    d.kinds = kSwSpecialWeapon;
    d.recharge_seconds = 30.0f;
    d.pulse_count = 3;
    d.pulse_delay_seconds = 0.5f;
    d.projectile = 0;
    d.valid_targets = kCatCapital;
    d.target_action_index = 1;
    return d;
}

SpecialWeaponWorld world_with_owners(const std::vector<int32_t>& owner_by_id, const std::vector<uint64_t>& cat_by_id) {
    SpecialWeaponWorld w;
    w.owner_of = [owner_by_id](ObjectId t) { return t >= 0 && static_cast<size_t>(t) < owner_by_id.size() ? owner_by_id[static_cast<size_t>(t)] : -1; };
    w.category_mask = [cat_by_id](ObjectId t) { return t >= 0 && static_cast<size_t>(t) < cat_by_id.size() ? cat_by_id[static_cast<size_t>(t)] : 0u; };
    return w;
}

void test_readiness() {
    SpecialWeaponSystem s({ion_cannon()});
    CHECK(s.add(5, 0, 0, 100));
    CHECK(!s.add(5, 0, 0, 100));  // once
    // [port] a new pack charges first: ready = 100 + int(30 x 30) = 1000.
    CHECK_EQ(s.ready_frame(5), 1000);
    CHECK_EQ(s.readiness(5, 100), 0.0f);
    CHECK_EQ(s.readiness(5, 550), 0.5f);
    CHECK_EQ(s.readiness(5, 999), static_cast<float>(899) / 900.0f);
    CHECK_EQ(s.readiness(5, 1000), 1.0f);
    CHECK_EQ(s.readiness(5, 5000), 1.0f);
    CHECK_EQ(s.readiness(6, 100), -1.0f);  // no pack
    SpecialWeaponConfig cfg;
    cfg.start_charged = true;
    SpecialWeaponSystem c({ion_cannon()}, cfg);
    CHECK(c.add(1, 0, 0, 10));
    CHECK_EQ(c.readiness(1, 10), 1.0f);
    // The Ready event fires once per charge.
    SimRng rng(1u);
    SpecialWeaponWorld w;
    s.tick(999, rng, w);
    CHECK_EQ(count(s.events(), SwEventKind::Ready), 0);
    s.tick(1000, rng, w);
    CHECK_EQ(count(s.events(), SwEventKind::Ready), 1);
    s.tick(1001, rng, w);
    CHECK_EQ(count(s.events(), SwEventKind::Ready), 0);
}

void test_targeting() {
    SpecialWeaponSystem s({ion_cannon()});
    CHECK(s.add(1, 0, 0, 0));
    CHECK(s.add(2, 0, 0, 0));
    CHECK(s.add(3, 0, 1, 0));
    CHECK(s.begin_targeting(1, 0));
    CHECK(s.targeting(1));
    CHECK(!s.begin_targeting(2, 0));  // space: another weapon of owner 0 is targeting
    CHECK(s.begin_targeting(3, 0));   // other owner
    CHECK(!s.begin_targeting(1, 0));  // second call toggles off and fails
    CHECK(!s.targeting(1));
    CHECK(s.begin_targeting(2, 0));
    SpecialWeaponConfig land;
    land.space = false;
    SpecialWeaponSystem l({ion_cannon()}, land);
    CHECK(l.add(1, 0, 0, 0));
    CHECK(l.add(2, 0, 0, 0));
    CHECK(l.begin_targeting(1, 0));
    CHECK(l.begin_targeting(2, 0));  // land: no one-per-owner rule
}

void test_fire_at_object() {
    SpecialWeaponConfig cfg;
    cfg.start_charged = true;
    SpecialWeaponSystem s({ion_cannon()}, cfg);
    CHECK(s.add(1, 0, 0, 0));
    // ids: 1 the weapon, 2 an enemy capital, 3 an enemy fighter, 4 an own capital.
    SpecialWeaponWorld w = world_with_owners({-1, 0, 1, 1, 0}, {0, 0, kCatCapital, kCatFighter, kCatCapital});
    CHECK(s.begin_targeting(1, 10));
    CHECK(!s.fire_at_object(1, 3, 10, w));  // category not in Special_Weapon_Valid_Targets
    CHECK(!s.fire_at_object(1, 4, 10, w));  // allied (same owner)
    CHECK(!s.fire_at_object(1, 1, 10, w));  // self
    SpecialWeaponWorld flagged = w;
    flagged.target_flagged = [](ObjectId t) { return t == 2; };
    CHECK(!s.fire_at_object(1, 2, 10, flagged));
    CHECK(s.fire_at_object(1, 2, 10, w));
    CHECK(!s.targeting(1));
    CHECK_EQ(s.ready_frame(1), 10 + 900);
    CHECK_EQ(s.readiness(1, 10), 0.0f);
    CHECK(!s.fire_at_object(1, 2, 11, w));  // not ready
    CHECK_EQ(count(s.events(), SwEventKind::Fired), 1);
    // Volley: 3 pulses, int(30 x 0.5) = 15 frames apart, the first at the fire frame.
    SimRng rng(7u);
    std::vector<uint32_t> pulse_frames;
    for (uint32_t f = 10; f < 60; ++f) {
        s.tick(f, rng, w);
        for (const SpecialWeaponShot& sh : s.shots()) {
            CHECK(sh.weapon == 1 && sh.target == 2 && !sh.at_point && sh.projectile == 0 && sh.owner == 0);
            CHECK_EQ(sh.pulse, pulse_frames.size());
            pulse_frames.push_back(f);
        }
    }
    CHECK(pulse_frames == std::vector<uint32_t>({10, 25, 40}));
    // A custom ally rule (allied players).
    SpecialWeaponWorld ally = w;
    ally.allied = [](int32_t, ObjectId) { return true; };
    CHECK(s.restart(1, 100, 0.0f));
    CHECK_EQ(s.readiness(1, 100), 1.0f);
    CHECK(!s.fire_at_object(1, 2, 100, ally));
}

void test_lobbing() {
    SpecialWeaponDesc d;
    d.kinds = kSwLobbing;
    d.recharge_seconds = 10.0f;
    d.projectile = 2;
    d.requires_base_power = true;
    SpecialWeaponConfig cfg;
    cfg.start_charged = true;
    cfg.space = false;
    SpecialWeaponSystem s({d}, cfg);
    CHECK(s.add(9, 0, 1, 0));
    CHECK_EQ(s.readiness(9, 0), 1.0f);
    CHECK(!s.begin_targeting(9, 0));  // lobbing objects have no targeting toggle
    SpecialWeaponWorld w;
    w.base_power = [](int32_t) { return false; };
    CHECK(!s.fire_lobbing_at(9, {100, 200, 0}, 5, w));  // Requires_Base_Power without power
    w.base_power = [](int32_t) { return true; };
    CHECK(s.fire_lobbing_at(9, {100, 200, 0}, 5, w));
    CHECK_EQ(s.ready_frame(9), 305);
    SimRng rng(3u);
    s.tick(5, rng, w);
    CHECK_EQ(s.shots().size(), 1);
    if (!s.shots().empty()) {
        CHECK(s.shots()[0].at_point && s.shots()[0].point.x == 100.0f && s.shots()[0].point.y == 200.0f);
        CHECK_EQ(s.shots()[0].projectile, 2);
    }
    CHECK(!s.fire_at_object(9, 1, 400, w));  // not a SPECIAL_WEAPON
}

SpecialWeaponDesc death_star() {  // SpaceUnitsSupers.xml Death_Star
    SpecialWeaponDesc d;
    d.kinds = kSwTsw;
    d.max_speed = 100.0f;
    d.tsw_start_distance = 8000.0f;
    d.tsw_attack_distance = 5800.0f;
    d.tsw_match_target_z = true;
    d.tsw_z_adjust = 1200.0f;
    d.tsw_power_up_seconds = 9.0f;
    d.tsw_attack_anim_seconds = 2.5f;
    d.tsw_debris_delay_frames = 33;
    d.tsw_post_destruction_frames = 350;
    return d;
}

struct TswHost {
    Vec3 pos{-20000.0f, 0.0f, 0.0f};
    Vec3 facing{1, 0, 0};
    bool have_target = true;
    bool attacker = false;
    uint32_t debris = 3;
    SpecialWeaponWorld world() {
        SpecialWeaponWorld w;
        w.find_tsw_target = [this](ObjectId, ObjectId& t, Vec3& p) {
            if (!have_target) return false;
            t = 77;
            p = Vec3{0.0f, 0.0f, 50.0f};
            return true;
        };
        w.position = [this](ObjectId) { return pos; };
        w.place = [this](ObjectId, Vec3 p, Vec3 f) {
            pos = p;
            facing = f;
        };
        w.is_battle_attacker = [this](int32_t) { return attacker; };
        w.debris_choices = [this](ObjectId) { return debris; };
        return w;
    }
};

void test_tsw_defender_path(StateHasher* out) {
    SpecialWeaponSystem s({death_star()});
    CHECK(s.add(4, 0, 1, 0));
    CHECK_EQ(s.tsw_state(4), kTswIdle);
    CHECK_EQ(s.readiness(4, 0), -1.0f);  // a TSW has no special-weapon pack
    TswHost host;
    host.have_target = false;
    SpecialWeaponWorld w = host.world();
    SimRng rng(11u);
    s.tick(1, rng, w);
    CHECK_EQ(s.tsw_state(4), kTswSearch);  // no target dummy yet
    host.have_target = true;
    s.tick(2, rng, w);  // first service with a target: placed at the start distance, target acquired
    CHECK_EQ(s.tsw_state(4), kTswAcquired);
    CHECK_EQ(host.pos.x, -8000.0f);
    CHECK_EQ(host.pos.z, 1250.0f);  // target z + TSW_Z_Adjust
    CHECK_EQ(host.facing.x, 1.0f);
    CHECK(!s.tsw_fire(4, true, 2));  // not ready
    s.tick(3, rng, w);
    CHECK_EQ(s.tsw_state(4), kTswArming);
    // Approach (8000 - 5800) / 100 = 22 frames.
    uint32_t f = 4;
    for (; f < 200 && s.tsw_state(4) != kTswReady; ++f) s.tick(f, rng, w);
    CHECK_EQ(f, 4 + 22);
    CHECK_EQ(host.pos.x, -5800.0f);
    CHECK_EQ(count(s.events(), SwEventKind::TswReady), 1);
    CHECK(!s.tsw_fire(4, false, f));  // planet test failed
    CHECK(s.tsw_fire(4, true, f));
    CHECK_EQ(s.tsw_state(4), kTswPowerUp);
    CHECK(!s.tsw_fire(4, true, f));
    // Power-up countdown int(30 x 9) = 270 services, then 6 -> 7 / 8 (attack anim 75 frames) -> 9 -> 10.
    const uint32_t fired = f;
    uint32_t anim_frame = 0, destroyed_frame = 0, debris_frame = 0, done_frame = 0;
    int32_t pick = -2;
    for (++f; f < fired + 1200; ++f) {
        s.tick(f, rng, w);
        for (const SpecialWeaponEvent& e : s.events()) {
            if (e.kind == SwEventKind::TswAttackAnim) anim_frame = f;
            if (e.kind == SwEventKind::TswPlanetDestroyed) {
                destroyed_frame = f;
                pick = e.value;
            }
            if (e.kind == SwEventKind::TswDebris) debris_frame = f;
            if (e.kind == SwEventKind::TswDone) done_frame = f;
        }
    }
    CHECK_EQ(anim_frame, fired + 270 + 1);
    CHECK_EQ(destroyed_frame, anim_frame + 75 + 1);
    CHECK(pick >= 0 && pick < 3);
    CHECK_EQ(debris_frame, destroyed_frame + 33);
    CHECK_EQ(done_frame, destroyed_frame + 350);
    CHECK_EQ(s.tsw_state(4), kTswDone);
    if (out) s.hash(*out);
}

void test_tsw_attacker() {
    SpecialWeaponSystem s({death_star()});
    CHECK(s.add(4, 0, 1, 0));
    TswHost host;
    host.attacker = true;
    host.debris = 1;
    SpecialWeaponWorld w = host.world();
    SimRng rng(5u);
    const uint64_t before = rng.draws();
    s.tick(1, rng, w);
    CHECK_EQ(s.tsw_state(4), kTswReady);  // the attacker's TSW starts ready at the attack distance
    CHECK_EQ(host.pos.x, -5800.0f);
    CHECK(s.tsw_fire(4, true, 1));
    for (uint32_t f = 2; f < 400; ++f) s.tick(f, rng, w);
    CHECK_EQ(rng.draws(), before);  // one debris type: no draw
    CHECK(s.tsw_state(4) >= kTswAftermath);
}

// ---- combat pieces ----

struct Combat {
    CombatData cd;
    std::unique_ptr<CombatSystem> sys;
    TreeSet trees{2};
    SimRng rng{99u};
    Combat() {
        ProjectileDesc p;
        p.name = "Proj_Test";
        p.max_speed = 50.0f;
        p.damage = 100.0f;
        p.max_lifetime = 10.0f;
        cd.projectiles.push_back(p);
        ProjectileDesc blast = p;
        blast.name = "Proj_Blast";
        blast.damage = 0.0f;
        blast.blast_area_damage = 40.0f;
        blast.blast_area_range = 300.0f;
        cd.projectiles.push_back(blast);
        UnitDesc u;
        u.name = "Target";
        u.tactical_health = 1000.0f;
        u.radius = 20.0f;
        u.category_mask = kCatCapital;
        cd.units.push_back(u);
        CombatConfig cfg;
        cfg.opportunity_fire = false;
        sys = std::make_unique<CombatSystem>(cd, cfg, std::vector<PlayerInfo>{PlayerInfo{}, PlayerInfo{}});
        sys->bind_rng(&rng);
    }
    void add(ObjectId id, uint32_t desc, int32_t owner, Vec3 pos) {
        UnitSpawn s;
        s.id = id;
        s.desc = desc;
        s.owner = owner;
        s.position = pos;
        CHECK(sys->add_unit(s, 0, rng));
        TreeMembership m;
        m.owner = owner;
        const float r = cd.units[desc].radius;
        CHECK(trees.add(static_cast<EntityHandle>(id), {{pos.x - r, pos.y - r, pos.z - r}, {pos.x + r, pos.y + r, pos.z + r}}, 1u, m));
    }
};

void test_launch_projectile() {
    Combat c;
    c.add(10, 0, 1, {500, 0, 0});
    const float h0 = c.sys->health(10);
    ProjectileLaunch l;
    l.shooter = 3;  // not a combat unit (a special weapon object)
    l.owner = 0;
    l.projectile = 0;
    l.from = {0, 0, 0};
    l.target = 10;
    CHECK(c.sys->launch_projectile(l, 1));
    l.target = 11;  // unknown target
    CHECK(!c.sys->launch_projectile(l, 1));
    l.target = kNoObject;
    l.projectile = 5;  // unknown projectile
    CHECK(!c.sys->launch_projectile(l, 1));
    CHECK_EQ(c.sys->projectile_count(), 1);
    for (uint32_t f = 2; f < 30; ++f) c.sys->tick(f, c.trees, c.rng);
    CHECK_EQ(c.sys->health(10), h0 - 100.0f);
    CHECK_EQ(c.sys->stats().hits, 1);
    // A point shot detonates at the point: area damage on expiry hits the unit within 300.
    ProjectileLaunch b;
    b.shooter = 3;
    b.owner = 0;
    b.projectile = 1;
    b.from = {0, 0, 0};
    b.point = {450, 100, 0};
    const float h1 = c.sys->health(10);
    CHECK(c.sys->launch_projectile(b, 30));
    for (uint32_t f = 31; f < 60; ++f) c.sys->tick(f, c.trees, c.rng);
    CHECK(c.sys->health(10) < h1);
    CHECK_EQ(c.sys->projectile_count(), 0);
}

class HideAll final : public CombatHooks {
public:
    bool hide = true;
    mutable uint32_t forced_queries = 0;
    bool target_hidden(int32_t, ObjectId, bool force) const override {
        if (force) ++forced_queries;
        return hide && force;  // hidden only for force-1 queries (an AI shooter: the fog's AI gate)
    }
};

void test_weapon_special_fog_gate() {
    // A station with one WEAPON_SPECIAL and one laser hard point; an enemy in range hidden (force 1 only).
    for (int kind = 0; kind < 2; ++kind) {
        Combat c;
        HardpointDesc hd;
        hd.name = kind == 0 ? "HP_SPECIAL" : "HP_LASER";
        hd.kind = kind == 0 ? HardpointKind::WeaponSpecial : HardpointKind::WeaponLaser;
        hd.projectile = 0;
        hd.min_recharge = hd.max_recharge = 1.0f;
        hd.fire_range = 2000.0f;
        hd.health = 100.0f;
        c.cd.hardpoints.push_back(hd);
        UnitDesc st;
        st.name = "Station";
        st.tactical_health = 5000.0f;
        st.radius = 50.0f;
        st.hardpoints = {0};
        st.max_attack_distance = 2000.0f;
        c.cd.units.push_back(st);
        CombatConfig cfg;
        cfg.opportunity_fire = false;
        c.sys = std::make_unique<CombatSystem>(c.cd, cfg, std::vector<PlayerInfo>{PlayerInfo{}, PlayerInfo{}});
        c.sys->bind_rng(&c.rng);
        HideAll hooks;
        c.sys->set_hooks(&hooks);
        c.add(1, 1, 0, {0, 0, 0});
        c.add(2, 0, 1, {800, 0, 0});
        for (uint32_t f = 1; f < 200; ++f) c.sys->tick(f, c.trees, c.rng);
        if (kind == 0) {
            CHECK_EQ(c.sys->stats().shots, 0);  // WEAPON_SPECIAL: never at a force-1-hidden target
            CHECK(hooks.forced_queries > 0);
            hooks.hide = false;
            for (uint32_t f = 200; f < 300; ++f) c.sys->tick(f, c.trees, c.rng);
            CHECK(c.sys->stats().shots > 0);
        } else {
            // OI tactical (TACTICAL_GAPS 5.8): targeting's FOW test is force 1 too, so no weapon acquires the hidden target.
            CHECK_EQ(c.sys->stats().shots, 0);
        }
        c.sys->set_hooks(nullptr);
    }
}

void test_space_fields() {
    SpaceFieldIndex f;
    CHECK(f.empty() && f.kinds_at(0, 0) == 0 && f.nearest(0, 0, ~0u) == -1);
    f.add({10, kFieldNebula, 0.0f, 0.0f, 100.0f});
    f.add({11, kFieldIonStorm | kFieldNebula, 150.0f, 0.0f, 100.0f});
    f.add({12, kFieldAsteroid, 1000.0f, 0.0f, 50.0f});
    CHECK_EQ(f.kinds_at(0, 0), kFieldNebula);
    CHECK_EQ(f.kinds_at(100, 0), kFieldNebula | kFieldIonStorm);  // both circles (boundary inclusive)
    CHECK_EQ(f.kinds_at(100.5f, 0), kFieldNebula | kFieldIonStorm);
    CHECK_EQ(f.kinds_at(1049, 0), kFieldAsteroid);
    CHECK_EQ(f.kinds_at(1051, 0), 0u);
    CHECK(f.contains(10, 10, kFieldNebula) && !f.contains(10, 10, kFieldAsteroid));
    CHECK_EQ(f.nearest(80, 0, kFieldNebula), 1);  // centre 150 is nearer than 0 (70 vs 80)
    CHECK_EQ(f.nearest(75, 0, kFieldNebula), 0);  // tie (75 / 75): the earlier field
    CHECK_EQ(f.nearest(-500, 0, kFieldAsteroid), 2);
    CHECK_EQ(f.nearest(0, 0, 0u), -1);
    CHECK(f.remove(11) && !f.remove(11));
    CHECK_EQ(f.kinds_at(200, 0), 0u);
}

// ---- weather (combat/weather.hpp, L1_S4_REMAINING §3.2) ----
void test_weather() {
    WeatherData data;
    WeatherScenario sc;
    sc.name = "Rain_Test";
    sc.phases.push_back({0.0f, 0.0f, 0.0f, WeatherType::Clear, 10, 4});
    sc.phases.push_back({1.0f, 0.5f, 0.2f, WeatherType::Rain, 20, 5});
    data.scenarios.push_back(sc);
    data.modifiers[static_cast<size_t>(WeatherType::Rain)].accuracy[7] = 0.4f;  // LASER
    data.modifiers[static_cast<size_t>(WeatherType::Rain)].sight = {{"INFANTRY", 0.7f}, {"DEFAULT", 0.9f}};
    CHECK(data.find("rain_test") == &data.scenarios[0] && !data.find("x") && !data.find(""));
    WeatherSystem w;
    SimRng rng(77u), ref(77u);
    w.start(data.find("Rain_Test"), &data, 100, rng);
    const float len0 = ref.range_float(3.0f, 10.0f) * 30.0f;
    CHECK(w.active() && w.type() == WeatherType::Clear && w.phase_index() == 0 && rng == ref);
    CHECK_EQ(w.accuracy_modifier(7), 1.0f);  // I <= 0.5: the CLEAR block
    uint32_t f = 101;
    for (; !w.transitioning() && f < 2000; ++f) w.service(f, rng);
    CHECK_EQ(static_cast<float>(f - 1 - 100), static_cast<float>(static_cast<uint32_t>(len0) + (len0 > static_cast<float>(static_cast<uint32_t>(len0)) ? 1u : 0u)));
    const float len1 = ref.range_float(0.3f * 4.0f, 4.0f) * 30.0f;
    CHECK(rng == ref);
    // Ease-out: intensity rises toward phase 1 (crossing 0.5 raises FowDirty), then the advance (one draw).
    bool dirty = false;
    const uint32_t trans_start = f - 1;
    for (; w.phase_index() == 0 && f < 4000; ++f) {
        w.clear_events();
        w.service(f, rng);
        for (const WeatherEvent& e : w.events()) dirty = dirty || e.kind == WeatherEvent::Kind::FowDirty;
    }
    (void)len1;
    ref.range_float(0.3f * 20.0f, 20.0f);
    CHECK(rng == ref && dirty && w.type() == WeatherType::Rain && f - 1 > trans_start);
    CHECK_EQ(w.intensity(), 1.0f);
    CHECK_EQ(w.accuracy_modifier(7), 0.4f);
    CHECK_EQ(w.accuracy_row()[7], 0.4f);
    CHECK_EQ(w.accuracy_modifier(0), 1.0f);
    CHECK_EQ(w.accuracy_modifier(12), 0.0f);  // out of range -> 0 [H]
    CHECK_EQ(w.sight_modifier("Infantry"), 0.7f);
    CHECK_EQ(w.sight_modifier("Speeder"), 0.9f);  // fallback "Default"
    CHECK_EQ(w.sight_modifier(""), 0.9f);
    CHECK_EQ(w.speed_modifier("Infantry"), 1.0f);  // missing -> 1
    // Force_Weather: one immediate advance (phase 1 -> 0), one draw.
    CHECK(w.force_weather(f, rng));
    ref.range_float(0.3f * 10.0f, 10.0f);
    CHECK(rng == ref && w.phase_index() == 0 && w.type() == WeatherType::Clear && !w.transitioning());
    WeatherSystem off;
    off.start(nullptr, &data, 1, rng);
    CHECK(!off.active() && !off.force_weather(2, rng));
    CHECK(weather_type_from_name("sandstorm") == WeatherType::Sandstorm && projectile_category_index("mptl_rocket") == 6);
    bool known = true;
    CHECK(weather_type_from_name("FOG", &known) == WeatherType::Clear && !known);
}

// ---- ion storm / nebula stamps (AbilitySystem, L1_S4_REMAINING §3.3) ----
void test_field_stamps() {
    Combat c;
    c.cd.units[0].shield_points = 100.0f;
    c.sys = std::make_unique<CombatSystem>(c.cd, CombatConfig{}, std::vector<PlayerInfo>{PlayerInfo{}, PlayerInfo{}});
    AbilityData ad;
    ad.objects.resize(1);
    AbilitySystem ab(*c.sys, ad, AbilityConfig{});
    UnitSpawn s;
    s.id = 5;
    s.owner = 1;
    CHECK(ab.add_unit(s, 0, 1, c.rng));
    DamageRequest r;
    r.victim = 5;
    r.amount = 10.0f;
    ab.set_ion_storm(5, 100);
    CHECK(ab.ion_storm_lockout(5, 100) && ab.ion_storm_lockout(5, 249) && !ab.ion_storm_lockout(5, 250));
    const DamageResult down = c.sys->take_damage(r, 120);
    CHECK_EQ(down.absorbed, 0.0f);  // shields absorb nothing in the ion-storm lockout
    ab.clear_ion_storm(5);
    const DamageResult up = c.sys->take_damage(r, 121);
    CHECK(up.absorbed > 0.0f);
    CHECK(!ab.nebula_stamped(5));
    ab.set_nebula(5, 10);
    CHECK(ab.nebula_stamped(5) && ab.nebula_lockout(5, 159) && !ab.nebula_lockout(5, 160));
    ab.clear_nebula(5);
    CHECK(!ab.nebula_stamped(5) && !ab.nebula_lockout(5, 11));
    CHECK_EQ(ab.cancel_active_abilities(5, 12), 0);
}

// ---- N1 stage 2: save / load mid-battle (SpecialWeaponSystem, SpaceFieldIndex write / read) ----

template <class S>
std::vector<uint8_t> bytes_of(const S& s) {
    ByteWriter w;
    s.write(w);
    return w.bytes();
}

uint64_t sw_digest(const SpecialWeaponSystem& s, const SimRng& rng, const TswHost& host) {
    StateHasher h;
    s.hash(h);
    rng.hash(h);
    h.f32(host.pos.x);
    h.f32(host.pos.y);
    h.f32(host.pos.z);
    return h.digest();
}

bool same_outputs(const SpecialWeaponSystem& a, const SpecialWeaponSystem& b) {
    if (a.shots().size() != b.shots().size() || a.events().size() != b.events().size()) return false;
    for (size_t i = 0; i < a.shots().size(); ++i) {
        const SpecialWeaponShot& x = a.shots()[i];
        const SpecialWeaponShot& y = b.shots()[i];
        if (x.weapon != y.weapon || x.target != y.target || x.pulse != y.pulse || x.projectile != y.projectile ||
            x.at_point != y.at_point || x.point.x != y.point.x || x.point.y != y.point.y)
            return false;
    }
    for (size_t i = 0; i < a.events().size(); ++i) {
        const SpecialWeaponEvent& x = a.events()[i];
        const SpecialWeaponEvent& y = b.events()[i];
        if (x.kind != y.kind || x.id != y.id || x.target != y.target || x.value != y.value || x.frame != y.frame) return false;
    }
    return true;
}

// The battle's script (frames > the save point run on both copies): fire the ion cannon, a lobbing barrage, a second
// cannon's targeting, the Death Star.
void sw_script(SpecialWeaponSystem& s, uint32_t f, const SpecialWeaponWorld& w) {
    if (f == 800) CHECK(s.tsw_fire(4, true, f));            // power-up until 1070
    if (f == 905) CHECK(s.fire_at_object(1, 2, f, w));     // 3 pulses at 905 / 920 / 935 (saved at 920)
    if (f == 910) CHECK(s.begin_targeting(5, f));          // owner 1's cannon targeting at the save
    if (f == 920) CHECK(s.fire_lobbing_at(9, Vec3{300.0f, -40.0f, 0.0f}, f, w));  // pulses 920 / 950 / 980
    if (f == 1000) CHECK(s.fire_at_object(5, 6, f, w));    // the targeting cannon fires after the load
    if (f == 1400) CHECK(s.restart(1, f, 2.0f));
}

void test_special_weapon_save() {
    // ids: 1 / 5 ion cannons (owners 0 / 1), 2 / 6 capitals (owners 1 / 0), 4 the Death Star (owner 1), 9 lobbing (0).
    const std::vector<int32_t> owners = {-1, 0, 1, 1, 1, 1, 0, 0, 0, 0};
    const std::vector<uint64_t> cats = {0, 0, kCatCapital, kCatFighter, kCatCapital, 0, kCatCapital, 0, 0, 0};
    SpecialWeaponDesc lob;
    lob.kinds = kSwLobbing;
    lob.recharge_seconds = 10.0f;
    lob.projectile = 2;
    lob.pulse_count = 3;
    lob.pulse_delay_seconds = 1.0f;
    // The host adds descriptors lazily per type (TacticalBattle's type -> desc cache); the loaded copy starts empty.
    SpecialWeaponSystem a;
    const uint32_t ion = a.add_desc(ion_cannon()), ds = a.add_desc(death_star()), lb = a.add_desc(lob);
    CHECK(a.add(1, ion, 0, 0) && a.add(5, ion, 1, 0) && a.add(4, ds, 1, 0) && a.add(9, lb, 0, 0));
    TswHost ha;
    ha.debris = 3;  // a synced debris draw after the load
    SpecialWeaponWorld wa = ha.world();
    const SpecialWeaponWorld owned = world_with_owners(owners, cats);
    wa.owner_of = owned.owner_of;
    wa.category_mask = owned.category_mask;
    SimRng ra(21u);
    const uint32_t kSave = 920, kEnd = 1600;
    for (uint32_t f = 0; f <= kSave; ++f) {
        sw_script(a, f, wa);
        a.tick(f, ra, wa);
    }
    CHECK(a.tsw_state(4) == kTswPowerUp && a.targeting(5) && !a.shots().empty());  // mid power-up, mid-volley
    const std::vector<uint8_t> saved = bytes_of(a);

    SpecialWeaponSystem b;
    ByteReader r(saved);
    CHECK(b.read(r));
    TswHost hb = ha;  // the host's own state (the TSW position)
    SpecialWeaponWorld wb = hb.world();
    wb.owner_of = owned.owner_of;
    wb.category_mask = owned.category_mask;
    SimRng rb = ra;
    CHECK(bytes_of(b) == saved && b.descs().size() == 3 && same_outputs(a, b));
    CHECK(sw_digest(a, ra, ha) == sw_digest(b, rb, hb));
    uint32_t diverged = 0, shots = 0, events = 0;
    bool destroyed = false;
    for (uint32_t f = kSave + 1; f <= kEnd; ++f) {
        sw_script(a, f, wa);
        sw_script(b, f, wb);
        a.tick(f, ra, wa);
        b.tick(f, rb, wb);
        shots += static_cast<uint32_t>(a.shots().size());
        events += static_cast<uint32_t>(a.events().size());
        destroyed = destroyed || count(a.events(), SwEventKind::TswPlanetDestroyed) > 0;
        if (!same_outputs(a, b) || sw_digest(a, ra, ha) != sw_digest(b, rb, hb)) ++diverged;
    }
    CHECK(diverged == 0 && destroyed && shots >= 5 && a.tsw_state(4) == kTswDone);
    CHECK(bytes_of(a) == bytes_of(b) && ra.draws() == rb.draws());

    // Rejected data leaves the system unchanged: truncation, trailing bytes, version, a pack's descriptor, a TSW state.
    SpecialWeaponSystem c({ion_cannon()});
    CHECK(c.add(3, 0, 0, 7));
    const std::vector<uint8_t> before = bytes_of(c);
    const auto rejected = [&c](const std::vector<uint8_t>& v) {
        ByteReader t(v);
        return !c.read(t);
    };
    uint32_t accepted = 0;
    for (size_t len = 0; len < saved.size(); ++len)
        if (!rejected(std::vector<uint8_t>(saved.begin(), saved.begin() + static_cast<std::ptrdiff_t>(len)))) ++accepted;
    CHECK(accepted == 0);
    std::vector<uint8_t> bad = saved;
    bad.push_back(0);
    CHECK(rejected(bad));
    bad = saved;
    bad[0] = 0;
    CHECK(rejected(bad));
    // version, descriptor count, 3 descriptors (63 bytes), pack count; packs (85 bytes) ascending id: 1, 4, 5, 9.
    const size_t packs = 4 + 4 + 3 * 63 + 4;
    bad = saved;
    bad[packs + 4] = 3;  // the first pack's descriptor index: past the table
    CHECK(rejected(bad));
    bad = saved;
    bad[packs + 85 + 51] = 12;  // the Death Star pack's TSW state: past kTswDone
    CHECK(rejected(bad));
    bad = saved;
    bad[packs + 85] = 0x7F;  // the second pack's id (127 > 5): ids out of order
    CHECK(rejected(bad));
    CHECK(bytes_of(c) == before);
    ByteReader ok(saved);
    CHECK(c.read(ok) && bytes_of(c) == saved);
    std::printf("special weapons save: %zu bytes at frame %u (TSW powering up, 2 volleys in flight), %u frames continued "
                "identically (%u shots, %u events)\n",
                saved.size(), kSave, kEnd - kSave, shots, events);

    // SpaceFieldIndex: fields in insertion order.
    SpaceFieldIndex f;
    f.add({10, kFieldNebula, 0.0f, 0.0f, 100.0f});
    f.add({11, kFieldIonStorm | kFieldNebula, 150.0f, 0.0f, 100.0f});
    f.add({12, kFieldAsteroid, 1000.0f, 0.0f, 50.0f});
    const std::vector<uint8_t> fb = bytes_of(f);
    SpaceFieldIndex g;
    ByteReader rg(fb);
    CHECK(g.read(rg) && bytes_of(g) == fb && g.kinds_at(100, 0) == (kFieldNebula | kFieldIonStorm) &&
          g.nearest(-500, 0, kFieldAsteroid) == 2);
    StateHasher h1, h2;
    f.hash(h1);
    g.hash(h2);
    CHECK(h1.digest() == h2.digest());
    SpaceFieldIndex e;
    e.add({1, kFieldAsteroid, 5.0f, 5.0f, 5.0f});
    const std::vector<uint8_t> e_before = bytes_of(e);
    accepted = 0;
    for (size_t len = 0; len < fb.size(); ++len) {
        ByteReader t(std::span<const uint8_t>(fb.data(), len));
        if (e.read(t)) ++accepted;
    }
    bad = fb;
    bad.push_back(0);
    ByteReader rt(bad);
    CHECK(accepted == 0 && !e.read(rt) && bytes_of(e) == e_before);
}

}  // namespace

int main() {
    test_space_fields();
    test_weather();
    test_field_stamps();
    test_readiness();
    test_targeting();
    test_fire_at_object();
    test_lobbing();
    StateHasher a, b;
    test_tsw_defender_path(&a);
    test_tsw_defender_path(&b);
    CHECK(a.digest() == b.digest());
    test_tsw_attacker();
    test_launch_projectile();
    test_weapon_special_fog_gate();
    test_special_weapon_save();  // N1 stage 2
    if (g_fail) {
        std::printf("alamo_tactical_special_weapons_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_tactical_special_weapons_tests: OK\n");
    return 0;
}

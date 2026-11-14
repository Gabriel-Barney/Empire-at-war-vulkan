// Part 2 of test_abilities (bo3 t03: split by position; see test_abilities_common.hpp).
#include "test_abilities_common.hpp"

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

void test_oi_stun_over_water() {
    // OI t02 (ABILITIES_HARDPOINTS §4.12 [port]): Destroy_When_Stunned_Over_Water - a flagged STUNNABLE unit whose stun
    // starts over water (host: here x < 0) is destroyed at once; on land, or without the flag, it is only stunned. No
    // over_water host: never destroyed.
    for (const bool water_host : {true, false}) {
        OiWorld w;
        UnitDesc u = oi_unit("MAL", 100.0f);
        u.stunnable = true;
        AbilityObjectDesc flagged;
        flagged.destroy_when_stunned_over_water = true;
        const uint32_t fd = push(w.cd, w.ad, u, flagged);
        const uint32_t pd = push(w.cd, w.ad, u, AbilityObjectDesc{});
        w.make(GameMode::Land);
        if (water_host) {
            AbilityHost h;
            h.over_water = [](Vec3 p) { return p.x < 0.0f; };
            w.ab->set_host(std::move(h));
        }
        w.add(1, fd, 0, {-50, 0, 0});  // flagged, over water
        w.add(2, fd, 0, {50, 0, 0});   // flagged, on land
        w.add(3, pd, 0, {-60, 0, 0});  // not flagged, over water
        for (ObjectId id : {ObjectId{1}, ObjectId{2}, ObjectId{3}}) CHECK(w.sys->stun_unit(id, 30, w.frame));
        const bool sunk = (w.sys->flags(1) & kUnitDead) != 0;
        CHECK(sunk == water_host);
        CHECK((w.sys->flags(2) & kUnitDead) == 0 && w.sys->stunned(2, w.frame));
        CHECK((w.sys->flags(3) & kUnitDead) == 0 && w.sys->stunned(3, w.frame));
        size_t deaths = 0;
        for (const DeathEvent& d : w.sys->deaths()) deaths += d.id == 1 ? 1u : 0u;
        CHECK(deaths == (water_host ? 1u : 0u));
        w.tick();
        CHECK((w.sys->flags(2) & kUnitDead) == 0 && (w.sys->flags(3) & kUnitDead) == 0);
    }
}
}  // namespace bo3split

namespace bo3split {

// OI t03: ticks from the order to the Hack's owner change; count > 0: animation 0x5a has `count` frames at `fps`.
int oi_hack_timing(uint32_t count, float fps) {
    OiWorld w;
    SpecialAbilityDesc hk = oi_special("Hack", SpecialClass::Hack, ActivationStyle::UserInput);
    hk.max_range = 1000.0f;
    const uint32_t hs = add_special(w.ad, hk);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kTargetedHack, 0.0f, 10.0f, "Hack")};
    a.specials = {hs};
    const uint32_t ad = push(w.cd, w.ad, oi_unit("A", 100.0f), a);
    const uint32_t td = push(w.cd, w.ad, oi_unit("T", 100.0f), AbilityObjectDesc{});
    w.make(GameMode::Land);
    if (count > 0) {
        AbilityHost h;
        h.anim_clip = [count, fps](ObjectId id, int32_t type, std::string_view, uint32_t, uint32_t& n, float& f) {
            if (id != 1 || type != 0x5a) return false;
            n = count;
            f = fps;
            return true;
        };
        w.ab->set_host(std::move(h));
    }
    w.add(1, ad, 0, {0, 0, 0});
    w.add(2, td, 1, {300, 0, 0});
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kTargetedHack, 2, -1, w.frame) == 1);
    for (int i = 1; i <= 100; ++i) {
        w.tick();
        if (w.sys->owner(2) == 0) return i;
    }
    return -1;
}
}  // namespace bo3split

namespace bo3split {

// OI t03: enemy health lost to a 2 s Berserker (Damage 10 per pass); count > 0: animation 0x68 has `count` frames (30 fps).
float oi_berserker_damage(uint32_t count) {
    OiWorld w;
    SpecialAbilityDesc b = oi_special("Berserk", SpecialClass::Berserker, ActivationStyle::UserInput);
    b.max_range = 500.0f;
    b.berserker_speed = 5.0f;
    b.berserker_damage = 10.0f;
    b.berserker_damage_range = 1000.0f;
    b.berserker_duration = 2.0f;  // 60 frames
    const uint32_t bs = add_special(w.ad, b);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kBerserker, 0.0f, 60.0f, "Berserk")};
    a.specials = {bs};
    const uint32_t bd = push(w.cd, w.ad, oi_unit("Berserker", 1000.0f), a);
    UnitDesc e = oi_unit("Enemy", 1000.0f);
    e.has_locomotor = true;
    const uint32_t td = push(w.cd, w.ad, e, AbilityObjectDesc{});
    w.make(GameMode::Land);
    if (count > 0) {
        AbilityHost h;
        h.anim_clip = [count](ObjectId id, int32_t type, std::string_view, uint32_t, uint32_t& n, float& f) {
            if (id != 1 || type != 0x68) return false;
            n = count;
            f = 30.0f;
            return true;
        };
        w.ab->set_host(std::move(h));
    }
    w.add(1, bd, 0, {0, 0, 0});
    w.add(2, td, 1, {100, 0, 0});
    const ObjectId one[] = {1};
    CHECK(w.ab->targeted_ability(one, kBerserker, 2, -1, w.frame) == 1);
    for (int i = 0; i < 80; ++i) w.tick();
    CHECK(!w.ab->special_applied(1, name_crc("Berserk")));
    return 1000.0f - w.sys->health(2);
}
}  // namespace bo3split

namespace bo3split {

void test_oi_anim_timings() {
    // OI t03 (ABILITIES_HARDPOINTS §4.13 [port]): clip position = sim frames since the start x (fps / 30) x speedup;
    // "at frame N" = first update with position >= N, "at the end" = position >= max(count - 2, 0); no clip: rate 1 and
    // the fixed fallbacks (removal with the last event, Hack 60 frames, Berserker window open).
    const std::array<int, 2> base = oi_generic_timing(0, 0.0f, 1.0f);
    CHECK(base[0] > 0 && base[1] == base[0]);  // no clip: hit and removal in the same update
    const std::array<int, 2> c30 = oi_generic_timing(20, 30.0f, 1.0f);
    CHECK(c30[0] == base[0] && c30[1] == base[0] + 8);  // end frame 18: held 8 frames past the hit at 10
    const std::array<int, 2> c15 = oi_generic_timing(20, 15.0f, 1.0f);
    CHECK(c15[0] == base[0] + 10 && c15[1] == base[0] + 26);  // half rate: hit at 20 sim frames, end at 36
    const std::array<int, 2> fast = oi_generic_timing(20, 30.0f, 2.0f);
    CHECK(fast[0] == base[0] - 5 && fast[1] == base[0] - 1);  // speedup 2: hit at 5 sim frames, end at 9
    const std::array<int, 2> short_clip = oi_generic_timing(5, 30.0f, 1.0f);
    CHECK(short_clip[0] == base[0] && short_clip[1] == base[0]);  // clip ends before the hit: removed with it
    // Hack: animation 0x5a end = ceil(max(count - 2, 0) / rate) sim frames, else kHackFrames (60).
    const int hack60 = oi_hack_timing(0, 0.0f);
    CHECK(hack60 > 60);
    CHECK(oi_hack_timing(32, 30.0f) == hack60 - 30);
    CHECK(oi_hack_timing(32, 60.0f) == hack60 - 45);
    CHECK(oi_hack_timing(33, 60.0f) == hack60 - 44);  // 31 / 2 rounds up to 16
    // Berserker: passes on counters 55, 50, ... 5 (n = 5 .. 55); a 50-frame clip loops at 49, window (10, 40) strict:
    // n = 15, 20, 25, 30, 35 only.
    CHECK(oi_berserker_damage(0) == 110.0f);
    CHECK(oi_berserker_damage(50) == 50.0f);
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

bool load_battle(CombatSystem& c, AbilitySystem& a, SimRng& rng, const SavedBattle& s) {
    ByteReader rc(s.combat), ra(s.abilities), rr(s.rng);
    return c.read(rc) && a.read(ra) && rng.read(rr);
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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

    const ParallelFor pf4 = alamo::test::make_thread_parallel_for(4);
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
}  // namespace bo3split

namespace bo3split {

// OI follow-up t01 / t03 (ABILITIES_HARDPOINTS §4.14, A1_READINGS §28.9): a Sith with Force_Lightning (FL_Attack 0x28: a
// 32-frame 30 fps clip when `clips`), FORCE_SIGHT and HUNT; troopers 2, 3 in range. `script_lock` is the host's share.
void oi_gate_world(OiWorld& w, bool clips, const bool* script_lock) {
    SpecialAbilityDesc l = oi_special("Lightning", SpecialClass::ForceLightning, ActivationStyle::UserInput);
    l.max_range = 300.0f;
    l.max_targets = 1;
    l.targets_radius = 10.0f;
    const uint32_t ls = add_special(w.ad, l);
    AbilityObjectDesc a;
    a.unit_abilities = {oi_gui(kForceLightning, 0.0f, 30.0f, "Lightning"), rec(kForceSight, 0.0f, 0.0f), rec(kHunt, 0.0f, 0.0f)};
    a.specials = {ls};
    const uint32_t ud = push(w.cd, w.ad, oi_unit("Sith", 100.0f), a);
    const uint32_t td = push(w.cd, w.ad, oi_unit("Trooper", 100.0f), AbilityObjectDesc{});
    w.make(GameMode::Land);
    AbilityHost h;
    if (clips)
        h.anim_clip = [](ObjectId, int32_t type, std::string_view, uint32_t, uint32_t& n, float& f) {
            if (type != 0x28) return false;
            n = 32;
            f = 30.0f;
            return true;
        };
    h.movement_locked = [script_lock](ObjectId id) { return id == 1 && script_lock && *script_lock; };
    h.map_rect = [](Vec3& lo, Vec3& hi) {
        lo = Vec3{-1000.0f, -1000.0f, 0.0f};
        hi = Vec3{1000.0f, 1000.0f, 0.0f};
        return true;
    };
    w.ab->set_host(std::move(h));
    w.add(1, ud, 0, {0, 0, 0});
    w.add(2, td, 1, {150, 0, 0});
    w.add(3, td, 1, {150, 100, 0});
}
}  // namespace bo3split

namespace bo3split {

void test_oi_anim_gates_and_locks() {
    const ObjectId one[] = {1};
    // §4.14: the cast records FL_Attack for the clip's length (end = 32 - 2 = 30 frames, then idle). While it runs a
    // second Lightning is refused and FORCE_SIGHT is busy (Can_Activate 0x34); afterwards both pass.
    {
        OiWorld w;
        oi_gate_world(w, true, nullptr);
        CHECK(w.ab->anim_type_at(1, w.frame) == -1 && w.ab->can_activate(1, kForceSight, false, false, w.frame));
        CHECK(w.ab->targeted_ability(one, kForceLightning, 2, -1, w.frame) == 1);
        w.tick();
        CHECK(!w.sys->alive(2) && w.ab->anim_type_at(1, w.frame) == 0x28);
        CHECK(!w.ab->can_activate(1, kForceSight, false, false, w.frame));
        CHECK(w.ab->force_recharge(1, kForceLightning, 0.0f));
        w.ab->targeted_ability(one, kForceLightning, 3, -1, w.frame);
        w.tick();
        CHECK(w.sys->alive(3));  // refused: FL_Attack still runs
        int idle = 0;
        for (int i = 1; i <= 40 && idle == 0; ++i) {
            w.tick();
            if (w.ab->anim_type_at(1, w.frame) == -1) idle = i;
        }
        CHECK(idle == 28);  // played at frame F; idle from F + 30 (two ticks already passed)
        CHECK(!w.ab->can_activate(1, kForceSight, false, false, w.frame));  // the gates read the previous frame's advance
        w.tick();
        CHECK(w.ab->can_activate(1, kForceSight, false, false, w.frame));
        CHECK(w.ab->force_recharge(1, kForceLightning, 0.0f));
        CHECK(w.ab->targeted_ability(one, kForceLightning, 3, -1, w.frame) == 1);
        w.tick();
        w.tick();
        CHECK(!w.sys->alive(3));
    }
    // No clip data: no record, every gate passes (the headless runs).
    {
        OiWorld w;
        oi_gate_world(w, false, nullptr);
        CHECK(w.ab->targeted_ability(one, kForceLightning, 2, -1, w.frame) == 1);
        w.tick();
        CHECK(!w.sys->alive(2) && w.ab->anim_type_at(1, w.frame) == -1 && w.ab->can_activate(1, kForceSight, false, false, w.frame));
    }
    // §28.9: HUNT cannot be switched on while movement is locked - by this system's share or by the host's (script lock).
    {
        bool script_lock = false;
        OiWorld w;
        oi_gate_world(w, false, &script_lock);
        CHECK(w.ab->lock_movement(1, true, w.frame) && w.ab->immobilize_count(1) == 1);
        w.ab->set_unit_ability_mode(one, kHunt, true, w.frame);
        CHECK(!w.ab->is_active(1, kHunt));
        CHECK(w.ab->lock_movement(1, false, w.frame) && w.ab->immobilize_count(1) == 0);
        script_lock = true;
        w.ab->set_unit_ability_mode(one, kHunt, true, w.frame);
        CHECK(!w.ab->is_active(1, kHunt));
        script_lock = false;
        CHECK(w.ab->set_unit_ability_mode(one, kHunt, true, w.frame) == 1 && w.ab->is_active(1, kHunt));
    }
    // Save v2: the animation record round-trips (write -> read -> write, same hash, same view).
    {
        OiWorld a, b;
        oi_gate_world(a, true, nullptr);
        oi_gate_world(b, true, nullptr);
        CHECK(a.ab->targeted_ability(one, kForceLightning, 2, -1, a.frame) == 1);
        for (int i = 0; i < 5; ++i) a.tick();
        const SavedBattle saved = save_battle(*a.sys, *a.ab, a.rng);
        CHECK(load_battle(*b.sys, *b.ab, b.rng, saved));
        b.frame = a.frame;
        const SavedBattle again = save_battle(*b.sys, *b.ab, b.rng);
        CHECK(again.abilities == saved.abilities);
        CHECK(battle_digest(*a.sys, *a.ab, a.rng) == battle_digest(*b.sys, *b.ab, b.rng));
        CHECK(b.ab->anim_type_at(1, b.frame) == 0x28 && b.ab->anim_type_at(1, b.frame + 30) == -1);
    }
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

// ================================================================================================
// FoC data (ALAMO_FOC_DATA)
// ================================================================================================

void walk(const data::XmlNode& n, const std::function<void(const data::XmlNode&)>& fn) {
    for (data::XmlNode c = n.first_child(); c; c = c.next_sibling()) {
        fn(c);
        walk(c, fn);
    }
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

void test_hero_clash_accepts() {
    // s78n t06 (HERO_CLASH §5.2): the accepts that read +0x3b4 apply the pairing rule - a unit in a hero clash can only be
    // acted on by, and only act on, its clash partner. Hack (0x6fd610) and TractorBeam (0x710540).
    constexpr uint64_t kCatInf = 16;  // Infantry (as test_bribe_hack_accept)
    auto hack = [](uint32_t user_flags, uint32_t target_flags, bool paired) {
        OiWorld w;
        SpecialAbilityDesc hk = oi_special("Hack", SpecialClass::Hack, ActivationStyle::UserInput);
        hk.applicable_categories = kCatInf;
        hk.max_range = 1000.0f;
        const uint32_t hs = add_special(w.ad, hk);
        AbilityObjectDesc a;
        a.unit_abilities = {oi_gui(kTargetedHack, 0.0f, 10.0f, "Hack")};
        a.specials = {hs};
        const uint32_t ad = push(w.cd, w.ad, oi_unit("R2", 100.0f), a);
        const uint32_t td = push(w.cd, w.ad, oi_unit("Turret", 100.0f, 5.0f, kCatInf), AbilityObjectDesc{});
        w.make(GameMode::Land);
        w.add(1, ad, 0, {0, 0, 0});
        w.add(2, td, 1, {300, 0, 0});
        if (user_flags) w.sys->set_flag(1, user_flags, true);
        if (target_flags) w.sys->set_flag(2, target_flags, true);
        if (paired) w.sys->set_hero_clashes({{1, 2}});
        const ObjectId one[] = {1};
        return w.ab->targeted_ability(one, kTargetedHack, 2, -1, w.frame);
    };
    CHECK(hack(0, 0, false) == 1);
    CHECK(hack(0, kUnitHeroClash, false) == 0);  // the target is in a clash
    CHECK(hack(kUnitHeroClash, 0, false) == 0);  // the user is in a clash
    CHECK(hack(kUnitHeroClash, kUnitHeroClash, false) == 0);  // both flagged, not the same clash
    CHECK(hack(kUnitHeroClash, kUnitHeroClash, true) == 1);   // clash partners
    {  // Tractor beam on a target in a clash: refused
        SpecialWorld w;
        w.add(1, w.sd, 0, {0, 0, 0});
        w.add(2, w.corvette, 1, {500, 0, 0});
        w.sys->set_flag(2, kUnitHeroClash, true);
        const ObjectId sd[] = {1};
        CHECK(w.ab->targeted_ability(sd, kTractorBeam, 2, -1, w.frame) == 0);
        w.sys->set_flag(2, kUnitHeroClash, false);
        CHECK(w.ab->targeted_ability(sd, kTractorBeam, 2, -1, w.frame) == 1);
    }
}
}  // namespace bo3split

// Part 3 of test_combat (bo3 t03: split by position; see test_combat_common.hpp).
#include "test_combat_common.hpp"

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

void test_convert_victim_test() {
    // s78n t05 (TACTICAL_COMBAT §3.5 step 2, HERO_CLASH §7.3: 0x395c10 in order): a convert projectile skips a vehicle taken
    // over by a vehicle thief, a PROXIMITY_MINE type, Friendly_Damage_Amount > 0, a named hero and a build pad holding a
    // live UC / finished object (host hook); the plain victim and an incognito one (not part of the test) convert.
    struct PadHooks final : CombatHooks {
        std::vector<ObjectId> converted;
        void unit_effect(ObjectId id, UnitEffect e, int32_t, Vec3, uint32_t) override {
            if (e == UnitEffect::Converted) converted.push_back(id);
        }
        bool occupied_build_pad(ObjectId id) const override { return id == 15; }
    };
    World w;
    PadHooks hk;
    ProjectileDesc cv;
    cv.name = "Proj_Convert";
    cv.max_speed = 10.0f;
    cv.convert = true;
    cv.convert_radius = 100.0f;
    w.data.projectiles.push_back(cv);
    UnitDesc d;
    d.tactical_health = 100.0f;
    const uint32_t plain = push_unit(w.data, d);
    UnitDesc hero = d;
    hero.named_hero = true;
    const uint32_t hd = push_unit(w.data, hero);
    UnitDesc fd = d;
    fd.friendly_damage_amount = 5.0f;
    const uint32_t fdd = push_unit(w.data, fd);
    UnitDesc mine = d;
    mine.proximity_mine = true;
    const uint32_t md = push_unit(w.data, mine);
    CombatConfig cfg;
    cfg.mode = GameMode::Land;
    w.make(cfg);
    w.sys->set_hooks(&hk);
    w.add(10, plain, 1, Vec3{10, 0, 0});
    w.add(11, hd, 1, Vec3{20, 0, 0});
    w.add(12, fdd, 1, Vec3{30, 0, 0});
    w.add(13, md, 1, Vec3{40, 0, 0});
    w.add(14, plain, 1, Vec3{0, 10, 0});
    w.add(15, plain, 1, Vec3{0, 20, 0});  // the occupied build pad
    w.add(16, plain, 1, Vec3{0, 30, 0});
    w.sys->set_flag(14, kUnitStolenVehicle, true);
    w.sys->set_flag(16, kUnitIncognito, true);
    ProjectileLaunch l;
    l.owner = 0;
    l.projectile = 0;
    l.from = Vec3{0, 0, 0};
    l.point = l.from;
    l.fuse_seconds = 0.0f;
    CHECK(w.sys->launch_projectile(l, w.frame));
    w.tick();
    w.tick();
    CHECK(w.sys->owner(10) == 0 && (w.sys->flags(10) & kUnitConverted));
    CHECK(w.sys->owner(16) == 0);
    for (const ObjectId id : {ObjectId{11}, ObjectId{12}, ObjectId{13}, ObjectId{14}, ObjectId{15}})
        CHECK(w.sys->owner(id) == 1 && !(w.sys->flags(id) & kUnitConverted));
    CHECK(hk.converted.size() == 2);
    CHECK((w.sys->flags(14) & kUnitStolenVehicle) != 0);  // permanent
}
}  // namespace bo3split

namespace bo3split {

// s78n review: (a) remove_from_world - the world removal 0x391310 without Die (the vehicle a thief's clone replaces): dead,
// one DeathEvent of kind 15, no hardpoint event, no death projectile, no DeathBehavior draw; (b) a self hit (STIM_PACK's
// 0x3a9e30(V, 1, x, V)) runs Take_Damage with the unit as its own source - its incognito bit goes (no CAUSE_DAMAGE), its
// own Damage list scales it - but keeps no kill credit and no damage-record source [port]; (c) s78n t13: the last-hit
// stamp is written at step 11, before the damage-enabled gate and before the listeners.
void test_s78n_review_combat() {
    const auto units_digest = [](const CombatSystem& s) {
        StateHasher h;
        s.hash_units(h);
        return h.digest();
    };
    {  // (a)
        World w;
        ProjectileDesc dp;
        dp.name = "Death_A";
        dp.max_speed = 5.0f;
        w.data.projectiles = {dp, dp};
        HardpointDesc hd;
        hd.kind = HardpointKind::Engine;
        hd.health = 50.0f;
        hd.destroyable = true;
        w.data.hardpoints.push_back(hd);
        UnitDesc car;
        car.tactical_health = 500.0f;
        car.hardpoints = {0, 0};
        car.death_projectiles = {0, 1};  // two entries: Die draws the pick
        car.spin_away_on_death = true;   // Die draws the spin-away
        const uint32_t cd = push_unit(w.data, car);
        w.make();
        w.sys->bind_rng(&w.rng);
        w.add(1, cd, 0, Vec3{0, 0, 0});
        w.add(2, cd, 0, Vec3{200, 0, 0});
        w.add(3, cd, 1, Vec3{400, 0, 0});
        w.tick();  // clears the per-tick lists
        const uint64_t d0 = w.rng.draws();
        w.sys->remove_from_world(1, 3, w.frame);
        CHECK(w.sys->has_unit(1) && !w.sys->alive(1));
        CHECK(w.sys->deaths().size() == 1 && w.sys->hardpoint_events().empty() && w.sys->projectile_count() == 0u);
        if (w.sys->deaths().size() == 1) {
            const DeathEvent& d = w.sys->deaths()[0];
            CHECK(d.id == 1 && d.killer == 3 && d.kind == kKindNoDeathClone && d.death_clone == -1 && d.death_projectile == -1 &&
                  !d.spin_away);
        }
        CHECK(w.rng.draws() == d0);  // no death-projectile pick, no spin-away draw
        w.sys->remove_from_world(1, 3, w.frame);  // already dead: nothing more
        CHECK(w.sys->deaths().size() == 1);
        w.sys->kill_unit(2, 3, kKindNormal, w.frame);  // the same unit through Die: its hardpoints die, the draws are made
        CHECK(w.sys->deaths().size() == 2 && w.sys->hardpoint_events().size() == 2 && w.sys->projectile_count() == 1u);
        CHECK(w.rng.draws() == d0 + 2);
    }
    {  // (b)
        World w;
        UnitDesc u;
        u.tactical_health = 1000.0f;
        const uint32_t ud = push_unit(w.data, u);
        w.make();
        w.add(1, ud, 0, Vec3{0, 0, 0});
        w.add(2, ud, 1, Vec3{200, 0, 0});
        w.add(3, ud, 0, Vec3{400, 0, 0});
        if (ModifierSet* m = w.sys->unit_modifiers(1)) m->insert(ModList::Damage, 1, 0.5f, kPermanent);
        w.tick();  // the modifier caches refresh in the tick
        DamageRequest r;
        r.victim = 1;
        r.amount = 100.0f;
        CHECK_F(w.sys->take_damage(r, w.frame).after_multipliers, 100.0f);  // sourceless: the Damage list is not read
        w.sys->set_flag(1, kUnitIncognito, true);
        r.source = 1;
        CHECK_F(w.sys->take_damage(r, w.frame).after_multipliers, 150.0f);  // its own Damage list: x (1 + 0.5)
        CHECK(!(w.sys->flags(1) & kUnitIncognito));
        CHECK(!w.sys->damage_events().empty());
        if (!w.sys->damage_events().empty()) {
            const DamageEvent& e = w.sys->damage_events().back();
            CHECK(e.victim == 1 && e.source == kNoObject && e.source_owner == -1);  // [port] recorded without a source
        }
        r.amount = 100000.0f;  // the self hit kills: no killer
        CHECK(w.sys->take_damage(r, w.frame).killed && !w.sys->alive(1));
        CHECK(w.sys->deaths().size() == 1 && w.sys->deaths()[0].id == 1 && w.sys->deaths()[0].killer == kNoObject);
        r.victim = 3;  // another unit's hit keeps its credit and its source
        r.source = 2;
        CHECK(w.sys->take_damage(r, w.frame).killed);
        CHECK(w.sys->deaths().size() == 2 && w.sys->deaths()[1].killer == 2);
        CHECK(w.sys->damage_events().back().source == 2 && w.sys->damage_events().back().source_owner == 1);
    }
    // s78n t19: a self hit (STIM_PACK shape) redirected to / shared with teammates stays the user's own damage - the
    // teammates it kills have no killer and their damage events name no source; another unit's hit on the same team keeps
    // its credit and its source.
    for (const bool share : {false, true}) {
        World w;
        UnitDesc u;
        u.tactical_health = 1000.0f;
        const uint32_t plain = push_unit(w.data, u);
        u.redirect_damage_to_teammates = !share;
        u.share_damage_with_teammates = share;
        const uint32_t team_rule = push_unit(w.data, u);
        w.make();
        const auto add = [&](ObjectId id, uint32_t desc, int32_t owner, uint32_t damage_team) {
            UnitSpawn sp;
            sp.id = id;
            sp.desc = desc;
            sp.owner = owner;
            sp.position = Vec3{static_cast<float>(id) * 50.0f, 0, 0};
            sp.damage_team = damage_team;
            CHECK(w.sys->add_unit(sp, w.frame, w.rng));
        };
        add(10, team_rule, 0, 9);  // the user
        add(11, plain, 0, 9);
        add(12, plain, 0, 9);
        add(20, team_rule, 0, 8);  // a second team, hit by an enemy
        add(21, plain, 0, 8);
        add(30, plain, 1, 0);      // the enemy
        DamageRequest r;
        r.victim = 10;
        r.source = 10;
        r.amount = 100000.0f;
        const size_t ev0 = w.sys->damage_events().size();
        w.sys->take_damage(r, w.frame);
        CHECK(!w.sys->alive(11) && !w.sys->alive(12) && w.sys->alive(10) == !share);  // redirect: the user takes none
        for (const DeathEvent& d : w.sys->deaths()) CHECK(d.killer == kNoObject);
        CHECK(w.sys->deaths().size() == (share ? 3u : 2u));
        CHECK(w.sys->damage_events().size() > ev0);
        for (size_t i = ev0; i < w.sys->damage_events().size(); ++i)
            CHECK(w.sys->damage_events()[i].source == kNoObject && w.sys->damage_events()[i].source_owner == -1);
        const size_t d0 = w.sys->deaths().size(), ev1 = w.sys->damage_events().size();
        r.victim = 20;
        r.source = 30;
        w.sys->take_damage(r, w.frame);
        CHECK(!w.sys->alive(21) && w.sys->deaths().size() > d0);
        for (size_t i = d0; i < w.sys->deaths().size(); ++i) CHECK(w.sys->deaths()[i].killer == 30);
        for (size_t i = ev1; i < w.sys->damage_events().size(); ++i)
            CHECK(w.sys->damage_events()[i].source == 30 && w.sys->damage_events()[i].source_owner == 1);
    }
    {  // (c) damage disabled: nothing is applied, but a projectile hit from a live shooter has stamped the victim
        World w;
        UnitDesc u;
        u.tactical_health = 1000.0f;
        const uint32_t ud = push_unit(w.data, u);
        CombatConfig cfg;
        cfg.damage_enabled = false;
        w.make(cfg);
        w.add(1, ud, 0, Vec3{0, 0, 0});
        w.add(2, ud, 1, Vec3{200, 0, 0});
        const uint64_t h0 = units_digest(*w.sys);
        DamageRequest r;
        r.victim = 1;
        r.amount = 10.0f;
        CHECK(!w.sys->take_damage(r, 500).applied);  // sourceless: no stamp
        r.source = 2;
        CHECK(!w.sys->take_damage(r, 500).applied);  // a plain non-projectile source: no stamp
        CHECK(units_digest(*w.sys) == h0);
        r.source_is_projectile = true;
        const DamageResult out = w.sys->take_damage(r, 500);
        CHECK(!out.applied && w.sys->health(1) == w.sys->max_health(1));
        CHECK(units_digest(*w.sys) != h0);  // ever_hit / last_hit_frame, written before the gate
        const uint64_t h1 = units_digest(*w.sys);
        (void)w.sys->take_damage(r, 500);  // the same frame again: the same stamp
        CHECK(units_digest(*w.sys) == h1);
        (void)w.sys->take_damage(r, 501);
        CHECK(units_digest(*w.sys) != h1);
    }
    {  // (c) a listener cancels the hit: no damage, but the stamp stands - the next hit at that frame is diminished
        struct CancelHooks final : CombatHooks {
            bool cancel = true;
            bool damage_listener(ObjectId, ObjectId, int32_t, uint32_t, SimRng*) override { return cancel; }
        } hk;
        World w;
        UnitDesc u;
        u.tactical_health = 1e6f;
        const uint32_t ud = push_unit(w.data, u);
        CombatConfig cfg;
        cfg.diminishing_firepower = {{0.0f, 0.6f}, {1.0f, 1.0f}};
        w.make(cfg);
        w.sys->set_hooks(&hk);
        w.add(1, ud, 0, Vec3{0, 0, 0});
        w.add(2, ud, 1, Vec3{200, 0, 0});
        DamageRequest r;
        r.victim = 1;
        r.amount = 100.0f;
        r.source = 2;
        r.source_is_projectile = true;
        const DamageResult first = w.sys->take_damage(r, 900);
        CHECK(first.cancelled && first.after_multipliers == 0.0f && w.sys->health(1) == w.sys->max_health(1));
        hk.cancel = false;
        CHECK_NEAR(w.sys->take_damage(r, 900).after_multipliers, 60.0, 1e-3);  // 0 s after the cancelled hit's stamp
        w.sys->set_hooks(nullptr);
    }
}
}  // namespace bo3split

namespace bo3split {

// s78n t20 batch 2 (MOVEMENT §5.3 "Hover in full"): a melee attacker (Targeting_Max_Attack_Distance 40 <= Melee_Cutoff_Range
// 51) neither acquires (Scan_Player), keeps (Validate_Current_Target: the auto target at the next scan, an ordered one at
// once) nor is ordered onto (Order_Attack C step 1) a target flagged kUnitHoverRaised; a ranged attacker (300) is not
// affected; a grounded hover unit (flag clear) is an ordinary target.
void test_hover_raised_melee() {
    const auto world = [](World& w, float reach) {
        w.data.projectiles.push_back(ProjectileDesc{});
        w.data.projectiles[0].max_speed = 10.0f;
        UnitDesc a;
        a.tactical_health = 100.0f;
        a.max_attack_distance = reach;
        a.weapon_projectile = 0;
        a.weapon_recharge = 100.0f;  // never fires during the test
        push_unit(w.data, a);
        UnitDesc t;
        t.tactical_health = 100.0f;
        t.radius = 10.0f;
        t.category_mask = kCatCapital;
        push_unit(w.data, t);
        CombatConfig cfg;
        cfg.short_range_cutoff = 51.0f;
        w.make(cfg);
        w.add(1, 0, 0, Vec3{0, 0, 0});
        w.add(2, 1, 1, Vec3{30, 0, 0});
    };
    const auto ticks = [](World& w, int n) {
        for (int i = 0; i < n; ++i) w.tick();
    };
    {  // melee: no acquisition while raised; acquired once grounded; dropped at the next scan when it rises again
        World w;
        world(w, 40.0f);
        CHECK(w.sys->melee_attacker(1));
        w.sys->set_flag(2, kUnitHoverRaised, true);
        ticks(w, 60);
        CHECK(w.sys->auto_target(1) == kNoObject);
        CHECK(!w.sys->order_attack_issue(1, 2));  // refused
        w.sys->set_flag(2, kUnitHoverRaised, false);
        ticks(w, 60);
        CHECK(w.sys->auto_target(1) == 2);
        w.sys->set_flag(2, kUnitHoverRaised, true);
        ticks(w, 60);
        CHECK(w.sys->auto_target(1) == kNoObject);
    }
    {  // melee: an ordered target is dropped on the tick it rises
        World w;
        world(w, 40.0f);
        w.sys->order_attack(1, 2);
        w.tick();
        CHECK(w.sys->current_target(1) == 2);
        w.sys->set_flag(2, kUnitHoverRaised, true);
        w.tick();
        CHECK(w.sys->current_target(1) != 2);
    }
    {  // ranged: unaffected
        World w;
        world(w, 300.0f);
        CHECK(!w.sys->melee_attacker(1));
        w.sys->set_flag(2, kUnitHoverRaised, true);
        ticks(w, 60);
        CHECK(w.sys->auto_target(1) == 2);
        CHECK(w.sys->order_attack_issue(1, 2));
    }
    std::printf("  hover raised: melee refuses / drops a raised target, ranged keeps it\n");
}
}  // namespace bo3split

namespace bo3split {

void test_incognito_targeting() {
    // s78n t05 (HERO_CLASH §8.2, +0x3a1 & 0x10 as the vehicle-thief boarding sets it): not auto-acquired, no auto-acquire
    // of its own (no scan, so no scan draws), an attack order still works; Take_Damage clears it.
    {  // an incognito target is not a candidate; once cleared it is acquired
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{300, 0, 0});
        t.w.sys->set_flag(2, kUnitIncognito, true);
        for (int i = 0; i < 60; ++i) t.w.tick();
        CHECK(t.w.sys->auto_target(1) == kNoObject);
        t.w.sys->set_flag(2, kUnitIncognito, false);
        for (int i = 0; i < 60 && t.w.sys->auto_target(1) == kNoObject; ++i) t.w.tick();
        CHECK(t.w.sys->auto_target(1) == 2);
    }
    {  // an incognito attacker does not scan (no draws) but keeps an ordered target
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{300, 0, 0});
        t.w.sys->set_flag(1, kUnitIncognito, true);
        const uint64_t d0 = t.w.rng.draws();
        for (int i = 0; i < 60; ++i) t.w.tick();
        CHECK(t.w.sys->auto_target(1) == kNoObject && t.w.rng.draws() == d0);
        t.w.sys->order_attack(1, 2);
        t.w.tick();
        CHECK(t.w.sys->current_target(1) == 2);
    }
    // s78n t12 (HERO_CLASH §8.4.1, Take_Damage 0x3a9e30 in retail order): the gates (+0x37d invulnerable, Immune_To_Damage,
    // garrisoned victim) stop the hit before any clear; then the victim's clear (a non-projectile source, or a projectile
    // aimed at it), TAKE_DAMAGE, the source's clear (a projectile's shooter: always with CAUSE_DAMAGE; a non-projectile
    // source only with the bit), and only then the damage-enabled gate.
    const auto inc = [](const TargetWorld& t, ObjectId id) { return (t.w.sys->flags(id) & kUnitIncognito) != 0; };
    {  // who is cleared
        TargetWorld t;
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{300, 0, 0});
        t.w.add(3, t.capital, 1, Vec3{-300, 0, 0});
        DamageRequest r;
        r.victim = 2;
        r.amount = 1.0f;
        t.w.sys->set_flag(2, kUnitIncognito, true);
        CHECK(t.w.sys->take_damage(r, t.w.frame).applied);  // no source: nothing is cleared
        CHECK(inc(t, 2));
        t.w.sys->set_flag(1, kUnitIncognito, true);
        r.source = 1;  // an armed non-projectile source that is not targeting 2: no weapon / target test, both cleared
        t.w.sys->take_damage(r, t.w.frame);
        CHECK(!inc(t, 2) && !inc(t, 1));
        t.w.sys->set_flag(2, kUnitIncognito, true);
        t.w.sys->set_flag(1, kUnitIncognito, true);
        r.source_is_projectile = true;  // a projectile aimed at 3 that hits 2 (splash / stray): 2 keeps it, the shooter loses it
        r.projectile_target = 3;
        t.w.sys->take_damage(r, t.w.frame);
        CHECK(inc(t, 2) && !inc(t, 1));
        r.projectile_target = kNoObject;  // a projectile without a target clears no victim
        t.w.sys->take_damage(r, t.w.frame);
        CHECK(inc(t, 2));
        r.source = 77;  // the projectile's own target loses it even when the shooter is gone
        r.projectile_target = 2;
        t.w.sys->take_damage(r, t.w.frame);
        CHECK(!inc(t, 2));
        // a dead victim that is still in the table: the clears still run (no live test in front of them)
        t.w.sys->kill_unit(3, kNoObject, kKindNormal, t.w.frame);
        CHECK(t.w.sys->has_unit(3) && !t.w.sys->alive(3));
        t.w.sys->set_flag(1, kUnitIncognito, true);
        r.victim = 3;
        r.source = 1;
        r.projectile_target = 3;
        const DamageResult dead = t.w.sys->take_damage(r, t.w.frame);
        CHECK(!dead.applied && !inc(t, 1));
        t.w.sys->set_flag(1, kUnitIncognito, true);  // non-projectile, incognito source on the dead victim
        r.source_is_projectile = false;
        r.projectile_target = kNoObject;
        t.w.sys->take_damage(r, t.w.frame);
        CHECK(!inc(t, 1));
    }
    {  // the gates in front of the clears: nothing cleared, nothing applied, no damage
        TargetWorld t;
        UnitDesc im = t.w.data.units[t.capital];
        im.immune_to_damage = true;
        const uint32_t imd = push_unit(t.w.data, im);
        t.w.make();
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{300, 0, 0});
        t.w.add(3, imd, 1, Vec3{-300, 0, 0});
        t.w.add(4, t.capital, 1, Vec3{0, 300, 0});
        t.w.sys->set_flag(2, kUnitInvulnerable, true);
        t.w.sys->set_flag(4, kUnitGarrisoned, true);
        for (const ObjectId v : {ObjectId{2}, ObjectId{3}, ObjectId{4}}) {
            for (const bool projectile : {false, true}) {
                t.w.sys->set_flag(v, kUnitIncognito, true);
                t.w.sys->set_flag(1, kUnitIncognito, true);
                DamageRequest r;
                r.victim = v;
                r.amount = 5.0f;
                r.source = 1;
                r.source_is_projectile = projectile;
                r.projectile_target = v;
                const float h0 = t.w.sys->health(v);
                const DamageResult out = t.w.sys->take_damage(r, t.w.frame);
                CHECK(!out.applied && inc(t, v) && inc(t, 1));
                CHECK_F(t.w.sys->health(v), h0);
            }
        }
    }
    {  // damage disabled: the gate sits after the clears - both bits go, nothing is applied
        TargetWorld t;
        CombatConfig cfg;
        cfg.damage_enabled = false;
        t.w.make(cfg);
        t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
        t.w.add(2, t.capital, 1, Vec3{300, 0, 0});
        t.w.sys->set_flag(1, kUnitIncognito, true);
        t.w.sys->set_flag(2, kUnitIncognito, true);
        DamageRequest r;
        r.victim = 2;
        r.amount = 5.0f;
        r.source = 1;
        const float h0 = t.w.sys->health(2);
        const DamageResult out = t.w.sys->take_damage(r, t.w.frame);
        CHECK(!out.applied && !inc(t, 1) && !inc(t, 2));
        CHECK_F(t.w.sys->health(2), h0);
    }
    {  // CAUSE_DAMAGE x 2 on the source: a non-projectile source gets it only while it has the bit (first hit 200, then
       // 100); a self hit clears the victim first, so no CAUSE_DAMAGE; a projectile's shooter gets it with or without the bit
        TargetWorld t;
        UnitAbilityDesc cause;
        cause.ability_type = kAbilityPowerToWeapons;
        cause.mult[static_cast<size_t>(ModKind::CauseDamage) - 1] = 2.0f;
        t.w.data.abilities = {cause};
        UnitDesc src = t.w.data.units[t.capital];
        src.tactical_health = 10000.0f;
        src.abilities = {0, -1};
        const uint32_t sd = push_unit(t.w.data, src);
        t.w.make();
        t.w.add(1, sd, 0, Vec3{0, 0, 0});
        t.w.add(2, sd, 1, Vec3{300, 0, 0});
        t.w.sys->set_ability_active(1, 0, true);
        DamageRequest r;
        r.victim = 2;
        r.amount = 100.0f;
        r.source = 1;
        t.w.sys->set_flag(1, kUnitIncognito, true);
        CHECK_F(t.w.sys->take_damage(r, t.w.frame).after_multipliers, 200.0f);
        CHECK(!inc(t, 1));
        CHECK_F(t.w.sys->take_damage(r, t.w.frame).after_multipliers, 100.0f);
        r.victim = 1;  // self hit (STIM_PACK shape): step 8 clears the victim, step 10 then finds no bit
        t.w.sys->set_flag(1, kUnitIncognito, true);
        CHECK_F(t.w.sys->take_damage(r, t.w.frame).after_multipliers, 100.0f);
        CHECK(!inc(t, 1));
        r.victim = 2;  // a projectile: the shooter's CAUSE_DAMAGE always
        r.source_is_projectile = true;
        CHECK_F(t.w.sys->take_damage(r, t.w.frame).after_multipliers, 200.0f);
    }
    {  // the delayed-damage queue keeps the projectile's target (combat save 15): a bomb aimed at 2 goes off between 2 and
       // 3, both 80 from it (area delay 0.4 s). Saved while the two hits wait; in the saved and in the loaded system the
       // target 2 loses its bit and the splash victim 3 keeps it.
        TargetWorld a;
        ProjectileDesc bomb;
        bomb.name = "Bomb";
        bomb.blast_area_damage = 10.0f;
        bomb.blast_area_range = 100.0f;
        a.w.data.projectiles.push_back(bomb);
        const uint32_t bi = static_cast<uint32_t>(a.w.data.projectiles.size() - 1);
        CombatConfig cfg;
        cfg.max_secs_for_ae_delayed_damage = 0.5f;
        a.w.make(cfg);
        a.w.add(1, a.capital, 0, Vec3{0, -900, 0});
        a.w.add(2, a.capital, 1, Vec3{80, 0, 0});
        a.w.add(3, a.capital, 1, Vec3{-80, 0, 0});
        a.w.sys->set_flag(2, kUnitIncognito, true);
        a.w.sys->set_flag(3, kUnitIncognito, true);
        ProjectileLaunch l;
        l.shooter = 1;
        l.owner = 0;
        l.projectile = bi;
        l.from = Vec3{0, 0, 0};
        l.target = 2;
        l.fuse_seconds = 0.0f;
        CHECK(a.w.sys->launch_projectile(l, a.w.frame));
        for (int i = 0; i < 6 && a.w.sys->projectile_count() > 0; ++i) a.w.tick();
        CHECK(a.w.sys->projectile_count() == 0u);
        CHECK(a.w.sys->health(2) == a.w.sys->max_health(2) && a.w.sys->health(3) == a.w.sys->max_health(3));  // queued
        CHECK(inc(a, 2) && inc(a, 3));
        const std::vector<uint8_t> saved = combat_bytes(*a.w.sys);
        TargetWorld b;
        b.w.data = a.w.data;
        b.w.make(cfg);
        ByteReader rd(saved);
        CHECK(b.w.sys->read(rd));
        CHECK(combat_bytes(*b.w.sys) == saved);
        b.w.frame = a.w.frame;
        for (int i = 0; i < 20; ++i) {
            a.w.tick();
            b.w.tick();
        }
        for (const TargetWorld* w : {&a, &b}) {
            CHECK(w->w.sys->health(2) < w->w.sys->max_health(2) && w->w.sys->health(3) < w->w.sys->max_health(3));
            CHECK(!inc(*w, 2) && inc(*w, 3));
        }
    }
    {  // save / load: both flags ride the saved flag word (no format change) and survive an owner change
        TargetWorld a;
        a.w.add(1, a.attacker, 0, Vec3{0, 0, 0});
        a.w.add(2, a.capital, 1, Vec3{300, 0, 0});
        a.w.sys->set_flag(2, kUnitIncognito | kUnitStolenVehicle, true);
        a.w.sys->set_owner(2, 0);
        ByteWriter wa;
        a.w.sys->write(wa);
        TargetWorld b;
        ByteReader r(wa.bytes());
        CHECK(b.w.sys->read(r));
        CHECK((b.w.sys->flags(2) & (kUnitIncognito | kUnitStolenVehicle)) == (kUnitIncognito | kUnitStolenVehicle));
        CHECK(b.w.sys->owner(2) == 0);
        StateHasher ha, hb, hc;
        a.w.sys->hash_units(ha);
        b.w.sys->hash_units(hb);
        a.w.sys->set_flag(2, kUnitIncognito, false);
        a.w.sys->hash_units(hc);
        CHECK(ha.digest() == hb.digest() && ha.digest() != hc.digest());  // hashed
    }
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

void test_team_head_alone() {
    // s78i t03 (TACTICAL_COMBAT §3.1.1 "Team targeting copies"): TeamTargeting belongs to the container and runs while it
    // lives, whatever its member count. A land team whose stand-in head is its last live unit (or a team of one) keeps the
    // team service. Observable: the team service has no TURRET branch, so a turret head whose turret is still slewing
    // (host aim 0) is in state 4 there, where its own TARGETING (Service_Land) gives 3.
    struct TurretTeam {
        World w;
        StateHooks h;
        explicit TurretTeam(bool with_member) {
            ProjectileDesc p;
            p.max_speed = 20.0f;
            w.data.projectiles.push_back(p);
            UnitDesc s;
            s.tactical_health = 100.0f;
            s.max_attack_distance = 300.0f;
            s.weapon_projectile = 0;
            s.weapon_recharge = 100.0f;
            s.turn_rate = 10.0f;
            s.turret = true;
            push_unit(w.data, s);
            UnitDesc t;
            t.tactical_health = 1.0e7f;
            t.radius = 5.0f;
            push_unit(w.data, t);
            CombatConfig cfg;
            cfg.mode = GameMode::Land;
            cfg.inaccuracy_enabled = false;
            w.make(cfg);
            h.aim = 0;  // the turret is still slewing
            w.sys->set_hooks(&h);
            w.add(2, 1, 1, Vec3{200, 5, 0});
            const auto add = [&](ObjectId id, uint32_t team, bool leader) {
                UnitSpawn sp;
                sp.id = id;
                sp.desc = 0;
                sp.owner = 0;
                sp.position = Vec3{0, static_cast<float>(id - 10) * 10.0f, 0};
                sp.facing = Vec3{1, 0, 0};
                sp.team = team;
                sp.team_leader = leader;
                CHECK(w.sys->add_unit(sp, w.frame, w.rng));
                TreeMembership m;
                m.owner = 0;
                CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(sp.position, 1.0f), 1u, m));
            };
            add(10, 7, true);
            if (with_member) add(11, 7, false);
            add(20, 0, false);  // control: no team, its own TARGETING
        }
    };
    {  // the member dies: the head alone keeps the team service (state 4 against the control's 3), tick after tick
        TurretTeam t(true);
        t.w.tick();
        CHECK(t.w.sys->targeting_state(10) == 4 && t.w.sys->targeting_state(11) == 4);
        CHECK(t.w.sys->targeting_state(20) == 3);
        t.w.sys->kill_unit(11, kNoObject, kKindNormal, t.w.frame);
        for (int i = 0; i < 5; ++i) {
            t.w.tick();
            CHECK(!t.w.sys->alive(11));
            CHECK(t.w.sys->auto_target(10) == 2 && t.w.sys->targeting_state(10) == 4);
            CHECK(t.w.sys->targeting_state(20) == 3);
        }
    }
    {  // a team of one from the start: the same
        TurretTeam t(false);
        for (int i = 0; i < 3; ++i) {
            t.w.tick();
            CHECK(t.w.sys->auto_target(10) == 2 && t.w.sys->targeting_state(10) == 4);
            CHECK(t.w.sys->targeting_state(20) == 3);
        }
    }
    {  // the member is the last one alive after the head dies: the hand-over gives it the head and the team service
        TurretTeam t(true);
        t.w.tick();
        t.w.sys->kill_unit(10, kNoObject, kKindNormal, t.w.frame);
        for (int i = 0; i < 3; ++i) {
            t.w.tick();
            CHECK(t.w.sys->auto_target(11) == 2 && t.w.sys->targeting_state(11) == 4);
        }
    }
}
}  // namespace bo3split

namespace bo3split {

void test_convert_container_unit() {
    // s78i t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" steps 3-5): a team container that is itself a combat unit
    // (damage_team 0; members carry its id) converts with its members: Change_Owner covers the container and every
    // member. Events: the victim, then the container, then the other members (ascending id); each object once.
    struct ConvertHooks final : CombatHooks {
        std::vector<ObjectId> converted;
        void unit_effect(ObjectId id, UnitEffect e, int32_t, Vec3, uint32_t) override {
            if (e == UnitEffect::Converted) converted.push_back(id);
        }
    };
    for (const GameMode mode : {GameMode::Land, GameMode::Space}) {
        for (const bool hit_container : {false, true}) {
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
            auto add = [&](ObjectId id, Vec3 at, uint32_t team) {
                UnitSpawn sp;
                sp.id = id;
                sp.desc = ud;
                sp.owner = 1;
                sp.position = at;
                if (mode == GameMode::Land) {
                    sp.team = team;
                } else {
                    sp.damage_team = team;
                }
                CHECK(w.sys->add_unit(sp, w.frame, w.rng));
                TreeMembership m;
                m.owner = 1;
                CHECK(w.trees.add(static_cast<EntityHandle>(id), box_of(at, 1.0f), 1u, m));
            };
            const Vec3 near{10, 0, 0}, far{900, 0, 0};
            add(10, hit_container ? far : near, 50);  // members of container 50
            add(11, Vec3{950, 0, 0}, 50);
            add(50, hit_container ? near : Vec3{920, 0, 0}, 0);  // the container: a combat unit, damage_team 0
            add(60, Vec3{930, 0, 0}, 0);                         // unrelated, out of the radius
            ProjectileLaunch l;
            l.owner = 0;
            l.projectile = 0;
            l.from = Vec3{0, 0, 0};
            l.point = l.from;
            l.fuse_seconds = 0.0f;
            CHECK(w.sys->launch_projectile(l, w.frame));
            w.tick();
            w.tick();
            for (const ObjectId id : {ObjectId{10}, ObjectId{11}, ObjectId{50}})
                CHECK(w.sys->owner(id) == 0 && (w.sys->flags(id) & kUnitConverted));
            CHECK(w.sys->owner(60) == 1 && !(w.sys->flags(60) & kUnitConverted));
            const std::vector<ObjectId> want = hit_container ? std::vector<ObjectId>{50, 10, 11} : std::vector<ObjectId>{10, 50, 11};
            CHECK(hk.converted == want);
        }
    }
}
}  // namespace bo3split

namespace bo3split {

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
}  // namespace bo3split

namespace bo3split {

HeroClashData clash_data() {
    HeroClashData d;
    CHECK(parse_hero_clash_xml(clash_bytes(kClashXml), d));
    return d;
}
}  // namespace bo3split

namespace bo3split {

void test_hero_clash_parse_and_rules() {
    std::vector<std::string> diag;
    HeroClashData d;
    CHECK(parse_hero_clash_xml(clash_bytes(kClashXml), d, &diag));
    CHECK(d.entries.size() == 3);  // duplicate names kept, file order
    if (d.entries.size() != 3) return;
    const HeroClashEntry& e = d.entries[0];
    CHECK(e.name == "Duel_A_B" && e.clash_type == kClashMelee && !e.play_conversation);
    CHECK_F(e.clash_range, 100.0f);
    CHECK_F(e.combat_distance, 17.65f);
    CHECK_F(e.damage_percentage, 0.15f);
    CHECK_F(e.first_win, 0.4f);
    CHECK_F(e.second_win, 0.3f);
    CHECK_F(e.first_multiplier, 2.5f);
    CHECK(e.first_type == "Hero_A" && e.second_type == "Hero_B");
    CHECK(e.involved.size() == 2 && e.involved.back() == "Hero_B");
    CHECK(e.first_anims[0] == "HC_WIN" && e.first_anims[1] == "HC_LOSE" && e.second_anims[2] == "HC_DRAW" && e.second_anims[3] == "IDLE");
    CHECK(e.first_speech == "Speech_A");
    CHECK(d.entries[1].clash_type == kClashActionDriven && d.entries[1].action_count == 2 && d.entries[1].name == "Action_B_C");
    CHECK_F(d.entries[1].first_win, 0.3f);  // 0.8 + 0.8 > 1: both 0.3, one diagnostic
    CHECK_F(d.entries[1].second_win, 0.3f);
    CHECK(diag.size() == 1);
    CHECK(d.entries[2].clash_type == -1 && d.entries[2].name == "Action_B_C");
    HeroClashData bad;
    CHECK(!parse_hero_clash_xml(clash_bytes("<Other/>"), bad));

    // Exchange damage (§3.6 step 4): round((500 x 0.15 + 0) x mult) - a first-type win 187.5 -> 188, second 75.
    CHECK(clash_exchange_damage(e, 500.0f, true, false) == 188);
    CHECK(clash_exchange_damage(e, 500.0f, false, true) == 75);
    CHECK(clash_exchange_damage(e, 500.0f, false, false) == 0);
    CHECK(clash_exchange_damage(e, 420.0f, true, false) == 158);  // 157.5 -> 158 (half away from zero)

    // Outcomes: one synced float draw each, never the previous outcome; first draw 0.4 / 0.3 / 0.3, after a first-hero win
    // 0.5 / 0.5 between the second hero's win and the draw.
    SimRng rng(777u);
    uint8_t prev = kExchangeNone;
    uint32_t first[4] = {0, 0, 0, 0}, after1[4] = {0, 0, 0, 0}, repeats = 0;
    for (int i = 0; i < 20000; ++i) {
        const uint64_t d0 = rng.draws();
        const uint8_t o = clash_pick_outcome(prev, 0.4f, 0.3f, rng);
        CHECK(rng.draws() == d0 + 1);
        if (o == prev || o < 1 || o > 3) ++repeats;
        if (prev == kExchangeFirstWins) ++after1[o & 3u];
        prev = o;
    }
    CHECK(repeats == 0);
    SimRng r2(4242u);
    for (int i = 0; i < 20000; ++i) ++first[clash_pick_outcome(kExchangeNone, 0.4f, 0.3f, r2) & 3u];
    CHECK_NEAR(first[1] / 20000.0, 0.4, 0.02);
    CHECK_NEAR(first[2] / 20000.0, 0.3, 0.02);
    CHECK_NEAR(first[3] / 20000.0, 0.3, 0.02);
    const double n1 = static_cast<double>(after1[2] + after1[3]);
    CHECK(after1[1] == 0 && n1 > 0.0);
    if (n1 > 0.0) CHECK_NEAR(after1[2] / n1, 0.5, 0.03);
}
}  // namespace bo3split

namespace bo3split {

void test_hero_clash_start_rule() {
    {  // detection only when frame % 15 == 0, no draws
        ClashRig r;
        r.frame = 16;
        for (int i = 0; i < 14; ++i) r.step();
        CHECK(r.m.empty() && r.rng.draws() == 0);
        CHECK(r.frame == 30);
        r.step();  // frame 30: starts
        CHECK(r.m.clashes().size() == 1 && r.host.get(1)->flag && r.host.get(2)->flag);
        CHECK(r.host.get(1)->moves == 1 && r.host.get(2)->moves == 1);
        // Spots on world X through the midpoint (25, 5), 17.65 apart; hero 1 is left of the midpoint.
        CHECK_F(r.host.get(1)->spot.x, 25.0f - 17.65f / 2.0f);
        CHECK_F(r.host.get(2)->spot.x, 25.0f + 17.65f / 2.0f);
        CHECK_F(r.host.get(1)->spot.y, 5.0f);
        CHECK(r.m.take_changed());
        CHECK(!r.m.take_changed());
        CHECK(r.m.pairs().size() == 1 && r.m.pairs()[0] == std::make_pair(ObjectId{1}, ObjectId{2}));
    }
    {  // out of range (3-D distance), then in range
        ClashRig r;
        r.host.get(2)->pos = Vec3{60.0f, 0.0f, 81.0f};  // 100.5 away
        r.step();
        CHECK(r.m.empty());
        r.host.get(2)->pos = Vec3{60.0f, 0.0f, 79.0f};  // 99.1
        r.frame = 30;
        r.step();
        CHECK(!r.m.empty());
    }
    for (int k = 0; k < 5; ++k) {  // same owner, allies, a non-hero type, a jet pack, already clashing: no start
        ClashRig r;
        if (k == 0) r.host.get(2)->owner = 0;
        if (k == 1) r.host.allies.emplace_back(0, 1);
        if (k == 2) r.host.get(2)->hero = false;
        if (k == 3) r.host.get(1)->jet = true;
        if (k == 4) r.host.get(2)->flag = true;
        r.step();
        CHECK(r.m.empty());
    }
    {  // the targeting condition: no target or one of the other hero type; another target blocks the start
        ClashRig r;
        r.host.add(3, kTypeX, 1, Vec3{200.0f, 0.0f, 0.0f});
        r.host.get(1)->target = 3;
        r.step();
        CHECK(r.m.empty());
        r.host.get(1)->target = 2;
        r.host.get(2)->target = 1;
        r.frame = 30;
        r.step();
        CHECK(!r.m.empty());
    }
    {  // each B joins one clash: two A heroes near one B -> one clash, the lower id first; the Action_Driven entry never
       // starts even with coincident positions
        ClashRig r;
        r.host.add(3, kTypeA, 0, Vec3{10.0f, 0.0f, 0.0f});
        r.host.add(4, kTypeC, 0, Vec3{50.0f, 10.0f, 0.0f});  // coincident with hero 2 (kTypeB vs kTypeC, range 0)
        r.step();
        CHECK(r.m.clashes().size() == 1 && r.m.clashes()[0].hero1 == 1 && !r.host.get(3)->flag && !r.host.get(4)->flag);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_hero_clash_fight() {
    {  // start -> snap (not walking) -> fight; every exchange 99 frames, damage once at 80 % (188 / 75), outcome never
       // repeats; the clash ends with the loser's death and releases the winner
        ClashRig r;
        r.step();
        CHECK(r.m.clashes().size() == 1);
        r.step();  // approach: not walking, off the combat distance -> snap
        CHECK(r.m.clashes()[0].state == HeroClashManager::kSnap);
        const HeroClashManager::Clash c0 = r.m.clashes()[0];
        CHECK(c0.snap_end > c0.snap_start && c0.snap_end - c0.snap_start <= 30u);
        while (!r.m.empty() && r.m.clashes()[0].state == HeroClashManager::kSnap) r.step();
        CHECK(!r.m.empty() && r.m.clashes()[0].state == HeroClashManager::kFight);
        CHECK_F(r.host.get(1)->pos.x, c0.spot1.x);  // placed on the spots
        CHECK_F(r.host.get(2)->pos.x, c0.spot2.x);
        CHECK(r.host.get(1)->held && r.host.get(2)->held);
        CHECK_NEAR(r.host.get(1)->yaw, 0.0, 1e-3);  // facing each other along X
        CHECK_NEAR(r.host.get(2)->yaw, 180.0, 1e-3);
        uint8_t prev = kExchangeNone;
        uint32_t exchanges = 0, ex_start = 0, hits = 0;
        bool repeated = false, bad = false;
        float h1 = 500.0f, h2 = 500.0f;
        for (int i = 0; i < 4000 && !r.m.empty(); ++i) {
            const uint32_t calls = r.host.damage_calls;
            const HeroClashManager::Clash before = r.m.clashes()[0];
            r.step();
            if (r.host.damage_calls != calls) {
                ++hits;
                // the frame hero 1's clip reaches 80 %: 81 frames into a 101-frame exchange
                if (r.frame - 1u - before.ex_start != 81u) bad = true;
                const float dmg = r.host.last_damage;
                if (before.ex_winner == 1 && dmg != 188.0f) bad = true;
                if (before.ex_winner == 2 && dmg != 75.0f) bad = true;
                if (before.ex_winner == 1) {
                    h2 -= dmg;
                } else {
                    h1 -= dmg;
                }
            }
            if (!r.m.empty() && r.m.clashes()[0].exchanges != before.exchanges) {
                const HeroClashManager::Clash& c = r.m.clashes()[0];
                if (c.outcome == prev) repeated = true;
                if (exchanges > 0 && c.ex_start - ex_start != 99u) bad = true;  // count - 2 frames per exchange
                prev = c.outcome;
                ex_start = c.ex_start;
                ++exchanges;
            }
        }
        CHECK(r.m.empty() && !repeated && !bad && hits > 0);
        CHECK(r.rng.draws() == exchanges);  // one float draw per exchange (one variant, no conversation)
        CHECK_F(r.host.get(1)->health, h1);
        CHECK_F(r.host.get(2)->health, h2);
        CHECK(r.host.get(1)->alive != r.host.get(2)->alive);
        CHECK(!r.host.get(1)->flag && !r.host.get(2)->flag && !r.host.get(1)->held && !r.host.get(2)->held);
        const std::vector<HeroClashManager::EndEvent> ev = r.m.take_ended();
        CHECK(ev.size() == 1);
        if (!ev.empty()) CHECK(!ev[0].by_removal && ev[0].winner == (r.host.get(1)->alive ? 1 : 2));
        CHECK(r.m.take_changed());
    }
    {  // already at the combat distance: no snap, straight to the fight
        ClashRig r;
        r.host.get(2)->pos = Vec3{17.65f, 0.0f, 0.0f};
        r.step();
        r.step();
        CHECK(r.m.clashes()[0].state == HeroClashManager::kFight && r.host.get(1)->places == 0);
    }
    {  // walking heroes wait in the approach
        ClashRig r;
        r.host.get(1)->walking = true;
        for (int i = 0; i < 6; ++i) r.step();
        CHECK(r.m.clashes()[0].state == HeroClashManager::kApproach);
        r.host.get(1)->walking = false;
        r.step();
        CHECK(r.m.clashes()[0].state == HeroClashManager::kSnap);
    }
    {  // a model without the clip: nothing plays, a new exchange (and draw) every frame
        ClashRig r;
        r.host.model = true;
        r.host.model_variants = 0;
        r.host.model_frames = 0;
        r.host.get(2)->pos = Vec3{17.65f, 0.0f, 0.0f};
        r.step();
        r.step();  // the start frame runs the update too: approach -> fight
        for (int i = 0; i < 10; ++i) r.step();
        CHECK(r.rng.draws() == 11 && r.host.damage_calls == 0);  // 11 updates since the fight
    }
    {  // two model variants: one extra synced int draw per exchange
        ClashRig r;
        r.host.model = true;
        r.host.model_variants = 2;
        r.host.get(2)->pos = Vec3{17.65f, 0.0f, 0.0f};
        r.step();
        r.step();
        r.step();  // first exchange
        CHECK(r.rng.draws() == 2 && r.m.clashes()[0].playing);
    }
}
}  // namespace bo3split

namespace bo3split {

void test_hero_clash_end_and_save() {
    {  // a hero removed in the fight: ends with the other as winner; the survivor is released, the gone hero's flag cleared
        ClashRig r;
        r.host.get(2)->pos = Vec3{17.65f, 0.0f, 0.0f};
        r.step();
        r.step();
        for (int i = 0; i < 20; ++i) r.step();
        r.host.get(1)->alive = false;
        r.step();
        CHECK(r.m.empty() && !r.host.get(1)->flag && !r.host.get(2)->flag && !r.host.get(2)->held);
        const std::vector<HeroClashManager::EndEvent> ev = r.m.take_ended();
        CHECK(ev.size() == 1);
        if (!ev.empty()) CHECK(ev[0].by_removal && ev[0].winner == 2);
    }
    {  // removed during the snap: the survivor is not released (retail quirk kept) - flagged and locked for good
        ClashRig r;
        r.step();
        r.step();
        CHECK(r.m.clashes()[0].state == HeroClashManager::kSnap);
        r.host.get(2)->alive = false;
        r.step();
        CHECK(r.m.empty() && r.host.get(1)->flag);
        for (int i = 0; i < 40; ++i) r.step();  // never restarts (in a clash already)
        CHECK(r.m.empty() && r.host.get(1)->flag);
    }
    {  // save / load mid-fight: the copy continues bit for bit (same outcomes, damage, end frame, hash)
        ClashRig a;
        for (int i = 0; i < 150; ++i) a.step();
        CHECK(!a.m.empty());
        ByteWriter w;
        a.m.write(w);
        ClashRig b;
        b.host = a.host;
        b.rng = a.rng;
        b.frame = a.frame;
        ByteReader rd(w.bytes());
        CHECK(b.m.read(rd) && rd.done());
        CHECK(b.m.take_changed());
        StateHasher ha, hb;
        a.m.hash(ha);
        b.m.hash(hb);
        CHECK(ha.digest() == hb.digest());
        for (int i = 0; i < 4000 && !a.m.empty(); ++i) {
            a.step();
            b.step();
        }
        CHECK(a.m.empty() && b.m.empty() && a.frame == b.frame);
        CHECK_F(a.host.get(1)->health, b.host.get(1)->health);
        CHECK_F(a.host.get(2)->health, b.host.get(2)->health);
        CHECK(a.rng.draws() == b.rng.draws());
        ByteWriter w2;
        a.m.write(w2);  // empty list: reads without data
        HeroClashManager none;
        ByteReader r2(w2.bytes());
        CHECK(none.read(r2));
        std::vector<uint8_t> cut(w.bytes().begin(), w.bytes().end() - 3);  // truncated
        ByteReader r3(cut);
        HeroClashManager c;
        c.set_data(&a.data, {{kTypeA, kTypeB}});
        CHECK(!c.read(r3));
        HeroClashManager nodata;  // a clash needs the entries (set_data first)
        ByteReader r4(w.bytes());
        CHECK(!nodata.read(r4));
    }
}
}  // namespace bo3split

namespace bo3split {

void test_hero_clash_pairing() {
    // CombatSystem side (§5.2): the pairing rule, Set_Target refusals, the candidate scan / validate / lock drop, TARGETING
    // suspended while flagged; a plain Take_Damage (area damage) still applies.
    TargetWorld t;
    t.w.add(1, t.attacker, 0, Vec3{0, 0, 0});
    t.w.add(2, t.capital, 1, Vec3{300, 0, 0});
    t.w.add(3, t.attacker, 1, Vec3{0, 300, 0});
    t.w.add(4, t.attacker, 0, Vec3{300, 300, 0});
    CombatSystem& s = *t.w.sys;
    CHECK(s.clash_allows(1, 2) && s.clash_allows(1, kNoObject));
    s.set_flag(1, kUnitHeroClash, true);
    s.set_flag(3, kUnitHeroClash, true);
    s.set_hero_clashes({{1, 3}});
    CHECK(s.clash_allows(1, 3) && s.clash_allows(3, 1));
    CHECK(!s.clash_allows(1, 2) && !s.clash_allows(2, 1) && !s.clash_allows(4, 3));
    s.set_flag(2, kUnitHeroClash, true);  // flagged but in no clash (the snap quirk): nothing pairs with it
    CHECK(!s.clash_allows(1, 2) && !s.clash_allows(2, 1) && !s.clash_allows(2, 3));
    CHECK(s.clash_allows(2, kNoObject));
    s.set_flag(2, kUnitHeroClash, false);
    // Set_Target: refused on a flagged target and for a flagged attacker (the partner too)
    s.order_attack(4, 3);
    CHECK(s.ordered_target(4) == kNoObject);
    s.order_attack(1, 2);
    CHECK(s.ordered_target(1) == kNoObject);
    s.order_attack(1, 3);
    CHECK(s.ordered_target(1) == kNoObject);
    CHECK(!s.order_attack_issue(4, 3) && !s.order_attack_issue(1, 2));
    // TARGETING suspended: a flagged unit acquires nothing; nobody acquires a flagged target
    for (int i = 0; i < 60; ++i) t.w.tick();
    CHECK(s.auto_target(1) == kNoObject && s.auto_target(3) == kNoObject && s.auto_target(4) != 3);
    // a target that joins a clash is dropped at the next tick (validate / lock), an ordered one too
    s.set_flag(1, kUnitHeroClash, false);
    s.set_flag(3, kUnitHeroClash, false);
    s.set_hero_clashes({});
    s.order_attack(4, 3);
    t.w.tick();
    CHECK(s.current_target(4) == 3);
    s.set_flag(3, kUnitHeroClash, true);
    t.w.tick();
    CHECK(s.current_target(4) == kNoObject);
    // area / direct damage has no clash test
    const float h0 = s.health(3);
    DamageRequest d;
    d.victim = 3;
    d.amount = 10.0f;
    d.source = 4;
    s.take_damage(d, t.w.frame);
    CHECK(s.health(3) < h0);
    // the flag is hashed with the unit
    StateHasher ha, hb;
    s.hash_units(ha);
    s.set_flag(3, kUnitHeroClash, false);
    s.hash_units(hb);
    CHECK(ha.digest() != hb.digest());
}
}  // namespace bo3split

namespace bo3split {

void test_hero_clash_fire_refused() {
    // Fire_At 0x3825b0 / the hardpoint pick 0x385190: a hardpoint shooter stops firing at a target that joined a clash.
    World w;
    ProjectileDesc pj;
    pj.max_speed = 50.0f;
    pj.damage = 5.0f;
    w.data.projectiles.push_back(pj);
    HardpointDesc gun;
    gun.kind = HardpointKind::WeaponLaser;
    gun.health = 100.0f;
    gun.projectile = 0;
    gun.min_recharge = gun.max_recharge = 0.2f;
    gun.fire_range = 2000.0f;
    w.data.hardpoints.push_back(gun);
    UnitDesc sh;
    sh.tactical_health = 100.0f;
    sh.max_attack_distance = 2000.0f;
    sh.hardpoints = {0};
    push_unit(w.data, sh);
    UnitDesc tg;
    tg.tactical_health = 100000.0f;
    tg.radius = 20.0f;
    push_unit(w.data, tg);
    CombatConfig cfg;
    cfg.inaccuracy_enabled = false;
    w.make(cfg);
    w.add(1, 0, 0, Vec3{-600.0f, 0.0f, 0.0f});
    w.add(2, 1, 1, Vec3{0.0f, 0.0f, 0.0f});
    w.sys->order_attack(1, 2);
    uint32_t shots = 0;
    for (int i = 0; i < 30; ++i) {
        w.tick();
        shots += static_cast<uint32_t>(w.sys->shot_events().size());
    }
    CHECK(shots > 0);
    w.sys->set_flag(2, kUnitHeroClash, true);
    shots = 0;
    for (int i = 0; i < 30; ++i) {
        w.tick();
        shots += static_cast<uint32_t>(w.sys->shot_events().size());
    }
    CHECK(shots == 0 && w.sys->current_target(1) == kNoObject);
    // s78n t06 review: the own weapon (WeaponBehavior, fire_unit_weapon) applies the same pairing rule. [port] The per-tick
    // target drop already takes a joining clash hero away before any weapon service, so this covers the path only.
    World u;
    u.data.projectiles.push_back(pj);
    UnitDesc gunner;
    gunner.tactical_health = 100.0f;
    gunner.max_attack_distance = 2000.0f;
    gunner.weapon_projectile = 0;
    gunner.weapon_recharge = 0.2f;
    push_unit(u.data, gunner);
    push_unit(u.data, tg);
    u.make(cfg);
    u.add(1, 0, 0, Vec3{-600.0f, 0.0f, 0.0f});
    u.add(2, 1, 1, Vec3{0.0f, 0.0f, 0.0f});
    u.sys->order_attack(1, 2);
    shots = 0;
    for (int i = 0; i < 60; ++i) {
        u.tick();
        shots += static_cast<uint32_t>(u.sys->shot_events().size());
    }
    CHECK(shots > 0);
    u.sys->set_flag(2, kUnitHeroClash, true);
    shots = 0;
    for (int i = 0; i < 60; ++i) {
        u.tick();
        shots += static_cast<uint32_t>(u.sys->shot_events().size());
    }
    CHECK(shots == 0 && u.sys->current_target(1) == kNoObject);
}
}  // namespace bo3split

namespace bo3split {

void test_hero_clash_foc_data() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("hero clash foc data: skipped (ALAMO_FOC_DATA unset)\n");
        return;
    }
#ifdef ALAMO_COMBAT_TESTS_HAVE_VFS
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    std::string verr;
    const std::unique_ptr<vfs::Vfs> vfs_ptr = vfs::Vfs::create(vcfg, &verr);
    if (!vfs_ptr) {
        std::printf("FAIL hero clash foc data: VFS on %s: %s\n", root, verr.c_str());
        ++g_fail;
        return;
    }
    const FileProvider& fs = *vfs_ptr;
#else
    DirectoryProvider fs(root);
#endif
    HeroClashData d;
    std::vector<std::string> diag;
    CHECK(load_hero_clash_data(fs, d, &diag));
    CHECK(d.entries.size() == 3 && diag.empty());
    if (d.entries.size() != 3) return;
    const HeroClashEntry& e = d.entries[0];
    CHECK(e.name == "Vader_ObiWan" && e.clash_type == kClashMelee && !e.play_conversation);
    CHECK(e.first_type == "Darth_Vader" && e.second_type == "Obi_Wan_Kenobi");
    CHECK_F(e.clash_range, 100.0f);
    CHECK_F(e.combat_distance, 17.65f);
    CHECK(clash_exchange_damage(e, 500.0f, true, false) == 188);
    CHECK(clash_exchange_damage(e, 500.0f, false, true) == 75);
    CHECK(e.first_anims[0] == "HC_WIN" && e.second_anims[1] == "HC_LOSE");
    CHECK(d.entries[1].clash_type == kClashActionDriven && d.entries[2].clash_type == kClashActionDriven);
    CHECK(d.entries[1].name == d.entries[2].name);  // the duplicate ObiWan_BobaFett, both kept
    CHECK_F(d.entries[1].clash_range, 0.0f);
    std::printf("hero clash foc data: %zu entries\n", d.entries.size());
}
}  // namespace bo3split

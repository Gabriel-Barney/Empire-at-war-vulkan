// alamo_sim_battle_ai_tests: S10 battle transitions, host side (docs/re/BATTLE_TRANSITIONS.md): the tactical AI in battles (BattleAi orders, find team, land drop, constants), land teams, landing objects.
// Split out of test_battles.cpp (WF1b t01); the fixture (synthetic data, maps, run_campaign) is battle_test_fixture.hpp.

#include "battle_test_fixture.hpp"

namespace {

#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
void test_battle_ai_orders(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = d.files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    using R = commands::RejectReason;
    objects::ObjectId mover = objects::kNoObjectId, target = objects::kNoObjectId;
    float d0 = 0.0f;
    bool issued = false, closer = false;
    for (uint64_t t = 0; t < 2500; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        objects::ObjectStore& st = tm->primary();
        auto dist = [&st](objects::ObjectId a, objects::ObjectId c) {
            const Vec3 p = st.position(st.find(a)), q = st.position(st.find(c));
            return std::sqrt((p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y));
        };
        if (b->frame() == 3 && !issued) {
            issued = true;
            objects::ObjectId own = objects::kNoObjectId, corvette = objects::kNoObjectId;
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h) || !b->movement().has_unit(st.id(h))) continue;
                const auto ty = static_cast<uint32_t>(st.type(h));
                if (ty == type_of(d, "S10_Frigate") && st.owner(h) == 1 && mover == objects::kNoObjectId) mover = st.id(h);
                if (ty == type_of(d, "S10_Corvette") && st.owner(h) == 1 && corvette == objects::kNoObjectId) corvette = st.id(h);
                if (ty == type_of(d, "S10_Frigate") && st.owner(h) == 0 && target == objects::kNoObjectId) target = st.id(h);
                if (st.owner(h) == 0 && own == objects::kNoObjectId) own = st.id(h);
            }
            CHECK(mover != objects::kNoObjectId && target != objects::kNoObjectId && corvette != objects::kNoObjectId);
            if (mover == objects::kNoObjectId || target == objects::kNoObjectId || corvette == objects::kNoObjectId) break;
            BattleAi ai(*b, *tm, host, setup);
            CHECK(!ai.controls(0) && !ai.controls(1));
            CHECK(ai.execute(1, commands::ev::attack(static_cast<uint32_t>(target), {static_cast<uint32_t>(mover)})) == R::None);
            CHECK(b->combat().ordered_target(mover) == target);
            d0 = dist(mover, target);
            CHECK(ai.execute(1, commands::ev::move_to_object(static_cast<uint32_t>(target), {static_cast<uint32_t>(mover)})) == R::None);
            CHECK(b->combat().ordered_target(mover) == objects::kNoObjectId);  // combat order cleared
            const auto v = b->movement().view(mover);
            const Vec3 tp = st.position(st.find(target));
            // The goal is the target's position up to the movement system's arrival offset.
            const float gd = std::sqrt((v.goal.x - tp.x) * (v.goal.x - tp.x) + (v.goal.y - tp.y) * (v.goal.y - tp.y));
            CHECK(v.has_goal && gd < 0.2f * d0);
            CHECK(ai.execute(1, commands::ev::move_to_object(0x200000u, {static_cast<uint32_t>(mover)})) == R::None);  // no such target: no-op
            CHECK(b->movement().view(mover).has_goal);
            CHECK(ai.execute(1, commands::ev::move_to_object(static_cast<uint32_t>(target), {static_cast<uint32_t>(own)})) == R::InvalidUnit);  // not player 1's unit
            // HIJACK_UNIT: a galactic frigate at the battle planet joins player 1's pool; a land squad is refused.
            const size_t pool0 = b->reserves(1).size();
            const ai::tactical::TacticalReinforcement r = ai.hijack_unit(1, static_cast<int32_t>(type_of(d, "S10_Frigate")), false);
            CHECK(r.key >= 0);
            CHECK(b->reserves(1).size() == pool0 + 1);
            CHECK(ai.hijack_unit(1, static_cast<int32_t>(type_of(d, "S10_Squad")), false).key < 0);
            CHECK(b->reserves(1).size() == pool0 + 1);
            // Change_Owner from an AI VM: the corvette joins the Empire (store owner, no longer an enemy of player 0).
            lua::ObjectBridge& br = ai.world().object_bridge();
            CHECK(static_cast<bool>(br.change_owner));
            if (br.change_owner) {
                CHECK(br.change_owner(st.find(corvette), 0) && st.owner(st.find(corvette)) == 0);
                CHECK(!br.change_owner(objects::ObjectHandle{}, 0));
            }
            CHECK(!b->combat().is_enemy(0, st.owner(st.find(corvette))));
            std::printf("  battle AI: MoveToObject goal (%.0f, %.0f) %.0f from the target, distance %.0f; hijack key %d, pool %zu -> %zu\n",
                        static_cast<double>(v.goal.x), static_cast<double>(v.goal.y), static_cast<double>(gd), static_cast<double>(d0), r.key, pool0,
                        b->reserves(1).size());
        }
        if (issued && b->frame() == 63) {
            CHECK(st.alive(st.find(mover)) && st.alive(st.find(target)));
            if (st.alive(st.find(mover)) && st.alive(st.find(target))) {
                const float d1 = dist(mover, target);
                std::printf("  battle AI: mover %.0f -> %.0f from the target after 60 frames\n", static_cast<double>(d0),
                            static_cast<double>(d1));
                closer = d1 < d0 - 10.0f;
            }
            break;
        }
    }
    CHECK(issued && closer);
}

// refixf t02: no two space units on one exact point (foc-station seeds 1 / 4 stacked the rebel fleet on the station's centre):
//   * a SPACE_OBSTACLE star base is a static entry of the space occupancy from the battle start (MOVEMENT §2.2) and leaves it
//     when it dies;
//   * the tactical AI's task-force Attack_Move on the station (refixf t06: one PortExtension group order, the further units
//     after the 24-byte block) is one group move (MOVEMENT §6.3): every goal outside the station's footprint and no two goals
//     on one point;
//   * no two live movable units share a position at the start, after the orders or while they fly in and fight.
void test_space_obstacle_orders() {
    auto d = make_data(false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup gsetup = make_setup(*d, false);
    for (int i = 0; i < 3; ++i) gsetup.forces.push_back({0, 1, type_of(*d, "S10_Frigate")});  // the battle outlives the station
    galactic::GalacticSim* gs = host.start_galactic(d->rules, gsetup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = d->files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    namespace mv = tactical::movement;
    // Pairs of live movable battle units on one exact XY point.
    auto stacked = [](const TacticalBattle& b, const objects::ObjectStore& st) {
        std::vector<objects::ObjectId> ids;
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h) && b.movement().has_unit(st.id(h))) ids.push_back(st.id(h));
        int pairs = 0;
        for (size_t i = 0; i < ids.size(); ++i)
            for (size_t j = i + 1; j < ids.size(); ++j) {
                const Vec3 p = st.position(st.find(ids[i])), q = st.position(st.find(ids[j]));
                if (p.x == q.x && p.y == q.y) ++pairs;
            }
        return pairs;
    };
    objects::ObjectId station = objects::kNoObjectId;
    Vec3 sp{};
    std::vector<objects::ObjectId> attackers;
    bool issued = false, station_died = false, hp_hit = false, hull_hit = false;
    float hp_max = 0.0f, hull_max = 0.0f;
    int stacks = 0;
    uint32_t last = 0;
    float closest = 1.0e9f;
    for (uint64_t t = 0; t < 6000; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (issued) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        if (b->frame() == 3 && !issued) {
            issued = true;
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h)) continue;
                if (st.owner(h) == 0 && static_cast<uint32_t>(st.type(h)) == type_of(*d, "S10_Star_Base_1")) station = st.id(h);
                if (st.owner(h) == 1 && b->movement().has_unit(st.id(h))) attackers.push_back(st.id(h));
            }
            CHECK(station != objects::kNoObjectId && attackers.size() >= 4);
            if (station == objects::kNoObjectId || attackers.size() < 2) break;
            sp = st.position(st.find(station));
            const mv::SpaceOccupancy& occ = b->movement().space();
            CHECK(occ.has_static(station));
            CHECK((occ.query_static({sp.x, sp.y}, {sp.x, sp.y}, 1.0f) & mv::kSpaceStatic) != 0);
            CHECK((occ.query_static({sp.x + 300.0f, sp.y}, {sp.x + 300.0f, sp.y}, 1.0f) & mv::kSpaceStatic) == 0);
            CHECK(stacked(*b, st) == 0);  // the start placement
            BattleAi ai(*b, *tm, host, setup);
            {  // as the tactical AI's issue_move (refixf t06): ONE group order for the task force, further units after the block
                std::vector<uint8_t> pay;
                auto put = [&pay](uint32_t v) {
                    for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
                };
                put(static_cast<uint32_t>(attackers.front()));
                put(static_cast<uint32_t>(station));
                put(std::bit_cast<uint32_t>(sp.x));
                put(std::bit_cast<uint32_t>(sp.y));
                put(std::bit_cast<uint32_t>(sp.z));
                put(0u);
                for (size_t i = 1; i < attackers.size(); ++i) put(static_cast<uint32_t>(attackers[i]));
                const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::AttackMove));
                CHECK(ai.execute(1, commands::ev::port_extension(sub, pay)) == commands::RejectReason::None);
            }
            std::vector<mv::V2> goals;
            for (const objects::ObjectId a : attackers) {
                const auto v = b->movement().view(a);
                CHECK(v.has_goal);
                const float gd = std::sqrt((v.goal.x - sp.x) * (v.goal.x - sp.x) + (v.goal.y - sp.y) * (v.goal.y - sp.y));
                CHECK(gd >= 250.0f);  // outside the station's footprint
                closest = std::min(closest, gd);
                for (const mv::V2& g : goals) CHECK(!(g.x == v.goal.x && g.y == v.goal.y));
                goals.push_back(v.goal);
            }
            std::printf("  station %d at (%.0f, %.0f): %zu attack-move orders, nearest goal %.0f from its centre\n", station,
                        static_cast<double>(sp.x), static_cast<double>(sp.y), attackers.size(), static_cast<double>(closest));
            // then an explicit attack on it (the weak station dies, the defender's frigates keep the battle going); refixf t06:
            // as the tactical AI's Attack_Target(station, "WEAPON_LASER"): ref2 = 1 + HardPointType, the laser is hardpoint 0
            std::vector<uint32_t> wire;
            for (const objects::ObjectId a : attackers) wire.push_back(static_cast<uint32_t>(a));
            commands::EventData atk = commands::ev::attack(static_cast<uint32_t>(station), wire);
            atk.set("ref2", 1u + static_cast<uint32_t>(tactical::combat::HardpointKind::WeaponLaser));
            CHECK(ai.execute(1, atk) == commands::RejectReason::None);
            for (const objects::ObjectId a : attackers)
                CHECK(b->combat().ordered_target(a) == station && b->combat().ordered_hardpoint(a) == 0);
            hp_max = b->combat().hardpoint_max(station, 0);
            hull_max = b->combat().health(station);
        }
        if (!issued || station == objects::kNoObjectId) continue;
        last = b->frame();
        stacks += stacked(*b, st);
        if (!station_died && !st.alive(st.find(station))) {
            station_died = true;
            std::printf("  station died at frame %u\n", b->frame());
        }
        if (!station_died) {  // refixf t06: the ordered attack damages the ordered hardpoint and the hull
            hp_hit = hp_hit || b->combat().hardpoint_health(station, 0) < hp_max;
            hull_hit = hull_hit || b->combat().health(station) < hull_max;
        }
        if (station_died) CHECK(!b->movement().space().has_static(station));  // removed with its object (same tick)
        if (station_died || b->frame() >= 4000) break;
    }
    std::printf("  stacked pairs over frames 3-%u: %d, station died %d, hardpoint hit %d, hull hit %d\n", last, stacks,
                station_died ? 1 : 0, hp_hit ? 1 : 0, hull_hit ? 1 : 0);
    CHECK(issued && stacks == 0);
    CHECK(station_died && hp_hit && hull_hit);
}

// S7 t29 / t30 (TACTICAL_COMBAT §2.10): the relation table makes the Is_Neutral player relation 2 (not enemy) both ways.
// The station goes to the Neutral player: a task-force Attack_Move on it is NOT refused - space keeps the object as the
// goal, so every member moves there, but nobody gets it as attack goal (path B needs relation 1); the explicit AI Attack
// and the human attack order are refused; a single unit's own Attack_Move (arg 0) is a plain move. Back with the Empire,
// the same task-force Attack_Move is accepted with the station as every member's attack goal.
void test_neutral_attack_move() {
    auto d = make_data(false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup gsetup = make_setup(*d, false);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, gsetup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = d->files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    using R = commands::RejectReason;
    bool done = false;
    for (uint64_t t = 0; t < 6000 && !done; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() != 3) continue;
        done = true;
        objects::ObjectStore& st = tm->primary();
        objects::ObjectId station = objects::kNoObjectId;
        std::vector<objects::ObjectId> attackers;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.alive(h)) continue;
            if (st.owner(h) == 0 && static_cast<uint32_t>(st.type(h)) == type_of(*d, "S10_Star_Base_1")) station = st.id(h);
            if (st.owner(h) == 1 && b->movement().has_unit(st.id(h)) && b->combat().can_attack(st.id(h))) attackers.push_back(st.id(h));
        }
        CHECK(station != objects::kNoObjectId && attackers.size() >= 2);
        if (station == objects::kNoObjectId || attackers.size() < 2) break;
        const Vec3 sp = st.position(st.find(station));
        BattleAi ai(*b, *tm, host, setup);
        auto attack_move = [&](const std::vector<objects::ObjectId>& units, uint32_t arg) {
            std::vector<uint8_t> pay;
            auto put = [&pay](uint32_t v) {
                for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
            };
            put(static_cast<uint32_t>(units.front()));
            put(static_cast<uint32_t>(station));
            put(std::bit_cast<uint32_t>(sp.x));
            put(std::bit_cast<uint32_t>(sp.y));
            put(std::bit_cast<uint32_t>(sp.z));
            put(arg);
            for (size_t i = 1; i < units.size(); ++i) put(static_cast<uint32_t>(units[i]));
            const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::AttackMove));
            return ai.execute(1, commands::ev::port_extension(sub, pay));
        };
        auto no_goal = [&] {
            bool none = true;
            for (const objects::ObjectId a : attackers) none = none && b->combat().attack_goal(a) != station;
            return none;
        };
        std::vector<uint32_t> wire;
        for (const objects::ObjectId a : attackers) wire.push_back(static_cast<uint32_t>(a));
        CHECK(b->change_owner(*tm, st.find(station), 2) && st.owner(st.find(station)) == 2);
        const bool rel = !b->combat().is_enemy(1, 2) && !b->combat().is_enemy(2, 1) && b->combat().is_enemy(1, 0);
        const R tf_neutral = attack_move(attackers, ai::tactical::kTfAttackMoveIssueAttack);
        bool tf_moved = true;
        for (const objects::ObjectId a : attackers) {
            const auto va = b->movement().view(a);
            tf_moved = tf_moved && va.has_goal && std::abs(va.goal.x - sp.x) < 1500.0f && std::abs(va.goal.y - sp.y) < 1500.0f;
        }
        const R atk_neutral = ai.execute(1, commands::ev::attack(static_cast<uint32_t>(station), wire));
        const R human_neutral = b->order_attack(*tm, 1, attackers, station, host.random().synced());
        const bool goals_neutral = no_goal();
        const std::vector<objects::ObjectId> one = {attackers.front()};
        const R own_neutral = attack_move(one, 0u);
        const auto v = b->movement().view(attackers.front());
        const bool moved = v.has_goal && std::abs(v.goal.x - sp.x) < 1500.0f && std::abs(v.goal.y - sp.y) < 1500.0f;
        const bool goals_own = no_goal();
        CHECK(b->change_owner(*tm, st.find(station), 0) && st.owner(st.find(station)) == 0);
        const R tf_enemy = attack_move(attackers, ai::tactical::kTfAttackMoveIssueAttack);
        bool goals_enemy = true;
        for (const objects::ObjectId a : attackers) goals_enemy = goals_enemy && b->combat().attack_goal(a) == station;
        std::printf("  neutral station: relation 2 %d; TF Attack_Move %d (moved %d), Attack %d, human attack %d, goals none %d; own "
                    "Attack_Move %d (moved %d, goals none %d); enemy again: TF Attack_Move %d, goals %d\n",
                    rel ? 1 : 0, static_cast<int>(tf_neutral), tf_moved ? 1 : 0, static_cast<int>(atk_neutral),
                    static_cast<int>(human_neutral), goals_neutral ? 1 : 0, static_cast<int>(own_neutral), moved ? 1 : 0,
                    goals_own ? 1 : 0, static_cast<int>(tf_enemy), goals_enemy ? 1 : 0);
        CHECK(rel);
        CHECK(tf_neutral == R::None && tf_moved && atk_neutral == R::InvalidTarget && human_neutral == R::InvalidTarget && goals_neutral);
        CHECK(own_neutral == R::None && moved && goals_own);
        CHECK(tf_enemy == R::None && goals_enemy);
    }
    CHECK(done);
}

// t41: BattleAi::fill hands the perception the station's DUMMY_STAR_BASE and Base_Level (the inputs of Variable_Self.BaseLevel).
void test_battle_ai_station_level(bool skirmish) {
    auto d = make_data(false, false, false, false, true);  // the star bases carry DUMMY_STAR_BASE here
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.battle.skirmish = skirmish;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = d->files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    bool checked = false;
    for (uint64_t t = 0; t < 2500 && !checked; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        BattleAi ai(*b, *tm, host, setup);
        ai::tactical::PerceptionWorld w;
        std::vector<ai::tactical::TacticalAiObject> facts;
        ai.fill(w, facts);
        int stations = 0, others = 0;
        for (const ai::tactical::PerceptionObject& o : w.objects) {
            if (o.dummy_star_base) {
                ++stations;
                CHECK(o.owner == 0 && o.starbase_level == 1 && o.type == static_cast<int32_t>(type_of(*d, "S10_Star_Base_1")));
            } else if (o.starbase_level != 0) {
                ++others;
            }
        }
        CHECK(stations == 1 && others == 0);
        CHECK(w.campaign == !skirmish);  // t48: space skirmish runs the skirmish rules, its markers are the battle anchors
        if (skirmish) {
            CHECK(w.markers.size() == 2);
            for (const ai::tactical::StartMarker& m : w.markers) CHECK(m.kind == ai::tactical::StartMarker::Kind::PlayerPosition);
        }
        checked = true;
    }
    CHECK(checked);
}

// t48: the battle mode reaches the perception (Game.IsCampaignGame, IsDefender, the start markers) and so do both land BaseLevel
// inputs: campaign = the conflict planet's structures, skirmish = each player's DUMMY_GROUND_STRUCTURE objects.
void test_battle_ai_land_mode(bool skirmish) {
    auto d = make_data(false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup setup = make_setup(*d, true);
    setup.forces.push_back({0, 1, type_of(*d, "S4_Barracks")});
    galactic::GalacticSim* gs = host.start_galactic(d->rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CHECK(gs->planets()[1].structures.size() == 1);
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.battle.skirmish = skirmish;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d->files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 2500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        BattleAi ai(*b, *tm, host, ts);
        ai::tactical::PerceptionWorld w;
        std::vector<ai::tactical::TacticalAiObject> facts;
        ai.fill(w, facts);
        CHECK(w.campaign == !skirmish);  // t50: a land skirmish runs the skirmish rules too (it has its skirmish base now)
        CHECK(w.conflict_ground_structures == 1);  // set in both modes; the token picks by mode
        int own = 0, other = 0;
        for (const ai::tactical::PerceptionObject& o : w.objects)
            if (o.dummy_ground_structure) ++(o.owner == 0 ? own : other);
        CHECK(own == 1 && other == 0);
        if (skirmish) {  // the battle anchors stand for Player_%d_Position_Marker
            CHECK(w.markers.size() == 2);
            for (const ai::tactical::StartMarker& m : w.markers) CHECK(m.kind == ai::tactical::StartMarker::Kind::PlayerPosition);
        } else {
            for (const ai::tactical::StartMarker& m : w.markers) CHECK(m.kind != ai::tactical::StartMarker::Kind::PlayerPosition);
        }
        checked = true;
    }
    CHECK(checked);
}

// refixf t03: Find_All_Objects_Of_Type matches the representative type (LUA_API.md "Find_All_Objects_Of_Type and the
// sibling finders"): in a land battle (team_types) a script's 'S10_Squad' finds the squads' TEAM containers (store type
// "Team") through the battle's ObjectBridge::representative_type hook, and 'Team' finds none. Without the hook the store
// type is matched instead.
void test_battle_ai_find_team() {
    auto d = make_data(false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d->files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    const objects::TypeIndex team = d->types.resolve_name("Team"), squad = d->types.resolve_name("S10_Squad"),
                             trooper = d->types.resolve_name("S10_Trooper");
    CHECK(team != objects::kNone && squad != objects::kNone && trooper != objects::kNone);
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 2500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        objects::ObjectStore& st = tm->primary();
        std::vector<objects::ObjectHandle> containers;
        size_t troopers = 0;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.alive(h)) continue;
            if (st.type(h) == team) containers.push_back(h);
            if (st.type(h) == trooper) ++troopers;
        }
        BattleAi ai(*b, *tm, host, ts);
        lua::ObjectBridge& br = ai.world().object_bridge();
        CHECK(static_cast<bool>(br.representative_type));
        if (!br.representative_type) break;
        for (const objects::ObjectHandle h : containers) CHECK(br.representative_type(h) == squad);
        CHECK(br.representative_type(objects::ObjectHandle{}) == objects::kNone);
        lua::LuaHost lh;
        lh.store = &st;
        lh.types = &d->types;
        lua::LuaVm vm(lh);
        lua::attach_object_bridge(vm, &br);
        // The count goes back through the chunk's success: error() when the finder's result has another size.
        auto finds = [&vm](const char* filter, size_t n) {
            std::string err;
            const std::string code = std::string("local n = 0 for _ in pairs(Find_All_Objects_Of_Type('") + filter +
                                     "')) do n = n + 1 end if n ~= " + std::to_string(n) + " then error('found ' .. n) end";
            const bool ok = vm.run(code, "find_team", &err);
            if (!ok) std::printf("  find team: '%s' (expected %zu): %s\n", filter, n, err.c_str());
            return ok;
        };
        std::printf("  find team: frame %u, %zu TEAM containers, %zu troopers\n", b->frame(), containers.size(), troopers);
        CHECK(!containers.empty() && troopers > 0);
        CHECK(finds("S10_Squad", containers.size()) && finds("Team", 0) && finds("S10_Trooper", troopers));
        const auto hook = br.representative_type;  // without the hook: the store type
        br.representative_type = nullptr;
        CHECK(finds("S10_Squad", 0) && finds("Team", containers.size()));
        br.representative_type = hook;
        lua::attach_object_bridge(vm, nullptr);
        checked = true;
    }
    CHECK(checked);
}

// q6: an attack ordered on a TEAM container (A1_READINGS §33.2.1 row 1: a TEAM container is a valid target on its own;
// TACTICAL_COMBAT §2.8 B step 5, LOB_SOLVER §5.1: each shot takes the container's member nearest the shooter). Land battle
// with team data: player 1's troopers attack one of player 0's squad containers. The AI host's Attack (BattleAi) and the
// human order (TacticalBattle::order_attack) are accepted (both were InvalidTarget); each attacker's combat target is the
// container's live member nearest it; when a member dies the attackers take the next one; the order ends when the
// container has no member left (no ordered target afterwards).
void test_battle_attack_team() {
    auto d = make_data(false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test orders units
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d->files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    using R = commands::RejectReason;
    const objects::TypeIndex team = d->types.resolve_name("Team");
    CHECK(team != objects::kNone);
    bool invaded = false, ordered = false, ended = false;
    objects::ObjectId target = objects::kNoObjectId;
    objects::ObjectHandle target_h;
    std::vector<objects::ObjectId> attackers, first_picks;
    size_t retargets = 0, wrong = 0, gone_frames = 0;
    uint32_t end_frame = 0;
    for (uint64_t t = 0; t < 8000 && !ended; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (ordered) break;  // the battle ended
            continue;
        }
        if (b->frame() < 3) continue;
        objects::ObjectStore& st = tm->primary();
        // The defenders do not fire back (TARGETING suspended every frame), so the attackers outlive the container: with the
        // s78f-s78i squad rules (team targeting copies, Mod_Multiplier) player 0's squads win this land fight.
        {
            auto& cs = const_cast<tactical::combat::CombatSystem&>(b->combat());
            for (const objects::ObjectHandle h : st.all_objects())
                if (st.alive(h) && st.owner(h) == 0 && cs.has_unit(st.id(h)))
                    cs.set_flag(st.id(h), tactical::combat::kUnitTargetingSuspended, true);
        }
        auto members_of = [&](objects::ObjectId c) {
            std::vector<objects::ObjectId> m;
            b->select_all_entry(c, -1, m);
            return m;
        };
        auto nearest = [&](objects::ObjectId a, const std::vector<objects::ObjectId>& ms) {
            objects::ObjectId best = objects::kNoObjectId;
            float bd = 0.0f;
            const Vec3 p = st.position(st.find(a));
            for (const objects::ObjectId m : ms) {
                const Vec3 q = st.position(st.find(m));
                const float dd = (p.x - q.x) * (p.x - q.x) + (p.y - q.y) * (p.y - q.y);
                if (best == objects::kNoObjectId || dd < bd || (dd == bd && m < best)) {
                    best = m;
                    bd = dd;
                }
            }
            return best;
        };
        if (!ordered) {
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h)) continue;
                if (st.type(h) == team && st.owner(h) == 0 && target == objects::kNoObjectId && members_of(st.id(h)).size() >= 2) {
                    target = st.id(h);
                    target_h = h;
                }
                if (st.owner(h) == 1 && b->combat().has_unit(st.id(h)) && b->movement().has_unit(st.id(h))) attackers.push_back(st.id(h));
            }
            std::sort(attackers.begin(), attackers.end());
            std::printf("  team attack: frame %u, target container %d (%zu members), %zu attackers\n", b->frame(), target,
                        members_of(target).size(), attackers.size());
            CHECK(target != objects::kNoObjectId && !attackers.empty());
            if (target == objects::kNoObjectId || attackers.empty()) break;
            std::vector<uint32_t> wire;
            for (const objects::ObjectId a : attackers) wire.push_back(static_cast<uint32_t>(a));
            const std::vector<objects::ObjectId> ms = members_of(target);
            // AI host: accepted, every attacker on a member (its nearest)
            BattleAi ai(*b, *tm, host, ts);
            CHECK(ai.execute(1, commands::ev::attack(static_cast<uint32_t>(target), wire)) == R::None);
            for (const objects::ObjectId a : attackers) {
                CHECK(b->combat().ordered_target(a) == nearest(a, ms));
                CHECK(b->combat().attack_goal(a) == b->combat().ordered_target(a));
            }
            // the human order (replaces the AI's): accepted the same way
            CHECK(b->order_attack(*tm, 1, attackers, target, host.random().synced()) == R::None);
            for (const objects::ObjectId a : attackers) {
                CHECK(b->combat().ordered_target(a) == nearest(a, ms));
                first_picks.push_back(b->combat().ordered_target(a));
            }
            CHECK(b->order_attack(*tm, 1, attackers, attackers.front(), host.random().synced()) == R::InvalidTarget);  // own unit
            ordered = true;
            continue;
        }
        const std::vector<objects::ObjectId> ms = members_of(target);
        const bool container_live = st.alive(target_h) && !ms.empty();
        // the frame the last member died the attackers still name it; combat drops the dead target in the next decide
        if (!container_live && ++gone_frames < 2) continue;
        for (size_t k = 0; k < attackers.size(); ++k) {
            const objects::ObjectId a = attackers[k];
            if (!st.alive(st.find(a)) || !b->combat().has_unit(a) || !b->combat().alive(a)) continue;
            const objects::ObjectId o = b->combat().ordered_target(a);
            if (container_live) {
                if (o != nearest(a, ms)) ++wrong;  // after the service the target is the nearest live member
                if (o != first_picks[k]) {
                    ++retargets;
                    first_picks[k] = o;
                }
            } else if (o != objects::kNoObjectId) {
                ++wrong;  // the order ended with the container
            }
        }
        if (!container_live && gone_frames > 3) {
            ended = true;
            end_frame = b->frame();
        }
    }
    std::printf("  team attack: container gone %d (frame %u), %zu target changes, %zu frames off the rule\n", ended ? 1 : 0, end_frame,
                retargets, wrong);
    CHECK(ordered && ended && retargets > 0 && wrong == 0);
}

// s78f t07 (TACTICAL_COMBAT §3.1.1 "Team targeting copies"): land team members join combat with their team (the TEAM
// container's object id), so the TeamTargeting copy runs in a real battle: a member keeps no target of its own and reads
// its team head's (the lowest-id live member standing in for the container), and every non-head member holds the same
// copied targeting state.
void test_land_team_targeting() {
    auto d = make_data(false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const objects::TypeIndex team = d->types.resolve_name("Team"), trooper = d->types.resolve_name("S10_Trooper");
    CHECK(team != objects::kNone && trooper != objects::kNone);
    bool invaded = false, registered = false;
    uint32_t copies = 0;  // ticks on which a team of >= 3 live members attacked through the copy
    for (uint64_t t = 0; t < 6000 && copies < 5; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        objects::ObjectStore& st = tm->primary();
        const tactical::combat::CombatSystem& cs = b->combat();
        std::vector<objects::ObjectId> teams;
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h) && st.type(h) == team) teams.push_back(st.id(h));
        std::sort(teams.begin(), teams.end());
        std::vector<std::vector<objects::ObjectId>> members(teams.size());  // per team, ascending id (combat order)
        for (size_t i = 0; i < cs.unit_count(); ++i) {
            const auto id = cs.unit_id_at(i);
            const uint32_t tid = cs.team(id);
            if (tid == 0) continue;
            const auto oid = static_cast<objects::ObjectId>(tid);
            const auto it = std::lower_bound(teams.begin(), teams.end(), oid);
            CHECK(it != teams.end() && *it == oid);  // the team id is a live TEAM container
            if (it == teams.end() || *it != oid) continue;
            const objects::ObjectHandle mh = st.find(id);
            CHECK(st.alive(mh) && st.type(mh) == trooper);
            if (cs.alive(id)) members[static_cast<size_t>(it - teams.begin())].push_back(id);
        }
        for (const auto& m : members) {
            if (m.empty()) continue;
            registered = true;
            const auto head = cs.current_target(m.front());
            for (size_t k = 1; k < m.size(); ++k) {
                CHECK(cs.current_target(m[k]) == head);  // the head's target, none of its own
                CHECK(cs.targeting_state(m[k]) == cs.targeting_state(m[1]));  // the raw state copy
            }
            if (m.size() >= 3 && head != tactical::combat::kNoObject && cs.targeting_state(m[1]) == 4) ++copies;
        }
    }
    std::printf("  land team targeting: registered %d, attacking copy ticks %u\n", registered ? 1 : 0, copies);
    CHECK(registered);
    CHECK(copies >= 5);
}

// s78j t03 / t04: a land team's SPEED and Change_Owner in a battle (sprint_types: TEAM containers whose troopers carry
// SPRINT, SPEED x 1.5).
//  * t03 (TACTICAL_COMBAT §4.4, MOVEMENT §3.4 item 4; s78k t03): the team locomotor runs at the container's speed - the
//    minimum member Max_Speed x the container's SPEED value (CombatSystem::team_mod_multiplier) - while every member
//    keeps its own product: one sprinting member (own factor 1.5) leaves the team cap at 1.0 x Max_Speed (the others
//    cap the minimum) and walks no faster than that; every member sprinting raises the cap to 1.5 x.
//  * t04 (TACTICAL_COMBAT §3.5 step 4): the story / Lua Change_Owner of one member (CampaignBattles::change_owner ->
//    TacticalBattle::script_change_owner) moves the container and every other live member too (store and combat owner);
//    another team is untouched. With the tactical AI build, the AI VMs' bridge (BattleAi) spreads the same way.
void test_team_speed_and_owner() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const objects::TypeIndex team_t = d->types.resolve_name("Team");
    const uint8_t sprint = tactical::abilities::ability_type_from_name("SPRINT");
    CHECK(team_t != objects::kNone);
    namespace tc = alamo::tactical::combat;
    bool invaded = false, done = false;
    int stage = 0, stage_ticks = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId team_id = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    // The live members (ascending id) of every team in combat, by team id.
    auto teams_now = [](const tc::CombatSystem& cs) {
        std::map<uint32_t, std::vector<objects::ObjectId>> out;
        for (size_t i = 0; i < cs.unit_count(); ++i) {
            const auto id = cs.unit_id_at(i);
            if (cs.team(id) != 0 && cs.alive(id)) out[cs.team(id)].push_back(id);
        }
        return out;
    };
    for (uint64_t t = 0; t < 6000 && !done; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        const auto& ms = b->movement();
        if (stage == 0) {  // pick a team of >= 2 live members; SPRINT on its lowest-id member only
            for (const auto& [tid, m] : teams_now(cs))
                if (m.size() >= 2 && team_id == objects::kNoObjectId) {
                    team_id = static_cast<objects::ObjectId>(tid);
                    members = m;
                }
            CHECK(team_id != objects::kNoObjectId);
            if (team_id == objects::kNoObjectId) break;
            CHECK(st.alive(st.find(team_id)) && st.type(st.find(team_id)) == team_t);
            CHECK(b->abilities().set_unit_ability_active(members[0], sprint, true, b->frame()));
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        if (b->frame() < stage_frame + 2) continue;  // the speed factors read last tick's Mod_Multiplier (phase A)
        const float tm_speed = cs.team_mod_multiplier(static_cast<uint32_t>(team_id), tc::ModKind::Speed);
        bool all_alive = true;
        for (const auto id : members) all_alive = all_alive && cs.alive(id);
        if (!all_alive) {
            std::printf("  team speed: a member died before stage %d, skipped\n", stage);
            break;
        }
        if (stage == 1) {
            CHECK(b->abilities().is_active(members[0], sprint) && !b->abilities().is_active(members[1], sprint));
            CHECK(tm_speed == 1.0f);  // the non-sprinting members cap the team
            CHECK(ms.speed_factor(members[0]) == 1.5f);  // s78k t03: the sprinter keeps its own product
            for (size_t k = 1; k < members.size(); ++k) CHECK(ms.speed_factor(members[k]) == 1.0f);
            // The team cap = min Max_Speed x mult x 1.0 = a plain member's max speed, below the sprinter's own.
            const float cap = ms.view(members[1]).max_speed;
            for (const auto id : members) CHECK(ms.view(id).team_speed == cap && ms.view(id).speed <= cap);
            CHECK(ms.view(members[0]).max_speed > cap);
            if (++stage_ticks < 20) continue;  // watch the sprinter stay within the cap for a while
            for (size_t k = 1; k < members.size(); ++k) CHECK(b->abilities().set_unit_ability_active(members[k], sprint, true, b->frame()));
            stage = 2;
            stage_frame = b->frame();
            continue;
        }
        CHECK(tm_speed == 1.5f);
        for (const auto id : members) CHECK(ms.speed_factor(id) == 1.5f && ms.view(id).team_speed == ms.view(id).max_speed);
        std::printf("  team speed: team %d of %zu members: all sprinting, team cap %.3f = member max speed\n", static_cast<int>(team_id),
                    members.size(), static_cast<double>(ms.view(members[0]).team_speed));
        // t04: Change_Owner of the team's last member through the story bridge.
        const int32_t old_owner = st.owner(st.find(team_id));
        const int32_t new_owner = old_owner == 0 ? 1 : 0;
        std::vector<std::pair<objects::ObjectId, int32_t>> others;  // every other team's members and their owners
        for (const auto& [tid, m] : teams_now(cs))
            if (static_cast<objects::ObjectId>(tid) != team_id)
                for (const auto id : m) others.emplace_back(id, st.owner(st.find(id)));
        CHECK(!others.empty());
        CHECK(battles.change_owner(st.find(members.back()), new_owner));
        CHECK(st.owner(st.find(team_id)) == new_owner);  // the container
        for (const auto id : members) CHECK(st.owner(st.find(id)) == new_owner && cs.owner(id) == new_owner);
        for (const auto& [id, o] : others) CHECK(st.owner(st.find(id)) == o && cs.owner(id) == o);
        // A Change_Owner of the container itself moves every member back.
        CHECK(battles.change_owner(st.find(team_id), old_owner));
        CHECK(st.owner(st.find(team_id)) == old_owner);
        for (const auto id : members) CHECK(st.owner(st.find(id)) == old_owner && cs.owner(id) == old_owner);
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
        // The AI VMs' Change_Owner (BattleAi object bridge) on another team's lowest-id member.
        if (!others.empty()) {
            ai::AiDefinitions defs;
            ai::EquationSet eqs;
            TacticalAiSetup ts;
            ts.defs = &defs;
            ts.equations = &eqs;
            ts.files = d->files.get();
            ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
            BattleAi ai(*b, *tm, host, ts);
            lua::ObjectBridge& br = ai.world().object_bridge();
            CHECK(static_cast<bool>(br.change_owner));
            if (br.change_owner) {
                const objects::ObjectId victim = others.front().first;
                const auto oteam = static_cast<objects::ObjectId>(cs.team(victim));
                const int32_t to = others.front().second == 0 ? 1 : 0;
                CHECK(br.change_owner(st.find(victim), to));
                CHECK(st.owner(st.find(oteam)) == to);
                for (const auto& [id, o] : others) {
                    const bool same = static_cast<objects::ObjectId>(cs.team(id)) == oteam;
                    CHECK(st.owner(st.find(id)) == (same ? to : o) && cs.owner(id) == (same ? to : o));
                }
                for (const auto id : members) CHECK(st.owner(st.find(id)) == old_owner);
            }
        }
#endif
        done = true;
    }
    CHECK(done);
}

// s78n t03 / t09 (TACTICAL_COMBAT §5.4.4 items 1 - 2, MOVEMENT §3.7, FIGHTER_FLYING §7): squad_types - the Team type on
// LAND_TEAM_CONTAINER_LOCOMOTOR, troopers sized as Squad_Stormtrooper (half-extent 2.0 x 1.5 = 3.0), the container's
// MaxJiggleDistance 0.75. In a land battle each squad container is a mover of its own:
//  * t03: its members hold slots around it (MovementSystem::land_team_slots); a move ordered on the members moves the
//    container along its own straight path (open map) - the store position every reader takes is the mover's, never the
//    members' box centre; the members steer from the container's state at the start of the tick, so they trail their
//    slots by about one frame of the team speed (the container leads) and hold the formation; the container arrives and
//    the members settle within 4.0 of their slot points.
//  * t09: Bribe's E_T input (TacticalBattle::squad_joint_box) is the joint box of the members' slots +- 3.0 from the
//    container's point (0, 0) plus J = 0.75.
void test_land_squad_container() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.skip_human_units = true;  // the stand-in leaves the human's squads to this test's orders
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const objects::TypeIndex team_t = d->types.resolve_name("Team");
    CHECK(team_t != objects::kNone);
    namespace tc = alamo::tactical::combat;
    namespace mv = alamo::tactical::movement;
    bool invaded = false, done = false;
    int stage = 0;
    uint32_t order_frame = 0, settle_frame = 0;
    objects::ObjectId team_id = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    std::vector<std::pair<mv::ObjectId, mv::V2>> slots;
    Vec3 start{}, goal{};
    float off_line = 0.0f, max_lead = 0.0f, team_speed = 0.0f, late_along = 0.0f, settle_err = 0.0f;
    uint32_t store_mismatch = 0, moving_frames = 0, recover_frames = 0;
    float start_error = -1.0f, recovered_error = -1.0f;
    for (uint64_t t = 0; t < 8000 && !done; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        const mv::MovementSystem& ms = b->movement();
        if (stage == 0) {  // a live team of the human player (0) with >= 2 members
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
            }
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() >= 2 && team_id == objects::kNoObjectId && st.alive(th) && st.owner(th) == 0) {
                    team_id = static_cast<objects::ObjectId>(tid);
                    members = m;
                }
            }
            CHECK(team_id != objects::kNoObjectId);
            if (team_id == objects::kNoObjectId) break;
            const objects::ObjectHandle th = st.find(team_id);
            CHECK(st.type(th) == team_t && ms.has_unit(team_id));
            slots = ms.land_team_slots(team_id);
            CHECK(slots.size() == members.size());
            for (size_t k = 0; k < slots.size() && k < members.size(); ++k) {
                mv::ObjectId c = -1;
                mv::V2 sl;
                CHECK(slots[k].first == members[k] && ms.team_slot(members[k], c, sl) && c == team_id && sl == slots[k].second);
            }
            // t09: the joint box of the slots +- 3.0 (from the container's point) + J 0.75
            float lx = 0.0f, hx = 0.0f, ly = 0.0f, hy = 0.0f;
            for (const auto& [id, sl] : slots) {
                lx = std::min(lx, sl.x - 3.0f);
                hx = std::max(hx, sl.x + 3.0f);
                ly = std::min(ly, sl.y - 3.0f);
                hy = std::max(hy, sl.y + 3.0f);
            }
            float ex = 0.0f, ey = 0.0f;
            CHECK(b->squad_joint_box(st, team_id, ex, ey));
            const float wx = (hx - lx) * 0.5f + 0.75f, wy = (hy - ly) * 0.5f + 0.75f;
            CHECK(std::fabs(ex - wx) < 1e-4f && std::fabs(ey - wy) < 1e-4f && ex >= 3.75f && ey >= 3.75f);
            float nx = 0.0f, ny = 0.0f;
            CHECK(!b->squad_joint_box(st, members[0], nx, ny));  // a member is no squad container
            std::printf("  land squad: team %d, %zu members, slots", static_cast<int>(team_id), slots.size());
            for (const auto& [id, sl] : slots) std::printf(" (%.2f, %.2f)", static_cast<double>(sl.x), static_cast<double>(sl.y));
            std::printf("; joint box E_T %.3f x %.3f\n", static_cast<double>(ex), static_cast<double>(ey));
            stage = 3;
            continue;
        }
        bool all_alive = true;
        for (const auto id : members) all_alive = all_alive && cs.alive(id);
        if (!all_alive) {
            std::printf("  land squad: a member died in stage %d, skipped\n", stage);
            break;
        }
        const objects::ObjectHandle th = st.find(team_id);
        const mv::UnitView k = ms.view(team_id);
        const Vec3 p = st.position(th);
        if (p.x != k.pos.x || p.y != k.pos.y) ++store_mismatch;  // every reader takes the store position: the mover's
        const float c = std::cos(k.yaw * 0.017453292f), s = std::sin(k.yaw * 0.017453292f);
        // the members' largest distance from their slot points (container + R(yaw) (slot + jiggle))
        const auto slot_error = [&]() {
            float e = 0.0f;
            for (size_t m = 0; m < members.size(); ++m) {
                const mv::UnitView v = ms.view(members[m]);
                const mv::V2 off = slots[m].second + v.jiggle;
                const float tx = k.pos.x + c * off.x - s * off.y, ty = k.pos.y + s * off.x + c * off.y;
                e = std::max(e, std::sqrt((v.pos.x - tx) * (v.pos.x - tx) + (v.pos.y - ty) * (v.pos.y - ty)));
            }
            return e;
        };
        if (stage == 3) {  // the drop placed the members off their slots: they recover to them while the team is idle
            if (start_error < 0.0f) start_error = slot_error();
            bool idle = !k.has_goal;
            for (const auto id : members) idle = idle && !ms.view(id).has_goal;
            if (!idle) {
                if (++recover_frames > 900) break;
                continue;
            }
            recovered_error = slot_error();
            // t03: a move on the members moves the container 300 ahead along its facing (no turn in place)
            start = p;
            const Vec3 f = st.facing(th);
            CHECK(b->order_move(*tm, 0, members, start.x + f.x * 300.0f, start.y + f.y * 300.0f, host.random().synced()) ==
                  commands::RejectReason::None);
            CHECK(ms.view(team_id).has_goal && ms.view(members[0]).has_goal && ms.view(members[0]).goal == ms.view(team_id).goal);
            goal = Vec3{ms.view(team_id).goal.x, ms.view(team_id).goal.y, start.z};
            order_frame = b->frame();
            stage = 1;
            continue;
        }
        if (stage == 1) {
            if (k.has_goal) {
                ++moving_frames;
                // the container's own straight path: its distance from the start -> goal line
                const float gx = goal.x - start.x, gy = goal.y - start.y, gl = std::sqrt(gx * gx + gy * gy);
                off_line = std::max(off_line, std::fabs((p.x - start.x) * gy - (p.y - start.y) * gx) / gl);
                team_speed = std::max(team_speed, k.speed);
                for (size_t m = 0; m < members.size(); ++m) {
                    const mv::UnitView v = ms.view(members[m]);
                    const float along = slots[m].second.x - (c * (v.pos.x - k.pos.x) + s * (v.pos.y - k.pos.y));
                    max_lead = std::max(max_lead, along);  // > 0: the member is behind its slot (the container leads)
                    if (b->frame() > order_frame + 60) late_along = std::max(late_along, std::fabs(along));
                }
            } else if (b->frame() > order_frame + 5) {
                CHECK(k.arrived);
                stage = 2;
                settle_frame = b->frame();
            }
            continue;
        }
        if (b->frame() < settle_frame + 120) continue;  // members recover to their slots once the team stopped
        settle_err = slot_error();
        for (const auto id : members) CHECK(!ms.view(id).has_goal);
        done = true;
    }
    CHECK(done);
    std::printf("  land squad: dropped up to %.2f off their slots, recovered to %.2f in %u frames\n", static_cast<double>(start_error),
                static_cast<double>(recovered_error), recover_frames);
    std::printf("  land squad: moving %u frames, off its line <= %.3f, members up to %.2f behind their slots at the team speed %.3f, "
                "along error after 2 s <= %.2f, settled within %.2f; store = mover %d\n",
                moving_frames, static_cast<double>(off_line), static_cast<double>(max_lead), static_cast<double>(team_speed),
                static_cast<double>(late_along), static_cast<double>(settle_err), store_mismatch == 0 ? 1 : 0);
    if (!done) return;
    CHECK(store_mismatch == 0);
    CHECK(start_error > 4.0f && recovered_error >= 0.0f && recovered_error <= 4.0f);  // idle recovery (> 4.0 from the slot)
    CHECK(moving_frames > 60 && off_line < 0.5f);
    CHECK(max_lead > 1.0f);   // the container leads (its members steer from its start-of-tick state)
    CHECK(late_along < 4.0f);  // and the members hold the formation
    CHECK(settle_err <= 4.0f);
}

// s78k t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" steps 3-5): a converting blaster (convert_types) turns
// enemy team members in a land battle. CombatSystem converts the victim's whole team (combat owner, one
// UnitEffect::Converted each); the host follows each event (store owner) and its TEAM container - the store owner and the
// container record (TacticalBattle::container_owner). At every tick end each live team has one owner: its members (combat
// and store), its container object and its container record. At least one team must change owner.
void test_team_convert() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace tc = alamo::tactical::combat;
    bool invaded = false;
    uint32_t ticks = 0, team_converts = 0, member_converts = 0, mismatches = 0;
    std::map<uint32_t, int32_t> team_owner;           // last seen container owner per team
    std::map<objects::ObjectId, int32_t> unit_owner;  // last seen owner per team member
    for (uint64_t t = 0; t < 6000; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        const TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        ++ticks;
        const objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        std::map<uint32_t, std::vector<objects::ObjectId>> teams;
        for (size_t i = 0; i < cs.unit_count(); ++i) {
            const auto id = cs.unit_id_at(i);
            if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
        }
        for (const auto& [tid, m] : teams) {
            const auto cid = static_cast<objects::ObjectId>(tid);
            const objects::ObjectHandle ch = st.find(cid);
            if (!st.alive(ch)) continue;
            const int32_t o = cs.owner(m.front());
            bool same = st.owner(ch) == o && b->container_owner(cid) == o;
            for (const auto id : m) {
                same = same && cs.owner(id) == o && st.owner(st.find(id)) == o;
                const auto [it, fresh] = unit_owner.emplace(id, o);
                if (!fresh && it->second != o) {
                    ++member_converts;
                    it->second = o;
                }
            }
            if (!same && mismatches++ < 3)
                std::printf("  team convert: frame %u team %u owner %d: container store %d record %d\n", b->frame(), tid, o,
                            st.owner(ch), b->container_owner(cid));
            const auto [it, fresh] = team_owner.emplace(tid, st.owner(ch));
            if (!fresh && it->second != st.owner(ch)) {
                ++team_converts;
                it->second = st.owner(ch);
            }
        }
    }
    std::printf("  team convert: %u battle ticks, %zu teams, %u container owner changes, %u member owner changes, %u mismatches\n",
                ticks, team_owner.size(), team_converts, member_converts, mismatches);
    CHECK(ticks > 0 && !team_owner.empty());
    CHECK(team_converts > 0);
    CHECK(mismatches == 0);
}

// s78l t04 (TACTICAL_COMBAT §3.5 "Convert and its team rule" step 4, 0x38e5a0; ABILITIES_HARDPOINTS Hack / Tactical_Bribe):
// Hack and Tactical_Bribe change the target's owner through Change_Owner, which propagates through the team by itself (a
// member -> its TEAM container -> every member in list order), so the container and ALL its members change owner,
// whatever their position. The Empire's slicer (hack_types) hacks / bribes one member of an Underworld team: afterwards the
// container (store owner and record) and every live member (store and combat) belong to the Empire, every other team keeps
// its owner, and at every later tick end each team has one owner. Hack changes the owner at its animation end (60 frames
// without a clip), Tactical_Bribe at once.
void test_team_hack_bribe(bool bribe) {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup setup = make_setup(*d, true);
    const uint32_t slicer_co = type_of(*d, "S10_Slicer_Co"), slicer_t = type_of(*d, "S10_Slicer");
    CHECK(slicer_co < d->types.size() && slicer_t < d->types.size());
    setup.forces.push_back({0, 1, slicer_co});  // the Empire's defenders: S10_Squad + the slicer
    if (!setup.players.empty()) setup.players[0].credits = 5000.0f;  // s78l t05: the bribing Empire pays the cost
    galactic::GalacticSim* gs = host.start_galactic(d->rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    float credits_at_order = -1.0f, credits_at_change = -1.0f;
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace tc = alamo::tactical::combat;
    const uint8_t kind = tactical::abilities::ability_type_from_name(bribe ? "TACTICAL_BRIBE" : "TARGETED_HACK");
    auto teams_now = [](const tc::CombatSystem& cs) {
        std::map<uint32_t, std::vector<objects::ObjectId>> out;
        for (size_t i = 0; i < cs.unit_count(); ++i) {
            const auto id = cs.unit_id_at(i);
            if (cs.team(id) != 0 && cs.alive(id)) out[cs.team(id)].push_back(id);
        }
        return out;
    };
    bool invaded = false, ordered = false, changed = false;
    int stage = 0;
    uint32_t order_frame = 0, change_frame = 0, after = 0, mismatches = 0;
    objects::ObjectId slicer = objects::kNoObjectId, victim = objects::kNoObjectId, team_id = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    std::vector<std::pair<objects::ObjectId, int32_t>> others;  // every other team member and its owner at the order
    for (uint64_t t = 0; t < 6000 && stage < 3; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;  // the battle ended
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        if (stage == 0) {  // the slicer and an Underworld team of >= 2 live members (its lowest-id member is the target)
            for (size_t i = 0; i < cs.unit_count() && slicer == objects::kNoObjectId; ++i) {
                const auto id = cs.unit_id_at(i);
                const objects::ObjectHandle h = st.find(id);
                if (cs.alive(id) && st.alive(h) && st.type(h) == slicer_t) slicer = id;
            }
            for (const auto& [tid, m] : teams_now(cs))
                if (team_id == objects::kNoObjectId && m.size() >= 2 && cs.owner(m.front()) == 1) {
                    team_id = static_cast<objects::ObjectId>(tid);
                    members = m;
                }
            CHECK(slicer != objects::kNoObjectId && team_id != objects::kNoObjectId);
            if (slicer == objects::kNoObjectId || team_id == objects::kNoObjectId) break;
            CHECK(b->abilities().has_ability(slicer, kind));
            victim = members.front();
            CHECK(st.owner(st.find(team_id)) == 1 && b->container_owner(team_id) == 1);
            for (const auto& [tid, m] : teams_now(cs))
                if (static_cast<objects::ObjectId>(tid) != team_id)
                    for (const auto id : m) others.emplace_back(id, cs.owner(id));
            const objects::ObjectId users[] = {slicer};
            ordered = b->abilities().targeted_ability(users, kind, victim, -1, b->frame()) == 1u;
            CHECK(ordered);
            if (!ordered) break;
            order_frame = b->frame();
            credits_at_order = gs->credits(0);
            stage = 1;
            continue;
        }
        if (stage == 1) {  // wait for the owner change (Bribe: the order's own pass; Hack: the 60-frame animation end)
            if (!cs.alive(victim)) {
                std::printf("  team %s: the victim died before the owner change\n", bribe ? "bribe" : "hack");
                break;
            }
            if (cs.owner(victim) != 0) {
                if (b->frame() >= order_frame + 200u) break;
                continue;
            }
            changed = true;
            change_frame = b->frame();
            credits_at_change = gs->credits(0);
            // The container (store + record) and every live member (store + combat), whatever their distance.
            CHECK(st.owner(st.find(team_id)) == 0 && b->container_owner(team_id) == 0);
            for (const auto id : members)
                if (cs.alive(id)) CHECK(st.owner(st.find(id)) == 0 && cs.owner(id) == 0);
            // No other team / unit changed.
            for (const auto& [id, o] : others)
                if (cs.alive(id)) CHECK(cs.owner(id) == o && st.owner(st.find(id)) == o);
            stage = 2;
            continue;
        }
        // stage 2: every team keeps one owner on each later tick end (30 ticks)
        for (const auto& [tid, m] : teams_now(cs)) {
            const auto cid = static_cast<objects::ObjectId>(tid);
            const objects::ObjectHandle ch = st.find(cid);
            if (!st.alive(ch)) continue;
            const int32_t o = cs.owner(m.front());
            bool same = st.owner(ch) == o && b->container_owner(cid) == o;
            for (const auto id : m) same = same && cs.owner(id) == o && st.owner(st.find(id)) == o;
            if (!same) ++mismatches;
        }
        if (++after >= 30) stage = 3;
    }
    std::printf("  team %s: slicer %d -> member %d of team %d (%zu members): owner change %d at frame +%u, %u mismatches later\n",
                bribe ? "bribe" : "hack", static_cast<int>(slicer), static_cast<int>(victim), static_cast<int>(team_id), members.size(),
                changed ? 1 : 0, change_frame - order_frame, mismatches);
    CHECK(ordered && changed && stage == 3 && mismatches == 0);
    if (bribe) CHECK(change_frame <= order_frame + 2u);  // Tactical_Bribe changes the owner at once (no animation wait)
    else CHECK(change_frame >= order_frame + 60u);       // Hack: at the end of its animation [port: 60 frames without a clip]
    // s78l t05: the bribe was paid from the Empire's credits (Tactical_Bribe_Cost x modifier); the hack costs nothing.
    std::printf("  team %s: credits %.1f at the order, %.1f at the owner change\n", bribe ? "bribe" : "hack",
                static_cast<double>(credits_at_order), static_cast<double>(credits_at_change));
    if (bribe) CHECK(credits_at_order > 0.0f && credits_at_change < credits_at_order);
    else CHECK(credits_at_change == credits_at_order);
}

// s78n t05 (HERO_CLASH §8.1 / §8.2): the Underworld's vehicle thief steals the Empire's speeder. The capture variant is
// spawned with the speeder's owner, then the boarding changes its owner to the thief's (store, combat, abilities), sets
// the permanent stolen-vehicle flag and the incognito flag, and the thief is inside it. While incognito no unit
// auto-acquires the variant; the stolen flag survives a later owner change.
void test_vehicle_thief() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup setup = make_setup(*d, true);
    const uint32_t thief_co = type_of(*d, "S10_Thief_Co"), thief_t = type_of(*d, "S10_Thief");
    const uint32_t speeder_co = type_of(*d, "S10_Speeder_Co"), speeder_t = type_of(*d, "S10_Speeder"),
                   stolen_t = type_of(*d, "S10_Speeder_Stolen");
    CHECK(thief_co < d->types.size() && speeder_co < d->types.size() && stolen_t < d->types.size());
    setup.forces.push_back({0, 1, speeder_co});  // the Empire's defenders + the speeder
    // the Underworld's invaders, the thief first (an invader's later companies may wait in reserve)
    setup.forces.insert(setup.forces.begin(), {1, 2, thief_co});
    galactic::GalacticSim* gs = host.start_galactic(d->rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace tc = alamo::tactical::combat;
    const uint8_t kind = tactical::abilities::ability_type_from_name("CAPTURE_VEHICLE");
    bool invaded = false;
    int stage = 0;
    uint32_t order_frame = 0, watched = 0, acquired = 0;
    objects::ObjectId thief = objects::kNoObjectId, speeder = objects::kNoObjectId, stolen = objects::kNoObjectId;
    for (uint64_t t = 0; t < 6000 && stage < 3; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;  // the battle ended
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        if (stage == 0) {  // the thief and the speeder
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                const objects::ObjectHandle h = st.find(id);
                if (!cs.alive(id) || !st.alive(h)) continue;
                if (st.type(h) == thief_t) thief = id;
                if (st.type(h) == speeder_t) speeder = id;
            }
            CHECK(thief != objects::kNoObjectId && speeder != objects::kNoObjectId);
            if (thief == objects::kNoObjectId || speeder == objects::kNoObjectId) break;
            CHECK(cs.owner(thief) == 1 && cs.owner(speeder) == 0 && b->abilities().has_ability(thief, kind));
            const objects::ObjectId users[] = {thief};
            const bool ordered = b->abilities().targeted_ability(users, kind, speeder, -1, b->frame()) == 1u;
            CHECK(ordered);
            if (!ordered) break;
            order_frame = b->frame();
            stage = 1;
            continue;
        }
        if (stage == 1) {  // the capture variant appears (the speeder is gone)
            for (size_t i = 0; i < cs.unit_count() && stolen == objects::kNoObjectId; ++i) {
                const auto id = cs.unit_id_at(i);
                const objects::ObjectHandle h = st.find(id);
                if (cs.alive(id) && st.alive(h) && st.type(h) == stolen_t) stolen = id;
            }
            if (stolen == objects::kNoObjectId) {
                if (b->frame() >= order_frame + 60u) break;
                continue;
            }
            CHECK(!cs.alive(speeder));
            CHECK(st.owner(st.find(stolen)) == 1 && cs.owner(stolen) == 1);
            CHECK((cs.flags(stolen) & tc::kUnitStolenVehicle) != 0 && (cs.flags(stolen) & tc::kUnitIncognito) != 0);
            CHECK(b->garrison_holder_of(thief) == stolen);
            stage = 2;
            continue;
        }
        // stage 2: while incognito, no unit auto-acquires the stolen vehicle (60 ticks)
        if (cs.alive(stolen) && (cs.flags(stolen) & tc::kUnitIncognito)) {
            ++watched;
            for (size_t i = 0; i < cs.unit_count(); ++i)
                if (cs.auto_target(cs.unit_id_at(i)) == stolen) ++acquired;
        }
        if (b->frame() >= order_frame + 90u) {
            // the stolen flag survives an owner change (Change_Owner keeps +0x3a2)
            if (cs.alive(stolen)) {
                CHECK(b->script_change_owner(*tm, st.find(stolen), 0));
                CHECK(cs.owner(stolen) == 0 && (cs.flags(stolen) & tc::kUnitStolenVehicle) != 0);
            }
            stage = 3;
        }
    }
    std::printf("  vehicle thief: thief %d -> speeder %d, variant %d; %u incognito ticks watched, %u acquisitions\n",
                static_cast<int>(thief), static_cast<int>(speeder), static_cast<int>(stolen), watched, acquired);
    CHECK(stage == 3 && stolen != objects::kNoObjectId && watched > 0 && acquired == 0);
}

// s78j t04 (TACTICAL_COMBAT §3.1.1 "other spawns"): a land reserve drop without a landing object (S10_Squad has no
// Company_Transport_Unit here) spawns its members - and their TEAM containers - with the owner's entry-facing record
// (TacticalBattle::player_drop_facing, the reinforce_facing hook of the Lua bridges), not the side's start facing.
// s78k t05 (attacker_marker = false): without an Attacker Entry Position the attacker's record stays unset (yaw 0, +x)
// while its start forces face the defender's anchor (the [port] rule for a side without its marker), so the drop's
// facing differs from the side's start facing and the test tells the two apart.
void test_reserve_drop_facing(bool attacker_marker) {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [attacker_marker](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                // Entry markers (yaw 180 = facing +y): the attacker has start units, so its entry-facing
                                // record is the marker's (TACTICAL_GAPS §4.2.2 item 1), not the yaw-0 default.
                                auto place = [&m](const char* type, float x, float y, float yaw) {
                                    terrain::ObjectPlacement p;
                                    p.type_crc = name_crc(type);
                                    p.owner = 2;
                                    p.position[0] = x;
                                    p.position[1] = y;
                                    p.rotation_deg[2] = yaw;
                                    m.objects.push_back(p);
                                };
                                place("Defending Forces Position", 900.0f, 900.0f, 0.0f);
                                if (attacker_marker) place("Attacker Entry Position", 300.0f, 300.0f, 180.0f);
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace mv = alamo::tactical::movement;
    const objects::TypeIndex trooper = d->types.resolve_name("S10_Trooper"), team_t = d->types.resolve_name("Team");
    bool invaded = false, dropped = false, ran = false;
    for (uint64_t t = 0; t < 1500 && !ran; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() != 1) continue;
        ran = true;
        objects::ObjectStore& st = tm->primary();
        const auto reserves = b->reserves(1);
        CHECK(reserves.size() == 1);
        if (reserves.empty()) break;
        const objects::ObjectId key = reserves[0].galactic_id;
        if (b->fog()) b->fog()->reveal_all(1);  // the drop test needs unfogged cells
        // The side's start facing (player 1's troopers already on the map), printed for reference.
        std::vector<objects::ObjectId> before;
        Vec3 start_face{};
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h)) {
                before.push_back(st.id(h));
                if (st.owner(h) == 1 && st.type(h) == trooper) start_face = st.facing(h);
            }
        std::sort(before.begin(), before.end());
        const uint32_t spawned_before = b->stats().spawned;
        for (int32_t cy = 6; cy < 58 && !dropped; cy += 2)
            for (int32_t cx = 6; cx < 58 && !dropped; cx += 2) {
                const mv::V2 c = mv::cell_center(cx, cy);
                if (!b->land_drop_valid(*tm, 1, key, c.x, c.y, false, false)) continue;
                if (b->order_reinforce(*tm, 1, key, c.x, c.y, host.random().synced()) != commands::RejectReason::None) continue;
                dropped = true;
            }
        CHECK(dropped);
        if (!dropped) break;
        CHECK(b->landings_unloaded().empty() && b->landings_in_flight(1) == 0);  // no landing object
        CHECK(b->stats().spawned > spawned_before);
        const Vec3 df = b->player_drop_facing(1);
        size_t new_troopers = 0, new_teams = 0;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.alive(h) || std::binary_search(before.begin(), before.end(), st.id(h))) continue;
            if (st.type(h) != trooper && st.type(h) != team_t) continue;
            const Vec3 f = st.facing(h);
            CHECK(f.x == df.x && f.y == df.y && f.z == df.z);
            if (st.type(h) == trooper) ++new_troopers;
            else ++new_teams;
        }
        std::printf("  reserve drop facing: %zu troopers, %zu teams at (%.3f, %.3f); side start facing (%.3f, %.3f)\n", new_troopers,
                    new_teams, static_cast<double>(df.x), static_cast<double>(df.y), static_cast<double>(start_face.x),
                    static_cast<double>(start_face.y));
        CHECK(new_troopers == reserves[0].units && new_teams > 0);
        if (attacker_marker) {
            CHECK(std::fabs(df.x) < 1e-5f && std::fabs(df.y - 1.0f) < 1e-5f);  // the marker's facing, not the yaw-0 default
        } else {
            CHECK(df.x == 1.0f && std::fabs(df.y) < 1e-6f);  // no record: yaw 0
            // The side's start facing points at the defender's anchor (900, 900), well off +x.
            CHECK(std::fabs(start_face.y) > 0.2f && std::fabs(start_face.x - df.x) + std::fabs(start_face.y - df.y) > 0.2f);
        }
    }
    CHECK(dropped);
}

// a1g t04 follow-up: BattleAi::land_drop_ok (TACTICAL_GAPS §4.2): the land Reinforce drop validity of a reserve entry. A spot
// is refused when a slot is impassable (an Obstacle strip on the map), fogged for the player (force 1), or its footprint is
// taken (a live unit stands there); an unknown entry is refused; otherwise accepted.
// a1h t06: the test is run by the host (TacticalBattle::land_drop_valid); blockers are the Collidable_By_Projectile_Living
// objects and S10_Trooper carries hard extents 6 x 6 (a type with neither extents nor a model has a zero drop cell: refused).
void test_battle_ai_land_drop(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    constexpr int32_t kStrip = 5;  // cells x < 5: Obstacle
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                m.legacy_passability.assign(63u * 63u, 0);
                                for (uint32_t x = 0; x < 5u; ++x)  // kStrip
                                    for (uint32_t y = 0; y < 63u; ++y) m.legacy_passability[x * 63u + y] = 2;
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d.files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };
    ts.constants.use_reinforcement_points = false;  // the synthetic map has no reinforcement points (enabling tested below)
    namespace mv = alamo::tactical::movement;
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 1500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() != 1) continue;
        checked = true;
        const auto reserves = b->reserves(1);
        CHECK(reserves.size() == 1);
        if (reserves.empty()) break;
        const auto key = static_cast<int32_t>(reserves[0].galactic_id);
        BattleAi ai(*b, *tm, host, ts);
        // Candidate spots over the map (every 4th cell centre right of the strip).
        auto scan = [&](std::vector<ai::tactical::Vec3f>& ok) {
            ok.clear();
            for (int32_t cy = 4; cy < 60; cy += 4)
                for (int32_t cx = kStrip + 6; cx < 58; cx += 4) {
                    const mv::V2 c = mv::cell_center(cx, cy);
                    if (ai.land_drop_ok(1, key, {c.x, c.y, 0.0f})) ok.push_back({c.x, c.y, 0.0f});
                }
        };
        std::vector<ai::tactical::Vec3f> fog_ok, clear_ok;
        scan(fog_ok);
        size_t newly = 0;
        const bool fog = b->fog() != nullptr;
        if (fog) {
            b->fog()->reveal_all(1);  // nothing fogged for player 1 any more
            scan(clear_ok);
            for (const auto& q : clear_ok)
                newly += std::none_of(fog_ok.begin(), fog_ok.end(), [&q](const ai::tactical::Vec3f& e) { return e.x == q.x && e.y == q.y; });
            CHECK(newly > 0);           // spots refused only because they were fogged
            CHECK(clear_ok.size() > fog_ok.size());
        } else {
            clear_ok = fog_ok;
        }
        CHECK(!clear_ok.empty());
        for (const auto& q : fog_ok) CHECK(ai.land_drop_ok(1, key, q));  // fog cleared: still valid (and same answer twice)
        const mv::V2 strip = mv::cell_center(2, 31);
        CHECK(!ai.land_drop_ok(1, key, {strip.x, strip.y, 0.0f}));  // impassable slots
        // a1h t06 (§4.2.2 item 5): the Reinforce command re-runs the test and refuses the whole drop (no clamp, no move).
        CHECK(b->order_reinforce(*tm, 1, reserves[0].galactic_id, strip.x, strip.y, host.random().synced()) ==
              commands::RejectReason::InvalidPosition);
        CHECK(b->reserves(1).size() == 1);
        // TACTICAL_GAPS §4.2.1: an AI drop with a member slot on an enemy unit passes (the AI ignores movers); the same
        // drop for a human fails (its movers block). Slot 0's offset comes from the shared layout.
        const LandDropLayout at0 = land_drop_layout(b->land_drop_shape(reserves[0].type), 0.0f, 0.0f, b->player_drop_yaw(1));
        CHECK(at0.ok);
        size_t next_to_enemy = 0;
        for (const objects::ObjectHandle h : tm->primary().owned_by(0)) {
            if (!at0.ok || !tm->primary().active(h) || !tm->movable(h) || next_to_enemy >= 3) continue;
            const Vec3 q = tm->primary().position(h);
            const ai::tactical::Vec3f c{q.x - at0.slots[0].x, q.y - at0.slots[0].y, 0.0f};
            if (!ai.land_drop_ok(1, key, c)) continue;  // refused for another reason (passability, radius, a structure)
            CHECK(!ai.land_drop_valid(1, key, c, true));
            ++next_to_enemy;
        }
        CHECK(next_to_enemy > 0);
        {  // Use_Reinforcement_Points: a spot is valid only when every slot is within an allied enabling radius.
            TacticalAiSetup te = ts;
            te.constants.use_reinforcement_points = true;
            BattleAi ae(*b, *tm, host, te);
            size_t enabled = 0;
            for (const auto& q : clear_ok) {
                const bool ok = ae.land_drop_ok(1, key, q);
                enabled += ok ? 1u : 0u;
                if (ok) CHECK(ai.land_drop_ok(1, key, q));  // the radii only remove spots
            }
            std::printf("  land drop: %zu of %zu spots inside the enabling radii\n", enabled, clear_ok.size());
        }
        if (!clear_ok.empty()) {
            CHECK(!ai.land_drop_ok(1, key + 1000, clear_ok[0]));  // unknown entry
            CHECK(!ai.land_drop_ok(0, key, clear_ok[0]));         // another player's entry
        }
        std::printf("  land drop: %zu / %zu spots fogged-valid / valid, %zu refused only by fog (fog %s), %zu next to enemy units\n",
                    fog_ok.size(), clear_ok.size(), newly, fog ? "on" : "off", next_to_enemy);
        // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): the reinforcement facing of a side = its entry-facing record
        // (+X when unset or for a player without a side); test_landing_drop checks a landing object against it.
        const Vec3 df = b->player_drop_facing(1);
        CHECK(df.z == 0.0f && std::fabs(df.x * df.x + df.y * df.y - 1.0f) < 1e-5f);
        CHECK(b->player_drop_facing(7).x == 1.0f && b->player_drop_facing(7).y == 0.0f && b->player_drop_facing(7).z == 0.0f);
    }
    CHECK(checked);
}

// a2h t05: load_tactical_ai_data, the battle hosts' loader (alamo_headless --skirmish-ai / galactic --battles, alamo_game),
// fills TacticalAiSetup::constants from GameConstants; a missing tag, a bad value or no GameConstants keeps the default.
void test_tactical_ai_constants_load() {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_tactical_ai_constants";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<GameObjectFiles></GameObjectFiles>\n");
    write_file(root / "Data/XML/TradeRouteFiles.xml", "<TradeRouteFiles></TradeRouteFiles>\n");
    write_file(root / "Data/XML/CampaignFiles.xml", "<CampaignFiles></CampaignFiles>\n");
    write_file(root / "Data/XML/GameConstants.xml",
               "<GameConstants>\n <Production_Speed_Factor>2.5</Production_Speed_Factor>\n"
               " <Max_Ground_Forces_On_Planet> 7 </Max_Ground_Forces_On_Planet>\n"
               " <AI_LandEvaluatorRegionSize>1500.0</AI_LandEvaluatorRegionSize>\n"
               " <AIUsesFogOfWarLand> True </AIUsesFogOfWarLand>\n"
               " <Use_Reinforcement_Points>False</Use_Reinforcement_Points>\n"
               " <Health_Low_Percent_Threshold>not a number</Health_Low_Percent_Threshold>\n</GameConstants>\n");
    DirectoryProvider dp(path_to_utf8(root));
    data::GameDatabase db;
    std::vector<std::string> diag;
    CHECK(db.load(dp, {}, &diag));
    const ai::tactical::TacticalAiConstants def;
    TacticalAiSetup s;
    s.land_plans = {"keep.lua"};
    load_tactical_ai_data(s, dp, &db.constants(), {});  // no lister: the plan lists stay
    const ai::tactical::TacticalAiConstants& c = s.constants;
    std::printf("  tactical AI constants: production_speed_factor %.2f max_ground_forces %d land_region %.0f fow_land %d rp %d health_low %.2f\n",
                static_cast<double>(c.production_speed_factor), c.max_ground_forces_on_planet, static_cast<double>(c.land_region_size),
                c.ai_uses_fow_land ? 1 : 0, c.use_reinforcement_points ? 1 : 0, static_cast<double>(c.health_low_percent_threshold));
    CHECK(c.production_speed_factor == 2.5f && c.max_ground_forces_on_planet == 7 && c.land_region_size == 1500.0f);
    CHECK(c.ai_uses_fow_land && !c.use_reinforcement_points);
    CHECK(c.health_low_percent_threshold == def.health_low_percent_threshold);  // unparsable: the default
    CHECK(c.space_region_size == def.space_region_size && c.high_threat_tolerance == def.high_threat_tolerance &&
          c.ai_uses_fow_space == def.ai_uses_fow_space);  // missing tags: the defaults
    CHECK(s.land_plans.size() == 1 && s.space_plans.empty());
    TacticalAiSetup n;  // no GameConstants: every default; an empty lister empties the plan lists
    n.land_plans = {"drop.lua"};
    load_tactical_ai_data(n, dp, nullptr, [](std::string_view) { return std::vector<std::string>(); });
    CHECK(n.constants.production_speed_factor == def.production_speed_factor &&
          n.constants.max_ground_forces_on_planet == def.max_ground_forces_on_planet &&
          n.constants.use_reinforcement_points == def.use_reinforcement_points);
    CHECK(n.land_plans.empty() && n.space_plans.empty());
    remove_all_long(root, ec);
}
#endif

}  // namespace

void test_landing_drop(bool timed, uint32_t destroy_at = 0) {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, true);
    const objects::TypeIndex lander = d->types.resolve_name("S10_Lander"), landing_t = d->types.resolve_name("S10_Lander_Landing");
    CHECK(lander != objects::kNone && landing_t != objects::kNone);
    if (!timed) {
        float hx = 0.0f, hy = 0.0f;
        drop_half_extents(d->types, d->catalog.get(), lander, hx, hy);
        CHECK(hx == 20.0f && hy == 15.0f);
        drop_deployed_extents(d->types, d->catalog.get(), lander, hx, hy);
        CHECK(hx == 30.0f && hy == 0.0f);  // 0x371d20: y from Custom_Hard_YExtent_Deployed whenever the deployed x is set
        drop_deployed_extents(d->types, d->catalog.get(), static_cast<uint32_t>(d->types.resolve_name("S10_Trooper")), hx, hy);
        CHECK(hx == 6.0f && hy == 6.0f);  // no deployed x: the normal extents
    }
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 900;
    opt.battle.landing_unload_timed = timed;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace mv = alamo::tactical::movement;
    bool invaded = false, dropped = false, seen_alive_late = false, gone = false, saved = false;
    objects::ObjectId landing_id = objects::kNoObjectId;
    objects::ObjectId key = objects::kNoObjectId;
    uint32_t drop_frame = 0, spawned_before = 0, squad = 0;
    LandDropLayout lay;
    for (uint64_t t = 0; t < 2500 && !gone; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (dropped) break;  // the battle ended before the take-off
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        if (!dropped) {
            if (b->frame() != 1) continue;
            const auto reserves = b->reserves(1);
            CHECK(reserves.size() == 1);
            if (reserves.empty()) break;
            key = reserves[0].galactic_id;
            squad = reserves[0].type;
            spawned_before = b->stats().spawned;
            if (b->fog()) b->fog()->reveal_all(1);  // the drop test needs unfogged cells
            size_t valid = 0, refused = 0;
            for (int32_t cy = 6; cy < 58 && !dropped; cy += 2)
                for (int32_t cx = 6; cx < 58 && !dropped; cx += 2) {
                    const mv::V2 c = mv::cell_center(cx, cy);
                    if (!b->land_drop_valid(*tm, 1, key, c.x, c.y, false, false)) continue;
                    ++valid;
                    if (const commands::RejectReason rr = b->order_reinforce(*tm, 1, key, c.x, c.y, host.random().synced());
                        rr != commands::RejectReason::None) {
                        if (refused++ == 0) std::printf("  landing drop: first refusal %d at (%.0f, %.0f)\n", static_cast<int>(rr), static_cast<double>(c.x), static_cast<double>(c.y));
                        continue;
                    }
                    dropped = true;
                    drop_frame = b->frame();
                    lay = land_drop_layout(b->land_drop_shape(squad), c.x, c.y, b->player_drop_yaw(1), 3.0f);
                    CHECK(lay.ok && lay.slots.size() == 4 && lay.group_x.size() == 2);  // two teams of Max_Squad_Size 2
                    CHECK(b->reserves(1).empty());
                    const auto recs = b->landings_unloaded();
                    // The landing object.
                    objects::ObjectHandle lh;
                    if (timed) {
                        CHECK(recs.empty() && b->stats().spawned == spawned_before);  // the company is still inside
                        CHECK(b->landings_in_flight(1) == 1);
                        for (const objects::ObjectHandle h : st.all_objects())
                            if (st.alive(h) && st.type(h) == landing_t) lh = h;
                        landing_id = st.alive(lh) ? st.id(lh) : objects::kNoObjectId;
                        CHECK(b->landing_hidden(landing_id));  // state 4 until the service sees it clear
                    } else {
                        CHECK(recs.size() == 1);
                        if (recs.size() != 1) break;
                        landing_id = recs[0].landing;
                        lh = st.find(landing_id);
                        CHECK(recs[0].galactic_id == key && recs[0].owner == 1 && recs[0].carried_type == squad);
                        CHECK(recs[0].units.size() == 4 && b->stats().spawned == spawned_before + 4u);
                        CHECK(b->landings_in_flight(1) == 0 && !b->landing_hidden(landing_id));
                        // The members at the unload offsets (3.0 x the team bounds, the side's drop yaw).
                        for (size_t k = 0; lay.ok && k < lay.slots.size() && k < recs[0].units.size(); ++k) {
                            const Vec3 mp = st.position(st.find(recs[0].units[k]));
                            CHECK(std::fabs(mp.x - lay.slots[k].x) < 1e-3f && std::fabs(mp.y - lay.slots[k].y) < 1e-3f);
                        }
                        // s78j t01 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object"): team 0 (units 0, 1)
                        // faces the landing's facing; team 1 (units 2, 3) faces along its offset from the landing centre.
                        const Vec3 psi = b->player_drop_facing(1);
                        const double oy = static_cast<double>(lay.group_y[1] - c.y), ox = static_cast<double>(lay.group_x[1] - c.x);
                        const double a1 = std::atan2(oy, ox);
                        for (size_t k = 0; lay.ok && k < recs[0].units.size(); ++k) {
                            const Vec3 f = st.facing(st.find(recs[0].units[k]));
                            const double ex = k < 2 ? static_cast<double>(psi.x) : std::cos(a1);
                            const double ey = k < 2 ? static_cast<double>(psi.y) : std::sin(a1);
                            CHECK(std::fabs(static_cast<double>(f.x) - ex) < 1e-4 && std::fabs(static_cast<double>(f.y) - ey) < 1e-4);
                        }
                        std::printf("  landing drop: team 0 facing (%.3f, %.3f), team 1 offset yaw %.1f deg\n", static_cast<double>(psi.x),
                                    static_cast<double>(psi.y), a1 * 57.29577951308232);
                    }
                    CHECK(st.alive(lh));
                    CHECK(st.type(lh) == landing_t);  // the Landing_Transport_Variant
                    CHECK(st.has_behavior(lh, objects::beh::TransportLanding) && st.carried_type(lh) == squad);
                    CHECK(st.parent_mode_id(lh) == key && st.owner(lh) == 1 && !st.landing_persist(lh));
                    const Vec3 lp = st.position(lh);
                    CHECK(lp.x == c.x && lp.y == c.y && std::fabs(lp.z - 10.0f) < 1e-4f);  // flat map + Hover_Offset 10
                    // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): the landing object faces the owner's entry-facing record.
                    const Vec3 lf = st.facing(lh), df = b->player_drop_facing(1);
                    CHECK(lf.x == df.x && lf.y == df.y && std::fabs(df.x * df.x + df.y * df.y - 1.0f) < 1e-5f);
                }
            if (!dropped) std::printf("  landing drop: no drop (%zu valid spots, %zu refused orders)\n", valid, refused);
            CHECK(dropped);
            if (!dropped) break;
            continue;
        }
        const objects::ObjectHandle lh = st.find(landing_id);
        const uint32_t since = b->frame() - drop_frame;  // 3 steps of 2 s (60 frames)
        if (!timed) {
            if (since < 180u) {
                CHECK(st.alive(lh));
                if (since == 179u) seen_alive_late = true;
                if (since == 1u) CHECK(b->landings_unloaded().empty());  // cleared by the next tick
            } else {
                CHECK(!st.alive(lh));
                gone = true;
            }
            continue;
        }
        // Timed: the service starts the step timer on the first tick (frame drop + 1); team 0 at +61, team 1 at +121, the
        // finish at +181.
        const uint32_t spawned = b->stats().spawned - spawned_before;
        if (since == 1u) CHECK(!b->landing_hidden(landing_id) && spawned == 0);
        if (since == 60u) CHECK(spawned == 0);
        if ((since == 61u || since == 121u) && (destroy_at == 0 || since < destroy_at)) {
            const uint32_t want = since == 61u ? 2u : 4u;
            CHECK(spawned == want);
            const objects::ObjectId uid = b->story_reinforced().empty() ? objects::kNoObjectId : b->story_reinforced().back();
            CHECK(uid != objects::kNoObjectId && b->movement_locked(uid));  // the locked exit move
            const Vec3 mp = st.position(st.find(uid));
            CHECK(std::fabs(mp.x - st.position(lh).x) < 30.0f && std::fabs(mp.y - st.position(lh).y) < 30.0f);  // spawned at the landing
            CHECK(b->landings_in_flight(1) == 1 && b->landings_unloaded().empty());
        }
        if (destroy_at > 0) {  // a1j t01: the landing destroyed mid-unload
            if (since == destroy_at) {
                CHECK(st.alive(lh) && tm->destroy(lh));
                continue;
            }
            if (since == destroy_at + 1u) {
                const uint32_t out = destroy_at > 61u ? 2u : 0u;
                const auto recs = b->landings_unloaded();
                CHECK(recs.size() == 1 && b->landings_in_flight(1) == 0 && spawned == out);
                if (recs.size() == 1) {
                    CHECK(recs[0].landing == landing_id && recs[0].galactic_id == key && recs[0].owner == 1 && recs[0].units.size() == out);
                    for (const objects::ObjectId u : recs[0].units) CHECK(!b->movement_locked(u));
                }
                seen_alive_late = true;
                continue;
            }
            if (since > destroy_at + 1u) {
                CHECK(spawned == (destroy_at > 61u ? 2u : 0u) && b->landings_unloaded().empty());  // the rest never comes
                if (since >= 200u) gone = true;
            }
            continue;
        }
        if (since == 90u && !saved) {  // mid-unload save / load of the battle state (in-flight landing included)
            StateHasher h0;
            b->hash(h0);
            ByteWriter w;
            b->write_part(TacticalBattle::SavePart::State, w);
            ByteReader r(w.bytes());
            CHECK(b->read_part(TacticalBattle::SavePart::State, r));
            // s78f t02 / q6: battle state version 8 (the two version-7 branches merged: FighterFormationRecord::pushed dropped
            // and the q6 TEAM-container attacks); ccpgt t02: version 9 (the domination timer); cu8k t01: version 10 (the
            // thrown sabers); cu8l t03: version 11 (the start fleet arrivals; a version 10 state is still read, without
            // them): 9 and 12 are refused, and so is this version 11 stream read as version 10 (the arrivals are left over).
            std::vector<uint8_t> stale(w.bytes().begin(), w.bytes().end());
            for (const uint32_t v : {9u, 10u, 12u}) {
                for (size_t k = 0; k < 4; ++k) stale[k] = static_cast<uint8_t>(v >> (8 * k));
                ByteReader rs(stale);
                CHECK(!b->read_part(TacticalBattle::SavePart::State, rs));
            }
            StateHasher h1;
            b->hash(h1);
            CHECK(h0.digest() == h1.digest() && b->landings_in_flight(1) == 1);
            saved = true;
        }
        if (since < 181u) {
            CHECK(st.alive(lh));
            if (since == 180u) seen_alive_late = true;
        } else {
            CHECK(!st.alive(lh));
            const auto recs = b->landings_unloaded();
            CHECK(recs.size() == 1 && b->landings_in_flight(1) == 0);
            if (recs.size() == 1) {
                CHECK(recs[0].landing == landing_id && recs[0].galactic_id == key && recs[0].carried_type == squad);
                CHECK(recs[0].units.size() == 4);
                for (const objects::ObjectId u : recs[0].units) CHECK(!b->movement_locked(u));  // unlocked at the finish
            }
            gone = true;
        }
    }
    CHECK(dropped && seen_alive_late && gone);
    if (timed && destroy_at == 0) CHECK(saved);
    std::printf("  landing drop (%s%s): landing %d at frame %u\n", timed ? "timed" : "untimed", destroy_at > 0 ? ", destroyed" : "",
                static_cast<int>(landing_id), drop_frame);
}

// s78l t04 (TACTICAL_COMBAT §3.1.1 "Units leaving a land landing object", MOVEMENT §3.5.1 / §6.1): a timed land Reinforce
// of a vehicle company (vehicle_landing_types: three S10_Tank) from a landing object. Each 2 s step one vehicle leaves at
// the landing centre on a locked flag-0x20 exit move whose final facing is the landing's yaw; on arrival a vehicle more
// than 10 degrees off that yaw turns to it at Stopped_Rate_Of_Turn (4, raw) and ends exactly on it; one within 10 degrees
// keeps its arrival yaw. At least one vehicle must make the turn.
void test_vehicle_landing_exit() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, true, false, false, false, true);
    const objects::TypeIndex landing_t = d->types.resolve_name("S10_Lander_Landing");
    const uint32_t tank_t = type_of(*d, "S10_Tank");
    CHECK(landing_t != objects::kNone && tank_t < d->types.size());
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 900;
    opt.battle.landing_unload_timed = true;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    namespace mv = alamo::tactical::movement;
    const auto wrap = [](double a) {
        while (a > 180.0) a -= 360.0;
        while (a <= -180.0) a += 360.0;
        return a;
    };
    struct Track {
        objects::ObjectId id = objects::kNoObjectId;
        uint32_t spawn = 0, arrive = 0;  // frames: spawned, arrived
        float arrival_yaw = 0.0f, last_yaw = 0.0f;
        bool arrived = false, settled = false, rate_ok = true;
        uint32_t turn_steps = 0;
    };
    std::vector<Track> tracks;
    bool invaded = false, dropped = false;
    objects::ObjectId landing_id = objects::kNoObjectId;
    double psi_yaw = 0.0;
    uint32_t drop_frame = 0;
    const float stopped_rate = 4.0f;
    for (uint64_t t = 0; t < 2500; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (dropped) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        if (!dropped) {
            if (b->frame() != 1) continue;
            const auto reserves = b->reserves(1);
            CHECK(!reserves.empty());
            if (reserves.empty()) break;
            const objects::ObjectId key = reserves[0].galactic_id;
            if (b->fog()) b->fog()->reveal_all(1);
            for (int32_t cy = 6; cy < 58 && !dropped; cy += 2)
                for (int32_t cx = 6; cx < 58 && !dropped; cx += 2) {
                    const mv::V2 c = mv::cell_center(cx, cy);
                    if (!b->land_drop_valid(*tm, 1, key, c.x, c.y, false, false)) continue;
                    if (b->order_reinforce(*tm, 1, key, c.x, c.y, host.random().synced()) != commands::RejectReason::None) continue;
                    dropped = true;
                    drop_frame = b->frame();
                }
            CHECK(dropped);
            if (!dropped) break;
            for (const objects::ObjectHandle h : st.all_objects())
                if (st.alive(h) && st.type(h) == landing_t) landing_id = st.id(h);
            const objects::ObjectHandle lh = st.find(landing_id);
            CHECK(st.alive(lh));
            const Vec3 psi = st.facing(lh);
            psi_yaw = std::atan2(static_cast<double>(psi.y), static_cast<double>(psi.x)) * 57.29577951308232;
            continue;
        }
        for (const objects::ObjectId id : b->story_reinforced()) {  // one vehicle per step
            const objects::ObjectHandle h = st.find(id);
            if (!st.alive(h) || st.type(h) != tank_t) continue;
            Track k;
            k.id = id;
            k.spawn = b->frame();
            k.last_yaw = b->movement().view(id).yaw;
            CHECK(b->movement_locked(id));  // the locked exit move
            tracks.push_back(k);
        }
        for (Track& k : tracks) {
            if (k.settled) continue;
            const mv::UnitView v = b->movement().view(k.id);
            if (!k.arrived) {
                if (v.arrived && !v.has_goal) {
                    k.arrived = true;
                    k.arrive = b->frame();
                    k.arrival_yaw = v.yaw;
                }
            } else {
                const double step = std::fabs(wrap(static_cast<double>(v.yaw) - static_cast<double>(k.last_yaw)));
                if (step > static_cast<double>(stopped_rate) + 1e-3) k.rate_ok = false;
                if (step > 0.0) ++k.turn_steps;
            }
            k.last_yaw = v.yaw;
            const bool far = std::fabs(wrap(static_cast<double>(k.arrival_yaw) - psi_yaw)) > 10.0;
            if (k.arrived && ((far && std::fabs(wrap(static_cast<double>(v.yaw) - psi_yaw)) < 1e-3) || b->frame() >= k.arrive + 60u))
                k.settled = true;
            if (b->frame() > k.spawn + 200u) k.settled = true;  // give up (checked below)
        }
        if (tracks.size() == 3 && std::all_of(tracks.begin(), tracks.end(), [](const Track& k) { return k.settled; })) break;
        if (b->frame() > drop_frame + 600u) break;
    }
    CHECK(dropped && tracks.size() == 3);
    uint32_t turned = 0;
    for (const Track& k : tracks) {
        const double off = wrap(static_cast<double>(k.arrival_yaw) - psi_yaw);
        const bool far = std::fabs(off) > 10.0;
        std::printf("  vehicle landing exit: vehicle %d spawned +%u, arrived %d +%u at yaw %.2f (landing yaw %.2f, %.2f off), %s in "
                    "%u steps, final yaw %.3f\n",
                    static_cast<int>(k.id), k.spawn - drop_frame, k.arrived ? 1 : 0, k.arrived ? k.arrive - drop_frame : 0u,
                    static_cast<double>(k.arrival_yaw), psi_yaw, off, far ? "turned" : "kept", k.turn_steps, static_cast<double>(k.last_yaw));
        CHECK(k.arrived && k.rate_ok);
        if (far) {
            ++turned;
            CHECK(std::fabs(wrap(static_cast<double>(k.last_yaw) - psi_yaw)) < 1e-3);  // ends on the landing's yaw
            CHECK(k.turn_steps == static_cast<uint32_t>(std::ceil((std::fabs(off) - 1e-4) / static_cast<double>(stopped_rate))));
        } else {
            CHECK(k.turn_steps == 0u && k.last_yaw == k.arrival_yaw);  // within 10 degrees: no turn
        }
    }
    CHECK(turned > 0);
}

// S7 t22 / t25 (FIGHTER_FLYING §4.10 A, TACTICAL_COMBAT §2.8 "Targets of opportunity"): the response-range test 0x4160a0.

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_temp_prefix = "alamo_sim_battle_ai_tests";
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--regen") g_regen = true;
    auto d = make_data();
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    std::printf("OI host: BattleAi MoveToObject, hijack_unit, AI Change_Owner\n");
    test_battle_ai_orders(*d);
    std::printf("refixf t02: SPACE_OBSTACLE station as a static entry, per-unit Lua orders as one group move\n");
    test_space_obstacle_orders();
    test_neutral_attack_move();  // S7 t29 / t30
    test_battle_ai_station_level(false);
    test_battle_ai_station_level(true);
    test_battle_ai_land_mode(false);
    test_battle_ai_land_mode(true);
    std::printf("refixf t03: Find_All_Objects_Of_Type('S10_Squad') finds the TEAM containers (representative type hook)\n");
    test_battle_ai_find_team();
    std::printf("q6: an attack ordered on a TEAM container (AI host and human order; nearest member, order ends with the container)\n");
    test_battle_attack_team();
    std::printf("a1g t04 follow-up: BattleAi::land_drop_ok (impassable / fogged / movers / enabling radius)\n");
    test_battle_ai_land_drop(*d);
    std::printf("a2h t05: the battle hosts load the tactical AI constants from GameConstants\n");
    test_tactical_ai_constants_load();
#endif
    std::printf("s78f t07: land team members registered with combat (TeamTargeting copy in a battle)\n");
    test_land_team_targeting();
    std::printf("s78j t03 / t04: land team SPEED = the container's value; Change_Owner spreads through the team; reserve drop facing\n");
    test_team_speed_and_owner();
    test_reserve_drop_facing(true);
    std::printf("s78k t04 / t05: convert spreads to the team container; reserve drop facing != the side's start facing\n");
    test_team_convert();
    test_reserve_drop_facing(false);
    std::printf("s78l t04: Hack / Tactical_Bribe spread to the whole team; a vehicle's landing exit ends on the landing yaw\n");
    test_team_hack_bribe(false);
    test_team_hack_bribe(true);
    test_vehicle_thief();  // s78n t05
    test_land_squad_container();  // s78n t03 / t09
    test_vehicle_landing_exit();
    std::printf("a1i t04 / t05: land Reinforce from a landing object (spawn, carried type, unload offsets, take-off)\n");
    test_landing_drop(false);
    test_landing_drop(true);
    test_landing_drop(true, 90);  // a1j t01: destroyed with team 0 out
    test_landing_drop(true, 30);  // a1j t01: destroyed before any unit is out
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_battle_ai_tests: all passed\n");
    return 0;
}

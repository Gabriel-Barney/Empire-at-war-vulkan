// Part 1 of test_battles_ai (bo3 t03: split by position; see test_battles_ai_common.hpp).
#include "test_battles_ai_common.hpp"

namespace bo3split {
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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// t48: the battle mode reaches the perception (Game.IsCampaignGame, IsDefender, the start markers) and so do both land BaseLevel
// inputs: campaign = the conflict planet's structures, skirmish = each player's DUMMY_GROUND_STRUCTURE objects.
// ccpgu t03: Game.IsLandControlGame is set only in a land skirmish with the control-point win condition (game type 4).
void test_battle_ai_land_mode(bool skirmish, bool control_points) {
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
    opt.battle.control_point_domination = control_points;
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
        CHECK(w.land_control == (skirmish && control_points));  // ccpgu t03
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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// q6 t28 (TERRAIN_WATER_POST 1.6): a map placement's .ted hint name (chunk 0x482) goes on the object the map load creates
// for it, and the battle's Lua finders see it through ObjectBridge::map_hint: Find_Hint(type, hint) (case-insensitive),
// Get_Hint, Find_All_Objects_With_Hint. [port] A Map_Load_Spawn_Table pick does not inherit its placeholder's hint.
void test_battle_map_hints() {
    auto d = make_data(false, false, false, false, false, true);  // the obstacle types (S10_Obst_Rock / _Spawner)
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
                                auto add = [&m](const char* type, const char* hint, float x) {
                                    terrain::ObjectPlacement p;
                                    p.type_crc = name_crc(type);
                                    p.owner = 2;  // the Neutral faction (as test_runtime_obstacles)
                                    p.position[0] = x;
                                    p.position[1] = 400.0f;
                                    p.hint = hint;
                                    m.objects.push_back(p);
                                };
                                add("S10_Obst_Rock", "FalconGear1", 300.0f);
                                add("S10_Obst_Rock", "", 600.0f);
                                add("S10_Obst_Rock_Spawner", "spawnedgear", 900.0f);
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
    const objects::TypeIndex rock = d->types.resolve_name("S10_Obst_Rock");
    CHECK(rock != objects::kNone);
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 2500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 1) continue;
        objects::ObjectStore& st = tm->primary();
        std::vector<objects::ObjectId> rocks;  // creation order: the hinted one, the plain one, the spawn table's pick
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h) && st.type(h) == rock) rocks.push_back(st.id(h));
        std::sort(rocks.begin(), rocks.end());
        CHECK(rocks.size() == 3);
        if (rocks.size() != 3) {
            std::printf("  map hints: %zu rocks\n", rocks.size());
            break;
        }
        CHECK(b->map_hint(rocks[0]) == "FalconGear1" && b->map_hint(rocks[1]).empty() && b->map_hint(rocks[2]).empty());
        BattleAi ai(*b, *tm, host, ts);
        lua::ObjectBridge& br = ai.world().object_bridge();
        CHECK(static_cast<bool>(br.map_hint));
        lua::LuaHost lh;
        lh.store = &st;
        lh.types = &d->types;
        lua::LuaVm vm(lh);
        lua::attach_object_bridge(vm, &br);
        std::string err;
        const std::string code = "local g = Find_Hint('S10_OBST_ROCK', 'falcongear1') "
                                 "if not g or g.Get_Object_ID() ~= " + std::to_string(rocks[0]) + " then error('Find_Hint') end "
                                 "if g.Get_Hint() ~= 'FalconGear1' then error('Get_Hint') end "
                                 "if table.getn(Find_All_Objects_With_Hint('FALCONGEAR1')) ~= 1 then error('Find_All') end "
                                 "if Find_Hint('S10_Trooper', 'falcongear1') ~= nil then error('type filter') end "
                                 "if Find_Hint('S10_Obst_Rock', 'spawnedgear') ~= nil then error('spawn table pick') end";
        const bool ok = vm.run(code, "map_hints", &err);
        if (!ok) std::printf("  map hints: %s\n", err.c_str());
        CHECK(ok);
        lua::attach_object_bridge(vm, nullptr);
        checked = true;
    }
    CHECK(checked);
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
            // s78n t26 review round 3: the test data has no reveal source, so the target would be hidden (FIGHTER_FLYING §4.11 C / E: no
            // land stop, no re-path while moving); this scenario is about a visible target (fog: test_land_attack_unseen)
            if (b->fog()) b->fog()->reveal_all(1);  // the attackers are player 1's
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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
    bool collided = false;  // the container had a collision object during the move
    bool detoured = false, have_prev = false;  // s78n t20: it walked a Path Around detour (a record on top of its path)
    float max_step = 0.0f;
    mv::V2 prev_pos{};
    // s78n t20 item 6 (MOVEMENT 4.3.1): after the checks above, the squad is ordered onto another standing squad
    int onto = 0;  // 0 not started, 1 ordered, 2 arrived, 3 no second standing squad
    objects::ObjectId other_team = objects::kNoObjectId;
    mv::V2 other_pos{}, onto_goal{};
    float other_hx = 0.0f, other_hy = 0.0f, own_hx = 0.0f, own_hy = 0.0f;
    uint32_t onto_frame = 0, hold_run = 0, hold_max = 0;
    float off_line = 0.0f, off_path = 0.0f, max_lead = 0.0f, team_speed = 0.0f, late_along = 0.0f, settle_err = 0.0f;
    uint32_t store_mismatch = 0, moving_frames = 0, recover_frames = 0, box_mismatch = 0;
    float start_error = -1.0f, recovered_error = -1.0f;
    std::vector<float> along_ref;  // s78n t14: each member's along error 120 frames into the move
    bool settle_target = false;    // a member has a target when the team stopped (no recovery, §7.3 item 6)
    for (uint64_t t = 0; t < 8000 && !(done && onto >= 2); ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) {
            if (done) break;  // the battle is over
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        const mv::MovementSystem& ms = b->movement();
        if (done) {
            // s78n t20 item 6: the destination check of the order moves the goal off the standing squad, so the mover
            // never waits in front of it (before: an early-arrival cut at best, a long Try Again Later hold at worst)
            if (onto == 0) {
                std::map<uint32_t, uint32_t> count;
                for (size_t i = 0; i < cs.unit_count(); ++i) {
                    const auto id = cs.unit_id_at(i);
                    if (cs.team(id) != 0 && cs.alive(id)) ++count[cs.team(id)];
                }
                for (const auto& [tid, n] : count) {
                    const auto cid = static_cast<objects::ObjectId>(tid);
                    const objects::ObjectHandle th2 = st.find(cid);
                    if (cid == team_id || n < 2 || !st.alive(th2) || !ms.has_unit(cid) || ms.view(cid).has_goal) continue;
                    // (the object test has no owner test: the human's own second squad when there is one, else any)
                    if (other_team == objects::kNoObjectId || (st.owner(th2) == 0 && st.owner(st.find(other_team)) != 0)) other_team = cid;
                }
                bool mine_alive = ms.has_unit(team_id);
                for (const auto id : members) mine_alive = mine_alive && cs.alive(id);
                if (other_team == objects::kNoObjectId || !mine_alive || !ms.team_joint_box(other_team, other_hx, other_hy) ||
                    !ms.team_joint_box(team_id, own_hx, own_hy)) {
                    onto = 3;
                    continue;
                }
                other_pos = ms.view(other_team).pos;
                CHECK(b->order_move(*tm, 0, members, other_pos.x, other_pos.y, host.random().synced()) == commands::RejectReason::None);
                onto_goal = ms.view(team_id).goal;
                onto_frame = b->frame();
                onto = 1;
                continue;
            }
            if (onto == 1) {
                bool held = false;
                for (const mv::CollisionObject& co : ms.collisions().list())
                    held = held || ((co.a == team_id && co.speed_a == 0.0f) || (co.b == team_id && co.speed_b == 0.0f));
                hold_run = held ? hold_run + 1 : 0;
                hold_max = std::max(hold_max, hold_run);
                if (!ms.has_unit(team_id) || !ms.has_unit(other_team)) onto = 3;  // (a squad died meanwhile)
                else if ((!ms.view(team_id).has_goal && b->frame() > onto_frame + 5) || b->frame() > onto_frame + 1500) onto = 2;
            }
            continue;
        }
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
            {  // s78n t18 review: the movement system's joint box (team_joint_box: the squad's collision footprint) is
               // the same box as the host's (squad_joint_slots: fog footprint, Bribe's E_T) while every member steers
                float mx = 0.0f, my = 0.0f;
                CHECK(ms.team_joint_box(team_id, mx, my) && std::fabs(mx - ex) < 1e-4f && std::fabs(my - ey) < 1e-4f);
            }
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
        // s78n t14: a member's live slot (the Rows layout re-lays at the move start and at waypoints)
        const auto live_slot = [&](size_t m) {
            mv::ObjectId lc = -1;
            mv::V2 sl = slots[m].second;
            CHECK(ms.team_slot(members[m], lc, sl) && lc == team_id);
            return sl;
        };
        // the members' largest distance from their slot points (container + R(yaw) (slot + jiggle))
        const auto slot_error = [&]() {
            float e = 0.0f;
            for (size_t m = 0; m < members.size(); ++m) {
                const mv::UnitView v = ms.view(members[m]);
                const mv::V2 off = live_slot(m) + v.jiggle;
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
                // the container walks its own planned path (never re-centred on its members): its distance from that path's
                // polyline, frame by frame (s78n t14 part B: the path itself may bend around another squad's container, so
                // the straight start -> goal line is only reported)
                const float gx = goal.x - start.x, gy = goal.y - start.y, gl = std::sqrt(gx * gx + gy * gy);
                off_line = std::max(off_line, std::fabs((p.x - start.x) * gy - (p.y - start.y) * gx) / gl);
                const std::span<const mv::V2> path = ms.land_path(team_id);
                float dmin = path.empty() || k.pushed ? 0.0f : 3.0e38f;
                for (size_t w = 0; w + 1 < path.size() && !k.pushed; ++w) {
                    const float ax = path[w].x, ay = path[w].y, bx = path[w + 1].x - ax, by = path[w + 1].y - ay;
                    const float l2 = bx * bx + by * by;
                    const float seg_t = l2 > 0.0f ? std::clamp(((k.pos.x - ax) * bx + (k.pos.y - ay) * by) / l2, 0.0f, 1.0f) : 0.0f;
                    const float ex = k.pos.x - (ax + seg_t * bx), ey = k.pos.y - (ay + seg_t * by);
                    dmin = std::min(dmin, std::sqrt(ex * ex + ey * ey));
                }
                if (path.size() == 1) dmin = std::sqrt((k.pos.x - path[0].x) * (k.pos.x - path[0].x) + (k.pos.y - path[0].y) * (k.pos.y - path[0].y));
                // s78n t18 (MOVEMENT 5.4.1 / 5.4.2): the container stays on its path through its collision objects too (no
                // LandTeamContainer code uses a collision vector; Repulsion / Repulse leave it at full speed on its path)
                {  // s78n t18 review: on the march too (the slots are re-laid at the move start and at waypoints)
                    float ex = 0.0f, ey = 0.0f, mx = 0.0f, my = 0.0f;
                    CHECK(b->squad_joint_box(st, team_id, ex, ey) && ms.team_joint_box(team_id, mx, my));
                    if (std::fabs(mx - ex) > 1e-4f || std::fabs(my - ey) > 1e-4f) ++box_mismatch;
                }
                bool rec_now = false;
                for (const mv::CollisionObject& co : ms.collisions().list()) rec_now = rec_now || co.a == team_id || co.b == team_id;
                collided = collided || rec_now;
                // s78n t20 (MOVEMENT 1.5.1 item 5): a Path Around detour is a record on top of the path stack; when it
                // ends the container heads from the detour's last corner straight for its next ordered waypoint, so from
                // then on it is beside the ordered polyline by design - the distance is taken up to the first detour, and
                // the step length (no displacement in one step) on every frame
                detoured = detoured || (k.pushed && k.push_kind == 2);
                if (have_prev) max_step = std::max(max_step, std::sqrt((k.pos.x - prev_pos.x) * (k.pos.x - prev_pos.x) + (k.pos.y - prev_pos.y) * (k.pos.y - prev_pos.y)));
                prev_pos = k.pos;
                have_prev = true;
                if (!detoured) off_path = std::max(off_path, dmin);
                team_speed = std::max(team_speed, k.speed);
                for (size_t m = 0; m < members.size(); ++m) {
                    const mv::UnitView v = ms.view(members[m]);
                    const float along = live_slot(m).x - (c * (v.pos.x - k.pos.x) + s * (v.pos.y - k.pos.y));
                    max_lead = std::max(max_lead, along);  // > 0: the member is behind its slot (the container leads)
                    // s78n t14 (FIGHTER_FLYING §7 step 4, §7.3): a member's vmax is the team speed, so a lag from the
                    // move start (start delay, turning onto the heading - no turn to the team facing after a recovery)
                    // is kept, not made up; once on the heading the member holds it: the along error stops changing.
                    if (b->frame() == order_frame + 120) {
                        along_ref.resize(members.size());
                        along_ref[m] = along;
                    } else if (b->frame() > order_frame + 120 && m < along_ref.size() && !collided) {
                        // (s78n t18: once the container has a collision object its members side-step - the vector 0x509810 -
                        // and their along errors move with that; measured only before the first object)
                        late_along = std::max(late_along, std::fabs(along - along_ref[m]));
                    }
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
        for (const auto id : members) {
            CHECK(!ms.view(id).has_goal);
            settle_target = settle_target || cs.current_target(id) != tc::kNoObject;
        }
        done = true;
    }
    CHECK(done);
    std::printf("  land squad: dropped up to %.2f off their slots, recovered to %.2f in %u frames\n", static_cast<double>(start_error),
                static_cast<double>(recovered_error), recover_frames);
    std::printf("  land squad: moving %u frames, off its line <= %.3f, off its path <= %.3f (collision object %d), members up to %.2f behind their slots at the team speed %.3f, "
                "along error change after 2 s <= %.2f, settled within %.2f (target %d); store = mover %d\n",
                moving_frames, static_cast<double>(off_line), static_cast<double>(off_path), collided ? 1 : 0,
                static_cast<double>(max_lead),
                static_cast<double>(team_speed),
                static_cast<double>(late_along), static_cast<double>(settle_err), settle_target ? 1 : 0, store_mismatch == 0 ? 1 : 0);
    if (!done) return;
    CHECK(store_mismatch == 0);
    CHECK(start_error > 4.0f && recovered_error >= 0.0f && recovered_error <= 4.0f);  // idle recovery (> 4.0 from the slot)
    // s78n t18: the container walks its own planned path (never re-centred on its members) and stays on it through its
    // collision objects: no nudge off the path, no repath against infantry (before t18 a MovingVsMoving object with another
    // squad's container side-stepped it up to about 16 off the path). s78n t20: against a standing vehicle it walks a
    // Path Around detour (the record on top of its path stack) and then resumes toward its next ordered waypoint; it
    // never moves farther in one step than its team speed.
    CHECK(moving_frames > 60 && off_path < 0.5f);
    CHECK(max_step <= team_speed + 1e-3f);
    std::printf("  land squad: Path Around detour %d, largest step %.3f\n", detoured ? 1 : 0, static_cast<double>(max_step));
    // s78n t20 item 6: ordered onto a standing squad - the goal is off its footprint (the two joint boxes, each
    // + BetweenFormationSpacing on the mover's side, do not overlap at the goal; s78n t20 F5: the accepted centre, no
    // cell snap), the squad arrives, and no collision record ever holds it for 30 frames
    if (onto == 2 && battles.battle() != nullptr && battles.battle()->movement().has_unit(team_id)) {
        const mv::UnitView k2 = battles.battle()->movement().view(team_id);
        const float gx = std::fabs(onto_goal.x - other_pos.x), gy = std::fabs(onto_goal.y - other_pos.y);
        std::printf("  land squad: ordered onto squad %d at (%.0f, %.0f) - goal (%.0f, %.0f), arrived %d, longest collision hold %u frames\n",
                    static_cast<int>(other_team), static_cast<double>(other_pos.x), static_cast<double>(other_pos.y),
                    static_cast<double>(onto_goal.x), static_cast<double>(onto_goal.y), k2.arrived ? 1 : 0, hold_max);
        CHECK(gx > own_hx + other_hx + 5.0f || gy > own_hy + other_hy + 5.0f);
        CHECK(k2.arrived && hold_max < 30);
    } else {
        std::printf("  land squad: ordered onto a standing squad - not run (state %d: no second standing squad, or one died)\n", onto);
    }
    CHECK(box_mismatch == 0);  // s78n t18 review: squad_joint_slots and team_joint_box agreed on every moving frame
    CHECK(max_lead > 1.0f);   // the container leads (its members steer from its start-of-tick state)
    CHECK(along_ref.size() == members.size() && late_along < 4.0f);  // and the members hold the formation
    CHECK(settle_target || settle_err <= 4.0f);  // s78n t14: they recover to their slots only while the team has no target
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// s78n review (TACTICAL_COMBAT §5.4.4 item 2, L1_S4_REMAINING §3.5): a land squad garrisons in a garrison vehicle and
// leaves it again (squad_types(true): S10_Walker, GARRISON_UNIT troopers). The garrison entry parks each member's team
// place (MovementSystem::park_team_member); the walker then drives off with the squad inside; the leave steers every
// member to its old slot again (rejoin_land_team): the slots it had when it went in, the container's store position = its
// mover's (team targeting P_U). An order on one member then moves the whole squad, which settles on its slots.
// s78n t14 (FIGHTER_FLYING §7.4): when the last member is in, the container is at the walker's position (and rides with
// it: it is teleported to the host centre at the leave); the squad leaves as one - the container at the host centre E,
// every member at E + its offset, then the container walks out to the garrison's exit point X (Move_To, flag 0x20).
void test_land_squad_garrison() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup setup = make_setup(*d, true);
    const uint32_t walker_co = type_of(*d, "S10_Walker_Co"), walker_t = type_of(*d, "S10_Walker");
    CHECK(walker_co < d->types.size() && walker_t < d->types.size());
    setup.forces.push_back({0, 1, walker_co});  // the Empire's defenders + the walker
    galactic::GalacticSim* gs = host.start_galactic(d->rules, setup);
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 5000;
    opt.battle.skip_human_units = true;  // the stand-in leaves the human's units to this test's orders
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
    namespace mv = alamo::tactical::movement;
    bool invaded = false, done = false;
    int stage = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId team_id = objects::kNoObjectId, walker = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    std::vector<std::pair<mv::ObjectId, mv::V2>> slots;
    float box_x = 0.0f, box_y = 0.0f, exit_gap = -1.0f, walked = 0.0f, settle_err = -1.0f, in_gap = -1.0f, off_err = -1.0f;
    float out_gap = -1.0f;
    bool settle_target = false;  // s78n t14: a member has a target when the squad stopped (no recovery, §7.3 item 6)
    bool inside_ok = false, rejoined = false, ordered = false, store_ok = true, teleported = false, walking_out = false;
    bool tele_members = false;  // s78n t18: the container's teleport moved its members with it
    uint32_t partial_frames = 0;  // s78n t18: frames with the squad partly inside its host
    std::vector<std::pair<mv::ObjectId, mv::V2>> in_slots;  // each member's slot when it went in (team_slot)
    std::vector<mv::V2> in_off;                            // and its offset from the container (parked_offset)
    Vec3 exit_x{};
    Vec3 walker_at{}, tele_to{};
    for (uint64_t t = 0; t < 12000 && !done; ++t) {
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
        const mv::MovementSystem& ms = b->movement();
        if (stage == 0) {  // the human's squad and walker
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                if (!cs.alive(id)) continue;
                if (cs.team(id) != 0) teams[cs.team(id)].push_back(id);
                if (st.type(st.find(id)) == walker_t) walker = id;
            }
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() >= 2 && team_id == objects::kNoObjectId && st.alive(th) && st.owner(th) == 0) {
                    team_id = static_cast<objects::ObjectId>(tid);
                    members = m;
                }
            }
            CHECK(team_id != objects::kNoObjectId && walker != objects::kNoObjectId);
            if (team_id == objects::kNoObjectId || walker == objects::kNoObjectId) break;
            CHECK(ms.is_land_team_container(team_id) && cs.owner(walker) == 0);
            slots = ms.land_team_slots(team_id);
            CHECK(slots.size() == members.size());
            CHECK(b->squad_joint_box(st, team_id, box_x, box_y) && box_x > 0.0f && box_y > 0.0f);
            {  // s78n t18 review: the movement system's joint box agrees
                float mx = 0.0f, my = 0.0f;
                CHECK(ms.team_joint_box(team_id, mx, my) && std::fabs(mx - box_x) < 1e-4f && std::fabs(my - box_y) < 1e-4f);
            }
            CHECK(b->order_garrison(*tm, 0, members, walker, host.random().synced()) == commands::RejectReason::None);
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        bool all_alive = cs.alive(walker);
        for (const auto id : members) all_alive = all_alive && cs.alive(id);
        CHECK(all_alive);
        if (!all_alive) break;
        const objects::ObjectHandle th = st.find(team_id);
        const mv::UnitView k = ms.view(team_id);
        if (st.position(th).x != k.pos.x || st.position(th).y != k.pos.y) store_ok = false;  // P_U = the mover's position
        if (stage == 1) {  // every member inside the walker
            bool inside = true;
            for (const auto id : members) {
                mv::ObjectId c = -1;
                mv::V2 sl;
                if (ms.team_slot(id, c, sl)) {  // still outside: remember its slot (the one it parks with)
                    if (in_slots.size() < members.size()) in_slots.assign(members.size(), {});
                    for (size_t m = 0; m < members.size(); ++m)
                        if (members[m] == id) in_slots[m] = {id, sl};
                }
                inside = inside && b->garrison_holder_of(id) == walker;
            }
            {  // s78n t18 (FIGHTER_FLYING 7.4): the squad garrisons as one - never some members inside and some outside
                size_t in = 0;
                for (const auto id : members) in += b->garrison_holder_of(id) == walker ? 1u : 0u;
                if (in != 0 && in != members.size()) ++partial_frames;
            }
            if (!inside) {
                if (b->frame() > stage_frame + 1500) break;
                continue;
            }
            {  // the container went in with the last member: at the walker's position
                const Vec3 hp = st.position(st.find(walker));
                in_gap = std::sqrt((k.pos.x - hp.x) * (k.pos.x - hp.x) + (k.pos.y - hp.y) * (k.pos.y - hp.y));
                in_off.clear();
                for (const auto id : members) {
                    mv::V2 o;
                    CHECK(ms.parked_offset(id, o));
                    in_off.push_back(o);
                }
            }
            inside_ok = ms.has_unit(team_id);
            for (const auto id : members) inside_ok = inside_ok && !ms.has_unit(id) && ms.team_member_parked(id);
            float ex = -1.0f, ey = -1.0f;
            inside_ok = inside_ok && ms.land_team(team_id).empty() && b->squad_joint_box(st, team_id, ex, ey) && ex == 0.0f && ey == 0.0f;
            {  // s78n t18 review: the absent members stay TEAM entries of the movement system - its box is the one they
               // went in with - and the container is hidden from the entry on (no wait for the next tick's feed)
                float mx = 0.0f, my = 0.0f;
                mv::ObjectId gh = -1;
                bool hidden = false;
                inside_ok = inside_ok && ms.team_joint_box(team_id, mx, my) && std::fabs(mx - box_x) < 1e-4f &&
                            std::fabs(my - box_y) < 1e-4f && ms.team_entries(team_id).size() == members.size() &&
                            ms.pair_gate(team_id, gh, hidden) && hidden && gh == walker;
            }
            // the walker drives 300 off with the squad inside
            const Vec3 wp = st.position(st.find(walker));
            walker_at = wp;
            const objects::ObjectId w1[] = {walker};
            CHECK(b->order_move(*tm, 0, w1, wp.x + 300.0f, wp.y, host.random().synced()) == commands::RejectReason::None);
            stage = 2;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 2) {  // the walker stops; the squad leaves
            if (ms.view(walker).has_goal && b->frame() < stage_frame + 900) continue;
            const Vec3 wp = st.position(st.find(walker));
            walked = std::sqrt((wp.x - walker_at.x) * (wp.x - walker_at.x) + (wp.y - walker_at.y) * (wp.y - walker_at.y));
            const objects::ObjectId one_m[] = {members.front()};  // one member's leave takes the whole squad out
            CHECK(b->order_leave_garrison(*tm, 0, one_m) == commands::RejectReason::None);
            exit_x = b->garrison().exit_point(walker, wp, st.facing(st.find(walker)));
            rejoined = in_slots.size() == members.size();
            const mv::V2 kp = ms.view(team_id).pos;
            off_err = 0.0f;
            for (size_t m = 0; m < members.size(); ++m) {
                mv::ObjectId c = -1;
                mv::V2 sl;
                rejoined = rejoined && ms.has_unit(members[m]) && b->garrison_holder_of(members[m]) == objects::kNoObjectId &&
                           ms.team_slot(members[m], c, sl) && c == team_id && sl == in_slots[m].second &&
                           !ms.team_member_parked(members[m]);
                if (m < in_off.size()) {  // every member at E + its offset
                    const mv::V2 p = ms.view(members[m]).pos;
                    off_err = std::max(off_err, std::fabs(p.x - (kp.x + in_off[m].x)) + std::fabs(p.y - (kp.y + in_off[m].y)));
                }
            }
            float ex = 0.0f, ey = 0.0f;
            CHECK(b->squad_joint_box(st, team_id, ex, ey) && ex > 0.0f && ey > 0.0f);
            {  // s78n t18 review: out again, every member steers: the two boxes agree (inside, the host's box is empty -
               // it reads the live steering members - while the movement system keeps the absent members' entries)
                float mx = 0.0f, my = 0.0f;
                CHECK(ms.team_joint_box(team_id, mx, my) && std::fabs(mx - ex) < 1e-4f && std::fabs(my - ey) < 1e-4f);
            }
            exit_gap = std::sqrt((kp.x - wp.x) * (kp.x - wp.x) + (kp.y - wp.y) * (kp.y - wp.y));
            walking_out = ms.view(team_id).has_goal;
            stage = 3;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 3) {  // the container walks out to X; then an order on one member moves the squad
            if (k.has_goal && b->frame() < stage_frame + 600) continue;
            out_gap = std::sqrt((k.pos.x - exit_x.x) * (k.pos.x - exit_x.x) + (k.pos.y - exit_x.y) * (k.pos.y - exit_x.y));
            const Vec3 p = st.position(th);
            const objects::ObjectId one[] = {members.back()};
            CHECK(b->order_move(*tm, 0, one, p.x - 200.0f, p.y + 100.0f, host.random().synced()) == commands::RejectReason::None);
            ordered = ms.view(team_id).has_goal;
            for (const auto id : members) ordered = ordered && ms.view(id).has_goal && ms.view(id).goal == ms.view(team_id).goal;
            stage = 4;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 4) {
            if (k.has_goal && b->frame() < stage_frame + 1200) continue;
            CHECK(k.arrived);
            stage = 5;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 5) {
            if (b->frame() < stage_frame + 150) continue;  // the members settle on their slots
            const float c = std::cos(k.yaw * 0.017453292f), s = std::sin(k.yaw * 0.017453292f);
            settle_err = 0.0f;
            for (size_t m = 0; m < members.size(); ++m) {
                const mv::UnitView v = ms.view(members[m]);
                mv::ObjectId mc = -1;
                mv::V2 sl;
                CHECK(ms.team_slot(members[m], mc, sl));  // the live slot (re-laid at the move start)
                const mv::V2 off = sl + v.jiggle;
                const float tx = k.pos.x + c * off.x - s * off.y, ty = k.pos.y + s * off.x + c * off.y;
                settle_err = std::max(settle_err, std::sqrt((v.pos.x - tx) * (v.pos.x - tx) + (v.pos.y - ty) * (v.pos.y - ty)));
            }
            for (const auto id : members) {
                CHECK(cs.team(id) == static_cast<uint32_t>(team_id));  // team targeting: still its team
                settle_target = settle_target || cs.current_target(id) != tc::kNoObject;
            }
            // s78n review (§5.4.4 item 2.4): a teleport of the container moves its mover (before, the store moved and
            // commit() wrote the old position back); the members then recover to their slots there
            const Vec3 p = st.position(th);
            tele_to = Vec3{p.x + 60.0f, p.y + 40.0f, p.z};
            std::vector<mv::V2> off0;
            for (const auto id : members) off0.push_back(ms.view(id).pos - k.pos);
            CHECK(b->teleport(*tm, team_id, tele_to, st.facing(th)));
            // s78n t18 (FIGHTER_FLYING 7.4, 0x3abb80): every member at the new place + its old offset, store = mover
            tele_members = true;
            for (size_t m = 0; m < members.size(); ++m) {
                const mv::UnitView v = ms.view(members[m]);
                const Vec3 sp = st.position(st.find(members[m]));
                tele_members = tele_members && std::fabs(v.pos.x - (tele_to.x + off0[m].x)) < 1e-3f &&
                               std::fabs(v.pos.y - (tele_to.y + off0[m].y)) < 1e-3f && sp.x == v.pos.x && sp.y == v.pos.y &&
                               v.speed == 0.0f;
            }
            stage = 6;
            stage_frame = b->frame();
            continue;
        }
        if (b->frame() < stage_frame + 2) continue;
        const Vec3 p = st.position(th);
        // s78n t18: the members went with the container (each at its offset), so the squad stands as it stood
        teleported = k.pos.x == tele_to.x && k.pos.y == tele_to.y && p.x == tele_to.x && p.y == tele_to.y && tele_members;
        done = true;
    }
    std::printf("  land squad garrison: stage %d, %zu members inside %d, walker drove %.1f, rejoined %d, container %.1f from the "
                "walker, squad order %d, settled within %.2f (target %d), store = mover %d\n",
                stage, members.size(), inside_ok ? 1 : 0, static_cast<double>(walked), rejoined ? 1 : 0, static_cast<double>(exit_gap),
                ordered ? 1 : 0, static_cast<double>(settle_err), settle_target ? 1 : 0, store_ok ? 1 : 0);
    std::printf("  land squad garrison: container teleport moves its mover %d\n", teleported ? 1 : 0);
    std::printf("  land squad garrison (s78n t14): container in %.2f from the walker, members out at E + offset within %.3f, "
                "walking out %d, ended %.2f from the exit point\n",
                static_cast<double>(in_gap), static_cast<double>(off_err), walking_out ? 1 : 0, static_cast<double>(out_gap));
    CHECK(in_gap >= 0.0f && in_gap < 0.01f && off_err >= 0.0f && off_err < 0.01f && walking_out && out_gap >= 0.0f && out_gap < 2.0f);
    CHECK(done);
    if (!done) return;
    CHECK(inside_ok && walked > 100.0f && rejoined && ordered && store_ok);
    CHECK(partial_frames == 0);
    CHECK(exit_gap >= 0.0f && exit_gap < 80.0f);  // the container came out with its squad, not left at the old approach point
    CHECK(settle_target || settle_err <= 4.0f);  // s78n t14: no recovery while the team has a target
    CHECK(teleported);
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// s78n t05 (HERO_CLASH §8.1 / §8.2): the Underworld's vehicle thief steals the Empire's speeder. The capture variant is
// spawned with the speeder's owner, then the boarding changes its owner to the thief's (store, combat, abilities), sets
// the permanent stolen-vehicle flag and the incognito flag, and the thief is inside it. While incognito no unit
// auto-acquires the variant; the stolen flag survives a later owner change.
// s78n t10 (TACTICAL_COMBAT §5.4.6 item 1, 0x371be0): a type without a land model has half extents (0, 0) unless BOTH
// Custom_Hard extents are set - one custom alone gives nothing (before: that axis x Scale_Factor); both need no model.
void test_drop_extents_no_model() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
    const uint32_t one = type_of(*d, "S10_OneExtent"), both = type_of(*d, "S10_Speeder");
    CHECK(one < d->types.size() && both < d->types.size());
    if (one >= d->types.size() || both >= d->types.size()) return;
    CHECK(d->types.get_float(one, "Custom_Hard_XExtent") == 12.0f && d->types.get_float(one, "Custom_Hard_YExtent") == 0.0f);
    for (const BattleCatalog* cat : {static_cast<const BattleCatalog*>(d->catalog.get()), static_cast<const BattleCatalog*>(nullptr)}) {
        float hx = -1.0f, hy = -1.0f;
        drop_half_extents(d->types, cat, one, hx, hy);
        CHECK(hx == 0.0f && hy == 0.0f);
        drop_deployed_extents(d->types, cat, one, hx, hy);
        CHECK(hx == 0.0f && hy == 0.0f);
        drop_half_extents(d->types, cat, both, hx, hy);
        CHECK(hx == 10.0f && hy == 10.0f);
    }
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// s78n t12 (HERO_CLASH §8.4.3): `variant` 0 = the speeder has a Vehicle_Thief_Inside_Clone (the clone is spawned, the
// speeder killed in the host pass, the clone boarded); 1 = no clone type (S10_Speeder_Plain: the ORIGINAL is boarded - the
// same object id lives on with the thief's owner, the stolen / incognito flags and the thief inside; no spawn, no kill; it
// becomes a garrison holder on demand); 2 = no clone type and unit ability EJECT_VEHICLE_THIEF (S10_Speeder_Eject: as 1,
// and ability 46 is turned on - only an object that has it gets it).
void test_vehicle_thief(int variant) {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup setup = make_setup(*d, true);
    const uint32_t thief_co = type_of(*d, "S10_Thief_Co"), thief_t = type_of(*d, "S10_Thief");
    const char* const speeder_name = variant == 0 ? "S10_Speeder" : variant == 1 ? "S10_Speeder_Plain" : "S10_Speeder_Eject";
    const uint32_t speeder_co = type_of(*d, (std::string(speeder_name) + "_Co").c_str()), speeder_t = type_of(*d, speeder_name),
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
    const uint8_t eject = tactical::abilities::ability_type_from_name("EJECT_VEHICLE_THIEF");
    size_t units0 = 0;
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
            CHECK(b->abilities().has_ability(speeder, eject) == (variant == 2));
            units0 = cs.unit_count();
            const objects::ObjectId users[] = {thief};
            const bool ordered = b->abilities().targeted_ability(users, kind, speeder, -1, b->frame()) == 1u;
            CHECK(ordered);
            if (!ordered) break;
            order_frame = b->frame();
            stage = 1;
            continue;
        }
        if (stage == 1 && variant != 0) {  // s78n t12: no clone type - the thief is inside the original speeder
            if (b->garrison_holder_of(thief) != speeder) {
                if (b->frame() >= order_frame + 60u) break;
                continue;
            }
            stolen = speeder;
            CHECK(cs.alive(speeder) && st.alive(st.find(speeder)) && st.type(st.find(speeder)) == speeder_t);  // not killed
            CHECK(cs.unit_count() == units0);                                                                 // nothing spawned
            CHECK(st.owner(st.find(speeder)) == 1 && cs.owner(speeder) == 1);
            CHECK((cs.flags(speeder) & tc::kUnitStolenVehicle) != 0 && (cs.flags(speeder) & tc::kUnitIncognito) != 0);
            CHECK(b->abilities().is_active(thief, kind));                         // CAPTURE_VEHICLE on for the thief
            CHECK(b->abilities().is_active(speeder, eject) == (variant == 2));    // 46 on only when the vehicle has it
            CHECK((cs.flags(thief) & tc::kUnitGarrisoned) != 0);
            stage = 2;
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
            CHECK(b->abilities().is_active(thief, kind) && !b->abilities().is_active(stolen, eject));  // the clone has no 46
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
    std::printf("  vehicle thief (%s): thief %d -> speeder %d, boarded %d; %u incognito ticks watched, %u acquisitions\n",
                speeder_name, static_cast<int>(thief), static_cast<int>(speeder), static_cast<int>(stolen), watched, acquired);
    CHECK(stage == 3 && stolen != objects::kNoObjectId && watched > 0 && acquired == 0);
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

uint64_t clash_digest(const TacticalBattle& b) {
    StateHasher hs;
    b.hash(hs);
    b.combat().hash_units(hs);
    b.movement().hash_units(hs);
    return hs.digest();
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

bool goal_at(const mv::UnitView& v, const std::vector<Vec3>& points) {
    for (const Vec3& q : points)
        if (v.has_goal && std::fabs(v.goal.x - q.x) < 1.0f && std::fabs(v.goal.y - q.y) < 1.0f) return true;
    return false;
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

HeroClashRun run_hero_clash_battle(const Data& d, uint32_t workers, bool save_load, bool queue) {
    HeroClashRun out;
    const uint32_t a_t = type_of(d, "S10_HeroA"), b_t = type_of(d, "S10_HeroB");
    CHECK(a_t < d.types.size() && b_t < d.types.size());
    const auto data = hero_clash_data("S10_HeroA", "S10_HeroB");
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.hero_clash = queue ? nullptr : data;        // queue: the clash data comes on once the moves are queued
    if (queue) opt.battle.skip_human_units = true;  // hero A (the human's) is left to the test's orders
    ClashSession s(d, workers, opt, {"S10_HeroA_Co"}, {"S10_HeroB_Co"}, true);
    if (!s.gs) return out;
    objects::ObjectId ha = objects::kNoObjectId, hb = objects::kNoObjectId;
    std::vector<Vec3> queued_at;
    SaveFile save;
    std::vector<uint64_t> after_save;  // the original's digests of the ticks after its save
    uint32_t after_end = 0;
    for (uint64_t t = 0; t < 8000 && after_end < 30; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 1) {
            if (out.started) break;  // the battle ended
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        if (ha == objects::kNoObjectId) ha = s.find(*b, a_t);
        if (hb == objects::kNoObjectId) hb = s.find(*b, b_t);
        const uint64_t dg = clash_digest(*b);
        out.digests.push_back(dg);
        if (out.save_ok) after_save.push_back(dg);
        const tc::HeroClashManager& m = b->hero_clashes();
        if (queue && !out.queued && ha != objects::kNoObjectId && hb != objects::kNoObjectId && m.empty()) {
            // a move away and three queued moves, then the clash data on (the next detection frame starts the clash)
            const uint8_t owner = static_cast<uint8_t>(st.owner(st.find(ha)));
            const Vec3 p = st.position(st.find(ha));
            const objects::ObjectId one[1] = {ha};
            CHECK(b->order_move(*tm, owner, one, p.x + 300.0f, p.y, s.host->random().synced()) == commands::RejectReason::None);
            for (int k = 1; k <= 3; ++k) {
                const Vec3 q{p.x + 40.0f * static_cast<float>(k), p.y + 200.0f, p.z};
                CHECK(b->order_move(*tm, owner, one, q.x, q.y, s.host->random().synced(), mv::kNoFacing, false, true) ==
                      commands::RejectReason::None);
                queued_at.push_back(q);
            }
            CHECK(b->queued_orders(ha).size() == 3);
            b->set_hero_clash_data(data);
            out.queued = true;
            out.queue_dropped = true;
        }
        if (!m.empty()) {
            if (!out.started) out.start_frame = static_cast<uint32_t>(s.host->frame());
            out.started = true;
            const tc::HeroClashManager::Clash& c = m.clashes()[0];
            out.exchanges = c.exchanges;
            CHECK(c.hero1 == ha && c.hero2 == hb);
            if (queue && cs.alive(ha)) {
                if (!b->queued_orders(ha).empty()) out.queue_dropped = false;  // dropped in the tick the clash started
                if (goal_at(b->movement().view(ha), queued_at)) out.queue_moved = true;
            }
            for (const objects::ObjectId h : {ha, hb}) {
                // the loser dies in combat phase D; its clash ends at the next update (the removal path), so a dead
                // hero is not checked here
                if (!cs.has_unit(h) || !cs.alive(h)) continue;
                if (!(cs.flags(h) & tc::kUnitHeroClash)) out.flagged = false;
                if (!b->movement_locked(h)) out.locked = false;
                if (b->abilities().can_activate(h, ta::kHunt, false, false, b->frame())) out.hunt_locked = false;
                if (cs.current_target(h) != tc::kNoObject) out.untargeted = false;
                for (size_t i = 0; i < cs.unit_count(); ++i) {
                    const auto id = cs.unit_id_at(i);
                    if (id != ha && id != hb && cs.alive(id) && cs.current_target(id) == h) out.untargeted = false;
                }
            }
            if (save_load && !out.save_ok && c.state == tc::HeroClashManager::kFight && c.exchanges >= 2) {
                std::string why;
                if (s.host->save_allowed(&why)) {
                    out.save_ok = s.host->save(save, &why);
                    if (!out.save_ok) std::printf("  hero clash save: %s\n", why.c_str());
                    out.save_tick = static_cast<uint32_t>(s.host->frame());
                }
            }
        } else if (out.started && !out.ended) {
            out.ended = true;
            out.end_frame = static_cast<uint32_t>(s.host->frame());
            out.health_a = cs.alive(ha) ? cs.health(ha) : 0.0f;
            out.health_b = cs.alive(hb) ? cs.health(hb) : 0.0f;
            const objects::ObjectId survivor = cs.alive(ha) ? ha : hb;
            out.released = (cs.alive(ha) != cs.alive(hb)) && !(cs.flags(survivor) & tc::kUnitHeroClash) &&
                           !b->movement_locked(survivor);
            out.hunt_after = b->abilities().can_activate(survivor, ta::kHunt, false, false, b->frame());
        }
        if (queue && out.ended && cs.alive(ha) && goal_at(b->movement().view(ha), queued_at)) out.queue_moved = true;
        if (out.ended) ++after_end;
    }
    if (!save_load || !out.save_ok) return out;
    // A fresh session (same data, setup and options; nothing launched: the save holds the galaxy and the battle), the
    // save loaded, then as many ticks as the original ran after its save: the same digests every tick.
    ClashSession f(d, workers, opt, {"S10_HeroA_Co"}, {"S10_HeroB_Co"}, false);
    if (!f.gs) return out;
    std::string err;
    LoadReport rep;
    out.load_ok = f.host->load(save, &err, &rep);
    if (!out.load_ok) std::printf("  hero clash load: %s\n", rep.text().c_str());
    f.invaded = true;
    out.load_same = out.load_ok && f.host->frame() == out.save_tick;
    for (size_t i = 0; out.load_ok && i < after_save.size(); ++i) {
        TacticalBattle* b = f.tick();
        if (!b || b->frame() < 1) {
            out.load_same = false;
            break;
        }
        ++out.compared;
        if (clash_digest(*b) != after_save[i]) {
            if (out.load_same) std::printf("  hero clash load: digest differs %zu tick(s) after the load\n", i + 1);
            out.load_same = false;
        }
    }
    // the host CRC logs (every registered system) agree too, over the same ticks
    const auto window = [&out](const CrcLog& from) {
        CrcLog w;
        for (const CrcRecord& r : from.records)
            if (r.tick >= out.save_tick && r.tick < out.save_tick + out.compared) w.records.push_back(r);
        w.systems = from.systems;
        w.parity_parts = from.parity_parts;
        return w;
    };
    const CrcLog log_a = window(s.host->crc_log()), log_b = window(f.host->crc_log());
    if (log_a.records.size() + 2 < out.compared) out.load_same = false;  // the original logged those ticks
    const CrcDivergence dv = first_divergence(log_a, log_b);
    if (dv.diverged) {
        std::printf("  hero clash load: %s\n", dv.message.c_str());
        out.load_same = false;
    }
    return out;
}
#endif
}  // namespace bo3split

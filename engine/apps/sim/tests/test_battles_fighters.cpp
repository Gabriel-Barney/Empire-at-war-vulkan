// alamo_sim_battle_fighter_tests: S10 battle transitions, host side (docs/re/BATTLE_TRANSITIONS.md): fighter squadrons in battles (formation push, chase rule, escorts, hangar guards, fog toggle).
// Split out of test_battles.cpp (WF1b t01); the fixture (synthetic data, maps, run_campaign) is battle_test_fixture.hpp.

#include "battle_test_fixture.hpp"


// S7 t12 (FIGHTER_FLYING §2 step 6 / §4.3, TACTICAL_COMBAT §2.8 Formation_Attack_Target): a fighter squadron moved past
// an enemy frigate keeps its move order. Before, the frigate (the squadron's combat auto target) became the locomotor's
// target, the attack run reset the squadron's path (§4.3 step 4) and the squadron never reached its destination (skirmish
// foc seed 5: a TIE squadron on Prepare_Ambush parked ~1900 short when its target went into the fog). Then an ordered
// attack on the frigate starts Directed_Combat at once (an ordered target is the formation attack target).
void test_fighter_move_past_enemy() {
    namespace mv = tactical::movement;
    {  // the pure rule (S7 t22: no auto-target input - an auto target reaches the squadron only as a pushed block)
        FighterTargetInputs in;
        CHECK(fighter_formation_target(in) == kNoObjectId);  // a position block / no formation: no object, keep Moving
        in.goal = 9;
        CHECK(fighter_formation_target(in) == kNoObjectId);  // a goal that is not hostile never qualifies
        in.goal_hostile = true;
        CHECK(fighter_formation_target(in) == 9);  // Attack_Move(object)
        in.pushed = 7;
        CHECK(fighter_formation_target(in) == 7);  // a pushed auto block is newer than the order's block
        in.ordered = 11;
        CHECK(fighter_formation_target(in) == 11);  // Attack / Attack_Target (locked team target)
    }
    auto d = make_data(false, false, false, false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSetup gsetup = make_setup(*d, false);
    gsetup.forces.push_back({0, 1, type_of(*d, "S10_TIE_Squadron")});
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
    using R = commands::RejectReason;
    std::vector<objects::ObjectId> members;
    objects::ObjectId enemy = objects::kNoObjectId;
    mv::V2 dest{};
    bool issued = false, auto_seen = false, arrived = false, attacked = false;
    uint32_t combat_while_moving = 0, arrive_frame = 0, attack_frame = 0, end_frame = 0;
    float closest_pass = 1.0e9f;
    for (uint64_t t = 0; t < 7000; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (issued) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (b->frame() == 3 && !issued) {
            issued = true;
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && st.owner(st.find(s.members.front())) == 0) members = s.members;
            CHECK(members.size() == 3);
            if (members.empty()) break;
            const Vec3 sp = st.position(st.find(members.front()));
            float best = 0.0f;
            for (const objects::ObjectHandle h : st.owned_by(1)) {
                if (!st.alive(h) || !b->movement().has_unit(st.id(h))) continue;
                const Vec3 p = st.position(h);
                const float d2 = (p.x - sp.x) * (p.x - sp.x) + (p.y - sp.y) * (p.y - sp.y);
                if (enemy == objects::kNoObjectId || d2 < best) {
                    enemy = st.id(h);
                    best = d2;
                }
            }
            CHECK(enemy != objects::kNoObjectId);
            if (enemy == objects::kNoObjectId) break;
            // Straight through the enemy and 1500 beyond it (inside the playable box).
            const Vec3 ep = st.position(st.find(enemy));
            const float dx = ep.x - sp.x, dy = ep.y - sp.y, len = std::sqrt(dx * dx + dy * dy);
            dest = {std::clamp(ep.x + dx / len * 1500.0f, -3700.0f, 3700.0f), std::clamp(ep.y + dy / len * 1500.0f, -3700.0f, 3700.0f)};
            CHECK(b->order_move(*tm, 0, members, dest.x, dest.y, host.random().synced()) == R::None);
            std::printf("  squadron %d..%d at (%.0f, %.0f), enemy %d at (%.0f, %.0f), move to (%.0f, %.0f)\n", members.front(),
                        members.back(), static_cast<double>(sp.x), static_cast<double>(sp.y), enemy, static_cast<double>(ep.x),
                        static_cast<double>(ep.y), static_cast<double>(dest.x), static_cast<double>(dest.y));
            continue;
        }
        if (!issued) continue;
        const objects::ObjectId lead = members.front();
        if (!st.alive(st.find(lead)) || !b->movement().has_unit(lead)) break;
        end_frame = b->frame();
        const mv::UnitView v = b->movement().view(lead);
        const bool enemy_alive = st.alive(st.find(enemy));
        if (enemy_alive) {
            const Vec3 lp = st.position(st.find(lead)), ep = st.position(st.find(enemy));
            closest_pass = std::min(closest_pass, std::sqrt((lp.x - ep.x) * (lp.x - ep.x) + (lp.y - ep.y) * (lp.y - ep.y)));
            if (!arrived && b->combat().current_target(lead) == enemy) auto_seen = true;
        }
        if (!arrived) {
            if (v.loco_state == mv::LocoState::FighterDirectedCombat) ++combat_while_moving;
            const float gx = v.pos.x - dest.x, gy = v.pos.y - dest.y;
            if (v.arrived || gx * gx + gy * gy < 300.0f * 300.0f) {
                arrived = true;
                arrive_frame = b->frame();
                if (enemy_alive) {  // then an ordered attack on the frigate: Directed_Combat from the next frame
                    CHECK(b->order_attack(*tm, 0, members, enemy, host.random().synced()) == R::None);
                    attack_frame = b->frame();
                }
            }
        } else if (attack_frame != 0 && !attacked && b->frame() <= attack_frame + 3) {
            attacked = v.loco_state == mv::LocoState::FighterDirectedCombat;
        }
        if ((arrived && (attack_frame == 0 || b->frame() > attack_frame + 3)) || b->frame() >= 3000) break;
    }
    std::printf("  frames to %u: enemy auto target seen %d, closest pass %.0f, Directed_Combat frames while moving %u, arrived %d (frame %u), "
                "ordered attack -> Directed_Combat %d\n",
                end_frame, auto_seen ? 1 : 0, static_cast<double>(closest_pass), combat_while_moving, arrived ? 1 : 0, arrive_frame,
                attacked ? 1 : 0);
    CHECK(issued && auto_seen && closest_pass < 560.0f);  // the frigate was the auto target and inside the strafe circle
    CHECK(combat_while_moving == 0 && arrived);
    CHECK(attack_frame != 0 && attacked);
}

// a1i t04 / t05: a land Reinforce of a company with a Company_Transport_Unit spawns its landing object (the transport's
// Landing_Transport_Variant at the drop point, z + Hover_Offset, TRANSPORT_LANDING, carried type = the company).
// Untimed ([port] option): the members spawn at the 3.0-factor unload offsets at the order, the unloaded record is posted
// and the landing object leaves after (teams + 1) x 2 s. Timed (BattleConfig::landing_unload_timed, the default since a1j
// t01): the company stays in the hidden landing; one team per 2 s leaves on a locked exit move; the last step unlocks
// them, posts the record and removes the landing; the battle state saves and loads mid-unload. Also the deployed extents
// getter. a1j t01 (A1_READINGS §36), timed with `destroy_at` > 0: the landing object is destroyed `destroy_at` frames after
// the order; the next service posts the signal 0x19 record of the detach (vf15 0x6442d0) with the units out so far
// (destroy_at 90: team 0; destroy_at 30: none [port: the original sends nothing then]), unlocked, and the rest never spawns.
void test_fighter_chase_rule() {
    FighterChaseInputs in;
    in.guard_range = 1000.0f;
    in.idle_range = 200.0f;
    in.attack_move_range = 300.0f;
    in.extension = 300.0f;
    in.attack_range = 500.0f;
    in.candidate = {650.0f, 0.0f, 0.0f};
    in.single_position = true;  // arrived at the end of its only (position) block
    CHECK(fighter_chase_test(in) == 0);  // no formation: no chase (its weapons still fire in range)
    in.formation = true;
    CHECK(fighter_chase_test(in) == 1);  // idle: 650 <= Idle_Chase_Range 200 + range 500 from the slot goal
    in.candidate = {750.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);  // beyond it
    in.candidate = {150.0f, 0.0f, 0.0f};
    in.single_position = false;
    CHECK(fighter_chase_test(in) == 0);  // a pushed auto block is the last block (or more than one block): no response
    in.moving = true;
    CHECK(fighter_chase_test(in) == 0);  // a plain position move under way: no response range at all (§4.10 A)
    in.flags = 0x01;
    in.candidate = {5000.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 2);  // an attack-move under way responds [M: the squadron is at its own path point]
    in.moving = false;
    in.single_position = true;
    CHECK(fighter_chase_test(in) == 0);  // arrived: rule 7 with Attack_Move_Response_Range
    in.candidate = {790.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);  // 790 <= 300 + 500
    in.flags = 0x02;
    in.single_position = false;
    in.last_guard = true;
    in.candidate = {1450.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);  // guard: Guard_Chase_Range 1000 + 500
    in.moving = true;
    in.unit = {900.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);  // a guard move is not an attack-move: rule 7 while moving, u within Rr of the goal
    in.unit = {1100.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);  // ... but not when u itself is farther than Rr from its slot goal
    in.moving = false;
    in.candidate = {1550.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);
    in.candidate_attacker = true;  // c attacks the carrier: + Autonomous_Move_Extension_Vs_Attacker, base 1
    CHECK(fighter_chase_test(in) == 2);
    in.candidate = {1250.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);  // within Rr 1300
    in.guarded_moving = true;
    CHECK(fighter_chase_test(in) == 1);  // a moving carrier: its attackers are answered ...
    in.candidate_attacker = false;
    in.candidate = {100.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);  // ... nothing else
    in.guarded_moving = false;
    CHECK(fighter_chase_test(in) == 1);
    in.candidate_is_goal = true;
    CHECK(fighter_chase_test(in) == 0);  // the first / last block's object: path B, not a chase
    in.candidate_is_goal = false;
    in.flags = 0;
    in.last_guard = false;
    in.single_position = true;
    in.idle_range = -1.0f;  // Rr < 0 -> 0
    in.candidate = {450.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 1);
    in.candidate = {550.0f, 0.0f, 0.0f};
    CHECK(fighter_chase_test(in) == 0);
}

namespace {
struct FighterBattle {
    std::unique_ptr<Data> d;
    std::unique_ptr<jobs::JobSystem> js;
    std::unique_ptr<SimHost> host;
    std::unique_ptr<CampaignBattles> battles;
    galactic::GalacticSim* gs = nullptr;
};

// The S7 t12 fighter battle: the TIE squadron (player 0) joins the S10 space fight; no stand-in orders.
bool start_fighter_battle(FighterBattle& fb, bool fog, bool carrier = false) {
    fb.d = make_data(fog, false, false, false, false, false, false, false, true);
    fb.js = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    fb.host = std::make_unique<SimHost>(fb.d->types, *fb.js, cfg);
    galactic::GalacticSetup gsetup = make_setup(*fb.d, false);
    gsetup.forces.push_back({0, 1, type_of(*fb.d, "S10_TIE_Squadron")});
    if (carrier) gsetup.forces.push_back({0, 1, type_of(*fb.d, "S10_Carrier")});  // S7 t24
    fb.gs = fb.host->start_galactic(fb.d->rules, gsetup);
    CHECK(fb.gs != nullptr);
    if (!fb.gs) return false;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.order_interval = 0;
    opt.battle.fog = fog;
    fb.battles = std::make_unique<CampaignBattles>(*fb.host, fb.d->crules, *fb.d->catalog,
                                                   [](const std::string& name, terrain::MapData& m) {
                                                       if (name != "S10_Space_B") return false;
                                                       m = space_map();
                                                       return true;
                                                   },
                                                   opt);
    CHECK(fb.battles->attach(nullptr));
    const std::vector<uint32_t> uf = fb.gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(fb.gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    return true;
}
}  // namespace

// S7 t22 (FIGHTER_FLYING §4.9): an auto target moves a squadron only after it is pushed into the squadron's formation. One
// battle, in phases:
//  A. no order yet: no formation. Then an Attack_Move(position) through the nearest enemy: the enemy (the auto target) is
//     pushed while the move is under way (Attack_Move_Response_Range) and the squadron starts its attack run (the PR #19
//     review: such squadrons never did);
//  B. Stop leaves the formation: the squadron idles next to its auto target for 300 frames and never attacks it;
//  C. a plain move to where it is gives it a formation again: the auto target lies within Idle_Chase_Range + range of the
//     slot goal, is pushed and attacked;
//  D. the pushed target turns friendly (Change_Owner): Directed_Combat finds no goal and runs enter_idle, which drops the
//     members' combat targets (the team target, §4.7 step 4) and keeps the formation.
void test_fighter_auto_push() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false)) return;
    enum class Ph { Wait, AttackMove, Stopped, Idle, Converted, Done } ph = Ph::Wait;
    std::vector<objects::ObjectId> members;
    objects::ObjectId squad = objects::kNoObjectId, enemy = objects::kNoObjectId, pushed = objects::kNoObjectId;
    uint32_t since = 0;
    bool no_formation_at_start = false, am_dc = false, am_moving = false, am_pushed = false;
    bool stop_cleared = false, b_auto = false, b_dc = false, c_formation = false, c_dc = false, c_pushed = false;
    bool d_idle = false, d_dropped = false, d_kept = false;
    std::vector<objects::ObjectId> converted;
    mv::LocoState prev = mv::LocoState::None;
    for (uint64_t t = 0; t < 9000 && ph != Ph::Done; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (ph != Ph::Wait) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (ph == Ph::Wait) {
            if (b->frame() != 3) continue;
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && st.owner(st.find(s.members.front())) == 0) {
                    members = s.members;
                    squad = s.id;
                }
            CHECK(members.size() == 3);
            if (members.empty()) break;
            no_formation_at_start = b->fighter_formation(squad) == nullptr;
            const Vec3 sp = st.position(st.find(members.front()));
            float best = 0.0f;
            for (const objects::ObjectHandle h : st.owned_by(1)) {
                if (!st.alive(h) || !b->movement().has_unit(st.id(h))) continue;
                const Vec3 p = st.position(h);
                const float d2 = (p.x - sp.x) * (p.x - sp.x) + (p.y - sp.y) * (p.y - sp.y);
                if (enemy == objects::kNoObjectId || d2 < best) {
                    enemy = st.id(h);
                    best = d2;
                }
            }
            CHECK(enemy != objects::kNoObjectId);
            if (enemy == objects::kNoObjectId) break;
            const Vec3 ep = st.position(st.find(enemy));
            const float dx = ep.x - sp.x, dy = ep.y - sp.y, len = std::sqrt(dx * dx + dy * dy);
            const float gx = std::clamp(ep.x + dx / len * 1500.0f, -3700.0f, 3700.0f), gy = std::clamp(ep.y + dy / len * 1500.0f, -3700.0f, 3700.0f);
            CHECK(b->order_move(*tm, 0, members, gx, gy, fb.host->random().synced(), mv::kNoFacing, true) == R::None);
            const FighterFormationRecord* r = b->fighter_formation(squad);
            CHECK(r && r->flags == mv::kMoveFlagAttackMove && b->fighter_pushed(squad).empty());
            ph = Ph::AttackMove;
            since = b->frame();
            continue;
        }
        const objects::ObjectId lead = members.front();
        if (!st.alive(st.find(lead)) || !b->movement().has_unit(lead)) break;
        const mv::UnitView v = b->movement().view(lead);
        const FighterFormationRecord* r = b->fighter_formation(squad);
        switch (ph) {
            case Ph::AttackMove:
                if (v.loco_state == mv::LocoState::FighterDirectedCombat) {
                    am_dc = true;
                    am_moving = !v.arrived;
                    am_pushed = r && !b->fighter_pushed(squad).empty();
                    CHECK(b->order_stop(*tm, 0, members) == R::None);
                    stop_cleared = b->fighter_formation(squad) == nullptr;
                    ph = Ph::Stopped;
                    since = b->frame();
                } else if (b->frame() > since + 2500) {
                    ph = Ph::Done;
                }
                break;
            case Ph::Stopped:
                for (const objects::ObjectId m : members) b_auto = b_auto || b->combat().auto_target(m) != objects::kNoObjectId;
                b_dc = b_dc || v.loco_state == mv::LocoState::FighterDirectedCombat;
                if (b->frame() >= since + 300) {
                    Vec3 lp = st.position(st.find(lead));
                    // S8 t17: the attack-move's scan cube R + Rr (FIGHTER_FLYING §4.10 A) engages earlier, so the stop can
                    // leave the squadron beyond R + Idle_Chase_Range of the enemy: the idle move ends 700 from it.
                    if (const objects::ObjectHandle eh = st.find(enemy); st.alive(eh)) {
                        const Vec3 ep = st.position(eh);
                        const float dx = lp.x - ep.x, dy = lp.y - ep.y, d = std::sqrt(dx * dx + dy * dy);
                        if (d > 700.0f) lp = Vec3{ep.x + dx / d * 700.0f, ep.y + dy / d * 700.0f, lp.z};
                    }
                    CHECK(b->order_move(*tm, 0, members, lp.x, lp.y, fb.host->random().synced()) == R::None);
                    const FighterFormationRecord* nr = b->fighter_formation(squad);
                    c_formation = nr && nr->flags == 0 && b->fighter_pushed(squad).empty();
                    ph = Ph::Idle;
                    since = b->frame();
                }
                break;
            case Ph::Idle:
                if (v.loco_state == mv::LocoState::FighterDirectedCombat && r && !b->fighter_pushed(squad).empty()) {
                    c_dc = true;
                    pushed = b->fighter_pushed(squad).back();
                    c_pushed = b->combat().has_unit(pushed);  // a ship (not a squadron)
                    if (c_pushed) CHECK(b->change_owner(*tm, st.find(pushed), 0));
                    if (c_pushed) converted.push_back(pushed);
                    ph = c_pushed ? Ph::Converted : Ph::Done;
                    since = b->frame();
                } else if (b->frame() > since + 600) {
                    ph = Ph::Done;
                }
                break;
            case Ph::Converted:
                if (prev != mv::LocoState::FighterIdle && v.loco_state == mv::LocoState::FighterIdle) {
                    d_idle = true;
                    d_dropped = true;
                    for (const objects::ObjectId m : members)
                        d_dropped = d_dropped && std::find(converted.begin(), converted.end(), b->combat().current_target(m)) == converted.end();
                    d_kept = r != nullptr;
                    ph = Ph::Done;
                } else if (b->frame() > since + 120) {
                    ph = Ph::Done;
                }
                // A ship pushed after the conversion (a second enemy in reach) turns friendly too, so Directed_Combat runs out
                // of goals (S8 t10-t12 shot timing brought one more frigate into reach).
                if (ph == Ph::Converted && r)
                    for (const objects::ObjectId k : b->fighter_pushed(squad))
                        if (std::find(converted.begin(), converted.end(), k) == converted.end() && b->combat().has_unit(k) &&
                            b->combat().owner(k) != 0) {
                            CHECK(b->change_owner(*tm, st.find(k), 0));
                            converted.push_back(k);
                        }
                break;
            default: break;
        }
        prev = v.loco_state;
    }
    std::printf("  auto push: squadron %d, enemy %d; attack-move: Directed_Combat %d (moving %d, pushed %d); stop: formation gone %d, "
                "auto target %d, Directed_Combat %d; idle move: formation %d, Directed_Combat %d (pushed %d); converted: idle %d, "
                "targets dropped %d, formation kept %d\n",
                squad, enemy, am_dc ? 1 : 0, am_moving ? 1 : 0, am_pushed ? 1 : 0, stop_cleared ? 1 : 0, b_auto ? 1 : 0, b_dc ? 1 : 0,
                c_formation ? 1 : 0, c_dc ? 1 : 0, pushed, d_idle ? 1 : 0, d_dropped ? 1 : 0, d_kept ? 1 : 0);
    CHECK(no_formation_at_start);
    CHECK(am_dc && am_moving && am_pushed);
    CHECK(stop_cleared && b_auto && !b_dc);
    CHECK(c_formation && c_dc && c_pushed);
    CHECK(d_idle && d_dropped && d_kept);
}

// S7 t24 (STATION_HANGARS_SOUND §2.5, A1_READINGS §33.4, FIGHTER_FLYING §4.9): a moving carrier's launch is a guard of the
// carrier. The launched squadron's formation record is {flags 0x02, goal object = the carrier} with a host follow, so its
// chase range is Guard_Chase_Range around the carrier; when the carrier moves away, the squadron re-paths to it (at most
// once per 30 frames, when the carrier is more than half the leader's attack range from the recorded goal) and keeps the
// guard record (flags, goal object). S7 t28 / t35 (A1_READINGS §33.4.1): while the carrier moves the squadron, within
// R = min(range / 2, Space_Guard_Range 750) of it, flies a copy of its path at most twice its speed (s78f t02: only while
// the squadron's formation moves, FIGHTER_FLYING §4.11 A; the leader's goal reaches the carrier's destination). A new
// order of the squadron ends the follow (a plain move record, a position block).
void test_hangar_escort_guard() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false, true)) return;
    const uint32_t carrier_type = type_of(*fb.d, "S10_Carrier");
    objects::ObjectId carrier = objects::kNoObjectId, squad = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    bool guard_record = false, followed = false, kept = true, ended = false, capped = false, copied = false;
    uint32_t launch_frame = 0, move_frame = 0, follow_frame = 0, copy_frame = 0;
    mv::V2 dest{};
    for (uint64_t t = 0; t < 5000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (carrier != objects::kNoObjectId) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (carrier == objects::kNoObjectId) {
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && st.type(h) == carrier_type) carrier = st.id(h);
            if (carrier == objects::kNoObjectId) continue;
        }
        if (!st.alive(st.find(carrier))) break;
        const Vec3 cp = st.position(st.find(carrier));
        if (squad == objects::kNoObjectId) {
            for (const mv::FighterSquad& s : b->movement().fighter_squads()) {
                const FighterFormationRecord* r = b->fighter_formation(s.id);
                if (!s.members.empty() && r && r->goal_object == carrier) {
                    squad = s.id;
                    members = s.members;
                    // S7 t37 / s78f t02: the movement layer's formation holds the guard block as its only block.
                    const mv::FighterFormation* fg = b->movement().fighter_formation(s.id);
                    guard_record = r->flags == mv::kMoveFlagGuardMove && b->fighter_pushed(s.id).empty() && fg && fg->blocks.size() == 1 &&
                                   fg->blocks[0].kind == mv::kFighterBlockObject && fg->blocks[0].object == carrier &&
                                   fg->blocks[0].flags == mv::kMoveFlagGuardMove && fg->next > b->frame() && fg->follow;
                }
            }
            if (squad == objects::kNoObjectId) {
                if (b->frame() > 300) break;
                continue;
            }
            launch_frame = b->frame();
            // The carrier moves 1500 away (inside the playable box); the escort is not ordered.
            dest = {cp.x, cp.y + (cp.y > 0.0f ? -1500.0f : 1500.0f)};
            const objects::ObjectId cid[1] = {carrier};
            CHECK(b->order_move(*tm, 0, cid, dest.x, dest.y, fb.host->random().synced()) == R::None);
            move_frame = b->frame();
            continue;
        }
        const FighterFormationRecord* r = b->fighter_formation(squad);
        if (!r || members.empty() || !b->movement().has_unit(members.front())) break;
        kept = kept && r->flags == mv::kMoveFlagGuardMove && r->goal_object == carrier;
        const mv::UnitView v = b->movement().view(members.front());
        // S7 t28 / s78f t02: while the carrier moves, the squadron formation's speed (F +0x21c) is capped at min(its own speed,
        // 2 x the carrier's speed) and, its centre within R of the carrier, it flies a copy of the carrier's path (its goal =
        // the carrier's goal point).
        const mv::UnitView cv = b->movement().view(carrier);
        const bool c_moving = cv.has_goal && !cv.arrived;
        if (const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
            ff && c_moving && ff->speed >= 0.0f && ff->speed <= 2.0f * cv.max_speed)
            capped = true;
        if (!copied && c_moving && v.has_goal && v.goal.x == cv.goal.x && v.goal.y == cv.goal.y) {
            copied = true;
            copy_frame = b->frame();
        }
        // s78f t02: the follow is the path copy / re-path of the formation (FIGHTER_FLYING §4.11 A): the leader's goal moved
        // with the carrier to its destination while the guard block stays the formation's only block.
        const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
        const bool guard_kept = ff && ff->blocks.size() == 1 && ff->blocks[0].object == carrier;
        const float gx = v.goal.x - dest.x, gy = v.goal.y - dest.y;
        if (!followed && guard_kept && v.has_goal && gx * gx + gy * gy < 600.0f * 600.0f) {
            followed = true;
            follow_frame = b->frame();
            // A plain move order of the squadron replaces the guard: no follow any more.
            const Vec3 lp = st.position(st.find(members.front()));
            CHECK(b->order_move(*tm, 0, members, lp.x, lp.y, fb.host->random().synced()) == R::None);
            const FighterFormationRecord* nr = b->fighter_formation(squad);
            const mv::FighterFormation* nf = b->movement().fighter_formation(squad);
            ended = nr && nr->flags == 0 && nr->goal_object == objects::kNoObjectId && nf && nf->blocks.size() == 1 &&
                    nf->blocks[0].kind == mv::kFighterBlockPosition && (nf->blocks[0].flags & mv::kMoveFlagGuardMove) == 0;
            break;
        }
        if (b->frame() > move_frame + 3000) break;
    }
    std::printf("  escort guard: carrier %d squadron %d launched frame %u (guard record %d); carrier moved frame %u to (%.0f, %.0f); "
                "followed %d (frame %u), speed cap %d, path copy %d (frame %u), guard kept %d, new order ends the follow %d\n",
                carrier, squad, launch_frame, guard_record ? 1 : 0, move_frame, static_cast<double>(dest.x), static_cast<double>(dest.y),
                followed ? 1 : 0, follow_frame, capped ? 1 : 0, copied ? 1 : 0, copy_frame, kept ? 1 : 0, ended ? 1 : 0);
    CHECK(carrier != objects::kNoObjectId && squad != objects::kNoObjectId && members.size() == 3);
    CHECK(guard_record);
    CHECK(followed && kept && ended);
    CHECK(capped && copied);
}

// s78f review (FIGHTER_FLYING §4.11 B 1 / B 3, 0x4160a0): host and movement stack agree around a shift waypoint: a
// launched escort (guard of its carrier) gets a shift-queued waypoint while the carrier moves: the waypoint goes to
//     the bottom of the movement stack and the host record still mirrors the guard on top (flags 0x02, goal object = the
//     carrier; Rr from the first block = the waypoint's); once the carrier stops the guard pops and the record follows the
//     waypoint (a position block: no guard, no object, goal = the waypoint).
void test_escort_waypoint() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false, true)) return;
    const uint32_t carrier_type = type_of(*fb.d, "S10_Carrier");
    objects::ObjectId carrier = objects::kNoObjectId, squad = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    mv::V2 wp{};
    bool queued = false, guard_while_moving = false, still_guard = true, popped = false, follows_wp = false;
    uint32_t pop_frame = 0, stop_frame = 0, guard_frames = 0;
    for (uint64_t t = 0; t < 6000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (carrier != objects::kNoObjectId) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (carrier == objects::kNoObjectId) {
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && st.type(h) == carrier_type) carrier = st.id(h);
            if (carrier == objects::kNoObjectId) continue;
        }
        if (!st.alive(st.find(carrier))) break;
        const Vec3 cp = st.position(st.find(carrier));
        if (squad == objects::kNoObjectId) {
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (const FighterFormationRecord* r = b->fighter_formation(s.id); !s.members.empty() && r && r->goal_object == carrier) {
                    squad = s.id;
                    members = s.members;
                }
            if (squad == objects::kNoObjectId) {
                if (b->frame() > 300) break;
                continue;
            }
            const objects::ObjectId cid[1] = {carrier};
            CHECK(b->order_move(*tm, 0, cid, cp.x, cp.y + (cp.y > 0.0f ? -1200.0f : 1200.0f), fb.host->random().synced()) == R::None);
            wp = {cp.x + (cp.x > 0.0f ? -1500.0f : 1500.0f), cp.y};
            CHECK(b->order_move(*tm, 0, members, wp.x, wp.y, fb.host->random().synced(), mv::kNoFacing, false, true) == R::None);
            const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
            queued = ff && ff->blocks.size() == 2 && ff->blocks[0].kind == mv::kFighterBlockPosition && ff->blocks[1].object == carrier &&
                     (ff->blocks[1].flags & mv::kMoveFlagGuardMove) != 0;
            continue;
        }
        if (members.empty() || !b->movement().has_unit(members.front())) break;
        const FighterFormationRecord* r = b->fighter_formation(squad);
        const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
        if (!r || !ff || ff->blocks.empty()) break;
        const mv::UnitView cv = b->movement().view(carrier);
        const bool c_moving = cv.has_goal && !cv.arrived;
        if (!c_moving && stop_frame == 0) stop_frame = b->frame();
        const bool guard_top = ff->blocks.back().kind == mv::kFighterBlockObject && ff->blocks.back().object == carrier &&
                               (ff->blocks.back().flags & mv::kMoveFlagGuardMove) != 0;
        if (!popped && guard_top) {
            const FighterOrderView ov = b->fighter_order_view(*r);
            const bool g = r->flags == mv::kMoveFlagGuardMove && r->goal_object == carrier && ov.first_flags == 0;
            still_guard = still_guard && g;
            if (c_moving) {
                guard_while_moving = guard_while_moving || g;
                ++guard_frames;
            }
        }
        if (!popped && ff->blocks.size() == 1 && ff->blocks[0].kind == mv::kFighterBlockPosition) {
            popped = true;
            pop_frame = b->frame();
        }
        if (popped && !follows_wp && b->frame() > pop_frame) {
            follows_wp = r->flags == 0 && r->goal_object == objects::kNoObjectId && r->goal.x == ff->blocks[0].point.x &&
                         r->goal.y == ff->blocks[0].point.y && std::abs(r->goal.x - wp.x) < 1.0f && std::abs(r->goal.y - wp.y) < 1.0f;
            break;
        }
        if (b->frame() > 4000) break;
    }
    std::printf("  escort + shift waypoint: squadron %d queued below the guard %d, guard record while the carrier moves %d "
                "(%u frames, kept %d), carrier stopped frame %u, guard popped %d (frame %u), record follows the waypoint %d\n",
                squad, queued ? 1 : 0, guard_while_moving ? 1 : 0, guard_frames, still_guard ? 1 : 0, stop_frame, popped ? 1 : 0,
                pop_frame, follows_wp ? 1 : 0);
    CHECK(squad != objects::kNoObjectId && queued && guard_while_moving && still_guard);
    CHECK(popped && stop_frame > 0 && pop_frame <= stop_frame + 2u * mv::kFighterGuardInterval + 1u && follows_wp);
}

// s78f review (FIGHTER_FLYING §4.11 B 1 / B 3 "new group move"): an Attack on an enemy already in range (exec_attack moves
// nobody) still replaces the squadron's whole stack (a launch guard with a shift waypoint below it) by the object block.
void test_escort_attack_in_range() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false, true)) return;
    const uint32_t carrier_type = type_of(*fb.d, "S10_Carrier");
    objects::ObjectId carrier = objects::kNoObjectId, squad = objects::kNoObjectId, enemy = objects::kNoObjectId;
    bool two = false, attack_stack = false, attack_record = false, in_range = false;
    for (uint64_t t = 0; t < 3000 && squad == objects::kNoObjectId; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) continue;
        const objects::ObjectStore& st = tm->primary();
        if (carrier == objects::kNoObjectId)
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && st.type(h) == carrier_type) carrier = st.id(h);
        if (carrier == objects::kNoObjectId) continue;
        std::vector<objects::ObjectId> members;
        for (const mv::FighterSquad& s : b->movement().fighter_squads())
            if (const FighterFormationRecord* r = b->fighter_formation(s.id); !s.members.empty() && r && r->goal_object == carrier) {
                squad = s.id;
                members = s.members;
            }
        if (squad == objects::kNoObjectId) {
            if (b->frame() > 300) break;
            continue;
        }
        const Vec3 cp = st.position(st.find(carrier));
        CHECK(b->order_move(*tm, 0, members, cp.x + 900.0f, cp.y, fb.host->random().synced(), mv::kNoFacing, false, true) == R::None);
        const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
        two = ff && ff->blocks.size() == 2 && ff->blocks[1].object == carrier;
        for (const objects::ObjectHandle h : st.owned_by(1))
            if (st.alive(h) && b->movement().has_unit(st.id(h)) && b->combat().has_unit(st.id(h)) &&
                b->movement().view(st.id(h)).locomotor != mv::LocomotorKind::Fighter) {
                enemy = st.id(h);
                break;
            }
        CHECK(enemy != objects::kNoObjectId);
        if (enemy == objects::kNoObjectId) break;
        const Vec3 ep = st.position(st.find(enemy));
        in_range = true;
        for (size_t k = 0; k < members.size(); ++k) {
            const Vec3 at{ep.x + 15.0f * static_cast<float>(k + 1), ep.y + 20.0f, ep.z};
            CHECK(b->teleport(*tm, members[k], at, Vec3{1, 0, 0}));
            const float dx = at.x - ep.x, dy = at.y - ep.y;
            in_range = in_range && std::sqrt(dx * dx + dy * dy) <= 80.0f;  // exec_attack: inside 0.8 x max(range, 100)
        }
        CHECK(b->order_attack(*tm, 0, members, enemy, fb.host->random().synced()) == R::None);
        const mv::FighterFormation* af = b->movement().fighter_formation(squad);
        attack_stack = af && af->blocks.size() == 1 && af->blocks[0].kind == mv::kFighterBlockObject && af->blocks[0].object == enemy &&
                       af->blocks[0].flags == 0 && !af->blocks[0].pushed;
        const FighterFormationRecord* ar = b->fighter_formation(squad);
        attack_record = ar && ar->flags == 0 && ar->goal_object == enemy;
    }
    std::printf("  in-range attack: squadron %d (guard + waypoint stack %d), enemy %d in range %d: stack replaced by the object "
                "block %d (record %d)\n",
                squad, two ? 1 : 0, enemy, in_range ? 1 : 0, attack_stack ? 1 : 0, attack_record ? 1 : 0);
    CHECK(squad != objects::kNoObjectId && two && in_range && attack_stack && attack_record);
}

// s78f t10: the AI order paths give the movement layer the same block as the host record. A Lua Guard_Target of the
// carrier by the squadron (BattleAi, as the tactical AI's group order): the movement stack holds one guard object block
// (flags 0x02, the carrier) and the record matches; the carrier then moves and the squadron's formation keeps that block
// (the follow service no longer re-issues a flags-0 position move to fighters with an object block). Then an AI Attack on
// an enemy already in range replaces the stack by the object block (as the human exec_attack does).
void test_ai_fighter_blocks() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false, true)) return;
    const uint32_t carrier_type = type_of(*fb.d, "S10_Carrier");
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup setup;
    setup.defs = &defs;
    setup.equations = &eqs;
    setup.files = fb.d->files.get();
    setup.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    objects::ObjectId carrier = objects::kNoObjectId, squad = objects::kNoObjectId, enemy = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    std::unique_ptr<BattleAi> ai;
    bool guard_stack = false, guard_record = false, kept = true, carrier_moved = false, attack_stack = false, attack_record = false;
    uint32_t guard_frame = 0, kept_frames = 0;
    Vec3 c0{};
    for (uint64_t t = 0; t < 6000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (carrier != objects::kNoObjectId) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (carrier == objects::kNoObjectId) {
            for (const objects::ObjectHandle h : st.owned_by(0))
                if (st.alive(h) && st.type(h) == carrier_type) carrier = st.id(h);
            if (carrier == objects::kNoObjectId) continue;
        }
        if (!st.alive(st.find(carrier))) break;
        if (squad == objects::kNoObjectId) {
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && b->fighter_formation(s.id)) {
                    squad = s.id;
                    members = s.members;
                }
            if (squad == objects::kNoObjectId) {
                if (b->frame() > 300) break;
                continue;
            }
            ai = std::make_unique<BattleAi>(*b, *tm, *fb.host, setup);
            const Vec3 cp = st.position(st.find(carrier));
            c0 = cp;
            std::vector<uint8_t> pay;
            auto put = [&pay](uint32_t v) {
                for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
            };
            put(static_cast<uint32_t>(members.front()));
            put(static_cast<uint32_t>(carrier));
            put(std::bit_cast<uint32_t>(cp.x));
            put(std::bit_cast<uint32_t>(cp.y));
            put(std::bit_cast<uint32_t>(cp.z));
            put(0u);
            for (size_t i = 1; i < members.size(); ++i) put(static_cast<uint32_t>(members[i]));
            const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::Guard));
            CHECK(ai->execute(0, commands::ev::port_extension(sub, pay)) == R::None);
            const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
            guard_stack = ff && ff->blocks.size() == 1 && ff->blocks[0].kind == mv::kFighterBlockObject && ff->blocks[0].object == carrier &&
                          ff->blocks[0].flags == mv::kMoveFlagGuardMove;
            const FighterFormationRecord* r = b->fighter_formation(squad);
            guard_record = r && r->flags == mv::kMoveFlagGuardMove && r->goal_object == carrier;
            guard_frame = b->frame();
            const objects::ObjectId cid[1] = {carrier};
            CHECK(b->order_move(*tm, 0, cid, cp.x, cp.y + (cp.y > 0.0f ? -1500.0f : 1500.0f), fb.host->random().synced()) == R::None);
            continue;
        }
        // The follow service (a1h t02) runs in update(); the stand-in frames (every 60th, BattleAi::standin_orders: plain moves
        // of the uncontrolled players' units) are skipped so only the follow re-issues moves.
        if ((b->frame() - 1u) % 60u != 0u) ai->update();
        if (b->frame() < guard_frame + 600u) {
            const mv::FighterFormation* ff = b->movement().fighter_formation(squad);
            const bool g = ff && !ff->blocks.empty() && ff->blocks[0].kind == mv::kFighterBlockObject && ff->blocks[0].object == carrier &&
                           (ff->blocks[0].flags & mv::kMoveFlagGuardMove) != 0;
            kept = kept && g;
            ++kept_frames;
            continue;
        }
        const Vec3 cp = st.position(st.find(carrier));
        carrier_moved = std::abs(cp.y - c0.y) > 300.0f;
        for (const objects::ObjectHandle h : st.owned_by(1))
            if (st.alive(h) && b->movement().has_unit(st.id(h)) && b->combat().has_unit(st.id(h)) &&
                b->movement().view(st.id(h)).locomotor != mv::LocomotorKind::Fighter) {
                enemy = st.id(h);
                break;
            }
        CHECK(enemy != objects::kNoObjectId);
        if (enemy == objects::kNoObjectId) break;
        const Vec3 ep = st.position(st.find(enemy));
        for (size_t k = 0; k < members.size(); ++k)
            CHECK(b->teleport(*tm, members[k], Vec3{ep.x + 15.0f * static_cast<float>(k + 1), ep.y + 20.0f, ep.z}, Vec3{1, 0, 0}));
        std::vector<uint32_t> wire;
        for (const objects::ObjectId m : members) wire.push_back(static_cast<uint32_t>(m));
        CHECK(ai->execute(0, commands::ev::attack(static_cast<uint32_t>(enemy), wire)) == R::None);
        const mv::FighterFormation* af = b->movement().fighter_formation(squad);
        attack_stack = af && af->blocks.size() == 1 && af->blocks[0].kind == mv::kFighterBlockObject && af->blocks[0].object == enemy &&
                       af->blocks[0].flags == 0;
        const FighterFormationRecord* ar = b->fighter_formation(squad);
        attack_record = ar && ar->flags == 0 && ar->goal_object == enemy;
        break;
    }
    std::printf("  AI orders: squadron %d, Lua guard of the carrier: stack %d record %d, kept while the carrier moves %d "
                "(%u frames, moved %d); AI attack in range on %d: stack %d record %d\n",
                squad, guard_stack ? 1 : 0, guard_record ? 1 : 0, kept ? 1 : 0, kept_frames, carrier_moved ? 1 : 0, enemy,
                attack_stack ? 1 : 0, attack_record ? 1 : 0);
    CHECK(squad != objects::kNoObjectId && guard_stack && guard_record && kept && kept_frames > 0 && carrier_moved);
    CHECK(attack_stack && attack_record);
}

// s78f review (FIGHTER_FLYING §4.11 B 1, §4.10 E): an auto block on a station (a non-mover: no movement unit) is dropped
// when the station dies - the death reaches MovementSystem::object_removed, and the position hook reads only live objects.
// The player-0 star base is handed to player 1 so player 0's TIE squadron, ordered next to it, auto-attacks it; player 0's
// frigate and corvette are ordered to attack it too so it dies inside the test.
void test_fighter_station_dies() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, false)) return;
    const uint32_t sb_type = type_of(*fb.d, "S10_Star_Base_1");
    objects::ObjectId squad = objects::kNoObjectId, station = objects::kNoObjectId;
    std::vector<objects::ObjectId> members, attackers;
    bool pushed = false, died = false, dropped = false, record_dropped = false;
    uint32_t push_frame = 0, death_frame = 0;
    for (uint64_t t = 0; t < 9000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (squad != objects::kNoObjectId) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (squad == objects::kNoObjectId) {
            if (b->frame() != 3) continue;
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && st.owner(st.find(s.members.front())) == 0) {
                    squad = s.id;
                    members = s.members;
                }
            std::vector<objects::ObjectId> ships;
            for (const objects::ObjectHandle h : st.owned_by(0)) {
                if (!st.alive(h)) continue;
                if (st.type(h) == sb_type) station = st.id(h);
                else if (b->movement().has_unit(st.id(h)) && b->movement().view(st.id(h)).locomotor != mv::LocomotorKind::Fighter)
                    ships.push_back(st.id(h));
            }
            CHECK(squad != objects::kNoObjectId && station != objects::kNoObjectId);
            if (squad == objects::kNoObjectId || station == objects::kNoObjectId) break;
            CHECK(!b->movement().has_unit(station));  // a non-mover
            CHECK(b->change_owner(*tm, st.find(station), 1));
            const Vec3 sp = st.position(st.find(station));
            for (size_t k = 0; k < members.size(); ++k)
                CHECK(b->teleport(*tm, members[k], Vec3{sp.x + 500.0f + 40.0f * static_cast<float>(k), sp.y, sp.z}, Vec3{-1, 0, 0}));
            CHECK(b->order_move(*tm, 0, members, sp.x + 500.0f, sp.y, fb.host->random().synced()) == R::None);
            attackers = ships;
            continue;
        }
        const objects::ObjectHandle sh = st.find(station);
        const bool alive = st.alive(sh);
        if (alive) {
            if (!pushed)
                for (const objects::ObjectId k : b->fighter_pushed(squad))
                    if (k == station) {
                        pushed = true;
                        push_frame = b->frame();
                    }
            // A2k integration: the ships are ordered once the auto block is on the station, so the station cannot die
            // before the squadron's auto attack starts (with them ordered at frame 3 it died at frame 27, stream-dependent).
            if (pushed && !attackers.empty()) {
                CHECK(b->order_attack(*tm, 0, attackers, station, fb.host->random().synced()) == R::None);
                attackers.clear();
            }
            if (b->frame() > 8000) break;
            continue;
        }
        died = true;
        death_frame = b->frame();
        dropped = true;
        if (const mv::FighterFormation* ff = b->movement().fighter_formation(squad))
            for (const mv::FighterBlock& bl : ff->blocks) dropped = dropped && !(bl.kind == mv::kFighterBlockObject && bl.object == station);
        const FighterFormationRecord* r = b->fighter_formation(squad);
        record_dropped = !r || r->goal_object != station;
        break;
    }
    std::printf("  station dies: squadron %d, station %d; auto block on it %d (frame %u), died %d (frame %u), blocks dropped "
                "at once %d, record %d\n",
                squad, station, pushed ? 1 : 0, push_frame, died ? 1 : 0, death_frame, dropped ? 1 : 0, record_dropped ? 1 : 0);
    CHECK(pushed && died && dropped && record_dropped);
}

// S7 t22 (FIGHTER_FLYING §4.9 step 3): a goal hidden by fog for the squadron's player is still its formation goal, so the
// squadron toggles Idle <-> Directed_Combat every frame (Directed_Combat: fogged -> NoTarget -> enter_idle; Idle: a goal
// that is not dying -> Directed_Combat), as in retail. Here the goal is an ordered Attack on an enemy frigate that no
// player-0 REVEAL source sees (the fighters reveal nothing); enter_idle keeps the order's object block.
// s78f review: the Attack's movement formation now holds the object block (exec_attack, §4.11 B 1), so after arrival the
// hidden-goal rule C 3 re-plans it toward the object (d >= 40: state 0, the plan's move, state 1, the members done at
// once, state 3 again): every third frame is a plan frame, so the state changes twice per three frames.
void test_fighter_fog_toggle() {
    namespace mv = tactical::movement;
    using R = commands::RejectReason;
    FighterBattle fb;
    if (!start_fighter_battle(fb, true)) return;
    std::vector<objects::ObjectId> members;
    objects::ObjectId squad = objects::kNoObjectId, enemy = objects::kNoObjectId;
    bool issued = false, stayed_hidden = true, order_kept = true;
    uint32_t settle = 0, window = 0, toggles = 0;
    mv::LocoState prev = mv::LocoState::None;
    for (uint64_t t = 0; t < 6000; ++t) {
        fb.host->tick_once();
        TacticalBattle* b = fb.battles->battle();
        TacticalMode* tm = fb.host->tactical();
        if (!b || !tm) {
            if (issued) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        if (!issued) {
            if (b->frame() != 3) continue;
            issued = true;
            for (const mv::FighterSquad& s : b->movement().fighter_squads())
                if (!s.members.empty() && st.owner(st.find(s.members.front())) == 0) {
                    members = s.members;
                    squad = s.id;
                }
            const Vec3 sp = members.empty() ? Vec3{} : st.position(st.find(members.front()));
            float best = 0.0f;
            for (const objects::ObjectHandle h : st.owned_by(1)) {
                if (!st.alive(h) || !b->movement().has_unit(st.id(h)) || !b->fog_object_hidden(0, st.id(h), true)) continue;
                const Vec3 p = st.position(h);
                const float d2 = (p.x - sp.x) * (p.x - sp.x) + (p.y - sp.y) * (p.y - sp.y);
                if (enemy == objects::kNoObjectId || d2 < best) {
                    enemy = st.id(h);
                    best = d2;
                }
            }
            CHECK(members.size() == 3 && enemy != objects::kNoObjectId);
            if (members.empty() || enemy == objects::kNoObjectId) break;
            CHECK(b->order_attack(*tm, 0, members, enemy, fb.host->random().synced()) == R::None);
            continue;
        }
        const objects::ObjectId lead = members.front();
        if (!st.alive(st.find(lead)) || !st.alive(st.find(enemy))) break;
        const mv::UnitView v = b->movement().view(lead);
        if (settle == 0 && !v.has_goal && v.loco_state != mv::LocoState::FighterMoving) settle = b->frame();
        if (settle != 0 && b->frame() > settle + 5) {
            ++window;
            if (v.loco_state != prev) ++toggles;
            stayed_hidden = stayed_hidden && b->fog_object_hidden(0, enemy, true);
            order_kept = order_kept && b->combat().ordered_target(lead) == enemy;
            if (window >= 60) break;
        }
        prev = v.loco_state;
    }
    const FighterFormationRecord* r = nullptr;
    if (TacticalBattle* b = fb.battles->battle()) r = b->fighter_formation(squad);
    std::printf("  fog toggle: squadron %d attacks hidden enemy %d; settled at %u, %u state changes in %u frames, hidden %d, order kept %d\n",
                squad, enemy, settle, toggles, window, stayed_hidden ? 1 : 0, order_kept ? 1 : 0);
    CHECK(window >= 60 && stayed_hidden && order_kept && toggles >= 38);
    CHECK(r && r->goal_object == enemy);
}


int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_temp_prefix = "alamo_sim_battle_fighter_tests";
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--regen") g_regen = true;
    std::printf("S7 t12: a fighter squadron moved past an enemy keeps its move order (formation attack target)\n");
    test_fighter_move_past_enemy();
    std::printf("S7 t22: squadron auto targets through the formation push (chase test, Stop, idle chase, enter_idle)\n");
    test_fighter_chase_rule();
    test_fighter_auto_push();
    std::printf("S7 t24: a carrier's launch guards the carrier (Guard_Chase_Range, follows it)\n");
    test_hangar_escort_guard();
    std::printf("s78f review: host record vs the movement stack (shift waypoint, attack order, a dying station)\n");
    test_escort_waypoint();
    test_escort_attack_in_range();
    test_ai_fighter_blocks();  // s78f t10
    test_fighter_station_dies();
    std::printf("S7 t22: a fogged formation goal toggles Idle <-> Directed_Combat\n");
    test_fighter_fog_toggle();
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_battle_fighter_tests: all passed\n");
    return 0;
}

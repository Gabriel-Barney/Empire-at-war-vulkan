#pragma once
// Split out of test_battles_ai.cpp (bo3 t03): shared includes, types and helpers; the tests live in test_battles_ai_partN.cpp.
// alamo_sim_battle_ai_tests: S10 battle transitions, host side (docs/re/BATTLE_TRANSITIONS.md): the tactical AI in battles (BattleAi orders, find team, land drop, constants), land teams, landing objects.
// Split out of test_battles.cpp (WF1b t01); the fixture (synthetic data, maps, run_campaign) is battle_test_fixture.hpp.

#include "battle_test_fixture.hpp"
#include "alamo/core/host_fs.hpp"

#include <array>
#include <iterator>

namespace bo3split {

#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
void test_battle_ai_orders(const Data& d);
void test_space_obstacle_orders();
void test_neutral_attack_move();
void test_battle_ai_station_level(bool skirmish);
void test_battle_ai_land_mode(bool skirmish, bool control_points = false);
void test_battle_ai_find_team();
void test_battle_map_hints();
void test_battle_attack_team();
void test_land_team_targeting();
void test_team_speed_and_owner();
void test_land_squad_container();
void test_land_squad_garrison();
void test_team_convert();
void test_team_hack_bribe(bool bribe);
void test_drop_extents_no_model();
void test_vehicle_thief(int variant);

// s78n t06 (HERO_CLASH §2-§6): two enemy named heroes (S10_HeroA defends, S10_HeroB invades) and a melee entry with a
// map-wide Clash_Range. The clash starts at a detection frame (global frame % 15 == 0), both heroes get +0x3b4 (combat
// kUnitHeroClash): movement locked, no current target, nobody targets them; they slide to their spots Combat_Distance
// apart and exchange until one dies; the survivor is released. The per-tick battle / combat / movement hashes are returned
// for the worker-count check (one ctest entry per worker count: the --hero-clash mode of main).
// s78n t06 review:
//   * the abilities see the clash's lock (AbilitySystem::locked: can(HUNT) refused while locked, allowed after release);
//   * `save_load`: the session is saved mid-fight and loaded into a fresh session, which then runs on with the
//     original's per-tick digests and CRC log, bit for bit;
//   * `queue`: hero A (left to the test's orders) has a move and three shift-queued moves when the clash data comes on:
//     the queue is dropped at the lock (A1_READINGS §28.9: a locked unit takes no queued order) and never popped into a
//     move while the clash runs or after it (before: the snap / hold cleared the goal and the queue drained into moves).
namespace tc = alamo::tactical::combat;
namespace mv = alamo::tactical::movement;
namespace ta = alamo::tactical::abilities;

inline std::shared_ptr<tc::HeroClashData> hero_clash_data(const char* first, const char* second) {
    auto data = std::make_shared<tc::HeroClashData>();
    tc::HeroClashEntry e;
    e.name = "S10_Duel";
    e.clash_type = tc::kClashMelee;
    e.clash_range = 5000.0f;
    e.combat_distance = 17.65f;
    e.damage_percentage = 0.15f;
    e.first_win = 0.4f;
    e.second_win = 0.3f;
    e.first_multiplier = 2.5f;
    e.second_multiplier = 1.0f;
    e.first_type = first;
    e.second_type = second;
    e.first_anims = {"HC_WIN", "HC_LOSE", "HC_DRAW", "IDLE"};
    e.second_anims = e.first_anims;
    data->entries.push_back(e);
    return data;
}

// One campaign session with a land battle at S10_B: `defenders` companies for player 0 at planet 1, `invaders` for
// player 1 at planet 2. `launch`: the invasion fleet is launched and moved (a fresh session that loads a save does not).
struct ClashSession {
    std::unique_ptr<jobs::JobSystem> js;
    std::unique_ptr<SimHost> host;
    galactic::GalacticSim* gs = nullptr;
    std::unique_ptr<CampaignBattles> battles;
    bool invaded = false;

    ClashSession(const Data& d, uint32_t workers, const CampaignBattleOptions& opt, std::initializer_list<const char*> defenders,
                 std::initializer_list<const char*> invaders, bool launch) {
        js = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{static_cast<int32_t>(workers)});
        SimConfig cfg;
        cfg.seed = 2026;
        cfg.players = 4;
        host = std::make_unique<SimHost>(d.types, *js, cfg);
        galactic::GalacticSetup setup = make_setup(d, true);
        for (const char* co : defenders) setup.forces.push_back({0, 1, type_of(d, co)});
        for (const char* co : invaders) setup.forces.insert(setup.forces.begin(), {1, 2, type_of(d, co)});
        gs = host->start_galactic(d.rules, setup);
        CHECK(gs != nullptr);
        if (!gs) return;
        battles = std::make_unique<CampaignBattles>(*host, d.crules, *d.catalog,
                                                    [](const std::string& name, terrain::MapData& m) {
                                                        if (name != "S10_Land_B") return false;
                                                        m = land_map();
                                                        return true;
                                                    },
                                                    opt);
        CHECK(battles->attach(nullptr));
        if (!launch) return;
        std::vector<objects::ObjectHandle> up;
        for (objects::ObjectHandle h : gs->planets()[2].surface)
            if (gs->store().owner(h) == 1) up.push_back(h);
        CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
        const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
        if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    }
    // One tick; the invasion once the fleet is in orbit. The running battle (null between battles).
    TacticalBattle* tick() {
        host->tick_once();
        if (!invaded && gs && host->modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        return battles ? battles->battle() : nullptr;
    }
    objects::ObjectId find(const TacticalBattle& b, uint32_t type) const {
        const objects::ObjectStore& st = host->tactical()->primary();
        const tc::CombatSystem& cs = b.combat();
        for (size_t i = 0; i < cs.unit_count(); ++i) {
            const auto id = cs.unit_id_at(i);
            const objects::ObjectHandle h = st.find(id);
            if (st.alive(h) && st.type(h) == type) return id;
        }
        return objects::kNoObjectId;
    }
};
uint64_t clash_digest(const TacticalBattle& b);
bool goal_at(const mv::UnitView& v, const std::vector<Vec3>& points);

struct HeroClashRun {
    bool started = false, flagged = true, locked = true, untargeted = true, ended = false, released = false;
    bool hunt_locked = true, hunt_after = false;                       // review finding 3
    bool save_ok = false, load_ok = false, load_same = false;          // review finding 7
    bool queued = false, queue_dropped = false, queue_moved = false;   // review finding 1
    uint32_t start_frame = 0, end_frame = 0, exchanges = 0, save_tick = 0, compared = 0;
    float health_a = 0.0f, health_b = 0.0f;
    std::vector<uint64_t> digests;
};
HeroClashRun run_hero_clash_battle(const Data& d, uint32_t workers, bool save_load = false, bool queue = false);

// s78n t12 review (HERO_CLASH §8.1 / §8.4.3, REMAINING_GAPS §5.1): the thief and the stolen vehicle. A battle at S10_B
// with S10_SpeederC (clone S10_SpeederC_Captured: ability 46, no garrison behaviour - the shape of 17 of the 18 shipped
// *_Captured types) for the Empire and one or two thief companies for the Underworld.
struct ThiefScene {
    ClashSession s;
    uint32_t thief_t, speeder_t, clone_t;
    uint8_t steal, eject;
    objects::ObjectId thief = objects::kNoObjectId, thief2 = objects::kNoObjectId, speeder = objects::kNoObjectId,
                      clone = objects::kNoObjectId;
    TacticalBattle* b = nullptr;
    TacticalMode* tm = nullptr;

    static CampaignBattleOptions options() {
        CampaignBattleOptions opt;
        opt.conflict.human_choice = galactic::BattleChoice::Fight;
        opt.battle.max_frames = 3000;
        return opt;
    }
    // `two_thieves`: one company of two S10_Thief (a second company would wait in reserve).
    ThiefScene(const Data& d, const char* thief_name, bool two_thieves, bool launch)
        : s(d, 0, options(), {"S10_SpeederC_Co"}, {two_thieves ? "S10_ThiefPair_Co" : thief_name_co(thief_name)}, launch),
          thief_t(type_of(d, thief_name)), speeder_t(type_of(d, "S10_SpeederC")), clone_t(type_of(d, "S10_SpeederC_Captured")),
          steal(ta::ability_type_from_name("CAPTURE_VEHICLE")), eject(ta::ability_type_from_name("EJECT_VEHICLE_THIEF")) {}
    static const char* thief_name_co(const char* thief_name) {
        return std::string_view(thief_name) == "S10_Thief" ? "S10_Thief_Co" : "S10_ThiefN_Co";
    }
    bool tick() {
        b = s.tick();
        tm = s.host->tactical();
        return b && tm && b->frame() >= 1;
    }
    // Runs until the battle stands, then finds the thief (the lowest id of its type), a second one and the speeder.
    bool start() {
        for (int i = 0; i < 6000; ++i) {
            if (!tick() || b->frame() < 3) continue;
            const objects::ObjectStore& st = tm->primary();
            const tc::CombatSystem& cs = b->combat();
            for (size_t k = 0; k < cs.unit_count(); ++k) {
                const auto id = cs.unit_id_at(k);
                const objects::ObjectHandle h = st.find(id);
                if (!cs.alive(id) || !st.alive(h)) continue;
                if (st.type(h) == thief_t) (thief == objects::kNoObjectId ? thief : thief2) = id;
                if (st.type(h) == speeder_t) speeder = id;
            }
            return thief != objects::kNoObjectId && speeder != objects::kNoObjectId;
        }
        return false;
    }
    bool order_theft() {
        const objects::ObjectId users[] = {thief};
        return b->abilities().targeted_ability(users, steal, speeder, -1, b->frame()) == 1u;
    }
    // Ticks until the thief sits in the capture clone (60 ticks at most).
    bool wait_boarded() {
        for (int i = 0; i < 60; ++i) {
            if (!tick()) return false;
            clone = s.find(*b, clone_t);
            if (clone != objects::kNoObjectId && b->garrison_holder_of(thief) == clone) return true;
        }
        return false;
    }
    Vec3 pos(objects::ObjectId id) const { return tm->primary().position(tm->primary().find(id)); }
};
void test_thief_release(const Data& d, int how);
void test_thief_save_load(const Data& d, bool no_garrison_unit);
void test_command_kill_save_load(const Data& d, int how);
void test_thief_dies_same_tick(const Data& d);
void test_hero_clash_battle(const Data& d);
int hero_clash_worker_check(int argc, char** argv);
void test_hero_clash_garrison(const Data& d);
void test_reserve_drop_facing(bool attacker_marker);
void test_battle_ai_land_drop(const Data& d);
void test_tactical_ai_constants_load();
#endif

}  // namespace
using namespace bo3split;

inline void test_landing_drop(bool timed, uint32_t destroy_at = 0) {
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
            // thrown sabers); cu8l t03: version 11 (the start fleet arrivals); s78n t20 stage A: version 12 (the land attack
            // blocks); fleet cohesion: version 13 (per-player cohesion; exact: every other version is refused): 11, 12 and 14
            // are refused.
            std::vector<uint8_t> stale(w.bytes().begin(), w.bytes().end());
            for (const uint32_t v : {11u, 12u, 14u}) {
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
inline void test_vehicle_landing_exit() {
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

// s78n t18 review: the land squad garrison battle of test_land_squad_garrison (squad_types(true): the human's squads and
// the garrison vehicle S10_Walker against the invading squads) as a session that can be saved and loaded.
struct SquadGarrisonScene {
    ClashSession s;
    uint32_t walker_t;
    objects::ObjectId team = objects::kNoObjectId, walker = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    TacticalBattle* b = nullptr;
    TacticalMode* tm = nullptr;

    static CampaignBattleOptions options() {
        CampaignBattleOptions opt;
        opt.conflict.human_choice = galactic::BattleChoice::Fight;
        opt.battle.max_frames = 5000;
        opt.battle.skip_human_units = true;  // the stand-in leaves the human's units to the test's orders
        return opt;
    }
    SquadGarrisonScene(const Data& d, bool launch) : s(d, 0, options(), {"S10_Walker_Co"}, {}, launch), walker_t(type_of(d, "S10_Walker")) {}
    bool tick() {
        b = s.tick();
        tm = s.host->tactical();
        return b && tm && b->frame() >= 1;
    }
    // Runs until the battle stands, then finds the human's first squad (>= 2 members, lowest team id) and the walker.
    bool start() {
        for (int i = 0; i < 12000; ++i) {
            if (!tick() || b->frame() < 3) continue;
            const objects::ObjectStore& st = tm->primary();
            const tc::CombatSystem& cs = b->combat();
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t k = 0; k < cs.unit_count(); ++k) {
                const auto id = cs.unit_id_at(k);
                if (!cs.alive(id)) continue;
                if (cs.team(id) != 0) teams[cs.team(id)].push_back(id);
                if (st.type(st.find(id)) == walker_t) walker = id;
            }
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() >= 2 && team == objects::kNoObjectId && st.alive(th) && st.owner(th) == 0) {
                    team = static_cast<objects::ObjectId>(tid);
                    members = m;
                }
            }
            return team != objects::kNoObjectId && walker != objects::kNoObjectId;
        }
        return false;
    }
    size_t inside() const {
        size_t n = 0;
        for (const auto id : members) n += b->garrison_holder_of(id) == walker ? 1u : 0u;
        return n;
    }
};

// s78n t18 review (items 6, 9, 10): a battle-level save / load while a squad garrisons - during the approach, during the
// entry fade, with the whole squad inside (its container hidden) and during the walk-out. Each loaded session has, right
// after the load and before any tick, the saving session's digest and the same movement pair gates / "hidden" for the
// container and every member still a mover (the host feeds them after a load: an order then meets the same gates), and
// runs on tick for tick with the saving session - through the entry, the leave order and the walk-out. Last: a garrison
// order on a hostile host is refused before the units' queues are cleared and before they are sent walking.
// s78n t20 batch 2 (FIGHTER_FLYING §7.4 "Garrison approach in full" items 4 - 5): a squad garrisons a garrison vehicle
// that drives off. The squad's container is put 50 behind the walker, the walker is ordered 400 ahead and the squad into
// it in the same frame. The squad's approach move ends where the walker stood; within 60 of the walker (3-D distance -
// the squad's radius) the squad goes to state 2 and the walker - still driving the tick before - is stopped in that tick
// (HostStop); the corrective move (flag 0x20, the whole excess toward the walker) then takes the squad in. Bunkers and
// single units take the same code (GarrisonSystem; alamo_tactical_garrison_tests test_approach).
inline void test_land_squad_garrison_host_stop() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, false, true);
    SquadGarrisonScene a(*d, true);
    CHECK(a.s.gs != nullptr);
    if (!a.s.gs || !a.start()) {
        CHECK(false);
        return;
    }
    using R = commands::RejectReason;
    for (int i = 0; i < 300; ++i) CHECK(a.tick());  // the squad lays out (a strung-out squad's radius is large)
    objects::ObjectStore& st = a.tm->primary();
    const Vec3 wp = st.position(st.find(a.walker));
    CHECK(a.b->teleport(*a.tm, a.team, Vec3{wp.x - 80.0f, wp.y, wp.z}, st.facing(st.find(a.team))));
    const objects::ObjectId w1[] = {a.walker};
    CHECK(a.b->order_garrison(*a.tm, 0, a.members, a.walker, a.s.host->random().synced()) == R::None);
    const uint32_t f0 = a.b->frame();
    uint32_t closing = 0, inside = 0, dropped = 0, drive = 0;
    bool stopped = false;
    float walker_moved = 0.0f, gap = -1.0f;
    for (int i = 0; i < 2000 && inside == 0 && dropped == 0; ++i) {
        // the reservation stopped the walker (host vf +0xe0); when the squad is 40 short of it the player drives it on
        if (drive == 0 && a.tm->primary().position(a.tm->primary().find(a.team)).x >= wp.x - 40.0f) {
            CHECK(a.b->order_move(*a.tm, 0, w1, wp.x + 400.0f, wp.y, a.s.host->random().synced()) == R::None);
            drive = a.b->frame();
        }
        const bool driving = a.b->movement().view(a.walker).has_goal;
        if (!a.tick()) break;
        const uint8_t state = a.b->garrison().state_of(a.members.front());
        const objects::ObjectStore& s2 = a.tm->primary();
        if (state == 2 && closing == 0) {
            closing = a.b->frame();
            stopped = driving && !a.b->movement().view(a.walker).has_goal;
            const Vec3 now = s2.position(s2.find(a.walker)), kp = s2.position(s2.find(a.team));
            walker_moved = std::sqrt((now.x - wp.x) * (now.x - wp.x) + (now.y - wp.y) * (now.y - wp.y));
            gap = std::sqrt((now.x - kp.x) * (now.x - kp.x) + (now.y - kp.y) * (now.y - kp.y));
        }
        if (state == 0 && a.inside() == 0) dropped = a.b->frame();
        if (a.inside() == a.members.size()) inside = a.b->frame();
    }
    std::printf("  garrison host stop: order %u, walker driven at %u, state 2 at %u (walker %.1f from its start, squad %.1f from "
                "it, stopped %d), inside at %u, dropped at %u\n",
                f0, drive, closing, static_cast<double>(walker_moved), static_cast<double>(gap), stopped ? 1 : 0, inside, dropped);
    CHECK(drive > f0 && closing > drive && stopped && walker_moved > 0.0f && dropped == 0 && inside >= closing);
}

// s78n t20 batch 2 item 2.4 (MOVEMENT §5.3 "Hover in full"): the squad-garrison data with the walker made a GARRISON_HOVER
// type with Hover_Height 80 (as Gallofree_HTT_Transport; no Deploys), reloaded from the rewritten S10Types.xml.
inline std::unique_ptr<Data> hover_garrison_data() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, false, true);
    const fs::path f = d->root / "Data/XML/S10Types.xml";
    std::string t;
    {
        std::ifstream in(f, std::ios::binary);
        t.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::string beh = "WALK_LOCOMOTOR, GARRISON_VEHICLE", head = "<GroundVehicle Name=\"S10_Walker\">";
    const size_t pb = t.find(beh), ph = t.find(head);
    CHECK(pb != std::string::npos && ph != std::string::npos);
    if (pb == std::string::npos || ph == std::string::npos) return d;
    t.replace(pb, beh.size(), "WALK_LOCOMOTOR, GARRISON_HOVER");
    t.insert(t.find(head) + head.size(), "<Hover_Height>80</Hover_Height>");
    write_file(f, t);
    auto h = std::make_unique<Data>();
    h->root = d->root;
    h->files = std::make_unique<DirectoryProvider>(path_to_utf8(h->root));
    std::vector<std::string> diag;
    CHECK(h->db.load(*h->files, {}, &diag));
    CHECK(h->raw.load(*h->files));
    CHECK(h->types.build(h->raw, h->files.get(), &diag));
    CHECK(galactic::load_galactic_rules(*h->files, h->db, h->types, h->rules, &diag));
    CHECK(galactic::load_conflict_rules(*h->files, h->db, h->types, h->rules, h->crules, &diag));
    h->catalog = std::make_unique<BattleCatalog>(h->types, h->files.get(), &h->db.constants(), &diag);
    return h;
}

// s78n t20 batch 2 item 2.4 (MOVEMENT §5.3 "Hover in full"; death 0x391310, eject request 0x635fc0): a squad garrisons
// the hover walker (the reservation lowers it); the walker is then driven off and rises to the ground + 80.
//   raised: killed in the air - the squad inside is killed with it;
//   grounded: killed on the ground - the squad is ejected alive;
//   eject: asked to eject while raised and driving - it is stopped and lowers at once.
inline void test_land_hover_garrison_death() {
    const auto d = hover_garrison_data();
    using R = commands::RejectReason;
    enum Case { kRaised, kGrounded, kEject };
    for (const Case c : {kRaised, kGrounded, kEject}) {
        SquadGarrisonScene a(*d, true);
        CHECK(a.s.gs != nullptr);
        if (!a.s.gs || !a.start()) {
            CHECK(false);
            return;
        }
        for (int i = 0; i < 300; ++i) CHECK(a.tick());
        objects::ObjectStore& st = a.tm->primary();
        const Vec3 wp = st.position(st.find(a.walker));
        CHECK(a.b->teleport(*a.tm, a.team, Vec3{wp.x - 60.0f, wp.y, wp.z}, st.facing(st.find(a.team))));
        CHECK(a.b->order_garrison(*a.tm, 0, a.members, a.walker, a.s.host->random().synced()) == R::None);
        uint32_t inside = 0;
        for (int i = 0; i < 2000 && inside == 0; ++i) {
            if (!a.tick()) break;
            if (a.inside() == a.members.size()) inside = a.b->frame();
        }
        CHECK(inside > 0);
        const mv::MovementSystem& mv = a.b->movement();
        for (int i = 0; i < 100 && mv.hover_state(a.walker) != 0; ++i) CHECK(a.tick());
        CHECK(mv.hover_state(a.walker) == 0);
        const float ground = st.position(st.find(a.walker)).z;
        float z = ground;
        if (c != kGrounded) {  // driven off: it rises, then moves at the ground + 80
            const objects::ObjectId w1[] = {a.walker};
            CHECK(a.b->order_move(*a.tm, 0, w1, wp.x + 400.0f, wp.y, a.s.host->random().synced()) == R::None);
            for (int i = 0; i < 400 && !(mv.hover_state(a.walker) == 1 && mv.view(a.walker).speed > 0.0f); ++i) CHECK(a.tick());
            z = st.position(st.find(a.walker)).z;
            CHECK(mv.hover_state(a.walker) == 1 && std::fabs(z - (ground + 80.0f)) < 1e-3f);
        }
        if (c == kEject) {
            CHECK(mv.view(a.walker).has_goal);
            CHECK(a.b->eject_garrison(*a.tm, a.walker) > 0);
            CHECK(!mv.view(a.walker).has_goal && mv.hover_state(a.walker) == 3);
            std::printf("  hover garrison: eject while raised (z %.1f over %.1f) - stopped, lowering\n", static_cast<double>(z - ground),
                        static_cast<double>(ground));
            continue;
        }
        CHECK(a.b->script_damage(*a.tm, a.walker, 1.0e9f, false));
        for (int i = 0; i < 5; ++i) CHECK(a.tick());
        uint32_t alive = 0, held = 0;
        for (const auto m : a.members) {
            alive += a.b->combat().alive(m) ? 1u : 0u;
            held += a.b->garrison_holder_of(m) != objects::kNoObjectId ? 1u : 0u;
        }
        std::printf("  hover garrison: walker killed %s (z %.1f over the ground) - %u of %zu members alive, %u still inside\n",
                    c == kRaised ? "raised" : "grounded", static_cast<double>(z - ground), alive, a.members.size(), held);
        CHECK(held == 0 && alive == (c == kRaised ? 0u : static_cast<uint32_t>(a.members.size())));
    }
}

// s78n t20 stage A (FIGHTER_FLYING §4.11 A / E, MOVEMENT §4.3.2 items 6 / 8): the land attack re-check. The human's squad
// (S10_Squad, LAND_TEAM_CONTAINER_LOCOMOTOR) attacks a trooper of the invader's squads; every unit is invulnerable and
// the stand-in is off, so only the order and its attack block move it.
//   blockers false (test i): ordered from 150 (it can hit: no path), then teleported 60 units outside its range: the
//   block's next 30-frame check re-approaches (a goal within 31 frames) and a member fires within 300 frames.
//   blockers true (test ii): two more human squads stand on the approach line at 0.76 and 0.92 x range; the squad,
//   ordered from range + 150, ends where it can hit and fires within 300 frames of that.
//   save_load (test iii): saved 10 frames after the teleport (the block alive), loaded into a fresh session, both run
//   400 ticks in lockstep with the same digest every tick.
//   use_ai (s78n t20 step 5): the blockers case with the order given through BattleAi (the AI host's Attack).
// s78n t20 step 5: every tick the movement collision info the host fed (TacticalBattle::feed_collision_info) is checked
// for the squad container and its first member: a target inside a squad is named by its container, at the targeted
// member's position, with the container's collision radius and the shooter's attack range; after the load the fed
// info equals the uninterrupted run's tick for tick.
struct LandAttackBlockRun {
    bool ordered = false, gated = false, teleported = false, regoal = false, fired = false, ended_in_range = false;
    bool saved = false, loaded = false, same = false, info_same = true;
    uint32_t regoal_frames = 0, fire_frames = 0, end_frame = 0, compared = 0;
    uint32_t info_frames = 0, info_bad = 0;
    size_t blocks_at_save = 0;
    float range = 0.0f;
};

// The fed info of `id` as a comparable tuple.
inline std::array<float, 6> fed_info(const mv::MovementSystem& ms, objects::ObjectId id) {
    mv::MovementSystem::CollisionInfo ci;
    if (!ms.collision_info(id, ci)) return {-2.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    return {static_cast<float>(ci.target), ci.target_pos.x, ci.target_pos.y, ci.target_radius, ci.attack_range, ci.short_range ? 1.0f : 0.0f};
}

// `workers`: the session's job workers; `digests` (s78n t20 review): every tick's digest from the order stage on.
inline LandAttackBlockRun run_land_attack_block(const Data& d, bool blockers, bool save_load, bool use_ai = false, uint32_t workers = 0,
                                         std::vector<uint64_t>* digests = nullptr) {
    LandAttackBlockRun out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.skip_human_units = true;
    opt.battle.order_interval = 0;  // no stand-in orders
    const auto session = [&](bool launch) {
        return blockers ? std::make_unique<ClashSession>(d, workers, opt, std::initializer_list<const char*>{"S10_Squad", "S10_Squad"},
                                                         std::initializer_list<const char*>{}, launch)
                        : std::make_unique<ClashSession>(d, workers, opt, std::initializer_list<const char*>{},
                                                         std::initializer_list<const char*>{}, launch);
    };
    auto s = session(true);
    if (!s->gs) return out;
    int stage = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId team = objects::kNoObjectId, target = objects::kNoObjectId;
    std::vector<objects::ObjectId> members, others;
    Vec3 dir{};
    SaveFile save;
    std::vector<uint64_t> after_save;
    std::vector<std::array<float, 6>> info_after_save;
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d.files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    for (uint64_t t = 0; t < 9000 && stage < 9; ++t) {
        TacticalBattle* b = s->tick();
        TacticalMode* tm = s->host->tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        auto& cs = const_cast<tc::CombatSystem&>(b->combat());
        const mv::MovementSystem& ms = b->movement();
        if (digests && stage >= 1) digests->push_back(clash_digest(*b));
        if (out.saved) {
            after_save.push_back(clash_digest(*b));
            info_after_save.push_back(fed_info(ms, team));
        }
        if (stage >= 2 && cs.alive(target)) {
            // the fed info (this tick's feed ran before the movement tick): the squad and its first member
            for (const objects::ObjectId id : {team, members.front()}) {
                mv::MovementSystem::CollisionInfo ci;
                if (!ms.collision_info(id, ci) || ci.target < 0) continue;
                ++out.info_frames;
                // (the feed ran before this tick's movement and combat: the targeted member may have moved a step or
                // the shooter switched to another member since, so the member is any of the named squad's members
                // within 3 of the fed position)
                bool member_there = false;
                for (size_t i = 0; i < cs.unit_count() && !member_there; ++i) {
                    const auto mid = cs.unit_id_at(i);
                    if (!cs.alive(mid) || cs.team(mid) != static_cast<uint32_t>(ci.target)) continue;
                    const Vec3 mp = st.position(st.find(static_cast<objects::ObjectId>(mid)));
                    member_there = std::fabs(mp.x - ci.target_pos.x) <= 3.0f && std::fabs(mp.y - ci.target_pos.y) <= 3.0f;
                }
                const bool ok = ms.is_land_team_container(ci.target) && member_there &&
                                ci.target_radius == ms.collision_radius(ci.target) && ci.target_radius > 0.0f &&
                                ci.attack_range == out.range && !ci.short_range;
                if (!ok) ++out.info_bad;
            }
        }
        if (stage == 0) {
            // s78n t26 review round 3: the test data has no reveal source, so the target would be hidden (FIGHTER_FLYING §4.11 C / E: no
            // land stop, no re-path while moving); this scenario is about a visible target (fog: test_land_attack_unseen)
            if (b->fog()) b->fog()->reveal_all(0);
            // the human's squads (ascending container id) and the invader's lowest-id squad member
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                cs.make_invulnerable(id, 1000000, b->frame());
                if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
            }
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() < 2 || !st.alive(th) || !ms.has_unit(static_cast<objects::ObjectId>(tid))) continue;
                if (st.owner(th) == 0) {
                    if (team == objects::kNoObjectId) {
                        team = static_cast<objects::ObjectId>(tid);
                        members = m;
                    } else {
                        others.push_back(static_cast<objects::ObjectId>(tid));
                    }
                } else if (st.owner(th) == 1 && target == objects::kNoObjectId) {
                    target = m.front();
                }
            }
            CHECK(team != objects::kNoObjectId && target != objects::kNoObjectId && (!blockers || others.size() == 2));
            if (team == objects::kNoObjectId || target == objects::kNoObjectId || (blockers && others.size() < 2)) break;
            out.range = cs.attack_range(members.front());
            CHECK(out.range > 100.0f);
            const Vec3 tp = st.position(st.find(target)), ap = st.position(st.find(team));
            const float dx = ap.x - tp.x, dy = ap.y - tp.y, dl = std::sqrt(dx * dx + dy * dy);
            dir = dl > 1.0f ? Vec3{dx / dl, dy / dl, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            const float start = blockers ? out.range + 150.0f : 150.0f;
            CHECK(b->teleport(*tm, team, Vec3{tp.x + dir.x * start, tp.y + dir.y * start, ap.z}, st.facing(st.find(team))));
            for (size_t k = 0; k < others.size() && k < 2; ++k) {
                const float at = (k == 0 ? 0.76f : 0.92f) * out.range;
                const objects::ObjectHandle oh = st.find(others[k]);
                CHECK(b->teleport(*tm, others[k], Vec3{tp.x + dir.x * at, tp.y + dir.y * at, ap.z}, st.facing(oh)));
            }
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        if (!cs.alive(target)) break;
        bool fired = false;
        for (const tc::ShotEvent& ev : cs.shot_events())
            for (const auto id : members)
                fired = fired || (ev.shooter == id && (ev.target == target || cs.team(ev.target) == cs.team(target)));
        if (stage == 1) {
            if (b->frame() < stage_frame + 2) continue;
            if (use_ai) {
                std::vector<uint32_t> wire;
                for (const objects::ObjectId m : members) wire.push_back(static_cast<uint32_t>(m));
                BattleAi bai(*b, *tm, *s->host, ts);
                out.ordered = bai.execute(0, commands::ev::attack(static_cast<uint32_t>(target), wire)) == commands::RejectReason::None;
            } else {
                out.ordered = b->order_attack(*tm, 0, members, target, s->host->random().synced()) == commands::RejectReason::None;
            }
            CHECK(out.ordered);
            // the order-time gate: from 150 the squad can hit, so it takes no path (blockers: it is out of range and walks)
            out.gated = blockers ? ms.view(team).has_goal : !ms.view(team).has_goal;
            stage = 2;
            stage_frame = b->frame();
            continue;
        }
        const Vec3 tp = st.position(st.find(target));
        if (!blockers) {
            if (stage == 2) {
                if (b->frame() < stage_frame + 45) continue;
                const float at = out.range + 60.0f;
                const objects::ObjectHandle th = st.find(team);
                out.teleported = b->teleport(*tm, team, Vec3{tp.x + dir.x * at, tp.y + dir.y * at, st.position(th).z}, st.facing(th));
                CHECK(out.teleported && cs.can_hit(members.front(), target) == tc::CanHit::OutOfMaxRange);
                stage = 3;
                stage_frame = b->frame();
                continue;
            }
            if (stage == 3) {
                if (!out.regoal && ms.view(team).has_goal) {
                    out.regoal = true;
                    out.regoal_frames = b->frame() - stage_frame;
                }
                if (fired) {
                    out.fired = true;
                    out.fire_frames = b->frame() - stage_frame;
                }
                if (save_load && !out.saved && b->frame() == stage_frame + 10) {
                    std::string why;
                    out.blocks_at_save = b->attack_blocks();
                    out.saved = s->host->save_allowed(&why) && s->host->save(save, &why);
                    if (!out.saved) std::printf("  land attack block save: %s\n", why.c_str());
                }
                if (out.fired || b->frame() > stage_frame + 300) stage = 4;
                continue;
            }
        } else {
            if (stage == 2) {
                const bool c = cs.can_hit(members.front(), target) == tc::CanHit::Yes;
                if (c && !ms.view(team).has_goal && !out.ended_in_range) {
                    out.ended_in_range = true;
                    out.end_frame = b->frame() - stage_frame;
                    stage_frame = b->frame();
                    stage = 3;
                }
                if (fired) out.fired = true;  // a member may fire on the way (in range while walking)
                if (b->frame() > stage_frame + 3000) stage = 4;
                continue;
            }
            if (stage == 3) {
                if (fired && !out.fired) {
                    out.fired = true;
                    out.fire_frames = b->frame() - stage_frame;
                }
                if (out.fired || b->frame() > stage_frame + 300) stage = 4;
                continue;
            }
        }
        if (stage == 4) {
            if (!out.saved || after_save.size() < 400) continue;
            stage = 9;
        }
    }
    if (!save_load || !out.saved) return out;
    auto f = session(false);
    if (!f->gs) return out;
    std::string err;
    LoadReport rep;
    out.loaded = f->host->load(save, &err, &rep);
    if (!out.loaded) std::printf("  land attack block load: %s\n", rep.text().c_str());
    f->invaded = true;
    out.same = out.loaded;
    for (size_t i = 0; out.loaded && i < after_save.size() && i < 400; ++i) {
        TacticalBattle* b = f->tick();
        if (!b) {
            out.same = false;
            break;
        }
        ++out.compared;
        if (clash_digest(*b) != after_save[i]) {
            if (out.same) std::printf("  land attack block load: digest differs %zu tick(s) after the load\n", i + 1);
            out.same = false;
        }
        if (i < info_after_save.size() && fed_info(b->movement(), team) != info_after_save[i]) out.info_same = false;
    }
    return out;
}

// s78n t20 review follow-ups (MOVEMENT §4.3.2 item 6; the interim approach point, replaced by t25): the squad data with
// edited troopers, the stand-in off. min_range false: Targeting_Max_Attack_Distance 20 (a melee range, as Vader / Dark_Trooper);
// the human's squad ordered to attack the invader's lowest-id member from 150 walks to where it can hit (before: it aimed
// 0.7 x max(range, 100) = 70 short of the target and stood there). min_range true: Targeting_Min_Attack_Distance 100
// (range 250); a land Attack_Move on the target through BattleAi from 400, then the squad is put 40 from the target (inside
// its minimum range): its block's next 30-frame check re-plans to the staged point min + 0.2 x (range - min) = 130 from the
// target (before: the target's own position), and it ends where it can hit.
struct LandReachRun {
    bool ordered = false, inside = false, replanned = false, reached = false;
    uint32_t replan_frames = 0, reach_frames = 0;
    float range = 0.0f, goal_dist = -1.0f, end_dist = -1.0f;
};

inline LandReachRun run_land_reach(const Data& d, bool min_range) {
    LandReachRun out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.skip_human_units = true;
    opt.battle.order_interval = 0;  // no stand-in orders
    ClashSession s(d, 0, opt, {}, {}, true);
    if (!s.gs) return out;
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d.files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    int stage = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId team = objects::kNoObjectId, target = objects::kNoObjectId;
    std::vector<objects::ObjectId> members;
    Vec3 dir{};
    for (uint64_t t = 0; t < 6000 && stage < 9; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        auto& cs = const_cast<tc::CombatSystem&>(b->combat());
        const mv::MovementSystem& ms = b->movement();
        if (stage == 0) {
            // s78n t26 review round 3: the test data has no reveal source, so the target would be hidden (FIGHTER_FLYING §4.11 C / E: no
            // land stop, no re-path while moving); this scenario is about a visible target (fog: test_land_attack_unseen)
            if (b->fog()) b->fog()->reveal_all(0);
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                cs.make_invulnerable(id, 1000000, b->frame());
                if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
            }
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() < 2 || !st.alive(th) || !ms.has_unit(static_cast<objects::ObjectId>(tid))) continue;
                if (st.owner(th) == 0 && team == objects::kNoObjectId) {
                    team = static_cast<objects::ObjectId>(tid);
                    members = m;
                } else if (st.owner(th) == 1 && target == objects::kNoObjectId) {
                    target = m.front();
                }
            }
            CHECK(team != objects::kNoObjectId && target != objects::kNoObjectId);
            if (team == objects::kNoObjectId || target == objects::kNoObjectId) break;
            out.range = cs.attack_range(members.front());
            const Vec3 tp = st.position(st.find(target)), ap = st.position(st.find(team));
            const float dx = ap.x - tp.x, dy = ap.y - tp.y, dl = std::sqrt(dx * dx + dy * dy);
            dir = dl > 1.0f ? Vec3{dx / dl, dy / dl, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            const float start = min_range ? 400.0f : 150.0f;
            CHECK(b->teleport(*tm, team, Vec3{tp.x + dir.x * start, tp.y + dir.y * start, ap.z}, st.facing(st.find(team))));
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        if (!cs.alive(target)) break;
        const Vec3 tp = st.position(st.find(target));
        const auto dist_to = [&](mv::V2 p) { return std::sqrt((p.x - tp.x) * (p.x - tp.x) + (p.y - tp.y) * (p.y - tp.y)); };
        if (stage == 1) {
            if (b->frame() < stage_frame + 2) continue;
            if (!min_range) {
                out.ordered = b->order_attack(*tm, 0, members, target, s.host->random().synced()) == commands::RejectReason::None;
            } else {
                std::vector<uint8_t> pay;
                const auto put = [&pay](uint32_t v) {
                    for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
                };
                put(static_cast<uint32_t>(members.front()));
                put(static_cast<uint32_t>(target));
                put(std::bit_cast<uint32_t>(tp.x));
                put(std::bit_cast<uint32_t>(tp.y));
                put(std::bit_cast<uint32_t>(tp.z));
                put(0u);
                for (size_t i = 1; i < members.size(); ++i) put(static_cast<uint32_t>(members[i]));
                const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::AttackMove));
                BattleAi bai(*b, *tm, *s.host, ts);
                out.ordered = bai.execute(0, commands::ev::port_extension(sub, pay)) == commands::RejectReason::None;
            }
            CHECK(out.ordered);
            stage = min_range ? 2 : 3;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 2) {  // min_range: 10 frames on the way, then 40 from the target
            if (b->frame() < stage_frame + 10) continue;
            const objects::ObjectHandle th = st.find(team);
            CHECK(b->teleport(*tm, team, Vec3{tp.x + dir.x * 40.0f, tp.y + dir.y * 40.0f, st.position(th).z}, st.facing(th)));
            out.inside = cs.can_hit(members.front(), target) == tc::CanHit::InsideMinRange;
            stage = 3;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 3) {
            const mv::UnitView v = ms.view(team);
            // the c of the block checks: from the squad container (TacticalBattle::attack_can_hit)
            tactical::Vec3 from = cs.can_hit_origin(members.front());
            from.x = v.pos.x;
            from.y = v.pos.y;
            const tc::CanHit c = cs.can_hit_from(members.front(), from, target);
            if (min_range && !out.replanned && v.has_goal) {
                out.replanned = true;
                out.replan_frames = b->frame() - stage_frame;
                out.goal_dist = dist_to(v.goal);
            }
            if (c == tc::CanHit::Yes && (!min_range || out.replanned)) {
                out.reached = true;
                out.reach_frames = b->frame() - stage_frame;
                out.end_dist = dist_to(mv::V2{v.pos.x, v.pos.y});
                stage = 9;
            } else if (b->frame() > stage_frame + 1500) {
                out.end_dist = dist_to(mv::V2{v.pos.x, v.pos.y});
                stage = 9;
            }
        }
    }
    return out;
}

// One squad data folder with S10_Trooper edited by `patch` (its own folder: g_temp_prefix + `name`).
inline std::unique_ptr<Data> squad_data_patched(const char* name, std::string (*patch)(std::string)) {
    static std::string prefix;
    const char* const old = g_temp_prefix;
    prefix = std::string(old) + name;
    g_temp_prefix = prefix.c_str();
    g_types_patch = patch;
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, true);
    g_types_patch = nullptr;
    g_temp_prefix = old;
    return d;
}

inline std::string trooper_melee(std::string t) {  // Targeting_Max_Attack_Distance 250 -> 20
    const std::string at = "<GroundInfantry Name=\"S10_Trooper\">", r = "<Targeting_Max_Attack_Distance>250</Targeting_Max_Attack_Distance>";
    if (const size_t p = t.find(at); p != std::string::npos)
        if (const size_t q = t.find(r, p); q != std::string::npos) t.replace(q, r.size(), "<Targeting_Max_Attack_Distance>20</Targeting_Max_Attack_Distance>");
    return t;
}

inline std::string trooper_min_range(std::string t) {  // Targeting_Min_Attack_Distance 100
    const std::string at = "<GroundInfantry Name=\"S10_Trooper\">";
    if (const size_t p = t.find(at); p != std::string::npos)
        t.insert(p + at.size(), "<Targeting_Min_Attack_Distance>100</Targeting_Min_Attack_Distance>");
    return t;
}

inline void test_land_attack_reach() {
    {
        auto d = squad_data_patched("_melee", trooper_melee);
        const LandReachRun r = run_land_reach(*d, false);
        std::printf("  land attack reach (melee): range %.0f, ordered %d, can hit after %u frame(s) %d, %.1f from the target\n",
                    static_cast<double>(r.range), r.ordered ? 1 : 0, r.reach_frames, r.reached ? 1 : 0, static_cast<double>(r.end_dist));
        CHECK(r.range == 20.0f && r.ordered && r.reached && r.reach_frames <= 600);
    }
    {
        auto d = squad_data_patched("_min_range", trooper_min_range);
        const LandReachRun r = run_land_reach(*d, true);
        std::printf("  land attack reach (minimum range 100): ordered %d, inside %d, re-planned after %u frame(s) to %.1f from the "
                    "target (staged 130), can hit after %u frame(s) %d, %.1f from the target\n",
                    r.ordered ? 1 : 0, r.inside ? 1 : 0, r.replan_frames, static_cast<double>(r.goal_dist), r.reach_frames,
                    r.reached ? 1 : 0, static_cast<double>(r.end_dist));
        CHECK(r.ordered && r.inside && r.replanned && r.replan_frames <= 31);
        CHECK(std::fabs(r.goal_dist - 130.0f) <= 10.0f);
        CHECK(r.reached && r.end_dist > 100.0f);
    }
}

// s78n t20 batch 1 item 1.5 (MOVEMENT §5.4.5 item 2): the land death clone of a type with IsDeathCloneObstacle and a
// UnitCollisionClass is a movement object without a locomotor at the dying unit's place, and leaves the movement system
// with its object at the planned removal frame. S10_Trooper dies into S10_Wreck (Vehicle class, 2 s + 0.5 s fade).
inline std::string trooper_wreck(std::string t) {
    const std::string at = "<GroundInfantry Name=\"S10_Trooper\">";
    if (const size_t p = t.find(at); p != std::string::npos)
        t.insert(p + at.size(), "<Death_Clone>Damage_Normal, S10_Wreck</Death_Clone>");
    if (const size_t end = t.find("</S10>"); end != std::string::npos)
        t.insert(end, " <GroundVehicle Name=\"S10_Wreck\"><LandBehavior>HIDE_WHEN_FOGGED</LandBehavior>"
                      "<UnitCollisionClass>Vehicle</UnitCollisionClass><IsDeathCloneObstacle>Yes</IsDeathCloneObstacle>"
                      "<Death_Persistence_Duration>2</Death_Persistence_Duration><Death_Fade_Time>0.5</Death_Fade_Time>"
                      "<Custom_Hard_XExtent>8</Custom_Hard_XExtent><Custom_Hard_YExtent>5</Custom_Hard_YExtent></GroundVehicle>\n");
    return t;
}

inline void test_land_death_clone_obstacle() {
    auto d = squad_data_patched("_wreck", trooper_wreck);
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.skip_human_units = true;
    opt.battle.order_interval = 0;
    ClashSession s(*d, 0, opt, {}, {}, true);
    CHECK(s.gs != nullptr);
    if (!s.gs) return;
    int stage = 0;
    objects::ObjectId clone = objects::kNoObjectId;
    mv::V2 at{};
    uint32_t kill_frame = 0, gone_frame = 0;
    bool registered = false, still = true, idle = true;
    size_t pending = 0;
    for (uint64_t t = 0; t < 3000 && stage < 9; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        const tc::CombatSystem& cs = b->combat();
        const mv::MovementSystem& ms = b->movement();
        if (stage == 0) {
            objects::ObjectId victim = objects::kNoObjectId;
            for (size_t i = 0; i < cs.unit_count() && victim == objects::kNoObjectId; ++i) {
                const auto id = cs.unit_id_at(i);
                if (cs.team(id) != 0 && cs.alive(id) && st.owner(st.find(id)) == 0) victim = id;
            }
            CHECK(victim != objects::kNoObjectId);
            if (victim == objects::kNoObjectId) break;
            const std::vector<mv::ObjectId> before = ms.unit_ids();
            const Vec3 vp = st.position(st.find(victim));
            pending = b->death_clones_pending();
            CHECK(b->script_damage(*tm, victim, 1.0e9f, false));
            for (const mv::ObjectId id : ms.unit_ids())
                if (std::find(before.begin(), before.end(), id) == before.end()) clone = id;
            registered = clone != objects::kNoObjectId && b->death_clones_pending() == pending + 1;
            at = mv::V2{vp.x, vp.y};
            kill_frame = b->frame();
            stage = 1;
            continue;
        }
        if (stage == 1) {
            if (ms.has_unit(clone)) {
                const mv::UnitView v = ms.view(clone);
                still = still && v.pos == at && v.speed == 0.0f;
                idle = idle && !v.has_goal && !v.pushed;
                for (const auto& c : ms.collisions().list()) idle = idle && c.a != clone;  // never a checker
            } else {
                gone_frame = b->frame();
                CHECK(b->death_clones_pending() == pending && !st.alive(st.find(clone)));
                stage = 9;
            }
        }
    }
    std::printf("  land death clone obstacle: registered %d at the death spot %d, idle %d, removed after %u frame(s)\n",
                registered ? 1 : 0, still ? 1 : 0, idle ? 1 : 0, gone_frame - kill_frame);
    CHECK(registered && still && idle && gone_frame > kill_frame + 60 && gone_frame <= kill_frame + 80);
}

inline void test_land_attack_blocks() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, true);
    const LandAttackBlockRun a = run_land_attack_block(*d, false, true);
    std::printf("  land attack blocks (i): range %.0f, ordered %d, no path in range %d, teleported %d, goal after %u frame(s), "
                "fired after %u; (iii) %zu block(s) at the save, loaded %d, %u ticks equal %d\n",
                static_cast<double>(a.range), a.ordered ? 1 : 0, a.gated ? 1 : 0, a.teleported ? 1 : 0, a.regoal_frames,
                a.fire_frames, a.blocks_at_save, a.loaded ? 1 : 0, a.compared, a.same ? 1 : 0);
    CHECK(a.ordered && a.gated && a.teleported);
    CHECK(a.regoal && a.regoal_frames <= 31);
    CHECK(a.fired && a.fire_frames <= 300);
    CHECK(a.saved && a.blocks_at_save >= 1 && a.loaded && a.same && a.compared == 400);
    std::printf("  land attack blocks: fed collision info checked on %u unit-frames, %u off, same after the load %d\n",
                a.info_frames, a.info_bad, a.info_same ? 1 : 0);
    CHECK(a.info_frames > 100 && a.info_bad == 0 && a.info_same);
    std::vector<uint64_t> w0, w4;
    const LandAttackBlockRun c = run_land_attack_block(*d, true, false, false, 0, &w0);
    std::printf("  land attack blocks (ii): friendly squads at 0.76 / 0.92 x range: ordered %d, walking %d, ended in range %d "
                "after %u frame(s), fired %d (%u frame(s) after the end)\n",
                c.ordered ? 1 : 0, c.gated ? 1 : 0, c.ended_in_range ? 1 : 0, c.end_frame, c.fired ? 1 : 0, c.fire_frames);
    CHECK(c.ordered && c.gated && c.ended_in_range && c.fired && c.fire_frames <= 300);
    // s78n t20 review: the same run (its blocks, re-plans and land stops) on 4 job workers - the same digest every tick
    (void)run_land_attack_block(*d, true, false, false, 4, &w4);
    size_t same = 0;
    while (same < w0.size() && same < w4.size() && w0[same] == w4[same]) ++same;
    std::printf("  land attack blocks (ii) serial / 4 workers: %zu / %zu tick(s), the first %zu equal\n", w0.size(), w4.size(), same);
    CHECK(!w0.empty() && w0 == w4);
    // s78n t20 step 5: the same through the AI host's Attack (BattleAi::execute): it ends in weapon range
    const LandAttackBlockRun e = run_land_attack_block(*d, true, false, true);
    std::printf("  land attack blocks (AI Attack): ordered %d, walking %d, ended in range %d after %u frame(s), fired %d (%u frame(s) "
                "after the end), fed info %u unit-frames, %u off\n",
                e.ordered ? 1 : 0, e.gated ? 1 : 0, e.ended_in_range ? 1 : 0, e.end_frame, e.fired ? 1 : 0, e.fire_frames, e.info_frames,
                e.info_bad);
    CHECK(e.ordered && e.gated && e.ended_in_range && e.fired && e.fire_frames <= 300 && e.info_bad == 0);
}

// s78n t25.3 (MOVEMENT §4.3.2 items 2 - 8, FIGHTER_FLYING §4.11 A / E): every land attack order goes through the attack
// block (TacticalBattle::land_attack_order). Two human squads 300 apart across the approach, 600 from the target (beyond
// the recruitment distance: two formations; not solo: 600 > foot + engage + Solo_Attack_Range), given ONE Attack:
//   (iv) one movement attack order (MovementStats::attack_orders + 1), both containers slotted (atk_rho > 0) at different
//        angles, both end in weapon range (can_hit Yes, no goal) and fire within 300 frames of that, apart from each other;
//   (v)  the target's squad is then teleported 200 farther: the next 30-frame re-check re-plans (a goal again,
//        attack_replans + 1) and the slot is bit-identical (theta, rho, the attack formation);
//   (vii) a Stop clears the first squad's slot, a plain move the second's.
// (vi, a structure target: the test data has no stamped structure; covered by alamo_tactical_movement_b5_tests, the
// stamped-cell scene: rho 0, the goal outside the footprint.)
struct LandAttackTwoRun {
    bool ordered = false, one_order = false, slotted = false, distinct = false, apart = false;
    bool ended[2] = {false, false}, fired[2] = {false, false};
    uint32_t end_frame[2] = {0, 0}, fire_frames[2] = {0, 0};
    float theta[2] = {0.0f, 0.0f}, rho[2] = {0.0f, 0.0f};
    bool teleported = false, replanned = false, same_slot = false;
    uint32_t replan_frames = 0;
    bool stop_cleared = false, move_cleared = false;
};

inline LandAttackTwoRun run_land_attack_two(const Data& d) {
    LandAttackTwoRun out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 8000;
    opt.battle.skip_human_units = true;
    opt.battle.order_interval = 0;  // no stand-in orders
    ClashSession s(d, 0, opt, {"S10_Squad", "S10_Squad"}, {}, true);
    if (!s.gs) return out;
    int stage = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId sq[2] = {objects::kNoObjectId, objects::kNoObjectId}, spare = objects::kNoObjectId;
    std::vector<objects::ObjectId> mem[2];
    objects::ObjectId target = objects::kNoObjectId;
    Vec3 dir{};
    mv::UnitView slot0;
    uint32_t replans0 = 0;
    for (uint64_t t = 0; t < 9000 && stage < 9; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        auto& cs = const_cast<tc::CombatSystem&>(b->combat());
        const mv::MovementSystem& ms = b->movement();
        if (stage == 0) {
            // s78n t26 review round 3: the test data has no reveal source, so the target would be hidden (FIGHTER_FLYING §4.11 C / E: no
            // land stop, no re-path while moving); this scenario is about a visible target (fog: test_land_attack_unseen)
            if (b->fog()) b->fog()->reveal_all(0);
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                cs.make_invulnerable(id, 1000000, b->frame());
                if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
            }
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() < 2 || !st.alive(th) || !ms.has_unit(static_cast<objects::ObjectId>(tid))) continue;
                if (st.owner(th) == 0) {
                    if (sq[0] == objects::kNoObjectId) {
                        sq[0] = static_cast<objects::ObjectId>(tid);
                        mem[0] = m;
                    } else if (sq[1] == objects::kNoObjectId) {
                        sq[1] = static_cast<objects::ObjectId>(tid);
                        mem[1] = m;
                    } else if (spare == objects::kNoObjectId) {
                        spare = static_cast<objects::ObjectId>(tid);
                    }
                } else if (st.owner(th) == 1 && target == objects::kNoObjectId) {
                    target = m.front();
                }
            }
            CHECK(sq[1] != objects::kNoObjectId && target != objects::kNoObjectId);
            if (sq[1] == objects::kNoObjectId || target == objects::kNoObjectId) break;
            const Vec3 tp = st.position(st.find(target)), ap = st.position(st.find(sq[0]));
            const float dx = ap.x - tp.x, dy = ap.y - tp.y, dl = std::sqrt(dx * dx + dy * dy);
            dir = dl > 1.0f ? Vec3{dx / dl, dy / dl, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 perp{-dir.y, dir.x, 0.0f};
            for (int k = 0; k < 2; ++k) {
                const float side = k == 0 ? 150.0f : -150.0f;
                const Vec3 p{tp.x + dir.x * 600.0f + perp.x * side, tp.y + dir.y * 600.0f + perp.y * side, ap.z};
                CHECK(b->teleport(*tm, sq[k], p, st.facing(st.find(sq[k]))));
            }
            if (spare != objects::kNoObjectId)  // the third squad out of the way, behind them
                CHECK(b->teleport(*tm, spare, Vec3{tp.x + dir.x * 1000.0f, tp.y + dir.y * 1000.0f, ap.z}, st.facing(st.find(spare))));
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        if (!cs.alive(target)) break;
        bool fired[2] = {false, false};
        for (const tc::ShotEvent& ev : cs.shot_events())
            for (int k = 0; k < 2; ++k)
                for (const auto id : mem[k])
                    fired[k] = fired[k] || (ev.shooter == id && (ev.target == target || cs.team(ev.target) == cs.team(target)));
        if (stage == 1) {
            if (b->frame() < stage_frame + 2) continue;
            std::vector<objects::ObjectId> all = mem[0];
            all.insert(all.end(), mem[1].begin(), mem[1].end());
            const uint32_t orders0 = ms.stats().attack_orders;
            out.ordered = b->order_attack(*tm, 0, all, target, s.host->random().synced()) == commands::RejectReason::None;
            CHECK(out.ordered);
            out.one_order = ms.stats().attack_orders == orders0 + 1;
            const mv::UnitView v0 = ms.view(sq[0]), v1 = ms.view(sq[1]);
            out.theta[0] = v0.atk_theta;
            out.theta[1] = v1.atk_theta;
            out.rho[0] = v0.atk_rho;
            out.rho[1] = v1.atk_rho;
            out.slotted = v0.atk_rho > 0.0f && v1.atk_rho > 0.0f && v0.atk_form != 0 && v1.atk_form != 0 && v0.atk_form != v1.atk_form;
            out.distinct = v0.atk_theta != v1.atk_theta;
            stage = 2;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 2) {
            for (int k = 0; k < 2; ++k) {
                if (!out.ended[k]) {
                    if (fired[k]) out.fired[k] = true;  // a member may fire on the way (in range while walking)
                    if (cs.can_hit(mem[k].front(), target) == tc::CanHit::Yes && !ms.view(sq[k]).has_goal) {
                        out.ended[k] = true;
                        out.end_frame[k] = b->frame();
                    }
                } else if (!out.fired[k] && fired[k]) {
                    out.fired[k] = true;
                    out.fire_frames[k] = b->frame() - out.end_frame[k];
                }
            }
            const bool done = out.ended[0] && out.ended[1] && out.fired[0] && out.fired[1];
            const bool late = (out.ended[0] && !out.fired[0] && b->frame() > out.end_frame[0] + 300) ||
                              (out.ended[1] && !out.fired[1] && b->frame() > out.end_frame[1] + 300);
            if (done || late || b->frame() > stage_frame + 3000) {
                for (int k = 0; k < 2; ++k)
                    if (out.ended[k]) out.end_frame[k] -= stage_frame;
                const Vec3 a = st.position(st.find(sq[0])), c = st.position(st.find(sq[1]));
                const float sep = std::sqrt((a.x - c.x) * (a.x - c.x) + (a.y - c.y) * (a.y - c.y));
                out.apart = sep > ms.collision_radius(sq[0]) + ms.collision_radius(sq[1]);
                if (!done) {
                    stage = 9;
                    break;
                }
                // (v): the target's squad 200 farther from the attackers
                slot0 = ms.view(sq[0]);
                replans0 = ms.stats().attack_replans;
                const auto tc_id = static_cast<objects::ObjectId>(cs.team(target));
                const objects::ObjectHandle th = st.find(tc_id);
                const Vec3 tp = st.position(th);
                out.teleported = b->teleport(*tm, tc_id, Vec3{tp.x - dir.x * 200.0f, tp.y - dir.y * 200.0f, tp.z}, st.facing(th));
                CHECK(out.teleported);
                stage = 3;
                stage_frame = b->frame();
            }
            continue;
        }
        if (stage == 3) {
            const mv::UnitView v = ms.view(sq[0]);
            if (v.has_goal && ms.stats().attack_replans > replans0) {
                out.replanned = true;
                out.replan_frames = b->frame() - stage_frame;
                out.same_slot = v.atk_theta == slot0.atk_theta && v.atk_rho == slot0.atk_rho && v.atk_form == slot0.atk_form &&
                                v.atk_target == slot0.atk_target && v.atk_rho > 0.0f;
                stage = 4;
            } else if (b->frame() > stage_frame + 40) {
                stage = 4;
            }
            continue;
        }
        if (stage == 4) {  // (vii)
            CHECK(b->order_stop(*tm, 0, mem[0]) == commands::RejectReason::None);
            const mv::UnitView v0 = ms.view(sq[0]);
            out.stop_cleared = v0.atk_rho == 0.0f && v0.atk_theta == 0.0f && v0.atk_form == 0 && v0.atk_target == -1;
            const Vec3 p = st.position(st.find(sq[1]));
            CHECK(b->order_move(*tm, 0, mem[1], p.x + dir.x * 100.0f, p.y + dir.y * 100.0f, s.host->random().synced()) ==
                  commands::RejectReason::None);
            const mv::UnitView v1 = ms.view(sq[1]);
            out.move_cleared = v1.atk_rho == 0.0f && v1.atk_theta == 0.0f && v1.atk_form == 0 && v1.atk_target == -1;
            stage = 9;
        }
    }
    return out;
}

inline void test_land_attack_order() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, true);
    const LandAttackTwoRun r = run_land_attack_two(*d);
    std::printf("  land attack order (iv): ordered %d, one movement order %d, slotted %d (theta %.7f / %.7f, rho %.3f / %.3f), "
                "distinct %d; ended in range %d / %d after %u / %u frame(s), fired %d / %d (%u / %u frame(s) after the end), "
                "apart %d\n",
                r.ordered ? 1 : 0, r.one_order ? 1 : 0, r.slotted ? 1 : 0, static_cast<double>(r.theta[0]), static_cast<double>(r.theta[1]),
                static_cast<double>(r.rho[0]), static_cast<double>(r.rho[1]), r.distinct ? 1 : 0, r.ended[0] ? 1 : 0, r.ended[1] ? 1 : 0,
                r.end_frame[0], r.end_frame[1], r.fired[0] ? 1 : 0, r.fired[1] ? 1 : 0, r.fire_frames[0], r.fire_frames[1],
                r.apart ? 1 : 0);
    CHECK(r.ordered && r.one_order && r.slotted && r.distinct);
    CHECK(r.ended[0] && r.ended[1] && r.fired[0] && r.fired[1] && r.fire_frames[0] <= 300 && r.fire_frames[1] <= 300 && r.apart);
    std::printf("  land attack order (v): target teleported 200 %d, re-planned %d after %u frame(s), slot bit-identical %d; "
                "(vii) Stop clears the slot %d, a plain move clears it %d\n",
                r.teleported ? 1 : 0, r.replanned ? 1 : 0, r.replan_frames, r.same_slot ? 1 : 0, r.stop_cleared ? 1 : 0,
                r.move_cleared ? 1 : 0);
    CHECK(r.teleported && r.replanned && r.replan_frames <= 31 && r.same_slot);
    CHECK(r.stop_cleared && r.move_cleared);
}

// s78n t25.4 (MOVEMENT §4.3.2 items 1, 2, 7 and 8; A1_READINGS §33.4): a Lua Guard_Target on a friendly squad (the
// tactical AI's PortExtension Guard on an object) is one movement order with the guard block. The guard squad stands
// 400 from the guarded one (> min(range, Land_Guard_Range 150): slotted); its goal is within 150 of the guarded squad
// (the guard trim). The guarded squad is then moved 300 sideways (> R = half the lead's range): the follow record's
// 30-frame service re-plans the guard (attack_replans + 1) with the slot bit-identical (theta, rho, atk_form, target)
// and a goal within 150 of the new position; the guard ends there.
struct LandGuardRun {
    bool ordered = false, slotted = false, near_goal = false, arrived = false;
    bool moved = false, replanned = false, same_slot = false, near_goal2 = false, arrived2 = false;
    float rho = 0.0f, theta = 0.0f, goal_d = 0.0f, goal_d2 = 0.0f, end_d = 0.0f;
    uint32_t replan_frames = 0;
    bool own_attack_move = false, block_kept = false;  // bkt1 t05
    size_t blocks0 = 0;
};

inline LandGuardRun run_land_guard(const Data& d) {
    LandGuardRun out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 8000;
    opt.battle.skip_human_units = true;
    opt.battle.order_interval = 0;  // no stand-in orders
    opt.battle.fog = false;         // the trim gate (item 7: T not fogged) open; the test data has no reveal source
    ClashSession s(d, 0, opt, {"S10_Squad", "S10_Squad"}, {}, true);
    if (!s.gs) return out;
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d.files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    std::unique_ptr<BattleAi> bai;
    int stage = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId sq[2] = {objects::kNoObjectId, objects::kNoObjectId};
    Vec3 dir{}, perp{};
    mv::UnitView slot0;
    uint32_t replans0 = 0;
    for (uint64_t t = 0; t < 6000 && stage < 9; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        auto& cs = const_cast<tc::CombatSystem&>(b->combat());
        const mv::MovementSystem& ms = b->movement();
        if (bai) bai->follow_service();  // the BattleAi::update step (this AI runs no plans)
        const auto gd = [&](mv::V2 p) {
            const Vec3 gp = st.position(st.find(sq[1]));
            return std::sqrt((p.x - gp.x) * (p.x - gp.x) + (p.y - gp.y) * (p.y - gp.y));
        };
        if (stage == 0) {
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                cs.make_invulnerable(id, 1000000, b->frame());
                if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
            }
            objects::ObjectId enemy = objects::kNoObjectId;
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() < 2 || !st.alive(th) || !ms.has_unit(static_cast<objects::ObjectId>(tid))) continue;
                if (st.owner(th) == 0) {
                    if (sq[0] == objects::kNoObjectId) sq[0] = static_cast<objects::ObjectId>(tid);
                    else if (sq[1] == objects::kNoObjectId) sq[1] = static_cast<objects::ObjectId>(tid);
                } else if (st.owner(th) == 1 && enemy == objects::kNoObjectId) {
                    enemy = m.front();
                }
            }
            CHECK(sq[1] != objects::kNoObjectId && enemy != objects::kNoObjectId);
            if (sq[1] == objects::kNoObjectId || enemy == objects::kNoObjectId) break;
            // the run_land_attack_two spots (open ground, one zone, 600 from the enemy on the attackers' side): the guard at
            // +150 across, the guarded squad at -250 across (400 apart)
            const Vec3 tp = st.position(st.find(enemy)), ap = st.position(st.find(sq[0]));
            const float dx = ap.x - tp.x, dy = ap.y - tp.y, dl = std::sqrt(dx * dx + dy * dy);
            dir = dl > 1.0f ? Vec3{dx / dl, dy / dl, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            perp = Vec3{-dir.y, dir.x, 0.0f};
            const auto spot = [&](float along, float across) {
                return Vec3{tp.x + dir.x * along + perp.x * across, tp.y + dir.y * along + perp.y * across, ap.z};
            };
            CHECK(b->teleport(*tm, sq[1], spot(600.0f, -250.0f), st.facing(st.find(sq[1]))));
            CHECK(b->teleport(*tm, sq[0], spot(600.0f, 150.0f), st.facing(st.find(sq[0]))));
            bai = std::make_unique<BattleAi>(*b, *tm, *s.host, ts);
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 1) {
            if (b->frame() < stage_frame + 2) continue;
            const Vec3 gp = st.position(st.find(sq[1]));
            std::vector<uint8_t> pay;
            const auto put = [&pay](uint32_t v) {
                for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
            };
            put(static_cast<uint32_t>(sq[0]));
            put(static_cast<uint32_t>(sq[1]));
            put(std::bit_cast<uint32_t>(gp.x));
            put(std::bit_cast<uint32_t>(gp.y));
            put(std::bit_cast<uint32_t>(gp.z));
            put(0u);
            const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::Guard));
            out.ordered = bai->execute(0, commands::ev::port_extension(sub, pay)) == commands::RejectReason::None;
            CHECK(out.ordered);
            const mv::UnitView v = ms.view(sq[0]);
            out.rho = v.atk_rho;
            out.theta = v.atk_theta;
            out.slotted = v.atk_rho > 0.0f && v.atk_form != 0 && v.atk_target == sq[1] && v.has_goal;
            out.goal_d = gd(v.goal);
            out.near_goal = out.goal_d > 140.0f && out.goal_d <= 150.0f + 0.5f;  // the trim: the 10-step inside the guard range
            slot0 = v;
            stage = 2;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 2) {
            const mv::UnitView v = ms.view(sq[0]);
            if (!v.has_goal && v.speed == 0.0f) {
                out.arrived = true;
                // 300 toward the enemy (to 300 from it, beside the attack test's slot ring)
                const objects::ObjectHandle gh = st.find(sq[1]);
                const Vec3 gp = st.position(gh);
                out.moved = b->teleport(*tm, sq[1], Vec3{gp.x - dir.x * 300.0f, gp.y - dir.y * 300.0f, gp.z}, st.facing(gh));
                CHECK(out.moved);
                replans0 = ms.stats().attack_replans;
                stage = 3;
                stage_frame = b->frame();
            } else if (b->frame() > stage_frame + 1500) {
                stage = 9;
            }
            continue;
        }
        if (stage == 3) {
            const mv::UnitView v = ms.view(sq[0]);
            if (v.has_goal && ms.stats().attack_replans > replans0) {
                out.replanned = true;
                out.replan_frames = b->frame() - stage_frame;
                out.same_slot = v.atk_theta == slot0.atk_theta && v.atk_rho == slot0.atk_rho && v.atk_form == slot0.atk_form &&
                                v.atk_target == slot0.atk_target;
                out.goal_d2 = gd(v.goal);
                out.near_goal2 = out.goal_d2 > 140.0f && out.goal_d2 <= 150.0f + 0.5f;  // the trim: the 10-step inside the guard range
                stage = 4;
                stage_frame = b->frame();
            } else if (b->frame() > stage_frame + 40) {
                stage = 9;
            }
            continue;
        }
        if (stage == 4) {
            const mv::UnitView v = ms.view(sq[0]);
            if (!v.has_goal && v.speed == 0.0f) {
                out.arrived2 = true;
                out.end_d = gd(v.pos);
                stage = 5;
            } else if (b->frame() > stage_frame + 1500) {
                stage = 9;
            }
            continue;
        }
        if (stage == 5) {
            // bkt1 t05 (b0075, BKT1_ATTACK_RE §3 [H]): the guard squad's own Attack_Move (arg 0) on the friendly squad, moved
            // 600 away first: the land attack move on the object - an attack block (atk_target = the squad), no lock
            const objects::ObjectHandle gh = st.find(sq[1]);
            const Vec3 gp0 = st.position(gh);
            CHECK(b->teleport(*tm, sq[1], Vec3{gp0.x + dir.x * 600.0f, gp0.y + dir.y * 600.0f, gp0.z}, st.facing(gh)));
            const Vec3 gp = st.position(gh);
            std::vector<uint8_t> pay;
            const auto put = [&pay](uint32_t v) {
                for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
            };
            objects::ObjectId tgt = objects::kNoObjectId;  // a trooper of the friendly squad (a combat unit)
            for (const auto& [mid, slot] : ms.land_team_slots(sq[1])) {
                (void)slot;
                if (tgt == objects::kNoObjectId) tgt = mid;
            }
            put(static_cast<uint32_t>(sq[0]));
            put(static_cast<uint32_t>(tgt));
            put(std::bit_cast<uint32_t>(gp.x));
            put(std::bit_cast<uint32_t>(gp.y));
            put(std::bit_cast<uint32_t>(gp.z));
            put(0u);
            const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::AttackMove));
            const bool ok = bai->execute(0, commands::ev::port_extension(sub, pay)) == commands::RejectReason::None;
            const mv::UnitView v = ms.view(sq[0]);
            bool no_lock = true;
            for (const auto& [mid, slot] : ms.land_team_slots(sq[0])) {
                (void)slot;
                no_lock = no_lock && cs.attack_goal(mid) == tc::kNoObject;
            }
            std::printf("  own Attack_Move: ok %d goal %d atk_target %d (trooper %d) no lock %d goal %.1f from it\n", ok ? 1 : 0,
                        v.has_goal ? 1 : 0, static_cast<int>(v.atk_target), static_cast<int>(tgt), no_lock ? 1 : 0,
                        static_cast<double>(gd(v.goal)));
            out.own_attack_move = ok && v.has_goal && v.atk_target == tgt && no_lock && gd(v.goal) < 300.0f;
            out.blocks0 = b->attack_blocks();
            stage = 6;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 6 && b->frame() >= stage_frame + 45) {  // through the block's first 30-frame check: kept (no lock)
            out.block_kept = out.blocks0 > 0 && b->attack_blocks() == out.blocks0;
            stage = 9;
        }
    }
    return out;
}

inline void test_land_guard() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, true);
    const LandGuardRun r = run_land_guard(*d);
    std::printf("  land guard: ordered %d, slotted %d (theta %.7f, rho %.3f), goal %.2f from the guarded squad, arrived %d; "
                "moved 300 %d, re-planned %d after %u frame(s), slot bit-identical %d, goal %.2f from it, arrived %d at %.2f\n",
                r.ordered ? 1 : 0, r.slotted ? 1 : 0, static_cast<double>(r.theta), static_cast<double>(r.rho),
                static_cast<double>(r.goal_d), r.arrived ? 1 : 0, r.moved ? 1 : 0, r.replanned ? 1 : 0, r.replan_frames,
                r.same_slot ? 1 : 0, static_cast<double>(r.goal_d2), r.arrived2 ? 1 : 0, static_cast<double>(r.end_d));
    CHECK(r.ordered && r.slotted && r.near_goal && r.arrived);
    CHECK(r.moved && r.replanned && r.replan_frames <= 31 && r.same_slot && r.near_goal2 && r.arrived2);
    std::printf("  own Attack_Move on the friendly squad: attack block, no lock %d\n", r.own_attack_move ? 1 : 0);
    CHECK(r.own_attack_move && r.block_kept);  // bkt1 t05
}

// s78n t26 review round 2 (LUA_API "Move_To / Attack_Move / Guard_Target - Move_Internal 0x57da90" [H]: for Move_To,
// flags 0, a GameObjectWrapper destination is the object's position and the order gets kind 1; TACTICAL_COMBAT §2.8
// "Summary by order": unit.Move_To(object) = 1 (position snapshot); A1_READINGS §33.4: the 30-frame re-path follows
// kind-3 goals only): a Lua Move_To(friendly squad) - the MoveToObject event (no threat number) and the LuaOrder MoveTo
// with the object (a threat number) - moves to where the squad stood. The squad is then moved 300 away: the mover is
// never re-pathed after it (its goal stays at the snapshot) and ends at the snapshot point, far from the squad.
struct LandMoveToObjectRun {
    bool ordered = false, moved = false, kept_goal = true, arrived = false;
    float goal_d = 0.0f, end_snap = 0.0f, end_obj = 0.0f;
};

inline LandMoveToObjectRun run_land_move_to_object(const Data& d, bool lua_order) {
    LandMoveToObjectRun out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 8000;
    opt.battle.skip_human_units = true;
    opt.battle.order_interval = 0;  // no stand-in orders
    opt.battle.fog = false;
    ClashSession s(d, 0, opt, {"S10_Squad", "S10_Squad"}, {}, true);
    if (!s.gs) return out;
    ai::AiDefinitions defs;
    ai::EquationSet eqs;
    TacticalAiSetup ts;
    ts.defs = &defs;
    ts.equations = &eqs;
    ts.files = d.files.get();
    ts.ai_type = [](int32_t, const galactic::GalacticSim&) { return std::string(); };  // no AI player: orders only
    std::unique_ptr<BattleAi> bai;
    int stage = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId sq[2] = {objects::kNoObjectId, objects::kNoObjectId};
    Vec3 dir{}, perp{}, snap{};
    mv::V2 goal0;
    for (uint64_t t = 0; t < 6000 && stage < 9; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        auto& cs = const_cast<tc::CombatSystem&>(b->combat());
        const mv::MovementSystem& ms = b->movement();
        if (bai) bai->follow_service();  // the BattleAi::update step (this AI runs no plans)
        if (stage == 0) {
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                cs.make_invulnerable(id, 1000000, b->frame());
                if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
            }
            objects::ObjectId enemy = objects::kNoObjectId;
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() < 2 || !st.alive(th) || !ms.has_unit(static_cast<objects::ObjectId>(tid))) continue;
                if (st.owner(th) == 0) {
                    if (sq[0] == objects::kNoObjectId) sq[0] = static_cast<objects::ObjectId>(tid);
                    else if (sq[1] == objects::kNoObjectId) sq[1] = static_cast<objects::ObjectId>(tid);
                } else if (st.owner(th) == 1 && enemy == objects::kNoObjectId) {
                    enemy = m.front();
                }
            }
            CHECK(sq[1] != objects::kNoObjectId && enemy != objects::kNoObjectId);
            if (sq[1] == objects::kNoObjectId || enemy == objects::kNoObjectId) break;
            // the run_land_guard spots (open ground, 600 from the enemy): the mover at +150 across, the target at -250
            const Vec3 tp = st.position(st.find(enemy)), ap = st.position(st.find(sq[0]));
            const float dx = ap.x - tp.x, dy = ap.y - tp.y, dl = std::sqrt(dx * dx + dy * dy);
            dir = dl > 1.0f ? Vec3{dx / dl, dy / dl, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            perp = Vec3{-dir.y, dir.x, 0.0f};
            const auto spot = [&](float along, float across) {
                return Vec3{tp.x + dir.x * along + perp.x * across, tp.y + dir.y * along + perp.y * across, ap.z};
            };
            CHECK(b->teleport(*tm, sq[1], spot(600.0f, -250.0f), st.facing(st.find(sq[1]))));
            CHECK(b->teleport(*tm, sq[0], spot(600.0f, 150.0f), st.facing(st.find(sq[0]))));
            bai = std::make_unique<BattleAi>(*b, *tm, *s.host, ts);
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 1) {
            if (b->frame() < stage_frame + 2) continue;
            const objects::ObjectHandle gh = st.find(sq[1]);
            snap = st.position(gh);
            if (lua_order) {
                std::vector<uint8_t> pay;
                const auto put = [&pay](uint32_t v) {
                    for (int i = 0; i < 4; ++i) pay.push_back(static_cast<uint8_t>(v >> (8 * i)));
                };
                put(static_cast<uint32_t>(sq[0]));
                put(static_cast<uint32_t>(sq[1]));
                put(std::bit_cast<uint32_t>(snap.x));
                put(std::bit_cast<uint32_t>(snap.y));
                put(std::bit_cast<uint32_t>(snap.z));
                put(std::bit_cast<uint32_t>(1.0e9f));  // a threat number (Move_To(obj, n)): the LuaOrder path
                const auto sub = static_cast<uint16_t>(lua::kLuaOrderSubtype + static_cast<uint16_t>(lua::LuaOrder::MoveTo));
                out.ordered = bai->execute(0, commands::ev::port_extension(sub, pay)) == commands::RejectReason::None;
            } else {
                out.ordered = bai->execute(0, commands::ev::move_to_object(static_cast<uint32_t>(sq[1]), {static_cast<uint32_t>(sq[0])})) ==
                              commands::RejectReason::None;
            }
            CHECK(out.ordered);
            goal0 = ms.view(sq[0]).goal;
            out.goal_d = std::sqrt((goal0.x - snap.x) * (goal0.x - snap.x) + (goal0.y - snap.y) * (goal0.y - snap.y));
            // the target walks off at once: 300 toward the enemy
            out.moved = b->teleport(*tm, sq[1], Vec3{snap.x - dir.x * 300.0f, snap.y - dir.y * 300.0f, snap.z}, st.facing(gh));
            CHECK(out.moved);
            stage = 2;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 2) {
            const mv::UnitView v = ms.view(sq[0]);
            if (v.has_goal && !(v.goal == goal0) && !v.pushed) out.kept_goal = false;  // a re-path after the moved squad
            if (!v.has_goal && v.speed == 0.0f && b->frame() > stage_frame + 30) {
                out.arrived = true;
                const Vec3 op = st.position(st.find(sq[1]));
                out.end_snap = std::sqrt((v.pos.x - snap.x) * (v.pos.x - snap.x) + (v.pos.y - snap.y) * (v.pos.y - snap.y));
                out.end_obj = std::sqrt((v.pos.x - op.x) * (v.pos.x - op.x) + (v.pos.y - op.y) * (v.pos.y - op.y));
                stage = 9;
            } else if (b->frame() > stage_frame + 1500) {
                stage = 9;
            }
        }
    }
    return out;
}

inline void test_land_move_to_object() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, true);
    for (const bool lua_order : {false, true}) {
        const LandMoveToObjectRun r = run_land_move_to_object(*d, lua_order);
        std::printf("  land Move_To(object) (%s): ordered %d, goal %.2f from the snapshot, target moved 300 %d, goal kept %d, "
                    "arrived %d %.2f from the snapshot and %.2f from the target\n",
                    lua_order ? "LuaOrder MoveTo + threat" : "MoveToObject", r.ordered ? 1 : 0, static_cast<double>(r.goal_d),
                    r.moved ? 1 : 0, r.kept_goal ? 1 : 0, r.arrived ? 1 : 0, static_cast<double>(r.end_snap),
                    static_cast<double>(r.end_obj));
        CHECK(r.ordered && r.moved && r.goal_d < 60.0f && r.kept_goal);
        CHECK(r.arrived && r.end_snap < 60.0f && r.end_obj > 200.0f);
    }
}

// s78n t26 review round 2 (FIGHTER_FLYING §4.11 E [H]: c = 0x40edd0(F) is true only when EVERY member of the formation can
// hit from where it stands; the land stop cuts F's path and stops every member at once): three squads 60 apart across the
// approach, 800 from the target, given one Attack - one attack formation (the middle squad seeds at the centroid and
// recruits both). The third squad's troopers get +140 % fire range (600 instead of 250), so it can hit long before the
// others. While it can hit and the seed is still well out of its own range (> 280) with every squad walking, no 30-frame
// check may land-stop (the formation walks on together); the window is longer than one check interval.
// bkm2 t08 (b0048): the target is revealed for player 0 (else the hidden term stops nothing at all), and a frame counts
// when every squad was walking at its start (a land stop in the frame ends "walking" in that same frame, so the old
// end-of-frame test could not see the stop it is about). Run on the pre-fix per-mover check (the grouping by atk_form
// disabled) it fails: the long-range squad's own block land-stops it (1 stop, the window cut short).
struct LandAttackFormationStopRun {
    bool ordered = false, one_form = false, ended = false;
    uint32_t window = 0, stops_in_window = 0;
};

inline LandAttackFormationStopRun run_land_attack_formation_stop(const Data& d) {
    LandAttackFormationStopRun out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 8000;
    opt.battle.skip_human_units = true;
    opt.battle.order_interval = 0;  // no stand-in orders
    ClashSession s(d, 0, opt, {"S10_Squad", "S10_Squad", "S10_Squad"}, {}, true);
    if (!s.gs) return out;
    int stage = 0;
    uint32_t stage_frame = 0, stops0 = 0;
    bool was_walking = false;  // every squad walking at the start of the frame
    objects::ObjectId sq[3] = {objects::kNoObjectId, objects::kNoObjectId, objects::kNoObjectId};
    std::vector<objects::ObjectId> mem[3];
    objects::ObjectId target = objects::kNoObjectId;
    for (uint64_t t = 0; t < 6000 && stage < 9; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        auto& cs = const_cast<tc::CombatSystem&>(b->combat());
        const mv::MovementSystem& ms = b->movement();
        if (stage == 0) {
            // bkm2 t08 (b0048): the test data has no reveal source, so without this the target is hidden for player 0 and
            // the hidden term (FIGHTER_FLYING §4.11 C 1 / C 4, s78n t26 review round 3) suppresses every land stop - the
            // check then passed on the per-mover stop too. The scenario is about a visible target.
            if (b->fog()) b->fog()->reveal_all(0);
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                cs.make_invulnerable(id, 1000000, b->frame());
                if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
            }
            int n = 0;
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() < 2 || !st.alive(th) || !ms.has_unit(static_cast<objects::ObjectId>(tid))) continue;
                if (st.owner(th) == 0 && n < 3) {
                    sq[n] = static_cast<objects::ObjectId>(tid);
                    mem[n] = m;
                    ++n;
                } else if (st.owner(th) == 1 && target == objects::kNoObjectId) {
                    target = m.front();
                }
            }
            CHECK(n == 3 && target != objects::kNoObjectId);
            if (n < 3 || target == objects::kNoObjectId) break;
            const Vec3 tp = st.position(st.find(target)), ap = st.position(st.find(sq[0]));
            const float dx = ap.x - tp.x, dy = ap.y - tp.y, dl = std::sqrt(dx * dx + dy * dy);
            const Vec3 dir = dl > 1.0f ? Vec3{dx / dl, dy / dl, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            const Vec3 perp{-dir.y, dir.x, 0.0f};
            const float across[3] = {0.0f, 60.0f, -60.0f};
            for (int k = 0; k < 3; ++k) {
                const Vec3 p{tp.x + dir.x * 800.0f + perp.x * across[k], tp.y + dir.y * 800.0f + perp.y * across[k], ap.z};
                CHECK(b->teleport(*tm, sq[k], p, st.facing(st.find(sq[k]))));
            }
            for (const objects::ObjectId id : mem[2])  // the long-range squad: Fire_Range bonus +140 % (250 -> 600)
                if (tc::ModifierSet* m = cs.unit_modifiers(id))
                    m->set_by_source(tc::ModList::FireRange, 0x54325232, 1.4f, tc::kPermanent, 0x54325232u, -0x5432);
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 1) {
            if (b->frame() < stage_frame + 2) continue;
            std::vector<objects::ObjectId> all;
            for (const auto& m : mem) all.insert(all.end(), m.begin(), m.end());
            out.ordered = b->order_attack(*tm, 0, all, target, s.host->random().synced()) == commands::RejectReason::None;
            CHECK(out.ordered);
            const uint32_t f0 = ms.view(sq[0]).atk_form;
            out.one_form = f0 != 0 && ms.view(sq[1]).atk_form == f0 && ms.view(sq[2]).atk_form == f0;
            stops0 = b->stats().attack_land_stops;
            stage = 2;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 2) {
            const uint32_t stops = b->stats().attack_land_stops;
            const Vec3 tp = st.position(st.find(target));
            const mv::UnitView v0 = ms.view(sq[0]);
            const float d0 = std::sqrt((v0.pos.x - tp.x) * (v0.pos.x - tp.x) + (v0.pos.y - tp.y) * (v0.pos.y - tp.y));
            const bool far_hit = cs.can_hit(mem[2].front(), target) == tc::CanHit::Yes;
            const bool walking = ms.view(sq[0]).has_goal && ms.view(sq[1]).has_goal && ms.view(sq[2]).has_goal;
            if (far_hit && was_walking && d0 > 280.0f) {
                ++out.window;
                out.stops_in_window += stops - stops0;
            }
            stops0 = stops;
            was_walking = walking;
            if (!walking && d0 <= 280.0f) {
                out.ended = true;
                stage = 9;
            } else if (b->frame() > stage_frame + 3000) {
                stage = 9;
            }
        }
    }
    return out;
}

inline void test_land_attack_formation_stop() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, true);
    const LandAttackFormationStopRun r = run_land_attack_formation_stop(*d);
    std::printf("  land attack formation stop: ordered %d, one attack formation %d, %u frame(s) with only the long-range squad "
                "able to hit (all walking), land stops in them %u, ended %d\n",
                r.ordered ? 1 : 0, r.one_form ? 1 : 0, r.window, r.stops_in_window, r.ended ? 1 : 0);
    CHECK(r.ordered && r.one_form && r.window > 31 && r.stops_in_window == 0);
}

// s78n t26 review round 3 (FIGHTER_FLYING §4.11 C 1 / C 3 / C 4 / E [H]): the land attack block's fog and stealth terms.
//   hidden: one squad (the fog revealed for its player, the test data has no reveal source) ordered onto an enemy squad 1300
//   away; 2 frames into the chase the fog comes back (Reveal_All undone); once the enemy's TEAM container is hidden for player
//   0 (FOG_OF_WAR §5.2, after its reveal memory) the enemy squad is moved 300 sideways (> R = 125). While the squad walks no 30-frame check may re-path
//   (0x4165f0 false for a hidden non-guard top) or land-stop (c false): the goal is kept for more than one check interval.
//   After arrival (states 3 / 4, C 3) d = |L - P| >= 40 (P = the hidden squad's position) plans again toward P: a new goal
//   nearer the moved enemy within one check.
//   stealthed: S10_Trooper gets a Stealth_Ability (unit ability STEALTH); the attackers' fire range +140 % (600: they stop
//   beyond the enemy's 250, so it never fires and breaks stealth). After the attack formation has arrived, every enemy
//   trooper turns stealth on; the next check (C 3: P = the lead's own position, d = 0 < 40) turns the block into a position
//   block at L: the record goes (BattleStats::attack_unseen_drops + 1), every attacker's attack order and attack goal are
//   cleared (0x38d730(member, null)), the movement's attack block too, and the squad stays where it stands.
struct LandAttackUnseenRun {
    bool ordered = false, hidden = false, moved = false, goal_kept = false, arrived = false, replanned = false, nearer = false;
    uint32_t window = 0, replans_moving = 0, stops_moving = 0, replan_frames = 0;
    bool stealthed = false, dropped = false, orders_cleared = false, block_cleared = false, stood = false;
    uint32_t drop_frames = 0;
};

inline std::string trooper_stealth(std::string t) {  // S10_Trooper: unit ability STEALTH + its Stealth_Ability
    const std::string at = "<GroundInfantry Name=\"S10_Trooper\">";
    if (const size_t p = t.find(at); p != std::string::npos)
        t.insert(p + at.size(),
                 "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>STEALTH</Type><Recharge_Seconds>60.0</Recharge_Seconds>"
                 "<Expiration_Seconds>600.0</Expiration_Seconds><GUI_Activated_Ability_Name>S10_Stealth</GUI_Activated_Ability_Name>"
                 "</Unit_Ability></Unit_Abilities_Data><Abilities SubObjectList=\"Yes\"><Stealth_Ability Name=\"S10_Stealth\">"
                 "<Activation_Style>User_Input</Activation_Style></Stealth_Ability></Abilities>");
    return t;
}

inline LandAttackUnseenRun run_land_attack_unseen(const Data& d, bool stealth) {
    LandAttackUnseenRun out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 9000;
    opt.battle.skip_human_units = true;
    opt.battle.order_interval = 0;  // no stand-in orders
    ClashSession s(d, 0, opt, {"S10_Squad"}, {}, true);
    if (!s.gs) return out;
    int stage = 0;
    uint32_t stage_frame = 0, replans0 = 0, stops0 = 0;
    objects::ObjectId sq = objects::kNoObjectId, tcid = objects::kNoObjectId, target = objects::kNoObjectId;
    std::vector<objects::ObjectId> mem, tmem;
    Vec3 dir{}, perp{};
    mv::V2 goal0{}, pos0{};
    for (uint64_t t = 0; t < 9000 && stage < 9; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 3) {
            if (stage > 0) break;
            continue;
        }
        objects::ObjectStore& st = tm->primary();
        auto& cs = const_cast<tc::CombatSystem&>(b->combat());
        const mv::MovementSystem& ms = b->movement();
        if (stage == 0) {
            std::map<uint32_t, std::vector<objects::ObjectId>> teams;
            for (size_t i = 0; i < cs.unit_count(); ++i) {
                const auto id = cs.unit_id_at(i);
                cs.make_invulnerable(id, 1000000, b->frame());
                if (cs.team(id) != 0 && cs.alive(id)) teams[cs.team(id)].push_back(id);
            }
            for (const auto& [tid, m] : teams) {
                const objects::ObjectHandle th = st.find(static_cast<objects::ObjectId>(tid));
                if (m.size() < 2 || !st.alive(th) || !ms.has_unit(static_cast<objects::ObjectId>(tid))) continue;
                if (st.owner(th) == 0 && sq == objects::kNoObjectId) {
                    sq = static_cast<objects::ObjectId>(tid);
                    mem = m;
                } else if (st.owner(th) == 1 && target == objects::kNoObjectId) {
                    tcid = static_cast<objects::ObjectId>(tid);
                    tmem = m;
                    target = m.front();
                }
            }
            CHECK(sq != objects::kNoObjectId && target != objects::kNoObjectId && b->fog() != nullptr);
            if (sq == objects::kNoObjectId || target == objects::kNoObjectId || !b->fog()) break;
            b->fog()->reveal_all(0);  // nothing hidden for player 0
            const Vec3 tp = st.position(st.find(target)), ap = st.position(st.find(sq));
            const float dx = ap.x - tp.x, dy = ap.y - tp.y, dl = std::sqrt(dx * dx + dy * dy);
            dir = dl > 1.0f ? Vec3{dx / dl, dy / dl, 0.0f} : Vec3{1.0f, 0.0f, 0.0f};
            perp = Vec3{-dir.y, dir.x, 0.0f};
            CHECK(b->teleport(*tm, sq, Vec3{tp.x + dir.x * (stealth ? 900.0f : 1300.0f), tp.y + dir.y * (stealth ? 900.0f : 1300.0f), ap.z},
                              st.facing(st.find(sq))));
            if (stealth)
                for (const objects::ObjectId id : mem)  // fire range +140 % (250 -> 600)
                    if (tc::ModifierSet* m = cs.unit_modifiers(id))
                        m->set_by_source(tc::ModList::FireRange, 0x54325233, 1.4f, tc::kPermanent, 0x54325233u, -0x5433);
            stage = 1;
            stage_frame = b->frame();
            continue;
        }
        const mv::UnitView v = ms.view(sq);
        if (stage == 1) {
            if (b->frame() < stage_frame + 2) continue;
            CHECK(!b->fog_object_hidden(0, tcid, true));
            out.ordered = b->order_attack(*tm, 0, mem, target, s.host->random().synced()) == commands::RejectReason::None;
            CHECK(out.ordered && b->attack_blocks() == 1);
            stage = stealth ? 5 : 2;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 2) {  // hidden: the fog back 2 frames into the chase; the squad hides once its memory lapses (~7 s)
            if (b->frame() < stage_frame + 2) continue;
            if (!b->fog()->reveal_all_players().empty()) b->fog()->undo_reveal_all(0);
            out.hidden = b->fog_object_hidden(0, tcid, true);
            if (!out.hidden) {
                if (b->frame() > stage_frame + 600 || !v.has_goal) stage = 9;
                continue;
            }
            CHECK(v.has_goal);
            const objects::ObjectHandle th = st.find(tcid);
            const Vec3 tp = st.position(th);
            out.moved = b->teleport(*tm, tcid, Vec3{tp.x + perp.x * 300.0f, tp.y + perp.y * 300.0f, tp.z}, st.facing(th));
            replans0 = ms.stats().attack_replans;
            stops0 = b->stats().attack_land_stops;
            goal0 = v.goal;
            out.goal_kept = true;
            stage = 3;
            stage_frame = b->frame();
            continue;
        }
        if (stage == 3) {
            if (v.has_goal) {
                ++out.window;
                out.goal_kept = out.goal_kept && v.goal == goal0;
                out.replans_moving = ms.stats().attack_replans - replans0;
                out.stops_moving = b->stats().attack_land_stops - stops0;
            } else {
                out.arrived = v.speed == 0.0f || out.arrived;
                replans0 = ms.stats().attack_replans;
                stage = 4;
                stage_frame = b->frame();
            }
            if (b->frame() > stage_frame + 4000) stage = 9;
            continue;
        }
        if (stage == 4) {
            if (v.has_goal && ms.stats().attack_replans > replans0) {
                out.replanned = true;
                out.replan_frames = b->frame() - stage_frame;
                const Vec3 tp = st.position(st.find(tcid));
                const auto dd = [&](mv::V2 p) { return std::sqrt((p.x - tp.x) * (p.x - tp.x) + (p.y - tp.y) * (p.y - tp.y)); };
                out.nearer = dd(v.goal) < dd(goal0);
                stage = 9;
            } else if (b->frame() > stage_frame + 40) {
                stage = 9;
            }
            continue;
        }
        if (stage == 5) {  // stealthed: wait for the arrival
            if (!v.has_goal && v.speed == 0.0f && b->frame() > stage_frame + 30) {
                out.arrived = b->attack_blocks() == 1 && v.atk_target != -1;
                std::vector<objects::ObjectId> on(tmem.begin(), tmem.end());
                (void)b->abilities().set_unit_ability_mode(on, tactical::abilities::kStealth, true, b->frame());
                out.stealthed = b->abilities().is_stealthed(target, b->frame());
                stops0 = b->stats().attack_unseen_drops;
                pos0 = v.pos;
                stage = 6;
                stage_frame = b->frame();
            } else if (b->frame() > stage_frame + 4000) {
                stage = 9;
            }
            continue;
        }
        if (stage == 6) {
            if (b->attack_blocks() == 0) {
                out.dropped = b->stats().attack_unseen_drops == stops0 + 1;
                out.drop_frames = b->frame() - stage_frame;
                out.orders_cleared = true;
                for (const objects::ObjectId id : mem)
                    out.orders_cleared = out.orders_cleared && cs.ordered_target(id) == tc::kNoObject && cs.attack_goal(id) == tc::kNoObject;
                out.block_cleared = v.atk_target == -1 && v.atk_form == 0 && v.atk_rho == 0.0f;
                stage = 7;
                stage_frame = b->frame();
            } else if (b->frame() > stage_frame + 40) {
                stage = 9;
            }
            continue;
        }
        if (stage == 7) {  // it stays where it stands (no move toward L, no new attack)
            if (b->frame() < stage_frame + 60) continue;
            out.stood = !v.has_goal && std::abs(v.pos.x - pos0.x) < 1.0f && std::abs(v.pos.y - pos0.y) < 1.0f;
            stage = 9;
        }
    }
    return out;
}

inline void test_land_attack_unseen() {
    {
        auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                           false, false, false, true);
        const LandAttackUnseenRun r = run_land_attack_unseen(*d, false);
        std::printf("  land attack, target hidden mid-chase: ordered %d, hidden %d, moved 300 %d; walking %u frame(s), re-paths %u, "
                    "land stops %u, goal kept %d; arrived %d, re-planned toward it %d after %u frame(s), nearer %d\n",
                    r.ordered ? 1 : 0, r.hidden ? 1 : 0, r.moved ? 1 : 0, r.window, r.replans_moving, r.stops_moving,
                    r.goal_kept ? 1 : 0, r.arrived ? 1 : 0, r.replanned ? 1 : 0, r.replan_frames, r.nearer ? 1 : 0);
        CHECK(r.ordered && r.hidden && r.moved && r.window > 31 && r.replans_moving == 0 && r.stops_moving == 0 && r.goal_kept);
        CHECK(r.arrived && r.replanned && r.replan_frames <= 31 && r.nearer);
    }
    {
        auto d = squad_data_patched("_stealth", trooper_stealth);
        const LandAttackUnseenRun r = run_land_attack_unseen(*d, true);
        std::printf("  land attack, target stealthed on arrival: ordered %d, arrived %d, stealthed %d; dropped %d after %u frame(s), "
                    "orders cleared %d, movement block cleared %d, stood %d\n",
                    r.ordered ? 1 : 0, r.arrived ? 1 : 0, r.stealthed ? 1 : 0, r.dropped ? 1 : 0, r.drop_frames,
                    r.orders_cleared ? 1 : 0, r.block_cleared ? 1 : 0, r.stood ? 1 : 0);
        CHECK(r.ordered && r.arrived && r.stealthed && r.dropped && r.drop_frames <= 31 && r.orders_cleared && r.block_cleared && r.stood);
    }
}

inline void test_land_squad_garrison_save_load() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, false, true);
    SquadGarrisonScene a(*d, true);
    CHECK(a.s.gs != nullptr);
    if (!a.s.gs || !a.start()) {
        CHECK(false);
        return;
    }
    namespace tg = alamo::tactical::garrison;
    const auto gates_equal = [&](const SquadGarrisonScene& f) {
        std::vector<objects::ObjectId> ids = a.members;
        ids.push_back(a.team);
        ids.push_back(a.walker);
        for (const auto id : ids) {
            mv::ObjectId ha = -2, hf = -2;
            bool hida = false, hidf = false;
            const bool ia = a.b->movement().pair_gate(id, ha, hida), jf = f.b->movement().pair_gate(id, hf, hidf);
            if (ia != jf || ha != hf || hida != hidf) return false;
        }
        return true;
    };
    const auto same_now = [&](const SquadGarrisonScene& f) {
        if (clash_digest(*a.b) != clash_digest(*f.b) || !gates_equal(f)) return false;
        for (const auto id : a.members)
            if (a.b->garrison_holder_of(id) != f.b->garrison_holder_of(id)) return false;
        return true;
    };
    // Saves the running session and loads it into a fresh one (null on failure); checked equal before any tick.
    const auto load_copy = [&](const char* what) -> std::unique_ptr<SquadGarrisonScene> {
        SaveFile save;
        std::string why;
        CHECK(a.s.host->save_allowed(&why));
        const bool saved = a.s.host->save(save, &why);
        if (!saved) std::printf("  squad garrison save (%s): %s\n", what, why.c_str());
        CHECK(saved);
        if (!saved) return nullptr;
        auto f = std::make_unique<SquadGarrisonScene>(*d, false);
        CHECK(f->s.gs != nullptr);
        if (!f->s.gs) return nullptr;
        std::string err;
        LoadReport rep;
        const bool loaded = f->s.host->load(save, &err, &rep);
        if (!loaded) std::printf("  squad garrison load (%s): %s\n", what, rep.text().c_str());
        CHECK(loaded && f->s.host->frame() == a.s.host->frame());
        if (!loaded) return nullptr;
        f->s.invaded = true;
        f->team = a.team;
        f->walker = a.walker;
        f->members = a.members;
        f->b = f->s.battles->battle();
        f->tm = f->s.host->tactical();
        CHECK(f->b && f->tm);
        if (!f->b || !f->tm) return nullptr;
        const bool same = same_now(*f);
        if (!same) std::printf("  squad garrison load (%s): the loaded session differs before its first tick\n", what);
        CHECK(same);
        return f;
    };
    // `ticks` ticks of both sessions; true when every one ended equal.
    const auto lockstep = [&](SquadGarrisonScene& f, int ticks, const char* what) {
        int same = 0;
        for (int i = 0; i < ticks; ++i) {
            const bool ta_ok = a.tick(), tf_ok = f.tick();
            CHECK(ta_ok && tf_ok);
            if (!ta_ok || !tf_ok) return false;
            if (same_now(f)) ++same;
            else if (same == i) std::printf("  squad garrison load (%s): differs %d tick(s) after the load\n", what, i + 1);
        }
        CHECK(same == ticks);
        return same == ticks;
    };
    const mv::MovementSystem& ms = a.b->movement();
    const auto gate = [](const SquadGarrisonScene& x, objects::ObjectId id, mv::ObjectId& host) {
        bool hidden = false;
        host = -1;
        CHECK(x.b->movement().pair_gate(id, host, hidden));
        return hidden;
    };
    SimRng& rng = a.s.host->random().synced();
    CHECK(a.b->order_garrison(*a.tm, 0, a.members, a.walker, rng) == commands::RejectReason::None);
    // 1. the approach: the members walk to the host (pending, not entering), the container's garrison host is the walker
    for (int i = 0; i < 3; ++i) CHECK(a.tick());
    bool approaching = a.inside() == 0;
    for (const auto id : a.members) approaching = approaching && a.b->garrison().pending_holder(id) == a.walker && !a.b->garrison().entering(id);
    CHECK(approaching && ms.view(a.team).has_goal);
    mv::ObjectId gh = -1;
    CHECK(!gate(a, a.team, gh) && gh == a.walker);
    const uint32_t approach_frame = a.b->frame();
    if (auto f = load_copy("approach")) {
        CHECK(!gate(*f, a.team, gh) && gh == a.walker);  // fed after the load: no collision object with the host
        lockstep(*f, 40, "approach");
    }
    // 2. the entry fade (30 frames from EnterStarted)
    bool entering = false;
    for (int i = 0; i < 1500 && !entering && a.inside() == 0; ++i) {
        CHECK(a.tick());
        for (const auto id : a.members) entering = entering || a.b->garrison().entering(id);
    }
    CHECK(entering && a.inside() == 0);
    const uint32_t enter_frame = a.b->frame();
    uint32_t inside_frame = 0;
    if (auto f = load_copy("entry")) {
        bool ef = false;
        for (const auto id : a.members) ef = ef || f->b->garrison().entering(id);
        CHECK(ef);
        // both run through the rest of the fade: the whole squad is inside on the same tick, the container hidden at once
        bool ok = true;
        for (int i = 0; i < 60 && ok && a.inside() != a.members.size(); ++i) ok = lockstep(*f, 1, "entry");
        CHECK(ok && a.inside() == a.members.size() && f->inside() == a.members.size());
        inside_frame = a.b->frame();
        CHECK(gate(a, a.team, gh) && gh == a.walker && gate(*f, a.team, gh) && gh == a.walker);
    }
    for (int i = 0; i < 60 && a.inside() != a.members.size(); ++i) CHECK(a.tick());  // (only when the load above failed)
    CHECK(a.inside() == a.members.size());
    // 3. the whole squad inside: the container is hidden in the saving session and, before any tick, in the loaded one
    for (int i = 0; i < 5; ++i) CHECK(a.tick());
    CHECK(gate(a, a.team, gh));
    bool walked_out = false;
    uint32_t leave_frame = 0, out_frame = 0;
    if (auto f = load_copy("inside")) {
        CHECK(gate(*f, a.team, gh) && gh == a.walker);
        for (const auto id : a.members) CHECK(!f->b->movement().has_unit(id) && f->b->movement().team_member_parked(id));
        lockstep(*f, 20, "inside");
        CHECK(gate(a, a.team, gh) && gate(*f, a.team, gh));  // still hidden, in both
        // the leave order in both, on the same tick: the squad leaves as one and its container walks out
        const objects::ObjectId one[] = {a.members.front()};
        CHECK(a.b->order_leave_garrison(*a.tm, 0, one) == commands::RejectReason::None);
        CHECK(f->b->order_leave_garrison(*f->tm, 0, one) == commands::RejectReason::None);
        leave_frame = a.b->frame();
        CHECK(same_now(*f) && a.inside() == 0 && f->inside() == 0);
        CHECK(!gate(a, a.team, gh) && !gate(*f, a.team, gh));  // unhidden with its squad, at once
        CHECK(ms.view(a.team).has_goal && f->b->movement().view(a.team).has_goal);
        lockstep(*f, 3, "leave");
        // 4. the walk-out: a second save while the container walks to the exit point
        CHECK(ms.view(a.team).has_goal);
        if (auto g = load_copy("walk-out")) {
            CHECK(g->b->movement().view(a.team).has_goal);
            bool ok = true;
            for (int i = 0; i < 600 && ok && ms.view(a.team).has_goal; ++i) ok = lockstep(*g, 1, "walk-out") && f->tick() && same_now(*f);
            walked_out = ok && !ms.view(a.team).has_goal && ms.view(a.team).arrived && g->b->movement().view(a.team).arrived;
            out_frame = a.b->frame();
            CHECK(walked_out);
            if (ok) lockstep(*g, 30, "after the walk-out");
        }
    }
    std::printf("  land squad garrison save / load: approach saved at frame %u, entry at %u, all inside at %u, leave at %u, walked out "
                "at %u (%d)\n", approach_frame, enter_frame, inside_frame, leave_frame, out_frame, walked_out ? 1 : 0);
    // 5. a hostile host: the walker changes sides - the garrison order is refused before any side effect (the squad's
    // order queue, its move and the synced RNG are untouched)
    for (int i = 0; i < 200 && ms.view(a.team).has_goal; ++i) CHECK(a.tick());
    if (!a.b || !a.tm) return;
    objects::ObjectStore& st = a.tm->primary();
    bool alive = a.b->combat().alive(a.walker);
    for (const auto id : a.members) alive = alive && a.b->combat().alive(id);
    CHECK(alive);
    if (!alive) return;
    const Vec3 p = st.position(st.find(a.team));
    CHECK(a.b->order_move(*a.tm, 0, a.members, p.x + 80.0f, p.y, rng) == commands::RejectReason::None);
    CHECK(a.b->order_move(*a.tm, 0, a.members, p.x + 80.0f, p.y + 80.0f, rng, mv::kNoFacing, false, true) == commands::RejectReason::None);
    CHECK(a.tick());
    CHECK(a.b->can_garrison(a.members.front(), a.walker));  // its own side's walker: allowed
    CHECK(a.b->script_change_owner(*a.tm, st.find(a.walker), 1));
    CHECK(!a.b->can_garrison(a.members.front(), a.walker) && a.b->garrison().masks_match(a.members.front(), a.walker));
    const size_t queued0 = a.b->queued_orders(a.members.front()).size();
    CHECK(queued0 == 1);
    const mv::UnitView v0 = ms.view(a.team);
    const uint64_t draws0 = rng.draws();
    CHECK(a.b->order_garrison(*a.tm, 0, a.members, a.walker, rng) == commands::RejectReason::InvalidTarget);
    const mv::UnitView v1 = ms.view(a.team);
    CHECK(rng.draws() == draws0);
    for (const auto id : a.members) CHECK(a.b->queued_orders(id).size() == queued0 && a.b->garrison().pending_holder(id) == objects::kNoObjectId);
    CHECK(v0.has_goal && v1.has_goal && v1.goal == v0.goal && v1.pos == v0.pos && v1.start_frame == v0.start_frame && v1.speed == v0.speed);
}

// Test functions (defined in the part files)

// alamo_sim_battle_tests: S10 battle transitions, host side (docs/re/BATTLE_TRANSITIONS.md).
//   * synthetic game data (types, hardpoints, projectiles, factions, contrast table) + a 3-planet galaxy;
//   * space: a human-defended orbit is attacked, the conflict is FOUGHT: tactical Space mode pushed over the
//     suspended galaxy, MovementSystem + CombatSystem run the battle on an in-memory map, the result is
//     written back (losses killed in the galaxy, loser retreats), mode popped, story events posted;
//   * land: an invasion against a defended surface is fought on an in-memory land map; a winning
//     attacker lands its survivors;
//   * the same campaign with auto-resolve only;
//   * per-tick CRC logs identical for 0/1/4/N workers and equal to testdata/battle_synthetic_golden.crc
//     (MSVC writes it with --regen; clang / gcc must reproduce it).
// With ALAMO_FOC_DATA (FoC "corruption" folder) a real FoC space map battle runs as well.

#include "battle_test_fixture.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/tactical/tree_set.hpp"

namespace {

void test_space_battle(const Data& d) {
    const uint64_t ticks = 2500;
    const Run a = run_campaign(d, false, true, 0, ticks);
    for (const std::string& s : a.battles) std::printf("  %s\n", s.c_str());
    for (const std::string& s : a.conflicts) std::printf("  %s\n", s.c_str());
    CHECK(a.fought == 1 && a.battles.size() == 1);
    CHECK(a.conflicts.size() == 1 && a.conflicts[0].find("tactical S10_Space_B") != std::string::npos);
    CHECK(a.depth == 1);  // the tactical mode was popped
    CHECK(a.won == 1 && a.lost == 1);
    CHECK(a.trace_battles);
    CHECK(!a.crc.records.empty() && a.crc.records.front().systems.size() == 12);  // 7 host + 5 battle systems
    // S10-int: ability data parsed from Unit_Abilities_Data (frigate + its corvette variant), command 43 accepted
    // and activated, hit particles spawned as PARTICLE objects.
    std::printf("  abilities: records %u, command 43 -> %d, activations %u, particle objects %u\n", a.ability_records,
                a.ability_command, a.activations, a.particles);
    CHECK(a.ability_records >= 2 && a.ability_command == 0 && a.activations > 0 && a.particles > 0);
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const Run b = run_campaign(d, false, true, w, ticks);
        const auto dv = first_divergence(a.crc, b.crc);
        std::printf("  space battle workers %2d: %s\n", w, dv.message.c_str());
        CHECK(!dv.diverged && b.battles == a.battles);
    }
    CrcLog log = a.crc;
    log.comments = {"S10 synthetic campaign: Underworld attacks the Empire orbit at S10_B, fought on S10_Space_B, seed 2026, " +
                    std::to_string(ticks) + " ticks"};
    std::string err;
    if (g_regen) {
        CHECK(log.save(golden_path(), &err));
        std::printf("  wrote %s\n", golden_path().c_str());
    } else {
        CrcLog g;
        if (!CrcLog::load(golden_path(), g, &err)) {
            std::printf("FAIL golden %s: %s (run with --regen)\n", golden_path().c_str(), err.c_str());
            ++g_fail;
        } else {
            const auto dv = first_divergence(g, log);
            std::printf("  golden battle_synthetic_golden.crc: %s\n", dv.message.c_str());
            CHECK(!dv.diverged);
        }
    }
}

void test_land_battle(const Data& d) {
    const Run a = run_campaign(d, true, true, 0, 3000);
    for (const std::string& s : a.battles) std::printf("  %s\n", s.c_str());
    for (const std::string& s : a.conflicts) std::printf("  %s\n", s.c_str());
    CHECK(a.fought == 1 && a.depth == 1);
    bool land_logged = false;
    for (const std::string& s : a.conflicts) land_logged = land_logged || s.find(" land S10_B ") != std::string::npos;
    CHECK(land_logged);
    // Whoever won holds the surface alone.
    CHECK((a.under_surface > 0) != (a.empire_surface > 0));
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // AI attacker (Underworld): credits halved for the land battle, the leftovers returned afterwards.
    std::printf("  credits: before %.1f, in battle %.1f, after %.1f\n", static_cast<double>(a.credits_before),
                static_cast<double>(a.credits_in_battle), static_cast<double>(a.credits_after));
    CHECK(a.credits_in_battle == 0.5f * a.credits_before && a.credits_after == a.credits_before);
#endif
    const Run b = run_campaign(d, true, true, 4, 3000);
    CHECK(!first_divergence(a.crc, b.crc).diverged);
}

// Stand-in retreat: the weaker Empire leaves after the 5 s retreat lock (test GameConstants) + its 5 s countdown; its ships survive in the
// galaxy and retreat to a neighbouring planet; the Underworld wins by "all capable enemies retreated".
void test_retreat(const Data& d) {
    const Run a = run_campaign(d, false, true, 0, 2500, 0.95f);
    for (const std::string& s : a.battles) std::printf("  %s\n", s.c_str());
    for (const std::string& s : a.conflicts) std::printf("  %s\n", s.c_str());
    CHECK(a.battles.size() == 1 && a.battles[0].find("all-retreated") != std::string::npos);
    CHECK(a.conflicts.size() == 1 && a.conflicts[0].find("retreat Empire") != std::string::npos);
    CHECK(a.empire_frigate_alive);
    // Playtest QA D1: the station cannot jump out, so it dies at the end of the retreat (BT §3.4), counts as a galactic loss
    // and is in the battle's loss records; the ships that left are not.
    CHECK(a.battles.size() == 1 && a.battles[0].find("+ starbase") != std::string::npos);
    const uint32_t base = type_of(d, "S10_Star_Base_1");
    const auto count = [](const Run& r, uint32_t t, int32_t owner) {
        return std::count_if(r.losses.begin(), r.losses.end(), [&](const auto& l) { return l.object_type == t && l.owner == owner; });
    };
    CHECK(count(a, base, 0) == 1);
    // story t06 (BT §3.1a): 0xc is declared when the retreat completes and the battle ends after the record's 210-frame
    // countdown (the world keeps ticking).
    CHECK(a.hold_ticks == 210);
    // A longer end hold (BattleConfig::end_hold_frames) is a minimum on that countdown: same winner and write-back;
    // deterministic across runs.
    const Run b = run_campaign(d, false, true, 0, 2700, 0.95f, true, 300);
    CHECK(b.hold_ticks == 300);
    CHECK(b.battles.size() == 1 && b.battles[0].find("all-retreated") != std::string::npos && b.battles[0].find("+ starbase") != std::string::npos);
    CHECK(b.conflicts.size() == 1 && b.conflicts[0].find("retreat Empire") != std::string::npos);
    CHECK(b.empire_frigate_alive && count(b, base, 0) == 1);
    const Run c = run_campaign(d, false, true, 4, 2700, 0.95f, true, 300);
    CHECK(!first_divergence(b.crc, c.crc).diverged);
}

// ---- integration fixes: human order API + skip_human_units; [port: Gabriel] queueable unit actions ----------------
// The space campaign with BattleConfig::skip_human_units: the stand-in never orders the human Empire's ships. At
// battle frame 3 the human orders its frigate F: plain move to P1 (with the corvette C), then queued: move to P2,
// attack the first Underworld frigate's hard point 0, POWER_TO_WEAPONS (command 43 with EventData::queued through
// the host's direct path). The queue must run in that order; plain orders / Stop clear C's queue. Per-tick queue
// trace + CRC log identical for 0/1/4/15 workers and equal to testdata/battle_queue_golden.crc.
struct QueueRun {
    CrcLog crc;
    uint64_t trace = 0;  // digest of (frame, F queue size, F goal, F ordered target) per tick
    int stage = -1;      // F's queue: 3 entries after the commands, then each completed stage
    bool order_ok = true, skip_ok = false, ai_moved = false, clears_ok = false;
    bool move2_ok = false, attack_ok = false, ability_ok = false;
    std::vector<int> stages;  // queue sizes in the order seen
};

std::string queue_golden_path() { return std::string(ALAMO_SIM_TESTDATA) + "/battle_queue_golden.crc"; }

QueueRun run_orders(const Data& d, int32_t workers, uint64_t ticks) {
    QueueRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.skip_human_units = true;
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
    using R = commands::RejectReason;
    using QO = TacticalBattle::QueuedOrder;
    objects::ObjectId fid = objects::kNoObjectId, eid = objects::kNoObjectId;
    float p2x = 0.0f, p2y = 0.0f;
    StateHasher trace;
    for (uint64_t t = 0; t < ticks; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        const objects::ObjectStore& st = tm->primary();
        if (b->frame() == 3 && fid == objects::kNoObjectId) {
            objects::ObjectId cid = objects::kNoObjectId;
            out.skip_ok = true;
            for (objects::ObjectHandle h : st.owned_by(0)) {
                const objects::ObjectId id = st.id(h);
                if (!st.alive(h) || !b->movement().has_unit(id)) continue;
                out.skip_ok = out.skip_ok && !b->movement().view(id).has_goal;  // the stand-in left them alone
                if (st.type(h) == d.types.resolve_name("S10_Frigate") && fid == objects::kNoObjectId) fid = id;
                if (st.type(h) == d.types.resolve_name("S10_Corvette") && cid == objects::kNoObjectId) cid = id;
            }
            for (objects::ObjectHandle h : st.owned_by(1)) {
                const objects::ObjectId id = st.id(h);
                if (!st.alive(h) || !b->movement().has_unit(id)) continue;
                out.ai_moved = out.ai_moved || b->movement().view(id).has_goal;
            }
            // The Underworld corvette nearest to F (one HP_S10_Laser_B hard point, 100 health).
            float best = 0.0f;
            const Vec3 fpos = fid != objects::kNoObjectId ? st.position(st.find(fid)) : Vec3{};
            for (objects::ObjectHandle h : st.owned_by(1)) {
                if (!st.alive(h) || st.type(h) != d.types.resolve_name("S10_Corvette")) continue;
                const Vec3 p = st.position(h);
                const float d2 = (p.x - fpos.x) * (p.x - fpos.x) + (p.y - fpos.y) * (p.y - fpos.y);
                if (eid == objects::kNoObjectId || d2 < best) {
                    eid = st.id(h);
                    best = d2;
                }
            }
            CHECK(fid != objects::kNoObjectId && cid != objects::kNoObjectId && eid != objects::kNoObjectId);
            if (fid == objects::kNoObjectId || cid == objects::kNoObjectId || eid == objects::kNoObjectId) break;
            SimRng& rng = host.random().synced();
            const Vec3 p0 = st.position(st.find(fid));
            const Vec3 e0 = st.position(st.find(eid));
            const float p1x = p0.x, p1y = p0.y + 100.0f;  // short hops, then towards the enemy
            p2x = p0.x + 0.05f * (e0.x - p0.x);
            p2y = p0.y + 0.05f * (e0.y - p0.y);
            const objects::ObjectId fc[] = {fid, cid}, f1[] = {fid}, c1[] = {cid};
            CHECK(b->order_move(*tm, 0, fc, p1x, p1y, rng) == R::None);
            CHECK(b->order_move(*tm, 1, fc, p1x, p1y, rng) == R::InvalidUnit);  // not player 1's units
            CHECK(b->order_move(*tm, 0, f1, p2x, p2y, rng, tactical::movement::kNoFacing, false, true) == R::None);
            CHECK(b->order_attack(*tm, 0, f1, fid, rng, 0, true) == R::InvalidTarget);  // own unit
            CHECK(b->order_attack(*tm, 0, f1, eid, rng, 0, true) == R::None);
            commands::EventData ev(commands::EventType::SetUnitAbilityMode);
            ev.set("ability", 29).set("on", 1).set_ids("units", {static_cast<uint32_t>(fid)});
            ev.set_queued(true);
            CHECK(host.execute_direct(0, commands::CommandSource::Player, ev) == R::None);
            const auto q = b->queued_orders(fid);
            CHECK(q.size() == 3 && q[0].kind == QO::Kind::Move && q[1].kind == QO::Kind::Attack && q[1].target == eid &&
                  q[1].hardpoint == 0 && q[2].kind == QO::Kind::Ability && q[0].group < q[1].group && q[1].group < q[2].group);
            // C: queued behind its move, cleared by Stop; an idle unit runs a queued order at once; a plain order clears.
            CHECK(b->order_move(*tm, 0, c1, p2x, p2y, rng, tactical::movement::kNoFacing, false, true) == R::None);
            const bool queued1 = b->queued_orders(cid).size() == 1;
            CHECK(b->order_stop(*tm, 0, c1) == R::None);
            const bool stopped = b->queued_orders(cid).empty() && !b->movement().view(cid).has_goal;
            CHECK(b->order_move(*tm, 0, c1, p2x, p2y, rng, tactical::movement::kNoFacing, false, true) == R::None);
            const bool ran_now = b->queued_orders(cid).empty() && b->movement().view(cid).has_goal;
            CHECK(b->order_move(*tm, 0, c1, p1x, p1y, rng, tactical::movement::kNoFacing, false, true) == R::None);
            const bool queued2 = b->queued_orders(cid).size() == 1;
            CHECK(b->order_move(*tm, 0, c1, p1x, p1y, rng) == R::None);
            out.clears_ok = queued1 && stopped && ran_now && queued2 && b->queued_orders(cid).empty();
            out.stage = 3;
            out.stages.push_back(3);
        }
        if (fid == objects::kNoObjectId) continue;
        const int n = static_cast<int>(b->queued_orders(fid).size());
        const bool alive = st.alive(st.find(fid));
        if (alive && n != out.stage) {
            if (n != out.stage - 1) out.order_ok = false;  // one entry at a time, in order
            out.stage = n;
            out.stages.push_back(n);
            if (n == 2) {  // the queued move started
                const auto v = b->movement().view(fid);
                const float dx = v.goal.x - p2x, dy = v.goal.y - p2y;
                out.move2_ok = v.has_goal && dx * dx + dy * dy < 300.0f * 300.0f;
            } else if (n == 1) {  // the attack on hard point 0
                out.attack_ok = b->combat().ordered_target(fid) == eid && b->combat().ordered_hardpoint(fid) == 0;
            } else if (n == 0) {  // hard point 0 (or the target) gone: POWER_TO_WEAPONS
                out.ability_ok = b->abilities().is_active(fid, 29);
            }
        }
        trace.u64(b->frame());
        trace.i32(n);
        if (alive && b->movement().has_unit(fid)) {
            const auto v = b->movement().view(fid);
            trace.f32(v.goal.x);
            trace.f32(v.goal.y);
            trace.i32(b->combat().ordered_target(fid));
        }
    }
    out.trace = trace.digest();
    out.crc = host.crc_log();
    return out;
}

void test_orders_and_queue(const Data& d) {
    const uint64_t ticks = 1500;
    const QueueRun a = run_orders(d, 0, ticks);
    std::string seq;
    for (int s : a.stages) seq += std::to_string(s) + " ";
    std::printf("  skip %d ai_moved %d clears %d order %d stages [%s] move2 %d attack %d ability %d\n", a.skip_ok, a.ai_moved,
                a.clears_ok, a.order_ok, seq.c_str(), a.move2_ok, a.attack_ok, a.ability_ok);
    CHECK(a.skip_ok && a.ai_moved && a.clears_ok && a.order_ok);
    CHECK(a.stages.size() == 4 && a.move2_ok && a.attack_ok && a.ability_ok);
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const QueueRun b = run_orders(d, w, ticks);
        const auto dv = first_divergence(a.crc, b.crc);
        std::printf("  order queue workers %2d: %s\n", w, dv.message.c_str());
        CHECK(!dv.diverged && b.trace == a.trace && b.stages == a.stages);
    }
    CrcLog log = a.crc;
    log.comments = {"S10 synthetic campaign, skip_human_units, human orders + queued move / hard point attack / ability at battle "
                    "frame 3, seed 2026, " + std::to_string(ticks) + " ticks"};
    std::string err;
    if (g_regen) {
        CHECK(log.save(queue_golden_path(), &err));
        std::printf("  wrote %s\n", queue_golden_path().c_str());
    } else {
        CrcLog g;
        if (!CrcLog::load(queue_golden_path(), g, &err)) {
            std::printf("FAIL golden %s: %s (run with --regen)\n", queue_golden_path().c_str(), err.c_str());
            ++g_fail;
        } else {
            const auto dv = first_divergence(g, log);
            std::printf("  golden battle_queue_golden.crc: %s\n", dv.message.c_str());
            CHECK(!dv.diverged);
        }
    }
}

void test_auto_only(const Data& d) {
    const Run a = run_campaign(d, false, false, 0, 600);
    for (const std::string& s : a.conflicts) std::printf("  %s\n", s.c_str());
    CHECK(a.fought == 0 && a.conflicts.size() == 1 && a.conflicts[0].find("auto-resolved") != std::string::npos);
    CHECK(a.won == 1 && a.lost == 1 && a.depth == 1);
}


using ::alamo::test::env_or_empty;

// Fog of war in battles (owner: fog agent; docs/re/FOG_OF_WAR.md): units with REVEAL reveal for their side, the
// human sees part of the map and enemy units hidden / shown over time, the AI player is never fogged (force 0),
// the neutral player has no grid; fog is worker-count independent (identical per-frame fog digests and CRC logs for
// 0 and 4 workers). Fog follow-up: the grids are still not in the CRC, but the human's targeting now reads them
// (S8 filter step 4: no target acquired while hidden), so fog on / off may diverge; Force Sight (land) = Reveal_All
// for 2 s; the ping (space) reveals 2000 units ahead one tick after use.
void test_fog_battle(const Data& d) {
    for (const bool land : {false, true}) {
        const uint64_t ticks = land ? 3000 : 2500;
        const Run a = run_campaign(d, land, true, 0, ticks, 0.0f, true);
        const Run off = run_campaign(d, land, true, 0, ticks, 0.0f, false);
        const Run w4 = run_campaign(d, land, true, 4, ticks, 0.0f, true);
        std::printf("  %s: fog frames %u, sources max %u, visible cells max %u, enemy hidden %u / shown %u, refogs %u, digest %016" PRIx64
                    "\n", land ? "land" : "space", a.fog_frames, a.fog_sources_max, a.fog_visible_max, a.fog_hidden, a.fog_shown, a.fog_refogs,
                    a.fog_hash);
        std::printf("  %s: human targets acquired %u (hidden %u), fog on / off CRC %s; %s at frame %u: probe fogged before %d, "
                    "after %d, Reveal_All frames %u\n",
                    land ? "land" : "space", a.fog_new_targets, a.fog_bad_targets,
                    first_divergence(a.crc, off.crc).diverged ? "diverge (human targeting)" : "identical", land ? "Force Sight" : "ping",
                    a.fog_ability_frame, a.fog_probe_before ? 1 : 0, a.fog_probe_after ? 1 : 0, a.fog_force_frames);
        CHECK(a.fought == 1 && a.fog_frames > 100 && a.fog_grids_ok && a.fog_ai_unfogged);
        CHECK(a.fog_sources_max > 0 && a.fog_visible_max > 0 && a.fog_hidden > 0 && a.fog_shown > 0);
        CHECK(off.fog_frames == 0);
        CHECK(a.fog_new_targets > 0 && a.fog_bad_targets == 0);
        CHECK(a.fog_ability_frame == 30 && a.fog_probe_before && a.fog_probe_after);
        if (land) CHECK(a.fog_force_on && a.fog_force_frames >= 59 && a.fog_force_frames <= 61);
        else CHECK(a.fog_force_frames == 0);
        CHECK(a.fog_hash == w4.fog_hash && !first_divergence(a.crc, w4.crc).diverged && a.battles == w4.battles);
        std::printf("  %s: OI fog shots %" PRIu64 " (shooter flashed on the target owner's grid %" PRIu64 "), host API %s\n",
                    land ? "land" : "space", a.fog_shots, a.fog_flashed, a.fog_api_ok ? "ok" : "FAILED");
        CHECK(a.fog_shots > 0 && a.fog_flashed > 0 && a.fog_api_ok && a.fog_api_checked);
    }
}

// FOWF t05: a land battle with a mod-style setup phase (SetupPhaseEnabled, SetupPhaseCountdownSeconds 3). During setup
// the human moves an own unit with SetupPhaseMove (31; an enemy unit is refused); SetupPhaseTriggerEnd (32) from the AI arms the 90-frame
// countdown, from the (only) human ends the phase at the next tick; afterwards 31 is refused.
struct SetupRun {
    bool setup_at_start = false, moved = false, enemy_refused = false, refused_after = false;
    int ready_cmd = -1;
    uint32_t ready_frame = 0, end_frame = 0;
};

SetupRun run_setup_battle(const Data& d, uint8_t ready_player, int32_t workers) {
    SetupRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, true));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.battle.fog = true;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    std::vector<std::string> diag;
    CHECK(battles.attach(&diag));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up.push_back(h);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    using R = commands::RejectReason;
    auto move_cmd = [](objects::ObjectId id, float x, float y, float angle) {
        commands::EventData ev(commands::EventType::SetupPhaseMove);
        ev.set("object", static_cast<uint32_t>(id)).set_f("x", x).set_f("y", y).set_f("z", 0.0f).set_f("angle", angle);
        return ev;
    };
    bool invaded = false, seen = false;
    for (uint64_t t = 0; t < 4000 && !out.refused_after; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) {
                CHECK(gs->invade(1, 1, {}) == R::None);
                invaded = true;
            }
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || !b->fog()) continue;
        const objects::ObjectStore& st = tm->primary();
        if (!seen) {
            seen = true;
            out.setup_at_start = b->fog_setup_phase();
        }
        if (b->frame() == 10 && b->fog_setup_phase()) {
            for (objects::ObjectHandle h : st.owned_by(1))
                if (st.active(h)) {
                    out.enemy_refused = host.execute_direct(0, commands::CommandSource::Player, move_cmd(st.id(h), 600.0f, 600.0f, 0.0f)) == R::InvalidUnit;
                    break;
                }
            for (objects::ObjectHandle h : st.owned_by(0)) {
                if (!st.active(h) || !b->combat().has_unit(st.id(h))) continue;
                const Vec3 p = st.position(h);
                const commands::EventData ev = move_cmd(st.id(h), p.x + 40.0f, p.y + 30.0f, 90.0f);
                const R r = host.execute_direct(0, commands::CommandSource::Player, ev);
                const Vec3 q = st.position(h), f = st.facing(h);
                out.moved = r == R::None && q.x == ev.f("x") && q.y == ev.f("y") && std::fabs(q.x - p.x - 40.0f) < 1.0f &&
                            std::fabs(f.x) < 1.0e-3f && std::fabs(f.y - 1.0f) < 1.0e-3f;
                break;
            }
            out.ready_frame = b->frame();
            out.ready_cmd = static_cast<int>(host.execute_direct(ready_player, commands::CommandSource::Player,
                                                                 commands::EventData(commands::EventType::SetupPhaseTriggerEnd)));
        }
        if (out.ready_frame && !out.end_frame && !b->fog_setup_phase()) {
            out.end_frame = b->frame();
            for (objects::ObjectHandle h : st.owned_by(0))
                if (st.active(h)) {
                    out.refused_after = host.execute_direct(0, commands::CommandSource::Player, move_cmd(st.id(h), 600.0f, 600.0f, 0.0f)) == R::NotAllowed;
                    break;
                }
            if (!out.refused_after) break;
        }
    }
    return out;
}

void test_fog_setup_battle(const Data& d) {
    const SetupRun ai = run_setup_battle(d, 1, 0), human = run_setup_battle(d, 0, 0), ai4 = run_setup_battle(d, 1, 4);
    for (const SetupRun* r : {&ai, &human})
        std::printf("  ready from %s: setup at start %d, own move %d, enemy refused %d, ready cmd %d at frame %u, "
                    "phase ended at frame %u, move refused after %d\n",
                    r == &ai ? "AI" : "human", r->setup_at_start ? 1 : 0, r->moved ? 1 : 0,
                    r->enemy_refused ? 1 : 0, r->ready_cmd, r->ready_frame, r->end_frame, r->refused_after ? 1 : 0);
    for (const SetupRun* r : {&ai, &human}) {
        CHECK(r->setup_at_start && r->moved && r->enemy_refused && r->refused_after);
        CHECK(r->ready_cmd == static_cast<int>(commands::RejectReason::None) && r->ready_frame == 10);
    }
    CHECK(ai.end_frame == ai.ready_frame + 90);       // 3 s x 30 fps after the first ready
    CHECK(human.end_frame == human.ready_frame + 1);  // every human ready: the next tick's end test
    CHECK(ai4.end_frame == ai.end_frame && ai4.moved && ai4.refused_after);
}

// FOW t01: Battlefield_Modifier feeds the fog reveal list (space battle, fog_bm data; see fog_bm_probe).
void test_fog_bm_battle(const Data& d) {
    const Run a = run_campaign(d, false, true, 0, 2500, 0.0f, true);
    std::printf("  bonus frames: Empire %u (then off %u), Underworld %u (then off %u); sight radius on %.1f / off %.1f; bad frames %u; "
                "grid %s, save %s\n",
                a.bm_on_frames[0], a.bm_off_after_on[0], a.bm_on_frames[1], a.bm_off_after_on[1], static_cast<double>(a.bm_r_on),
                static_cast<double>(a.bm_r_off), a.bm_bad_frames, a.bm_grid_ok ? "ok" : "FAILED", a.bm_save_ok ? "ok" : "FAILED");
    CHECK(a.fought == 1 && a.fog_frames > 100);
    CHECK(a.bm_on_frames[0] > 0 && a.bm_on_frames[1] > 0 && a.bm_bad_frames == 0);
    CHECK(a.bm_off_after_on[0] > 0 || a.bm_off_after_on[1] > 0);  // a side lost its last corvette: the bonus left with it
    CHECK(a.bm_r_on == 1350.0f && a.bm_r_off == 900.0f);
    CHECK(a.bm_saved && a.bm_grid_ok && a.bm_save_ok);
}

// S8 t08 (TACTICAL_COMBAT §3.1.1): the units with both weapon sources get their own weapon (WEAPON behaviour +
// Projectile_Types) next to their weapon hardpoints, with the Fire_Weapon_When_* / Fire_When_* flags of the table; a type
// with Projectile_Types but no WEAPON behaviour gets none. S8 t06 B4 (LOB_SOLVER §5.1): with model data the own weapon's
// fire bones MUZZLEA_NN come from the land model.
void test_foc_weapon_sources(const objects::GameObjectTypes& types, const BattleCatalog& catalog) {
    namespace tc = tactical::combat;
    struct Row {
        const char* unit;
        const char* own;  // own weapon projectile
        bool own_dep, own_undep, own_rocket, own_normal;
        const char* hp;  // weapon hardpoint projectile
        bool hp_dep, hp_undep, hp_rocket, hp_normal;
    };
    const Row rows[] = {
        {"SPMAT_Walker", "Proj_Ground_SPMAT_Grenade", true, false, true, true, "Proj_Blaster_Cannon_Green", false, true, true, true},
        {"T4B_Tank", "Proj_Ground_Laser_Cannon_Red_T4B", true, true, false, true, "Proj_T4B_Missile", true, true, true, false},
        {"Dark_Trooper_PhaseIII", "Proj_Ground_Laser_Cannon_Red_DT3", false, true, true, true, "Proj_Dark_Trooper_Missile", true, false, true,
         true},
        {"Destroyer_Droid", "Proj_Destroyer_Droid_Blaster_Cannon", true, false, false, true, "Proj_Destroyer_Droid_Ion_Cannon", true, false,
         true, false},
    };
    for (const Row& r : rows) {
        const objects::TypeIndex t = types.resolve_name(r.unit);
        CHECK(t != objects::kNone);
        if (t == objects::kNone) continue;
        std::vector<uint32_t> idx;
        const BattleTables bt = catalog.build_battle({static_cast<uint32_t>(t)}, idx, false);
        const tc::CombatData& cd = bt.combat;
        CHECK(cd.units.size() == 1);
        if (cd.units.size() != 1) continue;
        const tc::UnitDesc& u = cd.units[0];
        const bool own_ok = u.weapon_projectile >= 0 && cd.projectiles[static_cast<size_t>(u.weapon_projectile)].name == r.own &&
                            u.fire_weapon_when_deployed == r.own_dep && u.fire_weapon_when_undeployed == r.own_undep &&
                            u.fire_weapon_when_rocket_attack == r.own_rocket && u.fire_weapon_when_normal_attack == r.own_normal;
        uint32_t weapon_hps = 0, hp_ok = 0;
        for (const uint32_t h : u.hardpoints) {
            const tc::HardpointDesc& hd = cd.hardpoints[h];
            if (!tc::is_weapon(hd.kind)) continue;
            ++weapon_hps;
            if (hd.projectile >= 0 && cd.projectiles[static_cast<size_t>(hd.projectile)].name == r.hp && hd.fire_when_deployed == r.hp_dep &&
                hd.fire_when_undeployed == r.hp_undep && hd.fire_when_rocket_attack == r.hp_rocket &&
                hd.fire_when_normal_attack == r.hp_normal)
                ++hp_ok;
        }
        std::printf("foc weapon sources: %s own %s (%d), %u weapon hardpoints (%u as the table), muzzle model %d bones %zu\n", r.unit,
                    u.weapon_projectile >= 0 ? cd.projectiles[static_cast<size_t>(u.weapon_projectile)].name.c_str() : "-",
                    own_ok ? 1 : 0, weapon_hps, hp_ok, u.muzzle_model ? 1 : 0, u.muzzle_bones.size());
        CHECK(own_ok && weapon_hps > 0 && hp_ok == weapon_hps);
        if (u.muzzle_model) CHECK(!u.muzzle_bones.empty());  // the vanilla models carry MUZZLEA_00
    }
    {  // Projectile_Types without the WEAPON behaviour (and without weapon hardpoints): no weapon.
        uint32_t listed = 0, armed = 0;
        std::string example;
        for (uint32_t t = 0; t < types.size(); ++t) {
            if (types.get_list(t, "Projectile_Types").empty() ||
                types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Weapon) || !types.hardpoints(t).empty())
                continue;
            ++listed;
            std::vector<uint32_t> idx;
            const tc::CombatData cd = catalog.build({t}, idx);
            if (!cd.units.empty() && cd.units[0].weapon_projectile >= 0) ++armed;
            if (example.empty()) example = std::string(types.raw().types().name[t]);
        }
        std::printf("foc weapon sources: %u types list Projectile_Types without WEAPON or hardpoints (e.g. %s), %u armed\n", listed,
                    example.c_str(), armed);
        CHECK(listed > 0 && armed == 0);
    }
    {  // Fire bones over every land unit with an own weapon: how many models lack MUZZLEA_00 (those never fire). S8 t12 (a)
       // (LOB_SOLVER §5.1 "Muzzle bones on FoC land models"): WEAPON in Behavior or LandBehavior (the land set the battle host
       // reads) - every vanilla model has the bone. The 8 types the Union set used to list (Moldy_Crow, ..._Landing,
       // UM07_*) carry WEAPON in SpaceBehavior only: no land own weapon.
        uint32_t with_model = 0, no_bone = 0;
        std::string missing;
        for (uint32_t t = 0; t < types.size(); ++t) {
            if (types.get_list(t, "Projectile_Types").empty() ||
                !(types.has_behavior(t, objects::BehaviorSet::Generic, objects::beh::Weapon) ||
                  types.has_behavior(t, objects::BehaviorSet::Land, objects::beh::Weapon)))
                continue;
            std::vector<tactical::Vec3> bones;
            if (!catalog.muzzle_bones(t, false, bones)) continue;
            ++with_model;
            if (bones.empty()) {
                ++no_bone;
                if (missing.size() < 400) missing += std::string(missing.empty() ? "" : ", ") + std::string(types.raw().types().name[t]);
            }
        }
        std::printf("foc fire bones: %u land models of weapon users, %u without MUZZLEA_00: %s\n", with_model, no_bone, missing.c_str());
        CHECK(with_model > 0 && no_bone == 0);
    }
    {  // S8 t10 (LOB_SOLVER §5.1): the garrison gun bone table FP_0<i>_ALT<a> of every Num_Garrison_Guns type with a model.
        uint32_t hosts = 0, with_bones = 0;
        std::string line;
        for (uint32_t t = 0; t < types.size(); ++t) {
            const int32_t guns = types.get_int(t, "Num_Garrison_Guns");
            if (guns <= 0) continue;
            std::vector<std::vector<tc::GarrisonBone>> table;
            if (!catalog.garrison_bones(t, false, guns, table)) continue;
            ++hosts;
            size_t valid = 0;
            for (const auto& e : table)
                for (const tc::GarrisonBone& b : e) valid += b.valid ? 1u : 0u;
            with_bones += valid > 0 ? 1u : 0u;
            if (line.size() < 300)
                line += std::string(line.empty() ? "" : ", ") + std::string(types.raw().types().name[t]) + " " + std::to_string(guns) +
                        " guns x " + std::to_string(table.size()) + " ALT (" + std::to_string(valid) + " bones)";
        }
        std::printf("foc garrison guns: %u host models, %u with gun bones: %s\n", hosts, with_bones, line.c_str());
        CHECK(hosts == 0 || with_bones > 0);
    }
    {  // S8 t27 (TACTICAL_COMBAT §3.1.1 "Bone list"): Target_Bones found on the type's model (missing names dropped).
        uint32_t listed = 0, with_model = 0;
        std::string line;
        for (uint32_t t = 0; t < types.size(); ++t) {
            const auto names = types.get_list(t, "Target_Bones");
            if (names.empty()) continue;
            ++listed;
            const bool space = types.get_string(t, "Land_Model_Name").empty();  // land model when the type has one
            std::vector<tactical::Vec3> bones;
            if (!catalog.target_bones(t, space, bones)) continue;
            ++with_model;
            CHECK(bones.size() <= names.size());
            if (line.size() < 400)
                line += std::string(line.empty() ? "" : ", ") + std::string(types.raw().types().name[t]) + " " +
                        std::to_string(bones.size()) + "/" + std::to_string(names.size());
        }
        std::printf("foc target bones: %u types list Target_Bones, %u with a model: %s\n", listed, with_model, line.c_str());
        CHECK(listed >= 5);
        const objects::TypeIndex atat = types.resolve_name("AT_AT_Walker");
        std::vector<tactical::Vec3> b;
        if (atat != objects::kNone && catalog.target_bones(static_cast<uint32_t>(atat), false, b)) {
            CHECK(b.size() == 3);  // MISSILE_TARGET00-02 on the land model
            for (const tactical::Vec3& v : b) CHECK(v.z > 0.0f);
        }
    }
    {  // s78n t20 batch 2 (FIGHTER_FLYING §7.4 item 1): Garrison_Bone_Names resolved on the land model, host-local frame
        uint32_t listed = 0, resolved = 0;
        std::string line;
        for (uint32_t t = 0; t < types.size(); ++t) {
            const auto names = types.get_list(t, "Garrison_Bone_Names");
            if (names.empty()) continue;
            ++listed;
            std::vector<tactical::Vec3> bones;
            CHECK(catalog.entry_bones(t, false, bones));
            resolved += bones.size() == names.size() ? 1u : 0u;
            line += std::string(line.empty() ? "" : "; ") + std::string(types.raw().types().name[t]) + ":";
            for (const tactical::Vec3& v : bones) {
                char buf[64];
                std::snprintf(buf, sizeof buf, " (%.1f %.1f %.1f)", static_cast<double>(v.x), static_cast<double>(v.y),
                              static_cast<double>(v.z));
                line += buf;
            }
        }
        std::printf("foc garrison entry bones: %u types list them, %u resolve all: %s\n", listed, resolved, line.c_str());
        // 12 types list them (5 own lists, the rest inherited by variants); only Gargantuan_Battle_Platform_Death_Clone's
        // model lacks the bone (dropped)
        CHECK(listed >= 5 && resolved + 1 >= listed);
        const auto bones_of = [&](const char* name, std::vector<tactical::Vec3>& b) {
            const objects::TypeIndex t = types.resolve_name(name);
            return t != objects::kNone && catalog.entry_bones(static_cast<uint32_t>(t), false, b);
        };
        std::vector<tactical::Vec3> b;
        // HTT (Scale_Factor 0.3): one bone behind the hull on the centre line (the model's +Y tail -> local -x), scaled
        if (bones_of("Gallofree_HTT_Transport", b)) {
            CHECK(b.size() == 1);
            if (b.size() == 1) CHECK(b[0].x < -50.0f && b[0].x > -120.0f && std::fabs(b[0].y) < 1.0f && std::fabs(b[0].z) < 5.0f);
        }
        if (bones_of("HAV_Juggernaut", b)) {  // one bone at the tail
            CHECK(b.size() == 1);
            if (b.size() == 1) CHECK(b[0].x < -50.0f && std::fabs(b[0].y) < 1.0f);
        }
        // the bunker: four bones on the ground, one per side, 30 - 40 from the centre, opposite pairs about the centre
        if (bones_of("Garrison_Bunker_Template", b)) {
            CHECK(b.size() == 4);
            for (const tactical::Vec3& v : b) {
                const float r = std::sqrt(v.x * v.x + v.y * v.y);
                CHECK(r > 30.0f && r < 40.0f && std::fabs(v.z) < 0.5f && std::min(std::fabs(v.x), std::fabs(v.y)) < 1.0f);
            }
            if (b.size() == 4) CHECK(std::fabs(b[0].y + b[2].y) < 3.0f && std::fabs(b[1].x + b[3].x) < 3.0f);
        }
    }
}

// FoC: a battle request built by hand (two FoC frigate groups at Kuat-like orbit) on a real space map.
// a1g t04 follow-up (TACTICAL_GAPS §4.2.1): the land company drop layout, the oriented-box test and the radius rules.
// The worked example: FoC Rebel_Infantry_Squad (15 x Squad_Rebel_Trooper, Max_Squad_Size 5, team container locomotor,
// trooper half extents 3 x 3, FormationSpacing 0.5).
LandDropShape rebel_squad_shape() {
    LandDropShape s;
    s.members.assign(15, 7u);
    s.hx.assign(15, 3.0f);
    s.hy.assign(15, 3.0f);
    s.create_team = true;
    s.squad = 5;
    s.container_locomotor = true;
    s.formation_spacing = 0.5f;
    return s;
}

bool rebel_squad_slots_ok(const LandDropLayout& lay, float px, float py, float yaw_deg) {
    static const float kSlots[15][2] = {{35.5f, -16.0f}, {35.5f, 0.0f},   {35.5f, 16.0f},  {19.5f, -8.0f}, {19.5f, 8.0f},
                                        {8.0f, -16.0f},  {8.0f, 0.0f},    {8.0f, 16.0f},   {-8.0f, -8.0f}, {-8.0f, 8.0f},
                                        {-35.5f, 16.0f}, {-35.5f, 0.0f},  {-35.5f, -16.0f}, {-19.5f, 8.0f}, {-19.5f, -8.0f}};
    if (!lay.ok || lay.slots.size() != 15) return false;
    if (std::fabs(lay.half_x - 41.25f) > 1e-3f || std::fabs(lay.half_y - 42.0f) > 1e-2f) return false;
    const bool quarter = yaw_deg == 90.0f;
    for (size_t i = 0; i < 15; ++i) {
        const float ox = quarter ? -kSlots[i][1] : kSlots[i][0], oy = quarter ? kSlots[i][0] : kSlots[i][1];  // (x, y) -> (-y, x)
        if (std::fabs(lay.slots[i].x - (px + ox)) > 1e-3f || std::fabs(lay.slots[i].y - (py + oy)) > 1e-3f) {
            std::printf("  slot %zu: (%.4f, %.4f) expected (%.4f, %.4f)\n", i, static_cast<double>(lay.slots[i].x),
                        static_cast<double>(lay.slots[i].y), static_cast<double>(px + ox), static_cast<double>(py + oy));
            return false;
        }
    }
    return true;
}

void test_land_drop_rules() {
    // Layout: yaw 0 and the 90-degree rotation, at a drop point away from the origin.
    const LandDropShape rebel = rebel_squad_shape();
    CHECK(rebel_squad_slots_ok(land_drop_layout(rebel, 1000.0f, 500.0f, 0.0f), 1000.0f, 500.0f, 0.0f));
    CHECK(rebel_squad_slots_ok(land_drop_layout(rebel, 1000.0f, 500.0f, 90.0f), 1000.0f, 500.0f, 90.0f));
    {
        const LandDropLayout lay = land_drop_layout(rebel, 0.0f, 0.0f, 0.0f);
        CHECK(lay.ok && lay.slots[0].type == 7u && lay.slots[0].hx == 3.0f);
        CHECK(std::fabs(lay.slots[6].yaw_deg) < 1e-3f);  // (8, 0): the slot box's axis is the yaw of drop point -> slot
        CHECK(std::fabs(lay.slots[11].yaw_deg - 180.0f) < 1e-3f);
    }
    {  // without the team container locomotor the team uses 0x40f970; a partial last team keeps its first slots
        LandDropShape s = rebel;
        s.container_locomotor = false;
        s.members.resize(12);
        s.hx.resize(12);
        s.hy.resize(12);
        const LandDropLayout lay = land_drop_layout(s, 0.0f, 0.0f, 0.0f);
        CHECK(lay.ok && lay.slots.size() == 12);
    }
    {  // vehicles: one 0x40f970 grid. Cell (2 * 10 / 20 + 0, same) = (1, 1), 4 members: side = sqrt(7.2), 2 per row.
        LandDropShape v;
        v.members.assign(4, 3u);
        v.hx.assign(4, 10.0f);
        v.hy.assign(4, 10.0f);
        const LandDropLayout lay = land_drop_layout(v, 0.0f, 0.0f, 0.0f);
        CHECK(lay.ok && lay.slots.size() == 4);
        if (lay.ok && lay.slots.size() == 4) {
            const float side = std::sqrt(7.2f), gap = (side - 2.0f) / 3.0f;
            CHECK(std::fabs(lay.slots[0].x - 10.0f) < 1e-3f && std::fabs(lay.slots[2].x + 10.0f) < 1e-3f);
            CHECK(std::fabs(lay.slots[0].y - (gap - side / 2.0f + 0.5f) * 20.0f) < 1e-3f);
            CHECK(std::fabs(lay.slots[1].y + lay.slots[0].y) < 1e-3f);  // centred row
            CHECK(std::fabs(lay.half_x - 20.0f) < 1e-3f && std::fabs(lay.half_y - side * 10.0f) < 1e-3f);
        }
        v.hy.assign(4, 0.0f);  // b = 2 hy / 20 + FS = 0 -> no layout
        CHECK(!land_drop_layout(v, 0.0f, 0.0f, 0.0f).ok);
        CHECK(!land_drop_layout(LandDropShape{}, 0.0f, 0.0f, 0.0f).ok);
    }
    // Oriented boxes.
    CHECK((drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 0.0f}, {5.0f, 0.0f, 3.0f, 3.0f, 0.0f})));
    CHECK((!drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 0.0f}, {6.0f, 0.0f, 3.0f, 3.0f, 0.0f})));  // touching
    CHECK((!drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 0.0f}, {7.5f, 0.0f, 3.0f, 3.0f, 0.0f})));
    CHECK((!drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 45.0f}, {9.0f, 0.0f, 3.0f, 3.0f, 45.0f})));
    CHECK((drop_boxes_overlap({0.0f, 0.0f, 3.0f, 3.0f, 0.0f}, {6.5f, 0.0f, 3.0f, 3.0f, 45.0f})));  // the corner reaches 2.26
    CHECK((drop_boxes_overlap({0.0f, 0.0f, 20.0f, 1.0f, 90.0f}, {0.0f, 15.0f, 1.0f, 1.0f, 0.0f})));   // long axis along y
    CHECK((!drop_boxes_overlap({0.0f, 0.0f, 20.0f, 1.0f, 0.0f}, {0.0f, 15.0f, 1.0f, 1.0f, 0.0f})));
    // Radii: a POWERED enemy object with prevention radius 100, 60 from a slot, blocks only while its owner has base
    // power; without POWERED it always blocks. Enabling (<=) needs an allied object; prevention is strict (<).
    LandDropRadiusObject turret;
    turret.x = 60.0f;
    turret.prevention = 100.0f;
    turret.powered = true;
    turret.owner_base_power = true;
    LandDropRadiusObject point;
    point.allied = true;
    point.enabling = 500.0f;
    std::vector<LandDropRadiusObject> objs = {point, turret};
    CHECK(!land_drop_radius_ok(0.0f, 0.0f, objs, true));
    objs[1].owner_base_power = false;  // the owner's base-power list is empty
    CHECK(land_drop_radius_ok(0.0f, 0.0f, objs, true));
    objs[1].powered = false;
    CHECK(!land_drop_radius_ok(0.0f, 0.0f, objs, true));
    CHECK(land_drop_radius_ok(-40.0f, 0.0f, objs, true));  // d = 100: not < 100
    objs[1].allied = true;                                  // an allied object never prevents
    CHECK(land_drop_radius_ok(0.0f, 0.0f, objs, true));
    CHECK(land_drop_radius_ok(500.0f, 0.0f, objs, true));   // d = 500 <= 500
    CHECK(!land_drop_radius_ok(500.5f, 0.0f, objs, true));  // outside every enabling radius
    CHECK(land_drop_radius_ok(500.5f, 0.0f, objs, false));  // without Use_Reinforcement_Points
    {  // a1i t04: the unload offsets use 3.0 x the team bounds (the validity 2.5); team members keep their offsets in-team.
        const LandDropLayout v = land_drop_layout(rebel, 0.0f, 0.0f, 0.0f), u = land_drop_layout(rebel, 0.0f, 0.0f, 0.0f, 3.0f);
        CHECK(v.ok && u.ok && v.slots.size() == u.slots.size());
        bool wider = false;
        for (size_t i = 0; v.ok && u.ok && i < v.slots.size(); ++i) {
            const float dv = std::sqrt(v.slots[i].x * v.slots[i].x + v.slots[i].y * v.slots[i].y);
            const float du = std::sqrt(u.slots[i].x * u.slots[i].x + u.slots[i].y * u.slots[i].y);
            wider = wider || du > dv + 1.0f;
        }
        CHECK(wider && u.half_x >= v.half_x && u.half_y >= v.half_y);
        LandDropShape veh;  // vehicles: one member grid, no team factor
        veh.members.assign(4, 3u);
        veh.hx.assign(4, 10.0f);
        veh.hy.assign(4, 10.0f);
        const LandDropLayout a = land_drop_layout(veh, 5.0f, 5.0f, 30.0f), b = land_drop_layout(veh, 5.0f, 5.0f, 30.0f, 3.0f);
        CHECK(a.ok && b.ok && a.slots.size() == b.slots.size());
        for (size_t i = 0; a.ok && b.ok && i < a.slots.size(); ++i) CHECK(a.slots[i].x == b.slots[i].x && a.slots[i].y == b.slots[i].y);
    }
    {  // a1i t05: Find_Free_Placement rings (box 20 x 10 -> step 24, 22.5-degree angles, 360 inclusive).
        const FreePlacementBox box{-10.0f, -5.0f, 10.0f, 5.0f};
        float qx = 0.0f, qy = 0.0f;
        CHECK(find_free_placement(100.0f, 50.0f, box, {}, 2500.0f, qx, qy) && qx == 100.0f && qy == 50.0f);
        std::vector<FreePlacementBox> bl = {{90.0f, 45.0f, 110.0f, 55.0f}};  // covers P
        CHECK(find_free_placement(100.0f, 50.0f, box, bl, 2500.0f, qx, qy));
        CHECK(std::fabs(qx - 124.0f) < 1e-3f && std::fabs(qy - 50.0f) < 1e-3f);  // ring 1, angle 0
        bl.push_back({113.0f, 45.0f, 140.0f, 52.0f});                         // angle 0 of ring 1 taken too
        CHECK(find_free_placement(100.0f, 50.0f, box, bl, 2500.0f, qx, qy));
        CHECK(std::fabs(qx - (100.0f + 24.0f * std::cos(0.39269908f))) < 1e-2f && std::fabs(qy - (50.0f + 24.0f * std::sin(0.39269908f))) < 1e-2f);
        const std::vector<FreePlacementBox> touch = {{110.0f, 45.0f, 130.0f, 55.0f}};  // touches the box at P: free
        CHECK(find_free_placement(100.0f, 50.0f, box, touch, 2500.0f, qx, qy) && qx == 100.0f);
        const std::vector<FreePlacementBox> all = {{-5000.0f, -5000.0f, 5000.0f, 5000.0f}};
        CHECK(!find_free_placement(100.0f, 50.0f, box, all, 2500.0f, qx, qy));
        CHECK(!find_free_placement(0.0f, 0.0f, FreePlacementBox{}, all, 2500.0f, qx, qy));  // zero step: r = 0 only
        // q6 (OBJECT_SCRIPTS §10.4): Spawn_Unit's start angle (yaw - 45) and the land cell test.
        bl.resize(1);
        CHECK(find_free_placement(100.0f, 50.0f, box, bl, 2500.0f, qx, qy, 90.0f));  // ring 1 starts at 90 degrees
        CHECK(std::fabs(qx - 100.0f) < 1e-2f && std::fabs(qy - 74.0f) < 1e-2f);
        auto east_only = [](float x, float) { return x > 130.0f; };  // ring 1 (x <= 124) refused: ring 2, angle 0
        CHECK(find_free_placement(100.0f, 50.0f, box, {}, 2500.0f, qx, qy, 0.0f, east_only));
        CHECK(std::fabs(qx - 148.0f) < 1e-3f && std::fabs(qy - 50.0f) < 1e-3f);
    }
    std::printf("  land drop rules: OK\n");
}

// s78n t26.1 review 5 (TACTICAL_COMBAT §3.1.1, MOVEMENT §3.2.1 "Who is an auto-deployer"): Auto_Deploys defaults to true,
// so every shipped Deploys type but the AT-AT family and AT-AA is an auto-deployer. The Empire_Mobile_Defense_Unit (no
// Auto_Deploys tag; Fire_Weapon_When_Undeployed No) is one, but it has neither the WEAPON nor the TURRET behaviour, so it
// never holds an attack target and the combat auto-deploy check 0x568e60 never runs for it (it deploys by its DEPLOY
// ability). The check's path is driven with Destroyer_Droid (WEAPON, no TURRET): ordered onto a target in range it requests
// the deploy (0x391260), on a human player's auto-acquired target it does not, on an AI player's it does.
struct DeployHooks final : tactical::combat::CombatHooks {
    uint32_t requests = 0;
    void request_deploy(tactical::combat::ObjectId, uint32_t) override { ++requests; }
};

void test_foc_auto_deploy(const objects::GameObjectTypes& types, const BattleCatalog& catalog) {
    namespace tc = tactical::combat;
    const uint32_t mdu = static_cast<uint32_t>(types.resolve_name("Empire_Mobile_Defense_Unit"));
    const uint32_t droid = static_cast<uint32_t>(types.resolve_name("Destroyer_Droid"));
    const uint32_t foe = static_cast<uint32_t>(types.resolve_name("Rebel_Mobile_Defense_Unit"));
    CHECK(mdu < types.size() && droid < types.size() && foe < types.size());
    if (mdu >= types.size() || droid >= types.size() || foe >= types.size()) return;
    std::vector<uint32_t> idx;
    const BattleTables bt = catalog.build_battle({mdu, droid, foe}, idx, false);
    CHECK(idx.size() == 3);
    if (idx.size() != 3) return;
    const tc::UnitDesc& md = bt.combat.units[idx[0]];
    const tc::UnitDesc& dd = bt.combat.units[idx[1]];
    std::printf("foc MDU: deploys %d auto_deploys %d fire undeployed %d, weapon %d turret %d\n", md.deploys ? 1 : 0,
                md.auto_deploys ? 1 : 0, md.fire_weapon_when_undeployed ? 1 : 0, md.weapon_projectile, md.turret ? 1 : 0);
    CHECK(md.deploys && md.auto_deploys && !md.fire_weapon_when_undeployed && md.has_locomotor);
    CHECK(md.weapon_projectile < 0 && !md.turret);
    CHECK(dd.deploys && dd.auto_deploys && !dd.fire_weapon_when_undeployed && dd.weapon_projectile >= 0 && !dd.turret);
    const auto run = [&](bool ai, bool ordered) {
        tc::CombatConfig cfg;
        cfg.mode = tc::GameMode::Land;
        cfg.opportunity_fire = false;
        std::vector<tc::PlayerInfo> players(2);
        players[0].ai = ai;
        tc::CombatSystem cs(bt.combat, cfg, players);
        DeployHooks hooks;
        cs.set_hooks(&hooks);
        tactical::TreeSet trees(2);
        SimRng rng(26);
        const float d = std::max(40.0f, std::min(150.0f, 0.5f * dd.max_attack_distance));
        const auto add = [&](tc::ObjectId id, uint32_t desc, int32_t owner, tactical::Vec3 pos, tactical::Vec3 face) {
            tc::UnitSpawn s;
            s.id = id;
            s.desc = desc;
            s.owner = owner;
            s.position = pos;
            s.facing = face;
            CHECK(cs.add_unit(s, 1, rng));
            tactical::TreeMembership m;
            m.owner = owner;
            const float r = std::max(1.0f, bt.combat.units[desc].radius);
            CHECK(trees.add(static_cast<tactical::EntityHandle>(id), {{pos.x - r, pos.y - r, pos.z - r}, {pos.x + r, pos.y + r, pos.z + r}},
                            1u, m));
        };
        add(1, idx[1], 0, {0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f});
        add(2, idx[2], 1, {d, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f});
        if (ordered) cs.order_attack(1, 2);
        for (uint32_t f = 1; f < 90; ++f) cs.tick(f, trees, rng, serial_for);
        std::printf("  Destroyer_Droid ai %d ordered %d: targeting state %d, deploy requests %u\n", ai ? 1 : 0, ordered ? 1 : 0,
                    static_cast<int>(cs.targeting_state(1)), hooks.requests);
        return hooks.requests;
    };
    const uint32_t ordered = run(false, true), human_auto = run(false, false), ai_auto = run(true, false);
    std::printf("foc Destroyer_Droid auto-deploy requests: ordered %u, human auto target %u, AI auto target %u\n", ordered,
                human_auto, ai_auto);
    CHECK(ordered > 0 && human_auto == 0 && ai_auto > 0);
}

void test_foc() {
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("foc battle: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    vfs::VfsConfig vc;
    vc.foc_root = root;
    std::string err;
    auto files = vfs::Vfs::create(vc, &err);
    CHECK(files != nullptr);
    if (!files) return;
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::ConflictRules crules;
    std::vector<std::string> diag;
    CHECK(db.load(*files, {}, &diag) && raw.load(*files) && types.build(raw, files.get(), &diag));
    CHECK(galactic::load_galactic_rules(*files, db, types, rules, &diag));
    CHECK(galactic::load_conflict_rules(*files, db, types, rules, crules, &diag));
    const BattleCatalog catalog(types, files.get(), &db.constants(), &diag);
    std::printf("foc battle: contrast rows %zu, hardpoint records %zu, priority sets %zu, damage types %u\n", crules.contrast.size(),
                catalog.hardpoint_records(), catalog.priority_sets().size(), catalog.damage().damage_count());
    CHECK(crules.contrast.size() == 11 && catalog.hardpoint_records() > 100 && !catalog.priority_sets().empty());
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    {  // a2h t05: the hosts' loader on the shipped data - the tags are found (not the defaults' fallback) and read
        TacticalAiSetup ts;
        const vfs::Vfs* vp = files.get();
        load_tactical_ai_data(ts, *files, &db.constants(), [vp](std::string_view g) { return vp->list(g); });
        const data::GameConstants& gc = db.constants();
        CHECK(gc.get("Production_Speed_Factor") && gc.get("Max_Ground_Forces_On_Planet") && gc.get("Use_Reinforcement_Points") &&
              gc.get("Space_Reinforcement_Collision_Check_Distance"));
        CHECK(ts.constants.production_speed_factor == gc.get_float("Production_Speed_Factor", -1.0f));
        CHECK(ts.constants.max_ground_forces_on_planet == gc.get_int("Max_Ground_Forces_On_Planet", -1));
        CHECK(ts.constants.space_reinforcement_collision_check_distance ==
              gc.get_float("Space_Reinforcement_Collision_Check_Distance", -1.0f));
        CHECK(!ts.space_plans.empty() && !ts.land_plans.empty() && !ts.difficulties.empty());
        std::printf("foc tactical AI constants: production_speed_factor %.3f, max_ground_forces %d, space drop sweep %.0f, "
                    "%zu / %zu space / land plans, %zu difficulties\n",
                    static_cast<double>(ts.constants.production_speed_factor), ts.constants.max_ground_forces_on_planet,
                    static_cast<double>(ts.constants.space_reinforcement_collision_check_distance), ts.space_plans.size(),
                    ts.land_plans.size(), ts.difficulties.size());
    }
#endif
    {  // a1g t04 follow-up (TACTICAL_GAPS §4.2.1 worked example): the Rebel_Infantry_Squad drop shape and layout
        const objects::TypeIndex rq = types.resolve_name("Rebel_Infantry_Squad");
        CHECK(rq != objects::kNone && rq < types.size());
        if (rq != objects::kNone && rq < types.size()) {
            const LandDropShape s = land_drop_shape(types, &catalog, crules, static_cast<uint32_t>(rq));
            std::printf("foc drop shape Rebel_Infantry_Squad: %zu members, hx %.3f hy %.3f, team %d squad %u container %d fs %.2f\n",
                        s.members.size(), s.hx.empty() ? 0.0 : static_cast<double>(s.hx[0]), s.hy.empty() ? 0.0 : static_cast<double>(s.hy[0]),
                        s.create_team ? 1 : 0, s.squad, s.container_locomotor ? 1 : 0, static_cast<double>(s.formation_spacing));
            CHECK(s.members.size() == 15 && s.create_team && s.squad == 5 && s.container_locomotor && s.formation_spacing == 0.5f);
            CHECK(!s.hx.empty() && std::fabs(s.hx[0] - 3.0f) < 1e-4f && std::fabs(s.hy[0] - 3.0f) < 1e-4f);
            CHECK(rebel_squad_slots_ok(land_drop_layout(s, 250.0f, -125.0f, 0.0f), 250.0f, -125.0f, 0.0f));
            CHECK(rebel_squad_slots_ok(land_drop_layout(s, 250.0f, -125.0f, 90.0f), 250.0f, -125.0f, 90.0f));
        }
    }
    {  // refixf t01: rocket curve fields reach the battle tables (MPTL fires Proj_Ground_Proton_Torpedo: MPTL_Rocket,
       // straight 100, curve 50, offset 7, Target_Point_On_Terrain, Explode_When_Reached_Target_Radius)
        std::vector<uint32_t> idx;
        const BattleTables bt = catalog.build_battle({static_cast<uint32_t>(types.resolve_name("MPTL"))}, idx);
        const tactical::combat::ProjectileDesc* torp = nullptr;
        for (const tactical::combat::ProjectileDesc& pd : bt.combat.projectiles)
            if (pd.name == "Proj_Ground_Proton_Torpedo") torp = &pd;
        CHECK(torp != nullptr);
        if (torp) {
            std::printf("foc rocket: %s cat %d straight %.0f curve %.0f offset %.0f tpot %d explode %d mfd %.0f\n", torp->name.c_str(),
                        static_cast<int>(torp->category), static_cast<double>(torp->rocket_straight_distance),
                        static_cast<double>(torp->rocket_curve_distance), static_cast<double>(torp->rocket_curve_offset),
                        torp->target_point_on_terrain ? 1 : 0, torp->explode_when_reached_target_radius ? 1 : 0,
                        static_cast<double>(torp->max_flight_distance));
            CHECK(torp->category == tactical::combat::ProjectileCategory::MptlRocket && torp->rocket_straight_distance == 100.0f &&
                  torp->rocket_curve_distance == 50.0f && torp->rocket_curve_offset == 7.0f && torp->target_point_on_terrain &&
                  torp->explode_when_reached_target_radius && torp->max_flight_distance == 720.0f);
        }
    }
    {  // S8 t04 (LOB_SOLVER §1.2): Underworld_Merc fires Proj_Merc_Concussion_Grenade through Projectile_Types - GRENADE,
       // v 10, gravity 0.7 x lob mod 0.8 (G 0.56, R = v^2 / G = 178.6), Can_Lob_Slower, not sticky, lifetime 900.
        std::vector<uint32_t> idx;
        const BattleTables bt = catalog.build_battle({static_cast<uint32_t>(types.resolve_name("Underworld_Merc"))}, idx);
        const tactical::combat::ProjectileDesc* g = nullptr;
        for (const tactical::combat::ProjectileDesc& pd : bt.combat.projectiles)
            if (pd.name == "Proj_Merc_Concussion_Grenade") g = &pd;
        CHECK(g != nullptr && bt.combat.units.size() == 1 && bt.combat.units[0].weapon_projectile >= 0 &&
              &bt.combat.projectiles[static_cast<size_t>(bt.combat.units[0].weapon_projectile)] == g);
        if (g) {
            std::printf("foc grenade: %s cat %d v %.1f gravity %.9g lob mod %.3g slower %d sticky %d lifetime %.0f\n", g->name.c_str(),
                        static_cast<int>(g->category), static_cast<double>(g->max_speed), static_cast<double>(g->grenade_gravity),
                        static_cast<double>(g->lob_mod), g->can_lob_slower ? 1 : 0, g->sticks_on_collision ? 1 : 0,
                        static_cast<double>(g->max_lifetime));
            CHECK(g->lobbed && g->category == tactical::combat::ProjectileCategory::Grenade && g->max_speed == 10.0f &&
                  g->grenade_gravity == 0.7f && g->lob_mod == 0.8f && g->can_lob_slower && !g->sticks_on_collision &&
                  g->max_lifetime == 900.0f);
            const float r = tactical::combat::lob_range(g->max_speed, g->grenade_gravity, g->lob_mod);
            CHECK(r > 178.5f && r < 178.6f);
        }
    }
    test_foc_weapon_sources(types, catalog);  // S8 t08 / S8 t06 B4
    test_foc_auto_deploy(types, catalog);  // s78n t26.1 review 5
    {  // --- S4 (owner: S4 agent): shipped corruption options (PLANETS.XML) and garrison / capture descriptors ---
        const std::vector<galactic::CorruptionChoiceDef>& ae = rules.special.type(types.resolve_name("AetenII")).corruption;
        size_t missions = 0, options = 0;
        for (size_t t = 0; t < types.size(); ++t)
            for (const galactic::CorruptionChoiceDef& o : rules.special.type(static_cast<uint32_t>(t)).corruption) {
                ++options;
                missions += o.tactical ? 1u : 0u;
            }
        std::printf("foc corruption: %zu options, %zu with a tactical mission; AetenII %zu options\n", options, missions, ae.size());
        CHECK(ae.size() == 2 && options > 20 && missions > 5);
        if (ae.size() == 2) {
            const objects::TypeIndex bossk = types.resolve_name("Bossk");
            std::printf("  AetenII 1: type %d tactical %d space %d cost %d map '%s' plots '%s' units %zu x%d\n", ae[0].type,
                        ae[0].tactical ? 1 : 0, ae[0].tactical_space ? 1 : 0, ae[0].credit_cost_rounded, ae[0].tactical_map.c_str(),
                        ae[0].tactical_plots.c_str(), ae[0].required_unit_types.size(), ae[0].required_unit_count);
            CHECK(ae[0].type == 1 && ae[0].tactical && ae[0].tactical_space && ae[0].credit_cost_rounded == 1000 &&
                  ae[0].tactical_map == "_Space_Planet_AetenII_Piracy.ted" &&
                  ae[0].tactical_plots == "Story_Plots_Underworld_AetenII_Piracy.xml" && bossk != objects::kNone &&
                  ae[0].required_unit_types == std::vector<uint32_t>{bossk} && ae[0].required_unit_count == 1);
            CHECK(ae[1].type == 3 && !ae[1].tactical && ae[1].required_mission == 0 && ae[1].required_mission_count == 1 &&
                  ae[1].required_unit_count == 2);
        }
        tactical::garrison::HolderDesc hd;
        tactical::garrison::OccupantDesc od;
        tactical::garrison::CapturePointDesc cd;
        CHECK(tactical::garrison::holder_desc_from_type(types, types.resolve_name("HAV_Juggernaut"), hd) && hd.slots == 10 &&
              hd.radius == 30.0f && hd.kind == tactical::garrison::HolderKind::Vehicle);
        CHECK(tactical::garrison::holder_desc_from_type(types, types.resolve_name("Garrison_Bunker_Template"), hd) &&
              hd.kind == tactical::garrison::HolderKind::Structure);
        CHECK(tactical::garrison::occupant_desc_from_type(types, types.resolve_name("Dark_Trooper_PhaseIII"), od) && od.value == 2);
        CHECK(tactical::garrison::capture_desc_from_type(types, types.resolve_name("Reinforcement_Point"), cd) && cd.radius == 100.0f &&
              cd.transition_seconds == 10.0f && cd.sticky);
    }  // --- end S4 ---
    {  // S7 t07 (MOVEMENT §2.2): the FoC construction / capture pods have no radius tag; with model data their footprint is
       // the space model's collision-bound half extent x Scale_Factor (N_Orbital_Construction_Pod: x 1.75)
        namespace mv = tactical::movement;
        mv::SpaceObstacleHooks hooks;
        hooks.model_half_extents = [&catalog](uint32_t t, float& hx, float& hy) { return catalog.space_model_half_extents(t, hx, hy); };
        for (const char* name : {"Orbital_Construction_Pod", "N_Orbital_Construction_Pod", "Skirmish_Merchant_Dock"}) {
            const uint32_t t = static_cast<uint32_t>(types.resolve_name(name));
            CHECK(t < types.size());
            if (t >= types.size()) continue;
            float r = 0.0f;
            uint32_t bits = 0;
            CHECK(!mv::space_obstacle_footprint(types, t, r, bits));  // no radius tag: needs the model
            float hx = 0.0f, hy = 0.0f;
            if (!catalog.space_model_half_extents(t, hx, hy)) {
                std::printf("foc space obstacle: %s: no model data (sim built without assets), skipped\n", name);
                continue;
            }
            const float scale = types.get_float(t, "Scale_Factor") > 0.0f ? types.get_float(t, "Scale_Factor") : 1.0f;
            CHECK(mv::space_obstacle_footprint(types, t, r, bits, &hooks) && bits == mv::kSpaceStatic);
            CHECK(r == std::max(std::fabs(hx), std::fabs(hy)) * scale && r > 10.0f);
            std::printf("foc space obstacle: %s half %.1f x %.1f, scale %.2f -> radius %.1f\n", name, static_cast<double>(hx),
                        static_cast<double>(hy), static_cast<double>(scale), static_cast<double>(r));
        }
    }
    {  // s78n t08f (TACTICAL_COMBAT §5.4.5 item 2): the slot 43 / 46 model extents of Nebulon_B_Frigate (no Custom_Hard
       // extents, Scale_Factor 0.7, RV_NEBULONB.ALO collision-bound half (33.54, 157.07, 95.52)): X (along the facing) =
       // 0.7 x half y = 109.95, Y (across) = 0.7 x half x = 23.48.
        const uint32_t neb = static_cast<uint32_t>(types.resolve_name("Nebulon_B_Frigate"));
        CHECK(neb < types.size());
        float hx = 0.0f, hy = 0.0f;
        if (neb < types.size() && catalog.space_model_half_extents(neb, hx, hy)) {
            std::vector<uint32_t> idx;
            const BattleTables bt = catalog.build_battle({neb}, idx, true);
            CHECK(idx.size() == 1);
            if (idx.size() == 1) {
                const tactical::combat::UnitDesc& u = bt.combat.units[idx[0]];
                std::printf("foc range extents: Nebulon_B_Frigate half %.2f x %.2f -> X %.2f, Y %.2f\n", static_cast<double>(hx),
                            static_cast<double>(hy), static_cast<double>(u.model_x_extent), static_cast<double>(u.model_y_extent));
                CHECK(u.hard_x_extent == 0.0f && u.hard_y_extent == 0.0f);
                CHECK(std::fabs(u.model_x_extent - 109.95f) < 0.01f && std::fabs(u.model_y_extent - 23.48f) < 0.01f);
            }
        } else {
            std::printf("foc range extents: Nebulon_B_Frigate: no model data (sim built without assets), skipped\n");
        }
    }
    {  // s78n t11 (TACTICAL_COMBAT §5.4.6 item 6): the _Deployed range extents of the two FoC types that set one
       // (Custom_Hard_XExtent 30, _YExtent 15, _YExtent_Deployed 24, no _XExtent_Deployed): MPTL x Scale_Factor 1.1,
       // MAL_Rocket_Vehicle x 0.7. Type data only (no model read).
        struct Dep {
            const char* name;
            float x, y, dep_y;
        };
        for (const Dep c : {Dep{"MPTL", 33.0f, 16.5f, 26.4f}, Dep{"MAL_Rocket_Vehicle", 21.0f, 10.5f, 16.8f}}) {
            const uint32_t t = static_cast<uint32_t>(types.resolve_name(c.name));
            CHECK(t < types.size());
            if (t >= types.size()) continue;
            std::vector<uint32_t> idx;
            const BattleTables bt = catalog.build_battle({t}, idx, false);
            CHECK(idx.size() == 1);
            if (idx.size() != 1) continue;
            const tactical::combat::UnitDesc& u = bt.combat.units[idx[0]];
            std::printf("foc deployed range extents: %s X %.2f Y %.2f, deployed X %.2f Y %.2f, deploys %d\n", c.name,
                        static_cast<double>(u.hard_x_extent), static_cast<double>(u.hard_y_extent),
                        static_cast<double>(u.hard_x_extent_deployed), static_cast<double>(u.hard_y_extent_deployed),
                        u.deploys ? 1 : 0);
            CHECK(std::fabs(u.hard_x_extent - c.x) < 0.001f && std::fabs(u.hard_y_extent - c.y) < 0.001f);
            CHECK(u.hard_x_extent_deployed == 0.0f && std::fabs(u.hard_y_extent_deployed - c.dep_y) < 0.001f);
            CHECK(u.deploys && u.has_locomotor);
        }
    }
    {  // s78n t10 (TACTICAL_COMBAT §5.4.6 items 1, 2, 6): the type half-extents 0x371be0 with the land model
       // (drop_half_extents) - both customs: x Scale_Factor; no Land_Model_Name: (0, 0), no fallback to the space model;
       // no customs: Scale_Factor x the land model's collision-bound half x / y. 0x371d20 ignores a deployed Y alone.
        const auto type = [&types](const char* name) { return static_cast<uint32_t>(types.resolve_name(name)); };
        const uint32_t mptl = type("MPTL"), trooper = type("Stormtrooper"), neb = type("Nebulon_B_Frigate");
        CHECK(mptl < types.size() && trooper < types.size() && neb < types.size());
        float hx = 0.0f, hy = 0.0f;
        if (mptl < types.size()) {
            drop_half_extents(types, &catalog, mptl, hx, hy);
            CHECK(std::fabs(hx - 33.0f) < 0.001f && std::fabs(hy - 16.5f) < 0.001f);
            drop_deployed_extents(types, &catalog, mptl, hx, hy);
            CHECK(std::fabs(hx - 33.0f) < 0.001f && std::fabs(hy - 16.5f) < 0.001f);
        }
        if (trooper < types.size()) {
            drop_half_extents(types, &catalog, trooper, hx, hy);
            CHECK(std::fabs(hx - 3.0f) < 0.001f && std::fabs(hy - 3.0f) < 0.001f);
        }
        if (neb < types.size()) {
            CHECK(ascii_trim(types.get_string(neb, "Land_Model_Name")).empty());
            drop_half_extents(types, &catalog, neb, hx, hy);
            CHECK(hx == 0.0f && hy == 0.0f);
            // the same rule with the space model (the clearance / composition measure): 0.7 x (33.54, 157.07)
            float mx = 0.0f, my = 0.0f;
            if (catalog.space_model_half_extents(neb, mx, my)) {
                const tactical::movement::V2 h =
                    tactical::movement::clearance_half_extents(0.0f, 0.0f, types.columns().scale_factor[neb], true, mx, my);
                CHECK(std::fabs(h.x - 23.48f) < 0.01f && std::fabs(h.y - 109.95f) < 0.01f);
            }
        }
        uint32_t plain = static_cast<uint32_t>(types.size());
        for (uint32_t t = 0; t < types.size() && plain >= types.size(); ++t) {
            if (types.get_float(t, "Custom_Hard_XExtent") > 0.0f || types.get_float(t, "Custom_Hard_YExtent") > 0.0f) continue;
            if (ascii_trim(types.get_string(t, "Land_Model_Name")).empty() || !catalog.avoid_model(t, false)) continue;
            plain = t;
        }
        if (plain < types.size()) {
            const tactical::movement::AvoidModel* am = catalog.avoid_model(plain, false);
            const float sf = types.get_float(plain, "Scale_Factor");
            const float sc = sf > 0.0f ? sf : 1.0f;
            drop_half_extents(types, &catalog, plain, hx, hy);
            std::printf("foc drop extents: %s (no Custom_Hard extents) land bound half %.2f x %.2f, scale %.2f -> %.2f x %.2f\n",
                        std::string(types.raw().types().name[plain]).c_str(), static_cast<double>(am->bound_h.x),
                        static_cast<double>(am->bound_h.y), static_cast<double>(sc), static_cast<double>(hx), static_cast<double>(hy));
            CHECK(hx == std::fabs(am->bound_h.x) * sc && hy == std::fabs(am->bound_h.y) * sc && hx > 0.0f && hy > 0.0f);
        } else {
            std::printf("foc drop extents: no land model data (sim built without assets), skipped\n");
        }
    }
    // Fog of war on real data (owner: fog agent): shipped GameConstants; grids, dense mask and REVEAL placements of
    // two real maps.
    const tactical::fog::FogConstants& fc = catalog.fog_constants();
    std::printf("foc fog: cells %.1f / %.1f, regrow %.1f / %.1f s, AI uses fog %d / %d, overrun x%.1f after %.0f s\n",
                static_cast<double>(fc.desired_land_cell_size), static_cast<double>(fc.desired_space_cell_size),
                static_cast<double>(fc.land_regrow_time), static_cast<double>(fc.space_regrow_time), fc.ai_uses_fog_land ? 1 : 0,
                fc.ai_uses_fog_space ? 1 : 0, static_cast<double>(fc.tactical_overrun_multiple), static_cast<double>(fc.min_overrun_seconds));
    CHECK(fc.desired_land_cell_size == 15.0f && fc.desired_space_cell_size == 100.0f && fc.land_regrow_time == 6.0f &&
          fc.space_regrow_time == 6.0f && !fc.ai_uses_fog_land && !fc.ai_uses_fog_space && fc.tactical_overrun_multiple == 3.0f &&
          fc.min_overrun_seconds == 30.0f && !fc.setup_phase_enabled);
    const TypeCrcIndex index(types);
    const objects::BehaviorTypeId reveal = objects::behavior_type_id("REVEAL"), obstacle = objects::behavior_type_id("SPACE_OBSTACLE");
    for (const char* name : {"_SPACE_PLANET_CORELLIA_PIRACY", "_LAND_PLANET_BYSS_KIDNAPPING"}) {
        terrain::MapData map;
        std::string map_err;
        if (!terrain::load_map(*files, name, map, &map_err)) {
            std::printf("FAIL foc fog: map %s: %s\n", name, map_err.c_str());
            ++g_fail;
            continue;
        }
        float mn[2] = {-5000.0f, -5000.0f}, mx[2] = {5000.0f, 5000.0f};
        map.playable_box(mn, mx);
        tactical::fog::FogSetup s;
        s.mode = map.is_space() ? tactical::fog::FogMode::Space : tactical::fog::FogMode::Land;
        s.box = {mn[0], mn[1], mx[0], mx[1]};
        s.constants = fc;
        s.players = {{0, true, true}, {1, true, false}, {2, false, false}};
        tactical::fog::FogOfWar fog;
        fog.start(s);
        std::vector<tactical::fog::FogOfWar::DenseCircle> dense;
        uint32_t sources = 0;
        int32_t id = 1;
        for (const terrain::ObjectPlacement& pl : map.objects) {
            const objects::TypeIndex t = index.find(pl.type_crc);
            if (t == objects::kNone) continue;
            if (obstacle != objects::kNoBehavior && types.has_behavior(t, objects::BehaviorSet::Union, obstacle) &&
                (types.get_bool(t, "Is_Nebula") || types.get_bool(t, "Is_Asteroid_Field") || types.get_bool(t, "Is_Ion_Storm") ||
                 types.get_bool(t, "Is_Impassable_Asteroid"))) {
                const auto off = types.get_vec(t, "Space_Obstacle_Offset");
                dense.push_back({pl.position[0] + off[0], pl.position[1] + off[1], types.get_float(t, "Space_Obstacle_Radius")});
            }
            if (reveal == objects::kNoBehavior || !types.has_behavior(t, objects::BehaviorSet::Union, reveal)) continue;
            tactical::fog::FogObject o;
            o.owner = pl.owner == 0 ? 0 : 1;
            o.x = pl.position[0];
            o.y = pl.position[1];
            o.sight_range = map.is_space() ? types.columns().space_fow_reveal_range[t] : types.columns().land_fow_reveal_range[t];
            o.dense_multiplier = std::clamp(types.get_float(t, "Dense_FOW_Reveal_Range_Multiplier"), 0.0f, 1.0f);
            fog.source_service(id++, o);
            ++sources;
        }
        fog.set_dense_obstacles(dense);
        for (uint32_t f = 1; f <= 64; ++f) fog.decay_step(f);
        const tactical::fog::FogGrid* g = fog.grid(0);
        uint32_t vis = 0, dense_cells = 0;
        if (g) {
            for (uint8_t v : g->state()) vis += v != 0 ? 1u : 0u;
            for (uint8_t m : fog.dense_mask()) dense_cells += m;
        }
        std::printf("foc fog %s: grid %dx%d (cell %.2f), %u REVEAL placements, %zu dense obstacles (%u cells), player 0 sees %u cells\n",
                    name, g ? g->width() : 0, g ? g->height() : 0, g ? static_cast<double>(g->cell_w()) : 0.0, sources, dense.size(), dense_cells, vis);
        CHECK(g != nullptr && g->width() > 0 && g->width() <= 512 && g->height() > 0 && !fog.grid(2));
    }
}

#ifdef ALAMO_SIM_HAVE_ANIM
// ---- R3 anim: flinch / death clips / corpses in synthetic battles (SimAnimation with synthetic clip models) ----

// One-bone skeleton, clips without tracks (bound: ALA bones 0 + 1): only frame counts / fps matter here.
std::unique_ptr<anim::AnimModel> anim_model(std::initializer_list<std::pair<int32_t, std::pair<uint32_t, uint32_t>>> specs) {
    auto m = std::make_unique<anim::AnimModel>();
    m->model = "S10_SYNTH.ALO";
    m->base = "S10_SYNTH";
    m->skeleton.names = {"Root"};
    m->skeleton.parents = {-1};
    m->skeleton.billboard_mode = {0u};
    m->skeleton.bind_local = {anim::Mat34{}};
    m->has_skeleton = true;
    for (const auto& [type, vf] : specs)
        for (uint32_t v = 0; v < vf.first; ++v) {
            auto c = std::make_shared<anim::Clip>();
            c->name = anim::clip_file_name(m->base, type, v);
            c->frame_count = vf.second;
            c->fps = 30.0f;
            anim::ClipRef r;
            r.clip = c;
            m->clips[static_cast<size_t>(type)].push_back(r);
        }
    anim::bind_clips(*m);
    return m;
}

struct AnimRun {
    CrcLog crc;
    SimAnimStats stats;
    uint32_t max_detached = 0, max_corpses = 0, flinch_seen = 0, muzzles_seen = 0, hp_muzzles_seen = 0, foot_events = 0, impact_events = 0;
    uint64_t events_digest = 0;  // per-tick events (id, name, bone, position bits)
    std::vector<std::string> battles;
    // refix t01 / t03: death clone objects (+0x3a0 |= 0xc0) seen in the tactical store, battle clone count, searches
    uint32_t max_dying = 0, clone_objects = 0, dying_found = 0;
    bool corpse_path_trooper = false, corpse_path_trooper_k4 = true, corpse_path_frigate = true;
};

AnimRun run_anim_campaign(const Data& d, bool land, int32_t workers, uint64_t ticks) {
    AnimRun out;
    const auto trooper = anim_model({{anim::kAnimIdle, {2, 30}}, {anim::kAnimMove, {1, 24}}, {anim::kAnimWalkMove, {1, 24}},
                                     {anim::kAnimAttack, {1, 12}}, {anim::kAnimAttackIdle, {1, 20}}, {anim::kAnimDie, {2, 45}},
                                     {anim::kAnimFlinchLeft, {1, 9}}, {anim::kAnimFlinchRight, {1, 9}}, {anim::kAnimFlinchFront, {2, 9}},
                                     {anim::kAnimFlinchBack, {1, 9}}, {anim::kAnimAttackFlinchLeft, {1, 9}}, {anim::kAnimAttackFlinchRight, {1, 9}},
                                     {anim::kAnimAttackFlinchFront, {1, 9}}, {anim::kAnimAttackFlinchBack, {1, 9}},
                                     {anim::kAnimTurnLeft, {1, 10}}, {anim::kAnimTurnRight, {1, 10}}});
    const auto ship = anim_model({{anim::kAnimIdle, {1, 60}}});
    const auto wreck = anim_model({{anim::kAnimDie, {1, 30}}});
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    SimAnimation animation(d.types, *d.files);
    animation.set_model(d.types.resolve_name("S10_Trooper"), objects::ModeKind::Land, trooper.get());
    animation.set_model(d.types.resolve_name("S10_Frigate"), objects::ModeKind::Space, ship.get());
    animation.set_model(d.types.resolve_name("S10_Corvette"), objects::ModeKind::Space, ship.get());
    animation.set_model(d.types.resolve_name("S10_Frigate_Death_Clone"), objects::ModeKind::Space, wreck.get());
    {  // AnimationSFXMaps.txt rows for the synthetic clips (foot plants on the move clip, a body impact on DIE)
        anim::AnimEventMap map;
        map.parse("SURFACE\ts10_synth_move_00\t0\tINFANTRY_FOOT_PLANT\tB_Foot_R\nSURFACE\ts10_synth_move_00\t400\tINFANTRY_FOOT_PLANT\tB_Foot_L\n"
                  "SURFACE\ts10_synth_die_00\t500\tTROOPER_IMPACT\tRoot\nSOUND\ts10_synth_attack_00\t0\tUnit_Fire\n");
        animation.set_event_map(std::move(map));
    }
    animation.attach(host, true);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, land));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    opt.animation = &animation;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name == "S10_Space_B") m = space_map();
                                else if (name == "S10_Land_B") m = land_map();
                                else return false;
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    if (land) {
        std::vector<objects::ObjectHandle> up;
        for (objects::ObjectHandle h : gs->planets()[2].surface)
            if (gs->store().owner(h) == 1) up.push_back(h);
        CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    }
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false;
    StateHasher ev_hash;
    for (uint64_t t = 0; t < ticks; ++t) {
        host.tick_once();
        if (const TacticalBattle* b = battles.battle()) {
            out.clone_objects = std::max(out.clone_objects, b->stats().death_clones);
            const uint32_t trooper_type = type_of(d, "S10_Trooper");
            out.corpse_path_trooper = b->death_clone_corpse_path(trooper_type, static_cast<int32_t>(trooper_type), 1);
            out.corpse_path_trooper_k4 = b->death_clone_corpse_path(trooper_type, static_cast<int32_t>(trooper_type), 4);
            out.corpse_path_frigate =
                b->death_clone_corpse_path(type_of(d, "S10_Frigate"), static_cast<int32_t>(type_of(d, "S10_Frigate_Death_Clone")), 1);
        }
        if (TacticalMode* tm = host.tactical()) {
            {  // death clones: in the store (find by id, owner), dying, never in the active searches
                const objects::ObjectStore& st = tm->primary();
                uint32_t dying = 0;
                for (objects::ObjectHandle h : st.all_objects()) {
                    if (!st.alive(h) || !st.dying(h)) continue;  // (a clone the anim removed is dead until the flush)
                    ++dying;
                    CHECK(!st.active(h) && st.find(st.id(h)) == h);
                    for (objects::ObjectHandle o : st.find_all_of_type(st.type(h))) out.dying_found += o == h ? 1u : 0u;
                }
                out.max_dying = std::max(out.max_dying, dying);
            }
            out.muzzles_seen += static_cast<uint32_t>(animation.muzzles(*tm).size());
            for (const SimAnimation::Muzzle& mz : animation.muzzles(*tm)) out.hp_muzzles_seen += mz.hardpoint >= 0 ? 1u : 0u;
            for (const SimAnimation::FrameEvent& e : animation.events(*tm)) {
                out.foot_events += e.def->name == "INFANTRY_FOOT_PLANT" ? 1u : 0u;
                out.impact_events += e.def->name == "TROOPER_IMPACT" ? 1u : 0u;
                ev_hash.u64(t);
                ev_hash.i32(e.id);
                ev_hash.str(e.def->name);
                ev_hash.i32(e.bone);
                ev_hash.f32(e.position.x);
                ev_hash.f32(e.position.y);
                ev_hash.f32(e.position.z);
            }
            uint32_t detached = 0;
            for (const anim::Corpse& c : animation.corpses(*tm)) detached += c.detached ? 1u : 0u;
            out.max_detached = std::max(out.max_detached, detached);
            out.max_corpses = std::max(out.max_corpses, static_cast<uint32_t>(animation.corpses(*tm).size()));
            if (const anim::AnimWorld* w = animation.find_world(*tm))
                for (ObjectId id : w->ids()) out.flinch_seen += anim::is_flinch(w->current_type(id)) ? 1u : 0u;
        }
        if (land && !invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) {
                CHECK(gs->invade(1, 1, {}) == commands::RejectReason::None);
                invaded = true;
            }
        }
    }
    out.events_digest = ev_hash.digest();
    out.crc = host.crc_log();
    out.battles = battles.log();
    out.stats = animation.stats();
    animation.detach(host);
    return out;
}

// s78k t05 (TACTICAL_COMBAT §3.1.1 "Slot 47", "Order of the tests" [H]; s78j t02): the attack-idle pose hook tests the
// current clip before its Get_Float draw. On a land trooper of a battle with an animation world (the synthetic trooper
// model has clips 5 Attack and 6 Attack Idle), not moving: from Idle the hook draws once (blend in [0.2, 0.75]) and plays
// clip 6; a second service while clip 6 plays draws nothing and keeps it; while clip 5 Attack plays it draws nothing
// either and keeps clip 5. The hook runs with a private generator (TacticalBattle::test_attack_idle), so only its own
// draws are counted.
void test_attack_idle_clip_order() {
    auto d = make_data(false, true);
    const auto trooper = anim_model({{anim::kAnimIdle, {2, 30}}, {anim::kAnimMove, {1, 24}}, {anim::kAnimWalkMove, {1, 24}},
                                     {anim::kAnimAttack, {1, 12}}, {anim::kAnimAttackIdle, {1, 20}}, {anim::kAnimDie, {2, 45}}});
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    SimAnimation animation(d->types, *d->files);
    const objects::TypeIndex trooper_t = d->types.resolve_name("S10_Trooper");
    animation.set_model(trooper_t, objects::ModeKind::Land, trooper.get());
    animation.attach(host, true);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.animation = &animation;
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
    bool invaded = false, ran = false;
    for (uint64_t t = 0; t < 3000 && !ran; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() < 3) continue;
        const anim::AnimWorld* w = animation.find_world(*tm);
        CHECK(w != nullptr);
        if (!w) break;
        const tc::CombatSystem& cs = b->combat();
        const objects::ObjectStore& st = tm->primary();
        // A live, standing trooper with an animation row whose clip can be set (not locked).
        objects::ObjectId id = objects::kNoObjectId;
        for (size_t i = 0; i < cs.unit_count() && id == objects::kNoObjectId; ++i) {
            const auto u = cs.unit_id_at(i);
            if (!cs.alive(u) || (cs.flags(u) & tc::kUnitMoving) || st.type(st.find(u)) != trooper_t) continue;
            if (b->test_play_clip(u, anim::kAnimIdle)) id = u;
        }
        if (id == objects::kNoObjectId) continue;  // every trooper walking this tick: try the next one
        ran = true;
        CHECK(w->current_type(id) == anim::kAnimIdle);
        SimRng r1(77);
        CHECK(b->test_attack_idle(id, false, r1));
        CHECK(r1.draws() == 1u);  // Idle: one Get_Float(0.2, 0.75) for the blend
        CHECK(w->current_type(id) == anim::kAnimAttackIdle);
        SimRng r2(77);
        CHECK(b->test_attack_idle(id, false, r2));
        CHECK(r2.draws() == 0u);  // clip 6 plays: the clip test returns before the draw
        CHECK(w->current_type(id) == anim::kAnimAttackIdle);
        CHECK(b->test_play_clip(id, anim::kAnimAttack));
        SimRng r3(77);
        CHECK(b->test_attack_idle(id, false, r3));
        CHECK(r3.draws() == 0u);  // clip 5 plays: no draw, clip 5 kept
        CHECK(w->current_type(id) == anim::kAnimAttack);
        std::printf("  slot 47 on trooper %d (frame %u): idle 1 draw, attack idle %llu, attack %llu\n", static_cast<int>(id), b->frame(),
                    static_cast<unsigned long long>(r2.draws()), static_cast<unsigned long long>(r3.draws()));
    }
    CHECK(ran);
    animation.detach(host);
}

// s78l t03 (TACTICAL_COMBAT slot 47 "s78l t03"): IMPOSING_PRESENCE writes idle records (presence trooper id -> frames)
// into the IDLE pack of the other troopers in range, on the interval-30 schedule; the default idle type then picks
// Celebrate (Presence_Induced_Animations) for an idle trooper holding a record; records hold only live-at-write presence
// ids and never exceed int(30 x Unit_Presence_Relaxation_Time) frames; the IDLE attach set every trooper's first fidget
// frame (210 - 450 frames after its row creation).
void test_presence_idle() {
    auto d = make_data(false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       true);
    const auto trooper = anim_model({{anim::kAnimIdle, {2, 30}}, {anim::kAnimMove, {1, 24}}, {anim::kAnimWalkMove, {1, 24}},
                                     {anim::kAnimAttack, {1, 12}}, {anim::kAnimAttackIdle, {1, 20}}, {anim::kAnimDie, {2, 45}},
                                     {78, {1, 20}}});  // 78 = Celebrate
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    SimAnimation animation(d->types, *d->files);
    const objects::TypeIndex trooper_t = d->types.resolve_name("S10_Trooper");
    animation.set_model(trooper_t, objects::ModeKind::Land, trooper.get());
    animation.attach(host, true);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 900;
    opt.animation = &animation;
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
    const int32_t max_frames = static_cast<int32_t>(30.0f * opt.battle.presence_relaxation_seconds);
    bool invaded = false;
    uint32_t frames_with_records = 0, max_records = 0, celebrate_frames = 0, bad_records = 0, timers_checked = 0, bad_timers = 0;
    std::map<objects::ObjectId, uint64_t> first_seen;
    for (uint64_t t = 0; t < 4000; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (frames_with_records > 0) break;
            continue;
        }
        const anim::AnimWorld* w = animation.find_world(*tm);
        const anim::AnimBehaviors* beh = animation.find_behaviors(*tm);
        if (!w || !beh) continue;
        const objects::ObjectStore& st = tm->primary();
        uint32_t with = 0;
        for (const anim::ObjectId id : w->ids()) {
            const objects::ObjectHandle h = st.find(id);
            // Live troopers only: the dying type's own death clone (same type, IDLE stripped) gets a row without the draw.
            if (!st.alive(h) || st.dying(h) || st.type(h) != trooper_t) continue;
            if (first_seen.emplace(id, host.frame()).second) {  // the row's first frame: the IDLE attach timer
                const uint64_t it = w->idle_timer(id);
                ++timers_checked;
                if (it < host.frame() + 210 - 1 || it > host.frame() + 450) ++bad_timers;
            }
            const auto recs = beh->idle_records(id);
            if (!recs.empty()) ++with;
            // [port] a presence unit may hold a record of itself (its type lists its own type; retail's box query
            // includes it [M]), so self records are allowed.
            for (const anim::IdleRecord& r : recs)
                if (r.frames < 1 || r.frames > max_frames) ++bad_records;
            if (w->current_type(id) == 78) ++celebrate_frames;
        }
        if (with > 0) ++frames_with_records;
        max_records = std::max(max_records, with);
    }
    std::printf("  presence: %u frames with records (max %u troopers), %u Celebrate trooper-frames, %u bad records; %u attach timers (%u bad)\n",
                frames_with_records, max_records, celebrate_frames, bad_records, timers_checked, bad_timers);
    CHECK(frames_with_records > 0 && celebrate_frames > 0 && bad_records == 0);
    CHECK(timers_checked > 0 && bad_timers == 0);
    animation.detach(host);
}

void test_anim_battles() {
    auto d = make_data(false, true);
    for (const bool land : {true, false}) {
        const uint64_t ticks = land ? 3000 : 2500;
        const AnimRun a = run_anim_campaign(*d, land, 0, ticks);
        for (const std::string& s : a.battles) std::printf("  %s\n", s.c_str());
        const SimAnimStats& s = a.stats;
        StateHasher h;
        for (const CrcRecord& r : a.crc.records) h.u64(r.combined);
        std::printf("  %s + anim: rows created %u removed %u; flinches %u (flinch-frames %u), deaths %u (death clips %u, removed at death %u, "
                    "clone deaths %u), corpses removed %u, max corpses %u (detached %u); CRC log digest %016" PRIx64 "\n",
                    land ? "land" : "space", s.created, s.removed, s.flinches, a.flinch_seen, s.deaths, s.death_clips, s.removed_at_death,
                    s.clone_deaths, s.corpses_removed, a.max_corpses, a.max_detached, h.digest());
        std::printf("  %s + anim: turns %u, muzzle-frames %u (flashes %u), events %llu (foot plants %u, impacts %u), digest %016" PRIx64 "\n",
                    land ? "land" : "space", s.turns, a.muzzles_seen, s.muzzle_flashes, static_cast<unsigned long long>(s.events), a.foot_events,
                    a.impact_events, a.events_digest);
        std::printf("  %s + anim: death clone objects %u, max dying in the store %u\n", land ? "land" : "space", a.clone_objects, a.max_dying);
        CHECK(a.dying_found == 0);  // find_all_of_type skips dying objects
        bool has_anim_system = false;
        for (const std::string& n : a.crc.systems) has_anim_system = has_anim_system || n == "anim";
        CHECK(has_anim_system && !a.battles.empty());
        CHECK(s.deaths > 0);
        if (land) {
            // Troopers flinch on blaster hits, play DIE, persist 1 s + fade 0.5 s as detached corpse rows, then go.
            CHECK(s.flinches > 0 && a.flinch_seen > 0 && s.death_clips > 0 && a.max_detached > 0 && s.corpses_removed > 0);
            // Foot plants from the moving troopers' MOVE clip, impacts from the DIE clip (detached corpses too),
            // muzzle flashes from their blasters.
            CHECK(a.foot_events > 0 && a.impact_events > 0 && a.muzzles_seen > 0 && s.muzzle_flashes > 0);
            // refix t03: S10_Trooper (Infantry, no HIDE_WHEN_FOGGED) clones itself on the corpse path: no clone object,
            // the dying row plays the clone's death (clone_deaths); key 4 (Force lightning) and ships keep the object.
            CHECK(a.corpse_path_trooper && !a.corpse_path_trooper_k4 && !a.corpse_path_frigate);
            CHECK(s.clone_deaths > 0);
        } else {
            // Frigates / corvettes: the death clone plays DIE and is removed (store object destroyed) after 2 s + 1 s.
            // (refix t01: the star base's own-type clone - no model, Remove_Upon_Death: removed at once - is made on the
            // battle's last tick, which ends the battle before the anim sync starts clone deaths: removed_at_death 0.)
            CHECK(s.clone_deaths > 0 && s.corpses_removed > 0);
            // refix t01: the clones are store objects flagged dying / removing until their death sequence removes them.
            CHECK(a.clone_objects > 0 && a.max_dying > 0);
            // Ships fire from hardpoints: their muzzle records carry the HardPoints index.
            CHECK(a.hp_muzzles_seen > 0);
        }
        for (int32_t w : jobs::determinism_worker_counts()) {
            if (w == 0) continue;
            const AnimRun b = run_anim_campaign(*d, land, w, ticks);
            const auto dv = first_divergence(a.crc, b.crc);
            std::printf("  %s + anim workers %2d: %s\n", land ? "land" : "space", w, dv.message.c_str());
            CHECK(!dv.diverged && b.battles == a.battles && b.stats.flinches == s.flinches && b.stats.corpses_removed == s.corpses_removed &&
                  b.events_digest == a.events_digest && b.muzzles_seen == a.muzzles_seen);
        }
    }
}

// ---- refixf t04: death clone removal, headless and with the animation service ----
// The space campaign on the anim data: frigate / corvette clones (S10_Frigate_Death_Clone: 2 s persistence + 1 s fade, no
// Remove_Upon_Death) must leave the store at the planned frame; the star base's typed clone (30 s) is made on the battle's
// last tick and must not outlive the battle. `anim`: the SimAnimation service is attached (no "anim" CRC system, so the
// CRC logs compare); otherwise the same SimAnimation is only the battle's clone-model source. `wreck`: the clone type gets
// a 30-frame DIE clip (both paths), so the plan includes the clip length.
struct CloneRun {
    CrcLog crc;
    std::vector<std::string> battles;
    std::map<ObjectId, std::pair<uint64_t, uint64_t>> life;  // clone id -> (frame first alive, frame first gone; 0 = never)
    std::map<ObjectId, uint32_t> type;
    uint64_t end_frame = 0;      // frame of the battle's end (object_removed callback)
    uint32_t alive_at_end = ~0u;  // dying objects still alive in the store at the battle's end
    uint32_t rows_missing = 0, detached_corpses = 0;  // anim: alive clone without its row; corpse whose store object left
    SimAnimStats stats;
};

CloneRun run_clone_campaign(const Data& d, bool anim_service, bool wreck, int32_t workers, uint64_t ticks) {
    CloneRun out;
    const auto wreck_model = anim_model({{anim::kAnimDie, {1, 30}}});
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    SimAnimation animation(d.types, *d.files);
    if (wreck) animation.set_model(d.types.resolve_name("S10_Frigate_Death_Clone"), objects::ModeKind::Space, wreck_model.get());
    if (anim_service) animation.attach(host, false);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    if (anim_service) opt.animation = &animation;
    else opt.clone_models = &animation;
    opt.object_removed = [&host, &out](int32_t, uint32_t, uint32_t, bool) {  // after the battle's final tick, before the pop
        TacticalMode* tm = host.tactical();
        if (!tm || out.end_frame == host.frame()) return;
        out.end_frame = host.frame();
        out.alive_at_end = 0;
        const objects::ObjectStore& st = tm->primary();
        for (objects::ObjectHandle h : st.all_objects()) {  // dead-but-not-flushed too: clones made and removed on this tick
            if (!st.dying(h)) continue;
            out.alive_at_end += st.alive(h) ? 1u : 0u;
            const ObjectId id = st.id(h);
            if (out.life.emplace(id, std::make_pair(out.end_frame, out.end_frame)).second) out.type[id] = static_cast<uint32_t>(st.type(h));
        }
    };
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
    for (uint64_t t = 0; t < ticks; ++t) {
        host.tick_once();
        const uint64_t f = host.frame();
        TacticalMode* tm = host.tactical();
        std::vector<ObjectId> present;
        if (tm) {
            const objects::ObjectStore& st = tm->primary();
            const anim::AnimWorld* w = anim_service ? animation.find_world(*tm) : nullptr;
            for (objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h) || !st.dying(h)) continue;
                const ObjectId id = st.id(h);
                present.push_back(id);
                if (out.life.emplace(id, std::make_pair(f, uint64_t{0})).second) out.type[id] = static_cast<uint32_t>(st.type(h));
                if (w && wreck && out.type[id] == type_of(d, "S10_Frigate_Death_Clone") && !w->contains(id)) ++out.rows_missing;
            }
            if (anim_service)
                for (const anim::Corpse& c : animation.corpses(*tm)) out.detached_corpses += c.detached ? 1u : 0u;
        }
        for (auto& [id, l] : out.life)
            if (l.second == 0 && std::find(present.begin(), present.end(), id) == present.end()) l.second = f;
    }
    out.crc = host.crc_log();
    out.battles = battles.log();
    out.stats = animation.stats();
    if (anim_service) animation.detach(host);
    return out;
}

void test_death_clone_removal() {
    auto d = make_data(false, true);
    const uint32_t frigate_clone = type_of(*d, "S10_Frigate_Death_Clone"), base_clone = type_of(*d, "S10_Star_Base_Death_Clone");
    for (const bool wreck : {false, true}) {
        const CloneRun h = run_clone_campaign(*d, false, wreck, 0, 2500);
        // Planned lifetime: no clip 60 + 30 frames; with the 30-frame DIE clip (finished 28 advances after its start, seen
        // by the corpse service one frame later) 28 + 1 + 60 + 30.
        const uint64_t expect = wreck ? 119 : 90;
        uint32_t removed = 0, other = 0, base_clones = 0;
        uint64_t base_made = 0;
        for (const auto& [id, l] : h.life) {
            const uint32_t t = h.type.at(id);
            if (t == frigate_clone && l.second != 0 && l.second < h.end_frame) {
                ++removed;
                if (l.second - l.first != expect) ++other;
            }
            if (t == base_clone) {
                ++base_clones;
                base_made = l.first;
            }
        }
        for (const std::string& s : h.battles) std::printf("  %s\n", s.c_str());
        std::printf("  death clones %s: %zu clone objects, %u frigate clones removed after %llu frames (%u other), star base clone made "
                    "at %llu, battle end %llu, dying alive at the end %u\n",
                    wreck ? "(DIE clip)" : "(no clip)", h.life.size(), removed, static_cast<unsigned long long>(expect), other,
                    static_cast<unsigned long long>(base_made), static_cast<unsigned long long>(h.end_frame), h.alive_at_end);
        CHECK(removed > 0 && other == 0);              // headless: removed at the planned frame (were never removed before)
        CHECK(base_clones == 1 && base_made + 210 == h.end_frame);  // made at the declaration (story t06: 210 frames to the end) ...
        CHECK(h.alive_at_end == 0);                    // ... and gone with the battle, like every clone left
        for (const auto& [id, l] : h.life) CHECK(l.second != 0);  // no clone object after the battle
        // The animation service changes nothing in the sim: same CRC log, same clone lifetimes; its corpse rows follow
        // the battle's removals frame for frame (no clone without its row, no detached clone corpse).
        const CloneRun a = run_clone_campaign(*d, true, wreck, 0, 2500);
        const auto dv = first_divergence(h.crc, a.crc);
        std::printf("  death clones + anim service: %s; clone deaths %u, corpses removed %u, rows missing %u, detached %u\n",
                    dv.message.c_str(), a.stats.clone_deaths, a.stats.corpses_removed, a.rows_missing, a.detached_corpses);
        CHECK(!dv.diverged && a.battles == h.battles && a.life == h.life && a.end_frame == h.end_frame && a.alive_at_end == 0);
        CHECK(a.stats.clone_deaths > 0 && a.rows_missing == 0);
        if (wreck) CHECK(a.detached_corpses == 0 && a.stats.corpses_removed >= removed);
        // Worker counts: identical.
        const CloneRun w4 = run_clone_campaign(*d, false, wreck, 4, 2500);
        CHECK(!first_divergence(h.crc, w4.crc).diverged && w4.life == h.life);
    }
}
#endif

// --- P4 tactical: campaign Black Market gate (ABILITIES_HARDPOINTS 2.4, P4_SIM_SYSTEMS 1.5) -------------------------
// The space campaign on the bm data: at battle frame 10 the gate state of every frigate / corvette's POWER_TO_WEAPONS
// is read and the human Empire sends command 43 on it. Without a purchase both players are gated (no activation);
// with S10_BM_Item given to the Empire before the start only the Underworld stays gated. CRC log identical for every
// worker count and equal to testdata/battle_bm_golden.crc (MSVC writes it with --regen; clang must reproduce it).
struct BmRun {
    CrcLog crc;
    std::vector<std::string> battles;
    uint32_t allowed[2] = {0, 0}, gated[2] = {0, 0}, active_after = 0;
    int command = -1;
};

BmRun run_bm_campaign(const Data& d, bool bought, int32_t workers, uint64_t ticks) {
    BmRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    if (bought) CHECK(gs->bm_give_item(0, "S10_BM_Item"));
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 6000;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    std::vector<std::string> diag;
    CHECK(battles.attach(&diag));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    constexpr uint8_t kP2W = 29;
    for (uint64_t t = 0; t < ticks; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        if (!b || out.command >= 0 || b->frame() != 10) continue;
        TacticalMode* tm = host.tactical();
        std::vector<uint32_t> ids;
        std::vector<objects::ObjectId> empire;
        for (size_t p = 0; p < 2; ++p)
            for (objects::ObjectHandle h : tm->primary().owned_by(static_cast<int32_t>(p))) {
                const objects::ObjectId id = tm->primary().id(h);
                if (!tm->primary().alive(h) || !b->abilities().has_ability(id, kP2W)) continue;
                if (b->abilities().black_market_allows(id, kP2W)) ++out.allowed[p];
                else ++out.gated[p];
                if (p == 0) {
                    ids.push_back(static_cast<uint32_t>(id));
                    empire.push_back(id);
                }
            }
        commands::EventData ev(commands::EventType::SetUnitAbilityMode);
        ev.set("ability", kP2W).set("on", 1).set_ids("units", ids);
        out.command = static_cast<int>(host.execute_direct(0, commands::CommandSource::Player, ev));
        for (objects::ObjectId id : empire) out.active_after += b->abilities().is_active(id, kP2W) ? 1u : 0u;
    }
    out.crc = host.crc_log();
    out.battles = battles.log();
    return out;
}

void test_black_market_battle() {
    auto d = make_data(false, false, false, true);
    CHECK(d->rules.special.items.size() == 1);
    // Battle set-up: the blocked list per player (galactic purchases -> tactical name CRCs).
    {
        tactical::abilities::AbilityData ad;
        tactical::abilities::AbilityObjectDesc o;
        tactical::abilities::UnitAbilityRecord listed, unlisted, plain;
        listed.type = tactical::abilities::kDefend;
        listed.gui_activated_ability_name = "S10_BM_Power";
        listed.gui_name_crc = name_crc(listed.gui_activated_ability_name);
        listed.must_be_bought_on_black_market = true;
        unlisted = listed;  // marked but listed by no item: allowed (the TR case)
        unlisted.gui_activated_ability_name = "S10_Not_Listed";
        unlisted.gui_name_crc = name_crc(unlisted.gui_activated_ability_name);
        plain = listed;  // listed but not marked: allowed
        plain.must_be_bought_on_black_market = false;
        o.unit_abilities = {listed, unlisted, plain};
        ad.objects.push_back(o);
        jobs::JobSystem js(jobs::JobSystemConfig{0});
        SimConfig cfg;
        cfg.seed = 2026;
        cfg.players = 4;
        SimHost host(d->types, js, cfg);
        galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, false));
        CHECK(gs != nullptr);
        if (gs) {
            const std::vector<uint32_t> b0 = black_market_blocked_abilities(ad, *gs, 0);
            CHECK(b0.size() == 1 && b0[0] == name_crc("S10_BM_Power"));
            CHECK(gs->bm_give_item(0, "S10_BM_Item"));
            CHECK(black_market_blocked_abilities(ad, *gs, 0).empty() && black_market_blocked_abilities(ad, *gs, 1).size() == 1);
        }
    }
    const uint64_t ticks = 900;
    const BmRun a = run_bm_campaign(*d, false, 0, ticks);
    const BmRun p = run_bm_campaign(*d, true, 0, ticks);
    std::printf("  not bought: allowed %u/%u gated %u/%u, command 43 -> %d, active %u\n", a.allowed[0], a.allowed[1], a.gated[0],
                a.gated[1], a.command, a.active_after);
    std::printf("  bought:     allowed %u/%u gated %u/%u, command 43 -> %d, active %u\n", p.allowed[0], p.allowed[1], p.gated[0],
                p.gated[1], p.command, p.active_after);
    CHECK(a.command >= 0 && a.allowed[0] == 0 && a.allowed[1] == 0 && a.gated[0] > 0 && a.gated[1] > 0 && a.active_after == 0);
    CHECK(p.command >= 0 && p.allowed[0] == a.gated[0] && p.gated[0] == 0 && p.gated[1] == a.gated[1] && p.active_after == p.allowed[0]);
    CHECK(first_divergence(a.crc, p.crc).diverged);  // the activation shows in the state
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const BmRun b = run_bm_campaign(*d, false, w, ticks);
        const auto dv = first_divergence(a.crc, b.crc);
        std::printf("  bm battle workers %2d: %s\n", w, dv.message.c_str());
        CHECK(!dv.diverged && b.battles == a.battles);
    }
    const std::string path = std::string(ALAMO_SIM_TESTDATA) + "/battle_bm_golden.crc";
    CrcLog log = a.crc;
    log.comments = {"P4 tactical Black Market gate: S10 synthetic space campaign, POWER_TO_WEAPONS marked and not bought, "
                    "seed 2026, " + std::to_string(ticks) + " ticks"};
    std::string err;
    if (g_regen) {
        CHECK(log.save(path, &err));
        std::printf("  wrote %s\n", path.c_str());
    } else {
        CrcLog g;
        if (!CrcLog::load(path, g, &err)) {
            std::printf("FAIL golden %s: %s (run with --regen)\n", path.c_str(), err.c_str());
            ++g_fail;
        } else {
            const auto dv = first_divergence(g, log);
            std::printf("  golden battle_bm_golden.crc: %s\n", dv.message.c_str());
            CHECK(!dv.diverged);
        }
    }
}
// --- end P4 tactical ---

// --- battle host (BT pass): land lead / reserve split, HIJACK reserve, ResumeGame, map picker, pad command refusals ---
void test_bt_pass(const Data& d) {
    // Map picker (CU8F_T03_T05_RE §1.3, cu8f t03; [port] deterministic draw).
    {
        using MC = CampaignBattleOptions::MapCandidate;
        std::vector<MC> c(7);
        c[0] = {"Land_A", 1, 0, 1, 2, 5, 0};
        c[1] = {"Land_B", 1, 0, 1, 4, 5, 0};
        c[2] = {"Land_C", 1, 0, 1, 1, 5, 0};   // 1 player: never
        c[3] = {"Land_D", 1, 0, 3, 8, 5, 0};   // faction 3 (every MP map): only for query faction -1
        c[4] = {"Land_E", 1, 0, 1, 2, 5, 1};   // custom map: never a campaign pick
        c[5] = {"Space_A", 2, 7, 1, 2, 5, 0};
        c[6] = {"Land_F", -1, 0, 1, 2, 5, 0};  // no mini 01: matches nothing
        const std::string a = pick_random_map(c, false, 0, 1, 2);
        CHECK(a == "Land_A" || a == "Land_B");
        CHECK(pick_random_map(c, false, 0, 1, 2) == a);         // deterministic
        CHECK(pick_random_map(c, false, 3, 0, 2).empty());       // terrain 3, faction 0: no candidate
        CHECK(pick_random_map(c, true, 7, 1, 1) == "Space_A");   // one survivor: no draw
        CHECK(pick_random_map(c, false, 0, 1, 6).empty());       // base level above every max
        CHECK(pick_random_map(c, false, 0, 0, 2).empty());       // candidate faction is never a wildcard
        CHECK(pick_random_map(c, false, 0, 3, 2) == "Land_D");
        const std::string any = pick_random_map(c, false, 0, -1, 2);  // query faction -1: any faction
        CHECK(any == "Land_A" || any == "Land_B" || any == "Land_D");
        CHECK(pick_random_map(c, false, 0, 1, 2, "Land_A") == "Land_A");  // the scan stops after the preferred name
        const std::string b = pick_random_map(c, false, 0, 1, 2, "Land_B");
        CHECK(b == "Land_A" || b == "Land_B");
        // Terrain bit 31: "not this terrain" (0xffffffff: any terrain).
        CHECK(pick_random_map(c, true, static_cast<int32_t>(0x80000000u | 7u), 1, 1).empty());
        CHECK(pick_random_map(c, true, static_cast<int32_t>(0x80000003u), 1, 1) == "Space_A");
        CHECK(pick_random_map(c, true, -1, 1, 1) == "Space_A");
        CHECK(pick_random_map(c, false, 7, 1, 2).empty());  // a plain index is never a wildcard
        // The candidate fields come from the .ted's chunk 0 minis, retail defaults for absent ones (§1.2).
        const auto ted0 = [](std::vector<terrain::RawMini> minis) {  // a file holding only the map-info chunk
            terrain::ChunkWriter w;
            w.mini_leaf(0, minis);
            return w.take();
        };
        using terrain::mk_u32;
        MC e;
        CHECK(read_map_candidate(ted0({mk_u32(0x00, 513), mk_u32(0x01, 1), mk_u32(0x02, 6), mk_u32(0x03, 5), mk_u32(0x05, 3),
                                       mk_u32(0x06, 4), terrain::mk_u8(0x0B, 0)}),
                                 "DATA\\ART\\MAPS\\_MP_LAND_X.TED", e));
        CHECK(e.name == "DATA\\ART\\MAPS\\_MP_LAND_X.TED" && e.map_type == 1 && e.players == 6 && e.max_base_level == 5 &&
              e.faction == 3 && e.terrain == 4 && e.custom == 0);
        CHECK(read_map_candidate(ted0({mk_u32(0x00, 513)}), "X", e));  // only the format: every default
        CHECK(e.map_type == -1 && e.players == 1 && e.max_base_level == 1 && e.faction == 0 && e.terrain == 0 && e.custom == 0);
        CHECK(!read_map_candidate(std::vector<uint8_t>{1, 2, 3}, "Y", e));
        // The table: list A keeps every readable file, list B only custom-flag-1 files; a path already listed is skipped.
        struct Mem final : FileProvider {
            std::map<std::string, std::vector<uint8_t>> f;
            bool read(std::string_view p, std::vector<uint8_t>& out) const override {
                const auto it = f.find(std::string(p));
                if (it == f.end()) return false;
                out = it->second;
                return true;
            }
            bool exists(std::string_view p) const override { return f.count(std::string(p)) != 0; }
            std::string origin(std::string_view) const override { return {}; }
        } mem;
        mem.f["DATA\\ART\\MAPS\\A.TED"] = ted0({mk_u32(0x01, 1), mk_u32(0x02, 4)});
        mem.f["DATA\\CUSTOMMAPS\\C0.TED"] = ted0({mk_u32(0x01, 1), mk_u32(0x02, 4)});  // custom flag 0: not in list B
        mem.f["DATA\\CUSTOMMAPS\\C1.TED"] = ted0({mk_u32(0x01, 1), terrain::mk_u8(0x0B, 1)});
        mem.f["DATA\\ART\\MAPS\\M.TED"] = ted0({mk_u32(0x01, 2), terrain::mk_u8(0x0B, 1)});  // a mod-only custom map
        const MapLister lister = [](std::string_view g, bool mod) -> std::vector<std::string> {
            if (mod) return {"DATA\\ART\\MAPS\\A.TED", "DATA\\ART\\MAPS\\M.TED"};
            if (g.find("CustomMaps") != std::string_view::npos) return {"DATA\\CUSTOMMAPS\\C0.TED", "DATA\\CUSTOMMAPS\\C1.TED"};
            return {"DATA\\ART\\MAPS\\A.TED", "DATA\\ART\\MAPS\\MISSING.TED"};
        };
        const std::vector<MC> t = build_map_candidates(mem, lister);
        CHECK(t.size() == 3);
        if (t.size() == 3) {
            CHECK(t[0].name == "DATA\\ART\\MAPS\\A.TED" && t[0].players == 4 && t[0].custom == 0);
            CHECK(t[1].name == "DATA\\CUSTOMMAPS\\C1.TED" && t[1].custom == 1);
            CHECK(t[2].name == "DATA\\ART\\MAPS\\M.TED" && t[2].custom == 1 && t[2].map_type == 2);
        }
        CHECK(map_candidate_source(mem, lister)().size() == 3);
    }
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Pending;  // the human defender answers with ResumeGame
    opt.battle.max_frames = 600;
    CampaignBattles battles(host, d.crules, *d.catalog,
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
    CHECK(up.size() == 2);
    CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false, resumed = false, checked = false;
    for (uint64_t t = 0; t < 1500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        if (!resumed && battles.conflicts().state() == galactic::ConflictState::Vote) {
            commands::EventData ev(commands::EventType::ResumeGame);
            ev.set("flag", 0);  // bit 0 = fight
            CHECK(host.execute_direct(0, commands::CommandSource::Player, ev) == commands::RejectReason::None);
            resumed = true;
        }
        if (TacticalBattle* b = battles.battle(); b && host.tactical() && b->frame() == 1) {
            // TACTICAL_GAPS §4.2: the attacker starts with its lead unit only; the second squad is in the reserve.
            CHECK(!b->land_raid());
            CHECK(b->reserves(1).size() == 1);
            CHECK(b->reserves(0).empty());
            // HIJACK reserve: a galactic squad created at the battle planet joins player 1's pool; once only; a frigate
            // (not in this land battle's tables) is refused; the defender's own surface unit is already a participant.
            const objects::ObjectHandle hs = gs->spawn_unit(1, 1, type_of(d, "S10_Squad"));
            CHECK(static_cast<bool>(hs));
            CHECK(b->add_reserve(hs, 1));
            CHECK(!b->add_reserve(hs, 1));
            CHECK(b->reserves(1).size() == 2);
            CHECK(!b->can_add_reserve(type_of(d, "S10_Frigate")));
            CHECK(!b->add_reserve(hs, 2));  // not a side of this battle
            // TacticalBuild (33) / TacticalSell (34) refusals without pads.
            commands::EventData tb(commands::EventType::TacticalBuild);
            tb.set("pad", 1).set("type_index", type_of(d, "S10_Trooper"));
            CHECK(b->handle_command(*host.tactical(), 0, tb, host.random().synced()) == commands::RejectReason::InvalidTarget);
            commands::EventData ts(commands::EventType::TacticalSell);
            ts.set("object", 1);
            CHECK(b->handle_command(*host.tactical(), 0, ts, host.random().synced()) == commands::RejectReason::InvalidUnit);
            CHECK(b->lead_object() == objects::kNoObjectId);
            checked = true;
        }
    }
    CHECK(invaded && resumed && checked);
}
// --- end battle host ---

// --- S10 map owners [probe 2026-10-29] (REMAINING_GAPS §3.3): a .ted owner is a faction index resolved to that faction's
// player; natives without a player -> the Hostile player; a playable faction without a player -> dropped. The attacker
// that owns no reinforcement point gets the Neutral one nearest its Attacker Entry marker. The galaxy's player order
// deliberately differs from the faction order: Underworld (faction 1) = player 0 invades, Empire (faction 0) = player 1
// defends, Neutral (2) = player 2, Hostile (faction 3) = player 3. ---
void test_map_owners(const Data& d) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    std::swap(setup.players[0], setup.players[1]);
    for (galactic::StartingForce& f : setup.forces) f.player = f.player == 0 ? 1 : f.player == 1 ? 0 : f.player;
    galactic::PlayerSetup hostile;
    hostile.faction = d.rules.find_faction("Hostile");
    CHECK(hostile.faction == 3u && d.rules.find_faction("S10_Natives") == 4u && d.rules.find_faction("S10_Absent") == 5u);
    setup.players.push_back(hostile);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                auto place = [&m](const char* type, float x, float y, uint32_t raw) {
                                    terrain::ObjectPlacement p;
                                    p.type_crc = name_crc(type);
                                    p.owner = raw;
                                    p.position[0] = x;
                                    p.position[1] = y;
                                    m.objects.push_back(p);
                                };
                                place("Defending Forces Position", 900.0f, 900.0f, 2);
                                place("Attacker Entry Position", 300.0f, 300.0f, 2);
                                place("S4_RPoint", 1500.0f, 1500.0f, 2);  // far Neutral point: stays Neutral
                                place("S4_RPoint", 330.0f, 320.0f, 2);    // nearest the entry: the attacker's landing point
                                place("S4_Post", 600.0f, 1800.0f, 0);     // Empire -> player 1
                                place("S4_Post", 700.0f, 1800.0f, 1);     // Underworld -> player 0 = attacker: branched-map discard (§3.1 item 4)
                                place("S4_Post", 800.0f, 1800.0f, 4);     // native faction, no player -> Hostile (3)
                                place("S4_Post", 900.0f, 1800.0f, 5);     // playable faction without a player -> dropped
                                place("S4_Post", 1000.0f, 1800.0f, 3);    // Hostile -> player 3
                                place("S4_Post", 1100.0f, 1800.0f, 9);    // no such faction -> dropped
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 0) up.push_back(h);
    CHECK(gs->launch_units(0, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 0);
    if (!uf.empty()) CHECK(gs->move_fleet(0, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 1500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 0);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(0, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        if (!b || !host.tactical() || b->frame() != 1) continue;
        CHECK(b->request().info.attacker == 0 && b->request().info.defender == 1);
        // bkt1 t07 (b0084): the stand-in orders drive the attacker and the defender only; a third party idles
        CHECK(b->standin_owner(0) && b->standin_owner(1) && !b->standin_owner(2) && !b->standin_owner(-1));
        const objects::ObjectStore& st = host.tactical()->primary();
        const objects::TypeIndex post = d.types.resolve_name("S4_Post"), rp = d.types.resolve_name("S4_RPoint");
        std::map<int, int32_t> post_owner;  // x position -> owner
        int32_t near_owner = -9, far_owner = -9;
        objects::ObjectId near_id = objects::kNoObjectId;
        for (const objects::ObjectHandle h : st.all_objects()) {
            if (!st.alive(h)) continue;
            const Vec3 p = st.position(h);
            if (st.type(h) == post) post_owner[static_cast<int>(std::lround(p.x / 100.0f)) * 100] = st.owner(h);
            if (st.type(h) == rp && p.x < 400.0f) {
                near_owner = st.owner(h);
                near_id = st.id(h);
            }
            if (st.type(h) == rp && p.x > 1000.0f) far_owner = st.owner(h);
        }
        std::printf("  map owners: posts");
        for (const auto& [x, o] : post_owner) std::printf(" %d:%d", x, o);
        std::printf("; landing point owner %d (far point %d), landing points %zu\n", near_owner, far_owner, b->landing_points().size());
        CHECK(post_owner.size() == 3);
        CHECK(post_owner.count(600) && post_owner[600] == 1);
        CHECK(!post_owner.count(700));  // the attacker's Is_Branched_Map_Discardable object (default Yes) is discarded
        CHECK(post_owner.count(800) && post_owner[800] == 3);
        CHECK(post_owner.count(1000) && post_owner[1000] == 3);
        CHECK(!post_owner.count(900) && !post_owner.count(1100));
        CHECK(near_owner == 0 && far_owner == 2);
        CHECK(b->landing_points().size() == 1 && b->landing_points()[0] == near_id);
        checked = true;
    }
    CHECK(invaded && checked);
}

// ccpgt t02 (SKIRMISH_SETUP §4.2): the Capture Control Points rule on the S10 land invasion (Underworld player 0 attacks,
// Empire player 1 defends). S4_Post is CAPTURE_POINT with Control_Point_Domination_Condition_Relevant; the one post (Neutral)
// stands where the attackers gather, so both sides fight over it and the attackers hold it once the defenders are dead. Each
// completed capture by a playable owner arms the domination timer at 900 frames (30 s, no GameConstants entry; the only
// relevant point is the changing one, so "every other point allied" holds), a transition to Neutral disarms it; at <= 0 it
// tests 0x15 and the battle ends 210 frames later with the attacker winning 0x15. The defenders' elimination (0xb in a
// normal battle) wins nothing. Without BattleConfig::control_point_domination the timer never arms.
void test_control_point_domination(const Data& d, bool domination) {
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSetup setup = make_setup(d, true);
    std::swap(setup.players[0], setup.players[1]);
    for (galactic::StartingForce& f : setup.forces) f.player = f.player == 0 ? 1 : f.player == 1 ? 0 : f.player;
    galactic::GalacticSim* gs = host.start_galactic(d.rules, std::move(setup));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 4000;
    opt.battle.land_capture_countdown = 0.0f;  // the fixture has no Land_Capture_Allowed_Countdown_Seconds
    opt.battle.control_point_domination = domination;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                auto place = [&m](const char* type, float x, float y, uint32_t raw) {
                                    terrain::ObjectPlacement p;
                                    p.type_crc = name_crc(type);
                                    p.owner = raw;
                                    p.position[0] = x;
                                    p.position[1] = y;
                                    m.objects.push_back(p);
                                };
                                place("Defending Forces Position", 900.0f, 900.0f, 2);
                                place("Attacker Entry Position", 300.0f, 300.0f, 2);
                                place("S4_Post", 480.0f, 540.0f, 2);  // Neutral, where the attackers gather
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 0) up.push_back(h);
    CHECK(gs->launch_units(0, 2, up) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 0);
    if (!uf.empty()) CHECK(gs->move_fleet(0, uf[0], 1) == commands::RejectReason::None);
    bool invaded = false, ended = false;
    int32_t armed_timer = -1, armed_leader = -1, declared_type = -1, winner = -9, prev_timer = -1;
    uint32_t armed_frame = 0, declared_frame = 0, end_frame = 0, arms = 0;
    bool had_battle = false;
    for (uint64_t t = 0; t < 8000 && !ended; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 0);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(0, 1, {}) == commands::RejectReason::None;
        }
        const TacticalBattle* b = battles.battle();
        if (!b) {
            ended = had_battle;  // the battle is removed in the tick it finishes (the declared player wins)
            continue;
        }
        had_battle = true;
        end_frame = b->frame();
        if (const int32_t tm = b->control_point_timer(); tm >= 0 && (prev_timer < 0 || tm > prev_timer)) {  // (re)armed
            ++arms;
            armed_timer = tm;
            armed_leader = b->control_point_leader();
            armed_frame = b->frame();
        }
        prev_timer = b->control_point_timer();
        if (declared_type < 0 && b->victory_declared()) {
            declared_type = b->victory_declared_type();
            winner = b->victory_declared_player();
            declared_frame = b->frame();
        }
    }
    std::printf("  control points (%s): %u arms, last at frame %u timer %d leader %d, declared 0x%x at %u, end %u winner %d\n",
                domination ? "on" : "off", arms, armed_frame, armed_timer, armed_leader, static_cast<unsigned>(declared_type & 0xff),
                declared_frame, end_frame, winner);
    CHECK(invaded);
    if (!domination) {
        CHECK(armed_timer < 0);
        return;
    }
    // The last arming (about 900 frames left: the capture service and the countdown share the tick) is the attacker's, the
    // 0x15 test passes when it runs out, and the record's 210-frame countdown ends the battle.
    CHECK(arms >= 1 && armed_leader == 0 && armed_timer > 890 && armed_timer <= 900);
    CHECK(declared_type == 0x15 && declared_frame >= armed_frame + 890 && declared_frame <= armed_frame + 901);
    CHECK(ended && winner == 0 && end_frame >= declared_frame + 205 && end_frame <= declared_frame + 212);
}

// Campaign galaxy (build_campaign_setup of S10_GC): players Empire 0 (human), Underworld 1, Neutral 2 and the Hostile
// player 3 the setup adds. `drop_hostile` removes it again (the galaxy before the change).
galactic::GalacticSetup campaign_setup(const Data& d, bool drop_hostile) {
    std::vector<std::string> diag;
    galactic::GalacticSetup s = galactic::build_campaign_setup(d.db, d.rules, "S10_GC", {}, &diag);
    for (uint32_t i = 0; i < s.map.planets.size(); ++i)  // the synthetic planets carry no Galactic_Position
        s.map.planets.position[i] = {static_cast<float>(i) * 60.0f, 0.0f, 0.0f};
    // bkc1 t03: one player per faction in table order (player = faction index), so Hostile (3), S10_Natives (4) and
    // S10_Absent (5) trail the campaign's factions; dropping them is the run without the Hostile / native players.
    if (drop_hostile)
        while (!s.players.empty() && s.players.back().faction >= d.rules.find_faction("Hostile")) s.players.pop_back();
    return s;
}

// The S10_GC land invasion of S10_B (Underworld 1 attacks Empire 0): map posts at x 600.. with raw owners (faction
// indices) 0 Empire, 1 Underworld, 4 S10_Natives, 3 Hostile; returns x -> owner at battle frame 1. bkc1 t03: the campaign
// holds a player per faction (player = faction index), so the native post keeps its own faction's player 4.
std::map<int, int32_t> campaign_battle_posts(const Data& d, bool drop_hostile) {
    std::map<int, int32_t> out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 8;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, campaign_setup(d, drop_hostile));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                auto place = [&m](const char* type, float x, float y, uint32_t raw) {
                                    terrain::ObjectPlacement p;
                                    p.type_crc = name_crc(type);
                                    p.owner = raw;
                                    p.position[0] = x;
                                    p.position[1] = y;
                                    m.objects.push_back(p);
                                };
                                place("S4_Post", 600.0f, 1800.0f, 0);   // Empire (defender) -> player 0
                                place("S4_Post", 700.0f, 1800.0f, 1);   // Underworld (attacker): branched-map discard
                                place("S4_Post", 800.0f, 1800.0f, 4);   // native faction: its own player 4
                                place("S4_Post", 1000.0f, 1800.0f, 3);  // Hostile
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
    bool invaded = false, checked = false;
    for (uint64_t t = 0; t < 1500 && !checked; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        if (!b || !host.tactical() || b->frame() != 1) continue;
        CHECK(b->request().info.attacker == 1 && b->request().info.defender == 0);
        const objects::ObjectStore& st = host.tactical()->primary();
        const objects::TypeIndex post = d.types.resolve_name("S4_Post");
        for (const objects::ObjectHandle h : st.all_objects())
            if (st.alive(h) && st.type(h) == post) out[static_cast<int>(std::lround(st.position(h).x / 100.0f)) * 100] = st.owner(h);
        checked = true;
    }
    CHECK(invaded && checked);
    return out;
}

// [probe 2026-10-29, inferred] the campaign's Hostile player: inert on the galactic map (no AI, no credits, no planets, no
// income), and the owner of the map's Hostile objects in a campaign land battle. bkc1 t03 (docs/re/BKC1_RE_ANSWERS.md Q2
// [H]): a campaign creates one player for every faction of the table, in table order (player = faction index), so the
// factions the campaign does not name (Hostile 3, S10_Natives 4, S10_Absent 5) are all such inert players.
void test_campaign_hostile(const Data& d) {
    const galactic::GalacticSetup s = campaign_setup(d, false);
    const uint32_t hostile = d.rules.find_faction("Hostile");
    CHECK(s.players.size() == 6 && s.players.size() == d.rules.factions.size());
    for (size_t i = 0; i < s.players.size(); ++i) CHECK(s.players[i].faction == static_cast<uint32_t>(i));
    CHECK(s.players.size() == 6 && s.players[0].faction == d.rules.find_faction("Empire") && s.players[0].human &&
          s.players[1].faction == d.rules.find_faction("Underworld") && s.players[2].faction == d.rules.find_faction("Neutral"));
    CHECK(s.players.size() == 6 && s.players[3].faction == hostile);
    for (size_t i = 3; i < s.players.size(); ++i)
        CHECK(!s.players[i].human && !s.players[i].ai && s.players[i].credits == 0.0f);
    for (const galactic::StartingForce& f : s.forces) CHECK(f.player < 3);
    // Galactic behaviour per faction is the same with and without the Hostile player (no battle: 2400 ticks of economy).
    auto run = [&d](bool drop) {
        std::vector<float> trace;
        jobs::JobSystem js(jobs::JobSystemConfig{0});
        SimConfig cfg;
        cfg.seed = 2026;
        cfg.players = 8;
        SimHost host(d.types, js, cfg);
        galactic::GalacticSim* gs = host.start_galactic(d.rules, campaign_setup(d, drop));
        CHECK(gs != nullptr);
        if (!gs) return trace;
        for (int t = 0; t < 2400; ++t) {
            host.tick_once();
            if (t % 60 != 59) continue;
            for (const char* f : {"Empire", "Underworld", "Neutral"}) {
                const int32_t p = gs->player_of_faction(f);
                trace.push_back(gs->credits(p));
                trace.push_back(static_cast<float>(gs->planets_owned(p)));
                trace.push_back(static_cast<float>(gs->players()[static_cast<size_t>(p)].tech_level));
            }
            for (const galactic::PlanetState& p : gs->planets())
                trace.push_back(p.owner < 0 ? -1.0f : static_cast<float>(gs->players()[static_cast<size_t>(p.owner)].faction));
            const int32_t h = gs->player_of_faction("Hostile");
            if (!drop) CHECK(h == 3 && gs->credits(h) == 0.0f && gs->planets_owned(h) == 0 && gs->fleets_at(1, h).empty());
        }
        std::printf("  campaign hostile (%s): day %d, credits E %.0f U %.0f\n", drop ? "dropped" : "present", gs->galactic_day(),
                    static_cast<double>(gs->credits(gs->player_of_faction("Empire"))),
                    static_cast<double>(gs->credits(gs->player_of_faction("Underworld"))));
        return trace;
    };
    const std::vector<float> with = run(false), without = run(true);
    CHECK(!with.empty() && with == without);
    // The land battle: the Hostile- and native-owned posts survive with their players; without them they are dropped.
    std::map<int, int32_t> posts = campaign_battle_posts(d, false);
    std::printf("  campaign land battle posts:");
    for (const auto& [x, o] : posts) std::printf(" %d:%d", x, o);
    std::printf("\n");
    CHECK(posts.size() == 3 && posts.count(600) && posts[600] == 0 && !posts.count(700));
    CHECK(posts.count(800) && posts[800] == 4 && posts.count(1000) && posts[1000] == 3);
    posts = campaign_battle_posts(d, true);
    CHECK(posts.size() == 1 && posts.count(600) && posts[600] == 0);
}
// --- end S10 map owners ---

// --- hangars / station contents / skirmish tactical production (tactical_battle_hangar.cpp) ---
// s78n t20 step 5 (SKIRMISH_SETUP §3 "Completion (0x498f70)": signal 0x22 for every finished build, before the
// DUMMY_UPGRADE branch; AI_GALACTIC_3 §1.11): the S10 land battle as a skirmish; at frame 2 the Empire buys
// S10_Land_Upgrade (DUMMY_UPGRADE) at one of its troopers. When the build finishes the battle reports it in
// production_done (player 0, the upgrade's type) - the AI build task's Produce_Force waits for that signal - and the
// upgrade object sits in the trooper's contents; no reinforcement pool entry is made for it.
void test_land_upgrade_production_done() {
    auto d = make_data(false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, true));
    CHECK(gs != nullptr);
    if (!gs) return;
    gs->add_credits(0, 1000.0f, false);
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.battle.skirmish = true;
    opt.battle.skip_human_units = true;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Land_B") return false;
                                m = land_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    std::vector<objects::ObjectHandle> up_units;
    for (objects::ObjectHandle h : gs->planets()[2].surface)
        if (gs->store().owner(h) == 1) up_units.push_back(h);
    CHECK(gs->launch_units(1, 2, up_units) == commands::RejectReason::None);
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const uint32_t trooper_t = type_of(*d, "S10_Trooper"), up = type_of(*d, "S10_Land_Upgrade");
    objects::ObjectId builder = objects::kNoObjectId;
    bool invaded = false, bought = false, signalled = false, held = false;
    uint32_t bought_frame = 0, signal_frame = 0;
    size_t reserve0 = 0, reserve1 = 0;
    for (uint64_t t = 0; t < 6000 && !signalled; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) {
            if (bought) break;
            continue;
        }
        if (!bought && b->frame() >= 2) {
            CHECK(b->production_enabled());
            for (const objects::ObjectHandle h : tm->primary().owned_by(0))
                if (builder == objects::kNoObjectId && tm->primary().alive(h) && tm->primary().type(h) == trooper_t) builder = tm->primary().id(h);
            CHECK(builder != objects::kNoObjectId);
            if (builder == objects::kNoObjectId) break;
            reserve0 = b->reserves(0).size();
            commands::EventData ev(commands::EventType::Production);
            ev.set("type_index", up).set("location", static_cast<uint32_t>(builder));
            const commands::RejectReason rr = host.execute_direct(0, commands::CommandSource::Player, ev);
            CHECK(rr == commands::RejectReason::None);
            if (rr != commands::RejectReason::None) break;
            bought = true;
            bought_frame = b->frame();
            continue;
        }
        for (const TacticalBattle::ProductionDone& p : b->production_done())
            if (p.player == 0 && p.type == up) {
                signalled = true;
                signal_frame = b->frame();
            }
        if (signalled) {
            held = b->contents() && !b->contents()->contents(builder).empty();
            reserve1 = b->reserves(0).size();
        }
    }
    std::printf("  land upgrade: bought %d at frame %u, production-done signal %d at frame %u, held by the builder %d, reserve %zu -> %zu\n",
                bought ? 1 : 0, bought_frame, signalled ? 1 : 0, signal_frame, held ? 1 : 0, reserve0, reserve1);
    CHECK(bought && signalled && signal_frame >= bought_frame + 30 && held && reserve1 == reserve0);
}

// The S10 space battle as a skirmish battle: at frame 2 the human Empire buys S10_Base_Upgrade at its star base
// (Production {type_index, location}); a second purchase is refused (Build_Limit_Current_For_All_Allies 1), a
// non-builder location too. After the 1 s build the upgrade object sits in the star base's contents, its
// Skirmish_Automatic Starbase_Upgrade_Ability fires: S10_Star_Base_2 replaces the star base, the contents move with
// it, the battle tech level rises by one and the upgrade object is removed silently.
void test_station_production() {
    auto d = make_data(false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, false));
    CHECK(gs != nullptr);
    if (!gs) return;
    gs->add_credits(0, 1000.0f, false);
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 600;
    opt.battle.skirmish = true;
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const uint32_t base1 = type_of(*d, "S10_Star_Base_1"), base2 = type_of(*d, "S10_Star_Base_2"), up = type_of(*d, "S10_Base_Upgrade");
    objects::ObjectId station = objects::kNoObjectId;
    bool bought = false, held = false, upgraded = false;
    float before = 0.0f, after = 0.0f;
    int32_t tech0 = 0;
    for (uint64_t t = 0; t < 3000 && !upgraded; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        if (!bought && b->frame() == 2) {
            CHECK(b->production_enabled());
            for (const objects::ObjectHandle h : tm->primary().owned_by(0))
                if (tm->primary().alive(h) && tm->primary().type(h) == base1) station = tm->primary().id(h);
            CHECK(station != objects::kNoObjectId);
            tech0 = b->battle_tech_level(0);
            before = gs->credits(0);
            commands::EventData ev(commands::EventType::Production);
            ev.set("type_index", up).set("location", static_cast<uint32_t>(station));
            CHECK(host.execute_direct(0, commands::CommandSource::Player, ev) == commands::RejectReason::None);
            after = gs->credits(0);
            CHECK(host.execute_direct(0, commands::CommandSource::Player, ev) == commands::RejectReason::NotAllowed);  // limit 1
            commands::EventData bad(commands::EventType::Production);
            bad.set("type_index", up).set("location", 999999u);
            CHECK(host.execute_direct(0, commands::CommandSource::Player, bad) != commands::RejectReason::None);
            CHECK(b->production() && b->production()->queue(0).size() == 1);
            bought = true;
        }
        if (bought && !held && b->contents() && !b->contents()->contents(station).empty()) {  // the built upgrade object
            held = true;
            const tactical::combat::ObjectId uo = b->contents()->contents(station).front();
            std::printf("  frame %u: upgrade object %d built in the star base\n", b->frame(), uo);
            CHECK(b->abilities().has_object(uo) && b->contents()->container_of(uo) == station);
            CHECK(b->frame() >= 2 + 30);  // Tactical_Build_Time_Seconds 1 at 30 FPS
        }
        if (bought && !b->station_upgrades().empty()) {
            const TacticalBattle::StationUpgrade& u = b->station_upgrades().front();
            const objects::ObjectHandle nh = tm->primary().find(u.new_station);
            std::printf("  station %d -> %d (owner %d), tech %d -> %d, credits %.0f -> %.0f, contents of the new station %zu\n", u.old_station,
                        u.new_station, u.owner, tech0, b->battle_tech_level(0), static_cast<double>(before), static_cast<double>(after),
                        b->contents()->contents(u.new_station).size());
            CHECK(u.old_station == station && u.owner == 0 && tm->primary().alive(nh) && tm->primary().type(nh) == base2);
            CHECK(!tm->primary().alive(tm->primary().find(station)));
            CHECK(b->battle_tech_level(0) == tech0 + 1);
            CHECK(b->contents()->contents(station).empty());
            upgraded = true;
        }
    }
    std::printf("  bought %d, upgrade object held %d, upgraded %d, credits %.1f -> %.1f\n", bought ? 1 : 0, held ? 1 : 0,
                upgraded ? 1 : 0, static_cast<double>(before), static_cast<double>(after));
    CHECK(bought && held && upgraded);
    CHECK(after == before - 100.0f);
}
// --- end hangars ---

// cu8g t05 (CU8F_T04_T06_RE §3.3 [H]): TacticalBattle::player_gate_allows / special_weapons_allowed on the S10 space
// battle (gate data), campaign and skirmish. At battle frame 3: the Empire (0) has its star base's S10_Gate_On entries
// (allowed in both modes); the Underworld (1) has only reverse sources alive (no entry: allowed in a campaign battle,
// refused in a skirmish); bombardment has no entry for anyone (empty list rule); the reverse float list
// (Reinforcement_Time_Multiplier) is empty for both players (the old "reverse -> enemies" reading gave the Empire 1.0).
// The Lua Disable_Orbital_Bombardment entry is ANDed (false refuses even in a campaign battle; true allows even in a
// skirmish). Then an Underworld frigate dies: its reverse remove pushes the Underworld's Enable_Special_Weapons entry
// (allowed in both modes) and its reinforcement-time entry (2.0 - 1 = 1.0), for the owner only.
struct GateRun {
    bool seen = false, sw0 = false, sw1 = false, br0 = false, br1 = false, bd0 = false, bd_off = true, bd_on = false;
    bool killed = false, sw1_after = false;
    float rt0 = -1.0f, rt1 = -1.0f, rt0_after = -1.0f, rt1_after = -1.0f;
};
GateRun run_gate(const Data& d, bool skirmish) {
    GateRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 300;
    opt.battle.order_interval = 0;  // no stand-in orders
    opt.battle.skirmish = skirmish;
    CampaignBattles battles(host, d.crules, *d.catalog,
                            [](const std::string& name, terrain::MapData& m) {
                                if (name != "S10_Space_B") return false;
                                m = space_map();
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    CHECK(!uf.empty());
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    const auto frigate = static_cast<objects::TypeIndex>(type_of(d, "S10_Frigate"));
    using tactical::abilities::PlayerGate;
    using tactical::abilities::PlayerList;
    for (uint64_t t = 0; t < 3000 && !out.seen; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm || b->frame() != 3) continue;
        out.seen = true;
        out.sw0 = b->special_weapons_allowed(0);
        out.sw1 = b->special_weapons_allowed(1);
        out.br0 = b->player_gate_allows(0, PlayerGate::BombingRuns);
        out.br1 = b->player_gate_allows(1, PlayerGate::BombingRuns);
        out.bd0 = b->player_gate_allows(0, PlayerGate::Bombardment);
        out.rt0 = b->abilities().player_list(0, PlayerList::ReinforcementTime);
        out.rt1 = b->abilities().player_list(1, PlayerList::ReinforcementTime);
        b->set_bombardment_disabled(0, true);
        out.bd_off = b->player_gate_allows(0, PlayerGate::Bombardment);
        b->set_bombardment_disabled(0, false);
        out.bd_on = b->player_gate_allows(0, PlayerGate::Bombardment);
        for (const objects::ObjectHandle h : tm->primary().owned_by(1))
            if (tm->primary().alive(h) && tm->primary().type(h) == frigate) {
                out.killed = b->script_damage(*tm, tm->primary().id(h), 1.0e9f, false);
                break;
            }
        out.sw1_after = b->special_weapons_allowed(1);
        out.rt0_after = b->abilities().player_list(0, PlayerList::ReinforcementTime);
        out.rt1_after = b->abilities().player_list(1, PlayerList::ReinforcementTime);
    }
    return out;
}

void test_battlefield_modifier_gates() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, false,
                       false, false, true);
    for (const bool skirmish : {false, true}) {
        const GateRun r = run_gate(*d, skirmish);
        std::printf("  %s: frame 3 seen %d; special weapons E %d U %d, bombing runs E %d U %d, bombardment E %d (Lua off %d, on %d); "
                    "reinforcement time E %.2f U %.2f; U frigate killed %d -> special weapons U %d, reinforcement time E %.2f U %.2f\n",
                    skirmish ? "skirmish" : "campaign", r.seen, r.sw0, r.sw1, r.br0, r.br1, r.bd0, r.bd_off, r.bd_on,
                    static_cast<double>(r.rt0), static_cast<double>(r.rt1), r.killed, r.sw1_after, static_cast<double>(r.rt0_after),
                    static_cast<double>(r.rt1_after));
        CHECK(r.seen && r.killed);
        CHECK(r.sw0 && r.br0);                                // the star base's entries: allowed in both modes
        CHECK(r.sw1 == !skirmish && r.br1 == !skirmish);      // empty lists: campaign yes, skirmish no
        CHECK(r.bd0 == !skirmish && !r.bd_off && r.bd_on);    // empty, then the Lua entry alone (false, then true)
        CHECK(r.rt0 == 0.0f && r.rt1 == 0.0f);                // reverse: nothing while the sources live
        CHECK(r.sw1_after && r.rt1_after == 1.0f && r.rt0_after == 0.0f);  // the dead frigate's remove pushed, owner only
    }
}

// --- OI host: BattleAi orders without AI players (empty definitions; the battle's stand-in off) ---
//   * MoveToObject (Lua Move_To(object)): the mover's attack order is cleared and it moves to the target's position; a
//     missing target is a silent no-op; foreign movers are refused;
//   * hijack_unit (HIJACK_UNIT): a galactic frigate spawned at the battle planet joins the attacker's reserve pool; a type
//     outside the battle tables is refused;
//   * the AI VMs' ObjectBridge::change_owner: the battle's script_change_owner (store owner + the battle side).
// --- end OI host ---

// t41: the battle's loss records hold the deaths of the tick that ends the battle. The host can no longer read that tick's
// death events (the battle is gone when the tick returns), so counting them per tick falls short of the records.
void test_final_tick_losses(const Data& d) {
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
    size_t seen = 0;
    for (uint64_t t = 0; t < 7000; ++t) {
        host.tick_once();
        if (const TacticalBattle* b = battles.battle()) seen += b->combat().deaths().size();
        else if (battles.battles_fought() > 0) break;
    }
    CHECK(battles.battles_fought() == 1 && !battles.battle());
    const size_t recorded = battles.last_battle_losses().size();
    std::printf("  final tick: %zu deaths seen per tick, %zu in the loss records\n", seen, recorded);
    // story t06: the battle now ends 210 frames after the victory declaration, so the deaths come before the final tick;
    // the records still hold every one of them.
    CHECK(seen > 0 && recorded >= seen);
}

// story t06 (BT §3.1a): the tactical victory monitor in the S10 space battle (Underworld 1 attacks the Empire 0).
//   held:    ENABLE_VICTORY 0 at the battle's first tick (Param1-less and extra "1" rewards change nothing); the fight runs
//            to its end with every 0xb / 0xf test queued, nothing declared, the battle running; ENABLE_VICTORY 1 releases
//            the hold and replays the queue: a victory is declared at once, its 210-frame countdown ends the battle.
//   VICTORY: the reward for the Empire (Param2 1: 1 s, silent) at battle frame 10 releases a raised hold and ends the
//            battle 30 frames later as a story victory, the Underworld retreating; an unknown faction (-1) does nothing.
struct MonitorRun {
    bool started = false, declared_while_held = false, ended = false;
    size_t queued = 0;
    int32_t hold_after_noops = -1, declared_type = -1, declared_player = -2, countdown = -1;
    bool silent = false, unknown_refused = false;
    uint32_t release_frame = 0, end_frame = 0;
    std::string summary;
    CrcLog crc;
};

MonitorRun run_monitor(const Data& d, bool story_victory, int32_t workers) {
    MonitorRun out;
    jobs::JobSystem js(jobs::JobSystemConfig{workers});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d.types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d.rules, make_setup(d, false));
    CHECK(gs != nullptr);
    if (!gs) return out;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 0;  // no time limit: only the monitor ends the battle
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
    uint32_t last_deaths = 0, quiet = 0, last_frame = 0;
    for (uint64_t t = 0; t < 9000; ++t) {
        host.tick_once();
        TacticalBattle* b = battles.battle();
        if (!b) {
            if (!out.started) continue;
            out.ended = true;  // finished in this tick, popped in its hook
            out.end_frame = last_frame + 1;
            out.summary = battles.log().empty() ? std::string() : battles.log().back();
            break;
        }
        last_frame = b->frame();
        if (!out.started) {
            out.started = true;
            b->reward_enable_victory(true, 0);   // hold 1
            b->reward_enable_victory(false, 0);  // no Param1: nothing
            b->reward_enable_victory(true, 1);   // release: 0 (nothing queued yet)
            b->reward_enable_victory(true, 1);   // not held: nothing (stays 0)
            b->reward_enable_victory(true, 0);   // hold 1 again
            out.hold_after_noops = b->victory_hold();
        }
        const uint32_t f = b->frame();
        if (b->finished()) continue;
        if (story_victory) {
            if (f == 10) {
                out.unknown_refused = !b->reward_victory(-1, false) && !b->victory_declared();
                CHECK(b->reward_victory(0, true));
                out.release_frame = f;
                out.declared_type = b->victory_declared_type();
                out.declared_player = b->victory_declared_player();
                out.countdown = b->victory_countdown();
                out.silent = b->victory_declared_silent();
            }
            continue;
        }
        if (out.release_frame != 0) continue;
        if (b->victory_declared()) out.declared_while_held = true;
        const uint32_t deaths = b->stats().deaths;
        quiet = deaths == last_deaths ? quiet + 1 : 0;
        last_deaths = deaths;
        if (deaths > 0 && quiet >= 300) {  // the fight is over: release with replay
            out.queued = b->victory_queued();
            b->reward_enable_victory(true, 1);
            out.release_frame = f;
            out.declared_type = b->victory_declared() ? b->victory_declared_type() : -1;
            out.declared_player = b->victory_declared() ? b->victory_declared_player() : -2;
            out.countdown = b->victory_countdown();
            out.silent = b->victory_declared_silent();
        }
    }
    out.crc = host.crc_log();
    return out;
}

void test_victory_monitor(const Data& d) {
    const MonitorRun h = run_monitor(d, false, 0);
    std::printf("  held: hold after the no-ops %d, queued %zu at release frame %u, declared type 0x%x for %d (countdown %d), end %u; %s\n",
                h.hold_after_noops, h.queued, h.release_frame, static_cast<unsigned>(h.declared_type), h.declared_player, h.countdown,
                h.end_frame, h.summary.c_str());
    CHECK(h.started && h.hold_after_noops == 1 && !h.declared_while_held && h.queued > 0 && h.release_frame > 0);
    CHECK(h.declared_type == galactic::kVictoryAllDestroyed || h.declared_type == galactic::kVictoryStarbaseDestroyed);
    CHECK(h.declared_player == 0 || h.declared_player == 1);
    CHECK(h.countdown == 210 && !h.silent && h.ended && h.end_frame == h.release_frame + 210);
    const MonitorRun v = run_monitor(d, true, 0);
    std::printf("  VICTORY: unknown refused %d, declared type 0x%x for %d (countdown %d, silent %d) at %u, end %u; %s\n",
                v.unknown_refused ? 1 : 0, static_cast<unsigned>(v.declared_type), v.declared_player, v.countdown, v.silent ? 1 : 0,
                v.release_frame, v.end_frame, v.summary.c_str());
    CHECK(v.started && v.unknown_refused && v.declared_type == galactic::kVictoryStory && v.declared_player == 0 && v.countdown == 30 && v.silent);
    CHECK(v.ended && v.end_frame == v.release_frame + 30);
    CHECK(v.summary.find("winner Empire, victory 16, retreated Underworld") != std::string::npos);
    const MonitorRun h4 = run_monitor(d, false, 4);
    CHECK(!first_divergence(h.crc, h4.crc).diverged && h4.end_frame == h.end_frame);
}

// --- OI objects: behaviour Initialise draws (TACTICAL_GAPS 6.4) through the SimHost's synced RNG ---
// A space mode on its own: every spawned ship attaches SELECTABLE (1), SHIELDED (computed 90), POWERED (150), HUNT (30,
// starts disabled), AMBIENT_SFX (30), STARSHIP_LOCOMOTOR (1): four synced Get_Int(0, interval - 1) per ship in attach
// order, first due frame = mode frame + draw. Identical schedule and CRC log for every worker count.
void test_oi_objects_scheduling() {
    const fs::path root = fs::temp_directory_path() / "alamo_sim_oi_objects";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>U.xml</File></L>");
    write_file(root / "Data/XML/U.xml",
               "<R><SpaceUnit Name=\"OI_Ship\"><Victory_Relevant>Yes</Victory_Relevant><Tactical_Health>100</Tactical_Health><Max_Speed>2</Max_Speed>\n"
               "  <Behavior>SELECTABLE, SHIELDED, POWERED, HUNT, AMBIENT_SFX</Behavior><SpaceBehavior>STARSHIP_LOCOMOTOR</SpaceBehavior>\n"
               "</SpaceUnit></R>");
    DirectoryProvider dp(path_to_utf8(root));
    objects::ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    objects::GameObjectTypes types;
    CHECK(types.build(raw, &dp));
    const objects::TypeIndex ship = raw.find("OI_Ship");
    struct Result {
        CrcLog crc;
        std::vector<uint32_t> next;
        uint64_t draws = 0;
    };
    auto run = [&](int32_t workers) {
        Result r;
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        SimConfig cfg;
        cfg.seed = 4242;
        cfg.players = 2;
        SimHost host(types, js, cfg);
        auto& tm = static_cast<TacticalMode&>(host.start_game(objects::ModeKind::Space));
        objects::ObjectStore& st = tm.primary();
        auto spawn_checked = [&](float x) {
            SimRng ref = host.random().synced();
            const uint32_t frame = static_cast<uint32_t>(tm.frame());
            objects::SpawnParams sp;
            sp.type = ship;
            sp.owner = 0;
            sp.position = {x, 0.0f, 0.0f};
            const objects::ObjectHandle h = tm.spawn(sp, 1.0f);
            const auto bs = st.behaviors(h);
            for (uint32_t k = 0; k < bs.size(); ++k) {
                const uint32_t iv = st.behavior_interval(h, k);
                const uint32_t want = iv >= 2 ? frame + static_cast<uint32_t>(ref.range_int(0, static_cast<int32_t>(iv) - 1)) : 0u;
                CHECK(st.behavior_next_service(h, k) == want);
                r.next.push_back(st.behavior_next_service(h, k));
            }
            CHECK(ref == host.random().synced());
            CHECK(st.behavior_interval(h, 1) == 90 && st.behavior_interval(h, 2) == 150 && !st.behavior_enabled_by_type(h, objects::beh::Hunt));
        };
        for (int i = 0; i < 3; ++i) spawn_checked(100.0f * static_cast<float>(i));
        for (int t = 0; t < 20; ++t) host.tick_once();
        spawn_checked(500.0f);  // mid-battle creation: first due frame from the current mode frame
        for (int t = 0; t < 20; ++t) host.tick_once();
        r.crc = host.crc_log();
        r.draws = st.attach_draws();
        return r;
    };
    const Result a = run(0);
    std::printf("  oi objects: %llu attach draws, %zu behaviour schedules, %zu CRC records\n",
                static_cast<unsigned long long>(a.draws), a.next.size(), a.crc.records.size());
    CHECK(a.draws == 16 && !a.crc.records.empty());
    for (int32_t w : jobs::determinism_worker_counts()) {
        if (w == 0) continue;
        const Result b = run(w);
        CHECK(b.next == a.next && b.draws == a.draws && !first_divergence(a.crc, b.crc).diverged);
    }
    remove_all_long(root, ec);
}
// --- end OI objects ---

// S8 t10 follow-up (LOB_SOLVER §3.5 "MARKER removal", ABILITIES_HARDPOINTS §2.9): command 57 (special ability with a
// Dummy_Barrage_Target at a ground point) with PLACE_REMOTE_BOMB. The marker has no lifetime: it stays after the apply
// consumed the pending ability, while the thrower waits for its toss (the [port] cleanup of other markers would drop it
// there), and the toss kills it; the bomb (never armed) rests on the ground at the point until DETONATE_REMOTE_BOMB.
void test_remote_bomb_marker() {
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, true);
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
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
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
    const uint32_t dummy_type = type_of(*d, "Dummy_Barrage_Target");
    bool invaded = false, issued = false, waited = false, tossed = false;
    objects::ObjectId thrower = objects::kNoObjectId, marker = objects::kNoObjectId;
    float px = 0.0f, py = 0.0f;
    uint32_t issue_frame = 0;
    for (uint64_t t = 0; t < 6000 && !tossed; ++t) {
        host.tick_once();
        if (!invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        objects::ObjectStore& st = tm->primary();
        auto marker_alive = [&] { return marker != objects::kNoObjectId && st.alive(st.find(marker)); };
        if (!issued) {
            for (const objects::ObjectHandle h : st.owned_by(1))
                if (st.alive(h) && b->combat().has_unit(st.id(h)) && b->combat().alive(st.id(h))) {
                    thrower = st.id(h);
                    break;
                }
            CHECK(thrower > 0);
            if (thrower <= 0) return;
            const Vec3 p = st.position(st.find(thrower));
            px = p.x + 80.0f;
            py = p.y;
            commands::EventData ev(commands::EventType::TacticalSpecialAbilityWithDummyTarget);
            ev.set_ids("users", std::vector<uint32_t>{static_cast<uint32_t>(thrower)});
            ev.set("ability", static_cast<uint32_t>(tactical::abilities::kPlaceRemoteBomb)).set_f("x", px).set_f("y", py).set_f("z", 0.0f);
            CHECK(b->handle_command(*tm, 1, ev, host.random().synced()) == commands::RejectReason::None);
            for (const objects::ObjectHandle h : st.all_objects())
                if (st.alive(h) && static_cast<uint32_t>(st.type(h)) == dummy_type) marker = st.id(h);
            CHECK(marker_alive());
            issued = true;
            issue_frame = b->frame();
            continue;
        }
        // Applied (the pending order is consumed), not yet tossed: the marker stays.
        if (!waited && b->abilities().pending_type(thrower) < 0 && b->combat().projectile_count() == 0) {
            CHECK(marker_alive());
            waited = true;
        }
        for (size_t i = 0; i < b->combat().projectile_count(); ++i) {
            const tactical::combat::ProjectileView v = b->combat().projectile(i);
            if (!v.alive || !v.stuck) continue;
            CHECK(!marker_alive());  // killed by the toss
            CHECK(v.stuck_to == tactical::combat::kNoObject && std::fabs(v.pos.x - px) < 10.0f && std::fabs(v.pos.y - py) < 10.0f);
            std::printf("  remote bomb marker: issued at %u, bomb resting at (%.1f, %.1f) at %u, marker gone\n", issue_frame,
                        static_cast<double>(v.pos.x), static_cast<double>(v.pos.y), b->frame());
            tossed = true;
        }
    }
    CHECK(issued && waited && tossed);
}

// S8 t02 / S7 t13 (MOVEMENT §1.3 / §2.2): obstacle footprints follow their objects.
//   * land: a map-placed LAND_OBSTACLE bunker owns its placement stamp (object id) and loses it when it dies; a
//     Map_Load_Spawn_Table placeholder's stamp is replaced by the spawned rock's footprint; a rock spawned during the
//     battle is stamped at the end of that tick under its object id and unstamped the tick it is removed;
//   * space: a map-placed SPACE_OBSTACLE unit owns its placement entry (object id) and loses it when it dies (Hyperspace_Away
//     kill); a placeholder's entry is replaced by the spawned asteroid's (radius 400, not the placeholder's 50); S8 t09: the
//     asteroid (not a battle unit) loses its entry the tick it is removed;
//   * S8 t09 (S9_DEATH_CLONE_SHIELDS §1.6, MOVEMENT §1.3): a death clone keeps only HIDE_WHEN_FOGGED / IMPOSING_PRESENCE /
//     LAND_OBSTACLE (the last only with Death_Clone_Is_Obstacle) and gets DEATH; a clone without LAND_OBSTACLE is never
//     stamped.
void test_runtime_obstacles(bool space) {
    namespace mv = tactical::movement;
    auto d = make_data(false, false, false, false, false, true);
    jobs::JobSystem js(jobs::JobSystemConfig{0});
    SimConfig cfg;
    cfg.seed = 2026;
    cfg.players = 4;
    SimHost host(d->types, js, cfg);
    galactic::GalacticSim* gs = host.start_galactic(d->rules, make_setup(*d, !space));
    CHECK(gs != nullptr);
    if (!gs) return;
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.order_interval = 0;  // no stand-in orders: only the test moves units
    auto place = [](terrain::MapData& m, const char* type, float x, float y, uint32_t owner) {
        terrain::ObjectPlacement p;
        p.type_crc = name_crc(type);
        p.owner = owner;
        p.position[0] = x;
        p.position[1] = y;
        m.objects.push_back(p);
    };
    CampaignBattles battles(host, d->crules, *d->catalog,
                            [&place](const std::string& name, terrain::MapData& m) {
                                if (name == "S10_Space_B") {
                                    m = space_map();
                                    place(m, "S10_Obst_Mine", 2500.0f, 2500.0f, 0);               // placement 0
                                    place(m, "S10_Obst_Asteroid_Spawner", -2500.0f, 2500.0f, 2);  // placement 1
                                } else if (name == "S10_Land_B") {
                                    m = land_map();
                                    place(m, "S10_Obst_Bunker", 450.0f, 620.0f, 0);         // placement 0, near the attackers
                                    place(m, "S10_Obst_Rock_Spawner", 900.0f, 300.0f, 2);  // placement 1
                                } else {
                                    return false;
                                }
                                return true;
                            },
                            opt);
    CHECK(battles.attach(nullptr));
    if (!space) {
        std::vector<objects::ObjectHandle> up;
        for (objects::ObjectHandle h : gs->planets()[2].surface)
            if (gs->store().owner(h) == 1) up.push_back(h);
        CHECK(gs->launch_units(1, 2, up) == commands::RejectReason::None);
    }
    const std::vector<uint32_t> uf = gs->fleets_at(2, 1);
    if (!uf.empty()) CHECK(gs->move_fleet(1, uf[0], 1) == commands::RejectReason::None);
    // The cells a footprint covers on an empty grid of the battle grid's size (the expected stamp).
    auto cells_of = [](const mv::PassabilityGrid& like, float x, float y, float size) {
        mv::PassabilityGrid g;
        g.init(like.width(), like.height());
        g.stamp(1, mv::make_footprint({x, y}, 0.0f, size, size, -0.5f * size, -0.5f * size, 0, 0, 1.0f, 0));
        const std::span<const uint32_t> c = g.stamp_cells(1);
        return std::vector<uint32_t>(c.begin(), c.end());
    };
    auto same = [](std::span<const uint32_t> a, const std::vector<uint32_t>& b) { return std::equal(a.begin(), a.end(), b.begin(), b.end()); };
    bool invaded = false, checked = false, spawned_checked = false, removed_checked = false, died = false, asteroid_checked = false;
    objects::ObjectHandle asteroid_h{};
    objects::ObjectId asteroid = objects::kNoObjectId, prison_clone = objects::kNoObjectId, bunker_clone = objects::kNoObjectId;
    objects::ObjectId unit = objects::kNoObjectId, spawned = objects::kNoObjectId;
    objects::ObjectHandle spawned_h{};
    uint32_t spawn_frame = 0;
    for (uint64_t t = 0; t < 6000 && !(died && (space || removed_checked)); ++t) {
        host.tick_once();
        if (!space && !invaded && host.modes().depth() == 1) {
            const std::vector<uint32_t> f = gs->fleets_at(1, 1);
            if (!f.empty() && !gs->fleets()[f[0]].moving) invaded = gs->invade(1, 1, {}) == commands::RejectReason::None;
        }
        TacticalBattle* b = battles.battle();
        TacticalMode* tm = host.tactical();
        if (!b || !tm) continue;
        objects::ObjectStore& st = tm->primary();
        const mv::MovementSystem& ms = b->movement();
        if (!checked) {  // the first battle frame: the map objects own their placements' footprints
            checked = true;
            objects::ObjectId other = objects::kNoObjectId;
            const uint32_t unit_type = type_of(*d, space ? "S10_Obst_Mine" : "S10_Obst_Bunker");
            const uint32_t other_type = type_of(*d, space ? "S10_Obst_Asteroid" : "S10_Obst_Rock");
            for (const objects::ObjectHandle h : st.all_objects()) {
                if (!st.alive(h)) continue;
                if (static_cast<uint32_t>(st.type(h)) == unit_type) unit = st.id(h);
                if (static_cast<uint32_t>(st.type(h)) == other_type) other = st.id(h);
                CHECK(static_cast<uint32_t>(st.type(h)) != type_of(*d, space ? "S10_Obst_Asteroid_Spawner" : "S10_Obst_Rock_Spawner"));
            }
            CHECK(unit > 0 && other > 0);
            if (unit <= 0 || other <= 0) return;
            if (space) {
                const mv::SpaceOccupancy& occ = ms.space();
                mv::V2 c{};
                float r = 0.0f;
                uint32_t bits = 0;
                CHECK(!occ.has_static(mv::map_stamp_id(0)) && !occ.has_static(mv::map_stamp_id(1)));
                CHECK(occ.static_entry(unit, c, r, bits) && r == 120.0f && c.x == 2500.0f && c.y == 2500.0f);
                CHECK(occ.static_entry(other, c, r, bits) && r == 400.0f && c.x == -2500.0f && c.y == 2500.0f);  // not 50
                std::printf("  space: map unit %d entry r 120, spawned asteroid %d entry r %.0f\n", unit, other, static_cast<double>(r));
                CHECK(b->hyperspace_out(unit, 0, true));  // killed 90 frames later
                // S8 t09: the asteroid is no battle unit; removing it must drop its (re-keyed) entry at the next sync.
                asteroid = other;
                asteroid_h = st.find(other);
                CHECK(tm->destroy(asteroid_h));
            } else {
                const mv::PassabilityGrid& g = ms.grid();
                CHECK(!g.stamped(mv::map_stamp_id(0)) && !g.stamped(mv::map_stamp_id(1)));
                CHECK(g.stamped(unit) && same(g.stamp_cells(unit), cells_of(g, 450.0f, 620.0f, 60.0f)));
                CHECK(g.stamped(other) && same(g.stamp_cells(other), cells_of(g, 900.0f, 300.0f, 100.0f)));  // the rock's, not 20 x 20
                CHECK(g.effective(mv::world_to_cell(450.0f), mv::world_to_cell(620.0f)) == mv::kPassObstacle);
                std::vector<objects::ObjectId> attackers;
                for (const objects::ObjectHandle h : st.owned_by(1))
                    if (st.alive(h) && b->combat().has_unit(st.id(h)) && b->combat().alive(st.id(h))) attackers.push_back(st.id(h));
                CHECK(!attackers.empty());
                CHECK(b->order_attack(*tm, 1, attackers, unit, host.random().synced()) == commands::RejectReason::None);
                // A rock spawned between two ticks (script spawn path) is stamped by the next tick's end.
                objects::SpawnParams sp;
                sp.type = static_cast<objects::TypeIndex>(other_type);
                sp.owner = 2;
                sp.position = {300.0f, 1000.0f, 0.0f};
                sp.facing = {0.0f, -1.0f, 0.0f};
                spawned_h = tm->spawn(sp);
                spawned = st.id(spawned_h);
                spawn_frame = b->frame();
                CHECK(spawned > 0 && !g.stamped(spawned));
                // S8 t09: death clones of a bunker (Death_Clone_Is_Obstacle default true) and of a prison (false).
                sp.type = static_cast<objects::TypeIndex>(type_of(*d, "S10_Obst_Bunker"));
                sp.position = {1200.0f, 1000.0f, 0.0f};
                const objects::ObjectHandle bc = tm->spawn_death_clone(sp);
                sp.type = static_cast<objects::TypeIndex>(type_of(*d, "S10_Obst_Prison"));
                sp.position = {1200.0f, 600.0f, 0.0f};
                const objects::ObjectHandle pc = tm->spawn_death_clone(sp);
                bunker_clone = st.id(bc);
                prison_clone = st.id(pc);
                const auto list = [&st](objects::ObjectHandle h) {
                    return std::vector<objects::BehaviorTypeId>(st.behaviors(h).begin(), st.behaviors(h).end());
                };
                CHECK(st.dying(bc) && st.dying(pc));
                CHECK(list(bc) == (std::vector<objects::BehaviorTypeId>{objects::beh::LandObstacle, 0x44}));  // SELECTABLE gone
                CHECK(list(pc) == (std::vector<objects::BehaviorTypeId>{0x44}));  // LAND_OBSTACLE gone too
            }
            continue;
        }
        if (!space) {
            const mv::PassabilityGrid& g = ms.grid();
            if (!spawned_checked && b->frame() == spawn_frame + 1) {
                spawned_checked = true;
                CHECK(g.stamped(spawned) && same(g.stamp_cells(spawned), cells_of(g, 300.0f, 1000.0f, 100.0f)));
                CHECK(g.effective(mv::world_to_cell(300.0f), mv::world_to_cell(1000.0f)) == mv::kPassObstacle);
                // S8 t09: the bunker's clone kept LAND_OBSTACLE and is stamped; the prison's is not.
                CHECK(g.stamped(bunker_clone) && same(g.stamp_cells(bunker_clone), cells_of(g, 1200.0f, 1000.0f, 60.0f)));
                CHECK(!g.stamped(prison_clone) && g.effective(mv::world_to_cell(1200.0f), mv::world_to_cell(600.0f)) == mv::kPassClear);
                CHECK(tm->destroy(spawned_h));
            } else if (spawned_checked && !removed_checked) {
                removed_checked = true;
                CHECK(!g.stamped(spawned) && g.effective(mv::world_to_cell(300.0f), mv::world_to_cell(1000.0f)) == mv::kPassClear);
            }
            if (!died && !st.alive(st.find(unit))) {
                died = true;
                // The original's stamp goes; its death clone (no Death_Clone list: the bunker type itself, S9_DEATH_CLONE_SHIELDS
                // §1.6) keeps LAND_OBSTACLE (Death_Clone_Is_Obstacle, default true) and stamps the same cells (MOVEMENT §1.3 [H]).
                objects::ObjectId clone = objects::kNoObjectId;
                for (const objects::ObjectHandle h : st.all_objects())
                    if (st.alive(h) && st.dying(h) && static_cast<uint32_t>(st.type(h)) == type_of(*d, "S10_Obst_Bunker") &&
                        st.id(h) != bunker_clone)
                        clone = st.id(h);
                std::printf("  land: bunker %d died at frame %u (death clone %d), rock %d spawned at %u\n", unit, b->frame(), clone, spawned,
                            spawn_frame);
                CHECK(!g.stamped(unit) && clone > 0 && g.stamped(clone) && same(g.stamp_cells(clone), cells_of(g, 450.0f, 620.0f, 60.0f)));
            }
            if (!died) CHECK(g.stamped(unit));
        } else {
            if (!asteroid_checked && asteroid > 0) {
                asteroid_checked = true;
                CHECK(!st.exists(asteroid_h) && !ms.space().has_static(asteroid));
            }
            if (!died && !st.alive(st.find(unit))) {
                died = true;
                std::printf("  space: map unit %d died at frame %u\n", unit, b->frame());
                CHECK(!ms.space().has_static(unit) && ms.space().query_static({2500.0f, 2500.0f}, {2500.0f, 2500.0f}, 1.0f) == 0);
            }
            if (!died) CHECK(ms.space().has_static(unit));
        }
    }
    CHECK(checked && died);
    if (!space) CHECK(spawned_checked && removed_checked);
    if (space) CHECK(asteroid_checked);
}


// SP5 (docs/SIM_PARALLEL_DESIGN.md §5): the tick's services phase graph. Every run mode gives the default run's per-tick
// CRCs and fog digests (Serial = the declared order, Reverse = each phase backwards: an undeclared read or write between
// services of one phase would show here, Parallel at 4 workers), and the debug write check (every service's changes to
// the checkable components inside its declared writes) finds no violation.
void test_service_modes(const Data& d) {
    using tactical::services::RunMode;
    const TacticalBattle::ServiceRunDefaults saved = TacticalBattle::service_run_defaults();
    for (const bool land : {false, true}) {
        const uint64_t ticks = 1500;
        const Run ref = run_campaign(d, land, true, 0, ticks);
        CHECK(ref.fought == 1);
        struct Mode {
            RunMode mode;
            bool check;
            int32_t workers;
            const char* name;
        };
        const Mode modes[] = {{RunMode::Serial, false, 0, "serial"},
                              {RunMode::Reverse, false, 0, "reverse"},
                              {RunMode::Parallel, false, 4, "parallel w4"},
                              {RunMode::Serial, true, 4, "write check"}};
        for (const Mode& m : modes) {
            TacticalBattle::set_service_run_defaults({m.mode, m.check});
            const uint64_t v0 = TacticalBattle::service_violations_total();
            const Run r = run_campaign(d, land, true, m.workers, ticks);
            const auto dv = first_divergence(ref.crc, r.crc);
            const uint64_t violations = TacticalBattle::service_violations_total() - v0;
            std::printf("  %s %-12s %s, fog %s, violations %llu\n", land ? "land " : "space", m.name, dv.message.c_str(),
                        r.fog_hash == ref.fog_hash ? "same" : "DIFFERENT", static_cast<unsigned long long>(violations));
            CHECK(!dv.diverged && r.battles == ref.battles && r.fog_hash == ref.fog_hash);
            CHECK(violations == 0);
        }
        TacticalBattle::set_service_run_defaults(saved);
    }
}

}  // namespace


int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    g_temp_prefix = "alamo_sim_battle_tests";
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--regen") g_regen = true;
    auto d = make_data();
    std::printf("space battle\n");
    test_space_battle(*d);
    std::printf("land battle\n");
    test_land_battle(*d);
    std::printf("SP5 services graph: serial / reverse / parallel / write check\n");
    test_service_modes(*d);
    std::printf("retreat\n");
    test_retreat(*d);
    std::printf("auto-resolve only\n");
    test_auto_only(*d);
    std::printf("human orders + order queue (integration fixes, [port: Gabriel])\n");
    test_orders_and_queue(*d);
    std::printf("fog of war battles\n");
    {
        auto dfog = make_data(true);
        test_fog_battle(*dfog);
    }
    std::printf("fog of war: Battlefield_Modifier reveal multiplier (FOW t01)\n");
    {
        auto dbm = make_data(true, false, false, false, false, false, false, true);
        test_fog_bm_battle(*dbm);
    }
    std::printf("fog of war: land setup phase manager (FOWF t05)\n");
    {
        auto dsetup = make_data(false, false, false, false, false, false, false, false, false, true);
        test_fog_setup_battle(*dsetup);
    }
#ifdef ALAMO_SIM_HAVE_ANIM
    std::printf("anim battles (R3: flinch, death clips, corpses)\n");
    test_anim_battles();
    std::printf("death clone removal, headless and with the anim service (refixf t04)\n");
    test_death_clone_removal();
    std::printf("s78k t05: slot 47 tests the clip before the draw (animation world)\n");
    test_attack_idle_clip_order();
    test_presence_idle();  // s78l t03
#endif
    std::printf("black market gate (P4 tactical)\n");
    test_black_market_battle();
    std::printf("battle host BT pass: land split, HIJACK reserve, ResumeGame, map picker\n");
    test_bt_pass(*d);
    std::printf("S10 map owners: faction lookup, Hostile natives, attacker landing point [probe 2026-10-29]\n");
    {
        auto dmo = make_data(false, false, true);
        test_map_owners(*dmo);
        test_campaign_hostile(*dmo);
        std::printf("ccpgt t02: Capture Control Points (0x15 domination timer)\n");
        test_control_point_domination(*dmo, true);
        test_control_point_domination(*dmo, false);
    }
    std::printf("skirmish station production: Production at the star base, upgrade object, station upgrade\n");
    test_station_production();
    test_land_upgrade_production_done();  // s78n t20 step 5
    std::printf("cu8g t05: Battlefield_Modifier gates at the battle (skirmish vs campaign, empty list, apply / remove / reverse)\n");
    test_battlefield_modifier_gates();
    std::printf("a1g t04 follow-up: land company drop layout, oriented boxes, reinforcement radii\n");
    test_land_drop_rules();
    std::printf("S8 t02 / S7 t13: obstacle footprints follow their objects (land stamps, space static entries)\n");
    test_runtime_obstacles(false);
    test_remote_bomb_marker();  // S8 t10 follow-up
    test_runtime_obstacles(true);
    std::printf("t41: the final tick's deaths are in the battle's loss records\n");
    test_final_tick_losses(*d);
    std::printf("story t06: tactical victory monitor (hold, queued tests, ENABLE_VICTORY, VICTORY reward)\n");
    test_victory_monitor(*d);
    std::printf("OI objects: behaviour Initialise draws + scheduling\n");  // OI objects
    test_oi_objects_scheduling();
    test_foc();
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_sim_battle_tests: all passed\n");
    return 0;
}

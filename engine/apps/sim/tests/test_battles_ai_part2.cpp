// Part 2 of test_battles_ai (bo3 t03: split by position; see test_battles_ai_common.hpp).
#include "test_battles_ai_common.hpp"

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// `how` 0: the eject ability (unit ability 46 off: Take_Damage kind 13 on the vehicle, REMAINING_GAPS §5.1) - 1: lethal
// damage on the stolen vehicle. Either way the vehicle dies and the thief is released ALIVE where the vehicle stood, no
// longer garrisoned and a mover again. Before that: the stolen vehicle is no garrison (a second allied infantry unit is
// refused, there is no free slot) and the thief does not leave by the ordinary leave / eject-garrison orders.
void test_thief_release(const Data& d, int how) {
    ThiefScene t(d, "S10_Thief", true, true);
    CHECK(t.s.gs != nullptr);
    if (!t.s.gs || !t.start()) {
        CHECK(false);
        return;
    }
    CHECK(t.thief2 != objects::kNoObjectId);
    CHECK(t.order_theft() && t.wait_boarded());
    if (t.clone == objects::kNoObjectId) return;
    TacticalBattle& b = *t.b;
    const tc::CombatSystem& cs = b.combat();
    namespace tg = alamo::tactical::garrison;
    const tg::HolderDesc* hd = b.garrison().holder_desc(t.clone);
    CHECK(hd && hd->kind == tg::HolderKind::VehicleThief);
    CHECK(!cs.alive(t.speeder) && cs.owner(t.clone) == 1 && (cs.flags(t.thief) & tc::kUnitGarrisoned) != 0);
    CHECK(b.abilities().is_active(t.clone, t.eject) && b.abilities().is_active(t.thief, t.steal));
    // not a garrison for anyone else
    const objects::ObjectId other[] = {t.thief2}, self[] = {t.thief};
    CHECK(!b.can_garrison(t.thief2, t.clone) && b.garrison().free_slots(t.clone) == 0);
    {  // s78n t19: the refusal comes before any side effect - the unit's order queue, its move and the RNG are untouched
        SimRng& rng = t.s.host->random().synced();
        const Vec3 p2 = t.pos(t.thief2);
        CHECK(b.order_move(*t.tm, 1, other, p2.x + 60.0f, p2.y, rng) == commands::RejectReason::None);
        CHECK(b.order_move(*t.tm, 1, other, p2.x + 60.0f, p2.y + 60.0f, rng, mv::kNoFacing, false, true) == commands::RejectReason::None);
        const size_t queued0 = b.queued_orders(t.thief2).size();
        CHECK(queued0 == 1);
        const mv::UnitView v0 = b.movement().view(t.thief2);
        const uint64_t draws0 = rng.draws();
        CHECK(b.order_garrison(*t.tm, 1, other, t.clone, rng) == commands::RejectReason::InvalidTarget);
        const mv::UnitView v1 = b.movement().view(t.thief2);
        CHECK(rng.draws() == draws0 && b.queued_orders(t.thief2).size() == queued0);
        CHECK(v1.has_goal && v1.has_goal == v0.has_goal && v1.goal == v0.goal && v1.pos == v0.pos && v1.start_frame == v0.start_frame);
        CHECK(t.pos(t.thief2).x == p2.x && t.pos(t.thief2).y == p2.y);
    }
    CHECK(b.garrison().pending_holder(t.thief2) == objects::kNoObjectId);
    // no ordinary way out
    CHECK(b.order_leave_garrison(*t.tm, 1, self) != commands::RejectReason::None);
    CHECK(b.eject_garrison(*t.tm, t.clone) == 0u);
    for (int i = 0; i < 45; ++i) CHECK(t.tick());  // the eject ability's recharge (1 s) runs out meanwhile
    CHECK(t.b->garrison_holder_of(t.thief) == t.clone && t.b->garrison_holder_of(t.thief2) == objects::kNoObjectId);
    const Vec3 at = t.pos(t.clone);
    const float thief_health = t.b->combat().health(t.thief);
    if (how == 0) {
        const objects::ObjectId car[] = {t.clone};
        CHECK(t.b->abilities().set_unit_ability_mode(car, t.eject, false, t.b->frame()) == 1u);
    } else {
        CHECK(t.b->script_damage(*t.tm, t.clone, 1.0e9f, false));
    }
    int released_at = -1;
    for (int i = 0; i < 10 && released_at < 0; ++i) {
        CHECK(t.tick());
        if (t.b->garrison_holder_of(t.thief) == objects::kNoObjectId) released_at = i;
    }
    CHECK(released_at >= 0);
    const tc::CombatSystem& c2 = t.b->combat();
    const objects::ObjectStore& st = t.tm->primary();
    CHECK(!c2.alive(t.clone) && !st.alive(st.find(t.clone)));                                  // the vehicle is gone
    CHECK(c2.alive(t.thief) && st.alive(st.find(t.thief)) && c2.health(t.thief) == thief_health);  // the thief is not
    CHECK((c2.flags(t.thief) & tc::kUnitGarrisoned) == 0 && !t.b->garrison().is_holder(t.clone));
    CHECK(t.b->movement().has_unit(t.thief));
    const Vec3 p = t.pos(t.thief);
    std::printf("  thief release (%s): vehicle %d at (%.1f, %.1f), thief %d out %d tick(s) later at (%.1f, %.1f)\n",
                how == 0 ? "eject ability" : "vehicle destroyed", static_cast<int>(t.clone), static_cast<double>(at.x),
                static_cast<double>(at.y), static_cast<int>(t.thief), released_at + 1, static_cast<double>(p.x), static_cast<double>(p.y));
    CHECK(std::fabs(p.x - at.x) < 12.0f && std::fabs(p.y - at.y) < 12.0f);  // at the vehicle's place (it may have stepped since)
    // it is an ordinary unit again: it takes a move order and walks
    const objects::ObjectId one[] = {t.thief};
    CHECK(t.b->order_move(*t.tm, 1, one, p.x + 200.0f, p.y, t.s.host->random().synced()) == commands::RejectReason::None);
    for (int i = 0; i < 90; ++i) CHECK(t.tick());
    CHECK(c2.alive(t.thief) && std::fabs(t.pos(t.thief).x - p.x) > 5.0f);
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// Save / load with the thief inside the stolen vehicle. `no_garrison_unit`: the thief's type has no GARRISON_UNIT and
// nothing else in the battle touches the garrison system, so the theft is its first use: the loading session's rebuilt
// battle has no garrison / capture systems until the garrison part is read (tactical_battle_save.cpp). The loaded session
// runs on with the original's digests, then both eject: the thief is out, alive, in both.
void test_thief_save_load(const Data& d, bool no_garrison_unit) {
    const char* thief_name = no_garrison_unit ? "S10_ThiefN" : "S10_Thief";
    ThiefScene a(d, thief_name, false, true);
    CHECK(a.s.gs != nullptr);
    if (!a.s.gs || !a.start()) {
        CHECK(false);
        return;
    }
    if (no_garrison_unit) CHECK(!a.b->garrison().is_unit(a.thief) && !a.b->garrison().is_holder(a.speeder));  // unused so far
    CHECK(a.order_theft() && a.wait_boarded());
    if (a.clone == objects::kNoObjectId) return;
    for (int i = 0; i < 20; ++i) CHECK(a.tick());
    SaveFile save;
    std::string why;
    CHECK(a.s.host->save_allowed(&why));
    const bool saved = a.s.host->save(save, &why);
    if (!saved) std::printf("  thief save: %s\n", why.c_str());
    CHECK(saved);
    const uint64_t save_tick = a.s.host->frame();
    ThiefScene f(d, thief_name, false, false);
    CHECK(f.s.gs != nullptr);
    if (!saved || !f.s.gs) return;
    std::string err;
    LoadReport rep;
    const bool loaded = f.s.host->load(save, &err, &rep);
    if (!loaded) std::printf("  thief load: %s\n", rep.text().c_str());
    CHECK(loaded && f.s.host->frame() == save_tick);
    if (!loaded) return;
    f.s.invaded = true;
    f.thief = a.thief;
    f.clone = a.clone;
    uint32_t same = 0, compared = 0;
    const auto step = [&] {
        const bool ta_ok = a.tick(), tf_ok = f.tick();
        CHECK(ta_ok && tf_ok);
        if (!ta_ok || !tf_ok) return;
        ++compared;
        if (clash_digest(*a.b) == clash_digest(*f.b) && a.b->garrison_holder_of(a.thief) == f.b->garrison_holder_of(f.thief)) ++same;
    };
    for (int i = 0; i < 40; ++i) step();
    CHECK(f.b && f.b->garrison_holder_of(f.thief) == f.clone);
    namespace tg = alamo::tactical::garrison;
    if (f.b) {
        const tg::HolderDesc* hd = f.b->garrison().holder_desc(f.clone);
        CHECK(hd && hd->kind == tg::HolderKind::VehicleThief);
        const tg::OccupantDesc* od = f.b->garrison().unit_desc(f.thief);
        CHECK(od && (od->value == tg::kBoardOnlyValue) == no_garrison_unit);
    }
    const objects::ObjectId car[] = {a.clone};
    for (ThiefScene* x : {&a, &f})
        if (x->b) CHECK(x->b->abilities().set_unit_ability_mode(car, x->eject, false, x->b->frame()) == 1u);
    for (int i = 0; i < 30; ++i) step();
    for (ThiefScene* x : {&a, &f}) {
        if (!x->b) continue;
        CHECK(!x->b->combat().alive(x->clone) && x->b->combat().alive(x->thief));
        CHECK(x->b->garrison_holder_of(x->thief) == objects::kNoObjectId && x->b->movement().has_unit(x->thief));
    }
    std::printf("  thief save / load (%s): saved at %llu with the thief inside, %u of %u ticks after the load identical\n",
                thief_name, static_cast<unsigned long long>(save_tick), same, compared);
    CHECK(compared == 70 && same == compared);
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// s78n t19: a save in the window between a command-phase kill and the next battle tick. `how` 0: the eject ability
// kills the stolen vehicle (unit ability 46 off: Take_Damage kind 13) with the thief inside; 1: a STIM_PACK kills its
// user (S10_ThiefN: half its max health). The death is in the combat system's list and the host has not processed it:
// the dead unit's object is still in the store. The save is taken at once; the loaded session then runs on with the
// original's digests, and in both the object goes at the start of the next tick (how 0: the thief comes out alive).
void test_command_kill_save_load(const Data& d, int how) {
    const char* thief_name = how == 0 ? "S10_Thief" : "S10_ThiefN";
    ThiefScene a(d, thief_name, false, true);
    CHECK(a.s.gs != nullptr);
    if (!a.s.gs || !a.start()) {
        CHECK(false);
        return;
    }
    objects::ObjectId dead = objects::kNoObjectId;
    if (how == 0) {
        CHECK(a.order_theft() && a.wait_boarded());
        if (a.clone == objects::kNoObjectId) return;
        for (int i = 0; i < 45; ++i) CHECK(a.tick());  // the eject ability's recharge runs out
        const objects::ObjectId car[] = {a.clone};
        CHECK(a.b->abilities().set_unit_ability_mode(car, a.eject, false, a.b->frame()) == 1u);
        dead = a.clone;
    } else {
        for (int i = 0; i < 5; ++i) CHECK(a.tick());
        const uint8_t stim = ta::ability_type_from_name("STIM_PACK");
        CHECK(a.b->abilities().has_ability(a.thief, stim));
        CHECK(a.b->script_damage(*a.tm, a.thief, a.b->combat().health(a.thief) - 100.0f, false));  // 100 left of 5000
        CHECK(a.b->combat().alive(a.thief));
        const objects::ObjectId user[] = {a.thief};
        CHECK(a.b->abilities().set_unit_ability_mode(user, stim, true, a.b->frame()) == 1u);
        dead = a.thief;
    }
    {  // dead in combat, the host has not seen it yet
        const objects::ObjectStore& st = a.tm->primary();
        CHECK(!a.b->combat().alive(dead) && st.alive(st.find(dead)));
    }
    SaveFile save;
    std::string why;
    const bool allowed = a.s.host->save_allowed(&why);
    const bool saved = allowed && a.s.host->save(save, &why);
    if (!saved) std::printf("  command-kill save (%d): %s\n", how, why.c_str());
    CHECK(saved);
    const uint64_t save_tick = a.s.host->frame();
    ThiefScene f(d, thief_name, false, false);
    CHECK(f.s.gs != nullptr);
    if (!saved || !f.s.gs) return;
    std::string err;
    LoadReport rep;
    const bool loaded = f.s.host->load(save, &err, &rep);
    if (!loaded) std::printf("  command-kill load (%d): %s\n", how, rep.text().c_str());
    CHECK(loaded && f.s.host->frame() == save_tick);
    if (!loaded) return;
    f.s.invaded = true;
    f.thief = a.thief;
    f.clone = a.clone;
    uint32_t same = 0, compared = 0;
    for (int i = 0; i < 40; ++i) {
        const bool ta_ok = a.tick(), tf_ok = f.tick();
        CHECK(ta_ok && tf_ok);
        if (!ta_ok || !tf_ok) break;
        ++compared;
        if (clash_digest(*a.b) == clash_digest(*f.b)) ++same;
        if (i == 0)  // the first tick after the save processed the pending death in both sessions
            for (ThiefScene* x : {&a, &f}) {
                const objects::ObjectStore& st = x->tm->primary();
                CHECK(!st.alive(st.find(dead)));
            }
    }
    for (ThiefScene* x : {&a, &f}) {
        if (!x->b) continue;
        CHECK(!x->b->combat().alive(dead));
        if (how == 0) CHECK(x->b->combat().alive(x->thief) && x->b->garrison_holder_of(x->thief) == objects::kNoObjectId && x->b->movement().has_unit(x->thief));
    }
    std::printf("  command-phase kill save / load (%s): saved at %llu with the death pending, %u of %u ticks after the load identical\n",
                how == 0 ? "eject" : "STIM_PACK", static_cast<unsigned long long>(save_tick), same, compared);
    CHECK(compared == 40 && same == compared);
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// s78n t12 review: the thief dies in the tick of its own theft - after the apply (its unit service in combat phase D) and
// before the host pass that spawns the clone. The clone still replaces the vehicle, but nobody took it over: it stays the
// victim's owner's, without the stolen / incognito flags, with unit ability 46 off and nobody inside.
// How the tick is hit: the thief (1 health left) is put next to the Empire's troopers; a first session finds the tick in
// which their fire kills it, a second, identical one orders the theft `lead` tick(s) before that one.
void test_thief_dies_same_tick(const Data& d) {
    const uint32_t trooper_t = type_of(d, "S10_Trooper");
    // Runs the prelude and ticks on. `order_at` = the host frame before whose tick the theft is ordered (0: never).
    // Returns the host frame before the tick in which the thief died (0: it did not); `t` is left right after that tick.
    const auto run = [&](ThiefScene& t, uint64_t order_at) -> uint64_t {
        if (!t.s.gs || !t.start()) return 0;
        const tc::CombatSystem& cs = t.b->combat();
        const objects::ObjectStore& st = t.tm->primary();
        objects::ObjectId trooper = objects::kNoObjectId;
        for (size_t k = 0; k < cs.unit_count() && trooper == objects::kNoObjectId; ++k) {
            const objects::ObjectHandle h = st.find(cs.unit_id_at(k));
            if (st.alive(h) && st.type(h) == trooper_t && st.owner(h) == 0) trooper = cs.unit_id_at(k);
        }
        if (trooper == objects::kNoObjectId) return 0;
        const Vec3 tp = t.pos(trooper);
        if (!t.b->teleport(*t.tm, t.thief, Vec3{tp.x + 40.0f, tp.y, tp.z}, Vec3{-1.0f, 0.0f, 0.0f})) return 0;
        if (!t.b->script_damage(*t.tm, t.thief, cs.health(t.thief) - 1.0f, false)) return 0;
        for (int i = 0; i < 900; ++i) {
            const uint64_t before = t.s.host->frame();
            if (order_at != 0 && before == order_at) (void)t.order_theft();
            if (!t.tick()) return 0;
            if (!t.b->combat().alive(t.thief)) return before;
        }
        return 0;
    };
    ThiefScene probe(d, "S10_Thief", false, true);
    const uint64_t died = run(probe, 0);
    CHECK(died != 0);
    if (died == 0) return;
    bool hit = false;
    for (uint64_t lead = 1; lead <= 3 && !hit; ++lead) {
        ThiefScene t(d, "S10_Thief", false, true);
        const uint64_t died2 = run(t, died + 1 - lead);
        if (died2 == 0 || !t.b) continue;
        const tc::CombatSystem& cs = t.b->combat();
        const objects::ObjectId clone = t.s.find(*t.b, t.clone_t);
        if (died2 != died || clone == objects::kNoObjectId) continue;  // the apply came in another tick: next lead
        hit = true;
        const objects::ObjectStore& st = t.tm->primary();
        CHECK(!cs.alive(t.thief) && !cs.alive(t.speeder));                                    // both gone in that tick
        CHECK(cs.alive(clone) && cs.owner(clone) == 0 && st.owner(st.find(clone)) == 0);      // the victim's owner's
        CHECK((cs.flags(clone) & (tc::kUnitStolenVehicle | tc::kUnitIncognito)) == 0);
        CHECK(t.b->abilities().has_ability(clone, t.eject) && !t.b->abilities().is_active(clone, t.eject));
        CHECK(!t.b->garrison().is_holder(clone) && t.b->garrison_holder_of(t.thief) == objects::kNoObjectId);
        for (int i = 0; i < 30; ++i) CHECK(t.tick());
        CHECK(t.b->combat().alive(clone) && t.b->combat().owner(clone) == 0 && t.b->garrisoned_units(clone).empty());
        std::printf("  thief dies in its theft's tick (host frame %llu, theft ordered %llu tick(s) before): clone %d stays with "
                    "player 0\n",
                    static_cast<unsigned long long>(died), static_cast<unsigned long long>(lead), static_cast<int>(clone));
    }
    CHECK(hit);
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

void test_hero_clash_battle(const Data& d) {
    const HeroClashRun a = run_hero_clash_battle(d, 0, true);
    std::printf("  hero clash: start frame %u, end frame %u, %u exchanges, health A %.1f B %.1f; saved at %u, %u ticks after the "
                "load %s\n",
                a.start_frame, a.end_frame, a.exchanges, static_cast<double>(a.health_a), static_cast<double>(a.health_b),
                a.save_tick, a.compared, a.load_same ? "identical" : "DIFFER");
    CHECK(a.started && a.ended && a.flagged && a.locked && a.untargeted && a.released);
    CHECK(a.exchanges >= 2);               // 500 health against 188 / 75 per lost exchange: never one exchange
    CHECK(a.hunt_locked && a.hunt_after);  // review finding 3: the abilities' lock test sees the clash
    CHECK(a.save_ok && a.load_ok && a.load_same && a.compared > 0);  // review finding 7: mid-fight save / load
    const HeroClashRun q = run_hero_clash_battle(d, 0, false, true);
    std::printf("  hero clash with queued moves: start frame %u, end frame %u; queue dropped %d, moved to a queued point %d\n",
                q.start_frame, q.end_frame, q.queue_dropped ? 1 : 0, q.queue_moved ? 1 : 0);
    CHECK(q.queued && q.started && q.ended && q.locked && q.queue_dropped && !q.queue_moved);  // review finding 1
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// --hero-clash mode (the worker-count check, alamo_add_worker_count_check in engine/apps/CMakeLists.txt): one run of the
// hero-clash battle on `workers`; --crc-out writes its per-tick digests (one hex value per line), --golden compares.
int hero_clash_worker_check(int argc, char** argv) {
    uint32_t workers = 0;
    std::string out_path, golden_path;
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--workers") workers = static_cast<uint32_t>(std::strtoul(argv[i + 1], nullptr, 10));
        else if (a == "--crc-out") out_path = argv[i + 1];
        else if (a == "--golden") golden_path = argv[i + 1];
    }
    static std::string prefix;  // a fixture folder per worker count (the entries run side by side under ctest -j)
    prefix = "alamo_sim_battle_ai_clash_w" + std::to_string(workers);
    g_temp_prefix = prefix.c_str();
    auto d = make_data(false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
    const HeroClashRun r = run_hero_clash_battle(*d, workers);
    CHECK(r.started && r.ended && r.released && !r.digests.empty());
    std::printf("hero clash on %u workers: %zu ticks, end frame %u\n", workers, r.digests.size(), r.end_frame);
    if (!out_path.empty()) {
        std::ofstream f(out_path, std::ios::binary | std::ios::trunc);
        for (const uint64_t v : r.digests) f << std::hex << v << "\n";
        CHECK(static_cast<bool>(f));
    }
    if (!golden_path.empty()) {
        std::ifstream f(golden_path, std::ios::binary);
        std::vector<uint64_t> want;
        uint64_t v = 0;
        while (f >> std::hex >> v) want.push_back(v);
        CHECK(!want.empty());
        size_t first = 0;
        while (first < want.size() && first < r.digests.size() && want[first] == r.digests[first]) ++first;
        if (want != r.digests) {
            std::printf("hero clash on %u workers: digests differ from the 0-worker run at tick %zu (%zu vs %zu ticks)\n", workers,
                        first, r.digests.size(), want.size());
            ++g_fail;
        }
    }
    std::error_code ec;
    remove_all_long(d->root, ec);
    if (g_fail) {
        std::printf("%d check(s) FAILED\n", g_fail);
        return 1;
    }
    return 0;
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

// s78n t06 review finding 2 (HERO_CLASH §2.2 [port]): a garrisoned hero (S10_HeroC, a GARRISON_UNIT, inside a one-slot
// garrison vehicle of its own side) is no clash candidate; once out, the clash starts; a garrison order in progress at the
// clash start (approach or entry fade) is cancelled (Begin leaves the coordinators), and a garrison order for a clashing hero is refused, so
// the hero is never garrisoned while it duels. The human's units are left to the test (skip_human_units); the clash data
// comes on once the hero is inside.
void test_hero_clash_garrison(const Data& d) {
    const uint32_t c_t = type_of(d, "S10_HeroC"), b_t = type_of(d, "S10_HeroB"), v_t = type_of(d, "S10_Speeder_Stolen");
    CHECK(c_t < d.types.size() && b_t < d.types.size() && v_t < d.types.size());
    const auto data = hero_clash_data("S10_HeroC", "S10_HeroB");
    data->entries[0].damage_percentage = 0.0f;  // no exchange damage: the clash runs until the test is done
    CampaignBattleOptions opt;
    opt.conflict.human_choice = galactic::BattleChoice::Fight;
    opt.battle.max_frames = 3000;
    opt.battle.skip_human_units = true;
    ClashSession s(d, 0, opt, {"S10_HeroC_Co", "S10_HeroHold_Co"}, {"S10_HeroB_Co"}, true);
    if (!s.gs) return;
    using R = commands::RejectReason;
    int stage = 0;
    uint32_t stage_frame = 0;
    objects::ObjectId hc = objects::kNoObjectId, hold = objects::kNoObjectId;
    bool inside_no_clash = true, never_in_while_flagged = true, refused = false, approach_cancelled = false;
    auto in_clash = [](const TacticalBattle& b, objects::ObjectId id) {
        return b.combat().has_unit(id) && (b.combat().flags(id) & tc::kUnitHeroClash) != 0;
    };
    for (uint64_t t = 0; t < 8000 && stage < 5; ++t) {
        TacticalBattle* b = s.tick();
        TacticalMode* tm = s.host->tactical();
        if (!b || !tm || b->frame() < 1) {
            if (stage > 0) break;
            continue;
        }
        const objects::ObjectStore& st = tm->primary();
        const tc::HeroClashManager& m = b->hero_clashes();
        SimRng& rng = s.host->random().synced();
        if (stage == 0) {  // order the hero into the vehicle
            hc = s.find(*b, c_t);
            hold = s.find(*b, v_t);
            CHECK(hc != objects::kNoObjectId && s.find(*b, b_t) != objects::kNoObjectId && hold != objects::kNoObjectId);
            if (hc == objects::kNoObjectId || hold == objects::kNoObjectId) break;
        }
        const objects::ObjectId one[1] = {hc};
        const auto owner = static_cast<uint8_t>(st.owner(st.find(hc)));
        if (in_clash(*b, hc) && b->garrison_holder_of(hc) != objects::kNoObjectId) never_in_while_flagged = false;
        if (stage == 0) {
            CHECK(b->order_garrison(*tm, owner, one, hold, rng) == R::None);
            stage = 1;
            stage_frame = b->frame();
        } else if (stage == 1) {  // inside: the clash data on, then three detection frames
            if (b->garrison_holder_of(hc) == hold) {
                b->set_hero_clash_data(data);
                stage = 2;
                stage_frame = b->frame();
            } else if (b->frame() > stage_frame + 600) {
                CHECK(false);  // never got in
                break;
            }
        } else if (stage == 2) {
            if (!m.empty()) inside_no_clash = false;
            if (b->frame() >= stage_frame + 46) {
                // out (the leave is immediate) and at once back toward the vehicle: its 30-frame entry fade covers the
                // next detection frame
                CHECK(b->order_leave_garrison(*tm, owner, one) == R::None && b->garrison_holder_of(hc) == objects::kNoObjectId);
                CHECK(m.empty() && b->order_garrison(*tm, owner, one, hold, rng) == R::None);
                stage = 4;
                stage_frame = b->frame();
            }
        } else if (stage == 4) {  // the clash starts within 15 frames, while the hero walks back or fades in
            if (in_clash(*b, hc)) {
                approach_cancelled = !b->garrison().entering(hc) && b->garrison_holder_of(hc) == objects::kNoObjectId;
                refused = b->order_garrison(*tm, owner, one, hold, rng) == R::InvalidUnit;
                stage = 5;
            } else if (b->frame() > stage_frame + 40) {
                CHECK(false);  // no clash
                break;
            }
        }
    }
    for (int k = 0; k < 60 && stage == 5; ++k) {  // a few more ticks of the duel: still outside
        TacticalBattle* b = s.tick();
        if (!b) break;
        if (in_clash(*b, hc) && b->garrison_holder_of(hc) != objects::kNoObjectId) never_in_while_flagged = false;
    }
    std::printf("  hero clash garrison: stage %d, no clash while inside %d, approach cancelled %d, order refused %d, never inside "
                "while duelling %d\n",
                stage, inside_no_clash ? 1 : 0, approach_cancelled ? 1 : 0, refused ? 1 : 0, never_in_while_flagged ? 1 : 0);
    CHECK(stage == 5 && inside_no_clash && approach_cancelled && refused && never_in_while_flagged);
}
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
#endif
}  // namespace bo3split

namespace bo3split {
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI

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
}  // namespace bo3split

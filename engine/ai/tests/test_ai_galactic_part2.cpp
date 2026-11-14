// Part 2 of test_ai_galactic (bo3 t03: split by position; see test_ai_galactic_common.hpp).
#include "test_ai_galactic_common.hpp"

// a2m t02 (A2K_T08_RE_ANSWERS Q2.3 step 4 TRANSPORTS_TO_ORBIT, A2L_T01_RE_ANSWERS Q8.3) [H state; M name]: Produce_Force
// to the stage launches a member that is on the stage's surface and merges it into the FleetObject (DO_TASK_FORCE_MERGE),
// so the following Move_To, which moves the FleetObject only, takes it along. Without the launch the infantry would
// stay on P2's surface and Move_To would leave it behind (the a2l saboteur break: corruption -1 instead of 6).
void test_produce_force_stage_launch() {
    ParityWorld pw(false, {"A2mStage"}, own_p2);  // P2 held: Conquer_Planet @P3 stages at P2, where an infantry waits
    lua::GlobalValueStore gv;
    pw.world->set_global_values(&gv);
    auto num = [&gv](const char* n) { const lua::AiGlobalValue v = gv.get(n); return v.kind == lua::AiGlobalValue::Number ? v.number : -1.0; };
    std::vector<std::pair<objects::ObjectHandle, int64_t>> infantry;  // unit, the planet whose surface it starts on
    for (uint32_t p = 0; p < pw.sim->planets().size(); ++p)
        for (const objects::ObjectHandle u : pw.sim->planets()[p].surface)
            if (pw.store->alive(u) && pw.store->type(u) == tInfantry && pw.sim->planets()[p].owner == 0)
                infantry.emplace_back(u, static_cast<int64_t>(p));
    objects::ObjectHandle hero;
    for (const gal::FleetState& f : pw.sim->fleets())
        if (f.alive && f.owner == 0)
            for (const objects::ObjectHandle u : f.contents)
                if (pw.store->alive(u) && pw.store->type(u) == pw.hero) hero = u;
    CHECK(static_cast<bool>(hero) && !infantry.empty());
    int32_t frame = 1;
    auto step = [&pw, &frame]() {
        pw.sim->update();
        pw.world->update_player(0, frame, true, false);
        ++frame;
    };
    while (frame <= 30 * 400 && num("a2m_s_produced") < 0.0) step();
    CHECK(num("a2m_s_produced") == 1.0);
    // The claimed infantry is the one in the hero's fleet now.
    const uint32_t hf = pw.sim->fleet_containing(hero);
    CHECK(hf != gal::kNoFleet);
    if (hf == gal::kNoFleet) return;
    const int64_t stage = static_cast<int64_t>(pw.sim->fleets()[hf].planet);
    objects::ObjectHandle inf;
    int64_t inf_from = -1;
    for (const auto& [u, p] : infantry)
        if (pw.sim->fleet_containing(u) == hf) {
            inf = u;
            inf_from = p;
        }
    const uint32_t fo = pw.world->task_force_fleet_object(0, "MainForce");
    std::printf("produce_force stage launch: stage P%lld, infantry from P%lld's surface in the FleetObject %s (fleet %u, "
                "FleetObject %u)\n",
                static_cast<long long>(stage), static_cast<long long>(inf_from), inf ? "yes" : "no", hf, fo);
    CHECK(!pw.sim->fleets()[hf].moving && stage == 2);
    CHECK(static_cast<bool>(inf) && inf_from == stage);  // launched from the stage's surface, merged with the hero
    CHECK(fo == hf);                                     // into the FleetObject
    if (!inf) return;
    lua::AiGlobalValue go;
    go.kind = lua::AiGlobalValue::Number;
    go.number = 1.0;
    gv.set("a2m_s_go", go);
    while (frame <= 30 * 800 && num("a2m_s_done") < 0.0) step();
    const uint32_t hf2 = pw.sim->fleet_containing(hero), if2 = pw.sim->fleet_containing(inf);
    const int64_t hero_at = hf2 == gal::kNoFleet ? -1 : static_cast<int64_t>(pw.sim->fleets()[hf2].planet);
    std::printf("produce_force stage launch: Move_To done %.0f, hero at P%lld, infantry in its fleet %s\n", num("a2m_s_done"),
                static_cast<long long>(hero_at), hf2 == if2 ? "yes" : "no");
    CHECK(num("a2m_s_done") == 1.0);
    CHECK(hf2 != gal::kNoFleet && hf2 == if2 && hero_at != stage);  // the Move_To took the launched infantry along
    pw.world->set_global_values(nullptr);
}

// a2m t05 (A2M_T04_RE_ANSWERS Q6) [H]: Land_Units lands a transport (port: a ground-kind fleet member) at the player's
// own planet, refuses to land it anywhere else, and with the surface count == Max_Ground_Forces_On_Planet lands nothing
// (The over-full case, count > max, passes Land_Units' == test, but the sim's land order then refuses the unit, so it
// is not observable end to end.)
void test_land_units_owner_rule() {
    struct Result {
        bool home_status = false, away_status = false, done = false;
        int64_t home_landed = 0;   // frames the infantry spent on P0's surface after the first launch
        int64_t end_planet = -1;   // the infantry's planet at the end (fleet at a planet)
        bool end_in_fleet = false;
    };
    const auto run_case = [](int32_t max_forces) {
        Result r;
        ParityWorld pw(false, {"A2mLand"},
                       [](gal::GalacticSetup& s) {
                           for (gal::StartingForce& f : s.forces)
                               if (f.player == 0 && f.type == tInfantry) f.planet = 0;  // with the hero at P0
                       },
                       [max_forces](gal::GalacticRules& rr) { rr.constants.max_ground_forces_on_planet = max_forces; });
        lua::GlobalValueStore gv;
        pw.world->set_global_values(&gv);
        auto num = [&gv](const char* n) { const lua::AiGlobalValue v = gv.get(n); return v.kind == lua::AiGlobalValue::Number ? v.number : -1.0; };
        objects::ObjectHandle inf;
        for (const objects::ObjectHandle u : pw.sim->planets()[0].surface)
            if (pw.store->type(u) == tInfantry) inf = u;
        CHECK(static_cast<bool>(inf));
        for (int32_t f = 1; f <= 30 * 400 && num("a2m_l_done") < 0.0; ++f) {
            pw.sim->update();
            pw.world->update_player(0, f, true, false);
            take_when_ready(pw, gv);
            if (num("a2m_l_launched") >= 0.0 && num("a2m_l_landed") < 0.0) {
                const auto& surf = pw.sim->planets()[0].surface;
                r.home_landed += std::find(surf.begin(), surf.end(), inf) != surf.end() ? 1 : 0;
            }
        }
        r.home_status = num("a2m_l_home") == 1.0;
        r.away_status = num("a2m_l_away") == 1.0;
        r.done = num("a2m_l_done") == 1.0;
        const uint32_t fl = pw.sim->fleet_containing(inf);
        r.end_in_fleet = fl != gal::kNoFleet && !pw.sim->fleets()[fl].moving;
        r.end_planet = r.end_in_fleet ? static_cast<int64_t>(pw.sim->fleets()[fl].planet) : -1;
        pw.world->set_global_values(nullptr);
        return r;
    };
    // P0's surface holds nothing else of the Empire's ground forces once the infantry launched: count 0.
    const Result open = run_case(10), full = run_case(0);
    std::printf("land units: open home %d landed %lld away %d end planet %lld in fleet %d; full landed %lld\n",
                open.home_status ? 1 : 0, static_cast<long long>(open.home_landed), open.away_status ? 1 : 0,
                static_cast<long long>(open.end_planet), open.end_in_fleet ? 1 : 0, static_cast<long long>(full.home_landed));
    CHECK(open.done && open.home_status && open.away_status);
    CHECK(open.home_landed > 0);                                       // friendly P0: landed
    CHECK(open.end_in_fleet && open.end_planet != 0 && open.end_planet >= 0);  // the target (not the Empire's): not landed
    CHECK(full.home_status && full.home_landed == 0);                  // count 0 == max 0: nothing lands
}

// a2m t10 (A2M_T08_T10_T14_RE t10.3 / t10.8) [H]: the TF base handler raises No_Units_Remaining on every member loss
// that leaves no counted entry (not a named / generic hero, not a space special weapon, no lobbing superweapon). The hero
// left alone after the infantry dies gets the event (the old "every unit lost" rule did not raise it); the hero's own
// loss raises it again.
// a2m r2 (A2M_R2_RE_B §1.1-1.5) [H]: a build task that ends while its unit is in production, not taken by the PAYG reclaim
// (here: the plan ended through its own script, so no goal-system release ran), is cancelled at the producing planet's
// next production service: the head entry leaves the queue, the AI owner gets the paid fraction back, and no corvette is
// ever made.
// a2m t03 (A2M_T03_PRODUCE_FORCE_CLAIMS) [H]: instantiation leaves the selected hero on the free store, reserved by
// the PotentialPlan; plain Produce_Force() claims it, gives it straight back unreserved (it never joins the task force)
// and admits the produced corvette to the free store; the thread resumes when every task has ended.
void test_plain_produce_hands_back() {
    ParityWorld pw(false, {"A2mT03Plain"}, own_p2);  // P2 held: Conquer_Planet @P3 stages at P2
    lua::GlobalValueStore gv;
    pw.world->set_global_values(&gv);
    auto num = [&gv](const char* n) { const lua::AiGlobalValue v = gv.get(n); return v.kind == lua::AiGlobalValue::Number ? v.number : -1.0; };
    objects::ObjectHandle hero;
    for (const gal::FleetState& fl : pw.sim->fleets())
        if (fl.alive && fl.owner == 0)
            for (const objects::ObjectHandle u : fl.contents)
                if (pw.store->alive(u) && pw.store->type(u) == pw.hero) hero = u;
    CHECK(static_cast<bool>(hero));
    bool reserved_seen = false;
    for (int32_t f = 1; f <= 30 * 300 && num("t03_done") < 0.0; ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        reserved_seen = reserved_seen || (num("t03_count0") >= 0.0 && num("t03_done") < 0.0 && pw.world->free_store_owner(hero) >= 0);
    }
    uint32_t corvettes = 0;
    for (const gal::FleetState& fl : pw.sim->fleets())
        if (fl.alive && fl.owner == 0)
            for (const objects::ObjectHandle u : fl.contents) corvettes += pw.store->alive(u) && pw.store->type(u) == tCorvette ? 1u : 0u;
    const int32_t owner = hero ? pw.world->free_store_owner(hero) : -2;
    std::printf("plain produce hand-back: members %.0f -> %.0f, all on free store %.0f, done %.0f, reserved while blocked %d, "
                "hero owner %d, corvettes %u\n",
                num("t03_count0"), num("t03_count1"), num("t03_all"), num("t03_done"), reserved_seen ? 1 : 0, owner, corvettes);
    CHECK(num("t03_count0") == 0.0 && num("t03_all") == 0.0);   // no members; a unit to produce
    CHECK(num("t03_done") == 1.0 && num("t03_count1") == 0.0);  // returned with no member
    CHECK(hero && pw.store->alive(hero) && owner == -1);         // back on the store, unreserved
    CHECK(corvettes == 1);                                       // produced into the free store
    if (num("t03_done") != 1.0 || corvettes != 1)
        for (const AiDecision& d : pw.world->decision_log()) std::printf("    %s\n", d.text.c_str());
    pw.world->set_global_values(nullptr);
}

void test_ended_production_cancel() {
    ParityWorld pw(false, {"A2mCancel"}, own_p2);  // P2 held: Conquer_Planet @P3 stages at P2
    lua::GlobalValueStore gv;
    pw.world->set_global_values(&gv);
    auto num = [&gv](const char* n) { const lua::AiGlobalValue v = gv.get(n); return v.kind == lua::AiGlobalValue::Number ? v.number : -1.0; };
    const auto corvette_queued = [&pw] {
        for (uint32_t q = 0; q < 2; ++q)
            for (const gal::BuildEntry& e : pw.sim->queue_of(0, q))
                if (e.type == tCorvette && e.player == 0) return true;
        return false;
    };
    const auto corvettes = [&pw] {
        uint32_t n = 0;
        for (const gal::FleetState& fl : pw.sim->fleets())
            if (fl.alive && fl.owner == 0)
                for (const objects::ObjectHandle u : fl.contents) n += pw.store->alive(u) && pw.store->type(u) == tCorvette ? 1u : 0u;
        return n;
    };
    bool produced = false;
    int32_t f = 1;
    for (; f <= 30 * 120 && num("a2m_c_exit") < 0.0; ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        produced = produced || corvette_queued();
    }
    const bool queued_at_exit = corvette_queued();
    for (const int32_t e = f + 30 * 30; f < e; ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
    }
    bool cancelled = false;
    for (const AiDecision& d : pw.world->decision_log())
        cancelled = cancelled || d.text.find("cancel ended build Corvette at P0 (refund)") != std::string::npos;
    std::printf("ended production cancel: queued %d, queued at exit %d, cancelled %d, corvettes %u\n", produced ? 1 : 0,
                queued_at_exit ? 1 : 0, cancelled ? 1 : 0, corvettes());
    CHECK(num("a2m_c_exit") == 1.0 && produced && queued_at_exit);
    CHECK(cancelled && !corvette_queued() && corvettes() == 0);
    if (!cancelled)
        for (const AiDecision& d : pw.world->decision_log()) std::printf("    %s\n", d.text.c_str());
    pw.world->set_global_values(nullptr);
}

void test_no_units_remaining_counted_entries() {
    ParityWorld pw(false, {"A2mNur"}, [](gal::GalacticSetup& s) {
        for (gal::StartingForce& f : s.forces)
            if (f.player == 0 && f.type == tInfantry) f.planet = 0;  // with the hero at P0
    });
    lua::GlobalValueStore gv;
    pw.world->set_global_values(&gv);
    auto num = [&gv](const char* n) { const lua::AiGlobalValue v = gv.get(n); return v.kind == lua::AiGlobalValue::Number ? v.number : -1.0; };
    objects::ObjectHandle inf, hero;
    for (const objects::ObjectHandle u : pw.sim->planets()[0].surface)
        if (pw.store->type(u) == tInfantry) inf = u;
    CHECK(static_cast<bool>(inf));
    int32_t f = 1;
    auto step = [&](int32_t frames) {
        for (const int32_t e = f + frames; f < e; ++f) {
            pw.sim->update();
            pw.world->update_player(0, f, true, false);
            take_when_ready(pw, gv);
        }
    };
    while (f <= 30 * 120 && num("a2m_n_started") < 0.0) step(1);
    const double started = num("a2m_n_started");
    for (objects::ObjectId id = 0; !hero && id < 4096; ++id) {  // the hero ship: the other member, by type
        const objects::ObjectHandle h = pw.store->find(id);
        if (h && pw.store->alive(h) && pw.store->type(h) == pw.hero && pw.store->owner(h) == 0) hero = h;
    }
    CHECK(static_cast<bool>(hero));
    step(30);
    const double before = num("a2m_nur");
    CHECK(pw.sim->destroy_object(inf));
    step(10);
    const double after_inf = num("a2m_nur");
    CHECK(pw.sim->destroy_object(hero));
    step(10);
    const double after_hero = num("a2m_nur");
    bool logged = false;
    for (const AiDecision& d : pw.world->decision_log())
        logged = logged || d.text.find("MainForce no units remaining (1 uncounted)") != std::string::npos;
    std::printf("no units remaining: started with %.0f units, events before %.0f, after infantry %.0f, after hero %.0f, logged %d\n",
                started, before, after_inf, after_hero, logged ? 1 : 0);
    CHECK(started == 2.0 && before < 0.0 && after_inf == 1.0 && after_hero == 2.0 && logged);
    pw.world->set_global_values(nullptr);
}

// OI2 ai: production-disabled types (story DISABLE_BUILDABLE, REMAINING_GAPS §1.5) are never picked by AI production -
// neither by a normal plan's Production marshaller nor by a MagicPlan spawn nor by the team picks; and the build
// market's delivery path class (REMAINING_GAPS §6.2): 2 by default, 3 with +0xDA, 5 for Is_Stealth_Company.
void test_production_disabled_and_delivery() {
    {
        ParityWorld pw(false, {"HeroAttach"}, own_p2);
        pw.run(2);
        bool ok = false;
        const std::vector<uint32_t> a = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(a, tCorvette));
        CHECK(galactic_ai_player_can_build(*pw.sim, 0, tCorvette));
        pw.sim->set_production_disabled(0, tCorvette, true);
        CHECK(!galactic_ai_player_can_build(*pw.sim, 0, tCorvette));
        CHECK(galactic_ai_player_can_build(*pw.sim, 0, tFrigate));
        const std::vector<uint32_t> d = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        std::printf("production disabled: Corvette %s -> %zu selected, corvette %s\n", ok ? "solved" : "failed", d.size(),
                    pw.has(d, tCorvette) ? "yes" : "no");
        CHECK(!pw.has(d, tCorvette));
        pw.sim->set_production_disabled(0, tCorvette, false);  // ENABLE_BUILDABLE
        const std::vector<uint32_t> e = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(e, tCorvette));
    }
    {
        // MagicPlan: the production-disabled list (0x282510) and the locked list (0x282550) veto the spawn [H].
        for (int variant = 0; variant < 2; ++variant) {
            ParityWorld pw(false, {"MagicSpace"});
            if (variant == 0) pw.sim->set_production_disabled(0, tCorvette, true);
            else pw.sim->lock_type(0, tCorvette, true);
            pw.run(30 * 60);
            bool spawned = false;
            for (const AiDecision& d : pw.world->decision_log()) spawned = spawned || d.text.find("magic spawn Corvette") != std::string::npos;
            std::printf("production disabled: magic plan with Corvette %s -> spawned %s\n", variant == 0 ? "disabled" : "locked",
                        spawned ? "yes" : "no");
            CHECK(!spawned);
        }
    }
    {
        CHECK(galactic_ai_delivery_mode(false, false) == static_cast<int>(Reachability::EnemyDestination));
        CHECK(galactic_ai_delivery_mode(false, true) == static_cast<int>(Reachability::EnemyUndefended));
        CHECK(galactic_ai_delivery_mode(true, false) == static_cast<int>(Reachability::Any));
        CHECK(galactic_ai_delivery_mode(true, true) == static_cast<int>(Reachability::Any));
        // The chain P0..P7: the Empire holds P0 / P1, the Rebels P6 / P7, P2..P5 nobody. Class 2 stops at the first
        // unowned planet (reached, not expanded); class 5 (stealth companies) crosses unowned planets too.
        ParityWorld pw(false, {"HeroAttach"});
        pw.run(2);
        const std::vector<uint8_t> m2 = galactic_ai_delivery_flood(*pw.sim, 0, 0, galactic_ai_delivery_mode(false, false));
        const std::vector<uint8_t> m5 = galactic_ai_delivery_flood(*pw.sim, 0, 0, galactic_ai_delivery_mode(true, false));
        std::string a, b;
        for (size_t i = 0; i < m2.size(); ++i) {
            a += m2[i] ? '1' : '0';
            b += m5[i] ? '1' : '0';
        }
        std::printf("delivery floods from P0: class 2 %s, class 5 %s\n", a.c_str(), b.c_str());
        CHECK(m2.size() == 8 && m5.size() == 8);
        CHECK(m2[0] && m2[1] && !m2[4] && !m2[7]);
        for (size_t i = 0; i < m5.size(); ++i) CHECK(m5[i] != 0);
        CHECK(galactic_ai_delivery_flood(*pw.sim, 0, kInvalidPlanet, 2) == std::vector<uint8_t>(8, 0));
        // Request flag 1 is (m == 5) (FIXPASS_GALACTIC §1.4): in class 2 the Rebels' flood from P0 reaches the Empire's P1
        // (flag 4, destination) but does not cross it; class 5 does.
        const std::vector<uint8_t> r2 = galactic_ai_delivery_flood(*pw.sim, 1, 0, galactic_ai_delivery_mode(false, false));
        CHECK(r2.size() == 8 && r2[0] && r2[1] && !r2[2] && !r2[3]);
        const std::vector<uint8_t> r5 = galactic_ai_delivery_flood(*pw.sim, 1, 0, galactic_ai_delivery_mode(true, false));
        CHECK(r5.size() == 8 && r5[1] && r5[2] && r5[3]);
    }
}

// A2 f t01 (1): ProductionM's ground-structure slot check is used slots 0x3f8af0 (ground structures, 0 without a
// ground base) + queued < the planet type's Special_Structures_Land (0x372740, D+0x230), not the map's legacy
// <Max_Ground_Base>; plus the per-planet cap Build_Max_Instances_Per_Planet (0x3f94f0). The same slot max feeds the
// MaxStructureSlots / OpenStructureSlots tokens. P0 (the Empire's) holds SB1 + one Barracks.
void test_ground_structure_slots() {
    const std::vector<std::string> only = {"BuildBarracks"};
    const auto map_ground = [](int32_t v) {
        return [v](gal::GalacticSetup& s) {
            for (auto& m : s.map.planets.max_ground_base) m = v;
        };
    };
    const auto slots = [](int32_t v) { return [v](gal::GalacticRules& r) { r.types[tPlanet].ground_structure_slots = v; }; };
    auto solve = [](ParityWorld& pw, bool& ok) {
        pw.run(2);  // first service: templates, budget
        ok = false;
        return pw.world->solve_potential_plan(0, "BuildBarracks", "Upgrade_Starbase", 0, &ok);
    };
    auto token = [](ParityWorld& pw, const char* text) {
        EquationSet e;
        e.add("T", text);
        e.link();
        EvalContext c;
        c.self = 0;
        c.target = 0;
        const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
        CHECK(r.ok);
        return r.value;
    };
    bool ok = false;
    {
        // No Special_Structures_Land (-1): unlimited; the tokens fall back to 5.
        ParityWorld pw(false, only);
        const std::vector<uint32_t> s = solve(pw, ok);
        CHECK(ok && pw.has(s, tBarracks));
        CHECK(galactic_ai_ground_slot_max(*pw.sim, 0) == -1 && galactic_ai_ground_slots_used(*pw.sim, 0) == 1);
        CHECK(galactic_ai_ground_slots_used(*pw.sim, 2) == 0 && galactic_ai_ground_slot_max(*pw.sim, 99) == 0);
        CHECK(token(pw, "Variable_Target.MaxStructureSlotsUnnormalized") == 5.0);
        CHECK(token(pw, "Variable_Target.OpenStructureSlotsUnnormalized") == 4.0);
    }
    {
        // 3 slots, the map's <Max_Ground_Base> 0 (as on every shipped planet): 1 used < 3 -> solved.
        ParityWorld pw(false, only, map_ground(0), slots(3));
        const std::vector<uint32_t> s = solve(pw, ok);
        std::printf("ground slots: Special_Structures_Land 3, Max_Ground_Base 0 -> %s\n", ok ? "solved" : "vetoed");
        CHECK(ok && pw.has(s, tBarracks));
        CHECK(galactic_ai_ground_slot_max(*pw.sim, 0) == 3);
        CHECK(token(pw, "Variable_Target.MaxStructureSlotsUnnormalized") == 3.0);
        CHECK(token(pw, "Variable_Target.OpenStructureSlotsUnnormalized") == 2.0);
        CHECK(token(pw, "Variable_Target.MaxGroundbaseLevelUnnormalized") == 3.0);
    }
    {
        // 1 slot, already used by the Barracks: vetoed although <Max_Ground_Base> is 5.
        ParityWorld pw(false, only, map_ground(5), slots(1));
        const std::vector<uint32_t> s = solve(pw, ok);
        std::printf("ground slots: Special_Structures_Land 1 (used 1), Max_Ground_Base 5 -> %s\n", ok ? "solved" : "vetoed");
        CHECK(!ok && !pw.has(s, tBarracks));
        CHECK(token(pw, "Variable_Target.OpenStructureSlotsUnnormalized") == 0.0);
    }
    {
        // Build_Max_Instances_Per_Planet 1 with one Barracks present: vetoed with unlimited slots.
        ParityWorld pw(false, only, {}, [](gal::GalacticRules& r) { r.types[tBarracks].max_instances_per_planet = 1; });
        const std::vector<uint32_t> s = solve(pw, ok);
        CHECK(!ok && !pw.has(s, tBarracks));
    }
}

// a2m r2 (A2M_R2_RE_A §1.5) [H]: a structure's ETA includes the target queue's wait. The target P0 is held, so its
// source list starts at P0 and the structure marks P0's ground pool entry used: ETA = the planet production queue's time
// (0x401ba0 on the first solve: the current item's remaining time + the rest's build times) + the build time.
void test_structure_eta_queue_wait() {
    ParityWorld pw(false, {"BuildBarracks"});
    pw.run(2);
    bool ok = false;
    float idle = -1.0f, busy = -1.0f;
    pw.world->solve_potential_plan(0, "BuildBarracks", "Upgrade_Starbase", 0, &ok, &idle);
    CHECK(ok && idle > 0.0f);
    CHECK(pw.sim->enqueue(0, 0, tInfantry) == commands::RejectReason::None);
    CHECK(pw.sim->enqueue(0, 0, tInfantry) == commands::RejectReason::None);
    const auto q = pw.sim->queue_of(0, 0);
    CHECK(q.size() == 2);
    const float wait = q.size() == 2 ? q[0].remaining + q[1].build_time : 0.0f;
    ok = false;
    pw.world->solve_potential_plan(0, "BuildBarracks", "Upgrade_Starbase", 0, &ok, &busy);
    std::printf("structure ETA: idle %.3f, queue wait %.3f -> %.3f\n", static_cast<double>(idle), static_cast<double>(wait),
                static_cast<double>(busy));
    CHECK(ok && wait > 0.0f && busy == wait + idle);
}

// a2m r4 (A1_READINGS §28.6 step 3, AI_GALACTIC_2 §2.3 market step 5) [H]: a PotentialPlan's AIPlanetBuildQueue slot
// is made when its goal is accepted (0x6c2d60, inside Resolve_Category), so it is there when the plan starts, and the
// production start of the matching task removes it. The port made it at the next Commit's step 4 instead, after the
// production had started, so the slot was never removed and the finished build counted in every later re-solve ETA
// at that planet (the Rebel Naboo mine at 744 in seed 0).
void test_queue_slot_lifetime() {
    ParityWorld pw(false, {"BuildBarracks"});
    const auto slots = [&pw] {
        size_t n = 0;
        for (PlanetIndex p = 0; p < pw.sim->planets().size(); ++p) n += pw.world->planet_queue_slots(p, 0) + pw.world->planet_queue_slots(p, 1);
        return n;
    };
    const auto barracks_queued = [&pw](PlanetIndex& at) {
        for (PlanetIndex p = 0; p < pw.sim->planets().size(); ++p)
            for (uint32_t q = 0; q < 2; ++q)
                for (const gal::BuildEntry& e : pw.sim->queue_of(p, q))
                    if (e.type == tBarracks && e.player == 0) {
                        at = p;
                        return true;
                    }
        return false;
    };
    size_t log_seen = 0;
    int32_t accepted = -1, started = -1;
    size_t slots_at_accept = 0, slots_after_start = 99;
    PlanetIndex at = kInvalidPlanet;
    for (int32_t f = 1; f <= 30 * 200 && (started < 0 || f < started + 30 * 60); ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        const std::vector<AiDecision>& log = pw.world->decision_log();
        for (; log_seen < log.size(); ++log_seen)
            if (accepted < 0 && log[log_seen].text.find("BUILDBARRACKS for") != std::string::npos) {
                accepted = f;
                slots_at_accept = slots();
            }
        if (started < 0 && barracks_queued(at)) started = f;
        if (started >= 0) slots_after_start = slots();
    }
    const float wait = at != kInvalidPlanet ? pw.world->planet_queue_wait(at, 0) : -1.0f;
    float own = 0.0f;
    if (at != kInvalidPlanet) {
        const auto q = pw.sim->queue_of(at, 0);
        for (size_t i = 0; i < q.size(); ++i) own += i == 0 ? q[i].remaining : q[i].build_time;
    }
    std::printf("queue slot lifetime: accepted %d (slots %zu), production %d, slots %zu a minute later, wait %.2f (queue %.2f)\n",
                accepted, slots_at_accept, started, slots_after_start, static_cast<double>(wait), static_cast<double>(own));
    CHECK(accepted > 0 && slots_at_accept >= 1);           // made at accept, before the plan's first commit step 4
    CHECK(started >= accepted && slots_after_start == 0);  // the production start removed it; no later commit re-made it
    CHECK(wait == own);                                     // the re-solve wait has no stale slot
}

// A2 f t01 (2): BudgetFractionToBuild 0x667120 fails only without a goal system / for an unknown type. Before the
// budget's Init it reads the constructed zeros (an empty category = income rate + credits; a category = 0 / 0 -> D = 0
// -> never), so a template budget equation that reads it does not fail at Init (AI_GALACTIC_2 §3.2): the category keeps
// its weight and gets its share of the starting credits.
void test_budget_fraction_before_init() {
    const char* frac = "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Barracks\"}";
    const char* frac_infra = "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Barracks\", Parameter_Category = \"Infrastructure\"}";
    auto token = [](ParityWorld& pw, const char* text, bool& ok) {
        EquationSet e;
        e.add("T", text);
        e.link();
        EvalContext c;
        c.self = 0;
        const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
        ok = r.ok;
        return r.value;
    };
    bool ok = false;
    {
        ParityWorld pw(false, {"BuildBarracks"});  // no update yet: the budget is not initialised
        const double all = token(pw, frac, ok);
        CHECK(ok && all > 0.0 && all <= 500.0 / 6000.0);  // R / (time * income rate + credits)
        const double infra = token(pw, frac_infra, ok);
        CHECK(ok && infra == static_cast<double>(1.0e18f));  // D = 0 -> (double)1e18f (A2H_GALACTIC_AI_RE §2 step 6)
        CHECK(pw.world->tactical_budget_rows(0).empty() || pw.world->tactical_budget_rows(0)[0].available == 0.0f);
    }
    {
        ParityWorld pw(false, {"BuildBarracks"}, {}, {}, [frac_infra](EquationSet& e) {
            e.add("Infra_Budget", std::string("3 + 0 * ") + frac_infra);  // wins over the synthetic "3"
        });
        pw.run(1);  // first service: templates, budget Init
        int funded = 0;
        float total = 0.0f;
        for (const GalacticAiWorld::TacticalBudgetRow& r : pw.world->tactical_budget_rows(0)) {
            funded += r.available > 0.0f ? 1 : 0;
            total += r.available;
        }
        // Leaf cache (AI_GALACTIC_TOKENS §9.9): the key is the parameter set only, so this query hits the entry the
        // budget Init stored while the budget was still empty (D <= 0 -> (double)1e18f) until its 5 s lifetime runs out.
        const double cached = token(pw, frac_infra, ok);
        CHECK(ok && cached == static_cast<double>(1.0e18f));
        // Read afresh at the same frame (the AI queues its Barracks within the 5 s, after which the in-progress head
        // decides the token instead of the pool).
        const_cast<GalacticSimTokenResolver&>(pw.world->resolver()).clear_cache();
        const double infra = token(pw, frac_infra, ok);
        std::printf("budget init with BudgetFractionToBuild in Infra_Budget: %d funded categories, total %.0f, Barracks fraction %.4f\n",
                    funded, static_cast<double>(total), infra);
        CHECK(funded == 2);                      // Offensive and Infrastructure (Defensive = Zero)
        CHECK(ok && infra > 0.0 && infra < 1.0);  // Infrastructure holds more than one Barracks
    }
}

// A2f t02 R1: ProductionM vetoes a produced candidate whose Setup C time is 1e18 (no source planet passes 0x2804d0, or a
// structure 0x2804d0 refuses at the target) [port: AI_GALACTIC_2 §1.4 has no such veto]. Cases from the HotM diagnosis
// (docs/re/A2F_PRODUCTION_STALL_DIAG.md): a zero-cost hero type (Veers_AT_AT_Walker), Build_Initially_Locked never
// unlocked (TIE_Fighter_Squadron), a structure whose Required_Special_Structures is missing at the target (Heavy /
// Advanced Vehicle Factory). Free-store units of such types still join (FreeStore score, Production 0.5). Category-mask
// teams no longer drop zero-cost types (no cost rule in §1.3).
void test_production_unbuildable() {
    bool ok = false;
    float eta = -1.0f;
    const auto no_hero = [](gal::GalacticSetup& s) {
        s.forces.erase(std::remove_if(s.forces.begin(), s.forces.end(), [](const auto& f) { return f.type == tTypes; }), s.forces.end());
        own_p2(s);
    };
    {
        // HeroShip (cost 0, the HotM Veers case) not on the free store: listed first in the team and tied with the
        // Corvette, it used to win and wait on the market forever; now vetoed, the Corvette is produced.
        ParityWorld pw(false, {"HeroDeny"}, no_hero);
        pw.run(2);
        const std::vector<uint32_t> s = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok, &eta);
        std::printf("unbuildable: zero-cost hero type off the free store -> %s, hero %s, corvette %s, eta %.1f\n", ok ? "solved" : "failed",
                    pw.has(s, pw.hero) ? "yes" : "no", pw.has(s, tCorvette) ? "yes" : "no", static_cast<double>(eta));
        CHECK(ok && !pw.has(s, pw.hero) && pw.has(s, tCorvette));
        CHECK(eta > 0.0f);  // every produced selection has a source planet: the ETA counts it
        // On the free store the same hero type joins (free units are not vetoed).
        ParityWorld fs(false, {"HeroDeny"});
        fs.run(2);
        const std::vector<uint32_t> f = fs.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok, &eta);
        CHECK(ok && fs.has(f, fs.hero));
    }
    {
        // a2m t05 (A2M_T04_RE_ANSWERS Q2): Production Setup C 0x664210 and the ETA 0x6639f0 have no surface test of their
        // own; the can-build gate's step 24 lets a space unit through at a planet without an accessible surface (the
        // retail Interceptor4_Frigate at The_Maw). Every planet type surface-inaccessible: the Corvette is still produced.
        ParityWorld pw(false, {"HeroDeny"}, no_hero, [](gal::GalacticRules& r) {
            for (gal::TypeInfo& t : r.types) t.surface_accessible = false;
        });
        pw.run(2);
        const std::vector<uint32_t> s = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok, &eta);
        std::printf("unbuildable: no accessible surface anywhere -> %s, corvette %s, eta %.1f\n", ok ? "solved" : "failed",
                    pw.has(s, tCorvette) ? "yes" : "no", static_cast<double>(eta));
        CHECK(ok && pw.has(s, tCorvette) && eta > 0.0f);
    }
    {
        // Build_Initially_Locked (TIE_Fighter_Squadron in HotM): vetoed until unlocked; with the hero gone too the solve fails.
        ParityWorld pw(false, {"HeroDeny"}, no_hero, [](gal::GalacticRules& r) { r.types[tCorvette].build_initially_locked = true; });
        pw.run(2);
        const std::vector<uint32_t> s = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok);
        std::printf("unbuildable: locked Corvette -> %s (%zu selected)\n", ok ? "solved" : "failed", s.size());
        CHECK(!ok && s.empty());
        pw.sim->unlock_type(0, tCorvette);
        const std::vector<uint32_t> u = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(u, tCorvette) && !pw.has(u, pw.hero));
    }
    {
        // A structure needing a structure the target lacks (the Heavy / Advanced Vehicle Factory case): the Barracks
        // needs a Barracks; P0 holds one, P1 (the Empire's too) none.
        const auto needs = [](gal::GalacticRules& r) { r.types[tBarracks].required_special_structures = {tBarracks}; };
        ParityWorld pw(false, {"BuildBarracks"}, {}, needs);
        pw.run(2);
        const std::vector<uint32_t> at0 = pw.world->solve_potential_plan(0, "BuildBarracks", "Upgrade_Starbase", 0, &ok);
        CHECK(ok && pw.has(at0, tBarracks));
        const std::vector<uint32_t> at1 = pw.world->solve_potential_plan(0, "BuildBarracks", "Upgrade_Starbase", 1, &ok);
        std::printf("unbuildable: Barracks needing a Barracks at P1 -> %s\n", ok ? "solved" : "vetoed");
        CHECK(!ok && at1.empty());
        ParityWorld free_rule(false, {"BuildBarracks"});  // control: no requirement -> P1 solves
        free_rule.run(2);
        const std::vector<uint32_t> c1 = free_rule.world->solve_potential_plan(0, "BuildBarracks", "Upgrade_Starbase", 1, &ok);
        CHECK(ok && free_rule.has(c1, tBarracks));
    }
    {
        // Category-mask team "Corvette": a zero-cost non-hero CORVETTE type is listed (no cost rule in §1.3). Produced it
        // is vetoed (the Corvette is built); on the free store it joins.
        uint32_t zc = 0;
        const auto add = [&zc](gal::GalacticRules& r) {
            zc = static_cast<uint32_t>(r.types.size());
            gal::TypeInfo z = r.types[tCorvette];
            z.name = "ZeroCorvette";
            z.build_cost = 0;
            r.types.push_back(z);
        };
        const auto cat = [&zc](std::vector<GalacticAiTypeInfo>& t) { t[zc].categories = {"CORVETTE"}; };
        ParityWorld pw(false, {"MaskCorvette"}, own_p2, add, {}, cat);
        pw.run(2);
        const std::vector<uint32_t> s = pw.world->solve_potential_plan(0, "MaskCorvette", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(s, tCorvette) && !pw.has(s, zc));
        ParityWorld fs(false, {"MaskCorvette"}, [&zc](gal::GalacticSetup& st) { st.forces.push_back({0, 0, zc}); }, add, {}, cat);
        fs.run(2);
        const std::vector<uint32_t> f = fs.world->solve_potential_plan(0, "MaskCorvette", "Conquer_Planet", 3, &ok);
        std::printf("unbuildable: zero-cost CORVETTE type on the free store -> %s, joins %s\n", ok ? "solved" : "failed",
                    fs.has(f, zc) ? "yes" : "no");
        CHECK(ok && fs.has(f, zc) && !fs.has(f, tCorvette));
    }
}

GoalSystemConfig galactic_goal_config() {
    GoalSystemConfig c;
    c.nominal_allowed_time = true;  // as the galactic AI (A2f t09)
    return c;
}

int64_t late_goal_released_after(bool removable, float eta, LateMode mode, float nominal, int64_t* created) {
    AiDefinitions defs;
    defs.add_goals(kLateGoals, "g.xml");
    defs.link();
    LateHooks hooks;
    hooks.removable = removable;
    hooks.eta = eta;
    hooks.nominal = nominal;
    hooks.mode = mode;
    LearningSystem learning;
    SimRng rng(1);
    GoalSystem gs;
    gs.init(defs, {{0u, "F"}}, &hooks, &learning, nullptr, &rng, galactic_goal_config());
    for (int64_t f = 1; f <= 1200 && hooks.ended < 0; ++f) {
        hooks.now = f;
        gs.service(f);
    }
    if (created) *created = hooks.created;
    return hooks.created > 0 && hooks.ended > 0 ? hooks.ended - hooks.created : -1;
}

void test_goal_time_drop() {
    CHECK(galactic_maintenance_eta(10.0f, 300, 300) == 10.0f);
    CHECK(galactic_maintenance_eta(0.0f, 300, 690) == 0.0f);  // 0x722a30: elapsed is added only when the production time > 0
    CHECK(galactic_maintenance_eta(4.0f, 300, 391) > 7.0f);
    // ETA 10, tolerance 1.3 (AllowedTime 13). Stalled: 10 + elapsed > 13 after 3 s (90 frames).
    const int64_t stalled = late_goal_released_after(true, 10.0f, LateMode::Stalled);
    // On schedule: max(10 - elapsed, 0) + elapsed stays 10 <= 13, then the production time is 0 and the goal is never dropped.
    const int64_t on_time = late_goal_released_after(true, 10.0f, LateMode::OnSchedule);
    std::printf("goal time drop: ETA 10, tolerance 1.3 -> stalled released after %lld frames, on schedule after %lld\n",
                static_cast<long long>(stalled), static_cast<long long>(on_time));
    CHECK(stalled > 90 && stalled <= 92);
    CHECK(on_time < 0);
    CHECK(late_goal_released_after(false, 10.0f, LateMode::Stalled) < 0);  // Set_As_Goal_System_Removable(false): kept
    CHECK(late_goal_released_after(true, 0.0f, LateMode::Stalled) < 0);    // ETA 0 -> AllowedTime 0 = no limit

    // 0x525200 in a world (AI_GALACTIC_2 §3.4, q2f t01): a Barracks plan at P0 (Upgrade_Starbase goal). While its build
    // task waits on the market it is > 0; from production start (the bid record is released) it is 0.
    ParityWorld pw(false, {"BuildBarracks"});
    float first = -1.0f, last = -1.0f, peak = 0.0f;
    int64_t seen = 0, zero_after = 0;
    int32_t plan = -1;
    for (int32_t f = 1; f <= 30 * 120; ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        const AiContext* c = pw.world->context(0);
        if (plan < 0 && c)
            for (const auto& cat : c->goals.active())
                for (const Goal& g : cat)
                    if (g.running >= 0 && plan < 0) plan = g.running;
        if (plan < 0) continue;
        const float t = pw.world->plan_production_time(plan);
        if (t > 0.0f) {
            if (first < 0.0f) first = t;
            last = t;
            peak = std::max(peak, t);
            ++seen;
        } else if (seen > 0) {
            ++zero_after;
        }
    }
    std::printf("goal time drop: Barracks plan %d production time first %.1f, peak %.1f, last %.1f (%lld frames > 0, %lld at 0 after)\n",
                plan, static_cast<double>(first), static_cast<double>(peak), static_cast<double>(last), static_cast<long long>(seen),
                static_cast<long long>(zero_after));
    CHECK(plan >= 0 && seen > 0 && first > 0.0f && zero_after > 0);
    CHECK(pw.world->plan_production_time(-1) == 0.0f && pw.world->plan_production_time(123456) == 0.0f);
}

// ================================================================ A2f t09: AllowedTime base (A2F_RE_READS §1.4 item 2)
//
// 0x692860: Time_Limit without a PP, else 0x6c0450(PP) x Build_Time_Delay_Tolerance; 0x6c0450 = the float sum of btime(T, none)
// over the PP's selection x the plan player's Galactic_Build_Time_Multiplier (none: 1.0).
const Goal* last_active(const GoalSystem& gs) {
    const Goal* out = nullptr;
    for (const auto& cat : gs.active())
        for (const Goal& a : cat) out = &a;
    return out;
}

void test_goal_allowed_time_base() {
    // The multiplier lookup and the sum (galactic_goal_time.hpp).
    const std::vector<gal::DifficultyRules> diffs = {{"Easy", 1.0f, 1.5f}, {"Normal", 1.0f, 0.8f}, {"Hard", 1.0f, 0.6f}};
    CHECK(galactic_build_time_multiplier(diffs, 0) == 1.5f);
    CHECK(galactic_build_time_multiplier(diffs, 1) == 0.8f);
    CHECK(galactic_build_time_multiplier(diffs, 2) == 0.6f);
    CHECK(galactic_build_time_multiplier(diffs, 3) == 1.0f);
    CHECK(galactic_build_time_multiplier(diffs, gal::kNoIndex) == 1.0f);
    CHECK(galactic_build_time_multiplier({}, 0) == 1.0f);
    CHECK(galactic_nominal_time({10.0f, 12.0f, 0.5f}, 0.8f) == (10.0f + 12.0f + 0.5f) * 0.8f);
    CHECK(galactic_nominal_time({}, 1.5f) == 0.0f);

    // GoalSystem::allowed_time with and without a PP, galactic (nominal base) and tactical (ETA base) configuration.
    AiDefinitions defs;
    defs.add_goals(kLateGoals, "g.xml");  // Time_Limit 240, tolerance 1.3
    defs.link();
    LateHooks hooks;
    LearningSystem learning;
    SimRng rng(1);
    GoalSystem gal_gs, tac_gs;
    gal_gs.init(defs, {{0u, "F"}}, &hooks, &learning, nullptr, &rng, galactic_goal_config());
    tac_gs.init(defs, {{0u, "F"}}, &hooks, &learning, nullptr, &rng, {});
    Goal g;
    g.type = 0;
    g.eta = 10.0f;
    g.nominal_time = 30.0f;
    CHECK(gal_gs.allowed_time(g) == 240.0f && tac_gs.allowed_time(g) == 240.0f);  // no PP: Time_Limit
    g.plan = 0;
    CHECK(gal_gs.allowed_time(g) == 240.0f);  // a plan without a valid PP: still Time_Limit
    g.pp_valid = true;
    CHECK(gal_gs.allowed_time(g) == 30.0f * 1.3f);
    CHECK(tac_gs.allowed_time(g) == 10.0f * 1.3f);
    g.nominal_time = 0.0f;
    CHECK(gal_gs.allowed_time(g) == 0.0f);  // every build time 0: no limit

    // Accept (0x6b8660: allowed <= 0 || ETA <= allowed) and the maintenance drop read the nominal base: ETA 10 with a nominal
    // 20 (AllowedTime 26) survives a stall until ETA + elapsed > 26 (16 s); a nominal 5 (AllowedTime 6.5) rejects the plan.
    const int64_t longer = late_goal_released_after(true, 10.0f, LateMode::Stalled, 20.0f);
    int64_t created = 0;
    const int64_t rejected = late_goal_released_after(true, 10.0f, LateMode::Stalled, 5.0f, &created);
    std::printf("allowed time base: ETA 10, nominal 20 -> stalled released after %lld frames; nominal 5 -> created %lld\n",
                static_cast<long long>(longer), static_cast<long long>(created));
    CHECK(longer > 480 && longer <= 482);
    CHECK(rejected < 0 && created < 0);

    // Save / load keeps nominal_time; the galactic hash reads it, the tactical one does not.
    {
        LateHooks h1;
        h1.nominal = 20.0f;
        LearningSystem l1;
        SimRng r1(1);
        GoalSystem gs;
        gs.init(defs, {{0u, "F"}}, &h1, &l1, nullptr, &r1, galactic_goal_config());
        for (int64_t f = 1; f <= 60; ++f) {
            h1.now = f;
            gs.service(f);
        }
        const Goal* act = last_active(gs);
        CHECK(act && act->nominal_time == 20.0f && act->eta == 10.0f);
        ByteWriter w;
        gs.write(w);
        GoalSystem g2;
        g2.init(defs, {{0u, "F"}}, &h1, &l1, nullptr, &r1, galactic_goal_config());
        ByteReader rd(w.bytes());
        CHECK(g2.read(rd) && rd.done());
        const Goal* back = last_active(g2);
        CHECK(back && act && back->nominal_time == 20.0f && back->serial == act->serial);
        StateHasher ha, hb;
        gs.hash(ha);
        g2.hash(hb);
        CHECK(ha.digest() == hb.digest());
        // The same save with nominal_time 21: a different galactic hash, the same tactical one.
        std::vector<uint8_t> bytes = w.bytes();
        ByteWriter probe;
        probe.f32(20.0f);
        const std::vector<uint8_t> pat = probe.bytes();
        const auto at = std::search(bytes.begin(), bytes.end(), pat.begin(), pat.end());
        CHECK(at != bytes.end());
        if (at != bytes.end()) {
            ByteWriter other;
            other.f32(21.0f);
            const std::vector<uint8_t> rep = other.bytes();
            std::copy(rep.begin(), rep.end(), at);
            GoalSystem g3, t1, t3;
            g3.init(defs, {{0u, "F"}}, &h1, &l1, nullptr, &r1, galactic_goal_config());
            t1.init(defs, {{0u, "F"}}, &h1, &l1, nullptr, &r1, {});
            t3.init(defs, {{0u, "F"}}, &h1, &l1, nullptr, &r1, {});
            ByteReader r3(bytes), rt1(w.bytes()), rt3(bytes);
            CHECK(g3.read(r3) && t1.read(rt1) && t3.read(rt3));
            const Goal* changed = last_active(g3);
            CHECK(changed && changed->nominal_time == 21.0f);
            StateHasher hc, ht1, ht3;
            g3.hash(hc);
            t1.hash(ht1);
            t3.hash(ht3);
            CHECK(hc.digest() != ha.digest());
            CHECK(ht1.digest() == ht3.digest());
        }
    }

    // In a world: the active Barracks plan's goal carries 0x6c0450 of its live PP with the player's difficulty multiplier
    // (an "Easy" 1.5 entry here; the synthetic rules' Normal_Default is 1.0).
    ParityWorld pw(
        false, {"BuildBarracks"}, [](gal::GalacticSetup& s) { s.players[0].difficulty = 1; },
        [](gal::GalacticRules& r) { r.difficulties.push_back({"Easy", 1.0f, 1.5f}); });
    Goal found;
    bool have = false;
    for (int32_t f = 1; f <= 30 * 60 && !have; ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        if (const AiContext* c = pw.world->context(0))
            for (const auto& cat : c->goals.active())
                for (const Goal& a : cat)
                    if (!have && a.pp_valid && !pw.world->potential_plan_selection(a.pp_handle).empty()) {
                        found = a;
                        have = true;
                    }
    }
    CHECK(have);
    if (have) {
        const std::vector<uint32_t> sel = pw.world->potential_plan_selection(found.pp_handle);
        std::vector<float> bt;
        for (const uint32_t t : sel) bt.push_back(static_cast<float>(pw.rules.types[t].build_time_seconds));
        const float expect = galactic_nominal_time(bt, 1.5f);
        std::printf("allowed time base: world goal %u, %zu selected, nominal %.2f (expected %.2f), ETA %.2f\n", found.serial,
                    sel.size(), static_cast<double>(found.nominal_time), static_cast<double>(expect), static_cast<double>(found.eta));
        CHECK(expect > 0.0f);
        CHECK(found.nominal_time == expect);
        CHECK(pw.world->potential_plan_nominal_time(found.pp_handle) == expect);
    }
    CHECK(pw.world->potential_plan_nominal_time(-5) == -1.0f && pw.world->potential_plan_selection(-5).empty());
}

// ================================================================ a2g t03: btime(T, none) with Production_Speed_Factor
//
// 0x371230 without a planet = T0 of GALACTIC_ECONOMY §2.3: float(uint32(float(Build_Time_Seconds) x Production_Speed_Factor)),
// truncated. It feeds 0x6c0450 (Goal::nominal_time) and BudgetFractionToBuild's D (AI_GALACTIC_TOKENS). Barracks 10 s x 0.75
// = 7.5 -> 7; with the player's Galactic_Build_Time_Multiplier 1.5 the nominal time is 10.5 (untruncated it would be 11.25).
void test_btime_none_speed_factor() {
    ParityWorld pw(
        false, {"BuildBarracks"}, [](gal::GalacticSetup& s) { s.players[0].difficulty = 1; },
        [](gal::GalacticRules& r) {
            r.constants.production_speed_factor = 0.75f;
            r.difficulties.push_back({"Easy", 1.0f, 1.5f});
        });
    Goal found;
    bool have = false;
    int32_t frame = 0;
    while (frame < 30 * 60 && !have) {
        pw.sim->update();
        pw.world->update_player(0, ++frame, true, false);
        if (const AiContext* c = pw.world->context(0))
            for (const auto& cat : c->goals.active())
                for (const Goal& a : cat)
                    if (!have && a.pp_valid && !pw.world->potential_plan_selection(a.pp_handle).empty()) {
                        found = a;
                        have = true;
                    }
    }
    CHECK(have);
    if (have) {
        const std::vector<uint32_t> sel = pw.world->potential_plan_selection(found.pp_handle);
        std::vector<float> bt, raw;
        for (const uint32_t t : sel) {
            const float p = static_cast<float>(pw.rules.types[t].build_time_seconds) * 0.75f;
            bt.push_back(std::floor(p));  // every build time here is small and positive
            raw.push_back(p);
        }
        const float expect = galactic_nominal_time(bt, 1.5f);
        const float untruncated = galactic_nominal_time(raw, 1.5f);
        std::printf("btime none: factor 0.75, %zu selected (Barracks %s), nominal %.2f (expected %.2f, untruncated %.2f)\n",
                    sel.size(), pw.has(sel, tBarracks) ? "yes" : "no", static_cast<double>(found.nominal_time),
                    static_cast<double>(expect), static_cast<double>(untruncated));
        CHECK(pw.has(sel, tBarracks));
        CHECK(found.nominal_time == expect);
        CHECK(pw.world->potential_plan_nominal_time(found.pp_handle) == expect);
        CHECK(expect != untruncated);  // the truncation is visible
        CHECK(gal::econ::build_time_t0(10, 0.75f) == 7.0f);
    }
    // BudgetFractionToBuild (0x667120): R / D, D = btime(T, none) x income rate + credits (every category). Run on until the
    // budget holds an income rate, so the time term is read.
    while (frame < 30 * 240) {
        const AiContext* c = pw.world->context(0);
        if (c && c->budget.income_per_second() > 0.0f) break;
        pw.sim->update();
        pw.world->update_player(0, ++frame, true, false);
    }
    EquationSet e;
    e.add("T", "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Barracks\"}");
    e.link();
    EvalContext c;
    c.self = 0;
    const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
    const AiContext* ctx = pw.world->context(0);
    CHECK(r.ok && ctx);
    if (r.ok && ctx) {
        const double rate = static_cast<double>(ctx->budget.income_per_second());
        const double avail = static_cast<double>(pw.sim->credits(0));
        const bool building = !pw.sim->queue_of(0, 0).empty() || !pw.sim->queue_of(1, 0).empty();
        const double expect = 500.0 / (static_cast<double>(7.0f * ctx->budget.income_per_second()) + avail);  // float T x rate
        CHECK(!building);  // nothing of the type in progress: the pool term is read
        std::printf("btime none: BudgetFractionToBuild Barracks %.6f (rate %.3f, credits %.0f, expected %.6f, factor 1 %.6f)\n",
                    r.value, rate, avail, expect, 500.0 / (10.0 * rate + avail));
        CHECK(rate > 0.0);  // the time term is read
        CHECK(r.value == expect);
    }
}

// ================================================================ a2g t05 (a): nominal_time follows the plan_activatable re-solve
//
// Commit's 0x6c4a50 re-solves a new proposal's PP honouring the free-store reservations (A1_READINGS §28.6 step 3), and
// AllowedTime reads the re-solved selection (A2f t09). HeroDeny's team "HeroShip | Corvette = 1,1": the proposal's first
// solve takes the free-store hero (build time 50 s -> nominal 50); the hero is then reserved by another PP before the
// Commit, so the re-solve produces a Corvette (100 s -> nominal 100). The activated goal's nominal_time is the new one's.
// a2k t01: 0x6c4a50 also copies the re-solved net cost and ETA to the goal (G +0x78 / +0x7C, A2H_T06_TACTICAL_ETA §2.1) [H],
// so Resolve's fundable test and the budget add see them: the free hero's net 0 becomes the produced Corvette's cost. (The
// port kept the proposal's cost: on HotM the Empire's Build_Space_Forces reserved the first solve's Acclamator, 2500,
// while the re-solve built Tartan + Broadside, 3050, which left room for Build_Ground_Forces at frame 42.)
void test_goal_nominal_time_resolve() {
    ParityWorld pw(
        false, {"HeroDeny"}, [](gal::GalacticSetup& s) { s.planet_type[2] = tPlanet; },  // P2 (the target) lets Corvettes in
        [](gal::GalacticRules& r) {
            r.types[tTypes].build_time_seconds = 50;  // HeroShip (pushed at tTypes)
            r.types[tCorvette].build_time_seconds = 100;
        });
    CHECK(pw.hero == tTypes);
    const objects::ObjectHandle hero = pw.world->free_store_supply(0, pw.hero);
    CHECK(static_cast<bool>(hero));
    std::map<uint32_t, std::pair<std::vector<uint32_t>, float>> at_proposal;  // goal serial -> (selection, nominal_time)
    std::map<uint32_t, std::pair<float, float>> cost_at_proposal;             // goal serial -> (cost, eta)
    uint32_t watched = 0;
    bool reserved = false, checked = false;
    for (int32_t f = 1; f <= 30 * 120 && !checked; ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        const AiContext* c = pw.world->context(0);
        if (!c) continue;
        for (const auto& cat : c->goals.proposed())
            for (const Goal& g : cat) {
                if (!g.pp_valid || at_proposal.count(g.serial) != 0) continue;
                const std::vector<uint32_t> sel = pw.world->potential_plan_selection(g.pp_handle);
                at_proposal.emplace(g.serial, std::make_pair(sel, g.nominal_time));
                cost_at_proposal.emplace(g.serial, std::make_pair(g.cost, g.eta));
                if (!reserved && pw.has(sel, pw.hero) && pw.world->free_store_owner(hero) == -1) {
                    pw.world->set_free_store_owner(hero, 1 << 30);  // another PP's reservation (0x6c2b20)
                    reserved = true;
                    watched = g.serial;
                }
            }
        for (const auto& cat : c->goals.active())
            for (const Goal& a : cat) {
                if (checked || !reserved || a.serial != watched) continue;
                const auto& before = at_proposal[a.serial];
                const std::vector<uint32_t> now_sel = pw.world->potential_plan_selection(a.pp_handle);
                std::printf("nominal re-solve: goal %u proposed with %zu selected (hero %s, nominal %.1f), active with %zu (hero %s, "
                            "corvette %s, nominal %.1f, PP %.1f)\n",
                            a.serial, before.first.size(), pw.has(before.first, pw.hero) ? "yes" : "no",
                            static_cast<double>(before.second), now_sel.size(), pw.has(now_sel, pw.hero) ? "yes" : "no",
                            pw.has(now_sel, tCorvette) ? "yes" : "no", static_cast<double>(a.nominal_time),
                            static_cast<double>(pw.world->potential_plan_nominal_time(a.pp_handle)));
                CHECK(before.second == 50.0f);
                CHECK(now_sel != before.first && !pw.has(now_sel, pw.hero) && pw.has(now_sel, tCorvette));
                CHECK(a.nominal_time == 100.0f);
                CHECK(a.nominal_time == pw.world->potential_plan_nominal_time(a.pp_handle));
                float net = -1.0f, eta = -1.0f;
                CHECK(pw.world->potential_plan_cost(a.pp_handle, &net, &eta));
                const auto& cost_before = cost_at_proposal[a.serial];
                std::printf("cost re-solve: goal %u proposed net %.1f eta %.2f, active net %.1f eta %.2f (PP %.1f / %.2f, Corvette %d)\n",
                            a.serial, static_cast<double>(cost_before.first), static_cast<double>(cost_before.second),
                            static_cast<double>(a.cost), static_cast<double>(a.eta), static_cast<double>(net), static_cast<double>(eta),
                            pw.rules.types[tCorvette].build_cost);
                CHECK(cost_before.first == 0.0f);  // the free-store hero costs nothing
                CHECK(a.cost == net && a.eta == eta);
                CHECK(a.cost == static_cast<float>(pw.rules.types[tCorvette].build_cost) && a.cost > 0.0f);
                checked = true;
            }
    }
    std::printf("nominal re-solve: reserved %s, goal %u checked %s\n", reserved ? "yes" : "no", watched, checked ? "yes" : "no");
    CHECK(reserved && checked);
}

ResolveOutcome resolve_once(float nominal, float resolved, bool budget, bool galactic) {
    AiDefinitions defs;
    defs.add_goals(kResolveGoals, "g.xml");
    defs.link();
    ResolveHooks hooks;
    hooks.nominal = nominal;
    hooks.resolved = resolved;
    hooks.budget = budget;
    LearningSystem learning;
    SimRng rng(1);
    GoalSystem gs;
    gs.init(defs, {{0u, "F"}}, &hooks, &learning, nullptr, &rng, galactic ? galactic_goal_config() : GoalSystemConfig{});
    gs.service(1);  // first pass: one slice, then Commit
    ResolveOutcome o;
    CHECK(gs.commits() == 1);
    for (const auto& cat : gs.active()) o.activated = o.activated || !cat.empty();
    CHECK(o.activated == (hooks.instantiated == 1));
    o.activatable_calls = hooks.activatable_calls;
    o.activation_failures = learning.recent_activation_failures(0, 0, 1);
    o.drops = hooks.drops;
    return o;
}

void test_goal_resolve_time_ok() {
    // The re-solve keeps nominal 20 (AllowedTime 40 >= 10): activated in the greedy phase.
    const ResolveOutcome keep = resolve_once(20.0f, -1.0f, true);
    CHECK(keep.activated && keep.activatable_calls == 1 && keep.activation_failures == 0);
    // Re-solved to nominal 5: AllowedTime 10 == ETA passes (the test is AllowedTime < ETA).
    const ResolveOutcome edge = resolve_once(20.0f, 5.0f, true);
    CHECK(edge.activated && edge.activatable_calls == 1 && edge.activation_failures == 0);
    // Re-solved to nominal 4.5: AllowedTime 9 < 10. plan_activatable and fundable pass; time_ok alone fails it twice
    // (greedy: failure recorded, break; weighted: failure recorded, released) and its PP is dropped.
    const ResolveOutcome late = resolve_once(20.0f, 4.5f, true);
    std::printf("resolve time_ok: nominal 20 -> %s, re-solved 5 -> %s, re-solved 4.5 -> %s (%d re-solves, %d activation failures, "
                "%d PP drops)\n",
                keep.activated ? "active" : "released", edge.activated ? "active" : "released", late.activated ? "active" : "released",
                late.activatable_calls, late.activation_failures, late.drops);
    CHECK(!late.activated && late.activatable_calls == 2 && late.activation_failures == 2 && late.drops >= 1);
    // Without a budget the accept test (0x6b8660's time / budget check) is skipped, so Resolve's time_ok is the only time
    // gate: an accepted nominal 4.5 is released there; nominal 0 (AllowedTime 0) is no limit.
    const ResolveOutcome nobudget = resolve_once(4.5f, -1.0f, false);
    CHECK(!nobudget.activated && nobudget.activatable_calls == 2 && nobudget.activation_failures == 2);
    const ResolveOutcome zero = resolve_once(0.0f, -1.0f, false);
    CHECK(zero.activated && zero.activation_failures == 0);
    // Tactical configuration (ETA base): AllowedTime = 10 x 2 = 20 >= 10 whatever nominal_time holds.
    const ResolveOutcome tactical = resolve_once(20.0f, 4.5f, true, false);
    CHECK(tactical.activated && tactical.activation_failures == 0);
}

// ================================================================ a2h t04: ActiveGoals 0x6e0490 (A2H_GALACTIC_AI_RE §1)
//
// Exactly one Parameter_Category (no default: none, or a repeated parameter, fails the token); the names' AIGoalCategoryType
// VALUES are OR'ed (a plain id enum: [Offensive|Infrastructure] = 0 | 3 = 3 = Infrastructure) and the active-set vector of
// the category with that value is counted (an exact key, every entry). Unknown name: fails [L]. Shipped use:
// Can_Reclaim_Excess_MajorItem_Budget (ActiveGoals {Parameter_Category = AIGoalCategoryType[MajorItem]} > 0). The
// synthetic goals: Conquer_Planet / Build_Space_Forces = Offensive, Upgrade_Starbase = Infrastructure; nothing is MajorItem.
void test_active_goals_category() {
    ParityWorld pw(false, {"BuildBarracks", "HeroDeny"}, [](gal::GalacticSetup& s) { s.planet_type[2] = tPlanet; });
    const int32_t off = pw.defs.find_category("Offensive"), infra = pw.defs.find_category("Infrastructure");
    CHECK(off >= 0 && infra >= 0 && pw.defs.find_category("MajorItem") >= 0 && pw.defs.find_category("No_Such_Category") < 0);
    CHECK(pw.defs.category_value[static_cast<size_t>(off)] == 0 && pw.defs.category_value[static_cast<size_t>(infra)] == 3);
    auto token = [&pw](const char* text, bool& ok) {
        EquationSet e;
        e.add("T", text);
        e.link();
        EvalContext c;
        c.self = 0;
        const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
        ok = r.ok;
        return r.value;
    };
    size_t n_off = 0, n_infra = 0, n_all = 0;
    for (int32_t f = 1; f <= 30 * 120; ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        const AiContext* c = pw.world->context(0);
        if (!c) continue;
        const auto& act = c->goals.active();
        n_off = act[static_cast<size_t>(off)].size();
        n_infra = act[static_cast<size_t>(infra)].size();
        n_all = 0;
        for (const auto& cat : act) n_all += cat.size();
        if (n_off > 0 && n_infra > 0 && n_off != n_infra) break;
    }
    bool ok = false;
    token("Variable_Self.ActiveGoals", ok);
    const bool ok_none = ok;
    const double v_off = token("Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[Offensive]}", ok);
    CHECK(ok);
    const double v_infra = token("Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[Infrastructure]}", ok);
    CHECK(ok);
    const double v_major = token("Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[MajorItem]}", ok);
    CHECK(ok);
    // a2i t07 (A2I_TOKENS_RE §3.2-3.3) [H]: AIGoalCategoryType is a plain enum, so `[A|B]` (any '|') and an unknown name
    // do not convert: the equation never builds (not registered), so evaluating it fails. There is no OR of names.
    const char* unbuilt[] = {
        "Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[Offensive|Infrastructure]}",
        "Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[Offensive|Offensive]}",
        "Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[No_Such_Category]}",
        "Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[No_Such_Category|Infrastructure]}",
    };
    bool none_built = true;
    for (const char* text : unbuilt) {
        EquationSet e;
        none_built = none_built && e.add("T", text) == kUnresolved && e.find("T") == kUnresolved;
    }
    CHECK(none_built);
    token("Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[No_Such_Category]}", ok);
    const bool ok_unknown = ok;
    token("Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[Offensive|Infrastructure]}", ok);
    const bool ok_mixed = ok;
    // A non-literal parameter text naming a category still reaches the hook as one name.
    const double v_text = token("Variable_Self.ActiveGoals {Parameter_Category = \"Infrastructure\"}", ok);
    CHECK(ok);
    token("Variable_Self.ActiveGoals {Parameter_Category = AIGoalCategoryType[Offensive], "
          "Parameter_Category = AIGoalCategoryType[Infrastructure]}", ok);
    const bool ok_repeated = ok;
    // The failure is the equation's: a comparison over the token fails too (the same path as other failing tokens).
    token("Variable_Self.ActiveGoals > 0", ok);
    const bool ok_cmp = ok;
    std::printf("active goals: %zu active (Offensive %zu, Infrastructure %zu); token Offensive %.0f, Infrastructure %.0f, "
                "MajorItem %.0f, \"Infrastructure\" %.0f; [A|B] / unknown literals unbuilt %s; fails: none %s, unknown %s, "
                "Offensive|Infrastructure %s, repeated %s\n",
                n_all, n_off, n_infra, v_off, v_infra, v_major, v_text, none_built ? "yes" : "no", ok_none ? "no" : "yes",
                ok_unknown ? "no" : "yes", ok_mixed ? "no" : "yes", ok_repeated ? "no" : "yes");
    CHECK(n_off > 0 && n_infra > 0 && n_off != n_infra);
    CHECK(v_off == static_cast<double>(n_off) && v_infra == static_cast<double>(n_infra));
    CHECK(v_text == v_infra);
    CHECK(v_major == 0.0);
    CHECK(!ok_none && !ok_unknown && !ok_mixed && !ok_repeated && !ok_cmp);
}

// ================================================================ a2h t03: Maintenance / MaintenanceUnnormalized / NetIncome
//
// 0x669420 (A2H_GALACTIC_AI_RE §3) [H]: Self's fleets, every member: (cost / btime(T, none) / 30) x type Maintenance_Cost x
// faction Maintenance_Cost (float terms, double sum); surface units, starbases, structures and build time 0 add nothing;
// neutral faction 0; Maintenance = Unn / GrossIncomeUnnormalized; NetIncomeUnnormalized = Gross - Unn.
void test_maintenance_tokens() {
    ParityWorld pw(
        false, {"HeroDeny"}, [](gal::GalacticSetup& s) { s.forces.push_back({0, 0, tCorvette}); },
        [](gal::GalacticRules& r) {
            r.factions[0].maintenance_cost = 0.25f;  // Empire; the Rebel keeps 0
            r.types[tFrigate].maintenance_cost = 0.1f;
            r.types[tCorvette].maintenance_cost = 0.05f;
            r.types[tInfantry].maintenance_cost = 0.05f;  // on the surface: not counted
            r.types[tSB1].maintenance_cost = 0.3f;        // a starbase: not counted
            r.types[tTypes].maintenance_cost = 0.3f;      // HeroShip (build time 0): skipped
        });
    pw.run(2);
    auto token = [&pw](const char* text, int32_t self, bool& ok) {
        EquationSet e;
        e.add("T", text);
        e.link();
        EvalContext c;
        c.self = self;
        c.enemy = 1;
        const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
        ok = r.ok;
        return r.value;
    };
    // Expected: the Empire's fleet members at P0 (Frigate, Corvette, HeroShip).
    int32_t frigates = 0, corvettes = 0;
    for (const gal::FleetState& f : pw.sim->fleets())
        if (f.alive && f.owner == 0)
            for (const objects::ObjectHandle u : f.contents) {
                frigates += pw.store->type(u) == tFrigate ? 1 : 0;
                corvettes += pw.store->type(u) == tCorvette ? 1 : 0;
            }
    const float kf = 0.1f * 0.25f, kc = 0.05f * 0.25f;
    const double expect = frigates * static_cast<double>(700.0f / 12.0f / 30.0f * kf) + corvettes * static_cast<double>(400.0f / 8.0f / 30.0f * kc);
    bool ok = false;
    const double unn = token("Variable_Self.MaintenanceUnnormalized", 0, ok);
    CHECK(ok);
    const double m = token("Variable_Self.Maintenance", 0, ok);
    CHECK(ok);
    const double gross = token("Variable_Self.GrossIncomeUnnormalized", 0, ok);
    CHECK(ok);
    const double net = token("Variable_Self.NetIncomeUnnormalized", 0, ok);
    CHECK(ok);
    const double rebel = token("Variable_Self.MaintenanceUnnormalized", 1, ok);  // faction Maintenance_Cost 0
    CHECK(ok);
    const double neutral = token("Variable_Self.MaintenanceUnnormalized", 2, ok);
    CHECK(ok);
    // Another root: the planet-force sum of the Empire's enemies' fleet upkeep (A2I_TOKENS_RE §1.1); the Rebel faction's
    // Maintenance_Cost is 0 here, so 0 (test_force_maintenance_tokens covers a non-zero case).
    const double enemy = token("Variable_Enemy.Maintenance", 0, ok);
    CHECK(ok);
    std::printf("maintenance: %d frigate(s), %d corvette(s) in fleets; Unn %.6f (expected %.6f), gross %.1f, Maintenance %.8f, net %.6f\n",
                frigates, corvettes, unn, expect, gross, m, net);
    CHECK(frigates == 1 && corvettes == 1);
    CHECK(unn == expect && unn > 0.06 && unn < 0.07);  // 0.048611 + 0.020833
    CHECK(gross > 0.0 && m == unn / gross && net == gross - unn);
    CHECK(rebel == 0.0 && neutral == 0.0 && enemy == 0.0);
}

// ================================================================ a2i t04: non-Self Maintenance, planet-force upkeep, NB* income
//
// A2I_TOKENS_RE §1 [H]: Target.<side>.MaintenanceUnnormalized (orbiting fleets of that side, each member by its OWNER's
// faction factor, f x u x rate in float, Parameter_Category), .Maintenance (/ Variable_Enemy's or Variable_Self's
// GrossIncomeUnnormalized, den <= 0 fails); a non-Self root's MaintenanceUnnormalized = Σ planets of the Enemy-side leaf;
// Gross / NetIncomeNBPTM / NBTF = num / Σ potential income (all planets, cached for the game / the root's planets), no
// clamp, den 0 -> 0 when num is 0, else fail.
void test_force_maintenance_tokens() {
    ParityWorld pw(
        false, {"HeroDeny"},
        [](gal::GalacticSetup& s) {
            // The Rebel: one frigate fleet over P7. Player 3 (a Rebel-faction player) holds nothing. Trade routes P0-P1
            // (0.5) and P1-P2 (0.25).
            s.forces = {{0, 0, tSB1}, {0, 0, tBarracks}, {0, 0, tFrigate}, {0, 0, tCorvette}, {0, 1, tInfantry}, {1, 7, tFrigate}};
            gal::PlayerSetup idle;
            idle.faction = 1;
            s.players.push_back(idle);
            s.map.routes.credit_gain_factor[0] = 0.5f;
            s.map.routes.credit_gain_factor[1] = 0.25f;
        },
        [](gal::GalacticRules& r) {
            r.factions[0].maintenance_cost = 0.25f;  // Empire
            r.factions[1].maintenance_cost = 0.5f;   // Rebel
            r.types[tFrigate].maintenance_cost = 0.1f;
            r.types[tCorvette].maintenance_cost = 0.05f;
        });
    pw.run(1);
    const gal::GalacticSim& sim = *pw.sim;
    CHECK(sim.players().size() == 4 && sim.planets_owned(3) == 0 && sim.planets_owned(0) > 0);
    auto token = [&pw](const char* text, int32_t self, int32_t enemy, int32_t target, bool& ok) {
        EquationSet e;
        CHECK(e.add("T", text) != kUnresolved);
        e.link();
        EvalContext c;
        c.self = self;
        c.enemy = enemy;
        c.target = target;
        const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
        ok = r.ok;
        return r.value;
    };
    const float rate_f = 700.0f / 12.0f / 30.0f, rate_c = 400.0f / 8.0f / 30.0f;
    // Expected upkeep of one planet's orbit (Self = Empire), by the spec formula from the sim state.
    auto upkeep = [&](PlanetIndex p, bool enemy, uint64_t cat) {
        double v = 0.0;
        for (const uint32_t f : sim.planets()[p].orbit) {
            const gal::FleetState& fl = sim.fleets()[f];
            if (!fl.alive || fl.moving || (enemy ? fl.owner != 1 : fl.owner != 0)) continue;
            for (const objects::ObjectHandle u : fl.contents) {
                const uint32_t t = pw.store->type(u);
                const float fm = pw.store->owner(u) == 1 ? 0.5f : 0.25f;
                if (t == tFrigate && (cat & ai_category_bit("Frigate")) != 0) v += static_cast<double>(fm * 0.1f * rate_f);
                if (t == tCorvette && (cat & ai_category_bit("Corvette")) != 0) v += static_cast<double>(fm * 0.05f * rate_c);
            }
        }
        return v;
    };
    bool ok = false;
    const double e7 = token("Variable_Target.EnemyForce.MaintenanceUnnormalized", 0, 1, 7, ok);
    CHECK(ok && e7 == static_cast<double>(0.5f * 0.1f * rate_f) && e7 == upkeep(7, true, kAllCategories));
    const double f0 = token("Variable_Target.FriendlyForce.MaintenanceUnnormalized", 0, 1, 0, ok);
    CHECK(ok && f0 == upkeep(0, false, kAllCategories) && f0 > 0.0);
    const double f0_frig = token("Variable_Target.FriendlyForce.MaintenanceUnnormalized {Parameter_Category = "
                                 "GameObjectCategoryType[Frigate]}", 0, 1, 0, ok);
    CHECK(ok && f0_frig == upkeep(0, false, ai_category_bit("Frigate")));
    const double f0_corv = token("Variable_Target.FriendlyForce.MaintenanceUnnormalized {Parameter_Category = "
                                 "GameObjectCategoryType[Corvette]}", 0, 1, 0, ok);
    CHECK(ok && f0_corv == upkeep(0, false, ai_category_bit("Corvette")) && f0_frig + f0_corv == f0);
    const double f0_none = token("Variable_Target.FriendlyForce.MaintenanceUnnormalized {Parameter_Category = "
                                 "GameObjectCategoryType[None]}", 0, 1, 0, ok);  // value 0 = every category
    CHECK(ok && f0_none == f0);
    const double f0_inf = token("Variable_Target.FriendlyForce.MaintenanceUnnormalized {Parameter_Category = "
                                "GameObjectCategoryType[Infantry]}", 0, 1, 0, ok);
    CHECK(ok && f0_inf == 0.0);
    const double e0 = token("Variable_Target.EnemyForce.MaintenanceUnnormalized", 0, 1, 0, ok);
    CHECK(ok && e0 == 0.0);
    // Maintenance: Unn / the side player's GrossIncomeUnnormalized. Friendly at P0: / the Empire's (Self's) gross. Enemy
    // at P7: / Variable_Enemy's gross; player 3 holds no planet (gross 0) -> fail; an unbound Variable_Enemy fails the
    // same way; num 0 -> 0 whatever the den.
    const double gross0 = token("Variable_Self.GrossIncomeUnnormalized", 0, 1, -1, ok);
    CHECK(ok && gross0 > 0.0);
    const double gross1 = token("Variable_Enemy.GrossIncomeUnnormalized", 0, 1, -1, ok);
    CHECK(ok);
    const double fm0 = token("Variable_Target.FriendlyForce.Maintenance", 0, 1, 0, ok);
    CHECK(ok && fm0 == f0 / gross0);
    // The failing bindings first: the leaf cache key holds no Variable_Enemy (AI_GALACTIC_TOKENS §9.9, q6 t18) and
    // failures are never cached, so a value cached for player 1 would answer them for 15 s.
    token("Variable_Target.EnemyForce.Maintenance", 0, 3, 7, ok);
    const bool ok_enemy_gross0 = ok;
    token("Variable_Target.EnemyForce.Maintenance", 0, -1, 7, ok);
    const bool ok_unbound = ok;
    const double em7 = token("Variable_Target.EnemyForce.Maintenance", 0, 1, 7, ok);
    CHECK(gross1 > 0.0 ? ok && em7 == e7 / gross1 : !ok);
    const double em0 = token("Variable_Target.EnemyForce.Maintenance", 0, -1, 0, ok);
    CHECK(ok && em0 == 0.0);
    CHECK(!ok_enemy_gross0 && !ok_unbound);
    // A non-Self root (the Rebel or player 3 as Variable_Enemy of the Empire): Σ planets of the Enemy-side leaf, the same
    // for every non-Self root; a neutral root 0.
    const double unn_enemy = token("Variable_Enemy.MaintenanceUnnormalized", 0, 1, -1, ok);
    CHECK(ok);
    double sum = 0.0;
    for (size_t p = 0; p < sim.planets().size(); ++p)
        if (sim.planets()[p].is_planet) sum += upkeep(static_cast<PlanetIndex>(p), true, kAllCategories);
    CHECK(unn_enemy == sum && unn_enemy == e7);
    const double unn_idle = token("Variable_Enemy.MaintenanceUnnormalized", 0, 3, -1, ok);
    CHECK(ok && unn_idle == e7);
    const double unn_neutral = token("Variable_Enemy.MaintenanceUnnormalized", 0, 2, -1, ok);
    CHECK(ok && unn_neutral == 0.0);
    const double net_enemy = token("Variable_Enemy.NetIncomeUnnormalized", 0, 1, -1, ok);
    CHECK(ok && net_enemy == gross1 - e7);
    const double net3 = token("Variable_Enemy.NetIncomeUnnormalized", 0, 3, -1, ok);
    CHECK(ok && net3 == 0.0 - e7 && net3 < 0.0);  // gross 0 - the upkeep of the Empire's enemies
    // Potential income by the spec formula (0x3f83b0): base x black market, political at full control (curve q = 1:
    // term 0) unless neutral-owned, trade over every link with Credit_Gain_Factor > 0 whatever the neighbour's owner.
    auto potential = [&](size_t i) {
        const gal::PlanetState& p = sim.planets()[i];
        float s = gal::econ::income_base(p.base_value, p.black_market);
        if (p.owner != sim.neutral_player())
            s += gal::econ::income_political(p.base_value, 5.0f, sim.rules().constants.political_income_curve);
        float gain = 0.0f;
        if (p.owner >= 0)
            for (const uint32_t rt : p.links) gain += std::max(0.0f, sim.map().routes.credit_gain_factor[rt]);
        s += gal::econ::income_trade(p.base_value, gain);
        return static_cast<double>(s);
    };
    double max_all = 0.0, max_owned = 0.0;
    for (size_t i = 0; i < sim.planets().size(); ++i) {
        if (!sim.planets()[i].is_planet) continue;
        max_all += potential(i);
        if (sim.planets()[i].owner == 0) max_owned += potential(i);
    }
    const double all = token("Variable_Self.MaxAllPlanetsIncomeUnnormalized", 0, 1, -1, ok);
    CHECK(ok && all == max_all);
    const double owned = token("Variable_Self.MaxOwnedPlanetsIncomeUnnormalized", 0, 1, -1, ok);
    CHECK(ok && owned == max_owned && owned > 0.0 && owned < all);
    const double gptm = token("Variable_Self.GrossIncomeNBPTM", 0, 1, -1, ok);
    CHECK(ok && gptm == gross0 / max_all);
    const double gtf = token("Variable_Self.GrossIncomeNBTF", 0, 1, -1, ok);
    CHECK(ok && gtf == gross0 / max_owned);
    const double net0 = token("Variable_Self.NetIncomeUnnormalized", 0, 1, -1, ok);
    CHECK(ok);
    const double nptm = token("Variable_Self.NetIncomeNBPTM", 0, 1, -1, ok);
    CHECK(ok && nptm == net0 / max_all);
    // Player 3 as the root: no planet, so gross 0 and MaxOwned 0; its net is negative (the non-Self upkeep).
    const double r_gtf = token("Variable_Enemy.GrossIncomeNBTF", 0, 3, -1, ok);
    CHECK(ok && r_gtf == 0.0);  // 0 / 0 -> 0
    token("Variable_Enemy.NetIncomeNBTF", 0, 3, -1, ok);
    const bool ok_net_den0 = ok;  // num != 0, den 0 -> fail
    CHECK(!ok_net_den0);
    const double r_nptm = token("Variable_Enemy.NetIncomeNBPTM", 0, 3, -1, ok);
    CHECK(ok && r_nptm == net3 / max_all && r_nptm < 0.0);  // no clamp to [0, 1]
    const double net1 = net3;
    // MaxAll is cached for the game (lifetime -1), MaxOwned for 7 s: 20 s later only MaxAll hits the cache.
    pw.run(30 * 20);
    const uint64_t h0 = pw.world->resolver().cache_hits();
    token("Variable_Self.MaxAllPlanetsIncomeUnnormalized", 0, 1, -1, ok);
    const uint64_t h1 = pw.world->resolver().cache_hits();
    token("Variable_Self.MaxOwnedPlanetsIncomeUnnormalized", 0, 1, -1, ok);
    const uint64_t h2 = pw.world->resolver().cache_hits();
    CHECK(h1 == h0 + 1 && h2 == h1);
    std::printf("force maintenance: enemy@P7 %.8f friendly@P0 %.8f (frigate %.8f corvette %.8f), Maintenance %.8g, "
                "non-Self Unn %.8f, max all %.1f owned %.1f, NBPTM %.6f NBTF %.6f, player 3 net %.6f NBPTM %.8f\n",
                e7, f0, f0_frig, f0_corv, fm0, unn_enemy, all, owned, gptm, gtf, net1, r_nptm);
}

// ================================================================ a2h t07: BudgetFractionToBuild 0x667120 (A2H_GALACTIC_AI_RE §2)
//
// A category reads its income rate I (+0x50) and pool P (+0x4C), not A / R; a name without a budget entry gives 0 / 0
// (-> (double)1e18f). An item of the type at the head of one of Self's queues: no AI task there -> the token is 0; a
// running task lowers R (cost not yet built, capped by the goal's allocation, minus what the goal holds) and T (time left,
// capped by the goal's horizon) and drops the pool term.
void test_budget_fraction_to_build() {
    auto token = [](ParityWorld& pw, const char* text, bool& ok) {
        EquationSet e;
        e.add("T", text);
        e.link();
        EvalContext c;
        c.self = 0;
        const EvalResult r = evaluate(e, e.find("T"), c, pw.world->resolver());
        ok = r.ok;
        return r.value;
    };
    const char* frig = "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Frigate\"}";
    const char* frig_off = "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Frigate\", Parameter_Category = \"Offensive\"}";
    const char* frig_bad = "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Frigate\", Parameter_Category = \"No_Such_Category\"}";
    bool ok = false;
    {
        ParityWorld pw(false, {"HeroDeny"});
        // Run until the budget has ticked (I > 0) and a goal holds a required rate, so A / R differ from P / I.
        const int32_t off = pw.defs.find_category("Offensive");
        int32_t f = 0;
        const AiContext* c = nullptr;
        while (f < 30 * 400) {
            pw.sim->update();
            pw.world->update_player(0, ++f, true, false);
            c = pw.world->context(0);
            if (c && c->budget.income_rate(off) > 0.0f && c->budget.pool(off) != c->budget.available(off)) break;
        }
        CHECK(c != nullptr);
        bool building = false;
        for (size_t p = 0; p < pw.sim->planets().size(); ++p)
            for (uint32_t q = 0; q < 2; ++q)
                for (const gal::BuildEntry& e : pw.sim->queue_of(static_cast<PlanetIndex>(p), q))
                    building = building || (e.type == tFrigate && e.player == 0);
        if (c && !building) {
            const double v = token(pw, frig_off, ok);
            CHECK(ok);
            const double expect = 700.0 / (static_cast<double>(12.0f * c->budget.income_rate(off)) + static_cast<double>(c->budget.pool(off)));
            const double old = 700.0 / (12.0 * static_cast<double>(c->budget.rate(off)) + static_cast<double>(c->budget.available(off)));
            std::printf("budget fraction: frame %d Offensive I %.4f P %.2f (R %.4f A %.2f): Frigate %.6f (expected %.6f, A / R %.6f)\n", f,
                        static_cast<double>(c->budget.income_rate(off)), static_cast<double>(c->budget.pool(off)),
                        static_cast<double>(c->budget.rate(off)), static_cast<double>(c->budget.available(off)), v, expect, old);
            CHECK(v == expect && v != old);
        }
        CHECK(!building);
        const double bad = token(pw, frig_bad, ok);
        CHECK(ok && bad == static_cast<double>(1.0e18f));  // 0 / 0 -> 999999984306749440, not a failure
        CHECK(bad == 999999984306749440.0);
        token(pw, "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"No_Such_Type\"}", ok);
        CHECK(!ok);
        token(pw, "Variable_Self.BudgetFractionToBuild", ok);
        CHECK(!ok);
        EvalContext ce;
        ce.self = 0;
        const TokenResolver& res = pw.world->resolver();
        // Another root player: 0 (success).
        EquationSet e2;
        e2.add("T", "Variable_Enemy.BudgetFractionToBuild {Parameter_Type = \"Frigate\"}");
        e2.link();
        ce.enemy = 1;
        const EvalResult en = evaluate(e2, e2.find("T"), ce, res);
        CHECK(en.ok && en.value == 0.0);
    }
    {
        // A Frigate queued for the Empire with no AI build task on that planet queue: the whole token is 0.
        ParityWorld pw(false, {"HeroDeny"});
        pw.run(2);
        const double before = token(pw, frig, ok);
        CHECK(ok && before > 0.0);
        const auto rr = pw.sim->enqueue(0, 0, tFrigate);
        CHECK(rr == commands::RejectReason::None && !pw.sim->queue_of(0, 1).empty());
        // (The leaf cache lives 5 s and is keyed by the parameters: an ignored extra parameter reads it afresh.)
        const double after = token(pw, "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Frigate\", Parameter_Count = 1}", ok);
        std::printf("budget fraction: Frigate %.6f, queued without an AI task %.6f\n", before, after);
        CHECK(ok && after == 0.0);
    }
    {
        // BuildBarracks: the AI's own Barracks production. While it heads the ground queue with a live task, the token is
        // (min(cost x (1 - p), alloc) - min(alloc, a + c)) / (min(T, rem, max(h, 0)) x rate), no pool term.
        ParityWorld pw(false, {"BuildBarracks"});
        int32_t f = 0, seen = 0, checked = 0;
        while (f < 30 * 300 && checked < 3) {
            pw.sim->update();
            pw.world->update_player(0, ++f, true, false);
            const AiContext* c = pw.world->context(0);
            if (!c) continue;
            for (size_t p = 0; p < pw.sim->planets().size(); ++p) {
                const auto q0 = pw.sim->queue_of(static_cast<PlanetIndex>(p), 0);
                if (q0.empty() || q0.front().type != tBarracks || q0.front().player != 0) continue;
                ++seen;
                // The goal's budget entry: the only one held in the budget while the plan runs.
                const AiBudget::GoalEntry* ge = nullptr;
                int32_t entries = 0;
                for (int32_t k = 0; k < static_cast<int32_t>(c->budget.size()); ++k)
                    for (const AiBudget::GoalEntry& g : c->budget.category(k).goals) {
                        ge = &g;
                        ++entries;
                    }
                if (entries != 1 || f % 37 != 0) continue;
                const gal::BuildEntry& e = q0.front();
                const float cost = 500.0f;
                const float prog = std::clamp(1.0f - e.remaining / e.build_time, 0.0f, 1.0f);
                float R = std::min(cost, (1.0f - prog) * cost);
                R = std::min(R, ge->alloc);
                R = R - std::min(ge->alloc, ge->a + ge->c);
                float T = 10.0f;
                T = std::min(T, static_cast<float>(std::max<int64_t>(0, static_cast<int64_t>(e.start) + e.duration - pw.sim->frame())) / 30.0f);
                T = std::min(T, std::max(ge->h, 0.0f));
                const double D = static_cast<double>(T * c->budget.income_per_second());
                const double expect = R < 0.0001f ? 0.0 : D > 0.0 ? static_cast<double>(R) / D : static_cast<double>(1.0e18f);
                // An ignored extra parameter keyed by the frame: a fresh leaf (the cache lives 5 s).
                const std::string text =
                    "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Barracks\", Parameter_Count = " + std::to_string(f) + "}";
                const double v = token(pw, text.c_str(), ok);
                std::printf("budget fraction in progress: frame %d P%zu p %.3f alloc %.1f a %.1f c %.1f h %.2f -> R %.2f T %.2f: %.6g "
                            "(expected %.6g)\n",
                            f, p, static_cast<double>(prog), static_cast<double>(ge->alloc), static_cast<double>(ge->a),
                            static_cast<double>(ge->c), static_cast<double>(ge->h), static_cast<double>(R), static_cast<double>(T), v,
                            expect);
                CHECK(ok && v == expect);
                ++checked;
            }
        }
        std::printf("budget fraction in progress: %d frames with the Barracks at a queue head, %d checked\n", seen, checked);
        CHECK(checked > 0);
    }
}

// ================================================================ a2i t06: BudgetFractionToBuild's in-progress scan, R > 0
//
// A2H_GALACTIC_AI_RE §2 steps 4-6 with every input chosen by the test: the head entries' build time, timer start / duration
// and cached remaining time (written through the N1 save: GalacticSim has no setter), the AI build task of each planet queue
// and its goal's budget entry (the queue_task hook), the rate and pool (the budget hook). The expected values are the spec
// formula worked by hand in float (R, T, T x rate), D and the division in double.

// Rewrites the head entry of `planet`'s queue `q` through GalacticSim::write / read (the 28-byte BuildEntry record).
bool patch_queue_head(gal::GalacticSim& sim, PlanetIndex planet, uint32_t q, float build_time, int32_t start,
                             int32_t duration, float remaining) {
    const auto queue = sim.queue_of(planet, q);
    if (queue.empty()) return false;
    auto record = [](const gal::BuildEntry& b) {
        ByteWriter w;
        w.u32(b.type).f32(b.build_time).i32(b.start).i32(b.duration).f32(b.remaining).i32(b.player).f32(b.cost);
        return w.bytes();
    };
    const gal::BuildEntry old = queue.front();
    gal::BuildEntry now = old;
    now.build_time = build_time;
    now.start = start;
    now.duration = duration;
    now.remaining = remaining;
    const std::vector<uint8_t> from = record(old), to = record(now);
    ByteWriter w;
    sim.write(w);
    std::vector<uint8_t> bytes = w.bytes();
    size_t at = bytes.size(), hits = 0;
    for (size_t i = 0; i + from.size() <= bytes.size(); ++i)
        if (std::memcmp(bytes.data() + i, from.data(), from.size()) == 0) {
            at = i;
            ++hits;
        }
    if (hits != 1) return false;  // the record must be unique (the test enqueues on different frames)
    std::memcpy(bytes.data() + at, to.data(), to.size());
    ByteReader r(bytes);
    if (!sim.read(r)) return false;
    const gal::BuildEntry& e = sim.queue_of(planet, q).front();
    return e.build_time == build_time && e.start == start && e.duration == duration && e.remaining == remaining;
}

void test_budget_fraction_in_progress() {
    using QT = GalacticSimTokenResolver::QueueTask;
    gal::GalacticRules rules = syn_rules();
    objects::GameObjectTypes otypes;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, otypes, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    gal::GalacticSetup setup = syn_setup();
    setup.forces.push_back({0, 1, tSB1});  // P1: a second Empire starbase (Frigates build there too)
    CHECK(sim.start(setup));
    for (int i = 0; i < 40; ++i) sim.update();
    // Space queue (q = 1: Frigates need no ground base): a Frigate at P0, then one at P1 a frame later (a distinct record).
    CHECK(sim.enqueue(0, 0, tFrigate) == commands::RejectReason::None);
    sim.update();
    CHECK(sim.enqueue(0, 1, tFrigate) == commands::RejectReason::None);
    const int32_t F = sim.frame();
    // P0: build time 10 s, cached remaining 6 s -> p = 0.4 (the live timer would give 0.55); live rem = 135 / 30 = 4.5 s.
    CHECK(patch_queue_head(sim, 0, 1, 10.0f, F - 30, 165, 6.0f));
    // P1: build time 10 s, cached remaining 2 s -> p = 0.8; live rem = 60 / 30 = 2 s.
    CHECK(patch_queue_head(sim, 1, 1, 10.0f, F - 300, 360, 2.0f));
    CHECK(sim.frame() == F && sim.queue_of(0, 1).size() == 1 && sim.queue_of(1, 1).size() == 1);

    const std::vector<GalacticAiTypeInfo> tinfo(rules.types.size());
    // Rate / pool: all categories 2.5 / 1000, "Offensive" 0.75 / 300 (I and P).
    const float kRate = 2.5f, kPool = 1000.0f, kRateOff = 0.75f, kPoolOff = 300.0f;
    std::map<PlanetIndex, QT> tasks;  // planet -> its space-queue task (absent: None)
    int32_t task_calls = 0;
    GalacticSimTokenResolver::Hooks hooks;
    hooks.budget = [&](int32_t, std::string_view cat, float& rate, float& pool) {
        rate = cat.empty() ? kRate : cat == "Offensive" ? kRateOff : 0.0f;
        pool = cat.empty() ? kPool : cat == "Offensive" ? kPoolOff : 0.0f;
        return true;
    };
    hooks.queue_task = [&](int32_t, int32_t planet, uint32_t queue) {
        ++task_calls;
        CHECK(queue == 1u);
        const auto it = tasks.find(static_cast<PlanetIndex>(planet));
        return it == tasks.end() ? QT{} : it->second;
    };
    GalacticSimTokenResolver res(sim, tinfo, nullptr, hooks);
    res.refresh(sim.frame());
    auto token = [&](const char* text, bool& ok) {
        res.clear_cache();  // every case reads the leaf afresh
        task_calls = 0;
        EquationSet e;
        e.add("T", text);
        e.link();
        EvalContext c;
        c.self = 0;
        const EvalResult r = evaluate(e, e.find("T"), c, res);
        ok = r.ok;
        return r.value;
    };
    auto live = [](float alloc, float a, float c, float h) {
        QT t;
        t.kind = QT::Kind::Live;
        t.budget = true;
        t.alloc = alloc;
        t.a = a;
        t.c = c;
        t.h = h;
        return t;
    };
    const char* frig = "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Frigate\"}";
    const char* frig_off = "Variable_Self.BudgetFractionToBuild {Parameter_Type = \"Frigate\", Parameter_Category = \"Offensive\"}";
    const float cost = 700.0f;
    const float T0 = gal::econ::build_time_t0(12, rules.constants.production_speed_factor);
    CHECK(T0 == 12.0f);
    // Spec step 4 for one matching head entry, in float.
    struct Head {
        float build_time, remaining;
        int32_t start, duration;
    };
    const Head h0{10.0f, 6.0f, F - 30, 165}, h1{10.0f, 2.0f, F - 300, 360};
    auto step = [&](float& R, float& T, const Head& hd, const QT& t) {
        const float p = std::clamp(1.0f - hd.remaining / hd.build_time, 0.0f, 1.0f);
        const float rem = static_cast<float>(std::max(0, hd.start + hd.duration - F)) / 30.0f;
        R = std::min(R, (1.0f - p) * cost);
        R = std::min(R, t.alloc);
        R = R - std::min(t.alloc, t.a + t.c);
        T = std::min(T, rem);
        T = std::min(T, std::max(t.h, 0.0f));
    };
    auto result = [](float R, float T, float rate, bool inprog, float pool) {
        double D = static_cast<double>(T * rate);
        if (!inprog) D += static_cast<double>(pool);
        return R < 0.0001f ? 0.0 : D > 0.0 ? static_cast<double>(R) / D : static_cast<double>(1.0e18f);
    };
    struct Case {
        const char* name;
        QT p0, p1;     // the AI build tasks of P0's and P1's space queues
        bool p1_live;  // P1 takes part (else its task is Ended: the entry is ignored)
        double nice;   // the expected value with exact decimals (sanity bound on the float result)
    };
    const QT ended = [] {
        QT t;
        t.kind = QT::Kind::Ended;
        return t;
    }();
    bool ok = false;
    // (a) (1 - p) x cost is the R min (420 < 700, < alloc 500), the horizon h = 3 the T min (< rem 4.5, < T0 12):
    //     R = 420 - min(500, 30 + 20) = 370, T = 3, D = 3 x 2.5 = 7.5 (no pool) -> 49.333.
    // (b) alloc 250 the R min, rem 4.5 the T min (h 8): R = 250 - min(250, 100 + 60) = 90, D = 4.5 x 2.5 -> 8.0.
    // (c) alloc 100 the R min and a + c = 130 > alloc: R = 100 - 100 = 0 < 0.0001 -> 0 (not R / D).
    // (d) h = -2 -> max(h, 0) = 0: T = 0, D = 0 (the pool term dropped) and R = 420 -> (double)1e18f.
    // (e) both planets, target-list order P0 then P1: R 700 -> 420 -> 370 (P0: alloc 500, a + c 50), then
    //     min(370, (1 - 0.8) x 700 = 140) = 140, min(140, alloc 300), - min(300, 100) = 40; T = min(12, 4.5, 8) = 4.5, then
    //     min(4.5, 2, h 1.5) = 1.5; D = 1.5 x 2.5 = 3.75 -> 10.667. (P1 then P0 would give 140 - 100 = 40 -> min(40, 420),
    //     min(40, 500) - 50 = -10 -> 0.)
    const Case cases[] = {
        {"(1-p) x cost min, h min", live(500.0f, 30.0f, 20.0f, 3.0f), ended, false, 370.0 / 7.5},
        {"alloc min, rem min", live(250.0f, 100.0f, 60.0f, 8.0f), ended, false, 90.0 / 11.25},
        {"R < 0.0001", live(100.0f, 80.0f, 50.0f, 8.0f), ended, false, 0.0},
        {"D = 0 -> 1e18f", live(500.0f, 0.0f, 0.0f, -2.0f), ended, false, 999999984306749440.0},
        {"two planets in order", live(500.0f, 30.0f, 20.0f, 8.0f), live(300.0f, 100.0f, 0.0f, 1.5f), true, 40.0 / 3.75},
    };
    for (const Case& k : cases) {
        tasks = {{0, k.p0}, {1, k.p1}};
        float R = cost, T = T0;
        step(R, T, h0, k.p0);
        if (k.p1_live) step(R, T, h1, k.p1);
        const double expect = result(R, T, kRate, true, kPool);
        const double v = token(frig, ok);
        std::printf("budget fraction in progress (%s): R %.5f T %.3f -> %.9g (expected %.9g, exact decimals %.9g), %d task reads\n",
                    k.name, static_cast<double>(R), static_cast<double>(T), v, expect, k.nice, task_calls);
        CHECK(ok && v == expect);
        CHECK(std::fabs(v - k.nice) <= 1e-4 * std::max(1.0, k.nice));
        CHECK(task_calls == 2);  // both heads match; P1's Ended task is skipped, not a 0 return
        if (k.p1_live) {  // the reverse order (P1 first) gives another value: the scan really runs in list order
            float R2 = cost, T2 = T0;
            step(R2, T2, h1, k.p1);
            step(R2, T2, h0, k.p0);
            const double reverse = result(R2, T2, kRate, true, kPool);
            std::printf("budget fraction in progress: reverse order would give %.9g\n", reverse);
            CHECK(reverse == 0.0 && v != reverse);
        }
    }
    {
        // A category: rate = I and pool = P of "Offensive"; with a live task the pool is dropped: 90 / (4.5 x 0.75).
        tasks = {{0, live(250.0f, 100.0f, 60.0f, 8.0f)}, {1, ended}};
        float R = cost, T = T0;
        step(R, T, h0, tasks[0]);
        const double v = token(frig_off, ok);
        CHECK(ok && v == result(R, T, kRateOff, true, kPoolOff) && std::fabs(v - 90.0 / 3.375) < 1e-4);
        // Both tasks Ended: no entry counts, so R = cost, T = T0 and the pool term is back: 700 / (12 x 0.75 + 300).
        tasks = {{0, ended}, {1, ended}};
        const double idle = token(frig_off, ok);
        std::printf("budget fraction in progress: Offensive live %.9g, both Ended %.9g\n", v, idle);
        CHECK(ok && idle == static_cast<double>(cost) / (static_cast<double>(T0 * kRateOff) + static_cast<double>(kPoolOff)));
        CHECK(task_calls == 2);
    }
    {
        // An early 0.0 return stops the scan: P0 live, P1 without a task (None) -> 0 although P0 gave R > 0; the same with a
        // reclaimed P1 task. P0 None: 0 at the first planet (P1 is never read).
        tasks = {{0, live(500.0f, 30.0f, 20.0f, 8.0f)}};
        double v = token(frig, ok);
        CHECK(ok && v == 0.0 && task_calls == 2);
        QT rec;
        rec.kind = QT::Kind::EndedReclaimed;
        tasks = {{0, live(500.0f, 30.0f, 20.0f, 8.0f)}, {1, rec}};
        v = token(frig, ok);
        CHECK(ok && v == 0.0 && task_calls == 2);
        tasks = {{1, live(500.0f, 30.0f, 20.0f, 8.0f)}};
        v = token(frig, ok);
        CHECK(ok && v == 0.0 && task_calls == 1);
        // A goal without a budget entry: alloc = h = 0 -> R = min(420, 0) - 0 = 0 -> 0.
        tasks = {{0, QT{QT::Kind::Live, false, 0.0f, 0.0f, 0.0f, 0.0f}}, {1, ended}};
        v = token(frig, ok);
        CHECK(ok && v == 0.0);
    }
    {
        // Only the head of queue q counts. A second Frigate behind P0's head: P0 is still read once (the entry behind it is
        // not a match of its own).
        CHECK(sim.enqueue(0, 0, tFrigate) == commands::RejectReason::None && sim.queue_of(0, 1).size() == 2);
        tasks = {{0, live(250.0f, 100.0f, 60.0f, 8.0f)}, {1, ended}};
        float R = cost, T = T0;
        step(R, T, h0, tasks[0]);
        const double v = token(frig, ok);
        CHECK(ok && v == result(R, T, kRate, true, kPool) && task_calls == 2);
        // A Corvette head (P1 replaced): its Frigate behind is not read, so P1's None task cannot zero the token.
        CHECK(sim.cancel(0, 1, 1, 0, false) == commands::RejectReason::None && sim.queue_of(1, 1).empty());
        CHECK(sim.enqueue(0, 1, tCorvette) == commands::RejectReason::None);
        CHECK(sim.enqueue(0, 1, tFrigate) == commands::RejectReason::None && sim.queue_of(1, 1).size() == 2);
        tasks = {{0, live(250.0f, 100.0f, 60.0f, 8.0f)}};  // P1: None
        const double w = token(frig, ok);
        std::printf("budget fraction in progress: second Frigate behind P0's head %.9g, Frigate behind a Corvette at P1 %.9g\n", v, w);
        CHECK(ok && w == v && task_calls == 1);
    }
}

// ================================================================ a2g F2: the budget reset flag +0x74 on a template switch
//
// AI_GALACTIC_2 §3.1 tick second pass: with the reset flag, P = Σ x w first, then P += (credits - Σ) x w; the tick clears it.
// Retail sets it when the template set changes (docs/re/A2G_T01_RESET_FLAG_PROBE.md); the port never set it before.
void test_budget_reset_on_template_switch() {
    // AiBudget alone: Init {1, 1} with 1000 credits (A = P = 500 each), reweight {3, 1} (w 0.75 / 0.25, A = P = 500), one
    // tick with 1100 credits (Σ = 1000). Without the flag P = 500 + 100 w; with it P = 1000 w + 100 w.
    auto run = [](bool reset, AiBudget& b) {
        b.reset(2);
        CHECK(b.add_member(0) && b.add_member(1));  // q6 (AI_GALACTIC_TOKENS §9.5): only template-listed members weigh
        const std::vector<float> even = {1.0f, 1.0f}, skew = {3.0f, 1.0f};
        b.init(even, 1000.0f);
        b.reweight(skew);
        if (reset) b.mark_reset();
        CHECK(b.reset_pending() == reset);
    };
    AiBudget plain, flagged;
    run(false, plain);
    run(true, flagged);
    {
        ByteWriter w;  // the flag is saved and hashed
        flagged.write(w);
        AiBudget back;
        back.reset(2);
        ByteReader r(w.bytes());
        CHECK(back.read(r) && back.reset_pending());
        StateHasher ha, hb, hp;
        flagged.hash(ha);
        back.hash(hb);
        plain.hash(hp);
        CHECK(ha.digest() == hb.digest() && ha.digest() != hp.digest());
    }
    plain.tick(100.0f, 45.0f, 1100.0f);
    flagged.tick(100.0f, 45.0f, 1100.0f);
    std::printf("budget reset flag: after the tick P = %.1f / %.1f without, %.1f / %.1f with the flag\n", static_cast<double>(plain.pool(0)),
                static_cast<double>(plain.pool(1)), static_cast<double>(flagged.pool(0)), static_cast<double>(flagged.pool(1)));
    CHECK(plain.pool(0) == 575.0f && plain.pool(1) == 525.0f);
    CHECK(flagged.pool(0) == 825.0f && flagged.pool(1) == 275.0f);
    CHECK(!flagged.reset_pending() && !plain.reset_pending());  // the tick clears it
    flagged.tick(0.0f, 45.0f, 1100.0f);  // the next tick starts from P = A again and has no reset: the unflagged result
    CHECK(flagged.pool(0) == 575.0f && flagged.pool(1) == 525.0f);
    AiBudget again;  // init clears a pending flag
    again.reset(2);
    again.mark_reset();
    const std::vector<float> even = {1.0f, 1.0f};
    again.init(even, 10.0f);
    CHECK(!again.reset_pending());

    // In a world: Syn_Rich (priority 3, trigger Is_Rich) turns on once the AI credits drop below 5500 (6000 at the start; the
    // Barracks plan spends) -> a template switch -> the flag is set until the next fiscal tick. The control world (Is_Rich
    // never true) keeps the initial selection only (before Init) and never shows the flag.
    for (const bool rich : {true, false}) {
        ParityWorld pw(false, {"BuildBarracks"}, {}, {}, [rich](EquationSet& e) {
            e.add("Is_Rich", rich ? "Variable_Self.CreditsUnnormalized < 5500" : "Variable_Self.CreditsUnnormalized > 1000000");
        });
        int32_t set_at = -1, cleared_at = -1, switches = 0;
        size_t seen = 0;
        for (int32_t f = 1; f <= 30 * 150 && cleared_at < 0; ++f) {
            pw.sim->update();
            pw.world->update_player(0, f, true, false);
            const std::vector<AiDecision>& log = pw.world->decision_log();
            for (; seen < log.size(); ++seen)
                if (log[seen].text.rfind("templates", 0) == 0) ++switches;
            const AiContext* c = pw.world->context(0);
            if (!c) continue;
            if (c->budget.reset_pending() && set_at < 0) set_at = f;
            if (set_at >= 0 && !c->budget.reset_pending()) cleared_at = f;
        }
        std::printf("budget reset flag: %s world, %d template decisions, flag set at frame %d, cleared at %d\n",
                    rich ? "switching" : "control", switches, set_at, cleared_at);
        if (rich) {
            CHECK(switches >= 2 && set_at > 0 && cleared_at > set_at && cleared_at - set_at <= 1350 + 1);
        } else {
            CHECK(switches == 1 && set_at < 0);
        }
    }
}

// ================================================================ FIXPASS_GALACTIC §1: free-store moves on Friendly_Only
//
// Pure parts: the corrected AI request flags (§1.4), the hop-weight sum / high-16-bit test (§1.3 step 4), the orbit owner
// (step 6), Get_Next_Hop on an AiPathFinder matrix (steps 1-7) and the FindTarget pick (§1.5: at most one draw).
void test_free_store_path_rules() {
    for (int m = 0; m <= 5; ++m) {
        const gal::PathRequest r = ai_path_request(3, 4, m);
        CHECK(r.source == 3 && r.destination == 4 && r.fog_rule);
        CHECK(r.allow_unknown_or_neutral == (m == 5) && r.allow_enemy_owned == (m == 5));
        CHECK(r.skip_danger_check == (m == 1 || m == 5));
        CHECK(r.allow_impassable == (m > 2) && r.always_accept_destination == (m > 1));
    }
    // 0x691250 / 0x6913a0.
    CHECK(free_store_path_sum({}, 0) == -1 && !free_store_path_passes(-1));
    const std::vector<uint32_t> w3{500u, 0xFFFFu - 500u, 0u};
    CHECK(free_store_path_sum(w3, 3) == 0xFFFF && free_store_path_passes(0xFFFF));
    const std::vector<uint32_t> carry{500u, 0xFFFFu - 499u, 0u};  // distance terms carrying past 0xFFFF fail
    CHECK(free_store_path_sum(carry, 3) == 0x10000 && !free_store_path_passes(0x10000));
    const std::vector<uint32_t> cost{gal::pack_edge_weight(1u, 7u), 0u};  // node cost 1 on the destination
    CHECK(!free_store_path_passes(free_store_path_sum(cost, 2)));
    const std::vector<uint32_t> wrap{0xFFFFFFFFu, 2u, 0u};  // wrapping int32 sum: -1 + 2 = 1
    CHECK(free_store_path_sum(wrap, 3) == 1 && free_store_path_passes(1));
    CHECK(free_store_path_sum(std::vector<uint32_t>{0u, 0u}, 2) == 0 && free_store_path_passes(0));  // src == dst
    std::vector<uint32_t> long_path(1500, 1u);
    CHECK(free_store_path_sum(long_path, 1500) == 1000);  // at most 1000 hops summed
    // q2f t09: the sync ETA's path length 0x691370 = (low 16 bits of the sum) / 10, node costs ignored.
    {
        const std::vector<uint32_t> len{gal::pack_edge_weight(0u, 2200u), gal::pack_edge_weight(3u, 730u), 0u};
        CHECK(ai_path_length(len, 3) == 293.0);
        CHECK(ai_path_length(std::vector<uint32_t>{0u, 0u}, 2) == 0.0);
        CHECK(ai_path_length({}, 0) == static_cast<double>(0xFFFFu) / 10.0);  // empty path: the sum's start -1
    }
    // a2l t03: Reachable_Target's slot 7 (0x663810, A2L_T03_RE_ANSWERS Q1.2) per level.
    {
        const std::vector<uint32_t> free2{gal::pack_edge_weight(0u, 7u), 0u};  // A -> B, no node cost, hop 7
        const std::vector<uint32_t> cost2{gal::pack_edge_weight(1u, 7u), 0u};  // node cost 1 on the destination
        const std::vector<uint32_t> cost3{gal::pack_edge_weight(0u, 5u), gal::pack_edge_weight(1u, 7u), 0u};
        for (const int lv : {0, 2, 3, 6}) {
            CHECK(ai_reach_path_cost(free2, 2, lv) == 0.7);
            CHECK(ai_reach_path_cost(cost2, 2, lv) == kReachNone && ai_reach_path_cost(cost3, 3, lv) == kReachNone);
            CHECK(ai_reach_path_cost({}, 0, lv) == kReachNone);  // no path
        }
        // Level 4 does not count the final hop: the destination's node cost is ignored.
        CHECK(ai_reach_path_cost(cost2, 2, 4) == 0.0 && ai_reach_path_cost(cost3, 3, 4) == 0.5);
        const std::vector<uint32_t> cost3b{gal::pack_edge_weight(1u, 5u), gal::pack_edge_weight(0u, 7u), 0u};
        CHECK(ai_reach_path_cost(cost3b, 3, 4) == kReachNone && ai_reach_path_cost({}, 0, 4) == kReachNone);
        // Levels 1, 5, 10 accept every path, an empty one included.
        for (const int lv : {1, 5, 10}) {
            CHECK(ai_reach_path_cost(cost2, 2, lv) == 0.7 && ai_reach_path_cost(cost3, 3, lv) == 1.2);
            CHECK(ai_reach_path_cost({}, 0, lv) < kReachNone);
        }
        // a2m t05 (A2M_T04_RE_ANSWERS Q3): levels 7 / 8 / 9 accept float((S >> 16) x 100) <= 1000 / 3000 / 5000
        // (GameConstants Low_ / Medium_ / High_Threat_Reachability_Tolerance); an empty path (S = -1) is rejected.
        const auto node = [](uint32_t c) { return std::vector<uint32_t>{gal::pack_edge_weight(c, 7u), 0u}; };
        for (const int lv : {7, 8, 9}) {
            CHECK(ai_reach_path_cost(cost2, 2, lv) == 0.7 && ai_reach_path_cost(cost3, 3, lv) == 1.2);
            CHECK(ai_reach_path_cost({}, 0, lv) == kReachNone);
        }
        CHECK(ai_reach_path_cost(node(10), 2, 7) == 0.7 && ai_reach_path_cost(node(11), 2, 7) == kReachNone);
        CHECK(ai_reach_path_cost(node(30), 2, 8) == 0.7 && ai_reach_path_cost(node(31), 2, 8) == kReachNone);
        CHECK(ai_reach_path_cost(node(50), 2, 9) == 0.7 && ai_reach_path_cost(node(51), 2, 9) == kReachNone);
        const std::array<float, 3> tr{500.0f, 3000.0f, 5000.0f};  // Thrawn's-Revenge-style override of the low level
        CHECK(ai_reach_path_cost(node(5), 2, 7, tr) == 0.7 && ai_reach_path_cost(node(6), 2, 7, tr) == kReachNone);
        // Any other level has threshold 0.0: only a path without node costs passes.
        for (const int lv : {-1, 11, 40}) {
            CHECK(ai_reach_path_cost(free2, 2, lv) == 0.7);
            CHECK(ai_reach_path_cost(cost2, 2, lv) == kReachNone && ai_reach_path_cost({}, 0, lv) == kReachNone);
        }
    }
    // 0x3f9270.
    const OrbitFleet tr{1, true}, other{2, false}, mine{0, false};
    CHECK(orbit_owner(true, 5, std::vector<OrbitFleet>{mine}) == 5);
    CHECK(orbit_owner(false, 5, {}) == -1);
    CHECK(orbit_owner(false, 5, std::vector<OrbitFleet>{tr, other, mine}) == 2);
    CHECK(orbit_owner(false, 5, std::vector<OrbitFleet>{tr, OrbitFleet{3, true}}) == 1);

    // Get_Next_Hop on the diamond A(0)-B(1)-C(2), A-D(3)-C (free hops of 50 / 58.3 units).
    GalaxyMap map;
    map.campaign_name = "fs_paths";
    map.max_fleet_move_distance = 60.0f;
    const float xy[4][2] = {{0.0f, 0.0f}, {50.0f, 0.0f}, {100.0f, 0.0f}, {50.0f, 30.0f}};
    for (uint32_t i = 0; i < 4; ++i) {
        const auto pl = map.planets.add(std::string(1, static_cast<char>('A' + i)));
        map.planets.flags[pl] = kPlanetHasPlanetBehavior;
        map.planets.position[pl] = {xy[i][0], xy[i][1], 0.0f};
    }
    const gal::PlanetGraph g = gal::PlanetGraph::build(map);
    const PlanetIndex A = 0, B = 1, C = 2, D = 3;
    auto finder = [&g](const std::vector<double>& nv) {
        AiPathFinder f;
        f.create(g, ai_path_input(nv, nullptr, true, false), 0, 0, 1);
        return f;
    };
    std::vector<int32_t> owner_of(4, -1);
    const std::function<int32_t(PlanetIndex)> oo = [&owner_of](PlanetIndex p) { return owner_of[p]; };
    {
        const AiPathFinder f = finder(std::vector<double>(4, 0.0));
        FreeStoreHopResult r = free_store_next_hop(f, 0, A, C, false, oo);
        CHECK(r.kind == FreeStoreHop::Issue && r.hop == B && (static_cast<uint32_t>(r.sum) >> 16) == 0u);
        CHECK(free_store_next_hop(f, 0, A, C, true, oo).kind == FreeStoreHop::Wait);
        CHECK(free_store_next_hop(f, 0, kInvalidPlanet, C, false, oo).kind == FreeStoreHop::NotOnPlanet);
        CHECK(free_store_next_hop(f, 0, C, C, false, oo).kind == FreeStoreHop::Arrived);
        CHECK(free_store_next_hop(f, 0, A, 9, false, oo).kind == FreeStoreHop::NoPath);  // not a node: empty path
        owner_of[B] = 1;  // another player's fleet in B's orbit (an ally counts as other)
        CHECK(free_store_next_hop(f, 0, A, C, false, oo).kind == FreeStoreHop::OrbitBlocked);
        owner_of[B] = 0;
        CHECK(free_store_next_hop(f, 0, A, C, false, oo).kind == FreeStoreHop::Issue);
        owner_of[B] = -1;
        CHECK(free_store_next_hop(AiPathFinder{}, 0, A, C, false, oo).kind == FreeStoreHop::NoPath);  // no matrix
    }
    {   // Node cost on B: the matrix routes A -> D -> C, which is free -> pass, first hop D.
        std::vector<double> nv(4, 0.0);
        nv[B] = 1234.9;
        const FreeStoreHopResult r = free_store_next_hop(finder(nv), 0, A, C, false, oo);
        CHECK(r.kind == FreeStoreHop::Issue && r.hop == D);
    }
    {   // Node costs on B and D: every path crosses a costly planet -> no path, no fallback.
        std::vector<double> nv(4, 0.0);
        nv[B] = nv[D] = 150.0;
        const FreeStoreHopResult r = free_store_next_hop(finder(nv), 0, A, C, false, oo);
        CHECK(r.kind == FreeStoreHop::NoPath && r.sum > 0xFFFF);
    }
    {   // The destination counts, the start does not.
        std::vector<double> nv(4, 0.0);
        nv[C] = 100.0;
        CHECK(free_store_next_hop(finder(nv), 0, A, C, false, oo).kind == FreeStoreHop::NoPath);
        CHECK(free_store_next_hop(finder(nv), 0, C, A, false, oo).kind == FreeStoreHop::Issue);
        nv[C] = 99.9;  // node cost trunc(99.9) / 100 = 0
        CHECK(free_store_next_hop(finder(nv), 0, A, C, false, oo).kind == FreeStoreHop::Issue);
    }

    // FindTarget pick: draws counted.
    uint32_t draws = 0;
    float next = 0.0f;
    const std::function<float(float)> draw = [&](float total) {
        ++draws;
        CHECK(total > 0.0f);
        return next;
    };
    CHECK(find_target_pick({}, 0.5, draw) == -1 && draws == 0);
    CHECK(find_target_pick(std::vector<double>{0.0, -1.0}, 0.5, draw) == -1 && draws == 0);
    CHECK(find_target_pick(std::vector<double>{0.0, 4.0, -1.0}, 0.1, draw) == 1 && draws == 0);  // one positive: no draw
    CHECK(find_target_pick(std::vector<double>{2.0, 3.0}, 1.0, draw) == 1 && draws == 0);      // p = 1: the best
    // [2, 3], p = 0.5 (w = 1): list (0: 2), best 1 joins with 2 / 1 -> cumulative [2, 4], one draw in [0, 4].
    next = 2.0f;
    CHECK(find_target_pick(std::vector<double>{2.0, 3.0}, 0.5, draw) == 0 && draws == 1);
    next = 2.5f;
    CHECK(find_target_pick(std::vector<double>{2.0, 3.0}, 0.5, draw) == 1 && draws == 2);
    // Equal scores: the earlier stays best, the later joins the list first.
    next = 1.0f;
    CHECK(find_target_pick(std::vector<double>{3.0, 3.0}, 0.5, draw) == 1 && draws == 3);
    next = 3.5f;
    CHECK(find_target_pick(std::vector<double>{3.0, 3.0}, 0.5, draw) == 0 && draws == 4);
}

// A synthetic galactic world (ParityWorld: the Empire AI holds P0 / P1; P2..P5 unowned; the Rebels P6 / P7 with a
// starbase at P7) plus a neutral frigate at P3: Move_Object on the AI matrix, re-queried per hop; the orbit check;
// no fallback; the Friendly_Only Reachable_Target filter and its draw.
void test_free_store_world() {
    ParityWorld pw(false, {"MagicSpace"}, [](gal::GalacticSetup& s) { s.forces.push_back({2, 3, tFrigate}); });
    pw.run(2);  // first service: the AI path finder exists
    gal::GalacticSim& sim = *pw.sim;
    GalacticAiWorld& world = *pw.world;
    const objects::ObjectStore& st = *pw.store;
    auto unit_of = [&](uint32_t type) {
        for (const gal::FleetState& f : sim.fleets())
            if (f.alive && f.owner == 0)
                for (const objects::ObjectHandle u : f.contents)
                    if (st.alive(u) && st.type(u) == type) return u;
        return objects::ObjectHandle{};
    };
    auto fleet_of = [&](objects::ObjectHandle u) -> const gal::FleetState* {
        for (const gal::FleetState& f : sim.fleets())
            if (f.alive && std::find(f.contents.begin(), f.contents.end(), u) != f.contents.end()) return &f;
        return nullptr;
    };
    int32_t frame = 2;
    auto run_until_idle = [&](int32_t max_frames) {
        int32_t f = 0;
        for (; f < max_frames && world.pending_free_store_moves() > 0; ++f) {
            sim.update();
            world.update_player(0, ++frame, true, false);
        }
        return f;
    };
    auto logged = [&](const std::string& text) {
        for (const AiDecision& d : world.decision_log())
            if (d.text.find(text) != std::string::npos) return true;
        return false;
    };
    const objects::ObjectHandle frigate = unit_of(tFrigate), hero = unit_of(pw.hero);
    CHECK(frigate && hero && world.path_finder(0) && world.path_finder(0)->created());
    {
        const gal::FleetState* f = fleet_of(frigate);
        CHECK(f && f->planet == 0);
    }
    // Refusals and "already there".
    CHECK(!world.free_store_move(0, frigate, 99));
    CHECK(world.free_store_move(0, frigate, 0) && world.pending_free_store_moves() == 0);
    // P0 -> P1 (own planet, node cost 0): the frigate splits off and moves.
    CHECK(world.free_store_move(0, frigate, 1) && world.pending_free_store_moves() == 1);
    CHECK(!world.free_store_move(0, frigate, 2));  // in transit (a pending move)
    const int32_t t1 = run_until_idle(30 * 120);
    const gal::FleetState* ff = fleet_of(frigate);
    std::printf("free store world: frigate P0 -> P1 in %d frames, at P%u, fleet of %zu\n", t1,
                ff ? ff->planet : 99u, ff ? ff->contents.size() : 0u);
    CHECK(ff && ff->planet == 1 && !ff->moving && ff->contents.size() == 1);
    // P0 -> P7: the Rebel starbase at P7 gives it a node cost -> no path; logged; no fallback; the hero stays at P0.
    CHECK(world.free_store_move(0, hero, 7) && world.pending_free_store_moves() == 0);
    CHECK(logged("Unable to find a path from P0 to P7"));
    const gal::FleetState* hf = fleet_of(hero);
    CHECK(hf && hf->planet == 0 && !hf->moving);
    // P0 -> P3: the neutral frigate (relation 2 in the AI snapshot, A2F_RE_READS §3.3: any relation != 0 is hostile for the
    // last-seen-units test) gives P3 a node cost -> no path; logged; the hero stays at P0.
    const uint64_t moves_before = world.orders_issued();
    CHECK(world.free_store_move(0, hero, 3));
    const int32_t t3 = run_until_idle(30 * 300);
    hf = fleet_of(hero);
    std::printf("free store world: hero P0 -> P3 ended after %d frames at P%u (%zu orders)\n", t3, hf ? hf->planet : 99u,
                static_cast<size_t>(world.orders_issued() - moves_before));
    CHECK(world.pending_free_store_moves() == 0 && hf && hf->planet == 0 && !hf->moving);
    CHECK(logged("Unable to find a path from P0 to P3"));
    CHECK(world.path(0, 0, 3).size() == 4);  // P0 P1 P2 P3

    // Reachable_Target's Friendly_Only filter (§1.4): only planets held by the player or an ally.
    const std::vector<PlanetIndex> r0 = world.reach_planets(0, 0);
    const std::vector<PlanetIndex> r2 = world.reach_planets(0, 2);
    const std::vector<PlanetIndex> r5 = world.reach_planets(0, 5);
    std::printf("free store world: reach level 0 %zu planets, level 2 %zu, level 5 %zu\n", r0.size(), r2.size(), r5.size());
    CHECK(r0 == (std::vector<PlanetIndex>{0, 1}));
    CHECK(world.reach_planets(0, 1) == r0);
    CHECK(r2.size() == 3 && std::find(r2.begin(), r2.end(), PlanetIndex{2}) != r2.end());  // P2 reached (flag 4), not crossed
    CHECK(r5.size() == 8);
    const uint64_t d0 = pw.rng->synced().draws();
    CHECK(world.reachable_target(0, "One", "Friendly", "Friendly_Only", 1.0) == 0);  // ties keep the first, no draw
    CHECK(pw.rng->synced().draws() == d0);
    const int32_t pick = world.reachable_target(0, "One", "Friendly", "Friendly_Only", 0.1);
    CHECK((pick == 0 || pick == 1) && pw.rng->synced().draws() == d0 + 1);  // two candidates, p < 1: one draw
    // a2l t03 (A2L_T03_RE_ANSWERS Q1): without argument 6 there is no reachability test (the player-level flood above is
    // not read), so an unowned planet is a Friendly_Only candidate; with a source the candidate's slot 7 from the
    // source's planet decides: P3 (the neutral frigate's node cost) and P7 (the starbase) fail the zero-cost path test
    // from P0 at Friendly_Only, and pass at Any (threshold 1e18).
    CHECK(world.reachable_target(0, "One", "Neutral", "Friendly_Only", 1.0) == 2);
    CHECK(world.reachable_target(0, "One", "Enemy", "Friendly_Only", 1.0) == 6);
    auto slot7 = [&](PlanetIndex from, PlanetIndex to, int level) {
        if (from == to) return 0.0;
        std::vector<PlanetIndex> path;
        std::vector<uint32_t> wts;
        world.path_finder(0)->get_path(from, to, path, &wts);
        return ai_reach_path_cost(wts, path.size(), level);
    };
    CHECK(slot7(0, 3, 0) == kReachNone && slot7(0, 7, 0) == kReachNone && slot7(0, 1, 0) < kReachNone);
    auto first_pass = [&](std::initializer_list<PlanetIndex> cands, PlanetIndex from, int level) -> int32_t {
        for (const PlanetIndex c : cands)
            if (slot7(from, c, level) < kReachNone) return static_cast<int32_t>(c);
        return -1;
    };
    CHECK(world.reachable_target(0, "One", "Neutral", "Friendly_Only", 1.0, 0) == first_pass({2, 3, 4, 5}, 0, 0));
    CHECK(world.reachable_target(0, "One", "Neutral", "Friendly_Only", 1.0, 0) != 3);
    CHECK(world.reachable_target(0, "One", "Enemy", "Friendly_Only", 1.0, 0) == first_pass({6, 7}, 0, 0));
    CHECK(world.reachable_target(0, "One", "Enemy", "Friendly_Only", 1.0, 0) != 7);
    CHECK(world.reachable_target(0, "One", "Enemy", "Any", 1.0, 0) == 6);
    CHECK(world.reachable_target(0, "One", "Friendly", "Friendly_Only", 1.0, 1) == 0);  // P1 -> P0 is free; P1 itself is 0
    CHECK(world.reachable_target(0, "One", "Friendly", "Friendly_Only", 1.0, 99) == -1);  // no source planet: 1e18 for all
    // Argument 7: the 3-D distance from the source planet, after Evaluate.
    CHECK(world.reachable_target(0, "One", "Friendly", "Any", 1.0, 1, 1.0) == 1);
    CHECK(pw.rng->synced().draws() == d0 + 1);
}

// cu8f t04b (CU8F_T04_T06_RE §2.2 step 4): the free-store auto-land of an AI ground unit sitting in an orbiting fleet runs
// only while the unit's Prefer_Ground_Over_Space is true (the default); false (Lua Set_Prefer_Ground_Over_Space) keeps it
// in orbit. The infantry starts on P0's surface and is launched into an orbit fleet there before the AI runs.
// cu8g t04 (§2.2 step 3): only a unit with TRANSPORT lands: here the infantry's Company_Transport_Unit (a synthetic
// TRANSPORT type); without one (transport false) it stays in orbit. The detach (op 2) is logged before the land (op 5).
void test_prefer_ground_auto_land() {
    struct Case {
        bool prefer, transport;
    };
    for (const Case c : {Case{true, true}, Case{false, true}, Case{true, false}}) {
        const bool prefer = c.prefer;
        const bool expect = c.prefer && c.transport;
        ParityWorld pw(false, {"MagicSpace"}, [](gal::GalacticSetup& s) { s.forces.push_back({0, 0, tInfantry}); },
                       [&c](gal::GalacticRules& r) {
                           if (!c.transport) return;
                           gal::TypeInfo tr;
                           tr.name = "SynTransport";
                           tr.kind = gal::UnitKind::SpaceUnit;
                           tr.transport_behavior = true;
                           tr.hyperspace = true;
                           tr.hyperspace_speed = 1.5f;
                           r.types[tInfantry].company_transport = static_cast<uint32_t>(r.types.size());
                           r.types.push_back(tr);
                       });
        gal::GalacticSim& sim = *pw.sim;
        const objects::ObjectStore& st = *pw.store;
        objects::ObjectHandle inf{};
        for (const objects::ObjectHandle u : sim.planets()[0].surface)
            if (!inf && st.alive(u) && st.type(u) == tInfantry) inf = u;
        CHECK(static_cast<bool>(inf));
        if (!inf) continue;
        const objects::ObjectHandle one[] = {inf};
        CHECK(sim.launch_units(0, 0, one) == commands::RejectReason::None);
        bool in_orbit = false;
        for (const gal::FleetState& f : sim.fleets())
            if (f.alive && f.planet == 0 && std::find(f.contents.begin(), f.contents.end(), inf) != f.contents.end()) in_orbit = true;
        CHECK(in_orbit);
        CHECK(sim.prefer_ground_over_space(inf));  // default true
        sim.set_prefer_ground_over_space(inf, prefer);
        pw.run(30 * 20);
        bool landed = false;
        for (const objects::ObjectHandle u : sim.planets()[0].surface) landed = landed || u == inf;
        const std::string key = "auto-land " + std::to_string(st.id(inf)) + " ";
        const std::string dkey = "auto-land detach " + std::to_string(st.id(inf)) + " ";
        bool logged = false, detach_first = false;
        for (const AiDecision& d : pw.world->decision_log()) {
            if (!logged && d.text.find(dkey) != std::string::npos) detach_first = true;
            logged = logged || d.text.find(key) != std::string::npos;
        }
        std::printf("prefer ground %d, transport %d: infantry %s, auto-land %s\n", prefer ? 1 : 0, c.transport ? 1 : 0,
                    landed ? "landed" : "in orbit", logged ? "logged" : "not logged");
        CHECK(landed == expect && logged == expect && detach_first == expect);
    }
}

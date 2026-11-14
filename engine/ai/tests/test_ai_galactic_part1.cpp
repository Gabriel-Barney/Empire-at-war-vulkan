// Part 1 of test_ai_galactic (bo3 t03: split by position; see test_ai_galactic_common.hpp).
#include "test_ai_galactic_common.hpp"

// ================================================================ framework unit tests

void test_cadence() {
    // Service clock: next = NOW + max(1, (int)(interval x 30)).
    ServiceClock c{0.1f};
    CHECK(c.due(0));
    c.reschedule(0);
    CHECK(c.next == 3);
    ServiceClock g{0.0f};
    g.reschedule(5);
    CHECK(g.next == 6);
    ServiceClock p{10.0f};
    p.reschedule(7);
    CHECK(p.next == 307);

    // Context order and intervals over 600 frames with stub services.
    struct Stub final : ContextModeServices {
        std::vector<std::string> trace;
        void first_service(int64_t) override { trace.push_back("first"); }
        void template_init(int64_t) override { trace.push_back("tinit"); }
        void planning_service(int64_t n) override { trace.push_back("L" + std::to_string(n)); }
        void execution_service(int64_t n) override { trace.push_back("E" + std::to_string(n)); }
        void templates_changed(int64_t) override {}
    } stub;
    AiDefinitions defs;
    PlanLibrary lib;
    AiContext ctx(AiMode::Galactic);
    ctx.templates.init(defs, lib, {});
    struct NoHooks final : GoalModeHooks {
        uint32_t target_count() override { return 0; }
        bool applies(int32_t, uint32_t) override { return false; }
        bool blocked(uint32_t, int32_t) override { return false; }
        bool evaluate(const std::string&, int32_t, double&) override { return false; }
        bool accept(Goal&) override { return false; }
        bool plan_activatable(Goal&) override { return false; }
        bool plan_invalid(const Goal&) override { return true; }
        bool replaceable(const Goal&) override { return true; }
        void refresh_plan(Goal&) override {}
        void suspend_reservations(Goal&) override {}
        bool uses_budget() override { return false; }
        bool can_fund(const Goal&) override { return true; }
        float category_credits(int32_t) override { return 1.0f; }
        void budget_add(Goal&) override {}
        void budget_release(Goal&) override {}
        void budget_weights() override {}
        std::vector<int32_t> category_order() override { return {}; }
        void queue_production(Goal&) override {}
        void instantiate(Goal&) override {}
        void end_plan(Goal&, bool) override {}
        void drop_potential_plan(Goal&) override {}
        float elapsed_eta(const Goal&) override { return 0.0f; }
        int32_t quota_players() override { return 2; }
    } hooks;
    SimRng rng(1);
    LearningSystem dummy;
    ctx.goals.init(defs, {}, &hooks, &ctx.learning, &ctx.templates, &rng, {});
    ctx.attach(&stub, [](const std::string&, double&) { return false; });
    for (int64_t f = 1; f <= 600; ++f) ctx.service(f, true, false);
    CHECK(stub.trace.size() >= 3 && stub.trace[0] == "first" && stub.trace[1] == "tinit" && stub.trace[2] == "L1");
    CHECK(ctx.service_counts[0] == 2);    // perception: frames 1, 301
    CHECK(ctx.service_counts[1] == 600);  // goal: every frame
    CHECK(ctx.service_counts[2] == 200);  // planning: every 3 frames
    CHECK(ctx.service_counts[3] == 200);  // execution
    CHECK(ctx.service_counts[4] == 2);    // learning
    CHECK(ctx.service_counts[5] == 600);  // template
    // Order inside a frame: L before E on frame 1 (the perception service only reschedules: service_counts[0]).
    const auto pos = [&](const std::string& s) { return std::find(stub.trace.begin(), stub.trace.end(), s) - stub.trace.begin(); };
    CHECK(pos("L1") < pos("E1"));
    // Disabled: nothing runs, except galactic Planning while auto-resolve is busy.
    const auto before = ctx.service_counts;
    ctx.service(601, false, false);
    CHECK(ctx.service_counts == before);
    ctx.service(602, false, true);
    CHECK(ctx.service_counts[2] == before[2] + 1 && ctx.service_counts[1] == before[1]);
    std::printf("cadence: %zu trace entries\n", stub.trace.size());
}

// a2m r2 (A2M_R2_RE_A §2.1) [H]: the retail goal-pass cadence. 2 sources x 100 targets that all apply: every pass makes
// E = 200 evaluations. N - H = 7 -> cap (int)(20 / 7 + 0.5) = 3. Init seeds evalsLastPass with 2 x 100 = 200 -> pass 1
// quota min(ceil(200 / 150), 3) = 2; the seed is kept, so pass 1 ends with 400 -> pass 2 quota 3; pass 2 ends with 200 ->
// pass 3 quota 2. A slice stops when its counter reaches the quota, so a pass takes floor(E / q) + 1 frames: 101, 67, 101.
// Without retail_cadence: quota 3 at once and ceil(E / q) = 67 frames per pass.
void test_goal_pass_cadence() {
    struct CadenceHooks final : GoalModeHooks {
        uint32_t target_count() override { return 100; }
        bool applies(int32_t, uint32_t) override { return true; }
        bool blocked(uint32_t, int32_t) override { return false; }
        bool evaluate(const std::string&, int32_t, double&) override { return false; }  // counted all the same
        bool accept(Goal&) override { return false; }
        bool plan_activatable(Goal&) override { return false; }
        bool plan_invalid(const Goal&) override { return true; }
        bool replaceable(const Goal&) override { return true; }
        void refresh_plan(Goal&) override {}
        void suspend_reservations(Goal&) override {}
        bool uses_budget() override { return false; }
        bool can_fund(const Goal&) override { return true; }
        float category_credits(int32_t) override { return 1.0f; }
        void budget_add(Goal&) override {}
        void budget_release(Goal&) override {}
        void budget_weights() override {}
        std::vector<int32_t> category_order() override { return {}; }
        void queue_production(Goal&) override {}
        void instantiate(Goal&) override {}
        void end_plan(Goal&, bool) override {}
        void drop_potential_plan(Goal&) override {}
        float elapsed_eta(const Goal&) override { return 0.0f; }
        int32_t quota_players() override { return 7; }
    };
    AiDefinitions defs;
    for (const char* n : {"Goal_A", "Goal_B"}) {
        GoalTypeDef d;
        d.name = n;
        d.application = app::Enemy;
        d.category = 0;
        defs.goals.push_back(d);
    }
    const std::vector<GoalSource> sources{{0, "A"}, {1, "B"}};
    auto run = [&](bool retail) {
        CadenceHooks hooks;
        LearningSystem learning;
        SimRng rng(5);
        GoalSystem gs;
        GoalSystemConfig cfg;
        cfg.retail_cadence = retail;
        gs.init(defs, sources, &hooks, &learning, nullptr, &rng, cfg);
        std::vector<std::pair<int64_t, uint32_t>> ends;  // (frame the pass ended, quota of the pass that ran)
        uint32_t passes = 0, quota = 0;
        for (int64_t f = 1; f <= 300 && ends.size() < 3; ++f) {
            gs.service(f);
            if (f == 1) quota = gs.quota();
            if (gs.passes() != passes) {
                passes = gs.passes();
                ends.emplace_back(f, quota);
                quota = gs.quota();
            }
        }
        return ends;
    };
    const auto retail = run(true), old = run(false);
    CHECK(retail.size() == 3 && old.size() == 3);
    if (retail.size() == 3) {
        CHECK(retail[0] == (std::pair<int64_t, uint32_t>{101, 2u}));
        CHECK(retail[1] == (std::pair<int64_t, uint32_t>{168, 3u}));
        CHECK(retail[2] == (std::pair<int64_t, uint32_t>{269, 2u}));
    }
    if (old.size() == 3) {
        CHECK(old[0] == (std::pair<int64_t, uint32_t>{67, 3u}));
        CHECK(old[1].first - old[0].first == 100 && old[1].second == 2u);  // ceil(200 / 150) = 2 -> ceil(200 / 2) frames
    }
    std::printf("goal pass cadence: retail passes end at %lld / %lld / %lld\n", static_cast<long long>(retail.empty() ? 0 : retail[0].first),
                static_cast<long long>(retail.size() > 1 ? retail[1].first : 0), static_cast<long long>(retail.size() > 2 ? retail[2].first : 0));
}

void test_desire_and_hysteresis() {
    Goal g;
    g.desire = 10.0f;
    CHECK(GoalSystem::effective_desire(g) == 10.0f);  // proposals: no bonus
    g.state = GoalState::Active;
    const float d = 10.0f;
    CHECK(GoalSystem::effective_desire(g) == d + (1.0f - (d - 0.01f) / 39.99f) * 1.6f);
    g.desire = 40.0f;
    CHECK(GoalSystem::effective_desire(g) == 40.0f);
    g.desire = 0.005f;
    CHECK(GoalSystem::effective_desire(g) == 0.005f);
    // Learning: recent failures with expiry, permanent records, zero duration ignored.
    LearningSystem l;
    l.record_goal_outcome(3, 7, 1, false, 10.0f, 100);  // expiry 100 + 300
    l.record_goal_outcome(3, 7, 1, true, 10.0f, 100);
    l.record_goal_outcome(3, 7, 1, false, -1.0f, 100);  // permanent
    l.record_goal_outcome(3, 8, 1, false, 0.0f, 100);   // no recent record
    CHECK(l.recent_goal_failures(3, 7, 200) == 2);
    CHECK(l.recent_goal_failures(3, 7, 400) == 1);
    CHECK(l.recent_goal_failures(3, 8, 200) == 0);
    CHECK(l.recent_plan_failures(1, 7, 200) == 2);
    CHECK(l.plan_success_ratio(1) == 0.25);
    CHECK(l.plan_success_ratio(99) == 1.0);
    l.prune(400);
    CHECK(l.recent_goal_failures(3, 7, 400) == 1);
    // Desire formula: Evaluate + (double)((float)c1*w1 + (float)c2*w2).
    const double v = 12.5, w1 = -5.0, w2 = -8.0;
    const double desire = v + static_cast<double>(2.0f * static_cast<float>(w1) + 1.0f * static_cast<float>(w2));
    CHECK(desire == -5.5);
}

// a1g t01 - free-store reservations through the goal system (A1_READINGS §28.6): a mock mode whose PotentialPlans
// want unit 1 (else a fallback unit). The proposal's solve ignores reservations and reserves nothing; the accept
// re-solves honouring them and reserves; every Commit suspends the active goals' entries before any
// Resolve_Category and kept goals re-assert them; a goal whose unit another PP reserved is released.
void test_goal_reservations() {
    struct Pp {
        int32_t fallback = -1;
        std::vector<int32_t> units;
        int32_t solves = 0;
    };
    struct ResHooks final : GoalModeHooks {
        std::map<int32_t, int32_t> owner{{1, -1}, {2, -1}, {3, -1}};  // free-store entry +0x30 (-1 = 0)
        std::map<int32_t, Pp> pps;
        std::vector<std::string> trace;
        int32_t next_pp = 0, next_run = 0;
        void solve(int32_t h, bool first) {
            Pp& p = pps[h];
            ++p.solves;
            p.units.clear();
            for (const int32_t u : {1, p.fallback})
                if (first || owner[u] < 0) {  // Setup A: PP+0x104 == 0 or +0x30 == 0
                    p.units.push_back(u);
                    return;
                }
        }
        uint32_t target_count() override { return 0; }
        bool applies(int32_t, uint32_t) override { return false; }
        bool blocked(uint32_t, int32_t) override { return false; }
        bool evaluate(const std::string& f, int32_t, double& out) override {
            out = f == "A" ? 10.0 : 5.0;
            return true;
        }
        bool accept(Goal& g) override {
            g.pp_handle = next_pp++;
            pps[g.pp_handle].fallback = g.type == 0 ? 2 : 3;
            solve(g.pp_handle, true);
            g.plan = 0;
            g.pp_valid = true;
            trace.push_back("propose " + std::to_string(g.pp_handle));
            return true;
        }
        bool plan_activatable(Goal& g) override {
            solve(g.pp_handle, false);
            trace.push_back("resolve " + std::to_string(g.pp_handle));
            return !pps[g.pp_handle].units.empty();
        }
        bool plan_invalid(const Goal& g) override {
            if (g.running < 0) return true;
            for (const int32_t u : pps[g.pp_handle].units)
                if (owner[u] >= 0 && owner[u] != g.pp_handle) return true;
            return false;
        }
        bool replaceable(const Goal&) override { return true; }
        void refresh_plan(Goal& g) override {
            for (const int32_t u : pps[g.pp_handle].units) owner[u] = g.pp_handle;
            trace.push_back("reserve " + std::to_string(g.pp_handle));
        }
        void suspend_reservations(Goal& g) override {
            for (auto& [u, o] : owner)
                if (o == g.pp_handle) o = -1;
            trace.push_back("suspend " + std::to_string(g.pp_handle));
        }
        bool uses_budget() override { return false; }
        bool can_fund(const Goal&) override { return true; }
        float category_credits(int32_t) override { return 1.0f; }
        void budget_add(Goal&) override {}
        void budget_release(Goal&) override {}
        void budget_weights() override {}
        std::vector<int32_t> category_order() override { return {}; }
        void queue_production(Goal&) override {}
        void instantiate(Goal& g) override { g.running = next_run++; }
        void end_plan(Goal&, bool) override {}
        void drop_potential_plan(Goal& g) override {
            for (auto& [u, o] : owner)  // the destructor: 0x6c2320 clears only its own entries
                if (o == g.pp_handle) o = -1;
            if (g.pp_handle >= 0) trace.push_back("drop " + std::to_string(g.pp_handle));
            g.pp_handle = -1;
        }
        float elapsed_eta(const Goal&) override { return 0.0f; }
        int32_t quota_players() override { return 1; }
    } hooks;
    AiDefinitions defs;
    for (const char* n : {"Goal_A", "Goal_B"}) {
        GoalTypeDef d;
        d.name = n;
        d.application = app::Global;
        d.category = 0;
        defs.goals.push_back(d);
    }
    LearningSystem learning;
    SimRng rng(7);
    GoalSystem gs;
    GoalSystemConfig cfg;
    cfg.extra_goals_per_category = 2;
    gs.init(defs, {{0, "A"}, {1, "B"}}, &hooks, &learning, nullptr, &rng, cfg);
    auto active = [&gs]() { return gs.active()[0].size(); };
    auto at = [&hooks](const std::string& s) {
        return static_cast<size_t>(std::find(hooks.trace.begin(), hooks.trace.end(), s) - hooks.trace.begin());
    };

    // Pass 1: both proposals solve unit 1 (first solve ignores reservations) and reserve nothing; A (10) is accepted
    // first and reserves unit 1; B's accept re-solves past it to unit 3.
    gs.service(1);
    CHECK(gs.commits() == 1 && active() == 2);
    CHECK(hooks.pps[0].solves == 2 && hooks.pps[1].solves == 2);
    CHECK(at("propose 0") < at("propose 1") && at("propose 1") < at("resolve 0"));
    CHECK(at("resolve 0") < at("reserve 0") && at("reserve 0") < at("resolve 1") && at("resolve 1") < at("reserve 1"));
    CHECK(hooks.pps[0].units == std::vector<int32_t>{1} && hooks.pps[1].units == std::vector<int32_t>{3});
    CHECK(hooks.owner[1] == 0 && hooks.owner[2] == -1 && hooks.owner[3] == 1);

    // Pass 2: no proposals (both active). Commit step 4 suspends both before maintenance re-asserts them.
    hooks.trace.clear();
    gs.service(2);
    CHECK(gs.commits() == 2 && active() == 2);
    CHECK(at("suspend 0") < at("reserve 0") && at("suspend 1") < at("reserve 0") && at("reserve 1") < hooks.trace.size());
    CHECK(hooks.owner[1] == 0 && hooks.owner[3] == 1);

    // Another PP (99) takes unit 3: at the next Commit B's plan is invalid (its unit was stolen) and B is released;
    // its destructor leaves PP 99's entry alone. A keeps unit 1.
    hooks.owner[3] = 99;
    hooks.trace.clear();
    gs.service(3);
    CHECK(gs.commits() == 3 && active() == 1 && gs.active()[0][0].type == 0);
    CHECK(at("drop 1") < hooks.trace.size() && hooks.owner[3] == 99 && hooks.owner[1] == 0);

    // Pass 4: B is proposed again (its first solve takes unit 1 regardless of A); A is kept and re-asserts unit 1, then
    // B's re-solve finds unit 1 and unit 3 reserved: it cannot be activated and is dropped.
    hooks.trace.clear();
    gs.service(4);
    CHECK(gs.commits() == 4 && active() == 1);
    CHECK(hooks.pps[2].units.empty() && hooks.pps[2].solves >= 2);
    CHECK(at("propose 2") < at("suspend 0") && at("suspend 0") < at("reserve 0") && at("reserve 0") < at("resolve 2"));
    CHECK(at("reserve 2") == hooks.trace.size() && hooks.owner[1] == 0 && hooks.owner[2] == -1 && hooks.owner[3] == 99);
    std::printf("goal reservations: %zu hook calls in the last pass\n", hooks.trace.size());
}

// OPT2 t03 - batched goal values (ai_goals.hpp): a mode with candidate_pf() evaluates the slice's prepared candidates
// first (any thread, any range order) and commits every candidate in visit order; an unprepared function ("S", a Script_
// stand-in that draws from the synced RNG) is evaluated in place. Proposals, accepts, draws and evaluation counts must
// equal the unbatched run for every fan-out.
// OPT2 t10: the batched accept (ai_goals.hpp "BATCHED ACCEPT"). Every candidate's solve draws a data-dependent number of
// times from its own stream (seeded by frame, source and target), so the results and the synced stream's draws are
// the same for any fan-out; at latency 1 the solves of frame N are applied on N + 1 (never earlier), a save
// with solves pending continues exactly as the uninterrupted run, and a block drops the pending goals.
void test_goal_batched_accept() {
    struct Solve {
        SimRng rng;
        int32_t target = -1;
        bool ok = false;
        uint32_t plan = 0;
    };
    struct AcceptHooks final : GoalModeHooks {
        const ParallelFor* pf = nullptr;
        uint32_t latency = 0;
        int64_t frame = 0, ready = -1;
        bool early = false;     // an accept_finish before its ready frame
        uint32_t finishes = 0;
        std::vector<Solve> solves;
        uint32_t target_count() override { return 9; }
        bool applies(int32_t t, uint32_t) override { return t % 3 != 1; }
        bool blocked(uint32_t, int32_t) override { return false; }
        bool evaluate(const std::string& f, int32_t t, double& out) override {
            out = f == "A" ? 3.0 + t : 8.0 - 1.5 * t;
            return true;
        }
        bool accept(Goal&) override { return false; }  // never called: batched
        bool accept_batched() override { return true; }
        uint32_t accept_latency() override { return latency; }
        bool accept_prepare(Goal& g, uint32_t i) override {
            if (i == 0) solves.clear();
            if (g.target == 6) return false;  // rejected before its solve
            Solve s;
            s.rng.seed(static_cast<uint32_t>(g.created * 7919 + g.source * 131 + static_cast<uint32_t>(g.target + 1)));
            s.target = g.target;
            solves.push_back(s);
            return i + 1 == solves.size();
        }
        void accept_start(uint32_t n, int64_t now, int64_t r) override {
            CHECK(n == solves.size() && r == now + latency);
            ready = r;
            const auto body = [this](uint32_t b, uint32_t e) {
                for (uint32_t i = b; i < e; ++i) {
                    Solve& s = solves[i];
                    const int32_t k = s.rng.range_int(1, 4);
                    int32_t last = 0;
                    for (int32_t j = 0; j < k; ++j) last = s.rng.range_int(0, 99);
                    s.ok = last % 3 != 0;
                    s.plan = static_cast<uint32_t>(last % 2);
                }
            };
            if (pf) (*pf)(n, 1, body);
            else body(0, n);
        }
        bool accept_finish(Goal& g, uint32_t i) override {
            early = early || frame < ready;
            ++finishes;
            if (i >= solves.size() || solves[i].target != g.target || !solves[i].ok) return false;
            g.plan = solves[i].plan;
            g.pp_valid = true;
            return true;
        }
        void accept_cancel() override { solves.clear(); }
        bool plan_activatable(Goal&) override { return true; }
        bool plan_invalid(const Goal& g) override { return g.running < 0; }
        bool replaceable(const Goal&) override { return true; }
        void refresh_plan(Goal&) override {}
        void suspend_reservations(Goal&) override {}
        bool uses_budget() override { return false; }
        bool can_fund(const Goal&) override { return true; }
        float category_credits(int32_t) override { return 1.0f; }
        void budget_add(Goal&) override {}
        void budget_release(Goal&) override {}
        void budget_weights() override {}
        std::vector<int32_t> category_order() override { return {0}; }
        void queue_production(Goal&) override {}
        void instantiate(Goal& g) override { g.running = 1; }
        void end_plan(Goal&, bool) override {}
        void drop_potential_plan(Goal&) override {}
        float elapsed_eta(const Goal&) override { return 0.0f; }
        int32_t quota_players() override { return 4; }  // quota 5: slices end mid-source
    };
    AiDefinitions defs;
    for (const char* n : {"Goal_A", "Goal_B", "Goal_G"}) {
        GoalTypeDef d;
        d.name = n;
        d.application = std::strcmp(n, "Goal_G") == 0 ? app::Global : app::Enemy;
        d.category = 0;
        defs.goals.push_back(d);
    }
    const std::vector<GoalSource> sources{{0, "A"}, {1, "B"}, {2, "A"}};
    constexpr int64_t kFrames = 60;
    struct Run {
        std::vector<uint64_t> crc;
        uint64_t draws = 0;
        uint32_t passes = 0, finishes = 0, pending_frames = 0;
    };
    // save_at > 0: after that frame's service the goal system and the hooks are saved and the run continues from the copy.
    auto run = [&](const ParallelFor* pf, uint32_t latency, int64_t save_at, int64_t block_at) {
        LearningSystem learning;
        SimRng rng(99);
        auto hooks = std::make_unique<AcceptHooks>();
        hooks->pf = pf;
        hooks->latency = latency;
        auto gs = std::make_unique<GoalSystem>();
        gs->init(defs, sources, hooks.get(), &learning, nullptr, &rng, {});
        Run r;
        for (int64_t f = 1; f <= kFrames; ++f) {
            hooks->frame = f;
            gs->service(f);
            if (f == block_at) {
                gs->block_proposals(77);
                CHECK(!gs->accept_pending() || hooks->solves.empty());
                gs->unblock_proposals(77);
            }
            r.pending_frames += gs->accept_pending() ? 1u : 0u;
            StateHasher h;
            gs->hash(h);
            rng.hash(h);
            r.crc.push_back(h.digest());
            if (f == save_at) {
                CHECK(gs->accept_pending());
                ByteWriter w;
                gs->write(w);
                auto h2 = std::make_unique<AcceptHooks>(*hooks);  // the mode saves its solves' results
                auto g2 = std::make_unique<GoalSystem>();
                g2->init(defs, sources, h2.get(), &learning, nullptr, &rng, {});
                ByteReader rd(w.bytes());
                CHECK(g2->read(rd) && rd.done());
                CHECK(g2->accept_pending() && g2->accept_ready_frame() == gs->accept_ready_frame());
                hooks = std::move(h2);
                gs = std::move(g2);
            }
        }
        r.draws = rng.draws();
        r.passes = gs->passes();
        r.finishes = hooks->finishes;
        CHECK(!hooks->early && r.passes > 2 && r.finishes > 0);
        return r;
    };
    const ParallelFor serial = serial_for;
    const ParallelFor reverse = [](uint32_t count, uint32_t, const RangeFn& fn) {
        for (uint32_t i = count; i-- > 0;) fn(i, i + 1);
    };
    const ParallelFor threads = [](uint32_t count, uint32_t, const RangeFn& fn) {
        std::vector<std::thread> ts;
        for (uint32_t k = 0; k < 4; ++k)
            ts.emplace_back([&fn, count, k] {
                for (uint32_t i = k; i < count; i += 4) fn(i, i + 1);
            });
        for (std::thread& t : ts) t.join();
    };
    for (const uint32_t latency : {0u, 1u}) {
        const Run ref = run(nullptr, latency, 0, 0);
        CHECK((ref.pending_frames > 0) == (latency > 0));  // latency 0 never leaves solves pending at the end of a frame
        for (const ParallelFor* pf : {&serial, &reverse, &threads}) {
            const Run b = run(pf, latency, 0, 0);
            // the synced stream (in the CRC) sees only the goal system's own draws, the same for any fan-out
            CHECK(b.crc == ref.crc && b.draws == ref.draws && b.finishes == ref.finishes && b.passes == ref.passes);
        }
        if (latency > 0) {
            // A save with solves pending: find a frame that ends with solves in flight, save there.
            int64_t at = 0;
            for (int64_t f = 5; f < kFrames && at == 0; ++f) {
                LearningSystem learning;
                SimRng rng(99);
                AcceptHooks hk;
                hk.latency = latency;
                GoalSystem gs;
                gs.init(defs, sources, &hk, &learning, nullptr, &rng, {});
                for (int64_t g = 1; g <= f; ++g) {
                    hk.frame = g;
                    gs.service(g);
                }
                if (gs.accept_pending()) at = f;
            }
            CHECK(at > 0);
            const Run saved = run(&threads, latency, at, 0);
            CHECK(saved.crc == ref.crc && saved.finishes == ref.finishes);
            // A block while solves are pending drops them (the run goes on, still deterministic).
            const Run blocked = run(nullptr, latency, 0, at);
            const Run blocked2 = run(&threads, latency, 0, at);
            CHECK(blocked.crc == blocked2.crc && blocked.crc != ref.crc);
        }
        std::printf("goal batched accept, latency %u: %u passes, %u finishes, %u frames with solves pending\n", latency,
                    ref.passes, ref.finishes, ref.pending_frames);
    }
}

// sp4a (S7): GoalSystem::init resolves each source's function once through resolve_function(); the proposal walk and Commit's
// re-evaluation then call evaluate_resolved() with that id (a source the mode does not resolve keeps kAiNone).
void test_goal_resolved_functions() {
    struct IdHooks final : GoalModeHooks {
        std::vector<std::string> trace;
        uint32_t resolves = 0, by_name = 0;
        uint32_t target_count() override { return 0; }
        bool applies(int32_t, uint32_t) override { return false; }
        bool blocked(uint32_t, int32_t) override { return false; }
        uint32_t resolve_function(const std::string& f) override {
            ++resolves;
            return f == "A" ? 7u : kAiNone;
        }
        bool evaluate(const std::string&, int32_t, double& out) override {
            ++by_name;  // the default evaluate_resolved() is not used here: every call goes through the override below
            out = 1.0;
            return true;
        }
        bool evaluate_resolved(uint32_t e, const std::string& f, int32_t, double& out) override {
            trace.push_back(f + " " + (e == kAiNone ? std::string("none") : std::to_string(e)));
            out = f == "A" ? 10.0 : 5.0;
            return true;
        }
        bool accept(Goal& g) override {
            g.plan = 0;
            return true;
        }
        bool plan_activatable(Goal&) override { return true; }
        bool plan_invalid(const Goal& g) override { return g.running < 0; }
        bool replaceable(const Goal&) override { return true; }
        void refresh_plan(Goal&) override {}
        void suspend_reservations(Goal&) override {}
        bool uses_budget() override { return false; }
        bool can_fund(const Goal&) override { return true; }
        float category_credits(int32_t) override { return 1.0f; }
        void budget_add(Goal&) override {}
        void budget_release(Goal&) override {}
        void budget_weights() override {}
        std::vector<int32_t> category_order() override { return {}; }
        void queue_production(Goal&) override {}
        void instantiate(Goal& g) override { g.running = 1; }
        void end_plan(Goal&, bool) override {}
        void drop_potential_plan(Goal&) override {}
        float elapsed_eta(const Goal&) override { return 0.0f; }
        int32_t quota_players() override { return 1; }
    } hooks;
    AiDefinitions defs;
    for (const char* n : {"Goal_A", "Goal_B"}) {
        GoalTypeDef d;
        d.name = n;
        d.application = app::Global;
        d.category = 0;
        defs.goals.push_back(d);
    }
    LearningSystem learning;
    SimRng rng(7);
    GoalSystem gs;
    GoalSystemConfig cfg;
    cfg.extra_goals_per_category = 2;
    gs.init(defs, {{0, "A"}, {1, "B"}}, &hooks, &learning, nullptr, &rng, cfg);
    CHECK(hooks.resolves == 2 && gs.sources()[0].equation == 7u && gs.sources()[1].equation == kAiNone);
    gs.service(1);  // pass 1: both proposed and activated
    CHECK(hooks.trace == (std::vector<std::string>{"A 7", "B none"}));
    CHECK(gs.active()[0].size() == 2);
    hooks.trace.clear();
    gs.service(2);  // pass 2: Commit re-evaluates both incumbents by id
    CHECK(std::count(hooks.trace.begin(), hooks.trace.end(), "A 7") == 1 &&
          std::count(hooks.trace.begin(), hooks.trace.end(), "B none") == 1);
    CHECK(hooks.by_name == 0 && hooks.resolves == 2);  // resolved once, at init
}

void test_goal_batched_values() {
    struct BatchHooks final : GoalModeHooks {
        const ParallelFor* pf = nullptr;
        SimRng* rng = nullptr;
        std::vector<std::string> trace;  // serial calls only; a batched value is logged where evaluated() commits it
        uint32_t counted = 0;
        mutable std::atomic<uint32_t> prepared{0};  // evaluate_prepared() calls, any thread
        // OPT2 t10: slice_begin() / slice_end() bracket every slice; every batched value commit and slot 10 lies inside one.
        bool open = false, bracket_ok = true;
        uint32_t slices = 0;
        void slice_begin() override {
            bracket_ok = bracket_ok && !open;
            open = true;
            ++slices;
        }
        void slice_end() override {
            bracket_ok = bracket_ok && open;
            open = false;
        }
        static double value(const std::string& f, int32_t t) { return f == "A" ? 3.0 + t : 8.0 - 1.5 * t; }
        uint32_t target_count() override { return 9; }
        bool applies(int32_t t, uint32_t) override { return t % 3 != 1; }
        bool blocked(uint32_t, int32_t t) override { return t == 6; }
        bool evaluate(const std::string& f, int32_t t, double& out) override {  // also Commit's re-evaluation
            ++counted;
            trace.push_back("eval " + f);
            out = f == "S" ? static_cast<double>(rng->range_int(-2, 6)) : value(f, t);
            return f != "B" || t != 3;
        }
        const ParallelFor* candidate_pf() override { return pf; }
        bool prepare_evaluation(const std::string& f) override { return f != "S"; }
        bool evaluate_prepared(const std::string& f, int32_t t, double& out) const override {
            prepared.fetch_add(1, std::memory_order_relaxed);
            out = value(f, t);
            return f != "B" || t != 3;
        }
        void evaluated(const std::string& f) override {
            bracket_ok = bracket_ok && open;
            ++counted;
            trace.push_back("eval " + f);
        }
        bool accept(Goal& g) override {
            bracket_ok = bracket_ok && open;
            const int32_t r = rng->range_int(0, 3);
            trace.push_back("accept " + std::to_string(g.type) + " " + std::to_string(g.target) + " " + std::to_string(g.desire));
            if (r == 0) return false;
            g.plan = 0;
            g.pp_valid = true;
            return true;
        }
        bool plan_activatable(Goal&) override { return true; }
        bool plan_invalid(const Goal& g) override { return g.running < 0; }
        bool replaceable(const Goal&) override { return true; }
        void refresh_plan(Goal&) override {}
        void suspend_reservations(Goal&) override {}
        bool uses_budget() override { return false; }
        bool can_fund(const Goal&) override { return true; }
        float category_credits(int32_t) override { return 1.0f; }
        void budget_add(Goal&) override {}
        void budget_release(Goal&) override {}
        void budget_weights() override {}
        std::vector<int32_t> category_order() override { return {0}; }
        void queue_production(Goal&) override {}
        void instantiate(Goal& g) override { g.running = 1; }
        void end_plan(Goal&, bool) override {}
        void drop_potential_plan(Goal&) override {}
        float elapsed_eta(const Goal&) override { return 0.0f; }
        int32_t quota_players() override { return 4; }  // quota 5: slices end mid-source
    };
    struct Run {
        std::vector<std::string> trace;
        std::vector<uint64_t> crc;
        uint32_t counted = 0, prepared = 0;
        uint64_t draws = 0;
    };
    auto run = [](const ParallelFor* pf) {
        AiDefinitions defs;
        for (const char* n : {"Goal_A", "Goal_S", "Goal_B", "Goal_G"}) {
            GoalTypeDef d;
            d.name = n;
            d.application = std::strcmp(n, "Goal_G") == 0 ? app::Global : app::Enemy;
            d.category = 0;
            defs.goals.push_back(d);
        }
        LearningSystem learning;
        SimRng rng(99);
        BatchHooks hooks;
        hooks.pf = pf;
        hooks.rng = &rng;
        GoalSystem gs;
        gs.init(defs, {{0, "A"}, {1, "S"}, {2, "B"}, {3, "A"}}, &hooks, &learning, nullptr, &rng, {});
        Run r;
        uint32_t evals = 0;
        for (int64_t f = 1; f <= 40; ++f) {
            gs.service(f);
            evals += gs.evaluations_last_pass();
            StateHasher h;
            gs.hash(h);
            rng.hash(h);
            r.crc.push_back(h.digest());
        }
        r.trace = std::move(hooks.trace);
        r.counted = hooks.counted;
        r.prepared = hooks.prepared.load();
        r.draws = rng.draws();
        CHECK(gs.passes() > 2 && evals > 0);
        CHECK(hooks.bracket_ok && !hooks.open && hooks.slices > gs.passes());  // quota 5: several slices per pass
        return r;
    };
    const ParallelFor serial = serial_for;
    const ParallelFor reverse = [](uint32_t count, uint32_t, const RangeFn& fn) {
        for (uint32_t i = count; i-- > 0;) fn(i, i + 1);
    };
    uint32_t fan_outs = 0;  // batches with at least 4 prepared values (ai_goals.cpp kMinFanOut) reach the ParallelFor
    const ParallelFor threads = [&fan_outs](uint32_t count, uint32_t, const RangeFn& fn) {
        ++fan_outs;
        std::vector<std::thread> ts;
        for (uint32_t k = 0; k < 4; ++k)
            ts.emplace_back([&fn, count, k] {
                for (uint32_t i = k; i < count; i += 4) fn(i, i + 1);
            });
        for (std::thread& t : ts) t.join();
    };
    const Run ref = run(nullptr);
    uint32_t evals_in_place = 0;
    for (const std::string& s : ref.trace) evals_in_place += s.rfind("eval ", 0) == 0 ? 1u : 0u;
    CHECK(evals_in_place == ref.counted && ref.draws > 0 && ref.prepared == 0);
    for (const ParallelFor* pf : {&serial, &reverse, &threads}) {
        const Run b = run(pf);
        // The same evaluations (in place or committed from the batch) and accepts, in the same order.
        CHECK(b.crc == ref.crc && b.draws == ref.draws && b.counted == ref.counted && b.trace == ref.trace);
        CHECK(b.prepared > 0 && b.prepared < b.counted);
    }
    CHECK(fan_outs > 0);
    std::printf("goal batched values: %u evaluations, %llu draws, %zu ticks identical, %u fan-outs\n", ref.counted,
                static_cast<unsigned long long>(ref.draws), ref.crc.size(), fan_outs);
}

void test_plan_pick_and_solver() {
    // One synced draw: r = range_float(0, total), first cumulative >= r.
    SimRng a(1234), b(1234);
    const double ratios[] = {1.0, 0.0, 0.5};  // weights 2, 1, 1.5 -> cum 2, 3, 4.5
    const uint32_t k = pick_weighted(ratios, a);
    const float r = b.range_float(0.0f, 4.5f);
    const uint32_t want = r <= 2.0f ? 0u : (r <= 3.0f ? 1u : 2u);
    CHECK(k == want && a.state() == b.state() && a.draws() == 1);
    // Solver core (AI_GALACTIC_2 §1.2): strictly highest, ties -> lowest index, 1e18 veto (permanent erases),
    // 50 % same-type rule, setups A/B/C in order, finalize.
    struct M final : PpMarshaller {
        std::vector<float> s;
        std::vector<uint32_t> picked;
        std::string order;
        uint32_t limit = 3;
        void setup_a() override { order += "A"; }
        void setup_b() override { order += "B"; }
        void setup_c() override { order += "C"; }
        float score(uint32_t i) override { return picked.size() >= limit ? kAiBigFloat : s[i]; }
        void commit(uint32_t i) override { picked.push_back(i); }
        void finalize(bool& ok) override { ok = ok && picked.size() == limit; }
    } m;
    m.s = {1.0f, 5.0f, 5.0f, kAiBigFloat};
    const uint32_t types[] = {3, 7, 7, 9};
    PpMarshaller* ms[] = {&m};
    PpSolverIo io;
    io.candidate_count = [] { return size_t{4}; };
    io.type_of = [&types](uint32_t c) { return types[c]; };
    std::vector<uint32_t> picks;
    const bool solved = run_pp_solver(ms, io, picks);
    // Pick 1 (tie 1/2 -> lowest); then type 7 fills 100 % -> 1 and 2 vetoed while 0 remains -> 0; then types 3 and
    // 7 both fill 50 %: 0 and 1 are vetoed in candidate order, 2 is the only viable one left and stays -> 2.
    CHECK(solved && m.order == "ABC" && picks.size() == 3 && picks[0] == 1 && picks[1] == 0 && picks[2] == 2);
    CHECK(pp_marshaller_order(AiMode::Galactic).size() == 9 && pp_marshaller_order(AiMode::Galactic)[1] == PpMarshallerKind::Budget &&
          pp_marshaller_order(AiMode::Land)[2] == PpMarshallerKind::Reinforcement &&
          pp_marshaller_order(AiMode::Space)[1] == PpMarshallerKind::TechTree);
    // Team specs (§1.3): a,0 -> max a; percents renormalised over the plan.
    {
        PlanDefinition pd;
        TaskForceDef ta, tb;
        CHECK(parse_task_force({"A", "Frigate = 3,0", "Corvette = 30%"}, ta) && parse_task_force({"B", "Infantry = 10%"}, tb));
        CHECK(ta.teams[0].min == 3 && ta.teams[0].max == 3 && ta.teams[1].percent && ta.teams[1].min == 0 && ta.teams[1].max == 0);
        pd.task_forces = {ta, tb};
        renormalise_percents(pd);
        CHECK(pd.task_forces[0].teams[1].percent_value == 75.0f && pd.task_forces[1].teams[0].percent_value == 25.0f);
    }
    // Plan events (§5.2).
    PlanEvent ev{};
    CHECK(plan_event_name(PlanEvent::FleetDestroyed) == "Fleet_Destoyed" && plan_event_code("no_units_remaining", ev) &&
          ev == PlanEvent::NoUnitsRemaining && static_cast<int>(PlanEvent::ProductionFailed) == 4);
    // TaskForce entry syntax.
    TaskForceDef tf;
    CHECK(parse_task_force({"SpaceForce", "MinimumTotalSize = 4", "MinimumTotalForce = 2500",
                            "Frigate | Capital | Corvette = 100%", "DenyHeroAttach", "-Death_Star", "Infantry = 0,4"},
                           tf));
    CHECK(tf.name == "SpaceForce" && tf.minimum_total_size == 4 && tf.minimum_total_force == 2500.0f);
    CHECK(tf.teams.size() == 2 && tf.teams[0].percent && tf.teams[0].names.size() == 3 && tf.teams[1].min == 0 &&
          tf.teams[1].max == 4 && tf.deny_hero_attach && tf.excluded_types.size() == 1 && tf.percent_based());
}

void test_definitions_and_templates() {
    AiDefinitions d;
    d.add_goals(kSynGoals, "goals.xml");
    d.link();
    d.add_function_set("SynSet", kSynSet, "SynSet.xml");
    d.add_templates(kSynTemplates, "t.xml");
    char buf[1024];
    std::snprintf(buf, sizeof buf, kSynPlayer, "SynEmpire");
    d.add_players(buf, "p.xml");
    CHECK(d.goals.size() == 4 && d.diagnostics.empty());
    const uint32_t cq = d.find_goal("conquer_planet");
    CHECK(cq == 1 && d.goals[cq].application == (app::Enemy | app::Neutral) && d.goals[cq].time_limit == 240.0f);
    CHECK(d.goals[cq].reachability == Reachability::EnemyDestination && d.goals[cq].is_like.size() == 1);
    {  // bkg3 (BKG3_RE_ANSWERS Q4): the like-set is reflexive and symmetric (the listed type gets the lister too)
        const uint32_t bs = d.find_goal("Build_Space_Forces");
        CHECK(bs != kAiNone);
        if (bs != kAiNone) {
            CHECK(d.goals[cq].like(cq) && d.goals[cq].like(bs) && d.goals[bs].like(cq) && d.goals[bs].like(bs));
            CHECK(d.goals[cq].like_set.size() == 2 && d.goals[bs].like_set.size() == 2 && d.goals[bs].is_like.empty());
            for (uint32_t t = 0; t < d.goals.size(); ++t)
                if (t != cq && t != bs) CHECK(d.goals[t].like_set.size() == 1 && !d.goals[t].like(cq));
        }
    }
    CHECK(d.goals[3].mode == AiMode::Land && d.goals[2].reachability == Reachability::FriendlyOnly);
    CHECK(d.find_set("synset") && d.find_set("SynSet")->entries.size() == 4);
    CHECK(d.find_set("SynSet")->entries[2].function == "Needs_Starbase_Upgrade");
    // a2m r1: a repeated <Function> is one scalar field, the last occurrence wins (vanilla BasicDefensiveGalacticSet
    // Generate_Magic_Space_Defense: Zero, then Needs_Magic_Space_Defense; retail spawns the magic defense).
    d.add_function_set("RepSet", R"(<FunctionSet>
  <M><Goal>Upgrade_Starbase</Goal><Function>Zero</Function> <!-- x --> <Function> Needs_Magic </Function></M>
</FunctionSet>)", "RepSet.xml");
    CHECK(d.find_set("RepSet") && d.find_set("RepSet")->entries.size() == 1 &&
          d.find_set("RepSet")->entries[0].function == "Needs_Magic");
    const AiPlayerTypeDef* p = d.find_player("SynEmpire");
    CHECK(p && p->templates[0].size() == 2 && p->free_store_script[0] == "SynFreeStore");
    // Template switches: base turns Offensive + Infrastructure on (priority 1); Syn_Rich (priority 3)
    // turns Upgrade_Starbase off while active.
    PlanLibrary lib;
    TemplateSystem t;
    t.init(d, lib, {0u, 1u});
    bool rich = false;
    auto eval = [&rich](const std::string& eq, double& out) {
        out = eq == "One" ? 1.0 : (eq == "Is_Rich" ? (rich ? 1.0 : 0.0) : 0.0);
        return true;
    };
    t.first_service(eval);
    CHECK(t.goal_value(cq) == 1 && t.goal_value(2) == 1 && t.goal_value(3) == -1);
    CHECK(t.budget_equation(d.find_category("Offensive")) == "Offensive_Budget");
    CHECK(t.last_applied().size() == 1 && t.last_applied()[0] == 0u && !t.last_removed());  // the base (§9.5 inserts)
    rich = true;
    t.service(eval);  // count = 1: one template checked per frame (cursor: base)
    CHECK(t.last_applied().empty() && !t.last_removed());
    t.service(eval);  // Syn_Rich
    CHECK(t.active().size() == 2 && t.goal_value(2) == -3 && t.goal_value(cq) == 1);
    CHECK(t.last_applied().size() == 1 && t.last_applied()[0] == 1u && !t.last_removed());
    rich = false;
    t.service(eval);
    CHECK(t.active().size() == 1 && t.goal_value(2) == 1);
    CHECK(t.last_applied().empty() && t.last_removed());  // 0x612ac0: the budget's reset flag
    // Budget (AI_GALACTIC_2 §3): init, add goal, tick (asymmetric branch, drift), reweight, can fund, release.
    AiBudget b;
    b.reset(3);
    for (const int32_t c : {0, 1, 2}) CHECK(b.add_member(c));  // §9.5: the members, insertion order
    const float raw[] = {3.0f, -1.0f, 1.0f};
    b.init(raw, 1000.0f);
    CHECK(b.weight(0) == 0.75f && b.weight(1) == 0.0f && b.weight(2) == 0.25f);
    CHECK(b.available(0) == 750.0f && b.pool(0) == 750.0f && b.rate(0) == 0.0f);
    CHECK(b.can_fund(0, 750.0f, 10.0f, -1.0f) && !b.can_fund(0, 751.0f, 10.0f, -1.0f));
    CHECK(b.add_goal(0, 7, 500.0f, 30.0f, 20.0f));
    const AiBudget::GoalEntry* e = b.goal(0, 7);
    CHECK(e && e->req == 500.0f && e->h == 20.0f && e->c == 500.0f && b.available(0) == 250.0f && e->q == 0.0f);
    // a2m r3 (A2M_R3_RE_A §1.1): the accept-time gross test 0x6139f0 reads P / I, not A / R: the goal's 500 is held out
    // of A (250) but not out of P (750).
    CHECK(!b.can_fund(0, 300.0f, 10.0f, -1.0f) && b.can_fund_gross(0, 750.0f, 10.0f, -1.0f) && !b.can_fund_gross(0, 751.0f, 10.0f, -1.0f));
    // Tick: income 450 over T = 45 s -> rate 10; I = 0.75 x 10; share 0 (Iprev 0); x = 500/20 = 25 -> a = 1125.
    b.tick(450.0f, 45.0f, 1450.0f);
    e = b.goal(0, 7);
    CHECK(e && e->a == 25.0f * 45.0f && e->h == 0.0f && b.category(0).I == 7.5f);
    // 0x6139f0 horizon: min(ETA, Time_Limit), at least the period T (45 s): P + 45 x I.
    {
        const float p0 = b.pool(0) + 45.0f * 7.5f;
        CHECK(b.can_fund_gross(0, p0, 10.0f, 5.0f) && !b.can_fund_gross(0, p0 + 1.0f, 10.0f, 5.0f));
        CHECK(b.can_fund_gross(0, b.pool(0) + 60.0f * 7.5f, 90.0f, 60.0f) && !b.can_fund_gross(0, b.pool(0) + 60.0f * 7.5f + 1.0f, 90.0f, 60.0f));
    }
    // P0 = 250 + (0 - 25) x 45 + 500 + 1125 = 750; Σ = 750 + 0 + 250 = 1000 -> drift (1450 - 1000) x w.
    CHECK(b.pool(0) == 750.0f + 450.0f * 0.75f && b.pool(2) == 250.0f + 450.0f * 0.25f);
    b.reweight(raw);  // A = P - a, c = 0, needs re-add
    e = b.goal(0, 7);
    CHECK(e && e->readd && e->c == 0.0f && b.available(0) == b.pool(0) - 1125.0f && b.rate(0) == 7.5f);
    b.add_goal(0, 7, 0.0f, 0.0f, -1.0f);  // re-add: need = 500 + 0 - 1125 < 0 -> given back
    e = b.goal(0, 7);
    CHECK(e && !e->readd && e->a == 500.0f && e->c == 0.0f && b.available(0) == b.pool(0) - 500.0f);
    CHECK(b.can_fund_goal(0, 7));
    const float before = b.available(0);
    b.release(0, 7, 0.0f);
    CHECK(!b.has_goal(0, 7) && b.available(0) == before + 500.0f);
    const float zero[] = {0.0f, 0.0f, 0.0f};
    b.reweight(zero);
    CHECK(std::fabs(b.weight(1) - 1.0f / 3.0f) < 1e-7f);
    // a2k t10 (A2K_T10_CREDIT_NOTIFY_RE Q2) [H]: a notified Add_Credits adds x * w to every member's P (negative too); A, R
    // and the goal structs are untouched, and the next reweight (A = P) makes it spendable.
    const float p0 = b.pool(0), p2 = b.pool(2), a0 = b.available(0), r0 = b.rate(0);
    b.notify(300.0f);
    CHECK(b.pool(0) == 300.0f * b.weight(0) + p0 && b.pool(2) == 300.0f * b.weight(2) + p2);
    CHECK(b.available(0) == a0 && b.rate(0) == r0);
    b.notify(-300.0f);
    CHECK(std::fabs(b.pool(0) - p0) < 1e-3f && std::fabs(b.pool(2) - p2) < 1e-3f);
    b.notify(90.0f);
    b.reweight(zero);
    CHECK(b.available(0) == b.pool(0));
}

// AI_GALACTIC_TOKENS §9.5 [H]: the galactic budget holds only the categories the applied templates listed, in insertion
// order, never removed; a member without an equation weighs 0 but counts (Σ = 0 -> 1 / members, not 1 / categories); the
// reset flag makes the next income tick re-split every pool to credits x w; a non-member keeps its zeros.
void test_budget_membership() {
    AiBudget b;
    b.reset(5);
    CHECK(b.add_member(3) && b.add_member(1) && !b.add_member(3) && !b.add_member(7) && !b.add_member(-1));
    CHECK(b.members().size() == 2 && b.members()[0] == 3 && b.members()[1] == 1 && b.is_member(1) && !b.is_member(0));
    b.mark_reset();
    const float raw[] = {9.0f, 2.0f, 9.0f, 6.0f, 9.0f};  // only members 3 and 1 are read
    b.init(raw, 1000.0f);
    CHECK(!b.reset_pending());  // Init clears the flag set by the inserts before it
    CHECK(b.weight(3) == 0.75f && b.weight(1) == 0.25f && b.weight(0) == 0.0f && b.weight(2) == 0.0f && b.weight(4) == 0.0f);
    CHECK(b.pool(3) == 750.0f && b.pool(1) == 250.0f && b.pool(0) == 0.0f && b.pools_total() == 1000.0f);
    const float zero[5] = {};
    b.reweight(zero);  // Σ = 0: 1 / members
    CHECK(b.weight(3) == 0.5f && b.weight(1) == 0.5f && b.weight(0) == 0.0f);
    // The template that listed category 1 was removed: its equation is gone, so the caller passes 0; it stays a member.
    const float only3[] = {0.0f, 0.0f, 0.0f, 6.0f, 0.0f};
    b.reweight(only3);
    CHECK(b.weight(3) == 1.0f && b.weight(1) == 0.0f && b.is_member(1) && b.members().size() == 2);
    // No reset: the pools keep their split (P = A, + the drift x w).
    b.tick(100.0f, 45.0f, 1100.0f);
    CHECK(b.pool(3) == 750.0f + 100.0f && b.pool(1) == 250.0f);
    // A template change set the flag: the next tick makes every pool Σ P x w + drift x w = credits x w, then clears it.
    b.mark_reset();
    b.tick(100.0f, 45.0f, 1200.0f);
    CHECK(!b.reset_pending() && b.pool(3) == 1200.0f && b.pool(1) == 0.0f && b.pool(0) == 0.0f);
    std::printf("budget membership: members {3, 1}; Σ=0 -> 1/2; removed category weight 0; reset -> credits x w\n");
}

// AI_GALACTIC_TOKENS §9.8 [H]: the contrast arithmetic (galactic_pp.cpp ContrastM).
void test_contrast_rules() {
    // 0x5f1990 typed-row scale: (double)Max + (double)(float)(f x PFCA), then x the multiplier as double; not the float sum.
    const float fp = static_cast<float>(1) * 0.05f;  // ConquerPiratePlan f = 1
    CHECK(galactic_contrast_scale(1.15f, fp, 1.0f) == static_cast<double>(1.15f) + static_cast<double>(fp));
    CHECK(galactic_contrast_scale(1.5f, 0.5f, 0.9f) == 2.0 * static_cast<double>(0.9f));
    // List (§9.2): entry 0 the ground total, entry 1 the space total, two typed ground rows, two typed space rows.
    const std::vector<GalacticContrastEntry> list = {{-1, false, 1000.0}, {-1, true, 400.0}, {0, false, 600.0},
                                                     {1, false, 0.0},     {0, true, 300.0},  {1, true, 100.0}};
    // Only the ground total is still short: a space-only candidate is not "still allowed" by it, a ground one is (the
    // pre-fix port read remaining[0] for space and so swapped the two).
    std::vector<double> rem = {50.0, -20.0, 0.0, 0.0, -5.0, 0.0};
    CHECK(!galactic_contrast_total_open(list, rem, true, false));
    CHECK(galactic_contrast_total_open(list, rem, false, true));
    CHECK(galactic_contrast_total_open(list, rem, true, true));
    rem[0] = -1.0;
    rem[1] = 0.5;  // only the space total is short
    CHECK(galactic_contrast_total_open(list, rem, true, false) && !galactic_contrast_total_open(list, rem, false, true));
    // The totals are found by their layer byte, not by position.
    const std::vector<GalacticContrastEntry> swapped = {{-1, true, 400.0}, {-1, false, 1000.0}};
    const std::vector<double> rem2 = {-1.0, 10.0};  // ground short
    CHECK(galactic_contrast_total_open(swapped, rem2, false, true) && !galactic_contrast_total_open(swapped, rem2, true, false));
    // 0x72ac70 in float: ground typed rows 600 + 0 = 600; residual = (90/600) x (600/600) = 0.15 (row 1 has target 0).
    rem = {0.0, 0.0, 90.0, 7.0, 0.0, 0.0};
    CHECK(galactic_contrast_layer_satisfied(list, rem, false, 0.15f));
    CHECK(!galactic_contrast_layer_satisfied(list, rem, false, 0.149f));
    CHECK(galactic_contrast_layer_satisfied(list, rem, true, 0.0f));  // space rows met (rem <= 0)
    rem[5] = 50.0;  // space: (50/100) x (100/400) = 0.125
    CHECK(galactic_contrast_layer_satisfied(list, rem, true, 0.125f) && !galactic_contrast_layer_satisfied(list, rem, true, 0.12f));
    // An empty layer (no typed rows, total 0) is satisfied at any tolerance >= 0.
    CHECK(galactic_contrast_layer_satisfied(swapped, rem2, true, 0.0f));
    // The gate: the plan-wide flags with the candidate's team kind, not its own layers.
    CHECK(galactic_contrast_gate(true, false, true, false));
    CHECK(!galactic_contrast_gate(false, false, true, true));
    CHECK(!galactic_contrast_gate(true, false, false, true));  // escort-only candidate, escort contrast not met
    CHECK(galactic_contrast_gate(false, true, false, true));
    std::printf("contrast rules: totals by layer byte, float satisfied test, Max scale widened before the add, plan-wide gate\n");
}

gal::GalacticRules syn_rules() {
    gal::GalacticRules r;
    gal::FactionRules emp;
    emp.name = "Empire";
    emp.can_control_planets = emp.is_playable = true;
    gal::FactionRules reb = emp;
    reb.name = "Rebel";
    gal::FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, reb, neu};
    r.difficulties = {{"Normal_Default", 1.0f, 1.0f}};
    r.types.resize(tTypes);
    r.types[tPlanet].name = "SynPlanet";
    r.types[tPlanet].kind = gal::UnitKind::Planet;
    r.types[tPlanet].credit_value = 40;
    auto unit = [&r](uint32_t id, const char* name, gal::UnitKind k, int32_t cost, int32_t secs, float power) -> gal::TypeInfo& {
        gal::TypeInfo& t = r.types[id];
        t.name = name;
        t.kind = k;
        t.build_cost = cost;
        t.build_time_seconds = secs;
        t.ai_combat_power = power;
        return t;
    };
    unit(tSB1, "SB1", gal::UnitKind::StarBase, 900, 10, 600.0f).base_level = 1;
    gal::TypeInfo& sb2 = unit(tSB2, "SB2", gal::UnitKind::StarBase, 1500, 15, 1200.0f);
    sb2.base_level = 2;
    sb2.required_star_base = 1;
    gal::TypeInfo& fr = unit(tFrigate, "Frigate", gal::UnitKind::SpaceUnit, 700, 12, 1000.0f);
    fr.required_star_base = 1;
    fr.hyperspace = true;
    fr.hyperspace_speed = 2.0f;
    gal::TypeInfo& co = unit(tCorvette, "Corvette", gal::UnitKind::SpaceUnit, 400, 8, 500.0f);
    co.required_star_base = 1;
    co.hyperspace = true;
    co.hyperspace_speed = 3.0f;
    gal::TypeInfo& inf = unit(tInfantry, "Infantry", gal::UnitKind::GroundUnit, 250, 6, 200.0f);
    inf.required_ground_base = 1;
    inf.hyperspace = true;  // its own transport (synthetic)
    inf.hyperspace_speed = 1.5f;
    unit(tBarracks, "Barracks", gal::UnitKind::GroundStructure, 500, 10, 0.0f);
    return r;
}

gal::GalacticSetup syn_setup() {
    gal::GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "SyntheticAI";
    const int n = 8;
    for (int i = 0; i < n; ++i) {
        m.planets.add("P" + std::to_string(i));
        m.planets.position[static_cast<size_t>(i)] = {static_cast<float>(i) * 60.0f, static_cast<float>((i % 2) * 40), 0.0f};
        m.planets.flags[static_cast<size_t>(i)] = kPlanetHasPlanetBehavior;
        m.planets.max_space_base[static_cast<size_t>(i)] = 2;
        m.planets.max_ground_base[static_cast<size_t>(i)] = 2;
    }
    for (int i = 0; i + 1 < n; ++i) {
        const uint32_t r = m.routes.add("R" + std::to_string(i));
        m.routes.point_a[r] = static_cast<PlanetIndex>(i);
        m.routes.point_b[r] = static_cast<PlanetIndex>(i + 1);
        m.routes.hs_speed_factor[r] = 4.0f;
        m.routes.credit_gain_factor[r] = 0.0f;
    }
    s.planet_type.assign(static_cast<size_t>(n), tPlanet);
    gal::PlayerSetup e;
    e.faction = 0;
    e.ai = true;
    e.difficulty = 0;
    e.credits = 6000.0f;
    gal::PlayerSetup r = e;
    r.faction = 1;
    gal::PlayerSetup neu;
    neu.faction = 2;
    s.players = {e, r, neu};
    s.forces = {{0, 0, tSB1}, {0, 0, tBarracks}, {0, 0, tFrigate}, {0, 1, tInfantry}, {1, 7, tSB1}, {1, 7, tBarracks}, {1, 7, tFrigate}, {1, 6, tInfantry}};
    return s;
}

void syn_data(MemoryFiles& files, AiDefinitions& defs, EquationSet& eqs, std::vector<std::string>& plan_files) {
    files.add("Data\\Scripts\\Library\\SynBase.lua", kSynBase);
    files.add("Data\\Scripts\\Library\\SynAICommands.lua", kSynAICommands);
    files.add("Data\\Scripts\\AI\\BuildSpace.lua", kPlanBuildSpace);
    files.add("Data\\Scripts\\AI\\Conquer.lua", kPlanConquer);
    files.add("Data\\Scripts\\AI\\UpgradeStarbase.lua", kPlanUpgrade);
    files.add("Data\\Scripts\\FreeStore\\SynFreeStore.lua", kSynFreeStore);
    defs.add_goals(kSynGoals, "goals.xml");
    defs.link();
    defs.add_function_set("SynSet", kSynSet, "SynSet.xml");
    defs.add_templates(kSynTemplates, "t.xml");
    char buf[1024];
    for (const char* n : {"SynEmpire", "SynRebel"}) {
        std::snprintf(buf, sizeof buf, kSynPlayer, n);
        defs.add_players(buf, "p.xml");
    }
    eqs.add("One", "1.0");
    eqs.add("Zero", "0.0");
    eqs.add("Is_Rich", "Variable_Self.CreditsUnnormalized > 20000");
    eqs.add("Allow_Space", "(Variable_Self.CreditsUnnormalized >= 1500) * 2 + (Game.EnemyForce.SpaceTotalUnnormalized > Game.FriendlyForce.SpaceTotalUnnormalized)");
    eqs.add("Conquer_Desire",
            "(3 + 2 * (Variable_Target.EnemyForce.HasSpaceForce == 0) + Variable_Target.BaseIncome) * "
            "(Variable_Self.SpaceTotalUnnormalized >= 1000)");  // the starting frigate (a2h: starbases no longer count)
    eqs.add("Needs_Starbase_Upgrade",
            "(Variable_Target.StarbaseLevel < Variable_Target.MaxStarbaseLevel) * (Variable_Self.CreditsUnnormalized >= 1000) * 4");
    eqs.add("Offensive_Budget", "5 + 10 * Variable_Self.PlanetsControlled");
    eqs.add("Infra_Budget", "3");
    eqs.link();
    plan_files = list_plan_files([&files](std::string_view glob) {
        const std::string g(glob);
        return files.list(g.substr(0, g.find('*')));
    });
}

ParallelFor thread_pf(unsigned threads) {
    return [threads](uint32_t count, uint32_t grain, const RangeFn& fn) {
        if (count == 0) return;
        const uint32_t gr = std::max<uint32_t>(1, grain);
        const uint32_t chunks = (count + gr - 1) / gr;
        std::atomic<uint32_t> next{0};
        auto worker = [&] {
            for (;;) {
                const uint32_t c = next.fetch_add(1);
                if (c >= chunks) break;
                fn(c * gr, std::min(count, c * gr + gr));
            }
        };
        const unsigned extra = std::min<unsigned>(std::max(1u, threads), chunks) - 1;
        std::vector<std::thread> ts;
        for (unsigned t = 0; t < extra; ++t) ts.emplace_back(worker);
        worker();
        for (auto& t : ts) t.join();
    };
}

CampaignResult run_synthetic(unsigned threads, int32_t frames) {
    CampaignResult out;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    const gal::GalacticRules rules = syn_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    sim.set_parallel_for(thread_pf(threads));
    std::vector<std::string> diag;
    if (!sim.start(syn_setup(), &diag)) {
        std::printf("synthetic: start failed\n");
        ++g_fail;
        return out;
    }
    gal::ConflictRules crules;
    gal::ConflictManager conflicts(sim, crules, rng.synced());
    conflicts.attach();
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    tinfo[tFrigate].categories = {"FRIGATE"};
    tinfo[tCorvette].categories = {"CORVETTE"};
    tinfo[tInfantry].categories = {"INFANTRY"};
    tinfo[tSB1].categories = tinfo[tSB2].categories = {"STRUCTURE"};
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    const SimRng synced_before_world = rng.synced();
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    CHECK(rng.synced() == synced_before_world);  // A2k t04: the queue phases come from the galaxy build, not drawn here
    std::vector<std::string> pd;
    CHECK(world.load_plans(plan_files, &pd));
    for (const std::string& d : pd) std::printf("  plans: %s\n", d.c_str());
    CHECK(world.plans().plans.size() == 3);
    CHECK(world.start(&pd));
    for (int32_t f = 1; f <= frames; ++f) {
        sim.update();  // T5: the galactic tick
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p)
            world.update_player(p, f, true, conflicts.state() == gal::ConflictState::AutoRunning);  // T7
        StateHasher h;
        sim.hash_state(h);
        world.hash(h);
        rng.synced().hash(h);
        conflicts.hash(h);
        out.crc.push_back(h.digest());
    }
    for (const AiDecision& d : world.decision_log())
        out.decisions.push_back(std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text);
    out.errors = world.lua_errors();
    for (size_t p = 0; p < sim.players().size(); ++p) out.owned.push_back(sim.planets_owned(static_cast<int32_t>(p)));
    out.orders = world.orders_issued();
    out.rejected = world.orders_rejected();
    out.conflicts = conflicts.log().size();
    char buf[512];
    std::snprintf(buf, sizeof buf, "frames %d planets E=%d R=%d N=%d credits E=%.0f R=%.0f orders %llu rejected %llu conflicts %llu plans running %zu",
                  frames, out.owned[0], out.owned[1], out.owned[2], static_cast<double>(sim.credits(0)),
                  static_cast<double>(sim.credits(1)), static_cast<unsigned long long>(out.orders),
                  static_cast<unsigned long long>(out.rejected), static_cast<unsigned long long>(out.conflicts), world.running_plans());
    out.summary = buf;
    return out;
}

std::string golden_text(const CampaignResult& r) {
    std::ostringstream o;
    o << "# A2 synthetic AI-vs-AI campaign: per-frame CRC (every 150th frame + last), decisions\n";
    for (size_t i = 0; i < r.crc.size(); ++i)
        if ((i + 1) % 150 == 0 || i + 1 == r.crc.size()) {
            char b[64];
            std::snprintf(b, sizeof b, "frame %zu %016llx\n", i + 1, static_cast<unsigned long long>(r.crc[i]));
            o << b;
        }
    o << r.summary << "\n";
    for (const std::string& d : r.decisions) o << d << "\n";
    return o.str();
}

// OPT3 t09: PlanScriptRuntime::load builds the load VMs through a ParallelFor and commits them in path order, so a
// threaded load equals the serial one: library, pools, pristine digests, VM count and diagnostics (a script error, a
// missing file and a duplicate plan name included).
void test_plan_load_parallel() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    files.add("Data\\Scripts\\AI\\Broken.lua", "require(\"SynAICommands\")\nfunction Definitions(\n");
    files.add("Data\\Scripts\\AI\\LandMode\\Conquer.lua", kPlanConquer);  // same name as AI\Conquer: dropped
    plan_files.insert(plan_files.begin() + 1, "Data\\Scripts\\AI\\Broken.lua");
    plan_files.push_back("Data\\Scripts\\AI\\Missing.lua");
    plan_files.push_back("Data\\Scripts\\AI\\LandMode\\Conquer.lua");
    objects::GameObjectTypes types;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types);
    GameRandom rng(1u, 2u);
    struct Result {
        size_t loaded = 0, created = 0;
        std::vector<std::string> names, diag;
        std::vector<size_t> pooled;
        std::vector<uint64_t> digests;
    };
    auto run = [&](const ParallelFor& pf) {
        PlanScriptRuntime::VmSetup setup;
        setup.host = [&] {
            lua::LuaHost h;
            h.store = &store;
            h.types = &types;
            h.rng = &rng;
            h.files = &files;
            return h;
        };
        PlanScriptRuntime rt(std::move(setup));
        PlanLibrary lib;
        Result r;
        r.loaded = rt.load(files, plan_files, defs, lib, &r.diag, pf);
        r.created = rt.vms_created();
        for (uint32_t i = 0; i < lib.plans.size(); ++i) {
            const PlanDefinition& d = lib.plans[i];
            r.names.push_back(d.name + "|" + d.file + "|" + std::to_string(d.folder) + "|" + std::to_string(d.task_forces.size()) +
                              "|" + std::to_string(d.pool_count));
        }
        for (uint32_t i = 0; i < rt.plan_count(); ++i) {
            r.pooled.push_back(rt.pooled(i));
            r.digests.push_back(rt.pristine_digest(i));
        }
        return r;
    };
    const Result serial = run(serial_for);
    CHECK(serial.loaded == 3 && serial.names.size() == 3);  // BuildSpace, Conquer, UpgradeStarbase
    CHECK(serial.diag.size() >= 3);                         // the script error, the missing file, the duplicate
    for (unsigned threads : {2u, 4u, 8u}) {
        const Result par = run(thread_pf(threads));
        CHECK(par.loaded == serial.loaded);
        CHECK(par.created == serial.created);
        CHECK(par.names == serial.names);
        CHECK(par.diag == serial.diag);
        CHECK(par.pooled == serial.pooled);
        CHECK(par.digests == serial.digests);
    }
}

void test_synthetic_campaign() {
    const int32_t frames = 30 * 900;  // 15 minutes of galactic time
    const CampaignResult base = run_synthetic(1, frames);
    std::printf("synthetic campaign (1 thread): %s, %zu decisions, %zu Lua errors\n", base.summary.c_str(),
                base.decisions.size(), base.errors.size());
    for (size_t i = 0; i < base.errors.size() && i < 10; ++i) std::printf("  lua error: %s\n", base.errors[i].c_str());
    CHECK(base.errors.empty());
    CHECK(base.orders > 0 && base.rejected * 4 < base.orders + 1);
    CHECK(base.owned.size() == 3 && base.owned[0] + base.owned[1] > 4);  // the AIs expanded
    for (const unsigned t : {2u, 4u, 15u}) {
        const CampaignResult r = run_synthetic(t, frames);
        size_t first = r.crc.size() == base.crc.size() ? r.crc.size() : 0;
        for (size_t i = 0; i < std::min(r.crc.size(), base.crc.size()); ++i)
            if (r.crc[i] != base.crc[i]) {
                first = i;
                break;
            }
        std::printf("  threads %u vs 1: %s\n", t, first == r.crc.size() ? "identical" : ("DIVERGED at frame " + std::to_string(first + 1)).c_str());
        CHECK(first == r.crc.size() && r.decisions == base.decisions);
    }
    const std::string text = golden_text(base);
    const std::string path = std::string(ALAMO_AI_GOLDEN_DIR) + "/ai_galactic_synthetic_crc.txt";
    if (g_write_golden) {
        std::ofstream(std::filesystem::path(path), std::ios::binary) << text;
        std::printf("  wrote %s\n", path.c_str());
    } else {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        std::string want((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        want.erase(std::remove(want.begin(), want.end(), '\r'), want.end());
        const bool same = want == text;
        std::printf("  golden %s: %s\n", path.c_str(), same ? "MATCH" : (want.empty() ? "MISSING" : "DIFFERS"));
        if (!same) {
            const std::string actual = std::string(ALAMO_AI_LOG_DIR) + "/ai_galactic_synthetic_actual.txt";
            std::ofstream(std::filesystem::path(actual), std::ios::binary) << text;
            std::printf("  actual written to %s\n", actual.c_str());
        }
        CHECK(same);
    }
}

// AI execution path finder in the world (GALACTIC.md §4, ai_path_finder.hpp): lazy creation by a query before the
// first service, creation at the first service (stagger), the 90-frame recompute in the Execution service, Get_Path
// answers, non-AI players rejected, and the "ai.paths" state identical for any thread count.
std::vector<uint64_t> run_path_world(unsigned threads, int32_t frames, lua::LuaPlayerWorld* story) {
    std::vector<uint64_t> crc;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    const gal::GalacticRules rules = syn_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    sim.set_parallel_for(thread_pf(threads));
    std::vector<std::string> diag;
    if (!sim.start(syn_setup(), &diag)) {
        ++g_fail;
        return crc;
    }
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    cfg.path_pf = thread_pf(threads);
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    std::vector<std::string> pd;
    CHECK(world.load_plans(plan_files, &pd) && world.start(&pd));
    {   // GRC 2 visibility in the S2 snapshot: AI rows only with fog on (presence level, -1 on an owned planet).
        const GalacticSnapshot live = snapshot_from_sim(sim);
        const GalacticSnapshot fog = snapshot_from_sim(sim, true);
        CHECK(live.fog_disabled && live.visibility.empty() && !fog.fog_disabled && fog.visibility.size() == 3);
        CHECK(fog.visibility[0].size() == 8 && fog.visibility[2].empty());
        CHECK(sim.planets()[0].owner != 0 || fog.visibility[0][0] == 0xFFFFFFFFu);
        CHECK(fog.visibility[0][7] == sim.visibility_sources(7, 0).presence && fog.validate().empty());
        CHECK(sim.local_player() >= 0 || fog.ownership_visible.empty());  // all-AI galaxy: no local mask
    }
    if (story) {  // Lua Find_Path of a story world answers from the AI path finders while attached
        world.attach_story_world(story);
        CHECK(story->ai_paths() == &world.path_query());
    }
    // Before any service: no finder yet. The Lua-side query does not create it; path() creates it lazily (next = 0)
    // and answers on the 8-planet chain.
    CHECK(world.path_finder(0) && !world.path_finder(0)->created() && !world.path_finder(2));
    std::vector<PlanetIndex> early;
    CHECK(!world.path_query().ai_path(0, 0, 7, early) && early.empty() && !world.path_finder(0)->created());
    CHECK(world.path(0, 0, 7) == (std::vector<PlanetIndex>{0, 1, 2, 3, 4, 5, 6, 7}));
    CHECK(world.path_finder(0)->created() && world.path_finder(0)->next_recompute() == 0);
    std::vector<PlanetIndex> out;
    CHECK(!world.path_query().ai_path(2, 0, 7, out) && out.empty());  // the neutral player has no AI path finder
    CHECK(world.path(2, 0, 7).empty() && world.path(1, 3, 3) == (std::vector<PlanetIndex>{3, 3}));
    CHECK(world.path(1, 0, 99).empty());
    CrcRegistry reg;
    world.register_path_crc(reg);
    for (int32_t f = 1; f <= frames; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) world.update_player(p, f, true, false);
        crc.push_back(reg.capture(static_cast<uint64_t>(f)).combined);
    }
    for (int32_t p = 0; p < 2; ++p) {
        const AiPathFinder* pf = world.path_finder(p);
        CHECK(pf && pf->created());
        // Created at the first service, then a recompute every 90 mode frames (serviced every 3 frames).
        CHECK(pf->recomputes() >= static_cast<uint32_t>(frames / 90) && pf->recomputes() <= static_cast<uint32_t>(frames / 90 + 4));
        CHECK(pf->next_recompute() > sim.frame() - 3 && pf->next_recompute() <= sim.frame() + 90);
        const std::vector<PlanetIndex> fw = world.path(p, 7, 0);
        CHECK(fw.size() >= 2 && fw.front() == 7 && fw.back() == 0);
    }
    return crc;
}

void test_path_finder_world() {
    const int32_t frames = 30 * 40;
    lua::LuaPlayerWorld story;  // outlives the AI world: detached by its destructor
    const std::vector<uint64_t> base = run_path_world(1, frames, &story);
    CHECK(base.size() == static_cast<size_t>(frames) && story.ai_paths() == nullptr);
    for (const unsigned t : {2u, 4u, 15u}) CHECK(run_path_world(t, frames) == base);
}

// a2m r4 (AI_GALACTIC §1.3 [H]): the galactic AI services every player's clocks from the same frame, as the original
// (no per-player phase); GalacticAiConfig::stagger (OPT2 t01, off by default) gives the later AI ordinals a fixed phase.
// Returns per player {planning next, execution next, goal services, planning services} after `frames` frames.
std::vector<std::array<int64_t, 4>> run_clock_phase(bool stagger, int32_t frames) {
    std::vector<std::array<int64_t, 4>> out;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    const gal::GalacticRules rules = syn_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    std::vector<std::string> diag;
    if (!sim.start(syn_setup(), &diag)) {
        ++g_fail;
        return out;
    }
    GalacticAiConfig cfg;
    CHECK(!cfg.stagger);  // the default is the original timing
    cfg.stagger = stagger;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    std::vector<std::string> pd;
    CHECK(world.load_plans(plan_files, &pd) && world.start(&pd));
    for (int32_t f = 1; f <= frames; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) world.update_player(p, f, true, false);
    }
    for (int32_t p = 0; p < 2; ++p) {
        const AiContext* c = world.context(p);
        CHECK(c && c->initialised());
        if (!c) return out;
        out.push_back({c->planning_clock.next, c->execution_clock.next, static_cast<int64_t>(c->service_counts[1]),
                       static_cast<int64_t>(c->service_counts[2])});
    }
    return out;
}

void test_galactic_clock_phase() {
    const int32_t frames = 30;  // ordinal 0 plans on 1, 4, .., 28 (next 31), ordinal 1 on 2, .., 29 (next 32)
    const std::vector<std::array<int64_t, 4>> orig = run_clock_phase(false, frames);
    CHECK(orig.size() == 2 && orig[0] == orig[1]);  // both players on the same frames, same service counts
    const std::vector<std::array<int64_t, 4>> phased = run_clock_phase(true, frames);
    // OPT2 t01: ordinal 1 of 2 starts its planning / goal clocks stagger_offset(1, 2, 3) = 1 frame later.
    CHECK(phased.size() == 2 && phased[0] == orig[0] && phased[1][0] == phased[0][0] + AiContext::stagger_offset(1, 2, 3));
    CHECK(phased.size() == 2 && phased[1][2] + 1 == phased[0][2]);  // one goal service fewer for the later ordinal
}

// A2 galactic: the GalacticSim hooks the galactic AI uses. The §3.1 budget tick comes from the sim's income-tick
// listener (rate = gain / T of that cycle); the world may be destroyed before the sim (the listener goes quiet); fleet
// launches are FleetManagement events through the order sink.
void test_sim_hooks() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    const gal::GalacticRules rules = syn_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    CHECK(sim.start(syn_setup()));
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    tinfo[tFrigate].categories = {"FRIGATE"};
    tinfo[tCorvette].categories = {"CORVETTE"};
    tinfo[tInfantry].categories = {"INFANTRY"};
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    uint32_t fleet_events = 0, fleet_rejected = 0, ticks_seen = 0;
    int32_t f = 1;
    {
        GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                              [&](uint8_t p, const commands::EventData& ev) {
                                  const commands::RejectReason r = sim.execute(p, ev);
                                  if (ev.type() == commands::EventType::FleetManagement) {
                                      ++fleet_events;
                                      fleet_rejected += r == commands::RejectReason::None ? 0u : 1u;
                                  }
                                  return r;
                              },
                              cfg);
        CHECK(world.load_plans(plan_files) && world.start());
        int32_t day = -1;
        for (; f <= 30 * 300; ++f) {
            sim.update();
            for (int32_t p = 0; p < 2; ++p) world.update_player(p, f, true, false);
            const AiContext* c = world.context(0);
            if (!c || !c->budget.initialised()) continue;
            if (day < 0) day = sim.galactic_day();
            if (sim.galactic_day() != day) {  // an income tick after the budget's init
                day = sim.galactic_day();
                ++ticks_seen;
                CHECK(c->budget.period() > 0.0f);
                CHECK(c->budget.income_per_second() == sim.last_income(0) / c->budget.period());
            }
        }
    }
    for (int32_t k = 0; k < 30 * 120; ++k) sim.update();  // listener of the destroyed world: silent, no crash
    std::printf("sim hooks: %u income ticks checked, %u FleetManagement orders (%u rejected)\n", ticks_seen, fleet_events, fleet_rejected);
    CHECK(ticks_seen >= 2);
    CHECK(fleet_events > 0 && fleet_rejected == 0);
}

// A2 t03 (Conquer_Opponent): the force leaf Target.EnemyForce.SpaceTotalUnnormalized holds the starbase's per-tech
// spawned members only (AI_GALACTIC_TOKENS 2: "not the starbase's own power"), typed or not; the starbase's own
// AI_Combat_Power is the helper 0x5f2430's add (AI_GALACTIC_TOKENS 9.2) and lands in every contrast row its category (Capital)
// meets: the untyped total and the Capital row. The gate is "not held by the AI or an ally" (0x4ad9f0).
void test_starbase_force_split() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    eqs.add("Space_All", "Variable_Target.EnemyForce.SpaceTotalUnnormalized");
    eqs.add("Space_Capital", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Capital\"}");
    eqs.add("Space_Frigate", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Frigate\"}");
    eqs.add("Space_Corvette", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Corvette\"}");
    eqs.add("Space_FrigCorv", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Frigate\", Parameter_Category = \"Corvette\"}");
    eqs.add("Ground_All", "Variable_Target.EnemyForce.GroundTotalUnnormalized");
    eqs.link();
    const gal::GalacticRules rules = syn_rules();  // SB1 600, Frigate 1000, Corvette 500
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    gal::GalacticSetup setup = syn_setup();
    setup.forces = {{0, 0, tSB1}, {1, 7, tSB1}};  // the Empire's base at P0, the (hostile) Rebel base at P7, nothing else
    CHECK(sim.start(setup));
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    tinfo[tFrigate].categories = {"FRIGATE"};
    tinfo[tCorvette].categories = {"CORVETTE"};
    tinfo[tSB1].categories = tinfo[tSB2].categories = {"CAPITAL"};
    tinfo[tSB1].spawn_tech[0] = {{tCorvette, 2}, {tFrigate, 1}};  // 2 x 500 + 1 x 1000
    tinfo[tSB1].spawn_tech[3] = {{tFrigate, 9}};                  // above the owner's tech level: never read
    fill_category_masks(tinfo);
    const std::span<const GalacticAiTypeInfo> ti(tinfo);
    const uint32_t cap = static_cast<uint32_t>(ai_category_bit("Capital")), frig = static_cast<uint32_t>(ai_category_bit("Frigate")),
                   corv = static_cast<uint32_t>(ai_category_bit("Corvette"));
    CHECK(cap != 0 && frig != 0 && corv != 0 && sim.planets()[7].starbase && sim.planets()[7].owner == 1);
    CHECK(sim.players()[1].tech_level == 1);  // slot 1 empty: the fallback reads slot 0
    // The per-tech members of a type: never its own power, typed or untyped.
    CHECK(galactic_ai_member_force(sim, ti, tSB1, 1, kAllCategories) == 2000.0);
    CHECK(galactic_ai_member_force(sim, ti, tSB1, 1, cap) == 0.0);  // the starbase is a Capital, its own power is NOT in the row
    CHECK(galactic_ai_member_force(sim, ti, tSB1, 1, frig) == 1000.0);
    CHECK(galactic_ai_member_force(sim, ti, tSB1, 1, corv) == 1000.0);
    CHECK(galactic_ai_member_force(sim, ti, tSB1, 1, frig | corv) == 2000.0);
    CHECK(galactic_ai_member_force(sim, ti, tSB2, 1, kAllCategories) == 0.0);  // no members
    // Player-level tokens use the exact list (0x373fd0 / 0x373ee0): tech 1 has no list, so nothing; the fall-back walk reads slot 0.
    CHECK(galactic_ai_member_force(sim, ti, tSB1, 1, kAllCategories, true) == 0.0);
    // The leaf (members only) and the 0x5f2430 helper (+ the starbase's own power in every row its Capital mask meets).
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, true, kAllCategories) == 2000.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, kAllCategories) == 2600.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, 0) == 2600.0);   // mask 0 = every category
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, cap) == 600.0);  // the Capital row: the starbase's own power
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, cap | frig) == 1600.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, frig) == 1000.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, frig | corv) == 2000.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 0, 0, true, kAllCategories) == 0.0);  // its own base is not enemy strength
    CHECK(galactic_ai_planet_force(sim, ti, 0, 0, false, true, kAllCategories) == 2000.0);  // Friendly side: its members
    // The token leaf on the live sim (Self = Empire, Target = P7).
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    CHECK(world.load_plans(plan_files) && world.start());
    sim.update();
    world.update_player(0, 1, true, false);
    EvalContext c;
    c.self = 0;
    c.target = 7;
    auto value = [&](const char* name) {
        const EvalResult r = evaluate(eqs, eqs.find(name), c, world.resolver());
        CHECK(r.ok);
        return r.value;
    };
    CHECK(value("Space_All") == 2000.0);  // members only: the own 600 is not in the leaf
    CHECK(value("Space_Capital") == 0.0);
    CHECK(value("Space_Frigate") == 1000.0);
    CHECK(value("Space_Corvette") == 1000.0);
    CHECK(value("Space_FrigCorv") == 2000.0);
    CHECK(value("Ground_All") == 0.0);
    // The Empire's own base is not enemy force from its point of view.
    c.target = 0;
    CHECK(value("Space_All") == 0.0);
    // The Contrast list 0x5f1990 (AI_GALACTIC_TOKENS 9.2 layer order): ground total, space total, typed ground rows, typed
    // space rows; a typed row = strength x scale, 0 when its layer total is <= 0 (the ground total is 0 here).
    const uint64_t rows[] = {static_cast<uint64_t>(cap), static_cast<uint64_t>(frig)};
    const std::vector<GalacticContrastEntry> list = galactic_ai_contrast_list(sim, ti, 7, 0, rows, 2.0);
    CHECK(list.size() == 6);
    if (list.size() == 6) {
        CHECK(list[0].row == -1 && !list[0].space && list[0].target == 0.0);    // entry 0: the ground total (layer 1)
        CHECK(list[1].row == -1 && list[1].space && list[1].target == 2600.0);  // entry 1: the space total (layer 0)
        CHECK(list[2].row == 0 && !list[2].space && list[2].target == 0.0);     // ground rows: their layer total is 0
        CHECK(list[3].row == 1 && !list[3].space && list[3].target == 0.0);
        CHECK(list[4].row == 0 && list[4].space && list[4].target == 1200.0);   // Capital: the own 600 x 2
        CHECK(list[5].row == 1 && list[5].space && list[5].target == 2000.0);   // Frigate: the member 1000 x 2
    }
    // Ownership gate 0x4ad9f0: a neutral owner (not hostile, so the leaf's Enemy side is empty) still passes the gate and the
    // starbase's own power counts; an ally's planet adds nothing.
    sim.set_relation(0, 1, gal::Relation::Neutral);
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, true, kAllCategories) == 0.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, kAllCategories) == 600.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, cap) == 600.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, frig) == 0.0);
    sim.set_relation(0, 1, gal::Relation::Ally);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, kAllCategories) == 0.0);
    sim.reset_relations();
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, kAllCategories) == 2600.0);
    // q6 (AI_GALACTIC_TOKENS §9.4, 0x4b8000): the planet's persisted tactical built objects add their types' own power to
    // the ground total and to every ground row their category meets; no owner test (an Empire record at the Rebel planet
    // counts too), nothing in space; the ownership gate still applies.
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, kAllCategories) == 0.0);
    if (std::vector<gal::PersistedBuiltObject>* recs = sim.built_objects(7)) {
        recs->push_back({tFrigate, 10.0f, 20.0f, 0.1f, 0, 1});
        recs->push_back({tCorvette, 30.0f, 20.0f, 0.1f, 1, 0});
        recs->push_back({gal::kNoIndex, 50.0f, 20.0f, 0.1f, 0, 1});  // an unknown type: skipped
    }
    CHECK(sim.built_objects(999) == nullptr);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, kAllCategories) == 1500.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, 0) == 1500.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, frig) == 1000.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, cap) == 0.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, kAllCategories) == 2600.0);  // space: unchanged
    const std::vector<GalacticContrastEntry> glist = galactic_ai_contrast_list(sim, ti, 7, 0, rows, 2.0);
    CHECK(glist.size() == 6 && glist[0].target == 1500.0 && glist[2].target == 0.0 && glist[3].target == 2000.0);
    sim.set_relation(0, 1, gal::Relation::Ally);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, kAllCategories) == 0.0);
    sim.reset_relations();
    // Owner filter 0x4b7d90: only the kept owner's records stay, in order.
    sim.keep_built_objects(7, 1);
    CHECK(sim.planets()[7].built_objects.size() == 2 && sim.planets()[7].built_objects[0].type == tFrigate &&
          sim.planets()[7].built_objects[1].type == gal::kNoIndex);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, kAllCategories) == 1000.0);
    sim.keep_built_objects(7, 0);
    CHECK(sim.planets()[7].built_objects.empty());
    std::printf("starbase force split: leaf 2000 (members), helper 0x5f2430 untyped 2600 (+ own 600), Capital row 600, "
                "Frigate row 1000; neutral owner 600, ally 0; persisted ground objects 1500 (Frigate row 1000)\n");
}

// q6 t18 (AI_GALACTIC_TOKENS §9.9): the perception leaf cache. One node per (AI player, target planet, leaf); the entry key
// is the parameter set only, so an equation, the 0x5f2430 contrast helper (mask 0 = no parameter, a mask = the same entry as
// an equation's Parameter_Category) and any Enemy / Human binding share one entry; a hit is NOW <= stamp + L x 30 (15 s for
// the force leaves); the visibility signal 0xE recomputes and re-stamps the fogged nodes whose mask it covers or just
// stopped covering. GroundbaseTotalUnnormalized (§9.12) has lifetime 0: always live.
void test_leaf_cache() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    eqs.add("Space_All", "Variable_Target.EnemyForce.SpaceTotalUnnormalized");
    eqs.add("Space_Frigate", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Frigate\"}");
    eqs.add("Ground_All", "Variable_Target.EnemyForce.GroundTotalUnnormalized");
    eqs.add("Groundbase", "Variable_Target.EnemyForce.GroundbaseTotalUnnormalized");
    eqs.link();
    gal::GalacticRules rules = syn_rules();
    rules.types[tBarracks].ai_combat_power = 300.0f;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    gal::GalacticSetup setup = syn_setup();
    setup.forces = {{0, 0, tSB1}, {1, 7, tSB1}, {1, 7, tBarracks}, {1, 7, tFrigate}};
    CHECK(sim.start(setup));
    std::vector<GalacticAiTypeInfo> tinfo(tTypes);
    tinfo[tFrigate].categories = {"FRIGATE"};
    tinfo[tCorvette].categories = {"CORVETTE"};
    tinfo[tSB1].categories = tinfo[tSB2].categories = {"CAPITAL"};
    tinfo[tBarracks].categories = {"STRUCTURE"};
    fill_category_masks(tinfo);
    const std::span<const GalacticAiTypeInfo> ti(tinfo);
    const uint64_t frig = static_cast<uint64_t>(ai_category_bit("Frigate"));
    GalacticSimTokenResolver res(sim, ti, nullptr, {});
    const int32_t f0 = sim.frame() + 100;  // a stamp of 0 is never a hit (§9.9): start the resolver clock past it
    res.refresh(f0);
    EvalContext c;
    c.self = 0;
    c.target = 7;
    auto value = [&](const char* name) {
        const EvalResult r = evaluate(eqs, eqs.find(name), c, res);
        CHECK(r.ok);
        return r.value;
    };
    // Shared entries: the equation, the helper's leaf part and an Enemy / Human binding read one entry each.
    CHECK(value("Space_All") == 1000.0);  // the frigate; the starbase's own power is not in the leaf
    const size_t n1 = res.cache_size();
    CHECK(n1 >= 1);
    double v = 0.0;
    CHECK(res.enemy_force(0, 7, true, 0, v) && v == 1000.0 && res.cache_size() == n1);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, 0, nullptr, &res) == 1600.0 && res.cache_size() == n1);
    CHECK(value("Space_Frigate") == 1000.0);
    const size_t n2 = res.cache_size();
    CHECK(n2 == n1 + 1);
    CHECK(res.enemy_force(0, 7, true, frig, v) && v == 1000.0 && res.cache_size() == n2);  // Parameter_Category = mask
    c.enemy = 1;
    c.human = 1;
    CHECK(value("Space_All") == 1000.0 && res.cache_size() == n2);  // Enemy / Human are not in the key
    // Another AI player's perception system has its own node.
    CHECK(res.enemy_force(2, 7, true, 0, v) && res.cache_size() == n2 + 1);
    // Staleness: a relation change is not seen until the entry's 15 s run out.
    sim.set_relation(0, 1, gal::Relation::Neutral);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, 0) == 600.0);  // live: the leaf is empty, the own base counts
    CHECK(value("Space_All") == 1000.0 && galactic_ai_enemy_strength(sim, ti, 7, 0, true, 0, nullptr, &res) == 1600.0);
    res.refresh(f0 + 450);
    CHECK(value("Space_All") == 1000.0);  // NOW = stamp + 450: still a hit
    res.refresh(f0 + 451);
    CHECK(value("Space_All") == 0.0);  // expired: recomputed and re-stamped at f0 + 451
    sim.reset_relations();
    CHECK(value("Space_All") == 0.0);  // the fresh entry hides the change again
    // Signal 0xE: a ground-only mask recomputes the ground nodes only; the space mask recomputes the space nodes.
    CHECK(res.visible_flags(7, 0) == 0u);
    res.visibility_event(7, 0, 0x80u);
    CHECK(res.visible_flags(7, 0) == 0x80u && value("Space_All") == 0.0);
    res.visibility_event(7, 0, 0x20u);  // space covered (recompute), ground just lost coverage (recompute)
    CHECK(res.visible_flags(7, 0) == 0x20u && value("Space_All") == 1000.0 && value("Space_Frigate") == 1000.0);
    sim.set_relation(0, 1, gal::Relation::Neutral);
    res.visibility_event(7, 0, 0u);  // lost coverage: recompute once more, visible := 0
    CHECK(res.visible_flags(7, 0) == 0u && value("Space_All") == 0.0);
    sim.reset_relations();
    res.visibility_event(7, 0, 0u);  // not visible, not covered: nothing
    CHECK(value("Space_All") == 0.0);
    res.visibility_event(7, 1, 0xFFFFFFFFu);  // another player's event leaves player 0's nodes alone
    CHECK(value("Space_All") == 0.0 && res.visible_flags(7, 1) == 0x2B8u);
    // GroundbaseTotalUnnormalized (§9.12): every ground structure's power + the persisted records with a resolvable typed
    // record, live (lifetime 0); the side filter failing (its own planet) is 0.0, a success.
    CHECK(value("Groundbase") == 300.0);
    if (std::vector<gal::PersistedBuiltObject>* recs = sim.built_objects(7)) {
        recs->push_back({tFrigate, 10.0f, 20.0f, 0.1f, 0, 1});
        recs->push_back({tCorvette, 30.0f, 20.0f, 0.1f, 1, 0});
        recs->push_back({gal::kNoIndex, 50.0f, 20.0f, 0.1f, 0, 1});  // no type: skipped
        recs->push_back({tPlanet, 50.0f, 20.0f, 0.1f, 0, 1});        // no category mask: skipped
    }
    CHECK(value("Groundbase") == 1800.0);
    c.target = 0;
    CHECK(value("Groundbase") == 0.0);
    c.target = 7;
    // Save / load keeps the entries, their stamps and the visible flags.
    ByteWriter w;
    res.write(w);
    GalacticSimTokenResolver copy(sim, ti, nullptr, {});
    ByteReader rd(w.bytes());
    CHECK(copy.read(rd) && rd.ok() && copy.cache_size() == res.cache_size() && copy.visible_flags(7, 1) == 0x2B8u);
    ByteWriter w2;
    copy.write(w2);
    CHECK(w2.bytes() == w.bytes());
    const EvalResult rc = evaluate(eqs, eqs.find("Space_All"), c, copy);
    CHECK(rc.ok && rc.value == 0.0);  // the stale cached value survived the round trip
    std::printf("leaf cache: %zu entries, shared by equations / 0x5f2430 / Enemy-Human bindings; 15 s staleness; signal 0xE "
                "recompute by mask; Groundbase 1800 live\n", res.cache_size());
}

// A2 t03: per-tech members of the other objects the force totals walk (AI_GALACTIC_TOKENS 2 / 3): every unit of an orbiting
// fleet adds its type's members to the space leaf ("each unit's type power (category) + its per-tech members"); every
// ground structure adds its members (never its own power) to the ground leaf; surface units add only their power; a
// ground structure with Is_Special_Weapon_In_Space adds its own power to the space leaf by the structure mask (Capital
// default); the player-root totals add the members of every walked object.
void test_force_members() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    syn_data(files, defs, eqs, plan_files);
    eqs.add("Space_All", "Variable_Target.EnemyForce.SpaceTotalUnnormalized");
    eqs.add("Space_Capital", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Capital\"}");
    eqs.add("Space_Frigate", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Frigate\"}");
    eqs.add("Space_Corvette", "Variable_Target.EnemyForce.SpaceTotalUnnormalized {Parameter_Category = \"Corvette\"}");
    eqs.add("Ground_All", "Variable_Target.EnemyForce.GroundTotalUnnormalized");
    eqs.add("Ground_Infantry", "Variable_Target.EnemyForce.GroundTotalUnnormalized {Parameter_Category = \"Infantry\"}");
    eqs.add("Game_Space", "Game.EnemyForce.SpaceTotalUnnormalized");
    eqs.add("Self_Space", "Variable_Self.SpaceTotalUnnormalized");
    eqs.add("Self_Ground", "Variable_Self.GroundTotalUnnormalized");
    eqs.link();
    gal::GalacticRules rules = syn_rules();  // SB1 600, Frigate 1000, Corvette 500, Infantry 200
    rules.types[tBarracks].ai_combat_power = 300.0f;
    rules.types[tSB2].kind = gal::UnitKind::GroundStructure;  // SB2 stands in for an ion cannon (ground, +0x1E), power 1200
    rules.types[tSB2].base_level = 0;
    rules.types[tSB2].required_star_base = 0;
    // a2i t05: an orbital structure (DUMMY_ORBITAL_STRUCTURE, a sensor pod), power 50, kept in `structures` by the sim.
    const uint32_t tPod = static_cast<uint32_t>(rules.types.size());
    {
        gal::TypeInfo pod;
        pod.name = "SensorPod";
        pod.kind = gal::UnitKind::GroundStructure;
        pod.dummy = gal::kDummyOrbitalStructure;
        pod.ai_combat_power = 50.0f;
        rules.types.push_back(pod);
    }
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(4242u, 77u);
    gal::GalacticSim sim(rules, store, rng.synced());
    gal::GalacticSetup setup = syn_setup();
    // The Rebel holds P7: a level-1 base, a barracks, an "ion cannon", a frigate fleet and an infantry company.
    setup.forces = {{0, 0, tSB1}, {1, 7, tSB1}, {1, 7, tBarracks}, {1, 7, tSB2}, {1, 7, tPod}, {1, 7, tFrigate}, {1, 7, tInfantry}};
    CHECK(sim.start(setup));
    std::vector<GalacticAiTypeInfo> tinfo(rules.types.size());
    tinfo[tFrigate].categories = {"FRIGATE"};
    tinfo[tCorvette].categories = {"CORVETTE"};
    tinfo[tInfantry].categories = {"INFANTRY"};
    tinfo[tBarracks].categories = tinfo[tSB2].categories = {"STRUCTURE"};
    tinfo[tSB1].categories = {"CAPITAL"};
    tinfo[tSB1].spawn_tech[0] = {{tCorvette, 2}};   // starbase hangar: 1000
    tinfo[tFrigate].spawn_tech[0] = {{tCorvette, 1}};  // carrier hangar: 500
    tinfo[tBarracks].spawn_tech[0] = {{tInfantry, 3}};  // land garrison: 600
    tinfo[tInfantry].spawn_tech[0] = {{tInfantry, 5}};  // a surface unit's list is never read by the planet leaf
    tinfo[tSB2].special_weapon_space = true;
    tinfo[tPod].categories = {"STRUCTURE"};
    tinfo[tPod].spawn_tech[0] = {{tInfantry, 1}};  // an orbital structure's list: never read by the ground leaf (200)
    fill_category_masks(tinfo);
    const uint64_t cap = ai_category_bit("Capital"), frig = ai_category_bit("Frigate"), corv = ai_category_bit("Corvette"),
                   inf = ai_category_bit("Infantry"), structure = ai_category_bit("Structure");
    tinfo[tPod].garrison_mask = corv;  // its +0x1658 structure mask (Garrison_Category = Corvette)
    const std::span<const GalacticAiTypeInfo> ti(tinfo);
    const gal::PlanetState& p7 = sim.planets()[7];
    CHECK(p7.owner == 1 && p7.starbase && p7.structures.size() == 3 && p7.surface.size() == 1 && !p7.orbit.empty());
    // Space leaf: frigate 1000 + its corvette 500 + the base's corvettes 1000 + the special-weapon structure's own 1200
    // (its +0x1658 is 0 -> Capital) + the orbital pod's own 50 (no +0x1E test, its mask Corvette); the starbase's own 600
    // never (A2I_TOKENS_RE §2.1).
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, true, kAllCategories) == 3750.0);
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, true, frig) == 1000.0);
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, true, corv) == 1550.0);
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, true, cap) == 1200.0);
    {
        // The structure mask is the type's +0x1658 when set: SB2 with Garrison_Category = Frigate counts in the Frigate row
        // and leaves the Capital row.
        std::vector<GalacticAiTypeInfo> t2 = tinfo;
        t2[tSB2].garrison_mask = frig;
        CHECK(galactic_ai_structure_mask(t2, tSB2) == frig && galactic_ai_structure_mask(t2, tBarracks) == cap);
        CHECK(galactic_ai_planet_force(sim, t2, 7, 0, true, true, frig) == 2200.0);
        CHECK(galactic_ai_planet_force(sim, t2, 7, 0, true, true, cap) == 0.0);
        CHECK(galactic_ai_planet_force(sim, t2, 7, 0, true, true, kAllCategories) == 3750.0);
    }
    // Ground leaf: infantry 200 (not its own list) + barracks members 600; the structures' own power (300, 1200, 50) never,
    // and the orbital pod's members (200) are not walked (A2I_TOKENS_RE §2.2).
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, false, kAllCategories) == 800.0);
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, false, inf) == 800.0);
    // 0x5f2430: + the starbase's own 600 (space) / the ground structures' own 300 + 1200 (ground; the orbital pod is not
    // a ground structure). The untyped row adds every one; a typed row those whose CategoryMask meets the row (a2i t03 D1,
    // A2F_RE_READS §3.1): the Capital row gets the Capital starbase's 600, the Structure row the barracks and SB2.
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, kAllCategories) == 4350.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, corv) == 1550.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, cap) == 1800.0);  // 1200 (SB2 by its structure mask) + 600
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, frig) == 1000.0);  // the starbase is not a Frigate
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, kAllCategories) == 2300.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, inf) == 800.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, false, structure) == 1500.0);  // 0 leaf + barracks 300 + SB2 1200
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 1, true, cap) == 0.0);  // the Rebel's own planet: no enemy, no add
    // The token leaves on the live sim (Self = Empire).
    GalacticAiConfig cfg;
    cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    CHECK(world.load_plans(plan_files) && world.start());
    sim.update();
    world.update_player(0, 1, true, false);
    EvalContext c;
    c.self = 0;
    c.target = 7;
    auto value = [&](const char* name) {
        const EvalResult r = evaluate(eqs, eqs.find(name), c, world.resolver());
        CHECK(r.ok);
        return r.value;
    };
    CHECK(value("Space_All") == 3750.0);
    CHECK(value("Space_Capital") == 1200.0);
    CHECK(value("Space_Frigate") == 1000.0);
    CHECK(value("Space_Corvette") == 1550.0);
    CHECK(value("Ground_All") == 800.0);
    CHECK(value("Ground_Infantry") == 800.0);
    CHECK(value("Game_Space") == 3750.0);  // Σ planets: only P7 holds Rebel force
    // Player root (Self = Rebel): space = frigate 1000 + the special weapon SB2's 1200 + the orbital pod's 50 (0x66b1c0:
    // a type that is not a starship / squadron counts when +0x1E is set or it is DUMMY_ORBITAL_STRUCTURE and the category
    // meets its +0x1658 mask, A2I_TOKENS_RE §2.4); a galactic starbase is not a GOM object, so 0x66b1c0 never adds it
    // (A2H_T02_SPACE_TOTAL); ground = infantry 200. The members of every walked object use the EXACT per-tech list of
    // the owner's tech level (0x373fd0 / 0x373ee0, A2F_RE_READS §2.3): the Rebel is above tech 0 and every list here is
    // Tech_0, so no member is added.
    c.self = 1;
    CHECK(value("Self_Space") == 2250.0);
    CHECK(value("Self_Ground") == 200.0);
    std::printf("force members: space leaf 3750 (fleet + carrier members + base members + special weapon + orbital pod), "
                "ground leaf 800, 0x5f2430 untyped 4350 / 2300, typed Capital 1800, Structure 1500\n");
}

// a2m t03: a test plan that calls Wait_For_Members gets the units its PotentialPlan selected (take_selection_units)
// once it is waiting; the task force starts with no members (A2M_T03_PRODUCE_FORCE_CLAIMS Q1).
void take_when_ready(ParityWorld& pw, lua::GlobalValueStore& gv) {
    if (gv.get("take_ready").kind != lua::AiGlobalValue::Number || gv.get("take_done").kind == lua::AiGlobalValue::Number) return;
    CHECK(pw.world->take_selection_units(0, "MainForce"));
    lua::AiGlobalValue d;
    d.kind = lua::AiGlobalValue::Number;
    d.number = 1.0;
    gv.set("take_done", d);
}

// a1h t07: a build task of an AllowFreeStoreUnits plan takes its unit through 0x4d2bd0, which does not read the free-store
// entry's +0x30 owner (A1_READINGS §28.5): a unit another PotentialPlan reserved is still supplied.
void test_free_store_supply() {
    ParityWorld pw(false, {"HeroAttach"});
    pw.run(2);
    const objects::ObjectHandle f = pw.world->free_store_supply(0, tFrigate);  // the Empire's starting frigate at P0
    CHECK(static_cast<bool>(f) && pw.world->free_store_owner(f) == -1);
    CHECK(!pw.world->free_store_supply(0, tCorvette));  // none on the store
    CHECK(!pw.world->free_store_supply(1, tFrigate));   // not an AI player of this world
    pw.world->set_free_store_owner(f, 12345);           // reserved by another plan's accept (0x6c2b20)
    CHECK(pw.world->free_store_owner(f) == 12345 && pw.world->free_store_supply(0, tFrigate) == f);
    pw.world->set_free_store_owner(f, -1);
    CHECK(pw.world->free_store_owner(f) == -1);
    std::printf("free-store supply: a reserved unit is supplied (0x4d2bd0 ignores +0x30)\n");
}

// A2k t02: the production source planets of a planet target are the target's own reachability list (0x665160 with
// destination = the target, A2K_T02_RE_ANSWERS Q1 port change 3). At Conquer_Planet's Enemy_destination level P3 has a
// source list only when one of its neighbours is held, so the solver tests that produce for a P3 target give the Empire
// P2 too (an infantry company; P2 has no starbase, so the Corvette is still built at P0).
void own_p2(gal::GalacticSetup& s) { s.forces.push_back({0, 2, tInfantry}); }

// q6 t18 (AI_GALACTIC_TOKENS §9.10): the galactic escort flag 0x529 is 1 from Setup C on (the galactic escort list is
// always empty), so the Score gate (0x528 && B0) || (0x529 && B1) is open for an escort team even while the main contrast
// is unmet, and the first placement pass skips escort slots while the main contrast is unmet.
void test_escort_flag() {
    // The rule helpers.
    CHECK(galactic_escort_met(false) && !galactic_escort_met(true));
    CHECK(!galactic_place_skip(false, true, false) && galactic_place_skip(false, true, true));   // main unmet
    CHECK(!galactic_place_skip(true, true, false) && !galactic_place_skip(true, true, true));    // both met
    CHECK(galactic_place_skip(true, false, false) && !galactic_place_skip(true, false, true));   // IgnoreTarget-free tactical
    CHECK(galactic_contrast_gate(false, true, false, true) && !galactic_contrast_gate(false, true, true, false));
    // Synthetic plans at a target the Rebels hold with two frigates (the main contrast is unmet by one corvette).
    ParityWorld pw(false, {"EscortOnly", "MainEscort", "MaskCorvette"}, [](gal::GalacticSetup& s) {
        s.forces.push_back({1, 3, tFrigate});
        s.forces.push_back({1, 3, tFrigate});
        s.forces.push_back({0, 0, tCorvette});
        own_p2(s);  // A2k t02: a held neighbour gives P3 its production source list
    });
    pw.run(2);
    bool ok_main = false, ok_escort = false, ok_both = false;
    const std::vector<uint32_t> m = pw.world->solve_potential_plan(0, "MaskCorvette", "Conquer_Planet", 3, &ok_main);
    const std::vector<uint32_t> e = pw.world->solve_potential_plan(0, "EscortOnly", "Conquer_Planet", 3, &ok_escort);
    const std::vector<uint32_t> b = pw.world->solve_potential_plan(0, "MainEscort", "Conquer_Planet", 3, &ok_both);
    std::printf("escort flag: MainForce corvette plan %s (%zu), EscortForce-only plan %s (%zu, corvette %s), Main+Escort %s (%zu)\n",
                ok_main ? "solved" : "failed", m.size(), ok_escort ? "solved" : "failed", e.size(),
                pw.has(e, tCorvette) ? "yes" : "no", ok_both ? "solved" : "failed", b.size());
    // The escort-only candidate: v <= 0 (no main list score without a non-escort team), the gate is open through 0x529 and
    // its team is below its minimum, so it is still allowed: the corvette is chosen whatever the main contrast says.
    CHECK(ok_escort && pw.has(e, tCorvette));
    // With both kinds of team the candidate enters the "still allowed" checks while the main contrast is unmet (the old
    // escort_met = main_met rule vetoed both plans).
    CHECK(ok_main && ok_both && b.size() == 2 && pw.has(b, tCorvette));
}

void test_parity_rules() {
    {
        ParityWorld pw(false, {"HeroAttach", "HeroDeny"}, own_p2);
        pw.run(2);  // first service: templates, budget
        bool ok = false;
        // §2.4: the hero record is vetoed in a plan that allows hero attach; the corvette is chosen instead.
        const std::vector<uint32_t> a = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        std::printf("parity: hero-attach plan -> %zu selected (hero %s)\n", a.size(), pw.has(a, pw.hero) ? "yes" : "no");
        CHECK(ok && !pw.has(a, pw.hero) && pw.has(a, tCorvette));
        // Every TF DenyHeroAttach: the hero is a plain free-store unit and wins (FreeStore 2+).
        const std::vector<uint32_t> d = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(d, pw.hero));
        // §6.3: P2's planet type restricts Corvettes -> TerrainType vetoes it; the hero is vetoed too -> the solve fails.
        const std::vector<uint32_t> r = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 2, &ok);
        CHECK(!ok && r.empty());
        // §7.1: x / 0 fails, 0 / 0 = 0, clamp with lo > hi fails.
        EquationSet e;
        e.add("ZeroOverZero", "0 / 0");
        e.add("OneOverZero", "1 / 0");
        e.add("ClampBad", "clamp(5, 3, 1)");
        e.add("ClampOk", "clamp(5, 1, 3)");
        e.link();
        EvalContext c;
        c.self = 0;
        const EvalResult z = evaluate(e, e.find("ZeroOverZero"), c, pw.world->resolver());
        CHECK(z.ok && z.value == 0.0);
        CHECK(!evaluate(e, e.find("OneOverZero"), c, pw.world->resolver()).ok);
        CHECK(!evaluate(e, e.find("ClampBad"), c, pw.world->resolver()).ok);
        const EvalResult k = evaluate(e, e.find("ClampOk"), c, pw.world->resolver());
        CHECK(k.ok && k.value == 3.0);
        // STRATEGIC_GAPS §1.1: per-type survival ratio (1.0 unseen), fed by the auto-resolve roll and object removal
        // (UNIT_AI types only, AI owners only), keyed by the type-name CRC with the company-leader substitution.
        CHECK(pw.world->survival_ratio(0, tCorvette) == 1.0);
        pw.world->record_auto_resolve_survival(0, tCorvette, false);
        pw.world->record_auto_resolve_survival(0, tCorvette, true);
        CHECK(pw.world->survival_ratio(0, tCorvette) == 0.5);
        pw.world->record_object_removal(0, tCorvette, tCorvette, false);
        pw.world->record_object_removal(0, tFrigate, tFrigate, false);  // no UNIT_AI: skipped
        pw.world->record_object_removal(1, tCorvette, tCorvette, true);  // player 1 has no AI: skipped
        CHECK(std::fabs(pw.world->survival_ratio(0, tCorvette) - 1.0 / 3.0) < 1e-12 && pw.world->survival_ratio(0, tFrigate) == 1.0);
        CHECK(pw.world->survival_ratio(0, tInfantry) == pw.world->survival_ratio(0, tCorvette));  // company leader key
        CHECK(pw.world->survival_ratio(1, tCorvette) == 1.0);
        const std::vector<uint32_t> s2 = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(s2, tCorvette));  // the ratio is a 0..1 score term, not a veto
        LearningSystem ls;
        ls.record_survival(7u, true);
        ls.record_survival(7u, false);
        ls.record_survival(7u, false);
        CHECK(ls.survival_ratio(7u) * 3.0 == 1.0 && ls.survival_ratio(8u) == 1.0 && ls.survival_entries() == 1);
    }
    {
        // a2m t03 (A2M_R2_RE_B §3.1, §3.2): the record needs the unit's LIVE object script. A script that exits at its
        // first galactic pump (SpaceOnlyScript, as Home_One's) or does not load leaves a plain unit, which the hero-attach
        // plan takes; H's Execute_Script_On_Type instance still makes a record (vetoed).
        struct Case {
            const char* script;
            bool on_type;
            bool hero_selected;
        };
        for (const Case& c : {Case{"HeroShipScript", false, false}, Case{"SpaceOnlyScript", false, true},
                              Case{"NoSuchScript", false, true}, Case{"SpaceOnlyScript", true, false}}) {
            ParityWorld pw(false, {"HeroAttach"}, own_p2, {}, {}, [&c](std::vector<GalacticAiTypeInfo>& ti) {
                for (GalacticAiTypeInfo& x : ti)
                    if (x.hero) {
                        x.lua_script = c.script;
                        x.script_on_type = c.on_type;
                    }
            });
            pw.run(2);
            bool ok = false;
            const std::vector<uint32_t> a = pw.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok);
            std::printf("parity: hero script %s%s -> hero %s\n", c.script, c.on_type ? " (on type)" : "",
                        pw.has(a, pw.hero) ? "selected" : "vetoed");
            CHECK(ok && pw.has(a, pw.hero) == c.hero_selected);
            if (std::string(c.script) == "SpaceOnlyScript" && !c.on_type) {
                bool logged = false;
                for (const auto& [k, n] : pw.world->proposal_stats())
                    logged = logged || k.find("object script SpaceOnlyScript exits in galactic") != std::string::npos;
                CHECK(logged);
            }
        }
    }
    {
        // §3.5: with AI_Uses_Galactic_Pop_Cap a produced Corvette (pop 50) exceeds the remaining population.
        ParityWorld off(false, {"HeroAttach"}, own_p2), on(true, {"HeroAttach"}, own_p2);
        off.run(2);
        on.run(2);
        bool ok_off = false, ok_on = false;
        off.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok_off);
        on.world->solve_potential_plan(0, "HeroAttach", "Conquer_Planet", 3, &ok_on);
        std::printf("parity: pop cap off -> %s, on -> %s\n", ok_off ? "solved" : "failed", ok_on ? "solved" : "failed");
        CHECK(ok_off && !ok_on);
    }
    {
        // §4: a MagicPlan spawns its unit at the destination (no Production event, no budget charge).
        ParityWorld pw(false, {"MagicSpace"});
        pw.run(30 * 60);
        bool spawned = false, failed = false;
        for (const AiDecision& d : pw.world->decision_log()) {
            spawned = spawned || d.text.find("magic spawn Corvette") != std::string::npos;
            failed = failed || d.text.find("failed") != std::string::npos;
        }
        const bool produced = std::find(pw.orders.begin(), pw.orders.end(), std::to_string(static_cast<int>(commands::EventType::Production))) !=
                              pw.orders.end();
        std::printf("parity: magic plan spawned %s, production orders %s\n", spawned ? "yes" : "no", produced ? "yes" : "no");
        CHECK(spawned && !failed && !produced);
    }
}

// a2m t09 (A2M_T08_T10_T14_RE t08) [H]: the AI clock leaves use the float spf = 1.0f / 30.0f, not frame / 30.
void test_ai_clock_precision() {
    CHECK(kAiSecondsPerFrame == 0.033333335f);
    // Game.Age = (double)frame x (double)spf: Age > 120 first holds at frame 3600 (frame / 30 gave exactly 120.0 there).
    CHECK(galactic_ai_game_age(0) == 0.0);
    CHECK(galactic_ai_game_age(3599) < 120.0 && galactic_ai_game_age(3600) > 120.0);
    CHECK(galactic_ai_game_age(3600) == 3600.0 * static_cast<double>(0.033333335f));
    CHECK(galactic_ai_game_age(30) > 1.0 && galactic_ai_game_age(29) < 1.0);
    // TimeSinceCorruptionChange = (double)(float)((float)(uint32)(frame - stamp) x spf): rounded to float, so at the
    // whole seconds it lands on 1.0 / 2.0 exactly while Game.Age is a little above (the saboteur desire "on" frames).
    CHECK(galactic_ai_time_since_corruption_change(30, 0) == 1.0 && galactic_ai_time_since_corruption_change(60, 0) == 2.0);
    CHECK(galactic_ai_game_age(30) > galactic_ai_time_since_corruption_change(30, 0));
    CHECK(galactic_ai_game_age(60) > galactic_ai_time_since_corruption_change(60, 0));
    CHECK(galactic_ai_time_since_corruption_change(3599, 0) < 120.0 && galactic_ai_time_since_corruption_change(3600, 0) > 120.0);
    CHECK(galactic_ai_time_since_corruption_change(3600, 0) == static_cast<double>(120.00000762939453f));
    CHECK(galactic_ai_time_since_corruption_change(130, 100) == static_cast<double>(30.0f * 0.033333335f));
    // TimeSinceConversion / Ground / SpaceConflict: (double)(int32)(NOW - stamp) x (double)spf, never = (double)1e18f.
    CHECK(galactic_ai_time_since_stamp(100, -1) == 999999984306749440.0);
    CHECK(galactic_ai_time_since_stamp(3700, 100) == galactic_ai_game_age(3600));
    CHECK(galactic_ai_time_since_stamp(3699, 100) < 120.0 && galactic_ai_time_since_stamp(3700, 100) > 120.0);
    std::printf("ai clock: age(3600) %.13f, since corruption(3600) %.13f, never %.1f\n", galactic_ai_game_age(3600),
                galactic_ai_time_since_corruption_change(3600, 0), galactic_ai_time_since_stamp(0, -1));
}

// A2k t02: the per-target stage (A2K_T02_RE_ANSWERS Q1) and FreeStore Setup A (Q2(c)) on the synthetic chain P0..P7
// (the Empire holds P0 / P1, its hero and frigate at P0; Conquer_Planet = Enemy_destination, level 2).
void test_stage_and_setup_a() {
    CHECK(galactic_ai_hull_fraction(50.0f, 100.0f) == 0.5 && galactic_ai_hull_fraction(0.0f, 100.0f) == 1.0 &&
          galactic_ai_hull_fraction(30.0f, 0.0f) == 1.0);
    bool ok = false;
    {
        ParityWorld pw(false, {"HeroDeny"});
        pw.run(2);
        // P3's neighbours P2 / P4 are not held: no level-2 stage (Setup A then stages at the target). At level 5 the BFS
        // from P3's first neighbour P2 pops P1 before P0.
        CHECK(pw.world->stage_area(0, 3, 2) == kInvalidPlanet && pw.world->stage_list(0, 3, 2).empty());
        CHECK(pw.world->stage_list(0, 3, 5) == (std::vector<PlanetIndex>{1, 0}) && pw.world->stage_area(0, 3, 5) == 1);
        CHECK(pw.world->stage_area(0, 3, 4) == 3 && pw.world->stage_area(0, 3, 6) == kInvalidPlanet);
        CHECK(pw.world->stage_area(0, 3, 0) == kInvalidPlanet && pw.world->stage_area(0, 1, 0) == 1);  // held target: itself
        CHECK(pw.world->stage_area(1, 3, 5) == kInvalidPlanet);  // not an AI player of this world
        // a2l t03 (A2L_T03_RE_ANSWERS Q2): goal-system slot 9 = the target's slot 6 at the goal level is null.
        // Conquer_Planet (level 2) at P3: no held planet behind P2 / P4 -> blocked; Build_Space_Forces (Any) at P3: P1 ->
        // not blocked; Upgrade_Starbase (Friendly_Only) at the held P1: itself -> not blocked, at P3: not held -> blocked.
        CHECK(pw.world->goal_blocked(0, "Conquer_Planet", 3) && !pw.world->goal_blocked(0, "Build_Space_Forces", 3));
        CHECK(!pw.world->goal_blocked(0, "Upgrade_Starbase", 1) && pw.world->goal_blocked(0, "Upgrade_Starbase", 3));
        CHECK(!pw.world->goal_blocked(0, "Conquer_Planet", 2));  // P2's neighbour P1 is held
        CHECK(!pw.world->goal_blocked(0, "Conquer_Planet", -1) && !pw.world->goal_blocked(0, "No_Such_Goal", 3));
        for (PlanetIndex t = 0; t < 8; ++t) {  // slot 9 is exactly "slot 6 is null"
            CHECK(pw.world->goal_blocked(0, "Conquer_Planet", static_cast<int32_t>(t)) == (pw.world->stage_area(0, t, 2) == kInvalidPlanet));
            CHECK(pw.world->goal_blocked(0, "Upgrade_Starbase", static_cast<int32_t>(t)) == (pw.world->stage_area(0, t, 0) == kInvalidPlanet));
        }
        const std::vector<uint32_t> s = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(s, pw.hero));  // the hero's path P0 -> P3 has no node cost: eligible
    }
    {
        // The Empire also holds P2: P3's first neighbour is held, so the level-2 stage is P2 (BFS P2, P1, P0).
        ParityWorld pw(false, {"HeroDeny"}, [](gal::GalacticSetup& s) { s.forces.push_back({0, 2, tInfantry}); });
        pw.run(2);
        CHECK(pw.world->stage_list(0, 3, 2) == (std::vector<PlanetIndex>{2, 1, 0}) && pw.world->stage_area(0, 3, 2) == 2);
        CHECK(!pw.world->goal_blocked(0, "Conquer_Planet", 3));  // a2l t03: P3's neighbour P2 is held now
        const std::vector<uint32_t> s = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok);
        CHECK(ok && pw.has(s, pw.hero));
    }
    {
        // A Rebel frigate orbits P2: the hero's only path to the stage (P3) crosses a planet with node cost > 0, so Setup A
        // drops it (galactic: no finite stage cost) and the solve does not use it.
        ParityWorld pw(false, {"HeroDeny"}, [](gal::GalacticSetup& s) { s.forces.push_back({1, 2, tFrigate}); });
        pw.run(2);
        const std::vector<uint32_t> s = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok);
        std::printf("setup A: enemy frigate on the hero's path -> %s, hero %s\n", ok ? "solved" : "failed", pw.has(s, pw.hero) ? "yes" : "no");
        CHECK(!pw.has(s, pw.hero));
    }
    {
        // a2l t03 (A2L_T03_RE_ANSWERS Q4.1): a unit below GameConstants Health_Low_Percent_Threshold is dropped. The
        // synthetic hero is at full hull (1.0): a floor of 0.33 keeps it, a floor above 1.0 drops it.
        ParityWorld keep(false, {"HeroDeny"});
        CHECK(keep.rules.constants.health_low_percent_threshold == 0.33f);
        ParityWorld drop(false, {"HeroDeny"}, {}, [](gal::GalacticRules& r) { r.constants.health_low_percent_threshold = 1.5f; });
        keep.run(2);
        drop.run(2);
        CHECK(keep.has(keep.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok), keep.hero));
        CHECK(!drop.has(drop.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok), drop.hero));
    }
    {
        // a2l t03 (Q4.3): a unit in hyperspace stays a candidate; a2m t05 (A2M_T04_RE_ANSWERS Q1.3): keyed by its fleet's
        // FleetDataPack +0x50, the move's final destination.
        ParityWorld pw(false, {"HeroDeny"});
        pw.run(2);
        objects::ObjectHandle h;
        for (const gal::FleetState& f : pw.sim->fleets())
            if (f.alive && f.owner == 0)
                for (const objects::ObjectHandle u : f.contents)
                    if (pw.store->alive(u) && pw.store->type(u) == pw.hero) h = u;
        CHECK(static_cast<bool>(h) && pw.world->free_store_move(0, h, 1));
        bool moving = false;
        for (int32_t fr = 3; fr < 600 && !moving; ++fr) {
            pw.sim->update();
            pw.world->update_player(0, fr, true, false);
            for (const gal::FleetState& f : pw.sim->fleets())
                if (f.alive && f.moving && std::find(f.contents.begin(), f.contents.end(), h) != f.contents.end()) moving = true;
        }
        CHECK(moving);
        const std::vector<uint32_t> s = pw.world->solve_potential_plan(0, "HeroDeny", "Conquer_Planet", 3, &ok);
        std::printf("setup A: hero in hyperspace -> %s, hero %s\n", ok ? "solved" : "failed", pw.has(s, pw.hero) ? "yes" : "no");
        CHECK(ok && pw.has(s, pw.hero));
    }
}

// a2l t03 (A2L_T03_RE_ANSWERS Q5): the stage of a goal without a target is null (Get_Stage nil), and Produce_Force()
// without an argument has no destination even when the TF has a stage, so nothing gathers.
void test_null_stage_and_plain_produce() {
    ParityWorld pw(false, {"A2lNoTarget", "A2lPlainProduce"}, own_p2);  // P2 held: Conquer_Planet @P3 stages at P2
    lua::GlobalValueStore gv;
    pw.world->set_global_values(&gv);
    auto num = [&gv](const char* n) { const lua::AiGlobalValue v = gv.get(n); return v.kind == lua::AiGlobalValue::Number ? v.number : -1.0; };
    auto hero_planet = [&pw]() -> int64_t {
        for (const gal::FleetState& f : pw.sim->fleets())
            if (f.alive)
                for (const objects::ObjectHandle u : f.contents)
                    if (pw.store->alive(u) && pw.store->type(u) == pw.hero) return f.moving ? -2 : static_cast<int64_t>(f.planet);
        return -1;
    };
    int32_t frames = 0;
    bool hero_moved = false;
    for (int32_t f = 1; f <= 30 * 300 && (num("a2l_nt_done") < 0.0 || num("a2l_t_done") < 0.0 || frames < 30 * 20); ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        if (num("a2l_t_done") >= 0.0) ++frames;  // keep running a while after the plain Produce_Force finished
        hero_moved = hero_moved || hero_planet() != 0;
    }
    std::printf("null stage: no-target stage %.0f done %.0f; targeted stage %.0f done %.0f; hero moved %s\n", num("a2l_nt_stage"),
                num("a2l_nt_done"), num("a2l_t_stage"), num("a2l_t_done"), hero_moved ? "yes" : "no");
    CHECK(num("a2l_nt_stage") == 0.0 && num("a2l_nt_done") == 1.0);  // nil stage; Produce_Force() finishes in place
    CHECK(num("a2l_t_stage") == 1.0 && num("a2l_t_done") == 1.0);    // a targeted TF has a stage ...
    CHECK(!hero_moved);                                                // ... but plain Produce_Force() does not gather
    pw.world->set_global_values(nullptr);
}

// a2m t02 (A2L_T01_RE_ANSWERS Q8.1 / Q8.2) [H]: Move_To moves the task force's FleetObject only (the parent fleet of
// the first member added, TF +0x120); a member in another fleet stays where it is and is still counted.
void test_move_to_fleet_object() {
    // The Empire's frigate starts at P1 instead of P0: the hero (P0) and the frigate (P1) are in two fleets.
    ParityWorld pw(false, {"A2mMoveTo"}, [](gal::GalacticSetup& s) {
        for (gal::StartingForce& f : s.forces)
            if (f.player == 0 && f.type == tFrigate) f.planet = 1;
    });
    lua::GlobalValueStore gv;
    pw.world->set_global_values(&gv);
    auto num = [&gv](const char* n) { const lua::AiGlobalValue v = gv.get(n); return v.kind == lua::AiGlobalValue::Number ? v.number : -1.0; };
    auto where = [&pw](uint32_t type) -> int64_t {  // the planet of the Empire's unit of this type; -2 in transit
        for (const gal::FleetState& f : pw.sim->fleets())
            if (f.alive && f.owner == 0)
                for (const objects::ObjectHandle u : f.contents)
                    if (pw.store->alive(u) && pw.store->type(u) == type) return f.moving ? -2 : static_cast<int64_t>(f.planet);
        return -1;
    };
    CHECK(where(pw.hero) == 0 && where(tFrigate) == 1);
    bool hero_left = false, frigate_left = false;
    for (int32_t f = 1; f <= 30 * 300 && num("a2m_done") < 0.0; ++f) {
        pw.sim->update();
        pw.world->update_player(0, f, true, false);
        take_when_ready(pw, gv);
        hero_left = hero_left || where(pw.hero) != 0;
        frigate_left = frigate_left || where(tFrigate) != 1;
    }
    std::printf("move_to fleet object: block %.0f, members %.0f -> %.0f, hero moved %s, frigate moved %s\n", num("a2m_block"),
                num("a2m_count0"), num("a2m_count1"), hero_left ? "yes" : "no", frigate_left ? "yes" : "no");
    CHECK(num("a2m_done") == 1.0 && num("a2m_block") == 1.0);
    CHECK(num("a2m_count0") == 2.0 && num("a2m_count1") == 2.0);  // the member left behind is still in the task force
    CHECK(hero_left != frigate_left);                              // exactly one fleet moved
    pw.world->set_global_values(nullptr);
}

// a2m t02 (A2L_T01_RE_ANSWERS Q8.2) [H; M on 0x1b]: the FleetObject (TF +0x120) is re-elected when it is destroyed
// (signal 0x10) and, the port's [M] reading of the members' 0x1b, when it no longer holds any task-force member. The
// hero (P0) and the frigate (P1) start in two fleets and the test destroys the member that is in the FleetObject. In
// the "emptied" case a Corvette that is not a member shares each starting fleet, so the FleetObject lives on without a
// member. Either way the new FleetObject is the other member's fleet, and Move_To moves that fleet and nothing else.
void test_fleet_object_reelection() {
    for (const bool emptied : {false, true}) {
        ParityWorld pw(false, {"A2mReelect"}, [emptied](gal::GalacticSetup& s) {
            for (gal::StartingForce& f : s.forces)
                if (f.player == 0 && f.type == tFrigate) f.planet = 1;
            if (emptied) {
                s.forces.push_back({0, 0, tCorvette});
                s.forces.push_back({0, 1, tCorvette});
            }
        });
        lua::GlobalValueStore gv;
        pw.world->set_global_values(&gv);
        auto num = [&gv](const char* n) { const lua::AiGlobalValue v = gv.get(n); return v.kind == lua::AiGlobalValue::Number ? v.number : -1.0; };
        int32_t frame = 1;
        auto step = [&pw, &frame, &gv]() {
            pw.sim->update();
            pw.world->update_player(0, frame, true, false);
            take_when_ready(pw, gv);
            ++frame;
        };
        while (frame <= 30 * 120 && num("a2m_r_ready") < 0.0) step();
        CHECK(num("a2m_r_ready") == 1.0);
        const uint32_t fo = pw.world->task_force_fleet_object(0, "MainForce");
        CHECK(fo != gal::kNoFleet && fo < pw.sim->fleets().size());
        if (fo == gal::kNoFleet || fo >= pw.sim->fleets().size()) continue;
        // The member in the FleetObject (destroyed) and the member in the other fleet (kept); the non-member Corvette that
        // shares the FleetObject (one shares the kept member's fleet too and moves with it: Move_To moves the whole fleet).
        objects::ObjectHandle gone, kept;
        std::vector<std::pair<objects::ObjectHandle, int64_t>> corvettes;  // unit, start planet
        for (uint32_t i = 0; i < pw.sim->fleets().size(); ++i) {
            const gal::FleetState& fs = pw.sim->fleets()[i];
            if (!fs.alive || fs.owner != 0) continue;
            for (const objects::ObjectHandle u : fs.contents) {
                if (!pw.store->alive(u)) continue;
                const uint32_t t = pw.store->type(u);
                if (t == pw.hero || t == tFrigate) (i == fo ? gone : kept) = u;
                if (t == tCorvette && i == fo) corvettes.emplace_back(u, static_cast<int64_t>(fs.planet));
            }
        }
        CHECK(static_cast<bool>(gone) && static_cast<bool>(kept));
        if (!gone || !kept) continue;
        const uint32_t kept_fleet = pw.sim->fleet_containing(kept);
        const int64_t kept_from = static_cast<int64_t>(pw.sim->fleets()[kept_fleet].planet);
        CHECK(kept_fleet != fo);
        CHECK(pw.sim->fleets()[fo].contents.size() == (emptied ? 2u : 1u));
        CHECK(corvettes.size() == (emptied ? 1u : 0u));
        CHECK(pw.sim->destroy_object(gone));
        const bool fo_lives = pw.sim->fleets()[fo].alive && !pw.sim->fleets()[fo].contents.empty();
        CHECK(fo_lives == emptied);  // destroyed (0x10) / alive without a member (kept: 0x1b never re-elects)
        lua::AiGlobalValue go;
        go.kind = lua::AiGlobalValue::Number;
        go.number = 1.0;
        gv.set("a2m_r_go", go);
        bool kept_left = false, corvette_moved = false;
        const int32_t end = frame + 30 * 300;
        while (frame <= end && num("a2m_r_done") < 0.0) {
            step();
            const uint32_t kf = pw.sim->fleet_containing(kept);
            kept_left = kept_left || kf == gal::kNoFleet || pw.sim->fleets()[kf].moving ||
                        static_cast<int64_t>(pw.sim->fleets()[kf].planet) != kept_from;
            for (const auto& [c, from] : corvettes) {
                const uint32_t cf = pw.sim->fleet_containing(c);
                corvette_moved = corvette_moved || cf == gal::kNoFleet || pw.sim->fleets()[cf].moving ||
                                 static_cast<int64_t>(pw.sim->fleets()[cf].planet) != from;
            }
        }
        const uint32_t after = pw.world->task_force_fleet_object(0, "MainForce");
        std::printf("fleet object re-election (%s): FleetObject %u -> %u (kept member's fleet %u), block %.0f, members %.0f, "
                    "kept member moved %s, the old FleetObject's corvette moved %s\n",
                    emptied ? "emptied, 0x1b" : "destroyed, 0x10", fo, after, pw.sim->fleet_containing(kept), num("a2m_r_block"),
                    num("a2m_r_count"), kept_left ? "yes" : "no", corvette_moved ? "yes" : "no");
        CHECK(num("a2m_r_done") == 1.0 && num("a2m_r_block") == 1.0);  // Move_To found a FleetObject
        CHECK(num("a2m_r_count") == 1.0);
        // bkg2 b0099 (base handler 0x5fe5d0 step 1) [H]: one event 2 Unit_Destroyed with the removed member as argument.
        std::printf("fleet object re-election: Unit_Destroyed raised %.0f time(s), unit argument %s\n", num("a2m_r_destroyed"),
                    num("a2m_r_destroyed_arg") == 1.0 ? "yes" : "no");
        CHECK(num("a2m_r_destroyed") == 1.0);  // (the unit argument reads nil once the object is gone [port])
        if (emptied) {
            // bkg2 b0097 (BKG1_RE_ANSWERS 1.4) [H]: 0x1b never re-elects, so the FleetObject that lives on without a TF
            // member keeps its place: Move_To moves it (with its non-member corvette) and leaves the member's fleet.
            CHECK(!kept_left);
            CHECK(corvette_moved);
            CHECK(after == fo);
        } else {
            CHECK(kept_left);  // the fleet re-elected on 0x10 moved ...
            CHECK(!corvette_moved);
            CHECK(after == pw.sim->fleet_containing(kept) && after != fo);
        }
        pw.world->set_global_values(nullptr);
    }
}

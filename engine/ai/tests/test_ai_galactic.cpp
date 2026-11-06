// A2 tests: AI framework rules (cadence, quota, desire, plan pick, solver, templates, budget, commit) and the
// galactic AI on the Lua bridge: a synthetic AI-vs-AI campaign (no game data; per-frame CRCs identical for
// 1/2/4/15 threads, golden tests/golden/ai_galactic_synthetic_crc.txt) and, with ALAMO_FOC_DATA, the shipped
// FoC AI data + plan scripts.
//
//   alamo_ai_galactic_tests [--write-golden]
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "alamo/ai/ai_context.hpp"
#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_free_store_path.hpp"
#include "alamo/ai/ai_goals.hpp"
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/ai_plans.hpp"
#include "alamo/ai/galactic_ai.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/compress.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/players.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "../src/galactic_goal_time.hpp"
#include "../src/galactic_slots.hpp"
#ifdef ALAMO_AI_HAVE_VFS
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo;
using namespace alamo::ai;
namespace gal = alamo::galactic;

static int g_fail = 0, g_checks = 0;
#define CHECK(c)                                                                  \
    do {                                                                          \
        ++g_checks;                                                               \
        if (!(c)) {                                                               \
            ++g_fail;                                                             \
            std::printf("FAILED %s:%d: %s\n", __FILE__, __LINE__, #c);            \
        }                                                                         \
    } while (0)

static bool g_write_golden = false;

// ================================================================ framework unit tests

static void test_cadence() {
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
        void perception_service(int64_t n) override { trace.push_back("P" + std::to_string(n)); }
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
    CHECK(stub.trace.size() >= 3 && stub.trace[0] == "first" && stub.trace[1] == "tinit" && stub.trace[2] == "P1");
    CHECK(ctx.service_counts[0] == 2);    // perception: frames 1, 301
    CHECK(ctx.service_counts[1] == 600);  // goal: every frame
    CHECK(ctx.service_counts[2] == 200);  // planning: every 3 frames
    CHECK(ctx.service_counts[3] == 200);  // execution
    CHECK(ctx.service_counts[4] == 2);    // learning
    CHECK(ctx.service_counts[5] == 600);  // template
    // Order inside a frame: P before L before E on frame 1.
    const auto pos = [&](const std::string& s) { return std::find(stub.trace.begin(), stub.trace.end(), s) - stub.trace.begin(); };
    CHECK(pos("P1") < pos("L1") && pos("L1") < pos("E1"));
    // Disabled: nothing runs, except galactic Planning while auto-resolve is busy.
    const auto before = ctx.service_counts;
    ctx.service(601, false, false);
    CHECK(ctx.service_counts == before);
    ctx.service(602, false, true);
    CHECK(ctx.service_counts[2] == before[2] + 1 && ctx.service_counts[1] == before[1]);
    std::printf("cadence: %zu trace entries\n", stub.trace.size());
}

static void test_desire_and_hysteresis() {
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
static void test_goal_reservations() {
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
static void test_goal_batched_accept() {
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

static void test_goal_batched_values() {
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

static void test_plan_pick_and_solver() {
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

static const char* kSynGoals = R"(<Goals>
  <Build_Space_Forces><AIGoalApplicationFlags>Global</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Any</Reachability><Time_Limit>200</Time_Limit></Build_Space_Forces>
  <Conquer_Planet><AIGoalApplicationFlags>Enemy | Neutral</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Enemy_destination</Reachability><Time_Limit>240</Time_Limit>
    <Build_Time_Delay_Tolerance>1.3</Build_Time_Delay_Tolerance><Is_Like>Build_Space_Forces</Is_Like>
    <Tracking_Duration>300</Tracking_Duration><Per_Failure_Desire_Adjust>-1.0</Per_Failure_Desire_Adjust>
    <Activation_Tracking_Duration>60</Activation_Tracking_Duration>
    <Per_Activation_Failure_Desire_Adjust>-2.0</Per_Activation_Failure_Desire_Adjust></Conquer_Planet>
  <Upgrade_Starbase><AIGoalApplicationFlags>Friendly</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Infrastructure</Category><Reachability>Friendly_Only</Reachability><Time_Limit>60</Time_Limit></Upgrade_Starbase>
  <Land_Thing><AIGoalApplicationFlags>Enemy</AIGoalApplicationFlags><GameMode>Land</GameMode>
    <Category>Tactical_Targeted</Category></Land_Thing>
</Goals>)";
static const char* kSynSet = R"(<FunctionSet>
  <A><Goal>Build_Space_Forces</Goal><Function>Allow_Space</Function></A>
  <B><Goal>Conquer_Planet</Goal><Function>Conquer_Desire</Function></B>
  <C><Goal>Upgrade_Starbase</Goal><Function>Needs_Starbase_Upgrade</Function></C>
  <D><Goal>Land_Thing</Goal><Function>One</Function></D>
</FunctionSet>)";
static const char* kSynTemplates = R"(<AITemplates>
  <Syn_Default><Priority>1</Priority><Trigger>One</Trigger>
    <Budget><Offensive>Offensive_Budget</Offensive><Infrastructure>Infra_Budget</Infrastructure><Defensive>Zero</Defensive></Budget>
    <Turn_On><Goals><Category>Offensive</Category><Category>Infrastructure</Category></Goals>
      <Plans><Goal_Category>Offensive</Goal_Category><Goal_Category>Infrastructure</Goal_Category></Plans></Turn_On>
  </Syn_Default>
  <Syn_Rich><Priority>3</Priority><Trigger>Is_Rich</Trigger>
    <Turn_Off><Goals><Goal_Type>Upgrade_Starbase</Goal_Type></Goals></Turn_Off></Syn_Rich>
</AITemplates>)";
static const char* kSynPlayer = R"(<AIPlayerType><Name>%s</Name>
  <GalacticFreeStoreScript>SynFreeStore</GalacticFreeStoreScript>
  <GoalProposalFunctionSets>SynSet</GoalProposalFunctionSets>
  <Templates><Galactic>Syn_Default, Syn_Rich</Galactic></Templates>
  <Difficulty_Adjustments><Easy>Easy_Default</Easy><Normal>Normal_Default</Normal><Hard>Hard_Default</Hard></Difficulty_Adjustments>
</AIPlayerType>)";

static void test_definitions_and_templates() {
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
    CHECK(d.goals[3].mode == AiMode::Land && d.goals[2].reachability == Reachability::FriendlyOnly);
    CHECK(d.find_set("synset") && d.find_set("SynSet")->entries.size() == 4);
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
    rich = true;
    t.service(eval);  // count = 1: one template checked per frame (cursor: base)
    t.service(eval);  // Syn_Rich
    CHECK(t.active().size() == 2 && t.goal_value(2) == -3 && t.goal_value(cq) == 1);
    rich = false;
    t.service(eval);
    CHECK(t.active().size() == 1 && t.goal_value(2) == 1);
    // Budget (AI_GALACTIC_2 §3): init, add goal, tick (asymmetric branch, drift), reweight, can fund, release.
    AiBudget b;
    b.reset(3);
    const float raw[] = {3.0f, -1.0f, 1.0f};
    b.init(raw, 1000.0f);
    CHECK(b.weight(0) == 0.75f && b.weight(1) == 0.0f && b.weight(2) == 0.25f);
    CHECK(b.available(0) == 750.0f && b.pool(0) == 750.0f && b.rate(0) == 0.0f);
    CHECK(b.can_fund(0, 750.0f, 10.0f, -1.0f) && !b.can_fund(0, 751.0f, 10.0f, -1.0f));
    CHECK(b.add_goal(0, 7, 500.0f, 30.0f, 20.0f));
    const AiBudget::GoalEntry* e = b.goal(0, 7);
    CHECK(e && e->req == 500.0f && e->h == 20.0f && e->c == 500.0f && b.available(0) == 250.0f && e->q == 0.0f);
    // Tick: income 450 over T = 45 s -> rate 10; I = 0.75 x 10; share 0 (Iprev 0); x = 500/20 = 25 -> a = 1125.
    b.tick(450.0f, 45.0f, 1450.0f);
    e = b.goal(0, 7);
    CHECK(e && e->a == 25.0f * 45.0f && e->h == 0.0f && b.category(0).I == 7.5f);
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
}

// ================================================================ synthetic campaign

class MemoryFiles final : public FileProvider {
public:
    void add(const std::string& path, const std::string& text) { files_[normalize_path(path)] = text; }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        const auto it = files_.find(normalize_path(path));
        out.clear();
        if (it == files_.end()) return false;
        out.assign(it->second.begin(), it->second.end());
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "memory" : ""; }
    std::vector<std::string> list(std::string_view folder) const {
        std::vector<std::string> out;
        const std::string f = normalize_path(folder);
        for (const auto& [k, v] : files_)
            if (k.size() > f.size() && k.compare(0, f.size(), f) == 0 && k.find('\\', f.size()) == std::string::npos) out.push_back(k);
        return out;
    }

private:
    std::map<std::string, std::string> files_;
};

static const char* kSynBase = R"(
function ScriptExit()
  _ScriptExit()
  if GetThreadID() >= 0 then coroutine.yield(false) end
end
function PumpEvents()
  coroutine.yield(true)
  local e = GetEvent()
  while e do
    local p = GetEvent.Params()
    if p then e(unpack(p)) else e() end
    e = GetEvent()
  end
end
function Sleep(t)
  local s = GetCurrentTime()
  while GetCurrentTime() - s < t do PumpEvents() end
end
function BlockOnCommand(b, max)
  PumpEvents()
  if not b then return nil end
  local s = GetCurrentTime()
  repeat
    PumpEvents()
    if max and GetCurrentTime() - s > max then return nil end
  until b.IsFinished() == true
  return b.Result()
end
function TestValid(w)
  if w == nil then return false end
  if w.Is_Valid == nil then return false end
  return w.Is_Valid()
end
function Flush_G()
  local del = {}
  for k, v in pairs(_G) do
    if type(v) == "table" and k ~= "_LOADED" and k ~= "coroutine" and k ~= "string" and k ~= "table" and k ~= "_G" and k ~= "LuaWrapperMetaTable" then
      table.insert(del, k)
    end
  end
  for i, k in pairs(del) do _G[k] = nil end
end
)";
static const char* kSynAICommands = R"(
require("SynBase")
function Base_Definitions()
  MinContrastScale = 1.1
  MaxContrastScale = 1.5
  PerFailureContrastAdjust = 0.1
  AITarget = nil
  Target = nil
  PlayerObject = nil
  Budget = nil
  if TaskForce and type(TaskForce) == "table" then
    for i, d in pairs(TaskForce) do
      if type(d) == "table" and type(d[1]) == "string" then _G[d[1]] = nil end
    end
  end
  PlanDefinitionLoad = nil
  if Definitions then Definitions() end
end
)";
static const char* kPlanBuildSpace = R"(
require("SynAICommands")
ScriptPoolCount = 2
function Definitions()
  Category = "Build_Space_Forces"
  IgnoreTarget = true
  TaskForce = { { "ReserveForce", "Frigate | Corvette = 1,2" } }
  AllowFreeStoreUnits = false
end
function ReserveForce_Thread()
  ReserveForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(ReserveForce.Produce_Force())
  ReserveForce.Set_Plan_Result(true)
  ScriptExit()
end
)";
static const char* kPlanConquer = R"(
require("SynAICommands")
ScriptPoolCount = 2
function Definitions()
  Category = "Conquer_Planet"
  MinContrastScale = 1.2
  TaskForce = {
    { "SpaceForce", "MinimumTotalSize = 2", "Frigate | Corvette = 100%" },
    { "GroundForce", "Infantry = 2" }
  }
  RequiredCategories = { "Infantry" }
  SpaceSecured = false
  LandSecured = false
end
function SpaceForce_Thread()
  SpaceForce.Set_Plan_Result(true)
  BlockOnCommand(SpaceForce.Produce_Force(SpaceForce.Get_Stage()))
  if not BlockOnCommand(SpaceForce.Move_To(Target)) then
    SpaceForce.Set_Plan_Result(false)
    ScriptExit()
  end
  SpaceSecured = true
  while not LandSecured do Sleep(5) end
  SpaceForce.Release_Forces(1.0)
end
function GroundForce_Thread()
  BlockOnCommand(GroundForce.Produce_Force(GroundForce.Get_Stage()))
  while not SpaceSecured do Sleep(5) end
  BlockOnCommand(GroundForce.Launch_Units())
  BlockOnCommand(GroundForce.Move_To(Target))
  local st = GroundForce.Invade()
  if st and BlockOnCommand(st) then
    GroundForce.Set_As_Goal_System_Removable(false)
  else
    GroundForce.Set_Plan_Result(false)
  end
  LandSecured = true
  GroundForce.Release_Forces(1.0)
  ScriptExit()
end
function SpaceForce_No_Units_Remaining()
  SpaceForce.Set_Plan_Result(false)
  ScriptExit()
end
function GroundForce_Production_Failed(tf, t)
  ScriptExit()
end
)";
static const char* kPlanUpgrade = R"(
require("SynAICommands")
ScriptPoolCount = 4
function Definitions()
  Category = "Upgrade_Starbase"
  IgnoreTarget = true
  TaskForce = { { "BaseForce", "SB1 = 1" } }
end
function BaseForce_Thread()
  BaseForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(BaseForce.Produce_Force())
  BaseForce.Set_Plan_Result(true)
  GlobalValue.Set("upgrades", (GlobalValue.Get("upgrades") or 0) + 1)
  ScriptExit()
end
function BaseForce_Production_Failed(tf, t)
  ScriptExit()
end
)";
static const char* kSynFreeStore = R"(
require("SynBase")
function Base_Definitions()
  ServiceRate = 8
  UnitServciceRate = 8
  Added = 0
end
function main()
  while true do
    FreeStoreService()
    PumpEvents()
  end
end
function FreeStoreService()
  Count = FreeStore.Get_Object_Count()
end
function On_Unit_Added(object)
  Added = Added + 1
end
)";

enum SynType : uint32_t { tPlanet, tSB1, tSB2, tFrigate, tCorvette, tInfantry, tBarracks, tTypes };

static gal::GalacticRules syn_rules() {
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

static gal::GalacticSetup syn_setup() {
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

static void syn_data(MemoryFiles& files, AiDefinitions& defs, EquationSet& eqs, std::vector<std::string>& plan_files) {
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

static ParallelFor thread_pf(unsigned threads) {
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

struct CampaignResult {
    std::vector<uint64_t> crc;  // per frame
    std::string summary;
    std::vector<std::string> decisions;
    std::vector<std::string> errors;
    std::vector<int32_t> owned;
    uint64_t orders = 0, rejected = 0, conflicts = 0;
};

static CampaignResult run_synthetic(unsigned threads, int32_t frames) {
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
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
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

static std::string golden_text(const CampaignResult& r) {
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
static void test_plan_load_parallel() {
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

static void test_synthetic_campaign() {
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
static std::vector<uint64_t> run_path_world(unsigned threads, int32_t frames, lua::LuaPlayerWorld* story = nullptr) {
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

static void test_path_finder_world() {
    const int32_t frames = 30 * 40;
    lua::LuaPlayerWorld story;  // outlives the AI world: detached by its destructor
    const std::vector<uint64_t> base = run_path_world(1, frames, &story);
    CHECK(base.size() == static_cast<size_t>(frames) && story.ai_paths() == nullptr);
    for (const unsigned t : {2u, 4u, 15u}) CHECK(run_path_world(t, frames) == base);
}

// A2 galactic: the GalacticSim hooks the galactic AI uses. The §3.1 budget tick comes from the sim's income-tick
// listener (rate = gain / T of that cycle); the world may be destroyed before the sim (the listener goes quiet); fleet
// launches are FleetManagement events through the order sink.
static void test_sim_hooks() {
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
// AI_Combat_Power is the helper 0x5f2430's add (GALACTIC_ECONOMY 6.2) and lands in the PotentialPlan's untyped totals only.
static void test_starbase_force_split() {
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
    // The leaf (members only) and the 0x5f2430 helper (+ the starbase's own power in the untyped total and in every typed
    // row its CategoryMask meets: the Capital row, a2i t03 D1 / A2F_RE_READS §3.1).
    CHECK(galactic_ai_planet_force(sim, ti, 7, 0, true, true, kAllCategories) == 2000.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, kAllCategories) == 2600.0);
    CHECK(galactic_ai_enemy_strength(sim, ti, 7, 0, true, cap) == 600.0);
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
    std::printf("starbase force split: leaf 2000 (members), helper 0x5f2430 untyped 2600 (+ own 600), Capital row 0, Frigate row 1000\n");
}

// A2 t03: per-tech members of the other objects the force totals walk (AI_GALACTIC_TOKENS 2 / 3): every unit of an orbiting
// fleet adds its type's members to the space leaf ("each unit's type power (category) + its per-tech members"); every
// ground structure adds its members (never its own power) to the ground leaf; surface units add only their power; a
// ground structure with Is_Special_Weapon_In_Space adds its own power to the space leaf by the structure mask (Capital
// default); the player-root totals add the members of every walked object.
static void test_force_members() {
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

// A2 galactic, AI_GALACTIC_3 rules: heroes never attach through their AI script (§2.4), the TerrainType veto (§6.3),
// the galactic population cap when AI_Uses_Galactic_Pop_Cap is set (§3.5), MagicPlan spawns without production or
// charge (§4), and the perception DLL's division / clamp failures (§7.1).
static const char* kPlanHeroAttach = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "HeroShip | Corvette = 1,1" } }
end
function MainForce_Thread() ScriptExit() end
)";
static const char* kPlanHeroDeny = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "HeroShip | Corvette = 1,1" } }
end
function MainForce_Thread() ScriptExit() end
)";
static const char* kPlanMagic = R"(
require("SynAICommands")
ScriptPoolCount = 1
function Definitions()
  Category = "Build_Space_Forces"
  IgnoreTarget = true
  MagicPlan = true
  MagicPlanStealing = false
  TaskForce = { { "MagicForce", "Corvette = 1" } }
end
function MagicForce_Thread()
  MagicForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(MagicForce.Produce_Force())
  MagicForce.Set_Plan_Result(true)
  ScriptExit()
end
)";
// A2 f t01: a ground-structure plan on a friendly target (Upgrade_Starbase = the synthetic Infrastructure goal).
static const char* kPlanBuildBarracks = R"(
require("SynAICommands")
function Definitions()
  Category = "Upgrade_Starbase"
  TaskForce = { { "BaseForce", "Barracks = 1" } }
end
function BaseForce_Thread()
  BaseForce.Set_As_Goal_System_Removable(false)
  BlockOnCommand(BaseForce.Produce_Force())
  BaseForce.Set_Plan_Result(true)
  ScriptExit()
end
function BaseForce_Production_Failed(tf, t)
  ScriptExit()
end
)";

// A2f t02: a category-mask team (CORVETTE is a GameObjectCategoryType name, AI_GALACTIC_2 §1.3).
static const char* kPlanMaskCorvette = R"(
require("SynAICommands")
function Definitions()
  Category = "Conquer_Planet"
  TaskForce = { { "MainForce", "DenyHeroAttach", "Corvette = 1" } }
end
function MainForce_Thread() ScriptExit() end
)";

struct ParityWorld {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    std::vector<std::string> plan_files;
    gal::GalacticRules rules;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    std::unique_ptr<objects::ObjectStore> store;
    std::unique_ptr<GameRandom> rng;
    std::unique_ptr<gal::GalacticSim> sim;
    std::unique_ptr<GalacticAiWorld> world;
    uint32_t hero = 0, planet_r = 0;
    std::vector<std::string> orders;

    // only: the plan files to load (by base name), empty = all. rules_tweak: applied to the rules before the sim starts;
    // eq_first: equations added before the synthetic ones (a name added here wins: the first definition is kept);
    // tinfo_tweak: applied to the AI type info before the world is built.
    ParityWorld(bool pop_cap, const std::vector<std::string>& only,
                const std::function<void(gal::GalacticSetup&)>& tweak = {},
                const std::function<void(gal::GalacticRules&)>& rules_tweak = {},
                const std::function<void(EquationSet&)>& eq_first = {},
                const std::function<void(std::vector<GalacticAiTypeInfo>&)>& tinfo_tweak = {}) {
        if (eq_first) eq_first(eqs);
        syn_data(files, defs, eqs, plan_files);
        files.add("Data\\Scripts\\AI\\HeroAttach.lua", kPlanHeroAttach);
        files.add("Data\\Scripts\\AI\\HeroDeny.lua", kPlanHeroDeny);
        files.add("Data\\Scripts\\AI\\MagicSpace.lua", kPlanMagic);
        files.add("Data\\Scripts\\AI\\BuildBarracks.lua", kPlanBuildBarracks);
        files.add("Data\\Scripts\\AI\\MaskCorvette.lua", kPlanMaskCorvette);
        plan_files = list_plan_files([this](std::string_view glob) {
            const std::string g(glob);
            return files.list(g.substr(0, g.find('*')));
        });
        if (!only.empty())
            plan_files.erase(std::remove_if(plan_files.begin(), plan_files.end(),
                                            [&only](const std::string& f) {
                                                for (const std::string& o : only)
                                                    if (ai_iequal(f.substr(f.find_last_of('\\') + 1), o + ".lua")) return false;
                                                return true;
                                            }),
                             plan_files.end());
        rules = syn_rules();
        hero = static_cast<uint32_t>(rules.types.size());
        gal::TypeInfo h;
        h.name = "HeroShip";
        h.kind = gal::UnitKind::SpaceUnit;
        h.ai_combat_power = 3000.0f;
        h.hyperspace = true;
        h.hyperspace_speed = 2.0f;
        rules.types.push_back(h);
        planet_r = static_cast<uint32_t>(rules.types.size());
        gal::TypeInfo pr = rules.types[tPlanet];
        pr.name = "SynPlanetRestricted";
        pr.restricted_types = {tCorvette};
        rules.types.push_back(pr);
        if (pop_cap) {
            rules.constants.ai_uses_galactic_pop_cap = true;
            rules.types[tCorvette].population = 50;  // more than any capacity of this map
        }
        if (rules_tweak) rules_tweak(rules);
        store = std::make_unique<objects::ObjectStore>(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
        rng = std::make_unique<GameRandom>(4242u, 77u);
        sim = std::make_unique<gal::GalacticSim>(rules, *store, rng->synced());
        gal::GalacticSetup s = syn_setup();
        s.forces.push_back({0, 0, hero});  // the Empire's hero on its free store
        s.planet_type[2] = planet_r;       // P2 restricts Corvettes
        if (tweak) tweak(s);
        CHECK(sim->start(s));
        std::vector<GalacticAiTypeInfo> tinfo(rules.types.size());
        tinfo[tFrigate].categories = {"FRIGATE"};
        tinfo[tCorvette].categories = {"CORVETTE"};
        tinfo[tInfantry].categories = {"INFANTRY"};
        tinfo[hero].hero = true;
        tinfo[hero].lua_script = "HeroShipScript";  // a hero AI script: a hero record (§2.1)
        tinfo[tCorvette].unit_ai = true;            // STRATEGIC_GAPS §1.1 removal feed filter
        tinfo[tInfantry].company_first = tCorvette; // (synthetic) company-leader substitution for the survival key
        if (tinfo_tweak) tinfo_tweak(tinfo);
        GalacticAiConfig cfg;
        cfg.players = {{0, "SynEmpire", 1}};
        cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
        world = std::make_unique<GalacticAiWorld>(
            *sim, *store, &types, &files, *rng, defs, eqs, tinfo,
            [this](uint8_t p, const commands::EventData& ev) {
                orders.push_back(std::to_string(static_cast<int>(ev.type_id())));
                return sim->execute(p, ev);
            },
            cfg);
        CHECK(world->load_plans(plan_files) && world->start());
    }
    void run(int32_t frames) {
        for (int32_t f = 1; f <= frames; ++f) {
            sim->update();
            world->update_player(0, f, true, false);
        }
    }
    bool has(const std::vector<uint32_t>& sel, uint32_t t) const { return std::find(sel.begin(), sel.end(), t) != sel.end(); }
};

// a1h t07: a build task of an AllowFreeStoreUnits plan takes its unit through 0x4d2bd0, which does not read the free-store
// entry's +0x30 owner (A1_READINGS §28.5): a unit another PotentialPlan reserved is still supplied.
static void test_free_store_supply() {
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

static void test_parity_rules() {
    {
        ParityWorld pw(false, {"HeroAttach", "HeroDeny"});
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
        // §3.5: with AI_Uses_Galactic_Pop_Cap a produced Corvette (pop 50) exceeds the remaining population.
        ParityWorld off(false, {"HeroAttach"}), on(true, {"HeroAttach"});
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

// OI2 ai: production-disabled types (story DISABLE_BUILDABLE, REMAINING_GAPS §1.5) are never picked by AI production -
// neither by a normal plan's Production marshaller nor by a MagicPlan spawn nor by the team picks; and the build
// market's delivery path class (REMAINING_GAPS §6.2): 2 by default, 3 with +0xDA, 5 for Is_Stealth_Company.
static void test_production_disabled_and_delivery() {
    {
        ParityWorld pw(false, {"HeroAttach"});
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
static void test_ground_structure_slots() {
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

// A2 f t01 (2): BudgetFractionToBuild 0x667120 fails only without a goal system / for an unknown type. Before the
// budget's Init it reads the constructed zeros (an empty category = income rate + credits; a category = 0 / 0 -> D = 0
// -> never), so a template budget equation that reads it does not fail at Init (AI_GALACTIC_2 §3.2): the category keeps
// its weight and gets its share of the starting credits.
static void test_budget_fraction_before_init() {
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
static void test_production_unbuildable() {
    bool ok = false;
    float eta = -1.0f;
    const auto no_hero = [](gal::GalacticSetup& s) {
        s.forces.erase(std::remove_if(s.forces.begin(), s.forces.end(), [](const auto& f) { return f.type == tTypes; }), s.forces.end());
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
        ParityWorld pw(false, {"MaskCorvette"}, {}, add, {}, cat);
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

// A2f t02 R2: maintenance drops a replaceable goal when AllowedTime < ETA, ETA = 0x525200 + (NOW - created) / FPS
// (AI_GALACTIC §1.6) [H; 0x525200 = the time the goal's build tasks still need, M]. Was "ETA only": never late.
static const char* kLateGoals = R"(<Goals>
  <Late_Goal><AIGoalApplicationFlags>Friendly</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Any</Reachability><Time_Limit>240</Time_Limit>
    <Build_Time_Delay_Tolerance>1.3</Build_Time_Delay_Tolerance></Late_Goal>
</Goals>)";

enum class LateMode { Stalled, OnSchedule };

struct LateHooks final : GoalModeHooks {
    int64_t now = 0, ended = -1, created = -1;
    bool removable = true;
    float eta = 10.0f;
    float nominal = -1.0f;  // Goal::nominal_time (A2f t09); < 0 = the ETA
    LateMode mode = LateMode::Stalled;
    uint32_t target_count() override { return 1; }
    bool applies(int32_t, uint32_t) override { return true; }
    bool blocked(uint32_t, int32_t) override { return false; }
    bool evaluate(const std::string&, int32_t, double& out) override {
        out = 10.0;
        return true;
    }
    bool accept(Goal& g) override {
        g.plan = 0;
        g.pp_valid = true;
        g.eta = eta;
        g.nominal_time = nominal < 0.0f ? eta : nominal;
        return true;
    }
    bool plan_activatable(Goal&) override { return true; }
    bool plan_invalid(const Goal& g) override { return g.running < 0; }
    bool replaceable(const Goal&) override { return removable; }
    void refresh_plan(Goal&) override {}
    bool uses_budget() override { return true; }
    bool can_fund(const Goal&) override { return true; }
    float category_credits(int32_t) override { return 1.0f; }
    void budget_add(Goal&) override {}
    void budget_release(Goal&) override {}
    void budget_weights() override {}
    std::vector<int32_t> category_order() override { return {}; }
    void queue_production(Goal&) override {}
    void instantiate(Goal& g) override {
        g.running = 1;
        created = g.created;
    }
    void end_plan(Goal&, bool) override {
        if (ended < 0) ended = now;
    }
    void drop_potential_plan(Goal&) override {}
    void suspend_reservations(Goal&) override {}
    float elapsed_eta(const Goal& g) override {
        // Stalled: the build still needs its whole time; on schedule: the remaining time falls with the clock.
        const float elapsed = static_cast<float>(now - g.created) / static_cast<float>(kAiFps);
        const float production = mode == LateMode::Stalled ? g.eta : std::max(g.eta - elapsed, 0.0f);
        return galactic_maintenance_eta(production, g.created, now);
    }
    int32_t quota_players() override { return 1; }
};

static GoalSystemConfig galactic_goal_config() {
    GoalSystemConfig c;
    c.nominal_allowed_time = true;  // as the galactic AI (A2f t09)
    return c;
}

static int64_t late_goal_released_after(bool removable, float eta, LateMode mode, float nominal = -1.0f, int64_t* created = nullptr) {
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

static void test_goal_time_drop() {
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
static const Goal* last_active(const GoalSystem& gs) {
    const Goal* out = nullptr;
    for (const auto& cat : gs.active())
        for (const Goal& a : cat) out = &a;
    return out;
}

static void test_goal_allowed_time_base() {
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
static void test_btime_none_speed_factor() {
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
static void test_goal_nominal_time_resolve() {
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

// ================================================================ a2g t05 (b): Resolve's time_ok (0x6c8970) on its own
//
// Resolve_Category (AI_GALACTIC §1.6): a candidate passes plan_activatable (0x6c4a50) and fundable, then time_ok: AllowedTime
// (nominal_time x Build_Time_Delay_Tolerance, A2f t09) >= ETA, or AllowedTime <= 0. A failure in the greedy phase records an
// activation failure and stops it; the weighted phase then draws the goal, fails it again and releases it. Tolerance 2.0
// keeps the products exact; ETA 10.
static const char* kResolveGoals = R"(<Goals>
  <Resolve_Goal><AIGoalApplicationFlags>Friendly</AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Any</Reachability><Time_Limit>240</Time_Limit>
    <Build_Time_Delay_Tolerance>2.0</Build_Time_Delay_Tolerance>
    <Activation_Tracking_Duration>60</Activation_Tracking_Duration></Resolve_Goal>
</Goals>)";

struct ResolveHooks final : GoalModeHooks {
    float eta = 10.0f, nominal = 20.0f;  // set at accept
    float resolved = -1.0f;              // >= 0: the re-solve's new nominal_time
    bool budget = true;
    int32_t activatable_calls = 0, drops = 0, instantiated = 0;
    uint32_t target_count() override { return 1; }
    bool applies(int32_t, uint32_t) override { return true; }
    bool blocked(uint32_t, int32_t) override { return false; }
    bool evaluate(const std::string&, int32_t, double& out) override {
        out = 10.0;
        return true;
    }
    bool accept(Goal& g) override {
        g.plan = 0;
        g.pp_valid = true;
        g.eta = eta;
        g.nominal_time = nominal;
        return true;
    }
    bool plan_activatable(Goal& g) override {
        ++activatable_calls;
        if (resolved >= 0.0f) g.nominal_time = resolved;
        return true;
    }
    bool plan_invalid(const Goal& g) override { return g.running < 0; }
    bool replaceable(const Goal&) override { return true; }
    void refresh_plan(Goal&) override {}
    bool uses_budget() override { return budget; }
    bool can_fund(const Goal&) override { return true; }
    float category_credits(int32_t) override { return 1.0f; }
    void budget_add(Goal&) override {}
    void budget_release(Goal&) override {}
    void budget_weights() override {}
    std::vector<int32_t> category_order() override { return {}; }
    void queue_production(Goal&) override {}
    void instantiate(Goal& g) override {
        g.running = 1;
        ++instantiated;
    }
    void end_plan(Goal&, bool) override {}
    void drop_potential_plan(Goal&) override { ++drops; }
    void suspend_reservations(Goal&) override {}
    float elapsed_eta(const Goal& g) override { return g.eta; }
    int32_t quota_players() override { return 1; }
};

struct ResolveOutcome {
    bool activated = false;
    int32_t activatable_calls = 0, activation_failures = 0, drops = 0;
};

static ResolveOutcome resolve_once(float nominal, float resolved, bool budget, bool galactic = true) {
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

static void test_goal_resolve_time_ok() {
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
static void test_active_goals_category() {
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
static void test_maintenance_tokens() {
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
static void test_force_maintenance_tokens() {
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
    const double em7 = token("Variable_Target.EnemyForce.Maintenance", 0, 1, 7, ok);
    CHECK(gross1 > 0.0 ? ok && em7 == e7 / gross1 : !ok);
    token("Variable_Target.EnemyForce.Maintenance", 0, 3, 7, ok);
    const bool ok_enemy_gross0 = ok;
    token("Variable_Target.EnemyForce.Maintenance", 0, -1, 7, ok);
    const bool ok_unbound = ok;
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
static void test_budget_fraction_to_build() {
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
static bool patch_queue_head(gal::GalacticSim& sim, PlanetIndex planet, uint32_t q, float build_time, int32_t start,
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

static void test_budget_fraction_in_progress() {
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
static void test_budget_reset_on_template_switch() {
    // AiBudget alone: Init {1, 1} with 1000 credits (A = P = 500 each), reweight {3, 1} (w 0.75 / 0.25, A = P = 500), one
    // tick with 1100 credits (Σ = 1000). Without the flag P = 500 + 100 w; with it P = 1000 w + 100 w.
    auto run = [](bool reset, AiBudget& b) {
        b.reset(2);
        const std::vector<float> even = {1.0f, 1.0f}, skew = {3.0f, 1.0f};
        b.init(even, 1000.0f);
        b.reweight(skew);
        if (reset) b.request_reset();
        CHECK(b.reset_requested() == reset);
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
        CHECK(back.read(r) && back.reset_requested());
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
    CHECK(!flagged.reset_requested() && !plain.reset_requested());  // the tick clears it
    flagged.tick(0.0f, 45.0f, 1100.0f);  // the next tick starts from P = A again and has no reset: the unflagged result
    CHECK(flagged.pool(0) == 575.0f && flagged.pool(1) == 525.0f);
    AiBudget again;  // init clears a pending flag
    again.reset(2);
    again.request_reset();
    const std::vector<float> even = {1.0f, 1.0f};
    again.init(even, 10.0f);
    CHECK(!again.reset_requested());

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
            if (c->budget.reset_requested() && set_at < 0) set_at = f;
            if (set_at >= 0 && !c->budget.reset_requested()) cleared_at = f;
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
static void test_free_store_path_rules() {
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
static void test_free_store_world() {
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
    CHECK(world.reachable_target(0, "One", "Neutral", "Friendly_Only", 0.1) == -1);  // unowned: not reachable
    CHECK(world.reachable_target(0, "One", "Neutral", "Enemy_Destination", 0.1) == 2);
    CHECK(pw.rng->synced().draws() == d0);  // one candidate: no draw
    const int32_t pick = world.reachable_target(0, "One", "Friendly", "Friendly_Only", 0.1);
    CHECK((pick == 0 || pick == 1) && pw.rng->synced().draws() == d0 + 1);  // two candidates, p < 1: one draw
}

// ================================================================ P4 ai: Underworld special systems
//
// A synthetic Underworld AI against a passive Empire, with the four shipped Underworld hero plans (their Lua verbatim,
// require line aside) and the shipped Underworld goal equations (the save / planet-value terms reduced). The plans act
// through GalacticSim::ground_activated_trigger (Activate_Ability / Land_Units): the saboteur corrupts a clean planet
// (corruption_ai_pick) and sabotages a corrupted one, Tyber Zann buys on the black market of the planet he corrupted,
// IG-88 hacks the Death Star II orbiting a corrupted planet. Per-frame CRCs identical for 0 / 1 / 4 / 15 workers and
// against tests/golden/ai_underworld_crc.txt (MSVC and clang check the same file).
static const char* kUwLibrary = R"(
require("SynAICommands")
function AssembleForce(taskforce)
  BlockOnCommand(taskforce.Produce_Force(taskforce.Get_Stage()))
end
function LandUnits(taskforce)
  BlockOnCommand(taskforce.Land_Units())
end
)";
static const char* kUwPlanBlackMarket = R"(
require("SynUnderworld")
function Definitions()
	Category = "Black_Market_Purchase"
	IgnoreTarget = true
	TaskForce = {
		{
			"MainForce",
			"DenyHeroAttach",
			"Silri_Team | Tyber_Zann_Team | Urai_Fen_Team = 1"
		}
	}
end
function MainForce_Thread()
	BlockOnCommand(MainForce.Produce_Force(Target))
	BlockOnCommand(MainForce.Move_To(Target))
	MainForce.Activate_Ability()
	MainForce.Set_Plan_Result(true)
end
function MainForce_Production_Failed(tf, failed_object_type)
	ScriptExit()
end
)";
static const char* kUwPlanSaboteur = R"(
require("SynUnderworld")
function Definitions()
	Category = "Corrupt_Planet | Sabotage_Planet"
	IgnoreTarget = true
	TaskForce = {
		{
			"MainForce",
			"DenyHeroAttach",
			"Underworld_Saboteur_Team = 1"
		}
	}
end
function MainForce_Thread()
	AssembleForce(MainForce)
	BlockOnCommand(MainForce.Move_To(Target))
	MainForce.Activate_Ability()
	Sleep(10)
	if MainForce.Get_Goal_Type_Name() == "Corrupt_Planet" then
		if Target.Is_Corrupted() then
			MainForce.Set_Plan_Result(true)
		end
	end
end
function MainForce_Production_Failed(tf, failed_object_type)
	ScriptExit()
end
)";
static const char* kUwPlanHack = R"(
require("SynUnderworld")
function Definitions()
	Category = "Hack_Super_Weapon"
	IgnoreTarget = true
	TaskForce = {
		{
			"MainForce",
			"DenyHeroAttach",
			"IG88_Team = 1"
		}
	}
end
function MainForce_Thread()
	BlockOnCommand(MainForce.Produce_Force(Target))
	BlockOnCommand(MainForce.Move_To(Target))
	BlockOnCommand(LandUnits(MainForce))
	MainForce.Set_Plan_Result(true)
end
function MainForce_Production_Failed(tf, failed_object_type)
	ScriptExit()
end
)";
static const char* kUwGoals = R"(<Goals>
  <Corrupt_Planet><AIGoalApplicationFlags> Enemy </AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Infrastructure</Category><Reachability>Any</Reachability><Time_Limit>60.0</Time_Limit>
    <Build_Time_Delay_Tolerance>1.25</Build_Time_Delay_Tolerance><Tracking_Duration>300.0</Tracking_Duration>
    <Per_Failure_Desire_Adjust>-20.0</Per_Failure_Desire_Adjust></Corrupt_Planet>
  <Black_Market_Purchase><AIGoalApplicationFlags> Enemy </AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Hero</Category><Reachability>Any</Reachability><Time_Limit>0.0</Time_Limit></Black_Market_Purchase>
  <Hack_Super_Weapon><AIGoalApplicationFlags> Enemy </AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Hero</Category><Reachability>Any</Reachability><Time_Limit>0.0</Time_Limit></Hack_Super_Weapon>
  <Sabotage_Planet><AIGoalApplicationFlags> Enemy </AIGoalApplicationFlags><GameMode>Galactic</GameMode>
    <Category>Offensive</Category><Reachability>Any</Reachability><Time_Limit>90.0</Time_Limit>
    <Build_Time_Delay_Tolerance>1.5</Build_Time_Delay_Tolerance><Tracking_Duration>300.0</Tracking_Duration>
    <Per_Failure_Desire_Adjust>-20.0</Per_Failure_Desire_Adjust></Sabotage_Planet>
</Goals>)";
static const char* kUwSet = R"(<FunctionSet>
  <Corrupt_Planet><Goal>Corrupt_Planet</Goal><Function>Should_Corrupt_Planet</Function></Corrupt_Planet>
  <Black_Market_Purchase><Goal>Black_Market_Purchase</Goal><Function>Should_Make_Black_Market_Purchase</Function></Black_Market_Purchase>
  <Hack_Super_Weapon><Goal>Hack_Super_Weapon</Goal><Function>Should_Hack_Super_Weapon</Function></Hack_Super_Weapon>
  <Sabotage_Planet><Goal>Sabotage_Planet</Goal><Function>Should_Sabotage_Planet</Function></Sabotage_Planet>
</FunctionSet>)";
static const char* kUwTemplates = R"(<AITemplates>
  <Uw_Default><Priority>1</Priority><Trigger>One</Trigger>
    <Budget><Offensive>Offensive_Budget</Offensive><Infrastructure>Infra_Budget</Infrastructure><Hero>Zero</Hero></Budget>
    <Turn_On><Goals><Category>Offensive</Category><Category>Infrastructure</Category><Category>Hero</Category></Goals>
      <Plans><Goal_Category>Offensive</Goal_Category><Goal_Category>Infrastructure</Goal_Category><Goal_Category>Hero</Goal_Category></Plans></Turn_On>
  </Uw_Default>
</AITemplates>)";
static const char* kUwPlayer = R"(<AIPlayerType><Name>SynUnderworld</Name>
  <GalacticFreeStoreScript>SynFreeStore</GalacticFreeStoreScript>
  <GoalProposalFunctionSets>UwSet</GoalProposalFunctionSets>
  <Templates><Galactic>Uw_Default</Galactic></Templates>
  <Difficulty_Adjustments><Easy>Normal_Default</Easy><Normal>Normal_Default</Normal><Hard>Normal_Default</Hard></Difficulty_Adjustments>
</AIPlayerType>)";

enum UwType : uint32_t { uPlanet, uStruct, uDeathStar, uTyber, uSaboteur, uIG88, uTypes };
constexpr PlanetIndex kUwPlanets = 6;  // P0 Underworld home; P1..P5 Empire; P2, P3 corrupted at start; DS II over P3

static gal::GalacticRules uw_rules() {
    gal::GalacticRules r;
    gal::FactionRules emp;
    emp.name = "Empire";
    emp.can_control_planets = emp.is_playable = true;
    gal::FactionRules uw = emp;
    uw.name = "Underworld";
    gal::FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, uw, neu};
    r.difficulties = {{"Normal_Default", 1.0f, 1.0f}};
    r.types.resize(uTypes);
    r.types[uPlanet].name = "UwPlanet";
    r.types[uPlanet].kind = gal::UnitKind::Planet;
    r.types[uPlanet].credit_value = 60;
    r.types[uStruct].name = "E_Ground_Research_Facility";
    r.types[uStruct].kind = gal::UnitKind::GroundStructure;
    r.types[uStruct].build_cost = 1200;
    r.types[uDeathStar].name = "Death_Star_II";
    r.types[uDeathStar].kind = gal::UnitKind::SpaceUnit;
    r.types[uDeathStar].ai_combat_power = 20000.0f;
    for (const auto& [t, name] : {std::pair{uTyber, "Tyber_Zann_Team"}, std::pair{uSaboteur, "Underworld_Saboteur_Team"},
                                  std::pair{uIG88, "IG88_Team"}}) {
        gal::TypeInfo& h = r.types[t];
        h.name = name;
        h.kind = gal::UnitKind::GroundUnit;  // heroes with their own transport (synthetic)
        h.hyperspace = true;
        h.hyperspace_speed = 2.0f;
        h.ai_combat_power = 300.0f;
    }
    r.types[uSaboteur].build_cost = 600;  // the generic hero is buildable (vanilla: the Underworld builds saboteurs)
    r.types[uSaboteur].build_time_seconds = 15;
    r.types[uSaboteur].affiliation = {1};
    r.types[uSaboteur].required_ground_base = 1;
    gal::SpecialRules& s = r.special;
    auto ability = [&s](gal::SpecialAbilityClass cls, const char* name, bool despawn) -> uint32_t {
        gal::SpecialAbilityDef a;
        a.cls = cls;
        a.name = name;
        a.name_crc = gal::special_name_crc(name);
        a.style = gal::ActivationStyle::GroundActivated;
        a.causes_despawn = despawn;
        s.abilities.push_back(a);
        return static_cast<uint32_t>(s.abilities.size() - 1);
    };
    const uint32_t bm = ability(gal::SpecialAbilityClass::BlackMarket, "Tyber_Black_Market", true);
    s.abilities[bm].bm_min_respawn = {20.0f, 20.0f, 20.0f, 20.0f, 20.0f};
    s.abilities[bm].bm_max_respawn = {40.0f, 40.0f, 40.0f, 40.0f, 40.0f};
    const uint32_t sab = ability(gal::SpecialAbilityClass::Sabotage, "Saboteur_Galactic_Sabotage", true);
    s.abilities[sab].sabotage_can[gal::kSabotageSpecialStructure] = true;  // the vanilla saboteur: only this option
    const uint32_t cor = ability(gal::SpecialAbilityClass::Corruption, "Saboteur_Corruption", true);
    const uint32_t hack = ability(gal::SpecialAbilityClass::Hack, "IG88_Hack_Death_Star", false);
    s.activated_sabotage = {"Saboteur_Galactic_Sabotage"};
    s.types.resize(uTypes);
    s.types[uTyber].abilities = {bm};
    s.types[uTyber].named_hero = true;
    s.types[uSaboteur].abilities = {sab, cor};
    s.types[uSaboteur].generic_hero = true;
    s.types[uIG88].abilities = {hack};
    s.types[uIG88].named_hero = true;
    s.types[uStruct].sabotage_cost = 1000;
    gal::CorruptionChoiceDef bmc;  // Corruption_1_Type = Black_Market (6), the only choice
    bmc.slot = 1;
    bmc.type = 6;
    bmc.credit_cost = 500.0f;
    s.types[uPlanet].corruption = {bmc};
    s.types[uPlanet].black_market_modifier = 100.0f;
    for (const auto& [name, tech, cost] : {std::tuple{"Uw_Item_Low", 1, 1000}, std::tuple{"Uw_Item_High", 3, 2000}}) {
        gal::BlackMarketItemDef it;
        it.name = name;
        it.name_crc = gal::special_name_crc(name);
        it.faction = 0;  // Empire items: offered on Empire planets
        it.tech_level = tech;
        it.base_cost = cost;
        it.uncommon_price_multiplier = 5.5f;
        s.items.push_back(it);
    }
    s.default_hero_respawn_time = 30.0f;
    s.hack_required_type = uDeathStar;
    return r;
}

static gal::GalacticSetup uw_setup() {
    gal::GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "SyntheticUnderworld";
    for (PlanetIndex i = 0; i < kUwPlanets; ++i) {
        m.planets.add("U" + std::to_string(i));
        m.planets.position[i] = {static_cast<float>(i) * 50.0f, 0.0f, 0.0f};
        m.planets.flags[i] = kPlanetHasPlanetBehavior;
        m.planets.max_space_base[i] = 2;
        m.planets.max_ground_base[i] = 3;
    }
    for (PlanetIndex i = 0; i + 1 < kUwPlanets; ++i) {
        const uint32_t r = m.routes.add("UR" + std::to_string(i));
        m.routes.point_a[r] = i;
        m.routes.point_b[r] = i + 1;
        m.routes.hs_speed_factor[r] = 4.0f;
        m.routes.credit_gain_factor[r] = 0.0f;
    }
    s.planet_type.assign(kUwPlanets, uPlanet);
    gal::PlayerSetup emp;  // AI-controlled, no galactic AI attached: passive
    emp.faction = 0;
    emp.ai = true;
    emp.difficulty = 0;
    emp.credits = 5000.0f;
    gal::PlayerSetup uw = emp;
    uw.faction = 1;
    uw.credits = 20000.0f;
    gal::PlayerSetup neu;
    neu.faction = 2;
    s.players = {emp, uw, neu};
    s.forces = {{1, 0, uTyber}, {1, 0, uSaboteur}, {1, 0, uIG88}, {1, 0, uStruct}};  // a ground base at home
    for (PlanetIndex p = 1; p < kUwPlanets; ++p) s.forces.push_back({0, p, uStruct});
    s.forces.push_back({0, 3, uDeathStar});
    return s;
}

struct UwResult {
    std::vector<uint64_t> crc;
    std::vector<std::string> decisions, errors;
    std::string summary;
    int8_t corruption_p1 = -1;
    int32_t completed_bm = 0, sabotage_p2 = 0;
    size_t purchased = 0;
    bool p3_destroyed = false, ds_locked = false, ds_alive = true;
    float uw_credits = 0.0f;
    // a2k t08: the AI update in which the first corruption applied: credits and Σ category pools / available before
    // and after it (frame -1: no corruption applied).
    int32_t corr_frame = -1;
    float corr_credits_before = 0.0f, corr_credits_after = 0.0f;
    double corr_pool_before = 0.0, corr_pool_after = 0.0, corr_avail_before = 0.0, corr_avail_after = 0.0;
};

static UwResult run_underworld(unsigned threads, int32_t frames) {
    UwResult out;
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    files.add("Data\\Scripts\\Library\\SynBase.lua", kSynBase);
    files.add("Data\\Scripts\\Library\\SynAICommands.lua", kSynAICommands);
    files.add("Data\\Scripts\\Library\\SynUnderworld.lua", kUwLibrary);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_BlackMarket.lua", kUwPlanBlackMarket);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_DeploySaboteur.lua", kUwPlanSaboteur);
    files.add("Data\\Scripts\\AI\\AI_Plan_Underworld_HackSuperWeapon.lua", kUwPlanHack);
    files.add("Data\\Scripts\\FreeStore\\SynFreeStore.lua", kSynFreeStore);
    defs.add_goals(kUwGoals, "goals.xml");
    defs.link();
    defs.add_function_set("UwSet", kUwSet, "UwSet.xml");
    defs.add_templates(kUwTemplates, "t.xml");
    defs.add_players(kUwPlayer, "p.xml");
    eqs.add("One", "1.0");
    eqs.add("Zero", "0.0");
    eqs.add("Offensive_Budget", "5");
    eqs.add("Infra_Budget", "5");
    // AI_EQUATIONS_UNDERWORLD_GALACTIC.XML (Can_* / Should_* verbatim except the reduced terms noted).
    eqs.add("Can_Corrupt_Planet",
            "Variable_Target.RetainsResidualInfluence * (Variable_Target.IsCorrupted == 0.0) * "
            "(Variable_Target.IsCorruptionTransitionActive == 0.0) * "
            "((Variable_Target.IsConnectedToCorruption + (Game.Age > Variable_Target.TimeSinceCorruptionChange)) > 0)");
    eqs.add("Should_Corrupt_Planet",
            "Function_Can_Corrupt_Planet.Evaluate * (13.0 + 5.0 * Variable_Target.IsConnectedToCorruption + "
            "5.0 * Variable_Target.IncomeUnnormalized / 75)");
    eqs.add("Can_Make_Black_Market_Purchase",
            "Variable_Target.IsCorrupted * Variable_Target.BlackMarketAbilitiesAvailable {Parameter_Faction = \"Underworld\"}");
    eqs.add("Should_Make_Black_Market_Purchase",
            "Function_Can_Make_Black_Market_Purchase.Evaluate * "
            "(Variable_Self.CreditsUnnormalized > Variable_Target.BlackMarketMinimumAbilityPrice {Parameter_Faction = \"Underworld\"}) * "
            "(10.0 - Variable_Target.BlackMarketPriceModifier / 300.0)");
    eqs.add("Should_Hack_Super_Weapon",  // the Palpatine / planets-controlled weights reduced to a constant
            "Variable_Target.IsCorrupted * (Variable_Target.EnemyForce.HasSpaceUnitsBitfield {Parameter_Type = \"Death_Star_II\"} > 0.0) * 10.0");
    eqs.add("Can_Sabotage_Planet", "Variable_Target.IsCorrupted * (Variable_Target.GroundbaseLevelUnnormalized > 0)");
    eqs.add("Should_Sabotage_Planet",  // GenericPlanetValue / Should_Save_For_Corruption terms dropped
            "Function_Can_Sabotage_Planet.Evaluate * (50.0 + 10.0 * Variable_Target.HasStructure {Parameter_Type = \"E_Ground_Research_Facility\"})");
    eqs.link();
    const std::vector<std::string> plan_files = list_plan_files([&files](std::string_view glob) {
        const std::string g(glob);
        return files.list(g.substr(0, g.find('*')));
    });
    const gal::GalacticRules rules = uw_rules();
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(5150u, 3u);
    gal::GalacticSim sim(rules, store, rng.synced());
    if (threads > 0) sim.set_parallel_for(thread_pf(threads));
    std::vector<std::string> diag;
    if (!sim.start(uw_setup(), &diag)) {
        std::printf("underworld: start failed\n");
        ++g_fail;
        return out;
    }
    sim.set_corruption(2, 3, 1);  // Racketeering by the Underworld: a sabotage target
    sim.set_corruption(3, 3, 1);  // the Death Star II planet: a hack target
    std::vector<GalacticAiTypeInfo> tinfo(uTypes);
    tinfo[uTyber].hero = tinfo[uIG88].hero = true;
    tinfo[uSaboteur].generic_hero = true;
    tinfo[uStruct].categories = {"STRUCTURE"};
    tinfo[uDeathStar].categories = {"CAPITAL"};
    for (const uint32_t h : {uint32_t{uTyber}, uint32_t{uSaboteur}, uint32_t{uIG88}}) tinfo[h].categories = {"INFANTRY"};
    GalacticAiConfig cfg;
    cfg.players = {{1, "SynUnderworld", 1}};
    cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    GalacticAiWorld world(sim, store, &types, &files, rng, defs, eqs, tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, cfg);
    std::vector<std::string> pd;
    CHECK(world.load_plans(plan_files, &pd));
    for (const std::string& d : pd) std::printf("  underworld plans: %s\n", d.c_str());
    CHECK(world.plans().plans.size() == 3);
    CHECK(world.start(&pd));
    // Σ over the budget categories of the Underworld AI (player 1): pools P and available A.
    auto budget_sums = [&world, &defs](double& pool, double& avail) {
        pool = avail = 0.0;
        const AiContext* c = world.context(1);
        if (!c) return;
        for (size_t k = 0; k < defs.categories.size(); ++k) {
            pool += static_cast<double>(c->budget.pool(static_cast<int32_t>(k)));
            avail += static_cast<double>(c->budget.available(static_cast<int32_t>(k)));
        }
    };
    for (int32_t f = 1; f <= frames; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) {
            const bool watch = p == 1 && out.corr_frame < 0;
            const int32_t done_before = watch ? sim.corruptions_completed(1, 6) : 0;
            double pool = 0.0, avail = 0.0;
            const float cr = sim.credits(1);
            if (watch) budget_sums(pool, avail);
            world.update_player(p, f, true, false);
            if (watch && sim.corruptions_completed(1, 6) > done_before) {
                out.corr_frame = f;
                out.corr_credits_before = cr;
                out.corr_credits_after = sim.credits(1);
                out.corr_pool_before = pool;
                out.corr_avail_before = avail;
                budget_sums(out.corr_pool_after, out.corr_avail_after);
            }
        }
        StateHasher h;
        sim.hash_state(h);
        world.hash(h);
        rng.synced().hash(h);
        out.crc.push_back(h.digest());
    }
    for (const AiDecision& d : world.decision_log())
        out.decisions.push_back(std::to_string(d.frame) + " p" + std::to_string(d.player) + " " + d.text);
    out.errors = world.lua_errors();
    out.corruption_p1 = sim.planets()[1].corruption;
    out.completed_bm = sim.corruptions_completed(1, 6);
    out.sabotage_p2 = sim.sabotage_count(2);
    out.purchased = sim.bm_purchased(1).size();
    out.p3_destroyed = sim.planets()[3].destroyed;
    out.ds_locked = sim.type_locked(0, uDeathStar);
    out.ds_alive = false;
    for (const gal::FleetState& fl : sim.fleets())
        if (fl.alive)
            for (const objects::ObjectHandle u : fl.contents) out.ds_alive = out.ds_alive || (store.alive(u) && store.type(u) == uDeathStar);
    out.uw_credits = sim.credits(1);
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "frames %d P1 corruption %d (black-market corruptions %d) P2 sabotages %d purchased %zu P3 destroyed %d "
                  "DS locked %d DS alive %d credits U=%.0f orders %llu",
                  frames, static_cast<int>(out.corruption_p1), out.completed_bm, out.sabotage_p2, out.purchased,
                  out.p3_destroyed ? 1 : 0, out.ds_locked ? 1 : 0, out.ds_alive ? 1 : 0, static_cast<double>(out.uw_credits),
                  static_cast<unsigned long long>(world.orders_issued()));
    out.summary = buf;
    return out;
}

static void test_underworld_special() {
    const int32_t frames = 30 * 600;  // 10 minutes of galactic time
    const UwResult base = run_underworld(0, frames);
    std::printf("underworld (0 workers): %s, %zu decisions, %zu Lua errors\n", base.summary.c_str(), base.decisions.size(),
                base.errors.size());
    for (size_t i = 0; i < base.errors.size() && i < 10; ++i) std::printf("  lua error: %s\n", base.errors[i].c_str());
    for (const std::string& d : base.decisions)
        if (d.find("ability") != std::string::npos) std::printf("  %s\n", d.c_str());
    CHECK(base.errors.empty());
    CHECK(base.corruption_p1 == 6 && base.completed_bm >= 1);  // Corrupt_Planet: corruption_ai_pick on a clean planet
    CHECK(base.sabotage_p2 >= 1);                              // Sabotage_Planet: a structure of the corrupted P2
    CHECK(base.purchased >= 1);                                // Black_Market_Purchase at the Black_Market planet
    CHECK(base.p3_destroyed && base.ds_locked && !base.ds_alive);  // Hack_Super_Weapon through Land_Units
    // a2k t08 (docs/re/A2K_T08_RE_QUESTIONS.md §1): the AI corruption cost is not reserved or charged through the AI
    // budget. Retail HotM Rebel (a2j probe 074145): 800 credits (Intimidation at Naboo) left the Underworld's credits
    // between frames 585 and 616 while its Offensive / Infrastructure pools stayed as they were. Here: the update that
    // applies the Black_Market corruption (500) takes exactly its cost from the credits, and no budget pool or
    // available amount moves in that update.
    std::printf("  corruption apply: frame %d credits %.1f -> %.1f, budget pools %.3f -> %.3f, available %.3f -> %.3f\n",
                base.corr_frame, static_cast<double>(base.corr_credits_before), static_cast<double>(base.corr_credits_after),
                base.corr_pool_before, base.corr_pool_after, base.corr_avail_before, base.corr_avail_after);
    CHECK(base.corr_frame > 0);
    CHECK(base.corr_credits_before - base.corr_credits_after == 500.0f);
    CHECK(base.corr_pool_before == base.corr_pool_after && base.corr_avail_before == base.corr_avail_after);
    for (const unsigned t : {1u, 4u, 15u}) {
        const UwResult r = run_underworld(t, frames);
        size_t first = r.crc.size() == base.crc.size() ? r.crc.size() : 0;
        for (size_t i = 0; i < std::min(r.crc.size(), base.crc.size()); ++i)
            if (r.crc[i] != base.crc[i]) {
                first = i;
                break;
            }
        std::printf("  underworld workers %u vs 0: %s\n", t, first == r.crc.size() ? "identical" : ("DIVERGED at frame " + std::to_string(first + 1)).c_str());
        CHECK(first == r.crc.size() && r.decisions == base.decisions);
    }
    std::ostringstream o;
    o << "# P4 ai synthetic Underworld special systems: per-frame CRC (every 300th frame + last), decisions\n";
    for (size_t i = 0; i < base.crc.size(); ++i)
        if ((i + 1) % 300 == 0 || i + 1 == base.crc.size()) {
            char b[64];
            std::snprintf(b, sizeof b, "frame %zu %016llx\n", i + 1, static_cast<unsigned long long>(base.crc[i]));
            o << b;
        }
    o << base.summary << "\n";
    for (const std::string& d : base.decisions) o << d << "\n";
    const std::string text = o.str();
    const std::string path = std::string(ALAMO_AI_GOLDEN_DIR) + "/ai_underworld_crc.txt";
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
            const std::string actual = std::string(ALAMO_AI_LOG_DIR) + "/ai_underworld_actual.txt";
            std::ofstream(std::filesystem::path(actual), std::ios::binary) << text;
            std::printf("  actual written to %s\n", actual.c_str());
        }
        CHECK(same);
    }
}

// ================================================================ N1 save / load
//
// Two identical rigs (sim + store + RNG + conflicts + AI) run in lockstep. At the save point rig A's AI world is
// written (B's bytes must be equal), A then rejects broken streams without changing, B's AI world is destroyed and
// rebuilt as a loader would (load_plans + start on the same sim) and reads A's bytes. B's sim and store are not
// reloaded: they are A's twin, so only the AI state crosses the save. With Lua state persistence (LuaVm::write /
// read) both rigs then continue in lockstep comparing the per-tick CRC (sim, AI hash, path finders, RNG, conflicts)
// and every order each AI issued; without it (the n1-lua stubs) the Lua-free part is checked and the test reports
// PENDING.

struct OrderRecord {
    uint8_t player = 0;
    commands::EventData ev;
    commands::RejectReason why{};
    friend bool operator==(const OrderRecord&, const OrderRecord&) = default;
};

struct SaveRigInputs {
    const gal::GalacticRules* rules = nullptr;
    const gal::GalacticSetup* setup = nullptr;
    const objects::GameObjectTypes* types = nullptr;
    const FileProvider* files = nullptr;
    const AiDefinitions* defs = nullptr;
    const EquationSet* eqs = nullptr;
    std::vector<GalacticAiTypeInfo> tinfo;
    GalacticAiConfig cfg;
    std::vector<std::string> plan_files;
    uint32_t seed_synced = 1, seed_free = 1;
    bool conflicts = false;
};

class SaveRig {
public:
    explicit SaveRig(const SaveRigInputs& in) : rng(in.seed_synced, in.seed_free), in_(in) {}
    SaveRig(const SaveRig&) = delete;
    SaveRig& operator=(const SaveRig&) = delete;

    bool start() {
        store = std::make_unique<objects::ObjectStore>(objects::ModeKind::Galactic, objects::StoreKind::Primary, *in_.types, &ids_);
        sim = std::make_unique<gal::GalacticSim>(*in_.rules, *store, rng.synced());
        std::vector<std::string> diag;
        if (!sim->start(*in_.setup, &diag)) return false;
        if (in_.conflicts) {
            conflicts = std::make_unique<gal::ConflictManager>(*sim, crules_, rng.synced());
            conflicts->attach();
        }
        return make_world();
    }
    // A new AI world on this rig's sim, as a loader builds it before reading: load_plans + start.
    bool make_world() {
        world.reset();
        world = std::make_unique<GalacticAiWorld>(
            *sim, *store, in_.types, in_.files, rng, *in_.defs, *in_.eqs, in_.tinfo,
            [this](uint8_t p, const commands::EventData& ev) {
                const commands::RejectReason why = sim->execute(p, ev);
                orders.push_back({p, ev, why});
                return why;
            },
            in_.cfg);
        std::vector<std::string> pd;
        return world->load_plans(in_.plan_files, &pd) && world->start(&pd);
    }
    void tick(int32_t frame) {
        orders.clear();
        sim->update();
        const bool busy = conflicts && conflicts->state() == gal::ConflictState::AutoRunning;
        for (int32_t p = 0; p < static_cast<int32_t>(sim->players().size()); ++p) world->update_player(p, frame, true, busy);
    }
    uint64_t crc() const {
        StateHasher h;
        sim->hash_state(h);
        world->hash(h);
        world->hash_paths(h);
        rng.synced().hash(h);
        if (conflicts) conflicts->hash(h);
        return h.digest();
    }
    uint64_t ai_digest() const {
        StateHasher h;
        world->hash(h);
        world->hash_paths(h);
        return h.digest();
    }
    std::vector<uint8_t> save() const {
        ByteWriter w;
        world->write(w);
        return w.bytes();
    }
    bool load(const std::vector<uint8_t>& bytes) {
        ByteReader r(bytes);
        return world->read(r);
    }

    std::unique_ptr<objects::ObjectStore> store;
    GameRandom rng;
    std::unique_ptr<gal::GalacticSim> sim;
    std::unique_ptr<gal::ConflictManager> conflicts;
    std::unique_ptr<GalacticAiWorld> world;
    std::vector<OrderRecord> orders;  // this frame's AI orders, issue order

private:
    const SaveRigInputs& in_;
    objects::IdCounter ids_;
    gal::ConflictRules crules_;
};

// LuaVm::write / read implemented (false while engine/lua still has the N1 stubs).
static bool lua_state_persists() {
    lua::LuaVm a{lua::LuaHost{}};
    lua::LuaVm b{lua::LuaHost{}};
    ByteWriter w;
    a.write(w);
    ByteReader r(w.bytes());
    return b.read(r) && r.done();
}

// Ticks `a` and every rig of `others` together; false at the first frame whose CRC or AI orders differ from a's.
static bool save_lockstep(SaveRig& a, std::initializer_list<SaveRig*> others, int32_t first, int32_t frames, const char* label) {
    for (int32_t f = first; f < first + frames; ++f) {
        a.tick(f);
        const uint64_t ca = a.crc();
        for (SaveRig* o : others) {
            o->tick(f);
            const uint64_t co = o->crc();
            if (ca != co || a.orders != o->orders) {
                std::printf("  %s: rigs differ at frame %d (crc %016llx / %016llx, %zu / %zu orders)\n", label, f,
                            static_cast<unsigned long long>(ca), static_cast<unsigned long long>(co), a.orders.size(),
                            o->orders.size());
                return false;
            }
        }
    }
    return true;
}

template <class T>
static std::vector<uint8_t> save_chunk(const T& x) {
    ByteWriter w;
    x.write(w);
    return w.bytes();
}
template <class T>
static bool load_chunk(T& x, const std::vector<uint8_t>& bytes) {
    ByteReader r(bytes);
    return x.read(r) && r.done();
}

// Broken streams: read() fails and leaves the running world (and the shared RNG) exactly as it was.
static void save_rejects(SaveRig& a, const std::vector<uint8_t>& bytes, int cuts, const char* label) {
    const uint64_t digest = a.ai_digest();
    const GameRandom rng = a.rng;
    const std::vector<uint8_t> before = a.save();
    int accepted = 0;
    auto reject = [&](const std::vector<uint8_t>& d, const char* what) {
        if (a.load(d)) {
            ++accepted;
            std::printf("  %s: %s (%zu bytes) accepted\n", label, what, d.size());
        }
        CHECK(a.ai_digest() == digest && a.rng == rng);
    };
    reject({}, "empty stream");
    std::vector<uint8_t> v = bytes;
    v[0] = static_cast<uint8_t>(v[0] ^ 0xFFu);
    reject(v, "bad version");
    v = bytes;
    v[4] = static_cast<uint8_t>(v[4] ^ 1u);
    reject(v, "AI player count");
    v = bytes;
    v.push_back(0);
    reject(v, "trailing byte");
    reject(std::vector<uint8_t>(bytes.begin(), bytes.end() - 1), "truncated by one byte");
    for (int k = 1; k < cuts; ++k) {
        const auto cut = static_cast<std::ptrdiff_t>(bytes.size() * static_cast<size_t>(k) / static_cast<size_t>(cuts));
        reject(std::vector<uint8_t>(bytes.begin(), bytes.begin() + cut), "truncated stream");
    }
    CHECK(accepted == 0);
    CHECK(a.save() == before);
}

// `full_session`: also load the whole session (store, sim, conflicts, AI) into a third, fresh rig C. Needs object types
// from a GameObjectTypes database (ObjectStore::read rejects type indices it does not know).
// `max_bytes` / `max_packed` (0: unchecked): size guards (N1 save size) on the AI save, as written and as a save file
// packs it (lz_compress).
static void check_save_load(const SaveRigInputs& in, const char* label, int32_t warm, int32_t after, int cuts, bool full_session,
                            size_t max_bytes = 0, size_t max_packed = 0) {
    SaveRig a(in), b(in), c(in);
    const bool started = a.start() && b.start() && (!full_session || c.start());
    CHECK(started);
    if (!started) return;
    // Save at the first frame from `warm` on with running plans (task forces, statuses, build tasks live).
    int32_t frame = warm;
    bool twins = save_lockstep(a, {&b}, 1, warm, label);
    while (twins && a.world->running_plans() == 0 && frame < warm + 30 * 600) twins = save_lockstep(a, {&b}, ++frame, 1, label);
    CHECK(twins);  // the rigs are deterministic twins
    if (!twins) return;
    const bool lua = lua_state_persists();
    const std::vector<uint8_t> bytes = a.save();
    if (bytes != b.save()) {
        // Twin worlds must save the same bytes; tell the AI's own sections from the Lua VM images.
        a.world->set_save_lua_state(false);
        b.world->set_save_lua_state(false);
        const bool core = a.save() == b.save();
        a.world->set_save_lua_state(true);
        b.world->set_save_lua_state(true);
        CHECK(core && lua);
        std::printf("  %s: twin saves differ %s\n", label,
                    core ? "only inside the Lua VM images (LuaVm::write of twin VMs not byte-identical)" : "in the AI sections");
    }
    const uint64_t digest = a.ai_digest();
    const size_t packed = lz_compress(bytes).size();
    std::printf("N1 save %s: frame %d, %zu bytes (%zu packed), %zu running plans, %llu orders so far\n", label, frame,
                bytes.size(), packed, a.world->running_plans(), static_cast<unsigned long long>(a.world->orders_issued()));
    CHECK(a.world->running_plans() > 0);
    if (max_bytes && bytes.size() > max_bytes) std::printf("  %s: AI save larger than its %zu-byte guard\n", label, max_bytes);
    if (max_packed && packed > max_packed) std::printf("  %s: packed AI save larger than its %zu-byte guard\n", label, max_packed);
    CHECK(!max_bytes || bytes.size() <= max_bytes);
    CHECK(!max_packed || packed <= max_packed);
    // The rest of the session at the save point (n1-objects / n1-galactic), for the full load into rig C.
    const std::vector<uint8_t> s_rng = save_chunk(a.rng), s_store = save_chunk(*a.store), s_sim = save_chunk(*a.sim);
    const std::vector<uint8_t> s_conflicts = a.conflicts ? save_chunk(*a.conflicts) : std::vector<uint8_t>{};

    save_rejects(a, bytes, cuts, label);
    a.world->set_save_lua_state(false);  // the same with the deeper sections reached (fresh VMs staged, then dropped)
    save_rejects(a, bytes, cuts, label);
    a.world->set_save_lua_state(true);
    CHECK(a.ai_digest() == digest && a.crc() == b.crc());

    // B: the loader's rebuild of the AI world on B's (twin) sim. B's RNG as it was: building a world keeps no draw.
    const GameRandom rng_before = b.rng;
    const bool rebuilt = b.make_world();
    CHECK(rebuilt);
    if (!rebuilt) return;
    b.rng = rng_before;
    // C: a whole session loaded into a fresh rig (started from the setup, never ticked): RNG, objects, galactic sim,
    // conflicts, then the AI (objects resolve by id in the restored store).
    const char* failed = !full_session                                           ? nullptr
                         : !load_chunk(c.rng, s_rng)                               ? "RNG"
                         : !load_chunk(*c.store, s_store)                         ? "ObjectStore"
                         : !load_chunk(*c.sim, s_sim)                             ? "GalacticSim"
                         : c.conflicts && !load_chunk(*c.conflicts, s_conflicts) ? "ConflictManager"
                                                                                  : nullptr;
    const bool session = failed == nullptr;
    if (failed) std::printf("  %s: full session: %s read failed\n", label, failed);
    CHECK(session);
    if (!lua) {
        b.world->set_save_lua_state(false);
        if (full_session) c.world->set_save_lua_state(false);
    }
    const bool loaded_b = b.load(bytes), loaded_c = !full_session || (session && c.load(bytes));
    CHECK(loaded_b && loaded_c);
    if (!loaded_b || !loaded_c) {
        std::printf("  %s: read() failed (rebuilt world %s, full session %s)\n", label, loaded_b ? "ok" : "FAILED",
                    !session ? "not loaded" : loaded_c ? "ok" : "FAILED");
        return;
    }
    // Without Lua state (stubs) A's images are empty too, so the byte comparisons hold in both cases.
    const bool hashes = b.ai_digest() == digest && b.crc() == a.crc() &&
                        (!full_session || (c.ai_digest() == digest && c.crc() == a.crc()));
    const bool resave = b.save() == bytes && (!full_session || c.save() == bytes);  // save -> load -> save
    CHECK(hashes && resave);
    if (!lua) {
        std::printf("N1 save %s: PENDING n1-lua (LuaVm::write / read are stubs): rebuilt world%s loaded without the Lua VM "
                    "images, AI hash + paths + CRC %s, re-save %s; lockstep continuation not run\n", label,
                    full_session ? " + full session" : "", hashes ? "identical" : "DIFFER", resave ? "byte-identical" : "DIFFERS");
        return;
    }
    const size_t mark = a.world->decision_log().size();
    const bool same = full_session ? save_lockstep(a, {&b, &c}, frame + 1, after, label)
                                   : save_lockstep(a, {&b}, frame + 1, after, label);
    CHECK(same);
    const auto& la = a.world->decision_log();
    bool logs = true;
    for (const SaveRig* r : {&b, &c}) {
        if (r == &c && !full_session) continue;
        const auto& lr = r->world->decision_log();
        logs = logs && la.size() - mark == lr.size();
        for (size_t i = 0; logs && i < lr.size(); ++i)
            logs = la[mark + i].frame == lr[i].frame && la[mark + i].player == lr[i].player && la[mark + i].text == lr[i].text;
    }
    CHECK(logs);
    std::printf("N1 save %s: loaded into a rebuilt world%s, %d lockstep frames %s (per-tick CRC + orders), %zu decisions "
                "after the save %s, final crc %016llx\n", label, full_session ? " and a fully loaded session" : "", after,
                same ? "identical" : "DIFFER",
                la.size() - mark, logs ? "identical" : "DIFFER", static_cast<unsigned long long>(a.crc()));
}

static void test_save_synthetic() {
    MemoryFiles files;
    AiDefinitions defs;
    EquationSet eqs;
    SaveRigInputs in;
    syn_data(files, defs, eqs, in.plan_files);
    const gal::GalacticRules rules = syn_rules();
    const gal::GalacticSetup setup = syn_setup();
    objects::GameObjectTypes types;
    in.rules = &rules;
    in.setup = &setup;
    in.types = &types;
    in.files = &files;
    in.defs = &defs;
    in.eqs = &eqs;
    in.tinfo.resize(tTypes);
    in.tinfo[tFrigate].categories = {"FRIGATE"};
    in.tinfo[tCorvette].categories = {"CORVETTE"};
    in.tinfo[tInfantry].categories = {"INFANTRY"};
    in.tinfo[tSB1].categories = in.tinfo[tSB2].categories = {"STRUCTURE"};
    in.cfg.players = {{0, "SynEmpire", 1}, {1, "SynRebel", 1}};
    in.cfg.difficulties = {{"Normal_Default", 1.1f, 0.0f}};
    in.seed_synced = 4242u;
    in.seed_free = 77u;
    in.conflicts = true;
    check_save_load(in, "synthetic", 30 * 90, 30 * 240, 48, false);
}

// ================================================================ FoC data

#ifdef ALAMO_AI_HAVE_VFS
static std::string env(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

static void test_foc_data() {
    const std::string root = env("ALAMO_FOC_DATA");
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(path_from_utf8(root) / "Data", ec)) {
        std::printf("FoC data: ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v) {
        std::printf("FoC data: VFS failed (%s): skipped\n", err.c_str());
        return;
    }
    AiDefinitions defs;
    const AiDataPaths paths = list_ai_data([&v](std::string_view g) { return v->list(g); });
    CHECK(load_ai_definitions(*v, paths, defs));
    std::printf("FoC AI data: %zu categories, %zu goal types, %zu function sets, %zu player types, %zu templates, %zu diagnostics\n",
                defs.categories.size(), defs.goals.size(), defs.sets.size(), defs.players.size(), defs.templates.size(),
                defs.diagnostics.size());
    for (size_t i = 0; i < defs.diagnostics.size() && i < 12; ++i) std::printf("  %s\n", defs.diagnostics[i].c_str());
    CHECK(defs.goals.size() > 100 && defs.find_player("BasicEmpire") && defs.find_template("Basic_Empire_Default"));
    const uint32_t cq = defs.find_goal("Conquer_Opponent");
    CHECK(cq != kAiNone && defs.goals[cq].time_limit == 240.0f && defs.goals[cq].category == defs.find_category("Offensive"));

    // Every shipped plan script runs Base_Definitions on the Lua bridge and yields a definition.
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    gal::GalacticRules rules;
    std::vector<std::string> diag;
    if (!db.load(*v, {}, &diag) || !raw.load(*v, &diag) || !types.build(raw, v.get(), &diag) ||
        !gal::load_galactic_rules(*v, db, types, rules, &diag)) {
        std::printf("FoC data: game database incomplete: skipped\n");
        return;
    }
    EquationSet eqs;
    load_equation_files(*v, v->list(kPerceptionEquationGlob), eqs);
    const gal::GalacticSetup setup = gal::build_campaign_setup(db, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Galactic, objects::StoreKind::Primary, types, &ids);
    GameRandom rng(99u, 1u);
    gal::GalacticSim sim(rules, store, rng.synced());
    CHECK(sim.start(setup, &diag));
    GalacticAiConfig gcfg;
    for (size_t p = 0; p < sim.players().size(); ++p)
        if (sim.players()[p].ai) {
            const std::string fac = rules.factions[sim.players()[p].faction].name;
            gcfg.players.push_back({static_cast<int32_t>(p),
                                    ai_iequal(fac, "Empire") ? "BasicEmpire" : (ai_iequal(fac, "Rebel") ? "BasicRebel" : "AI_Player_Underworld"), 1});
        }
    gcfg.difficulties = load_ai_difficulties(*v);
    // A2 t03: the starbases' Starting_Spawned_Units_Tech_0 lists (type, count; repeated tags appended) reach the AI type
    // info, so the typed force rows hold squadrons instead of the starbase's own AI_Combat_Power.
    const std::vector<GalacticAiTypeInfo> foc_tinfo = make_galactic_ai_type_info(types);
    {
        const uint32_t sb1 = rules.find_type("Rebel_Star_Base_1"), xw = rules.find_type("Rebel_X-Wing_Squadron"), yw = rules.find_type("Y-Wing_Squadron");
        CHECK(sb1 != gal::kNoIndex && sb1 < foc_tinfo.size() && xw != gal::kNoIndex && yw != gal::kNoIndex);
        if (sb1 < foc_tinfo.size()) {
            const auto& l = foc_tinfo[sb1].spawn_tech[0];
            CHECK(l.size() == 2 && l[0].type == xw && l[0].count == 2 && l[1].type == yw && l[1].count == 2);
            CHECK(foc_tinfo[sb1].spawn_tech[1].empty() && rules.types[sb1].ai_combat_power == 5000.0f);
        }
    }
    GalacticAiWorld world(sim, store, &types, v.get(), rng, defs, eqs, foc_tinfo,
                          [&sim](uint8_t p, const commands::EventData& ev) { return sim.execute(p, ev); }, gcfg);
    std::vector<std::string> pd;
    const std::vector<std::string> plan_files = list_plan_files([&v](std::string_view g) { return v->list(g); });
    CHECK(world.load_plans(plan_files, &pd));
    std::printf("FoC plans: %zu files, %zu definitions loaded, %zu diagnostics\n", plan_files.size(), world.plans().plans.size(), pd.size());
    for (size_t i = 0; i < pd.size() && i < 15; ++i) std::printf("  %s\n", pd[i].c_str());
    CHECK(world.plans().find("ConquerOpponentPlan") != kAiNone && world.plans().find("BuildSpaceForcesPlan") != kAiNone);
    const uint32_t cop = world.plans().find("ConquerOpponentPlan");
    if (cop != kAiNone) {
        const PlanDefinition& d = world.plans().plans[cop];
        CHECK(d.task_forces.size() == 2 && d.task_forces[0].name == "SpaceForce" && d.task_forces[0].minimum_total_size == 4 &&
              d.min_contrast_scale == 1.25f && d.per_failure_contrast_adjust == 0.5f && d.required_categories.size() == 2);
    }
    // Token coverage of the galactic goal / budget / template equations.
    CHECK(world.start(&pd));
    for (int32_t f = 1; f <= 30 * 120; ++f) {
        sim.update();
        for (int32_t p = 0; p < static_cast<int32_t>(sim.players().size()); ++p) world.update_player(p, f, true, false);
    }
    // Which goal functions evaluate on this galaxy (per AI player, every planet / global).
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const AiContext* c = world.context(static_cast<int32_t>(p));
        if (!c) continue;
        uint32_t ok = 0, bad = 0, positive = 0;
        std::map<std::string, std::string> failing;
        for (const GoalSource& s : c->goals.sources()) {
            const uint32_t e = eqs.find(s.function);
            const bool global = defs.goals[s.goal_type].global();
            for (int32_t t = global ? -1 : 0; t < (global ? 0 : static_cast<int32_t>(sim.planets().size())); ++t) {
                EvalContext ctx;
                ctx.self = static_cast<int32_t>(p);
                ctx.target = ctx.location = t;
                ctx.enemy = c->opponent;
                ctx.human = c->opponent;
                SimRng scratch(1);
                ctx.rng = &scratch;
                const EvalResult r = e == kUnresolved ? EvalResult{} : evaluate(eqs, e, ctx, world.resolver());
                if (r.ok) {
                    ++ok;
                    positive += r.value > 0.0 ? 1u : 0u;
                } else {
                    ++bad;
                    failing[s.function] = e == kUnresolved ? "missing" : eval_error_name(r.error);
                }
            }
        }
        std::printf("FoC player %zu goal functions: %u evaluations ok (%u > 0), %u failed:", p, ok, positive, bad);
        for (const auto& [n, why] : failing) std::printf(" %s(%s)", n.c_str(), why.c_str());
        std::printf("\n");
    }
    size_t active = 0;
    for (size_t p = 0; p < sim.players().size(); ++p)
        if (const AiContext* c = world.context(static_cast<int32_t>(p)))
            for (const auto& cat : c->goals.active()) active += cat.size();
    std::printf("FoC HotM 120 s: %zu decisions, %zu running plans, %zu active goals, orders %llu (rejected %llu), %zu Lua errors\n",
                world.decision_log().size(), world.running_plans(), active, static_cast<unsigned long long>(world.orders_issued()),
                static_cast<unsigned long long>(world.orders_rejected()), world.lua_errors().size());
    for (const auto& [k, n] : world.proposal_stats()) std::printf("  proposals: %s x%u\n", k.c_str(), n);
    const std::vector<std::string> le = world.lua_errors();
    for (size_t i = 0; i < le.size() && i < 10; ++i) std::printf("  lua: %s\n", le[i].c_str());
    for (size_t i = 0; i < world.decision_log().size() && i < 25; ++i)
        std::printf("  %lld p%d %s\n", static_cast<long long>(world.decision_log()[i].frame), world.decision_log()[i].player,
                    world.decision_log()[i].text.c_str());
    CHECK(world.running_plans() + active > 0);
    // AI execution path finder on the real galaxy: every AI player's finder exists and is on the 90-frame cadence;
    // its Get_Path answers vs the former Lua Find_Path stand-in (fleet A*, all request flags) for every planet pair.
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const auto pi = static_cast<int32_t>(p);
        const AiPathFinder* f = world.path_finder(pi);
        if (!f) continue;
        CHECK(f->created() && f->recomputes() >= 3600u / 90u - 1u);
        uint32_t pairs = 0, reachable = 0, astar_reachable = 0, same = 0;
        gal::PlayerView view = sim.player_view(pi);
        if (view.is_human) view.revealed = gal::Bitset{};
        gal::PathScratch scratch;
        std::vector<PlanetIndex> ap;
        for (PlanetIndex a = 0; a < sim.planets().size(); ++a)
            for (PlanetIndex b = 0; b < sim.planets().size(); ++b) {
                if (a == b || !sim.planets()[a].is_planet || !sim.planets()[b].is_planet) continue;
                ++pairs;
                const std::vector<PlanetIndex> fw = world.path(pi, a, b);
                gal::PathRequest req;
                req.source = a;
                req.destination = b;
                const bool ok = gal::find_fleet_path(sim.graph(), view, req, scratch, ap);
                reachable += fw.empty() ? 0u : 1u;
                astar_reachable += ok ? 1u : 0u;
                same += (ok ? ap : std::vector<PlanetIndex>{}) == fw ? 1u : 0u;
                if (!fw.empty()) CHECK(fw.front() == a && fw.back() == b);
            }
        std::printf("FoC player %zu AI path finder: %u recomputes, %u pairs, %u reachable (A* stand-in %u), %u identical to A*\n",
                    p, f->recomputes(), pairs, reachable, astar_reachable, same);
        CHECK(reachable > 0);
    }
    // FIXPASS_GALACTIC §1 on the real galaxy (shipped GalacticFreeStore.lua drives the moves):
    //   * the Friendly_Only source list holds only planets held by the player or an ally (§1.4);
    //   * the free-store Reachable_Target calls return such a planet or nil;
    //   * Get_Next_Hop passes a pair exactly when every planet after the start has node cost 0 on the matrix.
    for (size_t p = 0; p < sim.players().size(); ++p) {
        const auto pi = static_cast<int32_t>(p);
        const AiPathFinder* f = world.path_finder(pi);
        if (!f) continue;
        const gal::PlayerView view = sim.player_view(pi);
        const std::vector<PlanetIndex> r0 = world.reach_planets(pi, 0);
        for (const PlanetIndex k : r0) CHECK(gal::PlayerView::bit(view.held_by_ally, k, false));
        CHECK(world.reach_planets(pi, 5).size() >= r0.size());
        for (const char* fn : {"Space_Priority_Defense_Score", "Low_Space_Defense_Score", "Ground_Priority_Defense_Score", "One"}) {
            const int32_t t = world.reachable_target(pi, fn, "Friendly", "Friendly_Only", 0.1);
            CHECK(t == -1 || std::find(r0.begin(), r0.end(), static_cast<PlanetIndex>(t)) != r0.end());
        }
        uint32_t pairs = 0, pass = 0, blocked = 0;
        std::vector<PlanetIndex> path;
        std::vector<uint32_t> w;
        for (PlanetIndex a = 0; a < sim.planets().size(); ++a)
            for (PlanetIndex b = 0; b < sim.planets().size(); ++b) {
                if (a == b || !sim.planets()[a].is_planet || !sim.planets()[b].is_planet) continue;
                ++pairs;
                const FreeStoreHopResult r = free_store_next_hop(*f, pi, a, b, false, {});
                f->get_path(a, b, path, &w);
                bool free_nodes = !path.empty();
                for (size_t k = 0; k + 1 < path.size(); ++k) free_nodes = free_nodes && (w[k] >> 16) == 0u;
                CHECK((r.kind == FreeStoreHop::Issue) == (free_nodes && path.size() >= 2));
                if (r.kind == FreeStoreHop::Issue) {
                    ++pass;
                    CHECK(r.hop == path[1]);
                } else {
                    ++blocked;
                }
            }
        size_t no_path = 0;
        for (const AiDecision& d : world.decision_log())
            if (d.player == pi && d.text.find("Unable to find a path") != std::string::npos) ++no_path;
        std::printf("FoC player %zu free store: Friendly_Only reach %zu planets, Get_Next_Hop %u of %u pairs pass (%u fail), "
                    "%zu moves ended without a path\n", p, r0.size(), pass, pairs, blocked, no_path);
        CHECK((!r0.empty() || sim.planets_owned(pi) == 0) && pass > 0);
    }
}

// N1 save / load on the shipped scripts (HotM, as test_foc_data).
static void test_save_foc() {
    const std::string root = env("ALAMO_FOC_DATA");
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(path_from_utf8(root) / "Data", ec)) {
        std::printf("N1 save FoC: ALAMO_FOC_DATA not set: skipped\n");
        return;
    }
    vfs::VfsConfig vcfg;
    vcfg.foc_root = root;
    std::string err;
    auto v = vfs::Vfs::create(vcfg, &err);
    if (!v) {
        std::printf("N1 save FoC: VFS failed (%s): skipped\n", err.c_str());
        return;
    }
    AiDefinitions defs;
    CHECK(load_ai_definitions(*v, list_ai_data([&v](std::string_view g) { return v->list(g); }), defs));
    data::GameDatabase db;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    gal::GalacticRules rules;
    std::vector<std::string> diag;
    if (!db.load(*v, {}, &diag) || !raw.load(*v, &diag) || !types.build(raw, v.get(), &diag) ||
        !gal::load_galactic_rules(*v, db, types, rules, &diag)) {
        std::printf("N1 save FoC: game database incomplete: skipped\n");
        return;
    }
    EquationSet eqs;
    load_equation_files(*v, v->list(kPerceptionEquationGlob), eqs);
    const gal::GalacticSetup setup = gal::build_campaign_setup(db, rules, "Sandbox_Heart_Of_The_Maw_Underworld", {}, &diag);
    SaveRigInputs in;
    in.rules = &rules;
    in.setup = &setup;
    in.types = &types;
    in.files = v.get();
    in.defs = &defs;
    in.eqs = &eqs;
    in.tinfo = make_galactic_ai_type_info(types);
    for (size_t p = 0; p < setup.players.size(); ++p)
        if (setup.players[p].ai) {
            const std::string fac = rules.factions[setup.players[p].faction].name;
            in.cfg.players.push_back({static_cast<int32_t>(p),
                                      ai_iequal(fac, "Empire") ? "BasicEmpire" : (ai_iequal(fac, "Rebel") ? "BasicRebel" : "AI_Player_Underworld"), 1});
        }
    in.cfg.difficulties = load_ai_difficulties(*v);
    in.plan_files = list_plan_files([&v](std::string_view g) { return v->list(g); });
    in.seed_synced = 99u;
    in.seed_free = 1u;
    // Size guards (N1 save size), about twice the 2026-10-29 sizes: 1.72 MB, 141 KB packed. Before pristine pooled VMs
    // were saved by reference the AI chunk held every pooled VM's image (15.3 MB in the headless HotM save at 1 min).
    check_save_load(in, "FoC HotM", 30 * 120, 30 * 60, 12, true, 4000000, 300000);
}
#endif

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep the progress lines when a test crashes
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--write-golden") == 0) g_write_golden = true;
    test_cadence();
    test_desire_and_hysteresis();
    test_goal_reservations();
    test_goal_batched_values();
    test_goal_batched_accept();  // OPT2 t10
    test_plan_pick_and_solver();
    test_definitions_and_templates();
    test_plan_load_parallel();
    test_synthetic_campaign();
    test_path_finder_world();
    test_sim_hooks();
    test_starbase_force_split();
    test_force_members();
    test_parity_rules();
    test_free_store_supply();
    test_production_disabled_and_delivery();
    test_ground_structure_slots();
    test_budget_fraction_before_init();
    test_production_unbuildable();
    test_goal_time_drop();
    test_goal_allowed_time_base();  // A2f t09
    test_btime_none_speed_factor();    // a2g t03
    test_goal_nominal_time_resolve();  // a2g t05 (a)
    test_goal_resolve_time_ok();       // a2g t05 (b)
    test_active_goals_category();      // a2g F1, a2h t04
    test_maintenance_tokens();         // a2h t03
    test_force_maintenance_tokens();   // a2i t04
    test_budget_fraction_to_build();   // a2h t07
    test_budget_fraction_in_progress();  // a2i t06
    test_budget_reset_on_template_switch();  // a2g F2
    test_free_store_path_rules();
    test_free_store_world();
    test_underworld_special();
    test_save_synthetic();
#ifdef ALAMO_AI_HAVE_VFS
    test_foc_data();
    test_save_foc();
#endif
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}

// AI framework: the goal system - proposal, commit and Resolve_Category (docs/re/AI_GALACTIC.md §1.3-1.6).
// Mode-agnostic: everything that depends on the mode (targets, Applies, the reachability block, desire
// evaluation, Find_Plan_For_Goal + PotentialPlan, plans, budget) goes through GoalModeHooks.
//
// SERVICE (every frame, 0x6cacd0) [H]: countdown > 0 -> decrement and return; else one proposal slice of
// `quota` evaluations; when the pass completes: budget weights (hook), Commit, then
//   quota = clamp(min(ceil(evalsLastPass / (FPS*5)), (int)(20 / max(1, players - activeHumans) + 0.5)), >= 1)
// and countdown = the difficulty's Goal_Cycle_Sleep frames.
// First pass: GoalSystemConfig::retail_cadence (galactic, a2m r2) seeds it from sources x targets; otherwise [port] the
// quota of the very first pass is the cap term.
//
// PROPOSAL CURSOR (0x6cb120, §1.5): sources = the player type's (goal type, function) pairs in
// GoalProposalFunctionSets order, filtered by the context's mode. For each proposable source (block list
// empty, template switch > 0, exclusion counter 0): Global types once with no target, others over every
// target (index order) that Applies; candidates skipped by slot 9 (already active, or blocked) do not
// count. Each counted candidate evaluates
//   desire = Evaluate(function){double} + (double)((float)c1*w1 + (float)c2*w2)
// (c1/c2 = recent failed goals / activations, Learning). desire > 0 -> slot 10 (hook accept: the plan pick
// = one synced draw, PotentialPlan, time/budget test) -> insert (duplicates keep their position and take the
// higher desire). Desire / priority stored as (float), clamped to [0, 1e18]. A failed evaluation counts as
// an evaluation with no proposal [port].
//
// BATCHED VALUES (OPT2 t03) [port, same results]: a mode whose candidate_pf() is set walks the slice first (the
// walk reads only the proposable / skip / applies state, which evaluations, slot 10 and insert do not change),
// then evaluates the candidates whose function prepare_evaluation() accepts on that ParallelFor (const, one output
// per candidate), then runs every candidate's Learning terms, slot 10 and insert in visit order. A rejected
// function (Script_ evaluators: Lua and synced draws) is evaluated in place in that serial loop, as before.
//
// SLICE BRACKET (OPT2 t10) [port, same results]: slice_begin() / slice_end() enclose each proposal slice (every slot 10
// of the slice runs in between). The tactical AI uses it to ask the host once per slice for what its solves read.
//
// BATCHED ACCEPT (OPT2 t10) [port, moves results]: a mode whose accept_batched() is true splits slot 10. In visit order,
// accept_prepare() runs the part before the solve (the tactical plan pick and the solve's inputs, drawing from the
// candidate's own RNG stream, not the synced one) and the candidate becomes PENDING (solve i of the slice, i = visit
// order among the prepared). After the slice, accept_start() runs the slice's solves (any thread, any order) and
// accept_finish() + the budget test + insert run for every pending goal in visit order on frame now + accept_latency():
// in the slice itself for latency 0, else at the start of that frame's service, before its next slice. No new slice
// starts while solves are pending, so slices keep one per frame at latency <= 1; a pass whose last slice left solves
// pending commits when they are applied. block_proposals() drops the pending goals (accept_cancel()). Pending goals and
// their ready frame are hashed and saved only while pending (latency 0 hashes and saves nothing new).
//
// COMMIT (0x6c8710) and RESOLVE (0x6c8970): exactly §1.6, with the hysteresis
//   eff(d) = state > 0 && 0.01 < d < 40 ? d + (1 - (d - 0.01) / 39.99) * 1.6 : d.
// [port] Every sort is std::stable_sort descending (the original's std::sort is insertion sort, i.e.
// stable, up to 32 elements and unstable above; ties keep insertion order here - a documented deviation
// that is only lockstep-safe because every peer runs this code). Category order: stable descending by
// normalised budget weight [M]. The per-category extra-goal count (AIPlayerType GoalSetExtensionSize, +0xE8 of the
// player type, not of the template) is GoalSystemConfig::extra_goals_per_category.
//
// FREE-STORE RESERVATIONS (entry +0x30, A1_READINGS §28.6) [H]: slot 10's solve (accept) reserves nothing and ignores
// reservations; Commit step 4 suspends every active goal's reservations (suspend_reservations, 0x6c0be0) before any
// Resolve_Category; in Resolve_Category a kept goal is checked (plan_invalid: another PP took one of its units) and
// re-reserves (refresh_plan, 0x6c2d60), a new proposal re-solves honouring reservations (plan_activatable, 0x6c4a50)
// and reserves at accept (refresh_plan); maintenance re-reserves every surviving goal.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_learning.hpp"
#include "alamo/ai/ai_templates.hpp"
#include "alamo/core/parallel.hpp"

namespace alamo {
class ByteReader;
class ByteWriter;
class SimRng;
class StateHasher;
}  // namespace alamo

namespace alamo::ai {

struct GoalSource {
    uint32_t goal_type = kAiNone;
    std::string function;  // perception equation name
};

// InstantiatedGoal (+0x48..+0x90).
struct Goal {
    uint32_t serial = 0;            // port: creation serial (stable id, never reused)
    uint32_t type = kAiNone;
    uint32_t source = kAiNone;      // GoalSource index that proposed it
    int32_t target = -1;            // -1 = global
    GoalState state = GoalState::Proposed;
    float desire = 0.0f;            // +0x50
    float priority = 0.0f;          // +0x54
    int64_t created = 0;            // +0x70 (frame)
    // PotentialPlan results (+0x68, +0x78, +0x7c).
    uint32_t plan = kAiNone;        // chosen plan definition
    bool pp_valid = false;
    float cost = 0.0f;              // +0x78
    float eta = 0.0f;               // +0x7c seconds
    // A2f t09: 0x6c0450(PP), the AllowedTime base (GoalSystemConfig::nominal_allowed_time): sum of btime(T, none)
    // over the PP's selection x the plan player's Galactic_ (A2f t09) / Land_ / Space_ (A2g t06) Build_Time_Multiplier. Set with eta; refreshed when the PP is
    // re-solved (plan_activatable). 0 without a PP.
    float nominal_time = 0.0f;
    int32_t pp_handle = -1;         // mode-owned PotentialPlan record
    bool production_queued = false; // commit step 4 done for this PP
    int32_t running = -1;           // running plan instance (mode-owned), -1 = none
    bool budgeted = false;          // allocated in the budget
    float allocated = 0.0f;
};

class GoalModeHooks {
public:
    virtual ~GoalModeHooks() = default;
    virtual uint32_t target_count() = 0;
    virtual bool applies(int32_t target, uint32_t application_flags) = 0;  // target slot 4
    virtual bool blocked(uint32_t goal_type, int32_t target) = 0;          // slot 9 reachability lookup
    // Perception for (self, opponent, human, target); `function` names an equation.
    virtual bool evaluate(const std::string& function, int32_t target, double& out) = 0;
    // Slot 10 (galactic 0x6b8660): Find_Plan_For_Goal + PotentialPlan + time/budget gross test. Fills
    // g.plan / g.pp_* / g.cost / g.eta (galactic: g.nominal_time). False rejects the candidate.
    virtual bool accept(Goal& g) = 0;
    // 0x6c4a50: a new proposal at accept; re-solves its PotentialPlan honouring the reservations (§28.6 step 3). The
    // galactic mode refreshes g.nominal_time from the re-solved selection (AllowedTime reads the PP live, A2f t09).
    virtual bool plan_activatable(Goal& g) = 0;
    virtual bool plan_invalid(const Goal& g) = 0;      // 0x6c47c0 (-> 0x6c45e0: a unit taken by another PP)
    virtual bool replaceable(const Goal& g) = 0;       // 0x692740
    virtual void refresh_plan(Goal& g) = 0;            // 0x6c2d60: reserve / re-assert (+0x30 = PP)
    // 0x6c0be0's reservation part, Commit step 4, every active goal with a PotentialPlan, every Commit: its free-store
    // reservations are cleared until refresh_plan re-asserts them (§28.6 step 2).
    virtual void suspend_reservations(Goal& g) = 0;
    virtual bool uses_budget() = 0;                    // galactic context
    virtual bool can_fund(const Goal& g) = 0;          // 0x6129c0 (true when no budget)
    // Accept-time gross test 0x6139f0 (slot 10 only; A2M_R3_RE_A §1.1): P / I instead of A / R. Default: can_fund.
    virtual bool can_fund_accept(const Goal& g) { return can_fund(g); }
    virtual float category_credits(int32_t category) = 0;  // cat+0x44 (positive when no budget)
    virtual void budget_add(Goal& g) = 0;
    virtual void budget_release(Goal& g) = 0;
    virtual void budget_weights() = 0;                 // 0x614590
    virtual std::vector<int32_t> category_order() = 0; // categories by normalised budget weight
    virtual void queue_production(Goal& g) = 0;        // 0x6c0be0
    virtual void instantiate(Goal& g) = 0;             // Planning 0x6bad60 (sets g.running)
    virtual void end_plan(Goal& g, bool released_by_goal_system) = 0;  // 0x5f6a50 when a plan runs
    virtual void drop_potential_plan(Goal& g) = 0;     // proposals discarded / goal released
    virtual float elapsed_eta(const Goal& g) = 0;      // galactic: ETA estimate + elapsed (0x525200)
    virtual int32_t quota_players() = 0;               // players - active humans
    // OPT2 t03, batched values (header comment). Default: none, every candidate is evaluated in place.
    virtual const ParallelFor* candidate_pf() { return nullptr; }
    // Serial, before the batch. True: `function` may be evaluated by evaluate_prepared() on any thread.
    virtual bool prepare_evaluation(const std::string& function) {
        (void)function;
        return false;
    }
    // Concurrent const evaluation of a prepared function (the same value evaluate() gives at this point of the slice).
    virtual bool evaluate_prepared(const std::string& function, int32_t target, double& out) const {
        (void)function, (void)target, (void)out;
        return false;
    }
    // Serial, in visit order, once per prepared candidate: the bookkeeping evaluate() does for itself.
    virtual void evaluated(const std::string& function) { (void)function; }
    // OPT2 t10: brackets each proposal slice (every slot 10 of the slice runs in between), so a mode may share host
    // answers between the slice's solves. Default: nothing.
    virtual void slice_begin() {}
    virtual void slice_end() {}
    // OPT2 t10, batched accept (header comment). Default: off, slot 10 runs accept() in place.
    virtual bool accept_batched() { return false; }
    virtual uint32_t accept_latency() { return 0; }  // frames from a slice to its accept_finish() calls
    // Serial, visit order: slot 10 up to its solve, which becomes solve `i` of the slice. False rejects g.
    virtual bool accept_prepare(Goal& g, uint32_t i) {
        (void)g, (void)i;
        return false;
    }
    // Serial, once per slice with prepared solves: starts solves 0..n-1; their results are read on frame `ready`.
    virtual void accept_start(uint32_t n, int64_t now, int64_t ready) { (void)n, (void)now, (void)ready; }
    // Serial, visit order, on the ready frame: the rest of slot 10 for solve i (fills g.plan / g.pp_*). False rejects g.
    virtual bool accept_finish(Goal& g, uint32_t i) {
        (void)g, (void)i;
        return false;
    }
    // The started solves will not be finished (block_proposals() while they were pending).
    virtual void accept_cancel() {}
};

struct GoalSystemConfig {
    int32_t sleep_frames = 0;             // Goal_Cycle_Sleep_Duration x FPS (slot 11)
    // AIPlayerType GoalSetExtensionSize (+0xE8, default 2) [H]. The galactic AI passes its player type's value; the
    // tactical AI still uses this default of 1 (its goldens move with 2; follow-up for the tactical owner).
    int32_t extra_goals_per_category = 1;
    // A2f t09 (A2F_RE_READS §1.4 item 2) [H]: AllowedTime (0x692860) with a PP = Goal::nominal_time x
    // Build_Time_Delay_Tolerance, and nominal_time is hashed. The galactic AI (A2f t09) and the tactical AI (A2g t06,
    // Land_ / Space_Build_Time_Multiplier) set it; off keeps the PP ETA x tolerance base.
    bool nominal_allowed_time = false;
    // a2m r2 (A2M_R2_RE_A §2.1) [H], set by the galactic AI: the goal-system Init 0x6ca1f0 seeds evalsLastPass with
    // sources x max(1, targets), the first quota comes from that seed, and the seed is not cleared, so pass 1's
    // evaluations add to it; a slice ends as soon as its counter reaches the quota (a pass of E evaluations at quota q
    // takes floor(E / q) + 1 frames). Off (the tactical AI, not re-checked): first quota = the cap term, a slice scans on
    // to the next counted candidate.
    bool retail_cadence = false;
};

class GoalSystem {
public:
    void init(const AiDefinitions& defs, std::vector<GoalSource> sources, GoalModeHooks* hooks, LearningSystem* learning,
              const TemplateSystem* templates, SimRng* rng, GoalSystemConfig cfg);
    void service(int64_t now);  // Goal subsystem Service (every frame)

    // ---- state queries ----
    const std::vector<GoalSource>& sources() const { return sources_; }
    const std::vector<std::vector<Goal>>& active() const { return active_; }      // per category
    const std::vector<std::vector<Goal>>& proposed() const { return proposed_; }  // per category
    uint32_t quota() const { return quota_; }
    int32_t countdown() const { return countdown_; }
    uint32_t evaluations_last_pass() const { return evals_last_; }
    uint32_t passes() const { return passes_; }
    bool accept_pending() const { return pending_ready_ >= 0; }  // OPT2 t10: batched-accept solves in flight
    int64_t accept_ready_frame() const { return pending_ready_; }
    uint32_t commits() const { return commits_; }
    int32_t exclusion_count(uint32_t goal_type) const;
    bool proposable(uint32_t goal_type) const;
    static float effective_desire(const Goal& g);
    // 0x692860 AllowedTime(G, 0): Time_Limit without a PP, else base x Build_Time_Delay_Tolerance (base = nominal_time
    // with GoalSystemConfig::nominal_allowed_time, else eta). <= 0 = no limit.
    float allowed_time(const Goal& g) const;

    // GiveDesireBonus (0x699ab0): one-shot bump of a PROPOSED (type, target). True when found.
    bool give_desire_bonus(uint32_t goal_type, int32_t target, float bonus);
    // A running plan ended by itself: unlink it from its goal (the goal is then dropped at the next commit).
    void plan_ended(int32_t running);
    // Ends every running plan except `keep` (Purge_Goals) [L]; their goals are released.
    void purge(int32_t keep_running);
    Goal* find_active_by_running(int32_t running);
    Goal* find_active(uint32_t goal_type, int32_t target);
    // Block list +0x2a0 (AI_GALACTIC_2 §5.1) [H]: TaskForce.Block_Goal_Proposal clears the proposed set and
    // appends the plan's goal (key = goal serial); Unblock removes every occurrence. While non-empty nothing
    // is proposed or activated.
    void block_proposals(uint32_t goal_serial);
    void unblock_proposals(uint32_t goal_serial);
    bool proposals_blocked() const { return !block_list_.empty(); }

    void hash(StateHasher& h) const;
    // N1 save / load (alamo/core/save_file.hpp): every field except the init() wiring (definitions, sources, hooks,
    // learning, templates, RNG, config), which the loader rebuilds. read() needs init() first: the category / goal
    // type counts and the source count must match the saved ones.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    bool proposal_slice(int64_t now);  // true when the pass completed
    void end_pass(int64_t now);        // budget weights, Commit, next quota and sleep
    void accept_tail(Goal g);          // slot 10's budget test, then insert
    void apply_pending();              // OPT2 t10: accept_finish + accept_tail of the pending goals
    bool skip(uint32_t goal_type, int32_t target);
    void insert(Goal g);
    void commit(int64_t now);
    void resolve(int32_t category, int64_t now);
    void maintenance(int32_t category, int64_t now);
    void activate(Goal& g, int64_t now, bool is_new);
    void release(Goal& g, bool by_goal_system);
    bool fundable(const Goal& g);
    bool time_ok(const Goal& g) const;
    bool is_like(uint32_t type, uint32_t other) const;

    const AiDefinitions* defs_ = nullptr;
    GoalModeHooks* hooks_ = nullptr;
    LearningSystem* learning_ = nullptr;
    const TemplateSystem* templates_ = nullptr;
    SimRng* rng_ = nullptr;
    GoalSystemConfig cfg_;
    std::vector<GoalSource> sources_;
    std::vector<std::vector<Goal>> active_, proposed_;
    std::vector<int32_t> exclusion_;  // per goal type (+0x260)
    std::vector<uint32_t> block_list_;  // +0x2a0 (goal serials)
    // OPT2 t03: one slice's candidates (visit order) and their batched values; scratch, rebuilt every slice
    struct Candidate {
        uint32_t source = 0;
        int32_t target = -1;
        uint8_t prepared = 0, ok = 0;
        double value = 0.0;
    };
    std::vector<Candidate> batch_;
    // OPT2 t10 batched accept: the goals whose solves are in flight (visit order), their ready frame (-1: none) and
    // whether their slice completed the pass (Commit waits for them).
    std::vector<Goal> pending_;
    int64_t pending_ready_ = -1;
    bool pending_pass_done_ = false;
    // cursor (+0x178..+0x18c)
    uint32_t cursor_source_ = 0, cursor_target_ = 0;
    uint32_t quota_ = 0;
    int32_t countdown_ = 0;
    uint32_t evals_pass_ = 0, evals_last_ = 0;
    uint32_t passes_ = 0, commits_ = 0;
    uint32_t next_serial_ = 1;
    int64_t now_ = 0;
};

}  // namespace alamo::ai

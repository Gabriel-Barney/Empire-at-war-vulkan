// AI framework: the goal system - proposal, commit and Resolve_Category (docs/re/AI_GALACTIC.md §1.3-1.6).
// Mode-agnostic: everything that depends on the mode (targets, Applies, the reachability block, desire
// evaluation, Find_Plan_For_Goal + PotentialPlan, plans, budget) goes through GoalModeHooks.
//
// SERVICE (every frame, 0x6cacd0) [H]: countdown > 0 -> decrement and return; else one proposal slice of
// `quota` evaluations; when the pass completes: budget weights (hook), Commit, then
//   quota = clamp(min(ceil(evalsLastPass / (FPS*5)), (int)(20 / max(1, players - activeHumans) + 0.5)), >= 1)
// and countdown = the difficulty's Goal_Cycle_Sleep frames.
// [port] The quota of the very first pass is the cap term (no previous pass exists; the original's initial
// value is not traced).
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
// COMMIT (0x6c8710) and RESOLVE (0x6c8970): exactly §1.6, with the hysteresis
//   eff(d) = state > 0 && 0.01 < d < 40 ? d + (1 - (d - 0.01) / 39.99) * 1.6 : d.
// [port] Every sort is std::stable_sort descending (the original's std::sort is insertion sort, i.e.
// stable, up to 32 elements and unstable above; ties keep insertion order here - a documented deviation
// that is only lockstep-safe because every peer runs this code). Category order: stable descending by
// normalised budget weight [M]. The per-category extra-goal count (template int +0xe8, open item 6) is
// GoalSystemConfig::extra_goals_per_category.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_learning.hpp"
#include "alamo/ai/ai_templates.hpp"

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
    // g.plan / g.pp_* / g.cost / g.eta. False rejects the candidate.
    virtual bool accept(Goal& g) = 0;
    virtual bool plan_activatable(const Goal& g) = 0;  // 0x6c4a50
    virtual bool plan_invalid(const Goal& g) = 0;      // 0x6c47c0
    virtual bool replaceable(const Goal& g) = 0;       // 0x692740
    virtual void refresh_plan(Goal& g) = 0;            // 0x6c2d60
    virtual bool uses_budget() = 0;                    // galactic context
    virtual bool can_fund(const Goal& g) = 0;          // 0x6129c0 (true when no budget)
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
};

struct GoalSystemConfig {
    int32_t sleep_frames = 0;             // Goal_Cycle_Sleep_Duration x FPS (slot 11)
    int32_t extra_goals_per_category = 1; // template +0xe8 [L, open item 6]
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
    uint32_t passes() const { return passes_; }
    uint32_t commits() const { return commits_; }
    int32_t exclusion_count(uint32_t goal_type) const;
    bool proposable(uint32_t goal_type) const;
    static float effective_desire(const Goal& g);

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

    void hash(StateHasher& h) const;
    // N1 save / load (alamo/core/save_file.hpp): every field except the init() wiring (definitions, sources, hooks,
    // learning, templates, RNG, config), which the loader rebuilds. read() needs init() first: the category / goal
    // type counts and the source count must match the saved ones.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    bool proposal_slice(int64_t now);  // true when the pass completed
    bool skip(uint32_t goal_type, int32_t target);
    void insert(Goal g);
    void commit(int64_t now);
    void resolve(int32_t category, int64_t now);
    void maintenance(int32_t category);
    void activate(Goal& g, int64_t now, bool is_new);
    void release(Goal& g, bool by_goal_system);
    bool fundable(const Goal& g);
    bool time_ok(const Goal& g) const;
    float allowed_time(const Goal& g) const;
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
    bool blocked_ = false;              // = !block_list_.empty()
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

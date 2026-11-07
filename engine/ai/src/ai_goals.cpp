#include "alamo/ai/ai_goals.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"

namespace alamo::ai {

namespace {

float clamp_desire(double d) {
    if (!(d > 0.0)) return 0.0f;
    const float f = static_cast<float>(d);
    return f > kAiBigFloat ? kAiBigFloat : f;
}

void sort_by_eff(std::vector<Goal>& v) {
    std::stable_sort(v.begin(), v.end(), [](const Goal& a, const Goal& b) {
        return GoalSystem::effective_desire(a) > GoalSystem::effective_desire(b);
    });
}

}  // namespace

float GoalSystem::effective_desire(const Goal& g) {
    const float d = g.desire;
    if (g.state != GoalState::Proposed && d > 0.01f && d < 40.0f) return d + (1.0f - (d - 0.01f) / 39.99f) * 1.6f;
    return d;
}

void GoalSystem::init(const AiDefinitions& defs, std::vector<GoalSource> sources, GoalModeHooks* hooks,
                      LearningSystem* learning, const TemplateSystem* templates, SimRng* rng, GoalSystemConfig cfg) {
    defs_ = &defs;
    rng_ = rng;
    sources_ = std::move(sources);
    hooks_ = hooks;
    learning_ = learning;
    templates_ = templates;
    cfg_ = cfg;
    active_.assign(defs.categories.size(), {});
    proposed_.assign(defs.categories.size(), {});
    exclusion_.assign(defs.goals.size(), 0);
    cursor_source_ = cursor_target_ = 0;
    countdown_ = 0;
    evals_pass_ = evals_last_ = 0;
    quota_ = 0;
    pending_.clear();
    pending_ready_ = -1;
    pending_pass_done_ = false;
}

int32_t GoalSystem::exclusion_count(uint32_t goal_type) const {
    return goal_type < exclusion_.size() ? exclusion_[goal_type] : 0;
}

bool GoalSystem::proposable(uint32_t goal_type) const {
    if (proposals_blocked()) return false;
    if (templates_ && templates_->goal_value(goal_type) <= 0) return false;
    return exclusion_count(goal_type) == 0;
}

// ---------------------------------------------------------------- service

void GoalSystem::service(int64_t now) {
    now_ = now;
    if (pending_ready_ >= 0) {  // OPT2 t10: a slice's solves are in flight
        if (now < pending_ready_) return;
        apply_pending();
        if (std::exchange(pending_pass_done_, false)) {
            end_pass(now);
            return;
        }
    }
    if (countdown_ > 0) {
        --countdown_;
        return;
    }
    if (quota_ == 0) {  // [port] first pass: the cap term
        const int32_t players = std::max(1, hooks_->quota_players());
        quota_ = static_cast<uint32_t>(std::max(1, static_cast<int32_t>(20.0f / static_cast<float>(players) + 0.5f)));
    }
    if (!proposal_slice(now)) return;
    if (pending_ready_ >= 0) {  // OPT2 t10: Commit once the last slice's solves are applied
        pending_pass_done_ = true;
        return;
    }
    end_pass(now);
}

void GoalSystem::end_pass(int64_t now) {
    hooks_->budget_weights();
    commit(now);
    const int32_t players = std::max(1, hooks_->quota_players());
    const auto cap = static_cast<int32_t>(20.0f / static_cast<float>(players) + 0.5f);
    const auto need = static_cast<int32_t>(
        std::ceil(static_cast<double>(evals_last_) / static_cast<double>(kAiFps * 5)));
    quota_ = static_cast<uint32_t>(std::max(1, std::min(need, cap)));
    countdown_ = cfg_.sleep_frames;
    ++passes_;
}

bool GoalSystem::skip(uint32_t goal_type, int32_t target) {
    if (find_active(goal_type, target)) return true;
    return hooks_->blocked(goal_type, target);
}

bool GoalSystem::proposal_slice(int64_t now) {
    // OPT2 t10: slice_end() on every return path.
    struct SliceScope {
        GoalModeHooks* h;
        explicit SliceScope(GoalModeHooks* x) : h(x) { h->slice_begin(); }
        ~SliceScope() { h->slice_end(); }
        SliceScope(const SliceScope&) = delete;
        SliceScope& operator=(const SliceScope&) = delete;
    } scope(hooks_);
    uint32_t used = 0;
    const bool batch_accept = hooks_->accept_batched();  // OPT2 t10
    auto evaluate_candidate = [&](uint32_t src, int32_t target, const Candidate* batched) {
        const GoalSource& s = sources_[src];
        const GoalTypeDef& def = defs_->goals[s.goal_type];
        ++evals_pass_;
        double v = 0.0;
        if (batched && batched->prepared) {
            hooks_->evaluated(s.function);
            if (!batched->ok) return;
            v = batched->value;
        } else if (!hooks_->evaluate(s.function, target, v)) {
            return;
        }
        const int32_t c1 = learning_->recent_goal_failures(s.goal_type, target, now);
        const int32_t c2 = learning_->recent_activation_failures(s.goal_type, target, now);
        const double desire = v + static_cast<double>(static_cast<float>(c1) * def.per_failure_desire_adjust +
                                                      static_cast<float>(c2) * def.per_activation_failure_desire_adjust);
        if (!(desire > 0.0)) return;
        Goal g;
        g.type = s.goal_type;
        g.source = src;
        g.target = target;
        g.desire = g.priority = clamp_desire(desire);
        g.created = now;
        if (batch_accept) {  // OPT2 t10: pending until the slice's solves are applied
            if (!hooks_->accept_prepare(g, static_cast<uint32_t>(pending_.size()))) {
                hooks_->drop_potential_plan(g);
                return;
            }
            pending_.push_back(std::move(g));
            return;
        }
        if (!hooks_->accept(g)) {
            hooks_->drop_potential_plan(g);
            return;
        }
        accept_tail(std::move(g));
    };
    // OPT2 t03: batched values (header comment): the walk only records the candidates; flush() evaluates and commits them.
    // OPT2 t10: batched accept walks the same way (with no candidate_pf every value is evaluated in place in flush()).
    const ParallelFor* pf = hooks_->candidate_pf();
    batch_.clear();
    auto visit = [&](uint32_t src, int32_t target) {
        if (pf || batch_accept) batch_.push_back({src, target});
        else evaluate_candidate(src, target, nullptr);
    };
    auto start_solves = [&] {  // OPT2 t10
        if (pending_.empty()) return;
        pending_ready_ = now + static_cast<int64_t>(hooks_->accept_latency());
        hooks_->accept_start(static_cast<uint32_t>(pending_.size()), now, pending_ready_);
        if (pending_ready_ == now) apply_pending();
    };
    auto flush = [&] {
        if (batch_.empty()) return;
        uint32_t prepared = 0;
        for (Candidate& c : batch_) {
            c.prepared = pf && hooks_->prepare_evaluation(sources_[c.source].function) ? 1 : 0;
            prepared += c.prepared;
        }
        const auto values = [this](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) {
                Candidate& c = batch_[i];
                if (c.prepared) c.ok = hooks_->evaluate_prepared(sources_[c.source].function, c.target, c.value) ? 1 : 0;
            }
        };
        // Fewer than kMinFanOut prepared values run in place: waking helpers costs about what they take (a space battle's
        // slices hold 2-3 values of about 20 us; a land battle's about 5 of about 120 us).
        constexpr uint32_t kMinFanOut = 4;
        if (prepared >= kMinFanOut) (*pf)(static_cast<uint32_t>(batch_.size()), 1, values);
        else if (prepared > 0) values(0, static_cast<uint32_t>(batch_.size()));
        for (const Candidate& c : batch_) evaluate_candidate(c.source, c.target, &c);
        batch_.clear();
        start_solves();
    };
    while (cursor_source_ < sources_.size()) {
        const GoalSource& s = sources_[cursor_source_];
        const GoalTypeDef& def = defs_->goals[s.goal_type];
        if (!proposable(s.goal_type) || def.category < 0) {
            ++cursor_source_;
            cursor_target_ = 0;
            continue;
        }
        if (def.global()) {
            if (cursor_target_ == 0 && !skip(s.goal_type, -1)) {
                if (used >= quota_) {
                    flush();
                    return false;
                }
                ++used;
                visit(cursor_source_, -1);
            }
            ++cursor_source_;
            cursor_target_ = 0;
            continue;
        }
        const uint32_t n = hooks_->target_count();
        while (cursor_target_ < n) {
            const auto t = static_cast<int32_t>(cursor_target_);
            if (!hooks_->applies(t, def.application) || skip(s.goal_type, t)) {
                ++cursor_target_;
                continue;
            }
            if (used >= quota_) {
                flush();
                return false;
            }
            ++used;
            visit(cursor_source_, t);
            ++cursor_target_;
        }
        ++cursor_source_;
        cursor_target_ = 0;
    }
    flush();
    cursor_source_ = cursor_target_ = 0;
    evals_last_ = evals_pass_;
    evals_pass_ = 0;
    return true;
}

void GoalSystem::accept_tail(Goal g) {
    if (hooks_->uses_budget()) {
        const float at = allowed_time(g);
        if (!((at <= 0.0f || at >= g.eta) && hooks_->can_fund(g))) {
            hooks_->drop_potential_plan(g);
            return;
        }
    }
    insert(std::move(g));
}

void GoalSystem::apply_pending() {
    std::vector<Goal> p = std::move(pending_);
    pending_.clear();
    pending_ready_ = -1;
    for (size_t i = 0; i < p.size(); ++i) {
        Goal& g = p[i];
        if (!hooks_->accept_finish(g, static_cast<uint32_t>(i))) {
            hooks_->drop_potential_plan(g);
            continue;
        }
        accept_tail(std::move(g));
    }
}

void GoalSystem::insert(Goal g) {
    const int32_t c = defs_->goals[g.type].category;
    std::vector<Goal>& p = proposed_[static_cast<size_t>(c)];
    for (Goal& e : p)
        if (e.type == g.type && e.target == g.target) {
            if (g.desire > e.desire) {
                hooks_->drop_potential_plan(e);
                const uint32_t serial = e.serial;
                e = std::move(g);
                e.serial = serial;
            } else {
                hooks_->drop_potential_plan(g);
            }
            return;
        }
    g.serial = next_serial_++;
    p.push_back(std::move(g));
}

bool GoalSystem::give_desire_bonus(uint32_t goal_type, int32_t target, float bonus) {
    if (goal_type >= defs_->goals.size()) return false;
    const int32_t c = defs_->goals[goal_type].category;
    if (c < 0) return false;
    for (Goal& e : proposed_[static_cast<size_t>(c)])
        if (e.type == goal_type && e.target == target) {
            e.desire = clamp_desire(static_cast<double>(e.desire) + static_cast<double>(bonus));
            e.priority = e.desire;
            return true;
        }
    return false;
}

Goal* GoalSystem::find_active(uint32_t goal_type, int32_t target) {
    if (goal_type >= defs_->goals.size()) return nullptr;
    const int32_t c = defs_->goals[goal_type].category;
    if (c < 0) return nullptr;
    for (Goal& g : active_[static_cast<size_t>(c)])
        if (g.type == goal_type && g.target == target) return &g;
    return nullptr;
}

void GoalSystem::block_proposals(uint32_t goal_serial) {
    for (auto& p : proposed_) {
        for (Goal& g : p) hooks_->drop_potential_plan(g);
        p.clear();
    }
    block_list_.push_back(goal_serial);
    if (!pending_.empty()) {  // OPT2 t10: proposals made before the block are dropped with the proposed set
        for (Goal& g : pending_) hooks_->drop_potential_plan(g);
        pending_.clear();
        hooks_->accept_cancel();
    }
}

void GoalSystem::unblock_proposals(uint32_t goal_serial) {
    block_list_.erase(std::remove(block_list_.begin(), block_list_.end(), goal_serial), block_list_.end());
}

Goal* GoalSystem::find_active_by_running(int32_t running) {
    for (auto& cat : active_)
        for (Goal& g : cat)
            if (g.running == running) return &g;
    return nullptr;
}

void GoalSystem::plan_ended(int32_t running) {
    if (running < 0) return;
    if (Goal* g = find_active_by_running(running)) g->running = -1;
}

void GoalSystem::purge(int32_t keep_running) {
    for (auto& cat : active_)
        for (Goal& g : cat)
            if (g.running >= 0 && g.running != keep_running) {
                hooks_->end_plan(g, true);
                g.running = -1;
            }
}

// ---------------------------------------------------------------- commit / resolve

// 0x692860(G, 0) [H]: no PP -> 0 + Time_Limit; else (0x6c0450(PP) + 0) x Build_Time_Delay_Tolerance (A2F_RE_READS §1.4
// item 2, A2f t09). 0x6c0450 is Goal::nominal_time (the selection's nominal build time x the mode's Build_Time_Multiplier),
// not the PP ETA: the accept test (0x6b8660), Resolve's time_ok (0x6c8970) and maintenance compare it with the ETA.
// [port] Without GoalSystemConfig::nominal_allowed_time the base stays the PP ETA (both AIs set it since A2g t06).
float GoalSystem::allowed_time(const Goal& g) const {
    const GoalTypeDef& def = defs_->goals[g.type];
    if (g.plan == kAiNone || !g.pp_valid) return def.time_limit;
    return (cfg_.nominal_allowed_time ? g.nominal_time : g.eta) * def.build_time_delay_tolerance;
}

bool GoalSystem::time_ok(const Goal& g) const {
    const float at = allowed_time(g);
    return !(at > 0.0f && at < g.eta);
}

bool GoalSystem::fundable(const Goal& g) { return !hooks_->uses_budget() || hooks_->can_fund(g); }

bool GoalSystem::is_like(uint32_t type, uint32_t other) const {
    const std::vector<uint32_t>& l = defs_->goals[type].is_like;
    return std::find(l.begin(), l.end(), other) != l.end();
}

void GoalSystem::activate(Goal& g, int64_t now, bool is_new) {
    const bool was_active = g.state == GoalState::Active || g.state == GoalState::Executing;
    if (!was_active) {
        g.state = GoalState::Active;
        for (const uint32_t x : defs_->goals[g.type].exclusions) ++exclusion_[x];
    }
    // Add goal 0x610d30: first add, or the re-add of a goal flagged by the commit's reweight (A2 stage 2).
    if (hooks_->uses_budget()) hooks_->budget_add(g);
    if (is_new) {
        const GoalTypeDef& def = defs_->goals[g.type];
        learning_->record_activation(g.type, g.target, true, def.activation_tracking_duration, now);
    }
}

void GoalSystem::release(Goal& g, bool by_goal_system) {
    const bool was_active = g.state == GoalState::Active || g.state == GoalState::Executing;
    if (g.running >= 0) {
        hooks_->end_plan(g, by_goal_system);
        g.running = -1;
    }
    if (was_active) {
        for (const uint32_t x : defs_->goals[g.type].exclusions)
            if (exclusion_[x] > 0) --exclusion_[x];
        if (hooks_->uses_budget()) hooks_->budget_release(g);
    }
    hooks_->drop_potential_plan(g);
    g.state = GoalState::Released;
}

void GoalSystem::maintenance(int32_t category, int64_t now) {
    std::vector<Goal>& a = active_[static_cast<size_t>(category)];
    std::vector<Goal> keep;
    for (Goal& g : a) {
        bool drop = g.state == GoalState::Released || g.running < 0;
        if (!drop && hooks_->uses_budget() && !hooks_->can_fund(g)) drop = true;
        if (!drop && effective_desire(g) <= 0.0f && hooks_->replaceable(g)) drop = true;
        if (!drop && hooks_->plan_invalid(g)) drop = true;  // template flag +0x60->+0xa8 not modelled
        if (!drop && hooks_->uses_budget()) {
            const float at = allowed_time(g);
            if (at > 0.0f && at < hooks_->elapsed_eta(g) && hooks_->replaceable(g)) drop = true;
        }
        if (drop) {
            release(g, true);
            continue;
        }
        if (hooks_->uses_budget()) hooks_->budget_add(g);  // re-add after the reweight (AI_GALACTIC_2 §3.2)
        hooks_->refresh_plan(g);
        keep.push_back(std::move(g));
    }
    a = std::move(keep);
    (void)now;
}

void GoalSystem::commit(int64_t now) {
    ++commits_;
    const size_t ncat = active_.size();
    // 1. Re-evaluate incumbents (no learning terms).
    for (auto& cat : active_)
        for (Goal& g : cat) {
            const GoalTypeDef& def = defs_->goals[g.type];
            const bool ok = g.target < 0 ? def.global() : hooks_->applies(g.target, def.application);
            if (!ok) continue;
            double v = 0.0;
            if (g.source < sources_.size() && hooks_->evaluate(sources_[g.source].function, g.target, v))
                g.desire = g.priority = clamp_desire(v);
            else
                g.desire = g.priority = 0.0f;
        }
    // 2. Sort incumbents.
    for (auto& cat : active_) sort_by_eff(cat);
    // 3. Category order.
    std::vector<int32_t> order = hooks_->category_order();
    if (order.size() != ncat) {
        order.clear();
        for (size_t c = 0; c < ncat; ++c) order.push_back(static_cast<int32_t>(c));
    }
    // 4. 0x6c0be0 for active goals with a PotentialPlan: production once per PP; the free-store reservations of every
    //    such goal are suspended (A1_READINGS §28.6 step 2) before any Resolve_Category.
    for (const int32_t c : order)
        for (Goal& g : active_[static_cast<size_t>(c)]) {
            if (g.pp_valid && !g.production_queued) {
                hooks_->queue_production(g);
                g.production_queued = true;
            }
            if (g.pp_valid) hooks_->suspend_reservations(g);
        }
    // 5. Sort and truncate proposals.
    for (size_t c = 0; c < ncat; ++c) {
        std::vector<Goal>& p = proposed_[c];
        sort_by_eff(p);
        const size_t limit = static_cast<size_t>(cfg_.extra_goals_per_category) + active_[c].size();
        while (p.size() > limit) {
            hooks_->drop_potential_plan(p.back());
            p.pop_back();
        }
    }
    // 6. Resolve in order, then clear proposals.
    for (const int32_t c : order) resolve(c, now);
    for (auto& p : proposed_) {
        for (Goal& g : p) hooks_->drop_potential_plan(g);
        p.clear();
    }
}

void GoalSystem::resolve(int32_t category, int64_t now) {
    const size_t ci = static_cast<size_t>(category);
    std::vector<Goal>& A = active_[ci];
    std::vector<Goal>& P = proposed_[ci];
    // 2. Filter proposals whose target still Applies.
    std::vector<Goal> props;
    for (Goal& g : P) {
        const GoalTypeDef& def = defs_->goals[g.type];
        if (g.target >= 0 && !hooks_->applies(g.target, def.application)) {
            hooks_->drop_potential_plan(g);
            continue;
        }
        props.push_back(std::move(g));
    }
    P.clear();
    if (props.empty()) {
        maintenance(category, now);
        return;
    }
    sort_by_eff(props);
    const size_t limit = static_cast<size_t>(cfg_.extra_goals_per_category) + A.size();
    const float best = effective_desire(props.front());
    // 3. Classify incumbents.
    std::vector<Goal> keep, old;
    for (Goal& g : A) {
        if (effective_desire(g) >= best || !hooks_->replaceable(g)) {
            if (hooks_->plan_invalid(g)) {
                release(g, true);
            } else {
                if (hooks_->uses_budget()) hooks_->budget_add(g);  // re-add after the reweight (§3.2)
                hooks_->refresh_plan(g);  // 0x6c2d60: re-assert its reservations (A1_READINGS §28.6 step 3)
                keep.push_back(std::move(g));
            }
        } else {
            old.push_back(std::move(g));
        }
    }
    A.clear();
    // 4. Pool.
    std::vector<Goal> pool;
    for (Goal& g : old) pool.push_back(std::move(g));
    for (Goal& g : props) pool.push_back(std::move(g));
    sort_by_eff(pool);
    uint32_t last_old_type = kAiNone;
    for (const Goal& g : pool)
        if (g.state != GoalState::Proposed) last_old_type = g.type;  // the lowest old candidate (pool sorted desc)
    std::vector<uint8_t> taken(pool.size(), 0);
    auto is_kept_or_active = [&](const Goal& g) {
        if (g.state != GoalState::Proposed) return true;
        for (const Goal& k : keep)
            if (k.type == g.type && k.target == g.target) return true;
        return false;
    };
    auto accept_goal = [&](size_t i) {
        Goal& g = pool[i];
        const bool is_new = g.state == GoalState::Proposed;
        hooks_->refresh_plan(g);
        activate(g, now, is_new);
        taken[i] = 1;
        keep.push_back(std::move(g));
    };
    // 5. Greedy phase.
    for (size_t i = 0; i < pool.size(); ++i) {
        if (hooks_->uses_budget() && !(hooks_->category_credits(category) > 0.0f)) break;
        Goal& g = pool[i];
        const GoalTypeDef& def = defs_->goals[g.type];
        if (proposals_blocked() || !proposable(g.type)) break;
        if (is_kept_or_active(g)) break;
        if (!hooks_->plan_activatable(g) || !fundable(g)) {
            learning_->record_activation(g.type, g.target, false, def.activation_tracking_duration, now);
            break;
        }
        if (!time_ok(g)) {
            learning_->record_activation(g.type, g.target, false, def.activation_tracking_duration, now);
            break;
        }
        if (last_old_type != kAiNone && is_like(g.type, last_old_type)) break;
        if (keep.size() >= limit) break;
        accept_goal(i);
    }
    // 6. Weighted random phase.
    while (keep.size() < limit) {
        std::vector<size_t> idx;
        std::vector<float> w;
        for (size_t i = 0; i < pool.size(); ++i) {
            if (taken[i]) continue;
            const Goal& g = pool[i];
            if (proposals_blocked() || !proposable(g.type) || is_kept_or_active(g)) continue;
            if (!fundable(g)) continue;
            const float e = effective_desire(g);
            if (!(e > 0.0f)) continue;
            idx.push_back(i);
            w.push_back(e);
        }
        float total = 0.0f;
        for (const float x : w) total += x;
        if (!(total > 0.0f)) break;
        const uint32_t k = pick_cumulative(w, *rng_);
        const size_t i = idx[k];
        Goal& g = pool[i];
        const GoalTypeDef& def = defs_->goals[g.type];
        if (!hooks_->plan_activatable(g) || !fundable(g) || !time_ok(g)) {
            learning_->record_activation(g.type, g.target, false, def.activation_tracking_duration, now);
            taken[i] = 1;
            release(g, true);
            continue;
        }
        accept_goal(i);
    }
    // 7. Finalize.
    for (Goal& g : keep) {
        if (g.running < 0 && g.state == GoalState::Active) {
            hooks_->instantiate(g);
            if (g.running >= 0) g.state = GoalState::Executing;
        }
        A.push_back(std::move(g));
    }
    for (size_t i = 0; i < pool.size(); ++i) {
        if (taken[i]) continue;
        Goal& g = pool[i];
        if (g.state != GoalState::Proposed) release(g, true);
        else hooks_->drop_potential_plan(g);
    }
}

// ---------------------------------------------------------------- CRC

void GoalSystem::hash(StateHasher& h) const {
    h.u32(cursor_source_);
    h.u32(cursor_target_);
    h.u32(quota_);
    h.i32(countdown_);
    h.u32(evals_pass_);
    h.u32(evals_last_);
    h.u32(next_serial_);
    const bool nominal = cfg_.nominal_allowed_time;  // A2f t09: hashed only where it is read (galactic, tactical since A2g t06)
    auto goal = [&h, nominal](const Goal& g) {
        h.u32(g.serial);
        h.u32(g.type);
        h.i32(g.target);
        h.u8(static_cast<uint8_t>(g.state));
        h.f32(g.desire);
        h.u32(g.plan);
        h.f32(g.cost);
        h.f32(g.eta);
        if (nominal) h.f32(g.nominal_time);
        h.i32(g.running);
    };
    for (const auto* v : {&active_, &proposed_}) {
        h.u32(static_cast<uint32_t>(v->size()));
        for (const auto& cat : *v) {
            h.u32(static_cast<uint32_t>(cat.size()));
            for (const Goal& g : cat) goal(g);
        }
    }
    for (const int32_t x : exclusion_) h.i32(x);
    h.u32(static_cast<uint32_t>(block_list_.size()));
    for (const uint32_t x : block_list_) h.u32(x);
    if (pending_ready_ >= 0) {  // OPT2 t10: only while solves are pending (nothing extra in the original order)
        h.i64(pending_ready_);
        h.boolean(pending_pass_done_);
        h.u32(static_cast<uint32_t>(pending_.size()));
        for (const Goal& g : pending_) goal(g);
    }
}

}  // namespace alamo::ai

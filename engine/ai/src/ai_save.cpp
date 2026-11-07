// N1 save / load of the AI framework (alamo/core/save_file.hpp contract): goal, template, learning and budget
// systems, the context, the enable gate, the per-AI path finder and the galactic perception + its snapshot.
// Every read() parses into a copy / locals and commits only when the whole record was valid.
#include <algorithm>

#include "ai_save_io.hpp"
#include "alamo/ai/ai_budget.hpp"
#include "alamo/ai/ai_context.hpp"
#include "alamo/ai/ai_goals.hpp"
#include "alamo/ai/ai_learning.hpp"
#include "alamo/ai/ai_path_finder.hpp"
#include "alamo/ai/ai_templates.hpp"
#include "alamo/ai/galactic_perception.hpp"

namespace alamo::ai {

using save::get;
using save::get_vec;
using save::put;
using save::put_vec;

namespace {
// kGoalsVersion 4 (cu12 t15): the blocked byte (= !block_list_.empty()) removed.
constexpr uint32_t kGoalsVersion = 4, kTemplatesVersion = 1, kLearningVersion = 1, kBudgetVersion = 2, kContextVersion = 1,
                   kGateVersion = 1, kPathVersion = 2, kPerceptionVersion = 1, kSnapshotVersion = 1;

void put_goal(ByteWriter& w, const Goal& g) {
    w.u32(g.serial);
    w.u32(g.type);
    w.u32(g.source);
    w.i32(g.target);
    w.u8(static_cast<uint8_t>(g.state));
    w.f32(g.desire);
    w.f32(g.priority);
    w.i64(g.created);
    w.u32(g.plan);
    w.boolean(g.pp_valid);
    w.f32(g.cost);
    w.f32(g.eta);
    w.f32(g.nominal_time);  // 3: A2f t09, the AllowedTime base
    w.i32(g.pp_handle);
    w.boolean(g.production_queued);
    w.i32(g.running);
    w.boolean(g.budgeted);
    w.f32(g.allocated);
}

void get_goal(ByteReader& r, Goal& g) {
    g.serial = r.u32();
    g.type = r.u32();
    g.source = r.u32();
    g.target = r.i32();
    const uint8_t st = r.u8();
    if (st > static_cast<uint8_t>(GoalState::Released)) r.fail();
    g.state = static_cast<GoalState>(st);
    g.desire = r.f32();
    g.priority = r.f32();
    g.created = r.i64();
    g.plan = r.u32();
    g.pp_valid = r.boolean();
    g.cost = r.f32();
    g.eta = r.f32();
    g.nominal_time = r.f32();
    g.pp_handle = r.i32();
    g.production_queued = r.boolean();
    g.running = r.i32();
    g.budgeted = r.boolean();
    g.allocated = r.f32();
}

using RecentMap = std::map<LearningSystem::Key, std::vector<LearningSystem::Record>>;

void put_recent(ByteWriter& w, const RecentMap& m) {
    w.u32(static_cast<uint32_t>(m.size()));
    for (const auto& [k, v] : m) {
        w.u32(k.first);
        w.i32(k.second);
        w.u32(static_cast<uint32_t>(v.size()));
        for (const LearningSystem::Record& rec : v) {
            w.boolean(rec.success);
            w.i64(rec.expiry);
        }
    }
}

bool get_recent(ByteReader& r, RecentMap& out) {
    const uint32_t n = save::count(r, 12);
    RecentMap m;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        LearningSystem::Key k;
        k.first = r.u32();
        k.second = r.i32();
        const uint32_t c = save::count(r, 9);
        std::vector<LearningSystem::Record> v(c);
        for (LearningSystem::Record& rec : v) {
            rec.success = r.boolean();
            rec.expiry = r.i64();
        }
        if (!m.empty() && !(m.rbegin()->first < k)) r.fail();  // canonical (ascending) key order
        m.emplace_hint(m.end(), k, std::move(v));
    }
    if (!r.ok()) return false;
    out = std::move(m);
    return true;
}

template <class K>
void put_counters(ByteWriter& w, const std::map<K, LearningSystem::Counter>& m) {
    w.u32(static_cast<uint32_t>(m.size()));
    for (const auto& [k, c] : m) {
        if constexpr (std::is_same_v<K, LearningSystem::Key>) {
            w.u32(k.first);
            w.i32(k.second);
        } else {
            w.u32(k);
        }
        w.u32(c.attempts);
        w.u32(c.successes);
    }
}

template <class K>
bool get_counters(ByteReader& r, std::map<K, LearningSystem::Counter>& out) {
    const uint32_t n = save::count(r, 12);
    std::map<K, LearningSystem::Counter> m;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        K k{};
        if constexpr (std::is_same_v<K, LearningSystem::Key>) {
            k.first = r.u32();
            k.second = r.i32();
        } else {
            k = r.u32();
        }
        LearningSystem::Counter c;
        c.attempts = r.u32();
        c.successes = r.u32();
        if (!m.empty() && !(m.rbegin()->first < k)) r.fail();
        m.emplace_hint(m.end(), k, c);
    }
    if (!r.ok()) return false;
    out = std::move(m);
    return true;
}
}  // namespace

// ================================================================ GoalSystem

void GoalSystem::write(ByteWriter& w) const {
    w.u32(kGoalsVersion);
    w.u32(static_cast<uint32_t>(sources_.size()));
    for (const auto* v : {&active_, &proposed_}) {
        w.u32(static_cast<uint32_t>(v->size()));
        for (const std::vector<Goal>& cat : *v) {
            w.u32(static_cast<uint32_t>(cat.size()));
            for (const Goal& g : cat) put_goal(w, g);
        }
    }
    put_vec(w, exclusion_);
    put_vec(w, block_list_);
    w.u32(cursor_source_);
    w.u32(cursor_target_);
    w.u32(quota_);
    w.i32(countdown_);
    w.u32(evals_pass_);
    w.u32(evals_last_);
    w.u32(passes_);
    w.u32(commits_);
    w.u32(next_serial_);
    w.i64(now_);
    // 2: OPT2 t10, the batched-accept goals whose solves are in flight (the mode saves the solves themselves)
    w.i64(pending_ready_);
    w.boolean(pending_pass_done_);
    w.u32(static_cast<uint32_t>(pending_.size()));
    for (const Goal& g : pending_) put_goal(w, g);
}

bool GoalSystem::read(ByteReader& r) {
    if (r.u32() != kGoalsVersion || !r.ok()) return false;
    if (r.u32() != sources_.size() || !r.ok()) return false;  // rebuilt from the same definitions
    GoalSystem t = *this;
    const size_t goal_types = defs_ ? defs_->goals.size() : 0;
    for (auto* v : {&t.active_, &t.proposed_}) {
        const uint32_t cats = r.u32();
        if (!r.ok() || cats != v->size()) return false;
        for (std::vector<Goal>& cat : *v) {
            const uint32_t n = save::count(r, 60);
            cat.assign(n, Goal{});
            for (Goal& g : cat) {
                get_goal(r, g);
                if (g.type >= goal_types || (g.source != kAiNone && g.source >= sources_.size())) r.fail();
            }
        }
    }
    std::vector<int32_t> excl;
    if (!get_vec(r, excl) || excl.size() != exclusion_.size()) return false;
    t.exclusion_ = std::move(excl);
    if (!get_vec(r, t.block_list_)) return false;
    t.cursor_source_ = r.u32();
    t.cursor_target_ = r.u32();
    t.quota_ = r.u32();
    t.countdown_ = r.i32();
    t.evals_pass_ = r.u32();
    t.evals_last_ = r.u32();
    t.passes_ = r.u32();
    t.commits_ = r.u32();
    t.next_serial_ = r.u32();
    t.now_ = r.i64();
    t.pending_ready_ = r.i64();
    t.pending_pass_done_ = r.boolean();
    const uint32_t np = save::count(r, 60);
    t.pending_.assign(np, Goal{});
    for (Goal& g : t.pending_) {
        get_goal(r, g);
        if (g.type >= goal_types || g.source >= sources_.size()) r.fail();
    }
    if (t.pending_ready_ < 0 && (np > 0 || t.pending_pass_done_)) r.fail();
    if (!r.ok()) return false;
    *this = std::move(t);
    return true;
}

// ================================================================ TemplateSystem

void TemplateSystem::write(ByteWriter& w) const {
    w.u32(kTemplatesVersion);
    put_vec(w, templates_);
    put_vec(w, active_);
    w.u32(cursor_);
    w.u32(count_);
    w.boolean(changed_);
}

bool TemplateSystem::read(ByteReader& r) {
    if (r.u32() != kTemplatesVersion || !r.ok()) return false;
    std::vector<uint32_t> templates, active;
    if (!get_vec(r, templates) || templates != templates_ || !get_vec(r, active)) return false;
    const uint32_t cursor = r.u32(), count = r.u32();
    const bool changed = r.boolean();
    if (!r.ok() || count != count_ || (cursor != 0 && cursor >= templates_.size())) return false;
    for (const uint32_t a : active)
        if (std::find(templates_.begin(), templates_.end(), a) == templates_.end()) return false;
    if (!active.empty() && (!defs_ || !plans_)) return false;
    active_ = std::move(active);
    cursor_ = cursor;
    changed_ = changed;
    recompute();  // the switch slots are a pure function of the active list
    return true;
}

// ================================================================ LearningSystem

void LearningSystem::write(ByteWriter& w) const {
    w.u32(kLearningVersion);
    put_recent(w, recent_goal_);
    put_recent(w, recent_activation_);
    put_recent(w, recent_plan_);
    put_counters(w, type_target_);
    put_counters(w, type_);
    put_counters(w, plan_);
    put_counters(w, survival_);
}

bool LearningSystem::read(ByteReader& r) {
    if (r.u32() != kLearningVersion || !r.ok()) return false;
    LearningSystem t;
    if (!get_recent(r, t.recent_goal_) || !get_recent(r, t.recent_activation_) || !get_recent(r, t.recent_plan_) ||
        !get_counters(r, t.type_target_) || !get_counters(r, t.type_) || !get_counters(r, t.plan_) ||
        !get_counters(r, t.survival_))
        return false;
    *this = std::move(t);
    return true;
}

// ================================================================ AiBudget

void AiBudget::write(ByteWriter& w) const {
    w.u32(kBudgetVersion);
    w.u32(static_cast<uint32_t>(cat_.size()));
    for (const Category& k : cat_) {
        w.f32(k.applied);
        w.f32(k.weight);
        w.f32(k.A);
        w.f32(k.R);
        w.f32(k.P);
        w.f32(k.I);
        w.f32(k.mean_h);
        w.u32(k.mean_n);
        w.u32(static_cast<uint32_t>(k.goals.size()));
        for (const GoalEntry& e : k.goals) {
            w.u32(e.goal);
            w.f32(e.req);
            w.f32(e.alloc);
            w.f32(e.h);
            w.f32(e.q);
            w.f32(e.a);
            w.f32(e.c);
            w.f32(e.s);
            w.boolean(e.readd);
        }
    }
    w.f32(rate68_);
    w.f32(period_);
    w.f32(refund_);
    w.boolean(reset_);
    w.boolean(init_);
    // Version 2 (q6 contrast/budget, AI_GALACTIC_TOKENS §9.5): + the member list in insertion order.
    w.u32(static_cast<uint32_t>(order_.size()));
    for (const int32_t c : order_) w.i32(c);
}

bool AiBudget::read(ByteReader& r) {
    if (r.u32() != kBudgetVersion || !r.ok()) return false;
    const uint32_t n = r.u32();
    if (!r.ok() || n != cat_.size()) return false;
    std::vector<Category> cats(n);
    for (Category& k : cats) {
        k.applied = r.f32();
        k.weight = r.f32();
        k.A = r.f32();
        k.R = r.f32();
        k.P = r.f32();
        k.I = r.f32();
        k.mean_h = r.f32();
        k.mean_n = r.u32();
        k.goals.resize(save::count(r, 33));
        for (GoalEntry& e : k.goals) {
            e.goal = r.u32();
            e.req = r.f32();
            e.alloc = r.f32();
            e.h = r.f32();
            e.q = r.f32();
            e.a = r.f32();
            e.c = r.f32();
            e.s = r.f32();
            e.readd = r.boolean();
        }
    }
    const float rate = r.f32(), period = r.f32(), refund = r.f32();
    const bool reset = r.boolean(), init = r.boolean();
    std::vector<int32_t> order(save::count(r, 4));
    for (int32_t& c : order) {
        c = r.i32();
        if (c < 0 || static_cast<uint32_t>(c) >= n || std::count(order.data(), &c, c) != 0) return false;  // in range, no repeat
    }
    if (!r.ok()) return false;
    cat_ = std::move(cats);
    order_ = std::move(order);
    rate68_ = rate;
    period_ = period;
    refund_ = refund;
    reset_ = reset;
    init_ = init;
    return true;
}

// ================================================================ AiContext / AiEnableGate

void AiContext::write(ByteWriter& w) const {
    w.u32(kContextVersion);
    w.u8(static_cast<uint8_t>(mode_));
    w.boolean(initialised_);
    w.i32(opponent);
    for (const ServiceClock* c : {&perception_clock, &goal_clock, &planning_clock, &execution_clock, &learning_clock,
                                  &template_clock})
        w.i64(c->next);
    for (const uint64_t n : service_counts) w.u64(n);
    goals.write(w);
    templates.write(w);
    learning.write(w);
    budget.write(w);
}

bool AiContext::read(ByteReader& r) {
    if (r.u32() != kContextVersion || !r.ok()) return false;
    if (r.u8() != static_cast<uint8_t>(mode_) || !r.ok()) return false;
    AiContext t = *this;  // the goal system's pointers keep naming this context's learning / templates
    t.initialised_ = r.boolean();
    t.opponent = r.i32();
    for (ServiceClock* c : {&t.perception_clock, &t.goal_clock, &t.planning_clock, &t.execution_clock, &t.learning_clock,
                            &t.template_clock})
        c->next = r.i64();
    for (uint64_t& n : t.service_counts) n = r.u64();
    if (!r.ok() || !t.goals.read(r) || !t.templates.read(r) || !t.learning.read(r) || !t.budget.read(r)) return false;
    *this = std::move(t);
    return true;
}

void AiEnableGate::write(ByteWriter& w) const {
    w.u32(kGateVersion);
    w.boolean(master_);
    for (const bool on : per_mode_) w.boolean(on);
}

bool AiEnableGate::read(ByteReader& r) {
    if (r.u32() != kGateVersion || !r.ok()) return false;
    const bool master = r.boolean();
    std::array<bool, kAiModeCount> per{};
    for (bool& on : per) on = r.boolean();
    if (!r.ok()) return false;
    master_ = master;
    per_mode_ = per;
    return true;
}

// ================================================================ AiPathFinder

void AiPathFinder::Input::write(ByteWriter& w) const {
    put_vec(w, value);
    put_vec(w, cost);
    w.boolean(has_revealed);
    save::put_bitset(w, revealed);
    put_vec(w, zoff);
}

bool AiPathFinder::Input::read(ByteReader& r) {
    if (!r.ok() || !get_vec(r, value) || !get_vec(r, cost)) return false;
    has_revealed = r.boolean();
    return r.ok() && save::get_bitset(r, revealed) && get_vec(r, zoff);
}

// Version 2 (OPT2 t02 / OPT3 t04): + the in-flight background recompute (kick and ready frame, its input).
void AiPathFinder::write(ByteWriter& w) const {
    w.u32(kPathVersion);
    w.boolean(created_);
    w.i32(next_);
    w.u32(runs_);
    w.u64(digest_);
    w.u32(matrix_runs_);
    in_.write(w);
    w.boolean(job_.in_flight());
    if (job_.in_flight()) {
        w.i64(job_.kick_frame());
        w.i64(job_.ready_frame());
        pend_.write(w);
    }
}

bool AiPathFinder::read(ByteReader& r, const galactic::PlanetGraph& graph, const ParallelFor& pf) {
    if (r.u32() != kPathVersion || !r.ok()) return false;
    AiPathFinder t;
    t.created_ = r.boolean();
    t.next_ = r.i32();
    t.runs_ = r.u32();
    t.digest_ = r.u64();
    t.matrix_runs_ = r.u32();
    if (!t.in_.read(r)) return false;
    const bool in_flight = r.boolean();
    int64_t kick = 0, ready = 0;
    if (in_flight) {
        kick = r.i64();
        ready = r.i64();
        if (!t.pend_.read(r) || ready < kick) return false;
    }
    if (!r.ok()) return false;
    if (t.created_) {
        // Rebuild the published matrices (and the cache's change detection) from the last applied recompute's input.
        t.cache_.update(graph, t.in_.view(), pf);
        if (matrices_digest(t.cache_.matrices()) != t.digest_) return false;
    }
    // The in-flight recompute again, inline: its result is applied on the saved ready frame (latency_job.hpp).
    if (in_flight) t.start_job(static_cast<int32_t>(kick), static_cast<int32_t>(ready), graph, pf, {});
    job_.cancel();
    t.set_background(latency_, std::move(bg_));
    *this = std::move(t);
    return true;
}

// ================================================================ GalacticSnapshot / GalacticPerception

void GalacticSnapshot::write(ByteWriter& w) const {
    w.u32(kSnapshotVersion);
    w.u32(planet_count);
    w.u32(static_cast<uint32_t>(players.size()));
    for (const PerceptionPlayer& p : players) {
        w.boolean(p.neutral);
        w.boolean(p.is_human);
        w.boolean(p.has_ai_system);
        w.boolean(p.benefits_from_corruption);
    }
    put_vec(w, relation);
    put_vec(w, owner);
    put_vec(w, political_control);
    put_vec(w, starbase_level);
    put_vec(w, starbase_power);
    put_vec(w, corruption);
    put_vec(w, unit_begin);
    w.u32(static_cast<uint32_t>(units.size()));
    for (const UnitPresence& u : units) {
        w.i32(u.owner);
        w.f32(u.combat_power);
        w.u8(static_cast<uint8_t>(u.layer));
    }
    put_vec(w, structure_begin);
    put_vec(w, structure_power);
    w.u32(static_cast<uint32_t>(sees_orbit.size()));
    for (const galactic::Bitset& b : sees_orbit) save::put_bitset(w, b);
    w.u32(static_cast<uint32_t>(visibility.size()));
    for (const std::vector<uint32_t>& v : visibility) put_vec(w, v);
    for (const galactic::Bitset* b : {&revealed, &hidden, &in_battle, &ownership_visible, &local_orbit_visible})
        save::put_bitset(w, *b);
    w.boolean(multiplayer);
    w.boolean(fog_disabled);
    w.boolean(planet_auto_reveal);
}

bool GalacticSnapshot::read(ByteReader& r) {
    if (r.u32() != kSnapshotVersion || !r.ok()) return false;
    GalacticSnapshot s;
    s.planet_count = r.u32();
    s.players.resize(save::count(r, 4));
    for (PerceptionPlayer& p : s.players) {
        p.neutral = r.boolean();
        p.is_human = r.boolean();
        p.has_ai_system = r.boolean();
        p.benefits_from_corruption = r.boolean();
    }
    if (!r.ok() || !get_vec(r, s.relation) || !get_vec(r, s.owner) || !get_vec(r, s.political_control) ||
        !get_vec(r, s.starbase_level) || !get_vec(r, s.starbase_power) || !get_vec(r, s.corruption) ||
        !get_vec(r, s.unit_begin))
        return false;
    s.units.resize(save::count(r, 9));
    for (UnitPresence& u : s.units) {
        u.owner = r.i32();
        u.combat_power = r.f32();
        const uint8_t layer = r.u8();
        if (layer > static_cast<uint8_t>(UnitLayer::Ground)) r.fail();
        u.layer = static_cast<UnitLayer>(layer);
    }
    if (!r.ok() || !get_vec(r, s.structure_begin) || !get_vec(r, s.structure_power)) return false;
    s.sees_orbit.resize(save::count(r, 4));
    for (galactic::Bitset& b : s.sees_orbit)
        if (!save::get_bitset(r, b)) return false;
    s.visibility.resize(save::count(r, 4));
    for (std::vector<uint32_t>& v : s.visibility)
        if (!get_vec(r, v)) return false;
    for (galactic::Bitset* b : {&s.revealed, &s.hidden, &s.in_battle, &s.ownership_visible, &s.local_orbit_visible})
        if (!save::get_bitset(r, *b)) return false;
    s.multiplayer = r.boolean();
    s.fog_disabled = r.boolean();
    s.planet_auto_reveal = r.boolean();
    if (!r.ok()) return false;
    // The CSR tables index the unit / structure lists directly.
    auto csr_ok = [&s](const std::vector<uint32_t>& begin, size_t items) {
        if (begin.empty()) return true;
        if (begin.size() != static_cast<size_t>(s.planet_count) + 1 || begin.back() > items) return false;
        for (size_t i = 1; i < begin.size(); ++i)
            if (begin[i] < begin[i - 1]) return false;
        return true;
    };
    if (!csr_ok(s.unit_begin, s.units.size()) || !csr_ok(s.structure_begin, s.structure_power.size())) return false;
    *this = std::move(s);
    return true;
}

void GalacticPerception::write(ByteWriter& w) const {
    w.u32(kPerceptionVersion);
    w.u32(planets_);
    w.u32(frame_);
    put_vec(w, tracked_);
    put_vec(w, human_);
    w.u32(static_cast<uint32_t>(memory_.size()));
    for (const PerceptionMemory& m : memory_) {
        put_vec(w, m.seen);
        put_vec(w, m.last_seen);
        put_vec(w, m.owner);
        put_vec(w, m.space_total);
        put_vec(w, m.ground_total);
    }
    w.u32(static_cast<uint32_t>(results_.size()));
    for (const PlanetPerception& p : results_) {
        put_vec(w, p.space_total);
        put_vec(w, p.ground_total);
        put_vec(w, p.space_ok);
        put_vec(w, p.ground_ok);
        put_vec(w, p.has_space_force);
        put_vec(w, p.has_space_force_ok);
        put_vec(w, p.target_player);
        put_vec(w, p.target_player_ok);
        put_vec(w, p.node_value);
        put_vec(w, p.ground_value);
    }
    save::put_bitset(w, revealed_);
}

bool GalacticPerception::read(ByteReader& r) {
    if (r.u32() != kPerceptionVersion || !r.ok()) return false;
    const uint32_t planets = r.u32(), frame = r.u32();
    std::vector<uint8_t> tracked, human;
    if (!r.ok() || !get_vec(r, tracked) || !get_vec(r, human)) return false;
    std::vector<PerceptionMemory> memory(save::count(r, 20));
    for (PerceptionMemory& m : memory)
        if (!get_vec(r, m.seen) || !get_vec(r, m.last_seen) || !get_vec(r, m.owner) || !get_vec(r, m.space_total) ||
            !get_vec(r, m.ground_total))
            return false;
    std::vector<PlanetPerception> results(save::count(r, 40));
    for (PlanetPerception& p : results)
        if (!get_vec(r, p.space_total) || !get_vec(r, p.ground_total) || !get_vec(r, p.space_ok) || !get_vec(r, p.ground_ok) ||
            !get_vec(r, p.has_space_force) || !get_vec(r, p.has_space_force_ok) || !get_vec(r, p.target_player) ||
            !get_vec(r, p.target_player_ok) || !get_vec(r, p.node_value) || !get_vec(r, p.ground_value))
            return false;
    galactic::Bitset revealed;
    if (!save::get_bitset(r, revealed)) return false;
    if (!r.ok() || memory.size() != tracked.size() || results.size() != tracked.size()) return false;
    // A tracked player's memory covers every planet (observe() indexes it by planet).
    for (size_t p = 0; p < tracked.size(); ++p) {
        const PerceptionMemory& m = memory[p];
        if (tracked[p] && (m.seen.size() != planets || m.last_seen.size() != planets || m.owner.size() != planets ||
                           m.space_total.size() != planets || m.ground_total.size() != planets))
            return false;
        // Published results: never evaluated (empty) or one entry per planet in every list.
        const PlanetPerception& q = results[p];
        const size_t n = q.space_total.size();
        if ((n != 0 && n != planets) || q.ground_total.size() != n || q.space_ok.size() != n || q.ground_ok.size() != n ||
            q.has_space_force.size() != n || q.has_space_force_ok.size() != n || q.target_player.size() != n ||
            q.target_player_ok.size() != n || q.node_value.size() != n || q.ground_value.size() != n)
            return false;
    }
    planets_ = planets;
    frame_ = frame;
    tracked_ = std::move(tracked);
    human_ = std::move(human);
    memory_ = std::move(memory);
    results_ = std::move(results);
    revealed_ = std::move(revealed);
    return true;
}

}  // namespace alamo::ai

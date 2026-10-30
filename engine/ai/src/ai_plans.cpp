#include "alamo/ai/ai_plans.hpp"

#include <algorithm>
#include <cstdlib>

#include "alamo/core/sim_rng.hpp"

namespace alamo::ai {

bool TaskForceDef::percent_based() const {
    for (const TeamDef& t : teams)
        if (t.percent) return true;
    return false;
}

bool PlanDefinition::serves(uint32_t goal_type) const {
    return std::find(goal_types.begin(), goal_types.end(), goal_type) != goal_types.end();
}

uint32_t PlanLibrary::find(std::string_view name) const {
    for (size_t i = 0; i < plans.size(); ++i)
        if (ai_iequal(plans[i].name, ai_trim(name))) return static_cast<uint32_t>(i);
    return kAiNone;
}

void sort_plan_files(std::vector<std::string>& paths) {
    std::sort(paths.begin(), paths.end(),
              [](const std::string& a, const std::string& b) { return ai_upper_copy(a) < ai_upper_copy(b); });
}

namespace {

bool parse_int(std::string_view s, int32_t& out) {
    s = ai_trim(s);
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-' || s[0] == '+') neg = s[i++] == '-';
    if (i == s.size()) return false;
    int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > 1000000000) return false;
    }
    out = static_cast<int32_t>(neg ? -v : v);
    return true;
}

}  // namespace

bool parse_task_force(const std::vector<std::string>& strings, TaskForceDef& out, std::vector<std::string>* diag) {
    out = TaskForceDef{};
    if (strings.empty()) return false;
    out.name = std::string(ai_trim(strings[0]));
    if (out.name.empty()) return false;
    for (size_t i = 1; i < strings.size(); ++i) {
        const std::string_view s = ai_trim(strings[i]);
        if (s.empty()) continue;
        const size_t eq = s.find('=');
        if (eq == std::string_view::npos) {
            if (ai_iequal(s, "DenyHeroAttach")) out.deny_hero_attach = true;
            else if (ai_iequal(s, "EscortForce")) out.escort = true;
            else if (ai_iequal(s, "TaskForceRequired")) out.required = true;
            else if (ai_iequal(s, "DenySpecialWeaponAttach")) out.deny_special_weapon_attach = true;
            else if (ai_iequal(s, "RequireUniqueStage")) out.require_unique_stage = true;
            else if (ai_iequal(s, "RequireMatchingStage")) out.require_matching_stage = true;
            else if (s[0] == '-') out.excluded_types.emplace_back(ai_trim(s.substr(1)));
            else out.masks.emplace_back(s);
            continue;
        }
        const std::string_view lhs = ai_trim(s.substr(0, eq));
        const std::string_view rhs = ai_trim(s.substr(eq + 1));
        if (ai_iequal(lhs, "MinimumTotalSize")) {
            if (!parse_int(rhs, out.minimum_total_size) && diag) diag->push_back(out.name + ": bad MinimumTotalSize");
            continue;
        }
        if (ai_iequal(lhs, "MinimumTotalForce")) {
            if (!ai_parse_float(rhs, out.minimum_total_force) && diag) diag->push_back(out.name + ": bad MinimumTotalForce");
            continue;
        }
        TeamDef t;
        t.names = ai_split(lhs, "|");
        if (t.names.empty()) {
            if (diag) diag->push_back(out.name + ": team without names: " + std::string(s));
            continue;
        }
        bool ok = true;
        if (!rhs.empty() && rhs.back() == '%') {
            t.percent = true;
            ok = ai_parse_float(rhs.substr(0, rhs.size() - 1), t.percent_value);
        } else if (const size_t comma = rhs.find(','); comma != std::string_view::npos) {
            ok = parse_int(rhs.substr(0, comma), t.min) && parse_int(rhs.substr(comma + 1), t.max);
            if (ok && t.max == 0) t.max = t.min;  // "a,0" -> max a (§1.3)
        } else {
            ok = parse_int(rhs, t.min);
            t.max = t.min;
        }
        if (!ok) {
            if (diag) diag->push_back(out.name + ": bad team count: " + std::string(s));
            continue;
        }
        out.teams.push_back(std::move(t));
    }
    return true;
}

void renormalise_percents(PlanDefinition& plan) {
    float sum = 0.0f;
    for (const TaskForceDef& tf : plan.task_forces)
        for (const TeamDef& t : tf.teams)
            if (t.percent) sum += t.percent_value;
    if (!(sum > 0.0f)) return;
    for (TaskForceDef& tf : plan.task_forces)
        for (TeamDef& t : tf.teams)
            if (t.percent) t.percent_value = static_cast<float>(static_cast<int32_t>(t.percent_value / sum * 100.0f + 0.5f));
}

uint32_t pick_cumulative(std::span<const float> weights, SimRng& rng) {
    if (weights.empty()) return kAiNone;
    std::vector<float> cum(weights.size());
    float total = 0.0f;
    for (size_t i = 0; i < weights.size(); ++i) {
        total += weights[i];
        cum[i] = total;
    }
    const float r = rng.range_float(0.0f, total);
    const auto it = std::lower_bound(cum.begin(), cum.end(), r);
    return it == cum.end() ? static_cast<uint32_t>(cum.size() - 1) : static_cast<uint32_t>(it - cum.begin());
}

uint32_t pick_weighted(std::span<const double> success_ratio, SimRng& rng) {
    std::vector<float> w(success_ratio.size());
    for (size_t i = 0; i < w.size(); ++i) w[i] = static_cast<float>(success_ratio[i] + 1.0);
    return pick_cumulative(w, rng);
}

namespace {
constexpr PpMarshallerKind kGalacticOrder[] = {PpMarshallerKind::FreeStore, PpMarshallerKind::Budget,
                                               PpMarshallerKind::TechTree,  PpMarshallerKind::Plan,
                                               PpMarshallerKind::Production, PpMarshallerKind::Hero,
                                               PpMarshallerKind::TerrainType, PpMarshallerKind::Contrast,
                                               PpMarshallerKind::Survival};
constexpr PpMarshallerKind kLandOrder[] = {PpMarshallerKind::FreeStore, PpMarshallerKind::Plan,
                                           PpMarshallerKind::Reinforcement, PpMarshallerKind::TechTree,
                                           PpMarshallerKind::Hero, PpMarshallerKind::Contrast,
                                           PpMarshallerKind::TacticalBuild};
constexpr PpMarshallerKind kSpaceOrder[] = {PpMarshallerKind::FreeStore, PpMarshallerKind::TechTree,
                                            PpMarshallerKind::Plan, PpMarshallerKind::Reinforcement,
                                            PpMarshallerKind::Hero, PpMarshallerKind::Contrast,
                                            PpMarshallerKind::TacticalBuild};
constexpr float kNoScore = -kAiBigFloat;
}  // namespace

std::span<const PpMarshallerKind> pp_marshaller_order(AiMode mode) {
    switch (mode) {
        case AiMode::Galactic: return kGalacticOrder;
        case AiMode::Land: return kLandOrder;
        case AiMode::Space: return kSpaceOrder;
    }
    return {};
}

const char* pp_marshaller_name(PpMarshallerKind k) {
    switch (k) {
        case PpMarshallerKind::FreeStore: return "FreeStore";
        case PpMarshallerKind::Budget: return "Budget";
        case PpMarshallerKind::TechTree: return "TechTree";
        case PpMarshallerKind::Plan: return "Plan";
        case PpMarshallerKind::Production: return "Production";
        case PpMarshallerKind::Hero: return "Hero";
        case PpMarshallerKind::TerrainType: return "TerrainType";
        case PpMarshallerKind::Contrast: return "Contrast";
        case PpMarshallerKind::Survival: return "Survival";
        case PpMarshallerKind::Reinforcement: return "Reinforcement";
        case PpMarshallerKind::TacticalBuild: return "TacticalBuild";
    }
    return "?";
}

bool run_pp_solver(std::span<PpMarshaller* const> marshallers, const PpSolverIo& io, std::vector<uint32_t>& selection,
                   uint32_t max_iterations) {
    selection.clear();
    for (PpMarshaller* m : marshallers) m->setup_a();
    for (PpMarshaller* m : marshallers) m->setup_b();
    for (PpMarshaller* m : marshallers) m->setup_c();
    const size_t n = io.candidate_count ? io.candidate_count() : 0;
    std::vector<uint8_t> erased(n, 0);
    std::vector<float> score(n, kNoScore);
    std::vector<uint32_t> selected_types;
    for (uint32_t iter = 0; iter < max_iterations; ++iter) {
        // 1. Scores in candidate order, marshallers in order.
        for (size_t i = 0; i < n; ++i) {
            score[i] = kNoScore;
            if (erased[i]) continue;
            float s = 0.0f;
            bool veto = false;
            for (size_t k = 0; k < marshallers.size(); ++k) {
                PpMarshaller* m = marshallers[k];
                const float v = m->score(static_cast<uint32_t>(i));
                if (v == kAiBigFloat) {
                    veto = true;
                    if (m->permanent_veto()) erased[i] = 1;
                    if (io.on_veto) io.on_veto(static_cast<uint32_t>(i), k);
                    break;
                }
                s += v;
            }
            if (!veto) score[i] = s;
        }
        // 2. The 50 % rule (skipped while the selection is empty).
        if (!selected_types.empty()) {
            for (size_t i = 0; i < n; ++i) {
                if (!(score[i] > kNoScore)) continue;
                size_t viable = 0;
                for (size_t j = 0; j < n; ++j) viable += score[j] > kNoScore ? 1u : 0u;
                if (viable <= 1) break;  // the only viable one stays
                const uint32_t t = io.type_of(static_cast<uint32_t>(i));
                size_t same = 0;
                for (const uint32_t st : selected_types) same += st == t ? 1u : 0u;
                if (static_cast<float>(same) / static_cast<float>(selected_types.size()) >= 0.5f) score[i] = kNoScore;
            }
        }
        // 3. Pick: strictly greatest, ties to the lowest index.
        float best_score = kNoScore;
        uint32_t best = kAiNone;
        for (size_t i = 0; i < n; ++i)
            if (score[i] > best_score) {
                best_score = score[i];
                best = static_cast<uint32_t>(i);
            }
        if (best == kAiNone) break;
        // 4. Commit.
        selection.push_back(best);
        selected_types.push_back(io.type_of(best));
        if (io.on_select) io.on_select(best);
        for (PpMarshaller* m : marshallers) m->commit(best);
        // 5. ETA.
        if (io.after_commit) io.after_commit();
    }
    bool success = true;
    for (PpMarshaller* m : marshallers) m->finalize(success);
    return success;
}

}  // namespace alamo::ai

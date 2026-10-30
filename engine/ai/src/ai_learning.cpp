#include "alamo/ai/ai_learning.hpp"

#include "alamo/ai/ai_types.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::ai {

int64_t LearningSystem::expiry_of(float duration, int64_t now) {
    if (duration < 0.0f) return -1;
    return now + static_cast<int32_t>(static_cast<float>(kAiFps) * duration);
}

void LearningSystem::record_goal_outcome(uint32_t goal_type, int32_t target, uint32_t plan, bool success,
                                         float tracking_duration, int64_t now) {
    Counter& tt = type_target_[{goal_type, target}];
    Counter& t = type_[goal_type];
    ++tt.attempts;
    ++t.attempts;
    if (success) {
        ++tt.successes;
        ++t.successes;
    }
    if (plan != kAiNone) {
        Counter& p = plan_[plan];
        ++p.attempts;
        if (success) ++p.successes;
    }
    if (tracking_duration == 0.0f) return;
    const Record r{success, expiry_of(tracking_duration, now)};
    if (plan != kAiNone) recent_plan_[{plan, target}].push_back(r);
    recent_goal_[{goal_type, target}].push_back(r);
}

void LearningSystem::record_activation(uint32_t goal_type, int32_t target, bool success, float duration, int64_t now) {
    if (duration == 0.0f) return;
    recent_activation_[{goal_type, target}].push_back({success, expiry_of(duration, now)});
}

int32_t LearningSystem::failures(const std::map<Key, std::vector<Record>>& m, Key k, int64_t now) {
    const auto it = m.find(k);
    if (it == m.end()) return 0;
    int32_t n = 0;
    for (const Record& r : it->second)
        if (!r.success && (r.expiry < 0 || r.expiry > now)) ++n;
    return n;
}

int32_t LearningSystem::recent_goal_failures(uint32_t goal_type, int32_t target, int64_t now) const {
    return failures(recent_goal_, {goal_type, target}, now);
}
int32_t LearningSystem::recent_activation_failures(uint32_t goal_type, int32_t target, int64_t now) const {
    return failures(recent_activation_, {goal_type, target}, now);
}
int32_t LearningSystem::recent_plan_failures(uint32_t plan, int32_t target, int64_t now) const {
    return failures(recent_plan_, {plan, target}, now);
}

double LearningSystem::goal_type_success_ratio(uint32_t goal_type) const {
    const auto it = type_.find(goal_type);
    if (it == type_.end() || it->second.attempts == 0) return 1.0;
    return static_cast<double>(it->second.successes) / static_cast<double>(it->second.attempts);
}

double LearningSystem::plan_success_ratio(uint32_t plan) const {
    const auto it = plan_.find(plan);
    if (it == plan_.end() || it->second.attempts == 0) return 1.0;
    return static_cast<double>(it->second.successes) / static_cast<double>(it->second.attempts);
}

void LearningSystem::record_survival(uint32_t type_name_crc, bool survived) {
    Counter& c = survival_[type_name_crc];
    ++c.attempts;
    if (survived) ++c.successes;
}

double LearningSystem::survival_ratio(uint32_t type_name_crc) const {
    const auto it = survival_.find(type_name_crc);
    if (it == survival_.end() || it->second.attempts == 0) return 1.0;
    return static_cast<double>(it->second.successes) / static_cast<double>(it->second.attempts);
}

void LearningSystem::prune(int64_t now) {
    for (auto* m : {&recent_goal_, &recent_activation_, &recent_plan_})
        for (auto it = m->begin(); it != m->end();) {
            std::vector<Record>& v = it->second;
            size_t k = 0;
            while (k < v.size() && v[k].expiry >= 0 && v[k].expiry <= now) ++k;
            v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k));
            it = v.empty() ? m->erase(it) : std::next(it);
        }
}

void LearningSystem::hash(StateHasher& h) const {
    auto recent = [&h](const std::map<Key, std::vector<Record>>& m) {
        h.u32(static_cast<uint32_t>(m.size()));
        for (const auto& [k, v] : m) {
            h.u32(k.first);
            h.i32(k.second);
            h.u32(static_cast<uint32_t>(v.size()));
            for (const Record& r : v) {
                h.boolean(r.success);
                h.u64(static_cast<uint64_t>(r.expiry));
            }
        }
    };
    recent(recent_goal_);
    recent(recent_activation_);
    recent(recent_plan_);
    h.u32(static_cast<uint32_t>(type_target_.size()));
    for (const auto& [k, c] : type_target_) {
        h.u32(k.first);
        h.i32(k.second);
        h.u32(c.attempts);
        h.u32(c.successes);
    }
    for (const auto* m : {&type_, &plan_}) {
        h.u32(static_cast<uint32_t>(m->size()));
        for (const auto& [k, c] : *m) {
            h.u32(k);
            h.u32(c.attempts);
            h.u32(c.successes);
        }
    }
    if (!survival_.empty()) {  // STRATEGIC_GAPS §1.1 (absent until the first record: existing CRCs unchanged)
        h.u32(static_cast<uint32_t>(survival_.size()));
        for (const auto& [k, c] : survival_) {
            h.u32(k);
            h.u32(c.attempts);
            h.u32(c.successes);
        }
    }
}

}  // namespace alamo::ai

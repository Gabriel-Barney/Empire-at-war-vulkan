#include "alamo/ai/ai_templates.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"

namespace alamo::ai {

void TemplateSystem::init(const AiDefinitions& defs, const PlanLibrary& plans, std::vector<uint32_t> templates) {
    defs_ = &defs;
    plans_ = &plans;
    templates_ = std::move(templates);
    active_.clear();
    cursor_ = 0;
    const size_t ncat = defs.categories.size();
    goal_cat_.assign(ncat, {});
    plan_cat_.assign(ncat, {});
    goal_type_.assign(defs.goals.size(), {});
    plan_name_.assign(plans.plans.size(), {});
    budget_slot_.assign(ncat, {});
    allowance_slot_.assign(ncat, {});
    budget_eq_.assign(ncat, {});
    allowance_.assign(ncat, 0.0f);
    const uint32_t n = static_cast<uint32_t>(templates_.size());
    const auto c = static_cast<uint32_t>(static_cast<int32_t>(n) / (kAiFps * 10));
    count_ = c < 1 ? 1 : c;
}

void TemplateSystem::write(Slot& s, int32_t value, int32_t priority) {
    if (s.written && priority < s.priority) return;
    s.value = value;
    s.priority = priority;
    s.written = true;
}

bool TemplateSystem::triggers(uint32_t t, const Evaluator& eval) const {
    const TemplateDef& d = defs_->templates[t];
    if (d.trigger.empty()) return false;
    double v = 0.0;
    return eval(d.trigger, v) && v > 0.0;
}

bool TemplateSystem::untriggers(uint32_t t, const Evaluator& eval) const {
    const TemplateDef& d = defs_->templates[t];
    if (!d.untrigger.empty()) {
        double v = 0.0;
        return eval(d.untrigger, v) && v > 0.0;
    }
    return !triggers(t, eval);
}

bool TemplateSystem::is_active(uint32_t t) const { return std::find(active_.begin(), active_.end(), t) != active_.end(); }

void TemplateSystem::first_service(const Evaluator& eval) {
    active_.clear();
    if (!templates_.empty()) active_.push_back(templates_[0]);
    for (size_t i = 1; i < templates_.size(); ++i)
        if (!is_active(templates_[i]) && triggers(templates_[i], eval)) active_.push_back(templates_[i]);
    recompute();
    changed_ = true;
}

void TemplateSystem::service(const Evaluator& eval) {
    if (templates_.empty()) return;
    bool added = false, removed = false;
    const uint32_t n = static_cast<uint32_t>(templates_.size());
    for (uint32_t k = 0; k < count_; ++k) {
        const uint32_t t = templates_[cursor_ % n];
        cursor_ = (cursor_ + 1) % n;
        if (!is_active(t) && triggers(t, eval)) {
            active_.push_back(t);
            added = true;
        }
    }
    const uint32_t base = templates_[0];
    for (size_t i = 0; i < active_.size();) {
        if (active_[i] != base && untriggers(active_[i], eval)) {
            active_.erase(active_.begin() + static_cast<std::ptrdiff_t>(i));
            removed = true;
        } else {
            ++i;
        }
    }
    if (added || removed) {
        recompute();
        changed_ = true;
    }
}

void TemplateSystem::recompute() {
    for (auto* v : {&goal_cat_, &goal_type_, &plan_cat_, &plan_name_, &budget_slot_, &allowance_slot_})
        for (Slot& s : *v) s = Slot{};
    std::fill(budget_eq_.begin(), budget_eq_.end(), std::string());
    std::fill(allowance_.begin(), allowance_.end(), 0.0f);
    for (const uint32_t t : active_) {
        const TemplateDef& d = defs_->templates[t];
        const int32_t p = d.priority;
        for (const auto& [sw, sign] : {std::pair{&d.turn_on, 1}, std::pair{&d.turn_off, -1}}) {
            for (const int32_t c : sw->goal_categories) write(goal_cat_[static_cast<size_t>(c)], sign * p, p);
            for (const uint32_t g : sw->goal_types) write(goal_type_[g], sign * p, p);
            for (const int32_t c : sw->plan_categories) write(plan_cat_[static_cast<size_t>(c)], sign * p, p);
            for (const std::string& name : sw->plan_names) {
                const uint32_t pi = plans_->find(name);
                if (pi != kAiNone) write(plan_name_[pi], sign * p, p);
            }
        }
        for (const auto& [c, eq] : d.budget) {
            Slot& s = budget_slot_[static_cast<size_t>(c)];
            if (s.written && p < s.priority) continue;
            write(s, 1, p);
            budget_eq_[static_cast<size_t>(c)] = eq;
        }
        for (const auto& [c, v] : d.tactical_allowance) {
            Slot& s = allowance_slot_[static_cast<size_t>(c)];
            if (s.written && p < s.priority) continue;
            write(s, 1, p);
            allowance_[static_cast<size_t>(c)] = v;
        }
    }
}

int32_t TemplateSystem::goal_value(uint32_t goal_type) const {
    if (goal_type >= goal_type_.size()) return -1;
    if (goal_type_[goal_type].written) return goal_type_[goal_type].value;
    const int32_t c = defs_->goals[goal_type].category;
    if (c >= 0 && goal_cat_[static_cast<size_t>(c)].written) return goal_cat_[static_cast<size_t>(c)].value;
    return -1;
}

int32_t TemplateSystem::plan_value(uint32_t plan) const {
    if (plan >= plan_name_.size()) return -1;
    if (plan_name_[plan].written) return plan_name_[plan].value;
    int32_t best = -1;
    bool any = false;
    for (const uint32_t g : plans_->plans[plan].goal_types) {
        const int32_t c = defs_->goals[g].category;
        if (c < 0 || !plan_cat_[static_cast<size_t>(c)].written) continue;
        const int32_t v = plan_cat_[static_cast<size_t>(c)].value;
        if (!any || v > best) best = v;
        any = true;
    }
    return any ? best : -1;
}

const std::string& TemplateSystem::budget_equation(int32_t category) const {
    static const std::string kNone;
    if (category < 0 || static_cast<size_t>(category) >= budget_eq_.size()) return kNone;
    return budget_eq_[static_cast<size_t>(category)];
}

float TemplateSystem::tactical_allowance(int32_t category) const {
    if (category < 0 || static_cast<size_t>(category) >= allowance_.size()) return 0.0f;
    return allowance_[static_cast<size_t>(category)];
}

bool TemplateSystem::budget_listed(int32_t category) const {
    if (category < 0 || static_cast<size_t>(category) >= budget_slot_.size()) return false;
    const auto c = static_cast<size_t>(category);
    return budget_slot_[c].written || (c < allowance_slot_.size() && allowance_slot_[c].written);
}

void TemplateSystem::hash(StateHasher& h) const {
    h.u32(cursor_);
    h.u32(count_);
    h.u32(static_cast<uint32_t>(active_.size()));
    for (const uint32_t t : active_) h.u32(t);
}

}  // namespace alamo::ai

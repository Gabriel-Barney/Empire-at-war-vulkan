#include "alamo/ai/ai_context.hpp"

#include "alamo/core/profile.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::ai {

AiContext::AiContext(AiMode mode) : mode_(mode) {}

void AiContext::attach(ContextModeServices* services, TemplateSystem::Evaluator eval) {
    services_ = services;
    eval_ = std::move(eval);
}

int32_t AiContext::stagger_offset(int32_t slot, int32_t slots, int32_t period) {
    if (slots <= 1 || period <= 1 || slot <= 0) return 0;
    return static_cast<int32_t>((static_cast<int64_t>(slot) * period / slots) % period);
}

void AiContext::service(int64_t now, bool enabled, bool auto_resolve_busy) {
    if (!services_) return;
    if (!enabled) {
        if (mode_ == AiMode::Galactic && auto_resolve_busy && initialised_ && planning_clock.due(now)) {
            planning_clock.reschedule(now);
            ++service_counts[2];
            services_->planning_service(now);
        }
        return;
    }
    if (!initialised_) {
        services_->first_service(now);
        templates.first_service(eval_);
        services_->template_init(now);
        templates.clear_changed();
        initialised_ = true;
        // OPT2 t01: this player's fixed phase (header comment).
        for (ServiceClock* c : {&perception_clock, &planning_clock, &execution_clock, &learning_clock})
            c->next = now + stagger_offset(stagger_slot_, stagger_slots_, c->period());
        goal_clock.next = now + stagger_offset(stagger_slot_, stagger_slots_, planning_clock.period());
    }
    if (perception_clock.due(now)) {
        perception_clock.reschedule(now);
        ++service_counts[0];
        services_->perception_service(now);
    }
    if (goal_clock.due(now)) {
        goal_clock.reschedule(now);
        ++service_counts[1];
        ALAMO_PROFILE_SCOPE("ai.goals");  // OPT2 t01: per-subsystem AI time
        goals.service(now);
    }
    if (planning_clock.due(now)) {
        planning_clock.reschedule(now);
        ++service_counts[2];
        ALAMO_PROFILE_SCOPE("ai.planning");  // OPT2 t01: per-subsystem AI time
        services_->planning_service(now);
    }
    if (execution_clock.due(now)) {
        execution_clock.reschedule(now);
        ++service_counts[3];
        ALAMO_PROFILE_SCOPE("ai.execution");  // OPT2 t01: per-subsystem AI time
        services_->execution_service(now);
    }
    if (learning_clock.due(now)) {
        learning_clock.reschedule(now);
        ++service_counts[4];
        learning.prune(now);
    }
    if (template_clock.due(now)) {
        template_clock.reschedule(now);
        ++service_counts[5];
        ALAMO_PROFILE_SCOPE("ai.templates");  // OPT2 t01: per-subsystem AI time
        templates.service(eval_);
        if (templates.changed()) {
            templates.clear_changed();
            services_->templates_changed(now);
        }
    }
}

void AiContext::hash(StateHasher& h) const {
    h.u8(static_cast<uint8_t>(mode_));
    h.boolean(initialised_);
    h.i32(opponent);
    for (const ServiceClock* c : {&perception_clock, &goal_clock, &planning_clock, &execution_clock, &learning_clock,
                                  &template_clock})
        h.i64(c->next);
    goals.hash(h);
    templates.hash(h);
    learning.hash(h);
    budget.hash(h);
}

}  // namespace alamo::ai

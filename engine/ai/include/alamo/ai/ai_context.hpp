// AI framework: the per-mode context (TacticalAIManagerClass) and the enable gate (TheAIClass)
// (docs/re/AI_GALACTIC.md §1.1-1.3). Shared by galactic (galactic_ai.hpp) and the tactical AI (A1).
//
// ORDER (0x5f5ab0, every frame of the player's update, only for the context of the current top mode):
//   if the mode's AI is disabled / suspended: nothing, except galactic Planning while the auto-resolve is
//   busy and Planning is due [M];
//   first service (+0x69 == 0): opponent pick (mode), slot 6 of Perception, Goal, Planning, Execution,
//   Learning (mode first_service), then Template init (activations + budget init);
//   then Perception -> Goal -> Planning -> Execution -> Learning -> Template, each only when due
//   (next <= NOW), rescheduling first: next = NOW + max(1, (int)(interval x FPS)) [H].
// Intervals: Goal 0, Template 0 (every frame), Planning 0.1 s, Execution 0.1 s (every 3 frames),
// Perception 10 s (only reschedules), Learning 10 s (pruning) [H].
// NOW is the global logic frame. No wall clock is read anywhere (the prior patch's 6 ms pacing is not
// original and is not ported, §1.3).
//
// OPT2 t01 stagger [port]: in the original every AI player's Perception / Goal / Planning / Execution / Learning clocks
// start at 0, so all players plan on the same frames (every 3rd frame). set_stagger(slot, slots) gives a player a fixed
// tick offset from its AI ordinal (players in index order): on the first service each of these clocks gets its first
// due frame NOW + stagger_offset(slot, slots, period) (the Goal clock, which runs every frame, takes the Planning
// period: it shifts the first goal-proposal pass), after which the clocks reschedule as before, so a player keeps its
// phase. Slot 0 keeps the original timing. Tick numbers only; the clocks are saved, the stagger is the owner's setting.
#pragma once
#include <array>
#include <cstdint>

#include "alamo/ai/ai_budget.hpp"
#include "alamo/ai/ai_goals.hpp"
#include "alamo/ai/ai_learning.hpp"
#include "alamo/ai/ai_templates.hpp"
#include "alamo/ai/ai_types.hpp"

namespace alamo {
class ByteReader;
class ByteWriter;
class StateHasher;
}

namespace alamo::ai {

// The mode side of a context: Perception / Planning / Execution and the first-service hooks.
class ContextModeServices {
public:
    virtual ~ContextModeServices() = default;
    virtual void first_service(int64_t now) = 0;      // opponent pick + slot 6 of the subsystems
    virtual void template_init(int64_t now) = 0;      // after templates.first_service: budget init
    virtual void perception_service(int64_t now) = 0;
    virtual void planning_service(int64_t now) = 0;   // running plans (Lua threads)
    virtual void execution_service(int64_t now) = 0;  // blocking statuses, free store
    virtual void templates_changed(int64_t now) = 0;  // template switches re-derived this frame
};

class AiContext {
public:
    explicit AiContext(AiMode mode = AiMode::Galactic);
    AiMode mode() const { return mode_; }

    // Wires the subsystems. `eval` evaluates template triggers (context-bound perception).
    void attach(ContextModeServices* services, TemplateSystem::Evaluator eval);
    // One frame of the context (0x5f5ab0). `enabled` = TheAI enabled for the mode; `auto_resolve_busy` =
    // the galactic exception.
    void service(int64_t now, bool enabled, bool auto_resolve_busy);

    bool initialised() const { return initialised_; }
    // OPT2 t01: this player's AI ordinal and the AI player count (call before the first service).
    void set_stagger(int32_t slot, int32_t slots) {
        stagger_slot_ = slot;
        stagger_slots_ = slots;
    }
    // (slot x period / slots) mod period: the slots spread evenly over one period; 0 for slots <= 1 or period <= 1.
    static int32_t stagger_offset(int32_t slot, int32_t slots, int32_t period);
    int32_t opponent = -1;  // +0x30
    GoalSystem goals;
    TemplateSystem templates;
    LearningSystem learning;
    AiBudget budget;
    ServiceClock perception_clock{kPerceptionInterval}, goal_clock{kGoalInterval}, planning_clock{kPlanningInterval},
        execution_clock{kExecutionInterval}, learning_clock{kLearningInterval}, template_clock{kTemplateInterval};

    // Port diagnostics: subsystem services run (perception, goal, planning, execution, learning, template).
    std::array<uint64_t, 6> service_counts{};
    void hash(StateHasher& h) const;
    // N1 save / load: opponent, the six clocks' next frames, service counts, the first-service flag and the goal,
    // template, learning and budget systems. Not saved: the mode (constructor) and attach() wiring. read() leaves the
    // context unchanged on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    AiMode mode_;
    bool initialised_ = false;
    ContextModeServices* services_ = nullptr;
    TemplateSystem::Evaluator eval_;
    int32_t stagger_slot_ = 0, stagger_slots_ = 0;  // OPT2 t01
};

// TheAIClass (vt 0x8795f8): enable gate only (§1.1). Is-enabled = master && perMode[mode] [H]; Start_Game
// sets all four bytes to 1. Suspend_AI(x) sets master = !(x > 0) [H]. The galactic pause disables galactic
// AI on 0 -> 1 and re-enables it on 1 -> 0 [H].
class AiEnableGate {
public:
    void start_game() {
        master_ = true;
        per_mode_ = {true, true, true};
    }
    bool enabled(AiMode m) const { return master_ && per_mode_[static_cast<size_t>(m)]; }
    void set_mode_enabled(AiMode m, bool on) { per_mode_[static_cast<size_t>(m)] = on; }
    void suspend(bool suspended) { master_ = !suspended; }
    bool suspended() const { return !master_; }
    // Galactic pause counter edges.
    void on_galactic_pause(bool paused) { per_mode_[static_cast<size_t>(AiMode::Galactic)] = !paused; }
    void write(ByteWriter& w) const;  // N1 save / load: master + per-mode switches
    bool read(ByteReader& r);

private:
    bool master_ = true;
    std::array<bool, kAiModeCount> per_mode_{true, true, true};
};

}  // namespace alamo::ai

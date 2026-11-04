// AI framework: AITemplateSystemClass (docs/re/AI_GALACTIC.md §1.7).
//
// The player type's template list for the mode; element 0 is the base template: always active, never
// un-triggered. Trigger fires when its equation is > 0; un-trigger when Un_Trigger > 0 if present, else
// when Trigger <= 0 [H]. A failed evaluation counts as 0 for Trigger and as "not > 0" for Un_Trigger
// [port].
// Init (0x6bbed0): activate the base, then every other triggering template; per-frame check count
// max(1, (int)(N / (FPS x 10))) [H]. Service (every frame, 0x6bc5c0): check `count` templates round-robin
// from the cursor (activating triggering ones, appended to the active list), then every active non-base
// template (removing those that un-trigger) [H].
// Switch values: each active template writes, in active-list order, with p = its Priority: goal
// category / goal type switches and plan category / plan name switches as +p (Turn_On) or -p (Turn_Off),
// budget category equations and tactical allowances. A target keeps a value only if p >= its stored
// priority [H]. [port] Turn_On is written before Turn_Off; every change re-derives all switches from the
// active list in order (= the doc's reset + re-apply, and an activation appended last).
// Unwritten targets stay at -1 (off): a goal category no active template turns on is never proposed and
// a plan is a candidate only when its value is > 0 [M]. A goal type's value = its type switch when
// written, else its category's; a plan's = its name switch when written, else the largest written
// category switch over the categories of the goal types it serves [M].
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_plans.hpp"

namespace alamo {
class ByteReader;
class ByteWriter;
class StateHasher;
}

namespace alamo::ai {

class TemplateSystem {
public:
    // Evaluates a perception equation by name for the context (self, opponent, first human; no target).
    using Evaluator = std::function<bool(const std::string& equation, double& out)>;

    void init(const AiDefinitions& defs, const PlanLibrary& plans, std::vector<uint32_t> templates);
    void first_service(const Evaluator& eval);  // Init
    void service(const Evaluator& eval);        // every frame
    bool changed() const { return changed_; }   // switches changed by the last first_service/service
    void clear_changed() { changed_ = false; }

    int32_t goal_value(uint32_t goal_type) const;
    int32_t plan_value(uint32_t plan) const;
    const std::string& budget_equation(int32_t category) const;  // "" = none
    float tactical_allowance(int32_t category) const;
    // The category is in the budget map: an active template wrote a <Budget> or Tactical_Budget_Allowance entry for it
    // (0x615420 / 0x615550, AI_GALACTIC_TOKENS §9.3).
    bool budget_listed(int32_t category) const;
    const std::vector<uint32_t>& active() const { return active_; }  // template indices, activation order
    const std::vector<uint32_t>& templates() const { return templates_; }
    void hash(StateHasher& h) const;
    // N1 save / load: active list, cursor, count, changed flag; the switch slots are re-derived from the active list
    // (recompute, a pure function of it). read() needs init() with the same template list.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Slot {
        int32_t value = -1;
        int32_t priority = 0;
        bool written = false;
    };
    static void write(Slot& s, int32_t value, int32_t priority);
    bool triggers(uint32_t t, const Evaluator& eval) const;
    bool untriggers(uint32_t t, const Evaluator& eval) const;
    bool is_active(uint32_t t) const;
    void recompute();

    const AiDefinitions* defs_ = nullptr;
    const PlanLibrary* plans_ = nullptr;
    std::vector<uint32_t> templates_;  // AiDefinitions template indices; [0] = base
    std::vector<uint32_t> active_;
    uint32_t cursor_ = 0, count_ = 1;
    bool changed_ = false;
    std::vector<Slot> goal_cat_, goal_type_, plan_cat_, plan_name_;
    std::vector<Slot> budget_slot_, allowance_slot_;
    std::vector<std::string> budget_eq_;
    std::vector<float> allowance_;
};

}  // namespace alamo::ai

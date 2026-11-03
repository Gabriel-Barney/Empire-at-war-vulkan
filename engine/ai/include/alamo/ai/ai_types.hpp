// AI framework (roadmap A2, shared with the tactical AI A1): common enums, parsers and the service clock.
// docs/re/AI_GALACTIC.md §1 (framework); confidence tags are the doc's.
//
// Everything in engine/ai/ai_*.hpp is mode-agnostic framework (galactic, space and land contexts use it);
// galactic-only parts live in galactic_ai*.hpp. The framework is single-threaded sim code: it runs on the
// sim thread inside the PlayerList update (T7), players in index order, and draws from the synced RNG only
// in the documented order (§1.13).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace alamo::ai {

// FPS: the integer logic rate (global 0x140b0a340) [H].
inline constexpr int32_t kAiFps = 30;
inline constexpr uint32_t kAiNone = 0xFFFFFFFFu;
inline constexpr float kAiBigFloat = 1.0e18f;  // the solver veto / clamp value (BIG_FLOAT in PGBaseDefinitions)

// Context mode (+0x34): 0 Galactic, 1 Land, 2 Space [H].
enum class AiMode : uint8_t { Galactic = 0, Land = 1, Space = 2 };
inline constexpr uint32_t kAiModeCount = 3;
const char* ai_mode_name(AiMode m);
// "Galactic" / "Land" / "Space" (case-insensitive); false when unknown.
bool parse_ai_mode(std::string_view s, AiMode& out);

// AIGoalApplicationType bits (0x5e4f20) [H].
namespace app {
inline constexpr uint32_t Friendly = 0x1, Enemy = 0x2, Neutral = 0x4, Contested = 0x8, Global = 0x10,
                          FriendlyUnit = 0x20, EnemyUnit = 0x40, FriendlyStructure = 0x80, EnemyStructure = 0x100,
                          TacticalLocation = 0x200, FriendlyBuildPad = 0x400, EnemyBuildPad = 0x800,
                          FriendlyCashPoint = 0x1000, EnemyCashPoint = 0x2000, FriendlyReinforcementPoint = 0x4000,
                          EnemyReinforcementPoint = 0x8000;
}
// "Enemy | Neutral" -> bits (names case-insensitive, '|' / ',' / whitespace separated). Unknown names
// set *ok = false and are skipped.
uint32_t parse_application_flags(std::string_view s, bool* ok = nullptr);

// AIGoalReachabilityType values (0x5e70b0) [H; 1 and 8-10 M].
enum class Reachability : uint8_t {
    FriendlyOnly = 0, FriendlyIgnoreThreat = 1, EnemyDestination = 2, EnemyUndefended = 3, SingleHopDisconnected = 4,
    Any = 5, NoThreat = 6, LowThreat = 7, MediumThreat = 8, HighThreat = 9, AnyThreat = 10,
};
// Case-insensitive (shipped data writes Enemy_Undefended, Friendly_Only). False when unknown.
bool parse_reachability(std::string_view s, Reachability& out);

// InstantiatedGoalStateType (0x5e5890) [H].
enum class GoalState : uint8_t { Proposed = 0, Active = 1, Executing = 2, Released = 3 };

// Plan event code <-> name (DAT_140b3a208, AI_GALACTIC_2 §5.2) [H; code 0 M]. Handlers are <TF>_<Name>;
// "Fleet_Destoyed" keeps the exe's spelling.
enum class PlanEvent : uint8_t {
    SpaceConflictBegin = 0, SpaceConflictEnd = 1, UnitDestroyed = 2, FleetDestroyed = 3, ProductionFailed = 4,
    OriginalTargetOwnerChanged = 5, CurrentTargetOwnerChanged = 6, NoUnitsRemaining = 7, GroundConflictBegin = 8,
    GroundConflictEnd = 9, OriginalTargetDestroyed = 10, CurrentTargetDestroyed = 11, TargetInRange = 12,
    ReachedSystemOnPath = 13, UnitDamaged = 14, SpecialWeaponOnline = 15, HardpointTargetInRange = 16,
    HardpointOpportunityTargetAcquired = 17, UnitMoveFinished = 18, UnitAbilityFinished = 19,
    UnitDiversionFinished = 20, UnitAbilityCancelled = 21, UnitAbilityReady = 22, UnitNoThreat = 23,
};
inline constexpr uint32_t kPlanEventCount = 24;
std::string_view plan_event_name(PlanEvent e);
bool plan_event_code(std::string_view name, PlanEvent& out);  // case-insensitive

// ---- text helpers (ASCII, locale-free) ----
char ai_upper(char c);
bool ai_iequal(std::string_view a, std::string_view b);
std::string ai_upper_copy(std::string_view s);
std::string_view ai_trim(std::string_view s);
// Splits at any of `seps` (and trims); empty pieces dropped.
std::vector<std::string> ai_split(std::string_view s, std::string_view seps = "|, \t\r\n");
// Locale-free float / int parse of trimmed text; false on garbage.
bool ai_parse_float(std::string_view s, float& out);

// ---- ServicedAISystemClass schedule (§1.3) ----
// A subsystem runs when next <= NOW; Service first reschedules next = NOW + max(1, (int)(interval x FPS))
// (0x64c4d0) [H]. A fresh clock is due at once.
struct ServiceClock {
    float interval = 0.0f;  // seconds (slot 3)
    int64_t next = 0;       // absolute next-service frame (+0x30)
    bool due(int64_t now) const { return next <= now; }
    // Frames between two services: max(1, (int)(interval x FPS)).
    int32_t period() const {
        const auto frames = static_cast<int32_t>(interval * static_cast<float>(kAiFps));
        return frames < 1 ? 1 : frames;
    }
    void reschedule(int64_t now) { next = now + period(); }
};

// Subsystem intervals (slot 3) [H].
inline constexpr float kGoalInterval = 0.0f, kTemplateInterval = 0.0f, kPlanningInterval = 0.1f,
                       kExecutionInterval = 0.1f, kPerceptionInterval = 10.0f, kLearningInterval = 10.0f;

}  // namespace alamo::ai

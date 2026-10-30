// S3 galactic economy: constants and the pure formulas of docs/re/GALACTIC_ECONOMY.md (GE below).
// Everything here is a side-effect-free function of its arguments so each rule can be unit tested
// with hand-computed values; the stateful simulation (players, planets, queues, fleets) is
// alamo/galactic/campaign.hpp.
//
// Arithmetic contract (GE section 5.2): float32 everywhere, multiplied in the documented order,
// no FMA contraction (engine flags), no CRT transcendentals (only sqrt via dmath). Frames are 30 Hz
// mode frames; timers truncate int(30 * s), the fiscal period rounds half away from zero.
//
// Confidence tags in comments are the spec's: [H] read and cross-checked, [M] read but intent
// inferred, [L] inferred - needs a Q2 reference trace.
#pragma once
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/sim_rng.hpp"

namespace alamo::galactic {

// Force alignment enum (faction Force_Alignment, planet The_Force). GE 1.4: "enum 1 is presumably
// Neutral" [M]; Light = 0 and Dark = 2 are the port's numbering of the other two names.
enum class ForceSide : int8_t { Light = 0, Neutral = 1, Dark = 2 };
ForceSide parse_force_side(std::string_view text);  // "Light" / "Dark" (case-insensitive), else Neutral

// Corruption types 0..7 (GE 1.3 [M] index<->name): Intimidation, Piracy, Kidnapping, Racketeering,
// Corrupt_Militia, Bribery, Black_Market, Bonded_Citizens; -1 = Corruption_None.
inline constexpr int8_t kCorruptionNone = -1;
inline constexpr int8_t kCorruptionPiracy = 1;
inline constexpr int8_t kCorruptionRacketeering = 3;  // GE 4.2: the type a planet starts with [H]

// GameConstants curve "x0,y0, x1,y1, ...". GE 1.4 [M]: evaluated piecewise-linearly, clamped to the
// end values outside the x range.
struct PiecewiseCurve {
    std::vector<float> x, y;
    static PiecewiseCurve parse(std::string_view text);
    float eval(float v) const;
};

// GameConstants used by S3 (vanilla FoC values as defaults).
struct EconomyConstants {
    float fiscal_cycle_seconds = 45.0f;            // Fiscal_Cycle_Time_In_Secs
    float credit_cap_per_planet = 40000.0f;        // Credit_Cap_Per_Planet (+0x17c)
    float black_market_min = 1.0f;                 // Black_Market_Income_Mult_Min
    float black_market_max = 3.0f;                 // Black_Market_Income_Mult_Max
    std::array<float, 8> corruption_income_pct{0.5f, 0.8f, 0.4f, 4.5f, 0.5f, 0.5f, 0.8f, 1.0f};
    float credit_alignment_bonus = 0.5f;           // MaxCreditIncomeAlignmentBonus (+0x57c)
    float credit_alignment_penalty = 0.5f;         // MaxCreditIncomeAlignmentPenalty (+0x580)
    PiecewiseCurve political_income_curve{{0.0f, 1.0f, 5.0f}, {0.0f, 1.0f, 1.0f}};  // Political_Income_Curve
    int32_t max_build_queue = 5;                   // Max_Build_Queue: constructor default, not in vanilla XML [H]
    bool pay_as_you_go = false;                    // Pay_As_You_Go (humans; AI always PAYG) [H]
    float production_speed_factor = 1.0f;          // Production_Speed_Factor
    // Production_Speed_Mod_Base_Vs_Tech_0.._4 (_0 is never read, GE 2.3).
    std::array<float, 5> base_vs_tech{1.0f, 0.90f, 0.75f, 0.55f, 0.45f};
    int32_t max_ground_forces_on_planet = 10;      // Max_Ground_Forces_On_Planet (+0x10c8)
    float corruption_hyperspace_bonus = 1.5f;      // Corruption_Hyperspace_Bonus (+0x1213c)
    float political_control_change_seconds = 7.0f; // Political_Control_Change_Time_Seconds (+0x124)
    float influence_alignment_bonus = 0.5f;        // MaxInfluenceTransitionAlignmentBonus (+0x574)
    float influence_alignment_penalty = 0.5f;      // MaxInfluenceTransitionAlignmentPenalty (+0x578)
    // GE 3.1 step 4 / STRATEGIC_GAPS 2.1 [H]: the galactic mode's speed scale (object +0x2b8 -> +0x20 -> mode
    // +0x234) = Object_Max_Speed_Multiplier_Galactic (GameConstants +0x15BC; FoC 5.0, Thrawn's Revenge 11.34). The mode
    // default is 1.0 (mode ctor), kept for rules without the tag (hand-built tests). M = D * this * 1.0 (D = the debug
    // game-speed factor, 1.0 in release play), rewritten every mode tick (GalacticSim::fleet_speed_multiplier()).
    // [port] loaded but not applied while strategic.cpp's kApplyModeSpeedMultiplier is false (the original measures M = 5,
    // G1 2026-10-29; the flip moves shared goldens and waits for Gabriel); 1.0 for rules without the tag.
    float fleet_speed_multiplier = 1.0f;
    // GALACTIC_REVEAL_CORRUPTION 1.3 / 4.5: Planet_Reveal_Delay_Time (GC +0x11880; FoC 0.5; the default is [L]) and the
    // three Particle_*_Per_Corruption_Level lists (GC +0x11EE8 / +0x11F00 / +0x11F18; [0] uncorrupted, [1] corrupted;
    // a missing element reads 1.0).
    float planet_reveal_delay_seconds = 0.5f;
    std::vector<float> particle_brightness_per_level, particle_scale_per_level, particle_energy_per_level;
    // A2 galactic (AI_GALACTIC_3 §3.1): AI_Uses_Galactic_Pop_Cap (+0x11EBC), ctor false, absent from shipped XML.
    bool ai_uses_galactic_pop_cap = false;
    // REMAINING_GAPS 1.5 item 2: Good_Side_Name (+0x12e0; "REBEL" in FoC and TR): story BUILDABLE_UNIT raises this
    // faction's tech when no sliceable candidate remains.
    std::string good_side_name = "REBEL";
};

// Faction fields read by S3 (FactionFiles.xml).
struct FactionRules {
    std::string name;
    bool is_neutral = false;                // Is_Neutral (+0x108)
    bool can_control_planets = false;       // Can_Control_Planets (+0x109)
    bool is_playable = false;               // Is_Playable (+0x20a8)
    bool benefits_from_corruption = false;  // Benefits_From_Corruption (+0x10b)
    float hyperspace_speed_factor = 0.07f;  // Hyperspace_Speed_Factor (+0x3b0); default [L] (every vanilla faction sets 0.07)
    ForceSide force_alignment = ForceSide::Neutral;  // Force_Alignment (+0x110)
    std::array<float, 2> corruption_time_mult{1.0f, 1.0f};   // Corruption_Galactic_Production_Time_Multipliers
    std::array<float, 2> corruption_price_mult{1.0f, 1.0f};  // Corruption_Galactic_Production_Price_Multipliers
    int32_t base_population_capacity = 0;  // Base_Population_Capacity (+0x2EE8), not in shipped XML; default [L] 0
    int32_t displayed_tech_level_adjustment = 0;  // P4 sim: Displayed_Tech_Level_Adjustment (+0x2FB0; FoC Rebel +1)
    // REMAINING_GAPS 6.3 base-level info fallback: faction +0x20ac <= the base level -> the level is shown [M: field meaning
    // and XML tag unknown, not loaded]. Default INT32_MAX = the fallback never fires.
    int32_t info_base_level_threshold = INT32_MAX;
    // OI galactic: victory monitor inputs (STRATEGIC_GAPS 1.4). Names as written; indices resolved by
    // load_galactic_rules (kNoIndex = none). Offsets: Home_Planet +0x2AB8, Faction_Leader +0x2AC8,
    // Faction_Leader_Company +0x2AD8, Primary_Enemy +0x3D8 [M: tag <-> offset by field order].
    std::string home_planet, leader_name, leader_company_name, primary_enemy_name;
    uint32_t home_planet_type = 0xFFFFFFFFu;  // planet TypeIndex
    uint32_t leader = 0xFFFFFFFFu;            // Faction_Leader TypeIndex
    uint32_t leader_company = 0xFFFFFFFFu;    // Faction_Leader_Company TypeIndex
    uint32_t primary_enemy = 0xFFFFFFFFu;     // faction index
};

// DifficultyAdjustments.xml entry (AI players only). No adjustment = the internal all-1.0 one [H].
struct DifficultyRules {
    std::string name;
    float credit_multiplier = 1.0f;               // Credit_Multiplier
    float galactic_build_time_multiplier = 1.0f;  // Galactic_Build_Time_Multiplier (+0x28)
};

namespace econ {

inline constexpr int32_t kFramesPerSecond = 30;
inline constexpr int32_t kNoExpiry = std::numeric_limits<int32_t>::max();

// GE 1.2 step 3 [H]: round_half_away(30 * Fiscal_Cycle_Time_In_Secs) -> 1350 for 45 s.
int32_t fiscal_period_frames(float seconds);
// GE 2.3 [H]: timer duration = int(30 * T), truncated toward zero.
int32_t timer_frames(float seconds);
// GE 2.3 [H]: remaining seconds = max(0, start + duration - now) / 30.
float timer_remaining_seconds(int32_t start, int32_t duration, int32_t now);
// GE 2.3 [H]: due when start + duration <= now.
inline bool timer_due(int32_t start, int32_t duration, int32_t now) { return start + duration <= now; }

// ---- credits (GE 1.1) ----------------------------------------------------------------------------
struct CreditAccount {
    float credits = 0.0f;
    float cap = -1.0f;  // -1 = uncapped
};
// Add_Credits [H]: positive amounts of AI players are multiplied by Credit_Multiplier; no debt (clamp
// at 0); clamp at the cap when cap >= 0 ([L] for the mode test, assumed true in the campaign).
// Returns the amount actually applied.
float add_credits(CreditAccount& a, float amount, bool ai_controlled, float credit_multiplier);
// Cap [H]: max(1, planets owned) * Credit_Cap_Per_Planet.
float credit_cap(int32_t planets_owned, float cap_per_planet);

// ---- planet income terms (GE 1.4) -------------------------------------------------------------------
// Base [H]: float(b) * black-market multiplier.
float income_base(int32_t b, float black_market);
// Force alignment [H arithmetic, M enum]. *keep = false when the entry is removed (s == Neutral).
float income_force(int32_t b, ForceSide owner, ForceSide planet, float strength, float bonus, float penalty,
                   bool* keep);
// Political control [H]: q = curve(c); q > 0 -> q * b - b, else 0.
float income_political(int32_t b, float control, const PiecewiseCurve& curve);
// Trade routes [H]: float(b) * G (G = sum of qualifying Credit_Gain_Factor, link order).
float income_trade(int32_t b, float gain_sum);
// Clamp + siphon of steps 5-6 [H]: returns the net income; *siphoned = the amount taken out.
float income_finish(float sum, bool has_min, float min_value, bool has_max, float max_value, bool siphon,
                    float siphon_fraction, float* siphoned);
// Corruption income table lookup [H]: 0 out of range.
float corruption_income_pct(const EconomyConstants& c, int32_t type);

// ---- production (GE 2.3 / 2.4 / 2.5 / 2.6) ------------------------------------------------------------
struct BuildTimeInputs {
    int32_t build_time_seconds = 0;      // Build_Time_Seconds
    float production_speed_factor = 1.0f;
    float planet_bonus = 0.0f;           // m_planet = 1 - bonus [M]
    bool tech_factor_applies = false;    // unit-like type (cost >= 1, not a ground structure) [M]
    int32_t starbase_level = 0;          // planet starbase level L (used even for ground units) [H]
    int32_t tech_level = 0;              // type Tech_Level
    const std::array<float, 5>* base_vs_tech = nullptr;
    float ai_multiplier = 1.0f;          // Galactic_Build_Time_Multiplier for AI non-humans [H]
    float corruption_multiplier = 1.0f;  // faction time multiplier [corrupted] when not benefiting [H]
    int32_t factories = 1;               // n = max(1, matching special structures) when reduced [H]
    bool cheat = false;                  // player +0x3c0: T = 5.0 [M]
};
// T0 = float(uint32(float(Build_Time_Seconds) * factor)); T = m_planet*T0*m_tech*m_ai*m_corr / n [H].
float build_time_t0(int32_t build_time_seconds, float production_speed_factor);
float tech_time_factor(int32_t starbase_level, int32_t tech_level, const std::array<float, 5>& table);
float build_time(const BuildTimeInputs& in);
// Cost [H]: raw = m_planet_cost * float(cost) * m_corr * m_techupg; raw > 0 -> max(1, raw);
// round half away from zero to int.
int32_t build_cost(int32_t build_cost_credits, float planet_cost_mult, float corruption_price_mult,
                   float tech_upgrade_mult);
// PAYG installment [H]: (prev_remaining - remaining) / T * C.
float payg_installment(float prev_remaining, float remaining, float build_time, float cost);
// Refund [H]: up-front payer -> C; PAYG -> clamp(1 - remaining / T, 0, 1) * C.
float refund_amount(bool pay_as_you_go, float remaining, float build_time, float cost);

// ---- fleets (GE 3.1 / 3.2) ------------------------------------------------------------------------------
// Hop factor R [H]: 1.0; corruption bonus when the mover benefits and both ends are corrupted; the
// route's HS_Speed_Factor when there is a route and it is >= the current value.
float hop_factor(bool mover_benefits, bool a_corrupted, bool b_corrupted, bool has_route, float route_hs,
                 float corruption_bonus);
// v0 = s * Hyperspace_Speed_Factor * M [H, M is L]; hop speed v = v0 * R [H].
float base_speed(float slowest_hyperspace_speed, float faction_factor, float m);
inline float hop_speed(float v0, float r) { return v0 * r; }

struct Vec3f {
    float x = 0, y = 0, z = 0;
};
// One locomotor frame (GE 3.2) [H rule, M geometry]: velocity = unit(target - pos) * v; step; when the
// remaining distance after the step is < 2 * |velocity| snap to the target. Returns true on arrival.
// `facing` receives the unit direction (left unchanged when already at the target).
bool fleet_step(Vec3f& pos, const Vec3f& target, float v, Vec3f* facing);
// Frames a straight hop of `distance` takes at v (first k with D - k v < 2 v, float accumulation along
// one axis) - a closed-form helper for tests.
int32_t hop_frames_1d(float distance, float v);

// ---- political control (GE 4.2) ---------------------------------------------------------------------
// Delta = e / (30 * m * Political_Control_Change_Time_Seconds) [H].
float political_delta(int32_t elapsed_frames, float m, float change_seconds);
// Alignment factor of one source [H arithmetic]: matching -> 1 - k * bonus, opposed -> 1 + k * penalty,
// else 1 (owner or planet Neutral) [M].
float influence_alignment_factor(ForceSide owner, ForceSide planet, float strength, float bonus, float penalty);
// Moves control toward target by delta, clamped at the target.
float political_ramp(float control, float target, float delta);

}  // namespace econ
}  // namespace alamo::galactic

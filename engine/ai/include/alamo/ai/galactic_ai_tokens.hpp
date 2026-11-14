// Galactic AI: perception tokens read from the S3 GalacticSim (docs/re/AI_GALACTIC_TOKENS.md, AI_GALACTIC_2 §4;
// roadmap A2 stage 2).
//
// Every leaf follows the recovered rule of its evaluator (planet 0x65f7b0, planet force 0x6dcb70, galactic
// player 0x66b9f0 / player 0x6e09c0, galactic game 0x66e380 / game 0x6e2170 / game force 0x6e3d40):
//   * Leaf caches [H] (AI_GALACTIC_TOKENS §9.9, q6 t18): one cache per token node = (AI player's perception system,
//     evaluator instance = target planet / player / game, token path); an entry is keyed by the parameter set only
//     (compared by value, order-free; Parameter_Category of the force leaves as its mask); Self is the system's player,
//     Target / Enemy / Human are NOT in the key, so the contrast helper 0x5f2430, the equations, the normalisers and the
//     Game.<side> sums share entries. Hit iff stamp != 0 and (forever or NOW <= stamp + L x 30); stored after every
//     success, never pruned. Fogged leaves are also recomputed and re-stamped by the visibility signal 0xE
//     (visibility_event: presence fold on owner changes, galaxy setup, landings; the two conflict starts).
//   * Fog [H]: AIUsesFogOfWarGalactic is False in the shipped GameConstants, so every fogged leaf is live,
//     *.TimeLastSeen* = 0 and AverageAgeOf*Intelligence = 0 (that token fails for a non-Self root).
//   * Unbound roots fail [H]: Variable_Human when no human exists, Variable_Enemy without an opponent.
//   * Paths the original does not register fail: Target.IsContestable / HasBuiltObject / Force / bare
//     EnemyForce / FriendlyForce / bare TimeLastSeen*, Target.Hints.<not in Galactic_Hints>, Game.ForceVisibility.
//   * Force sides [H]: Enemy = owner hostile to Self; Friendly = Self (or an ally: not hostile, not neutral);
//     unowned / neutral matches neither.
// PORT STAND-INS (marked [port] next to the code): the per-tech member lists (0x373e30) of fleet units, starbases and
// structures are the types' Starting_Spawned_Units_Tech_N lists (galactic_ai_member_force); the force leaves hold
// members only (galactic_ai_planet_force), the helper 0x5f2430 adds the matching structures' own AI_Combat_Power to every
// contrast row (galactic_ai_enemy_strength, with the 0x4b8000 persisted built objects); the structure mask +0x1658 is
// Garrison_Category, Capital when 0 (galactic_ai_structure_mask); the planet-type garrison list +0x20C8 is not modelled;
// Timeline = 0; black market = GalacticSim's item table (P4 ai); Hints = the Galactic_Hints defaults (no Markup_Filename
// file is read); island paths are hop-count shortest paths.
// Determinism: refresh(frame) runs once per frame on the sim thread (event stamps, neighbour lists);
// resolve() reads the sim and the leaf cache (mutex) and draws no RNG (Script_ references may, through
// their Lua; they run only from serial sim-thread callers).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ai/galactic_perception.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/core/galaxy.hpp"

namespace alamo {
class ByteReader;
class ByteWriter;
}  // namespace alamo
namespace alamo::galactic {
class GalacticSim;
class ConflictManager;
}  // namespace alamo::galactic
namespace alamo::objects {
class EnumConverter;
}

namespace alamo::ai {

// One (type, count) entry of a Starting_Spawned_Units_Tech_N list (the type's "per-tech members", AI_GALACTIC_TOKENS §2).
struct SpawnMember {
    uint32_t type = 0;
    int32_t count = 0;
};
inline constexpr size_t kSpawnTechSlots = 6;  // Starting_Spawned_Units_Tech_0..5

// Per object type facts the galactic AI needs beyond galactic::TypeInfo (index = TypeIndex).
struct GalacticAiTypeInfo {
    std::vector<std::string> categories;  // CategoryMask tokens (upper-case)
    uint64_t category_mask = 0;           // GameObjectCategoryType bits of `categories` (filled from them)
    int32_t max_space_base = -1;          // planet types: Max_Space_Base (-1 = unknown -> 5)
    int32_t max_ground_base = -1;         // planet types: Max_Ground_Base (-1 = unknown -> 5)
    bool hero = false;
    bool transport = false;
    bool death_clone = false;  // IsDeathClone: never a task-force candidate [port]
    // A2 galactic (AI_GALACTIC_3):
    bool create_team = false;          // Create_Team (+0x67, §8): never a category-team candidate
    bool cinematic_only = false;       // Cinematic_Object_Only (+0x21, §8)
    bool transport_behavior = false;   // a TRANSPORT behaviour (behaviour 5: company transports, IG-2000): not buildable
    bool generic_hero = false;         // Is_Generic_Hero (+0x14, §2.1)
    bool special_weapon_space = false; // Is_Special_Weapon_In_Space (+0x1E, §2.1)
    bool lobbing_superweapon = false;  // LOBBING_SUPERWEAPON (0x37) in the Land behaviour set (0x370320(type, 0x37, 1))
    // Structure mask +0x1658: Garrison_Category (a company: its members' OR, OBJECT_TYPES §6.2), else
    // Space_Or_Garrison_Category [port: the original keeps the tag written last]; 0 = unset (galactic_ai_structure_mask
    // reads it as Capital).
    uint64_t garrison_mask = 0;
    std::string lua_script;            // Lua_Script (the per-object / type script name, §2.1)
    bool script_on_type = false;       // Execute_Script_On_Type (the type-level script instance exists, §2.1)
    uint32_t representative = 0xFFFFFFFFu;  // first Company_Units member, else first Squadron_Units member (0x6c00c0)
    bool retains_residual_influence = true;  // P4 ai: planet types' Planet_Retains_Residual_Influence (+0xBA, default true)
    // STRATEGIC_GAPS §1.1 / §1.2 / §1.5 [H]:
    bool squadron_member = false;  // flag +0x29: listed in some type's Squadron_Units (0x377500)
    bool company_member = false;   // flag +0x2A: listed in some type's Company_Units
    bool dummy_base = false;       // behaviour 0x5C DUMMY_STAR_BASE or 0x5B DUMMY_GROUND_BASE (generic base-level type)
    bool super_category = false;   // CategoryMask contains "Super" (0x375270: never MagicPlan-spawned)
    bool unit_ai = false;          // a UNIT_AI behaviour (object-removal survival feed filter, 0x3a8380)
    bool stealth_company = false;  // Is_Stealth_Company (+0x2B): AI delivery path class 5 (REMAINING_GAPS §6.2)
    uint32_t company_first = 0xFFFFFFFFu;  // first Company_Units member (survival leader substitution, §1.1)
    bool company_first_create_team = false;  // ... and that member has Create_Team (then no substitution)
    // Starting_Spawned_Units_Tech_0..5 (list order, repeated tags appended): the "per-tech members" a fleet unit, starbase or
    // ground structure adds to the force totals (AI_GALACTIC_TOKENS §2; 0x373f40 / 0x373e30 / 0x373d30).
    std::array<std::vector<SpawnMember>, kSpawnTechSlots> spawn_tech;
    // (terrain, restrictions, population: GalacticSim's A2 galactic queries / TypeInfo)
    bool has_category(std::string_view c) const;
};

// GameObjectCategoryType name -> bits; 0 if unknown. `categories` = the loaded enum (GameObjectTypes::enums(), so a mod's
// renumbered / renamed categories resolve as the game resolves them); null = the vanilla FoC table (objects::EnumRegistry
// fallback); a discarded converter converts nothing (0, OBJECT_TYPES §9.1, A2k t06). Vanilla data loads an enum equal to
// that table.
uint64_t ai_category_bit(std::string_view name, const objects::EnumConverter* categories = nullptr);
// Fills category_mask of every entry from its category names (same enum rule).
void fill_category_masks(std::vector<GalacticAiTypeInfo>& types, const objects::EnumConverter* categories = nullptr);
// The structure mask the force leaves test (A2I_TOKENS_RE §2.1 / §2.4) [H]: the type's +0x1658, or the Capital mask when
// that is 0 (Capital from `categories`, as ai_category_bit).
uint64_t galactic_ai_structure_mask(std::span<const GalacticAiTypeInfo> types, uint32_t type,
                                   const objects::EnumConverter* categories = nullptr);

inline constexpr uint64_t kAllCategories = ~uint64_t{0};

// The per-tech members of one TYPE held by `owner` (AI_GALACTIC_TOKENS §2 "per-tech members": 0x373f40 count /
// 0x373e30 type / 0x373d30 count, tech = the owner's tech level +0x84): Σ AI_Combat_Power x count over the members
// whose category mask meets `mask`. Never the type's own power.
//   [H] the list is the type's Starting_Spawned_Units_Tech_N (type +0xAD0 + 0x18 x N, A2F_RE_READS §2); the member's
//   own mask +0x1648 is tested. `exact` = false (planet-level leaves): the slot is the highest N <= the tech level with
//   entries (0x373f40 / 0x373e30 / 0x373d30); `exact` = true (player-level tokens 0x66b1c0 / 0x6682c0): list N only.
double galactic_ai_member_force(const galactic::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                                uint32_t type, int32_t owner, uint64_t mask, bool exact = false);

// Target.<side>.{Space,Ground}TotalUnnormalized of `planet` for Self = `self` (PlanetForce 0x6dbd50 / 0x6d95d0,
// AI_GALACTIC_TOKENS §2; side = Enemy (hostile to Self) or Friendly (Self or a non-neutral ally)), category `mask`
// (kAllCategories = no Parameter_Category):
//   * space: per orbiting, non-moving fleet of that side, per unit: its type's power if its mask meets `mask`, plus
//     its per-tech members; if the planet's owner is on that side: the starbase's per-tech members with its owner's
//     tech (NOT its own power), plus the own power of ground structures with Is_Special_Weapon_In_Space (+0x1E), then of
//     every orbital structure (DUMMY_ORBITAL_STRUCTURE, no +0x1E test), each when `mask` meets the structure mask
//     +0x1658 (galactic_ai_structure_mask: Capital when 0) (A2I_TOKENS_RE §2.1);
//   * ground: per surface unit of that side, its type's power if its mask meets `mask`; if the planet's owner is on
//     that side: the per-tech members of every ground structure (orbital ones skipped) with the structure owner's tech
//     (NOT their own power) [port: the Enemy planet-type list +0x20C8 not modelled].
// `categories`: the enum that resolves the Capital default (galactic_ai_structure_mask).
double galactic_ai_planet_force(const galactic::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                                PlanetIndex planet, int32_t self, bool enemy, bool space, uint64_t mask,
                                const objects::EnumConverter* categories = nullptr);
// The enemy strength at `planet` for AI player `self` (the perception helper 0x5f2430, AI_GALACTIC_TOKENS §9.2 [H]): the
// Enemy leaf above (members only) with the effective mask (`mask`, or every bit for 0), plus, when the planet is NOT held by
// `self` or an ally (0x4ad9f0: owner set, relation Self / Ally, control > 0; pirates, neutral and unowned planets pass), the
// own AI_Combat_Power of every structure whose category mask meets the effective mask: space = the planet's starbase type,
// ground = each ground structure (D+0x158; the port's orbital structures skipped). So a typed row gets it too (every shipped starbase is Capital: the Capital row).
// Ground also adds 0x4b8000: the AI_Combat_Power of every persisted tactical built object of the planet
// (PlanetState::built_objects, §9.4) whose type's category mask meets the effective mask, summed in float.
// q6 t18 (§9.6 / §9.9): with `leaves` set, the leaf part is read through that resolver's leaf cache
// (GalacticSimTokenResolver::enemy_force: the same entries the equations use, up to 15 s old); null = computed live.
class GalacticSimTokenResolver;
double galactic_ai_enemy_strength(const galactic::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                                  PlanetIndex planet, int32_t self, bool space, uint64_t mask,
                                  const objects::EnumConverter* categories = nullptr,
                                  const GalacticSimTokenResolver* leaves = nullptr);
// A2k t02: the hull fraction 0x396df0 a PotentialPlan's P = AI_Combat_Power x hull uses (A2K_T02_RE_ANSWERS Q2(b)) [H]:
// health / the type's Tactical_Health; 1.0 when the type has no maximum or the stored health is not > 0 ([port] an
// untyped store keeps health 0).
double galactic_ai_hull_fraction(float health, float max_health);
// a2m t09 (A2M_T08_T10_T14_RE.md t08) [H]: the AI clock leaves. spf = FrameSynchronizer +0x24 = 1.0f / (float)FPS, a
// float (0.033333335f). Game.Age (0x6e1f20) = (double)frame x (double)spf, one double multiply.
// TimeSinceCorruptionChange (0x65f110) = (double)(float)((float)(uint32)(frame - stamp) x spf): the 32-bit difference is
// converted as an unsigned value to float, multiplied in float, then widened. TimeSinceConversion / GroundConflict /
// SpaceConflict (0x65f0c0 / 0x65f160 / 0x65f1b0) = (double)1e18f for a stamp < 0, else (double)(int32)(frame - stamp) x
// (double)spf. Off by one frame against frame / 30 at threshold crossings (Age > 120 first holds at frame 3600).
inline constexpr float kAiSecondsPerFrame = 1.0f / 30.0f;
double galactic_ai_game_age(int32_t frame);
double galactic_ai_time_since_corruption_change(int32_t frame, int32_t stamp);
double galactic_ai_time_since_stamp(int32_t frame, int32_t stamp);
// One entry of the PotentialPlan Contrast requirement list (0x5f1990): `row` = the EnemyContrastTypes row, -1 for a layer
// total; `space` = layer byte 0 (space) / false = layer byte 1 (ground); `target` = the required strength.
struct GalacticContrastEntry {
    int32_t row = -1;
    bool space = false;
    double target = 0.0;
};
// 0x5f1990 (AI_GALACTIC_TOKENS §9.2 layer order, AI_GALACTIC_2 Contrast Setup A) [H]: entry 0 = the ground total (0x5f2430
// flag 1, mask 0), entry 1 = the space total (flag 0), then one row per `row_masks` entry for ground, then one per entry for
// space, each = galactic_ai_enemy_strength(row mask) x `scale`, or 0 when its layer total is <= 0. The totals are unscaled.
std::vector<GalacticContrastEntry> galactic_ai_contrast_list(const galactic::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                                                             PlanetIndex planet, int32_t self, std::span<const uint64_t> row_masks,
                                                             double scale, const objects::EnumConverter* categories = nullptr,
                                                             const GalacticSimTokenResolver* leaves = nullptr);
// The contrast marshaller's arithmetic (AI_GALACTIC_TOKENS §9.8, AI_GALACTIC_2 Contrast) [H]:
// - 0x5f1990 typed-row scale: ((double)MaxContrastScale + (double)(float)(f x PerFailureContrastAdjust)) x (double)multiplier
//   (`failure_adjust` = the float product f x PFCA).
double galactic_contrast_scale(float max_contrast_scale, float failure_adjust, float multiplier);
// - 0x72ac70 satisfied test, in float: total = Σ (float)target over the layer's typed entries (row >= 0); residual =
//   Σ (rem / target) x (target / total) over those with target > 0 and rem > 0; satisfied <=> residual <= tolerance.
bool galactic_contrast_layer_satisfied(std::span<const GalacticContrastEntry> entries, std::span<const double> remaining,
                                       bool space, float tolerance);
// - 0x72b940 "still allowed" rule after v <= 0: an untyped total (row -1) still has remaining > 0 and the candidate covers
//   that entry's layer (layer byte 1 ground -> rec+0x90, byte 0 space -> rec+0x91). Found by layer byte, not position.
bool galactic_contrast_total_open(std::span<const GalacticContrastEntry> entries, std::span<const double> remaining,
                                  bool covers_space, bool covers_ground);
// - 0x72b940 gate: the "still allowed" checks run only when the plan-wide flag 0x140b3a528 (every active layer satisfied,
//   0x6bff00) is set and the candidate has a non-escort team (rec+0xB0), or 0x140b3a529 and an escort team (rec+0xB1);
//   otherwise v <= 0 is a veto.
constexpr bool galactic_contrast_gate(bool main_met, bool escort_met, bool non_escort, bool escort) {
    return (main_met && non_escort) || (escort_met && escort);
}
// - The escort flag 0x140b3a529 (§9.10 [H]): galactic Setup A empties the escort target list and clears 0x140b3a525, so
//   Setup C and every commit recompute (0x6bff00 / 0x6c4b20) find an empty escort remaining list, which always passes:
//   the flag is 1 from Setup C to the end of every non-IgnoreTarget solve, whatever the main contrast. IgnoreTarget plans
//   skip Setup C and the commit recompute returns early, so both flags stay 0.
constexpr bool galactic_escort_met(bool ignore_target) { return !ignore_target; }
// - Place 0x729830, first pass (§9.10, AI_GALACTIC_2 §1.4 corrected): a non-escort slot is skipped when 0x528 && !0x529,
//   an escort slot when 0x529 && !0x528. The second pass ignores the flags.
constexpr bool galactic_place_skip(bool main_met, bool escort_met, bool escort_slot) {
    return escort_slot ? (escort_met && !main_met) : (main_met && !escort_met);
}

// The galactic AI's planning-side buildability: GalacticSim::buildable (cost, queue kind, GalacticSim::build_check =
// 0x2804d0 in the STRATEGIC_GAPS §1.3 order, no credits).
bool galactic_ai_can_build(const galactic::GalacticSim& sim, int32_t player, uint32_t type, PlanetIndex planet);
// Player-level test (0x2ac320 / affiliation + tech, not production-disabled): the type could be built somewhere by
// this player.
bool galactic_ai_player_can_build(const galactic::GalacticSim& sim, int32_t player, uint32_t type);

// AI path class of a produced unit's delivery (ProductionBehaviorClass 0x6955a0 / 0x695c90 from the build market
// 0x53cd60, REMAINING_GAPS §6.2 [H readers]): 5 (Reachability::Any) for an Is_Stealth_Company type [M meaning: the
// path ignores enemy-held / neutral planets], else 3 (Enemy_Undefended) with the task flag +0xDA, else 2
// (Enemy_Destination).
int galactic_ai_delivery_mode(bool stealth_company, bool task_flag_da);
// The market's path test from `from` in that AI mode (vfunc +0x28) [port]: the reachability flood ai_reach_flood
// (ai_free_store_path.hpp) from `from` with the corrected AI request flags of the mode (FIXPASS_GALACTIC §1.4); every
// mode used here has flag 4, so a reached planet that is not a transit planet is accepted but not expanded. Danger =
// `dangerous` (the AI's Is_Planet_Dangerous; null = none). Result[p] != 0 = reachable; all 0 for a bad source.
std::vector<uint8_t> galactic_ai_delivery_flood(const galactic::GalacticSim& sim, int32_t owner, PlanetIndex from, int mode,
                                                const galactic::Bitset* dangerous = nullptr);

// Snapshot of a GalacticSim for the S2 perception (owners, starbases, units by layer). `ai_uses_fog_galactic` =
// GameConstants AIUsesFogOfWarGalactic (shipped False: every AI sees every planet live). With it on, each AI player's
// remembered owner / space / ground values refresh only while its GRC 2 presence level (GalacticSim::visibility_sources
// .presence, the signal 0xE value) covers the leaf mask (GalacticSnapshot::visibility, kVisMask*). The local player's
// folded visibility mask fills ownership_visible (bit 0x1) and local_orbit_visible (bit 0x10).
GalacticSnapshot snapshot_from_sim(const galactic::GalacticSim& sim, bool ai_uses_fog_galactic = false);

class GalacticSimTokenResolver final : public TokenResolver {
public:
    // The AI build task of a planet's AIPlanetBuildQueue (0x559b40(planet +0xD0, queue) +0x18) as BudgetFractionToBuild
    // 0x667120 reads it (A2H_GALACTIC_AI_RE §2 step 4).
    struct QueueTask {
        enum class Kind : uint8_t {
            None,            // no task: the token returns 0
            Ended,           // ended (+0xA8), not reclaimed: the entry is ignored
            EndedReclaimed,  // ended and taken by the PAYG reclaim (+0xD9): the token returns 0
            Live,            // a running task: its goal's budget entry below
        };
        Kind kind = Kind::None;
        bool budget = false;  // the goal has a BudgetedGoalStruct (else alloc = a = c = h = 0)
        float alloc = 0.0f, a = 0.0f, c = 0.0f, h = 0.0f;  // +0x2C, +0x38, +0x3C, +0x30
    };
    struct Hooks {
        // Variable_<root>.ActiveGoals (0x6e0490, A2H_GALACTIC_AI_RE §1, A2I_TOKENS_RE §3.3): `category` = the one
        // Parameter_Category name (the build-time converted AIGoalCategoryType literal); out = the size of the root
        // player's active-set vector of the category with that name's value (0 when none). false (the token fails) for a
        // name the definitions lack or a player without a galactic AI.
        std::function<bool(int32_t player, std::string_view category, int32_t& out)> active_goals;
        bool story_campaign = false;                          // Game.IsStoryCampaign
        // Apply_Markup accumulator of `player`'s AI target for `planet` (+0x80).
        std::function<double(int32_t player, int32_t planet)> markup;
        // AI CreditsUnnormalized: the galactic budget total (false = no galactic AI for that player).
        std::function<bool(int32_t player, double& out)> ai_credits;
        // BudgetFractionToBuild's rate and pool (0x667120 step 5): `category` "" = budget +0x68 and the player's credits;
        // else the category's income rate I (+0x50) and pool P (+0x4C), 0 / 0 for a name without a budget entry.
        // false = no goal system (the token fails).
        std::function<bool(int32_t player, std::string_view category, float& rate, float& pool)> budget;
        // The AI build task of `planet`'s queue `queue` (0 ground, 1 space) for BudgetFractionToBuild (null = None).
        std::function<QueueTask(int32_t player, int32_t planet, uint32_t queue)> queue_task;
        // AI difficulty index (0 Easy, 1 Normal, 2 Hard; -1 = no AI system).
        std::function<int32_t(int32_t player)> difficulty;
        // Script_<name>.Evaluate (AI_GALACTIC_2 §4.4); null = fails.
        std::function<bool(const EvalContext&, std::string_view name, std::span<const Param>, double&)> script;
        // Variable_Self.StoryTrigger{Parameter_Name} (story flag); null = 0.
        std::function<bool(int32_t player, std::string_view name)> story_flag;
        // q6 t03 BudgetAllocation (AI_GALACTIC_TOKENS §8.3): the normalised weight (+0x3C) of `player`'s budget category
        // `key` (an AIGoalCategoryType value), 0.0 for a key the budget does not hold; false = no goal system (fails).
        std::function<bool(int32_t player, int32_t key, double& out)> budget_weight;
        // AIGoalCategoryType name -> value (the Parameter_Category enum of BudgetAllocation); -1 = unknown.
        std::function<int32_t(std::string_view name)> goal_category;
    };
    GalacticSimTokenResolver(const galactic::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                             const TokenResolver* s2, Hooks hooks);

    // Once per frame before evaluating: event stamps (signals 0x12 / 0x14 / 0x1A, corruption), neighbour lists.
    // `logic_frame` = the global logic frame (FrameSynchronizer +0, the host frame; it keeps counting while a battle
    // runs), the clock of the player TimeSince*er tokens; < 0 = `frame`. q6 t03: the player conflict stamps (signals 0x11 /
    // 0x13 of AI_GALACTIC_TOKENS §8.2) are taken after it by observe_conflicts.
    void refresh(int32_t frame, int64_t logic_frame = -1);
    void set_s2(const TokenResolver* s2) { s2_ = s2; }
    void set_hooks(Hooks hooks) { hooks_ = std::move(hooks); }
    // The loaded GameObjectCategoryType enum for Parameter_Category masks and the Capital default (null = vanilla table).
    void set_category_enum(const objects::EnumConverter* categories) { categories_ = categories; }
    void clear_cache();

    // q6 t03 (AI_GALACTIC_TOKENS §8.2): one pass of the 0x11 / 0x13 listener 0x66d490 over the conflict manager's state
    // since the last pass, stamping `now` (the global logic frame). The port has no planet signal, so it reads the
    // manager after its service (T5, before the AI's T7):
    //   * 0x11 (space conflict head started or dropped): a space record that became the active conflict through the state
    //     machine (Idle -> Decide; a story-linked battle bypasses it and is not stamped), and every space log entry that
    //     was not such an observed start: a drop (how 0, validation failure: the original sends 0x11 before the head goes)
    //     or a conflict that started and ended inside one service (AI vs AI auto-resolve, a refused battle start).
    //   * 0x13 (land auto-resolve began, AutoResolveClass vf3): an active land record in AutoRunning, and a land
    //     auto-resolve log entry (how 1) of a record not seen in that state or in a tactical battle (the tactical HUD's
    //     auto-resolve, BT §4.5, does not run vf3) [M].
    // Per stamped record: a player whose id is the defender gets the defender stamp, else the attacker the attacker stamp.
    // [port] Active kind while not InBattle: the space head unless a space entry was logged in this pass or the space queue
    // is empty (the service runs the space head first); a cancelled-by-transfer drop (on_service_tail) is not told apart
    // from a validation drop and stamps too.
    //   q6 t18 (§9.9): the same pass raises the conflict visibility signals 0xE: space conflict state 4 -> mask 0x70, land
    //   conflict state -1 -> mask 0x180, for the record's attacker when it has an AI system (visibility_event).
    void observe_conflicts(const galactic::ConflictManager& conflicts, int32_t now);
    // Frame stamps of `player` (-1 = never): 0 space defender, 1 space attacker, 2 ground defender, 3 ground attacker.
    int32_t player_stamp(int which, int32_t player) const;

    // q6 t18 (AI_GALACTIC_TOKENS §9.9 [H]): visibility signal 0xE of `planet` for `player` (an AI player's perception
    // system) carrying `mask`. Every fogged leaf node of that planet's evaluators (the planet's own, then FriendlyForce, then
    // EnemyForce, each in token-table order [M]) with leaf mask M: if (mask & M) == M -> recompute every entry and set
    // visible; else if visible -> recompute and clear visible; else nothing. A node with more than 100 entries is not
    // recomputed. Recompute = the leaf's computation with Self = `player` and the entry's own parameters; a success
    // overwrites the value and re-stamps it with NOW (a failure leaves the entry). refresh() raises it for the presence
    // fold (GalacticSim visibility presence level) of every AI player at the first refresh (galaxy setup), on every planet
    // owner change, and for the owner of every unit new on a planet's surface (a landing [M: also a surface spawn]).
    void visibility_event(PlanetIndex planet, int32_t player, uint32_t mask);
    // The perception leaf Target.EnemyForce.<Space|Ground>TotalUnnormalized of `planet` for AI player `self` through the leaf
    // cache, as 0x5f2430 binds it (§9.6): mask 0 = no parameter, else Parameter_Category = mask. False when it fails.
    bool enemy_force(int32_t self, PlanetIndex planet, bool space, uint64_t mask, double& out) const;
    // Entries currently cached (tests / diagnostics) and the visible flags (bits = the fogged leaf masks M) of a node set.
    size_t cache_size() const;
    uint32_t visible_flags(PlanetIndex planet, int32_t player) const;

    bool resolve(const EvalContext& ctx, const TokenRef& token, double& out) const override;
    bool implements(Root root, std::span<const TokenId> path, const TokenTable& tokens) const override;
    bool resolve_script(const EvalContext& ctx, std::string_view name, std::span<const Param> params,
                        double& out) const override;

    uint64_t cache_hits() const;

    // N1 save / load (version 3): frame, the per-planet event stamps and last-seen values, the leaf cache (key order; key
    // = leaf, instance, system player, parameter key, and every entry keeps its parameters for the 0xE recompute), the
    // hit counter, the logic frame, the per-player conflict stamps, the conflict observer (log position, active record),
    // the visible flags and the last seen surface units. read() accepts only that version, like the other AI chunks
    // (the version 1 / 2 images were dropped in cu6; the 2026-11-05 kGoalsVersion 2 -> 3 bump had already made saves of
    // that time unloadable). The neighbour lists are rebuilt from the sim (static map data). read() fails (unchanged)
    // unless the saved planet count is 0 (never refreshed) or the sim's.
    // A load is a restore: it raises no visibility signal (the original refreshes every object at a load) [port].
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

    // One token node's cache entry key (§9.9): the node = (leaf, inst, self), the entry = the parameter set.
    struct CacheKey {
        std::string leaf;  // root kind + leaf path
        int32_t inst = -1;  // evaluator instance: planet index (Target / Location roots), player id, -1 = Game
        int32_t self = -1;  // the perception system's AI player (Self)
        std::string params;  // canonical parameter set (sorted; see param key rules in the .cpp)
        friend auto operator<=>(const CacheKey&, const CacheKey&) = default;
        friend bool operator==(const CacheKey&, const CacheKey&) = default;
    };

    struct Impl;  // evaluation helpers (galactic_ai_tokens.cpp)

private:
    friend struct Impl;
    void build_neighbours();  // planet evaluator neighbour lists (0x661da0)
    const galactic::GalacticSim* sim_;
    std::span<const GalacticAiTypeInfo> types_;
    const TokenResolver* s2_;
    Hooks hooks_;
    const objects::EnumConverter* categories_ = nullptr;
    int32_t frame_ = 0;
    int32_t logic_frame_ = 0;  // global logic frame of the last refresh (TimeSince*er clock)
    // q6 t03 per player conflict stamps (GalacticPlayer evaluator +0xA8 / +0xB0 / +0xAC / +0xB4; -1 = never): index 0 space
    // defender, 1 space attacker, 2 ground defender, 3 ground attacker. One set for every AI player's evaluators (they all
    // hear the same planet signals, §8.2 "Scope") [M: an evaluator created later has missed earlier events; not modelled].
    std::array<std::vector<int32_t>, 4> player_stamps_;
    struct ObservedConflict {  // the conflict observer's view of the active conflict
        bool valid = false;
        uint8_t kind = 0;  // galactic::ConflictKind
        uint32_t planet = 0xFFFFFFFFu;
        int32_t attacker = -1, defender = -1;
        bool linked = false, space_stamped = false, ground_stamped = false, in_battle = false;
    };
    ObservedConflict active_;
    static constexpr uint32_t kLogUnseen = 0xFFFFFFFFu;  // no pass yet since a version-1 load: re-synchronise
    uint32_t conflict_log_seen_ = 0;                     // conflict log entries already observed
    void stamp_players(bool space, int32_t attacker, int32_t defender, int32_t now);
    // per planet event stamps (frames, -1 = never): space conflict, ground conflict, conversion, corruption change
    std::vector<int32_t> stamp_space_, stamp_ground_, stamp_conversion_, stamp_corruption_;
    std::vector<int32_t> last_owner_;
    std::vector<int8_t> last_corruption_;
    std::vector<uint8_t> last_battle_;
    std::vector<std::vector<uint32_t>> neighbours_;  // planet evaluator list (self excluded), planet order
    // q6 t18 visibility signal 0xE state: per player, per planet, the visible flags of the fogged leaf nodes (bit M set =
    // the nodes with leaf mask M are visible; every fogged mask is one bit, and nodes with the same mask hear the same
    // events, so one word holds them all); and per planet the ObjectIds of the surface units seen at the last refresh
    // (sorted), from which landings are found.
    std::vector<std::vector<uint32_t>> visible_;
    std::vector<std::vector<int32_t>> last_surface_;
    void raise_presence(PlanetIndex planet, int32_t player);  // 0xE with the presence fold's level
    struct Entry {
        double value = 0.0;
        int32_t stamp = 0;
        std::vector<Param> params;  // the bindings copy (0x64c840): what the 0xE recompute evaluates with
    };
    mutable std::mutex mu_;
    mutable std::map<CacheKey, Entry> cache_;
    mutable uint64_t hits_ = 0;
};

}  // namespace alamo::ai

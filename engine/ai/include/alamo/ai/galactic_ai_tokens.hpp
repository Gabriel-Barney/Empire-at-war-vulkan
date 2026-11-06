// Galactic AI: perception tokens read from the S3 GalacticSim (docs/re/AI_GALACTIC_TOKENS.md, AI_GALACTIC_2 §4;
// roadmap A2 stage 2).
//
// Every leaf follows the recovered rule of its evaluator (planet 0x65f7b0, planet force 0x6dcb70, galactic
// player 0x66b9f0 / player 0x6e09c0, galactic game 0x66e380 / game 0x6e2170 / game force 0x6e3d40):
//   * Leaf caches [H]: each leaf has its lifetime in seconds (-1 forever, 0 none); an entry is keyed by the
//     leaf, its evaluator instance, the bindings (Self / Target / Enemy / Human / Location) and the exact
//     parameters; hit iff stamp != 0 and (forever or NOW <= stamp + L x 30); stored after every success.
//   * Fog [H]: AIUsesFogOfWarGalactic is False in the shipped GameConstants, so every fogged leaf is live,
//     *.TimeLastSeen* = 0 and AverageAgeOf*Intelligence = 0 (that token fails for a non-Self root).
//   * Unbound roots fail [H]: Variable_Human when no human exists, Variable_Enemy without an opponent.
//   * Paths the original does not register fail: Target.IsContestable / HasBuiltObject / Force / bare
//     EnemyForce / FriendlyForce / bare TimeLastSeen*, Target.Hints.<not in Galactic_Hints>, Game.ForceVisibility.
//   * Force sides [H]: Enemy = owner hostile to Self; Friendly = Self (or an ally: not hostile, not neutral);
//     unowned / neutral matches neither.
// PORT STAND-INS (marked [port] next to the code): the per-tech member lists (0x373e30) of fleet units, starbases and
// structures are the types' Starting_Spawned_Units_Tech_N lists (galactic_ai_member_force); the force leaves hold
// members only (galactic_ai_planet_force), the helper 0x5f2430 adds the structures' own AI_Combat_Power to the
// untyped totals (galactic_ai_enemy_strength); the structure mask +0x1658 is the Capital default (no galactic structure
// sets it); orbital structures D+0x170 and the planet-type garrison list +0x20C8 are not modelled; Timeline = 0; black
// market = GalacticSim's item table (P4 ai); Hints = the Galactic_Hints defaults (no Markup_Filename file is read);
// island paths are hop-count shortest paths.
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

// GameObjectCategoryType name -> bits (vanilla FoC table, objects::EnumRegistry fallback); 0 if unknown.
uint64_t ai_category_bit(std::string_view name);
// Fills category_mask of every entry from its category names.
void fill_category_masks(std::vector<GalacticAiTypeInfo>& types);

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
//     its per-tech members; if the planet's owner is on that side: the starbase's per-tech members (NOT its own
//     power), plus the own power of ground structures with Is_Special_Weapon_In_Space (+0x1E) when `mask` meets the
//     structure mask +0x1658 (Capital default) [port: orbital structures D+0x170 not modelled];
//   * ground: per surface unit of that side, its type's power if its mask meets `mask`; if the planet's owner is on
//     that side: the per-tech members of every ground structure (NOT their own power) [port: the Enemy planet-type
//     list +0x20C8 not modelled].
double galactic_ai_planet_force(const galactic::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                                PlanetIndex planet, int32_t self, bool enemy, bool space, uint64_t mask);
// The enemy strength at `planet` for AI player `self` (the perception helper 0x5f2430, GALACTIC_ECONOMY §6.2 [H]): the
// Enemy leaf above (members only) plus, for the untyped total (`mask` == kAllCategories: the Contrast entries 0 / 1),
// the own AI_Combat_Power of the planet's starbase (space) or of its ground structures (ground) when the planet's owner
// is hostile to `self` [port: the 0x4ad9f0 "not held by the AI or an ally" test read as hostile; no garrison types
// 0x4b8000]. A typed row (any other mask: the PotentialPlan Contrast rows [M]) never holds a structure's own power.
double galactic_ai_enemy_strength(const galactic::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                                  PlanetIndex planet, int32_t self, bool space, uint64_t mask);

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
    struct Hooks {
        // Variable_Self.ActiveGoals (0x6e0490): active goals whose category is in the Parameter_Category names (empty = all).
        std::function<int32_t(int32_t player, std::span<const std::string_view> categories)> active_goals;
        bool story_campaign = false;                          // Game.IsStoryCampaign
        // Apply_Markup accumulator of `player`'s AI target for `planet` (+0x80).
        std::function<double(int32_t player, int32_t planet)> markup;
        // AI CreditsUnnormalized: the galactic budget total (false = no galactic AI for that player).
        std::function<bool(int32_t player, double& out)> ai_credits;
        // Budget income rate / available of `category` ("" = all) for BudgetFractionToBuild (false = none).
        std::function<bool(int32_t player, std::string_view category, double& rate, double& available)> budget;
        // AI difficulty index (0 Easy, 1 Normal, 2 Hard; -1 = no AI system).
        std::function<int32_t(int32_t player)> difficulty;
        // Script_<name>.Evaluate (AI_GALACTIC_2 §4.4); null = fails.
        std::function<bool(const EvalContext&, std::string_view name, std::span<const Param>, double&)> script;
        // Variable_Self.StoryTrigger{Parameter_Name} (story flag); null = 0.
        std::function<bool(int32_t player, std::string_view name)> story_flag;
    };
    GalacticSimTokenResolver(const galactic::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                             const TokenResolver* s2, Hooks hooks);

    // Once per frame before evaluating: event stamps (signals 0x12 / 0x14 / 0x1A, corruption), neighbour lists.
    void refresh(int32_t frame);
    void set_s2(const TokenResolver* s2) { s2_ = s2; }
    void set_hooks(Hooks hooks) { hooks_ = std::move(hooks); }
    void clear_cache();

    bool resolve(const EvalContext& ctx, const TokenRef& token, double& out) const override;
    bool implements(Root root, std::span<const TokenId> path, const TokenTable& tokens) const override;
    bool resolve_script(const EvalContext& ctx, std::string_view name, std::span<const Param> params,
                        double& out) const override;

    // "Variable_Self.HasStructure" style key of a token path (upper-case, '.'-joined) for diagnostics.
    static std::string path_key(Root root, std::span<const TokenId> path, const TokenTable& tokens);
    static std::span<const std::string_view> known_paths();
    uint64_t cache_hits() const;

    // N1 save / load: frame, the per-planet event stamps and last-seen values, the leaf cache (key order) and the hit
    // counter. The neighbour lists are rebuilt from the sim (static map data). read() fails (unchanged) unless the
    // saved planet count is 0 (never refreshed) or the sim's.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

    struct CacheKey {
        std::string leaf;  // root kind + leaf path
        int32_t inst = -1, self = -1, target = -1, enemy = -1, human = -1;
        std::string params;
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
    int32_t frame_ = 0;
    // per planet event stamps (frames, -1 = never): space conflict, ground conflict, conversion, corruption change
    std::vector<int32_t> stamp_space_, stamp_ground_, stamp_conversion_, stamp_corruption_;
    std::vector<int32_t> last_owner_;
    std::vector<int8_t> last_corruption_;
    std::vector<uint8_t> last_battle_;
    std::vector<std::vector<uint32_t>> neighbours_;  // planet evaluator list (self excluded), planet order
    struct Entry {
        double value = 0.0;
        int32_t stamp = 0;
    };
    mutable std::mutex mu_;
    mutable std::map<CacheKey, Entry> cache_;
    mutable uint64_t hits_ = 0;
};

}  // namespace alamo::ai

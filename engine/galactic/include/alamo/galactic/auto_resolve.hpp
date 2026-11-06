// S10 "Battle transitions": the galactic auto-resolve (docs/re/BATTLE_TRANSITIONS.md §4, AutoResolveClass).
//
// Pure computation on a prepared combatant list; the conflict manager (conflicts.hpp) builds the list from
// the galaxy and applies the result. Order of everything below = the list order given (defender units,
// attacker units, then the planet's base / starbase combatants: §5 item 2).
//
//   sides (§4.2)      A side takes the first player id it sees; a third player id is skipped (the original
//                     fails Add_Object with 0x80670001).
//   vectors (§4.3)    2 + K doubles per side: [0] land total, [1] space total, [2 + j] the AI contrast
//                     category j (ContrastTable row j). Every strength piece (company / squadron members
//                     as count x P(member), then the combatant's own P unless it is a container in a space
//                     battle) adds its float32 power to the FIRST category whose mask intersects the
//                     piece's CategoryMask and to the battle's total (space total in space, land total on
//                     land). The weakest combatant (smallest own P, first wins) is tracked; Super flags.
//   degenerate (§4.3) space, no Super unit on either side and both space totals <= 0: the side with more
//                     combatants wins (tie: the side that is not the attacker), the loser retreats, nothing
//                     dies.
//   fire (§4.4.1)     each side fires once at a COPY of the other side's vector: per piece p, target j
//                     maximising (1 - ((R_j - p c_j) / max(R_j, p c_j))^2) x c_j (double, first max wins)
//                     over categories with R_j > 0 and c_j = contrast(piece type, row j) > 0; d = p c_j x
//                     bonus (bonus = 1: the planet modifier table 0x374010 is open item 7); a = min(d, R_j);
//                     R_j -= a; total -= a + (1 - a/d) p. No target: total -= p.
//   winner (§4.4.2)   a side facing an unanswerable Super retreats; a retreat flag -> the other side wins;
//                     else S = sum of the positive entries of each remaining vector; both > 0 and both AI:
//                     a non-Is_Playable side loses outright; else side 1 wins iff S0 <= S1, an exact tie
//                     (incl. both 0) goes to the attacker.
//   transports (4.4.4) space only, loser only: k = ground units carried in the loser's fleets (the port has no
//                     transport objects: each carried ground unit counts as one transport [port]); m =
//                     round((1 - AutoResolveTransportLosses) k), 0 when k = 1 or the loser is not playable;
//                     heroes always survive, then the first m others; any survivor -> loser retreats.
//   budget (4.4.5)    budget = rem + (orig - rem)(1 - X) on the battle total, X = loser / winner attrition
//                     (retreat variants when a retreat is flagged); a non-playable loser gets 0; x the
//                     tactical multiplier when started from a tactical battle.
//   survival (4.4.6)  per side, WINNER FIRST then loser [M: order not stated], until the list is empty: pick
//                     the first combatant that is or carries a named hero or has the always-die behaviour
//                     (0x5f), else index = synced RNG int(0, n - 1) (one draw per pick); loser Super units
//                     survive free unless the enemy has a Super-weapon killer; starbases drop to the
//                     highest level whose P x allowance <= budget (destroyed if none), budget -= P(level);
//                     loser always-die units die; other units survive iff budget - cost x allowance > 0 and
//                     then pay budget -= cost (floored at 0) [M: charged only on survival]. A winner with no
//                     survivor keeps its weakest combatant. Any loser survivor -> the loser retreats.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"

namespace alamo::galactic {

// AI contrast table: PGAICommands.lua Set_Contrast_Values (EnemyContrastTypes[k] = "<Category>",
// FriendlyContrastTypeNames / FriendlyContrastWeights). The original runs the Lua; the port reads the
// assignments from the script text [port] (same values for the shipped scripts).
struct ContrastRow {
    std::string enemy;        // EnemyContrastTypes[k] (category name)
    uint64_t enemy_mask = 0;  // GameObjectCategoryType mask of `enemy` (0 = unknown name)
    struct Entry {
        std::string name;     // category or type name
        uint64_t mask = 0;    // category mask (0 = not a category: matched by type name)
        float weight = 0.0f;
    };
    std::vector<Entry> friendly;  // list order
};

class ContrastTable {
public:
    std::vector<ContrastRow> rows;

    size_t size() const { return rows.size(); }
    // Weight of a piece (type name + CategoryMask) against row j: the first entry (list order) whose
    // category mask intersects `categories` or whose name equals `type_name` (case-insensitive); 0 if
    // none [M: WeightedTypeList lookup rule not traced].
    float contrast(size_t row, std::string_view type_name, uint64_t categories) const;
    // The AI PotentialPlan weight 0x5f1f90 (A2K_T02_RE_ANSWERS Q2(b), read from the disassembly) [H]: walking row j's
    // entries in order, a type-name entry equal to `type_name` returns its weight at once; otherwise every category
    // entry meeting `categories` counts as matched and, when its weight != 1.0, adds to a float sum and a count.
    // Result: sum / count when count > 0; else 1.0 if anything matched; else 0 (also for a missing row). [port] Used by
    // the AI solve only; auto-resolve keeps contrast() (its lookup was not traced).
    float ai_weight(size_t row, std::string_view type_name, uint64_t categories) const;
    // Parses the Set_Contrast_Values assignments out of PGAICommands.lua. `category_mask` resolves a
    // GameObjectCategoryType name (0 if unknown). False when no row was found.
    bool parse_lua(std::string_view source, const std::function<uint64_t(std::string_view)>& category_mask,
                   std::vector<std::string>* diagnostics = nullptr);
    void hash(StateHasher& h) const;
};

struct AutoResolveConstants {
    float attrition_allowance = 0.333333f;   // AutoResolveAttritionAllowanceFactor
    float transport_losses = 0.333333f;      // AutoResolveTransportLosses
    float loser_attrition = 0.80f;           // AutoResolveLoserAttrition
    float winner_attrition = 0.75f;          // AutoResolveWinnerAttrition
    float retreat_loser_attrition = 0.65f;   // RetreatAutoResolveLoserAttrition
    float retreat_winner_attrition = 0.50f;  // RetreatAutoResolveWinnerAttrition
    float tactical_multiplier = 0.85f;       // Auto_Resolve_Tactical_Multiplier
    float display_time = 0.01f;              // AutoResolveDisplayTime (seconds)
};

// One strength / firing piece of a combatant.
struct ArPiece {
    uint32_t type = 0xFFFFFFFFu;  // type index (contrast lookup by name)
    std::string_view name;        // type name (contrast lookup)
    uint64_t categories = 0;      // CategoryMask
    float power = 0.0f;           // count x AI_Combat_Power (float32)
};

// One starbase level a starbase combatant can fall back to (current level first, then lower ones).
struct ArStarbaseLevel {
    uint32_t type = 0xFFFFFFFFu;
    float power = 0.0f;
};

struct ArCombatant {
    int32_t owner = -1;
    std::vector<ArPiece> pieces;  // members first, then the own piece (when counted)
    float own_power = 0.0f;       // P of the representative type (weakest-combatant key)
    bool hero = false;            // is or carries a named hero
    bool always_dies = false;     // behaviour 0x5f [L: meaning unknown; no port type sets it]
    bool super = false;           // first member / own type in category "Super"
    bool super_killer = false;    // a member type flagged as Super-weapon killer
    std::vector<ArStarbaseLevel> starbase;  // non-empty: the planet's starbase (space)
};

struct ArSideInfo {
    int32_t player = -1;
    bool ai = false;        // not Is_Human
    bool playable = true;   // faction Is_Playable
};

struct AutoResolveInput {
    bool space = true;
    int32_t attacker = -1;      // the Begin player (+0xc)
    int32_t retreating = -1;    // Set_Retreating before Begin/resolve (-1 = none)
    bool from_tactical = false;
    std::vector<ArCombatant> combatants;     // list order
    std::vector<ArSideInfo> players;         // side info by player (looked up by id; missing = AI, playable)
    // Space: ground units carried in fleets, per carrying player, list order: true = named hero.
    struct Cargo {
        int32_t owner = -1;
        bool hero = false;
    };
    std::vector<Cargo> cargo;
};

struct AutoResolveResult {
    bool valid = false;               // two sides found
    bool degenerate = false;
    int32_t winner = -1, loser = -1;
    int32_t retreating = -1;          // final retreating player (-1 none)
    std::vector<uint8_t> survives;    // per combatant
    std::vector<uint32_t> starbase_type;  // per combatant: surviving starbase type (0xFFFFFFFF = destroyed / n.a.)
    std::vector<uint8_t> cargo_survives;  // per cargo entry
    std::vector<double> vec[2];       // original vectors (side 0 / 1)
    std::vector<double> rem[2];       // after the exchange of fire
    double strength[2] = {0.0, 0.0};  // S0, S1
    double budget[2] = {0.0, 0.0};    // initial survival budgets
    int32_t side_player[2] = {-1, -1};
    uint32_t losses[2] = {0, 0};      // combatants destroyed per side
    uint32_t draws = 0;               // synced RNG draws made
    void hash(StateHasher& h) const;
};

AutoResolveResult auto_resolve(const AutoResolveInput& in, const ContrastTable& contrast, const AutoResolveConstants& k,
                               SimRng& rng);

}  // namespace alamo::galactic

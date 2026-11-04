// Lua bridge, players family (roadmap L1 stage 2): the state behind Find_Player, PlayerWrapper,
// Suspend_AI, EvaluatePerception / Evaluate_In_Galactic_Context, EvaluateTypeList, WeightedTypeList,
// DiscreteDistribution, Purge_Goals, GiveDesireBonus and Budget.Flush_Category (src/bind_players.cpp).
//
// One LuaPlayerWorld per game session, shared by every script VM of that session
// (attach_player_world). It is sim state: owned and mutated by the sim thread only, hashed by hash().
//
// Backing (docs/re/GALACTIC_ECONOMY.md = GE, LUA_API.md):
//   * players, factions, credits, tech level: the S3 GalacticSim when attached (Give_Money goes
//     through GalacticSim::add_credits = Add_Credits 0x27f370: AI Credit_Multiplier, no debt, cap);
//     otherwise the LuaPlayerSetup rows (tactical / skirmish / tests) with the same econ::add_credits;
//   * perception: an ai::EquationSet + ai::TokenResolver per context (current mode, galactic), evaluated
//     with ai::evaluate; A2 tactical: '#' uses the evaluator's own reseeded LCG (AI_GALACTIC_3 §7.3: game seed
//     = PerceptionSource::game_seed, target name CRC 0 on this path [M]) and never the synced GameRandom stream;
//   * everything else (diplomacy, tech locks, AI suspension, selection/actor/retreat flags, tutorial and
//     bombardment switches, goal requests) lives here until the owning systems exist; requests are
//     queued in call order for the AI / UI hosts to drain.
//
// [inferred] behaviour (not specified by RE; settle with Q2 probes):
//   * relations (sim-less worlds): Self on the diagonal; Neutral when either faction Is_Neutral; else Enemy (the
//     same defaults as GalacticSim::default_relation); Make_Ally / Make_Enemy set both directions;
//   * Is_Ally is true for the player itself; Get_Enemy = the lowest-id player with relation Enemy (nil if none);
//   * Find_Player("local") = the local player (set_local_player; default the first human, else 0); faction
//     names match case-insensitively, the lowest player id wins, unknown names give nil;
//   * Get_Faction_Name = the faction name upper-cased (scripts compare with "PIRATES" / "HUTTS"); Get_Name =
//     the faction name as written; Get_Difficulty = the DifficultyAdjustments name up to its first '_'
//     ("Hard_Default" -> "Hard"), "Normal" without an adjustment; Get_Team = team or the player id;
//     Get_Clan_ID = 0;
//   * Suspend_AI(x) is a switch (scripts pair Suspend_AI(1) / Suspend_AI(0) like Lock_Controls): x != 0
//     suspends every AI player, 0 resumes;
//   * Lock_Tech / Unlock_Tech record a per-player override (tech_lock()); P4 lua: with a GalacticSim they also
//     set the sim's lock list (GalacticSim::lock_type, shared with the super-weapon hack; enqueue refuses locked types)
//     and Is_Build_Locked reads that list; Set_Black_Market_Tutorial / Set_Sabotage_Tutorial also set the sim's
//     player flags (set_bm_tutorial / set_sabotage_tutorial);
//   * OI lua: with a GalacticSim attached, relation() / set_relation() (Is_Ally / Is_Enemy / Make_Ally / Make_Enemy,
//     Get_Enemy) read / write GalacticSim::relation / set_relation, the table every galactic hostility consumer reads;
//     the world's own table (defaults above) only backs sim-less worlds (tactical / skirmish / tests).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo {
class StateHasher;
}
namespace alamo::galactic {
class GalacticSim;
}
namespace alamo::ai {
class AiPathQuery;
class EquationSet;
class TokenResolver;
}  // namespace alamo::ai

namespace alamo::lua {

class LuaVm;

enum class PlayerRelation : uint8_t { Self = 0, Ally = 1, Neutral = 2, Enemy = 3 };

// One player when no GalacticSim backs the world.
struct LuaPlayerSetup {
    std::string faction;          // FactionFiles name as written ("Empire")
    bool human = false;           // Is_Human (+0x62)
    bool ai = false;              // has an AI system (+0x360): Credit_Multiplier applies to gains
    bool neutral = false;         // faction Is_Neutral
    std::string difficulty;       // DifficultyAdjustments name ("Hard_Default"); empty = none
    float credit_multiplier = 1.0f;
    float credits = 0.0f;
    float credit_cap = -1.0f;     // -1 = uncapped
    int32_t tech_level = 1;
    int32_t team = -1;            // -1 = the player id
};

// Lua-only per-player state (all players, with or without a GalacticSim).
struct LuaPlayerState {
    LuaPlayerSetup setup;                     // facts when no GalacticSim is attached (credits live here)
    std::map<uint32_t, bool> tech_override;   // TypeIndex -> locked (Lock_Tech / Unlock_Tech)
    bool actor = false;                       // Enable_As_Actor
    objects::ObjectId selected = objects::kNoObjectId;  // Select_Object
    int32_t retreat_requests = 0;             // Retreat
    bool bombing_run_disabled = false;        // Disable_Bombing_Run
    bool orbital_bombardment_disabled = false;  // Disable_Orbital_Bombardment
    bool orbital_bombardment_removed = false;   // Remove_Orbital_Bombardment
    bool black_market_tutorial = false;       // Set_Black_Market_Tutorial
    bool sabotage_tutorial = false;           // Set_Sabotage_Tutorial
    std::map<std::string, bool> advisor_hints;  // Enable_Advisor_Hints(name, on)
    // (Is_Obsolete reads the locked list, STRATEGIC_GAPS §5.1: GalacticSim::type_locked, else tech_override.)
};

struct PerceptionSource {
    const ai::EquationSet* equations = nullptr;
    const ai::TokenResolver* resolver = nullptr;
    uint32_t game_seed = 0;  // A2 tactical: the session seed for the '#' reseed (AI_GALACTIC_3 §7.3)
};

// Requests scripts make of the AI goal system / tactical host, in call order.
struct DesireBonusRequest {
    int32_t player = -1;
    std::string goal;
    objects::ObjectId target = objects::kNoObjectId;
    double bonus = 0.0;
    double duration = -1.0;  // -1 = not given
};
struct TacticalCreditRelease {
    int32_t player = -1;
    double amount = -1.0;  // -1 = no amount given
};
struct BudgetFlushRequest {
    int32_t player = -1;   // the script's PlayerObject, -1 when it has none
    std::string category;
};

// a1j t07 (docs/re/A1_READINGS.md §38): the parent galaxy of a tactical mode, as its AI scripts see it. The battle host
// sets it on the tactical AI's world (LuaPlayerWorld::set_tactical_galactic); not saved (the host sets it again when a
// loaded battle is rebuilt). With it:
//   * Evaluate_In_Galactic_Context (0x69e1b0): the perception is evaluated with `source` (the galactic equations and a
//     galactic resolver), Self = the player, Target = `target`, Human = the local player; errors (script errors, as the
//     original's): unknown perception, a player without a galactic AI (`has_galactic_ai`), a failed evaluation. No
//     conflict record (target -1): no value [L]. [port] Enemy = LuaPlayerWorld::enemy_of (the original binds the
//     galactic AI's opponent).
//   * Release_Credits_For_Tactical (0x603c70): land only, the player must have an AI and a galactic AI, then
//     `release_credits(player, amount)` (amount < 0: none given) - the host's 0x6147d0 on the land credit transfer.
// Without it both keep their sim-less behaviour (galactic_perception() with no target; releases are queued).
struct TacticalGalacticContext {
    PerceptionSource source;
    int32_t target = -1;  // planet index of the parent's head conflict (0x28a7a0 -> 0x28aa50)
    bool land = false;    // the tactical mode is land
    std::function<bool(int32_t player)> has_galactic_ai;  // 0x4b06d0(AI system, galactic) != null
    std::function<void(int32_t player, double amount)> release_credits;
};

class LuaPlayerWorld {
public:
    // Players = the sim's players (ids = sim player ids). `sim` must outlive the world (nullptr detaches).
    void attach_galactic(galactic::GalacticSim* sim);
    galactic::GalacticSim* galactic() const { return sim_; }
    // The AI execution path finders (Lua Find_Path, GALACTIC.md 4/5). `q` must outlive the world (nullptr detaches;
    // Find_Path then falls back to the fleet A*).
    void attach_ai_paths(ai::AiPathQuery* q) { ai_paths_ = q; }
    ai::AiPathQuery* ai_paths() const { return ai_paths_; }
    // Players without a GalacticSim. Also sizes the Lua-only state; resets relations to the defaults.
    void set_players(std::vector<LuaPlayerSetup> players);
    size_t player_count() const { return state_.size(); }
    bool valid(int32_t p) const { return p >= 0 && static_cast<size_t>(p) < state_.size(); }
    void set_local_player(int32_t p) { local_ = p; }
    int32_t local_player() const;

    void set_perception(PerceptionSource current, PerceptionSource galactic_context) {
        current_ = current;
        galactic_ctx_ = galactic_context;
    }
    const PerceptionSource& perception() const { return current_; }
    const PerceptionSource& galactic_perception() const { return galactic_ctx_; }
    void set_tactical_galactic(TacticalGalacticContext c) {
        tactical_galactic_ = std::move(c);
        has_tactical_galactic_ = true;
    }
    const TacticalGalacticContext* tactical_galactic() const { return has_tactical_galactic_ ? &tactical_galactic_ : nullptr; }

    // ---- facts (sim when attached, else setup) ----
    std::string faction_name(int32_t p) const;  // as written
    bool is_human(int32_t p) const;
    bool has_ai(int32_t p) const;
    bool is_neutral(int32_t p) const;
    std::string difficulty_name(int32_t p) const;  // DifficultyAdjustments name, "" if none
    float credits(int32_t p) const;
    float give_money(int32_t p, float amount);  // Add_Credits; returns the amount applied
    int32_t tech_level(int32_t p) const;
    void set_tech_level(int32_t p, int32_t level);
    int32_t find_faction_player(std::string_view faction) const;  // lowest id, -1 if none
    // q6 t11 (docs/re/LUA_API.md "Lookups", Find_Player): 1 when `faction` names a faction of the game data (the attached
    // GalacticSim's FactionFiles list, case-insensitive), 0 when it does not, -1 when the world has no faction list (no
    // GalacticSim attached) [port: Find_Player then cannot tell an unknown faction from one without a player].
    int faction_known(std::string_view faction) const;

    // ---- diplomacy ----
    PlayerRelation relation(int32_t a, int32_t b) const;
    void set_relation(int32_t a, int32_t b, PlayerRelation r);  // both directions; ignored for a == b
    int32_t enemy_of(int32_t p) const;  // lowest-id Enemy, -1 if none

    // ---- AI ----
    void set_ai_suspended(bool on) { ai_suspended_ = on; }
    bool ai_suspended() const { return ai_suspended_; }
    int32_t tech_lock(int32_t p, uint32_t type) const;  // -1 no override, 0 unlocked, 1 locked

    std::vector<LuaPlayerState>& states() { return state_; }
    const std::vector<LuaPlayerState>& states() const { return state_; }
    std::vector<int32_t> purge_goal_requests;
    std::vector<DesireBonusRequest> desire_bonuses;
    std::vector<TacticalCreditRelease> tactical_releases;
    std::vector<BudgetFlushRequest> budget_flushes;

    // Ordered digest of everything above (sim-owned facts are hashed by the GalacticSim itself).
    void hash(StateHasher& h) const;

    // N1 save / load (contract: alamo/core/save_file.hpp; S4 session chunk "story.players"): u32 version, every
    // player's Lua-only state (setup included: it holds the credits of sim-less worlds), the relation table, the local
    // player, the AI switch and the four request queues. Not saved: the GalacticSim / path finder / perception
    // attachments (the reader's own; sim-owned facts are saved by the sim). Read after attach_galactic / set_players
    // (both reset the state). With a GalacticSim attached the saved player count must equal the sim's. read() replaces
    // all of the above; false and unchanged on bad data. Reads its own fields only (the owner checks done()).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    void reset_relations();
    galactic::GalacticSim* sim_ = nullptr;
    ai::AiPathQuery* ai_paths_ = nullptr;
    std::vector<LuaPlayerState> state_;
    std::vector<uint8_t> relation_;  // P x P, [a * P + b]
    int32_t local_ = -1;
    bool ai_suspended_ = false;
    PerceptionSource current_, galactic_ctx_;
    TacticalGalacticContext tactical_galactic_;
    bool has_tactical_galactic_ = false;
};

// Makes `world` reachable from `vm`'s bindings (nullptr detaches). Owner thread only.
void attach_player_world(LuaVm& vm, LuaPlayerWorld* world);
LuaPlayerWorld* player_world(LuaVm& vm);

}  // namespace alamo::lua

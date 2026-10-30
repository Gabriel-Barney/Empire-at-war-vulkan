// Galactic AI perception (roadmap S2, docs/re/GALACTIC_ECONOMY.md section 6): the four token paths
// galactic code evaluates, Node_Weight, and everything the AI's pathing reads from perception.
//
//   consumer (section 6.2)              token path                                       output here
//   Node_Weight (0x693db0 -> 0x5f2430)  Variable_Target.EnemyForce.SpaceTotalUnnormalized   node_values()
//     + AI_Combat_Power of the planet's STARBASE when the planet is not held by the AI or an ally
//   ground variant (ground flag 1)      ...EnemyForce.GroundTotalUnnormalized              ground_values()
//     + sum of AI_Combat_Power of ground structures / garrison types when not held
//   Is_Planet_Dangerous (0x3f18c0)      ...EnemyForce.HasSpaceForce                        PlayerView::dangerous
//   Is_Planet_Passable_For_Player       Variable_Target.TargetPlayerID                     PlayerView::ai_eval_failed /
//     (0x3f2510, AI branch)                                                                ai_perceived_non_neutral
// Each path is compiled once into an Expression and evaluated through the general equation
// evaluator (perception_equations.hpp) with GalacticTokenResolver, the same resolver the shipped
// equation files use for these tokens.
//
// PROVISIONAL - interim model of GALACTIC_ECONOMY.md section 6.4 [L] (the real semantics live in
// PerceptionFunctionG.dll / VPlanetForcePerceptualEvaluatorClass; settle with the section 6.4 Q2 probe):
//   * EnemyForce.SpaceTotalUnnormalized(Self, Target) = sum, in unit order, of the combat power (double
//     accumulation of float powers) of SPACE units in orbit of Target whose owner is a player hostile
//     to Self (relation != 0; unowned units count as hostile), AS LAST SEEN BY SELF: refreshed only on
//     frames where Self sees the planet's orbit (GalacticSnapshot::sees_orbit), else the remembered
//     value; 0 before the first sighting;
//   * EnemyForce.GroundTotalUnnormalized: the same on the surface;
//   * EnemyForce.HasSpaceForce = SpaceTotalUnnormalized > 0 ? 1.0 : 0.0;
//   * TargetPlayerID = the owner id as last seen by Self; evaluation FAILS before the first sighting
//     (the passability rule then treats the planet as passable);
//   * a failed Node_Weight evaluation contributes 0.0;
//   * "held by me or an ally" (0x4ad9f0) is also used for the starbase rule of Is_Planet_Dangerous
//     (the doc names 0x4ad9a0 there);
//   * squadron fill scaling of AI_Combat_Power (section 6.2, [M]) is the host's job when it fills
//     UnitPresence::combat_power.
// Everything else (Node_Weight arithmetic, the consumers, cadence) is [H] per the doc.
//
// Determinism: per-AI outputs are written per planet index; evaluation is pure (no '#' in these
// paths) and may fan out over planets through ParallelFor, but only from kPerceptionParallelMin
// planets (the measured-threshold rule used elsewhere, e.g. S6 rebuild >= 16K); galaxies are far
// below it, so the serial path is what runs. Results are identical either way.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/ai/perception_equations.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/galactic/all_pairs.hpp"
#include "alamo/galactic/bitset.hpp"
#include "alamo/galactic/fleet_path.hpp"

namespace alamo {
class ByteReader;
class ByteWriter;
}  // namespace alamo

namespace alamo::ai {

inline constexpr uint32_t kPerceptionParallelMin = 16384;

// Fogged-leaf masks M (AI_GALACTIC_TOKENS §0 [H masks, M meaning]) of the remembered values:
inline constexpr uint32_t kVisMaskOwner = 0x1;         // planet: owner / TargetPlayerID
inline constexpr uint32_t kVisMaskSpaceUnits = 0x20;   // force: space units (EnemyForce.SpaceTotal*) [M]
inline constexpr uint32_t kVisMaskGroundUnits = 0x80;  // force: ground units (EnemyForce.GroundTotal*) [M]

using PlayerId = int32_t;
inline constexpr PlayerId kNoPlayer = -1;

struct PerceptionPlayer {
    bool neutral = false;                   // faction Is_Neutral (+0x108)
    bool is_human = false;                  // +0x62
    bool has_ai_system = false;             // +0x360 != null
    bool benefits_from_corruption = false;  // faction +0x10b
};

enum class UnitLayer : uint8_t { Space = 0, Ground = 1 };

// One object present at a planet (orbit or surface). combat_power = the type's AI_Combat_Power
// (type +0xdf0), already scaled by the host where the squad-fill rule applies.
struct UnitPresence {
    PlayerId owner = kNoPlayer;
    float combat_power = 0.0f;
    UnitLayer layer = UnitLayer::Space;
};

// The galactic state perception reads, one frame. Planet arrays are indexed by PlanetIndex (graph
// node order); unit / structure lists are CSR (begin has planet_count + 1 entries) in a
// deterministic order (e.g. ascending object id). Empty optional arrays take the noted default.
struct GalacticSnapshot {
    uint32_t planet_count = 0;
    std::vector<PerceptionPlayer> players;        // index = player id
    std::vector<uint8_t> relation;                // players^2, [a * P + b]; 0 = friendly (self/ally). empty => only self friendly
    std::vector<PlayerId> owner;                  // kNoPlayer = unowned
    std::vector<float> political_control;         // empty => 1.0 for owned planets
    std::vector<int32_t> starbase_level;          // data pack +0x148; empty => 0
    std::vector<float> starbase_power;            // AI_Combat_Power of the starbase type (D +0x140); empty => 0
    std::vector<int32_t> corruption;              // data pack +0x2f4, -1 = none; empty => none
    std::vector<uint32_t> unit_begin;             // CSR into units; empty => no units
    std::vector<UnitPresence> units;
    std::vector<uint32_t> structure_begin;        // CSR into structure_power (ground variant); empty => none
    std::vector<float> structure_power;           // ground structures (D +0x158) + garrison types (0x4b8000)
    std::vector<galactic::Bitset> sees_orbit;     // per player: Self sees that planet's units. empty / missing => sees all
    // Per player, per planet: the VisibilityLevelType level the fogged leaves compare against (AI_GALACTIC_TOKENS §0:
    // a leaf is visible iff (level & M) == M). From GalacticSim the PRESENCE level (GRC 2.3: the value signal 0xE
    // carries, 0xFFFFFFFF when the player owns the planet). When a player's row is present it replaces sees_orbit for
    // that player, per memory field (kVisMask* below); empty / missing => sees_orbit. Only read while fog is on.
    std::vector<std::vector<uint32_t>> visibility;
    // LOCAL player (this machine) state, copied into PlayerView as the original does:
    galactic::Bitset revealed, hidden, in_battle, ownership_visible, local_orbit_visible;
    bool multiplayer = false;
    // AIUsesFogOfWarGalactic off (GC +0x2EC): every player sees every planet's units (sees()). Not the reveal state.
    bool fog_disabled = false;
    // Galactic mode +0x340 Planet_Auto_Reveal (GALACTIC_REVEAL_CORRUPTION 0.1): Is_Planet_Revealed answers true for
    // every planet without the hidden byte. Copied to PlayerView::fog_disabled (the galactic field keeps that name).
    bool planet_auto_reveal = false;

    uint32_t player_count() const { return static_cast<uint32_t>(players.size()); }
    bool is_player(PlayerId p) const { return p >= 0 && static_cast<uint32_t>(p) < players.size(); }
    bool friendly(PlayerId a, PlayerId b) const;            // a == b, or relation(a, b) == 0
    bool held_by_ally(uint32_t planet, PlayerId p) const;   // 0x4ad9f0
    bool unowned_or_neutral(uint32_t planet) const;         // 0x4ada70
    bool sees(PlayerId p, uint32_t planet) const;
    // Fogged-leaf visibility for the leaf mask `m` (AI_GALACTIC_TOKENS §0): fog off => true; a visibility row for p
    // => (level & m) == m; else sees().
    bool sees(PlayerId p, uint32_t planet, uint32_t m) const;
    std::span<const UnitPresence> units_at(uint32_t planet) const;
    std::span<const float> structures_at(uint32_t planet) const;
    std::vector<std::string> validate() const;  // empty == OK
    // N1 save / load: every field (the galactic AI keeps the snapshot of its last perception update).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};

// What one AI player remembers of each planet (fogged perception). Game state: hashed.
struct PerceptionMemory {
    std::vector<uint8_t> seen;            // ever seen
    std::vector<uint32_t> last_seen;      // frame of the last sighting
    std::vector<PlayerId> owner;          // owner as last seen
    std::vector<double> space_total;      // hostile space power as last seen
    std::vector<double> ground_total;     // hostile ground power as last seen
    void reset(uint32_t planets);
};

// Resolves galactic tokens for Self = an AI player, Target = a planet, from one snapshot and the
// players' memories. Thread-safe for concurrent evaluations.
class GalacticTokenResolver final : public TokenResolver {
public:
    GalacticTokenResolver(const GalacticSnapshot& snapshot, std::span<const PerceptionMemory> memory)
        : s_(snapshot), mem_(memory) {}
    bool resolve(const EvalContext& ctx, const TokenRef& token, double& out) const override;
    bool implements(Root root, std::span<const TokenId> path, const TokenTable& tokens) const override;

private:
    const GalacticSnapshot& s_;
    std::span<const PerceptionMemory> mem_;
};

// Per AI player, per planet results of one evaluation.
struct PlanetPerception {
    std::vector<double> space_total, ground_total;  // EnemyForce.*TotalUnnormalized (0 when failed)
    std::vector<uint8_t> space_ok, ground_ok;
    std::vector<double> has_space_force;
    std::vector<uint8_t> has_space_force_ok;
    std::vector<double> target_player;
    std::vector<uint8_t> target_player_ok;
    std::vector<double> node_value;    // Node_Weight input (ground flag 0): feeds AllPairsInput::node_value
    std::vector<double> ground_value;  // ground variant (flag 1)
};

class GalacticPerception {
public:
    GalacticPerception();

    // Sizes memories for the snapshot's planets / players and clears them.
    void reset(const GalacticSnapshot& s);
    // Once per frame, before the AI services (section 6.3): refresh every AI player's memory from
    // what it sees, then evaluate the four paths for every (AI player, planet). Players are processed
    // in id order; `pf` fans out over planets only from kPerceptionParallelMin planets.
    void update(const GalacticSnapshot& s, uint32_t frame, const ParallelFor& pf = serial_for);
    void observe(const GalacticSnapshot& s, uint32_t frame);
    void evaluate(const GalacticSnapshot& s, const ParallelFor& pf = serial_for);

    // Players that get perception: has_ai_system (humans with an AI system included).
    bool tracks(PlayerId p) const { return p >= 0 && static_cast<size_t>(p) < tracked_.size() && tracked_[static_cast<size_t>(p)]; }
    const PlanetPerception& results(PlayerId p) const { return results_[static_cast<size_t>(p)]; }
    std::span<const double> node_values(PlayerId p) const { return results(p).node_value; }
    const PerceptionMemory& memory(PlayerId p) const { return memory_[static_cast<size_t>(p)]; }
    size_t memory_count() const { return memory_.size(); }  // players reset() sized for
    // Is_Planet_Revealed (0x3fa420) for every planet, from the snapshot's LOCAL masks.
    const galactic::Bitset& revealed_mask() const { return revealed_; }

    // Every PlayerView field for mover `p` (Q5/Q6 + section 6.4 sources). Call after update().
    void fill_view(const GalacticSnapshot& s, PlayerId p, galactic::PlayerView& view) const;
    // All-pairs inputs for AI `p`: node_value = node_values(p); the fog mask only for a non-human
    // player with an AI system (Q8). Spans point into this object (valid until the next update()).
    galactic::AllPairsInput all_pairs_input(PlayerId p) const;

    // CRC (Q1 contract): per tracked player in id order, memory then published results.
    void hash(StateHasher& h) const;
    // N1 save / load: planets, frame, tracked / human players, every memory, the published results and the revealed
    // mask (all of it: the results are read between updates). The compiled consumer paths are not saved.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

    // The compiled consumer paths and the evaluator used for them (exposed for tests / tools).
    const EquationSet& consumer_paths() const { return paths_; }
    enum Path : uint32_t { kSpaceTotal = 0, kGroundTotal = 1, kHasSpaceForce = 2, kTargetPlayerId = 3 };

private:
    void evaluate_player(const GalacticSnapshot& s, PlayerId p, const ParallelFor& pf);

    EquationSet paths_;
    uint32_t path_index_[4] = {};
    uint32_t planets_ = 0;
    uint32_t frame_ = 0;
    std::vector<uint8_t> tracked_;
    std::vector<uint8_t> human_;  // Is_Human per player (all-pairs fog gate), refreshed by evaluate()
    std::vector<PerceptionMemory> memory_;
    std::vector<PlanetPerception> results_;
    galactic::Bitset revealed_;
};

// Q7 port: the node cost the all-pairs node pass stores for one Node_Weight input.
inline uint32_t node_cost(double node_value) { return galactic::node_cost_from_value(node_value); }

// Registers "ai.perception" (GalacticPerception::hash) with the per-tick CRC registry.
CrcRegistry::SystemId register_crc(CrcRegistry& reg, const GalacticPerception& p, std::string name = "ai.perception");

}  // namespace alamo::ai

// Per-AI galactic execution path finder (AIExecutionPathFinderClass / GalacticExecutionPathFinderClass,
// docs/re/GALACTIC.md §4, PHASE2_ANSWERS Q7-Q11). One per AI player, owned by its execution system
// (GalacticAiWorld, galactic_ai.hpp); answers Lua Find_Path (0x69da70) and the AI's own Get_Path queries.
//
//   matrices  : galactic::compute_all_pairs (Floyd-Warshall reference, or the provably identical fast path),
//               inputs = Node_Weight per planet (perception value + opposing strength, Q7), Edge_Weight (Q8,
//               source type's Layer_Z_Adjust), and the fog gate: an edge touching a planet that fails
//               Is_Planet_Revealed (0x3fa420: hidden -> false; Planet_Auto_Reveal -> true; else the revealed
//               byte) is unlinked, ONLY for a player that has an AI system and is not human (Q8 step 2).
//   cadence   : creation (Execution slot 6, 0x526120) recomputes at once, then next =
//               first_recompute_due(frame, slot, K) (stagger, K = player slots - active humans); the lazy
//               creation (0x524c10) recomputes at once and leaves next = 0; the Execution service (0x526590)
//               recomputes when next <= frame and sets next = frame + 90. Frame = the galactic mode frame.
//   Get_Path  : AllPairsMatrices::get_path (Q10): src == dst -> [src, src]; unknown / unreachable -> empty.
//
// Determinism: the matrices are a pure function of (graph, node costs, fog mask, z offsets) taken at the
// recompute frame; galactic::AllPairsCache skips the work when none changed (identical result). Any
// ParallelFor gives bit-identical matrices (galactic/all_pairs.hpp). Nothing here reads a clock or an RNG.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/core/galaxy.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/galactic/all_pairs.hpp"
#include "alamo/galactic/bitset.hpp"
#include "alamo/galactic/cadence.hpp"
#include "alamo/galactic/planet_graph.hpp"

namespace alamo {
class ByteReader;
class ByteWriter;
}  // namespace alamo

namespace alamo::ai {

// Q8 step 2: the all-pairs fog gate applies only to a player with an AI system that is not human.
constexpr bool ai_path_fog_gate(bool has_ai_system, bool is_human) { return has_ai_system && !is_human; }

// The AllPairsInput of one recompute. `revealed` is the Is_Planet_Revealed mask (auto-reveal already folded in,
// GalacticPerception::revealed_mask()); it is dropped unless ai_path_fog_gate(). Spans must outlive the call.
galactic::AllPairsInput ai_path_input(std::span<const double> node_value, const galactic::Bitset* revealed,
                                      bool has_ai_system, bool is_human, std::span<const float> z_offset = {});

class AiPathFinder {
public:
    // Creation at Execution slot 6 (0x526120): recompute now (outside the cadence), next = stagger.
    void create(const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input, int32_t frame, int32_t slot,
                int32_t k, const ParallelFor& pf = serial_for);
    // Lazy creation (0x524c10): recompute now, next stays 0 (the first service recomputes again).
    void create_lazy(const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input,
                     const ParallelFor& pf = serial_for);
    // Execution service (0x526590). Not created: nothing. When next <= frame: next = frame + 90, recompute with
    // make_input() (called only then). Returns true when the cadence fired.
    template <class MakeInput>
    bool service(int32_t frame, const galactic::PlanetGraph& graph, MakeInput&& make_input,
                 const ParallelFor& pf = serial_for) {
        if (!created_ || !galactic::recompute_due(frame, next_)) return false;
        next_ = galactic::next_after_service(frame);
        recompute(graph, make_input(), pf);
        return true;
    }
    void reset();  // back to "not created"

    bool created() const { return created_; }
    int32_t next_recompute() const { return next_; }
    uint32_t recomputes() const { return runs_; }                         // cadence firings incl. creation
    uint32_t matrix_recomputes() const { return matrix_base_ + cache_.recompute_count(); }  // runs whose inputs changed
    const galactic::AllPairsMatrices& matrices() const { return cache_.matrices(); }

    // Get_Path (Q10). Empty when not created, a planet is not a node, or unreachable.
    std::vector<PlanetIndex> path(PlanetIndex from, PlanetIndex to) const;
    bool get_path(PlanetIndex from, PlanetIndex to, std::vector<PlanetIndex>& out,
                  std::vector<uint32_t>* hop_weights = nullptr) const;
    // The all-pairs value W[from][to] (galactic::kUnlinked when unknown / unreachable).
    uint32_t weight(PlanetIndex from, PlanetIndex to) const;

    // created, next, recompute count, digest of the matrices (taken at each recompute).
    void hash(StateHasher& h) const;

    // N1 save / load: created, next, run counters, digest and the INPUTS of the last recompute (node values / costs,
    // fog mask, z offsets); the matrices are not saved but rebuilt from those inputs on `graph` (the session's static
    // planet graph), which also restores the cache's change detection exactly. read() fails (unchanged) when the rebuilt
    // matrices' digest differs from the saved one.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r, const galactic::PlanetGraph& graph, const ParallelFor& pf = serial_for);

private:
    void recompute(const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input, const ParallelFor& pf);
    galactic::AllPairsInput last_input() const;

    galactic::AllPairsCache cache_;
    bool created_ = false;
    int32_t next_ = galactic::kLazyCreatedNextRecompute;
    uint32_t runs_ = 0;
    uint64_t digest_ = 0;
    uint32_t matrix_base_ = 0;  // matrix recomputes before a load (the rebuilt cache counts from 1)
    // The last recompute's input (N1: the matrices are rebuilt from it on load).
    std::vector<double> in_value_;
    std::vector<int32_t> in_cost_;
    galactic::Bitset in_revealed_;
    bool in_has_revealed_ = false;
    std::vector<float> in_zoff_;
};

// Query side for hosts and Lua Find_Path (implemented by GalacticAiWorld::path_query(), attached to the AI's and the
// story's LuaPlayerWorld). It never creates a finder.
class AiPathQuery {
public:
    virtual ~AiPathQuery() = default;
    // False: `player` has no CREATED AI path finder (not an AI player of this world, or its AI has not had its first
    // service yet; Find_Path then falls back). True:
    // `out` holds Get_Path(from, to), empty when there is no path.
    virtual bool ai_path(int32_t player, PlanetIndex from, PlanetIndex to, std::vector<PlanetIndex>& out) = 0;
};

}  // namespace alamo::ai

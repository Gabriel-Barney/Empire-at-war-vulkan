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
//
// OPT2 t02 background mode (set_background, latency k > 0; GalacticAiConfig::path_latency is the default): a cadence
// recompute due on frame N copies its inputs and runs on the host's background hook; publish(frame) applies it on frame
// N + k (blocking only if it is late). Until then get_path / weight / hash see the previous matrices. Creation (create,
// create_lazy) stays synchronous: the first service and the lazy getter read the matrix on the same frame. The planet
// graph must not change while a recompute is in flight (the session's static graph). N1 save: the in-flight recompute's
// inputs and ready frame; a load recomputes them inline and applies the result on the same frame (latency_job.hpp).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/core/galaxy.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/ai/latency_job.hpp"
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

// StateHasher digest of an all-pairs result (u32 size, W, P), as AiPathFinder keeps it.
uint64_t matrices_digest(const galactic::AllPairsMatrices& m);

class AiPathFinder {
public:
    // OPT2 t02: cadence recomputes run in the background and are applied `latency` frames later (0 = at once, the
    // original timing). `bg` empty = inline at the kick (same results). Call before create().
    void set_background(uint32_t latency, BackgroundStart bg = {}) {
        latency_ = latency;
        bg_ = std::move(bg);
    }
    uint32_t latency() const { return latency_; }
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
        if (latency_ == 0) recompute(graph, make_input(), pf);
        else kick(frame, graph, make_input(), pf);
        return true;
    }
    // OPT2 t02: applies the in-flight recompute when `frame` reached its ready frame (once per frame, before anything
    // reads the matrices). True when a result was applied.
    bool publish(int32_t frame);
    bool pending() const { return job_.in_flight(); }
    int32_t ready_frame() const { return static_cast<int32_t>(job_.ready_frame()); }
    void reset();  // back to "not created"

    bool created() const { return created_; }
    int32_t next_recompute() const { return next_; }
    uint32_t recomputes() const { return runs_; }                         // cadence firings incl. creation
    // Runs whose inputs changed (synchronous recomputes) / that changed the matrices (background ones).
    uint32_t matrix_recomputes() const { return matrix_runs_; }
    const galactic::AllPairsMatrices& matrices() const { return published(); }

    // Get_Path (Q10). Empty when not created, a planet is not a node, or unreachable.
    std::vector<PlanetIndex> path(PlanetIndex from, PlanetIndex to) const;
    bool get_path(PlanetIndex from, PlanetIndex to, std::vector<PlanetIndex>& out,
                  std::vector<uint32_t>* hop_weights = nullptr) const;
    // The all-pairs value W[from][to] (galactic::kUnlinked when unknown / unreachable).
    uint32_t weight(PlanetIndex from, PlanetIndex to) const;

    // created, next, recompute count, digest of the matrices (taken at each recompute); OPT2 t02: the in-flight
    // recompute's ready frame (only while one is in flight).
    void hash(StateHasher& h) const;

    // N1 save / load: created, next, run counters, digest and the INPUTS of the last applied recompute (node values /
    // costs, fog mask, z offsets); the matrices are not saved but rebuilt from those inputs on `graph` (the session's
    // static planet graph). OPT3 t04: an in-flight background recompute is saved as its kick / ready frame and input;
    // read() recomputes it inline and applies it on that ready frame (publish). read() fails (unchanged) when the rebuilt
    // matrices' digest differs from the saved one; it keeps this finder's set_background().
    void write(ByteWriter& w) const;
    bool read(ByteReader& r, const galactic::PlanetGraph& graph, const ParallelFor& pf = serial_for);

private:
    // One recompute's input, owned (N1: the matrices are rebuilt from it on load; OPT2 t02: a background job's copy).
    struct Input {
        std::vector<double> value;
        std::vector<int32_t> cost;
        galactic::Bitset revealed;
        bool has_revealed = false;
        std::vector<float> zoff;
        void assign(const galactic::AllPairsInput& in);
        galactic::AllPairsInput view() const;
        void write(ByteWriter& w) const;
        bool read(ByteReader& r);
    };
    // A background recompute's result buffer: fresh matrices (no change detection: the job never reads the published
    // ones) and their digest.
    struct Result {
        galactic::AllPairsMatrices m;
        uint64_t digest = 0;
    };
    const galactic::AllPairsMatrices& published() const { return use_front_ ? front_ : cache_.matrices(); }
    void recompute(const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input, const ParallelFor& pf);
    void kick(int32_t frame, const galactic::PlanetGraph& graph, const galactic::AllPairsInput& input, const ParallelFor& pf);
    void start_job(int32_t frame, int32_t ready, const galactic::PlanetGraph& graph, const ParallelFor& pf,
                   const BackgroundStart& bg);

    galactic::AllPairsCache cache_;  // synchronous recomputes (creation, latency 0, a load's rebuild)
    bool created_ = false;
    int32_t next_ = galactic::kLazyCreatedNextRecompute;
    uint32_t runs_ = 0;
    uint64_t digest_ = 0;
    uint32_t matrix_runs_ = 0;  // recomputes that changed the matrices (saved)
    Input in_;                  // the last applied recompute's input
    // OPT2 t02 background mode: front_ holds the last applied background result (use_front_) until the next
    // synchronous recompute.
    uint32_t latency_ = 0;
    BackgroundStart bg_;
    galactic::AllPairsMatrices front_;
    bool use_front_ = false;
    Input pend_;  // the in-flight recompute's input
    LatencyJob<Result> job_;
};

// Lua _FindStageArea (0x6990f0) for a planet target, A2k t02 [M]: among `candidates` (the player's own planets, in
// planet row order) the one with the lowest all-pairs weight W[stage][target], compared unsigned; the first wins ties
// and unreachable pairs are skipped. kInvalidPlanet when there is none (or `f` is not created). The weight sums the
// node costs after the stage (high 16 bits) and the hops, so the stage is the start of the AI's cheapest path, not
// the planet with the fewest hops (docs/re/A2K_T02_RE_QUESTIONS.md).
PlanetIndex cheapest_stage(const AiPathFinder& f, std::span<const PlanetIndex> candidates, PlanetIndex target);

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

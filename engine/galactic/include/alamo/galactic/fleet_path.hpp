// Fleet movement path finding (GALACTIC.md section 3, GalacticPathFinderClass::Find_Path), with
// the open-list discipline and Edge_Cost order of docs/re/PHASE2_ANSWERS.md Q3-Q6.
//
// The search reproduces the original exactly, so paths under exact f ties are the original's:
//   * f = g + h in float, h = planet_distance(neighbour, goal) / max_speed_factor (Q3); the start
//     entry has g = f = 0.
//   * open list = 1-based binary min-heap on f only (no g / index tie-break), strict comparisons
//     in both sift directions (Q4); goal test on pop; popped entries are expanded with THEIR g
//     (stale duplicates included); closed nodes are never reopened; an improvement (strictly
//     smaller g, or first sighting) allocates a new path record and pushes a duplicate entry.
//   * neighbours in ascending planet index (PlanetGraph edge order).
//   * heap holds 3N+1 slots (N = graph node count); a push when size == 3N is silently dropped.
//     Reproduced, but unreachable: each heap entry owns a distinct record and the new record is
//     allocated before the push, so size <= records - 2 <= 3N - 2 when a push arrives.
//   * the record pool holds 3N records (start included). The original dereferences null when it
//     runs out (crash); here the search stops and fails with FleetPathStatus::RecordPoolExhausted.
//     Only seen on dense synthetic maps (many improvements per node), never on sparse ones.
// Unlike the original singleton, every request uses a caller-owned PathScratch, so requests on
// different threads (each with its own scratch) are safe; the graph and PlayerView are only read.
#pragma once
#include <cstdint>
#include <vector>

#include "alamo/galactic/bitset.hpp"
#include "alamo/galactic/planet_graph.hpp"

namespace alamo::galactic {

// The six request flags (Path_Request_Init sets all to true). Indices match GALACTIC.md; the
// meanings are the corrected ones of PHASE2_ANSWERS Q5 / Q6.
struct PathRequest {
    PlanetIndex source = kInvalidPlanet;
    PlanetIndex destination = kInvalidPlanet;
    bool allow_unknown_or_neutral = true;   // flag 0: ownership hidden from me, unowned, or neutral-owned
    bool allow_enemy_owned = true;          // flag 1: owned by a non-neutral, non-allied faction
    bool skip_danger_check = true;          // flag 2: passable planets skip Is_Planet_Dangerous
    bool allow_impassable = true;           // flag 3
    bool always_accept_destination = true;  // flag 4: destination accepted with the base cost
    bool fog_rule = true;                   // flag 5: AI fog gate (non-human players with an AI)

    // Fleet orders (MovementBlockStatus move mode m). Flag 5 stays as initialised.
    static PathRequest for_fleet_move_mode(PlanetIndex src, PlanetIndex dst, int m);
    // AI request builder (0x665160), AI mode m in 0..5 (flags per FIXPASS_GALACTIC.md §1.4).
    // Flag 5 stays as initialised.
    static PathRequest for_ai_mode(PlanetIndex src, PlanetIndex dst, int m);
};

// Everything Edge_Cost reads, from the MOVING player's point of view, except the fields marked
// LOCAL, which are the local (this machine's) player's state even when another player paths.
// That is the original's behaviour (Q5): preserved for single-player parity; in multiplayer it
// is a desync source of the original, so the sim must decide what to feed there.
// Every mask is indexed by PlanetIndex; an EMPTY mask (size 0) means the default noted on it.
// A default-constructed view is a human-branch player (no AI system) that sees every planet,
// owns nothing, and all planets are non-neutral enemies: with all flags set, everything passes.
struct PlayerView {
    // ---- mover -----------------------------------------------------------------------------
    bool is_human = false;                  // PlayerClass +0x62 (Is_Human)
    bool has_ai_system = false;             // PlayerClass +0x360 != null
    bool benefits_from_corruption = false;  // mover faction +0x10b (Benefits_From_Corruption)
    // ---- game --------------------------------------------------------------------------------
    bool multiplayer = false;               // game type 1, 2 or 4
    bool fog_disabled = false;              // galactic mode +0x340 = Planet_Auto_Reveal (GRC 0; AI fog gate only)
    // ---- per-planet state (galaxy-wide) -------------------------------------------------------
    Bitset revealed;   // LOCAL: data pack +0x2c9 revealed byte. empty => all revealed
    Bitset hidden;     // no data pack, or data pack +0x2e0 hidden byte. empty => none
    Bitset in_battle;  // in the galactic conflict list. empty => none
    Bitset corrupted;  // data pack +0x2f4 != -1 (corruption type index). empty => none
    Bitset unowned_or_neutral;  // 0x4ada70: owner id not a player, or owner faction Is_Neutral. empty => none
    Bitset ownership_visible;   // LOCAL: visibility mask (+0x1a8) bit 0x1 Planet_Ownership. empty => all visible
    Bitset local_orbit_visible; // LOCAL: visibility mask (+0x1a8) bit 0x10 Fleet_Presence. empty => all visible
    // ---- per-planet, relative to the mover ------------------------------------------------------
    Bitset held_by_ally;  // 0x4ad9f0: owner != -1, relation(mover, owner) == 0 (self/ally), control > 0. empty => none
    // AI branch of Is_Planet_Passable_For_Player (has_ai_system && !is_human), perception result:
    Bitset ai_eval_failed;            // perception evaluation failed => passable. empty => none
    Bitset ai_perceived_non_neutral;  // perceived owner resolves to a non-neutral player => flag 1,
                                      // else (no such player / neutral) => flag 0. empty => none
    // Is_Planet_Dangerous (Q6) for this mover, evaluated by the sim: starbase (+0x148 != 0) not
    // held by mover/ally; else with an AI system: danger perception failed or != 0.0; else (no
    // AI) LOCAL visibility bit 4 set and some orbiting unit not held by mover/ally. empty => none
    Bitset dangerous;

    // Is_Planet_Revealed (0x3fa420), used by the non-human AI fog gate and the AI all-pairs.
    bool is_planet_revealed(PlanetIndex p) const;
    // Is_Planet_Passable_For_Player (0x3f2510), Q5.
    bool is_passable(PlanetIndex p, const PathRequest& r) const;
    bool is_dangerous(PlanetIndex p) const { return bit(dangerous, p, false); }

    static bool bit(const Bitset& b, PlanetIndex p, bool empty_value) {
        return b.empty() ? empty_value : (p < b.size() && b.test(p));
    }
};

inline constexpr float kRejectedEdge = -1.0f;
// Q6 step 7: a dangerous planet that is corrupted, for a mover whose faction benefits from
// corruption, is accepted at cost + 100.0f (float add). Not a blockade penalty.
inline constexpr float kCorruptionSurcharge = 100.0f;

// Edge_Cost (0x3ef4c0) for the hop from -> graph.neighbour(e), where e is one of from's edge
// slots. Returns kRejectedEdge when the hop is not allowed.
float edge_cost(const PlanetGraph& graph, const PlayerView& view, const PathRequest& req,
                PlanetIndex from, uint32_t e);

enum class FleetPathStatus : uint8_t {
    Found,
    Unreachable,          // open list ran empty (original: failure)
    InvalidRequest,       // source / destination is not a graph node (original: failure)
    RecordPoolExhausted,  // more than 3N path records needed (original: null dereference)
};

// Reusable per-request scratch. Not shared between concurrent requests; reuse it freely across
// sequential requests (generation stamps avoid clearing; full reset on stamp wraparound).
class PathScratch {
public:
    struct Record {  // path record: planet + parent record (index into records_, -1 for start)
        PlanetIndex planet;
        int32_t parent;
    };
    struct OpenEntry {  // heap entry (the original's 0x18-byte entry minus the unused depth)
        int32_t record;
        float g;
        float f;
    };
    void prepare(uint32_t n, uint32_t node_count);  // called by find_fleet_path
    uint32_t generation() const { return gen_; }
    void force_generation_for_test(uint32_t g) { gen_ = g; }

    FleetPathStatus status() const { return status_; }
    uint32_t dropped_pushes() const { return dropped_; }  // pushes lost to the full heap (last search)
    uint32_t records_used() const { return static_cast<uint32_t>(records_.size()); }

    // Search state (public for the implementation; treat as opaque).
    std::vector<float> best_g_;
    std::vector<uint32_t> stamp_;
    std::vector<uint8_t> closed_;
    std::vector<Record> records_;
    std::vector<OpenEntry> heap_;  // heap_[0] unused (1-based)
    uint32_t heap_size_ = 0;
    uint32_t heap_capacity_ = 0;   // 3N + 1 slots
    uint32_t pool_capacity_ = 0;   // 3N records
    uint32_t dropped_ = 0;
    FleetPathStatus status_ = FleetPathStatus::InvalidRequest;

private:
    uint32_t gen_ = 0;
};

// Finds the fleet path source -> destination. On success `out` holds the planets from source to
// destination inclusive (same planet => [source]) and *cost (if given) the g of the popped goal
// entry. Returns false (out empty) otherwise; scratch.status() says why.
bool find_fleet_path(const PlanetGraph& graph, const PlayerView& view, const PathRequest& req,
                     PathScratch& scratch, std::vector<PlanetIndex>& out, float* cost = nullptr);

}  // namespace alamo::galactic

// SP5 (docs/SIM_PARALLEL_DESIGN.md §4.4): counter-based random streams for services.
//
// The synced SimRng is drawn in call order, which forces the services that draw to run one after another. A service
// converted to streams draws instead from a SimRng seeded per (session seed, tick, stream id, entity id): the seed is a
// splitmix64 finaliser chain over those four values (the OPT2 t10 pp_streams pattern), and the draw index is the LCG
// step count from that seed. Each entity's draws are then independent of every other entity's, of the thread that ran
// it and of the order the entities were serviced in. Same LCG, same range / percent formulas: per-draw distributions are
// unchanged; per-seed outcomes move (tier 3 draw order; tier 2 checked by the parity harness before a default switch).
//
// Every converted service keeps its synced path behind its own flag (StreamFlags), default off until its harness A/B
// passes, so goldens move one service at a time.
#pragma once
#include <cstdint>

#include "alamo/core/sim_rng.hpp"

namespace alamo::tactical::services {

inline uint64_t stream_mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

// Stream ids: one per converted service (fixed values: they are part of the outcome, never renumber).
enum class StreamId : uint32_t {
    Reinforce = 1,
    Landing = 2,
    Hangar = 3,
    Garrison = 4,
    Weapons = 5,
    Expiry = 6,
    Pads = 7,
    Fog = 8,
    Queues = 9,
    Victory = 10,
    Combat = 11,
};

inline uint32_t stream_seed(uint32_t session_seed, uint64_t tick, StreamId stream, uint64_t entity) {
    uint64_t h = stream_mix(session_seed);
    h = stream_mix(h ^ tick);
    h = stream_mix(h ^ static_cast<uint64_t>(stream));
    h = stream_mix(h ^ entity);
    return static_cast<uint32_t>(h >> 32);
}

// The stream of one entity in one tick (draw index = draws() of the returned generator).
inline SimRng stream_rng(uint32_t session_seed, uint64_t tick, StreamId stream, uint64_t entity) {
    return SimRng(stream_seed(session_seed, tick, stream, entity));
}

}  // namespace alamo::tactical::services

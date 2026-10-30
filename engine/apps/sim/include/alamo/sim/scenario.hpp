// G0 test drivers (not game logic): a seeded stand-in for human players issuing UI orders, and a
// stand-in AI controller. Used by alamo_headless (--map, --record-orders) and the tests.
//
//   RandomOrders  - UI stand-in. Runs BETWEEN ticks (like window-message input) and submits orders
//                   through SimHost::submit (scheduled: SP delay 1, recorded). It draws from its OWN
//                   SimRng, never the synced one, so a replay (which does not run it) has the same
//                   sim state. Some orders are invalid on purpose (foreign units, out of bounds,
//                   own targets) to exercise the handlers' silent rejection.
//   AiStandIn     - PlayerList-slot controller (T7): every `period` frames, staggered by player id,
//                   picks one of its movable units with the SYNCED RNG, finds the lowest-id enemy
//                   within `radius` (XY) with a spatial query on the all-objects tree and orders an
//                   attack through execute_direct (the AI path: no queue, not recorded). Replays
//                   rerun it identically.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/core/sim_rng.hpp"
#include "alamo/sim/map_spawn.hpp"
#include "alamo/sim/sim_host.hpp"
#include "alamo/tactical/spatial_tree.hpp"

namespace alamo::sim {

class RandomOrders {
public:
    explicit RandomOrders(uint32_t seed, uint32_t max_per_tick = 2) : rng_(seed ^ 0x51A7E5u), max_(max_per_tick) {}
    // Submits 0..max_per_tick orders; returns how many were accepted by the scheduler.
    uint32_t issue(SimHost& host);

private:
    SimRng rng_;
    uint32_t max_;
};

struct AiStandIn {
    uint32_t period = 30;
    float radius = 4000.0f;
    void operator()(SimHost& host, uint8_t player, uint64_t frame) const;
};

// One complete G0 run of a tactical map: session start, mode push (space map -> space mode, else
// land), spawn, AI stand-ins on every player slot, then `ticks` ticks with live UI orders
// (RandomOrders, recorded into `record` if given) or a replayed stream. Identical CRC logs for
// every worker count, and for a record run vs its replay.
struct MapRunSpec {
    uint64_t ticks = 600;
    int32_t workers = 0;
    uint32_t seed = 1;          // session seed (replay: taken from the stream)
    bool ai = true;
    bool live_orders = true;    // RandomOrders between ticks (ignored when replaying)
    commands::CommandRecording* record = nullptr;
    const commands::CommandRecording* replay = nullptr;
    bool realtime = false;      // FrameSynchronizer pacing on the steady clock
    std::vector<std::string>* trace = nullptr;
};
struct MapRunResult {
    CrcLog log;
    SpawnReport spawn;
    uint64_t ticks = 0, applied = 0, rejected = 0, orders_live = 0;
    uint64_t final_frame = 0;
    uint32_t live_objects = 0, orders_pending = 0;
    double run_ms = 0.0;
};
MapRunResult run_map(const objects::GameObjectTypes& types, const terrain::MapData& map, const TypeCrcIndex& index,
                     const MapRunSpec& spec);

}  // namespace alamo::sim

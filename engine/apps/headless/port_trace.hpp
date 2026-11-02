// alamo_headless --trace-out FILE (q2f t05): a per-tick state trace of a galactic run in the Q2TRACE 1 format of the
// Q2 reference traces (tools/probe_mod/q2_traces/README.md "Trace format"), so `q2trace.py validate` and
// `q2trace.py compare <run dir> --port-trace FILE` read a port run like a retail one: planet owners, object counts by
// type, player credits, fiscal-cycle fields and the synced-RNG state per tick.
//
// Read-only: the writer reads the galactic sim, its primary store and the synced RNG state after each tick. It never
// draws, writes or reorders sim state, so a run's CRCs are the same with and without it (wall-clock never appears in
// the file either: the same run always writes the same bytes, whatever the worker count).
//
// Conventions (mirroring the retail reader):
//   F <global frame> <mode type> <mode frame> <rng> <paused> <stack depth> 0 <last cycle> <next cycle> <cycle>
//     one line before the first tick (mode frame 0, RNG = the session seed, no objects: the retail trace's mode frame 0
//     comes before the galaxy is built, so "tick 1" holds the galaxy build in both), then one per tick.
//   S <global frame> <n live> 0 <sum of per-object CRCs> <rng crc> <ref_rng> <ref_objects> <ring>: the retail digests
//     (engine/core parity_object_crc / parity_rng_crc / StateHasher); the port has no cached object CRC (field 3 = 0)
//     and no frame-CRC ring (all ff/0, as in single-player retail).
//   O lines: every live object of the galactic primary store except starbases (no starbase objects in the retail
//     galactic store). Owner = the planet data-pack owner for planets (the store keeps none), container = GalacticSim
//     containment (unit -> fleet, fleet / surface unit / structure -> planet), fleets typed GALACTIC_FLEET.
//   G lines for planets, C lines for players (credits / cap as f32 bits), X when an object leaves; O / G / C only on a
//     change, except on keyframes (the first sample with objects, then every key_every mode frames).
#pragma once
#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace alamo::galactic {
class GalacticSim;
}

class PortStateTrace {
public:
    struct Sample {
        uint64_t global_frame = 0;
        uint32_t mode_type = 0;  // top mode kind (0 galactic)
        uint32_t rng_state = 0;
        uint32_t stack_depth = 1;
    };

    // Opens `path` and writes the header (meta lines, seed). False with `err` set when the file cannot be written.
    bool open(const std::string& path, const std::vector<std::pair<std::string, std::string>>& meta, uint32_t seed,
              std::string& err);
    bool is_open() const { return out_.is_open(); }
    // The mode-frame-0 line, before the first tick: cycle fields only, no objects.
    void start(const alamo::galactic::GalacticSim& gs, uint32_t seed);
    // After a tick: the F / S lines and every object / planet / player line that changed.
    void sample(const alamo::galactic::GalacticSim& gs, const Sample& s);
    // Type footer, end reason, "end"; false when a write failed.
    bool finish(const std::string& reason, std::string& err);

private:
    uint32_t type_index(const alamo::galactic::GalacticSim& gs, uint32_t store_type);

    std::ofstream out_;
    std::string path_;
    uint32_t key_every_ = 300;
    int64_t last_key_ = -1;
    std::map<uint32_t, std::string> objects_;  // object id -> last O line
    std::map<uint32_t, std::string> planets_;  // planet object id -> last G line
    std::map<uint32_t, std::string> players_;  // player index -> last C line
    std::map<uint32_t, std::string> types_;    // trace type index -> name (footer)
};

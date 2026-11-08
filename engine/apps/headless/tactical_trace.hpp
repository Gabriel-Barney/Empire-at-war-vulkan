// alamo_headless --skirmish-ai KIND --trace-out FILE (q4hf t02): a per-tick state trace of the run's tactical battle in the
// Q2TRACE 1 format of the retail skirmish traces (tools/probe_mod/q2_traces/README.md "Trace format", mode_type 2 space /
// 1 land), so the Q4 parity harness (tools/parity) reduces a port skirmish with the same reader as a retail one: per-object
// owner, type and health per tick (combat.damage_taken = hull lost by each faction's units), the players' credits and the
// first shot (combat.first_shot_frame). `q2trace.py validate` reads it like a retail trace.
//
// Read-only, as PortStateTrace (port_trace.hpp): the writer reads the tactical store, the combat system, the galactic
// players and the synced RNG state after each tick. It never draws, writes or reorders sim state, so a run's CRCs are the
// same with and without it, and the same run always writes the same bytes, whatever the worker count.
//
// Conventions:
//   One sample per host tick while a tactical battle runs (none before it, e.g. the synthetic kind's fleet approach); the
//     first sample writes the `start` line and is a keyframe, then every key_every battle frames.
//   F <global frame> <mode kind> <battle frame> <rng> 0 <stack depth> 0   (no fiscal-cycle fields: a tactical mode;
//     battle frame = TacticalBattle::stats().frames, the frame the --ai-log state lines show)
//   S as PortStateTrace: retail digests over the live (not dead) objects, no cached object CRC, no frame-CRC ring.
//   O lines: every alive object of the tactical primary store, by id. Owner = the store owner (the battle's player), type =
//     the tactical type index (T footer: upper-case names), health = the combat system's hull for a combat unit (the store
//     keeps the spawn value), else the store health. flags 0x02 (dead): a combat unit the combat system has removed or marks
//     dead while the store still holds the object (health 0). Container / parent / attach ids are "none".
//   C lines for the players (credits / cap as f32 bits, human, ai), X when an object leaves the store.
//   M first_shot_frame <battle frame>: the battle frame of the first tick in which the combat system fired (its shot
//     counter left 0). Retail traces show the first projectile as a PROJ_ object instead; the port's projectiles are not
//     store objects.
#pragma once
#include <cstdint>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace alamo::galactic {
class GalacticSim;
}
namespace alamo::sim {
class TacticalMode;
class TacticalBattle;
}  // namespace alamo::sim

class PortTacticalTrace {
public:
    struct Sample {
        uint64_t global_frame = 0;
        uint32_t mode_type = 2;  // top mode kind (objects::ModeKind: 1 land, 2 space)
        uint32_t rng_state = 0;
        uint32_t stack_depth = 1;
    };

    // Opens `path` and writes the header (meta lines, seed). False with `err` set when the file cannot be written.
    bool open(const std::string& path, const std::vector<std::pair<std::string, std::string>>& meta, uint32_t seed,
              std::string& err);
    bool is_open() const { return out_.is_open(); }
    // After a tick in which a tactical battle runs: the F / S lines and every object / player line that changed.
    void sample(const alamo::sim::TacticalMode& tm, const alamo::sim::TacticalBattle& tb, const alamo::galactic::GalacticSim& gs,
                const Sample& s);
    // Type footer, end reason (ticks_reached | mode_changed), "end"; false when a write failed.
    bool finish(const std::string& reason, std::string& err);

private:
    std::ofstream out_;
    std::string path_;
    uint32_t key_every_ = 300;
    int64_t last_key_ = -1;
    bool started_ = false;
    bool shot_ = false;
    std::map<uint32_t, std::string> objects_;  // object id -> last O line
    std::map<uint32_t, std::string> players_;  // player index -> last C line
    std::map<uint32_t, std::string> types_;    // tactical type index -> upper-case name (footer)
    std::set<uint32_t> combat_ids_;            // ids the combat system has listed as units (a later miss = dead)
};

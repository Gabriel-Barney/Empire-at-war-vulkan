// Tick profiler (OPT1, docs/OPT1_PLAN.md): scoped timers per sim / AI system, worker busy time, and a ring of per-tick
// records with a percentile report.
//
// Determinism: this is wall-clock data and is write-only from the sim's point of view. Nothing here may feed game
// logic, saves, replays or the CRC, and sim code must never branch on a value read from this header. Disabled (the
// default), a Scope costs one relaxed atomic load.
//
// One profiler per process: start() it before a run, call begin_tick()/end_tick() from the thread that drives the sim,
// then read the report. Two sims ticking at once in one process mix their numbers (profiling is off in tests that do).
//
// Zone values are inclusive thread time summed over all threads that ran the zone in that tick, so a zone inside a
// parallel loop can exceed the tick's wall time. The "tick" zone is wall time.
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace alamo::profile {

inline constexpr uint32_t kMaxZones = 96;
inline constexpr uint32_t kMaxThreads = 32;     // accumulator slots; more threads share slots (still correct)
inline constexpr uint32_t kMaxBusySlots = 32;   // job-system slots; slot 0 is the sim thread helping while it waits
using ZoneId = uint16_t;
inline constexpr ZoneId kNoZone = 0xFFFF;

// Registers a zone name (idempotent: the same name returns the same id). Thread-safe. kNoZone when the table is full.
ZoneId register_zone(const char* name);
const char* zone_name(ZoneId id);
uint32_t zone_count();

namespace detail {
extern std::atomic<bool> g_enabled;
}
inline bool enabled() noexcept { return detail::g_enabled.load(std::memory_order_relaxed); }

int64_t now_ns() noexcept;                          // monotonic clock
void add_zone_ns(ZoneId id, int64_t ns) noexcept;   // into the calling thread's accumulator
void add_busy_ns(uint32_t slot, int64_t ns) noexcept;  // job execution time on a job-system slot

// RAII timer. `ALAMO_PROFILE_SCOPE("name")` registers the zone once per call site.
class Scope {
public:
    explicit Scope(ZoneId id) noexcept : id_(id), t0_(enabled() && id != kNoZone ? now_ns() : -1) {}
    ~Scope() {
        if (t0_ >= 0) add_zone_ns(id_, now_ns() - t0_);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    ZoneId id_;
    int64_t t0_;
};

#define ALAMO_PROFILE_CAT2(a, b) a##b
#define ALAMO_PROFILE_CAT(a, b) ALAMO_PROFILE_CAT2(a, b)
#define ALAMO_PROFILE_SCOPE(name)                                                                           \
    static const ::alamo::profile::ZoneId ALAMO_PROFILE_CAT(alamo_pz_, __LINE__) =                           \
        ::alamo::profile::register_zone(name);                                                              \
    ::alamo::profile::Scope ALAMO_PROFILE_CAT(alamo_ps_, __LINE__)(ALAMO_PROFILE_CAT(alamo_pz_, __LINE__))

// One closed tick. Values saturate at ~4.29 s (uint32 ns).
struct TickRecord {
    uint64_t frame = 0;
    uint32_t total_ns = 0;
    uint32_t zone_ns[kMaxZones] = {};
    uint32_t busy_ns[kMaxBusySlots] = {};
};

// ---- session control (sim thread) ----
// Allocates the ring (oldest ticks are overwritten) and turns timing on. Calling it again restarts.
void start(uint32_t ring_ticks = 16384);
void stop();                       // timing off; the recorded ticks stay readable
void begin_tick(uint64_t frame);   // no-op when off
void lap_impl(const char* zone);
// Time since the previous lap / begin_tick goes to `zone` (sim thread only); no-op when off or outside a tick.
inline void lap(const char* zone) {
    if (enabled()) lap_impl(zone);
}
void end_tick();                   // closes the record
uint64_t ticks_recorded();         // ticks closed since start() (may exceed the ring size)
// The ring contents, oldest first; at most the newest `max_ticks` (the alamo_game overlay reads only its window).
// Sim thread, or while no tick runs.
std::vector<TickRecord> snapshot(uint32_t max_ticks = 0xFFFFFFFFu);

// ---- report ----
struct ZoneStat {
    std::string name;
    uint64_t active_ticks = 0;     // ticks with a non-zero value
    double mean_ms = 0, p50_ms = 0, p99_ms = 0, max_ms = 0;  // over all recorded ticks (zero when idle)
    double share = 0;              // total zone time / total tick time
};
struct WorstTick {
    uint64_t frame = 0;
    double total_ms = 0;
    std::vector<std::pair<std::string, double>> top;  // biggest zones of that tick, ms
};
struct Report {
    uint64_t ticks = 0;
    double mean_ms = 0, p50_ms = 0, p99_ms = 0, max_ms = 0;
    double budget_ms = 0;
    uint64_t over_budget = 0;
    std::vector<ZoneStat> zones;    // by share, largest first
    std::vector<WorstTick> worst;   // by total, largest first
    std::vector<double> busy_share; // per job slot: busy time / total tick time (empty slots dropped)
};
// Builds the report from the ring. budget_ms: the tick length that counts as a spike (default one 30 Hz tick).
Report build_report(const std::vector<TickRecord>& ticks, double budget_ms = 1000.0 / 30.0, uint32_t worst_count = 20);
std::string format_report(const Report& r);

}  // namespace alamo::profile

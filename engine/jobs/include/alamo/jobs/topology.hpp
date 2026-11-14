// CPU topology and the job system's pool (SIMP sp1, docs/SIM_PARALLEL_DESIGN.md 4.1), plus OS scheduling hints.
//
// The pool is one worker per physical core (SMT siblings left to the OS, the render thread and audio), on the
// performance cores only (hybrid CPUs: the highest EfficiencyClass), within one last-level-cache domain (dual-CCD parts:
// the domain with the most such cores, then the larger L3, e.g. the V-cache CCD of an X3D part), minus the sim thread
// and the AI thread. Windows CPU sets (GetSystemCpuSetInformation) give core, cache domain and efficiency class per
// logical processor; GetLogicalProcessorInformationEx gives the L3 sizes.
//
// Everything here is wall clock / scheduling only: no result the sim reads may depend on it (CLAUDE.md rule 3; results
// are already identical for every worker count).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace alamo::jobs {

struct LogicalCpu {
    uint32_t cpu_set_id = 0;   // SetThreadSelectedCpuSets id
    uint16_t group = 0;        // processor group
    uint8_t number = 0;        // logical processor number within the group
    uint8_t efficiency = 0;    // EfficiencyClass: higher = faster (P-cores on hybrid CPUs)
    uint32_t core = 0;         // physical core index (group-relative)
    uint32_t llc = 0;          // last-level-cache index (group-relative)
};

struct CpuTopology {
    std::vector<LogicalCpu> cpus;
    struct Cache {             // one L3 (or last-level) cache: its size and the logical processors sharing it
        uint16_t group = 0;
        uint64_t mask = 0;     // logical processors of `group`
        uint64_t size = 0;     // bytes
    };
    std::vector<Cache> l3;
    bool empty() const noexcept { return cpus.empty(); }
};

struct PoolPlan {
    uint32_t cores = 0;               // physical cores in the chosen domain (0: no topology information)
    uint32_t workers = 0;             // default worker count: cores - reserved threads, at least 1
    std::vector<uint32_t> cpu_set_ids;  // one logical processor per chosen core (the lowest-numbered sibling)
    bool hybrid = false;              // more than one efficiency class
    uint32_t domains = 0;             // last-level-cache domains holding performance cores
    uint32_t physical_total = 0;      // physical cores on the machine
    uint32_t logical_total = 0;
    std::string describe() const;
};

// Reads the machine's topology (empty on failure / non-Windows).
CpuTopology read_cpu_topology();
// Pure: the pool for `topo` with `reserved` threads (sim + AI) taken out of the worker count.
PoolPlan plan_pool(const CpuTopology& topo, uint32_t reserved = 2);
// The machine's plan (read once, cached).
const PoolPlan& machine_pool();

// Keeps the calling thread on `cpu_set_ids` (SetThreadSelectedCpuSets). False when unsupported or empty.
bool pin_current_thread(const std::vector<uint32_t>& cpu_set_ids);

// OS hints (wall clock only): opt the process out of power throttling (execution speed and timer resolution,
// SetProcessInformation ProcessPowerThrottling) and raise the calling thread one priority step (THREAD_PRIORITY_ABOVE_NORMAL).
// Each returns whether the OS accepted it.
bool disable_power_throttling();
bool raise_current_thread_priority();

}  // namespace alamo::jobs

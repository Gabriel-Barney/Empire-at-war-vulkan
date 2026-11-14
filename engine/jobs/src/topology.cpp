#include "alamo/jobs/topology.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <thread>
#include <tuple>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace alamo::jobs {

std::string PoolPlan::describe() const {
    char buf[200];
    std::snprintf(buf, sizeof buf, "%u physical / %u logical cores; pool %u cores (%s%u cache domain%s), %u default workers",
                  physical_total, logical_total, cores, hybrid ? "P-cores, " : "", domains, domains == 1 ? "" : "s", workers);
    return buf;
}

CpuTopology read_cpu_topology() {
    CpuTopology t;
#ifdef _WIN32
    ULONG len = 0;
    (void)GetSystemCpuSetInformation(nullptr, 0, &len, GetCurrentProcess(), 0);
    if (len > 0) {
        std::vector<unsigned char> buf(len);
        auto* first = reinterpret_cast<SYSTEM_CPU_SET_INFORMATION*>(buf.data());
        if (GetSystemCpuSetInformation(first, len, &len, GetCurrentProcess(), 0)) {
            for (ULONG off = 0; off + sizeof(DWORD) * 2 <= len;) {
                const auto* e = reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(buf.data() + off);
                if (e->Size == 0) break;
                if (e->Type == CpuSetInformation) {
                    LogicalCpu c;
                    c.cpu_set_id = e->CpuSet.Id;
                    c.group = e->CpuSet.Group;
                    c.number = e->CpuSet.LogicalProcessorIndex;
                    c.efficiency = e->CpuSet.EfficiencyClass;
                    c.core = e->CpuSet.CoreIndex;
                    c.llc = e->CpuSet.LastLevelCacheIndex;
                    t.cpus.push_back(c);
                }
                off += e->Size;
            }
        }
    }
    DWORD clen = 0;
    (void)GetLogicalProcessorInformationEx(RelationCache, nullptr, &clen);
    if (clen > 0) {
        std::vector<unsigned char> buf(clen);
        auto* first = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
        if (GetLogicalProcessorInformationEx(RelationCache, first, &clen)) {
            for (DWORD off = 0; off < clen;) {
                const auto* e = reinterpret_cast<const SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
                if (e->Size == 0) break;
                if (e->Relationship == RelationCache && e->Cache.Level == 3) {
                    CpuTopology::Cache c;
                    c.group = e->Cache.GroupMask.Group;
                    c.mask = static_cast<uint64_t>(e->Cache.GroupMask.Mask);
                    c.size = e->Cache.CacheSize;
                    t.l3.push_back(c);
                }
                off += e->Size;
            }
        }
    }
#endif
    return t;
}

PoolPlan plan_pool(const CpuTopology& topo, uint32_t reserved) {
    PoolPlan p;
    if (topo.empty()) return p;
    p.logical_total = static_cast<uint32_t>(topo.cpus.size());
    // Physical cores: (group, core) -> its logical processors' best efficiency class, cache domain and first cpu set.
    struct Core {
        uint8_t efficiency = 0;
        uint32_t llc = 0;
        uint32_t first_id = 0;
        uint8_t first_number = 0xFF;
    };
    std::map<std::pair<uint16_t, uint32_t>, Core> cores;
    for (const LogicalCpu& c : topo.cpus) {
        Core& k = cores[{c.group, c.core}];
        k.efficiency = std::max(k.efficiency, c.efficiency);
        if (c.number < k.first_number) {
            k.first_number = c.number;
            k.first_id = c.cpu_set_id;
            k.llc = c.llc;
        }
    }
    p.physical_total = static_cast<uint32_t>(cores.size());
    uint8_t top = 0, low = 0xFF;
    for (const auto& [key, k] : cores) {
        top = std::max(top, k.efficiency);
        low = std::min(low, k.efficiency);
    }
    p.hybrid = top != low;
    // Performance cores grouped by cache domain (group, llc), keys ascending.
    std::map<std::pair<uint16_t, uint32_t>, std::vector<std::pair<uint8_t, uint32_t>>> domains;  // -> (number, cpu set id)
    for (const auto& [key, k] : cores)
        if (k.efficiency == top) domains[{key.first, k.llc}].emplace_back(k.first_number, k.first_id);
    p.domains = static_cast<uint32_t>(domains.size());
    // L3 size of a domain: the cache whose mask holds the domain's first logical processor.
    auto l3_size = [&](uint16_t group, uint8_t number) -> uint64_t {
        for (const CpuTopology::Cache& c : topo.l3)
            if (c.group == group && number < 64 && (c.mask >> number) & 1u) return c.size;
        return 0;
    };
    const std::vector<std::pair<uint8_t, uint32_t>>* best = nullptr;
    size_t best_n = 0;
    uint64_t best_l3 = 0;
    for (const auto& [key, v] : domains) {
        const uint8_t first = std::min_element(v.begin(), v.end())->first;
        const uint64_t l3 = l3_size(key.first, first);
        if (!best || v.size() > best_n || (v.size() == best_n && l3 > best_l3)) {
            best = &v;
            best_n = v.size();
            best_l3 = l3;
        }
    }
    if (!best) return p;
    std::vector<std::pair<uint8_t, uint32_t>> chosen = *best;
    std::sort(chosen.begin(), chosen.end());
    p.cores = static_cast<uint32_t>(chosen.size());
    for (const auto& [number, id] : chosen) p.cpu_set_ids.push_back(id);
    p.workers = p.cores > reserved ? p.cores - reserved : 1u;
    return p;
}

const PoolPlan& machine_pool() {
    static const PoolPlan plan = [] {
        PoolPlan p = plan_pool(read_cpu_topology());
        if (p.cores == 0) {  // no topology: the old default (hardware threads - 1)
            const unsigned hc = std::thread::hardware_concurrency();
            p.workers = hc > 1 ? static_cast<uint32_t>(hc - 1) : 1u;
        }
        return p;
    }();
    return plan;
}

bool pin_current_thread(const std::vector<uint32_t>& cpu_set_ids) {
#ifdef _WIN32
    if (cpu_set_ids.empty()) return false;
    std::vector<ULONG> ids(cpu_set_ids.begin(), cpu_set_ids.end());
    return SetThreadSelectedCpuSets(GetCurrentThread(), ids.data(), static_cast<ULONG>(ids.size())) != 0;
#else
    (void)cpu_set_ids;
    return false;
#endif
}

bool disable_power_throttling() {
#ifdef _WIN32
    PROCESS_POWER_THROTTLING_STATE s{};
    s.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    s.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    s.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
#endif
    s.StateMask = 0;  // controlled and off: never throttle execution speed, honour timer resolution requests
    return SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &s, sizeof s) != 0;
#else
    return false;
#endif
}

bool raise_current_thread_priority() {
#ifdef _WIN32
    return SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL) != 0;
#else
    return false;
#endif
}

}  // namespace alamo::jobs

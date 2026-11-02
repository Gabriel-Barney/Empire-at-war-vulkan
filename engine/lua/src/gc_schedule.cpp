// Staggered, parallel scheduled collections of script VMs (OPT2 t04): see alamo/lua/gc_schedule.hpp.
#include "alamo/lua/gc_schedule.hpp"

#include <algorithm>
#include <vector>

#include "alamo/core/profile.hpp"

namespace alamo::lua {

namespace {

// The VM's slot inside the period: a mix of its automatic threshold (splitmix64 finaliser), so VMs that loaded the
// same script still get different slots once their heaps differ by a byte.
uint64_t slot_of(uint64_t threshold) {
    uint64_t x = threshold + 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

}  // namespace

bool gc_due(const LuaVm::HeapInfo& h, uint64_t tick, const GcPolicy& policy) {
    const uint64_t period = policy.period ? policy.period : 1u;
    if ((tick + slot_of(h.threshold) % period) % period != 0) return false;
    // Lua's counts are lu_mem (32-bit unsigned long on Windows x64), so neither product overflows.
    return h.bytes * 100u >= h.threshold * std::min<uint64_t>(policy.fill_percent, 100u);
}

uint32_t collect_scheduled(std::span<LuaVm* const> vms, uint64_t tick, const ParallelFor& pf, const GcPolicy& policy) {
    std::vector<LuaVm*> due;
    for (LuaVm* vm : vms)
        if (vm && std::find(due.begin(), due.end(), vm) == due.end() && !vm->gc_busy() &&
            gc_due(vm->heap_info(), tick, policy))
            due.push_back(vm);
    if (due.empty()) return 0;
    ALAMO_PROFILE_SCOPE("lua.gc.batch");  // wall time of the scheduled batch on the calling thread
    const auto n = static_cast<uint32_t>(due.size());
    const uint32_t threshold_percent = policy.threshold_percent;
    pf(n, 1, [&due, threshold_percent](uint32_t begin, uint32_t end) {
        for (uint32_t i = begin; i < end; ++i) due[i]->collect_detached(threshold_percent);
    });
    for (LuaVm* vm : due) vm->run_finalizers();
    return n;
}

}  // namespace alamo::lua

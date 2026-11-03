// Lua GC without spikes (OPT2 t04): staggered scheduled collections of a host's script VMs, run in parallel at one
// fixed point of the host's tick. Lua 5.0.2's collector is unchanged (no incremental GC): a VM still collects its
// whole heap in one go, and its automatic collection (allocation count >= GCthreshold, inside whatever script call
// allocates) still runs as the backstop. The schedule only collects a VM a little EARLIER, at a point the host picks
// where no script runs, so the automatic one rarely fires inside a script call and several VMs never stack up
// their collections inside one AI service.
//
// Schedule (rule 3: tick number and allocation counts only, never measured time): VM v is due on tick t when
//   (t + offset(v)) % period == 0   and   bytes(v) * 100 >= threshold(v) * fill_percent
// bytes / threshold are Lua's own byte count and next automatic collection point (lstate.h nblocks / GCthreshold,
// LuaVm::heap_info()). offset(v) is a hash of threshold(v): fixed between two collections of v, different from VM
// to VM, and saved with the VM (N1 writes the threshold), so a loaded game keeps every VM's slot without any new
// save data or host index.
//
// Threshold: after a collection stock Lua sets threshold = 2 x live bytes. A scheduled collection then scales it
// by threshold_percent (default 150 %: 3 x live), and the default fill (67 %) makes the VM due at 2 x live - the
// point where stock Lua would have collected. So a VM is collected about as often as before (garbage = 1 x live
// per collection), the collection is moved to the scheduled point, and the VM keeps 1 x live of allocation
// headroom per period before the automatic backstop fires. After an automatic collection (threshold 2 x live
// again) the VM is due from 1.33 x live and is back on the schedule after its next slot. Memory: at most 3 x live
// per VM instead of 2 x (script heaps are tens to hundreds of KB). Scripts' own collectgarbage(n) / gcinfo() keep
// their 5.0 meaning; no shipped FoC / EaW script calls gcinfo, uses weak tables or __gc, so collection timing is
// invisible to them.
//
// Parallel collection: the due VMs are collected through the host's ParallelFor, one VM per index. Lua 5.0.2 keeps
// all collector state in each state's own global_State (string table, object lists, byte count; nothing static and
// writable in the VM), the allocator is the CRT's thread-safe realloc, and the build patches make every collection
// independent of addresses (serial hashing, dead-key retiring: engine/lua/CMakeLists.txt). So a VM's collection
// gives the same heap whichever thread runs it. __gc finalizers run Lua code (which may call engine bindings):
// they are NOT run on the workers but afterwards on the calling thread, VM by VM in list order. (No shipped
// FoC / EaW script creates a finalizable userdata, so normally there are none.)
//
// Determinism of the list: the host passes its VMs in a container order that a save restores (players by index,
// plans by id, evaluators by name ...). Only the finalizer order depends on it.
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "alamo/core/parallel.hpp"
#include "alamo/lua/lua_vm.hpp"

namespace alamo::lua {

struct GcPolicy {
    uint32_t period = 32;              // ticks between two scheduled looks at one VM (about 1 s at 30 ticks/s); 0 = 1
    uint32_t fill_percent = 67;        // due once bytes reach this share of the automatic threshold (above 100 = 100)
    uint32_t threshold_percent = 150;  // after a scheduled collection: threshold = this share of Lua's own (>= 100)
};

// The schedule above for a VM with heap `h` on tick `tick`.
bool gc_due(const LuaVm::HeapInfo& h, uint64_t tick, const GcPolicy& policy = {});

// Collects every due VM of `vms` (null entries skipped): the collections run through `pf` (serial_for is fine),
// then pending finalizers run on the calling thread in list order. The calling thread must own every VM, no
// script may be running in any of them, and nothing else may touch them until this returns. A VM with a Lua call
// in progress is never due. Returns the number of VMs collected.
uint32_t collect_scheduled(std::span<LuaVm* const> vms, uint64_t tick, const ParallelFor& pf = serial_for,
                           const GcPolicy& policy = {});

// Teardown (OPT3 t11: a battle's AI holds dozens of plan, evaluator and free-store VMs, and closing them one by one
// made the battle-end tick). prepare_detached_close drops the null entries of `vms` and runs every VM's finalizers on
// the calling thread, VM by VM in list order (LuaVm::finalize_for_close); afterwards destroying a VM only frees its
// own heap (CRT allocator, thread-safe), so the VMs may be destroyed on any thread, in any order. The calling thread
// must own every VM and no script may be running in any of them. destroy_detached does both, the frees through `pf`
// (one VM per index), and clears the list. Game state does not depend on either: nothing is read from a closed VM.
void prepare_detached_close(std::vector<std::unique_ptr<LuaVm>>& vms);
void destroy_detached(std::vector<std::unique_ptr<LuaVm>>& vms, const ParallelFor& pf = serial_for);

}  // namespace alamo::lua

// OPT2 t04: Lua GC without spikes - staggered scheduled collections, collected in parallel (alamo/lua/gc_schedule.hpp).
//   * gc_due: below the fill share never due; at or above it due on exactly one tick per period; slots differ by VM;
//   * a __gc sentinel in each VM counts collections that ran inside a script call (automatic) and the ones the
//     schedule ran (finalizer on the owner thread after the detached collection): with the schedule the automatic
//     ones (nearly) vanish, without it there are no scheduled ones;
//   * the same multi-VM run gives identical script results, heap accounting and save images serially and on a job
//     system with 0 / 1 / 4 / 16 workers (rule 3);
//   * a run saved and loaded mid-way continues exactly like the uninterrupted run (the slot comes from saved state);
//   * no scheduled collection while a Lua call is in progress (gc_busy).
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/log.hpp"
#include "alamo/lua/gc_schedule.hpp"
#include "alamo/lua/lua_vm.hpp"
#ifdef ALAMO_LUA_HAVE_JOBS
#include "alamo/jobs/job_system.hpp"
#endif

#include <lua.h>

using namespace alamo;
using namespace alamo::lua;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static std::string run(LuaVm& vm, const std::string& code) {
    std::string err;
    if (!vm.run(code, "test", &err)) return err.empty() ? "error" : err;
    return {};
}

static double gnum(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const double d = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return d;
}

// A script VM that allocates garbage every tick. The proxy is a collection sentinel: its finalizer counts the
// collection (inside a script call = automatic, else = the schedule's run_finalizers) and arms a new proxy. `keep`
// grows slowly, so the heap and with it the threshold changes over the run. Collections during the setup chunk
// are not counted.
static const char* const kScript = R"(
auto_gc, sched_gc, in_script = 0, 0, true
local function arm()
  local p = newproxy(true)
  getmetatable(p).__gc = function()
    if in_script then auto_gc = auto_gc + 1 else sched_gc = sched_gc + 1 end
    arm()
  end
end
arm()
keep, acc = {}, 0
function Step(tick, n)
  in_script = true
  for i = 1, n do
    local t = {i, tostring(i + tick), {}}
    acc = acc + table.getn(t)
  end
  if math_mod(tick, 7) == 0 then table.insert(keep, 'k' .. tick) end
  in_script = false
end
function math_mod(a, b) return a - b * floor_div(a, b) end
function floor_div(a, b) local q = 0 while (q + 1) * b <= a do q = q + 1 end return q end
collectgarbage(64)
auto_gc, sched_gc, in_script = 0, 0, false
)";

static std::unique_ptr<LuaVm> make_vm(uint32_t i) {
    LuaVmOptions opt;
    opt.script_name = "gc" + std::to_string(i);
    auto vm = std::make_unique<LuaVm>(LuaHost{}, opt);
    CHECK(run(*vm, kScript) == "");
    return vm;
}

static void step(LuaVm& vm, uint64_t tick, uint32_t n) {
    CHECK(run(vm, "Step(" + std::to_string(tick) + ", " + std::to_string(n) + ")") == "");
}

// Per-VM allocation differs (n = base + stride x i small tables per tick), so the VMs reach their thresholds at
// different times.
struct Load {
    uint32_t base = 4, stride = 3;
};
struct RunResult {
    std::vector<std::string> trace;  // per tick: collected count, then per VM bytes / threshold
    std::vector<std::vector<uint8_t>> images;
    uint64_t scheduled = 0;
    double auto_gc = 0, sched_gc = 0;
};

static std::vector<uint8_t> image(const LuaVm& vm) {
    ByteWriter w;
    vm.write(w);
    return w.bytes();
}

static void tick_range(std::vector<std::unique_ptr<LuaVm>>& vms, uint64_t from, uint64_t to, bool schedule,
                       const ParallelFor& pf, const GcPolicy& policy, const Load& load, RunResult& r) {
    std::vector<LuaVm*> list;
    for (auto& v : vms) list.push_back(v.get());
    for (uint64_t t = from; t < to; ++t) {
        for (uint32_t i = 0; i < vms.size(); ++i) step(*vms[i], t, load.base + load.stride * i);
        const uint32_t n = schedule ? collect_scheduled(list, t, pf, policy) : 0u;
        r.scheduled += n;
        std::string line = std::to_string(n);
        for (auto& v : vms) {
            const LuaVm::HeapInfo h = v->heap_info();
            line += " " + std::to_string(h.bytes) + "/" + std::to_string(h.threshold);
        }
        r.trace.push_back(std::move(line));
    }
}

static RunResult run_vms(uint32_t count, uint64_t ticks, bool schedule, const ParallelFor& pf, const GcPolicy& policy = {},
                         const Load& load = {}) {
    std::vector<std::unique_ptr<LuaVm>> vms;
    for (uint32_t i = 0; i < count; ++i) vms.push_back(make_vm(i));
    RunResult r;
    tick_range(vms, 0, ticks, schedule, pf, policy, load, r);
    for (auto& v : vms) {
        r.images.push_back(image(*v));
        r.auto_gc += gnum(*v, "auto_gc");
        r.sched_gc += gnum(*v, "sched_gc");
    }
    return r;
}

static void test_due() {
    GcPolicy p;
    p.period = 16;
    p.fill_percent = 75;
    // Below the fill share: never due.
    LuaVm::HeapInfo h;
    h.threshold = 400000;
    h.bytes = 299999;
    uint32_t due = 0;
    for (uint64_t t = 0; t < 64; ++t) due += gc_due(h, t, p) ? 1u : 0u;
    CHECK(due == 0);
    // At or above it: exactly once per period, on the same slot every period.
    h.bytes = 300000;
    std::vector<uint64_t> ticks;
    for (uint64_t t = 0; t < 64; ++t)
        if (gc_due(h, t, p)) ticks.push_back(t);
    CHECK(ticks.size() == 4);
    for (size_t i = 1; i < ticks.size(); ++i) CHECK(ticks[i] - ticks[i - 1] == 16);
    // Different thresholds spread over the slots (not all on one tick).
    std::vector<uint32_t> per_slot(16, 0);
    for (uint64_t th = 100000; th < 100000 + 64 * 1024; th += 1024) {
        LuaVm::HeapInfo x;
        x.threshold = th;
        x.bytes = th;
        for (uint64_t t = 0; t < 16; ++t)
            if (gc_due(x, t, p)) ++per_slot[t];
    }
    uint32_t used = 0, most = 0;
    for (const uint32_t c : per_slot) {
        used += c ? 1u : 0u;
        most = c > most ? c : most;
    }
    CHECK(used >= 12 && most <= 12);  // 64 thresholds over 16 slots
    // period 0 = every tick; fill above 100 = 100.
    p.period = 0;
    p.fill_percent = 250;
    h.bytes = h.threshold;
    CHECK(gc_due(h, 3, p) && gc_due(h, 4, p));
    h.bytes = h.threshold - 1;
    CHECK(!gc_due(h, 3, p));
}

static void test_schedule_takes_over() {
    // ~50 KB live per VM, so with the default policy a VM has ~50 KB of headroom between its scheduled collection
    // point (2 x live) and its automatic one (3 x live); 1-3 small tables per tick (up to ~1.6 KB) fit one period.
    const Load light{1, 1};
    const RunResult off = run_vms(3, 1500, false, serial_for, {}, light);
    const RunResult on = run_vms(3, 1500, true, serial_for, {}, light);
    std::printf("gc: 3 VMs x 1500 ticks: unscheduled auto=%.0f; scheduled auto=%.0f sched=%.0f (collect_scheduled %llu); "
                "collected / bytes / threshold at the first and last tick: %s | %s\n",
                off.auto_gc, on.auto_gc, on.sched_gc, static_cast<unsigned long long>(on.scheduled), on.trace.front().c_str(),
                on.trace.back().c_str());
    CHECK(off.auto_gc >= 8 && off.sched_gc == 0 && off.scheduled == 0);
    CHECK(on.scheduled >= 8 && on.sched_gc == static_cast<double>(on.scheduled));
    CHECK(on.auto_gc * 10 <= off.auto_gc);  // the surprise collections inside script calls (nearly) vanish
}

static void test_workers() {
    // A short period, a low fill share and more garbage: several VMs collect on the same tick, on different workers.
    GcPolicy busy;
    busy.period = 3;
    busy.fill_percent = 55;
    const Load heavy{30, 20};
    const RunResult ref = run_vms(8, 400, true, serial_for, busy, heavy);
    uint32_t multi = 0;
    for (const std::string& line : ref.trace) multi += std::stoul(line) >= 2 ? 1u : 0u;
    std::printf("gc: 8 VMs x 400 ticks: %llu scheduled collections, %u ticks with 2+\n",
                static_cast<unsigned long long>(ref.scheduled), multi);
    CHECK(ref.scheduled > 0 && multi >= 10);
#ifdef ALAMO_LUA_HAVE_JOBS
    for (const int32_t workers : {0, 1, 4, 16}) {
        jobs::JobSystem js(jobs::JobSystemConfig{workers});
        const RunResult r = run_vms(8, 400, true, jobs::make_parallel_for(js), busy, heavy);
        CHECK(r.trace == ref.trace);
        CHECK(r.images == ref.images);
        CHECK(r.auto_gc == ref.auto_gc && r.sched_gc == ref.sched_gc);
        if (r.trace != ref.trace || r.images != ref.images) std::printf("FAIL workers=%d\n", workers);
    }
#else
    std::printf("gc: built without engine/jobs, worker-count check skipped\n");
#endif
}

static void test_save_load() {
    const uint32_t count = 3;
    std::vector<std::unique_ptr<LuaVm>> a;
    for (uint32_t i = 0; i < count; ++i) a.push_back(make_vm(i));
    RunResult ra;
    tick_range(a, 0, 250, true, serial_for, {}, {}, ra);
    // Save every VM at tick 250 and load into fresh VMs (same options), then run both to 600.
    std::vector<std::unique_ptr<LuaVm>> b;
    for (uint32_t i = 0; i < count; ++i) {
        const std::vector<uint8_t> img = image(*a[i]);
        LuaVmOptions opt;
        opt.script_name = "gc" + std::to_string(i);
        b.push_back(std::make_unique<LuaVm>(LuaHost{}, opt));
        ByteReader rd(img);
        CHECK(b.back()->read(rd));
    }
    RunResult rb;
    RunResult ra2;
    tick_range(a, 250, 600, true, serial_for, {}, {}, ra2);
    tick_range(b, 250, 600, true, serial_for, {}, {}, rb);
    CHECK(ra2.trace == rb.trace);
    CHECK(ra2.scheduled == rb.scheduled && rb.scheduled > 0);
    for (uint32_t i = 0; i < count; ++i) CHECK(image(*a[i]) == image(*b[i]));
}

static LuaVm* g_busy_vm = nullptr;
static int g_busy_seen = -1;
static int l_probe(lua_State*) {
    g_busy_seen = g_busy_vm->gc_busy() ? 1 : 0;
    LuaVm* list[] = {g_busy_vm};
    GcPolicy every;
    every.period = 1;
    every.fill_percent = 1;
    g_busy_seen += 2 * static_cast<int>(collect_scheduled(list, 0, serial_for, every));  // never collects here
    return 0;
}

static void test_busy() {
    auto vm = make_vm(0);
    CHECK(!vm->gc_busy());
    g_busy_vm = vm.get();
    lua_State* L = vm->state();
    lua_pushstring(L, "Probe");
    lua_pushcfunction(L, l_probe);
    lua_rawset(L, LUA_GLOBALSINDEX);
    CHECK(run(*vm, "Probe()") == "");
    CHECK(g_busy_seen == 1);  // busy, and not collected
    // A service with a live script thread is busy too: Probe from inside a thread.
    CHECK(run(*vm, "function T() Probe() coroutine.yield() end") == "");
    CHECK(vm->create_thread("T") >= 0);
    g_busy_seen = -1;
    vm->service_threads();
    CHECK(g_busy_seen == 1);
    CHECK(!vm->gc_busy());
    // Idle: a policy that is always due collects it.
    LuaVm* list[] = {vm.get(), nullptr, vm.get()};  // nulls skipped, duplicates collected once
    GcPolicy every;
    every.period = 1;
    every.fill_percent = 1;
    CHECK(collect_scheduled(list, 5, serial_for, every) == 1);
    CHECK(gnum(*vm, "sched_gc") == 1.0);
    g_busy_vm = nullptr;
}

int main() {
    log::set_min_level(log::Level::Error);
    test_due();
    test_schedule_takes_over();
    test_workers();
    test_save_load();
    test_busy();
    if (g_fail) {
        std::printf("alamo_lua_gc_tests: %d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("alamo_lua_gc_tests: all passed\n");
    return 0;
}

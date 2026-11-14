// N1 save / load of the script side (engine/lua): LuaVm::write / read (whole-heap persistence, src/vm_persist.cpp),
// FogScriptCells, ObjectBridge, LuaPlayerWorld and CinematicTransports.
//   * lockstep: a VM with sleeping threads, shared / open upvalues, metatables, table-keyed tables, weak tables,
//     suspended script coroutines, coroutine.wrap / string.gfind / bound-method closures, finalizers, wrappers of live
//     objects and the synced GameRandom is saved mid-run and loaded into a fresh VM; both then run in lockstep and
//     must agree on every message (pairs orders, serials, RNG draws, gcinfo), the heap accounting and the RNG state;
//   * save -> load -> save gives the same bytes (right after the load, mid-run and at the end);
//   * corrupted / truncated records are refused and leave the VM unchanged;
//   * states that cannot be saved (hooks, foreign light userdata, unknown C functions) fail cleanly;
//   * registry attachments (host bridges) are taken from the VM being read into;
//   * with ALAMO_FOC_DATA: every shipped script's top level is saved, loaded and saved again.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/players.hpp"
#include "alamo/lua/script_gaps.hpp"
#ifdef ALAMO_LUA_HAVE_FOG
#include "alamo/lua/fog_bridge.hpp"
#endif
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/test/support.hpp"
#ifdef ALAMO_LUA_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

#include <lua.h>

#include "vm_internal.hpp"  // detail::cfunction_aliases (diagnostics)

using namespace alamo;
using namespace alamo::lua;
namespace fs = std::filesystem;

using ::alamo::test::g_fail;

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
static std::vector<uint8_t> save(const LuaVm& vm) {
    ByteWriter w;
    vm.write(w);
    return w.bytes();
}
static bool load(LuaVm& vm, std::span<const uint8_t> bytes) {
    ByteReader r(bytes);
    return vm.read(r);
}
static bool same_heap(const LuaVm::HeapInfo& a, const LuaVm::HeapInfo& b) {
    return a.serial == b.serial && a.bytes == b.bytes && a.threshold == b.threshold && a.strings == b.strings;
}
static bool contains(const std::vector<std::string>& v, const std::string& s, size_t from = 0) {
    for (size_t i = from; i < v.size(); ++i)
        if (v[i] == s) return true;
    return false;
}
using ::alamo::test::write_file;
static std::string env(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? v : "";
#endif
}

// A store with three troopers (ids 1..3).
struct World {
    fs::path root;
    std::unique_ptr<DirectoryProvider> dp;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    objects::IdCounter ids;
    std::unique_ptr<objects::ObjectStore> store;
    objects::ObjectHandle h[3];
    bool ok = false;
    World() {
        root = fs::temp_directory_path() / "alamo_lua_save_test";
        std::error_code ec;
        remove_all_long(root, ec);
        write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>S.xml</File></L>");
        write_file(root / "Data/XML/S.xml",
                   "<R><GroundInfantry Name=\"Trooper\"><Tactical_Health>40</Tactical_Health>"
                   "<Behavior>SELECTABLE</Behavior></GroundInfantry></R>");
        dp = std::make_unique<DirectoryProvider>(root.string());
        ok = raw.load(*dp) && types.build(raw);
        if (!ok) return;
        store = std::make_unique<objects::ObjectStore>(objects::ModeKind::Land, objects::StoreKind::Primary, types, &ids);
        for (int i = 0; i < 3; ++i) {
            objects::SpawnParams sp;
            sp.type = raw.find("Trooper");
            sp.owner = i;
            sp.position = {static_cast<float>(i) * 10.0f, 0.0f, 0.0f};
            sp.facing = {1.0f, 0.0f, 0.0f};
            h[i] = store->create(sp);
        }
    }
    ~World() {
        store.reset();
        std::error_code ec;
        remove_all_long(root, ec);
    }
    LuaHost host(GameRandom* rng) const {
        LuaHost hst;
        hst.store = store.get();
        hst.types = &types;
        hst.rng = rng;
        return hst;
    }
};

static LuaVmOptions options() {
    LuaVmOptions o;
    o.script_name = "SaveTest";
    o.math_library = true;
    return o;
}

static const char* const kScript = R"(
function Pump() coroutine.yield(true) local ev = GetEvent() while ev do ev() ev = GetEvent() end end
function Sleep(t) local start = GetCurrentTime() while GetCurrentTime() - start < t do Pump() end end
function make_counter()
  local n = 0
  local function inc(d) n = n + (d or 1) return n end
  local function get() return n end
  return inc, get
end
inc, get = make_counter()
Base = {}
Base.__index = Base
function Base.new(v) return setmetatable({v = v}, Base) end
function Base.val(self) return self.v end
Derived = setmetatable({}, {__index = Base})
Derived.__index = Derived
function Derived.new(v) return setmetatable(Base.new(v), Derived) end
function Derived.val(self) return Base.val(self) * 2 end
keyed = {}
order_keys = {}
for i = 1, 12 do local k = {i = i} order_keys[i] = k keyed[k] = i * 10 end
weak = setmetatable({}, {__mode = 'k'})
weakv = setmetatable({}, {__mode = 'v'})
for i = 1, 6 do weak[order_keys[i]] = i weak[{}] = -i weakv[i] = {} weakv[i + 10] = order_keys[i] end
function order(t) local s = '' for k, v in pairs(t) do s = s .. tostring(v) .. ',' end return s end
gen = coroutine.create(function(a) local s = a while true do s = s + coroutine.yield(s) end end)
coroutine.resume(gen, 5)
wrapped = coroutine.wrap(function() local i = 0 while true do i = i + 1 coroutine.yield(i) end end)
words = string.gfind('alpha beta gamma delta epsilon zeta eta theta iota kappa lambda mu nu xi', '%a+')
get_id = Obj1.Get_Object_ID
function make_proxy(tag)
  local p = newproxy(true)
  getmetatable(p).__gc = function() print('gc ' .. tag) end
  return p
end
holder = {p = make_proxy('B')}
function Worker(tag, period)
  local count = 0
  local function bump() count = count + 1 return count end
  local a, d = Base.new(3), Derived.new(4)
  local churn = {}
  while true do
    local c = bump()
    local ok, g = coroutine.resume(gen, c)
    local k = {c = c}
    keyed[k] = 1000 + c
    churn[c] = k
    if c > 3 then keyed[churn[c - 3]] = nil churn[c - 3] = nil end
    local kb, th = gcinfo()
    print(tag .. ' c=' .. c .. ' inc=' .. inc(1) .. ' get=' .. get() .. ' v=' .. a:val() .. '/' .. d:val() ..
          ' gen=' .. tostring(g) .. ' wr=' .. wrapped() .. ' w=' .. (words() or 'none') ..
          ' r=' .. GameRandom(1, 1000) .. ' f=' .. GameRandom.Get_Float() .. ' m=' .. math.random(1, 50) ..
          ' id=' .. get_id() .. ' new=' .. tostring({}) .. ' gc=' .. kb .. '/' .. th .. ' keyed=' .. order(keyed))
    Sleep(period)
  end
end
function Watcher()
  local byobj = {}
  for i = 1, table.getn(objs) do byobj[objs[i]] = i * 7 end
  local p = make_proxy('A')
  local round = 0
  while true do
    round = round + 1
    local s = ''
    for k, v in pairs(byobj) do s = s .. k.Get_Object_ID() .. ':' .. v .. ' ' end
    print('W' .. round .. ' ' .. s .. 'weak=' .. order(weak) .. ' weakv=' .. order(weakv) ..
          ' tv=' .. tostring(ThreadValue('x')) .. ' t=' .. GetCurrentTime() .. ' fr=' .. GetCurrentTime.Frame())
    ThreadValue.Set('x', round)
    if round == 45 then p = nil end
    if math.mod(round, 10) == 0 then collectgarbage() end
    Pump()
  end
end
function Leaker()
  local secret = 'L'
  local n = 0
  leaked = function() n = n + 1 return secret .. n end
  while true do Pump() end
end
function Doomed()
  local t = {1, 2, 3}
  local function f() return t[1] end
  while true do Pump() f() end
end
function OnEvent() print('event ' .. GetCurrentTime() .. ' ' .. GetCurrentTime.Frame()) end
objs = {Obj1, Obj2, Obj3}
w1 = Create_Thread('Worker', 'A', 0.1)
w2 = Create_Thread('Worker', 'B', 0.05)
wt = Create_Thread('Watcher')
lk = Create_Thread('Leaker')
dm = Create_Thread('Doomed')
)";

// Garbage coroutines at save time: one whose open upvalue a live closure (leaked2) still uses, one with only garbage
// closures (a husk that still owns an open upvalue); plus a userdata waiting for its finalizer.
static const char* const kBeforeSave = R"(
local co = coroutine.create(function() local secret = 'S' local n = 0 leaked2 = function() n = n + 1 return secret .. n end coroutine.yield() end)
coroutine.resume(co)
local d = coroutine.create(function() local t = {7} local function f() return t end coroutine.yield() end)
coroutine.resume(d)
holder.p = nil
)";

static void step(LuaVm& vm, uint32_t f) {
    vm.set_time(f / 30.0);
    vm.set_frame(f);
    if (f % 7 == 3) vm.post_event("OnEvent");
    vm.service_threads();
}

// Returns the mid-run save (input of the corruption test).
static std::vector<uint8_t> test_lockstep(const World& w) {
    GameRandom rng_a(1234, 99);
    ObjectBridge bridge_a, bridge_b;
    const LuaHost host_a = w.host(&rng_a);
    LuaVm a(host_a, options());
    attach_object_bridge(a, &bridge_a);
    a.set_object_global("Obj1", w.h[0]);
    a.set_object_global("Obj2", w.h[1]);
    a.set_object_global("Obj3", w.h[2]);
    CHECK(run(a, kScript) == "");
    for (uint32_t f = 0; f < 40; ++f) {
        if (f == 20) CHECK(run(a, "Thread.Kill(dm)") == "");
        step(a, f);
    }
    CHECK(run(a, kBeforeSave) == "");
    a.post_event("OnEvent");  // pending in the GetEvent queue at save time
    CHECK(a.thread_count() == 4);
    for (const std::string& e : a.errors()) std::printf("  A error: %s\n", e.c_str());

    const std::vector<uint8_t> saved = save(a);
    CHECK(a.save_error().empty());
    if (!a.save_error().empty()) std::printf("  save error: %s\n", a.save_error().c_str());
    std::printf("  lockstep save: %zu bytes, heap %llu bytes, serial %u, %u strings\n", saved.size(),
                static_cast<unsigned long long>(a.heap_info().bytes), a.heap_info().serial, a.heap_info().strings);
    CHECK(save(a) == saved);  // write() changes nothing
    // Cross-compiler comparison: ALAMO_LUA_SAVE_DUMP=<file> writes this record.
    if (const std::string dump = env("ALAMO_LUA_SAVE_DUMP"); !dump.empty())
        std::ofstream(dump, std::ios::binary).write(reinterpret_cast<const char*>(saved.data()),
                                                     static_cast<std::streamsize>(saved.size()));

    GameRandom rng_b = rng_a;
    const LuaHost host_b = w.host(&rng_b);
    LuaVm b(host_b, options());
    attach_object_bridge(b, &bridge_b);
    CHECK(load(b, saved));
    CHECK(attached_object_bridge(b) == &bridge_b);  // the attachment is the reader's own
    CHECK(save(b) == saved);
    CHECK(same_heap(a.heap_info(), b.heap_info()));
    CHECK(b.thread_count() == a.thread_count());
    CHECK(b.thread_active(0) && b.thread_active(1) && b.thread_active(2) && b.thread_active(3) && !b.thread_active(4));
    CHECK(gnum(b, "w2") == 1.0);

    const size_t ma0 = a.messages().size(), ea0 = a.errors().size();
    bool msg_ok = true, heap_ok = true;
    std::set<uint64_t> thresholds;
    std::vector<uint8_t> mid_a, mid_b;
    for (uint32_t f = 40; f < 200; ++f) {
        for (LuaVm* vm : {&a, &b}) {
            if (f % 10 == 5) CHECK(run(*vm, "print('leaked ' .. leaked() .. ' ' .. leaked2())") == "");
            if (f == 100) CHECK(run(*vm, "Thread.Kill(lk)") == "");
            step(*vm, f);
        }
        const auto& m_a = a.messages();
        const auto& m_b = b.messages();
        if (m_a.size() - ma0 != m_b.size()) {
            if (msg_ok) std::printf("  tick %u: %zu vs %zu messages\n", f, m_a.size() - ma0, m_b.size());
            msg_ok = false;
        } else {
            for (size_t i = 0; i < m_b.size() && msg_ok; ++i)
                if (m_a[ma0 + i] != m_b[i]) {
                    std::printf("  tick %u: message %zu differs\n    A: %s\n    B: %s\n", f, i, m_a[ma0 + i].c_str(),
                                m_b[i].c_str());
                    msg_ok = false;
                }
        }
        if (!same_heap(a.heap_info(), b.heap_info()) || !(rng_a == rng_b) || a.thread_count() != b.thread_count() ||
            a.errors().size() - ea0 != b.errors().size()) {
            if (heap_ok) std::printf("  tick %u: heap / rng / thread state differs\n", f);
            heap_ok = false;
        }
        thresholds.insert(a.heap_info().threshold);
        if (f == 120) {
            mid_a = save(a);
            mid_b = save(b);
        }
    }
    CHECK(msg_ok);
    CHECK(heap_ok);
    CHECK(rng_a == rng_b);
    CHECK(thresholds.size() >= 3);  // collections ran after the load
    CHECK(!mid_a.empty() && mid_a == mid_b);
    CHECK(save(a) == save(b));
    CHECK(a.errors().size() == ea0 && b.errors().empty());
    for (const std::string& e : b.errors()) std::printf("  B error: %s\n", e.c_str());
    // The finalizers ran in both (B: the userdata pending at save time, A: the one dropped after the load).
    CHECK(contains(b.messages(), "[print] gc B") && contains(b.messages(), "[print] gc A"));
    CHECK(contains(a.messages(), "[print] gc B", ma0) && contains(a.messages(), "[print] gc A", ma0));
    CHECK(contains(b.messages(), "[print] leaked L1 S1"));
    CHECK(std::any_of(b.messages().begin(), b.messages().end(), [](const std::string& m) { return m.rfind("[print] event ", 0) == 0; }));
    CHECK(b.thread_count() == 3);
    return saved;
}

// Two VMs that run the same script write the same bytes, whatever the allocator does. Keys removed from a table stay
// in their node ("dead keys"); once their strings are collected and re-created, stock 5.0.2 would match the new string
// against the dead node only when the allocator happened to reuse the address (the CMake dead-key patch). VM B gets
// malloc noise between the collection and the re-creation so its strings land elsewhere.
static void test_twins(const World& w) {
    static const char* const script = R"(
store = {}
function churn(r)
  local t = {}
  for i = 1, 40 do t['key' .. r .. '_' .. i] = i end
  for i = 1, 40, 2 do t['key' .. r .. '_' .. i] = nil end
  store[r] = t
  reg_like = reg_like or {}
  reg_like['attach.' .. r] = r
  reg_like['attach.' .. r] = nil
end
function rekey(r)
  local t = store[r]
  for i = 1, 40 do t['key' .. r .. '_' .. i] = i * 2 end
  reg_like['attach.' .. r] = -r
  local s = ''
  for k, v in pairs(t) do s = s .. k .. '=' .. v .. ' ' end
  for k, v in pairs(reg_like) do s = s .. k .. '=' .. v .. ' ' end
  print(s)
end
)";
    GameRandom ra(9, 9), rb(9, 9);
    const LuaHost ha = w.host(&ra), hb = w.host(&rb);
    LuaVm a(ha, options()), b(hb, options());
    CHECK(run(a, script) == "" && run(b, script) == "");
    std::vector<void*> noise;
    bool same = true;
    for (int r = 1; r <= 12; ++r) {
        const std::string n = std::to_string(r);
        for (LuaVm* vm : {&a, &b}) {
            CHECK(run(*vm, "churn(" + n + ")") == "");
            CHECK(run(*vm, "collectgarbage()") == "");
            if (vm == &b)
                for (size_t k = 0; k < 64; ++k) noise.push_back(std::malloc(16 + (k % 8) * 8));
            CHECK(run(*vm, "rekey(" + n + ")") == "");
        }
        same = same && save(a) == save(b) && a.messages() == b.messages();
    }
    for (void* p : noise) std::free(p);
    noise.clear();
    CHECK(same);
    CHECK(a.messages() == b.messages());
    CHECK(same_heap(a.heap_info(), b.heap_info()));
    // Twins of the lockstep script too, saved at several points.
    GameRandom r1(1234, 99), r2(1234, 99);
    const LuaHost h1 = w.host(&r1), h2 = w.host(&r2);
    LuaVm x(h1, options()), y(h2, options());
    for (LuaVm* vm : {&x, &y}) {
        vm->set_object_global("Obj1", w.h[0]);
        vm->set_object_global("Obj2", w.h[1]);
        vm->set_object_global("Obj3", w.h[2]);
        CHECK(run(*vm, kScript) == "");
    }
    bool twins = true;
    for (uint32_t f = 0; f < 120; ++f) {
        step(x, f);
        noise.push_back(std::malloc(40));
        step(y, f);
        if (f % 20 == 19) twins = twins && save(x) == save(y);
    }
    for (void* p : noise) std::free(p);
    CHECK(twins);
    CHECK(x.messages() == y.messages());
}

// Same bytes with a recomputed checksum (u32 version, u8 saved, u64 FNV-1a of the rest).
static void reseal(std::vector<uint8_t>& d) {
    if (d.size() < 13) return;
    const uint64_t sum = fnv1a64(d.data() + 13, d.size() - 13);
    for (int i = 0; i < 8; ++i) d[static_cast<size_t>(5 + i)] = static_cast<uint8_t>(sum >> (8 * i));
}

static void test_corruption(const World& w, const std::vector<uint8_t>& good) {
    if (good.size() < 64) {
        CHECK(false);
        return;
    }
    GameRandom rng(7, 7);
    const LuaHost host = w.host(&rng);
    auto fresh = [&] {
        auto vm = std::make_unique<LuaVm>(host, options());
        CHECK(run(*vm, "x = {1, 2, 3} function f() return x[2] end") == "");
        return vm;
    };
    std::unique_ptr<LuaVm> t = fresh();
    const std::vector<uint8_t> before = save(*t);
    // Truncated: every short length, then lengths spread over the record.
    size_t refused = 0, tries = 0;
    for (size_t len = 0; len < good.size(); len += (len < 96 ? 1 : good.size() / 300 + 1)) {
        ++tries;
        if (!load(*t, std::span<const uint8_t>(good.data(), len))) ++refused;
    }
    CHECK(refused == tries);
    // Trailing bytes.
    std::vector<uint8_t> longer = good;
    longer.push_back(0);
    CHECK(!load(*t, longer));
    // Damaged bytes: the checksum refuses every one.
    uint32_t seed = 12345;
    auto next = [&] {
        seed = seed * 1664525u + 1013904223u;
        return seed >> 8;
    };
    refused = 0;
    for (int i = 0; i < 300; ++i) {
        std::vector<uint8_t> d = good;
        d[next() % d.size()] ^= static_cast<uint8_t>(1 + next() % 255);
        if (!load(*t, d)) ++refused;
    }
    CHECK(refused == 300);
    // A record write() refused, a wrong version.
    {
        ByteWriter bad;
        bad.u32(1).boolean(false).str("why");
        CHECK(!load(*t, bad.bytes()));
        std::vector<uint8_t> d = good;
        d[0] ^= 0x40;
        CHECK(!load(*t, d));
    }
    CHECK(save(*t) == before);
    // Damaged bytes behind a valid checksum (the structural checks): refused, or accepted as a consistent heap that
    // the collector can walk and save again.
    size_t accepted = 0;
    for (int i = 0; i < 400; ++i) {
        std::vector<uint8_t> d = good;
        const size_t at = 13 + next() % (d.size() - 13);
        d[at] ^= static_cast<uint8_t>(1u << (next() % 8));
        reseal(d);
        if (load(*t, d)) {
            ++accepted;
            run(*t, "collectgarbage()");
            ByteWriter again;
            t->write(again);
            t = fresh();
        } else if (save(*t) != before) {
            CHECK(false);
            std::printf("  refused record %d changed the VM\n", i);
            t = fresh();
        }
    }
    std::printf("  structural corruption: %zu of 400 single-bit flips accepted (values that stay valid)\n", accepted);
    CHECK(save(*t) == before);
    CHECK(run(*t, "y = f()") == "" && gnum(*t, "y") == 2.0);
}

static int count_hook_calls = 0;
static void test_hook(lua_State*, lua_Debug*) { ++count_hook_calls; }
// Unique code (an identical body could be folded with a library function by the linker's ICF).
static int test_cfunction(lua_State* L) {
    lua_pushstring(L, "alamo test_cfunction 0x5eed");
    lua_pushnumber(L, 271828.0);
    return 2;
}

static void test_unsaveable(const World& w) {
    GameRandom rng(1, 2);
    const LuaHost host = w.host(&rng);
    LuaVm vm(host, options());
    CHECK(run(vm, "t = {1, 2} function Idle() while true do coroutine.yield(true) end end Create_Thread('Idle')") == "");
    vm.service_threads();
    lua_State* L = vm.state();
    LuaVm other(host, options());
    const std::vector<uint8_t> other_before = save(other);
    auto refused = [&](const char* what) {
        const std::vector<uint8_t> d = save(vm);
        CHECK(!vm.save_error().empty());
        if (vm.save_error().empty()) std::printf("  not refused: %s\n", what);
        CHECK(!load(other, d));
        CHECK(save(other) == other_before);
    };
    // A debug hook.
    lua_sethook(L, test_hook, LUA_MASKCOUNT, 1000);
    refused("hook");
    lua_sethook(L, nullptr, 0, 0);
    CHECK(!save(vm).empty() && vm.save_error().empty());
    // A light userdata that is not a registry attachment.
    static int dummy = 0;
    lua_pushstring(L, "lud");
    lua_pushlightuserdata(L, &dummy);
    lua_rawset(L, LUA_GLOBALSINDEX);
    refused("light userdata global");
    CHECK(run(vm, "lud = nil") == "");
    save(vm);
    CHECK(vm.save_error().empty());
    // A light userdata key in the registry (it would hash by address).
    lua_pushlightuserdata(L, &dummy);
    lua_pushboolean(L, 1);
    lua_rawset(L, LUA_REGISTRYINDEX);
    refused("light userdata key");
    lua_pushlightuserdata(L, &dummy);
    lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    // A C function outside the name table.
    LuaVm vm2(host, options());
    lua_State* L2 = vm2.state();
    lua_pushstring(L2, "cfn");
    lua_pushcfunction(L2, test_cfunction);
    lua_rawset(L2, LUA_GLOBALSINDEX);
    ByteWriter wr;
    vm2.write(wr);
    CHECK(!vm2.save_error().empty());
    CHECK(!load(other, wr.bytes()));
    CHECK(run(vm2, "cfn = nil collectgarbage()") == "");
    CHECK(!save(vm2).empty() && vm2.save_error().empty());
    // A fresh VM saves and reloads to the same bytes.
    LuaVm v3(host, options());
    const std::vector<uint8_t> d3 = save(v3);
    LuaVm v4(host, options());
    CHECK(load(v4, d3));
    CHECK(save(v4) == d3);
}

static void test_attachments(const World& w) {
    GameRandom rng(3, 4);
    const LuaHost host = w.host(&rng);
    static int mine = 0, theirs = 0;
    auto attach = [](LuaVm& vm, void* p) {
        lua_State* L = vm.state();
        lua_pushstring(L, "test.attach");
        lua_pushlightuserdata(L, p);
        lua_rawset(L, LUA_REGISTRYINDEX);
    };
    auto attached = [](LuaVm& vm, int* type) {
        lua_State* L = vm.state();
        lua_pushstring(L, "test.attach");
        lua_rawget(L, LUA_REGISTRYINDEX);
        *type = lua_type(L, -1);
        void* p = lua_touserdata(L, -1);
        lua_pop(L, 1);
        return p;
    };
    LuaVm a(host, options());
    attach(a, &mine);
    CHECK(run(a, "v = 42") == "");
    const std::vector<uint8_t> d = save(a);
    CHECK(a.save_error().empty());
    LuaVm b(host, options());
    attach(b, &theirs);
    CHECK(load(b, d));
    int type = LUA_TNIL;
    CHECK(attached(b, &type) == &theirs && type == LUA_TLIGHTUSERDATA);
    CHECK(gnum(b, "v") == 42.0);
    LuaVm c(host, options());
    CHECK(load(c, d));
    CHECK(attached(c, &type) == nullptr && type == LUA_TLIGHTUSERDATA);
}

static void test_bridges() {
    // ObjectBridge ("story.bridge").
    {
        ObjectBridge a;
        a.orders_issued = 9;
        a.set_hint(5, "spawnpoint");
        ScriptObjectState& s = a.state(5);
        s.hidden = true;
        s.selectable = 0;
        s.shield = 0.25f;
        s.attack_target = 3;
        s.animation = "Idle";
        s.abilities[3] = AbilityState{true, false};
        s.abilities[1] = AbilityState{false, true};
        a.state(2).invulnerable = true;
        RemainingObjectState& rs = a.remaining_state(7);
        rs.behaviors[4] = false;
        rs.behaviors[2] = true;
        rs.divert_goal = {1.0f, 2.0f, 3.0f};
        const uint32_t r1 = a.new_reinforcement();
        a.new_reinforcement();
        a.finish_reinforcement(r1, {4, 5});
        a.watch_ability(9);  // S7 t15 A1: watched units' "unit ability finished" counts (bridge save version 2)
        a.watch_ability(4);
        a.ability_finished(9);
        a.ability_finished(9);
        ByteWriter w;
        a.write(w);
        ObjectBridge b;
        ByteReader r(w.bytes());
        CHECK(b.read(r) && r.done());
        CHECK(b.ability_signals(9) == 2 && b.ability_signals(4) == 0 && b.watch_ability(4) == 0);
        StateHasher ha, hb;
        a.hash(ha);
        b.hash(hb);
        CHECK(ha.digest() == hb.digest());
        ByteWriter w2;
        b.write(w2);
        CHECK(w2.bytes() == w.bytes());
        CHECK(b.new_reinforcement() == 3 && b.reinforcement(r1) && b.reinforcement(r1)->units.size() == 2);
        ObjectBridge c;
        c.set_hint(1, "x");
        ByteWriter wc;
        c.write(wc);
        for (size_t len = 0; len < w.size(); ++len) {
            ByteReader t(std::span<const uint8_t>(w.bytes().data(), len));
            CHECK(!c.read(t) || !t.ok());
        }
        ByteWriter wc2;
        c.write(wc2);
        CHECK(wc2.bytes() == wc.bytes());
    }
#if defined(ALAMO_LUA_PLAYERS)
    // LuaPlayerWorld ("story.players").
    {
        LuaPlayerWorld a;
        std::vector<LuaPlayerSetup> setup(3);
        setup[0].faction = "Empire";
        setup[0].human = true;
        setup[0].credits = 5000.0f;
        setup[1].faction = "Rebel";
        setup[1].ai = true;
        setup[1].difficulty = "Hard_Default";
        setup[2].faction = "Neutral";
        setup[2].neutral = true;
        a.set_players(setup);
        a.set_relation(0, 1, PlayerRelation::Ally);
        a.set_local_player(1);
        a.set_ai_suspended(true);
        a.states()[0].tech_override[12] = true;
        a.states()[0].tech_override[4] = false;
        a.states()[1].advisor_hints["b"] = true;
        a.states()[1].advisor_hints["a"] = false;
        a.states()[2].selected = 8;
        a.purge_goal_requests = {1, 0};
        a.desire_bonuses.push_back(DesireBonusRequest{1, "Conquer", 6, 2.5, -1.0});
        a.tactical_releases.push_back(TacticalCreditRelease{0, 100.0});
        a.budget_flushes.push_back(BudgetFlushRequest{1, "Defense"});
        ByteWriter w;
        a.write(w);
        LuaPlayerWorld b;
        ByteReader r(w.bytes());
        CHECK(b.read(r) && r.done());
        StateHasher ha, hb;
        a.hash(ha);
        b.hash(hb);
        CHECK(ha.digest() == hb.digest());
        CHECK(b.relation(1, 0) == PlayerRelation::Ally && b.local_player() == 1 && b.ai_suspended());
        CHECK(b.credits(0) == 5000.0f);
        LuaPlayerWorld c;
        c.set_players(std::vector<LuaPlayerSetup>(2));
        StateHasher hc0;
        c.hash(hc0);
        for (size_t len = 0; len < w.size(); ++len) {
            ByteReader t(std::span<const uint8_t>(w.bytes().data(), len));
            CHECK(!c.read(t) || !t.ok());
        }
        StateHasher hc1;
        c.hash(hc1);
        CHECK(hc0.digest() == hc1.digest());
    }
#endif
    // CinematicTransports (tactical host).
    {
        CinematicTransports a;
        a.add(10, 1, 0.5f, 2.0f, true);
        a.add(12, 0, 0.0f, 1.0f, false);
        std::vector<CinematicStep> out;
        const auto frames = [](objects::ObjectId, uint8_t) { return 30u; };
        for (uint32_t f = 100; f < 140; ++f) a.service(f, frames, out);
        ByteWriter w;
        a.write(w);
        CinematicTransports b;
        ByteReader r(w.bytes());
        CHECK(b.read(r) && r.done());
        StateHasher ha, hb;
        a.hash(ha);
        b.hash(hb);
        CHECK(ha.digest() == hb.digest());
        // Both continue identically.
        std::vector<CinematicStep> oa, ob;
        for (uint32_t f = 140; f < 260; ++f) {
            a.service(f, frames, oa);
            b.service(f, frames, ob);
        }
        CHECK(oa.size() == ob.size());
        StateHasher ha2, hb2;
        a.hash(ha2);
        b.hash(hb2);
        CHECK(ha2.digest() == hb2.digest());
        CinematicTransports c;
        for (size_t len = 0; len < w.size(); ++len) {
            ByteReader t(std::span<const uint8_t>(w.bytes().data(), len));
            CHECK(!c.read(t) || !t.ok());
        }
        CHECK(c.entries().empty());
    }
#ifdef ALAMO_LUA_HAVE_FOG
    // FogScriptCells (Reveal snapshots).
    {
        FogScriptCells a;
        tactical::fog::FogCells c1, c2, c3;
        c1.player = 0;
        c1.cells = {1, 2, 3};
        c2.player = 1;
        c2.cells = {7};
        c3.player = 0;
        a.add(c1);
        const int32_t mid = a.add(c2);
        a.add(c3);
        a.erase(mid);
        ByteWriter w;
        a.write(w);
        FogScriptCells b;
        ByteReader r(w.bytes());
        CHECK(b.read(r) && r.done());
        CHECK(b.entries().size() == 2 && b.entries()[0].cells.cells == c1.cells && b.entries()[1].id == 3);
        CHECK(a.add(c1) == b.add(c1));  // the next id is kept
        FogScriptCells c;
        for (size_t len = 0; len < w.size(); ++len) {
            ByteReader t(std::span<const uint8_t>(w.bytes().data(), len));
            CHECK(!c.read(t) || !t.ok());
        }
        CHECK(c.entries().empty() && c.add(c1) == 1);
    }
#endif
}

// Every shipped script's top level (with its requires): save, load into a fresh VM, save again; both VMs then run a
// few services in lockstep.
static void test_shipped_scripts() {
    const std::string root = env("ALAMO_FOC_DATA");
    if (root.empty()) {
        std::printf("  ALAMO_FOC_DATA unset: shipped scripts skipped\n");
        return;
    }
#ifdef ALAMO_LUA_HAVE_VFS
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    const std::string eaw = env("ALAMO_EAW_DATA");
    cfg.eaw_root = eaw;
    std::string verr;
    std::unique_ptr<vfs::Vfs> files = vfs::Vfs::create(cfg, &verr);
    if (!files) {
        std::printf("  vfs: %s\n", verr.c_str());
        return;
    }
    std::vector<std::string> paths;
    for (const std::string& p : files->list("Data\\Scripts\\**")) {
        std::string lower = p;
        for (char& ch : lower) ch = static_cast<char>((ch >= 'A' && ch <= 'Z') ? ch - 'A' + 'a' : ch);
        if (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".lua") == 0) paths.push_back(p);
    }
    std::sort(paths.begin(), paths.end());
    LuaHost host;
    host.files = files.get();
    uint32_t ran = 0, saved = 0, identical = 0, lockstep = 0;
    std::string failures;
    for (const std::string& path : paths) {
        std::vector<uint8_t> bytes;
        if (!files->read(path, bytes)) continue;
        GameRandom rng_a(5, 6);
        LuaHost host_a = host;
        host_a.rng = &rng_a;
        LuaVm a(host_a, LuaVmOptions{path, false});
        std::string err;
        if (!a.run(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path, &err)) continue;
        ++ran;
        // A thread on the script's main entry when it has one (it may fail: no host systems).
        a.run("if type(main) == 'function' then Create_Thread('main') end", "probe");
        a.set_time(0.0);
        a.service_threads();
        const std::vector<uint8_t> d = save(a);
        if (!a.save_error().empty()) {
            if (failures.size() < 2000) failures += "  " + path + ": " + a.save_error() + "\n";
            continue;
        }
        ++saved;
        GameRandom rng_b = rng_a;
        LuaHost host_b = host;
        host_b.rng = &rng_b;
        LuaVm b(host_b, LuaVmOptions{path, false});
        if (!load(b, d)) {
            if (failures.size() < 2000) failures += "  " + path + ": load refused\n";
            continue;
        }
        if (save(b) == d) ++identical;
        bool same = true;
        const size_t m0 = a.messages().size(), e0 = a.errors().size();
        for (int f = 1; f <= 8; ++f) {
            a.set_time(f / 30.0);
            b.set_time(f / 30.0);
            a.service_threads();
            b.service_threads();
            same = same && same_heap(a.heap_info(), b.heap_info()) && a.thread_count() == b.thread_count();
        }
        same = same && a.messages().size() - m0 == b.messages().size() && a.errors().size() - e0 == b.errors().size() &&
               rng_a == rng_b && save(a) == save(b);
        if (same) ++lockstep;
        else if (failures.size() < 2000) failures += "  " + path + ": not in lockstep after the load\n";
    }
    std::printf("  shipped scripts: %u ran, %u saved, %u reloaded to identical bytes, %u in lockstep\n", ran, saved,
                identical, lockstep);
    if (!failures.empty()) std::printf("%s", failures.c_str());
    CHECK(ran > 0 && saved == ran && identical == saved && lockstep == saved);
#else
    std::printf("  no vfs: shipped scripts skipped\n");
#endif
}

int main() {
    log::set_min_level(log::Level::Error);
    World w;
    CHECK(w.ok);
    if (!w.ok) return 1;
    {
        // C functions saved under another name (shared dispatchers; with MSVC's /OPT:ICF also functions the linker
        // folded). Printed so the MSVC and clang logs can be compared.
        const std::vector<std::string> al = detail::cfunction_aliases();
        uint64_t h = 0xcbf29ce484222325ull;
        for (const std::string& a : al) h = fnv1a64(a.data(), a.size(), h);
        std::printf("C function aliases: %zu, digest %016llx\n", al.size(), static_cast<unsigned long long>(h));
        if (!env("ALAMO_LUA_SAVE_ALIASES").empty())
            for (const std::string& a : al) std::printf("  alias %s\n", a.c_str());
    }
    std::printf("lockstep\n");
    const std::vector<uint8_t> saved = test_lockstep(w);
    std::printf("twins\n");
    test_twins(w);
    std::printf("corruption\n");
    test_corruption(w, saved);
    std::printf("unsaveable\n");
    test_unsaveable(w);
    std::printf("attachments\n");
    test_attachments(w);
    std::printf("bridges\n");
    test_bridges();
    std::printf("shipped scripts\n");
    test_shipped_scripts();
    std::printf(g_fail ? "lua save tests: %d FAILED\n" : "lua save tests: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

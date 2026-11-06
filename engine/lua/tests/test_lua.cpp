// engine/lua tests: VM basics, binding registry, determinism of math / random through the bindings,
// coroutine scheduling, dead-object handles, owner thread, coverage report; with ALAMO_FOC_DATA /
// ALAMO_EAW_DATA: compile every shipped script, run its top level, static-scan its calls.
// Reports: <project>/logs/lua_binding_coverage.txt, logs/lua_scripts_<game>.txt.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "alamo/core/dmath.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/lua/bindings.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/script_loader.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#ifdef ALAMO_LUA_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

using namespace alamo;
using namespace alamo::lua;
namespace fs = std::filesystem;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __builtin_FILE(), __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

#ifndef ALAMO_LUA_LOG_DIR
#define ALAMO_LUA_LOG_DIR "logs"
#endif

// Runs `code`; returns the error text ("" on success).
static std::string run(LuaVm& vm, const std::string& code) {
    std::string err;
    if (!vm.run(code, "test", &err)) return err.empty() ? "error" : err;
    return {};
}

// Global as string / number (after running code that sets it).
static std::string gstr(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "<nil>";
    lua_pop(L, 1);
    return out;
}
static double gnum(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const double d = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return d;
}

static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

static void write_log(const std::string& name, const std::string& text) {
    std::error_code ec;
    fs::create_directories(ALAMO_LUA_LOG_DIR, ec);
    std::ofstream(fs::path(ALAMO_LUA_LOG_DIR) / name, std::ios::binary) << text;
}

// ------------------------------------------------------------------------------------------

static void test_vm_basics() {
    LuaHost host;
    LuaVm vm(host);
    CHECK(run(vm, "x = 1 + 2 * 3; s = string.format('%d-%s', x, 'a'); t = {}; table.insert(t, 5); n = table.getn(t)") == "");
    CHECK(gnum(vm, "x") == 7.0 && gstr(vm, "s") == "7-a" && gnum(vm, "n") == 1.0);
    CHECK(run(vm, "v = _VERSION") == "" && gstr(vm, "v") == "Lua 5.0.2");
    // 5.0 semantics: no '%' operator, arg table for varargs, coroutine in base.
    CHECK(run(vm, "function f(...) return arg.n end; c = f(1, 2, 3); co = type(coroutine.create)") == "");
    CHECK(gnum(vm, "c") == 3.0 && gstr(vm, "co") == "function");
    std::string err;
    CHECK(!vm.compile("x = 5 % 2", "mod", &err) && !err.empty());
    // Libraries the original does not expose.
    CHECK(run(vm, "libs = tostring(io) .. tostring(os) .. tostring(math) .. tostring(debug) .. tostring(loadlib)") == "");
    CHECK(gstr(vm, "libs") == "nilnilnilnilnil");
    // ^ needs the math library's __pow in 5.0 (absent in the original).
    CHECK(run(vm, "y = 2 ^ 3") != "");
    // print goes to the engine log / messages, never stdout.
    CHECK(run(vm, "print('hello', 42, nil)") == "");
    CHECK(!vm.messages().empty() && vm.messages().back() == "[print] hello\t42\tnil");
    CHECK(run(vm, "f = loadstring('return 40 + 2'); z = f()") == "" && gnum(vm, "z") == 42.0);
    // Syntax errors carry the chunk name and line.
    CHECK(!vm.compile("x = = 1", "Data\\Scripts\\Bad.lua", &err) && err.find("Bad.lua:1") != std::string::npos);
    // Runtime errors are reported, not thrown.
    CHECK(run(vm, "error('boom')").find("boom") != std::string::npos);
    // Engine globals are userdata (like the original's LuaUserVar objects) and print by name.
    CHECK(run(vm, "ty = type(Find_Object_Type); nm = tostring(Find_Object_Type); mt = type(LuaWrapperMetaTable)") == "");
    CHECK(gstr(vm, "ty") == "userdata" && gstr(vm, "nm") == "Find_Object_Type" && gstr(vm, "mt") == "table");
    // Bytecode is accepted too (string.dump -> loadstring).
    CHECK(run(vm, "local b = string.dump(function() return 9 end); bc = string.byte(b, 1); g = loadstring(b)()") == "");
    CHECK(gnum(vm, "bc") == 27.0 && gnum(vm, "g") == 9.0);
    const std::vector<uint8_t> sig = {0x1B, 'L', 'u', 'a', 0x50};
    CHECK(is_bytecode(sig) && !is_bytecode(std::vector<uint8_t>{'-', '-'}));
}

static void test_registry() {
    const auto table = binding_table();
    size_t globals = 0, methods = 0;
    for (const BindingInfo& b : table) (b.kind == BindingKind::Global ? globals : methods)++;
    CHECK(globals == 143 && methods == 323);

    LuaHost host;
    LuaVm vm(host);
    lua_State* L = vm.state();
    size_t missing = 0;
    for (const BindingInfo& b : table) {
        if (b.kind == BindingKind::Global) {
            lua_pushstring(L, b.name);
            lua_rawget(L, LUA_GLOBALSINDEX);
            const int t = lua_type(L, -1);
            lua_pop(L, 1);
            const bool ok = is_host_value_global(b.name) ? t == LUA_TNIL : t == LUA_TUSERDATA;
            if (!ok) {
                std::printf("  global not registered: %s\n", b.name);
                ++missing;
            }
        } else if (vm.resolve_method(b.cls, b.name) < 0) {
            std::printf("  method not registered: %s.%s\n", b.cls, b.name);
            ++missing;
        }
    }
    CHECK(missing == 0);
    CHECK(vm.resolve_method("LandTaskForceClass", "Produce_Force") >= 0);  // inherited from TaskForceClass (bound by the AI family when present)
    CHECK(vm.resolve_method("GameObjectWrapper", "Is_Pool_Safe") == 1);    // LuaUserVar
    CHECK(vm.resolve_method("GameObjectWrapper", "Is_Valid") == 1);
    CHECK(vm.resolve_method("GameObjectWrapper", "No_Such_Method") == -1);

    // Unbound bindings raise a clear error and are counted. The L1 families implement bindings over
    // time, so the probes are whichever globals are still unbound when the test runs (none since L1 final:
    // the counters must then stay at zero, and the class-less GlobalValue object answers Get / Set).
    CHECK(run(vm, "GlobalValue.Set('probe', 1); gvp = GlobalValue.Get('probe')") == "");
    std::vector<std::string> probes;
    for (const BindingInfo& b : table)
        if (probes.size() < 2 && b.kind == BindingKind::Global && !is_host_value_global(b.name) &&
            !binding_implemented(b))
            probes.emplace_back(b.name);
    std::map<std::string, uint32_t> expected;
    for (size_t i = 0; i < probes.size(); ++i) {
        const std::string e = run(vm, probes[i] + "('X')");
        CHECK(e.find("unbound: " + probes[i]) != std::string::npos);
        expected[probes[i]] = 1;
    }
    if (!probes.empty()) {
        CHECK(run(vm, probes[0] + "('Y')") != "");
        expected[probes[0]] = 2;
    }
    if (vm.resolve_method("BudgetWrapper", "Flush_Category") == 0) {
        const std::string e = run(vm, "Budget.Flush_Category('x')");
        CHECK(e.find("unbound: BudgetWrapper.Flush_Category") != std::string::npos);
        expected["BudgetWrapper.Flush_Category"] = 1;
    }
    const auto hits = vm.unbound_hits();
    uint64_t expected_total = 0;
    for (const auto& [name, count] : expected) expected_total += count;
    CHECK(hits.size() == expected.size() && vm.unbound_total() == expected_total);
    for (const UnboundHit& h : hits) CHECK(expected.count(h.name) && expected.at(h.name) == h.count);
    // Missing methods are nil (scripts test `wrapper.Is_Valid == nil`).
    CHECK(run(vm, "m = tostring(GameRandom.No_Such_Method)") == "" && gstr(vm, "m") == "nil");
    // Flush_G (PGBase) calls Is_Pool_Safe on every userdata global.
    CHECK(run(vm, "ps = GameRandom.Is_Pool_Safe()") == "");
    lua_pushstring(L, "ps");
    lua_rawget(L, LUA_GLOBALSINDEX);
    CHECK(lua_toboolean(L, -1) == 1);
    lua_pop(L, 1);

    const BindingCoverage cov = binding_coverage();
    CHECK(cov.globals == 143 && cov.methods == 323 && cov.host_values == 8 && cov.globals_implemented >= 15 &&
          cov.methods_implemented >= 20 && cov.extra_methods >= 1);  // ThreadValue.Set + the AI family's documented extras
    const std::string report = binding_coverage_report();
    write_log("lua_binding_coverage.txt", report);
    std::printf("  bindings: globals %u (implemented %u, host values %u, unbound %u), methods %u (implemented %u, "
                "unbound %u), +%u extra\n",
                cov.globals, cov.globals_implemented, cov.host_values,
                cov.globals - cov.globals_implemented - cov.host_values, cov.methods, cov.methods_implemented,
                cov.methods - cov.methods_implemented, cov.extra_methods);
}

// ------------------------------------------------------------------------------------------

static uint64_t fnv_double(uint64_t h, double d) {
    uint64_t bits;
    std::memcpy(&bits, &d, sizeof bits);
    for (int i = 0; i < 8; ++i) {
        h ^= (bits >> (8 * i)) & 0xFFu;
        h *= 0x100000001B3ull;
    }
    return h;
}

static void test_random_and_math() {
    // GameRandom / Get_Float / Free_Random vs the engine RNG directly.
    GameRandom rng(12345u, 777u);
    GameRandom ref(12345u, 777u);
    LuaHost host;
    host.rng = &rng;
    LuaVm vm(host);
    CHECK(run(vm, "a = GameRandom(); b = GameRandom(10); c = GameRandom(5, 8); f = GameRandom.Get_Float(); "
                  "g = GameRandom.Get_Float(2, 4)") == "");
    CHECK(gnum(vm, "a") == ref.synced().range_int(0, 0xFFFF));
    CHECK(gnum(vm, "b") == ref.synced().range_int(0, 10));
    CHECK(gnum(vm, "c") == ref.synced().range_int(5, 8));
    CHECK(gnum(vm, "f") == static_cast<double>(ref.synced().range_float(0.0f, 1.0f)));
    CHECK(gnum(vm, "g") == static_cast<double>(ref.synced().range_float(2.0f, 4.0f)));
    CHECK(rng == ref);
    // Free_Random quirk: no-argument form draws from the SYNCHRONIZED stream...
    CHECK(run(vm, "q = GameRandom.Free_Random()") == "");
    CHECK(gnum(vm, "q") == ref.synced().range_int(0, 0xFFFF) && rng.synced() == ref.synced());
    // ... ranged forms from the free stream in single player (synced untouched) ...
    CHECK(run(vm, "r = GameRandom.Free_Random(1, 6)") == "");
    CHECK(gnum(vm, "r") == ref.free().range_int(1, 6) && rng.synced() == ref.synced() && rng.free() == ref.free());
    CHECK(run(vm, "GameRandom.Free_Random(1)").find("Invalid parameter 2") != std::string::npos);
    CHECK(run(vm, "GameRandom('x')").find("Invalid parameter 1") != std::string::npos);
    // ... and from the synchronized stream in multiplayer.
    vm.host().multiplayer = true;
    CHECK(run(vm, "m = GameRandom.Free_Random(1, 6)") == "");
    CHECK(gnum(vm, "m") == ref.synced().range_int(1, 6) && rng == ref);

    // Opt-in math table: dmath-backed, bit-identical across compilers; random on the synced stream.
    LuaVmOptions opt;
    opt.math_library = true;
    LuaVm mv(host, opt);
    CHECK(run(mv, "s = math.sin(0.5); p = math.pow(2.5, 1.7); e = 2 ^ 10; l = math.log(10); fl = math.floor(-2.5); "
                  "at = math.atan2(1, -2); rr = math.random(1, 100)") == "");
    CHECK(gnum(mv, "s") == dmath::sin(0.5) && gnum(mv, "p") == dmath::pow(2.5, 1.7) && gnum(mv, "e") == 1024.0 &&
          gnum(mv, "l") == dmath::log(10.0) && gnum(mv, "fl") == -3.0 && gnum(mv, "at") == dmath::atan2(1.0, -2.0));
    const double r15 = static_cast<double>(ref.synced().next15()) / 32768.0;
    CHECK(gnum(mv, "rr") == static_cast<double>(static_cast<int>(r15 * 100.0) + 1) && rng == ref);

    // Bit-identical numeric digest: the same on MSVC, GCC and Clang (number parsing, formatting,
    // arithmetic, dmath, string.format). Golden from the MSVC build.
    GameRandom r2(42u, 7u);
    LuaHost h2;
    h2.rng = &r2;
    LuaVm dv(h2, opt);
    CHECK(run(dv, "out = {}\n"
                  "for i = 1, 200 do\n"
                  "  local x = i * 0.37 - 11.5\n"
                  "  table.insert(out, math.sin(x) * math.cos(x / 3) + math.exp(x / 50) - math.sqrt(i) / 7)\n"
                  "  table.insert(out, math.pow(1.0001, i * 13) + math.atan2(x, i) + math.log10(i))\n"
                  "  table.insert(out, tonumber(string.format('%.6f', x / 7)) + tonumber('1e-3') * i)\n"
                  "  table.insert(out, GameRandom(1, 1000) + GameRandom.Get_Float() + math.random())\n"
                  "end") == "");
    lua_State* L = dv.state();
    lua_pushstring(L, "out");
    lua_rawget(L, LUA_GLOBALSINDEX);
    uint64_t digest = 0xCBF29CE484222325ull;
    for (int i = 1; i <= 800; ++i) {
        lua_rawgeti(L, -1, i);
        digest = fnv_double(digest, lua_tonumber(L, -1));
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    std::printf("  numeric digest %016llx\n", static_cast<unsigned long long>(digest));
    CHECK(digest == 0x5C4BB890FC69E99Eull);  // golden: MSVC 14.51 build; GCC / Clang must match

    // pairs() order over table / userdata keys is reproducible (keys hash by creation serial, not
    // address): two VMs, the second created after unrelated allocations, give the same order.
    const char* order_script =
        "keys = {} for i = 1, 40 do keys[i] = {i} end\n"
        "t = {} for i = 1, 40 do t[keys[i]] = i end\n"
        "t[GameRandom] = 100 t[Find_Object_Type] = 101 t[coroutine.create(function() end)] = 102\n"
        "order = '' for k, v in pairs(t) do order = order .. v .. ',' end";
    LuaVm o1(host);
    CHECK(run(o1, order_script) == "");
    std::vector<std::unique_ptr<std::string>> noise;
    for (int i = 0; i < 1000; ++i) noise.push_back(std::make_unique<std::string>(static_cast<size_t>(i % 97 + 1), 'x'));
    LuaVm o2(host);
    CHECK(run(o2, "local junk = {} for i = 1, 10 do junk[i] = {} end junk = nil collectgarbage()") == "");
    LuaVm o3(host);
    CHECK(run(o3, order_script) == "");
    CHECK(gstr(o1, "order") == gstr(o3, "order") && gstr(o1, "order").size() > 40);
}

// OI lua (L1 determinism): tostring() names, gcinfo and weak tables depend only on the VM's own history.
static void test_gc_determinism() {
    const char* script =
        "names = tostring({}) .. ' ' .. tostring(print) .. ' ' .. tostring(function() end) .. ' ' .. "
        "tostring(coroutine.create(function() end)) .. ' ' .. tostring(newproxy())\n"
        // Weak tables: keys / values cleared at a collection; survivors listed in pairs order.
        "wk = setmetatable({}, {__mode = 'k'}) wv = setmetatable({}, {__mode = 'v'}) keep = {}\n"
        "local c = 0 for i = 1, 60 do local k = {i} wk[k] = i wv[i] = {i} c = c + 1 if c == 3 then c = 0 keep[i] = k end end\n"
        "collectgarbage()\n"
        "surv = '' for k, v in pairs(wk) do surv = surv .. v .. ',' end\n"
        "nv = 0 for k, v in pairs(wv) do nv = nv + 1 end\n"
        // Automatic collections inside a loop (threshold from collectgarbage(n), like PGBaseDefinitions).
        "collectgarbage(64) ww = setmetatable({}, {__mode = 'v'}) seen = '' local r, s = 0, 0\n"
        "for i = 1, 4000 do r = r + 1 if r == 50 then r = 0 end ww[i] = {string.rep('x', r)} s = s + 1 "
        "if s == 500 then s = 0 local n = 0 for _ in pairs(ww) do n = n + 1 end seen = seen .. n .. ',' end end\n"
        "local a, b = gcinfo() gc = a .. '/' .. b\n";
    LuaHost host;
    LuaVm v1(host);
    CHECK(run(v1, script) == "");
    std::vector<std::unique_ptr<std::string>> noise;
    for (int i = 0; i < 2000; ++i) noise.push_back(std::make_unique<std::string>(static_cast<size_t>(i % 89 + 1), 'y'));
    LuaVm v2(host);
    CHECK(run(v2, script) == "");
    const std::string n1 = gstr(v1, "names");
    std::printf("  gc determinism: names '%s' weak-k survivors %zu chars, weak-v %g, auto-gc counts %s, gcinfo %s\n",
                n1.c_str(), gstr(v1, "surv").size(), gnum(v1, "nv"), gstr(v1, "seen").c_str(), gstr(v1, "gc").c_str());
    CHECK(n1.find("table: ") == 0 && n1.find("0x") == std::string::npos);
    CHECK(n1 == gstr(v2, "names"));
    CHECK(gstr(v1, "surv") == gstr(v2, "surv") && gnum(v1, "nv") == gnum(v2, "nv"));
    CHECK(gstr(v1, "seen") == gstr(v2, "seen") && gstr(v1, "gc") == gstr(v2, "gc"));
    // Strongly held weak keys survive; unreferenced values are gone after a full collection.
    uint32_t survivors = 0;
    for (char ch : gstr(v1, "surv")) survivors += ch == ',' ? 1u : 0u;
    CHECK(survivors == 20 && gnum(v1, "nv") == 0.0);
    // Golden (MSVC 14.51 build; clang must match) on a bare patched VM (base library only, so engine binding
    // changes do not move it): tostring names, pairs order over the surviving table keys, the automatic
    // collection points and gcinfo are fixed by creation serials and Lua's own byte accounting, not addresses.
    lua_State* B = lua_open();
    luaopen_base(B);
    luaopen_string(B);
    lua_settop(B, 0);
    const bool bare_ok = luaL_loadbuffer(B, script, std::strlen(script), "gc") == 0 && lua_pcall(B, 0, 0, 0) == 0;
    CHECK(bare_ok);
    std::string bare;
    for (const char* g : {"names", "surv", "seen", "gc"}) {
        lua_pushstring(B, g);
        lua_rawget(B, LUA_GLOBALSINDEX);
        const char* s = lua_tostring(B, -1);
        bare += std::string(s ? s : "<nil>") + "|";
        lua_pop(B, 1);
    }
    lua_close(B);
    uint64_t h = 0xCBF29CE484222325ull;
    for (char ch : bare) {
        h ^= static_cast<uint8_t>(ch);
        h *= 0x100000001B3ull;
    }
    std::printf("  gc determinism (bare VM): %s digest %016llx\n", bare.c_str(), static_cast<unsigned long long>(h));
    CHECK(h == 0xA129F10BF6E9E006ull);  // golden: MSVC 14.51 build = clang 20 build
}

// ------------------------------------------------------------------------------------------

static void test_threads() {
    LuaHost host;
    LuaVm vm(host);
    // A PGBase-style yield helper and Sleep (same structure as PGBase.lua's PumpEvents / Sleep).
    CHECK(run(vm,
              "trace = ''\n"
              "function Pump() coroutine.yield(true) local ev = GetEvent() while ev do ev() ev = GetEvent() end end\n"
              "function MySleep(t) ThreadValue.Set('Start', GetCurrentTime())\n"
              "  while GetCurrentTime() - ThreadValue('Start') < t do Pump() end end\n"
              "function Worker(tag, n) for i = 1, n do trace = trace .. tag .. i .. '(' .. GetThreadID() .. ') ' Pump() end end\n"
              "function Sleeper() MySleep(1.0) trace = trace .. 'woke@' .. GetCurrentTime() .. ' ' end\n"
              "function Spawner() trace = trace .. 'spawn ' Create_Thread('Worker', 'C', 1) end\n"
              "function Quitter() coroutine.yield(false) trace = trace .. 'NEVER ' end\n"
              "function Bad() Pump() error('bad thread') end\n"
              "function OnEvent() trace = trace .. 'event ' end\n"
              "a = Create_Thread('Worker', 'A', 3)\n"
              "b = Create_Thread('Worker', 'B', 2)\n"
              "main_id = GetThreadID()\n") == "");
    CHECK(gnum(vm, "a") == 0.0 && gnum(vm, "b") == 1.0 && gnum(vm, "main_id") == -1.0);
    CHECK(vm.thread_count() == 2 && vm.thread_active(0) && vm.thread_active(1));
    CHECK(run(vm, "Create_Thread('NoSuchFunction')").find("LuaCreateThread") != std::string::npos);
    vm.service_threads();
    CHECK(gstr(vm, "trace") == "A1(0) B1(1) ");
    vm.service_threads();
    vm.service_threads();
    CHECK(gstr(vm, "trace") == "A1(0) B1(1) A2(0) B2(1) A3(0) ");
    CHECK(vm.thread_count() == 1);  // B returned
    vm.service_threads();
    CHECK(vm.thread_count() == 0);

    // Sleep on game time; events delivered by PumpEvents-style loops.
    CHECK(run(vm, "trace = '' s = Create_Thread('Sleeper')") == "");
    for (int frame = 0; frame <= 40; ++frame) {
        vm.set_time(frame / 30.0);
        if (frame == 10) vm.post_event("OnEvent");
        vm.service_threads();
    }
    CHECK(gstr(vm, "trace") == "event woke@1 ");
    CHECK(vm.thread_count() == 0);

    // Threads created while servicing run on the next service; Kill; yield(false) ends a thread;
    // errors end the thread and are reported.
    CHECK(run(vm, "trace = '' Create_Thread('Spawner') q = Create_Thread('Quitter') k = Create_Thread('Worker', 'K', 5) "
                  "x = Create_Thread('Bad')") == "");
    vm.service_threads();
    CHECK(gstr(vm, "trace") == "spawn K1(5) ");
    CHECK(!vm.thread_active(static_cast<int32_t>(gnum(vm, "q"))));
    CHECK(run(vm, "Thread.Kill(k) act = Thread.Is_Thread_Active(k) nm = Thread.Get_Name(x)") == "");
    CHECK(gstr(vm, "nm") == "Bad");
    vm.service_threads();
    CHECK(gstr(vm, "trace") == "spawn K1(5) C1(7) ");
    CHECK(vm.errors().size() == 1 && vm.errors()[0].find("bad thread") != std::string::npos);
    vm.service_threads();
    CHECK(vm.thread_count() == 0);

    // ThreadValue is per thread.
    CHECK(run(vm, "ThreadValue.Set('k', 'main')\n"
                  "function TV() ThreadValue.Set('k', 'thread') Pump() tv_seen = ThreadValue('k') end\n"
                  "Create_Thread('TV')") == "");
    vm.service_threads();
    vm.service_threads();
    CHECK(gstr(vm, "tv_seen") == "thread" && run(vm, "mv = ThreadValue('k')") == "" && gstr(vm, "mv") == "main");

    // _ScriptExit ends every thread at the next yield.
    CHECK(run(vm, "function Exiter() _ScriptExit() Pump() trace = trace .. 'NEVER' end trace = '' "
                  "Create_Thread('Exiter') Create_Thread('Worker', 'Z', 9)") == "");
    vm.service_threads();
    CHECK(vm.exit_requested() && vm.thread_count() == 0 && gstr(vm, "trace") == "");

    // Logging bindings.
    CHECK(run(vm, "_ScriptMessage('hello') _OuputDebug('dbg') s = DumpCallStack()") == "");
    const auto& msgs = vm.messages();
    CHECK(msgs.size() >= 2 && msgs[msgs.size() - 2] == "[ScriptMessage] hello" && msgs.back() == "[OutputDebug] dbg");
    CHECK(gstr(vm, "s").find("Lua call stack") == 0);
}

// ------------------------------------------------------------------------------------------

static void test_objects() {
    const fs::path root = fs::temp_directory_path() / "alamo_lua_objects_test";
    std::error_code ec;
    remove_all_long(root, ec);
    write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>S.xml</File></L>");
    write_file(root / "Data/XML/S.xml",
               "<R><GroundInfantry Name=\"Trooper\"><Tactical_Health>40</Tactical_Health>"
               "<Behavior>SELECTABLE</Behavior></GroundInfantry>\n"
               "<Prop Name=\"Rock\"><Behavior>DUMMY_TOOLTIP</Behavior></Prop></R>");
    DirectoryProvider dp(root.string());
    objects::ObjectTypeDatabase raw;
    CHECK(raw.load(dp));
    objects::GameObjectTypes types;
    CHECK(types.build(raw));
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Land, objects::StoreKind::Primary, types, &ids);
    objects::SpawnParams sp;
    sp.type = raw.find("Trooper");
    sp.owner = 2;
    const objects::ObjectHandle t1 = store.create(sp);
    const objects::ObjectHandle t2 = store.create(sp);
    sp.type = raw.find("Rock");
    sp.owner = 0;
    store.create(sp);
    sp.type = raw.find("Trooper");
    sp.owner = 3;
    const objects::ObjectHandle t3 = store.create(sp);

    LuaHost host;
    host.store = &store;
    host.types = &types;
    LuaVm vm(host);
    CHECK(run(vm, "tt = Find_Object_Type('trooper'); nm = tt.Get_Name(); none = tostring(Find_Object_Type('Nope'))\n"
                  "all = Find_All_Objects_Of_Type(tt); n = table.getn(all)\n"
                  "ids = '' for i, o in ipairs(all) do ids = ids .. o.Get_Object_ID() .. ',' end\n"
                  "byname = table.getn(Find_All_Objects_Of_Type('Rock'))\n"
                  "same = (all[1] == Find_All_Objects_Of_Type(tt)[1]) and rawequal(all[1], Find_All_Objects_Of_Type(tt)[1])\n"
                  "tn = all[2].Get_Type().Get_Name() owner = all[1].Get_Owner().Get_ID() mode = Get_Game_Mode()\n"
                  "victim = all[2]") == "");
    CHECK(gstr(vm, "nm") == "Trooper" && gstr(vm, "none") == "nil");
    CHECK(gnum(vm, "n") == 3.0 && gstr(vm, "ids") == "4,2,1,");  // newest first
    CHECK(gnum(vm, "byname") == 1.0 && gstr(vm, "tn") == "Trooper" && gnum(vm, "owner") == 3.0 && gstr(vm, "mode") == "Land");
    CHECK(run(vm, "ok = same and 1 or 0") == "" && gnum(vm, "ok") == 1.0);
    // Wrapper identity: tables keyed by wrappers hit (raw identity), for objects, types and players.
    CHECK(run(vm, "keyed = {} keyed[all[1]] = 'o' keyed[tt] = 't' keyed[all[1].Get_Owner()] = 'p'\n"
                  "again = Find_All_Objects_Of_Type('Trooper')\n"
                  "k1 = keyed[again[1]] k2 = keyed[Find_Object_Type('TROOPER')] k3 = keyed[again[1].Get_Owner()]\n"
                  "k4 = tostring(keyed[again[2].Get_Owner()])\n"
                  "pid = rawequal(all[1].Get_Owner(), again[1].Get_Owner()) and 1 or 0\n"
                  "peq = (all[1].Get_Owner() == again[1].Get_Owner()) and 1 or 0") == "");
    CHECK(gstr(vm, "k1") == "o" && gstr(vm, "k2") == "t" && gstr(vm, "k3") == "p" && gstr(vm, "k4") == "nil");
    CHECK(gnum(vm, "pid") == 1.0 && gnum(vm, "peq") == 1.0);
    // refixf t03: the original's script error helper logs and returns nothing (no unwind).
    CHECK(run(vm, "bad = Find_All_Objects_Of_Type(5) bad = tostring(bad)") == "" && gstr(vm, "bad") == "nil" &&
          vm.messages().back().find("unknown filter parameter 1") != std::string::npos);
    CHECK(run(vm, "Find_All_Objects_Of_Type()") == "" &&
          vm.messages().back().find("Expected a filter parameter") != std::string::npos);

    // Dead object: Is_Valid false immediately (before the flush), methods raise "this object is dead!".
    CHECK(store.destroy(t2));
    CHECK(run(vm, "v1 = victim.Is_Valid() and 1 or 0") == "" && gnum(vm, "v1") == 0.0);
    std::string e = run(vm, "victim.Get_Type()");
    CHECK(e.find("GameObjectWrapper::Get_Type -- this object is dead!") != std::string::npos);
    CHECK(run(vm, "victim.Get_Position()").find("this object is dead!") != std::string::npos);  // unbound, but dead first
    store.flush();
    CHECK(run(vm, "v2 = victim.Is_Valid() and 1 or 0 n2 = table.getn(Find_All_Objects_Of_Type(tt)) s = tostring(victim)") == "");
    CHECK(gnum(vm, "v2") == 0.0 && gnum(vm, "n2") == 2.0 && gstr(vm, "s").find("[dead]") != std::string::npos);
    // A new object reusing the slot is not the old wrapper (id check).
    const objects::ObjectHandle t4 = store.create(sp);
    CHECK(t4.slot() == t2.slot());
    CHECK(run(vm, "v3 = victim.Is_Valid() and 1 or 0") == "" && gnum(vm, "v3") == 0.0);
    vm.sweep_dead_wrappers();
    CHECK(run(vm, "c = table.getn(Find_All_Objects_Of_Type(tt))") == "" && gnum(vm, "c") == 3.0);
    // A table keyed by the dead wrapper keeps it (still a dead wrapper after the sweep); the live
    // object in the reused slot gets its own wrapper.
    CHECK(run(vm, "dk = {} dk[victim] = 1 dead_keys = 0 for k in pairs(dk) do if not k.Is_Valid() then "
                  "dead_keys = dead_keys + 1 end end newest = Find_All_Objects_Of_Type(tt)[1] "
                  "fresh = (dk[newest] == nil) and 1 or 0") == "");
    CHECK(gnum(vm, "dead_keys") == 1.0 && gnum(vm, "fresh") == 1.0);
    // Host value globals.
    vm.set_object_global("Object", t1);
    CHECK(run(vm, "oid = Object.Get_Object_ID() ot = type(Object)") == "" && gnum(vm, "oid") == 1.0 &&
          gstr(vm, "ot") == "userdata");
    vm.set_object_global("Object", {});
    CHECK(run(vm, "on = tostring(Object)") == "" && gstr(vm, "on") == "nil");
    (void)t3;
    remove_all_long(root, ec);
}

static void test_owner_thread() {
    LuaHost host;
    LuaVm vm(host);
    std::atomic<bool> foreign_owner{true};
    std::thread th([&] { foreign_owner = vm.on_owner_thread(); });
    th.join();
    CHECK(!foreign_owner && vm.on_owner_thread());
    // Hand-off between frames.
    std::atomic<bool> ran{false};
    std::thread th2([&] {
        vm.claim_ownership();
        ran = vm.on_owner_thread() && run(vm, "h = 1") == "";
    });
    th2.join();
    CHECK(ran && !vm.on_owner_thread());
    vm.claim_ownership();
    CHECK(gnum(vm, "h") == 1.0);
}

static void test_static_scan() {
    ScriptScan scan;
    scan_script("-- Find_Player('x')\n--[[ Story_Event() ]]\nrequire(\"PGBase\")\n"
                "function Definitions() local p = Find_Player(\"Empire\") p.Get_Name() end\n"
                "local function helper() end\nGlobalThing = 5\ns = string.format('%d', 1) x = \"GameRandom(1)\"\n"
                "u = Find_All_Objects_Of_Type(Find_Object_Type('X')) Create_Thread 'Foo' t = {Spawn_Unit {}}",
                scan);
    CHECK(scan.global_calls.count("Story_Event") == 0 && scan.global_calls.at("Find_Player") == 1);
    CHECK(scan.global_calls.at("require") == 1 && scan.global_calls.count("GameRandom") == 0);
    CHECK(scan.global_calls.at("Create_Thread") == 1 && scan.global_calls.at("Spawn_Unit") == 1);
    CHECK(scan.method_calls.at("Get_Name") == 1 && scan.library_calls.at("string.format") == 1);
    CHECK(scan.defined.count("Definitions") && scan.defined.count("GlobalThing") && !scan.defined.count("helper"));
    ScanSummary sum;
    const std::string rep = scan_report(scan, &sum);
    // Five engine globals are called; how many are implemented depends on which L1 families have landed.
    uint32_t impl = 0;
    for (const BindingInfo& b : binding_table())
        for (const char* n : {"Find_All_Objects_Of_Type", "Find_Object_Type", "Create_Thread", "Find_Player", "Spawn_Unit"})
            if (b.kind == BindingKind::Global && std::strcmp(b.name, n) == 0 && binding_implemented(b)) ++impl;
    CHECK(impl >= 3 && sum.engine_globals_implemented == impl && sum.engine_globals_unbound == 5 - impl);
    CHECK(rep.find("Find_Player") != std::string::npos);
}

// ------------------------------------------------------------------------------------------
// Shipped scripts (ALAMO_FOC_DATA / ALAMO_EAW_DATA).

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
    return v ? std::string(v) : std::string();
#endif
}

static bool has_script_ext(const std::string& p) {
    auto lower = p;
    for (char& c : lower) c = static_cast<char>((c >= 'A' && c <= 'Z') ? c - 'A' + 'a' : c);
    return (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".lua") == 0) ||
           (lower.size() > 3 && lower.compare(lower.size() - 3, 3, ".lc") == 0);
}

static void list_loose(const fs::path& dir, const std::string& rel, std::vector<std::string>& out) {
    std::error_code ec;
    for (const fs::path& p : list_directory(dir, ec)) {
        const std::string name = path_to_utf8(p.filename());
        std::error_code ec2;
        if (fs::is_directory(long_path(p), ec2))
            list_loose(p, rel + name + "\\", out);
        else if (has_script_ext(name))
            out.push_back(rel + name);
    }
}

static void test_shipped_scripts(const char* var, const char* game) {
    const std::string root = env(var);
    if (root.empty()) {
        std::printf("  %s unset: shipped %s scripts skipped\n", var, game);
        return;
    }
    std::unique_ptr<FileProvider> files;
    std::vector<std::string> paths;
#ifdef ALAMO_LUA_HAVE_VFS
    vfs::VfsConfig cfg;
    cfg.foc_root = root;
    cfg.eaw_root = "";
    std::string verr;
    if (auto v = vfs::Vfs::create(cfg, &verr)) {
        for (const std::string& p : v->list("Data\\Scripts\\**"))
            if (has_script_ext(p)) paths.push_back(p);
        files = std::move(v);
    }
#endif
    if (!files) {
        files = std::make_unique<DirectoryProvider>(root);
        fs::path scripts;
        std::error_code ec;
        for (const fs::path& p : list_directory(path_from_utf8(root), ec))
            for (const fs::path& q : list_directory(p, ec))
                if (alamo::normalize_path(path_to_utf8(q.filename())) == "SCRIPTS") scripts = q;
        if (!scripts.empty()) list_loose(scripts, "Data\\Scripts\\", paths);
        std::sort(paths.begin(), paths.end());
    }
    if (paths.empty()) {
        std::printf("  %s: no scripts under Data\\Scripts\n", game);
        return;
    }
    ScriptScan scan;
    uint32_t text = 0, binary = 0, compile_errors = 0, run_errors = 0, run_unbound = 0;
    std::string details;
    std::map<std::string, uint32_t> unbound_at_top;
    for (const std::string& path : paths) {
        std::vector<uint8_t> bytes;
        if (!files->read(path, bytes)) {
            details += "READ FAIL " + path + "\n";
            ++compile_errors;
            continue;
        }
        const std::string_view src(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (is_bytecode(bytes)) ++binary;
        else {
            ++text;
            scan_script(src, scan);
        }
        LuaHost host;
        host.files = files.get();
        LuaVmOptions opt;
        opt.script_name = path;
        LuaVm vm(host, opt);
        std::string err;
        if (!vm.compile(src, path, &err)) {
            ++compile_errors;
            details += "COMPILE " + err + "\n";
            continue;
        }
        // Top level only (function definitions, requires, constants): no campaign logic runs.
        if (!vm.run(src, path, &err)) {
            ++run_errors;
            details += "TOPLEVEL " + err + "\n";
        }
        for (const UnboundHit& h : vm.unbound_hits()) {
            unbound_at_top[h.name] += h.count;
            run_unbound += h.count;
        }
    }
    ScanSummary sum;
    std::string report = std::string(game) + " scripts: " + std::to_string(paths.size()) + " (" + std::to_string(text) +
                         " source, " + std::to_string(binary) + " bytecode)\ncompile errors: " +
                         std::to_string(compile_errors) + "\ntop-level run errors: " + std::to_string(run_errors) +
                         ", unbound calls at top level: " + std::to_string(run_unbound) + "\n";
    for (const auto& [name, n] : unbound_at_top) report += "  top-level unbound " + name + " x" + std::to_string(n) + "\n";
    report += details + "\n== static scan\n" + scan_report(scan, &sum);
    write_log(std::string("lua_scripts_") + game + ".txt", report);
    std::printf("  %s: %zu scripts (%u source, %u bytecode), compile errors %u, top-level errors %u, unbound "
                "referenced: %u globals / %u methods (%llu call sites), implemented referenced: %u / %u (%llu)\n",
                game, paths.size(), text, binary, compile_errors, run_errors, sum.engine_globals_unbound,
                sum.engine_methods_unbound, static_cast<unsigned long long>(sum.unbound_call_sites),
                sum.engine_globals_implemented, sum.engine_methods_implemented,
                static_cast<unsigned long long>(sum.implemented_call_sites));
    CHECK(compile_errors == 0);

    // The real PGBase Sleep / PumpEvents on top of the bindings.
    LuaHost host;
    host.files = files.get();
    LuaVm vm(host);
    std::string err;
    if (vm.require("PGBase", &err)) {
        CHECK(run(vm, "done = 0 function T() Sleep(0.5) done = GetCurrentTime() end Create_Thread('T')") == "");
        for (int f = 0; f <= 30; ++f) {
            vm.set_time(f / 30.0);
            vm.service_threads();
        }
        CHECK(gnum(vm, "done") == 0.5 && vm.thread_count() == 0 && vm.errors().empty());
        if (!vm.errors().empty()) std::printf("  PGBase Sleep: %s\n", vm.errors()[0].c_str());
    } else {
        std::printf("  %s: PGBase not loadable: %s\n", game, err.c_str());
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Warn);
    test_vm_basics();
    test_registry();
    test_random_and_math();
    test_gc_determinism();
    test_threads();
    test_objects();
    test_owner_thread();
    test_static_scan();
    test_shipped_scripts("ALAMO_FOC_DATA", "foc");
    test_shipped_scripts("ALAMO_EAW_DATA", "eaw");
    if (g_fail) {
        std::printf("alamo_lua_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_lua_tests: all passed\n");
    return 0;
}

// q6 t19: the per-owner script block (alamo/lua/script_save.hpp, docs/re/LUA_SAVE.md §2 / §4).
//   * a plot-style script saved mid-run - `main` waiting in Sleep, a second thread waiting for an event that is still in
//     the GetEvent queue at save time, a table graph with shared references and cycles, a closure with an upvalue,
//     tables keyed by object / player wrappers - is restored into a FROM-SAVE instance (constructed, bridges attached,
//     nothing run) and then runs in lockstep with the original: same messages, same heap accounting, same RNG, same
//     bytes at the end; shared references, cycles and wrapper identity survive;
//   * the header names the script, its options and the thread records (no second `main`);
//   * an object-script style instance (Base_Definitions, `main`, `Object`) continues without re-running its OnEnter;
//   * refusals leave the target unchanged: another script name, other options, a target that is not fresh
//     (ScriptLoad::FromSave), damaged / truncated blocks, a VM that could not be saved.
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/players.hpp"
#include "alamo/lua/script_save.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"

#include <lauxlib.h>
#include <lua.h>

using namespace alamo;
using namespace alamo::lua;
namespace fs = std::filesystem;

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
static std::string gstr(LuaVm& vm, const char* expr) {
    lua_State* L = vm.state();
    const std::string chunk = std::string("return tostring(") + expr + ")";
    if (luaL_loadbuffer(L, chunk.data(), chunk.size(), "=gstr") != 0 || lua_pcall(L, 0, 1, 0) != 0) {
        lua_settop(L, 0);
        return "<error>";
    }
    std::string s = lua_tostring(L, -1) ? lua_tostring(L, -1) : "";
    lua_settop(L, 0);
    return s;
}
static std::vector<uint8_t> block(const LuaVm& vm) {
    ByteWriter w;
    write_script_block(w, vm);
    return w.bytes();
}
static std::vector<uint8_t> image(const LuaVm& vm) {
    ByteWriter w;
    vm.write(w);
    return w.bytes();
}
static bool restore(LuaVm& vm, const std::vector<uint8_t>& b, ScriptLoad mode, std::string* err = nullptr) {
    ByteReader r(b);
    return read_script_block(r, vm, mode, err) && r.done();
}
static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

// A store with three troopers (ids 1..3). Own temp folder (parallel ctest: no shared fixture with test_lua_save).
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
        root = fs::temp_directory_path() / "alamo_lua_script_save_test";
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

static LuaVmOptions plot_options() {
    LuaVmOptions o;
    o.script_name = "Story_Block_Test";
    return o;
}

// PGBase-style Pump / Sleep written inline (the in-memory test has no Data\Scripts).
static const char* const kPlot = R"(
function Pump() coroutine.yield(true) local ev = GetEvent() while ev do ev() ev = GetEvent() end end
function Sleep(t) local start = GetCurrentTime() while GetCurrentTime() - start < t do Pump() end end
graph = {name = 'root'}
graph.self = graph
shared = {count = 0}
graph.left = {parent = graph, data = shared}
graph.right = {parent = graph, data = shared}
do local a, b, c = {n = 1}, {n = 2}, {n = 3} a.next = b b.next = c c.next = a ring = {head = a} end
by_obj = {}
by_obj[Obj1] = 'one'
by_obj[Obj2] = 'two'
P = Find_Player and Find_Player('Empire') or nil
by_player = {}
if P then by_player[P] = 'emp' end
function make_acc() local total = 0 return function(d) total = total + d return total end end
acc = make_acc()
function main()
  local step = 0
  local mine = graph.left
  local cyc = ring.head
  while true do
    step = step + 1
    shared.count = shared.count + 1
    cyc = cyc.next
    print('main ' .. step .. ' t=' .. GetCurrentTime() .. ' shared=' .. shared.count ..
          ' same=' .. tostring(rawequal(mine.data, graph.right.data) and rawequal(mine.parent, graph)) ..
          ' cyc=' .. tostring(rawequal(graph.self.self, graph)) .. ' ring=' .. cyc.n .. ' acc=' .. acc(step) ..
          ' r=' .. GameRandom(1, 100) .. ' new=' .. tostring({}))
    Sleep(0.5)
  end
end
function On_Signal() signals = (signals or 0) + 1 end
function Event_Watcher()
  local seen = 0
  while true do
    while (signals or 0) == seen do Pump() end
    seen = signals
    local s = ''
    for o, v in pairs(by_obj) do s = s .. o.Get_Object_ID() .. '=' .. v .. ' ' end
    local p = ''
    for k, v in pairs(by_player) do p = p .. v .. ':' .. tostring(rawequal(k, Find_Player('Empire'))) end
    print('signal ' .. seen .. ' at ' .. GetCurrentTime() .. ' objs ' .. s .. 'players ' .. p ..
          ' obj_same=' .. tostring(rawequal(next(by_obj) ~= nil and Obj1 or nil, Obj1)))
  end
end
)";

static void step(LuaVm& vm, uint32_t f) {
    vm.set_time(f / 30.0);
    vm.set_frame(f);
    if (f % 11 == 4) vm.post_event("On_Signal");
    vm.service_threads();
}

static void test_plot(const World& w) {
    GameRandom rng_a(77, 5);
#if defined(ALAMO_LUA_PLAYERS)
    LuaPlayerWorld players;
    std::vector<LuaPlayerSetup> setup(2);
    setup[0].faction = "Empire";
    setup[0].human = true;
    setup[1].faction = "Rebel";
    setup[1].ai = true;
    players.set_players(setup);
#endif
    ObjectBridge bridge_a, bridge_b;
    LuaVm a(w.host(&rng_a), plot_options());
    attach_object_bridge(a, &bridge_a);
#if defined(ALAMO_LUA_PLAYERS)
    attach_player_world(a, &players);
#endif
    CHECK(a.fresh());
    a.set_object_global("Obj1", w.h[0]);
    a.set_object_global("Obj2", w.h[1]);
    CHECK(a.fresh());  // host values are not code
    CHECK(run(a, kPlot) == "");
    CHECK(!a.fresh());
    CHECK(a.create_thread("main") == 0 && a.create_thread("Event_Watcher") == 1);
    for (uint32_t f = 0; f < 40; ++f) step(a, f);
    a.post_event("On_Signal");  // still queued at save time
    for (const std::string& e : a.errors()) std::printf("  A error: %s\n", e.c_str());
    CHECK(a.errors().empty());
    const std::string acc_total = gstr(a, "acc(0)");  // before the save (running code changes the heap)

    const std::vector<uint8_t> saved = block(a);
    CHECK(a.save_error().empty());
    CHECK(block(a) == saved);  // writing changes nothing
    ScriptBlockHeader hd;
    std::string err;
    CHECK(peek_script_block(saved, hd, &err));
    CHECK(hd.version == kScriptBlockVersion && hd.name == "Story_Block_Test" && hd.saved && !hd.math_library);
    CHECK(hd.threads.size() == 2 && hd.next_thread == 2);
    if (hd.threads.size() == 2) {
        CHECK(hd.threads[0].id == 0 && hd.threads[0].function == "main" && hd.threads[0].started && !hd.threads[0].killed);
        CHECK(hd.threads[1].id == 1 && hd.threads[1].function == "Event_Watcher");
    }
    std::printf("  plot block: %zu bytes, %zu threads\n", saved.size(), hd.threads.size());

    // The from-save instance: constructed, bridges attached, nothing run.
    GameRandom rng_b = rng_a;
    LuaVm b(w.host(&rng_b), plot_options());
    attach_object_bridge(b, &bridge_b);
#if defined(ALAMO_LUA_PLAYERS)
    attach_player_world(b, &players);
#endif
    CHECK(restore(b, saved, ScriptLoad::FromSave, &err));
    if (!err.empty()) std::printf("  restore: %s\n", err.c_str());
    CHECK(!b.fresh());
    CHECK(attached_object_bridge(b) == &bridge_b);
    CHECK(b.thread_list() == a.thread_list() && b.next_thread_id() == 2);
    CHECK(block(b) == saved);
    CHECK(image(b) == image(a));
    {  // Identity inside the restored graph (a third copy: running code here would perturb B's heap).
        GameRandom rng_c = rng_a;
        LuaVm c(w.host(&rng_c), plot_options());
#if defined(ALAMO_LUA_PLAYERS)
        attach_player_world(c, &players);
#endif
        CHECK(restore(c, saved, ScriptLoad::FromSave));
        CHECK(gstr(c, "rawequal(graph.left.data, graph.right.data)") == "true");
        CHECK(gstr(c, "rawequal(graph.self, graph) and rawequal(graph.left.parent, graph)") == "true");
        CHECK(gstr(c, "rawequal(ring.head.next.next.next, ring.head)") == "true");
        CHECK(gstr(c, "by_obj[Obj1] .. by_obj[Obj2]") == "onetwo");
        CHECK(gstr(c, "acc(0)") == acc_total && acc_total != "0");  // the closure's upvalue came back
#if defined(ALAMO_LUA_PLAYERS)
        CHECK(gstr(c, "by_player[Find_Player('Empire')]") == "emp");
#endif
    }

    const size_t ma0 = a.messages().size();
    bool same = true;
    for (uint32_t f = 40; f < 200; ++f) {
        step(a, f);
        step(b, f);
        const auto& ma = a.messages();
        const auto& mb = b.messages();
        if (ma.size() - ma0 != mb.size()) {
            if (same) std::printf("  frame %u: %zu vs %zu messages\n", f, ma.size() - ma0, mb.size());
            same = false;
        } else {
            for (size_t i = 0; same && i < mb.size(); ++i)
                if (ma[ma0 + i] != mb[i]) {
                    std::printf("  frame %u message %zu:\n    A: %s\n    B: %s\n", f, i, ma[ma0 + i].c_str(), mb[i].c_str());
                    same = false;
                }
        }
        const LuaVm::HeapInfo ha = a.heap_info(), hb = b.heap_info();
        if (ha.bytes != hb.bytes || ha.serial != hb.serial || ha.threshold != hb.threshold || !(rng_a == rng_b)) {
            if (same) std::printf("  frame %u: heap / RNG differ\n", f);
            same = false;
        }
    }
    CHECK(same);
    CHECK(rng_a == rng_b);
    CHECK(block(a) == block(b));
    CHECK(b.errors().empty());
    // The event queued at save time was delivered after the load, and the sleeping main went on counting.
    bool signal = false, main_line = false, identity = false;
    for (const std::string& m : b.messages()) {
        signal = signal || m.rfind("[print] signal ", 0) == 0;
        main_line = main_line || m.rfind("[print] main ", 0) == 0;
        identity = identity || m.find("same=true cyc=true") != std::string::npos;
    }
    CHECK(signal && main_line && identity);
    CHECK(gstr(b, "signals") == gstr(a, "signals") && gstr(b, "shared.count") == gstr(a, "shared.count"));
}

// The refusals: each leaves the target as it was.
static void test_refusals(const World& w) {
    GameRandom rng(4, 4);
    LuaVm a(w.host(&rng), plot_options());
    a.set_object_global("Obj1", w.h[0]);
    a.set_object_global("Obj2", w.h[1]);
    CHECK(run(a, kPlot) == "");
    a.create_thread("main");
    for (uint32_t f = 0; f < 10; ++f) step(a, f);
    const std::vector<uint8_t> good = block(a);
    std::string err;

    // Another script name / other options.
    LuaVmOptions other = plot_options();
    other.script_name = "Another_Script";
    LuaVm wrong_name(w.host(&rng), other);
    const std::vector<uint8_t> wn0 = image(wrong_name);
    CHECK(!restore(wrong_name, good, ScriptLoad::FromSave, &err) && !err.empty());
    CHECK(wrong_name.fresh() && image(wrong_name) == wn0);
    LuaVmOptions math = plot_options();
    math.math_library = true;
    LuaVm wrong_opts(w.host(&rng), math);
    CHECK(!restore(wrong_opts, good, ScriptLoad::FromSave) && wrong_opts.fresh());

    // A target that ran code: refused as FromSave, accepted as Any (its state is then replaced whole).
    LuaVm rebuilt(w.host(&rng), plot_options());
    CHECK(run(rebuilt, "rebuilt_only = 1") == "");
    rebuilt.create_thread("Pump");
    const std::vector<uint8_t> rb0 = image(rebuilt);
    CHECK(!restore(rebuilt, good, ScriptLoad::FromSave, &err));
    CHECK(image(rebuilt) == rb0);
    CHECK(restore(rebuilt, good, ScriptLoad::Any, &err));
    CHECK(block(rebuilt) == good);  // before gstr below runs code in it
    CHECK(gstr(rebuilt, "rebuilt_only") == "nil" && rebuilt.thread_list() == a.thread_list());

    // Damaged and truncated blocks.
    LuaVm target(w.host(&rng), plot_options());
    const std::vector<uint8_t> t0 = image(target);
    for (size_t pos : {size_t{0}, size_t{4}, size_t{12}, size_t{20}, good.size() / 2, good.size() - 1}) {
        std::vector<uint8_t> bad = good;
        bad[pos] = static_cast<uint8_t>(bad[pos] ^ 0x5a);
        CHECK(!restore(target, bad, ScriptLoad::FromSave));
    }
    const size_t stride = good.size() > 400 ? good.size() / 200 : 1;
    for (size_t len = 0; len < good.size(); len += stride) {
        std::vector<uint8_t> cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(len));
        ScriptBlockHeader hd;
        CHECK(!peek_script_block(cut, hd));
        CHECK(!restore(target, cut, ScriptLoad::FromSave));
    }
    std::vector<uint8_t> longer = good;
    longer.push_back(0);
    CHECK(!restore(target, longer, ScriptLoad::FromSave));
    CHECK(target.fresh() && image(target) == t0);
    CHECK(restore(target, good, ScriptLoad::FromSave));

    // A VM that cannot be saved (a debug hook): the block says so and is refused.
    LuaVm hooked(w.host(&rng), plot_options());
    CHECK(run(hooked, "x = 1") == "");
    lua_sethook(hooked.state(), [](lua_State*, lua_Debug*) {}, LUA_MASKCOUNT, 1000);
    const std::vector<uint8_t> unsaved = block(hooked);
    lua_sethook(hooked.state(), nullptr, 0, 0);
    ScriptBlockHeader hd;
    CHECK(peek_script_block(unsaved, hd) && !hd.saved && !hd.save_error.empty());
    LuaVm t2(w.host(&rng), plot_options());
    CHECK(!restore(t2, unsaved, ScriptLoad::FromSave, &err) && err.find("could not be saved") != std::string::npos);
    CHECK(t2.fresh());
}

// An object script (OBJECT_SCRIPTS §1.3 / §2.6): the normal attach (top level, Base_Definitions, main, Object), a few
// pumps, save; the from-save attach (constructor only) continues - one `main`, OnEnter not re-run, Object the same object.
static void test_object_script(const World& w) {
    static const char* const script = R"(
function Base_Definitions() ServiceRate = 0.1 Spawned = 0 Ticks = 0 end
function main()
  Spawned = Spawned + 1
  print('spawn ' .. Object.Get_Object_ID())
  local mine = Object
  while true do
    Ticks = Ticks + 1
    if not rawequal(mine, Object) then print('object changed') end
    coroutine.yield(true)
  end
end
)";
    LuaVmOptions o;
    o.script_name = "ObjectScript_Block_Test";
    o.upper_type_names = true;
    GameRandom rng_a(8, 8);
    LuaVm a(w.host(&rng_a), o);
    CHECK(run(a, script) == "");
    std::string err;
    CHECK(a.call("Base_Definitions", &err));
    CHECK(a.create_thread("main") == 0);
    a.set_object_global("Object", w.h[2]);
    for (uint32_t f = 0; f < 6; ++f) {
        a.set_time(f / 30.0);
        a.service_threads();
    }
    const std::vector<uint8_t> saved = block(a);
    ScriptBlockHeader hd;
    CHECK(peek_script_block(saved, hd) && hd.upper_type_names && hd.threads.size() == 1 && hd.threads[0].function == "main");

    GameRandom rng_b = rng_a;
    LuaVm b(w.host(&rng_b), o);  // from-save: no require, no Base_Definitions, no main, no Object
    CHECK(restore(b, saved, ScriptLoad::FromSave, &err));
    CHECK(b.thread_count() == 1 && b.thread_list().size() == 1);
    // The same query in both VMs keeps their heaps in step (gstr runs code).
    CHECK(gstr(a, "Object.Get_Object_ID()") == "3" && gstr(b, "Object.Get_Object_ID()") == "3");
    for (uint32_t f = 6; f < 20; ++f) {
        for (LuaVm* vm : {&a, &b}) {
            vm->set_time(f / 30.0);
            vm->service_threads();
        }
    }
    CHECK(block(a) == block(b));  // before the one-sided queries below
    CHECK(gstr(b, "Spawned") == "1" && gstr(b, "Ticks") == gstr(a, "Ticks") && gstr(b, "Ticks") == "20");
    CHECK(b.messages().empty());  // no second 'spawn', no 'object changed'
}

int main() {
    log::set_min_level(log::Level::Error);
    World w;
    CHECK(w.ok);
    if (!w.ok) return 1;
    std::printf("plot script\n");
    test_plot(w);
    std::printf("refusals\n");
    test_refusals(w);
    std::printf("object script\n");
    test_object_script(w);
    std::printf(g_fail ? "lua script save tests: %d FAILED\n" : "lua script save tests: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

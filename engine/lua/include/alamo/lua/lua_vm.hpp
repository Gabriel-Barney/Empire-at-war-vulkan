// One script VM (the original's LuaScriptClass: one lua_State per script - story plots, AI plans,
// GameObject scripts, evaluators, free stores; docs/re/SUBSYSTEMS.md "lua_script").
//
// Threading: a LuaVm is owned by ONE thread (the sim thread that created it, or the last one to
// call claim_ownership() between frames). Every entry point checks it and aborts on a foreign
// thread; no job-system worker may touch a VM. Lua itself is not thread-safe and scripts draw from
// the synchronized RNG, so script execution is part of the sim's single-threaded semantic order.
// One exception (OPT2 t04): collect_detached() runs a collection on a worker while the owner waits
// for it (gc_schedule.hpp); it runs no Lua code.
//
// Determinism (rule 3):
//   * the VM is the original's Lua 5.0.2 (double numbers), compiled as C++ with /fp:precise or
//     -ffp-contract=off; table/function/userdata keys hash by a creation serial, not by address
//     (patched at build time, engine/lua/CMakeLists.txt), so pairs() order is reproducible;
//   * libraries: base (+coroutine), string, table - as the original. No io / os / debug /
//     loadlib. No math table (the original exposes none: PGBase.lua's Dirty_Floor comment); an
//     opt-in math table (LuaVmOptions::math_library) uses alamo::dmath and the synced GameRandom;
//   * require / dofile / loadfile read through the FileProvider (Data\Scripts roots), print and
//     the debug bindings go to the engine log, never to stdout;
//   * GameRandom / GameRandom.Get_Float draw from the synchronized stream; GameRandom.Free_Random
//     keeps the original's quirk (no-argument form draws from the SYNCHRONIZED stream; ranged forms
//     from the free stream in single player, the synced one in multiplayer) - GameRandom in
//     engine/core;
//   * script threads are resumed in thread-id order (creation order), once per service.
//
// Script threads (Create_Thread / Thread.*, docs/re/LUA_API.md): Lua coroutines started from a
// global function by name. service_threads() resumes each live thread once; a thread that yields
// `false` (PGBase ScriptExit) or returns ends; a Lua error ends it and is reported. Threads created
// while servicing first run on the NEXT service (PROVISIONAL: the original's start point is not
// traced). Thread ids count from 0 per VM; GetThreadID() is -1 outside a thread (PGBase tests >= 0).
// PGBase's Sleep/BlockOnCommand/PumpEvents run unchanged on top of: ThreadValue (per-thread values),
// GetCurrentTime (set_time), GetEvent (post_event) and coroutine.yield.
//
// Object handles: a GameObjectWrapper userdata holds an ObjectHandle of the host's current store
// plus the object id. Is_Valid() is false once the object is dead (destroyed, even before the
// flush) or gone; every other method then raises "GameObjectWrapper::<Method> -- this object is
// dead!" (docs/re/OBJECT_RUNTIME.md §1.5). One script gets the same userdata for the same object
// (cache keyed by object id, never iterated for logic; sweep_dead_wrappers() drops dead entries).
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/objects/object_store.hpp"

struct lua_State;

namespace alamo {
class FileProvider;
class GameRandom;
}  // namespace alamo
namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::lua {

// What a VM can reach in the engine. Pointers must outlive the VM (or be reset to null).
struct LuaHost {
    objects::ObjectStore* store = nullptr;            // current mode's primary store
    const objects::GameObjectTypes* types = nullptr;  // Find_Object_Type, Get_Type
    GameRandom* rng = nullptr;                        // GameRandom, Free_Random, math.random
    bool multiplayer = false;                         // Free_Random falls back to the synced stream
    bool campaign = false;                            // Is_Campaign_Game (a galactic campaign session)
    const FileProvider* files = nullptr;              // require / dofile / loadfile
};

struct LuaVmOptions {
    std::string script_name = "Script";  // tostring(Script)
    bool math_library = false;           // expose the dmath-backed 'math' table (not in the original)
    // q6 object scripts: GameObjectTypeWrapper.Get_Name returns the upper-cased name, as the original (it stores type
    // names upper-cased, OBJECT_TYPES §3.1). [port interim] Off by default: other VMs keep the name as written.
    bool upper_type_names = false;
};

struct UnboundHit {
    std::string name;  // "Find_Player" or "GameObjectWrapper.Get_Position"
    uint32_t count = 0;
};

class LuaVm {
public:
    explicit LuaVm(const LuaHost& host, LuaVmOptions options = {});
    ~LuaVm();
    LuaVm(const LuaVm&) = delete;
    LuaVm& operator=(const LuaVm&) = delete;

    // ---- ownership ----
    bool on_owner_thread() const { return std::this_thread::get_id() == owner_; }
    // Re-bind to the calling thread (a frame-boundary hand-off; the previous owner must be done).
    void claim_ownership() { owner_ = std::this_thread::get_id(); }

    LuaHost& host();
    lua_State* state();  // raw access for engine code on the owner thread

    // ---- loading / running (main thread of the VM, protected calls) ----
    // Compiles a chunk (source or Lua 5.0 bytecode, detected by the "\033Lua" signature) without
    // running it. False + *err on a syntax error / bad bytecode.
    bool compile(std::string_view chunk, std::string_view chunk_name, std::string* err = nullptr);
    // Compiles and runs a chunk.
    bool run(std::string_view chunk, std::string_view chunk_name, std::string* err = nullptr);
    // Loads Data\Scripts\<root>\<name>.lua/.lc through the host FileProvider and runs it once
    // (require semantics: _LOADED[name]).
    bool require(std::string_view name, std::string* err = nullptr);
    // Calls a global function with no arguments (not as a thread: it must not yield).
    bool call(std::string_view global_function, std::string* err = nullptr);
    // True when the global `name` is a function (raw lookup, no metamethods; q6 t06: optional engine calls).
    bool has_function(std::string_view name);
    // q6 t09 (GAME_SCORING.md §2 ServiceRate / LastService): a global number by raw lookup. get_number is false (out
    // unchanged) when the global is not a Lua number (nil, a string, ...); set_number writes a new number global.
    bool get_number(std::string_view name, double& out);
    void set_number(std::string_view name, double value);

    // ---- script threads ----
    // Create_Thread(name) from C++. Returns the thread id, or -1 if `name` is not a global function.
    int32_t create_thread(std::string_view global_function);
    // Resumes every live thread once in id order. Returns the number still alive. Errors are
    // appended to errors(); _ScriptExit kills every thread at the next yield.
    size_t service_threads();
    size_t thread_count() const;
    bool thread_active(int32_t id) const;
    bool exit_requested() const;

    // ---- host inputs ----
    void set_time(double seconds);  // GetCurrentTime()
    void set_frame(uint32_t frame);  // GetCurrentTime.Frame()
    // Queues an event for GetEvent(): the global function `name` (called by PGBase's PumpEvents).
    void post_event(std::string_view name);
    // Sets a host value global (Object, Target, ...) to the wrapper of `h` (null handle -> nil).
    void set_object_global(std::string_view name, objects::ObjectHandle h);
    // Drops cached wrappers of dead objects (call at a deterministic point, e.g. after a flush).
    void sweep_dead_wrappers();

    // ---- q6 script pool (docs/re/OBJECT_SCRIPTS.md §1.3, §1.4, §2.3; the host is engine/apps/sim object_scripts) ----
    // Release steps 1-3 of 0x2488e0: the exit flag set, every thread killed and removed (thread ids restart at 0, as the
    // original's thread vector is emptied), the ThreadValue table and the GetEvent queue cleared. Globals are untouched:
    // the caller runs Flush_G() / Base_Definitions() next.
    void pool_kill_threads();
    // Release steps 5.3 / 5.5: the Lua stack reset, then one full collection (finalizers included).
    void collect_garbage();
    // Handout of a pooled instance (0x244170): the exit flag (+0x121) cleared, no current thread (+0x60 = -1).
    void pool_handout();
    // §1.3 step 3 (0x2567b0 / 0x546c70 at every attach): the engine globals of the binding table registered again,
    // replacing whatever a script stored under those names (`Script`, a constructor global, is left alone).
    void reinstall_engine_globals();
    // A new LuaHost for a pooled instance handed to another object. When the store changes, the game-object wrapper
    // caches are emptied (their handles belong to the old store).
    void rebind_host(const LuaHost& host);

    // ---- diagnostics ----
    const std::vector<std::string>& messages() const;  // _ScriptMessage / _OuputDebug / print lines
    const std::vector<std::string>& errors() const;    // thread errors
    std::vector<UnboundHit> unbound_hits() const;      // names sorted, counts > 0 only
    uint64_t unbound_total() const;
    // How a method resolves on a wrapper of class `cls`: 1 implemented, 0 unbound stub, -1 missing.
    int resolve_method(std::string_view cls, std::string_view name) const;

    // ---- N1 save / load (contract: alamo/core/save_file.hpp; heap layout: src/vm_persist.cpp) ----
    // write(): the whole script state - every Lua object (globals, registry, tables with their exact node layout,
    // closures, prototypes, upvalues, coroutines suspended in yield with their frames and saved pcs, userdata,
    // creation serials and the serial counter, Lua's byte count / GC threshold) plus the engine side (script
    // threads: ids, function names, flags, next id; set_time / set_frame; exit flag). Not saved: messages(),
    // errors(), unbound counters (diagnostics), host and options (the reader's own).
    // Must be called between services (no script running). When the state cannot be saved (save_error() says
    // why, e.g. a debug hook or an unknown C function) write() stores a record read() rejects and logs an error.
    void write(ByteWriter& w) const;
    // read(): into a freshly constructed VM with the same LuaHost and options; attach the host bridges BEFORE
    // reading (registry attachments - the bridges' light userdata - are kept from this VM by key). Replaces the
    // whole Lua state (no script source is run); requires r.done() at the end. False (VM unchanged) on bad data.
    bool read(ByteReader& r);
    const std::string& save_error() const;

    // N1 diagnostics: the Lua heap's accounting (a restored VM matches the original exactly).
    struct HeapInfo {
        uint32_t serial = 0;     // last creation serial (table / function / userdata / thread keys hash by it)
        uint64_t bytes = 0;      // Lua's own byte count (lstate.h nblocks)
        uint64_t threshold = 0;  // the next automatic collection runs at bytes >= threshold
        uint32_t strings = 0;    // interned strings
    };
    HeapInfo heap_info() const;

    // ---- garbage collection (OPT2 t04, scheduled by alamo/lua/gc_schedule.hpp) ----
    // True while a Lua call runs in this VM (a script, a binding, a service): no scheduled collection then.
    bool gc_busy() const;
    // One full collection (Lua 5.0.2's own mark and sweep and new threshold, the latter then scaled by
    // threshold_percent, at least 100) WITHOUT the __gc finalizers, which wait for run_finalizers(). The one entry
    // point that may run on a thread that does not own the VM: a job worker of collect_scheduled() while the owner
    // thread waits for that job and nothing else touches the VM. It runs no Lua code and touches nothing outside
    // this VM's own heap. Requires !gc_busy().
    void collect_detached(uint32_t threshold_percent = 100);
    // Runs the finalizers the last collect_detached() queued, if any (owner thread, between services).
    void run_finalizers();
    // Teardown (OPT3 t11): runs on the owner thread the Lua code lua_close would run - the __gc finalizers of every
    // finalizable userdata, with lua_close's own steps (upvalues closed, finalizers separated, errors retried) - so
    // that destroying the VM afterwards only frees this VM's memory and may run on another thread
    // (destroy_detached, gc_schedule.hpp). No Lua code may run in the VM after it. Requires !gc_busy().
    void finalize_for_close();

    struct Impl;

private:
    void check_owner() const;
    std::unique_ptr<Impl> impl_;
    std::thread::id owner_;
};

}  // namespace alamo::lua

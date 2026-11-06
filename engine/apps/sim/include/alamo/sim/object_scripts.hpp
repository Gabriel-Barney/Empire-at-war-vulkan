// q6: game-object Lua scripts (docs/re/OBJECT_SCRIPTS.md): one script instance per object whose type names a
// Lua_Script, attached at construction, serviced after the store update, released on ScriptExit / a binding error /
// death. Owned by SimHost (SimConfig::object_scripts; off by default, so sessions without it keep their CRC streams).
//
// POOL (§1.4, 0x244170): instances come from a pool keyed by the script name, one per host (session). The first use of a
// name loads one instance (new VM, the file's top level run) and reads its global ScriptPoolCount once: absent -> a pool
// of 1 (this instance); N >= 1 -> N - 1 more instances loaded at once (top level run for each, in order), all in the
// pool; 0 (or less, or not a whole count above 0) -> no pool, this and every later instance of the name is a fresh,
// unpooled VM. A known name hands out the last free instance in list order, else loads one more and appends it (pools
// grow to the peak live count). Handout: exit flag cleared, no current thread, the VM's host rebound to the object's
// store (wrapper caches emptied when the store changes).
// ATTACH (§1.3, ObjectStore::on_created of every mode's PRIMARY store). Order: the instance (pool), the engine globals
// registered again (LuaVm::reinstall_engine_globals) + GlobalValue + the bridges (player world, object bridge; null
// detaches), Base_Definitions() only for a fresh instance (a reused one ran it at its release), the `main` thread
// (created, not run), the global Object last (so Object is nil in the top level and in Base_Definitions). A file that is
// missing or does not compile = no script. Death clones get no script (TacticalMode::spawn_death_clone suppresses the
// hook: the original attaches and releases at once, §1.2) [port: Base_Definitions does not run for them].
// [port] Secondary stores (local / cosmetic objects) attach nothing.
// RELEASE (§2.3, 0x2488e0): threads killed (ids restart at 0), ThreadValue / GetEvent cleared; a pooled instance then runs
// Flush_G() (when a function) and Base_Definitions() at once, the stack is reset and a full collection runs, and it is
// free again; an unpooled one is closed. [port] The bridges are detached before Flush_G / Base_Definitions run (bindings
// called there see the store only: at a mode end the battle's systems may already be shut down), and the next handout
// attaches the new store's. [port] Proximity registrations of a released object stay in its VM's registry: they are keyed
// by object id, ids are never reused, so they never fire again.
// TYPE INSTANCES (§1.5, Execute_Script_On_Type, type +0x5D): at type setup ([port] the first mode pushed with the script
// files set) every type with Execute_Script_On_Type and a Lua_Script takes one instance from the same name pool (type
// index order), engine globals registered, Base_Definitions() once while fresh [port: the original may run it once per
// load pass, §1.5 M]; no main, no Object, never serviced, never released (the host keeps it for the session; End_All
// only collects it, so its globals survive). Read by the AI hero setup C 0x726710 (type_vm); [port] the AI ports do not
// call into hero scripts (galactic_pp.cpp / tactical_pp.cpp HeroM notes), so nothing calls it yet.
//
// SERVICE (§3), service(mode) right after the mode's primary store update (GameMode::common_tick; GalacticMode after
// GalacticSim::update), not while the mode is paused: the scripts of that store newest-attached first (the store's
// serviced order, newest registered first; scripts attached during the pass run from the next service). For each:
//   * object gone, dead or dying -> released (death stops the service at once, §2.4; [port] freed at once instead of
//     at the deferred delete: nothing can resume it either way);
//   * the global ServiceRate is not a number -> nothing;
//   * now = (double)((float)mode frame x 0.033333335f); pump when LastService is not a number or now - LastService >
//     ServiceRate (strict, double);
//   * exit flag already set (ScriptExit from Base_Definitions, a binding script error) -> released without a pump;
//   * pump = LuaVm::service_threads (every live thread once, creation order; a Lua runtime error kills only its
//     thread; ScriptExit / a binding script error stops the pass) with time = now, frame = the mode frame;
//   * exit flag set -> released; else LastService := -(one synced draw in [0, first_pump_draw_hi(ServiceRate)]) when
//     LastService was not a number before the pump (the first pump), else now.
// Everything runs on the sim thread in that fixed order, so worker counts never change it (rule 3). The pool adds no
// RNG draw: which instance an object gets (list order) and when the top levels / release-time Base_Definitions run are
// fixed points of that order.
// SAVES (q6 t19, §2.6, LUA_SAVE.md; SimHost chunk "object_scripts", write / read below). Saved: every live script
// (object id + handle, its pool name = the script name, which instance it holds) and its instance's script block
// (lua::write_script_block: the whole Lua heap - globals incl. LastService, threads at their saved pc, GetEvent queue,
// wrappers - plus the thread records); every pool in name order with each instance's flags (in use, fresh) and block;
// the type instances (type index -> instance); the stats. The load is the from-save attach of §1.3: a new VM per
// instance by its saved name, GlobalValue (and, for an instance a live script holds, its store's player world and object
// bridge) attached, then the block - no file top level, no Base_Definitions, no `main`, no `Object`: the saved state
// supplies them, so OnEnter-style code is not re-run (TR's company spawner does not spawn again) and no new first-pump
// draw happens. Everything the rebuilt session attached before the load is replaced whole. [port] Free pooled
// instances and the type instances are saved too (retail saves only the objects' instances and rebuilds the type
// instances at type setup, §1.5): pools keep their size, order and Flush_G'd globals, and the type instance's globals
// survive like in a running session. A load taken during a battle restores the scripts in pass 2 only (every store is
// on the stack then).
// [port] Diagnostics (messages(), error_counts()) are not saved.
//
// Diagnostics: per-object messages ("<mode frame> <script> (id N): <text>") and a count per distinct error text.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo {
class FileProvider;
}
namespace alamo::lua {
class LuaVm;
class ObjectBridge;
class LuaPlayerWorld;
class GlobalValueStore;
}  // namespace alamo::lua

namespace alamo::sim {

class GameMode;

// §3.2 step 7 / §8 item 1: the upper bound of the first-pump draw is not in the decompile (hi is in xmm2 at the call).
// [port] best guess: ServiceRate itself (the draw then spreads the second pumps over the first period, §3.2 step 8).
// Not finite or < 0 -> 0. The one place to change it.
float first_pump_draw_hi(double service_rate);

// The mode-frame clock of §3.2 step 4.
inline constexpr float kObjectScriptSecondsPerFrame = 0.033333335f;
double object_script_time(uint64_t mode_frame);

class ObjectScriptHost {
public:
    explicit ObjectScriptHost(GameRandom& random);
    ~ObjectScriptHost();
    ObjectScriptHost(const ObjectScriptHost&) = delete;
    ObjectScriptHost& operator=(const ObjectScriptHost&) = delete;

    // Session inputs (before the modes are pushed). files: the script roots (null = nothing attaches).
    // Set after a mode was pushed: the type instances load now (into the first store's pool binding).
    void set_files(const FileProvider* files);
    void set_global_values(lua::GlobalValueStore* g) { globals_ = g; }
    void set_campaign(bool campaign) { campaign_ = campaign; }

    // SimHost::start_game: hooks the mode's primary store (scripts attach from now on).
    void mode_pushed(GameMode& mode);
    // SimHost::end_game_mode, before the pop: every script of the mode's store is released, the hook removed.
    void mode_popping(GameMode& mode);
    // The store's object bridge / player world (the battle host, the story): attached to the store's existing scripts
    // and to every later one. Null detaches. They must outlive the scripts of that store (or be reset first).
    void set_bridge(const objects::ObjectStore& store, lua::ObjectBridge* bridge);
    void set_player_world(const objects::ObjectStore& store, lua::LuaPlayerWorld* world);
    // §3: after the mode's primary store update (the caller skips paused modes).
    void service(GameMode& mode);

    // q6 t19: N1 save / load (SAVES above), between ticks. write: false (+ *err) when an instance's Lua state cannot be
    // saved (LuaVm::save_error; the bytes are then not loadable). read: the rebuilt session's stores must be the saved
    // ones (same count and mode kinds, push order); false (+ *err) on damaged data or a refused block, nothing changed.
    static constexpr uint32_t kSaveVersion = 1;
    bool write(ByteWriter& w, std::string* err = nullptr) const;
    bool read(ByteReader& r, std::string* err = nullptr);

    // Live scripts in every store.
    size_t live() const;
    size_t live(const objects::ObjectStore& store) const;
    bool has_script(const objects::ObjectStore& store, objects::ObjectHandle h) const;
    const lua::LuaVm* vm_of(const objects::ObjectStore& store, objects::ObjectHandle h) const;
    // §1.5: the type instance of `t` (Execute_Script_On_Type), null when the type has none.
    const lua::LuaVm* type_vm(objects::TypeIndex t) const;
    size_t type_instances() const { return type_scripts_.size(); }
    // §1.4: one entry per pooled script name (ascending name): instances in the pool and how many are handed out.
    struct PoolInfo {
        std::string name;
        size_t size = 0, in_use = 0;
    };
    std::vector<PoolInfo> pools() const;

    struct Stats {
        uint64_t attached = 0;       // object attaches (Base_Definitions run when fresh, main thread made)
        uint64_t load_failures = 0;  // missing / not compiling: no script
        uint64_t loaded = 0;         // instances loaded (new VM, top level run): pool loads + unpooled ones
        uint64_t reused = 0;         // attaches that took a free pooled instance
        uint64_t pooled_releases = 0;  // releases that returned an instance to its pool (Flush_G + Base_Definitions)
        uint64_t pumps = 0;          // services that resumed the threads
        uint64_t first_draws = 0;    // synced draws for the first-pump LastService
        uint64_t released_exit = 0;  // ScriptExit or a binding script error (exit flag)
        uint64_t released_dead = 0;  // the object died / is gone
        uint64_t released_mode = 0;  // the mode ended
        uint64_t thread_errors = 0;  // Lua runtime errors (each killed one thread)
    };
    const Stats& stats() const { return stats_; }
    // "<mode frame> <script> (id N): <text>"; at most kMaxMessages lines kept (the counts below are complete).
    static constexpr size_t kMaxMessages = 4000;
    const std::vector<std::string>& messages() const { return messages_; }
    // Each distinct thread error / SCRIPT ERROR / load error text (the object id stripped) with its count.
    const std::map<std::string, uint64_t>& error_counts() const { return error_counts_; }
    uint64_t error_total() const;

private:
    struct Instance;
    struct Script;
    struct StoreScripts;
    struct TypeScript;
    StoreScripts* find(const objects::ObjectStore& store);
    const StoreScripts* find(const objects::ObjectStore& store) const;
    void attach(StoreScripts& ss, objects::ObjectHandle h);
    void load_type_scripts(StoreScripts& ss);
    // §1.4: an instance of `name` bound to the store of `ss` (null: load failure, noted). Unpooled instances are
    // returned through `owned`.
    Instance* acquire(StoreScripts& ss, const std::string& name, objects::ObjectId id, std::unique_ptr<Instance>& owned);
    std::unique_ptr<Instance> load(StoreScripts& ss, const std::string& name, objects::ObjectId id);
    void collect(uint64_t frame, Instance& in, objects::ObjectId id);
    void release(StoreScripts& ss, Script& s, uint64_t& counter);
    void note(uint64_t frame, const std::string& name, objects::ObjectId id, const std::string& what, bool error);

    GameRandom* random_;
    const FileProvider* files_ = nullptr;
    lua::GlobalValueStore* globals_ = nullptr;
    bool campaign_ = false;
    std::vector<std::unique_ptr<StoreScripts>> stores_;  // push order
    // §1.4: name -> the pool's instances in list order (string keys; lookups and in-order walks only).
    std::map<std::string, std::vector<std::unique_ptr<Instance>>> pools_;
    std::vector<std::unique_ptr<TypeScript>> type_scripts_;  // ascending type index
    bool types_loaded_ = false;
    Stats stats_;
    std::vector<std::string> messages_;
    std::map<std::string, uint64_t> error_counts_;  // diagnostics only (never read by the sim)
};

}  // namespace alamo::sim

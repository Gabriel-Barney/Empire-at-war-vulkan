// q6: game-object Lua scripts (docs/re/OBJECT_SCRIPTS.md): one script instance per object whose type names a
// Lua_Script, attached at construction, serviced after the store update, released on ScriptExit / a binding error /
// death. Owned by SimHost (SimConfig::object_scripts; off by default, so sessions without it keep their CRC streams).
//
// ATTACH (§1.3, ObjectStore::on_created of every mode's PRIMARY store): a fresh VM per object [port: no pool, §7.1 item
// 3]. Order: the file's top level (require), GlobalValue + the bridges (player world, object bridge when the host has
// set one for the store), Base_Definitions() when it is a function, the `main` thread (created, not run), the global
// Object last (so Object is nil in the top level and in Base_Definitions). A file that is missing or does not compile
// = no script. Death clones get no script (TacticalMode::spawn_death_clone suppresses the hook: the original attaches
// and releases at once, §1.2) [port: Base_Definitions does not run for them].
// [port] Secondary stores (local / cosmetic objects) attach nothing. Execute_Script_On_Type (§1.5) is not loaded.
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
// Everything runs on the sim thread in that fixed order, so worker counts never change it (rule 3).
//
// Diagnostics: per-object messages ("<mode frame> <script> (id N): <text>") and a count per distinct error text.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

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
    void set_files(const FileProvider* files) { files_ = files; }
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

    // Live scripts in every store (save_allowed refuses while > 0).
    size_t live() const;
    size_t live(const objects::ObjectStore& store) const;
    bool has_script(const objects::ObjectStore& store, objects::ObjectHandle h) const;
    const lua::LuaVm* vm_of(const objects::ObjectStore& store, objects::ObjectHandle h) const;

    struct Stats {
        uint64_t attached = 0;       // instances created (Base_Definitions run, main thread made)
        uint64_t load_failures = 0;  // missing / not compiling: no script
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
    struct Script;
    struct StoreScripts;
    StoreScripts* find(const objects::ObjectStore& store);
    const StoreScripts* find(const objects::ObjectStore& store) const;
    void attach(StoreScripts& ss, objects::ObjectHandle h);
    void collect(StoreScripts& ss, Script& s);
    void release(StoreScripts& ss, Script& s, uint64_t& counter);
    void note(uint64_t frame, const Script& s, const std::string& what, bool error);

    GameRandom* random_;
    const FileProvider* files_ = nullptr;
    lua::GlobalValueStore* globals_ = nullptr;
    bool campaign_ = false;
    std::vector<std::unique_ptr<StoreScripts>> stores_;  // push order
    Stats stats_;
    std::vector<std::string> messages_;
    std::map<std::string, uint64_t> error_counts_;  // diagnostics only (never read by the sim)
};

}  // namespace alamo::sim

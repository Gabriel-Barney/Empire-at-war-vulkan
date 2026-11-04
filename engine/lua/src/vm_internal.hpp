// Internals shared by the VM, the dispatcher and the binding implementations.
//
// Error discipline: Lua 5.0 raises errors with longjmp, which skips C++ destructors. So no Lua error
// is ever raised while a C++ object with a destructor is alive in a binding:
//   * binding implementations (ImplFn) never call lua_error / luaL_check*. q6 t11 (docs/re/LUA_API.md "Binding error
//     model across all bindings"): a binding never raises a Lua error at all. An error-level diagnostic is
//     Call::script_error_stop (log, set the script's stop flag, return no values), a warning Call::script_warning (log
//     only); the legacy Call::fail (writes Call::err, returns -1: the dispatcher raises it, pcall catches it) is no
//     longer used by any binding;
//   * the dispatcher's outermost frame (no C++ locals) raises the Lua error for the port's own failures only (an
//     unbound binding, a C++ exception, a legacy fail()) after the implementation has returned and destroyed
//     everything.
// Only allocation failures inside lua_push* can still longjmp out of an implementation.
#pragma once
#include <cstdint>
#include <deque>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The vendored VM is compiled as C++ (engine/lua/CMakeLists.txt), so its headers are included with
// C++ linkage - no extern "C".
#include <lauxlib.h>
#include <lua.h>

#include "alamo/core/bytes.hpp"
#include "alamo/core/profile.hpp"
#include "alamo/lua/bindings.hpp"
#include "alamo/lua/lua_vm.hpp"

namespace alamo::lua {

namespace detail {

// OPT2 t08 (measurement): profiler zone "lua.exec" = time of the outermost Lua execution on this thread (a chunk run,
// a call, a thread service, an AI event / global call); a call made inside it (a binding calling another VM) is part of
// it. Wall-clock and write-only like every profiler zone.
class ExecZone {
public:
    ExecZone() noexcept : outer_(depth()++ == 0), scope_(outer_ ? zone() : profile::kNoZone) {}
    ~ExecZone() { --depth(); }
    ExecZone(const ExecZone&) = delete;
    ExecZone& operator=(const ExecZone&) = delete;

private:
    static int& depth() noexcept {
        thread_local int d = 0;
        return d;
    }
    static profile::ZoneId zone() noexcept {
        static const profile::ZoneId z = profile::register_zone("lua.exec");
        return z;
    }
    bool outer_;
    profile::Scope scope_;
};

inline constexpr uint32_t kNoBinding = 0xFFFFFFFFu;
inline constexpr uint32_t kNoClass = 0xFFFFFFFFu;

// Every engine userdata (globals, game objects, types, players, the Script object).
struct Ud {
    uint32_t cls = kNoClass;      // class id (class_names())
    uint32_t binding = kNoBinding;  // global row for callable globals
    uint64_t value = 0;           // ObjectHandle value / TypeIndex
    int32_t id = 0;               // object id / player id
    int32_t reserved = 0;
};

struct Call;
using ImplFn = int (*)(Call&);

struct ImplEntry {
    BindingKind kind;
    const char* cls;  // Method owner; "" for globals
    const char* name;
    ImplFn fn;
};
std::span<const ImplEntry> implementations();  // bind_registry.cpp: all families below, concatenated in this order
// One table per binding family, each in its own file so families can be worked on in parallel.
std::span<const ImplEntry> impl_core();          // bind_engine.cpp: threads, random, debug, basic object/type/player
std::span<const ImplEntry> impl_objects();       // bind_objects.cpp: finding, spawning and commanding game objects
std::span<const ImplEntry> impl_players();       // bind_players.cpp: players, factions, credits, AI, perception
std::span<const ImplEntry> impl_story();         // bind_story.cpp: story events/flags, objectives, radar blips, timers
std::span<const ImplEntry> impl_presentation();  // bind_presentation.cpp: camera, cinematics, fades, fog, audio, UI
std::span<const ImplEntry> impl_ai();            // bind_ai.cpp: task forces, AI targets, plan budget, free store (A2)
std::span<const ImplEntry> impl_ai_tactical();   // bind_ai_tactical.cpp: land/space task-force orders, tactical queries (A1)
std::span<const ImplEntry> impl_script_gaps();   // bind_script_gaps.cpp: special weapons, TSW, cinematics, markup, parent mode (L1/S4 wave 2b)
std::span<const ImplEntry> impl_fog();           // bind_fog.cpp: FogOfWar.* and LuaFOWCellsClass.Undo_Reveal (fog follow-up)
std::span<const ImplEntry> impl_script_remaining();  // bind_script_remaining.cpp: the last unbound globals / methods (L1 final)

// L1/S4 wave 2b (SCRIPT_GAPS.md §1.4 / §1.5): Land/SpaceTaskForceClass.Fire_Special_Weapon(type_name, target) on the
// task force's units (unit-list order). For the tactical AI family's binding (bind_ai_tactical.cpp): pass the task
// force's units (AiScriptHost::tf_units) and its player (§1.5 upgrade gate; -1 = the first unit's owner). Pushes the
// result (the firing unit, or false / nothing on a script error) and returns the value count, like an ImplFn.
int task_force_fire_special_weapon(Call& c, bool space, const std::vector<objects::ObjectHandle>& units, int32_t player);
// bind_script_remaining.cpp: calls GameObjectWrapper.<name> of `unit` with `nargs` arguments at stack index `first` (the
// task-force variants delegate to the object bindings). The results are dropped; returns the first result's truth (false
// on an error, whose message is discarded [port: the original logs per unit]).
bool call_object_method(Call& c, objects::ObjectHandle unit, const char* name, int first, int nargs);
// bind_objects.cpp, S7 t15 A1: GameObjectWrapper.Activate_Ability of `unit` with the script arguments at `first` (the
// task-force Activate_Ability, AI_TACTICAL §7.3); results dropped. True when the activation went through (the S9 command
// accepted the unit, or the event path issued it); false for a dead unit, an argument error or a refused activation.
bool object_activate_ability(Call& c, objects::ObjectHandle unit, int first, int nargs);
// bind_objects.cpp: TaskForceClass.Set_Targeting_Priorities(set [, type or category filter]) applied to `units` through
// the object binding (script argument 1 = set name, 2 = optional filter).
void task_force_set_targeting_priorities(Call& c, const std::vector<objects::ObjectHandle>& units);

// OI lua (A1 ask 2). bind_ai.cpp: script argument i as an AI wrapper - an AITargetLocationWrapper -> its game object,
// a task force -> its first live unit. Returns 0 when the argument is not an AI wrapper, 1 when it is (out = null
// when it resolves to no live object, or no AI host is attached). The objects family's position arguments use it.
int ai_wrapper_object(Call& c, int i, objects::ObjectHandle& out);
// An AITargetLocationWrapper with no live object (a region / marker target): its host position [port]. False otherwise.
bool ai_target_position(Call& c, int i, Vec3& out);
// q6 t03 (LUA_API.md "Unit order bindings: target arguments"): argument i as an AITargetLocationWrapper.
//   is_target: the argument is one (false: anything else, nothing else filled);
//   dead:      no AI host, the target is not valid (the original's AI target +0x38 null), or its object is gone [port];
//   object:    its live game object (AI target vf+0x30), null for a region / marker target;
//   has_position / position: the object's position, else the host's target_position (vf+0x68).
struct AiTargetArg {
    bool is_target = false;
    bool dead = false;
    objects::ObjectHandle object{};
    bool has_position = false;
    Vec3 position{};
};
AiTargetArg ai_target_arg(Call& c, int i);
// A task force at argument i (any TaskForceClass kind): true and its live units in host order (empty without a host).
bool ai_task_force_units(Call& c, int i, std::vector<objects::ObjectHandle>& out);
// bind_ai_tactical.cpp: EvaluatePerception(name, player [, target at target_index; 0 = none]) through the attached
// tactical AI host (AI targets and task forces accepted). Pushes the result and returns 1; -2 = no tactical host.
int tactical_evaluate_perception(Call& c, std::string_view name, int32_t player, int target_index);
// refixf t03, bind_fog.cpp: FOW_Object_Hidden 0x35f470(player, object, force) through the attached fog hooks
// (fog_bridge.hpp FogScriptHooks::object_hidden); false without fog.
bool object_fogged_for(Call& c, int32_t player, objects::ObjectHandle h, bool force);

// Class registry (sorted names; ids are indices). Built once from the binding table.
const std::vector<std::string>& class_names();
uint32_t class_id(std::string_view cls);
uint32_t parent_class(uint32_t cls);  // kNoClass at the root (LuaUserVar)

// q6 t11 (LUA_API.md "Mode gates", "Dead or null wrappers"): what the dispatcher checks before the implementation runs.
// The mode gate is tested first (an error), then the dead-wrapper rule of a GameObjectWrapper method.
enum class ModeGate : uint8_t {
    None,
    Tactical,  // error in a galactic mode (the VM's store, or the parent store for a parent-mode wrapper)
    Galactic,  // error in a tactical mode
};
enum class DeadRule : uint8_t {
    Silent,       // the default: returns nothing, no message, no flag
    Binding,      // the implementation tests it (Is_Valid = false, the unit orders, the proximity events)
    Error,        // script error "this object is already dead" (Call::script_error_stop)
    WarnNothing,  // warning "this object is dead!", returns nothing
    WarnZero,     // warning "this object is dead!", returns 0
};

struct MethodSlot {
    uint32_t row = kNoBinding;  // binding row, kNoBinding for extra (implemented-only) methods
    uint32_t cls = kNoClass;    // owner
    ImplFn fn = nullptr;        // nullptr = unbound stub
    std::string label;          // "GameObjectWrapper.Get_Position"
    ModeGate gate = ModeGate::None;
    DeadRule dead = DeadRule::Silent;  // GameObjectWrapper methods only
};
ModeGate global_gate(uint32_t row);
const std::vector<MethodSlot>& method_slots();
// Walks cls, its parents, then LuaUserVar. Returns an index into method_slots() or kNoBinding.
uint32_t find_method(uint32_t cls, std::string_view name);
ImplFn global_impl(uint32_t row);
// Row of a global by name (kNoBinding if not in the table).
uint32_t global_row(std::string_view name);
// Number of rows + extra methods (size of the unbound counter array).
size_t counter_slots();
uint32_t extra_counter_slot(uint32_t method_slot);

}  // namespace detail

struct ThreadRec {
    lua_State* co = nullptr;
    int ref = LUA_NOREF;  // keeps the coroutine alive (registry)
    std::string function;
    int nargs = 0;
    bool started = false;
    bool killed = false;
};

struct LuaVm::Impl {
    LuaVm* owner = nullptr;
    lua_State* L = nullptr;
    LuaHost host;
    LuaVmOptions options;
    double time = 0.0;
    uint32_t frame = 0;
    std::vector<std::string> messages, errors;
    std::vector<uint32_t> unbound;  // per binding row (then extra slots)
    uint64_t unbound_total = 0;
    std::map<int32_t, ThreadRec> threads;  // id order = service order
    int32_t next_thread = 0;
    int32_t current_thread = -1;
    bool exit_requested = false;
    bool mp_free_random_warned = false;
    // L1/S4 wave 2b (Get_Parent_Mode_Object): which store host.store is while a binding runs - 0 the VM's own, 1 the
    // parent mode's (ObjectBridge::parent_store, swapped in by the dispatcher for a method of a parent-mode wrapper).
    // GameObjectWrapper Ud::reserved holds the store tag of its object.
    int32_t store_tag = 0;
    std::string save_error;  // N1: why the last LuaVm::write could not save ("" = it saved)
    bool touched = false;    // q6 t19: LuaVm::fresh() is !touched

    void message(std::string line);
    void kill_thread(int32_t id);  // marks; removal happens in reap()
    void reap();
    // Starts a thread running the function at absolute stack index fn_index of `from` with `nargs`
    // arguments copied from args_first... Returns the id, or -1 if fn_index is not a function.
    int32_t start_thread(lua_State* from, int fn_index, int args_first, int nargs, std::string name);
};

namespace detail {

// One binding invocation. Script argument i (1-based) is at stack index first + i - 1.
struct Call {
    lua_State* L;
    LuaVm::Impl* vm;
    const Ud* self;  // the receiver (methods) or the global userdata (calls)
    int first;
    int nargs;
    char* err;
    size_t err_cap;

    int idx(int i) const { return first + i - 1; }
    bool has(int i) const { return i >= 1 && i <= nargs; }
    bool is_number(int i) const { return has(i) && lua_type(L, idx(i)) == LUA_TNUMBER; }
    bool is_string(int i) const { return has(i) && lua_type(L, idx(i)) == LUA_TSTRING; }
    double number(int i) const { return lua_tonumber(L, idx(i)); }
    std::string_view string(int i) const {
        const char* s = lua_tostring(L, idx(i));
        return s ? std::string_view(s, lua_strlen(L, idx(i))) : std::string_view();
    }
    const Ud* userdata(int i, uint32_t cls) const;  // nullptr unless an engine userdata of `cls`
    // Legacy (unwinds): writes err, returns -1; the dispatcher raises it as a Lua error that pcall catches. q6 t11: not
    // the original's behaviour; no binding uses it (kept for the port's own failures).
    int fail(const char* fmt, ...);
    // q6 t03 / t11 (LUA_API.md "What an error does to the script", "Binding error model across all bindings"): the
    // original's script error 0x248190, used for every error-level diagnostic of every binding. Logs "SCRIPT ERROR:
    // <text>" and sets the script's exited flag (+0x121, LuaVm::Impl::exit_requested, the flag ScriptExit sets), with no
    // Lua error: the binding returns nothing (returns 0), pcall succeeds, the calling thread runs on to its next yield,
    // and service_threads then resumes no other thread of the script and kills them all at the end of the pass; the
    // hosts release the script on exit_requested(). A binding that carries on after an error (a loop entry, a defaulted
    // argument) ignores the return value.
    int script_error_stop(const char* fmt, ...);
    // The original's warning 0x248620: logs "SCRIPT WARNING: <text>", sets no flag, returns 0 (nothing).
    int script_warning(const char* fmt, ...);
};

// Pushers. Wrappers are cached per VM: the same engine entity gives the SAME userdata every time, so
// scripts can key tables by wrappers (Lua 5.0 table lookup is raw identity; __eq is not consulted).
//   objects: cache keyed by object id (ids are never reused); dead entries dropped by
//            LuaVm::sweep_dead_wrappers(); a stale entry (same id, other handle) is replaced;
//   types:   keyed by type index, kept for the VM's lifetime;
//   players and the other entity wrappers (story plots/events, ...): push_cached_userdata, keyed by
//            (class, key), kept for the VM's lifetime (their entities live as long as the session).
// Value wrappers (PositionWrapper, BlockingStatus, created instances) are new userdata per call.
void push_object(lua_State* L, LuaVm::Impl& vm, objects::ObjectHandle h);
// L1/S4 wave 2b: the wrapper of object `h` of the parent mode's store (ObjectBridge::parent_store; nil without
// one). Its methods run on that store (the dispatcher swaps LuaVm::Impl::host.store, kParentStoreTag).
void push_parent_object(lua_State* L, LuaVm::Impl& vm, objects::ObjectHandle h);
objects::ObjectStore* parent_store(LuaVm::Impl& vm);  // nullptr without a bridge / parent store
// Pushes the cached wrapper of class ud.cls under `key` (exact integer in a double), creating it from
// `ud` when absent or when the cached payload differs.
void push_cached_userdata(lua_State* L, const Ud& ud, lua_Number key);
void push_type(lua_State* L, LuaVm::Impl& vm, uint32_t type_index);
void push_player(lua_State* L, LuaVm::Impl& vm, int32_t player_id);
bool object_alive(const LuaVm::Impl& vm, const Ud& ud);
objects::ObjectHandle object_handle(const Ud& ud);
// Saturating double -> int32 (truncation toward zero; NaN -> 0).
int32_t to_int(double d);

// GetEvent queue (one per VM, FIFO, kept in the Lua registry): appends the value at `fn_index` (a
// function, or a global function NAME resolved when GetEvent pops it) with the table at
// `params_index` as its GetEvent.Params() (0 = no parameters). bind_engine.cpp.
void queue_event(lua_State* L, int fn_index, int params_index);

// BlockingStatus kinds (Ud::reserved of a BlockingStatus userdata; BlockOnCommand polls IsFinished):
// 0 order / 1 reinforcement / 2 forever are answered by bind_objects.cpp; the galactic waits by the
// players family (bind_players.cpp, galactic_block_finished; always finished in builds without it).
inline constexpr int32_t kBlockOrder = 0, kBlockReinforcement = 1, kBlockForever = 2;
inline constexpr int32_t kBlockStarbaseLevel = 3;    // WaitForStarbase(planet, level): id = planet object id, value = level
inline constexpr int32_t kBlockGroundbaseLevel = 4;  // WaitForGroundbase(planet, level)
// (5 = kBlockBombingRun, script_remaining.hpp.) S7 t15 A1: AbilityBlockStatus of GameObjectWrapper.Activate_Ability (count
// 1): id = object, value = ObjectBridge::watch_ability at creation; finished once the bridge counted a later signal 0x29 of
// the object, or the object is dead / dying / removed (bind_objects.cpp).
inline constexpr int32_t kBlockAbility = 6;
bool galactic_block_finished(Call& c);  // c.self = the BlockingStatus

// OI follow-up t03/t04 (A1_READINGS §28.9): the object's movement lock (0x39ad40) as the bindings see it - a script
// Lock_Current_Orders, an ability's Disable_Movement share, or a running Divert move (bind_objects.cpp).
bool movement_locked(Call& c, objects::ObjectId id);

// --- P4 lua: FoC galactic special systems (bind_players.cpp; stubs without the players family) ---------------------
// GameObjectWrapper.Activate_Ability in a galactic script (Activate_Ability_Galactic 0x570670, P4_SIM_SYSTEMS 6.1): true
// when the VM's store is the galactic sim's (the trigger path ran; the caller returns a finished BlockingStatus), false
// otherwise (the caller continues with its tactical paths).
bool galactic_activate_ability(Call& c);
// L1P4_LUA_GAPS 1.1: the optional argument of the galactic Activate_Ability (GameObjectWrapper and GalacticTaskForce,
// bind_ai.cpp): 0 or 1 arguments, an object type given as a GameObjectTypeWrapper or a type name (case-insensitive).
// `filter` = that type (objects::kNone without an argument). False after a script error (logged, no unwind): more than
// one argument, an unknown type name, any other value (explicit nil included); the caller then fires nothing.
bool galactic_ability_type_arg(Call& c, const char* who, uint32_t& filter);
// GalacticSim::ground_activated_trigger(unit, planet row) for each pair still alive, in order (galactic scripts only).
// The task-force orders (Land_Units, GalacticTaskForce Activate_Ability) fire it in the AI host (engine/ai).
void galactic_trigger(Call& c, const std::vector<std::pair<objects::ObjectHandle, uint32_t>>& units);
// The campaign Must_Be_Bought_On_Black_Market gate of a unit ability (ABILITIES_HARDPOINTS 2.4, 0x39bbd0 =
// GalacticSim::bm_allows_unit_ability with special_name_crc(ability_name)); true without a galactic sim.
bool black_market_allows(Call& c, int32_t player, std::string_view ability_name, bool must_be_bought);
// --- end P4 lua ---

// a1g t06: Spawn_Unit at a planet in a galactic script (the VM's store is the galactic sim's, `location` is one of its
// planets): GalacticSim::spawn_unit, so the unit lives in the sim like a produced one (space units in the owner's orbit
// fleet, companies landed or in a new fleet, ground units on the surface, structures on the planet; one object per
// company: the members are the company's views, no member objects). Null when not handled (no galactic sim, not a
// planet, a type without a galactic role): the caller then creates the object directly.
objects::ObjectHandle galactic_spawn_unit(Call& c, uint32_t type, int32_t player, objects::ObjectHandle location);

// Store tag of a GameObjectWrapper's object (Ud::reserved, LuaVm::Impl::store_tag): 0 the VM's store, 1 the parent
// mode's store (L1/S4 wave 2b, Get_Parent_Mode_Object).
inline constexpr int32_t kParentStoreTag = 1;

// refix t04 (L1P4_LUA_GAPS 1.3): company member views. The original's galactic company is a transport object carrying
// one object per Company_Units entry; the port's is ONE object of the company type (campaign.hpp "Company transports").
// A member view is a GameObjectWrapper of that company object standing for its Company_Units entry `index`: Ud::reserved
// = store tag (low byte) | kMemberViewFlag | index << 16. Galactic Find_All_Objects_Of_Type returns views for the entries
// that pass its filters (refixf t03: member type, category / properties masks, owner), Find_First_Object for member type
// names; Get_Type gives the member type, Get_Parent_Object the company's plain wrapper (the
// original's transport), the galactic Activate_Ability does nothing (a unit inside a transport). Every other method acts
// on the company object [port]. Views are cached in the object cache under object_cache_key (negative keys).
inline constexpr int32_t kStoreTagMask = 0xff;
inline constexpr int32_t kMemberViewFlag = 0x100;
inline constexpr uint32_t kMaxMemberViews = 256;  // Company_Units entries past this get no view
inline int32_t store_tag_of(const Ud& ud) { return ud.reserved & kStoreTagMask; }
inline bool is_member_view(const Ud& ud) { return (ud.reserved & kMemberViewFlag) != 0; }
inline uint32_t member_index(const Ud& ud) { return static_cast<uint32_t>(ud.reserved) >> 16; }
// The object-cache key of a GameObjectWrapper: its id, or -(id * 256 + index + 1) for a member view.
lua_Number object_cache_key(const Ud& ud);
// Pushes the cached member view of company object `h`, Company_Units entry `index` (nil for a missing object).
void push_member_view(lua_State* L, LuaVm::Impl& vm, objects::ObjectHandle h, uint32_t index);
// A member view's member type (the company type's Company_Units[index]); objects::kNone for a plain wrapper.
uint32_t member_view_type(const LuaVm::Impl& vm, const Ud& ud);

void install_metatable(lua_State* L, LuaVm::Impl& vm);  // also sets global LuaWrapperMetaTable
void push_engine_userdata(lua_State* L, const Ud& ud);   // new userdata with the shared metatable
// with_script = false: only the binding-table globals (LuaVm::reinstall_engine_globals), not the `Script` object.
void install_globals(lua_State* L, LuaVm::Impl& vm, bool with_script = true);
// bind_engine.cpp: drops the GetEvent queue and the last event's parameters (LuaVm::pool_kill_threads).
void reset_event_queue(lua_State* L);
void install_math_library(lua_State* L, LuaVm::Impl& vm);  // math_lib.cpp

// ---- N1 save / load (vm_persist.cpp) ----
// C functions pushed at run time (not reachable from a fresh VM's globals / registry), by a stable name: a saved
// closure of one of them is restored by that name. Every family that pushes such a closure lists it here.
struct NamedCFunction {
    const char* name;
    lua_CFunction fn;
};
std::span<const NamedCFunction> runtime_cfunctions_bindings();      // bindings.cpp: bound method closures
std::span<const NamedCFunction> runtime_cfunctions_presentation();  // bind_presentation.cpp: block status tables

// The whole Lua heap of L (layout: vm_persist.cpp). vm_self = the light userdata the engine's closures carry (the
// VM's Impl). thread_index[i] = object index of threads[i] (coroutine states the engine holds). False + err when
// the state cannot be saved (w then holds nothing usable).
bool write_heap(lua_State* L, const void* vm_self, const std::vector<lua_State*>& threads,
                std::vector<uint32_t>& thread_index, ByteWriter& w, std::string& err);
// Parses and validates a heap written by write_heap (the rest of r: requires r.done()), then builds it as a NEW state
// (`out`); L_old is only read (registry attachments are taken from it). thread_states[i] = the state of object
// thread_index[i]. False + err on bad data; nothing is then allocated and L_old is untouched.
bool read_heap(ByteReader& r, lua_State* L_old, void* vm_self, lua_CFunction panic,
               const std::vector<uint32_t>& thread_index, std::vector<lua_State*>& thread_states, lua_State*& out,
               std::string& err);
struct HeapStats {
    uint32_t serial = 0;
    uint64_t bytes = 0, threshold = 0;
    uint32_t strings = 0;
};
HeapStats heap_stats(lua_State* L);
// The persistence name table's aliases ("name=saved name": names of a C function that is saved under another name,
// e.g. the shared dispatchers, or functions the linker folded together). Diagnostics / tests.
std::vector<std::string> cfunction_aliases();

// Fixed class ids used by the implementations.
uint32_t cls_game_object();
uint32_t cls_type();
uint32_t cls_player();
uint32_t cls_script();
uint32_t cls_user_var();

}  // namespace detail
}  // namespace alamo::lua

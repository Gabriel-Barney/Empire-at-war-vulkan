// Binding registry, class table, the shared wrapper metatable and the dispatcher.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>

#include "alamo/lua/object_bridge.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

namespace {

constexpr BindingInfo kTable[] = {
#include "binding_table.inc"
};

constexpr std::string_view kHostValues[] = {"AITarget",       "FreeStore", "LastService", "LastUnitService",
                                            "Object",         "PlanDefinitionLoad", "PlayerObject", "Target"};

// L1 final: class-less globals that are objects with methods get a port class (their methods are extras).
constexpr std::pair<std::string_view, const char*> kPortGlobalClasses[] = {{"GlobalValue", "GlobalValueClass"}};

// Wrapper classes that derive from another wrapper class (methods are inherited).
constexpr std::pair<std::string_view, std::string_view> kParents[] = {
    {"GalacticTaskForceClass", "TaskForceClass"},
    {"LandTaskForceClass", "TaskForceClass"},
    {"SpaceTaskForceClass", "TaskForceClass"},
};

constexpr std::string_view kFixedClasses[] = {"GameObjectTypeWrapper", "GameObjectWrapper", "LuaScriptClass",
                                              "LuaUserVar", "PlayerWrapper"};

// q6 t11 (LUA_API.md "Mode gates"): globals that are a script error outside their mode. The bindings that test it
// themselves with their own text (Find_Nearest, Create_Cinematic_Transport, Spawn_Special_Weapon, ...) are listed too:
// the dispatcher's test runs first.
constexpr std::string_view kTacticalGlobals[] = {
    "Allow_Localized_SFX",      "Are_On_Opposite_Sides_Of_Shield", "Camera_To_Follow",
    "Create_Cinematic_Transport", "Enable_Distance_Fog",           "Enable_Fog",
    "Find_Hint",                "Find_Nearest",                    "Find_Nearest_Space_Field",
    "Get_Most_Defended_Position", "Hide_Object",                   "Hide_Sub_Object",
    "Point_Camera_At",          "Project_By_Unit_Range",           "Promote_To_Space_Cinematic_Layer",
    "Reinforce_Unit",           "Rotate_Camera_By",                "Rotate_Camera_To",
    "Scroll_Camera_To",         "Set_Cinematic_Environment",       "Set_New_Environment",
    "Spawn_Special_Weapon",     "Start_Cinematic_Space_Retreat",   "Weather_Audio_Pause",
    "Zoom_Camera"};
constexpr std::string_view kGalacticGlobals[] = {"Find_Path"};
using detail::DeadRule;
using detail::ModeGate;
// Methods with a mode or object-kind gate the dispatcher tests ("Class.Method"). The galactic-object / planet / fleet
// gates of Get_Planet_Location, Is_Planet_Destroyed, Get_Next_Starbase_Type and Invade need the object, so a dead wrapper
// cannot fail them: Get_Planet_Location is gated on the mode here, the others in their bindings. Attack_Target tests its
// own gate (bind_objects.cpp).
constexpr std::pair<std::string_view, ModeGate> kMethodGates[] = {
    {"GameObjectWrapper.Get_Energy", ModeGate::Tactical},     {"GameObjectWrapper.Get_Hull", ModeGate::Tactical},
    {"GameObjectWrapper.Get_Shield", ModeGate::Tactical},     {"GameObjectWrapper.Hyperspace_Away", ModeGate::Tactical},
    {"GameObjectWrapper.Make_Invulnerable", ModeGate::Tactical}, {"GameObjectWrapper.Stop", ModeGate::Tactical},
    {"GameObjectWrapper.Teleport", ModeGate::Tactical},       {"GameObjectWrapper.Teleport_And_Face", ModeGate::Tactical},
    {"GameObjectWrapper.Get_Planet_Location", ModeGate::Galactic},
    {"PlayerWrapper.Get_Space_Station", ModeGate::Tactical},  {"TaskForceClass.Get_AI_Power_Vs_Unit", ModeGate::Tactical},
};
// Gated bindings that carry their own recovered text for the mode error (their implementations test the finer mode -
// space / land - themselves with the same text); the dispatcher's gate uses it instead of the generic "this command is
// only supported in ..." text. Keyed by the global or method name.
constexpr std::pair<std::string_view, const char*> kGateTexts[] = {
    {"Find_Nearest", "Find_Nearest - this function may only be used in tactical modes."},
    {"Find_Nearest_Space_Field", "Find_Nearest_Space_Field -- may only be used in space mode"},
    {"Are_On_Opposite_Sides_Of_Shield", "Are_On_Opposite_Sides_Of_Shield -- only valid in land mode"},
    {"Spawn_Special_Weapon", "Spawn_Special_Weapon -- only available in space tactical mode"},
    {"Create_Cinematic_Transport", "Create_Cinematic_Transport -- only available in land tactical mode"},
    {"Start_Cinematic_Space_Retreat", "Start_Cinematic_Space_Retreat -- only available in space tactical mode"},
    {"Get_Energy", "GameObjectWrapper::Get_Energy -- only valid in tactical mode"},
    {"Hyperspace_Away", "GameObjectWrapper::Hyperspace_Away -- only valid in space mode"},
    {"Get_AI_Power_Vs_Unit", "Get_AI_Power_Vs_Unit -- only valid in tactical mode"},
};
// LUA_API.md "Dead or null wrappers": GameObjectWrapper methods whose dead wrapper is not the silent default.
constexpr std::pair<std::string_view, DeadRule> kDeadRules[] = {
    // the implementation tests it
    {"Is_Valid", DeadRule::Binding}, {"Is_Pool_Safe", DeadRule::Binding}, {"Service_Wrapper", DeadRule::Binding},
    // q6 t03 unit orders (Attack_Target returns nothing, Move_Internal reports a script error after its mode gate)
    {"Attack_Target", DeadRule::Binding}, {"Move_To", DeadRule::Binding}, {"Attack_Move", DeadRule::Binding},
    {"Guard_Target", DeadRule::Binding},
    // q6 t12 (OBJECT_SCRIPTS §4.3): the proximity events do nothing on a dead object
    {"Event_Object_In_Range", DeadRule::Binding}, {"Cancel_Event_Object_In_Range", DeadRule::Binding},
    // error "already dead"
    {"Are_Engines_Online", DeadRule::Error}, {"Contains_Hero", DeadRule::Error}, {"Fire_Special_Weapon", DeadRule::Error},
    {"Get_Build_Pad_Contents", DeadRule::Error}, {"Get_Contained_Heroes", DeadRule::Error}, {"Get_Distance", DeadRule::Error},
    {"Get_Starbase_Level", DeadRule::Error}, {"Sell", DeadRule::Error},
    // warning "this object is dead!"
    {"Change_Owner", DeadRule::WarnNothing}, {"Despawn", DeadRule::WarnNothing},
    {"Mark_Parent_Mode_Object_For_Death", DeadRule::WarnNothing}, {"Set_Importance", DeadRule::WarnNothing},
    {"Set_Selectable", DeadRule::WarnNothing}, {"Take_Damage", DeadRule::WarnNothing},
    {"Set_Targeting_Priorities", DeadRule::WarnNothing}, {"Set_Targeting_Stickiness_Time_Threshold", DeadRule::WarnNothing},
    {"Get_Rate_Of_Damage_Taken", DeadRule::WarnZero}, {"Get_Time_Till_Dead", DeadRule::WarnZero},
};

template <class List>
bool listed(const List& list, std::string_view name) {
    return std::find(std::begin(list), std::end(list), name) != std::end(list);
}

struct Registry {
    std::vector<std::string> classes;  // sorted
    std::vector<uint32_t> parent;
    std::vector<detail::MethodSlot> slots;
    std::vector<std::map<std::string, uint32_t, std::less<>>> methods_by_class;  // lookup only
    std::vector<detail::ImplFn> global_fn;  // per table row (globals only)
    std::vector<detail::ModeGate> global_gate;  // per table row (globals only)
    std::map<std::string, uint32_t, std::less<>> global_rows;
    uint32_t user_var = detail::kNoClass;

    Registry() {
        std::vector<std::string> names(std::begin(kFixedClasses), std::end(kFixedClasses));
        for (const BindingInfo& b : kTable)
            if (*b.cls) names.emplace_back(b.cls);
        for (const detail::ImplEntry& e : detail::implementations())
            if (*e.cls) names.emplace_back(e.cls);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end()), names.end());
        classes = names;
        user_var = id("LuaUserVar");
        parent.assign(classes.size(), user_var);
        parent[user_var] = detail::kNoClass;
        for (const auto& [c, p] : kParents)
            if (id(c) != detail::kNoClass) parent[id(c)] = id(p);
        methods_by_class.resize(classes.size());

        global_fn.assign(std::size(kTable), nullptr);
        global_gate.assign(std::size(kTable), detail::ModeGate::None);
        for (uint32_t row = 0; row < std::size(kTable); ++row) {
            const BindingInfo& b = kTable[row];
            if (b.kind == BindingKind::Global) {
                global_rows.emplace(b.name, row);
                global_fn[row] = find_impl(BindingKind::Global, "", b.name);
                if (listed(kTacticalGlobals, b.name)) global_gate[row] = detail::ModeGate::Tactical;
                if (listed(kGalacticGlobals, b.name)) global_gate[row] = detail::ModeGate::Galactic;
            } else {
                add_slot(row, id(b.cls), b.name, find_impl(BindingKind::Method, b.cls, b.name));
            }
        }
        for (const detail::ImplEntry& e : detail::implementations()) {
            if (e.kind != BindingKind::Method) continue;
            const uint32_t c = id(e.cls);
            if (methods_by_class[c].count(e.name)) continue;
            add_slot(detail::kNoBinding, c, e.name, e.fn);
        }
    }
    uint32_t id(std::string_view c) const {
        const auto it = std::lower_bound(classes.begin(), classes.end(), c);
        return (it != classes.end() && *it == c) ? static_cast<uint32_t>(it - classes.begin()) : detail::kNoClass;
    }
    static detail::ImplFn find_impl(BindingKind kind, std::string_view cls, std::string_view name) {
        for (const detail::ImplEntry& e : detail::implementations())
            if (e.kind == kind && cls == e.cls && name == e.name) return e.fn;
        return nullptr;
    }
    void add_slot(uint32_t row, uint32_t cls, std::string_view name, detail::ImplFn fn) {
        detail::MethodSlot s;
        s.row = row;
        s.cls = cls;
        s.fn = fn;
        s.label = classes[cls] + "." + std::string(name);
        for (const auto& [label, gate] : kMethodGates)
            if (label == s.label) s.gate = gate;
        if (classes[cls] == "GameObjectWrapper")
            for (const auto& [method, rule] : kDeadRules)
                if (method == name) s.dead = rule;
        methods_by_class[cls].emplace(std::string(name), static_cast<uint32_t>(slots.size()));
        slots.push_back(std::move(s));
    }
};

const Registry& reg() {
    static const Registry r;
    return r;
}

}  // namespace

std::span<const BindingInfo> binding_table() { return kTable; }

bool is_host_value_global(std::string_view name) {
    return std::find(std::begin(kHostValues), std::end(kHostValues), name) != std::end(kHostValues);
}

bool binding_implemented(const BindingInfo& b) {
    const Registry& r = reg();
    if (b.kind == BindingKind::Global) {
        const auto it = r.global_rows.find(std::string_view(b.name));
        return it != r.global_rows.end() && r.global_fn[it->second] != nullptr;
    }
    const uint32_t s = detail::find_method(r.id(b.cls), b.name);
    return s != detail::kNoBinding && r.slots[s].fn != nullptr && r.slots[s].cls == r.id(b.cls);
}

BindingCoverage binding_coverage() {
    BindingCoverage c;
    for (const BindingInfo& b : kTable) {
        c.calls_total += b.calls;
        const bool host = b.kind == BindingKind::Global && is_host_value_global(b.name);
        const bool impl = !host && binding_implemented(b);
        if (b.kind == BindingKind::Global) {
            ++c.globals;
            if (host) ++c.host_values;
            if (impl) ++c.globals_implemented;
        } else {
            ++c.methods;
            if (impl) ++c.methods_implemented;
        }
        if (impl) c.calls_implemented += b.calls;
    }
    for (const detail::MethodSlot& s : reg().slots)
        if (s.row == detail::kNoBinding) ++c.extra_methods;
    return c;
}

std::string binding_coverage_report() {
    const BindingCoverage c = binding_coverage();
    std::string out;
    char line[512];
    std::snprintf(line, sizeof line,
                  "Lua binding coverage (docs/re/lua_api.json)\n"
                  "globals %u: implemented %u, host values %u, unbound %u\n"
                  "methods %u: implemented %u, unbound %u (+%u implemented methods missing from the RE table)\n"
                  "RE call sites covered: %llu of %llu\n\n",
                  c.globals, c.globals_implemented, c.host_values, c.globals - c.globals_implemented - c.host_values,
                  c.methods, c.methods_implemented, c.methods - c.methods_implemented, c.extra_methods,
                  static_cast<unsigned long long>(c.calls_implemented), static_cast<unsigned long long>(c.calls_total));
    out += line;
    for (const BindingInfo& b : kTable) {
        const char* status = (b.kind == BindingKind::Global && is_host_value_global(b.name)) ? "host-value"
                             : binding_implemented(b)                                          ? "implemented"
                                                                                               : "unbound";
        if (b.kind == BindingKind::Global)
            std::snprintf(line, sizeof line, "global %-12s %-40s calls %5u  class %s\n", status, b.name, b.calls,
                          *b.cls ? b.cls : "-");
        else
            std::snprintf(line, sizeof line, "method %-12s %s.%s  calls %u\n", status, b.cls, b.name, b.calls);
        out += line;
    }
    for (const detail::MethodSlot& s : reg().slots)
        if (s.row == detail::kNoBinding) out += "method extra        " + s.label + "\n";
    return out;
}

namespace detail {

const std::vector<std::string>& class_names() { return reg().classes; }
uint32_t class_id(std::string_view cls) { return reg().id(cls); }
uint32_t parent_class(uint32_t cls) { return cls < reg().parent.size() ? reg().parent[cls] : kNoClass; }
const std::vector<MethodSlot>& method_slots() { return reg().slots; }

uint32_t find_method(uint32_t cls, std::string_view name) {
    const Registry& r = reg();
    for (uint32_t c = cls; c != kNoClass && c < r.classes.size(); c = r.parent[c]) {
        const auto it = r.methods_by_class[c].find(name);
        if (it != r.methods_by_class[c].end()) return it->second;
    }
    return kNoBinding;
}

ImplFn global_impl(uint32_t row) { return row < reg().global_fn.size() ? reg().global_fn[row] : nullptr; }
ModeGate global_gate(uint32_t row) { return row < reg().global_gate.size() ? reg().global_gate[row] : ModeGate::None; }

uint32_t global_row(std::string_view name) {
    const auto it = reg().global_rows.find(name);
    return it == reg().global_rows.end() ? kNoBinding : it->second;
}

uint32_t cls_game_object() {
    static const uint32_t c = class_id("GameObjectWrapper");
    return c;
}
uint32_t cls_type() {
    static const uint32_t c = class_id("GameObjectTypeWrapper");
    return c;
}
uint32_t cls_player() {
    static const uint32_t c = class_id("PlayerWrapper");
    return c;
}
uint32_t cls_script() {
    static const uint32_t c = class_id("LuaScriptClass");
    return c;
}
uint32_t cls_user_var() {
    static const uint32_t c = class_id("LuaUserVar");
    return c;
}
uint32_t cls_position() {
    static const uint32_t c = class_id("PositionWrapper");
    return c;
}
uint32_t cls_target() {
    static const uint32_t c = class_id("AITargetLocationWrapper");
    return c;
}

int32_t to_int(double d) {
    if (!(d == d)) return 0;
    if (d >= 2147483647.0) return 2147483647;
    if (d <= -2147483648.0) return -2147483647 - 1;
    return static_cast<int32_t>(d);
}

int Call::script_error_stop(const char* fmt, ...) {
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vm->message(std::string("SCRIPT ERROR: ") + buf);
    vm->exit_requested = true;
    return 0;
}

int Call::script_warning(const char* fmt, ...) {
    char buf[640];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    vm->message(std::string("SCRIPT WARNING: ") + buf);
    return 0;
}

const Ud* Call::userdata(int i, uint32_t cls) const {
    if (!has(i) || lua_type(L, idx(i)) != LUA_TUSERDATA) return nullptr;
    const auto* ud = static_cast<const Ud*>(lua_touserdata(L, idx(i)));
    return (ud && ud->cls == cls) ? ud : nullptr;
}

objects::ObjectHandle object_handle(const Ud& ud) { return objects::ObjectHandle{ud.value}; }

// L1/S4 wave 2b: a wrapper of another store (Ud::reserved = its store tag, LuaVm::Impl::store_tag) is not alive for
// the store the binding runs on: its handle means nothing there.
bool object_alive(const LuaVm::Impl& vm, const Ud& ud) {
    const objects::ObjectStore* st = vm.host.store;
    const objects::ObjectHandle h = object_handle(ud);
    return store_tag_of(ud) == vm.store_tag && st && h && st->alive(h) && st->id(h) == ud.id;
}

namespace {

// The parent mode's store (Get_Parent_Mode_Object wrappers): the attached ObjectBridge's parent_store.
objects::ObjectStore* parent_store_of(LuaVm::Impl& vm) {
    const ObjectBridge* b = vm.owner ? attached_object_bridge(*vm.owner) : nullptr;
    return b ? b->parent_store : nullptr;
}

// The store a game-object wrapper belongs to, from outside a binding (tostring).
const objects::ObjectStore* wrapper_store(LuaVm::Impl& vm, const Ud& ud) {
    if (store_tag_of(ud) == vm.store_tag) return vm.host.store;
    return store_tag_of(ud) == kParentStoreTag ? parent_store_of(vm) : nullptr;
}

bool alive_in(const objects::ObjectStore* st, const Ud& ud) {
    const objects::ObjectHandle h = object_handle(ud);
    return st && h && st->alive(h) && st->id(h) == ud.id;
}

const char kMetaKey[] = "alamo.meta";
const char kObjCacheKey[] = "alamo.objcache";
const char kObjCacheParentKey[] = "alamo.objcache.parent";  // wave 2b: parent-mode wrappers (Get_Parent_Mode_Object)
const char kTypeCacheKey[] = "alamo.typecache";
const char kWrapCacheKey[] = "alamo.wrapcache";  // class id -> (key -> wrapper): players, plots, events, ...

LuaVm::Impl* vm_of(lua_State* L) { return static_cast<LuaVm::Impl*>(lua_touserdata(L, lua_upvalueindex(1))); }

// The only place errors are raised: no C++ object with a destructor is alive here.
int raise(lua_State* L, const char* msg) {
    luaL_where(L, 1);
    lua_pushstring(L, msg);
    lua_concat(L, 2);
    return lua_error(L);
}

struct Invocation {
    LuaVm::Impl* vm;
    const Ud* self;
    ImplFn fn;
    uint32_t counter;   // unbound counter slot: the binding row (kNoBinding = none)
    const char* label;  // binding name for messages
    ModeGate gate;      // q6 t11: tested first
    bool object;        // a GameObjectWrapper method: `dead` applies
    DeadRule dead;
    const char* method; // method name (dead / gate messages)
    int first;
    int nargs;
};

// The mode the gate tests: the VM's store (the parent store inside a parent-mode wrapper's method). No store: no gate.
bool gate_fails(const LuaVm::Impl& vm, ModeGate gate) {
    const objects::ObjectStore* st = vm.host.store;
    if (gate == ModeGate::None || !st) return false;
    const bool galactic = st->mode() == objects::ModeKind::Galactic;
    return gate == ModeGate::Tactical ? galactic : !galactic;
}

// L1/S4 wave 2b: a method of a parent-mode wrapper runs with host.store = the parent mode's store (restored after).
struct StoreSwap {
    LuaVm::Impl* vm;
    objects::ObjectStore* store;
    int32_t tag;
    StoreSwap(LuaVm::Impl* v, const Ud* self) : vm(v), store(v->host.store), tag(v->store_tag) {
        if (self && self->cls == cls_game_object() && store_tag_of(*self) == kParentStoreTag && v->store_tag != kParentStoreTag) {
            v->host.store = parent_store_of(*v);
            v->store_tag = kParentStoreTag;
        }
    }
    ~StoreSwap() {
        vm->host.store = store;
        vm->store_tag = tag;
    }
    StoreSwap(const StoreSwap&) = delete;
    StoreSwap& operator=(const StoreSwap&) = delete;
};

// q6 t11 (LUA_API.md "Binding error model across all bindings"): the mode gate (a script error), then a dead
// GameObjectWrapper's rule, then the implementation. Only the port's own failures return -1 (raised by the caller): an
// unbound binding or a C++ exception; `err` then holds the message.
int invoke(lua_State* L, const Invocation& inv, char* err, size_t cap) noexcept {
    const StoreSwap swap(inv.vm, inv.self);
    Call c{L, inv.vm, inv.self, inv.first, inv.nargs};
    if (gate_fails(*inv.vm, inv.gate)) {
        for (const auto& [name, text] : kGateTexts)
            if (name == inv.method) return c.script_error_stop("%s", text);
        const char* who = inv.object ? "GameObjectWrapper::" : "";
        return c.script_error_stop(inv.gate == ModeGate::Tactical ? "%s%s -- this command is only supported in tactical modes"
                                                                   : "%s%s -- this command is only supported in galactic mode",
                                   who, inv.method);
    }
    if (inv.object && inv.dead != DeadRule::Binding && !object_alive(*inv.vm, *inv.self)) {
        switch (inv.dead) {
            case DeadRule::Error:
                return c.script_error_stop("GameObjectWrapper::%s -- this object is already dead", inv.method);
            case DeadRule::WarnNothing:
                return c.script_warning("GameObjectWrapper::%s -- this object is dead!", inv.method);
            case DeadRule::WarnZero:
                c.script_warning("GameObjectWrapper::%s -- this object is dead!", inv.method);
                lua_pushnumber(L, 0);
                return 1;
            default:
                return 0;  // silent: nothing
        }
    }
    if (!inv.fn) {
        if (inv.counter < inv.vm->unbound.size()) ++inv.vm->unbound[inv.counter];
        ++inv.vm->unbound_total;
        std::snprintf(err, cap, "unbound: %s", inv.label);
        return -1;
    }
    try {
        return inv.fn(c);
    } catch (const std::exception& e) {
        std::snprintf(err, cap, "%s: %s", inv.label, e.what());
    } catch (...) {
        std::snprintf(err, cap, "%s: C++ exception", inv.label);
    }
    return -1;
}

// __call(ud, ...): engine globals.
int mt_call(lua_State* L) {
    char err[640];
    err[0] = 0;
    LuaVm::Impl* vm = vm_of(L);
    const auto* ud = static_cast<const Ud*>(lua_touserdata(L, 1));
    if (!ud || ud->binding == kNoBinding) return raise(L, "this engine object is not callable");
    const char* name = binding_table()[ud->binding].name;
    const Invocation inv{vm,   ud,    global_impl(ud->binding), ud->binding, name, global_gate(ud->binding),
                         false, DeadRule::Silent, name, 2, lua_gettop(L) - 1};
    const int n = invoke(L, inv, err, sizeof err);
    if (n < 0) return raise(L, err);
    return n;
}

// Bound method closure: upvalues (vm, receiver, slot).
int method_call(lua_State* L) {
    char err[640];
    err[0] = 0;
    LuaVm::Impl* vm = vm_of(L);
    const auto* ud = static_cast<const Ud*>(lua_touserdata(L, lua_upvalueindex(2)));
    const uint32_t slot = static_cast<uint32_t>(lua_tonumber(L, lua_upvalueindex(3)));
    const MethodSlot& s = method_slots()[slot];
    const char* name = s.label.c_str() + class_names()[s.cls].size() + 1;
    // q6 t11: the gate and the dead-wrapper rule of the slot (kMethodGates / kDeadRules above). The receiver's class
    // decides whether the dead rule applies (a GameObjectWrapper); the slot's class only names the rule.
    const bool object = ud->cls == cls_game_object();
    // An extra (implemented-only) method is never unbound, so it has no counter.
    const Invocation inv{vm, ud, s.fn, s.row, s.label.c_str(), s.gate, object, s.dead, name, 1, lua_gettop(L)};
    const int n = invoke(L, inv, err, sizeof err);
    if (n < 0) return raise(L, err);
    return n;
}

// __index(ud, key): a closure bound to the receiver, or nil.
int mt_index(lua_State* L) {
    const auto* ud = static_cast<const Ud*>(lua_touserdata(L, 1));
    if (!ud || lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    const uint32_t slot = find_method(ud->cls, std::string_view(lua_tostring(L, 2), lua_strlen(L, 2)));
    if (slot == kNoBinding) {
        lua_pushnil(L);
        return 1;
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_pushvalue(L, 1);
    lua_pushnumber(L, static_cast<lua_Number>(slot));
    lua_pushcclosure(L, method_call, 3);
    return 1;
}

int mt_eq(lua_State* L) {
    const auto* a = static_cast<const Ud*>(lua_touserdata(L, 1));
    const auto* b = static_cast<const Ud*>(lua_touserdata(L, 2));
    lua_pushboolean(L, a && b && a->cls == b->cls && a->binding == b->binding && a->value == b->value &&
                           a->id == b->id && a->reserved == b->reserved);
    return 1;
}

int mt_tostring(lua_State* L) {
    char buf[320];
    LuaVm::Impl* vm = vm_of(L);
    const auto* ud = static_cast<const Ud*>(lua_touserdata(L, 1));
    if (!ud) {
        lua_pushstring(L, "userdata");
        return 1;
    }
    const auto* types = vm->host.types;
    if (ud->cls == cls_game_object()) {
        const objects::ObjectStore* st = wrapper_store(*vm, *ud);
        const bool alive = alive_in(st, *ud);
        std::string_view type_name = "?";
        if (alive && types) {
            const uint32_t mt = member_view_type(*vm, *ud);  // refix t04: a member view prints its member type
            const objects::TypeIndex t = mt != objects::kNone ? mt : st->type(object_handle(*ud));
            if (t < types->raw().type_count()) type_name = types->raw().types().name[t];
        }
        std::snprintf(buf, sizeof buf, "GameObject %.*s (id %d)%s", static_cast<int>(type_name.size()),
                      type_name.data(), ud->id, alive ? "" : " [dead]");
    } else if (ud->cls == cls_type()) {
        std::string_view n = "?";
        if (types && ud->value < types->raw().type_count()) n = types->raw().types().name[ud->value];
        std::snprintf(buf, sizeof buf, "GameObjectType %.*s", static_cast<int>(n.size()), n.data());
    } else if (ud->cls == cls_player()) {
        std::snprintf(buf, sizeof buf, "Player %d", ud->id);
    } else if (ud->cls == cls_script()) {
        std::snprintf(buf, sizeof buf, "%s", vm->options.script_name.c_str());
    } else if (ud->binding != kNoBinding) {
        std::snprintf(buf, sizeof buf, "%s", binding_table()[ud->binding].name);
    } else {
        std::snprintf(buf, sizeof buf, "%s", class_names()[ud->cls].c_str());
    }
    lua_pushstring(L, buf);
    return 1;
}

void push_registry_table(lua_State* L, const char* key) {
    lua_pushstring(L, key);
    lua_rawget(L, LUA_REGISTRYINDEX);
}

}  // namespace

void install_metatable(lua_State* L, LuaVm::Impl& vm) {
    lua_newtable(L);
    const std::pair<const char*, lua_CFunction> fns[] = {
        {"__index", mt_index}, {"__call", mt_call}, {"__eq", mt_eq}, {"__tostring", mt_tostring}};
    for (const auto& [name, fn] : fns) {
        lua_pushstring(L, name);
        lua_pushlightuserdata(L, &vm);
        lua_pushcclosure(L, fn, 1);
        lua_rawset(L, -3);
    }
    lua_pushstring(L, kMetaKey);
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_REGISTRYINDEX);
    // PGBase's Flush_G keeps a global table of this name (its wrapper metatable).
    lua_pushstring(L, "LuaWrapperMetaTable");
    lua_pushvalue(L, -2);
    lua_rawset(L, LUA_GLOBALSINDEX);
    lua_pop(L, 1);
    for (const char* key : {kObjCacheKey, kObjCacheParentKey, kTypeCacheKey, kWrapCacheKey}) {
        lua_pushstring(L, key);
        lua_newtable(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
}

void push_engine_userdata(lua_State* L, const Ud& ud) {
    void* mem = lua_newuserdata(L, sizeof(Ud));
    std::memcpy(mem, &ud, sizeof(Ud));
    push_registry_table(L, kMetaKey);
    lua_setmetatable(L, -2);
}

void install_globals(lua_State* L, LuaVm::Impl& /*vm*/, bool with_script) {
    const auto table = binding_table();
    for (uint32_t row = 0; row < table.size(); ++row) {
        const BindingInfo& b = table[row];
        if (b.kind != BindingKind::Global || is_host_value_global(b.name)) continue;
        Ud ud;
        ud.cls = *b.cls ? class_id(b.cls) : cls_user_var();
        for (const auto& [name, cls] : kPortGlobalClasses)
            if (name == b.name && class_id(cls) != kNoClass) ud.cls = class_id(cls);
        ud.binding = row;
        lua_pushstring(L, b.name);
        push_engine_userdata(L, ud);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
    if (!with_script) return;
    Ud script;
    script.cls = cls_script();
    lua_pushstring(L, "Script");
    push_engine_userdata(L, script);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

namespace {

// Pushes the cached wrapper for `key` in the cache table on top of the stack (which is replaced by
// the wrapper), creating it from `ud` when absent or stale (same key, different payload).
void push_cached_in_top(lua_State* L, lua_Number key, const Ud& ud) {
    lua_pushnumber(L, key);
    lua_rawget(L, -2);
    if (lua_type(L, -1) == LUA_TUSERDATA) {
        const auto* old = static_cast<const Ud*>(lua_touserdata(L, -1));
        if (old->cls == ud.cls && old->binding == ud.binding && old->value == ud.value && old->id == ud.id) {
            lua_remove(L, -2);
            return;
        }
    }
    lua_pop(L, 1);
    push_engine_userdata(L, ud);
    lua_pushnumber(L, key);
    lua_pushvalue(L, -2);
    lua_rawset(L, -4);
    lua_remove(L, -2);
}

void push_cached(lua_State* L, const char* cache_key, lua_Number key, const Ud& ud) {
    push_registry_table(L, cache_key);
    push_cached_in_top(L, key, ud);
}

}  // namespace

void push_cached_userdata(lua_State* L, const Ud& ud, lua_Number key) {
    push_registry_table(L, kWrapCacheKey);
    lua_rawgeti(L, -1, static_cast<int>(ud.cls));
    if (lua_type(L, -1) != LUA_TTABLE) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_rawseti(L, -3, static_cast<int>(ud.cls));
    }
    lua_remove(L, -2);
    push_cached_in_top(L, key, ud);
}

void push_object(lua_State* L, LuaVm::Impl& vm, objects::ObjectHandle h) {
    const objects::ObjectStore* st = vm.host.store;
    if (!st || !h || !st->exists(h)) {
        lua_pushnil(L);
        return;
    }
    Ud ud;
    ud.cls = cls_game_object();
    ud.value = h.value;
    ud.id = st->id(h);
    ud.reserved = vm.store_tag;  // wave 2b: inside a parent-mode wrapper's method, objects of the parent store
    push_cached(L, vm.store_tag == kParentStoreTag ? kObjCacheParentKey : kObjCacheKey, static_cast<lua_Number>(ud.id), ud);
}

// Wave 2b: the wrapper of an object of the parent mode's store (Get_Parent_Mode_Object).
void push_parent_object(lua_State* L, LuaVm::Impl& vm, objects::ObjectHandle h) {
    const objects::ObjectStore* st = parent_store_of(vm);
    if (!st || !h || !st->exists(h)) {
        lua_pushnil(L);
        return;
    }
    Ud ud;
    ud.cls = cls_game_object();
    ud.value = h.value;
    ud.id = st->id(h);
    ud.reserved = kParentStoreTag;
    push_cached(L, kObjCacheParentKey, static_cast<lua_Number>(ud.id), ud);
}
objects::ObjectStore* parent_store(LuaVm::Impl& vm) { return parent_store_of(vm); }

// refix t04: company member views (vm_internal.hpp).
lua_Number object_cache_key(const Ud& ud) {
    if (!is_member_view(ud)) return static_cast<lua_Number>(ud.id);
    return -(static_cast<lua_Number>(ud.id) * static_cast<lua_Number>(kMaxMemberViews) + static_cast<lua_Number>(member_index(ud)) + 1.0);
}

void push_member_view(lua_State* L, LuaVm::Impl& vm, objects::ObjectHandle h, uint32_t index) {
    const objects::ObjectStore* st = vm.host.store;
    if (!st || !h || !st->exists(h) || index >= kMaxMemberViews) {
        lua_pushnil(L);
        return;
    }
    Ud ud;
    ud.cls = cls_game_object();
    ud.value = h.value;
    ud.id = st->id(h);
    ud.reserved = vm.store_tag | kMemberViewFlag | static_cast<int32_t>(index << 16);
    push_cached(L, vm.store_tag == kParentStoreTag ? kObjCacheParentKey : kObjCacheKey, object_cache_key(ud), ud);
}

uint32_t member_view_type(const LuaVm::Impl& vm, const Ud& ud) {
    if (ud.cls != cls_game_object() || !is_member_view(ud) || store_tag_of(ud) != vm.store_tag) return objects::kNone;
    const objects::ObjectStore* st = vm.host.store;
    const objects::GameObjectTypes* types = vm.host.types;
    const objects::ObjectHandle h = object_handle(ud);
    if (!st || !types || !st->exists(h)) return objects::kNone;
    const std::vector<objects::TypeIndex> members = types->get_type_refs(st->type(h), "Company_Units");
    const uint32_t i = member_index(ud);
    return i < members.size() ? members[i] : objects::kNone;
}

void push_type(lua_State* L, LuaVm::Impl& vm, uint32_t type_index) {
    if (!vm.host.types || type_index >= vm.host.types->raw().type_count()) {
        lua_pushnil(L);
        return;
    }
    Ud ud;
    ud.cls = cls_type();
    ud.value = type_index;
    push_cached(L, kTypeCacheKey, static_cast<lua_Number>(type_index), ud);
}

// Players live for the whole session: one wrapper per player id, kept for the VM's lifetime, so
// script tables keyed by players (PGCommands' AttackedTable-style idioms) hit.
void push_player(lua_State* L, LuaVm::Impl& /*vm*/, int32_t player_id) {
    Ud ud;
    ud.cls = cls_player();
    ud.id = player_id;
    push_cached_userdata(L, ud, static_cast<lua_Number>(player_id));
}

// N1: the bound method closures mt_index pushes (a script can keep one, e.g. local f = obj.Get_Position).
std::span<const NamedCFunction> runtime_cfunctions_bindings() {
    static const NamedCFunction fns[] = {{"~method_call", method_call}};
    return fns;
}

}  // namespace detail

void LuaVm::reinstall_engine_globals() {
    check_owner();
    detail::install_globals(impl_->L, *impl_, false);
    lua_settop(impl_->L, 0);
}

void LuaVm::rebind_host(const LuaHost& host_in) {
    check_owner();
    const bool moved = impl_->host.store != host_in.store;
    impl_->host = host_in;
    if (!moved) return;
    lua_State* L = impl_->L;
    for (const char* key : {detail::kObjCacheKey, detail::kObjCacheParentKey}) {
        lua_pushstring(L, key);
        lua_newtable(L);
        lua_rawset(L, LUA_REGISTRYINDEX);
    }
}

}  // namespace alamo::lua

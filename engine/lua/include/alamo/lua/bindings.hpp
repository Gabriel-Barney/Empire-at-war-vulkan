// Lua binding registry (roadmap L1 stage 1). The table of every binding the original registers is
// generated from docs/re/lua_api.json (engine/lua/tools/gen_bindings.py -> src/binding_table.inc):
// 143 globals + 323 wrapper methods.
//
// How the original exposes them (docs/re/LUA_API.md, "How the engine registers Lua bindings"), and
// how the port mirrors it:
//   * every engine global is a userdata object (one C++ LuaUserVar subclass per function); calling it
//     invokes its call operator. Here: a full userdata whose shared metatable (the global
//     LuaWrapperMetaTable) has __call / __index / __eq / __tostring.
//   * methods are looked up on the userdata by name and called with DOT syntax and no self
//     (`unit.Get_Type()`, `GameRandom.Get_Float()`): __index returns a closure bound to the object.
//     Lookup walks the class, its parent (Land/Space/GalacticTaskForceClass -> TaskForceClass), then
//     LuaUserVar (Is_Pool_Safe, which PGBase's Flush_G calls on every userdata global).
//   * a few globals are per-script VALUES the host sets (Object, Target, AITarget, PlayerObject,
//     FreeStore, LastService, LastUnitService, PlanDefinitionLoad): nil until set.
// Every other binding is registered. Those not implemented yet raise the Lua error
// "unbound: <name>" ("unbound: <Class>.<Method>" for methods) and are counted per name (LuaVm).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace alamo::lua {

enum class BindingKind : uint8_t { Global, Method };

struct BindingInfo {
    BindingKind kind;
    const char* cls;   // Method: owner wrapper class. Global: implementing class ("" if unknown).
    const char* name;  // Lua name
    uint32_t calls;    // call sites in the RE corpora (vanilla FoC + TR 3.3), for prioritising
};

// The generated table: globals sorted by name, then methods sorted by (class, name).
std::span<const BindingInfo> binding_table();

// Host-set value globals (see above).
bool is_host_value_global(std::string_view name);
// True when the port implements this binding (the rest are "unbound" stubs).
bool binding_implemented(const BindingInfo& b);

struct BindingCoverage {
    uint32_t globals = 0, methods = 0;                          // rows in the table (143 / 323)
    uint32_t host_values = 0;                                   // globals set by the host
    uint32_t globals_implemented = 0, methods_implemented = 0;  // the rest are unbound stubs
    uint32_t extra_methods = 0;  // implemented methods the RE table lacks (ThreadValue.Set)
    uint64_t calls_total = 0, calls_implemented = 0;            // weighted by RE call sites
};
BindingCoverage binding_coverage();
// Text report: totals, then every binding with its status (implemented / unbound / host value).
std::string binding_coverage_report();

}  // namespace alamo::lua

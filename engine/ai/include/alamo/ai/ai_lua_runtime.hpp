// AI framework, Lua side (library alamo_ai_lua): plan-script loading and the script pool
// (docs/re/AI_GALACTIC.md §1.9.1, §1.9.4). Shared by the galactic AI and the tactical AI (A1).
//
// LOADING (0x4e7e30 / 0x4e7170) [H]: every *.lua of the plan folders (kPlanFolders order; [port] sorted by
// name within a folder) is run in a fresh VM; then PlanDefinitionLoad = true and Base_Definitions() (PGAICommands:
// defaults, Set_Contrast_Values(), Definitions()); the definition globals are read (read_plan_definition).
// The plan name is the file base name. A plan whose Category names no known goal type is discarded.
//
// POOL (0x244170 / 0x2488e0) [H]: ScriptPoolCount missing = 1 instance, N = N, 0 = not poolable. The load VM
// is the first pooled instance [port: further instances are created on demand, not preloaded - the same
// script state either way]. Release: kill the threads, Flush_G(), Base_Definitions(), back to the pool
// (pool-safe userdata globals survive Flush_G, as in the original).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_plans.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/lua/lua_vm.hpp"

namespace alamo::ai {

// Reads the plan-definition globals of a VM after Base_Definitions (Category, TaskForce, IgnoreTarget, ...).
// False when the plan has no known goal type or no task force.
bool read_plan_definition(lua::LuaVm& vm, const AiDefinitions& defs, PlanDefinition& out, std::vector<std::string>* diag);

class PlanScriptRuntime {
public:
    struct VmSetup {
        std::function<lua::LuaHost()> host;            // store / types / rng / files for new VMs
        std::function<void(lua::LuaVm&)> prepare;      // attach families (players world, AI host, ...)
    };
    explicit PlanScriptRuntime(VmSetup setup) : setup_(std::move(setup)) {}

    // Loads `paths` (already in load order). Appends to `library`.
    size_t load(const FileProvider& files, const std::vector<std::string>& paths, const AiDefinitions& defs,
                PlanLibrary& library, std::vector<std::string>* diag);
    // A VM of `plan` (pooled or fresh, Base_Definitions already run). nullptr on a script error.
    std::unique_ptr<lua::LuaVm> acquire(uint32_t plan, std::string* err = nullptr);
    // Back to the pool (or destroyed when not poolable / the pool is full).
    void release(uint32_t plan, std::unique_ptr<lua::LuaVm> vm);
    size_t pooled(uint32_t plan) const;
    size_t vms_created() const { return created_; }

    // A fresh VM running `chunk` (a whole script file) with `name`; prepare() applied first.
    std::unique_ptr<lua::LuaVm> make_vm(const std::string& name, const std::string& chunk, const std::string& chunk_name,
                                        std::string* err);

    // ---- N1 save / load (GalacticAiWorld::write / read drive these) ----
    size_t plan_count() const { return entries_.size(); }
    const std::string& plan_name(uint32_t plan) const { return entries_[plan].name; }
    // The pooled VMs of `plan`, pool order (back = the next acquire).
    const std::vector<std::unique_ptr<lua::LuaVm>>& pool(uint32_t plan) const { return entries_[plan].free; }
    // A VM to restore a saved Lua state into (LuaVm::read): the setup's host, script name `name`, prepare() applied,
    // no chunk run. Not counted in vms_created().
    std::unique_ptr<lua::LuaVm> blank_vm(const std::string& name);
    // A new instance of `plan` as acquire() makes one when its pool is empty (chunk + Base_Definitions).
    std::unique_ptr<lua::LuaVm> fresh(uint32_t plan, std::string* err = nullptr);
    // Replaces every pool (one vector per plan, plan order) and the created counter. A null entry stands for this
    // runtime's pristine(plan) VM, which is kept there (and stays pristine); every other VM of the old pools is destroyed.
    void restore_pools(std::vector<std::vector<std::unique_ptr<lua::LuaVm>>> pools, size_t created);
    void set_vms_created(size_t created) { created_ = created; }  // undoes the count of VMs a failed load made
    // N1 save size: the VM load() pooled for `plan` while it has never been acquired - pooled VMs only run between
    // acquire and release, so it is still exactly as load() left it, and every session rebuilt from the same data
    // holds the same one. Null when there is none. A save names it (by plan and pristine_digest) instead of storing
    // its image; the reader keeps its own after checking the digest.
    const lua::LuaVm* pristine(uint32_t plan) const;
    // StateHasher digest of pristine(plan)'s LuaVm::write image (0 without one): computed on first use, then cached
    // (the VM does not change while it stays pristine). Owner thread of the VMs only.
    uint64_t pristine_digest(uint32_t plan) const;

private:
    struct Entry {
        std::string name, file, source;
        int32_t pool_count = 1;
        std::vector<std::unique_ptr<lua::LuaVm>> free;
        const lua::LuaVm* pristine = nullptr;  // in `free`, or null (cleared when acquired)
        mutable uint64_t digest = 0;           // pristine_digest() cache
        mutable bool digest_known = false;
    };
    VmSetup setup_;
    std::vector<Entry> entries_;  // by plan index
    size_t created_ = 0;
};

// Script_<File>.Evaluate perception evaluators (AI_GALACTIC_2 §4.4, AI_GALACTIC_TOKENS §(b)) [H], shared by the galactic
// and the tactical AI (AI_TACTICAL_PERCEPTION: Script_ references unchanged in tactical):
//   * Data/Scripts/Evaluators/<File>.lua is loaded once, on first use [port: lazily, not by a folder scan at start - the
//     same instance either way], as ONE shared Lua instance for every AI player of the owning AI world [port: one per
//     world; the original's instance is shared by all modes too, but the port's worlds own their VMs];
//   * no cache; the host sets `PlayerObject` (Self) and `Target` (the context target, nil without one) on vm(), then
//     evaluate() calls Evaluate(<Parameter_Script_String values in order, a [A|B] list expanded>,
//     <Parameter_Script_Number>) and Evaluator_Clean_Up(); only a Lua number result succeeds.
//   * Script globals persist across calls and players. Serial sim-thread callers only.
class ScriptEvaluators {
public:
    // The shared VM of `name` (case-insensitive), loaded through `runtime` (its prepare() attaches the host families)
    // on first use; nullptr when the file is missing or fails to load (remembered; the load error goes to `errors`).
    lua::LuaVm* vm(PlanScriptRuntime& runtime, const FileProvider& files, std::string_view name, std::vector<std::string>& errors);
    // Evaluate(args...) then Evaluator_Clean_Up() on `vm`; clears the script logs. False (and `out` untouched) unless
    // Evaluate returned a Lua number. Script errors go to `errors`.
    static bool evaluate(lua::LuaVm& vm, std::string_view name, std::span<const Param> params, double& out,
                         std::vector<std::string>& errors);
    void clear() {
        vms_.clear();
        missing_.clear();
    }
    size_t loaded() const { return vms_.size(); }

    // ---- N1 save / load ----
    struct Loaded {
        std::string name;  // the file name as first requested (the VM's script name)
        std::unique_ptr<lua::LuaVm> vm;
    };
    const std::map<std::string, Loaded>& loaded_vms() const { return vms_; }  // upper-cased key order
    const std::set<std::string>& missing() const { return missing_; }
    void restore(std::map<std::string, Loaded> vms, std::set<std::string> missing) {
        vms_ = std::move(vms);
        missing_ = std::move(missing);
    }

private:
    std::map<std::string, Loaded> vms_;  // upper-cased file name (ordered)
    std::set<std::string> missing_;
};

// The plan files of every folder in load order: list(folder + "*.lua") per kPlanFolders entry, each folder
// sorted with sort_plan_files.
std::vector<std::string> list_plan_files(const std::function<std::vector<std::string>(std::string_view)>& list);

}  // namespace alamo::ai

#include "alamo/ai/ai_lua_runtime.hpp"

#include <algorithm>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/lua/ai_bridge.hpp"

namespace alamo::ai {

// ---------------------------------------------------------------- Script_ evaluators (AI_GALACTIC_2 §4.4)
lua::LuaVm* ScriptEvaluators::vm(PlanScriptRuntime& runtime, const FileProvider& files, std::string_view name,
                                 std::vector<std::string>& errors) {
    const std::string key = ai_upper_copy(name);
    auto it = vms_.find(key);
    if (it != vms_.end()) return it->second.vm.get();
    if (missing_.count(key)) return nullptr;
    const std::string path = "Data\\Scripts\\Evaluators\\" +std::string(name) + ".lua";
    std::vector<uint8_t> bytes;
    std::string err;
    std::unique_ptr<lua::LuaVm> v;
    if (files.read(path, bytes))
        v = runtime.make_vm(std::string(name), std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), path, &err);
    if (!v) {
        missing_.insert(key);
        if (!err.empty()) errors.push_back("evaluator " + std::string(name) + ": " + err);
        return nullptr;
    }
    return vms_.emplace(key, Loaded{std::string(name), std::move(v)}).first->second.vm.get();
}

bool ScriptEvaluators::evaluate(lua::LuaVm& vm, std::string_view name, std::span<const Param> params, double& out,
                                std::vector<std::string>& errors) {
    std::vector<lua::AiEventArg> args;
    for (const Param& p : params)
        if (ai_iequal(p.name, "Parameter_Script_String")) {
            lua::AiEventArg a;
            a.kind = lua::AiEventArg::String;
            if (p.value.kind == ParamValue::Kind::Enum && !p.value.enum_values.empty()) {  // a table of strings is expanded
                for (const std::string& v : p.value.enum_values) {
                    a.text = v;
                    args.push_back(a);
                }
                continue;
            }
            a.text = p.value.text;
            args.push_back(a);
        }
    for (const Param& p : params)
        if (ai_iequal(p.name, "Parameter_Script_Number") && p.value.kind == ParamValue::Kind::Number) {
            lua::AiEventArg a;
            a.kind = lua::AiEventArg::Number;
            a.number = p.value.number;
            args.push_back(a);
        }
    std::string err;
    double v = 0.0;
    const bool ok = lua::call_global_number(vm, "Evaluate", args, v, &err);
    if (!err.empty()) errors.push_back("evaluator " + std::string(name) + ": " + err);
    std::string err2;
    lua::call_global_function(vm, "Evaluator_Clean_Up", {}, &err2);
    lua::clear_script_logs(vm);
    if (ok) out = v;
    return ok;
}

bool read_plan_definition(lua::LuaVm& vm, const AiDefinitions& defs, PlanDefinition& out, std::vector<std::string>* diag) {
    const std::string category = lua::global_string(vm, "Category");
    out.goal_types.clear();
    for (const std::string& name : ai_split(category, "|")) {
        const uint32_t g = defs.find_goal(name);
        if (g == kAiNone) {
            if (diag && !ai_iequal(name, "AlwaysOff")) diag->push_back(out.name + ": unknown goal type '" + name + "'");
            continue;
        }
        out.goal_types.push_back(g);
    }
    out.task_forces.clear();
    for (const std::vector<std::string>& tf : lua::global_string_table_list(vm, "TaskForce")) {
        TaskForceDef d;
        if (parse_task_force(tf, d, diag)) out.task_forces.push_back(std::move(d));
    }
    renormalise_percents(out);  // 0x670630 (AI_GALACTIC_2 §1.3)
    out.ignore_target = lua::global_bool(vm, "IgnoreTarget", false);
    out.magic_plan = lua::global_bool(vm, "MagicPlan", false);
    out.magic_plan_stealing = lua::global_bool(vm, "MagicPlanStealing", true);
    out.allow_free_store_units = lua::global_bool(vm, "AllowFreeStoreUnits", true);
    out.allow_engaged_units = lua::global_bool(vm, "AllowEngagedUnits", true);
    out.additional_cost = static_cast<float>(lua::global_number(vm, "AdditionalCost", 0.0));
    out.minimum_cost = static_cast<float>(lua::global_number(vm, "MinimumCost", 0.0));
    out.per_failure_contrast_adjust = static_cast<float>(lua::global_number(vm, "PerFailureContrastAdjust", 0.1));
    out.min_contrast_scale = static_cast<float>(lua::global_number(vm, "MinContrastScale", 1.1));
    out.max_contrast_scale = static_cast<float>(lua::global_number(vm, "MaxContrastScale", 1.5));
    out.required_categories = lua::global_string_array(vm, "RequiredCategories");
    out.pool_count = lua::global_is_nil(vm, "ScriptPoolCount")
                         ? 1
                         : static_cast<int32_t>(lua::global_number(vm, "ScriptPoolCount", 1.0));
    if (out.goal_types.empty()) return false;
    if (out.task_forces.empty()) {
        if (diag) diag->push_back(out.name + ": no TaskForce definitions");
        return false;
    }
    return true;
}

std::unique_ptr<lua::LuaVm> PlanScriptRuntime::make_vm(const std::string& name, const std::string& chunk,
                                                        const std::string& chunk_name, std::string* err) {
    lua::LuaVmOptions opt;
    opt.script_name = name;
    auto vm = std::make_unique<lua::LuaVm>(setup_.host ? setup_.host() : lua::LuaHost{}, opt);
    ++created_;
    if (setup_.prepare) setup_.prepare(*vm);
    std::string e;
    if (!vm->run(chunk, chunk_name, &e)) {
        if (err) *err = name + ": " + e;
        return nullptr;
    }
    return vm;
}

size_t PlanScriptRuntime::load(const FileProvider& files, const std::vector<std::string>& paths, const AiDefinitions& defs,
                               PlanLibrary& library, std::vector<std::string>* diag) {
    size_t loaded = 0;
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i < paths.size(); ++i) {
        const std::string& path = paths[i];
        if (!files.read(path, bytes)) {
            if (diag) diag->push_back(path + ": cannot read");
            continue;
        }
        const std::string name = file_base_name(path);
        if (library.find(name) != kAiNone) {
            if (diag) diag->push_back(path + ": duplicate plan name " + name + " (first kept)");
            continue;
        }
        std::string source(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::string err;
        std::unique_ptr<lua::LuaVm> vm = make_vm(name, source, path, &err);
        if (!vm) {
            if (diag) diag->push_back(err);
            continue;
        }
        lua::set_bool_global(*vm, "PlanDefinitionLoad", true);
        if (!vm->call("Base_Definitions", &err)) {
            if (diag) diag->push_back(path + ": Base_Definitions: " + err);
            continue;
        }
        PlanDefinition def;
        def.name = name;
        def.file = path;
        for (uint32_t f = 0; f < std::size(kPlanFolders); ++f) {
            const std::string folder = kPlanFolders[f];
            if (path.size() > folder.size() && ai_iequal(std::string_view(path).substr(0, folder.size()), folder) &&
                path.find('\\', folder.size()) == std::string::npos)
                def.folder = f;
        }
        if (!read_plan_definition(*vm, defs, def, diag)) continue;
        Entry e;
        e.name = name;
        e.file = path;
        e.source = std::move(source);
        e.pool_count = def.pool_count;
        lua::clear_script_logs(*vm);
        if (e.pool_count != 0) {
            e.pristine = vm.get();
            e.free.push_back(std::move(vm));
        }
        library.plans.push_back(std::move(def));
        entries_.push_back(std::move(e));
        ++loaded;
    }
    return loaded;
}

std::unique_ptr<lua::LuaVm> PlanScriptRuntime::acquire(uint32_t plan, std::string* err) {
    if (plan >= entries_.size()) return nullptr;
    Entry& e = entries_[plan];
    if (!e.free.empty()) {
        std::unique_ptr<lua::LuaVm> vm = std::move(e.free.back());
        e.free.pop_back();
        if (vm.get() == e.pristine) {
            e.pristine = nullptr;
            e.digest_known = false;
        }
        return vm;
    }
    return fresh(plan, err);
}

std::unique_ptr<lua::LuaVm> PlanScriptRuntime::fresh(uint32_t plan, std::string* err) {
    if (plan >= entries_.size()) return nullptr;
    const Entry& e = entries_[plan];
    std::unique_ptr<lua::LuaVm> vm = make_vm(e.name, e.source, e.file, err);
    if (!vm) return nullptr;
    std::string ce;
    if (!vm->call("Base_Definitions", &ce)) {
        if (err) *err = e.name + ": Base_Definitions: " + ce;
        return nullptr;
    }
    return vm;
}

std::unique_ptr<lua::LuaVm> PlanScriptRuntime::blank_vm(const std::string& name) {
    lua::LuaVmOptions opt;
    opt.script_name = name;
    auto vm = std::make_unique<lua::LuaVm>(setup_.host ? setup_.host() : lua::LuaHost{}, opt);
    if (setup_.prepare) setup_.prepare(*vm);
    return vm;
}

void PlanScriptRuntime::restore_pools(std::vector<std::vector<std::unique_ptr<lua::LuaVm>>> pools, size_t created) {
    for (size_t i = 0; i < entries_.size(); ++i) {
        Entry& e = entries_[i];
        std::vector<std::unique_ptr<lua::LuaVm>> next = i < pools.size() ? std::move(pools[i]) : decltype(e.free){};
        std::unique_ptr<lua::LuaVm> kept;  // the pristine VM, while no null entry has taken it
        for (std::unique_ptr<lua::LuaVm>& vm : e.free)
            if (vm && vm.get() == e.pristine) kept = std::move(vm);
        bool named = false;
        for (std::unique_ptr<lua::LuaVm>& slot : next)
            if (!slot && kept) {
                slot = std::move(kept);
                named = true;
            }
        std::erase_if(next, [](const std::unique_ptr<lua::LuaVm>& vm) { return !vm; });
        if (!named) {
            e.pristine = nullptr;
            e.digest_known = false;
        }
        e.free = std::move(next);  // the old pool's other VMs (and an unnamed pristine one) are destroyed
    }
    created_ = created;
}

const lua::LuaVm* PlanScriptRuntime::pristine(uint32_t plan) const {
    return plan < entries_.size() ? entries_[plan].pristine : nullptr;
}

uint64_t PlanScriptRuntime::pristine_digest(uint32_t plan) const {
    if (plan >= entries_.size() || !entries_[plan].pristine) return 0;
    const Entry& e = entries_[plan];
    if (!e.digest_known) {
        ByteWriter w;
        e.pristine->write(w);
        StateHasher h;
        h.bytes(w.bytes().data(), w.size());
        e.digest = h.digest();
        e.digest_known = true;
    }
    return e.digest;
}

void PlanScriptRuntime::release(uint32_t plan, std::unique_ptr<lua::LuaVm> vm) {
    if (!vm || plan >= entries_.size()) return;
    Entry& e = entries_[plan];
    lua::reset_script_threads(*vm);
    std::string err;
    vm->call("Flush_G", &err);
    lua::install_global_value_table(*vm);
    vm->call("Base_Definitions", &err);
    lua::clear_script_logs(*vm);
    const size_t cap = e.pool_count <= 0 ? 0 : static_cast<size_t>(e.pool_count);
    if (e.free.size() < cap) e.free.push_back(std::move(vm));
}

size_t PlanScriptRuntime::pooled(uint32_t plan) const { return plan < entries_.size() ? entries_[plan].free.size() : 0; }

void append_script_errors(const lua::LuaVm& vm, const std::string& prefix, std::vector<std::string>& out) {
    for (const std::string& e : vm.errors()) out.push_back(prefix + e);
    for (const std::string& m : vm.messages())
        if (m.starts_with("SCRIPT ERROR")) out.push_back(prefix + m);
}

std::vector<std::string> list_plan_files(const std::function<std::vector<std::string>(std::string_view)>& list) {
    std::vector<std::string> out;
    for (const char* folder : kPlanFolders) {
        std::vector<std::string> files = list(std::string(folder) + "*.lua");
        // Only direct children of the folder (Data\Scripts\AI\*.lua must not take SpaceMode\ files).
        std::vector<std::string> direct;
        const size_t n = std::string_view(folder).size();
        for (std::string& f : files)
            if (f.size() > n && f.find('\\', n) == std::string::npos && f.find('/', n) == std::string::npos)
                direct.push_back(std::move(f));
        sort_plan_files(direct);
        for (std::string& f : direct) out.push_back(std::move(f));
    }
    return out;
}

}  // namespace alamo::ai

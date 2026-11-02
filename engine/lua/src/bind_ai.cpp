// Binding family: AI plans and free stores (roadmap A2; alamo/lua/ai_bridge.hpp). Every binding forwards to
// the AiScriptHost attached to the VM (engine/ai implements it); without a host the bindings answer as an
// empty AI would (no task force, nil statuses). Argument checks follow the vm_internal.hpp error discipline
// (Call::fail, never lua_error). Validation strings are the port's own unless LUA_API.md gives one.
//
// Methods NOT in the RE binding table (intentional, counted as "extra" by binding_coverage):
//   * Is_Pool_Safe -> false on TaskForceClass, AIBlockingStatus, AITargetLocationWrapper, BudgetWrapper,
//     AIFreeStoreClass, GalacticFreeStoreClass [inferred]: these wrappers point at one plan's state, so
//     PGBase's Flush_G must drop them when a pooled script is released (LuaUserVar's default says true);
//   * AIBlockingStatus.IsFinished / Result: the AI's own status class (the objects family owns BlockingStatus);
//   * GalacticFreeStoreClass.Get_Object_Count / Is_Object_On_Free_Store: the table lists them on
//     AIFreeStoreClass only and the registry has no AIFreeStoreClass parent for GalacticFreeStoreClass.
#include <cstring>
#include <string>
#include <vector>

#include "alamo/lua/ai_bridge.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

namespace {

using detail::Call;
using detail::Ud;

const char kHostKey[] = "alamo.ai.host";
const char kScriptKey[] = "alamo.ai.script";
const char kMetaKey[] = "alamo.meta";  // bindings.cpp: the shared wrapper metatable

AiScriptHost* host_of(lua_State* L) {
    lua_pushstring(L, kHostKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    void* p = lua_type(L, -1) == LUA_TLIGHTUSERDATA ? lua_touserdata(L, -1) : nullptr;
    lua_pop(L, 1);
    return static_cast<AiScriptHost*>(p);
}

int32_t script_of(lua_State* L) {
    lua_pushstring(L, kScriptKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int32_t id = lua_type(L, -1) == LUA_TNUMBER ? detail::to_int(lua_tonumber(L, -1)) : -1;
    lua_pop(L, 1);
    return id;
}

// The VM's Impl from the shared metatable's __index closure (upvalue 1 = &Impl, bindings.cpp).
LuaVm::Impl* impl_of(lua_State* L) {
    lua_pushstring(L, kMetaKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    LuaVm::Impl* vm = nullptr;
    if (lua_type(L, -1) == LUA_TTABLE) {
        lua_pushstring(L, "__index");
        lua_rawget(L, -2);
        if (lua_type(L, -1) == LUA_TFUNCTION && lua_getupvalue(L, -1, 1)) {
            if (lua_type(L, -1) == LUA_TLIGHTUSERDATA) vm = static_cast<LuaVm::Impl*>(lua_touserdata(L, -1));
            lua_pop(L, 1);
        }
        lua_pop(L, 1);
    }
    lua_pop(L, 1);
    return vm;
}

uint32_t cls(const char* name) { return detail::class_id(name); }
uint32_t cls_status() {
    static const uint32_t c = cls("AIBlockingStatus");
    return c;
}
uint32_t cls_target() {
    static const uint32_t c = cls("AITargetLocationWrapper");
    return c;
}
uint32_t cls_budget() {
    static const uint32_t c = cls("BudgetWrapper");
    return c;
}
uint32_t cls_free_store(bool galactic) {
    static const uint32_t g = cls("GalacticFreeStoreClass");
    static const uint32_t a = cls("AIFreeStoreClass");
    return galactic ? g : a;
}
uint32_t cls_tf(AiTaskForceKind k) {
    static const uint32_t generic = cls("TaskForceClass");
    static const uint32_t gal = cls("GalacticTaskForceClass");
    static const uint32_t space = cls("SpaceTaskForceClass");
    static const uint32_t land = cls("LandTaskForceClass");
    switch (k) {
        case AiTaskForceKind::Galactic: return gal;
        case AiTaskForceKind::Space: return space;
        case AiTaskForceKind::Land: return land;
        default: return generic;
    }
}
bool is_tf_class(uint32_t c) {
    return c == cls_tf(AiTaskForceKind::Generic) || c == cls_tf(AiTaskForceKind::Galactic) ||
           c == cls_tf(AiTaskForceKind::Space) || c == cls_tf(AiTaskForceKind::Land);
}

// Key spaces for push_cached_userdata are per class; ids are non-negative.
void push_tf(lua_State* L, int32_t tf, AiTaskForceKind kind) {
    Ud ud;
    ud.cls = cls_tf(kind);
    ud.id = tf;
    detail::push_cached_userdata(L, ud, static_cast<lua_Number>(tf));
}
void push_target(lua_State* L, int32_t target) {
    if (target < 0) {
        lua_pushnil(L);
        return;
    }
    Ud ud;
    ud.cls = cls_target();
    ud.id = target;
    detail::push_cached_userdata(L, ud, static_cast<lua_Number>(target));
}
int push_status(Call& c, int32_t status) {
    if (status < 0) {
        lua_pushnil(c.L);
        return 1;
    }
    Ud ud;
    ud.cls = cls_status();
    ud.id = status;
    detail::push_engine_userdata(c.L, ud);
    return 1;
}
int push_nil(Call& c) {
    lua_pushnil(c.L);
    return 1;
}
int push_bool(Call& c, bool b) {
    lua_pushboolean(c.L, b ? 1 : 0);
    return 1;
}
int push_num(Call& c, double d) {
    lua_pushnumber(c.L, d);
    return 1;
}

bool is_bool(const Call& c, int i) { return c.has(i) && lua_type(c.L, c.idx(i)) == LUA_TBOOLEAN; }
bool to_bool(const Call& c, int i) { return lua_toboolean(c.L, c.idx(i)) != 0; }

// Any wrapper argument -> AiArg.
AiArg arg_of(lua_State* L, LuaVm::Impl& vm, int index) {
    AiArg a;
    if (lua_type(L, index) != LUA_TUSERDATA) return a;
    const auto* ud = static_cast<const Ud*>(lua_touserdata(L, index));
    if (!ud) return a;
    if (ud->cls == detail::cls_game_object()) {
        if (detail::object_alive(vm, *ud)) {
            a.present = true;
            a.object = detail::object_handle(*ud);
        }
    } else if (ud->cls == cls_target()) {
        a.present = true;
        a.target = ud->id;
    } else if (is_tf_class(ud->cls)) {
        a.present = true;
        a.task_force = ud->id;
    } else if (ud->cls == detail::cls_player()) {
        a.present = true;
        a.player = ud->id;
    }
    return a;
}
AiArg arg(Call& c, int i) { return c.has(i) ? arg_of(c.L, *c.vm, c.idx(i)) : AiArg{}; }

objects::ObjectHandle object_arg(Call& c, int i) {
    const AiArg a = arg(c, i);
    return a.object;
}

// ---------------------------------------------------------------- task forces

int m_tf_is_valid(Call& c) {
    AiScriptHost* h = host_of(c.L);
    return push_bool(c, h && h->tf_valid(c.self->id));
}
int m_tf_force_count(Call& c) {
    AiScriptHost* h = host_of(c.L);
    return push_num(c, h ? h->tf_force_count(c.self->id) : 0);
}
int tf_flag(Call& c, AiTfFlag flag, const char* name) {
    if (c.nargs < 1 || !is_bool(c, 1)) return c.fail("TaskForceClass::%s -- expected a boolean parameter.", name);
    if (AiScriptHost* h = host_of(c.L)) h->tf_set_flag(c.self->id, flag, to_bool(c, 1));
    return 0;
}
int m_tf_set_plan_result(Call& c) { return tf_flag(c, AiTfFlag::PlanResult, "Set_Plan_Result"); }
int m_tf_set_removable(Call& c) { return tf_flag(c, AiTfFlag::Removable, "Set_As_Goal_System_Removable"); }
int m_tf_test_target_contrast(Call& c) { return tf_flag(c, AiTfFlag::TestTargetContrast, "Test_Target_Contrast"); }
// Block_Goal_Proposal / Unblock_Goal_Proposal: arguments not analysed (AI_GALACTIC.md open item 9).
int m_tf_block_goal_proposal(Call& c) {
    if (AiScriptHost* h = host_of(c.L)) h->tf_set_flag(c.self->id, AiTfFlag::BlockGoalProposal, true);
    return 0;
}
int m_tf_unblock_goal_proposal(Call& c) {
    if (AiScriptHost* h = host_of(c.L)) h->tf_set_flag(c.self->id, AiTfFlag::BlockGoalProposal, false);
    return 0;
}
int m_tf_produce_force(Call& c) {
    AiScriptHost* h = host_of(c.L);
    if (!h) return push_nil(c);
    AiArg stage;
    bool sync = false;
    for (int i = 1; i <= c.nargs; ++i) {
        if (is_bool(c, i))
            sync = to_bool(c, i);
        else if (lua_type(c.L, c.idx(i)) == LUA_TUSERDATA)
            stage = arg(c, i);
    }
    return push_status(c, h->tf_produce_force(c.self->id, stage, sync));
}
int m_tf_get_stage(Call& c) {
    AiScriptHost* h = host_of(c.L);
    push_target(c.L, h ? h->tf_get_stage(c.self->id) : -1);
    return 1;
}
int m_tf_all_on_free_store(Call& c) {
    AiScriptHost* h = host_of(c.L);
    return push_bool(c, h && h->tf_all_on_free_store(c.self->id));
}
int tf_order(Call& c, AiTfOrder order, bool needs_arg, const char* name) {
    AiScriptHost* h = host_of(c.L);
    const AiArg a = arg(c, 1);
    if (needs_arg && !a.present) return c.fail("TaskForceClass::%s -- invalid target parameter.", name);
    if (!h) return push_nil(c);
    return push_status(c, h->tf_order(c.self->id, order, a));
}
int m_tf_move_to(Call& c) { return tf_order(c, AiTfOrder::MoveTo, true, "Move_To"); }
int m_tf_form_units(Call& c) { return tf_order(c, AiTfOrder::FormUnits, false, "Form_Units"); }
// P4 lua: Land_Units (0x6cec80) and the galactic Activate_Ability (0x6cd180) fire GalacticSim::ground_activated_trigger
// per unit (P4_SIM_SYSTEMS 6.1 / 3.4); the AI host does it inside its tf_order (engine/ai galactic_ai.cpp, P4 ai), so
// the bindings only forward (one fire per unit).
int m_tf_land_units(Call& c) { return tf_order(c, AiTfOrder::LandUnits, false, "Land_Units"); }
int m_tf_launch_units(Call& c) { return tf_order(c, AiTfOrder::LaunchUnits, false, "Launch_Units"); }
int m_tf_invade(Call& c) { return tf_order(c, AiTfOrder::Invade, false, "Invade"); }
int m_tf_raid(Call& c) { return tf_order(c, AiTfOrder::Raid, true, "Raid"); }
int m_tf_force_test_space_conflict(Call& c) {
    return tf_order(c, AiTfOrder::ForceTestSpaceConflict, false, "Force_Test_Space_Conflict");
}
// OI lua: Activate_Ability([name] [, on | target]). Land / Space / generic task forces (0x5f9890, AI_TACTICAL §7.3):
// each unit's GameObjectWrapper.Activate_Ability with the same arguments (task-force unit order; the objects family
// resolves an AI target / task force target argument), then the host's status. GalacticTaskForceClass (0x6cd180,
// P4_SIM_SYSTEMS 6.1, L1P4_LUA_GAPS 1): only the host, which fires the Ground_Activated trigger per member (the trigger
// tries every Ground_Activated ability; shipped galactic plans pass no argument); the optional argument is an object
// type (galactic_ability_type_arg) whose name the host gets as `ability`; like Activate_Ability_Galactic it returns no
// values (refix t04, L1P4_LUA_GAPS 1.2 item 4: the host's status id is dropped). Other task forces: the host gets the
// name, the on flag and the target (AiScriptHost::tf_activate_ability).
int m_tf_activate_ability(Call& c) {
    AiScriptHost* h = host_of(c.L);
    if (!h) return c.self->cls == cls_tf(AiTaskForceKind::Galactic) ? 0 : push_nil(c);
    const bool named = c.is_string(1);
    const std::string name = named ? std::string(c.string(1)) : std::string();
    const int ti = named ? 2 : 1;
    const AiArg target = arg(c, ti);
    const bool on = is_bool(c, ti) ? to_bool(c, ti) : true;
    if (c.self->cls == cls_tf(AiTaskForceKind::Galactic)) {
        // L1P4_LUA_GAPS 1: the galactic body (0x6cd180) takes an optional object type (wrapper or name), not an ability
        // name; a bad argument fires nothing. The host gets the type's name ("" = no filter) and keeps the task-force
        // members of that type.
        uint32_t filter = objects::kNone;
        if (!detail::galactic_ability_type_arg(c, "GalacticTaskForce::Activate_Ability", filter)) return 0;
        const objects::GameObjectTypes* types = c.vm->host.types;
        const std::string type_name =
            filter != objects::kNone && types && filter < types->raw().type_count() ? std::string(types->raw().types().name[filter]) : std::string();
        h->tf_activate_ability(c.self->id, type_name, on, AiArg{});
        return 0;
    }
    // S7 t15 A1 (MOVEMENT §3.1.1 step 6): the host learns which units activated (one AbilityBlockStatus count each; the
    // others get Unit_Ability_Finished queued at once).
    std::vector<std::pair<objects::ObjectHandle, bool>> results;
    for (const objects::ObjectHandle u : h->tf_units(c.self->id))
        results.emplace_back(u, detail::object_activate_ability(c, u, c.first, c.nargs));
    return push_status(c, h->tf_activate_ability_units(c.self->id, name, on, target, results));
}
int m_tf_is_raid_capable(Call& c) {
    AiScriptHost* h = host_of(c.L);
    return push_bool(c, h && h->tf_is_raid_capable(c.self->id));
}
int m_tf_release_forces(Call& c) {
    double f = 1.0;
    if (c.nargs >= 1) {
        if (!c.is_number(1)) return c.fail("TaskForceClass::Release_Forces -- expected a number parameter.");
        f = c.number(1);
    }
    if (f < 0.0 || f > 1.0) return c.fail("TaskForceClass::Release_Forces -- fraction must be in [0, 1].");
    if (AiScriptHost* h = host_of(c.L)) h->tf_release_forces(c.self->id, f);
    return 0;
}
int m_tf_release_unit(Call& c) {
    const objects::ObjectHandle u = object_arg(c, 1);
    if (!u) return c.fail("TaskForceClass::Release_Unit -- invalid unit parameter.");
    if (AiScriptHost* h = host_of(c.L)) h->tf_release_unit(c.self->id, u);
    return 0;
}
int m_tf_get_unit_table(Call& c) {
    AiScriptHost* h = host_of(c.L);
    std::vector<objects::ObjectHandle> units;
    if (h) units = h->tf_units(c.self->id);
    lua_newtable(c.L);
    int n = 0;
    for (const objects::ObjectHandle u : units) {
        detail::push_object(c.L, *c.vm, u);
        if (lua_isnil(c.L, -1)) {
            lua_pop(c.L, 1);
            continue;
        }
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}
int m_tf_goal_type_name(Call& c) {
    AiScriptHost* h = host_of(c.L);
    const std::string s = h ? h->tf_goal_type_name(c.self->id) : std::string();
    lua_pushlstring(c.L, s.data(), s.size());
    return 1;
}
int m_tf_collect_all_free_units(Call& c) {
    AiScriptHost* h = host_of(c.L);
    return push_bool(c, h && h->tf_collect_free_units(c.self->id));
}
int m_not_pool_safe(Call& c) { return push_bool(c, false); }

// ---------------------------------------------------------------- blocking statuses

int m_status_is_finished(Call& c) {
    AiScriptHost* h = host_of(c.L);
    return push_bool(c, !h || h->status_finished(c.self->id));
}
int m_status_result(Call& c) {
    AiScriptHost* h = host_of(c.L);
    const AiStatusResult r = h ? h->status_result(c.self->id) : AiStatusResult::Nil;
    if (r == AiStatusResult::Nil) return push_nil(c);
    return push_bool(c, r == AiStatusResult::True);
}

// ---------------------------------------------------------------- AI targets

int m_target_is_valid(Call& c) {
    AiScriptHost* h = host_of(c.L);
    return push_bool(c, h && h->target_valid(c.self->id));
}
int m_target_game_object(Call& c) {
    AiScriptHost* h = host_of(c.L);
    if (!h) return push_nil(c);
    detail::push_object(c.L, *c.vm, h->target_object(c.self->id));
    return 1;
}
int m_target_distance(Call& c) {
    AiScriptHost* h = host_of(c.L);
    const AiArg other = arg(c, 1);
    if (!other.present) return c.fail("AITargetLocationWrapper::Get_Distance -- invalid parameter 1.");
    const double d = h ? h->target_distance(c.self->id, other) : -1.0;
    return d < 0.0 ? push_nil(c) : push_num(c, d);
}

// ---------------------------------------------------------------- Budget (the calling plan's goal)

int budget_query(Call& c, AiBudgetOp op, bool has_amount, bool has_type, const char* name) {
    double amount = 0.0;
    std::string type;
    if (has_amount) {
        if (!c.is_number(1)) return c.fail("BudgetWrapper::%s -- invalid type for parameter 1. Expected number.", name);
        amount = c.number(1);
    }
    if (has_type) {
        if (!c.is_string(2)) return c.fail("BudgetWrapper::%s -- invalid type for parameter 2. Expected string.", name);
        type = std::string(c.string(2));
    }
    AiScriptHost* h = host_of(c.L);
    return push_num(c, h ? h->budget_op(c.self->id, op, amount, type) : 0.0);
}
int m_budget_allocate(Call& c) { return budget_query(c, AiBudgetOp::Allocate, true, false, "Allocate_Resources"); }
int m_budget_flush_all(Call& c) { return budget_query(c, AiBudgetOp::FlushAll, false, false, "Flush_All_Resources"); }
int m_budget_flush_unallocated(Call& c) {
    return budget_query(c, AiBudgetOp::FlushUnallocated, false, false, "Flush_Unallocated_Resources");
}
int m_budget_spendable(Call& c) { return budget_query(c, AiBudgetOp::Spendable, false, false, "Get_Spendable_Resources"); }
int m_budget_unallocated(Call& c) {
    return budget_query(c, AiBudgetOp::Unallocated, false, false, "Get_Unallocated_Resources");
}
int m_budget_give(Call& c) { return budget_query(c, AiBudgetOp::GiveToGoal, true, true, "Give_Resources_To_Goal"); }
int m_budget_take(Call& c) { return budget_query(c, AiBudgetOp::TakeFromGoal, true, true, "Take_Resources_From_Goal"); }
int budget_wait(Call& c, AiBudgetOp op, const char* name) {
    if (!c.is_number(1)) return c.fail("BudgetWrapper::%s -- invalid type for parameter 1. Expected number.", name);
    AiScriptHost* h = host_of(c.L);
    return push_status(c, h ? h->budget_wait(c.self->id, op, c.number(1)) : -1);
}
int m_budget_wait_spendable(Call& c) { return budget_wait(c, AiBudgetOp::WaitSpendable, "Wait_For_Spendable_Resources"); }
int m_budget_wait_unallocated(Call& c) {
    return budget_wait(c, AiBudgetOp::WaitUnallocated, "Wait_For_Unallocated_Resources");
}

// ---------------------------------------------------------------- free store

int m_fs_object_count(Call& c) {
    int which = -1;
    if (c.nargs >= 1 && is_bool(c, 1)) which = to_bool(c, 1) ? 1 : 0;
    AiScriptHost* h = host_of(c.L);
    return push_num(c, h ? h->free_store_count(c.self->id, which) : 0);
}
int m_fs_has(Call& c) {
    AiScriptHost* h = host_of(c.L);
    const objects::ObjectHandle u = object_arg(c, 1);
    return push_bool(c, h && u && h->free_store_has(c.self->id, u));
}
int m_fs_in_transit(Call& c) {
    AiScriptHost* h = host_of(c.L);
    const objects::ObjectHandle u = object_arg(c, 1);
    if (!u) return c.fail("GalacticFreeStoreClass::Is_Unit_In_Transit -- invalid unit parameter.");
    return push_bool(c, h && h->unit_in_transit(u));
}
int m_fs_safe(Call& c) {
    AiScriptHost* h = host_of(c.L);
    const objects::ObjectHandle u = object_arg(c, 1);
    if (!u) return c.fail("GalacticFreeStoreClass::Is_Unit_Safe -- invalid unit parameter.");
    return push_bool(c, !h || h->unit_safe(u));
}
int m_fs_move_object(Call& c) {
    AiScriptHost* h = host_of(c.L);
    const objects::ObjectHandle u = object_arg(c, 1);
    const AiArg dest = arg(c, 2);
    if (!u || !dest.present) return c.fail("GalacticFreeStoreClass::Move_Object -- invalid parameters.");
    return push_bool(c, h && h->free_store_move(c.self->id, u, dest));
}

// ---------------------------------------------------------------- FindTarget / _FindStageArea

std::string string_arg(Call& c, int i) { return c.is_string(i) ? std::string(c.string(i)) : std::string(); }

// FindTarget(source, perception, application, probability [, max distance]) [arguments per PGTaskForce /
// PGCommands call sites].
int g_find_target(Call& c) {
    if (c.nargs < 3) return c.fail("FindTarget -- invalid number of parameters. Expected at least 3, got %d.", c.nargs);
    if (!c.is_string(2) || !c.is_string(3)) return c.fail("FindTarget -- invalid type for parameters 2/3. Expected strings.");
    AiFindTargetRequest r;
    r.source = arg(c, 1);
    r.perception = string_arg(c, 2);
    r.application = string_arg(c, 3);
    if (c.is_number(4)) r.probability = c.number(4);
    if (c.is_number(5)) r.max_distance = c.number(5);
    AiScriptHost* h = host_of(c.L);
    const int32_t t = h ? h->find_target(script_of(c.L), r) : -1;
    // A1 (tiny marked fix): a unit target is returned as its game object (AI_TACTICAL.md 6.1).
    if (h && t >= 0)
        if (const objects::ObjectHandle o = h->find_target_object(t)) {
            detail::push_object(c.L, *c.vm, o);
            return 1;
        }
    push_target(c.L, t);
    return 1;
}
// FindTarget.Reachable_Target(player, perception, application, reachability, probability [, source [, max distance]])
int m_reachable_target(Call& c) {
    if (c.nargs < 5) return c.fail("FindTarget.Reachable_Target -- invalid number of parameters. Expected at least 5, got %d.", c.nargs);
    if (!c.is_string(2) || !c.is_string(3) || !c.is_string(4))
        return c.fail("FindTarget.Reachable_Target -- invalid type for parameters 2-4. Expected strings.");
    AiFindTargetRequest r;
    r.source = arg(c, 1);
    r.perception = string_arg(c, 2);
    r.application = string_arg(c, 3);
    r.reachability = string_arg(c, 4);
    if (c.is_number(5)) r.probability = c.number(5);
    r.relative = arg(c, 6);
    if (c.is_number(7)) r.max_distance = c.number(7);
    AiScriptHost* h = host_of(c.L);
    push_target(c.L, h ? h->find_target(script_of(c.L), r) : -1);
    return 1;
}
// _FindStageArea(player, target, taskforce) -> the staging PLANET object (PGTaskForce / PathBonus pass it
// to Find_Path, which takes planets) [inferred].
int g_find_stage_area(Call& c) {
    AiScriptHost* h = host_of(c.L);
    const int32_t t = h ? h->find_stage_area(script_of(c.L), arg(c, 1), arg(c, 2), arg(c, 3)) : -1;
    if (t < 0) return push_nil(c);
    detail::push_object(c.L, *c.vm, h->target_object(t));
    return 1;
}

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;

#define ALAMO_TF_METHODS(K)                                                  \
    {M, K, "Is_Valid", m_tf_is_valid},                                       \
    {M, K, "Get_Force_Count", m_tf_force_count},                             \
    {M, K, "Set_Plan_Result", m_tf_set_plan_result},                         \
    {M, K, "Set_As_Goal_System_Removable", m_tf_set_removable},              \
    {M, K, "Test_Target_Contrast", m_tf_test_target_contrast},               \
    {M, K, "Block_Goal_Proposal", m_tf_block_goal_proposal},                 \
    {M, K, "Unblock_Goal_Proposal", m_tf_unblock_goal_proposal},             \
    {M, K, "Produce_Force", m_tf_produce_force},                             \
    {M, K, "Get_Stage", m_tf_get_stage},                                     \
    {M, K, "Are_All_Units_On_Free_Store", m_tf_all_on_free_store},           \
    {M, K, "Move_To", m_tf_move_to},                                         \
    {M, K, "Form_Units", m_tf_form_units},                                   \
    {M, K, "Release_Forces", m_tf_release_forces},                           \
    {M, K, "Release_Unit", m_tf_release_unit},                               \
    {M, K, "Get_Unit_Table", m_tf_get_unit_table},                           \
    {M, K, "Get_Goal_Type_Name", m_tf_goal_type_name},                       \
    {M, K, "Collect_All_Free_Units", m_tf_collect_all_free_units},           \
    {M, K, "Activate_Ability", m_tf_activate_ability}

constexpr detail::ImplEntry kImpl[] = {
    // TaskForceClass (inherited by Galactic/Space/LandTaskForceClass)
    ALAMO_TF_METHODS("TaskForceClass"),
    {M, "TaskForceClass", "Is_Pool_Safe", m_not_pool_safe},
    // GalacticTaskForceClass orders
    {M, "GalacticTaskForceClass", "Land_Units", m_tf_land_units},
    {M, "GalacticTaskForceClass", "Launch_Units", m_tf_launch_units},
    {M, "GalacticTaskForceClass", "Invade", m_tf_invade},
    {M, "GalacticTaskForceClass", "Raid", m_tf_raid},
    {M, "GalacticTaskForceClass", "Is_Raid_Capable", m_tf_is_raid_capable},
    {M, "GalacticTaskForceClass", "Force_Test_Space_Conflict", m_tf_force_test_space_conflict},
    // statuses
    {M, "AIBlockingStatus", "IsFinished", m_status_is_finished},
    {M, "AIBlockingStatus", "Result", m_status_result},
    {M, "AIBlockingStatus", "Is_Pool_Safe", m_not_pool_safe},
    // AI targets
    {M, "AITargetLocationWrapper", "Is_Valid", m_target_is_valid},
    {M, "AITargetLocationWrapper", "Get_Game_Object", m_target_game_object},
    {M, "AITargetLocationWrapper", "Get_Distance", m_target_distance},
    {M, "AITargetLocationWrapper", "Is_Pool_Safe", m_not_pool_safe},
    // Budget (Flush_Category: players family)
    {M, "BudgetWrapper", "Allocate_Resources", m_budget_allocate},
    {M, "BudgetWrapper", "Flush_All_Resources", m_budget_flush_all},
    {M, "BudgetWrapper", "Flush_Unallocated_Resources", m_budget_flush_unallocated},
    {M, "BudgetWrapper", "Get_Spendable_Resources", m_budget_spendable},
    {M, "BudgetWrapper", "Get_Unallocated_Resources", m_budget_unallocated},
    {M, "BudgetWrapper", "Give_Resources_To_Goal", m_budget_give},
    {M, "BudgetWrapper", "Take_Resources_From_Goal", m_budget_take},
    {M, "BudgetWrapper", "Wait_For_Spendable_Resources", m_budget_wait_spendable},
    {M, "BudgetWrapper", "Wait_For_Unallocated_Resources", m_budget_wait_unallocated},
    {M, "BudgetWrapper", "Is_Pool_Safe", m_not_pool_safe},
    // free store
    {M, "AIFreeStoreClass", "Get_Object_Count", m_fs_object_count},
    {M, "AIFreeStoreClass", "Is_Object_On_Free_Store", m_fs_has},
    {M, "AIFreeStoreClass", "Is_Pool_Safe", m_not_pool_safe},
    {M, "GalacticFreeStoreClass", "Get_Object_Count", m_fs_object_count},
    {M, "GalacticFreeStoreClass", "Is_Object_On_Free_Store", m_fs_has},
    {M, "GalacticFreeStoreClass", "Is_Unit_In_Transit", m_fs_in_transit},
    {M, "GalacticFreeStoreClass", "Is_Unit_Safe", m_fs_safe},
    {M, "GalacticFreeStoreClass", "Move_Object", m_fs_move_object},
    {M, "GalacticFreeStoreClass", "Is_Pool_Safe", m_not_pool_safe},
    // globals
    {G, "", "FindTarget", g_find_target},
    {M, "FindTargetClass", "Reachable_Target", m_reachable_target},
    {G, "", "_FindStageArea", g_find_stage_area},
};

#undef ALAMO_TF_METHODS

// GlobalValue.Get / Set: L1 final moved them to the remaining family (bind_script_remaining.cpp: GlobalValueClass
// extras, AI host first, then the session store, then a VM-local table).

void set_global(lua_State* L, std::string_view name) {  // value on top
    lua_pushlstring(L, name.data(), name.size());
    lua_insert(L, -2);
    lua_rawset(L, LUA_GLOBALSINDEX);
}

void push_arg(lua_State* L, LuaVm::Impl& vm, const AiEventArg& a) {
    switch (a.kind) {
        case AiEventArg::Number: lua_pushnumber(L, a.number); break;
        case AiEventArg::Boolean: lua_pushboolean(L, a.boolean ? 1 : 0); break;
        case AiEventArg::String: lua_pushlstring(L, a.text.data(), a.text.size()); break;
        case AiEventArg::Player:
            if (a.id >= 0)
                detail::push_player(L, vm, a.id);
            else
                lua_pushnil(L);
            break;
        case AiEventArg::Object: detail::push_object(L, vm, a.object); break;
        case AiEventArg::TaskForce:
            if (a.id >= 0)
                push_tf(L, a.id, a.tf_kind);
            else
                lua_pushnil(L);
            break;
        default: lua_pushnil(L); break;
    }
}

bool push_global_function(lua_State* L, std::string_view name) {
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_type(L, -1) == LUA_TFUNCTION) return true;
    lua_pop(L, 1);
    return false;
}

}  // namespace

std::span<const detail::ImplEntry> detail::impl_ai() { return kImpl; }

// L1P4_LUA_GAPS 1.1 (vm_internal.hpp). Error texts as the original's Activate_Ability_Galactic.
bool detail::galactic_ability_type_arg(Call& c, const char* who, uint32_t& filter) {
    filter = objects::kNone;
    if (c.nargs > 1) {
        c.vm->message(std::string("SCRIPT ERROR: ") + who + " -- invalid number of parameters. Expected 0 or 1, got " +
                      std::to_string(c.nargs));
        return false;
    }
    if (c.nargs == 0) return true;
    if (const Ud* t = c.userdata(1, detail::cls_type())) {
        filter = static_cast<uint32_t>(t->value);
        return true;
    }
    if (c.is_string(1)) {
        const objects::GameObjectTypes* types = c.vm->host.types;
        const objects::TypeIndex t = types ? types->raw().find(c.string(1)) : objects::kNone;  // case-insensitive
        if (t != objects::kNone) {
            filter = t;
            return true;
        }
        c.vm->message(std::string("SCRIPT ERROR: ") + who + " -- unknown object type " + std::string(c.string(1)));
        return false;
    }
    c.vm->message(std::string("SCRIPT ERROR: ") + who + " -- invalid type for parameter 1. Expected object type.");
    return false;
}

// OI lua (A1 ask 2): an AITargetLocationWrapper -> its game object, a task force -> its first live unit (the rule
// of the former Find_Nearest shim). 0 = not an AI wrapper; 1 = an AI wrapper (out = null when it has no live object
// or no host is attached).
int detail::ai_wrapper_object(Call& c, int i, objects::ObjectHandle& out) {
    out = {};
    if (!c.has(i) || lua_type(c.L, c.idx(i)) != LUA_TUSERDATA) return 0;
    const auto* ud = static_cast<const Ud*>(lua_touserdata(c.L, c.idx(i)));
    if (!ud || (ud->cls != cls_target() && !is_tf_class(ud->cls))) return 0;
    AiScriptHost* h = host_of(c.L);
    const objects::ObjectStore* st = c.vm->host.store;
    if (!h || !st) return 1;
    if (ud->cls == cls_target()) {
        if (h->target_valid(ud->id)) {
            const objects::ObjectHandle o = h->target_object(ud->id);
            if (o && st->alive(o)) out = o;
        }
        return 1;
    }
    for (const objects::ObjectHandle u : h->tf_units(ud->id))
        if (u && st->alive(u)) {
            out = u;
            break;
        }
    return 1;
}

bool detail::ai_target_position(Call& c, int i, Vec3& out) {
    if (!c.has(i) || lua_type(c.L, c.idx(i)) != LUA_TUSERDATA) return false;
    const auto* ud = static_cast<const Ud*>(lua_touserdata(c.L, c.idx(i)));
    if (!ud || ud->cls != cls_target()) return false;
    AiScriptHost* h = host_of(c.L);
    return h && h->target_valid(ud->id) && h->target_position(ud->id, out);
}

// ================================================================ host-side API

void attach_ai_host(LuaVm& vm, AiScriptHost* host, int32_t script_id) {
    lua_State* L = vm.state();
    lua_pushstring(L, kHostKey);
    if (host)
        lua_pushlightuserdata(L, host);
    else
        lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, kScriptKey);
    lua_pushnumber(L, script_id);
    lua_rawset(L, LUA_REGISTRYINDEX);
    if (host) install_global_value_table(vm);
}

AiScriptHost* ai_host(LuaVm& vm) { return host_of(vm.state()); }

// L1 final: GlobalValue is the engine userdata of its binding row (class GlobalValueClass, methods Get / Set), as
// install_globals creates it; this re-installs it (after a script or Flush_G replaced the global).
void install_global_value_table(LuaVm& vm) {
    lua_State* L = vm.state();
    Ud ud;
    ud.cls = cls("GlobalValueClass");
    ud.binding = detail::global_row("GlobalValue");
    if (ud.cls == detail::kNoClass) ud.cls = detail::cls_user_var();
    detail::push_engine_userdata(L, ud);
    set_global(L, "GlobalValue");
}

void set_task_force_global(LuaVm& vm, std::string_view name, int32_t tf, AiTaskForceKind kind) {
    lua_State* L = vm.state();
    if (tf < 0)
        lua_pushnil(L);
    else
        push_tf(L, tf, kind);
    set_global(L, name);
}

void set_ai_target_global(LuaVm& vm, std::string_view name, int32_t target) {
    lua_State* L = vm.state();
    push_target(L, target);
    set_global(L, name);
}

void set_budget_global(LuaVm& vm, int32_t script_id) {
    lua_State* L = vm.state();
    Ud ud;
    ud.cls = cls_budget();
    ud.id = script_id;
    detail::push_engine_userdata(L, ud);
    set_global(L, "Budget");
}

void set_free_store_global(LuaVm& vm, int32_t script_id, bool galactic) {
    lua_State* L = vm.state();
    Ud ud;
    ud.cls = cls_free_store(galactic);
    ud.id = script_id;
    detail::push_engine_userdata(L, ud);
    set_global(L, "FreeStore");
}

void set_player_global(LuaVm& vm, std::string_view name, int32_t player) {
    lua_State* L = vm.state();
    LuaVm::Impl* impl = impl_of(L);
    if (player < 0 || !impl)
        lua_pushnil(L);
    else
        detail::push_player(L, *impl, player);
    set_global(L, name);
}

void set_number_global(LuaVm& vm, std::string_view name, double value) {
    lua_State* L = vm.state();
    lua_pushnumber(L, value);
    set_global(L, name);
}

void set_bool_global(LuaVm& vm, std::string_view name, bool value) {
    lua_State* L = vm.state();
    lua_pushboolean(L, value ? 1 : 0);
    set_global(L, name);
}

void set_nil_global(LuaVm& vm, std::string_view name) {
    lua_State* L = vm.state();
    lua_pushnil(L);
    set_global(L, name);
}

bool dispatch_task_force_event(LuaVm& vm, int32_t tf, AiTaskForceKind kind, std::string_view tf_name,
                               std::string_view event, const std::vector<AiEventArg>& args, bool queued,
                               std::string* err) {
    lua_State* L = vm.state();
    LuaVm::Impl* impl = impl_of(L);
    if (!impl || impl->exit_requested) return false;
    const int top = lua_gettop(L);
    std::string handler = std::string(tf_name) + "_" + std::string(event);
    if (!push_global_function(L, handler)) {
        handler = "Default_" + std::string(event);
        if (!push_global_function(L, handler)) return false;
    }
    const int fn = lua_gettop(L);
    if (queued) {
        lua_newtable(L);
        push_tf(L, tf, kind);
        lua_rawseti(L, -2, 1);
        int n = 1;
        for (const AiEventArg& a : args) {
            push_arg(L, *impl, a);
            lua_rawseti(L, -2, ++n);
        }
        detail::queue_event(L, fn, lua_gettop(L));
        lua_settop(L, top);
        return true;
    }
    push_tf(L, tf, kind);
    for (const AiEventArg& a : args) push_arg(L, *impl, a);
    const int st = lua_pcall(L, static_cast<int>(args.size()) + 1, 0, 0);
    bool ok = true;
    if (st != 0) {
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        ok = false;
    }
    lua_settop(L, top);
    return ok;
}

bool call_global_function(LuaVm& vm, std::string_view name, const std::vector<AiEventArg>& args, std::string* err) {
    lua_State* L = vm.state();
    LuaVm::Impl* impl = impl_of(L);
    if (!impl) return false;
    const int top = lua_gettop(L);
    if (!push_global_function(L, name)) return false;
    for (const AiEventArg& a : args) push_arg(L, *impl, a);
    const int st = lua_pcall(L, static_cast<int>(args.size()), 0, 0);
    bool ok = true;
    if (st != 0) {
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
        ok = false;
    }
    lua_settop(L, top);
    return ok;
}

bool call_global_number(LuaVm& vm, std::string_view name, const std::vector<AiEventArg>& args, double& out,
                        std::string* err) {
    lua_State* L = vm.state();
    LuaVm::Impl* impl = impl_of(L);
    if (!impl) return false;
    const int top = lua_gettop(L);
    if (!push_global_function(L, name)) return false;
    for (const AiEventArg& a : args) push_arg(L, *impl, a);
    const int st = lua_pcall(L, static_cast<int>(args.size()), 1, 0);
    bool ok = false;
    if (st != 0) {
        if (err) *err = lua_tostring(L, -1) ? lua_tostring(L, -1) : "error";
    } else if (lua_type(L, -1) == LUA_TNUMBER) {  // anything else (nil, boolean) fails (AI_GALACTIC_TOKENS §6b)
        out = lua_tonumber(L, -1);
        ok = true;
    }
    lua_settop(L, top);
    return ok;
}

void reset_script_threads(LuaVm& vm) {
    lua_State* L = vm.state();
    LuaVm::Impl* impl = impl_of(L);
    if (!impl) return;
    for (auto& [id, t] : impl->threads) t.killed = true;
    impl->reap();
    impl->exit_requested = false;
}

void clear_script_logs(LuaVm& vm) {
    lua_State* L = vm.state();
    LuaVm::Impl* impl = impl_of(L);
    if (!impl) return;
    impl->messages.clear();
    impl->errors.clear();
}

bool global_is_nil(LuaVm& vm, std::string_view name) {
    lua_State* L = vm.state();
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool nil = lua_isnil(L, -1);
    lua_pop(L, 1);
    return nil;
}

bool global_bool(LuaVm& vm, std::string_view name, bool fallback) {
    lua_State* L = vm.state();
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    bool v = fallback;
    if (lua_type(L, -1) == LUA_TBOOLEAN) v = lua_toboolean(L, -1) != 0;
    else if (lua_type(L, -1) == LUA_TNUMBER) v = lua_tonumber(L, -1) != 0.0;
    lua_pop(L, 1);
    return v;
}

double global_number(LuaVm& vm, std::string_view name, double fallback) {
    lua_State* L = vm.state();
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    const double v = lua_type(L, -1) == LUA_TNUMBER ? lua_tonumber(L, -1) : fallback;
    lua_pop(L, 1);
    return v;
}

std::string global_string(LuaVm& vm, std::string_view name) {
    lua_State* L = vm.state();
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    std::string v;
    if (lua_type(L, -1) == LUA_TSTRING) v.assign(lua_tostring(L, -1), lua_strlen(L, -1));
    lua_pop(L, 1);
    return v;
}

namespace {
std::vector<std::string> strings_of_table(lua_State* L, int t) {
    std::vector<std::string> out;
    const int n = luaL_getn(L, t);
    for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L, t, i);
        if (lua_type(L, -1) == LUA_TSTRING) out.emplace_back(lua_tostring(L, -1), lua_strlen(L, -1));
        lua_pop(L, 1);
    }
    return out;
}
}  // namespace

std::vector<std::vector<std::string>> global_string_table_list(LuaVm& vm, std::string_view name) {
    lua_State* L = vm.state();
    std::vector<std::vector<std::string>> out;
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int t = lua_gettop(L);
    if (lua_type(L, t) == LUA_TTABLE) {
        const int n = luaL_getn(L, t);
        for (int i = 1; i <= n; ++i) {
            lua_rawgeti(L, t, i);
            if (lua_type(L, -1) == LUA_TTABLE) out.push_back(strings_of_table(L, lua_gettop(L)));
            lua_pop(L, 1);
        }
    }
    lua_settop(L, t - 1);
    return out;
}

std::vector<std::string> global_string_array(LuaVm& vm, std::string_view name) {
    lua_State* L = vm.state();
    lua_pushlstring(L, name.data(), name.size());
    lua_rawget(L, LUA_GLOBALSINDEX);
    const int t = lua_gettop(L);
    std::vector<std::string> out;
    if (lua_type(L, t) == LUA_TTABLE) out = strings_of_table(L, t);
    lua_settop(L, t - 1);
    return out;
}

}  // namespace alamo::lua

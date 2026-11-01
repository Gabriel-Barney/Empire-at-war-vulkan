// Binding family: tactical AI (roadmap A1 stage 2; alamo/lua/ai_tactical_bridge.hpp). Every binding forwards
// to the AiTacticalScriptHost attached to the VM (engine/ai/tactical implements it). Argument checks follow the
// vm_internal.hpp error discipline (Call::fail, never lua_error). Validation strings are the port's own unless
// docs/re/AI_TACTICAL.md / LUA_API.md give one.
#include <bit>
#include <string>
#include <vector>

#include "alamo/lua/ai_tactical_bridge.hpp"
#include "vm_internal.hpp"

namespace alamo::lua {

namespace {

using detail::Call;
using detail::Ud;

const char kTacticalHostKey[] = "alamo.ai.tactical.host";
const char kScriptKey[] = "alamo.ai.script";  // bind_ai.cpp: the calling script id (shared registry key)

AiTacticalScriptHost* host_of(lua_State* L) {
    lua_pushstring(L, kTacticalHostKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    void* p = lua_type(L, -1) == LUA_TLIGHTUSERDATA ? lua_touserdata(L, -1) : nullptr;
    lua_pop(L, 1);
    return static_cast<AiTacticalScriptHost*>(p);
}

int32_t script_of(lua_State* L) {
    lua_pushstring(L, kScriptKey);
    lua_rawget(L, LUA_REGISTRYINDEX);
    const int32_t id = lua_type(L, -1) == LUA_TNUMBER ? detail::to_int(lua_tonumber(L, -1)) : -1;
    lua_pop(L, 1);
    return id;
}

uint32_t cls(const char* name) { return detail::class_id(name); }
uint32_t cls_target() {
    static const uint32_t c = cls("AITargetLocationWrapper");
    return c;
}
uint32_t cls_position() {
    static const uint32_t c = cls("PositionWrapper");
    return c;
}
bool is_tf_class(uint32_t c) {
    static const uint32_t a = cls("TaskForceClass"), b = cls("GalacticTaskForceClass"), s = cls("SpaceTaskForceClass"),
                          l = cls("LandTaskForceClass");
    return c == a || c == b || c == s || c == l;
}

// PositionWrapper layout of bind_objects.cpp (x / y float bits in Ud::value, z float bits in Ud::id). Keep in
// sync with push_position / position_of there.
void push_position(lua_State* L, float x, float y, float z) {
    Ud u;
    u.cls = cls_position();
    u.value = static_cast<uint64_t>(std::bit_cast<uint32_t>(x)) | (static_cast<uint64_t>(std::bit_cast<uint32_t>(y)) << 32);
    u.id = std::bit_cast<int32_t>(z);
    detail::push_engine_userdata(L, u);
}

int push_status(Call& c, int32_t status) {
    if (status < 0) return push_nil(c);
    Ud ud;
    ud.cls = cls("AIBlockingStatus");
    ud.id = status;
    detail::push_engine_userdata(c.L, ud);
    return 1;
}
int push_object(Call& c, objects::ObjectHandle h) {
    if (!h) return push_nil(c);
    detail::push_object(c.L, *c.vm, h);
    return 1;
}

// Any wrapper argument -> AiArg (bind_ai.cpp's rule) plus PositionWrapper -> AiPoint.
void arg_of(Call& c, int i, AiArg& a, AiPoint& p) {
    a = AiArg{};
    p = AiPoint{};
    if (!c.has(i) || lua_type(c.L, c.idx(i)) != LUA_TUSERDATA) return;
    const auto* ud = static_cast<const Ud*>(lua_touserdata(c.L, c.idx(i)));
    if (!ud) return;
    if (ud->cls == detail::cls_game_object()) {
        if (detail::object_alive(*c.vm, *ud)) {
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
    } else if (ud->cls == cls_position()) {
        p.present = true;
        p.x = std::bit_cast<float>(static_cast<uint32_t>(ud->value & 0xFFFFFFFFu));
        p.y = std::bit_cast<float>(static_cast<uint32_t>(ud->value >> 32));
        p.z = std::bit_cast<float>(ud->id);
    }
}

const char* tf_class_name(Call& c) {
    static const uint32_t s = cls("SpaceTaskForceClass");
    return c.self->cls == s ? "SpaceTaskForceClass" : (c.self->cls == cls("LandTaskForceClass") ? "LandTaskForceClass" : "TaskForceClass");
}

// ---------------------------------------------------------------- task force orders

int tf_order(Call& c, AiTacticalOrder order, const char* name) {
    AiArg a;
    AiPoint p;
    arg_of(c, 1, a, p);
    if (!a.present && !p.present) return c.fail("%s::%s -- invalid target parameter.", tf_class_name(c), name);
    const double number = c.is_number(2) ? c.number(2) : -1.0;
    bool flag = false;
    for (int i = 2; i <= c.nargs; ++i)
        if (is_bool(c, i)) flag = lua_toboolean(c.L, c.idx(i)) != 0;
    AiTacticalScriptHost* h = host_of(c.L);
    if (!h) return push_nil(c);
    return push_status(c, h->tf_tactical_order(c.self->id, order, a, p, number, flag));
}
int m_move_to(Call& c) { return tf_order(c, AiTacticalOrder::MoveTo, "Move_To"); }
int m_attack_move(Call& c) { return tf_order(c, AiTacticalOrder::AttackMove, "Attack_Move"); }
int m_guard_target(Call& c) { return tf_order(c, AiTacticalOrder::GuardTarget, "Guard_Target"); }
int m_explore_area(Call& c) { return tf_order(c, AiTacticalOrder::ExploreArea, "Explore_Area"); }
int m_prepare_ambush(Call& c) { return tf_order(c, AiTacticalOrder::PrepareAmbush, "Prepare_Ambush"); }
// Fire_Special_Weapon(type_name, target): SCRIPT_GAPS §1.4 / §1.5 on the task force's units (unit-list order),
// through the L1/S4 wave-2b helper; player -1 = the first unit's owner.
int m_fire_special_weapon(Call& c) {
    static const uint32_t space_cls = cls("SpaceTaskForceClass");
    AiScriptHost* h = c.vm && c.vm->owner ? ai_host(*c.vm->owner) : nullptr;
    const std::vector<objects::ObjectHandle> units = h ? h->tf_units(c.self->id) : std::vector<objects::ObjectHandle>{};
    return detail::task_force_fire_special_weapon(c, c.self->cls == space_cls, units, -1);
}

int m_attack_target(Call& c) {
    AiArg a;
    AiPoint p;
    arg_of(c, 1, a, p);
    if (!a.present || (!a.object && a.target < 0))
        return c.fail("%s::Attack_Target -- only unit targets may be attacked; use Move_To instead.", tf_class_name(c));
    std::string hp;
    if (c.is_string(2)) hp = std::string(c.string(2));
    AiTacticalScriptHost* h = host_of(c.L);
    if (!h) return push_nil(c);
    return push_status(c, h->tf_attack_target(c.self->id, a, hp));
}

int m_reinforce(Call& c) {
    AiArg a;
    AiPoint p;
    arg_of(c, 1, a, p);
    if (!a.present && !p.present) return c.fail("%s::Reinforce -- invalid target parameter.", tf_class_name(c));
    const double wait = c.is_number(2) ? c.number(2) : -1.0;
    AiTacticalScriptHost* h = host_of(c.L);
    if (!h) return push_nil(c);
    const int32_t s = h->tf_reinforce(c.self->id, a, p, wait);
    if (s == -2) {
        c.vm->message(std::string(tf_class_name(c)) + "::Reinforce -- taskforce has no reinforcements");
        return push_nil(c);
    }
    return push_status(c, s);
}

int m_build_all(Call& c) {
    AiTacticalScriptHost* h = host_of(c.L);
    return push_status(c, h ? h->tf_build_all(c.self->id) : -1);
}

int m_build(Call& c) {
    if (!c.is_string(1)) return c.fail("%s::Build -- invalid type for parameter 1. Expected string.", tf_class_name(c));
    AiArg pad;
    AiPoint p;
    arg_of(c, 2, pad, p);
    AiTacticalScriptHost* h = host_of(c.L);
    return push_status(c, h ? h->tf_build(c.self->id, c.string(1), pad) : -1);
}

int m_reserved_pads(Call& c) {
    AiTacticalScriptHost* h = host_of(c.L);
    std::vector<objects::ObjectHandle> pads;
    if (h) pads = h->tf_reserved_pads(c.self->id);
    lua_newtable(c.L);
    int n = 0;
    for (const objects::ObjectHandle o : pads) {
        detail::push_object(c.L, *c.vm, o);
        if (lua_isnil(c.L, -1)) {
            lua_pop(c.L, 1);
            continue;
        }
        lua_rawseti(c.L, -2, ++n);
    }
    return 1;
}

int m_withdraw(Call& c) {
    AiTacticalScriptHost* h = host_of(c.L);
    return push_bool(c, h && h->tf_withdraw(c.self->id));
}
int m_threat_max(Call& c) {
    AiTacticalScriptHost* h = host_of(c.L);
    return push_num(c, h ? h->tf_self_threat(c.self->id, false) : 0.0);
}
int m_threat_sum(Call& c) {
    AiTacticalScriptHost* h = host_of(c.L);
    return push_num(c, h ? h->tf_self_threat(c.self->id, true) : 0.0);
}
int m_distance(Call& c) {
    AiArg a;
    AiPoint p;
    arg_of(c, 1, a, p);
    if (!a.present && !p.present) return c.fail("%s::Get_Distance -- invalid parameter 1.", tf_class_name(c));
    AiTacticalScriptHost* h = host_of(c.L);
    const double d = h ? h->tf_distance(c.self->id, a, p) : -1.0;
    return d < 0.0 ? push_nil(c) : push_num(c, d);
}
// Set_Targeting_Priorities(set [, type or category filter]) (LUA_API.md strings). OI lua: the units get the per-unit
// override through the objects family (S8 CombatSystem::set_priority_override for combat units,
// detail::task_force_set_targeting_priorities); the host keeps its record of the set.
int m_set_targeting_priorities(Call& c) {
    if (c.nargs != 1 && c.nargs != 2)
        return c.fail("Set_Targeting_Priorities - invalid number of parameters. Expected 1 or 2, got %d.", c.nargs);
    if (!c.is_string(1)) return c.fail("Set_Targeting_Priorities - invalid type for parameter 1. Expected string.");
    if (c.nargs == 2 && !c.is_string(2)) return c.fail("Set_Targeting_Priorities -- invalid type for parameter 2. Expected string.");
    AiScriptHost* ai = c.vm && c.vm->owner ? ai_host(*c.vm->owner) : nullptr;
    if (ai) detail::task_force_set_targeting_priorities(c, ai->tf_units(c.self->id));
    if (AiTacticalScriptHost* h = host_of(c.L)) h->tf_set_targeting_priorities(c.self->id, c.string(1));
    return 0;
}
int m_noop(Call&) { return 0; }

// ---------------------------------------------------------------- globals

// FindDeadlyEnemy(object | unit AI target | task force) or FindDeadlyEnemy(AI target, radius, player) (§6.3).
int g_find_deadly_enemy(Call& c) {
    if (c.nargs != 1 && c.nargs != 3)
        return c.fail("FindDeadlyEnemy -- invalid number of parameters. Expected 1 or 3, got %d.", c.nargs);
    AiArg a;
    AiPoint p;
    arg_of(c, 1, a, p);
    if (!a.present) return c.fail("FindDeadlyEnemy -- invalid type for parameter 1.");
    double radius = 0.0;
    int32_t player = -1;
    if (c.nargs == 3) {
        if (!c.is_number(2)) return c.fail("FindDeadlyEnemy -- invalid type for parameter 2. Expected number.");
        radius = c.number(2);
        AiArg pa;
        AiPoint pp;
        arg_of(c, 3, pa, pp);
        if (pa.player < 0) return c.fail("FindDeadlyEnemy -- invalid type for parameter 3. Expected player.");
        player = pa.player;
    }
    AiTacticalScriptHost* h = host_of(c.L);
    return push_object(c, h ? h->find_deadly_enemy(script_of(c.L), a, radius, player) : objects::ObjectHandle{});
}

// Get_Most_Defended_Position(source, player) (§6.4): exactly 2 arguments; no result -> the 1e18 position.
int g_most_defended_position(Call& c) {
    if (c.nargs != 2) return c.fail("Get_Most_Defended_Position -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    AiArg a;
    AiPoint p;
    arg_of(c, 1, a, p);
    if (!a.present && !p.present) return c.fail("Get_Most_Defended_Position -- invalid type for parameter 1.");
    AiArg pa;
    AiPoint pp;
    arg_of(c, 2, pa, pp);
    if (pa.player < 0) return c.fail("Get_Most_Defended_Position -- invalid type for parameter 2. Expected player.");
    AiTacticalScriptHost* h = host_of(c.L);
    AiPoint out;
    if (!h || !h->most_defended_position(script_of(c.L), a, p, pa.player, out)) {
        constexpr float kNone = 1.0e18f;
        push_position(c.L, kNone, kNone, kNone);
        return 1;
    }
    push_position(c.L, out.x, out.y, out.z);
    return 1;
}

// Project_By_Unit_Range(unit, position source) (AI_TACTICAL_PERCEPTION.md §5).
int g_project_by_unit_range(Call& c) {
    if (c.nargs != 2) return c.fail("Project_By_Unit_Range -- invalid number of parameters. Expected 2, got %d.", c.nargs);
    AiArg u, a;
    AiPoint up, p;
    arg_of(c, 1, u, up);
    arg_of(c, 2, a, p);
    if (!u.object) return c.fail("Project_By_Unit_Range -- invalid type for parameter 1. Expected a game object.");
    if (!a.present && !p.present) return c.fail("Project_By_Unit_Range -- invalid type for parameter 2.");
    AiTacticalScriptHost* h = host_of(c.L);
    AiPoint out;
    if (!h || !h->project_by_unit_range(u.object, a, p, out)) return push_nil(c);
    push_position(c.L, out.x, out.y, out.z);
    return 1;
}

int m_is_good_against(Call& c) {
    if (c.nargs != 1) return c.fail("GameObjectWrapper::Is_Good_Against -- invalid number of parameters. Expected 1, got %d.", c.nargs);
    AiArg a;
    AiPoint p;
    arg_of(c, 1, a, p);
    if (!a.object) return c.fail("GameObjectWrapper::Is_Good_Against -- invalid type for parameter 1. Expected a game object.");
    if (!detail::object_alive(*c.vm, *c.self)) return c.fail("GameObjectWrapper::Is_Good_Against -- this object is dead!");
    AiTacticalScriptHost* h = host_of(c.L);
    return push_bool(c, h && h->is_good_against(detail::object_handle(*c.self), a.object) == 1);
}

// unit.Should_Switch_Weapons(target): body not traced (AI_TACTICAL.md open item 6) -> false [port].
int m_should_switch_weapons(Call& c) {
    if (!detail::object_alive(*c.vm, *c.self)) return c.fail("GameObjectWrapper::Should_Switch_Weapons -- this object is dead!");
    return push_bool(c, false);
}

constexpr BindingKind G = BindingKind::Global;
constexpr BindingKind M = BindingKind::Method;

#define ALAMO_TACTICAL_TF_METHODS(K)                                     \
    {M, K, "Move_To", m_move_to},                                        \
    {M, K, "Attack_Move", m_attack_move},                                \
    {M, K, "Attack_Target", m_attack_target},                            \
    {M, K, "Guard_Target", m_guard_target},                              \
    {M, K, "Explore_Area", m_explore_area},                              \
    {M, K, "Prepare_Ambush", m_prepare_ambush},                          \
    {M, K, "Fire_Special_Weapon", m_fire_special_weapon},                \
    {M, K, "Reinforce", m_reinforce},                                    \
    {M, K, "Build_All", m_build_all},                                    \
    {M, K, "Build", m_build},                                            \
    {M, K, "Get_Reserved_Build_Pads", m_reserved_pads},                  \
    {M, K, "Get_Distance", m_distance},                                  \
    {M, K, "Enable_Attack_Positioning", m_noop},                         \
    {M, K, "Release_Reinforcements", m_noop}

constexpr detail::ImplEntry kImpl[] = {
    ALAMO_TACTICAL_TF_METHODS("LandTaskForceClass"),
    ALAMO_TACTICAL_TF_METHODS("SpaceTaskForceClass"),
    {M, "TaskForceClass", "Get_Self_Threat_Max", m_threat_max},
    {M, "TaskForceClass", "Get_Self_Threat_Sum", m_threat_sum},
    {M, "TaskForceClass", "Withdraw_Units", m_withdraw},
    {M, "TaskForceClass", "Set_Targeting_Priorities", m_set_targeting_priorities},
    {G, "", "FindDeadlyEnemy", g_find_deadly_enemy},
    {G, "", "Get_Most_Defended_Position", g_most_defended_position},
    {G, "", "Project_By_Unit_Range", g_project_by_unit_range},
    {M, "GameObjectWrapper", "Is_Good_Against", m_is_good_against},
    {M, "GameObjectWrapper", "Should_Switch_Weapons", m_should_switch_weapons},
};

#undef ALAMO_TACTICAL_TF_METHODS

}  // namespace

std::span<const detail::ImplEntry> detail::impl_ai_tactical() { return kImpl; }

// OI lua (A1 ask 2): the players family's EvaluatePerception forwards here while a tactical host is attached (what the
// EvaluatePerception shim did in the AI VMs). -2 = no tactical host (the caller keeps its own path).
int detail::tactical_evaluate_perception(Call& c, std::string_view name, int32_t player, int target_index) {
    AiTacticalScriptHost* h = host_of(c.L);
    if (!h) return -2;
    AiArg t;
    AiPoint p;
    if (target_index > 0) arg_of(c, target_index, t, p);
    double v = 0.0;
    if (!h->evaluate_perception(script_of(c.L), name, player, t, v)) return push_nil(c);
    return push_num(c, v);
}

void attach_ai_tactical_host(LuaVm& vm, AiTacticalScriptHost* host) {
    lua_State* L = vm.state();
    lua_pushstring(L, kTacticalHostKey);
    if (host)
        lua_pushlightuserdata(L, host);
    else
        lua_pushnil(L);
    lua_rawset(L, LUA_REGISTRYINDEX);
}

AiTacticalScriptHost* ai_tactical_host(LuaVm& vm) { return host_of(vm.state()); }

}  // namespace alamo::lua

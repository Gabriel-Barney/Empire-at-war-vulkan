// Lua bridge, tactical AI family (roadmap A1 stage 2; docs/re/AI_TACTICAL.md §6, §7): the land / space
// task-force orders, the tactical query globals and the tactical perception entry (src/bind_ai_tactical.cpp).
// The plan-generic bindings (TaskForceClass, AITargetLocationWrapper, FindTarget, free stores, statuses) are
// A2's family (ai_bridge.hpp); a tactical AI host implements both interfaces and attaches both.
//
// Bound here:
//   LandTaskForceClass / SpaceTaskForceClass: Attack_Move, Attack_Target, Guard_Target, Explore_Area,
//     Reinforce, Build_All, Build, Get_Reserved_Build_Pads, Fire_Special_Weapon, Prepare_Ambush,
//     Get_Distance, Enable_Attack_Positioning, Release_Reinforcements;
//   TaskForceClass: Get_Self_Threat_Max, Get_Self_Threat_Sum, Withdraw_Units, Set_Targeting_Priorities;
//   globals: FindDeadlyEnemy, Get_Most_Defended_Position, Project_By_Unit_Range;
//   GameObjectWrapper.Is_Good_Against, GameObjectWrapper.Should_Switch_Weapons (false: open item 6).
// Extra methods (not in the RE binding table, counted as "extra" by binding_coverage):
//   * LandTaskForceClass / SpaceTaskForceClass.Move_To: shadows A2's TaskForceClass.Move_To so tactical
//     task forces accept positions and task forces as destinations (§7.3); the threat / zone-path
//     arguments are passed to the host.
// The tactical EvaluatePerception (§6.5: Self / Enemy = context opponent / Human / Target = an AITargetLocation or a
// game object) is the players family's EvaluatePerception, which forwards to this host whenever one is attached;
// Find_Nearest and the other objects-family position arguments resolve AI targets / task forces themselves (the
// former port Lua shim and PlayerWrapper.Evaluate_Tactical_Perception are gone).
// Error discipline as vm_internal.hpp (Call::fail, never lua_error). Without an attached host the bindings
// answer as an empty AI would (nil statuses / results).
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/lua/ai_bridge.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo::lua {

class LuaVm;

// A destination / position argument: a PositionWrapper, a game object, an AI target or a task force.
struct AiPoint {
    bool present = false;   // a PositionWrapper (x / y / z valid)
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// TaskForce order kinds of the tactical family (each returns a blocking status id or -1 = nil).
enum class AiTacticalOrder : uint8_t { MoveTo, AttackMove, GuardTarget, ExploreArea, PrepareAmbush, FireSpecialWeapon };

class AiTacticalScriptHost {
public:
    virtual ~AiTacticalScriptHost() = default;

    // ---- task force orders (§7.3) ----
    // `dest`: AiArg (object / target / task force) or `point` (PositionWrapper). `number` = the optional
    // numeric argument (threat tolerance), `flag` = the optional boolean (Move_To useZonePath).
    virtual int32_t tf_tactical_order(int32_t tf, AiTacticalOrder order, const AiArg& dest, const AiPoint& point,
                                      double number, bool flag) = 0;
    // Attack_Target(unit [, hardpoint]): `hardpoint` = "" (none), "Random" or a hard-point type name.
    virtual int32_t tf_attack_target(int32_t tf, const AiArg& target, std::string_view hardpoint) = 0;
    // Reinforce(target [, max wait]): -2 = "taskforce has no reinforcements" (nil + script message).
    virtual int32_t tf_reinforce(int32_t tf, const AiArg& target, const AiPoint& point, double max_wait) = 0;
    virtual int32_t tf_build_all(int32_t tf) = 0;
    virtual int32_t tf_build(int32_t tf, std::string_view type_name, const AiArg& pad) = 0;
    virtual std::vector<objects::ObjectHandle> tf_reserved_pads(int32_t tf) = 0;
    virtual bool tf_withdraw(int32_t tf) = 0;
    virtual double tf_self_threat(int32_t tf, bool sum) = 0;
    virtual double tf_distance(int32_t tf, const AiArg& other, const AiPoint& point) = 0;  // < 0 = unknown
    virtual void tf_set_targeting_priorities(int32_t tf, std::string_view set) = 0;

    // ---- globals (§6.3-6.5) ----
    // FindDeadlyEnemy(x [, radius, player]) -> the attacker object (null = nil).
    virtual objects::ObjectHandle find_deadly_enemy(int32_t script, const AiArg& source, double radius, int32_t player) = 0;
    // Get_Most_Defended_Position(source, player) -> position (false = the 1e18 sentinel position).
    virtual bool most_defended_position(int32_t script, const AiArg& source, const AiPoint& point, int32_t player,
                                        AiPoint& out) = 0;
    // Project_By_Unit_Range(unit, position source) -> position (false = nil).
    virtual bool project_by_unit_range(objects::ObjectHandle unit, const AiArg& other, const AiPoint& point, AiPoint& out) = 0;
    // unit.Is_Good_Against(other): 1 / 0, -1 unknown (false).
    virtual int is_good_against(objects::ObjectHandle unit, objects::ObjectHandle other) = 0;
    // EvaluatePerception(name, player [, target]) in the tactical context. False -> nil.
    virtual bool evaluate_perception(int32_t script, std::string_view name, int32_t player, const AiArg& target,
                                     double& out) = 0;

    // ---- L1 final (script_remaining.hpp). Defaults: -2 = not handled here (the binding then uses the mode's
    // RemainingScriptHooks; without them it answers nil) ----
    // LandTaskForceClass.Bombing_Run(target) / Fire_Orbital_Bombardment(target or position) -> status id, -1 = nil.
    virtual int32_t tf_bombing_run(int32_t /*tf*/, const AiArg& /*target*/, const AiPoint& /*point*/) { return -2; }
    virtual int32_t tf_orbital_bombardment(int32_t /*tf*/, const AiArg& /*target*/, const AiPoint& /*point*/) { return -2; }
};

// Makes `host` reachable from `vm`'s tactical bindings (nullptr detaches). Owner thread only.
void attach_ai_tactical_host(LuaVm& vm, AiTacticalScriptHost* host);
AiTacticalScriptHost* ai_tactical_host(LuaVm& vm);

}  // namespace alamo::lua

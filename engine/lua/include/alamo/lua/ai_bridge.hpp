// Lua bridge, AI family (roadmap A2; docs/re/AI_GALACTIC.md §1.9-1.10, §2.5, §2.8): the plan-facing
// bindings - TaskForce / GalacticTaskForce methods, AITarget wrappers, the plan's Budget, the free store,
// FindTarget, _FindStageArea and the AI blocking statuses (src/bind_ai.cpp). WaitForStarbase /
// WaitForGroundbase belong to the players family (bind_players.cpp).
//
// engine/lua does not know the AI: the AI runtime (engine/ai, alamo_ai_lua) implements AiScriptHost and
// attaches it to every plan / free-store script VM with attach_ai_host(vm, host, script id). The script
// id tells the host which plan (or free store) is calling (Budget, free store, FindTarget sources).
// Everything here runs on the VM's owner thread (the sim thread), in the scripts' own call order.
//
// Userdata the family creates (cached per VM, so scripts can key tables by them):
//   TaskForce          class GalacticTaskForceClass / SpaceTaskForceClass / LandTaskForceClass /
//                      TaskForceClass, Ud.id = task force id
//   AITarget           class AITargetLocationWrapper, Ud.id = target id
//   Budget             class BudgetWrapper, Ud.id = script id (new per plan instantiation)
//   FreeStore          class GalacticFreeStoreClass (galactic) / AIFreeStoreClass, Ud.id = script id
//   blocking statuses  class AIBlockingStatus (IsFinished / Result), Ud.id = status id (value object)
// GlobalValue (class-less in the binding table) is an engine userdata of the port class GlobalValueClass with the
// methods Get / Set (L1 final, script_remaining.hpp), backed by the attached host's session-wide store [port shim:
// the original's GlobalValue object is not documented].
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/objects/object_store.hpp"

namespace alamo::lua {

class LuaVm;

enum class AiTaskForceKind : uint8_t { Generic = 0, Galactic = 1, Space = 2, Land = 3 };

// A script argument as the host sees it (whichever of these the value was).
struct AiArg {
    bool present = false;
    int32_t target = -1;          // AITargetLocationWrapper
    objects::ObjectHandle object;  // GameObjectWrapper
    int32_t task_force = -1;      // a TaskForce
    int32_t player = -1;          // PlayerWrapper
};

enum class AiStatusResult : uint8_t { Nil = 0, True = 1, False = 2 };
inline constexpr int32_t kAiNotAPlanet = -2;  // tf_produce_force: the stage argument is not a planet

// TaskForce orders returning a blocking status (or nil): GalacticTaskForceClass + TaskForceClass.
enum class AiTfOrder : uint8_t { MoveTo, LandUnits, LaunchUnits, Invade, FormUnits, Raid, ForceTestSpaceConflict, ActivateAbility };
enum class AiTfFlag : uint8_t { PlanResult, Removable, TestTargetContrast, BlockGoalProposal };
enum class AiBudgetOp : uint8_t {
    Allocate, FlushAll, FlushUnallocated, Spendable, Unallocated, GiveToGoal, TakeFromGoal, WaitSpendable, WaitUnallocated
};

// FindTarget(source, perception, application flags, [reachability,] probability, [source object], [max distance])
struct AiFindTargetRequest {
    AiArg source;                 // task force, player or object the search starts from
    std::string perception;
    std::string application;      // "Friendly | Enemy" ...
    std::string reachability;     // Reachable_Target only ("" = Any)
    double probability = 1.0;     // chance of taking the best target
    AiArg relative;               // relative source (Reachable_Target param 6)
    double max_distance = -1.0;
};

struct AiGlobalValue {
    enum Kind : uint8_t { Nil = 0, Number = 1, String = 2, Boolean = 3, Table = 4 } kind = Nil;
    double number = 0.0;
    std::string text;
    // L1 final (L1_S4_REMAINING §1.5: tables are stored too): Table entries, keys[i] -> values[i], in the source
    // table's next() order (deterministic with the serial patch).
    std::vector<AiGlobalValue> keys, values;
};

class AiScriptHost {
public:
    virtual ~AiScriptHost() = default;

    // ---- task forces ----
    virtual bool tf_valid(int32_t tf) = 0;
    virtual int32_t tf_force_count(int32_t tf) = 0;
    virtual void tf_set_flag(int32_t tf, AiTfFlag flag, bool value) = 0;
    // Produce_Force([stage], [synchronized]) -> status id (-1 = nil). `stage.present` is false only with no argument (the
    // binding raises the script error for nil / non-wrapper values). kAiNotAPlanet: the argument resolved to no planet,
    // so the binding raises "Expected a planet object for parameter 1" (a2m t05, A2M_T04_RE_ANSWERS Q4.1).
    virtual int32_t tf_produce_force(int32_t tf, const AiArg& stage, bool synchronized) = 0;
    virtual int32_t tf_get_stage(int32_t tf) = 0;  // target id, -1 = nil
    virtual bool tf_all_on_free_store(int32_t tf) = 0;
    // An order -> status id (-1 = nil).
    virtual int32_t tf_order(int32_t tf, AiTfOrder order, const AiArg& arg) = 0;
    virtual bool tf_is_raid_capable(int32_t tf) = 0;
    virtual void tf_release_forces(int32_t tf, double fraction) = 0;
    virtual void tf_release_unit(int32_t tf, objects::ObjectHandle unit) = 0;
    virtual std::vector<objects::ObjectHandle> tf_units(int32_t tf) = 0;
    virtual std::string tf_goal_type_name(int32_t tf) = 0;
    // Collect_All_Free_Units([category]) (0x5fc040): `category_mask` = the GameObjectCategoryType bits of the argument,
    // ~0 without one (a1k t01, A1_READINGS §39.3). The host takes the free units whose type matches.
    virtual bool tf_collect_free_units(int32_t tf, uint64_t category_mask) = 0;

    // ---- blocking statuses ----
    virtual bool status_finished(int32_t status) = 0;
    virtual AiStatusResult status_result(int32_t status) = 0;

    // ---- targets ----
    virtual bool target_valid(int32_t target) = 0;
    virtual objects::ObjectHandle target_object(int32_t target) = 0;
    virtual double target_distance(int32_t target, const AiArg& other) = 0;  // < 0 = unknown
    // The target's position when it is not a unit (a region / marker target), for "something which defines a
    // position" arguments [port]. False = none (the default).
    virtual bool target_position(int32_t /*target*/, Vec3& /*out*/) { return false; }

    // ---- the calling plan's budget ----
    virtual double budget_op(int32_t script, AiBudgetOp op, double amount, std::string_view goal_type) = 0;
    virtual int32_t budget_wait(int32_t script, AiBudgetOp op, double amount) = 0;  // status id

    // ---- free store ----
    // which: -1 every unit, 1 space units, 0 ground units.
    virtual int32_t free_store_count(int32_t script, int which) = 0;
    virtual bool free_store_has(int32_t script, objects::ObjectHandle unit) = 0;
    virtual bool unit_in_transit(objects::ObjectHandle unit) = 0;
    virtual bool unit_safe(objects::ObjectHandle unit) = 0;
    virtual bool free_store_move(int32_t script, objects::ObjectHandle unit, const AiArg& destination) = 0;

    // ---- globals ----
    virtual int32_t find_target(int32_t script, const AiFindTargetRequest& request) = 0;  // target id, -1 = nil
    // A1 (tiny marked fix): plain FindTarget returns the GAME OBJECT of a unit target (AI_TACTICAL.md 6.1).
    // Hosts return the object for such targets; null (the default, galactic) keeps the AITargetLocationWrapper.
    virtual objects::ObjectHandle find_target_object(int32_t /*target*/) { return {}; }
    // _FindStageArea(player, target, taskforce) -> target id (-1 = nil).
    virtual int32_t find_stage_area(int32_t script, const AiArg& player, const AiArg& target, const AiArg& tf) = 0;
    virtual AiGlobalValue global_value(std::string_view name) = 0;
    virtual void set_global_value(std::string_view name, const AiGlobalValue& value) = 0;

    // ---- L1 final (script_remaining.hpp). Defaults = "not available": hosts override what they support ----
    // TaskForceClass.Add_Force(unit): true = the unit joined the task force.
    virtual bool tf_add_unit(int32_t /*tf*/, objects::ObjectHandle /*unit*/) { return false; }
    // Add / Remove / Clear_Opportunity_Fire_Event_Subscription(s) (L1_S4_REMAINING §2.12): the TF's list of weapon
    // HardPointType values (5..10). op 0 add (1 = already present), 1 remove (1 = not present), 2 clear. The default
    // keeps the lists here (consumer not traced: the Hardpoint_Opportunity_Target_Acquired filter [M]).
    virtual int tf_opportunity_fire_subscription(int32_t tf, int op, int32_t hardpoint_type) {
        std::vector<int32_t>& l = opportunity_fire_[tf];
        const auto it = std::find(l.begin(), l.end(), hardpoint_type);
        if (op == 2) {
            l.clear();
            return 0;
        }
        if (op == 0) {
            if (it != l.end()) return 1;
            l.push_back(hardpoint_type);
            return 0;
        }
        if (it == l.end()) return 1;
        l.erase(it);
        return 0;
    }
    const std::vector<int32_t>* opportunity_fire_subscriptions(int32_t tf) const {
        const auto it = opportunity_fire_.find(tf);
        return it == opportunity_fire_.end() ? nullptr : &it->second;
    }
    // GalacticTaskForceClass.Refit_To_Definition(planet, budget) (AI_GALACTIC.md 2.4, L1_S4_REMAINING §2.12): status
    // id; -1 = nil, -3 = the TF definition is not percentage based (script error).
    virtual int32_t tf_refit(int32_t /*tf*/, objects::ObjectHandle /*planet*/, double /*budget*/) { return -1; }
    // FindTarget.Best_Of(tf, targets, perception): index into `targets` of the best one (-1 = nil).
    virtual int32_t best_of(int32_t /*script*/, int32_t /*tf*/, const std::vector<AiArg>& /*targets*/,
                            std::string_view /*perception*/) {
        return -1;
    }
    // Get_AI_Power_Vs_Unit (object or task force `self` against an object / AI target): < 0 = unknown.
    virtual double ai_power_vs(const AiArg& /*self*/, const AiArg& /*other*/) { return -1.0; }
    // GameObjectWrapper.Explore_Area(target) -> status id (-1 = nil).
    virtual int32_t unit_explore_area(int32_t /*script*/, objects::ObjectHandle /*unit*/, int32_t /*target*/) { return -1; }
    // OI lua: TaskForceClass / GalacticTaskForceClass.Activate_Ability([name] [, on | target]) -> status id (-1 = nil).
    // `ability` = parameter 1 when a string ("" = none), `on` = the boolean argument (true without one), `target` = the
    // wrapper argument after the name. Land / Space / generic task forces: the binding has ALREADY called each unit's
    // GameObjectWrapper.Activate_Ability with the script's arguments (the tactical ability path, AI_TACTICAL §7.3), so the
    // host only answers the status and must not activate again. Galactic task forces: the host fires the Ground_Activated
    // trigger per member (P4_SIM_SYSTEMS 6.1); `ability` is then the type name of the optional object-type argument
    // (L1P4_LUA_GAPS 1.1, "" = no filter) and the script gets no return value (the status id is dropped, 1.2 item 4).
    // Default: tf_order(ActivateAbility, target).
    virtual int32_t tf_activate_ability(int32_t tf, std::string_view /*ability*/, bool /*on*/, const AiArg& target) {
        return tf_order(tf, AiTfOrder::ActivateAbility, target);
    }
    // S7 t15 A1 (AI_TACTICAL §7.3, MOVEMENT §3.1.1 step 6): the land / space / generic form after the binding called each
    // unit's Activate_Ability: `units` = the task force's units (list order) with whether the activation went through.
    // A tactical host answers an AbilityBlockStatus (pending until every activated unit raised "unit ability finished" or
    // died / was removed) and queues Unit_Ability_Finished at once for the units that failed. Default: tf_activate_ability.
    virtual int32_t tf_activate_ability_units(int32_t tf, std::string_view ability, bool on, const AiArg& target,
                                              const std::vector<std::pair<objects::ObjectHandle, bool>>& /*units*/) {
        return tf_activate_ability(tf, ability, on, target);
    }
    // S7 t20 (AI_TACTICAL §7.3.2): the task force's name for the retail script-error texts ("" = unknown).
    virtual std::string tf_name(int32_t /*tf*/) { return {}; }

private:
    std::map<int32_t, std::vector<int32_t>> opportunity_fire_;  // TF id -> subscribed HardPointType values (lookups only)
};

// Makes `host` reachable from `vm`'s AI bindings, with the calling script's id (nullptr detaches) and
// installs the GlobalValue table. Owner thread only.
void attach_ai_host(LuaVm& vm, AiScriptHost* host, int32_t script_id);
AiScriptHost* ai_host(LuaVm& vm);
// Re-installs the GlobalValue object (a script may have replaced it; the name is kept for the AI runtime).
void install_global_value_table(LuaVm& vm);

// Host-value globals of a plan / free store, as userdata of this family.
void set_task_force_global(LuaVm& vm, std::string_view name, int32_t tf, AiTaskForceKind kind);
void set_ai_target_global(LuaVm& vm, std::string_view name, int32_t target);  // -1 -> nil
void set_budget_global(LuaVm& vm, int32_t script_id);                       // "Budget"
void set_free_store_global(LuaVm& vm, int32_t script_id, bool galactic);    // "FreeStore"
void set_player_global(LuaVm& vm, std::string_view name, int32_t player);   // "PlayerObject" (-1 -> nil)
void set_number_global(LuaVm& vm, std::string_view name, double value);
void set_bool_global(LuaVm& vm, std::string_view name, bool value);
void set_nil_global(LuaVm& vm, std::string_view name);

// Task-force event dispatch (AI_GALACTIC.md §1.9.4 "Events"): the handler <tf_name>_<event>, else
// Default_<event>; arguments [task force, args...]. queued: appended to the VM's GetEvent queue (run by
// the next PumpEvents); else called at once (protected). Skipped when the script is exiting. Returns
// false when no handler exists. Errors go to *err.
struct AiEventArg {
    enum Kind : uint8_t { Nil, Number, Boolean, String, Player, Object, TaskForce, Type } kind = Nil;
    double number = 0.0;
    bool boolean = false;
    std::string text;
    int32_t id = -1;  // player / task force id
    objects::ObjectHandle object;
    AiTaskForceKind tf_kind = AiTaskForceKind::Galactic;
    uint32_t type = 0xFFFFFFFFu;  // Type: GameObjectTypeWrapper of this type index (out of range -> nil; q6 t09)
};
bool dispatch_task_force_event(LuaVm& vm, int32_t tf, AiTaskForceKind kind, std::string_view tf_name,
                               std::string_view event, const std::vector<AiEventArg>& args, bool queued,
                               std::string* err = nullptr);
// Calls the global function `name` with args (protected, not as a thread). False when missing / on error.
bool call_global_function(LuaVm& vm, std::string_view name, const std::vector<AiEventArg>& args,
                          std::string* err = nullptr);
// A2 stage 2 (Script_ perception references): calls `name` with args and returns its result; true only when
// the call succeeded and the first result is a Lua number.
bool call_global_number(LuaVm& vm, std::string_view name, const std::vector<AiEventArg>& args, double& out,
                        std::string* err = nullptr);

// Script pool support (AI_GALACTIC.md §1.9.1 "Script pool"): kills every thread of the VM, clears the
// exit flag, the message / error logs and the GetEvent queue state the host owns. Owner thread only.
void reset_script_threads(LuaVm& vm);
void clear_script_logs(LuaVm& vm);

// Reading the plan definition globals after Base_Definitions (AI_GALACTIC.md §1.9.1).
bool global_is_nil(LuaVm& vm, std::string_view name);
bool global_bool(LuaVm& vm, std::string_view name, bool fallback);
double global_number(LuaVm& vm, std::string_view name, double fallback);
std::string global_string(LuaVm& vm, std::string_view name);
// TaskForce = { {"Name", "lhs = rhs", ...}, ... }: every inner table's strings, in index order.
std::vector<std::vector<std::string>> global_string_table_list(LuaVm& vm, std::string_view name);
// A plain array of strings (RequiredCategories), index order; non-strings skipped.
std::vector<std::string> global_string_array(LuaVm& vm, std::string_view name);

}  // namespace alamo::lua

// engine/lua L1/S4 wave 2a: the object bindings on a tactical host's systems (object_bridge.hpp) - the S9
// AbilitySystem (Has / Is_Ability_Active / Ready / Autofire, Activate_Ability families, Cancel_Ability,
// Force_Ability_Recharge, Reset_Ability_Counter, autofire setters, Get_Hull / Get_Shield, Are_Engines_Online,
// Is_Under_Effects_Of_Ability), the R3 AnimWorld (Play_Animation, Get_Bone_Position), the type query
// Is_Affected_By_Missile_Shield, and the single UnitAbilityType name table. Parts need engine/tactical/abilities
// or engine/anim in the build (ALAMO_LUA_HAVE_ABILITIES / ALAMO_LUA_HAVE_ANIM); they are skipped otherwise.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/test/support.hpp"
#if ALAMO_LUA_HAVE_ABILITIES
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#endif
#if ALAMO_LUA_HAVE_ANIM
#include "alamo/anim/anim_state.hpp"
#include "alamo/anim/anim_types.hpp"
#include "alamo/anim/clip.hpp"
#endif

#include <lua.h>

using namespace alamo;
using namespace alamo::lua;
namespace fs = std::filesystem;

using ::alamo::test::g_fail;

static std::string run(LuaVm& vm, const std::string& code) {
    std::string err;
    if (!vm.run(code, "test", &err)) return err.empty() ? "error" : err;
    return {};
}
// q6 t11 (docs/re/LUA_API.md "Binding error model across all bindings"): a binding error does not unwind; it logs
// "SCRIPT ERROR: ..." and sets the script's stop flag. run_stop() reports those lines as its result (a Lua error as
// before) and clears the flag, so the next snippet runs on a live script.
static std::string run_stop(LuaVm& vm, const std::string& code) {
    std::string err;
    const size_t m0 = vm.messages().size();
    if (!vm.run(code, "test", &err)) return err.empty() ? "error" : err;
    if (!vm.exit_requested()) return {};
    std::string out;
    for (size_t i = m0; i < vm.messages().size(); ++i)
        if (vm.messages()[i].rfind("SCRIPT ERROR: ", 0) == 0) out += vm.messages()[i] + "\n";
    reset_script_threads(vm);
    return out.empty() ? std::string("stop") : out;
}
static bool gtrue(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool b = lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return b;
}
static bool gnil(LuaVm& vm, const char* name) {  // S7 t26
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool b = lua_isnil(L, -1);
    lua_pop(L, 1);
    return b;
}
static double gnum(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const double d = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return d;
}
using ::alamo::test::write_file;

struct Db {
    fs::path root;
    std::unique_ptr<DirectoryProvider> dp;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    bool ok = false;
    Db() {
        root = fs::temp_directory_path() / "alamo_lua_tactical_family_test";
        std::error_code ec;
        remove_all_long(root, ec);
        write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>T.xml</File></L>");
        write_file(root / "Data/XML/T.xml",
                   "<R>\n"
                   "<SpaceUnit Name=\"Ship\"><Tactical_Health>1000</Tactical_Health><Shield_Points>500</Shield_Points>"
                   "<Behavior>STARSHIP_LOCOMOTOR</Behavior>"
                   "<Unit_Abilities_Data SubObjectList=\"Yes\">"
                   "<Unit_Ability><Type>DEFEND</Type></Unit_Ability><Unit_Ability><Type>SPOILER_LOCK</Type></Unit_Ability>"
                   "</Unit_Abilities_Data></SpaceUnit>\n"
                   "<SpaceUnit Name=\"Target_Ship\"><Tactical_Health>400</Tactical_Health></SpaceUnit>\n"
                   "<SpaceUnit Name=\"Hauler\"><Tactical_Health>200</Tactical_Health></SpaceUnit>\n"
                   "<Projectile Name=\"Rocket_P\"><Projectile_Category>ROCKET</Projectile_Category></Projectile>\n"
                   "<Projectile Name=\"Laser_P\"><Projectile_Category>LASER</Projectile_Category></Projectile>\n"
                   "</R>");
        dp = std::make_unique<DirectoryProvider>(root.string());
        ok = raw.load(*dp) && types.build(raw);
    }
    ~Db() {
        std::error_code ec;
        remove_all_long(root, ec);
    }
    objects::TypeIndex t(const char* name) const { return raw.find(name); }
};

// A minimal AI host: task force 3 with `units`; Activate_Ability's host call is recorded.
struct TfAi final : AiScriptHost {
    std::vector<objects::ObjectHandle> units;
    int calls = 0;
    std::string name;
    bool on = false;
    AiArg target;
    std::vector<std::pair<objects::ObjectHandle, bool>> activated;  // S7 t15 A1: the last per-unit activation results
    bool tf_valid(int32_t tf) override { return tf == 3; }
    int32_t tf_force_count(int32_t) override { return static_cast<int32_t>(units.size()); }
    void tf_set_flag(int32_t, AiTfFlag, bool) override {}
    int32_t tf_produce_force(int32_t, const AiArg&, bool) override { return -1; }
    int32_t tf_get_stage(int32_t) override { return -1; }
    bool tf_all_on_free_store(int32_t) override { return false; }
    int32_t tf_order(int32_t, AiTfOrder, const AiArg&) override { return -1; }
    int32_t tf_activate_ability(int32_t, std::string_view ability, bool flag, const AiArg& t) override {
        ++calls;
        name = std::string(ability);
        on = flag;
        target = t;
        return 7;
    }
    int32_t tf_activate_ability_units(int32_t tf, std::string_view ability, bool flag, const AiArg& t,
                                      const std::vector<std::pair<objects::ObjectHandle, bool>>& units_activated) override {
        activated = units_activated;
        return tf_activate_ability(tf, ability, flag, t);
    }
    bool tf_is_raid_capable(int32_t) override { return false; }
    void tf_release_forces(int32_t, double) override {}
    void tf_release_unit(int32_t, objects::ObjectHandle) override {}
    std::vector<objects::ObjectHandle> tf_units(int32_t tf) override { return tf == 3 ? units : std::vector<objects::ObjectHandle>{}; }
    std::string tf_goal_type_name(int32_t) override { return {}; }
    bool tf_collect_free_units(int32_t, uint64_t) override { return false; }
    bool status_finished(int32_t) override { return true; }
    AiStatusResult status_result(int32_t) override { return AiStatusResult::True; }
    bool target_valid(int32_t) override { return false; }
    objects::ObjectHandle target_object(int32_t) override { return {}; }
    double target_distance(int32_t, const AiArg&) override { return -1.0; }
    double budget_op(int32_t, AiBudgetOp, double, std::string_view) override { return 0.0; }
    int32_t budget_wait(int32_t, AiBudgetOp, double) override { return -1; }
    int32_t free_store_count(int32_t, int) override { return 0; }
    bool free_store_has(int32_t, objects::ObjectHandle) override { return false; }
    bool unit_in_transit(objects::ObjectHandle) override { return false; }
    bool unit_safe(objects::ObjectHandle) override { return true; }
    bool free_store_move(int32_t, objects::ObjectHandle, const AiArg&) override { return false; }
    int32_t find_target(int32_t, const AiFindTargetRequest&) override { return -1; }
    int32_t find_stage_area(int32_t, const AiArg&, const AiArg&, const AiArg&) override { return -1; }
    AiGlobalValue global_value(std::string_view) override { return {}; }
    void set_global_value(std::string_view, const AiGlobalValue&) override {}
};

static objects::ObjectHandle make(objects::ObjectStore& st, objects::TypeIndex t, int32_t owner, Vec3 p) {
    objects::SpawnParams sp;
    sp.type = t;
    sp.owner = owner;
    sp.position = p;
    sp.facing = {1.0f, 0.0f, 0.0f};
    return st.create(sp);
}

// The single UnitAbilityType name table (object_bridge.hpp): the local fallback table equals S9's.
static void test_name_table() {
    CHECK(unit_ability_id("defend") == 1 && unit_ability_id("DEPLOY_SQUAD") == 75 && unit_ability_id("Bogus") == 0);
    CHECK(unit_ability_name(30) == "TRACTOR_BEAM" && unit_ability_name(0).empty() && unit_ability_name(200).empty());
    for (uint32_t i = 1; i <= 75; ++i) CHECK(unit_ability_id(unit_ability_name(static_cast<uint8_t>(i))) == i);
#if ALAMO_LUA_HAVE_ABILITIES
    for (uint32_t i = 1; i <= tactical::abilities::kMaxAbilityType; ++i)
        CHECK(unit_ability_name(static_cast<uint8_t>(i)) == tactical::abilities::ability_type_info(i).name);
    std::printf("  ability names: lua table = S9 ability_types (75 rows)\n");
#endif
}

static void test_missile_shield_type(const Db& db) {
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Space, objects::StoreKind::Primary, db.types, &ids);
    LuaHost host;
    host.store = &store;
    host.types = &db.types;
    LuaVm vm(host);
    CHECK(run(vm, "r = Find_Object_Type('Rocket_P').Is_Affected_By_Missile_Shield() "
                  "l = Find_Object_Type('Laser_P').Is_Affected_By_Missile_Shield() "
                  "s = Find_Object_Type('Ship').Is_Affected_By_Missile_Shield()") == "");
    CHECK(gtrue(vm, "r") && !gtrue(vm, "l") && !gtrue(vm, "s"));
}

// Without tactical systems on the bridge the stage-2 behaviour stays (script state, defaults).
static void test_no_systems(const Db& db) {
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Space, objects::StoreKind::Primary, db.types, &ids);
    const objects::ObjectHandle ship = make(store, db.t("Ship"), 0, {5, 6, 7});
    (void)ship;
    ObjectBridge bridge;
    LuaHost host;
    host.store = &store;
    host.types = &db.types;
    LuaVm vm(host);
    attach_object_bridge(vm, &bridge);
    CHECK(run(vm, "s = Find_First_Object('Ship') s.Activate_Ability('DEFEND', true) a = s.Is_Ability_Active('DEFEND') "
                  "s.Force_Ability_Recharge('DEFEND') s.Force_Ability_Recharge('DEFEND', 2) s.Reset_Ability_Counter('DEFEND') "
                  "e = s.Are_Engines_Online() u = s.Is_Under_Effects_Of_Ability('TRACTOR_BEAM') "
                  "x, y, z = s.Get_Bone_Position('Root').Get_XYZ() b = s.Play_Animation('Attack', false)") == "");
    CHECK(gtrue(vm, "a") && gtrue(vm, "e") && !gtrue(vm, "u"));
    // L1/S4 wave 2b (SCRIPT_GAPS.md §2.1): only TRACTOR_BEAM / ION_CANNON_SHOT; others log a script error, no value.
    const size_t msgs = vm.messages().size();
    CHECK(run(vm, "d = s.Is_Under_Effects_Of_Ability('DEFEND')") == "" && !gtrue(vm, "d"));
    CHECK(vm.messages().size() == msgs + 1 && vm.messages().back().find("not yet supported") != std::string::npos);
    CHECK(gnum(vm, "x") == 5.0 && gnum(vm, "y") == 6.0 && gnum(vm, "z") == 7.0);  // [port] no animation data: the position
    CHECK(run_stop(vm, "s.Force_Ability_Recharge()") != "" && run_stop(vm, "s.Force_Ability_Recharge('DEFEND', 'x')") != "" &&
          run_stop(vm, "s.Reset_Ability_Counter()") != "" && run_stop(vm, "s.Get_Bone_Position(3)") != "" &&
          run_stop(vm, "s.Is_Under_Effects_Of_Ability()") != "");
    // Cancel_Hyperspace: a Lua order on the direct path (counted even without a pipeline).
    const uint32_t before = bridge.orders_issued;
    CHECK(run(vm, "s.Cancel_Hyperspace()") == "");
    CHECK(bridge.orders_issued == before + 1);
    // The generic bone hook wins.
    bridge.bone_position = [](objects::ObjectHandle, std::string_view bone, Vec3& out) {
        if (bone != "HP_X") return false;
        out = {1.0f, 2.0f, 3.0f};
        return true;
    };
    CHECK(run(vm, "x, y, z = s.Get_Bone_Position('HP_X').Get_XYZ()") == "");
    CHECK(gnum(vm, "x") == 1.0 && gnum(vm, "y") == 2.0 && gnum(vm, "z") == 3.0);
}

#if ALAMO_LUA_HAVE_ABILITIES
namespace ab = tactical::abilities;
namespace cb = tactical::combat;

static ab::UnitAbilityRecord rec(uint8_t type, float exp, float rech, const char* gui = nullptr) {
    ab::UnitAbilityRecord r;
    r.type = type;
    r.combat.ability_type = type;
    r.expiration_seconds = exp;
    r.recharge_seconds = rech;
    if (gui) {
        r.gui_activated_ability_name = gui;
        r.gui_name_crc = name_crc(gui);
    }
    return r;
}

// Ship (abilities, an engine hardpoint, shields), Target_Ship (a corvette, tractor target), Hauler (engine only).
struct AbilityWorld {
    cb::CombatData cd;
    ab::AbilityData ad;
    std::unique_ptr<cb::CombatSystem> combat;
    std::unique_ptr<ab::AbilitySystem> abilities;
    SimRng rng{77u};
    uint32_t frame = 1;
    static constexpr uint64_t kCorvette = 2;
    AbilityWorld() {
        cb::HardpointDesc eng;
        eng.name = "HP_Engine";
        eng.kind = cb::HardpointKind::Engine;
        eng.health = 50.0f;
        eng.destroyable = eng.targetable = true;
        cb::HardpointDesc tb = eng;  // TRACTOR_BEAM needs a live tractor hardpoint (gate 10)
        tb.name = "HP_Tractor";
        tb.kind = cb::HardpointKind::TractorBeam;
        cd.hardpoints = {eng, tb};
        cb::PrioritySet ps_a, ps_b;  // OI lua: Set_Targeting_Priorities targets
        ps_a.name = "Capital_Attack";
        ps_b.name = "Fighter_Attack";
        cd.priority_sets = {ps_a, ps_b};
        ab::SpecialAbilityDesc tractor;
        tractor.name = "Tractor";
        tractor.name_crc = name_crc("Tractor");
        tractor.cls = ab::SpecialClass::TractorBeam;
        tractor.style = ab::ActivationStyle::UserInput;
        tractor.applicable_categories = kCorvette;
        tractor.max_range = 1000.0f;
        ad.specials = {tractor};
        cb::UnitDesc ship;
        ship.name = "Ship";
        ship.tactical_health = 1000.0f;
        ship.shield_points = 500.0f;
        ship.radius = 50.0f;
        ship.hardpoints = {0, 1};
        ab::AbilityObjectDesc sa;
        sa.name = ship.name;
        sa.name_crc = name_crc(ship.name);
        sa.unit_abilities = {rec(ab::kDefend, 15.0f, 20.0f), rec(ab::kSpoilerLock, 0.0f, 0.0f), rec(ab::kTractorBeam, 0.0f, 5.0f, "Tractor"),
                             rec(ab::kHarmonicBomb, 0.0f, 10.0f), rec(ab::kBuzzDroids, 0.0f, 10.0f), rec(ab::kDeploy, 0.0f, 0.0f)};
        sa.specials = {0};
        push(ship, sa);
        cb::UnitDesc target;
        target.name = "Target_Ship";
        target.tactical_health = 400.0f;
        target.radius = 20.0f;
        target.category_mask = kCorvette;
        ab::AbilityObjectDesc ta;
        push(target, ta);
        cb::UnitDesc hauler;
        hauler.name = "Hauler";
        hauler.tactical_health = 200.0f;
        hauler.radius = 20.0f;
        hauler.hardpoints = {0};
        push(hauler, ab::AbilityObjectDesc{});
        // cu8l t04: a squad member (desc 3) with HUNT / AVOID_DANGER (Activate_Ability ids 6 / 12) and AFTERBURNER
        // (8, unsupported by Activate_Ability); its team container uses desc 4 (no abilities of its own).
        cb::UnitDesc squaddie;
        squaddie.name = "Squaddie";
        squaddie.tactical_health = 100.0f;
        squaddie.radius = 5.0f;
        ab::AbilityObjectDesc qa;
        qa.unit_abilities = {rec(ab::kHunt, 0.0f, 0.0f), rec(ab::kAvoidDanger, 0.0f, 0.0f), rec(ab::kAfterburner, 0.0f, 0.0f)};
        push(squaddie, qa);
        cb::UnitDesc team;
        team.name = "Squad_Team";
        push(team, ab::AbilityObjectDesc{});
        combat =std::make_unique<cb::CombatSystem>(cd, cb::CombatConfig{}, std::vector<cb::PlayerInfo>{cb::PlayerInfo{}, cb::PlayerInfo{}});
        abilities = std::make_unique<ab::AbilitySystem>(*combat, ad, ab::AbilityConfig{});
        combat->bind_rng(&rng);
    }
    void push(cb::UnitDesc u, ab::AbilityObjectDesc a) {
        cd.units.push_back(u);
        a.name = u.name;
        a.name_crc = name_crc(u.name);
        a.category_mask = u.category_mask;
        ab::link_combat_abilities(a, cd, cd.units.back());
        ad.objects.push_back(std::move(a));
    }
    void add(objects::ObjectId id, uint32_t desc, int32_t owner, Vec3 p) {
        cb::UnitSpawn s;
        s.id = id;
        s.desc = desc;
        s.owner = owner;
        s.position = {p.x, p.y, p.z};
        CHECK(abilities->add_unit(s, desc, frame, rng));
    }
};

static void test_abilities(const Db& db) {
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Space, objects::StoreKind::Primary, db.types, &ids);
    const objects::ObjectId ship = store.id(make(store, db.t("Ship"), 0, {0, 0, 0}));
    const objects::ObjectId target = store.id(make(store, db.t("Target_Ship"), 1, {300, 0, 0}));
    const objects::ObjectId hauler = store.id(make(store, db.t("Hauler"), 0, {0, 500, 0}));
    AbilityWorld w;
    w.add(ship, 0, 0, {0, 0, 0});
    w.add(target, 1, 1, {300, 0, 0});
    w.add(hauler, 2, 0, {0, 500, 0});
    ab::AbilitySystem& a = *w.abilities;
    ObjectBridge bridge;
    bridge.abilities = &a;
    bridge.ability_frame = [&w] { return w.frame; };
    Vec3 dummy_at{};
    int32_t dummy_owner = -9;
    bridge.spawn_ability_target = [&](Vec3 p, int32_t owner) {
        dummy_at = p;
        dummy_owner = owner;
        return target;  // a stand-in object (no Harmonic Bomb special here: nothing is accepted)
    };
    LuaHost host;
    host.store = &store;
    host.types = &db.types;
    LuaVm vm(host);
    attach_object_bridge(vm, &bridge);
    vm.set_frame(999);  // the ability frame comes from the hook, not the VM
    CHECK(run(vm, "s = Find_First_Object('Ship') t = Find_First_Object('Target_Ship') h = Find_First_Object('Hauler') "
                  "has_d = s.Has_Ability('DEFEND') has_t = s.Has_Ability('TURBO') rdy = s.Is_Ability_Ready('DEFEND') "
                  "s.Activate_Ability('DEFEND', true) act = s.Is_Ability_Active('defend') eff = s.Is_Under_Effects_Of_Ability('ION_CANNON_SHOT') == false "
                  "tr = t.Is_Under_Effects_Of_Ability('TRACTOR_BEAM')") == "");
    CHECK(gtrue(vm, "has_d") && !gtrue(vm, "has_t") && gtrue(vm, "rdy") && gtrue(vm, "act") && gtrue(vm, "eff") && !gtrue(vm, "tr"));
    CHECK(a.is_active(ship, ab::kDefend) && a.countdown_maximum(ship, ab::kDefend) == 450);
    // The script state is not used for systems' objects (no stage-2 copy).
    const ScriptObjectState* st = bridge.find_state(ship);
    CHECK(!st || st->abilities.empty());
    // Cancel (§5: SetUnitAbilityMode off, early cancel -> recharge) then a forced recharge length.
    CHECK(run(vm, "s.Cancel_Ability('DEFEND', 0) act2 = s.Is_Ability_Active('DEFEND') rdy2 = s.Is_Ability_Ready('DEFEND')") == "");
    // (cancelled in the activation frame: the early-cancel recharge is int(600 x 0) = 0, so it is ready again)
    CHECK(!gtrue(vm, "act2") && gtrue(vm, "rdy2") == a.is_ready(ship, ab::kDefend, w.frame) && !a.is_active(ship, ab::kDefend));
    CHECK(run(vm, "s.Force_Ability_Recharge('DEFEND', 2)") == "");
    CHECK(a.countdown_counter(ship, ab::kDefend) == 60 && a.countdown_maximum(ship, ab::kDefend) == 60);
    // Reset_Ability_Counter: only with a zero counter (SPOILER_LOCK has no timers) -> counter = maximum = 1.
    CHECK(run(vm, "s.Reset_Ability_Counter('DEFEND') s.Reset_Ability_Counter('SPOILER_LOCK')") == "");
    CHECK(a.countdown_counter(ship, ab::kDefend) == 60 && a.countdown_counter(ship, ab::kSpoilerLock) == 1 &&
          a.countdown_maximum(ship, ab::kSpoilerLock) == 1);
    // Toggle with a bool argument.
    CHECK(run(vm, "s.Reset_Ability_Counter('SPOILER_LOCK')") == "");
    a.force_recharge(ship, ab::kSpoilerLock, 0.0f);  // counter 0 again
    CHECK(run(vm, "s.Activate_Ability('SPOILER_LOCK', true) on = s.Is_Ability_Active('SPOILER_LOCK') "
                  "s.Activate_Ability('SPOILER_LOCK', false) off = s.Is_Ability_Active('SPOILER_LOCK')") == "");
    CHECK(gtrue(vm, "on") && !gtrue(vm, "off"));
    // Object family (command 40): attack order + pending ability; Is_Ability_Ready is true while pending.
    CHECK(run(vm, "s.Activate_Ability('TRACTOR_BEAM', t) prdy = s.Is_Ability_Ready('TRACTOR_BEAM')") == "");
    CHECK(a.pending_type(ship) == ab::kTractorBeam && a.pending_target(ship) == target && gtrue(vm, "prdy"));
    CHECK(w.combat->ordered_target(ship) == target);
    // S7 t32 (AI_TACTICAL §7.3.3 part 6): HARMONIC_BOMB (35) is command 43 mode on with the argument ignored - no dummy.
    CHECK(run(vm, "hb = s.Activate_Ability('HARMONIC_BOMB', Create_Position(50, 60, 0))") == "");
    CHECK(dummy_owner == -9 && gnil(vm, "hb"));
    // Autofire: slot byte written directly.
    CHECK(run(vm, "s.Set_Single_Ability_Autofire('DEFEND', true) af = s.Is_Ability_Autofire('DEFEND') "
                  "s.Set_All_Abilities_Autofire(false) af2 = s.Is_Ability_Autofire('DEFEND')") == "");
    // OI abilities (TACTICAL_GAPS §1.3): the setter 0x3a78b0 refuses a record without Supports_Autofire for a human owner.
    CHECK(!gtrue(vm, "af") && !gtrue(vm, "af2") && !a.is_autofire(ship, ab::kDefend));
    // Get_Hull / Get_Shield from the combat system.
    w.combat->set_health(ship, w.combat->max_health(ship) * 0.5f);
    w.combat->set_shield(ship, w.combat->max_shield_of(ship) * 0.25f);
    CHECK(run(vm, "hull = s.Get_Hull() sh = s.Get_Shield() tsh = t.Get_Shield()") == "");
    CHECK(gnum(vm, "hull") == static_cast<double>(w.combat->script_hull(ship)) && gnum(vm, "hull") > 0.49 && gnum(vm, "hull") < 0.51);
    CHECK(gnum(vm, "sh") == static_cast<double>(w.combat->shield_fraction(ship)) && gnum(vm, "tsh") == 0.0);
    // Are_Engines_Online: no engine hardpoint -> online; the hauler's only engine destroyed -> offline.
    CHECK(run(vm, "e1 = t.Are_Engines_Online() e2 = h.Are_Engines_Online()") == "");
    CHECK(gtrue(vm, "e1") && gtrue(vm, "e2"));
    cb::DamageRequest d;
    d.victim = hauler;
    d.amount = 100000.0f;
    d.hardpoint = 0;
    w.combat->take_damage(d, w.frame);
    CHECK(w.combat->hardpoint_destroyed(hauler, 0));
    CHECK(run(vm, "e3 = h.Are_Engines_Online()") == "");
    CHECK(!gtrue(vm, "e3"));
    // OI lua: task-force Activate_Ability(name, on | target) = each unit's Activate_Ability (AI_TACTICAL §7.3); the host
    // gets the name, the flag and the target for the status.
    a.force_recharge(ship, ab::kSpoilerLock, 0.0f);
    TfAi ai;
    ai.units = {store.find(ship)};
    attach_ai_host(vm, &ai, 1);
    set_task_force_global(vm, "TF", 3, AiTaskForceKind::Space);
    CHECK(run(vm, "st = TF.Activate_Ability('SPOILER_LOCK', true) sf = st.IsFinished()") == "" && gtrue(vm, "sf"));
    CHECK(a.is_active(ship, ab::kSpoilerLock) && ai.calls == 1 && ai.name == "SPOILER_LOCK" && ai.on && !ai.target.present);
    CHECK(run(vm, "TF.Activate_Ability('SPOILER_LOCK', false) TF.Activate_Ability('TRACTOR_BEAM', t)") == "");
    CHECK(!a.is_active(ship, ab::kSpoilerLock) && ai.calls == 3 && ai.name == "TRACTOR_BEAM" && ai.target.present &&
          a.pending_type(ship) == ab::kTractorBeam && a.pending_target(ship) == target);
    // OI lua: Set_Targeting_Priorities = the S8 per-unit override (set index; unknown name clears), also via the TF.
    CHECK(run(vm, "s.Set_Targeting_Priorities('fighter_attack')") == "" && w.combat->priority_override(ship) == 1);
    CHECK(run(vm, "TF.Set_Targeting_Priorities('Capital_Attack', 'Ship')") == "" && w.combat->priority_override(ship) == 0);
    CHECK(run(vm, "TF.Set_Targeting_Priorities('Fighter_Attack', 'Hauler')") == "" && w.combat->priority_override(ship) == 0);
    CHECK(run(vm, "s.Set_Targeting_Priorities('No_Such_Set')") == "" && w.combat->priority_override(ship) == -1);
    // cu8f t04a (CU8F_T04_T06_RE §1.1): Set_Targeting_Stickiness_Time_Threshold writes the combat threshold TD +0x158 (spawn
    // 0.0) as given, negative included (= the type's value); a task-force release puts the type's value back.
    CHECK(w.combat->stickiness_override(ship) == 0.0f && w.combat->stickiness_override(target) == 0.0f);
    CHECK(run(vm, "s.Set_Targeting_Stickiness_Time_Threshold(2.5)") == "" && w.combat->stickiness_override(ship) == 2.5f);
    CHECK(run(vm, "t.Set_Targeting_Stickiness_Time_Threshold(-1)") == "" && w.combat->stickiness_override(target) == -1.0f);
    CHECK(w.combat->stickiness_override(ship) == 2.5f && w.combat->stickiness_override(hauler) == 0.0f);
    CHECK(run_stop(vm, "s.Set_Targeting_Stickiness_Time_Threshold(2, 3)").find("Expected 1, got 2") != std::string::npos &&
          w.combat->stickiness_override(ship) == 2.5f);
    CHECK(w.combat->reset_stickiness(ship) && w.combat->stickiness_override(ship) == w.cd.units[0].stickiness_threshold);
    // S7 t15 A1 / S7 t26 (AI_TACTICAL §7.3.3 item 1): the host learns which units answered true (a block: one
    // AbilityBlockStatus count each). SPOILER_LOCK is a "never" family: false even when it turned on; TRACTOR_BEAM (object
    // target) is true when the pending ability was stored.
    a.force_recharge(ship, ab::kSpoilerLock, 0.0f);
    CHECK(run(vm, "TF.Activate_Ability('SPOILER_LOCK', true)") == "" && ai.activated.size() == 1 &&
          ai.activated[0].first == store.find(ship) && !ai.activated[0].second && a.is_active(ship, ab::kSpoilerLock));
    CHECK(run(vm, "TF.Activate_Ability('SPOILER_LOCK', false)") == "" && !a.is_active(ship, ab::kSpoilerLock));
    CHECK(run(vm, "TF.Activate_Ability('TRACTOR_BEAM', t)") == "" && ai.activated.size() == 1 && ai.activated[0].second);
    attach_ai_host(vm, nullptr, -1);
    // S7 t26: which object Activate_Ability calls return a block - "never" families (DEFEND, SPOILER_LOCK) nil even on
    // success; an ability the unit lacks nil; DEPLOY nil when already in the requested state, else a block; the
    // Dummy_Barrage_Target ids (BUZZ_DROIDS) a block once the gates pass.
    a.force_recharge(ship, ab::kSpoilerLock, 0.0f);
    CHECK(run(vm, "n1 = s.Activate_Ability('SPOILER_LOCK', true) n2 = s.Activate_Ability('TURBO', true) "
                  "n3 = s.Activate_Ability('DEPLOY', false) d1 = s.Activate_Ability('DEPLOY', true) "
                  "bz = s.Activate_Ability('BUZZ_DROIDS', Create_Position(10, 20, 0))") == "");
    std::printf("  activate ability blocks: n1 %d n2 %d n3 %d d1 %d bz %d\n", gnil(vm, "n1"), gnil(vm, "n2"), gnil(vm, "n3"),
                gnil(vm, "d1"), gnil(vm, "bz"));
    CHECK(gnil(vm, "n1") && gnil(vm, "n2") && gnil(vm, "n3") && !gnil(vm, "d1") && !gnil(vm, "bz") && a.is_active(ship, ab::kSpoilerLock));
    // S7 t32: BUZZ_DROIDS (71) - the host spawns the Dummy_Barrage_Target at the point (command-40 path on it).
    CHECK(dummy_at.x == 10.0f && dummy_at.y == 20.0f && dummy_owner == 0);
    CHECK(run(vm, "s.Activate_Ability('SPOILER_LOCK', false)") == "");
    // S7 t15 A1 / t26 (§7.3.3 item 2): the object block (TRACTOR_BEAM, accepted) is pending until the unit's next signal
    // 0x29 (ObjectBridge::ability_finished) or 0xb (order_replaced) or its death / removal (0x10). A signal before the
    // activation or for another object does not count.
    bridge.ability_finished(ship);  // before the activation
    CHECK(run(vm, "ab1 = s.Activate_Ability('TRACTOR_BEAM', t) f1 = ab1 ~= nil and ab1.IsFinished()") == "" && !gnil(vm, "ab1"));
    CHECK(!gtrue(vm, "f1"));
    const uint32_t signals0 = bridge.ability_signals(ship);
    bridge.ability_finished(target);  // not watched: ignored
    CHECK(run(vm, "f2 = ab1.IsFinished()") == "" && !gtrue(vm, "f2") && bridge.ability_signals(target) == 0);
    bridge.ability_finished(ship);
    CHECK(run(vm, "f3 = ab1.IsFinished() ab2 = s.Activate_Ability('TRACTOR_BEAM', t) f4 = ab2.IsFinished()") == "");
    CHECK(gtrue(vm, "f3") && !gtrue(vm, "f4") && bridge.ability_signals(ship) == signals0 + 1);
    bridge.order_replaced(ship);  // S7 t26: a new order (0xb) ends it too
    CHECK(run(vm, "f5 = ab2.IsFinished() f6 = ab1.IsFinished()") == "" && gtrue(vm, "f5") && gtrue(vm, "f6"));
    {  // S7 t31 (§7.3.3 item 1): Activate_Ability of a listed id (BUZZ_DROIDS 71, gates passed) raises 0xb on the unit before
       // executing, through the host's raiser (the battle): a pending block on the unit ends. TRACTOR_BEAM (30) is not listed.
        std::vector<objects::ObjectId> raised;
        bridge.raise_order_replaced = [&](objects::ObjectId id) {
            raised.push_back(id);
            bridge.order_replaced(id);
        };
        CHECK(run(vm, "ab4 = s.Activate_Ability('TRACTOR_BEAM', t) f9 = ab4 ~= nil and ab4.IsFinished()") == "");
        CHECK(!gnil(vm, "ab4") && !gtrue(vm, "f9") && raised.empty());
        CHECK(run(vm, "s.Activate_Ability('BUZZ_DROIDS', Create_Position(10, 20, 0)) f10 = ab4.IsFinished()") == "");
        std::printf("  activate ability 0xb: raised %zu (ship %d), earlier block finished %d\n", raised.size(),
                    !raised.empty() && raised[0] == ship ? 1 : 0, gtrue(vm, "f10") ? 1 : 0);
        CHECK(gtrue(vm, "f10") && raised.size() == 1 && raised[0] == ship);
        bridge.raise_order_replaced = nullptr;
    }
    {  // death finishes the block (signal 0x10)
        const objects::ObjectHandle h2 = make(store, db.t("Ship"), 0, {0, -500, 0});
        w.add(store.id(h2), 0, 0, {0, -500, 0});
        vm.set_object_global("S2", h2);
        CHECK(run(vm, "ab3 = S2.Activate_Ability('TRACTOR_BEAM', t) f7 = ab3.IsFinished()") == "" && !gtrue(vm, "f7"));
        CHECK(store.destroy(h2));
        CHECK(run(vm, "f8 = ab3.IsFinished()") == "" && gtrue(vm, "f8"));
    }
    // OI abilities t01 (A1_READINGS §28.8, §28.9): Lock / Unlock_Current_Orders and the ability share of the obj +0x3a8
    // counter.
    auto said = [&vm](const char* text) {
        size_t n = 0;
        for (const std::string& m : vm.messages()) n += m.find(text) != std::string::npos ? 1u : 0u;
        return n;
    };
    auto stack_warnings = [&] { return said("Script cannot stack locks"); };
    CHECK(run(vm, "s.Lock_Current_Orders()") == "" && bridge.state(ship).orders_locked && a.immobilize_count(ship) == 0);
    CHECK(run(vm, "s.Lock_Current_Orders()") == "" && stack_warnings() == 1 && bridge.state(ship).orders_locked);
    CHECK(run(vm, "s.Unlock_Current_Orders()") == "" && !bridge.state(ship).orders_locked);
    CHECK(a.lock_movement(ship, true, w.frame));  // a Disable_Movement ability holds the counter
    CHECK(run(vm, "s.Lock_Current_Orders()") == "" && stack_warnings() == 2 && !bridge.state(ship).orders_locked);
    CHECK(run(vm, "s.Unlock_Current_Orders()") == "" && a.immobilize_count(ship) == 0);  // releases the ability's share
    CHECK(run(vm, "s.Unlock_Current_Orders()") == "" && a.immobilize_count(ship) == 0);  // nothing locked: no change
    CHECK(said("Unlock_Current_Orders - object Ship is not locked.") == 1);
    // OI follow-up t04: no locomotor -> the immobile error, nothing locked.
    CHECK(run(vm, "t.Lock_Current_Orders()") == "" && !bridge.state(target).orders_locked &&
          said("SCRIPT ERROR: GameObjectWrapper::Lock_Current_Orders - Target_Ship is immobile; locking not supported.") == 1);
    // OI follow-up t03: a locked unit refuses Move_To / Attack_Target (warning, no order) and cannot move.
    CHECK(run(vm, "s.Lock_Current_Orders() mv = s.Can_Move() s.Move_To(t) s.Attack_Target(t)") == "" && !gtrue(vm, "mv"));
    CHECK(said("Attack_Target - object Ship is locked into existing orders. Attack_Target fails.") == 2);
    CHECK(run(vm, "s.Unlock_Current_Orders() mv = s.Can_Move()") == "" && gtrue(vm, "mv"));
    CHECK(vm.errors().empty());
    std::printf("  abilities: S9 AbilitySystem bindings OK\n");
}

// cu8l t04 (CU8K_T04_COORDINATOR_TRACES "Which check ends the script", LUA_API Activate_Ability row): the checks of
// 0x570a50 in order - count, name (script errors), not the unit's ability (warning, nil), not ready (nil, the argument
// not read), then the argument kind per ability id (script error; a dead target object of an object id: warning, nil);
// ids 6 / 10 / 12 always switch the mode on, on the team container; ids 4 8 47 75 76 are unsupported (script error).
static void test_activate_ability_checks(const Db& db) {
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Space, objects::StoreKind::Primary, db.types, &ids);
    const objects::ObjectId ship = store.id(make(store, db.t("Ship"), 0, {0, 0, 0}));
    const objects::ObjectId target = store.id(make(store, db.t("Target_Ship"), 1, {300, 0, 0}));
    const objects::ObjectHandle doomed_h = make(store, db.t("Target_Ship"), 1, {350, 0, 0});
    const objects::ObjectId doomed = store.id(doomed_h);
    const objects::ObjectId m1 = store.id(make(store, db.t("Hauler"), 0, {0, 100, 0}));
    const objects::ObjectId m2 = store.id(make(store, db.t("Hauler"), 0, {0, 110, 0}));
    const objects::ObjectId team = store.id(make(store, db.t("Hauler"), 0, {0, 105, 0}));
    const objects::ObjectId lone = store.id(make(store, db.t("Hauler"), 0, {0, 200, 0}));
    AbilityWorld w;
    w.add(ship, 0, 0, {0, 0, 0});
    w.add(target, 1, 1, {300, 0, 0});
    w.add(doomed, 1, 1, {350, 0, 0});
    w.add(m1, 3, 0, {0, 100, 0});
    w.add(m2, 3, 0, {0, 110, 0});
    const objects::ObjectId members[] = {m1, m2};
    CHECK(w.abilities->add_container(team, 4, 0, members, w.frame));
    w.add(lone, 3, 0, {0, 200, 0});
    ab::AbilitySystem& a = *w.abilities;
    CHECK(a.container_of(m1) == team && a.container_of(lone) == objects::kNoObjectId);
    ObjectBridge bridge;
    bridge.abilities = &a;
    bridge.ability_frame = [&w] { return w.frame; };
    int dummies = 0;
    bridge.spawn_ability_target = [&](Vec3, int32_t) {
        ++dummies;
        return target;
    };
    LuaHost host;
    host.store = &store;
    host.types = &db.types;
    LuaVm vm(host);
    attach_object_bridge(vm, &bridge);
    vm.set_object_global("S", store.find(ship));
    vm.set_object_global("T", store.find(target));
    vm.set_object_global("D", doomed_h);
    vm.set_object_global("M1", store.find(m1));
    vm.set_object_global("M2", store.find(m2));
    vm.set_object_global("L", store.find(lone));
    auto has = [](const std::string& s, const char* what) { return s.find(what) != std::string::npos; };
    auto said = [&vm](const char* text) {
        size_t n = 0;
        for (const std::string& m : vm.messages()) n += m.find(text) != std::string::npos ? 1u : 0u;
        return n;
    };
    // 1. Argument count != 2 is a script error (the one-argument and no-argument forms too; a nil counts as present).
    CHECK(has(run_stop(vm, "S.Activate_Ability('SPOILER_LOCK')"), "invalid number of parameters.  Expected 2, got 1."));
    CHECK(has(run_stop(vm, "S.Activate_Ability()"), "Expected 2, got 0."));
    CHECK(has(run_stop(vm, "S.Activate_Ability('SPOILER_LOCK', true, 1)"), "Expected 2, got 3."));
    CHECK(!a.is_active(ship, ab::kSpoilerLock));
    CHECK(has(run_stop(vm, "S.Activate_Ability('SPOILER_LOCK', nil)"), "Ability SPOILER_LOCK expects a bool."));
    // 2. Argument 1: not a string / an unknown name.
    CHECK(has(run_stop(vm, "S.Activate_Ability(5, true)"), "invalid type for ability 1.  Expected string."));
    CHECK(has(run_stop(vm, "S.Activate_Ability('NO_SUCH_ABILITY', true)"), "unknown ability NO_SUCH_ABILITY."));
    // 3. Not the unit's ability: a warning, nil, the script goes on (the argument is not looked at).
    CHECK(run_stop(vm, "w1 = S.Activate_Ability('HUNT', T)") == "" && gnil(vm, "w1") && !vm.exit_requested());
    CHECK(said("SCRIPT WARNING: GameObjectWrapper::Activate_Ability -- Object Ship does not have ability HUNT.") == 1);
    // 4. The argument kind per id, gates passed: a script error, nothing executed.
    CHECK(has(run_stop(vm, "S.Activate_Ability('SPOILER_LOCK', T)"), "Ability SPOILER_LOCK expects a bool."));
    CHECK(has(run_stop(vm, "S.Activate_Ability('SPOILER_LOCK', 1)"), "Ability SPOILER_LOCK expects a bool."));
    CHECK(has(run_stop(vm, "S.Activate_Ability('DEPLOY', T)"), "Ability DEPLOY expects a bool."));
    CHECK(!a.is_active(ship, ab::kSpoilerLock) && !a.is_active(ship, ab::kDeploy));
    CHECK(has(run_stop(vm, "S.Activate_Ability('TRACTOR_BEAM', true)"), "Ability TRACTOR_BEAM expects a target object."));
    CHECK(has(run_stop(vm, "S.Activate_Ability('TRACTOR_BEAM', Create_Position(300, 0, 0))"), "expects a target object."));
    CHECK(a.pending_type(ship) == -1);
    CHECK(has(run_stop(vm, "S.Activate_Ability('BUZZ_DROIDS', true)"), "Ability BUZZ_DROIDS expects a target position."));
    CHECK(has(run_stop(vm, "S.Activate_Ability('BUZZ_DROIDS', 'x')"), "expects a target position."));
    CHECK(dummies == 0);
    // 14 23 ... 72 (HARMONIC_BOMB 35) do not read the argument.
    CHECK(run_stop(vm, "S.Activate_Ability('HARMONIC_BOMB', 'anything')") == "");
    // A dead target: the object ids warn and answer nil (the script goes on); the position ids error.
    CHECK(store.destroy(doomed_h));
    const std::string dead = run_stop(vm, "d1 = S.Activate_Ability('TRACTOR_BEAM', D)");
    if (!dead.empty()) std::printf("  dead target: %s\n", dead.c_str());
    CHECK(dead.empty() && gnil(vm, "d1") && a.pending_type(ship) == -1);
    CHECK(said("SCRIPT WARNING: GameObjectWrapper::Activate_Ability -- target object is already dead.") == 1);
    CHECK(has(run_stop(vm, "S.Activate_Ability('BUZZ_DROIDS', D)"), "Ability BUZZ_DROIDS expects a target position."));
    // 5. Not ready: nil without reading the argument (no error for a wrong kind).
    CHECK(a.force_recharge(ship, ab::kTractorBeam, 5.0f) && !a.is_ready(ship, ab::kTractorBeam, w.frame));
    CHECK(run_stop(vm, "n1 = S.Activate_Ability('TRACTOR_BEAM', true)") == "" && gnil(vm, "n1"));
    // The right kinds still work.
    CHECK(run_stop(vm, "S.Activate_Ability('SPOILER_LOCK', true)") == "" && a.is_active(ship, ab::kSpoilerLock));
    CHECK(run_stop(vm, "bz = S.Activate_Ability('BUZZ_DROIDS', T)") == "" && dummies == 1 && !gnil(vm, "bz"));
    // 6. Unsupported ids (4 8 47 75 76): a script error.
    CHECK(has(run_stop(vm, "M1.Activate_Ability('AFTERBURNER', true)"), "unsupported unit ability AFTERBURNER."));
    // 7. 6 / 10 / 12: a bool only; its value is ignored (mode on); a team member's command goes to its team container,
    // which forwards it to every member.
    CHECK(has(run_stop(vm, "M1.Activate_Ability('HUNT', T)"), "Ability HUNT expects a bool."));
    CHECK(has(run_stop(vm, "M1.Activate_Ability('HUNT', Create_Position(0, 0, 0))"), "Ability HUNT expects a bool."));
    CHECK(!a.is_active(m1, ab::kHunt) && !a.is_active(m2, ab::kHunt));
    CHECK(run_stop(vm, "h1 = M1.Activate_Ability('HUNT', false)") == "" && gnil(vm, "h1"));
    std::printf("  activate ability 6/10/12: HUNT(false) on member 1 -> member 1 %d, member 2 %d\n", a.is_active(m1, ab::kHunt) ? 1 : 0,
                a.is_active(m2, ab::kHunt) ? 1 : 0);
    CHECK(a.is_active(m1, ab::kHunt) && a.is_active(m2, ab::kHunt) && !a.is_active(lone, ab::kHunt));
    CHECK(run_stop(vm, "L.Activate_Ability('AVOID_DANGER', false)") == "" && a.is_active(lone, ab::kAvoidDanger) &&
          !a.is_active(m1, ab::kAvoidDanger));
    // 8. Task-force Activate_Ability runs each unit's checks (0x5f9890 -> 0x570a50).
    TfAi ai;
    ai.units = {store.find(ship)};
    attach_ai_host(vm, &ai, 1);
    set_task_force_global(vm, "TF", 3, AiTaskForceKind::Space);
    CHECK(has(run_stop(vm, "TF.Activate_Ability('SPOILER_LOCK')"), "Expected 2, got 1."));
    CHECK(ai.activated.size() == 1 && !ai.activated[0].second);
    attach_ai_host(vm, nullptr, -1);
    CHECK(vm.errors().empty());
    std::printf("  activate ability checks: count / name / kind errors, warnings, 6/10/12, unsupported OK\n");
}
#endif

#if ALAMO_LUA_HAVE_ANIM
static void test_animation(const Db& db) {
    objects::IdCounter ids;
    objects::ObjectStore store(objects::ModeKind::Space, objects::StoreKind::Primary, db.types, &ids);
    const objects::ObjectHandle h = make(store, db.t("Ship"), 0, {100, 0, 0});
    store.set_facing(h, {0.0f, 1.0f, 0.0f});
    const objects::ObjectId id = store.id(h);
    anim::AnimModel model;
    const std::string names[] = {"Root", "B_Muzzle"};
    const int32_t parents[] = {-1, 0};
    const uint32_t billboard[] = {0, 0};
    anim::Mat34 bind[2];
    bind[1].m[0][3] = 1.0f;
    bind[1].m[1][3] = 2.0f;
    bind[1].m[2][3] = 3.0f;
    model.skeleton = anim::skeleton_from_arrays(names, parents, billboard, bind);
    model.has_skeleton = true;
    auto clip = std::make_shared<anim::Clip>();
    clip->frame_count = 10;
    anim::ClipRef ref;
    ref.clip = clip;
    model.clips[anim::kAnimAttack].push_back(ref);
    anim::AnimWorld world;
    world.add(id, &model, 0);
    ObjectBridge bridge;
    bridge.anim = &world;
    uint64_t global = 40;
    bridge.anim_frame = [&global] { return global; };
    LuaHost host;
    host.store = &store;
    host.types = &db.types;
    LuaVm vm(host);
    attach_object_bridge(vm, &bridge);
    // Bind pose bone (1, 2, 3) under the default transform: facing +Y at (100, 0, 0); the model faces -Y, so
    // model +X (left) -> world -X and model +Y (aft) -> world -Y.
    CHECK(run(vm, "s = Find_First_Object('Ship') x, y, z = s.Get_Bone_Position('b_muzzle').Get_XYZ() "
                  "rx, ry, rz = s.Get_Bone_Position('No_Such_Bone').Get_XYZ()") == "");
    CHECK(gnum(vm, "x") == 99.0 && gnum(vm, "y") == -2.0 && gnum(vm, "z") == 3.0);
    CHECK(gnum(vm, "rx") == 100.0 && gnum(vm, "ry") == 0.0 && gnum(vm, "rz") == 0.0);
    // The host's transform hook.
    bridge.object_to_world = [](objects::ObjectHandle, anim::Mat34& m) {
        m = anim::Mat34{};
        m.m[2][3] = 10.0f;
        return true;
    };
    CHECK(run(vm, "x, y, z = s.Get_Bone_Position('B_Muzzle').Get_XYZ()") == "");
    CHECK(gnum(vm, "x") == 1.0 && gnum(vm, "y") == 2.0 && gnum(vm, "z") == 13.0);
    // Play_Animation: find_anim_type + play (blend 0.33 s); an unknown name plays nothing.
    CHECK(run(vm, "s.Play_Animation('attack', false) s.Play_Animation('No_Such_Anim', true)") == "");
    CHECK(world.current_type(id) == anim::kAnimAttack && world.slot(id).mode == anim::SlotMode::OneShot);
    const ScriptObjectState* st = bridge.find_state(id);
    CHECK(st && st->animation == "No_Such_Anim" && st->animation_loop);
    std::printf("  animation: Play_Animation / Get_Bone_Position on AnimWorld OK\n");
}
#endif

int main() {
    log::set_min_level(log::Level::Error);
    const Db db;
    CHECK(db.ok);
    if (!db.ok) return 1;
    test_name_table();
    test_missile_shield_type(db);
    test_no_systems(db);
#if ALAMO_LUA_HAVE_ABILITIES
    test_abilities(db);
    test_activate_ability_checks(db);
#else
    std::printf("  abilities: built without engine/tactical/abilities, skipped\n");
#endif
#if ALAMO_LUA_HAVE_ANIM
    test_animation(db);
#else
    std::printf("  animation: built without engine/anim, skipped\n");
#endif
    std::printf(g_fail ? "alamo_lua_tactical_tests: %d failure(s)\n" : "alamo_lua_tactical_tests: all passed\n", g_fail);
    return g_fail ? 1 : 0;
}

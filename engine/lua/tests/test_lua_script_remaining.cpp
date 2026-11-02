// engine/lua L1 final: the remaining family (bind_script_remaining.cpp, alamo/lua/script_remaining.hpp; spec
// docs/re/L1_S4_REMAINING.md §1 / §2): every global and method it binds, with and without host hooks (the "not
// available" answers and the documented data fallbacks), GlobalValue (process store, tables, userdata refused, save /
// load), LuaScriptWrapper across two VMs, the task-force delegation to the object bindings, the family table (no
// duplicates) and the coverage (no unbound row left).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/ai_tactical_bridge.hpp"
#include "alamo/lua/bindings.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "alamo/lua/script_remaining.hpp"
#if ALAMO_LUA_PLAYERS
#include "alamo/lua/players.hpp"
#endif
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#include "vm_internal.hpp"

#include <lua.h>

using namespace alamo;
using namespace alamo::lua;
namespace fs = std::filesystem;
using objects::ObjectHandle;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

static std::string run(LuaVm& vm, const std::string& code) {
    std::string err;
    if (!vm.run(code, "test", &err)) return err.empty() ? "error" : err;
    return {};
}
static bool gtrue(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool b = lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return b;
}
static bool gnil(LuaVm& vm, const char* name) {
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
static std::string gstr(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const char* s = lua_tostring(L, -1);
    std::string r = s ? s : "";
    lua_pop(L, 1);
    return r;
}
static int32_t gid(LuaVm& vm, const char* name) {  // object id of a GameObjectWrapper global, -1 otherwise
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const auto* ud = lua_type(L, -1) == LUA_TUSERDATA ? static_cast<const detail::Ud*>(lua_touserdata(L, -1)) : nullptr;
    const int32_t id = ud ? ud->id : -1;
    lua_pop(L, 1);
    return id;
}
static bool last_message_has(LuaVm& vm, const char* text) {
    return !vm.messages().empty() && vm.messages().back().find(text) != std::string::npos;
}
static void write_file(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}

struct Db {
    fs::path root;
    std::unique_ptr<DirectoryProvider> dp;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    bool ok = false;
    Db() {
        root = fs::temp_directory_path() / "alamo_lua_script_remaining_test";
        std::error_code ec;
        remove_all_long(root, ec);
        write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>T.xml</File></L>");
        write_file(root / "Data/XML/T.xml",
                   "<R>\n"
                   "<SpaceProp Name=\"Nebula_A\"><Behavior>SPACE_OBSTACLE</Behavior><Is_Nebula>Yes</Is_Nebula>"
                   "<Space_Obstacle_Radius>100</Space_Obstacle_Radius></SpaceProp>\n"
                   "<SpaceProp Name=\"Asteroids_A\"><Behavior>SPACE_OBSTACLE</Behavior><Is_Asteroid_Field>Yes</Is_Asteroid_Field>"
                   "<Space_Obstacle_Radius>50</Space_Obstacle_Radius></SpaceProp>\n"
                   "<SpaceProp Name=\"Rocks_A\"><Behavior>SPACE_OBSTACLE</Behavior><Is_Impassable_Asteroid>Yes</Is_Impassable_Asteroid>"
                   "<Space_Obstacle_Radius>50</Space_Obstacle_Radius></SpaceProp>\n"
                   "<SpaceProp Name=\"Storm_A\"><Behavior>SPACE_OBSTACLE</Behavior><Is_Ion_Storm>Yes</Is_Ion_Storm>"
                   "<Space_Obstacle_Radius>80</Space_Obstacle_Radius></SpaceProp>\n"
                   "<Projectile Name=\"Laser_P\"><Projectile_Category>LASER</Projectile_Category></Projectile>\n"
                   "<Projectile Name=\"Missile_P\"><Projectile_Category>MISSILE</Projectile_Category></Projectile>\n"
                   "<SpaceUnit Name=\"Ship\"><Behavior>STARSHIP_LOCOMOTOR</Behavior><CategoryMask>Capital</CategoryMask>"
                   "<AI_Combat_Power>40</AI_Combat_Power><Projectile_Types>Laser_P</Projectile_Types>"
                   "<Energy_Capacity>250</Energy_Capacity><Tactical_Bribe_Cost>300</Tactical_Bribe_Cost></SpaceUnit>\n"
                   "<SpaceUnit Name=\"Fighter\"><CategoryMask>Fighter</CategoryMask><AI_Combat_Power>10</AI_Combat_Power></SpaceUnit>\n"
                   "<GroundStructure Name=\"Bunker\"><Garrison_Category>Infantry</Garrison_Category>"
                   "<Property_Flags>CanContainGarrison | GarrisonCanFire</Property_Flags></GroundStructure>\n"
                   "<GroundStructure Name=\"Pad\"><Behavior>TACTICAL_BUILD_OBJECTS</Behavior></GroundStructure>\n"
                   "<GroundInfantry Name=\"Trooper\"><Behavior>UNIT_AI</Behavior><CategoryMask>Infantry</CategoryMask>"
                   "<Is_Valid_Target>Yes</Is_Valid_Target><Bribe_Modifier_On_Corrupted_Planets>1.5</Bribe_Modifier_On_Corrupted_Planets>"
                   "<Bribe_Modifier_On_Uncorrupted_Planets>0.5</Bribe_Modifier_On_Uncorrupted_Planets></GroundInfantry>\n"
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

static ObjectHandle make(objects::ObjectStore& st, objects::TypeIndex t, int32_t owner, Vec3 p, ObjectHandle attach = {}) {
    objects::SpawnParams sp;
    sp.type = t;
    sp.owner = owner;
    sp.position = p;
    sp.facing = {1.0f, 0.0f, 0.0f};
    sp.attach_parent = attach;
    return st.create(sp);
}

struct Env {
    objects::IdCounter ids;
    objects::ObjectStore store;
    ObjectBridge bridge;
    RemainingScriptHooks hooks;
    GameRandom rng{1234u, 99u};
    std::unique_ptr<LuaVm> vm;
    Env(const Db& db, objects::ModeKind mode) : store(mode, objects::StoreKind::Primary, db.types, &ids) {
        LuaHost host;
        host.store = &store;
        host.types = &db.types;
        host.rng = &rng;
        vm = std::make_unique<LuaVm>(host);
        attach_object_bridge(*vm, &bridge);
    }
    void global(const char* name, ObjectHandle h) { vm->set_object_global(name, h); }
    void player_global(const char* name, int32_t id) {
        detail::Ud u;
        u.cls = detail::cls_player();
        u.id = id;
        lua_State* L = vm->state();
        lua_pushstring(L, name);
        detail::push_engine_userdata(L, u);
        lua_rawset(L, LUA_GLOBALSINDEX);
    }
};

// A minimal AI host: one task force (id 3) with a unit list, one AI target (id 5) -> an object.
struct FakeAi final : AiScriptHost {
    std::vector<ObjectHandle> units;
    ObjectHandle target;
    std::map<std::string, AiGlobalValue> globals;
    int32_t best = -1;
    bool tf_valid(int32_t tf) override { return tf == 3; }
    int32_t tf_force_count(int32_t) override { return static_cast<int32_t>(units.size()); }
    void tf_set_flag(int32_t, AiTfFlag, bool) override {}
    int32_t tf_produce_force(int32_t, const AiArg&, bool) override { return -1; }
    int32_t tf_get_stage(int32_t) override { return -1; }
    bool tf_all_on_free_store(int32_t) override { return false; }
    int32_t tf_order(int32_t, AiTfOrder, const AiArg&) override { return -1; }
    bool tf_is_raid_capable(int32_t) override { return false; }
    void tf_release_forces(int32_t, double) override {}
    void tf_release_unit(int32_t, ObjectHandle) override {}
    std::vector<ObjectHandle> tf_units(int32_t tf) override { return tf == 3 ? units : std::vector<ObjectHandle>{}; }
    std::string tf_goal_type_name(int32_t) override { return {}; }
    bool tf_collect_free_units(int32_t) override { return false; }
    bool status_finished(int32_t) override { return true; }
    AiStatusResult status_result(int32_t) override { return AiStatusResult::Nil; }
    bool target_valid(int32_t t) override { return t == 5; }
    ObjectHandle target_object(int32_t t) override { return t == 5 ? target : ObjectHandle{}; }
    double target_distance(int32_t, const AiArg&) override { return -1.0; }
    double budget_op(int32_t, AiBudgetOp, double, std::string_view) override { return 0.0; }
    int32_t budget_wait(int32_t, AiBudgetOp, double) override { return -1; }
    int32_t free_store_count(int32_t, int) override { return 0; }
    bool free_store_has(int32_t, ObjectHandle) override { return false; }
    bool unit_in_transit(ObjectHandle) override { return false; }
    bool unit_safe(ObjectHandle) override { return true; }
    bool free_store_move(int32_t, ObjectHandle, const AiArg&) override { return false; }
    int32_t find_target(int32_t, const AiFindTargetRequest&) override { return -1; }
    int32_t find_stage_area(int32_t, const AiArg&, const AiArg&, const AiArg&) override { return -1; }
    AiGlobalValue global_value(std::string_view name) override {
        const auto it = globals.find(std::string(name));
        return it == globals.end() ? AiGlobalValue{} : it->second;
    }
    void set_global_value(std::string_view name, const AiGlobalValue& v) override { globals[std::string(name)] = v; }
    bool tf_add_unit(int32_t tf, ObjectHandle u) override {
        if (tf != 3) return false;
        units.push_back(u);
        return true;
    }
    int32_t best_of(int32_t, int32_t, const std::vector<AiArg>&, std::string_view) override { return best; }
};

// ---- the family table, coverage ----
static void test_table() {
    for (const detail::ImplEntry& e : detail::impl_script_remaining()) {
        int same = 0;
        for (const detail::ImplEntry& o : detail::implementations())
            if (o.kind == e.kind && std::strcmp(o.name, e.name) == 0 && std::strcmp(o.cls, e.cls) == 0) ++same;
        if (same != 1) {
            std::printf("  DUPLICATE binding %s.%s (%d implementations)\n", e.cls, e.name, same);
            ++g_fail;
        }
    }
    const BindingCoverage cov = binding_coverage();
    CHECK(cov.globals == cov.globals_implemented + cov.host_values);
    CHECK(cov.methods == cov.methods_implemented);
    CHECK(cov.calls_implemented == cov.calls_total);
    std::printf("  coverage: globals %u (%u implemented, %u host values), methods %u/%u, call sites %llu/%llu\n", cov.globals,
                cov.globals_implemented, cov.host_values, cov.methods_implemented, cov.methods,
                static_cast<unsigned long long>(cov.calls_implemented), static_cast<unsigned long long>(cov.calls_total));
    CHECK(parse_space_field_mask("Asteroid | Nebula") == (kFieldAsteroid | kFieldNebula) &&
          parse_space_field_mask("ion_storm,IonStorm") == kFieldIonStorm && parse_space_field_mask("Bogus") == 0);
}

// ---- singleton calls, GlobalValue (§1.3-§1.5, §1.7) ----
static void test_singletons_and_global_value(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    process_global_values().clear();
    CHECK(run(vm, "a = Budget(); b = DiscreteDistribution(); c = WeightedTypeList(1); d = SFXManager(); g = GlobalValue('x'); "
                  "same = (d == SFXManager)") == "");
    CHECK(gnil(vm, "a") && gnil(vm, "b") && gnil(vm, "c") && gtrue(vm, "same") && gnil(vm, "g"));
    // DiscreteDistribution.Sample: one synced draw; empty -> nothing (no draw).
    GameRandom ref = e.rng;
    CHECK(run(vm, "dd = DiscreteDistribution.Create(); e0 = dd.Sample(); dd.Insert('a', 1); dd.Insert('b', 0); dd.Insert('c', 3); "
                  "s = dd.Sample()") == "");
    CHECK(gnil(vm, "e0") && (gstr(vm, "s") == "a" || gstr(vm, "s") == "c"));
    ref.synced().range_float(0.0f, 4.0f);
    CHECK(ref == e.rng);
    // The process store (no attached store, no AI host): numbers, strings, booleans, tables; unknown -> nothing.
    CHECK(run(vm, "GlobalValue.Set('n', 3); GlobalValue.Set('s', 'txt'); GlobalValue.Set('b', true); "
                  "GlobalValue.Set('t', {1, 2, k = 'v', sub = {x = 4}}); n = GlobalValue.Get('n'); s = GlobalValue.Get('s'); "
                  "bb = GlobalValue.Get('b'); t = GlobalValue.Get('t'); t1 = t[1]; tk = t.k; tx = t.sub.x; u = GlobalValue.Get('none')") == "");
    CHECK(gnum(vm, "n") == 3.0 && gstr(vm, "s") == "txt" && gtrue(vm, "bb") && gnum(vm, "t1") == 1.0 && gstr(vm, "tk") == "v" &&
          gnum(vm, "tx") == 4.0 && gnil(vm, "u"));
    CHECK(process_global_values().size() == 4);
    CHECK(run(vm, "GlobalValue.Set('o', GlobalValue)") == "" && last_message_has(vm, "cannot be a Lua User Variable"));
    CHECK(run(vm, "GlobalValue.Set('o', {GlobalValue})") == "" && last_message_has(vm, "cannot be a Lua User Variable"));
    CHECK(run(vm, "GlobalValue.Get(5)") == "" && last_message_has(vm, "Expected string."));
    CHECK(run(vm, "GlobalValue.Get()") == "" && last_message_has(vm, "Expected 1, got 0"));
    CHECK(run(vm, "GlobalValue.Set('x')") == "" && last_message_has(vm, "Expected 2, got 1"));
    CHECK(run(vm, "GlobalValue.Set('n', nil); n = GlobalValue.Get('n')") == "" && gnil(vm, "n") && process_global_values().size() == 3);
    // Shared by every VM of the process.
    Env other(db, objects::ModeKind::Land);
    CHECK(run(*other.vm, "s2 = GlobalValue.Get('s')") == "" && gstr(*other.vm, "s2") == "txt");
    // Save / load (name order) and the hash.
    ByteWriter w;
    process_global_values().write(w);
    GlobalValueStore copy;
    ByteReader r(w.bytes());
    CHECK(copy.read(r) && r.done() && copy.size() == 3 && copy.get("t").kind == AiGlobalValue::Table && copy.get("t").keys.size() == 4);
    StateHasher h1, h2;
    copy.hash(h1);
    process_global_values().hash(h2);
    CHECK(h1.digest() == h2.digest());
    copy.set("t", AiGlobalValue{});
    StateHasher h3;
    copy.hash(h3);
    CHECK(h1.digest() != h3.digest());
    // An attached store takes over, then (detached) an AI host.
    GlobalValueStore gv;
    attach_global_values(vm, &gv);
    CHECK(run(vm, "GlobalValue.Set('k', 7); k = GlobalValue.Get('k'); s3 = GlobalValue.Get('s')") == "");
    CHECK(gnum(vm, "k") == 7.0 && gnil(vm, "s3") && gv.size() == 1);
    attach_global_values(vm, nullptr);
    FakeAi ai;
    attach_ai_host(vm, &ai, 1);
    CHECK(run(vm, "GlobalValue.Set('q', 2); q = GlobalValue.Get('q')") == "" && gnum(vm, "q") == 2.0 && ai.globals.count("q") == 1);
    CHECK(run(vm, "GlobalValue = nil") == "");
    install_global_value_table(vm);
    CHECK(run(vm, "q2 = GlobalValue.Get('q')") == "" && gnum(vm, "q2") == 2.0);
    attach_ai_host(vm, nullptr, -1);
    process_global_values().clear();
}

// ---- space fields (§1.6), shields (§1.2), weather ----
static void test_fields(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    const ObjectHandle neb = make(e.store, db.t("Nebula_A"), -1, {1000, 0, 0});
    const ObjectHandle ast = make(e.store, db.t("Asteroids_A"), -1, {0, 1000, 0});
    make(e.store, db.t("Rocks_A"), -1, {0, -1000, 0});
    make(e.store, db.t("Storm_A"), -1, {-1000, 0, 0});
    const ObjectHandle ship = make(e.store, db.t("Ship"), 0, {1050, 10, 0});
    e.global("Ship", ship);
    e.global("Neb", neb);
    CHECK(run(vm, "a = Is_Point_In_Nebula(Ship); b = Is_Point_In_Asteroid_Field(Ship); c = Is_Point_In_Ion_Storm(Create_Position(-990, 0, 0)); "
                  "d = Ship.Is_In_Nebula(); f = Ship.Is_In_Ion_Storm(); g = Ship.Is_In_Asteroid_Field(); "
                  "r = Is_Point_In_Asteroid_Field(Create_Position(0, -1000, 0))") == "");
    CHECK(gtrue(vm, "a") && !gtrue(vm, "b") && gtrue(vm, "c") && gtrue(vm, "d") && !gtrue(vm, "f") && !gtrue(vm, "g"));
    CHECK(!gtrue(vm, "r"));  // Is_Impassable_Asteroid does not count (§1.6)
    CHECK(run(vm, "n1 = Find_Nearest_Space_Field(Ship, 'Asteroid'); n2 = Find_Nearest_Space_Field(Ship); "
                  "n3 = Find_Nearest_Space_Field(Create_Position(0, 900, 0), 'Nebula | Asteroid')") == "");
    CHECK(gid(vm, "n2") == e.store.id(neb) && gid(vm, "n1") == e.store.id(ast) && gid(vm, "n3") == e.store.id(ast));
    CHECK(run(vm, "Find_Nearest_Space_Field(Ship, 'Bogus')") == "" && last_message_has(vm, "unknown field type Bogus"));
    CHECK(run(vm, "Find_Nearest_Space_Field(Ship, 3)") == "" && last_message_has(vm, "Expected string."));
    CHECK(run(vm, "Find_Nearest_Space_Field(5)") == "" && last_message_has(vm, "could not deduce source position"));
    CHECK(run(vm, "Is_Point_In_Nebula()") == "" && last_message_has(vm, "Expected 1, got 0"));
    // Hooks override the data fallback.
    e.bridge.remaining = &e.hooks;
    e.hooks.point_in_field = [](Vec3, uint32_t mask) { return mask == kFieldIonStorm; };
    e.hooks.nearest_field = [](Vec3, uint32_t) { return ObjectHandle{}; };
    CHECK(run(vm, "a = Is_Point_In_Nebula(Ship); c = Ship.Is_In_Ion_Storm(); n = Find_Nearest_Space_Field(Ship)") == "");
    CHECK(!gtrue(vm, "a") && gtrue(vm, "c") && gnil(vm, "n"));
    // Space only.
    Env land(db, objects::ModeKind::Land);
    land.global("Ship", make(land.store, db.t("Ship"), 0, {0, 0, 0}));
    CHECK(run(*land.vm, "Find_Nearest_Space_Field(Ship)") == "" && last_message_has(*land.vm, "may only be used in space mode"));
    // Shield: land mode only; false without the system; errors; the hooks.
    CHECK(run(vm, "Are_On_Opposite_Sides_Of_Shield(Ship, Neb)") == "" && last_message_has(vm, "only valid in land mode"));
    LuaVm& lv = *land.vm;
    land.player_global("P", 0);
    CHECK(run(lv, "s = Are_On_Opposite_Sides_Of_Shield(Ship, Create_Position(5, 5, 0))") == "" && !gtrue(lv, "s") && !gnil(lv, "s"));
    CHECK(run(lv, "Are_On_Opposite_Sides_Of_Shield(Ship)") == "" && last_message_has(lv, "Expected 2 or 4, got 1"));
    CHECK(run(lv, "Are_On_Opposite_Sides_Of_Shield(Ship, Ship, 1, 2)") == "" && last_message_has(lv, "Expected player."));
    CHECK(run(lv, "Are_On_Opposite_Sides_Of_Shield(Ship, Ship, P, 2)") == "" && last_message_has(lv, "Expected boolean."));
    CHECK(run(lv, "Are_On_Opposite_Sides_Of_Shield(1, Ship)") == "" && last_message_has(lv, "parameter 1"));
    land.bridge.remaining = &land.hooks;
    land.hooks.opposite_sides_of_shield = [](Vec3 a, Vec3 b, int32_t p) { return a.x < b.x && p == -1; };
    land.hooks.opposite_sides_of_shield4 = [](Vec3, Vec3, int32_t p, bool friendly) { return p == 0 && friendly; };
    CHECK(run(lv, "s = Are_On_Opposite_Sides_Of_Shield(Ship, Create_Position(5, 0, 0)); s4 = Are_On_Opposite_Sides_Of_Shield(Ship, Ship, P, true)") ==
          "");
    CHECK(gtrue(lv, "s") && gtrue(lv, "s4"));
    // Weather.
    CHECK(run(vm, "Force_Weather()") == "" && last_message_has(vm, "no weather system"));
    int forced = 0;
    e.hooks.force_weather = [&forced] {
        ++forced;
        return true;
    };
    CHECK(run(vm, "Force_Weather()") == "" && forced == 1);
}

// ---- Find_Best_Local_Threat_Center (§1.1) ----
static void test_threat_center(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    e.global("A", make(e.store, db.t("Ship"), 1, {0, 0, 0}));
    e.global("B", make(e.store, db.t("Fighter"), 1, {100, 0, 0}));
    e.global("C", make(e.store, db.t("Fighter"), 1, {5000, 0, 0}));
    // n = 3: k = 1, the mean position, no draw.
    GameRandom ref = e.rng;
    CHECK(run(vm, "p, t = Find_Best_Local_Threat_Center({C, B, A}, 4000); x, y, z = p.Get_XYZ()") == "");
    CHECK(gnum(vm, "t") == 60.0 && gnum(vm, "x") == 1700.0 && gnum(vm, "y") == 0.0 && gnum(vm, "z") == 0.0 && ref == e.rng);
    // Nothing in range of the centre -> nothing.
    CHECK(run(vm, "p, t = Find_Best_Local_Threat_Center({A, B, C}, 150)") == "" && gnil(vm, "p") && gnil(vm, "t"));
    // n = 5: k = 2, exactly four synced draws (two per centre), then Lloyd; the dense pair wins.
    e.global("D", make(e.store, db.t("Ship"), 1, {120, 0, 0}));
    e.global("E", make(e.store, db.t("Fighter"), 1, {5100, 0, 0}));
    CHECK(run(vm, "p, t = Find_Best_Local_Threat_Center({A, B, C, D, E}, 300); x, y = p.Get_XYZ()") == "");
    for (int i = 0; i < 4; ++i) ref.synced().range_float(0.0f, 1.0f);
    CHECK(ref == e.rng && gnum(vm, "t") == 90.0 && gnum(vm, "x") > 60.0 && gnum(vm, "x") < 80.0);
    CHECK(run(vm, "p = Find_Best_Local_Threat_Center({}, 150)") == "" && gnil(vm, "p") && last_message_has(vm, "no objects supplied"));
    CHECK(run(vm, "Find_Best_Local_Threat_Center({A, 5}, 150)") == "");
    bool warned = false;
    for (const std::string& m : vm.messages()) warned = warned || m.find("SCRIPT WARNING: Find_Best_Local_Threat_Center -- invalid type for entry at index 2") != std::string::npos;
    CHECK(warned);
    CHECK(run(vm, "Find_Best_Local_Threat_Center(A, 150)") == "" && last_message_has(vm, "Expected table."));
    CHECK(run(vm, "Find_Best_Local_Threat_Center({A}, 'x')") == "" && last_message_has(vm, "Expected number."));
    CHECK(run(vm, "Find_Best_Local_Threat_Center({A})") == "" && last_message_has(vm, "Expected 2, got 1"));
}

// ---- GameObjectWrapper / type methods (§2.1-§2.8, §2.12) ----
static void test_object_methods(const Db& db) {
    Env e(db, objects::ModeKind::Land);
    LuaVm& vm = *e.vm;
    const ObjectHandle ship = make(e.store, db.t("Ship"), 0, {0, 0, 0});
    const ObjectHandle bunker = make(e.store, db.t("Bunker"), 0, {10, 0, 0});
    const ObjectHandle pad = make(e.store, db.t("Pad"), 0, {20, 0, 0});
    const ObjectHandle trooper = make(e.store, db.t("Trooper"), 0, {30, 0, 0});
    e.global("Ship", ship);
    e.global("Bunker", bunker);
    e.global("Pad", pad);
    e.global("Trooper", trooper);
    // Without hooks.
    CHECK(run(vm, "cf = Bunker.Can_Garrison_Fire(); cs = Ship.Can_Garrison_Fire(); dv = Ship.Divert(Pad); od = Ship.Is_On_Diversion(); "
                  "pw = Ship.Get_AI_Power_Vs_Unit(Bunker); en = Ship.Get_Energy(); en0 = Bunker.Get_Energy(); fb = Ship.Get_Final_Blow_Player(); "
                  "rd = Ship.Get_Rate_Of_Damage_Taken(); td = Ship.Get_Time_Till_Dead(); bc = Pad.Get_Build_Pad_Contents()") == "");
    CHECK(gtrue(vm, "cf") && !gtrue(vm, "cs") && !gtrue(vm, "dv") && !gtrue(vm, "od") && !gnil(vm, "od"));
    CHECK(gnum(vm, "pw") == 40.0 && gnum(vm, "en") == 1.0 && gnum(vm, "en0") == 0.0 && gnil(vm, "fb"));
    CHECK(gnum(vm, "rd") == 0.0 && gnum(vm, "td") == 1.0e18 && gnil(vm, "bc"));
    CHECK(run(vm, "Ship.Divert(Pad)") == "" && last_message_has(vm, "between moves"));
    CHECK(run(vm, "Bunker.Divert(Pad)") == "" && last_message_has(vm, "this object cannot move"));
    CHECK(run(vm, "Bunker.Is_On_Diversion()") == "" && last_message_has(vm, "this object cannot move"));
    CHECK(run(vm, "ea = Ship.Explore_Area(Pad)") == "" && gnil(vm, "ea") && last_message_has(vm, "Expected ai target location."));
    CHECK(run(vm, "ft = Ship.Force_Test_Space_Conflict()") == "" && gnil(vm, "ft") && last_message_has(vm, "is not a fleet or a planet"));
    CHECK(run(vm, "Ship.Get_AI_Power_Vs_Unit(4)") == "" && last_message_has(vm, "Expected object or ai target."));
    CHECK(run(vm, "Ship.Get_Build_Pad_Contents()") == "" && last_message_has(vm, "is not a build pad"));
    // Build on a pad (no host): rules off -> the UC object is created and becomes the contents; rules on -> refused.
    CHECK(run(vm, "Ship.Build('Fighter')") == "" && last_message_has(vm, "is not a valid build location"));
    CHECK(run(vm, "Pad.Build('No_Such_Type')") == "" && last_message_has(vm, "unknown game object type No_Such_Type"));
    CHECK(run(vm, "r = Pad.Build('Fighter', true)") == "" && !gtrue(vm, "r") && !gnil(vm, "r") && last_message_has(vm, "game rules prevent"));
    CHECK(run(vm, "Pad.Build('Fighter', 1)") == "" && last_message_has(vm, "Expected boolean."));
    CHECK(run(vm, "Pad.Lock_Build_Pad_Contents(true)") == "" && last_message_has(vm, "cannot lock an empty build pad"));
    CHECK(run(vm, "r = Pad.Build('Fighter'); bc = Pad.Get_Build_Pad_Contents(); bn = bc.Get_Type().Get_Name()") == "");
    CHECK(gtrue(vm, "r") && gstr(vm, "bn") == "Fighter");
    CHECK(run(vm, "Pad.Build('Fighter')") == "" && last_message_has(vm, "already has something built on it"));
    // Projectiles without the combat hook: the type's Projectile_Types.
    CHECK(run(vm, "pt = Ship.Get_All_Projectile_Types(); n = table.getn(pt); p1 = pt[1].Get_Name(); "
                  "cp = Ship.Get_Current_Projectile_Type().Get_Name(); none = table.getn(Pad.Get_All_Projectile_Types()); "
                  "cn = Pad.Get_Current_Projectile_Type()") == "");
    CHECK(gnum(vm, "n") == 1.0 && gstr(vm, "p1") == "Laser_P" && gstr(vm, "cp") == "Laser_P" && gnum(vm, "none") == 0.0 && gnil(vm, "cn"));
    // State setters.
    const int loco = objects::behavior_type_id("STARSHIP_LOCOMOTOR");
    CHECK(run(vm, "Pad.Lock_Build_Pad_Contents(true); Ship.Enable_Behavior(" + std::to_string(loco) + ", false); Ship.Enable_Behavior(78, true); "
                  "Ship.Enable_Dynamic_LOD(false); Ship.Set_Targeting_Stickiness_Time_Threshold(2.5); Trooper.Set_Prefer_Ground_Over_Space(true)") ==
          "");
    const RemainingObjectState* ps = e.bridge.find_remaining_state(e.store.id(pad));
    const RemainingObjectState* ss = e.bridge.find_remaining_state(e.store.id(ship));
    const RemainingObjectState* ts = e.bridge.find_remaining_state(e.store.id(trooper));
    CHECK(ps && ps->build_pad_locked && ss && !ss->dynamic_lod && ss->behaviors.size() == 1 && ss->behaviors.at(loco) == false &&
          ss->stickiness == 2.5f && ts && ts->prefer_ground == 1);
    CHECK(run(vm, "Ship.Set_Prefer_Ground_Over_Space(true)") == "" && last_message_has(vm, "missing Unit_AI behavior"));
    CHECK(run(vm, "Ship.Set_Prefer_Ground_Over_Space()") == "" && last_message_has(vm, "Missing required boolean parameter."));
    CHECK(run(vm, "Ship.Enable_Behavior('x', true)") == "" && last_message_has(vm, "Expected number."));
    CHECK(run(vm, "Ship.Enable_Behavior(200, true)") == "" && last_message_has(vm, "invalid behavior 200"));
    CHECK(run(vm, "Ship.Enable_Behavior(1, 2)") == "" && last_message_has(vm, "Expected a bool."));
    CHECK(run(vm, "Ship.Enable_Dynamic_LOD()") == "" && last_message_has(vm, "command takes a single boolean value"));
    CHECK(run(vm, "Ship.Enable_Dynamic_LOD(1)") == "" && last_message_has(vm, "Expected boolean."));
    CHECK(run(vm, "Pad.Lock_Build_Pad_Contents()") == "" && last_message_has(vm, "Expected 1, got 0"));
    CHECK(run(vm, "Bunker.Lock_Build_Pad_Contents(true)") == "" && last_message_has(vm, "is not a build pad"));
    CHECK(run(vm, "Ship.Set_Targeting_Stickiness_Time_Threshold('a')") == "" && last_message_has(vm, "Expected number."));
    {  // The state is hashed only when present.
        ObjectBridge empty;
        StateHasher a, b;
        empty.hash(a);
        e.bridge.hash(b);
        CHECK(a.digest() != b.digest());
    }
    // Release: the wrapper becomes dead and leaves the cache (a new wrapper is made next time).
    const ObjectHandle spare = make(e.store, db.t("Fighter"), 0, {50, 0, 0});
    e.global("Spare", spare);
    CHECK(run(vm, "old = Spare; r = Spare.Release(); v = old.Is_Valid()") == "" && gnil(vm, "r") && !gtrue(vm, "v"));
    e.global("Spare2", spare);
    CHECK(run(vm, "v2 = Spare2.Is_Valid(); fresh = (Spare2 ~= old)") == "" && gtrue(vm, "v2") && gtrue(vm, "fresh"));
    // Hooks.
    e.bridge.remaining = &e.hooks;
    Vec3 goal{};
    bool has_goal = false;
    e.hooks.move_to = [&](ObjectHandle, Vec3 p) {
        goal = p;
        has_goal = true;
        return true;
    };
    bool report = true;
    e.hooks.move_goal = [&](ObjectHandle, Vec3& g) {
        g = goal;
        return has_goal && report;
    };
    e.hooks.energy = [](ObjectHandle) { return 125.0f; };
    e.hooks.final_blow_player = [](ObjectHandle) { return 1; };
    e.hooks.rate_of_damage = [](ObjectHandle) { return 30.0; };
    e.hooks.time_till_dead = [](ObjectHandle) { return 4.0; };
    e.hooks.build_pad_contents = [bunker](ObjectHandle) { return bunker; };
    e.hooks.current_projectile = [&db](ObjectHandle) { return db.t("Missile_P"); };
    e.hooks.projectile_types = [&db](ObjectHandle) {
        return std::vector<objects::TypeIndex>{db.t("Missile_P"), db.t("Laser_P"), db.t("Missile_P"), objects::kNone};
    };
    e.hooks.ai_power_vs = [ship](ObjectHandle u, ObjectHandle) { return u == ship ? 2.0 : 1.0; };
    e.hooks.can_garrison_fire = [ship](ObjectHandle h) { return h == ship; };
    bool locked = false;
    e.hooks.lock_build_pad = [&locked](ObjectHandle, bool l) { locked = l; };
    CHECK(run(vm, "dv = Ship.Divert(Pad, 3); od = Ship.Is_On_Diversion(); en = Ship.Get_Energy(); fb = Ship.Get_Final_Blow_Player(); "
                  "rd = Ship.Get_Rate_Of_Damage_Taken(); td = Ship.Get_Time_Till_Dead(); bc = (Pad.Get_Build_Pad_Contents() == Bunker); "
                  "cp = Ship.Get_Current_Projectile_Type().Get_Name(); pw = Ship.Get_AI_Power_Vs_Unit(Bunker); "
                  "cf = Bunker.Can_Garrison_Fire(); cs = Ship.Can_Garrison_Fire(); Pad.Lock_Build_Pad_Contents(true)") == "");
    CHECK(gtrue(vm, "dv") && gtrue(vm, "od") && gnum(vm, "en") == 0.5 && !gnil(vm, "fb") && gnum(vm, "rd") == 30.0 && gnum(vm, "td") == 4.0 &&
          gtrue(vm, "bc") && gstr(vm, "cp") == "Missile_P" && gnum(vm, "pw") == 80.0 && !gtrue(vm, "cf") && gtrue(vm, "cs") && locked);
    CHECK(goal.x == 20.0f && goal.y == 0.0f);
    goal = Vec3{99.0f, 0.0f, 0.0f};  // another move replaced the diversion
    CHECK(run(vm, "od = Ship.Is_On_Diversion()") == "" && !gtrue(vm, "od"));
    report = false;  // the host reports no goal after the move (it ended at once)
    CHECK(run(vm, "dv = Ship.Divert(Pad); has = Ship.Is_On_Diversion()") == "" && gtrue(vm, "dv") && !gtrue(vm, "has"));
    CHECK(run(vm, "pt = Ship.Get_All_Projectile_Types(); n = table.getn(pt); p1 = pt[1].Get_Name(); p3 = pt[3].Get_Name()") == "");
    CHECK(gnum(vm, "n") == 3.0 && gstr(vm, "p1") == "Missile_P" && gstr(vm, "p3") == "Missile_P");  // duplicates kept
    // Locked movement.
    e.bridge.state(e.store.id(ship)).orders_locked = true;
    CHECK(run(vm, "dv = Ship.Divert(Pad)") == "" && !gtrue(vm, "dv") && last_message_has(vm, "has had its movement locked"));
    e.bridge.state(e.store.id(ship)).orders_locked = false;
    // Types: Get_Bribe_Cost (§2.8).
    CHECK(run(vm, "b0 = Ship.Get_Type().Get_Bribe_Cost(); b1 = Ship.Get_Type().Get_Bribe_Cost(Trooper)") == "");
    CHECK(gnum(vm, "b0") == 300.0 && gnum(vm, "b1") == 300.0);  // no battle planet: the base cost
    int corrupted = 1;
    e.hooks.battle_planet_corruption = [&corrupted] { return corrupted; };
    CHECK(run(vm, "b1 = Ship.Get_Type().Get_Bribe_Cost(Trooper)") == "" && gnum(vm, "b1") == 450.0);
    corrupted = 0;
    CHECK(run(vm, "b1 = Ship.Get_Type().Get_Bribe_Cost(Trooper)") == "" && gnum(vm, "b1") == 150.0);
    CHECK(run(vm, "Ship.Get_Type().Get_Bribe_Cost(3)") == "" && last_message_has(vm, "Expected game object wrapper."));
    // Get_Energy is tactical only.
    Env gal(db, objects::ModeKind::Galactic);
    gal.global("Ship", make(gal.store, db.t("Ship"), 0, {0, 0, 0}));
    CHECK(run(*gal.vm, "Ship.Get_Energy()") == "" && last_message_has(*gal.vm, "only valid in tactical mode"));
}

static void test_player_methods(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    e.player_global("P", 0);
    e.global("Ship", make(e.store, db.t("Ship"), 0, {0, 0, 0}));
    CHECK(run(vm, "id = P.Get_GameSpy_Stats_Player_ID(); t = P.Give_Random_Sliceable_Tech(); ob = Ship.Get_Type().Is_Obsolete(P)") == "");
    CHECK(gnum(vm, "id") == -1.0 && gnil(vm, "t") && !gtrue(vm, "ob"));
    e.bridge.remaining = &e.hooks;
    e.hooks.give_random_sliceable_tech = [&db](int32_t) { return db.t("Fighter"); };
    e.hooks.type_obsolete = [](objects::TypeIndex, int32_t p) { return p == 0; };
    CHECK(run(vm, "t = P.Give_Random_Sliceable_Tech().Get_Name(); ob = Ship.Get_Type().Is_Obsolete(P)") == "");
    CHECK(gstr(vm, "t") == "Fighter" && gtrue(vm, "ob"));
    CHECK(run(vm, "Ship.Get_Type().Is_Obsolete()") == "" && last_message_has(vm, "Expected 1, got 0"));
    CHECK(run(vm, "Ship.Get_Type().Is_Obsolete(1)") == "" && last_message_has(vm, "Expected player"));
    // _ProduceObject (§1.9): argument checks; refused without a galactic sim.
    CHECK(run(vm, "_ProduceObject(P, 'Fighter')") == "" && last_message_has(vm, "Player: Ok, Object: Ok, Planet: Invalid"));
    bool count_logged = false;
    for (const std::string& m : vm.messages()) count_logged = count_logged || m.find("Invalid Number of parameters: 2 should be 3.") != std::string::npos;
    CHECK(count_logged);
    CHECK(run(vm, "_ProduceObject(P, 'Nope', Ship)") == "" && last_message_has(vm, "Player: Ok, Object: Invalid, Planet: Ok"));
    CHECK(run(vm, "r = _ProduceObject(P, 'Fighter', Ship)") == "" && gnil(vm, "r") && last_message_has(vm, "Scheduling Production Event Failed!"));
}

// ---- task forces (§2.5, §2.10, §2.12) ----
static void test_task_forces(const Db& db) {
    Env e(db, objects::ModeKind::Land);
    LuaVm& vm = *e.vm;
    FakeAi ai;
    attach_ai_host(vm, &ai, 1);
    const ObjectHandle t1 = make(e.store, db.t("Trooper"), 0, {0, 0, 0});
    const ObjectHandle t2 = make(e.store, db.t("Trooper"), 0, {10, 0, 0});
    const ObjectHandle ship = make(e.store, db.t("Ship"), 0, {20, 0, 0});
    const ObjectHandle bunker = make(e.store, db.t("Bunker"), 0, {30, 0, 0});
    make(e.store, db.t("Trooper"), 1, {1000, 0, 0});
    const ObjectHandle enemy_near = make(e.store, db.t("Trooper"), 1, {200, 0, 0});
    const ObjectHandle enemy_ship = make(e.store, db.t("Ship"), 1, {50, 0, 0});  // enemy capital (not Infantry)
    ai.units = {t1, t2};
    ai.target = bunker;
    set_task_force_global(vm, "TF", 3, AiTaskForceKind::Land);
    set_ai_target_global(vm, "Tgt", 5);
    e.global("Ship", ship);
    e.global("Bunker", bunker);
    e.global("T1", t1);
    CHECK(run(vm, "a = TF.Add_Force(Ship); n = TF.Get_Force_Count(); ty = TF.Get_Type_Of_Unit(2).Get_Name(); "
                  "t0 = TF.Get_Type_Of_Unit(0).Get_Name(); t9 = TF.Get_Type_Of_Unit(9); tn = TF.Get_Type_Of_Unit(-1)") == "");
    CHECK(gnil(vm, "a") && gnum(vm, "n") == 3.0 && gstr(vm, "ty") == "Ship" && gstr(vm, "t0") == "Trooper" && gnil(vm, "t9") && gnil(vm, "tn"));
    CHECK(run(vm, "TF.Add_Force(3)").find("expected a GameObjectClass") != std::string::npos);
    CHECK(run(vm, "TF.Get_Type_Of_Unit('x')").find("expected an integer index") != std::string::npos);
    // Opportunity-fire subscriptions: weapon hard-point types only.
    CHECK(run(vm, "TF.Add_Opportunity_Fire_Event_Subscription('HARD_POINT_WEAPON_LASER'); "
                  "TF.Add_Opportunity_Fire_Event_Subscription('hard_point_weapon_torpedo')") == "");
    const std::vector<int32_t>* subs = ai.opportunity_fire_subscriptions(3);
    CHECK(subs && *subs == (std::vector<int32_t>{5, 7}));
    CHECK(run(vm, "TF.Add_Opportunity_Fire_Event_Subscription('HARD_POINT_WEAPON_LASER')") == "" && last_message_has(vm, "already subscribed"));
    CHECK(run(vm, "TF.Remove_Opportunity_Fire_Event_Subscription('HARD_POINT_WEAPON_MISSILE')") == "" && last_message_has(vm, "is not subscribed"));
    CHECK(run(vm, "TF.Remove_Opportunity_Fire_Event_Subscription('HARD_POINT_WEAPON_LASER')") == "" && *subs == std::vector<int32_t>{7});
    CHECK(run(vm, "TF.Add_Opportunity_Fire_Event_Subscription('HARD_POINT_ENGINE')") == "" && last_message_has(vm, "not a weapon hard point type"));
    CHECK(run(vm, "TF.Add_Opportunity_Fire_Event_Subscription('X')") == "" && last_message_has(vm, "unknown hard point type X"));
    CHECK(run(vm, "TF.Add_Opportunity_Fire_Event_Subscription(1)") == "" && last_message_has(vm, "Expected string"));
    CHECK(run(vm, "TF.Clear_Opportunity_Fire_Event_Subscriptions()") == "" && subs->empty());
    // Garrison delegation (objects family Can_Garrison / Garrison).
    CHECK(run(vm, "cg = TF.Can_Garrison(Bunker); cg2 = TF.Can_Garrison(Tgt); cg3 = TF.Can_Garrison(Ship); TF.Garrison(Tgt); TF.Leave_Garrison()") ==
          "");
    CHECK(gtrue(vm, "cg") && gtrue(vm, "cg2") && !gtrue(vm, "cg3"));
    CHECK(e.bridge.orders_issued == 6);  // Garrison and Leave_Garrison for each of the three units
    CHECK(run(vm, "TF.Can_Garrison(4)") == "" && last_message_has(vm, "Expected game object or AI target."));
    // Power, stickiness, autofire.
    CHECK(run(vm, "pw = TF.Get_AI_Power_Vs_Unit(Tgt); TF.Set_Targeting_Stickiness_Time_Threshold(2); "
                  "TF.Set_Targeting_Stickiness_Time_Threshold(4, 'Infantry'); TF.Set_Targeting_Stickiness_Time_Threshold(9, 'Fighter'); "
                  "TF.Set_All_Abilities_Autofire(true); TF.Set_Single_Ability_Autofire('SPREAD_OUT', false)") == "");
    CHECK(gnum(vm, "pw") == 40.0);
    const RemainingObjectState* ss = e.bridge.find_remaining_state(e.store.id(ship));
    const RemainingObjectState* ts = e.bridge.find_remaining_state(e.store.id(t1));
    CHECK(!ss && ts && ts->stickiness == 4.0f);  // the ship has no UNIT_AI
    CHECK(run(vm, "TF.Set_Targeting_Stickiness_Time_Threshold('2')") == "" && last_message_has(vm, "Expected string."));
    CHECK(run(vm, "TF.Set_Targeting_Stickiness_Time_Threshold(2, 'No_Such')") == "" && last_message_has(vm, "parameter 2"));
    // OI lua: Set_Targeting_Priorities(set [, type name or category]) reaches the units (script state here; combat units
    // get the S8 override), later calls overriding earlier ones for the units they match.
    CHECK(run(vm, "TF.Set_Targeting_Priorities('Infantry_Attack_Move'); TF.Set_Targeting_Priorities('Capital_Set', 'Capital'); "
                  "TF.Set_Targeting_Priorities('Trooper_Set', 'trooper'); TF.Set_Targeting_Priorities('Nope_Set', 'Fighter')") == "");
    {
        const ScriptObjectState* s1 = e.bridge.find_state(e.store.id(t1));
        const ScriptObjectState* s2 = e.bridge.find_state(e.store.id(t2));
        const ScriptObjectState* s3 = e.bridge.find_state(e.store.id(ship));
        CHECK(s1 && s2 && s3 && s1->targeting_priorities == "Trooper_Set" && s2->targeting_priorities == "Trooper_Set" &&
              s3->targeting_priorities == "Capital_Set");
    }
    CHECK(run(vm, "TF.Set_Targeting_Priorities()").find("Expected 1 or 2, got 0") != std::string::npos);
    CHECK(run(vm, "TF.Set_Targeting_Priorities('a', 3)").find("parameter 2. Expected string.") != std::string::npos);
    // Find_Closest_Enemy.
    CHECK(run(vm, "ce = TF.Find_Closest_Enemy('Infantry'); ca = TF.Find_Closest_Enemy(); cx = TF.Find_Closest_Enemy('Fighter')") == "");
    CHECK(gid(vm, "ce") == e.store.id(enemy_near) && gid(vm, "ca") == e.store.id(enemy_ship) && gnil(vm, "cx"));
    CHECK(run(vm, "TF.Find_Closest_Enemy(1)") == "" && last_message_has(vm, "Expected string."));
    CHECK(run(vm, "TF.Find_Closest_Enemy('Infantry', 2)") == "" && last_message_has(vm, "expected 1 or 2, got 2"));
    CHECK(run(vm, "TF.Find_Closest_Enemy('Bogus')") == "" && last_message_has(vm, "unknown category Bogus"));
    // Bombing run / orbital bombardment.
    CHECK(run(vm, "br = TF.Bombing_Run(Tgt)") == "" && gnil(vm, "br") && last_message_has(vm, "No bombing run available"));
    CHECK(run(vm, "ob = TF.Fire_Orbital_Bombardment(Create_Position(1, 2, 3))") == "" && gnil(vm, "ob"));
    e.bridge.remaining = &e.hooks;
    Vec3 hit{};
    int fired = 0;
    e.hooks.orbital_bombardment = [&](int32_t p, Vec3 v) {
        hit = v;
        fired += p == 0 ? 1 : 0;
        return true;
    };
    e.hooks.bombing_run = [](int32_t, Vec3) { return true; };
    CHECK(run(vm, "ob = TF.Fire_Orbital_Bombardment(Create_Position(1, 2, 3)); br = TF.Bombing_Run(Tgt); done = br.IsFinished()") == "");
    CHECK(gnil(vm, "ob") && fired == 1 && hit.x == 1.0f && hit.y == 2.0f && hit.z == 3.0f && gtrue(vm, "done"));
    CHECK(run(vm, "TF.Bombing_Run(Create_Position(1, 2, 3))") == "" && last_message_has(vm, "Exepcted game objecdt or AI target"));
    CHECK(run(vm, "TF.Fire_Orbital_Bombardment(7)") == "" && last_message_has(vm, "could not extract a position"));
    // Best_Of: the host's pick indexes the valid entries.
    ai.best = 1;
    CHECK(run(vm, "bo = FindTarget.Best_Of(TF, {Tgt, 5, Bunker}, 'Some_Perception'); ok = (bo == Bunker)") == "" && gtrue(vm, "ok"));
    ai.best = -1;
    CHECK(run(vm, "bo = FindTarget.Best_Of(TF, {Tgt}, 'P')") == "" && gnil(vm, "bo"));
    CHECK(run(vm, "FindTarget.Best_Of(Ship, {}, 'P')") == "" && last_message_has(vm, "Expected taskforce."));
    CHECK(run(vm, "FindTarget.Best_Of(TF, 1, 'P')") == "" && last_message_has(vm, "Expected table of targets."));
    CHECK(run(vm, "FindTarget.Best_Of(TF, {}, 1)") == "" && last_message_has(vm, "unrecognized perception function"));
    // Refit_To_Definition / Explore_Area without host support.
    set_task_force_global(vm, "GTF", 3, AiTaskForceKind::Galactic);
    CHECK(run(vm, "rf = GTF.Refit_To_Definition(Bunker, 100); ea = T1.Explore_Area(Tgt)") == "" && gnil(vm, "rf") && gnil(vm, "ea"));
    CHECK(run(vm, "GTF.Refit_To_Definition(Bunker)") == "" && last_message_has(vm, "Expected 2, got 1"));
    CHECK(run(vm, "GTF.Refit_To_Definition(Bunker, 'x')") == "" && last_message_has(vm, "Expected number."));

    // OI lua (A1 ask 2): objects-family position arguments take AI targets (-> their object) and task forces (-> the
    // first live unit), without the AI VMs' Lua shim.
    set_ai_target_global(vm, "Gone", 9);  // not a valid target: no object
    // refixf t03: Find_Nearest candidates are UNIT_AI objects (the troopers; the ship has no UNIT_AI).
    CHECK(run(vm, "n1 = Find_Nearest(TF, 'Trooper'); n2 = Find_Nearest(Tgt, 'Trooper'); n3 = Find_Nearest(Gone)") == "");
    CHECK(gid(vm, "n1") == e.store.id(t2) && gid(vm, "n2") == e.store.id(t2) && gnil(vm, "n3"));
    const uint32_t before = e.bridge.orders_issued;
    CHECK(run(vm, "s1 = Ship.Attack_Move(Tgt); s2 = Ship.Move_To(TF); s3 = Ship.Guard_Target(Tgt)") == "");
    CHECK(e.bridge.orders_issued == before + 3 && !gnil(vm, "s1") && !gnil(vm, "s2"));
    CHECK(run(vm, "Ship.Move_To(Gone)").find("could not extract a position") != std::string::npos);
    ai.units.clear();
    CHECK(run(vm, "n5 = Find_Nearest(TF)") == "" && gnil(vm, "n5"));
    attach_ai_host(vm, nullptr, -1);
    CHECK(run(vm, "n6 = Find_Nearest(Tgt)") == "" && gnil(vm, "n6"));  // no host: an AI wrapper resolves to nothing
}

#if ALAMO_LUA_PLAYERS
// OI lua (A1 ask 2): EvaluatePerception goes through an attached tactical AI host (AI target / task force targets).
struct FakeTactical final : AiTacticalScriptHost {
    std::string name;
    int32_t player = -2, target = -2, tf = -2, calls = 0;
    int32_t ambush = -1, ambush_calls = 0, ambush_target = -2, direction = -2;  // S7 t21 Prepare_Ambush
    double min_dist = 0.0, threat = 0.0;
    bool zone_path = false;
    int32_t tf_prepare_ambush(int32_t, const AiArg& t, int dir, double dist, double thr, bool zone, std::string* n) override {
        ++ambush_calls;
        ambush_target = t.target;
        direction = dir;
        min_dist = dist;
        threat = thr;
        zone_path = zone;
        if (n) *n = "Gone_Target";
        return ambush;
    }
    int32_t tf_tactical_order(int32_t, AiTacticalOrder, const AiArg&, const AiPoint&, double, bool) override { return -1; }
    int32_t tf_attack_target(int32_t, const AiArg&, std::string_view) override { return -1; }
    int32_t tf_reinforce(int32_t, const AiArg&, const AiPoint&, double) override { return -1; }
    int32_t tf_build_all(int32_t) override { return -1; }
    int32_t tf_build(int32_t, std::string_view, const AiArg&) override { return -1; }
    std::vector<ObjectHandle> tf_reserved_pads(int32_t) override { return {}; }
    bool tf_withdraw(int32_t) override { return false; }
    double tf_self_threat(int32_t, bool) override { return 0.0; }
    double tf_distance(int32_t, const AiArg&, const AiPoint&) override { return -1.0; }
    void tf_set_targeting_priorities(int32_t, std::string_view) override {}
    ObjectHandle find_deadly_enemy(int32_t, const AiArg&, double, int32_t) override { return {}; }
    bool most_defended_position(int32_t, const AiArg&, const AiPoint&, int32_t, AiPoint&) override { return false; }
    bool project_by_unit_range(ObjectHandle, const AiArg&, const AiPoint&, AiPoint&) override { return false; }
    int is_good_against(ObjectHandle, ObjectHandle) override { return -1; }
    bool evaluate_perception(int32_t, std::string_view n, int32_t p, const AiArg& t, double& out) override {
        name = std::string(n);
        player = p;
        target = t.target;
        tf = t.task_force;
        ++calls;
        out = 7.5;
        return n != "Missing";
    }
};

static void test_tactical_perception(const Db& db) {
    Env e(db, objects::ModeKind::Land);
    LuaVm& vm = *e.vm;
    FakeTactical th;
    attach_ai_tactical_host(vm, &th);
    set_ai_target_global(vm, "AITarget", 5);
    set_task_force_global(vm, "TF", 3, AiTaskForceKind::Land);
    e.player_global("P", 1);
    CHECK(run(vm, "a = EvaluatePerception('Syn_Destroy_Desire', P, AITarget); b = EvaluatePerception('Other', P, TF); "
                  "c = EvaluatePerception('Self_Only', P); d = EvaluatePerception('Missing', P)") == "");
    CHECK(gnum(vm, "a") == 7.5 && gnum(vm, "b") == 7.5 && gnum(vm, "c") == 7.5 && gnil(vm, "d") && th.calls == 4);
    CHECK(th.name == "Missing" && th.player == 1 && th.target == -1 && th.tf == -1);
    CHECK(run(vm, "EvaluatePerception('X', P, TF)") == "" && th.tf == 3 && th.target == -1);
    CHECK(run(vm, "EvaluatePerception(1, P)").find("Expected string") != std::string::npos);
    // S7 t20 (AI_TACTICAL §7.3.2): Move_To argument errors are script errors (logged, no unwind) answering nil.
    CHECK(run(vm, "m1 = TF.Move_To()") == "" && gnil(vm, "m1") && last_message_has(vm, "SCRIPT ERROR: No destination set for TaskForce"));
    CHECK(run(vm, "m2 = TF.Move_To(nil)") == "" && gnil(vm, "m2") &&
          last_message_has(vm, "Parameter 1 is not a valid destination for TaskForce"));
    const size_t said = vm.messages().size();
    CHECK(run(vm, "m3 = TF.Move_To(P)") == "" && gnil(vm, "m3") && vm.messages().size() == said + 1);
    // S7 t21 (§7.3.1): Prepare_Ambush argument checks (land: 4 or 5 arguments), then the host's status / error codes.
    CHECK(run(vm, "a1 = TF.Prepare_Ambush(AITarget, 'Left', 100)") == "" && gnil(vm, "a1") &&
          last_message_has(vm, "Invalid number of parameters"));
    CHECK(run(vm, "a2 = TF.Prepare_Ambush(AITarget, 'up', 100, 5)") == "" && gnil(vm, "a2") && last_message_has(vm, "Unrecognized direction up"));
    CHECK(run(vm, "a3 = TF.Prepare_Ambush(AITarget, 'left', 0, 5)") == "" && gnil(vm, "a3") &&
          last_message_has(vm, "Parameter 3 is not a positive number"));
    CHECK(run(vm, "a4 = TF.Prepare_Ambush(5, 'left', 10, 5)") == "" && gnil(vm, "a4") && th.ambush_calls == 0);
    CHECK(run(vm, "a5 = TF.Prepare_Ambush(AITarget, 'RiGhT', 250, 0.5, true)") == "" && gnil(vm, "a5") && th.ambush_calls == 1);
    CHECK(th.ambush_target == 5 && th.direction == 2 && th.min_dist == 250.0 && th.threat == 0.5 && th.zone_path);
    th.ambush = AiTacticalScriptHost::kAmbushTargetRegion;
    CHECK(run(vm, "a6 = TF.Prepare_Ambush(AITarget, 'back', 250, 0.5)") == "" && gnil(vm, "a6") && th.direction == 3 && !th.zone_path &&
          last_message_has(vm, "Target Gone_Target is invalid: AI targets must represent game objects"));
    th.ambush = AiTacticalScriptHost::kAmbushTargetDead;
    CHECK(run(vm, "a7 = TF.Prepare_Ambush(AITarget, 'front', 250, 0.5)") == "" && gnil(vm, "a7") &&
          last_message_has(vm, "Target object is already dead") && th.direction == 0);
    attach_ai_tactical_host(vm, nullptr);
    const int32_t calls = th.calls;
    run(vm, "z = EvaluatePerception('Syn_Destroy_Desire', P, AITarget)");  // the players family's own path
    CHECK(th.calls == calls);
}
#endif

// ---- the L1 systems hooks: Hyperspace_Away's flag, per-object field membership, BombingBlockStatus, strike switches ----
static void test_l1_system_hooks(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    TacticalScriptHooks th;
    e.bridge.tactical = &th;
    e.bridge.remaining = &e.hooks;
    const ObjectHandle ship = make(e.store, db.t("Ship"), 0, {0, 0, 0});
    const ObjectHandle prop = make(e.store, db.t("Fighter"), 0, {0, 0, 0});
    e.global("Ship", ship);
    e.global("Prop", prop);
    // Hyperspace_Away([bool = true]) -> (unit, 0, bool), returns nothing; a locomotor is required.
    std::vector<std::string> jumps;
    th.hyperspace_out_link = [&](ObjectHandle u, int32_t delay, bool keep) {
        jumps.push_back(std::to_string(e.store.id(u)) + ":" + std::to_string(delay) + ":" + (keep ? "1" : "0"));
        return true;
    };
    CHECK(run(vm, "r = Ship.Hyperspace_Away(); Ship.Hyperspace_Away(false)") == "" && gnil(vm, "r"));
    const std::string sid = std::to_string(e.store.id(ship));
    CHECK(jumps == (std::vector<std::string>{sid + ":0:1", sid + ":0:0"}));
    CHECK(run(vm, "Prop.Hyperspace_Away()").find("no locomotor") != std::string::npos);
    // Is_In_*: the host's per-object answer; -1 -> the point test.
    e.hooks.object_in_field = [ship](ObjectHandle h, uint32_t kind) { return h == ship ? (kind == kFieldNebula ? 1 : 0) : -1; };
    CHECK(run(vm, "a = Ship.Is_In_Nebula(); b = Ship.Is_In_Ion_Storm(); c = Prop.Is_In_Nebula()") == "");
    CHECK(gtrue(vm, "a") && !gtrue(vm, "b") && !gtrue(vm, "c"));
    // Land strikes.
    Env land(db, objects::ModeKind::Land);
    LuaVm& lv = *land.vm;
    FakeAi ai;
    attach_ai_host(lv, &ai, 1);
    land.bridge.remaining = &land.hooks;
    const ObjectHandle t1 = make(land.store, db.t("Trooper"), 0, {0, 0, 0});
    ai.units = {t1};
    ai.target = make(land.store, db.t("Bunker"), 1, {300, 0, 0});
    set_task_force_global(lv, "TF", 3, AiTaskForceKind::Land);
    set_ai_target_global(lv, "Tgt", 5);
    bool available = true, started = false, finished = false;
    land.hooks.bombing_run = [&](int32_t p, Vec3) { return started = (p == 0 && available); };
    land.hooks.bombing_run_available = [&](int32_t) { return available; };
    land.hooks.bombing_run_finished = [&](int32_t p) { return p == 0 && finished; };
    land.hooks.bombing_run_result = [](int32_t) { return 0.75f; };
    CHECK(run(lv, "br = TF.Bombing_Run(Tgt); f0 = br.IsFinished()") == "" && started && !gtrue(lv, "f0"));
    finished = true;
    CHECK(run(lv, "f1 = br.IsFinished(); res = br.Result()") == "" && gtrue(lv, "f1") && gnum(lv, "res") == 0.75);
    land.hooks.bombing_run = [](int32_t, Vec3) { return false; };
    CHECK(run(lv, "TF.Bombing_Run(Tgt)") == "" && last_message_has(lv, "within an invalid area"));
    available = false;
    CHECK(run(lv, "TF.Bombing_Run(Tgt)") == "" && last_message_has(lv, "No bombing run available"));
    attach_ai_host(lv, nullptr, -1);
#if ALAMO_LUA_PLAYERS
    // PlayerWrapper switches reach the mode's strike managers (and stay in the players world).
    LuaPlayerWorld world;
    std::vector<LuaPlayerSetup> setup(2);
    setup[0].faction = "Empire";
    setup[1].faction = "Rebel";
    world.set_players(setup);
    attach_player_world(lv, &world);
    land.player_global("P", 0);
    std::vector<std::string> sw;
    land.hooks.set_bombing_run_disabled = [&](int32_t p, bool on) { sw.push_back("run " + std::to_string(p) + (on ? " on" : " off")); };
    land.hooks.set_bombardment_disabled = [&](int32_t p, bool on) { sw.push_back("bomb " + std::to_string(p) + (on ? " on" : " off")); };
    land.hooks.remove_bombardment = [&](int32_t p, bool on) { sw.push_back("remove " + std::to_string(p) + (on ? " on" : " off")); };
    CHECK(run(lv, "P.Disable_Bombing_Run(true); P.Disable_Orbital_Bombardment(false); P.Remove_Orbital_Bombardment(true)") == "");
    CHECK(sw == (std::vector<std::string>{"run 0 on", "bomb 0 off", "remove 0 on"}) && world.states()[0].bombing_run_disabled &&
          world.states()[0].orbital_bombardment_removed);
    CHECK(run(lv, "P.Disable_Bombing_Run(1)").find("Expected boolean.") != std::string::npos && sw.size() == 3);
    attach_player_world(lv, nullptr);
#endif
}

// ---- LuaScriptWrapper across two VMs (§2.11) ----
struct TwoScripts final : ScriptWrapperHost {
    LuaVm* self = nullptr;   // script 3
    LuaVm* other = nullptr;  // script 7
    LuaVm* script_vm(int32_t id) override { return id == 7 ? other : (id == 3 ? self : nullptr); }
    int32_t script_id(const LuaVm& vm) override { return &vm == other ? 7 : (&vm == self ? 3 : -1); }
};
static void test_script_wrapper(const Db& db) {
    Env a(db, objects::ModeKind::Space), b(db, objects::ModeKind::Space);
    const ObjectHandle ship = make(a.store, db.t("Ship"), 0, {0, 0, 0});
    a.global("Ship", ship);
    TwoScripts host;
    host.other = b.vm.get();
    attach_script_wrapper_host(*a.vm, &host);
    set_script_wrapper_global(*a.vm, "Other", 7);
    set_script_wrapper_global(*a.vm, "Gone", 8);
    CHECK(run(*b.vm, "function Add(x, y) return x + y, 'sum' end; function Boom() error('bad') end; seed = {k = 2, 3}") == "");
    CHECK(run(*a.vm, "v = Other.Is_Valid(); g = Gone.Is_Valid(); Other.Set_Variable('x', 5); Other.Set_Variable('t', {1, 2, n = 'a'}); "
                     "x = Other.Get_Variable('x'); s = Other.Get_Variable('seed'); sk = s.k; s1 = s[1]; r, w = Other.Call_Function('Add', 2, 3); "
                     "gv = Gone.Get_Variable('x'); Other.Set_Variable('obj', Ship)") == "");
    CHECK(gtrue(*a.vm, "v") && !gtrue(*a.vm, "g") && gnum(*a.vm, "x") == 5.0 && gnum(*a.vm, "sk") == 2.0 && gnum(*a.vm, "s1") == 3.0);
    CHECK(gnum(*a.vm, "r") == 5.0 && gstr(*a.vm, "w") == "sum" && gnil(*a.vm, "gv"));
    CHECK(run(*b.vm, "tn = t.n; t2 = t[2]; objnil = (obj == nil)") == "" && gstr(*b.vm, "tn") == "a" && gnum(*b.vm, "t2") == 2.0);
    CHECK(gtrue(*b.vm, "objnil"));  // a different store: the object does not exist there
    CHECK(run(*a.vm, "Other.Call_Function('Boom')") == "" && last_message_has(*a.vm, "bad"));
    CHECK(run(*a.vm, "Other.Call_Function('Nope')") == "" && last_message_has(*a.vm, "Nope is not a function"));
    CHECK(run(*a.vm, "Other.Call_Function()") == "" && last_message_has(*a.vm, "Expected at least 1, got 0"));
    CHECK(run(*a.vm, "Other.Get_Variable(1)") == "" && last_message_has(*a.vm, "Expected string."));
    CHECK(run(*a.vm, "Other.Set_Variable('x')") == "" && last_message_has(*a.vm, "Expected 2, got 1"));
    // OI lua (STRATEGIC_GAPS §5.2): a script object copied into another VM becomes a LuaScriptWrapper there (cached per
    // target VM); unknown to the host -> nil.
    CHECK(run(*a.vm, "Other.Set_Variable('caller', Script)") == "" && run(*b.vm, "cnil = (caller == nil)") == "" && gtrue(*b.vm, "cnil"));
    host.self = a.vm.get();
    attach_script_wrapper_host(*b.vm, &host);
    CHECK(run(*b.vm, "function Me() return Script, {s = Script} end") == "");
    CHECK(run(*a.vm, "mark = 11; Other.Set_Variable('caller', Script); me, mt = Other.Call_Function('Me'); same = (me == Other); "
                     "same2 = (mt.s == Other); mv = me.Is_Valid()") == "");
    CHECK(gtrue(*a.vm, "same") && gtrue(*a.vm, "same2") && gtrue(*a.vm, "mv"));
    CHECK(run(*b.vm, "m = caller.Get_Variable('mark'); cv = caller.Is_Valid()") == "" && gnum(*b.vm, "m") == 11.0 && gtrue(*b.vm, "cv"));
    attach_script_wrapper_host(*b.vm, nullptr);
}

int main() {
    log::set_min_level(log::Level::Error);
    std::printf("alamo_lua_script_remaining_tests\n");
    test_table();
    const Db db;
    CHECK(db.ok);
    if (db.ok) {
        test_singletons_and_global_value(db);
        test_fields(db);
        test_threat_center(db);
        test_object_methods(db);
        test_player_methods(db);
        test_task_forces(db);
#if ALAMO_LUA_PLAYERS
        test_tactical_perception(db);
#endif
        test_l1_system_hooks(db);
        test_script_wrapper(db);
    }
    std::printf(g_fail ? "FAILED (%d)\n" : "OK\n", g_fail);
    return g_fail ? 1 : 0;
}

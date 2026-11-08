// engine/lua L1/S4 wave 2b: the script-gap family (bind_script_gaps.cpp, alamo/lua/script_gaps.hpp; spec
// docs/re/SCRIPT_GAPS.md): Fire_Special_Weapon (three paths, upgrade gate, destroy-after sweep), the task-force
// variant, Fire_Tactical_Superweapon / Is_Tactical_Superweapon_Ready, Spawn_Special_Weapon,
// Is_Affected_By_Laser_Defense, Create_Cinematic_Transport + the landing machine, Start_Cinematic_Space_Retreat,
// Apply_Markup handles, Get_Parent_Mode_Object (a wrapper on the parent store), and the family table itself.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/bindings.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/test/support.hpp"
#include "vm_internal.hpp"

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
static bool last_message_has(LuaVm& vm, const char* text) {
    return !vm.messages().empty() && vm.messages().back().find(text) != std::string::npos;
}
using ::alamo::test::write_file;

struct Db {
    fs::path root;
    std::unique_ptr<DirectoryProvider> dp;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    bool ok = false;
    Db() {
        root = fs::temp_directory_path() / "alamo_lua_script_gaps_test";
        std::error_code ec;
        remove_all_long(root, ec);
        write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>T.xml</File></L>");
        write_file(root / "Data/XML/T.xml",
                   "<R>\n"
                   "<SpaceUnit Name=\"Ion_Cannon\"><Behavior>SPECIAL_WEAPON</Behavior>"
                   "<Is_Special_Weapon_In_Space>Yes</Is_Special_Weapon_In_Space><Special_Weapon_Index>0</Special_Weapon_Index>"
                   "<Special_Weapon_Valid_Targets>Capital</Special_Weapon_Valid_Targets></SpaceUnit>\n"
                   "<SpaceUnit Name=\"No_Index_Weapon\"><Is_Special_Weapon_In_Space>Yes</Is_Special_Weapon_In_Space>"
                   "<Special_Weapon_Index>-1</Special_Weapon_Index></SpaceUnit>\n"
                   "<GroundStructure Name=\"Lobber\"><Behavior>LOBBING_SUPERWEAPON</Behavior></GroundStructure>\n"
                   "<GroundStructure Name=\"Land_Lobber\"><LandBehavior>LOBBING_SUPERWEAPON</LandBehavior></GroundStructure>\n"
                   "<StarBase Name=\"Station\"><Behavior>DUMMY_STAR_BASE</Behavior></StarBase>\n"
                   "<SpaceUnit Name=\"Ship\"><Behavior>STARSHIP_LOCOMOTOR</Behavior><Max_Speed>5</Max_Speed>"
                   "<CategoryMask>Capital</CategoryMask></SpaceUnit>\n"
                   "<SpaceUnit Name=\"Fast_Ship\"><Behavior>STARSHIP_LOCOMOTOR</Behavior><Max_Speed>10</Max_Speed>"
                   "<CategoryMask>Fighter</CategoryMask></SpaceUnit>\n"
                   "<SpaceUnit Name=\"Prop\"><Tactical_Health>10</Tactical_Health></SpaceUnit>\n"
                   "<SpaceUnit Name=\"Bomb_Marker\"><Behavior>DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED</Behavior></SpaceUnit>\n"
                   "<SpaceUnit Name=\"Weapon_Source\"><Behavior>DUMMY_SPECIAL_WEAPON_SOURCE</Behavior></SpaceUnit>\n"
                   "<SpaceUnit Name=\"Death_Star\"><Behavior>TACTICAL_SUPER_WEAPON</Behavior></SpaceUnit>\n"
                   "<Marker Name=\"Attacker Entry Position\"><Behavior>MARKER</Behavior></Marker>\n"
                   "<GroundVehicle Name=\"Shuttle\"><Behavior>TRANSPORT</Behavior><Hover_Offset>5</Hover_Offset></GroundVehicle>\n"
                   "<GroundVehicle Name=\"Space_Shuttle\"><Behavior>TRANSPORT</Behavior><Has_Space_Evaluator>Yes</Has_Space_Evaluator>"
                   "</GroundVehicle>\n"
                   "<Projectile Name=\"Missile_P\"><Projectile_Category>MISSILE</Projectile_Category></Projectile>\n"
                   "<Projectile Name=\"Rocket_P\"><Projectile_Category>ROCKET</Projectile_Category></Projectile>\n"
                   "<Projectile Name=\"Mptl_P\"><Projectile_Category>MPTL_ROCKET</Projectile_Category></Projectile>\n"
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

static objects::ObjectHandle make(objects::ObjectStore& st, objects::TypeIndex t, int32_t owner, Vec3 p,
                                  objects::ObjectId parent = objects::kNoObjectId) {
    objects::SpawnParams sp;
    sp.type = t;
    sp.owner = owner;
    sp.position = p;
    sp.facing = {1.0f, 0.0f, 0.0f};
    sp.parent_mode_id = parent;
    return st.create(sp);
}

struct Env {
    objects::IdCounter ids;
    objects::ObjectStore store;
    ObjectBridge bridge;
    TacticalScriptHooks hooks;
    std::unique_ptr<LuaVm> vm;
    Env(const Db& db, objects::ModeKind mode) : store(mode, objects::StoreKind::Primary, db.types, &ids) {
        bridge.tactical = &hooks;
        LuaHost host;
        host.store = &store;
        host.types = &db.types;
        vm = std::make_unique<LuaVm>(host);
        attach_object_bridge(*vm, &bridge);
    }
};

// ---- the family table and the behaviour ids it hard-codes ----
static void test_table() {
    CHECK(objects::behavior_type_id("SPECIAL_WEAPON") == 0x2F && objects::behavior_type_id("LOBBING_SUPERWEAPON") == 0x37 &&
          objects::behavior_type_id("DUMMY_STAR_BASE") == 0x5C && objects::behavior_type_id("DUMMY_SPECIAL_WEAPON_SOURCE") == 0x63 &&
          objects::behavior_type_id("DUMMY_DESTROY_AFTER_SPECIAL_WEAPON_FIRED") == 0x67);
    for (const detail::ImplEntry& e : detail::impl_script_gaps()) {
        int same = 0;
        for (const detail::ImplEntry& o : detail::implementations())
            if (o.kind == e.kind && std::strcmp(o.name, e.name) == 0 && std::strcmp(o.cls, e.cls) == 0) ++same;
        if (same != 1) {
            std::printf("  DUPLICATE binding %s.%s (%d implementations)\n", e.cls, e.name, same);
            ++g_fail;
        }
    }
    // Every RE-table row of the family is implemented.
    for (const BindingInfo& b : binding_table())
        for (const char* n : {"Fire_Special_Weapon", "Fire_Tactical_Superweapon", "Is_Tactical_Superweapon_Ready", "Get_Parent_Mode_Object",
                              "Is_Affected_By_Laser_Defense", "Spawn_Special_Weapon", "Create_Cinematic_Transport",
                              "Start_Cinematic_Space_Retreat", "Apply_Markup"})
            if (std::strcmp(b.name, n) == 0 && (std::strcmp(b.cls, "GameObjectWrapper") == 0 || std::strcmp(b.cls, "GameObjectTypeWrapper") == 0 ||
                                               b.kind == BindingKind::Global))
                CHECK(binding_implemented(b));
}

static void test_laser_defense(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    CHECK(run(*e.vm, "m = Find_Object_Type('Missile_P').Is_Affected_By_Laser_Defense() "
                     "r = Find_Object_Type('Rocket_P').Is_Affected_By_Laser_Defense() "
                     "p = Find_Object_Type('Mptl_P').Is_Affected_By_Laser_Defense() "
                     "l = Find_Object_Type('Laser_P').Is_Affected_By_Laser_Defense() "
                     "s = Find_Object_Type('Ship').Is_Affected_By_Laser_Defense()") == "");
    CHECK(gtrue(*e.vm, "m") && gtrue(*e.vm, "r") && gtrue(*e.vm, "p") && !gtrue(*e.vm, "l") && !gtrue(*e.vm, "s"));
}

static void test_fire_special_weapon(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    const objects::ObjectHandle cannon = make(e.store, db.t("Ion_Cannon"), 1, {0, 0, 0});
    const objects::ObjectHandle lobber = make(e.store, db.t("Lobber"), 1, {0, 0, 0});
    const objects::ObjectHandle station = make(e.store, db.t("Station"), 1, {0, 0, 0});
    const objects::ObjectHandle enemy = make(e.store, db.t("Ship"), 2, {100, 200, 3});
    const objects::ObjectHandle marker1 = make(e.store, db.t("Bomb_Marker"), 1, {0, 0, 0});
    const objects::ObjectHandle marker2 = make(e.store, db.t("Bomb_Marker"), 2, {0, 0, 0});
    make(e.store, db.t("Prop"), 1, {0, 0, 0});
    float ready = 0.5f;
    bool allowed = true, station_ok = false;
    int begun = 0, fired = 0;
    Vec3 lobbed{};
    e.hooks.weapon_readiness = [&](objects::ObjectHandle) { return ready; };
    e.hooks.begin_targeting = [&](objects::ObjectHandle) { return ++begun > 0; };
    e.hooks.fire_at_object = [&](objects::ObjectHandle w, objects::ObjectHandle t) { return w == cannon && t == enemy && ++fired > 0; };
    e.hooks.fire_lobbing_at = [&](objects::ObjectHandle, Vec3 p) {
        lobbed = p;
        return true;
    };
    e.hooks.special_weapons_allowed = [&](int32_t) { return allowed; };
    e.hooks.station_readiness = [&](objects::ObjectHandle) { return 1.0f; };
    e.hooks.station_fire = [&](objects::ObjectHandle, objects::ObjectHandle, int32_t) { return station_ok; };
    CHECK(run(vm, "c = Find_First_Object('Ion_Cannon') l = Find_First_Object('Lobber') st = Find_First_Object('Station') "
                  "x = Find_First_Object('Ship') pr = Find_First_Object('Prop') p1 = pr.Get_Owner()") == "" &&
          !gnil(vm, "p1"));
    // Not a special weapon -> script error, nil.
    CHECK(run(vm, "r = pr.Fire_Special_Weapon(x, p1)") == "" && gnil(vm, "r") && last_message_has(vm, "is not a special weapon"));
    // Special weapon not ready -> warning, nil; no fire, no sweep.
    CHECK(run(vm, "r = c.Fire_Special_Weapon(x, p1)") == "" && gnil(vm, "r") && last_message_has(vm, "not ready") && fired == 0);
    // Ready but the upgrade gate refuses (space mode).
    ready = 1.0f;
    allowed = false;
    CHECK(run(vm, "r = c.Fire_Special_Weapon(x, p1)") == "" && gnil(vm, "r") && last_message_has(vm, "Purchase a special weapon upgrade"));
    allowed = true;
    CHECK(run(vm, "r = c.Fire_Special_Weapon(x, p1) same = (r == c)") == "" && gtrue(vm, "same") && fired == 1);
    // Destroy-after sweep for the player argument only.
    CHECK(!e.store.alive(marker1) && e.store.alive(marker2));
    // Lobbing superweapon: fires at the target's position.
    CHECK(run(vm, "r = l.Fire_Special_Weapon(x, p1) lsame = (r == l)") == "" && gtrue(vm, "lsame") && lobbed.x == 100.0f &&
          lobbed.y == 200.0f && lobbed.z == 3.0f);
    // Space station: can-fire refused -> warning.
    CHECK(run(vm, "r = st.Fire_Special_Weapon(x, p1)") == "" && gnil(vm, "r") && last_message_has(vm, "unable to fire at target"));
    station_ok = true;
    CHECK(run(vm, "r = st.Fire_Special_Weapon(x, p1) ssame = (r == st)") == "" && gtrue(vm, "ssame"));
    // Bad arguments: script errors, no unwind.
    CHECK(run(vm, "r = c.Fire_Special_Weapon(x)") == "" && gnil(vm, "r") && last_message_has(vm, "Expected 2"));
    CHECK(run(vm, "r = c.Fire_Special_Weapon(3, p1)") == "" && gnil(vm, "r") && last_message_has(vm, "invalid target"));
    (void)lobber;
    (void)station;
}

// detail::task_force_fire_special_weapon (A1's Land/SpaceTaskForceClass.Fire_Special_Weapon calls it): driven here through a
// test C function; the LuaVm::Impl comes from the shared metatable's __index upvalue.
static std::vector<objects::ObjectHandle>* g_units = nullptr;
static int tf_fire(lua_State* L) {
    lua_pushstring(L, "alamo.meta");
    lua_rawget(L, LUA_REGISTRYINDEX);
    lua_pushstring(L, "__index");
    lua_rawget(L, -2);
    lua_getupvalue(L, -1, 1);
    auto* impl = static_cast<LuaVm::Impl*>(lua_touserdata(L, -1));
    lua_pop(L, 3);
    const bool space = lua_toboolean(L, 1) != 0;
    detail::Call c{L, impl, nullptr, 2, lua_gettop(L) - 1};
    return detail::task_force_fire_special_weapon(c, space, *g_units, -1);
}

static void test_task_force(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    const objects::ObjectHandle c1 = make(e.store, db.t("Ion_Cannon"), 1, {0, 0, 0});
    const objects::ObjectHandle c2 = make(e.store, db.t("Ion_Cannon"), 1, {0, 0, 0});
    make(e.store, db.t("Ship"), 2, {100, 0, 0});
    make(e.store, db.t("Fast_Ship"), 2, {100, 0, 0});
    std::vector<objects::ObjectHandle> units{c1, c2};
    g_units = &units;
    e.hooks.weapon_readiness = [&](objects::ObjectHandle h) { return h == c2 ? 1.0f : 0.0f; };
    e.hooks.begin_targeting = [](objects::ObjectHandle) { return true; };
    objects::ObjectHandle shooter;
    e.hooks.fire_at_object = [&](objects::ObjectHandle w, objects::ObjectHandle) {
        shooter = w;
        return true;
    };
    lua_register(vm.state(), "TF_Fire", tf_fire);
    // The first READY member of the type fires; a target outside Special_Weapon_Valid_Targets -> false.
    CHECK(run(vm, "u = TF_Fire(true, 'Ion_Cannon', Find_First_Object('Ship')) "
                  "f = TF_Fire(true, 'Ion_Cannon', Find_First_Object('Fast_Ship'))") == "");
    CHECK(shooter == c2 && !gnil(vm, "u") && !gtrue(vm, "f") && !gnil(vm, "f"));
    // Land variant: the type must be a lobbing superweapon.
    CHECK(run(vm, "g = TF_Fire(false, 'Ion_Cannon', Find_First_Object('Ship'))") == "" && gnil(vm, "g") &&
          last_message_has(vm, "not a land special weapon"));
    g_units = nullptr;
}

// LandTaskForce.Fire_Special_Weapon (AI_TACTICAL §7.3.4, s78f t04): exactly 2 arguments, a string type name, a game object
// (or AI target) target - a plain position is an error; the first READY lobbing unit of the type is returned even when the
// fire order is refused; none ready -> false.
static void test_land_task_force(const Db& db) {
    Env e(db, objects::ModeKind::Land);
    LuaVm& vm = *e.vm;
    const objects::ObjectHandle l1 = make(e.store, db.t("Land_Lobber"), 1, {0, 0, 0});
    const objects::ObjectHandle l2 = make(e.store, db.t("Land_Lobber"), 1, {0, 0, 0});
    const objects::ObjectHandle l3 = make(e.store, db.t("Land_Lobber"), 1, {0, 0, 0});
    const objects::ObjectHandle marker = make(e.store, db.t("Bomb_Marker"), 1, {0, 0, 0});
    make(e.store, db.t("Ship"), 2, {40, 50, 6});
    std::vector<objects::ObjectHandle> units{l1, l2, l3};
    g_units = &units;
    bool ready2 = true, ready3 = true;
    e.hooks.weapon_readiness = [&](objects::ObjectHandle h) {
        return h == l1 ? 0.5f : h == l2 ? (ready2 ? 1.0f : 0.0f) : (ready3 ? 1.0f : 0.0f);
    };
    std::vector<objects::ObjectHandle> ordered;
    Vec3 lobbed{};
    e.hooks.fire_lobbing_at = [&](objects::ObjectHandle h, Vec3 p) {
        ordered.push_back(h);
        lobbed = p;
        return false;  // refused: the unit is still returned and later units are not tried
    };
    lua_register(vm.state(), "TF_Fire", tf_fire);
    CHECK(run(vm, "x = Find_First_Object('Ship') r = TF_Fire(false, 'Land_Lobber', x)") == "" && !gnil(vm, "r"));
    CHECK(ordered.size() == 1 && ordered[0] == l2 && lobbed.x == 40.0f && lobbed.y == 50.0f && lobbed.z == 6.0f);
    CHECK(!e.store.alive(marker));  // the owner's destroy-after sweep ran
    // Wrong argument count / type name / target kind: script errors, nothing ordered.
    CHECK(run(vm, "a = TF_Fire(false, 'Land_Lobber')") == "" && gnil(vm, "a") && last_message_has(vm, "expected 2 got 1"));
    CHECK(run(vm, "b = TF_Fire(false, 7, x)") == "" && gnil(vm, "b") && last_message_has(vm, "Expected string"));
    CHECK(run(vm, "p = TF_Fire(false, 'Land_Lobber', x.Get_Position())") == "" && gnil(vm, "p") &&
          last_message_has(vm, "Expected game object or target location"));
    // A type with LOBBING_SUPERWEAPON only in its generic <Behavior> set is not a land special weapon (AI_TACTICAL
    // §7.3.4: 0x370320(type, 0x37, 1) reads the Land set).
    CHECK(run(vm, "q = TF_Fire(false, 'Lobber', x)") == "" && gnil(vm, "q") && last_message_has(vm, "Lobber is not a land special weapon"));
    CHECK(ordered.size() == 1);
    // No ready unit of the type -> false.
    ready2 = ready3 = false;
    CHECK(run(vm, "n = TF_Fire(false, 'Land_Lobber', x)") == "" && !gnil(vm, "n") && !gtrue(vm, "n") && ordered.size() == 1);
    g_units = nullptr;
}

static void test_tsw_and_spawn(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    make(e.store, db.t("Death_Star"), 1, {0, 0, 0});
    make(e.store, db.t("Ship"), 1, {0, 0, 0});
    int32_t state = 3;
    e.hooks.tsw_state = [&](objects::ObjectHandle) { return state; };
    e.hooks.tsw_fire = [&](objects::ObjectHandle) { return state == 4; };
    CHECK(run(vm, "d = Find_First_Object('Death_Star') r1 = d.Is_Tactical_Superweapon_Ready() f1 = d.Fire_Tactical_Superweapon()") == "");
    CHECK(!gtrue(vm, "r1") && !gtrue(vm, "f1"));
    state = 4;
    CHECK(run(vm, "r2 = d.Is_Tactical_Superweapon_Ready() f2 = d.Fire_Tactical_Superweapon()") == "" && gtrue(vm, "r2") && gtrue(vm, "f2"));
    CHECK(run(vm, "n = Find_First_Object('Ship').Fire_Tactical_Superweapon()") == "" && gnil(vm, "n") &&
          last_message_has(vm, "is not a tactical superweapon"));
    // Spawn_Special_Weapon: no source yet -> error; then at the first source's position, for the player.
    CHECK(run(vm, "p1 = Find_First_Object('Ship').Get_Owner()") == "" && !gnil(vm, "p1"));
    CHECK(run(vm, "w = Spawn_Special_Weapon('Ion_Cannon', p1)") == "" && gnil(vm, "w") && last_message_has(vm, "SPECIAL_WEAPON_SOURCE"));
    make(e.store, db.t("Weapon_Source"), -1, {7, 8, 9});
    make(e.store, db.t("Weapon_Source"), -1, {1, 1, 1});
    CHECK(run(vm, "w = Spawn_Special_Weapon('Ion_Cannon', p1) x, y, z = w.Get_Position().Get_XYZ() o = w.Get_Owner().Get_ID()") == "");
    CHECK(gnum(vm, "x") == 7.0 && gnum(vm, "y") == 8.0 && gnum(vm, "z") == 9.0 && gnum(vm, "o") == 1.0);
    CHECK(run(vm, "w2 = Spawn_Special_Weapon('Ship', p1)") == "" && gnil(vm, "w2") && last_message_has(vm, "not a space special weapon"));
    CHECK(run(vm, "w3 = Spawn_Special_Weapon('No_Index_Weapon', p1)") == "" && gnil(vm, "w3") && last_message_has(vm, "no special weapon index"));
    Env land(db, objects::ModeKind::Land);
    CHECK(run(*land.vm, "w = Spawn_Special_Weapon('Ion_Cannon', 1)") == "" && last_message_has(*land.vm, "space tactical mode"));
}

static void test_landing_machine() {
    CinematicTransports t;
    t.add(10, 1, 0.25f, 20.0f, true);   // the shipped call: lands from 25 % and stays
    t.add(11, 1, 0.0f, 1.0f, false);    // lands, idles 1 s, takes off, destroyed
    t.add(12, 3, 0.5f, 2.0f, false);    // starts taking off from the middle of Takeoff
    t.add(13, 2, 0.0f, 0.5f, false);    // already landed: 0.5 s, then takes off
    std::vector<CinematicStep> steps;
    std::map<objects::ObjectId, std::vector<std::pair<uint32_t, CinematicStep::Kind>>> seen;
    for (uint32_t f = 0; f <= 300; ++f) {
        steps.clear();
        t.service(f, {}, steps);
        for (const CinematicStep& s : steps) seen[s.id].emplace_back(f, s.kind);
    }
    using K = CinematicStep::Kind;
    // 10: offset int(59 x 0.25) = 14, landed when (14 + f) / 60 > 0.98 -> f = 45; persist: nothing after.
    CHECK(seen[10].size() == 2 && seen[10][0] == std::make_pair(0u, K::PlayLand) && seen[10][1] == std::make_pair(45u, K::Landed));
    CHECK(t.find(10) && t.find(10)->state == 2);
    // 11: landed at 59 ((f) / 60 > 0.98), take-off at 59 + 30, cargo past 50 % at speed 1.15, destroyed past 98 %.
    const auto& s11 = seen[11];
    CHECK(s11.size() == 5 && s11[1] == std::make_pair(59u, K::Landed) && s11[2] == std::make_pair(89u, K::PlayTakeoff) &&
          s11[3] == std::make_pair(116u, K::CargoReleased) && s11[4] == std::make_pair(141u, K::Destroy));
    CHECK(!t.find(11) && !t.find(12));
    CHECK(seen[12].front() == std::make_pair(0u, K::PlayTakeoff) && seen[12].back().second == K::Destroy);
    CHECK(seen[13][1] == std::make_pair(0u, K::Landed) && seen[13][2] == std::make_pair(15u, K::PlayTakeoff));
    CHECK(t.entries().size() == 1);
}

static void test_cinematic_transport(const Db& db) {
    Env e(db, objects::ModeKind::Land);
    LuaVm& vm = *e.vm;
    CinematicTransports transports;
    e.hooks.transports = &transports;
    e.hooks.ground_height = [](float, float) { return 50.0f; };
    CHECK(run(vm, "t = Create_Cinematic_Transport('Shuttle', 1, Create_Position(10, 20, 30), 0, 1, 0.25, 20, 1, 'Drop_Ship') "
                  "x, y, z = t.Get_Position().Get_XYZ() o = t.Get_Owner().Get_ID() h = t.Get_Hint()") == "");
    CHECK(gnum(vm, "x") == 10.0 && gnum(vm, "y") == 20.0 && gnum(vm, "z") == 55.0);  // max(30, ground 50) + Hover_Offset 5
    CHECK(transports.entries().size() == 1 && transports.entries()[0].persist && transports.entries()[0].anim_delta == 0.25f &&
          transports.entries()[0].idle_seconds == 20.0f && transports.entries()[0].landing_mode == 1);
    const objects::ObjectHandle th = e.store.find(transports.entries()[0].id);
    CHECK(e.store.has_behavior(th, objects::beh::TransportLanding) && e.bridge.find_state(e.store.id(th)) &&
          e.bridge.find_state(e.store.id(th))->hint == "Drop_Ship");
    // Wrong-typed arguments 4..8 log and keep the defaults; still created.
    CHECK(run(vm, "t2 = Create_Cinematic_Transport('Shuttle', 1, Create_Position(0, 0, 0), 'x', 1, 0, 2, 0)") == "" && !gnil(vm, "t2") &&
          last_message_has(vm, "parameter 4"));
    // Too few arguments, a non-transport / space-evaluator type, a land-only binding in space.
    CHECK(run(vm, "t3 = Create_Cinematic_Transport('Shuttle', 1, Create_Position(0, 0, 0))") == "" && gnil(vm, "t3") &&
          last_message_has(vm, "Expected 8 or 9"));
    CHECK(run(vm, "t4 = Create_Cinematic_Transport('Space_Shuttle', 1, Create_Position(0, 0, 0), 0, 1, 0, 2, 0)") == "" && gnil(vm, "t4"));
    CHECK(run(vm, "t5 = Create_Cinematic_Transport('Prop', 1, Create_Position(0, 0, 0), 0, 1, 0, 2, 0)") == "" && gnil(vm, "t5"));
    Env space(db, objects::ModeKind::Space);
    CHECK(run(*space.vm, "t = Create_Cinematic_Transport('Shuttle', 1, Create_Position(0, 0, 0), 0, 1, 0, 2, 0)") == "" &&
          last_message_has(*space.vm, "land tactical mode"));
}

static void test_space_retreat(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    make(e.store, db.t("Attacker Entry Position"), -1, {1000, 0, 0});
    const objects::ObjectHandle slow = make(e.store, db.t("Ship"), 1, {0, 0, 0});
    const objects::ObjectHandle fast = make(e.store, db.t("Fast_Ship"), 1, {0, 0, 0});
    const objects::ObjectHandle other = make(e.store, db.t("Ship"), 2, {0, 0, 0});
    make(e.store, db.t("Prop"), 1, {0, 0, 0});  // no locomotor: not retreating
    std::vector<std::pair<objects::ObjectHandle, int32_t>> jumps;
    e.hooks.hyperspace_out_link = [&](objects::ObjectHandle h, int32_t delay, bool) {
        jumps.emplace_back(h, delay);
        return true;
    };
    e.hooks.retreat_increment_frames = [](int32_t) { return 7; };
    CHECK(run(vm, "Start_Cinematic_Space_Retreat(1, 2)") == "");
    // Faster first in the line-up (slot 0 = column -2), facing away from the marker's facing (+X) = -X; then the
    // jump order by distance to the point 10000 out along -X: the slow ship (column -1) is nearer.
    CHECK(e.store.position(fast).x == 1000.0f && e.store.position(fast).y == 5000.0f && e.store.position(slow).y == 2500.0f);
    CHECK(e.store.facing(fast).x == -1.0f && e.store.position(other).x == 0.0f);
    CHECK(jumps.size() == 2 && jumps[0] == std::make_pair(slow, 60) && jumps[1] == std::make_pair(fast, 67));
    Env land(db, objects::ModeKind::Land);
    CHECK(run(*land.vm, "Start_Cinematic_Space_Retreat(1)") == "" && last_message_has(*land.vm, "space tactical mode"));
}

struct FakeMarkup final : ScriptMarkupHost {
    const objects::ObjectStore* store = nullptr;
    std::map<int32_t, double> value;
    int32_t markup_target_of_object(int32_t, objects::ObjectHandle o) override { return 1000 + store->id(o); }
    int32_t markup_target_of_player(int32_t, int32_t player) override { return 2000 + player; }
    void add_markup(int32_t target, double delta) override { value[target] += delta; }
};

static void test_markup(const Db& db) {
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    const objects::ObjectHandle ship = make(e.store, db.t("Ship"), 2, {0, 0, 0});
    make(e.store, db.t("Prop"), 1, {0, 0, 0});
    FakeMarkup m;
    m.store = &e.store;
    attach_markup_host(vm, &m);
    CHECK(markup_host(vm) == &m);
    set_ai_target_global(vm, "Tgt", 77);
    CHECK(run(vm, "p1 = Find_First_Object('Prop').Get_Owner() p2 = Find_First_Object('Ship').Get_Owner()") == "" && !gnil(vm, "p1"));
    const int32_t sid = 1000 + e.store.id(ship);
    CHECK(run(vm, "s = Find_First_Object('Ship') h = Apply_Markup(p1, s, 5) fin = h.IsFinished()") == "" && gtrue(vm, "fin"));
    CHECK(m.value[sid] == 5.0);
    // Reusing the handle removes the previous amount first.
    CHECK(run(vm, "h2 = Apply_Markup(p1, Tgt, 3, h) same = (h2 == h)") == "" && gtrue(vm, "same"));
    CHECK(m.value[sid] == 0.0 && m.value[77] == 3.0);
    // A table of targets (object, AI target, player) and a fresh handle.
    CHECK(run(vm, "h3 = Apply_Markup(p1, {s, Tgt, p2}, 1.5) diff = (h3 ~= h)") == "" && gtrue(vm, "diff"));
    CHECK(m.value[sid] == 1.5 && m.value[77] == 4.5 && m.value[2002] == 1.5);
    CHECK(run(vm, "Apply_Markup(p1, {s, Tgt, p2}, 0, h3)") == "" && m.value[sid] == 0.0 && m.value[77] == 3.0 && m.value[2002] == 0.0);
    // Errors: count, player, amount, target type.
    CHECK(run(vm, "e1 = Apply_Markup(p1, s)") == "" && gnil(vm, "e1") && last_message_has(vm, "Expected 3 or 4"));
    CHECK(run(vm, "e2 = Apply_Markup(1, s, 1)") == "" && gnil(vm, "e2") && last_message_has(vm, "parameter 1"));
    CHECK(run(vm, "e3 = Apply_Markup(p1, s, 'x')") == "" && gnil(vm, "e3") && last_message_has(vm, "parameter 3"));
    CHECK(run(vm, "e4 = Apply_Markup(p1, 'nope', 1)") == "" && gnil(vm, "e4") && last_message_has(vm, "parameter 2"));
    attach_markup_host(vm, nullptr);
    CHECK(markup_host(vm) == nullptr && run(vm, "h4 = Apply_Markup(p1, s, 9)") == "" && !gnil(vm, "h4"));
}

static void test_parent_mode_object(const Db& db) {
    objects::IdCounter gids;
    objects::ObjectStore galaxy(objects::ModeKind::Galactic, objects::StoreKind::Primary, db.types, &gids);
    gids.set_next(500);
    const objects::ObjectHandle gship = make(galaxy, db.t("Ship"), 1, {11, 22, 33});
    Env e(db, objects::ModeKind::Space);
    LuaVm& vm = *e.vm;
    e.bridge.parent_store = &galaxy;
    const objects::ObjectHandle tship = make(e.store, db.t("Ship"), 1, {1, 2, 3}, galaxy.id(gship));
    make(e.store, db.t("Prop"), 1, {0, 0, 0});
    (void)tship;
    CHECK(run(vm, "t = Find_First_Object('Ship') p = t.Get_Parent_Mode_Object() q = t.Get_Parent_Mode_Object() "
                  "id = t.Get_Parent_Mode_Object_ID() px, py, pz = p.Get_Position().Get_XYZ() v = p.Is_Valid() "
                  "tx = t.Get_Position().Get_XYZ() ident = rawequal(p, q) diff = (p ~= t) "
                  "n = Find_First_Object('Prop').Get_Parent_Mode_Object() pp = p.Get_Parent_Mode_Object()") == "");
    CHECK(gnum(vm, "px") == 11.0 && gnum(vm, "py") == 22.0 && gnum(vm, "pz") == 33.0 && gtrue(vm, "v") && gnum(vm, "tx") == 1.0);
    CHECK(gnum(vm, "id") == static_cast<double>(galaxy.id(gship)) && gtrue(vm, "ident") && gtrue(vm, "diff") && gnil(vm, "n") && gnil(vm, "pp"));
    galaxy.destroy(gship);
    CHECK(run(vm, "v2 = p.Is_Valid()") == "" && !gtrue(vm, "v2") && run(vm, "np = (function(...) return arg.n end)(p.Get_Position())") == "" && !vm.exit_requested());
    // Without a parent store: nil.
    e.bridge.parent_store = nullptr;
    CHECK(run(vm, "p3 = t.Get_Parent_Mode_Object()") == "" && gnil(vm, "p3"));
}

int main() {
    log::set_min_level(log::Level::Error);
    const Db db;
    CHECK(db.ok);
    if (!db.ok) return 1;
    test_table();
    test_laser_defense(db);
    test_fire_special_weapon(db);
    test_task_force(db);
    test_land_task_force(db);
    test_tsw_and_spawn(db);
    test_landing_machine();
    test_cinematic_transport(db);
    test_space_retreat(db);
    test_markup(db);
    test_parent_mode_object(db);
    if (g_fail) {
        std::printf("alamo_lua_script_gaps_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_lua_script_gaps_tests: all passed\n");
    return 0;
}

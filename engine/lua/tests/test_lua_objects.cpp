// engine/lua binding family OBJECTS (src/bind_objects.cpp): every binding through real Lua snippets on
// a synthetic store, dead-handle errors, newest-first / tie ordering, orders on the direct command
// path (engine/commands), determinism of a scripted run, and the shipped scripts' patterns (PGBase
// BlockOnCommand on top of the bindings when ALAMO_FOC_DATA is set).
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/log.hpp"
#include "alamo/lua/ai_bridge.hpp"  // reset_script_threads
#include "alamo/lua/bindings.hpp"
#include "alamo/lua/fog_bridge.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/script_gaps.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/test/support.hpp"
#if ALAMO_LUA_HAVE_COMMANDS
#include "alamo/commands/pipeline.hpp"
#endif
#ifdef ALAMO_LUA_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
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
static std::string gstr(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const char* s = lua_tostring(L, -1);
    std::string out = s ? s : "<nil>";
    lua_pop(L, 1);
    return out;
}
static bool gtrue(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool b = lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1) != 0;
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
static bool contains(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }
// The original's script error helper (Find_All_Objects_Of_Type / Find_First_Object / Find_Nearest): logged, no unwind.
static bool last_message_has(LuaVm& vm, const char* text) {
    return !vm.messages().empty() && vm.messages().back().find(text) != std::string::npos;
}

using ::alamo::test::write_file;

// ------------------------------------------------------------------------------------------
// Synthetic type database.

struct Db {
    fs::path root;
    std::unique_ptr<DirectoryProvider> dp;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    bool ok = false;
    Db() {
        root = fs::temp_directory_path() / "alamo_lua_objects_family_test";
        std::error_code ec;
        remove_all_long(root, ec);
        write_file(root / "Data/XML/GameObjectFiles.xml", "<L><File>S.xml</File></L>");
        write_file(root / "Data/XML/S.xml",
                   "<R>\n"
                   "<GroundInfantry Name=\"Trooper\"><Tactical_Health>100</Tactical_Health>"
                   "<CategoryMask>Infantry</CategoryMask><Property_Flags>Fodder</Property_Flags>"
                   "<Behavior>SELECTABLE, WALK_LOCOMOTOR, UNIT_AI</Behavior>"
                   "<Unit_Abilities_Data SubObjectList=\"Yes\">"
                   "<Unit_Ability><Type>SPRINT</Type><Recharge_Seconds>5</Recharge_Seconds></Unit_Ability>"
                   "<Unit_Ability><Type> STEALTH </Type></Unit_Ability>"
                   "</Unit_Abilities_Data></GroundInfantry>\n"
                   "<GroundVehicle Name=\"Tank\"><Tactical_Health>400</Tactical_Health><Shield_Points>50</Shield_Points>"
                   "<CategoryMask>Vehicle</CategoryMask><Property_Flags>HealsInfantry</Property_Flags>"
                   "<Behavior>SELECTABLE, UNIT_AI</Behavior></GroundVehicle>\n"
                   "<GroundCompany Name=\"Trooper_Team\"><Company_Units>Trooper, Trooper</Company_Units>"
                   "<Behavior>TEAM, TEAM_LOCOMOTOR</Behavior></GroundCompany>\n"
                   "<GroundCompany Name=\"Squad\"><Company_Units>Squaddie, Squaddie</Company_Units>"
                   "<Behavior>TEAM, LAND_TEAM_CONTAINER_LOCOMOTOR</Behavior></GroundCompany>\n"
                   "<GroundInfantry Name=\"Squaddie\"><Behavior>LAND_TEAM_INFANTRY_LOCOMOTOR, UNIT_AI</Behavior></GroundInfantry>\n"
                   "<Marker Name=\"Marker_Generic\"><Behavior>MARKER</Behavior></Marker>\n"
                   "<SpaceUnit Name=\"Hero_Ship\"><Is_Named_Hero>Yes</Is_Named_Hero><Tactical_Health>1000</Tactical_Health>"
                   "<AI_Combat_Power>700</AI_Combat_Power><Build_Cost_Credits>1500</Build_Cost_Credits>"
                   "<Tech_Level>3</Tech_Level><Score_Cost_Credits>2000</Score_Cost_Credits>"
                   "<Tactical_Build_Cost_Multiplayer>900</Tactical_Build_Cost_Multiplayer>"
                   "<Behavior>TRANSPORT</Behavior></SpaceUnit>\n"
                   "<Planet Name=\"Planet_X\"><Behavior>PLANET</Behavior></Planet>\n"
                   "<Planet Name=\"Planet_Y\"><Behavior>PLANET</Behavior></Planet>\n"
                   "<GalacticFleet Name=\"Fleet\"><Behavior>FLEET</Behavior></GalacticFleet>\n"
                   "<HardPoint Name=\"HP_Gun\"><Tactical_Health>30</Tactical_Health></HardPoint>\n"
                   "<StarBase Name=\"Station\"><Tactical_Health>5000</Tactical_Health></StarBase>\n"
                   "<GroundStructure Name=\"Bunker\"><Behavior>GARRISON_STRUCTURE</Behavior>"
                   "<Garrison_Category>Infantry</Garrison_Category></GroundStructure>\n"
                   // refixf t03 finder tests (test_find_filters).
                   "<GroundInfantry Name=\"Probe_Droid\"><Behavior>UNIT_AI</Behavior></GroundInfantry>\n"
                   "<GroundStructure Name=\"Depot\"><CategoryMask>Structure</CategoryMask>"
                   "<Property_Flags>CanContainGarrison | TacticalStructure</Property_Flags></GroundStructure>\n"
                   "<SpaceUnit Name=\"Cruiser\"><CategoryMask>Capital</CategoryMask><Behavior>UNIT_AI</Behavior></SpaceUnit>\n"
                   "<Team Name=\"Team_Box\"><Behavior>TEAM, UNIT_AI</Behavior></Team>\n"
                   "<GroundCompany Name=\"Squad_Co\"><CategoryMask>Air</CategoryMask>"
                   "</GroundCompany>\n"
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
                                  objects::ObjectHandle attach = {}) {
    objects::SpawnParams sp;
    sp.type = t;
    sp.owner = owner;
    sp.position = p;
    sp.facing = {1.0f, 0.0f, 0.0f};
    sp.attach_parent = attach;
    return st.create(sp);
}

// Tactical scene: ids 1 Trooper(p1) @0, 2 Trooper(p2) @10, 3 Tank(p2) @5, 4/5 markers, 6 HP_Gun on 3.
struct Scene {
    objects::IdCounter ids;
    objects::ObjectStore store;
    ObjectBridge bridge;
    LuaHost host;
    std::unique_ptr<LuaVm> vm;
    objects::ObjectHandle trooper1, trooper2, tank, marker1, marker2, gun;
    explicit Scene(const Db& db, objects::ModeKind mode = objects::ModeKind::Land)
        : store(mode, objects::StoreKind::Primary, db.types, &ids) {
        trooper1 = make(store, db.t("Trooper"), 1, {0, 0, 0});
        trooper2 = make(store, db.t("Trooper"), 2, {10, 0, 0});
        tank = make(store, db.t("Tank"), 2, {5, 0, 0});
        marker1 = make(store, db.t("Marker_Generic"), -1, {100, 0, 0});
        marker2 = make(store, db.t("Marker_Generic"), -1, {200, 0, 0});
        gun = make(store, db.t("HP_Gun"), 2, {5, 1, 0}, tank);
        bridge.set_hint(store.id(marker1), "spawnpoint");
        bridge.set_hint(store.id(marker2), "SpawnPoint");
        host.store = &store;
        host.types = &db.types;
        vm = std::make_unique<LuaVm>(host);
        attach_object_bridge(*vm, &bridge);
        CHECK(run(*vm, "t1 = Find_All_Objects_Of_Type('Trooper')[2] t2 = Find_All_Objects_Of_Type('Trooper')[1] "
                       "tank = Find_All_Objects_Of_Type('Tank')[1] p1 = t1.Get_Owner() p2 = t2.Get_Owner()") == "");
    }
};

// ------------------------------------------------------------------------------------------

static void test_registry_rows() {
    // Every implementation of this family must be a row of the RE table (no "extra" methods).
    const char* globals[] = {"Find_Hint", "Find_All_Objects_With_Hint", "Find_First_Object", "FindPlanet",
                             "Find_Nearest", "Spawn_Unit", "Create_Generic_Object", "Hide_Object", "Create_Position"};
    for (const char* g : globals) {
        bool found = false;
        for (const BindingInfo& b : binding_table())
            if (b.kind == BindingKind::Global && std::strcmp(b.name, g) == 0) found = binding_implemented(b);
        CHECK(found);
        if (!found) std::printf("  global not implemented: %s\n", g);
    }
    LuaHost host;
    LuaVm vm(host);
    CHECK(vm.resolve_method("GameObjectWrapper", "Move_To") == 1);
    CHECK(vm.resolve_method("GameObjectWrapper", "Get_Position") == 1);
    CHECK(vm.resolve_method("PositionWrapper", "Get_XYZ") == 1);
    CHECK(vm.resolve_method("BlockingStatus", "IsFinished") == 1);
    CHECK(vm.resolve_method("FindPlanetClass", "Get_All_Planets") == 1);
    CHECK(vm.resolve_method("GameObjectTypeWrapper", "Is_Hero") == 1);
    const std::string rep = binding_coverage_report();
    CHECK(!contains(rep, "method extra        GameObjectWrapper") && !contains(rep, "method extra        PositionWrapper"));
}

static void test_find(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    // Find_Hint: type wrapper or name, case-insensitive hint, newest first.
    CHECK(run(vm, "h1 = Find_Hint('Marker_Generic', 'spawnpoint').Get_Object_ID() "
                  "h2 = Find_Hint(Find_Object_Type('marker_generic'), 'SPAWNPOINT').Get_Object_ID() "
                  "h3 = tostring(Find_Hint('Trooper', 'spawnpoint')) h4 = tostring(Find_Hint('Marker_Generic', 'nope'))") == "");
    CHECK(gnum(vm, "h1") == 5.0 && gnum(vm, "h2") == 5.0 && gstr(vm, "h3") == "nil" && gstr(vm, "h4") == "nil");
    CHECK(contains(run_stop(vm, "Find_Hint('x')"), "LuaFindMarkerCommandClass -- Invalid number of parameters."));
    CHECK(contains(run_stop(vm, "Find_Hint(5, 'x')"), "LuaFindMarkerCommandClass -- Expected type or name for parameter 1"));
    // Find_All_Objects_With_Hint: newest first.
    CHECK(run(vm, "l = Find_All_Objects_With_Hint('SpawnPoint') hn = table.getn(l) hs = l[1].Get_Object_ID() .. ',' .. l[2].Get_Object_ID() "
                  "he = table.getn(Find_All_Objects_With_Hint('none'))") == "");
    CHECK(gnum(vm, "hn") == 2.0 && gstr(vm, "hs") == "5,4" && gnum(vm, "he") == 0.0);
    CHECK(contains(run_stop(vm, "Find_All_Objects_With_Hint()"), "invalid number of parameters. Expected 1, got 0."));
    CHECK(contains(run_stop(vm, "Find_All_Objects_With_Hint(1)"), "invalid type for parameter 1. Expected string."));
    CHECK(run(vm, "gh = Find_Hint('Marker_Generic', 'spawnpoint').Get_Hint() gn = tostring(t1.Get_Hint())") == "");
    CHECK(gstr(vm, "gh") == "SpawnPoint" && gstr(vm, "gn") == "nil");
    // q6 t28: map hints (.ted 0x482) come from the host's map_hint hook; a script-set hint wins over it.
    {
        const objects::ObjectId tank_id = s.store.id(s.tank), m1 = s.store.id(s.marker1);
        s.bridge.map_hint = [&s, tank_id, m1](objects::ObjectHandle h) {
            const objects::ObjectId id = s.store.id(h);
            return id == tank_id ? std::string_view("falcongear1") : id == m1 ? std::string_view("mapname") : std::string_view();
        };
        CHECK(run(vm, "mh1 = Find_Hint('Tank', 'FalconGear1').Get_Object_ID() mh2 = tostring(Find_Hint('Trooper', 'falcongear1')) "
                      "mh3 = tank.Get_Hint() mh4 = tostring(Find_Hint('Marker_Generic', 'mapname')) "
                      "mh5 = table.getn(Find_All_Objects_With_Hint('falcongear1')) mh6 = Find_Hint('Marker_Generic', 'spawnpoint').Get_Object_ID()") == "");
        CHECK(gnum(vm, "mh1") == static_cast<double>(tank_id) && gstr(vm, "mh2") == "nil" && gstr(vm, "mh3") == "falcongear1");
        CHECK(gstr(vm, "mh4") == "nil" && gnum(vm, "mh5") == 1.0 && gnum(vm, "mh6") == 5.0);
        s.bridge.map_hint = nullptr;
    }
    // Find_First_Object: newest first, case-insensitive type name.
    CHECK(run(vm, "f1 = Find_First_Object('trooper').Get_Object_ID() f2 = tostring(Find_First_Object('Nope'))") == "");
    CHECK(gnum(vm, "f1") == 2.0 && gstr(vm, "f2") == "nil");
    // refixf t03: errors log and return nothing (no unwind); category names are not type names.
    CHECK(run(vm, "fe = Find_First_Object(3) fe = tostring(fe) fc = tostring(Find_First_Object('Infantry'))") == "" &&
          gstr(vm, "fe") == "nil" && gstr(vm, "fc") == "nil");
    CHECK(run(vm, "Find_First_Object(3)") == "" && last_message_has(vm, "FindPlanet -- Parameter 1 is not a valid string"));
    CHECK(run(vm, "Find_First_Object()") == "" && last_message_has(vm, "FindPlanet -- No planet name set"));
    // Find_Nearest (refixf t03): type filter, enemies of a player, owned by a player, category, property. Candidates are
    // UNIT_AI objects of players (the markers are ownerless, the gun has no UNIT_AI and no CategoryMask).
    CHECK(run(vm, "n1 = Find_Nearest(t1, 'Tank').Get_Object_ID() "
                  "n2 = Find_Nearest(t1, p1, false).Get_Object_ID() "     // enemies of p1: tank @5
                  "n3 = tostring(Find_Nearest(t1, p1, true)) "            // p1 owns nothing else
                  "n4 = Find_Nearest(t1, 'Infantry').Get_Object_ID() "    // category -> trooper 2
                  "n5 = Find_Nearest(t1, 'HealsInfantry', p2, true).Get_Object_ID() "  // property, owned by p2
                  "n6 = Find_Nearest(tank, 'Fodder').Get_Object_ID() "    // trooper 1 and 2 both at 5: lowest player (1)
                  "n8 = Find_Nearest(t1).Get_Object_ID() "
                  "n10 = Find_Nearest(t1, p1).Get_Object_ID() "           // one slot only: the player is ignored
                  "n11 = Find_Nearest(t1, 'Infantry', p1).Get_Object_ID() "  // ditto after a filter
                  "n12 = Find_Nearest(t1, 'Infantry | Vehicle').Get_Object_ID()") == "");
    CHECK(gnum(vm, "n1") == 3.0 && gnum(vm, "n2") == 3.0 && gstr(vm, "n3") == "nil" && gnum(vm, "n4") == 2.0);
    CHECK(gnum(vm, "n5") == 3.0 && gnum(vm, "n6") == 1.0 && gnum(vm, "n8") == 3.0);
    CHECK(gnum(vm, "n10") == 3.0 && gnum(vm, "n11") == 2.0 && gnum(vm, "n12") == 3.0);
    CHECK(run(vm, "n7 = Find_Nearest(t1, 'NoSuchThing') n7 = tostring(n7)") == "" && gstr(vm, "n7") == "nil" &&
          last_message_has(vm, "Find_Nearest - unknown game object type, properties mask or catefory NoSuchThing."));
    CHECK(run(vm, "Find_Nearest()") == "" && last_message_has(vm, "Expected between 1 and 4, got 0."));
    CHECK(run(vm, "Find_Nearest(5)") == "" && last_message_has(vm, "invalid type for parameter 1; expected game object"));
    // modc t06: a nil filter (a mod's category helper returning nil) is not a filter, the player is then read at slot 2;
    // a non-player there is a null player (no player filter), a non-bool flag keeps true. No error in any of these.
    {
        const size_t before = vm.messages().size();
        CHECK(run(vm, "n14 = Find_Nearest(t1, nil, p1, false).Get_Object_ID() "  // slot 2 is nil: null player, all players
                      "n15 = Find_Nearest(t1, nil, nil, true).Get_Object_ID() "
                      "n16 = Find_Nearest(t1, 'Infantry', 7, 'x').Get_Object_ID()") == "");
        CHECK(gnum(vm, "n14") == 3.0 && gnum(vm, "n15") == 3.0 && gnum(vm, "n16") == 2.0);
        CHECK(vm.messages().size() == before);
    }
    // Bridge relation hook: "allied or self" = not an enemy. Nothing is p1's enemy -> p2's tank is a friend of p1.
    s.bridge.is_enemy = [](int32_t, int32_t) { return false; };
    CHECK(run(vm, "n9 = Find_Nearest(t1, p1, true).Get_Object_ID() n13 = tostring(Find_Nearest(t1, p1, false))") == "" &&
          gnum(vm, "n9") == 3.0 && gstr(vm, "n13") == "nil");
    s.bridge.is_enemy = nullptr;
    // Positions.
    CHECK(run(vm, "pos = tank.Get_Position() x, y, z = pos.Get_XYZ() d = t1.Get_Distance(tank) d2 = t1.Get_Distance(Create_Position(3, 4, 0)) "
                  "pv = pos.Is_Valid() and 1 or 0 same = (Create_Position(5, 0, 0) == pos) and 1 or 0 "
                  "ps = tostring(pos)") == "");
    CHECK(gnum(vm, "x") == 5.0 && gnum(vm, "y") == 0.0 && gnum(vm, "d") == 5.0 && gnum(vm, "d2") == 5.0);
    CHECK(gnum(vm, "pv") == 1.0 && gnum(vm, "same") == 1.0 && gstr(vm, "ps") == "PositionWrapper");
    CHECK(contains(run_stop(vm, "t1.Get_Distance()"), "Expceted 1, got 0."));
    CHECK(contains(run_stop(vm, "t1.Get_Distance(5)"), "could not extract a position from parameter 1."));
    CHECK(contains(run_stop(vm, "Create_Position(1, 2)"), "LuaCreatePosition -- Expected 3 float parameters"));
    CHECK(contains(run_stop(vm, "Create_Position(1, 2, 'a')"), "LuaCreatePosition -- Invalid parameter, expected 3 floats"));
}

// Stage 3: proximity events (q6, OBJECT_SCRIPTS.md §4.3: synchronous callbacks), reinforcements, Get_Space_Station.
static void test_stage3(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    // t1 @0; trooper 2 @10, tank @5, gun @(5,1) (all p2); markers @100/@200 (no owner).
    // Box of half-size 5: the tank (5,0,0) and the gun (5,1,0) are in it (the gun is sqrt(26) > 5 away: a box, not a
    // sphere); called synchronously inside Service_Wrapper, ascending id.
    CHECK(run(vm, "hits = '' function Prox(self_obj, trig) hits = hits .. self_obj.Get_Object_ID() .. '>' .. "
                  "trig.Get_Object_ID() .. ',' end t1.Event_Object_In_Range(Prox, 5) t1.Service_Wrapper()") == "");
    CHECK(gstr(vm, "hits") == "1>3,1>6,");
    // The same function again is a second registration (no dedupe); player filter p2 (owner or ally).
    CHECK(run(vm, "hits = '' t1.Event_Object_In_Range(Prox, 20, p2) t1.Service_Wrapper()") == "");
    CHECK(gstr(vm, "hits") == "1>3,1>6,1>2,1>3,1>6,");
    // Fires on every call (no enter / leave edge); Cancel removes every registration of the function.
    CHECK(run(vm, "hits = '' t1.Service_Wrapper() t1.Cancel_Event_Object_In_Range(Prox) t1.Service_Wrapper()") == "");
    CHECK(gstr(vm, "hits") == "1>3,1>6,1>2,1>3,1>6,");
    // Type filter (either argument slot), player filter of a player with nothing else in range.
    CHECK(run(vm, "hits = '' t1.Event_Object_In_Range(Prox, 20, Find_Object_Type('Tank')) t1.Service_Wrapper() "
                  "t1.Cancel_Event_Object_In_Range(Prox) t1.Event_Object_In_Range(Prox, 20, p2, Find_Object_Type('Trooper')) "
                  "t1.Service_Wrapper() t1.Cancel_Event_Object_In_Range(Prox) t1.Event_Object_In_Range(Prox, 20, p1) "
                  "t1.Service_Wrapper() t1.Cancel_Event_Object_In_Range(Prox)") == "");
    CHECK(gstr(vm, "hits") == "1>3,1>2,");
    // A callback that cancels itself fires once; one that registers restarts the scan, done entries are skipped.
    CHECK(run(vm, "hits = '' function Once(o, t) hits = hits .. 'o' .. t.Get_Object_ID() .. ',' "
                  "o.Cancel_Event_Object_In_Range(Once) end t1.Event_Object_In_Range(Once, 20) t1.Service_Wrapper() "
                  "t1.Service_Wrapper()") == "");
    CHECK(gstr(vm, "hits") == "o2,");
    CHECK(run(vm, "hits = '' added = false function Adder(o, t) hits = hits .. 'a' .. t.Get_Object_ID() .. ',' "
                  "if not added then added = true o.Event_Object_In_Range(Prox, 5) end end "
                  "t1.Event_Object_In_Range(Adder, 20) t1.Service_Wrapper() t1.Cancel_Event_Object_In_Range(Adder) "
                  "t1.Cancel_Event_Object_In_Range(Prox)") == "");
    CHECK(gstr(vm, "hits") == "a2,1>3,1>6,");
    // A callback's Lua error is the binding's error (the calling thread dies).
    CHECK(contains(run_stop(vm, "function Bad(o, t) error('boom') end t1.Event_Object_In_Range(Bad, 20) t1.Service_Wrapper()"),
                   "Service_Wrapper -- "));
    CHECK(run(vm, "t1.Cancel_Event_Object_In_Range(Bad)") == "" && !vm.exit_requested());
    // A host spatial query narrows the candidates (the binding still re-tests the box).
    s.bridge.objects_in_range = [&s](Vec3, float, std::vector<objects::ObjectId>& out) { out = {s.store.id(s.tank)}; };
    CHECK(run(vm, "hits = '' t1.Event_Object_In_Range(Prox, 20) t1.Service_Wrapper() t1.Cancel_Event_Object_In_Range(Prox)") == "");
    CHECK(gstr(vm, "hits") == "1>3,");
    s.bridge.objects_in_range = nullptr;
    // q6 t12: the proximity bindings on a dead wrapper (an object script's own unit killed under it) are silent no-ops,
    // not the generic "this object is dead!" thread error (§4.3: the exe has no dead-object string for these).
    CHECK(run(vm, "hits = '' dead = Create_Generic_Object('Trooper', Create_Position(50, 0, 0), p2) dead.Despawn() "
                  "dead.Event_Object_In_Range(Prox, 5) dead.Service_Wrapper() dead.Cancel_Event_Object_In_Range(Prox)") == "");
    CHECK(gstr(vm, "hits") == "" && !vm.exit_requested());
    // GetEvent keeps posted named events FIFO; Params nil for them.
    vm.post_event("Named");
    CHECK(run(vm, "named = 0 function Named() named = named + 1 end ev = GetEvent() pn = tostring(GetEvent.Params()) ev()") == "");
    CHECK(gnum(vm, "named") == 1.0 && gstr(vm, "pn") == "nil");
    // Argument errors are the script error (+0x121: logged, the script's exit flag; no unwind).
    CHECK(run(vm, "t1.Event_Object_In_Range(5, 1)") == "" &&
          last_message_has(vm, "Event_Object_In_Range -- Expected function object as parameter 1.") && vm.exit_requested());
    CHECK(run(vm, "t1.Event_Object_In_Range(Prox, 'x')") == "" && last_message_has(vm, "Expected number as parameter 2."));
    CHECK(run(vm, "t1.Event_Object_In_Range(Prox)") == "" && last_message_has(vm, "Event_Object_In_Range -- Invalid number of params."));
    CHECK(run(vm, "t1.Cancel_Event_Object_In_Range()") == "" &&
          last_message_has(vm, "Cancel_Event_Object_In_Range -- Invalid number of params."));

    // Reinforce_Unit without a host hook: created at once at the location (company container), finished.
    CHECK(run(vm, "blk = Reinforce_Unit(Find_Object_Type('Trooper_Team'), Create_Position(50, 0, 0), p1, true, false) "
                  "fin = blk.IsFinished() and 1 or 0 units = blk.Result() rn = table.getn(units) "
                  "rt = units[1].Get_Type().Get_Name() ro = units[1].Get_Owner().Get_ID() "
                  "b2 = Reinforce_Unit('Tank', false, p1) f2 = b2.IsFinished() and 1 or 0 n2 = table.getn(b2.Result())") == "");
    CHECK(gnum(vm, "fin") == 1.0 && gnum(vm, "rn") == 1.0 && gstr(vm, "rt") == "Trooper_Team" && gnum(vm, "ro") == 1.0);
    CHECK(gnum(vm, "f2") == 1.0 && gnum(vm, "n2") == 0.0);
    // With a hook: pending until the host finishes the request.
    ReinforceRequest seen;
    s.bridge.reinforce = [&seen](const ReinforceRequest& r) {
        seen = r;
        return true;
    };
    CHECK(run(vm, "b3 = Reinforce_Unit('Tank', tank, p2, false, true) f3 = b3.IsFinished() and 1 or 0 r3 = tostring(b3.Result())") ==
          "");
    CHECK(gnum(vm, "f3") == 0.0 && gstr(vm, "r3") == "nil" && seen.player == 2 && seen.has_location &&
          seen.marker == s.store.id(s.tank) && seen.obey_zones && !seen.ignore_rules && seen.type == db.t("Tank"));
    s.bridge.finish_reinforcement(seen.id, {s.store.id(s.trooper1)});
    CHECK(run(vm, "f4 = b3.IsFinished() and 1 or 0 r4 = b3.Result()[1].Get_Object_ID()") == "" && gnum(vm, "f4") == 1.0 &&
          gnum(vm, "r4") == 1.0);
    s.bridge.reinforce = nullptr;
    CHECK(contains(run_stop(vm, "Reinforce_Unit('Tank')"), "LuaReinforceCommandClass::Reinforce -- Invalid number of parameters."));
    CHECK(contains(run_stop(vm, "Reinforce_Unit('Nope', false, p1)"), "Reinforce -- Invalid parameter for type."));
    CHECK(contains(run_stop(vm, "Reinforce_Unit('Tank', false, 5)"), "Reinforce -- Invalid parameter for player."));
    CHECK(contains(run_stop(vm, "Reinforce_Unit('Tank', 5, p1)"), "Reinforce -- Invalid position parameter."));
    // Spawn_From_Reinforcement_Pool: direct spawn stand-in, or the host's pool.
    CHECK(run(vm, "sp = Spawn_From_Reinforcement_Pool(Find_Object_Type('Tank'), tank, p1) sn = table.getn(sp) "
                  "st = sp[1].Get_Type().Get_Name()") == "" && gnum(vm, "sn") == 1.0 && gstr(vm, "st") == "Tank");
    s.bridge.spawn_from_pool = [](objects::TypeIndex, int32_t, Vec3) { return std::vector<objects::ObjectHandle>{}; };
    CHECK(run(vm, "sn2 = table.getn(Spawn_From_Reinforcement_Pool('Tank', tank, p1))") == "" && gnum(vm, "sn2") == 0.0);
    s.bridge.spawn_from_pool = nullptr;
    CHECK(contains(run_stop(vm, "Spawn_From_Reinforcement_Pool('Tank', tank)"), "Expected 3, got 2"));
    // Get_Space_Station: the player's newest StarBase.
    const objects::ObjectHandle station = make(s.store, db.t("Station"), 2, {0, 50, 0});
    CHECK(run(vm, "ss = p2.Get_Space_Station().Get_Object_ID() ss1 = tostring(p1.Get_Space_Station())") == "");
    CHECK(gnum(vm, "ss") == static_cast<double>(s.store.id(station)) && gstr(vm, "ss1") == "nil");
    // Bridge digest reflects the reinforcement records.
    StateHasher h1;
    s.bridge.hash(h1);
    s.bridge.new_reinforcement();
    StateHasher h2;
    s.bridge.hash(h2);
    CHECK(h1.digest() != h2.digest());
    CHECK(vm.errors().empty());
}

// refixf t03 (docs/re/LUA_API.md "Find_All_Objects_Of_Type and the sibling finders"): the string chain (properties
// mask, category mask, type name), "A | B" masks, AND-combined filters, the owner filter, newest-first order, the
// error cases (nothing returned), the representative type (ObjectBridge::representative_type), death clones / killed
// objects, Find_Nearest's fog test.
static std::string ids_of(LuaVm& vm, const std::string& args) {
    const std::string code = "r = Find_All_Objects_Of_Type(" + args + ") ids = 'nil' if r then ids = '' "
                             "for _, o in ipairs(r) do ids = ids .. o.Get_Object_ID() .. ',' end end";
    const std::string err = run(vm, code);
    return err.empty() ? gstr(vm, "ids") : "error: " + err;
}
static void test_find_filters(const Db& db) {
    objects::IdCounter ids;
    objects::ObjectStore st(objects::ModeKind::Land, objects::StoreKind::Primary, db.types, &ids);
    make(st, db.t("Trooper"), 1, {0, 0, 0});                                     // 1
    make(st, db.t("Depot"), 1, {50, 0, 0});                                      // 2
    const objects::ObjectHandle tank = make(st, db.t("Tank"), 2, {5, 0, 0});     // 3
    make(st, db.t("Cruiser"), 1, {30, 0, 0});                                    // 4
    const objects::ObjectHandle tr5 = make(st, db.t("Trooper"), 2, {10, 0, 0});  // 5
    const objects::ObjectHandle team = make(st, db.t("Team_Box"), 2, {12, 0, 0});  // 6
    const objects::ObjectHandle probe = make(st, db.t("Probe_Droid"), 2, {1, 0, 0});  // 7
    ObjectBridge bridge;
    LuaHost host;
    host.store = &st;
    host.types = &db.types;
    LuaVm vm(host);
    attach_object_bridge(vm, &bridge);
    CHECK(run(vm, "t1 = Find_All_Objects_Of_Type('Trooper')[2] p1 = Find_First_Object('Depot').Get_Owner() "
                  "p2 = Find_First_Object('Tank').Get_Owner() tt = Find_Object_Type('Trooper')") == "");
    // Category / properties names, case-insensitive, "|" / "," / space separators, AND with the other filters.
    CHECK(ids_of(vm, "'Structure'") == "2,");
    CHECK(ids_of(vm, "p1, 'Capital'") == "4," && ids_of(vm, "'Capital', p2") == "");
    CHECK(ids_of(vm, "'Infantry'") == "5,1,");
    CHECK(ids_of(vm, "'infantry | VEHICLE'") == "5,3,1," && ids_of(vm, "'Infantry,Vehicle'") == "5,3,1," &&
          ids_of(vm, "' Infantry \\t Vehicle\\n'") == "5,3,1,");
    CHECK(ids_of(vm, "'Fighter | Bomber'") == "");  // a valid mask with no match: an empty table, not nil
    CHECK(ids_of(vm, "'Capital', 'HealsInfantry'") == "" && ids_of(vm, "'HealsInfantry', 'Vehicle'") == "3,");
    CHECK(ids_of(vm, "'CanContainGarrison', p1") == "2," && ids_of(vm, "'TacticalStructure | Turret'") == "2,");
    CHECK(ids_of(vm, "'TROOPER', p2") == "5," && ids_of(vm, "p1, tt") == "1," && ids_of(vm, "tt, 'Fodder', p1") == "1,");
    CHECK(ids_of(vm, "''") == "");  // all-delimiter string: properties mask 0, which no object passes
    // A player alone; a later player replaces an earlier one; newest first.
    CHECK(ids_of(vm, "p2") == "7,6,5,3," && ids_of(vm, "p1, p2") == "7,6,5,3," && ids_of(vm, "p2, p1") == "4,2,1,");
    // Errors: logged, nothing returned (nil), no unwind.
    CHECK(ids_of(vm, "'Trooper', 'Tank'") == "nil" && last_message_has(vm, "FindAllObjectsOfType -- unknown filter parameter 2."));
    CHECK(ids_of(vm, "'Infantry', 'Vehicle'") == "nil" && last_message_has(vm, "unknown filter parameter 2."));
    CHECK(ids_of(vm, "tt, 'Tank'") == "nil" && last_message_has(vm, "unknown filter parameter 2."));
    CHECK(ids_of(vm, "tt, Find_Object_Type('Tank')") == "nil" && last_message_has(vm, "unknown filter parameter 2."));
    CHECK(ids_of(vm, "p1, 'Bogus'") == "nil" && last_message_has(vm, "unknown filter parameter 2."));
    CHECK(ids_of(vm, "5") == "nil" && last_message_has(vm, "unknown filter parameter 1."));
    CHECK(ids_of(vm, "") == "nil" && last_message_has(vm, "FindAllObjectsOfType -- Expected a filter parameter."));
    // Representative type: the Team_Box container stands for Squad_Co (Air) through the host hook.
    CHECK(ids_of(vm, "'Team_Box'") == "6," && ids_of(vm, "'Air'") == "");
    bridge.representative_type = [&](objects::ObjectHandle h) { return h == team ? db.t("Squad_Co") : objects::kNone; };
    CHECK(ids_of(vm, "'Team_Box'") == "" && ids_of(vm, "'Squad_Co'") == "6," && ids_of(vm, "'Air', p2") == "6,");
    CHECK(run(vm, "f6 = Find_First_Object('Team_Box').Get_Object_ID() n6 = Find_Nearest(t1, 'Squad_Co').Get_Object_ID() "
                  "a6 = Find_Nearest(t1, 'Air').Get_Object_ID()") == "");
    CHECK(gnum(vm, "f6") == 6.0 && gnum(vm, "n6") == 6.0 && gnum(vm, "a6") == 6.0);  // Find_First_Object: own type
    // Find_Nearest never finds a representative type with CategoryMask 0 (the probe, nearest at 1).
    CHECK(run(vm, "q1 = Find_Nearest(t1).Get_Object_ID() q2 = tostring(Find_Nearest(t1, 'Probe_Droid'))") == "");
    CHECK(gnum(vm, "q1") == 3.0 && gstr(vm, "q2") == "nil");    // Death clones (dying state): found by Find_All_Objects_Of_Type / Find_First_Object as valid wrappers, never by
    // Find_Nearest. Killed objects pending the flush are skipped [port].
    st.set_flag(tank, objects::kFlagDeathClone);
    CHECK(ids_of(vm, "'Tank'") == "3," && run(vm, "v3 = Find_First_Object('Tank').Is_Valid() and 1 or 0 "
                                               "q3 = Find_Nearest(t1).Get_Object_ID() q4 = tostring(Find_Nearest(t1, 'Vehicle'))") == "");
    CHECK(gnum(vm, "v3") == 1.0 && gnum(vm, "q3") == 5.0 && gstr(vm, "q4") == "nil");
    CHECK(st.destroy(probe));
    CHECK(ids_of(vm, "p2") == "6,5,3," && run(vm, "fp = tostring(Find_First_Object('Probe_Droid'))") == "" && gstr(vm, "fp") == "nil");
#ifdef ALAMO_LUA_HAVE_FOG
    // Find_Nearest with a player skips objects fogged for that player (FOW_Object_Hidden, force = 1).
    FogScriptHooks fog;
    TacticalScriptHooks tac;
    tac.fog = &fog;
    bridge.tactical = &tac;
    fog.object_hidden = [&](objects::ObjectHandle h, int32_t player, bool force) { return h == tr5 && player == 1 && force; };
    CHECK(run(vm, "h1 = Find_Nearest(t1, p1, false).Get_Object_ID() "  // trooper 5 hidden from p1 -> the team (Squad_Co)
                  "h2 = Find_Nearest(t1, 'Infantry').Get_Object_ID()") == "");  // no player: no fog test
    CHECK(gnum(vm, "h1") == 6.0 && gnum(vm, "h2") == 5.0);
    bridge.tactical = nullptr;
#else
    (void)tr5;
#endif
}

static void test_queries(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    CHECK(run(vm, "nm = tank.Get_Name() c1 = t1.Is_Category('Infantry') and 1 or 0 c2 = t1.Is_Category('Vehicle') and 1 or 0 "
                  "c3 = t1.Is_Category('Bogus') and 1 or 0 pr = tank.Has_Property('HealsInfantry') and 1 or 0 "
                  "pr2 = tank.Has_Property('Fodder') and 1 or 0 sh = tank.Get_Shield() sh2 = t1.Get_Shield() "
                  "st = tank.Get_Game_Scoring_Type().Get_Name() sel = t1.Is_Selectable() and 1 or 0 "
                  "mv = t1.Can_Move() and 1 or 0 mv2 = tank.Can_Move() and 1 or 0 tr = t1.Is_Transport() and 1 or 0") == "");
    CHECK(gstr(vm, "nm") == "Tank" && gnum(vm, "c1") == 1.0 && gnum(vm, "c2") == 0.0 && gnum(vm, "c3") == 0.0);
    CHECK(gnum(vm, "pr") == 1.0 && gnum(vm, "pr2") == 0.0 && gnum(vm, "sh") == 1.0 && gnum(vm, "sh2") == 0.0);
    CHECK(gstr(vm, "st") == "Tank" && gnum(vm, "sel") == 1.0 && gnum(vm, "mv") == 1.0 && gnum(vm, "mv2") == 0.0 && gnum(vm, "tr") == 0.0);
    CHECK(contains(run_stop(vm, "t1.Is_Category()"), "GameObjectWrapper::Is_Category -- invalid number of parameters. Expected 1, got 0."));
    CHECK(contains(run_stop(vm, "t1.Is_Category(1)"), "GameObjectWrapper::Is_Category -- parameter 1 is not a valid string."));
    // Type wrapper data.
    CHECK(run(vm, "ht = Find_Object_Type('Hero_Ship') hero = ht.Is_Hero() and 1 or 0 nohero = Find_Object_Type('Tank').Is_Hero() and 1 or 0 "
                  "cr = ht.Get_Combat_Rating() tl = ht.Get_Tech_Level() bc = ht.Get_Build_Cost() sc = ht.Get_Score_Cost_Credits() "
                  "tb = ht.Get_Tactical_Build_Cost() bl = ht.Get_Base_Level()") == "");
    CHECK(gnum(vm, "hero") == 1.0 && gnum(vm, "nohero") == 0.0 && gnum(vm, "cr") == 700.0 && gnum(vm, "tl") == 3.0);
    CHECK(gnum(vm, "bc") == 1500.0 && gnum(vm, "sc") == 2000.0 && gnum(vm, "tb") == 900.0 && gnum(vm, "bl") == 0.0);
    // Abilities from Unit_Abilities_Data.
    CHECK(run(vm, "a1 = t1.Has_Ability('SPRINT') and 1 or 0 a2 = t1.Has_Ability('stealth') and 1 or 0 a3 = t1.Has_Ability('JET_PACK') and 1 or 0 "
                  "a4 = tank.Has_Ability('SPRINT') and 1 or 0 r1 = t1.Is_Ability_Ready('SPRINT') and 1 or 0") == "");
    CHECK(gnum(vm, "a1") == 1.0 && gnum(vm, "a2") == 1.0 && gnum(vm, "a3") == 0.0 && gnum(vm, "a4") == 0.0 && gnum(vm, "r1") == 1.0);
    CHECK(contains(run_stop(vm, "t1.Has_Ability(1)"), "GameObjectWrapper::Has_Ability -- invalid type for ability 1. Expected string."));
}

// s78h t05 (TACTICAL_COMBAT §3.1.1): tactical Spawn_Unit faces the unit from its spawn point toward the world
// origin, +X at x = y = 0; an object location lends its position only; company members share the facing.
// Create_Generic_Object: s78i t02 (test_other_spawn_facing).
static void test_spawn_unit_facing(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    auto facing_of = [&](const char* global) {
        const objects::ObjectId id = static_cast<objects::ObjectId>(gnum(vm, global));
        return s.store.facing(s.store.find(id));
    };
    CHECK(run(vm, "a = Spawn_Unit(Find_Object_Type('Tank'), Create_Position(100, 0, 0), p1)[1].Get_Object_ID() "
                  "b = Spawn_Unit(Find_Object_Type('Tank'), Create_Position(0, 0, 7), p1)[1].Get_Object_ID() "
                  "d = Spawn_Unit(Find_Object_Type('Tank'), Create_Position(-3, 4, 0), p1)[1].Get_Object_ID()") == "");
    Vec3 f = facing_of("a");
    CHECK(f.x == -1.0f && f.y == 0.0f && f.z == 0.0f);
    f = facing_of("b");
    CHECK(f.x == 1.0f && f.y == 0.0f && f.z == 0.0f);
    f = facing_of("d");
    CHECK(f.x == 0.6f && f.y == -0.8f && f.z == 0.0f);
    // Object location: marker1 sits at (100, 0, 0) facing +Y; only its position counts.
    s.store.set_facing(s.marker1, {0.0f, 1.0f, 0.0f});
    CHECK(run(vm, "team = Spawn_Unit(Find_Object_Type('Trooper_Team'), Find_Hint('Marker_Generic', 'spawnpoint'), p1)[1] "
                  "tm = team.Get_Object_ID() mem = Find_All_Objects_Of_Type('Trooper') "
                  "m1 = mem[1].Get_Object_ID() m2 = mem[2].Get_Object_ID()") == "");
    for (const char* g : {"tm", "m1", "m2"}) {
        f = facing_of(g);
        CHECK(f.x == -1.0f && f.y == 0.0f && f.z == 0.0f);
    }
    // Create_Generic_Object: +X at a position.
    CHECK(run(vm, "g = Create_Generic_Object('Tank', Create_Position(100, 0, 0), p1).Get_Object_ID()") == "");
    f = facing_of("g");
    CHECK(f.x == 1.0f && f.y == 0.0f);
}

// s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): Create_Generic_Object faces +X even at an object location;
// Spawn_From_Reinforcement_Pool (stand-in) faces the world origin like Spawn_Unit; the Reinforce_Unit stand-in faces
// the owner's entry-facing record from the host hook, +X without one (never the location object's facing).
static void test_other_spawn_facing(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    auto facing_of = [&](const char* global) {
        const objects::ObjectId id = static_cast<objects::ObjectId>(gnum(vm, global));
        return s.store.facing(s.store.find(id));
    };
    // marker1 sits at (100, 0, 0); turn it to +Y so a lent facing would show.
    s.store.set_facing(s.marker1, {0.0f, 1.0f, 0.0f});
    CHECK(run(vm, "mk = Find_Hint('Marker_Generic', 'spawnpoint') "
                  "g = Create_Generic_Object('Tank', mk, p1).Get_Object_ID() "
                  "g2 = Create_Generic_Object('Tank', Create_Position(-3, 4, 0), p1).Get_Object_ID()") == "");
    for (const char* gname : {"g", "g2"}) {
        const Vec3 f = facing_of(gname);
        CHECK(f.x == 1.0f && f.y == 0.0f && f.z == 0.0f);
    }
    // Spawn_From_Reinforcement_Pool stand-in: toward the origin; at an object location only its position counts.
    CHECK(run(vm, "sp1 = Spawn_From_Reinforcement_Pool('Tank', Create_Position(-3, 4, 0), p1)[1].Get_Object_ID() "
                  "sp2 = Spawn_From_Reinforcement_Pool('Tank', mk, p1)[1].Get_Object_ID() "
                  "sp3 = Spawn_From_Reinforcement_Pool('Tank', Create_Position(0, 0, 9), p1)[1].Get_Object_ID()") == "");
    Vec3 f = facing_of("sp1");
    CHECK(f.x == 0.6f && f.y == -0.8f && f.z == 0.0f);
    f = facing_of("sp2");
    CHECK(f.x == -1.0f && f.y == 0.0f && f.z == 0.0f);
    f = facing_of("sp3");
    CHECK(f.x == 1.0f && f.y == 0.0f && f.z == 0.0f);
    // Reinforce_Unit stand-in without a facing hook: +X, not the marker's +Y.
    CHECK(run(vm, "r1 = Reinforce_Unit('Tank', mk, p1).Result()[1].Get_Object_ID()") == "");
    f = facing_of("r1");
    CHECK(f.x == 1.0f && f.y == 0.0f && f.z == 0.0f);
    // With the hook: the owner's record; a company's members share it.
    int32_t asked = -1;
    s.bridge.reinforce_facing = [&asked](int32_t player) {
        asked = player;
        return Vec3{0.0f, -1.0f, 0.0f};
    };
    CHECK(run(vm, "r2 = Reinforce_Unit(Find_Object_Type('Trooper_Team'), Create_Position(50, 0, 0), p2).Result()[1] "
                  "r2t = r2.Get_Object_ID() rm = r2t + 1") == "");  // the first member is created right after its container
    CHECK(asked == 2);
    CHECK(s.store.container(s.store.find(static_cast<objects::ObjectId>(gnum(vm, "rm")))) ==
          s.store.find(static_cast<objects::ObjectId>(gnum(vm, "r2t"))));
    for (const char* gname : {"r2t", "rm"}) {
        f = facing_of(gname);
        CHECK(f.x == 0.0f && f.y == -1.0f && f.z == 0.0f);
    }
    s.bridge.reinforce_facing = nullptr;
}

static void test_spawn_and_state(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    // Spawn_Unit: a table with the company container; members are contained.
    CHECK(run(vm, "mk = Find_Hint('Marker_Generic', 'spawnpoint') "
                  "list = Spawn_Unit(Find_Object_Type('Trooper_Team'), mk, p1) sn = table.getn(list) team = list[1] "
                  "tn = team.Get_Type().Get_Name() cnt = team.Get_Contained_Object_Count() "
                  "has = team.Contains_Object_Type(Find_Object_Type('Trooper')) and 1 or 0 "
                  "own = team.Get_Owner().Get_ID() tx = team.Get_Position().Get_XYZ() "
                  "members = Find_All_Objects_Of_Type('Trooper') par = members[1].Get_Parent_Object().Get_Object_ID() "
                  "tid = team.Get_Object_ID()") == "");
    CHECK(gnum(vm, "sn") == 1.0 && gstr(vm, "tn") == "Trooper_Team" && gnum(vm, "cnt") == 2.0 && gnum(vm, "has") == 1.0);
    // OI follow-up t04 (A1_READINGS §28.9 item 4): in land a team that is not a squad container locks / unlocks each
    // member (a second lock or unlock warns per member); a squad container locks itself, a squad member locks its
    // container, and Unlock on the container walks the members and leaves the container locked.
    auto said = [&vm](const char* text) {
        size_t n = 0;
        for (const std::string& m : vm.messages()) n += m.find(text) != std::string::npos ? 1u : 0u;
        return n;
    };
    CHECK(run(vm, "team.Lock_Current_Orders() team.Lock_Current_Orders() c1 = members[1].Can_Move() and 1 or 0 "
                  "c2 = members[2].Can_Move() and 1 or 0 tc = team.Can_Move() and 1 or 0") == "");
    CHECK(gnum(vm, "c1") == 0.0 && gnum(vm, "c2") == 0.0 && gnum(vm, "tc") == 1.0);
    CHECK(said("Lock_Current_Orders - Trooper is already locked.  Script cannot stack locks.") == 2);
    CHECK(run(vm, "team.Unlock_Current_Orders() team.Unlock_Current_Orders() c1 = members[1].Can_Move() and 1 or 0") == "");
    CHECK(gnum(vm, "c1") == 1.0 && said("Unlock_Current_Orders - object Trooper is not locked.") == 2);
    {  // s78n t06 review (A1_READINGS §28.9 item 4): a land TEAM Unlock decrements each locked member's counter, so a member
       // locked only by its hero clash moves again (its +0x3b4 stays: Unlock on the member itself is still the error)
        RemainingScriptHooks hk;
        bool held = true;
        CHECK(run(vm, "hm = members[1].Get_Object_ID()") == "");
        const objects::ObjectHandle hm = s.store.find(static_cast<objects::ObjectId>(gnum(vm, "hm")));
        hk.in_hero_clash = [hm](objects::ObjectHandle h) { return h == hm; };
        hk.hero_clash_locked = [hm, &held](objects::ObjectHandle h) { return h == hm && held; };
        hk.release_hero_clash_lock = [hm, &held](objects::ObjectHandle h) {
            if (h != hm || !held) return false;
            held = false;
            return true;
        };
        s.bridge.remaining = &hk;
        CHECK(run(vm, "c1 = members[1].Can_Move() and 1 or 0") == "" && gnum(vm, "c1") == 0.0);
        CHECK(run(vm, "team.Unlock_Current_Orders() c1 = members[1].Can_Move() and 1 or 0") == "");
        CHECK(!held && gnum(vm, "c1") == 1.0 && said("Unlock_Current_Orders - object Trooper is not locked.") == 3);  // member 2 only
        CHECK(contains(run_stop(vm, "members[1].Unlock_Current_Orders()"), "is locked by a hero clash.  Cannot unlock."));
        s.bridge.remaining = nullptr;
    }
    CHECK(run(vm, "sq = Spawn_Unit(Find_Object_Type('Squad'), mk, p1)[1] sm = Find_All_Objects_Of_Type('Squaddie') "
                  "sq.Lock_Current_Orders() q1 = sq.Can_Move() and 1 or 0 m1 = sm[1].Can_Move() and 1 or 0 "
                  "sm[2].Lock_Current_Orders() sq.Unlock_Current_Orders() q2 = sq.Can_Move() and 1 or 0") == "");
    CHECK(gnum(vm, "q1") == 0.0 && gnum(vm, "m1") == 1.0 && gnum(vm, "q2") == 0.0);
    CHECK(said("Lock_Current_Orders - Squad is already locked.") == 1 && said("object Squaddie is not locked.") == 2);
    CHECK(run(vm, "sq.Despawn()") == "");
    CHECK(gnum(vm, "own") == 1.0 && gnum(vm, "tx") == 200.0 && gnum(vm, "par") == gnum(vm, "tid"));
    CHECK(contains(run_stop(vm, "Spawn_Unit(Find_Object_Type('Tank'), mk)"), "LuaSpawnUnitCommandClass -- Invalid number of parameters."));
    CHECK(contains(run_stop(vm, "Spawn_Unit('Tank', mk, p1)"), "LuaSpawnUnitCommandClass -- Invalid parameter for type."));
    CHECK(contains(run_stop(vm, "Spawn_Unit(Find_Object_Type('Tank'), mk, 1)"), "LuaSpawnUnitCommandClass -- Invalid parameter for player."));
    // Despawn of a TEAM despawns the members, reverse order; the team stays.
    CHECK(run(vm, "team.Despawn() alive = team.Is_Valid() and 1 or 0 m1 = members[1].Is_Valid() and 1 or 0 "
                  "m2 = members[2].Is_Valid() and 1 or 0 cnt2 = team.Get_Contained_Object_Count()") == "");
    CHECK(gnum(vm, "alive") == 1.0 && gnum(vm, "m1") == 0.0 && gnum(vm, "m2") == 0.0 && gnum(vm, "cnt2") == 0.0);
    {  // q6 t12: a host that lays out companies (ObjectBridge::spawn_company) takes a company; a plain type is not
       // offered; a null answer falls back to the store container + members.
        int calls = 0;
        objects::TypeIndex asked = objects::kNone;
        objects::ObjectHandle give = s.trooper1;
        s.bridge.spawn_company = [&](const objects::SpawnParams& p) {
            ++calls;
            asked = p.type;
            return give;
        };
        CHECK(run(vm, "hc = Spawn_Unit(Find_Object_Type('Trooper_Team'), mk, p1)[1].Get_Object_ID() "
                      "Spawn_Unit(Find_Object_Type('Tank'), mk, p1)[1].Despawn()") == "");
        CHECK(calls == 1 && asked == db.t("Trooper_Team") && gnum(vm, "hc") == static_cast<double>(s.store.id(s.trooper1)));
        give = {};
        CHECK(run(vm, "fb = Spawn_Unit(Find_Object_Type('Trooper_Team'), mk, p1)[1] fbn = fb.Get_Contained_Object_Count() "
                      "fb.Despawn()") == "");
        CHECK(calls == 2 && gnum(vm, "fbn") == 2.0);
        s.bridge.spawn_company = nullptr;
    }
    // Create_Generic_Object: name or type, one object.
    CHECK(run(vm, "g = Create_Generic_Object('Tank', Create_Position(1, 2, 3), p1) gx, gy, gz = g.Get_Position().Get_XYZ() "
                  "go = g.Get_Owner().Get_ID() gnil = tostring(Create_Generic_Object('Nope', t1, p1)) "
                  "g.Despawn() gv = g.Is_Valid() and 1 or 0") == "");
    CHECK(gnum(vm, "gx") == 1.0 && gnum(vm, "gy") == 2.0 && gnum(vm, "gz") == 3.0 && gnum(vm, "go") == 1.0 &&
          gstr(vm, "gnil") == "nil" && gnum(vm, "gv") == 0.0);
    CHECK(contains(run_stop(vm, "Create_Generic_Object('Tank', 5, p1)"), "Could not deduce a creation position from parameter 2."));
    CHECK(contains(run_stop(vm, "Create_Generic_Object('Tank', t1, 5)"), "Invalid type for parameter 3. Expected player."));
    CHECK(contains(run_stop(vm, "Create_Generic_Object('Tank', t1)"), "Invalid number of parameters. Expected 3, got 2."));
    // Hull / damage / invulnerability / cannot-be-killed / hardpoint.
    CHECK(run(vm, "h0 = tank.Get_Hull() tank.Take_Damage(100) h1 = tank.Get_Hull() tank.Make_Invulnerable(true) tank.Take_Damage(100) "
                  "h2 = tank.Get_Hull() tank.Make_Invulnerable(false) tank.Set_Cannot_Be_Killed(true) tank.Take_Damage(10000) "
                  "h3 = tank.Get_Health() v3 = tank.Is_Valid() and 1 or 0 tank.Take_Damage(15, 'HP_Gun') "
                  "gun = Find_First_Object('HP_Gun') gh = gun.Get_Hull() tank.Take_Damage(-100000) hmax = tank.Get_Hull()") == "");
    // q6 t12: Take_Damage is damage kind 0, which passes invulnerability (OBJECT_SCRIPTS §6.1 step 4): h2 drops too.
    CHECK(gnum(vm, "h0") == 1.0 && gnum(vm, "h1") == 0.75 && gnum(vm, "h2") == 0.5 && gnum(vm, "h3") == 1.0 / 400.0);
    CHECK(gnum(vm, "v3") == 1.0 && gnum(vm, "gh") == 0.5 && gnum(vm, "hmax") == 1.0);
    CHECK(run(vm, "tank.Set_Cannot_Be_Killed(false) tank.Take_Damage(1000) v4 = tank.Is_Valid() and 1 or 0") == "");
    CHECK(gnum(vm, "v4") == 0.0);
    {  // q6 t12: a host damage system takes Take_Damage(amount) (not the hardpoint form); refused -> the store path
        objects::ObjectHandle hit{};
        float amount = 0.0f;
        bool cannot = false, accept = true;
        int calls = 0;
        s.bridge.take_damage = [&](objects::ObjectHandle h, float a, bool cd) {
            ++calls;
            hit = h;
            amount = a;
            cannot = cd;
            return accept;
        };
        const float before = s.store.health(s.trooper2);
        CHECK(run(vm, "t2.Set_Cannot_Be_Killed(true) t2.Take_Damage(25)") == "");
        CHECK(calls == 1 && hit == s.trooper2 && amount == 25.0f && cannot && s.store.health(s.trooper2) == before);
        accept = false;
        CHECK(run(vm, "t2.Set_Cannot_Be_Killed(false) t2.Take_Damage(1)") == "");
        CHECK(calls == 2 && !cannot && s.store.health(s.trooper2) == before - 1.0f);
        s.store.set_health(s.trooper2, before);
        s.bridge.take_damage = nullptr;
    }
    CHECK(contains(run_stop(vm, "t1.Take_Damage('a')"), "Take_Damage -- invalid type for parameter 1. Expected number."));
    CHECK(contains(run_stop(vm, "t1.Make_Invulnerable(1)"), "Make_Invulnerable -- invalid type for parameter 1. Expected boolean."));
    CHECK(contains(run_stop(vm, "t1.Make_Invulnerable()"), "Make_Invulnerable -- invalid number of parameters. Expected 1, got 0."));
    // Script flags land in the bridge state.
    // (A suspended locomotor is not part of the movement lock, A1_READINGS §28.9; Can_Move is false for either.)
    CHECK(run(vm, "t1.Hide(true) t1.Set_Selectable(false) t1.Prevent_AI_Usage(true) t1.Prevent_All_Fire(true) "
                  "t1.Prevent_Opportunity_Fire(true) t1.Lock_Current_Orders() t1.Suspend_Locomotor(true) t1.Highlight(true, 25) t1.Highlight_Small(true) "
                  "t1.Set_In_Limbo(true) t1.Override_Max_Speed(0.5) t1.Set_Importance(3) t1.Set_Targeting_Priorities('Infantry_Attack_Move') "
                  "t1.Disable_Capture(true) t1.Set_Garrison_Spawn(true) t1.Set_Check_Contested_Space(true) "
                  "b = t1.Play_Animation('Cinematic', false, 2) pf = b.IsFinished() and 1 or 0 "
                  "sel = t1.Is_Selectable() and 1 or 0 mv = t1.Can_Move() and 1 or 0 Hide_Object(t2, 1)") == "");
    const ScriptObjectState& st = s.bridge.state(1);
    CHECK(st.hidden && st.selectable == 0 && st.prevent_ai_usage && st.prevent_all_fire && st.prevent_opportunity_fire);
    CHECK(st.locomotor_suspended && st.highlighted && st.highlight_offset == 25.0f && st.highlighted_small && st.in_limbo);
    CHECK(st.max_speed_override == 0.5f && st.importance == 3 && st.targeting_priorities == "Infantry_Attack_Move" && st.orders_locked);
    CHECK(st.capture_disabled && st.garrison_spawn && st.check_contested_space && st.animation == "Cinematic" &&
          !st.animation_loop && st.animation_index == 2);
    CHECK(s.bridge.state(2).hidden && gnum(vm, "pf") == 1.0 && gnum(vm, "sel") == 0.0 && gnum(vm, "mv") == 0.0);
    CHECK(run(vm, "t1.Override_Max_Speed(false) t1.Unlock_Current_Orders()") == "" && s.bridge.state(1).max_speed_override < 0.0f &&
          !s.bridge.state(1).orders_locked);
    CHECK(contains(run_stop(vm, "t1.Hide()"), "GameObjectWrapper::Hide -- Missing required boolean parameter."));
    CHECK(contains(run_stop(vm, "t1.Hide(1)"), "GameObjectWrapper::Hide -- Expected bool parameter."));
    CHECK(contains(run_stop(vm, "t1.Prevent_AI_Usage()"), "GameObjectWrapper::Prevent_AI_Usage -- Expected bool parameter."));
    CHECK(contains(run_stop(vm, "t1.Highlight()"), "GameObjectWrapper::Highlight -- invalid number of parameters. Expected 1 or 2."));
    CHECK(contains(run_stop(vm, "t1.Override_Max_Speed('x')"), "Override_Max_Speed -- Expected a number or boolean parameter."));
    CHECK(contains(run_stop(vm, "Hide_Object(t1)"), "LuaHideObject -- Expected gameobject pointer parameter 1, float parameter 2"));
    // Teleport / facing.
    CHECK(run(vm, "t2.Teleport(Create_Position(7, 8, 9)) a, b2, c = t2.Get_Position().Get_XYZ() "
                  "t1.Teleport_And_Face(t2) d = t1.Get_Distance(t2) t1.Face_Immediate(Create_Position(7, 18, 9))") == "");
    CHECK(gnum(vm, "a") == 7.0 && gnum(vm, "b2") == 8.0 && gnum(vm, "c") == 9.0 && gnum(vm, "d") == 0.0);
    CHECK(s.store.facing(s.trooper1).x == 0.0f && s.store.facing(s.trooper1).y == 1.0f);
    CHECK(contains(run_stop(vm, "t1.Teleport_And_Face(Create_Position(1, 1, 1))"), "Expected game object for parameter 1, got position instead."));
    CHECK(contains(run_stop(vm, "t1.Teleport()"), "GameObjectWrapper::Teleport -- invalid number of parameters. Expected 1, got 0."));
    {  // q6 t12 (L1_S4_REMAINING §3.4): with the tactical teleport hook, Teleport moves the unit there (facing kept)
        TacticalScriptHooks hk;
        objects::ObjectHandle moved{};
        Vec3 to{}, face{};
        hk.teleport = [&](objects::ObjectHandle h, Vec3 p, Vec3 f) {
            moved = h;
            to = p;
            face = f;
        };
        s.bridge.tactical = &hk;
        const Vec3 f2 = s.store.facing(s.trooper2);
        CHECK(run(vm, "t2.Teleport(Create_Position(1, 2, 3))") == "");
        CHECK(moved == s.trooper2 && to.x == 1.0f && to.y == 2.0f && to.z == 3.0f && face.x == f2.x && face.y == f2.y);
        CHECK(s.store.position(s.trooper2).x == 7.0f);  // the host moved it (here: nothing did)
        // q6 t19: Teleport_And_Face goes through the same hook, with the target's position and facing.
        moved = {};
        s.store.set_facing(s.trooper1, {0.0f, -1.0f, 0.0f});
        const Vec3 p1 = s.store.position(s.trooper1);
        CHECK(run(vm, "t2.Teleport_And_Face(t1)") == "");
        CHECK(moved == s.trooper2 && to.x == p1.x && to.y == p1.y && to.z == p1.z && face.x == 0.0f && face.y == -1.0f);
        CHECK(s.store.facing(s.trooper2).x == f2.x && s.store.facing(s.trooper2).y == f2.y);  // only the hook moves it
        CHECK(contains(run_stop(vm, "t2.Teleport_And_Face(t1, t1)"),
                       "GameObjectWrapper::Teleport_And_Face -- invalid number of parameters.  Expected 1, got 2."));
        s.bridge.tactical = nullptr;
    }
    CHECK(contains(run_stop(vm, "t1.Face_Immediate()"), "Face_Immediate -- Missing required position or gameobject parameter."));
    // Change_Owner goes through the host hook.
    int32_t new_owner = -5;
    objects::ObjectHandle changed{};
    s.bridge.change_owner = [&](objects::ObjectHandle h, int32_t o) {
        changed = h;
        new_owner = o;
        return true;
    };
    CHECK(run(vm, "t1.Change_Owner(p2)") == "" && changed == s.trooper1 && new_owner == 2);
    CHECK(contains(run_stop(vm, "t1.Change_Owner(5)"), "Change_Owner -- invalid type for parameter 1. Expected PlayerWrapper."));
    // Set_Garrison_Spawn notifies the host (handle + value) and keeps the stored flag; bad calls don't notify.
    // (The hook-unset path already ran above: Set_Garrison_Spawn(true) only stored the flag.)
    int gs_calls = 0;
    objects::ObjectHandle gs_obj{};
    bool gs_value = true;
    s.bridge.garrison_spawn_changed = [&](objects::ObjectHandle h, bool on) {
        ++gs_calls;
        gs_obj = h;
        gs_value = on;
    };
    CHECK(run(vm, "t1.Set_Garrison_Spawn(false)") == "" && gs_calls == 1 && gs_obj == s.trooper1 && !gs_value &&
          !s.bridge.state(1).garrison_spawn);
    CHECK(run(vm, "t2.Set_Garrison_Spawn(true)") == "" && gs_calls == 2 && gs_obj == s.trooper2 && gs_value &&
          s.bridge.state(2).garrison_spawn);
    CHECK(contains(run_stop(vm, "t1.Set_Garrison_Spawn(1)"), "Set_Garrison_Spawn -- invalid type for parameter 1. Expected boolean."));
    CHECK(contains(run_stop(vm, "t1.Set_Garrison_Spawn()"), "Set_Garrison_Spawn -- invalid number of parameters. Expected 1, got 0."));
    CHECK(gs_calls == 2);
    s.bridge.garrison_spawn_changed = nullptr;
    CHECK(run(vm, "t1.Set_Garrison_Spawn(true)") == "" && gs_calls == 2 && s.bridge.state(1).garrison_spawn);
    // Without a bridge: queries still work, setters are no-ops, nothing is issued.
    attach_object_bridge(vm, nullptr);
    CHECK(run(vm, "t2.Hide(false) nb = tostring(Find_Hint('Marker_Generic', 'spawnpoint')) bb = t2.Move_To(t1).IsFinished() and 1 or 0") == "");
    CHECK(gstr(vm, "nb") == "nil" && gnum(vm, "bb") == 1.0 && s.bridge.state(2).hidden);
}

static void test_dead_handles(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    CHECK(run(vm, "victim = t2") == "");
    s.bridge.state(2).hidden = true;
    CHECK(s.store.destroy(s.trooper2));
    // q6 t03: the unit order bindings handle a dead caller themselves (LUA_API.md "Unit order bindings"): Move_To is a
    // script error (no unwind, the exited flag), Attack_Target returns nothing.
    CHECK(run(vm, "r = 1 r = victim.Move_To(t1) rn = r == nil and 1 or 0") == "" &&
          last_message_has(vm, "SCRIPT ERROR: GameObjectWrapper::Move_To -- this object is already dead"));
    CHECK(gnum(vm, "rn") == 1.0 && vm.exit_requested());
    reset_script_threads(vm);
    CHECK(run(vm, "r = 1 r = victim.Attack_Target(t1) rn = r == nil and 1 or 0") == "" && gnum(vm, "rn") == 1.0 && !vm.exit_requested());
    // q6 t11: a dead caller is silent in most methods (nothing returned, no message, no stop); the WarnNothing ones log
    // "SCRIPT WARNING: GameObjectWrapper::<m> -- this object is dead!" and go on.
    for (const char* m : {"Get_Position()", "Hide(true)", "Get_Hull()", "Is_Category('Infantry')", "Activate_Ability('SPRINT', true)"}) {
        const size_t m0 = vm.messages().size();
        CHECK(run(vm, std::string("nr = (function(...) return arg.n end)(victim.") + m + ")") == "" && gnum(vm, "nr") == 0.0 &&
              vm.messages().size() == m0 && !vm.exit_requested());
    }
    for (const char* m : {"Despawn()", "Take_Damage(1)"}) {
        const std::string_view call(m);
        const std::string method(call.substr(0, call.find('(')));
        CHECK(run(vm, std::string("victim.") + m) == "" && !vm.exit_requested() &&
              last_message_has(vm, ("SCRIPT WARNING: GameObjectWrapper::" + method + " -- this object is dead!").c_str()));
    }
    CHECK(run(vm, "Find_Nearest(victim)") == "" &&
          last_message_has(vm, "Find_Nearest - parameter 1 is already dead; cannot extract a position from it."));
    reset_script_threads(vm);
    CHECK(contains(run_stop(vm, "t1.Get_Distance(victim)"), "could not extract a position from parameter 1."));
    CHECK(run(vm, "t1.Attack_Target(victim)") == "" && last_message_has(vm, "SCRIPT ERROR: GameObjectWrapper::Attack_Target -- Target is already dead."));
    CHECK(run(vm, "t1.Attack_Target(5)") == "" &&
          last_message_has(vm, "Attack_Target -- Parameter 1 is not a valid destination for unit Trooper"));
    reset_script_threads(vm);
    CHECK(contains(run_stop(vm, "t1.Teleport_And_Face(victim)"), "Teleport_And_Face -- Invalid game object given for parameter 1."));
    CHECK(contains(run_stop(vm, "Hide_Object(victim, 1)"), "LuaHideObject -- Invalid parameter 1, expected a game object"));
    // Dead objects are skipped by every finder; dead before the flush already.
    CHECK(run(vm, "ff = Find_First_Object('Trooper').Get_Object_ID() fn = Find_Nearest(tank, 'Trooper').Get_Object_ID()") == "");
    CHECK(gnum(vm, "ff") == 1.0 && gnum(vm, "fn") == 1.0);
    s.store.flush();
    s.bridge.prune(s.store);
    CHECK(s.bridge.find_state(2) == nullptr && s.bridge.find_state(4) != nullptr);
    CHECK(run(vm, "vv = victim.Is_Valid() and 1 or 0") == "" && gnum(vm, "vv") == 0.0);
}

#if ALAMO_LUA_HAVE_COMMANDS
struct Recorder {
    std::vector<commands::Command> cmds;
    std::vector<uint64_t> ticks;
};

static uint32_t u32_at(std::span<const uint8_t> d, size_t at) {
    return static_cast<uint32_t>(d[at]) | (static_cast<uint32_t>(d[at + 1]) << 8) | (static_cast<uint32_t>(d[at + 2]) << 16) |
           (static_cast<uint32_t>(d[at + 3]) << 24);
}

static void test_orders(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    commands::CommandPipeline pipe;
    Recorder rec;
    pipe.set_handler([&](uint64_t tick, const commands::Command& c) {
        rec.cmds.push_back(c);
        rec.ticks.push_back(tick);
        return commands::RejectReason::None;
    });
    s.bridge.pipeline = &pipe;
    vm.set_frame(7);
    using commands::EventType;
    CHECK(run(vm, "b = t1.Move_To(tank) fin = b.IsFinished() and 1 or 0 res = tostring(b.Result()) ao = t1.Has_Active_Orders() and 1 or 0") == "");
    CHECK(rec.cmds.size() == 1 && rec.ticks[0] == 7 && gnum(vm, "fin") == 1.0 && gstr(vm, "res") == "nil" && gnum(vm, "ao") == 1.0);
    CHECK(rec.cmds[0].payload.type() == EventType::MoveToObject && rec.cmds[0].payload.u("target") == 3 &&
          rec.cmds[0].payload.ids("movers") == std::vector<uint32_t>{1});
    CHECK(rec.cmds[0].header.player == 1 && rec.cmds[0].header.source == commands::CommandSource::Script);
    // Lua-only orders: PortExtension with the documented payload.
    CHECK(run(vm, "t2.Move_To(Create_Position(1.5, -2, 3))") == "");
    {
        const commands::EventData& e = rec.cmds.back().payload;
        CHECK(e.type() == EventType::PortExtension && e.u("subtype") == kLuaOrderSubtype + static_cast<uint32_t>(LuaOrder::MoveTo));
        const auto d = e.bytes_view("data");
        CHECK(d.size() == kLuaOrderPayloadBytes && u32_at(d, 0) == 2 && u32_at(d, 4) == static_cast<uint32_t>(objects::kNoObjectId));
        CHECK(std::bit_cast<float>(u32_at(d, 8)) == 1.5f && std::bit_cast<float>(u32_at(d, 12)) == -2.0f &&
              std::bit_cast<float>(u32_at(d, 16)) == 3.0f);
        CHECK(rec.cmds.back().header.player == 2);
    }
    CHECK(run(vm, "t1.Attack_Target(tank) hat = t1.Has_Attack_Target() and 1 or 0 gat = t1.Get_Attack_Target().Get_Object_ID()") == "");
    CHECK(rec.cmds.back().payload.type() == EventType::Attack && rec.cmds.back().payload.u("target") == 3 &&
          rec.cmds.back().payload.ids("attackers") == std::vector<uint32_t>{1});
    CHECK(gnum(vm, "hat") == 1.0 && gnum(vm, "gat") == 3.0);
    // S7 t36 (AI_TACTICAL §5.6): a current target keeps Has_Active_Orders true after the order flag has been cleared.
    s.bridge.state(1).has_orders = false;
    CHECK(run(vm, "ao3 = t1.Has_Active_Orders() and 1 or 0") == "" && gnum(vm, "ao3") == 1.0);
    CHECK(run(vm, "t1.Attack_Move(t2) t1.Guard_Target(Create_Position(1, 1, 0)) t1.Hyperspace_Away(true)") == "");
    CHECK(rec.cmds.size() == 6);
    CHECK(rec.cmds[3].payload.u("subtype") == kLuaOrderSubtype + static_cast<uint32_t>(LuaOrder::AttackMove) &&
          u32_at(rec.cmds[3].payload.bytes_view("data"), 4) == 2);
    CHECK(rec.cmds[4].payload.u("subtype") == kLuaOrderSubtype + static_cast<uint32_t>(LuaOrder::Guard));
    CHECK(rec.cmds[5].payload.u("subtype") == kLuaOrderSubtype + static_cast<uint32_t>(LuaOrder::HyperspaceAway) &&
          u32_at(rec.cmds[5].payload.bytes_view("data"), 20) == 1);
    CHECK(run(vm, "t1.Stop() ao2 = t1.Has_Active_Orders() and 1 or 0 hat2 = t1.Has_Attack_Target() and 1 or 0") == "");
    CHECK(rec.cmds.back().payload.u("subtype") == kLuaOrderSubtype + static_cast<uint32_t>(LuaOrder::Stop) &&
          gnum(vm, "ao2") == 0.0 && gnum(vm, "hat2") == 0.0);
    // Turn_To_Face: Facing event with the heading (atan2 convention) quantized.
    CHECK(run(vm, "t1.Turn_To_Face(Create_Position(0, 10, 0))") == "");
    CHECK(rec.cmds.back().payload.type() == EventType::Facing && rec.cmds.back().payload.ids("units") == std::vector<uint32_t>{1});
    CHECK(std::fabs(rec.cmds.back().payload.f("angle") - 90.0f) < 0.1f);
    CHECK(contains(run_stop(vm, "t1.Turn_To_Face()"), "Turn_To_Face -- invalid number of parameters. Expected 1, got 0."));
    // Abilities: SetUnitAbilityMode / SetAbilityAutofire on the direct path.
    CHECK(run(vm, "t1.Activate_Ability('SPRINT', true) act = t1.Is_Ability_Active('SPRINT') and 1 or 0 rdy = t1.Is_Ability_Ready('SPRINT') and 1 or 0") == "");
    {
        const commands::EventData& e = rec.cmds.back().payload;
        CHECK(e.type() == EventType::SetUnitAbilityMode && e.u("ability") == 40 && e.u("on") == 1 && e.u("count") == 1 &&
              e.ids("units") == std::vector<uint32_t>{1} && e.u("object") == commands::kNoObject);
    }
    CHECK(gnum(vm, "act") == 1.0 && gnum(vm, "rdy") == 0.0);
    CHECK(run(vm, "t1.Activate_Ability('STEALTH', true) t1.Activate_Ability('SPRINT', false) act2 = t1.Is_Ability_Active('SPRINT') and 1 or 0") == "");
    CHECK(rec.cmds[rec.cmds.size() - 2].payload.u("ability") == 48 && rec.cmds[rec.cmds.size() - 2].payload.u("object") == commands::kNoObject);
    CHECK(rec.cmds.back().payload.u("on") == 0 && gnum(vm, "act2") == 0.0);
    const size_t before = rec.cmds.size();
    // cu8l t04 (CU8K_T04_COORDINATOR_TRACES "Which check ends the script"): on the stage-2 path too, a count other than 2,
    // an unknown name and a wrong argument kind (STEALTH takes a bool) are script errors; nothing is issued.
    CHECK(contains(run_stop(vm, "t1.Activate_Ability('SPRINT')"), "invalid number of parameters.  Expected 2, got 1."));
    CHECK(contains(run_stop(vm, "t1.Activate_Ability('NOT_AN_ABILITY', true)"), "unknown ability NOT_AN_ABILITY."));
    CHECK(contains(run_stop(vm, "t1.Activate_Ability('STEALTH', tank)"), "Ability STEALTH expects a bool."));
    CHECK(rec.cmds.size() == before);
    CHECK(run(vm, "nb = t1.Activate_Ability('JET_PACK', tank) nf = nb.IsFinished() and 1 or 0") == "" && rec.cmds.size() == before);  // not owned: no-op
    CHECK(last_message_has(vm, "SCRIPT WARNING: GameObjectWrapper::Activate_Ability -- Object ") && !vm.exit_requested());
    CHECK(run(vm, "t1.Set_Single_Ability_Autofire('STEALTH', true) af = t1.Is_Ability_Autofire('STEALTH') and 1 or 0") == "");
    CHECK(rec.cmds.back().payload.type() == EventType::SetAbilityAutofire && rec.cmds.back().payload.u("ability") == 48 &&
          gnum(vm, "af") == 1.0);
    CHECK(contains(run_stop(vm, "t1.Set_Single_Ability_Autofire('STEALTH')"), "Expected 2, got 1."));
    // Garrison / Leave_Garrison / Sell.
    CHECK(run(vm, "t1.Garrison(tank) t1.Leave_Garrison() tank.Sell()") == "");
    const size_t n = rec.cmds.size();
    CHECK(rec.cmds[n - 3].payload.type() == EventType::Garrison && rec.cmds[n - 3].payload.u("garrison") == 3);
    CHECK(rec.cmds[n - 2].payload.u("garrison") == commands::kNoObject);
    CHECK(rec.cmds[n - 1].payload.type() == EventType::TacticalSell && rec.cmds[n - 1].payload.u("object") == 3 &&
          rec.cmds[n - 1].header.player == 2);
    // BlockingStatus waits on the host's completion hook.
    uint32_t asked_serial = 0;
    s.bridge.order_finished = [&](objects::ObjectId id, uint32_t serial) {
        asked_serial = serial;
        return id != 1;
    };
    CHECK(run(vm, "bb = t1.Move_To(t2) w1 = bb.IsFinished() and 1 or 0 w2 = t2.Move_To(t1).IsFinished() and 1 or 0") == "");
    CHECK(gnum(vm, "w1") == 0.0 && gnum(vm, "w2") == 1.0 && asked_serial == s.bridge.orders_issued);
    CHECK(s.bridge.orders_issued == rec.cmds.size());
}

// Galactic: FindPlanet, Get_All_Planets, Get_Planet_Location, Move_To(planet) = MoveObjectToObject.
static void test_galactic(const Db& db) {
    objects::IdCounter ids;
    objects::ObjectStore st(objects::ModeKind::Galactic, objects::StoreKind::Primary, db.types, &ids);
    const objects::ObjectHandle px = make(st, db.t("Planet_X"), 1, {0, 0, 0});
    make(st, db.t("Planet_Y"), 2, {50, 0, 0});
    objects::SpawnParams sp;
    sp.type = db.t("Fleet");
    sp.owner = 1;
    sp.container = px;
    const objects::ObjectHandle fleet = st.create(sp);
    sp.type = db.t("Hero_Ship");
    sp.container = fleet;
    st.create(sp);
    LuaHost host;
    host.store = &st;
    host.types = &db.types;
    LuaVm vm(host);
    ObjectBridge bridge;
    commands::CommandPipeline pipe;
    std::vector<commands::Command> cmds;
    pipe.set_handler([&](uint64_t, const commands::Command& c) {
        cmds.push_back(c);
        return commands::RejectReason::None;
    });
    bridge.pipeline = &pipe;
    attach_object_bridge(vm, &bridge);
    // refixf t03: FindPlanet is Find_First_Object (one class): no planet test, so a fleet type name finds the fleet.
    CHECK(run(vm, "px = FindPlanet('planet_x') py = FindPlanet('Planet_Y') pn = FindPlanet('Fleet').Get_Object_ID() "
                  "all = FindPlanet.Get_All_Planets() an = table.getn(all) a1 = all[1].Get_Object_ID() "
                  "ship = Find_First_Object('Hero_Ship') loc = ship.Get_Planet_Location().Get_Object_ID() "
                  "fl = ship.Get_Parent_Object() floc = fl.Get_Planet_Location().Get_Object_ID() "
                  "pl = tostring(px.Get_Planet_Location()) fl.Move_To(py) "
                  "sp = Spawn_Unit(Find_Object_Type('Tank'), py, py.Get_Owner())[1] spl = sp.Get_Planet_Location().Get_Object_ID()") == "");
    CHECK(gnum(vm, "pn") == 3.0 && gnum(vm, "an") == 2.0 && gnum(vm, "a1") == 2.0 && gnum(vm, "loc") == 1.0 &&
          gnum(vm, "floc") == 1.0 && gstr(vm, "pl") == "nil" && gnum(vm, "spl") == 2.0);
    CHECK(cmds.size() == 1 && cmds[0].payload.type() == commands::EventType::MoveObjectToObject &&
          cmds[0].payload.u("object") == 3 && cmds[0].payload.u("target") == 2);
    CHECK(run(vm, "FindPlanet(5)") == "" && last_message_has(vm, "FindPlanet -- Parameter 1 is not a valid string"));
    CHECK(run(vm, "FindPlanet()") == "" && last_message_has(vm, "FindPlanet -- No planet name set"));
    // Find_Nearest is tactical only.
    CHECK(run(vm, "gn = Find_Nearest(px) gn = tostring(gn)") == "" && gstr(vm, "gn") == "nil" &&
          last_message_has(vm, "Find_Nearest - this function may only be used in tactical modes."));
    // Mark_Parent_Mode_Object_For_Death on a tactical store whose parent mode is this one.
    objects::IdCounter tids;
    objects::ObjectStore tac(objects::ModeKind::Space, objects::StoreKind::Primary, db.types, &tids);
    objects::SpawnParams tp;
    tp.type = db.t("Hero_Ship");
    tp.owner = 1;
    tp.parent_mode_id = 4;  // the galactic Hero_Ship
    tac.create(tp);
    LuaHost thost;
    thost.store = &tac;
    thost.types = &db.types;
    LuaVm tvm(thost);
    ObjectBridge tb;
    tb.parent_store = &st;
    attach_object_bridge(tvm, &tb);
    CHECK(run(tvm, "Find_First_Object('Hero_Ship').Mark_Parent_Mode_Object_For_Death()") == "");
    CHECK((st.flags(st.find(4)) & objects::kFlagMarkedByChildMode) != 0);
}

// Same script, two fresh scenes: identical command streams and query results.
static std::string scripted_run(const Db& db, std::vector<uint8_t>& stream) {
    Scene s(db);
    commands::CommandPipeline pipe;
    pipe.set_handler([&](uint64_t, const commands::Command& c) {
        commands::encode_command(c, stream);
        return commands::RejectReason::None;
    });
    s.bridge.pipeline = &pipe;
    const std::string code =
        "out = '' for i = 1, 20 do local u = Create_Generic_Object('Trooper', Create_Position(i * 3, 0, 0), p1) "
        "u.Move_To(Find_Nearest(u, p1, false)) out = out .. u.Get_Object_ID() .. ':' .. Find_Nearest(u, 'Trooper').Get_Object_ID() .. ',' end "
        "for _, o in ipairs(Find_All_Objects_Of_Type('Trooper')) do o.Attack_Move(tank) end";
    const std::string err = run(*s.vm, code);
    CHECK(err.empty());
    return gstr(*s.vm, "out");
}
static void test_determinism(const Db& db) {
    std::vector<uint8_t> a, b;
    const std::string ra = scripted_run(db, a);
    const std::string rb = scripted_run(db, b);
    CHECK(ra == rb && a == b && !a.empty());
}
#endif

// The shipped scripts' patterns: TestValid / Find_Hint / Spawn_Unit / Teleport_And_Face / BlockOnCommand in a
// thread. With ALAMO_FOC_DATA the real PGBase.lua drives BlockOnCommand; otherwise a minimal stand-in.
using ::alamo::test::env_or_empty;

static void test_script_patterns(const Db& db) {
    Scene s(db);
    std::unique_ptr<FileProvider> files;
    const std::string root = env_or_empty("ALAMO_FOC_DATA");
#ifdef ALAMO_LUA_HAVE_VFS
    if (!root.empty()) {
        vfs::VfsConfig cfg;
        cfg.foc_root = root;
        cfg.eaw_root = "";
        std::string verr;
        if (auto v = vfs::Vfs::create(cfg, &verr)) files = std::move(v);
    }
#endif
    if (!files && !root.empty()) files = std::make_unique<DirectoryProvider>(root);
    LuaHost host = s.host;
    host.files = files.get();
    LuaVm vm(host);
    attach_object_bridge(vm, &s.bridge);
    std::string err;
    bool pgbase = false;
    if (files) pgbase = vm.require("PGBase", &err);
    if (!pgbase) {
        if (!root.empty()) std::printf("  PGBase not loadable (%s): stand-in used\n", err.c_str());
        CHECK(run(vm, "function TestValid(w) if w == nil then return false end if w.Is_Valid == nil then return false end return w.Is_Valid() end "
                      "function Sleep(t) local s = GetCurrentTime() while GetCurrentTime() - s < t do coroutine.yield() end end "
                      "function BlockOnCommand(b) if not b then return nil end repeat coroutine.yield() until b.IsFinished() == true return b.Result() end") == "");
    } else {
        std::printf("  PGBase.lua loaded: BlockOnCommand / Sleep / TestValid from the shipped library\n");
    }
    CHECK(run(vm, "function Story_Thread() "
                  "  local flag = Find_Hint('MARKER_GENERIC', 'spawnpoint') "
                  "  if not TestValid(flag) then done = -1 return end "
                  "  local list = Spawn_Unit(Find_Object_Type('Tank'), flag, Find_First_Object('Trooper').Get_Owner()) "
                  "  local unit = list[1] "
                  "  unit.Teleport_And_Face(flag) unit.Prevent_AI_Usage(true) unit.Hide(false) "
                  "  BlockOnCommand(unit.Move_To(Find_Nearest(unit, 'Trooper'))) "
                  "  Sleep(0.1) "
                  "  unit.Take_Damage(10000) "
                  "  done = TestValid(unit) and 2 or 1 "
                  "end "
                  "Create_Thread('Story_Thread')") == "");
    for (int f = 0; f <= 10; ++f) {
        vm.set_time(f / 30.0);
        vm.set_frame(static_cast<uint32_t>(f));
        vm.service_threads();
    }
    CHECK(gnum(vm, "done") == 1.0 && vm.errors().empty());
    if (!vm.errors().empty()) std::printf("  script pattern error: %s\n", vm.errors()[0].c_str());
}

// L1/S4 wave 1 on a tactical store: Change_Owner without a hook (ObjectStore::set_owner), garrison queries
// on a GARRISON_STRUCTURE holder, Eject_Garrison orders, BlockForever, containment without hooks.
static void test_wave1(const Db& db) {
    Scene s(db);
    LuaVm& vm = *s.vm;
    CHECK(run(vm, "t1.Change_Owner(p2) o = t1.Get_Owner().Get_ID()") == "" && s.store.owner(s.trooper1) == 2 &&
          gnum(vm, "o") == 2.0 && s.store.owned_by(2).back() == s.trooper1);
    const objects::ObjectHandle bunker = make(s.store, db.t("Bunker"), 1, {50, 0, 0});
    objects::SpawnParams sp;
    sp.type = db.t("Trooper");
    sp.owner = 1;
    sp.container = bunker;
    const objects::ObjectHandle inside = s.store.create(sp);
    sp.type = db.t("Tank");
    const objects::ObjectHandle tank_in = s.store.create(sp);  // a tank inside too (Can_Garrison is a category test)
    vm.set_object_global("Bunker", bunker);
    vm.set_object_global("Inside", inside);
    CHECK(run(vm, "g1 = Inside.Is_In_Garrison() g2 = t2.Is_In_Garrison() h1 = Bunker.Has_Garrison() h2 = t2.Has_Garrison() "
                  "gl = table.getn(Bunker.Get_Garrisoned_Units()) gf = Bunker.Get_Garrisoned_Units()[1] == Inside "
                  "cg1 = t2.Can_Garrison(Bunker) cg2 = tank.Can_Garrison(Bunker) cg3 = Inside.Can_Garrison(t2) "
                  "pc = Inside.Get_Parent_Object() == Bunker n = Bunker.Get_Contained_Object_Count() "
                  "bf = BlockForever().IsFinished()") == "");
    // t2 (player 2) may garrison only an empty or own bunker: the bunker (player 1) holds units -> false.
    CHECK(gtrue(vm, "g1") && !gtrue(vm, "g2") && gtrue(vm, "h1") && !gtrue(vm, "h2") && gnum(vm, "gl") == 2.0 && gtrue(vm, "gf"));
    CHECK(!gtrue(vm, "cg1") && !gtrue(vm, "cg2") && !gtrue(vm, "cg3") && gtrue(vm, "pc") && gnum(vm, "n") == 2.0 && !gtrue(vm, "bf"));
    CHECK(run(vm, "t2.Change_Owner(Bunker.Get_Owner()) cg4 = t2.Can_Garrison(Bunker)") == "" && gtrue(vm, "cg4"));
    const uint32_t before = s.bridge.orders_issued;
    CHECK(run(vm, "Bunker.Eject_Garrison() t2.Eject_Garrison()") == "" && s.bridge.orders_issued == before + 2);
    CHECK(s.bridge.state(s.store.id(inside)).has_orders && s.bridge.state(s.store.id(tank_in)).has_orders);
    CHECK(run(vm, "Bunker.Destroy_Contained_Objects(0.5)") == "" && !s.store.alive(inside) && s.store.alive(tank_in));
}

// q6 t11 (docs/re/LUA_API.md "Binding error model across all bindings").
static size_t messages_with(LuaVm& vm, const char* text) {
    size_t n = 0;
    for (const std::string& m : vm.messages()) n += m.find(text) != std::string::npos ? 1u : 0u;
    return n;
}

static void test_binding_error_model(const Db& db) {
    {
        Scene s(db);
        LuaVm& vm = *s.vm;
        // A binding error is not a Lua error: pcall reports success, the call returns no value, the code runs on.
        CHECK(run(vm, "ok, e = pcall(function() return t1.Is_Category() end) okv = ok and 1 or 0 "
                      "ne = (function(...) return arg.n end)(t1.Is_Category()) after = 1") == "");
        CHECK(gnum(vm, "okv") == 1.0 && gstr(vm, "e") == "<nil>" && gnum(vm, "ne") == 0.0 && gnum(vm, "after") == 1.0);
        CHECK(vm.exit_requested() &&
              last_message_has(vm, "SCRIPT ERROR: GameObjectWrapper::Is_Category -- invalid number of parameters. Expected 1, got 0."));
        reset_script_threads(vm);
        // The error ends the script at the end of the pass: the erring thread runs to its yield, no other thread is resumed
        // and every thread is gone afterwards.
        CHECK(run(vm, "function A() t1.Has_Property() a_after = 1 coroutine.yield(true) a_never = 1 end "
                      "function B() b_ran = 1 end Create_Thread('A') Create_Thread('B')") == "");
        vm.service_threads();
        CHECK(gnum(vm, "a_after") == 1.0 && gstr(vm, "a_never") == "<nil>" && gstr(vm, "b_ran") == "<nil>");
        CHECK(vm.exit_requested() && vm.thread_count() == 0 && vm.errors().empty());
        vm.service_threads();  // nothing left to resume, the flag stays
        CHECK(vm.exit_requested() && gstr(vm, "a_never") == "<nil>");
        reset_script_threads(vm);

        // nil arguments count: Is_Category('Infantry', nil) has two arguments.
        CHECK(run(vm, "r = t1.Is_Category('Infantry', nil)") == "" && vm.exit_requested() &&
              last_message_has(vm, "Is_Category -- invalid number of parameters. Expected 1, got 2."));
        reset_script_threads(vm);
        // Argument counts from the code, not from the stale messages.
        CHECK(run(vm, "FindTarget(t1, 'P', 'A', 1, 2, 3)") == "" && vm.exit_requested() &&
              last_message_has(vm, "FindTarget -- invalid number of parameters"));
        reset_script_threads(vm);
        CHECK(run(vm, "GiveDesireBonus(nil, 'G', t1, 1)") == "" && vm.exit_requested() &&
              last_message_has(vm, "Give_Desire_Bonus : invalid number of parameters. Expected 4, got 4."));
        reset_script_threads(vm);
        CHECK(run(vm, "EvaluatePerception('Is_Campaign')") == "" && vm.exit_requested() &&
              last_message_has(vm, "Evaluate_Perception: Invalid number of parameters. Expected 2 or 3, got 1."));
        reset_script_threads(vm);
        const size_t before = vm.messages().size();
        CHECK(run(vm, "Set_Cinematic_Target_Key(t1, 0, 0, 0, 0, 0, 0, 0, 0)") == "" && !vm.exit_requested() &&
              vm.messages().size() == before);  // 9 arguments: accepted
        CHECK(run(vm, "Set_Cinematic_Target_Key(t1, 0, 0)") == "" && vm.exit_requested());
        reset_script_threads(vm);

        // Warnings log and continue (no flag).
        CHECK(run(vm, "Point_Camera_At(5) w1 = 1") == "" && !vm.exit_requested() && gnum(vm, "w1") == 1.0 &&
              last_message_has(vm, "SCRIPT WARNING: LuaPointCamera -- Expected position parameter 1"));

        // Dead wrappers: silent by default, Is_Valid false, the listed errors and warnings.
        CHECK(run(vm, "victim = t2") == "");
        CHECK(s.store.destroy(s.trooper2));
        const size_t m0 = vm.messages().size();
        CHECK(run(vm, "np = (function(...) return arg.n end)(victim.Get_Position()) nh = (function(...) return arg.n end)(victim.Get_Hull()) "
                      "nc = (function(...) return arg.n end)(victim.Is_Category('Infantry')) vv = tostring(victim.Is_Valid())") == "");
        CHECK(gnum(vm, "np") == 0.0 && gnum(vm, "nh") == 0.0 && gnum(vm, "nc") == 0.0 && !gtrue(vm, "vv") &&
              gstr(vm, "vv") == "false");
        CHECK(!vm.exit_requested() && vm.messages().size() == m0);
        CHECK(run(vm, "nd = (function(...) return arg.n end)(victim.Despawn()) td = victim.Get_Time_Till_Dead()") == "" && !vm.exit_requested());
        CHECK(gnum(vm, "nd") == 0.0 && gnum(vm, "td") == 0.0 &&
              messages_with(vm, "SCRIPT WARNING: GameObjectWrapper::Despawn -- this object is dead!") == 1 &&
              last_message_has(vm, "SCRIPT WARNING: GameObjectWrapper::Get_Time_Till_Dead -- this object is dead!"));
        CHECK(run(vm, "dd = victim.Get_Distance(t1)") == "" && vm.exit_requested() &&
              last_message_has(vm, "SCRIPT ERROR: GameObjectWrapper::Get_Distance -- this object is already dead"));
        reset_script_threads(vm);
    }
    {
        // The mode gate comes before the dead-wrapper check: a dead object's Get_Hull in a galactic mode is an error.
        Scene s(db, objects::ModeKind::Galactic);
        LuaVm& vm = *s.vm;
        CHECK(run(vm, "victim = t2") == "");
        CHECK(s.store.destroy(s.trooper2));
        CHECK(run(vm, "h = victim.Get_Hull() after = 1") == "" && gnum(vm, "after") == 1.0 && vm.exit_requested() &&
              last_message_has(vm, "SCRIPT ERROR: GameObjectWrapper::Get_Hull -- this command is only supported in tactical modes"));
        reset_script_threads(vm);
        CHECK(run(vm, "Zoom_Camera(1, 2)") == "" && vm.exit_requested() &&
              last_message_has(vm, "SCRIPT ERROR: Zoom_Camera -- this command is only supported in tactical modes"));
        reset_script_threads(vm);
    }
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    log::set_min_level(log::Level::Warn);
    Db db;
    CHECK(db.ok);
    if (!db.ok) return 1;
    test_registry_rows();
    test_find(db);
    test_find_filters(db);
    test_queries(db);
    test_stage3(db);
    test_spawn_and_state(db);
    test_spawn_unit_facing(db);
    test_other_spawn_facing(db);
    test_dead_handles(db);
    test_binding_error_model(db);
    test_wave1(db);
#if ALAMO_LUA_HAVE_COMMANDS
    test_orders(db);
    test_galactic(db);
    test_determinism(db);
#else
    std::printf("  engine/commands not configured: order tests skipped\n");
#endif
    test_script_patterns(db);
    const BindingCoverage cov = binding_coverage();
    std::printf("  coverage: globals %u/%u, methods %u/%u, call sites %llu/%llu\n", cov.globals_implemented, cov.globals,
                cov.methods_implemented, cov.methods, static_cast<unsigned long long>(cov.calls_implemented),
                static_cast<unsigned long long>(cov.calls_total));
    if (g_fail == 0) std::printf("alamo_lua_objects_tests: all passed\n");
    return g_fail == 0 ? 0 : 1;
}

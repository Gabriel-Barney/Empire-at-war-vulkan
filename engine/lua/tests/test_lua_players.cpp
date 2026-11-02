// L1 stage 2, players family: Find_Player / PlayerWrapper / Suspend_AI / perception / type lists /
// distributions / goal requests through Lua snippets on a synthetic S3 galaxy (alamo_galactic_sim),
// EvaluatePerception on the shipped perceptual equations (ALAMO_FOC_DATA, skipped when unset), and
// determinism (identical digests across runs and threads).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <span>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "alamo/lua/bindings.hpp"
#include "alamo/lua/lua_vm.hpp"

#if defined(ALAMO_LUA_PLAYERS)
#include "alamo/ai/ai_path_finder.hpp"
#include "alamo/ai/galactic_perception.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/commands/pipeline.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/galactic/special_systems.hpp"
#include "alamo/lua/ai_bridge.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/players.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/objects/object_types.hpp"
#include "vm_internal.hpp"  // family table (engine/lua/src is on this test's include path)
#endif

using namespace alamo;
using namespace alamo::lua;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

#if defined(ALAMO_LUA_PLAYERS)

namespace fs = std::filesystem;

namespace {

std::string run(LuaVm& vm, const std::string& code) {
    std::string err;
    if (!vm.run(code, "test", &err)) return err.empty() ? "error" : err;
    return {};
}
std::string gstr(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const char* s = lua_type(L, -1) == LUA_TNIL ? nullptr : lua_tostring(L, -1);
    std::string out = s ? s : "<nil>";
    lua_pop(L, 1);
    return out;
}
double gnum(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const double d = lua_tonumber(L, -1);
    lua_pop(L, 1);
    return d;
}
bool gnil(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool n = lua_isnil(L, -1);
    lua_pop(L, 1);
    return n;
}
bool gtrue(LuaVm& vm, const char* name) {
    lua_State* L = vm.state();
    lua_pushstring(L, name);
    lua_rawget(L, LUA_GLOBALSINDEX);
    const bool b = lua_type(L, -1) == LUA_TBOOLEAN && lua_toboolean(L, -1) != 0;
    lua_pop(L, 1);
    return b;
}
bool same_bits(double a, double b) { return std::memcmp(&a, &b, sizeof a) == 0; }

class MemProvider final : public FileProvider {
public:
    void add(std::string_view path, std::string text) { files_[normalize_path(path)] = std::move(text); }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        out.clear();
        const auto it = files_.find(normalize_path(path));
        if (it == files_.end()) return false;
        out.assign(it->second.begin(), it->second.end());
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "mem:" + std::string(path) : ""; }

private:
    std::map<std::string, std::string> files_;
};

// Resolves every token path (deterministically) except paths containing a token named MISSING.
class TestResolver final : public ai::TokenResolver {
public:
    bool resolve(const ai::EvalContext& ctx, const ai::TokenRef& t, double& out) const override {
        uint64_t h = 1469598103934665603ull ^ static_cast<uint64_t>(t.root);
        for (const ai::TokenId id : t.path) {
            const std::string_view n = t.tokens->name(id);
            if (n == "MISSING") return false;
            for (const char ch : n) {
                h ^= static_cast<uint8_t>(ch >= 'a' && ch <= 'z' ? ch - 'a' + 'A' : ch);
                h *= 1099511628211ull;
            }
            h ^= '.';
            h *= 1099511628211ull;
        }
        out = static_cast<double>(h % 1000u) / 10.0 + ctx.self * 1000.0 + (ctx.target + 1) * 7.0;
        return true;
    }
    bool implements(ai::Root, std::span<const ai::TokenId>, const ai::TokenTable&) const override { return true; }
};

// Synthetic galaxy: Empire (human) / Underworld (AI, Hard_Default) / Neutral over planets A - B - C.
struct Galaxy {
    MemProvider files;
    objects::ObjectTypeDatabase raw;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    objects::IdCounter ids;
    std::unique_ptr<objects::ObjectStore> store;
    SimRng sim_rng{0x5eedu};
    std::unique_ptr<galactic::GalacticSim> sim;
    GameRandom rng{4242u, 99u};
    LuaPlayerWorld world;

    // `extra` starting forces (by type name, appended after the default ones; L1/S4 wave 1 tests).
    // `tune` edits the rules before the sim is built (P4 lua: the special-systems tables).
    explicit Galaxy(std::vector<std::tuple<int32_t, PlanetIndex, const char*>> extra = {},
                    const std::function<void(galactic::GalacticRules&, const objects::ObjectTypeDatabase&)>& tune = {}) {
        files.add("Data\\XML\\GameObjectFiles.xml", "<L><File>Units.xml</File></L>");
        files.add("Data\\XML\\Units.xml",
                  "<R>\n"
                  "<Planet Name=\"Planet_A\"><Planet_Credit_Value>10</Planet_Credit_Value></Planet>\n"
                  "<Planet Name=\"Planet_B\"><Planet_Credit_Value>20</Planet_Credit_Value></Planet>\n"
                  "<Planet Name=\"Planet_C\"><Planet_Credit_Value>5</Planet_Credit_Value></Planet>\n"
                  "<SpaceUnit Name=\"Frigate_T\"><CategoryMask>Frigate</CategoryMask>"
                  "<AI_Combat_Power>500</AI_Combat_Power><Hyperspace>Yes</Hyperspace>"
                  "<Hyperspace_Speed>1.0</Hyperspace_Speed></SpaceUnit>\n"
                  "<GroundInfantry Name=\"Trooper_T\"><CategoryMask>Infantry</CategoryMask>"
                  "<AI_Combat_Power>50</AI_Combat_Power><Affiliation>Rebel, Empire</Affiliation></GroundInfantry>\n"
                  "<SpaceUnit Name=\"Hero_Ship_T\"><CategoryMask>Frigate</CategoryMask><Is_Named_Hero>Yes</Is_Named_Hero>"
                  "<Hyperspace>Yes</Hyperspace><Hyperspace_Speed>1.0</Hyperspace_Speed></SpaceUnit>\n"
                  // P4 lua: a galactic saboteur hero and a unit with a Black Market gated unit ability.
                  "<GroundInfantry Name=\"Halter_T\"><CategoryMask>Infantry</CategoryMask></GroundInfantry>\n"
                  "<GroundInfantry Name=\"Stealth_Trooper_T\"><CategoryMask>Infantry</CategoryMask>"
                  "<Unit_Abilities_Data SubObjectList=\"Yes\"><Unit_Ability><Type>STEALTH</Type>"
                  "<Must_Be_Bought_On_Black_Market> Yes </Must_Be_Bought_On_Black_Market>"
                  "<GUI_Activated_Ability_Name>Stealth_BM</GUI_Activated_Ability_Name></Unit_Ability>"
                  "<Unit_Ability><Type>SPRINT</Type></Unit_Ability></Unit_Abilities_Data></GroundInfantry>\n"
                  // OI lua: two slicer-unlockable Empire types (Give_Random_Sliceable_Tech -> GalacticSim::unlock_type /
                  // raise_tech_level).
                  "<GroundInfantry Name=\"Slice_A_T\"><Affiliation>Empire</Affiliation><Tech_Level>1</Tech_Level>"
                  "<Build_Initially_Locked>Yes</Build_Initially_Locked><Build_Can_Be_Unlocked_By_Slicer>Yes</Build_Can_Be_Unlocked_By_Slicer>"
                  "<Slice_Cost_Credits>100</Slice_Cost_Credits></GroundInfantry>\n"
                  "<GroundInfantry Name=\"Slice_B_T\"><Affiliation>Empire</Affiliation><Tech_Level>1</Tech_Level>"
                  "<Build_Initially_Locked>Yes</Build_Initially_Locked><Build_Can_Be_Unlocked_By_Slicer>Yes</Build_Can_Be_Unlocked_By_Slicer>"
                  "<Slice_Cost_Credits>200</Slice_Cost_Credits></GroundInfantry>\n"
                  // refix t04: a galactic company (one port object) whose Company_Units the member views stand for.
                  "<GroundInfantry Name=\"Smuggler_T\"><CategoryMask>Infantry</CategoryMask></GroundInfantry>\n"
                  "<GroundInfantry Name=\"Crew_T\"><CategoryMask>Infantry</CategoryMask></GroundInfantry>\n"
                  "<HeroCompany Name=\"Smuggler_Team_T\"><Company_Units>Smuggler_T, Crew_T, Smuggler_T</Company_Units>"
                  "<Company_Transport_Unit>Frigate_T</Company_Transport_Unit></HeroCompany>\n"
                  "</R>");
        CHECK(raw.load(files));
        CHECK(types.build(raw));
        galactic::FactionRules emp, und, neu;
        emp.name = "Empire";
        emp.can_control_planets = emp.is_playable = true;
        und.name = "Underworld";
        und.can_control_planets = und.is_playable = und.benefits_from_corruption = true;
        neu.name = "Neutral";
        neu.is_neutral = true;
        rules.factions = {emp, und, neu};
        rules.difficulties = {{"Easy_Default", 0.5f, 1.0f}, {"Hard_Default", 1.2f, 1.0f}};
        rules.types = galactic::extract_type_info(types, rules.factions);
        if (tune) tune(rules, raw);
        store =std::make_unique<objects::ObjectStore>(objects::ModeKind::Galactic, objects::StoreKind::Primary, types,
                                                       &ids);
        sim = std::make_unique<galactic::GalacticSim>(rules, *store, sim_rng);

        galactic::GalacticSetup s;
        s.map.campaign_name = "Players";
        const char* names[] = {"A", "B", "C"};
        for (uint32_t i = 0; i < 3; ++i) {
            s.map.planets.add(names[i]);
            s.map.planets.position[i] = Vec3{60.0f * static_cast<float>(i), 0.0f, 0.0f};
            s.map.planets.flags[i] = kPlanetHasPlanetBehavior;
        }
        for (const auto& [rn, ra, rb] : {std::tuple{"A_B", 0u, 1u}, std::tuple{"B_C", 1u, 2u}}) {
            const uint32_t r = s.map.routes.add(rn);
            s.map.routes.point_a[r] = ra;
            s.map.routes.point_b[r] = rb;
            s.map.routes.hs_speed_factor[r] = 5.0f;
            s.map.routes.credit_gain_factor[r] = 0.0f;
        }
        s.planet_type = {raw.find("Planet_A"), raw.find("Planet_B"), raw.find("Planet_C")};
        galactic::PlayerSetup e, u, n;
        e.faction = 0;
        e.human = true;
        e.credits = 5000.0f;
        u.faction = 1;
        u.ai = true;
        u.difficulty = 1;
        u.credits = 1000.0f;
        u.tech_level = 2;
        n.faction = 2;
        s.players = {e, u, n};
        s.forces = {{0, 0, raw.find("Frigate_T")}, {0, 0, raw.find("Trooper_T")}, {1, 2, raw.find("Trooper_T")}};
        for (const auto& [pl, row, ty] : extra) s.forces.push_back({pl, row, raw.find(ty)});
        std::vector<std::string> diag;
        CHECK(sim->start(s, &diag));
        for (const std::string& d : diag) std::printf("  setup: %s\n", d.c_str());
        world.attach_galactic(sim.get());
    }

    LuaHost host() {
        LuaHost h;
        h.store = store.get();
        h.types = &types;
        h.rng = &rng;
        return h;
    }
    objects::ObjectHandle planet(uint32_t row) const { return sim->planets()[row].object; }

    // GalacticSnapshot of the sim (orbit fleets' members = space, surface = ground; no fog).
    ai::GalacticSnapshot snapshot() const {
        ai::GalacticSnapshot snap;
        snap.planet_count = static_cast<uint32_t>(sim->planets().size());
        for (size_t p = 0; p < sim->players().size(); ++p) {
            const galactic::PlayerState& ps = sim->players()[p];
            ai::PerceptionPlayer pp;
            pp.neutral = rules.factions[ps.faction].is_neutral;
            pp.is_human = ps.human;
            pp.has_ai_system = ps.ai;
            pp.benefits_from_corruption = rules.factions[ps.faction].benefits_from_corruption;
            snap.players.push_back(pp);
        }
        for (const galactic::PlanetState& ps : sim->planets()) {
            snap.owner.push_back(ps.owner);
            snap.unit_begin.push_back(static_cast<uint32_t>(snap.units.size()));
            auto add = [&](objects::ObjectHandle h, ai::UnitLayer layer) {
                if (!store->alive(h)) return;
                ai::UnitPresence u;
                u.owner = store->owner(h);
                u.combat_power = rules.types[store->type(h)].ai_combat_power;
                u.layer = layer;
                snap.units.push_back(u);
            };
            for (const uint32_t f : ps.orbit)
                for (const objects::ObjectHandle m : sim->fleets()[f].contents) add(m, ai::UnitLayer::Space);
            for (const objects::ObjectHandle m : ps.surface) add(m, ai::UnitLayer::Ground);
        }
        snap.unit_begin.push_back(static_cast<uint32_t>(snap.units.size()));
        return snap;
    }
};

// ------------------------------------------------------------------------------------------

void test_players() {
    Galaxy g;
    LuaVm vm(g.host());
    // Without a world: Find_Player gives nil, player methods fail.
    CHECK(run(vm, "np = Find_Player('Empire')") == "" && gnil(vm, "np"));
    attach_player_world(vm, &g.world);
    CHECK(player_world(vm) == &g.world);
    g.world.set_local_player(0);

    CHECK(run(vm, "emp = Find_Player('empire'); und = Find_Player('Underworld'); neu = Find_Player('NEUTRAL'); "
                  "loc = Find_Player('local'); none = Find_Player('Imperial'); "
                  "e_id = emp.Get_ID(); u_id = und.Get_ID(); same = (loc == emp); diff = (emp ~= und)") == "");
    CHECK(gnum(vm, "e_id") == 0.0 && gnum(vm, "u_id") == 1.0 && gnil(vm, "none") && gtrue(vm, "same") &&
          gtrue(vm, "diff"));
    // Wrapper identity: one userdata per player and VM (tables keyed by players hit).
    CHECK(run(vm, "pk = {} pk[emp] = 'e' pk[und] = 'u' i1 = pk[Find_Player('EMPIRE')] i2 = pk[emp.Get_Enemy()] "
                  "i3 = rawequal(loc, emp) and 'same' or 'different'") == "");
    CHECK(gstr(vm, "i1") == "e" && gstr(vm, "i2") == "u" && gstr(vm, "i3") == "same");
    CHECK(run(vm, "Find_Player()").find("Find_Player -- invalid number of parameters. Expected 1, got 0.") !=
          std::string::npos);
    CHECK(run(vm, "Find_Player(5)").find("Expected string") != std::string::npos);

    // Facts.
    CHECK(run(vm, "fn = emp.Get_Faction_Name(); nm = emp.Get_Name(); h1 = emp.Is_Human(); h2 = und.Is_Human(); "
                  "n1 = neu.Is_Neutral(); n2 = emp.Is_Neutral(); d1 = emp.Get_Difficulty(); d2 = und.Get_Difficulty(); "
                  "t1 = und.Get_Tech_Level(); tm = und.Get_Team(); cl = emp.Get_Clan_ID()") == "");
    CHECK(gstr(vm, "fn") == "EMPIRE" && gstr(vm, "nm") == "Empire" && gtrue(vm, "h1") && !gtrue(vm, "h2"));
    CHECK(gtrue(vm, "n1") && !gtrue(vm, "n2") && gstr(vm, "d1") == "Normal" && gstr(vm, "d2") == "Hard");
    CHECK(gnum(vm, "t1") == 2.0 && gnum(vm, "tm") == 1.0 && gnum(vm, "cl") == 0.0);

    // Credits: Add_Credits semantics (AI Credit_Multiplier 1.2 on gains, no debt), float32.
    CHECK(run(vm, "c0 = emp.Get_Credits(); emp.Give_Money(250); c1 = emp.Get_Credits(); und.Give_Money(100); "
                  "c2 = und.Get_Credits(); emp.Give_Money(-99999); c3 = emp.Get_Credits()") == "");
    CHECK(gnum(vm, "c0") == 5000.0 && gnum(vm, "c1") == 5250.0 && gnum(vm, "c3") == 0.0);
    CHECK(gnum(vm, "c2") == static_cast<double>(1000.0f + 100.0f * 1.2f));
    CHECK(g.sim->credits(1) == 1000.0f + 100.0f * 1.2f && g.sim->credits(0) == 0.0f);
    CHECK(run(vm, "emp.Give_Money()").find("PlayerWrapper::Lua_Give_Money -- Missing required parameter.") !=
          std::string::npos);
    CHECK(run(vm, "emp.Give_Money('x')").find("Parameter 1 must be a number.") != std::string::npos);
    CHECK(run(vm, "emp.Set_Tech_Level(3); t2 = emp.Get_Tech_Level()") == "" && gnum(vm, "t2") == 3.0);
    CHECK(g.sim->players()[0].tech_level == 3);

    // Diplomacy.
    CHECK(run(vm, "a1 = emp.Is_Enemy(und); a2 = emp.Is_Enemy(neu); a3 = emp.Is_Ally(emp); a4 = emp.Is_Ally(und); "
                  "ge = emp.Get_Enemy(); ge_id = ge.Get_ID(); ne = neu.Get_Enemy()") == "");
    CHECK(gtrue(vm, "a1") && !gtrue(vm, "a2") && gtrue(vm, "a3") && !gtrue(vm, "a4") && gnum(vm, "ge_id") == 1.0 &&
          gnil(vm, "ne"));
    CHECK(run(vm, "emp.Make_Ally(und); b1 = und.Is_Ally(emp); b2 = und.Is_Enemy(emp); ge2 = emp.Get_Enemy(); "
                  "emp.Make_Enemy(neu); b3 = neu.Is_Enemy(emp)") == "");
    CHECK(gtrue(vm, "b1") && !gtrue(vm, "b2") && gnil(vm, "ge2") && gtrue(vm, "b3"));
    CHECK(g.world.relation(0, 1) == PlayerRelation::Ally && g.world.relation(2, 0) == PlayerRelation::Enemy);
    // OI lua: the GalacticSim's table is the one written / read.
    CHECK(g.sim->relation(1, 0) == galactic::Relation::Ally && g.sim->relation(0, 2) == galactic::Relation::Enemy);
    g.sim->set_relation(0, 1, galactic::Relation::Enemy);
    CHECK(run(vm, "b4 = und.Is_Enemy(emp)") == "" && gtrue(vm, "b4"));
    g.sim->set_relation(0, 1, galactic::Relation::Ally);
    CHECK(run(vm, "emp.Is_Enemy()").find("PlayerWrapper::Lua_Is_Enemy -- invalid number of parameters. Expected 1, got 0.") !=
          std::string::npos);
    CHECK(run(vm, "emp.Make_Ally(5)").find("PlayerWrapper::Make_Ally -- invalid type for parameter 1. Expected player.") !=
          std::string::npos);

    // Tech locks, selection, flags, requests.
    const uint32_t frigate = g.raw.find("Frigate_T");
    CHECK(run(vm, "ft = Find_Object_Type('Frigate_T'); emp.Lock_Tech(ft)") == "" && g.world.tech_lock(0, frigate) == 1);
    CHECK(run(vm, "emp.Unlock_Tech(ft)") == "" && g.world.tech_lock(0, frigate) == 0 && g.world.tech_lock(1, frigate) == -1);
    // REMAINING_GAPS 1.5: Unlock_Tech = GalacticSim::unlock_tech (leaves the locked list, joins the unlocked list).
    CHECK(!g.sim->type_locked(0, frigate) && g.sim->type_unlocked(0, frigate) && !g.sim->type_unlocked(1, frigate));
    CHECK(run(vm, "emp.Lock_Tech(emp)").find("Expected game object type.") != std::string::npos);
    // Stage 3: Is_Build_Locked (script locks), Is_Affiliated_With (<Affiliation>), planet facts, Find_Path.
    // Find_Path answers from the AI finder (GALACTIC.md 4/5): AI players only, all-pairs fog rule (auto-reveal on by
    // default, so no frame / revealed byte is needed), never the human hop gate (GRC 1.1).
    CHECK(run(vm, "bl0 = ft.Is_Build_Locked(emp) and 1 or 0 emp.Lock_Tech(ft) bl1 = ft.Is_Build_Locked(emp) and 1 or 0 "
                  "emp.Unlock_Tech(ft) tt = Find_Object_Type('Trooper_T') af1 = tt.Is_Affiliated_With(emp) and 1 or 0 "
                  "af2 = tt.Is_Affiliated_With(und) and 1 or 0 af3 = ft.Is_Affiliated_With(emp) and 1 or 0 "
                  "pa = Find_All_Objects_Of_Type('Planet_A')[1] pc = Find_All_Objects_Of_Type('Planet_C')[1] "
                  "path = Find_Path(und, pa, pc) pn = table.getn(path) pm = path[2].Get_Type().Get_Name() "
                  "sl = pa.Get_Starbase_Level() co = pa.Is_Corrupted() and 1 or 0 de = pa.Is_Planet_Destroyed() and 1 or 0 "
                  "gt = GetCurrentTime.Galactic_Time() camp = Is_Campaign_Game() and 1 or 0") == "");
    CHECK(run(vm, "Find_Path(emp, pa, pc)").find("Find_Path -- can't do this from the point of view of non-ai players") !=
          std::string::npos);
    {
        // An attached AI path finder answers (its path, not the A* one); "no finder" (false) falls back to the A*.
        struct StubPaths final : ai::AiPathQuery {
            bool known = true;
            int calls = 0;
            bool ai_path(int32_t player, PlanetIndex from, PlanetIndex to, std::vector<PlanetIndex>& out) override {
                ++calls;
                out.clear();
                if (!known || player != 1) return false;
                if (from != to) out = {from, to};  // a direct "hop" the A* would never give; src == dst: unreachable
                return true;
            }
        } stub;
        g.world.attach_ai_paths(&stub);
        CHECK(g.world.ai_paths() == &stub);
        CHECK(run(vm, "sp = Find_Path(und, pa, pc) sn = table.getn(sp) s2 = sp[2].Get_Type().Get_Name() "
                      "sz = Find_Path(und, pa, pa)") == "");
        CHECK(stub.calls == 2 && gnum(vm, "sn") == 2.0 && gstr(vm, "s2") == "Planet_C" && gnil(vm, "sz"));
        CHECK(run(vm, "Find_Path(emp, pa, pc)").find("non-ai players") != std::string::npos && stub.calls == 2);
        stub.known = false;
        CHECK(run(vm, "fp = Find_Path(und, pa, pc) fn2 = table.getn(fp) fm = fp[2].Get_Type().Get_Name()") == "");
        CHECK(stub.calls == 3 && gnum(vm, "fn2") == 3.0 && gstr(vm, "fm") == "Planet_B");
        g.world.attach_ai_paths(nullptr);
        CHECK(run(vm, "fp2 = Find_Path(und, pa, pc) fn3 = table.getn(fp2)") == "" && gnum(vm, "fn3") == 3.0 &&
              stub.calls == 3);
    }
    CHECK(gnum(vm, "bl0") == 0.0 && gnum(vm, "bl1") == 1.0 && gnum(vm, "af1") == 1.0 && gnum(vm, "af2") == 0.0 &&
          gnum(vm, "af3") == 0.0);
    CHECK(gnum(vm, "pn") == 3.0 && gstr(vm, "pm") == "Planet_B" && gnum(vm, "sl") == 0.0 && gnum(vm, "co") == 0.0 &&
          gnum(vm, "de") == 0.0 && gnum(vm, "gt") == 0.0 && gnum(vm, "camp") == 0.0);
    CHECK(run(vm, "Find_Path(emp, pa)").find("Find_Path -- invalid number of parameters. Expected 3, got 2.") != std::string::npos);
    CHECK(run(vm, "Find_Path(emp, ft, pc)").find("parameter 2. This needs to be a valid planet") != std::string::npos);
    CHECK(run(vm, "ft.Is_Affiliated_With(1)").find("Is_Affiliated_With -- invalid type for parameter 1. Expected player") !=
          std::string::npos);
    vm.set_object_global("Target", g.planet(0));
    CHECK(run(vm, "emp.Select_Object(Target); emp.Enable_As_Actor(); und.Retreat(); und.Retreat(); "
                  "emp.Disable_Bombing_Run(true); und.Disable_Orbital_Bombardment(true); und.Remove_Orbital_Bombardment(false); "
                  "emp.Set_Black_Market_Tutorial(true); emp.Set_Sabotage_Tutorial(false); "
                  "emp.Enable_Advisor_Hints('space', false); und.Release_Credits_For_Tactical(1500); "
                  "und.Release_Credits_For_Tactical()") == "");
    const LuaPlayerState& es = g.world.states()[0];
    const LuaPlayerState& us = g.world.states()[1];
    CHECK(es.selected == g.store->id(g.planet(0)) && es.actor && us.retreat_requests == 2 && es.bombing_run_disabled);
    CHECK(us.orbital_bombardment_disabled && !us.orbital_bombardment_removed && es.black_market_tutorial &&
          !es.sabotage_tutorial && es.advisor_hints.at("space") == false);
    CHECK(g.world.tactical_releases.size() == 2 && g.world.tactical_releases[0].amount == 1500.0 &&
          g.world.tactical_releases[1].amount == -1.0);
    CHECK(run(vm, "emp.Disable_Bombing_Run(1)").find("Expected boolean.") != std::string::npos);
    CHECK(run(vm, "emp.Set_Sabotage_Tutorial()").find("PlayerWrapper::Lua_Set_Sabotage_Tutorial -- Missing required parameter.") !=
          std::string::npos);

    CHECK(run(vm, "Suspend_AI(1)") == "" && g.world.ai_suspended());
    CHECK(run(vm, "Suspend_AI(0)") == "" && !g.world.ai_suspended());
    CHECK(run(vm, "Suspend_AI()").find("LuaSuspendAI -- Expected float parameter 1") != std::string::npos);
    CHECK(run(vm, "Purge_Goals(und); r = GiveDesireBonus(und, 'Build_Initial_Starbase_Only', Target, 15, 5); "
                  "Declare_Goal_Type(); PlayerObject = und; Budget.Flush_Category('MajorItem')") == "");
    CHECK(gnil(vm, "r") && g.world.purge_goal_requests == std::vector<int32_t>{1});
    CHECK(g.world.desire_bonuses.size() == 1 && g.world.desire_bonuses[0].goal == "Build_Initial_Starbase_Only" &&
          g.world.desire_bonuses[0].target == g.store->id(g.planet(0)) && g.world.desire_bonuses[0].bonus == 15.0 &&
          g.world.desire_bonuses[0].duration == 5.0);
    CHECK(g.world.budget_flushes.size() == 1 && g.world.budget_flushes[0].player == 1 &&
          g.world.budget_flushes[0].category == "MajorItem");
    CHECK(run(vm, "Purge_Goals(1)").find("Purge_Goals -- invalid type for parameter 1. Expected a player.") !=
          std::string::npos);

    // WeightedTypeList + EvaluateTypeList: planet A holds the Empire frigate (orbit) and trooper (surface).
    CHECK(run(vm, "names = {'Frigate', 'Infantry', 'Corvette', 'Trooper_T', 'All'}; "
                  "wl = WeightedTypeList.Create(); wl.Parse(names, {1, 2, 3, 4, 5}); "
                  "found = EvaluateTypeList(und, Target, wl); "
                  "f1 = found[1]; f2 = found[2]; f3 = found[3]; f4 = found[4]; f5 = found[5]; fn = table.getn(found); "
                  "Target2 = Find_All_Objects_Of_Type('Planet_B')[1]; "
                  "g2 = EvaluateTypeList(und, Target2, wl); g1 = g2[1]; g5 = g2[5]") == "");
    CHECK(gtrue(vm, "f1") && gtrue(vm, "f2") && !gtrue(vm, "f3") && gtrue(vm, "f4") && gtrue(vm, "f5") &&
          gnum(vm, "fn") == 5.0);
    CHECK(!gtrue(vm, "g1") && gtrue(vm, "g5"));  // B: only the planet itself
    CHECK(run(vm, "EvaluateTypeList(und, Target)").find("Expected 3, got 2.") != std::string::npos);
    CHECK(run(vm, "EvaluateTypeList(und, Target, Target)").find("Expected weighted type list.") != std::string::npos);
    CHECK(run(vm, "WeightedTypeList.Parse({}, {})").find("WeightedTypeListClass -- Invalid parameters") != std::string::npos);

    // DiscreteDistribution: one synced float draw over the positive weights.
    GameRandom ref = g.rng;
    CHECK(run(vm, "dd = DiscreteDistribution.Create(); dd.Insert('a', 1); dd.Insert('zero', 0); dd.Insert('b', 3); "
                  "s1 = dd.Sample(); s2 = dd.Sample(); e = DiscreteDistribution.Create().Sample()") == "");
    for (const char* name : {"s1", "s2"}) {
        const float r = ref.synced().range_float(0.0f, 4.0f);
        CHECK(gstr(vm, name) == (r < 1.0f ? "a" : "b"));
    }
    CHECK(gnil(vm, "e") && g.rng == ref);
    CHECK(run(vm, "dd.Insert('c')").find("DiscreteDistribution::Insert -- invalid number of parameters. Expected 2, got 1.") !=
          std::string::npos);

    // Every implemented binding of this family resolves to an implementation.
    for (const auto& [cls, name] : {std::pair{"PlayerWrapper", "Get_Credits"}, std::pair{"PlayerWrapper", "Make_Ally"},
                                    std::pair{"WeightedTypeListClass", "Parse"},
                                    std::pair{"LuaDiscreteDistributionClass", "Sample"},
                                    std::pair{"BudgetWrapper", "Flush_Category"}})
        CHECK(vm.resolve_method(cls, name) == 1);
    CHECK(vm.errors().empty());
}

// ------------------------------------------------------------------------------------------

void test_perception() {
    Galaxy g;
    LuaVm vm(g.host());
    attach_player_world(vm, &g.world);

    // Current-mode equations with a resolver that knows every token.
    ai::EquationSet eq;
    eq.add("Self_Score", "Variable_Self.Some.Token * 2 + 1");
    eq.add("Target_Score", "clamp(Variable_Target.Other.Token, 0, 50) + Function_Self_Score.Evaluate");
    eq.add("Rand", "1 # 5");
    eq.add("Missing_Token", "Variable_Self.MISSING + 1");
    eq.link();
    TestResolver tr;

    // Galactic-context equations on the real GalacticPerception of the sim's state.
    ai::EquationSet geq;
    geq.add("Enemy_Space", "Variable_Target.EnemyForce.SpaceTotalUnnormalized");
    geq.add("Constant", "2 * 3");
    geq.link();
    const ai::GalacticSnapshot snap = g.snapshot();
    CHECK(snap.validate().empty());
    ai::GalacticPerception perc;
    perc.reset(snap);
    perc.update(snap, 0);
    std::vector<ai::PerceptionMemory> mem;
    for (uint32_t p = 0; p < snap.player_count(); ++p) mem.push_back(perc.memory(static_cast<int32_t>(p)));
    const ai::GalacticTokenResolver gres(snap, mem);
    g.world.set_perception({&eq, &tr}, {&geq, &gres});

    vm.set_object_global("Target", g.planet(0));
    SimRng before = g.rng.synced();
    CHECK(run(vm, "und = Find_Player('Underworld'); emp = Find_Player('Empire'); "
                  "v1 = EvaluatePerception('Self_Score', und); v2 = EvaluatePerception('Target_Score', und, Target); "
                  "v3 = EvaluatePerception('Rand', emp); v4 = EvaluatePerception('Missing_Token', und); "
                  "v5 = EvaluatePerception('No_Such_Equation', und); "
                  "w1 = Evaluate_In_Galactic_Context('Constant', und); w2 = Evaluate_In_Galactic_Context('Enemy_Space', und)") == "");
    ai::EvalContext ctx;
    ctx.self = 1;
    ctx.enemy = g.world.enemy_of(1);
    ctx.human = g.world.local_player();
    const ai::EvalResult r1 = ai::evaluate(eq, eq.find("Self_Score"), ctx, tr);
    ctx.target = ctx.location = 0;  // planet A's row
    const ai::EvalResult r2 = ai::evaluate(eq, eq.find("Target_Score"), ctx, tr);
    ai::EvalContext rc;
    rc.self = 0;
    rc.rng = &before;
    const ai::EvalResult r3 = ai::evaluate(eq, eq.find("Rand"), rc, tr);
    CHECK(r1.ok && same_bits(gnum(vm, "v1"), r1.value));
    CHECK(r2.ok && same_bits(gnum(vm, "v2"), r2.value));
    CHECK(r3.ok && same_bits(gnum(vm, "v3"), r3.value) && before == g.rng.synced());
    CHECK(gnil(vm, "v4") && gnil(vm, "v5"));
    CHECK(gnum(vm, "w1") == 6.0 && gnil(vm, "w2"));  // no target in galactic context

    // EvaluatePerception in the galactic context itself: Underworld (AI) sees the Empire frigate at A.
    g.world.set_perception({&geq, &gres}, {&geq, &gres});
    CHECK(run(vm, "s_a = EvaluatePerception('Enemy_Space', und, Target); "
                  "s_b = EvaluatePerception('Enemy_Space', und, Find_All_Objects_Of_Type('Planet_B')[1])") == "");
    CHECK(gnum(vm, "s_a") == 500.0 && gnum(vm, "s_b") == 0.0);

    CHECK(run(vm, "EvaluatePerception('X')").find("Evaluate_Perception: Invalid number of parameters. Expected 2 or 3, got 1.") !=
          std::string::npos);
    CHECK(run(vm, "EvaluatePerception('X', 3)").find("Expected player object.") != std::string::npos);
    CHECK(run(vm, "Evaluate_In_Galactic_Context(1, und)").find("parameter 1. Expected string.") != std::string::npos);
}

// Shipped Data\XML\AI\PerceptualEquations through EvaluatePerception == ai::evaluate, bit for bit.
fs::path find_child(const fs::path& dir, std::string_view name) {
    std::error_code ec;
    for (const fs::path& p : list_directory(dir, ec)) {
        const std::string n = path_to_utf8(p.filename());
        if (normalize_path(n) == normalize_path(name)) return p;
    }
    return {};
}

void test_shipped_equations() {
    const char* root = std::getenv("ALAMO_FOC_DATA");
    if (!root || !*root) {
        std::printf("  shipped equations: ALAMO_FOC_DATA not set, skipped\n");
        return;
    }
    fs::path dir = path_from_utf8(root);
    for (const char* part : {"Data", "XML", "AI", "PerceptualEquations"}) {
        dir = find_child(dir, part);
        if (dir.empty()) break;
    }
    if (dir.empty()) {
        std::printf("  shipped equations: no Data\\XML\\AI\\PerceptualEquations under ALAMO_FOC_DATA, skipped\n");
        return;
    }
    std::error_code ec;
    std::vector<fs::path> paths = list_directory(dir, ec);
    std::vector<std::pair<std::string, fs::path>> files;
    for (const fs::path& p : paths)
        if (normalize_path(path_to_utf8(p.extension())) == ".XML") files.emplace_back(normalize_path(path_to_utf8(p.filename())), p);
    std::sort(files.begin(), files.end());
    ai::EquationSet set;
    ai::LoadReport report;
    for (const auto& [name, p] : files) {
        std::vector<uint8_t> bytes;
        CHECK(read_host_file(p, bytes));
        set.add_file(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), name, report);
    }
    set.link(&report);
    CHECK(set.size() > 100);

    Galaxy g;
    LuaVm vm(g.host());
    attach_player_world(vm, &g.world);
    TestResolver tr;
    g.world.set_perception({&set, &tr}, {&set, &tr});
    vm.set_object_global("Target", g.planet(1));
    CHECK(run(vm, "und = Find_Player('Underworld')") == "");
    uint32_t ok = 0, failed = 0, mismatched = 0;
    for (uint32_t i = 0; i < set.size(); ++i) {
        const ai::Equation& e = set.equation(i);
        if (!e.parsed || set.find(e.name) != i) continue;
        SimRng before = g.rng.synced();
        if (run(vm, "pv = EvaluatePerception('" + e.name + "', und, Target)") != "") {
            ++mismatched;
            continue;
        }
        ai::EvalContext ctx;
        ctx.self = 1;
        ctx.target = ctx.location = 1;
        ctx.enemy = g.world.enemy_of(1);
        ctx.human = g.world.local_player();
        ctx.rng = &before;
        const ai::EvalResult r = ai::evaluate(set, i, ctx, tr);
        const bool lua_nil = gnil(vm, "pv");
        if (r.ok != !lua_nil || (r.ok && !same_bits(r.value, gnum(vm, "pv"))) || !(before == g.rng.synced())) {
            ++mismatched;
            std::printf("  mismatch: %s\n", e.name.c_str());
        }
        (r.ok ? ok : failed)++;
    }
    std::printf("  shipped equations: %zu files, %zu equations, %u evaluated, %u nil (Script_/unparsed refs), %u mismatches\n",
                files.size(), set.size(), ok, failed, mismatched);
    CHECK(mismatched == 0 && ok > 100);
}

// ------------------------------------------------------------------------------------------

uint64_t session_digest() {
    Galaxy g;
    LuaVm vm(g.host());
    attach_player_world(vm, &g.world);
    ai::EquationSet eq;
    eq.add("Rand", "Variable_Self.X # 9000");
    eq.link();
    TestResolver tr;
    g.world.set_perception({&eq, &tr}, {&eq, &tr});
    vm.set_object_global("Target", g.planet(2));
    const std::string err = run(vm,
                                "emp = Find_Player('Empire'); und = Find_Player('Underworld'); out = '' "
                                "for i = 1, 20 do "
                                "  und.Give_Money(i * 37.25); emp.Give_Money(-i * 11.5) "
                                "  local d = DiscreteDistribution.Create() d.Insert('x', i) d.Insert('y', 21 - i) "
                                "  out = out .. d.Sample() .. string.format('%.9g;', EvaluatePerception('Rand', und, Target)) "
                                "  if i == 7 then emp.Make_Ally(und) end "
                                "end "
                                "GiveDesireBonus(und, 'G', Target, 2.5) Purge_Goals(emp) "
                                "out = out .. string.format('%.9g %.9g', emp.Get_Credits(), und.Get_Credits())");
    CHECK(err == "");
    StateHasher h;
    g.world.hash(h);
    g.sim->hash_state(h);
    h.str(gstr(vm, "out"));
    h.i32(g.rng.synced().range_int(0, 1000000));
    return h.digest();
}

void test_determinism() {
    const uint64_t a = session_digest();
    const uint64_t b = session_digest();
    uint64_t c = 0;
    std::thread t([&c] { c = session_digest(); });
    t.join();
    CHECK(a == b && a == c);
    std::printf("  session digest %016llx (x3 identical: %s)\n", static_cast<unsigned long long>(a),
                (a == b && a == c) ? "yes" : "NO");

    // The world + sim hash sees every Lua-side change (OI lua: relations live in the GalacticSim when one is attached).
    Galaxy g;
    LuaVm vm(g.host());
    attach_player_world(vm, &g.world);
    StateHasher h0;
    g.world.hash(h0);
    g.sim->hash_state(h0);
    CHECK(run(vm, "Find_Player('Empire').Make_Ally(Find_Player('Underworld'))") == "");
    StateHasher h1;
    g.world.hash(h1);
    g.sim->hash_state(h1);
    CHECK(h0.digest() != h1.digest());
}

// OI lua: Give_Random_Sliceable_Tech with a GalacticSim writes the sim's unlocked list.
void test_sliceable_unlock() {
    Galaxy g;
    LuaVm vm(g.host());
    attach_player_world(vm, &g.world);
    const uint32_t a = g.raw.find("Slice_A_T"), b = g.raw.find("Slice_B_T");
    CHECK(!g.sim->type_unlocked(0, a) && !g.sim->type_unlocked(0, b));
    CHECK(run(vm, "emp = Find_Player('Empire') t1 = emp.Give_Random_Sliceable_Tech().Get_Name()") == "");
    const std::string t1 = gstr(vm, "t1");
    const uint32_t first = t1 == "Slice_A_T" ? a : b;
    CHECK((t1 == "Slice_A_T" || t1 == "Slice_B_T") && g.sim->type_unlocked(0, first) && !g.sim->type_unlocked(1, first));
    // The unlocked type is no longer a candidate: the second call gives the other one; the single-candidate rule raises
    // the tech level (GalacticSim::raise_tech_level, REMAINING_GAPS 1.4), whose sweep unlocks it (Tech_Level 1 < 2).
    const int32_t tech0 = g.sim->players()[0].tech_level;
    const uint32_t second = first == a ? b : a;
    CHECK(run(vm, "t2 = emp.Give_Random_Sliceable_Tech().Get_Name()") == "" && gstr(vm, "t2") != t1);
    CHECK(g.sim->players()[0].tech_level == tech0 + 1 && g.sim->type_unlocked(0, second) && !g.sim->type_unlocked(1, second));

    // At the max tech the single-candidate raise does nothing: tech and unlocked list unchanged.
    Galaxy h;
    LuaVm vm2(h.host());
    attach_player_world(vm2, &h.world);
    CHECK(run(vm2, "emp = Find_Player('Empire') u1 = emp.Give_Random_Sliceable_Tech().Get_Name()") == "");
    const uint32_t other = gstr(vm2, "u1") == "Slice_A_T" ? b : a;
    const int32_t tech1 = h.sim->players()[0].tech_level;
    h.sim->set_max_tech_level(0, tech1);
    CHECK(run(vm2, "u2 = emp.Give_Random_Sliceable_Tech().Get_Name()") == "");
    CHECK(h.sim->players()[0].tech_level == tech1 && !h.sim->type_unlocked(0, other));
}

// Players without a galactic sim (tactical / skirmish): setup rows back the facts.
void test_setup_players() {
    LuaPlayerWorld w;
    LuaPlayerSetup a, b;
    a.faction = "Rebel";
    a.human = true;
    a.credits = 100.0f;
    b.faction = "Empire";
    b.ai = true;
    b.credit_multiplier = 0.5f;
    b.credits = 10.0f;
    b.credit_cap = 50.0f;
    b.difficulty = "Easy_Default";
    w.set_players({a, b});
    LuaHost host;
    LuaVm vm(host);
    attach_player_world(vm, &w);
    CHECK(run(vm, "r = Find_Player('rebel'); e = Find_Player('Empire'); e.Give_Money(30); c1 = e.Get_Credits(); "
                  "e.Give_Money(500); c2 = e.Get_Credits(); d = e.Get_Difficulty(); loc = Find_Player('local').Get_ID(); "
                  "en = r.Is_Enemy(e)") == "");
    CHECK(gnum(vm, "c1") == 25.0 && gnum(vm, "c2") == 50.0 && gstr(vm, "d") == "Easy" && gnum(vm, "loc") == 0.0 &&
          gtrue(vm, "en"));
    CHECK(w.credits(1) == 50.0f);
}

// L1/S4 wave 1: galactic containment, Assemble_Fleet, Change_Owner (GalacticSim::change_owner), Invade,
// base waits / next base types, planet usability, heroes, Destroy_Contained_Objects, StringCompare.
void test_galactic_ops() {
    Galaxy g({{0, 0, "Frigate_T"}, {0, 0, "Hero_Ship_T"}, {1, 1, "Frigate_T"}});
    galactic::GalacticSim& sim = *g.sim;
    ObjectBridge bridge;  // as StorySession wires it
    bridge.change_owner = [&sim](objects::ObjectHandle h, int32_t p) { return sim.change_owner(h, p); };
    bridge.container_of = [&sim](objects::ObjectHandle h) { return sim.container_of(h); };
    bridge.contents_of = [&sim](objects::ObjectHandle h) { return sim.contents_of(h); };
    bridge.destroy_object = [&sim](objects::ObjectHandle h) { return sim.destroy_object(h); };
    LuaVm vm(g.host());
    attach_player_world(vm, &g.world);
    attach_object_bridge(vm, &bridge);
    const std::vector<uint32_t> fa = sim.fleets_at(0, 0);
    CHECK(fa.size() == 1 && sim.fleets()[fa[0]].contents.size() == 3);
    if (fa.size() != 1 || sim.fleets()[fa[0]].contents.size() != 3) return;
    const std::vector<objects::ObjectHandle> c = sim.fleets()[fa[0]].contents;
    const uint32_t ufb = sim.fleets_at(1, 1).empty() ? galactic::kNoFleet : sim.fleets_at(1, 1)[0];
    CHECK(ufb != galactic::kNoFleet);
    if (ufb == galactic::kNoFleet) return;
    vm.set_object_global("F1", c[0]);
    vm.set_object_global("F2", c[1]);
    vm.set_object_global("Hero", c[2]);
    vm.set_object_global("Fleet", sim.fleets()[fa[0]].object);
    vm.set_object_global("UFr", sim.fleets()[ufb].contents[0]);
    vm.set_object_global("PA", g.planet(0));
    vm.set_object_global("PB", g.planet(1));
    vm.set_object_global("PC", g.planet(2));
    vm.set_object_global("Tr", sim.planets()[0].surface[0]);
    vm.set_object_global("UTr", sim.planets()[2].surface[0]);
    const objects::ObjectHandle tr = sim.planets()[0].surface[0], utr = sim.planets()[2].surface[0];
    CHECK(run(vm, "emp = Find_Player('Empire') und = Find_Player('Underworld')") == "");

    // Containment through the GalacticSim (ships carry no store container).
    CHECK(run(vm, "p1 = F1.Get_Parent_Object() == Fleet l1 = F1.Get_Planet_Location() == PA l2 = Tr.Get_Planet_Location() == PA "
                  "l3 = Fleet.Get_Planet_Location() == PA n1 = Fleet.Get_Contained_Object_Count() "
                  "n2 = PA.Get_Contained_Object_Count() h1 = Fleet.Contains_Hero() h2 = PA.Contains_Hero() h3 = Tr.Contains_Hero() "
                  "hl = table.getn(Fleet.Get_Contained_Heroes()) hn = Fleet.Get_Contained_Heroes()[1] == Hero "
                  "ct = Fleet.Contains_Object_Type(Find_Object_Type('Hero_Ship_T'))") == "");
    CHECK(gtrue(vm, "p1") && gtrue(vm, "l1") && gtrue(vm, "l2") && gtrue(vm, "l3") && gnum(vm, "n1") == 3.0 &&
          gnum(vm, "n2") == 2.0);
    CHECK(gtrue(vm, "h1") && gtrue(vm, "h2") && !gtrue(vm, "h3") && gnum(vm, "hl") == 1.0 && gtrue(vm, "hn") && gtrue(vm, "ct"));
    // Garrison queries (no garrison holders in this galaxy).
    CHECK(run(vm, "g1 = Tr.Is_In_Garrison() g2 = PA.Has_Garrison() gl = table.getn(PA.Get_Garrisoned_Units()) "
                  "cg = Tr.Can_Garrison(PA) Tr.Eject_Garrison()") == "");
    CHECK(!gtrue(vm, "g1") && !gtrue(vm, "g2") && gnum(vm, "gl") == 0.0 && !gtrue(vm, "cg"));
    CHECK(run(vm, "Tr.Can_Garrison(5)").find("GameObjectWrapper::Can_Garrison -- invalid type for parameter 1. Expected game object or AI target.") !=
          std::string::npos);

    // Assemble_Fleet.
    CHECK(run(vm, "af = Assemble_Fleet({Hero}) a1 = (af ~= Fleet) a2 = af.Get_Contained_Object_Count() "
                  "af2 = Assemble_Fleet({F1, F2}) a3 = (af2 == Fleet) none = Assemble_Fleet({PA, Fleet})") == "");
    CHECK(gtrue(vm, "a1") && gnum(vm, "a2") == 1.0 && gtrue(vm, "a3") && gnil(vm, "none") && sim.fleets_at(0, 0).size() == 2);
    const uint32_t hero_fleet = sim.fleet_containing(c[2]);
    CHECK(run(vm, "af3 = Assemble_Fleet({F1, Hero, UFr}) a4 = af3.Get_Contained_Object_Count() "
                  "a5 = Fleet.Get_Contained_Object_Count()") == "");
    CHECK(gnum(vm, "a4") == 2.0 && gnum(vm, "a5") == 1.0 && !sim.fleets()[hero_fleet].alive && sim.fleets_at(0, 0).size() == 2 &&
          sim.fleet_containing(c[0]) == sim.fleet_containing(c[2]) && sim.fleet_containing(sim.fleets()[ufb].contents[0]) == ufb);
    CHECK(run(vm, "Assemble_Fleet(5)").find("Assemble_Fleet -- invalid type for parameter 1. Expected lua table of units.") != std::string::npos);
    CHECK(run(vm, "Assemble_Fleet({5})").find("Assemble_Fleet -- unexpected type found in table of units to assemble.") != std::string::npos);
    CHECK(run(vm, "Assemble_Fleet()").find("Assemble_Fleet -- invalid number of parameters. Expected 1, got 0.") != std::string::npos);

    // Change_Owner: a fleet member, a surface unit, a planet (GE 4.2 forced transfer), a fleet object.
    sim.set_emit_events(true);
    const uint32_t old_fleet = fa[0];
    CHECK(run(vm, "F2.Change_Owner(und) Tr.Change_Owner(und) PC.Change_Owner(emp) o1 = F2.Get_Owner().Get_ID() "
                  "o2 = F2.Get_Parent_Object().Get_Owner().Get_ID()") == "");
    CHECK(g.store->owner(c[1]) == 1 && g.store->owner(tr) == 1 && !sim.fleets()[old_fleet].alive && sim.fleets_at(0, 1).size() == 1);
    CHECK(gnum(vm, "o1") == 1.0 && gnum(vm, "o2") == 1.0);
    CHECK(sim.planets()[2].owner == 0 && sim.planets()[2].control == 1.0f && g.store->owner(utr) == 0);
    std::vector<galactic::GalacticEvent> evs;
    sim.drain_events(evs);
    CHECK(std::any_of(evs.begin(), evs.end(), [](const galactic::GalacticEvent& e) {
        return e.kind == galactic::GalacticEventKind::PlanetOwnerChanged && e.planet == 2 && e.player == 0 && e.other_player == 1;
    }));
    sim.update();
    CHECK(sim.planets()[2].owner == 0);  // derived owner keeps the transfer (converted surface unit)
    CHECK(run(vm, "af3.Change_Owner(und) ufa = F2.Get_Parent_Object() uc = ufa.Get_Contained_Object_Count()") == "");
    CHECK(gnum(vm, "uc") == 3.0 && sim.fleets_at(0, 1).size() == 1 && sim.fleets_at(0, 0).size() == 0 && g.store->owner(c[2]) == 1);
    CHECK(run(vm, "PC.Change_Owner(emp)") == "" && sim.planets()[2].owner == 0);  // same owner: nothing
    // Invade on a fleet: one direct-path order (no pipeline attached here).
    const uint32_t issued = bridge.orders_issued;
    CHECK(run(vm, "ufa.Invade() PA.Invade()") == "" && bridge.orders_issued == issued + 1);

    // Base waits, next base types, planet usability, landing.
    CHECK(run(vm, "w1 = WaitForStarbase(PA, 1).IsFinished() w0 = WaitForGroundbase(PA, 0).IsFinished() "
                  "nb = GetNextStarbaseType(PA) ng = GetNextGroundbaseType(PA) nm = PA.Get_Next_Starbase_Type() "
                  "bf = BlockForever().IsFinished()") == "");
    CHECK(!gtrue(vm, "w1") && gtrue(vm, "w0") && gnil(vm, "nb") && gnil(vm, "ng") && gnil(vm, "nm") && !gtrue(vm, "bf"));
    CHECK(run(vm, "WaitForStarbase(PA, 9)").find("WaitForStarBase : Invalid value 9 for parameter 2. Must be between 0 and 5.") !=
          std::string::npos);
    CHECK(run(vm, "WaitForStarbase(F1, 1)").find("WaitForStarbase : Invalid type for parameter 1. Expected planet game object.") !=
          std::string::npos);
    CHECK(run(vm, "WaitForGroundbase(PA)").find("WaitForGroundBase : Invalid number of parameters: 1 should be 2.") != std::string::npos);
    CHECK(run(vm, "GetNextStarbaseType()").find("GetNextStarbaseType : invalid number of parameters. Expected 1, got 0.") !=
          std::string::npos);
    sim.set_hidden(1, true);
    CHECK(run(vm, "u1 = PA.Get_Is_Planet_AI_Usable() u2 = PB.Get_Is_Planet_AI_Usable() u3 = F1.Get_Is_Planet_AI_Usable() "
                  "c1 = UTr.Can_Land_On_Planet(PA) c2 = F1.Can_Land_On_Planet(PA) c3 = UTr.Can_Land_On_Planet(PB)") == "");
    CHECK(gtrue(vm, "u1") && !gtrue(vm, "u2") && !gtrue(vm, "u3") && gtrue(vm, "c1") && !gtrue(vm, "c2") && !gtrue(vm, "c3"));
    CHECK(run(vm, "UTr.Can_Land_On_Planet(1)").find("GameObjectWrapper::Can_Land_On_Planet -- invalid type for parameter 1. Expected game object.") !=
          std::string::npos);
    CHECK(run(vm, "s1 = StringCompare('Galactic', 'GALACTIC') s2 = StringCompare('Land', 'Galactic') s3 = StringCompare(1, 1)") == "");
    CHECK(gtrue(vm, "s1") && !gtrue(vm, "s2") && !gtrue(vm, "s3"));

    // Removal through the sim: Destroy_Contained_Objects, Despawn.
    CHECK(run(vm, "ufa.Destroy_Contained_Objects(1.0) UTr.Despawn()") == "");
    CHECK(sim.fleets_at(0, 1).empty() && !g.store->alive(c[0]) && !g.store->alive(c[2]) && sim.planets()[2].surface.empty());
    CHECK(run(vm, "PA.Destroy_Contained_Objects('x')").find("Expected number.") != std::string::npos);
    sim.update();  // flush: the destroyed objects leave the store
    CHECK(!g.store->exists(c[0]) && !g.store->exists(c[2]) && !g.store->exists(utr));
}

// ------------------------------------------------------------------------------------------
// P4 lua: the bindings of the FoC galactic special systems (special_systems.hpp, P4_SIM_SYSTEMS.md): galactic
// Activate_Ability / task-force Activate_Ability + Land_Units -> GalacticSim::ground_activated_trigger, Lock_Tech ->
// lock_type, Set_*_Tutorial -> the sim flags, Is_Ability_Ready / Activate_Ability Black Market gate.

// Minimal AI host: one galactic task force (id 3) whose units the test sets; orders are recorded (not executed).
class TriggerAi final : public AiScriptHost {
public:
    std::vector<objects::ObjectHandle> units;
    std::vector<AiTfOrder> orders;
    bool tf_valid(int32_t tf) override { return tf == 3; }
    int32_t tf_force_count(int32_t) override { return static_cast<int32_t>(units.size()); }
    void tf_set_flag(int32_t, AiTfFlag, bool) override {}
    int32_t tf_produce_force(int32_t, const AiArg&, bool) override { return -1; }
    int32_t tf_get_stage(int32_t) override { return -1; }
    bool tf_all_on_free_store(int32_t) override { return false; }
    int32_t tf_order(int32_t, AiTfOrder o, const AiArg&) override {
        orders.push_back(o);
        return static_cast<int32_t>(orders.size());
    }
    std::vector<std::string> abilities;  // L1P4_LUA_GAPS 1: the type name the galactic Activate_Ability passes
    int32_t tf_activate_ability(int32_t tf, std::string_view ability, bool, const AiArg& a) override {
        abilities.emplace_back(ability);
        return tf_order(tf, AiTfOrder::ActivateAbility, a);
    }
    bool tf_is_raid_capable(int32_t) override { return false; }
    void tf_release_forces(int32_t, double) override {}
    void tf_release_unit(int32_t, objects::ObjectHandle) override {}
    std::vector<objects::ObjectHandle> tf_units(int32_t tf) override { return tf == 3 ? units : std::vector<objects::ObjectHandle>{}; }
    std::string tf_goal_type_name(int32_t) override { return {}; }
    bool tf_collect_free_units(int32_t) override { return false; }
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

void tune_special(galactic::GalacticRules& r, const objects::ObjectTypeDatabase& raw) {
    galactic::SpecialRules& s = r.special;
    s.types.resize(r.types.size());
    galactic::SpecialAbilityDef halt;  // P4 2.2 option 4 only (credit halt), bought on the black market
    halt.cls = galactic::SpecialAbilityClass::Sabotage;
    halt.name = "Halter_Sabotage";
    halt.name_crc = galactic::special_name_crc(halt.name);
    halt.style = galactic::ActivationStyle::GroundActivated;
    halt.causes_despawn = true;
    halt.min_respawn = halt.max_respawn = 5.0f;
    halt.sabotage_can[galactic::kSabotageHalt] = true;
    halt.halt_duration = 10.0f;
    halt.must_be_bought = true;
    s.abilities = {halt};
    const uint32_t halter = raw.find("Halter_T");
    s.types[halter].abilities = {0};
    s.types[halter].generic_hero = true;
    galactic::BlackMarketItemDef a, b;
    a.name = "Halt_Tech";
    a.name_crc = galactic::special_name_crc(a.name);
    a.ability_crcs = {halt.name_crc};
    b.name = "Stealth_Item";
    b.name_crc = galactic::special_name_crc(b.name);
    b.ability_crcs = {galactic::special_name_crc("Stealth_BM")};
    s.items = {a, b};
}

// One scripted session; returns its digest (world + sim + RNG + script output).
uint64_t special_session() {
    Galaxy g({{0, 0, "Halter_T"}, {0, 0, "Halter_T"}, {0, 0, "Halter_T"}, {1, 2, "Stealth_Trooper_T"}}, tune_special);
    galactic::GalacticSim& sim = *g.sim;
    sim.set_emit_events(true);
    const uint32_t halter = g.raw.find("Halter_T"), frigate = g.raw.find("Frigate_T"), trooper = g.raw.find("Trooper_T");
    std::vector<objects::ObjectHandle> h;
    for (const objects::ObjectHandle u : sim.planets()[0].surface)
        if (g.store->type(u) == halter) h.push_back(u);
    CHECK(h.size() == 3);
    if (h.size() != 3) return 0;
    // Empire heroes launched to A's orbit, then handed to the Underworld: Underworld saboteurs in orbit of an Empire planet.
    CHECK(sim.launch_units(0, 0, h) == commands::RejectReason::None);
    for (const objects::ObjectHandle u : h) CHECK(sim.change_owner(u, 1));
    const uint32_t uf = sim.fleet_containing(h[0]);
    CHECK(uf != galactic::kNoFleet && g.store->owner(h[0]) == 1 && sim.fleets()[uf].planet == 0 && !sim.fleets()[uf].moving);
    if (uf == galactic::kNoFleet) return 0;
    const objects::ObjectHandle stealth = sim.planets()[2].surface.back();
    CHECK(g.store->type(stealth) == g.raw.find("Stealth_Trooper_T"));

    LuaHost host = g.host();
    host.campaign = true;
    LuaVm vm(host);
    attach_player_world(vm, &g.world);
    vm.set_object_global("H1", h[0]);
    vm.set_object_global("UFleet", sim.fleets()[uf].object);
    vm.set_object_global("S", stealth);
    CHECK(run(vm, "emp = Find_Player('Empire') und = Find_Player('Underworld') ft = Find_Object_Type('Frigate_T') "
                  "tt = Find_Object_Type('Trooper_T') out = ''") == "");

    // Lock_Tech / Unlock_Tech = the sim's lock list (shared with the hack lock); Is_Build_Locked reads it.
    sim.lock_type(1, trooper, true);  // as the super-weapon hack would
    CHECK(run(vm, "emp.Lock_Tech(ft) l1 = ft.Is_Build_Locked(emp) l2 = tt.Is_Build_Locked(und) l3 = tt.Is_Build_Locked(emp)") == "");
    CHECK(gtrue(vm, "l1") && gtrue(vm, "l2") && !gtrue(vm, "l3") && sim.type_locked(0, frigate) && g.world.tech_lock(0, frigate) == 1);
    // OI lua: Is_Obsolete reads the same locked list (STRATEGIC_GAPS §5.1): Lock_Tech and the hack lock count.
    CHECK(run(vm, "o1 = ft.Is_Obsolete(emp) o2 = tt.Is_Obsolete(und) o3 = tt.Is_Obsolete(emp)") == "");
    CHECK(gtrue(vm, "o1") && gtrue(vm, "o2") && !gtrue(vm, "o3"));
    CHECK(run(vm, "und.Unlock_Tech(tt) l4 = tt.Is_Build_Locked(und)") == "" && !gtrue(vm, "l4") && !sim.type_locked(1, trooper));
    // REMAINING_GAPS 1.5: Unlock_Tech also joins the unlocked list (unlock_tech); lock_type(false) alone does not.
    CHECK(sim.type_unlocked(1, trooper) && !sim.type_unlocked(0, trooper));
    sim.lock_type(0, trooper, true);
    sim.lock_type(0, trooper, false);
    CHECK(!sim.type_locked(0, trooper) && !sim.type_unlocked(0, trooper));
    // Set_Black_Market_Tutorial / Set_Sabotage_Tutorial = the sim's player flags.
    CHECK(run(vm, "und.Set_Black_Market_Tutorial(true) und.Set_Sabotage_Tutorial(true) emp.Set_Sabotage_Tutorial(false)") == "");
    CHECK(sim.players()[1].bm_tutorial && sim.players()[1].sabotage_tutorial && !sim.players()[0].sabotage_tutorial);
    CHECK(run(vm, "und.Set_Black_Market_Tutorial(false)") == "" && !sim.players()[1].bm_tutorial && sim.players()[1].sabotage_tutorial);
    CHECK(run(vm, "und.Set_Sabotage_Tutorial(false)") == "" && !sim.players()[1].sabotage_tutorial);

    // Is_Ability_Ready: STEALTH is Must_Be_Bought_On_Black_Market and listed by Stealth_Item (not bought); SPRINT is free.
    CHECK(run(vm, "r1 = S.Is_Ability_Ready('STEALTH') r2 = S.Is_Ability_Ready('SPRINT')") == "");
    CHECK(!gtrue(vm, "r1") && gtrue(vm, "r2"));
    {   // the same object type in a non-campaign script: skirmish always passes
        LuaVm skirmish(g.host());
        attach_player_world(skirmish, &g.world);
        skirmish.set_object_global("S", stealth);
        CHECK(run(skirmish, "r0 = S.Is_Ability_Ready('STEALTH')") == "" && gtrue(skirmish, "r0"));
    }
    // Activate_Ability on the stage-2 path (a campaign battle store without the S9 system): gated likewise.
    objects::ObjectStore tstore(objects::ModeKind::Land, objects::StoreKind::Primary, g.types, &g.ids);
    objects::SpawnParams sp;
    sp.type = g.raw.find("Stealth_Trooper_T");
    sp.owner = 1;
    const objects::ObjectHandle ts = tstore.create(sp);
    LuaHost thost = g.host();
    thost.store = &tstore;
    thost.campaign = true;
    ObjectBridge tbridge;  // script-side ability state (no pipeline: orders are counted, not executed)
    LuaVm tvm(thost);
    attach_object_bridge(tvm, &tbridge);
    attach_player_world(tvm, &g.world);
    tvm.set_object_global("T", ts);
    CHECK(run(tvm, "T.Activate_Ability('STEALTH') a1 = T.Is_Ability_Active('STEALTH') T.Activate_Ability('SPRINT') "
                   "a2 = T.Is_Ability_Active('SPRINT')") == "");
    CHECK(!gtrue(tvm, "a1") && gtrue(tvm, "a2"));
    CHECK(sim.bm_give_item(1, "Stealth_Item"));
    CHECK(run(vm, "r3 = S.Is_Ability_Ready('STEALTH')") == "" && gtrue(vm, "r3"));
    CHECK(run(tvm, "T.Activate_Ability('STEALTH') a3 = T.Is_Ability_Active('STEALTH')") == "" && gtrue(tvm, "a3"));

    // Galactic Activate_Ability -> ground_activated_trigger; returns nothing (L1P4_LUA_GAPS 1.2). The halt ability must
    // be bought: nothing yet.
    const int32_t halted_before = sim.planets()[0].halt_until;
    CHECK(run(vm, "b = H1.Activate_Ability() bn = (b == nil)") == "" && gtrue(vm, "bn"));
    CHECK(g.store->alive(h[0]) && sim.planets()[0].halt_until == halted_before && sim.respawns_pending(0).empty());
    CHECK(sim.bm_give_item(1, "Halt_Tech"));
    auto messages_with = [&vm](std::string_view what) {
        size_t n = 0;
        for (const std::string& m : vm.messages()) n += m.find(what) != std::string::npos ? 1u : 0u;
        return n;
    };
    // L1P4_LUA_GAPS 1.1: bad arguments are script errors and fire nothing (more than one argument, an unknown type name,
    // a number, an explicit nil); an ability name is not a type.
    CHECK(run(vm, "UFleet.Activate_Ability(ft, ft) UFleet.Activate_Ability('UNUSED_NAME') H1.Activate_Ability('UNUSED_NAME') "
                  "UFleet.Activate_Ability(5) UFleet.Activate_Ability(nil)") == "");
    CHECK(g.store->alive(h[0]) && g.store->alive(h[1]) && g.store->alive(h[2]) && sim.respawns_pending(0).empty());
    CHECK(messages_with("Expected 0 or 1, got 2") == 1 && messages_with("unknown object type UNUSED_NAME") == 2 &&
          messages_with("Expected object type.") == 2);
    // The fleet with a type filter (wrapper, or a type name in any case) that matches no saboteur: nothing; then the unit
    // itself with the same filter, which the unit branch does not apply: the halt applies, the hero departs (one synced
    // draw, respawn queued at A).
    CHECK(run(vm, "UFleet.Activate_Ability(ft) UFleet.Activate_Ability('frigate_t')") == "" && g.store->alive(h[0]));
    const SimRng before = g.sim_rng;
    CHECK(run(vm, "H1.Activate_Ability('Frigate_T')") == "");
    CHECK(!g.store->alive(h[0]) && sim.planets()[0].halt_until > halted_before && sim.respawns_pending(0).size() == 1 &&
          !(g.sim_rng == before));

    // Task forces: Land_Units / Activate_Ability only forward to the AI host, which fires the trigger itself (engine/ai):
    // the binding adds no second fire. Galactic Activate_Ability passes the argument's type name (any case in, the
    // type's own name out; "" without one) and calls nothing on a bad argument.
    TriggerAi ai;
    attach_ai_host(vm, &ai, 1);
    set_task_force_global(vm, "TF", 3, AiTaskForceKind::Galactic);
    ai.units = {h[1], h[2]};
    CHECK(run(vm, "st = TF.Land_Units() sf = st.IsFinished() tfr = TF.Activate_Ability() tfn = (tfr == nil) "
                  "TF.Activate_Ability('halter_t') TF.Activate_Ability('UNUSED_NAME') TF.Activate_Ability(ft, true)") == "" &&
          gtrue(vm, "sf") && gtrue(vm, "tfn"));  // refix t04: no return value (L1P4_LUA_GAPS 1.2 item 4)
    CHECK(ai.orders.size() == 3 && ai.orders[0] == AiTfOrder::LandUnits && ai.orders[1] == AiTfOrder::ActivateAbility &&
          ai.orders[2] == AiTfOrder::ActivateAbility && ai.abilities.size() == 2 && ai.abilities[0].empty() &&
          ai.abilities[1] == "Halter_T" && g.store->alive(h[1]) && g.store->alive(h[2]) && sim.respawns_pending(0).size() == 1);
    attach_ai_host(vm, nullptr, 0);
    // The fleet itself with a matching type name: every carried unit of that type (content order) tries its abilities -
    // both remaining saboteurs apply.
    CHECK(run(vm, "UFleet.Activate_Ability('HALTER_T')") == "");
    CHECK(!g.store->alive(h[1]) && !g.store->alive(h[2]) && sim.respawns_pending(0).size() == 3);
    // A landed unit (on a surface) fires nothing ("is landed").
    vm.set_object_global("Landed", sim.planets()[0].surface.front());
    CHECK(run(vm, "Landed.Activate_Ability()") == "" && messages_with("is landed") == 1);

    std::vector<galactic::GalacticEvent> evs;
    sim.drain_events(evs);
    const auto applied = std::count_if(evs.begin(), evs.end(), [](const galactic::GalacticEvent& e) {
        return e.kind == galactic::GalacticEventKind::SpecialApplied && e.planet == 0 && e.player == 1;
    });
    CHECK(applied == 3);
    CHECK(vm.errors().empty() && tvm.errors().empty());

    StateHasher hs;
    g.world.hash(hs);
    sim.hash_state(hs);
    hs.u64(static_cast<uint64_t>(applied));
    hs.i32(g.sim_rng.range_int(0, 1000000));
    hs.i32(g.rng.synced().range_int(0, 1000000));
    return hs.digest();
}

void test_special_systems() {
    const uint64_t a = special_session();
    const uint64_t b = special_session();
    uint64_t c = 0;
    std::thread t([&c] { c = special_session(); });
    t.join();
    CHECK(a != 0 && a == b && a == c);
    std::printf("  P4 special systems: session digest %016llx (x3 identical: %s)\n", static_cast<unsigned long long>(a),
                (a == b && a == c) ? "yes" : "NO");
}

// refix t04 (L1P4_LUA_GAPS 1.3, STORY_TUTORIAL_M04_GALACTIC.LUA): Find_All_Objects_Of_Type(member type) gives company
// member views; a view's Get_Parent_Object() is the company (the original's transport), whose Activate_Ability fires only
// that company, not the rest of its fleet. The fleet's type filter matches representative types (company type), not
// member types. A view's own Activate_Ability does nothing (a unit inside a transport).
void test_company_member_views() {
    Galaxy g({{0, 0, "Halter_T"}, {0, 0, "Smuggler_Team_T"}}, [](galactic::GalacticRules& r, const objects::ObjectTypeDatabase& raw) {
        tune_special(r, raw);
        const uint32_t team = raw.find("Smuggler_Team_T");
        r.special.types[team].abilities = {0};  // the port's company carries its members' abilities
        r.special.types[team].generic_hero = true;
    });
    galactic::GalacticSim& sim = *g.sim;
    const uint32_t halter = g.raw.find("Halter_T"), team = g.raw.find("Smuggler_Team_T");
    objects::ObjectHandle hal{}, comp{};
    for (const objects::ObjectHandle u : sim.planets()[0].surface) {
        if (g.store->type(u) == halter) hal = u;
        if (g.store->type(u) == team) comp = u;
    }
    CHECK(static_cast<bool>(hal) && static_cast<bool>(comp));
    if (!hal || !comp) return;
    const objects::ObjectHandle launch[] = {hal, comp};
    CHECK(sim.launch_units(0, 0, launch) == commands::RejectReason::None);
    CHECK(sim.change_owner(hal, 1) && sim.change_owner(comp, 1));
    const uint32_t f = sim.fleet_containing(comp);
    CHECK(f != galactic::kNoFleet && f == sim.fleet_containing(hal) && sim.fleets()[f].planet == 0);
    if (f == galactic::kNoFleet) return;
    CHECK(sim.representative_type(comp) == team && sim.bm_give_item(1, "Halt_Tech"));

    ObjectBridge bridge;  // as StorySession wires it
    bridge.container_of = [&sim](objects::ObjectHandle h) { return sim.container_of(h); };
    bridge.contents_of = [&sim](objects::ObjectHandle h) { return sim.contents_of(h); };
    bridge.destroy_object = [&sim](objects::ObjectHandle h) { return sim.destroy_object(h); };
    LuaHost host = g.host();
    host.campaign = true;
    LuaVm vm(host);
    attach_player_world(vm, &g.world);
    attach_object_bridge(vm, &bridge);
    vm.set_object_global("Comp", comp);
    vm.set_object_global("Fleet", sim.fleets()[f].object);
    // The M04 lookup: views per matching Company_Units entry (entries 0 and 2), cached (same userdata every time),
    // typed as the member, distinct from the company's own wrapper; Find_First_Object finds the first entry.
    CHECK(run(vm, "list = Find_All_Objects_Of_Type('SMUGGLER_T') n = table.getn(list) sm = list[1] "
                  "same = rawequal(Find_All_Objects_Of_Type('Smuggler_T')[1], sm) and rawequal(Find_First_Object('smuggler_t'), sm) "
                  "two = not rawequal(list[2], sm) tn = sm.Get_Type().Get_Name() crew = Find_First_Object('Crew_T') "
                  "cn = crew.Get_Type().Get_Name() ncrew = table.getn(Find_All_Objects_Of_Type('Crew_T')) "
                  "differs = not (sm == Comp) and not rawequal(sm, Comp) valid = sm.Is_Valid() "
                  "nemp = table.getn(Find_All_Objects_Of_Type('Smuggler_T', Find_Player('Empire'))) "
                  "ct = Comp.Get_Type().Get_Name() pa = Comp.Get_Planet_Location() pl = pa ~= nil and rawequal(sm.Get_Planet_Location(), pa) "
                  "smuggler_transport = sm.Get_Parent_Object() isc = rawequal(smuggler_transport, Comp) "
                  "pf = rawequal(smuggler_transport.Get_Parent_Object(), Fleet) ts = tostring(sm)") == "");
    CHECK(static_cast<int>(gnum(vm, "n")) == 2 && gtrue(vm, "same") && gtrue(vm, "two") && gstr(vm, "tn") == "Smuggler_T");
    CHECK(gstr(vm, "cn") == "Crew_T" && static_cast<int>(gnum(vm, "ncrew")) == 1 && gtrue(vm, "differs") && gtrue(vm, "valid"));
    CHECK(static_cast<int>(gnum(vm, "nemp")) == 0 && gstr(vm, "ct") == "Smuggler_Team_T" && gtrue(vm, "pl"));
    CHECK(gtrue(vm, "isc") && gtrue(vm, "pf") && gstr(vm, "ts").find("Smuggler_T") != std::string::npos);
    // refixf t03: every filter kind sees the member views (the original's cargo objects): a player alone lists the
    // company followed by its matching entries (company first, entry order).
    CHECK(run(vm, "own = Find_All_Objects_Of_Type(Comp.Get_Owner()) ci = 0 vi = 0 "
                  "for i, o in ipairs(own) do if rawequal(o, Comp) then ci = i elseif rawequal(o, sm) then vi = i end end") == "");
    CHECK(gnum(vm, "ci") >= 1.0 && gnum(vm, "vi") == gnum(vm, "ci") + 1.0);
    // Nothing fires: the view itself (inside a transport: silently), the fleet filtered by a member type (the members'
    // representative types are Halter_T and Smuggler_Team_T).
    CHECK(run(vm, "sm.Activate_Ability() Fleet.Activate_Ability('Smuggler_T')") == "");
    CHECK(g.store->alive(comp) && g.store->alive(hal) && sim.respawns_pending(0).empty());
    // smuggler_transport.Activate_Ability(): only the company tries its abilities (the halt applies, the hero departs);
    // the halter in the same fleet, which has the same ability, does not fire.
    CHECK(run(vm, "r = smuggler_transport.Activate_Ability() rn = (r == nil) gone = not sm.Is_Valid()") == "");
    CHECK(gtrue(vm, "rn") && gtrue(vm, "gone") && !g.store->alive(comp) && g.store->alive(hal) &&
          sim.respawns_pending(0).size() == 1);
    CHECK(vm.errors().empty());
    for (const std::string& m : vm.messages())
        if (m.find("SCRIPT ERROR") != std::string::npos) {
            std::printf("  member views: unexpected %s\n", m.c_str());
            ++g_fail;
        }
}

// a1g t06: Spawn_Unit at a galactic planet goes through GalacticSim::spawn_unit, so the units live in the sim like
// produced ones (space units in the owner's orbit fleet, ground units and companies on the surface, one object per
// company), the shipped SpawnList + Assemble_Fleet pattern (Story_Campaign_Underworld.lua, S10 PROBE A) gives a fleet
// (landed units are launched into it) and the fleet's Move_To flies it. The sim state matches a direct
// GalacticSim::spawn_unit of the same units (save / CRC see them the same way).
void test_spawn_assemble() {
    Galaxy g;
    galactic::GalacticSim& sim = *g.sim;
    ObjectBridge bridge;  // as StorySession wires it
    bridge.container_of = [&sim](objects::ObjectHandle h) { return sim.container_of(h); };
    bridge.contents_of = [&sim](objects::ObjectHandle h) { return sim.contents_of(h); };
    commands::CommandPipeline pipe;
    std::vector<commands::RejectReason> results;
    pipe.set_handler([&sim, &results](uint64_t t, const commands::Command& c) {
        results.push_back(sim.handle_command(t, c));
        return results.back();
    });
    bridge.pipeline = &pipe;
    LuaVm vm(g.host());
    attach_player_world(vm, &g.world);
    attach_object_bridge(vm, &bridge);
    vm.set_object_global("PA", g.planet(0));
    vm.set_object_global("PB", g.planet(1));
    const uint32_t frigate = g.raw.find("Frigate_T"), trooper = g.raw.find("Trooper_T"), team = g.raw.find("Smuggler_Team_T");
    auto live_objects = [&g] {
        size_t n = 0;
        for (const objects::ObjectHandle h : g.store->all_objects()) n += g.store->alive(h) ? 1u : 0u;
        return n;
    };
    const size_t before = live_objects();
    const std::vector<uint32_t> fa = sim.fleets_at(0, 0);
    CHECK(fa.size() == 1 && sim.planets()[0].surface.size() == 1);
    if (fa.size() != 1) return;
    const uint32_t home = fa[0];
    CHECK(run(vm, "emp = Find_Player('Empire') "
                  "space = Spawn_Unit(Find_Object_Type('Frigate_T'), PA, emp) "
                  "ground = Spawn_Unit(Find_Object_Type('Trooper_T'), PA, emp) "
                  "comp = Spawn_Unit(Find_Object_Type('Smuggler_Team_T'), PA, emp) "
                  "n = table.getn(space) + table.getn(ground) + table.getn(comp) "
                  "loc = space[1].Get_Planet_Location() == PA and ground[1].Get_Planet_Location() == PA and "
                  "comp[1].Get_Planet_Location() == PA "
                  "gp = ground[1].Get_Parent_Object() == PA") == "");
    CHECK(gnum(vm, "n") == 3.0 && gtrue(vm, "loc") && gtrue(vm, "gp"));
    // Registered with the sim: the frigate joined the Empire's orbit fleet at A, the trooper and the company (one object,
    // no member objects) landed on A's surface.
    CHECK(live_objects() == before + 3);
    CHECK(sim.fleets()[home].contents.size() == 2 && g.store->type(sim.fleets()[home].contents[1]) == frigate);
    const std::vector<objects::ObjectHandle> surface = sim.planets()[0].surface;
    CHECK(surface.size() == 3 && g.store->type(surface[1]) == trooper && g.store->type(surface[2]) == team);
    if (surface.size() != 3) return;

    // SpawnList + Assemble_Fleet of the ground units: a new fleet at A carrying both, launched off the surface.
    CHECK(run(vm, "units = {} for _, u in ipairs(ground) do table.insert(units, u) end "
                  "for _, u in ipairs(comp) do table.insert(units, u) end "
                  "lf = Assemble_Fleet(units) lok = lf ~= nil ln = lok and lf.Get_Contained_Object_Count() "
                  "lp = lok and lf.Get_Planet_Location() == PA lg = lok and ground[1].Get_Parent_Object() == lf "
                  "sf = Assemble_Fleet(space) sok = sf ~= nil and sf ~= lf sn = sok and sf.Get_Contained_Object_Count()") == "");
    CHECK(gtrue(vm, "lok") && gnum(vm, "ln") == 2.0 && gtrue(vm, "lp") && gtrue(vm, "lg"));
    CHECK(gtrue(vm, "sok") && gnum(vm, "sn") == 1.0);
    const uint32_t lf = sim.fleet_containing(surface[1]);
    CHECK(lf != galactic::kNoFleet && lf == sim.fleet_containing(surface[2]) && sim.planets()[0].surface.size() == 1);
    CHECK(sim.fleets()[home].alive && sim.fleets()[home].contents.size() == 1 && sim.fleets_at(0, 0).size() == 3);
    if (lf == galactic::kNoFleet) return;

    // The fleet flies: Move_To(B) is a MoveObjectToObject order the sim executes; it arrives in orbit of B.
    // (B is unrevealed for the human Empire in this galaxy: the human hop gate would refuse the path.)
    sim.reveal_all_planets();
    CHECK(run(vm, "blk = lf.Move_To(PB)") == "");
    CHECK(results.size() == 1 && results[0] == commands::RejectReason::None && sim.fleets()[lf].moving);
    for (int t = 0; t < 3000 && sim.fleets()[lf].moving; ++t) sim.update();
    CHECK(!sim.fleets()[lf].moving && sim.fleets()[lf].planet == 1);
    CHECK(run(vm, "arrived = lf.Get_Planet_Location() == PB") == "" && gtrue(vm, "arrived"));
    CHECK(vm.errors().empty());

    // The same spawns made directly by the sim give the same sim state (fleets, surface lists, store).
    Galaxy lua_side, direct;
    {
        LuaVm v2(lua_side.host());
        attach_player_world(v2, &lua_side.world);
        v2.set_object_global("PA", lua_side.planet(0));
        CHECK(run(v2, "emp = Find_Player('Empire') Spawn_Unit(Find_Object_Type('Frigate_T'), PA, emp) "
                      "Spawn_Unit(Find_Object_Type('Trooper_T'), PA, emp) Spawn_Unit(Find_Object_Type('Smuggler_Team_T'), PA, emp)") == "");
    }
    for (const uint32_t t : {frigate, trooper, team}) CHECK(static_cast<bool>(direct.sim->spawn_unit(0, 0, t)));
    StateHasher hl, hd;
    lua_side.sim->hash_state(hl);
    direct.sim->hash_state(hd);
    CHECK(hl.digest() == hd.digest());
}

}  // namespace

int main() {
    test_players();
    test_spawn_assemble();  // a1g t06
    test_special_systems();
    test_company_member_views();  // refix t04
    test_perception();
    test_shipped_equations();
    test_determinism();
    test_sliceable_unlock();  // OI lua
    test_setup_players();
    test_galactic_ops();
    const BindingCoverage cov = binding_coverage();
    std::printf("  coverage: globals implemented %u/%u, methods implemented %u/%u, RE call sites %llu/%llu\n",
                cov.globals_implemented, cov.globals, cov.methods_implemented, cov.methods,
                static_cast<unsigned long long>(cov.calls_implemented), static_cast<unsigned long long>(cov.calls_total));
    // This family's share: table rows implemented by impl_players() (no duplicates with other families).
    uint32_t fam_rows = 0;
    uint64_t fam_calls = 0;
    for (const BindingInfo& b : binding_table())
        for (const detail::ImplEntry& e : detail::impl_players())
            if (e.kind == b.kind && std::strcmp(e.name, b.name) == 0 &&
                (b.kind == BindingKind::Global || std::strcmp(e.cls, b.cls) == 0)) {
                ++fam_rows;
                fam_calls += b.calls;
            }
    CHECK(fam_rows == detail::impl_players().size());
    for (const detail::ImplEntry& e : detail::impl_players()) {
        int same = 0;
        for (const detail::ImplEntry& o : detail::implementations())
            if (o.kind == e.kind && std::strcmp(o.name, e.name) == 0 && std::strcmp(o.cls, e.cls) == 0) ++same;
        if (same != 1) {
            std::printf("  DUPLICATE binding %s.%s (%d implementations)\n", e.cls, e.name, same);
            ++g_fail;
        }
    }
    std::printf("  players family: %u bindings, %llu RE call sites\n", fam_rows, static_cast<unsigned long long>(fam_calls));
    if (g_fail) {
        std::printf("alamo_lua_players_tests: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("alamo_lua_players_tests: OK\n");
    return 0;
}

#else  // !ALAMO_LUA_PLAYERS

int main() {
    std::printf("alamo_lua_players_tests: skipped (players family needs galactic_sim + ai modules)\n");
    return g_fail;
}

#endif

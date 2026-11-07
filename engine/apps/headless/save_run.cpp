// alamo_headless N1 save / load (owner: n1-host): see save_run.hpp.
#include "save_run.hpp"
#include "crc_out.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/commands/recording.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/save_file.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/save_session.hpp"
#include "alamo/sim/sim_host.hpp"
#include "skirmish_run.hpp"  // N1 stage 2: the skirmish session
#ifdef ALAMO_SIM_HAVE_BATTLES
#include "alamo/sim/campaign_battles.hpp"
#endif
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/sim/sim_anim.hpp"
#endif
#ifdef ALAMO_SIM_HAVE_STORY
#include "alamo/sim/story_session.hpp"
#endif

#if defined(ALAMO_HEADLESS_HAVE_GALACTIC_AI) && defined(ALAMO_SIM_HAVE_BATTLES)
#define ALAMO_SAVE_RUN_GALACTIC 1
#include "alamo/ai/ai_definitions.hpp"
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/galactic_ai.hpp"
#include "alamo/ai/perception_equations.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/sim/campaign_battles.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
#include "alamo/sim/battle_ai.hpp"  // N1 stage 2: TacticalAiSetup (--battles fight-all)
#endif
#endif

namespace {

using namespace alamo;

std::string env_text(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

std::string join(const std::vector<std::string>& v, const char* sep = ",") {
    std::string out;
    for (const std::string& s : v) out += (out.empty() ? "" : sep) + s;
    return out;
}

// In-memory Data\ tree (the synthetic session needs no files on disk).
class MemoryFiles final : public FileProvider {
public:
    void add(std::string_view path, std::string_view text) {
        files_[normalize_path(path)] = std::vector<uint8_t>(text.begin(), text.end());
    }
    bool read(std::string_view path, std::vector<uint8_t>& out) const override {
        const auto it = files_.find(normalize_path(path));
        if (it == files_.end()) {
            out.clear();
            return false;
        }
        out = it->second;
        return true;
    }
    bool exists(std::string_view path) const override { return files_.count(normalize_path(path)) != 0; }
    std::string origin(std::string_view path) const override { return exists(path) ? "memory:" + std::string(path) : std::string(); }
    uint64_t digest() const {
        StateHasher h;
        h.u32(static_cast<uint32_t>(files_.size()));
        for (const auto& [name, bytes] : files_) {
            h.str(name);
            h.u32(static_cast<uint32_t>(bytes.size()));
            h.bytes(bytes.data(), bytes.size());
        }
        return h.digest();
    }

private:
    std::map<std::string, std::vector<uint8_t>> files_;
};

// ------------------------------------------------------------------------------------------------- sessions
// One running session. Member order = destruction order reversed: the AI world outlives the host (the host's CRC
// registry and controllers capture it), the battles glue detaches from the host before the host dies.
struct Session {
    std::unique_ptr<jobs::JobSystem> js;
#ifdef ALAMO_SAVE_RUN_GALACTIC
    std::unique_ptr<ai::GalacticAiWorld> world;
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    std::vector<std::vector<ai::tactical::TacticalBudgetCategory>> budget_rows;
#endif
#endif
    std::unique_ptr<sim::SimHost> host;
#ifdef ALAMO_SIM_HAVE_ANIM
    std::unique_ptr<sim::SimAnimation> anim;  // N1 stage 2: --battles ...+anim (before the battles glue that points at it)
#endif
#ifdef ALAMO_SIM_HAVE_BATTLES
    std::unique_ptr<sim::CampaignBattles> battles;
#endif
    SkirmishSaveRun skirmish;          // N1 stage 2: the skirmish session's glue
    std::function<void()> after_tick;  // harness step between ticks (skirmish: the land invasion)
    galactic::GalacticSim* gs = nullptr;
    bool tactical_top() const { return host->modes().depth() > 1; }
    // OPT3 t04 (--save-when scheduled): AI background jobs in flight (galactic and battle path finders).
    size_t ai_jobs_in_flight() const {
        size_t n = 0;
#ifdef ALAMO_SAVE_RUN_GALACTIC
        if (world) n += world->background_jobs_in_flight();
#endif
#if defined(ALAMO_SIM_HAVE_BATTLES) && defined(ALAMO_SIM_HAVE_AI_TACTICAL)
        const sim::CampaignBattles* b = battles ? battles.get() : skirmish.battles.get();
        if (b && b->tactical_ai()) n += b->tactical_ai()->world().background_jobs_in_flight();
#endif
        return n;
    }
    // A conflict queued / being decided on the galaxy, no battle running (--save-when pending).
    bool conflict_pending() const {
#ifdef ALAMO_SIM_HAVE_BATTLES
        const sim::CampaignBattles* b = battles ? battles.get() : skirmish.battles.get();
        if (!b || tactical_top()) return false;
        const galactic::ConflictManager& cm = b->conflicts();
        return cm.state() != galactic::ConflictState::Idle || !cm.space_queue().empty() || !cm.land_queue().empty();
#else
        return false;
#endif
    }
};

// N1 stage 2: what a harness builds beyond its kind (recorded in the descriptor's options; --load reads them back).
struct HarnessOptions {
    std::string campaign;  // galactic: the campaign; skirmish: synthetic | foc | foc-land
    std::string game_dir;
    std::vector<std::string> game_args;
    std::string battles = "auto";  // auto | fight | fight-all
    bool anim = false;
    bool story = false;
    std::string map;  // skirmish
};

class Harness {
public:
    virtual ~Harness() = default;
    // A new-game session (no tick run yet) with this seed, its descriptor set (SimHost::set_session).
    virtual std::unique_ptr<Session> build(uint32_t seed, int32_t workers, std::string& err) = 0;
};

sim::SessionPlayer session_player(const galactic::GalacticRules& rules, const galactic::PlayerSetup& ps, std::string ai_type) {
    sim::SessionPlayer p;
    p.faction = ps.faction < rules.factions.size() ? rules.factions[ps.faction].name : std::string();
    p.human = ps.human;
    p.ai = ps.ai;
    p.ai_type = std::move(ai_type);
    p.difficulty = ps.difficulty < rules.difficulties.size() ? rules.difficulties[ps.difficulty].name : std::string();
    return p;
}

// ------------------------------------------------------------------------------------------- synthetic
namespace syn {
using namespace alamo::galactic;
enum : uint32_t { tPA, tPB, tPC, tPD, tSB1, tCorvette, tInfantry, tBarracks, tFighters, tCount };
enum : PlanetIndex { rCore = 0, rA = 1, rB = 2, rC = 3, rD = 4 };
enum : int32_t { pEmpire = 0, pUnder = 1, pNeutral = 2 };

// Object types in the GalacticRules order below (the galactic store's objects carry rules type indices, which the
// store's save checks against the GameObjectTypes, as with real data where the rules are built from those types).
const char* const kTypesXml =
    "<SaveTest>\n"
    " <Planet Name=\"PlanetA\"><Behavior>PLANET</Behavior></Planet>\n"
    " <Planet Name=\"PlanetB\"><Behavior>PLANET</Behavior></Planet>\n"
    " <Planet Name=\"PlanetC\"><Behavior>PLANET</Behavior></Planet>\n"
    " <Planet Name=\"PlanetD\"><Behavior>PLANET</Behavior></Planet>\n"
    " <StarBase Name=\"SB1\"/>\n"
    " <SpaceUnit Name=\"Corvette\"><Lua_Script>Save_Test_Object</Lua_Script></SpaceUnit>\n"
    " <GroundInfantry Name=\"Infantry\"/>\n"
    " <GroundStructure Name=\"Barracks\"/>\n"
    " <SpaceUnit Name=\"Fighters\"><Lua_Script>Save_Test_Object</Lua_Script></SpaceUnit>\n"
    "</SaveTest>\n";
const char* const kPlots =
    "<Story_Mode_Plots><Lua_Script>Story_Save_Test</Lua_Script><Active_Plot>Story_Save_Test.xml</Active_Plot>"
    "<Suspended_Plot /></Story_Mode_Plots>\n";
// [name-ok-begin: synthetic save-test data of the headless test host (factions below), not a rule]
const char* const kPlot =
    "<Story>\n"
    " <Event Name=\"Begin\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>0</Event_Param1></Event>\n"
    " <Event Name=\"Raise_Tech\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>SET_TECH_LEVEL</Reward_Type>"
    "<Reward_Param1>Empire</Reward_Param1><Reward_Param2>2</Reward_Param2><Prereq>Begin</Prereq></Event>\n"
    " <Event Name=\"Tech_Seen\"><Event_Type>STORY_TECH_LEVEL</Event_Type><Event_Param1>2</Event_Param1>"
    "<Prereq>Raise_Tech</Prereq></Event>\n"
    " <Event Name=\"Script_Ack\"><Event_Type>STORY_AI_NOTIFICATION</Event_Type><Event_Param2>SCRIPT_SAW_TECH</Event_Param2>"
    "<Reward_Type>SET_FLAG</Reward_Type><Reward_Param1>Acked</Reward_Param1><Reward_Param2>1</Reward_Param2></Event>\n"
    " <Event Name=\"Enemy_Takes_C\"><Event_Type>STORY_CONQUER</Event_Type><Event_Param1>PlanetC</Event_Param1>"
    "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3><Prereq>Begin</Prereq></Event>\n"
    " <Event Name=\"Enemy_Takes_B\"><Event_Type>STORY_CONQUER</Event_Type><Event_Param1>PlanetB</Event_Param1>"
    "<Event_Param3>FILTER_ENEMY_ONLY</Event_Param3><Prereq>Enemy_Takes_C</Prereq></Event>\n"
    " <Event Name=\"Corvette_At_C\"><Event_Type>STORY_ENTER</Event_Type><Event_Param1>PlanetC</Event_Param1>"
    "<Event_Param3>Corvette</Event_Param3></Event>\n"
    " <Event Name=\"Landed_C\"><Event_Type>STORY_LAND_ON</Event_Type><Event_Param1>PlanetC</Event_Param1></Event>\n"
    " <Event Name=\"Four_Fighters\"><Event_Type>STORY_CONSTRUCT</Event_Type><Event_Param1>Fighters</Event_Param1>"
    "<Event_Param2>4</Event_Param2><Event_Param3>FILTER_FRIENDLY_ONLY</Event_Param3></Event>\n"
    " <Event Name=\"Bonus\"><Event_Type>STORY_TRIGGER</Event_Type><Reward_Type>CREDITS</Reward_Type>"
    "<Reward_Param1>1000</Reward_Param1><Prereq>Four_Fighters</Prereq></Event>\n"
    " <Event Name=\"Ten_Seconds\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>10</Event_Param1>"
    "<Prereq>Begin</Prereq></Event>\n"
    " <Event Name=\"Twenty_Seconds\"><Event_Type>STORY_ELAPSED</Event_Type><Event_Param1>10</Event_Param1>"
    "<Prereq>Ten_Seconds</Prereq></Event>\n"
    " <Event Name=\"Ack_Flag\"><Event_Type>STORY_FLAG</Event_Type><Event_Param1>Acked</Event_Param1>"
    "<Event_Param2>1</Event_Param2></Event>\n"
    "</Story>\n";
// Script state across the save: a service counter (global), a coroutine (main's loop) and a GlobalValue; q6 t19: a
// thread sleeping on GetCurrentTime (PGBase Sleep inline), a thread waiting for an event the story delivers later
// (Ten_Seconds / Twenty_Seconds), a closure counter (upvalue), a cycle and a shared table. Every GlobalValue write is in
// the frame CRC.
const char* const kScript =
    "local function make_counter() local c = 0 return function() c = c + 1 return c end end\n"
    "bump = make_counter()\n"
    "function Base_Definitions() ServiceRate = 0.5 services = 0 seen = {} ring = {} ring.next = {back = ring} "
    "graph = {a = seen, b = seen, ring = ring} end\n"
    "function Story_Event_Trigger(name) seen[name] = services if name == \"Tech_Seen\" then Story_Event(\"SCRIPT_SAW_TECH\") end end\n"
    "function Sleeper() while true do local s = GetCurrentTime() while GetCurrentTime() - s < 3 do coroutine.yield(true) end "
    "GlobalValue.Set(\"Save_Test_Sleeps\", bump()) end end\n"
    "function Waiter() while not graph.b[\"Twenty_Seconds\"] do coroutine.yield(true) end "
    "GlobalValue.Set(\"Save_Test_Waited\", services .. \"/\" .. tostring(graph.a == seen) .. \"/\" .. "
    "tostring(graph.ring.next.back == ring)) end\n"
    "function main() Create_Thread(\"Sleeper\") Create_Thread(\"Waiter\") while true do services = services + 1 "
    "GlobalValue.Set(\"Save_Test_Services\", services) coroutine.yield(true) end end\n";
// q6 t19: the GameScoring script (SimHost::set_scoring_script; its state is the "scoring" chunk): services, the planet
// owner changes it is told about (planet wrappers kept in a table), the mode-start event.
const char* const kScoring =
    "function Base_Definitions() ServiceRate = 0.3 changes = {} count = 0 end\n"
    "function Game_Mode_Starting_Event(mode, map) GlobalValue.Set(\"Save_Test_Scoring_Mode\", mode .. \":\" .. map) end\n"
    "function Galactic_Planet_Faction_Change(planet, new_owner, old_owner) count = count + 1 changes[count] = planet "
    "GlobalValue.Set(\"Save_Test_Scoring_Changes\", count .. \"/\" .. tostring(changes[count] == planet)) end\n"
    "function main() local n = 0 while true do n = n + 1 GlobalValue.Set(\"Save_Test_Scoring_Pumps\", n) "
    "coroutine.yield(true) end end\n";
// q6 t19: a game-object script on the Corvette (attached at the galaxy setup) and the Fighters (attached as they are
// built): its state is the "object_scripts" chunk; one first-pump draw each, a GlobalValue per pump.
const char* const kObjectScript =
    "function Base_Definitions() ServiceRate = 1 pumps = 0 end\n"
    "function main() local id = Object.Get_Object_ID() local times = {} while true do pumps = pumps + 1 "
    "times[pumps] = GetCurrentTime() GlobalValue.Set(\"Save_Test_Object_\" .. id, pumps .. \"/\" .. table.getn(times)) "
    "coroutine.yield(true) end end\n";

GalacticRules rules() {
    GalacticRules r;
    FactionRules emp;
    emp.name = "Empire";
    emp.basic_ai = "BasicEmpire";
    emp.can_control_planets = emp.is_playable = true;
    FactionRules und;
    und.name = "Underworld";
    und.basic_ai = "AI_Player_Underworld";
    und.can_control_planets = und.is_playable = und.benefits_from_corruption = true;
    FactionRules neu;
    neu.name = "Neutral";
    neu.is_neutral = true;
    r.factions = {emp, und, neu};
    // [name-ok-end]
    r.difficulties = {{"Normal", 1.0f, 1.0f}};
    r.types.resize(tCount);
    const char* names[] = {"PlanetA", "PlanetB", "PlanetC", "PlanetD"};
    for (uint32_t i = 0; i < 4; ++i) {
        r.types[i].name = names[i];
        r.types[i].kind = UnitKind::Planet;
        r.types[i].credit_value = 10;
        r.types[i].capture_bonus = 100;
    }
    auto unit = [&r](uint32_t id, const char* name, UnitKind k, int32_t cost, int32_t secs) -> TypeInfo& {
        TypeInfo& t = r.types[id];
        t.name = name;
        t.kind = k;
        t.build_cost = cost;
        t.build_time_seconds = secs;
        return t;
    };
    unit(tSB1, "SB1", UnitKind::StarBase, 1000, 15).base_level = 1;
    TypeInfo& co = unit(tCorvette, "Corvette", UnitKind::SpaceUnit, 800, 10);
    co.required_star_base = 1;
    co.hyperspace = true;
    co.hyperspace_speed = 2.0f;
    co.ai_combat_power = 600.0f;
    co.squadron_capacity = 2;
    unit(tInfantry, "Infantry", UnitKind::GroundUnit, 300, 10).required_ground_base = 1;
    unit(tBarracks, "Barracks", UnitKind::GroundStructure, 600, 20);
    unit(tFighters, "Fighters", UnitKind::Squadron, 200, 2).required_star_base = 1;
    return r;
}

GalacticSetup setup() {
    GalacticSetup s;
    GalaxyMap& m = s.map;
    m.campaign_name = "SaveTest";
    const char* names[] = {"Core", "A", "B", "C", "D"};
    const Vec3 pos[] = {{500, 500, 0}, {0, 0, 0}, {60, 0, 0}, {120, 0, 0}, {60, 80, 0}};
    for (uint32_t i = 0; i < 5; ++i) {
        m.planets.add(names[i]);
        m.planets.position[i] = pos[i];
        m.planets.flags[i] = i == 0 ? 0u : static_cast<uint32_t>(kPlanetHasPlanetBehavior);
    }
    auto route = [&m](const char* n, PlanetIndex a, PlanetIndex b) {
        const uint32_t ri = m.routes.add(n);
        m.routes.point_a[ri] = a;
        m.routes.point_b[ri] = b;
        m.routes.hs_speed_factor[ri] = 8.0f;
    };
    route("A_B", rA, rB);
    route("B_C", rB, rC);
    route("B_D", rB, rD);
    s.planet_type = {kNoIndex, tPA, tPB, tPC, tPD};
    PlayerSetup e;
    e.faction = 0;
    e.human = true;
    e.credits = 5000.0f;
    PlayerSetup u;
    u.faction = 1;
    u.ai = true;
    u.difficulty = 0;
    u.credits = 5000.0f;
    PlayerSetup n;
    n.faction = 2;
    s.players = {e, u, n};
    s.forces = {{pEmpire, rA, tSB1}, {pEmpire, rA, tBarracks}, {pUnder, rD, tCorvette}, {pUnder, rD, tInfantry}};
    return s;
}

galactic::GalacticSim* galaxy_on_top(sim::SimHost& h) {
    sim::GalacticMode* g = h.galactic_mode();
    if (!g || !g->sim() || h.modes().top() != g || g->paused()) return nullptr;
    return g->sim();
}

uint32_t wire(const galactic::GalacticSim& gs, objects::ObjectHandle o) {
    return static_cast<uint32_t>(gs.store().id(o));
}

// Empire (the "human"): buys Fighters / Corvettes at A through the scheduled pipeline (SimHost::submit), so orders
// sit in the command queue across a save. Depends only on the frame and sim state.
void empire_orders(sim::SimHost& h, uint8_t player, uint64_t frame) {
    galactic::GalacticSim* gs = galaxy_on_top(h);
    if (!gs || frame % 120u != 60u) return;
    const galactic::PlanetState& a = gs->planets()[rA];
    if (a.owner != static_cast<int32_t>(player)) return;
    commands::EventData ev(commands::EventType::Production);
    ev.set("location", wire(*gs, a.object));
    ev.set("type_index", (frame / 120u) % 3u == 2u ? static_cast<uint32_t>(tCorvette) : static_cast<uint32_t>(tFighters));
    (void)h.submit(player, std::move(ev));
}

// Underworld (AI stand-in, execute_direct): ground units go up to orbit, the fleet carrying them flies to the first of
// C, B it does not own and invades it. Depends only on the frame and sim state.
void underworld_orders(sim::SimHost& h, uint8_t player, uint64_t frame) {
    galactic::GalacticSim* gs = galaxy_on_top(h);
    if (!gs || frame % 30u != 15u) return;
    const int32_t p = player;
    const auto& store = gs->store();
    const auto& types = gs->rules().types;
    auto direct = [&h, player](const commands::EventData& ev) { (void)h.execute_direct(player, commands::CommandSource::AI, ev); };
    PlanetIndex target = kInvalidPlanet;
    for (const PlanetIndex t : {rC, rB})
        if (gs->planets()[t].owner != p) {
            target = t;
            break;
        }
    if (target == kInvalidPlanet) return;
    for (const PlanetIndex at : {rD, rC, rB}) {  // own ground units still on a surface: launch them
        if (gs->planets()[at].owner != p || at == target) continue;
        const std::vector<objects::ObjectHandle> surface = gs->planets()[at].surface;
        bool launched = false;
        for (const objects::ObjectHandle u : surface)
            if (store.alive(u) && store.owner(u) == p && types[store.type(u)].kind == UnitKind::GroundUnit) {
                direct(galactic::fleet_management_event(galactic::kFleetLaunch, commands::kNoObject, commands::kNoObject, wire(*gs, u)));
                launched = true;
            }
        if (launched) return;
    }
    for (uint32_t fi = 0; fi < gs->fleets().size(); ++fi) {
        const galactic::FleetState& fl = gs->fleets()[fi];
        if (!fl.alive || fl.owner != p || fl.moving || fl.planet == kInvalidPlanet) continue;
        bool ground = false;
        for (const objects::ObjectHandle m : fl.contents)
            ground = ground || (store.alive(m) && types[store.type(m)].kind == UnitKind::GroundUnit);
        if (!ground) continue;
        if (fl.planet == target) {
            commands::EventData ev(commands::EventType::Invade);
            ev.set("planet", wire(*gs, gs->planets()[target].object));
            direct(ev);
        } else {
            commands::EventData ev(commands::EventType::MoveObjectToObject);
            ev.set("object", wire(*gs, fl.object));
            ev.set("target", wire(*gs, gs->planets()[target].object));
            direct(ev);
        }
        return;
    }
}
}  // namespace syn

class SyntheticHarness final : public Harness {
public:
    explicit SyntheticHarness(bool story) : story_(story && sim::SimHost::story_supported()) {}
    bool init(std::string& err) {
        files_.add("Data\\XML\\GameObjectFiles.xml", "<GameObjectFiles><File>SaveTest.xml</File></GameObjectFiles>\n");
        files_.add("Data\\XML\\SaveTest.xml", syn::kTypesXml);
        files_.add("Data\\XML\\Story_Plots_Save_Test.xml", syn::kPlots);
        files_.add("Data\\XML\\Story_Save_Test.xml", syn::kPlot);
        files_.add("Data\\Scripts\\Story\\Story_Save_Test.lua", syn::kScript);
        files_.add("Data\\Scripts\\Miscellaneous\\Save_Test_Scoring.lua", syn::kScoring);    // q6 t19
        files_.add("Data\\Scripts\\GameObject\\Save_Test_Object.lua", syn::kObjectScript);  // q6 t19
        if (!db_.load(files_) || !types_.build(db_)) {
            err = "synthetic: object types did not build";
            return false;
        }
        rules_ = syn::rules();
        setup_ = syn::setup();
        fingerprint_ = sim::combine_fingerprints({sim::fingerprint_types(types_), sim::fingerprint_galactic(rules_, setup_), files_.digest()});
        return true;
    }

    std::unique_ptr<Session> build(uint32_t seed, int32_t workers, std::string& err) override {
        auto s = std::make_unique<Session>();
        s->js = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{workers});
        sim::SimConfig cfg;
        cfg.seed = seed;
        cfg.story_crc = story_;
        cfg.players = sim::session_players(setup_.players.size());
        cfg.object_scripts = story_;  // q6 t19: the story session also runs a GameScoring and game-object scripts
        s->host = std::make_unique<sim::SimHost>(types_, *s->js, cfg);
        sim::SimHost& host = *s->host;
        if (story_) {
            host.set_scoring_script(&files_, "Save_Test_Scoring");
            host.set_object_script_files(&files_);
        }
        s->gs = host.start_galactic(rules_, setup_);
        if (!s->gs) {
            err = "synthetic: start_galactic failed";
            return nullptr;
        }
        if (story_) {
            sim::StoryStart st;
            st.files = &files_;
            st.plot_sets = {{syn::pEmpire, "Story_Plots_Save_Test.xml"}};
            std::vector<std::string> diag;
            if (!host.start_story(st, &diag) || !host.story()) {
                err = "synthetic: start_story failed: " + join(diag, "; ");
                return nullptr;
            }
        }
        host.set_controller(static_cast<uint8_t>(syn::pEmpire), syn::empire_orders);
        host.set_controller(static_cast<uint8_t>(syn::pUnder), syn::underworld_orders);
        sim::SessionDescriptor d;
        d.kind = "synthetic";
        d.campaign = setup_.map.campaign_name;
        for (const galactic::PlayerSetup& ps : setup_.players) d.setup.push_back(session_player(rules_, ps, ps.ai ? "stand-in" : ""));
        d.set_option("story", story_ ? "1" : "0");
        d.set_option("scripts", story_ ? "scoring,object" : "none");  // q6 t19
        d.set_option("orders", "empire-production,underworld-invade");  // [name-ok: label of the test host's scripted orders]
        d.data_fingerprint = fingerprint_;
        host.set_session(std::move(d));
        return s;
    }

private:
    bool story_ = true;
    MemoryFiles files_;
    objects::ObjectTypeDatabase db_;
    objects::GameObjectTypes types_;
    galactic::GalacticRules rules_;
    galactic::GalacticSetup setup_;
    uint64_t fingerprint_ = 0;
};

// ------------------------------------------------------------------------------------------- galactic (FoC data)
#ifdef ALAMO_SAVE_RUN_GALACTIC
class GalacticHarness final : public Harness {
public:
    // 1 = loaded, 0 = no data (skip), -1 = error.
    int init(const std::string& game_dir, const std::vector<std::string>& game_args, const std::string& campaign, const HarnessOptions& ho,
             std::string& err) {
        ho_ = ho;
        if (ho_.battles != "auto" && ho_.battles != "fight" && ho_.battles != "fight-all") {
            err = "galactic: --battles " + ho_.battles + " (auto | fight | fight-all[+anim])";
            return -1;
        }
#ifndef ALAMO_SIM_HAVE_ANIM
        if (ho_.anim) {
            err = "galactic: this build has no animation (engine/anim)";
            return -1;
        }
#endif
        if (ho_.story && !sim::SimHost::story_supported()) {
            err = "galactic: --story needs engine/lua";
            return -1;
        }
        root_ = game_dir.empty() ? env_text("ALAMO_FOC_DATA") : game_dir;
        if (root_.empty()) return 0;
        args_ = game_args;
        vfs::VfsConfig vcfg;
        vcfg.foc_root = root_;
        if (game_dir.empty()) {  // as ai_run.cpp: the EaW tier from ALAMO_EAW_DATA on the environment route
            const std::string eaw = env_text("ALAMO_EAW_DATA");
            if (!eaw.empty()) vcfg.eaw_root = eaw;
        }
        vfs::parse_command_line(args_, vcfg);
        files_ = vfs::Vfs::create(vcfg, &err);
        if (!files_) return -1;
        std::vector<std::string> diag;
        if (!db_.load(*files_, {}, &diag) || !raw_.load(*files_, &diag) || !types_.build(raw_, files_.get(), &diag) ||
            !galactic::load_galactic_rules(*files_, db_, types_, rules_, &diag)) {
            err = "galactic: game data incomplete (" + std::to_string(diag.size()) + " diagnostics)";
            return -1;
        }
        std::vector<std::string> cdiag;
        galactic::load_conflict_rules(*files_, db_, types_, rules_, crules_, &cdiag);
        catalog_ = std::make_unique<sim::BattleCatalog>(types_, files_.get(), &db_.constants(), &cdiag);
        const auto lister = [this](std::string_view g) { return files_->list(g); };
        if (!ai::load_ai_definitions(*files_, ai::list_ai_data(lister), defs_)) {
            err = "galactic: no AI definitions found";
            return -1;
        }
        ai::load_equation_files(*files_, files_->list(ai::kPerceptionEquationGlob), eqs_);
        plan_files_ = ai::list_plan_files(lister);
        tinfo_ = ai::make_galactic_ai_type_info(types_);
        campaign_ = campaign.empty() ? std::string("Sandbox_Heart_Of_The_Maw_Underworld") : campaign;  // [name-ok: test default campaign]
        setup_ = galactic::build_campaign_setup(db_, rules_, campaign_, {}, &diag);
        if (setup_.map.planets.size() == 0) {
            err = "galactic: campaign '" + campaign_ + "' has no planets";
            return -1;
        }
        // As alamo_headless --economy --galactic-ai: every playable faction a galactic AI player (the campaign's
        // AI_Player_Control pairs, else the faction's Basic_AI), difficulty Normal_Default [port].
        const uint32_t normal = rules_.find_difficulty("Normal_Default");
        gcfg_.difficulties = ai::load_ai_difficulties(*files_);
        gcfg_.contrast = &crules_.contrast;
        gcfg_.ai_uses_fog_galactic = db_.constants().get_bool("AIUsesFogOfWarGalactic", gcfg_.ai_uses_fog_galactic);
        const data::CampaignDef* cdef = db_.find_campaign(campaign_);
        const std::vector<std::string> no_pairs;
        const std::vector<std::string>& control = cdef ? cdef->ai_player_control : no_pairs;
        const std::map<std::string, std::string> basic_ai = ai::load_faction_basic_ai(*files_);
        ai_types_.assign(setup_.players.size(), std::string());
        for (size_t p = 0; p < setup_.players.size(); ++p) {
            galactic::PlayerSetup& ps = setup_.players[p];
            const std::string fac = ps.faction < rules_.factions.size() ? rules_.factions[ps.faction].name : std::string();
            std::string type = ai::campaign_ai_type(control, basic_ai, fac, true);
            if (!type.empty() && !defs_.find_player(type)) type.clear();
            if (type.empty() || (ps.faction < rules_.factions.size() && rules_.factions[ps.faction].is_neutral)) continue;
            ps.human = false;
            ps.ai = true;
            ps.difficulty = normal;
            gcfg_.players.push_back({static_cast<int32_t>(p), type, 1});
            ai_types_[p] = type;
        }
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
        if (ho_.battles != "auto") {  // N1 stage 2: the tactical AI of fought battles (as skirmish_run.cpp foc_session)
            tai_ = std::make_shared<sim::TacticalAiSetup>();
            sim::load_tactical_ai_data(*tai_, *files_, &db_.constants(), lister);  // a2h t05: plans, difficulties, GameConstants
            tai_->defs = &defs_;
            tai_->equations = &eqs_;
            tai_->files = files_.get();
        }
#endif
        if (ho_.story && !cdef) {
            err = "galactic: --story: campaign '" + campaign_ + "' has no definition";
            return -1;
        }
        StateHasher h;  // the AI inputs the run depends on beyond types / rules / map
        h.u32(static_cast<uint32_t>(plan_files_.size()));
        for (const std::string& f : plan_files_) h.str(f);
        h.u32(static_cast<uint32_t>(defs_.goals.size()));
        h.u32(static_cast<uint32_t>(eqs_.size()));
        h.u32(static_cast<uint32_t>(gcfg_.players.size()));
        fingerprint_ = sim::combine_fingerprints({sim::fingerprint_types(types_), sim::fingerprint_galactic(rules_, setup_), h.digest()});
        std::printf("galactic: campaign %s, %zu planets, %zu AI players, %zu plan files, %zu equations\n", campaign_.c_str(),
                    setup_.map.planets.size(), gcfg_.players.size(), plan_files_.size(), eqs_.size());
        return 1;
    }

    std::unique_ptr<Session> build(uint32_t seed, int32_t workers, std::string& err) override {
        auto s = std::make_unique<Session>();
        Session* sp = s.get();
        s->js = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{workers});
        sim::SimConfig cfg;
        cfg.seed = seed;
        cfg.story_crc = ho_.story;
        cfg.players = sim::session_players(setup_.players.size());
        s->host = std::make_unique<sim::SimHost>(types_, *s->js, cfg);
        sim::SimHost& host = *s->host;
        s->gs = host.start_galactic(rules_, setup_);
        if (!s->gs) {
            err = "galactic: start_galactic failed";
            return nullptr;
        }
#ifdef ALAMO_SIM_HAVE_STORY
        if (ho_.story) {  // N1 stage 2: the campaign's story plot sets (as map_run.cpp story_run)
            sim::StoryStart st;
            st.files = files_.get();
            st.plot_sets = sim::campaign_plot_sets(*db_.find_campaign(campaign_), *s->gs);
            std::vector<std::string> sd;
            if (!host.start_story(st, &sd) || !host.story()) {
                err = "galactic: start_story failed: " + join(sd, "; ");
                return nullptr;
            }
        }
#endif
        sim::CampaignBattleOptions bo;
        bo.conflict.human_choice = ho_.battles == "auto" ? galactic::BattleChoice::AutoResolve : galactic::BattleChoice::Fight;
        bo.conflict.fight_ai_battles = ho_.battles == "fight-all";
        bo.tactical = ho_.battles != "auto";
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
        bo.tactical_ai = tai_;
#endif
        sim::load_tactical_systems(*files_, rules_, &db_.constants(), bo);
#ifdef ALAMO_SIM_HAVE_ANIM
        if (ho_.anim) {
            s->anim = std::make_unique<sim::SimAnimation>(types_, *files_);
            s->anim->attach(host, true);
            bo.animation = s->anim.get();
        }
#endif
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
        s->budget_rows.resize(s->gs->players().size());
        bo.tactical_budget = [sp](int32_t p) -> std::span<ai::tactical::TacticalBudgetCategory> {
            if (!sp->world || p < 0 || static_cast<size_t>(p) >= sp->budget_rows.size()) return {};
            auto& rows = sp->budget_rows[static_cast<size_t>(p)];
            rows.clear();
            for (const auto& r : sp->world->tactical_budget_rows(p)) rows.push_back({r.allowance, r.available, r.pool});
            return rows;
        };
#endif
        bo.object_removed = [sp](int32_t owner, uint32_t type, uint32_t original, bool survived) {
            if (sp->world) sp->world->record_object_removal(owner, type, original, survived);
        };
        s->battles = std::make_unique<sim::CampaignBattles>(host, crules_, *catalog_, sim::CampaignBattles::file_map_loader(*files_), bo);
        std::vector<std::string> ad;
        if (!s->battles->attach(&ad)) {
            err = "galactic: CampaignBattles::attach failed: " + join(ad, "; ");
            return nullptr;
        }
        s->battles->conflicts().on_survival_roll = [sp](int32_t owner, uint32_t type, bool survived) {
            if (sp->world) sp->world->record_auto_resolve_survival(owner, type, survived);
        };
        sim::SimHost* hp = &host;
        ai::GalacticAiConfig wcfg = gcfg_;
        wcfg.lua_gc_pf = host.parallel_for();  // OPT2 t04: scheduled Lua collections on the host's job system
        wcfg.background = jobs::make_background_start(host.jobs());  // OPT2 t02: background path recomputes
        s->world = std::make_unique<ai::GalacticAiWorld>(
            *s->gs, host.galactic_mode()->primary(), &types_, files_.get(), host.random(), defs_, eqs_, tinfo_,
            [hp](uint8_t p, const commands::EventData& ev) { return hp->execute_direct(p, commands::CommandSource::AI, ev); }, wcfg);
        ai::GalacticAiWorld& world = *s->world;
        world.set_conflicts(&s->battles->conflicts());  // q6 t03: TimeSince*Defender/Attacker stamps
        std::vector<std::string> wd;
        if (!world.load_plans(plan_files_, &wd) || !world.start(&wd)) {
            err = "galactic: AI start failed: " + join(wd, "; ");
            return nullptr;
        }
        world.register_crc(host.crc_registry());
        world.set_global_values(host.global_values());
        sim::CampaignBattles* bp = s->battles.get();
        for (const ai::GalacticAiPlayerConfig& pc : gcfg_.players)
            host.set_controller(static_cast<uint8_t>(pc.player), [sp, bp](sim::SimHost& h, uint8_t p, uint64_t frame) {
                const sim::GameMode* top = h.modes().top();
                const bool galactic_top = top && !top->tactical();
                const bool busy = bp->conflicts().state() == galactic::ConflictState::AutoRunning;
                sp->world->update_player(p, static_cast<int64_t>(frame), galactic_top, busy);
            });
        // N1: the galactic AI's chunk (GalacticAiWorld::write / read; pending while those are stubs).
        sim::SaveParticipant ai_part;
        ai_part.name = "ai";
        ai_part.write = [sp](ByteWriter& w) { sim::write_if_saveable(*sp->world, w); };
        ai_part.read = [sp](ByteReader& r) { return sim::read_if_saveable(*sp->world, r); };
        host.add_save_participant(std::move(ai_part));
        sim::SessionDescriptor d;
        d.kind = "galactic";
        d.campaign = campaign_;
        d.data_root = root_;
        d.data_args = args_;
        for (size_t p = 0; p < setup_.players.size(); ++p) d.setup.push_back(session_player(rules_, setup_.players[p], ai_types_[p]));
        d.difficulty = "Normal_Default";
        d.set_option("galactic_ai", "all");
        d.set_option("battles", ho_.battles);
        if (ho_.anim) d.set_option("anim", "1");  // N1 stage 2 (absent = off: stage-1 descriptors unchanged)
        if (ho_.story) d.set_option("story", "1");
        d.data_fingerprint = fingerprint_;
        host.set_session(std::move(d));
        return s;
    }

private:
    HarnessOptions ho_;
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
    std::shared_ptr<sim::TacticalAiSetup> tai_;
#endif
    std::string root_, campaign_;
    std::vector<std::string> args_;
    std::unique_ptr<vfs::Vfs> files_;
    data::GameDatabase db_;
    objects::ObjectTypeDatabase raw_;
    objects::GameObjectTypes types_;
    galactic::GalacticRules rules_;
    galactic::ConflictRules crules_;
    std::unique_ptr<sim::BattleCatalog> catalog_;
    ai::AiDefinitions defs_;
    ai::EquationSet eqs_;
    std::vector<std::string> plan_files_;
    std::vector<ai::GalacticAiTypeInfo> tinfo_;
    galactic::GalacticSetup setup_;
    ai::GalacticAiConfig gcfg_;
    std::vector<std::string> ai_types_;
    uint64_t fingerprint_ = 0;
};
#endif

// ------------------------------------------------------------------------------------------- skirmish (N1 stage 2)
class SkirmishHarness final : public Harness {
public:
    // 1 = loaded, 0 = no data (skip), -1 = error.
    int init(const HarnessOptions& ho, std::string& err) {
        ho_ = ho;
        if (ho_.campaign.empty()) ho_.campaign = "foc";
        bool skipped = false;
        data_ = skirmish_save_load(ho_.campaign, ho_.game_dir, ho_.game_args, ho_.map, skipped, err);
        if (skipped) return 0;
        return data_ ? 1 : -1;
    }

    std::unique_ptr<Session> build(uint32_t seed, int32_t workers, std::string& err) override {
        auto s = std::make_unique<Session>();
        s->js = std::make_unique<jobs::JobSystem>(jobs::JobSystemConfig{workers});
        sim::SimConfig cfg;
        cfg.seed = seed;
        cfg.players = 4;  // as skirmish_ai_run
        s->host = std::make_unique<sim::SimHost>(skirmish_save_types(*data_), *s->js, cfg);
        if (!skirmish_save_start(*data_, *s->host, ho_.anim, s->skirmish, err)) return nullptr;
        s->after_tick = s->skirmish.after_tick;
        if (sim::GalacticMode* g = s->host->galactic_mode()) s->gs = g->sim();
        sim::SessionDescriptor d;
        d.kind = "skirmish";
        d.campaign = ho_.campaign;
        d.data_root = ho_.game_dir.empty() ? env_text("ALAMO_FOC_DATA") : ho_.game_dir;
        d.data_args = ho_.game_args;
        d.set_option("map", skirmish_save_map(*data_));
        d.set_option("battles", "fight-all");
        if (ho_.anim) d.set_option("anim", "1");
        d.data_fingerprint = skirmish_save_fingerprint(*data_);
        s->host->set_session(std::move(d));
        return s;
    }

private:
    HarnessOptions ho_;
    std::shared_ptr<SkirmishSaveData> data_;
};

// kind: synthetic | hotm | galactic | skirmish. Returns null with skip = true when the session needs data that is not there.
std::unique_ptr<Harness> make_harness(std::string_view kind, const HarnessOptions& ho, bool& skip, std::string& err) {
    skip = false;
    if (kind == "synthetic" || kind == "synthetic-nostory") {
        if (ho.anim || ho.story || ho.battles != "auto") {
            err = "synthetic: --battles / --story do not apply";
            return nullptr;
        }
        auto h = std::make_unique<SyntheticHarness>(kind == "synthetic");
        if (!h->init(err)) return nullptr;
        return h;
    }
    if (kind == "skirmish") {
        auto h = std::make_unique<SkirmishHarness>();
        const int r = h->init(ho, err);
        if (r == 0) {
            skip = true;
            return nullptr;
        }
        if (r < 0) return nullptr;
        return h;
    }
    if (kind == "hotm" || kind == "galactic") {
#ifdef ALAMO_SAVE_RUN_GALACTIC
        auto h = std::make_unique<GalacticHarness>();
        const int r = h->init(ho.game_dir, ho.game_args, kind == "hotm" ? std::string() : ho.campaign, ho, err);
        if (r == 0) {
            skip = true;
            err = "no --game <corruption dir> and ALAMO_FOC_DATA unset";
            return nullptr;
        }
        if (r < 0) return nullptr;
        return h;
#else
        skip = true;
        err = "this build has no galactic AI (engine/ai + engine/lua) or no S10 battles";
        return nullptr;
#endif
    }
    err = "unknown session '" + std::string(kind) + "' (synthetic | synthetic-nostory | hotm | galactic | skirmish)";
    return nullptr;
}

// The harness options of the command line (save) or of a save's descriptor (load).
HarnessOptions harness_options(const SaveRunOptions& opt) {
    HarnessOptions ho;
    ho.campaign = opt.campaign;
    ho.game_dir = opt.game_dir;
    ho.game_args = opt.game_args;
    ho.map = opt.map;
    ho.story = opt.story;
    std::string b = opt.battles;
    if (const size_t plus = b.find("+anim"); plus != std::string::npos) {
        ho.anim = true;
        b.erase(plus, 5);
    }
    ho.battles = b.empty() ? std::string("auto") : b;
    if (opt.session == "skirmish") ho.battles = "auto";  // skirmish battles are always fought (only +anim counts)
    return ho;
}

// ------------------------------------------------------------------------------------------------- runs
uint64_t final_crc(const CrcLog& log) {
    return log.records.empty() ? 0 : log.records.back().combined;
}

CrcLog records_from(const CrcLog& log, uint64_t tick) {
    CrcLog out;
    out.systems = log.systems;
    out.parity_parts = log.parity_parts;
    for (const CrcRecord& r : log.records)
        if (r.tick >= tick) out.records.push_back(r);
    return out;
}

bool run_ticks(Session& s, uint64_t n, std::string& err) {
    for (uint64_t i = 0; i < n; ++i) {
        if (!s.host->tick_once()) {
            err = "the session stopped at frame " + std::to_string(s.host->frame());
            return false;
        }
        if (s.after_tick) s.after_tick();  // harness step between ticks (part of the session: the save records its state)
    }
    return true;
}

// Runs to `at`, then on to the first tick where saving is allowed and the --save-when condition holds (any: at most
// 10 min of game time later; battle / pending: 30 min), and saves.
bool run_to_save(Session& s, uint64_t at, const std::string& when, SaveFile& save, std::string& err) {
    sim::SimHost& host = *s.host;
    if (when != "any" && when != "battle" && when != "galactic" && when != "pending" && when != "scheduled") {
        err = "--save-when " + when + " (any | battle | galactic | pending | scheduled)";
        return false;
    }
    if (host.frame() < at && !run_ticks(s, at - host.frame(), err)) return false;
    const uint64_t limit = at + (when == "any" ? 18000u : 54000u);
    std::string why;
    auto ready = [&] {
        if (when == "battle" && !s.tactical_top()) {
            why = "no tactical battle running";
            return false;
        }
        if (when == "galactic" && s.tactical_top()) {
            why = "a tactical battle is running";
            return false;
        }
        if (when == "pending" && !s.conflict_pending()) {
            why = "no conflict pending";
            return false;
        }
        if (when == "scheduled" && s.ai_jobs_in_flight() == 0) {  // OPT3 t04: a save with AI work in flight
            why = "no AI background job in flight";
            return false;
        }
        return host.save_allowed(&why);
    };
    while (!ready()) {
        if (host.frame() >= limit) {
            err = "saving (--save-when " + when + ") was not possible from tick " + std::to_string(at) + " to " + std::to_string(limit) +
                  ": " + why;
            return false;
        }
        if (!run_ticks(s, 1, err)) return false;
    }
    if (host.frame() != at) std::printf("save: not at tick %" PRIu64 " (%s); saved at tick %" PRIu64 "\n", at, why.c_str(), host.frame());
    std::printf("save: mode stack depth %zu%s, %zu AI background job(s) in flight\n", static_cast<size_t>(host.modes().depth()),
                s.conflict_pending() ? ", conflict pending" : "", s.ai_jobs_in_flight());
    return host.save(save, &err);
}

size_t save_bytes(const SaveFile& f) {
    size_t n = 0;
    for (const SaveChunk& c : f.chunks()) n += c.data.size();
    return n;
}

bool write_crc(CrcLog log, const std::string& path, const std::string& comment) {
    if (path.empty()) return true;
    log.comments = {comment};
    std::string err;
    if (!alamo_headless::save_crc_log(log, path, &err)) {
        std::fprintf(stderr, "crc-out: %s\n", err.c_str());
        return false;
    }
    std::printf("wrote %s\n", path.c_str());
    return true;
}

commands::SavePoint save_point(const SaveFile& f) {
    const std::vector<uint8_t> bytes = f.encode();
    return {f.tick, commands::save_digest(bytes)};
}

// --save-session KIND --save-at T FILE [--ticks K] [--crc-out F] [--record-orders F]
int save_mode(Harness& hz, const SaveRunOptions& opt) {
    std::string err;
    auto s = hz.build(opt.seed, opt.workers, err);
    if (!s) {
        std::fprintf(stderr, "save: %s\n", err.c_str());
        return 1;
    }
    commands::CommandRecording rec;
    if (!opt.record_orders.empty()) s->host->record_to(&rec);
    SaveFile save;
    if (!run_to_save(*s, opt.save_at, opt.save_when, save, err) || !save.write_file(opt.save_file, &err)) {
        std::fprintf(stderr, "save: %s\n", err.c_str());
        return 1;
    }
    std::printf("saved tick %" PRIu64 " to %s: %zu chunks, %zu bytes, CRC at save %016" PRIx64 "\n", save.tick, opt.save_file.c_str(),
                save.chunks().size(), save_bytes(save), final_crc(s->host->crc_log()));
    if (!run_ticks(*s, opt.ticks, err)) {
        std::fprintf(stderr, "save: %s\n", err.c_str());
        return 1;
    }
    std::printf("ran to tick %" PRIu64 ", final CRC %016" PRIx64 "\n", s->host->frame(), final_crc(s->host->crc_log()));
    if (!opt.record_orders.empty()) {
        if (!rec.save(opt.record_orders, &err)) {
            std::fprintf(stderr, "record-orders: %s\n", err.c_str());
            return 1;
        }
        std::printf("wrote %s (%zu commands)\n", opt.record_orders.c_str(), rec.command_count());
    }
    return write_crc(s->host->crc_log(), opt.crc_out, "save-session " + opt.session + " seed " + std::to_string(opt.seed) + " saved at " +
                                                          std::to_string(save.tick))
               ? 0
               : 1;
}

// The harness that rebuilds the session a save's descriptor describes (--load, BM1 open_save_source).
std::unique_ptr<Harness> harness_for_save(const sim::SessionDescriptor& d, const std::string& game_dir,
                                          const std::vector<std::string>& game_args, bool& skip, std::string& err) {
    // The session kind decides the builder; a galactic save names its campaign.
    const std::string kind = d.kind == "synthetic" && d.option("story") == "0" ? std::string("synthetic-nostory") : d.kind;
    HarnessOptions ho;  // N1 stage 2: the session options the save recorded
    ho.campaign = d.campaign;
    ho.game_dir = game_dir;
    ho.game_args = game_args.empty() ? d.data_args : game_args;
    if (d.kind == "hotm" || d.kind == "galactic") {
        ho.battles = d.option("battles").empty() ? std::string("auto") : d.option("battles");
        ho.story = d.option("story") == "1";
    }
    ho.anim = d.option("anim") == "1";
    if (d.kind == "skirmish") ho.map = d.option("map");
    return make_harness(kind, ho, skip, err);
}

// --load FILE [--ticks K] [--crc-out F] [--replay F] [--record-orders F]
int load_mode(const SaveRunOptions& opt) {
    std::string err;
    SaveFile save;
    if (!SaveFile::read_file(opt.load_file, save, &err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    sim::SessionDescriptor d;
    if (!sim::read_session(save, d, &err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    std::printf("load: %s (tick %" PRIu64 "): %s session '%s', seed %u, written by %s\n", opt.load_file.c_str(), save.tick, d.kind.c_str(),
                d.campaign.c_str(), d.seed, d.writer.c_str());
    bool skip = false;
    auto hz = harness_for_save(d, opt.game_dir, opt.game_args, skip, err);
    if (!hz) {
        std::printf("load: %s%s\n", err.c_str(), skip ? ": skipped" : "");
        return skip ? 0 : 1;
    }
    auto s = hz->build(d.seed, opt.workers, err);
    if (!s) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    sim::LoadReport rep;
    const bool ok = s->host->load(save, &err, &rep);
    std::printf("load: %s\n", rep.text().c_str());
    if (!ok) return 1;
    const commands::SavePoint at = save_point(save);
    commands::CommandRecording replay, record;
    if (!opt.replay.empty()) {
        if (!commands::CommandRecording::load(opt.replay, replay, &err) || !s->host->replay_after_load(&replay, at, &err)) {
            std::fprintf(stderr, "replay: %s\n", err.c_str());
            return 1;
        }
        std::printf("replaying %s (%zu commands%s)\n", opt.replay.c_str(), replay.command_count(), replay.from_save ? ", from this save" : "");
    }
    if (!opt.record_orders.empty()) s->host->record_after_load(&record, at);
    if (!run_ticks(*s, opt.ticks, err)) {
        std::fprintf(stderr, "load: %s\n", err.c_str());
        return 1;
    }
    std::printf("load: ran %" PRIu64 " ticks to %" PRIu64 ", final CRC %016" PRIx64 "\n", opt.ticks, s->host->frame(),
                final_crc(s->host->crc_log()));
    if (!opt.replay.empty() && s->host->pipeline().replay_failures() != 0) {
        std::fprintf(stderr, "replay: %zu recorded commands failed to insert\n", s->host->pipeline().replay_failures());
        return 1;
    }
    if (!opt.record_orders.empty()) {
        if (!record.save(opt.record_orders, &err)) {
            std::fprintf(stderr, "record-orders: %s\n", err.c_str());
            return 1;
        }
        std::printf("wrote %s (%zu commands, continues the save)\n", opt.record_orders.c_str(), record.command_count());
    }
    return write_crc(s->host->crc_log(), opt.crc_out, "loaded " + opt.load_file + " at tick " + std::to_string(save.tick)) ? 0 : 1;
}

std::string quoted(const std::string& s) {
    return "\"" + s + "\"";
}

// Runs `self --load FILE ...` in a new process; its CRC log goes to crc_path.
int run_child(const SaveRunOptions& opt, const std::string& crc_path) {
    std::string cmd = quoted(opt.self) + " --load " + quoted(opt.save_file) + " --ticks " + std::to_string(opt.ticks) + " --crc-out " +
                      quoted(crc_path) + " --workers " + std::to_string(opt.workers) + " --quiet";
    if (!opt.game_dir.empty()) cmd += " --game " + quoted(opt.game_dir);
    for (const std::string& a : opt.game_args) cmd += " " + quoted(a);
#ifdef _WIN32
    cmd = "\"" + cmd + "\"";  // cmd.exe /c strips one outer pair of quotes
#endif
    std::fflush(stdout);
    std::fflush(stderr);
    return std::system(cmd.c_str());
}

// --save-session KIND --save-check --save-at T FILE --ticks K
int check_mode(Harness& hz, const SaveRunOptions& opt) {
    std::string err;
    bool ok = true;
    auto fail = [&ok](const std::string& m) {
        std::printf("FAIL %s\n", m.c_str());
        ok = false;
    };
    // A: the original run, recording its scheduled commands from tick 0.
    commands::CommandRecording rec_a;
    SaveFile save;
    CrcRecord at_save;
    CrcLog log_a;
    SimRng rng_save, rng_end;  // N1 stage 2: the synced RNG at the save tick / at T+K
    {
        auto a = hz.build(opt.seed, opt.workers, err);
        if (!a) {
            std::fprintf(stderr, "check: %s\n", err.c_str());
            return 1;
        }
        a->host->record_to(&rec_a);
        if (!run_to_save(*a, opt.save_at, opt.save_when, save, err) || !save.write_file(opt.save_file, &err)) {
            std::fprintf(stderr, "check: %s\n", err.c_str());
            return 1;
        }
        at_save = a->host->crc_log().records.back();
        rng_save = a->host->random().synced();
        if (!run_ticks(*a, opt.ticks, err)) {
            std::fprintf(stderr, "check: %s\n", err.c_str());
            return 1;
        }
        log_a = records_from(a->host->crc_log(), save.tick);
        rng_end = a->host->random().synced();
        std::printf("A original: saved tick %" PRIu64 " (%zu chunks, %zu bytes, CRC %016" PRIx64 "), ran to %" PRIu64 ", %zu scheduled "
                    "commands, final CRC %016" PRIx64 "\n",
                    save.tick, save.chunks().size(), save_bytes(save), at_save.combined, a->host->frame(), rec_a.command_count(),
                    final_crc(log_a));
        if (a->gs) {  // what the session did (planets / credits per player at the end)
            std::string line = "  end state: frame " + std::to_string(a->gs->frame()) + ", planets/credits";
            for (size_t p = 0; p < a->gs->players().size(); ++p)
                line += " p" + std::to_string(p) + "=" + std::to_string(a->gs->planets_owned(static_cast<int32_t>(p))) + "/" +
                        std::to_string(static_cast<int64_t>(a->gs->credits(static_cast<int32_t>(p))));
            std::printf("%s\n", line.c_str());
        }
    }
    const uint64_t T = save.tick;
    const commands::SavePoint at = save_point(save);

    // A session with another seed must refuse the save (descriptor check, nothing read).
    {
        auto r = hz.build(opt.seed + 1u, opt.workers, err);
        sim::LoadReport rep;
        std::string e;
        if (!r) fail("check: " + err);
        else if (r->host->load(save, &e, &rep) || rep.descriptor_error.empty()) fail("a session with seed+1 accepted the save");
        else std::printf("seed+1 session: refused (%s)\n", rep.descriptor_error.c_str());
    }

    // B: load into a fresh session in this process.
    auto b = hz.build(opt.seed, opt.workers, err);
    if (!b) {
        std::fprintf(stderr, "check: %s\n", err.c_str());
        return 1;
    }
    sim::LoadReport rep;
    std::string load_err;
    const bool loaded = b->host->load(save, &load_err, &rep);
    if (!(b->host->random().synced() == rng_save)) fail("B: the synced RNG after the load differs from the original's at the save tick");
    else std::printf("B load: synced RNG equals the original's at the save tick (state %08x, %" PRIu64 " draws)\n", rng_save.state(),
                     rng_save.draws());
    std::printf("B load: %s\n", rep.text().c_str());
    if (!rep.descriptor_error.empty()) {
        fail("load refused its own save: " + rep.descriptor_error);
        return 1;
    }
    std::vector<std::string> failing = rep.failed;
    failing.insert(failing.end(), rep.missing.begin(), rep.missing.end());
    failing.insert(failing.end(), rep.unexpected.begin(), rep.unexpected.end());
    if (!failing.empty()) fail("chunks failed to load: " + join(failing));

    if (!loaded) {
        fail("load failed: " + load_err);
        // Diagnostic: the CRC systems whose loaded state already differs from the original's at the save tick.
        const CrcRegistry& reg = b->host->crc_registry();
        std::vector<std::string> differing;
        for (CrcRegistry::SystemId i = 0; i < reg.size() && i < at_save.systems.size(); ++i)
            if (reg.hash_system(i) != at_save.systems[i]) differing.push_back(reg.name(i));
        std::printf("tick %" PRIu64 " CRC systems differing from the save: %s\n", T, differing.empty() ? "(none)" : join(differing, " ").c_str());
        std::printf("save check (%s): FAILED\n", opt.session.c_str());
        return 1;
    }

    // Every chunk loaded: B continues to T+K recording its orders as a stream continuing the save.
    commands::CommandRecording rec_b;
    b->host->record_after_load(&rec_b, at);
    if (!run_ticks(*b, opt.ticks, err)) fail("B: " + err);
    {
        const auto d = first_divergence(log_a, b->host->crc_log());
        std::printf("B in-process load vs original, ticks %" PRIu64 "..%" PRIu64 ": %s\n", T, T + opt.ticks, d.message.c_str());
        if (d.diverged) fail("B diverged");
        if (!(b->host->random().synced() == rng_end)) fail("B: the synced RNG at T+K differs from the original's");
    }
    b.reset();

    // C: a fresh process loads the file.
    {
        const std::string crc_c = opt.save_file + ".load.crc";
        const int rc = run_child(opt, crc_c);
        CrcLog log_c;
        if (rc != 0) fail("fresh process exited with " + std::to_string(rc));
        else if (!CrcLog::load(crc_c, log_c, &err)) fail("fresh process CRC log: " + err);
        else {
            const auto d = first_divergence(log_a, log_c);
            std::printf("C fresh process load vs original: %s\n", d.message.c_str());
            if (d.diverged) fail("C diverged");
        }
    }

    // D / E: replay from the save, a stream recorded after the load (B) and the original's full tick-0 stream (A).
    auto replay_check = [&](const char* label, const commands::CommandRecording& rec) {
        std::string e;
        auto s = hz.build(opt.seed, opt.workers, e);
        if (!s || !s->host->load(save, &e)) {
            fail(std::string(label) + ": " + e);
            return;
        }
        if (!s->host->replay_after_load(&rec, at, &e)) {
            fail(std::string(label) + ": replay_from_save refused: " + e);
            return;
        }
        if (!run_ticks(*s, opt.ticks, e)) fail(std::string(label) + ": " + e);
        const auto d = first_divergence(log_a, s->host->crc_log());
        std::printf("%s (%zu commands): %s\n", label, rec.command_count(), d.message.c_str());
        if (d.diverged) fail(std::string(label) + " diverged");
        if (s->host->pipeline().replay_failures() != 0) fail(std::string(label) + ": recorded commands failed to insert");
    };
    replay_check("D load + replay of the stream recorded after the load", rec_b);
    replay_check("E load + replay of the original's tick-0 stream", rec_a);

    std::printf("save check (%s, save at %" PRIu64 ", +%" PRIu64 " ticks): %s\n", opt.session.c_str(), T, opt.ticks, ok ? "OK" : "FAILED");
    return ok ? 0 : 1;
}

}  // namespace

// ------------------------------------------------------------------------------------------------- BM1 benchmark
namespace {

class LoadedSessionImpl final : public LoadedSession {
public:
    explicit LoadedSessionImpl(std::unique_ptr<Session> s) : s_(std::move(s)) {}
    bool tick(std::string& err) override { return run_ticks(*s_, 1, err); }
    const CrcLog& crc_log() const override { return s_->host->crc_log(); }
    uint64_t frame() const override { return s_->host->frame(); }
    size_t replay_failures() const override { return s_->host->pipeline().replay_failures(); }
    uint32_t worker_count() const override { return s_->js->worker_count(); }

private:
    std::unique_ptr<Session> s_;
};

// The harness (game data, AI data) outlives every session it built: sessions are destroyed before the source.
class SaveSessionSourceImpl final : public SaveSessionSource {
public:
    SaveFile save;
    sim::SessionDescriptor d;
    std::unique_ptr<Harness> hz;
    commands::SavePoint at;

    uint64_t save_tick() const override { return save.tick; }
    std::string describe() const override {
        const std::string map = d.option("map");
        return d.kind + " '" + d.campaign + "' seed " + std::to_string(d.seed) + (map.empty() ? std::string() : " map " + map);
    }
    std::unique_ptr<LoadedSession> load(int32_t workers, const commands::CommandRecording* replay, commands::CommandRecording* record,
                                        std::string& err) override {
        auto s = hz->build(d.seed, workers, err);
        if (!s) return nullptr;
        sim::LoadReport rep;
        if (!s->host->load(save, &err, &rep)) {
            err = "load refused: " + err + " (" + rep.text() + ")";
            return nullptr;
        }
        if (replay && !s->host->replay_after_load(replay, at, &err)) {
            err = "replay refused: " + err;
            return nullptr;
        }
        if (record) s->host->record_after_load(record, at);
        return std::make_unique<LoadedSessionImpl>(std::move(s));
    }
};

}  // namespace

int open_save_source(const std::string& save_path, const std::string& game_dir, const std::vector<std::string>& game_args,
                     std::unique_ptr<SaveSessionSource>& out, std::string& err) {
    auto src = std::make_unique<SaveSessionSourceImpl>();
    if (!SaveFile::read_file(save_path, src->save, &err) || !sim::read_session(src->save, src->d, &err)) return -1;
    bool skip = false;
    src->hz = harness_for_save(src->d, game_dir, game_args, skip, err);
    if (!src->hz) return skip ? 0 : -1;
    src->at = save_point(src->save);
    out = std::move(src);
    return 1;
}

int make_save(const SaveRunOptions& opt, uint64_t& saved_tick, std::string& err) {
    bool skip = false;
    auto hz = make_harness(opt.session, harness_options(opt), skip, err);
    if (!hz) return skip ? 0 : -1;
    auto s = hz->build(opt.seed, opt.workers, err);
    if (!s) return -1;
    // The game-data folder is informational (never compared on load) but would make the save's bytes, and so the scenario
    // version, depend on where this machine installed the game: a checked-in benchmark save does not record it.
    sim::SessionDescriptor d = s->host->session();
    d.data_root.clear();
    s->host->set_session(std::move(d));
    SaveFile save;
    if (!run_to_save(*s, opt.save_at, opt.save_when, save, err) || !save.write_file(opt.save_file, &err)) return -1;
    saved_tick = save.tick;
    return 1;
}

int save_run(const SaveRunOptions& opt) {
    if (!opt.load_file.empty() && !opt.check) return load_mode(opt);
    if (opt.session.empty() || !opt.save_at_set || opt.save_file.empty()) {
        std::fprintf(stderr, "save: --save-session KIND needs --save-at TICK FILE (or use --load FILE)\n");
        return 2;
    }
    std::string err;
    bool skip = false;
    auto hz = make_harness(opt.session, harness_options(opt), skip, err);
    if (!hz) {
        std::printf("save-session %s: %s%s\n", opt.session.c_str(), err.c_str(), skip ? ": skipped" : "");
        return skip ? 0 : 1;
    }
    return opt.check ? check_mode(*hz, opt) : save_mode(*hz, opt);
}

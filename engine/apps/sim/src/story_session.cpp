#include "alamo/sim/story_session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <map>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/core/dmath.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/object_bridge.hpp"
#include "alamo/lua/presentation.hpp"
#include "alamo/lua/script_remaining.hpp"  // Int-final: attach_global_values
#include "alamo/lua/story_model.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/sim/save_session.hpp"  // N1: write_section / write_if_saveable
#if defined(ALAMO_LUA_PLAYERS)
#include "alamo/lua/players.hpp"
#endif

#include <lua.h>

namespace alamo::sim {

namespace {

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

int32_t to_int(std::string_view s, int32_t fallback) {
    s = trim(s);
    if (s.empty()) return fallback;
    bool neg = false;
    size_t i = 0;
    if (s[0] == '-' || s[0] == '+') {
        neg = s[0] == '-';
        i = 1;
    }
    if (i >= s.size()) return fallback;
    int64_t v = 0;
    for (; i < s.size(); ++i) {
        if (s[i] < '0' || s[i] > '9') break;  // "2.0" -> 2
        v = v * 10 + (s[i] - '0');
        if (v > 0x7FFFFFFF) return fallback;
    }
    return static_cast<int32_t>(neg ? -v : v);
}

// Event / reward parameter i as text ("" when absent; object / player values set from Lua print as
// "#object:<id>" / "#player:<id>" and never equal a name).
std::string param_text(const std::vector<lua::StoryParam>& v, size_t i) {
    if (i >= v.size()) return {};
    const lua::StoryParam& p = v[i];
    switch (p.kind) {
    case lua::StoryParam::Kind::Text: return std::string(trim(p.text));
    case lua::StoryParam::Kind::Object: return "#object:" + std::to_string(p.id);
    case lua::StoryParam::Kind::Player: return "#player:" + std::to_string(p.id);
    }
    return {};
}

std::string join_params(const std::vector<lua::StoryParam>& v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) s += '|';
        s += param_text(v, i);
    }
    return s + "]";
}

const char* corruption_name(int32_t type) {
    // GE 1.3 [M] index <-> name.
    static const char* const kNames[] = {"INTIMIDATION",    "PIRACY",  "KIDNAPPING",   "RACKETEERING",
                                         "CORRUPT_MILITIA", "BRIBERY", "BLACK_MARKET", "BONDED_CITIZENS"};
    return type >= 0 && type < 8 ? kNames[type] : "NONE";
}

constexpr double kDefaultServiceRate = 0.2;  // PGStateMachine Base_Definitions: ServiceRate = 0.2

// The galactic rules' type index `ty` names the same type in the object type database (they share one
// index space when the rules were extracted from it; hand-made test rules may not).
bool same_type(const objects::GameObjectTypes& types, const galactic::GalacticRules& rules, uint32_t ty) {
    return ty < rules.types.size() && ty < types.raw().type_count() && iequal(types.raw().types().name[ty], rules.types[ty].name);
}

// Wave 2a: GameObjectCategoryType name -> CategoryMask bits (StoryModel::category_mask).
std::function<uint64_t(std::string_view)> category_resolver(const objects::GameObjectTypes& types) {
    const objects::GameObjectTypes* t = &types;
    return [t](std::string_view c) -> uint64_t {
        const auto m = t->enums().get(objects::EnumId::GameObjectCategory).convert(c);
        return m ? static_cast<uint64_t>(*m) : 0u;
    };
}

// Wave 2b: player -> faction index (StoryModel::faction_of; FILTER_* compare factions, SCRIPT_GAPS.md §4.5).
std::function<int32_t(int32_t)> faction_resolver(const galactic::GalacticSim& sim) {
    const galactic::GalacticSim* s = &sim;
    return [s](int32_t p) -> int32_t {
        if (p < 0 || static_cast<size_t>(p) >= s->players().size()) return -1;
        const uint32_t f = s->players()[static_cast<size_t>(p)].faction;
        return f < s->rules().factions.size() ? static_cast<int32_t>(f) : -1;
    };
}

std::string type_name_of(const objects::GameObjectTypes& types, objects::TypeIndex t) {
    return t < types.raw().type_count() ? std::string(types.raw().types().name[t]) : std::string();
}

// Named hero type names of a type (wave 1 rule): the type itself and its Company_Units members.
std::vector<std::string> hero_names(const objects::GameObjectTypes& types, objects::TypeIndex t) {
    std::vector<std::string> out;
    if (t >= types.raw().type_count()) return out;
    if (types.get_bool(t, "Is_Named_Hero")) out.push_back(type_name_of(types, t));
    for (const objects::TypeIndex m : types.get_type_refs(t, "Company_Units"))
        if (m != objects::kNone && m < types.raw().type_count() && types.get_bool(m, "Is_Named_Hero"))
            out.push_back(type_name_of(types, m));
    return out;
}

double dist3(Vec3 a, Vec3 b) {
    const double dx = static_cast<double>(a.x) - static_cast<double>(b.x);
    const double dy = static_cast<double>(a.y) - static_cast<double>(b.y);
    const double dz = static_cast<double>(a.z) - static_cast<double>(b.z);
    return dmath::sqrt(dx * dx + dy * dy + dz * dz);
}

}  // namespace

// ------------------------------------------------------------------------------------------------ data
struct StorySession::Script {
    uint32_t plot = 0;
    std::string name;
    std::unique_ptr<lua::LuaVm> vm;
    double rate = kDefaultServiceRate;
    double last_service = 0.0;
    bool serviced = false;
    size_t errors_seen = 0;
};

struct StorySession::Families {
#if defined(ALAMO_LUA_PLAYERS)
    lua::LuaPlayerWorld players;
#endif
    lua::ObjectBridge bridge;
    lua::PresentationLog presentation;
};

struct StorySession::Set {
    int32_t player = -1;
    uint32_t number = 0;  // plot set number in the log (galactic: index; tactical: after the galactic ones)
    std::string file;
    lua::StoryModel model;
    std::vector<Script> scripts;
    size_t fired_seen = 0;
    bool credits_seen = false;  // ACCUMULATE stand-in: credits at the last poll
    float last_credits = 0.0f;
};

// Wave 2a: the last battle begun (begin_battle), for the WIN/LOSE_BATTLES / INVASION_BOUNCED routing.
struct StorySession::Battle {
    PlanetIndex planet = kInvalidPlanet;
    int32_t kind = 0;  // 1 land, 2 space (the S10 battle events' value)
    int32_t attacker = -1, defender = -1;
    bool base = false;  // space: a starbase; land: defender ground structures
    std::vector<std::pair<int32_t, uint64_t>> masks;     // (owner, CategoryMask) per unit and team member, request order
    std::vector<std::pair<int32_t, std::string>> names;  // (owner, type name), same order
};

// Wave 2a: the attached tactical story of a running battle.
struct StorySession::Tactical {
    struct Pending {
        StoryTrigger trigger;
        TacticalStoryEvent kind = TacticalStoryEvent::Count;
        int32_t owner = -1;
        std::string target;
        double distance = -1.0;
        std::vector<std::string> heroes;  // Destroyed: hero type names (DEFEAT_HERO)
        bool has_position = false;        // wave 2b: UnitCommanded destination
        double x = 0.0, y = 0.0, z = 0.0;
    };
    std::unique_ptr<Set> set;  // nullptr: no tactical plots for this battle
    objects::ObjectStore* store = nullptr;
    lua::ObjectBridge* bridge = nullptr;
    std::unique_ptr<lua::ObjectBridge> own_bridge;
    uint64_t start_frame = 0;
    double spf = 1.0 / 30.0;
    std::function<bool(int32_t, bool)> side_destroyed;
    std::function<bool(int32_t, double, double, double)> fog_point_visible;  // wave 2b hooks (fog agent)
    std::function<bool(int32_t, int32_t)> fog_object_visible;
    std::vector<Pending> pending;
};

std::string StoryTrigger::text() const {
    std::string s = "T " + std::to_string(frame) + " " + std::to_string(index) + " " + kind;
    auto add = [&s](const char* key, const std::string& v) {
        if (!v.empty()) s += std::string(" ") + key + "=" + v;
    };
    add("player", faction);
    add("other", other_faction);
    add("planet", planet);
    add("to", other_planet);
    add("type", type);
    if (event.object != 0) s += " object=" + std::to_string(event.object);
    if (event.fleet != galactic::kNoFleet) s += " fleet=" + std::to_string(event.fleet);
    if (event.kind != galactic::GalacticEventKind::Count) s += " value=" + std::to_string(event.value);
    if (!units.empty()) {
        s += " units=";
        for (size_t i = 0; i < units.size(); ++i) s += (i ? "," : "") + units[i];
    }
    if (event.kind != galactic::GalacticEventKind::Count) s += " gframe=" + std::to_string(event.frame);
    return s;
}

std::string StoryLogEntry::text() const {
    return "F " + std::to_string(frame) + " " + std::to_string(plot_set) + ":" + std::to_string(sequence) + " " + plot +
           " " + event + " " + type + " " + params + " reward=" + reward + " via=" + via;
}

std::vector<StoryPlotSet> campaign_plot_sets(const data::CampaignDef& campaign, const galactic::GalacticSim& sim) {
    std::vector<StoryPlotSet> out;
    const auto players = sim.players();
    for (size_t p = 0; p < players.size(); ++p) {
        const uint32_t f = players[p].faction;
        if (f >= sim.rules().factions.size()) continue;
        const std::string tag = sim.rules().factions[f].name + "_Story_Name";
        for (const auto& [t, v] : campaign.raw)
            if (iequal(t, tag) && !trim(v).empty()) {
                out.push_back({static_cast<int32_t>(p), std::string(trim(v))});
                break;
            }
    }
    return out;
}

// --------------------------------------------------------------------------------------------- session
StorySession::StorySession(galactic::GalacticSim& sim, objects::ObjectStore& store, const objects::GameObjectTypes& types,
                           GameRandom& rng)
    : sim_(&sim), store_(&store), types_(&types), rng_(&rng), lua_(std::make_unique<Families>()) {
#if defined(ALAMO_LUA_PLAYERS)
    lua_->players.attach_galactic(sim_);
    lua::LuaPlayerWorld* world = &lua_->players;
    lua_->bridge.is_enemy = [world](int32_t a, int32_t b) {
        return world->valid(a) && world->valid(b) && world->relation(a, b) == lua::PlayerRelation::Enemy;
    };
#endif
    // Story VMs run on the galactic store, whose ownership / membership the GalacticSim keeps (wave 1).
    galactic::GalacticSim* gs = sim_;
    lua_->bridge.change_owner = [gs](objects::ObjectHandle h, int32_t owner) { return gs->change_owner(h, owner); };
    lua_->bridge.container_of = [gs](objects::ObjectHandle h) { return gs->container_of(h); };
    lua_->bridge.contents_of = [gs](objects::ObjectHandle h) { return gs->contents_of(h); };
    lua_->bridge.destroy_object = [gs](objects::ObjectHandle h) { return gs->destroy_object(h); };
}

std::string_view story_ui_event_name(StoryUiEvent e) {
    static constexpr std::string_view kNames[] = {"SelectUnit",  "SelectPlanet", "ZoomIntoPlanet", "ZoomOutPlanet",
                                                  "ClickGui",    "SpeechDone",   "MovieDone",      "Generic",
                                                  "MissionLost", "BuyBlackMarket", "GalacticSabotage", "OpenCorruption"};
    const auto i = static_cast<size_t>(e);
    return i < std::size(kNames) ? kNames[i] : std::string_view("?");
}
std::string_view story_ui_event_type(StoryUiEvent e) {
    static constexpr std::string_view kTypes[] = {
        "STORY_SELECT_UNIT", "STORY_SELECT_PLANET", "STORY_ZOOM_INTO_PLANET", "STORY_ZOOM_OUT_PLANET",
        "STORY_CLICK_GUI",   "STORY_SPEECH_DONE",   "STORY_MOVIE_DONE",       "STORY_GENERIC",
        "STORY_MISSION_LOST", "STORY_BUY_BLACK_MARKET", "STORY_GALACTIC_SABOTAGE", "STORY_OPEN_CORRUPTION"};
    const auto i = static_cast<size_t>(e);
    return i < std::size(kTypes) ? kTypes[i] : std::string_view();
}

// Queued UI notification as a trigger: kind "Ui<Kind>", type = the dispatched name (resolved now: the
// object's type name, a planet's type name), planet = planet type name, faction = acting player.
void StorySession::notify_ui(const StoryUiNotification& n) {
    if (story_ui_event_type(n.kind).empty()) return;
    // --- P4 lua: BUY_BLACK_MARKET / GALACTIC_SABOTAGE come from the galactic stream (BlackMarketPurchase /
    // GalacticSabotage, notifications()), so they replay with the commands; the UI kinds are ignored (no double fire).
    if (n.kind == StoryUiEvent::BuyBlackMarket || n.kind == StoryUiEvent::GalacticSabotage) return;
    // --- end P4 lua ---
    // L1/S4 wave 1 (tactical, header): battle-screen SELECT_UNIT / CLICK_GUI / SPEECH_DONE -> the tactical plot set
    // (notify_tactical: the object resolved in the battle's store); only SPEECH_DONE also continues to the galactic sets.
    if (n.tactical) {
        TacticalStoryEvent k = TacticalStoryEvent::Count;
        if (n.kind == StoryUiEvent::SelectUnit) k = TacticalStoryEvent::SelectUnit;
        else if (n.kind == StoryUiEvent::ClickGui) k = TacticalStoryEvent::ClickGui;
        else if (n.kind == StoryUiEvent::SpeechDone) k = TacticalStoryEvent::SpeechDone;
        const bool attached = tactical_ && tactical_->set && (n.player < 0 || n.player == tactical_->set->player);
        if (attached && k != TacticalStoryEvent::Count) {
            TacticalStoryNotification tn;
            tn.kind = k;
            if (k == TacticalStoryEvent::SelectUnit) {
                tn.object = n.object;  // name = its type, filter owner = its owner (notify_tactical)
            } else {
                tn.player = n.player;
                tn.name = n.name;
            }
            notify_tactical(tn);
        }
        if (n.kind == StoryUiEvent::SelectUnit || n.kind == StoryUiEvent::ClickGui) return;  // battle-store id / tactical HUD
    }
    StoryTrigger t;
    t.kind = "Ui" + std::string(story_ui_event_name(n.kind));
    t.event.player = n.player;
    t.event.object = n.object;
    t.event.kind = galactic::GalacticEventKind::Count;
    t.event.value = static_cast<int32_t>(n.kind);
    t.faction = faction_name(n.player);
    const objects::ObjectHandle h = n.object != 0 ? store_->find(n.object) : objects::ObjectHandle{};
    const bool live = h && store_->alive(h);
    const PlanetIndex row = live ? sim_->planet_of(h) : kInvalidPlanet;
    if (row != kInvalidPlanet) t.planet = planet_type_name(row);
    if (!n.name.empty()) {
        t.type = n.name;
    } else if (row != kInvalidPlanet) {
        t.type = t.planet;
    } else if (live && store_->type(h) < sim_->rules().types.size()) {
        t.type = sim_->rules().types[store_->type(h)].name;
    }
    if (!live) t.event.object = 0;
    t.event.other_player = live ? store_->owner(h) : -1;  // owner of the object (SELECT_UNIT filter)
    pending_ui_.push_back(std::move(t));
}

// Type names of a unit for the evaluators: its own type, then its Company_Units members (wave 1).
void StorySession::add_unit_names(objects::ObjectHandle h, std::vector<std::string>& out) const {
    if (!sim_->store().exists(h)) return;
    const auto ty = static_cast<uint32_t>(sim_->store().type(h));
    if (ty >= sim_->rules().types.size()) return;
    out.push_back(sim_->rules().types[ty].name);
    if (same_type(*types_, sim_->rules(), ty))
        for (const objects::TypeIndex m : types_->get_type_refs(ty, "Company_Units"))
            if (m != objects::kNone && m < sim_->rules().types.size()) out.push_back(sim_->rules().types[m].name);
}

// [port poll] planets whose destroyed flag went up since the last service -> "PlanetDestroyed" triggers
// (GalacticSim::set_destroyed emits no stream event). The first call only records the flags.
void StorySession::poll_planets(std::vector<StoryTrigger>& batch) {
    const auto planets = sim_->planets();
    if (!destroyed_init_) {
        destroyed_init_ = true;
        destroyed_seen_.assign(planets.size(), 0);
        for (size_t i = 0; i < planets.size(); ++i) destroyed_seen_[i] = planets[i].destroyed ? 1 : 0;
        return;
    }
    destroyed_seen_.resize(planets.size(), 0);
    const auto& names = sim_->map().planets.name;
    for (size_t i = 0; i < planets.size(); ++i) {
        const uint8_t now = planets[i].destroyed ? 1 : 0;
        if (now == destroyed_seen_[i]) continue;
        destroyed_seen_[i] = now;
        if (!now || !planets[i].is_planet) continue;
        StoryTrigger t;
        t.kind = "PlanetDestroyed";
        t.event.kind = galactic::GalacticEventKind::Count;
        t.event.planet = static_cast<PlanetIndex>(i);
        t.event.player = planets[i].owner;
        if (i < names.size()) t.planet = names[i];
        t.faction = faction_name(planets[i].owner);
        batch.push_back(std::move(t));
    }
}

StorySession::~StorySession() {
    tactical_.reset();  // VMs before the families (and the bridge) they point to
    sets_.clear();
}

void StorySession::set_command_pipeline(commands::CommandPipeline* pipeline) {
    lua_->bridge.pipeline = pipeline;
    if (tactical_ && tactical_->own_bridge) tactical_->own_bridge->pipeline = pipeline;
}
lua::PresentationLog* StorySession::presentation() { return &lua_->presentation; }
lua::ObjectBridge* StorySession::object_bridge() { return &lua_->bridge; }
lua::LuaPlayerWorld* StorySession::player_world() {
#if defined(ALAMO_LUA_PLAYERS)
    return &lua_->players;
#else
    return nullptr;
#endif
}

const lua::StoryModel& StorySession::model(size_t set) const { return sets_.at(set)->model; }
lua::StoryModel& StorySession::model(size_t set) { return sets_.at(set)->model; }
int32_t StorySession::plot_set_player(size_t set) const { return sets_.at(set)->player; }
size_t StorySession::script_count() const {
    size_t n = 0;
    for (const auto& s : sets_) n += s->scripts.size();
    return n;
}

std::string StorySession::faction_name(int32_t player) const {
    if (player < 0 || static_cast<size_t>(player) >= sim_->players().size()) return {};
    const uint32_t f = sim_->players()[static_cast<size_t>(player)].faction;
    return f < sim_->rules().factions.size() ? sim_->rules().factions[f].name : std::string();
}

void StorySession::lua_note(const Script& sc, std::string_view what) {
    lua_messages_.push_back(std::to_string(frame_) + " " + sc.name + ": " + std::string(what));
}

bool StorySession::start(const StoryStart& st, std::vector<std::string>* diagnostics) {
    files_ = st.files;
    global_values_ = st.global_values;  // Int-final
    auto note =[diagnostics](std::string m) {
        if (diagnostics) diagnostics->push_back(std::move(m));
    };
    if (!files_) {
        note("story: no file provider");
        return false;
    }
    for (const StoryPlotSet& ps : st.plot_sets) {
        auto set = std::make_unique<Set>();
        set->player = ps.player;
        set->number = static_cast<uint32_t>(sets_.size());
        set->file = ps.plots_file;
        std::string err;
        if (!set->model.load_plots(*files_, ps.plots_file, &err)) {
            note("story: " + err);
            return false;
        }
        for (const std::string& w : set->model.warnings()) note("story " + ps.plots_file + ": " + w);
        set->model.set_player(ps.player);
        set->model.category_mask = category_resolver(*types_);  // wave 2a: WIN/LOSE_BATTLES CLASS_* tests
        set->model.faction_of = faction_resolver(*sim_);        // wave 2b: FILTER_* compare factions
        Set* raw = set.get();
        // Rewards the model leaves to the game, applied synchronously at Fire step 5 (STORY.md §6.1).
        set->model.on_reward = [this, raw](uint32_t, uint32_t, const std::string& type,
                                           const std::vector<lua::StoryParam>& params) {
            std::vector<std::string> rp;
            for (size_t i = 0; i < params.size(); ++i) rp.push_back(param_text(params, i));
            apply_reward(*raw, type, rp);
        };
        sets_.push_back(std::move(set));
    }
    if (!st.lua) return true;
    for (size_t si = 0; si < sets_.size(); ++si) {
        Set& s = *sets_[si];
        for (uint32_t p = 0; p < s.model.plot_count(); ++p) {
            const std::string& script = s.model.plot(p).lua_script;
            if (script.empty()) continue;
            Script sc;
            sc.plot = p;
            sc.name = script;
            lua::LuaHost host;
            host.store = store_;
            host.types = types_;
            host.rng = rng_;
            host.files = files_;
            host.campaign = true;
            lua::LuaVmOptions opt;
            opt.script_name = script;
            sc.vm = std::make_unique<lua::LuaVm>(host, opt);
            lua::attach_story(*sc.vm, &s.model);
#if defined(ALAMO_LUA_PLAYERS)
            lua::attach_player_world(*sc.vm, &lua_->players);
#endif
            lua::attach_object_bridge(*sc.vm, &lua_->bridge);
            lua::attach_presentation(*sc.vm, &lua_->presentation);
            if (global_values_) lua::attach_global_values(*sc.vm, global_values_);  // Int-final
            std::string err;
            if (!sc.vm->require(script, &err)) lua_note(sc, "require: " + err);
            else if (!sc.vm->call("Base_Definitions", &err)) lua_note(sc, "Base_Definitions: " + err);
            if (sc.vm->create_thread("main") < 0) lua_note(sc, "no main()");
            s.model.set_script_vm(p, sc.vm.get());  // Story_Event_Trigger synchronously at Fire (§1.5)
            s.scripts.push_back(std::move(sc));
            collect(si, "load");  // Story_Event calls from Definitions (none in the shipped scripts)
        }
    }
    return true;
}

void StorySession::host_event(StoryHostEvent e, uint64_t frame) {
    StoryTrigger t;
    t.frame = frame;
    t.kind = e == StoryHostEvent::TacticalStarted ? "TacticalStarted" : "TacticalEnded";
    pending_host_.push_back(std::move(t));
}

StoryTrigger StorySession::translate(const galactic::GalacticEvent& e) const {
    using K = galactic::GalacticEventKind;
    StoryTrigger t;
    t.event = e;
    t.kind = std::string(galactic::galactic_event_name(e.kind));
    const auto& names = sim_->map().planets.name;
    if (e.planet < names.size()) t.planet = names[e.planet];
    if (e.other_planet < names.size()) t.other_planet = names[e.other_planet];
    if (e.type < sim_->rules().types.size()) t.type = sim_->rules().types[e.type].name;
    t.faction = faction_name(e.player);
    if (e.kind == K::PlanetOwnerChanged || e.kind == K::BattleWon || e.kind == K::BattleLost)
        t.other_faction = faction_name(e.other_player);
    if (e.kind == K::CorruptionChanged) t.type = corruption_name(e.value);
    if (e.kind == K::FleetArrived || e.kind == K::FleetDeparted || e.kind == K::FleetBounced) {
        // Contents now; a fleet merged on arrival is gone: use the owner's fleets in orbit there [inferred].
        std::vector<uint32_t> fleets;
        if (e.fleet < sim_->fleets().size() && sim_->fleets()[e.fleet].alive) fleets.push_back(e.fleet);
        else if (e.planet < names.size()) fleets = sim_->fleets_at(e.planet, e.player);
        for (uint32_t f : fleets)
            for (objects::ObjectHandle h : sim_->fleets()[f].contents)
                if (sim_->store().exists(h)) {
                    const uint32_t ty = static_cast<uint32_t>(sim_->store().type(h));
                    if (ty < sim_->rules().types.size()) t.units.push_back(sim_->rules().types[ty].name);
                }
    }
    return t;
}

std::string StorySession::planet_type_name(uint32_t planet) const {
    if (planet >= sim_->planets().size()) return {};
    const uint32_t t = sim_->planets()[planet].type;
    if (t < sim_->rules().types.size()) return sim_->rules().types[t].name;
    const auto& names = sim_->map().planets.name;
    return planet < names.size() ? names[planet] : std::string();
}

// Galactic event -> the story notifications the original raises for it (STORY.md §5 dispatch rows;
// [port] where the doc gives no dispatcher). Planets are named by their TYPE name (CONQUER dispatch:
// "name = conquered planet's type name"); FoC map planet names are the planet type names.
std::vector<std::pair<std::string, lua::StoryNotification>> StorySession::notifications(const StoryTrigger& t) const {
    using K = galactic::GalacticEventKind;
    std::vector<std::pair<std::string, lua::StoryNotification>> out;
    const galactic::GalacticEvent& e = t.event;
    if (t.kind == "TacticalStarted" || t.kind == "TacticalEnded") return out;  // SPACE/LAND_TACTICAL: no source yet
    lua::StoryNotification n;
    n.owner = e.player;
    n.owner_neutral = e.player >= 0 && e.player == sim_->neutral_player();
    n.planet = planet_type_name(e.planet);
    if (t.kind == "PlanetDestroyed") {  // wave 1 poll: Enter family, planet = its type name
        n.has_object = true;
        out.emplace_back("STORY_PLANET_DESTROYED", n);
        return out;
    }
    if (t.kind.rfind("Ui", 0) == 0) {  // wave 1 UI notification (notify_ui)
        const auto kind = static_cast<StoryUiEvent>(e.value);
        n.name = t.type;
        n.planet = t.planet;
        n.owner = e.other_player;
        n.owner_neutral = e.other_player >= 0 && e.other_player == sim_->neutral_player();
        switch (kind) {
            case StoryUiEvent::SelectUnit: n.has_object = e.object != 0; break;
            case StoryUiEvent::SelectPlanet:
            case StoryUiEvent::ZoomIntoPlanet:
            case StoryUiEvent::ZoomOutPlanet:
            case StoryUiEvent::MovieDone:
                if (!n.name.empty()) n.params.push_back(lua::StoryParam::of_text(n.name));  // generic rule, Param1
                break;
            default: break;  // String family: Param1 empty or names n.name
        }
        out.emplace_back(std::string(story_ui_event_type(kind)), n);
        return out;
    }
    // Unit lists for the evaluators: each unit's type, then its Company_Units members [inferred].
    auto expand = [this](std::vector<std::string>& v) {
        std::vector<std::string> all;
        for (const std::string& u : v) {
            all.push_back(u);
            const uint32_t ty = sim_->rules().find_type(u);
            if (same_type(*types_, sim_->rules(), ty))
                for (const objects::TypeIndex m : types_->get_type_refs(ty, "Company_Units"))
                    if (m != objects::kNone && m < sim_->rules().types.size()) all.push_back(sim_->rules().types[m].name);
        }
        v = std::move(all);
    };
    n.units = t.units;
    expand(n.units);
    switch (e.kind) {
        case K::PlanetOwnerChanged:
            n.name = n.planet;
            n.has_object = true;  // the planet; filter = its new owner
            out.emplace_back("STORY_CONQUER", n);
            out.emplace_back("STORY_CONQUER_COUNT", n);
            break;
        case K::ProductionCompleted: {
            n.name = t.type;
            n.has_object = true;  // the planet [M: filter on the producing player]
            out.emplace_back("STORY_CONSTRUCT", n);
            if (e.type < sim_->rules().types.size() && e.planet < sim_->planets().size()) {
                const galactic::UnitKind k = sim_->rules().types[e.type].kind;
                if (k == galactic::UnitKind::StarBase || k == galactic::UnitKind::GroundStructure) {
                    lua::StoryNotification lv = n;
                    lv.category = k == galactic::UnitKind::StarBase ? 0 : 1;
                    lv.value = k == galactic::UnitKind::StarBase ? sim_->planets()[e.planet].starbase_level
                                                                 : sim_->ground_base_level(e.planet);
                    out.emplace_back("STORY_CONSTRUCT_LEVEL", lv);
                }
            }
            break;
        }
        case K::FleetArrived:
            if (e.value == galactic::kArrivePassThrough) break;  // [port] stopping arrivals only
            n.has_object = true;
            out.emplace_back("STORY_ENTER", n);
            out.emplace_back("STORY_MOVE", n);
            break;
        case K::FleetBounced:
            n.has_object = true;
            out.emplace_back("STORY_FLEET_BOUNCED", n);
            break;
        case K::UnitsLanded: {
            // The landed units: the last `value` surface units of the player at the planet [port] (invade
            // appends them in landing order; the stream is routed at the start of the next T9).
            n.has_object = true;
            if (e.planet < sim_->planets().size() && e.value > 0) {
                std::vector<objects::ObjectHandle> mine;
                for (const objects::ObjectHandle u : sim_->planets()[e.planet].surface)
                    if (sim_->store().alive(u) && sim_->store().owner(u) == e.player) mine.push_back(u);
                const size_t k = std::min(mine.size(), static_cast<size_t>(e.value));
                for (size_t i = mine.size() - k; i < mine.size(); ++i) add_unit_names(mine[i], n.units);
            }
            out.emplace_back("STORY_LAND_ON", n);
            out.emplace_back("STORY_DEPLOY", n);  // wave 1: hero / team deployed to the surface (MOVE rule)
            break;
        }
        case K::ObjectDestroyed: {
            n.name = t.type;
            n.has_object = true;
            // Wave 2b (SCRIPT_GAPS.md §4.5): DESTROY only for objects inside a container chain reaching a planet;
            // data2 = that planet (Param2 planet list, the filter tests the planet's owner).
            if (e.planet < sim_->planets().size()) {
                lua::StoryNotification d = n;
                d.has_planet = true;
                d.planet_owner = sim_->planets()[e.planet].owner;
                d.planet_owner_neutral = d.planet_owner >= 0 && d.planet_owner == sim_->neutral_player();
                out.emplace_back("STORY_DESTROY", d);
            }
            // wave 1: a named hero (or a team carrying one) -> STORY_DEFEAT_HERO per hero type name.
            if (same_type(*types_, sim_->rules(), e.type)) {
                std::vector<std::string> heroes;
                auto hero = [this](objects::TypeIndex ty) { return types_->get_bool(ty, "Is_Named_Hero"); };
                if (hero(static_cast<objects::TypeIndex>(e.type))) heroes.push_back(t.type);
                for (const objects::TypeIndex m : types_->get_type_refs(static_cast<objects::TypeIndex>(e.type), "Company_Units"))
                    if (m != objects::kNone && m < sim_->rules().types.size() && hero(m)) heroes.push_back(sim_->rules().types[m].name);
                for (const std::string& h : heroes) {
                    lua::StoryNotification d = n;
                    d.name = h;
                    out.emplace_back("STORY_DEFEAT_HERO", d);
                }
            }
            break;
        }
        case K::CorruptionChanged:
            n.name = t.type;  // corruption type name
            n.value = e.value;  // wave 2b: the corruption type (§4.7 vf3 compares it with the Param2 keyword)
            out.emplace_back("STORY_CORRUPTION_CHANGED", n);
            break;
        // S10 battle results (BattleWon / BattleLost / BattleRetreat, posted by the conflict manager); routed
        // player-scoped (route()): the winner's / loser's / retreating player's plot sets only [inferred].
        // WIN/LOSE_BATTLES (STORY.md 5): counter + Param4 planet list; Param2 location enum and the Param3
        // FRIENDLY/ENEMY_CONTAINS + CLASS_* composition test are not modelled yet (no battle composition in
        // the event). RETREAT: Retreat class, generic rule with Param1 = the planet's type name [inferred].
        // Wave 2a: Param2 location = the event's value (1 land / 2 space); base + Param3 composition from the
        // begin_battle snapshot of the same planet and kind (auto-resolved battles have none: no base, empty sides).
        case K::BattleWon:
        case K::BattleLost: {
            n.has_object = true;
            n.location = e.value;
            const bool snap = battle_ && battle_->planet == e.planet && battle_->kind == e.value;
            if (snap) {
                n.base = battle_->base;
                for (const auto& [owner, mask] : battle_->masks) (owner == e.player ? n.friendly : n.enemy).push_back(mask);
            }
            out.emplace_back(e.kind == K::BattleWon ? "STORY_WIN_BATTLES" : "STORY_LOSE_BATTLES", n);
            // INVASION_BOUNCED [inferred]: a land battle lost by the invader (the snapshot's attacker; without a
            // snapshot, a loser that did not own the planet at the previous service). Enter family.
            if (e.kind == K::BattleLost && e.value == 1 && e.player >= 0) {
                bool invader = false;
                if (snap) invader = battle_->attacker == e.player;
                else if (e.planet < owner_seen_.size()) invader = owner_seen_[e.planet] != e.player;
                if (invader) {
                    lua::StoryNotification b;
                    b.owner = e.player;
                    b.owner_neutral = n.owner_neutral;
                    b.planet = n.planet;
                    b.has_object = true;
                    if (snap)
                        for (const auto& [owner, name] : battle_->names)
                            if (owner == e.player) b.units.push_back(name);
                    out.emplace_back("STORY_INVASION_BOUNCED", b);
                }
            }
            break;
        }
        case K::BattleRetreat:
            n.name = n.planet;
            n.params.push_back(lua::StoryParam::of_text(n.planet));
            out.emplace_back("STORY_RETREAT", n);
            break;
        // --- P4 lua (P4_SIM_SYSTEMS 1.4 / 2.4, STORY.md String class: Param1 empty or names `name`) ---
        // BUY_BLACK_MARKET 0x38: the original's dialog dispatches it at click time for the local human only, with the item
        // Name; the port takes the executed purchase of a human buyer (other_player 1) [port: a purchase refused for
        // credits fires nothing]; AI purchases fire nothing [H].
        case K::BlackMarketPurchase:
            if (e.other_player != 1 || e.value < 0 || static_cast<size_t>(e.value) >= sim_->rules().special.items.size()) break;
            n.name = sim_->rules().special.items[static_cast<size_t>(e.value)].name;
            out.emplace_back("STORY_BUY_BLACK_MARKET", n);
            break;
        // GALACTIC_SABOTAGE 0x39: the command executor and the SABOTAGE_STRUCTURE reward (player -1), structure type name.
        case K::GalacticSabotage:
            n.name = t.type;
            out.emplace_back("STORY_GALACTIC_SABOTAGE", n);
            break;
        // --- end P4 lua ---
        default: break;
    }
    return out;
}

void StorySession::route(const StoryTrigger& t) {
    const auto list = notifications(t);
    // Player-scoped dispatches (STORY.md 1.3: the optional player argument): UI notifications and the S10
    // battle results go to the acting / winning / losing / retreating player's plot sets only.
    using K = galactic::GalacticEventKind;
    const bool battle = t.event.kind == K::BattleWon || t.event.kind == K::BattleLost || t.event.kind == K::BattleRetreat;
    // P4 lua: the buyer's / saboteur's plot sets (as the UI kinds they replace); a reward sabotage (player -1) goes to all.
    const bool special = t.kind.rfind("Ui", 0) != 0 && (t.event.kind == K::BlackMarketPurchase || t.event.kind == K::GalacticSabotage);
    const bool scoped = (battle || special || t.kind.rfind("Ui", 0) == 0) && t.event.player >= 0;
    for (size_t si = 0; si < sets_.size(); ++si) {
        if (scoped && sets_[si]->player != t.event.player) continue;
        for (const auto& [type, n] : list) {
            sets_[si]->model.dispatch(type, n);
            collect(si, "trigger:" + t.kind + "#" + std::to_string(t.index));
        }
    }
}

// Player-scoped game notifications the galactic sim has no event for [port]: TECH_LEVEL every T9 with the
// plot set's tech level, ACCUMULATE when its credits changed.
void StorySession::poll(size_t si) {
    Set& s = *sets_[si];
    if (s.player < 0 || static_cast<size_t>(s.player) >= sim_->players().size()) return;
    lua::StoryNotification n;
    n.value = sim_->players()[static_cast<size_t>(s.player)].tech_level;
    s.model.dispatch("STORY_TECH_LEVEL", n);
    collect(si, "poll");
    s.model.dispatch("STORY_BEGIN_ERA", n);  // wave 1: same BeginEra evaluator (STORY.md 0.1 / 5)
    collect(si, "poll");
    const float credits = sim_->credits(s.player);
    if (!s.credits_seen || credits != s.last_credits) {
        s.credits_seen = true;
        s.last_credits = credits;
        lua::StoryNotification a;
        a.value = static_cast<double>(credits);
        s.model.dispatch("STORY_ACCUMULATE", a);
        collect(si, "poll");
    }
}

void StorySession::apply_reward(Set& s, const std::string& type, const std::vector<std::string>& params) {
    auto at = [&params](size_t i) { return i < params.size() ? params[i] : std::string(); };
    if (type == "SET_TECH_LEVEL") {
        const int32_t player = sim_->player_of_faction(at(0));
        // REMAINING_GAPS 1.4 (0x461fc0): raises the max first when needed, then Set_Tech_Level with the sweep.
        const int32_t level = to_int(at(1), 1);
        if (player >= 0 && sim_->max_tech_level(player) < level) sim_->set_max_tech_level(player, level);
        if (player >= 0) sim_->set_tech_level(player, level, true);
    } else if (type == "CREDITS") {
        if (s.player >= 0) sim_->add_credits(s.player, static_cast<float>(to_int(at(0), 0)), false);
    } else if (type == "LINK_TACTICAL") {  // wave 2a: recorded for the battle host / begin_battle (not applied here)
        StoryLinkedTactical l;
        l.frame = frame_;
        l.plot_set = static_cast<int32_t>(s.number);
        l.params = params;
        linked_.push_back(std::move(l));
    } else if (type == "ENABLE_FOW") {  // fog follow-up (FOG_OF_WAR.md §6.6): no Param1 -> FOW off; atoi(P1) 0 -> off + hidden
        const std::string p1 = std::string(trim(at(0)));
        if (enable_fow_) enable_fow_(!p1.empty(), static_cast<int32_t>(std::atoi(p1.c_str())));
    } else if (type == "SET_TACTICAL_MAP") {
        // S4 (L1_S4_REMAINING.md §4.4, 0x461d90): Reward_Param1 = map file; Reward_Param2 location via the CRC keyword map
        // (SPACE 1 -> space slot; 0 / missing -> land slot; anything else: "invalid location (GROUND/SPACE)", ignored);
        // Reward_Param3 = a faction name kept with the slot (forced faction [M], not applied by the port).
        const std::string loc_text = at(1);
        const std::string_view loc = trim(loc_text);
        const std::string map = std::string(trim(at(0)));
        if (loc.empty() || loc == "0" || iequal(loc, "GROUND") || iequal(loc, "LAND")) tactical_map_[0] = map;
        else if (iequal(loc, "SPACE") || loc == "1") tactical_map_[1] = map;
        else lua_messages_.push_back(std::to_string(frame_) + " story: SET_TACTICAL_MAP failed due to invalid location (GROUND/SPACE): '" +
                                     std::string(loc) + "' (" + std::to_string(params.size()) + " params)");
    }
    // --- P4 lua (P4_SIM_SYSTEMS 1.4 / 2.4) ---
    else if (type == "GIVE_BLACK_MARKET") {  // 0x7E: the item whose Name equals Param1 (exact), to the plot's player, free
        const std::string item = at(0);
        if (s.player >= 0) sim_->bm_give_item(s.player, trim(item));
    } else if (type == "SABOTAGE_STRUCTURE") {
        // 0x81: Param1 planet, Param2 structure type: the first structure of that type there is destroyed (no cost, no
        // saboteur; GALACTIC_SABOTAGE with player -1, counter + 1). Planet by map name, else by planet type name [port].
        const std::string p1 = at(0), p2 = at(1);
        const std::string_view pn = trim(p1);
        const uint32_t type_index = sim_->rules().find_type(trim(p2));
        const auto& names = sim_->map().planets.name;
        PlanetIndex row = kInvalidPlanet;
        for (PlanetIndex i = 0; i < sim_->planets().size() && row == kInvalidPlanet; ++i)
            if ((i < names.size() && iequal(names[i], pn)) || iequal(planet_type_name(i), pn)) row = i;
        if (row != kInvalidPlanet && type_index != galactic::kNoIndex) sim_->sabotage_structure_reward(row, type_index);
    }
    // --- end P4 lua ---
    // --- OI lua (P4_SIM_SYSTEMS 3.3 / 6.4) ---
    else if (type == "LOCK_UNIT") {
        // 0x4D: Param1 = unit type name (shipped data), locked for the plot's player in the same list as the super-weapon
        // hack and Lua Lock_Tech (GalacticSim::lock_type). Unknown type: logged, ignored [port message].
        const std::string p1 = at(0);
        const uint32_t type_index = sim_->rules().find_type(trim(p1));
        if (s.player >= 0 && type_index != galactic::kNoIndex) sim_->lock_type(s.player, type_index, true);
        else if (type_index == galactic::kNoIndex)
            lua_messages_.push_back(std::to_string(frame_) + " story: LOCK_UNIT failed due to unknown unit type '" + p1 + "'");
    } else if (type == "BUILDABLE_UNIT") {
        // REMAINING_GAPS 1.5 item 2 (reward 1, 0x45e8b0): Param1 leaves the locked list and joins the unlocked list,
        // Param2 (the type it supersedes, e.g. Z95 -> X-Wing) joins the locked list; then the Good_Side_Name faction
        // raises its tech when no sliceable candidate remains (GalacticSim::buildable_unit_reward).
        const std::string p1 = at(0), p2 = at(1);
        const uint32_t type_index = sim_->rules().find_type(trim(p1));
        const uint32_t obsolete = trim(p2).empty() ? galactic::kNoIndex : sim_->rules().find_type(trim(p2));
        if (s.player >= 0 && type_index != galactic::kNoIndex) sim_->buildable_unit_reward(s.player, type_index, obsolete);
        else if (type_index == galactic::kNoIndex)
            lua_messages_.push_back(std::to_string(frame_) + " story: " + type + " failed due to unknown unit type '" + p1 + "'");
    } else if (type == "ENABLE_BUILDABLE") {
        // REMAINING_GAPS 1.5 (reward 0x70): Param1 leaves the plot player's production-disabled list (GUI build bar and
        // AI production only; not buildability, not an unlock).
        const std::string p1 = at(0);
        const uint32_t type_index = sim_->rules().find_type(trim(p1));
        if (s.player >= 0 && type_index != galactic::kNoIndex) sim_->set_production_disabled(s.player, type_index, false);
        else if (type_index == galactic::kNoIndex)
            lua_messages_.push_back(std::to_string(frame_) + " story: " + type + " failed due to unknown unit type '" + p1 + "'");
    } else if (type == "REMOVE_CORRUPTION") {
        // Param1 planet (map name, else planet type name [port], as SABOTAGE_STRUCTURE): a corrupted planet starts the
        // timed removal (wrapper 0x3f6770, GRC 4.3) over Param2 seconds (default 10.0; the sim's guard refuses an
        // uncorrupted planet or a running transition). No shipped event uses it.
        const std::string p1 = at(0);
        const std::string_view pn = trim(p1);
        const auto& names = sim_->map().planets.name;
        PlanetIndex row = kInvalidPlanet;
        for (PlanetIndex i = 0; i < sim_->planets().size() && row == kInvalidPlanet; ++i)
            if ((i < names.size() && iequal(names[i], pn)) || iequal(planet_type_name(i), pn)) row = i;
        if (row == kInvalidPlanet)
            lua_messages_.push_back(std::to_string(frame_) + " story: REMOVE_CORRUPTION failed due to unknown planet '" + p1 + "'");
        else {
            const std::string p2 = std::string(trim(at(1)));
            const float seconds = p2.empty() ? 10.0f : std::strtof(p2.c_str(), nullptr);
            sim_->begin_corruption_removal(row, seconds);
        }
    }
    // --- GRC reveal (docs/re/GALACTIC_REVEAL_CORRUPTION.md §1.5, 0x456970) ---
    else if (type == "REVEAL_PLANET") {
        // 0x33: Param1 planet TYPE name, Param2 int; both required (else a no-op): every planet of that type gets the
        // local player's revealed byte := atoi(Param2) != 0.
        const std::string p1 = std::string(trim(at(0))), p2 = std::string(trim(at(1)));
        if (!p1.empty() && !p2.empty()) {
            const uint32_t type_index = sim_->rules().find_type(p1);
            if (type_index != galactic::kNoIndex) sim_->reveal_planets_of_type(type_index, to_int(p2, 0) != 0);
            else lua_messages_.push_back(std::to_string(frame_) + " story: REVEAL_PLANET failed due to unknown planet type '" + p1 + "'");
        }
    } else if (type == "REVEAL_ALL_PLANETS") {  // 0x39
        sim_->reveal_all_planets();
    } else if (type == "ENABLE_GALACTIC_REVEAL") {  // 0x44: auto-reveal := atoi(Param1) != 0; missing Param1 -> logged
        const std::string p1 = std::string(trim(at(0)));
        if (p1.empty()) lua_messages_.push_back(std::to_string(frame_) + " story: ENABLE_GALACTIC_REVEAL failed due to missing Param1");
        else sim_->set_planet_auto_reveal(to_int(p1, 0) != 0);
    }
    else if (type == "SET_PLANET_VISIBILITY_LEVEL") {
        // 0x51 (GRC 2.3, 0x461a60): Param1 planet (map name, else planet type name [port], as REMOVE_CORRUPTION), Param2
        // faction, Param3 level name or "none" (remove), Param4 entry key, Param5 seconds (missing = -1 = never).
        const std::string p1 = at(0);
        const std::string_view pn = trim(p1);
        const auto& names = sim_->map().planets.name;
        PlanetIndex row = kInvalidPlanet;
        for (PlanetIndex i = 0; i < sim_->planets().size() && row == kInvalidPlanet; ++i)
            if ((i < names.size() && iequal(names[i], pn)) || iequal(planet_type_name(i), pn)) row = i;
        const std::string p2 = std::string(trim(at(1))), p3 = std::string(trim(at(2))), p4 = std::string(trim(at(3))),
                          p5 = std::string(trim(at(4)));
        const float seconds = p5.empty() ? -1.0f : static_cast<float>(std::atof(p5.c_str()));
        if (row == kInvalidPlanet)
            lua_messages_.push_back(std::to_string(frame_) + " story: " + type + " failed due to unknown planet '" + p1 + "'");
        else
            sim_->story_set_planet_visibility(row, p2, p3, p4, seconds);
    }
    // --- end GRC reveal ---
    // --- end OI lua ---
    // --- OI galactic (owner: galactic agent; STRATEGIC_GAPS 1.3 / 4, shipped event data) ---
    else if (type == "DISABLE_BUILDABLE") {
        // REMAINING_GAPS 1.5 (reward 0x6F): Param1 joins the plot player's production-disabled list (GUI build bar and
        // AI production only; buildability ignores it). ENABLE_BUILDABLE removes it.
        const std::string p1 = at(0);
        const uint32_t type_index = sim_->rules().find_type(trim(p1));
        if (s.player >= 0 && type_index != galactic::kNoIndex) sim_->set_production_disabled(s.player, type_index, true);
        else if (type_index == galactic::kNoIndex)
            lua_messages_.push_back(std::to_string(frame_) + " story: DISABLE_BUILDABLE failed due to unknown unit type '" + p1 + "'");
    } else if (type == "UNIQUE_UNIT" || type == "SPAWN_HERO") {
        // GALAXY_MAP_PRESENTATION §7.1 (handler 0x4626a0, shared by both rewards): Param1 type, Param2 planet (map name, else
        // planet type name [port]), Param3 copy count (atoi; empty -> 1; < 1 -> none). Anchor = that planet; missing from
        // the map -> the lowest-index planet the plot player owns ([M] the per-owner list order; §7.2: Corellia in Heart of
        // the Maw); none -> fails (logged). Not owned by the player -> the nearest owned planet (squared distance from the
        // anchor, strict <, row order); then an enemy fleet in its orbit (GalacticSim::blockaded) -> the nearest owned
        // planet without one, none -> keep. [port] "fully owned" = PlanetState::owner (the port keeps one owner field).
        // Each copy goes through GalacticSim::spawn_unit (the planet create path 0x400b40; companies as transports).
        const std::string p1 = at(0), p2 = at(1), p3 = at(2);
        const std::string_view pn = trim(p2);
        const uint32_t type_index = sim_->rules().find_type(trim(p1));
        const auto& names = sim_->map().planets.name;
        const auto& planets = sim_->planets();
        PlanetIndex row = kInvalidPlanet;
        if (!pn.empty())
            for (PlanetIndex i = 0; i < planets.size() && row == kInvalidPlanet; ++i)
                if ((i < names.size() && iequal(names[i], pn)) || iequal(planet_type_name(i), pn)) row = i;
        const int32_t pl = s.player;
        auto owned = [&](PlanetIndex i) { return planets[i].is_planet && !planets[i].destroyed && planets[i].owner == pl; };
        if (row == kInvalidPlanet && pl >= 0)
            for (PlanetIndex i = 0; i < planets.size() && row == kInvalidPlanet; ++i)
                if (owned(i)) row = i;
        auto nearest = [&](PlanetIndex from, bool unblocked) {
            PlanetIndex best = kInvalidPlanet;
            float best_d = 0.0f;
            for (PlanetIndex i = 0; i < planets.size(); ++i) {
                if (!owned(i) || (unblocked && sim_->blockaded(i, pl))) continue;
                const float dx = planets[i].position.x - planets[from].position.x, dy = planets[i].position.y - planets[from].position.y,
                            dz = planets[i].position.z - planets[from].position.z;
                const float d = dx * dx + dy * dy + dz * dz;
                if (best == kInvalidPlanet || d < best_d) {
                    best = i;
                    best_d = d;
                }
            }
            return best;
        };
        if (row != kInvalidPlanet && pl >= 0 && !owned(row))
            if (const PlanetIndex n = nearest(row, false); n != kInvalidPlanet) row = n;
        if (row != kInvalidPlanet && pl >= 0 && sim_->blockaded(row, pl))
            if (const PlanetIndex n = nearest(row, true); n != kInvalidPlanet) row = n;
        const int32_t copies = trim(p3).empty() ? 1 : to_int(p3, 1);
        if (pl >= 0 && type_index != galactic::kNoIndex && row != kInvalidPlanet) {
            for (int32_t k = 0; k < copies; ++k) sim_->spawn_unit(pl, row, type_index);
        } else if (!trim(p1).empty()) {
            lua_messages_.push_back(std::to_string(frame_) + " story: " + type + " failed for '" + p1 + "' at '" + p2 + "'");
        }
    } else if (type == "SET_MAX_TECH_LEVEL") {
        // Param1 faction, Param2 level (shipped: Rebel acts I-IV, 1..4): the player's max tech (P +0x88), which
        // buildability step 7 compares with Build_Advances_Tech_Level [inferred from SET_TECH_LEVEL's params].
        const int32_t player = sim_->player_of_faction(at(0));
        if (player >= 0) sim_->set_max_tech_level(player, to_int(at(1), 5));
    } else if (type == "DISABLE_AUTORESOLVE" || type == "ENABLE_AUTORESOLVE") {
        autoresolve_disabled_ = type == "DISABLE_AUTORESOLVE";  // BT §1.6 story lock, global flag [M]
    } else if (type == "RESTRICT_AUTORESOLVE") {
        // Param1 planet (map name, else planet type name [port]), Param2 1 = add to the lock list, 0 = remove [M].
        const std::string p1 = at(0);
        const std::string_view pn = trim(p1);
        const auto& names = sim_->map().planets.name;
        PlanetIndex row = kInvalidPlanet;
        for (PlanetIndex i = 0; i < sim_->planets().size() && row == kInvalidPlanet; ++i)
            if ((i < names.size() && iequal(names[i], pn)) || iequal(planet_type_name(i), pn)) row = i;
        if (row != kInvalidPlanet) {
            const auto it = std::find(autoresolve_planets_.begin(), autoresolve_planets_.end(), row);
            if (to_int(at(1), 1) != 0) {
                if (it == autoresolve_planets_.end()) autoresolve_planets_.push_back(row);
            } else if (it != autoresolve_planets_.end()) {
                autoresolve_planets_.erase(it);
            }
        } else {
            lua_messages_.push_back(std::to_string(frame_) + " story: RESTRICT_AUTORESOLVE failed due to unknown planet '" + p1 + "'");
        }
    }
    // --- end OI galactic ---
}

// --- OI galactic (owner: galactic agent): story lock 0x4472a0 ---
bool StorySession::autoresolve_locked(PlanetIndex planet) const {
    return autoresolve_disabled_ ||
           std::find(autoresolve_planets_.begin(), autoresolve_planets_.end(), planet) != autoresolve_planets_.end();
}
// --- end OI galactic ---

// --- S4 (owner: S4 agent): STORY_LOAD_TACTICAL_MAP / SET_TACTICAL_MAP (L1_S4_REMAINING.md §4.4) ---
void StorySession::load_tactical_map(PlanetIndex planet, bool space, uint64_t frame) {
    frame_ = frame;
    StoryTrigger t;
    t.frame = frame;
    t.index = static_cast<uint32_t>(triggers_.size());
    t.kind = "LoadTacticalMap";
    t.event.kind = galactic::GalacticEventKind::Count;
    t.event.planet = planet;
    t.event.value = space ? 1 : 0;
    const auto& names = sim_->map().planets.name;
    if (planet < names.size()) t.planet = names[planet];
    lines_.push_back(t.text());
    triggers_.push_back(t);
    lua::StoryNotification n;
    n.planet = planet_type_name(planet);
    n.value = space ? 1.0 : 0.0;
    const std::string via = "trigger:LoadTacticalMap#" + std::to_string(t.index);
    for (size_t si = 0; si < sets_.size(); ++si) {  // the current (galactic) story, all plots, synchronously
        Set& s = *sets_[si];
        s.model.load_map_hero = [this, planet](std::string_view type, size_t index) {
            // Param2 hero entry i: the i-th object of that type in the galactic store (newest first [port]), its first
            // container ancestor that is a planet must be the battle planet.
            const objects::TypeIndex ti = types_->resolve_name(type);
            if (ti == objects::kNone) return false;
            const std::vector<objects::ObjectHandle> all = store_->find_all_of_type(ti);
            if (index >= all.size()) return false;
            objects::ObjectHandle h = all[index];
            for (int depth = 0; depth < 8 && h; ++depth) {
                const objects::ObjectHandle c = sim_->container_of(h);
                if (!c) break;
                for (PlanetIndex p = 0; p < sim_->planets().size(); ++p)
                    if (sim_->planets()[p].object == c) return p == planet;
                h = c;
            }
            return false;
        };
        s.model.dispatch("STORY_LOAD_TACTICAL_MAP", n);
        s.model.load_map_hero = {};
        collect(si, via);
    }
}

std::string StorySession::take_tactical_map(bool space) { return std::exchange(tactical_map_[space ? 1 : 0], std::string()); }
// --- end S4 ---

void StorySession::collect(size_t set, std::string_view via) { collect_set(*sets_[set], static_cast<uint32_t>(set), via); }

void StorySession::collect_set(Set& s, uint32_t number, std::string_view via) {
    const auto& fired = s.model.fired();
    for (; s.fired_seen < fired.size(); ++s.fired_seen) {
        const lua::StoryFired& f = fired[s.fired_seen];
        StoryLogEntry e;
        e.frame = frame_;
        e.sequence = f.sequence;
        e.plot_set = number;
        e.plot = f.plot_name;
        e.event = f.event_name;
        e.type = f.event_type;
        e.params = join_params(s.model.plot(f.plot).events[f.event].event_params);
        e.reward = f.reward_type.empty() ? std::string("-") : f.reward_type + join_params(f.reward_params);
        e.via = std::string(via);
        lines_.push_back(e.text());
        log_.push_back(std::move(e));
    }
    auto& errs = s.model.script_errors();
    for (const std::string& m : errs) lua_messages_.push_back(std::to_string(frame_) + " story: " + m);
    errs.clear();
}

// One plot script's Lua service (T9 step 3), gated by its ServiceRate / LastService.
void StorySession::service_script(size_t si, Script& sc, uint64_t frame, double seconds) {
    service_script_in(*sets_[si], static_cast<uint32_t>(si), sc, frame, seconds);
}

void StorySession::service_script_in(Set& s, uint32_t si, Script& sc, uint64_t frame, double seconds) {
    std::string err;
    s.model.deliver_script_triggers(sc.plot, *sc.vm, &err);  // only fires made before the VM existed
    if (!err.empty()) lua_note(sc, err);
    collect_set(s, si, "lua");
    // Service gate: ServiceRate seconds since LastService (MAIN_LOOP 3.4) [inferred: first service at once].
    if (sc.serviced && seconds - sc.last_service < sc.rate) return;
    lua_State* L = sc.vm->state();
    lua_pushstring(L, "LastService");
    lua_pushnumber(L, sc.serviced ? sc.last_service : seconds);
    lua_rawset(L, LUA_GLOBALSINDEX);
    sc.vm->set_time(seconds);
    sc.vm->set_frame(static_cast<uint32_t>(frame));
    sc.vm->service_threads();
    sc.serviced = true;
    sc.last_service = seconds;
    lua_pushstring(L, "ServiceRate");
    lua_rawget(L, LUA_GLOBALSINDEX);
    if (lua_isnumber(L, -1)) {
        const double r = lua_tonumber(L, -1);
        sc.rate = r > 0.0 ? r : kDefaultServiceRate;
    }
    lua_pop(L, 1);
    const auto& errors = sc.vm->errors();
    for (; sc.errors_seen < errors.size(); ++sc.errors_seen) lua_note(sc, errors[sc.errors_seen]);
    collect_set(s, si, "lua");
}

// T9 (STORY.md §4.1): [the port's stand-in for the synchronous game notifications of this tick:
// the galactic stream and host events, then the player-scoped polls] -> STORY_ELAPSED -> per plot in
// table order: Lua service, then (active plots) inactivity timers, FLAG poll, timeouts ->
// OBJECTIVE_TIMEOUT. UNIT_PROXIMITY, CHECK_DESTROYED, FOG reveals and GENERIC battle_end_closed have
// no source yet. Rewards applied by the host raise new galactic events: routed in further rounds.
double StorySession::t9_seconds(uint64_t mode_frame) {
    return lua::StoryModel::t9_seconds(mode_frame, 1.0f / 30.0f);
}

void StorySession::service(uint64_t frame, double mode_seconds) {
    frame_ = frame;
    seconds_ = mode_seconds;
    const bool paused = sim_->paused();
    for (const auto& s : sets_) s->model.set_clock(static_cast<uint32_t>(frame), mode_seconds, paused);
    for (int round = 0; round < kMaxRounds; ++round) {
        // Notifications raised since the last service: galactic stream, then host events.
        scratch_.clear();
        sim_->drain_events(scratch_);
        if (drain_tap_ && !scratch_.empty()) drain_tap_(round, scratch_);  // OI host: presentation tap (read-only)
        std::vector<StoryTrigger> batch;
        batch.reserve(scratch_.size() + pending_host_.size() + pending_ui_.size());
        for (const galactic::GalacticEvent& e : scratch_) batch.push_back(translate(e));
        poll_planets(batch);  // wave 1: PLANET_DESTROYED (no stream event)
        for (StoryTrigger& h : pending_host_) batch.push_back(std::move(h));
        pending_host_.clear();
        for (StoryTrigger& u : pending_ui_) batch.push_back(std::move(u));  // wave 1: notify_ui
        pending_ui_.clear();
        if (round > 0 && batch.empty()) break;
        for (StoryTrigger& t : batch) {
            t.frame = frame;
            t.index = static_cast<uint32_t>(triggers_.size());
            lines_.push_back(t.text());
            triggers_.push_back(t);
            route(triggers_.back());
        }
        if (round > 0) continue;  // the clock and the scripts run once per tick
        for (size_t si = 0; si < sets_.size(); ++si) poll(si);
        // STORY_ELAPSED.
        for (size_t si = 0; si < sets_.size(); ++si) {
            sets_[si]->model.service_elapsed();
            collect(si, "clock");
        }
        // Per plot (table order): Lua service, then the active plot's timers and FLAG poll.
        for (size_t si = 0; si < sets_.size(); ++si) {
            Set& s = *sets_[si];
            for (const uint32_t p : s.model.plot_order()) {
                for (Script& sc : s.scripts)
                    if (sc.plot == p) service_script(si, sc, frame, mode_seconds);
                s.model.service_plot(p);
                collect(si, "clock");
            }
            s.model.service_objective_timeouts();
            collect(si, "clock");
        }
    }
    // Script state of objects that left the store (ids are never reused: pruning is order-free).
    lua_->bridge.prune(*store_);
    // Wave 2a: the tactical story's T9, then the planet owners for the next INVASION_BOUNCED test.
    if (tactical_ && tactical_->set) service_tactical(frame);
    const auto planets = sim_->planets();
    owner_seen_.resize(planets.size(), -1);
    for (size_t i = 0; i < planets.size(); ++i) owner_seen_[i] = planets[i].owner;
}

std::string StorySession::log_text(std::string_view header) const {
    std::string out;
    if (!header.empty()) out += "# " + std::string(header) + "\n";
    out += "# alamo story log v1: T frame index kind fields | F frame set:seq plot event type [params] reward=type[params] via=cause\n";
    for (size_t i = 0; i < sets_.size(); ++i)
        out += "# plot set " + std::to_string(i) + ": player " + std::to_string(sets_[i]->player) + " " + sets_[i]->file +
               " (" + std::to_string(sets_[i]->model.plot_count()) + " plots, " + std::to_string(sets_[i]->scripts.size()) +
               " scripts)\n";
    for (const std::string& t : tactical_headers_) out += t + "\n";
    for (const std::string& l : lines_) out += l + "\n";
    return out;
}

void StorySession::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(sets_.size()));
    for (const auto& s : sets_) {
        h.i32(s->player);
        s->model.hash(h);
        h.boolean(s->credits_seen);
        h.f32(s->last_credits);
    }
    h.u32(static_cast<uint32_t>(triggers_.size()));
#if defined(ALAMO_LUA_PLAYERS)
    lua_->players.hash(h);
#endif
    lua_->bridge.hash(h);
    // Wave 2a: only while a tactical plot set is attached (runs without one hash exactly as before).
    if (tactical_ && tactical_->set) {
        h.u32(tactical_->set->number);
        h.i32(tactical_->set->player);
        tactical_->set->model.hash(h);
        h.u32(static_cast<uint32_t>(tactical_->pending.size()));
        if (tactical_->bridge) tactical_->bridge->hash(h);
    }
    if (!tactical_map_[0].empty() || !tactical_map_[1].empty()) {  // S4: pending SET_TACTICAL_MAP slots (absent otherwise)
        h.str(tactical_map_[0]);
        h.str(tactical_map_[1]);
    }
}

// ============================================================================================ wave 2a

std::string_view tactical_story_event_name(TacticalStoryEvent e) {
    static constexpr std::string_view kNames[] = {"Destroyed",     "HardpointDestroyed", "Reinforced",   "UnitArrived",
                                                  "FullStop",      "Garrisoned",         "HeroCaptured", "StructureCaptured",
                                                  "UnitCommanded", "UnitGuarding",       "BaseDestroyed", "LoadTacticalMap",
                                                  "SelectUnit",    "ClickGui",           "SpeechDone"};
    const auto i = static_cast<size_t>(e);
    return i < std::size(kNames) ? kNames[i] : std::string_view("?");
}
std::string_view tactical_story_event_type(TacticalStoryEvent e) {
    static constexpr std::string_view kTypes[] = {
        "STORY_TACTICAL_DESTROY", "STORY_ATTACK_HARDPOINT", "STORY_REINFORCE",     "STORY_UNIT_ARRIVED",
        "STORY_FULL_STOP",        "STORY_GARRISON_UNIT",    "STORY_CAPTURE_HERO",  "STORY_CAPTURE_STRUCTURE",
        "STORY_COMMAND_UNIT",     "STORY_GUARD_UNIT",       "STORY_DESTROY_BASE",  "STORY_LOAD_TACTICAL_MAP",
        "STORY_SELECT_UNIT",      "STORY_CLICK_GUI",        "STORY_SPEECH_DONE"};
    const auto i = static_cast<size_t>(e);
    return i < std::size(kTypes) ? kTypes[i] : std::string_view();
}

bool StorySession::battle_attached() const { return tactical_ != nullptr; }
int32_t StorySession::tactical_player() const { return tactical_ && tactical_->set ? tactical_->set->player : -1; }  // S4
std::string StorySession::tactical_plots_file() const { return tactical_ && tactical_->set ? tactical_->set->file : std::string(); }
const lua::StoryModel* StorySession::tactical_model() const { return tactical_ && tactical_->set ? &tactical_->set->model : nullptr; }
lua::StoryModel* StorySession::tactical_model() { return tactical_ && tactical_->set ? &tactical_->set->model : nullptr; }

bool StorySession::begin_battle(const StoryBattleStart& s, uint64_t frame, std::vector<std::string>* diagnostics) {
    if (tactical_) end_battle(-1, frame);
    frame_ = frame;
    auto note = [diagnostics](std::string m) {
        if (diagnostics) diagnostics->push_back(std::move(m));
    };
    if (resume_tactical_) {
        // N1 stage 2: the battle of a save (rebuilt by the battle host between the load passes). The battle-start
        // trigger, the STORY_*_TACTICAL dispatch and the snapshot already happened in the saved run (battle_ came from
        // the save); the tactical story is attached again and the saved plots reloaded; read() pass 2 restores them.
        const TacticalResume res = std::move(*resume_tactical_);
        resume_tactical_.reset();
        attach_tactical(s, frame);
        if (!res.has_set) return false;
        if (!s.store || !files_) {
            note("story: the resumed battle's tactical plots " + res.file + " cannot load (no store / file provider)");
            return false;
        }
        return load_tactical(res.file, res.player, diagnostics, res.number);
    }
    battle_.reset();
    // Battle snapshot (request order: defender forces, attacker forces, then the starbase / structures).
    auto b = std::make_unique<Battle>();
    std::vector<int32_t> participants;
    if (const galactic::BattleRequest* r = s.request) {
        b->planet = r->info.planet;
        b->kind = static_cast<int32_t>(r->info.kind);
        b->attacker = r->info.attacker;
        b->defender = r->info.defender;
        b->base = r->starbase ? true : !r->structures.empty();
        const objects::ObjectStore& gst = sim_->store();
        auto add = [&](objects::ObjectHandle h, int32_t owner) {
            if (!gst.exists(h)) return;
            const objects::TypeIndex t = gst.type(h);
            if (t >= types_->raw().type_count()) return;
            b->masks.emplace_back(owner, types_->get_mask(t, "CategoryMask"));
            b->names.emplace_back(owner, type_name_of(*types_, t));
            for (const char* tag : {"Company_Units", "Squadron_Units"})  // team members [inferred]
                for (const objects::TypeIndex m : types_->get_type_refs(t, tag))
                    if (m != objects::kNone && m < types_->raw().type_count()) {
                        b->masks.emplace_back(owner, types_->get_mask(m, "CategoryMask"));
                        b->names.emplace_back(owner, type_name_of(*types_, m));
                    }
        };
        for (const galactic::BattleForces& f : r->forces) {
            if (std::find(participants.begin(), participants.end(), f.player) == participants.end()) participants.push_back(f.player);
            for (const objects::ObjectHandle h : f.start) add(h, f.player);
            for (const objects::ObjectHandle h : f.reserve) add(h, f.player);
        }
        if (r->starbase) add(r->starbase, gst.exists(r->starbase) ? gst.owner(r->starbase) : r->info.defender);
        for (const objects::ObjectHandle h : r->structures) add(h, gst.exists(h) ? gst.owner(h) : r->info.defender);
        for (const int32_t p : {r->info.defender, r->info.attacker})
            if (p >= 0 && std::find(participants.begin(), participants.end(), p) == participants.end()) participants.push_back(p);
    }
    const bool space = b->kind == static_cast<int32_t>(galactic::ConflictKind::Space);
    // Trigger line + STORY_SPACE/LAND_TACTICAL to the participants' galactic plot sets, at once (STORY.md 4.1).
    StoryTrigger t;
    t.frame = frame;
    t.index = static_cast<uint32_t>(triggers_.size());
    t.kind = "BattleStarted";
    t.event.kind = galactic::GalacticEventKind::Count;
    t.event.player = b->attacker;
    t.event.other_player = b->defender;
    t.event.planet = b->planet;
    t.event.value = b->kind;
    t.faction = faction_name(b->attacker);
    t.other_faction = faction_name(b->defender);
    const auto& names = sim_->map().planets.name;
    if (b->planet < names.size()) t.planet = names[b->planet];
    lines_.push_back(t.text());
    triggers_.push_back(t);
    const std::string via = "trigger:BattleStarted#" + std::to_string(t.index);
    const std::string type = space ? "STORY_SPACE_TACTICAL" : "STORY_LAND_TACTICAL";
    std::string plots = s.plots_file;
    int32_t owner = s.local_player;
    int32_t named_by = -1;
    if (s.request) {
        lua::StoryNotification n;
        n.planet = planet_type_name(b->planet);
        n.location = b->kind;
        // Wave 2b (SCRIPT_GAPS.md §4.1): single player only; Param2 planets or no battle planet; Param1 = the plots
        // file for the plot's player (the first one fired names the tactical set [port: one tactical plot set]).
        for (size_t si = 0; si < sets_.size() && !s.multiplayer; ++si) {
            Set& set = *sets_[si];
            if (std::find(participants.begin(), participants.end(), set.player) == participants.end()) continue;
            const size_t before = set.model.fired().size();
            set.model.dispatch(type, n);
            const auto& fired = set.model.fired();
            for (size_t i = before; i < fired.size(); ++i) {
                if (fired[i].event_type != type || !plots.empty()) continue;
                const auto& ep = set.model.plot(fired[i].plot).events[fired[i].event].event_params;
                const std::string file = ep.empty() ? std::string() : std::string(trim(ep[0].text));
                if (!file.empty()) {
                    plots = file;
                    named_by = set.player;
                }
            }
            collect(si, via);
        }
        // A LINK_TACTICAL reward for this planet and kind (Param1 planet, Param2 SPACE / LAND / GROUND, Param7 plots).
        if (plots.empty())
            for (StoryLinkedTactical& l : linked_) {
                if (l.used || l.params.size() < 7 || trim(l.params[6]).empty()) continue;
                const std::string_view where = trim(l.params[0]), kind = trim(l.params[1]);
                const bool kind_ok = space ? iequal(kind, "SPACE") : (iequal(kind, "LAND") || iequal(kind, "GROUND"));
                if (!kind_ok || !(iequal(where, n.planet) || iequal(where, t.planet))) continue;
                plots = std::string(trim(l.params[6]));
                l.used = true;
                if (l.plot_set >= 0 && static_cast<size_t>(l.plot_set) < sets_.size()) named_by = sets_[static_cast<size_t>(l.plot_set)]->player;
                break;
            }
    }
    if (owner < 0) owner = named_by;
    if (owner < 0)
        for (const auto& set : sets_)
            if (std::find(participants.begin(), participants.end(), set->player) != participants.end()) {
                owner = set->player;
                break;
            }
    if (owner < 0) owner = b->attacker;
    battle_ = std::move(b);
    attach_tactical(s, frame);
    if (plots.empty() || !s.store) return false;
    if (!files_) {
        note("story: no file provider for the tactical plots " + plots);
        return false;
    }
    return load_tactical(plots, owner, diagnostics);
}

void StorySession::attach_tactical(const StoryBattleStart& s, uint64_t frame) {
    // The tactical story (attached even without plots: end_battle / notify_tactical then do nothing).
    tactical_ = std::make_unique<Tactical>();
    tactical_->store = s.store;
    tactical_->start_frame = frame;
    tactical_->spf = s.seconds_per_frame > 0.0 ? s.seconds_per_frame : 1.0 / 30.0;
    tactical_->side_destroyed = s.side_destroyed;
    tactical_->fog_point_visible = s.fog_point_visible;
    tactical_->fog_object_visible = s.fog_object_visible;
    tactical_->bridge = s.bridge;
    if (!tactical_->bridge) {
        tactical_->own_bridge = std::make_unique<lua::ObjectBridge>();
        tactical_->own_bridge->is_enemy = lua_->bridge.is_enemy;
        tactical_->own_bridge->pipeline = lua_->bridge.pipeline;  // kept current by set_command_pipeline
        tactical_->bridge = tactical_->own_bridge.get();
    }
    if (!tactical_->bridge->parent_store) tactical_->bridge->parent_store = store_;  // Mark_Parent_Mode_Object_For_Death
}

bool StorySession::load_tactical(const std::string& plots, int32_t player, std::vector<std::string>* diagnostics,
                                 uint32_t number_override) {
    auto set = std::make_unique<Set>();
    set->player = player;
    set->number = number_override != UINT32_MAX ? number_override : static_cast<uint32_t>(sets_.size() + tactical_headers_.size());
    set->file = plots;
    std::string err;
    if (!set->model.load_plots(*files_, plots, &err)) {
        if (diagnostics) diagnostics->push_back("story: " + err);
        return false;
    }
    if (diagnostics)
        for (const std::string& w : set->model.warnings()) diagnostics->push_back("story " + plots + ": " + w);
    set->model.set_player(player);
    set->model.category_mask = category_resolver(*types_);
    set->model.faction_of = faction_resolver(*sim_);  // wave 2b
    // Wave 2b: COMMAND_UNIT (SCRIPT_GAPS.md §4.3) - a live object of that type strictly within range of the point.
    set->model.objects_within = [this](std::string_view type, double x, double y, double z, double range) {
        const objects::ObjectStore* st = tactical_ ? tactical_->store : nullptr;
        if (!st) return false;
        for (const objects::ObjectHandle h : st->all_objects()) {
            if (!st->alive(h) || !iequal(type_name_of(*types_, st->type(h)), type)) continue;
            const Vec3 p = st->position(h);
            const double dx = p.x - x, dy = p.y - y, dz = p.z - z;
            if (dmath::sqrt(dx * dx + dy * dy + dz * dz) < range) return true;
        }
        return false;
    };
    Set* raw = set.get();
    set->model.on_reward = [this, raw](uint32_t, uint32_t, const std::string& type, const std::vector<lua::StoryParam>& params) {
        std::vector<std::string> rp;
        for (size_t i = 0; i < params.size(); ++i) rp.push_back(param_text(params, i));
        apply_reward(*raw, type, rp);
    };
    tactical_->set = std::move(set);
    Set& s = *tactical_->set;
    for (uint32_t p = 0; p < s.model.plot_count(); ++p) {
        const std::string& script = s.model.plot(p).lua_script;
        if (script.empty()) continue;
        Script sc;
        sc.plot = p;
        sc.name = script;
        lua::LuaHost host;
        host.store = tactical_->store;
        host.types = types_;
        host.rng = rng_;
        host.files = files_;
        host.campaign = true;
        lua::LuaVmOptions opt;
        opt.script_name = script;
        sc.vm = std::make_unique<lua::LuaVm>(host, opt);
        lua::attach_story(*sc.vm, &s.model);
#if defined(ALAMO_LUA_PLAYERS)
        lua::attach_player_world(*sc.vm, &lua_->players);
#endif
        lua::attach_object_bridge(*sc.vm, tactical_->bridge);
        lua::attach_presentation(*sc.vm, &lua_->presentation);
        if (global_values_) lua::attach_global_values(*sc.vm, global_values_);  // Int-final
        std::string e;
        if (!sc.vm->require(script, &e)) lua_note(sc, "require: " + e);
        else if (!sc.vm->call("Base_Definitions", &e)) lua_note(sc, "Base_Definitions: " + e);
        if (sc.vm->create_thread("main") < 0) lua_note(sc, "no main()");
        s.model.set_script_vm(p, sc.vm.get());
        s.scripts.push_back(std::move(sc));
        collect_set(s, s.number, "load");
    }
    tactical_headers_.push_back("# tactical plot set " + std::to_string(s.number) + ": player " + std::to_string(player) + " " +
                                plots + " (" + std::to_string(s.model.plot_count()) + " plots, " +
                                std::to_string(s.scripts.size()) + " scripts) from frame " + std::to_string(frame_));
    return true;
}

void StorySession::end_battle(int32_t winner, uint64_t frame) {
    if (!tactical_) return;
    frame_ = frame;
    StoryTrigger t;
    t.frame = frame;
    t.index = static_cast<uint32_t>(triggers_.size());
    t.kind = "BattleEnded";
    t.event.kind = galactic::GalacticEventKind::Count;
    t.event.player = winner;
    t.faction = faction_name(winner);
    lines_.push_back(t.text());
    triggers_.push_back(t);
    if (tactical_->set) {
        Set& s = *tactical_->set;
        // Pending notifications of the last tick first, then VICTORY (Param1 = winner faction names).
        route_tactical(frame);
        const double secs = lua::StoryModel::t9_seconds(frame - tactical_->start_frame, static_cast<float>(tactical_->spf));
        s.model.set_clock(static_cast<uint32_t>(frame), secs, false);
        lua::StoryNotification n;
        n.name = t.faction;
        s.model.dispatch("STORY_VICTORY", n);
        collect_set(s, s.number, "trigger:BattleEnded#" + std::to_string(t.index));
        for (const Script& sc : s.scripts) {
            const auto& errors = sc.vm->errors();
            for (size_t i = sc.errors_seen; i < errors.size(); ++i) lua_note(sc, errors[i]);
        }
    }
    route_corruption_mission(winner, frame);  // wave 2b
    tactical_.reset();
}

void StorySession::set_corruption_mission(int32_t player, int32_t corruption_type) {
    corruption_player_ = player < 0 ? -1 : player;
    corruption_type_ = player < 0 ? -1 : corruption_type;
}

// Wave 2b (SCRIPT_GAPS.md §4.7, GMM battle end 0x28bf10): a pending corruption mission ends with the battle - the
// corrupting player won: corruption applied (0x3f6810) + STORY_CORRUPTION_TACTICAL_COMPLETE, else _FAILED; both with
// (battle planet, type) to the galactic story manager; then the mission is cleared. [port] the player stat and the
// local-player message are not modelled.
void StorySession::route_corruption_mission(int32_t winner, uint64_t frame) {
    if (corruption_player_ < 0) return;
    const int32_t player = corruption_player_, type = corruption_type_;
    corruption_player_ = corruption_type_ = -1;
    const PlanetIndex planet = battle_ ? battle_->planet : kInvalidPlanet;
    const bool won = winner == player;
    if (won && planet < sim_->planets().size() && type >= 0 && type < 8)  // GRC 4.3: timed gain over GMM +0x1FC (5 s)
        sim_->begin_corruption_gain(planet, static_cast<int8_t>(type), player, 5.0f);
    StoryTrigger t;
    t.frame = frame;
    t.index = static_cast<uint32_t>(triggers_.size());
    t.kind = won ? "CorruptionMissionComplete" : "CorruptionMissionFailed";
    t.event.kind = galactic::GalacticEventKind::Count;
    t.event.player = player;
    t.event.planet = planet;
    t.event.value = type;
    t.faction = faction_name(player);
    const auto& names = sim_->map().planets.name;
    if (planet < names.size()) t.planet = names[planet];
    lines_.push_back(t.text());
    triggers_.push_back(t);
    lua::StoryNotification n;
    n.planet = planet_type_name(planet);
    n.value = type;
    n.owner = player;
    const std::string kind = won ? "STORY_CORRUPTION_TACTICAL_COMPLETE" : "STORY_CORRUPTION_TACTICAL_FAILED";
    for (size_t si = 0; si < sets_.size(); ++si) {
        sets_[si]->model.dispatch(kind, n);
        collect(si, "trigger:" + t.kind + "#" + std::to_string(t.index));
    }
}

void StorySession::notify_tactical(const TacticalStoryNotification& n) {
    if (!tactical_ || !tactical_->set || tactical_story_event_type(n.kind).empty()) return;
    const objects::ObjectStore* st = tactical_->store;
    const objects::ObjectHandle h = (st && n.object != 0) ? st->find(n.object) : objects::ObjectHandle{};
    const bool live = h && st->exists(h);
    const objects::ObjectHandle oh = (st && n.other != 0) ? st->find(n.other) : objects::ObjectHandle{};
    const bool other_live = oh && st->exists(oh);
    Tactical::Pending p;
    p.kind = n.kind;
    std::string name = n.name;
    if (name.empty() && live) name = type_name_of(*types_, st->type(h));
    int32_t owner = n.player >= 0 ? n.player : (live ? st->owner(h) : -1);
    if (n.kind == TacticalStoryEvent::HardpointDestroyed)  // the attacker's side is filtered [inferred]
        owner = other_live ? st->owner(oh) : n.player;
    if (other_live) p.target = type_name_of(*types_, st->type(oh));
    p.distance = n.distance;
    if (n.has_position) {  // wave 2b: COMMAND_UNIT destination (else the `other` object's position [port])
        p.has_position = true;
        p.x = n.x;
        p.y = n.y;
        p.z = n.z;
    } else if (n.kind == TacticalStoryEvent::UnitCommanded && other_live) {
        const Vec3 o = st->position(oh);
        p.has_position = true;
        p.x = o.x;
        p.y = o.y;
        p.z = o.z;
    }
    p.owner = owner;
    if (n.kind == TacticalStoryEvent::Destroyed && live) p.heroes = hero_names(*types_, st->type(h));
    StoryTrigger& t = p.trigger;
    t.kind = "Tac" + std::string(tactical_story_event_name(n.kind));
    t.event.kind = galactic::GalacticEventKind::Count;
    t.event.player = owner;
    t.event.object = n.object;
    t.event.value = static_cast<int32_t>(n.kind);
    t.type = name;
    t.faction = faction_name(owner);
    if (!p.target.empty()) t.units.push_back(p.target);
    tactical_->pending.push_back(std::move(p));
}

// Queued notify_tactical() notifications -> the tactical plot set ("the running mode's story manager").
void StorySession::route_tactical(uint64_t frame) {
    Tactical& tac = *tactical_;
    Set& s = *tac.set;
    std::vector<Tactical::Pending> batch;
    batch.swap(tac.pending);
    for (Tactical::Pending& p : batch) {
        p.trigger.frame = frame;
        p.trigger.index = static_cast<uint32_t>(triggers_.size());
        lines_.push_back(p.trigger.text());
        triggers_.push_back(p.trigger);
        lua::StoryNotification n;
        n.name = p.trigger.type;
        n.owner = p.owner;
        n.owner_neutral = p.owner >= 0 && p.owner == sim_->neutral_player();
        n.has_object = p.owner >= 0;
        n.target = p.target;
        n.distance = p.distance;
        n.has_position = p.has_position;
        n.x = p.x;
        n.y = p.y;
        n.z = p.z;
        const std::string via = "trigger:" + p.trigger.kind + "#" + std::to_string(p.trigger.index);
        // S4 (L1_S4_REMAINING.md §4.2): CAPTURE_STRUCTURE data1 = the structure, data2 = the new owner's faction.
        if (p.kind == TacticalStoryEvent::StructureCaptured) n.faction = faction_name(p.owner);
        if (p.kind == TacticalStoryEvent::BaseDestroyed || p.kind == TacticalStoryEvent::LoadTacticalMap)
            n.params.push_back(lua::StoryParam::of_text(n.name));  // generic rule, Param1 = the name
        s.model.dispatch(tactical_story_event_type(p.kind), n);
        collect_set(s, s.number, via);
        for (const std::string& hero : p.heroes) {  // a named hero died: DEFEAT_HERO (wave 1 rule)
            lua::StoryNotification d = n;
            d.name = hero;
            s.model.dispatch("STORY_DEFEAT_HERO", d);
            collect_set(s, s.number, via);
        }
    }
}

// Tactical T9 (STORY.md 4.1 on the tactical story manager): queued notifications, ELAPSED, per plot Lua +
// timers + FLAG, UNIT_PROXIMITY, CHECK_DESTROYED, OBJECTIVE_TIMEOUT.
void StorySession::service_tactical(uint64_t frame) {
    Tactical& tac = *tactical_;
    Set& s = *tac.set;
    const double secs = lua::StoryModel::t9_seconds(frame - tac.start_frame, static_cast<float>(tac.spf));
    s.model.set_clock(static_cast<uint32_t>(frame), secs, false);
    route_tactical(frame);
    s.model.service_elapsed();
    collect_set(s, s.number, "clock");
    for (const uint32_t p : s.model.plot_order()) {
        for (Script& sc : s.scripts)
            if (sc.plot == p) service_script_in(s, s.number, sc, frame, secs);
        s.model.service_plot(p);
        collect_set(s, s.number, "clock");
    }
    poll_tactical();
    s.model.service_objective_timeouts();
    collect_set(s, s.number, "clock");
    if (tac.own_bridge && tac.store) tac.own_bridge->prune(*tac.store);
}

// UNIT_PROXIMITY (STORY.md 4.1 step 4) and CHECK_DESTROYED (step 5) polls - wave 2b: the original rules
// (SCRIPT_GAPS.md §4.8), see the comments inside.
void StorySession::poll_tactical() {
    Tactical& tac = *tactical_;
    Set& s = *tac.set;
    const objects::ObjectStore* st = tac.store;
    if (!st) return;
    const int32_t local = s.player;
    auto upper_eq = [](std::string_view a, std::string_view b) { return iequal(a, b); };
    auto in_list = [&](std::string_view list, std::string_view name) {
        std::string_view rest = list;
        while (!rest.empty()) {
            while (!rest.empty() && (rest.front() == ',' || rest.front() == ' ' || rest.front() == '\t' || rest.front() == '\r' ||
                                     rest.front() == '\n'))
                rest.remove_prefix(1);
            size_t k = 0;
            while (k < rest.size() && rest[k] != ',' && rest[k] != ' ' && rest[k] != '\t' && rest[k] != '\r' && rest[k] != '\n') ++k;
            if (k && upper_eq(rest.substr(0, k), name)) return true;
            rest.remove_prefix(k);
        }
        return false;
    };
    auto param = [](const lua::StoryEventDef& d, size_t i) -> std::string_view {
        return i < d.event_params.size() ? trim(d.event_params[i].text) : std::string_view();
    };
    // A copy: a fired event's reward / Story_Event_Trigger may create or destroy objects.
    const auto span = st->all_objects();
    const std::vector<objects::ObjectHandle> all(span.begin(), span.end());
    // UNIT_PROXIMITY (0x455480 / 0x452030): every local-player object without PROJECTILE is a candidate (Param1
    // empty: any; else its type must be listed); for each Param2 TYPE name (no hint lookup), every live object of
    // that type: fire when the 3D distance is strictly below Param3.
    s.model.poll("STORY_UNIT_PROXIMITY", [&](const lua::StoryEventDef& d) {
        const std::string_view units = param(d, 0), targets = param(d, 1);
        const double range = std::atof(std::string(param(d, 2)).c_str());
        if (targets.empty()) return false;
        for (const objects::ObjectHandle uh : all) {
            if (!st->alive(uh) || st->owner(uh) != local || st->has_behavior(uh, objects::beh::Projectile)) continue;
            if (!units.empty() && !in_list(units, type_name_of(*types_, st->type(uh)))) continue;
            const Vec3 up = st->position(uh);
            for (const objects::ObjectHandle th : all)
                if (st->alive(th) && in_list(targets, type_name_of(*types_, st->type(th))) && dist3(up, st->position(th)) < range)
                    return true;
        }
        return false;
    });
    collect_set(s, s.number, "poll");
    // CHECK_DESTROYED (vf2 0x463c90 / vf3 0x453ce0): Param1 = ONE faction name -> the first player of that faction
    // (unknown: never fires); Param2 = DESTROY_ALL 1 / DESTROY_ALL_UNITS 2 / DESTROY_ALL_STRUCTURES 3 /
    // DESTROY_ALL_INDIGENOUS_SPAWNERS 4; Param3 = the spawner type (4). Fire when:
    //   1: the player has no live SELECTABLE object (counting type 0x372230 [M]: every type);
    //   2: the same, DUMMY_LAND_BASE_LEVEL_COMPONENT objects ignored;
    //   3: the player has no DUMMY_LAND_BASE_LEVEL_COMPONENT object, or exactly one and it is dying;
    //   4: no live indigenous spawner (the global list 0x140a43690 [M]: objects with SPAWN_INDIGENOUS_UNITS) of type
    //      Param3 (no Param3: any live spawner blocks).
    static const objects::BehaviorTypeId kBaseComponent = objects::behavior_type_id("DUMMY_LAND_BASE_LEVEL_COMPONENT");
    static const objects::BehaviorTypeId kSpawner = objects::behavior_type_id("SPAWN_INDIGENOUS_UNITS");
    auto no_selectable = [&](int32_t player, bool units_only) {
        if (tac.side_destroyed) return tac.side_destroyed(player, units_only);  // [port] host override
        for (const objects::ObjectHandle h : all) {
            if (!st->alive(h) || st->owner(h) != player || !st->has_behavior(h, objects::beh::Selectable)) continue;
            if (units_only && st->has_behavior(h, kBaseComponent)) continue;
            return false;
        }
        return true;
    };
    s.model.poll("STORY_CHECK_DESTROYED", [&](const lua::StoryEventDef& d) {
        const std::string_view kw = param(d, 1);
        const int32_t mode = iequal(kw, "DESTROY_ALL") ? 1 : iequal(kw, "DESTROY_ALL_UNITS") ? 2 : iequal(kw, "DESTROY_ALL_STRUCTURES") ? 3
                             : iequal(kw, "DESTROY_ALL_INDIGENOUS_SPAWNERS")                                                 ? 4
                                                                                                                             : 0;
        if (mode == 4) {
            const std::string_view type = param(d, 2);
            for (const objects::ObjectHandle h : all)
                if (st->alive(h) && st->has_behavior(h, kSpawner) && (type.empty() || iequal(type_name_of(*types_, st->type(h)), type)))
                    return false;
            return true;
        }
        const std::string_view faction = param(d, 0);
        const int32_t player = faction.empty() ? -1 : sim_->player_of_faction(std::string(faction));
        if (player < 0 || mode == 0) return false;
        if (mode == 1 || mode == 2) return no_selectable(player, mode == 2);
        uint32_t count = 0;
        bool dying = false;
        for (const objects::ObjectHandle h : all)
            if (st->exists(h) && st->owner(h) == player && st->has_behavior(h, kBaseComponent)) {
                ++count;
                dying = !st->alive(h);
            }
        return count == 0 || (count == 1 && dying);
    });
    collect_set(s, s.number, "poll");
    // Wave 2b: STORY_FOG_POSITION_REVEAL / STORY_FOG_OBJECT_REVEAL through the fog hooks (FOG_OF_WAR.md §6.6).
    if (tac.fog_point_visible) {
        s.model.poll("STORY_FOG_POSITION_REVEAL", [&](const lua::StoryEventDef& d) {
            double v[3] = {0.0, 0.0, 0.0};
            std::string_view rest = param(d, 0);
            for (int k = 0; k < 3 && !rest.empty(); ++k) {
                while (!rest.empty() && (rest.front() == ',' || rest.front() == ' ' || rest.front() == '\t')) rest.remove_prefix(1);
                size_t n = 0;
                while (n < rest.size() && rest[n] != ',' && rest[n] != ' ' && rest[n] != '\t') ++n;
                v[k] = std::atof(std::string(rest.substr(0, n)).c_str());
                rest.remove_prefix(n);
            }
            return tac.fog_point_visible(local, v[0], v[1], v[2]);
        });
        collect_set(s, s.number, "poll");
    }
    if (tac.fog_object_visible) {
        s.model.poll("STORY_FOG_OBJECT_REVEAL", [&](const lua::StoryEventDef& d) {
            const std::string_view type = param(d, 0);
            for (const objects::ObjectHandle h : all)
                if (st->alive(h) && iequal(type_name_of(*types_, st->type(h)), type) && tac.fog_object_visible(local, st->id(h)))
                    return true;
            return false;
        });
        collect_set(s, s.number, "poll");
    }
}

// ------------------------------------------------------------------------------------ N1 save / load
// (owner: n1-host) SimHost chunks "story", "story.lua", "story.bridge", "story.players" (story_session.hpp).
namespace {

constexpr uint32_t kStorySaveVersion = 2;  // 2 (stage 2): + the attached tactical story
constexpr uint32_t kMaxSaveCount = 1u << 24;

uint32_t read_count(ByteReader& r) {
    const uint32_t n = r.u32();
    if (n > kMaxSaveCount) r.fail();
    return r.ok() ? n : 0;
}

void write_strings(ByteWriter& w, const std::vector<std::string>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (const std::string& s : v) w.str(s);
}

bool read_strings(ByteReader& r, std::vector<std::string>& v) {
    const uint32_t n = read_count(r);
    v.clear();
    for (uint32_t i = 0; i < n && r.ok(); ++i) v.push_back(r.str());
    return r.ok();
}

void write_event(ByteWriter& w, const galactic::GalacticEvent& e) {
    w.i32(e.frame).u32(static_cast<uint32_t>(e.kind)).i32(e.player).i32(e.other_player);
    w.u32(static_cast<uint32_t>(e.planet)).u32(static_cast<uint32_t>(e.other_planet)).u32(e.fleet).u32(e.type);
    w.i32(e.object).i32(e.value);
}

bool read_event(ByteReader& r, galactic::GalacticEvent& e) {
    e.frame = r.i32();
    const uint32_t kind = r.u32();
    if (kind > static_cast<uint32_t>(galactic::GalacticEventKind::Count)) r.fail();
    e.kind = static_cast<galactic::GalacticEventKind>(kind);
    e.player = r.i32();
    e.other_player = r.i32();
    e.planet = static_cast<PlanetIndex>(r.u32());
    e.other_planet = static_cast<PlanetIndex>(r.u32());
    e.fleet = r.u32();
    e.type = r.u32();
    e.object = r.i32();
    e.value = r.i32();
    return r.ok();
}

void write_trigger(ByteWriter& w, const StoryTrigger& t) {
    w.u64(t.frame).u32(t.index).str(t.kind);
    write_event(w, t.event);
    w.str(t.planet).str(t.other_planet).str(t.type).str(t.faction).str(t.other_faction);
    write_strings(w, t.units);
}

bool read_trigger(ByteReader& r, StoryTrigger& t) {
    t.frame = r.u64();
    t.index = r.u32();
    t.kind = r.str();
    if (!read_event(r, t.event)) return false;
    t.planet = r.str();
    t.other_planet = r.str();
    t.type = r.str();
    t.faction = r.str();
    t.other_faction = r.str();
    return read_strings(r, t.units);
}

void write_triggers(ByteWriter& w, const std::vector<StoryTrigger>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (const StoryTrigger& t : v) write_trigger(w, t);
}

bool read_triggers(ByteReader& r, std::vector<StoryTrigger>& v) {
    const uint32_t n = read_count(r);
    v.clear();
    v.reserve(n);
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        StoryTrigger t;
        if (!read_trigger(r, t)) return false;
        v.push_back(std::move(t));
    }
    return r.ok();
}

}  // namespace

std::string StorySession::save_blocker() const {
    return {};  // stage 2: the attached tactical story saves too
}

void StorySession::write(ByteWriter& w) const {
    w.u32(kStorySaveVersion);
    w.u64(frame_).f64(seconds_);
    // Plot sets: identity (checked on read), poll state, fired-log cursor, the StoryModel.
    w.u32(static_cast<uint32_t>(sets_.size()));
    for (const auto& sp : sets_) {
        const Set& s = *sp;
        w.i32(s.player).u32(s.number).str(s.file).u32(static_cast<uint32_t>(s.model.plot_count())).u32(static_cast<uint32_t>(s.scripts.size()));
        w.boolean(s.credits_seen).f32(s.last_credits).u64(static_cast<uint64_t>(s.fired_seen));
        write_section(w, [&s](ByteWriter& sub) { s.model.write(sub); });
    }
    write_triggers(w, triggers_);
    write_triggers(w, pending_host_);
    write_triggers(w, pending_ui_);
    w.u32(static_cast<uint32_t>(destroyed_seen_.size()));
    for (const uint8_t d : destroyed_seen_) w.u8(d);
    w.boolean(destroyed_init_);
    w.u32(static_cast<uint32_t>(owner_seen_.size()));
    for (const int32_t o : owner_seen_) w.i32(o);
    w.boolean(battle_ != nullptr);
    if (battle_) {
        const Battle& b = *battle_;
        w.u32(static_cast<uint32_t>(b.planet)).i32(b.kind).i32(b.attacker).i32(b.defender).boolean(b.base);
        w.u32(static_cast<uint32_t>(b.masks.size()));
        for (const auto& [owner, mask] : b.masks) w.i32(owner).u64(mask);
        w.u32(static_cast<uint32_t>(b.names.size()));
        for (const auto& [owner, name] : b.names) w.i32(owner).str(name);
    }
    w.u32(static_cast<uint32_t>(linked_.size()));
    for (const StoryLinkedTactical& l : linked_) {
        w.u64(l.frame).i32(l.plot_set);
        write_strings(w, l.params);
        w.boolean(l.used);
    }
    w.i32(corruption_player_).i32(corruption_type_);
    w.str(tactical_map_[0]).str(tactical_map_[1]);
    w.boolean(autoresolve_disabled_);
    w.u32(static_cast<uint32_t>(autoresolve_planets_.size()));
    for (const PlanetIndex p : autoresolve_planets_) w.u32(static_cast<uint32_t>(p));
    // The run's logs (log_text / log / lua_messages continue across the load).
    write_strings(w, tactical_headers_);
    w.u32(static_cast<uint32_t>(log_.size()));
    for (const StoryLogEntry& e : log_) {
        w.u64(e.frame).u32(e.sequence).u32(e.plot_set);
        w.str(e.plot).str(e.event).str(e.type).str(e.params).str(e.reward).str(e.via);
    }
    write_strings(w, lines_);
    write_strings(w, lua_messages_);
    // v2: the attached tactical story.
    w.boolean(tactical_ != nullptr);
    if (!tactical_) return;
    const Tactical& t = *tactical_;
    w.u64(t.start_frame).f64(t.spf);
    w.boolean(t.set != nullptr);
    if (t.set) {
        const Set& s = *t.set;
        w.i32(s.player).u32(s.number).str(s.file).u32(static_cast<uint32_t>(s.model.plot_count())).u32(static_cast<uint32_t>(s.scripts.size()));
        w.boolean(s.credits_seen).f32(s.last_credits).u64(static_cast<uint64_t>(s.fired_seen));
        write_section(w, [&s](ByteWriter& sub) { s.model.write(sub); });
    }
    w.u32(static_cast<uint32_t>(t.pending.size()));
    for (const Tactical::Pending& p : t.pending) {
        write_trigger(w, p.trigger);
        w.u8(static_cast<uint8_t>(p.kind)).i32(p.owner).str(p.target).f64(p.distance);
        write_strings(w, p.heroes);
        w.boolean(p.has_position).f64(p.x).f64(p.y).f64(p.z);
    }
    w.boolean(t.own_bridge != nullptr);
    if (t.own_bridge) write_section(w, [&t](ByteWriter& sub) { write_if_saveable(*t.own_bridge, sub); });
}

bool StorySession::read(ByteReader& r) {
    const uint32_t version = r.u32();
    if (version != 1 && version != kStorySaveVersion) return false;
    const uint64_t frame = r.u64();
    const double seconds = r.f64();
    struct SetIn {
        bool credits_seen = false;
        float last_credits = 0.0f;
        uint64_t fired_seen = 0;
        std::span<const uint8_t> model;
    };
    const uint32_t nsets = read_count(r);
    if (!r.ok() || nsets != sets_.size()) return false;
    std::vector<SetIn> sets(nsets);
    for (uint32_t i = 0; i < nsets; ++i) {
        const Set& s = *sets_[i];
        const int32_t player = r.i32();
        const uint32_t number = r.u32();
        const std::string file = r.str();
        const uint32_t plots = r.u32(), scripts = r.u32();
        sets[i].credits_seen = r.boolean();
        sets[i].last_credits = r.f32();
        sets[i].fired_seen = r.u64();
        const uint32_t len = r.u32();
        sets[i].model = r.raw(len);
        if (!r.ok() || player != s.player || number != s.number || file != s.file || plots != s.model.plot_count() ||
            scripts != s.scripts.size())
            return false;
    }
    std::vector<StoryTrigger> triggers, pending_host, pending_ui;
    if (!read_triggers(r, triggers) || !read_triggers(r, pending_host) || !read_triggers(r, pending_ui)) return false;
    std::vector<uint8_t> destroyed_seen(read_count(r));
    for (uint8_t& d : destroyed_seen) d = r.u8();
    const bool destroyed_init = r.boolean();
    std::vector<int32_t> owner_seen(read_count(r));
    for (int32_t& o : owner_seen) o = r.i32();
    std::unique_ptr<Battle> battle;
    if (r.boolean()) {
        battle = std::make_unique<Battle>();
        battle->planet = static_cast<PlanetIndex>(r.u32());
        battle->kind = r.i32();
        battle->attacker = r.i32();
        battle->defender = r.i32();
        battle->base = r.boolean();
        const uint32_t nm = read_count(r);
        for (uint32_t i = 0; i < nm && r.ok(); ++i) {
            const int32_t owner = r.i32();
            const uint64_t mask = r.u64();
            battle->masks.emplace_back(owner, mask);
        }
        const uint32_t nn = read_count(r);
        for (uint32_t i = 0; i < nn && r.ok(); ++i) {
            const int32_t owner = r.i32();
            battle->names.emplace_back(owner, r.str());
        }
    }
    std::vector<StoryLinkedTactical> linked(read_count(r));
    for (StoryLinkedTactical& l : linked) {
        l.frame = r.u64();
        l.plot_set = r.i32();
        if (!read_strings(r, l.params)) return false;
        l.used = r.boolean();
    }
    const int32_t corruption_player = r.i32(), corruption_type = r.i32();
    std::string tactical_map0 = r.str(), tactical_map1 = r.str();
    const bool autoresolve_disabled = r.boolean();
    std::vector<PlanetIndex> autoresolve_planets(read_count(r));
    for (PlanetIndex& p : autoresolve_planets) p = static_cast<PlanetIndex>(r.u32());
    std::vector<std::string> tactical_headers, lines, lua_messages;
    if (!read_strings(r, tactical_headers)) return false;
    std::vector<StoryLogEntry> log(read_count(r));
    for (StoryLogEntry& e : log) {
        e.frame = r.u64();
        e.sequence = r.u32();
        e.plot_set = r.u32();
        e.plot = r.str();
        e.event = r.str();
        e.type = r.str();
        e.params = r.str();
        e.reward = r.str();
        e.via = r.str();
    }
    if (!read_strings(r, lines) || !read_strings(r, lua_messages) || !r.ok()) return false;
    // v2: the attached tactical story (stage 2; see the header for the two load passes).
    bool has_tac = false, has_set = false, has_own = false;
    uint64_t tac_start = 0;
    double tac_spf = 0.0;
    TacticalResume tac_id;
    SetIn tac_set;
    std::span<const uint8_t> own_bridge;
    std::vector<Tactical::Pending> pending;
    if (version >= 2) has_tac = r.boolean();
    if (has_tac) {
        tac_start = r.u64();
        tac_spf = r.f64();
        has_set = r.boolean();
        if (has_set) {
            tac_id.has_set = true;
            tac_id.player = r.i32();
            tac_id.number = r.u32();
            tac_id.file = r.str();
            const uint32_t plots = r.u32(), scripts = r.u32();
            tac_set.credits_seen = r.boolean();
            tac_set.last_credits = r.f32();
            tac_set.fired_seen = r.u64();
            tac_set.model = r.raw(r.u32());
            if (!r.ok()) return false;
            if (tactical_) {  // pass 2: the rebuilt battle's tactical set must be the saved one
                const Set* s = tactical_->set.get();
                if (!s || s->player != tac_id.player || s->number != tac_id.number || s->file != tac_id.file ||
                    plots != s->model.plot_count() || scripts != s->scripts.size())
                    return false;
            }
        }
        pending.resize(read_count(r));
        for (Tactical::Pending& p : pending) {
            if (!read_trigger(r, p.trigger)) return false;
            const uint8_t kind = r.u8();
            if (kind > static_cast<uint8_t>(TacticalStoryEvent::Count)) r.fail();
            p.kind = static_cast<TacticalStoryEvent>(kind);
            p.owner = r.i32();
            p.target = r.str();
            p.distance = r.f64();
            if (!read_strings(r, p.heroes)) return false;
            p.has_position = r.boolean();
            p.x = r.f64();
            p.y = r.f64();
            p.z = r.f64();
        }
        has_own = r.boolean();
        if (has_own) own_bridge = r.raw(r.u32());
    }
    if (!r.ok() || !r.done()) return false;
    if (tactical_ && (!has_tac || has_set != (tactical_->set != nullptr) || has_own != (tactical_->own_bridge != nullptr)))
        return false;  // a battle the save does not have, or another tactical story
    // The models last (each StoryModel::read is all-or-nothing; a later model refusing its data leaves the earlier ones
    // loaded - SimHost::load then fails and the session is discarded).
    for (uint32_t i = 0; i < nsets; ++i) {
        Set& s = *sets_[i];
        ByteReader mr(sets[i].model);
        if (!s.model.read(mr) || !mr.ok() || !mr.done()) return false;
        if (sets[i].fired_seen > s.model.fired().size()) return false;
    }
    if (tactical_ && has_set) {
        Set& s = *tactical_->set;
        ByteReader mr(tac_set.model);
        if (!s.model.read(mr) || !mr.ok() || !mr.done() || tac_set.fired_seen > s.model.fired().size()) return false;
    }
    if (tactical_ && has_own) {  // read in place: the tactical VMs hold this bridge
        ByteReader br(own_bridge);
        if (!read_if_saveable(*tactical_->own_bridge, br) || !br.done()) return false;
    }
    if (tactical_) {
        tactical_->start_frame = tac_start;
        tactical_->spf = tac_spf;
        tactical_->pending = std::move(pending);
        if (Set* s = tactical_->set.get()) {
            s->credits_seen = tac_set.credits_seen;
            s->last_credits = tac_set.last_credits;
            s->fired_seen = static_cast<size_t>(tac_set.fired_seen);
            s->model.script_errors().clear();
        }
        resume_tactical_.reset();
    } else if (has_tac) {  // pass 1: the battle host's rebuild resumes this battle (begin_battle)
        resume_tactical_ = std::make_unique<TacticalResume>(std::move(tac_id));
    } else {
        resume_tactical_.reset();
    }
    for (uint32_t i = 0; i < nsets; ++i) {
        Set& s = *sets_[i];
        s.credits_seen = sets[i].credits_seen;
        s.last_credits = sets[i].last_credits;
        s.fired_seen = static_cast<size_t>(sets[i].fired_seen);
        s.model.script_errors().clear();
    }
    frame_ = frame;
    seconds_ = seconds;
    triggers_ = std::move(triggers);
    pending_host_ = std::move(pending_host);
    pending_ui_ = std::move(pending_ui);
    destroyed_seen_ = std::move(destroyed_seen);
    destroyed_init_ = destroyed_init;
    owner_seen_ = std::move(owner_seen);
    battle_ = std::move(battle);
    linked_ = std::move(linked);
    corruption_player_ = corruption_player;
    corruption_type_ = corruption_type;
    tactical_map_[0] = std::move(tactical_map0);
    tactical_map_[1] = std::move(tactical_map1);
    autoresolve_disabled_ = autoresolve_disabled;
    autoresolve_planets_ = std::move(autoresolve_planets);
    tactical_headers_ = std::move(tactical_headers);
    log_ = std::move(log);
    lines_ = std::move(lines);
    lua_messages_ = std::move(lua_messages);
    return true;
}

void StorySession::write_scripts(ByteWriter& w) const {
    auto write_set = [&w](const Set& s) {
        w.u32(static_cast<uint32_t>(s.scripts.size()));
        for (const Script& sc : s.scripts) {
            w.u32(sc.plot).str(sc.name).f64(sc.rate).f64(sc.last_service).boolean(sc.serviced);
            w.u64(static_cast<uint64_t>(sc.errors_seen));
            write_section(w, [&sc](ByteWriter& sub) { sc.vm->write(sub); });
        }
    };
    w.u32(kStorySaveVersion);
    w.u32(static_cast<uint32_t>(sets_.size()));
    for (const auto& sp : sets_) write_set(*sp);
    const Set* tac = tactical_ ? tactical_->set.get() : nullptr;  // v2: the tactical set's scripts
    w.boolean(tac != nullptr);
    if (tac) write_set(*tac);
}

bool StorySession::read_scripts(ByteReader& r) {
    struct ScriptIn {
        uint32_t plot = 0;
        std::string name;
        double rate = 0.0, last_service = 0.0;
        bool serviced = false;
        uint64_t errors_seen = 0;
        std::span<const uint8_t> vm;
    };
    auto parse_set = [&r](std::vector<ScriptIn>& out) {
        out.resize(read_count(r));
        for (ScriptIn& si : out) {
            si.plot = r.u32();
            si.name = r.str();
            si.rate = r.f64();
            si.last_service = r.f64();
            si.serviced = r.boolean();
            si.errors_seen = r.u64();
            si.vm = r.raw(r.u32());
        }
        return r.ok();
    };
    auto matches = [](const Set& s, const std::vector<ScriptIn>& in) {
        if (in.size() != s.scripts.size()) return false;
        for (size_t k = 0; k < in.size(); ++k)
            if (in[k].plot != s.scripts[k].plot || in[k].name != s.scripts[k].name) return false;
        return true;
    };
    const uint32_t version = r.u32();
    if (version != 1 && version != kStorySaveVersion) return false;
    const uint32_t nsets = read_count(r);
    if (!r.ok() || nsets != sets_.size()) return false;
    std::vector<Set*> targets;
    std::vector<std::vector<ScriptIn>> in(nsets);
    for (uint32_t i = 0; i < nsets; ++i) {
        if (!parse_set(in[i]) || !matches(*sets_[i], in[i])) return false;
        targets.push_back(sets_[i].get());
    }
    const bool has_tac = version >= 2 && r.boolean();
    if (has_tac) {
        std::vector<ScriptIn> tac;
        if (!parse_set(tac)) return false;
        if (tactical_) {  // pass 2 (without an attached tactical story: pass 1, the tactical scripts wait for pass 2)
            if (!tactical_->set || !matches(*tactical_->set, tac)) return false;
            in.push_back(std::move(tac));
            targets.push_back(tactical_->set.get());
        }
    }
    if (!r.ok() || !r.done()) return false;
    if (tactical_ && tactical_->set && !has_tac) return false;  // a tactical story the save does not have
    for (size_t i = 0; i < in.size(); ++i)  // each LuaVm::read is all-or-nothing (see read() for a later refusal)
        for (size_t k = 0; k < in[i].size(); ++k) {
            ByteReader vr(in[i][k].vm);
            if (!targets[i]->scripts[k].vm->read(vr) || !vr.ok() || !vr.done()) return false;
        }
    for (size_t i = 0; i < in.size(); ++i)
        for (size_t k = 0; k < in[i].size(); ++k) {
            Script& sc = targets[i]->scripts[k];
            sc.rate = in[i][k].rate;
            sc.last_service = in[i][k].last_service;
            sc.serviced = in[i][k].serviced;
            // Lua errors are diagnostics: only the ones the loaded VM still lists count as seen.
            sc.errors_seen = std::min(static_cast<size_t>(in[i][k].errors_seen), sc.vm->errors().size());
        }
    return true;
}

// The shared families: through their own write / read once engine/lua has them (save_session.hpp
// write_if_saveable); until then "pending" (nothing written, read false).
void StorySession::write_bridge(ByteWriter& w) const { write_if_saveable(lua_->bridge, w); }
bool StorySession::read_bridge(ByteReader& r) { return read_if_saveable(lua_->bridge, r); }

void StorySession::write_players(ByteWriter& w) const {
#if defined(ALAMO_LUA_PLAYERS)
    write_if_saveable(lua_->players, w);
#else
    (void)w;  // no players family: nothing to save
#endif
}

bool StorySession::read_players(ByteReader& r) {
#if defined(ALAMO_LUA_PLAYERS)
    return read_if_saveable(lua_->players, r);
#else
    return r.done();
#endif
}

void StorySession::write_presentation(ByteWriter& w) const { write_if_saveable(lua_->presentation, w); }  // N1 stage 2
bool StorySession::read_presentation(ByteReader& r) { return read_if_saveable(lua_->presentation, r); }

}  // namespace alamo::sim

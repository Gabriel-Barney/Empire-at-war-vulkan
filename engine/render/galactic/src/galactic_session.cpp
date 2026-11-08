#include "alamo/render/galactic/galactic_session.hpp"

#include <filesystem>
#include <fstream>

#include "alamo/commands/command.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::render {

namespace {
constexpr uint32_t kOrderInterval = 90;  // global frames between order rounds
}

// ------------------------------------------------------------------------------------------ session
GalacticSession::GalacticSession(const objects::GameObjectTypes& types, jobs::JobSystem& js, const galactic::GalacticRules& rules,
                                 const galactic::GalacticSetup& setup, const GalacticSessionSpec& spec,
                                 std::vector<std::string>* diagnostics)
    : host_(types, js, [&] {
          sim::SimConfig cfg;
          cfg.seed = spec.seed;
          cfg.players = sim::session_players(setup.players.size());
          return cfg;
      }()),
      lcg_(spec.seed * 2654435761u + 12345u) {
    sim_ = host_.start_galactic(rules, setup, diagnostics);
    if (!sim_) return;
    // Ground units board their owner's orbiting fleet (AI / Lua direct path), planet order then player order.
    const objects::ObjectStore& store = sim_->store();
    for (PlanetIndex p = 0; p < sim_->planets().size(); ++p) {
        for (int32_t pl = 0; pl < static_cast<int32_t>(sim_->players().size()); ++pl) {
            if (pl == sim_->neutral_player() || sim_->fleets_at(p, pl).empty()) continue;
            std::vector<objects::ObjectHandle> units;
            for (objects::ObjectHandle h : sim_->planets()[p].surface)
                if (store.alive(h) && store.owner(h) == pl) units.push_back(h);
            if (units.empty()) continue;
            if (sim_->launch_units(pl, p, units) == commands::RejectReason::None) boarded_ += units.size();
        }
    }
    between();  // UI input before the first tick (R1 / run_map order)
}

uint32_t GalacticSession::next() {
    lcg_ = lcg_ * 1103515245u + 12345u;
    return (lcg_ >> 8) & 0xFFFFFFu;
}

void GalacticSession::between() {
    if (!sim_ || host_.frame() % kOrderInterval != 0) return;
    const objects::ObjectStore& store = sim_->store();
    const std::span<const galactic::PlanetState> planets = sim_->planets();
    const std::span<const galactic::FleetState> fleets = sim_->fleets();
    const galactic::PlanetGraph& graph = sim_->graph();
    const galactic::GalacticRules& rules = sim_->rules();
    for (int32_t pl = 0; pl < static_cast<int32_t>(sim_->players().size()); ++pl) {
        if (pl == sim_->neutral_player()) continue;
        const uint32_t fi = sim_->players()[static_cast<size_t>(pl)].faction;
        if (fi < rules.factions.size() && rules.factions[fi].is_neutral) continue;
        for (size_t f = 0; f < fleets.size(); ++f) {
            const galactic::FleetState& fs = fleets[f];
            if (!fs.alive || fs.owner != pl || fs.moving || fs.planet >= planets.size() || !store.alive(fs.object)) continue;
            const galactic::PlanetState& here = planets[fs.planet];
            if (!here.is_planet || !store.alive(here.object)) continue;
            const auto here_id = static_cast<uint32_t>(store.id(here.object));
            if (here.owner != pl) {
                commands::EventData ev(commands::EventType::Invade);
                ev.set("planet", here_id).set_ids("units", {});
                host_.submit(static_cast<uint8_t>(pl), std::move(ev));
                ++orders_;
                continue;
            }
            const uint32_t r = next();
            if (r % 4 == 0) continue;  // stays this round
            const uint32_t b = graph.edge_begin(fs.planet), e = graph.edge_end(fs.planet);
            if (e <= b) continue;
            const PlanetIndex to = graph.neighbour(b + (r >> 2) % (e - b));
            if (to >= planets.size() || !planets[to].is_planet || !store.alive(planets[to].object)) continue;
            host_.submit(static_cast<uint8_t>(pl), commands::ev::move_object_to_object(static_cast<commands::ObjectId>(store.id(fs.object)),
                                                                                    static_cast<commands::ObjectId>(store.id(planets[to].object))));
            ++orders_;
        }
    }
}

bool GalacticSession::tick() {
    if (!sim_ || !host_.tick_once()) return false;
    ++ticks_;
    between();
    return true;
}

bool GalacticSession::loop_iteration(uint32_t now_ms) {
    if (!sim_ || !host_.loop_iteration(now_ms)) return false;
    ++ticks_;
    between();
    return true;
}

uint64_t GalacticSession::state_hash() const {
    if (!sim_) return 0;
    StateHasher h;
    sim_->hash_state(h);
    return h.digest();
}

namespace {
// Faction name vs Good_Side_Name / Evil_Side_Name: case-insensitive, surrounding blanks ignored.
bool same_name(std::string_view a, std::string_view b) { return ascii_iequals(ascii_trim(a), ascii_trim(b)); }
}  // namespace

GalacticViewOptions galactic_view_options(const galactic::GalacticSim& sim, std::string_view evil_side_name) {
    GalacticViewOptions o;
    o.viewpoint = 0;
    for (size_t p = 0; p < sim.players().size(); ++p)
        if (sim.players()[p].human) {
            o.viewpoint = static_cast<int32_t>(p);
            break;
        }
    const galactic::GalacticRules& rules = sim.rules();
    if (evil_side_name.empty()) evil_side_name = rules.constants.evil_side_name;
    for (const galactic::PlayerState& p : sim.players()) {
        const bool known = p.faction < rules.factions.size();
        o.neutral.push_back((known && rules.factions[p.faction].is_neutral) ? 1 : 0);
        // Route slot by faction (FIXPASS_GALACTIC 3.1): Good_Side_Name -> Good, Evil_Side_Name -> Evil, other -> Corrupt.
        uint8_t side = kRouteSideNone;
        if (known && !rules.factions[p.faction].is_neutral) {
            const std::string& fn = rules.factions[p.faction].name;
            side = same_name(fn, rules.constants.good_side_name) ? kRouteSideGood
                   : same_name(fn, evil_side_name)               ? kRouteSideEvil
                                                                 : kRouteSideOther;
        }
        o.side.push_back(side);
    }
    return o;
}

// ------------------------------------------------------------------------------------ synthetic world
namespace {

bool write_file(const std::filesystem::path& p, const std::string& s) {
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f << s;
    return static_cast<bool>(f);
}

struct SynPlanet {
    const char* name;
    float x, y;
    uint32_t rgba;
};
// 12 planets over +-250 (FoC galaxies span about +-300, Planets.xml), z = 10 like the vanilla positions.
const SynPlanet kSynPlanets[] = {
    {"Syn_Kuat", -220, 120, rgba8(150, 160, 175)},   {"Syn_Corulag", -120, 190, rgba8(90, 140, 210)},
    {"Syn_Anaxes", -60, 60, rgba8(170, 120, 80)},    {"Syn_Fresia", -200, -20, rgba8(80, 170, 90)},
    {"Syn_Bothawui", -90, -110, rgba8(200, 180, 120)}, {"Syn_Nexus", 40, -40, rgba8(230, 230, 235)},
    {"Syn_Mygeeto", 60, 140, rgba8(180, 220, 240)},  {"Syn_Felucia", 170, 200, rgba8(190, 90, 160)},
    {"Syn_Dantooine", 230, 60, rgba8(120, 190, 110)}, {"Syn_Sullust", 150, -60, rgba8(210, 90, 50)},
    {"Syn_Ryloth", 210, -170, rgba8(220, 170, 90)},  {"Syn_Hutta", 40, -190, rgba8(140, 150, 60)},
};
const uint32_t kSynRoutes[][2] = {{0, 1}, {0, 3}, {1, 2}, {1, 6}, {2, 3}, {2, 5}, {2, 6}, {3, 4}, {4, 5},
                                  {4, 11}, {5, 9}, {6, 7}, {7, 8}, {8, 9}, {9, 10}, {10, 11}, {6, 8}};

}  // namespace

std::unique_ptr<GalacticWorld> make_synthetic_galactic_world(const std::string& dir, std::string* error) {
    const std::filesystem::path root = path_from_utf8(dir);
    auto model = [](GalacticPrimitive k, uint32_t c) { return "<Galactic_Model_Name>" + primitive_model_name(k, c) + "</Galactic_Model_Name>"; };
    std::string xml = "<GalacticSynth>\n";
    for (const SynPlanet& p : kSynPlanets)
        xml += std::string(" <Planet Name=\"") + p.name + "\">" + model(GalacticPrimitive::Sphere, p.rgba) +
               "<Scale_Factor>1.8</Scale_Factor><Loop_Idle_Anim_00>Yes</Loop_Idle_Anim_00><Behavior>PLANET</Behavior></Planet>\n";
    xml += " <Planet Name=\"Syn_Galaxy_Core\">" + model(GalacticPrimitive::Disc, rgba8(255, 236, 200)) +
           "<Scale_Factor>420</Scale_Factor><In_Background>yes</In_Background><Behavior>IDLE</Behavior></Planet>\n";
    xml += " <Container Name=\"Galactic_Fleet\">" + model(GalacticPrimitive::Fleet, rgba8(255, 255, 255)) +
           "<Scale_Factor>2.5</Scale_Factor><Behavior>SELECTABLE, FLEET, FLEET_LOCOMOTOR</Behavior></Container>\n";
    xml += " <SpaceUnit Name=\"Syn_Frigate\"><Hyperspace>Yes</Hyperspace><Hyperspace_Speed>2.0</Hyperspace_Speed></SpaceUnit>\n";
    xml += " <SpaceUnit Name=\"Syn_Flagship\"><Galactic_Fleet_Override_Model_Name>" +
           primitive_model_name(GalacticPrimitive::Fleet, rgba8(255, 255, 255)) +
           "</Galactic_Fleet_Override_Model_Name><Scale_Factor>3.2</Scale_Factor></SpaceUnit>\n";
    xml += " <StarBase Name=\"Syn_Starbase\">" + model(GalacticPrimitive::Station, rgba8(255, 255, 255)) +
           "<Scale_Factor>2.0</Scale_Factor></StarBase>\n";
    xml += " <GroundCompany Name=\"Syn_Infantry\"></GroundCompany>\n";
    xml += "</GalacticSynth>\n";
    const bool ok = write_file(root / "Data/XML/GameObjectFiles.xml", "<GameObjectFiles><File>GalacticSynth.xml</File></GameObjectFiles>\n") &&
                    write_file(root / "Data/XML/GalacticSynth.xml", xml);
    if (!ok) {
        if (error) *error = "cannot write the synthetic galactic types under " + dir;
        return nullptr;
    }
    auto w = std::make_unique<GalacticWorld>();
    w->name = "synthetic";
    w->synthetic = true;
    w->files = std::make_unique<DirectoryProvider>(dir);
    if (!w->db.load(*w->files) || !w->types.build(w->db)) {
        if (error) *error = "synthetic galactic types did not load";
        return nullptr;
    }
    using galactic::FactionRules;
    using galactic::TypeInfo;
    using galactic::UnitKind;
    galactic::GalacticRules& r = w->rules;
    FactionRules emp, reb, und, neu;
    emp.name = "Empire";
    reb.name = "Rebel";
    und.name = "Underworld";
    neu.name = "Neutral";
    emp.can_control_planets = emp.is_playable = reb.can_control_planets = reb.is_playable = true;
    und.can_control_planets = und.is_playable = und.benefits_from_corruption = true;
    neu.is_neutral = true;
    r.factions = {emp, reb, und, neu};
    r.difficulties = {{"Normal", 1.0f, 1.0f}};
    r.types.assign(w->types.size(), TypeInfo{});
    auto type = [&w](const char* n) { return w->types.resolve_name(n); };
    for (objects::TypeIndex t = 0; t < r.types.size(); ++t) r.types[t].name = std::string(w->types.raw().types().name[t]);
    for (const SynPlanet& p : kSynPlanets) {
        TypeInfo& t = r.types[type(p.name)];
        t.kind = UnitKind::Planet;
        t.credit_value = 20;
        t.capture_bonus = 100;
    }
    const objects::TypeIndex frig = type("Syn_Frigate"), flag = type("Syn_Flagship"), sb = type("Syn_Starbase"), inf = type("Syn_Infantry");
    for (objects::TypeIndex t : {frig, flag}) {
        r.types[t].kind = UnitKind::SpaceUnit;
        r.types[t].hyperspace = true;
        r.types[t].hyperspace_speed = 2.0f;
        r.types[t].ai_combat_power = 600.0f;
        r.types[t].build_cost = 800;
        r.types[t].build_time_seconds = 10;
    }
    r.types[sb].kind = UnitKind::StarBase;
    r.types[sb].base_level = 1;
    r.types[sb].ai_combat_power = 1500.0f;
    r.types[inf].kind = UnitKind::GroundUnit;
    // Map: 12 planet rows, then the background galaxy disc (scenery: no PLANET behaviour, no object).
    galactic::GalacticSetup& s = w->setup;
    GalaxyMap& m = s.map;
    m.campaign_name = "Synthetic_Galaxy";
    for (const SynPlanet& p : kSynPlanets) {
        const PlanetIndex i = m.planets.add(p.name);
        m.planets.position[i] = {p.x, p.y, 10.0f};
        m.planets.flags[i] = kPlanetHasPlanetBehavior;
        s.planet_type.push_back(type(p.name));
    }
    const PlanetIndex core = m.planets.add("Syn_Galaxy_Core");
    m.planets.position[core] = {0, 0, -60.0f};
    m.planets.flags[core] = kPlanetInBackground;
    s.planet_type.push_back(galactic::kNoIndex);
    for (const auto& rt : kSynRoutes) {
        const uint32_t i = m.routes.add(std::string(kSynPlanets[rt[0]].name) + "_" + kSynPlanets[rt[1]].name);
        m.routes.point_a[i] = rt[0];
        m.routes.point_b[i] = rt[1];
        m.routes.hs_speed_factor[i] = 6.0f;
    }
    galactic::PlayerSetup pe, pr, pu, pn;
    pe.faction = 0;
    pe.human = true;
    pe.credits = 5000.0f;
    pr.faction = 1;
    pr.ai = true;
    pr.difficulty = 0;
    pr.credits = 5000.0f;
    pu.faction = 2;
    pu.ai = true;
    pu.difficulty = 0;
    pu.credits = 5000.0f;
    pn.faction = 3;
    s.players = {pe, pr, pu, pn};
    auto home = [&s, sb, frig, flag, inf](int32_t player, PlanetIndex at, PlanetIndex second, int frigates) {
        s.forces.push_back({player, at, sb});
        for (int i = 0; i < frigates; ++i) s.forces.push_back({player, at, frig});
        s.forces.push_back({player, at, flag});
        s.forces.push_back({player, at, inf});
        s.forces.push_back({player, at, inf});
        s.forces.push_back({player, second, inf});
    };
    home(0, 0, 1, 3);
    home(1, 8, 7, 3);
    home(2, 11, 10, 2);
    // Stand-in faction table (Factions.xml values; Display_Font_Color = Factions.xml's for Empire / Rebel / Neutral).
    w->faction_colors = {{"Empire", false, true, {54 / 255.0f, 134 / 255.0f, 242 / 255.0f, 1}, {1, 1, 1, 1}, true, {24 / 255.0f, 133 / 255.0f, 251 / 255.0f, 1}, false, {}},
                         {"Rebel", false, true, {185 / 255.0f, 40 / 255.0f, 39 / 255.0f, 1}, {1, 1, 1, 1}, true, {185 / 255.0f, 40 / 255.0f, 39 / 255.0f, 1}, false, {}},
                         {"Underworld", false, true, {240 / 255.0f, 200 / 255.0f, 40 / 255.0f, 1}, {1, 1, 1, 1}, true, {240 / 255.0f, 200 / 255.0f, 40 / 255.0f, 1}, false, {}},
                         {"Neutral", true, true, {100 / 255.0f, 100 / 255.0f, 100 / 255.0f, 1}, {0.7f, 0.7f, 0.7f, 1}, true, {128 / 255.0f, 128 / 255.0f, 128 / 255.0f, 1}, false, {}}};
    return w;
}

// -------------------------------------------------------------------------------------- real data
std::unique_ptr<GalacticWorld> load_galactic_world(std::unique_ptr<FileProvider> files, const std::string& campaign, std::string* error) {
    auto w = std::make_unique<GalacticWorld>();
    w->name = campaign.empty() ? std::string(kDefaultGalacticCampaign) : campaign;
    w->files = std::move(files);
    if (!w->files) {
        if (error) *error = "no file provider";
        return nullptr;
    }
    w->gdb = std::make_unique<data::GameDatabase>();
    if (!w->gdb->load(*w->files, {}, &w->diagnostics) || !w->db.load(*w->files, &w->diagnostics) ||
        !w->types.build(w->db, w->files.get(), &w->diagnostics) ||
        !galactic::load_galactic_rules(*w->files, *w->gdb, w->types, w->rules, &w->diagnostics)) {
        if (error) *error = "game data incomplete (" + std::to_string(w->diagnostics.size()) + " diagnostics)";
        return nullptr;
    }
    w->setup = galactic::build_campaign_setup(*w->gdb, w->rules, w->name, {}, &w->diagnostics);
    if (w->setup.map.planets.size() == 0) {
        if (error) *error = "campaign '" + w->name + "' has no planets";
        return nullptr;
    }
    w->faction_colors = load_faction_colors(*w->files);
    w->route_style = load_route_line_style(*w->files);
    w->corruption_path = load_corruption_path_settings(w->gdb->constants().element);  // cu5 t04: the parsed GameConstants
    return w;
}

}  // namespace alamo::render

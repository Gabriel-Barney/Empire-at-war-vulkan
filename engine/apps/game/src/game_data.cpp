#include "alamo/game/game_data.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>

#include "alamo/sim/save_session.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/ai_types.hpp"
#endif

namespace alamo::game {

namespace {

bool load_common(GameData& out, std::string* err) {
    std::vector<std::string>& diag = out.diagnostics;
    if (!out.db.load(*out.files, {}, &diag)) {
        if (err) *err = "game database: load failed under " + out.root;
        return false;
    }
    if (!out.raw.load(*out.files, &diag) || !out.types.build(out.raw, out.files.get(), &diag)) {
        if (err) *err = "GameObjectFiles.xml not found / object types failed under " + out.root;
        return false;
    }
    if (!galactic::load_galactic_rules(*out.files, out.db, out.types, out.rules, &diag)) {
        if (err) *err = "galactic rules: load failed";
        return false;
    }
    galactic::load_conflict_rules(*out.files, out.db, out.types, out.rules, out.crules, &diag);  // missing parts: defaults
    out.catalog = std::make_unique<sim::BattleCatalog>(out.types, out.files.get(), &out.db.constants(), &diag);
    return true;
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

uint32_t neutral_faction(const galactic::GalacticRules& rules) {
    for (uint32_t f = 0; f < rules.factions.size(); ++f)
        if (rules.factions[f].is_neutral) return f;
    return galactic::kNoIndex;
}

}  // namespace

bool load_game_data(const std::string& root, const std::vector<std::string>& args, GameData& out, std::string* err) {
    vfs::VfsConfig cfg;  // mounted exactly as alamo_headless does
    cfg.foc_root = root;
    vfs::parse_command_line(args, cfg);
    std::string e;
    std::unique_ptr<vfs::Vfs> v = vfs::Vfs::create(cfg, &e);
    if (!v) {
        if (err) *err = "VFS: " + e;
        return false;
    }
    out.root = root;
    out.args = args;
    out.vfs = v.get();
    out.files = std::move(v);
    if (!load_common(out, err)) return false;
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    {  // AI data (as alamo_headless --galactic-ai): definitions, equations, plan scripts, difficulties, type facts
        auto a = std::make_shared<AiData>();
        const vfs::Vfs* vp = out.vfs;
        const auto lister = [vp](std::string_view g) { return vp->list(g); };
        if (ai::load_ai_definitions(*out.files, ai::list_ai_data(lister), a->defs)) {
            ai::load_equation_files(*out.files, vp->list(ai::kPerceptionEquationGlob), a->equations);
            a->plan_files = ai::list_plan_files(lister);
            a->difficulties = ai::load_ai_difficulties(*out.files);
            a->type_info = ai::make_galactic_ai_type_info(out.types);
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
            auto ts = std::make_shared<sim::TacticalAiSetup>();
            ts->defs = &a->defs;
            ts->equations = &a->equations;
            ts->files = out.files.get();
            ts->space_plans = ai::tactical::tactical_plan_files(ai::tactical::TacticalMode::Space, lister);
            ts->land_plans = ai::tactical::tactical_plan_files(ai::tactical::TacticalMode::Land, lister);
            ts->difficulties = ai::tactical::load_tactical_ai_difficulties(*out.files);
            if (!ts->space_plans.empty() || !ts->land_plans.empty()) a->tactical = std::move(ts);
#endif
            out.ai = std::move(a);
        } else {
            out.diagnostics.push_back("AI definitions not found: AI players use the S10 stand-in");
        }
    }
#endif
    return true;
}

uint64_t GameData::types_fingerprint() const {
    if (!types_fp_done_) {
        types_fp_ = sim::fingerprint_types(types);
        types_fp_done_ = true;
    }
    return types_fp_;
}

std::string ai_type_for(const std::string& faction) {
    const std::string f = upper(faction);
    if (f == "EMPIRE") return "BasicEmpire";
    if (f == "REBEL") return "BasicRebel";
    if (f == "UNDERWORLD") return "AI_Player_Underworld";
    return {};
}

bool load_game_data_dir(const std::string& dir, GameData& out, std::string* err) {
    out.root = dir;
    out.vfs = nullptr;
    out.files = std::make_unique<DirectoryProvider>(dir);
    return load_common(out, err);
}

std::vector<std::string> default_skirmish_fleet(const std::string& faction) {
    const std::string f = upper(faction);
    if (f == "EMPIRE")
        return {"Star_Destroyer", "Tartan_Patrol_Cruiser", "Tartan_Patrol_Cruiser", "Acclamator_Assault_Ship", "Broadside_Class_Cruiser"};
    if (f == "REBEL")
        return {"Calamari_Cruiser", "Nebulon_B_Frigate", "Corellian_Corvette", "Corellian_Corvette", "Alliance_Assault_Frigate"};
    if (f == "UNDERWORLD")
        return {"Vengeance_Frigate", "Kedalbe_Battleship", "Interceptor4_Frigate", "Crusader_Gunship", "Crusader_Gunship"};
    return {};
}

std::vector<std::string> default_skirmish_ground(const std::string& faction) {
    const std::string f = upper(faction);
    if (f == "EMPIRE") return {"Imperial_Stormtrooper_Squad", "Imperial_Stormtrooper_Squad", "Imperial_Armor_Group", "Imperial_Anti_Infantry_Brigade"};
    if (f == "REBEL") return {"Rebel_Infantry_Squad", "Rebel_Infantry_Squad", "Rebel_Light_Tank_Brigade", "Rebel_Tank_Buster_Squad"};
    if (f == "UNDERWORLD") return {"Underworld_Merc_Squad", "Underworld_Merc_Squad", "Underworld_Disruptor_Merc_Squad", "MZ8_Pulse_Cannon_Tank_Company"};
    return {};
}

bool make_skirmish_galaxy(const GameData& data, const SkirmishSetup& s, galactic::GalacticSetup& g, std::string* err) {
    const galactic::GalacticRules& rules = data.rules;
    g = galactic::GalacticSetup{};
    galactic::PlayerSetup human, ai, neutral;
    human.faction = rules.find_faction(s.human_faction);
    human.human = true;
    ai.faction = rules.find_faction(s.ai_faction);
    ai.ai = true;
    // t50: a land skirmish side starts with MP_Default_Credits (GameConstants, shipped 6000): the pads' Tactical_Build_Cost_Multiplayer
    // charges the galactic account. A space skirmish buys nothing in the battle, so its sides keep 0.
    if (s.land) human.credits = ai.credits = static_cast<float>(std::max(0, data.db.constants().get_int("MP_Default_Credits", 0)));
    neutral.faction = neutral_faction(rules);
    if (human.faction == galactic::kNoIndex || ai.faction == galactic::kNoIndex) {
        if (err) *err = "skirmish: faction " + (human.faction == galactic::kNoIndex ? s.human_faction : s.ai_faction) + " unknown";
        return false;
    }
    uint32_t planet = galactic::kNoIndex;
    if (!s.planet_type.empty()) planet = rules.find_type(s.planet_type);
    for (const char* n : {"Kuat", "Coruscant"})
        if (planet == galactic::kNoIndex) planet = rules.find_type(n);
    for (uint32_t t = 0; t < rules.types.size() && planet == galactic::kNoIndex; ++t)
        if (rules.types[t].kind == galactic::UnitKind::Planet) planet = t;
    if (planet == galactic::kNoIndex) {
        if (err) *err = "skirmish: no planet type";
        return false;
    }
    GalaxyMap& m = g.map;
    m.campaign_name = "Alamo_Skirmish";
    for (uint32_t i = 0; i < 3; ++i) {
        m.planets.add(std::string("ALAMO_SKIRMISH_P") + std::to_string(i));
        m.planets.position[i] = {static_cast<float>(i) * 60.0f, 0.0f, 0.0f};
        m.planets.flags[i] = static_cast<uint32_t>(kPlanetHasPlanetBehavior);
    }
    for (uint32_t i = 0; i + 1 < 3; ++i) {
        const uint32_t r = m.routes.add("ALAMO_SKIRMISH_R" + std::to_string(i));
        m.routes.point_a[r] = i;
        m.routes.point_b[r] = i + 1;
        m.routes.hs_speed_factor[r] = 5.0f;
    }
    g.planet_type = {planet, planet, planet};
    g.players = {human, ai};
    if (neutral.faction != galactic::kNoIndex) g.players.push_back(neutral);
    // S10 map owners [probe 2026-10-29]: retail skirmish has Neutral and Hostile players besides the slots; map objects
    // resolve their owner by faction (TacticalBattle::bt_map_owner), natives without a player go to Hostile.
    galactic::PlayerSetup hostile;
    hostile.faction = rules.find_faction("Hostile");
    if (hostile.faction != galactic::kNoIndex && hostile.faction != human.faction && hostile.faction != ai.faction) g.players.push_back(hostile);
    auto add = [&](int32_t player, PlanetIndex at, const std::vector<std::string>& names, const std::string& faction, bool ground = false) {
        const std::vector<std::string> list = !names.empty() ? names : ground ? default_skirmish_ground(faction) : default_skirmish_fleet(faction);
        uint32_t n = 0;
        for (const std::string& name : list)
            if (const uint32_t t = rules.find_type(name); t != galactic::kNoIndex) {
                g.forces.push_back({player, at, t});
                ++n;
            }
        return n;
    };
    // Defender (AI) holds the battle planet first, then the human's forces at the far planet.
    uint32_t na = 0, nh = 0;
    if (s.land) {
        na = add(1, kSkirmishBattlePlanet, s.ground_ai, s.ai_faction, true);
        const std::vector<std::string> fleet = s.units_human.empty() ? default_skirmish_fleet(s.human_faction) : s.units_human;
        if (!fleet.empty() && add(0, kSkirmishHumanPlanet, {fleet.front()}, s.human_faction) > 0)  // the carrier fleet
            nh = add(0, kSkirmishHumanPlanet, s.ground_human, s.human_faction, true);
    } else {
        na = add(1, kSkirmishBattlePlanet, s.units_ai, s.ai_faction);
        nh = add(0, kSkirmishHumanPlanet, s.units_human, s.human_faction);
        if (nh > 0 && !s.ground_human.empty()) add(0, kSkirmishHumanPlanet, s.ground_human, s.human_faction, true);  // P4 host
    }
    if (na == 0 || nh == 0) {
        if (err) *err = "skirmish: no known unit type in the " + std::string(nh == 0 ? "human" : "AI") + " fleet";
        return false;
    }
    return true;
}

namespace {
std::vector<std::string> list_maps(const GameData& data, const char* glob) {
    std::vector<std::string> out;
    if (!data.vfs) return out;
    for (const std::string& p : data.vfs->list(glob)) {
        std::string stem = p;
        if (const size_t s = stem.find_last_of("\\/"); s != std::string::npos) stem = stem.substr(s + 1);
        if (const size_t d = stem.find_last_of('.'); d != std::string::npos) stem = stem.substr(0, d);
        stem = upper(stem);
        if (std::find(out.begin(), out.end(), stem) == out.end()) out.push_back(stem);
    }
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

std::vector<std::string> skirmish_land_maps(const GameData& data) { return list_maps(data, "Data\\Art\\Maps\\_MP_LAND_*.ted"); }

std::string skirmish_map_planet_type(const galactic::GalacticRules& rules, std::string_view map) {
    std::string stem(map);
    if (const size_t s = stem.find_last_of("\\/"); s != std::string::npos) stem = stem.substr(s + 1);
    if (const size_t d = stem.find_last_of('.'); d != std::string::npos) stem = stem.substr(0, d);
    stem = upper(stem);
    for (const std::string_view prefix : {std::string_view("_MP_SPACE_"), std::string_view("_MP_LAND_")}) {
        if (stem.size() <= prefix.size() || stem.compare(0, prefix.size(), prefix) != 0) continue;
        const uint32_t t = rules.find_type(std::string_view(stem).substr(prefix.size()));
        if (t != galactic::kNoIndex && rules.types[t].kind == galactic::UnitKind::Planet) return rules.types[t].name;
    }
    return {};
}

std::vector<std::string> skirmish_space_maps(const GameData& data) {
    std::vector<std::string> out;
    if (!data.vfs) return out;
    for (const std::string& p : data.vfs->list("Data\\Art\\Maps\\_MP_SPACE_*.ted")) {
        std::string stem = p;
        if (const size_t s = stem.find_last_of("\\/"); s != std::string::npos) stem = stem.substr(s + 1);
        if (const size_t d = stem.find_last_of('.'); d != std::string::npos) stem = stem.substr(0, d);
        stem = upper(stem);
        if (std::find(out.begin(), out.end(), stem) == out.end()) out.push_back(stem);
    }
    std::sort(out.begin(), out.end());
    if (const auto it = std::find(out.begin(), out.end(), "_MP_SPACE_KUAT"); it != out.end()) std::rotate(out.begin(), it, it + 1);
    return out;
}

}  // namespace alamo::game

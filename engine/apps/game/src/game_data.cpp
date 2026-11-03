#include "alamo/game/game_data.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <filesystem>

#include "alamo/sim/save_session.hpp"
#include "alamo/terrain/chunk.hpp"
#include "alamo/terrain/ted.hpp"
#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
#include "alamo/ai/ai_lua_runtime.hpp"
#include "alamo/ai/ai_types.hpp"
#endif

namespace alamo::game {

namespace {

// Q5: runs one loader step and records it in out.stages (name, its diagnostics, time, result).
template <class F>
bool stage(GameData& out, const char* name, F&& step) {
    GameData::LoadStage s;
    s.name = name;
    s.first_diag = out.diagnostics.size();
    const auto t0 = std::chrono::steady_clock::now();
    s.ok = step(out.diagnostics);
    s.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    s.diag_count = out.diagnostics.size() - s.first_diag;
    out.stages.push_back(std::move(s));
    return out.stages.back().ok;
}

bool load_common(GameData& out, std::string* err) {
    if (!stage(out, "game database", [&](std::vector<std::string>& diag) { return out.db.load(*out.files, {}, &diag); })) {
        if (err) *err = "game database: load failed under " + out.root;
        return false;
    }
    if (!stage(out, "object types", [&](std::vector<std::string>& diag) {
            return out.raw.load(*out.files, &diag) && out.types.build(out.raw, out.files.get(), &diag);
        })) {
        if (err) *err = "GameObjectFiles.xml not found / object types failed under " + out.root;
        return false;
    }
    if (!stage(out, "galactic rules", [&](std::vector<std::string>& diag) {
            return galactic::load_galactic_rules(*out.files, out.db, out.types, out.rules, &diag);
        })) {
        if (err) *err = "galactic rules: load failed";
        return false;
    }
    stage(out, "conflict rules", [&](std::vector<std::string>& diag) {
        galactic::load_conflict_rules(*out.files, out.db, out.types, out.rules, out.crules, &diag);  // missing parts: defaults
        return true;
    });
    stage(out, "battle catalog", [&](std::vector<std::string>& diag) {
        out.catalog = std::make_unique<sim::BattleCatalog>(out.types, out.files.get(), &out.db.constants(), &diag);
        return true;
    });
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
    stage(out, "AI data", [&](std::vector<std::string>&) {  // as alamo_headless --galactic-ai: definitions, equations,
        auto a = std::make_shared<AiData>();                  // plan scripts, difficulties, type facts
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
        return true;
    });
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

namespace {
constexpr std::string_view kOfficialDir = "DATA\\ART\\MAPS\\";

// The header of one candidate through the normal lookup (UI.md 9.1 / 9.3).
SkirmishMapEntry read_map_entry(const FileProvider& files, const std::string& path) {
    SkirmishMapEntry e;
    e.path = path;
    e.map = path;
    if (path.size() > kOfficialDir.size() && path.compare(0, kOfficialDir.size(), kOfficialDir) == 0 &&
        path.find('\\', kOfficialDir.size()) == std::string::npos) {
        e.map = path.substr(kOfficialDir.size());
        if (e.map.size() > 4 && e.map.compare(e.map.size() - 4, 4, ".TED") == 0) e.map.resize(e.map.size() - 4);
    }
    std::vector<uint8_t> bytes;
    if (!files.read(path, bytes)) return e;
    terrain::ParseOptions opt;
    opt.environments_only = true;  // chunk 0 only (plus the cheap environment set); later chunks are never trusted here
    terrain::MapData md;
    std::string err;
    terrain::parse_ted(bytes, md, &err, opt);  // a malformed later chunk leaves the already decoded header usable
    if (!md.info.present) return e;
    e.readable = true;
    e.kind = md.info.map_type;
    e.players = terrain::find_mini(md.info.raw, 0x02) ? md.info.counts[0] : 1u;
    e.custom = md.info.flag_0b == 1;
    e.name_key = md.info.name_key;
    return e;
}
}  // namespace

SkirmishMapPreview read_skirmish_map_preview(const FileProvider& files, std::string_view map, bool space) {
    SkirmishMapPreview pv;
    std::vector<uint8_t> bytes;
    if (!files.read(terrain::map_path(map), bytes)) return pv;
    pv.found = true;
    terrain::MapData md;
    terrain::ParseOptions opt;
    opt.environments_only = true;  // the header only; chunks 3 and 0x13 are taken from the top-level sequence below
    terrain::parse_ted(bytes, md, nullptr, opt);
    std::vector<terrain::ChunkView> top;
    if (!terrain::read_chunks(bytes, top)) return pv;
    for (const terrain::ChunkView& k : top) {
        if (k.id == 0x13 && !k.has_children) pv.embedded_image.assign(k.payload.begin(), k.payload.end());
        if (k.id != 3 || k.has_children || k.payload.size() < 4) continue;
        const float w = md.info.world_width, h = md.info.world_height;
        if (!md.info.present || md.info.flag_12 == 0 || !(w > 0.0f) || !(h > 0.0f)) continue;
        uint32_t n = 0;
        std::memcpy(&n, k.payload.data(), 4);
        if (n > (k.payload.size() - 4) / 8) continue;
        for (uint32_t i = 0; i < n; ++i) {
            float xy[2];
            std::memcpy(xy, k.payload.data() + 4 + 8 * static_cast<size_t>(i), 8);
            const float u = xy[0] / w + (space ? 0.5f : 0.0f), v = xy[1] / h + (space ? 0.5f : 0.0f);
            pv.starts.emplace_back(std::clamp(u, 0.0f, 1.0f), 1.0f - std::clamp(v, 0.0f, 1.0f));
        }
    }
    return pv;
}

SkirmishMapCatalog build_skirmish_map_catalog(const vfs::Vfs& vfs) {
    SkirmishMapCatalog c;
    constexpr uint32_t kLoose = vfs::Vfs::kLayerBase | vfs::Vfs::kLayerMeg;
    for (const std::string& p : vfs.list("Data\\Art\\Maps\\*.TED", kLoose)) c.official.push_back(read_map_entry(vfs, p));
    std::vector<std::string> custom = vfs.list("Data\\CustomMaps\\*.TED", kLoose);
    for (std::string& p : vfs.list("Data\\Art\\Maps\\*.TED", vfs::Vfs::kLayerMod)) custom.push_back(std::move(p));
    std::sort(custom.begin(), custom.end());
    custom.erase(std::unique(custom.begin(), custom.end()), custom.end());
    for (const std::string& p : custom) {
        SkirmishMapEntry e = read_map_entry(vfs, p);
        if (e.readable && e.custom) c.custom_scan.push_back(std::move(e));
    }
    return c;
}

SkirmishMapCatalog build_skirmish_map_catalog(const GameData& data) {
    return data.vfs ? build_skirmish_map_catalog(*data.vfs) : SkirmishMapCatalog{};
}

std::vector<SkirmishMapEntry> skirmish_map_rows(const SkirmishMapCatalog& catalog, bool custom_tab) {
    std::vector<SkirmishMapEntry> out;
    const auto add = [&](const std::vector<SkirmishMapEntry>& list) {
        for (const SkirmishMapEntry& e : list) {
            if (!e.readable || (e.kind != 1 && e.kind != 2) || e.players < 2 || e.custom != custom_tab) continue;
            if (std::any_of(out.begin(), out.end(), [&](const SkirmishMapEntry& o) { return o.path == e.path; })) continue;
            out.push_back(e);
        }
    };
    add(catalog.official);
    add(catalog.custom_scan);
    return out;
}

const SkirmishMapEntry* find_skirmish_map(const SkirmishMapCatalog& catalog, std::string_view map) {
    if (map.empty()) return nullptr;
    const std::string path = terrain::map_path(map);
    for (const auto* list : {&catalog.official, &catalog.custom_scan})
        for (const SkirmishMapEntry& e : *list)
            if (e.path == path) return &e;
    return nullptr;
}

}  // namespace alamo::game

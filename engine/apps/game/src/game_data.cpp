#include "alamo/game/game_data.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <map>
#include <optional>

#include "alamo/core/ascii.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/data/xml.hpp"
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

void load_faction_skirmish_data(GameData& out);

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
    load_faction_skirmish_data(out);  // q6 t08: no stage of its own (the Q5 stage list stays as it was)
    return true;
}

void load_faction_skirmish_data(GameData& out) {
    out.skirmish_forces = load_skirmish_forces(out.rules.factions);  // cu5 t01: the galactic rules' faction parse
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    out.basic_ai = ai::load_faction_basic_ai(out.rules.factions);
#endif
}

uint32_t neutral_faction(const galactic::GalacticRules& rules) {
    for (uint32_t f = 0; f < rules.factions.size(); ++f)
        if (rules.factions[f].is_neutral) return f;
    return galactic::kNoIndex;
}

}  // namespace

bool load_game_data(const std::string& root, const std::vector<std::string>& args, GameData& out, std::string* err) {
    std::unique_ptr<vfs::Vfs> v = vfs::mount_game(root, args, err);  // mounted exactly as alamo_headless does
    if (!v) return false;
    out.root = root;
    out.args = args;
    out.mod_layer = !v->mod_dirs().empty();
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
            sim::load_tactical_ai_data(*ts, *out.files, &out.db.constants(), lister);  // a2h t05: plans, difficulties, GameConstants
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

bool load_game_data_dir(const std::string& dir, GameData& out, std::string* err) {
    out.root = dir;
    out.vfs = nullptr;
    out.files = std::make_unique<DirectoryProvider>(dir);
    return load_common(out, err);
}

std::vector<std::string> data_fallback_forces(const GameData& data, const std::string& faction, bool land) {
    return game::data_fallback_forces(data.rules, data.types, faction, land);
}

namespace {
std::vector<std::string> game_default_forces(const GameData& data, const std::string& faction, bool land) {
    return skirmish_default_forces({data.rules, data.types, data.skirmish_forces}, faction, land);
}
}  // namespace

std::vector<std::string> default_skirmish_fleet(const GameData& data, const std::string& faction) {
    return game_default_forces(data, faction, false);
}

std::vector<std::string> default_skirmish_ground(const GameData& data, const std::string& faction) {
    return game_default_forces(data, faction, true);
}

std::string default_skirmish_faction(const galactic::GalacticRules& rules, bool ai_slot, std::string_view human) {
    for (const galactic::FactionRules& f : rules.factions) {
        if (!f.is_playable || f.is_neutral) continue;
        if (ai_slot && !human.empty() && rules.find_faction(human) == rules.find_faction(f.name)) continue;
        return f.name;
    }
    return {};
}

SkirmishSetup resolve_skirmish_factions(const galactic::GalacticRules& rules, SkirmishSetup s) {
    if (s.human_faction.empty()) s.human_faction = default_skirmish_faction(rules, false);
    if (s.ai_faction.empty()) s.ai_faction = default_skirmish_faction(rules, true, s.human_faction);
    return s;
}

bool make_skirmish_galaxy(const GameData& data, const SkirmishSetup& setup, galactic::GalacticSetup& g, std::string* err) {
    const galactic::GalacticRules& rules = data.rules;
    const SkirmishSetup s = resolve_skirmish_factions(rules, setup);
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
    if (planet == galactic::kNoIndex) planet = skirmish_galaxy_planet(rules, {}, s.land);  // modc t19: from the data
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
    hostile.faction = rules.native_owner_faction();  // modc t03: by data (FoC: Hostile)
    if (hostile.faction != galactic::kNoIndex && hostile.faction != human.faction && hostile.faction != ai.faction) g.players.push_back(hostile);
    auto add = [&](int32_t player, PlanetIndex at, const std::vector<std::string>& names, const std::string& faction, bool ground = false) {
        const std::vector<std::string> list =
            !names.empty() ? names : ground ? default_skirmish_ground(data, faction) : default_skirmish_fleet(data, faction);
        uint32_t n = 0;
        for (const std::string& name : list)
            if (const uint32_t t = rules.find_type(name); t != galactic::kNoIndex) {
                g.forces.push_back({player, at, t});
                ++n;
            }
        return n;
    };
    // Defender (AI) holds the battle planet first, then the human's forces at the far planet.
    if (s.no_forces) return true;
    uint32_t na = 0, nh = 0;
    if (s.land) {
        na = add(1, kSkirmishBattlePlanet, s.ground_ai, s.ai_faction, true);
        const std::vector<std::string> fleet = s.units_human.empty() ? default_skirmish_fleet(data, s.human_faction) : s.units_human;
        if (!fleet.empty() && add(0, kSkirmishHumanPlanet, {fleet.front()}, s.human_faction) > 0)  // the carrier fleet
            nh = add(0, kSkirmishHumanPlanet, s.ground_human, s.human_faction, true);
    } else {
        na = add(1, kSkirmishBattlePlanet, s.units_ai, s.ai_faction);
        // q6 [port]: the human fleet hops to the battle (the skirmish is hosted as a galactic hop), so a bought fleet
        // without a hyperspace member (Thrawn's Revenge sells Nantex fighters) is led by the hyperspace ship the default
        // forces would get (skirmish_hyperspace_leader); otherwise its opening hop is refused and the battle never starts.
        // Retail places the bought units at the map's spawn markers (SKIRMISH_SETUP 2.2). Default fleets carry their own.
        if (!s.units_human.empty()) {
            std::vector<uint32_t> bought;
            for (const std::string& name : s.units_human)
                if (const uint32_t t = rules.find_type(name); t != galactic::kNoIndex) bought.push_back(t);
            if (const uint32_t lead = skirmish_hyperspace_leader(rules, data.types, human.faction, bought); lead != galactic::kNoIndex)
                g.forces.push_back({0, kSkirmishHumanPlanet, lead});
        }
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
// q6 t02 [port]: a listed map whose loose file (through the normal lookup, mod first) is empty or cannot be sized is left
// out - Thrawn's Revenge ships 0-byte _MP_SPACE_KUAT.ted / _MP_LAND_*.ted files over the FoC maps. A file stat, not a read
// (mod maps run to 20 MB each). MEG entries are kept as indexed. No shipped FoC / EaW map is empty, so vanilla lists are
// unchanged.
bool map_file_usable(const vfs::Vfs& v, const std::string& path) {
    const std::optional<vfs::Vfs::Resolution> r = v.resolve(path);
    if (!r) return false;
    if (r->layer == vfs::Vfs::Resolution::Meg) return true;
    std::error_code ec;
    const std::uintmax_t n = std::filesystem::file_size(path_from_utf8(r->location), ec);
    return !ec && n > 0;
}

std::vector<std::string> list_maps(const GameData& data, const char* glob) {
    std::vector<std::string> out;
    if (!data.vfs) return out;
    for (const std::string& p : data.vfs->list(glob)) {
        std::string stem = p;
        if (const size_t s = stem.find_last_of("\\/"); s != std::string::npos) stem = stem.substr(s + 1);
        if (const size_t d = stem.find_last_of('.'); d != std::string::npos) stem = stem.substr(0, d);
        ascii_upper_in_place(stem);
        if (std::find(out.begin(), out.end(), stem) != out.end()) continue;
        if (!map_file_usable(*data.vfs, p)) continue;
        out.push_back(stem);
    }
    std::sort(out.begin(), out.end());
    return out;
}
}  // namespace

std::vector<std::string> skirmish_land_maps(const GameData& data) { return list_maps(data, "Data\\Art\\Maps\\_MP_LAND_*.ted"); }

std::vector<std::string> skirmish_space_maps(const GameData& data) {
    return list_maps(data, "Data\\Art\\Maps\\_MP_SPACE_*.ted");
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
            // UI.md 9.6.1: space maps are first shifted by half the world size (centred on the origin), land maps not
            // (origin at the lower-left corner); fx = x / width, fy = y / height, each clamped to 0..1; image y = 1 - fy.
            const float fx = space ? (xy[0] + w * 0.5f) / w : xy[0] / w, fy = space ? (xy[1] + h * 0.5f) / h : xy[1] / h;
            pv.starts.emplace_back(std::clamp(fx, 0.0f, 1.0f), 1.0f - std::clamp(fy, 0.0f, 1.0f));
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

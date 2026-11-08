// BM3: game-made benchmark saves and the map listing (benchmark_save.hpp).
#include "alamo/game/benchmark_save.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <map>

#include "alamo/game/game_session.hpp"
#include "alamo/game/save_games.hpp"
#include "alamo/sim/bench_scenario.hpp"
#include "alamo/sim/map_spawn.hpp"
#include "alamo/terrain/terrain.hpp"

namespace alamo::game {

namespace {

constexpr uint32_t kMaxScale = 16;
constexpr uint64_t kBattleWaitTicks = 6000;  // the battle must be on the mode stack within 200 s of game time

void fail(std::string* err, std::string msg) {
    if (err) *err = std::move(msg);
}

bool parse_u64(const std::string& v, uint64_t& out) {
    const auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
    return ec == std::errc() && p == v.data() + v.size() && !v.empty();
}

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> repeated(const std::vector<std::string>& names, uint32_t times) {
    std::vector<std::string> out;
    for (uint32_t i = 0; i < times; ++i) out.insert(out.end(), names.begin(), names.end());
    return out;
}

}  // namespace

int parse_bench_new_arg(std::span<const std::string> args, size_t& i, BenchNewSave& o, std::string* err) {
    const std::string& a = args[i];
    auto value = [&](std::string& out) -> bool {
        if (i + 1 >= args.size()) {
            fail(err, a + " needs a value");
            return false;
        }
        out = args[++i];
        return true;
    };
    auto number = [&](uint64_t lo, uint64_t hi, uint64_t& out) -> bool {
        std::string v;
        if (!value(v)) return false;
        uint64_t n = 0;
        if (!parse_u64(v, n) || n < lo || n > hi) {
            fail(err, a + ": '" + v + "' is not a number from " + std::to_string(lo) + " to " + std::to_string(hi));
            return false;
        }
        out = n;
        return true;
    };
    uint64_t n = 0;
    if (a == "--bench-new-save") return value(o.out) ? 1 : -1;
    if (a == "--bench-map") return value(o.map) ? 1 : -1;
    if (a == "--bench-human") return value(o.human) ? 1 : -1;
    if (a == "--bench-ai") return value(o.ai) ? 1 : -1;
    if (a == "--bench-scale") return number(1, kMaxScale, n) ? (o.scale = static_cast<uint32_t>(n), 1) : -1;
    if (a == "--bench-save-after") return number(0, 100000, n) ? (o.save_after = n, 1) : -1;
    if (a == "--bench-seed") return number(0, 0xFFFFFFFFull, n) ? (o.seed = static_cast<uint32_t>(n), 1) : -1;
    if (a == "--bench-maps") {
        o.maps = true;
        return 1;
    }
    return 0;
}

std::string check_bench_new_save(const BenchNewSave& o) {
    if (!o.out.empty() && o.map.empty()) return "--bench-new-save needs --bench-map MAP (a skirmish map; --bench-maps lists them)";
    if (!o.out.empty() && o.maps) return "--bench-new-save and --bench-maps do not mix";
    if (o.human.empty() || o.ai.empty()) return "--bench-human / --bench-ai need a faction name";
    if (upper(o.human) == upper(o.ai)) return "--bench-human and --bench-ai must be different factions";
    return {};
}

bool make_bench_save(const GameData& data, jobs::JobSystem& js, const BenchNewSave& o, std::string* summary, std::string* err) {
    if (const std::string why = check_bench_new_save(o); !why.empty()) {
        fail(err, why);
        return false;
    }
    terrain::MapData probe;
    std::string merr;
    if (!data.files || !terrain::load_map(*data.files, o.map, probe, &merr)) {
        fail(err, "map " + o.map + " did not load" + (merr.empty() ? std::string() : ": " + merr));
        return false;
    }
    const bool land = !probe.is_space();
    SessionSpec spec;
    spec.kind = SessionKind::Skirmish;
    spec.skirmish_map = o.map;
    spec.skirmish.human_faction = o.human;
    spec.skirmish.ai_faction = o.ai;
    if (land) {
        spec.skirmish.ground_human = repeated(default_skirmish_ground(data, o.human), o.scale);
        spec.skirmish.ground_ai = repeated(default_skirmish_ground(data, o.ai), o.scale);
    } else {
        spec.skirmish.units_human = repeated(default_skirmish_fleet(data, o.human), o.scale);
        spec.skirmish.units_ai = repeated(default_skirmish_fleet(data, o.ai), o.scale);
    }
    spec.seed = o.seed;
    spec.battle_max_frames = 27000;      // the game's own limit (15 minutes)
    spec.battle_end_hold_frames = 150;   // as alamo_game (kBattleEndHoldFrames)
    GameSession session(data, js, spec);
    std::vector<std::string> diag;
    if (!session.start(&diag)) {
        std::string why = "the skirmish did not start";
        for (const std::string& d : diag) why += "; " + d;
        fail(err, why);
        return false;
    }
    uint64_t battle_tick = 0;
    for (uint64_t t = 0; t < kBattleWaitTicks && battle_tick == 0; ++t) {
        if (!session.tick()) {
            fail(err, "the session stopped at tick " + std::to_string(session.host().frame()) + " before the battle started");
            return false;
        }
        if (session.in_battle()) battle_tick = session.host().frame();
    }
    if (battle_tick == 0) {
        fail(err, "no battle within " + std::to_string(kBattleWaitTicks) + " ticks");
        return false;
    }
    for (uint64_t t = 0; t < o.save_after; ++t) {
        if (!session.tick() || !session.in_battle()) {
            fail(err, "the battle ended " + std::to_string(t) + " ticks after it started (before --bench-save-after " + std::to_string(o.save_after) +
                          "): use a smaller value or a longer-lived matchup");
            return false;
        }
    }
    // The game-data folder is informational (never compared on load) but would make the bytes depend on where this machine
    // installed the game; the file carries no wall-clock time either (saved_at 0).
    sim::SessionDescriptor d = session.host().session();
    d.data_root.clear();
    session.host().set_session(std::move(d));
    SaveFile file;
    const SaveMeta meta = session.save_meta(sanitize_save_name(std::filesystem::path(o.out).stem().string()), 0);
    std::string e;
    if (!session.save(file, meta, &e) || !write_save_file(file, o.out, &e)) {
        fail(err, "cannot save: " + e);
        return false;
    }
    if (summary)
        *summary = "saved tick " + std::to_string(session.host().frame()) + " (battle started at tick " + std::to_string(battle_tick) + ") of " + o.map +
                   " (" + (land ? "land" : "space") + "), " + o.human + " (idle) vs " + o.ai + ", forces x" + std::to_string(o.scale) + " to " + o.out;
    return true;
}

bool bench_regen_save(const GameData& data, jobs::JobSystem& js, const std::string& manifest, std::string* tmp_save, std::string* summary,
                      std::string* err) {
    sim::bench::Scenario sc;
    std::string e;
    if (!sim::bench::Scenario::load_recipe(manifest, sc, &e)) {
        fail(err, manifest + ": " + e);
        return false;
    }
    if (sc.host != sim::bench::Host::Game) {
        fail(err, manifest + " is not a game scenario (alamo_headless --bench-regen remakes headless ones)");
        return false;
    }
    if (sc.make_args.empty()) {
        fail(err, manifest + " has no make line (the --bench-new-save arguments it was made with)");
        return false;
    }
    BenchNewSave o;
    for (size_t i = 0; i < sc.make_args.size(); ++i) {
        const int r = parse_bench_new_arg(sc.make_args, i, o, &e);
        if (r <= 0) {
            fail(err, manifest + ": make line: " + (e.empty() ? "unknown argument " + sc.make_args[i] : e));
            return false;
        }
    }
    if (!o.out.empty() || o.maps) {
        fail(err, manifest + ": the make line must not name --bench-new-save (the output is chosen by --bench-regen)");
        return false;
    }
    const std::filesystem::path dir = std::filesystem::path(manifest).parent_path();
    o.out = (dir / (sc.name + ".regen.alsv")).string();
    if (!make_bench_save(data, js, o, summary, err)) return false;
    if (tmp_save) *tmp_save = o.out;
    return true;
}

std::vector<std::string> bench_map_lines(const GameData& data) {
    std::vector<std::string> out;
    if (!data.files) return out;
    const sim::TypeCrcIndex index(data.types);
    std::vector<std::string> maps = skirmish_space_maps(data);
    for (const std::string& m : skirmish_land_maps(data)) maps.push_back(m);
    for (const std::string& name : maps) {
        terrain::MapData md;
        if (!terrain::load_map(*data.files, name, md)) {
            out.push_back(name + ": not loaded");
            continue;
        }
        std::string line = name + ": " + (md.is_space() ? "space" : "land");
        if (!md.is_space()) {
            const terrain::Terrain& t = md.terrain;
            const terrain::TerrainHeader& h = t.header;
            uint64_t under = 0;
            for (uint32_t y = 0; y < t.height; ++y)
                for (uint32_t x = 0; x < t.width; ++x)
                    if (t.height_at(x, y) < h.water_height) ++under;
            const double share = t.width && t.height ? 100.0 * static_cast<double>(under) / (static_cast<double>(t.width) * t.height) : 0.0;
            const char* wt = h.water_type == terrain::WaterType::Water ? "water" : h.water_type == terrain::WaterType::Lava   ? "lava"
                                                                         : h.water_type == terrain::WaterType::Ice            ? "ice"
                                                                                                                              : "none";
            char buf[160];
            std::snprintf(buf, sizeof buf, " grid %ux%u, %s at %.0f, %.1f%% of the vertices under it, reflection %.2f / %.2f, refraction %.2f", t.width,
                          t.height, wt, h.water_height, share, h.reflection_fraction, h.reflection_distortion, h.refraction_distortion);
            line += buf;
        }
        if (const terrain::Environment* env = md.environments.start()) {
            line += ", sky";
            for (const std::string& s : env->skydome)
                if (!s.empty()) line += " " + s;
            if (!env->weather_effect.empty()) line += ", weather " + env->weather_effect;
            if (env->fog_enabled) line += ", fog";
        }
        std::map<std::string, uint32_t> kinds;  // placed objects whose type name says nebula / cloud / field
        std::string nebulae;                    // the nebulae's positions (x y z), the way a camera path finds them
        for (const terrain::ObjectPlacement& pl : md.objects) {
            const objects::TypeIndex t = index.find(pl.type_crc);
            if (t == objects::kNone) continue;
            const std::string tn = upper(std::string(data.types.raw().types().name[t]));
            for (const char* key : {"NEBULA", "CLOUD", "FIELD", "ION_STORM", "STORM"})
                if (tn.find(key) != std::string::npos) {
                    ++kinds[tn];
                    if (tn.find("NEBULA") != std::string::npos) {
                        char at[96];
                        std::snprintf(at, sizeof at, " %s@%.0f,%.0f,%.0f", tn.c_str(), static_cast<double>(pl.position[0]), static_cast<double>(pl.position[1]),
                                      static_cast<double>(pl.position[2]));
                        nebulae += at;
                    }
                    break;
                }
        }
        if (!kinds.empty()) {
            line += ", props";
            for (const auto& [n, c] : kinds) line += " " + n + "x" + std::to_string(c);
            if (!nebulae.empty()) line += "; nebulae at" + nebulae;
        }
        out.push_back(std::move(line));
    }
    return out;
}

}  // namespace alamo::game

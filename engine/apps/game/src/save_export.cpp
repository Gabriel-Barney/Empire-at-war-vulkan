// Launcher support: a saved game's galactic state as JSON (see save_export.hpp).
#include "alamo/game/save_export.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "alamo/galactic/campaign.hpp"
#include "alamo/game/save_games.hpp"

namespace alamo::game {

namespace fs = std::filesystem;

namespace {

class Json {
public:
    std::string out;

    void raw(std::string_view s) { out.append(s); }
    static bool valid_utf8(std::string_view s) {
        for (size_t i = 0; i < s.size();) {
            const auto c = static_cast<unsigned char>(s[i]);
            const size_t n = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
            if (n == 0 || i + n > s.size()) return false;
            for (size_t k = 1; k < n; ++k)
                if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
            i += n;
        }
        return true;
    }
    void str(std::string_view s) {
        const bool utf8 = valid_utf8(s);
        out.push_back('"');
        for (const char c : s) {
            const auto u = static_cast<unsigned char>(c);
            switch (c) {
                case '"': out.append("\\\""); break;
                case '\\': out.append("\\\\"); break;
                case '\n': out.append("\\n"); break;
                case '\r': out.append("\\r"); break;
                case '\t': out.append("\\t"); break;
                default:
                    if (u < 0x20) {
                        char b[8];
                        std::snprintf(b, sizeof b, "\\u%04x", u);
                        out.append(b);
                    } else if (u >= 0x80 && !utf8) {
                        // XML names are read as bytes; text that is not UTF-8 is Latin-1 (older mods): encode as UTF-8.
                        out.push_back(static_cast<char>(0xC0 | (u >> 6)));
                        out.push_back(static_cast<char>(0x80 | (u & 0x3F)));
                    } else {
                        out.push_back(c);
                    }
            }
        }
        out.push_back('"');
    }
    void num(double v) {
        if (!std::isfinite(v)) return raw("0");
        char b[32];
        std::snprintf(b, sizeof b, "%.6g", v);
        out.append(b);
    }
    void num(int64_t v) { out.append(std::to_string(v)); }
    void num(uint64_t v) { out.append(std::to_string(v)); }
    void boolean(bool v) { out.append(v ? "true" : "false"); }
    // "key": (with the comma before it when needed)
    void key(std::string_view k, bool& first) {
        if (!first) out.push_back(',');
        first = false;
        str(k);
        out.push_back(':');
    }
};

std::string type_name(const GameData& data, objects::TypeIndex t) {
    const auto& names = data.types.raw().types().name;
    return t < names.size() ? std::string(names[t]) : std::string();
}

struct Group {
    objects::TypeIndex type;
    int32_t owner;
    uint32_t count;
};

// Live objects grouped by (type, owner), first-seen order.
template <class Handles>
std::vector<Group> group(const objects::ObjectStore& store, const Handles& handles) {
    std::vector<Group> out;
    for (const objects::ObjectHandle h : handles) {
        if (!store.alive(h)) continue;
        const objects::TypeIndex t = store.type(h);
        const int32_t o = store.owner(h);
        bool found = false;
        for (Group& g : out) {
            if (g.type == t && g.owner == o) {
                ++g.count;
                found = true;
                break;
            }
        }
        if (!found) out.push_back({t, o, 1});
    }
    return out;
}

void write_groups(Json& j, const GameData& data, const std::vector<Group>& groups) {
    j.raw("[");
    for (size_t i = 0; i < groups.size(); ++i) {
        if (i) j.raw(",");
        bool f = true;
        j.raw("{");
        j.key("type", f), j.str(type_name(data, groups[i].type));
        j.key("owner", f), j.num(static_cast<int64_t>(groups[i].owner));
        j.key("count", f), j.num(static_cast<uint64_t>(groups[i].count));
        j.raw("}");
    }
    j.raw("]");
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x - 'A' + 'a');
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y - 'A' + 'a');
        if (x != y) return false;
    }
    return true;
}

}  // namespace

bool export_session_json(const GameData& data, const GameSession& session, const SaveFile& file, std::span<const ExportColor> colors,
                         std::string& out, std::string* err) {
    const galactic::GalacticSim* g = session.galaxy();
    if (!g) {
        if (err) *err = "the save has no galactic map";
        return false;
    }
    const objects::ObjectStore& store = g->store();
    const GalaxyMap& map = g->map();
    const auto planets = g->planets();
    const auto fleets = g->fleets();
    const auto players = g->players();
    SaveMeta meta;
    decode_description(file.description, meta);

    Json j;
    bool top = true;
    j.raw("{");
    j.key("format", top), j.num(static_cast<int64_t>(kSaveExportFormat));
    j.key("save", top);
    {
        bool f = true;
        j.raw("{");
        j.key("name", f), j.str(meta.name);
        j.key("kind", f), j.str(meta.kind);
        j.key("campaign", f), j.str(meta.campaign);
        j.key("title", f), j.str(meta.title);
        j.key("faction", f), j.str(meta.faction);
        j.key("difficulty", f), j.num(static_cast<int64_t>(meta.difficulty));
        j.key("week", f), j.num(static_cast<uint64_t>(meta.week));
        j.key("gameSeconds", f), j.num(meta.game_seconds);
        j.key("savedAt", f), j.num(meta.saved_at);
        j.key("tick", f), j.num(file.tick);
        j.raw("}");
    }
    j.key("galacticDay", top), j.num(static_cast<int64_t>(g->galactic_day()));
    j.key("modeFrame", top), j.num(static_cast<int64_t>(g->frame()));
    j.key("localPlayer", top), j.num(static_cast<int64_t>(g->local_player()));

    j.key("players", top);
    j.raw("[");
    for (size_t i = 0; i < players.size(); ++i) {
        const galactic::PlayerState& p = players[i];
        const bool known = p.faction < data.rules.factions.size();
        const std::string faction = known ? data.rules.factions[p.faction].name : std::string();
        if (i) j.raw(",");
        bool f = true;
        j.raw("{");
        j.key("index", f), j.num(static_cast<int64_t>(i));
        j.key("faction", f), j.str(faction);
        j.key("color", f);
        const ExportColor* c = nullptr;
        for (const ExportColor& e : colors)
            if (iequal(e.faction, faction)) {
                c = &e;
                break;
            }
        if (c) {
            char b[8];
            std::snprintf(b, sizeof b, "#%02x%02x%02x", c->r, c->g, c->b);
            j.str(b);
        } else {
            j.raw("null");
        }
        j.key("human", f), j.boolean(p.human);
        j.key("ai", f), j.boolean(p.ai);
        j.key("neutral", f), j.boolean(known && data.rules.factions[p.faction].is_neutral);
        j.key("credits", f), j.num(static_cast<double>(p.account.credits));
        j.key("techLevel", f), j.num(static_cast<int64_t>(p.tech_level));
        j.key("maxTechLevel", f), j.num(static_cast<int64_t>(p.max_tech_level));
        j.key("planets", f), j.num(static_cast<int64_t>(g->planets_owned(static_cast<int32_t>(i))));
        j.raw("}");
    }
    j.raw("]");

    j.key("planets", top);
    j.raw("[");
    for (size_t i = 0; i < map.planets.size(); ++i) {
        const galactic::PlanetState* ps = i < planets.size() ? &planets[i] : nullptr;
        const bool is_planet = ps && ps->is_planet;
        if (i) j.raw(",");
        bool f = true;
        j.raw("{");
        j.key("index", f), j.num(static_cast<int64_t>(i));
        j.key("name", f), j.str(map.planets.name[i]);
        j.key("type", f), j.str(is_planet ? type_name(data, static_cast<objects::TypeIndex>(ps->type)) : map.planets.name[i]);
        j.key("isPlanet", f), j.boolean(is_planet);
        const Vec3 pos = map.planets.position[i];
        j.key("x", f), j.num(static_cast<double>(pos.x));
        j.key("y", f), j.num(static_cast<double>(pos.y));
        j.key("z", f), j.num(static_cast<double>(pos.z));
        j.key("owner", f), j.num(static_cast<int64_t>(is_planet ? ps->owner : galactic::kNoPlayer));
        j.key("control", f), j.num(static_cast<double>(is_planet ? ps->control : 0.0f));
        j.key("creditValue", f), j.num(static_cast<int64_t>(i < map.planets.credit_value.size() ? map.planets.credit_value[i] : 0));
        j.key("starbaseLevel", f), j.num(static_cast<int64_t>(is_planet ? ps->starbase_level : 0));
        j.key("starbaseOwner", f), j.num(static_cast<int64_t>(is_planet ? ps->starbase_owner : galactic::kNoPlayer));
        j.key("groundLevel", f), j.num(static_cast<int64_t>(is_planet ? g->ground_base_level(static_cast<PlanetIndex>(i)) : 0));
        j.key("revealed", f), j.boolean(is_planet && g->is_planet_revealed(static_cast<PlanetIndex>(i)));
        j.key("hidden", f), j.boolean(is_planet && ps->hidden);
        j.key("destroyed", f), j.boolean(is_planet && ps->destroyed);
        j.key("corrupted", f), j.boolean(is_planet && ps->corruption != galactic::kCorruptionNone);
        j.key("inBattle", f), j.boolean(is_planet && ps->in_battle);
        j.key("structures", f), write_groups(j, data, is_planet ? group(store, ps->structures) : std::vector<Group>{});
        j.key("surface", f), write_groups(j, data, is_planet ? group(store, ps->surface) : std::vector<Group>{});
        j.key("fleets", f);
        j.raw("[");
        bool first_fleet = true;
        if (is_planet)
            for (const uint32_t fi : ps->orbit) {
                if (fi >= fleets.size() || !fleets[fi].alive) continue;
                if (!first_fleet) j.raw(",");
                first_fleet = false;
                j.num(static_cast<uint64_t>(fi));
            }
        j.raw("]");
        j.raw("}");
    }
    j.raw("]");

    j.key("routes", top);
    j.raw("[");
    for (size_t i = 0; i < map.routes.size(); ++i) {
        if (i) j.raw(",");
        bool f = true;
        j.raw("{");
        j.key("a", f), j.num(static_cast<int64_t>(map.routes.point_a[i] == kInvalidPlanet ? -1 : static_cast<int64_t>(map.routes.point_a[i])));
        j.key("b", f), j.num(static_cast<int64_t>(map.routes.point_b[i] == kInvalidPlanet ? -1 : static_cast<int64_t>(map.routes.point_b[i])));
        j.raw("}");
    }
    j.raw("]");

    j.key("fleets", top);
    j.raw("[");
    bool first = true;
    const auto planet_or_none = [](PlanetIndex p) { return p == kInvalidPlanet ? int64_t{-1} : static_cast<int64_t>(p); };
    for (size_t i = 0; i < fleets.size(); ++i) {
        const galactic::FleetState& fl = fleets[i];
        if (!fl.alive) continue;
        if (!first) j.raw(",");
        first = false;
        bool f = true;
        j.raw("{");
        j.key("index", f), j.num(static_cast<int64_t>(i));
        j.key("owner", f), j.num(static_cast<int64_t>(fl.owner));
        j.key("planet", f), j.num(planet_or_none(fl.planet));
        j.key("from", f), j.num(planet_or_none(fl.from));
        j.key("to", f), j.num(planet_or_none(fl.to));
        j.key("moving", f), j.boolean(fl.moving);
        j.key("x", f), j.num(static_cast<double>(fl.position.x));
        j.key("y", f), j.num(static_cast<double>(fl.position.y));
        j.key("z", f), j.num(static_cast<double>(fl.position.z));
        j.key("units", f), write_groups(j, data, group(store, fl.contents));
        j.raw("}");
    }
    j.raw("]");
    j.raw("}\n");
    out = std::move(j.out);
    return true;
}

bool export_save(const GameData& data, jobs::JobSystem& js, const std::string& save_path, const std::string& out_path,
                 std::span<const ExportColor> colors, std::string* err) {
    SaveFile file;
    std::string e;
    std::unique_ptr<GameSession> session = load_game(data, js, save_path, &e, nullptr, nullptr, &file);
    if (!session) {
        if (err) *err = e;
        return false;
    }
    std::string json;
    if (!export_session_json(data, *session, file, colors, json, err)) return false;
    const fs::path out(out_path);
    std::error_code ec;
    if (out.has_parent_path()) fs::create_directories(out.parent_path(), ec);
    const fs::path tmp = fs::path(out_path + ".tmp");
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!f) {
            if (err) *err = "cannot write " + tmp.string();
            return false;
        }
    }
    fs::rename(tmp, out, ec);
    if (ec) {
        fs::remove(tmp, ec);
        if (err) *err = "cannot write " + out_path;
        return false;
    }
    return true;
}

}  // namespace alamo::game

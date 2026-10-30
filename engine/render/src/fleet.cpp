#include "alamo/render/fleet.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "alamo/assets/asset_cache.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/materials/effect_loader.hpp"
#include "alamo/render/pipelines.hpp"
#include "alamo/vfs/vfs.hpp"

namespace alamo::render {

namespace {

struct Rng {  // xorshift64*: deterministic, no std:: distribution (implementation-defined)
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint64_t next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return s * 0x2545F4914F6CDD1Dull;
    }
    float uniform() { return static_cast<float>(next() >> 40) / static_cast<float>(1u << 24); }  // [0,1)
};

template <class T>
void shuffle(std::vector<T>& v, Rng& r) {
    for (size_t i = v.size(); i > 1; --i) std::swap(v[i - 1], v[static_cast<size_t>(r.next() % i)]);
}

char upper_c(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }

std::string upper(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = upper_c(c);
    return r;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper_c(a[i]) != upper_c(b[i])) return false;
    return true;
}

bool iends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && iequals(s.substr(s.size() - suffix.size()), suffix);
}

// Upper-cased CategoryMask tokens of every <CategoryMask> child (repeated tags append).
std::vector<std::string> category_tokens(const data::XmlNode& element) {
    std::vector<std::string> out;
    for (data::XmlNode c : element.children("CategoryMask")) {
        const std::string_view t = c.text();
        size_t i = 0;
        while (i < t.size()) {
            while (i < t.size() && (t[i] == '|' || t[i] == ',' || t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
            const size_t b = i;
            while (i < t.size() && !(t[i] == '|' || t[i] == ',' || t[i] == ' ' || t[i] == '\t' || t[i] == '\r' || t[i] == '\n')) ++i;
            if (i > b) out.push_back(upper(t.substr(b, i - b)));
        }
    }
    return out;
}

// -1: excluded.
int classify(const std::vector<std::string>& tokens) {
    bool capital = false, corvette = false, fighter = false;
    for (const std::string& t : tokens) {
        for (const char* bad : {"STRUCTURE", "SUPER", "TRANSPORT", "UPGRADE", "WALL", "INFANTRY", "VEHICLE"})
            if (t == bad) return -1;
        capital = capital || t == "CAPITAL" || t == "FRIGATE";
        corvette = corvette || t == "CORVETTE";
        fighter = fighter || t == "FIGHTER" || t == "BOMBER";
    }
    if (capital) return static_cast<int>(FleetClass::Capital);
    if (corvette) return static_cast<int>(FleetClass::Corvette);
    if (fighter) return static_cast<int>(FleetClass::Fighter);
    return -1;
}

}  // namespace

const char* fleet_class_name(FleetClass c) {
    switch (c) {
        case FleetClass::Capital: return "capital";
        case FleetClass::Corvette: return "corvette";
        case FleetClass::Fighter: return "fighter";
    }
    return "?";
}

std::vector<FleetShipType> fleet_ship_types(const data::GameDatabase& db) {
    std::vector<FleetShipType> out;
    std::set<std::string> seen_models;  // only membership is used (no iteration)
    for (const data::GameObjectRecord& rec : db.game_objects()) {
        if (rec.duplicate || !rec.element) continue;
        if (!iequals(rec.type, "SpaceUnit") && !iequals(rec.type, "UniqueUnit")) continue;
        if (iends_with(rec.name, "_Death_Clone")) continue;
        const std::string_view model = rec.element.child_text("Space_Model_Name");
        if (model.empty()) continue;
        const int cls = classify(category_tokens(rec.element));
        if (cls < 0) continue;
        std::string path = "Data\\Art\\Models\\" + upper(model);
        if (!seen_models.insert(path).second) continue;
        out.push_back({std::string(rec.name), std::move(path), static_cast<FleetClass>(cls)});
    }
    return out;
}

FleetModels pick_fleet_models(const vfs::Vfs& files, assets::AssetCache& cache, jobs::JobSystem& js, uint32_t seed,
                              uint32_t capitals, uint32_t fighters, uint32_t corvettes) {
    data::GameDatabase db;
    db.load(files);  // errors are logged by the loader; whatever loaded is used
    EffectCache effects(&files);
    return pick_fleet_models(db, cache, js, seed, capitals, fighters, corvettes, &effects);
}

FleetModels pick_fleet_models(const data::GameDatabase& db, assets::AssetCache& cache, jobs::JobSystem& js, uint32_t seed,
                              uint32_t capitals, uint32_t fighters, uint32_t corvettes, EffectCache* effects) {
    const std::vector<FleetShipType> cand = fleet_ship_types(db);
    std::vector<uint8_t> ok(cand.size(), 0), translated(cand.size(), 0);  // per-index outputs
    js.parallel_for(static_cast<uint32_t>(cand.size()), 4, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            auto m = cache.model(cand[i].model);
            ok[i] = (m && m->bounds.valid() && !m->submeshes.empty()) ? 1 : 0;
            if (!ok[i] || !effects) continue;
            for (const assets::Submesh& sm : m->submeshes) {
                if (!sm.is_render_geometry() || (sm.flags & assets::kSubmeshHidden) || translated[i]) continue;
                const auto fx = effects->get_or_default(sm.material.effect);
                if (!fx) continue;
                for (const materials::ResolvedPass& p : fx->passes)
                    if (!p.restore_only && p.program_status == materials::ProgramStatus::Translated) translated[i] = 1;
            }
        }
    });
    FleetModels out;
    out.candidates = static_cast<uint32_t>(cand.size());
    std::vector<uint32_t> by_class[3];
    for (uint32_t i = 0; i < static_cast<uint32_t>(cand.size()); ++i)
        if (ok[i]) by_class[static_cast<int>(cand[i].cls)].push_back(i);
    Rng r(seed);
    const uint32_t want[3] = {capitals, corvettes, fighters};
    std::vector<std::string>* models[3] = {&out.capitals, &out.corvettes, &out.fighters};
    std::vector<std::string>* names[3] = {&out.capital_names, &out.corvette_names, &out.fighter_names};
    for (int c : {0, 2, 1}) {  // capital, fighter, corvette: the first two keep the pre-corvette RNG order
        std::vector<uint32_t>& v = by_class[c];
        out.available[c] = static_cast<uint32_t>(v.size());
        shuffle(v, r);
        if (v.size() > want[c]) v.resize(want[c]);
    }
    // Track-B coverage (see fleet.hpp): candidates in database order, classes with a quota.
    std::vector<uint32_t> tcand;
    bool covered = false;
    for (uint32_t i = 0; i < static_cast<uint32_t>(cand.size()); ++i) {
        if (!ok[i] || !translated[i]) continue;
        ++out.translated_candidates;
        if (want[static_cast<int>(cand[i].cls)] > 0) tcand.push_back(i);
    }
    for (const std::vector<uint32_t>& v : by_class)
        for (uint32_t i : v) covered = covered || translated[i] != 0;
    if (!covered && !tcand.empty()) {
        const uint32_t pick = tcand[static_cast<size_t>(r.next() % tcand.size())];
        std::vector<uint32_t>& v = by_class[static_cast<int>(cand[pick].cls)];
        if (v.size() >= want[static_cast<int>(cand[pick].cls)]) v.back() = pick;
        else v.push_back(pick);
        out.translated_forced = true;
    }
    for (int c = 0; c < 3; ++c)
        for (uint32_t i : by_class[c]) {
            models[c]->push_back(cand[i].model);
            names[c]->push_back(cand[i].type_name);
            out.translated_picked += translated[i] ? 1u : 0u;
        }
    return out;
}

float fleet_radius(uint32_t count) { return 150.0f * std::sqrt(static_cast<float>(std::max<uint32_t>(count, 1))) * 1.45f; }

std::vector<FleetInstance> make_fleet(uint32_t count, uint32_t seed, uint32_t capital_types, uint32_t fighter_types) {
    return make_fleet(count, seed, capital_types, 0, fighter_types);
}

std::vector<FleetInstance> make_fleet(uint32_t count, uint32_t seed, uint32_t capital_types, uint32_t corvette_types,
                                      uint32_t fighter_types) {
    static const materials::float4 kTeams[4] = {{0.20f, 0.40f, 1.00f, 1}, {1.00f, 0.25f, 0.15f, 1},
                                                {0.25f, 0.90f, 0.30f, 1}, {0.95f, 0.85f, 0.20f, 1}};
    std::vector<FleetInstance> v(count);
    Rng r(seed ^ 0xF1EE7ull);
    const float L = 150.0f * std::sqrt(static_cast<float>(std::max<uint32_t>(count, 1)));
    for (uint32_t i = 0; i < count; ++i) {
        FleetInstance& f = v[i];
        const uint32_t slot = i % 10;
        const bool capital = capital_types > 0 && (slot == 0 || (corvette_types == 0 && fighter_types == 0));
        const bool corvette = !capital && corvette_types > 0 && (slot == 1 || slot == 2 || fighter_types == 0);
        if (capital) f.model = static_cast<uint32_t>(r.next() % capital_types);
        else if (corvette) f.model = capital_types + static_cast<uint32_t>(r.next() % corvette_types);
        else f.model = capital_types + corvette_types + static_cast<uint32_t>(r.next() % std::max<uint32_t>(fighter_types, 1));
        const float x = (r.uniform() * 2.0f - 1.0f) * L, y = (r.uniform() * 2.0f - 1.0f) * L;
        const float z = (r.uniform() * 2.0f - 1.0f) * L * 0.125f;
        const float yaw = r.uniform() * 6.2831853f;
        const float c = std::cos(yaw), s = std::sin(yaw);
        f.transform.m[0][0] = c;
        f.transform.m[0][1] = -s;
        f.transform.m[1][0] = s;
        f.transform.m[1][1] = c;
        f.transform.m[0][3] = x;
        f.transform.m[1][3] = y;
        f.transform.m[2][3] = z;
        f.team = kTeams[(x >= 0 ? 1 : 0) + (y >= 0 ? 2 : 0)];
    }
    return v;
}

uint32_t ring_recording_threads(uint32_t requested, uint32_t job_threads) {
    return std::max<uint32_t>(std::max<uint32_t>(requested, job_threads), 1u);
}

}  // namespace alamo::render

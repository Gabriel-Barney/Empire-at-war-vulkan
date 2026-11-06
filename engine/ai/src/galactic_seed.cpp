// Interim GalacticSnapshot builders (see the header).
#include "alamo/ai/galactic_seed.hpp"

#include <utility>

#include "alamo/core/sim_rng.hpp"
#include "alamo/data/game_database.hpp"
#ifdef ALAMO_AI_HAVE_GALACTIC_SIM
#include "alamo/galactic/galactic_rules.hpp"  // q6: galactic_unit_kind (the galactic role rule)
#endif
#include "alamo/objects/game_object_types.hpp"

namespace alamo::ai {

namespace {

char upper(char c) { return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c; }
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}
std::string_view trim(std::string_view s) {
    auto sp = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!s.empty() && sp(s.front())) s.remove_prefix(1);
    while (!s.empty() && sp(s.back())) s.remove_suffix(1);
    return s;
}
std::vector<std::string_view> split_commas(std::string_view s) {
    std::vector<std::string_view> out;
    size_t start = 0;
    for (;;) {
        const size_t c = s.find(',', start);
        out.push_back(trim(s.substr(start, c == std::string_view::npos ? std::string_view::npos : c - start)));
        if (c == std::string_view::npos) break;
        start = c + 1;
    }
    return out;
}

bool space_category(std::span<const std::string_view> mask) {
    static constexpr std::string_view kSpace[] = {"Fighter", "Bomber", "Corvette", "Frigate", "Capital", "SpaceHero"};
    for (std::string_view m : mask)
        for (std::string_view k : kSpace)
            if (iequals(m, k)) return true;
    return false;
}

void build_csr(uint32_t n, const std::vector<std::vector<UnitPresence>>& per, GalacticSnapshot& s) {
    s.unit_begin.assign(static_cast<size_t>(n) + 1, 0);
    s.units.clear();
    for (uint32_t i = 0; i < n; ++i) {
        s.unit_begin[i] = static_cast<uint32_t>(s.units.size());
        s.units.insert(s.units.end(), per[i].begin(), per[i].end());
    }
    s.unit_begin[n] = static_cast<uint32_t>(s.units.size());
}

}  // namespace

namespace {

// The element tags the seed's own rule classifies (StarBase, structures, space units, ground units).
bool seed_tag(std::string_view el) {
    static constexpr std::string_view kTags[] = {"StarBase", "GroundStructure", "SpecialStructure", "SpaceUnit",    "Squadron",
                                                 "SpaceProp", "GroundCompany",  "GroundInfantry",   "GroundVehicle"};
    for (const std::string_view k : kTags)
        if (iequals(el, k)) return true;
    return false;
}

// The seed's own element-tag rule (S2, unchanged).
void legacy_force_type(const objects::GameObjectTypes& types, objects::TypeIndex t, ForceType& f) {
    const std::string_view el = types.raw().element_name(t);
    if (iequals(el, "StarBase")) {
        f.starbase = true;
        f.layer = UnitLayer::Space;
        f.base_level = types.get_int(t, "Base_Level");
    } else if (iequals(el, "GroundStructure") || iequals(el, "SpecialStructure")) {
        f.structure = true;
        f.layer = UnitLayer::Ground;
    } else if (iequals(el, "SpaceUnit") || iequals(el, "Squadron") || iequals(el, "SpaceProp")) {
        f.layer = UnitLayer::Space;
    } else if (iequals(el, "GroundCompany") || iequals(el, "GroundInfantry") || iequals(el, "GroundVehicle")) {
        f.layer = UnitLayer::Ground;
    } else {
        bool space = space_category(types.get_list(t, "CategoryMask"));
        if (!space && iequals(el, "HeroCompany")) {
            const auto members = types.get_list(t, "Company_Units");
            if (!members.empty()) {
                const objects::TypeIndex m = types.resolve_name(members.front());
                if (m != objects::kNone && m < types.size()) space = space_category(types.get_list(m, "CategoryMask"));
            }
        }
        f.layer = space ? UnitLayer::Space : UnitLayer::Ground;
    }
}

}  // namespace

// q6: the galactic role rule (galactic::galactic_unit_kind: a DUMMY_* behaviour's role first, then the tag table, a vanilla
// unclassified tag stays unclassified). Roles: StarBase -> starbase (space, Base_Level), GroundStructure -> structure
// (ground), SpaceUnit / Squadron -> space, GroundUnit -> ground; no role (Other, Planet) -> the seed's own rule.
// [port] The tags where the seed's own table and the galactic tag table disagree keep the seed's rule, so base-game seeds
// do not change (FoC 85 / EaW 44 types): GroundBase (galactic: a structure), HeroCompany / HeroUnit / UniqueUnit
// (galactic: ground, or Hyperspace -> space; the seed: CategoryMask / the first company member), SpaceProp (galactic: no
// role). Mod tags outside both tables (Thrawn's Revenge) take their DUMMY_* behaviour's role.
ForceType force_type_of(const objects::GameObjectTypes& types, objects::TypeIndex t, bool legacy) {
    ForceType f;
    if (t == objects::kNone || t >= types.size()) return f;
    f.valid = true;
    f.combat_power = types.columns().ai_combat_power[t];
    const std::string_view el = types.raw().element_name(t);
#ifdef ALAMO_AI_HAVE_GALACTIC_SIM
    const bool galactic_tag = galactic::unit_kind_from_element(el, false) != galactic::UnitKind::Other;
#else
    legacy = true;  // no galactic rules library in this module list
    const bool galactic_tag = false;
#endif
    if (legacy || galactic_tag != seed_tag(el)) {
        legacy_force_type(types, t, f);
        return f;
    }
#ifdef ALAMO_AI_HAVE_GALACTIC_SIM
    uint8_t dummy = 0;
    for (uint8_t id = galactic::kDummyGroundBase; id <= galactic::kDummyStarship; ++id)
        if (types.has_behavior(t, objects::BehaviorSet::Union, id)) {
            dummy = id;
            break;
        }
    const galactic::UnitKind k = galactic::galactic_unit_kind(el, types.get_bool(t, "Hyperspace"), dummy);
    switch (k) {
        case galactic::UnitKind::StarBase:
            f.starbase = true;
            f.layer = UnitLayer::Space;
            f.base_level = types.get_int(t, "Base_Level");
            break;
        case galactic::UnitKind::GroundStructure:
            f.structure = true;
            f.layer = UnitLayer::Ground;
            break;
        case galactic::UnitKind::SpaceUnit: case galactic::UnitKind::Squadron: f.layer = UnitLayer::Space; break;
        case galactic::UnitKind::GroundUnit: f.layer = UnitLayer::Ground; break;
        default: legacy_force_type(types, t, f); break;
    }
#endif
    return f;
}

ForceTypeLookup make_force_type_lookup(const objects::GameObjectTypes& types) {
    return [&types](std::string_view name) { return force_type_of(types, types.resolve_name(name)); };
}

CampaignSeed seed_campaign_snapshot(const data::CampaignDef& campaign, const GalaxyMap& map,
                                    const ForceTypeLookup& lookup, const CampaignSeedOptions& options) {
    CampaignSeed out;
    GalacticSnapshot& s = out.snapshot;
    const auto n = static_cast<uint32_t>(map.planets.size());
    s.planet_count = n;
    s.fog_disabled = !options.fog;
    // [port] the synthetic seed couples the two: no fog also means auto-reveal (keeps the headless goldens).
    s.planet_auto_reveal = !options.fog;

    auto is_neutral = [&](std::string_view f) { return options.is_neutral ? options.is_neutral(f) : iequals(f, "Neutral"); };
    auto corrupt = [&](std::string_view f) {
        return options.benefits_from_corruption ? options.benefits_from_corruption(f) : iequals(f, "Underworld");
    };
    auto player_of = [&](std::string_view faction) -> PlayerId {
        for (size_t i = 0; i < out.player_names.size(); ++i)
            if (iequals(out.player_names[i], faction)) return static_cast<PlayerId>(i);
        out.player_names.emplace_back(faction);
        return static_cast<PlayerId>(out.player_names.size() - 1);
    };
    for (const std::string& e : campaign.ai_player_control) {
        const auto f = split_commas(e);
        if (!f.empty() && !f[0].empty()) player_of(f[0]);
    }
    for (const auto& [faction, planet] : campaign.home_locations) player_of(trim(faction));
    struct Force {
        PlayerId player;
        uint32_t planet;
        ForceType type;
    };
    std::vector<Force> forces;
    for (const std::string& e : campaign.starting_forces) {
        const auto f = split_commas(e);
        if (f.size() != 3 || f[0].empty() || f[1].empty() || f[2].empty()) {
            out.diagnostics.push_back("malformed Starting_Forces: " + e);
            continue;
        }
        const PlayerId p = player_of(f[0]);
        const PlanetIndex planet = map.planets.find(f[1]);
        if (planet == kInvalidPlanet) {
            out.diagnostics.push_back("Starting_Forces planet not in the campaign: " + e);
            continue;
        }
        const ForceType t = lookup ? lookup(f[2]) : ForceType{};
        if (!t.valid) {
            out.diagnostics.push_back("Starting_Forces unknown type: " + e);
            continue;
        }
        forces.push_back(Force{p, planet, t});
    }
    PlayerId neutral = kNoPlayer;
    for (size_t i = 0; i < out.player_names.size(); ++i)
        if (is_neutral(out.player_names[i])) {
            neutral = static_cast<PlayerId>(i);
            break;
        }
    if (neutral == kNoPlayer) neutral = player_of("Neutral");

    const size_t P = out.player_names.size();
    s.players.resize(P);
    for (size_t i = 0; i < P; ++i) {
        PerceptionPlayer& pl = s.players[i];
        pl.neutral = is_neutral(out.player_names[i]) || static_cast<PlayerId>(i) == neutral;
        pl.has_ai_system = !pl.neutral;
        pl.is_human = false;
        pl.benefits_from_corruption = corrupt(out.player_names[i]);
    }
    s.relation.assign(P * P, 1);
    for (size_t i = 0; i < P; ++i) s.relation[i * P + i] = 0;

    s.owner.assign(n, kNoPlayer);
    s.political_control.assign(n, 1.0f);
    s.starbase_level.assign(n, 0);
    s.starbase_power.assign(n, 0.0f);
    s.corruption.assign(n, -1);
    std::vector<PlayerId> ground_owner(n, kNoPlayer), base_owner(n, kNoPlayer), space_owner(n, kNoPlayer);
    std::vector<std::vector<UnitPresence>> units(n);
    std::vector<std::vector<float>> structures(n);
    s.sees_orbit.assign(P, galactic::Bitset(n));
    for (const Force& f : forces) {
        const uint32_t i = f.planet;
        s.sees_orbit[static_cast<size_t>(f.player)].set(i);
        if (f.type.starbase) {
            if (base_owner[i] == kNoPlayer) base_owner[i] = f.player;
            if (f.type.base_level > s.starbase_level[i]) {
                s.starbase_level[i] = f.type.base_level;
                s.starbase_power[i] = f.type.combat_power;
            }
            continue;
        }
        if (f.type.structure) {
            if (ground_owner[i] == kNoPlayer) ground_owner[i] = f.player;
            structures[i].push_back(f.type.combat_power);
            continue;
        }
        if (f.type.layer == UnitLayer::Ground) {
            if (ground_owner[i] == kNoPlayer) ground_owner[i] = f.player;
        } else if (space_owner[i] == kNoPlayer) {
            space_owner[i] = f.player;
        }
        units[i].push_back(UnitPresence{f.player, f.type.combat_power, f.type.layer});
    }
    for (uint32_t i = 0; i < n; ++i) {
        PlayerId o = ground_owner[i];
        if (o == kNoPlayer) o = base_owner[i];
        if (o == kNoPlayer) o = space_owner[i];
        if (o == kNoPlayer) o = neutral;
        s.owner[i] = o;
        s.sees_orbit[static_cast<size_t>(o)].set(i);
    }
    build_csr(n, units, s);
    s.structure_begin.assign(static_cast<size_t>(n) + 1, 0);
    for (uint32_t i = 0; i < n; ++i) {
        s.structure_begin[i] = static_cast<uint32_t>(s.structure_power.size());
        s.structure_power.insert(s.structure_power.end(), structures[i].begin(), structures[i].end());
    }
    s.structure_begin[n] = static_cast<uint32_t>(s.structure_power.size());
    return out;
}

GalacticSnapshot make_synthetic_snapshot(uint32_t planets, uint32_t ai_players, uint32_t seed) {
    GalacticSnapshot s;
    SimRng r(seed ^ 0x5EEDA1u);
    const uint32_t P = ai_players + 1;  // last = neutral
    s.planet_count = planets;
    s.players.resize(P);
    for (uint32_t p = 0; p < P; ++p) {
        s.players[p].has_ai_system = p < ai_players;
        s.players[p].neutral = p == ai_players;
        s.players[p].benefits_from_corruption = (p % 3) == 2;
    }
    s.relation.assign(static_cast<size_t>(P) * P, 1);
    for (uint32_t a = 0; a < P; ++a) s.relation[static_cast<size_t>(a) * P + a] = 0;
    // Players 0 and 2 (when present) are allies.
    if (ai_players > 2) {
        s.relation[0 * P + 2] = 0;
        s.relation[2 * static_cast<size_t>(P) + 0] = 0;
    }
    s.owner.resize(planets);
    s.political_control.resize(planets);
    s.starbase_level.resize(planets);
    s.starbase_power.resize(planets);
    s.corruption.resize(planets);
    std::vector<std::vector<UnitPresence>> units(planets);
    s.structure_begin.assign(static_cast<size_t>(planets) + 1, 0);
    for (uint32_t i = 0; i < planets; ++i) {
        s.owner[i] = static_cast<PlayerId>(r.range_int(0, static_cast<int32_t>(P) - 1));
        s.political_control[i] = r.range_int(0, 9) == 0 ? 0.0f : 1.0f;
        const int32_t level = r.range_int(0, 6) < 4 ? 0 : r.range_int(1, 5);
        s.starbase_level[i] = level;
        s.starbase_power[i] = level ? static_cast<float>(level * 2500 + r.range_int(0, 99)) : 0.0f;
        s.corruption[i] = r.range_int(0, 7) == 0 ? r.range_int(0, 3) : -1;
        const int32_t count = r.range_int(0, 4);
        for (int32_t u = 0; u < count; ++u) {
            UnitPresence up;
            up.owner = static_cast<PlayerId>(r.range_int(0, static_cast<int32_t>(P) - 1));
            up.layer = r.range_int(0, 2) == 0 ? UnitLayer::Ground : UnitLayer::Space;
            up.combat_power = static_cast<float>(r.range_int(50, 4000)) * 0.75f;
            units[i].push_back(up);
        }
        s.structure_begin[i] = static_cast<uint32_t>(s.structure_power.size());
        const int32_t st = r.range_int(0, 2);
        for (int32_t k = 0; k < st; ++k) s.structure_power.push_back(static_cast<float>(r.range_int(100, 900)));
    }
    s.structure_begin[planets] = static_cast<uint32_t>(s.structure_power.size());
    build_csr(planets, units, s);
    s.sees_orbit.assign(P, galactic::Bitset(planets));
    for (uint32_t p = 0; p < P; ++p)
        for (uint32_t i = 0; i < planets; ++i)
            if (s.owner[i] == static_cast<PlayerId>(p) || r.range_int(0, 2) == 0) s.sees_orbit[p].set(i);
    s.revealed = galactic::Bitset(planets, true);
    for (uint32_t i = 0; i < planets; ++i)
        if (r.range_int(0, 11) == 0) s.revealed.set(i, false);
    return s;
}

void perturb_snapshot(GalacticSnapshot& s, uint32_t seed, uint32_t frame) {
    const uint32_t n = s.planet_count;
    if (n == 0 || s.players.empty()) return;
    SimRng r(seed * 2654435761u + frame * 40503u + 1u);
    const auto P = static_cast<int32_t>(s.players.size());
    std::vector<std::vector<UnitPresence>> units(n);
    for (uint32_t i = 0; i < n; ++i) {
        const auto u = s.units_at(i);
        units[i].assign(u.begin(), u.end());
    }
    const uint32_t changes = n / 16 + 1;
    for (uint32_t c = 0; c < changes; ++c) {
        const auto i = static_cast<uint32_t>(r.range_int(0, static_cast<int32_t>(n) - 1));
        switch (r.range_int(0, 3)) {
            case 0: {  // a unit arrives
                UnitPresence up;
                up.owner = static_cast<PlayerId>(r.range_int(0, P - 1));
                up.layer = r.range_int(0, 2) == 0 ? UnitLayer::Ground : UnitLayer::Space;
                up.combat_power = static_cast<float>(r.range_int(50, 4000)) * 0.75f;
                units[i].push_back(up);
                break;
            }
            case 1:  // the oldest unit leaves / dies
                if (!units[i].empty()) units[i].erase(units[i].begin());
                break;
            case 2:  // ownership flips
                s.owner[i] = static_cast<PlayerId>(r.range_int(0, P - 1));
                break;
            default: {  // a player's view of the planet toggles
                const auto p = static_cast<size_t>(r.range_int(0, P - 1));
                if (p < s.sees_orbit.size() && i < s.sees_orbit[p].size())
                    s.sees_orbit[p].set(i, !s.sees_orbit[p].test(i));
                break;
            }
        }
    }
    build_csr(n, units, s);
}

}  // namespace alamo::ai

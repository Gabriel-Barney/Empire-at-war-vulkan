// Galactic AI perception tokens on the S3 GalacticSim (see galactic_ai_tokens.hpp). The rule of every leaf is
// docs/re/AI_GALACTIC_TOKENS.md; [port] marks a stand-in for data the port does not have.
#include "alamo/ai/galactic_ai_tokens.hpp"

#include <algorithm>
#include <cmath>

#include "alamo/ai/ai_free_store_path.hpp"
#include "alamo/ai/ai_types.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/economy.hpp"  // build_time_t0 (0x371230)
#include "alamo/galactic/fleet_path.hpp"
#include "alamo/objects/enums.hpp"
#include "galactic_slots.hpp"

namespace alamo::ai {

namespace gal = alamo::galactic;
using objects::ObjectHandle;

uint64_t galactic_ai_structure_mask(std::span<const GalacticAiTypeInfo> types, uint32_t type) {
    static const uint64_t builtin_capital = ai_category_bit("Capital");
    if (type >= types.size()) return builtin_capital;
    const GalacticAiTypeInfo& t = types[type];
    if (t.garrison_mask != 0) return t.garrison_mask;
    return t.capital_mask ? *t.capital_mask : builtin_capital;
}

bool GalacticAiTypeInfo::has_category(std::string_view c) const {
    for (const std::string& s : categories)
        if (ai_iequal(s, c)) return true;
    return false;
}

uint64_t ai_category_bit(std::string_view name, const objects::EnumRegistry* enums) {
    static const objects::EnumRegistry builtin;
    const objects::EnumRegistry& reg = enums ? *enums : builtin;
    const auto v = reg.get(objects::EnumId::GameObjectCategory).lookup(name);
    return v ? static_cast<uint64_t>(*v) : 0u;
}

namespace {
uint64_t names_mask(const GalacticAiTypeInfo& t, const objects::EnumRegistry* enums) {
    // [port, unchanged] the OR of the names the port knows; the exe converts the CategoryMask value as a whole (one
    // unknown token fails it, OBJECT_TYPES §9.1). Same result for shipped data (every token converts).
    uint64_t m = 0;
    for (const std::string& c : t.categories) m |= ai_category_bit(c, enums);
    return m;
}
}  // namespace

void fill_category_masks(std::vector<GalacticAiTypeInfo>& types, const objects::EnumRegistry* enums) {
    const uint64_t capital = ai_category_bit("Capital", enums);
    for (GalacticAiTypeInfo& t : types) {
        t.category_mask = names_mask(t, enums);
        t.capital_mask = capital;
    }
}

void fill_unset_category_masks(std::vector<GalacticAiTypeInfo>& types, const objects::EnumRegistry* enums) {
    const uint64_t capital = ai_category_bit("Capital", enums);
    for (GalacticAiTypeInfo& t : types) {
        if (t.category_mask == 0) t.category_mask = names_mask(t, enums);
        if (!t.capital_mask) t.capital_mask = capital;
    }
}

double galactic_ai_member_force(const gal::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types, uint32_t type,
                                int32_t owner, uint64_t mask, bool exact) {
    const auto& gt = sim.rules().types;
    double sum = 0.0;
    if (type >= types.size()) return sum;
    // Per-tech members (0x373f40 count / 0x373e30 type / 0x373d30 count, tech = the owner's tech level +0x84).
    // A level without entries falls back to the highest lower level with entries for the planet-level leaves (0x373f40 /
    // 0x373e30 / 0x373d30 [H]); the player-level tokens use the exact list (0x373fd0 / 0x373ee0, `exact`, no fall back)
    // (A2F_RE_READS §2). The original returns nothing for tech >= 6; the port clamps to slot 5 (unreachable in vanilla).
    const auto players = sim.players();
    const int32_t tech = owner >= 0 && static_cast<size_t>(owner) < players.size() ? players[static_cast<size_t>(owner)].tech_level : 0;
    for (int32_t n = std::min(tech, static_cast<int32_t>(kSpawnTechSlots) - 1); n >= 0; --n) {
        const std::vector<SpawnMember>& list = types[type].spawn_tech[static_cast<size_t>(n)];
        if (list.empty()) {
            if (exact) break;
            continue;
        }
        for (const SpawnMember& m : list)
            if (m.type < types.size() && m.type < gt.size() && m.count > 0 && (types[m.type].category_mask & mask) != 0)
                sum += static_cast<double>(gt[m.type].ai_combat_power) * static_cast<double>(m.count);
        break;
    }
    return sum;
}

double galactic_ai_planet_force(const gal::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types, PlanetIndex planet,
                                int32_t self, bool enemy, bool space, uint64_t mask) {
    const auto planets = sim.planets();
    const auto players = sim.players();
    const gal::GalacticRules& rules = sim.rules();
    const objects::ObjectStore& st = sim.store();
    if (static_cast<size_t>(planet) >= planets.size()) return 0.0;
    auto valid = [&](int32_t p) { return p >= 0 && static_cast<size_t>(p) < players.size(); };
    auto neutral = [&](int32_t p) {
        const uint32_t f = valid(p) ? players[static_cast<size_t>(p)].faction : gal::kNoIndex;
        return f < rules.factions.size() && rules.factions[f].is_neutral;
    };
    // Side filter [H]: Enemy 0x39a4e0 (hostile to Self), Friendly 0x39a2c0 (Self or an ally); unowned matches neither.
    auto side = [&](int32_t owner) {
        if (!valid(owner) || !valid(self)) return false;
        if (enemy) return sim.is_hostile(self, owner);
        return owner == self || (!sim.is_hostile(self, owner) && !neutral(owner) && !neutral(self));
    };
    auto power = [&](uint32_t t) { return t < rules.types.size() ? static_cast<double>(rules.types[t].ai_combat_power) : 0.0; };
    auto type_mask = [&](uint32_t t) { return t < types.size() ? types[t].category_mask : uint64_t{0}; };
    const gal::PlanetState& ps = planets[static_cast<size_t>(planet)];
    double sum = 0.0;
    if (space) {  // 0x6dbd50
        for (const uint32_t f : ps.orbit) {
            if (f >= sim.fleets().size()) continue;
            const gal::FleetState& fl = sim.fleets()[f];
            if (!fl.alive || fl.moving || !side(fl.owner)) continue;
            for (const ObjectHandle u : fl.contents) {
                if (!st.alive(u)) continue;
                const uint32_t t = static_cast<uint32_t>(st.type(u));
                if ((type_mask(t) & mask) != 0) sum += power(t);
                sum += galactic_ai_member_force(sim, types, t, fl.owner, mask);  // "+ its per-tech members" [H; port list]
            }
        }
        if (side(ps.owner)) {
            // A2I_TOKENS_RE §2.1 [H]. The starbase type's per-tech members with the tech of its owner (D +0x74; the port
            // reads the starbase object's owner: PlanetState::starbase_owner can be -1 while the object lives, seen in the
            // HotM human run), none when that owner is no player; not the starbase's own power.
            if (ps.starbase && st.alive(ps.starbase) && valid(st.owner(ps.starbase)))
                sum += galactic_ai_member_force(sim, types, static_cast<uint32_t>(st.type(ps.starbase)), st.owner(ps.starbase), mask);
            // Then the own power of the ground structures D +0x158 whose type byte +0x1E (Is_Special_Weapon_In_Space) is
            // set, then of every orbital structure D +0x170 (no +0x1E test), each when `mask` meets the type's structure
            // mask +0x1658 (Capital when 0). The port keeps both families in `structures`: DUMMY_ORBITAL_STRUCTURE (0x60)
            // types are the orbital ones. Vanilla: none of these types has AI_Combat_Power, so this adds 0.
            for (const bool orbital : {false, true})
                for (const ObjectHandle s : ps.structures) {
                    if (!st.alive(s)) continue;
                    const uint32_t t = static_cast<uint32_t>(st.type(s));
                    if (t >= rules.types.size() || gal::is_orbital_structure_type(rules.types[t]) != orbital) continue;
                    if (!orbital && !(t < types.size() && types[t].special_weapon_space)) continue;
                    if ((mask & galactic_ai_structure_mask(types, t)) != 0) sum += power(t);
                }
        }
    } else {  // 0x6d95d0
        for (const ObjectHandle u : ps.surface)
            if (st.alive(u) && side(st.owner(u)) && (type_mask(static_cast<uint32_t>(st.type(u))) & mask) != 0)
                sum += power(static_cast<uint32_t>(st.type(u)));
        // The per-tech members of every ground structure (D +0x158 only: orbital structures are skipped) with the tech of
        // the structure's own owner (S +0x58), not their own power and no structure mask (A2I_TOKENS_RE §2.2) [H]. The
        // Enemy planet-type list +0x20C8 [port: not modelled].
        if (side(ps.owner))
            for (const ObjectHandle s : ps.structures) {
                if (!st.alive(s)) continue;
                const uint32_t t = static_cast<uint32_t>(st.type(s));
                if (t < rules.types.size() && gal::is_orbital_structure_type(rules.types[t])) continue;
                sum += galactic_ai_member_force(sim, types, t, st.owner(s), mask);
            }
    }
    return sum;
}

double galactic_ai_enemy_strength(const gal::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types, PlanetIndex planet,
                                  int32_t self, bool space, uint64_t mask) {
    double s = galactic_ai_planet_force(sim, types, planet, self, true, space, mask);
    if (static_cast<size_t>(planet) >= sim.planets().size()) return s;
    const gal::PlanetState& ps = sim.planets()[static_cast<size_t>(planet)];
    const objects::ObjectStore& st = sim.store();
    const auto& gt = sim.rules().types;
    // A typed row (the PotentialPlan Contrast rows) adds a structure only when its type's CategoryMask (+0x1648) meets the
    // row's mask (A2F_RE_READS §3.1, a2i t03 D1 in A2I_T03_TRAJECTORY §3.3) [H]; the untyped row (kAllCategories) adds
    // every one, as before [port: a type without a CategoryMask counts there].
    auto power = [&](ObjectHandle h) {
        const uint32_t t = static_cast<uint32_t>(st.type(h));
        if (t >= gt.size()) return 0.0;
        if (mask != kAllCategories && (t >= types.size() || (types[t].category_mask & mask) == 0)) return 0.0;
        return static_cast<double>(gt[t].ai_combat_power);
    };
    // [port] "not held by the AI or an ally" (0x4ad9f0) is read as the leaf's Enemy owner filter (hostile to Self).
    if (ps.owner < 0 || !sim.is_hostile(self, ps.owner)) return s;
    if (space) {
        if (ps.starbase && st.alive(ps.starbase)) s += power(ps.starbase);  // the starbase type's AI_Combat_Power [H]
    } else {
        // Σ AI_Combat_Power of the ground structures D +0x158 (orbital ones are not in that list) [H]; garrison types
        // 0x4b8000 [port: none].
        for (const ObjectHandle u : ps.structures) {
            if (!st.alive(u)) continue;
            const uint32_t t = static_cast<uint32_t>(st.type(u));
            if (t < gt.size() && gal::is_orbital_structure_type(gt[t])) continue;
            s += power(u);
        }
    }
    return s;
}

namespace {

constexpr double kNever = 1.0e18;              // TimeSince* of an event that never happened [H]
constexpr double kMaxFleetDistance = 100.0;    // GC+0x108 MaximumFleetMovementDistance
constexpr double kMaxLevel = 5.0;              // GC+0xF4 / +0xF8 / +0xFC, MaximumPoliticalControl GC+0xF0
constexpr int32_t kLifeForever = -1;

// Galactic_Hints hint-set defaults [H].
constexpr std::string_view kGalacticHints[] = {"CHOKEPOINT", "OFFENSEDELAYGROUND", "OFFENSEDELAYSPACE", "PRIORITYTARGET"};

// Paths the port resolves (coverage report). Root-relative, upper-case.
const std::string_view kKnownPaths[] = {
    // player roots (Self / Human / Enemy)
    "PLANETSCONTROLLED", "PLANETSCONTROLLEDUNNORMALIZED", "CREDITSUNNORMALIZED", "TECHLEVEL", "ISFACTION",
    "ISDIFFICULTY", "HASSTRUCTURE", "NUMSTRUCTURE", "STRUCTURECOUNT", "HASUNIT", "HASSTARBASEOFLEVEL",
    "NUMSTARBASEOFLEVEL", "HASGROUNDBASEOFLEVEL", "NUMGROUNDBASEOFLEVEL", "MAXEDSTARBASES", "MAXEDGROUNDBASES",
    "MAXSTARBASELEVEL", "MAXSTARBASELEVELUNNORMALIZED", "MAXGROUNDBASELEVEL", "MAXGROUNDBASELEVELUNNORMALIZED",
    "MAINTENANCE", "MAINTENANCEUNNORMALIZED", "SPACETOTALUNNORMALIZED", "GROUNDTOTALUNNORMALIZED", "ANYCURRENTTHREATS",
    "AVERAGEAGEOFSPACEINTELLIGENCE", "AVERAGEAGEOFGROUNDINTELLIGENCE", "CANADVANCETECH", "BUDGETFRACTIONTOBUILD",
    "NETINCOMEUNNORMALIZED", "NETINCOMENBPTM", "NETINCOMENBTF", "GROSSINCOMEUNNORMALIZED", "GROSSINCOMENBPTM",
    "GROSSINCOMENBTF", "ACTIVEGOALS", "WORSTISLANDFRACTIONOFLARGEST", "HASTECHTOPRODUCE", "STORYTRIGGER",
    "SPACEDEFENDEDPLANETS", "GROUNDDEFENDEDPLANETS", "MAXALLPLANETSINCOMEUNNORMALIZED", "MAXOWNEDPLANETSINCOMEUNNORMALIZED",
    // target root (planets)
    "ISHUMANCONTROLLED", "STARBASELEVEL", "STARBASELEVELUNNORMALIZED", "GROUNDBASELEVEL", "GROUNDBASELEVELUNNORMALIZED",
    "MAXSTARBASELEVEL", "MAXSTARBASELEVELUNNORMALIZED", "MAXGROUNDBASELEVEL", "MAXGROUNDBASELEVELUNNORMALIZED",
    "BASEINCOME", "BASEINCOMENBTP", "INCOMEUNNORMALIZED", "MAXSTRUCTURESLOTS", "MAXSTRUCTURESLOTSUNNORMALIZED",
    "OPENSTRUCTURESLOTS", "OPENSTRUCTURESLOTSUNNORMALIZED", "OPENGROUNDCOMPANYSLOTS", "TRADEROUTES", "ACTIVETRADEROUTES",
    "HINTS.CHOKEPOINT", "HINTS.PRIORITYTARGET", "HINTS.OFFENSEDELAYGROUND", "HINTS.OFFENSEDELAYSPACE", "MARKUP",
    "CONNECTSISOLATEDPLANETSBYFORCE", "CONNECTSISOLATEDPLANETSBYLENGTH", "CONNECTSLARGESTISLANDS", "ISCONNECTEDTO",
    "ISCORRUPTED", "ISCONNECTEDTOCORRUPTION", "ISCORRUPTIONTRANSITIONACTIVE", "RETAINSRESIDUALINFLUENCE",
    "TIMESINCECORRUPTIONCHANGE", "ISSURFACEACCESSIBLE", "HASCREDITSIPHON", "TARGETPOLITICALCONTROL", "TARGETPLAYERID",
    "TIMESINCESPACECONFLICT", "TIMESINCEGROUNDCONFLICT", "TIMESINCECONVERSION", "HASSTARBASEOFLEVEL",
    "HASGROUNDBASEOFLEVEL", "HASSTRUCTURE", "STRUCTURECOUNT", "ISFACTION", "OWNER.ISFACTION", "TYPE.TOKEN",
    "BLACKMARKETABILITIESAVAILABLE", "BLACKMARKETMINIMUMABILITYPRICE", "BLACKMARKETPRICEMODIFIER",
    // planet forces (EnemyForce. / FriendlyForce. prefixes)
    "ENEMYFORCE.SPACETOTALUNNORMALIZED", "ENEMYFORCE.GROUNDTOTALUNNORMALIZED", "ENEMYFORCE.SPACETOTAL",
    "ENEMYFORCE.GROUNDTOTAL", "ENEMYFORCE.SPACETOTALNBTD", "ENEMYFORCE.GROUNDTOTALNBTD", "ENEMYFORCE.SPACETOTALNBPTM",
    "ENEMYFORCE.GROUNDTOTALNBPTM", "ENEMYFORCE.SPACETOTALNBTT", "ENEMYFORCE.GROUNDTOTALNBTT",
    "ENEMYFORCE.NEARBYSPACETOTAL", "ENEMYFORCE.NEARBYGROUNDTOTAL", "ENEMYFORCE.HASSPACEFORCE",
    "ENEMYFORCE.HASGROUNDFORCE", "ENEMYFORCE.HASNEARBYSPACEFORCE", "ENEMYFORCE.HASNEARBYGROUNDFORCE",
    "ENEMYFORCE.STARBASETOTALUNNORMALIZED", "ENEMYFORCE.GROUNDBASETOTALUNNORMALIZED",
    "ENEMYFORCE.HASSPACEUNITSBITFIELD", "ENEMYFORCE.HASGROUNDUNITSBITFIELD", "ENEMYFORCE.ANTISTEALTHEFFECTIVENESS",
    "ENEMYFORCE.MAINTENANCEUNNORMALIZED", "ENEMYFORCE.MAINTENANCE", "FRIENDLYFORCE.MAINTENANCEUNNORMALIZED",
    "FRIENDLYFORCE.MAINTENANCE",
    "ENEMYFORCE.SPACETOTAL.TIMELASTSEEN", "ENEMYFORCE.SPACETOTAL.TIMELASTSEENUNNORMALIZED",
    "ENEMYFORCE.GROUNDTOTAL.TIMELASTSEEN", "ENEMYFORCE.GROUNDTOTAL.TIMELASTSEENUNNORMALIZED",
    "FRIENDLYFORCE.SPACETOTALUNNORMALIZED", "FRIENDLYFORCE.GROUNDTOTALUNNORMALIZED", "FRIENDLYFORCE.SPACETOTAL",
    "FRIENDLYFORCE.GROUNDTOTAL", "FRIENDLYFORCE.SPACETOTALNBTD", "FRIENDLYFORCE.GROUNDTOTALNBTD",
    "FRIENDLYFORCE.SPACETOTALNBPTM", "FRIENDLYFORCE.GROUNDTOTALNBPTM", "FRIENDLYFORCE.HASSPACEFORCE",
    "FRIENDLYFORCE.HASGROUNDFORCE", "FRIENDLYFORCE.HASGROUNDUNITSBITFIELD", "FRIENDLYFORCE.NEARBYSPACETOTAL",
    "FRIENDLYFORCE.NEARBYGROUNDTOTAL", "FRIENDLYFORCE.STARBASETOTALUNNORMALIZED",
    // game root
    "AGE", "TIMELINE", "ISSTORYCAMPAIGN", "ISCAMPAIGNGAME", "ISLANDCONTROLGAME", "ISSETUPPHASE", "PLANETSCORRUPTED",
    "PLANETSCORRUPTEDUNNORMALIZED", "ENEMYFORCE.SPACETOTALUNNORMALIZED", "ENEMYFORCE.GROUNDTOTALUNNORMALIZED",
    "FRIENDLYFORCE.SPACETOTALUNNORMALIZED", "FRIENDLYFORCE.GROUNDTOTALUNNORMALIZED",
    "ENEMYFORCE.PERTARGETMAXSPACEFORCE", "ENEMYFORCE.PERTARGETMAXGROUNDFORCE", "FRIENDLYFORCE.PERTARGETMAXSPACEFORCE",
    "FRIENDLYFORCE.PERTARGETMAXGROUNDFORCE",
};

std::vector<std::string_view> param_texts(std::span<const Param> params, std::string_view name) {
    std::vector<std::string_view> out;
    for (const Param& p : params)
        if (ai_iequal(p.name, name)) {
            if (p.value.kind == ParamValue::Kind::Enum)
                for (const std::string& e : p.value.enum_values) out.emplace_back(e);
            else if (p.value.kind != ParamValue::Kind::Number)
                out.emplace_back(p.value.text);
        }
    return out;
}
bool has_param(std::span<const Param> params, std::string_view name) {
    for (const Param& p : params)
        if (ai_iequal(p.name, name)) return true;
    return false;
}
bool param_number(std::span<const Param> params, std::string_view name, double& out) {
    for (const Param& p : params)
        if (ai_iequal(p.name, name) && p.value.kind == ParamValue::Kind::Number) {
            out = p.value.number;
            return true;
        }
    return false;
}
std::string param_key(std::span<const Param> params) {
    std::string k;
    for (const Param& p : params) {
        k += ai_upper_copy(p.name);
        k += '=';
        switch (p.value.kind) {
            case ParamValue::Kind::Number: {
                char b[40];
                std::snprintf(b, sizeof b, "%.17g", p.value.number);
                k += b;
                break;
            }
            case ParamValue::Kind::Enum:
                k += ai_upper_copy(p.value.text);
                for (const std::string& e : p.value.enum_values) (k += '|') += ai_upper_copy(e);
                break;
            default: k += ai_upper_copy(p.value.text); break;
        }
        k += ';';
    }
    return k;
}
double b2d(bool b) { return b ? 1.0 : 0.0; }
// Normalisation rule of the suffix leaves [H]: num 0 -> 0; num <= den -> num/den (>= 0); else 1.
double normalise(double num, double den) {
    if (num == 0.0) return 0.0;
    if (num <= den) return std::clamp(num / den, 0.0, 1.0);
    return 1.0;
}

bool space_kind(gal::UnitKind k) { return k == gal::UnitKind::SpaceUnit || k == gal::UnitKind::Squadron; }
bool ground_kind(gal::UnitKind k) { return k == gal::UnitKind::GroundUnit; }

}  // namespace

std::span<const std::string_view> GalacticSimTokenResolver::known_paths() { return kKnownPaths; }

// ================================================================ buildability mirror

bool galactic_ai_player_can_build(const gal::GalacticSim& sim, int32_t player, uint32_t type) {
    const auto& rules = sim.rules();
    if (type >= rules.types.size() || player < 0 || static_cast<size_t>(player) >= sim.players().size()) return false;
    const gal::TypeInfo& t = rules.types[type];
    if (t.build_cost <= 0) return false;
    // Story DISABLE_BUILDABLE (production-disabled list 0x282510, REMAINING_GAPS §1.5) [port: also for team picks].
    if (sim.production_disabled(player, type)) return false;
    if (!t.affiliation.empty()) {
        const uint32_t f = sim.players()[static_cast<size_t>(player)].faction;
        if (std::find(t.affiliation.begin(), t.affiliation.end(), f) == t.affiliation.end()) return false;
    }
    return t.tech_level <= sim.players()[static_cast<size_t>(player)].tech_level;
}

bool galactic_ai_can_build(const gal::GalacticSim& sim, int32_t player, uint32_t type, PlanetIndex p) {
    // GalacticSim::buildable = cost >= 1, a queue kind and GalacticSim::build_check (buildability 0x2804d0 in the
    // STRATEGIC_GAPS §1.3 order: next starbase level, surface, Required_Special_Structures OR groups, ...; credits
    // not checked). The former AI-side copies of those rules are gone (OI ai follow-up).
    return sim.buildable(p, player, type);
}

// ================================================================ delivery path class (REMAINING_GAPS §6.2)

int galactic_ai_delivery_mode(bool stealth_company, bool task_flag_da) {
    if (stealth_company) return static_cast<int>(Reachability::Any);
    return static_cast<int>(task_flag_da ? Reachability::EnemyUndefended : Reachability::EnemyDestination);
}

std::vector<uint8_t> galactic_ai_delivery_flood(const gal::GalacticSim& sim, int32_t owner, PlanetIndex from, int mode,
                                                const gal::Bitset* dangerous) {
    const size_t n = sim.planets().size();
    std::vector<uint8_t> r(n, 0);
    const gal::PlanetGraph& g = sim.graph();
    if (from >= n || !sim.planets()[from].is_planet || !g.included(from) || owner < 0 ||
        static_cast<size_t>(owner) >= sim.players().size())
        return r;
    // The flood 0x3f12e0 under the corrected AI request flags (FIXPASS_GALACTIC §1.4, ai_reach_flood). Request flags 0
    // and 1 are equal at every AI level, so the perceived-owner bit (ai_perceived_non_neutral, which the sim's view now
    // fills with the true owner, the AI's own view overlays the remembered one) is behaviourally inert for this flood
    // for now. Danger (flags 2 / 3) reads `dangerous` (the AI's Is_Planet_Dangerous, none when null).
    gal::PlayerView view = sim.player_view(owner);
    if (dangerous) view.dangerous = *dangerous;
    const PlanetIndex start[1] = {from};
    ai_reach_flood(g, view, start, mode, &r);
    r.resize(n, 0);
    return r;
}

// ================================================================ S2 snapshot

GalacticSnapshot snapshot_from_sim(const gal::GalacticSim& sim, bool ai_uses_fog_galactic) {
    GalacticSnapshot s;
    const auto planets = sim.planets();
    const auto players = sim.players();
    const auto& rules = sim.rules();
    const objects::ObjectStore& st = sim.store();
    s.planet_count = static_cast<uint32_t>(planets.size());
    for (size_t p = 0; p < players.size(); ++p) {
        PerceptionPlayer pp;
        const uint32_t f = players[p].faction;
        pp.neutral = f < rules.factions.size() && rules.factions[f].is_neutral;
        pp.benefits_from_corruption = f < rules.factions.size() && rules.factions[f].benefits_from_corruption;
        pp.is_human = players[p].human;
        pp.has_ai_system = players[p].ai;
        s.players.push_back(pp);
    }
    const size_t P = players.size();
    s.relation.assign(P * P, 1);
    for (size_t a = 0; a < P; ++a)
        for (size_t b = 0; b < P; ++b) {
            if (a == b) {
                s.relation[a * P + b] = 0;
                continue;
            }
            // 0x4ad9f0 table (A2F_RE_READS §3.2 [H]): 0 friendly (self, ally), 1 enemy, 2 neutral. The port enum is Self 0,
            // Ally 1, Neutral 2, Enemy 3, so the map is explicit.
            switch (sim.relation(static_cast<int32_t>(a), static_cast<int32_t>(b))) {
                case gal::Relation::Self:
                case gal::Relation::Ally: s.relation[a * P + b] = 0; break;
                case gal::Relation::Enemy: s.relation[a * P + b] = 1; break;
                case gal::Relation::Neutral: s.relation[a * P + b] = 2; break;
            }
        }
    auto power = [&](ObjectHandle h) {
        const objects::TypeIndex t = st.type(h);
        return t < rules.types.size() ? rules.types[t].ai_combat_power : 0.0f;
    };
    s.unit_begin.push_back(0);
    s.structure_begin.push_back(0);
    for (size_t i = 0; i < planets.size(); ++i) {
        const gal::PlanetState& ps = planets[i];
        s.owner.push_back(ps.owner);
        s.political_control.push_back(ps.control);
        s.starbase_level.push_back(ps.starbase_level);
        s.starbase_power.push_back(ps.starbase && st.alive(ps.starbase) ? power(ps.starbase) : 0.0f);
        s.corruption.push_back(ps.corruption);
        for (const uint32_t f : ps.orbit) {
            if (f >= sim.fleets().size() || !sim.fleets()[f].alive) continue;
            for (const ObjectHandle u : sim.fleets()[f].contents) {
                if (!st.alive(u)) continue;
                const objects::TypeIndex t = st.type(u);
                const bool space = t < rules.types.size() && space_kind(rules.types[t].kind);
                s.units.push_back({st.owner(u), power(u), space ? UnitLayer::Space : UnitLayer::Ground});
            }
        }
        for (const ObjectHandle u : ps.surface)
            if (st.alive(u)) s.units.push_back({st.owner(u), power(u), UnitLayer::Ground});
        s.unit_begin.push_back(static_cast<uint32_t>(s.units.size()));
        for (const ObjectHandle u : ps.structures)
            if (st.alive(u)) s.structure_power.push_back(power(u));
        s.structure_begin.push_back(static_cast<uint32_t>(s.structure_power.size()));
    }
    // AIUsesFogOfWarGalactic (GC +0x2EC; shipped False => the AI sees live values, AI_GALACTIC_TOKENS fog answer).
    s.fog_disabled = !ai_uses_fog_galactic;
    // GRC 2 visibility (GALACTIC_REVEAL_CORRUPTION §2.3): the fogged leaves of an AI player follow the presence level
    // the sim signals with 0xE for that player (0xFFFFFFFF when it owns the planet); spies and story entries do not enter
    // the signal [M: the signal carries the presence value only]. Only read while fog is on, so only built then.
    if (ai_uses_fog_galactic) {
        s.visibility.assign(players.size(), {});
        for (size_t p = 0; p < players.size(); ++p) {
            if (!players[p].ai) continue;
            std::vector<uint32_t>& row = s.visibility[p];
            row.resize(planets.size());
            for (size_t i = 0; i < planets.size(); ++i)
                row[i] = sim.visibility_sources(static_cast<PlanetIndex>(i), static_cast<int32_t>(p)).presence;
        }
    }
    // The LOCAL player's folded mask (PD +0x1A8) bits the non-AI branches read: 0x1 ownership visible (passability),
    // 0x10 orbiting units visible (danger). No local player (all AI): left empty = visible.
    if (const int32_t local = sim.local_player(); local >= 0) {
        s.ownership_visible = galactic::Bitset(planets.size());
        s.local_orbit_visible = galactic::Bitset(planets.size());
        for (size_t i = 0; i < planets.size(); ++i) {
            const uint32_t m = sim.visibility_mask(static_cast<PlanetIndex>(i), local);
            s.ownership_visible.set(i, (m & gal::kVisPlanetOwnership) != 0);
            s.local_orbit_visible.set(i, (m & gal::kVisFleetPresence) != 0);
        }
    }
    // LOCAL reveal state (GALACTIC_REVEAL_CORRUPTION 1.1): Is_Planet_Revealed takes no player, so the AI reads the
    // local human's bytes. Multiplayer is not modelled by the sim.
    s.planet_auto_reveal = sim.planet_auto_reveal();
    s.revealed = galactic::Bitset(planets.size());
    s.hidden = galactic::Bitset(planets.size());
    for (size_t i = 0; i < planets.size(); ++i) {
        s.revealed.set(i, sim.planet_revealed_raw(static_cast<PlanetIndex>(i)));
        s.hidden.set(i, !planets[i].is_planet || planets[i].hidden);  // no data pack / hidden byte +0x2E0
    }
    return s;
}

// ================================================================ resolver

GalacticSimTokenResolver::GalacticSimTokenResolver(const gal::GalacticSim& sim, std::span<const GalacticAiTypeInfo> types,
                                                   const TokenResolver* s2, Hooks hooks)
    : sim_(&sim), types_(types), s2_(s2), hooks_(std::move(hooks)) {}

void GalacticSimTokenResolver::clear_cache() {
    std::lock_guard<std::mutex> lock(mu_);
    cache_.clear();
}

uint64_t GalacticSimTokenResolver::cache_hits() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
}

// Planet evaluator neighbours (0x661da0) [H]: every other planet within GC+0x108 (3-D) or route-connected.
void GalacticSimTokenResolver::build_neighbours() {
    const auto planets = sim_->planets();
    const size_t n = planets.size();
    neighbours_.assign(n, {});
    const gal::PlanetGraph& g = sim_->graph();
    for (size_t i = 0; i < n; ++i) {
        if (!planets[i].is_planet) continue;
        for (size_t j = 0; j < n; ++j) {
            if (j == i || !planets[j].is_planet) continue;
            const double dx = static_cast<double>(planets[i].position.x) - planets[j].position.x;
            const double dy = static_cast<double>(planets[i].position.y) - planets[j].position.y;
            const double dz = static_cast<double>(planets[i].position.z) - planets[j].position.z;
            bool near = std::sqrt(dx * dx + dy * dy + dz * dz) <= kMaxFleetDistance;
            if (!near && g.included(static_cast<PlanetIndex>(i)))
                for (uint32_t e = g.edge_begin(static_cast<PlanetIndex>(i)); e < g.edge_end(static_cast<PlanetIndex>(i)); ++e)
                    near = near || g.neighbour(e) == j;
            if (near) neighbours_[i].push_back(static_cast<uint32_t>(j));
        }
    }
}

void GalacticSimTokenResolver::refresh(int32_t frame) {
    frame_ = frame;
    const auto planets = sim_->planets();
    const size_t n = planets.size();
    if (neighbours_.size() != n) {
        build_neighbours();
        stamp_space_.assign(n, -1);
        stamp_ground_.assign(n, -1);
        stamp_conversion_.assign(n, -1);
        stamp_corruption_.assign(n, 0);  // D+0x2F0 [M: starts at frame 0]
        last_owner_.assign(n, gal::kNoPlayer);
        last_corruption_.assign(n, gal::kCorruptionNone);
        last_battle_.assign(n, 0);
        for (size_t i = 0; i < n; ++i) {
            last_owner_[i] = planets[i].owner;
            last_corruption_[i] = planets[i].corruption;
        }
    }
    // Event stamps (signals 0x12 space conflict, 0x14 ground conflict, 0x1A conversion) [H]; the port observes
    // the conflict flag's rising edge: hostile fleets in orbit = space, else ground [port].
    const objects::ObjectStore& st = sim_->store();
    for (size_t i = 0; i < n; ++i) {
        const gal::PlanetState& ps = planets[i];
        if (ps.owner != last_owner_[i]) {
            stamp_conversion_[i] = frame;
            last_owner_[i] = ps.owner;
        }
        if (ps.corruption != last_corruption_[i]) {
            stamp_corruption_[i] = frame;
            last_corruption_[i] = ps.corruption;
        }
        const uint8_t battle = ps.in_battle ? 1 : 0;
        if (battle && !last_battle_[i]) {
            bool hostile_orbit = false;
            for (const uint32_t f : ps.orbit) {
                if (f >= sim_->fleets().size() || !sim_->fleets()[f].alive) continue;
                for (const uint32_t g : ps.orbit)
                    if (g < sim_->fleets().size() && sim_->fleets()[g].alive &&
                        sim_->is_hostile(sim_->fleets()[f].owner, sim_->fleets()[g].owner))
                        hostile_orbit = true;
                if (ps.owner >= 0 && sim_->is_hostile(sim_->fleets()[f].owner, ps.owner)) hostile_orbit = true;
            }
            bool hostile_ground = false;
            for (const ObjectHandle u : ps.surface)
                if (st.alive(u) && ps.owner >= 0 && sim_->is_hostile(st.owner(u), ps.owner)) hostile_ground = true;
            if (hostile_orbit || !hostile_ground) stamp_space_[i] = frame;
            else stamp_ground_[i] = frame;
        }
        last_battle_[i] = battle;
    }
}

std::string GalacticSimTokenResolver::path_key(Root root, std::span<const TokenId> path, const TokenTable& tokens) {
    std::string k = ai_upper_copy(root_name(root));
    for (const TokenId id : path) {
        k += '.';
        k += ai_upper_copy(tokens.name(id));
    }
    return k;
}

bool GalacticSimTokenResolver::implements(Root root, std::span<const TokenId> path, const TokenTable& tokens) const {
    std::string k;
    for (const TokenId id : path) {
        if (!k.empty()) k += '.';
        k += ai_upper_copy(tokens.name(id));
    }
    (void)root;
    for (const std::string_view p : kKnownPaths)
        if (p == k) return true;
    return false;
}

bool GalacticSimTokenResolver::resolve_script(const EvalContext& ctx, std::string_view name, std::span<const Param> params,
                                              double& out) const {
    return hooks_.script && hooks_.script(ctx, name, params, out);
}

// ---------------------------------------------------------------- evaluation helpers

struct GalacticSimTokenResolver::Impl {
    const GalacticSimTokenResolver& r;
    const EvalContext& ctx;
    const gal::GalacticSim& sim;
    const gal::GalacticRules& rules;
    const objects::ObjectStore& st;
    std::span<const gal::PlanetState> planets;
    std::span<const gal::PlayerState> players;

    Impl(const GalacticSimTokenResolver& res, const EvalContext& c)
        : r(res), ctx(c), sim(*res.sim_), rules(res.sim_->rules()), st(res.sim_->store()), planets(res.sim_->planets()),
          players(res.sim_->players()) {}

    // ---- the leaf cache ----
    template <class F>
    bool leaf(std::string_view key, int32_t inst, int32_t life, std::span<const Param> params, F&& compute, double& out) const {
        CacheKey k;
        if (life != 0) {
            k.leaf = std::string(key);
            k.inst = inst;
            k.self = ctx.self;
            k.target = ctx.target;
            k.enemy = ctx.enemy;
            k.human = ctx.human;
            k.params = param_key(params);
            std::lock_guard<std::mutex> lock(r.mu_);
            const auto it = r.cache_.find(k);
            if (it != r.cache_.end() && it->second.stamp != 0 &&
                (life == kLifeForever ||
                 static_cast<int64_t>(r.frame_) <= static_cast<int64_t>(it->second.stamp) + static_cast<int64_t>(life) * kAiFps)) {
                out = it->second.value;
                ++r.hits_;
                return true;
            }
        }
        if (!compute(out)) return false;
        if (life != 0) {
            std::lock_guard<std::mutex> lock(r.mu_);
            Entry& e = r.cache_[k];
            e.value = out;
            e.stamp = r.frame_;
        }
        return true;
    }

    bool valid_player(int32_t p) const { return p >= 0 && static_cast<size_t>(p) < players.size(); }
    bool valid_planet(int32_t p) const { return p >= 0 && static_cast<size_t>(p) < planets.size() && planets[static_cast<size_t>(p)].is_planet; }
    bool neutral(int32_t p) const {
        const uint32_t f = valid_player(p) ? players[static_cast<size_t>(p)].faction : gal::kNoIndex;
        return f < rules.factions.size() && rules.factions[f].is_neutral;
    }
    // Side filter of an owner relative to Self [H]: Enemy 0x39a4e0 (hostile), Friendly 0x39a2c0 (self or ally).
    bool side(bool enemy, int32_t owner) const {
        if (!valid_player(owner) || !valid_player(ctx.self)) return false;
        if (enemy) return sim.is_hostile(ctx.self, owner);
        return owner == ctx.self || (!sim.is_hostile(ctx.self, owner) && !neutral(owner) && !neutral(ctx.self));
    }
    uint64_t type_mask(uint32_t t) const { return t < r.types_.size() ? r.types_[t].category_mask : 0u; }
    double power(uint32_t t) const { return t < rules.types.size() ? static_cast<double>(rules.types[t].ai_combat_power) : 0.0; }
    // Parameter_Category read with 0x5f4780 (a raw qword) [H]: a `Type[...]` literal carries the value its converter gave
    // when the equation was built (A2I_TOKENS_RE §3.1; with A2J t07 that converter may come from Data\XML\Enum), so it is
    // used as is; any other value form keeps the by-name lookup [port: in the built-in table, the resolver holds no
    // converter; every shipped Parameter_Category is a literal].
    static uint64_t category_param(std::span<const Param> params) {
        if (!has_param(params, "Parameter_Category")) return kAllCategories;
        uint64_t m = 0;
        for (const Param& p : params) {
            if (!ai_iequal(p.name, "Parameter_Category")) continue;
            if (p.value.kind == ParamValue::Kind::Enum) m |= static_cast<uint64_t>(p.value.enum_value);
            else if (p.value.kind != ParamValue::Kind::Number) m |= ai_category_bit(p.value.text);
        }
        return m;
    }
    bool type_named(uint32_t t, std::span<const std::string_view> names) const {
        if (t >= rules.types.size()) return false;
        for (const std::string_view n : names)
            if (ai_iequal(rules.types[t].name, n)) return true;
        return false;
    }
    // D+0x230 = the planet type's Special_Structures_Land (+0x92C) (MaxStructureSlots; MaxGroundbaseLevel takes
    // min(5, it) [M: the D path 0x4b7eb0 is unread, the no-D path is type+0x92C]); not the legacy <Max_Ground_Base>
    // (0 on every shipped planet). Tag absent (-1) -> 5 [port] (galactic_slots.hpp, A2 f t01).
    int32_t max_ground_base(size_t p) const {
        const int32_t m = galactic_ai_ground_slot_max(sim, static_cast<PlanetIndex>(p));
        return m >= 0 ? m : 5;
    }
    int32_t max_space_base(size_t p) const {
        const auto& m = sim.map().planets.max_space_base;
        return p < m.size() && m[p] >= 0 ? m[p] : 5;
    }
    double distance(size_t a, size_t b) const {
        const double dx = static_cast<double>(planets[a].position.x) - planets[b].position.x;
        const double dy = static_cast<double>(planets[a].position.y) - planets[b].position.y;
        const double dz = static_cast<double>(planets[a].position.z) - planets[b].position.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    bool routed(size_t a, size_t b) const {
        const gal::PlanetGraph& g = sim.graph();
        if (!g.included(static_cast<PlanetIndex>(a))) return false;
        for (uint32_t e = g.edge_begin(static_cast<PlanetIndex>(a)); e < g.edge_end(static_cast<PlanetIndex>(a)); ++e)
            if (g.neighbour(e) == b) return true;
        return false;
    }
    // Planet income, read-only (Planet_Income(P, gross = 0) 0x3f68b0 without the side effects) [port].
    double planet_income(size_t i) const {
        const gal::PlanetState& p = planets[i];
        if (!p.is_planet || p.type >= rules.types.size()) return 0.0;
        if (sim.frame() <= p.halt_until) return 0.0;
        const gal::EconomyConstants& c = rules.constants;
        const int32_t b = p.destroyed ? rules.types[p.type].destroyed_credit_value : p.base_value;
        float sum = 0.0f;
        for (const gal::IncomeEntry& e : p.income)
            if (e.kind == gal::kIncomeAbility && e.expiry >= sim.frame()) sum += e.value;
        sum += gal::econ::income_base(b, p.black_market);
        bool keep = false;
        const gal::ForceSide owner_side =
            valid_player(p.owner) && players[static_cast<size_t>(p.owner)].faction < rules.factions.size()
                ? rules.factions[players[static_cast<size_t>(p.owner)].faction].force_alignment
                : gal::ForceSide::Neutral;
        const float force = gal::econ::income_force(b, owner_side, rules.types[p.type].the_force, rules.types[p.type].force_strength,
                                                    c.credit_alignment_bonus, c.credit_alignment_penalty, &keep);
        if (keep) sum += force;
        if (p.owner != sim.neutral_player()) sum += gal::econ::income_political(b, p.control, c.political_income_curve);
        float gain = 0.0f;
        const GalaxyMap& m = sim.map();
        for (const uint32_t rt : p.links) {
            const PlanetIndex nb = m.routes.point_a[rt] == i ? m.routes.point_b[rt] : m.routes.point_a[rt];
            const float factor = m.routes.credit_gain_factor[rt];
            if (nb < planets.size() && planets[nb].is_planet && valid_player(p.owner) && planets[nb].owner == p.owner && factor > 0.0f)
                gain += factor;
        }
        sum += gal::econ::income_trade(b, gain);
        float siphoned = 0.0f;
        return static_cast<double>(gal::econ::income_finish(sum, p.min_income_set, p.min_income, p.max_income_set, p.max_income,
                                                            false, 0.0f, &siphoned));
    }

    // Potential income 0x3f83b0 (A2I_TOKENS_RE §1.4) [H]: the planet's income at full political control, in float, in this
    // order: base x black-market multiplier, the force-alignment term (as Planet_Income), the political term at
    // MaximumPoliticalControl when the owner is not the neutral player, trade routes over every qualifying link whatever
    // the neighbour's owner (none when the owner is no player). No halt, ability entries, min / max clamp or siphon.
    double potential_income(size_t i) const {
        const gal::PlanetState& p = planets[i];
        if (!p.is_planet || p.type >= rules.types.size()) return 0.0;
        const gal::EconomyConstants& c = rules.constants;
        const int32_t b = p.destroyed ? rules.types[p.type].destroyed_credit_value : p.base_value;
        float sum = gal::econ::income_base(b, p.black_market);
        bool keep = false;
        const gal::ForceSide owner_side =
            valid_player(p.owner) && players[static_cast<size_t>(p.owner)].faction < rules.factions.size()
                ? rules.factions[players[static_cast<size_t>(p.owner)].faction].force_alignment
                : gal::ForceSide::Neutral;
        const float force = gal::econ::income_force(b, owner_side, rules.types[p.type].the_force, rules.types[p.type].force_strength,
                                                    c.credit_alignment_bonus, c.credit_alignment_penalty, &keep);
        if (keep) sum += force;
        if (p.owner != sim.neutral_player())
            sum += gal::econ::income_political(b, static_cast<float>(kMaxLevel), c.political_income_curve);
        float gain = 0.0f;
        if (valid_player(p.owner)) {
            const GalaxyMap& m = sim.map();
            for (const uint32_t rt : p.links) {
                const PlanetIndex nb = m.routes.point_a[rt] == i ? m.routes.point_b[rt] : m.routes.point_a[rt];
                const float factor = m.routes.credit_gain_factor[rt];
                if (nb < planets.size() && planets[nb].is_planet && factor > 0.0f) gain += factor;
            }
        }
        sum += gal::econ::income_trade(b, gain);
        return static_cast<double>(sum);
    }

    // ---------------- planet force (PlanetForce 0x6dcb70) ----------------
    // Target.<side>.MaintenanceUnnormalized 0x6daf20 (A2I_TOKENS_RE §1.2) [H], lifetime 15 (fogged leaf, mask 0x40: not
    // modelled, like every fogged leaf of this resolver; AIUsesFogOfWarGalactic is False in the shipped data, so the leaf
    // is live; A2J_T07_LITERALS §4 has the fog-on rule): over the orbiting, non-moving fleets of that side, every
    // member M of type T with f = the Maintenance_Cost of M's OWNER's faction and u = T's Maintenance_Cost: skip
    // f x u = 0, btime(T, none) = 0, or a non-zero Parameter_Category (default all bits; 0 = no test) that T's
    // CategoryMask does not meet; add (double)(f x u x (cost / btime / 30)) in float.
    bool force_maintenance_unn(size_t p, bool enemy, std::span<const Param> params, double& out) const {
        const uint64_t cat = category_param(params);
        return leaf(enemy ? "PF:E.MAINTUNN" : "PF:F.MAINTUNN", static_cast<int32_t>(p), 15, params, [&](double& v) {
            v = 0.0;
            for (const uint32_t f : planets[p].orbit) {
                if (f >= sim.fleets().size()) continue;
                const gal::FleetState& fl = sim.fleets()[f];
                if (!fl.alive || fl.moving || !side(enemy, fl.owner)) continue;
                for (const ObjectHandle u : fl.contents) {
                    if (!st.alive(u)) continue;
                    const uint32_t t = st.type(u);
                    if (t >= rules.types.size()) continue;
                    const gal::TypeInfo& ti = rules.types[t];
                    const int32_t mo = st.owner(u);
                    const uint32_t fac = valid_player(mo) ? players[static_cast<size_t>(mo)].faction : gal::kNoIndex;
                    const float fm = fac < rules.factions.size() ? rules.factions[fac].maintenance_cost : 0.0f;
                    const float fu = fm * ti.maintenance_cost;
                    if (fu == 0.0f) continue;
                    const float bt = gal::econ::build_time_t0(ti.build_time_seconds, rules.constants.production_speed_factor);
                    if (bt == 0.0f) continue;
                    if (cat != 0 && (type_mask(t) & cat) == 0) continue;
                    v += static_cast<double>(fu * (static_cast<float>(ti.build_cost) / bt / static_cast<float>(kAiFps)));
                }
            }
            return true;
        }, out);
    }
    // Target.<side>.Maintenance 0x6dabc0 (A2I_TOKENS_RE §1.3) [H], lifetime 15: num = the side's MaintenanceUnnormalized
    // (no caller parameters); 0 -> 0; den = Variable_Enemy (Enemy side) / Variable_Self (Friendly side)
    // .GrossIncomeUnnormalized, its success ignored; den > 0 -> num / den, else the token fails.
    bool force_maintenance(size_t p, bool enemy, std::span<const Param> params, double& out) const {
        return leaf(enemy ? "PF:E.MAINT" : "PF:F.MAINT", static_cast<int32_t>(p), 15, params, [&](double& v) {
            double num = 0.0;
            if (!force_maintenance_unn(p, enemy, {}, num)) return false;
            if (num == 0.0) {
                v = 0.0;
                return true;
            }
            double den = 0.0;
            if (!resolve_player(enemy ? ctx.enemy : ctx.self, "GROSSINCOMEUNNORMALIZED", {}, den)) den = 0.0;
            if (!(den > 0.0)) return false;
            v = num / den;
            return true;
        }, out);
    }
    // GroundTotalUnnormalized 0x6d95d0 / SpaceTotalUnnormalized 0x6dbd50 (live; the cache is the caller's).
    double force_raw(size_t p, bool enemy, bool space, uint64_t cat) const {
        return galactic_ai_planet_force(sim, r.types_, static_cast<PlanetIndex>(p), ctx.self, enemy, space, cat);
    }
    bool force_unn(size_t p, bool enemy, bool space, std::span<const Param> params, double& out) const {
        const char* key = space ? (enemy ? "PF:E.SPACE" : "PF:F.SPACE") : (enemy ? "PF:E.GROUND" : "PF:F.GROUND");
        const uint64_t cat = category_param(params);
        return leaf(key, static_cast<int32_t>(p), 15, params, [&](double& v) {
            v = force_raw(p, enemy, space, cat);
            return true;
        }, out);
    }
    // Game.<side>.X (GameForce 0x6e3a10): Σ over the target list with the caller's params dropped; 30 s.
    bool game_force(bool enemy, bool space, bool max_of, double& out) const {
        const char* key = max_of ? (space ? (enemy ? "G:E.MAXSPACE" : "G:F.MAXSPACE") : (enemy ? "G:E.MAXGROUND" : "G:F.MAXGROUND"))
                                 : (space ? (enemy ? "G:E.SPACE" : "G:F.SPACE") : (enemy ? "G:E.GROUND" : "G:F.GROUND"));
        return leaf(key, -1, 30, {}, [&](double& v) {
            v = 0.0;
            for (size_t p = 0; p < planets.size(); ++p) {
                if (!planets[p].is_planet) continue;
                double x = 0.0;
                if (!force_unn(p, enemy, space, {}, x)) return false;
                v = max_of ? std::max(v, x) : v + x;
            }
            return true;
        }, out);
    }
    bool has_force(size_t p, bool enemy, bool space, std::span<const Param> params, double& out) const {
        const char* key = space ? (enemy ? "PF:E.HASSPACE" : "PF:F.HASSPACE") : (enemy ? "PF:E.HASGROUND" : "PF:F.HASGROUND");
        const std::vector<std::string_view> fac = param_texts(params, "Parameter_Faction");
        auto faction_ok = [&](int32_t owner) {
            if (fac.empty()) return true;
            const uint32_t f = valid_player(owner) ? players[static_cast<size_t>(owner)].faction : gal::kNoIndex;
            for (const std::string_view n : fac)
                if (f < rules.factions.size() && ai_iequal(rules.factions[f].name, n)) return true;
            return false;
        };
        return leaf(key, static_cast<int32_t>(p), 7, params, [&](double& v) {
            bool any = false;
            const gal::PlanetState& ps = planets[p];
            if (space) {
                for (const uint32_t f : ps.orbit)
                    if (f < sim.fleets().size() && sim.fleets()[f].alive && !sim.fleets()[f].moving &&
                        side(enemy, sim.fleets()[f].owner) && faction_ok(sim.fleets()[f].owner))
                        any = true;
            } else {
                for (const ObjectHandle u : ps.surface)
                    if (st.alive(u) && side(enemy, st.owner(u)) && faction_ok(st.owner(u))) any = true;
            }
            v = b2d(any);
            return true;
        }, out);
    }

    bool resolve_force(size_t p, bool enemy, std::string_view k, std::span<const Param> params, double& out) const {
        auto opposite_total = [&](bool space, double& v) {
            double a = 0.0, b = 0.0;
            if (!game_force(enemy, space, false, a) || !game_force(!enemy, space, false, b)) return false;
            v = a + b;
            return true;
        };
        for (const bool space : {true, false}) {
            const std::string base = space ? "SPACETOTAL" : "GROUNDTOTAL";
            if (k == base + "UNNORMALIZED") return force_unn(p, enemy, space, params, out);
            if (k == base + ".TIMELASTSEEN" || k == base + ".TIMELASTSEENUNNORMALIZED") {
                out = 0.0;  // fog off: always visible [H]
                return true;
            }
            if (k == base || k == base + "NBTD" || k == base + "NBPTM" || k == base + "NBTT") {
                const std::string key = std::string(enemy ? "PF:E." : "PF:F.") + std::string(k);
                return leaf(key, static_cast<int32_t>(p), 15, params, [&](double& v) {
                    double num = 0.0, den = 0.0;
                    if (!force_unn(p, enemy, space, params, num)) return false;
                    if (k == base) {
                        if (!opposite_total(space, den)) return false;
                    } else if (k == base + "NBTD") {
                        if (!game_force(enemy, space, false, den)) return false;
                    } else if (k == base + "NBPTM") {
                        if (!game_force(enemy, space, true, den)) return false;
                    } else if (!force_unn(p, enemy, space, {}, den)) {
                        return false;
                    }
                    v = normalise(num, den);
                    return true;
                }, out);
            }
            if (k == (space ? std::string("NEARBYSPACETOTAL") : std::string("NEARBYGROUNDTOTAL"))) {
                const std::string key = std::string(enemy ? "PF:E." : "PF:F.") + std::string(k);
                return leaf(key, static_cast<int32_t>(p), 30, params, [&](double& v) {
                    double a = 1.0;  // Parameter_Attenuator default 1.0 (no attenuation) [H]
                    param_number(params, "Parameter_Attenuator", a);
                    double s = 0.0;
                    auto add = [&](size_t n) {
                        double x = 0.0;
                        if (!force_unn(n, enemy, space, params, x)) return false;
                        s += (1.0 - distance(p, n) / kMaxFleetDistance * (1.0 - a)) * x;
                        return true;
                    };
                    if (!add(p)) return false;  // the force neighbour list includes the planet itself
                    for (const uint32_t n : r.neighbours_[p])
                        if (!add(n)) return false;
                    double den = 0.0;
                    if (!opposite_total(space, den)) return false;
                    if (den == 0.0) {
                        if (s != 0.0) return false;
                        v = 0.0;
                        return true;
                    }
                    v = s / den;
                    return true;
                }, out);
            }
            if (k == (space ? std::string("HASSPACEFORCE") : std::string("HASGROUNDFORCE"))) return has_force(p, enemy, space, params, out);
            if (k == (space ? std::string("HASNEARBYSPACEFORCE") : std::string("HASNEARBYGROUNDFORCE"))) {
                const std::string key = std::string(enemy ? "PF:E." : "PF:F.") + std::string(k);
                return leaf(key, static_cast<int32_t>(p), 11, params, [&](double& v) {
                    double x = 0.0;
                    if (!has_force(p, enemy, space, params, x)) return false;
                    bool any = x > 0.0;
                    for (const uint32_t n : r.neighbours_[p]) {
                        if (any) break;
                        if (!has_force(n, enemy, space, params, x)) return false;
                        any = x > 0.0;
                    }
                    v = b2d(any);
                    return true;
                }, out);
            }
            if (k == (space ? std::string("HASSPACEUNITSBITFIELD") : std::string("HASGROUNDUNITSBITFIELD"))) {
                const std::string key = std::string(enemy ? "PF:E." : "PF:F.") + std::string(k);
                return leaf(key, static_cast<int32_t>(p), 13, params, [&](double& v) {
                    const std::vector<std::string_view> names = param_texts(params, "Parameter_Type");
                    uint64_t bits = 0;
                    auto mark = [&](uint32_t t) {
                        for (size_t i = 0; i < names.size() && i < 52; ++i)
                            if (t < rules.types.size() && ai_iequal(rules.types[t].name, names[i])) bits |= uint64_t{1} << i;
                    };
                    const gal::PlanetState& ps = planets[p];
                    if (space) {
                        for (const uint32_t f : ps.orbit)
                            if (f < sim.fleets().size() && sim.fleets()[f].alive && side(enemy, sim.fleets()[f].owner))
                                for (const ObjectHandle u : sim.fleets()[f].contents)
                                    if (st.alive(u)) mark(st.type(u));
                    } else {
                        for (const ObjectHandle u : ps.surface)
                            if (st.alive(u) && side(enemy, st.owner(u))) mark(st.type(u));
                    }
                    v = static_cast<double>(bits);
                    return true;
                }, out);
            }
        }
        if (k == "MAINTENANCEUNNORMALIZED") return force_maintenance_unn(p, enemy, params, out);
        if (k == "MAINTENANCE") return force_maintenance(p, enemy, params, out);
        if (k == "STARBASETOTALUNNORMALIZED") {
            const gal::PlanetState& ps = planets[p];
            out = side(enemy, ps.owner) && ps.starbase && st.alive(ps.starbase) ? power(st.type(ps.starbase)) : 0.0;
            return true;
        }
        if (k == "GROUNDBASETOTALUNNORMALIZED") {
            double s = 0.0;
            if (side(enemy, planets[p].owner))
                for (const ObjectHandle u : planets[p].structures)
                    if (st.alive(u)) s += power(st.type(u));
            out = s;
            return true;
        }
        if (k == "ANTISTEALTHEFFECTIVENESS") {  // [port] per-unit value 0x395830 not modelled
            out = 0.0;
            return true;
        }
        return false;  // bare EnemyForce / FriendlyForce and unknown leaves are not registered
    }

    // ---------------- target (planet) ----------------
    int32_t political_player(size_t p) const { return planets[p].owner; }  // TargetPlayerID [M: owner = control record]
    bool has_structure(size_t p, std::span<const Param> params, bool count, double& out) const {
        if (!has_param(params, "Parameter_Type")) return false;
        const std::vector<std::string_view> names = param_texts(params, "Parameter_Type");
        double only_complete = 0.0;
        param_number(params, "Parameter_Only_Consider_Complete", only_complete);
        return leaf(count ? "T:STRUCTURECOUNT" : "T:HASSTRUCTURE", static_cast<int32_t>(p), count ? 2 : 13, params, [&](double& v) {
            const gal::PlanetState& ps = planets[p];
            int32_t n = 0;
            for (const ObjectHandle s : ps.structures) n += st.alive(s) && type_named(st.type(s), names) ? 1 : 0;
            if (ps.starbase && st.alive(ps.starbase) && type_named(st.type(ps.starbase), names)) ++n;
            if (only_complete != 1.0)
                for (const auto& q : ps.queue)
                    for (size_t i = 0; i < q.size(); ++i)
                        if ((count || i == 0) && type_named(q[i].type, names)) ++n;
            v = count ? static_cast<double>(n) : b2d(n > 0);
            return true;
        }, out);
    }
    bool has_base_of_level(size_t p, bool ground, std::span<const Param> params, double& out) const {
        double level = 1.0;
        param_number(params, "Parameter_Level", level);
        const auto l = static_cast<int32_t>(level + 0.5);
        if (l <= 0) return false;
        const int32_t have = ground ? sim.ground_base_level(static_cast<PlanetIndex>(p)) : planets[p].starbase_level;
        out = b2d(have >= l);
        return true;
    }
    bool is_faction(int32_t owner, std::span<const Param> params, double& out) const {
        if (!has_param(params, "Parameter_Faction")) return false;
        const uint32_t f = valid_player(owner) ? players[static_cast<size_t>(owner)].faction : gal::kNoIndex;
        bool is = false;
        for (const std::string_view n : param_texts(params, "Parameter_Faction"))
            is = is || (f < rules.factions.size() && ai_iequal(rules.factions[f].name, n));
        out = b2d(is);
        return true;
    }
    // Island analysis 0x4e4850 for Self [port: hop-count shortest paths]: components of Self's planets over the
    // routes, sizes, and the flags of the planets on the paths between islands.
    struct Islands {
        std::vector<int32_t> sizes;  // per island
        std::vector<uint8_t> connects, largest;
    };
    Islands islands(int32_t self) const {
        Islands is;
        const size_t n = planets.size();
        is.connects.assign(n, 0);
        is.largest.assign(n, 0);
        std::vector<int32_t> comp(n, -1);
        const gal::PlanetGraph& g = sim.graph();
        for (size_t i = 0; i < n; ++i) {
            if (comp[i] >= 0 || !planets[i].is_planet || planets[i].owner != self || !g.included(static_cast<PlanetIndex>(i))) continue;
            const auto c = static_cast<int32_t>(is.sizes.size());
            is.sizes.push_back(0);
            std::vector<PlanetIndex> stack{static_cast<PlanetIndex>(i)};
            comp[i] = c;
            while (!stack.empty()) {
                const PlanetIndex x = stack.back();
                stack.pop_back();
                ++is.sizes.back();
                for (uint32_t e = g.edge_begin(x); e < g.edge_end(x); ++e) {
                    const PlanetIndex y = g.neighbour(e);
                    if (y < n && comp[y] < 0 && planets[y].is_planet && planets[y].owner == self) {
                        comp[y] = c;
                        stack.push_back(y);
                    }
                }
            }
        }
        if (is.sizes.size() < 2) return is;
        // BFS from every planet of island a; the shortest path to the nearest planet of each other island.
        auto mark_path = [&](int32_t a, int32_t b, std::vector<uint8_t>& flags) {
            std::vector<int32_t> prev(n, -2);
            std::vector<PlanetIndex> q;
            for (size_t i = 0; i < n; ++i)
                if (comp[i] == a) {
                    prev[i] = -1;
                    q.push_back(static_cast<PlanetIndex>(i));
                }
            for (size_t h = 0; h < q.size(); ++h) {
                const PlanetIndex x = q[h];
                if (comp[x] == b) {
                    for (int32_t y = prev[x]; y >= 0 && comp[static_cast<size_t>(y)] != a; y = prev[static_cast<size_t>(y)])
                        flags[static_cast<size_t>(y)] = 1;
                    return;
                }
                for (uint32_t e = g.edge_begin(x); e < g.edge_end(x); ++e) {
                    const PlanetIndex y = g.neighbour(e);
                    if (y < n && prev[y] == -2 && planets[y].is_planet) {
                        prev[y] = static_cast<int32_t>(x);
                        q.push_back(y);
                    }
                }
            }
        };
        for (int32_t a = 0; a < static_cast<int32_t>(is.sizes.size()); ++a)
            for (int32_t b = 0; b < static_cast<int32_t>(is.sizes.size()); ++b)
                if (a != b) mark_path(a, b, is.connects);
        int32_t l1 = 0, l2 = -1;
        for (int32_t c = 1; c < static_cast<int32_t>(is.sizes.size()); ++c)
            if (is.sizes[static_cast<size_t>(c)] > is.sizes[static_cast<size_t>(l1)]) l1 = c;
        for (int32_t c = 0; c < static_cast<int32_t>(is.sizes.size()); ++c)
            if (c != l1 && (l2 < 0 || is.sizes[static_cast<size_t>(c)] > is.sizes[static_cast<size_t>(l2)])) l2 = c;
        if (l2 >= 0) mark_path(l1, l2, is.largest);
        return is;
    }

    // P4 ai: BlackMarketAbilitiesAvailable 0x65c280 / BlackMarketMinimumAbilityPrice 0x65e5e0 (AI_GALACTIC_TOKENS rows,
    // P4_SIM_SYSTEMS §1.7) [H unless marked]. Items = the black-market table (file order) offered on the planet: item
    // faction unset or the planet owner's faction ([inferred] the offer rule of P4 §1.3); "used" = purchased by the
    // Parameter_Faction player (the first player of that faction, index order [inferred]; none = nothing purchased).
    // Available: the planet carries Corruption_Black_Market (6) and some item is not purchased. Minimum price: min over
    // those items of Base_Cost x (Uncommon_Price_Multiplier when the item's faction lags its tech: P4 §1.3 single-player
    // rule) + Planet_Black_Market_Modifier; none -> 1e18 + modifier. [port] The tutorial costs are not used here.
    bool black_market(size_t p, std::span<const Param> params, bool available, double& v) const {
        uint32_t fac = gal::kNoIndex;
        for (const std::string_view n : param_texts(params, "Parameter_Faction")) {
            fac = rules.find_faction(n);
            if (fac != gal::kNoIndex) break;
        }
        if (fac == gal::kNoIndex) return false;  // faction parameter absent / unknown [H]
        int32_t buyer = -1;
        for (size_t i = 0; i < players.size() && buyer < 0; ++i)
            if (players[i].faction == fac) buyer = static_cast<int32_t>(i);
        const gal::PlanetState& ps = planets[p];
        const gal::SpecialRules& sp = rules.special;
        const uint32_t owner_fac = valid_player(ps.owner) ? players[static_cast<size_t>(ps.owner)].faction : gal::kNoIndex;
        const double modifier = static_cast<double>(sp.type(ps.type).black_market_modifier);
        constexpr int8_t kCorruptionBlackMarket = 6;  // Corruption_Black_Market
        bool any = false;
        double best = kNever;
        for (uint32_t i = 0; i < sp.items.size(); ++i) {
            const gal::BlackMarketItemDef& it = sp.items[i];
            if (it.faction != gal::kNoIndex && it.faction != owner_fac) continue;
            if (buyer >= 0 && sim.bm_has_item(buyer, i)) continue;
            any = true;
            float u = 1.0f;
            if (it.faction != gal::kNoIndex)
                for (size_t q = 0; q < players.size(); ++q)
                    if (players[q].faction == it.faction && sim.player_tech(static_cast<int32_t>(q)) < it.tech_level) {
                        u = it.uncommon_price_multiplier;
                        break;
                    }
            best = std::min(best, static_cast<double>(static_cast<float>(it.base_cost) * u));
        }
        if (available) {
            v = b2d(ps.corruption == kCorruptionBlackMarket && any);
            return true;
        }
        v = best + modifier;
        return true;
    }

    bool resolve_target(size_t p, std::string_view k, std::span<const Param> params, double& out) const {
        const gal::PlanetState& ps = planets[p];
        const auto ip = static_cast<int32_t>(p);
        if (k.substr(0, 11) == "ENEMYFORCE.") return resolve_force(p, true, k.substr(11), params, out);
        if (k.substr(0, 14) == "FRIENDLYFORCE.") return resolve_force(p, false, k.substr(14), params, out);
        if (k == "TARGETPOLITICALCONTROL") {
            const double v = static_cast<double>(static_cast<int32_t>(std::fabs(ps.control))) / kMaxLevel;
            out = political_player(p) != ctx.self ? -v : v;
            return true;
        }
        if (k == "TARGETPLAYERID") {
            out = political_player(p);
            return true;
        }
        if (k == "ISHUMANCONTROLLED") {
            out = b2d(valid_player(ps.owner) && players[static_cast<size_t>(ps.owner)].human);
            return true;
        }
        if (k == "STARBASELEVEL") return out = ps.starbase_level / kMaxLevel, true;
        if (k == "STARBASELEVELUNNORMALIZED") return out = ps.starbase_level, true;
        if (k == "GROUNDBASELEVEL") return out = sim.ground_base_level(static_cast<PlanetIndex>(p)) / kMaxLevel, true;
        if (k == "GROUNDBASELEVELUNNORMALIZED") return out = sim.ground_base_level(static_cast<PlanetIndex>(p)), true;
        if (k == "MAXSTARBASELEVEL") return out = max_space_base(p) / kMaxLevel, true;
        if (k == "MAXSTARBASELEVELUNNORMALIZED") return out = max_space_base(p), true;
        if (k == "MAXGROUNDBASELEVEL") return out = std::min(5, max_ground_base(p)) / kMaxLevel, true;
        if (k == "MAXGROUNDBASELEVELUNNORMALIZED") return out = std::min(5, max_ground_base(p)), true;
        // Structure slots: D+0x230 ground slots (Special_Structures_Land), used 0x3f8af0 (galactic_slots.hpp, A2 f t01)
        // [port: no orbital (D+0x234) slots].
        const int32_t used_ground = galactic_ai_ground_slots_used(sim, static_cast<PlanetIndex>(p));
        if (k == "MAXSTRUCTURESLOTS") return out = max_ground_base(p) / kMaxLevel, true;
        if (k == "MAXSTRUCTURESLOTSUNNORMALIZED") return out = max_ground_base(p), true;
        if (k == "OPENSTRUCTURESLOTS")
            return leaf("T:OPENSTRUCTURESLOTS", ip, 15, params, [&](double& v) {
                const int32_t total = max_ground_base(p);
                v = total <= 0 ? 0.0 : 1.0 - static_cast<double>(used_ground) / static_cast<double>(total);
                return true;
            }, out);
        if (k == "OPENSTRUCTURESLOTSUNNORMALIZED") return out = max_ground_base(p) - used_ground, true;
        if (k == "OPENGROUNDCOMPANYSLOTS")
            return leaf("T:OPENGROUNDCOMPANYSLOTS", ip, 13, params, [&](double& v) {
                v = static_cast<double>(rules.constants.max_ground_forces_on_planet) - static_cast<double>(ps.surface.size());
                return true;
            }, out);
        if (k == "BASEINCOME" || k == "BASEINCOMENBTP")
            return leaf(k == "BASEINCOME" ? "T:BASEINCOME" : "T:BASEINCOMENBTP", ip, 15, params, [&](double& v) {
                const int32_t group = political_player(p);
                double mx = 0.0;
                for (size_t i = 0; i < planets.size(); ++i)
                    if (planets[i].is_planet && (k == "BASEINCOME" || political_player(i) == group))
                        mx = std::max(mx, static_cast<double>(planets[i].base_value));
                v = mx <= 0.0 ? 0.0 : std::clamp(static_cast<double>(ps.base_value) / mx, 0.0, 1.0);
                return true;
            }, out);
        if (k == "INCOMEUNNORMALIZED")
            return leaf("T:INCOMEUNNORMALIZED", ip, 15, params, [&](double& v) {
                v = planet_income(p);
                return true;
            }, out);
        if (k == "TRADEROUTES")
            return leaf("T:TRADEROUTES", ip, kLifeForever, params, [&](double& v) {
                double mx = 0.0;
                for (const gal::PlanetState& q : planets)
                    if (q.is_planet) mx = std::max(mx, static_cast<double>(q.links.size()));
                v = ps.links.empty() || mx <= 0.0 ? 0.0 : static_cast<double>(ps.links.size()) / mx;
                return true;
            }, out);
        if (k == "ACTIVETRADEROUTES")
            return leaf("T:ACTIVETRADEROUTES", ip, 15, params, [&](double& v) {
                if (ps.links.empty()) {
                    v = 0.0;
                    return true;
                }
                int32_t friendly = 0;
                const GalaxyMap& m = sim.map();
                for (const uint32_t rt : ps.links) {
                    const PlanetIndex nb = m.routes.point_a[rt] == p ? m.routes.point_b[rt] : m.routes.point_a[rt];
                    const int32_t o = nb < planets.size() ? planets[nb].owner : -1;
                    if (valid_player(o) && valid_player(ps.owner) &&
                        (o == ps.owner || (!sim.is_hostile(o, ps.owner) && !neutral(o) && !neutral(ps.owner))))
                        ++friendly;
                }
                v = static_cast<double>(friendly) / static_cast<double>(ps.links.size());
                return true;
            }, out);
        if (k == "CONNECTSISOLATEDPLANETSBYFORCE" || k == "CONNECTSISOLATEDPLANETSBYLENGTH" || k == "CONNECTSLARGESTISLANDS") {
            const Islands is = islands(ctx.self);
            out = b2d(k == "CONNECTSLARGESTISLANDS" ? is.largest[p] != 0 : is.connects[p] != 0);
            return true;
        }
        if (k == "ISCONNECTEDTO")
            return leaf("T:ISCONNECTEDTO", ip, 5, params, [&](double& v) {
                const std::vector<std::string_view> types = param_texts(params, "Parameter_Type");
                const bool fac = has_param(params, "Parameter_Faction");
                if (types.empty() && !fac) {
                    v = 0.0;
                    return true;
                }
                double a = 0.0;
                param_number(params, "Parameter_Attenuator", a);
                double best = 0.0;
                for (const uint32_t n : r.neighbours_[p]) {
                    bool match = planets[n].type < rules.types.size() && type_named(planets[n].type, types);
                    if (!match && fac) {
                        double f = 0.0;
                        if (!is_faction(planets[n].owner, params, f)) return false;
                        match = f > 0.0;
                    }
                    if (!match) continue;
                    if (a <= 0.0) {
                        v = 1.0;
                        return true;
                    }
                    const double d = routed(p, n) ? 0.0 : distance(p, n);
                    best = std::max(best, 1.0 - d / kMaxFleetDistance * a);
                }
                v = best;
                return true;
            }, out);
        if (k == "ISFACTION" || k == "OWNER.ISFACTION") return is_faction(ps.owner, params, out);
        if (k == "HASSTARBASEOFLEVEL") return has_base_of_level(p, false, params, out);
        if (k == "HASGROUNDBASEOFLEVEL") return has_base_of_level(p, true, params, out);
        if (k == "HASSTRUCTURE") return has_structure(p, params, false, out);
        if (k == "STRUCTURECOUNT") return has_structure(p, params, true, out);
        if (k == "HASCREDITSIPHON") return out = b2d(ps.siphon_player >= 0), true;
        if (k == "ISSURFACEACCESSIBLE") return out = b2d(sim.surface_accessible(static_cast<PlanetIndex>(p))), true;  // 0x375220 (destroyed -> 0)
        if (k == "ISCORRUPTED")
            return leaf("T:ISCORRUPTED", ip, 3, params, [&](double& v) {
                v = b2d(ps.corruption != gal::kCorruptionNone);  // false leaves the output unwritten (0) [H]
                return true;
            }, out);
        if (k == "ISCONNECTEDTOCORRUPTION")
            return leaf("T:ISCONNECTEDTOCORRUPTION", ip, 4, params, [&](double& v) {
                bool any = false;
                for (const uint32_t n : r.neighbours_[p]) any = any || planets[n].corruption != gal::kCorruptionNone;
                v = b2d(any);
                return true;
            }, out);
        if (k == "ISCORRUPTIONTRANSITIONACTIVE")
            return leaf("T:ISCORRUPTIONTRANSITIONACTIVE", ip, 2, params, [&](double& v) {
                v = b2d(sim.corruption_transition_active(static_cast<PlanetIndex>(p)));  // D+0x2E2 gaining || +0x2E3 removing
                return true;
            }, out);
        if (k == "TIMESINCECORRUPTIONCHANGE")
            return out = static_cast<double>(r.frame_ - r.stamp_corruption_[p]) / kAiFps, true;
        // P4 ai: type byte +0xBA = Planet_Retains_Residual_Influence, a bool that defaults true (OBJECT_TYPES §4.6 [H]).
        if (k == "RETAINSRESIDUALINFLUENCE")
            return out = b2d(ps.type >= r.types_.size() || r.types_[ps.type].retains_residual_influence), true;
        if (k == "TIMESINCESPACECONFLICT" || k == "TIMESINCEGROUNDCONFLICT" || k == "TIMESINCECONVERSION") {
            const int32_t s = k == "TIMESINCESPACECONFLICT" ? r.stamp_space_[p]
                              : k == "TIMESINCEGROUNDCONFLICT" ? r.stamp_ground_[p] : r.stamp_conversion_[p];
            out = s < 0 ? kNever : static_cast<double>(r.frame_ - s) / kAiFps;
            return true;
        }
        if (k == "MARKUP") {
            out = r.hooks_.markup ? r.hooks_.markup(ctx.self, ip) : 0.0;
            return true;
        }
        if (k.substr(0, 6) == "HINTS.") {
            for (const std::string_view h : kGalacticHints)
                if (k.substr(6) == h) {
                    out = 0.0;  // hint-set default [port: Markup_Filename values not read]
                    return true;
                }
            return false;
        }
        if (k == "TYPE.TOKEN") return out = static_cast<double>(ps.type), true;  // [L] the type evaluator's token
        if (k == "BLACKMARKETABILITIESAVAILABLE" || k == "BLACKMARKETMINIMUMABILITYPRICE") {
            const bool avail = k == "BLACKMARKETABILITIESAVAILABLE";
            return leaf(avail ? "T:BLACKMARKETABILITIESAVAILABLE" : "T:BLACKMARKETMINIMUMABILITYPRICE", ip, avail ? 5 : 6, params,
                        [&](double& v) { return black_market(p, params, avail, v); }, out);
        }
        if (k == "BLACKMARKETPRICEMODIFIER")  // type float +0x800 (Planet_Black_Market_Modifier) [H]
            return leaf("T:BLACKMARKETPRICEMODIFIER", ip, -1, params, [&](double& v) {
                v = static_cast<double>(rules.special.type(ps.type).black_market_modifier);
                return true;
            }, out);
        return false;  // IsContestable, HasBuiltObject, Force, TimeLastSeen*, ... are not registered
    }

    // ---------------- players ----------------
    std::vector<size_t> owned(int32_t pl) const {
        std::vector<size_t> o;
        for (size_t i = 0; i < planets.size(); ++i)
            if (planets[i].is_planet && political_player(i) == pl) o.push_back(i);
        return o;
    }
    // Player-object walk incl. units in transit (0x2acb70): fleet members, surface units, starbases, structures.
    template <class F>
    void player_objects(int32_t pl, F&& fn) const {
        for (const gal::FleetState& f : sim.fleets())
            if (f.alive && f.owner == pl)
                for (const ObjectHandle u : f.contents)
                    if (st.alive(u)) fn(u);
        for (const gal::PlanetState& ps : planets) {
            for (const ObjectHandle u : ps.surface)
                if (st.alive(u) && st.owner(u) == pl) fn(u);
            if (ps.starbase && st.alive(ps.starbase) && st.owner(ps.starbase) == pl) fn(ps.starbase);
            for (const ObjectHandle s : ps.structures)
                if (st.alive(s) && st.owner(s) == pl) fn(s);
        }
    }
    double player_total(int32_t pl, bool space, uint64_t cat) const {
        double s = 0.0;
        player_objects(pl, [&](ObjectHandle u) {
            const uint32_t t = st.type(u);
            if (t >= rules.types.size()) return;
            const gal::UnitKind kind = rules.types[t].kind;
            // A galactic starbase is a type pointer on the planet (PD +0x140), not a GOM object, so 0x66b1c0's object
            // walk never reaches it: neither its power nor its members count (A2GAL_READINGS §2.1; A2H_T02_SPACE_TOTAL:
            // retail HST 21135 / SST 24250 fit exactly without starbases) [H].
            if (kind == gal::UnitKind::StarBase) return;
            if (space) {
                // DUMMY_STARSHIP / DUMMY_SPACE_FIGHTER_SQUADRON: power by +0x1648. Any other type (A2I_TOKENS_RE §2.4, the
                // 0x66b1c0 correction) [H]: power when its byte +0x1E (Is_Special_Weapon_In_Space) is set or it is a
                // DUMMY_ORBITAL_STRUCTURE, and `cat` meets its structure mask +0x1658 (Capital when 0). With the default
                // all-bits category the mask always passes; companies and plain ground structures are out because they
                // are neither. Vanilla: those types have no AI_Combat_Power, so the branch adds 0.
                if (space_kind(kind)) {
                    if ((type_mask(t) & cat) != 0) s += power(t);
                } else if ((t < r.types_.size() && r.types_[t].special_weapon_space) || gal::is_orbital_structure_type(rules.types[t])) {
                    if ((cat & galactic_ai_structure_mask(r.types_, t)) != 0) s += power(t);
                }
            } else if (ground_kind(kind) && (type_mask(t) & cat) != 0) {
                s += power(t);  // DUMMY_GROUND_COMPANY only
            }
            // "+ per-tech members (0x373fd0 / 0x373ee0) matching +0x1648" for every walked object, in both variants
            // (AI_GALACTIC_TOKENS §3; exact list, A2F_RE_READS §2.3 [H]).
            s += galactic_ai_member_force(sim, r.types_, t, pl, cat, true);
        });
        return s;
    }
    // Owned planets where the Target leaf `fn` is > 0; failures propagate.
    template <class F>
    bool count_owned(int32_t pl, F&& fn, int32_t& n) const {
        n = 0;
        for (const size_t p : owned(pl)) {
            double v = 0.0;
            if (!fn(p, v)) return false;
            if (v > 0.0) ++n;
        }
        return true;
    }
    // MaintenanceUnnormalized 0x669420 (A2H_GALACTIC_AI_RE §3) [H]: 0 for a neutral faction; for Self, over the player's
    // fleets (orbiting and in transit), every member by its type (a company by its company type): k = type
    // Maintenance_Cost x faction Maintenance_Cost (float); when k != 0 and btime(T, no planet) != 0, + (cost / btime / 30) x k
    // (float terms, double sum). Surface units, starbases and structures add nothing. Another (non-neutral) root player
    // (A2I_TOKENS_RE §1.1) [H]: Σ over the target list of Target.EnemyForce.MaintenanceUnnormalized (the planet leaf,
    // no parameters), relative to Self, so the value does not depend on the root; fleets in transit add nothing. A planet
    // failure fails.
    bool maintenance_unnormalized(int32_t pl, bool self, double& out) const {
        out = 0.0;
        if (!valid_player(pl) || neutral(pl)) return true;
        if (!self) {
            for (size_t p = 0; p < planets.size(); ++p) {
                if (!planets[p].is_planet) continue;
                double x = 0.0;
                if (!force_maintenance_unn(p, true, {}, x)) return false;
                out += x;
            }
            return true;
        }
        const uint32_t f = players[static_cast<size_t>(pl)].faction;
        const float fm = f < rules.factions.size() ? rules.factions[f].maintenance_cost : 0.0f;
        double s = 0.0;
        for (const gal::FleetState& fl : sim.fleets()) {
            if (!fl.alive || fl.owner != pl) continue;
            for (const ObjectHandle u : fl.contents) {
                if (!st.alive(u)) continue;
                const uint32_t t = st.type(u);
                if (t >= rules.types.size()) continue;
                const gal::TypeInfo& ti = rules.types[t];
                const float k = ti.maintenance_cost * fm;
                if (k == 0.0f) continue;
                const float bt = gal::econ::build_time_t0(ti.build_time_seconds, rules.constants.production_speed_factor);
                if (bt == 0.0f) continue;
                s += static_cast<double>(static_cast<float>(ti.build_cost) / bt / static_cast<float>(kAiFps) * k);
            }
        }
        out = s;
        return true;
    }
    double gross_income(int32_t pl) const {
        double s = 0.0;
        for (const size_t p : owned(pl)) s += planet_income(p);
        return s;
    }
    uint32_t next_tech_type(int32_t pl) const {
        const int32_t tech = players[static_cast<size_t>(pl)].tech_level;
        for (size_t t = 0; t < rules.types.size(); ++t) {
            const gal::TypeInfo& ti = rules.types[t];
            if (ti.advances_tech == 0 || ti.tech_level != tech) continue;
            if (!ti.affiliation.empty() &&
                std::find(ti.affiliation.begin(), ti.affiliation.end(), players[static_cast<size_t>(pl)].faction) == ti.affiliation.end())
                continue;
            return static_cast<uint32_t>(t);
        }
        return gal::kNoIndex;
    }

    bool resolve_player(int32_t pl, std::string_view k, std::span<const Param> params, double& out) const {
        if (!valid_player(pl)) return false;  // unbound root (no human, no opponent) [H]
        const gal::PlayerState& ps = players[static_cast<size_t>(pl)];
        const bool self = pl == ctx.self;
        if (k == "PLANETSCONTROLLEDUNNORMALIZED" || k == "PLANETSCONTROLLED")
            return leaf(std::string("P:") + std::string(k), pl, 3, params, [&](double& v) {
                v = static_cast<double>(owned(pl).size());
                if (k == "PLANETSCONTROLLED") {
                    size_t n = 0;
                    for (const gal::PlanetState& q : planets) n += q.is_planet ? 1u : 0u;
                    v = n ? v / static_cast<double>(n) : 0.0;
                }
                return true;
            }, out);
        if (k == "SPACETOTALUNNORMALIZED" || k == "GROUNDTOTALUNNORMALIZED")
            return leaf(std::string("P:") + std::string(k), pl, 10, params, [&](double& v) {
                v = player_total(pl, k == "SPACETOTALUNNORMALIZED", category_param(params));
                return true;
            }, out);
        if (k == "SPACEDEFENDEDPLANETS" || k == "GROUNDDEFENDEDPLANETS")
            return leaf(std::string("P:") + std::string(k), pl, 5, params, [&](double& v) {
                const std::vector<size_t> o = owned(pl);
                if (o.empty()) {
                    v = 0.0;
                    return true;
                }
                double s = 0.0;
                for (const size_t p : o) {
                    double x = 0.0;
                    if (!has_force(p, !self, k == "SPACEDEFENDEDPLANETS", {}, x)) return false;
                    s += x;
                }
                v = s / static_cast<double>(o.size());
                return true;
            }, out);
        if (k == "MAXEDSTARBASES" || k == "MAXEDGROUNDBASES")
            return leaf(std::string("P:") + std::string(k), pl, 5, params, [&](double& v) {
                const std::vector<size_t> o = owned(pl);
                if (o.empty()) {
                    v = 0.0;
                    return true;
                }
                int32_t maxed = 0;
                for (const size_t p : o) {
                    const bool ground = k == "MAXEDGROUNDBASES";
                    const int32_t mx = ground ? std::min(5, max_ground_base(p)) : max_space_base(p);
                    const int32_t lv = ground ? sim.ground_base_level(static_cast<PlanetIndex>(p)) : planets[p].starbase_level;
                    if (mx == 0 || lv == mx) ++maxed;
                }
                v = static_cast<double>(maxed) / static_cast<double>(o.size());
                return true;
            }, out);
        if (k == "MAXSTARBASELEVEL" || k == "MAXSTARBASELEVELUNNORMALIZED" || k == "MAXGROUNDBASELEVEL" ||
            k == "MAXGROUNDBASELEVELUNNORMALIZED")
            return leaf(std::string("P:") + std::string(k), pl, 4, params, [&](double& v) {
                const bool ground = k.substr(0, 9) == "MAXGROUND";
                int32_t mx = 0;
                for (const size_t p : owned(pl))
                    mx = std::max(mx, ground ? sim.ground_base_level(static_cast<PlanetIndex>(p)) : planets[p].starbase_level);
                v = k.find("UNNORMALIZED") != std::string_view::npos ? mx : mx / kMaxLevel;
                return true;
            }, out);
        if (k == "NUMSTARBASEOFLEVEL" || k == "NUMGROUNDBASEOFLEVEL" || k == "NUMSTRUCTURE" || k == "HASSTARBASEOFLEVEL" ||
            k == "HASGROUNDBASEOFLEVEL" || k == "HASSTRUCTURE")
            return leaf(std::string("P:") + std::string(k), pl, 3, params, [&](double& v) {
                int32_t n = 0;
                const bool structure = k == "NUMSTRUCTURE" || k == "HASSTRUCTURE";
                const bool ground = k.find("GROUND") != std::string_view::npos;
                const bool ok = count_owned(pl, [&](size_t p, double& x) {
                    return structure ? has_structure(p, params, false, x) : has_base_of_level(p, ground, params, x);
                }, n);
                if (!ok) return false;
                double count = 1.0;
                param_number(params, "Parameter_Count", count);
                const int32_t num = std::max(0, n - static_cast<int32_t>(count + 0.5) + 1);
                v = k.substr(0, 3) == "NUM" ? static_cast<double>(num) : b2d(num > 0);
                return true;
            }, out);
        if (k == "STRUCTURECOUNT")
            return leaf("P:STRUCTURECOUNT", pl, 4, params, [&](double& v) {
                v = 0.0;
                for (const size_t p : owned(pl)) {
                    double x = 0.0;
                    if (!has_structure(p, params, true, x)) return false;
                    v += x;
                }
                return true;
            }, out);
        if (k == "WORSTISLANDFRACTIONOFLARGEST") {
            if (!self) return out = 0.0, true;
            Islands is = islands(pl);
            if (is.sizes.size() < 2) return out = 0.0, true;
            std::sort(is.sizes.begin(), is.sizes.end(), std::greater<int32_t>());
            return out = static_cast<double>(is.sizes[1]) / static_cast<double>(is.sizes[0]), true;
        }
        if (k == "ANYCURRENTTHREATS")
            return leaf("P:ANYCURRENTTHREATS", pl, 15, params, [&](double& v) {
                v = 0.0;
                if (ps.human) return true;
                for (size_t p = 0; p < planets.size() && v == 0.0; ++p) {
                    const gal::PlanetState& q = planets[p];
                    if (!q.is_planet || !valid_player(q.owner) || !players[static_cast<size_t>(q.owner)].human) continue;
                    bool human_orbit = false;
                    for (const uint32_t f : q.orbit)
                        human_orbit = human_orbit || (f < sim.fleets().size() && sim.fleets()[f].alive &&
                                                      valid_player(sim.fleets()[f].owner) &&
                                                      players[static_cast<size_t>(sim.fleets()[f].owner)].human);
                    if (!human_orbit) continue;
                    for (const uint32_t n : r.neighbours_[p])
                        if (planets[n].owner == pl) v = 1.0;
                }
                return true;
            }, out);
        if (k == "AVERAGEAGEOFSPACEINTELLIGENCE" || k == "AVERAGEAGEOFGROUNDINTELLIGENCE") {
            if (!self) return false;  // Self only [H]
            return out = 0.0, true;   // fog off: TimeLastSeen = 0
        }
        if (k == "BUDGETFRACTIONTOBUILD")  // 0x667120 (A2H_GALACTIC_AI_RE §2) [H]
            return leaf("P:BUDGETFRACTIONTOBUILD", pl, 5, params, [&](double& v) {
                if (!self) {  // step 1: another root player -> 0
                    v = 0.0;
                    return true;
                }
                if (!r.hooks_.budget) return false;  // no goal system
                // Step 2: Parameter_Type (absent / unknown: fail); Parameter_Category default -1 = every category.
                const std::vector<std::string_view> names = param_texts(params, "Parameter_Type");
                const uint32_t t = names.empty() ? gal::kNoIndex : rules.find_type(names[0]);
                if (t == gal::kNoIndex) return false;
                const std::vector<std::string_view> cats = param_texts(params, "Parameter_Category");
                float rate = 0.0f, pool = 0.0f;
                if (!r.hooks_.budget(pl, cats.empty() ? std::string_view{} : cats[0], rate, pool)) return false;
                // Step 3: R = cost and T = btime without a planet (T0, GALACTIC_ECONOMY §2.3); queue q as the planet queues.
                const gal::TypeInfo& ti = rules.types[t];
                const float cost = static_cast<float>(ti.build_cost);
                float R = cost;
                float T = gal::econ::build_time_t0(ti.build_time_seconds, rules.constants.production_speed_factor);
                const uint32_t q = ti.build_cost >= 1 && sim.queue_kind(t) == gal::kGroundQueue ? 0u : 1u;
                bool inprog = false;
                // Step 4: the head entry of queue q of every target-list planet, in list order.
                for (size_t p = 0; p < planets.size(); ++p) {
                    if (!planets[p].is_planet) continue;
                    const std::vector<gal::BuildEntry>& queue = planets[p].queue[q];
                    if (queue.empty()) continue;
                    const gal::BuildEntry& e = queue.front();
                    if (e.type != t || e.player != pl) continue;
                    // p = clamp(1 - remaining (cached at the last advance) / T, 0, 1) [H]; [port] a 0 build time is done.
                    const float prog = e.build_time > 0.0f ? std::clamp(1.0f - e.remaining / e.build_time, 0.0f, 1.0f) : 1.0f;
                    const int64_t left = std::max<int64_t>(0, static_cast<int64_t>(e.start) + e.duration - sim.frame());
                    const float rem = static_cast<float>(left) / static_cast<float>(kAiFps);  // live timer (0x4ad310)
                    const QueueTask task = r.hooks_.queue_task ? r.hooks_.queue_task(pl, static_cast<int32_t>(p), q) : QueueTask{};
                    if (task.kind == QueueTask::Kind::None || task.kind == QueueTask::Kind::EndedReclaimed) {
                        v = 0.0;  // the whole token returns 0 at once [H]
                        return true;
                    }
                    if (task.kind == QueueTask::Kind::Ended) continue;
                    R = std::min(R, (1.0f - prog) * cost);
                    R = std::min(R, task.alloc);                    // 0x6928c0 (0 without a budget)
                    R = R - std::min(task.alloc, task.a + task.c);  // 0x692ca0: the goal's "have"
                    T = std::min(T, rem);
                    T = std::min(T, std::max(task.h, 0.0f));        // 0x692840 (0 without a budget)
                    inprog = true;
                }
                // Steps 5-6: D = (double)(T x rate) (+ pool when nothing is in progress); R / D, 1e18f for D <= 0.
                double D = static_cast<double>(T * rate);
                if (!inprog) D += static_cast<double>(pool);
                if (R < 0.0001f) v = 0.0;
                else if (D > 0.0) v = static_cast<double>(R) / D;
                else v = static_cast<double>(1.0e18f);
                return true;
            }, out);
        if (k == "CANADVANCETECH")
            return leaf("P:CANADVANCETECH", pl, 6, params, [&](double& v) {
                const uint32_t t = next_tech_type(pl);
                v = 0.0;
                if (t == gal::kNoIndex) return true;
                for (size_t p = 0; p < planets.size() && v == 0.0; ++p) {
                    if (!planets[p].is_planet) continue;
                    for (const auto& q : planets[p].queue)
                        for (size_t i = 0; i < q.size() && i < 2; ++i)
                            if (q[i].type == t && q[i].player == pl) v = 1.0;
                    if (galactic_ai_can_build(sim, pl, t, static_cast<PlanetIndex>(p))) v = 1.0;
                }
                return true;
            }, out);
        if (k == "HASTECHTOPRODUCE")
            return leaf("P:HASTECHTOPRODUCE", pl, 8, params, [&](double& v) {
                if (!has_param(params, "Parameter_Type")) return false;
                bool any = false;
                for (const std::string_view n : param_texts(params, "Parameter_Type")) {
                    const uint32_t t = rules.find_type(n);
                    if (t == gal::kNoIndex) continue;
                    const gal::TypeInfo& ti = rules.types[t];
                    const bool aff = ti.affiliation.empty() ||
                                     std::find(ti.affiliation.begin(), ti.affiliation.end(), ps.faction) != ti.affiliation.end();
                    any = any || (aff && ti.tech_level <= ps.tech_level);
                }
                v = b2d(any);
                return true;
            }, out);
        if (k == "GROSSINCOMEUNNORMALIZED" || k == "NETINCOMEUNNORMALIZED")
            return leaf(std::string("P:") + std::string(k), pl, 7, params, [&](double& v) {
                v = gross_income(pl);  // credits per income cycle (0x667fb0) [H]
                if (k == "NETINCOMEUNNORMALIZED") {  // 0x66a420: Gross - Unn (a non-Self root: A2I_TOKENS_RE §1.1) [H]
                    double m = 0.0;
                    if (!maintenance_unnormalized(pl, self, m)) return false;
                    v -= m;
                }
                return true;
            }, out);
        // 0x66a730(numerator, denominator) (A2I_TOKENS_RE §1.4) [H]: both paths on the root with no parameters; either
        // failing fails; den = 0: 0 when num = 0, else fail; otherwise num / den, no clamp (a negative net stays negative).
        if (k == "GROSSINCOMENBPTM" || k == "GROSSINCOMENBTF" || k == "NETINCOMENBPTM" || k == "NETINCOMENBTF")
            return leaf(std::string("P:") + std::string(k), pl, 7, params, [&](double& v) {
                double num = 0.0, den = 0.0;
                if (!resolve_player(pl, k.substr(0, 3) == "NET" ? "NETINCOMEUNNORMALIZED" : "GROSSINCOMEUNNORMALIZED", {}, num)) return false;
                const bool all = k.find("NBPTM") != std::string_view::npos;
                if (!resolve_player(pl, all ? "MAXALLPLANETSINCOMEUNNORMALIZED" : "MAXOWNEDPLANETSINCOMEUNNORMALIZED", {}, den)) return false;
                if (den == 0.0) {
                    if (num != 0.0) return false;
                    v = 0.0;
                    return true;
                }
                v = num / den;
                return true;
            }, out);
        // MaxAllPlanetsIncomeUnnormalized 0x6698f0 [H]: Σ potential income over every target-list planet (each float term
        // cast to double), lifetime -1 (computed once, kept for the game). MaxOwnedPlanetsIncomeUnnormalized 0x669ca0 [H]:
        // the same over the planets whose TargetPlayerID (truncated to int) is the root's id; lifetime 7.
        if (k == "MAXALLPLANETSINCOMEUNNORMALIZED" || k == "MAXOWNEDPLANETSINCOMEUNNORMALIZED") {
            const bool all = k == "MAXALLPLANETSINCOMEUNNORMALIZED";
            return leaf(std::string("P:") + std::string(k), pl, all ? kLifeForever : 7, params, [&](double& v) {
                v = 0.0;
                for (size_t p = 0; p < planets.size(); ++p) {
                    if (!planets[p].is_planet) continue;
                    if (!all) {
                        double id = 0.0;
                        if (!resolve_target(p, "TARGETPLAYERID", {}, id)) return false;
                        if (static_cast<int32_t>(id) != pl) continue;
                    }
                    v += potential_income(p);
                }
                return true;
            }, out);
        }
        if (k == "MAINTENANCE" || k == "MAINTENANCEUNNORMALIZED")  // 0x669110 / 0x669420 (A2H_GALACTIC_AI_RE §3) [H]
            return leaf(std::string("P:") + std::string(k), pl, 7, params, [&](double& v) {
                if (!maintenance_unnormalized(pl, self, v)) return false;
                if (k == "MAINTENANCE") {  // Unn / GrossIncomeUnnormalized, 0 when the gross is 0
                    const double g = gross_income(pl);
                    v = g == 0.0 ? 0.0 : v / g;
                }
                return true;
            }, out);
        if (k == "TECHLEVEL") return out = ps.tech_level, true;
        if (k == "ISFACTION") return is_faction(pl, params, out);
        if (k == "ISDIFFICULTY") {
            if (!has_param(params, "Parameter_Difficulty_Level_Type")) return false;
            const int32_t d = ps.human || !r.hooks_.difficulty ? -1 : r.hooks_.difficulty(pl);
            if (d < 0) return out = 0.0, true;
            static constexpr std::string_view kNames[] = {"EASY", "NORMAL", "HARD"};
            bool is = false;
            for (const std::string_view n : param_texts(params, "Parameter_Difficulty_Level_Type"))
                is = is || (d < 3 && ai_iequal(kNames[d], n));
            return out = b2d(is), true;
        }
        if (k == "CREDITSUNNORMALIZED") {
            double c = 0.0;
            if (!ps.human && r.hooks_.ai_credits && r.hooks_.ai_credits(pl, c)) return out = c, true;
            return out = ps.account.credits, true;
        }
        if (k == "HASUNIT")
            return leaf("P:HASUNIT", pl, 8, params, [&](double& v) {
                if (!has_param(params, "Parameter_Type")) return false;
                const std::vector<std::string_view> names = param_texts(params, "Parameter_Type");
                double count = 1.0, complete = 0.0;
                param_number(params, "Parameter_Count", count);
                param_number(params, "Parameter_Only_Consider_Complete", complete);
                auto n = static_cast<int32_t>(count);
                // q2f t01 [H] (STRATEGIC_GAPS §1.7, A1_READINGS §29): the original's walk sees a company's transport (its
                // 0x3973b0 type = the company type) and each Company_Units member as its own object; the port keeps one
                // object per company, so its members are counted from the company type.
                player_objects(pl, [&](ObjectHandle u) {
                    const uint32_t t = st.type(u);
                    n -= type_named(t, names) ? 1 : 0;
                    if (t < rules.types.size())
                        for (const uint32_t m : rules.types[t].company_units) n -= type_named(m, names) ? 1 : 0;
                });
                if (!(complete > 0.0))
                    for (const gal::PlanetState& q : planets)
                        for (const auto& queue : q.queue)
                            for (const gal::BuildEntry& e : queue) n -= e.player == pl && type_named(e.type, names) ? 1 : 0;
                v = b2d(n <= 0);
                return true;
            }, out);
        if (k == "ACTIVEGOALS") {  // 0x6e0490 (A2H_GALACTIC_AI_RE §1) [H]
            // Exactly one Parameter_Category holding the category value: none, or a repeated parameter (a list value),
            // fails the token (0x5f4780 has no default) [H / M].
            const Param* cat = nullptr;
            int32_t n_cat = 0;
            for (const Param& p : params)
                if (ai_iequal(p.name, "Parameter_Category")) {
                    cat = &p;
                    ++n_cat;
                }
            if (n_cat != 1 || !r.hooks_.active_goals) return false;
            // One value: an AIGoalCategoryType literal carries exactly one name (A2I_TOKENS_RE §3.3).
            const std::vector<std::string_view> names = param_texts(std::span<const Param>(cat, 1), "Parameter_Category");
            if (names.size() != 1) return false;
            int32_t n = 0;
            if (!r.hooks_.active_goals(pl, names[0], n)) return false;
            return out = static_cast<double>(n), true;
        }
        if (k == "STORYTRIGGER") {
            const std::vector<std::string_view> n = param_texts(params, "Parameter_Name");
            if (n.empty()) return false;
            return out = b2d(r.hooks_.story_flag && r.hooks_.story_flag(pl, n[0])), true;
        }
        return false;  // HomePlanet.*, BudgetAllocation, ... (not modelled)
    }

    // ---------------- game ----------------
    bool resolve_game(std::string_view k, std::span<const Param> params, double& out) const {
        if (k == "AGE") return out = static_cast<double>(sim.frame()) / kAiFps, true;
        if (k == "TIMELINE") return out = 0.0, true;  // mode +0x9C [port: unknown, 0]
        if (k == "ISSETUPPHASE" || k == "ISLANDCONTROLGAME") return out = 0.0, true;
        if (k == "ISCAMPAIGNGAME") return out = 1.0, true;  // galactic mode
        if (k == "ISSTORYCAMPAIGN") return out = b2d(r.hooks_.story_campaign), true;
        if (k == "PLANETSCORRUPTED" || k == "PLANETSCORRUPTEDUNNORMALIZED")
            return leaf(std::string("G:") + std::string(k), -1, 7, params, [&](double& v) {
                int32_t n = 0, total = 0;
                for (const gal::PlanetState& ps : planets) {
                    if (!ps.is_planet) continue;
                    ++total;
                    n += ps.corruption != gal::kCorruptionNone ? 1 : 0;
                }
                v = k == "PLANETSCORRUPTED" ? (total ? static_cast<double>(n) / total : 0.0) : static_cast<double>(n);
                return true;
            }, out);
        for (const bool enemy : {true, false}) {
            const std::string_view pre = enemy ? "ENEMYFORCE." : "FRIENDLYFORCE.";
            if (k.substr(0, pre.size()) != pre) continue;
            const std::string_view leaf_name = k.substr(pre.size());
            if (!valid_player(ctx.self)) return false;
            if (leaf_name == "SPACETOTALUNNORMALIZED") return game_force(enemy, true, false, out);
            if (leaf_name == "GROUNDTOTALUNNORMALIZED") return game_force(enemy, false, false, out);
            if (leaf_name == "PERTARGETMAXSPACEFORCE") return game_force(enemy, true, true, out);
            if (leaf_name == "PERTARGETMAXGROUNDFORCE") return game_force(enemy, false, true, out);
            return false;
        }
        return false;  // ForceVisibility is not registered on the galactic Game [H]
    }
};

bool GalacticSimTokenResolver::resolve(const EvalContext& ctx, const TokenRef& token, double& out) const {
    if (!token.tokens) return false;
    std::string key;
    for (const TokenId id : token.path) {
        if (!key.empty()) key += '.';
        key += ai_upper_copy(token.tokens->name(id));
    }
    if (key.empty()) return false;
    switch (token.root) {
        case Root::Self:
        case Root::Human:
        case Root::Enemy: {
            const int32_t p = token.root == Root::Self ? ctx.self : token.root == Root::Human ? ctx.human : ctx.enemy;
            const Impl x(*this, ctx);
            return x.resolve_player(p, key, token.params, out);
        }
        case Root::Target:
        case Root::Location: {
            EvalContext c = ctx;
            if (token.root == Root::Location) c.target = ctx.location >= 0 ? ctx.location : ctx.target;
            const Impl x(*this, c);
            if (!x.valid_planet(c.target)) return false;
            return x.resolve_target(static_cast<size_t>(c.target), key, token.params, out);
        }
        case Root::Game: {
            const Impl x(*this, ctx);
            return x.resolve_game(key, token.params, out);
        }
    }
    return false;
}

}  // namespace alamo::ai

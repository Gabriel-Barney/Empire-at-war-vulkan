// docs/re/GALACTIC_REVEAL_CORRUPTION.md (GRC) §2 on GalacticSim (declared in campaign.hpp): planet visibility masks.
// The per-(planet, player) modifier lists PD +0x1B8 (stored entries: Ground_Activated spies, story
// SET_PLANET_VISIBILITY_LEVEL, host adds; upsert by key, expiry in the planet service), the OR fold, and the derived
// sources (baseline 0x3fa490, presence 0x3fd080, always-on System_Spy_Ability).
//
// Determinism: serial code; lists are vectors walked in list order, planets in row order, players in index order; keys
// are ObjectIds and CRCs, never pointers; no RNG; the only float math is the expiry frame count ((float)FPS x seconds
// + 0.5f, no FMA) and the influence-range test (float squares, as launch_reveal).
#include <algorithm>
#include <cstdlib>

#include "alamo/core/text.hpp"
#include "alamo/galactic/campaign.hpp"

namespace alamo::galactic {

namespace {

struct LevelName {
    const char* name;
    uint32_t bits;
};
constexpr LevelName kLevelNames[] = {
    {"Planet_Ownership", 0x1},          {"Starbase_Level", 0x2},
    {"Ground_Base_Level", 0x4},         {"Credit_Income", 0x8},
    {"Fleet_Presence", 0x10},           {"Num_Fleets", 0x20},
    {"Fleet_Contents", 0x40},           {"Num_Ground_Companies", 0x80},
    {"Ground_Company_Contents", 0x100}, {"Special_Structures", 0x200},
    {"Political_Control", 0x400},       {"Political_Control_Breakdown", 0x800},
    {"Object_Under_Production", 0x1000}, {"Enemy_Minor_Stealth_Heroes", 0x2000},
    {"Enemy_Major_Stealth_Heroes", 0x4000}, {"Credit_Income_Breakdown", 0x8000},
    {"Most_Powerful_Ship", 0x10000},    {"Force_Sensitive_Heroes", 0x20000},
    {"Super_Weapons", 0x40000},         {"Minor_Stealth_Heroes", 0x2000},
    {"Major_Stealth_Heroes", 0x4000},   {"Evil_Default", kVisEvilDefault},
    {"Good_Default", kVisGoodDefault},  {"Planet_Owner", kVisPlanetOwner},
    {"Maximum", kVisMaximum},
};

bool parse_token(std::string_view t, uint32_t* out) {
    for (const LevelName& n : kLevelNames)
        if (ascii_iequals(t, n.name)) {
            *out = n.bits;
            return true;
        }
    // A plain integer (decimal or 0x hex).
    if (t.empty()) return false;
    const bool hex = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X');
    uint64_t v = 0;
    for (size_t i = hex ? 2 : 0; i < t.size(); ++i) {
        const char c = ascii_lower(t[i]);
        uint32_t d = 0;
        if (c >= '0' && c <= '9') d = static_cast<uint32_t>(c - '0');
        else if (hex && c >= 'a' && c <= 'f') d = static_cast<uint32_t>(c - 'a' + 10);
        else return false;
        v = v * (hex ? 16u : 10u) + d;
        if (v > 0xFFFFFFFFull) return false;
    }
    *out = static_cast<uint32_t>(v);
    return true;
}

}  // namespace

bool parse_visibility_level(std::string_view text, uint32_t* out) {
    uint32_t acc = 0;
    bool any = false;
    size_t i = 0;
    while (i < text.size()) {
        const auto sep = [](char c) { return c == '|' || c == ',' || c == '+' || c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
        while (i < text.size() && sep(text[i])) ++i;
        size_t j = i;
        while (j < text.size() && !sep(text[j])) ++j;
        if (j > i) {
            uint32_t bits = 0;
            if (!parse_token(text.substr(i, j - i), &bits)) return false;
            acc |= bits;
            any = true;
        }
        i = j;
    }
    if (!any) return false;
    *out = acc;
    return true;
}

uint32_t faction_default_visibility(std::string_view faction_name) {
    // GRC 2.3 baseline: "Rebel" 0x11, "Empire" 0, "Underworld" 0x11, other 0.
    if (ascii_iequals(faction_name, "Rebel") || ascii_iequals(faction_name, "Underworld")) return kVisGoodDefault;
    return kVisEvilDefault;
}

// ---- derived sources --------------------------------------------------------------------------------------------------

uint32_t GalacticSim::visibility_baseline(PlanetIndex planet, int32_t player) const {
    // 0x3fa490: the local player only. Planet_Owner when the planet's owner is the local player, else the local
    // faction's default by name.
    if (player != local_player_ || !valid_player(player)) return 0;
    if (planets_[planet].owner == player) return kVisPlanetOwner;
    return faction_default_visibility(faction_of(player).name);
}

uint32_t GalacticSim::visibility_presence(PlanetIndex planet, int32_t player) const {
    // 0x3fd080: players with an AI system (+0x360) only.
    if (!valid_player(player) || !players_[static_cast<size_t>(player)].ai) return 0;
    const PlanetState& p = planets_[planet];
    if (p.owner == player) return kVisMaximum;  // no entry, signalled level -1 (all) [port: folded as Maximum]
    uint32_t v = faction_default_visibility(faction_of(player).name);
    for (uint32_t f : p.orbit)
        if (fleets_[f].alive && fleets_[f].owner == player) {
            v |= kVisPresenceOrbit;
            break;
        }
    for (ObjectHandle u : p.surface)
        if (store_->alive(u) && store_->owner(u) == player) {
            v |= kVisPresenceGround;
            break;
        }
    return v;
}

uint32_t GalacticSim::visibility_auto_spy(PlanetIndex planet, int32_t player) const {
    // Always-on spies (Galactic_Automatic System_Spy_Ability, duration fixed to -1) [port: derived from where the
    // player's objects are; the original adds an entry on activation / arrival / ownership change (0x70f4a0 / 0x70f950)
    // and removes it on deactivation (0x70fb20) [M triggers]].
    if (!valid_player(player)) return 0;
    const Vec3 c = planets_[planet].position;
    uint32_t acc = 0;
    const auto consider = [&](uint32_t type, PlanetIndex at) {
        const uint32_t m = special().type(type).auto_spy_mask;
        if (m == 0 || (acc | m) == acc) return;
        if (at == planet) {
            acc |= m;
            return;
        }
        if (type >= rules_->types.size()) return;
        float r = rules_->types[type].influence_range;
        for (const uint32_t cu : rules_->types[type].company_units)
            if (cu < rules_->types.size()) r = std::max(r, rules_->types[cu].influence_range);
        if (!(r > 0.0f)) return;
        const Vec3 q = planets_[at].position;
        const float dx = q.x - c.x, dy = q.y - c.y, dz = q.z - c.z;
        if (dx * dx + dy * dy + dz * dz <= r * r) acc |= m;
    };
    const auto obj = [&](ObjectHandle h, PlanetIndex at) {
        if (store_->alive(h) && store_->owner(h) == player) consider(static_cast<uint32_t>(store_->type(h)), at);
    };
    for (PlanetIndex q = 0; q < planets_.size(); ++q) {
        const PlanetState& d = planets_[q];
        if (!d.is_planet) continue;
        if (d.owner == player) consider(d.type, q);
        for (uint32_t f : d.orbit) {
            const FleetState& fl = fleets_[f];
            if (!fl.alive) continue;
            for (ObjectHandle m : fl.contents) obj(m, q);
        }
        for (ObjectHandle u : d.surface) obj(u, q);
        for (ObjectHandle s : d.structures) obj(s, q);
        obj(d.starbase, q);
    }
    return acc;
}

// ---- fold and accessors -----------------------------------------------------------------------------------------------

GalacticSim::VisibilitySources GalacticSim::visibility_sources(PlanetIndex planet, int32_t player) const {
    VisibilitySources s;
    if (!valid_planet(planet) || !valid_player(player)) return s;
    s.baseline = visibility_baseline(planet, player);
    s.presence = visibility_presence(planet, player);
    s.auto_spy = visibility_auto_spy(planet, player);
    for (const VisibilityModifier& m : visibility_modifiers(planet, player)) s.stored |= m.value;  // BitwiseOr, list order
    return s;
}

uint32_t GalacticSim::visibility_mask(PlanetIndex planet, int32_t player) const {
    return visibility_sources(planet, player).mask();
}

std::span<const VisibilityModifier> GalacticSim::visibility_modifiers(PlanetIndex planet, int32_t player) const {
    if (!valid_planet(planet) || !valid_player(player)) return {};
    const auto& lists = planets_[planet].visibility;
    const size_t i = static_cast<size_t>(player);
    if (i >= lists.size()) return {};
    return lists[i];
}

uint32_t GalacticSim::visibility_expiry_frame(float seconds) const {
    if (seconds < 0.0f) return kVisNever;
    const float frames = static_cast<float>(econ::kFramesPerSecond) * seconds;
    return static_cast<uint32_t>(frame_) + static_cast<uint32_t>(static_cast<int32_t>(frames + 0.5f));
}

bool GalacticSim::add_visibility_modifier(PlanetIndex planet, int32_t player, const VisibilityKey& key, uint32_t value,
                                          float seconds, uint32_t name_hash) {
    if (!valid_planet(planet) || !valid_player(player)) return false;
    auto& lists = planets_[planet].visibility;
    if (lists.size() < players_.size()) lists.resize(players_.size());
    std::vector<VisibilityModifier>& l = lists[static_cast<size_t>(player)];
    VisibilityModifier e;
    e.value = value;
    e.expiry = visibility_expiry_frame(seconds);
    e.name_hash = name_hash;
    e.key = key;
    for (VisibilityModifier& m : l)
        if (m.key == key) {
            m = e;
            return true;
        }
    l.push_back(e);
    return true;
}

bool GalacticSim::remove_visibility_modifier(PlanetIndex planet, int32_t player, const VisibilityKey& key) {
    if (!valid_planet(planet) || !valid_player(player)) return false;
    auto& lists = planets_[planet].visibility;
    const size_t i = static_cast<size_t>(player);
    if (i >= lists.size()) return false;
    std::vector<VisibilityModifier>& l = lists[i];
    for (size_t k = 0; k < l.size(); ++k)
        if (l[k].key == key) {
            l.erase(l.begin() + static_cast<std::ptrdiff_t>(k));
            return true;
        }
    return false;
}

void GalacticSim::expire_visibility(PlanetIndex planet) {
    // 0x285e40: entries whose expiry < the store frame leave the list (the refresh that follows is the fold, which the
    // port does on query).
    const uint32_t now = static_cast<uint32_t>(frame_);
    for (std::vector<VisibilityModifier>& l : planets_[planet].visibility)
        l.erase(std::remove_if(l.begin(), l.end(), [now](const VisibilityModifier& m) { return m.expiry < now; }), l.end());
}

// ---- story and spies --------------------------------------------------------------------------------------------------

bool GalacticSim::story_set_planet_visibility(PlanetIndex planet, std::string_view faction, std::string_view level,
                                              std::string_view key, float seconds) {
    if (!valid_planet(planet)) return false;
    const int32_t player = player_of_faction(faction);
    if (!valid_player(player)) return false;
    const uint32_t h = special_name_crc(key);
    VisibilityKey k;
    k.object = store_->exists(planets_[planet].object) ? static_cast<int32_t>(store_->id(planets_[planet].object)) : -1;
    k.sub = h;
    std::string_view lv = level;
    while (!lv.empty() && (lv.front() == ' ' || lv.front() == '\t')) lv.remove_prefix(1);
    while (!lv.empty() && (lv.back() == ' ' || lv.back() == '\t' || lv.back() == '\r' || lv.back() == '\n')) lv.remove_suffix(1);
    if (ascii_iequals(lv, "none")) return remove_visibility_modifier(planet, player, k);  // 0x3fdbc0
    uint32_t mask = 0;
    if (!parse_visibility_level(lv, &mask)) return false;
    return add_visibility_modifier(planet, player, k, mask, seconds, h);  // 0x3feb90
}

bool GalacticSim::apply_system_spy(const SpecialAbilityDef& a, ObjectHandle unit, int32_t owner, PlanetIndex planet) {
    // SystemSpyAbilityClass apply 0x70f510 (Ground_Activated): an entry keyed by the ability on every target planet.
    if (!valid_player(owner) || !valid_planet(planet) || !store_->exists(unit)) return false;
    VisibilityKey k;
    k.object = static_cast<int32_t>(store_->id(unit));
    k.sub = a.name_crc;
    const uint32_t type = static_cast<uint32_t>(store_->type(unit));
    float r = 0.0f;
    if (type < rules_->types.size()) {
        r = rules_->types[type].influence_range;
        for (const uint32_t cu : rules_->types[type].company_units)
            if (cu < rules_->types.size()) r = std::max(r, rules_->types[cu].influence_range);
    }
    if (!(r > 0.0f)) return add_visibility_modifier(planet, owner, k, a.spy_mask, a.spy_duration, a.name_crc);
    const Vec3 c = planets_[planet].position;
    bool any = false;
    for (PlanetIndex q = 0; q < planets_.size(); ++q) {
        if (!planets_[q].is_planet) continue;
        const Vec3 p = planets_[q].position;
        const float dx = p.x - c.x, dy = p.y - c.y, dz = p.z - c.z;
        if (dx * dx + dy * dy + dz * dz <= r * r) any = add_visibility_modifier(q, owner, k, a.spy_mask, a.spy_duration, a.name_crc) || any;
    }
    return any;
}

int32_t GalacticSim::system_spy_remove(ObjectHandle unit) {
    if (!store_->exists(unit)) return 0;
    const int32_t id = static_cast<int32_t>(store_->id(unit));
    int32_t n = 0;
    for (PlanetState& p : planets_)
        for (std::vector<VisibilityModifier>& l : p.visibility) {
            const size_t before = l.size();
            l.erase(std::remove_if(l.begin(), l.end(), [id](const VisibilityModifier& m) { return m.key.object == id; }), l.end());
            n += static_cast<int32_t>(before - l.size());
        }
    return n;
}

bool GalacticSim::trade_route_visible(uint32_t route) const {
    if (route >= map_.routes.size()) return false;
    const auto raw = [this](PlanetIndex p) { return !valid_planet(p) || planets_[p].revealed; };
    return raw(map_.routes.point_a[route]) && raw(map_.routes.point_b[route]);
}

// ---- CRC ---------------------------------------------------------------------------------------------------------------

bool GalacticSim::visibility_state_default() const {
    for (const PlanetState& p : planets_)
        for (const std::vector<VisibilityModifier>& l : p.visibility)
            if (!l.empty()) return false;
    return true;
}

void GalacticSim::hash_visibility(StateHasher& h) const {
    h.u32(0x56495331u);  // "VIS1"
    for (PlanetIndex i = 0; i < planets_.size(); ++i) {
        const PlanetState& p = planets_[i];
        size_t n = 0;
        for (const std::vector<VisibilityModifier>& l : p.visibility) n += l.size();
        if (n == 0) continue;
        h.u32(i);
        for (size_t pl = 0; pl < p.visibility.size(); ++pl) {
            const std::vector<VisibilityModifier>& l = p.visibility[pl];
            if (l.empty()) continue;
            h.u32(static_cast<uint32_t>(pl));
            h.u32(static_cast<uint32_t>(l.size()));
            for (const VisibilityModifier& m : l) {
                h.u32(m.value);
                h.u32(m.expiry);
                h.u32(m.name_hash);
                h.i32(m.key.object);
                h.u32(m.key.serial);
                h.u32(m.key.sub);
                h.u32(m.key.extra);
            }
        }
    }
    h.u32(0xFFFFFFFFu);
}

}  // namespace alamo::galactic

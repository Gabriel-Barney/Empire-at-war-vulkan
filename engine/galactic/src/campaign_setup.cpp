// Campaign definition (engine/data CampaignDef) -> GalacticSetup.
#include <algorithm>

#include "alamo/data/game_database.hpp"
#include "alamo/data/values.hpp"
#include "alamo/galactic/campaign.hpp"

namespace alamo::galactic {

namespace {

bool iequals(std::string_view a, std::string_view b) {
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

GalacticSetup build_campaign_setup(const data::GameDatabase& db, const GalacticRules& rules, std::string_view campaign,
                                   const CampaignOptions& options, std::vector<std::string>* diagnostics) {
    GalacticSetup s;
    const data::CampaignDef* c = db.find_campaign(campaign);
    if (!c) {
        if (diagnostics) diagnostics->push_back("galactic: unknown campaign " + std::string(campaign));
        return s;
    }
    s.map = data::build_galaxy_map(db, campaign, diagnostics);
    s.planet_type.assign(s.map.planets.size(), kNoIndex);
    for (size_t i = 0; i < s.map.planets.size(); ++i) s.planet_type[i] = rules.find_type(s.map.planets.name[i]);

    // Factions the campaign names, collected as a set and ordered by FactionFiles order [L].
    std::vector<uint8_t> used(rules.factions.size(), 0);
    auto use = [&](std::string_view name) {
        const uint32_t f = rules.find_faction(data::trim(name));
        if (f != kNoIndex) used[f] = 1;
        return f;
    };
    auto first_field = [](std::string_view raw) {
        const std::vector<std::string_view> fields = data::split_fields(raw);
        return fields.empty() ? std::string_view{} : fields[0];
    };
    for (const auto& hl : c->home_locations) use(hl.first);
    for (const std::string& r : c->starting_credits) use(first_field(r));
    for (const std::string& r : c->starting_forces) use(first_field(r));
    for (const std::string& r : c->ai_player_control) use(first_field(r));
    for (const std::string& r : c->starting_tech_level) use(first_field(r));
    for (const std::string& r : c->max_tech_level) use(first_field(r));
    if (!c->starting_active_player.empty()) use(c->starting_active_player);
    const uint32_t neutral = rules.neutral_faction();
    if (neutral != kNoIndex) used[neutral] = 1;
    // REMAINING_GAPS 3.3 [inferred, probe 2026-10-29]: a Hostile player, so tactical maps' native objects (Hostile /
    // Sarlacc / Hutts owners) survive the battle's owner lookup. Retail campaigns hold a player per faction at its
    // faction index; Hostile's own entry was not logged. It takes no galactic part: no AI (no AI_Player_Control),
    // no credits, not playable (income, victory and the Holocron skip it), owns nothing.
    const uint32_t hostile = rules.find_faction("Hostile");
    if (hostile != kNoIndex && !rules.factions[hostile].is_playable && !rules.factions[hostile].is_neutral) used[hostile] = 1;

    const std::string_view human = options.human_faction.empty() ? std::string_view(c->starting_active_player)
                                                                  : std::string_view(options.human_faction);
    const uint32_t difficulty = rules.find_difficulty(options.ai_difficulty);
    std::vector<int32_t> player_of(rules.factions.size(), kNoPlayer);
    for (uint32_t f = 0; f < rules.factions.size(); ++f) {
        if (!used[f]) continue;
        PlayerSetup p;
        p.faction = f;
        p.human = iequals(rules.factions[f].name, data::trim(human));
        for (const std::string& r : c->ai_player_control) {
            const std::vector<std::string_view> fields = data::split_fields(r);
            if (fields.size() >= 2 && iequals(fields[0], rules.factions[f].name) && !iequals(fields[1], "None"))
                p.ai = !p.human;
        }
        if (p.ai) p.difficulty = difficulty;
        player_of[f] = static_cast<int32_t>(s.players.size());
        s.players.push_back(p);
    }
    for (const std::string& r : c->starting_credits) {
        const std::vector<std::string_view> fields = data::split_fields(r);
        if (fields.size() < 2) continue;
        const uint32_t f = rules.find_faction(fields[0]);
        if (f != kNoIndex && player_of[f] != kNoPlayer)
            s.players[static_cast<size_t>(player_of[f])].credits = data::parse_float_or(fields[1], 0.0f);
    }
    for (const std::string& r : c->starting_tech_level) {
        const std::vector<std::string_view> fields = data::split_fields(r);
        if (fields.size() < 2) continue;
        const uint32_t f = rules.find_faction(fields[0]);
        if (f != kNoIndex && player_of[f] != kNoPlayer)
            s.players[static_cast<size_t>(player_of[f])].tech_level = data::parse_int_or(fields[1], 1);
    }
    for (const std::string& r : c->max_tech_level) {  // REMAINING_GAPS 1.4 (0x4af2d0)
        const std::vector<std::string_view> fields = data::split_fields(r);
        if (fields.size() < 2) continue;
        const uint32_t f = rules.find_faction(fields[0]);
        if (f != kNoIndex && player_of[f] != kNoPlayer)
            s.players[static_cast<size_t>(player_of[f])].max_tech_level = data::parse_int_or(fields[1], 5);
    }
    s.tech_sweep_at_start = true;
    for (const auto& hl : c->home_locations) {  // GRC 1.4: Home_Location (faction, planet); the first entry wins
        const uint32_t f = rules.find_faction(data::trim(hl.first));
        const PlanetIndex p = s.map.planets.find(data::trim(hl.second));
        if (f != kNoIndex && player_of[f] != kNoPlayer && s.players[static_cast<size_t>(player_of[f])].home == kInvalidPlanet)
            s.players[static_cast<size_t>(player_of[f])].home = p;
    }
    for (std::string_view v : c->raw_values("Planet_Auto_Reveal"))  // GRC 0: campaign default true
        s.planet_auto_reveal = data::parse_bool_or(data::trim(v), s.planet_auto_reveal);
    for (const std::string& r : c->starting_forces) {
        const std::vector<std::string_view> fields = data::split_fields(r);
        if (fields.size() < 3) continue;
        const uint32_t f = rules.find_faction(fields[0]);
        const PlanetIndex p = s.map.planets.find(fields[1]);
        const uint32_t t = rules.find_type(fields[2]);
        if (f == kNoIndex || p == kInvalidPlanet || t == kNoIndex) {
            if (diagnostics) diagnostics->push_back("galactic: starting force skipped: " + r);
            continue;
        }
        s.forces.push_back({player_of[f], p, t});
    }
    return s;
}

}  // namespace alamo::galactic

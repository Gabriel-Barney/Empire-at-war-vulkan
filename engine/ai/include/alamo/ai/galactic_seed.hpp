// Builders for GalacticSnapshot until the S3 galactic economy (engine/galactic) owns the live
// galactic state: a campaign's starting state (Starting_Forces / AI_Player_Control / Home_Location)
// and a deterministic synthetic galaxy for the headless host's synthetic maps and the tests.
//
// PROVISIONAL (interim; replaced by the S3 state when it lands):
//   * players: the factions named by AI_Player_Control, then Home_Location, then Starting_Forces, in
//     first-appearance order, plus a "Neutral" player if none is named; every non-neutral faction
//     is an AI player (headless AI-vs-AI); relation = friendly only to itself;
//   * planet owner: the faction of the first ground force / structure listed at the planet, else of
//     the starbase, else of the first space force, else the neutral player (GALACTIC_ECONOMY.md 4.1
//     precedence, simplified); political control 1.0;
//   * starbase: the highest Base_Level StarBase listed at the planet; ground / special structures go
//     to the structure list (ground variant); every other force is a unit (layer per force type);
//   * Self sees the orbit of planets it owns or has forces at (no fog-reveal rules yet).
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ai/galactic_perception.hpp"
#include "alamo/core/galaxy.hpp"

namespace alamo::data {
struct CampaignDef;
}
namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::ai {

struct ForceType {
    bool valid = false;
    UnitLayer layer = UnitLayer::Space;
    bool starbase = false;   // StarBase element
    bool structure = false;  // GroundStructure / SpecialStructure element
    float combat_power = 0.0f;  // AI_Combat_Power
    int32_t base_level = 0;     // Base_Level (starbases)
};
using ForceTypeLookup = std::function<ForceType(std::string_view type_name)>;

// From the typed object database (engine/objects). Layer: SpaceUnit / Squadron / StarBase /
// SpaceProp -> space; GroundCompany / GroundInfantry / GroundVehicle / structures -> ground;
// HeroCompany / UniqueUnit and others -> space if the type (or a HeroCompany's first Company_Units
// member) has a CategoryMask token among Fighter, Bomber, Corvette, Frigate, Capital, SpaceHero,
// else ground. PROVISIONAL classification. `types` must outlive the lookup.
ForceTypeLookup make_force_type_lookup(const objects::GameObjectTypes& types);

struct CampaignSeedOptions {
    std::function<bool(std::string_view faction)> is_neutral;            // default: "Neutral"
    std::function<bool(std::string_view faction)> benefits_from_corruption;  // default: "Underworld"
    bool fog = true;  // false: GalacticSnapshot::fog_disabled and planet_auto_reveal
};

struct CampaignSeed {
    GalacticSnapshot snapshot;
    std::vector<std::string> player_names;  // index = player id
    std::vector<std::string> diagnostics;   // unknown planets / types, malformed entries
};

CampaignSeed seed_campaign_snapshot(const data::CampaignDef& campaign, const GalaxyMap& map,
                                    const ForceTypeLookup& lookup, const CampaignSeedOptions& options = {});

// Deterministic synthetic galaxy (alamo::SimRng): `ai_players` AI factions + one neutral player,
// random owners, starbases, units and orbit visibility. Same arguments => identical snapshot.
GalacticSnapshot make_synthetic_snapshot(uint32_t planets, uint32_t ai_players, uint32_t seed);

// Small deterministic change of a snapshot between frames (units move / die, owners flip), for
// hosts and tests that need perception input to evolve without the S3 simulation. Uses `seed`
// and `frame` only.
void perturb_snapshot(GalacticSnapshot& s, uint32_t seed, uint32_t frame);

}  // namespace alamo::ai

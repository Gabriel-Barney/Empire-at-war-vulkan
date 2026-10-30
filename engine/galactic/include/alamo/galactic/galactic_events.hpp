// S4 "Story & campaign events": the game-side events the galactic simulation emits for the story
// system (story plots' STORY_* triggers, docs/re/SUBSYSTEMS.md "story") and for the script event
// callbacks the engine looks up by name (docs/re/LUA_API.md: Galactic_Planet_Faction_Change,
// Galactic_Production_Begin_Event / _Canceled_Event / _End_Event, Galactic_Starbase_Level_Change,
// Galactic_Unit_Destroyed_Event).
//
// Stream contract (determinism rule 3): GalacticSim appends events from serial code only, in the
// order the state changes happen inside update() / the command and story entry points. Each event
// carries the galactic MODE frame at emission; the index in the stream is the tie-break. Nothing here
// is part of the galactic CRC (hash_state is unchanged), so enabling emission never changes goldens.
// Emission is off by default (no unbounded growth for hosts that never drain); a host that wants
// the stream calls set_emit_events(true) and drains it once per tick (drain_events).
//
// Which story triggers read which event (port mapping; the original's listener wiring is not
// traced, [L]):
//   PlanetOwnerChanged   STORY_CONQUER (Param1 planet, FILTER_*), Galactic_Planet_Faction_Change
//   FleetDeparted        (STORY_MOVE bookkeeping)
//   FleetArrived         STORY_ENTER (planet [, unit type]), STORY_MOVE (hero type, planet)
//   FleetBounced         STORY_FLEET_BOUNCED (planet)
//   ProductionBegin      Galactic_Production_Begin_Event
//   ProductionCanceled   Galactic_Production_Canceled_Event
//   ProductionCompleted  STORY_CONSTRUCT (type [, count]), Galactic_Production_End_Event
//   StarbaseLevelChanged STORY_CONSTRUCT_LEVEL, Galactic_Starbase_Level_Change
//   ObjectDestroyed      STORY_DESTROY, STORY_CHECK_DESTROYED, Galactic_Unit_Destroyed_Event
//   UnitsLanded          STORY_LAND_ON (planet)
//   CorruptionChanged    STORY_CORRUPTION_CHANGED (planet, corruption type)
//   TechLevelChanged     STORY_TECH_LEVEL (level)
//   GalacticDayPassed    (day counter; STORY_ELAPSED counts mode seconds, not days: GE §8)
#pragma once
#include <cstdint>
#include <string_view>

#include "alamo/core/galaxy.hpp"
#include "alamo/galactic/galactic_rules.hpp"

namespace alamo::galactic {

enum class GalacticEventKind : uint8_t {
    PlanetOwnerChanged = 0,  // planet, player = new owner, other_player = old owner
    FleetDeparted,           // fleet/object, player = owner, planet = from, other_planet = to
    FleetArrived,            // fleet/object, player = owner, planet, value = kArrive* reason
    FleetBounced,            // fleet/object, player = owner, planet = inaccessible planet, other_planet = back to
    ProductionBegin,         // player, planet, type, value = queue (0 ground, 1 space)
    ProductionCanceled,      // player, planet, type, value = queue, other_player = 1 if refunded
    ProductionCompleted,     // player, planet, type, object = created object (0 = none: tech upgrade)
    StarbaseLevelChanged,    // player, planet, value = new level (0 = destroyed)
    ObjectDestroyed,         // object, type (kNoIndex for a fleet object), player = owner, planet (or invalid)
    UnitsLanded,             // player, planet, value = units landed
    CorruptionChanged,       // planet, player = corrupting player, value = corruption type, other_player = old type
    TechLevelChanged,        // player, value = new level, other_player = old level
    GalacticDayPassed,       // value = fiscal cycle counter after the increment
    // S10 battle results (BATTLE_TRANSITIONS §3.1 step 7 / §4.1 step 2), posted by the conflict manager:
    BattleWon,               // planet, player = winner, other_player = loser, value = 1 land / 2 space, type = victory type
    BattleLost,              // planet, player = loser, other_player = winner, value = 1 land / 2 space
    BattleRetreat,           // planet, player = the retreating player, value = 1 land / 2 space
    // P4 sim (special_systems.hpp; docs/re/P4_SIM_SYSTEMS.md):
    SpecialDialog,           // a human's hero ability opens its panel: player, planet, object = hero, other_player = the
                             // SpecialAbility kind the panel answers with (4 black market / 5 sabotage / 6 corruption /
                             // 7 hack), value = q5 code of the black-market respawn time R (else 0)
    SpecialError,            // the local error SFX: player, planet, object = hero
    SpecialApplied,          // a Ground_Activated apply succeeded (story LAND_ON 2 + DEPLOY 0xC): player, planet, object, type = unit
                             // type, value = SpecialRules::abilities index
    BlackMarketPurchase,     // player, planet, object = hero, value = item index, other_player = 1 for a human buyer
                             // (story BUY_BLACK_MARKET 0x38 with the item Name [port: at execute time; humans only])
    GalacticSabotage,        // story GALACTIC_SABOTAGE 0x39: player (-1 story reward), planet, type = structure type
    SabotageVigilance,       // sabotage failed on Planet_Vigilance: player = saboteur owner, other_player = planet owner, planet
    PlanetDestroyed,         // story PLANET_DESTROYED 0x17: planet, player = destroyer, other_player = old owner
    SuperWeaponHacked,       // player = hacker owner, planet, type = super weapon type, other_player = its owner
    HeroRespawnScheduled,    // player = owner, planet = scheduling planet, type, value = due mode frame
    HeroRespawned,           // player, planet, type, object = the new object
    // REMAINING_GAPS 1 (tech theft and unlocks):
    TypeUnlocked,            // player, type = the type added to the unlocked list, value = 1 tech sweep / 2 slice / 3 story or
                             // Lua / 4 corruption (faction SFXEvent_New_Construction_Options_Available for the local player)
    Slice,                   // player = slicer owner, planet, object = slicer, type = the entry's type (kNoIndex: no
                             // candidate), value = outcome (0 success, 1 captured, 2 lost effort, 3 cheated, 4 vigilance),
                             // other_player = planet owner
    Count
};

// FleetArrived reasons.
inline constexpr int32_t kArrivePassThrough = 0;  // intermediate hop, the fleet continues
inline constexpr int32_t kArriveFinal = 1;        // final hop, the fleet stops in orbit
inline constexpr int32_t kArriveIntercepted = 2;  // intermediate hop, stopped by a conflict

std::string_view galactic_event_name(GalacticEventKind kind);

struct GalacticEvent {
    int32_t frame = 0;  // galactic mode frame at emission
    GalacticEventKind kind = GalacticEventKind::Count;
    int32_t player = -1;
    int32_t other_player = -1;
    PlanetIndex planet = kInvalidPlanet;
    PlanetIndex other_planet = kInvalidPlanet;
    uint32_t fleet = 0xFFFFFFFFu;  // kNoFleet
    uint32_t type = kNoIndex;      // TypeIndex
    int32_t object = 0;            // ObjectId (0 = none)
    int32_t value = 0;
};

}  // namespace alamo::galactic

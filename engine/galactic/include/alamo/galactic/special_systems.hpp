// P4 sim (owner: galactic agent): the FoC galactic special systems behind the P4 panels (docs/re/P4_SIM_SYSTEMS.md = P4):
// the Ground_Activated hero trigger and hero respawns, the black market, sabotage, the super-weapon hack, the planet
// income abilities and the corruption requirements. Data tables: special_data.hpp (GalacticRules::special). The
// operations are GalacticSim members (campaign.hpp "P4 sim" block); this header holds their result types, the
// SpecialAbility (event 13) payload builders / readers and the API overview below.
//
// ---- API for the consumers (engine/ui panels, engine/lua, engine/ai) -----------------------------------------------
// Plain getters (const, no RNG, no state change):
//   bm_offers(player, planet, hero)            black-market offers in item-table order {item, price, purchased} (P4 1.3)
//   bm_purchased(player) / bm_has_item          the player's purchased items (purchase order) / one item
//   bm_allows_unit_ability(player, crc, must)   Must_Be_Bought_On_Black_Market gate of a Unit_Ability record (P4 1.5)
//   bm_allows_special_ability(player, crc, must)  the same gate for an <Abilities> entry
//   player_tech(player)                         tech(P) of P4 1.2 step 3 (tech level + Displayed_Tech_Level_Adjustment)
//   sabotage_options(planet, saboteur)          option kinds 0..4 available at the planet, option order (P4 2.2)
//   sabotage_cost(player, planet, structure, saboteur)  cost of one structure (P4 2.4; -1 = not sabotageable)
//   sabotage_targets(player, planet, saboteur)  the dialog rows: structures with cost >= 1 {object, type, cost}
//   hack_find_super_weapon(planet)              the required-type object orbiting the planet (P4 3.3), null if none
//   hack_cost()                                 GameConstants Hack_Super_Weapon_Cost
//   corruption_choices(planet)                  the planet type's choices (slot order)
//   corruption_requirements(player, planet, choice)  per-row "n/m" counts and the four checks (P4 5.2)
//   corruptions_completed(player, type)         completed-corruption counter (P4 5.2 mission rule)
//   economy_breakdown_by_tag(player)            income breakdown rows grouped by source tag (P4 4.3)
//   economy_income_for_tag(player, tag) / mining_income(player)  one row (TEXT_ECONOMY_MINING_FACILITIES)
//   respawns_pending(planet), type_locked(player, type), special_tutorial flags, sabotage_count(planet)
// Operations (sim thread, serial; deterministic):
//   ground_activated_trigger(unit, planet)      P4 6.1: every Ground_Activated ability of the unit's type, list order
//                                               (Lua Activate_Ability / Land_Units and the AI plans call this)
//   schedule_hero_respawn(hero, planet, s) / hero_depart(hero, planet, s)   P4 6.2 (negative time drops the respawn)
//   bm_execute_purchase(...), sabotage_execute_structure(...), hack_execute(...)   the command executors
//   bm_give_item(player, name) (story GIVE_BLACK_MARKET), sabotage_structure_reward(planet, type) (SABOTAGE_STRUCTURE)
//   corruption_ai_pick, corruption_charge, count_corruption   corruption executor helpers (apps/sim kind 6 path)
//   planet_destroy(planet, destroyer)           the shared planet destruction routine (hack; Death Star later)
//   lock_type(player, type, on), set_bm_tutorial / set_sabotage_tutorial (Lua Lock_Tech, Set_*_Tutorial)
// Commands: SpecialAbility (13) kinds 1 activate, 2 slice, 4 buy, 5 sabotage a structure, 7 hack are executed by
// GalacticSim::execute; kind 6 (corruption) stays Unhandled for apps/sim CampaignBattles (campaign_battles.hpp).
// Dialogs: a human's apply returns "not applied" and posts GalacticEventKind::SpecialDialog (the UI opens the panel
// and answers with the kind-4/5/6/7 command); errors post SpecialError (the local error SFX).
//
// ---- CRC ---------------------------------------------------------------------------------------------------------
// "galactic.state" gets, after the S3 sections and only while some special state is non-default (sessions that
// never use these systems keep their CRC stream): per player (index order) purchased item CRCs (purchase order),
// tutorial flags, corruption counters[8], locked types (lock order); per planet (row order) sabotage counter and
// respawn entries (list order: due, type, owner, flag). Ability income entries hash their key / tag / next-eval
// frame after the S3 fields, only for entries written by these systems.
#pragma once
#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/galactic/special_data.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo::galactic {

// SpecialAbility (event 13) kinds (P4 0 item 1, COMMANDS.md id 13) [H].
inline constexpr uint32_t kSpecialActivate = 1;
inline constexpr uint32_t kSpecialSlice = 2;  // REMAINING_GAPS 1.3.1
inline constexpr uint32_t kSpecialBlackMarket = 4;
inline constexpr uint32_t kSpecialSabotage = 5;
inline constexpr uint32_t kSpecialCorruption = 6;  // apps/sim CampaignBattles (kSpecialAbilityCorrupt)
inline constexpr uint32_t kSpecialHack = 7;
// Quantizers of the payload floats (COMMANDS.md 3.2): q8 amounts, q5 durations.
inline constexpr uint8_t kQuantAmount = 8;
inline constexpr uint8_t kQuantDuration = 5;

// Sabotage option kinds (P4 2.2 order).
enum SabotageOption : uint8_t {
    kSabotageGroundBase = 0, kSabotageStarbase = 1, kSabotageProduction = 2, kSabotageSpecialStructure = 3, kSabotageHalt = 4
};

// Black-market offer (P4 1.3).
struct BlackMarketOffer {
    uint32_t item = 0xFFFFFFFFu;  // SpecialRules::items index
    float price = 0.0f;
    bool purchased = false;
};

struct SabotageTarget {
    objects::ObjectHandle structure;
    uint32_t type = 0xFFFFFFFFu;
    int32_t cost = -1;
};

// Corruption requirement check (P4 5.2) for one choice; *_have / *_need are the dialog's "n/m".
struct CorruptionRequirements {
    bool credits = true, mission = true, units = true, planets = true;
    float cost = 0.0f;
    int32_t mission_have = 0, mission_need = 0;
    int32_t units_have = 0, units_need = 0;
    int32_t planets_have = 0, planets_need = 0;
    bool all() const { return credits && mission && units && planets; }
};
// corruption_charge result: the executor's order (P4 5.2: credits, charge, then units; no refund).
enum class CorruptionCharge : uint8_t { NoCredits = 0, UnitsUnmet = 1, Charged = 2 };

// One economy-panel row: entries of the player's planets summed per source tag (first-seen order: planet row order,
// entry order). Standard entries have tag 0 and their IncomeKind; ability entries their source tag (kIncomeAbility).
// Siphon / piracy rows use the tags of TEXT_ECONOMY_SMUGGLING (taken out, negative), TEXT_INCOME_SMUGGLER (gained),
// TEXT_INCOME_PIRACY.
struct EconomyRow {
    uint32_t tag = 0;
    uint8_t kind = 0;
    float value = 0.0f;
};
uint32_t economy_mining_tag();  // special_name_crc("TEXT_ECONOMY_MINING_FACILITIES")

// Slice dialog row / AI entry (REMAINING_GAPS 1.3 step 3, 16 bytes in the original).
struct SliceEntry {
    uint32_t type = 0xFFFFFFFFu;
    float price = 0.0f;
    float chance = 0.0f;
};

// Respawn entry of a planet's spawn list (P4 6.2: D +0x240, 0x28 bytes).
struct RespawnEntry {
    int32_t due = 0;        // galactic mode frame
    uint32_t type = 0xFFFFFFFFu;
    int32_t owner = -1;
    uint8_t flag = 0;       // hero +0x330 [L]: always 0 in the port
};

// ---- SpecialAbility payloads ([port] layout of the Opaque "data" bytes, little-endian; COMMANDS.md gives the
// original's bit fields: id32s, q8 / q5 codes, flag bits) ----------------------------------------------------------
// kind 1: u32 planet id, u32 unit id (a unit or a fleet: its first member with an accepting Ground_Activated ability)
// kind 4: u32 planet, u32 buyer hero, u32 item name CRC, u32 q8 price code, u32 q5 respawn-seconds code, u8 only-one
// kind 5: u32 planet, u32 saboteur, u32 structure, u32 q8 cost code, u32 q5 respawn-seconds code
// kind 7: u32 planet, u32 hacker, u32 q8 cost code
commands::EventData special_activate_event(uint32_t planet_id, uint32_t unit_id);
commands::EventData black_market_purchase_event(uint32_t planet_id, uint32_t hero_id, uint32_t item_crc, float price,
                                                float respawn_seconds, bool only_one = false);
commands::EventData sabotage_structure_event(uint32_t planet_id, uint32_t saboteur_id, uint32_t structure_id, float cost,
                                             float respawn_seconds = 0.0f);
commands::EventData hack_super_weapon_event(uint32_t planet_id, uint32_t hacker_id, float cost);
// kind 2: u32 planet, u32 slicer, u32 type, u32 q8 price code, u32 q5 code of chance x 1000, u32 q5 respawn code, u8 only-one
commands::EventData slice_event(uint32_t planet_id, uint32_t slicer_id, uint32_t type, float price, float chance,
                                float respawn_seconds, bool only_one);

struct SpecialPayload {
    uint32_t kind = 0;
    uint32_t planet = 0, unit = 0;
    uint32_t item_crc = 0, structure = 0;
    float amount = 0.0f;   // dequantized q8 (price / cost)
    float seconds = 0.0f;  // dequantized q5
    bool only_one = false;
    uint32_t type = 0xFFFFFFFFu;  // kind 2: the sliced type
    float chance = 0.0f;          // kind 2: dequantized q5 / 1000
};
// False for another event type, kind 6 or a malformed size.
bool read_special_payload(const commands::EventData& ev, SpecialPayload& out);

}  // namespace alamo::galactic

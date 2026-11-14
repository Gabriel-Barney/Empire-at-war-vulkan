// OI host (alamo_game): data-free feeds for the HUD widgets of docs/re/HUD_GAPS.md, computed from sim state on the host
// side (presentation only, never read by the sim):
//   day_cycle_fraction / galactic_cycle_fraction   §4: the fiscal-cycle (income) timer behind the 10 day pips:
//                        (mode frame - cycle start frame) / round(30 x Fiscal_Cycle_Time_In_Secs), clamped 0..1; < 0
//                        (pips hidden) without a cycle length.
//   order_buttons        §5: c_button00..05 enable inputs from the local player's OWN selected units (attack behaviour,
//                        locomotor); controls locked (story LOCK_CONTROLS / Lua Lock_Controls) disables all.
//   bombing_run_button   §9.2 / §9.3 (cu9g): the land b_special_weapon button: disabled := controls locked or the run not
//                        available; dial = the 0x495250 cooldown fraction (0 shown as a full dial, none when ready).
//   bombing_run_style    §9.4 / §9.5: the faction's Bombing_Run_Shadow_Blob_Material_Name and Bombing_Run_Blob_Size.
//   hardpoint_type_name  §1.5: HardPointType name of a combat HardpointKind (the reticle texture table key).
//   hardpoint_world      §1.3: a hard point's unit-local attachment offset (x forward, y left, z up) in world space.
//   galactic_hero_entries §3.2: the galactic hero roster of a player - named heroes with an Icon_Name on the map (one
//                        per type, full dial), then the player's named heroes being built (queue head: build fraction)
//                        or awaiting respawn (respawn list), at most `max_entries`.
//   conflict_units       GALACTIC_DIALOGS §1.3 / §3.1: one side's forces of a conflict - space: the player's fleets in
//                        orbit (fleet order, member order; carried ground companies included), then its starbase; land:
//                        the attacker's invading units (ConflictInfo::units), the defender's surface units.
//   battle_force_summary §1.3 commander graph of one side: icon = the first named hero's Icon_Name (its portrait), else the
//                        first unit with an icon (lead unit); units = live count; stacks = ceil(units / Ships_Per_Stack)
//                        capped at 5 (fleet_stacks_01..05; 0 = fleet_empty).
//   ar_result_magnitude  §3.1 / BATTLE_TRANSITIONS §4.1 step 4: the TEXT_AR_* magnitude (0 MAJOR, 1 REGULAR, 2 MINOR;
//                        ui::auto_resolve_text's argument) from the recorded combatant losses n_own / n_other of the
//                        viewpoint player: loss MAJOR n_own > 3 n_other, REGULAR n_own > n_other; win the same swapped.
//   is_named_hero        Is_Named_Hero of the type or of a company's first member.
//   galactic_build_options ui4 t02 (GALACTIC_DIALOGS §5.1, A2K_T01 §1): the galactic build pads of a player at a planet -
//                        GalacticSim::buildable in type order; cost = build_cost_for (planet / player price modifiers,
//                        corruption and tech-upgrade multipliers), block = can_enqueue's reason (galactic_build_block),
//                        queued = the player's entries of the type in the planet's two queues. Const reads only.
//   conflict_log_line    ui4 t05 [port]: the Holocron Droid Log line of a battle record (instead of ConflictLogEntry::text's
//                        debug line): "Battle of <planet> (space|land): <attacker> vs <defender> - <winner> victorious", with
//                        " (auto-resolved)", "<player> retreated" or "called off" (a dropped conflict). Names come from the
//                        callbacks (planet display name, faction display name).
//   galactic_announcement ui4 t08: the local player's Factions.xml announcer tag for one galactic event, nullptr none -
//                        PlanetOwnerChanged: SFXEvent_Planet_Gained_Control when the local player gains a planet ("plays anytime
//                        control is gained"), _Lost_Control when another player takes it from the local player (XML comments);
//                        CorruptionChanged: SFXEvent_Planet_Corrupted (a type set) / SFXEvent_Corruption_Removed (cleared) when
//                        the local player owns the planet or is the corrupter (GALACTIC_REVEAL_CORRUPTION §4.4 step 4 [H]);
//                        FleetDeparted: SFXEvent_Enemy_Fleet_Approaching_Planet when a hostile fleet that is not all galactic-
//                        stealthed sets off towards a planet of the local player [port].
//   galactic_build_block a can_enqueue RejectReason as a pad block: QueueFull, the population cap, CannotAfford (Funds),
//                        anything else Prerequisite (greyed, no tooltip [port]); None stays None.
// Needs engine/ui's alamo_ui_hud (ALAMO_GAME_HAVE_CONTROLS).
#pragma once
#ifdef ALAMO_GAME_HAVE_CONTROLS
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/ui/galactic_hud.hpp"
#include "alamo/ui/hud_markers.hpp"
#include "alamo/ui/hud_panels.hpp"
#include "alamo/ui/tactical_hud.hpp"

namespace alamo::game {

float day_cycle_fraction(int32_t mode_frame, int32_t cycle_start, int32_t cycle_frames);
float galactic_cycle_fraction(const galactic::GalacticSim& g);

struct OrderUnitState {
    int32_t owner = -1;
    bool can_attack = false;  // attack behaviour (a live weapon)
    bool can_move = false;    // locomotor
};
ui::HudOrderButtons order_buttons(std::span<const OrderUnitState> selection, int32_t local_player, bool controls_locked);

// HUD_GAPS §9.2: `land` = a land battle (else the button is not driven); ready = !controls_locked && available
// (TacticalBattle::bombing_run_available); `fraction` = TacticalBattle::bombing_run_cooldown_fraction. Not ready:
// dial = fraction, a 0 shown as 1.0 (§9.3 rule 4: a skirmish before its first run is disabled with a full dial); ready:
// no dial [port, §9.9 item 1].
ui::HudBombingRunButton bombing_run_button(bool land, bool controls_locked, bool available, float fraction);
// ui1 t04 (HUD_GAPS §9.1, 0x317d00; [port] shown / hidden rule, backlog b0238): the space b_special_weapon. `space` = a
// space battle (else not driven); `icon` = the local player's weapon (its Special_Weapon_Index, a station 2), < 0 = none:
// hidden; ready = readiness >= 1, allowed (SpecialWeaponsAllowed, else tooltip 4) and controls not locked; the dial =
// readiness while not ready.
ui::HudBombingRunButton space_weapon_button(bool space, int32_t icon, bool allowed, bool controls_locked, float readiness);

struct BombingRunStyle {
    std::string blob_material;   // Bombing_Run_Shadow_Blob_Material_Name ("" = no blob: targeting stays on without one)
    float blob_size = 100.0f;    // Bombing_Run_Blob_Size: the square's full side (DECORATIONS §3.2)
};
BombingRunStyle bombing_run_style(const galactic::FactionRules& faction);

const char* hardpoint_type_name(uint8_t kind);
ui::Vec3 hardpoint_world(ui::Vec3 position, ui::Vec3 facing, ui::Vec3 offset);

// `icon(type)` = the type's Icon_Name ("" = no slot); `selection` = selected object ids (ascending).
// GALAXY_MAP_PRESENTATION §4: `ranking(type)` = the type's Ranking_In_Category (null: 0) fills HeroEntry::ranking;
// HeroEntry::ranking / type_name come from hero_bar_type(type), not the *_Team type (FIXPASS_GALACTIC §2.4, capture C3);
// ui::sort_hero_entries orders them.
std::vector<ui::HeroEntry> galactic_hero_entries(const galactic::GalacticSim& g, int32_t player,
                                                 const std::function<std::string(uint32_t type)>& icon,
                                                 std::span<const uint32_t> selection, size_t max_entries = 11,
                                                 const std::function<int32_t(uint32_t type)>& ranking = {});

bool is_named_hero(const galactic::GalacticSim& g, uint32_t type);

// FIXPASS_GALACTIC §2.4 (capture C3): the type a hero bar entry stands for. A company = its Company_Transport_Unit when that
// is a named hero (Han_Solo_Team -> Millennium_Falcon), else its first Company_Units member when that is one
// (Boba_Fett_Team -> Boba_Fett, its Slave_I is not named); any other type = itself.
uint32_t hero_bar_type(const galactic::GalacticRules& rules, uint32_t type);

std::vector<objects::ObjectHandle> conflict_units(const galactic::GalacticSim& g, const galactic::ConflictInfo& c, int32_t player);

struct BattleForceSummary {
    std::string icon;       // "" = no icon
    bool hero = false;      // icon is a named hero's
    uint32_t units = 0;
    uint32_t stacks = 0;    // 0..5
};
BattleForceSummary battle_force_summary(const galactic::GalacticSim& g, std::span<const objects::ObjectHandle> units,
                                        const std::function<std::string(uint32_t type)>& icon, uint32_t ships_per_stack);

int32_t ar_result_magnitude(bool win, uint32_t n_own, uint32_t n_other);

ui::HudBuildOption::Block galactic_build_block(commands::RejectReason why);
const char* galactic_announcement(const galactic::GalacticSim& g, const galactic::GalacticEvent& e, int32_t local_player);
std::u32string conflict_log_line(const galactic::ConflictLogEntry& e, const std::function<std::u32string(PlanetIndex planet)>& planet_name,
                                 const std::function<std::u32string(int32_t player)>& faction_name);
// `icon(type)` = the type's Icon_Name.
std::vector<ui::GalacticBuildOption> galactic_build_options(const galactic::GalacticSim& g, int32_t player, PlanetIndex planet,
                                                            const std::function<std::string(uint32_t type)>& icon);

}  // namespace alamo::game
#endif

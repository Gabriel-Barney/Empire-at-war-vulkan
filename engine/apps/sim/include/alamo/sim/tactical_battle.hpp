// S10 "Battle transitions": the tactical battle of one galactic conflict (docs/re/BATTLE_TRANSITIONS.md §2-§3)
// on a pushed Land/Space mode, driven by MovementSystem (S7) + CombatSystem (S8) wrapped by AbilitySystem (S9).
//
// SETUP (setup(), BT §2.1-2.6, REMAINING_GAPS §3 / §6.5). Map placements (bt_map_prepass / bt_map_load): the
// base-level filter (mini 0x0B vs the planet's base level), placements whose owner (mini 02 = player id) is not a
// battle player are discarded, branched-map discard; Map_Load_Spawn_Table rows draw (synced) their type and
// Affiliation owner; selectable / mobile placements are adopted as combat units of their map owner (Is_Neutral owners
// stay inert; map-placed companies are skipped [port]); MARKER objects feed bt_place_forces (station pick, structure
// markers, Defending / Attacker Entry facings). Persisted pad children are restored (§6.5). Land =
// passability grid + LAND_OBSTACLE stamps + terrain height, space = playable box + SPACE_OBSTACLE statics.
// Participants in BattleRequest order (defender start force, defender reserve, attacker start force,
// attacker reserve, then the starbase / ground structures): a galactic unit becomes one tactical object of
// its type, a squadron / company becomes its Squadron_Units / Company_Units members [M] followed by one team
// container object of the galactic type (S10-int: TEAM container for the abilities, no tree entry, dies with
// its last member), every member carries the galactic object id as parent-mode id. Positions [port]: the map's
// "Defending Forces Position" / "Attacker Entry Position" markers, else +-45% of the playable box on X; rows of
// 6 facing the enemy; land spawns snap to the nearest cell passable for the unit's MovementClass (ring
// search). The synced RNG is not reseeded (only the combat units' hardpoint / first-cooldown draws happen at
// spawn). Victory conditions registered (§2.5): all enemy units destroyed (0xb), all capable enemies
// retreated (0xc), space station destroyed (0xf, only with Space_Station_Destruction_Forces_Retreat).
//
// TICK (tick(), right after the tactical mode tick, BattleHost hook):
//   1. reinforcements [port stand-in for the reinforcement UI / AI]: every reinforce_interval frames, or at
//      once when a side has no unit on the map, reserve units (pool order) deploy while their population
//      fits under the faction's Space_Tactical_Unit_Cap (space; land: all at once); P host: with skip_human_units a
//      human player's reserves wait for Reinforce commands (order_reinforce) unless the side has no unit left;
//   2. stand-in orders [port, until the tactical AI (A1)]: every order_interval frames each movable unit
//      (ascending id) moves to 70% of its attack range from the nearest enemy unit (squared XY distance,
//      ties by id); CombatSystem targets and fires on its own;
//   3. retreat coordinator (§3.4): requests allowed after Space/Land_Retreat_Allowed_Countdown_Seconds;
//      countdown = faction Space/Land_Retreat_Countdown_Seconds; then the player's units leave the map
//      (preserved in the galaxy). Stand-in policy: a side retreats when its power < retreat_ratio x the
//      enemy's (0 = never);
//   4. S10-int speed factors (MOVEMENT 3.4): per movable unit Mod_Multiplier(Speed) x
//      Engines_Disabled_Speed_Modifier (after the last ENGINE hardpoint died) x (1 + modifier list 5),
//      0 while the ability system immobilises it; MovementSystem::tick + commit, tree refit + maintain;
//      with animation: the locomotor drives Move / Walk (switch_preserving_phase, set_speed) and idles;
//   5. combat through the ability system (positions / facings pushed first; ability commands 40/43/57/58
//      ran at the command phase), then in this order: damage events -> DAMAGE_TRACKING; projectile hits /
//      expiries -> PARTICLE objects (detonation particle, then the synced hit-particle pick); hardpoint
//      destructions -> Death_Explosion_Particles (PARTICLE) + Death_Breakoff_Prop objects; deaths ->
//      TacticalMode::destroy + movement removal, death clone object (refixf t04: its death plan - spin / variant
//      draws, removal frame - schedule_death_clone), first Death_Explosions entry [port];
//      damaged-smoke events -> attached_effects() (render); ability events (EnginesOffline); clear_events;
//      death clones whose planned removal frame came are destroyed; PARTICLE objects whose
//      Particle_Lifetime_Frames ran out are destroyed; dummy targets without a pending ability are removed;
//   6. victory monitor (BT §3.1a, tactical_battle_victory.cpp): event driven - a victory-relevant object leaving a player
//      (death / removal / owner change) tests 0xb (then 0xe land / 0xf space) for that player's enemies, a completed retreat
//      tests 0xc; the hold counter (ENABLE_VICTORY, TSW) queues tests; a declaration ends the battle after its record's
//      countdown (210 frames, story VICTORY 7 s / 1 s); max_frames [port time limit: the defender holds, the attacker
//      withdraws]. The tick that finishes the battle removes every death clone left (forever ones too) [port].
// RESULT: winner, victory type, retreated player, galactic objects whose every tactical counterpart died
// (never-deployed reserves and retreated units survive), starbase destroyed.
// Determinism: units and participants in vectors by id / request order; all draws on the synced RNG in
// serial code; movement / combat parallel phases are worker-count independent; spawned objects (particles,
// props, clones, containers, dummies) consume ids in the event order above.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "alamo/commands/command.hpp"
#include "alamo/commands/events.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/galactic/conflicts.hpp"
#include "alamo/sim/battle_catalog.hpp"
#include "alamo/sim/game_modes.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/combat/combat_system.hpp"
#include "alamo/tactical/movement/fleet_placement.hpp"  // cu8l t03
#include "alamo/tactical/movement/movement_system.hpp"
#include "alamo/tactical/movement/space_avoid.hpp"  // SPACE_AVOIDANCE (fighter avoidance members)
#include "alamo/tactical/abilities/space_fields.hpp"     // L1 systems
#include "alamo/tactical/combat/weather.hpp"            // L1 systems
#include "alamo/tactical/combat/hero_clash.hpp"         // s78n t06: land hero clashes
#include "alamo/tactical/abilities/special_weapons.hpp"  // L1 systems
#include "alamo/tactical/movement/hyperspace.hpp"        // L1 systems
#include "alamo/tactical/hangar/engine_loops.hpp"         // hangars / engine loops (STATION_HANGARS_SOUND.md)
#include "alamo/tactical/hangar/hangar_system.hpp"
#include "alamo/tactical/hangar/contents.hpp"            // station contents / tactical production
#include "alamo/terrain/ted.hpp"
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
#include "alamo/ai/tactical/damage_tracking.hpp"
#endif
#include "alamo/tactical/fog/fog_of_war.hpp"  // fog of war hookup (owner: fog agent)
#include "alamo/tactical/garrison/capture.hpp"   // S4 garrisons + structure capture (owner: S4 agent)
#include "alamo/tactical/garrison/garrison.hpp"

namespace alamo::anim {
struct IdleRecord;  // s78l t03 (alamo/anim/anim_behaviors.hpp)
}  // namespace alamo::anim

namespace alamo::sim {

class SimAnimation;
class BattleHooks;  // tactical_battle.cpp: CombatHooks forwarder (abilities + animation)
class BattleAi;     // A1: battle_ai.hpp

struct BattleConfig {
    uint32_t order_interval = 60;      // [port] stand-in AI re-order cadence (frames)
    uint32_t reinforce_interval = 300; // [port] reserve deployment cadence (frames)
    uint32_t max_frames = 27000;       // [port] time limit (15 min)
    float retreat_ratio = 0.0f;        // [port] stand-in retreat threshold (0 = never retreat)
    // [port] Playtest QA D1: frames the battle keeps running at least between the victory declaration and the result (the mode
    // pop), so the last death's break-up can play. story t06: the monitor's own countdown (BT §3.1a: 210 frames, VICTORY 30 /
    // 210) now runs in every battle; this is a minimum on it (0 = the record's countdown only). The time limit never holds.
    uint32_t end_hold_frames = 0;
    float space_spacing = 250.0f;      // [port] formation spacing (world units)
    float land_spacing = 40.0f;
    // S10-int (cu9 t13: sim PARTICLE objects are always spawned, R4 B.1 layer 1; the `particles` switch is gone)
    uint32_t particle_default_lifetime = 30;  // [port] frames when Particle_Lifetime_Frames is unset / <= 0
    uint32_t dummy_lifetime = 900;            // [port] a Dummy_Barrage_Target is removed after this many frames at most
    float walk_speed_ratio = 0.5f;            // [port M] WALKMOVE below this x max speed (global 0x140b16d04 unknown)
    float min_move_anim_ratio = 0.1f;         // [port] move-clip speed floor (current / max speed)
    // s78l t03 (TACTICAL_GAPS 6.2): GameConstants Unit_Presence_Relaxation_Time (GC +0x117a8), seconds an IMPOSING_PRESENCE
    // idle record lasts (int(FPS x value) frames). [port] 3.0: the shipped XML does not set it and the default was not read.
    float presence_relaxation_seconds = 3.0f;
    // Fog of war (docs/re/FOG_OF_WAR.md; owner: fog agent). Not in the frame CRC, never draws RNG.
    bool fog = true;
    // The overrun reveal (FOG_OF_WAR §3.3) runs only in the multiplayer game types: not ported (CU7).
    // --- integration fixes (alamo_game): [port] true = the units of human players (GalacticSim::players()[p].human)
    // take orders only from commands (TacticalBattle::order_*): the stand-in orders (issue_orders, BattleAi's
    // stand-in) skip them. Default false: headless sims / goldens let the stand-in drive the "human" side too. ---
    bool skip_human_units = false;
    // HARDPOINT_DISABLED.md §4: a skirmish battle (the skirmish session's battle) - SpecialWeaponsAllowed (0x527620) then
    // ANDs the player's special-weapon hard-point flag (CombatSystem::special_weapon_hp_enabled).
    bool skirmish = false;
    // GALAXY_MAP_PRESENTATION §6: a map battle (CampaignBattles::start_map_battle, the attract demo's Load_Map): no
    // conflict behind it, so the branched-map discard (single player with a defender, bt_map_prepass) does not apply.
    bool map_battle = false;
    // a1i t04 (A1_READINGS §35.2 / §35.3): true (default since a1j t01) = the TransportLanding state machine: hidden until
    // its footprint is clear of the other landings, then one team / vehicle per 2 s x (1 + s) step on a locked exit move,
    // unlocked with the signal 0x19 record after the last (the tactical AI's TF.Reinforce waits for it, A1_READINGS §36);
    // false = [port] a landing object's company appears at the Reinforce order (every unload step at once).
    bool landing_unload_timed = true;
    // skai (q2f t04): a skirmish map battle with the skirmish setup (the headless skirmish host; with skirmish and map_battle):
    // both slots' team markers become their faction's objects, Income_Stream_Ability pays, the skirmish victory rules apply
    // (tactical_battle_skirmish.cpp). The attract demo's map battle leaves it off.
    bool skirmish_map = false;
    // ccpgt t02 (SKIRMISH_SETUP §4.2): the Capture Control Points game type (4), land only: each player's win condition is 0x15
    // CONTROL_POINT_DOMINATION instead of the map kind's default. Off: unchanged rules.
    bool control_point_domination = false;
    // --- S4 garrisons / structure capture (owner: S4 agent; tactical_battle_garrison.cpp) ---
    // GameConstants Garrisoned_Max_Attack_Distance_Multiplier / Land_ / Space_Capture_Allowed_Countdown_Seconds (vanilla
    // values; used only when the catalog's GameConstants lack them: BattleCatalog::capture_countdown).
    float garrisoned_range_multiplier = 1.5f;
    float land_capture_countdown = 30.0f, space_capture_countdown = 30.0f;
    // cu9 t13: garrison holders / occupants / capture points are always registered (the `garrisons` switch is gone).
    // --- end S4 ---
};

struct BattleStats {
    uint32_t frames = 0, spawned = 0, reinforcements = 0, deaths = 0, orders = 0;
    uint64_t shots = 0, hits = 0;
    // S10-int
    uint32_t containers = 0, ability_events = 0, activations = 0, ability_commands = 0, hardpoints_destroyed = 0;
    uint32_t particles = 0, props = 0, death_clones = 0, smoke = 0, dummies = 0, anim_plays = 0;
    uint32_t death_clones_removed = 0;  // refixf t04: clone objects removed by their death plan (at once included)
    // t50 (land skirmish): pads of the two slots, pads that started built, structures started on a pad during the battle.
    uint32_t skirmish_pads = 0, skirmish_prebuilt = 0, pad_builds = 0;
    uint32_t pad_restored = 0;  // §9.4 persisted pad children recreated at setup
    // cu8l t03: the campaign space fleet placement (objects placed, their adopt draws), the vf82 pass's TEAM-container lock
    // adopt draws, the arrivals started and the arrival ends that registered a new coordinator.
    uint32_t fleet_placed = 0, fleet_adopts = 0, fleet_lock_adopts = 0, fleet_arrivals = 0, fleet_arrival_ends = 0;
};

// A render-side attachment reported by the sim (Damaged_Smoke_Asset_Name at 75 % / 50 %).
struct AttachedEffect {
    objects::ObjectId id = kNoObjectId;
    std::string asset;
    uint8_t percent = 75;
    uint32_t frame = 0;
};

// ---- a1g t04 follow-up: land company drop layout and validity (TACTICAL_GAPS §4.2.1: 0x3b6170, 0x5abc00, 0x40f970) ----
// One layout serves the AI drop check (BattleAi::land_drop_ok) and the reserve deploy (order_reinforce), so they agree.
struct LandDropShape {
    std::vector<uint32_t> members;  // Company_Units member types in list order, one per spawned member; a member without a land
                                    // model that names a Unique_Ground_Unit (+0x1b60) is replaced by it (a1h t06, §4.2.2 item 2)
    std::vector<float> hx, hy;      // each member's half extents (0x371be0)
    bool create_team = false;       // the FIRST ORIGINAL member's Create_Team (+0x67): infantry teams, else one vehicle grid
    uint32_t squad = 10;            // the company's Max_Squad_Size (+0x1e44)
    bool container_locomotor = false;  // Create_Team_Type has LAND_TEAM_CONTAINER_LOCOMOTOR in its LandBehavior (+0x718)
    float formation_spacing = 0.0f;    // the first (substituted) member's FormationSpacing (+0x16d4)
};
struct LandDropSlot {
    uint32_t type = 0;
    float x = 0.0f, y = 0.0f;    // world position
    float hx = 0.0f, hy = 0.0f;  // the member's half extents
    float yaw_deg = 0.0f;        // the slot box's axis: yaw of the drop point -> slot (0x20acd0)
};
struct LandDropLayout {
    std::vector<LandDropSlot> slots;
    // a1i t04: the unload exit targets (0x643130, world): per team its grid offset (Create_Team companies), else per member
    // its slot. One entry per unload step.
    std::vector<float> group_x, group_y;
    float half_x = 0.0f, half_y = 0.0f;  // broad-phase formation box half extents (world, along the drop yaw)
    bool ok = false;                     // false: no member, or a degenerate grid (b = 0)
};
// The slots of `shape` dropped at (px, py) with the layout turned by `yaw_deg` (the player's entry facing): the member
// grid (0x40f970) for vehicles; for Create_Team companies teams of min(Max_Squad_Size, n) laid out by 0x5abc00 (or
// 0x40f970), the teams on a 0x40f970 grid from `team_factor` x the team bounds, each team turned to face its offset. All
// float. The drop validity (0x3b6170) uses 2.5; the landing object's unload offsets (0x643130, A1_READINGS §35.2) 3.0.
LandDropLayout land_drop_layout(const LandDropShape& shape, float px, float py, float yaw_deg, float team_factor = 2.5f);
// 2-D oriented boxes (centre, half extents, yaw in degrees); true when they overlap (separating-axis test, touching
// boxes do not overlap).
struct DropBox {
    float cx = 0.0f, cy = 0.0f, hx = 0.0f, hy = 0.0f, yaw_deg = 0.0f;
};
bool drop_boxes_overlap(const DropBox& a, const DropBox& b);
// The reinforcement radii of one slot (TACTICAL_GAPS §4.2.1 test 4, check-radius drops): with Use_Reinforcement_Points
// some allied object with Reinforcement_Enabling_Radius R > 0 lies within d <= R; no non-allied object with
// Reinforcement_Prevention_Radius P > 0 lies within d < P, except a POWERED one whose owner has no base power.
struct LandDropRadiusObject {
    float x = 0.0f, y = 0.0f;
    bool allied = false;
    float enabling = 0.0f, prevention = 0.0f;
    bool powered = false;           // the object has the POWERED behaviour
    bool owner_base_power = false;  // its owner's base-power list is non-empty (TACTICAL_GAPS §7.3)
};
bool land_drop_radius_ok(float x, float y, std::span<const LandDropRadiusObject> objects, bool use_reinforcement_points);
// The drop shape of a company type (its Company_Units in list order, or the type itself) and the half extents 0x371be0
// of a type with the land model (s78n t10, TACTICAL_COMBAT §5.4.6 item 1): Custom_Hard_X/YExtent x Scale_Factor when both
// are > 0; else no Land_Model_Name -> (0, 0); else per axis the hard extent when > 0, else the |collision-bound half x /
// y| of the land model, x Scale_Factor ([port] a named model that does not load: the hard extent, else 0).
LandDropShape land_drop_shape(const objects::GameObjectTypes& types, const BattleCatalog* catalog, const galactic::ConflictRules& rules,
                              uint32_t company_type);
void drop_half_extents(const objects::GameObjectTypes& types, const BattleCatalog* catalog, uint32_t type, float& hx, float& hy);
// a1i t05 (A1_READINGS §35.4 item 1): the deployed half extents 0x371d20: with Custom_Hard_XExtent_Deployed > 0, x = it x
// Scale_Factor and y = Custom_Hard_YExtent_Deployed x Scale_Factor (even when <= 0); else drop_half_extents.
void drop_deployed_extents(const objects::GameObjectTypes& types, const BattleCatalog* catalog, uint32_t type, float& hx, float& hy);
// a1i t05 (A1_READINGS §35.4 item 4): Find_Free_Placement 0x2a9670 in the plane. `local` = the placed type's model box
// (model space, no Scale_Factor [M]); step = 1.2 x its larger x / y span. Rings r = 0, step, 2 step, ... while r <
// `radius`, per ring the angles 0, 22.5, ..., 360 degrees inclusive (r = 0 tested once): the first Q = P + r (cos a, sin a)
// whose box Q + local overlaps no blocker in x and y (touching boxes do not overlap [M]) -> (out_x, out_y), true. None ->
// false (the caller 0x2bf2d0 then uses P). A zero step tests r = 0 only.
struct FreePlacementBox {
    float min_x = 0.0f, min_y = 0.0f, max_x = 0.0f, max_y = 0.0f;
};
// q6 (OBJECT_SCRIPTS §10.4 step 5): `start_deg` turns the ring angles (start, start + 22.5, ..., start + 360); `accept`
// (land: the candidate's cell is pure Clear) must also pass for a candidate to be taken.
bool find_free_placement(float px, float py, const FreePlacementBox& local, std::span<const FreePlacementBox> blockers, float radius,
                         float& out_x, float& out_y, float start_deg = 0.0f,
                         const std::function<bool(float x, float y)>& accept = {});
// ---- end a1g t04 follow-up ----
// S7 t12 / t22: the space fighter locomotor's formation attack target (FIGHTER_FLYING §2 step 6 / §4.3 / §4.9,
// Formation_Attack_Target(F, &hp, -1) 0x41a2e0, TACTICAL_COMBAT §2.8) for a squadron leader or a fighter without a
// squadron, from its combat state and its formation record (rules: TacticalBattle::feed_fighter_target). Pure;
// kNoObjectId = no target. No fog test: a fogged goal is still the goal (§4.9 step 3, the Idle <-> Directed_Combat toggle).
struct FighterTargetInputs {
    objects::ObjectId ordered = kNoObjectId;  // live ordered combat target [port: the order's object block / team target]
    objects::ObjectId pushed = kNoObjectId;   // the newest live, hostile auto attack block of the formation (§4.9 step 2)
    objects::ObjectId goal = kNoObjectId;     // live attack goal (the order's object block: Attack_Move(object))
    bool goal_hostile = false;                // goal owner is an enemy of the fighter's owner
};
objects::ObjectId fighter_formation_target(const FighterTargetInputs& in);

// S7 t22 / t25: the formation response-range test 0x4160a0(F, u, c) (FIGHTER_FLYING §4.10 A, TACTICAL_COMBAT §2.8
// "Targets of opportunity") for an auto target c of a squadron u (or a fighter without one). Pure. Returns 0 (no chase),
// else 1 / 2 (chase).
//   1. 0 without a formation, or when c is the object of the formation's first or last block (the goal: path B);
//   2. Rr from the first block's flags: 0x02 Guard_Chase_Range, else 0x01 Attack_Move_Response_Range, else
//      Idle_Chase_Range (the squadron type's; FoC X-wing squadron 1000 / 300 / 200); Rr < 0 -> 0;
//   3. c attacks the guarded object or the squadron [port: its combat target is one of them; retail: c is in their
//      damage-tracking attacker lists]: Rr += Autonomous_Move_Extension_Vs_Attacker, base 1. While a guarded object is
//      moving, any other c -> 0 (a moving escort answers only the carrier's attackers);
//   4. the formation still moving on an attack-move block: 2 [M: within Rr of its own path point, always true here];
//   5. otherwise the last block must be a guard, or the formation arrived at the end of its only block, a position
//      (F +0x240): else 0 - a squadron on a plain position move, or one already chasing a pushed auto block, gets no
//      response (§4.10 A corrects §4.9's "Idle_Chase_Range from the destination while moving");
//   6. not arrived: u (the leader) must lie within Rr (XY) of the slot goal, else 0;
//   7. c within Rr of the slot goal -> 1; within Rr + the attack range -> base + 1 [M: 0x5672b0 "attackable from the point
//      Rr toward c"]; else 0.
// [port] not modelled: the deployed / order-locked / space-layer refusals (fighters and squadrons have no Space_Layer in
// FoC), the last-block flag 0x40 / byte +0x5e, the guarded block's flag 0x10.
struct FighterChaseInputs {
    bool formation = false;          // F (locomotor pack +0x1f0) exists
    uint8_t flags = 0;               // first block flags: 0x01 attack-move, 0x02 guard
    bool last_guard = false;         // the last block is a guard (no live pushed auto block after a guard order)
    bool single_position = false;    // arrived at the end of its only block, a position (F +0x240 = 1)
    bool moving = false;             // the formation's move is under way (not arrived)
    bool candidate_is_goal = false;  // c is the object of the first or last block
    bool candidate_attacker = false; // c attacks the guarded object or the squadron
    bool guarded_moving = false;     // the guard block's object is moving
    float guard_range = 0.0f, idle_range = 0.0f, attack_move_range = 0.0f;
    float extension = 0.0f;          // Autonomous_Move_Extension_Vs_Attacker of the squadron type
    float attack_range = 0.0f;       // Get_Max_Attack_Range of the squadron leader (or the fighter)
    Vec3 goal;                       // the formation's slot goal
    Vec3 unit;                       // u's position (the squadron leader)
    Vec3 candidate;                  // c's position
};
uint8_t fighter_chase_test(const FighterChaseInputs& in);

// S7 t22: a squadron's (or a fighter's without one) formation as the host keeps it (FIGHTER_FLYING §4.9) [port: the
// retail coordinator / formation objects are not modelled]. Created by every move / attack order of the squadron (all
// order paths: human, Lua / AI, hangar launch (S7 t24: a carrier escort is a guard of the carrier: flags 0x02, goal object
// = the carrier, followed by the movement layer's formation, mv::FighterFormation (S7 t37 / s78f t02), whose guard
// block's recorded goal and object loss the record mirrors; a station launch a move to the flyout point), stand-in
// orders); replaced by the next
// order; removed by Stop and when the squadron is gone. [M, §11 open item "lifetime of a squadron's formation after its
// move ends"] It survives the end of the move and enter_idle (the retail formation goal survives enter_idle, §4.7, and Idle_Chase_Range is measured
// from an arrived formation's slot goal, TACTICAL_COMBAT §2.8 rule 5), so an idle squadron that was ever ordered still
// chases auto targets in its idle chase range; a squadron that never had an order (map start, reinforcement) has none and
// does not chase (its weapons fire at targets in range).
struct FighterFormationRecord {
    objects::ObjectId key = kNoObjectId;          // the squadron container's id, or the fighter's own
    uint8_t flags = 0;                            // the order's block flags (0x01 attack-move, 0x02 guard)
    Vec3 goal;                                    // slot goal: the order's destination (an object order: its position then)
    objects::ObjectId goal_object = kNoObjectId;  // object block (Attack / Attack_Move / Guard of an object): the goal follows it
    // s78f t02: the auto attack blocks (0x41cf70) live in the movement layer's block stack (MovementSystem::fighter_push,
    // FighterBlock::pushed); TacticalBattle::fighter_pushed reads them (the S7 t22 `pushed` list is gone).
    // s78f review: while the movement formation exists, flags / goal / goal_object follow its CURRENT block that is not an
    // auto block (TacticalBattle::fighter_order_view, refreshed by fighter_sync): the order's own block keeps the order's
    // values (a guard stays a guard of its object while it is on top), a queued waypoint (0x08) below it takes over once
    // the order's block is popped.
};

// s78f review (FIGHTER_FLYING §4.11 B, 0x4160a0): the order block the host's chase tests read. `first_flags` = blocks[0]'s
// flags (0x4160a0 rule 2 and 0x4165f0 pick Rr from the FIRST block: Guard_Chase_Range only when the bottom block is a
// guard, so a guard with a shift waypoint queued below it reads the waypoint's range, as the movement leash does); `flags`,
// `goal`, `goal_object` = the current order block (guard status, the guarded object, the slot goal). 0x01 / 0x02 bits only.
struct FighterOrderView {
    uint8_t first_flags = 0;
    uint8_t flags = 0;
    Vec3 goal;
    objects::ObjectId goal_object = kNoObjectId;
};

class TacticalBattle {
public:
    // `pf` (OPT3 t09): the model loads of the battle tables' build run on it (results do not depend on it).
    TacticalBattle(const BattleCatalog& catalog, const galactic::GalacticSim& galaxy, const galactic::ConflictRules& rules,
                   galactic::BattleRequest request, BattleConfig cfg = {}, const ParallelFor& pf = serial_for);
    ~TacticalBattle();
    // OPT3 t12: loads, through `pf`, the catalog's model data (bones, avoid models, land stamp extents) of the types a map
    // battle on `map` is likely to use: the forces' spawn types, the map's placement types and their pads' buildable /
    // constructed types. Called before the battle's first tick (CampaignBattles::start_map_battle, at session load), so the
    // setup finds them cached. The cache is a pure memo of the model files: nothing the battle computes depends on it.
    static void prepare_models(const BattleCatalog& catalog, const galactic::GalacticSim& galaxy, const galactic::ConflictRules& rules,
                               const galactic::BattleRequest& request, const terrain::MapData& map, const ParallelFor& pf);
    TacticalBattle(const TacticalBattle&) = delete;
    TacticalBattle& operator=(const TacticalBattle&) = delete;

    // S10-int: sim-owned animation (SimAnimation::world of the tactical mode). Call before setup; null = none.
    void set_animation(SimAnimation* anim) { anim_ = anim; }
    // refixf t04: model / DeathBehavior source of the death-clone plans when no animation service is set (an unattached
    // SimAnimation, e.g. a session cache or a test's injected models; must outlive the battle). Null = the battle makes its
    // own from the catalog's files. With set_animation the service's own models are used (same files, same answer).
    void set_clone_models(SimAnimation* models) { clone_models_ = models; }
    // refixf t04: death clone objects still in the store (removal pending; forever ones until the battle ends).
    size_t death_clones_pending() const { return death_clones_.size(); }
    // Spawns the battle into `mode` (pushed, nothing in it yet). `players` = the host's player count.
    void setup(TacticalMode& mode, terrain::MapData map, SimRng& rng, uint32_t players, const ParallelFor& pf);
    // One battle tick (after the tactical mode tick). Sets finished() when a victory condition holds.
    // `global_frame` = the host frame (animation time base).
    void tick(TacticalMode& mode, SimRng& rng, const ParallelFor& pf, double sim_time, uint64_t global_frame = 0);

    // S10-int command phase: ability commands 43 SetUnitAbilityMode, 40 TacticalSpecialAbility,
    // 57 TacticalSpecialAbilityWithDummyTarget (spawns a Dummy_Barrage_Target), 58 SetAbilityAutofire on the
    // issuing player's objects (missing / foreign ids skipped). Other events: Unhandled.
    commands::RejectReason handle_command(TacticalMode& mode, uint8_t player, const commands::EventData& ev, SimRng& rng);
    // S10-int Change_Owner of a battle object (Lua ObjectBridge::change_owner on tactical hosts): store owner,
    // TreeSet::set_owner, ability / combat owner, the unit's side. False when not a live battle object.
    bool change_owner(TacticalMode& mode, objects::ObjectHandle h, int32_t owner);
    // OI host (L1/S4 wave 1 integrator ask 2): ObjectBridge::change_owner of every tactical VM (story and AI): change_owner()
    // for a battle object, else (props, objects outside the battle tables) store owner + TreeSet::set_owner
    // (ObjectOwnerChanged) + the fog / garrison owner hooks. False when the object is dead or the owner negative.
    bool script_change_owner(TacticalMode& mode, objects::ObjectHandle h, int32_t owner);

    // --- L1/S4 wave 2b story hooks (owner: L1/S4 agent; keep other hooks separate) ---
    // Reserve units deployed during the last tick() (StorySession STORY_REINFORCE), deploy order.
    std::span<const objects::ObjectId> story_reinforced() const { return story_reinforced_; }
    // S7 t15 A1 - "unit ability finished" (object signal 0x29) raised during the last tick(), in order: ability ends
    // (AbilitySystem Ended: countdown expiry, special removals, the Remote_Bomb toss at its Toss_Anim end) and the jetpack's swap
    // back to the primary locomotor (MOVEMENT §3.1.1 step 6, seen as the end of the weapons suspension before the combat
    // tick, or at the end of the tick when the swap runs after that point: S7 t27). The tactical AI and the story bridge read it.
    std::span<const objects::ObjectId> ability_finished_events() const { return ability_finished_; }
    // S7 t31 - object signal 0xb "a new order replaces this unit's order" (AI_TACTICAL §7.3.3 item 2, A1_READINGS §33.3),
    // raised synchronously on each unit an order takes: the human / story orders (order_move / _facing / _attack /
    // _move_to_object / _stop, garrison enter / exit), ability commands 40 / 43 / 57 of the 0xb ability ids (before
    // executing; also on the TEAM members), the tactical AI's executed orders (BattleAi) and Lua Activate_Ability
    // (ObjectBridge::raise_order_replaced). Listeners (the tactical AI world, the story bridge) get the unit id and add its
    // TEAM container themselves. Not saved: nothing is pending between calls.
    using OrderReplacedListener = std::function<void(objects::ObjectId)>;
    uint32_t add_order_replaced_listener(OrderReplacedListener fn);
    void remove_order_replaced_listener(uint32_t token);
    void raise_order_replaced(std::span<const objects::ObjectId> ids);
    void raise_order_replaced(objects::ObjectId id) { raise_order_replaced(std::span<const objects::ObjectId>(&id, 1)); }
    // True for the ability ids whose command raises 0xb on the unit before executing (0x570a50 list, §7.3.3 item 1).
    static bool ability_raises_order_replaced(uint8_t ability);
    // a1i t04 (A1_READINGS §35.3): signal 0x19 (ReinforcementsUnloadedDataClass) of the land landing objects that finished
    // their unload during the last tick() or the commands before it (cleared when tick() starts): the landing object, the
    // reserve's galactic id, the owner, the carried company type and the unloaded units in spawn order. No listener yet.
    struct LandingUnload {
        objects::ObjectId landing = objects::kNoObjectId;
        objects::ObjectId galactic_id = objects::kNoObjectId;
        int32_t owner = -1;
        uint32_t carried_type = 0;
        std::vector<objects::ObjectId> units;
    };
    std::span<const LandingUnload> landings_unloaded() const { return landings_unloaded_; }
    // q6 t19 (SKIRMISH_SETUP §3 / §3.1): a finished land tactical build (not DUMMY_UPGRADE) never spawns: 0x27fad0 appends
    // the type to the player's reinforcement pool (a reserve participant with no galactic counterpart, built = true, key
    // kBuiltReserveKeyBase + its participant index) and signal 0x22 tells the build task it completed. The records of
    // the tick just run (transient, not saved), in completion order.
    static constexpr objects::ObjectId kBuiltReserveKeyBase = 0x01000000;
    struct ProductionDone {
        int32_t player = -1;
        uint32_t type = 0;
        objects::ObjectId key = objects::kNoObjectId;  // the pool entry, kNoObjectId when the pool refused the type
    };
    std::span<const ProductionDone> production_done() const { return production_done_; }
    // Appends `type` to `owner`'s pool (above). False (nothing added) when the battle finished or a type it spawns is not
    // in this battle's tables. Returns the key through `key`.
    bool add_built_reserve(int32_t owner, uint32_t type, objects::ObjectId& key);
    // a1i t04: a landing object still waiting for its footprint to clear (state 4: model hidden) [port: no renderer reads
    // it yet]; the landing objects of `player` that have not finished their unload (timed mode; untimed ones finish at the
    // order).
    bool landing_hidden(objects::ObjectId landing) const;
    uint32_t landings_in_flight(int32_t player) const;
    // The Dummy_Barrage_Target of a position-targeted ability for `player` (command 57; Lua Activate_Ability with a
    // position through ObjectBridge::spawn_ability_target). Returns its id, kNoObjectId when the type is missing.
    objects::ObjectId spawn_dummy_target(TacticalMode& mode, int32_t player, Vec3 at, SimRng& rng);
    // --- end L1/S4 wave 2b ---

    // --- S4 garrisons, structure capture, story feeds (owner: S4 agent; tactical_battle_garrison.cpp) ---
    // docs/re/L1_S4_REMAINING.md §3.5 / §4.1 / §4.2 (rules in engine/tactical/garrison garrison.hpp / capture.hpp).
    // Garrison hosts (GARRISON_VEHICLE / STRUCTURE / HOVER, CanContainGarrison) and occupants (GARRISON_UNIT) are
    // registered at deploy; map placements with CAPTURE_POINT or a garrison behaviour and no locomotor are spawned
    // (owner: the galaxy's neutral player [port]) and registered. Commands Garrison (52: unit + host = enter, unit +
    // 0x3FFFFF = leave, 0x3FFFFF unit + host = eject all) and MoveToGarrison (55: a = host, b / c = units [port field use])
    // reach handle_command. Each tick (after the combat events): dead / retreated hosts eject (or kill), approaches /
    // entering units advance (movement removed, kUnitGarrisoned, position = the host's, fire range x garrisoned
    // multiplier, or no fire without GarrisonCanFire [port: FireRange -1000]), exits re-enter movement at the exit point;
    // capture points change owner (store, trees, battle side). Story feeds (this tick, cleared at the start of tick()):
    struct StoryGarrisonEvent {
        objects::ObjectId unit = objects::kNoObjectId, holder = objects::kNoObjectId;
    };
    struct StoryCaptureEvent {
        objects::ObjectId object = objects::kNoObjectId;
        int32_t old_owner = -1, new_owner = -1;
    };
    std::span<const StoryGarrisonEvent> story_garrisoned() const { return story_garrisoned_; }
    std::span<const StoryCaptureEvent> story_captured() const { return story_captured_; }
    // Garrison API for the Lua bindings (Can_Garrison / Garrison / Leave_Garrison / Eject_Garrison / Can_Garrison_Fire /
    // Is_In_Garrison / Get_Garrisoned_Units). Orders: `player` = the units' owner (script orders pass the unit's owner);
    // units must be live battle units of that player. can_garrison = the Lua Can_Garrison rule (L1_S4_REMAINING.md §2.5:
    // free - reserved - value > 0). order_garrison (Lua Garrison / command 55 / 52): units with matching masks walk to
    // the host's approach point and enter after the entering fade; InvalidTarget (not a host / hostile) / InvalidUnit
    // (none can). order_leave_garrison: units inside leave now (an approach is cancelled). eject_garrison: every
    // occupant leaves (Eject_Garrison).
    bool can_garrison(objects::ObjectId unit, objects::ObjectId holder) const;
    bool can_garrison_fire(objects::ObjectId id) const;
    objects::ObjectId garrison_holder_of(objects::ObjectId unit) const;  // kNoObjectId when not inside
    // Read-only "contained by" query for the renderer snapshot (render sim_snapshot.hpp apply_contained): the garrison host
    // of a battle object, kNoObjectId when it is not contained (transports are not modelled in battles).
    objects::ObjectId contained_by(objects::ObjectId id) const { return garrison_holder_of(id); }
    std::vector<objects::ObjectId> garrisoned_units(objects::ObjectId holder) const;  // entry order
    commands::RejectReason order_garrison(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                          objects::ObjectId holder, SimRng& rng);
    commands::RejectReason order_leave_garrison(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units);
    uint32_t eject_garrison(TacticalMode& mode, objects::ObjectId holder);
    const tactical::garrison::GarrisonSystem& garrison() const { return garrison_; }
    const tactical::garrison::CapturePointSystem* captures() const { return captures_.get(); }
    // --- end S4 ---

    // --- integration fixes: human order API (alamo_game battle_orders.cpp) ---
    // Player commands on the battle's MovementSystem / CombatSystem with the rules BattleAi applies to the tactical
    // AI's orders (battle_ai.hpp). `units`: object ids in selection order; only live, non-retreated battle units
    // owned by `player` (store owner) count, others are skipped, a repeated id counts once. Returns None when at
    // least one unit was ordered; NotAllowed once finished(); InvalidPosition (non-finite point), InvalidTarget
    // (missing / dead / retreated / non-enemy target), InvalidUnit (no eligible unit). Call in the command phase
    // (between ticks) with the synced RNG; nothing is ordered on a reject.
    //  order_move: one MovementSystem::move of the movable units to (x, y) clamped to the mode bounds (formation
    //    move, optional facing in degrees); attack orders cleared unless `attack_move` (CombatSystem keeps
    //    targeting on its own, BattleAi's LuaOrder AttackMove).
    //  order_attack: CombatSystem::order_attack on each unit with a weapon (hardpoint_type >= 0: the target's first
    //    targetable hard point of that HardPointType, else the system's choice); a movable attacker further than
    //    80 % of max(attack range, 100) moves to 70 % of it on the line to the target.
    //  order_move_to_object: MoveToObject / Escort / Guard [port: MovementSystem has no follow / guard state]: an
    //    order_move to the target object's current position (the target itself is never moved).
    //  order_stop: MovementSystem::stop on the movable units, attack orders cleared, order queues cleared.
    // `queued` ([port: Gabriel] queueable unit actions, the command's EventData::queued()): false = the units'
    // order queues are cleared and the order runs at once (as before); true = appended after each unit's current
    // order. A unit that is idle with an empty queue starts a queued order at once. See "order queue" below.
    // `move_flags` (P1_DOUBLE_CLICK_MOVE.md §3.2): the move event's 8-bit flags, copied unchanged into MoveOrder::flags
    // (0x04 player order, 0x01 attack-move, 0x02 guard-move, 0x08 waypoint append, 0x80 double-click); AI / Lua moves 0.
    // `switch_locomotor` (S7 t10, MOVEMENT §3.1.1 / §6.2): the MoveToPosition 1-bit field. The units with
    // Uses_Multiple_Locomotors swap to their secondary (jetpack) locomotor first (MoveOrder::switch_locomotor; a queued
    // order swaps them at once and queues the move). Parity only: no FoC sender sets it.
    commands::RejectReason order_move(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units, float x, float y,
                                      SimRng& rng, float facing_deg = tactical::movement::kNoFacing, bool attack_move = false,
                                      bool queued = false, uint8_t move_flags = 0, bool switch_locomotor = false);
    // Facing event (id 20, P1_DOUBLE_CLICK_MOVE.md §2.2: a land drag that started on a selected unit; rotate only) [port:
    // MovementSystem has no turn-in-place order]: one single-unit move per eligible movable unit (selection order) to its
    // own position with final facing `facing_deg`; attack orders and order queues are kept.
    commands::RejectReason order_facing(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units, float facing_deg,
                                        SimRng& rng);
    // `hardpoint` = a hard point index of the target (CombatSystem::order_attack; -1 = the system's choice).
    commands::RejectReason order_attack(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                        objects::ObjectId target, SimRng& rng, int32_t hardpoint = -1, bool queued = false);
    commands::RejectReason order_move_to_object(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units,
                                                objects::ObjectId target, SimRng& rng, bool attack_move = false, bool queued = false);
    commands::RejectReason order_stop(TacticalMode& mode, uint8_t player, std::span<const objects::ObjectId> units);
    // The target's first targetable hard point of HardPointType `type` (Attack ref2 = 1 + type), -1 when none.
    int32_t first_hardpoint_of_type(objects::ObjectId target, int32_t type) const;
    // True when `player`'s units are left to commands (BattleConfig::skip_human_units and a human galactic player).
    bool commanded_by_player(int32_t player) const;
    // OI follow-up t03 (A1_READINGS §28.9): movement locked (0x39ad40) = the abilities' share of the obj +0x3a8 counter,
    // or a share a Lua bridge reports (Lock_Current_Orders, a running Divert). The lock never changes speed: a group move
    // (player commands, AI orders, the stand-in) leaves locked units out and they keep their current order; ability
    // moves (CONFUSE, HUNT, JET_PACK) are not filtered.
    bool movement_locked(objects::ObjectId id) const;
    // Registered per bridge owner `key` (an empty fn removes it; the battle outlives its AI / story bridges).
    void set_script_movement_lock(const void* key, std::function<bool(objects::ObjectId)> fn);
    bool script_movement_locked(objects::ObjectId id) const;  // the bridges' shares only
    // s78n t06 review: the hero clash's share of the lock (+0x3b4, HERO_CLASH §3.2) while it is held. release_hero_clash_lock
    // = a Lua Unlock_Current_Orders through the hero's land TEAM object (A1_READINGS §28.9 item 4: retail's decrement of
    // the member's counter): the hero's movement orders are free again while +0x3b4 stays set (until the clash release
    // clears both). False when the clash holds no share.
    bool hero_clash_locked(objects::ObjectId id) const;
    bool release_hero_clash_lock(objects::ObjectId id);
    // cu8l t02 (CU8K_T02_T06_RE §3 rule 6): a Lua Lock_Current_Orders that took the obj +0x3a8 counter 0 -> 1 (the bridge's
    // script share, lua::TacticalScriptHooks::movement_first_lock): the first lock's split / adopt (at most one draw of
    // the battle's synced RNG, at the call).
    void script_first_lock(TacticalMode& mode, objects::ObjectId id);
    // --- end human order API ---

    // --- P host (owner: game host agent): human reinforcements ---
    // The undeployed reserve participants of `player` (participant order): the tactical HUD's reinforcement pad.
    struct Reserve {
        objects::ObjectId galactic_id = objects::kNoObjectId;  // Reinforce event "unit"
        uint32_t type = 0;
        float population = 0.0f;
        uint32_t units = 0;  // tactical objects it spawns
    };
    std::vector<Reserve> reserves(int32_t player) const;
    // Tactical HUD population (ui ask 7, presentation reads): the population of `player`'s live, not retreated deployed
    // participants (the reinforcement cap rule's count) and the space unit cap (Space_Tactical_Unit_Cap); land: -1 (no
    // tactical cap in the S10 battle).
    float population_used(int32_t player) const;
    float population_cap(int32_t player) const;
    // World unit icons (ui ask 4, presentation read; QA U7): every live, not retreated, movable unit with the id standing
    // for its marker - the first such unit (ascending id) of its team container, else of its participant. A hangar-launched
    // squadron's members are one participant each, so the container decides. Ascending unit id.
    struct IconLink {
        objects::ObjectId unit = objects::kNoObjectId, squad = objects::kNoObjectId;
    };
    std::vector<IconLink> icon_links() const;
    // Reinforce (COMMANDS.md id 12) from a human player: the rules of BattleAi's Reinforce (owner, undeployed reserve,
    // the side not retreating, space unit cap -> QueueFull). Space: the members by the free-placement ring search
    // (space_member_spots, a1i t05), else in rows of 6 around (x, y) clamped to the mode bounds [port]. Land (a1h t06, TACTICAL_GAPS §4.2.2 item 5: ReinforceEventClass Execute 0x403cb0): the drop
    // validity is re-run (land_drop_valid, movers block only for a human issuer); a refused drop is refused whole
    // (InvalidPosition: nothing moved or clamped, the unit stays in the pool); else the members spawn at the layout's
    // slots. a1i t04 (A1_READINGS §35): a company with a Company_Transport_Unit lands in a landing object (its
    // Landing_Transport_Variant, TRANSPORT_LANDING, carried type = the company) at (x, y); the members spawn at the unload
    // offsets (team grid 3.0 x the team bounds) and the landing object leaves after the unload steps' timers
    // (BattleConfig::landing_unload_timed: the company stays inside and landing_service unloads it step by step). a1i t05
    // (§35.4 item 3): a campaign battle refuses (NotAllowed) a type with Stay_In_Transport_During_Ground_Battle or one the
    // battle planet restricts; the "battle decided" gate stays [M]. Command phase, synced RNG. InvalidUnit when
    // `galactic_id` is not one of the player's reserves.
    commands::RejectReason order_reinforce(TacticalMode& mode, uint8_t player, objects::ObjectId galactic_id, float x, float y,
                                           SimRng& rng);
    // a1g t04 follow-up: land_drop_shape / drop_half_extents with this battle's data. a1h t06: the layout yaw of a side =
    // its entry-facing record (mode +0x288, TACTICAL_GAPS §4.2.2 item 1): the yaw of the defender's first Defending Forces
    // marker, of the chosen Attacker Entry marker (only with attacker start units), else 0.
    LandDropShape land_drop_shape(uint32_t company_type) const;
    void drop_half_extents(uint32_t type, float& hx, float& hy) const;
    float drop_yaw(uint8_t side) const;
    // a1g t04 follow-up / a1h t06 - land drop validity (TACTICAL_GAPS §4.2.1, §4.2.2; LandMode vf80 0x3b5f60 -> 0x3b6170,
    // force 0, check-radius): the reserve entry `key` of `player` (not deployed) laid out by land_drop_layout around (x, y),
    // turned by the side's drop yaw. True only when EVERY slot passes: inside the playable box [port] and passable for its
    // (substituted) member's MovementClass; not fogged for `player` (fog builds); its oriented box clear of every blocking
    // object's footprint box (after the formation-box broad phase); the reinforcement radii (land_drop_radius_ok;
    // `use_reinforcement_points` = GameConstants Use_Reinforcement_Points). Blocking objects = the object tree GOM +0xd8: live objects of any owner whose type has
    // Collidable_By_Projectile_Living (death clones: _Dead), minus PROXIMITY_MINE objects; an AI issuer (`human` false)
    // ignores movers (a locomotor and no LAND_OBSTACLE) and counts landing transports; a human issuer counts movers and
    // ignores TRANSPORT_LANDING objects except persistent ones (Create_Cinematic_Transport persist, a1i t05). Footprint =
    // position + R(yaw)(Custom_Hard_X/YExtent_Offset x Scale_Factor), half extents drop_half_extents, or the deployed
    // extents (drop_deployed_extents) while a Deploys type with a locomotor is deploying / deployed (a1i t05, 0x396850
    // rule 2; s78n t10: Walk state 0x18 / 0x19).
    // Unknown entry -> false. Space battles -> true.
    bool land_drop_valid(TacticalMode& mode, int32_t player, objects::ObjectId key, float x, float y, bool human,
                         bool use_reinforcement_points) const;
    float player_drop_yaw(int32_t player) const { return drop_yaw(side_player(1) == player ? uint8_t{1} : uint8_t{0}); }
    // s78i t02 (TACTICAL_COMBAT §3.1.1 "other spawns"): the facing the reinforcement creators give `player`'s new objects:
    // its side's entry-facing record (mode +0x288), +X when the record is zero or the player has no side.
    Vec3 player_drop_facing(int32_t player) const;
    // --- end P host ---

    // --- [port: Gabriel] queueable unit actions: the per-unit order queue (docs/DECISIONS.md "Camera and controls") ---
    // Each battle object (unit or ability team container) has a queue of pending orders behind its current one.
    // A queued command (order_* with queued = true; ability commands 40 / 43 / 57 / 58 with EventData::queued())
    // appends one entry per eligible unit (one `group` serial per command); a plain command clears the queues of
    // its units; order_stop clears them. Each tick, after the combat events (tick step 5b), every unit whose
    // current order completed pops its next entry: Move / MoveToObject complete on arrival (no movement goal),
    // Attack when the target is dead / gone or its ordered hard point destroyed, Ability when no ability is
    // pending; before the first popped entry the current order is the unit's live state (moving, an ordered
    // attack on a live target, a pending ability). Entries popped in the same pass with the same group and player
    // start together (one formation move / one ability command, ascending id); entries whose target died are
    // dropped and the next one is tried (at most 8 passes per tick). Dead / retreated units lose their queue.
    // Serial code, synced RNG only: identical for any worker count. In the frame CRC ("battle") once any order
    // was queued, so battles without queued orders keep their CRC stream.
    struct QueuedOrder {
        enum class Kind : uint8_t { Move, Attack, MoveToObject, Ability };
        Kind kind = Kind::Move;
        uint8_t player = 0;
        uint32_t group = 0;                             // issuing command serial
        objects::ObjectId target = objects::kNoObjectId; // Attack / MoveToObject / Ability 40 target
        int32_t hardpoint = -1;                          // Attack: hard point index (-1 any)
        float x = 0.0f, y = 0.0f;                        // Move: destination (clamped); Ability 57: ground point
        float facing = tactical::movement::kNoFacing;    // Move
        bool attack_move = false;                        // Move / MoveToObject: attack orders kept
        uint8_t flags = 0;                               // Move: the event's movement flags (MoveOrder::flags)
        commands::EventData ability;                     // Ability: the command (users / units replaced at start)
    };
    // The pending orders of `unit` behind its current one, in run order (HUD queue markers). Empty when none.
    std::span<const QueuedOrder> queued_orders(objects::ObjectId unit) const;
    // --- end queueable unit actions ---

    // --- L1 systems (owner: tactical-systems agent; tactical_battle_l1.cpp) ---
    // The systems behind lua::TacticalScriptHooks (docs/re/SCRIPT_GAPS.md §1-§3): special weapons, lobbing superweapons
    // and the tactical superweapon (tactical::abilities::SpecialWeaponSystem), hyperspace departures, timed engine
    // disables and teleport (tactical::movement::HyperspaceDepartures / EngineControl / MovementSystem::teleport).
    //  * Objects: battle units and objects spawned later (adopt_object, e.g. Spawn_Special_Weapon through the script
    //    spawn hook) whose type has SPECIAL_WEAPON / LOBBING_SUPERWEAPON / TACTICAL_SUPER_WEAPON in the mode's behaviour
    //    set get a pack; their weapon projectiles are in the battle tables (the constructor adds the mode's special-
    //    weapon types: space Is_Special_Weapon_In_Space + Special_Weapon_Index >= 0, land LOBBING_SUPERWEAPON).
    //  * Tick: hyperspace jumps + engine re-enables right after the movement commit (step 4), special-weapon volleys
    //    (CombatSystem::launch_projectile from the weapon's position) and the TSW state machine after the combat events.
    //  * CRC: in hash_abilities only once a pack / jump / engine entry exists (battles without them keep their bytes).
    // Ids are battle-store object ids; every call is serial (command phase / Lua service / tick).
    float sw_readiness(objects::ObjectId weapon) const;                     // < 0: no special-weapon pack
    bool sw_begin_targeting(objects::ObjectId weapon);
    bool sw_fire_at_object(const TacticalMode& mode, objects::ObjectId weapon, objects::ObjectId target);
    bool sw_fire_lobbing_at(const TacticalMode& mode, objects::ObjectId weapon, Vec3 point);
    // SpecialWeaponsAllowed (0x527620): false once the battle planet is destroyed (in the galaxy, or by this battle's TSW).
    // cu8f t06: then the player's Enable_Special_Weapons list (AbilitySystem PlayerGate::SpecialWeapons): empty -> allowed
    // in a campaign battle, refused in a skirmish (BattleConfig::skirmish); non-empty -> its AND and
    // CombatSystem::special_weapon_hp_enabled(player) (HARDPOINT_DISABLED §4).
    bool special_weapons_allowed(int32_t player) const;
    int32_t tsw_state(objects::ObjectId tsw) const;                         // < 0: not a TSW
    bool tsw_fire(objects::ObjectId tsw);                                   // 0x42a200 incl. the planet checks
    bool tsw_planet_destroyed() const { return l1_planet_destroyed_; }      // the TSW destroyed the battle planet
    // TSW planet destruction (docs/re/TSW_PLANET_DESTROY.md): state 9 kills the target dummy (its Death_Explosions pick
    // for the neutral player) and latches the destroyed planet; the battle host then destroys the galactic planet in the
    // same tick (tsw_kill_owner() = the TSW owner on that tick, else -1; GalacticSim::tsw_destroy_planet) and, when the
    // leader test hit, calls set_tsw_story_victory(owner). State-10 end: every Death_By_TSW_Replacements entry at the
    // saved dummy transform (neutral). Victory hold (§2): while a TSW is in states 5..13 no victory check runs (retreat,
    // all destroyed, station / base, time limit); at the release the checks run once that frame, a pending story victory
    // first: (TSW owner, 0x10, the other side retreats).
    int32_t tsw_kill_owner() const { return l1_tsw_kill_owner_; }
    // story t06: adds the monitor record {0x10, player, 30 frames} and tests it (queued while held: it then fires at the
    // TSW's state-13 release). tsw_story_victory() = that player until the monitor declares (-1 none).
    void set_tsw_story_victory(int32_t player);
    int32_t tsw_story_victory() const { return l1_tsw_story_winner_; }
    bool victory_held() const { return victory_hold() > 0; }
    // --- story t06: tactical victory monitor (BT §3.1a; tactical_battle_victory.cpp) ---
    // Records (§2.5, registered at setup, countdown 210 frames): 0xb and 0xc for the attacker and the defender when their
    // faction is not Is_Neutral; 0xe for the attacker with Land_Base_Destruction_Forces_Retreat (land), 0xf for the attacker
    // with Space_Station_Destruction_Forces_Retreat (space); a skirmish map battle instead 0x13 (land) / 0x16 (space) per slot
    // player (SKIRMISH_SETUP §4). [port] 0xd and the AI 0x11 are not registered.
    // Tests (§3.1a item 2): 0xb only when a victory-relevant object (Victory_Relevant, else Land_ / Space_Victory_Relevant of
    // its own type) leaves player P - removal, death, owner change, a completed retreat - for every enemy E of P that is the
    // attacker or defender (ascending player id), then 0xe / 0xf; skirmish: 0x13 / 0x16 when the object is a base component.
    // E wins 0xb when no active victory-relevant object of an enemy of E is left (no behaviour, category or reserve test).
    // [port] losses are found at the end of each battle tick by comparing the relevant objects (id, owner) with the last
    // tick's: this tick's combat deaths first in death order, then the others by ascending id. A completed retreat releases
    // every hold (no replay) and tests 0xc at once. While held (count > 0) a test only queues {type, player} (no duplicates).
    // Declared: the record is copied, its countdown runs one per battle tick from the next tick; the battle finishes when
    // declared, the countdown <= 0 and the hold <= 0. Nothing is evaluated at frame 0 / 1, and a side that never had a
    // relevant object cannot lose by 0xb.
    // Story rewards (the host forwards them, BT §3.1a item 3): ENABLE_VICTORY (reward 0x1d): has_param && value == 0 -> hold
    // + 1; has_param && value != 0 -> release one level with replay, only while held; no Param1 -> nothing (value =
    // atoi(Reward_Param1)). VICTORY: player = the faction of Reward_Param1 (< 0: nothing; the host logs "Unable to find
    // faction %s to force victory"), short_silent = atoi(Reward_Param2) != 0; releases every hold without replay, adds {0x10,
    // player, 7 s (short: 1 s, silent)} and tests it (type 0x10 always passes). True when that test declared it.
    void reward_enable_victory(bool has_param, int32_t value);
    bool reward_victory(int32_t player, bool short_silent);
    // The hold counter (+0x5c; TSW fire / release included). Raise = max(0, h + 1); a TSW release = h - 1 (may go below 0).
    int32_t victory_hold() const;
    bool victory_declared() const { return vm_declared_; }
    uint8_t victory_declared_type() const { return vm_decl_.type; }     // monitor type (0x13 / 0x16 for skirmish)
    int32_t victory_declared_player() const { return vm_decl_.player; }
    bool victory_declared_silent() const { return vm_decl_.silent; }    // +0x15: no TEXT_WIN / LOSE_TACTICAL
    int32_t victory_countdown() const { return vm_countdown_; }
    size_t victory_queued() const { return vm_queue_.size(); }           // deferred {type, player} tests
    // ccpgt t02 (SKIRMISH_SETUP §4.2): BattleConfig::control_point_domination registers only {0x15, player} records (the slot
    // players on a skirmish map, else the attacker and the defender), so no other rule (elimination, retreat, base) can win.
    // 0x15 is tested only by the domination timer: a completed capture transition of a relevant point (CAPTURE_POINT with
    // Control_Point_Domination_Condition_Relevant) arms it for round(30 x Control_Point_Domination_Victory_Time_In_Secs) frames
    // (GameConstants, 30 s) when every other relevant point is allied (relation 0) with the new owner and its faction is
    // Is_Playable, else disarms it; each battle tick it counts down, at <= 0 it tests 0x15 for that player and disarms. E wins
    // 0x15 when its faction is playable, at least one relevant point exists and every one is allied with E (any owner, no
    // alive / Victory_Relevant test). The win then waits the record's 210-frame countdown. [port] the 30 / 15 / 5 s announcer
    // lines are the host's (presentation): control_point_timer() = frames left (-1 not armed), control_point_leader() = the
    // armed player (-1 none).
    int32_t control_point_timer() const { return vm_cp_armed_ ? vm_cp_timer_ : -1; }
    int32_t control_point_leader() const { return vm_cp_armed_ ? vm_cp_player_ : -1; }
    // --- end story t06 ---
    // Teleport (0x3a8f90): store, movement (stops the unit), combat, trees. False when the object is not alive.
    bool teleport(TacticalMode& mode, objects::ObjectId id, Vec3 position, Vec3 facing);
    // Locomotor vf +0x2a8(keep_parent_link, delay) (L1_S4_REMAINING §3.4): a live battle unit with a FIGHTER /
    // SIMPLE_SPACE locomotor stops at once and is killed delay + 90 frames later (STARSHIP_LOCOMOTOR: empty stub ->
    // false). keep_parent_link false (Start_Cinematic_Space_Retreat, Hyperspace_Away(false)): the galactic unit survives
    // (the battle counts it as retreated); true (Hyperspace_Away()): a death. False when not such a unit.
    bool hyperspace_out(objects::ObjectId id, int32_t delay_frames, bool keep_parent_link);
    // Are_Engines_Online: 1 / 0 for battle units with a locomotor (destroyed engines, timed disables), -1 otherwise.
    int32_t engines_online(objects::ObjectId id) const;
    // Locomotor_Set_Engines: disable for `seconds` (< 0 indefinitely); enable_engines = repair (not after destruction).
    bool disable_engines(objects::ObjectId id, float seconds);
    // cu8f t02 (CU8F_T01_T02_T07_RE.md §2.4): the energy-pool path (CombatHooks::engines_drained, inside Take_Damage). With
    // a locomotor: the timed disable (as disable_engines: expiry = max(old, now + int(30 t)), only while the engines are on
    // or a timed disable runs) and the current move stopped; then the moving, then the idle engine loop off (engine-loop
    // units). [port] the stop does not check the last waypoint's flag 0x10 [M]; signal 0x16 is not modelled.
    void energy_drain_engines(objects::ObjectId id, float seconds);
    bool enable_engines(objects::ObjectId id);
    // max(terrain height, water height) at (x, y) on land maps; space: -3e38 (no ground).
    float ground_height(float x, float y) const;
    // Frame count of clip `anim` (index 0) of the object's model in this mode (sim animation); 0 = unknown.
    uint32_t anim_clip_frames(const TacticalMode& mode, objects::ObjectId id, uint8_t anim) const;
    // Frame count and fps of clip `anim` (a ModelAnimType id, index `variant`) of the object's model (OI t03 ability timings:
    // static clip data, never the playing clock). False without a sim animation, model or clip.
    bool anim_clip_info(const TacticalMode& mode, objects::ObjectId id, int32_t anim, uint32_t& frames, float& fps,
                        uint32_t variant = 0) const;
    // Plays clip `anim` of the object from `start_fraction` at `speed` (Create_Cinematic_Transport Land / Takeoff;
    // sim animation only). False without a clip.
    bool play_clip(const TacticalMode& mode, objects::ObjectId id, uint8_t anim, float start_fraction, float speed, uint64_t global_frame);
    // Space_Retreat_Unit_Increment_Wait_Frames per galactic faction index (load_retreat_increments below); 0 when unset.
    void set_retreat_increments(std::vector<int32_t> by_faction) { l1_retreat_increments_ = std::move(by_faction); }
    // Space_Retreat_Flight_Move_Increment per galactic faction index (the post-jump step); 500 [port] when unset.
    void set_retreat_flight_increments(std::vector<float> by_faction) { l1_flight_increments_ = std::move(by_faction); }
    float retreat_flight_increment(int32_t player) const;
    int32_t retreat_increment_frames(int32_t player) const;
    // A script-spawned object joins the L1 systems (special-weapon / TSW packs). Call after TacticalMode::spawn.
    void adopt_object(TacticalMode& mode, objects::ObjectHandle h);
    // q6 object scripts: Spawn_Unit of a game-object script (ObjectBridge::spawn_unit). TacticalMode::spawn (trees), then
    // like an ability spawn (oi_spawn_unit): a combat / ability / movement unit, then the L1 systems (adopt_object). q6
    // t12: a type outside the battle tables joins them first (script_add_types), so every script-spawned unit is a real
    // battle unit (movement, combat, AI control, the owner's unit lists). An object with a container / attach parent is
    // only TacticalMode::spawn'd.
    objects::ObjectHandle script_spawn(TacticalMode& mode, const objects::SpawnParams& p);
    // q6 t12: Spawn_Unit of a COMPANY type (ObjectBridge::spawn_company): the company comes down like a reinforcement the
    // battle knows (deploy(): its spawn_types members as battle units, then the REMAINING_GAPS §3.4 TEAM containers), as a
    // built participant (no galactic object) of p.owner. Members at the reinforcement drop layout around p.position
    // (land_drop_layout with the side's drop yaw, each slot snapped to a passable cell of its member) [port: Spawn_Unit's
    // tactical member placement is not in docs/re]. Returns the first TEAM container, else the first member (null when the
    // type is not a company or nothing spawned). The types join the tables first (script_add_types).
    objects::ObjectHandle script_spawn_company(TacticalMode& mode, const objects::SpawnParams& p);
    // q6 t12: Lua Take_Damage(amount) on a battle combat unit (ObjectBridge::take_damage): CombatSystem::take_damage with
    // damage kind 0 (Debug: passes invulnerability, TACTICAL_COMBAT §3.6 step 3; OBJECT_SCRIPTS §6.1 step 4), no source,
    // at the battle frame; `cannot_die` (the script's Set_Cannot_Be_Killed) sets kUnitCannotDie for that hit. A kill is
    // processed at once like a combat death (process_death: store removal, death clone, team container, victory monitor
    // order) [port: outside the battle tick; the original's Die runs inside Take_Damage]. False when `id` is not a live
    // combat unit of this battle (the caller keeps its store-only path).
    bool script_damage(TacticalMode& mode, objects::ObjectId id, float amount, bool cannot_die);
    // q6 (OBJECT_SCRIPTS §10.2): Lua Make_Invulnerable(on) (ObjectBridge::invulnerable_changed). The INVULNERABLE behaviour:
    // on = CombatSystem kUnitInvulnerable (permanent, only damage kinds 0 / 13 pass, targeting unchanged); off = detached,
    // which also ends an Invulnerability-ability duration. A TEAM container passes it to each member, not to itself. False
    // when `id` is neither a combat unit of this battle nor a TEAM container.
    bool script_invulnerable(objects::ObjectId id, bool on);
    // q6 (OBJECT_SCRIPTS §10.3): Lua Hide's model test (ObjectBridge::has_model): the type of `h` names a model for this
    // battle's mode (Space_ / Land_Model_Name). [port] by type data; the original reads the object's model pointer.
    bool script_has_model(const TacticalMode& mode, objects::ObjectHandle h) const;
    // q6 (COMMANDS §2.4.1 step 3 / 4): Select All's battle tests of an owned SELECTABLE object `id` (the store tests are the
    // caller's, SimHost): false = skipped (its owner's retreat is running [port: every unit of that player], a TEAM member,
    // a garrisoned unit); a TEAM container appends its live, not retreated members (member order) to `members`.
    bool select_all_entry(objects::ObjectId id, int32_t owner, std::vector<objects::ObjectId>& members) const;
    // q6 (OBJECT_SCRIPTS §10.4 step 2): Spawn_Unit's facing at `p`: toward the map origin (ObjectBridge::spawn_facing).
    Vec3 script_spawn_facing(Vec3 p) const;
    const tactical::abilities::SpecialWeaponSystem& special_weapons() const { return l1_sw_; }
    const tactical::movement::HyperspaceDepartures& hyperspace() const { return l1_jumps_; }
    const tactical::movement::EngineControl& engines() const { return l1_engines_; }
    std::span<const tactical::abilities::SpecialWeaponEvent> special_weapon_events() const { return l1_sw_events_; }  // this tick
    // Space fields (space_fields.hpp; built at setup from the mode's field objects): kinds (kField* bits: asteroid 1,
    // ion storm 2, nebula 4) at a point, and the nearest field object of `mask` (kNoObjectId when none).
    uint32_t field_kinds_at(float x, float y) const { return l1_fields_.kinds_at(x, y); }
    objects::ObjectId nearest_field(float x, float y, uint32_t mask) const;
    // Is_In_* of one battle object (L1_S4_REMAINING §2.7; kind one kField* bit): 1 / 0, -1 when not alive. Team
    // containers: the bindings OR over the members (rule 1).
    int32_t object_in_field(objects::ObjectId id, uint32_t kind) const;
    const tactical::abilities::SpaceFieldIndex& space_fields() const { return l1_fields_; }
    // Field effects (L1_S4_REMAINING §3.3), serviced at the start of the combat object pass (phase D, ascending id):
    // NEBULA objects - entry cancels every active ability, lockout stamp (Nebula_Ability_Disable_Time); SHIELDED
    // objects - ion-storm stamp (shields down, DEFEND blocked for Ion_Storm_Shield_Disable_Time); ASTEROID_FIELD_DAMAGE
    // objects (Capital / Frigate / SuperCapital layers, moving) - per overlapping asteroid field one synced float(0, 1)
    // <= Asteroid_Field_Damage_Rate -> Take_Damage(Asteroid_Field_Damage) and one Asteroid_Damage_Hit_Particles pick.
    // GameConstants the L1 systems read (load_l1_constants; the defaults are the shipped FoC values [port]).
    struct L1Constants {
        float asteroid_field_damage = 20.0f, asteroid_field_damage_rate = 0.20f;
        float max_bombing_run_interval = 120.0f, min_bombing_run_interval = 120.0f;
        int32_t bombing_run_reduction_per_squadron = 0;
        float max_bombard_interval = 260.0f, min_bombard_interval = 260.0f;
        // cu9h t01: the skirmish "special weapons" option (0x140b154bc & 4, HUD_GAPS §9.2) gates skirmish bombing runs. [port] no
        // lobby: GameConstants MP_Default_Allow_SuperWeapons (shipped yes).
        bool skirmish_superweapons = true;
        // cu8l t03 (CU8K_T03_T08_T09_RE §1.9): GameConstants In_Game_Cinematics (GC 0x140b16dd2; shipped True), one of the
        // three gates of the start fleet's hyperspace arrival.
        bool in_game_cinematics = true;
        Vec3 bombardment_offset{0.0f, 0.0f, 600.0f};
        std::vector<int32_t> bombardment_distribution{0, 3, 1, 2, 3, 2, 0, 1, 2, 1, 3, 0, 1, 2, 0, 3, 0, 2, 1, 3, 3, 2, 1, 0};
    };
    void set_l1_constants(L1Constants c) { l1_k_ = std::move(c); }
    // Tactical weather (land; tactical::combat::WeatherSystem, L1_S4_REMAINING §3.2): scenario = the active
    // environment's Weather effect name (TEE mini 29; cu8g t02: the map-load pick), looked up in the data set here
    // (load_weather_data); started at the first battle tick (two synced draws, CU8G_T02_ENV_PICK_RE §4), serviced at the
    // start of each tick (before the fog decay); the accuracy row feeds CombatSystem::set_weather_accuracy.
    // Force_Weather = force_weather (one advance, one draw).
    void set_weather_data(std::shared_ptr<const tactical::combat::WeatherData> d) { l1_weather_data_ = std::move(d); }
    // s78n t06 (HERO_CLASH §2 / §6): HEROCLASH.XML for the land hero clashes (tactical::combat::HeroClashManager, ticked
    // after the weather service, land only). The First / Second hero type names resolve through the catalog here; none =
    // no clashes. The running clashes are saved with the Combat part and hashed with the battle (absent while none).
    void set_hero_clash_data(std::shared_ptr<const tactical::combat::HeroClashData> d);
    const tactical::combat::HeroClashManager& hero_clashes() const { return hero_clash_; }
    const tactical::combat::WeatherSystem& weather() const { return l1_weather_; }
    bool force_weather(SimRng& rng);
    // Sight factor of a battle object (Weather_Category of its type; 1.0 without weather): the fog's Land_FOW_Reveal_Range
    // x weather factor (FOG_OF_WAR §3.1.1; owner: fog agent). Weather FowDirty events this tick: weather().events().
    float weather_sight_modifier(objects::ObjectId id) const;
    // cu8g t02 / t03 (CU8G_T02_ENV_PICK_RE): the mode's active environment (index into the map's EnvironmentSet;
    // RtsScene +0xa0). setup() picks it after the map objects (bt_map_load: chunk 0x102 and its spawn-table draws) and
    // before every post-load step (pick_environment: one synced int(0, n - 1) when n != 1, "Cinematic" -> the next
    // record). Not saved: a loaded battle takes the map's start index (mini 25, clamped) without a draw (§2).
    // set_environment = mode vf25 (Lua Set_New_Environment): index := clamp(index, 0, n - 1), no draw; false (nothing
    // changes) when the map has no environments [port]. Land: the weather's re-init on its next service
    // (l1_weather_tick) sees the changed index and restarts with the new environment's scenario (2 synced draws when it
    // has phases; none -> weather off); an unchanged index re-inits nothing. Space: the index only (no weather). The
    // render host reads environment_index() and re-applies lighting / fog / bloom / shadow colour and land clouds / wind
    // (render::MapScene; the skydomes stay). Not hashed: its only game-logic effect is the land weather restart, which
    // the weather state and the RNG show on the next land tick.
    uint32_t environment_index() const { return env_index_; }
    bool set_environment(int32_t index);
    // Mode vf27 (0x2770e0, "random environment of a weather type"; no caller found in retail or the port): type <= 4 ->
    // the environments whose mini 13 (Environment::field_13) equals type, in index order; m = 1 -> that one, m >= 2 ->
    // one synced int(0, m - 1) (no "Cinematic" skip); m = 0 or type > 4 -> pick_environment over all n. False when the
    // map has no environments.
    bool set_environment_of_weather_type(uint32_t type, SimRng& rng);
    // 0x277030 (§1): n == 0 / 1 -> 0 without a draw; else i = synced int(0, n - 1), a record named exactly "Cinematic"
    // (case-sensitive) -> i + 1 wrapping to 0, no re-check.
    static uint32_t pick_environment(const terrain::EnvironmentSet& envs, SimRng& rng);
    // Damage statistics (L1_S4_REMAINING §2.4): DAMAGE_TRACKING objects accumulate the applied damage of the tick's
    // CombatSystem::damage_events (after multipliers [M]); every 30 frames (the behaviour interval, first service at
    // registration [port]) rate = FPS x accumulated / interval. TEAM containers sum their members. -1 = not alive.
    double rate_of_damage(objects::ObjectId id) const;     // damage per second (0 without a tracker)
    double time_till_dead(objects::ObjectId id) const;     // seconds; 1e18 without a tracker / rate 0
    // Final-blow player (obj +0x330): the last damaging attacker's owner, the killer's owner at death; copied to the
    // team container. -1 = none.
    int32_t final_blow_player(objects::ObjectId id) const;
    // Get_All_Projectile_Types (§2.1): the projectile type of every live hard point with a projectile (HardPoints
    // order), then the type's Projectile_Types. Get_Current_Projectile_Type: Projectile_Types[weapon index] (combat
    // weapon_index: SWAP_WEAPONS / Permanent_Weapon_Swap; no WEAPON behaviour needed), none when the index is outside the
    // list (S8 t12). TEAM containers answer for their first live member (none for an empty team).
    std::vector<objects::TypeIndex> projectile_types(objects::ObjectId id) const;
    objects::TypeIndex current_projectile(objects::ObjectId id) const;
    // Are_On_Opposite_Sides_Of_Shield (§1.2): an active BASE_SHIELD with Base_Shield_Radius > 0 that holds exactly one
    // of the points strictly inside (3-D); with `player` >= 0 only shields whose friendliness to it equals `friendly`.
    // [port] a live BASE_SHIELD object counts as active (no base-power model); the projectile gate (a shot that does no
    // shield damage) is applied by the caller passing `shield_damage` false: then no shield counts (pack +0xc unknown).
    bool opposite_sides_of_shield(Vec3 a, Vec3 b, int32_t player, bool friendly, bool shield_damage = true) const;
    // ---- land strikes: orbital bombardment (§3.8) and bombing runs (§3.7) - tactical_battle_l1_strikes.cpp ----
    // Faction data (load_l1_factions: Bombardment_* fields; by galactic faction index).
    struct L1Faction {
        std::vector<std::string> bombard_ships;        // Bombardment_Required_Orbital_Ships
        std::vector<std::string> bombard_projectiles;  // Bombardment_Projectile (one per salvo; the last repeats)
        std::vector<std::string> bombard_particles;    // Bombardment_Particle (one per salvo; the last repeats)
        int32_t salvos = 0, shots_per_salvo = 0;       // Bombardment_Number_Of_Salvos / _Shots_Per_Salvo
        float shot_delay = 0.0f, salvo_delay = 0.0f, initial_delay = 0.0f;  // seconds
        bool playable = false;                         // cu9h t01: Is_Playable (+0x20a8)
        std::string skirmish_land_bomber;              // cu9h t01: Skirmish_Land_Bomber (the skirmish bombing-run bomber type)
    };
    void set_l1_factions(std::vector<L1Faction> f) { l1_factions_ = std::move(f); }
    // Campaign: only the battle attacker [M: battle-record slot 1] gets a record, enabled when one of its orbit fleets
    // holds a required ship (bombardment) / a Ship_Class 1 bomber with a Land_Bomber_Type (bombing run); land only.
    // start_*: the validity rules (fog force 1, active base shields, prevention radius for bombing runs) and
    // availability; false = refused (the Lua forms log their warnings). The strike runs from the next tick.
    bool start_bombardment(TacticalMode& mode, int32_t player, Vec3 target, SimRng& rng);
    bool bombardment_available(int32_t player) const;
    bool bombardment_target_valid(int32_t player, Vec3 target) const;
    void set_bombardment_disabled(int32_t player, bool disabled);  // Disable_Orbital_Bombardment (tactical list +0x250)
    void remove_bombardment(int32_t player, bool removed);        // Remove_Orbital_Bombardment: enabled := !removed
    bool start_bombing_run(TacticalMode& mode, int32_t player, Vec3 target);
    bool bombing_run_available(int32_t player) const;
    bool bombing_run_target_valid(int32_t player, Vec3 target) const;
    void set_bombing_run_disabled(int32_t player, bool disabled);  // Disable_Bombing_Run (tactical list +0x1c8)
    // cu8f t06 (CU8F_T04_T06_RE §3.3): a player's tactical bool list gate (bombing runs 0x527550, bombardment 0x5275a0):
    // the AND of the list (Battlefield_Modifier Enable_* entries, the Lua Disable_* entry); an empty list allows in a
    // campaign battle and refuses in a skirmish (BattleConfig::skirmish).
    bool player_gate_allows(int32_t player, tactical::abilities::PlayerGate g) const;
    // BombingBlockStatus: IsFinished = no bomber left; Result = undamaged / total ([port] bombers are not targets: 1).
    bool bombing_run_finished(int32_t player) const;
    float bombing_run_result(int32_t player) const;
    // UI queries (HUD_GAPS §9.3, cu9g t02; const, presentation only - the sim never reads them back):
    // frames_until_available = 0x4950f0's frame minus now (0 = ready; a run in progress: interval + the longest remaining
    // flight); cooldown_fraction = 0x495250's dial fraction 0..1 (1 = ready, 0 = shown as a full dial by the caller).
    uint32_t bombing_run_frames_until_available(int32_t player) const;
    float bombing_run_cooldown_fraction(int32_t player) const;
    // The player's bomber type (Land_Bomber_Type of the first Ship_Class 1 bomber in orbit; objects::kNone = no record):
    // the HUD plays its SFXEvent_Bomb_Run_Select_Target / _Incoming (HUD_GAPS §9.4, §9.6).
    objects::TypeIndex bombing_run_bomber_type(int32_t player) const;
    const L1Constants& l1_constants() const { return l1_k_; }
    // --- end L1 systems ---

    // --- Int-final: build pads (L1_S4_REMAINING §3.6; tactical_battle_pads.cpp) ---
    // Pads = live objects of the battle mode with TACTICAL_BUILD_OBJECTS (map placements arrive through the S4 capture
    // placements); their state is created on first use. pad_build = Lua Build (§2.2) / start build 0x42dbd0 without
    // charge or SFX: 1 started, 0 refused by the rules (check_rules: the type in the pad owner's faction row of
    // Tactical_Buildable_Objects_Campaign [S10 battles are campaign battles] and rule 0x42e400: no live non-allied
    // Influences_Capture_Point object within the pad type's Capture_Point_Radius, 3-D; q6 t19: no Neutral exemption, objects
    // with BASE_SHIELD / DUMMY_GROUND_STRUCTURE ignored), -1 start failed (pad not empty, cooldown running, type missing,
    // `builder` not allied with the pad owner - relation 0, so a skirmish Neutral pad never). The under-construction (UC)
    // object of `type` is spawned at the pad (position, facing; owner = `builder`, default the pad owner as Lua pad.Build;
    // the pad owner is unchanged, AI_TACTICAL §7.5.1). Each tick (after the S4 step): a UC reaching its end frame (Tactical_Build_Time_Seconds
    // of the UC type x 30 [port: per-mode player multiplier 1]) is replaced by its Tactical_Buildable_Constructed type at
    // the UC position (+0.1 z, same facing, builder's ownership); a UC / built object that died clears the pad (lock
    // cleared with the built object; Destroy_When_Child_Dies kills the pad; else Minimum_Time_Before_Pad_Can_Build_Again
    // cooldown). Battle host pass: UC / built objects are battle combat units with the UC health ramp (block below).
    // REMAINING_GAPS §6.5: the UC (and the finished object, +0.1 z) stands at the pad type's
    // Tactical_Build_Attachment_Bone_Name bone of the pad model (bind pose, Scale_Factor, pad yaw), else at the pad;
    // on completion every object of exactly the built (else UC) type's Previous_Upgrade_Level_Type owned by a player
    // allied with the pad owner is killed silently; when the child goes away a CAPTURE_POINT pad goes back to the
    // neutral player (owner + capture record). Galactic persistence: see set_pad_persistence. CRC: hash_abilities once a
    // pad was used.
    int32_t pad_build(TacticalMode& mode, objects::ObjectId pad, objects::TypeIndex type, bool check_rules, int32_t builder = -1);
    objects::ObjectId pad_contents(objects::ObjectId pad) const;  // built, else UC, else kNoObjectId
    // s78n t05 (HERO_CLASH §7.1 / §7.2, 0x395c10 step 6): the pad's UC or built slot is set AND that object is alive (a
    // combat unit: not dead in CombatSystem; else a battle unit: alive and not retreated; [port] else set counts).
    // Retail clears the slot in the frame its content dies; pads_tick clears it on the next poll, so the liveness test
    // stands for that. Read by the Bribe accept and the convert victim test through their host hooks.
    bool pad_occupied(objects::ObjectId pad) const;
    bool pad_locked(objects::ObjectId pad) const;
    void pad_lock(objects::ObjectId pad, bool lock);
    // --- end Int-final ---
    // --- battle host (BT pass): pad objects are battle combat units; player / AI TacticalBuild, TacticalSell ---
    // The UC object and the constructed object join the battle as combat units of the builder (targetable, they fight
    // with their own hard points / weapons, they die like any unit; side = the builder's side; they never reach the
    // galaxy: no galactic counterpart, pop 0). Their types are added to the battle tables at setup (every UC type in
    // the Tactical_Buildable_Objects_Campaign lists of the map's pads and its Tactical_Buildable_Constructed type).
    // UC health ramp (TacticalUnderConstructionBehavior vf9 / vf6): health starts at max / build frames [inferred:
    // one service's worth] and grows by max / build frames per frame; the constructed object carries the UC's health
    // fraction. tactical_build = TacticalBuildEvent 0x42edf0 (command 33) for `player`: refusals in order (UC present,
    // built present -> NotAllowed; pad fogged for the player or not allied-owned -> NotAllowed; cooldown -> NotAllowed;
    // type without TACTICAL_UNDER_CONSTRUCTION -> InvalidUnit); `ai` (an AI player): built at once without a charge;
    // otherwise cost = build_cost(type) > credits -> CannotAfford, else charged (credit hooks) and started.
    // The AI agent's TacticalBuild hook: call tactical_build(mode, player, pad, type, rng, /*ai=*/true).
    // tactical_sell = TacticalSellBehavior 0x5248b0 (command 34): the built object of a pad owned by `player`; silently
    // refused (NotAllowed) while the pad is locked; refund = int(cost of the UC entry whose constructed type matches x
    // Tactical_Sell_Percentage), the pad is released without cooldown and the object killed.
    commands::RejectReason tactical_build(TacticalMode& mode, uint8_t player, objects::ObjectId pad, objects::TypeIndex type, SimRng& rng,
                                          bool ai);
    commands::RejectReason tactical_sell(TacticalMode& mode, uint8_t player, objects::ObjectId object);
    // Pad price 0x3744e0 (campaign): Tactical_Build_Cost_Campaign, else Tactical_Build_Cost_Multiplayer when <= 0
    // [port: the corruption planet factor 0x33e440 is not applied].
    float build_cost(objects::TypeIndex uc_type) const;
    // t50: the land skirmish rules. A land skirmish (BattleConfig::skirmish, land map) prices pads from
    // Tactical_Build_Cost_Multiplayer and lists their buildable types from Tactical_Buildable_Objects_Multiplayer
    // (L1_S4_REMAINING §3.6: "skirmish ... Multiplayer", else Campaign), and gets the skirmish base (bt_skirmish_base).
    bool skirmish_land() const { return cfg_.skirmish && !space_; }
    // skai: a skirmish map battle with the skirmish setup (BattleConfig::skirmish_map), land or space.
    bool skirmish_map() const { return cfg_.skirmish && cfg_.map_battle && cfg_.skirmish_map; }
    const char* pad_list_tag() const { return skirmish_land() ? "Tactical_Buildable_Objects_Multiplayer" : "Tactical_Buildable_Objects_Campaign"; }
    // t50: the skirmish slot -> player map [M: slot N = the N-th battle participant by ascending player id]; -1 = none.
    int32_t skirmish_slot_player(int32_t slot) const;
    // t50: Land_Skirmish_Unit_Cap_By_Player_Count of the player's faction at the battle's player count [M: linear
    // interpolation between the curve's keys, clamped to its ends]; -1 = no curve.
    float skirmish_land_unit_cap(int32_t player) const;
    // Galactic persistence (AI_GALACTIC_TOKENS §9.4, REMAINING_GAPS §6.5: gate 0x42f160, add 0x4b6c70, remove 0x4bb3a0,
    // lookup 0x4b7c00, recreate 0x42d8b0): `records` = the battle planet's GalacticSim::built_objects list, set by the host
    // before setup (null = off: no planet). Not in a skirmish (cfg.skirmish) at all.
    //   gate (add / remove)  campaign battle with a planet list, not the sandbox story battle (BattleRequest::sandbox, the
    //                        original's conflict state 9), the pad type's Tactically_Built_Child_Object_Persists (default yes).
    //   add                  at construction completion: {finished type, the finished object's position (build point +
    //                        0.1 z), the pad's base level (map mini 0x0B; refused outside 0..5), the pad's owner};
    //                        refused when a record has the same x and y (exact; z not compared).
    //   remove               the pad's finished child dies or is sold: the record at its x and y with the same type.
    //   recreate (`restore`) at setup, also in a sandbox battle, not while a save is loaded (restore = false: the saved
    //                        battle keeps its saved objects): each pad's own x and y, last match wins; the finished type
    //                        for the record's owner at the pad's build point with the pad's facing, full health, in the
    //                        pad's child slot (so a pre-placed type is not spawned). No match / unknown type / owner -1:
    //                        nothing, the record stays. [port] an owner that is not a battle player: nothing either.
    using PadRecord = galactic::PersistedBuiltObject;
    void set_pad_persistence(std::vector<PadRecord>* records, bool restore = true) {
        pad_records_ = records;
        pad_restore_ = restore;
    }
    // Credits of the galactic player (campaign battles spend the galactic account). Unset: every player has 0 credits
    // and refunds are dropped. CampaignBattles installs GalacticSim::credits / add_credits.
    void set_credit_hooks(std::function<float(int32_t)> get, std::function<void(int32_t, float)> add) {
        credits_get_ = std::move(get);
        credits_add_ = std::move(add);
    }
    // --- end battle host pads ---
    // --- story t12: the reinforcement hyperspace hold (L1_S4_REMAINING §3.4, flag 0x140b27d5f = LINK_TACTICAL P9 == 2,
    // cleared by Lua Resume_Hyperspace_In). The source is the host's (CampaignBattles::linked_presentation, saved and
    // hashed there). While it answers true in a SPACE battle, reinforcements do not arrive: the stand-in deploy (tick
    // step 1) waits and a Reinforce order (human or tactical AI) is refused NotAllowed, the unit staying in the pool.
    // [port] the original accepts the order and freezes the arriving unit hidden in its hyperspace-in state 4 until the
    // release; the port has no hyperspace-in phase (reserves appear at once), so the deployment itself waits. Start units
    // and land battles are not affected. Unset: never held. ---
    void set_arrival_hold(std::function<bool()> held) { arrival_hold_ = std::move(held); }
    bool arrival_held() const { return space_ && arrival_hold_ && arrival_hold_(); }

    // --- battle host (BT pass): HIJACK reserve (AI_GALACTIC_3 §4.3 step 5 "tactical with parent") ---
    // Appends a galactic object (a unit the galactic sim just created at the battle planet, e.g. GalacticSim::magic_spawn
    // for a MagicPlan HIJACK_UNIT) to `owner`'s reinforcement pool of the running battle: a reserve participant keyed by
    // its galactic id (BattleAi::reinforcement_pool / TF.Reinforce / order_reinforce see it at once). Refused (false)
    // when the battle finished, the object is dead / already a participant, `owner` is not the attacker or defender, or
    // a type it spawns is not in this battle's tables (can_add_reserve false: tables are fixed at construction).
    bool add_reserve(objects::ObjectHandle galactic, int32_t owner);
    bool can_add_reserve(uint32_t galactic_type) const;
    // --- end HIJACK ---

    // --- battle host (BT pass): setup / transition results (docs/re/TACTICAL_GAPS.md §4) ---
    // Land force split (§4.2, constructor): the defender's surface units all start; the attacker's start force is its lead
    // unit (0x29c0c0 rules (b) Preferred_Pathfinder_Types non-heroes then heroes, (c) first non-hero, else first hero;
    // [port] rule (a) has no fleet-lead flag / forced type for ground units), the rest of its invading units go to the
    // reserve, unless the units pass the raid test 0x432e20 (all start). No population cap at land start.
    // Space rule 1 (§4.6): a TACTICAL_SUPER_WEAPON (space behaviour set) participant always starts.
    bool land_raid() const { return bt_raid_; }
    // Lead object (§4.8): the tactical id of a spawned super-weapon (a starting TACTICAL_SUPER_WEAPON participant placed at
    // the DEATH_STAR_POSITION marker [M: branch condition]), kNoObjectId when none. At the battle end, when it lives and
    // its owner is not the winner: the winner owns a live Is_Super_Weapon_Killer battle unit -> the super-weapon is
    // killed (its galactic object joins result().destroyed, super_weapon_killed() true); otherwise result().retreated :=
    // the winner. The galactic victory records 3 / 6 are the galactic side's (not posted here).
    objects::ObjectId lead_object() const { return bt_lead_; }
    bool super_weapon_killed() const { return bt_sw_killed_; }
    // Retreat attrition (§4.9): units killed when the retreat countdown ended (one synced draw per eligible unit even at
    // probability 0).
    uint32_t retreat_attrition_kills() const { return bt_attrition_kills_; }
    // Object removal records (0x3a8380: every object leaving the battle mode; the galactic AI's survival learning,
    // GalacticAiWorld::record_object_removal): battle units and team containers that died, in death-event order, then at
    // the battle end every unit / container still on the map or retreated (survived = true), ascending id. original_type
    // = the unit's team container type (0x3973b0), else its own type. Hosts filter (UNIT_AI, TRANSPORT, AI owners).
    struct RemovalRecord {
        int32_t owner = -1;
        uint32_t object_type = 0, original_type = 0;
        bool survived = false;
    };
    std::span<const RemovalRecord> removals() const { return bt_removals_; }
    // Playtest QA D1 / V1: this battle's dead units for the battle-end loss lists: the death records of removals(), without the
    // team containers (a squadron counts its ships). Includes deaths of the final tick, which no host sees through combat().deaths().
    std::vector<RemovalRecord> unit_losses() const;
    // Team containers (REMAINING_GAPS §3.4): in land a company needs Create_Team; its team object type is
    // Create_Team_Type (default "Team"), the container's behaviour type is the company type (or the team type when
    // Create_Team_Type names one). Members deploy in chunks of Max_Squad_Size (default 10, list order); each chunk gets one
    // team object at the chunk centroid with the first member's owner / facing. Land companies without Create_Team get no
    // container. Team containers follow their first live member (store position, each tick).
    // Landing reinforcement points (§3.3): the REINFORCEMENT_POINT placements the map gives to the attacker (mini 02
    // owner = player id); the old nearest-to-anchor inference is gone. Land cap (AI_GALACTIC_3 §3.3) =
    // Base_Population_Capacity + the held points' Additional_Population_Capacity.
    std::span<const objects::ObjectId> landing_points() const { return bt_landing_; }
    // --- end BT pass ---

    bool finished() const { return finished_; }
    // Playtest QA D1 / story t06: a victory is declared and its countdown is running out. Hosts start the fade to the summary
    // here; end_hold_frames_left() = ticks until finished() (at least 1 while the monitor is held).
    bool end_hold_active() const { return vm_declared_ && !finished_; }
    uint32_t end_hold_frames_left() const {
        if (!end_hold_active()) return 0;
        const int32_t left = vm_countdown_ > 0 ? vm_countdown_ : 0;
        return static_cast<uint32_t>(victory_hold() > 0 && left < 1 ? 1 : left);
    }
    const galactic::BattleResult& result() const { return result_; }
    const galactic::BattleRequest& request() const { return req_; }
    const BattleStats& stats() const { return stats_; }
    // cu8l t03: the attacker's hyperspace arrivals still running (records; a squadron container counts until its members end).
    size_t arrivals_pending() const { return arrivals_.size(); }
    // HARDPOINT_DISABLED.md §9.1 (presentation): units whose engines came back online this tick (EnginesOnline: an ENGINE
    // hardpoint repaired to max) - the host restarts their SFXEvent_Engine_Idle_Loop. Not hashed.
    const std::vector<objects::ObjectId>& engines_restarted() const { return engines_restarted_; }
    // --- hangars / station upgrade / engine loops (STATION_HANGARS_SOUND.md; tactical_battle_hangar.cpp) ---
    // Engine-loop requests of the last tick (EngineLoops: Set_Engine_Idle_Loop / Set_Engine_Moving_Loop in call order;
    // start = fade-in, stop = fade-out) for the audio host. Not hashed (presentation).
    std::span<const tactical::hangar::EngineLoopRequest> engine_loop_requests() const { return engine_loop_requests_; }
    float engine_moving_threshold() const { return engine_loops_.moving_threshold(); }
    const tactical::hangar::HangarSystem* hangars() const { return hangars_.get(); }
    uint32_t hangar_launches() const { return hg_launches_; }
    // Station upgrades done so far (battle order): the host copies selection / control groups old -> new and plays the
    // announcement (owner = the upgrade object's owner: own / ally / enemy SFXEvent_Starbase_*_Upgraded).
    struct StationUpgrade {
        objects::ObjectId old_station = objects::kNoObjectId, new_station = objects::kNoObjectId;
        int32_t owner = -1;
    };
    std::span<const StationUpgrade> station_upgrades() const { return hg_upgrades_; }
    int32_t battle_tech_level(int32_t player) const { return hg_tech(player); }  // galactic tech + upgrade bonus
    // Station contents (DUMMY_UPGRADE objects in their station) and the skirmish tactical production queues (contents.hpp).
    // Production runs in skirmish space battles only: Production {type_index, location = the builder, cancel, index}
    // buys Tactical_Buildable_Objects_Multiplayer types of an own builder (the player's faction row) - prerequisites
    // (Tactical_Build_Prerequisites: every listed type owned alive by the player [port]), Build_Limit_Current_For_All_
    // Allies (live objects of the player and allies + their queued orders), Tactical_Build_Cost_Multiplayer charged
    // through the credit hooks, Tactical_Build_Time_Seconds [port: no difficulty / modifier multipliers]. A cancel
    // (index = the order's position among the player's orders at that builder) and a builder's removal refund the
    // charge. Hashed in "battle.abilities" once anything was queued.
    const tactical::hangar::ContentsSystem* contents() const { return contents_.get(); }
    const tactical::hangar::TacticalProduction* production() const { return production_.get(); }
    // skai t03: land skirmish too (docs/re/SKIRMISH_SETUP.md §3: one build manager per player in every skirmish).
    bool production_enabled() const { return production_ != nullptr && cfg_.skirmish; }
    // Lua Set_Garrison_Spawn (obj +0x3CB) for a SPAWN_SQUADRON object: HangarSystem::set_spawn_enabled. Unknown ids are
    // ignored. Not called yet: the Lua object bridge has no hook for the setter (it only stores
    // ScriptObjectState::garrison_spawn).
    void set_garrison_spawn(objects::ObjectId id, bool on);
    // The tactical buy bar of `builder` for `player` (the HUD's Build_NN buttons): every type of the builder's faction
    // row in list order with its price, its open orders at this builder and why a Production would be refused now
    // (block: 0 buyable, 3 Funds, 4 Prerequisite, 5 Limit = ui HudBuildOption::Block; [port] no queue-size or population
    // check, so 1 QueueFull / 2 PopCap never occur). Empty when the builder is not an own production builder.
    struct BuildOptionInfo {
        int32_t type = -1;
        int32_t cost = 0;
        uint32_t queued = 0;
        uint8_t block = 0;
    };
    std::vector<BuildOptionInfo> station_build_options(const objects::ObjectStore& st, int32_t player, objects::ObjectId builder) const;
    // --- end hangars ---
    uint32_t frame() const { return frame_; }
    // Retreat coordinator request (§3.4 vf9): false when not a participant, not yet allowed, already retreating or
    // (cu8 t01) prevented by an interdict / Retreat_Prevention entry.
    bool request_retreat(int32_t player);
    // cu8 t01: the ability check of the request (AbilitySystem::retreat_blocked, TACTICAL_GAPS §1.5); cu8f t07: in space
    // also while the player owns a live TACTICAL_SUPER_WEAPON battle unit (CU8F_T01_T02_T07_RE.md §3.2).
    bool retreat_prevented(int32_t player) const;
    // --- OI host: the retreating player while a retreat countdown runs (-1 none / done). The mode's unit-response
    // "excluded set" (AUDIO.md §3.3, STRATEGIC_GAPS §7.4 [M]): that player's units give no unit responses. ---
    int32_t retreating_player() const {
        return retreat_side_ >= 0 && !retreat_complete_ ? side_player(static_cast<uint8_t>(retreat_side_)) : -1;
    }
    // --- end OI host ---
    // --- P4 sim (owner: galactic agent): the retreat dialog's Autoresolve (BT §4.5, P4_SIM_SYSTEMS §9.3): gated like a
    // retreat request of `player` (a side, retreat allowed, none running, not finished); finishes the battle with
    // result().destroyed / starbase_destroyed = the deaths so far and winner -1 (the galaxy's auto-resolve decides it:
    // CampaignBattles::auto_resolve_battle). False when the gate refuses.
    bool stop_for_auto_resolve(int32_t player);
    // --- end P4 sim ---
    std::string summary() const;

    const tactical::movement::MovementSystem& movement() const { return *movement_; }
    // S7 t22: the formation record of a squadron container (or a fighter without one); null = no formation.
    // s78f t02: the auto attack blocks of `key`'s movement formation (FighterBlock::pushed), bottom to top.
    std::vector<objects::ObjectId> fighter_pushed(objects::ObjectId key) const;
    const FighterFormationRecord* fighter_formation(objects::ObjectId key) const;
    // s78f review: the order block of `r` as the chase tests read it (FighterOrderView). Const read only.
    FighterOrderView fighter_order_view(const FighterFormationRecord& r) const;
    // S7 render attitude (MovementSystem::attitude in radians, for render::object_transform): roll right-handed about
    // the forward axis (positive = right wing down; a left turn banks negative), pitch right-handed about the left axis
    // (positive = nose down), applied pitch then roll to the level yaw frame of the store facing. Space fighters, land
    // flyers (and the bike lean); other movers 0 / 0. False (both 0) when `id` is not a mover. Const read only.
    bool unit_attitude(objects::ObjectId id, float& roll_rad, float& pitch_rad) const;
    const tactical::combat::CombatSystem& combat() const { return *combat_; }
    // s78k t04: a TEAM container's record owner (Container::owner: loss records, replenish spawns); -1 when `id` is no
    // container of this battle.
    int32_t container_owner(objects::ObjectId id) const;
    // s78k t05 test seams (ALAMO_SIM_HAVE_ANIM; without it both return false): the combat hook of slot 47 (the attack-idle
    // pose, CombatHooks::attack_idle) on `id` with the caller's generator, and a clip play on the battle's animation world
    // (index 0, looping, hard cut). False when the battle has no animation world or `id` has no row in it.
    bool test_attack_idle(objects::ObjectId id, bool arg, SimRng& rng);
    bool test_play_clip(objects::ObjectId id, int32_t clip_type);
    // s78n t09 test seam (TACTICAL_COMBAT §5.4.4 item 1, 0x404450): the joint box half-extents (along / across the
    // facing) of movement-backed land squad container `id` - the AbilityHost::land_squad_slots input Bribe's E_T folds
    // (abilities::squad_joint_half_extents). False: not such a container. Const read only.
    bool squad_joint_box(const objects::ObjectStore& st, objects::ObjectId id, float& hx, float& hy) const;
    // s78n t10 review test seam (FOG_OF_WAR §5.2, TACTICAL_COMBAT §5.4.6 port spec item 3): the 8 (x, y) points of object
    // `h`'s fog multisample footprint 0x38de80 (the 4 corners, then c + hx a, c - hy b, c - hx a, c + hy b), whether or
    // not its type has Multisample_FOW_Check. Const read only.
    void fog_footprint(const objects::ObjectStore& st, objects::ObjectHandle h, float* xy16) const { fog_footprint_samples(st, h, xy16); }
    const tactical::abilities::AbilitySystem& abilities() const { return *abilities_; }
    tactical::abilities::AbilitySystem& abilities() { return *abilities_; }
    const BattleTables& tables() const { return *tables_; }
    std::span<const AttachedEffect> attached_effects() const { return effects_; }
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    // A1 DAMAGE_TRACKING (behaviour 0x3E) per battle: objects with the behaviour are enabled at spawn and
    // serviced on their behaviour cadence (interval 30, first service at once) with AI_SpaceThreatDecayStep.
    // Fed every tick from CombatSystem::damage_events, recorded inline in TacticalBattle (S10-int).
    ai::tactical::DamageTrackingStore& damage_tracking() { return damage_tracking_; }
    const ai::tactical::DamageTrackingStore& damage_tracking() const { return damage_tracking_; }
#endif
    // --- fog of war (owner: fog agent; tactical_battle_fog.cpp) ---
    // One FogOfWar per battle (BattleConfig::fog): grids created in setup() (neutral player skipped, relations
    // from CombatSystem::is_enemy, humans from the galaxy, dense mask from the mode's nebula / asteroid / ion
    // storm SPACE_OBSTACLE objects), decay at the start of tick() (common-tick step 4), REVEAL units serviced
    // after the combat events (object-service step; dead / retreated / garrisoned units undone, every shot flashes
    // the shooter for the target's owner), battle scale (land: the defender by the attacker's Modifies_Reveal_Range
    // objects; space: the attacker by the defender's), no overrun reveal (multiplayer only, not ported),
    // land setup phase when SetupPhaseEnabled. Null when fog is off or the battle is not set up.
    tactical::fog::FogOfWar* fog() { return fog_on_ ? &fog_ : nullptr; }
    const tactical::fog::FogOfWar* fog() const { return fog_on_ ? &fog_ : nullptr; }
    // Carried global FOW-active flag / render option (space keeps the previous battle's values). Before setup.
    void set_fog_globals(bool fow_active, bool render_option) {
        fog_global_active_ = fow_active;
        fog_global_render_ = render_option;
    }
    // Refog hook (AI threat grid slot 3): (player, FOW cell, cell centre x, y), cell order per grid.
    void set_fog_refog(std::function<void(int32_t player, uint32_t cell, float x, float y)> fn);
    // FOW_Point_Test / FOW_Object_Hidden (FOG_OF_WAR §5) on the battle's mode; false while fog is off.
    bool fog_point_fogged(int32_t player, float x, float y, bool force) const;
    bool fog_object_hidden(int32_t player, objects::ObjectId id, bool force) const;
    // refixf t03: the representative type 0x3973b0 (LUA_API.md "Find_All_Objects_Of_Type and the sibling finders") for
    // the Lua finders (lua::ObjectBridge::representative_type): a TEAM container -> its Container::type (Create_Team_Type,
    // else the company type). The battle has no tactical transport or fleet objects; anything else -> objects::kNone (the
    // binding then uses the store type).
    objects::TypeIndex representative_type(objects::ObjectId id) const;
    // The fog's view of a live object (Lua Reveal / Temporary_Reveal input); false when not alive.
    bool fog_object(objects::ObjectId id, tactical::fog::FogObject& out) const;
    // OI fog: host-level FOW-active flag / render option (FOG_OF_WAR §1.3, §6.5: Lua Disable_Rendering, story
    // ENABLE_FOW). Set on the battle's fog while it runs, otherwise on the carried globals used by the next setup
    // (land forces both on at setup). fog_active() = the flag game logic reads (false without fog).
    void set_fog_active(bool fow_active, bool render_option);
    bool fog_active() const;
    bool fog_render_option() const;
    // OI fog: land setup phase (FOG_OF_WAR §3.4, TACTICAL_GAPS §5.4). setup() starts it on land when SetupPhaseEnabled
    // (False in FoC and TR); the setup manager (FOWF t05) ends it when every human player sent SetupPhaseTriggerEnd (32)
    // or SetupPhaseCountdownSeconds after the first one. No-ops in space, without fog, or when not in that state.
    void fog_begin_setup_phase();
    void fog_end_setup_phase();
    bool fog_setup_phase() const;
    // OI fog: player FOW reveal-range list (+0x1f8) for the Battlefield_Modifier writer (TACTICAL_GAPS §2.3.11:
    // FOW_Reveal_Range_Multiplier per affected player, category Stacking_Category, source = (ability, user)).
    void fog_add_reveal_multiplier(int32_t player, int32_t category, uint64_t source, float multiplier);
    void fog_remove_reveal_modifier(int32_t player, uint64_t source);
    // OI fog: space reinforcement FOW sampler (TACTICAL_GAPS §5.5) over the live objects with a
    // Reinforcement_Prevention_Radius. Overlay = 0 / 0xFF per cell of `player`'s grid (render-only palette 3; false
    // without a grid). Point test = the fog / prevention part of the space drop validity (SpaceMode vf80, force 1);
    // true while fog is off.
    bool fog_reinforce_overlay(int32_t player, std::vector<uint8_t>& out) const;
    bool fog_reinforce_point_ok(int32_t player, float x, float y) const;
    // --- end fog ---
    void hash(StateHasher& h) const;  // battle-host state (participants, units, retreat, result)
    // S10-int: abilities.objects + combat.ext (S9 state) and the integration state (speed factors, particles,
    // containers, dummies, engines-offline flags). Registered by CampaignBattles as "battle.abilities".
    void hash_abilities(StateHasher& h) const;

    // ---- N1 stage 2 save / load (owner: n2-host; tactical_battle_save.cpp) ----
    // A running battle saves as several chunks, one per part (CampaignBattles registers them as save participants at the
    // battle start, campaign_battles.hpp). Between ticks only. The loader first builds the battle exactly as at its start
    // (constructor from the saved request over the restored galaxy, the same setters, setup() on the same map) - that
    // rebuilds every definition (catalog, request, config, map, battle tables, types used, constants, behaviour ids,
    // hooks, pointers between systems) - then read_part() replaces the mutable state:
    //   State      this class: participants, units, containers, particles / dummies / effects, anchors, retreat, result,
    //              stats, order queues, fog flags + pings, L1 caches / trackers / strikes, pads, BT records, OI respawns,
    //              hangar records; checks types_used_ / descs_ against the rebuilt battle. Per-tick outputs read in the
    //              same tick (story feeds, special-weapon / engine-loop events, crush events) are not saved.
    //   Movement   MovementSystem, HyperspaceDepartures, EngineControl
    //   Combat     CombatSystem, WeatherSystem
    //   Abilities  AbilitySystem, SpecialWeaponSystem (incl. its lazily added descriptors), SpaceFieldIndex
    //   Garrison   GarrisonSystem, CapturePointSystem (nothing without the garrison lib)
    //   Hangar     HangarSystem, ContentsSystem, TacticalProduction, EngineLoops (absent ones: a flag)
    //   Fog        FogOfWar (nothing without engine/tactical/fog)
    //   Damage     DamageTrackingStore (nothing without engine/ai)
    // Every module system is written through write_system / read_system (save_session.hpp; a system without write /
    // read is a build error, cu9 t16). read_part() returns false (State: unchanged) on bad data.
    enum class SavePart : uint8_t { State, Movement, Combat, Abilities, Garrison, Hangar, Fog, Damage, Count };
    static const char* save_part_name(SavePart p);  // "battle.state", "battle.movement", ...
    void write_part(SavePart p, ByteWriter& w) const;
    bool read_part(SavePart p, ByteReader& r);

    // refix t03 (S9_DEATH_CLONE_SHIELDS.md §1.6 [L]): true when the death clone of a dying_type unit takes the infantry
    // corpse path (no clone object; the dying unit's corpse row plays the clone type) instead of a store object.
    bool death_clone_corpse_path(uint32_t dying_type, int32_t clone_type, int32_t key) const;

private:
    template <class Ar, class Self>
    static void state_io(Ar& a, Self& b);  // tactical_battle_save.cpp
    void resume_tick_context();            // tactical_battle.cpp (BattleHooks is defined there)
    void attach_rocket_terrain();          // refixf t01: land heightmap for spline rockets (setup and after a load)
    void share_groups_sync();              // S8 t17: squadron share groups (tick start and after a load)
    friend class BattleHooks;
    friend class BattleAi;  // A1 tactical AI hookup (battle_ai.hpp): perception snapshot + AI order execution
    struct Participant {
        objects::ObjectHandle galactic;
        objects::ObjectId galactic_id = objects::kNoObjectId;
        int32_t owner = -1;
        uint32_t type = 0;
        uint8_t side = 0;  // 0 defender, 1 attacker
        bool reserve = false, starbase = false, structure = false, deployed = false;
        bool built = false;  // battle host: a pad UC / constructed object (no galactic counterpart)
        float pop = 0.0f, power = 0.0f;
        uint32_t spawned = 0, alive = 0;
        // --- map placement (REMAINING_GAPS §3; tactical_battle_bt.cpp "map load" / "post-load placement") ---
        bool map = false;    // a map-placed object (no galactic counterpart)
        bool third = false;  // owned by neither the attacker nor the defender: not counted for victory / side power
        bool unplaced = false;  // §3.2 step 4: a structure with no free marker is not spawned
        int32_t other_type = -1;  // §3.2 step 3: Place_Other_Type_At_Every_Specific_Marker_Position (one per spot)
        std::vector<std::pair<Vec3, Vec3>> spots;  // marker positions / facings (starbase, structures, step 3 types)
    };
    struct Unit {
        objects::ObjectId id = objects::kNoObjectId;
        objects::ObjectHandle h;
        uint32_t participant = 0;
        uint32_t type = 0;
        uint8_t side = 0;
        bool movable = false, alive = true, retreated = false;
        bool tracked = false;     // DAMAGE_TRACKING
        bool engines_offline = false;  // S10-int: last ENGINE hardpoint destroyed (cleared by EnginesOnline)
        uint32_t dt_next = 0;     // next DAMAGE_TRACKING service frame
        uint32_t container = 0xFFFFFFFFu;  // index into containers_
        float power = 0.0f, range = 0.0f;
        // OI abilities §5.2: a DEPLOY_TROOPERS member attached at bone `attach_bone` of `attached_to` (0x38d320) and
        // obj +0x3a2 (`flags_3a2`, bit 2 set at the spawn) [port: recorded, the position is not pinned to the bone].
        objects::ObjectId attached_to = objects::kNoObjectId;
        int32_t attach_bone = -1;
        uint8_t flags_3a2 = 0;
        bool fighter_seen = false;  // S7 fighters: set_fighter_params done (fighter_sync)
    };
    struct Container {
        objects::ObjectId id = objects::kNoObjectId;
        objects::ObjectHandle h;
        uint32_t alive = 0;
        bool removed = false;
        bool team = false;   // battle host: a Create_Team_Type TEAM object (follows its first live member)
        uint32_t type = 0;   // battle host: the container's type
        int32_t owner = -1;  // battle host
        uint32_t squad_type = 0xFFFFFFFFu;  // S7 fighters: the company / squadron type (Squadron_Formation_Error_Tolerance)
    };
    struct TimedObject {  // PARTICLE objects (death frame) and dummy targets (spawn frame)
        objects::ObjectId id = objects::kNoObjectId;
        objects::ObjectHandle h;
        uint32_t frame = 0;
    };
    void service_damage_tracking();
    void add_participant(objects::ObjectHandle g, int32_t owner, uint8_t side, bool reserve, bool starbase, bool structure);
    // a1i t04: `member_first` / `member_count` spawn a member range of an already deployed participant (one landing
    // unload step: one team chunk or one vehicle); the default spawns everything once.
    void deploy(TacticalMode& mode, uint32_t participant, SimRng& rng, uint32_t member_first = 0,
                uint32_t member_count = 0xFFFFFFFFu);
    void space_obstacles_sync(const TacticalMode& mode);  // refixf t02: SPACE_OBSTACLE units as space static entries
    // S8 t02 / S7 t13 (MOVEMENT §1.3 / §2.2): obstacle footprints of single objects under their object id (tactical_battle.cpp).
    bool land_obstacle_attach(const TacticalMode& mode, objects::ObjectHandle h, const ParallelFor& pf = serial_for);   // LAND_OBSTACLE stamp; false: none
    bool space_obstacle_attach(const TacticalMode& mode, objects::ObjectHandle h);  // SPACE_OBSTACLE static entry
    void land_obstacles_sync(const TacticalMode& mode, const ParallelFor& pf);  // S8 t02: stamp new LAND_OBSTACLE objects, unstamp the gone
    std::vector<uint8_t> land_obstacle_none_;  // S8 t02: per type, 1 = no footprint (no XML size, no model); derived, not saved
    Vec3 slot_position(uint8_t side, uint32_t passability_mask);
    Vec3 snap_passable(Vec3 p, uint32_t passability_mask, bool always) const;  // slot_position's box clamp + cell search
    std::vector<Vec3> drop_pos_;  // a1g t04 follow-up: order_reinforce's land layout slots for deploy (transient)
    bool deploy_unload_ = false;  // s78i t02: deploy places a landing object's company (keeps the side facing; transient)
    std::vector<Vec3> drop_face_;  // s78j t01: per member (deploy's member index), the landing unload facing (transient)
    // a1i t04 (A1_READINGS §35.2): one landing object (TransportDataPack fields in brackets).
    struct Landing {
        objects::ObjectId id = objects::kNoObjectId;
        objects::ObjectHandle h;
        uint32_t participant = 0;
        uint8_t state = 4;               // [+0x50] 4 waiting (hidden), 1 landing, 2 unloading, 3 taking off
        bool timed = false;              // BattleConfig::landing_unload_timed at the order
        uint32_t timer_start = 0, timer_len = 0;  // [+0x60 / +0x64] frames
        uint32_t unloaded = 0;           // [+0x68] members spawned
        uint32_t next_offset = 0;        // [+0x90]
        uint32_t remove_frame = 0;       // untimed: the take-off frame
        std::vector<Vec3> targets;       // [+0x78] exit targets (world), one per step
        std::vector<objects::ObjectId> units;  // [+0xa8] unloaded units (locked while unloading)
    };
    // The land landing object of a Company_Transport_Unit company (id kNoObjectId: none).
    Landing spawn_landing(TacticalMode& mode, const Participant& p, float x, float y);
    uint32_t landing_step_frames(int32_t player) const;  // 2 s x (1 + Reinforcement_Deploy_Time_Multiplier list)
    void landing_service(TacticalMode& mode, SimRng& rng);  // timed state machine + take-off removal (each tick)
    bool landing_locked(objects::ObjectId id) const;       // an unloaded unit of an unloading landing (exit path lock)
    std::vector<Landing> landings_;                        // live landing objects
    // a1i t05 (A1_READINGS §35.4 item 4): the space Reinforce members' positions around (x, y), in spawn order (one member:
    // the point). False when a member's model box is not loaded (no model data) [port: the rows of 6 then].
    bool space_member_spots(TacticalMode& mode, const Participant& p, float x, float y, std::vector<Vec3>& out) const;
    // q6 (OBJECT_SCRIPTS §10.4 step 5): Spawn_Unit's member spots around `at`, in member order: per member the free
    // placement ring search (radius 2500, start angle yaw - 45), land candidates on a pure-Clear cell with z := terrain
    // height; each placed member blocks the later ones; no free spot -> `at`.
    void script_member_spots(const TacticalMode& mode, std::span<const uint32_t> members, Vec3 at, float yaw_deg,
                             std::vector<Vec3>& out) const;
    std::vector<LandingUnload> landings_unloaded_;    // transient (not saved): see landings_unloaded()
    std::vector<ProductionDone> production_done_;     // q6 t19, transient (not saved): see production_done()
    void reinforce(TacticalMode& mode, SimRng& rng, bool force_empty_sides);
    void issue_orders(TacticalMode& mode, SimRng& rng);
    void retreat_policy();
    void complete_retreat(TacticalMode& mode, uint8_t side);
    // --- story t06: victory monitor (tactical_battle_victory.cpp) ---
    struct VmRecord {
        uint8_t type = 0;
        int32_t player = -1;
        int32_t countdown = 0;  // +8: frames from the declaration to the end
        bool silent = false;    // +0x15
    };
    struct VmTracked {  // a victory-relevant object at the last scan
        objects::ObjectId id = objects::kNoObjectId;
        int32_t owner = -1;
        bool base = false;  // skirmish map: DUMMY_LAND_BASE_LEVEL_COMPONENT (land) / DUMMY_STAR_BASE (space)
    };
    void vm_setup(TacticalMode& mode);  // end of setup(): records + the first scan
    void vm_scan(const TacticalMode& mode, std::vector<VmTracked>& out);
    void vm_add_record(uint8_t type, int32_t player, int32_t countdown, bool silent);
    bool vm_test(uint8_t type, int32_t player);       // 0x344710 with its gates
    bool vm_evaluate(uint8_t type, int32_t player);   // the condition alone
    bool vm_blocker(int32_t player, bool base_only);  // an active relevant object of an enemy of `player`
    void vm_object_lost(const VmTracked& t);          // 0x2c0340
    void vm_retreat_complete(int32_t retreated);      // 0x340750
    void vm_raise();                                  // 0x341fe0
    void vm_release(bool replay);                     // 0x3440a0
    void vm_sync_tsw();                               // TSW fire / state-13 holds -> raise / release
    bool vm_base_object(const objects::ObjectStore& st, objects::ObjectHandle h) const;
    // ccpgt t02 (SKIRMISH_SETUP §4.2): the domination rule.
    bool vm_cp_relevant(const objects::ObjectStore& st, objects::ObjectHandle h) const;  // CAPTURE_POINT + the type flag
    // Every relevant point except `skip` allied with `player`; `any` = at least one relevant point (skip excluded).
    bool vm_cp_all_allied(int32_t player, objects::ObjectId skip, bool& any) const;
    bool vm_player_playable(int32_t player) const;
    void vm_cp_capture(objects::ObjectId point, int32_t new_owner);  // 0x360d90, before the new owner is written
    void vm_cp_disarm();
    void check_victory(TacticalMode& mode);
    // BT §3.1a item 1 (0x372230): Victory_Relevant, else Space_ / Land_Victory_Relevant of the type. Cached per type in
    // vr_cache_ (0 unknown, 1 no, 2 yes): a pure function of the type data and the mode, not saved.
    bool victory_relevant(uint32_t type);
    void finish(int32_t winner, uint8_t victory, int32_t retreated);
    float side_power(uint8_t side) const;
    int32_t side_player(uint8_t side) const { return side == 0 ? req_.info.defender : req_.info.attacker; }
    Unit* unit_by_id(objects::ObjectId id);
    // cu8j t02 (MOVEMENT §4.7.1): the adopt 0x39bcb0 of object h - the predicate (not a TEAM member; the mode's locomotor
    // kind, a TEAM_LOCOMOTOR container only when a member's locomotor admits; [port] builders and hyperspacing objects are
    // not tested), then for a movement unit MovementSystem::adopt (no draw while it has a coordinator; `leave` first leaves
    // every coordinator: stop_and_hold), for any other object one FormationClass ctor draw. True = one draw.
    bool mv_adopt(const TacticalMode& mode, objects::ObjectHandle h, SimRng& rng, bool leave = false);
    int8_t mv_adopt_kind(uint32_t type);  // per type and mode: 1 land, 2 space, 0 not admitted, -1 TEAM_LOCOMOTOR (cached)
    // cu8l t02 (CU8K_T02_T06_RE §3 rule 6): the first movement lock's split / adopt of a unit or TEAM container (no
    // draw for a squad member, or an object in a coordinator without a formation). True = one draw.
    bool mv_first_lock(const TacticalMode& mode, objects::ObjectId id, SimRng& rng);
    std::string_view mv_locomotor_token(uint32_t type) const;  // the mode's active *_LOCOMOTOR behaviour token (empty: none)
    bool mv_self_registers(uint32_t type) const;  // cu8l t01: LandTeamContainer (land) / SimpleSpace (space) locomotor
    // s78n t03 (TACTICAL_COMBAT §5.4.4 item 2, MOVEMENT §3.7): makes land squad container containers_[ci] (object type
    // `object_type`, a LAND_TEAM_CONTAINER locomotor) a movement unit walking its own path, its members steering to their
    // slots (MovementSystem::set_land_team). False (nothing changed): a member is not a Default-locomotor walker without
    // Uses_Multiple_Locomotors [port: such squads keep bt_team_positions' re-centring].
    bool mv_squad_add(TacticalMode& mode, size_t ci, uint32_t object_type);
    // s78n t09 (TACTICAL_COMBAT §5.4.6 items 1, 2, 4): 0x371be0 - a type's half-extents (Custom_Hard_X/YExtent x
    // Scale_Factor when both > 0, else per axis the mode model's collision-bound half x / y x Scale_Factor, no model 0);
    // 0x396850 for a unit (0x371d20, the deployed pair, for a deploying / deployed Deploys walker).
    void type_half_extents(uint32_t type, float& hx, float& hy) const;
    void unit_half_extents(const Unit& u, float& hx, float& hy) const;
    // s78n t10 (§5.4.6 item 2, 0x371d20 as 0x396850 selects it): replaces (hx, hy) by the deployed pair when object `id`
    // of type `type` is a movement-backed Deploys walker in Walk state 0x18 / 0x19 with Custom_Hard_XExtent_Deployed > 0.
    // Const reads only (the fog footprint calls it from the parallel filter).
    void deployed_half_extents(uint32_t type, objects::ObjectId id, float& hx, float& hy) const;
    // s78n t09 (§5.4.4 item 1, 0x404450): a movement-backed land squad container's joint box input - its live members'
    // slots (team index order) with their half-extents, the container type's MaxJiggleDistance and the container's
    // facing. False: not such a container.
    bool squad_joint_slots(const objects::ObjectStore& st, objects::ObjectId container,
                           std::vector<tactical::abilities::SquadSlot>& slots, float& jiggle, Vec3& facing) const;
    void combat_team_positions(const objects::ObjectStore& store);  // s78n t03: CombatSystem::set_team_position per team
    void s4_rejoin_squad(TacticalMode& mode, const Unit& u, Vec3 at);  // s78n review: a member out of a garrison
    // s78n t14 (FIGHTER_FLYING §7.2 / §7.4): a squad member's layout shape (MovementSystem::set_team_member_shape); the
    // garrisoned squad container (the last member in takes it in; a squad leaves as one, see s4_squad_leave).
    void mv_team_shape(const Unit& u);
    struct SquadExit {
        objects::ObjectId container = objects::kNoObjectId;
        Vec3 at{};          // E: where the container was put (the host centre)
        Vec3 to{};          // X: the walk-out point (Exited)
        bool walk = false;  // Exited (walk out to X) / Ejected (everything at the host position)
    };
    void s4_squad_enter(TacticalMode& mode, const Unit& u, Vec3 host);
    Vec3 s4_squad_leave(TacticalMode& mode, const tactical::garrison::GarrisonEvent& e, std::vector<SquadExit>& outs,
                        std::vector<tactical::garrison::GarrisonEvent>& events);
    std::vector<int8_t> adopt_kind_;      // cu8j t02: mv_adopt_kind cache (+1; 0 = not computed); derived, not saved
    bool deploy_script_ = false;          // cu8j t02: deploy() for a script spawn (no team helper adopt; transient)
    // FIGHTER_FLYING §6.3.1: FlyingHooks::clearance type data per object (ClearanceInfoFn), cached per type.
    bool clearance_info(objects::ObjectId id, tactical::movement::ClearanceInfo& out);
    // SPACE_AVOIDANCE: the avoidance tree members (space battles), rebuilt from the committed store before the movement
    // tick, ascending object id; FighterAvoidFn = tactical::movement::space_avoid over them.
    void avoid_snapshot(const objects::ObjectStore& store);
    void unit_gone(TacticalMode& mode, Unit& u);  // container bookkeeping after a death / retreat
    // S10-int steps
    void apply_speed_factors();
    void drop_locked(std::vector<objects::ObjectId>& ids) const;  // OI follow-up t03: group moves skip locked units
    // OI follow-up t03: per bridge owner, registration order (not saved: the bridges' own state)
    std::vector<std::pair<const void*, std::function<bool(objects::ObjectId)>>> script_locks_;
    void feed_attack_targets(const objects::ObjectStore& store);
    // S7 fighters (FIGHTER_FLYING.md; tactical_battle.cpp "fighter host"): squadrons / params into the MovementSystem,
    // the combat target as the formation attack target, the locomotor's Provoke / Retarget orders back into combat.
    void fighter_sync();
    void feed_fighter_target(const Unit& u, const objects::ObjectStore& store);
    void forward_fighter_events();
    objects::ObjectId fighter_combat_unit(objects::ObjectId id) const;  // a squadron container -> its leader
    // S7 t22 (FIGHTER_FLYING §4.9): the formation records (FighterFormationRecord, sorted by key, saved in battle.state).
    // fighter_order: a move / attack order of `ids` (fighters only; members map to their squadron) replaces their
    // formation (flags 0x01 attack-move / 0x02 guard, slot goal, object block); fighter_order_clear: Stop.
    void fighter_order(std::span<const objects::ObjectId> ids, uint8_t flags, Vec3 goal, objects::ObjectId goal_object = kNoObjectId);
    void fighter_order_clear(std::span<const objects::ObjectId> ids);
    objects::ObjectId fighter_key(const Unit& u) const;  // the squadron container's id (TEAM container), else the unit's
    FighterFormationRecord* fighter_formation_mut(objects::ObjectId key);
    // The push (§4.9 step 2): the squadron's unlocked auto target -> a new auto attack block when it is not the formation
    // goal already and the chase test (fighter_chase_test) passes. Returns the newest live, hostile pushed block.
    objects::ObjectId fighter_push(const Unit& leader, objects::ObjectId key, const objects::ObjectStore& store);
    objects::ObjectId fighter_goal_key(objects::ObjectId id);  // a squadron member -> its container, else id
    bool fighter_goal_live(objects::ObjectId key, const objects::ObjectStore& store);
    // S8 t17 (FIGHTER_FLYING §4.10 A): 0x4160a0(F, u, c, &Rr) for combat's scan (CombatSystem::set_scan_range_query);
    // read-only (combat's parallel gather phase). c = kNoObjectId: the scan cube test.
    uint8_t fighter_scan_range(const objects::ObjectStore& store, objects::ObjectId member, objects::ObjectId candidate, float range,
                               float& rr) const;
    // enter_idle (§4.7 step 4): fighters that entered Idle during the movement tick drop their (team) combat target.
    void fighter_enter_idle();
    void fighter_formation_hooks();  // s78f t02: MovementSystem::set_fighter_formation_hooks (space setup)
    std::vector<FighterFormationRecord> fformations_;
    std::vector<std::array<float, 4>> fchase_cache_;  // per type: Guard / Idle_Chase_Range, Attack_Move_Response_Range,
                                                      // Autonomous_Move_Extension_Vs_Attacker (derived)
    std::vector<uint8_t> fchase_have_;
    void drive_animation(objects::ObjectStore& store, SimRng& rng, uint64_t global_frame);
    // s78l t03 (TACTICAL_COMBAT slot 47 "s78l t03", TACTICAL_GAPS 6.2): IMPOSING_PRESENCE (0x624ab0, interval 30 on the
    // store's behaviour schedule) - idle records for the IDLE units around each presence object. `gated` = per units_
    // index, the IDLE moving gate of this frame (drive_animation).
    void imposing_presence(objects::ObjectStore& store, const std::vector<uint8_t>& gated);
    // Presence_Induced_Animations (anim type + type list per entry, list order) and Presence_Range of a type; cached per
    // type (a pure function of the type data, not saved).
    struct PresenceTypeInfo {
        bool parsed = false;
        float range = 0.0f;
        std::vector<std::pair<int32_t, std::vector<uint32_t>>> anims;
    };
    const PresenceTypeInfo& presence_info(uint32_t type);
    // s78l t03: the default idle type's presence step for `unit` over its idle records (anim::IdleRecord, ascending
    // presence id): the anim type or -1 (none).
    int32_t presence_pick(const objects::ObjectStore& st, int32_t unit, const anim::IdleRecord* records, size_t count);
    // s78l t02: drains MovementSystem::take_turn_clip_events after the movement tick and plays them (synced draws).
    void play_turn_clip_events(SimRng& rng, uint64_t global_frame);
    std::vector<PresenceTypeInfo> presence_cache_;
    std::vector<uint8_t> idle_gate_;  // drive_animation scratch (per units_ index)
    void consume_events(TacticalMode& mode, SimRng& rng);
    // consume_events step d for one combat death (also script_damage's out-of-tick kills): a dead or unknown unit is skipped.
    // s78n t12 review: `silent` = a world removal without Die (CombatSystem::remove_unit: the vehicle a thief's clone
    // replaces) - the object leaves as for a death, without the Death_Explosions particle and the death clip.
    void process_death(TacticalMode& mode, const tactical::combat::DeathEvent& d, SimRng& rng, bool silent = false);
    void spawn_particle(TacticalMode& mode, int32_t type, Vec3 pos, int32_t owner);
    objects::ObjectHandle spawn_prop(TacticalMode& mode, int32_t type, Vec3 pos, Vec3 facing, int32_t owner);
    // refix t01 / t03: Die step 8's clone object (flags 0xc0), and the infantry corpse-path test (S9 §1.6).
    objects::ObjectHandle spawn_death_clone(TacticalMode& mode, int32_t type, Vec3 pos, Vec3 facing, int32_t owner);
    mutable std::optional<uint64_t> infantry_category_;  // GameObjectCategoryType "Infantry" bit (lazy; ~0 = unknown)
    void expire_objects(TacticalMode& mode);
    std::vector<objects::ObjectId> owned_ids(const TacticalMode& mode, uint8_t player, const std::vector<uint32_t>& wire) const;
    // --- [port: Gabriel] queueable unit actions (tactical_battle.cpp "order queue") ---
    struct OrderQueue {
        objects::ObjectId id = objects::kNoObjectId;
        std::vector<QueuedOrder> orders;  // pending, run order
        bool has_current = false;         // the current order came from this queue (else: live state)
        QueuedOrder::Kind current = QueuedOrder::Kind::Move;
    };
    std::vector<OrderQueue> queues_;  // ascending id, never empty orders
    uint32_t queue_group_ = 0;        // command serial
    bool queue_used_ = false;         // some order was queued (CRC section on from then)
    bool order_busy(objects::ObjectId id, const OrderQueue* q) const;
    void clear_queues(std::span<const objects::ObjectId> ids);
    std::vector<objects::ObjectId> enqueue(std::span<const objects::ObjectId> ids, const QueuedOrder& o);  // -> run now
    void advance_queues(TacticalMode& mode, SimRng& rng);
    void late_swap_back_signals();  // S7 t27: signal 0x29 for a jetpack swap back after the combat_pre loop
    bool queue_object_live(const TacticalMode& mode, objects::ObjectId id) const;
    void exec_move(TacticalMode& mode, std::span<const objects::ObjectId> ids, float x, float y, SimRng& rng, float facing_deg,
                   bool attack_move, uint8_t flags = 0, bool switch_locomotor = false,
                   objects::ObjectId goal_object = kNoObjectId);  // S7 t37: an object goal (mv::MoveOrder::goal_object)
    uint32_t exec_attack(TacticalMode& mode, std::span<const objects::ObjectId> ids, objects::ObjectId target, int32_t hp, SimRng& rng);
    // --- q6: an attack ordered on a TEAM container (A1_READINGS §33.2.1 row 1: a TEAM container passes 0x5680b0 on its
    // own; TACTICAL_COMBAT §2.8 B step 5 / FIGHTER_FLYING §4.10 B: the attacker's target is the container itself). The
    // container is not a combat unit in the port, so the host keeps the container order per attacker and gives combat the
    // member the next shot would take: the live member nearest the attacker (LOB_SOLVER §5.1, 0x404fc0; XY distance, ties
    // by the lower id [port]); a squadron (combat share group) shares its first attacker's pick (0x406370). The member is
    // re-picked after every combat tick (nearest changed, member dead); the order ends when the container is gone (its last
    // member died or left), the attacker's combat order was dropped or replaced, or a newer order (signal 0xb) arrives.
    struct TeamAttack {
        objects::ObjectId attacker = objects::kNoObjectId;   // ascending
        objects::ObjectId container = objects::kNoObjectId;  // the ordered target
        objects::ObjectId member = objects::kNoObjectId;     // combat's ordered target (the current pick)
        bool goal = false;  // the order also set the formation attack goal (AI host: Attack_Target block, §2.8)
    };
    std::vector<TeamAttack> team_attacks_;
    struct TeamPick {  // one pass's squadron pick (0x406370: the squadron shares its first attacker's pick)
        objects::ObjectId group = objects::kNoObjectId, container = objects::kNoObjectId, member = objects::kNoObjectId;
    };
    // A live TEAM container (not removed, at least one member alive), nullptr otherwise (also for a unit id).
    const Container* team_target(objects::ObjectId id) const;
    // The member of `c` an attacker at `from` aims at (see above; `group` = the attacker's combat share group, `picks` the
    // squadron picks already made in this pass); kNoObjectId when no member is alive.
    objects::ObjectId team_pick(const TacticalMode& mode, const Container& c, Vec3 from, objects::ObjectId group,
                                std::vector<TeamPick>& picks) const;
    void team_attack_set(objects::ObjectId attacker, objects::ObjectId container, objects::ObjectId member, bool goal);
    void team_attack_drop(std::span<const objects::ObjectId> ids);
    void team_attack_service(TacticalMode& mode, SimRng& rng);  // after the combat tick's events
    // exec_attack's approach: a movable, unlocked unit farther than 80 % of max(range, 100) from `tp` moves to 70 %. With a
    // `goal` (the order's object block), a fighter's move names it, and in range the fighter still gets a new formation.
    void attack_approach(TacticalMode& mode, const Unit& u, Vec3 tp, SimRng& rng, objects::ObjectId goal = objects::kNoObjectId);
    commands::RejectReason handle_ability(TacticalMode& mode, uint8_t player, const commands::EventData& ev, SimRng& rng);
    void hash_queues(StateHasher& h) const;
    // --- end queueable unit actions ---
    const BattleTables::UnitVisuals* visuals_of_type(uint32_t type) const;

    const BattleCatalog* catalog_;
    const galactic::GalacticSim* galaxy_;
    const galactic::ConflictRules* rules_;
    galactic::BattleRequest req_;
    BattleConfig cfg_;
    std::vector<objects::ObjectId> engines_restarted_;  // engines_restarted() (presentation)
    bool space_ = true;
    terrain::MapData map_;
    uint32_t desc_of(uint32_t type) const;
    std::vector<uint32_t> spawn_types(const Participant& p) const;

    std::unique_ptr<BattleTables> tables_;
    std::vector<uint32_t> types_used_;  // every type spawned in this battle, first-use order
    std::vector<uint32_t> descs_;       // CombatData::units index per types_used_ entry
    // OPT3 t09: constructor / setup's table build (q6 t12: kept for script_add_types). Never null after the constructor.
    std::unique_ptr<BattleCatalog::BattleBuild> tables_build_;
    void tables_extend();  // tables_ / descs_ := the build of types_used_ (only the appended types are built)
    std::unique_ptr<tactical::movement::MovementSystem> movement_;
    std::vector<tactical::movement::ClearanceInfo> clr_types_;  // §6.3.1: per type (derived from type / model data)
    std::vector<uint8_t> clr_known_;                            // clr_types_[t] computed
    std::vector<tactical::movement::AvoidObstacle> avoid_members_;  // SPACE_AVOIDANCE: this tick's members (derived)
    std::vector<int8_t> avoid_type_;  // per type: -1 unknown, 0 not a member, 1 member, 2 member with SHIELDED
    std::unique_ptr<tactical::combat::CombatSystem> combat_;
    std::unique_ptr<tactical::abilities::AbilitySystem> abilities_;
    std::unique_ptr<BattleHooks> hooks_;  // installed over the ability system's hooks; destroyed first
    SimAnimation* anim_ = nullptr;
    int32_t dummy_type_ = -1;           // Dummy_Barrage_Target
    std::vector<Participant> parts_;
    std::vector<Unit> units_;  // ascending object id
    std::vector<Container> containers_;
    std::vector<TimedObject> particles_;  // spawn order; frame = death frame
    std::vector<TimedObject> dummies_;    // spawn order; frame = spawn frame
    // refixf t04: death clone objects (kFlagDeathClone) in spawn order; frame = battle frame of the removal from the
    // death plan (kForeverFrame: kept until the battle ends). The battle owns their removal (headless and with anim).
    std::vector<TimedObject> death_clones_;
    static constexpr uint32_t kForeverFrame = 0xFFFFFFFFu;
    SimAnimation* clone_models_ = nullptr;
#ifdef ALAMO_SIM_HAVE_ANIM
    std::unique_ptr<SimAnimation> own_clone_models_;  // made on first use when neither anim_ nor clone_models_ is set
#endif
    void schedule_death_clone(TacticalMode& mode, objects::ObjectHandle clone, int32_t clone_type, uint8_t kind, SimRng& rng);
    void remove_death_clones(TacticalMode& mode, bool all);  // due ones (expire_objects), or all (battle end)
    std::vector<AttachedEffect> effects_;
    std::vector<objects::ObjectId> story_reinforced_;  // L1/S4 wave 2b: reserve units deployed this tick
    std::vector<objects::ObjectId> ability_finished_;  // S7 t15 A1: signal 0x29 this tick (not saved: read the same tick)
    std::vector<std::pair<uint32_t, OrderReplacedListener>> order_replaced_listeners_;  // S7 t31: token order
    uint32_t next_listener_token_ = 1;
    // --- S4 garrisons / capture (owner: S4 agent; tactical_battle_garrison.cpp; no-ops without the garrison lib) ---
    std::vector<StoryGarrisonEvent> story_garrisoned_;
    std::vector<StoryCaptureEvent> story_captured_;
    void s4_owner_changed(TacticalMode& mode, objects::ObjectId id, int32_t old_owner, int32_t owner);  // change_owner()
    // s78j t04: one object's script owner change (change_owner(), else store + trees + fog / garrison hooks), and the
    // team spread of script_change_owner (TACTICAL_COMBAT §3.5 step 4): the container, then the other live members.
    bool owner_change_one(TacticalMode& mode, objects::ObjectHandle h, int32_t owner);
    void team_change_owner(TacticalMode& mode, objects::ObjectId id, int32_t owner);
    void s4_init();  // relations / neutral player of the garrison and capture systems (first registration)
    // setup; `spawned` (optional) receives the placed object (null when the spawn failed)
    bool s4_placement(TacticalMode& mode, objects::TypeIndex t, const objects::SpawnParams& sp, float health,
                      objects::ObjectHandle* spawned = nullptr);
    void s4_register(TacticalMode& mode, const Unit& u);  // deploy: holder / occupant registration
    void s4_positions(TacticalMode& mode);                // tick step 5: occupants on their holders
    void s4_tick(TacticalMode& mode, SimRng& rng);        // tick after the combat events
    void s4_cancel(std::span<const objects::ObjectId> ids);  // a new move order replaces a pending enter
    void s4_set_owner(TacticalMode& mode, objects::ObjectId id, int32_t owner);
    void s4_leave(TacticalMode& mode, objects::ObjectId unit, Vec3 at);
    void s4_hold(TacticalMode& mode, objects::ObjectId holder);  // cu8l t02: a garrison host's stop-and-hold
    void s4_apply(TacticalMode& mode);  // garrison / capture events -> movement, combat, store, owners, story feeds
    commands::RejectReason s4_command(TacticalMode& mode, uint8_t player, const commands::EventData& ev, SimRng& rng);
    void s4_hash(StateHasher& h) const;
    tactical::garrison::GarrisonSystem garrison_;
    std::unique_ptr<tactical::garrison::CapturePointSystem> captures_;
    bool s4_used_ = false;  // some garrison / capture state exists (CRC section on from then)
    objects::ObjectStore* s4_store_ = nullptr;  // the battle mode's primary store (capture rule b)
    // --- end S4 ---
    Vec3 anchor_[2]{}, facing_[2]{};
    Vec3 drop_facing_[2]{};  // a1h t06: the sides' entry-facing records (mode +0x288; zero = yaw 0), see drop_yaw
    uint32_t placed_[2] = {0, 0};
    uint32_t frame_ = 0;
    uint32_t retreat_allowed_frame_ = 0;
    int32_t retreat_side_ = -1;        // side retreating (-1 none)
    uint32_t retreat_done_frame_ = 0;  // countdown end
    bool retreat_complete_ = false;
    // story t06: victory monitor state (saved; hashed once declared, held or queued)
    bool vm_declared_ = false;
    VmRecord vm_decl_;
    int32_t vm_countdown_ = 0;
    int32_t vm_hold_ = 0;
    std::vector<std::pair<uint8_t, int32_t>> vm_queue_;  // +0x60 AwaitingVictoryTestType {type, player}
    std::vector<VmRecord> vm_records_;
    std::vector<VmTracked> vm_tracked_;                    // last scan, ascending id
    uint32_t vm_tsw_seen_ = 0;                             // l1_sw_.victory_holds() at the last sync
    uint32_t vm_declared_frame_ = 0;                       // frame_ of the declaration (the countdown runs from the next tick)
    bool vm_cp_armed_ = false;                             // ccpgt t02: mode +0x2b0..+0x2b8, the domination timer
    int32_t vm_cp_timer_ = -1;
    int32_t vm_cp_player_ = -1;
    std::vector<objects::ObjectId> vm_deaths_;             // transient: this tick's combat deaths in event order
    TacticalMode* vm_mode_ = nullptr;
    bool finished_ = false;
    galactic::BattleResult result_;
    BattleStats stats_;
#ifdef ALAMO_SIM_HAVE_AI_TACTICAL
    ai::tactical::DamageTrackingStore damage_tracking_;
#endif
    // --- fog of war (owner: fog agent; tactical_battle_fog.cpp) ---
    void fog_begin(TacticalMode& mode, uint32_t players);  // setup(): before the start forces deploy
    void fog_after_deploy(TacticalMode& mode);              // setup(): dense mask, battle scale
    void fog_decay();                                       // tick(): step 4, before movement
    void fog_service(TacticalMode& mode);                   // tick(): after consume_events
    void fog_setup_tick();                                  // tick(): after fog_service (FOWF t05 setup manager end test)
    // FOWF t05: commands 31 SetupPhaseMove / 32 SetupPhaseTriggerEnd (handle_command).
    commands::RejectReason fog_setup_command(TacticalMode& mode, uint8_t player, const commands::EventData& ev);
    void fog_owner_changed(TacticalMode& mode, objects::ObjectHandle h, int32_t old_owner);  // change_owner()
    // fog follow-up: a FOW_REVEAL_PING object spawned this tick (REVEAL source while it lives) and the S9
    // AbilityEventKind::SpawnObject handler that spawns it (Spawned_Object_Type at the target, gone next tick [M]).
    void fog_ping(objects::ObjectId id);
    void spawn_ability_object(TacticalMode& mode, const tactical::abilities::AbilityEvent& e);
    std::vector<objects::ObjectId> fog_pings_;  // fog follow-up: live ping objects, spawn order
    tactical::fog::FogObject fog_object_of(const objects::ObjectStore& store, objects::ObjectHandle h) const;
    std::vector<tactical::fog::FogSetupObject> fog_setup_objects() const;              // OI fog
    std::vector<tactical::fog::PreventionCircle> fog_prevention_circles() const;       // OI fog
    void fog_footprint_samples(const objects::ObjectStore& store, objects::ObjectHandle h, float* xy) const;  // FOWF t02
    void fog_service_sources(const objects::ObjectStore& store);                       // OI fog
    tactical::fog::FogOfWar fog_;
    TacticalMode* fog_mode_ = nullptr;
    bool fog_on_ = false;
    bool fog_global_active_ = true, fog_global_render_ = true;
    objects::BehaviorTypeId fog_reveal_beh_ = 0, fog_hide_beh_ = 0, fog_team_beh_ = 0;
    // --- end fog ---
    // --- L1 systems (owner: tactical-systems agent; tactical_battle_l1.cpp) ---
    void l1_add_types(std::vector<uint32_t>& use) const;    // constructor: the mode's special-weapon types
    void l1_setup(TacticalMode& mode);                       // setup(): the world callbacks
    void l1_register(TacticalMode& mode, objects::ObjectHandle h, int32_t type);  // deploy / adopt_object
    void l1_motion(TacticalMode& mode, SimRng& rng);         // tick step 4 (after the movement commit)
    // cu8 t02 / cu8f t07 (TACTICAL_GAPS §2.3.1a, CU8F_T01_T02_T07_RE.md §3.1): every object carrying DISABLE_FORCE_ABILITIES
    // (0x52, interval 1: every frame) with Disable_Force_Abilities_Range r > 0 stamps the Force-disable timer of every live
    // SELECTABLE battle unit of another owner within 3-D distance < r that owns an F-flag unit ability. After the movement
    // commit, before the combat tick.
    void l1_force_disable(const TacticalMode& mode);
    std::vector<float> force_disable_range_;  // per type (Disable_Force_Abilities_Range), filled on first use
    void l1_weapons(TacticalMode& mode, SimRng& rng);        // tick after consume_events
    void l1_engines_destroyed(objects::ObjectId id);         // EnginesOffline
    bool l1_engines_off(objects::ObjectId id) const { return !l1_engines_.online(id); }
    void l1_hash(StateHasher& h) const;
    uint32_t l1_desc_of(int32_t type);                       // SpecialWeaponDesc index (cache by type)
    tactical::abilities::SpecialWeaponWorld l1_world(const TacticalMode& mode);
    tactical::abilities::SpecialWeaponSystem l1_sw_;
    std::vector<std::pair<int32_t, uint32_t>> l1_desc_cache_;  // (type, desc index), lookup only
    std::vector<tactical::abilities::SpecialWeaponEvent> l1_sw_events_;
    tactical::movement::HyperspaceDepartures l1_jumps_;
    tactical::movement::EngineControl l1_engines_;
    tactical::abilities::SpaceFieldIndex l1_fields_;
    L1Constants l1_k_;
    std::vector<objects::ObjectId> l1_in_asteroid_;  // ASTEROID_FIELD_DAMAGE stamps set (ascending id)
    void l1_fields(uint32_t frame, SimRng& rng);  // BattleHooks::before_units (combat phase D start)
    // --- OI tactical: crush (MOVEMENT.md 5.5): the movement tick's CrushEvents, applied as Take_Damage at combat phase
    //     D start (BattleHooks::before_units) so the deaths are reported by this tick's combat. ---
    std::vector<tactical::movement::CrushEvent> oi_crush_;
    void oi_apply_crush(uint32_t frame);
    // --- end OI tactical ---
    // s78n t06: the hero-clash manager step (land tick step 4, after the weather; tactical_battle_clash.cpp).
    friend class BattleClashHost;
    void hero_clash_tick(TacticalMode& mode, SimRng& rng, uint64_t global_frame);
    void hero_clash_sync();  // the running pairs -> CombatSystem::set_hero_clashes (after a change and after a load)
    void hero_clash_apply_hits(uint32_t frame);  // BattleHooks::before_units: this tick's exchange damage (transient)
    std::vector<tactical::combat::DamageRequest> hero_clash_hits_;
    tactical::combat::HeroClashManager hero_clash_;
    std::shared_ptr<const tactical::combat::HeroClashData> hero_clash_data_;
    void l1_weather_tick(SimRng& rng);            // tick start (land weather service)
    tactical::combat::WeatherSystem l1_weather_;
    std::shared_ptr<const tactical::combat::WeatherData> l1_weather_data_;
    bool l1_weather_started_ = false;
    uint32_t l1_weather_env_ = 0;  // cu8g t03: the environment index the weather was (re)initialised for (not saved)
    uint32_t env_index_ = 0;       // cu8g t02: the active environment (not saved; see environment_index)
    bool fog_weather_dirty_ = false;  // Int-final: a weather FowDirty event not yet seen by fog_service
    void l1_weather_note_fow();       // Int-final: FowDirty in weather().events() -> fog_weather_dirty_
    struct L1Tracker {
        objects::ObjectId id = objects::kNoObjectId;
        float acc = 0.0f, rate = 0.0f;
        uint32_t next = 0;
    };
    std::vector<L1Tracker> l1_trackers_;                                // DAMAGE_TRACKING, ascending id
    std::vector<std::pair<objects::ObjectId, int32_t>> l1_final_blow_;  // ascending id
    void l1_damage_stats();                                             // after consume_events
    void l1_set_final_blow(objects::ObjectId id, int32_t player);
    std::vector<L1Faction> l1_factions_;
    struct L1Bombard {
        int32_t player = -1;
        uint32_t faction = 0xFFFFFFFFu;
        bool enabled = false, available = false, in_progress = false, timer_only = true, disabled = false;
        uint32_t ready = 0, interval = 0, start = 0, next_shot = 0, cursor = 0;
        int64_t deferred = -1;
        int32_t shot = 0, salvo = 0;
        Vec3 target;
    };
    struct L1Bomber {
        objects::ObjectId id = objects::kNoObjectId, marker = objects::kNoObjectId;
        Vec3 pos, dir, exit;
        float speed = 0.0f;
        uint32_t next_fire = 0;
        int32_t projectile = -1;
        float range = 0.0f, recharge = 0.0f;
    };
    struct L1BombingRun {
        int32_t player = -1;
        bool enabled = false, available = false, in_progress = false, disabled = false;
        uint32_t ready = 0, interval = 0, last_start = 0;
        int32_t bomber_type = -1;
        std::vector<L1Bomber> bombers;
        uint32_t total = 0;
    };
    std::vector<L1Bombard> l1_bombard_;
    std::vector<L1BombingRun> l1_runs_;
    int64_t l1_bombing_run_ready_frame(const L1BombingRun& r) const;  // 0x4950f0 absolute frame, -1 = disabled
    std::vector<std::pair<int32_t, int32_t>> l1_extra_projectiles_;  // (type, CombatData projectile) appended for strikes
    void l1_strikes_setup(TacticalMode& mode);  // setup(): records
    void l1_strikes(TacticalMode& mode, SimRng& rng);  // tick after consume_events
    void l1_add_strike_projectiles();  // constructor: Bombardment_Projectile types appended to the tables
    int32_t l1_projectile_of_type(int32_t type) const;
    bool l1_base_shield_covers(Vec3 p) const;
    std::vector<int32_t> l1_retreat_increments_;
    std::vector<float> l1_flight_increments_;
    TacticalMode* l1_mode_ = nullptr;
    bool l1_used_ = false;               // some pack / jump / engine entry existed (CRC section on from then)
    bool l1_planet_destroyed_ = false;
    // TSW planet destruction (TSW_PLANET_DESTROY): this tick's state-9 TSW owner (not saved: the host reads it in the
    // same tick), the story victory 0x10 pending at the hold release, the saved dummy transforms for the state-10 debris.
    struct L1TswDebris {
        objects::ObjectId tsw = objects::kNoObjectId;
        int32_t type = -1;  // the dead dummy's type (Death_By_TSW_Replacements)
        Vec3 pos{}, facing{};
    };
    int32_t l1_tsw_kill_owner_ = -1;
    int32_t l1_tsw_story_winner_ = -1;
    std::vector<L1TswDebris> l1_tsw_debris_;
    void l1_tsw_destruction(TacticalMode& mode, const tactical::abilities::SpecialWeaponEvent& e);
    void l1_tsw_debris(TacticalMode& mode, const tactical::abilities::SpecialWeaponEvent& e);
    // --- end L1 systems ---
    // --- Int-final: build pads (tactical_battle_pads.cpp) ---
    struct PadState {
        objects::ObjectId pad = objects::kNoObjectId;
        objects::ObjectId uc = objects::kNoObjectId, built = objects::kNoObjectId;
        int32_t builder = -1;
        uint32_t uc_end = 0, cooldown_start = 0, cooldown_end = 0;
        bool locked = false;
        objects::TypeIndex built_type = objects::kNone;  // §6.5: the built child's type (persistence removal)
    };
    // §9.4 galactic persistence add (0x4b6c70, `pos` = the finished object's position) / remove (0x4bb3a0, `pos` = the
    // child's position) behind the gate 0x42f160 (set_pad_persistence).
    void pad_persist(const objects::ObjectStore& st, objects::ObjectHandle pad, objects::TypeIndex type, const Vec3& pos, bool add);
    // §9.4: the pad's base level byte (+0x39C, map mini 0x0B) of map-placed pads with a non-zero level, ascending pad id
    // (bt_map_load; derived from the map, so not saved: a load's setup rebuilds it). Other pads: 0.
    std::vector<std::pair<objects::ObjectId, int32_t>> pad_levels_;
    int32_t pad_base_level(objects::ObjectId pad) const;
    std::vector<PadState> pads_;  // ascending pad id
    bool pads_used_ = false;
    TacticalMode* pads_mode_ = nullptr;
    PadState* pad_state(objects::ObjectId pad, bool create);
    const PadState* pad_state(objects::ObjectId pad) const;
    void pads_tick(TacticalMode& mode);
    void pads_hash(StateHasher& h) const;
    // --- end Int-final ---
    // --- battle host (BT pass) ---
    void bt_add_pad_types(const terrain::MapData& map);       // setup(): UC / constructed types of the map's pads -> tables
    void bt_force_split();                                     // constructor: land split, space super-weapon rule
    // `map`: a map-placed object (§3.1): movable when it has a locomotor; `third` when owned by neither side.
    // OI host: `container` (index into containers_) = a REPLENISH_WINGMEN member joining that team (movable, member of
    // the container in the ability system, the container's live count raised).
    uint32_t bt_adopt_built(TacticalMode& mode, objects::ObjectHandle h, int32_t owner, float health_fraction, SimRng* rng, bool map = false,
                            uint32_t container = 0xFFFFFFFFu);
    void bt_pad_health();                                      // UC health ramp (pads_tick)
    void bt_retreat_attrition(uint8_t side, SimRng& rng);      // countdown end, before complete_retreat
    void bt_apply_attrition(uint32_t frame);                   // BattleHooks::before_units (deaths reported this tick)
    void bt_battle_end();                                      // finish(): lead object step 4
    void bt_hash(StateHasher& h) const;
    bool bt_is_super_weapon(uint32_t type) const;
    std::vector<uint8_t> vr_cache_;  // victory_relevant() per type
    // Land: live BASE_SHIELD objects (Base_Shield_Radius > 0) are secondary occupants of the cells in their radius
    // (PassabilityGrid::stamp_secondary_circle, kPassShield [inferred]); unstamped when they die / leave. Setup + per tick.
    void bt_shield_stamps(const TacticalMode& mode);
    // §3.4 team helper: the TEAM object type (Create_Team_Type, else "Team"; 0xFFFFFFFF none), the TEAM behaviour type
    // (the team type, or the company type when Create_Team_Type is empty) and the chunk size (company Max_Squad_Size,
    // default 10). False when the helper does not run (land and the first member's type has no Create_Team).
    bool bt_team(uint32_t company_type, uint32_t first_member, uint32_t& object_type, uint32_t& behavior_type, uint32_t& squad) const;
    void bt_landing_points(TacticalMode& mode);  // setup, after the deploys: the attacker's map-owned reinforcement points
    // --- map load / post-load placement (REMAINING_GAPS §3.1 / §3.2; tactical_battle_bt.cpp) ---
    struct MapMarker {
        objects::ObjectId id = objects::kNoObjectId;
        objects::TypeIndex type = objects::kNone;
        uint8_t kind = 0;  // MarkerKind (tactical_battle_bt.cpp)
        int32_t owner = -1;
        Vec3 pos{}, facing{1.0f, 0.0f, 0.0f};
        std::vector<objects::TypeIndex> specific;  // Marker_For_Specific_Object_Type
        bool used = false;
    };
    std::vector<MapMarker> bt_markers_;  // MARKER objects with an owner, map (GOM list) order
    // S10 land outcome: the start markers for the AI's IsFriendly / IsEnemyStartLocation (AI_TACTICAL_PERCEPTION §3):
    // (true = Defending Forces Position, false = Attacker Entry Position, position), map order.
    std::vector<std::pair<bool, Vec3>> bt_start_markers() const;
    uint32_t bt_players_ = 0;            // the host's player count (valid map owner ids)
    void bt_map_prepass();                                  // setup: base-level filter + discard rule, map types -> tables
    void bt_map_load(TacticalMode& mode, SimRng& rng);      // setup: spawn the kept placements (spawn-table draws)
    void bt_place_forces(TacticalMode& mode, SimRng& rng, const ParallelFor& pf);  // setup: §3.2 steps 1-7 + the start-force deploys
    bool bt_owner_valid(int32_t owner) const;
    bool bt_owner_neutral(int32_t owner) const;  // owner's faction Is_Neutral
    // S7 t30 (TACTICAL_COMBAT §2.10): the start value of the relation table entry (row p, column q): 0 friendly, 1 enemy,
    // 2 neutral. Skirmish (cfg_.skirmish) or campaign rules; the combat matrix's enemy flag = (value == 1).
    int32_t bt_relation(int32_t p, int32_t q) const;
    bool bt_allied(int32_t p, int32_t q) const { return bt_relation(p, q) == 0; }  // 0x39a230 / 0x39a2c0 (AI_TACTICAL §7.5.1)
    // q6 t19 rule 0x42e400 per object (alive + Influences_Capture_Point tested by the caller): no BASE_SHIELD, no
    // DUMMY_GROUND_STRUCTURE, owner not allied with `pad_owner`.
    bool pad_rule_blocker(const objects::ObjectStore& st, objects::ObjectHandle h, int32_t pad_owner) const;
    int32_t bt_map_owner(uint32_t raw) const;    // .ted owner (faction index) -> player [probe 2026-10-29]; -1 = unknown
    void s4_capture_grant(TacticalMode& mode, objects::ObjectId point, int32_t owner);  // landing point -> attacker
    void bt_kill_silently(TacticalMode& mode, objects::ObjectHandle h);  // 0x391310 flag 1 (no death events)
    void s4_capture_reset(TacticalMode& mode, objects::ObjectId pad);   // §6.5: the pad's capture point -> neutral
    Vec3 bt_pad_attach_point(const TacticalMode& mode, objects::ObjectHandle pad) const;  // §6.5 UC attachment bone
    std::vector<PadRecord>* pad_records_ = nullptr;  // §9.4 galactic persistence (GalacticSim::built_objects of the planet)
    bool pad_restore_ = true;                         // §9.4 recreate at setup (false while a save is loaded)
    void bt_pad_restore(TacticalMode& mode);          // setup: persistent pad children back on their pads
    void bt_skirmish_base(TacticalMode& mode);        // t50 setup: the land skirmish's pre-built base (command center, barracks)
    void bt_skirmish_anchors();                       // t50 setup: land skirmish start positions from the slot's map markers
    // skai: skirmish map battles (tactical_battle_skirmish.cpp). Per-type Income_Stream data (parsed on first use, by type).
    struct SkIncome {
        bool parsed = false;
        float value = 0.0f, interval = 0.0f, multiplier = 1.0f, additive = 0.0f;
    };
    std::vector<SkIncome> sk_income_;
    const SkIncome& sk_income_of(uint32_t type);
    void sk_income(const TacticalMode& mode);           // tick: the Income_Stream credit trickle
    void bt_team_positions(TacticalMode& mode);                            // tick, after the movement commit
    std::vector<RemovalRecord> bt_removals_;
    std::vector<objects::ObjectId> bt_landing_;
    std::vector<objects::ObjectId> bt_shields_;  // stamped shield objects, ascending id
    std::function<float(int32_t)> credits_get_;
    std::function<void(int32_t, float)> credits_add_;
    std::function<bool()> arrival_hold_;  // story t12
    SimRng* bt_rng_ = nullptr;                 // the host's synced RNG (setup / commands): pad objects' add_unit draws
    std::vector<objects::ObjectId> bt_attrition_ids_;
    uint32_t bt_attrition_kills_ = 0;
    objects::ObjectId bt_lead_ = objects::kNoObjectId;
    uint32_t bt_lead_participant_ = 0xFFFFFFFFu;
    bool bt_raid_ = false, bt_sw_killed_ = false, bt_used_ = false;
    bool bt_have_ds_ = false;  // DEATH_STAR_POSITION marker found (space maps)
    Vec3 bt_ds_pos_{};
    struct UcRamp {
        objects::ObjectId id = objects::kNoObjectId;
        float rate = 0.0f;  // health per frame
    };
    std::vector<UcRamp> bt_ramps_;  // ascending id
    // --- end BT pass ---
    // --- OI abilities (owner: tactical abilities agent; tactical_battle_oi_abilities.cpp): the host side of
    // docs/re/TACTICAL_GAPS.md §1-§2 / §7 - ability host hooks, ability-spawned objects, Destruction_Survivors /
    // Tactical_Respawn, ability move / stop / owner actions ---
    void oi_add_ability_types();                       // setup(): spawn / survivor types the abilities name -> tables
    void oi_install(TacticalMode& mode, SimRng& rng);  // setup(): AbilityHost, RNG, credit hooks (after the combat hooks)
    void oi_events(TacticalMode& mode);                // consume_events(): ability actions, spawn requests, survivors
    void oi_respawn_due(TacticalMode& mode);           // tick(): due Tactical_Respawn creations
    // S8 t11 / t12 (MOVEMENT §3.2): drains MovementSystem::take_deploy_events() - each Walk deploy-state change sets DEPLOY
    // Is_Active / kUnitDeploying / kUnitDeployTransition (AbilitySystem::set_deploy_state), and each change with `finished`
    // (a deploy / undeploy clip end, a blocked reverse) is "unit ability finished" (signal 0x29) in ability_finished_.
    // Called after the movement tick (clip ends, before the combat flags are read) and after oi_events (requests).
    void oi_deploy_sync();
    objects::ObjectId oi_spawn_unit(TacticalMode& mode, int32_t type, Vec3 pos, Vec3 facing, int32_t owner);
    // q6 t12: script-spawned types (and, for a company, its members and TEAM types) join the battle tables mid-battle:
    // appended to types_used_ and built onto the live tables (tables_extend on the kept setup build), then the ability /
    // survivor types they name (oi_add_ability_types). [port] Their hangar descriptors (hg_add_types) are not added.
    void script_add_types(std::span<const uint32_t> types);
    void oi_hash(StateHasher& h) const;
    bool oi_company_members(int32_t type, std::vector<int32_t>& members) const;  // §5.2 Company_Units / Squadron_Units
    struct OiRespawn {
        uint32_t due = 0;
        int32_t type = -1, owner = -1;
        Vec3 pos{}, facing{1.0f, 0.0f, 0.0f};
    };
    std::vector<OiRespawn> oi_respawns_;  // event order
    uint64_t oi_global_frame_ = 0;  // the host frame of the current tick (animation time base for ability spawns)
    // World position of bone `bone` (any case) of `h`'s model: bind pose x Scale_Factor, rotated by the yaw [M: static
    // pose]. > 0 found, 0 the model lacks the bone, < 0 no model.
    int model_bone_point(const objects::ObjectStore& st, objects::ObjectHandle h, std::string_view bone, Vec3& out) const;
    bool s4_board(TacticalMode& mode, objects::ObjectId unit, objects::ObjectId holder);  // Vehicle_Thief: board at once
    // --- end OI abilities ---
    // --- hangars / station upgrade / engine loops (tactical_battle_hangar.cpp) ---
    bool hg_is_spawner(uint32_t type) const;
    void hg_add_types();                         // setup(): spawn lists, members, team types, Next_Level_Base -> tables
    void hg_setup(TacticalMode& mode);           // setup(): HangarSystem (after combat / abilities)
    void hg_ability_host(tactical::abilities::AbilityHost& h);  // oi_install(): DEPLOY_SQUAD hooks
    int32_t hg_tech(int32_t player) const;
    bool hg_engine_unit(const Unit& u) const;    // SimpleSpace engine-loop unit
    bool hg_walk_engine_unit(const Unit& u) const;  // cu8g t01: land Walk-locomotor engine-loop unit
    void hg_adopt(const Unit& u, SimRng& rng);   // after add_unit: holster attach, engine mode enter, add_carrier
    void hg_unit_removed(objects::ObjectId id);  // death / silent kill / retreat / container removal
    void hg_engine_service();                    // tick(): after the movement commit
    // cu8g t01 HOLSTER_WEAPON (CU8F_T01_T02_T07_RE.md §2.3; EngineLoops holds the state): per-type data (lazy, derived,
    // not saved), the attach at adopt, the per-frame service (tick(): after the hangar services), the Saber_Throw throw.
    struct HgHolsterType {
        uint8_t known = 0, has_bone_name = 0;
        tactical::hangar::HolsterDesc desc;
        std::string bone;
    };
    const HgHolsterType* hg_holster_type(objects::TypeIndex t);
    void hg_holster_attach(const Unit& u);
    void hg_holster_service();
    // cu8k t01 (CU8F_T01_T02_T07_RE.md §2.7 items 1-4): Saber_Throw's thrower machine. The apply records the target point
    // T (event point); kSaberThrowDelay frames later the throw spawns the Saber_Name projectile (CombatSystem::
    // launch_saber); from the throw + 6 (SWORD_CONTROL) the thrower ends the ability as soon as the saber is gone, or
    // catches it within Catch_Dist (3D) and ends 11 frames later (SWORD_CATCH). Serviced after the combat tick (the saber
    // steps before the thrower's update, as retail's service order: epsilon 0) and before hg_holster_service, which runs
    // the EngineLoops throw / catch snaps. Saved in the battle state (version 10).
    struct SaberThrowState {
        objects::ObjectId thrower = objects::kNoObjectId, target = objects::kNoObjectId;
        Vec3 aim;                       // T: the target point at the apply
        uint32_t throw_frame = 0;       // the throw (spawn) frame
        uint32_t serial = 0;            // the saber projectile, 0 = not thrown yet
        int32_t projectile = -1;        // its ProjectileDesc
        float catch_dist = 0.0f;
        uint32_t end_frame = 0;         // 0 = still flying / waiting; set at the catch (catch + 11)
    };
    std::vector<SaberThrowState> sabers_;  // apply order
    // --- cu8l t03: the campaign space fleet placement 0x4fbb10 and the start fleet's hyperspace arrival
    // (CU8K_T03_T08_T09_RE §1, §4; tactical_battle_bt.cpp) ---
    struct FleetEntry {      // a registration (object, group) of the campaign spawn 0x2a00b0 (0x4fc7e0)
        uint32_t unit = 0xFFFFFFFFu;       // index into units_ (a unit registered as itself), else
        uint32_t container = 0xFFFFFFFFu;  // index into containers_ (the first chunk's TEAM container)
        uint32_t group = 0;                // group k: anchor k mod (marker count)
    };
    void bt_fleet_placement(TacticalMode& mode, SimRng& rng, const std::vector<FleetEntry>& reg, const std::vector<size_t>& markers);
    void bt_fleet_arrival(TacticalMode& mode, SimRng& rng, const std::vector<FleetEntry>& reg);
    tactical::movement::fleet::HalfBox fleet_box(uint32_t type) const;            // cull-box half extents [M: model bounds]
    std::vector<tactical::movement::V2> fleet_offsets(uint32_t type) const;       // Squadron_Offsets (x, y)
    // The start fleet's hyperspace arrival (§1.9, L1_S4_REMAINING §3.4 "Hyperspace in") [port stand-in: the units are not
    // moved, hidden or suspended; only the timer, its synced draw, the movement lock and the end's new coordinator are
    // modelled]. Unit record (container = none): timer kArrivalState5 (the vf82 call: the first service turns it to
    // state 4), kArrivalState4 (state 4's first frame: timer := clamp(int(1, 4) + 5 trunc(4 f), 0, 24), one synced draw),
    // then += 1 per frame unless the hyperspace hold is on; > 149: the finish. `composes`: a ship outside a team (its finish
    // 0x624dc0: lock released, a new coordinator holding its position - one space composition at the next step 10).
    // Container record: the TEAM container's lock (taken by the placement's vf82 0x62f800), released once none of its
    // members' records is left (0x630390: a new coordinator - one composition). Ascending id per kind.
    struct ArrivalRec {
        objects::ObjectId id = objects::kNoObjectId;
        uint32_t container = 0xFFFFFFFFu;  // containers_ index of a container record
        int32_t timer = 0;
        bool composes = false;
    };
    static constexpr int32_t kArrivalState5 = -2, kArrivalState4 = -1;
    std::vector<ArrivalRec> arrivals_;  // saved (battle state version 11)
    void arrival_service(TacticalMode& mode, SimRng& rng);  // tick: after the L1 motion services, before step 10
    bool arrival_locked(objects::ObjectId key) const;        // MovementSystem::set_locked_fn
    uint32_t state_io_version_ = 0;  // the battle state version state_io reads (0: the current one)
    void saber_throw_service(TacticalMode& mode);
    bool anim_engaged_clip(objects::ObjectId id) const;  // tactical_battle.cpp: current clip ATTACK (5) / SPINMOVE (0x68)
    std::vector<HgHolsterType> hg_holster_types_;
    const TacticalMode* hg_mode_ = nullptr;  // set by hg_setup (before the map load)
    void hg_tick(TacticalMode& mode, SimRng& rng);  // tick(): after the combat tick and its events
    void hg_station_upgrade(TacticalMode& mode, const tactical::abilities::SpawnRequest& q);
    void hg_publish_engine_requests();           // tick() end
    void hg_hash(StateHasher& h) const;
    // station contents / tactical production
    void hg_production(TacticalMode& mode, SimRng& rng);  // tick(): before the object services
    commands::RejectReason hg_production_command(TacticalMode& mode, uint8_t player, const commands::EventData& ev);
    objects::ObjectId hg_spawn(TacticalMode& mode, int32_t type, int32_t owner, Vec3 origin, Vec3 facing, Vec3 ring_center,
                               bool launched, SimRng& rng, std::vector<objects::ObjectId>& ordered);
    // 0 ok, 1 not buildable here, 4 prerequisite missing, 5 build limit reached (BuildOptionInfo::block values).
    uint8_t hg_build_check(const objects::ObjectStore& st, int32_t player, objects::ObjectId builder, objects::TypeIndex type) const;
    bool hg_buildable(const objects::ObjectStore& st, int32_t player, objects::ObjectId builder, objects::TypeIndex type) const {
        return hg_build_check(st, player, builder, type) == 0;
    }
    void hg_refund(uint32_t order, bool pay);    // drops the order's charge record; pay = credit it back
    void hg_remove_silently(objects::ObjectId id);  // RemoveSilently: contents / abilities bookkeeping of a non-unit
    struct HgOrderCost {
        uint32_t order = 0;
        int32_t player = -1;
        float cost = 0.0f;
    };
    struct HgUpgradeObject {
        objects::ObjectId id = objects::kNoObjectId;
        objects::ObjectHandle h;
    };
    std::unique_ptr<tactical::hangar::ContentsSystem> contents_;
    std::unique_ptr<tactical::hangar::TacticalProduction> production_;
    std::vector<HgOrderCost> hg_order_cost_;                  // charged credits per open order (issue order)
    std::vector<HgUpgradeObject> hg_upgrade_objs_;            // DUMMY_UPGRADE objects created by production (creation order)
    std::vector<objects::ObjectId> hg_orphans_;               // contents of a removed container: removed next frame
    bool prod_used_ = false;
    std::unique_ptr<tactical::hangar::HangarSystem> hangars_;
    std::vector<int32_t> hangar_desc_;           // TypeIndex -> HangarDesc index (-1 none)
    tactical::hangar::EngineLoops engine_loops_;
    std::vector<tactical::hangar::EngineLoopRequest> engine_loop_requests_;
    std::vector<int32_t> hg_tech_bonus_;         // per player: station-upgrade tech levels in this battle [port]
    std::vector<StationUpgrade> hg_upgrades_;
    uint32_t hg_launches_ = 0;
    bool hg_used_ = false;                       // hashed once a carrier / upgrade existed
    // --- end hangars ---
};

// --- P4 tactical: campaign Black Market gate (ABILITIES_HARDPOINTS 2.4, P4_SIM_SYSTEMS 1.5) ---
// The ability names of `data` that are blocked for galactic player `player`: every unit-ability record
// (GUI_Activated_Ability_Name) and special ability (Name) with Must_Be_Bought_On_Black_Market for which
// GalacticSim::bm_allows_unit_ability / bm_allows_special_ability (special_name_crc of the name as written) is false.
// Returned as tactical name_crc values (AbilitySystem::set_black_market_blocked), sorted and unique. TacticalBattle
// (always a campaign battle: skirmish = false) sets every battle player's list at setup, before the deploys.
std::vector<uint32_t> black_market_blocked_abilities(const tactical::abilities::AbilityData& data,
                                                     const galactic::GalacticSim& galaxy, int32_t player);
// --- end P4 tactical ---

}  // namespace alamo::sim

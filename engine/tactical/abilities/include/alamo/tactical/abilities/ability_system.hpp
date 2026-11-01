// S9 - AbilitySystem: unit abilities (Unit_Abilities_Data slots, countdowns, command 43 / 40 / 57 /
// 58 paths, Can_Activate gates, Is_Active, Mod_Multiplier via combat system B) and the special
// abilities this pass covers (tractor beam, ion cannon shot, lucky shot, concentrate fire, energy
// weapon, stealth, sensor jamming, laser defense, interdict, missile shield, redirect / absorb
// blaster listeners). Spec: docs/re/ABILITIES_HARDPOINTS.md (sections in the comments below).
//
// WIRING
//   * Wraps one CombatSystem and installs itself as its CombatHooks; the host keeps calling
//     CombatSystem::tick (or AbilitySystem::tick, the same thing). All ability work happens inside
//     the combat tick's serial phases (begin_tick, phase D per unit, fire / damage / death hooks),
//     so it is identical for any worker count.
//   * Objects: every combat unit with abilities is added through add_unit() (which adds the combat
//     unit: hardpoints are created there with their one synced draw each, HardPoints order), and
//     team containers (squadrons) through add_container(). Kept sorted by id; all-object scans run
//     in ascending id (port choice, 6.3).
//   * Commands (command phase, before the tick): set_unit_ability_mode (43), targeted_ability
//     (40 / 57 with a host-spawned dummy target), set_autofire (58). Lua Activate_Ability /
//     Cancel_Ability execute the same calls directly.
//   * Per unit inside the tick (6.2): countdown (unit_begin) -> targeting -> pending targeted ability
//     + special-ability updates (unit_after_targeting) -> hardpoints. Team containers are serviced
//     before the units (before_units, descending id) [port].
//
// RNG: no draw anywhere in the ability framework (6.1); the only synced draws S9 adds are in combat
// (hardpoint creation, death projectile, converted decay) and in the Take_Damage listeners here
// (redirect / block rolls, TACTICAL_COMBAT 3.8).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/tactical/abilities/ability_data.hpp"
#include "alamo/tactical/abilities/countdown.hpp"
#include "alamo/tactical/combat/combat_system.hpp"

namespace alamo {
class StateHasher;
class CrcRegistry;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::tactical::abilities {

using combat::kNoObject;
using combat::ObjectId;

struct AbilityConfig {
    GameMode mode = GameMode::Space;
    bool skirmish = true;                       // Skirmish_Automatic styles; black-market gate passes
    uint32_t fps = combat::kDefaultFps;
    float nebula_ability_disable_time = 5.0f;   // GameConstants Nebula_Ability_Disable_Time
    float force_ability_disable_time = 0.0f;    // Force_Ability_Disable_Time
    float ion_storm_shield_disable_time = 5.0f; // Ion_Storm_Shield_Disable_Time
    float stealth_detection_time = 0.0f;        // GameConstants +0x12094 (not in vanilla XML) [L]
    float stealth_fire_fade = 0.3f;             // break on firing (0x3a06a0)
    uint64_t corvette_category = 0;             // CategoryMask bit of "Corvette" (tractor x2 rule)
    bool abilities_enabled = true;              // global abilities-off flag (0x37c270)
};

enum class AbilityEventKind : uint8_t {
    Activated,       // Set_Unit_Ability_Active(on) succeeded
    Deactivated,     // ... (off)
    Ready,           // signal 0x2c: recharge finished and Can_Activate passes
    Ended,           // signal 0x29: active phase expired / special effect removed
    Unhandled,       // type or special class without behaviour in this module (host / later work)
    TargetIssued,    // command 40/57 accepted for this user: attack order + pending ability
    PendingFired,    // the pending targeted ability applied (target in range)
    Immobilised,     // Disable_Movement counter 0 -> 1 (host stops the unit)
    Released,        // ... 1 -> 0
    InterdictOn,     // story event Interdict_Activated
    InterdictOff,
    StealthOn,
    StealthBroken,
    StealthDetected,
    EffectOn,        // special ability applied (beam / particle visuals)
    Zap,             // laser defense destroyed a projectile (target = shooter of it)
    Redirected,      // Take_Damage listener cancelled a hit
    Blocked,
    // Last functioning ENGINE hardpoint went offline (movement: x Engines_Disabled_Speed_Modifier). `value` 1 = it was
    // destroyed (Locomotor_Set_Engines(destroyed = 1): EngineControl::destroy, permanent), 0 = disabled by the
    // station-upgrade copy (destroyed = 0, t < 0: EngineControl::disable(id, -1)) (HARDPOINT_DISABLED.md §9.1).
    EnginesOffline,
    SelfDamage,      // STIM_PACK self damage
    // Fog follow-up: FOW_REVEAL_PING activated (FOG_OF_WAR.md §3.3). The host spawns the record's
    // Spawned_Object_Type (record(id, type)->spawned_object_type) at `target`'s position (the user's own when
    // target = id) for the user's owner; the spawned object reveals through its own REVEAL behaviour [M].
    SpawnObject,
    // ---- OI abilities: host actions, in event order after the tick (the sim state is already changed) ----
    MoveOrder,      // move `id` to `point` (0x2a7b80: HUNT, JET_PACK, CONFUSE, BERSERKER, distract) - not an order cancel
    StopOrder,      // stop `id` (0x426d50 / 0x39bcb0: DEPLOY, mines, melee holds)
    OwnerChanged,   // `id` now belongs to player `value` (Hack, Tactical_Bribe, convert): store / trees / movement
    Deployed,       // DEPLOY on (`value` 1) / off (0): the host's DeployedBehavior / model state
    MineTriggered,  // mine `id` (bound object, may be kNoObject) triggered at `point`: dies and detonates [M]
    Stunned,        // land stun of `id` (`value` frames; 0 = released): movement frozen while combat().stunned()
    // An ENGINE hardpoint of `id` was repaired to max (Set_System_Online online, 0x393980; no live-count check): the
    // host calls EngineControl::repair_online (online := 1, permanent := 0, timer := -1, signal 0x17 when they were
    // off). TURBO / SPOILER_LOCK stay off (HARDPOINT_DISABLED.md §9.1).
    EnginesOnline,
    // STATION_HANGARS_SOUND.md §1.1: remove `id` silently (Kill 0x391310 flag 1: no death clone / explosion /
    // survivors). Starbase_Upgrade_Ability: the upgrade object after its one service, whether or not the upgrade ran.
    RemoveSilently,
};

struct AbilityEvent {
    AbilityEventKind kind = AbilityEventKind::Activated;
    uint8_t type = 0;  // unit-ability type (0 if not tied to one)
    ObjectId id = kNoObject;
    ObjectId target = kNoObject;
    uint32_t frame = 0;
    Vec3 point;         // OI abilities: MoveOrder / MineTriggered position
    int32_t value = 0;  // OI abilities: OwnerChanged owner, Deployed state, Stunned frames
};

// OI abilities: an object the host creates for an ability AFTER the tick, in request order (each consumes an object
// id; objects are never created inside the combat tick). Draws that pick positions are made in the tick.
enum class SpawnPurpose : uint8_t {
    InstantEffect,  // 0x3a3e20 Spawned_Object_Type that is not a known projectile (it detonates / acts on its own) [M]
    Mine,           // Proximity_Mines Mine_Type: bind_spawn() makes it a mine (trigger radius, arm frame)
    Wingman,        // REPLENISH_WINGMEN member of `container`
    Troopers,       // DEPLOY_TROOPERS member i of Spawned_Object_Type at Owner_Attachment_Bone i (REMAINING_GAPS §5.2)
    CaptureVariant, // Vehicle_Thief: the victim's capture variant, owner = the victim's owner (the thief boards it)
    Summon,         // Summon / Spawn / Buzz_Droids / Radioactive Object_Type after its delay
    BarrageTarget,  // BARRAGE Dummy_Barrage_Target owned by the shooter's first enemy (the host issues the attack)
    // Starbase_Upgrade_Ability service (0x70ec20 -> Do_Starbase_Upgrade 0x70f070, STATION_HANGARS_SOUND.md §1.2):
    // `source` = the station S to replace, `target` = the upgrade object, `object_type` = -1 (the host resolves S's
    // Next_Level_Base; none -> no upgrade), position / facing / owner = S's. Emitted before the RemoveSilently event
    // of the upgrade object. Host order (§1.6): spawn S' -> hangar::complete_station_upgrade (hardpoint copy by index,
    // +0x3CB spawn flag copy, S dropped from the hangar) -> move S's contents to S' in list order (the upgrade object
    // included) -> per player (index order) selection and control groups -> tech +1 for the upgrade object's owner,
    // then for every ally (player order), each with its tactical production builder id S -> S' -> remove S silently
    // -> 2D announcement (own / ally / enemy) -> remove the upgrade object silently. Hull, shields, cooldowns and
    // hangar counts are not carried over.
    StationUpgrade,
};
struct SpawnRequest {
    uint32_t serial = 0;
    SpawnPurpose purpose = SpawnPurpose::InstantEffect;
    int32_t object_type = -1;  // host TypeIndex (-1: the host resolves it from the record / ability)
    Vec3 position, facing{1.0f, 0.0f, 0.0f};
    int32_t owner = -1;
    ObjectId source = kNoObject;  // the user
    ObjectId target = kNoObject;  // the victim (capture variant) / the container (wingman)
    uint8_t ability_type = 0;
    float fuse_seconds = -1.0f;   // BOMB countdown for spawned bombs
    // Troopers (REMAINING_GAPS §5.2): index into the record's Owner_Attachment_Bone list. The host spawns `object_type`
    // at `position` (the bone) with the owner's facing, runs locomotor vf +0x200, plays anim 0x5f (0 when missing),
    // attaches it at that bone of `source` (0x38d320), sets obj +0x3a2 |= 2, then calls bind_spawn (trooper list).
    int32_t bone = -1;
};

// OI abilities: host queries (all optional; unset ones use the documented fallbacks). Called from the serial tick
// phases; they must not create or remove objects.
struct AbilityHost {
    // FOW_Point_Test / FOW_Object_Hidden for `player` with `force` (fallback: nothing fogged / hidden).
    std::function<bool(int32_t player, Vec3 p, bool force)> point_fogged;
    std::function<bool(int32_t player, ObjectId id, bool force)> object_hidden;
    // The mode's map rectangle (vf +0x1f8, else the playable box 0x2cea10); fallback: the units' bounding box.
    std::function<bool(Vec3& lo, Vec3& hi)> map_rect;
    std::function<bool(ObjectId id)> moving;     // locomotor moving (fallback: position changed since last tick)
    std::function<float(float x, float y)> ground_height;  // terrain z for mines (fallback: the point's z)
    // Battlefield_Modifier list +0x1f8 (TacticalBattle::fog_add_reveal_multiplier / fog_remove_reveal_modifier).
    std::function<void(int32_t player, int32_t category, uint64_t source, float multiplier)> fow_reveal_add;
    std::function<void(int32_t player, uint64_t source)> fow_reveal_remove;
    // ---- REMAINING_GAPS §5 ----
    // §5.1: the first occupant of `vehicle`'s container list (obj +0xf8 -> list +0x20), kNoObject when empty. Unset:
    // no occupant (EJECT_VEHICLE_THIEF does nothing).
    std::function<ObjectId(ObjectId vehicle)> first_occupant;
    // §5.2: the member types (host TypeIndex, list order) of `type`'s Company_Units (+0x958), else Squadron_Units
    // (+0x978); false when it has neither (DEPLOY_TROOPERS fails, state 10). Unset: [port] `type` is its own single
    // member.
    std::function<bool(int32_t type, std::vector<int32_t>& members)> company_members;
    // §5.2: world position of bone `bone` of `owner`'s RENDER model (0x12d2a0 / 0x12d2c0): > 0 found (writes `pos`),
    // 0 the model lacks the bone (skipped), < 0 no model (DEPLOY_TROOPERS fails, state 10). Unset: [port] every bone
    // is found at the owner's position.
    std::function<int(ObjectId owner, std::string_view bone, Vec3& pos)> bone_position;
    // Starbase_Upgrade_Ability (STATION_HANGARS_SOUND.md §1.2 step 1): the parent container (obj +0x2b0) of an upgrade
    // object that is not itself a station, kNoObject when none (no upgrade). Unset: [port] the object's team container,
    // else the lowest-id live DUMMY_STAR_BASE unit (UnitDesc::repairs_hardpoints) of the same owner.
    std::function<ObjectId(ObjectId upgrade)> upgrade_container;
    // DEPLOY_SQUAD (75) with the hangar module (STATION_HANGARS_SOUND.md §2.5, ABILITIES_HARDPOINTS §2.4 / §2.5):
    // gate = the carrier's SPAWN_SQUADRON state is 4 (hangar::HangarSystem::deploy_ready); handler = reset it and set
    // its next-ready frame (hangar::HangarSystem::deploy_squad(id, frame, Recharge_Seconds)), then the recharge starts.
    // Unset: [port] no state gate, and the generic countdown path (Unhandled event) as before.
    std::function<bool(ObjectId carrier)> squadron_deploy_ready;
    std::function<void(ObjectId carrier, uint32_t frame, float recharge_seconds)> squadron_deploy;
    // FIGHTER_FLYING §6.5: Cable_Ability start (CableAttackAbilityClass 0x6f34d0): after the recharge starts, the user
    // (a unit or its team container) starts circling the cable target between Activation_Min_Range and
    // Activation_Max_Range of the ability record (the host: MovementSystem::start_circling). Unset: no circling.
    std::function<void(ObjectId user, ObjectId target, float r_min, float r_max)> cable_attack;
    // OI t02 (ABILITIES_HARDPOINTS §4.12 [port]): the map has a water plane above the terrain under `p` (XY). Unset: no
    // water anywhere (Destroy_When_Stunned_Over_Water never fires).
    std::function<bool(Vec3 p)> over_water;
    // OI t03 (ABILITIES_HARDPOINTS §4.13 [port]): frame count and fps of animation `anim_type` (a ModelAnimType id; < 0:
    // the XML name `anim_name`, e.g. an Attack_Animation / Toss_Anim value) of `id`'s sim model, variant 0. Static model
    // data only, never a playing clock. False when the object has no model or the model lacks the clip. Unset: no clips
    // (every animation-driven timing uses its fixed fallback).
    std::function<bool(ObjectId id, int32_t anim_type, std::string_view anim_name, uint32_t& frame_count, float& fps)> anim_clip;
};

// OI abilities: the per-player tactical lists Battlefield_Modifier writes (§2.3.11) besides the reveal range.
enum class PlayerList : uint8_t {
    ReinforcementTime = 0,    // +0x230: integer frame delta round(F x m) - F
    StealthDuration = 1,      // +0x268: m - 1 (stealth apply, max-then-sum)
    CaptureTime = 2,          // +0x188 -> +0x288: m - 1
    ReinforcementDeploy = 3,  // +0x18c -> +0x2a8: m - 1
};

class AbilitySystem final : public combat::CombatHooks {
public:
    AbilitySystem(combat::CombatSystem& combat, const AbilityData& data, AbilityConfig config);
    ~AbilitySystem() override;
    AbilitySystem(const AbilitySystem&) = delete;
    AbilitySystem& operator=(const AbilitySystem&) = delete;

    const AbilityConfig& config() const { return cfg_; }
    combat::CombatSystem& combat() { return *combat_; }
    void reserve(uint32_t objects, uint32_t events = 512);

    // ---- objects (host) ----
    // GameObject::Init_From_Type (0x3989a0) order: unit-ability slots (XML order, later entries of a
    // type replace earlier ones) -> combat unit + hardpoints (one synced Get_Int each, list order)
    // -> special abilities (automatic styles applied) -> Active_By_Default [port]. `container` = the
    // team container the unit belongs to (added later with add_container, or before).
    bool add_unit(const combat::UnitSpawn& s, uint32_t ability_desc, uint32_t frame, SimRng& rng,
                  ObjectId container = kNoObject);
    // A team container (squadron, TEAM behaviour): not a combat unit. Ids strictly increasing across
    // add_unit / add_container.
    // OI abilities: `rng` makes the SAP-init draws of the container's own special abilities (§4.3); nullptr: none.
    bool add_container(ObjectId id, uint32_t ability_desc, int32_t owner, std::span<const ObjectId> members,
                       uint32_t frame, SimRng* rng = nullptr);
    void remove(ObjectId id);  // host removal (combat unit removed too)
    // S10-int: Change_Owner (Lua / story): the object's owner here and, for a combat unit, in combat.
    bool set_owner(ObjectId id, int32_t owner);

    // ---- commands ----
    // 43 SetUnitAbilityMode (0x430270): per object Can_Activate(t, on, on), skip when Is_Active == on,
    // turning on with a countdown needs counter < 1, then Set_Unit_Ability_Active. Returns how many
    // objects changed.
    uint32_t set_unit_ability_mode(std::span<const ObjectId> objects, uint8_t type, bool on, uint32_t frame,
                                   ObjectId target = kNoObject);
    // 40 TacticalSpecialAbility (0x429960) / 57 (host spawns the Dummy_Barrage_Target and passes it):
    // attack order + pending ability for every accepted user. Returns the number accepted.
    uint32_t targeted_ability(std::span<const ObjectId> users, uint8_t type, ObjectId target, int32_t hardpoint,
                              uint32_t frame);
    // 58 SetAbilityAutofire (0x4b3860): slot byte +9 on listed non-member objects that have it.
    void set_autofire(std::span<const ObjectId> objects, uint8_t type, bool on);
    // Set_Unit_Ability_Active (0x38a350), public for Lua / host.
    bool set_unit_ability_active(ObjectId id, uint8_t type, bool on, uint32_t frame, ObjectId target = kNoObject);

    // ---- queries ----
    bool has_object(ObjectId id) const { return find(id) >= 0; }
    // Team membership (TEAM slot +0x348): the team container a member was added with (add_unit `container`, or
    // add_container's member list), kNoObject when none / unknown; and a container's members (add order; dead members
    // stay listed until the host removes them). Empty for a non-container.
    ObjectId container_of(ObjectId member) const;
    std::span<const ObjectId> members_of(ObjectId container) const;
    bool has_ability(ObjectId id, uint8_t type) const;
    bool is_active(ObjectId id, uint8_t type) const;  // Is_Unit_Ability_Active (0x39b950)
    bool can_activate(ObjectId id, uint8_t type, bool check_recharge, bool check_nebula, uint32_t frame) const;
    bool is_ready(ObjectId id, uint8_t type, uint32_t frame) const;  // Lua Is_Ability_Ready
    bool is_autofire(ObjectId id, uint8_t type) const;
    const UnitAbilityRecord* record(ObjectId id, uint8_t type) const;  // Get_Unit_Ability_Record (0x397d90)
    int32_t countdown_counter(ObjectId id, uint8_t type) const;
    int32_t countdown_maximum(ObjectId id, uint8_t type) const;
    uint8_t countdown_phase(ObjectId id, uint8_t type) const;
    int32_t pending_type(ObjectId id) const;  // -1 none
    ObjectId pending_target(ObjectId id) const;
    bool is_stealthed(ObjectId id, uint32_t frame) const;  // Is_Stealthed (0x39b140)
    bool tractored(ObjectId id) const;                     // CD +0x132
    bool interdicting(ObjectId id) const;
    bool interdicted(int32_t player) const;  // some enemy interdict entry targets this player [L]
    int32_t immobilize_count(ObjectId id) const;
    bool special_applied(ObjectId id, uint32_t ability_name_crc) const;
    ObjectId special_target(ObjectId id, uint32_t ability_name_crc) const;
    // R5 jamming visual (presentation only): the applied Sensor_Jamming_Ability of `id` (null when none / dead) and the radius
    // its update jams with (unit ability 63 Effective_Radius, else the type's Passive_Missile_Shield_Radius).
    const SpecialAbilityDesc* jamming_special(ObjectId id, float& radius) const;
    float get_hull(ObjectId id) const { return combat_->script_hull(id); }      // Lua Get_Hull
    float get_shield(ObjectId id) const { return combat_->shield_fraction(id); }  // Lua Get_Shield

    // ---- Lua helpers (5) ----
    bool force_recharge(ObjectId id, uint8_t type, float seconds = -1.0f);  // Force_Ability_Recharge
    bool reset_counter(ObjectId id, uint8_t type);                          // Reset_Ability_Counter [M]

    // ---- host-fed state ----
    void set_nebula(ObjectId id, uint32_t frame);         // CD +0x104: last frame inside a nebula
    void set_force_disabled(ObjectId id, uint32_t frame); // CD +0x110
    void set_ion_storm(ObjectId id, uint32_t frame);      // CD +0x100
    // ---- L1 systems: space fields (L1_S4_REMAINING.md §3.3) ----
    void clear_nebula(ObjectId id);     // stamp CD +0x104 := -1 (left the nebula)
    void clear_ion_storm(ObjectId id);  // stamp CD +0x100 := -1
    bool nebula_stamped(ObjectId id) const;  // stamp set (in a nebula, or its lockout running)
    bool nebula_lockout(ObjectId id, uint32_t frame) const;     // 0x380bb0
    bool ion_storm_lockout(ObjectId id, uint32_t frame) const;  // 0x380b70
    // Nebula entry (0x4376f0): every active unit-ability slot of the object is deactivated (Deactivated events);
    // returns how many.
    uint32_t cancel_active_abilities(ObjectId id, uint32_t frame);
    // CombatHooks: shields neither absorb nor count as up during the ion-storm lockout (0x549ac0).
    bool ion_storm_shields_down(ObjectId victim, uint32_t frame) const override;
    void set_locomotor_busy(ObjectId id, bool busy);      // also the combat flag (no ion stun)
    void set_abilities_disabled(ObjectId id, bool off);   // SAP +0x28
    void set_corrupted(ObjectId id, bool on);             // CORRUPT_SYSTEMS behaviour on the victim
    void set_weapons_disabled(ObjectId id, bool off);     // obj +0x3a0 bit 0x10
    void set_garrison_count(ObjectId id, int32_t n);
    void set_special_enabled(int32_t player, uint32_t ability_name_crc, bool enabled);  // player override
    void set_player_stealth_bonus(int32_t player, float s);  // player list +0x268 (Stealth_Duration_Multiplier)
    // P4 tactical: campaign Black Market gate (ABILITIES_HARDPOINTS 2.4, 0x39bbd0 / 0x39b0b0; P4_SIM_SYSTEMS 1.5).
    // `name_crcs` = the ability names (name_crc: UnitAbilityRecord::gui_name_crc / SpecialAbilityDesc::name_crc) that
    // are blocked for `player`: the host lists a name when its record has Must_Be_Bought_On_Black_Market, some
    // BlackMarketItem lists it and the player bought none of those items (GalacticSim::bm_allows_*). Set once at
    // battle start, before the units are added (purchases cannot happen during a battle); replaces the player's
    // list. Ignored when config().skirmish (skirmish always passes). Not hashed: a battle-start input derived from
    // the galactic state, its effects are in the hashed state. Effect:
    //   * unit abilities: a slot whose record (team containers: the first live member's, as record()) is marked and
    //     blocked fails can_activate / is_ready / black_market_allows, and command 43 (turning on) / 40 / 57 skip
    //     the object. The internal Can_Activate (0x39b480: autofire, container member checks) is unchanged [H];
    //   * special abilities: a marked, blocked special never applies (part of the generic trigger, like a
    //     player-disabled one).
    void set_black_market_blocked(int32_t player, std::vector<uint32_t> name_crcs);
    bool black_market_allows(ObjectId id, uint8_t type) const;  // Black_Market_Allows_Ability (0x39bbd0)
    // Fog follow-up: Force Sight (ForceSightAbilityClass, FOG_OF_WAR.md §3.3) calls fn(owner, true) when it starts
    // channelling (Reveal_All for the owner) and fn(owner, false) when it ends / the user dies or is removed (Undo
    // Reveal_All), from the serial tick phases or the command / Lua call that activated it. Unset: no fog effect.
    void set_fog_reveal_all(std::function<void(int32_t player, bool on)> fn) { fog_reveal_all_ = std::move(fn); }

    // ---- OI abilities (TACTICAL_GAPS.md §1, §2, §7) ----
    void set_host(AbilityHost h) { host_ = std::move(h); }
    // Objects to create after this tick (cleared by clear_events()); bind_spawn tells which object a request became.
    std::span<const SpawnRequest> spawn_requests() const { return spawns_; }
    bool bind_spawn(uint32_t serial, ObjectId id);
    // Active_By_Default (0x3a82a0, §1.2 [H]): the first two Unit_Abilities_Data entries with Active_By_Default are
    // switched on. add_unit / add_container call it at creation; the A1/A2 AI layer calls it when a unit is released
    // to the free store (after un-immobilising it) or leaves a task force without hunting.
    void apply_active_by_default(ObjectId id, uint32_t frame);
    // Order cancel 0x38f5d0 (§1.1 [H]): every new move / attack / escort order (commands, Lua, AI) switches off
    // BARRAGE, LUCKY_SHOT, ION_CANNON_SHOT (on the team container) and HUNT when active. Not called for the moves
    // abilities issue themselves (MoveOrder events).
    void order_cancel(ObjectId id, uint32_t frame);
    // Autofire setter 0x3a78b0 (command 58 / Lua Set_Single_Ability_Autofire, §1.3): writes the slot byte AND the
    // per-object map entry (keyed by the ability name CRC); refused (false) for records without Supports_Autofire
    // unless the owner is an AI player.
    bool set_ability_autofire(ObjectId id, uint8_t type, bool on);
    // 0x48d5e0: the map entry when set, else true for AI-controlled (non-human) owners.
    bool autofire_on(ObjectId id, uint8_t type) const;
    // Retreat permission (0x282750 / 0x2825e0, §1.5): true while some interdict / Retreat_Prevention entry targets the
    // player (the original returns the entry's reason code +0x38 [M]).
    bool retreat_blocked(int32_t player) const;
    // Command 45 RepairHardpointEvent (§1.12): adds `player` as a repairer of the hardpoint (CombatSystem).
    bool repair_hardpoint(ObjectId id, uint32_t hp, int32_t player);
    // Lua Set_Targeting_Priorities(name) on a unit or container (members follow their head): the named
    // Targeting_Priority_Set overrides the type's (empty name clears). False for an unknown set / object.
    bool set_targeting_priorities(ObjectId id, std::string_view set_name);
    // Battlefield_Modifier player lists (max-then-sum over stacking categories, ascending; 0 when empty).
    float player_list(int32_t player, PlayerList l) const;
    // Weatherproof (§7.4): weather-affected = no entry for the player (every Weatherproof entry is false).
    bool weather_affected(int32_t player) const;
    bool hunting(ObjectId id) const;     // HUNT behaviour enabled
    bool deployed(ObjectId id) const;    // DEPLOY state (DEPLOY 2 Is_Active)
    bool spread_out(ObjectId id) const;  // SPREAD_OUT member flag obj +0x448 (default idle: Idle Crouch)
    size_t mine_count() const { return mines_.size(); }
    // REMAINING_GAPS §5.2: DEPLOY_TROOPERS behaviour data +0x40 (troopers deployed so far) and the trooper list +0x10
    // (ids the host bound with bind_spawn, spawn order).
    int32_t troopers_deployed(ObjectId id) const;
    std::span<const ObjectId> troopers(ObjectId id) const;
    // REMAINING_GAPS §5.5: the Blast special-weapon state counter (SAP pack +0x394) of `id`: > 0 charging, < 0 charged
    // shots left (negated), 0 none.
    int32_t blast_counter(ObjectId id) const;
    // TACTICAL_GAPS 2.3.2: the Berserker speed override (apply: locomotor +0x2a0 := Berserker_Speed, flag +0x29c := 1,
    // 0x3a8c90) while a Berserker special of `id` is applied; -1 = none. [inferred] the flag clears when the special is
    // removed (the remove path is not traced). The host forwards it to MovementSystem::set_speed_override each frame.
    float speed_override(ObjectId id) const;

    // ---- outputs ----
    std::span<const AbilityEvent> events() const { return events_; }
    void clear_events() {  // host: once per frame after consuming them (OI abilities: and the spawn requests)
        events_.clear();
        spawns_.clear();
    }
    void hash(StateHasher& h) const;
    // Adds "<prefix>.objects" and the combat extension "<combat_prefix>.ext".
    void register_crc(CrcRegistry& registry, const std::string& prefix = "abilities",
                      const std::string& combat_prefix = "combat") const;
    // N1 stage 2: save / load during a battle (contract: alamo/core/save_file.hpp; layout: ability_save.cpp). Between
    // ticks only. Writes this system's state only (objects with their slots, countdowns, special-ability states and
    // OI behaviour state, spawn requests, mines, player / source lists, remote bombs, events, overrides, stealth bonus,
    // black-market lists, counters); the wrapped CombatSystem saves itself (CombatSystem::write). read() checks the
    // descriptor counts (AbilityData objects / specials, players), validates every index, requires r.done() and
    // leaves the system unchanged on failure. Not saved (re-attached by the host): AbilityHost, bound RNG, fog
    // reveal-all callback, config. No host call is needed after read (the combat hooks stay installed).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

    void tick(uint32_t frame, const TreeSet& trees, SimRng& rng, const ParallelFor& pf = serial_for) {
        combat_->tick(frame, trees, rng, pf);
    }

    // ---- CombatHooks ----
    void begin_tick(uint32_t frame) override;
    void before_units(uint32_t frame, SimRng& rng) override;
    void unit_begin(ObjectId id, uint32_t frame, SimRng& rng) override;
    void unit_after_targeting(ObjectId id, uint32_t frame, SimRng& rng) override;
    bool shot_allowed(ObjectId shooter, ObjectId target, int32_t target_hp, int32_t& projectile) override;
    void shot_fired(ObjectId shooter, ObjectId target, int32_t target_hp, int32_t projectile, uint32_t frame) override;
    // REMAINING_GAPS §5.5: Blast_Ability state in Fire_At (0x3825b0).
    bool fire_gate(ObjectId shooter, const combat::HardpointDesc* hardpoint, int32_t& projectile, float& damage_scale) override;
    void fire_done(ObjectId shooter, const combat::HardpointDesc* hardpoint, uint32_t frame) override;
    float lucky_shot(ObjectId source, ObjectId victim, int32_t& hardpoint, Vec3 impact, bool has_impact) override;
    bool damage_listener(ObjectId victim, ObjectId source, int32_t projectile_desc, uint32_t frame, SimRng* rng) override;
    void hardpoint_destroyed(ObjectId owner, uint32_t index, combat::HardpointKind kind, bool last_of_kind,
                             uint32_t frame) override;  // = hardpoint_offline(..., destroyed = true, ...)
    // Set_System_Online (0x384d00) ability side, HARDPOINT_DISABLED.md §9: offline (destroyed or disabled) / online.
    void hardpoint_offline(ObjectId owner, uint32_t index, combat::HardpointKind kind, bool last_of_kind, bool destroyed,
                           uint32_t frame) override;
    void hardpoint_online(ObjectId owner, uint32_t index, combat::HardpointKind kind, uint32_t frame) override;
    // ENABLE_SPECIAL_ABILITY (§9.6, Set_Special_Ability_Enabled 0x288890): the special ability `crc` of every object of
    // type `desc` owned by `player` (and later spawns of it) is enabled / disabled. [port] kept as a (player, type,
    // ability) override that takes precedence over set_special_enabled.
    void set_type_special_enabled(int32_t player, uint32_t desc, uint32_t ability_name_crc, bool enabled);
    // Special ability `ability_name_crc` usable state for the object (black market, type override, player override,
    // Initially_Enabled). False for an unknown object / ability.
    bool special_enabled(ObjectId id, uint32_t ability_name_crc) const;
    void unit_dying(ObjectId id, uint32_t frame) override;
    void ion_stunned(ObjectId id, uint32_t frame) override;
    void units_removed() override;
    void unit_effect(ObjectId id, combat::UnitEffect effect, int32_t player, Vec3 point, uint32_t frame) override;  // OI abilities

private:
    static constexpr uint32_t kNever = 0xFFFFFFFFu;
    struct SpecialState {
        uint32_t desc = 0;
        uint32_t due = 0;
        uint8_t applied = 0;
        uint8_t fired = 0;
        uint8_t trigger = 0;  // unit-ability type that forwarded to it
        ObjectId target = kNoObject;
        int32_t hp = -1;
        int32_t frames = 0;
        int32_t cooldown = 0;
        int32_t player = -1;  // Force Sight: the player Reveal_All was applied for (not hashed: fog is not in the CRC)
        // ---- OI abilities: class state (TACTICAL_GAPS §2.3) ----
        uint32_t next = 0;    // class timer frame (fire / damage / creation / spawn frame)
        int32_t counter = 0;  // class counter (Berserker / Drain / Leech duration, Blast charge)
        uint8_t phase = 0;    // class state machine
        uint8_t direct = 0;   // ordered directly (pending context / no target): the auto-use pre-check is skipped
        uint8_t aura = 0;     // event-driven class applied (Combat_Bonus / Battlefield_Modifier / Base_Power ...)
        float radius = 0.0f;  // wave radius / sweep angle
        float value = 0.0f;   // stored damage D
        Vec3 point;           // target point
        std::vector<ObjectId> victims;  // per-cast id list (waves, lightning, drain)
    };
    struct Obj {
        ObjectId id = kNoObject;
        uint32_t desc = 0;
        int32_t owner = -1;
        ObjectId container = kNoObject;
        uint8_t is_container = 0, combat_unit = 0, dead = 0;
        std::vector<ObjectId> members;
        std::array<int16_t, kAbilitySlots> record{};  // index into unit_abilities, -1 = empty slot
        std::array<uint8_t, kAbilitySlots> active{}, autofire{}, behavior{};
        Countdown cd;
        // pending targeted ability (CD +0x148 / +0x150 / +0x154)
        ObjectId pending_target = kNoObject;
        int32_t pending_hp = -1;
        uint32_t pending_crc = 0;
        int32_t pending_type = -1;
        // host-fed
        uint32_t nebula_frame = kNever, force_frame = kNever, ion_storm_frame = kNever;
        uint8_t locomotor_busy = 0, sap_disabled = 0, corrupted = 0, weapons_disabled = 0;
        int32_t garrison_count = 0;
        // state
        uint8_t gravity_well_destroyed = 0, tractored = 0, interdicting = 0;
        int32_t immobilize = 0;
        uint8_t stealthed = 0;
        int32_t stealth_frames = 0;
        uint32_t last_detected = 0;
        // ion cannon shot [0] / lucky shot [1] per shooter (members get copies)
        std::array<ObjectId, 2> shot_target{kNoObject, kNoObject};
        std::array<int32_t, 2> shot_hp{-1, -1};
        std::array<int32_t, 2> shot_proj{-1, -1};
        uint8_t lucky_fired = 0;
        std::vector<SpecialState> specials;
        // ---- OI abilities ----
        uint32_t heal_due = 0;  // Force_Healing due frame, shared by every healing ability of the object (+0x25c)
        std::vector<std::pair<uint32_t, uint8_t>> autofire_map;  // SAP pack +0x100 map: ability name CRC -> on
        uint32_t hunt_due = 0;  // HUNT behaviour next service
        uint8_t deployed = 0, spread = 0, self_destruct = 0, confused = 0, barrage = 0, stunned = 0;
        uint32_t confuse_end = 0;
        ObjectId barrage_dummy = kNoObject;
        uint8_t pending_direct = 0;  // the pending ability came from a command (context flag +0x2e -> +0x11)
        // ---- REMAINING_GAPS §5 ----
        int32_t troopers_deployed = 0;  // §5.2 DEPLOY_TROOPERS behaviour data +0x40
        std::vector<ObjectId> troopers; // §5.2 trooper list +0x10 (bound ids)
    };
    struct RemoteBomb {  // Remote_Bomb_Ability bombs waiting for DETONATE_REMOTE_BOMB (0x280ec0)
        int32_t player;
        uint32_t serial;  // combat projectile serial
    };
    struct Mine {  // ProximityMineBehavior (0x6373b0) on a spawned Mine_Type object
        uint32_t serial = 0;    // spawn request
        ObjectId id = kNoObject;
        int32_t owner = -1;
        Vec3 pos;
        float trigger = 0.0f;
        float detect = 0.0f;    // its own detection range (0x3979d0) [port: Trigger_Radius x 2]
        uint32_t arm = 0;
        uint8_t detected = 0, dead = 0;
    };
    struct PlayerEntry {  // Battlefield_Modifier player-list entry (append insert 0x38c850, permanent)
        int32_t player;
        uint8_t list;
        int32_t category;
        float value;
        ObjectId source;
        uint32_t crc;
    };
    struct SourceEntry {  // interdict / Retreat_Prevention list (+0x180), Weatherproof list (+0x410)
        int32_t player;
        ObjectId source;
        uint32_t crc;
        uint8_t kind;  // 0 interdict, 1 retreat prevention, 2 weatherproof
    };
    struct PlayerOverride {
        int32_t player;
        uint32_t crc;
        uint8_t enabled;
    };

    int32_t find(ObjectId id) const;
    Obj* obj(ObjectId id);
    const Obj* obj(ObjectId id) const;
    const AbilityObjectDesc& desc(const Obj& o) const { return data_->objects[o.desc]; }
    const AbilityTypeInfo& info(uint32_t t) const { return ability_type_info(t); }
    const UnitAbilityRecord* own_record(const Obj& o, uint32_t t) const;
    const UnitAbilityRecord* record_of(const Obj& o, uint32_t t) const;
    bool has(const Obj& o, uint32_t t) const;
    bool active_state(const Obj& o, uint32_t t) const;
    bool can(const Obj& o, uint32_t t, bool check_recharge, bool check_nebula, uint32_t frame) const;
    bool in_nebula_lockout(const Obj& o, uint32_t frame) const;
    bool set_active(Obj& o, uint32_t t, bool on, uint32_t frame, ObjectId target);
    void handler(Obj& o, uint32_t t, bool on, uint32_t frame, ObjectId target);
    void sync_combat(Obj& o);
    void update_emitter(Obj& o);
    void immobilize(Obj& o, int32_t delta, uint32_t frame);
    void event(AbilityEventKind k, uint8_t type, ObjectId id, ObjectId target, uint32_t frame);
    float own_recharge_bonus(const Obj& o) const;  // list 9 of the object's own set only
    void start_active(Obj& o, uint32_t t);
    void start_recharge(Obj& o, uint32_t t);
    void deactivate_early(Obj& o, uint32_t t, uint32_t frame);
    void update_countdown(Obj& o, uint32_t frame);
    void expire(Obj& o, uint32_t t, uint32_t frame);
    Vec3 position(const Obj& o) const;
    bool alive_unit(ObjectId id) const;

    // special abilities
    SpecialState* special_by_crc(Obj& o, uint32_t crc);
    bool allowed(const SpecialAbilityDesc& s) const;
    bool enabled(int32_t player, const SpecialAbilityDesc& s) const;
    bool enabled(const Obj& o, const SpecialAbilityDesc& s) const;  // + the (player, type) override (§9.6)
    bool bm_blocked(int32_t player, bool must_be_bought, uint32_t name_crc) const;  // P4 tactical
    bool bm_ok(const Obj& o, uint32_t t) const;                                     // P4 tactical (unit slot)
    bool activate_by_name(Obj& o, uint32_t crc, ObjectId target, int32_t hp, uint32_t frame, uint8_t trigger);
    bool accepts(const Obj& o, const SpecialAbilityDesc& s, ObjectId target, uint32_t frame) const;
    bool target_filter(const SpecialAbilityDesc& s, ObjectId target) const;
    bool in_range(const Obj& o, const SpecialAbilityDesc& s, ObjectId target) const;
    bool apply(Obj& o, SpecialState& sp, ObjectId target, int32_t hp, uint32_t frame);
    void update_special(Obj& o, SpecialState& sp, uint32_t frame);
    void remove_special(Obj& o, SpecialState& sp, uint32_t frame);
    void sap_update(Obj& o, uint32_t frame);
    void try_pending(Obj& o, uint32_t frame);
    void apply_automatic(Obj& o, uint32_t frame);
    void interdict(Obj& o, bool on, uint32_t frame);
    void break_stealth(Obj& o, uint32_t frame);
    void refresh_stealth_flag(const Obj& o, uint32_t frame);
    bool stealthed_now(const Obj& o, uint32_t frame) const;
    void detect(Obj& o, uint32_t frame);
    void shot_units(Obj& o, std::array<Obj*, 64>& out, uint32_t& n);  // the owner itself or its members

    // ---- OI abilities (ability_behaviors.cpp / ability_classes.cpp) ----
    int32_t interval_of(const SpecialAbilityDesc& s) const {  // §1.4 per class unless overridden
        return s.service_interval >= 0 ? s.service_interval : special_service_interval(s.cls, cfg_.fps);
    }
    void sap_init(Obj& o, uint32_t frame, SimRng* rng);         // §4.3 first-due stagger draws
    void active_by_default(Obj& o, uint32_t frame);             // §1.2
    void autofire_driver(Obj& o, ObjectId target, uint32_t frame);  // §1.3
    float draw_unit();                                          // inline synced [0, 1] draw (0x5324d0)
    float draw_float(float lo, float hi);
    int32_t draw_int(int32_t lo, int32_t hi);
    bool allied(int32_t a, int32_t b) const { return a == b || (a >= 0 && b >= 0 && !combat_->is_enemy(a, b)); }
    bool enemy(int32_t a, int32_t b) const { return combat_->is_enemy(a, b); }
    bool moving(const Obj& o) const;
    void move_order(ObjectId id, Vec3 p, uint32_t frame, uint8_t type = 0);  // type: the ability issuing it (event)
    void stop_order(ObjectId id, uint32_t frame);
    void change_owner(Obj& o, int32_t owner, uint32_t frame);
    uint32_t request_spawn(SpawnRequest r);
    float max_health_and_shield(ObjectId id) const;
    bool map_rect(Vec3& lo, Vec3& hi) const;
    // event-driven classes (auras)
    void auras_unit_added(Obj& n, uint32_t frame);
    void aura_on(Obj& src, SpecialState& sp, uint32_t frame);
    void aura_off(Obj& src, SpecialState& sp, uint32_t frame);
    void aura_give(const Obj& src, const SpecialAbilityDesc& s, ObjectId target);
    bool aura_target(const Obj& src, const SpecialAbilityDesc& s, const Obj& t) const;
    float player_reduce(int32_t player, uint8_t list) const;
    // unit-ability handler bodies (§2.1) and behaviours (§2.2)
    bool team_gates(Obj& o, uint32_t t, uint32_t frame);
    void h_deploy(Obj& o, bool on, uint32_t frame);
    void h_jet_pack(Obj& o, ObjectId target, uint32_t frame);
    void h_barrage(Obj& o, bool on, ObjectId target, uint32_t frame);
    void h_spread_out(Obj& o, bool on, uint32_t frame);
    void h_replenish(Obj& o, uint32_t frame);
    void h_instant(Obj& o, uint32_t t, ObjectId target, uint32_t frame);
    void h_eject_thief(Obj& o, uint32_t frame);                                // REMAINING_GAPS §5.1
    void deploy_troopers(Obj& o, const UnitAbilityRecord& r, uint32_t frame);  // §5.2 (state 4 -> 0x590730)
    SpecialState* blast_state(Obj& o);                                         // §5.5: the applied Blast, if any
    void h_behavior(Obj& o, uint32_t t, bool on, uint32_t frame);
    void behaviors_service(Obj& o, uint32_t frame);  // HUNT, SELF_DESTRUCT, CONFUSE expiry
    void hunt_decide(Obj& o, uint32_t frame);
    void confuse(Obj& v, float secs, uint32_t frame);
    void mines_update(uint32_t frame);
    // special-ability class bodies (ability_classes.cpp)
    bool class_accepts(const Obj& o, const SpecialAbilityDesc& s, ObjectId target, uint32_t frame) const;
    bool class_apply(Obj& o, SpecialState& sp, ObjectId target, int32_t hp, uint32_t frame);
    void class_update(Obj& o, SpecialState& sp, uint32_t frame);
    void class_remove(Obj& o, SpecialState& sp, uint32_t frame);
    bool range_ok(const Obj& o, const SpecialAbilityDesc& s, ObjectId target) const;
    void heal_pass(Obj& o, const SpecialAbilityDesc& s, uint32_t frame);
    uint32_t wave_victims(Obj& o, SpecialState& sp, float radius, std::vector<ObjectId>& out);
    // OI t03: an animation's timing on sim frames (§4.13). rate = clip frames per sim frame at speed 1 (clip fps / 30,
    // the sim animation's clock); count = clip frames, end = max(count - 2, 0) (where a one-shot finishes); count = 0:
    // no clip (rate 1, the fixed fallbacks).
    struct ClipTiming {
        float rate = 1.0f;
        float count = 0.0f;
        float end = 0.0f;
    };
    ClipTiming clip_timing(ObjectId id, int32_t anim_type, std::string_view anim_name) const;
    void lightning_targets(Obj& o, const SpecialAbilityDesc& s, ObjectId primary, std::vector<ObjectId>& out);

    AbilityHost host_;
    SimRng* rng_ = nullptr;  // synced RNG for draws outside a tick (bind_rng) and inside (the hooks' rng)
    std::vector<SpawnRequest> spawns_;
    uint32_t spawn_serial_ = 1;
    std::vector<Mine> mines_;
    std::vector<PlayerEntry> player_entries_;
    std::vector<SourceEntry> source_entries_;
    std::vector<RemoteBomb> remote_bombs_;
    std::vector<ObjectId> scratch_;  // per-call query scratch (reserved)
    std::vector<int32_t> member_scratch_;  // §5.2 company member types (host query output)
    uint8_t direct_ctx_ = 1;         // activate_by_name: the apply comes from a direct order (not autofire)

public:
    // OI abilities: the synced RNG ability applies use outside the combat tick (commands 43 / 40 / Lua; chance rolls,
    // Confuse destinations, mine scatter). The tick binds its own for the tick's duration.
    void bind_rng(SimRng* rng) { rng_ = rng; }

private:
    combat::CombatSystem* combat_;
    const AbilityData* data_;
    AbilityConfig cfg_;
    std::vector<Obj> objs_;  // sorted by id
    std::vector<AbilityEvent> events_;
    std::vector<PlayerOverride> overrides_;
    struct TypeOverride {
        int32_t player;
        uint32_t desc;
        uint32_t crc;
        uint8_t enabled;
    };
    std::vector<TypeOverride> type_overrides_;  // insertion order; hashed only when non-empty
    std::vector<float> stealth_bonus_;  // per player
    std::vector<std::vector<uint32_t>> bm_blocked_;  // P4 tactical: per player, sorted unique name CRCs
    std::function<void(int32_t, bool)> fog_reveal_all_;  // fog follow-up (Force Sight)
};

}  // namespace alamo::tactical::abilities

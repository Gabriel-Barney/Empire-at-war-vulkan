// L1 systems - tactical special weapons, lobbing superweapons and tactical superweapons (the systems behind the Lua
// hooks lua::TacticalScriptHooks weapon_readiness / begin_targeting / fire_at_object / fire_lobbing_at / tsw_state /
// tsw_fire). Spec: docs/re/SCRIPT_GAPS.md §1 (conventions, §1.1, §1.2, §1.6) and docs/re/L1_S4_REMAINING.md.
//
// OBJECTS
//   * SPECIAL_WEAPON (behaviour 0x2f, e.g. Ground_Ion_Cannon / Hypervelocity gun in space): a data pack (obj +0x158)
//     with a start frame (+0x8), a ready frame (+0xc) and a targeting flag (+0x28). Readiness (0x4914e0) = 1 when
//     now >= ready, else clamp((now - start) / (ready - start), 0, 1); "ready" = readiness >= 1.
//   * LOBBING_SUPERWEAPON (0x37): the same pack (combatant pack fire request +0x3c, target point +0x30).
//   * TACTICAL_SUPER_WEAPON (0x2c, the Death Star): the TSW data pack (obj +0x148), state +0x20 (4 = ready).
//   The host adds every such object with a SpecialWeaponDesc built from its type (one object may carry several kinds;
//   SCRIPT_GAPS §1.1 picks the path by kind) and removes it on death.
//
// FIRING (§1.6)
//   * begin_targeting (0x491b40): a second call toggles the flag off and fails; in space it fails while another
//     SPECIAL_WEAPON of the same owner is targeting.
//   * fire_at_object (0x491cd0): pack ready, target not flagged, NOT allied to the weapon owner, target categories
//     intersect Special_Weapon_Valid_Targets, target != self. Then the pack restarts (start = now, ready = now +
//     int(fps x recharge)), the flag clears and Projectile_Fire_Pulse_Count shots are queued, Projectile_Fire_Pulse_
//     Delay_Seconds apart [M: the weapon event 0x549d70 fires the type's weapon at the target].
//   * fire_lobbing_at (0x4ea3f0): the owner's base power when Requires_Base_Power; no range / ally / fog check; the
//     point and the fire request are stored, the pack restarts, the shots fly at the point.
//   Shots are emitted by tick() into shots(): the host turns each into CombatSystem::launch_projectile (from the
//   weapon's position). The upgrade gate (SpecialWeaponsAllowed) and the destroy-after sweep are the caller's
//   (the Lua binding / the host), as in the original.
//
// TACTICAL SUPERWEAPON (§1.2 state machine, vf6 0x42a7b0) [M names]
//   0 idle -> find the DUMMY_TACTICAL_SUPER_WEAPON_TARGET (host) -> 2; 1 search (= 0); 2 target acquired -> 3;
//   3 arming: approach the target [port: at Max_Speed per frame, straight line, until within
//   TSW_Attack_Distance_From_Target; the 0x42b560 progress rule is not in the docs] -> 4 + TswReady;
//   4 ready (Fire_Tactical_Superweapon); fire (0x42a200: planet checks by the host, state 4 and a target) -> 5,
//   countdown int(30 x TSW_Power_Up_Countdown_Seconds) -> 6 -> 7 attack animation (anim 5) + SFXEvent_Fire,
//   countdown int(30 x TSW_Attack_Anim_Duration_Seconds) -> 9 planet destruction (debris type: synced Get_Int when
//   the planet lists several) -> 10 aftermath (TSW_Post_Destruction_Wait_Frames) -> 11 done.
//   First service with a target: placed toward it at TSW_Start_Distance_From_Target, or at
//   TSW_Attack_Distance_From_Target when the owner is the battle attacker (GMM 0x28aec0 [M]) - then it starts in
//   state 4 with the target set; otherwise state 2. TSW_Start_Pos_Match_Targets_Z: z = target z +
//   TSW_Z_Adjust_Relative_To_Target_Pos [M].
//
// DETERMINISM: packs sorted by object id, serviced in ascending id from the host's serial tick step; the only draw
// is the TSW debris pick (synced). No transcendentals except dmath::sqrt.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/combat/combat_system.hpp"

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::tactical::abilities {

using combat::kNoObject;
using combat::ObjectId;

enum SpecialWeaponKind : uint8_t {
    kSwSpecialWeapon = 1u << 0,  // SPECIAL_WEAPON (0x2f)
    kSwLobbing = 1u << 1,        // LOBBING_SUPERWEAPON (0x37)
    kSwTsw = 1u << 2,            // TACTICAL_SUPER_WEAPON (0x2c)
};

// TSW states (data pack +0x20).
enum TswState : int32_t {
    kTswIdle = 0,
    kTswSearch = 1,
    kTswAcquired = 2,
    kTswArming = 3,
    kTswReady = 4,
    kTswPowerUp = 5,
    kTswFire = 6,
    kTswAttackAnim = 7,
    kTswAttackWait = 8,
    kTswDestruction = 9,
    kTswAftermath = 10,
    kTswDone = 11,
};

struct SpecialWeaponDesc {
    uint8_t kinds = 0;                // SpecialWeaponKind bits
    float recharge_seconds = 0.0f;    // Projectile_Fire_Recharge_Seconds [M: the 0x373630 getter]
    int32_t pulse_count = 1;          // Projectile_Fire_Pulse_Count
    float pulse_delay_seconds = 0.0f; // Projectile_Fire_Pulse_Delay_Seconds
    int32_t projectile = -1;          // CombatData projectile of the type's weapon (-1: no shot, events only)
    uint64_t valid_targets = 0;       // Special_Weapon_Valid_Targets (category mask)
    bool requires_base_power = false; // Requires_Base_Power (lobbing)
    int32_t target_action_index = -1; // Special_Weapon_Target_Action_Index (faction speech, presentation)
    // TSW
    float max_speed = 0.0f;                    // Max_Speed (approach, per frame) [port]
    float tsw_start_distance = 0.0f;           // TSW_Start_Distance_From_Target
    float tsw_attack_distance = 0.0f;          // TSW_Attack_Distance_From_Target
    bool tsw_match_target_z = false;           // TSW_Start_Pos_Match_Targets_Z
    float tsw_z_adjust = 0.0f;                 // TSW_Z_Adjust_Relative_To_Target_Pos
    float tsw_power_up_seconds = 0.0f;         // TSW_Power_Up_Countdown_Seconds
    float tsw_attack_anim_seconds = 0.0f;      // TSW_Attack_Anim_Duration_Seconds
    int32_t tsw_debris_delay_frames = 0;       // TSW_Explosion_Debris_Creation_Frame_Delay
    int32_t tsw_post_destruction_frames = 0;   // TSW_Post_Destruction_Wait_Frames
};

struct SpecialWeaponConfig {
    uint32_t fps = combat::kDefaultFps;
    bool space = true;           // space mode (begin_targeting's one-per-owner rule)
    // [port] a new pack: false = it starts charging at add (start = frame, ready = frame + recharge); true = ready
    // at once. The pack's constructor values are not in the docs.
    bool start_charged = false;
};

enum class SwEventKind : uint8_t {
    Ready,            // readiness reached 1 (SFXEvent_Special_Weapon_Ready / HUD "available") [port: once per charge]
    TargetingOn,      // begin_targeting set the flag
    TargetingOff,     // ... toggled it off
    Fired,            // fire_at_object / fire_lobbing_at accepted (SFXEvent_Fire, target-action speech)
    TswState,         // value = the new state
    TswPlaced,        // first service: placed toward the target (point = position)
    TswReady,         // 3 -> 4: "superweapon ready" speech (own / enemy text by relation)
    TswFiring,        // fire accepted: firing sequence (0x2ccf30), Activate voice, Power_Up SFX, victory monitor
    TswAttackAnim,    // state 7: attack animation (anim 5) + SFXEvent_Fire
    TswPlanetDestroyed,  // state 9: value = the debris pick (-1 none)
    TswDebris,        // TSW_Explosion_Debris_Creation_Frame_Delay frames later: the debris objects (value = pick)
    TswDone,          // aftermath wait over
};

struct SpecialWeaponEvent {
    SwEventKind kind = SwEventKind::Ready;
    ObjectId id = kNoObject;
    ObjectId target = kNoObject;
    int32_t value = 0;
    uint32_t frame = 0;
    Vec3 point;
};

// One queued pulse due this tick: the host launches `projectile` from the weapon's position at `target` (an object)
// or `point` (at_point).
struct SpecialWeaponShot {
    ObjectId weapon = kNoObject;
    int32_t owner = -1;
    int32_t projectile = -1;
    ObjectId target = kNoObject;
    Vec3 point;
    bool at_point = false;
    uint32_t pulse = 0;  // 0-based pulse index of the volley
};

// What the system asks the host (all optional: unset = the permissive answer noted).
struct SpecialWeaponWorld {
    std::function<bool(int32_t owner, ObjectId target)> allied;     // target allied to `owner` (unset: owner equality)
    std::function<int32_t(ObjectId target)> owner_of;               // target owner (unset: -1)
    std::function<uint64_t(ObjectId target)> category_mask;         // target CategoryMask (unset: all bits)
    std::function<bool(ObjectId target)> target_flagged;            // obj +0x3a0 & 0x40 [M] (unset: false)
    std::function<bool(int32_t player)> base_power;                 // Requires_Base_Power gate (unset: true)
    // TSW
    std::function<bool(ObjectId tsw, ObjectId& target, Vec3& position)> find_tsw_target;  // first dummy target
    std::function<Vec3(ObjectId id)> position;                      // current position (TSW placement / approach)
    std::function<void(ObjectId id, Vec3 position, Vec3 facing)> place;  // move the TSW (store, combat, trees)
    std::function<bool(int32_t owner)> is_battle_attacker;          // 0x28aec0 [M] (unset: false)
    std::function<uint32_t(ObjectId tsw)> debris_choices;           // planet debris types (unset: 0)
};

class SpecialWeaponSystem {
public:
    explicit SpecialWeaponSystem(std::vector<SpecialWeaponDesc> descs = {}, SpecialWeaponConfig config = {});

    const SpecialWeaponConfig& config() const { return cfg_; }
    const std::vector<SpecialWeaponDesc>& descs() const { return descs_; }
    uint32_t add_desc(const SpecialWeaponDesc& d);  // returns its index

    // ---- objects ----
    bool add(ObjectId id, uint32_t desc, int32_t owner, uint32_t frame);  // ids in any order (kept sorted)
    void remove(ObjectId id);
    // Removes every pack whose object `keep` rejects (ascending id). Returns the number removed.
    size_t prune(const std::function<bool(ObjectId id)>& keep);
    bool set_owner(ObjectId id, int32_t owner);
    bool has(ObjectId id) const { return find(id) >= 0; }
    uint8_t kinds(ObjectId id) const;
    bool empty() const { return packs_.empty(); }
    size_t size() const { return packs_.size(); }

    // ---- special weapon / lobbing pack (§1.6) ----
    float readiness(ObjectId id, uint32_t frame) const;  // < 0: no pack (not a special weapon / lobbing object)
    bool targeting(ObjectId id) const;
    bool begin_targeting(ObjectId id, uint32_t frame);
    bool fire_at_object(ObjectId id, ObjectId target, uint32_t frame, const SpecialWeaponWorld& w);
    bool fire_lobbing_at(ObjectId id, Vec3 point, uint32_t frame, const SpecialWeaponWorld& w);
    // Lua Force-style helper [port]: restart the pack (start = frame, ready = frame + seconds x fps; < 0 = the
    // type's recharge).
    bool restart(ObjectId id, uint32_t frame, float seconds = -1.0f);
    uint32_t ready_frame(ObjectId id) const;

    // ---- tactical superweapon (§1.2) ----
    int32_t tsw_state(ObjectId id) const;  // < 0: not a TSW
    ObjectId tsw_target(ObjectId id) const;
    // 0x42a200 without the planet test (`planet_ok` = battle planet in the parent store, not destroyed and its type
    // Planet_Is_Destroyable_By_TSW: the host's): state 4 and a target -> 5. True = fired.
    bool tsw_fire(ObjectId id, bool planet_ok, uint32_t frame);

    // ---- per tick (serial, after the combat events) ----
    // Due pulses -> shots(), readiness / TSW events -> events(). Outputs are cleared at the start of each tick.
    void tick(uint32_t frame, SimRng& rng, const SpecialWeaponWorld& w);
    std::span<const SpecialWeaponShot> shots() const { return shots_; }
    std::span<const SpecialWeaponEvent> events() const { return events_; }
    void clear_outputs() {
        shots_.clear();
        events_.clear();
    }

    void hash(StateHasher& h) const;
    // N1 stage 2 (save_file.hpp contract, ability_save.cpp). Between ticks. Saves the descriptor table too (the host
    // adds descriptors lazily per type with add_desc, so a rebuilt battle may not have them yet; the host's own type ->
    // descriptor cache must be saved with it), every pack (charge frames, targeting flag, queued volley, TSW state) and
    // the current outputs. The config is not saved. False (unchanged) on a bad index / truncated data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Pack {
        ObjectId id = kNoObject;
        uint32_t desc = 0;
        int32_t owner = -1;
        uint32_t start = 0, ready = 0;  // +0x8 / +0xc
        uint8_t targeting_flag = 0;     // +0x28
        uint8_t ready_signalled = 0;
        // queued volley
        int32_t pulses_left = 0;
        uint32_t pulse_index = 0;
        uint32_t next_pulse = 0;
        ObjectId shot_target = kNoObject;
        Vec3 shot_point;
        uint8_t at_point = 0;
        // TSW
        int32_t tsw = -1;  // state, -1 = not a TSW
        uint8_t tsw_init = 0;
        ObjectId tsw_target = kNoObject;
        Vec3 tsw_target_pos;
        int32_t countdown = 0;
        int32_t debris_pick = -1;
        uint32_t debris_frame = 0;
        uint8_t debris_pending = 0;
    };
    int32_t find(ObjectId id) const;
    Pack* pack(ObjectId id);
    const Pack* pack(ObjectId id) const;
    float readiness_of(const Pack& p, uint32_t frame) const;
    uint32_t recharge_frames(const SpecialWeaponDesc& d) const;
    void queue_volley(Pack& p, const SpecialWeaponDesc& d, ObjectId target, Vec3 point, bool at_point, uint32_t frame);
    void set_tsw_state(Pack& p, int32_t s, uint32_t frame);
    void tsw_service(Pack& p, const SpecialWeaponDesc& d, uint32_t frame, SimRng& rng, const SpecialWeaponWorld& w);
    void event(SwEventKind k, const Pack& p, uint32_t frame, int32_t value = 0, ObjectId target = kNoObject, Vec3 point = {});

    std::vector<SpecialWeaponDesc> descs_;
    SpecialWeaponConfig cfg_;
    std::vector<Pack> packs_;  // ascending id
    std::vector<SpecialWeaponShot> shots_;
    std::vector<SpecialWeaponEvent> events_;
};

}  // namespace alamo::tactical::abilities

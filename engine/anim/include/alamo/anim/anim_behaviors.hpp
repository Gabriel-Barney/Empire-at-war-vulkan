// R3 behaviour-driven clip selection on top of AnimWorld (docs/re/ANIM_PARTICLES.md §A.7, ABILITIES_HARDPOINTS
// §4.9): flinch (0x62dde0), idle while engaged (0x62e570), the death clip (DeathBehavior 0x58cdb0) and the corpse
// countdown (0x58d430). The per-object behaviour state lives here (sorted by ObjectId); the clip state stays in the
// AnimWorld. Everything is integer frame timers + synced RNG draws in the caller's service order, so it is
// deterministic; the only float the renderer reads (corpse fade fraction) is derived, never hashed.
//
// Rules (tags as in the RE doc):
//   flinch    [H flow, M direction naming] on a hit: type by the angle between the victim's facing and the hit's
//             travel direction (< 45 deg back, 45-135 deg left / right by side, > 135 deg front), 83-86 instead of
//             79-82 while the current clip is attack-like or Attack Idle; random variant (synced), loop, blend
//             Max_Flinch_Blend_Seconds, speed 1. Held Max_Flinch_Hold_Seconds, then no new flinch for
//             Idle_ / Attack_Flinch_Interval_Seconds. No clip of the type = nothing (no draw).
//   engaged   [H] the idle behaviour, current clip attack-like / Attack Idle and finished: Attack Idle (index 0,
//             one-shot, blend 0, speed 1); a fresh engagement (coming from an attack) sets the combat timer =
//             frame + 90 + synced int [0, 30]; a finished idle-like clip while engaged -> Attack Idle with blend =
//             synced float [0.2, 0.75]. Otherwise the plain idle behaviour (AnimWorld::service_idle).
//   death     [H] Die resets the animation state (overlay cleared, flinch dropped), then Specific_Death_Anim_Type /
//             _Index (default Die, 0xFFFF = synced random variant), one-shot, blend 0.5, speed 1. No clip and
//             Remove_Upon_Death -> removed at once.
//   corpse    [H] the Death_Persistence_Duration countdown (-1 = forever) starts once the current clip is no longer
//             the death type or has finished (the death clip length is sim-visible), then Death_Fade_Time, then
//             the object (and its row) is removed.
//
// PORT CHOICES ([port], listed in docs/PHASE4.md R3):
//   * seconds -> frames by truncation int(30 * s) (ABILITIES_HARDPOINTS §6.3), negative -> 0;
//   * flinch only from no clip / idle-like / attack-like / Attack Idle (not while moving, turning or dying); the
//     hold end plays the return clip (Attack Idle when the flinch was an attack flinch and the clip exists, else
//     the default idle with Loop_Idle_Anim_00 / Idle_Anim_00_Rate_Mod) with the flinch blend; both flinch plays
//     use blend mode 2 (rescale a running fade);
//   * left / right: the hit travels toward the victim's left side (cross(facing, dir).z > 0) -> it came from the
//     right -> Flinch_Right;
//   * the death play passes lock = 1 (no later behaviour or Lua call replaces the death clip);
//   * corpse phases cascade within one service (persistence 0 and fade 0 remove the corpse on the frame the
//     death clip ends);
//   * locomotor: move-family priority Deployed_Walk > CROUCHMOVE > FIRE_MOVE > WALKMOVE > Move, phase kept across
//     the whole family; turn clips only for stationary units with a yaw change > 0.01 deg, index 0, speed 1
//     (Rotation_Animation_Speed unused: units not documented), move blend; Walk_ / Crouch_Animation_Speed unused
//     (the doc's walk row names Movement_Animation_Speed only);
//   * death type by damage kind (death_type_for_kind) when the type has no Specific_Death_Anim_Type and the model
//     has the clip; spin-away decided at death with one synced float when 0 < chance < 1 (the original draw was
//     not found, ABILITIES_HARDPOINTS §8 item 10), lasting Spin_Away_On_Death_Time, then removed (no fade).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/anim/anim_state.hpp"

namespace alamo::anim {

struct FlinchParams {
    float blend_seconds = 0.25f;           // Max_Flinch_Blend_Seconds (+0x20e8)
    float hold_seconds = 0.5f;             // Max_Flinch_Hold_Seconds (+0x20ec)
    float idle_interval_seconds = 1.0f;    // Idle_Flinch_Interval_Seconds (+0x20f4)
    float attack_interval_seconds = 3.0f;  // Attack_Flinch_Interval_Seconds (+0x20f0)
};

struct IdleParams {
    int32_t idle_type = kAnimIdle;  // default idle (crouch / garrison mapping: the caller's)
    bool loop_idle = false;         // Loop_Idle_Anim_00
    float idle_rate = 1.0f;         // Idle_Anim_00_Rate_Mod
};

struct DeathParams {
    int32_t type = kAnimDie;              // Specific_Death_Anim_Type (+0xd98)
    bool type_explicit = false;           // the type sets Specific_Death_Anim_Type (no damage-kind mapping then)
    uint32_t index = kRandomVariant;      // Specific_Death_Anim_Index (+0xd9c)
    bool remove_upon_death = false;       // Remove_Upon_Death (+0x9b)
    float persistence_seconds = -1.0f;    // Death_Persistence_Duration (+0x2290), < 0 = forever
    float fade_seconds = 0.25f;           // Death_Fade_Time (+0x2294)
    bool spin_away = false;               // Spin_Away_On_Death (+0x9a)
    float spin_away_chance = 0.0f;        // Spin_Away_On_Death_Chance (+0x2208)
    float spin_away_seconds = 0.0f;       // Spin_Away_On_Death_Time (+0x220c)
};

// Locomotor clip selection (WalkLocomotor 0x61f840, turn 0x620190; §A.7 walk / move and turn rows).
struct LocomotorParams {
    float movement_anim_speed = 1.0f;       // Movement_Animation_Speed (+0x370)
    float deployed_walk_anim_speed = 1.0f;  // Deployed_Walk_Anim_Speed
    float walk_speed_ratio = 0.5f;          // global 0x140b16d04 x max speed: WALKMOVE below it [port value]
    float min_move_ratio = 0.1f;            // [port] floor of the current / max speed factor
    IdleParams idle;
};

struct LocomotorInput {
    bool movable = true;
    float speed = 0.0f, max_speed = 0.0f;  // current / max speed (moving when speed > 0)
    float yaw_deg = 0.0f;                  // facing yaw this tick (turn detection)
    bool crouched = false;                 // behaviour 7 crouch query
    bool burning = false;                  // object +0x3a1 & 2 [M]
    bool deployed = false;                 // deployed walker
    int32_t idle_type = kNoAnimType;       // garrison container mapping (caller); kNoAnimType = default rule
};

// Locomotor / default-idle rules, exposed for tests.
inline constexpr int32_t kAnimFireDie = 62, kAnimDeployedDie = 60, kAnimCaDie = 58, kAnimDeployedCaDie = 59;
inline constexpr int32_t kAnimFwDie = 32, kAnimFtkDie = 36, kAnimFlDie = 41, kAnimEatenDie = 65, kAnimCrushed = 101;
inline constexpr int32_t kAnimTurnLeftCrouch = 72, kAnimTurnRightCrouch = 73;
inline constexpr float kDeployBlendSeconds = 0.0333f;
bool is_move_family(int32_t type);                     // 2, 61, 66, 67, 115
int32_t default_idle_type(const AnimModel* m, const LocomotorInput& in, const IdleParams& idle);
// [port] death type for a damage kind (DamageType enum) when the type does not set Specific_Death_Anim_Type:
// Force_Whirlwind -> FW_DIE, Force_Telekinesis -> FTK_DIE, Force_Lightning -> FL_DIE, Fire -> FIRE_DIE,
// Cable_Attack -> CA_DIE, Cable_Attack_Deployed -> DEPLOYED_CA_DIE, Normal_Deployed -> DEPLOYED_DIE,
// Crush -> CRUSHED, Eat -> EATEN_DIE; kNoAnimType otherwise (used only when the model has that clip).
int32_t death_type_for_kind(uint8_t damage_kind);

// refixf t04: the data-driven part of Die + DeathBehavior, computed WITHOUT an AnimWorld so the sim can schedule the
// removal of a death clone store object identically with and without the animation service (ANIM_PARTICLES §A.7: the
// death clip length is sim-visible; it is read from the model's clip data, not from a running animation).
struct DeathPlan {
    bool spin = false;           // spin-away death (no clip; removed after Spin_Away_On_Death_Time)
    int32_t type = kNoAnimType;  // death clip type that plays (kNoAnimType: none - missing clip, spin or no model)
    uint32_t variant = 0;        // its variant
    bool remove_now = false;     // no clip, no spin and Remove_Upon_Death: removed at once
    bool forever = false;        // Death_Persistence_Duration < 0 (no spin) or a clip that never ends: never removed
    uint64_t lifetime = 0;       // frames from the death to the removal (valid when !remove_now && !forever)
};
// Same draws in the same order as begin_death (spin: one synced float when 0 < chance < 1; then the variant: one int in
// [0, n - 1] when `p.index` is random and `m` has n > 0 variants of the death type - AnimWorld::play's draw). The death
// type is p.type, or death_type_for_kind(damage_kind) when the type sets none and `m` has that clip. m = null: no clip,
// no variant draw. lifetime = what AnimBehaviors::service's countdown gives a corpse that starts at the death frame:
//   spin   Spin_Away_On_Death_Time;
//   clip   one_shot_frames(clip, speed 1) + 1 (the corpse service runs before the advance, so it sees the finished clip
//          one frame later) + Death_Persistence_Duration + Death_Fade_Time;
//   none   Death_Persistence_Duration + Death_Fade_Time.
DeathPlan plan_death(const AnimModel* m, const DeathParams& p, uint8_t damage_kind, SimRng& rng);
inline constexpr uint64_t kNeverFrames = ~uint64_t{0};
// Sim frames a one-shot clip of `frame_count` frames at `fps` x `speed` takes to finish after the frame it starts on
// (AnimWorld::advance with dt = 1/30 per frame, stopping at max(count - 2, 0); the start frame's own advance has dt 0, so
// a clip of <= 2 frames finishes there: 0); kNeverFrames when it makes no progress.
uint64_t one_shot_frames(uint32_t frame_count, float fps, float speed);

inline constexpr float kDeathBlendSeconds = 0.5f;
inline constexpr uint32_t kEngagedBaseFrames = 90;   // combat timer = frame + 90 + int [0, 30]
inline constexpr int32_t kEngagedRandomFrames = 30;

// int(30 * seconds), 0 for negative / NaN.
uint64_t seconds_to_frames(float seconds);
// Flinch type for a hit travelling along (dx, dy) on a victim facing (fx, fy) (2-D, z ignored). attacking: the
// Attack_Flinch_* set. A zero direction or facing counts as a hit from the front.
int32_t flinch_type(float fx, float fy, float dx, float dy, bool attacking);

enum class CorpsePhase : uint8_t { Dying = 0, Persisting = 1, Fading = 2, SpinAway = 3 };

// Render-side data of a corpse is captured at death (the store object of a battle unit is removed at death,
// [port]: the corpse lives on as an AnimWorld row + this record until the countdown ends).
struct CorpseInfo {
    uint32_t object_type = 0xFFFFFFFFu;  // GameObjectType index
    int32_t owner = -1;
    float position[3] = {0.0f, 0.0f, 0.0f};
    float facing[3] = {1.0f, 0.0f, 0.0f};
};

struct Corpse {
    ObjectId id = 0;
    CorpsePhase phase = CorpsePhase::Dying;
    int32_t death_type = kNoAnimType;  // the type played (kNoAnimType: no clip)
    bool forever = false;              // persistence < 0: never fades
    bool spin_away = false;            // spin-away death: no clip, removed after Spin_Away_On_Death_Time
    uint8_t damage_kind = 1;           // killing damage kind
    bool detached = false;             // the store object is gone (render draws it from `info`)
    uint64_t death_frame = 0;
    uint64_t phase_end = 0;            // Persisting / Fading: frame the phase ends
    uint64_t persist_frames = 0, fade_frames = 0;
    CorpseInfo info;
    // Render helper: 1 until the fade starts, then linearly down to 0 at the end (never hashed).
    float alpha(uint64_t frame) const;
};

enum class DeathResult : uint8_t { Corpse = 0, RemoveNow, NoRow };

class AnimBehaviors {
public:
    // ---- flinch ----
    // Returns true when a flinch clip started. `idle` = the clip to return to after the hold.
    bool on_hit(AnimWorld& w, ObjectId id, const FlinchParams& p, const IdleParams& idle, float fx, float fy, float dx, float dy,
                SimRng& rng, uint64_t frame);
    bool flinching(ObjectId id) const;
    uint64_t next_flinch_frame(ObjectId id) const;  // 0 when none recorded

    // ---- idle behaviour (incl. idle while engaged) ----
    // Runs only when the current clip is finished or absent (or the crouch state changed). True if it played.
    bool service_idle(AnimWorld& w, ObjectId id, const IdleParams& idle, SimRng& rng, uint64_t frame, bool crouch_changed = false);
    uint64_t combat_timer(ObjectId id) const;  // 0 when none recorded

    // ---- locomotor (one call per unit per tick, the battle's locomotor step) ----
    // moving: Move family by state (Deployed_Walk 115 > CROUCHMOVE 67 > FIRE_MOVE 61 > WALKMOVE 66 when slow >
    // Move 2; a missing clip falls back to Move), loop, speed = (Deployed_Walk_Anim_Speed for 115, else
    // Movement_Animation_Speed) x max(speed / max, min); restarted only on a type change: inside the family the
    // phase is kept (switch_preserving_phase), from outside a synced random start frame. Stopped: a Move-family
    // clip goes back to the default idle (loop Loop_Idle_Anim_00, idle blend); a stationary yaw change plays
    // TURNL / TURNR (CROUCHTURNL / R when crouched; one-shot, index 0, move blend, speed 1 [port]) while turning;
    // otherwise the idle behaviour (crouch change forces it). Returns the number of plays started (0 / 1).
    uint32_t service_locomotor(AnimWorld& w, ObjectId id, const LocomotorParams& p, const LocomotorInput& in, SimRng& rng, uint64_t frame);

    // ---- deploy / undeploy (0x61e930: 25 / 26, synced random variant, one-shot, blend 0.0333, speed
    // Deployment_Anim_Rate). Returns true if a clip started (current_frame_count gives its length). ----
    bool deploy(AnimWorld& w, ObjectId id, bool undeploy, float deployment_anim_rate, SimRng& rng, uint64_t frame);

    // ---- death / corpse ----
    // Die + DeathBehavior start. NoRow: `id` has no AnimWorld row (a corpse without clip is still recorded,
    // unless remove_upon_death). RemoveNow: the row (if any) was removed; the caller removes the object.
    // `damage_kind` = the killing hit's DamageType (death_type_for_kind when the type has no explicit death type).
    // Spin-away (Spin_Away_On_Death): [port] decided here, one synced float [0, 1) < chance when 0 < chance < 1
    // (chance >= 1 or unset: always); a spinning death plays no clip and is removed after Spin_Away_On_Death_Time.
    DeathResult begin_death(AnimWorld& w, ObjectId id, const DeathParams& p, const CorpseInfo& info, SimRng& rng, uint64_t frame,
                            uint8_t damage_kind = 1);
    // refixf t04: the same with the spin decision / death type / variant already made by plan_death (no draws): a death
    // clone whose removal the sim schedules. The clip plays when the row's model has plan.type / plan.variant.
    DeathResult begin_death(AnimWorld& w, ObjectId id, const DeathParams& p, const DeathPlan& plan, const CorpseInfo& info, uint64_t frame,
                            uint8_t damage_kind = 1);
    bool is_corpse(ObjectId id) const;
    const Corpse* corpse(ObjectId id) const;
    std::span<const Corpse> corpses() const { return corpses_; }  // ascending id
    void detach(ObjectId id);                                      // store object gone, corpse kept

    // ---- per frame (before AnimWorld::advance) ----
    // Ends flinch holds (return clip) and runs the corpse countdown in ascending id. Corpses whose countdown
    // ended are removed (row + state) and their ids appended to `removed` (the caller removes the objects).
    void service(AnimWorld& w, uint64_t frame, std::vector<ObjectId>& removed);

    // Drops every behaviour state of `id` (object removed by someone else).
    void remove(ObjectId id);
    void hash(StateHasher& h) const;
    size_t state_count() const { return entries_.size(); }

    // ---- N1 save / load (contract: alamo/core/save_file.hpp; src/anim_save.cpp) ----
    // Layout: u32 version (1); u32 count + every behaviour entry in ascending id (i32 id, i32 flinch type, u64 flinch
    // end, u64 next flinch, f32 flinch blend, i32 return type, bool return loop, f32 return speed, u64 combat timer,
    // f32 last yaw, bool have yaw, bool last crouch); u32 count + every corpse in ascending id (i32 id, u8 phase, i32
    // death type, bool forever, bool spin-away, u8 damage kind, bool detached, u64 death frame, u64 phase end, u64
    // persist frames, u64 fade frames, CorpseInfo: u32 object type, i32 owner, f32 position[3], f32 facing[3]).
    // Independent of the AnimWorld (the host reads it before the AnimWorld). False and unchanged on bad data; reads
    // its own fields only (the owner checks done()).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Entry {
        ObjectId id = 0;
        int32_t flinch = kNoAnimType;  // flinch type being held
        uint64_t flinch_end = 0, flinch_next = 0;
        float flinch_blend = 0.0f;
        int32_t ret_type = kAnimIdle;
        bool ret_loop = false;
        float ret_speed = 1.0f;
        uint64_t combat_timer = 0;
        float last_yaw = 0.0f;  // locomotor
        bool have_yaw = false, last_crouch = false;
    };
    Entry* find(ObjectId id);
    const Entry* find(ObjectId id) const;
    Entry& entry(ObjectId id);
    Corpse* find_corpse(ObjectId id);

    std::vector<Entry> entries_;   // ascending id
    std::vector<Corpse> corpses_;  // ascending id
};

}  // namespace alamo::anim

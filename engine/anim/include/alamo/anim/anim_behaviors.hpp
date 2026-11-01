// R3 behaviour-driven clip selection on top of AnimWorld (docs/re/ANIM_PARTICLES.md §A.7, ABILITIES_HARDPOINTS
// §4.9): flinch (0x62dde0), idle while engaged (0x62e570), the death clip (DeathBehavior 0x58cdb0) and the corpse
// countdown (0x58d430). The per-object behaviour state lives here (sorted by ObjectId); the clip state stays in the
// AnimWorld. Everything is integer frame timers + synced RNG draws in the caller's service order, so it is
// deterministic; the only float the renderer reads (corpse fade fraction) is derived, never hashed.
//
// Rules (tags as in the RE doc):
//   flinch    [H flow, M direction naming] §A.12 (0x62dde0 projectile hit, 0x62e040 EXPLOSION): gates - the victim's
//             type is not Deploys, the flinch timer has expired and no flinch is in progress, the current primary
//             type C is idle-like (0, 1, 71, 77, 78, 79-82: flinch set 79-82) or attack-like (flinch set 83-86);
//             Attack Idle (6), no clip, moving, turning, death and every other clip never flinch. Type by
//             D = wrap(yaw(hit direction) - facing yaw) into (-180, 180]: |D| < 45 back (82 / 86), 45-135 by sign
//             (D > 0: 80 / 84 Right, D < 0: 79 / 83 Left), > 135 front (81 / 85). Random variant (synced), loop,
//             blend Max_Flinch_Blend_Seconds, speed 1; intensity +0x58 = I (projectile Induced_Flinch_Intensity, or
//             explosion_flinch_intensity), +0x5c = C. No clip of the type = nothing (no draw). Progress, in
//             service() [L flow, M weight meaning]: BlendIn - once the blend-in weight reaches I or the fade has
//             ended, the clip freezes (speed 1e-4), the flinch timer = now + Max_Flinch_Hold_Seconds x I, +0x58 = 0;
//             Hold - when the timer expires, C is replayed (synced random variant, one-shot, blend
//             Max_Flinch_Blend_Seconds, blend mode 3, speed 1); Return - once the model is back on C with the fade
//             complete, the timer = now + Attack_Flinch_Interval_Seconds (C attack-like or 6) or
//             Idle_Flinch_Interval_Seconds, and +0x5c = -1. A new flinch needs the timer expired and +0x58 = 0, so
//             one may start during Return (literal reading of the gates [M]).
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
//   * the flinch play uses blend mode 2 (rescale a running fade; §A.12 names no mode for it);
//   * each service makes at most one flinch phase step (BlendIn -> Hold -> Return -> cooldown);
//   * a flinch whose clip is replaced by another play (BlendIn / Hold: current type != the flinch type; Return:
//     current type != C), or whose replay of C fails, ends at once and its cooldown starts on that frame;
//   * a flinch in BlendIn also blocks a new one (I = 0 would leave +0x58 = 0 there);
//   * the replay of C is at speed 1 (Idle_Anim_00_Rate_Mod / loop flags of the interrupted clip are not restored);
//   * the replay draws its random variant from the rng passed to service(); null = variant 0;
//   * intensity is clamped to [0, 1] (vanilla never sets Induced_Flinch_Intensity: 1.0 everywhere);
//   * D sign: yaw = atan2(y, x) in the xy plane, so D > 0 <=> cross(facing, dir).z > 0 -> Flinch_Right (80 / 84), as
//     §A.12 gives (no longer a port reading);
//   * the death play passes lock = 1 (no later behaviour or Lua call replaces the death clip);
//   * corpse phases cascade within one service (persistence 0 and fade 0 remove the corpse on the frame the
//     death clip ends);
//   * locomotor: move-family priority Deployed_Walk > CROUCHMOVE > FIRE_MOVE > WALKMOVE > Move, phase kept across
//     the whole family; turn clips only for stationary units with a yaw change > 0.01 deg, index 0, speed 1
//     (Rotation_Animation_Speed unused: units not documented), move blend; Walk_ / Crouch_Animation_Speed unused
//     (the doc's walk row names Movement_Animation_Speed only);
//   * default idle (§A.11): the locomotor's stop path falls back to IdleParams::base_idle_type when the default idle
//     type (crouch 71 / presence 77, 78) has no clip (the idle service itself keeps the type: its play fails, retried
//     every frame, and the unit holds the last frame of its finished idle, §A.11);
//   * death type by damage kind (death_type_for_kind) when the type has no Specific_Death_Anim_Type and the model
//     has the clip;
//   * spin-away is NOT drawn here: TACTICAL_GAPS §1.9 [H] (DeathBehavior Initialise 0x58cdb0, ABILITIES_HARDPOINTS
//     §8 item 10) makes ONE synced Get_Float(0, 1) per death, spin when draw <= Spin_Away_On_Death_Chance, and the
//     port makes it once, in combat (DeathEvent::spin_away). The caller passes that decision in DeathParams::spin;
//     a spinning death plays no clip, lasts Spin_Away_On_Death_Time, then is removed (no fade).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/anim/anim_state.hpp"

namespace alamo::anim {

struct FlinchParams {
    bool deploys = false;                  // the victim's type has Deploys (+0xd1): never flinches (§A.12)
    float blend_seconds = 0.25f;           // Max_Flinch_Blend_Seconds (+0x20e8)
    float hold_seconds = 0.5f;             // Max_Flinch_Hold_Seconds (+0x20ec)
    float idle_interval_seconds = 1.0f;    // Idle_Flinch_Interval_Seconds (+0x20f4)
    float attack_interval_seconds = 3.0f;  // Attack_Flinch_Interval_Seconds (+0x20f0)
};

struct IdleParams {
    int32_t idle_type = kAnimIdle;  // default idle T of 0x395920 (presence 77 / 78 or crouch 71: the caller's), even without a clip
    int32_t base_idle_type = kAnimIdle;  // the default idle without presence / crouch (0, or Space Idle): the locomotor's
                                         // stop-path fallback when T has no clip ([port], §A.11)
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
    bool spin = false;                    // the caller's spin-away decision (TACTICAL_GAPS §1.9: combat's one synced draw
                                          // under Spin_Away_On_Death +0x9a, DeathEvent::spin_away); never drawn here.
                                          // death_params() leaves it false.
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
    int32_t idle_type = kNoAnimType;       // presence-induced idle (77 / 78, §A.11; the caller's); kNoAnimType = none
};

// Locomotor / default-idle rules, exposed for tests.
inline constexpr int32_t kAnimFireDie = 62, kAnimDeployedDie = 60, kAnimCaDie = 58, kAnimDeployedCaDie = 59;
inline constexpr int32_t kAnimFwDie = 32, kAnimFtkDie = 36, kAnimFlDie = 41, kAnimEatenDie = 65, kAnimCrushed = 101;
inline constexpr int32_t kAnimTurnLeftCrouch = 72, kAnimTurnRightCrouch = 73;
inline constexpr float kDeployBlendSeconds = 0.0333f;
bool is_move_family(int32_t type);                     // 2, 61, 66, 67, 115
// 0x395920 (§A.11): 71 Idle Crouch when crouched (SPREAD_OUT), else in.idle_type (presence idle) when set, else
// idle.idle_type. The type is kept even when `m` has no clip of it (the idle service's play then fails, §A.11).
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
// Same draws in the same order as begin_death: none for spin-away (p.spin is the caller's decision, §1.9); without a
// spin, one int in [0, n - 1] when `p.index` is random and `m` has n > 0 variants of the death type (AnimWorld::play's
// variant draw). The death type is p.type, or death_type_for_kind(damage_kind) when the type sets none and `m` has that
// clip. m = null: no clip, no variant draw. lifetime = what AnimBehaviors::service's countdown gives a corpse that starts at the death frame:
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
// Flinch type for a hit travelling along (dx, dy) on a victim facing (fx, fy) (2-D, z ignored; §A.12 angle rule above).
// attacking: the Attack_Flinch_* set. A zero direction or facing counts as a hit from the front.
int32_t flinch_type(float fx, float fy, float dx, float dy, bool attacking);
// §A.12 defaults: Induced_Flinch_Intensity (projectile +0x20e4 / explosion type) and Induced_Flinch_Range (explosion
// type +0x20e0: half-extent of the GOM-wide victim cube).
inline constexpr float kDefaultFlinchIntensity = 1.0f;
inline constexpr float kDefaultFlinchRange = 100.0f;
inline constexpr float kMinExplosionFlinchIntensity = 0.25f;
inline constexpr float kFlinchFrozenSpeed = 1.0e-4f;  // 0x3a8120: the frozen flinch clip's speed
// EXPLOSION victim intensity (0x62e040): clamp(I x (1 - 0.5 x d2 / R2), 0.25, 1) with d2 = dx^2 + dy^2 the horizontal
// squared distance victim - explosion and R = Induced_Flinch_Range. R <= 0: clamp(I, 0.25, 1) [port].
float explosion_flinch_intensity(float intensity, float dx, float dy, float range);
// Flinch gate on the current primary type alone (§A.12): idle-like or attack-like, never Attack Idle / none.
bool flinch_allowed_from(int32_t current_type);

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
    // Returns true when a flinch clip started. (fx, fy) = the victim's facing; (dx, dy) = D of §A.12: the projectile's
    // last-move velocity, or victim position - explosion position. `intensity` = the projectile's
    // Induced_Flinch_Intensity, or explosion_flinch_intensity(...) for an EXPLOSION victim (default 1 = full flinch).
    // `idle` is unused since §A.12 (the interrupted clip C is replayed); kept for the call sites. Rules: header comment.
    bool on_hit(AnimWorld& w, ObjectId id, const FlinchParams& p, const IdleParams& idle, float fx, float fy, float dx, float dy,
                SimRng& rng, uint64_t frame, float intensity = kDefaultFlinchIntensity);
    enum class FlinchPhase : uint8_t { None = 0, BlendIn = 1, Hold = 2, Return = 3 };
    FlinchPhase flinch_phase(ObjectId id) const;
    bool flinching(ObjectId id) const;              // BlendIn or Hold (the flinch clip is the current clip)
    uint64_t next_flinch_frame(ObjectId id) const;  // the flinch timer (+0x50): hold end in Hold, cooldown end after; 0 = none
    int32_t flinch_return_type(ObjectId id) const;  // +0x5c: the interrupted type C (kNoAnimType when no flinch)

    // ---- idle behaviour (incl. idle while engaged) ----
    // Runs only when the current clip is finished or absent, or when idle_class_restart(C, T = idle.idle_type): C is
    // an idle or crouch clip of the other crouch class (§A.11, restart mid-clip; flinch / attack / Attack Idle are never
    // cut); crouch_changed forces the restart too. Presence idles (77 Attention, 78 Celebrate) are ordinary idle types: same fidget timer and
    // synced variant draw; without a clip of T the play fails (the unit holds its finished clip) and is retried on
    // the next call. True if it played.
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
    // Spin-away: p.spin, the caller's decision (TACTICAL_GAPS §1.9, combat's DeathEvent::spin_away); nothing is drawn
    // for it here. A spinning death plays no clip and is removed after Spin_Away_On_Death_Time.
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
    // Steps the flinches (freeze, hold end -> replay C, back on C -> cooldown; header comment) and runs the corpse
    // countdown, both in ascending id. Corpses whose countdown ended are removed (row + state) and their ids appended to
    // `removed` (the caller removes the objects). `rng`: the synced RNG for the replay's random variant (in ascending
    // id order); null = variant 0.
    void service(AnimWorld& w, uint64_t frame, std::vector<ObjectId>& removed, SimRng* rng = nullptr);

    // Drops every behaviour state of `id` (object removed by someone else).
    void remove(ObjectId id);
    void hash(StateHasher& h) const;
    size_t state_count() const { return entries_.size(); }

    // ---- N1 save / load (contract: alamo/core/save_file.hpp; src/anim_save.cpp) ----
    // Layout: u32 version (3: R3 §A.12 flinch phases); u32 count + every behaviour entry in ascending id (i32 id, i32
    // flinch type, u8 flinch phase, u64 flinch timer, f32 flinch intensity, i32 return type C, f32 flinch blend, u64
    // hold frames, u64 cooldown frames, u64 combat timer, f32 last yaw, bool have yaw, bool last crouch); u32 count + every corpse in ascending id (i32 id, u8 phase, i32
    // death type, bool forever, bool spin-away, u8 damage kind, bool detached, u64 death frame, u64 phase end, u64
    // persist frames, u64 fade frames, CorpseInfo: u32 object type, i32 owner, f32 position[3], f32 facing[3]).
    // Independent of the AnimWorld (the host reads it before the AnimWorld). False and unchanged on bad data; reads
    // its own fields only (the owner checks done()).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Entry {
        ObjectId id = 0;
        int32_t flinch = kNoAnimType;  // flinch clip type (kNoAnimType when phase None)
        FlinchPhase phase = FlinchPhase::None;
        uint64_t flinch_timer = 0;      // +0x50
        float flinch_intensity = 0.0f;  // +0x58: the blend weight at which the clip freezes (BlendIn only, else 0)
        int32_t ret_type = kNoAnimType; // +0x5c: C, replayed at the hold end
        float flinch_blend = 0.0f;      // the victim's Max_Flinch_Blend_Seconds
        uint64_t hold_frames = 0;       // int(30 x Max_Flinch_Hold_Seconds x I)
        uint64_t cooldown_frames = 0;   // int(30 x Attack_ / Idle_Flinch_Interval_Seconds) by C
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

// L1 systems presentation: the tactical super weapon's sound / camera cues (refix t06), the in-game cinematic camera's
// explosion shake and the TSW firing shot (refixf t05). Spec: docs/re/TSW_PLANET_DESTROY.md §3 (state table), §4 steps
// 4-5, §11 (presentation: camera shake 0x2cdbb0 / 0x2cdc00, firing camera 0x2ccf30 / 0x4a5ef0), TACTICAL_GAPS.md §7.1.
//
//   tsw_cues        turns one battle tick's TacticalBattle::special_weapon_events() into cues the hosts play:
//                     Firing          fire accepted (state 5): the TSW type's TSW_SFXEvent_Activate_Voice,
//                                     TSW_SFXEvent_Weapon_Power_Up and music TSW_MusicEvent_Activation_Start (mode 8);
//                                     the firing shot (§11.2) starts
//                     AttackAnim      state 7: the TSW type's SFXEvent_Fire
//                     PlanetDestroyed state 9 with a live target dummy: camera shake (0x2cdbb0, §11.1) at the dummy and
//                                     its type's Death_SFXEvent_Start_Die; the sim has killed the dummy (silently) and
//                                     spawned the Death_Explosions pick (`pick`) in the same tick.
//                     LobChargeup     S8 t31 (TACTICAL_COMBAT.md "Charge-up"): a LOBBING_SUPERWEAPON unit was aimed at its
//                                     fire-at-position point: its type's Lobbing_Superweapon_SFXEvent_Chargeup and the
//                                     Lobbing_Superweapon_Chargeup_Particle on Lobbing_Superweapon_Chargeup_Particle_Bone_Name
//                     LobCharged      its Lobbing_Superweapon_Chargeup_Frames ran out (the order is charged):
//                                     Lobbing_Superweapon_SFXEvent_Fire
//                   Read right after the battle tick: the dummy killed at state 9 is still in the store (dead, not
//                   flushed until the next tick's store update), so its type is readable. No dummy (§4 step 3) = no cue.
//   track_lob_chargeups  the charge-ups whose particle is shown (BattleView attaches it to the unit's bone).
//   noise / ease    §11.1: Perlin's improved gradient noise with the CUBIC fade, its 3-octave fBm, the shake envelope
//                   (0x7da450) and the shot's trapezoid speed profile (0x49bb40).
//   CameraShake     §11.1 CameraEffectTrackers of the cinematic controller: each {effect, start, duration} is evaluated at
//                   the integer shot clock; trackers are only freed with the cinematic (End_Cinematic_Mode).
//   TswFiringShot   §11.2 the shot Fire starts: eye / look key frames on the TSW (attached keys follow its transform),
//                   the hold and the cut at frame 315, the optional `Space Station Position` leg to 765, input skip rules.
//   Letterbox       §11.2 step 3: the bars slide in / out over 0.5 s (wall time).
//
// Presentation only: const reads of the battle between ticks, no sim writes, no synced draws (the shake's NoiseOffset
// comes from UnsyncedRandom, never the lockstep RNG), so the CRC is the same with the presentation on or off.
#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "alamo/objects/object_store.hpp"
#include "alamo/tactical/geometry.hpp"

namespace alamo::tactical::abilities {
struct SpecialWeaponEvent;
}
namespace alamo::objects {
class GameObjectTypes;
}

namespace alamo::game {

struct TswCue {
    enum class Kind : uint8_t { Firing, AttackAnim, PlanetDestroyed, LobChargeup, LobCharged };
    Kind kind = Kind::Firing;
    objects::ObjectId object = objects::kNoObjectId;  // the TSW (Firing / AttackAnim), the target dummy (PlanetDestroyed)
                                                      // or the lobbing unit (LobChargeup / LobCharged)
    objects::TypeIndex type = objects::kNone;         // that object's type
    tactical::Vec3 position;                          // the object's position (PlanetDestroyed: the dummy's point)
    objects::ObjectId target = objects::kNoObjectId;  // Firing / AttackAnim: the target dummy (kNoObjectId when none)
    int32_t pick = 0;                                 // PlanetDestroyed: index of the Death_Explosions entry the sim spawned
    uint32_t frame = 0;                               // LobChargeup / LobCharged: the battle frame of the event
};

// In event order. Events whose object is not in `store` (flushed / unknown) give no cue.
std::vector<TswCue> tsw_cues(std::span<const tactical::abilities::SpecialWeaponEvent> events, const objects::ObjectStore& store);

// S8 t31: `active` = the charge-ups whose Lobbing_Superweapon_Chargeup_Particle is shown, in start order. A LobChargeup
// cue adds its unit (a unit already listed is restarted: new frame, moved to the end), a LobCharged cue removes it. The
// host also drops the units whose order is gone (cancelled, unit dead: CombatSystem::fire_at_position_state != 1)
// [port: the particle is listed for the charge-up only; the spec just "starts" it].
void track_lob_chargeups(std::vector<TswCue>& active, std::span<const TswCue> cues);

// ---- §11.1 noise and envelopes --------------------------------------------------------------------------------------
inline constexpr int32_t kCinematicFps = 30;  // FPS of the cinematic controller (frames = tactical frames)

// Ken Perlin's reference permutation (stored twice in the original, 0x140a45d20).
std::span<const uint8_t, 256> perlin_permutation();
// N (0x7d8590): improved gradient noise with the cubic fade 3f^2 - 2f^3.
float improved_noise(float x, float y, float z);
// Fbm (0x7d8a50): N(p) + N(1.9 p) / 1.9 + N(6.859 p) / 3.61 (cumulative scaling, 3 octaves).
float noise_fbm(float x, float y, float z);
// Ease (0x7da450): 0 -> smooth rise over [0, ease_in], 1, smooth fall over [1 - ease_out, 1] -> 0.
float shake_ease(float t, float ease_in, float ease_out);
// Accel (0x49bb40): trapezoid speed profile; `a` = acceleration share at the start, `b` = deceleration share at the end.
// accel(0) = 0, accel(1) = 1.
float shot_accel(float s, float a, float b);

// A CameraShakeEffect (the parameters 0x2cdc00 stores).
struct ShakeEffect {
    float x_amplitude = 0.0f, y_amplitude = 0.0f;  // world units at the look point (0.1 x at the eye)
    float roll_amplitude = 0.0f;                   // degrees
    float frequency = 0.0f;                        // noise lattice cells per unit of t (t = 0..1 over the duration)
    float noise_offset = 0.0f;                     // 0.00 .. 2.00
    float ease_in = 0.0f, ease_out = 0.25f;
};
// 0x2cdc00's parameters: J = the explosion type's Explosion_Jitter_Factor, D = 100 for the TSW (fixed = 1) else
// |object - camera| + 0.1; `noise_draw` = int(0, 100) from the UNSYNCED generator (NoiseOffset = draw / 50).
ShakeEffect explosion_shake(float jitter, float distance, int32_t noise_draw);
inline constexpr float kTswShakeDistance = 100.0f;
// Explosion_Jitter_Factor of `explosion` (type +0x2130), 1.0 when the type does not set it (constructor default).
float explosion_jitter_factor(const objects::GameObjectTypes& types, objects::TypeIndex explosion);
// The Death_Explosions entry `pick` of `dummy_type` (kNone when the list is empty / the index is out of range: no
// explosion and no shake, §11.3).
objects::TypeIndex tsw_explosion_type(const objects::GameObjectTypes& types, objects::TypeIndex dummy_type, int32_t pick);

// The presentation generator (0x140a2ab18: seeded from the wall clock at boot, never the lockstep LCG). xorshift64* [port:
// the original's generator is not specified beyond being unsynced].
class UnsyncedRandom {
public:
    explicit UnsyncedRandom(uint64_t seed = 0xf0f0f0f0u) { reseed(seed); }
    void reseed(uint64_t seed) { s_ = seed != 0 ? seed : 0x9e3779b97f4a7c15ull; }
    int32_t range(int32_t lo, int32_t hi);  // inclusive; lo >= hi -> lo
private:
    uint64_t s_ = 1;
};

// The shake trackers of the cinematic controller (+0x278).
class CameraShake {
public:
    static constexpr size_t kMaxTrackers = 256;  // [port] memory bound; the original never drops one before the cinematic ends
    void start(const ShakeEffect& e, int32_t start_clock, int32_t duration_frames = kCinematicFps);
    struct Sample {
        tactical::Vec3 offset;  // dx * R + dy * U (world)
        float roll_deg = 0.0f;
    };
    // Sum over the trackers at shot clock `clock`; `right` / `up` = the unshaken camera's normalised right / up axes.
    Sample evaluate(int32_t clock, const tactical::Vec3& right, const tactical::Vec3& up) const;
    void reset() { trackers_.clear(); }
    bool empty() const { return trackers_.empty(); }
    size_t count() const { return trackers_.size(); }

private:
    struct Tracker {
        ShakeEffect e;
        int32_t start = 0, duration = kCinematicFps;
    };
    std::vector<Tracker> trackers_;  // start order
};

// 0x2cdc00's gate and tracker: nothing (no tracker, no draw) unless the shake enable (+0x34d, always on), a land / space
// mode (`battle_mode`) and a running cinematic (+0x34a) all hold; else one tracker of FPS frames from `clock` with the
// NoiseOffset drawn from `rng`. True when a tracker was added.
bool cinematic_shake(CameraShake& shake, bool cinematic_active, bool battle_mode, float jitter, float distance, int32_t clock,
                     UnsyncedRandom& rng);

// Cinematic mode on saves the mode camera once (+0x2c8); End_Cinematic_Mode restores it (projection included).
template <class Camera>
class SavedCamera {
public:
    void save_once(const Camera& c) {
        if (!saved_) saved_.emplace(c);
    }
    bool saved() const { return saved_.has_value(); }
    bool restore(Camera& c) {  // false when nothing was saved
        if (!saved_) return false;
        c = *saved_;
        saved_.reset();
        return true;
    }
    void drop() { saved_.reset(); }

private:
    std::optional<Camera> saved_;
};

// A cinematic camera pose: keyframed position, look point and roll, then the shake (end of 0x49cd00).
struct CinematicPose {
    tactical::Vec3 eye, look;
    tactical::Vec3 up{0.0f, 0.0f, 1.0f};  // rolled camera up (world Z-up)
    float roll_deg = 0.0f;
};
// Unshaken camera axes for eye -> look with `roll_deg` about the view axis (0x22d600: column 0 = right, 1 = up).
void camera_axes(const tactical::Vec3& eye, const tactical::Vec3& look, float roll_deg, tactical::Vec3& right, tactical::Vec3& up);
// eye = pos + 0.1 offset; look = pos + 1000 normalise(look - pos) + offset; roll += shake roll.
CinematicPose apply_shake(const tactical::Vec3& pos, const tactical::Vec3& look, float roll_deg, const CameraShake& shake, int32_t clock);

// ---- §11.2 the firing shot ------------------------------------------------------------------------------------------
// An object's render transform without scale: position + the model axes in world space (columns: model +X, +Y, +Z).
struct ObjectFrame {
    bool valid = false;
    tactical::Vec3 position;
    tactical::Vec3 axis[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    tactical::Vec3 apply(const tactical::Vec3& local) const {
        return position + axis[0] * local.x + axis[1] * local.y + axis[2] * local.z;
    }
};

// A key frame (0x54-byte record, the fields this shot uses).
struct ShotKey {
    enum class Segment : uint8_t { Spline = 0, Linear = 1, Hold = 2, Orbit = 3 };  // mode of the segment starting here
    int32_t frame = 0;
    Segment segment = Segment::Spline;
    bool attached = false;   // offset in the TSW's frame (+ Ranged_Target_Z_Adjust); else a world position
    tactical::Vec3 where;    // offset (attached) or world position
    float ease_out = 0.0f;   // +0x44: acceleration share of the segment starting at this key
    float ease_in = 0.0f;    // +0x48: deceleration share of the segment ending at this key
};

// What 0x4a5ef0 reads once at Fire.
struct TswShotInputs {
    tactical::Vec3 tsw_position, dummy_position;  // translations of the render transforms
    tactical::Vec3 half_extents{1.0f, 1.0f, 1.0f};  // E (model box x Scale_Factor) [M: half-extents]
    float z_adjust = 6.0f;                        // the TSW type's Ranged_Target_Z_Adjust (default 6.0)
    bool have_station = false;                    // a `Space Station Position` marker exists
    tactical::Vec3 station_position;
};

class TswFiringShot {
public:
    static constexpr int32_t kGlideEnd = 300;    // FPS x 10
    static constexpr int32_t kCut = 315;         // (int)(FPS x 10.5)
    static constexpr int32_t kLength = 765;      // (int)(FPS x 25.5): the shot timer
    static constexpr float kNearPlane = 100.0f;  // 0x4a5ef0 (after cinematic mode on set 1.0 / 10000)
    static constexpr float kFarPlane = 10000.0f;
    static constexpr float kStationBeyond = 2000.0f;

    // 0x4a5ef0 + cinematic mode on. `fire_frame` = the tactical frame of Fire (shot clock 0); `cursor_*` = the cursor at
    // Fire (baseline of the move-skip test).
    void begin(const TswShotInputs& in, uint32_t fire_frame, int32_t cursor_x, int32_t cursor_y);
    void end() { active_ = false; }
    bool active() const { return active_; }
    uint32_t fire_frame() const { return fire_frame_; }
    // Shot clock (+0x154) at tactical frame `frame`: frames since Fire.
    int32_t clock(uint32_t frame) const { return frame >= fire_frame_ ? static_cast<int32_t>(frame - fire_frame_) : 0; }
    bool expired(uint32_t frame) const { return clock(frame) >= kLength; }

    std::span<const ShotKey> eye_keys() const { return eye_; }
    std::span<const ShotKey> look_keys() const { return look_; }
    const TswShotInputs& inputs() const { return in_; }

    // Keyframed eye / look at shot clock `c` with the TSW's current frame (invalid: the TSW is gone - its last valid frame
    // is used, so attached keys keep their previous positions).
    tactical::Vec3 eye(int32_t c, const ObjectFrame& tsw);
    tactical::Vec3 look(int32_t c, const ObjectFrame& tsw);

    // §11.2 input (0x2cc3e0, state 3): a mouse move within 1/20 of the screen width AND height of the baseline is swallowed,
    // a larger move or any button / wheel event ends the shot. True = end it.
    bool input_cancels(int32_t mouse_x, int32_t mouse_y, uint32_t width, uint32_t height, bool button_or_wheel) const;

private:
    tactical::Vec3 evaluate(std::span<const ShotKey> keys, int32_t c, const ObjectFrame& tsw) const;
    tactical::Vec3 key_position(const ShotKey& k, const ObjectFrame& tsw) const;
    const ObjectFrame& frame_of(const ObjectFrame& tsw);

    bool active_ = false;
    uint32_t fire_frame_ = 0;
    int32_t cursor_x_ = 0, cursor_y_ = 0;
    TswShotInputs in_;
    std::vector<ShotKey> eye_, look_;
    ObjectFrame last_tsw_;
};

// Letterbox bars (0x44b2c0 in / 0x44b6f0 out): level 0..1 moves by dt / 0.5 s towards the target; each bar is
// `height` (Camera_FX_Manager_Letterbox_Height, vanilla 0.085) x level of the screen height.
class Letterbox {
public:
    static constexpr float kSlideSeconds = 0.5f;
    void slide_in() { target_ = 1.0f; }
    void slide_out() { target_ = 0.0f; }  // scaled by how far it is in (same rate)
    void remove() { level_ = target_ = 0.0f; }
    void update(float dt_seconds);
    float level() const { return level_; }
    bool visible() const { return level_ > 0.0f; }

private:
    float level_ = 0.0f, target_ = 0.0f;
};

}  // namespace alamo::game

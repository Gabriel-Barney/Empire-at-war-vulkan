// L1 systems presentation (refix t06): the tactical super weapon's sound / camera cues and a presentation-only camera
// shake. Spec: docs/re/TSW_PLANET_DESTROY.md §3 (state table) and §4 steps 4-5, docs/re/TACTICAL_GAPS.md §7.1.
//
//   tsw_cues        turns one battle tick's TacticalBattle::special_weapon_events() into cues the hosts play:
//                     Firing          fire accepted (state 5): the TSW type's TSW_SFXEvent_Activate_Voice,
//                                     TSW_SFXEvent_Weapon_Power_Up and music TSW_MusicEvent_Activation_Start (mode 8)
//                     AttackAnim      state 7: the TSW type's SFXEvent_Fire
//                     PlanetDestroyed state 9 with a live target dummy: camera shake (0x2cdbb0) at the dummy and its
//                                     type's Death_SFXEvent_Start_Die; the sim has killed the dummy (silently) and spawned
//                                     the Death_Explosions pick in the same tick.
//                   Read right after the battle tick: the dummy killed at state 9 is still in the store (dead, not
//                   flushed until the next tick's store update), so its type is readable. No dummy (§4 step 3) = no cue.
//   CameraShake     [port] wall-clock camera offset; the original routine 0x2cdbb0 is not specified (no parameters in the
//                   docs), so the shape and the TSW constants below are the port's. Applied to the rendered camera only,
//                   never to the camera controller, picking or anything the sim reads.
//
// Presentation only: const reads of the battle between ticks, no sim writes, no synced draws (the CRC is the same with
// the cues on or off).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/objects/object_store.hpp"
#include "alamo/tactical/geometry.hpp"

namespace alamo::tactical::abilities {
struct SpecialWeaponEvent;
}

namespace alamo::game {

struct TswCue {
    enum class Kind : uint8_t { Firing, AttackAnim, PlanetDestroyed };
    Kind kind = Kind::Firing;
    objects::ObjectId object = objects::kNoObjectId;  // the TSW (Firing / AttackAnim) or the target dummy (PlanetDestroyed)
    objects::TypeIndex type = objects::kNone;         // that object's type
    tactical::Vec3 position;                          // the object's position (PlanetDestroyed: the dummy's point)
};

// In event order. Events whose object is not in `store` (flushed / unknown) give no cue.
std::vector<TswCue> tsw_cues(std::span<const tactical::abilities::SpecialWeaponEvent> events, const objects::ObjectStore& store);

struct CameraShakeParams {
    float duration_s = 2.0f;     // linear-squared fade to 0
    float magnitude = 0.015f;    // peak offset as a fraction of the camera's eye-target distance
    float frequency_hz = 8.0f;   // base wobble frequency
    float radius = 0.0f;         // > 0: strength falls linearly to 0 at this distance of the camera target from the source
};
// [port] the TSW planet kill: a map-wide jolt (the target dummy is a planet backdrop thousands of units away).
inline constexpr CameraShakeParams kTswPlanetShake{3.0f, 0.02f, 7.0f, 0.0f};

class CameraShake {
public:
    static constexpr size_t kMaxShakes = 8;  // the oldest is dropped when a ninth starts
    void start(const tactical::Vec3& source, const CameraShakeParams& p);
    void update(double dt_seconds);  // advances every shake, drops finished ones
    void reset() { shakes_.clear(); }
    bool active() const { return !shakes_.empty(); }
    size_t count() const { return shakes_.size(); }
    // The offset to add to both the eye and the target of a camera at `eye` looking at `target` (sum of the shakes;
    // zero when none is active).
    tactical::Vec3 offset(const tactical::Vec3& eye, const tactical::Vec3& target) const;

private:
    struct Shake {
        tactical::Vec3 source;
        CameraShakeParams p;
        double t = 0.0;
    };
    std::vector<Shake> shakes_;  // start order
};

}  // namespace alamo::game

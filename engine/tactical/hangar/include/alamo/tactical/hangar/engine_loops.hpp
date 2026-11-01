// Engine loop sound requests, tactical side (docs/re/STATION_HANGARS_SOUND.md §3).
//
// Set_Engine_Idle_Loop / Set_Engine_Moving_Loop (0x3a7cf0 / 0x3a7e30): per-object requested-state flags (moving
// +0x3B5, idle +0x3B6). A call with on = 1 while the flag is 0 emits a Start request and sets the flag; on = 0 while
// it is 1 emits a Stop request (fade-out = the event's Loop_Fade_Out_Seconds) and clears it; anything else does
// nothing. The flag changes even when the type has no event (fighters, squadrons) or the audio side refuses the start.
// Dead / removed objects and objects with engine audio suppressed (+0x3B9) do nothing.
//
// The flags are sim-side object state that only gate the requests: they never feed back into sim logic and are not
// part of any CRC. The audio side (engine/audio) turns requests into SFX instances: Start = start the type's
// SFXEvent_Engine_Idle_Loop / _Moving_Loop attached to the object with its fade-in; Stop = stop with fade-out
// (mark fading, excluded from instance counts, AUDIO §1.5); object death / removal kills instances without fade and
// is not routed through here (remove()).
//
// HOST WIRING (engine/apps/sim TacticalBattle), all serial in the tick:
//   * mode_enter(id)        - the object enters the tactical mode: creation in battle, reinforcement spawn, the start
//                             of hyperspace-in, map load. Idle on, moving off.
//   * locomotor_service(id, speed, in_hyperspace) - every SimpleSpace locomotor service (corvettes, frigates,
//                             capitals, transports; NOT fighters, squadrons or other locomotors) after the motion
//                             step: moving := speed >= 1.1 x SpaceIdleMovementSpeed, then idle := speed below it.
//                             In a hyperspace state (arrival / departure / scripted) nothing changes.
//   * engines_offline(id)   - last ENGINE hardpoint destroyed / station-upgrade disable (AbilityEventKind::
//                             EnginesOffline): moving off, idle off.
//   * engines_online(id)    - ENGINE hardpoint repaired to full (EnginesOnline): idle on.
//   * timed_disable(id)     - timed engine disable from Take_Damage (energy-pool projectile): moving off, idle off.
//                             The timer expiry does not call audio.
//   * suppress(id) / unsuppress(id) - 0x392410 / 0x393970 (land only in vanilla): both off then blocked / unblocked.
//   * remove(id)            - death or removal: forget the object, no request.
//   * after the tick: requests() to the audio side, then clear_requests().
#pragma once
#include <algorithm>
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/tactical/combat/combat_system.hpp"

namespace alamo {
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::tactical::hangar {

enum class EngineLoop : uint8_t { Idle, Moving };

struct EngineLoopRequest {
    combat::ObjectId id = combat::kNoObject;
    EngineLoop loop = EngineLoop::Idle;
    bool start = false;  // true: start with fade-in; false: stop with fade-out
    uint32_t frame = 0;
};

class EngineLoops {
public:
    // `space_idle_movement_speed` = GameConstants SpaceIdleMovementSpeed, in the units of the speed passed to
    // locomotor_service.
    explicit EngineLoops(float space_idle_movement_speed = 0.0f) : threshold_(1.1f * space_idle_movement_speed) {}

    void set_space_idle_movement_speed(float v) { threshold_ = 1.1f * v; }
    float moving_threshold() const { return threshold_; }

    void mode_enter(combat::ObjectId id, uint32_t frame) {
        Obj& o = get(id);
        set(o, EngineLoop::Idle, true, frame);
        set(o, EngineLoop::Moving, false, frame);
    }
    void locomotor_service(combat::ObjectId id, float speed, bool in_hyperspace, uint32_t frame) {
        if (in_hyperspace) return;
        Obj& o = get(id);
        const bool moving = speed >= threshold_;
        set(o, EngineLoop::Moving, moving, frame);  // moving call first
        set(o, EngineLoop::Idle, !moving, frame);
    }
    void engines_offline(combat::ObjectId id, uint32_t frame) { both_off(id, frame); }
    void timed_disable(combat::ObjectId id, uint32_t frame) { both_off(id, frame); }
    void engines_online(combat::ObjectId id, uint32_t frame) { set(get(id), EngineLoop::Idle, true, frame); }
    void suppress(combat::ObjectId id, uint32_t frame) {
        Obj& o = get(id);
        if (o.moving) push(o.id, EngineLoop::Moving, false, frame);
        if (o.idle) push(o.id, EngineLoop::Idle, false, frame);
        o.moving = o.idle = 0;
        o.suppressed = 1;
    }
    void unsuppress(combat::ObjectId id) { get(id).suppressed = 0; }
    void remove(combat::ObjectId id) {
        const auto it = lower(id);
        if (it != objs_.end() && it->id == id) objs_.erase(it);
    }

    bool idle_on(combat::ObjectId id) const {
        const Obj* o = find(id);
        return o && o->idle;
    }
    bool moving_on(combat::ObjectId id) const {
        const Obj* o = find(id);
        return o && o->moving;
    }
    std::span<const EngineLoopRequest> requests() const { return requests_; }
    void clear_requests() { requests_.clear(); }
    // N1 stage 2 (save_file.hpp contract, hangar_save.cpp): the per-object flags (ascending id) and the pending
    // requests, so a loaded battle restarts no loop twice. The threshold is configuration (not saved).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Obj {
        combat::ObjectId id = combat::kNoObject;
        uint8_t idle = 0, moving = 0, suppressed = 0;
    };
    std::vector<Obj>::iterator lower(combat::ObjectId id) {
        return std::lower_bound(objs_.begin(), objs_.end(), id, [](const Obj& a, combat::ObjectId v) { return a.id < v; });
    }
    const Obj* find(combat::ObjectId id) const {
        const auto it =
            std::lower_bound(objs_.begin(), objs_.end(), id, [](const Obj& a, combat::ObjectId v) { return a.id < v; });
        return (it != objs_.end() && it->id == id) ? &*it : nullptr;
    }
    Obj& get(combat::ObjectId id) {
        auto it = lower(id);
        if (it == objs_.end() || it->id != id) {
            Obj o;
            o.id = id;
            it = objs_.insert(it, o);
        }
        return *it;
    }
    void push(combat::ObjectId id, EngineLoop l, bool start, uint32_t frame) {
        requests_.push_back(EngineLoopRequest{id, l, start, frame});
    }
    void set(Obj& o, EngineLoop l, bool on, uint32_t frame) {
        if (o.suppressed) return;
        uint8_t& flag = l == EngineLoop::Idle ? o.idle : o.moving;
        if (on && !flag) {
            flag = 1;
            push(o.id, l, true, frame);
        } else if (!on && flag) {
            flag = 0;
            push(o.id, l, false, frame);
        }
    }
    void both_off(combat::ObjectId id, uint32_t frame) {
        Obj& o = get(id);
        set(o, EngineLoop::Moving, false, frame);
        set(o, EngineLoop::Idle, false, frame);
    }

    float threshold_;
    std::vector<Obj> objs_;  // sorted by id
    std::vector<EngineLoopRequest> requests_;
};

}  // namespace alamo::tactical::hangar

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
//   * walk_service(id, moving) - cu8g t01: every land Walk locomotor service (0x61e930, §3.3 last row), the same switch
//                             (moving call first) on the host's moving test [port: §3.3 leaves the Walk test untraced [M];
//                             the host passes "order in progress or speed > 0"]. Bike / LandTeamInfantry / JetPack / the
//                             tactical super weapon are not wired.
//   * engines_offline(id)   - last ENGINE hardpoint destroyed / station-upgrade disable (AbilityEventKind::
//                             EnginesOffline): moving off, idle off.
//   * engines_online(id)    - ENGINE hardpoint repaired to full (EnginesOnline): idle on.
//   * timed_disable(id)     - a timed engine disable (§3.3: Take_Damage, energy-pool projectile): moving off, idle off.
//                             The timer expiry does not call audio. Called by TacticalBattle::disable_engines (the L1
//                             timed-disable entry point) and TacticalBattle::energy_drain_engines (cu8f t02: the energy-pool
//                             absorb 0x56c230 in Take_Damage, CombatHooks::engines_drained) for engine-loop units.
//   * suppress(id, fade) / unsuppress(id) - 0x392410 / 0x393970: suppress clears both flags, blocks the switch (+0x3B9)
//                             and emits one Suppress request carrying `fade` (seconds; < 0 = each loop's own
//                             Loop_Fade_Out_Seconds): the audio side stops the moving, idle AND cinematic-focus loops whatever
//                             the flags say (GameAudio::suppress_engine_audio). unsuppress unblocks and emits an Unsuppress
//                             request (GameAudio::clear_engine_audio_suppression); it restarts nothing - the next locomotor
//                             service or mode enter does. Their only callers are the HOLSTER_WEAPON behaviour below and
//                             Saber_Throw (CU8F_T01_T02_T07_RE.md §2.2 / §2.3), wired since cu8g t01.
//   * remove(id)            - death or removal: forget the object (and its holster state), no request.
//   * after the tick: requests() to the audio side, then clear_requests().
//
// HOLSTER_WEAPON (behaviour 0x4E, HolsterWeaponBehaviorClass; CU8F_T01_T02_T07_RE.md §2.3), land lightsaber heroes. Sim
// state only (the bone motion is presentation); it decides the engine-loop (lightsaber hum) suppression and the draw /
// holster SFX requests. Like the loop flags it never feeds back into sim logic and is not part of any CRC.
//   * holster_attach(id)    - initialise (0x6355c0) / the map-load "placed" broadcast (0x635180), for an object whose
//                             behaviour found its bone [port: the host's test]: drawn := 0, transition cleared,
//                             suppress(fade 0) whatever Holster_Disable_Engine_Loops says. Call BEFORE mode_enter: the hero
//                             starts suppressed, so the mode-enter idle-on is skipped and no hum plays until the saber is drawn.
//   * holster_service(id, desc, engaged, saber_in_flight) - every frame (Service 0x635750, interval 1): first the
//                             interpolation step (0x635950: while a transition is set and no thrown saber is in flight; at
//                             now >= end the transition clears and, when drawn and Holster_Disable_Engine_Loops,
//                             unsuppress). Then on frame % 15 == 0 only: engaged -> last-engaged := now, want drawn; else
//                             want = drawn && now < last-engaged + Holster_Minimum_Drawn_Time frames. want != drawn starts
//                             a transition [now, now + Holster_Transition_Time frames]; to drawn: DrawWeapon request,
//                             drawn := 1; to holstered: HolsterWeapon request, drawn := 0 and, when
//                             Holster_Disable_Engine_Loops, suppress(-1) (each loop's own fade-out).
//   * saber_throw(id, frame) / saber_catch(id) - Saber_Throw's throw (0x635450) / catch (0x635310): drawn := 0 / 1,
//                             both suppress(fade 0) (the catch too: the hum stays off until a later draw transition completes).
//   * saber_schedule(id, throw, catch, end) - cu8j t03 (CU8F_T01_T02_T07_RE §2.7 item 4, the jitter-free stand-in): the
//                             thrown saber's frames (absolute; catch 0 = no catch). saber_service runs them around the
//                             host's holster_service each frame: before it, the throw (snap holstered) at the throw frame;
//                             the saber is in flight (SAP +0x2e8 set) from the throw up to and including the end frame (the
//                             ability's end, not the catch); after it, the catch snap (drawn) on the catch frame only when
//                             there is a catch, and the end clears the flight. So the held draw transition clears and the
//                             hum is unsuppressed the frame after the end (retail §2.6: "catch" = the end).
//   Frame counts: holster_frames(s, fps) = round-half-away(fps x s) (0x27c9a0 -> 0x22f9c0): 0.17 s -> 5, 15 s -> 450 at 30.
//   Saved with the flags (save version 4; 2 and 3 are still read): attached, drawn, transition start / end frames, last
//   engaged frame, the thrown saber's throw / catch / end frames (version 4, cu8j t03; version 3 had the cu8i catch frame).
#pragma once
#include <algorithm>
#include <cmath>
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

// What a request asks the audio side for. Loop: start / stop `loop` (the original engine-loop requests); Suppress /
// Unsuppress: 0x392410 / 0x393970; DrawWeapon / HolsterWeapon: play the type's SFXEvent_Draw_Weapon /
// SFXEvent_Holster_Weapon attached to the object (HOLSTER_WEAPON transition start).
enum class EngineLoopOp : uint8_t { Loop, Suppress, Unsuppress, DrawWeapon, HolsterWeapon };

struct EngineLoopRequest {
    combat::ObjectId id = combat::kNoObject;
    EngineLoop loop = EngineLoop::Idle;  // Loop only
    bool start = false;                  // Loop only: true: start with fade-in; false: stop with fade-out
    uint32_t frame = 0;
    EngineLoopOp op = EngineLoopOp::Loop;
    float fade = -1.0f;  // Suppress only: stop fade in seconds; < 0 = each loop's own Loop_Fade_Out_Seconds
};

// HOLSTER_WEAPON type data (HeroUnit fields, xml_field_tables), in frames. Configuration: the host passes it on each
// service; not saved.
struct HolsterDesc {
    uint32_t transition_frames = 0;     // holster_frames(Holster_Transition_Time_In_Secs)
    uint32_t min_drawn_frames = 0;      // holster_frames(Holster_Minimum_Drawn_Time_In_Secs)
    bool disable_engine_loops = false;  // Holster_Disable_Engine_Loops (+0xe2)
};

// frames(s) = round-half-away(fps x s) (0x27c9a0 -> 0x22f9c0); negative / NaN -> 0.
inline uint32_t holster_frames(float seconds, uint32_t fps) {
    const float f = static_cast<float>(fps) * seconds;
    return f > 0.0f ? static_cast<uint32_t>(std::lround(f)) : 0u;
}

inline constexpr uint32_t kHolsterDecisionInterval = 15;  // the draw / holster decision runs on frame % 15 == 0

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
    void walk_service(combat::ObjectId id, bool moving, uint32_t frame) {
        Obj& o = get(id);
        set(o, EngineLoop::Moving, moving, frame);  // moving call first
        set(o, EngineLoop::Idle, !moving, frame);
    }
    void engines_offline(combat::ObjectId id, uint32_t frame) { both_off(id, frame); }
    void timed_disable(combat::ObjectId id, uint32_t frame) { both_off(id, frame); }
    void engines_online(combat::ObjectId id, uint32_t frame) { set(get(id), EngineLoop::Idle, true, frame); }
    void suppress(combat::ObjectId id, uint32_t frame, float fade = -1.0f) { suppress(get(id), frame, fade); }
    void unsuppress(combat::ObjectId id, uint32_t frame) { unsuppress(get(id), frame); }
    void remove(combat::ObjectId id) {
        const auto it = lower(id);
        if (it != objs_.end() && it->id == id) objs_.erase(it);
    }

    // ---- HOLSTER_WEAPON (see the header comment) ----
    void holster_attach(combat::ObjectId id, uint32_t frame) {
        Obj& o = get(id);
        o.holster = 1;
        o.drawn = 0;  // bone snapped to the holstered translation
        o.t_start = o.t_end = 0;
        suppress(o, frame, 0.0f);  // not gated by Holster_Disable_Engine_Loops
    }
    void holster_service(combat::ObjectId id, const HolsterDesc& d, bool engaged, bool saber_in_flight, uint32_t frame) {
        Obj* o = find_mut(id);
        if (!o || !o->holster) return;
        // Interpolation step (0x635950): the bone lerp is presentation; the end clears the transition.
        if ((o->t_start != 0 || o->t_end != 0) && !saber_in_flight && frame >= o->t_end) {
            o->t_start = o->t_end = 0;
            if (o->drawn && d.disable_engine_loops) unsuppress(*o, frame);
        }
        if (frame % kHolsterDecisionInterval != 0) return;
        bool want = false;
        if (engaged) {
            o->last_engaged = frame;
            want = true;
        } else {
            want = o->drawn != 0 && frame < o->last_engaged + d.min_drawn_frames;
        }
        if (want == (o->drawn != 0)) return;
        o->t_start = frame;
        o->t_end = frame + d.transition_frames;
        if (want) {
            push_op(o->id, EngineLoopOp::DrawWeapon, frame);
            o->drawn = 1;
        } else {
            push_op(o->id, EngineLoopOp::HolsterWeapon, frame);
            o->drawn = 0;
            if (d.disable_engine_loops) suppress(*o, frame, -1.0f);
        }
    }
    void saber_throw(combat::ObjectId id, uint32_t frame) { (void)snap(id, false, frame); }  // 0x635450
    void saber_catch(combat::ObjectId id, uint32_t frame) { (void)snap(id, true, frame); }   // 0x635310
    // cu8j t03: schedule a thrown saber (absolute frames; catch_frame 0 = it dies on its way back, no catch; end_frame >=
    // throw_frame). Only for an object with a holster (the snaps need it). A new schedule replaces a pending one.
    void saber_schedule(combat::ObjectId id, uint32_t throw_frame, uint32_t catch_frame, uint32_t end_frame) {
        Obj* o = find_mut(id);
        if (!o || !o->holster) return;
        o->throw_frame = std::max(1u, throw_frame);
        o->catch_frame = catch_frame;
        o->end_frame = std::max(o->throw_frame, end_frame);
    }
    // The host's per-frame saber steps around holster_service(id, ..., saber_in_flight(id), frame): before = true runs a
    // due throw; before = false runs the catch snap on the catch frame and the end on the end frame.
    void saber_service(combat::ObjectId id, uint32_t frame, bool before) {
        Obj* o = find_mut(id);
        if (!o || o->end_frame == 0) return;
        if (before) {
            if (o->throw_frame != 0 && frame >= o->throw_frame) {
                o->throw_frame = 0;
                saber_throw(id, frame);
            }
            return;
        }
        if (o->throw_frame != 0) return;
        if (o->catch_frame != 0 && frame >= o->catch_frame) {
            o->catch_frame = 0;
            saber_catch(id, frame);
        }
        if (frame >= o->end_frame) o->end_frame = 0;
    }
    uint32_t saber_throw_frame(combat::ObjectId id) const {  // 0 = none pending
        const Obj* o = find(id);
        return o ? o->throw_frame : 0u;
    }
    uint32_t saber_catch_frame(combat::ObjectId id) const {  // 0 = no catch pending
        const Obj* o = find(id);
        return o ? o->catch_frame : 0u;
    }
    uint32_t saber_end_frame(combat::ObjectId id) const {  // 0 = no saber thrown or scheduled
        const Obj* o = find(id);
        return o ? o->end_frame : 0u;
    }
    // Thrown (SAP +0x2e8 set): from the throw up to and including the end frame.
    bool saber_in_flight(combat::ObjectId id) const {
        const Obj* o = find(id);
        return o && o->end_frame != 0 && o->throw_frame == 0;
    }

    bool has_holster(combat::ObjectId id) const {
        const Obj* o = find(id);
        return o && o->holster;
    }
    bool holster_drawn(combat::ObjectId id) const {
        const Obj* o = find(id);
        return o && o->drawn;
    }
    bool holster_transitioning(combat::ObjectId id) const {
        const Obj* o = find(id);
        return o && (o->t_start != 0 || o->t_end != 0);
    }
    uint32_t holster_last_engaged(combat::ObjectId id) const {
        const Obj* o = find(id);
        return o ? o->last_engaged : 0u;
    }
    bool suppressed(combat::ObjectId id) const {
        const Obj* o = find(id);
        return o && o->suppressed;
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
    // N1 stage 2 (save_file.hpp contract, hangar_save.cpp): the per-object flags and holster state (ascending id) and the
    // pending requests, so a loaded battle restarts no loop twice. The threshold is configuration (not saved).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Obj {
        combat::ObjectId id = combat::kNoObject;
        uint8_t idle = 0, moving = 0, suppressed = 0;
        // HOLSTER_WEAPON (behaviour +0x40 bone found, +0x50 drawn, +0x44 / +0x48 transition, +0x4c last engaged frame)
        uint8_t holster = 0, drawn = 0;
        uint32_t t_start = 0, t_end = 0, last_engaged = 0;
        // cu8j t03: Saber_Throw's pending throw frame (0 = thrown or none), catch frame (0 = none pending) and end frame
        // (0 = no saber).
        uint32_t throw_frame = 0, catch_frame = 0, end_frame = 0;
    };
    Obj* find_mut(combat::ObjectId id) {
        const auto it = lower(id);
        return (it != objs_.end() && it->id == id) ? &*it : nullptr;
    }
    void suppress(Obj& o, uint32_t frame, float fade) {  // 0x392410
        EngineLoopRequest q;
        q.id = o.id;
        q.frame = frame;
        q.op = EngineLoopOp::Suppress;
        q.fade = fade;
        requests_.push_back(q);
        o.moving = o.idle = 0;
        o.suppressed = 1;
    }
    void unsuppress(Obj& o, uint32_t frame) {  // 0x393970: restarts nothing
        push_op(o.id, EngineLoopOp::Unsuppress, frame);
        o.suppressed = 0;
    }
    Obj* snap(combat::ObjectId id, bool drawn, uint32_t frame) {
        Obj* o = find_mut(id);
        if (!o || !o->holster) return nullptr;
        o->drawn = drawn ? 1 : 0;  // bone to the drawn / holstered translation
        suppress(*o, frame, 0.0f);
        return o;
    }
    void push_op(combat::ObjectId id, EngineLoopOp op, uint32_t frame) {
        EngineLoopRequest q;
        q.id = id;
        q.frame = frame;
        q.op = op;
        requests_.push_back(q);
    }
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

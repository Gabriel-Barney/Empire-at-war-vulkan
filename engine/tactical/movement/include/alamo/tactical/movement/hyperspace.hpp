// L1 systems - hyperspace departures and the locomotor engines flag (docs/re/L1_S4_REMAINING.md §3.4, docs/re/SCRIPT_GAPS.md
// §2.2 / §3.2; the MOVEMENT.md §2.6 "Departure" figures do not apply, L1_S4_REMAINING §3.4 corrections).
//
// HyperspaceDepartures: locomotor vf +0x2a8(keep_parent_link, delay) (0x5caf40, FIGHTER / SIMPLE_SPACE locomotors;
// STARSHIP_LOCOMOTOR has an empty stub - the host checks that) [H]:
//   * at once: the locomotor stops (the host takes the unit out of path following), state 0x28 "Fly Hyperspace Away",
//     timer := delay + 90; keep_parent_link false clears the parent-mode id (the galactic unit survives);
//   * each frame (0x5cd680): a teleport step along the yaw - 8.125 while timer > 90, else the owner faction's
//     Space_Retreat_Flight_Move_Increment; at timer == 90 the jump SFX + model stretch (Jumped event); at 0 the object
//     is killed (0x391310, flag 1: Killed event). Total delay + 90 frames, NO RNG.
//   [M] order inside one frame: step (by the timer before the decrement), Jumped at 90, decrement, Killed at 0.
// Serviced serially in ascending id.
//
// EngineControl: Locomotor_Set_Engines (0x5545a0) [H]: engine hard point destroyed -> engines off for good
// (+0xd0 = 0, +0xd1 = 1); disable for t seconds -> off with re-enable frame +0xd4 = max(+0xd4, now + int(30 t));
// t < 0 -> off indefinitely. Both act only when (online or a timed disable is pending) and not permanent
// (HARDPOINT_DISABLED.md §9.1): so destroying the last engine hard point while the engines are already off
// indefinitely does NOT set permanent. repair_online = the hardpoint-repair online path 0x393980 (clears permanent);
// enable = the script path (on unless destroyed). online() = Are_Engines_Online (+0xd0); while off, the effective
// max speed takes x Engines_Disabled_Speed_Modifier live (MOVEMENT §3.4 item 5). Timed disables end in tick()
// (ascending id; EngineEvent::On = object signal 0x17, Off = 0x16).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/tactical/geometry.hpp"
#include "alamo/tactical/movement/common.hpp"

namespace alamo {
class StateHasher;
}

namespace alamo::tactical::movement {

inline constexpr int32_t kHyperspaceJumpTimer = 90;          // timer value of the jump (SFX / stretch)
inline constexpr float kHyperspacePreJumpStep = 8.125f;       // per frame while timer > 90

struct HyperspaceEvent {
    enum class Kind : uint8_t { Jumped, Killed };
    Kind kind = Kind::Jumped;
    ObjectId id = 0;
    bool keep_parent_link = true;  // Killed: false = the galactic counterpart survives (parent-mode id cleared)
    uint32_t frame = 0;
};

class HyperspaceDepartures {
public:
    // vf +0x2a8 at `frame`: the unit at `position` with XY `facing` (yaw) leaves after delay + 90 frames;
    // `flight_increment` = the owner faction's Space_Retreat_Flight_Move_Increment. A unit already departing keeps its
    // first departure (false).
    bool schedule(ObjectId id, uint32_t delay, bool keep_parent_link, Vec3 position, Vec3 facing, float flight_increment);
    void remove(ObjectId id);  // host removal (death)
    bool departing(ObjectId id) const { return find(id) >= 0; }
    int32_t timer(ObjectId id) const;  // -1 when not departing
    // One frame: steps, Jumped / Killed events (ascending id); killed units leave the list.
    void tick(uint32_t frame);
    Vec3 position(ObjectId id) const;
    Vec3 facing(ObjectId id) const;
    std::span<const HyperspaceEvent> events() const { return events_; }
    bool empty() const { return jumps_.empty(); }
    void hash(StateHasher& h) const;
    // N1 stage 2 save / load (contract: alamo/core/save_file.hpp; src/movement_save.cpp): u32 version, the departures
    // (ascending id, every field) and this frame's events. The battle host owns the instance and saves it with the
    // movement chunk (or its own); nothing to re-link. False (unchanged) on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Jump {
        ObjectId id = 0;
        int32_t timer = 0;
        uint8_t keep = 1;
        float increment = 0.0f;
        Vec3 pos, dir{1.0f, 0.0f, 0.0f};
    };
    int32_t find(ObjectId id) const;

    std::vector<Jump> jumps_;  // ascending id
    std::vector<HyperspaceEvent> events_;
};
struct EngineEvent {
    enum class Kind : uint8_t { Off, On };
    Kind kind = Kind::Off;
    ObjectId id = 0;
    uint32_t frame = 0;
};

class EngineControl {
public:
    // Engines destroyed (last ENGINE hard point): off for good.
    void destroy(ObjectId id, uint32_t frame);
    // Disable for `seconds` (< 0: indefinitely) from `frame`.
    void disable(ObjectId id, float seconds, uint32_t frame);
    // Enable (script): on unless destroyed.
    void enable(ObjectId id, uint32_t frame);
    // Engine hardpoint repaired to max (0x393980 -> 0x554640): when the engines are off, online := 1, permanent := 0,
    // timer := -1 and EngineEvent::On (signal 0x17); when already on (or unknown: no locomotor entry), nothing.
    // TURBO / SPOILER_LOCK are not restored. True when it switched them on.
    bool repair_online(ObjectId id, uint32_t frame);
    bool online(ObjectId id) const;     // unknown ids: online
    bool destroyed(ObjectId id) const;
    uint32_t reenable_frame(ObjectId id) const;  // 0 when not timed
    void remove(ObjectId id);
    // Timed disables whose frame came (frame >= re-enable): back on (EngineEvent::On), ascending id.
    void tick(uint32_t frame);
    std::span<const EngineEvent> events() const { return events_; }
    void clear_events() { events_.clear(); }
    bool empty() const { return list_.empty(); }
    void hash(StateHasher& h) const;
    // N1 stage 2 save / load (src/movement_save.cpp): u32 version, the entries (ascending id) and the pending events.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Entry {
        ObjectId id = 0;
        uint8_t online = 1;      // +0xd0
        uint8_t destroyed = 0;   // +0xd1
        uint8_t indefinite = 0;  // t < 0
        uint32_t reenable = 0;   // +0xd4
    };
    Entry& entry(ObjectId id);
    const Entry* find(ObjectId id) const;
    std::vector<Entry> list_;  // ascending id
    std::vector<EngineEvent> events_;
};

}  // namespace alamo::tactical::movement

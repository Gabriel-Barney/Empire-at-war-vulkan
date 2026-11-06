// S4 - structure capture: CAPTURE_POINT objects (reinforcement points, build pads, abandoned turrets, community
// structures) change owner when one side controls them; the source of STORY_CAPTURE_STRUCTURE. Spec:
// docs/re/L1_S4_REMAINING.md §4.2 (CapturePointBehaviorClass service 0x501980, capture complete 0x500e30) [H unless
// tagged]:
//   * service every `interval` frames per point (CAPTURE_POINT behaviour interval 4; first service at once);
//   * start gate (REMAINING_GAPS §3.3, campaign only; the host passes 0 for skirmish): frame >= allowed_frame
//     (int(30 x Land_ / Space_Capture_Allowed_Countdown_Seconds + 0.5)), or the owner is none / the neutral player / a
//     player of an Is_Neutral faction (set_neutral_owner);
//   * controller: (b) a garrison host with occupants -> the first occupant's owner (occupant_owner hook); (c) else the
//     influencers within the 3D Capture_Point_Radius (host-filtered: Influences_Capture_Point, alive, not garrisoned,
//     owned) excluding the point: neutral-owned point -> the first one's owner, any later one not allied with it ->
//     neutral (contested); owned point -> any one not allied with the owner -> neutral, else the owner; (d) none -> the
//     owner when the point is sticky (REINFORCEMENT_POINT or Ownership_Sticks), else neutral (decay). (a) build pads
//     with contents (UC or child) -> the current owner, before (b) (q6 t19: set_pad_occupied);
//   * progress 0..1: step = 1 / (T x 30) x interval (T = Capture_Point_Transition_Time_Seconds, T <= 0 -> 1; the
//     controller's Capture_Point_Time_Multiplier m = 1 [port: no battlefield modifiers]); controller == owner ->
//     progress -= step (min 0; at 0 pending := owner); else pending := controller, progress += step (max 1); at 1 the
//     capture completes (owner := pending, progress := 0, CaptureEvent old -> new). An enemy-held point goes owner ->
//     neutral -> capturer in two transitions.
// Determinism: serial, points in id order, influencers in the host's (ascending id) order, float steps in a fixed order.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "alamo/core/galaxy.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/garrison/garrison.hpp"

namespace alamo::tactical::garrison {

struct CapturePointDesc {
    float radius = 0.0f;              // Capture_Point_Radius (3D)
    float transition_seconds = 0.0f;  // Capture_Point_Transition_Time_Seconds
    bool sticky = false;              // REINFORCEMENT_POINT behaviour or Ownership_Sticks
};

struct Influencer {
    ObjectId id = kNoObject;
    int32_t owner = -1;
    Vec3 position{};
};

struct CaptureEvent {
    ObjectId point = kNoObject;
    int32_t old_owner = -1, new_owner = -1;
    uint32_t frame = 0;
};

class CapturePointSystem {
public:
    CapturePointSystem(int32_t neutral_player, uint32_t interval = 4, uint32_t allowed_frame = 0)
        : neutral_(neutral_player), interval_(interval ? interval : 1), allowed_frame_(allowed_frame) {}

    void set_allied(std::function<bool(int32_t a, int32_t b)> fn) { allied_ = std::move(fn); }
    // Rule (b): the owner of a garrison host's first occupant, -1 when the point is not a host with occupants.
    void set_occupant_owner(std::function<int32_t(ObjectId point)> fn) { occupant_owner_ = std::move(fn); }
    // REMAINING_GAPS §3.3 start gate: points owned by an Is_Neutral faction's player (faction +0x108) are serviced at once,
    // like unowned / neutral-player points. Unset: only the neutral player counts.
    void set_neutral_owner(std::function<bool(int32_t owner)> fn) { neutral_owner_ = std::move(fn); }
    // q6 t19 rule (a) (AI_GALACTIC_TOKENS §9.11, AI_TACTICAL §7.5.1 item 1): a build pad whose pad data holds an
    // under-construction object or a child keeps its current owner, whoever stands on it. Unset: no pads.
    void set_pad_occupied(std::function<bool(ObjectId point)> fn) { pad_occupied_ = std::move(fn); }

    bool add_point(ObjectId id, int32_t owner, Vec3 position, const CapturePointDesc& d, uint32_t first_frame = 0);
    void remove(ObjectId id);
    void set_owner(ObjectId id, int32_t owner);  // external owner changes (Change_Owner): pending / progress cleared
    // REMAINING_GAPS §6.5 capture reset 0x501050(point, owner): owner := `owner`, the capture record's controller (+8) :=
    // `owner`, progress cleared. No event (the host refreshes the store owner).
    void reset(ObjectId id, int32_t owner);
    void set_position(ObjectId id, Vec3 p);
    bool is_point(ObjectId id) const { return index(id) >= 0; }
    int32_t owner(ObjectId id) const;
    int32_t pending(ObjectId id) const;  // -1 none
    float progress(ObjectId id) const;
    size_t point_count() const { return points_.size(); }
    ObjectId point_at(size_t i) const { return points_[i].id; }

    void service(uint32_t frame, std::span<const Influencer> influencers);

    std::span<const CaptureEvent> events() const { return events_; }
    void clear_events() { events_.clear(); }
    void hash(StateHasher& h) const;
    // N1 stage 2 (save_file.hpp contract, garrison_save.cpp): the constructor parameters (neutral player, interval,
    // allowed frame - restored), every point (owner, position, descriptor, step, pending, progress, next service) and
    // the pending events. Not saved: the callbacks (the host's). False (unchanged) on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Point {
        ObjectId id = kNoObject;
        int32_t owner = -1;
        Vec3 pos{};
        CapturePointDesc d;
        float step = 1.0f;
        int32_t pending = -1;
        float progress = 0.0f;
        uint32_t next = 0;
    };
    int32_t index(ObjectId id) const;
    bool allied(int32_t a, int32_t b) const { return a == b || (allied_ && allied_(a, b)); }
    int32_t controller(const Point& p, std::span<const Influencer> inf) const;

    int32_t neutral_;
    uint32_t interval_;
    uint32_t allowed_frame_;
    std::function<bool(int32_t, int32_t)> allied_;
    std::function<int32_t(ObjectId)> occupant_owner_;
    std::function<bool(int32_t)> neutral_owner_;
    std::function<bool(ObjectId)> pad_occupied_;
    std::vector<Point> points_;  // ascending id
    std::vector<CaptureEvent> events_;
};

}  // namespace alamo::tactical::garrison

// Space navigation (docs/re/MOVEMENT.md §2).
//
// SpaceOccupancy - the object-tracking system (§2.2): static entries (SpaceObstacleBehavior
//   circles: asteroids, stations, the layer-8 static map) and per-ship tracks (the planned
//   trajectory, timestamped in frames) per SpaceLayer. Queries OR the SpaceCollisionType bits of
//   every hit, so the result does not depend on traversal order. The retail ring of 45 x 90-frame
//   time slices is an acceleration structure only (a query ORs the same entries); this port tests
//   tracks directly over the look-ahead window [now, now + count * interval] and keeps the ring
//   constants for that window. Tracks are linear between waypoints; before the first / after the
//   last waypoint the ship is stationary there (bit Static). A ship plans only around tracks of
//   its own layer plus every static entry (§2.2 "Layers").
//
// plan_space_path - kinematic time-parameterised A* (§2.1) with the retry policy of §2.4:
//   state lattice (x cell, y cell, heading bucket); cell = floor((p - start) * 1.5 / D), bucket =
//   floor(heading * (N + 1) / 360), N = MaxRotationsSpace / rotation divisor (24 -> 15 deg turn
//   steps); D = max(speed, 300 * step scale), max(50, dist / 2) when dist < 2D; turn radius
//   R = speed * (360 / turn rate) / 2pi. Operators in index order: 0 straight min(dist, D);
//   1 arc toward the goal when the heading error is in (0.01 deg, one step]; 2 / 3 arc left / right
//   one step; 4 straight to the goal (after op 1); 7 wait (100 frames at 0.2 x speed); 8 / 9 turn in
//   place to the escape headings (first expansion only, when the start overlaps an obstacle; 10 deg
//   probes up to +-180). Cost g += frames * multiplier (op 1 0.99, op 4 0.98, arcs
//   1 + 0.5 / max(1, dist_to_goal / 400), wait 0.8); h = distance / speed; f = g + w h with
//   w = 1 / heuristic divisor until the node travelled the weighting distance. Obstacle hits with
//   bits outside the mask are free; inside the mask a hit rejects the step when it involves bits
//   other than Moving and lies beyond radius * 1.2 + R from the start (and always for op 4), else the
//   step costs x MinObstacleCostSpace / CurrentPathCostCoefficientSpace. Successors outside the
//   playable box cost x OffMapCostPenalty, or are rejected when start and goal are inside and the
//   step is far from the start (and always for op 4). Open list = RetailHeap with capacity (a push
//   into a full heap is dropped); visited = hash on (cx, cy, bucket), probed only; closed states
//   re-expand only for ops 1, 4, 7, 8 when the new g is not worse; the budget counts pushes.
//   Retries (up to SpacePathingTries): from try 2 rotation divisor += 0.5, step scale += 0.5,
//   budget x 1.7, facing dropped; from try 3 weighting distance = max(prev + 0.25 * dist, best
//   progress) and heuristic divisor x 0.66; from try 4 the mask becomes Moving | Static; from try 5
//   the speed is halved. Moves shorter than 40 units skip planning.
// OI tactical (§2.1 operators): ops 5 / 6 (default mode) and 19 / 20 (facing mode) change the speed to max or
//   down to the wait speed along the heading: distance |v^2 - v0^2| / (2a), time |dv| / a (a = accel up / decel
//   down, <= 0 -> the speed: full change in one frame); op 5 needs v0 < max, op 6 v0 > wait speed. Closed states
//   re-expand only for ops 1, 4, 5, 6, 7, 8, 10, 11, 14, 15, 17-21 (the doc's list). FACING MODE (request
//   goal_facing set, try 1 only - the retries drop it): ops 10-22 (+ 23 / 24 = the escape turns on the first pop)
//   replace 0-9; goal test = goal cell AND (goal-facing bucket OR within MatchFacingDeltaSpace); h = (R x angle x
//   (FinalFacing180Penalty when angle > pi) + distance x SpecialAlignedOperatorBonus) / speed with angle = turn to
//   the goal direction + turn from it to the facing [port: one radius R]. Op 10 = full step D; 13 / 14 = arcs left /
//   right (as 2 / 3); 18 = wait (as 7). [port] geometry for the ops the doc only names: the goal-facing circles are
//   the two slow-turn circles (radius R x the wait-speed coefficient) tangent to the goal pose (left = traversed
//   counter-clockwise); 11 / 12 = straight tangent from the node onto the left / right circle, at full speed, only
//   when the node already heads along it (within one turn step), multiplier 0.6; 15 / 16 = the same at the wait
//   speed from a wait-speed node; 17 = the final arc along that circle from the tangent point onto the goal pose
//   (after 11 / 12 / 15 / 16), 21 / 22 = the same from any node lying on the left / right circle heading along it;
//   final arcs run at the wait speed with the wait-speed coefficient as multiplier, are collision-tested per turn
//   step and densified into turn-step waypoints (§2.4 "arc ops are densified").
// THREAT MODE (REMAINING_GAPS §4.2; request threat_mode()): D = ThreatExpansionDistance (600) and the operator set is
//   25-32 only: the compass table 0x140a43b80 (0,1) (1,1) (1,0) (1,-1) (0,-1) (-1,-1) (-1,0) (-1,1), not normalised;
//   d = min(distance to the goal, D), position += d x (dx, dy) (a diagonal covers d x sqrt 2), time += length / speed,
//   heading unchanged (planar port: the z := goal z step has no effect). A successor outside the playable box is
//   rejected; cost multiplier = ThreatCost::factor(successor) x the normal obstacle multiplier. [port]: the waypoint
//   heading of a compass step is its travel direction (the Hermite playback needs a tangent; the retail path
//   builder 0x597dc0 is not read for these ops). Arcs are collision-tested by their chord with
//   the radius inflated by the sagitta. When every try fails (retail outcome not traced), the port
// flies a rescue route: 8-connected A* over a 100-unit grid of the box around the static entries,
// string-pulled, straight legs with turn-in-place corners (PORT ADDITION [L]); only without a box
// or route does it fly the straight line.
//
// Path output (§2.4) and playback (§2.5): waypoints (position, heading, speed, time in frames);
// SimpleSpace replays them with a cubic Hermite curve (standard basis [M]), tangents = waypoint
// direction x speed x segment duration; speed = |derivative|, yaw = derivative direction, Z
// unchanged; zero-length segments interpolate yaw linearly.
#pragma once
#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/movement/constants.hpp"
#include "alamo/tactical/movement/heap.hpp"

#include "alamo/tactical/movement/common.hpp"

namespace alamo::tactical::movement {

enum SpaceCollisionBits : uint32_t {
    kSpaceMoving = 0x1, kSpaceStatic = 0x2, kSpaceAsteroid = 0x4, kSpaceIonStorm = 0x8, kSpaceNebula = 0x10,
    kSpaceImpassableAsteroid = 0x20,
};
inline constexpr uint32_t kSpaceDefaultMask = 0x3F;

// SpaceLayerType bits (§2.2).
enum SpaceLayer : uint32_t {
    kLayerCapital = 0x1, kLayerFrigate = 0x2, kLayerCorvette = 0x4, kLayerFighter = 0x8, kLayerSuperCapital = 0x20,
};

struct SpaceWaypoint {
    V2 pos;
    float heading = 0.0f;  // degrees
    float speed = 0.0f;    // units / frame
    float time = 0.0f;     // frames since the path start
};

class SpaceOccupancy {
public:
    void set_box(V2 min_xy, V2 max_xy);
    bool has_box() const { return has_box_; }
    bool inside_box(V2 p) const {
        return !has_box_ || (p.x >= box_min_.x && p.x <= box_max_.x && p.y >= box_min_.y && p.y <= box_max_.y);
    }
    V2 box_min() const { return box_min_; }
    V2 box_max() const { return box_max_; }
    void set_window(uint32_t tree_count, uint32_t interval) { window_ = tree_count * interval; }

    void add_static(ObjectId id, V2 center, float radius, uint32_t bits = kSpaceStatic);
    bool remove_static(ObjectId id);
    bool has_static(ObjectId id) const { return find(statics_, id) != statics_.end(); }
    // S7 t13: moves static entry `from` to id `to` unchanged (a map placement's entry, ted_grid.hpp map_stamp_id, handed to
    // the object spawned for it; space_obstacles_sync removes it when that object is a battle unit that left). False (nothing changes) when `from` has no entry, `to`
    // already has one, or from == to.
    bool rekey_static(ObjectId from, ObjectId to);
    // The static entry of `id` (centre, radius, SpaceCollisionType bits); false when there is none.
    bool static_entry(ObjectId id, V2& center, float& radius, uint32_t& bits) const;
    // Replaces the track of `id`. `start_frame` = absolute frame of waypoint time 0.
    void set_track(ObjectId id, uint32_t layer, float radius, uint32_t start_frame, std::vector<SpaceWaypoint> wps);
    void clear_track(ObjectId id);
    bool has_track(ObjectId id) const { return find(tracks_, id) != tracks_.end(); }

    // OR of the bits of every entry the circle (radius) swept a -> b over absolute frames [t0, t1]
    // touches. `layer`: tracks of that layer are tested; statics are tested when `with_static`.
    uint32_t query_segment(V2 a, V2 b, float t0, float t1, float radius, uint32_t layer, ObjectId self, bool with_static,
                           uint32_t now) const;
    uint32_t query_point(V2 p, float t, float radius, uint32_t layer, ObjectId self, uint32_t now) const {
        return query_segment(p, p, t, t, radius, layer, self, true, now);
    }
    // Static entries only (no time).
    uint32_t query_static(V2 a, V2 b, float radius) const;

    void hash(StateHasher& h) const;
    // N1 stage 2 save / load (src/movement_save.cpp): everything (static entries and tracks by ascending id, box, look-
    // ahead window). False (unchanged) on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Static {
        V2 c;
        float r;
        uint32_t bits;
    };
    // OPT3 t06: query_segment's per-piece values (start / end frame, start point, velocity, bit) formed once in set_track
    // with the same float expressions the query used to form per call, plus the waypoints' bounding box for a cull.
    struct Piece {
        float ps = 0.0f, pe = 0.0f;
        V2 p0, pv;
        uint32_t bit = 0;
        bool seg = false;  // a segment between two waypoints (the point moves); else before the first / after the last
    };
    struct Track {
        uint32_t layer;
        float r;
        uint32_t start;
        std::vector<SpaceWaypoint> wps;
        std::vector<Piece> pieces;  // derived from the fields above (make_pieces), never saved or hashed
        V2 lo, hi;                  // waypoint bounding box
    };
    static void make_pieces(Track& t);
    // OPT3 t06: id-sorted contiguous arrays (ascending id, the order the std::maps had) instead of tree nodes: the
    // planner's inner loop walks every entry per query.
    template <class T>
    using IdVec = std::vector<std::pair<ObjectId, T>>;
    template <class V>
    static auto lower(V& v, ObjectId id) {
        return std::lower_bound(v.begin(), v.end(), id, [](const auto& e, ObjectId k) { return e.first < k; });
    }
    template <class V>
    static auto find(V& v, ObjectId id) {
        const auto it = lower(v, id);
        return it != v.end() && it->first == id ? it : v.end();
    }
    template <class T>
    static void put(IdVec<T>& v, ObjectId id, T value) {  // insert or replace, keeping the order
        const auto it = lower(v, id);
        if (it != v.end() && it->first == id) it->second = std::move(value);
        else v.emplace(it, id, std::move(value));
    }
    template <class T>
    static bool erase(IdVec<T>& v, ObjectId id) {
        const auto it = find(v, id);
        if (it == v.end()) return false;
        v.erase(it);
        return true;
    }
    IdVec<Static> statics_;
    IdVec<Track> tracks_;
    V2 box_min_, box_max_;
    bool has_box_ = false;
    uint32_t window_ = 45u * 90u;
};

struct SpaceRequest {
    ObjectId self = 0;
    V2 start, goal;
    float start_heading = 0.0f;  // degrees
    float speed = 0.0f;          // units / frame
    float turn_rate = 0.0f;      // degrees / frame
    float radius = 0.0f;
    uint32_t layer = kLayerFrigate;
    uint32_t mask = kSpaceDefaultMask;
    uint32_t now = 0;            // absolute frame of the path start
    bool wait_operators = true;
    // OI tactical: speed-change operators and facing mode.
    float accel = 0.0f;          // units / frame^2 (<= 0: = speed)
    float decel = 0.0f;
    float goal_facing = 1e18f;   // degrees; 1e18 (kNoFacing) = no final facing required
    // REMAINING_GAPS §4.2: threat mode (+0x121). On when threat_threshold != kThreatOff AND space_layer_type == 0
    // (the raw Space_Layer, None / unset: fighters and the few types tagged None; never corvettes and up).
    float threat_threshold = kThreatOff;  // order threshold T
    uint32_t space_layer_type = 0;        // raw Space_Layer (UnitDesc::space_layer_type)
    const ThreatFn* threat = nullptr;     // threat grid point query for the owner (nullptr: no grid, cost x 1)
    bool threat_mode() const { return threat_threshold != kThreatOff && space_layer_type == 0; }
};

struct SpacePath {
    bool ok = false;
    bool fallback = false;       // every try failed: straight line
    bool skipped = false;        // shorter than 40 units: no planning
    uint32_t tries = 0;
    uint32_t pushes = 0;         // total over the tries
    std::vector<SpaceWaypoint> wps;
};

struct SpaceScratch {
    struct Node {
        V2 pos;
        float heading = 0.0f, g = 0.0f, time = 0.0f, travelled = 0.0f, speed = 0.0f;
        int32_t parent = -1;
        uint8_t op = 0;
        // OI tactical: final arcs (ops 17 / 21 / 22): circle centre, radius (0 = not an arc) and signed sweep (deg).
        V2 arc_c;
        float arc_r = 0.0f, arc_sweep = 0.0f;
    };
    std::vector<Node> nodes;
    RetailHeap<int32_t> heap;
    struct Visit {
        float g;
        bool closed;
    };
    std::unordered_map<uint64_t, Visit> visited;  // probed only, never iterated
};

bool plan_space_path(const SpaceOccupancy& occ, const MovementConstants& k, const SpaceRequest& req, SpaceScratch& scratch,
                     SpacePath& out);

// Destination search (§2.4, 0x59caa0): goal if free, else biased rings starting at radius * 1.34,
// +50 per ring, 40 rings; `reserved` = other group members' destinations (with their radii),
// walked in list order. Returns the point used (the goal when nothing is free).
struct ReservedSpot {
    V2 pos;
    float radius = 0.0f;
};
V2 find_space_destination(const SpaceOccupancy& occ, const MovementConstants& k, V2 goal, V2 approach_from, float radius,
                          uint32_t layer, ObjectId self, uint32_t now, std::span<const ReservedSpot> reserved);

struct SpacePose {
    V2 pos;
    float heading = 0.0f;
    float speed = 0.0f;
    bool done = false;  // t >= last waypoint time
};
// Hermite playback at `t` frames since the path start.
SpacePose eval_space_path(std::span<const SpaceWaypoint> wps, float t);

}  // namespace alamo::tactical::movement

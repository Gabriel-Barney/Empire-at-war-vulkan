// Land collision helpers (docs/re/MOVEMENT.md §1.5, §5). There is no rigid-body physics: a
// collision is a predicted conflict along the planned paths, resolved by behaviour. The
// orchestration (every-4-frames checks, candidate query, resolutions) lives in MovementSystem;
// this header holds the pure pieces and the persistent collision-object layer (priority order,
// crowd refresh, budget gate; TACTICAL_GAPS.md §6.7).
//
// s78n t18 (MOVEMENT §5.3.1): footprints are oriented rectangles (FootRect) - a squad container's is its joint slot box,
// any other unit's its hard extents; the three contact tests are foot_overlap_coefficient (stopped vs stopped),
// foot_path_contact (moving vs stopped) and foot_moving_contact (moving vs moving). The older circle helpers below
// (predict_moving_contact, path_contact_distance, overlap_ratio) are kept for their callers outside the land tick.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/movement/constants.hpp"
#include "alamo/tactical/movement/passability.hpp"

namespace alamo::tactical::movement {

// UnitCollisionClassType (§5.2 step 6).
enum UnitCollisionClass : int32_t {
    kUccNone = -1, kUccLandingTransport = 0, kUccBuildPad = 1, kUccLowObstacle = 2, kUccMediumObstacle = 3,
    kUccHighObstacle = 4, kUccInfantry = 5, kUccBike = 6, kUccVehicle = 7, kUccLargeVehicle = 8, kUccGiantVehicle = 9,
};

// Whether two collision classes interact (the table applied from both sides). Build pads are
// treated as never colliding (the enemy-only faction test is not modelled [M]).
bool collision_classes_interact(int32_t a, int32_t b);

// Moving vs moving: closest approach of p + v t over t in [0, frames]; true when the distance drops
// below `radius_sum`; `t_contact` = first time it does.
bool predict_moving_contact(V2 pa, V2 va, V2 pb, V2 vb, float radius_sum, float frames, float& t_contact);

// Moving vs stopped: walks the path from `pos` through path[next..] up to `max_dist`; returns the
// distance along the path at which the swept circle first touches `obstacle` (radius_sum), or -1.
float path_contact_distance(V2 pos, std::span<const V2> path, uint32_t next, V2 obstacle, float radius_sum, float max_dist);

// Stopped vs stopped overlap ratio (penetration / smaller diameter), 0 when apart.
float overlap_ratio(V2 a, float ra, V2 b, float rb);

// Dynamic local pathfinder (§1.5): window = cell box of start and target, grown by
// DynamicAvoidanceRectangleBound + the unit's half size in cells + 2 (no +2 for Giant), then by 3
// cells per side per failed attempt, up to 20 attempts. Inside the window an 8-connected cell A*
// (no corner cutting; same heap rules) where cells covered by another unit's inflated circle cost
// x DynamicObstacleOverlapPenalty. `complexity` = the search's graph vertex count (4 corners per
// obstacle in the window + 2), which the caller adds to the quota counter.
// PROVISIONAL [M]: the retail graph is a visibility graph over obstacle rectangle corners; the cell
// search keeps its cost model (overlap penalty, window) and quota accounting.
struct DynamicObstacle {
    V2 pos;
    float radius = 0.0f;
};
bool dynamic_repath(const PassabilityGrid& grid, V2 start, V2 target, uint32_t mask, float self_radius, bool giant,
                    std::span<const DynamicObstacle> obstacles, const MovementConstants& k, std::vector<V2>& out,
                    uint32_t& complexity);

// Crush (§5.5): victims whose world AABB overlaps any crusher box. Inputs are XY boxes; returns
// indices into `victims` ascending (the caller iterates players / objects in their order and
// applies 1,000,000 damage of Squash_Damage_Type).
struct Box2 {
    V2 min, max;
};
std::vector<uint32_t> find_crush_victims(std::span<const Box2> crusher_boxes, std::span<const Box2> victims);
inline constexpr float kCrushDamage = 1000000.0f;

// ---- s78n t18 (MOVEMENT §5.3.1): footprint rectangles of the three land contact tests ----
// The rectangle 0x38de80(obj): axis a = (cos yaw, sin yaw), b = (-sin yaw, cos yaw), centre, half extents (hx along a, hy
// along b). A squad container's half extents are its joint slot box (0x404450), any other unit's its hard extents.
// [port] `round`: a unit whose type has no hard extents is a circle of its soft radius hx (= hy): its extent is hx along
// every axis, and in the separating-axis tests it brings the centre line and its normal instead of its own two axes.
struct FootRect {
    V2 c;      // centre
    V2 a;      // forward axis (unit)
    float hx = 0.0f, hy = 0.0f;
    bool round = false;
};
inline FootRect foot_rect(V2 centre, float yaw_deg, float hx, float hy) { return {centre, dir_from_deg(yaw_deg), hx, hy, false}; }
inline FootRect foot_circle(V2 centre, float yaw_deg, float r) { return {centre, dir_from_deg(yaw_deg), r, r, true}; }
// R(rect, n) = |a . n| hx + |b . n| hy (0x20d140), n a unit axis.
float foot_extent(const FootRect& r, V2 n);
// Overlap 0x20d690: |D . n| <= R1(n) + R2(n) on the four axes {a1, b1, a2, b2} (touching counts).
bool foot_overlap(const FootRect& r1, const FootRect& r2);
// Overlap coefficient 0x20d7a0 = min(1, min over the four axes of 1 - |D . n| / (R1 + R2)); an axis on which the two are
// apart, or with R1 + R2 = 0, gives 0.
float foot_overlap_coefficient(const FootRect& r1, const FootRect& r2);
// Gap 0x20dc10 = max over the four axes of max(0, |D . n| - (R1 + R2)) (0 when they touch or overlap).
float foot_gap(const FootRect& r1, const FootRect& r2);
// Directional radius rho(h, c) = hy + (hx - hy) c^2, c = |cos| of the angle between the forward axis and the line to the
// other party.
inline float foot_rho(float hx, float hy, float c) { return hy + (hx - hy) * c * c; }
// 0x4422f0 (in doubles): a point moving m -> m + d against the origin with the contact radius running linearly r0 -> r1:
// the first s in [0, 1] with |m + s d| <= r0 + s (r1 - r0); already inside at s = 0 -> 0; none -> -1.
double foot_swept_contact(V2 m, V2 d, float r0, float r1);
inline constexpr float kFootApproxStep = 2.0f;  // ApproximationForwardDistance: the path approximation's point spacing
// MovingVsStopped 0x441b60: the mover's path polyline from `pos` through path[next..] (its forward axis is the segment
// direction; half extents ha = (hx, hy)) against the stopped rectangle `b`. Per segment the contact radius is rho(mover,
// cos to the segment direction) + rho(b, cos to b's facing) at both ends, linear in between. Returns the path length to the
// first contact when that is < max_dist, else -1. `turn_yaw` != kNoFacing: the mover turns in place - one zero-length
// segment at `pos` with the forward axis dir(turn_yaw) (the caller swells ha toward a square).
float foot_path_contact(V2 pos, std::span<const V2> path, uint32_t next, V2 ha, const FootRect& b, float max_dist,
                        float turn_yaw = kNoFacing);
// MovingVsMoving predictor 0x442540: both movers walked along their polylines in step at their assumed speeds (the
// effective max speed when facing toward the other party, else the current speed), directional radii with the lane rule,
// whole frames; true when they touch before `frames` (45; 90 for a crusher pair). With neither moving at the start: the
// rectangle overlap of the two footprints. [port] one step per frame along each polyline.
struct FootTrack {
    V2 pos;
    float yaw = 0.0f;
    std::span<const V2> path;  // remaining waypoints are path[next..]
    uint32_t next = 0;
    float speed = 0.0f, max_speed = 0.0f;
    float hx = 0.0f, hy = 0.0f;
};
bool foot_moving_contact(const FootTrack& p, const FootTrack& q, int32_t frames);

// ---- S7 persistent collision objects (TACTICAL_GAPS.md §6.7, MOVEMENT.md §5.4, §1.5).
// A collision object (MovingVsMoving / MovingVsStopped / StoppedVsStopped) lives across frames from the check that
// found it (creation frame +0x64) until the driver cancels it (no longer applies) or its resolution finishes.
// Driver 0x50a780, once per frame:
//   1. keep pass in list order: collisions whose keep test fails are cancelled (removed; list order is kept);
//   2. priority = crowd - (float)(now - creation frame); crowd is refreshed when now >= the refresh frame (+0x6c),
//      then the refresh frame = now + 30: crowd = 5 x the land object-grid objects in a 160 x 160 box centred on the
//      XY midpoint of the first two participants, 0 with fewer than 2 participants;
//   3. ascending sort by priority (older, less crowded collisions first). Retail uses MSVC std::sort (insertion
//      sort, stable on ties, for <= 32 entries); [port] a stable sort by (priority, insertion order) everywhere;
//   4. each collision in that order: full resolution when the dynamic land pathfinder budget allows (0x679740:
//      quota counter < DynamicLandComplexityQuota), else the cheap fallback.
// Containers are list-ordered vectors and a sorted vector of id pair keys (no pointer keys, no hashing).
enum class CollisionKind : uint8_t { MovingVsMoving = 0, MovingVsStopped = 1, StoppedVsStopped = 2 };
// The retail per-class state enums (MOVEMENT §5.4 table) folded into one: Active = state 0 None (not decided yet),
// Resolved = MovingVsStopped 5 / 6, MovingVsMoving 8 / 9 (the driver drops the record in the same pass). s78n t18
// (§5.4.1 / §5.4.2): TryAgainLater = MovingVsStopped 7; Repulsion = MovingVsStopped 2 / MovingVsMoving 3 Repulse;
// PathAround = state 1 of both; Crusher = MovingVsStopped 4 (the crusher drives on) / MovingVsMoving 7 (both at max
// speed); SpeedSlave = MovingVsMoving 2 Speed Slave Only. StoppedVsStopped and the MovingVsMoving stand-in of pairs that
// are not Infantry / Bike stay Active ([port]).
enum class CollisionState : uint8_t {
    Active = 0, TryAgainLater = 1, Resolved = 2, Repulsion = 3, PathAround = 4, Crusher = 5, SpeedSlave = 6,
};

inline constexpr uint32_t kCollisionCrowdRefreshFrames = 30;
inline constexpr float kCollisionCrowdBox = 160.0f;  // side of the crowd box
inline constexpr uint32_t kCollisionCrowdWeight = 5;

struct CollisionObject {
    uint32_t seq = 0;  // insertion order (list position key)
    CollisionKind kind = CollisionKind::MovingVsMoving;
    CollisionState state = CollisionState::Active;
    uint8_t participants = 2;
    ObjectId a = 0;            // the unit whose check found it (the mover for MovingVsStopped)
    ObjectId b = 0;            // the other participant
    uint32_t created = 0;      // creation frame (+0x64)
    uint32_t crowd_frame = 0;  // next crowd refresh frame (+0x6c); = created, so the first sort refreshes
    float crowd = 0.0f;
    float priority = 0.0f;     // this frame's sort key
    uint32_t retry = 0;        // TryAgainLater: the next re-decide frame (+0x80)
    // s78n t18 (vf +0x40): the speed this record answers participant a / b (3e38 = its max speed) and the "at once"
    // flag, as of its last service; MovingVsMoving: the master (0 = a, 1 = b). Saved and hashed.
    float speed_a = 3.0e38f, speed_b = 3.0e38f;
    bool now_a = false, now_b = false;
    uint8_t master = 0;
};

// crowd - (float)(now - created).
float collision_priority(float crowd, uint32_t now, uint32_t created);

class CollisionLayer {
public:
    using KeepFn = std::function<bool(CollisionObject&)>;
    using CountFn = std::function<uint32_t(const CollisionObject&)>;  // object-grid objects in the crowd box
    using BudgetFn = std::function<bool()>;                           // true = full resolution allowed now
    using ResolveFn = std::function<void(CollisionObject&, bool full)>;
    struct Step {
        uint32_t seq = 0;
        bool full = false;
    };

    // Pair filter (0x5c7900): at most one collision per unordered pair.
    bool has_pair(ObjectId a, ObjectId b) const;
    // Appends a collision at the end of the list; returns its seq. No-op (returns UINT32_MAX) when the pair exists.
    uint32_t add(CollisionKind kind, ObjectId a, ObjectId b, uint32_t frame);
    // One driver pass (steps 1-4 above). Resolve must not add or remove collisions; a collision it marks Resolved is
    // dropped after the pass.
    void run(uint32_t frame, const KeepFn& keep, const CountFn& count, const BudgetFn& budget, const ResolveFn& resolve);

    std::span<const CollisionObject> list() const { return list_; }
    std::span<const Step> last_order() const { return order_; }  // the last run's resolution order
    uint32_t last_cancelled() const { return cancelled_; }
    uint32_t last_resolved() const { return resolved_; }
    void clear();
    void hash(StateHasher& h) const;
    // N1 stage 2 save / load (src/movement_save.cpp): the list (insertion order, every field), the last run's order and
    // counters, the next seq. The pair index is rebuilt from the list. False (unchanged) on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    void drop_pair(const CollisionObject& c);
    std::vector<CollisionObject> list_;  // list (insertion) order
    std::vector<uint64_t> pairs_;        // sorted unordered pair keys of list_
    std::vector<Step> order_;
    std::vector<uint32_t> sort_;         // scratch
    uint32_t next_seq_ = 0;
    uint32_t cancelled_ = 0, resolved_ = 0;
};

}  // namespace alamo::tactical::movement

// Land collision helpers (docs/re/MOVEMENT.md §1.5, §5). There is no rigid-body physics: a
// collision is a predicted conflict along the planned paths, resolved by behaviour. The
// orchestration (every-4-frames checks, candidate query, resolutions) lives in MovementSystem;
// this header holds the pure pieces and the persistent collision-object layer (priority order,
// crowd refresh, budget gate; TACTICAL_GAPS.md §6.7).
//
// PROVISIONAL (§8 item 9, MovingVsMoving resolution / push-return / footprint semantics are not
// traced): footprints are circles of the unit's soft radius; contact tests use closest approach
// of straight-line extrapolation (moving vs moving) and distance along the remaining path
// (moving vs stopped); the stopped-vs-stopped overlap ratio is the circle penetration depth over
// the smaller diameter.
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
// [port] the retail per-class state enums (§5.4 table) are reduced to what the port's resolutions use.
enum class CollisionState : uint8_t { Active = 0, TryAgainLater = 1, Resolved = 2 };

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
    uint32_t retry = 0;        // TryAgainLater: resolution resumes at this frame
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

// Formations (docs/re/MOVEMENT.md §4).
//
// kmeans_buckets (§4.2 step 1): k = floor(sum(4 * halfX * halfY) / 400 / Max_Formation_Area) + 1;
//   k == 1 or k > n -> one bucket; initial assignment round-robin (i mod k); Lloyd iterations with
//   centroids summed in member order, reassignment to the nearest centroid by strict "<" (ties keep
//   the lower bucket), stop when nothing moves or after `max_iterations` (retail cap unread [L]: 16).
// split_into_formations (§4.2 step 2): seed = remaining member nearest the bucket centroid (ties
//   keep the earlier); Solo seeds stay alone; otherwise members join while the grouping rule passes
//   (Standard recruits Standard; SameOrder needs SameOrder with an equal FormationOrder), the
//   `compatible` callback agrees (same locomotor kind, zone connectivity) and the candidate is
//   within the recruitment distance (60 infantry not in a team, else 100) of the nearest member.
//   With `double_click_speed_ratio` > 0 (order flag 0x80, P1_DOUBLE_CLICK_MOVE.md §4.2) a candidate with a non-zero
//   max speed s_c is also rejected when max(s_c, s_seed) / min(s_c, s_seed) > the ratio (DoubleClickMoveMaxSpeedRatio;
//   symmetric; a zero-speed seed rejects every moving candidate); rejected units seed later formations, so the
//   selection splits into speed bands. PROVISIONAL [M]: the engagement and path-length tests are not modelled.
// large_hover_formation_order (P1_DOUBLE_CLICK_MOVE.md §4.3, 0x439360): the index order of the LargeHover formations
//   of one order: without 0x80 by centroid projection on the move direction, descending (front to back); with 0x80
//   by formation speed, descending (fastest first). Ties keep creation order (stable [port]). The retail layout of
//   these formations around the group centroid is not modelled.
// layout_formation (§4.3): ranks (Hero first; ascending max speed, slowest in front; speeds within
//   tolerance T are one rank, T starting at 0 and growing by 15 while >= 4 ranks remain, cap 4 steps
//   [L]; equal speeds by case-insensitive type name, then object id - the stable replacement of the
//   retail unstable std::sort, §7), rows built front to back (a row = one rank, candidates ordered by
//   forward projection descending, taken while the cumulative width <= 2 * half-width, overflow
//   returned to the front, kept members re-sorted by lateral projection so units keep their
//   left/right order), lateral placement gap = (2hw - sum widths) / (count + 1), rows stepping back
//   by the row's largest width. Widths are in cells (x 20 for world units): member width =
//   FormationSpacing + 2 * radius / 20; half-width = max(side * cell + 0.25, sqrt(max(1, area * 1.8)))
//   with side = ceil(sqrt(n)), cell = the largest member width, area = sum of (2r/20)^2 [L].
//   PROVISIONAL [L]: the block is centred on the destination (front row half the depth ahead).
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <vector>

#include "alamo/tactical/movement/common.hpp"
#include "alamo/tactical/movement/constants.hpp"

namespace alamo::tactical::movement {

enum FormationGrouping : int32_t { kGroupingSolo = 0, kGroupingSameOrder = 1, kGroupingStandard = 2 };

struct FormationMember {
    ObjectId id = 0;
    V2 pos;
    float max_speed = 0.0f;
    float radius = 10.0f;
    float spacing = 0.0f;           // FormationSpacing (cells)
    bool hero = false;
    std::string_view type_name;
    int32_t grouping = kGroupingStandard;
    int32_t order = 0;               // FormationOrder
    int32_t collision_class = -1;
    bool team_member = false;
};

std::vector<std::vector<uint32_t>> kmeans_buckets(std::span<const FormationMember> m, float max_formation_area,
                                                  uint32_t max_iterations = 16);

using CompatibleFn = std::function<bool(uint32_t seed, uint32_t candidate)>;
std::vector<std::vector<uint32_t>> split_into_formations(std::span<const FormationMember> m, std::span<const uint32_t> bucket,
                                                         const MovementConstants& k, const CompatibleFn& compatible = {},
                                                         float double_click_speed_ratio = 0.0f);

struct FormationKey {
    float speed = 0.0f;  // formation speed (slowest member)
    V2 centroid;
};
// Returns indices into `f` in index order (position = the formation's +0x22c index).
std::vector<uint32_t> large_hover_formation_order(std::span<const FormationKey> f, V2 move_dir, bool fastest_first);

// Rank order (front to back) and per-member rank, for `members` (indices into m).
struct RankedMember {
    uint32_t member;
    uint32_t rank;
};
std::vector<RankedMember> rank_members(std::span<const FormationMember> m, std::span<const uint32_t> members);

struct Slot {
    uint32_t member = 0;
    float lateral = 0.0f;  // world units along `right` (= forward rotated -90 deg)
    float back = 0.0f;     // world units behind the destination
    uint32_t row = 0;
};
// Slots in front-to-back, left-to-right order.
std::vector<Slot> layout_formation(std::span<const FormationMember> m, std::span<const uint32_t> members, V2 forward,
                                   const MovementConstants& k);
inline V2 formation_right(V2 forward) { return {forward.y, -forward.x}; }
inline V2 slot_world(V2 destination, V2 forward, const Slot& s) {
    return destination + formation_right(forward) * s.lateral - forward * s.back;
}

// s78n t20 (MOVEMENT §4.3.1 item 4, 0x43d6a0): the offset from the pass centre of try k (0 .. 39) of one formation's
// destination check. t = 0 at first and grows by 7.0 after each try with k mod 8 < 4, so t(k) = 7 x (4 x (k / 8) + k mod
// 8) there and t - 28 = 28 x (k / 8) for k mod 8 = 4 .. 7. Tries k mod 8 < 4: t v; tries 4 .. 7: (t - 28) v + m x {(1, 0),
// (0, 1), (-1, 0), (0, -1)} (world axes; m = min(HX + S, HY + S)). `first_unblocked` (the order's first formation, until
// a try met impassable ground): the v term is halved and takes the factor +1 for even k, -0.75 for odd k - with v
// pointing back along the final direction: 0, 2.625 beyond, 7 short, 7.875 beyond, four sideways, 14 short, ...
V2 destination_try_offset(uint32_t k, bool first_unblocked, V2 v, float m);

// Side-error correction while moving as a group (§4.4): steer by (error -+ 0.1) / 30 when
// |error| > FormationMinimumSideError, else 0.
inline float formation_side_correction(float error, float min_err = 0.1f, float max_err = 30.0f) {
    if (error > min_err) return (error - min_err) / max_err;
    if (error < -min_err) return (error + min_err) / max_err;
    return 0.0f;
}

}  // namespace alamo::tactical::movement

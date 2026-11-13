// [port] Fleet groups, phase 1 (docs/DECISIONS.md "Fleet groups: exception to the current focus", Gabriel 2026-11-13;
// design: the claude.ai doc "Fleet groups design"). Not retail; off by default (TacticalBattle::enable_fleet_groups). With
// the switch off no group can exist, so nothing here reaches the tick CRC or a save.
//
// A fleet group is a persistent, named set of one player's battle units (at most kMaxMembers, kMaxGroupsPerPlayer groups per
// player). A unit is in at most one group. Each member has a SLOT: its offset from the group centre in the group's frame
// (along = forward, side = left; the frame's forward is `facing_deg`, movement convention: 0 = +X, counter-clockwise).
// Slots are captured when the group is made and when it is (re)locked; a locked move keeps them.
//
// Locked move (plan_move): the selected members of a locked group move as a block. The block's centre (the mean slot of
// the moving members) goes to the destination, the frame turns to the event's facing (or, without one, to the direction
// from the members' current centroid to the destination), and each member gets one single-unit move to its slot. Queued
// moves are not expanded (the waypoint queue keeps the retail path).
//
// Speed (regulate, every tick after the host's speed factors): every member of a moving locked group runs at most at the
// slowest member's effective max speed (scale = slowest / own). Stragglers are waited for (Gabriel 2026-11-13): progress
// p = 1 - remaining / start distance to the slot; a member whose lead over the least-progressed member, lead = (p - p_min)
// x its start distance, exceeds straggler_distance is slowed by 1 - (lead - D) / D, floored at min_leader_scale. A move
// ends when no member has a movement goal left; the slots stay as they were.
//
// Determinism (rule 3): groups in a vector in creation order, members ascending by id, every loop in that order, the math
// is + - * / sqrt and alamo::dmath; ids are issued from a counter and never reused within a battle.
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "alamo/commands/events.hpp"
#include "alamo/commands/fleet_group.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/tactical/movement/common.hpp"

namespace alamo {
class ByteWriter;
class ByteReader;
class StateHasher;
}  // namespace alamo

namespace alamo::sim {

struct FleetMember {
    objects::ObjectId id = objects::kNoObjectId;
    tactical::movement::V2 slot;  // (along, side) in the group frame
    // A running locked move: the member's goal and its distance to it when the move started.
    tactical::movement::V2 goal;
    float start_dist = 0.0f;
    bool moving = false;          // part of the running move
};

struct FleetGroup {
    uint16_t id = 0;
    uint8_t owner = 0;
    bool locked = false;
    bool moving = false;  // a locked move is running
    float facing_deg = 0.0f;
    std::string name;
    std::vector<FleetMember> members;  // ascending id
};

// What the host knows about one unit. `valid` false = not a live, movable battle unit (the group drops it).
struct FleetUnitInfo {
    bool valid = false;
    int32_t owner = -1;      // store owner (a member whose owner is no longer the group's leaves it)
    tactical::movement::V2 pos;
    float yaw_deg = 0.0f;
    float max_speed = 0.0f;  // effective max speed this tick (host speed factor applied)
    bool has_goal = false;   // still moving / waiting to move
};

class FleetGroups {
public:
    static constexpr size_t kMaxGroupsPerPlayer = 20;
    static constexpr size_t kMaxMembers = 40;
    using InfoFn = std::function<FleetUnitInfo(objects::ObjectId)>;

    // Straggler rule (see the header comment); world units and a fraction.
    float straggler_distance = 150.0f;
    float min_leader_scale = 0.25f;

    // One fleet group command for `player`. `units` = the issuer's selection, already filtered to the player's live movable
    // battle units (ascending or not). Create needs at least one unit and a free group; Rename needs the player's group.
    commands::RejectReason apply(uint8_t player, const commands::FleetGroupCommand& c, std::span<const objects::ObjectId> units,
                                 const InfoFn& info);

    const std::vector<FleetGroup>& groups() const { return groups_; }
    const FleetGroup* group_of(objects::ObjectId id) const;
    const FleetGroup* group(uint16_t id) const;
    bool used() const { return used_; }  // a group was made in this battle (CRC / save gate)
    bool any_moving() const;

    // A locked move of one group: one goal per moving member (ascending id) and the block's new facing.
    struct MemberGoal {
        objects::ObjectId id = objects::kNoObjectId;
        tactical::movement::V2 goal;
    };
    struct LockedMove {
        uint16_t group = 0;
        float facing_deg = 0.0f;
        std::vector<MemberGoal> goals;
    };
    // Splits `units` (the issuer's) into the members of `player`'s locked groups (one LockedMove per group, in group order)
    // and the rest (`rest`, in the given order). `facing_deg` = the event's facing or kNoFacing. Changes nothing: the host
    // issues the orders, then calls start_move for each move it issued.
    void plan_move(uint8_t player, std::span<const objects::ObjectId> units, tactical::movement::V2 dest, float facing_deg,
                   const InfoFn& info, std::vector<LockedMove>& moves, std::vector<objects::ObjectId>& rest) const;
    void start_move(const LockedMove& m, const InfoFn& info);
    // Another order reached these units: their group's running move stops (the lock and slots stay).
    void interrupt(std::span<const objects::ObjectId> units);
    // Drops members whose info is no longer valid (dead, retreated) or whose owner changed, and removes groups left empty. Group order.
    void prune(const InfoFn& info);
    // Per tick, after the host set its own speed factors: `scale(id, s)` for every member of a moving locked group (group
    // order, ascending id), s in (0, 1]. Ends the moves that have no member with a goal left.
    void regulate(const InfoFn& info, const std::function<void(objects::ObjectId, float)>& scale);

    void hash(StateHasher& h) const;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);  // false (state unchanged) on bad data

private:
    FleetGroup* find(uint16_t id);
    void remove_units(std::span<const objects::ObjectId> units);
    void drop_empty();
    static void capture_slots(FleetGroup& g, const InfoFn& info);

    std::vector<FleetGroup> groups_;
    uint16_t next_id_ = 1;
    bool used_ = false;
};

}  // namespace alamo::sim

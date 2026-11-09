// cu8l t03 - the campaign space fleet placement 0x4fbb10 (docs/re/CU8K_T03_T08_T09_RE.md §1.2 - §1.8, §4 items 1-3): the
// pure layout of one anchor group. The host (TacticalBattle) registers the spawned objects, splits them into groups (group k
// on marker k mod count), describes each object (class, cull-box half extents, squadron offsets) and applies the result
// (orientation := the anchor's, teleport, leave + adopt, the squadron members).
//
// Per anchor [H]: objects by Ship_Class into L0 capital, L1 corvette / frigate, L2 (never filled), L3 fighter / bomber
// (interleaved when both kinds are present), L4 transport, L5 super; other classes are not placed. Then: L5 through the
// capital grid with size (0, 0) (every super at the anchor); the first non-empty of L0..L3 as the grid core (size §1.3),
// r = sqrt(W^2 + W^2); for a core list i < 3 the next list as the screen arc and the rest as lead arcs; the transports in
// rows behind the core; with L0..L3 all empty the transports form the grid instead. Results in placement order (supers,
// core, screen, leads, transports), the order of the adopt draws.
//
// All arithmetic is float, angles turn into radians as theta * (2 pi / 360) (dmath sin / cos), no FMA (CLAUDE.md rule 3).
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/tactical/movement/common.hpp"

namespace alamo::tactical::movement::fleet {

// Cull-box half extents of an object (object +0x28 vf +8: +0xc / +0x10). [M] The port reads the type's model bounds in
// the object's frame: hx along its facing, hy across it.
struct HalfBox {
    float hx = 0.0f, hy = 0.0f;
};

struct FleetObject {
    int32_t ship_class = -1;  // Ship_Class (a team container: its first member's)
    bool team = false;        // a TEAM container (members follow it)
    HalfBox box;              // not a team: its own box
    std::vector<HalfBox> members;  // team: the members' boxes, member order (live members)
    // team: Squadron_Offsets (x, y) of the team's +0x50 type (Create_Team_Type, else the company type) and of the company
    // type (+0x58), and their Squadron_Units counts (type +0x980 [M]) for the capital grid's offset-type switch.
    std::vector<V2> offsets_team, offsets_company;
    uint32_t units_team = 0, units_company = 0;
};

enum class Via : uint8_t { Capital = 0, Screen = 1, Lead = 2, Transport = 3 };

struct Placed {
    uint32_t index = 0;      // into the group's object list
    Via via = Via::Capital;
    V2 pos;                  // the object's new position (z unchanged)
    std::vector<V2> members; // team: member j's new position (empty: the members are not moved - transport rows)
};

// The list (0..5) an object's Ship_Class goes to; -1: not placed.
int32_t list_of_class(int32_t ship_class);
// 0x4ff100: the L3 order when it holds both fighters (class 0) and bombers (class 1); each kind keeps its order.
std::vector<uint32_t> interleave(std::span<const uint32_t> fighters, std::span<const uint32_t> bombers);
// 0x4fca30 rows: n < 3 -> 1; < 7 -> 2; < 10 -> 3; < 17 -> 4; <= 25 -> 5; else 6.
uint32_t grid_rows(uint32_t n);
// Ships per row (row 0 first): n div R each, the remainder spread by 0x4fb9c0's table.
std::vector<uint32_t> grid_row_counts(uint32_t n);
// 0x4fc3d0: S = (Sx, Sy) = (2 p, 2 q) of a list (Sx the lateral spacing, Sy the row depth).
V2 list_size(std::span<const FleetObject> objs, std::span<const uint32_t> list);
// The anchor-local rotation angles: theta = yaw + 270, minus 360 once when > 360 (grid / transport rows), and yaw + a +
// 270 wrapped into [0, 360] by repeated +-360 (screen / lead arcs).
float grid_theta(float yaw_deg);
float arc_theta(float yaw_deg, float a_deg);

// Layout functions (positions appended to `out` in placement order). Each returns what the original returns.
// 0x4fca30: W (the half width of the widest row: (m - 1) * Sx / 2, or Sx / 2 for one ship per row); 0 for an empty list.
float capital_grid(std::span<const FleetObject> objs, std::span<const uint32_t> list, V2 size, V2 anchor, float yaw_deg,
                   std::vector<Placed>& out);
// 0x4fdfb0 / 0x4fd5b0: the outer radius r' (r unchanged for an empty list).
float screen_arc(std::span<const FleetObject> objs, std::span<const uint32_t> list, float r, V2 anchor, float yaw_deg,
                 std::vector<Placed>& out);
float lead_arc(std::span<const FleetObject> objs, std::span<const uint32_t> list, float r, V2 anchor, float yaw_deg,
               std::vector<Placed>& out);
// 0x4fea10: rows behind the core (W = the core grid's half width).
void transport_rows(std::span<const FleetObject> objs, std::span<const uint32_t> list, float w, V2 anchor, float yaw_deg,
                    std::vector<Placed>& out);

// One anchor group (objects in registration order; team members are not listed): the whole 0x4fbb10 per-anchor pass.
std::vector<Placed> place_group(std::span<const FleetObject> objs, V2 anchor, float yaw_deg);

}  // namespace alamo::tactical::movement::fleet

// A1 - tactical AI target list (AITargetLocations), docs/re/AI_TACTICAL.md 3.1-3.2.
//
// One list per AI player per battle (perception system +0xe0; goal proposal and FindTarget iterate
// it in list order). Entries:
//   * unit targets (TacticalUnitAITargetLocationClass): objects whose representative type has
//     Has_Land_Evaluator (land) / Has_Space_Evaluator (space), APPENDED when the object is added
//     to the mode and ERASED IN PLACE (order of the rest kept) on its destroyed signal;
//   * region targets (TacticalRegionAITargetLocationClass): land = one per map AI hint zone, in
//     zone-list order, appended after the existing objects at battle start; space = the
//     AI_SpaceEvaluatorRegionSize grid "CELL_<x>_<y>", x outer / y inner, built only when the
//     threat grid exists.
// Later unit targets are appended after the regions (one vector) [H].
//
// Name ids: entries carry the name string ("OBJECT_<id>", the zone name, "CELL_<x>_<y>"); the
// interned name id / perceptual evaluator registration is the A2 framework's (stage 2 hookup).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ai/tactical/constants.hpp"
#include "alamo/ai/tactical/hint_zones.hpp"

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::ai::tactical {

struct Vec3f {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    friend bool operator==(const Vec3f&, const Vec3f&) = default;
};

enum class TargetKind : uint8_t { Unit = 0, Region = 1 };

struct TacticalTarget {
    TargetKind kind = TargetKind::Unit;
    int32_t object_id = -1;  // unit targets (the live position is the object's; see find_target.hpp)
    std::string name;        // "OBJECT_<id>" / hint-zone name / "CELL_<x>_<y>"
    Vec3f position;          // regions: land = zone centre, space = unclipped cell centre with z = 0
    Rect2 eval_rect;         // space cells: the cell clipped to the map (evaluator rect)
    int32_t zone = -1;       // land regions: hint-zone index (owns the ZoneReachability object)
    int32_t cell_x = -1, cell_y = -1;  // space cells
    bool is_unit() const { return kind == TargetKind::Unit; }
    // Only land regions have a reachability object (3.2) [H].
    bool has_reachability() const { return kind == TargetKind::Region && zone >= 0; }
};

// What the target list needs to know about an object (filled by the host / A2 from the object
// store and type data).
struct TargetObjectInfo {
    int32_t id = -1;
    bool dead_or_removing = false;          // obj +0x3a0 bits 0x40 / 0x80
    bool rep_has_land_evaluator = false;    // representative type (fleet / transport / team container) Has_Land_Evaluator
    bool rep_has_space_evaluator = false;   // representative type Has_Space_Evaluator
    bool own_has_space_evaluator = false;   // the object's own type Has_Space_Evaluator (space init, 3.1) [H]
};

std::string unit_target_name(int32_t object_id);         // "OBJECT_<id>"
std::string cell_target_name(int32_t x, int32_t y);      // "CELL_<x>_<y>"
// Perception region rect (slot 14, 3.2): square of side `region_size` centred on the target.
Rect2 region_rect(const TacticalTarget& t, float region_size);

class TacticalTargetList {
public:
    explicit TacticalTargetList(TacticalMode mode) : mode_(mode) {}
    TacticalMode mode() const { return mode_; }

    // ---- perception slot 6 (battle start), in this order ----
    // Every existing object in GOM object-list order. Land uses the runtime rule (slot 11,
    // representative type); space init tests the object's OWN type (0x6ba320) [H; keep both].
    // Returns the number added.
    uint32_t add_existing_objects(std::span<const TargetObjectInfo> objects_in_gom_order);
    // Land: one region per hint zone, appended in zone-list order. No-op in space. Returns count.
    uint32_t add_land_regions(std::span<const HintZone> zones);
    // Space: grid over `bounds` with square cells of `cell_size`; nx = ceil(width / size),
    // ny = ceil(height / size); x outer, y inner. Built only when `threat_grid_exists` [H].
    // No-op in land or when size <= 0. Returns the number of cells.
    uint32_t build_space_grid(const Rect2& bounds, float cell_size, bool threat_grid_exists);

    // ---- runtime ----
    // GOM add hook (slot 11): appends a unit target if the object is alive, its representative
    // type has the mode's evaluator flag and it is not registered yet. True if added.
    bool on_object_added(const TargetObjectInfo& obj);
    // Destroyed signal (0x6b96f0): erases the matching unit target keeping order. True if found.
    bool on_object_destroyed(int32_t object_id);

    std::span<const TacticalTarget> targets() const { return targets_; }
    size_t size() const { return targets_.size(); }
    const TacticalTarget& operator[](size_t i) const { return targets_[i]; }
    int32_t find_object(int32_t object_id) const;     // index or -1
    // First region with this name, case-insensitive (as the original's post-load relink 0x6b9580, which collapses
    // duplicate-named zones onto the first, TACTICAL_GAPS 6.8). Diagnostics only: nothing relinks by name in the port
    // (N1 saves keep each region's zone index).
    int32_t find_region(std::string_view name) const;
    bool is_registered(int32_t object_id) const;

    // CRC: mode, then every target in list order (kind, object id, name, position, rect, zone, cell).
    void hash(StateHasher& h) const;

    // N1 save / load: u32 version (1), u8 mode (must equal this list's), every target (all fields), the registered ids
    // (strictly ascending). False and unchanged on bad data; reads its own fields only.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    bool add_unit(int32_t object_id);

    TacticalMode mode_;
    std::vector<TacticalTarget> targets_;
    std::vector<int32_t> registered_;  // sorted object ids (lookup only; never defines an order)
};

}  // namespace alamo::ai::tactical

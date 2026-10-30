// Land passability (docs/re/MOVEMENT.md §1.1-1.3).
//
//   * One cell per 20-unit terrain vertex, cell (x, y) centred on world (20x, 20y); world -> cell is
//     floor((w + 10) / 20) on each axis (common.hpp world_to_cell).
//   * Each cell keeps a base value (.ted 0x10A, CellPassabilityType bits) and an effective value:
//     base | playable-box Obstacle | the OR of the objects stamped on it; any bit other than Clear
//     clears Clear (§1.1 steps 1-4).
//   * A unit may enter a cell iff every bit set in the cell is in its MovementClass mask:
//     (effective & mask) == effective. Out-of-range cells read 0xFFFFFFFF (never passable).
//   * LandObstacleBehavior footprints (§1.3) are sampled every 10 world units, rotated by the
//     object's yaw; the water rule skips submerged samples. Stamps are reference counted per bit
//     so unstamping restores exactly the previous effective values.
//   * OI tactical: secondary occupants (§1.1 step 3) - stamp_secondary() ORs an object's
//     SecondaryOccupationPassability into the cells it occupies as a secondary occupant (own list, so
//     one object can be a hard and a secondary occupant at once); Fine_Tune_Occupied_Passability (§1.3)
//     - a footprint with `fine_tune` set keeps only the samples whose vertical ray hits the object's
//     collision mesh (the caller supplies the ray test: the sim has no collision meshes yet).
// PROVISIONAL: cells outside the playable box test the CELL CENTRE strictly outside the box ([M] in
// the doc); which objects become secondary occupants and with what footprint is not traced (the
// vanilla users are the three base-shield generators, Base_Shield_Radius [inferred]: stamp_secondary_circle).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/tactical/movement/common.hpp"

namespace alamo::tactical::movement {

enum PassBits : uint32_t {
    kPassClear = 0x1, kPassSteep = 0x2, kPassObstacle = 0x4, kPassRebelWall = 0x8, kPassEmpireWall = 0x10,
    kPassWater = 0x20, kPassShield = 0x40, kPassInfantryOnly = 0x80, kPassImpassable = 0x100,
};
inline constexpr uint32_t kOutOfRangeCell = 0xFFFFFFFFu;

inline bool passable(uint32_t cell, uint32_t mask) { return (cell & mask) == cell; }

// MovementClassType (Enum\MovementClassType.xml) and its masks (MovementClassTypeDefs.xml, §1.2).
enum class MovementClass : int32_t {
    Infantry = 0, InfantryNoShield, Wheeled, WheeledNoShield, Walker, WalkerNoShield, Flying, Tracked,
    TrackedNoShield, Hover, HoverNoShield, LargeWalker, LargeWalkerNoShield, Space, Lancet, Spotter, LargeHover,
    BombingRun,
};
inline constexpr uint32_t kMovementClassCount = 18;

class MovementClassTable {
public:
    MovementClassTable();  // vanilla FoC table (§1.2)
    // Parses MovementClassTypeDefs.xml text (<Class> Bit | Bit </Class>; class names from
    // MovementClassType, bit names from CellPassabilityType, case-insensitive). Classes absent from
    // the file keep their current mask. False if the XML does not parse or a token is unknown.
    bool load_xml(std::string_view text);
    // Mask of a class value; an invalid class (-1 = no MovementClass) passes only pure Clear.
    uint32_t mask(int32_t movement_class) const;
    void set_mask(int32_t movement_class, uint32_t m);
    // Distinct masks of every class, ascending (the masks the region map precomputes).
    std::vector<uint32_t> distinct_masks() const;

private:
    std::array<uint32_t, kMovementClassCount> masks_{};
};

// Parses "Clear | Shield" (CellPassabilityType names). False on an unknown token.
bool parse_passability_bits(std::string_view text, uint32_t& out);

// LandObstacleBehavior footprint (§1.3). The rectangle is [x_offset, x_offset + width] x
// [y_offset, y_offset + height] in object space, rotated by yaw_deg about `position`.
struct ObstacleFootprint {
    V2 position;
    float yaw_deg = 0.0f;
    float width = 0.0f, height = 0.0f;
    float x_offset = 0.0f, y_offset = 0.0f;
    uint32_t bits = kPassObstacle;  // OverridePassability, or the behaviour default (Obstacle)
    float terrain_z = 0.0f;         // terrain height under the object (water rule)
    float obstacle_height = 0.0f;   // model bbox z * scale (water rule)
    // OI tactical: Fine_Tune_Occupied_Passability (§1.3): when set, a sample is stamped only if the vertical ray
    // (z +1000 -> -1000) at its world XY hits the object's collision mesh. Must be deterministic (const data).
    std::function<bool(V2 world_xy)> fine_tune;
};
// From the type fields: Obstacle_Width/Height/X/Y_Offset when set, else 2*|half extent|*scale and
// -half extent*scale (the caller supplies the model half extents; 0 when unknown).
ObstacleFootprint make_footprint(V2 position, float yaw_deg, float obstacle_width, float obstacle_height,
                                 float obstacle_x_offset, float obstacle_y_offset, float model_half_x,
                                 float model_half_y, float scale, uint32_t bits);

class PassabilityGrid {
public:
    // width x height cells, every base Clear (the pre-load state).
    void init(uint32_t width, uint32_t height);
    // Row-major (index = y * width + x) base values, size width * height (.ted MapData::passability).
    bool set_base(std::span<const uint32_t> row_major);
    void set_base(int32_t x, int32_t y, uint32_t v);
    // Playable box (world XY): cells whose centre lies strictly outside get Obstacle forced on.
    void set_playable_box(V2 min_xy, V2 max_xy);
    void clear_playable_box();
    // Water plane height for the stamping water rule (no water: leave unset).
    void set_water_height(float h) { water_height_ = h; has_water_ = true; }

    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    bool in_range(int32_t x, int32_t y) const {
        return x >= 0 && y >= 0 && static_cast<uint32_t>(x) < w_ && static_cast<uint32_t>(y) < h_;
    }
    uint32_t base(int32_t x, int32_t y) const { return in_range(x, y) ? base_[idx(x, y)] : kOutOfRangeCell; }
    uint32_t effective(int32_t x, int32_t y) const { return in_range(x, y) ? eff_[idx(x, y)] : kOutOfRangeCell; }
    bool passable_cell(int32_t x, int32_t y, uint32_t mask) const { return passable(effective(x, y), mask); }
    bool passable_world(V2 p, uint32_t mask) const { return passable_cell(world_to_cell(p.x), world_to_cell(p.y), mask); }
    std::span<const uint32_t> effective_values() const { return eff_; }

    // Dynamic stamping (§1.3). stamp() replaces a previous stamp of the same id. Returns the
    // number of distinct cells stamped.
    uint32_t stamp(ObjectId id, const ObstacleFootprint& fp);
    bool unstamp(ObjectId id);
    bool stamped(ObjectId id) const { return stamps_.find(id) != stamps_.end(); }
    // OI tactical: secondary occupants (§1.1 step 3). `fp.bits` = the type's SecondaryOccupationPassability. Same
    // sampling (and Fine_Tune / water rules) as stamp(); replaces a previous secondary stamp of the same id.
    uint32_t stamp_secondary(ObjectId id, const ObstacleFootprint& fp);
    // Secondary occupation of every cell whose centre lies within `radius` of `center` [inferred: base shields].
    uint32_t stamp_secondary_circle(ObjectId id, V2 center, float radius, uint32_t bits);
    bool unstamp_secondary(ObjectId id);
    bool secondary_stamped(ObjectId id) const { return secondary_.find(id) != secondary_.end(); }

    // Incremented whenever any effective value changes ("regions dirty", §1.1 step 5).
    uint64_t version() const { return version_; }

    // Q1 hasher: dimensions, effective values (row-major), stamped ids ascending; then the secondary stamps
    // (ids ascending) when there are any (OI tactical; the format is unchanged without secondary occupants).
    void hash(StateHasher& h) const;

    // N1 stage 2 save / load (contract: alamo/core/save_file.hpp; src/movement_save.cpp). Saved: version(), the stamps
    // and the secondary stamps (ids ascending: bits, distinct cells ascending). Checked, not restored: the dimensions
    // and static_digest() (base values, playable-box cells, water plane = the map-derived part): the host rebuilds it
    // from the map first (grid_from_map + the same setup), then read() replaces the whole stamp set (map obstacles
    // included) and recomputes the per-bit counts and the effective values, which are pure functions of base, box and
    // stamps. False (unchanged) on another map, bad ids / bits / cells, or truncated data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
    uint64_t static_digest() const;

private:
    size_t idx(int32_t x, int32_t y) const { return static_cast<size_t>(y) * w_ + static_cast<size_t>(x); }
    void recompute(size_t i);
    void recompute_all();

    uint32_t w_ = 0, h_ = 0;
    std::vector<uint32_t> base_, eff_;
    std::vector<uint8_t> forced_;  // 1 = outside the playable box
    // Per cell, per bit (9 CellPassabilityType bits): number of stamps; allocated on first stamp.
    std::vector<std::array<uint16_t, 9>> counts_;
    struct Stamp {
        std::vector<uint32_t> cells;  // distinct cell indices, ascending
        uint32_t bits = 0;
    };
    std::map<ObjectId, Stamp> stamps_;  // id-keyed, ordered
    std::map<ObjectId, Stamp> secondary_;  // OI tactical: secondary occupants, id-keyed, ordered
    void sample_cells(const ObstacleFootprint& fp, std::vector<uint32_t>& out) const;
    void apply(const Stamp& s, int delta);
    float water_height_ = 0.0f;
    bool has_water_ = false;
    uint64_t version_ = 0;
};

}  // namespace alamo::tactical::movement

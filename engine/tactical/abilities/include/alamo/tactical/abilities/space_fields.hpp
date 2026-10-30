// L1 systems - space fields: nebulae, ion storms and asteroid fields of a space battle (Lua Is_In_Nebula /
// Is_In_Ion_Storm / Is_In_Asteroid_Field, Is_Point_In_*, Find_Nearest_Space_Field; the S9 nebula / ion-storm inputs).
//
// A field is an object whose type sets Is_Asteroid_Field (+0x54) / Is_Impassable_Asteroid (+0x55) (asteroid),
// Is_Ion_Storm (+0x56) or Is_Nebula (+0x57) AND has a SPACE_OBSTACLE behaviour (0x4c): the circle at its position +
// Space_Obstacle_Offset (+0x16e4) with radius Space_Obstacle_Radius (+0x16f0) - the rule of the fog's dense mask
// (FOG_OF_WAR.md §1.3, 0x35e870) [H for the set; M: the same circle answers the "in field" queries]. A point is inside
// when its XY distance to the centre is <= the radius [M]. Find_Nearest_Space_Field: the field of the asked kinds
// whose centre is nearest in XY, ties by the lower field index (insertion order = the mode's object order) [port].
// Kinds use the Lua mask bits of script_remaining.hpp (asteroid 1, ion storm 2, nebula 4).
// Pure data; no draws; queries are const and thread-safe.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/tactical/combat/combat_system.hpp"

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::tactical::abilities {

// Is_Impassable_Asteroid fields carry kFieldImpassable only: the Lua tests / asteroid damage read Is_Asteroid_Field
// (+0x54) alone (L1_S4_REMAINING §1.6 / §3.3).
inline constexpr uint32_t kFieldAsteroid = 1u, kFieldIonStorm = 2u, kFieldNebula = 4u, kFieldImpassable = 8u;

struct SpaceField {
    combat::ObjectId id = combat::kNoObject;  // the field object
    uint32_t kinds = 0;                       // kField* bits
    float x = 0.0f, y = 0.0f;                 // centre (position + Space_Obstacle_Offset)
    float radius = 0.0f;                      // Space_Obstacle_Radius
};

class SpaceFieldIndex {
public:
    void clear() { fields_.clear(); }
    void add(const SpaceField& f) { fields_.push_back(f); }
    bool remove(combat::ObjectId id);  // a field object that left the mode
    std::span<const SpaceField> fields() const { return fields_; }
    bool empty() const { return fields_.empty(); }

    // Kinds of every field containing (x, y) (0 = none).
    uint32_t kinds_at(float x, float y) const;
    bool contains(float x, float y, uint32_t mask) const { return (kinds_at(x, y) & mask) != 0; }
    // The nearest field of `mask` (centre XY distance), -1 when none.
    int32_t nearest(float x, float y, uint32_t mask) const;

    void hash(StateHasher& h) const;
    // N1 stage 2 (save_file.hpp contract, ability_save.cpp): u32 version, u32 count, the fields in insertion order.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    std::vector<SpaceField> fields_;  // insertion order
};

}  // namespace alamo::tactical::abilities

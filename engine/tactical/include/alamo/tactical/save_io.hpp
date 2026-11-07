// N1 save helpers shared by the tactical save files (movement, combat, abilities, garrison, hangar). Header-only.
// Counts and nested sections use the core helpers (ByteReader::count, put_section / get_section in
// alamo/core/bytes.hpp); this adds the vector field codec, which serves both alamo::Vec3 (the object store's) and
// alamo::tactical::Vec3: three f32 bit patterns, x, y, z.
#pragma once
#include "alamo/core/bytes.hpp"

namespace alamo::tactical::save_io {

template <class V>
void put_vec3(ByteWriter& w, const V& v) {
    w.f32(v.x).f32(v.y).f32(v.z);
}

template <class V>
V get_vec3(ByteReader& r) {
    V v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

}  // namespace alamo::tactical::save_io

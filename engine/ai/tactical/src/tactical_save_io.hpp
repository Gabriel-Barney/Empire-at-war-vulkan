// N1 save / load helpers of the tactical AI (tactical_save.cpp, tactical_ai.cpp). Not installed. Byte rules of
// alamo/core/save_file.hpp: little-endian fields, counts before data, floats as bit patterns, no pointers / size_t.
#pragma once
#include <bit>
#include <cstdint>

#include "ai_save_io.hpp"  // engine/ai/src, through alamo_ai_save_io
#include "alamo/ai/tactical/hint_zones.hpp"
#include "alamo/ai/tactical/tactical_pp.hpp"
#include "alamo/ai/tactical/target_list.hpp"
#include "alamo/core/bytes.hpp"

namespace alamo::ai::tactical::save_io {

// Rejects the data (r.fail()) and returns false: semantic checks of the readers.
inline bool bad(ByteReader& r) {
    r.fail();
    return false;
}

// The u32 version that starts every record; false (r.fail()) on another value or truncation.
inline bool version(ByteReader& r, uint32_t want) {
    const uint32_t v = r.u32();
    return (r.ok() && v == want) || bad(r);
}

// The shared AI save codec (count, put_count, put / get, min_size, put_vec / get_vec): engine/ai/src/ai_save_io.hpp,
// exported to this library by the alamo_ai_save_io INTERFACE target. Same byte layout.
using ai::save::count;
using ai::save::get;
using ai::save::get_vec;
using ai::save::min_size;
using ai::save::put;
using ai::save::put_count;
using ai::save::put_vec;

inline void put_rect(ByteWriter& w, const Rect2& x) {
    w.f32(x.min_x);
    w.f32(x.min_y);
    w.f32(x.max_x);
    w.f32(x.max_y);
}

inline Rect2 get_rect(ByteReader& r) {
    Rect2 x;
    x.min_x = r.f32();
    x.min_y = r.f32();
    x.max_x = r.f32();
    x.max_y = r.f32();
    return x;
}

inline void put_vec3(ByteWriter& w, const Vec3f& v) {
    w.f32(v.x);
    w.f32(v.y);
    w.f32(v.z);
}

inline Vec3f get_vec3(ByteReader& r) {
    Vec3f v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

// TacticalTarget (target lists, AI target slots): u8 kind, i32 object id, name, position, evaluator rect, zone, cell x / y.
inline constexpr size_t kTargetBytes = 1 + 4 + 4 + 12 + 16 + 4 + 4 + 4;

inline void put_target(ByteWriter& w, const TacticalTarget& t) {
    w.u8(static_cast<uint8_t>(t.kind));
    w.i32(t.object_id);
    w.str(t.name);
    put_vec3(w, t.position);
    put_rect(w, t.eval_rect);
    w.i32(t.zone);
    w.i32(t.cell_x);
    w.i32(t.cell_y);
}

inline bool get_target(ByteReader& r, TacticalTarget& t) {
    const uint8_t kind = r.u8();
    if (kind > static_cast<uint8_t>(TargetKind::Region)) r.fail();
    t.kind = static_cast<TargetKind>(kind);
    t.object_id = r.i32();
    t.name = r.str();
    t.position = get_vec3(r);
    t.eval_rect = get_rect(r);
    t.zone = r.i32();
    t.cell_x = r.i32();
    t.cell_y = r.i32();
    return r.ok();
}

// TacticalPpPick (PotentialPlan picks): i32 type, u8 source, i32 ref, i32 task force, i32 team.
inline constexpr size_t kPickBytes = 4 + 1 + 4 + 4 + 4;

inline void put_pick(ByteWriter& w, const TacticalPpPick& k) {
    w.i32(k.type);
    w.u8(static_cast<uint8_t>(k.source));
    w.i32(k.ref);
    w.i32(k.tf);
    w.i32(k.team);
}

inline bool get_pick(ByteReader& r, TacticalPpPick& k) {
    k.type = r.i32();
    const uint8_t source = r.u8();
    if (source > static_cast<uint8_t>(PpSource::Produce)) r.fail();
    k.source = static_cast<PpSource>(source);
    k.ref = r.i32();
    k.tf = r.i32();
    k.team = r.i32();
    return r.ok();
}

// Bit-exact float equality (geometry checks: a reader built from the same data has the same bits).
inline bool same_bits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

}  // namespace alamo::ai::tactical::save_io

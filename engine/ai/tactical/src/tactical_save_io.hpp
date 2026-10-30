// N1 save / load helpers of the tactical AI (tactical_save.cpp, tactical_ai.cpp). Not installed. Byte rules of
// alamo/core/save_file.hpp: little-endian fields, counts before data, floats as bit patterns, no pointers / size_t.
#pragma once
#include <bit>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "alamo/ai/tactical/hint_zones.hpp"
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

// A count followed by at least `min_bytes` per element: rejected (r.fail(), 0) when the rest of the data cannot hold it.
inline uint32_t count(ByteReader& r, size_t min_bytes) {
    const uint32_t n = r.u32();
    if (!r.ok()) return 0;
    if (min_bytes > 0 && static_cast<uint64_t>(n) * min_bytes > r.remaining()) {
        r.fail();
        return 0;
    }
    return n;
}

inline void put_count(ByteWriter& w, size_t n) { w.u32(static_cast<uint32_t>(n)); }

template <class T>
void put(ByteWriter& w, const T& v) {
    if constexpr (std::is_same_v<T, bool>) w.boolean(v);
    else if constexpr (std::is_same_v<T, uint8_t>) w.u8(v);
    else if constexpr (std::is_same_v<T, int32_t>) w.i32(v);
    else if constexpr (std::is_same_v<T, uint32_t>) w.u32(v);
    else if constexpr (std::is_same_v<T, int64_t>) w.i64(v);
    else if constexpr (std::is_same_v<T, uint64_t>) w.u64(v);
    else if constexpr (std::is_same_v<T, float>) w.f32(v);
    else if constexpr (std::is_same_v<T, double>) w.f64(v);
    else {
        static_assert(std::is_same_v<T, std::string>);
        w.str(v);
    }
}

template <class T>
T get(ByteReader& r) {
    if constexpr (std::is_same_v<T, bool>) return r.boolean();
    else if constexpr (std::is_same_v<T, uint8_t>) return r.u8();
    else if constexpr (std::is_same_v<T, int32_t>) return r.i32();
    else if constexpr (std::is_same_v<T, uint32_t>) return r.u32();
    else if constexpr (std::is_same_v<T, int64_t>) return r.i64();
    else if constexpr (std::is_same_v<T, uint64_t>) return r.u64();
    else if constexpr (std::is_same_v<T, float>) return r.f32();
    else if constexpr (std::is_same_v<T, double>) return r.f64();
    else {
        static_assert(std::is_same_v<T, std::string>);
        return r.str();
    }
}

template <class T>
constexpr size_t min_size() {
    if constexpr (std::is_same_v<T, std::string>) return 4;
    else return sizeof(T);
}

template <class T>
void put_vec(ByteWriter& w, const std::vector<T>& v) {
    put_count(w, v.size());
    for (const T& x : v) put<T>(w, x);
}

template <class T>
bool get_vec(ByteReader& r, std::vector<T>& out) {
    const uint32_t n = count(r, min_size<T>());
    std::vector<T> v;
    v.reserve(n);
    for (uint32_t i = 0; i < n && r.ok(); ++i) v.push_back(get<T>(r));
    if (!r.ok()) return false;
    out = std::move(v);
    return true;
}

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

// Bit-exact float equality (geometry checks: a reader built from the same data has the same bits).
inline bool same_bits(float a, float b) { return std::bit_cast<uint32_t>(a) == std::bit_cast<uint32_t>(b); }

}  // namespace alamo::ai::tactical::save_io

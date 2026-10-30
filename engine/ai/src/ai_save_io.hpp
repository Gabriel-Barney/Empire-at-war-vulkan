// N1 save / load helpers shared by the AI save code (ai_save.cpp, galactic_ai_save.cpp, ai_lua_runtime.cpp). Not
// installed. Byte rules of alamo/core/save_file.hpp: little-endian fields, counts before data, no pointers / size_t.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/galactic/bitset.hpp"

namespace alamo::ai::save {

template <class T>
inline constexpr bool kSupported =
    std::is_same_v<T, bool> || std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t> || std::is_same_v<T, int32_t> ||
    std::is_same_v<T, uint32_t> || std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t> || std::is_same_v<T, float> ||
    std::is_same_v<T, double> || std::is_same_v<T, std::string>;

template <class T>
void put(ByteWriter& w, const T& v) {
    static_assert(kSupported<T>);
    if constexpr (std::is_same_v<T, bool>) w.boolean(v);
    else if constexpr (std::is_same_v<T, uint8_t>) w.u8(v);
    else if constexpr (std::is_same_v<T, int8_t>) w.u8(static_cast<uint8_t>(v));
    else if constexpr (std::is_same_v<T, int32_t>) w.i32(v);
    else if constexpr (std::is_same_v<T, uint32_t>) w.u32(v);
    else if constexpr (std::is_same_v<T, int64_t>) w.i64(v);
    else if constexpr (std::is_same_v<T, uint64_t>) w.u64(v);
    else if constexpr (std::is_same_v<T, float>) w.f32(v);
    else if constexpr (std::is_same_v<T, double>) w.f64(v);
    else w.str(v);
}

template <class T>
T get(ByteReader& r) {
    static_assert(kSupported<T>);
    if constexpr (std::is_same_v<T, bool>) return r.boolean();
    else if constexpr (std::is_same_v<T, uint8_t>) return r.u8();
    else if constexpr (std::is_same_v<T, int8_t>) return static_cast<int8_t>(r.u8());
    else if constexpr (std::is_same_v<T, int32_t>) return r.i32();
    else if constexpr (std::is_same_v<T, uint32_t>) return r.u32();
    else if constexpr (std::is_same_v<T, int64_t>) return r.i64();
    else if constexpr (std::is_same_v<T, uint64_t>) return r.u64();
    else if constexpr (std::is_same_v<T, float>) return r.f32();
    else if constexpr (std::is_same_v<T, double>) return r.f64();
    else return r.str();
}

// Smallest encoded size of one T (count sanity check on corrupted data).
template <class T>
constexpr size_t min_size() {
    if constexpr (std::is_same_v<T, std::string>) return 4;
    else return sizeof(T);
}

// A count followed by at least `min_bytes` per element: rejected (r.fail()) when the rest of the data cannot hold it.
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

template <class T>
void put_set(ByteWriter& w, const std::set<T>& s) {
    put_count(w, s.size());
    for (const T& x : s) put<T>(w, x);
}

// Strictly ascending keys (the canonical std::set order); anything else is invalid data.
template <class T>
bool get_set(ByteReader& r, std::set<T>& out) {
    const uint32_t n = count(r, min_size<T>());
    std::set<T> s;
    for (uint32_t i = 0; i < n && r.ok(); ++i) {
        T x = get<T>(r);
        if (!s.empty() && !(*s.rbegin() < x)) r.fail();
        s.insert(s.end(), std::move(x));
    }
    if (!r.ok()) return false;
    out = std::move(s);
    return true;
}

// Bit count, then the 64-bit words; bits past the size must be clear.
inline void put_bitset(ByteWriter& w, const galactic::Bitset& b) {
    put_count(w, b.size());
    for (const uint64_t word : b.words()) w.u64(word);
}

inline bool get_bitset(ByteReader& r, galactic::Bitset& out) {
    const uint32_t bits = r.u32();
    if (!r.ok()) return false;
    const uint64_t words = (static_cast<uint64_t>(bits) + 63u) / 64u;
    if (words * 8u > r.remaining()) {
        r.fail();
        return false;
    }
    galactic::Bitset b(bits);
    for (uint64_t k = 0; k < words; ++k) {
        const uint64_t word = r.u64();
        const uint64_t base = k * 64u;
        const uint64_t valid = bits - base >= 64u ? 64u : bits - base;
        if (valid < 64u && (word >> valid) != 0u) r.fail();
        for (uint64_t i = 0; i < valid; ++i)
            if ((word >> i) & 1u) b.set(static_cast<size_t>(base + i));
    }
    if (!r.ok()) return false;
    out = std::move(b);
    return true;
}

}  // namespace alamo::ai::save

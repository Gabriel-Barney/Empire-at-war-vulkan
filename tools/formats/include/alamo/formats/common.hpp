// alamo_formats - clean-room parsers for Alamo engine (Empire at War / Forces of Corruption) assets.
// Common primitive types, error type and a bounds-checked little-endian byte reader.
#pragma once

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

static_assert(std::endian::native == std::endian::little,
              "alamo_formats assumes a little-endian host (all Alamo data is little-endian)");

namespace alamo {

using Bytes = std::span<const std::uint8_t>;

struct Vec2 { float x = 0, y = 0; };
// Also defined (token-identical, same guard) in engine/core galaxy.hpp, so both headers can share a TU.
#ifndef ALAMO_VEC3_DEFINED
#define ALAMO_VEC3_DEFINED
struct Vec3 { float x = 0, y = 0, z = 0; };
#endif
struct Vec4 { float x = 0, y = 0, z = 0, w = 0; };
// Quaternion, component order (x, y, z, w) as stored in ALA files.
struct Quat { float x = 0, y = 0, z = 0, w = 1; };

// Row-major 3x4 affine matrix exactly as stored in ALO bone chunks:
//   m[r][0..2] = rotation/scale row r, m[r][3] = translation component r.
// A point p transforms as p'[r] = m[r][0]*p.x + m[r][1]*p.y + m[r][2]*p.z + m[r][3].
struct Mat34 {
    float m[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    static Mat34 identity() { return {}; }
    Vec3 transform_point(const Vec3& p) const;
    Vec3 transform_dir(const Vec3& p) const;
    Mat34 operator*(const Mat34& rhs) const;  // this * rhs  (apply rhs first)
    Mat34 inverse() const;                     // general affine inverse
};

struct AABB { Vec3 min, max; };

// Thrown for any malformed input. `offset` is the absolute file offset where the problem was found.
class FormatError : public std::runtime_error {
public:
    FormatError(const std::string& what, std::size_t offset)
        : std::runtime_error(what + " (at offset " + std::to_string(offset) + ")"), offset_(offset) {}
    std::size_t offset() const noexcept { return offset_; }
private:
    std::size_t offset_;
};

// Bounds-checked sequential reader over a byte span. `base` is the absolute offset of data[0]
// in the file, used only for error messages.
// In an inline namespace: engine/core has an unrelated alamo::ByteReader (core/bytes.hpp); the distinct mangled
// name keeps their members from colliding at link time (LNK2005) in programs that link both (alamo_render --sim).
inline namespace formats_io {
class ByteReader {
public:
    ByteReader(Bytes data, std::size_t base = 0) : data_(data), base_(base) {}

    std::size_t pos() const { return pos_; }
    std::size_t size() const { return data_.size(); }
    std::size_t remaining() const { return data_.size() - pos_; }
    bool eof() const { return pos_ >= data_.size(); }
    std::size_t abs_pos() const { return base_ + pos_; }
    void seek(std::size_t p) {
        if (p > data_.size()) throw FormatError("seek out of range", base_ + p);
        pos_ = p;
    }
    void skip(std::size_t n) { need(n); pos_ += n; }

    template <class T> T read() {
        static_assert(std::is_trivially_copyable_v<T>);
        need(sizeof(T));
        T v;
        std::memcpy(&v, data_.data() + pos_, sizeof(T));
        pos_ += sizeof(T);
        return v;
    }
    std::uint8_t u8() { return read<std::uint8_t>(); }
    std::uint16_t u16() { return read<std::uint16_t>(); }
    std::int16_t i16() { return read<std::int16_t>(); }
    std::uint32_t u32() { return read<std::uint32_t>(); }
    std::int32_t i32() { return read<std::int32_t>(); }
    float f32() { return read<float>(); }
    Vec2 vec2() { Vec2 v; v.x = f32(); v.y = f32(); return v; }
    Vec3 vec3() { Vec3 v; v.x = f32(); v.y = f32(); v.z = f32(); return v; }
    Vec4 vec4() { Vec4 v; v.x = f32(); v.y = f32(); v.z = f32(); v.w = f32(); return v; }
    Bytes bytes(std::size_t n) { need(n); Bytes b = data_.subspan(pos_, n); pos_ += n; return b; }
    // Fixed-size field holding a NUL-terminated (possibly NUL-padded) ASCII string.
    std::string fixed_string(std::size_t n);

private:
    void need(std::size_t n) const {
        if (n > remaining())
            throw FormatError("unexpected end of data (need " + std::to_string(n) + " bytes, have " +
                                  std::to_string(remaining()) + ")",
                              base_ + pos_);
    }
    Bytes data_;
    std::size_t base_ = 0;
    std::size_t pos_ = 0;
};
}  // namespace formats_io

// Upper-case hex without prefix, e.g. 0x10007 -> "10007".
std::string hex_id(std::uint32_t id);

// Interpret a byte span as a NUL-terminated string (stops at the first NUL or the end of the span).
std::string to_cstring(Bytes b);

// Read an entire file into memory. Throws std::runtime_error on I/O failure.
std::vector<std::uint8_t> read_file(const std::filesystem::path& path);

// Standard CRC-32 (IEEE 802.3, reflected polynomial 0xEDB88320) as used by Alamo for
// MEG file names and string-table keys (callers upper-case the input first).
std::uint32_t crc32(std::string_view data);
std::uint32_t crc32(Bytes data);

}  // namespace alamo

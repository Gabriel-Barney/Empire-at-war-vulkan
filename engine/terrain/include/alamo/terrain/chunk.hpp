// alamo_terrain - bounds-checked chunk / mini-chunk walker for .ted maps (SPEC.md 1).
//
//   chunk: u32 id, u32 size (bit 31 = payload is child chunks), payload[size]
//   mini : u8 id, u8 size, payload[size]   (fills a leaf exactly)
//
// Every function treats its input as untrusted: nothing reads outside the span, malformed input
// returns false. No exceptions, no allocation beyond the output vectors.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace alamo::terrain {

struct ChunkView {
    uint32_t id = 0;
    bool has_children = false;
    std::span<const uint8_t> payload;
};
// Splits `bytes` into a chunk sequence. False when the sequence does not end exactly at the end.
bool read_chunks(std::span<const uint8_t> bytes, std::vector<ChunkView>& out);

struct MiniView {
    uint8_t id = 0;
    std::span<const uint8_t> payload;
};
// Splits a leaf into mini-chunks. False when they do not fill the leaf exactly.
bool read_minis(std::span<const uint8_t> leaf, std::vector<MiniView>& out);

// Typed mini payload readers. They fail (return false / leave the output) on short payloads and,
// for floats, on non-finite values, so a corrupt map can never inject NaN/Inf into the outputs.
bool mini_f32(const MiniView& m, float& v);
bool mini_f32x3(const MiniView& m, float (&v)[3]);
// u32 from a 4-byte payload; 1- and 2-byte payloads are zero-extended (the editor writes bools as
// u8). Empty payload -> false.
bool mini_u32(const MiniView& m, uint32_t& v);
uint32_t mini_u32_or(const MiniView& m, uint32_t fallback);
// 8-bit string up to the first NUL (or the payload end).
std::string mini_string(const MiniView& m);
// UTF-16LE string up to the first NUL, converted to UTF-8 (unpaired surrogates -> U+FFFD).
std::string mini_utf16(const MiniView& m);

// Raw copy of a mini-chunk (kept for fields whose meaning is not established, and for writing).
struct RawMini {
    uint8_t id = 0;
    std::vector<uint8_t> data;
    friend bool operator==(const RawMini&, const RawMini&) = default;
};
std::vector<RawMini> copy_minis(std::span<const MiniView> minis);
const RawMini* find_mini(const std::vector<RawMini>& minis, uint8_t id);
bool raw_f32(const std::vector<RawMini>& minis, uint8_t id, float& v);
bool raw_f32x3(const std::vector<RawMini>& minis, uint8_t id, float (&v)[3]);

// ---- writer side (used by write_ted and by tests that build synthetic maps) ----
class ChunkWriter {
public:
    // Opens a node chunk (children follow until end()).
    void begin(uint32_t id);
    void end();
    void leaf(uint32_t id, std::span<const uint8_t> payload);
    // Leaf of mini-chunks.
    void mini_leaf(uint32_t id, std::span<const RawMini> minis);
    std::vector<uint8_t>& bytes() { return out_; }
    std::vector<uint8_t> take();

private:
    std::vector<uint8_t> out_;
    std::vector<size_t> open_;
};

// Mini builders.
RawMini mk_f32(uint8_t id, float v);
RawMini mk_f32x3(uint8_t id, const float (&v)[3]);
RawMini mk_u32(uint8_t id, uint32_t v);
RawMini mk_u8(uint8_t id, uint8_t v);
RawMini mk_string(uint8_t id, const std::string& s);   // NUL-terminated, truncated to 254 chars
RawMini mk_utf16(uint8_t id, const std::string& utf8); // ASCII/BMP only, NUL-terminated, <= 126 units

}  // namespace alamo::terrain

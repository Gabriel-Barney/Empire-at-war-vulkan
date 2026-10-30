// Engine save files (N1 save / load): a header plus named, versioned chunks. This is the new engine's own
// format; loading the original game's saves is N3.
//
// SYSTEM CONTRACT (every module that saves state follows it):
//   * `void write(ByteWriter& w) const` writes the system's state; `bool read(ByteReader& r)` restores it.
//     read() returns false and leaves the system unchanged on a version mismatch, an invalid value or
//     truncated data, and requires r.done() at the end of its own chunk.
//   * Definitions (XML types, plots, maps, scripts' source) are not saved: the loader rebuilds the session
//     from the same data first, then reads the state over it.
//   * After a successful read the system's CRC hash equals the saved system's, and continuing the sim gives
//     the same per-tick CRCs as the original run (N1 done-check). So write() covers at least everything the
//     CrcRegistry hasher covers, plus whatever later ticks read that the hasher leaves out (caches that are
//     not rebuilt, pending queues, RNG state, id / serial counters).
//   * Same byte rules as the hasher: values field by field, little-endian, counts before variable-length
//     data, canonical (id-ascending / deterministic) order, never pointers, never size_t.
//
// FILE LAYOUT (format 2, little-endian): "ALSV" magic (u32 0x56534c41), u32 format version, u64 tick, str
// description (both readable without unpacking), u8 body encoding (0 stored, 1 LZ - alamo/core/compress.hpp), u32 body
// size, u32 packed size, the packed body; last a u64 StateHasher digest of every byte before it (so any damaged or
// missing byte is refused before the body is unpacked). The body: u32 chunk count, then per chunk: str name, u32
// chunk version, u32 byte length, bytes. Chunk names match the system's CrcRegistry name where there is one
// ("galactic.state", "story", ...). encode() stores the body LZ-packed unless that is not smaller; the packing is a pure
// function of the body, so equal saves encode to equal bytes. Format 1 (the body unpacked, no encoding / size fields)
// is refused.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/bytes.hpp"

namespace alamo {

inline constexpr uint32_t kSaveMagic = 0x56534c41u;  // "ALSV"
inline constexpr uint32_t kSaveFormatVersion = 2;  // 2: packed body (N1 save size)

struct SaveChunk {
    std::string name;
    uint32_t version = 0;
    std::vector<uint8_t> data;
};

class SaveFile {
public:
    uint64_t tick = 0;        // sim tick the state was taken after
    std::string description;  // shown in the load dialog

    // Adds a chunk. Names: non-empty, no whitespace, unique (throws std::invalid_argument).
    void add(std::string name, uint32_t version, const ByteWriter& w);
    void add(std::string name, uint32_t version, std::vector<uint8_t> data);
    // Null when there is no such chunk.
    const SaveChunk* find(std::string_view name) const;
    const std::vector<SaveChunk>& chunks() const noexcept { return chunks_; }

    std::vector<uint8_t> encode() const;
    // False + *err on a bad magic, an unknown format version, a digest mismatch, a body that does not unpack or
    // truncated data; `out` is then unchanged.
    static bool decode(std::span<const uint8_t> bytes, SaveFile& out, std::string* err = nullptr);

    bool write_file(const std::string& path, std::string* err = nullptr) const;
    static bool read_file(const std::string& path, SaveFile& out, std::string* err = nullptr);

private:
    std::vector<SaveChunk> chunks_;
};

}  // namespace alamo

// Generic Alamo chunk container.
//
//   chunk      := u32 id, u32 size_and_flag, payload[size]
//                 size = size_and_flag & 0x7FFFFFFF
//                 bit 31 set   -> payload is a sequence of child chunks
//                 bit 31 clear -> payload is raw data (often a sequence of mini-chunks)
//   mini-chunk := u8 id, u8 size, payload[size]      (only inside leaf chunks)
//
// A file is simply a sequence of top-level chunks.
#pragma once

#include "alamo/formats/common.hpp"

#include <optional>
#include <string>
#include <vector>

namespace alamo {

class ChunkCursor;
class MiniChunkCursor;

struct Chunk {
    std::uint32_t id = 0;
    bool has_children = false;
    Bytes data;                 // payload (excluding the 8-byte header)
    std::size_t offset = 0;     // absolute file offset of the chunk header
    std::size_t data_offset() const { return offset + 8; }
    ChunkCursor children() const;
    MiniChunkCursor minis() const;
    ByteReader reader() const { return ByteReader(data, data_offset()); }
    std::string as_string() const { return to_cstring(data); }
};

// Iterates sibling chunks in a byte range. Throws FormatError on a chunk that overruns its parent.
class ChunkCursor {
public:
    ChunkCursor(Bytes data, std::size_t base = 0) : data_(data), base_(base) {}
    std::optional<Chunk> next();
    bool done() const { return pos_ >= data_.size(); }
    // Collect all remaining chunks.
    std::vector<Chunk> all();
private:
    Bytes data_;
    std::size_t base_;
    std::size_t pos_ = 0;
};

struct MiniChunk {
    std::uint8_t id = 0;
    Bytes data;
    std::size_t offset = 0;  // absolute offset of the mini-chunk header
    ByteReader reader() const { return ByteReader(data, offset + 2); }
    std::uint8_t as_u8() const;
    std::uint32_t as_u32() const;
    float as_float() const;
    Vec3 as_vec3() const;
    Vec4 as_vec4() const;
    std::string as_string() const { return to_cstring(data); }
};

class MiniChunkCursor {
public:
    MiniChunkCursor(Bytes data, std::size_t base = 0) : data_(data), base_(base) {}
    std::optional<MiniChunk> next();
    std::vector<MiniChunk> all();
private:
    Bytes data_;
    std::size_t base_;
    std::size_t pos_ = 0;
};

// Returns true if `data` decomposes exactly into a sequence of mini-chunks (heuristic used by the
// generic dumper/validator; leaf chunks that are known to be mini-chunk lists do not need it).
bool looks_like_minichunks(Bytes data);

// Fully materialised chunk tree, used by the validator and generic dumps.
struct ChunkNode {
    std::uint32_t id = 0;
    bool has_children = false;
    std::size_t offset = 0;
    std::size_t size = 0;
    std::vector<ChunkNode> children;
};
std::vector<ChunkNode> parse_chunk_tree(Bytes file);

ChunkCursor top_level_chunks(Bytes file);

// ---- Known chunk catalogue ----------------------------------------------------------------------
// Paths are '/'-separated hex chunk ids from the file root, e.g. "400/10000/10007".
// Returns a human-readable name, or nullptr if the path is not in the verified catalogue.
const char* known_chunk_name(std::string_view path);
// Returns the list of all catalogued (path, name) pairs (for docs / tooling).
struct KnownChunk { const char* path; const char* name; bool verified; };
std::span<const KnownChunk> known_chunks();

}  // namespace alamo

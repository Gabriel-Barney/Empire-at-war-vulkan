#include "alamo/formats/chunk.hpp"

#include <array>

namespace alamo {

ChunkCursor Chunk::children() const { return ChunkCursor(data, data_offset()); }
MiniChunkCursor Chunk::minis() const { return MiniChunkCursor(data, data_offset()); }

std::optional<Chunk> ChunkCursor::next() {
    if (pos_ >= data_.size()) return std::nullopt;
    if (data_.size() - pos_ < 8)
        throw FormatError("truncated chunk header (" + std::to_string(data_.size() - pos_) +
                              " trailing bytes)",
                          base_ + pos_);
    ByteReader r(data_.subspan(pos_, 8), base_ + pos_);
    Chunk c;
    c.id = r.u32();
    const std::uint32_t raw = r.u32();
    c.has_children = (raw & 0x80000000u) != 0;
    const std::size_t size = raw & 0x7FFFFFFFu;
    c.offset = base_ + pos_;
    if (size > data_.size() - pos_ - 8)
        throw FormatError("chunk 0x" + hex_id(c.id) + " size " + std::to_string(size) + " overruns parent",
                          c.offset);
    c.data = data_.subspan(pos_ + 8, size);
    pos_ += 8 + size;
    return c;
}

std::vector<Chunk> ChunkCursor::all() {
    std::vector<Chunk> v;
    while (auto c = next()) v.push_back(*c);
    return v;
}

std::optional<MiniChunk> MiniChunkCursor::next() {
    if (pos_ >= data_.size()) return std::nullopt;
    if (data_.size() - pos_ < 2) throw FormatError("truncated mini-chunk header", base_ + pos_);
    MiniChunk m;
    m.id = data_[pos_];
    const std::size_t size = data_[pos_ + 1];
    m.offset = base_ + pos_;
    if (size > data_.size() - pos_ - 2)
        throw FormatError("mini-chunk 0x" + hex_id(m.id) + " overruns chunk", m.offset);
    m.data = data_.subspan(pos_ + 2, size);
    pos_ += 2 + size;
    return m;
}

std::vector<MiniChunk> MiniChunkCursor::all() {
    std::vector<MiniChunk> v;
    while (auto m = next()) v.push_back(*m);
    return v;
}

std::uint8_t MiniChunk::as_u8() const {
    if (data.size() < 1) throw FormatError("mini-chunk too small for u8", offset);
    return data[0];
}
std::uint32_t MiniChunk::as_u32() const {
    if (data.size() == 1) return data[0];
    if (data.size() == 2) return reader().u16();
    if (data.size() != 4) throw FormatError("mini-chunk is not a u32 (size " + std::to_string(data.size()) + ")", offset);
    return reader().u32();
}
float MiniChunk::as_float() const {
    if (data.size() != 4) throw FormatError("mini-chunk is not a float", offset);
    return reader().f32();
}
Vec3 MiniChunk::as_vec3() const {
    if (data.size() != 12) throw FormatError("mini-chunk is not a float3", offset);
    return reader().vec3();
}
Vec4 MiniChunk::as_vec4() const {
    if (data.size() != 16) throw FormatError("mini-chunk is not a float4", offset);
    return reader().vec4();
}

bool looks_like_minichunks(Bytes d) {
    std::size_t p = 0;
    if (d.empty()) return false;
    while (p + 2 <= d.size()) p += 2 + d[p + 1];
    return p == d.size();
}

ChunkCursor top_level_chunks(Bytes file) { return ChunkCursor(file, 0); }

namespace {
void build(ChunkCursor cur, std::vector<ChunkNode>& out) {
    while (auto c = cur.next()) {
        ChunkNode n;
        n.id = c->id;
        n.has_children = c->has_children;
        n.offset = c->offset;
        n.size = c->data.size();
        if (c->has_children) build(c->children(), n.children);
        out.push_back(std::move(n));
    }
}
}  // namespace

std::vector<ChunkNode> parse_chunk_tree(Bytes file) {
    std::vector<ChunkNode> v;
    build(ChunkCursor(file, 0), v);
    return v;
}

// ---- catalogue ----
namespace {
constexpr KnownChunk kKnown[] = {
    // ALO model
    {"200", "skeleton", true},
    {"200/201", "skeleton header (u32 bone count + 124 pad)", true},
    {"200/202", "bone", true},
    {"200/202/203", "bone name", true},
    {"200/202/205", "bone v1 (parent, visible, 3x4 matrix)", false},
    {"200/202/206", "bone v2 (parent, visible, billboard, 3x4 matrix)", true},
    {"400", "mesh", true},
    {"400/401", "mesh name", true},
    {"400/402", "mesh info (submesh count, bbox, unknown, hidden, collision)", true},
    {"400/10100", "submesh material", true},
    {"400/10100/10101", "shader name", true},
    {"400/10100/10102", "int parameter", true},
    {"400/10100/10103", "float parameter", true},
    {"400/10100/10104", "float3 parameter", true},
    {"400/10100/10105", "texture parameter", true},
    {"400/10100/10106", "float4 parameter", true},
    {"400/10000", "submesh data", true},
    {"400/10000/10001", "submesh info (vertex count, primitive count)", true},
    {"400/10000/10002", "vertex format name", true},
    {"400/10000/10004", "index buffer (u16)", true},
    {"400/10000/10005", "vertex buffer (legacy, 128-byte vertices)", true},
    {"400/10000/10006", "skin bone map (u32)", true},
    {"400/10000/10007", "vertex buffer (144-byte vertices)", true},
    {"400/10000/1200", "collision tree", true},
    {"400/10000/1200/1201", "collision tree header (bbox, node/tri counts)", true},
    {"400/10000/1200/1202", "collision tree nodes (10 bytes each)", true},
    {"400/10000/1200/1203", "collision tree triangle list (u16)", true},
    {"1300", "light", true},
    {"1300/1301", "light name", true},
    {"1300/1302", "light info", true},
    {"600", "connections", true},
    {"600/601", "connection counts", true},
    {"600/602", "object connection", true},
    {"600/603", "proxy", true},
    {"600/604", "dazzle (unverified)", false},
    // ALO particle system
    {"900", "particle system", true},
    {"900/0", "system name", true},
    {"900/1", "particle-editor system ID (u32)", true},
    {"900/2", "system u8 leave particles when detached", true},
    {"900/800", "emitter list", true},
    {"900/800/700", "emitter", true},
    {"900/800/700/2", "emitter property block (mini-chunks, see particle::emitter_fields)", true},
    {"900/800/700/3", "color texture name", true},
    {"900/800/700/16", "emitter name", true},
    {"900/800/700/29", "spawn shapes (velocity, lifetime, position)", true},
    {"900/800/700/29/1100", "spawn shape", true},
    {"900/800/700/29/1100/1101", "spawn shape record (64 bytes)", true},
    {"900/800/700/1", "tracks", true},
    {"900/800/700/1/0", "track header", true},
    {"900/800/700/1/1", "track keys", true},
    {"900/800/700/36", "emitter links (37 spawn-on-death, 39 spawn-during-life)", true},
    {"900/800/700/45", "bump texture name", true},
    // ALA
    {"1000", "animation", true},
    {"1000/1001", "animation info", true},
    {"1000/1002", "bone animation", true},
    {"1000/1002/1003", "bone animation info", true},
    {"1000/1002/1004", "per-bone translation track (u16x3/frame)", true},
    {"1000/1002/1005", "per-bone track, 1-byte elements (not observed)", false},
    {"1000/1002/1006", "per-bone rotation track (i16x4, const or per frame)", true},
    {"1000/1002/1007", "visibility bitfield", true},
    {"1000/1002/1008", "step-frame bitfield", true},
    {"1000/1009", "rotation block data (i16)", true},
    {"1000/100A", "translation block data (u16)", true},
    {"1000/100B", "scale block data (u16; not observed)", false},
    // TEM / TEE
    {"2", "terrain material set", true},
    {"2/3", "terrain material (mini-chunks)", true},
    {"5", "environment set end marker (empty)", true},
    {"6", "environment / lighting record (mini-chunks)", true},
};
}  // namespace

std::span<const KnownChunk> known_chunks() { return kKnown; }

const char* known_chunk_name(std::string_view path) {
    for (const auto& k : kKnown)
        if (path == k.path) return k.name;
    return nullptr;
}

}  // namespace alamo

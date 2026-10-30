#include "alamo/formats/ala.hpp"

#include <algorithm>
#include <cmath>

namespace alamo::ala {

namespace {

template <class T>
std::vector<T> read_array(const Chunk& c) {
    if (c.data.size() % sizeof(T)) throw FormatError("array chunk size not a multiple of element size", c.offset);
    std::vector<T> v(c.data.size() / sizeof(T));
    if (!v.empty()) std::memcpy(v.data(), c.data.data(), c.data.size());
    return v;
}

Quat quat_from_i16(const std::int16_t* q) {
    Quat r{q[0] / 32767.f, q[1] / 32767.f, q[2] / 32767.f, q[3] / 32767.f};
    const float n = std::sqrt(r.x * r.x + r.y * r.y + r.z * r.z + r.w * r.w);
    if (n > 1e-8f) { r.x /= n; r.y /= n; r.z /= n; r.w /= n; }
    else r = Quat{};
    return r;
}

bool bit(const std::vector<std::uint8_t>& bits, std::uint32_t i) {
    return (i >> 3) < bits.size() && ((bits[i >> 3] >> (i & 7)) & 1);
}

BoneTrack parse_bone(const Chunk& c, Animation& a) {
    BoneTrack b;
    bool info = false;
    auto cur = c.children();
    while (auto ch = cur.next()) {
        switch (ch->id) {
            case 0x1003: {
                info = true;
                auto mc = ch->minis();
                while (auto m = mc.next()) {
                    switch (m->id) {
                        case 0x04: b.name = m->as_string(); break;
                        case 0x05: b.bone_index = m->as_u32(); break;
                        case 0x0A: b.overlay_excluded = m->as_u32(); break;
                        case 0x06: b.translation_offset = m->as_vec3(); break;
                        case 0x07: b.translation_scale = m->as_vec3(); break;
                        case 0x08: b.scale_offset = m->as_vec3(); break;
                        case 0x09: b.scale_scale = m->as_vec3(); break;
                        case 0x0E: b.translation_index = static_cast<std::uint16_t>(m->as_u32()); break;
                        case 0x0F: b.scale_index = static_cast<std::uint16_t>(m->as_u32()); break;
                        case 0x10: b.rotation_index = static_cast<std::uint16_t>(m->as_u32()); break;
                        case 0x11: {
                            if (m->data.size() != 8) throw FormatError("default rotation not 8 bytes", m->offset);
                            std::int16_t q[4];
                            std::memcpy(q, m->data.data(), 8);
                            b.default_rotation = quat_from_i16(q);
                            break;
                        }
                        default: break;
                    }
                }
                break;
            }
            case 0x1004: b.translation_data = read_array<std::uint16_t>(*ch); break;
            case 0x1005: b.scale_data = read_array<std::uint16_t>(*ch); break;
            case 0x1006: b.rotation_data = read_array<std::int16_t>(*ch); break;
            case 0x1007: b.visibility.assign(ch->data.begin(), ch->data.end()); break;
            case 0x1008: b.step_frames.assign(ch->data.begin(), ch->data.end()); break;
            default: break;
        }
    }
    if (!info) throw FormatError("bone animation without info chunk 0x1003", c.offset);
    const std::size_t nf = a.num_frames;
    const std::size_t bits = (nf + 7) / 8;
    if (!b.visibility.empty() && b.visibility.size() != bits)
        throw FormatError("visibility bitfield size mismatch", c.offset);
    if (!b.step_frames.empty() && b.step_frames.size() != bits)
        throw FormatError("step bitfield size mismatch", c.offset);
    if (!b.translation_data.empty() && b.translation_data.size() != nf * 3)
        throw FormatError("per-bone translation track size mismatch", c.offset);
    if (!b.scale_data.empty() && b.scale_data.size() != nf * 3)
        throw FormatError("per-bone scale track size mismatch", c.offset);
    if (!b.rotation_data.empty() && b.rotation_data.size() != 4 && b.rotation_data.size() != nf * 4)
        throw FormatError("per-bone rotation track size mismatch", c.offset);
    return b;
}

}  // namespace

Animation parse(Bytes data) {
    Animation a;
    auto top = top_level_chunks(data);
    auto root = top.next();
    if (!root || root->id != 0x1000 || !root->has_children) throw FormatError("not an animation (chunk 0x1000)", 0);
    if (auto extra = top.next()) throw FormatError("unexpected chunk after animation", extra->offset);
    bool info = false, block_layout = false;
    auto cur = root->children();
    while (auto ch = cur.next()) {
        switch (ch->id) {
            case 0x1001: {
                info = true;
                auto mc = ch->minis();
                while (auto m = mc.next()) {
                    switch (m->id) {
                        case 0x01: a.num_frames = m->as_u32(); break;
                        case 0x02: a.fps = m->as_float(); break;
                        case 0x03: a.num_bones = m->as_u32(); break;
                        case 0x0B: a.rotation_block = m->as_u32(); block_layout = true; break;
                        case 0x0C: a.translation_block = m->as_u32(); block_layout = true; break;
                        case 0x0D: a.scale_block = m->as_u32(); block_layout = true; break;
                        default: break;
                    }
                }
                a.layout = block_layout ? Layout::Block : Layout::PerBone;
                break;
            }
            case 0x1002:
                if (!info) throw FormatError("bone before animation info", ch->offset);
                a.bones.push_back(parse_bone(*ch, a));
                break;
            case 0x1009: a.rotation_data = read_array<std::int16_t>(*ch); break;
            case 0x100A: a.translation_data = read_array<std::uint16_t>(*ch); break;
            case 0x100B: a.scale_data = read_array<std::uint16_t>(*ch); break;
            default: break;
        }
    }
    if (!info) throw FormatError("animation without info chunk", root->offset);
    if (a.bones.size() != a.num_bones)
        throw FormatError("animation bone count mismatch: header " + std::to_string(a.num_bones) + ", found " +
                              std::to_string(a.bones.size()), root->offset);
    const std::size_t nf = a.num_frames;
    if (a.layout == Layout::Block) {
        if (a.rotation_data.size() != nf * a.rotation_block)
            throw FormatError("rotation block data size mismatch", root->offset);
        if (a.translation_data.size() != nf * a.translation_block)
            throw FormatError("translation block data size mismatch", root->offset);
        if (a.scale_data.size() != nf * a.scale_block)
            throw FormatError("scale block data size mismatch", root->offset);
        for (const auto& b : a.bones) {
            // With zero frames the block arrays are empty, so an animated index would read past them.
            if (nf == 0 && (b.rotation_index != 0xFFFF || b.translation_index != 0xFFFF || b.scale_index != 0xFFFF))
                throw FormatError("animated track in a zero-frame animation (" + b.name + ")", root->offset);
            if (b.rotation_index != 0xFFFF && b.rotation_index + 4u > a.rotation_block)
                throw FormatError("rotation index out of block (" + b.name + ")", root->offset);
            if (b.translation_index != 0xFFFF && b.translation_index + 3u > a.translation_block)
                throw FormatError("translation index out of block (" + b.name + ")", root->offset);
            if (b.scale_index != 0xFFFF && b.scale_index + 3u > a.scale_block)
                throw FormatError("scale index out of block (" + b.name + ")", root->offset);
        }
    }
    return a;
}

Animation load(const std::filesystem::path& path) {
    auto buf = read_file(path);
    return parse(buf);
}

// ---- sampling ----
namespace {
std::uint32_t clampf(const Animation& a, std::uint32_t f) { return a.num_frames ? std::min(f, a.num_frames - 1) : 0; }
}

bool Animation::is_translation_animated(std::size_t i) const {
    const auto& b = bones.at(i);
    return layout == Layout::Block ? b.translation_index != 0xFFFF : !b.translation_data.empty();
}
bool Animation::is_rotation_animated(std::size_t i) const {
    const auto& b = bones.at(i);
    return layout == Layout::Block ? b.rotation_index != 0xFFFF : b.rotation_data.size() > 4;
}

Vec3 Animation::translation(std::size_t i, std::uint32_t frame) const {
    const auto& b = bones.at(i);
    frame = clampf(*this, frame);
    const std::uint16_t* q = nullptr;
    if (layout == Layout::Block) {
        if (b.translation_index != 0xFFFF) q = &translation_data[std::size_t(frame) * translation_block + b.translation_index];
    } else if (!b.translation_data.empty()) {
        q = &b.translation_data[std::size_t(frame) * 3];
    }
    if (!q) return b.translation_offset;
    return {b.translation_offset.x + b.translation_scale.x * q[0],
            b.translation_offset.y + b.translation_scale.y * q[1],
            b.translation_offset.z + b.translation_scale.z * q[2]};
}

Vec3 Animation::scale(std::size_t i, std::uint32_t frame) const {
    const auto& b = bones.at(i);
    frame = clampf(*this, frame);
    const std::uint16_t* q = nullptr;
    if (layout == Layout::Block) {
        if (b.scale_index != 0xFFFF) q = &scale_data[std::size_t(frame) * scale_block + b.scale_index];
    } else if (!b.scale_data.empty()) {
        q = &b.scale_data[std::size_t(frame) * 3];
    }
    if (!q) return b.scale_offset;
    return {b.scale_offset.x + b.scale_scale.x * q[0], b.scale_offset.y + b.scale_scale.y * q[1],
            b.scale_offset.z + b.scale_scale.z * q[2]};
}

Quat Animation::rotation(std::size_t i, std::uint32_t frame) const {
    const auto& b = bones.at(i);
    frame = clampf(*this, frame);
    if (layout == Layout::Block) {
        if (b.rotation_index == 0xFFFF) return b.default_rotation;
        return quat_from_i16(&rotation_data[std::size_t(frame) * rotation_block + b.rotation_index]);
    }
    if (b.rotation_data.size() == 4) return quat_from_i16(b.rotation_data.data());
    if (b.rotation_data.empty()) return Quat{};
    return quat_from_i16(&b.rotation_data[std::size_t(frame) * 4]);
}

bool Animation::visible(std::size_t i, std::uint32_t frame) const {
    const auto& b = bones.at(i);
    if (b.visibility.empty()) return true;
    return bit(b.visibility, clampf(*this, frame));
}

}  // namespace alamo::ala

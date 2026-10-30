// ALA: Alamo skeletal animation.
//
// Two on-disk layouts exist, both rooted at chunk 0x1000:
//   * "Per-bone" layout (EaW-era, info has only minis 01/02/03): every bone owns its own quantised
//     tracks in chunks 0x1004 (translation u16x3/frame), 0x1006 (rotation i16x4, either one
//     constant quaternion or one per frame) and optional 0x1005 (scale, not observed).
//   * "Block" layout (FoC-era, info adds minis 0B/0C/0D): all animated bones are packed into
//     shared per-frame blocks in chunks 0x1009 (rotation i16), 0x100A (translation u16) and
//     0x100B (scale u16, not observed); each bone has an element index into the block.
// Both layouts share: translation = offset + scale * u16, rotation = i16 / 32767,
// visibility bitfield 0x1007 and step-frame bitfield 0x1008 ((frames+7)/8 bytes, LSB first).
#pragma once

#include "alamo/formats/chunk.hpp"

#include <optional>
#include <string>
#include <vector>

namespace alamo::ala {

enum class Layout { PerBone, Block };

struct BoneTrack {
    std::string name;
    std::uint32_t bone_index = 0;      // mini 0x05 (1-based in practice; 0 is skeleton root)
    // mini 0x0A (0/1): when this animation is played as an overlay on top of another one, bones
    // with the flag set are NOT driven by it (the runtime's per-bone "exclude from overlay" test).
    std::uint32_t overlay_excluded = 0;
    Vec3 translation_offset;           // mini 0x06
    Vec3 translation_scale;            // mini 0x07
    Vec3 scale_offset;                 // mini 0x08
    Vec3 scale_scale;                  // mini 0x09
    // Block layout only: element offsets into the per-frame blocks (0xFFFF = not animated).
    std::uint16_t translation_index = 0xFFFF;  // mini 0x0E
    std::uint16_t scale_index = 0xFFFF;        // mini 0x0F
    std::uint16_t rotation_index = 0xFFFF;     // mini 0x10
    Quat default_rotation;                     // mini 0x11 (block layout)
    // Per-bone layout only: raw quantised tracks.
    std::vector<std::uint16_t> translation_data;  // 0x1004, 3 per frame
    std::vector<std::uint16_t> scale_data;        // 0x1005, 3 per frame (unverified)
    std::vector<std::int16_t> rotation_data;      // 0x1006, 4 (constant) or 4 per frame
    std::vector<std::uint8_t> visibility;  // 0x1007 bitfield, empty = always visible
    std::vector<std::uint8_t> step_frames; // 0x1008 bitfield; loaded by the runtime but never read
};

struct Animation {
    Layout layout = Layout::Block;
    std::uint32_t num_frames = 0;   // mini 0x01
    float fps = 0;                  // mini 0x02
    std::uint32_t num_bones = 0;    // mini 0x03
    std::uint32_t rotation_block = 0;     // mini 0x0B, i16 elements per frame
    std::uint32_t translation_block = 0;  // mini 0x0C, u16 elements per frame
    std::uint32_t scale_block = 0;        // mini 0x0D, u16 elements per frame
    std::vector<BoneTrack> bones;
    std::vector<std::int16_t> rotation_data;      // 0x1009
    std::vector<std::uint16_t> translation_data;  // 0x100A
    std::vector<std::uint16_t> scale_data;        // 0x100B (unverified)

    float duration() const { return fps > 0 ? float(num_frames) / fps : 0.f; }
    // Decoded samples (frame clamped to [0, num_frames-1]).
    Vec3 translation(std::size_t bone, std::uint32_t frame) const;
    Quat rotation(std::size_t bone, std::uint32_t frame) const;   // normalised
    Vec3 scale(std::size_t bone, std::uint32_t frame) const;
    bool visible(std::size_t bone, std::uint32_t frame) const;
    bool is_translation_animated(std::size_t bone) const;
    bool is_rotation_animated(std::size_t bone) const;
};

Animation parse(Bytes data);
Animation load(const std::filesystem::path& path);

}  // namespace alamo::ala

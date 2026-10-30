// R3 clip library (docs/re/ANIM_PARTICLES.md §A.2, docs/formats/SPEC.md §4).
//
// Clip     one .ALA file, kept QUANTISED (u16 translations, i16 rotations, visibility bits) exactly as
//          stored, so sampling dequantises with the engine's fixed float sequence (anim_math.hpp). Both
//          on-disk layouts (per-bone EaW, block FoC) are copied into one track-major layout.
// Skeleton the model's bones (ALO chunk 0x200): parents, bind-local matrices, billboard modes.
// AnimModel per model: 119 variant lists (type id -> clips _00.._NN) + skeleton + per-clip binding.
//
// Discovery (§A.2): base = model file name without extension, or the type's
// Land_/Space_Model_Anim_Override_Name; for each id 0..118, NN = 00, 01, ... the loader looks up
// <model dir>\<base>_<SUFFIX>_<NN>.ALA and STOPS at the first missing index. Files without an index are
// never loaded.
// Binding (§A.2): a clip binds only when model bones == ALA bones + 1 (the ALA has no root bone);
// otherwise it stays in the list (frame count / fps / variant count are sim-visible) but poses as the
// bind pose. A bound track drives skeleton bone = ALA mini 0x05 (when in 1..bones-1), else track + 1.
//
// Thread safety: loading functions touch no shared state except ClipCache (internally locked).
// AnimModel / Clip are immutable after loading and may be read from any thread.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/anim/anim_math.hpp"
#include "alamo/anim/anim_types.hpp"

namespace alamo {
class FileProvider;
}

namespace alamo::anim {

struct Skeleton {
    std::vector<std::string> names;
    std::vector<int32_t> parents;          // -1 for the root; parent < own index
    std::vector<uint32_t> billboard_mode;  // ALO billboard mode (flag bits 3..6); 6 / 7 copy the parent (§A.6)
    std::vector<Mat34> bind_local;         // relative to the parent bone

    size_t size() const { return names.size(); }
    // Case-insensitive name lookup (Lua Get_Bone_Position, fire bones). -1 if absent.
    int32_t find(std::string_view name) const;
    // A bone composed by copying its parent's world transform (flags & 0x78 == 0x30 / 0x38). [M]
    bool copies_parent(size_t bone) const { return bone < billboard_mode.size() && (billboard_mode[bone] == 6 || billboard_mode[bone] == 7); }
};

// Parse the skeleton of an in-memory .ALO (chunk 0x200). False (with *error) on a malformed file or a
// particle system.
bool skeleton_from_alo(std::span<const uint8_t> bytes, Skeleton& out, std::string* error = nullptr);
// From already-decoded arrays (render side: assets::Skeleton, whose Mat3x4 has Mat34's layout).
// bind_local must hold names.size() entries.
Skeleton skeleton_from_arrays(std::span<const std::string> names, std::span<const int32_t> parents,
                              std::span<const uint32_t> billboard_mode, std::span<const Mat34> bind_local);

struct ClipTrack {
    std::string bone_name;
    uint32_t file_bone_index = 0;  // ALA mini 0x05
    bool overlay_excluded = false; // ALA mini 0x0A
    bool rot_animated = false, trans_animated = false, has_visibility = false;
    Float3 trans_offset, trans_scale;  // minis 0x06 / 0x07
    uint32_t rot_first = 0;    // into Clip::rot_raw (4 i16 per key); 1 key when !rot_animated
    uint32_t trans_first = 0;  // into Clip::trans_raw (3 u16 per key); no keys when !trans_animated
    uint32_t vis_first = 0;    // byte offset into Clip::vis_bits ((frames + 7) / 8 bytes, LSB first)
};

struct Clip {
    std::string name;       // normalised path it was loaded from
    uint32_t name_crc = 0;  // alamo::name_crc(name)
    uint32_t frame_count = 0;
    float fps = 30.0f;
    uint32_t bone_count = 0;  // ALA bones (mini 0x03)
    std::vector<ClipTrack> tracks;
    std::vector<int16_t> rot_raw;
    std::vector<uint16_t> trans_raw;
    std::vector<uint8_t> vis_bits;

    // Sampled local rotation / translation of `track` between keys a and b (both < frame_count, or 0)
    // at fraction t: nlerp_fast / lerp of the dequantised keys (§A.6). Constant channels still pass
    // through nlerp_fast (normalisation), as the original samples every bone the same way.
    void sample(uint32_t track, uint32_t a, uint32_t b, float t, Quat& rot, Float3& trans) const;
    bool visible(uint32_t track, uint32_t frame) const;
};

// Decode an in-memory .ALA. False (with *error) on malformed data.
bool parse_clip(std::span<const uint8_t> bytes, std::string_view name, Clip& out, std::string* error = nullptr);

// "<base>_<SUFFIX>_<NN>.ALA" (NN two digits, more when >= 100) - the discovery file name.
std::string clip_file_name(std::string_view base, int32_t type, uint32_t index);
// Model name -> discovery base ("Data\Art\Models\EI_Trooper.alo" -> "EI_TROOPER").
std::string model_base_name(std::string_view model);

// Thread-safe cache of parsed clips by normalised path (a clip shared by several models through an
// override name is parsed once). Lookup only; never iterated.
class ClipCache {
public:
    // nullptr when missing or malformed (failures counted).
    std::shared_ptr<const Clip> get(const FileProvider& files, const std::string& path);
    size_t size() const;
    size_t failures() const;

private:
    mutable std::mutex mu_;
    std::map<std::string, std::shared_ptr<const Clip>> clips_;
    size_t failures_ = 0;
};

struct ClipRef {
    std::shared_ptr<const Clip> clip;
    bool bound = false;              // bones match (model == ALA + 1); false -> poses as bind pose
    std::vector<int16_t> track_bone; // per clip track: skeleton bone it drives (-1 = none); empty when unbound
};

struct DiscoveryStats {
    uint32_t clips = 0, bound = 0, mismatched = 0, failed = 0;
    uint32_t track_name_mismatch = 0;  // bound tracks whose name differs from the driven bone's (diagnostic)
};

class AnimModel {
public:
    std::string model;      // ALO path it was loaded for
    std::string base;       // discovery base
    Skeleton skeleton;
    bool has_skeleton = false;
    std::array<std::vector<ClipRef>, kAnimTypeCount> clips;  // [type][variant]
    DiscoveryStats stats;

    uint32_t variant_count(int32_t type) const {
        return type >= 0 && type < static_cast<int32_t>(kAnimTypeCount) ? static_cast<uint32_t>(clips[static_cast<size_t>(type)].size()) : 0u;
    }
    const ClipRef* clip(int32_t type, uint32_t variant) const {
        if (type < 0 || type >= static_cast<int32_t>(kAnimTypeCount)) return nullptr;
        const auto& v = clips[static_cast<size_t>(type)];
        return variant < v.size() ? &v[variant] : nullptr;
    }
    bool has_any_clip() const;
};

// Discovery of every (type, NN) clip of `base` in `dir` (e.g. "DATA\ART\MODELS\"), stopping each type at
// its first missing index. Clips are bound against out.skeleton (unbound when !has_skeleton).
void discover_clips(const FileProvider& files, std::string_view dir, std::string_view base, AnimModel& out,
                    ClipCache* cache = nullptr);
// (Re)computes every ClipRef's binding against out.skeleton and refreshes out.stats.
void bind_clips(AnimModel& out);

// Full load for a model: skeleton from the ALO (via the FileProvider), clip discovery with base =
// anim_override if non-empty, else the model's base name. False (with *error) when the ALO is missing or
// malformed (the clip lists are still discovered, all unbound).
bool load_anim_model(const FileProvider& files, std::string_view model, std::string_view anim_override, AnimModel& out,
                     ClipCache* cache = nullptr, std::string* error = nullptr);

}  // namespace alamo::anim

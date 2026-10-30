// alamo_assets - skeleton, ALA animation clips and CPU pose sampling / skin palettes.
//
// Skinning model (SPEC.md 2.6, PORTING.md 2.4): rigid, one bone per vertex. Skinned vertices are
// stored in bind (model) space, so the palette matrix of bone b is
//     world_anim(b) * inverse(world_bind(b)).
// Static meshes are stored in the space of their attach bone: draw with world_anim(attach_bone).
//
// Animation (SPEC.md 4.3): each ALA bone track REPLACES the bind-local translation / rotation /
// scale of the same-named skeleton bone. Bones without a track keep their bind-local matrix.
//
// Thread safety: all functions are pure (inputs const, outputs caller-owned) and may run
// concurrently on worker threads.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/assets/math.hpp"

namespace alamo {
class FileProvider;
}

namespace alamo::assets {

struct Skeleton {
    std::vector<std::string> names;
    std::vector<std::int32_t> parents;          // -1 for the root; parent < own index (verified)
    std::vector<std::uint8_t> visible;          // ALO bone visibility flag
    std::vector<std::uint32_t> billboard_mode;  // alo BillboardMode value (0 = none)
    std::vector<Mat3x4> bind_local;             // relative to the parent bone
    std::vector<Mat3x4> bind_world;             // bone -> model space
    std::vector<Mat3x4> inverse_bind_world;

    std::size_t size() const { return names.size(); }
    // Case-insensitive lookup through the CRC name hash (sorted table). -1 if absent.
    std::int32_t find(std::string_view name) const;

    // Called by the loader after names/parents/bind_local are filled: computes bind_world,
    // inverse_bind_world and the lookup table. Throws std::runtime_error on a bad parent index.
    void finalize();

private:
    struct NameKey { std::uint32_t crc; std::uint32_t bone; };
    std::vector<NameKey> lookup_;  // sorted by (crc, bone)
};

// One decoded bone track. Arrays hold either 1 entry (constant) or frame_count entries.
struct BoneTrack {
    std::string bone_name;
    std::uint32_t name_crc = 0;        // alamo::name_crc(bone_name)
    std::uint32_t file_bone_index = 0; // ALA mini 0x05 (informational; binding is by name)
    bool overlay_excluded = false;     // ALA mini 0x0A
    std::vector<Float3> translation;
    std::vector<Quat> rotation;        // normalised
    std::vector<Float3> scale;
    std::vector<std::uint8_t> visible; // one byte per frame (0/1); empty = always visible
};

struct AnimationClip {
    std::string name;
    float fps = 30.f;
    std::uint32_t frame_count = 0;
    std::vector<BoneTrack> tracks;
    // Length used for looping: frame_count / fps (the formats library's convention).
    float duration() const { return fps > 0 ? float(frame_count) / fps : 0.f; }
};

struct Pose {
    std::vector<Mat3x4> local;         // per skeleton bone
    std::vector<std::uint8_t> visible; // per skeleton bone: ALO flag AND track visibility
};

// Bind pose (local = bind_local, visible = skeleton flag).
Pose bind_pose(const Skeleton& skel);

// Sample `clip` at `time` seconds. loop=true wraps time into [0, duration) and interpolates the
// last frame toward frame 0; loop=false clamps to [0, frame_count-1]. Linear interpolation of
// translation/scale, shortest-arc nlerp of rotation, visibility from the lower key.
// out is resized to skel.size(). Tracks naming no skeleton bone are ignored.
void sample(const Skeleton& skel, const AnimationClip& clip, float time, bool loop, Pose& out);

// world[b] = world[parent(b)] * local[b]. local.size() and world.size() must equal skel.size()
// (otherwise nothing is written).
void compute_world(const Skeleton& skel, std::span<const Mat3x4> local, std::span<Mat3x4> world);

// out[i] = world[palette[i]] * skel.inverse_bind_world[palette[i]]; out.size() >= palette.size().
// Out-of-range palette entries produce identity.
void compute_skin_palette(const Skeleton& skel, std::span<const Mat3x4> world,
                          std::span<const std::uint16_t> palette, std::span<Mat3x4> out);

// Decode an in-memory .ala into sampleable tracks (both the per-bone and the block layout).
// Returns false (with *error) on malformed data.
bool build_animation(std::span<const std::uint8_t> bytes, std::string_view name, AnimationClip& out,
                     std::string* error = nullptr);
// Read through the FileProvider; `path` as for animation_path() (model.hpp).
bool load_animation(const FileProvider& files, std::string_view path, AnimationClip& out,
                    std::string* error = nullptr);

}  // namespace alamo::assets

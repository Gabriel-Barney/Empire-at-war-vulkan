// alamo_render (lib alamo_render_sim) - R5 sensor jamming visual (docs/re/ABILITY_VISUALS.md section 3, finding 6.3 "Jamming").
// Render-side only: it reads InterpolatedFrame::jamming (filled by apply_battle_jamming from the sim's const state) and draws:
//   * Jammer WITHOUT Owner_Particle_Effect (the two Underworld jammer structures): a radius blob = a square of side 2 x radius
//     centred on the jammer, no rotation, material Blob_Material_Name (ShadowBlobMaterials.xml; vanilla Sensor_Jamming_Blob =
//     i_circle00.tga, ALPHA), colour Blob_Color (200, 50, 200, 128). Shown to the jammer's allies (the local player's team);
//     with Show_Blob_To_Enemies also to others, except while the jammer is fogged for the local player (kSnapFogHidden).
//   * Jammer WITH Owner_Particle_Effect (Skipray, Houndstooth): no blob; the particle is a SnapshotEffect (kEffectJamming)
//     that the existing R4 attach path (particles::sync_sim_frame) spawns on the jammer, at the first Owner_Particle_Bone_Name
//     bone when the model has it.
//   * Jammed units have no visual of their own; the radar drops the blips of hostile jammed units (ui::HudUnit::jammed).
// No object ids are consumed (CLAUDE.md: parity by behaviour; ABILITY_VISUALS 6.4 decision).
// [M] The blob is a flat unit-quad model with the standard MeshAlpha effect (ALPHA blend, texture x Diffuse = Blob_Color) in the
// Transparent bucket, one model per (material, colour) like the selection circle; the original's alBlob / terrain decal PrimAlpha
// path is not reproduced exactly (lighting of MeshAlpha applies) and land blobs are flat at the jammer's ground height, not draped.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/assets/model.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/terrain/decorations.hpp"

namespace alamo::render {

inline constexpr float kJamBlobLift = 0.5f;  // [port] z offset above the jammer's plane (z-fighting guard)
inline constexpr const char* kJamBlobFallbackTexture = "i_circle00.tga";  // the vanilla Sensor_Jamming_Blob texture

// One blob to draw this frame.
struct JamBlob {
    objects::ObjectId jammer = objects::kNoObjectId;
    Vec3 center{};
    float half_size = 0.0f;  // the jam radius: the square's side is 2 x half_size
    std::string material;
    float color[4] = {1, 1, 1, 1};  // Blob_Color 0..1
};

// Who sees a jammer's blob (ABILITY_VISUALS 3): allies always; others only with Show_Blob_To_Enemies and while the jammer is not
// fogged for the local player. `jammer` = the jammer's object in the frame (null: not drawn).
bool jam_blob_visible(const SnapshotJammer& j, const SnapshotObject* jammer);

// The blobs of a frame, ascending jammer id. A jammer whose object is missing, dying, faded out, contained or has a
// non-positive radius draws nothing.
std::vector<JamBlob> jam_blobs(const InterpolatedFrame& frame);

// Unit quad (+-1 in X / Y, z = 0, normal +Z, uv (0,0) top-left) with the standard MeshAlpha effect: BaseTexture =
// material.texture (kJamBlobFallbackTexture when empty), Diffuse = `color`.
std::shared_ptr<assets::ModelAsset> jam_blob_model(const terrain::ShadowBlobMaterial& material, const float (&color)[4]);
// Instance of an uploaded blob model: scale half_size, translation = centre + kJamBlobLift in z.
Instance jam_blob_instance(const GpuModel& model, const JamBlob& blob);

// Uploaded blob models (one per material + colour) and the per-frame instances. Single-threaded (render thread).
class JamBlobModels {
public:
    // The ShadowBlobMaterials table (builtin + XML); names matched case-insensitively. Replaces the table; models stay.
    void set_materials(std::vector<terrain::ShadowBlobMaterial> materials) { materials_ = std::move(materials); }
    // Appends one instance per visible blob of `frame` to `out`, uploading the model of a new (material, colour) first
    // (a failed upload is remembered, that blob is skipped). `ground`: land height under (x, y) (the blob sits on the
    // ground at the jammer's x, y); null = space (the jammer's own z). Returns the number of instances appended.
    uint32_t build(Renderer& renderer, const TextureSource& textures, const InterpolatedFrame& frame,
                   const std::function<float(float, float)>& ground, std::vector<Instance>& out);
    void shutdown(Renderer& renderer);
    size_t model_count() const { return models_.size(); }

private:
    struct Entry {
        GpuModel model;
        bool ok = false;
    };
    std::vector<terrain::ShadowBlobMaterial> materials_;
    std::map<std::string, std::unique_ptr<Entry>> models_;  // key: lower-case material + colour bytes (lookup only)
};

}  // namespace alamo::render

// alamo_render - land map world: terrain patches, water and the water's extra passes
// (docs/re/TERRAIN_WATER_POST.md 2, 3), drawn by Renderer::render() while attached with set_world().
//
// Terrain (bucket 0, before the bucket's mesh draws): per patch a 36-byte vertex buffer
// (terrain::TerrainVertex = materials PrimVertLit), per pass a u16 index buffer and the 32x32 blend
// texture (B4G4R4A4 + Swizzle::ARGB4_IN_BGRA4, linear + clamp from the FXO's BlendSampler). Patches
// are culled by AABB against the frustum and drawn in row-major order, passes in list order, with
// TerrainRenderBump's best technique (`bump`, translated), world identity, the engine's per-pass
// SRC_ALPHA / ONE_MINUS_SRC_ALPHA blend, LESS_EQUAL depth with write. Per pass: diffuse + normal
// textures, diffuseTexU/V, materialDiffuse/Specular (TerrainMaterialGpu); per patch: blendTexScale /
// Offset; per frame: CLOUD_TEXTURE + CLOUD_TEX_U/V = (1/s,0,0,-ox/s), (0,1/s,0,-oy/s), offset += velocity
// * dt. The terrain is in the main depth-stencil before the Shadow bucket, so stencil volumes darken it.
//
// Water (bucket 7, before the bucket's mesh draws), when the surface is enabled: the surface effect's
// best technique (t0 = WaterLOD 2) over the water patches (terrain::WaterVertex, render kWaterLayout),
// water_constants(surface, water_time) (dt clamped to 1 s per update), the surface's texture bindings,
// m_texOffset left at the FXO default. Its targets (surface.targets), rendered every frame before the
// main scene:
//   * sky cube (128^2 per face): the environment's skydome objects, cleared to the environment clear
//     colour; re-rendered when the environment (or the sky object list) changes;
//   * reflection (N x N): camera mirrored about z = height, clip plane (0,0,1) at height - offset, cleared
//     to white with the skydome objects drawn first (the water-detail-2 path), then terrain + the mesh
//     buckets Terrain, TerrainMesh, Opaque, OpaqueNonOccluding, Transparent (bucket mask 0x12B);
//   * refraction (N x N): the camera, clip plane at height + offset keeping what is BELOW it, environment
//     clear colour, the same buckets without the sky.
// User clip planes: the translated shaders have no clip-distance output, so the plane is applied as an
// oblique near plane in the projection (clip_projection()), which works for every program type.
// Targets use the scene's colour format (B8G8R8A8_UNORM here) and depth-stencil format (D32FS8 here)
// so the scene pipelines are reused; each water target owns its depth image (the original shares one).
// The original draws TerrainRenderBaked (pre-baked patch textures) in the reflection / refraction
// passes; this port draws the normal splatting passes there.
//
// Decorations (docs/re/DECORATIONS.md, engine/terrain decorations.hpp / fow.hpp), main pass only:
//   bucket 0 right after the terrain patches, in the order 3 (map tracks on land), 2 (dynamic tracks),
//   1 (decals, D_master_decal.tga, one draw per render mode 0..13), 0 (shadow blobs per material),
//   4 (fog of war: every visible patch again with TerrainFogOfWar, DESTCOLOR / ZERO), all with the camera's
//   near and far + 0.3 (kDecorationDepthOffset); type 5 (map tracks on the water plane) in bucket 7 right
//   after the water with the unmodified camera. Prim render modes draw with Engine\Prim* (FF passes through
//   the FF ubershader, translated ones through their program): one merged vertex / index buffer for the
//   static decorations (map tracks, decals), one per frame slot for the per-frame ones (dynamic tracks,
//   shadow blobs, fading decals); one hot-set set 0 per call, a pipeline bind only when it changes.
// Fog of war: set_fog_of_war() uploads FowGrid::texels() (R8G8B8A8, linear clamp); while enabled the
// renderer binds it as FOW_TEXTURE to every sampler with that semantic (terrain mesh, grass, batch mesh,
// water, the FOW passes) with FOW_TEX_U / V of the grid box; otherwise FOW_TEXTURE is the white texture
// (FOW_TEX_U / V of the box are still fed).
// Space maps (WorldOptions::space): no terrain / water; with fog of war the SpaceFogOfWar plane over the box
// at z = kSpaceFowHeight is drawn in the background scene (RecordOptions::space).
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/render/renderer.hpp"
#include "alamo/terrain/decorations.hpp"
#include "alamo/terrain/fow.hpp"
#include "alamo/terrain/mesh.hpp"
#include "alamo/terrain/ted.hpp"
#include "alamo/terrain/water.hpp"

namespace alamo::render {

// Static decorations of a map (built on the CPU by the caller, e.g. MapScene), consumed by WorldRenderer::init.
struct WorldDecorations {
    std::vector<terrain::TrackGeometry> tracks;      // map tracks (type 3 land / type 5 water plane)
    std::vector<terrain::DecalGeometry> decals;      // type 1
    std::vector<terrain::TerrainDecal> decal_records;  // same order as decals (fade time, intensity)
    std::vector<terrain::ShadowBlobMaterial> blob_materials;  // type 0 material table (builtin + XML)
    std::vector<std::string> extra_textures;         // dynamic-track textures etc. (loaded at init)
    // R5: the map will get runtime decals (TerrainDecalFX scorch marks / footprints, set_runtime_decals): load
    // D_master_decal.tga even when the map has no decals of its own.
    bool runtime_decals = false;
};
// One per-frame dynamic track (type 2): ribbon from DynamicTrackGenerator::build.
struct DynamicTrackMesh {
    terrain::DecorationMesh mesh;
    std::string texture;
    terrain::RenderMode mode = terrain::RenderMode::DiffuseAlpha;
};
// One per-frame shadow blob (type 0).
struct BlobMesh {
    terrain::BlobGeometry geometry;
    uint32_t material = 0;  // into WorldDecorations::blob_materials
};

struct DecorationStats {
    uint32_t land_tracks = 0, water_tracks = 0, decals = 0, dynamic_tracks = 0, blobs = 0;
    uint32_t draws = 0, ff_draws = 0, translated_draws = 0, pending = 0, fow_draws = 0, water_track_draws = 0;
    uint32_t pipeline_binds = 0, space_fow_draws = 0;
    uint64_t triangles = 0;
    bool fow = false;
};

struct WorldOptions {
    bool terrain = true;
    bool water = true;
    bool water_targets = true;  // reflection / refraction / sky cube (false: water samples white / black)
    // Batched terrain / water draws (default): per call one pipeline / dynamic-state / push / vertex + index
    // buffer bind (patches share merged buffers, drawn with firstIndex / vertexOffset), texture sets
    // written once per (patch, pass) at first use, register images in persistent per-frame-slot uniform
    // regions whose descriptor sets are written once; per draw only the constants copy, one set bind and
    // the draw. Same draw order and register contents as the unbatched path (false: every draw through
    // Renderer::post_translated, the reference for the pixel-identity test).
    bool batch_draws = true;
    bool decorations = true;    // draw the WorldDecorations / dynamic tracks / blobs / FOW pass
    bool space = false;         // space map: no terrain or water; space FOW plane instead of the terrain pass
};

// Command / descriptor work of the world geometry draws of one frame (terrain + water; main pass,
// reflection and refraction).
struct WorldBindStats {
    uint32_t draws = 0, pipeline_binds = 0, set_binds = 0, buffer_binds = 0, push_constants = 0, dynamic_state = 0;
    uint32_t set_allocs = 0, set_writes = 0, uniform_allocs = 0;
};

struct WorldStats {
    uint32_t patches = 0, visible_patches = 0, terrain_draws = 0, terrain_pending = 0;
    uint64_t terrain_triangles = 0;
    uint32_t water_patches = 0, water_draws = 0, water_pending = 0;
    uint64_t water_triangles = 0;
    bool reflection = false, refraction = false, sky_cube_rendered = false;
    uint32_t reflection_draws = 0, refraction_draws = 0;  // mesh items + terrain draws of the extra passes
    uint32_t textures = 0, uploads_pending = 0;
    WorldBindStats binds;
    DecorationStats decorations;
};

class WorldRenderer {
public:
    WorldRenderer();  // out of line: members hold types private to renderer.cpp / world.cpp
    ~WorldRenderer();
    WorldRenderer(const WorldRenderer&) = delete;
    WorldRenderer& operator=(const WorldRenderer&) = delete;

    // Creates the GPU buffers / textures of the mesh and the water surface (queued on the renderer's
    // uploader) and the water targets. `water` may be null. Textures come from `textures` (AssetCache).
    bool init(Renderer& r, const terrain::TerrainMesh& mesh, const terrain::WaterSurface* water,
              const terrain::CloudParams& clouds, const TextureSource& textures, const WorldOptions& options = {},
              std::string* error = nullptr, const WorldDecorations* decorations = nullptr);
    // ---- Decorations / fog of war (after init).
    // Per-frame dynamic tracks and shadow blobs (replace the previous frame's; textures must be among
    // WorldDecorations::extra_textures or the blob materials, else they draw white).
    void set_dynamic_tracks(std::vector<DynamicTrackMesh> tracks) { dyn_tracks_ = std::move(tracks); }
    void set_shadow_blobs(std::vector<BlobMesh> blobs) { blobs_ = std::move(blobs); }
    // R5: per-frame runtime decals (type 1, D_master_decal.tga; colours already faded by the caller, e.g. RuntimeDecals).
    // Drawn after the map's decals, one draw per decal in the given order. Needs WorldDecorations::runtime_decals (or map
    // decals) at init for the atlas texture, else they draw white.
    void set_runtime_decals(std::vector<terrain::DecalGeometry> decals) { runtime_decals_ = std::move(decals); }
    // FOW_TEX_U / V of the playable box (fed even with the fog of war off).
    void set_fow_box(float x0, float y0, float x1, float y1);
    // Uploads the grid's texels (FowGrid::build_texels ran) as the FOW texture and enables the fog of war
    // (the new texture is used once its upload finished). False on allocation failure.
    bool set_fog_of_war(const terrain::FowGrid& grid);
    void set_fow_enabled(bool on) { fow_enabled_ = on; }
    // Draw types 0-3 and 5 (default on; tests / diagnostics turn them off to attribute pixels).
    void set_decorations_visible(bool on) { deco_visible_ = on; }
    bool fow_enabled() const { return fow_enabled_ && fow_image_ok_; }
    bool fow_upload_pending() const { return fow_next_pending_; }  // a set_fog_of_war upload not yet resident
    // Blocking (scripted runs, before the frame's record_acquires): waits for a pending set_fog_of_war upload and
    // acquires it on the graphics queue, so this frame draws it. Without it a fog changing every frame keeps superseding
    // its own in-flight upload and the plane keeps an old texture. False when nothing was pending or the wait failed.
    bool finish_fow_upload();
    // R5 space reinforcement overlay: the SpaceFogOfWar plane's stage-1 grid = W_Space_Reinforce_FOW_Grid.tga instead of
    // W_Space_FOW_Grid.tga (DECORATIONS §2.1 +0x388). Space worlds only.
    void set_space_fow_reinforce(bool on) { space_reinforce_ = on; }
    bool space_fow_reinforce() const { return space_reinforce_; }
    // SpaceFogOfWar grid texture repeats across the FOW box (the FXO's stage-1 transform scales FOW_TEX_U / V by 200).
    static constexpr float kSpaceFowGridRepeats = 200.0f;
    bool space() const { return options_.space; }
    void shutdown();  // GPU idle
    // (Re)creates the water / sky-cube targets in the renderer's current colour format (init, and
    // Renderer::set_color_format with the GPU idle). False on allocation failure.
    bool create_targets();
    // Call after Uploader::record_acquires each frame; true once every upload is resident.
    bool make_resident();
    bool resident() const { return resident_; }
    // Scene update: water time (dt clamped to 1 s) and the cloud offset (velocity * dt).
    void update(float dt);
    // Environment skydome objects for the sky cube (instances at the origin with their skydome transform).
    void set_sky(std::span<const Instance> sky);
    void environment_changed() { sky_dirty_ = true; }
    const WorldStats& stats() const { return stats_; }
    // Switch between the batched and the reference (unbatched) terrain / water draws (WorldOptions::batch_draws).
    void set_batch_draws(bool batched) { options_.batch_draws = batched; }
    // Bindless slot of the cloud texture once resident (hot-set / FF CLOUD_TEXTURE), 0 = none.
    uint32_t cloud_texture_slot() const { return cloud_slot_; }
    float water_time() const { return water_time_; }
    float cloud_offset_x() const { return cloud_off_[0]; }
    float cloud_offset_y() const { return cloud_off_[1]; }
    // Tests: the water targets (null when not created).
    const rhi::RenderTarget* reflection_target() const { return reflection_ ? &reflection_->rt : nullptr; }
    const rhi::RenderTarget* refraction_target() const { return refraction_ ? &refraction_->rt : nullptr; }

    // ---- CPU helpers (exposed for tests).
    // CLOUD_TEX_U / V of TERRAIN_WATER_POST.md 2.5.
    static void cloud_matrices(float scale, float offset_x, float offset_y, materials::float4& u, materials::float4& v);
    // World mirror about the plane z = height (determinant -1).
    static Mat4 reflection_matrix(float height);
    // Camera mirrored about z = height: view x mirror, eye mirrored. The result is a mirrored camera
    // (draw with RecordOptions::mirrored).
    static ViewMatrices mirrored_view(const ViewMatrices& v, float height);
    // Replaces the near plane by the world plane (a,b,c,d) (kept: a x + b y + c z + d >= 0), Lengyel's
    // oblique near-plane clipping for a 0..1 depth RH projection. False (unchanged) when the camera is
    // on the kept side of the plane (the near plane can only cut the eye's side away).
    // The oblique matrix tilts the far plane through the original far frustum corner on the plane's side,
    // so geometry near the far plane on the other side would be cut: `far_scale` first pushes the far
    // plane out (the extra passes use kObliqueFarScale).
    static bool clip_projection(ViewMatrices& v, const Float4& world_plane, float far_scale = 1.0f);
    static constexpr float kObliqueFarScale = 8.0f;

private:
    friend class Renderer;
    struct PassGpu {
        uint8_t material = 0;
        uint32_t first_index = 0;  // into terrain_ib_
        uint32_t index_count = 0;
        rhi::Image blend;
        VkDescriptorSet textures = VK_NULL_HANDLE;  // batched: set 1, written at first use (sets_)
    };
    struct PatchGpu {
        int32_t vertex_offset = 0;  // into terrain_vb_ (vertices)
        assets::Aabb bounds;
        float blend_scale[2] = {0, 0}, blend_offset[2] = {0, 0};
        std::vector<PassGpu> passes;
    };
    struct MaterialGpu {
        int diffuse = -1, normal = -1;  // textures_
        std::vector<float> tex_u, tex_v, diffuse_color, specular_color, shininess;
    };
    struct WaterPatchGpu {
        int32_t vertex_offset = 0;  // into water_vb_
        uint32_t first_index = 0;   // into water_ib_
        uint32_t index_count = 0;
        assets::Aabb bounds;
    };
    struct WaterTarget {
        rhi::RenderTarget rt;
    };
    // Persistent constant regions (batched draws): per frame slot, chunks of kRegionsPerChunk regions of
    // [VSConstants | PSConstants | VS int/bool | PS int/bool] in one host-visible buffer, each with its
    // set 0 written once. A frame hands them out in order; slot reuse waits for the GPU (FrameRing).
    static constexpr uint32_t kRegionsPerChunk = 128;
    static constexpr VkDeviceSize kRegionBytes = 4096 + 512 + 512 + 512;  // 256-aligned parts
    struct ConstChunk {
        rhi::Buffer buffer;
        VkDescriptorSet sets[kRegionsPerChunk] = {};
    };
    struct ConstRegion {
        uint8_t* ptr = nullptr;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };
    static constexpr uint32_t kMaxSlots = 3;
    // Next free region of this frame (int_bool: the set 0 layout with bindings 2/3); null on failure.
    ConstRegion const_region(const rhi::Frame& frame, bool int_bool);
    static void copy_region(uint8_t* dst, const Renderer::XConstCache& cc, bool int_bool);
    int add_texture(const std::string& name, assets::TextureRole role);
    bool upload_buffer(rhi::Buffer& b, std::span<const uint8_t> bytes, VkBufferUsageFlags usage, const char* name);

    Renderer* r_ = nullptr;
    TextureSource textures_src_;
    WorldOptions options_;
    std::vector<PatchGpu> patches_;
    rhi::Buffer terrain_vb_, terrain_ib_, water_vb_, water_ib_;  // every patch, merged
    rhi::DescriptorAllocator sets_;                              // persistent sets (texture sets, constant regions)
    const materials::TranslatedProgram* tex_sets_tp_ = nullptr;  // program + samplers the texture sets were written for
    VkSampler tex_sets_samplers_[16] = {};
    std::vector<ConstChunk> chunks_[kMaxSlots][2];               // [frame slot][int_bool]
    uint32_t cursor_[kMaxSlots][2] = {};
    uint64_t cursor_frame_[kMaxSlots] = {};
    std::vector<std::pair<uint32_t, uint32_t>> visible_;         // (patch, pass) of the current terrain call
    uint32_t cloud_slot_ = 0;                                    // bindless slot of the cloud texture (0 = none)
    MaterialGpu materials_[terrain::kMaterialSlots];
    std::vector<GpuTexture> textures_;
    int cloud_texture_ = -1;
    std::shared_ptr<const materials::MaterialEffect> terrain_fx_;
    const materials::ResolvedPass* terrain_pass_ = nullptr;
    // Water.
    bool water_on_ = false;
    terrain::WaterSurface water_;  // copy without the patch vertex data
    std::vector<WaterPatchGpu> water_patches_;
    std::vector<std::pair<std::string, int>> water_textures_;  // parameter -> textures_
    std::vector<uint32_t> water_vis_scratch_;                    // Q3: visible water patches (kept across frames)
    std::vector<uint32_t> fow_vis_scratch_;                      // Q3: visible FOW patches (kept across frames)
    std::shared_ptr<const materials::MaterialEffect> water_fx_;
    const materials::ResolvedPass* water_pass_ = nullptr;
    std::unique_ptr<WaterTarget> reflection_, refraction_;
    rhi::RenderTarget sky_rt_;
    rhi::Image sky_cube_;
    bool sky_cube_valid_ = false, sky_dirty_ = true;
    rhi::ImageState sky_cube_state_ = rhi::ImageState::Undefined;  // tracked across frames (WAR vs last frame's reads)
    std::vector<Instance> sky_;
    // Uploads.
    std::vector<rhi::UploadTicket> tickets_;
    bool resident_ = false;
    // Time.
    float water_time_ = 0, cloud_off_[2] = {0, 0}, cloud_scale_ = 500.0f, cloud_vel_[2] = {5, 5};
    uint64_t draw_generation_ = 0;
    std::unique_ptr<Renderer::XConstCache[]> caches_;  // per terrain material: register images of the current draw call
    std::unique_ptr<Renderer::XConstCache> water_cache_;
    WorldStats stats_;

    // ---- Decorations (decorations.cpp).
    struct DecoDraw {
        terrain::DecorationType type = terrain::DecorationType::LandTrack;
        terrain::RenderMode mode = terrain::RenderMode::DiffuseAlpha;
        int texture = -1;               // textures_ (-1 = white)
        bool clamp = false;             // CLAMP addressing (else WRAP)
        bool prelit = false;            // PrimVert 24 B (blobs), else PrimVertLit 36 B
        VkDeviceSize vb_offset = 0;     // bytes into the vertex buffer (indices are local to it)
        uint32_t first_index = 0, index_count = 0;  // u32 indices
        float u_speed = 0, v_speed = 0; // map tracks: texture-matrix scroll
        assets::Aabb bounds;            // culling (invalid = always drawn)
    };
    struct DynSlot {
        rhi::Buffer buffer;             // vertices then indices, host visible
        VkDeviceSize size = 0;
    };
    bool init_decorations(const terrain::TerrainMesh& mesh, const WorldDecorations& d, std::string* error);
    void shutdown_decorations();
    // Per-frame geometry (dynamic tracks, blobs, fading decals) into this frame slot's buffer.
    bool build_dynamic(const rhi::Frame& frame, std::vector<DecoDraw>& out, VkBuffer& buffer, VkDeviceSize& index_base);
    bool upload_fow_pending();
    std::vector<DecoDraw> deco_static_;
    rhi::Buffer deco_vb_, deco_ib_;
    std::vector<terrain::DecalGeometry> fading_decals_;
    std::vector<terrain::TerrainDecal> fading_records_;
    std::vector<terrain::ShadowBlobMaterial> blob_materials_;
    std::vector<int> blob_texture_;                  // per blob material
    std::vector<DynamicTrackMesh> dyn_tracks_;
    std::vector<BlobMesh> blobs_;
    std::vector<terrain::DecalGeometry> runtime_decals_;  // R5
    DynSlot dyn_[kMaxSlots];
    std::vector<int> slot_textures_;                 // textures_ that get a bindless slot (FF decoration draws)
    int master_decal_ = -1, space_grid_ = -1, space_reinforce_grid_ = -1;
    bool space_reinforce_ = false;
    float deco_time_ = 0;                            // seconds since attach (track scroll, decal age)
    bool deco_visible_ = true;
    std::shared_ptr<const materials::MaterialEffect> prim_fx_[terrain::kRenderModeCount];
    const materials::ResolvedPass* prim_pass_[terrain::kRenderModeCount] = {};
    std::unique_ptr<materials::FfDrawTemplate> prim_tpl_[terrain::kRenderModeCount];
    std::unique_ptr<Renderer::XConstCache> deco_cache_;
    // Fog of war.
    rhi::Buffer fow_ib_;                              // full 31x31-quad list, then per holed patch its rendered quads
    std::vector<std::pair<uint32_t, uint32_t>> fow_patch_;  // per patch (first index, count)
    rhi::Image fow_image_, fow_next_;
    rhi::UploadTicket fow_ticket_;
    uint32_t fow_slot_ = 0;
    bool fow_image_ok_ = false, fow_enabled_ = false, fow_next_pending_ = false;
    materials::float4 fow_u_{}, fow_v_{};
    float fow_box_[4] = {0, 0, 0, 0};
    std::shared_ptr<const materials::MaterialEffect> fow_fx_, space_fow_fx_;
    const materials::ResolvedPass* fow_pass_ = nullptr;
    const materials::ResolvedPass* space_fow_pass_ = nullptr;
    std::unique_ptr<materials::FfDrawTemplate> space_fow_tpl_;
    std::unique_ptr<Renderer::XConstCache> fow_cache_;
    // 6 PrimVert vertices over the box at kSpaceFowHeight, one buffer per frame slot (rewritten only in its own
    // slot, when the box generation changed, so no frame in flight reads a buffer being written).
    rhi::Buffer space_plane_vb_[kMaxSlots];
    uint64_t space_plane_gen_[kMaxSlots] = {};
    uint64_t fow_box_gen_ = 0;
    bool write_space_plane(uint32_t slot);
};

}  // namespace alamo::render

// alamo_render_particles - GPU simulation and drawing of the ParticleWorld (R4, Gabriel's decision: particles on the GPU).
//
// Renderer::SceneHook implementation:
//   prepare(): replays the world's queued UpdatePackets on the primary (range clears, then per packet CSUpdate over every
//     slot of its emitters and CSSpawn over its spawn records), builds the frame's draw list (visible systems, parent
//     fade > 0.01, whole-system AABB vs frustum, B.5) and sorts the particles of sorted emitters (blend 0/2/5/11 with
//     0x2B) back-to-front with one CSSort group each;
//   draw(bucket): systems back-to-front (per renderable, RENDER.md 1.4), inside a system its emitters in file order, one
//     instanced draw each (6 vertices x pool, dead slots degenerate): bucket 8 (Transparent), bucket 7 (Water) for
//     systems flagged water, bucket 11 (Heat) for heat emitters (0x3B, drawn as PrimAlpha).
// Blend states (SPEC 3.1): 0 opaque (z-write), 1/4 ONE/ONE (alpha fades RGB), 2/5/7/11/13 SRCALPHA/INVSRCALPHA,
// 3/6 ZERO/SRCCOLOR. Soft depth sprites (4/5/6), the bump of mode 11 and scene fog are not reproduced (open items).
// GPU particle buffers persist across frames; per-frame data goes through the frame ring (alloc_storage / uniform).
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "alamo/assets/texture.hpp"
#include "alamo/render/particles/particle_sim.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/rhi/rhi.hpp"

namespace alamo::render::particles {

// Colour texture by (directory-less, upper-case) name; null = missing (the built-in soft dot is used).
using ParticleTextureSource = std::function<std::shared_ptr<const assets::TextureAsset>(std::string_view name)>;

struct ParticleFrameStats {
    uint32_t packets = 0, updates = 0, spawns = 0, update_groups = 0;
    uint32_t systems = 0, systems_drawn = 0, systems_culled = 0, systems_hidden = 0;
    uint32_t emitters_drawn = 0, draw_calls = 0, sorted_emitters = 0;
    uint32_t water_draws = 0, transparent_draws = 0, heat_draws = 0;
    uint32_t textures = 0, textures_pending = 0;
};

class ParticleRenderer final : public SceneHook {
public:
    ParticleRenderer();
    ~ParticleRenderer() override;
    ParticleRenderer(const ParticleRenderer&) = delete;
    ParticleRenderer& operator=(const ParticleRenderer&) = delete;

    // `world` must outlive the renderer; its packets are recorded from now on (set_record_packets).
    bool init(rhi::Context& ctx, rhi::Allocator& alloc, rhi::Uploader& up, ParticleWorld& world, ParticleTextureSource textures);
    void shutdown();  // the GPU must be idle
    bool initialized() const { return ctx_ != nullptr; }

    // Wind for the Affected_By_Wind particle path (FrameConstants::wind, particles.hlsl). Not wired yet: no host feeds the
    // map / weather wind here, so it stays zero (open item, roadmap cu8f).
    void set_wind(const Float3& w) { wind_ = w; }
    // Scene fog on the main-scene particle draws (the renderer forwards its lighting's FOG_VALS before prepare; never in
    // the heat pass).
    void scene_fog(bool enabled, const Float3& color, float fog_x, float fog_y) override {
        fog_on_ = enabled;
        fog_color_ = color;
        fog_x_ = fog_x;
        fog_y_ = fog_y;
    }
    void set_enabled(bool on) { enabled_ = on; }  // global "particles enabled" (B.5); off: nothing drawn (still simulated)
    // Back-to-front sort range (camera distance mapped to 19 bits).
    void set_sort_range(float r) { sort_range_ = r; }
    bool sort_supported() const { return sort_ok_; }

    void prepare(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view) override;
    bool has(PhaseBucket bucket) const override;
    void draw(const SceneHookPass& pass) override;
    // Bucket 8 interleave by distance with the transparent meshes (B.5): systems of the Transparent list.
    uint32_t draw_count(PhaseBucket bucket) const override;
    float draw_distance(PhaseBucket bucket, uint32_t index) const override;
    void draw_range(const SceneHookPass& pass, uint32_t begin, uint32_t end) override;

    const ParticleFrameStats& last_stats() const { return stats_; }

private:
    struct GpuTexture {
        rhi::Image image;
        rhi::UploadTicket ticket;
        bool ok = false;
    };
    struct DrawItem {
        uint32_t entry = 0;      // draw table index
        uint32_t pool = 0;
        uint32_t mode = 2;       // 0 opaque, 1 additive, 2 alpha, 3 modulate
        const GpuTexture* tex = nullptr;
    };
    struct SystemDraw {
        float distance = 0;
        std::vector<DrawItem> items;
    };
    const GpuTexture* texture(const std::string& name);
    VkPipeline pipeline(uint32_t mode, const SceneHookPass& p);
    VkDescriptorSet texture_set(rhi::Frame& frame, const GpuTexture* t);
    bool create_layouts();

    rhi::Context* ctx_ = nullptr;
    rhi::Allocator* alloc_ = nullptr;
    rhi::Uploader* up_ = nullptr;
    ParticleWorld* world_ = nullptr;
    ParticleTextureSource source_;
    rhi::PipelineCache cache_;
    rhi::SamplerCache samplers_;
    VkDescriptorSetLayout set0_ = VK_NULL_HANDLE, set1_ = VK_NULL_HANDLE;
    VkPipelineLayout layout_ = VK_NULL_HANDLE;
    VkPipeline cs_update_ = VK_NULL_HANDLE, cs_spawn_ = VK_NULL_HANDLE, cs_sort_ = VK_NULL_HANDLE, cs_clear_ = VK_NULL_HANDLE;
    std::map<std::tuple<uint32_t, VkFormat, VkFormat, VkFormat>, VkPipeline> gfx_;
    rhi::Buffer particles_, order_, death_;
    uint32_t capacity_ = 0;
    GpuTexture default_tex_;
    std::map<std::string, std::unique_ptr<GpuTexture>> textures_;
    bool sort_ok_ = false;
    bool enabled_ = true;
    Float3 wind_{0, 0, 0};
    bool fog_on_ = false;
    Float3 fog_color_{1, 1, 1};
    float fog_x_ = -0.0002f, fog_y_ = 3.0f;
    float sort_range_ = 20000.0f;
    // Frame state (prepare -> draw).
    rhi::Frame* frame_ = nullptr;
    VkDescriptorSet draw_set_ = VK_NULL_HANDLE;
    std::vector<SystemDraw> lists_[3];  // water, transparent, heat
    std::map<const GpuTexture*, VkDescriptorSet> tex_sets_;
    ParticleFrameStats stats_;
};

}  // namespace alamo::render::particles

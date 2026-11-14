// Graphics pipelines for dynamic rendering (no VkRenderPass) from a small description, and a
// VkPipelineCache persisted to disk. Dynamic state (always): viewport, scissor, stencil reference,
// stencil compare mask, stencil write mask (MeshShadowVolume uses 63), blend constants, depth bias
// (PORTING.md 3.2; see depth_bias_from_d3d9). The masks in DepthStencilState are therefore ignored:
// set them with vkCmdSetStencilCompareMask / vkCmdSetStencilWriteMask when stencil is enabled.
// D3D9 conventions to remember when filling this from effect state: D3D treats clockwise as front
// facing and D3DCULL_CCW culls counter-clockwise faces -> front_face = CLOCKWISE, cull = BACK.
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "alamo/rhi/vk.hpp"

namespace alamo::rhi {

class Context;

// SPIR-V words from bytes (DXC -Fh headers are unsigned char arrays with no alignment guarantee).
std::vector<uint32_t> spirv_from_bytes(std::span<const uint8_t> bytes);

struct VertexBinding {
    uint32_t binding = 0;
    uint32_t stride = 0;
    bool per_instance = false;
};
struct VertexAttribute {
    uint32_t location = 0;
    uint32_t binding = 0;
    VkFormat format = VK_FORMAT_R32G32B32_SFLOAT;
    uint32_t offset = 0;
};

struct BlendState {
    bool enable = false;
    VkBlendFactor src_color = VK_BLEND_FACTOR_ONE, dst_color = VK_BLEND_FACTOR_ZERO;
    VkBlendOp color_op = VK_BLEND_OP_ADD;
    VkBlendFactor src_alpha = VK_BLEND_FACTOR_ONE, dst_alpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp alpha_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags write_mask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
};

struct DepthStencilState {
    bool depth_test = false;
    bool depth_write = false;
    VkCompareOp depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    bool stencil_test = false;
    VkStencilOpState front{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
    VkStencilOpState back{VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_STENCIL_OP_KEEP, VK_COMPARE_OP_ALWAYS, 0xFF, 0xFF, 0};
};

struct RasterState {
    VkPolygonMode polygon = VK_POLYGON_MODE_FILL;
    VkCullModeFlags cull = VK_CULL_MODE_BACK_BIT;
    VkFrontFace front_face = VK_FRONT_FACE_CLOCKWISE;
    bool depth_bias = false;  // values set with vkCmdSetDepthBias
    bool depth_clamp = false;
};

struct SpecConstant {
    uint32_t id = 0;
    uint32_t value = 0;  // bit pattern (bool/int/uint/float)
};

// VK_EXT_pipeline_creation_feedback (core 1.3). Filled when a desc's `feedback` is set.
struct PipelineFeedback {
    VkPipelineCreationFeedback pipeline{};
    VkPipelineCreationFeedback stages[2]{};  // graphics: [0] vertex, [1] fragment; compute: [0]
    uint32_t stage_count = 0;
    bool valid() const { return (pipeline.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) != 0; }
    bool cache_hit() const {
        return (pipeline.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT) != 0;
    }
    double duration_ms() const { return static_cast<double>(pipeline.duration) * 1e-6; }
};

struct GraphicsPipelineDesc {
    std::span<const uint32_t> vs_spirv;
    std::span<const uint32_t> fs_spirv;  // may be empty (depth-only / stencil passes)
    const char* vs_entry = "main";
    const char* fs_entry = "main";
    std::span<const SpecConstant> vs_spec;
    std::span<const SpecConstant> fs_spec;

    std::span<const VertexBinding> bindings;
    std::span<const VertexAttribute> attributes;
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    RasterState raster;
    DepthStencilState depth;
    BlendState blend;  // applied to every color attachment
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    bool alpha_to_coverage = false;
    // Adds VK_DYNAMIC_STATE_STENCIL_OP: front/back fail/pass/depth-fail ops and compare op come from
    // vkCmdSetStencilOp (must be set before drawing when the stencil test is on). Lets one pipeline
    // serve both two-sided shadow-volume variants.
    bool dynamic_stencil_op = false;

    std::span<const VkFormat> color_formats;
    VkFormat depth_format = VK_FORMAT_UNDEFINED;
    VkFormat stencil_format = VK_FORMAT_UNDEFINED;

    VkPipelineLayout layout = VK_NULL_HANDLE;
    const char* name = nullptr;
    PipelineFeedback* feedback = nullptr;  // optional creation feedback (cache hit, duration)
};

struct ComputePipelineDesc {
    std::span<const uint32_t> cs_spirv;
    const char* entry = "main";
    std::span<const SpecConstant> spec;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    const char* name = nullptr;
    PipelineFeedback* feedback = nullptr;  // optional creation feedback (cache hit, duration)
};

// Generic pipeline layout from explicit set layouts + one push-constant range.
VkPipelineLayout create_pipeline_layout(const Context& ctx, std::span<const VkDescriptorSetLayout> sets,
                                        uint32_t push_bytes, VkShaderStageFlags push_stages);

// Header check for VkPipelineCache blobs (VkPipelineCacheHeaderVersionOne). Pure; exposed for tests.
bool pipeline_cache_header_matches(std::span<const uint8_t> blob, uint32_t vendor_id, uint32_t device_id,
                                   const uint8_t (&uuid)[VK_UUID_SIZE]);

// On-disk cache file = 24-byte alamo header {"ALPC", u32 version 1, u64 blob size, u64 FNV-1a-64 of
// the blob} + the driver blob. Pure; exposed for tests. unwrap returns false (blob untouched) for a
// truncated, oversized, wrong-magic/version or checksum-mismatched file; on success `blob` views
// the driver data inside `file`.
inline constexpr size_t kPipelineCacheFileHeader = 24;
std::vector<uint8_t> pipeline_cache_wrap(std::span<const uint8_t> driver_blob);
bool pipeline_cache_unwrap(std::span<const uint8_t> file, std::span<const uint8_t>& blob);

// Running totals over every create_graphics/create_compute of one PipelineCache, from creation
// feedback (always requested, whether or not the desc asks for it).
struct PipelineCacheStats {
    uint64_t created = 0;   // successful creations
    uint64_t failed = 0;    // creations that returned an error
    uint64_t hits = 0;      // feedback valid + APPLICATION_PIPELINE_CACHE_HIT
    uint64_t misses = 0;    // feedback valid, no hit
    uint64_t unknown = 0;   // driver left feedback invalid (hit/miss unknown)
    double total_ms = 0.0;  // summed pipeline creation duration of valid feedback
};

class PipelineCache {
public:
    PipelineCache() = default;
    ~PipelineCache();
    PipelineCache(const PipelineCache&) = delete;
    PipelineCache& operator=(const PipelineCache&) = delete;

    // Loads `file` when it exists, passes the length/checksum check (pipeline_cache_unwrap) and
    // matches this device/driver; otherwise starts empty (a corrupt file is not an error).
    bool init(Context& ctx, const std::filesystem::path& file = {});
    // Writes atomically: a uniquely named temp file (per process/thread/call), flushed to disk,
    // then renamed over the target. Concurrent saves never share a temp file; the last rename wins.
    // No-op without a path.
    bool save() const;
    void shutdown(bool save_first = true);
    VkPipelineCache handle() const { return cache_; }
    bool loaded_from_disk() const { return loaded_; }

    VkPipeline create_graphics(const GraphicsPipelineDesc& desc) const;  // VK_NULL_HANDLE on error
    VkPipeline create_compute(const ComputePipelineDesc& desc) const;

    // Thread-safe snapshot (create_* may run on worker threads). Reset by init() and reset_stats().
    PipelineCacheStats stats() const;
    void reset_stats();

private:
    void record(VkResult r, const PipelineFeedback& fb) const;
    mutable std::atomic<uint64_t> created_{0}, failed_{0}, hits_{0}, misses_{0}, unknown_{0}, total_ns_{0};
    Context* ctx_ = nullptr;
    VkPipelineCache cache_ = VK_NULL_HANDLE;
    std::filesystem::path file_;
    bool loaded_ = false;
};

}  // namespace alamo::rhi

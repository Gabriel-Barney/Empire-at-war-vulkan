#include "alamo/rhi/pipeline.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <string>
#include <system_error>

#include "alamo/core/atomic_file.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/rhi/context.hpp"

namespace alamo::rhi {

std::vector<uint32_t> spirv_from_bytes(std::span<const uint8_t> bytes) {
    std::vector<uint32_t> words(bytes.size() / 4);
    if (!words.empty()) std::memcpy(words.data(), bytes.data(), words.size() * 4);
    return words;
}

VkPipelineLayout create_pipeline_layout(const Context& ctx, std::span<const VkDescriptorSetLayout> sets,
                                        uint32_t push_bytes, VkShaderStageFlags push_stages) {
    VkPushConstantRange pc{push_stages, 0, push_bytes};
    auto ci = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
    ci.setLayoutCount = static_cast<uint32_t>(sets.size());
    ci.pSetLayouts = sets.data();
    ci.pushConstantRangeCount = push_bytes ? 1u : 0u;
    ci.pPushConstantRanges = &pc;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (!vk_check(ctx.vkd().vkCreatePipelineLayout(ctx.device(), &ci, nullptr, &layout), "vkCreatePipelineLayout"))
        return VK_NULL_HANDLE;
    return layout;
}

bool pipeline_cache_header_matches(std::span<const uint8_t> blob, uint32_t vendor_id, uint32_t device_id,
                                   const uint8_t (&uuid)[VK_UUID_SIZE]) {
    // VkPipelineCacheHeaderVersionOne: u32 headerSize, u32 version, u32 vendorID, u32 deviceID, u8 uuid[16].
    constexpr size_t kHeader = 16 + VK_UUID_SIZE;
    if (blob.size() < kHeader) return false;
    auto u32 = [&](size_t off) {
        return static_cast<uint32_t>(blob[off]) | static_cast<uint32_t>(blob[off + 1]) << 8 |
               static_cast<uint32_t>(blob[off + 2]) << 16 | static_cast<uint32_t>(blob[off + 3]) << 24;
    };
    const uint32_t header_size = u32(0);
    if (header_size < kHeader || header_size > blob.size()) return false;
    if (u32(4) != static_cast<uint32_t>(VK_PIPELINE_CACHE_HEADER_VERSION_ONE)) return false;
    if (u32(8) != vendor_id || u32(12) != device_id) return false;
    return std::memcmp(blob.data() + 16, uuid, VK_UUID_SIZE) == 0;
}

namespace {

constexpr uint8_t kCacheMagic[4] = {'A', 'L', 'P', 'C'};
constexpr uint32_t kCacheVersion = 1;

void put_le(std::vector<uint8_t>& out, uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

uint64_t get_le(std::span<const uint8_t> in, size_t off, int bytes) {
    uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) v |= static_cast<uint64_t>(in[off + static_cast<size_t>(i)]) << (8 * i);
    return v;
}

}  // namespace

std::vector<uint8_t> pipeline_cache_wrap(std::span<const uint8_t> driver_blob) {
    std::vector<uint8_t> out;
    out.reserve(kPipelineCacheFileHeader + driver_blob.size());
    out.insert(out.end(), std::begin(kCacheMagic), std::end(kCacheMagic));
    put_le(out, kCacheVersion, 4);
    put_le(out, driver_blob.size(), 8);
    put_le(out, alamo::fnv1a64(driver_blob), 8);
    out.insert(out.end(), driver_blob.begin(), driver_blob.end());
    return out;
}

bool pipeline_cache_unwrap(std::span<const uint8_t> file, std::span<const uint8_t>& blob) {
    if (file.size() < kPipelineCacheFileHeader) return false;
    if (std::memcmp(file.data(), kCacheMagic, sizeof kCacheMagic) != 0) return false;
    if (get_le(file, 4, 4) != kCacheVersion) return false;
    const uint64_t size = get_le(file, 8, 8);
    if (size != file.size() - kPipelineCacheFileHeader) return false;  // truncated or trailing bytes
    const std::span<const uint8_t> body = file.subspan(kPipelineCacheFileHeader);
    if (get_le(file, 16, 8) != alamo::fnv1a64(body)) return false;
    blob = body;
    return true;
}

PipelineCache::~PipelineCache() { shutdown(false); }

bool PipelineCache::init(Context& ctx, const std::filesystem::path& file) {
    shutdown(false);
    reset_stats();
    ctx_ = &ctx;
    file_ = file;
    std::vector<uint8_t> raw;
    std::span<const uint8_t> blob;
    if (!file.empty()) {
        // fs::path overload: no UTF-8 round trip (that turned "\\?\C:\x" into "//?/C:/x"); applies
        // long_path itself and opens with FILE_SHARE_DELETE so it never blocks a concurrent save().
        if (read_host_file(file, raw)) {
            const DeviceCaps& c = ctx.caps();
            if (!pipeline_cache_unwrap(raw, blob)) {
                log::infof("rhi", "pipeline cache %s is truncated or corrupt; starting empty",
                           path_to_utf8(file).c_str());
                blob = {};
            } else if (!pipeline_cache_header_matches(blob, c.vendor_id, c.device_id, c.pipeline_cache_uuid)) {
                log::infof("rhi", "pipeline cache %s is for another device/driver; starting empty",
                           path_to_utf8(file).c_str());
                blob = {};
            }
        }
    }
    auto ci = vk_struct<VkPipelineCacheCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO);
    ci.initialDataSize = blob.size();
    ci.pInitialData = blob.empty() ? nullptr : blob.data();
    VkResult r = ctx.vkd().vkCreatePipelineCache(ctx.device(), &ci, nullptr, &cache_);
    if (r != VK_SUCCESS && !blob.empty()) {  // driver rejected the blob: retry empty
        ci.initialDataSize = 0;
        ci.pInitialData = nullptr;
        blob = {};
        r = ctx.vkd().vkCreatePipelineCache(ctx.device(), &ci, nullptr, &cache_);
    }
    if (!vk_check(r, "vkCreatePipelineCache")) {
        cache_ = VK_NULL_HANDLE;
        return false;
    }
    loaded_ = !blob.empty();
    return true;
}

bool PipelineCache::save() const {
    if (!cache_ || file_.empty()) return true;
    size_t size = 0;
    const DeviceFns& fn = ctx_->vkd();
    if (!vk_check(fn.vkGetPipelineCacheData(ctx_->device(), cache_, &size, nullptr), "vkGetPipelineCacheData"))
        return false;
    std::vector<uint8_t> data(size);
    if (!vk_check(fn.vkGetPipelineCacheData(ctx_->device(), cache_, &size, data.data()), "vkGetPipelineCacheData"))
        return false;
    data.resize(size);
    const std::vector<uint8_t> wrapped = pipeline_cache_wrap(data);
    std::error_code ec;
    const std::filesystem::path target = long_path(file_);  // "\\?\" form on Windows: no MAX_PATH limit
    if (target.has_parent_path()) std::filesystem::create_directories(target.parent_path(), ec);
    // The temp file is flushed to the device before the rename. A concurrent save, or a foreign reader
    // without FILE_SHARE_DELETE, may hold the target briefly (Windows). Bounded: 10 attempts, back-off
    // 1, 2, 4, ... capped at 50 ms (~0.26 s worst case). A lost race stays a failed save, as before.
    AtomicWriteOptions opt;
    opt.attempts = 10;
    opt.max_backoff_ms = 50;
    opt.flush_to_disk = true;
    const AtomicWriteResult r = write_file_atomic(target, wrapped, opt);
    if (r == AtomicWriteResult::WriteFailed) {
        log::warnf("rhi", "pipeline cache save failed: cannot write a temp file beside %s", path_to_utf8(target).c_str());
        return false;
    }
    if (r == AtomicWriteResult::ReplaceFailed) {
        log::warnf("rhi", "pipeline cache save failed: cannot replace %s", path_to_utf8(target).c_str());
        return false;
    }
    return true;
}

void PipelineCache::record(VkResult r, const PipelineFeedback& fb) const {
    if (r != VK_SUCCESS) {
        failed_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    created_.fetch_add(1, std::memory_order_relaxed);
    if (!fb.valid()) {
        unknown_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    (fb.cache_hit() ? hits_ : misses_).fetch_add(1, std::memory_order_relaxed);
    total_ns_.fetch_add(fb.pipeline.duration, std::memory_order_relaxed);
}

PipelineCacheStats PipelineCache::stats() const {
    PipelineCacheStats s;
    s.created = created_.load(std::memory_order_relaxed);
    s.failed = failed_.load(std::memory_order_relaxed);
    s.hits = hits_.load(std::memory_order_relaxed);
    s.misses = misses_.load(std::memory_order_relaxed);
    s.unknown = unknown_.load(std::memory_order_relaxed);
    s.total_ms = static_cast<double>(total_ns_.load(std::memory_order_relaxed)) * 1e-6;
    return s;
}

void PipelineCache::reset_stats() {
    for (std::atomic<uint64_t>* a : {&created_, &failed_, &hits_, &misses_, &unknown_, &total_ns_})
        a->store(0, std::memory_order_relaxed);
}

void PipelineCache::shutdown(bool save_first) {
    if (!ctx_) return;
    if (cache_) {
        if (save_first) save();
        ctx_->vkd().vkDestroyPipelineCache(ctx_->device(), cache_, nullptr);
    }
    cache_ = VK_NULL_HANDLE;
    ctx_ = nullptr;
    loaded_ = false;
}

namespace {

VkShaderModule make_module(const Context& ctx, std::span<const uint32_t> code) {
    auto ci = vk_struct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
    ci.codeSize = code.size() * 4;
    ci.pCode = code.data();
    VkShaderModule m = VK_NULL_HANDLE;
    if (!vk_check(ctx.vkd().vkCreateShaderModule(ctx.device(), &ci, nullptr, &m), "vkCreateShaderModule"))
        return VK_NULL_HANDLE;
    return m;
}

struct SpecData {
    std::vector<VkSpecializationMapEntry> entries;
    std::vector<uint32_t> values;
    VkSpecializationInfo info{};
    const VkSpecializationInfo* build(std::span<const SpecConstant> sc) {
        if (sc.empty()) return nullptr;
        for (size_t i = 0; i < sc.size(); ++i) {
            entries.push_back({sc[i].id, static_cast<uint32_t>(i * 4), 4});
            values.push_back(sc[i].value);
        }
        info.mapEntryCount = static_cast<uint32_t>(entries.size());
        info.pMapEntries = entries.data();
        info.dataSize = values.size() * 4;
        info.pData = values.data();
        return &info;
    }
};

}  // namespace

VkPipeline PipelineCache::create_graphics(const GraphicsPipelineDesc& d) const {
    if (d.vs_spirv.empty() || !d.layout) {
        log::errorf("rhi", "create_graphics: missing vertex shader or layout");
        return VK_NULL_HANDLE;
    }
    const Context& ctx = *ctx_;
    VkShaderModule vs = make_module(ctx, d.vs_spirv);
    VkShaderModule fs = d.fs_spirv.empty() ? VK_NULL_HANDLE : make_module(ctx, d.fs_spirv);
    if (!vs || (!d.fs_spirv.empty() && !fs)) {
        if (vs) ctx.vkd().vkDestroyShaderModule(ctx.device(), vs, nullptr);
        if (fs) ctx.vkd().vkDestroyShaderModule(ctx.device(), fs, nullptr);
        return VK_NULL_HANDLE;
    }

    SpecData vs_spec, fs_spec;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = d.vs_entry;
    stages[0].pSpecializationInfo = vs_spec.build(d.vs_spec);
    uint32_t stage_count = 1;
    if (fs) {
        stages[1] = {};
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs;
        stages[1].pName = d.fs_entry;
        stages[1].pSpecializationInfo = fs_spec.build(d.fs_spec);
        stage_count = 2;
    }

    std::vector<VkVertexInputBindingDescription> vb;
    for (const VertexBinding& b : d.bindings)
        vb.push_back({b.binding, b.stride, b.per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    std::vector<VkVertexInputAttributeDescription> va;
    for (const VertexAttribute& a : d.attributes) va.push_back({a.location, a.binding, a.format, a.offset});
    auto vi = vk_struct<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
    vi.vertexBindingDescriptionCount = static_cast<uint32_t>(vb.size());
    vi.pVertexBindingDescriptions = vb.data();
    vi.vertexAttributeDescriptionCount = static_cast<uint32_t>(va.size());
    vi.pVertexAttributeDescriptions = va.data();

    auto ia = vk_struct<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
    ia.topology = d.topology;

    auto vp = vk_struct<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    auto rs = vk_struct<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
    // LINE/POINT need fillModeNonSolid (enabled whenever the device has it); otherwise draw solid.
    rs.polygonMode = d.raster.polygon == VK_POLYGON_MODE_FILL || ctx.caps().fill_mode_non_solid
                         ? d.raster.polygon
                         : VK_POLYGON_MODE_FILL;
    rs.cullMode = d.raster.cull;
    rs.frontFace = d.raster.front_face;
    rs.depthBiasEnable = d.raster.depth_bias ? VK_TRUE : VK_FALSE;
    rs.depthClampEnable = d.raster.depth_clamp && ctx.caps().depth_clamp ? VK_TRUE : VK_FALSE;
    rs.lineWidth = 1.0f;

    auto ms = vk_struct<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
    ms.rasterizationSamples = d.samples;
    ms.alphaToCoverageEnable = d.alpha_to_coverage ? VK_TRUE : VK_FALSE;

    auto ds = vk_struct<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
    ds.depthTestEnable = d.depth.depth_test ? VK_TRUE : VK_FALSE;
    ds.depthWriteEnable = d.depth.depth_write ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = d.depth.depth_op;
    ds.stencilTestEnable = d.depth.stencil_test ? VK_TRUE : VK_FALSE;
    ds.front = d.depth.front;
    ds.back = d.depth.back;
    ds.minDepthBounds = 0.0f;
    ds.maxDepthBounds = 1.0f;

    std::vector<VkPipelineColorBlendAttachmentState> att(d.color_formats.size());
    for (auto& a : att) {
        a.blendEnable = d.blend.enable ? VK_TRUE : VK_FALSE;
        a.srcColorBlendFactor = d.blend.src_color;
        a.dstColorBlendFactor = d.blend.dst_color;
        a.colorBlendOp = d.blend.color_op;
        a.srcAlphaBlendFactor = d.blend.src_alpha;
        a.dstAlphaBlendFactor = d.blend.dst_alpha;
        a.alphaBlendOp = d.blend.alpha_op;
        a.colorWriteMask = d.blend.write_mask;
    }
    auto cb = vk_struct<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
    cb.attachmentCount = static_cast<uint32_t>(att.size());
    cb.pAttachments = att.data();

    VkDynamicState dyn[8] = {VK_DYNAMIC_STATE_VIEWPORT,           VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE,  VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                             VK_DYNAMIC_STATE_DEPTH_BIAS,         VK_DYNAMIC_STATE_STENCIL_OP};
    auto dy = vk_struct<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
    dy.dynamicStateCount = d.dynamic_stencil_op ? 8u : 7u;
    dy.pDynamicStates = dyn;

    auto ri = vk_struct<VkPipelineRenderingCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO);
    ri.colorAttachmentCount = static_cast<uint32_t>(d.color_formats.size());
    ri.pColorAttachmentFormats = d.color_formats.data();
    ri.depthAttachmentFormat = d.depth_format;
    ri.stencilAttachmentFormat = d.stencil_format;

    // Feedback is always requested (core 1.3) so stats() sees every creation.
    PipelineFeedback local_fb;
    PipelineFeedback& out_fb = d.feedback ? *d.feedback : local_fb;
    out_fb = PipelineFeedback{};
    out_fb.stage_count = stage_count;
    auto fb = vk_struct<VkPipelineCreationFeedbackCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
    fb.pPipelineCreationFeedback = &out_fb.pipeline;
    fb.pipelineStageCreationFeedbackCount = stage_count;
    fb.pPipelineStageCreationFeedbacks = out_fb.stages;
    ri.pNext = &fb;
    auto ci = vk_struct<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
    ci.pNext = &ri;
    ci.stageCount = stage_count;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = d.layout;

    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = ctx.vkd().vkCreateGraphicsPipelines(ctx.device(), cache_, 1, &ci, nullptr, &p);
    ctx.vkd().vkDestroyShaderModule(ctx.device(), vs, nullptr);
    if (fs) ctx.vkd().vkDestroyShaderModule(ctx.device(), fs, nullptr);
    record(r, out_fb);
    if (!vk_check(r, "vkCreateGraphicsPipelines")) return VK_NULL_HANDLE;
    if (d.name) ctx.set_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(p), d.name);
    return p;
}

}  // namespace alamo::rhi

namespace alamo::rhi {

VkPipeline PipelineCache::create_compute(const ComputePipelineDesc& d) const {
    if (d.cs_spirv.empty() || !d.layout) {
        log::errorf("rhi", "create_compute: missing shader or layout");
        return VK_NULL_HANDLE;
    }
    const Context& ctx = *ctx_;
    VkShaderModule cs = make_module(ctx, d.cs_spirv);
    if (!cs) return VK_NULL_HANDLE;
    SpecData spec;
    auto ci = vk_struct<VkComputePipelineCreateInfo>(VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO);
    ci.stage = vk_struct<VkPipelineShaderStageCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO);
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = cs;
    ci.stage.pName = d.entry;
    ci.stage.pSpecializationInfo = spec.build(d.spec);
    ci.layout = d.layout;
    PipelineFeedback local_fb;
    PipelineFeedback& out_fb = d.feedback ? *d.feedback : local_fb;
    out_fb = PipelineFeedback{};
    out_fb.stage_count = 1;
    auto fb = vk_struct<VkPipelineCreationFeedbackCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO);
    fb.pPipelineCreationFeedback = &out_fb.pipeline;
    fb.pipelineStageCreationFeedbackCount = 1;
    fb.pPipelineStageCreationFeedbacks = out_fb.stages;
    ci.pNext = &fb;
    VkPipeline p = VK_NULL_HANDLE;
    const VkResult r = ctx.vkd().vkCreateComputePipelines(ctx.device(), cache_, 1, &ci, nullptr, &p);
    ctx.vkd().vkDestroyShaderModule(ctx.device(), cs, nullptr);
    record(r, out_fb);
    if (!vk_check(r, "vkCreateComputePipelines")) return VK_NULL_HANDLE;
    if (d.name) ctx.set_name(VK_OBJECT_TYPE_PIPELINE, reinterpret_cast<uint64_t>(p), d.name);
    return p;
}

}  // namespace alamo::rhi
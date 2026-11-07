#include "alamo/render/particles/particle_gpu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "alamo/core/log.hpp"
#include "alamo/render/convert.hpp"
#include "alamo/render/frustum.hpp"
#include "particles_cs_clear.h"
#include "particles_cs_sort.h"
#include "particles_cs_spawn.h"
#include "particles_cs_update.h"
#include "particles_ps.h"
#include "particles_vs.h"

namespace alamo::render::particles {

namespace {

// GPU records (particles.hlsl; every member 16-byte aligned, so std430 = the C++ layout).
struct ParticleGpu {
    float v[16];
};
static_assert(sizeof(ParticleGpu) == 64);
struct ShapeGpu {
    uint32_t kind[4];
    float a[4], b[4], c[4], d[4];
};
struct DescGpu {
    float accel[4];
    float motion[4];
    uint32_t flags[4];
    float rcolor[4];
    float rot[4];
    float weather[4];
    ShapeGpu vel, pos;
    uint32_t interp[8];
    float tracks[kTrackCount * kTrackSamples];
};
static_assert(sizeof(DescGpu) == 1184);
struct EmitterGpu {
    uint32_t info[4];
    int32_t link[4];
    float prev[12];
    float cur[12];
    float vel_d[4];
    float color[4];  // draw: system colour multiplier (ParticleWorld scalar index 4); update: 1
};
static_assert(sizeof(EmitterGpu) == 160);
struct FrameConsts {
    float view_proj[16];
    float right[4], up[4], fwd[4], eye[4], wind[4];
    uint32_t counts[4];
    float fog_color[4];  // scene fog (set_scene_fog)
    float fog_vals[4];   // FOG_VALS x, y
};
static_assert(sizeof(FrameConsts) == 192);
struct Push {
    uint32_t emitter = 0, mode = 0, fog = 0, pad1 = 0;
};

enum : uint32_t {
    kFObjectSpace = 1u, kFObjectSpaceAccel = 2u, kFWind = 4u, kFRotationFixed = 8u, kFRandomDirection = 16u,
    kFColorUniform = 32u, kFWeather = 64u, kFTail = 128u, kFAdditiveFade = 256u, kFTriangle = 512u,
};
enum : uint32_t { kEParent = 1u, kESorted = 2u };

ShapeGpu shape_gpu(const Shape& s) {
    ShapeGpu g{};
    g.kind[0] = s.kind;
    g.kind[1] = s.sphere_surface ? 1u : 0u;
    g.kind[2] = s.cylinder_surface ? 1u : 0u;
    for (int i = 0; i < 3; ++i) {
        g.a[i] = s.box_min[i];
        g.b[i] = s.box_max[i];
        g.c[i] = s.point[i];
    }
    g.a[3] = s.cube_half;
    g.b[3] = s.sphere_radius;
    g.c[3] = s.cylinder_radius;
    g.d[0] = s.cylinder_height;
    return g;
}

DescGpu desc_gpu(const EmitterDef& e) {
    DescGpu d{};
    for (int i = 0; i < 3; ++i) d.accel[i] = e.weather ? 0.0f : e.acceleration[i];
    d.accel[3] = e.weather ? 0.0f : e.gravity;
    d.motion[0] = e.outward_speed;
    d.motion[1] = e.weather ? 0.0f : e.outward_acceleration;
    d.motion[2] = e.inherit_velocity ? e.inherit_velocity_factor : 0.0f;
    d.motion[3] = e.tail_size;
    uint32_t f = 0;
    if (e.object_space) f |= kFObjectSpace;
    if (e.object_space_acceleration) f |= kFObjectSpaceAccel;
    if (e.affected_by_wind) f |= kFWind;
    if (e.rotation_fixed_at_spawn) f |= kFRotationFixed;
    if (e.random_rotation_direction) f |= kFRandomDirection;
    if (e.random_color_uniform) f |= kFColorUniform;
    if (e.weather) f |= kFWeather;
    if (e.has_tail) f |= kFTail;
    if (e.blend_mode == 1 || e.blend_mode == 4) f |= kFAdditiveFade;
    if (e.geometry == 0) f |= kFTriangle;
    d.flags[0] = f;
    d.flags[1] = e.blend_mode;
    d.flags[2] = std::max<uint32_t>(e.texture_cells, 1u);
    d.flags[3] = std::max<uint32_t>(static_cast<uint32_t>(std::lround(std::sqrt(static_cast<double>(d.flags[2])))), 1u);
    for (int i = 0; i < 4; ++i) d.rcolor[i] = e.random_color[i];
    d.rot[0] = e.random_initial_rotation;
    d.rot[1] = e.random_rotation_speed;
    d.rot[2] = e.weather_cube_size;
    d.rot[3] = e.weather_camera_distance;
    d.weather[0] = e.weather_fade_start;
    d.weather[1] = e.weather_fade_length;
    d.vel = shape_gpu(e.velocity);
    d.pos = shape_gpu(e.position);
    for (uint32_t t = 0; t < kTrackCount; ++t) {
        d.interp[t] = e.tracks[t].interpolation;
        for (uint32_t i = 0; i < kTrackSamples; ++i) d.tracks[t * kTrackSamples + i] = e.tracks[t].v[i];
    }
    return d;
}

void rows(const Mat3x4& m, float (&out)[12]) {
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m.m[r][c];
}

// Draw mode of a blend mode (SPEC 3.1 table): 0 opaque, 1 additive, 2 alpha, 3 modulate.
uint32_t draw_mode(uint32_t blend) {
    switch (blend) {
        case 0: return 0;
        case 1:
        case 4: return 1;
        case 3:
        case 6: return 3;
        default: return 2;
    }
}

VkDescriptorBufferInfo range_of(const rhi::UniformAlloc& a) { return {a.buffer, a.offset, a.size}; }

}  // namespace

ParticleRenderer::ParticleRenderer() = default;
ParticleRenderer::~ParticleRenderer() { shutdown(); }

bool ParticleRenderer::create_layouts() {
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkShaderStageFlags all = VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    std::array<VkDescriptorSetLayoutBinding, 8> b0{};
    for (uint32_t i = 0; i < 8; ++i) {
        b0[i].binding = i;
        b0[i].descriptorType = i == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b0[i].descriptorCount = 1;
        b0[i].stageFlags = all;
    }
    auto ci = rhi::vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
    ci.bindingCount = static_cast<uint32_t>(b0.size());
    ci.pBindings = b0.data();
    if (fn.vkCreateDescriptorSetLayout(ctx_->device(), &ci, nullptr, &set0_) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b1{};
    b1.binding = 0;
    b1.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b1.descriptorCount = 1;
    b1.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    ci.bindingCount = 1;
    ci.pBindings = &b1;
    if (fn.vkCreateDescriptorSetLayout(ctx_->device(), &ci, nullptr, &set1_) != VK_SUCCESS) return false;
    const VkDescriptorSetLayout sets[2] = {set0_, set1_};
    layout_ = rhi::create_pipeline_layout(*ctx_, sets, sizeof(Push), all);
    return layout_ != VK_NULL_HANDLE;
}

bool ParticleRenderer::init(rhi::Context& ctx, rhi::Allocator& alloc, rhi::Uploader& up, ParticleWorld& world,
                            ParticleTextureSource textures) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    up_ = &up;
    world_ = &world;
    source_ = std::move(textures);
    capacity_ = world.gpu_capacity();
    samplers_.init(ctx);
    if (!cache_.init(ctx) || !create_layouts()) {
        log::errorf("particles", "pipeline layout creation failed");
        shutdown();
        return false;
    }
    auto compute = [&](std::span<const unsigned char> code, const char* entry, const char* name) {
        const std::vector<uint32_t> spv = rhi::spirv_from_bytes(std::span<const uint8_t>(code.data(), code.size()));
        rhi::ComputePipelineDesc d;
        d.cs_spirv = spv;
        d.entry = entry;
        d.layout = layout_;
        d.name = name;
        return cache_.create_compute(d);
    };
    cs_update_ = compute(g_particles_cs_update, "CSUpdate", "particles.update");
    cs_spawn_ = compute(g_particles_cs_spawn, "CSSpawn", "particles.spawn");
    cs_clear_ = compute(g_particles_cs_clear, "CSClear", "particles.clear");
    // CSSort: 256 threads, 32 KiB of group-shared keys.
    VkPhysicalDeviceProperties props{};
    ctx.vki().vkGetPhysicalDeviceProperties(ctx.physical_device(), &props);
    sort_ok_ = props.limits.maxComputeSharedMemorySize >= 32768 && props.limits.maxComputeWorkGroupInvocations >= 256 &&
               props.limits.maxComputeWorkGroupSize[0] >= 256;
    if (sort_ok_) cs_sort_ = compute(g_particles_cs_sort, "CSSort", "particles.sort");
    sort_ok_ = sort_ok_ && cs_sort_ != VK_NULL_HANDLE;
    if (!cs_update_ || !cs_spawn_ || !cs_clear_) {
        log::errorf("particles", "compute pipeline creation failed");
        shutdown();
        return false;
    }
    const VkBufferUsageFlags su = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!alloc.create_buffer(static_cast<VkDeviceSize>(capacity_) * sizeof(ParticleGpu), su, rhi::MemoryUsage::GpuOnly, particles_,
                             "particles.pool") ||
        !alloc.create_buffer(static_cast<VkDeviceSize>(capacity_) * 4, su, rhi::MemoryUsage::GpuOnly, order_, "particles.order") ||
        !alloc.create_buffer(static_cast<VkDeviceSize>(capacity_) * 16, su, rhi::MemoryUsage::GpuOnly, death_, "particles.death")) {
        log::errorf("particles", "particle buffers (%u particles) failed", capacity_);
        shutdown();
        return false;
    }
    // Built-in soft dot (missing / fallback textures): 64x64 white, alpha = radial falloff.
    {
        assets::TextureAsset t;
        t.name = "<particle dot>";
        t.format = assets::TextureFormat::RGBA8;
        t.width = t.height = 64;
        t.mip_levels = 1;
        t.data.resize(64 * 64 * 4);
        for (uint32_t y = 0; y < 64; ++y)
            for (uint32_t x = 0; x < 64; ++x) {
                const float dx = (static_cast<float>(x) + 0.5f) / 32.0f - 1.0f, dy = (static_cast<float>(y) + 0.5f) / 32.0f - 1.0f;
                const float a = std::clamp(1.0f - std::sqrt(dx * dx + dy * dy), 0.0f, 1.0f);
                uint8_t* p = &t.data[(y * 64 + x) * 4];
                p[0] = p[1] = p[2] = 255;
                p[3] = static_cast<uint8_t>(std::lround(a * a * 255.0f));
            }
        t.levels.push_back({0, 0, 64, 64, 0, t.data.size()});
        rhi::ImageDesc d;
        d.format = rhi::Format::RGBA8;
        d.width = d.height = 64;
        d.mips = 1;
        d.name = "particles.dot";
        if (!alloc.create_image(d, default_tex_.image)) {
            shutdown();
            return false;
        }
        const rhi::SubresourceData sub{0, 0, t.level_bytes(t.levels[0])};
        default_tex_.ticket = up.upload_image(default_tex_.image, std::span<const rhi::SubresourceData>(&sub, 1));
        default_tex_.ok = static_cast<bool>(default_tex_.ticket);
        if (default_tex_.ok) default_tex_.ok = up.finish(default_tex_.ticket);
    }
    world.set_record_packets(true);
    // Every range allocated before init must be cleared on the GPU before its first use.
    return true;
}

void ParticleRenderer::shutdown() {
    if (!ctx_) return;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkDevice dev = ctx_->device();
    for (auto& [k, p] : gfx_)
        if (p) fn.vkDestroyPipeline(dev, p, nullptr);
    gfx_.clear();
    for (VkPipeline* p : {&cs_update_, &cs_spawn_, &cs_sort_, &cs_clear_}) {
        if (*p) fn.vkDestroyPipeline(dev, *p, nullptr);
        *p = VK_NULL_HANDLE;
    }
    if (layout_) fn.vkDestroyPipelineLayout(dev, layout_, nullptr);
    if (set0_) fn.vkDestroyDescriptorSetLayout(dev, set0_, nullptr);
    if (set1_) fn.vkDestroyDescriptorSetLayout(dev, set1_, nullptr);
    layout_ = VK_NULL_HANDLE;
    set0_ = set1_ = VK_NULL_HANDLE;
    for (auto& [n, t] : textures_)
        if (t->image) {
            up_->forget(t->image.handle);
            alloc_->destroy(t->image);
        }
    textures_.clear();
    if (default_tex_.image) {
        up_->forget(default_tex_.image.handle);
        alloc_->destroy(default_tex_.image);
    }
    default_tex_ = GpuTexture{};
    for (rhi::Buffer* b : {&particles_, &order_, &death_})
        if (*b) alloc_->destroy(*b);
    cache_.shutdown(false);
    samplers_.shutdown();
    if (world_) world_->set_record_packets(false);
    for (auto& l : lists_) l.clear();
    tex_sets_.clear();
    ctx_ = nullptr;
    world_ = nullptr;
}

const ParticleRenderer::GpuTexture* ParticleRenderer::texture(const std::string& name) {
    if (name.empty() || !source_) return &default_tex_;
    auto [it, inserted] = textures_.try_emplace(name);
    if (inserted) {
        it->second = std::make_unique<GpuTexture>();
        GpuTexture& g = *it->second;
        const std::shared_ptr<const assets::TextureAsset> t = source_(name);
        rhi::Swizzle sw = rhi::Swizzle::Identity;
        const rhi::Format f = t ? to_rhi_format(t->format) : rhi::Format::Undefined;
        const bool usable = t && !t->is_fallback && f != rhi::Format::Undefined && !t->cubemap && t->layers == 1 && t->mip_levels > 0 &&
                            t->levels.size() == t->mip_levels && to_rhi_swizzle(t->swizzle, sw) &&
                            (!rhi::format_info(f).compressed || ctx_->caps().texture_bc);
        if (usable) {
            rhi::ImageDesc d;
            d.format = f;
            d.width = t->width;
            d.height = t->height;
            d.mips = t->mip_levels;
            d.swizzle = sw;
            d.name = name.c_str();
            if (alloc_->create_image(d, g.image)) {
                std::vector<rhi::SubresourceData> subs;
                for (const assets::TextureLevel& l : t->levels) subs.push_back({l.mip, l.layer, t->level_bytes(l)});
                g.ticket = up_->upload_image(g.image, subs, rhi::ImageState::ShaderRead);
                g.ok = static_cast<bool>(g.ticket);
            }
        }
        if (!g.ok) log::warnf("particles", "texture %s missing / unusable: soft dot used", name.c_str());
    }
    const GpuTexture& g = *it->second;
    if (!g.ok) return &default_tex_;
    if (!up_->ready(g.ticket)) {
        ++stats_.textures_pending;
        return &default_tex_;
    }
    return &g;
}

VkDescriptorSet ParticleRenderer::texture_set(rhi::Frame& frame, const GpuTexture* t) {
    auto it = tex_sets_.find(t);
    if (it != tex_sets_.end()) return it->second;
    const VkDescriptorSet s = frame.descriptors(0).allocate(set1_);
    if (s) {
        VkDescriptorImageInfo ii{samplers_.get(rhi::SamplerKey::linear_clamp()), t->image.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        auto w = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        w.dstSet = s;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &ii;
        ctx_->vkd().vkUpdateDescriptorSets(ctx_->device(), 1, &w, 0, nullptr);
    }
    tex_sets_.emplace(t, s);
    return s;
}

VkPipeline ParticleRenderer::pipeline(uint32_t mode, const SceneHookPass& p) {
    const auto key = std::make_tuple(mode, p.color_format, p.depth_format, p.stencil_format);
    auto it = gfx_.find(key);
    if (it != gfx_.end()) return it->second;
    const std::vector<uint32_t> vs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_particles_vs, sizeof g_particles_vs));
    const std::vector<uint32_t> fs = rhi::spirv_from_bytes(std::span<const uint8_t>(g_particles_ps, sizeof g_particles_ps));
    rhi::GraphicsPipelineDesc d;
    d.vs_spirv = vs;
    d.fs_spirv = fs;
    d.vs_entry = "VSMain";
    d.fs_entry = "PSMain";
    d.raster.cull = VK_CULL_MODE_NONE;
    d.depth.depth_test = p.depth_format != VK_FORMAT_UNDEFINED;
    d.depth.depth_write = mode == 0 && d.depth.depth_test;
    d.depth.depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
    switch (mode) {
        case 1:  // ONE / ONE
            d.blend.enable = true;
            d.blend.src_color = d.blend.dst_color = VK_BLEND_FACTOR_ONE;
            d.blend.src_alpha = VK_BLEND_FACTOR_ZERO;
            d.blend.dst_alpha = VK_BLEND_FACTOR_ONE;
            break;
        case 2:  // SRCALPHA / INVSRCALPHA
            d.blend.enable = true;
            d.blend.src_color = VK_BLEND_FACTOR_SRC_ALPHA;
            d.blend.dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            d.blend.src_alpha = VK_BLEND_FACTOR_SRC_ALPHA;
            d.blend.dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            break;
        case 3:  // ZERO / SRCCOLOR
            d.blend.enable = true;
            d.blend.src_color = VK_BLEND_FACTOR_ZERO;
            d.blend.dst_color = VK_BLEND_FACTOR_SRC_COLOR;
            d.blend.src_alpha = VK_BLEND_FACTOR_ZERO;
            d.blend.dst_alpha = VK_BLEND_FACTOR_ONE;
            break;
        default: break;
    }
    const VkFormat color = p.color_format;
    d.color_formats = std::span<const VkFormat>(&color, 1);
    d.depth_format = p.depth_format;
    d.stencil_format = p.stencil_format;
    d.layout = layout_;
    d.name = "particles.draw";
    const VkPipeline pl = cache_.create_graphics(d);
    if (!pl) log::errorf("particles", "draw pipeline (mode %u) failed", mode);
    gfx_.emplace(key, pl);
    return pl;
}

void ParticleRenderer::prepare(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view) {
    for (auto& l : lists_) l.clear();
    tex_sets_.clear();
    draw_set_ = VK_NULL_HANDLE;
    frame_ = &frame;
    stats_ = ParticleFrameStats{};
    if (!ctx_ || !world_) return;
    const rhi::DeviceFns& fn = ctx_->vkd();
    std::vector<UpdatePacket> packets = world_->take_packets();
    stats_.packets = static_cast<uint32_t>(packets.size());

    // Descriptor tables of this frame: one DescGpu per distinct emitter definition.
    std::unordered_map<const EmitterDef*, uint32_t> desc_index;
    std::vector<const EmitterDef*> desc_list;
    auto desc_of = [&](const EmitterDef* e) {
        auto [it, inserted] = desc_index.try_emplace(e, static_cast<uint32_t>(desc_list.size()));
        if (inserted) desc_list.push_back(e);
        return it->second;
    };
    // Camera.
    FrameConsts fc{};
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) fc.view_proj[r * 4 + c] = view.view_proj.m[r][c];
    for (int c = 0; c < 3; ++c) {
        fc.right[c] = view.view.m[0][c];
        fc.up[c] = view.view.m[1][c];
        fc.fwd[c] = -view.view.m[2][c];
    }
    fc.eye[0] = view.eye.x;
    fc.eye[1] = view.eye.y;
    fc.eye[2] = view.eye.z;
    fc.eye[3] = sort_range_;
    fc.wind[0] = wind_.x;
    fc.wind[1] = wind_.y;
    fc.wind[2] = wind_.z;
    fc.fog_color[0] = fog_color_.x;
    fc.fog_color[1] = fog_color_.y;
    fc.fog_color[2] = fog_color_.z;
    fc.fog_vals[0] = fog_x_;
    fc.fog_vals[1] = fog_y_;

    // ---- Draw list (B.5): visible systems with parent fade > 0.01, AABB vs frustum, back-to-front.
    std::vector<EmitterGpu> draw_table;
    struct Pending {
        uint32_t bucket;
        float distance;
        std::vector<DrawItem> items;
        std::vector<const EmitterDef*> defs;
    };
    std::vector<Pending> pend;
    std::vector<uint32_t> sort_entries;
    const Frustum fr = frustum_of(view.view_proj);
    world_->for_each([&](uint32_t index, const SystemState& s) {
        ++stats_.systems;
        if (!enabled_ || !s.visible || !(s.fade > 0.01f)) {
            ++stats_.systems_hidden;
            return;
        }
        const Aabb box = world_->bounds(s);  // empty emitters: a unit box at the system (B.5)
        if (!aabb_visible(fr, box)) {
            ++stats_.systems_culled;
            return;
        }
        // Submitted: "drawn" for the invisible-system throttle (+0xC9, B.4) [M: also while it has no live particles].
        world_->mark_drawn(index);
        bool any = false;
        for (const EmitterState& e : s.emitters) any = any || (e.live > 0 && e.base != UINT32_MAX);
        if (!any) return;
        const float cx = 0.5f * (box.min.x + box.max.x) - view.eye.x, cy = 0.5f * (box.min.y + box.max.y) - view.eye.y,
                    cz = 0.5f * (box.min.z + box.max.z) - view.eye.z;
        const float dist = cx * cx + cy * cy + cz * cz;
        Pending heat{2u, dist, {}, {}}, main{s.water ? 0u : 1u, dist, {}, {}};
        for (const EmitterState& e : s.emitters) {  // file order
            if (e.live == 0 || e.base == UINT32_MAX || !e.def->enabled) continue;
            EmitterGpu g{};
            g.info[0] = e.base;
            g.info[1] = e.pool;
            g.info[2] = desc_of(e.def);
            const bool sorted = sort_ok_ && e.def->sorted() && !e.def->heat;
            g.info[3] = sorted ? kESorted : 0u;
            g.link[0] = -1;
            g.link[1] = static_cast<int32_t>(e.base);
            rows(s.cur, g.prev);
            rows(s.cur, g.cur);
            g.vel_d[3] = std::min(s.fade, 1.0f);
            for (int c = 0; c < 4; ++c) g.color[c] = s.color[c];
            const auto entry = static_cast<uint32_t>(draw_table.size());
            draw_table.push_back(g);
            if (sorted) sort_entries.push_back(entry);
            DrawItem di;
            di.entry = entry;
            di.pool = e.pool;
            di.mode = e.def->heat ? 2u : draw_mode(e.def->blend_mode);  // heat: PrimAlpha (SPEC 0x3B)
            di.tex = texture(e.def->color_texture);
            (e.def->heat ? heat : main).items.push_back(di);
            ++stats_.emitters_drawn;
        }
        ++stats_.systems_drawn;
        if (!main.items.empty()) pend.push_back(std::move(main));
        if (!heat.items.empty()) pend.push_back(std::move(heat));
    });
    // Back-to-front per renderable; ties keep creation order (stable).
    std::stable_sort(pend.begin(), pend.end(), [](const Pending& a, const Pending& b) { return a.distance > b.distance; });
    for (Pending& p : pend) lists_[p.bucket].push_back(SystemDraw{p.distance, std::move(p.items)});
    // Packets reference descriptors too.
    for (const UpdatePacket& p : packets)
        for (const PacketEmitter& e : p.emitters) (void)desc_of(e.def);

    if (desc_list.empty() && packets.empty()) return;
    // ---- Upload the descriptor table.
    const rhi::UniformAlloc da = frame.alloc_storage(std::max<size_t>(desc_list.size(), 1) * sizeof(DescGpu));
    if (!da) {
        log::errorf("particles", "frame ring full (descriptors)");
        for (auto& l : lists_) l.clear();
        return;
    }
    for (size_t i = 0; i < desc_list.size(); ++i) {
        const DescGpu g = desc_gpu(*desc_list[i]);
        std::memcpy(static_cast<uint8_t*>(da.ptr) + i * sizeof(DescGpu), &g, sizeof g);
    }
    auto make_set = [&](const rhi::UniformAlloc& em, const rhi::UniformAlloc& sp, const rhi::UniformAlloc& gr, const rhi::UniformAlloc& cb) {
        const VkDescriptorSet set = frame.descriptors(0).allocate(set0_);
        if (!set) return set;
        VkDescriptorBufferInfo bi[8] = {{particles_.handle, 0, VK_WHOLE_SIZE}, {order_.handle, 0, VK_WHOLE_SIZE},
                                        {death_.handle, 0, VK_WHOLE_SIZE},     range_of(em),
                                        range_of(da),                          range_of(sp),
                                        range_of(gr),                          range_of(cb)};
        VkWriteDescriptorSet w[8];
        for (uint32_t i = 0; i < 8; ++i) {
            w[i] = rhi::vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
            w[i].dstSet = set;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = i == 7 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[i].pBufferInfo = &bi[i];
        }
        fn.vkUpdateDescriptorSets(ctx_->device(), 8, w, 0, nullptr);
        return set;
    };
    auto alloc_copy = [&](const void* data, size_t bytes) {
        rhi::UniformAlloc a = frame.alloc_storage(std::max<size_t>(bytes, 16));
        if (a && bytes) std::memcpy(a.ptr, data, bytes);
        return a;
    };
    auto alloc_fc = [&](const FrameConsts& c) {
        rhi::UniformAlloc a = frame.alloc_uniform(sizeof c);
        if (a) std::memcpy(a.ptr, &c, sizeof c);
        return a;
    };
    const VkPipelineStageFlags2 cs = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    const VkAccessFlags2 rw = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    auto barrier = [&](VkPipelineStageFlags2 src, VkAccessFlags2 sa, VkPipelineStageFlags2 dst, VkAccessFlags2 dacc) {
        const VkMemoryBarrier2 mb = rhi::memory_barrier(src, sa, dst, dacc);
        rhi::cmd_barrier(fn, cmd, {}, {}, std::span<const VkMemoryBarrier2>(&mb, 1));
    };
    // Previous frames' draws / dispatches (same queue, earlier in submission order) before this frame's writes.
    barrier(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | cs, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, cs, rw);
    auto bind = [&](VkPipeline p, VkDescriptorSet set) {
        fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout_, 0, 1, &set, 0, nullptr);
    };
    const uint32_t empty16[4] = {0, 0, 0, 0};
    // ---- Packets in order: clears, CSUpdate, CSSpawn.
    for (const UpdatePacket& p : packets) {
        std::vector<EmitterGpu> ems;
        ems.reserve(p.emitters.size());
        std::vector<uint32_t> groups;  // (entry, first) pairs or (first particle, count) pairs
        for (const ClearRange& c : p.clears)
            for (uint32_t o = 0; o < c.count; o += 64) {
                groups.push_back(c.base + o);
                groups.push_back(std::min<uint32_t>(64u, c.count - o));
            }
        const auto clear_groups = static_cast<uint32_t>(groups.size() / 2);
        for (size_t i = 0; i < p.emitters.size(); ++i) {
            const PacketEmitter& pe = p.emitters[i];
            EmitterGpu g{};
            g.info[0] = pe.base;
            g.info[1] = pe.pool;
            g.info[2] = desc_of(pe.def);
            g.info[3] = pe.parent_base >= 0 ? kEParent : 0u;
            g.link[0] = pe.parent_base;
            g.link[1] = static_cast<int32_t>(pe.base);
            rows(pe.prev, g.prev);
            rows(pe.cur, g.cur);
            g.vel_d[0] = pe.velocity.x;
            g.vel_d[1] = pe.velocity.y;
            g.vel_d[2] = pe.velocity.z;
            g.vel_d[3] = pe.d;  // the system's own step (throttled systems step with their gathered d)
            for (float& c : g.color) c = 1.0f;
            ems.push_back(g);
            for (uint32_t o = 0; o < pe.pool; o += 64) {
                groups.push_back(static_cast<uint32_t>(i));
                groups.push_back(o);
            }
        }
        const auto total_groups = static_cast<uint32_t>(groups.size() / 2);
        const rhi::UniformAlloc em = alloc_copy(ems.empty() ? static_cast<const void*>(empty16) : ems.data(),
                                                ems.empty() ? 16 : ems.size() * sizeof(EmitterGpu));
        const rhi::UniformAlloc sp = alloc_copy(p.spawns.empty() ? static_cast<const void*>(empty16) : p.spawns.data(),
                                                p.spawns.empty() ? 16 : p.spawns.size() * sizeof(SpawnRecord));
        if (clear_groups > 0) {
            const rhi::UniformAlloc gr = alloc_copy(groups.data(), clear_groups * 8u);
            FrameConsts c = fc;
            c.counts[1] = clear_groups;
            const rhi::UniformAlloc cb = alloc_fc(c);
            if (!em || !sp || !gr || !cb) break;
            const VkDescriptorSet set = make_set(em, sp, gr, cb);
            if (!set) break;
            bind(cs_clear_, set);
            fn.vkCmdDispatch(cmd, clear_groups, 1, 1);
            barrier(cs, rw, cs, rw);
        }
        if (!(p.d > 0.0f) || p.emitters.empty()) continue;
        ++stats_.updates;
        const uint32_t upd_groups = total_groups - clear_groups;
        const rhi::UniformAlloc gr = alloc_copy(groups.data() + clear_groups * 2u, std::max<size_t>(upd_groups, 1) * 8u);
        FrameConsts c = fc;
        c.wind[3] = p.d;
        c.counts[0] = static_cast<uint32_t>(p.spawns.size());
        c.counts[1] = upd_groups;
        const rhi::UniformAlloc cb = alloc_fc(c);
        if (!em || !sp || !gr || !cb) {
            log::errorf("particles", "frame ring full (packet)");
            break;
        }
        const VkDescriptorSet set = make_set(em, sp, gr, cb);
        if (!set) break;
        if (upd_groups > 0) {
            bind(cs_update_, set);
            fn.vkCmdDispatch(cmd, upd_groups, 1, 1);
            barrier(cs, rw, cs, rw);
            stats_.update_groups += upd_groups;
        }
        if (!p.spawns.empty()) {
            bind(cs_spawn_, set);
            fn.vkCmdDispatch(cmd, static_cast<uint32_t>((p.spawns.size() + 63) / 64), 1, 1);
            barrier(cs, rw, cs, rw);
            stats_.spawns += static_cast<uint32_t>(p.spawns.size());
        }
    }
    // ---- Draw table, per-particle sort.
    if (!draw_table.empty()) {
        const rhi::UniformAlloc em = alloc_copy(draw_table.data(), draw_table.size() * sizeof(EmitterGpu));
        const rhi::UniformAlloc sp = alloc_copy(empty16, 16), gr = alloc_copy(empty16, 16);
        const rhi::UniformAlloc cb = alloc_fc(fc);
        if (em && sp && gr && cb) draw_set_ = make_set(em, sp, gr, cb);
        if (!draw_set_) {
            log::errorf("particles", "frame ring / descriptor pool full (draw table)");
            for (auto& l : lists_) l.clear();
        } else if (!sort_entries.empty()) {
            bind(cs_sort_, draw_set_);
            for (uint32_t e : sort_entries) {
                const Push pc{e, 0, 0, 0};
                fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof pc, &pc);
                fn.vkCmdDispatch(cmd, 1, 1, 1);
            }
            stats_.sorted_emitters = static_cast<uint32_t>(sort_entries.size());
        }
    }
    // Compute writes before the draws' vertex shader reads (and the next frame's compute).
    barrier(cs, rw, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | cs, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    stats_.textures = static_cast<uint32_t>(textures_.size());
}

bool ParticleRenderer::has(PhaseBucket bucket) const {
    if (!draw_set_) return false;
    switch (bucket) {
        case PhaseBucket::Water: return !lists_[0].empty();
        case PhaseBucket::Transparent: return !lists_[1].empty();
        case PhaseBucket::Heat: return !lists_[2].empty();
        default: return false;
    }
}

uint32_t ParticleRenderer::draw_count(PhaseBucket bucket) const {
    return draw_set_ && bucket == PhaseBucket::Transparent ? static_cast<uint32_t>(lists_[1].size()) : 0u;
}

float ParticleRenderer::draw_distance(PhaseBucket bucket, uint32_t index) const {
    return bucket == PhaseBucket::Transparent && index < lists_[1].size() ? lists_[1][index].distance : 0.0f;
}

void ParticleRenderer::draw(const SceneHookPass& p) { draw_range(p, 0, UINT32_MAX); }

void ParticleRenderer::draw_range(const SceneHookPass& p, uint32_t begin, uint32_t end) {
    if (!ctx_ || !draw_set_ || !p.frame) return;
    const int li = p.bucket == PhaseBucket::Water ? 0 : p.bucket == PhaseBucket::Transparent ? 1 : p.bucket == PhaseBucket::Heat ? 2 : -1;
    if (li < 0 || lists_[li].empty()) return;
    end = std::min<uint32_t>(end, static_cast<uint32_t>(lists_[li].size()));
    if (begin >= end) return;
    const rhi::DeviceFns& fn = ctx_->vkd();
    const VkCommandBuffer cmd = p.cmd;
    // Viewport with negative height (camera.hpp: D3D clip-space Y up).
    VkViewport vp{0.0f, static_cast<float>(p.height), static_cast<float>(p.width), -static_cast<float>(p.height), 0.0f, 1.0f};
    VkRect2D sc{{0, 0}, {p.width, p.height}};
    fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    VkPipeline bound = VK_NULL_HANDLE;
    VkDescriptorSet bound_tex = VK_NULL_HANDLE;
    for (uint32_t si = begin; si < end; ++si) {
        const SystemDraw& s = lists_[li][si];
        for (const DrawItem& d : s.items) {
            const VkPipeline pl = pipeline(d.mode, p);
            if (!pl) continue;
            if (pl != bound) {
                fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
                fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 1, &draw_set_, 0, nullptr);
                bound = pl;
                bound_tex = VK_NULL_HANDLE;
            }
            const VkDescriptorSet ts = texture_set(*p.frame, d.tex);
            if (!ts) continue;
            if (ts != bound_tex) {
                fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 1, 1, &ts, 0, nullptr);
                bound_tex = ts;
            }
            const Push pc{d.entry, d.mode, fog_on_ && li != 2 ? 1u : 0u, 0};
            fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  sizeof pc, &pc);
            fn.vkCmdDraw(cmd, 6, d.pool, 0, 0);
            ++stats_.draw_calls;
            if (li == 0) ++stats_.water_draws;
            else if (li == 1) ++stats_.transparent_draws;
            else ++stats_.heat_draws;
        }
    }
}

}  // namespace alamo::render::particles

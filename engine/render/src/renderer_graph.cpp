// Renderer: frame graph, post passes and primitives (split from renderer.cpp).
#include "alamo/render/renderer.hpp"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <unordered_map>
#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/materials/constants.hpp"
#include "alamo/materials/params.hpp"
#include "alamo/render/convert.hpp"
#include "alamo/render/ssao.hpp"
#include "alamo/render/world.hpp"
#include "render_internal.hpp"
#include "fx2_reader.hpp"
#include "gamma.hpp"  // [port] output gamma
#include "fsr.hpp"  // [port] FSR upscaling
#include "xess.hpp"  // [port] XeSS upscaling
#include "smaa.hpp"  // [port] SMAA
#include "taa.hpp"  // [port] TAA
#include "renderer_shared.hpp"

namespace alamo::render {
using namespace detail;

// ------------------------------------------------------------------------------------ frame graph

VkExtent2D render_scale_extent(VkExtent2D out, float scale) {
    if (!(scale < 1.0f)) return out;  // also NaN
    const double k = std::sqrt(static_cast<double>(std::max(scale, kRenderScaleMin)));
    auto side = [k](uint32_t s) {
        const uint32_t v = static_cast<uint32_t>(std::llround(static_cast<double>(s) * k));
        return std::clamp(v, 1u, std::max(s, 1u));
    };
    return {side(out.width), side(out.height)};
}

bool Renderer::ensure_targets(VkExtent2D ext) {
    if (tg_.ready && tg_.scene.extent().width == ext.width && tg_.scene.extent().height == ext.height) return true;
    if (tg_.ready) ctx_->wait_idle();  // old images may still be in use by frames in flight
    const rhi::Format color = rhi::from_vk(desc_.color_format);
    auto make = [&](rhi::RenderTarget& t, rhi::Format depth, float scale, const char* name) {
        rhi::RenderTargetDesc d;
        d.color = color;
        d.depth = depth;
        d.scale = scale;
        d.name = name;
        // [port] Scene depth is sampled after the opaque buckets (SSAO) and by TAA; usage only, no behaviour change.
        d.sample_depth = (&t == &tg_.scene);
        if (tg_.ready) {
            t.resize(ext);  // returns whether it recreated; a failed allocation leaves the images missing
            return t.has_color() && (depth == rhi::Format::Undefined || t.has_depth());
        }
        return t.init(*alloc_, d, ext);
    };
    const rhi::Format ds = ds_format_ != VK_FORMAT_UNDEFINED ? rhi::from_vk(ds_format_) : rhi::from_vk(desc_.depth_format);
    // Back-buffer equivalent + depth-stencil, SceneTexture, DistortionTexture (1x), bloom A/B (1/4x),
    // all A8R8G8B8-equivalent UNORM (TERRAIN_WATER_POST.md 4.1).
    if (!make(tg_.scene, ds, 1.0f, "render.scene") || !make(tg_.scene_tex, rhi::Format::Undefined, 1.0f, "SceneTexture") ||
        !make(tg_.distortion, rhi::Format::Undefined, 1.0f, "DistortionTexture") ||
        !make(tg_.bloom_a, rhi::Format::Undefined, 0.25f, "bloom A") ||
        !make(tg_.bloom_b, rhi::Format::Undefined, 0.25f, "bloom B")) {
        log::errorf("render", "frame graph targets (%ux%u) could not be created", ext.width, ext.height);
        tg_.scene.shutdown();
        tg_.scene_tex.shutdown();
        tg_.distortion.shutdown();
        tg_.bloom_a.shutdown();
        tg_.bloom_b.shutdown();
        tg_.ready = false;
        return false;
    }
    tg_.ready = true;
    return true;
}

const materials::MaterialEffect* Renderer::post_effect(int which) {
    static const char* const kNames[5] = {"StencilDarken", "StencilDarkenToAlpha", "StencilDarkenFinalBlur", "SceneBloom",
                                          "SceneHeat"};
    // The original's technique selection rejects techniques whose VENDOR_FILTER_n equals the adapter vendor: on AMD
    // SceneHeat draws t1_ati (V half-texel -1/4), elsewhere t0 (-1/2) (EffectCache::get_for_vendor, g1f t06 capture).
    if (!fx_post_[which] && desc_.effects)
        fx_post_[which] = desc_.effects->get_for_vendor(kNames[which], ctx_ ? ctx_->caps().vendor_id : 0u);
    return fx_post_[which].get();
}

namespace {
// D3DXMatrixOrthoRH(2, 2, 0.1, 2) of the 2D prim camera (column-vector rows).
ViewMatrices prim_camera(uint32_t width, uint32_t height) {
    ViewMatrices v;
    v.width = width;
    v.height = height;
    v.proj = Mat4::identity();
    v.proj.m[2][2] = 1.0f / (0.1f - 2.0f);
    v.proj.m[2][3] = 0.1f / (0.1f - 2.0f);
    v.view_proj = v.proj;
    return v;
}
}  // namespace

// One translated draw outside the mesh path: full-screen post passes (SceneBloom / SceneHeat, the FLOAT2
// quad, identity matrices), prim passes (StencilDarkenFinalBlur under the 2D camera) and geometry draws
// (terrain / water patches with their own camera, buffers and identity world). Register files from the
// effect + engine semantics (RESOLUTION_CONSTANTS = bound target) + PostSpec values, combined samplers
// bound by texture parameter name or semantic, the D3D9 half-pixel push offset. Called inside rendering
// with the target's Y-flipped viewport. False (nothing drawn) while the program / pipeline is compiling.
bool Renderer::translated_state(const PostSpec& s, XConstCache* cache, GeoState& out) {
    if (!s.fx || !s.pass || !s.fx->effect || !s.pass->vs_plan || !s.pass->ps_plan) return false;
    const bool geometry = s.vb != VK_NULL_HANDLE;
    XConstCache local;
    XConstCache& cc = cache ? *cache : local;
    const uint32_t aniso = aniso_cap();
    // [port] The anisotropy setting applies to geometry draws (terrain, water, decorations) only, never to
    // full-screen post or 2D prim passes.
    const uint32_t aniso_level = geometry ? aniso_ : 0u;
    // Program, pipeline and samplers once per (pass, generation): every key input is the same for the draws
    // of one generation (same pass, render pass, layout, blend / cull overrides) and anisotropy setting.
    const bool state_cached = cache && cc.pass == s.pass && cc.state_generation == cc.generation && cc.pipe &&
                              cc.sampler_aniso == aniso_level;
    const materials::TranslatedProgram* tp = state_cached ? cc.tp : translated_program(s.fx, *s.pass);
    if (!tp) return false;
    VkPipeline pipe = state_cached ? cc.pipe : VK_NULL_HANDLE;
    if (!state_cached) {
        materials::PsoKey key = tp->key(s.pass->pso[0]);
        key.render_pass = s.render_pass;
        key.vertex_layout = geometry ? s.layout : (s.prim ? materials::VertexLayout::PrimVert : kFullScreenLayout);
        if (s.alpha_blend) {
            key.blend_enable = 1;
            key.src_color = materials::BlendFactor::SrcAlpha;
            key.dst_color = materials::BlendFactor::OneMinusSrcAlpha;
            key.color_op = materials::BlendOp::Add;
            key.src_alpha = materials::BlendFactor::SrcAlpha;
            key.dst_alpha = materials::BlendFactor::OneMinusSrcAlpha;
            key.alpha_op = materials::BlendOp::Add;
        }
        if (s.mirrored) key.cull = mirrored_cull(key.cull);
        if (s.cull_none) key.cull = materials::CullMode::None;
        if (s.no_depth_write) key.depth_write = 0;
        pipe = psos_.get(key);
        if (!pipe) return false;
        for (const shaderxlat::SamplerDesc& sd : tp->ps.iface.samplers) {
            if (sd.slot >= rhi::kTextureSlots || sd.slot >= 16) continue;
            const materials::SamplerBinding* sb = nullptr;
            for (const auto& x : s.pass->samplers)
                if (x.slot == sd.slot) sb = &x;
            const materials::SamplerKey key_s = s.override_sampler ? s.sampler_override : (sb ? sb->key : materials::SamplerKey{});
            cc.samplers[sd.slot] = samplers_.get(to_rhi_sampler(key_s, aniso, aniso_level));
        }
        if (cache) {
            cc.tp = tp;
            cc.pipe = pipe;
            cc.state_generation = cc.generation;
            cc.sampler_aniso = aniso_level;
        }
    }
    out.tp = tp;
    out.pipe = pipe;
    out.layout = translated_layout(*tp);
    out.int_bool = tp->vs_int_bool || tp->ps_int_bool;
    std::copy(std::begin(cc.samplers), std::end(cc.samplers), std::begin(out.samplers));
    shaderxlat::TranslatedPush push = s.pass->translated_push;
    translated_pixel_offset(s.width, s.height, push.pixel_offset);
    if (s.fog && lighting_.fog_enabled) {
        push.flags = (push.flags & ~(3u << shaderxlat::kFogTableShift)) | shaderxlat::kPushFog;
        push.fog_color[0] = lighting_.fog_color.x;
        push.fog_color[1] = lighting_.fog_color.y;
        push.fog_color[2] = lighting_.fog_color.z;
    }
    out.push = push;
    return true;
}

void Renderer::translated_fill(const PostSpec& s, XConstCache& cc, bool cached, bool ib) {
    const bool geometry = s.vb != VK_NULL_HANDLE;
    const fx::Effect& fxe = *s.fx->effect;
    const bool preshader = s.pass->vs_plan->preshader || s.pass->ps_plan->preshader;
    const bool reuse = cached && !preshader && cc.pass == s.pass && cc.filled_generation == cc.generation;
    if (!reuse) {
        // Constants: identity world/view/projection (the full-screen VS passes positions through), the 2D prim
        // camera, or the geometry camera.
        ViewMatrices v = geometry && s.view ? *s.view : (s.prim ? prim_camera(s.width, s.height) : ViewMatrices{});
        v.width = s.width;
        v.height = s.height;
        if (!(geometry && s.view)) v.eye = {0, 0, 0};
        EngineFrame f;
        engine_frame(v, scene_lights(), f);
        EngineParams e;
        const Instance none;
        make_engine_draw(Mat3x4{}, none, v, f, e);
        // Material values by name (ParamStore semantics: case-insensitive, the last set wins, patch values over values)
        // straight from the spec's views: no store is built (Q3: no allocation per post draw).
        const materials::ParamLookup lookup = [&e, &s](const fx::Parameter& p, std::span<float> out) {
            const Src src = param_src(p);
            if (src != Src::Store) return engine_write(e, nullptr, p, out, src);
            const PostValue* hit = nullptr;
            for (size_t k = s.patch_values.size(); k-- > 0 && !hit;)
                if (alamo::iequals(s.patch_values[k].name, p.type.name)) hit = &s.patch_values[k];
            for (size_t k = s.values.size(); k-- > 0 && !hit;)
                if (alamo::iequals(s.values[k].name, p.type.name)) hit = &s.values[k];
            if (!hit) return false;
            std::fill(out.begin(), out.end(), 0.0f);
            std::copy_n(hit->v.begin(), std::min(hit->v.size(), out.size()), out.begin());
            return true;
        };
        cc.vs = {};
        cc.ps = {};
        materials::fill_register_file(fxe, *s.pass->vs_plan, lookup, cc.vs.c);
        materials::fill_register_file(fxe, *s.pass->ps_plan, lookup, cc.ps.c);
        materials::fill_bump_env(*s.pass, fxe, lookup, cc.ps.c);
        if (ib) {
            materials::fill_int_bool_constants(fxe, *s.pass->vs_plan, lookup, cc.vib);
            materials::fill_int_bool_constants(fxe, *s.pass->ps_plan, lookup, cc.pib);
        }
        cc.pass = s.pass;
        cc.filled_generation = cc.generation;
        return;
    }
    // Same pass and generation: only the per-draw values change, written at their CTAB registers.
    auto patch = [&](const materials::RegisterFilePlan& plan, std::span<materials::float4> regs) {
        for (const PostValue& pv : s.patch_values)
            for (const materials::RegisterBinding& rb : plan.floats) {
                const std::span<const float> val = pv.v;
                if (rb.name != pv.name || rb.count == 0) continue;
                for (uint16_t r = 0; r < rb.count && rb.reg + r < regs.size(); ++r) {
                    float f[4] = {0, 0, 0, 0};
                    for (size_t k = 0; k < 4 && r * 4u + k < val.size(); ++k) f[k] = val[r * 4u + k];
                    regs[rb.reg + r] = {f[0], f[1], f[2], f[3]};
                }
            }
    };
    patch(*s.pass->vs_plan, cc.vs.c);
    patch(*s.pass->ps_plan, cc.ps.c);
}

uint32_t Renderer::translated_textures(const PostSpec& s, const GeoState& g, rhi::TextureBinding* tb) const {
    uint32_t ntb = 0;
    for (const shaderxlat::SamplerDesc& sd : g.tp->ps.iface.samplers) {
        if (sd.slot >= rhi::kTextureSlots || sd.slot >= 16) continue;
        const materials::SamplerBinding* sb = nullptr;
        for (const auto& x : s.pass->samplers)
            if (x.slot == sd.slot) sb = &x;
        VkImageView view = VK_NULL_HANDLE;
        if (sb)
            for (const PostTexture& pt : s.textures)
                if ((!sb->texture_param.empty() && alamo::iequals(sb->texture_param, pt.name)) ||
                    (!sb->texture_semantic.empty() && alamo::iequals(sb->texture_semantic, pt.name)))
                    view = pt.view;
        if (!view && sb && fow_view_ && is_fow_binding(*sb)) view = fow_view_;  // engine FOW_TEXTURE
        if (!view) {
            if (sd.dim == shaderxlat::TextureDim::Cube) view = white_cube_.view;
            else if (sd.dim == shaderxlat::TextureDim::Volume) view = white_3d_.view;
            else view = s.slot_default ? s.slot_default : white_.view;
        }
        tb[ntb++] = {sd.slot, view, g.samplers[sd.slot], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    return ntb;
}

void Renderer::bind_geometry_state(VkCommandBuffer cmd, const PostSpec& s, const GeoState& g, VkBuffer vb, VkBuffer ib) {
    const rhi::DeviceFns& fn = ctx_->vkd();
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe);
    cmd_set_dynamic_state(fn, cmd, s.pass->dynamic, rhi::from_vk(ds_format_));
    const VkBuffer vbs[2] = {vb, zero_vb_.handle};
    const VkDeviceSize offs[2] = {0, 0};
    fn.vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
    fn.vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof g.push, &g.push);
    fn.vkCmdBindIndexBuffer(cmd, ib, 0, s.index_type);
    if (bind_stats_) {
        ++bind_stats_->pipeline_binds;
        ++bind_stats_->dynamic_state;
        bind_stats_->buffer_binds += 2;
        ++bind_stats_->push_constants;
    }
}

// Q3: the entry of an uncached post_translated() call. Digest = every input of translated_state / translated_fill: the
// spec's pass, flags, target size, values, the engine frame of its camera and the scene fog; equal digests give equal
// images, so a hit skips the fill. 32 entries, least recently used replaced (allocates only up to the high water).
Renderer::XConstCache& Renderer::post_cache_entry(const PostSpec& s, bool& hit) {
    const bool geometry = s.vb != VK_NULL_HANDLE;
    ViewMatrices v = geometry && s.view ? *s.view : (s.prim ? prim_camera(s.width, s.height) : ViewMatrices{});
    v.width = s.width;
    v.height = s.height;
    if (!(geometry && s.view)) v.eye = {0, 0, 0};
    EngineFrame f;
    engine_frame(v, scene_lights(), f);
    uint64_t h = alamo::fnv1a64(&f, sizeof f);
    const uintptr_t ptrs[2] = {reinterpret_cast<uintptr_t>(s.pass), reinterpret_cast<uintptr_t>(s.fx.get())};
    h = alamo::fnv1a64(ptrs, sizeof ptrs, h);
    const uint32_t scal[4] = {s.render_pass, s.width, s.height, static_cast<uint32_t>(s.layout)};
    h = alamo::fnv1a64(scal, sizeof scal, h);
    const uint8_t flags[8] = {s.prim, s.alpha_blend, s.mirrored, s.fog, s.cull_none, s.no_depth_write, s.override_sampler,
                              static_cast<uint8_t>(lighting_.fog_enabled)};
    h = alamo::fnv1a64(flags, sizeof flags, h);
    if (s.override_sampler) h = alamo::fnv1a64(&s.sampler_override, sizeof s.sampler_override, h);
    const float fog[3] = {lighting_.fog_color.x, lighting_.fog_color.y, lighting_.fog_color.z};
    h = alamo::fnv1a64(fog, sizeof fog, h);
    for (const std::span<const PostValue> list : {s.values, s.patch_values}) {
        const uint32_t n = static_cast<uint32_t>(list.size());
        h = alamo::fnv1a64(&n, sizeof n, h);
        for (const PostValue& pv : list) {
            h = alamo::fnv1a64(pv.name.data(), pv.name.size(), h);
            const uint32_t m = static_cast<uint32_t>(pv.v.size());
            h = alamo::fnv1a64(&m, sizeof m, h);
            if (m) h = alamo::fnv1a64(pv.v.data(), pv.v.size() * sizeof(float), h);
        }
    }
    ++post_cache_clock_;
    PostCacheEntry* victim = nullptr;
    for (const std::unique_ptr<PostCacheEntry>& e : post_cache_) {
        if (e->digest == h && e->cc.pass == s.pass) {
            e->last_use = post_cache_clock_;
            hit = e->valid;
            return e->cc;
        }
        if (!victim || e->last_use < victim->last_use) victim = e.get();
    }
    if (post_cache_.size() < 32) {
        post_cache_.push_back(std::make_unique<PostCacheEntry>());
        victim = post_cache_.back().get();
    }
    *victim = PostCacheEntry{};
    victim->digest = h;
    victim->last_use = post_cache_clock_;
    victim->cc.pass = s.pass;  // translated_fill sets it again; the lookup above keys on it
    hit = false;
    return victim->cc;
}

bool Renderer::post_translated(rhi::Frame& frame, VkCommandBuffer cmd, const PostSpec& s, XConstCache* cache) {
    bool hit = false;
    XConstCache* use = cache ? cache : &post_cache_entry(s, hit);
    GeoState g;
    if (!translated_state(s, use, g)) return false;
    const bool geometry = s.vb != VK_NULL_HANDLE;
    XConstCache& cc = *use;
    const bool ib = g.int_bool;
    if (cache) {
        translated_fill(s, cc, true, ib);
    } else if (!hit) {
        translated_fill(s, cc, false, ib);
        for (const std::unique_ptr<PostCacheEntry>& e : post_cache_)
            if (&e->cc == use) e->valid = true;
    }
    const rhi::UniformAlloc va = frame.alloc_uniform(sizeof cc.vs), pa = frame.alloc_uniform(sizeof cc.ps);
    const rhi::UniformAlloc via = ib ? frame.alloc_uniform(sizeof cc.vib) : rhi::UniformAlloc{};
    const rhi::UniformAlloc pia = ib ? frame.alloc_uniform(sizeof cc.pib) : rhi::UniformAlloc{};
    VkDescriptorSet s0 = frame.descriptors(0).allocate(std_layouts_.constants(ib));
    VkDescriptorSet s1 = frame.descriptors(0).allocate(std_layouts_.textures());
    if (!va || !pa || (ib && (!via || !pia)) || !s0 || !s1) return false;
    std::memcpy(va.ptr, &cc.vs, sizeof cc.vs);
    std::memcpy(pa.ptr, &cc.ps, sizeof cc.ps);
    rhi::write_constants(*ctx_, s0, va.buffer, va.offset, sizeof cc.vs, pa.buffer, pa.offset, sizeof cc.ps);
    if (ib) {
        std::memcpy(via.ptr, &cc.vib, sizeof cc.vib);
        std::memcpy(pia.ptr, &cc.pib, sizeof cc.pib);
        rhi::write_int_bool_constants(*ctx_, s0, via.buffer, via.offset, pia.buffer, pia.offset);
    }
    rhi::TextureBinding tb[rhi::kTextureSlots];
    const uint32_t ntb = translated_textures(s, g, tb);
    if (ntb) rhi::write_textures(*ctx_, s1, std::span<const rhi::TextureBinding>(tb, ntb));
    const rhi::DeviceFns& fn = ctx_->vkd();
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipe);
    const VkDescriptorSet xs[2] = {s0, s1};
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 2, xs, 0, nullptr);
    cmd_set_dynamic_state(fn, cmd, s.pass->dynamic, rhi::from_vk(ds_format_));
    VkBuffer vb0 = s.prim ? prim_vb_.handle : quad_vb_.handle;
    VkDeviceSize off0 = s.prim ? static_cast<VkDeviceSize>(frame.slot() % 4) * 2 * 6 * 24 : 0;
    if (geometry) {
        vb0 = s.vb;
        off0 = s.vb_offset;
    }
    const VkBuffer vbs[2] = {vb0, zero_vb_.handle};
    const VkDeviceSize offs[2] = {off0, 0};
    fn.vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
    fn.vkCmdPushConstants(cmd, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof g.push, &g.push);
    if (geometry) {
        fn.vkCmdBindIndexBuffer(cmd, s.ib, s.ib_offset, s.index_type);
        fn.vkCmdDrawIndexed(cmd, s.index_count, 1, 0, 0, 0);
    } else {
        fn.vkCmdDraw(cmd, 6, 1, 0, 0);
    }
    if (bind_stats_) {
        ++bind_stats_->draws;
        ++bind_stats_->pipeline_binds;
        ++bind_stats_->set_binds;
        ++bind_stats_->dynamic_state;
        bind_stats_->buffer_binds += geometry ? 2u : 1u;
        ++bind_stats_->push_constants;
        bind_stats_->set_allocs += 2;
        bind_stats_->set_writes += 1u + (ib ? 1u : 0u) + (ntb ? 1u : 0u);
        bind_stats_->uniform_allocs += ib ? 4u : 2u;
    }
    return true;
}
// StencilDarken / StencilDarkenToAlpha (fixed function): 6 pre-lit prim vertices with the shadow colour
// (alpha 0) under the orthographic 2D camera (identity view, -1..1, near 0.1, far 2), TERRAIN_WATER_POST
// 4.2 / 4.3. The FF ubershader reads its own FrameCB / DrawData / FfDrawData set.
bool Renderer::post_darken(rhi::Frame& frame, VkCommandBuffer cmd, const materials::MaterialEffect& fx, uint32_t width,
                           uint32_t height, bool reset_alpha) {
    const materials::ResolvedPass* pass = nullptr;
    for (const auto& p : fx.passes)
        if (!p.restore_only && !pass) pass = &p;
    if (!pass || !pass->fixed_function || !pass->ff || !fx.effect || !pass->program.vs) return false;
    materials::PsoKey key = pass->pso[0];
    key.render_pass = rp_main_;  // [port] MSAA: the main scene target
    key.vertex_layout = materials::VertexLayout::PrimVert;
    // Alpha reset [port]: the StencilDarkenToAlpha pass (colour write A only) without its stencil test, vertex alpha 1.
    if (reset_alpha) {
        key.stencil_enable = 0;
        key.color_write_mask = 0x8;
        key.blend_enable = 0;
    }
    const VkPipeline pipe = psos_.get(key);
    if (!pipe) return false;
    // Prim vertices: BGRA8 colour = shadow colour, alpha 0 (reset: 1); uv (0,1) at (-1,-1) .. (1,0) at (1,1); z in front.
    // Float -> byte by truncation: Kuat's shadow colour 0.549 reaches the darken / resolve quad as 0x8B (g1f t06 capture,
    // vertex blob at call 66927756), where rounding would give 0x8C.
    auto b8 = [](float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    const uint8_t col[4] = {b8(lighting_.shadow_color.z), b8(lighting_.shadow_color.y), b8(lighting_.shadow_color.x),
                            static_cast<uint8_t>(reset_alpha ? 255 : 0)};
    const float pos[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, -1}, {1, 1}, {-1, 1}};
    const size_t block = static_cast<size_t>(frame.slot() % 4) * 2 + (reset_alpha ? 1u : 0u);
    uint8_t* dst = static_cast<uint8_t*>(prim_vb_.mapped()) + block * 6 * 24;
    for (int i = 0; i < 6; ++i) {
        const float p[3] = {pos[i][0], pos[i][1], -1.0f};
        const float uv[2] = {0.5f * (pos[i][0] + 1.0f), 0.5f * (1.0f - pos[i][1])};
        std::memcpy(dst + i * 24, p, 12);
        std::memcpy(dst + i * 24 + 12, col, 4);
        std::memcpy(dst + i * 24 + 16, uv, 8);
    }
    // Frame constants of the 2D camera.
    const ViewMatrices v = prim_camera(width, height);
    materials::FrameCB fcb;
    frame_constants(v, 0u, fcb);  // camera, lights, SH (no fog / fade / world matrices in the 2D pass)
    materials::DrawData dd{};
    store_rows(Mat3x4{}, dd.world);
    dd.light_scale = {1, 1, 1, 1};
    // One empty store for every call: MSVC's std::map allocates its head node on construction, which made each darken
    // draw allocate twice (Q3 steady-state allocation check, first hit once space frames drew stencil shadows).
    static const materials::ParamStore store;
    materials::FfDrawData ffd = materials::build_ff_draw_data(*pass->ff, *fx.effect, store.lookup(), true);
    // Neither StencilDarken FXO sets texture stage states, so the prim path's stage 0 state applies:
    // colour and alpha = TEXTURE x DIFFUSE with stage 0 unbound (white). The D3D9 default ALPHAOP
    // SELECTARG1(TEXTURE) would leave StencilDarkenToAlpha writing alpha 1, i.e. no soft shadow at all.
    ffd.stage[0].color_op = 4;    // D3DTOP_MODULATE
    ffd.stage[0].color_arg1 = 2;  // D3DTA_TEXTURE
    ffd.stage[0].color_arg2 = 0;  // D3DTA_DIFFUSE
    ffd.stage[0].alpha_op = 4;
    ffd.stage[0].alpha_arg1 = 2;
    ffd.stage[0].alpha_arg2 = 0;
    if (ffd.misc.x < 1) ffd.misc.x = 1;
    const materials::MeshMaterial mm{};
    const materials::BoneMatrix bm{};
    rhi::BufferRange fcb_r;
    if (!upload_frame_cb(frame, fcb, fcb_r)) return false;
    const rhi::UniformAlloc da = frame.alloc_storage(sizeof dd), ma = frame.alloc_storage(sizeof mm),
                            ba = frame.alloc_storage(sizeof bm), xa = frame.alloc_storage(sizeof ffd);
    VkDescriptorSet s0 = frame.descriptors(0).allocate(hot_.buffers());
    if (!da || !ma || !ba || !xa || !s0) return false;
    std::memcpy(da.ptr, &dd, sizeof dd);
    std::memcpy(ma.ptr, &mm, sizeof mm);
    std::memcpy(ba.ptr, &bm, sizeof bm);
    std::memcpy(xa.ptr, &ffd, sizeof ffd);
    const rhi::BufferRange sto[rhi::kHotStorageBuffers] = {
        {da.buffer, da.offset, sizeof dd}, {ma.buffer, ma.offset, sizeof mm}, {ba.buffer, ba.offset, sizeof bm},
        {xa.buffer, xa.offset, sizeof ffd}};
    rhi::write_hot_buffers(*ctx_, s0, fcb_r, sto);
    const rhi::DeviceFns& fn = ctx_->vkd();
    fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    const VkDescriptorSet sets[2] = {s0, bindless_};
    fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
    cmd_set_dynamic_state(fn, cmd, pass->dynamic, rhi::from_vk(ds_format_));
    const VkDeviceSize off = static_cast<VkDeviceSize>(block) * 6 * 24;
    fn.vkCmdBindVertexBuffers(cmd, 0, 1, &prim_vb_.handle, &off);
    materials::PushConstants pc = pass->push;
    pc.draw_index = 0;
    fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    fn.vkCmdDraw(cmd, 6, 1, 0, 0);
    return true;
}

namespace detail {
void begin_target(const rhi::DeviceFns& fn, VkCommandBuffer cmd, rhi::RenderTarget& t, bool depth, const float* clear_color,
                  bool clear_depth, bool secondaries) {
    t.transition_color(fn, cmd, rhi::ImageState::ColorAttachment);
    if (depth) t.transition_depth(fn, cmd, rhi::ImageState::DepthAttachment);
    auto ca = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    ca.imageView = t.color().attachment_view ? t.color().attachment_view : t.color().view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (clear_color) ca.clearValue.color = {{clear_color[0], clear_color[1], clear_color[2], clear_color[3]}};
    auto da = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    da.imageView = depth ? (t.depth().attachment_view ? t.depth().attachment_view : t.depth().view) : VK_NULL_HANDLE;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = clear_depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
    da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil = {1.0f, 0};
    const bool stencil = depth && (t.depth().aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    auto ri = rhi::vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, t.extent()};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = depth ? &da : nullptr;
    ri.pStencilAttachment = stencil ? &da : nullptr;
    if (secondaries) ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    fn.vkCmdBeginRendering(cmd, &ri);
    if (secondaries) return;
    VkViewport vp{};
    vp.y = static_cast<float>(t.extent().height);
    vp.width = static_cast<float>(t.extent().width);
    vp.height = -static_cast<float>(t.extent().height);
    vp.maxDepth = 1.0f;
    fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{{0, 0}, t.extent()};
    fn.vkCmdSetScissor(cmd, 0, 1, &sc);
}
// [port] MSAA: an empty dynamic-rendering instance over the multisampled `ms` whose end resolves its colour (average) into
// `color_dst` and / or its depth (sample 0; the stencil aspect is not attached) into `depth_dst`. Both are left as
// attachments; a barrier after the pass orders the depth resolve (colour-attachment-output stage) before later depth use.
void resolve_into(const rhi::DeviceFns& fn, VkCommandBuffer cmd, rhi::RenderTarget& ms, rhi::RenderTarget* color_dst,
                  rhi::RenderTarget* depth_dst) {
    if (color_dst) {
        ms.transition_color(fn, cmd, rhi::ImageState::ColorAttachment);
        color_dst->transition_color(fn, cmd, rhi::ImageState::ColorAttachment);
    }
    if (depth_dst) {
        ms.transition_depth(fn, cmd, rhi::ImageState::DepthAttachment);
        // The depth resolve writes in the colour-attachment-output stage (not the fragment tests): the barrier into the
        // attachment layout covers that stage too, on both sides (an earlier resolve into the same image).
        const rhi::Image& d = depth_dst->depth();
        VkImageMemoryBarrier2 b = rhi::transition(d.handle, depth_dst->depth_state(), rhi::ImageState::DepthAttachment, d.range());
        b.srcStageMask |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.srcAccessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstStageMask |= VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        b.dstAccessMask |= VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        rhi::cmd_barrier(fn, cmd, b);
        depth_dst->set_depth_state(rhi::ImageState::DepthAttachment);
    }
    auto view_of = [](const rhi::Image& i) { return i.attachment_view ? i.attachment_view : i.view; };
    auto ca = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    if (color_dst) {
        ca.imageView = view_of(ms.color());
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
        ca.resolveImageView = view_of(color_dst->color());
        ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    auto da = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    if (depth_dst) {
        da.imageView = view_of(ms.depth());
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.resolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        da.resolveImageView = view_of(depth_dst->depth());
        da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    auto ri = rhi::vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, ms.extent()};
    ri.layerCount = 1;
    ri.colorAttachmentCount = color_dst ? 1u : 0u;
    ri.pColorAttachments = color_dst ? &ca : nullptr;
    ri.pDepthAttachment = depth_dst ? &da : nullptr;
    fn.vkCmdBeginRendering(cmd, &ri);
    fn.vkCmdEndRendering(cmd);
    if (depth_dst) {
        const rhi::StateInfo to = rhi::state_info(rhi::ImageState::DepthAttachment);
        const rhi::Image& d = depth_dst->depth();
        rhi::cmd_barrier(fn, cmd,
                         rhi::image_barrier(d.handle, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, to.layout,
                                            VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                                            to.stages, to.access, d.range()));
    }
}
rhi::CopyImage copy_of(const rhi::RenderTarget& t, rhi::ImageState after) {
    rhi::CopyImage c;
    c.image = t.color().handle;
    c.extent = t.extent();
    c.format = t.color().vk_format();
    c.before = t.color_state();
    c.after = after;
    return c;
}
}  // namespace detail
using detail::begin_target;
using detail::copy_of;
using detail::resolve_into;

bool Renderer::build_sky(rhi::Frame& frame, const ViewMatrices& view, std::span<const Instance> sky, DrawMode mode,
                         const RecordOptions& opts, Built& out, std::vector<Instance>& storage, DrawStats& st) {
    // Background objects: the main camera with near 10 / far 100000 (SPACE_AND_OPEN.md D.7 2: same position,
    // rotation and FOV; no re-centring). Land skydomes are camera-relative (translation = eye).
    const ViewMatrices sv = background_view(view);
    storage.clear();
    for (const Instance& s : sky) {
        Instance si = s;
        if (!opts.space) {  // land skydomes follow the camera; space backgrounds stay where they are (A.3)
            Mat3x4 tr;
            tr.m[0][3] = view.eye.x;
            tr.m[1][3] = view.eye.y;
            tr.m[2][3] = view.eye.z;
            si.transform = tr * s.transform;
        }
        storage.push_back(si);
    }
    RecordOptions so = opts;
    so.frustum_cull = opts.space;  // space: real objects at their positions (planets, nebulae); land: domes
    so.threads = 1;
    sky_build_ = true;
    if (!opts.space) {
        const bool ok = build(frame, sv, storage, mode, so, out, st, false);
        sky_build_ = false;
        return ok;
    }
    // Space background scene (SPACE_AND_OPEN.md A.4 item 3, A.4 "background scene properties"): the environment's
    // ambient + sun, fill lights 1 and 2 at intensity 0, fog off, distance fade off.
    const Lighting saved = lighting_;
    const float fb = fade_begin_, fe = fade_end_;
    lighting_.fill[0].intensity = lighting_.fill[1].intensity = 0.0f;
    lighting_.fog_enabled = false;
    fade_begin_ = fade_end_ = -1.0f;
    const bool ok = build(frame, sv, storage, mode, so, out, st, false);
    sky_build_ = false;
    lighting_ = saved;
    fade_begin_ = fb;
    fade_end_ = fe;
    return ok;
}

bool Renderer::render(rhi::Frame& frame, VkCommandBuffer primary, const PassTarget& target, const ViewMatrices& view_in,
                      std::span<const Instance> instances, DrawMode mode, const RecordOptions& opts, DrawStats* stats,
                      DrawTiming* timing, std::span<const Instance> sky) {
    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    if (ring_) collect_released();  // after begin_frame: retire released bindless slots + their images
    latch_anisotropy(frame);
    // [port: Gabriel] Render scale: the scene targets take the scaled size; the view's pixel size follows them (lasers,
    // the D3D9 half-pixel offset, TAA jitter), its projection (aspect) stays the output's.
    // [port: Gabriel 2026-11-13] FSR (fsr.cpp) / XeSS (xess.cpp): the scene and its post chain at the quality mode's
    // render size instead (the render scale is then not used), upscaled to the output at the end.
    VkExtent2D scene_ext = render_scale_extent(target.extent, opts.render_scale);
    VkExtent2D fsr_ext = target.extent;
    const bool fsr_on = fsr_begin(target.extent, opts, fsr_ext);  // FSR or XeSS frame
    const bool xess_on = fsr_on && opts.upscaler == Upscaler::Xess;
    if (fsr_on) scene_ext = fsr_ext;
    if (!ensure_targets(scene_ext)) return false;
    // [port] MSAA (RecordOptions::msaa): the main scene's sample count, render pass class and multisampled target. Every
    // build of this frame for the main scene uses rp_main_; the world prepass switches to the single-sample rp_scene_.
    set_msaa(msaa_samples(opts.msaa));  // a failure falls back to one sample (logged)
    if (msaa_ > 1 && (!tg_.scene_ms.has_color() || tg_.scene_ms.extent().width != scene_ext.width ||
                      tg_.scene_ms.extent().height != scene_ext.height)) {
        if (tg_.scene_ms.has_color()) ctx_->wait_idle();  // old images may still be in use by frames in flight
        tg_.scene_ms.shutdown();
        rhi::RenderTargetDesc md;
        md.color = rhi::from_vk(desc_.color_format);
        md.depth = rhi::from_vk(ds_format_);
        md.samples = static_cast<VkSampleCountFlagBits>(msaa_);
        md.name = "render.scene_ms";
        if (!tg_.scene_ms.init(*alloc_, md, scene_ext)) {
            log::errorf("render", "MSAA %ux scene target (%ux%u) could not be created; drawing with one sample", msaa_,
                        scene_ext.width, scene_ext.height);
            tg_.scene_ms.shutdown();
            set_msaa(1);
        }
    }
    render_pass_ = rp_world_ = rp_main_;
    struct RpRestore {  // every exit: the world class back to single-sample, the main class for builds outside render()
        Renderer* r;
        ~RpRestore() {
            r->rp_world_ = r->rp_scene_;
            r->render_pass_ = r->rp_main_;
        }
    } rp_restore{this};
    ViewMatrices scaled_view = view_in;
    scaled_view.width = scene_ext.width;
    scaled_view.height = scene_ext.height;
    // [port] TAA (taa.cpp): every scene draw of the frame uses the sub-pixel jittered camera; without TAA `view_in` itself.
    // FSR frames jitter with FSR's sequence for the render / output ratio.
    const ViewMatrices taa_view =
        taa_begin(scaled_view, opts, fsr_on ? upscale_jitter_phases(scene_ext.width, target.extent.width) : 0u);
    const ViewMatrices& view = taa_view;
    if (world_ && mode == DrawMode::Lit) {
        world_frame_constants();  // clouds before any build of this frame
    } else {
        reset_world_constants();
    }
    DrawStats st;
    // The lists keep their vectors across frames (no per-frame allocation / zeroing of groups and items).
    Built& b = built(kBuiltMain);
    Built& skyb = built(kBuiltSky);
    skyb.items.clear();
    if (!build(frame, view, instances, mode, opts, b, st)) return false;
    if (!sky.empty()) {
        DrawStats sst;
        if (!build_sky(frame, view, sky, mode, opts, skyb, sky_storage_[0], sst)) return false;
        st.sky_draws = static_cast<uint32_t>(skyb.items.size());
    }
    const auto t1 = clock::now();

    const rhi::DeviceFns& fn = ctx_->vkd();
    // [port] MSAA: `scene` is the target of the scene buckets (multisampled with MSAA), `out` the single-sample target the
    // post chain works on and the frame is copied out of; without MSAA they are the same target.
    const bool msaa = msaa_ > 1;
    rhi::RenderTarget& out = tg_.scene;
    rhi::RenderTarget& scene = msaa ? tg_.scene_ms : tg_.scene;
    st.msaa = msaa_;
    const uint32_t W = scene.extent().width, H = scene.extent().height;
    scene.set_color_state(rhi::ImageState::Undefined);  // contents of the last frame are discarded
    scene.set_depth_state(rhi::ImageState::Undefined);
    out.set_color_state(rhi::ImageState::Undefined);
    out.set_depth_state(rhi::ImageState::Undefined);
    const bool depth = scene.has_depth();
    const float clear[4] = {target.clear[0], target.clear[1], target.clear[2], 1.0f};  // frame clear alpha is 0xFF
    // Space too: the "no stencil shadows" rule of space mode applies to the background scene only (SPACE_AND_OPEN.md A
    // item 2); the main space scene draws its shadow volumes, the darken and the soft-shadow resolve (RENDER.md 7.3:
    // ~20 volumes per frame in a space battle; g1f t06 Kuat frame order). The background build never draws Shadow items.
    const bool shadows_on = opts.shadows && opts.shader_detail > 0 && ds_format_ != VK_FORMAT_UNDEFINED;
    const bool soft_on = opts.soft_shadows && opts.shader_detail > 0;  // shader detail 0: StencilDarken (hard)
    const float bloom_strength = opts.bloom_strength >= 0.0f ? opts.bloom_strength : lighting_.bloom_strength;
    const bool bloom_on = opts.post && !opts.galactic && bloom_strength > 0.0f;

    const size_t s0 = bucket_begin(b.items, PhaseBucket::Shadow), s1 = bucket_begin(b.items, PhaseBucket::Water);
    const size_t h0 = bucket_begin(b.items, PhaseBucket::Heat);
    // R4 scene hook (particles): compute before any pass; draws after the Water / Transparent / Heat mesh items.
    SceneHook* const hook = scene_hook_;
    if (hook) {
        float fx = 0.0f, fy = 1.0f;
        const bool fog_on = mode == DrawMode::Lit && scene_fog_vals(lighting_, fx, fy);
        hook->scene_fog(fog_on, lighting_.fog_color, fx, fy);
        hook->prepare(frame, primary, view);
    }
    const bool hook_heat = hook && hook->has(PhaseBucket::Heat);
    const bool heat_on = opts.post && !opts.galactic && (h0 < b.items.size() || hook_heat);
    auto hook_pass = [&](PhaseBucket bucket, bool open_pass, uint32_t range_begin = 0, uint32_t range_end = UINT32_MAX) {
        SceneHookPass hp;
        hp.frame = &frame;
        hp.cmd = primary;
        hp.view = &view;
        hp.bucket = bucket;
        hp.color_format = desc_.color_format;
        hp.depth_format = depth ? scene.depth().vk_format() : VK_FORMAT_UNDEFINED;
        hp.stencil_format = depth && (scene.depth().aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? scene.depth().vk_format() : VK_FORMAT_UNDEFINED;
        hp.samples = static_cast<VkSampleCountFlagBits>(msaa_);
        hp.width = W;
        hp.height = H;
        if (open_pass) begin_target(fn, primary, scene, depth, nullptr, false, false);
        if (range_begin == 0 && range_end == UINT32_MAX) hook->draw(hp);
        else hook->draw_range(hp, range_begin, range_end);
        if (open_pass) fn.vkCmdEndRendering(primary);
    };

    // Chunks of a range for secondaries (contiguous, inside each bucket).
    auto record_pass = [&](size_t begin, size_t end, const float* clear_color, bool clear_depth) -> bool {
        std::vector<std::pair<size_t, size_t>>& chunks = chunks_;
        chunks.clear();
        const size_t min_chunk = std::max<uint32_t>(opts.min_draws_per_chunk, 1);
        if (opts.threads > 1 && desc_.js && end - begin >= 2 * min_chunk) {
            for (size_t s = begin; s < end;) {
                size_t e = s;
                while (e < end && b.items[e].bucket == b.items[s].bucket) ++e;
                const size_t n = e - s;
                const size_t k = std::clamp<size_t>(n / min_chunk, 1, opts.threads);
                // Boundaries moved to the end of the instanced run they fall into (runs never cross buckets), so
                // the calls are the same for any thread count.
                size_t from = s;
                for (size_t c = 1; c <= k; ++c) {
                    size_t to = c == k ? e : s + n * c / k;
                    if (to > from && to < e && b.run_end[to - 1] > to) to = b.run_end[to - 1];
                    to = std::min(to, e);
                    if (to > from) {
                        chunks.push_back({from, to});
                        from = to;
                    }
                }
                s = e;
            }
        }
        const bool secondaries = chunks.size() > 1;
        begin_target(fn, primary, scene, depth, clear_color, clear_depth, secondaries);
        if (!secondaries) {
            if (end > begin) record_range(primary, b, begin, end, W, H, true);
        } else {
            auto inh = rhi::vk_struct<VkCommandBufferInheritanceRenderingInfo>(
                VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO);
            const VkFormat color = desc_.color_format;
            inh.colorAttachmentCount = 1;
            inh.pColorAttachmentFormats = &color;
            inh.depthAttachmentFormat = depth ? scene.depth().vk_format() : VK_FORMAT_UNDEFINED;
            inh.stencilAttachmentFormat = depth && (scene.depth().aspect & VK_IMAGE_ASPECT_STENCIL_BIT) ? scene.depth().vk_format()
                                                                                                         : VK_FORMAT_UNDEFINED;
            inh.rasterizationSamples = static_cast<VkSampleCountFlagBits>(msaa_);  // [port] MSAA
            std::vector<VkCommandBuffer>& secs = secs_;
            secs.assign(chunks.size(), VK_NULL_HANDLE);
            desc_.js->parallel_for(static_cast<uint32_t>(chunks.size()), 1, [&](uint32_t cb, uint32_t ce) {
                for (uint32_t c = cb; c < ce; ++c) {
                    VkCommandBuffer sc = frame.secondary(desc_.js->current_slot(), inh);
                    if (!sc) continue;
                    record_range(sc, b, chunks[c].first, chunks[c].second, W, H, true);
                    fn.vkEndCommandBuffer(sc);
                    secs[c] = sc;
                }
            });
            if (std::find(secs.begin(), secs.end(), VK_NULL_HANDLE) != secs.end()) {
                fn.vkCmdEndRendering(primary);
                log::errorf("render", "secondary command buffer allocation failed");
                return false;
            }
            fn.vkCmdExecuteCommands(primary, static_cast<uint32_t>(secs.size()), secs.data());
            if (timing) timing->chunks += static_cast<uint32_t>(secs.size());
        }
        fn.vkCmdEndRendering(primary);
        return true;
    };
    if (timing) timing->chunks = 0;

    // 0. Land world: sky cube, water reflection / refraction targets (TERRAIN_WATER_POST.md 3.5), before the
    //    main scene. They render into their own targets with their own builds.
    const bool world_on = world_ != nullptr && mode == DrawMode::Lit;
    rhi::GpuTimer* gt = opts.gpu_timer && opts.gpu_timer->enabled() ? opts.gpu_timer : nullptr;
    const auto tw0 = clock::now();
    if (world_on) {
        const uint32_t world_scope = gt ? gt->begin_scope(primary, "world") : 0u;
        render_pass_ = rp_world_ = rp_scene_;  // [port] MSAA: water / sky-cube targets are single-sample
        const bool wok = world_prepass(frame, primary, view, instances, sky, mode, opts);
        render_pass_ = rp_world_ = rp_main_;
        if (gt) gt->end_scope(primary, world_scope);
        if (!wok) return false;
    }
    const auto tw1 = clock::now();
    const uint32_t scene_scope = gt ? gt->begin_scope(primary, "scene") : 0u;
    // 1. Background scene(s), then a depth clear (RENDER.md 1.2). Space: its Shadow / Bloom / Heat items are not drawn
    //    (no stencil shadows, no heat in the background scene) and the space FOW plane closes it (A.4).
    bool cleared = false;
    const bool space_fow = world_on && opts.space && world_->space() && world_->fow_enabled();
    if (!skyb.items.empty() || space_fow) {
        begin_target(fn, primary, scene, depth, clear, true, false);
        if (opts.space) {
            const size_t k0 = bucket_begin(skyb.items, PhaseBucket::Shadow), k1 = bucket_begin(skyb.items, PhaseBucket::Water);
            // Background bucket mask 0xbbf (SPACE_AND_OPEN.md A.5) leaves out Shadow (bit 6) and Bloom (bit 10); Heat
            // (bit 11) is in the mask but the background has no heat target / composite (A.4: heat 0), so it is skipped.
            const size_t k2 = bucket_begin(skyb.items, PhaseBucket::Bloom);
            if (k0 > 0) record_range(primary, skyb, 0, k0, W, H, true);
            if (k2 > k1) record_range(primary, skyb, k1, k2, W, H, true);
        } else {
            record_range(primary, skyb, 0, skyb.items.size(), W, H, true);
        }
        if (space_fow) {
            world_draw_space_fow(frame, primary, background_view(view), W, H);  // the background camera
        }
        fn.vkCmdEndRendering(primary);
        cleared = true;
    }
    // 1b. Terrain patches (bucket 0, the terrain render task precedes the bucket's mesh draws), then the
    //     terrain decorations 3, 2, 1, 0, 4 (DECORATIONS.md 0 item 2).
    bool depth_cleared = false;
    if (world_on) {
        begin_target(fn, primary, scene, depth, cleared ? nullptr : clear, true, false);
        world_draw_terrain(frame, primary, view, W, H, false, true);
        world_draw_decorations(frame, primary, view, W, H);
        fn.vkCmdEndRendering(primary);
        cleared = depth_cleared = true;
    }
    // 2. Buckets Terrain .. Opaque-non-occluding, plus the Shadow bucket when shadows are on.
    const size_t m1_end = shadows_on ? s1 : s0;
    // A failed record still closes the GPU timer scope, so no stale timing is reported as this frame's.
    auto fail_scene = [&] {
        if (gt) gt->end_scope(primary, scene_scope);
        return false;
    };
    if (!record_pass(0, m1_end, cleared ? nullptr : clear, !depth_cleared)) return fail_scene();
    count_items(b, 0, m1_end, st);
    if (!shadows_on) st.not_drawn += static_cast<uint32_t>(s1 - s0);
    // 3. Stencil darken (after the Shadow bucket, only if volumes were drawn).
    if (shadows_on && s1 > s0) {
        const materials::MaterialEffect* dk = post_effect(soft_on ? 1 : 0);
        bool ok = false;
        if (dk) {
            begin_target(fn, primary, scene, depth, nullptr, false, false);
            // Soft path [port]: the blur reads the frame's ALPHA as the shadow mask, but the port's passes write alpha
            // (texture alpha of terrain, units, decorations, water ...) where the original keeps it at 1 (RGB-only
            // writes, TERRAIN_WATER_POST.md 4.3). Reset it to 1 first, else every pixel with alpha < 1 takes the
            // shadow colour (playtest 3: the whole land frame darkened).
            ok = !soft_on || post_darken(frame, primary, *dk, W, H, true);
            if (ok) ok = post_darken(frame, primary, *dk, W, H);
            fn.vkCmdEndRendering(primary);
        }
        if (ok && soft_on) {
            // StencilDarkenFinalBlur: back buffer -> SceneTexture, 4 taps, ZERO / SRCCOLOR (4.3). [port] MSAA: resolved.
            if (msaa) {
                resolve_into(fn, primary, scene, &tg_.scene_tex, nullptr);
                tg_.scene_tex.transition_color(fn, primary, rhi::ImageState::ShaderRead);
            } else {
                tg_.scene_tex.copy_from(fn, primary, copy_of(scene, rhi::ImageState::ColorAttachment), VK_FILTER_NEAREST,
                                        rhi::ImageState::ShaderRead);
                scene.set_color_state(rhi::ImageState::ColorAttachment);
            }
            const materials::MaterialEffect* fb = post_effect(2);
            ok = false;
            if (fb)
                for (const auto& p : fb->passes)
                    if (!p.restore_only && p.program_status == materials::ProgramStatus::Translated) {
                        PostSpec ps;
                        ps.fx = fx_post_[2];
                        ps.pass = &p;
                        ps.render_pass = rp_main_;  // [port] MSAA: the main scene target
                        ps.width = W;
                        ps.height = H;
                        ps.slot_default = tg_.scene_tex.color().view;  // samplers 0-3 = SceneTexture (alpha = shadow mask)
                        ps.prim = true;  // shadow colour arrives as COLOR0 of the prim vertices
                        begin_target(fn, primary, scene, depth, nullptr, false, false);
                        ok = post_translated(frame, primary, ps);
                        fn.vkCmdEndRendering(primary);
                        break;
                    }
            st.soft_shadows = ok;
        }
        st.darkened = ok;
        if (!ok) ++st.post_pending;
    }
    // 3a. [port] Screen-space ambient occlusion over the opaque scene (ssao.hpp): not in galactic / space frames,
    //     mirrored or coverage-alpha renders. Depth is left sampled; the next pass makes it an attachment again.
    if (opts.ambient_occlusion && opts.ao_strength > 0.0f && depth && !opts.galactic && !opts.space && !opts.mirrored &&
        !opts.coverage_alpha) {
        if (!ssao_) {
            ssao_ = std::make_unique<SsaoPass>();
            ssao_->init(*ctx_, *alloc_);  // a failure is logged once; record() then does nothing
        }
        // [port] MSAA: the AO reads the resolved depth (sample 0) and multiplies into the multisampled colour.
        if (msaa) resolve_into(fn, primary, scene, nullptr, &out);
        ssao_->record(frame, primary, scene, out, desc_.color_format, static_cast<VkSampleCountFlagBits>(msaa_), view,
                      opts.ao_strength, opts.ao_radius, 1.0f, gt);
    }
    // 3b. Water patches (bucket 7, the water render task before the bucket's mesh draws).
    if (world_on) {
        begin_target(fn, primary, scene, depth, nullptr, false, false);
        world_draw_water(frame, primary, view, W, H);
        world_draw_water_tracks(frame, primary, view, W, H);  // decoration type 5 right after the water
        fn.vkCmdEndRendering(primary);
    }
    // Laser projectiles (PROJECTILE_NEBULA_DRAW.md 1): one PrimAdditive batch per kind at the end of the Transparent
    // bucket [port: where the original's single flush task sorts among the bucket's tasks is not traced].
    auto laser_pass = [&]() {
        if ((lasers_.empty() && !beams_active_) || mode != DrawMode::Lit) return;
        begin_target(fn, primary, scene, depth, nullptr, false, false);
        if (!lasers_.empty()) draw_lasers(frame, primary, view, st);
        if (beams_active_) draw_beams(frame, primary, view, st);  // ability beams: additive ribbons / dots / lightning (bucket 8)
        fn.vkCmdEndRendering(primary);
    };
    // 4. Water .. Bloom buckets (Transparent back-to-front). R4: the hook's Water / Transparent draws follow the
    //    bucket's mesh items (their own pass each, so the mesh passes may still use secondaries).
    if (hook && (hook->has(PhaseBucket::Water) || hook->has(PhaseBucket::Transparent))) {
        const size_t tb0 = bucket_begin(b.items, PhaseBucket::Transparent), l0 = bucket_begin(b.items, PhaseBucket::Lasers);
        if (!record_pass(s1, tb0, nullptr, false)) return fail_scene();
        if (hook->has(PhaseBucket::Water)) hook_pass(PhaseBucket::Water, true);
        const uint32_t nh = hook->has(PhaseBucket::Transparent) ? hook->draw_count(PhaseBucket::Transparent) : 0u;
        if (nh == 0) {
            if (!record_pass(tb0, l0, nullptr, false)) return fail_scene();
            if (hook->has(PhaseBucket::Transparent)) hook_pass(PhaseBucket::Transparent, true);
        } else {
            // R4 B.5: bucket 8 back-to-front per renderable - the hook's systems merged with the mesh items by squared
            // camera distance (both lists already far to near; equal distances: meshes first [port]).
            const auto mesh_d = [&](size_t idx) { return b.groups[b.items[idx].group].depth_key; };
            const auto hook_d = [&](uint32_t k) { return hook->draw_distance(PhaseBucket::Transparent, k); };
            size_t i = tb0;
            uint32_t k = 0;
            while (i < l0 || k < nh) {
                if (k < nh && (i >= l0 || hook_d(k) > mesh_d(i))) {
                    uint32_t k2 = k + 1;
                    while (k2 < nh && (i >= l0 || hook_d(k2) > mesh_d(i))) ++k2;
                    hook_pass(PhaseBucket::Transparent, true, k, k2);
                    st.hook_interleaved += k2 - k < nh ? 1u : 0u;
                    k = k2;
                } else {
                    size_t i2 = i + 1;
                    while (i2 < l0 && !(k < nh && hook_d(k) > mesh_d(i2))) ++i2;
                    if (!record_pass(i, i2, nullptr, false)) return fail_scene();
                    i = i2;
                }
            }
        }
        laser_pass();
        if (!record_pass(l0, h0, nullptr, false)) return fail_scene();
    } else if ((lasers_.empty() && !beams_active_) || mode != DrawMode::Lit) {
        if (!record_pass(s1, h0, nullptr, false)) return fail_scene();
    } else {
        const size_t l0 = bucket_begin(b.items, PhaseBucket::Lasers);
        if (!record_pass(s1, l0, nullptr, false)) return fail_scene();
        laser_pass();
        if (!record_pass(l0, h0, nullptr, false)) return fail_scene();
    }
    count_items(b, s1, h0, st);
    if (gt) gt->end_scope(primary, scene_scope);
    // [port] MSAA: the finished scene (colour average, depth sample 0) into the single-sample target of the post chain.
    if (msaa) {
        const uint32_t msaa_scope = gt ? gt->begin_scope(primary, "msaa") : 0u;
        resolve_into(fn, primary, scene, &out, &out);
        if (gt) gt->end_scope(primary, msaa_scope);
    }
    // 5. Post: SceneTexture copy after Transparent, heat, bloom (RENDER.md 1.4/1.5, 4.4/4.5).
    const auto tp0 = clock::now();
    const uint32_t post_scope = gt ? gt->begin_scope(primary, "post") : 0u;
    if (heat_on || bloom_on) {
        tg_.scene_tex.copy_from(fn, primary, copy_of(out, rhi::ImageState::ColorAttachment), VK_FILTER_NEAREST,
                                rhi::ImageState::ShaderRead);
        out.set_color_state(rhi::ImageState::ColorAttachment);
    }
    if (heat_on) {
        // Heat bucket into the back buffer cleared to 0x008080FF (A 0, R G 128, B 255), scene depth kept. [port] MSAA: drawn
        // into the multisampled target (its pipelines are the scene's) and resolved into DistortionTexture.
        const float heat_clear[4] = {128.0f / 255.0f, 128.0f / 255.0f, 1.0f, 0.0f};
        begin_target(fn, primary, scene, depth, heat_clear, false, false);
        record_range(primary, b, h0, b.items.size(), W, H, true);
        if (hook_heat) hook_pass(PhaseBucket::Heat, false);  // R4: heat particles after the heat meshes
        fn.vkCmdEndRendering(primary);
        count_items(b, h0, b.items.size(), st);
        if (msaa) {
            resolve_into(fn, primary, scene, &tg_.distortion, nullptr);
            tg_.distortion.transition_color(fn, primary, rhi::ImageState::ShaderRead);
        } else {
            tg_.distortion.copy_from(fn, primary, copy_of(scene, rhi::ImageState::ColorAttachment), VK_FILTER_NEAREST,
                                     rhi::ImageState::ShaderRead);
            scene.set_color_state(rhi::ImageState::ColorAttachment);
        }
        // SceneHeat composite (DistortionAmount = FXO default 0.25 unless overridden).
        const materials::MaterialEffect* he = post_effect(4);
        bool ok = false;
        if (he)
            for (const auto& p : he->passes)
                if (!p.restore_only && p.program_status == materials::ProgramStatus::Translated) {
                    PostSpec ps;
                    ps.fx = fx_post_[4];
                    ps.pass = &p;
                    ps.render_pass = rp_post_;
                    ps.width = W;
                    ps.height = H;
                    const PostTexture heat_tex[2] = {{"SCENETEXTURE", tg_.scene_tex.color().view},
                                                     {"DISTORTIONTEXTURE", tg_.distortion.color().view}};
                    const float amount[1] = {opts.heat_amount};
                    const PostValue heat_val[1] = {{"DistortionAmount", amount}};
                    ps.textures = heat_tex;
                    if (opts.heat_amount >= 0.0f) ps.values = heat_val;
                    begin_target(fn, primary, out, false, nullptr, false, false);
                    ok = post_translated(frame, primary, ps);
                    fn.vkCmdEndRendering(primary);
                    break;
                }
        if (!ok) {  // composite not ready: put the pre-heat scene back
            out.copy_from(fn, primary, copy_of(tg_.scene_tex, rhi::ImageState::ShaderRead), VK_FILTER_NEAREST,
                            rhi::ImageState::ColorAttachment);
            ++st.post_pending;
        }
        st.heat = ok;
    } else {
        st.not_drawn += static_cast<uint32_t>(b.items.size() - h0);
    }
    if (bloom_on) {
        // SceneBloom: p0 bright filter -> A (1/4), p1 x4 ping-pong (B, A, B, A), p2 screen-blend into the back buffer.
        const materials::MaterialEffect* bl = post_effect(3);
        const materials::ResolvedPass* bp[3] = {nullptr, nullptr, nullptr};  // the first three non-restore passes
        size_t nbp = 0;
        if (bl)
            for (const auto& p : bl->passes)
                if (!p.restore_only && nbp < 3) bp[nbp++] = &p;
        bool ok = nbp >= 3;
        for (size_t k = 0; k < nbp; ++k) ok = ok && bp[k]->program_status == materials::ProgramStatus::Translated;
        const float cutoff[1] = {lighting_.bloom_cutoff}, strength[1] = {bloom_strength}, size[1] = {lighting_.bloom_size};
        auto bloom_pass = [&](const materials::ResolvedPass* p, rhi::RenderTarget& dst, VkImageView src, float iteration,
                              uint8_t rp) {
            PostSpec ps;
            ps.fx = fx_post_[3];
            ps.pass = p;
            ps.render_pass = rp;
            ps.width = dst.extent().width;
            ps.height = dst.extent().height;
            const PostTexture tex[1] = {{"SCENETEXTURE", src}};
            const float it[1] = {iteration};
            const PostValue vals[4] = {{"BloomCutoff", cutoff}, {"BloomStrength", strength}, {"BloomSize", size}, {"BloomIteration", it}};
            ps.textures = tex;
            ps.values = vals;
            begin_target(fn, primary, dst, false, nullptr, false, false);
            const bool r = post_translated(frame, primary, ps);
            fn.vkCmdEndRendering(primary);
            return r;
        };
        if (ok) ok = bloom_pass(bp[0], tg_.bloom_a, tg_.scene_tex.color().view, 0.0f, rp_post_);
        rhi::RenderTarget* last = &tg_.bloom_a;
        for (int i = 0; ok && i < 4; ++i) {  // iterations 0..3 write B, A, B, A
            rhi::RenderTarget& src = (i % 2 == 0) ? tg_.bloom_a : tg_.bloom_b;
            rhi::RenderTarget& dst = (i % 2 == 0) ? tg_.bloom_b : tg_.bloom_a;
            src.transition_color(fn, primary, rhi::ImageState::ShaderRead);
            ok = bloom_pass(bp[1], dst, src.color().view, static_cast<float>(i), rp_post_);
            last = &dst;
        }
        if (ok) {
            last->transition_color(fn, primary, rhi::ImageState::ShaderRead);
            ok = bloom_pass(bp[2], out, last->color().view, 0.0f, rp_post_);
        }
        st.bloom = ok;
        if (!ok) ++st.post_pending;
    }
    bool fsr_out = false;  // [port] FSR: the frame's image is fsr_->output() (display size)
    if ((opts.taa || fsr_on) && taa_) {  // [port] TAA resolve + sharpen / FSR at the end of the post chain (taa.cpp, fsr.cpp)
        const uint32_t taa_scope = gt ? gt->begin_scope(primary, xess_on ? "xess" : (fsr_on ? "fsr" : "taa")) : 0u;
        // Object motion: the opaque draws of keyed instances (Instance::motion_id) that moved since the last TAA frame.
        // Alpha-tested / fading passes are left out (their cut-outs would write vectors over what shows through).
        std::vector<TaaMover>& movers = taa_->movers();
        movers.clear();
        if (!taa_->history_reset()) {
            const uint32_t always = static_cast<uint32_t>(materials::CompareOp::Always);
            for (size_t i = 0; i < s0; ++i) {
                const Built::Item& it = b.items[i];
                if (it.bucket != PhaseBucket::Opaque && it.bucket != PhaseBucket::Occluded && it.bucket != PhaseBucket::OpaqueNonOccluding)
                    continue;
                const Built::Group& gr = b.groups[it.group];
                if (it.pass < 0 || it.fading || !gr.inst->motion_id) continue;
                const GpuSubmesh& g = *gr.g;
                if (g.passes[static_cast<size_t>(it.pass)].pass->push.alpha_func != always || (it.tp && it.tp->can_discard)) continue;
                Mat3x4 delta;
                if (!taa_->motion_delta(gr.inst->motion_id, gr.inst->transform, delta)) continue;
                TaaMover m;
                m.vertices = g.vertices.handle;
                m.indices = g.indices.handle;
                m.index_type = g.index_type;
                m.index_count = g.index_count;
                m.skinned = g.skinned;
                m.translated = it.tp != nullptr;
                m.draw_index = static_cast<uint32_t>(i);
                // previous view-projection x (delta as a 4x4)
                Mat4 d4;
                for (int r = 0; r < 3; ++r)
                    for (int c = 0; c < 4; ++c) d4.m[r][c] = delta.m[r][c];
                d4.m[3][0] = d4.m[3][1] = d4.m[3][2] = 0.0f;
                d4.m[3][3] = 1.0f;
                const Mat4 pm = taa_->previous_view_proj() * d4;
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) m.prev_clip[r][c] = pm.m[r][c];
                movers.push_back(m);
            }
        }
        taa_->commit_motion(instances);
        if (xess_on) {  // [port] XeSS: the same inputs as FSR, then the upscale into xess_->output()
            fsr_out = taa_->fsr_inputs(frame, primary, out, b.set0) && xess_->dispatch(primary, out, *taa_, taa_->history_reset());
            st.xess = fsr_out;
            st.taa_reset = fsr_out && taa_->history_reset();
        } else if (fsr_on) {  // [port] FSR: motion vectors + depth of the resolved scene, then the upscale into fsr_->output()
            fsr_out = taa_->fsr_inputs(frame, primary, out, b.set0) &&
                      fsr_->dispatch(primary, out, *taa_, opts.fsr_sharpness, opts.frame_ms, taa_->history_reset());
            st.fsr = fsr_out;
            st.taa_reset = fsr_out && taa_->history_reset();
        } else {
            st.taa = taa_->resolve(frame, primary, out, opts.taa_sharpen,
                                   opts.taa_weight >= 0.0f ? opts.taa_weight : kTaaCurrentWeight, b.set0);
            st.taa_reset = st.taa && taa_->history_reset();
        }
        st.taa_movers = taa_->last_movers();
        if (gt) gt->end_scope(primary, taa_scope);
    }
    if (opts.smaa && !fsr_on) {  // [port] SMAA 1x on the finished frame (smaa.hpp; not on FSR frames); SceneTexture is free again here
        if (!smaa_) {
            smaa_ = std::make_unique<SmaaPass>();
            smaa_->init(*ctx_, *alloc_);  // a failure is logged once; record() then does nothing
        }
        const uint32_t smaa_scope = gt ? gt->begin_scope(primary, "smaa") : 0u;
        st.smaa = smaa_->record(primary, out, tg_.scene_tex);
        if (gt) gt->end_scope(primary, smaa_scope);
    }
    if (gt) gt->end_scope(primary, post_scope);
    const auto tp1 = clock::now();
    // 6. Out to the caller's image (StretchRect, point filter; a render-scaled scene is stretched with a linear filter).
    rhi::CopyImage dst;
    dst.image = target.color;
    dst.extent = target.extent;
    dst.format = desc_.color_format;
    dst.before = rhi::ImageState::Undefined;
    dst.after = target.color_after;
    if (fsr_out) {  // [port] FSR / XeSS output (output size, General) -> the caller's image; the scene keeps its state
        rhi::RenderTarget& up = xess_on ? xess_->output() : fsr_->output();
        rhi::cmd_stretch_rect(fn, primary, copy_of(up, rhi::ImageState::General), dst, VK_FILTER_NEAREST);
        up.set_color_state(rhi::ImageState::General);
    } else {
        // A render-scaled scene (or an FSR frame whose upscale failed) is stretched up with a linear filter.
        const bool native = W == target.extent.width && H == target.extent.height;
        rhi::cmd_stretch_rect(fn, primary, copy_of(out, rhi::ImageState::ColorAttachment), dst,
                              native ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        out.set_color_state(rhi::ImageState::ColorAttachment);
    }
    st.render_width = W;
    st.render_height = H;
    const auto t2 = clock::now();
    if (stats) *stats = st;
    if (timing) {
        timing->build_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        timing->record_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        timing->world_ms = std::chrono::duration<double, std::milli>(tw1 - tw0).count();
        timing->post_ms = std::chrono::duration<double, std::milli>(tp1 - tp0).count();
        timing->cull_ms = b.cull_ms;
        timing->sort_ms = b.sort_ms;
        timing->fill_ms = b.fill_ms;
    }
    return true;
}

// ------------------------------------------------------------------------------------ pass helpers

void begin_pass(const rhi::DeviceFns& fn, VkCommandBuffer cmd, const PassTarget& t) {
    VkImageMemoryBarrier2 b[2];
    uint32_t nb = 0;
    b[nb++] = rhi::transition(t.color, rhi::ImageState::Undefined, rhi::ImageState::ColorAttachment);
    if (t.depth)
        b[nb++] = rhi::transition(t.depth, rhi::ImageState::Undefined, rhi::ImageState::DepthAttachment,
                                  {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1});
    rhi::cmd_barrier(fn, cmd, std::span<const VkImageMemoryBarrier2>(b, nb));

    auto ca = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    ca.imageView = t.color_view;
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color = {{t.clear[0], t.clear[1], t.clear[2], t.clear[3]}};
    auto da = rhi::vk_struct<VkRenderingAttachmentInfo>(VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO);
    da.imageView = t.depth_view;
    da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil = {1.0f, 0};
    auto ri = rhi::vk_struct<VkRenderingInfo>(VK_STRUCTURE_TYPE_RENDERING_INFO);
    ri.renderArea = {{0, 0}, t.extent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    ri.pDepthAttachment = t.depth ? &da : nullptr;
    if (t.secondaries) ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
    fn.vkCmdBeginRendering(cmd, &ri);
    if (t.secondaries) return;  // only vkCmdExecuteCommands may follow; secondaries set their own viewport

    // D3D-style clip space (Y up): flip with a negative viewport height (Vulkan 1.1 core).
    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = static_cast<float>(t.extent.height);
    vp.width = static_cast<float>(t.extent.width);
    vp.height = -static_cast<float>(t.extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    fn.vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc{{0, 0}, t.extent};
    fn.vkCmdSetScissor(cmd, 0, 1, &sc);
}

void end_pass(const rhi::DeviceFns& fn, VkCommandBuffer cmd, const PassTarget& t) {
    fn.vkCmdEndRendering(cmd);
    rhi::cmd_barrier(fn, cmd, rhi::transition(t.color, rhi::ImageState::ColorAttachment, t.color_after));
}

// ------------------------------------------------------------------------------------- streamer

ModelStreamer::ModelStreamer(Renderer& r, assets::AssetCache& cache, jobs::JobSystem& js)
    : renderer_(r), cache_(cache), js_(js), counter_(std::make_unique<jobs::JobCounter>()) {}

ModelStreamer::~ModelStreamer() { wait_jobs(); }

uint32_t ModelStreamer::request(std::string name) {
    entries_.push_back(std::make_unique<Entry>());
    Entry* e = entries_.back().get();
    e->name = std::move(name);
    js_.spawn(
        *counter_,
        [this, e] {
            const auto t0 = std::chrono::steady_clock::now();
            std::shared_ptr<const assets::ModelAsset> m = cache_.model(e->name);
            if (!m) {
                e->error = "model load failed";
                for (const auto& f : cache_.failures())
                    if (f.kind == "model") e->error = f.key + ": " + f.reason;
                e->state.store(static_cast<int>(State::Failed));
            } else {
                e->state.store(static_cast<int>(State::Uploading));
                std::string err;
                const bool ok = renderer_.upload_model(
                    m, [this](std::string_view n, assets::TextureRole role) { return cache_.texture(n, role); }, e->model,
                    &js_, &err);
                if (!ok) {
                    e->error = err;
                    e->state.store(static_cast<int>(State::Failed));
                }
            }
            e->cpu_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            e->finished.store(true, std::memory_order_release);
        },
        jobs::Priority::Background);
    return static_cast<uint32_t>(entries_.size() - 1);
}

void ModelStreamer::update() {
    for (auto& e : entries_) {
        if (!e->job_done) e->job_done = e->finished.load(std::memory_order_acquire);
        if (!e->job_done || e->state.load() != static_cast<int>(State::Uploading)) continue;
        if (!e->pipelines_requested) {
            renderer_.prepare_pipelines(e->model);  // start compiling before the buffers land
            e->pipelines_requested = true;
        }
        if (renderer_.make_resident(e->model)) e->state.store(static_cast<int>(State::Ready));
    }
}

ModelStreamer::State ModelStreamer::state(uint32_t h) const {
    if (h >= entries_.size()) return State::Failed;
    return static_cast<State>(entries_[h]->state.load());
}

GpuModel* ModelStreamer::model(uint32_t h) {
    if (h >= entries_.size() || !entries_[h]->finished.load(std::memory_order_acquire)) return nullptr;
    return &entries_[h]->model;
}

const std::string& ModelStreamer::error(uint32_t h) const {
    static const std::string kNone;
    return h < entries_.size() ? entries_[h]->error : kNone;
}

double ModelStreamer::cpu_ms(uint32_t h) const { return h < entries_.size() ? entries_[h]->cpu_ms : 0.0; }

void ModelStreamer::wait_jobs() {
    if (counter_) js_.wait(*counter_);
}

void ModelStreamer::finish_pending() {
    bool pending = false;
    for (const auto& e : entries_) pending = pending || e->state.load() < static_cast<int>(State::Ready);
    if (!pending) return;
    wait_jobs();
    for (auto& e : entries_)
        if (e->state.load() == static_cast<int>(State::Uploading) && !renderer_.finish_uploads(e->model)) {
            e->error = "upload failed";
            e->state.store(static_cast<int>(State::Failed));
        }
    update();
}

void ModelStreamer::destroy_all() {
    wait_jobs();
    for (auto& e : entries_) renderer_.destroy_model(e->model);
    entries_.clear();
}

// ------------------------------------------------------------------------------------ validation

namespace {
void set_env(const char* k, const std::string& v) {
    _putenv_s(k, v.c_str());
}
}  // namespace

bool setup_validation_env(const std::string& source_dir) {
    std::error_code ec;
    const std::filesystem::path vvl = alamo::path_from_utf8(source_dir) / "toolchain" / "vvl" / "bin";
    const char* lp = std::getenv("VK_LAYER_PATH");
    if ((!lp || !*lp) && std::filesystem::exists(vvl / "VkLayer_khronos_validation.json", ec))
        set_env("VK_LAYER_PATH", vvl.lexically_normal().make_preferred().string());
    const char* dis = std::getenv("VK_LOADER_LAYERS_DISABLE");
    if (!dis || !*dis) set_env("VK_LOADER_LAYERS_DISABLE", "~implicit~");
    lp = std::getenv("VK_LAYER_PATH");
    if (!lp || !*lp) return false;
    const std::string s(lp);
    const char sep = ';';
    size_t start = 0;
    while (start <= s.size()) {
        const size_t end = std::min(s.find(sep, start), s.size());
        const std::string dir = s.substr(start, end - start);
        if (!dir.empty() && std::filesystem::exists(alamo::path_from_utf8(dir) / "VkLayer_khronos_validation.json", ec))
            return true;
        start = end + 1;
    }
    return false;
}

}  // namespace alamo::render

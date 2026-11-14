// Renderer: frame constants, build and record of the draw lists (split from renderer.cpp).
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
#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/materials/constants.hpp"
#include "alamo/materials/params.hpp"
#include "alamo/render/convert.hpp"
#include "alamo/render/frustum.hpp"
#include "alamo/render/ssao.hpp"
#include "alamo/render/world.hpp"
#include "render_internal.hpp"
#include "fx2_reader.hpp"
#include "gamma.hpp"  // [port] output gamma
#include "smaa.hpp"  // [port] SMAA
#include "taa.hpp"  // [port] TAA
#include "renderer_shared.hpp"

namespace alamo::render {
using namespace detail;

// ------------------------------------------------------------------------------ frame constants

const SceneLightConstants& Renderer::scene_lights() {
    SlcCache& c = slc_cache_;
    const Lighting& l = lighting_;
    const bool same = c.valid && std::memcmp(&c.sun, &l.sun, sizeof c.sun) == 0 &&
                      std::memcmp(&c.fill[0], &l.fill[0], sizeof c.fill[0]) == 0 &&
                      std::memcmp(&c.fill[1], &l.fill[1], sizeof c.fill[1]) == 0 &&
                      std::memcmp(&c.ambient, &l.ambient, sizeof c.ambient) == 0;
    if (!same) {
        c.sun = l.sun;
        c.fill[0] = l.fill[0];
        c.fill[1] = l.fill[1];
        c.ambient = l.ambient;
        c.slc = scene_light_constants(l);
        c.valid = true;
    }
    return c.slc;
}

void Renderer::frame_constants(const ViewMatrices& v, uint32_t extras, materials::FrameCB& cb) {
    fill_frame_cb(v, time_, cb);
    fill_frame_lights(lighting_, cb);
    const SceneLightConstants& slc = scene_lights();
    std::copy(slc.sph_all.begin(), slc.sph_all.end(), cb.sph_all);
    std::copy(slc.sph_fill.begin(), slc.sph_fill.end(), cb.sph_fill);
    if (extras & kFcbFog) apply_scene_fog(lighting_, cb);
    if (extras & kFcbFade) {
        distance_fade_vals(fade_begin_, fade_end_, cb.fog_vals_fade_vals.z, cb.fog_vals_fade_vals.w);
        cb.shadow_params.x = kShadowExtrusionDistance;  // SHADOW_EXTRUSION_DISTANCE, FrameCB.shadow_params.x
    }
    if (extras & kFcbWorld) {
        cb.cloud_tex_u = cloud_u_;
        cb.cloud_tex_v = cloud_v_;
        cb.fow_tex_u = fow_u_;
        cb.fow_tex_v = fow_v_;
    }
}

namespace {
bool sh_zero(const materials::float4* m, size_t count) {
    for (size_t i = 0; i < count; ++i)
        if (m[i].x != 0.0f || m[i].y != 0.0f || m[i].z != 0.0f || m[i].w != 0.0f) return false;
    return true;
}
bool sh_contract_ok(const materials::FrameCB& cb, const SceneLightConstants& slc) {
    const bool lit_all = !sh_zero(slc.sph_all.data(), slc.sph_all.size());
    const bool lit_fill = !sh_zero(slc.sph_fill.data(), slc.sph_fill.size());
    return !(lit_all && sh_zero(cb.sph_all, 12)) && !(lit_fill && sh_zero(cb.sph_fill, 12));
}
}  // namespace

bool Renderer::frame_cb_sh_ok(const materials::FrameCB& cb, const Lighting& lighting) {
    return sh_contract_ok(cb, scene_light_constants(lighting));
}

bool Renderer::upload_frame_cb(rhi::Frame& frame, const materials::FrameCB& cb, rhi::BufferRange& out) {
    if (!sh_contract_ok(cb, scene_lights()) && fcb_violations_.fetch_add(1) == 0)
        log::errorf("render", "FrameCB uploaded with zero SH under a lit scene (SH lighting contract)");
    const rhi::UniformAlloc a = frame.alloc_uniform(sizeof cb);
    if (!a) return false;
    std::memcpy(a.ptr, &cb, sizeof cb);
    out = {a.buffer, a.offset, sizeof cb};
    return true;
}

// ------------------------------------------------------------------------------------------ draw




namespace detail {

// Every corner of the box deeper than `depth` along the view direction (camera looks along -Z).
bool beyond_depth(const Mat4& view, const assets::Aabb& b, float depth) {
    for (int i = 0; i < 8; ++i) {
        const Float4 p{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z, 1.0f};
        if (-(view * p).z <= depth) return false;
    }
    return true;
}

materials::CullMode mirrored_cull(materials::CullMode c) {
    return c == materials::CullMode::Front ? materials::CullMode::Back
                                           : (c == materials::CullMode::Back ? materials::CullMode::Front : c);
}

// RecordOptions::coverage_alpha [port]: "over" passes accumulate coverage (One / InvSrcAlpha), other blends keep
// the destination alpha (an additive glow quad would otherwise write its texture alpha over its whole square).
void coverage_alpha_key(materials::PsoKey& k) {
    if (!k.blend_enable) return;
    if (k.src_color == materials::BlendFactor::SrcAlpha && k.dst_color == materials::BlendFactor::OneMinusSrcAlpha &&
        k.color_op == materials::BlendOp::Add) {
        k.src_alpha = materials::BlendFactor::One;
        k.dst_alpha = materials::BlendFactor::OneMinusSrcAlpha;
        k.alpha_op = materials::BlendOp::Add;
    } else {
        k.color_write_mask = static_cast<uint8_t>(k.color_write_mask & 0x7u);
    }
}

bool is_cloud_slot(const materials::ResolvedPass& p, uint32_t slot) {
    for (const materials::SamplerBinding& sb : p.samplers)
        if (sb.slot == slot) return alamo::iequals(sb.texture_semantic, "CLOUD_TEXTURE");
    return false;
}
// FOW_TEXTURE semantic (or the FXOs' m_FOWTexture parameter, which carries it).
bool is_fow_binding(const materials::SamplerBinding& sb) {
    return alamo::iequals(sb.texture_semantic, "FOW_TEXTURE") || alamo::iequals(sb.texture_param, "m_FOWTexture");
}
bool is_fow_slot(const materials::ResolvedPass& p, uint32_t slot) {
    for (const materials::SamplerBinding& sb : p.samplers)
        if (sb.slot == slot) return is_fow_binding(sb);
    return false;
}

template <class F>
void for_range(jobs::JobSystem* js, bool parallel, uint32_t count, uint32_t grain, F&& fn) {
    if (parallel && js && count > grain) js->parallel_for(count, grain, fn);
    else if (count) fn(0u, count);
}

}  // namespace detail

Renderer::Built& Renderer::built(BuiltSlot slot) {
    std::unique_ptr<Built>& b = built_[slot];
    if (!b) b = std::make_unique<Built>();
    return *b;
}

bool Renderer::xt_prepare(XTemplate& t, rhi::Frame& frame, uint32_t thread, const EngineFrame& f, const float (&pixel_offset)[4],
                          bool fog) {
    t.ok = false;
    t.set0 = t.set1 = VK_NULL_HANDLE;
    t.vs_r = t.ps_r = t.vib_r = t.pib_r = rhi::BufferRange{};
    if (!t.g || !t.pass || !t.tp || !t.model || !t.g->effect || !t.g->effect->effect || !t.pass->pass || !t.pass->pass->vs_plan ||
        !t.pass->pass->ps_plan)
        return false;
    const GpuSubmesh& g = *t.g;
    const GpuPass& p = *t.pass;
    const GpuModel& gm = *t.model;
    const materials::TranslatedProgram& tp = *t.tp;
    const fx::Effect& fxe = *g.effect->effect;
    const materials::ResolvedPass& rp = *p.pass;
    const bool ib = tp.vs_int_bool || tp.ps_int_bool;
    if (!t.analysed) xt_analyse(t, fxe, rp);
    if (t.params != g.params.get()) {  // material values changed: both frame images are stale
        t.params = g.params.get();
        t.slots[0].valid = t.slots[1].valid = false;
    }
    t.cur = nullptr;
    EngineFrame masked;
    mask_frame(f, t.frame_mask, masked);
    for (const XTemplate::Slot& s : t.slots)
        if (s.valid && std::memcmp(&s.frame, &masked, sizeof masked) == 0) t.cur = &s;
    if (!t.cur) {
        XTemplate::Slot& s = t.slots[t.next];
        t.next ^= 1u;
        xt_fill_slot(t, s, masked, fxe, rp, t.params, ib);
        t.cur = &s;
    }
    const bool inst = tp.vs.iface.instanced;  // Q3: float images are storage-buffer ranges (storage alignment)
    auto upload = [&](const void* src, size_t bytes, rhi::BufferRange& r, bool storage) {
        const rhi::UniformAlloc a = storage ? frame.alloc_storage(bytes) : frame.alloc_uniform(bytes);
        if (!a) return false;
        std::memcpy(a.ptr, src, bytes);
        r = {a.buffer, a.offset, bytes};
        return true;
    };
    if (t.vs_static && !upload(&t.cur->vs, sizeof t.cur->vs, t.vs_r, inst)) return false;
    if (t.ps_static && !upload(&t.cur->ps, sizeof t.cur->ps, t.ps_r, inst)) return false;
    if (ib && !t.ib_draw &&
        (!upload(&t.cur->vib, sizeof t.cur->vib, t.vib_r, false) || !upload(&t.cur->pib, sizeof t.cur->pib, t.pib_r, false)))
        return false;
    // Combined samplers (set 1): the same for every draw of the pass in this build.
    t.set1 = frame.descriptors(thread).allocate(std_layouts_.textures());
    if (!t.set1) return false;
    rhi::TextureBinding tb[rhi::kTextureSlots];
    uint32_t ntb = 0;
    const uint32_t aniso = aniso_cap();
    for (const shaderxlat::SamplerDesc& sd : tp.ps.iface.samplers) {
        if (sd.slot >= rhi::kTextureSlots || ntb >= rhi::kTextureSlots) continue;
        const GpuPass::XSlot* xs = nullptr;
        for (const GpuPass::XSlot& x : p.xslots)
            if (x.slot == sd.slot) xs = &x;
        VkImageView view_handle = white_.view;  // D3D9 default for a missing 2D texture: white
        if (sd.dim == shaderxlat::TextureDim::Cube) view_handle = white_cube_.view;
        else if (sd.dim == shaderxlat::TextureDim::Volume) view_handle = white_3d_.view;
        else if (cloud_view_ && (!xs || xs->tex < 0) && is_cloud_slot(rp, sd.slot))
            view_handle = cloud_view_;  // engine-bound CLOUD_TEXTURE (the world's clouds)
        else if (fow_view_ && (!xs || xs->tex < 0) && is_fow_slot(rp, sd.slot))
            view_handle = fow_view_;    // engine-bound FOW_TEXTURE (white while the fog of war is off)
        else if (xs && xs->tex >= 0 && static_cast<size_t>(xs->tex) < gm.textures.size() &&
                 gm.textures[static_cast<size_t>(xs->tex)].resident && gm.textures[static_cast<size_t>(xs->tex)].image)
            view_handle = gm.textures[static_cast<size_t>(xs->tex)].image.view;
        const materials::SamplerKey sk = xs ? xs->key : materials::SamplerKey{};
        tb[ntb++] = {sd.slot, view_handle, samplers_.get(to_rhi_sampler(sk, aniso, aniso_)), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    }
    if (ntb) rhi::write_textures(*ctx_, t.set1, std::span<const rhi::TextureBinding>(tb, ntb));
    if (t.vs_static && t.ps_static && (!ib || !t.ib_draw)) {  // nothing per draw: one set 0 for the pass
        t.set0 = frame.descriptors(thread).allocate(std_layouts_.constants(ib, inst));
        if (!t.set0) return false;
        if (inst)
            rhi::write_constants_storage(*ctx_, t.set0, t.vs_r.buffer, t.vs_r.offset, sizeof(materials::VSConstants), t.ps_r.buffer,
                                         t.ps_r.offset, sizeof(materials::PSConstants));
        else
            rhi::write_constants(*ctx_, t.set0, t.vs_r.buffer, t.vs_r.offset, sizeof(materials::VSConstants), t.ps_r.buffer, t.ps_r.offset,
                                 sizeof(materials::PSConstants));
        if (ib) rhi::write_int_bool_constants(*ctx_, t.set0, t.vib_r.buffer, t.vib_r.offset, t.pib_r.buffer, t.pib_r.offset);
    }
    shaderxlat::TranslatedPush push = rp.translated_push;
    // Scene fog (D3DRS_FOGENABLE from the scene, vertex fog from FOG_VALS) unless the pass sets FogEnable = FALSE;
    // same state as the hot-set path (Built::fog).
    push.flags &= ~(shaderxlat::kPushFog | (3u << shaderxlat::kFogTableShift));
    if (fog && !p.fog_off) {
        push.flags |= shaderxlat::kPushFog;
        push.fog_color[0] = lighting_.fog_color.x;
        push.fog_color[1] = lighting_.fog_color.y;
        push.fog_color[2] = lighting_.fog_color.z;
    }
    std::copy(pixel_offset, pixel_offset + 4, push.pixel_offset);
    t.push = push;
    t.ok = true;
    return true;
}

namespace {
// Items i and j (j = i + 1) form one instanced draw: everything but their DrawData (and, translated, their per-draw
// register images) equal, not in a back-to-front bucket (SPACE_AND_OPEN.md D.1: only the sorted buckets keep strict
// per-object order by depth; merging there is not done even though the order would be kept).
bool same_draw(const Renderer::Built::Item& a, const Renderer::Built::Item& b, const std::vector<Renderer::Built::Group>& groups) {
    if (a.pipeline != b.pipeline || a.bucket != b.bucket || a.pass != b.pass || a.fading != b.fading) return false;
    if (a.bucket == PhaseBucket::Transparent || a.bucket == PhaseBucket::Heat) return false;
    // Q3: translated items of the instanced form share one call when they use the same program and register-image
    // template (their per-draw images are consecutive in the build's storage block); int / bool constants (per-draw
    // uniform images) are never merged.
    if (a.tp || b.tp) {
        if (a.tp != b.tp || a.xt != b.xt || !a.tp->vs.iface.instanced || a.tp->vs_int_bool || a.tp->ps_int_bool) return false;
    }
    return groups[a.group].g == groups[b.group].g;
}
}  // namespace

bool Renderer::build(rhi::Frame& frame, const ViewMatrices& view, std::span<const Instance> instances, DrawMode mode,
                     const RecordOptions& opts, Built& out, DrawStats& st, bool main_list) {
    using Group = Built::Group;
    using Item = Built::Item;
    using clock = std::chrono::steady_clock;
    using PassMemo = BuildCache::PassMemo;
    using SubMemo = BuildCache::SubMemo;
    using ModelMemo = BuildCache::ModelMemo;
    const auto t0 = clock::now();
    const bool par = opts.threads > 1 && desc_.js != nullptr;
    const uint32_t n_inst = static_cast<uint32_t>(instances.size());
    out.set0 = out.set0_shadow = VK_NULL_HANDLE;  // `out` is a reused list
    if (!bc_) bc_ = std::make_unique<BuildCache>();
    BuildCache& bc = *bc_;
    if (++bc.stamp == 0) {  // wrapped: no stale stamp may equal a new one
        for (BuildCache::MemoSet& s : bc.sets)
            for (ModelMemo& m : s.memos) {
                m.stamp = 0;
                for (PassMemo& pm : m.passes)
                    if (pm.xt) pm.xt->stamp = 0;
            }
        bc.stamp = 1;
    }
    const uint32_t stamp = bc.stamp;

    // ---- Per-model memo (persistent, serial, first-appearance order). Instances share models, so PipelineManager
    // (mutex) and the translation table are touched only while a model still waits for a pipeline / program.
    const int cfg = (mode == DrawMode::Lit ? 1 : 0) | (external_ ? 2 : 0) | (opts.mirrored ? 4 : 0) | (sky_build_ ? 8 : 0) |
                    (opts.coverage_alpha ? 16 : 0) | (!external_ && render_pass_ != rp_scene_ ? 32 : 0);  // [port] MSAA class
    BuildCache::MemoSet& ms = bc.sets[cfg];
    if (ms.render_pass != render_pass_) {  // colour format changed: every key differs
        ms.index.clear();
        ms.memos.clear();
        ms.last_models.clear();
        ms.ranks_valid = false;
        ms.render_pass = render_pass_;
    }
    ms.models.clear();
    bc.inst_model.assign(n_inst, UINT32_MAX);
    bool memo_rebuilt = false;
    for (uint32_t i = 0; i < n_inst; ++i) {
        const GpuModel* m = instances[i].model;
        if (!m || !m->buffers_ready || !m->asset) continue;
        bool inserted = false;
        const uint32_t slot = ms.index.find_or_insert(m, static_cast<uint32_t>(ms.memos.size()), inserted);
        if (inserted) ms.memos.emplace_back();
        bc.inst_model[i] = slot;
        ModelMemo& mm = ms.memos[slot];
        if (mm.stamp == stamp) continue;
        mm.stamp = stamp;
        ms.models.push_back(slot);
        if (mm.built && mm.uid == m->uid && m->uid != 0 && mm.model == m) continue;
        // (Re)build: the model was (re-)uploaded since (new uid) or was never seen.
        memo_rebuilt = true;
        mm.model = m;
        mm.uid = m->uid;
        mm.built = true;
        mm.resolved = false;
        mm.subs.assign(m->submeshes.size(), SubMemo{});
        mm.passes.clear();
        for (size_t si = 0; si < m->submeshes.size(); ++si) {
            const GpuSubmesh& g = m->submeshes[si];
            mm.subs[si].first_pass = static_cast<uint32_t>(mm.passes.size());
            if (mode != DrawMode::Lit || g.lit != LitStatus::Lit) continue;
            for (const GpuPass& p : g.passes) {
                PassMemo& pm = mm.passes.emplace_back();
                if (p.translated) {
                    pm.xt = std::make_unique<XTemplate>();
                    pm.xt->model = m;
                    pm.xt->g = &g;
                    pm.xt->pass = &p;
                }
            }
        }
    }
    // Resolution of the lit passes (both LIGHT_SCALE variants): until every wanted pipeline and translated program
    // exists, re-queried every build (same requests and results as a fresh per-build memo).
    for (uint32_t slot : ms.models) {
        ModelMemo& mm = ms.memos[slot];
        if (mm.resolved) continue;
        const GpuModel& m = *mm.model;
        bool done = true;
        for (size_t si = 0; si < m.submeshes.size(); ++si) {
            const GpuSubmesh& g = m.submeshes[si];
            SubMemo& sm = mm.subs[si];
            sm.lit = sm.translating = sm.no_translation = false;
            if (mode != DrawMode::Lit || g.lit != LitStatus::Lit) continue;
            sm.lit = true;
            for (size_t pi = 0; pi < g.passes.size(); ++pi) {
                const GpuPass& p = g.passes[pi];
                PassMemo& pm = mm.passes[sm.first_pass + pi];
                // Both variants: select_exact() picks one of the pair per draw. The fading one is only
                // requested when the pass can fade, so no pipeline is compiled that is never used.
                const bool can_fade = p.pass->fade_dynamic || p.pass->fade_expr;
                pm.tp = nullptr;
                if (p.translated) {
                    // Track B: translation job first (one per program pair), then the pipeline.
                    // Q3: mesh passes use the instanced form (storage-buffer register images, instanced runs).
                    pm.tp = translated_mesh_program(g.effect, *p.pass);
                    bool unusable = !dxc_ok_;
                    if (!pm.tp && !unusable) {
                        std::lock_guard<std::mutex> lock(xlat_mutex_);
                        auto it = xlat_.find(xlat_key_of(*p.pass, true));
                        unusable = it != xlat_.end() && it->second->state.load() == 2;
                    }
                    if (unusable) {
                        sm.lit = false;
                        sm.no_translation = true;
                    } else if (!pm.tp) {
                        sm.translating = true;
                        done = false;
                    }
                    if (pm.xt) pm.xt->tp = pm.tp;
                }
                for (int v = 0; v < 2; ++v) {
                    if (p.translated && !pm.tp) {
                        pm.pipe[v] = VK_NULL_HANDLE;
                        pm.hash[v] = 0;
                        continue;
                    }
                    materials::PsoKey k = p.translated ? translated_key(g, p, *pm.tp, v) : draw_key_variant(g, p, v);
                    if (opts.mirrored) k.cull = mirrored_cull(k.cull);
                    if (opts.coverage_alpha) coverage_alpha_key(k);
                    pm.hash[v] = materials::hash(k);
                    const bool wanted = (v == 0 || can_fade) && sm.lit;
                    pm.pipe[v] = wanted ? psos_.get(k) : VK_NULL_HANDLE;
                    if (wanted && !pm.pipe[v]) done = false;
                }
            }
        }
        mm.resolved = done;
    }
    // Effect ranks and material-hash ranks of the build's models, both order-preserving, so the packed 64-bit
    // sort key below orders exactly like (effect, pass, material hash). Effect order = SPACE_AND_OPEN.md D.1: the
    // original's global effect list in first-creation order (GpuSubmesh::effect_order), effects outside the list
    // after it by name. Pure function of the model set + installed shader files (deterministic); recomputed only
    // when the set of models changes.
    if (memo_rebuilt || !ms.ranks_valid || ms.models != ms.last_models) {
        auto effect_less = [](const GpuSubmesh* a, const GpuSubmesh* b) {
            return a->effect_order != b->effect_order ? a->effect_order < b->effect_order : a->effect_name < b->effect_name;
        };
        std::vector<const GpuSubmesh*>& effect_names = bc.effect_names;
        effect_names.clear();
        rank_scratch_.clear();
        uint32_t max_passes = 0;
        for (uint32_t slot : ms.models)
            for (const GpuSubmesh& g : ms.memos[slot].model->submeshes) {
                if (!g.effect_name.empty()) effect_names.push_back(&g);
                rank_scratch_.push_back(g.material_hash);
                max_passes = std::max(max_passes, static_cast<uint32_t>(g.passes.size()));
            }
        std::sort(effect_names.begin(), effect_names.end(), effect_less);
        effect_names.erase(std::unique(effect_names.begin(), effect_names.end(),
                                       [](const GpuSubmesh* a, const GpuSubmesh* b) {
                                           return a->effect_order == b->effect_order && a->effect_name == b->effect_name;
                                       }),
                           effect_names.end());
        std::sort(rank_scratch_.begin(), rank_scratch_.end());
        rank_scratch_.erase(std::unique(rank_scratch_.begin(), rank_scratch_.end()), rank_scratch_.end());
        const std::vector<uint64_t>& mat_hashes = rank_scratch_;
        for (uint32_t slot : ms.models) {
            ModelMemo& mm = ms.memos[slot];
            for (size_t si = 0; si < mm.model->submeshes.size(); ++si) {
                const GpuSubmesh& g = mm.model->submeshes[si];
                SubMemo& sm = mm.subs[si];
                sm.rank = g.effect_name.empty()
                              ? 0u
                              : 1u + static_cast<uint32_t>(std::lower_bound(effect_names.begin(), effect_names.end(), &g, effect_less) -
                                                           effect_names.begin());
                sm.mat_rank = static_cast<uint32_t>(std::lower_bound(mat_hashes.begin(), mat_hashes.end(), g.material_hash) -
                                                    mat_hashes.begin());
            }
        }
        ms.effect_count = static_cast<uint32_t>(effect_names.size());
        ms.mat_count = static_cast<uint32_t>(mat_hashes.size());
        ms.max_passes = max_passes;
        ms.last_models = ms.models;
        ms.ranks_valid = true;
    }

    // ---- A1 (parallel, per-index outputs): cull, LOD, counts.
    using InstInfo = BuildCache::InstInfo;
    std::vector<InstInfo>& info = bc.info;
    info.assign(n_inst, InstInfo{});
    // Per-draw expression inputs (LIGHT_SCALE, TIME, VIEWINVERSE as a row-major D3DXMATRIX, FOW, team colour).
    float view_inv16[16];
    for (int r = 0; r < 4; ++r)
        for (int cc = 0; cc < 4; ++cc) view_inv16[r * 4 + cc] = view.view_inv.m[cc][r];
    auto expr_inputs = [&](const Instance& in) {
        materials::ExprInputs e;
        e.light_scale = in.light_scale;
        e.time = time_;
        e.view_inv = view_inv16;
        e.fow_tex_u = &fow_u_;
        e.fow_tex_v = &fow_v_;
        if (cloud_view_) {  // land world attached: FF texture transforms read the live cloud matrices
            e.cloud_tex_u = &cloud_u_;
            e.cloud_tex_v = &cloud_v_;
        }
        e.colorization = in.team_color_override ? &in.team_color : nullptr;
        return e;
    };
    // All chosen pipelines of a lit submesh ready? (variants chosen per pass with select_exact; bit pi of
    // `variants` = variant of pass pi, kept for the first 30 passes).
    auto lit_ready = [&](const GpuSubmesh& g, const ModelMemo& mm, const SubMemo& sm, const materials::ExprInputs& e,
                         uint32_t& variants) {
        variants = 0;
        for (size_t pi = 0; pi < g.passes.size(); ++pi) {
            const int v = pass_variant(g.passes[pi], e);
            if (!mm.passes[sm.first_pass + pi].pipe[v]) return false;
            if (v && pi < 30) variants |= 1u << pi;
        }
        return true;
    };
    // Per (instance, submesh) result of A1 for A2 (no second visibility / readiness / variant evaluation):
    // kSubGroup = drawn as a group, kSubLit = lit with every pipeline ready, low 30 bits = pass variants.
    constexpr uint32_t kSubGroup = 1u << 31, kSubLit = 1u << 30;
    std::vector<uint32_t>& sub_off = bc.sub_off;
    sub_off.resize(n_inst);
    uint32_t n_sub = 0;
    for (uint32_t i = 0; i < n_inst; ++i) {
        sub_off[i] = n_sub;
        if (bc.inst_model[i] != UINT32_MAX) n_sub += static_cast<uint32_t>(instances[i].model->submeshes.size());
    }
    std::vector<uint32_t>& sub_state = bc.sub_state;
    sub_state.assign(n_sub, 0);
    auto submesh_drawn = [&](const Instance& in, const GpuSubmesh& g, int lod) {
        const std::span<const uint8_t> hidden = in.mesh_hidden.empty() ? std::span<const uint8_t>(in.model->mesh_hidden) : in.mesh_hidden;
        if (g.mesh < hidden.size() && hidden[g.mesh]) return false;
        if (g.alt >= 0 && g.alt != in.alt) return false;  // R4: other damage alternates
        if (in.no_shadow && g.effect && g.bucket == PhaseBucket::Shadow) return false;  // R5 stealth: shadow bit cleared
        return g.vertices && g.indices && (g.lod < 0 || g.lod == lod);
    };
    // Shadow-volume and Heat passes have no unlit stand-in, in any draw mode: while their pipelines compile
    // (Lit) or always (Unlit) they are skipped. An unlit draw would put them into Opaque / Transparent (a
    // shadow volume drawn white through the hull); Heat is never remapped, SPACE_AND_OPEN.md B.6.
    auto no_fallback = [&](const GpuSubmesh& g) {
        return g.effect && (g.bucket == PhaseBucket::Heat || g.bucket == PhaseBucket::Shadow);
    };
    const std::vector<uint32_t>& inst_model = bc.inst_model;
    const Frustum view_frustum = frustum_of(view.view_proj);
    for_range(desc_.js, par, n_inst, 32, [&](uint32_t b, uint32_t e) {
        for (uint32_t ii = b; ii < e; ++ii) {
            if (inst_model[ii] == UINT32_MAX) continue;
            const Instance& in = instances[ii];
            InstInfo& o = info[ii];
            ++o.st.instances;
            const GpuModel& gm = *in.model;
            const assets::Aabb box = gm.bounds.valid() ? assets::transform_aabb(in.transform, gm.bounds) : assets::Aabb{};
            if (opts.frustum_cull && box.valid() && !aabb_visible(view_frustum, box)) {
                ++o.st.culled;
                continue;
            }
            // Culling frustum far plane = Object_Fade_End (RENDER.md 3.4 / camera +0x3c).
            if (opts.frustum_cull && fade_end_ > 0.0f && box.valid() && beyond_depth(view.view, box, fade_end_)) {
                ++o.st.culled;
                continue;
            }
            int lod = gm.max_lod;
            if (opts.dynamic_lod && gm.max_lod > 0 && box.valid())
                lod = choose_lod(gm.max_lod, lod_detail(in.lod_bias, screen_coverage(view.view_proj.m, box), opts.lod_detail));
            o.lod = lod;
            const ModelMemo& mm = ms.memos[inst_model[ii]];
            const materials::ExprInputs ein = expr_inputs(in);
            for (size_t si = 0; si < gm.submeshes.size(); ++si) {
                const GpuSubmesh& g = gm.submeshes[si];
                if (!submesh_drawn(in, g, lod)) continue;
                if (mode == DrawMode::Lit) {
                    if (g.lit == LitStatus::NotDrawn) {
                        ++o.st.not_drawn;
                        continue;
                    }
                    if (g.lit == LitStatus::NoEffect) ++o.st.no_effect;
                    if (g.lit == LitStatus::Untranslated) ++o.st.untranslated;
                    if (g.lit == LitStatus::LayoutMismatch) ++o.st.layout_mismatch;
                }
                const bool lit = mm.subs[si].lit;
                uint32_t variants = 0;
                const bool ready = lit && lit_ready(g, mm, mm.subs[si], ein, variants);
                if (mode == DrawMode::Lit && mm.subs[si].no_translation) ++o.st.no_translation;
                if (lit && !ready) ++(mm.subs[si].translating ? o.st.translation_pending : o.st.pending_pipeline);  // never stall
                if (!ready && no_fallback(g)) continue;
                sub_state[sub_off[ii] + si] = kSubGroup | (ready ? kSubLit | variants : 0u);
                ++o.groups;
                if (g.skinned) o.bones += static_cast<uint32_t>(std::max<size_t>(in.model->asset->submeshes[g.submesh].bone_palette.size(), 1));
                o.items += ready ? static_cast<uint32_t>(g.passes.size()) : 1u;
            }
        }
    });
    // ---- Serial prefix sums (deterministic placement in instance order).
    std::vector<uint32_t>& goff = bc.goff;
    std::vector<uint32_t>& ioff = bc.ioff;
    std::vector<uint32_t>& boff = bc.boff;
    goff.resize(n_inst);
    ioff.resize(n_inst);
    boff.resize(n_inst);
    uint32_t ng = 0, ni = 0, bones = 0;
    for (uint32_t i = 0; i < n_inst; ++i) {
        goff[i] = ng;
        ioff[i] = ni;
        boff[i] = bones;
        ng += info[i].groups;
        ni += info[i].items;
        bones += info[i].bones;
        const BuildCache::InstStats& s = info[i].st;
        st.instances += s.instances;
        st.culled += s.culled;
        st.not_drawn += s.not_drawn;
        st.no_effect += s.no_effect;
        st.untranslated += s.untranslated;
        st.layout_mismatch += s.layout_mismatch;
        st.pending_pipeline += s.pending_pipeline;
        st.translation_pending += s.translation_pending;
        st.no_translation += s.no_translation;
    }
    out.groups.resize(ng);
    out.items.resize(ni);
    // ---- A2 (parallel): groups and items written at their offsets.
    for_range(desc_.js, par, n_inst, 32, [&](uint32_t b, uint32_t e) {
        for (uint32_t ii = b; ii < e; ++ii) {
            const InstInfo& o = info[ii];
            if (o.lod == INT32_MIN || !o.groups) continue;
            const Instance& in = instances[ii];
            const GpuModel& gm = *in.model;
            const assets::ModelAsset& ma = *gm.asset;
            const ModelMemo& mm = ms.memos[inst_model[ii]];
            const materials::ExprInputs ein = expr_inputs(in);
            const std::span<const Mat3x4> bw = in.bone_world.empty() ? std::span<const Mat3x4>(ma.skeleton.bind_world) : in.bone_world;
            uint32_t gi = goff[ii], it = ioff[ii], bb = boff[ii];
            for (size_t si = 0; si < gm.submeshes.size(); ++si) {
                const GpuSubmesh& g = gm.submeshes[si];
                const uint32_t state = sub_state[sub_off[ii] + si];
                if (!(state & kSubGroup)) continue;  // A1: not drawn, NotDrawn, or no fallback while compiling
                const bool lit = (state & kSubLit) != 0;
                const assets::Submesh& sm = ma.submeshes[g.submesh];
                Group& gr = out.groups[gi];
                gr = Group{&in, ii, &g, {}, 0, 0, 0.0f, mm.subs[si].rank, mm.subs[si].mat_rank};
                gr.world = g.skinned ? in.transform : static_mesh_world(in, ma, bw, sm.attach_bone, view, lighting_.sun.direction);
                if (g.skinned) {
                    gr.bone_base = bb;
                    gr.bone_count = static_cast<uint32_t>(std::max<size_t>(sm.bone_palette.size(), 1));
                    bb += gr.bone_count;
                }
                const Float3 c{0.5f * (sm.bounds.min.x + sm.bounds.max.x), 0.5f * (sm.bounds.min.y + sm.bounds.max.y),
                               0.5f * (sm.bounds.min.z + sm.bounds.max.z)};
                const Float3 wc = gr.world.transform_point(c);
                const float dx = wc.x - view.eye.x, dy = wc.y - view.eye.y, dz = wc.z - view.eye.z;
                gr.depth_key = dx * dx + dy * dy + dz * dz;
                if (lit) {
                    for (size_t pi = 0; pi < g.passes.size(); ++pi) {
                        const PassMemo& pm = mm.passes[mm.subs[si].first_pass + pi];
                        const int v = pi < 30 ? static_cast<int>((state >> pi) & 1u) : pass_variant(g.passes[pi], ein);
                        out.items[it++] = {gi, static_cast<int>(pi), pm.pipe[v], g.bucket, v == 1, pm.hash[v], pm.tp,
                                           pm.tp ? pm.xt.get() : nullptr};
                    }
                } else {
                    const BlendMode bm = g.unlit_blend;
                    out.items[it++] = {gi, -1, (external_ ? unlit_ : render_pass_ != rp_scene_ ? unlit_ms_ : unlit_int_)[g.skinned ? 1 : 0][static_cast<int>(bm)],
                                       bm == BlendMode::Opaque ? PhaseBucket::Opaque : PhaseBucket::Transparent, false,
                                       static_cast<uint64_t>((g.skinned ? 3 : 0) + static_cast<int>(bm)), nullptr, nullptr};
                }
                ++gi;
            }
        }
    });
    const auto t1 = clock::now();

    // ---- C: RENDER.md 1.4 order with integer keys: buckets in enum order; Transparent/Heat back-to-front;
    // the others by effect (D.1 creation-order rank), pass, material. A stable sort over the instance-ordered list keeps
    // ties in instance order, so the result is identical for any thread count.
    // Packed form: one 64-bit key per item (bucket 4 bits | depth-writing 1 | effect rank | pass + 1 | material
    // rank; Transparent / Heat: bucket | inverted depth bits), sorted by a stable LSD radix sort that skips
    // the bytes no key differs in. The ranks are order-preserving, so the order equals the (k1, material hash)
    // comparison sort, which stays as the fallback when the fields do not fit 59 bits.
    const auto& groups = out.groups;
    {
        auto bits_for = [](uint64_t v) {
            uint32_t b = 0;
            while (v) {
                ++b;
                v >>= 1;
            }
            return b;
        };
        // Unlit stand-ins (no standard effect task) after every effect batch of their bucket (D.1 step 4): rank
        // size + 1, past the effect ranks 1..size.
        const uint64_t unlit_rank = static_cast<uint64_t>(ms.effect_count) + 1u;
        const uint32_t be = bits_for(unlit_rank), bp = bits_for(ms.max_passes), bm = bits_for(ms.mat_count);
        const bool packed = be + bp + bm <= 59u;
        const uint32_t sp = bm, se = bm + bp;  // shifts of the pass and effect-rank fields
        std::vector<SortKey>& keys = sort_keys_;
        keys.resize(out.items.size());
        for_range(desc_.js, par, static_cast<uint32_t>(keys.size()), 1024, [&](uint32_t b, uint32_t e) {
            for (uint32_t i = b; i < e; ++i) {
                const Item& itm = out.items[i];
                const Group& gr = groups[itm.group];
                const bool back_to_front = itm.bucket == PhaseBucket::Transparent || itm.bucket == PhaseBucket::Heat;
                uint32_t bits = 0;
                if (back_to_front) std::memcpy(&bits, &gr.depth_key, 4);  // non-negative float: bit order == value order
                // Depth-writing translated PS (kPsoWritesDepth) after the others of the bucket: they
                // disable early-Z, so they are not interleaved with ordinary opaque batches.
                const uint64_t rank = itm.pass < 0 ? unlit_rank : gr.rank;
                const uint64_t wd = itm.tp && itm.tp->writes_depth ? 1u : 0u;
                const uint64_t pass1 = static_cast<uint32_t>(itm.pass + 1);
                if (packed) {
                    const uint64_t bucket = static_cast<uint64_t>(itm.bucket) << 60;
                    keys[i] = {back_to_front ? bucket | (0xFFFFFFFFull - bits)
                                             : bucket | (wd << 59) | (rank << se) | (pass1 << sp) | gr.mat_rank,
                               0, i};
                } else {
                    const uint64_t bucket = static_cast<uint64_t>(itm.bucket) << 56;
                    keys[i] = back_to_front ? SortKey{bucket | (0xFFFFFFFFull - bits), 0, i}
                                            : SortKey{bucket | (wd << 52) | (rank << 32) | pass1, gr.g->material_hash, i};
                }
            }
        });
        if (packed) {
            uint64_t all_or = 0, all_and = ~0ull;
            for (const SortKey& k : keys) {
                all_or |= k.k1;
                all_and &= k.k1;
            }
            const uint64_t varying = all_or ^ all_and;
            sort_tmp_.resize(keys.size());
            for (uint32_t shift = 0; shift < 64; shift += 8) {
                if (((varying >> shift) & 0xFFu) == 0) continue;
                uint32_t count[257] = {};
                for (const SortKey& k : keys) ++count[((k.k1 >> shift) & 0xFFu) + 1];
                for (int d = 0; d < 256; ++d) count[d + 1] += count[d];
                for (const SortKey& k : keys) sort_tmp_[count[(k.k1 >> shift) & 0xFFu]++] = k;
                keys.swap(sort_tmp_);
            }
        } else {
            std::stable_sort(keys.begin(), keys.end(), [](const SortKey& a, const SortKey& b) {
                return a.k1 != b.k1 ? a.k1 < b.k1 : a.k2 < b.k2;
            });
        }
        out.items_tmp.resize(keys.size());
        for (size_t i = 0; i < keys.size(); ++i) out.items_tmp[i] = out.items[keys[i].index];
        out.items.swap(out.items_tmp);
    }
    const auto& items = out.items;
    // Instanced runs (serial, from the sorted list only: identical for any thread count).
    out.run_end.resize(items.size());
    for (size_t i = items.size(); i-- > 0;)
        out.run_end[i] = opts.instancing && i + 1 < items.size() && same_draw(items[i], items[i + 1], groups)
                             ? out.run_end[i + 1]
                             : static_cast<uint32_t>(i + 1);
    const auto t2 = clock::now();

    // ---- D: records straight into the frame ring (lock-free bump allocations). Per item in parallel;
    // materials interned serially in item order afterwards, so MaterialTable indices match any thread count.
    auto slot_of = [](const GpuModel& m, int ti) -> uint32_t {
        if (ti < 0 || static_cast<size_t>(ti) >= m.textures.size()) return materials::kNoTexture;
        const GpuTexture& t = m.textures[static_cast<size_t>(ti)];
        return t.resident ? t.slot : materials::kNoTexture;
    };
    out.fog = lighting_.fog_enabled;
    const size_t n = items.size();
    const rhi::UniformAlloc da = frame.alloc_storage(std::max<size_t>(n, 1) * sizeof(materials::DrawData));
    const rhi::UniformAlloc ba = frame.alloc_storage(std::max<uint32_t>(bones, 1) * sizeof(materials::BoneMatrix));
    if (!da || !ba) {
        log::errorf("render", "frame uniform ring exhausted (%zu draws, %u bones)", n, bones);
        return false;
    }
    auto* draws = static_cast<materials::DrawData*>(da.ptr);
    std::vector<materials::MeshMaterial>& mats = bc.mats;
    std::vector<uint32_t>& ff_slot = bc.ff_slot;
    std::vector<uint64_t>& mat_hash = bc.mat_hash;  // hashed here in parallel, interned serially below
    mats.resize(n);
    ff_slot.assign(n, UINT32_MAX);
    mat_hash.assign(n, 0);
    uint32_t nff = 0;  // FF records get their index in item order up front
    for (size_t i = 0; i < n; ++i)
        if (items[i].pass >= 0 && groups[items[i].group].g->passes[static_cast<size_t>(items[i].pass)].ff) ff_slot[i] = nff++;
    std::vector<materials::FfDrawData>& ff_tmp = bc.ff_tmp;
    ff_tmp.resize(nff);
    out.xset0.assign(n, VK_NULL_HANDLE);
    out.xset1.assign(n, VK_NULL_HANDLE);
    out.xpush.assign(n, shaderxlat::TranslatedPush{});
    std::atomic<bool> xfail{false};
    float pixel_offset[4];
    translated_pixel_offset(view.width, view.height, pixel_offset);
    // Translated passes: their register-image templates (built once per frame inputs) and the per-build shared
    // uniforms / sets, one prep per distinct pass in first-use order.
    const uint32_t nslots = desc_.js ? desc_.js->thread_count() : 1u;
    if (bc.palettes.size() < nslots) bc.palettes.resize(nslots);
    {  // every slot can take the largest palette of the build (which worker gets which group varies per frame)
        size_t max_pal = 0;
        for (const Group& gr : groups)
            if (gr.g->skinned)
                max_pal = std::max<size_t>(max_pal, std::max<size_t>(gr.bone_count, gr.inst->model->asset->submeshes[gr.g->submesh].bone_palette.size()));
        for (std::vector<Mat3x4>& pal : bc.palettes) pal.reserve(max_pal);
    }
    bc.xts.clear();
    for (size_t i = 0; i < n; ++i) {
        XTemplate* xt = items[i].tp ? items[i].xt : nullptr;
        if (xt && xt->stamp != stamp) {
            xt->stamp = stamp;
            bc.xts.push_back(xt);
        }
    }
    EngineFrame ef{};
    if (!bc.xts.empty()) {
        engine_frame(view, scene_lights(), ef);  // SH once per build (cached per light set)
        for_range(desc_.js, par, static_cast<uint32_t>(bc.xts.size()), 4, [&](uint32_t b, uint32_t e) {
            const uint32_t thread = par && desc_.js ? desc_.js->current_slot() : 0u;
            for (uint32_t k = b; k < e; ++k)
                if (!xt_prepare(*bc.xts[k], frame, thread, ef, pixel_offset, out.fog)) xfail = true;
        });
    }
    // Q3: per-draw register images of the instanced translated form - one storage block per build, slots in item order
    // (each instanced run's images consecutive: instance k of a run reads image slot(head) + k).
    std::vector<uint32_t>& xslot_vs = bc.xslot_vs;
    std::vector<uint32_t>& xslot_ps = bc.xslot_ps;
    xslot_vs.assign(n, UINT32_MAX);
    xslot_ps.assign(n, UINT32_MAX);
    uint32_t nxvs = 0, nxps = 0;
    for (size_t i = 0; i < n && !xfail; ++i) {
        const Item& it = items[i];
        if (!it.tp || !it.tp->vs.iface.instanced || !it.xt || !it.xt->ok || it.xt->set0) continue;
        if (!it.xt->vs_static) xslot_vs[i] = nxvs++;
        if (!it.xt->ps_static) xslot_ps[i] = nxps++;
    }
    rhi::UniformAlloc xvs_block, xps_block;
    if (nxvs) xvs_block = frame.alloc_storage(static_cast<size_t>(nxvs) * sizeof(materials::VSConstants));
    if (nxps) xps_block = frame.alloc_storage(static_cast<size_t>(nxps) * sizeof(materials::PSConstants));
    if ((nxvs && !xvs_block) || (nxps && !xps_block)) xfail = true;
    for_range(desc_.js, par, static_cast<uint32_t>(n), 256, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            const Item& it = items[i];
            const Group& gr = groups[it.group];
            const GpuSubmesh& g = *gr.g;
            const GpuModel& gm = *gr.inst->model;
            materials::DrawData d{};
            store_rows(gr.world, d.world);
            d.light_scale = gr.inst->light_scale;
            d.material_index = 0;  // patched after interning (FF: final now)
            d.bone_base = gr.bone_base;
            d.flags = 0;
            d.pad = 0;
            if (it.pass < 0) {
                materials::MeshMaterial m{};
                m.diffuse = {1, 1, 1, 1};
                m.tex.x = materials::pack_texture(slot_of(gm, g.base_texture), materials::kSamplerWrapLinear);
                mats[i] = m;
            } else {
                const GpuPass& p = g.passes[static_cast<size_t>(it.pass)];
                uint32_t packed[4];
                for (int s = 0; s < 4; ++s) {
                    const uint32_t ts = (p.cloud_slots & (1u << s)) ? cloud_slot_ : slot_of(gm, p.tex[s]);
                    packed[s] = materials::pack_texture(ts, p.sampler[s]);
                }
                if (p.ff) {
                    materials::FfDrawData f = p.ff_data;
                    // Per-draw FF state (const, allocation-free, thread-safe), then the instance's hook.
                    if (p.ff_tpl || gr.inst->ff_hook) {
                        const materials::ExprInputs ein = expr_inputs(*gr.inst);
                        if (p.ff_tpl) p.ff_tpl->update(ein, f);
                        if (gr.inst->ff_hook && *gr.inst->ff_hook) (*gr.inst->ff_hook)(g, static_cast<size_t>(it.pass), ein, f);
                    }
                    f.tex.x = packed[0];
                    f.tex.y = packed[1];
                    f.tex.z = packed[2];
                    ff_tmp[ff_slot[i]] = f;
                    d.material_index = ff_slot[i];
                } else {
                    materials::MeshMaterial m = g.material;
                    if (gr.inst->team_color_override) m.colorization = gr.inst->team_color;
                    m.tex = {packed[0], packed[1], packed[2], packed[3]};
                    mats[i] = m;
                }
            }
            if (ff_slot[i] == UINT32_MAX) mat_hash[i] = alamo::fnv1a64(&mats[i], sizeof(materials::MeshMaterial));
            std::memcpy(&draws[i], &d, sizeof d);
            if (it.tp) {
                // Track B per draw: only the per-draw registers are written (the pass's template carries the
                // material / frame part); images without per-draw parameters, the combined samplers and the base
                // push block were prepared once for the pass above. Descriptor sets come from the executing
                // thread's own allocator (Frame per-thread pools).
                XTemplate* xt = it.xt;
                if (!xt || !xt->ok || !xt->cur) {
                    xfail = true;
                    continue;
                }
                const GpuPass& p = g.passes[static_cast<size_t>(it.pass)];
                const materials::TranslatedProgram& tp = *it.tp;
                const bool ib = tp.vs_int_bool || tp.ps_int_bool;
                const uint32_t thread = par && desc_.js ? desc_.js->current_slot() : 0u;
                VkDescriptorSet s0 = xt->set0;
                shaderxlat::TranslatedPush push = xt->push;
                if (!s0 && tp.vs.iface.instanced) {
                    // Q3 instanced form: this item's per-draw images in the build's storage block; the run head's set 0
                    // covers the whole run (stride 256 / 32 float4 when the image is per draw, else the shared image).
                    const bool vs_draw = !xt->vs_static, ps_draw = !xt->ps_static, ib_draw = ib && xt->ib_draw;
                    auto* vsp = vs_draw ? reinterpret_cast<materials::VSConstants*>(static_cast<uint8_t*>(xvs_block.ptr) +
                                                                                   static_cast<size_t>(xslot_vs[i]) * sizeof(materials::VSConstants))
                                        : nullptr;
                    auto* psp = ps_draw ? reinterpret_cast<materials::PSConstants*>(static_cast<uint8_t*>(xps_block.ptr) +
                                                                                   static_cast<size_t>(xslot_ps[i]) * sizeof(materials::PSConstants))
                                        : nullptr;
                    rhi::BufferRange vir = xt->vib_r, pir = xt->pib_r;
                    auto alloc_ub = [&](size_t bytes, rhi::BufferRange& r) -> void* {
                        const rhi::UniformAlloc a = frame.alloc_uniform(bytes);
                        if (!a) return nullptr;
                        r = {a.buffer, a.offset, bytes};
                        return a.ptr;
                    };
                    void* vip = ib_draw ? alloc_ub(sizeof(materials::IntBoolConstants), vir) : nullptr;
                    void* pip = ib_draw ? alloc_ub(sizeof(materials::IntBoolConstants), pir) : nullptr;
                    if ((vs_draw && xslot_vs[i] == UINT32_MAX) || (ps_draw && xslot_ps[i] == UINT32_MAX) || (ib_draw && (!vip || !pip))) {
                        xfail = true;
                        continue;
                    }
                    EngineParams ep;
                    translated_draw_params(*gr.inst, g, view, ef, bc.palettes[thread], ep);
                    const DrawLookupCtx ctx{&ep, g.params.get(), gr.inst, g.effect->effect.get(), xt->src.data()};
                    const materials::ParamLookup lookup = draw_lookup(&ctx);
                    xt_images(*xt, *xt->cur, *g.effect->effect, *p.pass, lookup, vsp, psp, static_cast<materials::IntBoolConstants*>(vip),
                              static_cast<materials::IntBoolConstants*>(pip));
                    const bool head = i == 0 || out.run_end[i - 1] != out.run_end[i];
                    if (head) {
                        const size_t run = out.run_end[i] - i;
                        s0 = frame.descriptors(thread).allocate(std_layouts_.constants(ib, true));
                        if (!s0) {
                            xfail = true;
                            continue;
                        }
                        const VkDeviceSize vs_off = vs_draw ? xvs_block.offset + static_cast<VkDeviceSize>(xslot_vs[i]) * sizeof(materials::VSConstants)
                                                            : xt->vs_r.offset;
                        const VkDeviceSize ps_off = ps_draw ? xps_block.offset + static_cast<VkDeviceSize>(xslot_ps[i]) * sizeof(materials::PSConstants)
                                                            : xt->ps_r.offset;
                        rhi::write_constants_storage(*ctx_, s0, vs_draw ? xvs_block.buffer : xt->vs_r.buffer, vs_off,
                                                     static_cast<VkDeviceSize>(vs_draw ? run : 1) * sizeof(materials::VSConstants),
                                                     ps_draw ? xps_block.buffer : xt->ps_r.buffer, ps_off,
                                                     static_cast<VkDeviceSize>(ps_draw ? run : 1) * sizeof(materials::PSConstants));
                        if (ib) rhi::write_int_bool_constants(*ctx_, s0, vir.buffer, vir.offset, pir.buffer, pir.offset);
                    }
                    push.flags |= (vs_draw ? shaderxlat::kPushInstancedVs : 0u) | (ps_draw ? shaderxlat::kPushInstancedPs : 0u);
                } else if (!s0) {
                    const bool vs_draw = !xt->vs_static, ps_draw = !xt->ps_static, ib_draw = ib && xt->ib_draw;
                    // Per-draw images go straight into the frame ring (write-only: template copy + patched registers).
                    rhi::BufferRange vr = xt->vs_r, pr = xt->ps_r, vir = xt->vib_r, pir = xt->pib_r;
                    auto alloc = [&](size_t bytes, rhi::BufferRange& r) -> void* {
                        const rhi::UniformAlloc a = frame.alloc_uniform(bytes);
                        if (!a) return nullptr;
                        r = {a.buffer, a.offset, bytes};
                        return a.ptr;
                    };
                    void* vsp = vs_draw ? alloc(sizeof(materials::VSConstants), vr) : nullptr;
                    void* psp = ps_draw ? alloc(sizeof(materials::PSConstants), pr) : nullptr;
                    void* vip = ib_draw ? alloc(sizeof(materials::IntBoolConstants), vir) : nullptr;
                    void* pip = ib_draw ? alloc(sizeof(materials::IntBoolConstants), pir) : nullptr;
                    s0 = frame.descriptors(thread).allocate(std_layouts_.constants(ib));
                    if (!s0 || (vs_draw && !vsp) || (ps_draw && !psp) || (ib_draw && (!vip || !pip))) {
                        xfail = true;
                        continue;
                    }
                    EngineParams ep;
                    translated_draw_params(*gr.inst, g, view, ef, bc.palettes[thread], ep);
                    const DrawLookupCtx ctx{&ep, g.params.get(), gr.inst, g.effect->effect.get(), xt->src.data()};
                    const materials::ParamLookup lookup = draw_lookup(&ctx);
                    xt_images(*xt, *xt->cur, *g.effect->effect, *p.pass, lookup, static_cast<materials::VSConstants*>(vsp),
                              static_cast<materials::PSConstants*>(psp), static_cast<materials::IntBoolConstants*>(vip),
                              static_cast<materials::IntBoolConstants*>(pip));
                    rhi::write_constants(*ctx_, s0, vr.buffer, vr.offset, sizeof(materials::VSConstants), pr.buffer, pr.offset,
                                         sizeof(materials::PSConstants));
                    if (ib) rhi::write_int_bool_constants(*ctx_, s0, vir.buffer, vir.offset, pir.buffer, pir.offset);
                }
                push.draw_index = static_cast<uint32_t>(i);
                out.xset0[i] = s0;
                out.xset1[i] = xt->set1;
                out.xpush[i] = push;
            }
        }
    });
    if (xfail) {
        log::errorf("render", "frame ring / descriptor pools exhausted for translated draws");
        return false;
    }
    // Never read back from the mapped ring (write-combined / ReBAR memory): indices stay in a CPU array.
    // Interning stays serial in item order (indices identical for any thread count), but only distinct
    // materials reach the table: an item equal to the previous one (the sort groups by material) reuses
    // its index, the rest go through an open-addressing hash -> record table (hashes from the parallel loop
    // above, collisions resolved by content), so the indices equal MaterialTable::intern() per item.
    std::vector<materials::MeshMaterial>& records = bc.records;
    std::vector<uint32_t>& mat_index = bc.mat_index;
    records.clear();
    mat_index.resize(n);
    {
        size_t cap = 64;
        while (cap < 2 * n) cap *= 2;
        if (bc.intern.size() < cap) {
            bc.intern.assign(cap, BuildCache::InternSlot{});
            bc.intern_stamp = 0;
        }
        if (++bc.intern_stamp == 0) {
            std::fill(bc.intern.begin(), bc.intern.end(), BuildCache::InternSlot{});
            bc.intern_stamp = 1;
        }
    }
    const uint32_t istamp = bc.intern_stamp;
    const size_t imask = bc.intern.size() - 1;
    size_t prev = SIZE_MAX;
    for (size_t i = 0; i < n; ++i) {
        if (ff_slot[i] != UINT32_MAX) {
            mat_index[i] = ff_slot[i];
            continue;
        }
        uint32_t mi = 0;
        if (prev != SIZE_MAX && mat_hash[i] == mat_hash[prev] &&
            std::memcmp(&mats[i], &mats[prev], sizeof(materials::MeshMaterial)) == 0) {
            mi = mat_index[prev];
        } else {
            size_t j = static_cast<size_t>(mat_hash[i]) & imask;
            bool found = false;
            while (bc.intern[j].stamp == istamp) {
                if (std::memcmp(&records[bc.intern[j].index], &mats[i], sizeof(materials::MeshMaterial)) == 0) {
                    mi = bc.intern[j].index;
                    found = true;
                    break;
                }
                j = (j + 1) & imask;
            }
            if (!found) {
                mi = static_cast<uint32_t>(records.size());
                records.push_back(mats[i]);
                bc.intern[j] = {istamp, mi};
            }
        }
        mat_index[i] = mi;
        prev = i;
    }
    for_range(desc_.js, par, static_cast<uint32_t>(n), 1024, [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i)
            if (ff_slot[i] == UINT32_MAX)
                std::memcpy(reinterpret_cast<uint8_t*>(&draws[i]) + offsetof(materials::DrawData, material_index), &mat_index[i], 4);
    });
    st.materials = static_cast<uint32_t>(records.size());

    auto* bone_data = static_cast<materials::BoneMatrix*>(ba.ptr);
    for_range(desc_.js, par, static_cast<uint32_t>(groups.size()), 64, [&](uint32_t b, uint32_t e) {
        std::vector<Mat3x4>& palette = bc.palettes[par && desc_.js ? desc_.js->current_slot() : 0u];
        for (uint32_t gi = b; gi < e; ++gi) {
            const Group& gr = groups[gi];
            if (!gr.g->skinned) continue;
            const assets::ModelAsset& ma = *gr.inst->model->asset;
            const assets::Submesh& sm = ma.submeshes[gr.g->submesh];
            const std::span<const Mat3x4> bw =
                gr.inst->bone_world.empty() ? std::span<const Mat3x4>(ma.skeleton.bind_world) : gr.inst->bone_world;
            palette.assign(gr.bone_count, Mat3x4{});
            if (bw.size() == ma.skeleton.size() && !sm.bone_palette.empty())
                assets::compute_skin_palette(ma.skeleton, bw, sm.bone_palette, palette);
            for (uint32_t k = 0; k < gr.bone_count; ++k) {
                materials::BoneMatrix bm;
                store_rows(gr.world * palette[k], bm.rows);
                std::memcpy(&bone_data[gr.bone_base + k], &bm, sizeof bm);
            }
        }
    });

    if (main_list) {  // only the main pass (sky / reflection / refraction builds keep it)
        last_list_.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const Group& gr = groups[items[i].group];
            last_list_[i] = {gr.inst_index, gr.g->submesh, items[i].pass, items[i].key_hash, mat_index[i]};
        }
    }

    auto storage = [&](const void* src, size_t bytes, rhi::BufferRange& r) {
        const rhi::UniformAlloc a = frame.alloc_storage(std::max<size_t>(bytes, 16));
        if (!a) return false;
        if (bytes) std::memcpy(a.ptr, src, bytes);
        r = {a.buffer, a.offset, std::max<VkDeviceSize>(bytes, 16)};
        return true;
    };
    rhi::BufferRange mats_r, ff_r;
    const materials::MeshMaterial empty_mat{};
    const materials::FfDrawData empty_ff{};
    if (!storage(records.empty() ? static_cast<const void*>(&empty_mat) : records.data(),
                 std::max<size_t>(records.size(), 1) * sizeof(materials::MeshMaterial), mats_r) ||
        !storage(ff_tmp.empty() ? static_cast<const void*>(&empty_ff) : ff_tmp.data(),
                 std::max<size_t>(ff_tmp.size(), 1) * sizeof(materials::FfDrawData), ff_r)) {
        log::errorf("render", "frame uniform ring exhausted (%zu draws, %u bones)", n, bones);
        return false;
    }
    const rhi::BufferRange draws_r{da.buffer, da.offset, std::max<size_t>(n, 1) * sizeof(materials::DrawData)};
    const rhi::BufferRange bones_r{ba.buffer, ba.offset, std::max<uint32_t>(bones, 1) * sizeof(materials::BoneMatrix)};
    materials::FrameCB fcb;
    frame_constants(view, kFcbScene, fcb);
    rhi::BufferRange frame_cb;
    if (!upload_frame_cb(frame, fcb, frame_cb)) return false;
    out.set0 = frame.descriptors(0).allocate(hot_.buffers());
    if (!out.set0) return false;
    const rhi::BufferRange sto[rhi::kHotStorageBuffers] = {draws_r, mats_r, bones_r, ff_r};
    rhi::write_hot_buffers(*ctx_, out.set0, frame_cb, sto);
    // Shadow bucket: camera z-bias 0.001 (near and far both moved by -0.001 near, RENDER.md 3.3).
    bool any_shadow = false;
    for (const Item& itm : items) any_shadow = any_shadow || itm.bucket == PhaseBucket::Shadow;
    if (any_shadow) {
        const float m22 = view.proj.m[2][2], m23 = view.proj.m[2][3];
        // zn / zf back from the 0..1 RH projection: m22 = zf/(zn-zf), m23 = zn*zf/(zn-zf).
        const float zn = m23 / m22, zf = m23 / (m22 + 1.0f);
        const float bias = 0.001f * zn;
        ViewMatrices sv = view;
        const float n2 = zn - bias, f2 = zf - bias;
        sv.proj.m[2][2] = f2 / (n2 - f2);
        sv.proj.m[2][3] = n2 * f2 / (n2 - f2);
        sv.view_proj = sv.proj * sv.view;
        materials::FrameCB scb;
        frame_constants(sv, kFcbScene, scb);
        rhi::BufferRange shadow_cb;
        if (!upload_frame_cb(frame, scb, shadow_cb)) return false;
        out.set0_shadow = frame.descriptors(0).allocate(hot_.buffers());
        if (!out.set0_shadow) return false;
        rhi::write_hot_buffers(*ctx_, out.set0_shadow, shadow_cb, sto);
    }
    const auto t3 = clock::now();
    out.cull_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    out.sort_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
    out.fill_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
    return true;
}

void Renderer::record_range(VkCommandBuffer cmd, const Built& b, size_t begin, size_t end, uint32_t width, uint32_t height,
                            bool set_viewport) const {
    const rhi::DeviceFns& fn = ctx_->vkd();
    if (set_viewport) {
        // State does not inherit into secondaries: same Y-flipped viewport as begin_pass.
        VkViewport vp{};
        vp.y = static_cast<float>(height);
        vp.width = static_cast<float>(width);
        vp.height = -static_cast<float>(height);
        vp.maxDepth = 1.0f;
        fn.vkCmdSetViewport(cmd, 0, 1, &vp);
        const VkRect2D sc{{0, 0}, {width, height}};
        fn.vkCmdSetScissor(cmd, 0, 1, &sc);
    }
    const VkFormat depth_vk = external_ || ds_format_ == VK_FORMAT_UNDEFINED ? desc_.depth_format : ds_format_;
    const rhi::Format depth_fmt = rhi::from_vk(depth_vk);
    VkPipeline bound = VK_NULL_HANDLE;
    const materials::DynamicState* dyn_bound = nullptr;
    static const materials::DynamicState kDefaultDyn{};
    VkDescriptorSet hot_bound = VK_NULL_HANDLE;  // set 0 of the hot interface currently bound (none after a translated draw)
    for (size_t i = begin; i < end;) {
        const Built::Item& it = b.items[i];
        const GpuSubmesh& g = *b.groups[it.group].g;
        if (it.pipeline != bound) {
            fn.vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, it.pipeline);
            bound = it.pipeline;
        }
        if (it.tp) {
            // Track B: StandardLayouts interface, per-draw sets, 64-byte push block, mesh + zero buffer.
            const VkPipelineLayout tl = translated_layout(*it.tp);
            const VkDescriptorSet xs[2] = {b.xset0[i], b.xset1[i]};
            fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tl, 0, 2, xs, 0, nullptr);
            hot_bound = VK_NULL_HANDLE;
            const materials::DynamicState* dyn = &g.passes[static_cast<size_t>(it.pass)].pass->dynamic;
            if (dyn != dyn_bound) {
                cmd_set_dynamic_state(fn, cmd, *dyn, depth_fmt);
                dyn_bound = dyn;
            }
            const VkBuffer vbs[2] = {g.vertices.handle, zero_vb_.handle};
            const VkDeviceSize offs[2] = {0, 0};
            fn.vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            fn.vkCmdBindIndexBuffer(cmd, g.indices.handle, 0, g.index_type);
            fn.vkCmdPushConstants(cmd, tl, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                  sizeof(shaderxlat::TranslatedPush), &b.xpush[i]);
            // Q3: an instanced run of the instanced form (its head's set 0 covers the run's register images).
            const size_t run = std::min<size_t>(i < b.run_end.size() ? b.run_end[i] : i + 1, end);
            const uint32_t count = static_cast<uint32_t>(std::max<size_t>(run, i + 1) - i);
            fn.vkCmdDrawIndexed(cmd, g.index_count, count, 0, 0, 0);
            i += count;
            continue;
        }
        // Shadow volumes use the z-biased frame constants (RENDER.md 3.3).
        const VkDescriptorSet want = it.bucket == PhaseBucket::Shadow && b.set0_shadow ? b.set0_shadow : b.set0;
        if (want != hot_bound) {
            const VkDescriptorSet sets[2] = {want, bindless_};
            fn.vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 2, sets, 0, nullptr);
            hot_bound = want;
        }
        const materials::DynamicState* dyn = it.pass < 0 ? &kDefaultDyn : &g.passes[static_cast<size_t>(it.pass)].pass->dynamic;
        if (dyn != dyn_bound) {
            cmd_set_dynamic_state(fn, cmd, *dyn, depth_fmt);
            dyn_bound = dyn;
        }
        const VkDeviceSize zero = 0;
        fn.vkCmdBindVertexBuffers(cmd, 0, 1, &g.vertices.handle, &zero);
        fn.vkCmdBindIndexBuffer(cmd, g.indices.handle, 0, g.index_type);
        materials::PushConstants pc;
        bool fog_off = g.unlit_fog_off;
        if (it.pass >= 0) {
            const GpuPass& gp = g.passes[static_cast<size_t>(it.pass)];
            pc = gp.pass->push;
            fog_off = gp.fog_off;
            if (it.fading) pc.flags |= materials::kPushFading;
        } else {  // unlit stand-in: the material's alpha test
            pc.alpha_func = g.unlit_alpha_func;
            pc.alpha_ref = g.unlit_alpha_ref;
        }
        // Scene fog (D3DRS_FOGENABLE per scene) unless the pass sets FogEnable = FALSE (RENDER.md 2.4).
        pc.flags &= ~materials::kPushFog;
        if (b.fog && !fog_off) pc.flags |= materials::kPushFog;
        // Instanced run: items i .. run_end - 1 (clamped to the range) in one call; instance k reads
        // DrawData[draw_index + k] (common.hlsli), so each keeps its own transform / material / bones / fade.
        const size_t run = std::min<size_t>(i < b.run_end.size() ? b.run_end[i] : i + 1, end);
        const uint32_t count = static_cast<uint32_t>(std::max<size_t>(run, i + 1) - i);
        pc.draw_index = static_cast<uint32_t>(i);
        fn.vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        fn.vkCmdDrawIndexed(cmd, g.index_count, count, 0, 0, 0);
        i += count;
    }
}

namespace detail {
void count_items(const Renderer::Built& bl, size_t b, size_t e, DrawStats& st) {
    for (size_t i = b; i < e; ++i) {
        const Renderer::Built::Item& it = bl.items[i];
        const GpuSubmesh& g = *bl.groups[it.group].g;
        ++st.draws;
        st.triangles += g.index_count / 3;
        if (it.pass < 0) ++st.unlit_draws;
        else if (it.tp) ++st.translated_draws;
        else if (g.passes[static_cast<size_t>(it.pass)].ff) ++st.ff_draws;
        else ++st.lit_draws;
        if (g.skinned) ++st.skinned_draws;
        if (it.bucket == PhaseBucket::Transparent || it.bucket == PhaseBucket::Heat) ++st.transparent_draws;
        if (it.bucket == PhaseBucket::Shadow) ++st.shadow_volumes;
        if (it.bucket == PhaseBucket::Heat) ++st.heat_draws;
    }
    for (size_t i = b; i < e;) {  // calls as record_range issues them for [b, e)
        const size_t run = std::max<size_t>(std::min<size_t>(i < bl.run_end.size() ? bl.run_end[i] : i + 1, e), i + 1);
        ++st.draw_calls;
        if (run - i > 1) ++st.instanced_calls;
        if (run - i > 1 && bl.items[i].tp) ++st.translated_instanced_calls;
        i = run;
    }
}
// First item index whose bucket is >= b (items are sorted by bucket).
size_t bucket_begin(const std::vector<Renderer::Built::Item>& items, PhaseBucket b) {
    size_t i = 0;
    while (i < items.size() && items[i].bucket < b) ++i;
    return i;
}
}  // namespace detail

bool Renderer::draw(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, std::span<const Instance> instances,
                    DrawMode mode, DrawStats* stats) {
    return draw(frame, cmd, view, instances, mode, RecordOptions{}, stats);
}

bool Renderer::draw(rhi::Frame& frame, VkCommandBuffer cmd, const ViewMatrices& view, std::span<const Instance> instances,
                    DrawMode mode, const RecordOptions& opts, DrawStats* stats) {
    // Inline single pass into the caller's attachments: no stencil, no heat target, so the Shadow and
    // Heat buckets are skipped (render() draws them).
    if (ring_) collect_released();
    latch_anisotropy(frame);
    // Same world constants as render(): never views / slots of a world that was shut down.
    if (world_ && mode == DrawMode::Lit) world_frame_constants();
    else reset_world_constants();
    DrawStats st;
    Built& b = built(kBuiltDraw);
    external_ = true;
    render_pass_ = rp_external_;
    const bool ok = build(frame, view, instances, mode, opts, b, st);
    external_ = false;
    render_pass_ = rp_main_;
    if (!ok) return false;
    const size_t s0 = bucket_begin(b.items, PhaseBucket::Shadow), s1 = bucket_begin(b.items, PhaseBucket::Water);
    const size_t h0 = bucket_begin(b.items, PhaseBucket::Heat);
    record_range(cmd, b, 0, s0, view.width, view.height, true);
    record_range(cmd, b, s1, h0, view.width, view.height, false);
    count_items(b, 0, s0, st);
    count_items(b, s1, h0, st);
    st.not_drawn += static_cast<uint32_t>((s1 - s0) + (b.items.size() - h0));
    if (stats) *stats = st;
    return true;
}

}  // namespace alamo::render

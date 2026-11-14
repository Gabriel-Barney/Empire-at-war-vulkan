// Renderer: translated (DXC) programs, engine parameters and register images (split from renderer.cpp).
#include "alamo/render/renderer.hpp"
#include <algorithm>
#include <cassert>
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

// ------------------------------------------------------------------------------------ track B

namespace detail {
uint64_t xlat_key(uint32_t vs_id, uint32_t ps_id) { return (static_cast<uint64_t>(vs_id) << 32) | ps_id; }
// Translation table key of a pass in the default (cbuffer) or the Q3 instanced form.
uint64_t xlat_key_of(const materials::ResolvedPass& pass, bool instanced) {
    if (!instanced) return xlat_key(pass.pso[0].vs_id, pass.pso[0].ps_id);
    shaderxlat::TranslateOptions vo = pass.vs_options, po = pass.ps_options;
    vo.instanced = po.instanced = true;
    return xlat_key(materials::translated_program_id(pass.vs_bytecode, vo), materials::translated_program_id(pass.ps_bytecode, po));
}
}  // namespace detail

const materials::TranslatedProgram* Renderer::find_translated(uint32_t vs_id, uint32_t ps_id) const {
    std::lock_guard<std::mutex> lock(xlat_mutex_);
    auto it = xlat_.find(xlat_key(vs_id, ps_id));
    return it != xlat_.end() && it->second->state.load() == 1 ? it->second->tp.get() : nullptr;
}

const materials::TranslatedProgram* Renderer::translated_program(const std::shared_ptr<const materials::MaterialEffect>& effect,
                                                                 const materials::ResolvedPass& pass) {
    return translated_program_impl(effect, pass, false);
}

const materials::TranslatedProgram* Renderer::translated_mesh_program(const std::shared_ptr<const materials::MaterialEffect>& effect,
                                                                      const materials::ResolvedPass& pass) {
    return translated_program_impl(effect, pass, true);
}

const materials::TranslatedProgram* Renderer::translated_program_impl(const std::shared_ptr<const materials::MaterialEffect>& effect,
                                                                      const materials::ResolvedPass& pass, bool instanced) {
    if (!dxc_ok_ || pass.program_status != materials::ProgramStatus::Translated) return nullptr;
    // Q3 instanced form: its own program ids (the options key differs), registered next to the default form's so
    // the pipeline manager's find_translated resolves both.
    std::shared_ptr<materials::ResolvedPass> inst_pass;
    const uint64_t key = xlat_key_of(pass, instanced);
    XlatEntry* e = nullptr;
    {
        std::lock_guard<std::mutex> lock(xlat_mutex_);
        auto& slot = xlat_[key];
        if (slot) return slot->state.load() == 1 ? slot->tp.get() : nullptr;
        slot = std::make_unique<XlatEntry>();
        e = slot.get();
        if (effect) {
            e->effect = effect->name;
            e->path = effect->path;
        }
    }
    if (instanced) {  // the translation inputs of the pass with the instanced options (kept alive by the job)
        inst_pass = std::make_shared<materials::ResolvedPass>();
        inst_pass->name = pass.name;
        inst_pass->program_status = pass.program_status;
        inst_pass->pso[0] = pass.pso[0];
        inst_pass->vs_bytecode = pass.vs_bytecode;
        inst_pass->ps_bytecode = pass.ps_bytecode;
        inst_pass->vs_options = pass.vs_options;
        inst_pass->ps_options = pass.ps_options;
        inst_pass->vs_options.instanced = inst_pass->ps_options.instanced = true;
    }
    // One job per program pair; the effect is kept alive by the capture (the pass lives in it).
    auto job = [this, e, effect, inst_pass, p = inst_pass ? inst_pass.get() : &pass] {
        (void)inst_pass;  // keeps the instanced inputs alive while the job runs
        const auto t0 = std::chrono::steady_clock::now();
        std::string err;
        std::shared_ptr<const materials::TranslatedProgram> tp = materials::translate_pass(*p, *dxc_, scache_.get(), &err);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        std::lock_guard<std::mutex> lock(xlat_mutex_);
        e->tp = std::move(tp);
        e->error = err;
        e->ms = ms;
        e->state.store(e->tp ? 1 : 2);
        if (!e->tp) log::warnf("render", "translate %s: %s", effect ? effect->name.c_str() : "?", err.c_str());
    };
    // Serial job system: the result is only polled, so nobody would run the job (see PipelineManager::request).
    if (desc_.js && !desc_.js->serial()) desc_.js->spawn(*xlat_counter_, std::move(job), jobs::Priority::Background);
    else job();
    std::lock_guard<std::mutex> lock(xlat_mutex_);
    return e->state.load() == 1 ? e->tp.get() : nullptr;
}

void Renderer::wait_translations() {
    if (desc_.js && xlat_counter_) desc_.js->wait(*xlat_counter_);
}

Renderer::TranslationStats Renderer::translation_stats() const {
    TranslationStats s;
    std::lock_guard<std::mutex> lock(xlat_mutex_);
    for (const auto& [k, e] : xlat_) {
        ++s.programs;
        const int st = e->state.load();
        if (st == 0) ++s.pending;
        else if (st == 2) ++s.failed;
        else {
            ++s.ready;
            if (e->tp->vs.from_cache && e->tp->ps.from_cache) ++s.cache_hits;
        }
        s.translate_ms += e->ms;
    }
    return s;
}

std::vector<materials::ShaderIssue> Renderer::shader_issues() const {
    std::vector<materials::ShaderIssue> out;
    if (desc_.effects) out = desc_.effects->issues();
    std::vector<materials::ShaderIssue> failed;
    {
        std::lock_guard<std::mutex> lock(xlat_mutex_);
        for (const auto& [k, e] : xlat_)
            if (e->state.load() == 2) failed.push_back({materials::ShaderIssueKind::TranslateFailed, e->effect, e->path, false, e->error});
    }
    // xlat_ is hashed: order by content, drop the instanced / cbuffer twins of one failure.
    auto less = [](const materials::ShaderIssue& x, const materials::ShaderIssue& y) {
        if (x.effect != y.effect) return x.effect < y.effect;
        if (x.kind != y.kind) return x.kind < y.kind;
        return x.detail < y.detail;
    };
    std::sort(failed.begin(), failed.end(), less);
    failed.erase(std::unique(failed.begin(), failed.end(),
                             [](const materials::ShaderIssue& x, const materials::ShaderIssue& y) {
                                 return x.effect == y.effect && x.detail == y.detail;
                             }),
                 failed.end());
    out.insert(out.end(), failed.begin(), failed.end());
    std::stable_sort(out.begin(), out.end(), less);
    return out;
}

materials::PsoKey Renderer::translated_key(const GpuSubmesh& g, const GpuPass& p, const materials::TranslatedProgram& tp,
                                           int variant) const {
    materials::PsoKey k = tp.key(p.pass->pso[variant ? 1 : 0]);
    k.render_pass = render_pass_;
    k.vertex_layout = g.layout;  // inputs are matched against the mesh layout
    return k;
}

void Renderer::translated_pixel_offset(uint32_t width, uint32_t height, float (&out)[4]) {
    // D3D9 rasterises with pixel centres at integer coordinates, Vulkan at +0.5: shifting the clip
    // position by +1/W (x) and -1/H (y; NDC y is up and the viewport flips it) moves the geometry half
    // a pixel right / down on screen, which reproduces D3D9's coverage and interpolation.
    out[0] = 1.0f / static_cast<float>(width ? width : 1);
    out[1] = -1.0f / static_cast<float>(height ? height : 1);
    out[2] = out[3] = 0.0f;
}

namespace detail {

// DISTANCE_FADE_VALS in the FOG_VALS form from the camera's fade begin / end; the off default (-2e-5, 3) when
// either is <= -1 or the pair is empty (RENDER.md 2.2).
void distance_fade_vals(float begin, float end, float& x, float& y) {
    if (begin <= -1.0f || end <= -1.0f || end <= 0.0f || end == begin) {
        x = -2e-5f;
        y = 3.0f;
        return;
    }
    x = 1.0f / (begin - end);
    y = -end / (begin - end);
}

void to_d3dx(const Mat4& m, float (&out)[16]) {
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) out[r * 4 + c] = m.m[c][r];
}

Mat4 general_inverse(const Mat4& a) {  // affine inverse is enough for every engine matrix here
    return a.inverse_affine();
}

void make_engine_draw(const Mat3x4& world3, const Instance& inst, const ViewMatrices& v, const EngineFrame& f, EngineParams& e) {
    e.f = &f;
    const Mat4 world = Mat4::from(world3);
    const Mat4 world_view = v.view * world;
    const Mat4 world_inv = general_inverse(world);
    Mat4 world_inv_t;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) world_inv_t.m[r][c] = world_inv.m[c][r];
    to_d3dx(world, e.world);
    to_d3dx(world_view, e.world_view);
    to_d3dx(v.view_proj * world, e.wvp);
    to_d3dx(world_inv, e.world_inv);
    to_d3dx(world_inv_t, e.world_inv_t);
    to_d3dx(general_inverse(world_view), e.world_view_inv);
    const Float4 eo = world_inv * Float4{v.eye.x, v.eye.y, v.eye.z, 1.0f};
    const float eye_obj[4] = {eo.x, eo.y, eo.z, 1.0f};
    std::copy(eye_obj, eye_obj + 4, e.eye_obj);
    const float ls[4] = {inst.light_scale.x, inst.light_scale.y, inst.light_scale.z, inst.light_scale.w};
    std::copy(ls, ls + 4, e.light_scale);
    const Float4 lo = world_inv * Float4{f.l0_vec[0], f.l0_vec[1], f.l0_vec[2], 0.0f};
    const float lol = std::sqrt(lo.x * lo.x + lo.y * lo.y + lo.z * lo.z);
    const float l0obj[4] = {lol > 0 ? lo.x / lol : 0.0f, lol > 0 ? lo.y / lol : 0.0f, lol > 0 ? lo.z / lol : 1.0f, 0.0f};
    std::copy(l0obj, l0obj + 4, e.l0_obj);
}

void write_matrix(const fx::Parameter& p, const float (&m)[16], std::span<float> out) {
    const uint32_t rows = std::max(1u, p.type.rows), cols = std::max(1u, p.type.columns);
    std::fill(out.begin(), out.end(), 0.0f);
    size_t k = 0;
    for (uint32_t r = 0; r < rows && r < 4; ++r)
        for (uint32_t c = 0; c < cols && c < 4 && k < out.size(); ++c) out[k++] = m[r * 4 + c];
}
void write_vec(const float (&v)[4], std::span<float> out) {
    std::fill(out.begin(), out.end(), 0.0f);
    std::copy_n(v, std::min<size_t>(4, out.size()), out.begin());
}

Src semantic_src(const std::string& s) {
    if (s.empty()) return Src::Store;
    if (ascii_iequals(s, "WORLD") || ascii_iequals(s, "MODEL")) return Src::World;
    if (ascii_iequals(s, "VIEW")) return Src::View;
    if (ascii_iequals(s, "PROJECTION")) return Src::Proj;
    if (ascii_iequals(s, "VIEWPROJECTION")) return Src::ViewProj;
    if (ascii_iequals(s, "WORLDVIEW") || ascii_iequals(s, "MODELVIEW")) return Src::WorldView;
    if (ascii_iequals(s, "WORLDVIEWPROJECTION") || ascii_iequals(s, "MODELVIEWPROJECTION")) return Src::Wvp;
    if (ascii_iequals(s, "VIEWINVERSE")) return Src::ViewInv;
    if (ascii_iequals(s, "WORLDINVERSE") || ascii_iequals(s, "MODELINVERSE")) return Src::WorldInv;
    if (ascii_iequals(s, "WORLDINVERSETRANSPOSE") || ascii_iequals(s, "MODELINVERSETRANSPOSE")) return Src::WorldInvT;
    if (ascii_iequals(s, "WORLDVIEWINVERSE")) return Src::WorldViewInv;
    if (ascii_iequals(s, "EYE_POSITION")) return Src::Eye;
    if (ascii_iequals(s, "EYE_OBJ_POSITION")) return Src::EyeObj;
    if (ascii_iequals(s, "LIGHT_SCALE")) return Src::LightScale;
    if (ascii_iequals(s, "TIME")) return Src::Time;
    if (ascii_iequals(s, "RESOLUTION_CONSTANTS")) return Src::Resolution;
    if (ascii_iequals(s, "FOG_VALS")) return Src::Fog;
    if (ascii_iequals(s, "DISTANCE_FADE_VALS")) return Src::Fade;
    if (ascii_iequals(s, "SPH_LIGHT_ALL")) return Src::SphAll;
    if (ascii_iequals(s, "SPH_LIGHT_FILL")) return Src::SphFill;
    if (ascii_iequals(s, "SHADOW_EXTRUSION_DISTANCE")) return Src::Extrusion;
    if (ascii_iequals(s, "CLOUD_TEX_U")) return Src::CloudU;
    if (ascii_iequals(s, "CLOUD_TEX_V")) return Src::CloudV;
    if (ascii_iequals(s, "FOW_TEX_U")) return Src::FowU;
    if (ascii_iequals(s, "FOW_TEX_V")) return Src::FowV;
    if (ascii_iequals(s, "WIND_BEND_VECTOR") || ascii_iequals(s, "WIND_GRASS_PARAMS")) return Src::Zero;
    return Src::Store;
}

Src param_src(const fx::Parameter& p) {
    const Src s = semantic_src(p.type.semantic);
    if (s != Src::Store) return s;
    const std::string& n = p.type.name;
    if (ascii_iequals(n, "SKINMATRIXARRAY")) return Src::Skin;
    if (ascii_iequals(n, "GLOBAL_AMBIENT")) return Src::Ambient;
    if (ascii_iequals(n, "DIR_LIGHT_VEC_0")) return Src::L0Vec;
    if (ascii_iequals(n, "DIR_LIGHT_DIFFUSE_0")) return Src::L0Dif;
    if (ascii_iequals(n, "DIR_LIGHT_SPECULAR_0")) return Src::L0Spec;
    if (ascii_iequals(n, "DIR_LIGHT_VIEW_VEC_0")) return Src::L0View;
    if (ascii_iequals(n, "DIR_LIGHT_OBJ_VEC_0")) return Src::L0Obj;
    return Src::Store;
}

// Parameters whose value depends on the instance (transform, LIGHT_SCALE, pose, team colour): rewritten per
// draw; everything else is per material or per frame and lives in the pass's cached register images.
bool draw_param(const fx::Parameter& p) {
    if (ascii_iequals(p.type.name, "COLORIZATION")) return true;  // team colour override (checked before the engine table)
    switch (param_src(p)) {
    case Src::World: case Src::WorldView: case Src::Wvp: case Src::WorldInv: case Src::WorldInvT: case Src::WorldViewInv:
    case Src::EyeObj: case Src::LightScale: case Src::Skin: case Src::L0Obj: return true;
    default: return false;
    }
}

bool engine_write(const EngineParams& e, const materials::ParamStore* store, const fx::Parameter& p, std::span<float> out, Src src) {
    const EngineFrame& f = *e.f;
    switch (src) {
    case Src::Store: return store && store->get(p, out);
    case Src::World: return write_matrix(p, e.world, out), true;
    case Src::View: return write_matrix(p, f.view, out), true;
    case Src::Proj: return write_matrix(p, f.proj, out), true;
    case Src::ViewProj: return write_matrix(p, f.view_proj, out), true;
    case Src::WorldView: return write_matrix(p, e.world_view, out), true;
    case Src::Wvp: return write_matrix(p, e.wvp, out), true;
    case Src::ViewInv: return write_matrix(p, f.view_inv, out), true;
    case Src::WorldInv: return write_matrix(p, e.world_inv, out), true;
    case Src::WorldInvT: return write_matrix(p, e.world_inv_t, out), true;
    case Src::WorldViewInv: return write_matrix(p, e.world_view_inv, out), true;
    case Src::Eye: return write_vec(f.eye, out), true;
    case Src::EyeObj: return write_vec(e.eye_obj, out), true;
    case Src::LightScale: return write_vec(e.light_scale, out), true;
    case Src::Time: return write_vec(f.time4, out), true;
    case Src::Resolution: return write_vec(f.resolution, out), true;
    case Src::Fog: return write_vec(f.fog, out), true;
    case Src::Fade: return write_vec(f.fade, out), true;
    case Src::SphAll:
    case Src::SphFill: {
        const materials::float4* m = src == Src::SphAll ? f.sph_all : f.sph_fill;
        std::fill(out.begin(), out.end(), 0.0f);
        // float4x4[3]; the irradiance matrices are symmetric, so row- and column-major agree.
        for (size_t r = 0; r < 12 && r * 4 + 3 < out.size(); ++r) {
            out[r * 4 + 0] = m[r].x;
            out[r * 4 + 1] = m[r].y;
            out[r * 4 + 2] = m[r].z;
            out[r * 4 + 3] = m[r].w;
        }
        return true;
    }
    case Src::Extrusion: {
        const float x[4] = {kShadowExtrusionDistance, kShadowExtrusionDistance, kShadowExtrusionDistance, kShadowExtrusionDistance};
        return write_vec(x, out), true;
    }
    case Src::CloudU: return write_vec(f.cloud_u, out), true;
    case Src::CloudV: return write_vec(f.cloud_v, out), true;
    case Src::FowU: return write_vec(f.fow_u, out), true;
    case Src::FowV: return write_vec(f.fow_v, out), true;
    case Src::Zero: return std::fill(out.begin(), out.end(), 0.0f), true;
    case Src::Skin: {
        // Rigid skinning palette (RENDER.md 4): world-space bone matrices, D3DX row-major per element.
        std::fill(out.begin(), out.end(), 0.0f);
        const uint32_t rows = std::max(1u, p.type.rows), cols = std::max(1u, p.type.columns);
        const size_t per = static_cast<size_t>(rows) * cols;
        for (size_t i = 0; i < e.palette.size() && (i + 1) * per <= out.size(); ++i) {
            float dx[16];
            to_d3dx(Mat4::from(e.palette[i]), dx);
            for (uint32_t r = 0; r < rows && r < 4; ++r)
                for (uint32_t cc = 0; cc < cols && cc < 4; ++cc) out[i * per + r * cols + cc] = dx[r * 4 + cc];
        }
        return true;
    }
    case Src::Ambient: return write_vec(f.ambient, out), true;
    case Src::L0Vec: return write_vec(f.l0_vec, out), true;
    case Src::L0Dif: return write_vec(f.l0_dif, out), true;
    case Src::L0Spec: return write_vec(f.l0_spec, out), true;
    case Src::L0View: return write_vec(f.l0_view, out), true;
    case Src::L0Obj: return write_vec(e.l0_obj, out), true;
    }
    return false;
}


uint8_t src_code(const fx::Parameter& p) {
    return static_cast<uint8_t>(static_cast<uint8_t>(param_src(p)) | (ascii_iequals(p.type.name, "COLORIZATION") ? kSrcColorization : 0));
}
materials::ParamLookup draw_lookup(const DrawLookupCtx* c) {
    return [c](const fx::Parameter& prm, std::span<float> out) {
        uint8_t code = 0;
        if (c->src && !c->fx->parameters.empty() && &prm >= c->fx->parameters.data() &&
            &prm < c->fx->parameters.data() + c->fx->parameters.size())
            code = c->src[static_cast<size_t>(&prm - c->fx->parameters.data())];
        else
            code = src_code(prm);
        if ((code & kSrcColorization) && c->inst && c->inst->team_color_override) {
            const float tc[4] = {c->inst->team_color.x, c->inst->team_color.y, c->inst->team_color.z, c->inst->team_color.w};
            write_vec(tc, out);
            return true;
        }
        return engine_write(*c->e, c->store, prm, out, static_cast<Src>(code & 0x7F));
    };
}

// World / palette of a translated draw (skinned effects: WORLD = identity and a world-space palette, RENDER.md 4)
// into e (palette storage in `pal`).
void translated_draw_params(const Instance& inst, const GpuSubmesh& g, const ViewMatrices& view, const EngineFrame& f,
                            std::vector<Mat3x4>& pal, EngineParams& e) {
    const assets::ModelAsset& ma = *inst.model->asset;
    const assets::Submesh& sm = ma.submeshes[g.submesh];
    const std::span<const Mat3x4> bw = inst.bone_world.empty() ? std::span<const Mat3x4>(ma.skeleton.bind_world) : inst.bone_world;
    const Mat3x4 world = g.skinned ? Mat3x4{} : static_mesh_world(inst, ma, bw, sm.attach_bone, view, {f.l0_vec[0], f.l0_vec[1], f.l0_vec[2]});
    pal.clear();
    if (g.skinned && !sm.bone_palette.empty()) {
        pal.assign(sm.bone_palette.size(), Mat3x4{});
        if (bw.size() == ma.skeleton.size()) assets::compute_skin_palette(ma.skeleton, bw, sm.bone_palette, pal);
        for (Mat3x4& m : pal) m = inst.transform * m;
    }
    make_engine_draw(world, inst, view, f, e);
    e.palette = pal;
}

// ---- Register images per (material, pass) (Renderer::XTemplate).
constexpr size_t kMaxPatchFloats = 1024;

// Which bindings / steps of the pass's fill read per-draw parameters. The patched image equals the full fill
// when (a) the per-draw bindings overlap no other binding (else: full fill per draw) and (b) the preshader
// and the bump-env registers, written after the CTAB bindings, are re-run when they read per-draw inputs or
// cover a per-draw binding.
void xt_analyse(Renderer::XTemplate& t, const fx::Effect& fxe, const materials::ResolvedPass& rp) {
    t.analysed = true;
    t.full = false;
    t.src.resize(fxe.parameters.size());
    for (size_t i = 0; i < fxe.parameters.size(); ++i) t.src[i] = src_code(fxe.parameters[i]);
    auto split = [&](const materials::RegisterFilePlan& plan, size_t nregs, std::vector<uint32_t>& draw, bool& pre) {
        draw.clear();
        pre = false;
        auto in_file = [&](const materials::RegisterBinding& b) { return b.parameter >= 0 && b.reg < nregs; };
        for (size_t i = 0; i < plan.floats.size(); ++i) {
            const materials::RegisterBinding& b = plan.floats[i];
            if (!in_file(b)) continue;
            const fx::Parameter& p = fxe.parameters[static_cast<size_t>(b.parameter)];
            if (!draw_param(p)) continue;
            if (materials::param_float_count(p) > kMaxPatchFloats) t.full = true;
            draw.push_back(static_cast<uint32_t>(i));
        }
        for (uint32_t d : draw) {
            const materials::RegisterBinding& a = plan.floats[d];
            for (size_t j = 0; j < plan.floats.size(); ++j) {
                const materials::RegisterBinding& b = plan.floats[j];
                if (j == d || !in_file(b)) continue;
                if (a.reg < b.reg + b.count && b.reg < a.reg + a.count) t.full = true;
            }
        }
        if (!plan.preshader) return;
        for (const materials::FxlcInput& in : plan.preshader->inputs) {
            const fx::Parameter* p = fxe.findParameter(in.name);
            if (p && draw_param(*p)) pre = true;
        }
        if (pre || draw.empty()) return;
        for (const materials::FxlcInstruction& ins : plan.preshader->code) {  // outputs (table 4) over a per-draw binding
            if (ins.output.table != 4) continue;
            const uint32_t n = ins.op == 0x500 ? 1u : std::max<uint32_t>(1, ins.comps);
            for (uint32_t c = 0; c < n; ++c) {
                const uint32_t reg = (ins.output.offset + c) / 4;
                for (uint32_t d : draw)
                    if (reg >= plan.floats[d].reg && reg < static_cast<uint32_t>(plan.floats[d].reg) + plan.floats[d].count) pre = true;
            }
        }
    };
    split(*rp.vs_plan, 256, t.vs_draw, t.vs_pre);
    split(*rp.ps_plan, 32, t.ps_draw, t.ps_pre);
    t.bump = rp.uses_bump_env;  // ps_1_x bump passes (none in vanilla): re-run per draw
    t.ib_draw = false;
    for (const materials::RegisterFilePlan* plan : {&*rp.vs_plan, &*rp.ps_plan})
        for (const auto* list : {&plan->ints, &plan->bools})
            for (const materials::RegisterBinding& b : *list)
                if (b.parameter >= 0 && draw_param(fxe.parameters[static_cast<size_t>(b.parameter)])) t.ib_draw = true;
    if (t.full) t.ib_draw = true;
    t.vs_static = !t.full && t.vs_draw.empty() && !t.vs_pre;
    t.ps_static = !t.full && t.ps_draw.empty() && !t.ps_pre && !t.bump;
    // Frame inputs of the per-material part (the slot key): sources of every binding, preshader input and int / bool
    // constant that is not per draw. A frame that changes only other inputs (e.g. TIME for a pass that does not
    // read it) reuses the images.
    t.frame_mask = 0;
    auto add = [&](const fx::Parameter& p) {
        if (!draw_param(p)) t.frame_mask |= 1ull << static_cast<unsigned>(param_src(p));
    };
    for (const materials::RegisterFilePlan* plan : {&*rp.vs_plan, &*rp.ps_plan}) {
        for (const auto* list : {&plan->floats, &plan->ints, &plan->bools})
            for (const materials::RegisterBinding& b : *list)
                if (b.parameter >= 0) add(fxe.parameters[static_cast<size_t>(b.parameter)]);
        if (plan->preshader)
            for (const materials::FxlcInput& in : plan->preshader->inputs)
                if (const fx::Parameter* p = fxe.findParameter(in.name)) add(*p);
    }
    if (t.bump || t.full) t.frame_mask = ~0ull;
}

// The fields of `f` the sources in `mask` read; the others zero.
void mask_frame(const EngineFrame& f, uint64_t mask, EngineFrame& out) {
    out = EngineFrame{};
    if (mask == ~0ull) {
        out = f;
        return;
    }
    auto has = [&](Src s) { return (mask >> static_cast<unsigned>(s)) & 1u; };
    auto copy16 = [](const float (&a)[16], float (&b)[16]) { std::copy(a, a + 16, b); };
    auto copy4 = [](const float (&a)[4], float (&b)[4]) { std::copy(a, a + 4, b); };
    if (has(Src::View)) copy16(f.view, out.view);
    if (has(Src::Proj)) copy16(f.proj, out.proj);
    if (has(Src::ViewProj)) copy16(f.view_proj, out.view_proj);
    if (has(Src::ViewInv)) copy16(f.view_inv, out.view_inv);
    if (has(Src::Eye)) copy4(f.eye, out.eye);
    if (has(Src::Time)) copy4(f.time4, out.time4);
    if (has(Src::Resolution)) copy4(f.resolution, out.resolution);
    if (has(Src::Fog)) copy4(f.fog, out.fog);
    if (has(Src::Fade)) copy4(f.fade, out.fade);
    if (has(Src::Ambient)) copy4(f.ambient, out.ambient);
    if (has(Src::L0Vec)) copy4(f.l0_vec, out.l0_vec);
    if (has(Src::L0Dif)) copy4(f.l0_dif, out.l0_dif);
    if (has(Src::L0Spec)) copy4(f.l0_spec, out.l0_spec);
    if (has(Src::L0View)) copy4(f.l0_view, out.l0_view);
    if (has(Src::CloudU)) copy4(f.cloud_u, out.cloud_u);
    if (has(Src::CloudV)) copy4(f.cloud_v, out.cloud_v);
    if (has(Src::FowU)) copy4(f.fow_u, out.fow_u);
    if (has(Src::FowV)) copy4(f.fow_v, out.fow_v);
    if (has(Src::SphAll)) std::copy(f.sph_all, f.sph_all + 12, out.sph_all);
    if (has(Src::SphFill)) std::copy(f.sph_fill, f.sph_fill + 12, out.sph_fill);
}

// The frame part of a template: every binding filled from the frame inputs and the material (per-draw values
// neutral: identity-free zero matrices, no team colour), exactly the full fill's sequence.
void xt_fill_slot(const Renderer::XTemplate& t, Renderer::XTemplate::Slot& s, const EngineFrame& masked, const fx::Effect& fxe,
                  const materials::ResolvedPass& rp, const materials::ParamStore* store, bool int_bool) {
    s.frame = masked;  // every field the per-material part reads (frame_mask)
    EngineParams e;
    e.f = &s.frame;
    const DrawLookupCtx ctx{&e, store, nullptr};
    const materials::ParamLookup lookup = draw_lookup(&ctx);
    s.vs = {};
    s.ps = {};
    materials::fill_register_file(fxe, *rp.vs_plan, lookup, s.vs.c);
    materials::fill_register_file(fxe, *rp.ps_plan, lookup, s.ps.c);
    materials::fill_bump_env(rp, fxe, lookup, s.ps.c);
    s.vib = {};
    s.pib = {};
    if (int_bool) {
        materials::fill_int_bool_constants(fxe, *rp.vs_plan, lookup, s.vib);
        materials::fill_int_bool_constants(fxe, *rp.ps_plan, lookup, s.pib);
    }
    const std::function<bool(const fx::Parameter&)> per_draw = &draw_param;
    if (t.vs_pre) materials::bake_preshader(fxe, *rp.vs_plan, lookup, per_draw, s.vs_bake);
    if (t.ps_pre) materials::bake_preshader(fxe, *rp.ps_plan, lookup, per_draw, s.ps_bake);
    s.valid = true;
}

// Images of one draw from the template slot: copy + per-draw bindings (+ preshader / bump env re-run), or the
// full fill when the split does not apply. Null outputs are skipped.
void xt_images(const Renderer::XTemplate& t, const Renderer::XTemplate::Slot& s, const fx::Effect& fxe,
               const materials::ResolvedPass& rp, const materials::ParamLookup& lookup, materials::VSConstants* vs,
               materials::PSConstants* ps, materials::IntBoolConstants* vib, materials::IntBoolConstants* pib) {
    if (vs) {
        if (t.full) {
            *vs = {};
            materials::fill_register_file(fxe, *rp.vs_plan, lookup, vs->c);
        } else {
            *vs = s.vs;
            for (uint32_t i : t.vs_draw) materials::fill_register_binding(fxe, rp.vs_plan->floats[i], lookup, vs->c);
            if (t.vs_pre && s.vs_bake.usable) materials::run_preshader(*rp.vs_plan, s.vs_bake, lookup, vs->c);
            else if (t.vs_pre) materials::fill_register_preshader(fxe, *rp.vs_plan, lookup, vs->c);
        }
    }
    if (ps) {
        if (t.full) {
            *ps = {};
            materials::fill_register_file(fxe, *rp.ps_plan, lookup, ps->c);
            materials::fill_bump_env(rp, fxe, lookup, ps->c);
        } else {
            *ps = s.ps;
            for (uint32_t i : t.ps_draw) materials::fill_register_binding(fxe, rp.ps_plan->floats[i], lookup, ps->c);
            if (t.ps_pre && s.ps_bake.usable) materials::run_preshader(*rp.ps_plan, s.ps_bake, lookup, ps->c);
            else if (t.ps_pre) materials::fill_register_preshader(fxe, *rp.ps_plan, lookup, ps->c);
            if (t.bump) materials::fill_bump_env(rp, fxe, lookup, ps->c);
        }
    }
    if (vib) {
        if (t.ib_draw) materials::fill_int_bool_constants(fxe, *rp.vs_plan, lookup, *vib);
        else *vib = s.vib;
    }
    if (pib) {
        if (t.ib_draw) materials::fill_int_bool_constants(fxe, *rp.ps_plan, lookup, *pib);
        else *pib = s.pib;
    }
}

}  // namespace detail

void Renderer::engine_frame(const ViewMatrices& v, const SceneLightConstants& slc, EngineFrame& f) const {
    f = EngineFrame{};
    to_d3dx(v.view, f.view);
    to_d3dx(v.proj, f.proj);
    to_d3dx(v.view_proj, f.view_proj);
    to_d3dx(v.view_inv, f.view_inv);
    const float eye[4] = {v.eye.x, v.eye.y, v.eye.z, 1.0f};
    std::copy(eye, eye + 4, f.eye);
    const float t4[4] = {time_, time_, time_, time_};
    std::copy(t4, t4 + 4, f.time4);
    const float w = static_cast<float>(v.width), h = static_cast<float>(v.height);
    const float res[4] = {w, h, 0.5f / w, 0.5f / h};  // size of the bound render target (TERRAIN_WATER_POST.md 4)
    std::copy(res, res + 4, f.resolution);
    (void)scene_fog_vals(lighting_, f.fog[0], f.fog[1]);  // the hot set's FOG_VALS (default pair when fog is off)
    f.fade[0] = -2e-5f;
    f.fade[1] = 3.0f;
    distance_fade_vals(fade_begin_, fade_end_, f.fade[0], f.fade[1]);
    materials::FrameCB cb{};
    fill_frame_lights(lighting_, cb);
    const float amb[4] = {cb.light_ambient.x, cb.light_ambient.y, cb.light_ambient.z, 1.0f};
    std::copy(amb, amb + 4, f.ambient);
    const float l0v[4] = {cb.light0_vector.x, cb.light0_vector.y, cb.light0_vector.z, 0.0f};
    std::copy(l0v, l0v + 4, f.l0_vec);
    const float l0d[4] = {cb.light0_diffuse.x, cb.light0_diffuse.y, cb.light0_diffuse.z, 1.0f};
    std::copy(l0d, l0d + 4, f.l0_dif);
    const float l0s[4] = {cb.light0_specular.x, cb.light0_specular.y, cb.light0_specular.z, 0.0f};
    std::copy(l0s, l0s + 4, f.l0_spec);
    const Float4 lv = v.view * Float4{l0v[0], l0v[1], l0v[2], 0.0f};
    const float lvl = std::sqrt(lv.x * lv.x + lv.y * lv.y + lv.z * lv.z);
    const float l0view[4] = {lvl > 0 ? lv.x / lvl : 0.0f, lvl > 0 ? lv.y / lvl : 0.0f, lvl > 0 ? lv.z / lvl : 1.0f, 0.0f};
    std::copy(l0view, l0view + 4, f.l0_view);
    std::memcpy(f.cloud_u, &cloud_u_, sizeof f.cloud_u);
    std::memcpy(f.cloud_v, &cloud_v_, sizeof f.cloud_v);
    std::memcpy(f.fow_u, &fow_u_, sizeof f.fow_u);
    std::memcpy(f.fow_v, &fow_v_, sizeof f.fow_v);
    std::copy(slc.sph_all.begin(), slc.sph_all.end(), f.sph_all);
    std::copy(slc.sph_fill.begin(), slc.sph_fill.end(), f.sph_fill);
}

bool Renderer::translated_constants(const Instance& inst, const GpuSubmesh& g, const GpuPass& p, const ViewMatrices& view,
                                    materials::VSConstants& vs, materials::PSConstants& ps, materials::IntBoolConstants* vs_ib,
                                    materials::IntBoolConstants* ps_ib) const {
    const SceneLightConstants slc = scene_light_constants(lighting_);
    return translated_constants_slc(inst, g, p, view, slc, vs, ps, vs_ib, ps_ib);
}

bool Renderer::translated_constants_slc(const Instance& inst, const GpuSubmesh& g, const GpuPass& p, const ViewMatrices& view,
                                        const SceneLightConstants& slc, materials::VSConstants& vs, materials::PSConstants& ps,
                                        materials::IntBoolConstants* vs_ib, materials::IntBoolConstants* ps_ib) const {
    if (!g.effect || !g.effect->effect || !p.pass || !p.pass->vs_plan || !p.pass->ps_plan || !inst.model || !inst.model->asset)
        return false;
    EngineFrame f;
    engine_frame(view, slc, f);
    std::vector<Mat3x4> palette;
    EngineParams e;
    translated_draw_params(inst, g, view, f, palette, e);
    const DrawLookupCtx ctx{&e, g.params.get(), &inst};
    const materials::ParamLookup lookup = draw_lookup(&ctx);
    vs = {};
    ps = {};
    const fx::Effect& fxe = *g.effect->effect;
    materials::fill_register_file(fxe, *p.pass->vs_plan, lookup, vs.c);
    materials::fill_register_file(fxe, *p.pass->ps_plan, lookup, ps.c);
    materials::fill_bump_env(*p.pass, fxe, lookup, ps.c);
    if (vs_ib) materials::fill_int_bool_constants(fxe, *p.pass->vs_plan, lookup, *vs_ib);
    if (ps_ib) materials::fill_int_bool_constants(fxe, *p.pass->ps_plan, lookup, *ps_ib);
    return true;
}

bool Renderer::translated_constants_cached(const Instance& inst, const GpuSubmesh& g, const GpuPass& p, const ViewMatrices& view,
                                           materials::VSConstants& vs, materials::PSConstants& ps, materials::IntBoolConstants* vs_ib,
                                           materials::IntBoolConstants* ps_ib) {
    if (!g.effect || !g.effect->effect || !p.pass || !p.pass->vs_plan || !p.pass->ps_plan || !inst.model || !inst.model->asset)
        return false;
    const fx::Effect& fxe = *g.effect->effect;
    EngineFrame f;
    engine_frame(view, scene_lights(), f);
    XTemplate t;
    xt_analyse(t, fxe, *p.pass);
    auto slot = std::make_unique<XTemplate::Slot>();
    EngineFrame masked;
    mask_frame(f, t.frame_mask, masked);
    xt_fill_slot(t, *slot, masked, fxe, *p.pass, g.params.get(), vs_ib || ps_ib);
    std::vector<Mat3x4> palette;
    EngineParams e;
    translated_draw_params(inst, g, view, f, palette, e);
    const DrawLookupCtx ctx{&e, g.params.get(), &inst};
    const materials::ParamLookup lookup = draw_lookup(&ctx);
    xt_images(t, *slot, fxe, *p.pass, lookup, &vs, &ps, vs_ib, ps_ib);
    return true;
}

}  // namespace alamo::render

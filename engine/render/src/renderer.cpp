// Renderer: init / shutdown, texture and model upload, pipelines (split: see renderer_shared.hpp).
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

// SPIR-V of shaders/unlit.hlsl (DXC -Fh byte arrays, see CMakeLists.txt).
#include "unlit_ps.h"
#include "unlit_vs.h"
#include "unlit_vs_skin.h"

namespace alamo::render {
using namespace detail;

namespace {

const Mat3x4& bone_or_identity(std::span<const Mat3x4> world, uint32_t b) {
    static const Mat3x4 kIdentity{};
    return b < world.size() ? world[b] : kIdentity;
}

}  // namespace

// ---- ALO bone billboards (docs/formats/SPEC.md 2.2 modes; SPACE_AND_OPEN.md C.4 planet Glow = sun_glow, star-dome
// sun_billboard = sun). Applied to the world matrix of a static submesh attached to a billboard bone. Axis convention
// [port]: billboard geometry is authored facing -Y (the 3ds Max front view, e.g. the planet Glow disc in the XZ plane),
// so the facing modes map local +X -> camera right, local +Z -> camera up and local -Y -> towards the camera; the
// column lengths (instance + bone scale) are kept. The original's exact axis use is not traced.
namespace {
Float3 bb_sub(const Float3& a, const Float3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Float3 bb_scale(const Float3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float bb_dot(const Float3& a, const Float3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Float3 bb_cross(const Float3& a, const Float3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
bool bb_norm(Float3& v) {
    const float l = std::sqrt(bb_dot(v, v));
    if (!(l > 1e-6f) || !std::isfinite(l)) return false;
    v = bb_scale(v, 1.0f / l);
    return true;
}
Float3 bb_col(const Mat3x4& m, int c) { return {m.m[0][c], m.m[1][c], m.m[2][c]}; }
void bb_set_col(Mat3x4& m, int c, const Float3& v) {
    m.m[0][c] = v.x;
    m.m[1][c] = v.y;
    m.m[2][c] = v.z;
}
// Rotation part of `m` replaced by the orthonormal frame (right, forward, up) scaled by m's column lengths.
void bb_orient(Mat3x4& m, const Float3& right, const Float3& forward, const Float3& up) {
    const float sx = std::sqrt(bb_dot(bb_col(m, 0), bb_col(m, 0))), sy = std::sqrt(bb_dot(bb_col(m, 1), bb_col(m, 1))),
                sz = std::sqrt(bb_dot(bb_col(m, 2), bb_col(m, 2)));
    bb_set_col(m, 0, bb_scale(right, sx));
    bb_set_col(m, 1, bb_scale(forward, sy));  // local +Y points away from the camera: the -Y face looks at it
    bb_set_col(m, 2, bb_scale(up, sz));
}
// Facing frame for a view direction `fwd` (normalised): right / up from the camera's up axis.
bool bb_face(Mat3x4& m, const Float3& fwd, const Float3& cam_up) {
    Float3 right = bb_cross(fwd, cam_up);
    if (!bb_norm(right)) return false;
    Float3 up = bb_cross(right, fwd);
    if (!bb_norm(up)) return false;
    bb_orient(m, right, fwd, up);
    return true;
}
}  // namespace

Mat3x4 billboard_world(const Mat3x4& world, uint32_t mode, const Mat3x4& parent_world, float bone_distance, const Float3& eye,
                       const Float3& cam_right, const Float3& cam_up, const Float3& cam_forward, const Float3& sun) {
    Mat3x4 m = world;
    const Float3 o = world.translation();
    Float3 s = sun;
    const bool have_sun = bb_norm(s);
    switch (mode) {
        case 1:  // parallel: the camera's right / up axes
            bb_orient(m, cam_right, cam_forward, cam_up);
            break;
        case 2: {  // face: towards the camera position
            Float3 f = bb_sub(o, eye);
            if (bb_norm(f)) bb_face(m, f, cam_up);
            break;
        }
        case 3:    // z_axis_view: about local Z towards the view
        case 5: {  // z_axis_light: about local Z towards the sun
            Float3 z = bb_col(world, 2);
            const float sz = std::sqrt(bb_dot(z, z));
            if (!bb_norm(z)) break;
            Float3 to = mode == 3 ? bb_sub(eye, o) : s;
            if (mode == 5 && !have_sun) break;
            to = bb_sub(to, bb_scale(z, bb_dot(to, z)));
            if (!bb_norm(to)) break;
            const Float3 fwd = bb_scale(to, -1.0f);  // -Y faces the target
            const Float3 right = bb_cross(fwd, z);
            const float sx = std::sqrt(bb_dot(bb_col(world, 0), bb_col(world, 0))), sy = std::sqrt(bb_dot(bb_col(world, 1), bb_col(world, 1)));
            bb_set_col(m, 0, bb_scale(right, sx));
            bb_set_col(m, 1, bb_scale(fwd, sy));
            bb_set_col(m, 2, bb_scale(z, sz));
            break;
        }
        case 6:    // sun_glow: the parent's origin + sun direction x the bone's distance, facing the camera
        case 7: {  // sun: camera + sun direction x the bone's distance, facing the camera
            if (!have_sun) break;
            const float scale = std::sqrt(bb_dot(bb_col(parent_world, 0), bb_col(parent_world, 0)));
            const Float3 base = mode == 6 ? parent_world.translation() : eye;
            const Float3 p{base.x + s.x * bone_distance * scale, base.y + s.y * bone_distance * scale, base.z + s.z * bone_distance * scale};
            m.m[0][3] = p.x;
            m.m[1][3] = p.y;
            m.m[2][3] = p.z;
            Float3 f = bb_sub(p, eye);
            if (bb_norm(f)) bb_face(m, f, cam_up);
            break;
        }
        default:  // 0 none, 4 z_axis_wind (no scene wind vector here [port]): unchanged
            break;
    }
    return m;
}

namespace detail {

// World matrix of a static submesh: instance x attach bone, with the bone's billboard mode applied.
Mat3x4 static_mesh_world(const Instance& inst, const assets::ModelAsset& ma, std::span<const Mat3x4> bw, uint32_t bone,
                         const ViewMatrices& view, const Float3& sun) {
    Mat3x4 w = inst.transform * bone_or_identity(bw, bone);
    const assets::Skeleton& sk = ma.skeleton;
    const uint32_t mode = bone < sk.billboard_mode.size() ? sk.billboard_mode[bone] : 0u;
    if (mode == 0 || mode == 4) return w;
    const int32_t parent = bone < sk.parents.size() ? sk.parents[bone] : -1;
    const Mat3x4 pw = inst.transform * (parent >= 0 ? bone_or_identity(bw, static_cast<uint32_t>(parent)) : Mat3x4{});
    // Bone distance from its parent in model units (bind pose: the loader keeps the ALO translation).
    float dist = 0.0f;
    if (bone < sk.bind_local.size()) {
        const Float3 t = sk.bind_local[bone].translation();
        dist = std::sqrt(bb_dot(t, t));
    }
    const Mat4& vi = view.view_inv;  // camera -> world: columns = camera right / up / back
    const Float3 right{vi.m[0][0], vi.m[1][0], vi.m[2][0]}, up{vi.m[0][1], vi.m[1][1], vi.m[2][1]};
    const Float3 fwd{-vi.m[0][2], -vi.m[1][2], -vi.m[2][2]};
    return billboard_world(w, mode, pw, dist, view.eye, right, up, fwd, sun);
}

uint32_t engine_sampler_for(const materials::SamplerKey& k) {
    const bool clamp = k.u == materials::AddressMode::ClampToEdge || k.u == materials::AddressMode::ClampToBorder ||
                       k.u == materials::AddressMode::MirrorClampToEdge;
    const bool point = k.min == materials::Filter::Nearest;
    if (point) return clamp ? materials::kSamplerClampPoint : materials::kSamplerWrapPoint;
    return clamp ? materials::kSamplerClampLinear : materials::kSamplerWrapLinear;
}

const assets::MaterialParam* find_param(const assets::MaterialRef& m, std::string_view name) {
    return name.empty() ? nullptr : m.find(name);
}

materials::ParamStore param_store(const assets::MaterialRef& m) {
    materials::ParamStore s;
    for (const assets::MaterialParam& p : m.params) {
        switch (p.type) {
        case assets::ParamType::Int: s.set(p.name, {static_cast<float>(p.int_value)}); break;
        case assets::ParamType::Float: s.set(p.name, {p.value.x}); break;
        case assets::ParamType::Float3: s.set(p.name, {p.value.x, p.value.y, p.value.z}); break;
        case assets::ParamType::Float4: s.set(p.name, {p.value.x, p.value.y, p.value.z, p.value.w}); break;
        case assets::ParamType::Texture: break;
        }
    }
    s.set_semantic("LIGHT_SCALE", {1, 1, 1, 1});
    return s;
}

}  // namespace detail

BlendMode blend_mode_for_key(const materials::PsoKey& k) {
    if (!k.blend_enable) return BlendMode::Opaque;
    return k.dst_color == materials::BlendFactor::One ? BlendMode::Additive : BlendMode::Alpha;
}

std::string diffuse_texture_name(const assets::MaterialRef& m) {
    for (const auto& p : m.params)
        if (p.type == assets::ParamType::Texture && !p.texture.empty() && ascii_lower(p.name) == "basetexture") return p.texture;
    for (const auto& p : m.params)
        if (p.type == assets::ParamType::Texture && !p.texture.empty() &&
            assets::texture_role_for_param(p.name) == assets::TextureRole::Color)
            return p.texture;
    return {};
}

PhaseBucket phase_bucket(materials::RenderPhase p) {
    using RP = materials::RenderPhase;
    switch (p) {
    case RP::Terrain: return PhaseBucket::Terrain;
    case RP::TerrainMesh: return PhaseBucket::TerrainMesh;
    case RP::Fissure: return PhaseBucket::Fissure;
    case RP::Occluded: return PhaseBucket::Occluded;
    case RP::Shadow: return PhaseBucket::Shadow;
    case RP::TerrainWater:
    case RP::Water: return PhaseBucket::Water;
    case RP::Transparent: return PhaseBucket::Transparent;
    case RP::Bloom: return PhaseBucket::Bloom;
    case RP::Heat: return PhaseBucket::Heat;
    case RP::None:
    case RP::Opaque:
    case RP::Count: break;
    }
    return PhaseBucket::Opaque;  // effects without _ALAMO_RENDER_PHASE go to Opaque
}

const char* phase_bucket_name(PhaseBucket b) {
    static const char* const kNames[] = {"Terrain", "TerrainMesh", "Fissure", "Opaque", "Occluded", "OpaqueNonOccluding",
                                         "Shadow", "Water", "Transparent", "Lasers", "Bloom", "Heat"};
    const auto i = static_cast<size_t>(b);
    return i < std::size(kNames) ? kNames[i] : "?";
}

bool GpuModel::resident() const {
    if (!buffers_ready) return false;
    for (const auto& t : textures)
        if (!t.resident) return false;
    return true;
}

assets::Aabb GpuModel::drawn_bounds() const { return submesh_bounds(true); }
assets::Aabb GpuModel::cull_bounds() const { return submesh_bounds(false); }

assets::Aabb GpuModel::submesh_bounds(bool default_mask) const {
    assets::Aabb total{{1, 1, 1}, {-1, -1, -1}};
    if (!asset) return total;
    for (const GpuSubmesh& g : submeshes) {
        const assets::Submesh& sm = asset->submeshes[g.submesh];
        if (!sm.bounds.valid() || (default_mask && g.mesh < mesh_hidden.size() && mesh_hidden[g.mesh])) continue;
        if (default_mask && g.alt > 0) continue;  // R4: damage alternates are not part of the default (alt 0) look
        const assets::Aabb b =
            sm.skinned() ? sm.bounds : assets::transform_aabb(bone_or_identity(asset->skeleton.bind_world, sm.attach_bone), sm.bounds);
        total = total.valid() ? assets::merge(total, b) : b;
    }
    return total;
}

assets::Sphere GpuModel::drawn_sphere() const {
    const assets::Aabb b = drawn_bounds();
    if (!b.valid()) return asset ? asset->sphere : assets::Sphere{};
    assets::Sphere s;
    s.center = {0.5f * (b.min.x + b.max.x), 0.5f * (b.min.y + b.max.y), 0.5f * (b.min.z + b.max.z)};
    const float dx = b.max.x - s.center.x, dy = b.max.y - s.center.y, dz = b.max.z - s.center.z;
    s.radius = std::sqrt(dx * dx + dy * dy + dz * dz);
    return s;
}

// ------------------------------------------------------------------------------------------ init

Renderer::Renderer() = default;
Renderer::~Renderer() { shutdown(); }

uint32_t Renderer::aniso_cap() const {
    return ctx_ && ctx_->caps().sampler_anisotropy ? static_cast<uint32_t>(ctx_->caps().max_anisotropy) : 1u;
}

uint32_t Renderer::anisotropy() const { return ctx_ ? (aniso_ & 0xFFu) : anisotropy_level(aniso_request_, 16); }

bool Renderer::write_engine_samplers(uint32_t level) {
    // Described as materials keys and converted; the linear pair is the engine quality sampler (8x, PORTING.md
    // 3.3), raised by the anisotropy setting like any material key ([port]).
    materials::SamplerKey wrap_linear;
    wrap_linear.max_anisotropy = 8;
    materials::SamplerKey clamp_linear = wrap_linear;
    clamp_linear.u = clamp_linear.v = clamp_linear.w = materials::AddressMode::ClampToEdge;
    materials::SamplerKey wrap_point;
    wrap_point.mag = wrap_point.min = materials::Filter::Nearest;
    materials::SamplerKey clamp_point = wrap_point;
    clamp_point.u = clamp_point.v = clamp_point.w = materials::AddressMode::ClampToEdge;
    const uint32_t cap = aniso_cap();
    const VkSampler smp[materials::kEngineSamplerCount] = {
        samplers_.get(to_rhi_sampler(wrap_linear, cap, level)), samplers_.get(to_rhi_sampler(clamp_linear, cap, level)),
        samplers_.get(to_rhi_sampler(wrap_point, cap, level)), samplers_.get(to_rhi_sampler(clamp_point, cap, level))};
    for (VkSampler s : smp)
        if (!s) return false;
    rhi::write_bindless_samplers(*ctx_, bindless_, smp);
    return true;
}

// [port] Start of draw()/render(): latch the requested anisotropy for this frame's recording threads. The bindless
// sampler binding is not update-after-bind, so the engine samplers are rewritten only by the first call of a new
// frame (nothing of it has bound the set yet) and after the frames in flight finished (one idle wait per change).
void Renderer::latch_anisotropy(const rhi::Frame& frame) {
    aniso_ = anisotropy_level(aniso_request_, aniso_cap()) | (mip_skip_request_ << 8);
    const bool new_frame = frame.number() != aniso_frame_;
    aniso_frame_ = frame.number();
    if (!new_frame || aniso_ == aniso_engine_ || !bindless_) return;
    // Only the linear pair depends on the level: no rewrite while both levels map to the same samplers.
    materials::SamplerKey linear;
    linear.max_anisotropy = 8;
    if (to_rhi_sampler(linear, aniso_cap(), aniso_) == to_rhi_sampler(linear, aniso_cap(), aniso_engine_)) {
        aniso_engine_ = aniso_;
        return;
    }
    ctx_->wait_idle();
    if (write_engine_samplers(aniso_)) aniso_engine_ = aniso_;
}

bool Renderer::init(rhi::Context& ctx, rhi::Allocator& alloc, rhi::Uploader& up, const RendererDesc& desc) {
    shutdown();
    ctx_ = &ctx;
    alloc_ = &alloc;
    up_ = &up;
    desc_ = desc;
    // Scene depth-stencil for the frame graph (stencil shadows); draw() keeps the caller's formats.
    ds_format_ = rhi::to_vk(rhi::pick_depth_stencil_format(ctx));
    rp_external_ = render_pass_id(desc.color_format, desc.depth_format);
    rp_scene_ = ds_format_ != VK_FORMAT_UNDEFINED ? render_pass_id(desc.color_format, ds_format_) : rp_external_;
    rp_post_ = render_pass_id(desc.color_format, VK_FORMAT_UNDEFINED);
    render_pass_ = rp_main_ = rp_world_ = rp_scene_;
    msaa_ = 1;

    if (!hot_.init(ctx, desc.texture_capacity)) return false;
    layout_ = hot_.create_pipeline_layout(rhi::kHotPushBytes);
    if (!layout_) return false;
    samplers_.init(ctx);
    if (!unlit_cache_.init(ctx)) return false;
    if (!bindless_alloc_.init(ctx, hot_.bindless_pool_ratios(1))) return false;
    bindless_ = bindless_alloc_.allocate(hot_.textures());
    if (!bindless_) return false;

    // Engine samplers (EngineSampler order) at the current anisotropy setting.
    aniso_ = aniso_engine_ = anisotropy_level(aniso_request_, aniso_cap()) | (mip_skip_request_ << 8);
    aniso_frame_ = ~0ull;
    if (!write_engine_samplers(aniso_)) return false;

    // Slot 0 = materials::kNoTexture: 1x1 opaque white.
    rhi::ImageDesc wd;
    wd.format = rhi::Format::RGBA8;
    wd.width = wd.height = 1;
    wd.mips = 1;
    wd.name = "render.white";
    if (!alloc.create_image(wd, white_)) return false;
    const uint8_t white[4] = {255, 255, 255, 255};
    const rhi::UploadTicket t = up.upload_image_chain(white_, white);
    if (!t || !up.finish(t)) return false;
    rhi::write_bindless_texture(ctx, bindless_, materials::kNoTexture, white_.view);
    // Slots 1.. through the rhi table (deferred reuse; retired slots show white again).
    const uint32_t table_slots = desc.bindless_slots ? std::min(desc.bindless_slots, hot_.texture_capacity()) : hot_.texture_capacity();
    if (!table_.init(ctx, bindless_, table_slots, white_.view, 1)) return false;

    // ---- Track B: runtime DXC, shader cache, the StandardLayouts interface, zero buffer, white cube.
    xlat_counter_ = std::make_unique<jobs::JobCounter>();
    if (desc.translated && desc.effects) {
        dxc_ = std::make_unique<shaderxlat::RuntimeDxc>();
        std::string err;
        dxc_ok_ = dxc_->load({}, &err);
        dxc_status_ = dxc_ok_ ? dxc_->version() + " (" + dxc_->library_path() + ")" : "unavailable: " + err;
        if (!dxc_ok_) log::warnf("render", "runtime DXC %s; translated passes draw unlit", dxc_status_.c_str());
        scache_ = desc.shader_cache_dir.empty() ? std::make_unique<shaderxlat::ShaderCache>()
                                                : std::make_unique<shaderxlat::ShaderCache>(desc.shader_cache_dir);
    } else {
        dxc_status_ = "disabled";
    }
    if (!std_layouts_.init(ctx, rhi::kLayoutIntBoolConstants)) return false;
    for (int k = 0; k < 4; ++k) {  // [int/bool] + 2 x [Q3 instanced form: storage-buffer register images]
        tlayout_[k] = std_layouts_.create_pipeline_layout(rhi::kTranslatedPushBytes, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                                          (k & 1) != 0, (k & 2) != 0);
        if (!tlayout_[k]) return false;
    }
    if (!alloc.create_buffer(64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, rhi::MemoryUsage::GpuOnly,
                             zero_vb_, "render.zero_vb"))
        return false;
    const uint8_t zeros[64] = {};
    const rhi::UploadTicket zt = up.upload_buffer(zero_vb_, 0, zeros, VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                                                  VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    rhi::ImageDesc cd;
    cd.format = rhi::Format::RGBA8;
    cd.width = cd.height = 1;
    cd.mips = 1;
    cd.layers = 6;
    cd.cube = true;
    cd.name = "render.white_cube";
    if (!alloc.create_image(cd, white_cube_)) return false;
    std::vector<uint8_t> faces(6 * 4, 255);
    const rhi::UploadTicket ct = up.upload_image_chain(white_cube_, faces);
    if (!zt || !ct || !up.finish(zt) || !up.finish(ct)) return false;
    rhi::ImageDesc vd;  // 1x1x1 white volume for 3D sampler slots without a texture
    vd.format = rhi::Format::RGBA8;
    vd.width = vd.height = 1;
    vd.depth = 1 + 1;  // rhi builds a 3D image for depth > 1
    vd.mips = 1;
    vd.name = "render.white_3d";
    if (!alloc.create_image(vd, white_3d_)) return false;
    const uint8_t white8[8] = {255, 255, 255, 255, 255, 255, 255, 255};
    const rhi::UploadTicket vt = up.upload_image_chain(white_3d_, white8);
    // Full-screen quad, FLOAT2 at +-1.005 as two triangles (the engine's 4-vertex strip, 4.2).
    const float q = 1.005f;
    const float quad[12] = {-q, -q, q, -q, -q, q, q, -q, q, q, -q, q};
    if (!alloc.create_buffer(sizeof quad, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             rhi::MemoryUsage::GpuOnly, quad_vb_, "render.fullscreen_quad"))
        return false;
    const rhi::UploadTicket qt = up.upload_buffer(quad_vb_, 0, {reinterpret_cast<const uint8_t*>(quad), sizeof quad},
                                                  VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                                                  VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
    if (!vt || !qt || !up.finish(vt) || !up.finish(qt)) return false;
    // Pre-lit prim vertices of the stencil darken (colour = shadow colour), two blocks per frame slot: [0] the darken /
    // blur quad, [1] the alpha reset quad of the soft path (post_darken(reset_alpha)).
    if (!alloc.create_buffer(4 * 2 * 6 * 24, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, rhi::MemoryUsage::Dynamic, prim_vb_,
                             "render.darken_prims"))
        return false;

    PipelineManagerDesc pd;
    pd.dir = desc.pipeline_cache_dir;
    pd.js = desc.js;
    pd.layout = layout_;
    pd.translated = [this](const materials::PsoKey& k, PipelineBuild& b, std::string* err) {
        const materials::TranslatedProgram* tp = find_translated(k.vs_id, k.ps_id);
        if (!tp) {
            if (err) *err = "translated program not ready";
            return false;
        }
        return pipeline_desc_from_translated(k, *tp, translated_layout(*tp), b, err);
    };
    if (!psos_.init(ctx, pd)) return false;
    return create_unlit_pipelines();
}

void Renderer::shutdown() {
    if (!ctx_) return;
    wait_translations();
    bc_.reset();  // memo: pipeline / program handles of this device
    for (auto& b : built_) b.reset();
    ssao_.reset();  // [port] SSAO pipelines + targets
    smaa_.reset();  // [port] SMAA pipelines + targets + lookup textures
    taa_.reset();   // [port] TAA pipelines + history
    gamma_.reset();  // [port] gamma pipeline + scratch image
    psos_.shutdown(true);
    for (VkPipelineLayout& l : tlayout_) {
        if (l) ctx_->vkd().vkDestroyPipelineLayout(ctx_->device(), l, nullptr);
        l = VK_NULL_HANDLE;
    }
    std_layouts_.shutdown();
    if (zero_vb_) {
        up_->forget(zero_vb_.handle);
        alloc_->destroy(zero_vb_);
    }
    if (white_cube_) {
        up_->forget(white_cube_.handle);
        alloc_->destroy(white_cube_);
    }
    if (white_3d_) {
        up_->forget(white_3d_.handle);
        alloc_->destroy(white_3d_);
    }
    if (quad_vb_) {
        up_->forget(quad_vb_.handle);
        alloc_->destroy(quad_vb_);
    }
    if (prim_vb_) alloc_->destroy(prim_vb_);
    tg_.scene.shutdown();
    tg_.scene_tex.shutdown();
    tg_.distortion.shutdown();
    tg_.bloom_a.shutdown();
    tg_.bloom_b.shutdown();
    tg_.scene_ms.shutdown();
    tg_.ready = false;
    for (auto& f : fx_post_) f.reset();
    {
        std::lock_guard<std::mutex> lock(xlat_mutex_);
        xlat_.clear();
    }
    xlat_counter_.reset();
    dxc_.reset();
    scache_.reset();
    dxc_ok_ = false;
    destroy_unlit_pipelines();
    if (layout_) ctx_->vkd().vkDestroyPipelineLayout(ctx_->device(), layout_, nullptr);
    layout_ = VK_NULL_HANDLE;
    shutdown_lasers();
    shutdown_beams();
    {  // GPU idle: every deferred image can go
        std::lock_guard<std::mutex> lock(deferred_mutex_);
        for (Deferred& d : deferred_images_) {
            up_->forget(d.image.handle);
            alloc_->destroy(d.image);
        }
        deferred_images_.clear();
        for (DeferredBuffer& d : deferred_buffers_) alloc_->destroy(d.buffer);
        deferred_buffers_.clear();
    }
    table_.shutdown();
    if (white_) {
        up_->forget(white_.handle);
        alloc_->destroy(white_);
    }
    bindless_alloc_.shutdown();
    bindless_ = VK_NULL_HANDLE;
    unlit_cache_.shutdown(false);
    samplers_.shutdown();
    hot_.shutdown();
    ctx_ = nullptr;
}

bool Renderer::set_color_format(VkFormat format) {
    if (format == desc_.color_format) return true;
    destroy_unlit_pipelines();
    desc_.color_format = format;
    rp_external_ = render_pass_id(desc_.color_format, desc_.depth_format);
    rp_scene_ = ds_format_ != VK_FORMAT_UNDEFINED ? render_pass_id(desc_.color_format, ds_format_) : rp_external_;
    rp_post_ = render_pass_id(desc_.color_format, VK_FORMAT_UNDEFINED);
    // Lit keys follow automatically. [port] MSAA: the main scene class follows the format with the same sample count.
    const uint8_t rp_ms = msaa_ > 1 ? render_pass_id(desc_.color_format, ds_format_, msaa_) : uint8_t{0};
    if (!rp_ms) msaa_ = 1;
    rp_main_ = rp_ms ? rp_ms : rp_scene_;
    rp_world_ = rp_scene_;
    render_pass_ = rp_main_;
    tg_.scene_ms.shutdown();
    if (tg_.ready) {
        tg_.scene.shutdown();
        tg_.scene_tex.shutdown();
        tg_.distortion.shutdown();
        tg_.bloom_a.shutdown();
        tg_.bloom_b.shutdown();
        tg_.ready = false;
    }
    if (world_ && !world_->create_targets()) return false;  // water / sky-cube targets follow the colour format
    return create_unlit_pipelines();
}

void Renderer::destroy_unlit_pipelines() {
    for (auto* set : {&unlit_, &unlit_int_, &unlit_ms_})
        for (auto& row : *set)
            for (VkPipeline& p : row) {
                if (p) ctx_->vkd().vkDestroyPipeline(ctx_->device(), p, nullptr);
                p = VK_NULL_HANDLE;
            }
}

bool Renderer::create_unlit_pipelines() {
    const std::vector<uint32_t> vs[2] = {rhi::spirv_from_bytes({g_unlit_vs, sizeof(g_unlit_vs)}),
                                         rhi::spirv_from_bytes({g_unlit_vs_skin, sizeof(g_unlit_vs_skin)})};
    const std::vector<uint32_t> ps = rhi::spirv_from_bytes({g_unlit_ps, sizeof(g_unlit_ps)});
    for (int set = 0; set < 3; ++set)
    for (int skinned = 0; skinned < 2; ++skinned) {
        if (set == 2 && msaa_ <= 1) break;  // [port] MSAA: set 2 only with a multisampled main scene
        VertexInputDesc vin;
        const materials::VertexLayout l = skinned ? materials::VertexLayout::RSkinNU2 : materials::VertexLayout::NU2;
        if (!build_vertex_input(l, spirv_input_locations(vs[skinned]), vin)) return false;
        // set 0: the caller's formats (draw()); set 1: the frame graph's scene depth-stencil (render()); set 2: the same with
        // msaa_ samples (the multisampled main scene).
        const VkFormat depth = set == 0 || ds_format_ == VK_FORMAT_UNDEFINED ? desc_.depth_format : ds_format_;
        const rhi::FormatInfo& dfi = rhi::format_info(rhi::from_vk(depth));
        for (int b = 0; b < 3; ++b) {
            rhi::GraphicsPipelineDesc d;
            d.vs_spirv = vs[skinned];
            d.fs_spirv = ps;
            d.vs_entry = "VSMain";
            d.fs_entry = "PSMain";
            d.bindings = {&vin.binding, 1};
            d.attributes = vin.attributes;
            d.raster.cull = VK_CULL_MODE_BACK_BIT;  // same convention as materials (pso.hpp): outward faces CCW
            d.raster.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            d.depth.depth_test = true;
            d.depth.depth_write = b == static_cast<int>(BlendMode::Opaque);
            d.depth.depth_op = VK_COMPARE_OP_LESS_OR_EQUAL;
            if (b == static_cast<int>(BlendMode::Alpha)) {
                d.blend.enable = true;
                d.blend.src_color = VK_BLEND_FACTOR_SRC_ALPHA;
                d.blend.dst_color = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                d.blend.src_alpha = VK_BLEND_FACTOR_ONE;
                d.blend.dst_alpha = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            } else if (b == static_cast<int>(BlendMode::Additive)) {
                d.blend.enable = true;
                d.blend.src_color = VK_BLEND_FACTOR_ONE;
                d.blend.dst_color = VK_BLEND_FACTOR_ONE;
                d.blend.src_alpha = VK_BLEND_FACTOR_ZERO;
                d.blend.dst_alpha = VK_BLEND_FACTOR_ONE;
            }
            d.color_formats = {&desc_.color_format, 1};
            d.depth_format = depth;
            d.stencil_format = dfi.stencil ? depth : VK_FORMAT_UNDEFINED;
            d.samples = set == 2 ? static_cast<VkSampleCountFlagBits>(msaa_) : VK_SAMPLE_COUNT_1_BIT;
            d.layout = layout_;
            d.name = "render.unlit";
            VkPipeline& slot = set == 0 ? unlit_[skinned][b] : set == 1 ? unlit_int_[skinned][b] : unlit_ms_[skinned][b];
            slot = unlit_cache_.create_graphics(d);
            if (!slot) return false;
        }
    }
    return true;
}

uint32_t Renderer::msaa_samples(uint32_t requested) const {
    if (!ctx_ || ds_format_ == VK_FORMAT_UNDEFINED) return 1;  // no frame-graph depth-stencil: nothing to multisample
    const VkSampleCountFlags ok = ctx_->caps().framebuffer_samples;
    for (const uint32_t s : {8u, 4u, 2u})  // VkSampleCountFlagBits values are the counts
        if (s <= requested && (ok & s) != 0 && render_pass_id(desc_.color_format, ds_format_, s) != 0) return s;
    return 1;
}

bool Renderer::set_msaa(uint32_t samples) {
    if (samples == msaa_) return true;
    ctx_->wait_idle();  // the old scene_ms target and unlit_ms_ pipelines may be used by frames in flight
    tg_.scene_ms.shutdown();
    destroy_unlit_pipelines();
    const uint8_t rp = samples > 1 ? render_pass_id(desc_.color_format, ds_format_, samples) : uint8_t{0};
    msaa_ = rp ? samples : 1u;
    rp_main_ = rp ? rp : rp_scene_;
    render_pass_ = rp_main_;
    if (!create_unlit_pipelines()) {
        log::errorf("render", "MSAA %ux: unlit pipelines could not be created", samples);
        if (msaa_ > 1) {  // back to one sample
            destroy_unlit_pipelines();
            msaa_ = 1;
            rp_main_ = render_pass_ = rp_scene_;
            create_unlit_pipelines();
        }
        return false;
    }
    return msaa_ == samples;
}

// ---------------------------------------------------------------------------------------- upload

bool Renderer::upload_texture(GpuTexture& t, const assets::TextureAsset& src) {
    const assets::TextureAsset* tex = &src;
    assets::TextureAsset fallback;
    auto usable = [&](const assets::TextureAsset& a) {
        const rhi::Format f = to_rhi_format(a.format);
        if (f == rhi::Format::Undefined || a.cubemap || a.layers != 1 || a.mip_levels == 0 || a.width == 0 || a.height == 0)
            return false;
        if (rhi::format_info(f).compressed && !ctx_->caps().texture_bc) return false;
        for (const assets::TextureLevel& l : a.levels) {
            if (l.layer != 0 || l.mip >= a.mip_levels) return false;
            if (l.size != rhi::subresource_size(f, rhi::mip_extent(a.width, l.mip), rhi::mip_extent(a.height, l.mip)))
                return false;
            if (l.offset + l.size > a.data.size()) return false;
        }
        return a.levels.size() == a.mip_levels;
    };
    if (!usable(*tex)) {
        log::warnf("render", "texture %s: unsupported layout (format %s, %u layers%s), using fallback", t.name.c_str(),
                   assets::texture_format_name(tex->format), tex->layers, tex->cubemap ? ", cube" : "");
        fallback = assets::make_fallback_texture(t.role);
        tex = &fallback;
        t.fallback = true;
    }
    t.fallback = t.fallback || tex->is_fallback;
    rhi::Swizzle sw = rhi::Swizzle::Identity;
    if (!to_rhi_swizzle(tex->swizzle, sw))
        log::warnf("render", "texture %s: component mapping has no rhi swizzle, using identity", t.name.c_str());
    rhi::ImageDesc d;
    d.format = to_rhi_format(tex->format);
    d.width = tex->width;
    d.height = tex->height;
    d.mips = tex->mip_levels;
    d.swizzle = sw;
    d.name = t.name.c_str();
    if (!alloc_->create_image(d, t.image)) return false;
    // Native DDS data stays inside the file bytes: one SubresourceData per level at its offset.
    std::vector<rhi::SubresourceData> subs;
    subs.reserve(tex->levels.size());
    for (const assets::TextureLevel& l : tex->levels) subs.push_back({l.mip, l.layer, tex->level_bytes(l)});
    t.ticket = up_->upload_image(t.image, subs, rhi::ImageState::ShaderRead);
    return static_cast<bool>(t.ticket);
}

uint8_t Renderer::cloud_texture_slots(const materials::ResolvedPass& p, const int (&tex)[4]) {
    uint8_t mask = 0;
    for (const materials::SamplerBinding& sb : p.samplers)
        if (sb.slot < 4 && tex[sb.slot] < 0 && alamo::iequals(sb.texture_semantic, "CLOUD_TEXTURE"))
            mask = static_cast<uint8_t>(mask | (1u << sb.slot));
    return mask;
}

void Renderer::setup_lit(const assets::ModelAsset& model, const assets::Submesh& sm, GpuSubmesh& g,
                         const std::function<int(const std::string&, assets::TextureRole)>& add_texture) {
    (void)model;
    g.lit = LitStatus::NoEffect;
    if (!desc_.effects || sm.material.effect.empty()) return;
    g.effect = desc_.effects->get_or_default(sm.material.effect);  // D.3: missing -> green TNoShader default
    g.effect_name = effect_key(sm.material.effect);
    if (!g.effect) return;
    const materials::MaterialEffect& e = *g.effect;
    // D.1 / D.3: every missing effect name resolves to the ONE shared default effect object, so those draws
    // form a single batch (sorted by material key) under the default effect's identity; a file that exists but
    // fails keeps its own name (the green effect re-created under it).
    if (e.fallback == materials::EffectFallback::Missing || e.fallback == materials::EffectFallback::BuiltinDefault)
        g.effect_name = materials::kDefaultEffectName;
    g.effect_order = desc_.effects->load_rank(e);
    g.bucket = phase_bucket(e.phase);
    const materials::ParamStore store = param_store(sm.material);
    const materials::ParamLookup lookup = store.lookup();
    if (e.effect) {
        g.material = materials::build_mesh_material(*e.effect, lookup);
    } else {
        g.material.diffuse = {1, 1, 1, 1};
        g.material.specular = {1, 1, 1, 16};
        g.material.colorization = {1, 1, 1, 1};
    }
    if (desc_.team_color_override) g.material.colorization = desc_.team_color;
    g.material.tex = {};
    uint64_t h = alamo::fnv1a64(&g.material, sizeof g.material);

    g.params = std::make_shared<const materials::ParamStore>(store);
    bool untranslated = false, mismatch = false;
    for (const materials::ResolvedPass& p : e.passes) {
        if (p.restore_only) continue;
        GpuPass gp;
        gp.pass = &p;
        gp.ff = p.fixed_function;
        gp.translated = p.program_status == materials::ProgramStatus::Translated;
        {
            const materials::StateSlot& fs = p.state.slot(materials::RS::FogEnable);
            gp.fog_off = (fs.origin == materials::StateOrigin::Constant || fs.origin == materials::StateOrigin::Phase) && fs.value == 0;
        }
        if (!gp.translated && (p.program_status != materials::ProgramStatus::Ok || !p.program.vs)) untranslated = true;
        // FF passes take their program from the mesh layout (draw_key_variant), so only shader passes
        // must match the skinned / static family of the vertices.
        const materials::VertexLayout el = p.pso[0].vertex_layout;
        // Translated passes read the mesh through their own input table (pipeline_desc_from_translated).
        if (!p.fixed_function && !gp.translated && el != materials::VertexLayout::Unknown &&
            materials::vertex_layout_skinned(el) != g.skinned)
            mismatch = true;
        auto texture_for = [&](const std::string& param) -> int {
            const assets::MaterialParam* mp = find_param(sm.material, param);
            if (!mp || mp->type != assets::ParamType::Texture || mp->texture.empty()) return -1;
            return add_texture(mp->texture, assets::texture_role_for_param(param));
        };
        if (gp.translated)
            for (const materials::SamplerBinding& sb : p.samplers)
                gp.xslots.push_back({static_cast<uint8_t>(sb.slot), sb.texture_param.empty() ? -1 : texture_for(sb.texture_param), sb.key});
        for (const materials::SamplerBinding& sb : p.samplers) {
            if (sb.slot >= 4) continue;
            gp.sampler[sb.slot] = engine_sampler_for(sb.key);
            if (!p.fixed_function && !sb.texture_param.empty()) gp.tex[sb.slot] = texture_for(sb.texture_param);
        }
        if (p.fixed_function && p.ff) {
            for (int s = 0; s < materials::kFfStages && s < 4; ++s)
                if (!p.ff->stage_texture[static_cast<size_t>(s)].empty())
                    gp.tex[s] = texture_for(p.ff->stage_texture[static_cast<size_t>(s)]);
            if (e.effect) {
                auto tpl = std::make_shared<materials::FfDrawTemplate>();
                std::string why;
                if (tpl->build(*p.ff, *e.effect, lookup, sm.format.color, &why)) {
                    gp.ff_data = tpl->base();
                    gp.ff_tpl = std::move(tpl);
                } else {
                    log::warnf("render", "%s: FF template: %s (load-time values used)", g.effect_name.c_str(), why.c_str());
                    gp.ff_data = materials::build_ff_draw_data(*p.ff, *e.effect, lookup, sm.format.color);
                }
            }
        }
        gp.cloud_slots = cloud_texture_slots(p, gp.tex);  // engine CLOUD_TEXTURE: the world's clouds per draw
        for (int s = 0; s < 4; ++s) {
            const int ti = gp.tex[s];
            h = alamo::fnv1a64(&ti, sizeof ti, h);
        }
        g.passes.push_back(gp);
    }
    g.material_hash = h;
    if (g.passes.empty() || untranslated) g.lit = LitStatus::Untranslated;
    else if (mismatch) g.lit = LitStatus::LayoutMismatch;
    else g.lit = LitStatus::Lit;  // Shadow / Heat buckets: drawn by render()'s frame graph only
    if (!g.passes.empty()) {
        g.unlit_blend = blend_mode_for_key(g.passes[0].pass->pso[0]);
        g.unlit_alpha_func = g.passes[0].pass->push.alpha_func;  // the material state's alpha test
        g.unlit_alpha_ref = g.passes[0].pass->push.alpha_ref;
        g.unlit_fog_off = g.passes[0].fog_off;
    }
}

bool Renderer::upload_model(std::shared_ptr<const assets::ModelAsset> model, const TextureSource& textures, GpuModel& out,
                            jobs::JobSystem* js, std::string* error) {
    out = GpuModel{};
    {
        static std::atomic<uint64_t> next_uid{0};  // process-wide: memo entries never confuse two uploads
        out.uid = ++next_uid;
    }
    if (!model) {
        if (error) *error = "no model";
        return false;
    }
    out.asset = model;
    // LOD picked per instance at draw time. Damage decal / state meshes and ALO-hidden render meshes (B.10: a
    // destroyed hardpoint can show them) are uploaded too and hidden by the default mask (ALO flag + desc rule)
    // or the instance's mask (the type's hardpoint XML, destroyed hardpoints).
    // R4: every _ALT damage alternate is uploaded too (GpuSubmesh::alt); Instance::alt picks one per frame.
    out.selection = select_meshes_all_lods(*model, kAnyAlt, true, nullptr);
    for (size_t i = 0; i < model->meshes.size(); ++i)
        if (model->meshes[i].hidden) out.selection.mesh_visible[i] = 1;
    out.mesh_hidden = hidden_mesh_mask(*model, desc_.hide_damage_decals ? desc_.damage_decal_rule : nullptr);  // + ALO flag
    for (size_t i = 0; i < model->meshes.size(); ++i)
        out.damage_meshes_hidden += (out.mesh_hidden[i] && out.selection.mesh_visible[i] && !model->meshes[i].hidden) ? 1u : 0u;
    // + stencil shadow volumes; ALO-hidden render geometry included (masked by mesh_hidden).
    const std::vector<uint32_t> drawn = drawable_submeshes(*model, out.selection, true, true);

    struct TexReq {
        std::string name, key;
        assets::TextureRole role;
    };
    std::vector<TexReq> reqs;
    auto add_texture = [&](const std::string& name, assets::TextureRole role) -> int {
        const std::string key = ascii_lower(name) + "#" + std::to_string(static_cast<int>(role));
        for (size_t i = 0; i < reqs.size(); ++i)
            if (reqs[i].key == key) return static_cast<int>(i);
        reqs.push_back({name, key, role});
        return static_cast<int>(reqs.size() - 1);
    };

    bool ok = true;
    // R1: highest _ALT index of the model's sub-objects (meshes and proxies; 0x12dca0, TACTICAL_GAPS §6.3).
    out.max_alt = -1;
    for (const assets::MeshInfo& mi : model->meshes) out.max_alt = std::max(out.max_alt, mesh_alt_index(mi.name));
    for (const auto& px : model->proxies) out.max_alt = std::max(out.max_alt, mesh_alt_index(px.name));
    for (uint32_t si : drawn) {
        const assets::Submesh& sm = model->submeshes[si];
        GpuSubmesh g;
        g.submesh = si;
        g.mesh = sm.mesh;
        g.lod = sm.mesh < model->meshes.size() ? model->meshes[sm.mesh].lod : -1;
        g.alt = sm.mesh < model->meshes.size() ? mesh_alt_index(model->meshes[sm.mesh].name) : -1;
        out.max_lod = std::max(out.max_lod, g.lod);
        // The EFFECTIVE vertex type (the effect's _ALAMO_VERTEX_TYPE override when the loader applied one, D.2).
        const std::string& vfmt = sm.vertex_type_overridden && !sm.effective_vertex_format.empty() ? sm.effective_vertex_format
                                                                                                    : sm.vertex_format;
        g.layout = vertex_layout_for(vfmt, sm.format);
        if (g.layout == materials::VertexLayout::Unknown || assets_layout_for(g.layout) != sm.layout) {
            log::warnf("render", "%s submesh %u: vertex format '%s' does not match its loaded layout; using the loaded one",
                       model->name.c_str(), si, vfmt.c_str());
            g.layout = sm.skinned() ? materials::VertexLayout::RSkinNU2 : materials::VertexLayout::NU2;
        }
        g.skinned = sm.skinned();
        assert(materials::vertex_layout_desc(g.layout).stride == assets::vertex_stride(sm.layout));
        assert(sm.vertices.size() == static_cast<size_t>(sm.vertex_count) * assets::vertex_stride(sm.layout));
        assert(sm.indices.size() == static_cast<size_t>(sm.index_count) * assets::index_size(sm.index_type));
        g.index_type = sm.index_type == assets::IndexType::U16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        g.index_count = sm.index_count;
        const std::string tn = diffuse_texture_name(sm.material);
        if (!tn.empty()) g.base_texture = add_texture(tn, assets::TextureRole::Color);
        setup_lit(*model, sm, g, add_texture);

        if (!alloc_->create_buffer(sm.vertices.size(), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   rhi::MemoryUsage::GpuOnly, g.vertices, "render.vb") ||
            !alloc_->create_buffer(sm.indices.size(), VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                   rhi::MemoryUsage::GpuOnly, g.indices, "render.ib")) {
            ok = false;
            out.submeshes.push_back(std::move(g));
            break;
        }
        const rhi::UploadTicket tv = up_->upload_buffer(g.vertices, 0, sm.vertex_bytes(), VK_PIPELINE_STAGE_2_VERTEX_ATTRIBUTE_INPUT_BIT,
                                                        VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT);
        const rhi::UploadTicket ti =
            up_->upload_buffer(g.indices, 0, sm.index_bytes(), VK_PIPELINE_STAGE_2_INDEX_INPUT_BIT, VK_ACCESS_2_INDEX_READ_BIT);
        if (!tv || !ti) ok = false;
        out.buffer_tickets.push_back(tv);
        out.buffer_tickets.push_back(ti);
        out.upload_bytes += sm.vertices.size() + sm.indices.size();
        out.submeshes.push_back(std::move(g));
    }

    out.textures.resize(reqs.size());
    std::vector<uint8_t> tex_ok(reqs.size(), 0);
    auto load_range = [&](uint32_t b, uint32_t e) {
        for (uint32_t i = b; i < e; ++i) {
            GpuTexture& t = out.textures[i];
            t.name = reqs[i].name;
            t.role = reqs[i].role;
            std::shared_ptr<const assets::TextureAsset> a = textures ? textures(t.name, t.role) : nullptr;
            assets::TextureAsset fb;
            if (!a) fb = assets::make_fallback_texture(t.role);
            tex_ok[i] = upload_texture(t, a ? *a : fb) ? 1 : 0;
        }
    };
    if (js && reqs.size() > 1) js->parallel_for(static_cast<uint32_t>(reqs.size()), 1, load_range);
    else load_range(0, static_cast<uint32_t>(reqs.size()));
    for (size_t i = 0; i < reqs.size(); ++i) {
        if (!tex_ok[i]) ok = false;
        const GpuTexture& t = out.textures[i];
        if (t.image) out.upload_bytes += rhi::mip_chain_size(t.image.format, t.image.width, t.image.height, t.image.mips);
    }
    out.bounds = out.cull_bounds();  // every uploaded mesh: an instance mask may show any of them
    if (!ok && error) *error = "GPU allocation or upload failed";
    return ok;
}

bool Renderer::make_resident(GpuModel& m) {
    if (!m.buffers_ready) {
        bool all = true;
        for (const rhi::UploadTicket& t : m.buffer_tickets) all = all && t && up_->ready(t);
        m.buffers_ready = all;
    }
    for (GpuTexture& t : m.textures) {
        if (t.resident || !t.image || !t.ticket || !up_->ready(t.ticket)) continue;
        // Fresh or retired slot: no frame in flight reads it (update-after-bind).
        t.slot = allocate_texture_slot(t.image.view, t.name.c_str());
        t.resident = true;
    }
    return m.resident();
}

bool Renderer::finish_uploads(const GpuModel& m) {
    if (!up_) return false;
    rhi::UploadTicket last;  // one timeline: finishing the highest value completes and acquires every lower one
    for (const rhi::UploadTicket& t : m.buffer_tickets)
        if (t.value > last.value) last = t;
    for (const GpuTexture& t : m.textures)
        if (t.ticket.value > last.value) last = t.ticket;
    return !last || up_->ready(last) || up_->finish(last);
}

uint32_t Renderer::allocate_texture_slot(VkImageView view, const char* what) {
    const uint32_t slot = table_.allocate(view);
    if (slot == rhi::kInvalidBindlessSlot) {
        log::warnf("render", "bindless texture table full (%u slots, %u pending); %s draws white", hot_.texture_capacity(),
                   table_.slots().pending(), what);
        return 0;
    }
    return slot;
}

void Renderer::release_texture(uint32_t slot, rhi::Image* image) {
    // retire_value() (current + 1) is safe from any thread (destroy_model may run off the begin_frame thread).
    const uint64_t retire = ring_ ? ring_->retire_value() : 0;
    if (slot != 0) table_.release(slot, retire);
    if (!ring_ && slot != 0) table_.collect(UINT64_MAX);  // no ring: GPU idle, the slot shows white at once
    if (!image || !*image) return;
    up_->forget(image->handle);
    if (!ring_ || (slot == 0 && retire <= ring_->completed_value())) {
        alloc_->destroy(*image);
        return;
    }
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_images_.push_back({*image, retire});
    *image = rhi::Image{};
}

uint32_t Renderer::collect_released() {
    const uint64_t done = ring_ ? ring_->completed_value() : UINT64_MAX;
    const uint32_t n = table_.collect(done);  // slots rewritten to white before their images go
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    size_t w = 0;
    for (size_t i = 0; i < deferred_images_.size(); ++i) {
        if (deferred_images_[i].retire <= done) alloc_->destroy(deferred_images_[i].image);
        else deferred_images_[w++] = deferred_images_[i];
    }
    deferred_images_.resize(w);
    w = 0;
    for (size_t i = 0; i < deferred_buffers_.size(); ++i) {
        if (deferred_buffers_[i].retire <= done) alloc_->destroy(deferred_buffers_[i].buffer);
        else deferred_buffers_[w++] = deferred_buffers_[i];
    }
    deferred_buffers_.resize(w);
    return n;
}

size_t Renderer::buffers_pending() {
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    return deferred_buffers_.size();
}

void Renderer::release_buffer(rhi::Buffer& b) {
    if (!b) return;
    up_->forget(b.handle);
    // Frames up to retire_value() may still read it (same rule as release_texture's images).
    const uint64_t retire = ring_ ? ring_->retire_value() : 0;
    if (!ring_ || retire <= ring_->completed_value()) {
        alloc_->destroy(b);
        return;
    }
    std::lock_guard<std::mutex> lock(deferred_mutex_);
    deferred_buffers_.push_back({b, retire});
    b = rhi::Buffer{};
}

void Renderer::destroy_model(GpuModel& m) {
    for (GpuSubmesh& g : m.submeshes) {
        release_buffer(g.vertices);
        release_buffer(g.indices);
    }
    // Slots released at the ring's current value; images destroyed once collect_released() retired them.
    for (GpuTexture& t : m.textures) release_texture(t.resident ? t.slot : 0u, &t.image);
    m = GpuModel{};
}

materials::PsoKey Renderer::draw_key_variant(const GpuSubmesh& g, const GpuPass& p, int variant) const {
    materials::PsoKey k = p.pass->pso[variant ? 1 : 0];
    k.render_pass = render_pass_;
    k.vertex_layout = g.layout;  // same attribute table as the effect's layout family (checked in setup_lit)
    if (p.ff) {
        const materials::Program prog = materials::ff_program(materials::ff_inputs_for_layout(g.layout));
        if (prog.vs) k.vs_id = prog.vs->id;
        if (prog.ps) k.ps_id = prog.ps->id;
    }
    return k;
}

materials::PsoKey Renderer::draw_key(const GpuSubmesh& g, const GpuPass& p, float w) const {
    return draw_key_variant(g, p, (p.pass->fade_dynamic && w < 1.0f) ? 1 : 0);
}

int Renderer::pass_variant(const GpuPass& p, const materials::ExprInputs& in) {
    return &p.pass->select_exact(in) == &p.pass->pso[1] ? 1 : 0;
}

void Renderer::prepare_pipelines(const GpuModel& m, float w) {
    for (const GpuSubmesh& g : m.submeshes)
        if (g.lit == LitStatus::Lit)
            for (const GpuPass& p : g.passes) {
                if (p.translated) {  // translation first; its pipelines are requested once the program exists
                    const materials::TranslatedProgram* tp = translated_mesh_program(g.effect, *p.pass);  // Q3 mesh form
                    if (!tp) continue;
                    (void)psos_.get(translated_key(g, p, *tp, (p.pass->fade_dynamic && w < 1.0f) ? 1 : 0));
                    if (p.pass->fade_dynamic || p.pass->fade_expr) (void)psos_.get(translated_key(g, p, *tp, 1));
                    continue;
                }
                (void)psos_.get(draw_key(g, p, w));
                if (p.pass->fade_dynamic || p.pass->fade_expr) (void)psos_.get(draw_key_variant(g, p, 1));
            }
}

}  // namespace alamo::render

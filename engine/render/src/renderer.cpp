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
#include "alamo/render/ssao.hpp"
#include "alamo/render/world.hpp"
#include "render_internal.hpp"
#include "fx2_reader.hpp"
#include "gamma.hpp"  // [port] output gamma
#include "taa.hpp"  // [port] TAA

// SPIR-V of shaders/unlit.hlsl (DXC -Fh byte arrays, see CMakeLists.txt).
#include "unlit_ps.h"
#include "unlit_vs.h"
#include "unlit_vs_skin.h"

namespace alamo::render {

namespace {

std::string lower(std::string_view s) {
    std::string r(s);
    for (char& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return r;
}

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

namespace {

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

}  // namespace

BlendMode blend_mode_for_key(const materials::PsoKey& k) {
    if (!k.blend_enable) return BlendMode::Opaque;
    return k.dst_color == materials::BlendFactor::One ? BlendMode::Additive : BlendMode::Alpha;
}

std::string diffuse_texture_name(const assets::MaterialRef& m) {
    for (const auto& p : m.params)
        if (p.type == assets::ParamType::Texture && !p.texture.empty() && lower(p.name) == "basetexture") return p.texture;
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
    render_pass_ = rp_scene_;

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
    render_pass_ = rp_scene_;  // lit keys follow automatically
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
    for (auto* set : {&unlit_, &unlit_int_})
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
    for (int set = 0; set < 2; ++set)
    for (int skinned = 0; skinned < 2; ++skinned) {
        VertexInputDesc vin;
        const materials::VertexLayout l = skinned ? materials::VertexLayout::RSkinNU2 : materials::VertexLayout::NU2;
        if (!build_vertex_input(l, spirv_input_locations(vs[skinned]), vin)) return false;
        // set 0: the caller's formats (draw()); set 1: the frame graph's scene depth-stencil (render()).
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
            d.layout = layout_;
            d.name = "render.unlit";
            VkPipeline& slot = set == 0 ? unlit_[skinned][b] : unlit_int_[skinned][b];
            slot = unlit_cache_.create_graphics(d);
            if (!slot) return false;
        }
    }
    return true;
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
        const std::string key = lower(name) + "#" + std::to_string(static_cast<int>(role));
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

// ------------------------------------------------------------------------------------ track B

namespace {
uint64_t xlat_key(uint32_t vs_id, uint32_t ps_id) { return (static_cast<uint64_t>(vs_id) << 32) | ps_id; }
// Translation table key of a pass in the default (cbuffer) or the Q3 instanced form.
uint64_t xlat_key_of(const materials::ResolvedPass& pass, bool instanced) {
    if (!instanced) return xlat_key(pass.pso[0].vs_id, pass.pso[0].ps_id);
    shaderxlat::TranslateOptions vo = pass.vs_options, po = pass.ps_options;
    vo.instanced = po.instanced = true;
    return xlat_key(materials::translated_program_id(pass.vs_bytecode, vo), materials::translated_program_id(pass.ps_bytecode, po));
}
}  // namespace

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
    if (desc_.js) desc_.js->spawn(*xlat_counter_, std::move(job), jobs::Priority::Background);
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

namespace {

// SHADOW_EXTRUSION_DISTANCE (FrameCB.shadow_params.x); the original's global, 200 world units.
constexpr float kShadowExtrusion = kShadowExtrusionDistance;

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

// Engine parameters of one draw in the ORIGINAL D3DX form (row-major, row-vector convention:
// the transpose of the column-vector matrices used elsewhere in the renderer), RENDER.md 2.2.
// The per-frame half lives in EngineFrame (once per build); this is the per-draw half.
struct EngineParams {
    const EngineFrame* f = nullptr;
    float world[16] = {}, world_view[16] = {}, wvp[16] = {}, world_inv[16] = {}, world_inv_t[16] = {}, world_view_inv[16] = {};
    float eye_obj[4] = {}, light_scale[4] = {}, l0_obj[4] = {};
    std::span<const Mat3x4> palette;  // skinned: world-space bone matrices in bone_palette order
};

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

bool iequal(std::string_view a, const char* b) {
    const size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i)
        if (std::toupper(static_cast<unsigned char>(a[i])) != static_cast<unsigned char>(b[i])) return false;
    return true;
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

// Where an engine parameter's value comes from: the semantic table first, then the light-0 / skinning names
// (only light 0 is uploaded by name, as in the original, RENDER.md 2.1), else the ALO material value (Store).
enum class Src : uint8_t {
    Store, World, View, Proj, ViewProj, WorldView, Wvp, ViewInv, WorldInv, WorldInvT, WorldViewInv, Eye, EyeObj,
    LightScale, Time, Resolution, Fog, Fade, SphAll, SphFill, Extrusion, CloudU, CloudV, FowU, FowV, Zero, Skin,
    Ambient, L0Vec, L0Dif, L0Spec, L0View, L0Obj
};

Src semantic_src(const std::string& s) {
    if (s.empty()) return Src::Store;
    if (iequal(s, "WORLD") || iequal(s, "MODEL")) return Src::World;
    if (iequal(s, "VIEW")) return Src::View;
    if (iequal(s, "PROJECTION")) return Src::Proj;
    if (iequal(s, "VIEWPROJECTION")) return Src::ViewProj;
    if (iequal(s, "WORLDVIEW") || iequal(s, "MODELVIEW")) return Src::WorldView;
    if (iequal(s, "WORLDVIEWPROJECTION") || iequal(s, "MODELVIEWPROJECTION")) return Src::Wvp;
    if (iequal(s, "VIEWINVERSE")) return Src::ViewInv;
    if (iequal(s, "WORLDINVERSE") || iequal(s, "MODELINVERSE")) return Src::WorldInv;
    if (iequal(s, "WORLDINVERSETRANSPOSE") || iequal(s, "MODELINVERSETRANSPOSE")) return Src::WorldInvT;
    if (iequal(s, "WORLDVIEWINVERSE")) return Src::WorldViewInv;
    if (iequal(s, "EYE_POSITION")) return Src::Eye;
    if (iequal(s, "EYE_OBJ_POSITION")) return Src::EyeObj;
    if (iequal(s, "LIGHT_SCALE")) return Src::LightScale;
    if (iequal(s, "TIME")) return Src::Time;
    if (iequal(s, "RESOLUTION_CONSTANTS")) return Src::Resolution;
    if (iequal(s, "FOG_VALS")) return Src::Fog;
    if (iequal(s, "DISTANCE_FADE_VALS")) return Src::Fade;
    if (iequal(s, "SPH_LIGHT_ALL")) return Src::SphAll;
    if (iequal(s, "SPH_LIGHT_FILL")) return Src::SphFill;
    if (iequal(s, "SHADOW_EXTRUSION_DISTANCE")) return Src::Extrusion;
    if (iequal(s, "CLOUD_TEX_U")) return Src::CloudU;
    if (iequal(s, "CLOUD_TEX_V")) return Src::CloudV;
    if (iequal(s, "FOW_TEX_U")) return Src::FowU;
    if (iequal(s, "FOW_TEX_V")) return Src::FowV;
    if (iequal(s, "WIND_BEND_VECTOR") || iequal(s, "WIND_GRASS_PARAMS")) return Src::Zero;
    return Src::Store;
}

Src param_src(const fx::Parameter& p) {
    const Src s = semantic_src(p.type.semantic);
    if (s != Src::Store) return s;
    const std::string& n = p.type.name;
    if (iequal(n, "SKINMATRIXARRAY")) return Src::Skin;
    if (iequal(n, "GLOBAL_AMBIENT")) return Src::Ambient;
    if (iequal(n, "DIR_LIGHT_VEC_0")) return Src::L0Vec;
    if (iequal(n, "DIR_LIGHT_DIFFUSE_0")) return Src::L0Dif;
    if (iequal(n, "DIR_LIGHT_SPECULAR_0")) return Src::L0Spec;
    if (iequal(n, "DIR_LIGHT_VIEW_VEC_0")) return Src::L0View;
    if (iequal(n, "DIR_LIGHT_OBJ_VEC_0")) return Src::L0Obj;
    return Src::Store;
}

// Parameters whose value depends on the instance (transform, LIGHT_SCALE, pose, team colour): rewritten per
// draw; everything else is per material or per frame and lives in the pass's cached register images.
bool draw_param(const fx::Parameter& p) {
    if (iequal(p.type.name, "COLORIZATION")) return true;  // team colour override (checked before the engine table)
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
        const float x[4] = {kShadowExtrusion, kShadowExtrusion, kShadowExtrusion, kShadowExtrusion};
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


// The lookup of one translated draw: the instance's team colour for COLORIZATION, then the engine table, then the
// ALO material values. A single captured pointer, so the std::function never allocates.
struct DrawLookupCtx {
    const EngineParams* e = nullptr;
    const materials::ParamStore* store = nullptr;
    const Instance* inst = nullptr;  // null: no team colour override (templates)
    // Optional: Src of every parameter of `fx` (bit 7: named COLORIZATION), precomputed per pass (XTemplate::src),
    // so per-draw lookups skip the name / semantic comparisons.
    const fx::Effect* fx = nullptr;
    const uint8_t* src = nullptr;
};
constexpr uint8_t kSrcColorization = 0x80;
uint8_t src_code(const fx::Parameter& p) {
    return static_cast<uint8_t>(static_cast<uint8_t>(param_src(p)) | (iequal(p.type.name, "COLORIZATION") ? kSrcColorization : 0));
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

}  // namespace

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
        cb.shadow_params.x = kShadowExtrusion;
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




namespace {

// Every corner of the box deeper than `depth` along the view direction (camera looks along -Z).
bool beyond_depth(const Mat4& view, const assets::Aabb& b, float depth) {
    for (int i = 0; i < 8; ++i) {
        const Float4 p{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z, 1.0f};
        if (-(view * p).z <= depth) return false;
    }
    return true;
}

bool outside_frustum(const Mat4& vp, const assets::Aabb& b) {
    int out[6] = {0, 0, 0, 0, 0, 0};
    for (int i = 0; i < 8; ++i) {
        const Float4 p{(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z, 1.0f};
        const Float4 c = vp * p;
        out[0] += c.x < -c.w;
        out[1] += c.x > c.w;
        out[2] += c.y < -c.w;
        out[3] += c.y > c.w;
        out[4] += c.z < 0.0f;
        out[5] += c.z > c.w;
    }
    for (int k = 0; k < 6; ++k)
        if (out[k] == 8) return true;
    return false;
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

}  // namespace

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
                    (opts.coverage_alpha ? 16 : 0);
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
    for_range(desc_.js, par, n_inst, 32, [&](uint32_t b, uint32_t e) {
        for (uint32_t ii = b; ii < e; ++ii) {
            if (inst_model[ii] == UINT32_MAX) continue;
            const Instance& in = instances[ii];
            InstInfo& o = info[ii];
            ++o.st.instances;
            const GpuModel& gm = *in.model;
            const assets::Aabb box = gm.bounds.valid() ? assets::transform_aabb(in.transform, gm.bounds) : assets::Aabb{};
            if (opts.frustum_cull && box.valid() && outside_frustum(view.view_proj, box)) {
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
                    out.items[it++] = {gi, -1, (external_ ? unlit_ : unlit_int_)[g.skinned ? 1 : 0][static_cast<int>(bm)],
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

namespace {
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
}  // namespace

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
    render_pass_ = rp_scene_;
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

// ------------------------------------------------------------------------------------ frame graph

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
    if (!fx_post_[which] && desc_.effects) fx_post_[which] = desc_.effects->get(kNames[which]);
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

struct Renderer::PostCacheEntry {
    uint64_t digest = 0;
    uint32_t last_use = 0;
    bool valid = false;  // images filled for `digest`
    XConstCache cc;
};

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
    key.render_pass = rp_scene_;
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
    auto b8 = [](float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
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
    const materials::ParamStore store;
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
    if (!ensure_targets(target.extent)) return false;
    // [port] TAA (taa.cpp): every scene draw of the frame uses the sub-pixel jittered camera; without TAA `view_in` itself.
    const ViewMatrices taa_view = taa_begin(view_in, opts);
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
    rhi::RenderTarget& scene = tg_.scene;
    const uint32_t W = scene.extent().width, H = scene.extent().height;
    scene.set_color_state(rhi::ImageState::Undefined);  // contents of the last frame are discarded
    scene.set_depth_state(rhi::ImageState::Undefined);
    const bool depth = scene.has_depth();
    const float clear[4] = {target.clear[0], target.clear[1], target.clear[2], 1.0f};  // frame clear alpha is 0xFF
    const bool shadows_on = opts.shadows && opts.shader_detail > 0 && !lighting_.space && ds_format_ != VK_FORMAT_UNDEFINED;
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
            inh.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
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
        const bool wok = world_prepass(frame, primary, view, instances, sky, mode, opts);
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
            // StencilDarkenFinalBlur: back buffer -> SceneTexture, 4 taps, ZERO / SRCCOLOR (4.3).
            tg_.scene_tex.copy_from(fn, primary, copy_of(scene, rhi::ImageState::ColorAttachment), VK_FILTER_NEAREST,
                                    rhi::ImageState::ShaderRead);
            scene.set_color_state(rhi::ImageState::ColorAttachment);
            const materials::MaterialEffect* fb = post_effect(2);
            ok = false;
            if (fb)
                for (const auto& p : fb->passes)
                    if (!p.restore_only && p.program_status == materials::ProgramStatus::Translated) {
                        PostSpec ps;
                        ps.fx = fx_post_[2];
                        ps.pass = &p;
                        ps.render_pass = rp_scene_;
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
        ssao_->record(frame, primary, scene, desc_.color_format, view, opts.ao_strength, opts.ao_radius, 1.0f, gt);
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
    // 5. Post: SceneTexture copy after Transparent, heat, bloom (RENDER.md 1.4/1.5, 4.4/4.5).
    const auto tp0 = clock::now();
    const uint32_t post_scope = gt ? gt->begin_scope(primary, "post") : 0u;
    if (heat_on || bloom_on) {
        tg_.scene_tex.copy_from(fn, primary, copy_of(scene, rhi::ImageState::ColorAttachment), VK_FILTER_NEAREST,
                                rhi::ImageState::ShaderRead);
        scene.set_color_state(rhi::ImageState::ColorAttachment);
    }
    if (heat_on) {
        // Heat bucket into the back buffer cleared to 0x008080FF (A 0, R G 128, B 255), scene depth kept.
        const float heat_clear[4] = {128.0f / 255.0f, 128.0f / 255.0f, 1.0f, 0.0f};
        begin_target(fn, primary, scene, depth, heat_clear, false, false);
        record_range(primary, b, h0, b.items.size(), W, H, true);
        if (hook_heat) hook_pass(PhaseBucket::Heat, false);  // R4: heat particles after the heat meshes
        fn.vkCmdEndRendering(primary);
        count_items(b, h0, b.items.size(), st);
        tg_.distortion.copy_from(fn, primary, copy_of(scene, rhi::ImageState::ColorAttachment), VK_FILTER_NEAREST,
                                 rhi::ImageState::ShaderRead);
        scene.set_color_state(rhi::ImageState::ColorAttachment);
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
                    begin_target(fn, primary, scene, false, nullptr, false, false);
                    ok = post_translated(frame, primary, ps);
                    fn.vkCmdEndRendering(primary);
                    break;
                }
        if (!ok) {  // composite not ready: put the pre-heat scene back
            scene.copy_from(fn, primary, copy_of(tg_.scene_tex, rhi::ImageState::ShaderRead), VK_FILTER_NEAREST,
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
            ok = bloom_pass(bp[2], scene, last->color().view, 0.0f, rp_post_);
        }
        st.bloom = ok;
        if (!ok) ++st.post_pending;
    }
    if (opts.taa && taa_) {  // [port] TAA resolve + sharpen at the end of the post chain (taa.cpp)
        const uint32_t taa_scope = gt ? gt->begin_scope(primary, "taa") : 0u;
        st.taa = taa_->resolve(frame, primary, scene, opts.taa_sharpen, opts.taa_weight >= 0.0f ? opts.taa_weight : kTaaCurrentWeight);
        st.taa_reset = st.taa && taa_->history_reset();
        if (gt) gt->end_scope(primary, taa_scope);
    }
    if (gt) gt->end_scope(primary, post_scope);
    const auto tp1 = clock::now();
    // 6. Out to the caller's image (StretchRect, point filter).
    rhi::CopyImage dst;
    dst.image = target.color;
    dst.extent = target.extent;
    dst.format = desc_.color_format;
    dst.before = rhi::ImageState::Undefined;
    dst.after = target.color_after;
    rhi::cmd_stretch_rect(fn, primary, copy_of(scene, rhi::ImageState::ColorAttachment), dst, VK_FILTER_NEAREST);
    scene.set_color_state(rhi::ImageState::ColorAttachment);
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

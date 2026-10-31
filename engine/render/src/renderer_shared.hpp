// Renderer internals shared by renderer.cpp, renderer_xlat.cpp, renderer_draw.cpp and renderer_graph.cpp (the four files
// renderer.cpp was split into). Not part of the render module's public headers.
#pragma once

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

namespace alamo::render::detail {

// SHADOW_EXTRUSION_DISTANCE (FrameCB.shadow_params.x); the original's global, 200 world units.
inline constexpr float kShadowExtrusion = kShadowExtrusionDistance;

// Engine parameters of one draw in the ORIGINAL D3DX form (row-major, row-vector convention:
// the transpose of the column-vector matrices used elsewhere in the renderer), RENDER.md 2.2.
// The per-frame half lives in EngineFrame (once per build); this is the per-draw half.
struct EngineParams {
    const EngineFrame* f = nullptr;
    float world[16] = {}, world_view[16] = {}, wvp[16] = {}, world_inv[16] = {}, world_inv_t[16] = {}, world_view_inv[16] = {};
    float eye_obj[4] = {}, light_scale[4] = {}, l0_obj[4] = {};
    std::span<const Mat3x4> palette;  // skinned: world-space bone matrices in bone_palette order
};

// Where an engine parameter's value comes from: the semantic table first, then the light-0 / skinning names
// (only light 0 is uploaded by name, as in the original, RENDER.md 2.1), else the ALO material value (Store).
enum class Src : uint8_t {
    Store, World, View, Proj, ViewProj, WorldView, Wvp, ViewInv, WorldInv, WorldInvT, WorldViewInv, Eye, EyeObj,
    LightScale, Time, Resolution, Fog, Fade, SphAll, SphFill, Extrusion, CloudU, CloudV, FowU, FowV, Zero, Skin,
    Ambient, L0Vec, L0Dif, L0Spec, L0View, L0Obj
};

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
inline constexpr uint8_t kSrcColorization = 0x80;

Mat3x4 static_mesh_world(const Instance& inst, const assets::ModelAsset& ma, std::span<const Mat3x4> bw, uint32_t bone,
    const ViewMatrices& view, const Float3& sun);
uint64_t xlat_key_of(const materials::ResolvedPass& pass, bool instanced);
void distance_fade_vals(float begin, float end, float& x, float& y);
void make_engine_draw(const Mat3x4& world3, const Instance& inst, const ViewMatrices& v, const EngineFrame& f, EngineParams& e);
Src param_src(const fx::Parameter& p);
bool engine_write(const EngineParams& e, const materials::ParamStore* store, const fx::Parameter& p, std::span<float> out, Src src);
materials::ParamLookup draw_lookup(const DrawLookupCtx* c);
void translated_draw_params(const Instance& inst, const GpuSubmesh& g, const ViewMatrices& view, const EngineFrame& f,
    std::vector<Mat3x4>& pal, EngineParams& e);
void xt_analyse(Renderer::XTemplate& t, const fx::Effect& fxe, const materials::ResolvedPass& rp);
void mask_frame(const EngineFrame& f, uint64_t mask, EngineFrame& out);
void xt_fill_slot(const Renderer::XTemplate& t, Renderer::XTemplate::Slot& s, const EngineFrame& masked, const fx::Effect& fxe,
    const materials::ResolvedPass& rp, const materials::ParamStore* store, bool int_bool);
void xt_images(const Renderer::XTemplate& t, const Renderer::XTemplate::Slot& s, const fx::Effect& fxe,
    const materials::ResolvedPass& rp, const materials::ParamLookup& lookup, materials::VSConstants* vs,
    materials::PSConstants* ps, materials::IntBoolConstants* vib, materials::IntBoolConstants* pib);
materials::CullMode mirrored_cull(materials::CullMode c);
bool is_fow_binding(const materials::SamplerBinding& sb);
void count_items(const Renderer::Built& bl, size_t b, size_t e, DrawStats& st);
size_t bucket_begin(const std::vector<Renderer::Built::Item>& items, PhaseBucket b);

}  // namespace alamo::render::detail

namespace alamo::render {

// Complete here (not in renderer_graph.cpp) because Renderer's destructor in renderer.cpp frees post_cache_.
struct Renderer::PostCacheEntry {
    uint64_t digest = 0;
    uint32_t last_use = 0;
    bool valid = false;  // images filled for `digest`
    XConstCache cc;
};

}  // namespace alamo::render

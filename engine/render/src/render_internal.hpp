// alamo_render - definitions shared by renderer.cpp and world.cpp (not part of the public API).
#pragma once
#include <algorithm>
#include <climits>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/render/renderer.hpp"

namespace alamo::render {

// Q3: PostSpec inputs are non-owning views (names are literals / long-lived strings, values the caller's arrays), so
// building a spec per draw / per frame allocates nothing.
struct PostValue {
    std::string_view name;
    std::span<const float> v;
};
struct PostTexture {
    std::string_view name;
    VkImageView view = VK_NULL_HANDLE;
};

struct Renderer::PostSpec {
    std::shared_ptr<const materials::MaterialEffect> fx;
    const materials::ResolvedPass* pass = nullptr;
    uint8_t render_pass = 0;
    uint32_t width = 0, height = 0;                            // bound render target (RESOLUTION_CONSTANTS)
    std::span<const PostValue> values;                         // material parameters by name (a later entry wins)
    // Values that change per draw while the rest stays (terrain blendTexScale / Offset): patched into the
    // cached register images by CTAB register (full refill when the shader has a preshader). They win over `values`.
    std::span<const PostValue> patch_values;
    std::span<const PostTexture> textures;                     // by texture parameter name or semantic
    VkImageView slot_default = VK_NULL_HANDLE;                          // samplers without a texture parameter
    materials::SamplerKey sampler_override;
    bool override_sampler = false;
    // Prim-mode pass (StencilDarkenFinalBlur, prim mode 9): the frame's 6 pre-lit PrimVert vertices written by
    // post_darken (pos, shadow colour, uv) under the orthographic 2D camera instead of the FLOAT2 quad.
    bool prim = false;
    // Geometry draw (terrain / water patches): own camera, vertex + index buffer, identity world.
    const ViewMatrices* view = nullptr;
    materials::VertexLayout layout = materials::VertexLayout::Unknown;
    VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    VkDeviceSize vb_offset = 0, ib_offset = 0;  // into the world's merged vertex / index buffers
    uint32_t index_count = 0;
    bool alpha_blend = false;   // force SRC_ALPHA / ONE_MINUS_SRC_ALPHA (the terrain renderer's per-pass state)
    bool mirrored = false;      // swap the cull face (mirrored camera)
    bool fog = false;           // scene fog in the PS epilogue
    VkIndexType index_type = VK_INDEX_TYPE_UINT16;
    bool cull_none = false;     // decorations: no culling
    bool no_depth_write = false;  // decorations: blended, never write depth
};

// One fixed-function draw of render-owned geometry (Renderer::ff_geometry_draws).
struct Renderer::FfGeoDraw {
    std::shared_ptr<const materials::MaterialEffect> fx;
    const materials::ResolvedPass* pass = nullptr;      // FF pass (pass->ff)
    const materials::FfDrawTemplate* tpl = nullptr;     // built from pass->ff (vertex colour on)
    materials::VertexLayout layout = materials::VertexLayout::PrimVertLit;
    VkBuffer vb = VK_NULL_HANDLE, ib = VK_NULL_HANDLE;
    VkDeviceSize vb_offset = 0;
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    uint32_t first_index = 0, index_count = 0;
    uint32_t tex[3] = {0, 0, 0};                         // bindless slot per stage (0 = white)
    uint32_t sampler[3] = {0, 0, 0};                     // EngineSampler per stage
    bool scroll = false;                                 // stage-0 texture matrix = translation (u, v)
    float uv_offset[2] = {0, 0};
    bool stage0_rows = false;                            // stage-0 texture matrix = these register rows (COUNT2)
    materials::float4 rows[4] = {};
    bool cull_none = true;
    bool no_depth_write = true;
    bool fog = true;
};

// Register images of the last post_translated() call for one (pass, generation): reused while only
// PostSpec::patch_values change.
struct Renderer::XConstCache {
    const materials::ResolvedPass* pass = nullptr;
    uint64_t generation = 0, filled_generation = ~0ull;
    materials::VSConstants vs{};
    materials::PSConstants ps{};
    materials::IntBoolConstants vib{}, pib{};
    // Pipeline + program + samplers resolved for this (pass, generation).
    uint64_t state_generation = ~0ull;
    const materials::TranslatedProgram* tp = nullptr;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkSampler samplers[16] = {};
    uint32_t sampler_aniso = 0;  // [port] the anisotropy level `samplers` were picked with
};

// Resolved state of one translated draw (Renderer::translated_state): what every draw of a batched world
// call shares.
struct Renderer::GeoState {
    const materials::TranslatedProgram* tp = nullptr;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkSampler samplers[16] = {};
    bool int_bool = false;
    shaderxlat::TranslatedPush push{};
};

// Per-frame half of the engine parameters of translated draws (D3DX row-major form, RENDER.md 2.2): everything
// that does not depend on the instance. Computed once per build; also the key of the cached register images
// (XTemplate), compared bytewise. Floats only (no padding).
struct EngineFrame {
    float view[16], proj[16], view_proj[16], view_inv[16];
    float eye[4], time4[4], resolution[4], fog[4], fade[4], ambient[4];
    float l0_vec[4], l0_dif[4], l0_spec[4], l0_view[4];
    float cloud_u[4], cloud_v[4], fow_u[4], fow_v[4];
    materials::float4 sph_all[12], sph_fill[12];
};

// Register images of one translated (material, pass) - built once per frame inputs, then only the per-draw
// parameters (WORLD*, EYE_OBJ_POSITION, LIGHT_SCALE, DIR_LIGHT_OBJ_VEC_0, SKINMATRIXARRAY, COLORIZATION) are
// rewritten per draw (Renderer::build). Images whose bindings are all per material / per frame are uploaded
// once per build and shared by every draw of the pass, like the combined-sampler set 1.
struct Renderer::XTemplate {
    const GpuModel* model = nullptr;  // owner (per-model memo entry)
    const GpuSubmesh* g = nullptr;
    const GpuPass* pass = nullptr;
    const materials::TranslatedProgram* tp = nullptr;  // set once translated
    // Static split of the pass's plans (once).
    bool analysed = false;
    bool full = false;                     // overlapping bindings / oversized values: full fill per draw
    std::vector<uint32_t> vs_draw, ps_draw;  // per-draw float bindings (indices into plan.floats, plan order)
    bool vs_pre = false, ps_pre = false;   // preshader re-run per draw (per-draw input or overlap)
    bool bump = false;                     // fill_bump_env re-run per draw
    bool ib_draw = false;                  // int / bool constants per draw
    bool vs_static = false, ps_static = false;  // no per-draw part at all
    uint64_t frame_mask = 0;  // EngineFrame inputs the per-material part reads (bit = renderer.cpp Src)
    std::vector<uint8_t> src;  // per effect parameter: its value source (renderer.cpp src_code)
    const materials::ParamStore* params = nullptr;  // the material values the images were built from
    // Frame-dependent images for up to two frame inputs (e.g. a view used by two builds of one frame).
    struct Slot {
        bool valid = false;
        EngineFrame frame{};  // only the frame_mask fields (the rest zero): the slot's key
        materials::VSConstants vs{};
        materials::PSConstants ps{};
        materials::IntBoolConstants vib{}, pib{};
        materials::PreshaderBake vs_bake, ps_bake;  // vs_pre / ps_pre: static preshader inputs packed
    };
    Slot slots[2];
    uint32_t next = 0;
    const Slot* cur = nullptr;
    // Per build (Renderer::build's prep pass): shared ring ranges and descriptor sets, base push block.
    uint32_t stamp = 0;
    bool ok = false;
    rhi::BufferRange vs_r{}, ps_r{}, vib_r{}, pib_r{};
    VkDescriptorSet set0 = VK_NULL_HANDLE;  // shared set 0 when VS, PS and int/bool images are all static
    VkDescriptorSet set1 = VK_NULL_HANDLE;  // combined samplers
    shaderxlat::TranslatedPush push{};
};

struct Renderer::Built {
    struct Group {  // one (instance, submesh): transform + palette slice
        const Instance* inst;
        uint32_t inst_index;
        const GpuSubmesh* g;
        Mat3x4 world;
        uint32_t bone_base, bone_count;
        float depth_key;
        uint32_t rank;      // effect rank for the opaque sort (D.1 creation order, then name)
        uint32_t mat_rank;  // material-hash rank (same order as GpuSubmesh::material_hash)
    };
    struct Item {
        uint32_t group;
        int pass;  // -1 = unlit
        VkPipeline pipeline;
        PhaseBucket bucket;
        bool fading;
        uint64_t key_hash;
        const materials::TranslatedProgram* tp;  // translated pass (own layout, sets, push block)
        XTemplate* xt;                           // translated: the pass's cached register images
    };
    std::vector<Group> groups;
    std::vector<Item> items;
    std::vector<Item> items_tmp;  // sort scratch (swapped with items)
    // Instanced draws: one past the last item of the run containing item i. A run = consecutive items that
    // differ only in their DrawData (same pipeline, submesh, pass, fading, bucket; not translated, not the
    // back-to-front Transparent / Heat buckets); record_range draws it with one vkCmdDrawIndexed whose
    // instances read DrawData[draw_index + SV_InstanceID]. Same order, same rasterisation as one draw per item.
    std::vector<uint32_t> run_end;
    VkDescriptorSet set0 = VK_NULL_HANDLE;
    VkDescriptorSet set0_shadow = VK_NULL_HANDLE;  // Shadow bucket: z-biased projection, extrusion distance
    // Translated items: per-draw sets + push block (other items leave them empty).
    std::vector<VkDescriptorSet> xset0, xset1;
    std::vector<shaderxlat::TranslatedPush> xpush;
    bool fog = false;  // scene fog of the lighting the list was built with (hot-set / FF push flag)
    double cull_ms = 0, sort_ms = 0, fill_ms = 0;
};

// Open-addressing pointer -> index map (lookup only, never iterated, so its layout cannot affect any output).
// Allocates only when it grows.
struct PtrIndex {
    struct Slot {
        const void* key = nullptr;
        uint32_t value = 0;
    };
    std::vector<Slot> slots;
    size_t count = 0;
    static size_t hash(const void* p) {
        uint64_t x = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(p));
        x ^= x >> 33;
        x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33;
        return static_cast<size_t>(x);
    }
    // The value of `key`; a new key gets `fresh` (inserted = true).
    uint32_t& find_or_insert(const void* key, uint32_t fresh, bool& inserted) {
        if ((count + 1) * 2 > slots.size()) grow();
        const size_t mask = slots.size() - 1;
        size_t i = hash(key) & mask;
        while (slots[i].key && slots[i].key != key) i = (i + 1) & mask;
        inserted = slots[i].key == nullptr;
        if (inserted) {
            slots[i] = {key, fresh};
            ++count;
        }
        return slots[i].value;
    }
    void grow() {
        std::vector<Slot> old = std::move(slots);
        slots.assign(std::max<size_t>(64, old.size() * 2), Slot{});
        count = 0;
        bool ins = false;
        for (const Slot& s : old)
            if (s.key) find_or_insert(s.key, s.value, ins);
    }
    void clear() {
        slots.clear();
        count = 0;
    }
};

// build() state kept across frames (frame thread only; builds never overlap).
struct Renderer::BuildCache {
    // ---- Per-model memo (persistent): pipelines + key hashes of every lit pass for both LIGHT_SCALE variants,
    // translated programs and their register-image templates. One set per build configuration (draw mode,
    // render pass, mirrored culling). An entry is rebuilt when its GpuModel was (re-)uploaded (GpuModel::uid),
    // and re-resolved every build until every pipeline / translation it waits for exists.
    struct PassMemo {
        VkPipeline pipe[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        uint64_t hash[2] = {0, 0};
        const materials::TranslatedProgram* tp = nullptr;  // translated passes, null while translating
        std::unique_ptr<XTemplate> xt;                     // translated passes
    };
    struct SubMemo {
        uint32_t first_pass = 0;
        bool lit = false;  // lit path wanted (pipelines may still be compiling)
        bool translating = false, no_translation = false;
        uint32_t rank = 0;      // effect rank (D.1) among the build's models
        uint32_t mat_rank = 0;  // rank of GpuSubmesh::material_hash among the build's models
    };
    struct ModelMemo {
        const GpuModel* model = nullptr;
        uint64_t uid = 0;
        bool built = false, resolved = false;
        uint32_t stamp = 0;  // build that listed it
        std::vector<SubMemo> subs;
        std::vector<PassMemo> passes;
    };
    struct MemoSet {
        uint8_t render_pass = 0;
        PtrIndex index;                  // GpuModel* -> memos
        std::vector<ModelMemo> memos;
        std::vector<uint32_t> models, last_models;  // memo slots of this / the previous build, first-appearance order
        uint32_t effect_count = 0, mat_count = 0, max_passes = 0;
        bool ranks_valid = false;
    };
    static constexpr int kSets = 64;  // Lit/Unlit x scene/external render pass x mirrored x background (build_sky) x coverage alpha
                                      // x [port] multisampled main scene (MSAA)
    MemoSet sets[kSets];
    uint32_t stamp = 0;
    // ---- Scratch (sizes only grow).
    std::vector<const GpuSubmesh*> effect_names;
    std::vector<uint32_t> inst_model;  // instance -> memo slot (UINT32_MAX: not drawn)
    std::vector<uint32_t> sub_off, sub_state, goff, ioff, boff;
    std::vector<materials::MeshMaterial> mats, records;
    std::vector<uint32_t> ff_slot, mat_index;
    std::vector<materials::FfDrawData> ff_tmp;
    std::vector<uint64_t> mat_hash;
    struct InternSlot {
        uint32_t stamp = 0, index = 0;
    };
    std::vector<InternSlot> intern;  // material hash -> first record (open addressing, stamped per build)
    uint32_t intern_stamp = 0;
    std::vector<XTemplate*> xts;     // translated templates used by the build, first-use order
    // Q3: per item, its slot in the build's storage block of per-draw VS / PS register images (instanced translated
    // form; UINT32_MAX = none), assigned in item order so an instanced run's images are consecutive.
    std::vector<uint32_t> xslot_vs, xslot_ps;
    std::vector<std::vector<Mat3x4>> palettes;  // per job-system slot (skin palettes)
    struct InstStats {  // the DrawStats counters A1 produces (compact: 2000+ of these are zeroed and summed per frame)
        uint32_t instances = 0, culled = 0, not_drawn = 0, no_effect = 0, untranslated = 0, layout_mismatch = 0;
        uint32_t pending_pipeline = 0, translation_pending = 0, no_translation = 0;
    };
    struct InstInfo {
        int lod = INT32_MIN;  // INT32_MIN: not drawn (no model / culled)
        uint32_t groups = 0, items = 0, bones = 0;
        InstStats st;
    };
    std::vector<InstInfo> info;
};

namespace detail {
// Dynamic rendering over a renderer target: colour (+ depth-stencil), clear or load, Y-flipped viewport
// unless the contents are secondaries (they set their own).
void begin_target(const rhi::DeviceFns& fn, VkCommandBuffer cmd, rhi::RenderTarget& t, bool depth, const float* clear_color,
                  bool clear_depth, bool secondaries);
rhi::CopyImage copy_of(const rhi::RenderTarget& t, rhi::ImageState after);
// [port] MSAA: resolve the multisampled `ms` (colour average into `color_dst`, depth sample 0 into `depth_dst`; either may be
// null) with an empty dynamic-rendering pass. Targets are left as attachments.
void resolve_into(const rhi::DeviceFns& fn, VkCommandBuffer cmd, rhi::RenderTarget& ms, rhi::RenderTarget* color_dst,
                  rhi::RenderTarget* depth_dst);
// First item index whose bucket is >= b (items are sorted by bucket).
size_t bucket_begin(const std::vector<Renderer::Built::Item>& items, PhaseBucket b);
}  // namespace detail

}  // namespace alamo::render

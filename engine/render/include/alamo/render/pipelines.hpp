// alamo_render - materials effects -> Vulkan pipelines.
//
// EffectCache: thread-safe front of materials::EffectLibrary (FXO through the VFS, resolved
//   passes, PsoKeys). LoadOptions::ff_fallback_for_untranslated is on: effects whose best technique
//   needs track-B translation use their FIXEDFUNCTION technique through the FF ubershader.
// pipeline_desc_from_key(): PsoKey -> rhi::GraphicsPipelineDesc (blend, depth, stencil, raster/cull,
//   vertex input from materials::vertex_layout_desc() restricted to the VS inputs found in the
//   SPIR-V, SPIR-V from the program ids, HotSetLayouts pipeline layout). PsoKey::render_pass holds
//   render_pass_id(colour, depth format).
// PipelineManager: PsoKey -> VkPipeline map. A key seen for the first time is created on the job
//   system (background) through rhi::PipelineCache; get() returns VK_NULL_HANDLE until it is ready
//   and never blocks. Persistence (when a directory is given):
//     <dir>\alamo_pso_<vendor>_<device>.keys     every PsoKey created (reloaded and pre-created at init)
//     <dir>\alamo_pso_<vendor>_<device>.vkcache  the VkPipelineCache blob (rhi::PipelineCache)
//   Both are discarded when materials::programs_fingerprint(), the driver version, the device or
//   the pipeline-cache UUID differ from the .keys header. Default directory (app):
//   %LOCALAPPDATA%\alamo\render\ (default_pipeline_cache_dir()).
#pragma once
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "alamo/assets/model.hpp"
#include "alamo/materials/effect_loader.hpp"
#include "alamo/materials/pso.hpp"
#include "alamo/materials/translated.hpp"
#include "alamo/rhi/rhi.hpp"

namespace alamo {
class FileProvider;
}
namespace alamo::jobs {
class JobSystem;
class JobCounter;
}  // namespace alamo::jobs

namespace alamo::render {

// "MeshBumpColorize.fx" / "Data\Art\Shaders\MESHBUMPCOLORIZE.FXO" -> "MESHBUMPCOLORIZE".
std::string effect_key(std::string_view effect);
// True when technique `technique` of `e` carries a VENDOR_FILTER_0..9 annotation (int, or a decimal string) equal
// to `vendor_id` (RENDER.md "Technique selection"). False for vendor_id 0 or an index out of range.
bool technique_vendor_filtered(const fx::Effect& e, size_t technique, uint32_t vendor_id);

class EffectCache {
public:
    // files may be null: only effects added with add() resolve.
    explicit EffectCache(const FileProvider* files, materials::LoadOptions options = default_options());
    ~EffectCache();
    static materials::LoadOptions default_options();

    // Thread-safe; loads once per key (failures cached). nullptr when missing / malformed.
    std::shared_ptr<const materials::MaterialEffect> get(std::string_view effect, std::string* err = nullptr);
    // Submesh effects (SPACE_AND_OPEN.md D.3): get(), but a missing / failing effect resolves to the built-in
    // green TNoShader default (EffectLibrary::load_or_default, is_default_effect()). nullptr only without a
    // file provider. Post / engine effects keep using get() (nullptr = pass skipped).
    std::shared_ptr<const materials::MaterialEffect> get_or_default(std::string_view effect);
    // get(), with the original's vendor filter applied (RENDER.md "Technique selection": a technique is rejected
    // when one of its VENDOR_FILTER_0..9 int annotations equals the adapter's PCI vendor id). When the technique
    // get() picked is rejected for `vendor_id`, the best remaining technique (same rule as
    // materials::pick_best_technique) is resolved instead and cached per (name, vendor); otherwise get()'s effect
    // is returned. vendor_id 0 = no filter. In the vanilla shaders only SceneHeat's DX9 technique matters: t0 has
    // VENDOR_FILTER_0 = 4098 (0x1002, AMD), so AMD adapters draw t1_ati (V half-texel -1/4 instead of -1/2,
    // g1f t06 capture). The PrimDepthSprite* filters (0x10DE) only reject DX8 techniques, never the best one.
    // Used for the post effects (Renderer::post_effect); mesh effects keep get() / get_or_default().
    std::shared_ptr<const materials::MaterialEffect> get_for_vendor(std::string_view effect, uint32_t vendor_id,
                                                                    std::string* err = nullptr);
    // assets::ModelLoadOptions whose vertex_type_override returns the material effect's _ALAMO_VERTEX_TYPE
    // (MaterialEffect::vertex_type_override(), SPACE_AND_OPEN.md D.2: Planet.fx -> alD3dVertNU2U3U3), "" to keep
    // the ALO format. Runs on loader threads through get() (mutex; deterministic per material). The cache must
    // outlive every AssetCache / load using the options.
    assets::ModelLoadOptions model_load_options();
    // Registers an engine-made (or test) effect under effect_key(name).
    void add(std::string_view name, materials::MaterialEffect e);
    // D.1 creation-order rank of an effect (materials::EffectLoadOrder: engine head, then the boot preload of
    // the VFS shader directories; size() for names outside it, which callers order after, by name).
    // Pure function of the installed shader files; thread-safe.
    uint32_t load_rank(const materials::MaterialEffect& e) const;

    struct Stats {
        size_t loaded = 0, failed = 0, ff_fallback = 0, untranslated_passes = 0, translated_passes = 0;
        size_t defaulted = 0;  // failed names resolved to the green default effect (get_or_default)
        size_t remapped = 0;   // missing names resolved to a shipped effect (LoadOptions::missing_effect_remap, opt-in)
    };
    Stats stats() const;

private:
    mutable std::mutex mutex_;
    std::unique_ptr<materials::EffectLibrary> lib_;
    materials::EffectLoadOrder head_;  // without a file provider: the engine head only
    std::unordered_map<std::string, std::shared_ptr<const materials::MaterialEffect>> defaults_;  // D.3 per failed name
    std::unordered_map<std::string, std::shared_ptr<const materials::MaterialEffect>> map_;
    std::unordered_map<std::string, std::shared_ptr<const materials::MaterialEffect>> vendor_map_;  // "KEY#vendor"
    Stats stats_;
};

// Attachment formats <-> PsoKey::render_pass (stable small ids, 0 = unknown).
uint8_t render_pass_id(VkFormat color, VkFormat depth);
bool render_pass_formats(uint8_t id, VkFormat& color, VkFormat& depth);

const materials::ShaderBlob* shader_blob_by_id(uint32_t id);
// Input locations of the vertex-stage SPIR-V (Location-decorated Input variables), sorted.
std::vector<uint32_t> spirv_input_locations(std::span<const uint32_t> spirv);

// Owns what the desc's spans point at.
struct PipelineBuild {
    std::vector<uint32_t> vs, fs;
    std::string vs_entry, fs_entry;
    rhi::VertexBinding binding;                     // binding 0: the mesh vertex buffer
    std::vector<rhi::VertexBinding> all_bindings;   // + binding 1 (zero buffer, stride 0) for translated programs
    std::vector<rhi::VertexAttribute> attributes;
    VkFormat color = VK_FORMAT_UNDEFINED;
    rhi::GraphicsPipelineDesc desc;
};
bool pipeline_desc_from_key(const materials::PsoKey& key, VkPipelineLayout layout, PipelineBuild& out,
                            std::string* err = nullptr);
// Track B: key.flags & kPsoTranslated. SPIR-V + entry points from the TranslatedProgram; vertex
// input from its VS inputs matched against the MESH layout key.vertex_layout (vertex_layout_desc):
// locations the mesh provides read binding 0, every other declared input reads a zero buffer on
// binding 1 (stride 0, R32G32B32A32_SFLOAT). `layout` = the StandardLayouts translated layout.
bool pipeline_desc_from_translated(const materials::PsoKey& key, const materials::TranslatedProgram& tp,
                                   VkPipelineLayout layout, PipelineBuild& out, std::string* err = nullptr);
constexpr uint32_t kZeroBinding = 1;
// Dynamic state of a pass (stencil reference/masks, blend constants, depth bias in Vulkan units).
void cmd_set_dynamic_state(const rhi::DeviceFns& fn, VkCommandBuffer cmd, const materials::DynamicState& d,
                           rhi::Format depth_format);

std::filesystem::path default_pipeline_cache_dir();

// Builds the description of a translated key (renderer: looks the TranslatedProgram up). Called on
// pipeline-creation jobs; must be thread-safe.
using TranslatedResolver = std::function<bool(const materials::PsoKey&, PipelineBuild&, std::string*)>;

struct PipelineManagerDesc {
    std::filesystem::path dir;           // empty: in-memory only
    jobs::JobSystem* js = nullptr;       // null: created synchronously inside get()
    VkPipelineLayout layout = VK_NULL_HANDLE;
    bool preload = true;                 // pre-create the keys reloaded from disk (translated keys excluded:
                                         // their programs are translated on demand; they stay in the file)
    TranslatedResolver translated;       // required for kPsoTranslated keys
};

class PipelineManager {
public:
    PipelineManager() = default;
    ~PipelineManager();
    PipelineManager(const PipelineManager&) = delete;
    PipelineManager& operator=(const PipelineManager&) = delete;

    bool init(rhi::Context& ctx, const PipelineManagerDesc& desc);
    void shutdown(bool save = true);  // waits for pending creations; GPU must be idle
    bool save() const;

    // Render thread. VK_NULL_HANDLE while pending (creation requested on first sight) or failed.
    VkPipeline get(const materials::PsoKey& key);
    bool failed(const materials::PsoKey& key) const;
    void wait_idle();  // blocks until every requested pipeline finished (tools / tests)

    struct Stats {
        uint32_t keys = 0, ready = 0, failed = 0, pending = 0;
        uint32_t reloaded = 0;          // keys read from disk at init
        uint32_t reloaded_translated = 0;  // ... of which translated (created when first requested)
        bool cache_loaded = false;      // VkPipelineCache blob accepted by rhi
        bool invalidated = false;       // files discarded (fingerprint / device / driver changed)
        // From rhi::PipelineCache::stats() (creation feedback, always requested):
        uint32_t feedback = 0;          // pipelines created with valid creation feedback (hits + misses)
        uint32_t cache_hits = 0;        // ... of which VkPipelineCache hits (APPLICATION_PIPELINE_CACHE_HIT)
        uint32_t cache_misses = 0;
        uint32_t feedback_unknown = 0;  // driver left the feedback invalid
        double create_ms = 0;           // summed vkCreateGraphicsPipelines wall time
        double max_create_ms = 0;
    };
    Stats stats() const;

private:
    struct Entry {
        materials::PsoKey key;
        std::atomic<int> state{0};  // 0 pending, 1 ready, 2 failed
        VkPipeline pipeline = VK_NULL_HANDLE;
    };
    void create(Entry& e);
    void request(const materials::PsoKey& key);
    std::filesystem::path keys_path() const;
    std::filesystem::path cache_path() const;

    rhi::Context* ctx_ = nullptr;
    PipelineManagerDesc desc_{};
    rhi::PipelineCache cache_;
    mutable std::mutex mutex_;
    std::unordered_map<materials::PsoKey, std::unique_ptr<Entry>> map_;
    std::vector<materials::PsoKey> deferred_;  // reloaded translated keys not requested yet (kept on save)
    std::unique_ptr<jobs::JobCounter> counter_;
    Stats stats_;
};

}  // namespace alamo::render

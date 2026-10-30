#include "alamo/render/pipelines.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/log.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/materials/programs.hpp"
#include "alamo/render/convert.hpp"
#include "alamo/vfs/vfs.hpp"

namespace alamo::render {

// ------------------------------------------------------------------------------------ effects

std::string effect_key(std::string_view n) {
    const size_t slash = n.find_last_of("\\/");
    if (slash != std::string_view::npos) n = n.substr(slash + 1);
    const size_t dot = n.find_last_of('.');
    if (dot != std::string_view::npos) n = n.substr(0, dot);
    std::string k(n);
    for (char& c : k)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return k;
}

materials::LoadOptions EffectCache::default_options() {
    materials::LoadOptions o;
    // Track B on: passes without a hand-written program reach ProgramStatus::Translated and are
    // translated on demand (renderer worker jobs), instead of falling back to the FF technique.
    o.ff_fallback_for_untranslated = false;
    o.engine_sampler.max_anisotropy = 8;  // engine quality sampler (PORTING.md 3.3)
    return o;
}

EffectCache::EffectCache(const FileProvider* files, materials::LoadOptions options) {
    if (!files) return;
    lib_ = std::make_unique<materials::EffectLibrary>(*files, options);
    // D.1 effect creation order from the shader directories (a VFS; other providers: the engine head only).
    if (const auto* v = dynamic_cast<const vfs::Vfs*>(files)) lib_->set_shader_files(materials::shader_files_from_vfs(*v));
}

uint32_t EffectCache::load_rank(const materials::MaterialEffect& e) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return lib_ ? lib_->effect_load_rank(e) : head_.rank(e);
}
EffectCache::~EffectCache() = default;

std::shared_ptr<const materials::MaterialEffect> EffectCache::get(std::string_view effect, std::string* err) {
    const std::string key = effect_key(effect);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        if (!it->second && err) *err = "effect " + key + " failed to load";
        return it->second;
    }
    std::shared_ptr<const materials::MaterialEffect> e;
    std::string why;
    if (lib_) {
        std::optional<materials::MaterialEffect> m = lib_->load(effect, &why);
        if (m) e = std::make_shared<const materials::MaterialEffect>(std::move(*m));
    } else {
        why = "no file provider";
    }
    if (e) {
        ++stats_.loaded;
        if (e->used_ff_fallback) ++stats_.ff_fallback;
        for (const auto& p : e->passes) {
            if (p.restore_only) continue;
            if (p.program_status == materials::ProgramStatus::Translated) ++stats_.translated_passes;
            else if (p.program_status != materials::ProgramStatus::Ok) ++stats_.untranslated_passes;
        }
    } else {
        ++stats_.failed;
        // D.3: the original logs nothing for a missing file (one debug line per name here; failures are cached)
        // and "Failed to load effect" for a file that exists but does not load.
        if (lib_ && !lib_->find_effect_path(effect)) log::debugf("render", "effect %s: not found (%s)", key.c_str(), why.c_str());
        else log::warnf("render", "effect %s: %s", key.c_str(), why.c_str());
        if (err) *err = why;
    }
    map_[key] = e;
    return e;
}

std::shared_ptr<const materials::MaterialEffect> EffectCache::get_or_default(std::string_view effect) {
    std::shared_ptr<const materials::MaterialEffect> e = get(effect);
    if (e || !lib_) return e;
    const std::string key = effect_key(effect);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = defaults_.find(key);
    if (it != defaults_.end()) return it->second;
    std::string why;
    materials::MaterialEffect m = lib_->load_or_default(effect, &why);
    std::shared_ptr<const materials::MaterialEffect> d;
    if (!m.is_default_effect() && !m.remapped_from.empty()) {
        // Opt-in compatibility remap (LoadOptions::missing_effect_remap): a shipped effect stands in.
        ++stats_.remapped;
        d = std::make_shared<const materials::MaterialEffect>(std::move(m));
    } else if (m.fallback == materials::EffectFallback::Missing) {
        ++stats_.defaulted;
        // D.3: a missing name returns the single default effect object (one D.1 batch for all of them).
        auto& shared = defaults_[materials::kDefaultEffectName];
        if (!shared) shared = std::make_shared<const materials::MaterialEffect>(std::move(m));
        d = shared;
    } else {
        ++stats_.defaulted;
        d = std::make_shared<const materials::MaterialEffect>(std::move(m));  // Failed: own object under its name
    }
    defaults_[key] = d;
    return d;
}

assets::ModelLoadOptions EffectCache::model_load_options() {
    assets::ModelLoadOptions o;
    o.vertex_type_override = [this](const assets::MaterialRef& m) -> std::string {
        if (m.effect.empty()) return {};
        // Same resolution as the submesh effect (get_or_default): a remapped effect's annotation applies.
        const std::shared_ptr<const materials::MaterialEffect> e = get_or_default(m.effect);
        if (!e || e->is_default_effect() || e->vertex_type_override() == materials::VertexLayout::Unknown) return {};
        return e->meta.vertex_type;  // the annotation string, e.g. "alD3dVertNU2U3U3"
    };
    return o;
}

void EffectCache::add(std::string_view name, materials::MaterialEffect e) {
    std::lock_guard<std::mutex> lock(mutex_);
    map_[effect_key(name)] = std::make_shared<const materials::MaterialEffect>(std::move(e));
}

EffectCache::Stats EffectCache::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

// ------------------------------------------------------------------------------ translation

namespace {
constexpr VkFormat kColorFormats[] = {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
                                      VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                      VK_FORMAT_R16G16B16A16_SFLOAT};
// Ids are persisted in the pipeline-cache key file: 1 + colour * 4 + depth; colour-only passes (post,
// no depth attachment) use 128 + colour.
constexpr VkFormat kDepthFormats[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT,
                                      VK_FORMAT_D16_UNORM};
constexpr uint8_t kColorOnlyBase = 128;
}  // namespace

uint8_t render_pass_id(VkFormat color, VkFormat depth) {
    if (depth == VK_FORMAT_UNDEFINED) {
        for (size_t c = 0; c < std::size(kColorFormats); ++c)
            if (kColorFormats[c] == color) return static_cast<uint8_t>(kColorOnlyBase + c);
        return 0;
    }
    for (size_t c = 0; c < std::size(kColorFormats); ++c)
        for (size_t d = 0; d < std::size(kDepthFormats); ++d)
            if (kColorFormats[c] == color && kDepthFormats[d] == depth)
                return static_cast<uint8_t>(1 + c * std::size(kDepthFormats) + d);
    return 0;
}

bool render_pass_formats(uint8_t id, VkFormat& color, VkFormat& depth) {
    if (id == 0) return false;
    if (id >= kColorOnlyBase) {
        if (static_cast<size_t>(id - kColorOnlyBase) >= std::size(kColorFormats)) return false;
        color = kColorFormats[id - kColorOnlyBase];
        depth = VK_FORMAT_UNDEFINED;
        return true;
    }
    const size_t i = static_cast<size_t>(id - 1);
    const size_t c = i / std::size(kDepthFormats), d = i % std::size(kDepthFormats);
    if (c >= std::size(kColorFormats)) return false;
    color = kColorFormats[c];
    depth = kDepthFormats[d];
    return true;
}

const materials::ShaderBlob* shader_blob_by_id(uint32_t id) {
    for (const materials::ShaderBlob& b : materials::all_shader_blobs())
        if (b.id == id) return &b;
    return nullptr;
}

std::vector<uint32_t> spirv_input_locations(std::span<const uint32_t> w) {
    std::vector<uint32_t> out;
    if (w.size() < 5 || w[0] != 0x07230203u) return out;
    std::unordered_map<uint32_t, uint32_t> location;  // id -> Location
    std::vector<uint32_t> inputs;
    for (size_t i = 5; i < w.size();) {
        const uint32_t op = w[i] & 0xFFFFu, n = w[i] >> 16;
        if (n == 0 || i + n > w.size()) break;
        if (op == 71 /*OpDecorate*/ && n >= 4 && w[i + 2] == 30 /*Location*/) location[w[i + 1]] = w[i + 3];
        if (op == 59 /*OpVariable*/ && n >= 4 && w[i + 3] == 1 /*Input*/) inputs.push_back(w[i + 2]);
        i += n;
    }
    for (uint32_t id : inputs) {
        auto it = location.find(id);
        if (it != location.end()) out.push_back(it->second);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

namespace {
VkStencilOpState stencil_face(const materials::StencilFace& f) {
    VkStencilOpState s{};
    s.failOp = static_cast<VkStencilOp>(f.fail_op);
    s.passOp = static_cast<VkStencilOp>(f.pass_op);
    s.depthFailOp = static_cast<VkStencilOp>(f.depth_fail_op);
    s.compareOp = static_cast<VkCompareOp>(f.compare);
    s.compareMask = 0xFF;  // dynamic (rhi): set per draw from DynamicState
    s.writeMask = 0xFF;
    s.reference = 0;
    return s;
}
}  // namespace

bool pipeline_desc_from_key(const materials::PsoKey& k, VkPipelineLayout layout, PipelineBuild& b, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    const materials::ShaderBlob* vs = shader_blob_by_id(k.vs_id);
    const materials::ShaderBlob* ps = shader_blob_by_id(k.ps_id);
    if (!vs || vs->stage != materials::ShaderStage::Vertex) return fail("unknown vertex program id");
    if (k.ps_id && (!ps || ps->stage != materials::ShaderStage::Pixel)) return fail("unknown pixel program id");
    VkFormat depth = VK_FORMAT_UNDEFINED;
    if (!render_pass_formats(k.render_pass, b.color, depth)) return fail("unknown render pass id");
    b.vs.assign(vs->spirv.begin(), vs->spirv.end());
    if (ps) b.fs.assign(ps->spirv.begin(), ps->spirv.end());
    b.vs_entry = vs->entry_point;
    b.fs_entry = ps ? ps->entry_point : "main";
    const std::vector<uint32_t> locs = spirv_input_locations(b.vs);
    VertexInputDesc vin;
    if (!build_vertex_input(k.vertex_layout, locs, vin)) return fail("vertex layout does not feed the VS inputs");
    if (k.flags & materials::kPsoTranslated) return fail("translated key: use pipeline_desc_from_translated");
    b.binding = vin.binding;
    b.all_bindings = {b.binding};
    b.attributes = std::move(vin.attributes);

    rhi::GraphicsPipelineDesc& d = b.desc;
    d = {};
    d.vs_spirv = b.vs;
    d.fs_spirv = b.fs;
    d.vs_entry = b.vs_entry.c_str();
    d.fs_entry = b.fs_entry.c_str();
    d.bindings = b.all_bindings;
    d.attributes = b.attributes;
    d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.raster.polygon = static_cast<VkPolygonMode>(k.polygon_mode);
    d.raster.cull = static_cast<VkCullModeFlags>(k.cull);
    d.raster.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;  // materials pso.hpp convention (negative-height viewport)
    d.raster.depth_bias = k.depth_bias_enable != 0;
    d.depth.depth_test = k.depth_test != 0;
    d.depth.depth_write = k.depth_write != 0;
    d.depth.depth_op = static_cast<VkCompareOp>(k.depth_compare);
    d.depth.stencil_test = k.stencil_enable != 0;
    d.depth.front = stencil_face(k.front);
    d.depth.back = stencil_face(k.back);
    d.blend.enable = k.blend_enable != 0;
    d.blend.src_color = static_cast<VkBlendFactor>(k.src_color);
    d.blend.dst_color = static_cast<VkBlendFactor>(k.dst_color);
    d.blend.color_op = static_cast<VkBlendOp>(k.color_op);
    d.blend.src_alpha = static_cast<VkBlendFactor>(k.src_alpha);
    d.blend.dst_alpha = static_cast<VkBlendFactor>(k.dst_alpha);
    d.blend.alpha_op = static_cast<VkBlendOp>(k.alpha_op);
    d.blend.write_mask = static_cast<VkColorComponentFlags>(k.color_write_mask & 0xF);
    d.color_formats = {&b.color, 1};
    d.depth_format = depth;
    const rhi::FormatInfo& dfi = rhi::format_info(rhi::from_vk(depth));
    d.stencil_format = dfi.stencil ? depth : VK_FORMAT_UNDEFINED;
    d.layout = layout;
    d.name = vs->name;
    return true;
}

bool pipeline_desc_from_translated(const materials::PsoKey& k, const materials::TranslatedProgram& tp, VkPipelineLayout layout,
                                   PipelineBuild& b, std::string* err) {
    auto fail = [&](const std::string& why) {
        if (err) *err = why;
        return false;
    };
    if (!(k.flags & materials::kPsoTranslated)) return fail("not a translated key");
    if (tp.vs.spirv.empty() || tp.ps.spirv.empty()) return fail("translated program without SPIR-V");
    VkFormat depth = VK_FORMAT_UNDEFINED;
    if (!render_pass_formats(k.render_pass, b.color, depth)) return fail("unknown render pass id");
    b.vs = tp.vs.spirv;
    b.fs = tp.ps.spirv;
    b.vs_entry = tp.vs.entry;
    b.fs_entry = tp.ps.entry;
    const materials::VertexLayoutDesc md = effective_layout_desc(k.vertex_layout);
    if (md.stride == 0) return fail("translated pass on a layout without a vertex table");
    b.binding = {0, md.stride, false};
    b.all_bindings = {b.binding};
    b.attributes.clear();
    bool zero = false;
    for (const materials::TranslatedInput& in : tp.inputs) {
        rhi::VertexAttribute a;
        a.location = in.location;
        const materials::VertexAttributeDesc* fed = nullptr;
        for (const materials::VertexAttributeDesc& m : md.attributes)
            if (m.location == in.location) fed = &m;
        if (fed) {
            a.binding = 0;
            a.format = static_cast<VkFormat>(fed->vk_format);
            a.offset = fed->offset;
        } else {  // declared but not in the mesh: (0,0,0,0) from the zero buffer
            a.binding = kZeroBinding;
            a.format = VK_FORMAT_R32G32B32A32_SFLOAT;
            a.offset = 0;
            zero = true;
        }
        b.attributes.push_back(a);
    }
    if (zero) b.all_bindings.push_back({kZeroBinding, 0, false});

    rhi::GraphicsPipelineDesc& d = b.desc;
    d = {};
    d.vs_spirv = b.vs;
    d.fs_spirv = b.fs;
    d.vs_entry = b.vs_entry.c_str();
    d.fs_entry = b.fs_entry.c_str();
    d.bindings = b.all_bindings;
    d.attributes = b.attributes;
    d.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    d.raster.polygon = static_cast<VkPolygonMode>(k.polygon_mode);
    d.raster.cull = static_cast<VkCullModeFlags>(k.cull);
    d.raster.front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    d.raster.depth_bias = k.depth_bias_enable != 0;
    d.depth.depth_test = k.depth_test != 0;
    d.depth.depth_write = k.depth_write != 0;
    d.depth.depth_op = static_cast<VkCompareOp>(k.depth_compare);
    d.depth.stencil_test = k.stencil_enable != 0;
    d.depth.front = stencil_face(k.front);
    d.depth.back = stencil_face(k.back);
    d.blend.enable = k.blend_enable != 0;
    d.blend.src_color = static_cast<VkBlendFactor>(k.src_color);
    d.blend.dst_color = static_cast<VkBlendFactor>(k.dst_color);
    d.blend.color_op = static_cast<VkBlendOp>(k.color_op);
    d.blend.src_alpha = static_cast<VkBlendFactor>(k.src_alpha);
    d.blend.dst_alpha = static_cast<VkBlendFactor>(k.dst_alpha);
    d.blend.alpha_op = static_cast<VkBlendOp>(k.alpha_op);
    d.blend.write_mask = static_cast<VkColorComponentFlags>(k.color_write_mask & 0xF);
    d.color_formats = {&b.color, 1};
    d.depth_format = depth;
    const rhi::FormatInfo& dfi = rhi::format_info(rhi::from_vk(depth));
    d.stencil_format = dfi.stencil ? depth : VK_FORMAT_UNDEFINED;
    d.layout = layout;
    d.name = "render.translated";
    return true;
}

void cmd_set_dynamic_state(const rhi::DeviceFns& fn, VkCommandBuffer cmd, const materials::DynamicState& d,
                           rhi::Format depth_format) {
    fn.vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, d.stencil_ref & 0xFFu);
    fn.vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, d.stencil_read_mask & 0xFFu);
    fn.vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, d.stencil_write_mask & 0xFFu);
    const uint32_t c = d.blend_factor;  // D3DCOLOR ARGB
    const float bc[4] = {static_cast<float>((c >> 16) & 0xFF) / 255.0f, static_cast<float>((c >> 8) & 0xFF) / 255.0f,
                         static_cast<float>(c & 0xFF) / 255.0f, static_cast<float>((c >> 24) & 0xFF) / 255.0f};
    fn.vkCmdSetBlendConstants(cmd, bc);
    const rhi::DepthBias db = rhi::depth_bias_from_d3d9(d.depth_bias, d.slope_scaled_depth_bias, depth_format);
    fn.vkCmdSetDepthBias(cmd, db.constant_factor, db.clamp, db.slope_factor);
}

std::filesystem::path default_pipeline_cache_dir() {
#ifdef _WIN32
    const char* la = std::getenv("LOCALAPPDATA");
    if (la && *la) return alamo::path_from_utf8(la) / "alamo" / "render";
#endif
    const char* home = std::getenv("HOME");
    if (home && *home) return alamo::path_from_utf8(home) / ".cache" / "alamo" / "render";
    return std::filesystem::temp_directory_path() / "alamo" / "render";
}

// ------------------------------------------------------------------------------ manager

namespace {
#pragma pack(push, 1)
struct KeysHeader {
    char magic[8];
    uint32_t version;
    uint32_t vendor, device, driver;
    uint8_t uuid[VK_UUID_SIZE];
    uint64_t fingerprint;
    uint32_t count;
};
#pragma pack(pop)
constexpr char kMagic[8] = {'A', 'L', 'P', 'S', 'O', 'K', 'E', 'Y'};
constexpr uint32_t kKeysVersion = 1;

KeysHeader make_header(const rhi::DeviceCaps& c, uint32_t count) {
    KeysHeader h{};
    std::memcpy(h.magic, kMagic, 8);
    h.version = kKeysVersion;
    h.vendor = c.vendor_id;
    h.device = c.device_id;
    h.driver = c.driver_version;
    std::memcpy(h.uuid, c.pipeline_cache_uuid, VK_UUID_SIZE);
    h.fingerprint = materials::programs_fingerprint();
    h.count = count;
    return h;
}
}  // namespace

PipelineManager::~PipelineManager() { shutdown(false); }

std::filesystem::path PipelineManager::keys_path() const {
    const rhi::DeviceCaps& c = ctx_->caps();
    char name[64];
    std::snprintf(name, sizeof name, "alamo_pso_%04x_%04x.keys", c.vendor_id, c.device_id);
    return alamo::long_path(desc_.dir / name);  // \\?\ on Windows: cache dirs over MAX_PATH
}
std::filesystem::path PipelineManager::cache_path() const {
    const rhi::DeviceCaps& c = ctx_->caps();
    char name[64];
    std::snprintf(name, sizeof name, "alamo_pso_%04x_%04x.vkcache", c.vendor_id, c.device_id);
    return alamo::long_path(desc_.dir / name);  // rhi reads it through read_host_file(path): no UTF-8 round trip
}

bool PipelineManager::init(rhi::Context& ctx, const PipelineManagerDesc& desc) {
    shutdown(false);
    ctx_ = &ctx;
    desc_ = desc;
    stats_ = {};
    counter_ = std::make_unique<jobs::JobCounter>();
    std::vector<materials::PsoKey> reload;
    if (!desc_.dir.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(alamo::long_path(desc_.dir), ec);
        std::ifstream f(keys_path(), std::ios::binary);
        bool valid = false;
        if (f) {
            KeysHeader h{};
            f.read(reinterpret_cast<char*>(&h), sizeof h);
            const KeysHeader want = make_header(ctx.caps(), h.count);
            valid = f && std::memcmp(&h, &want, sizeof h) == 0 && h.count < 1u << 20;
            if (valid) {
                reload.resize(h.count);
                f.read(reinterpret_cast<char*>(reload.data()), static_cast<std::streamsize>(reload.size() * sizeof(materials::PsoKey)));
                if (!f) {
                    reload.clear();
                    valid = false;
                }
            }
            if (!valid) {
                // Programs, driver or device changed: both files are stale.
                f.close();
                std::filesystem::remove(keys_path(), ec);
                std::filesystem::remove(cache_path(), ec);
                stats_.invalidated = true;
                log::infof("render", "pipeline cache in %s invalidated (programs/driver/device changed)",
                           alamo::path_to_utf8(desc_.dir).c_str());
            }
        }
    }
    if (!cache_.init(ctx, desc_.dir.empty() ? std::filesystem::path() : cache_path())) return false;
    stats_.cache_loaded = cache_.loaded_from_disk();
    stats_.reloaded = static_cast<uint32_t>(reload.size());
    for (const materials::PsoKey& k : reload)
        if (k.flags & materials::kPsoTranslated) {
            deferred_.push_back(k);
            ++stats_.reloaded_translated;
        } else if (desc_.preload) {
            request(k);
        }
    return true;
}

void PipelineManager::shutdown(bool save_first) {
    if (!ctx_) return;
    wait_idle();
    if (save_first && !save())
        log::warnf("render", "pipeline cache: could not write %s (path too long or not writable?)",
                   alamo::path_to_utf8(keys_path()).c_str());
    for (auto& [k, e] : map_)
        if (e->pipeline) ctx_->vkd().vkDestroyPipeline(ctx_->device(), e->pipeline, nullptr);
    map_.clear();
    deferred_.clear();
    cache_.shutdown(false);
    counter_.reset();
    ctx_ = nullptr;
}

bool PipelineManager::save() const {
    if (!ctx_ || desc_.dir.empty()) return true;
    std::vector<materials::PsoKey> keys;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [k, e] : map_)
            if (e->state.load() == 1) keys.push_back(k);
        for (const materials::PsoKey& k : deferred_)
            if (!map_.count(k)) keys.push_back(k);
    }
    std::sort(keys.begin(), keys.end());
    const KeysHeader h = make_header(ctx_->caps(), static_cast<uint32_t>(keys.size()));
    const std::filesystem::path p = keys_path();
    std::filesystem::path tmp = p;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(reinterpret_cast<const char*>(&h), sizeof h);
        f.write(reinterpret_cast<const char*>(keys.data()), static_cast<std::streamsize>(keys.size() * sizeof(materials::PsoKey)));
        if (!f) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, p, ec);
    return !ec && cache_.save();
}

void PipelineManager::create(Entry& e) {
    PipelineBuild b;
    std::string err;
    VkPipeline p = VK_NULL_HANDLE;
    const auto t0 = std::chrono::steady_clock::now();
    rhi::PipelineFeedback fb;  // VK_EXT_pipeline_creation_feedback (core 1.3): cache hit + duration
    const bool built = (e.key.flags & materials::kPsoTranslated)
                           ? (desc_.translated ? desc_.translated(e.key, b, &err) : (err = "no translated resolver", false))
                           : pipeline_desc_from_key(e.key, desc_.layout, b, &err);
    if (built) {
        b.desc.feedback = &fb;
        p = cache_.create_graphics(b.desc);
    }
    else log::warnf("render", "PsoKey %016llx: %s", static_cast<unsigned long long>(materials::hash(e.key)), err.c_str());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::lock_guard<std::mutex> lock(mutex_);
    e.pipeline = p;
    e.state.store(p ? 1 : 2);
    stats_.create_ms += ms;
    stats_.max_create_ms = std::max(stats_.max_create_ms, ms);
}

void PipelineManager::request(const materials::PsoKey& key) {
    Entry* e = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& slot = map_[key];
        if (slot) return;
        slot = std::make_unique<Entry>();
        slot->key = key;
        e = slot.get();
    }
    if (desc_.js) desc_.js->spawn(*counter_, [this, e] { create(*e); }, jobs::Priority::Background);
    else create(*e);
}

VkPipeline PipelineManager::get(const materials::PsoKey& key) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = map_.find(key);
        if (it != map_.end()) return it->second->state.load() == 1 ? it->second->pipeline : VK_NULL_HANDLE;
    }
    request(key);
    std::lock_guard<std::mutex> lock(mutex_);
    const Entry& e = *map_[key];
    return e.state.load() == 1 ? e.pipeline : VK_NULL_HANDLE;
}

bool PipelineManager::failed(const materials::PsoKey& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    return it != map_.end() && it->second->state.load() == 2;
}

void PipelineManager::wait_idle() {
    if (desc_.js && counter_) desc_.js->wait(*counter_);
}

PipelineManager::Stats PipelineManager::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats s = stats_;
    // Cache hits from the rhi PipelineCache's creation-feedback totals (every creation of this manager).
    const rhi::PipelineCacheStats cs = cache_.stats();
    s.feedback = static_cast<uint32_t>(cs.hits + cs.misses);
    s.cache_hits = static_cast<uint32_t>(cs.hits);
    s.cache_misses = static_cast<uint32_t>(cs.misses);
    s.feedback_unknown = static_cast<uint32_t>(cs.unknown);
    s.keys = static_cast<uint32_t>(map_.size());
    for (const auto& [k, e] : map_) {
        const int st = e->state.load();
        if (st == 0) ++s.pending;
        else if (st == 1) ++s.ready;
        else ++s.failed;
    }
    return s;
}

}  // namespace alamo::render

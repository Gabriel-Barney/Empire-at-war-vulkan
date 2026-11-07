// FXO material loader: reads an effect through alamo::FileProvider, parses it with alamo_fx2,
// picks the best technique (DX9 > DX8ATI > DX8 > FIXEDFUNCTION; max_viewport and unknown LODs
// last), resolves every pass against engine device defaults (D3D9 defaults + CULLMODE = CW) <-
// ENGINE\Phase<phase>.fxo pass 0, and
// produces PsoKeys (opaque + fading variants), dynamic state, push-constant defaults, sampler
// keys, the hot-set program (or "needs translation") and the parameter bindings.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/materials/d3d9_state.hpp"
#include "alamo/materials/fixed_function.hpp"
#include "alamo/materials/params.hpp"
#include "alamo/materials/programs.hpp"
#include "alamo/materials/pso.hpp"
#include "alamo/materials/register_file.hpp"
#include "alamo/materials/translated.hpp"
#include "alamo/shaderxlat/translate.hpp"

namespace alamo {
class FileProvider;
}
namespace alamo::fx {
struct Effect;
}

namespace alamo::materials {

enum class RenderPhase : uint8_t {
    None = 0, Opaque, Transparent, Shadow, TerrainMesh, TerrainWater, Heat, Occluded, Terrain, Water,
    Bloom, Fissure, Count
};
RenderPhase render_phase_from_name(std::string_view s);   // _ALAMO_RENDER_PHASE / RenderPhase strings
const char* render_phase_name(RenderPhase p);
const char* phase_effect_file(RenderPhase p);             // "Data\\Art\\Shaders\\Engine\\PhaseOpaque.fxo"

enum class TechniqueLod : uint8_t { DX9 = 0, DX8ATI, DX8, FixedFunction, FF, Unknown, MaxViewport };
TechniqueLod technique_lod_from_string(std::string_view lod, std::string_view technique_name);
const char* technique_lod_name(TechniqueLod l);
// Index of the best technique (lowest TechniqueLod; first wins on ties), -1 if none.
int pick_best_technique(const fx::Effect& e);

struct SamplerBinding {
    uint16_t slot = 0;             // s# (shader passes) or texture stage (FF passes)
    std::string sampler_param;     // effect sampler parameter ("" for FF stage without one)
    std::string texture_param;     // effect texture parameter ("" = engine bound, e.g. particles)
    std::string texture_semantic;  // FOW_TEXTURE, SKY_CUBE_TEXTURE ... when engine supplied
    SamplerKey key;
};

struct ResolvedPass {
    std::string name;
    int pass_index = 0;
    int technique = -1;            // technique index in the effect
    bool fixed_function = false;   // no VS/PS: FF ubershader
    bool restore_only = false;     // cleanup pass (no shaders, no draw-relevant state); see notes
    bool fade_dynamic = false;     // ALPHABLENDENABLE = (m_lightScale.w < 1)
    PsoKey pso[2];                 // [0] opaque (lightScale.w == 1), [1] fading
    DynamicState dynamic;
    PushConstants push;            // alpha test + fog defaults (draw_index set per draw)
    std::vector<SamplerBinding> samplers;
    ProgramStatus program_status = ProgramStatus::NeedsTranslation;
    Program program;
    // Track B data (translated shaders): CTAB register plans of the pass bytecode.
    std::optional<RegisterFilePlan> vs_plan, ps_plan;
    uint64_t vs_bytecode_hash = 0, ps_bytecode_hash = 0;
    std::optional<FfPassState> ff;
    // Track B (ProgramStatus::Translated): the pass bytecode, translation options and push defaults.
    std::vector<uint8_t> vs_bytecode, ps_bytecode;
    shaderxlat::TranslateOptions vs_options, ps_options;
    shaderxlat::TranslatedPush translated_push;
    // ps_1_x pixel shader using the bump-env registers (texbem / texbeml / bem): fill_bump_env()
    // writes PSConstants c16..c31 only for these passes (== translated ps iface.bump_env).
    bool uses_bump_env = false;
    DeviceState state;            // fully resolved device state (for diagnostics / translation)
    ApplyReport report;

    // Picks the PSO variant for the current LIGHT_SCALE.w (the only state expression that
    // changes the pipeline, PORTING.md 3.2).
    const PsoKey& select(float light_scale_w) const { return pso[(fade_dynamic && light_scale_w < 1.0f) ? 1 : 0]; }

    // Per-draw evaluation of the ALPHABLENDENABLE expression itself (any mod expression, not
    // only "w < 1"); allocation-free and thread-safe. Falls back to select() when the pass has
    // no dynamic blend expression.
    std::shared_ptr<const CompiledExpression> fade_expr;
    bool blend_enabled(const ExprInputs& in) const;
    const PsoKey& select_exact(const ExprInputs& in) const;
};

// The pass can be drawn as resolved: a restore-only pass, a track-B pass (translated on demand) or one
// with a hand-written / FF program. False for shader passes track B does not take (shader model 3 mixed
// with an older stage, a VS without a PS or the reverse, shader + FF mix): the renderer draws those
// submeshes unlit.
bool pass_has_program(const ResolvedPass& p);

struct ParamBinding {
    std::string name, semantic;
    uint32_t type = 0, cls = 0, rows = 0, columns = 0, elements = 0;  // fx::ParamType / ParamClass
    bool engine = false;           // engine-supplied (semantic block / _ALAMO_)
    bool texture = false, sampler = false, numeric = false;
};

struct EffectMetadata {
    std::string render_phase, vertex_proc, vertex_type;
    bool tangent_space = false, shadow_volume = false, z_sort = false;
    int bones_per_vertex = 0;
};

// Built-in default effect (SPACE_AND_OPEN.md D.3): the original compiles a fixed-function effect
// `TNoShader` from a source string in the exe and draws with it whenever an effect is missing or
// fails to load: solid opaque green (material ambient/diffuse/specular/emissive = (0,1,0,1),
// stage 0 colour/alpha = DIFFUSE, stage 1 disabled, no blend, no texture), bucket 3 (Opaque), no
// _ALAMO_* annotations (vertex layout = the mesh's own ALO format). Nothing is logged.
inline constexpr const char* kDefaultEffectName = "<DEFAULT>";  // not a legal file name: no clash
enum class EffectFallback : uint8_t {
    None = 0,        // a real effect
    BuiltinDefault,  // the default effect itself (EffectLibrary::default_effect())
    Missing,         // neither <name>.fx nor <name>.fxo exists: the default effect object is returned
    Failed,          // the file exists but does not load: the green effect re-created under its own name
                     // (D3DXCreateEffect failure), or, when technique selection fails, the shared
                     // default object itself (name kDefaultEffectName, rank 0)
};

struct MaterialEffect {
    std::string name;              // upper-case, no path/extension
    std::string path;              // as resolved through the FileProvider
    std::shared_ptr<const fx::Effect> effect;
    EffectMetadata meta;
    RenderPhase phase = RenderPhase::None;
    VertexLayout layout = VertexLayout::Unknown;
    int technique = -1;
    std::string technique_name;
    TechniqueLod lod = TechniqueLod::Unknown;
    bool used_ff_fallback = false; // best shader technique needs translation; FF technique used
    // [port] Best technique skipped because a shader pass has no program (pass_has_program(); vs_3_0 /
    // ps_3_0 in mods), like a technique failing ValidateTechnique in the original: its name ("" = none).
    // `technique` is then the next one by LOD / file order whose passes all have a program.
    std::string skipped_technique;
    int skipped_technique_index = -1, skipped_pass = -1;  // its index and its first pass without a program
    std::vector<ResolvedPass> passes;
    std::vector<ParamBinding> params;
    GenericLayout generic_layout;  // per-effect material record for non-mesh effects
    std::vector<std::string> warnings;
    // D.3 fallback: != None means this is the green default effect (or its Failed re-creation).
    EffectFallback fallback = EffectFallback::None;
    std::string requested;         // the name passed to load_or_default() when fallback != None
    // Opt-in compatibility remap (LoadOptions::missing_effect_remap, not original behaviour): the
    // missing name passed to load_or_default() that this shipped effect stands in for ("" = none).
    std::string remapped_from;

    // Draw passes (restore_only skipped).
    size_t draw_pass_count() const;
    bool is_default_effect() const { return fallback != EffectFallback::None; }
    // D.2: the effect's _ALAMO_VERTEX_TYPE overrides the ALO submesh vertex format at load (the
    // records are re-packed into this layout, e.g. Planet.fx -> NU2U3U3 with T = B = 0). Returns
    // the annotated layout, VertexLayout::Unknown when the effect has no (known) annotation.
    // The raw annotation string is meta.vertex_type ("alD3dVertNU2U3U3").
    VertexLayout vertex_type_override() const { return vertex_layout_from_name(meta.vertex_type.c_str()); }
};

// ------------------------------------------------------------- effect creation order (D.1)
// Opaque draws are grouped per effect in the order the original first created each effect
// object (global list, appended on first load, never re-sorted):
//   1. kDefaultEffectName, GLOBAL, ALAMOENGINE (Engine\Global, Engine\AlamoEngine);
//   2. the device-init engine effects in code order (engine_effect_load_sequence());
//   3. the boot preload: pattern-major (*.fx, then *.fxo), then directory-major over the shader
//      directories in search order (each mod dir's Data\Art\Shaders\ and \Terrain\, then the
//      base install's, then fallback dirs). Within one directory: loose files in case-insensitive
//      name order, then MEG entries in table order (= ascending CRC-32 of the upper-case path).
//      A base name already created is skipped.
//   4. names first requested later (outside the shader directories): not in the list.
// [L] parts (not pinned by RE): the iteration order of several mounted MEGs (entries of every
// archive are merged and ordered by path CRC, exact for vanilla where each shader name is
// reached once); MEG entries are attributed to the base install directory only; the position
// of fallback (FALLBACKPATH / EaW GameData) loose dirs after the base install.
struct ShaderFileEntry {
    std::string path;      // game path, any case: "Data\\Art\\Shaders\\Terrain\\TerrainBump.fxo"
    bool loose = false;    // loose file; false = MEG entry
    int dir_order = 0;     // search position of the directory root holding it: mod dirs 0..n-1,
                           // base install n (MEG entries use n too), fallback dirs n+1...
    bool from_mod = false; // reached through a mod: a loose file in a mod dir, or an entry of a MEG
                           // mounted from a mod dir (audit_effects() / load report only; D.1 ignores it)
};

// True when host path `host` lies inside directory `dir` (case-insensitive, '/' == '\'; `dir` == `host`
// is not inside). Used to attribute MEG mounts to mod directories.
bool host_path_under(std::string_view host, std::string_view dir);

// Head of the list (steps 1 and 2), upper-case base names, starting with kDefaultEffectName.
const std::vector<std::string>& engine_effect_load_sequence();

class EffectLoadOrder {
public:
    EffectLoadOrder();  // head only
    // Pure function of the entry set (input order and thread timing do not matter).
    explicit EffectLoadOrder(std::span<const ShaderFileEntry> shader_files);
    // Rank of an effect name in any form ("MeshGloss.fx", path, "MESHGLOSS"); size() when the
    // name is not in the list (step 4: callers order those after, by name).
    uint32_t rank(std::string_view effect) const;
    // Same for a loaded effect; Missing / BuiltinDefault fallbacks rank as the default effect (0).
    uint32_t rank(const MaterialEffect& e) const;
    const std::vector<std::string>& names() const { return names_; }
    size_t size() const { return names_.size(); }

private:
    void add(std::string name);
    std::vector<std::string> names_;
    std::map<std::string, uint32_t, std::less<>> index_;
};

// Builds the entries from an alamo::vfs::Vfs (template so materials does not link vfs):
// lib.set_shader_files(shader_files_from_vfs(*vfs)). The D.1 preload covers Shaders\ and
// Shaders\Terrain\ only; include_engine = true also lists Shaders\Engine\ (for audit_effects(): engine
// effects a mod overrides are loaded by name too).
template <class VfsT>
std::vector<ShaderFileEntry> shader_files_from_vfs(const VfsT& vfs, bool include_engine = false) {
    std::vector<ShaderFileEntry> out;
    const int mods = static_cast<int>(vfs.mod_dirs().size());
    std::vector<const char*> globs = {"DATA\\ART\\SHADERS\\*.FX", "DATA\\ART\\SHADERS\\*.FXO",
                                      "DATA\\ART\\SHADERS\\TERRAIN\\*.FX", "DATA\\ART\\SHADERS\\TERRAIN\\*.FXO"};
    if (include_engine) {
        globs.push_back("DATA\\ART\\SHADERS\\ENGINE\\*.FX");
        globs.push_back("DATA\\ART\\SHADERS\\ENGINE\\*.FXO");
    }
    for (const char* glob : globs) {
        for (auto& p : vfs.list(glob)) {
            auto r = vfs.resolve(p);
            if (!r) continue;
            ShaderFileEntry e;
            e.loose = r->layer != VfsT::Resolution::Meg;
            if (r->layer == VfsT::Resolution::Mod) e.dir_order = r->layer_index;
            else if (r->layer == VfsT::Resolution::Fallback) e.dir_order = mods + 1 + r->layer_index;
            else e.dir_order = mods;
            if (r->layer == VfsT::Resolution::Mod) {
                e.from_mod = true;
            } else if (r->layer == VfsT::Resolution::Meg && r->layer_index >= 0 &&
                       static_cast<size_t>(r->layer_index) < vfs.mounts().size()) {
                const std::string& host = vfs.mounts()[static_cast<size_t>(r->layer_index)].path;
                for (const auto& d : vfs.mod_dirs())
                    if (host_path_under(host, d)) e.from_mod = true;
            }
            e.path = std::move(p);
            out.push_back(std::move(e));
        }
    }
    return out;
}

// Missing-effect compatibility remap: upper-case effect base name ("FISSUREMESHGLOSS") -> shipped
// effect loaded in its place ("MeshGloss.fx"; any form load() accepts).
using EffectRemap = std::map<std::string, std::string, std::less<>>;

// Built-in compatibility table (opt-in; the original draws these with the green default, D.3):
//   FISSUREMESHGLOSS -> MeshGloss.fx. FissureMeshGloss.fx ships in no FoC/EaW MEG or loose dir; its
//   only user is W_TEST_FISSURE.ALO (EaW models.meg, submesh "Line01", referenced by no XML/map).
//   The submesh supplies exactly Emissive, Diffuse, Specular, Shininess + BaseTexture, which is
//   the full material interface of MeshGloss / TerrainMeshGloss / BatchMeshGloss (same engine
//   parameters, NU2 layout). MeshGloss is picked: same name stem, Opaque phase = the bucket the
//   green fallback already draws in, and not a batch- or terrain-phase variant.
const EffectRemap& compat_missing_effect_remap();

struct LoadOptions {
    // Engine sampler defaults for states the effect leaves at D3D9 defaults (PORTING.md 3.3).
    SamplerKey engine_sampler{};
    // When the best technique has no hot-set program but a FIXEDFUNCTION technique exists, use
    // the FF technique through the FF ubershader. Off by default: such passes are now
    // ProgramStatus::Translated (track B, translate_pass()).
    bool ff_fallback_for_untranslated = false;
    // Overrides _ALAMO_RENDER_PHASE (e.g. effects without metadata drawn by the engine).
    std::optional<RenderPhase> phase_override;
    // Opt-in, empty by default (= faithful D.3 green default). load_or_default() looks a MISSING
    // effect (no .fx/.fxo found) up by base name and loads the mapped effect instead
    // (MaterialEffect::remapped_from set, fallback None). Not used by load(), nor for files that
    // exist but fail to load; a mapped effect that is itself missing still gives the green default.
    // Keys in any form ("FissureMeshGloss.fx", a path): normalised by EffectLibrary.
    EffectRemap missing_effect_remap;
};

// Loads and caches phase baselines; thread-compatible (not internally synchronised).
class EffectLibrary {
public:
    explicit EffectLibrary(const alamo::FileProvider& files, LoadOptions options = {});
    ~EffectLibrary();

    // `effect` may be an ALO material shader name ("MeshBumpColorize.fx"), an FXO file name or
    // a game path. Searches Data\Art\Shaders\, \Engine\ and \Terrain\. Returns nullopt (and
    // *err) if not found or malformed.
    std::optional<MaterialEffect> load(std::string_view effect, std::string* err = nullptr);
    // Same, from bytes already in memory (tests, mod tools).
    std::optional<MaterialEffect> load_from_memory(std::string_view name, const std::vector<uint8_t>& bytes,
                                                   std::string* err = nullptr);
    // Baseline state of a phase: engine_device_defaults() (D3D9 defaults + CULLMODE = CW) <-
    // Phase<phase>.fxo pass 0 (device defaults if missing).
    const DeviceState& phase_baseline(RenderPhase p);
    // The compiled .fxo the loader reads: a game path, never a .fx source (nullopt if none).
    std::optional<std::string> find_effect_path(std::string_view effect) const;
    // HLSL source `<name>.fx` of the same name, same search order (nullopt if none). The original
    // compiles it with D3DX at run time and prefers it over the .fxo (RENDER.md 6). The port has no
    // fx_2_0 compiler: with a .fxo present the .fxo is loaded; without one load() fails and
    // load_or_default() returns the green default under the effect's name (EffectFallback::Failed,
    // the original's result when its compile fails). docs/MODS.md, shader section.
    std::optional<std::string> find_effect_source(std::string_view effect) const;

    // D.3: the built-in green fixed-function effect (fallback = BuiltinDefault), built on first use.
    const MaterialEffect& default_effect();
    // load(), but never fails: a missing effect returns a copy of default_effect() with
    // fallback = Missing (name kDefaultEffectName); a file that exists but fails to load returns
    // the same green effect renamed to the requested effect (fallback = Failed, *err set). A file
    // that parses but has no selectable technique returns the default effect object itself
    // (fallback = Failed, name kDefaultEffectName, *err set). A `<name>.fx` source without a .fxo counts as
    // a file that fails to load (Failed under its own name; see find_effect_source()).
    // FissureMeshGloss.fx (no FXO shipped) -> Missing,
    // unless options().missing_effect_remap maps it (opt-in, compat_missing_effect_remap()).
    MaterialEffect load_or_default(std::string_view effect, std::string* err = nullptr);
    // Replaces options().missing_effect_remap (keys normalised to upper-case base names; {} = off).
    // Call before loading (not synchronised).
    void set_missing_effect_remap(EffectRemap remap);
    const EffectRemap& missing_effect_remap() const { return options_.missing_effect_remap; }

    // D.1 effect creation order (see EffectLoadOrder). Without shader files only the engine head
    // is known.
    void set_shader_files(std::span<const ShaderFileEntry> shader_files) { order_ = EffectLoadOrder(shader_files); }
    const EffectLoadOrder& load_order() const { return order_; }
    uint32_t effect_load_rank(const MaterialEffect& e) const { return order_.rank(e); }
    uint32_t effect_load_rank(std::string_view effect) const { return order_.rank(effect); }
    // Track B on demand (worker jobs; never at load): see translated.hpp translate_pass().
    static std::shared_ptr<const TranslatedProgram> translate_pass(const ResolvedPass& pass, const shaderxlat::RuntimeDxc& dxc,
                                                                   const shaderxlat::ShaderCache* cache,
                                                                   std::string* err = nullptr) {
        return materials::translate_pass(pass, dxc, cache, err);
    }

    const LoadOptions& options() const { return options_; }

private:
    std::optional<MaterialEffect> load_impl(std::string_view name, const std::vector<uint8_t>& bytes,
                                            std::string* err);
    std::optional<MaterialEffect> load_file(std::string_view effect, std::string* err);
    std::optional<MaterialEffect> load_parsed(std::string_view name, std::shared_ptr<fx::Effect> e, std::string* err);
    const alamo::FileProvider& files_;
    LoadOptions options_;
    std::map<RenderPhase, DeviceState> baselines_;
    std::optional<MaterialEffect> default_;
    EffectLoadOrder order_;
    bool technique_failed_ = false;  // set by load_parsed() when technique selection fails
};

// Resolves one pass of `e` (used by the loader; exposed for tests with hand-built effects).
ResolvedPass resolve_pass(const fx::Effect& e, int technique, int pass, const DeviceState& baseline,
                          const LoadOptions& options, std::string_view effect_name, VertexLayout layout);

// Signature of the PSO-relevant render states a pass SETS (before merging with the baseline):
// sorted (state, value or 'dynamic') pairs. Used to compare with the Phase 1 count of 52.
std::string partial_state_signature(const fx::Effect& e, int technique, int pass);

}  // namespace alamo::materials

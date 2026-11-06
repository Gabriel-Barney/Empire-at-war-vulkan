// Effect audit (docs/MODS.md, shader section; roadmap modc t11): what the port does with every effect
// the mounted shader directories ship, mod or base, in a form a load report can list. No GPU needed;
// DXC optional. The renderer reaches the same results lazily per drawn effect (render::EffectCache::
// issues(), Renderer::shader_issues()); this runs them all up front.
//
// Effects are looked up by name through the FileProvider (mod dirs first, RENDER.md 6), so a mod .fxo
// that overrides or adds an effect is parsed and its shader passes go through track B (shaderxlat ->
// HLSL -> DXC -> SPIR-V) like the shipped ones. What cannot be drawn as the original draws it is an
// issue below; the renderer's fallback for each kind is fixed (no crash, no silent skip):
//   SourceOnly / LoadFailed  green default effect under the effect's name (SPACE_AND_OPEN.md D.3)
//   NoTechnique              the shared green default effect (D.3)
//   TranslateFailed /
//   UnsupportedPass          the submesh is drawn with the unlit fallback (base texture, the pass's blend);
//                            Heat / Shadow bucket submeshes are not drawn
//   SourceShadowed           the .fxo is used (the original compiles the .fx instead)
//   TechniqueSkipped         the next technique whose passes all have a program is drawn
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/materials/effect_loader.hpp"

namespace alamo::shaderxlat {
class RuntimeDxc;
class ShaderCache;
}  // namespace alamo::shaderxlat

namespace alamo::materials {

enum class ShaderIssueKind : uint8_t {
    SourceOnly = 0,   // <name>.fx without a compiled <name>.fxo: the original compiles the HLSL with D3DX at run
                      // time; the port has no fx_2_0 compiler -> green default under the effect's name
    SourceShadowed,   // <name>.fx and <name>.fxo both reachable: the original compiles the .fx (it wins), the
                      // port loads the .fxo (same result only if that .fxo was compiled from that .fx)
    LoadFailed,       // the .fxo exists but does not parse / load -> green default under its name
    NoTechnique,      // loads, no selectable technique -> the shared green default
    TranslateFailed,  // a track-B pass failed translation or DXC -> unlit fallback draw for its submeshes
    UnsupportedPass,  // a shader pass track B does not take (vs_3_0 / ps_3_0, a VS without a PS or the reverse,
                      // a shader + fixed-function mix) and no hand-written program, in every technique -> unlit
                      // fallback draw
    TechniqueSkipped, // the best technique has such a pass; a lower one is drawn (MaterialEffect::skipped_technique,
                      // the original on hardware where that technique fails ValidateTechnique)
};
inline constexpr size_t kShaderIssueKinds = 7;
// "source_only", "source_shadowed", "load_failed", "no_technique", "translate_failed", "unsupported_pass",
// "technique_skipped".
const char* shader_issue_kind_name(ShaderIssueKind k);

struct ShaderIssue {
    ShaderIssueKind kind = ShaderIssueKind::LoadFailed;
    std::string effect;   // upper-case base name ("MESHFRESNELGLOW")
    std::string path;     // game path of the file concerned (the .fx for Source*, else the .fxo)
    bool in_mod = false;  // that file is reached through a mod (ShaderFileEntry::from_mod)
    std::string detail;   // loader / translator / DXC message ("" when the kind says it all)
};

struct EffectAudit {
    size_t effects = 0;            // distinct effect names in the shader directories
    size_t mod_effects = 0;        // of which a file is reached through a mod
    size_t loaded = 0;             // loaded as a real effect (not a default)
    size_t translated_passes = 0;  // track-B passes of the loaded effects (restore-only passes excluded)
    size_t translated_ok = 0;      // of which translated (and, with DXC, compiled to SPIR-V)
    bool compiled = false;         // DXC was given: TranslateFailed covers DXC errors too
    std::vector<ShaderIssue> issues;  // sorted by (effect, kind)
    size_t count(ShaderIssueKind k) const;
};

// The load-time issues of one effect name (no translation): Source* from the files present, LoadFailed /
// NoTechnique when `loaded` is a default (`err` = the load_or_default() message), UnsupportedPass per
// shader pass with no program. `loaded` = lib.load_or_default(effect). in_mod is left false (callers that
// know the mod files set it). Used by audit_effects() and render::EffectCache::issues().
std::vector<ShaderIssue> effect_load_issues(const EffectLibrary& lib, std::string_view effect, const MaterialEffect& loaded,
                                            std::string_view err);
// Upper-case, '\'-separated form of a game path, the key in_mod lookups use.
std::string shader_path_key(std::string_view path);

// Audits every effect named by `shader_files` (shader_files_from_vfs(vfs, true)): loads it by base name
// through `lib` (the renderer's lookup), and for each track-B pass translates the VS / PS bytecode
// (shaderxlat::translate); with `dxc` the HLSL is compiled too (materials::translate_pass, `cache`
// optional). Deterministic: result order does not depend on input order. Not thread-safe on `lib`.
EffectAudit audit_effects(EffectLibrary& lib, std::span<const ShaderFileEntry> shader_files,
                          const shaderxlat::RuntimeDxc* dxc = nullptr, const shaderxlat::ShaderCache* cache = nullptr);

}  // namespace alamo::materials

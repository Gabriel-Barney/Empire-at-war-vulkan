// Effect audit. See effect_audit.hpp.
#include "alamo/materials/effect_audit.hpp"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>

#include "alamo/materials/register_file.hpp"
#include "alamo/materials/translated.hpp"
#include "alamo/shaderxlat/bytecode.hpp"
#include "alamo/shaderxlat/translate.hpp"
#include "fx2_reader.hpp"

namespace alamo::materials {

namespace {

std::string upper_ascii(std::string_view s) {
    std::string r(s);
    for (char& c : r)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return r;
}

// "Data\Art\Shaders\Terrain\TerrainBump.fxo" -> {"TERRAINBUMP", ".FXO"}.
std::pair<std::string, std::string> split_name(std::string_view path) {
    const size_t slash = path.find_last_of("\\/");
    if (slash != std::string_view::npos) path = path.substr(slash + 1);
    std::string ext;
    const size_t dot = path.find_last_of('.');
    if (dot != std::string_view::npos) {
        ext = upper_ascii(path.substr(dot));
        path = path.substr(0, dot);
    }
    return {upper_ascii(path), ext};
}

bool translate_only(const std::vector<uint8_t>& bytecode, const shaderxlat::TranslateOptions& options, std::string* err) {
    shaderxlat::Program prog;
    if (!shaderxlat::parse(bytecode, prog, err)) return false;
    shaderxlat::Translation t;
    return shaderxlat::translate(prog, options, t, err);
}

// Why a shader pass has neither a hand-written nor a translated program: the shader versions its
// states reference (the loader takes vs/ps up to 2_x, or vs_3_0 with ps_3_0, with both stages present,
// effect_loader resolve_pass).
std::string unsupported_reason(const fx::Effect& e, int technique, int pass) {
    if (technique < 0 || static_cast<size_t>(technique) >= e.techniques.size()) return "no technique";
    const auto& passes = e.techniques[static_cast<size_t>(technique)].passes;
    if (pass < 0 || static_cast<size_t>(pass) >= passes.size()) return "no pass";
    int vs_major = -1, ps_major = -1;
    std::string versions;
    for (const fx::State& s : passes[static_cast<size_t>(pass)].states) {
        const std::span<const uint8_t> b = shader_bytecode(e, s);
        if (b.size() < 4) continue;
        const uint32_t tok = static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
                             (static_cast<uint32_t>(b[2]) << 16) | (static_cast<uint32_t>(b[3]) << 24);
        const uint32_t kind = tok >> 16;
        if (kind != 0xFFFEu && kind != 0xFFFFu) continue;
        const int major = static_cast<int>((tok >> 8) & 0xFFu), minor = static_cast<int>(tok & 0xFFu);
        (kind == 0xFFFEu ? vs_major : ps_major) = major;
        if (!versions.empty()) versions += ", ";
        versions += std::string(kind == 0xFFFEu ? "vs_" : "ps_") + std::to_string(major) + "_" + std::to_string(minor);
    }
    if ((vs_major >= 3) != (ps_major >= 3) && vs_major >= 0 && ps_major >= 0)
        return "shader model 3 mixed with an older stage (D3D9 needs vs_3_0 with ps_3_0) (" + versions + ")";
    if (vs_major < 0 && ps_major >= 0) return "pixel shader without a vertex shader (" + versions + ")";
    if (ps_major < 0 && vs_major >= 0) return "vertex shader without a pixel shader (" + versions + ")";
    if (vs_major < 0 && ps_major < 0) return "no shader bytecode found";
    return "shader + fixed-function mix or unparsable bytecode (" + versions + ")";
}


}  // namespace

const char* shader_issue_kind_name(ShaderIssueKind k) {
    switch (k) {
    case ShaderIssueKind::SourceOnly: return "source_only";
    case ShaderIssueKind::SourceShadowed: return "source_shadowed";
    case ShaderIssueKind::LoadFailed: return "load_failed";
    case ShaderIssueKind::NoTechnique: return "no_technique";
    case ShaderIssueKind::TranslateFailed: return "translate_failed";
    case ShaderIssueKind::UnsupportedPass: return "unsupported_pass";
    case ShaderIssueKind::TechniqueSkipped: return "technique_skipped";
    }
    return "?";
}

size_t EffectAudit::count(ShaderIssueKind k) const {
    return static_cast<size_t>(std::count_if(issues.begin(), issues.end(), [k](const ShaderIssue& i) { return i.kind == k; }));
}

std::string shader_path_key(std::string_view path) {
    std::string r = upper_ascii(path);
    for (char& c : r)
        if (c == '/') c = '\\';
    return r;
}

std::vector<ShaderIssue> effect_load_issues(const EffectLibrary& lib, std::string_view effect, const MaterialEffect& m,
                                            std::string_view err) {
    std::vector<ShaderIssue> out;
    const std::string name = split_name(effect).first;
    const std::optional<std::string> fxo = lib.find_effect_path(effect);
    const std::optional<std::string> src = lib.find_effect_source(effect);
    std::string src_why;
    const FxSourceState st = src ? lib.fx_source_status(effect, &src_why) : FxSourceState::None;
    const bool from_src = st == FxSourceState::Compiled;  // modc t23: the compiled .fx is what load() reads
    if (src && !from_src) {
        // The original compiles this .fx; the port has no compiled copy of it (why: not prebuilt, fxc error, off).
        const std::string reason = st == FxSourceState::Disabled ? std::string("HLSL .fx compilation is off") : src_why;
        if (!fxo) {
            out.push_back({ShaderIssueKind::SourceOnly, name, *src, false,
                           reason + "; drawn with the green default effect"});
            return out;
        }
        out.push_back({ShaderIssueKind::SourceShadowed, name, *src, false,
                       reason + "; the port loads " + *fxo + " in place of this .fx"});
    }
    if (!fxo && !from_src) return out;  // missing: the original's silent green default (D.3), not a shader issue
    const std::string file = from_src ? *src : *fxo;
    if (m.is_default_effect()) {
        const bool no_tech = m.name == kDefaultEffectName;
        out.push_back({no_tech ? ShaderIssueKind::NoTechnique : ShaderIssueKind::LoadFailed, name, file, false, std::string(err)});
        return out;
    }
    if (!m.skipped_technique.empty() && m.effect) {
        const std::string why = unsupported_reason(*m.effect, m.skipped_technique_index, m.skipped_pass);
        out.push_back({ShaderIssueKind::TechniqueSkipped, name, file, false,
                       "technique " + m.skipped_technique + " (" + why + ") skipped; drawn with " + m.technique_name});
    }
    for (const ResolvedPass& p : m.passes) {
        // Same rule as the renderer's LitStatus::Untranslated (render setup_lit).
        if (pass_has_program(p)) continue;
        out.push_back({ShaderIssueKind::UnsupportedPass, name, file, false,
                       "pass " + p.name + ": " + (m.effect ? unsupported_reason(*m.effect, m.technique, p.pass_index) : std::string("no effect"))});
    }
    return out;
}

EffectAudit audit_effects(EffectLibrary& lib, std::span<const ShaderFileEntry> shader_files,
                          const shaderxlat::RuntimeDxc* dxc, const shaderxlat::ShaderCache* cache) {
    // Ordered by name: the result does not depend on the input order.
    std::map<std::string, bool> names;  // effect base name -> some file of it comes from a mod
    std::map<std::string, bool> mod_path;  // shader_path_key(path) -> from_mod
    for (const ShaderFileEntry& f : shader_files) {
        auto [name, ext] = split_name(f.path);
        if (name.empty() || (ext != ".FX" && ext != ".FXO")) continue;
        names[name] = names[name] || f.from_mod;
        bool& pm = mod_path[shader_path_key(f.path)];
        pm = pm || f.from_mod;
    }
    auto in_mod = [&](const std::string& path) {
        auto it = mod_path.find(shader_path_key(path));
        return it != mod_path.end() && it->second;
    };

    EffectAudit a;
    a.compiled = dxc != nullptr;
    for (const auto& [name, any_mod] : names) {
        ++a.effects;
        if (any_mod) ++a.mod_effects;
        // The renderer looks submesh effects up by base name; so does the audit (mod dirs first).
        std::string err;
        const MaterialEffect m = lib.load_or_default(name, &err);
        if (lib.find_effect_source(name)) {
            ++a.sources;
            if (lib.fx_source_status(name) == FxSourceState::Compiled) ++a.sources_compiled;
        }
        for (ShaderIssue& i : effect_load_issues(lib, name, m, err)) {
            i.in_mod = in_mod(i.path);
            a.issues.push_back(std::move(i));
        }
        if (m.is_default_effect()) continue;
        ++a.loaded;
        const bool fxo_mod = in_mod(m.path);
        for (const ResolvedPass& p : m.passes) {
            if (p.restore_only || p.program_status != ProgramStatus::Translated) continue;
            ++a.translated_passes;
            std::string terr;
            bool ok = false;
            if (dxc) {
                ok = translate_pass(p, *dxc, cache, &terr) != nullptr;
            } else {
                std::string e;
                ok = translate_only(p.vs_bytecode, p.vs_options, &e);
                if (!ok) terr = p.name + " VS: " + e;
                if (ok) {
                    ok = translate_only(p.ps_bytecode, p.ps_options, &e);
                    if (!ok) terr = p.name + " PS: " + e;
                }
            }
            if (ok) ++a.translated_ok;
            else a.issues.push_back({ShaderIssueKind::TranslateFailed, name, m.path, fxo_mod, terr});
        }
    }
    std::stable_sort(a.issues.begin(), a.issues.end(), [](const ShaderIssue& x, const ShaderIssue& y) {
        return std::tie(x.effect, x.kind) < std::tie(y.effect, y.kind);
    });
    return a;
}

}  // namespace alamo::materials

// alamo_effect_audit - lists what the port does with every effect a game + mod set ships
// (materials::audit_effects, docs/MODS.md shader section, roadmap modc t11).
//
//   alamo_effect_audit <FoC dir with Data\> [MODPATH=<dir>]... [FALLBACKPATH=<dir>]... [--no-dxc] [--json <file>|-]
//                      [--fx-cache <dir>] [--fx-prebuild] [--fxc <fxc.exe>] [--no-fx]
//
// Effect sources (.fx, modc t23) are taken from the FxCache (--fx-cache, default FxCache::default_directory());
// --fx-prebuild first compiles the missing ones like alamo_fx_prebuild; --no-fx audits the t11 behaviour.
//
// Prints one line per issue (kind, mod/base, effect, path, detail) and a summary; exit code 0 (issues are
// findings, not errors), 2 for bad arguments or a VFS that cannot be created.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/materials/effect_audit.hpp"
#include "alamo/materials/effect_loader.hpp"
#include "alamo/shaderxlat/dxc.hpp"
#include "alamo/vfs/vfs.hpp"
#include "fx_compile.hpp"

using namespace alamo;

namespace {

std::string json_escape(const std::string& s) {
    std::string o;
    for (const char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char b[8];
                std::snprintf(b, sizeof b, "\\u%04x", static_cast<unsigned>(static_cast<unsigned char>(c)));
                o += b;
            } else {
                o += c;
            }
        }
    }
    return o;
}

void write_json(std::ostream& os, const materials::EffectAudit& a, double ms) {
    os << "{\n  \"schema\": \"alamo.effect_audit\", \"version\": 1,\n";
    os << "  \"effects\": " << a.effects << ", \"mod_effects\": " << a.mod_effects << ", \"loaded\": " << a.loaded
       << ", \"translated_passes\": " << a.translated_passes << ", \"translated_ok\": " << a.translated_ok
       << ", \"compiled\": " << (a.compiled ? "true" : "false") << ", \"fx_sources\": " << a.sources
       << ", \"fx_sources_compiled\": " << a.sources_compiled << ", \"ms\": " << static_cast<long long>(ms) << ",\n";
    os << "  \"issues\": [";
    for (size_t i = 0; i < a.issues.size(); ++i) {
        const materials::ShaderIssue& s = a.issues[i];
        os << (i ? ",\n" : "\n") << "    {\"kind\": \"" << materials::shader_issue_kind_name(s.kind) << "\", \"effect\": \""
           << json_escape(s.effect) << "\", \"path\": \"" << json_escape(s.path) << "\", \"in_mod\": " << (s.in_mod ? "true" : "false")
           << ", \"detail\": \"" << json_escape(s.detail) << "\"}";
    }
    os << (a.issues.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: alamo_effect_audit <FoC dir> [MODPATH=<dir>]... [FALLBACKPATH=<dir>]... [--no-dxc] [--json <file>|-] "
                    "[--fx-cache <dir>] [--fx-prebuild] [--fxc <exe>] [--no-fx]\n");
        return 2;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = argv[1];
    std::vector<std::string> switches;
    bool use_dxc = true, use_fx = true, fx_prebuild = false;
    std::string json, fx_cache, fxc_arg;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--no-dxc") == 0) use_dxc = false;
        else if (std::strcmp(argv[i], "--no-fx") == 0) use_fx = false;
        else if (std::strcmp(argv[i], "--fx-prebuild") == 0) fx_prebuild = true;
        else if (std::strcmp(argv[i], "--fx-cache") == 0 && i + 1 < argc) fx_cache = argv[++i];
        else if (std::strcmp(argv[i], "--fxc") == 0 && i + 1 < argc) fxc_arg = argv[++i];
        else if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) json = argv[++i];
        else switches.emplace_back(argv[i]);
    }
    if (!vfs::parse_command_line(switches, cfg)) {
        std::printf("bad switch (empty value)\n");
        return 2;
    }
    std::string err;
    auto v = vfs::Vfs::create(cfg, &err);
    if (!v) {
        std::printf("vfs: %s\n", err.c_str());
        return 2;
    }
    shaderxlat::RuntimeDxc dxc;
    bool have_dxc = false;
    if (use_dxc) {
        std::string derr;
        have_dxc = dxc.load({}, &derr);
        if (!have_dxc) std::printf("runtime DXC not available (%s): translation only, no compile\n", derr.c_str());
    }
    materials::LoadOptions lo;
    lo.use_fx_sources = use_fx;
    if (!fx_cache.empty()) lo.fx_cache_dir = path_from_utf8(fx_cache);
    const std::vector<materials::ShaderFileEntry> files = materials::shader_files_from_vfs(*v, true);
    if (use_fx && fx_prebuild) {
        const auto p0 = std::chrono::steady_clock::now();
        const materials::EffectLibrary probe(*v, lo);
        std::vector<materials::FxPrebuildItem> items;
        std::string perr;
        const std::vector<std::string> paths = materials::fx_source_paths(probe, files);
        const std::filesystem::path fxc = fxc_arg.empty() ? materials::default_fxc_path() : path_from_utf8(fxc_arg);
        if (!materials::prebuild_fx_sources(*v, paths, probe.fx_cache(), fxc, false, items, &perr))
            std::printf("fx prebuild: %s\n", perr.c_str());
        size_t ok = 0;
        for (const materials::FxPrebuildItem& it : items) ok += it.ok ? 1 : 0;
        std::printf("fx prebuild: %zu sources, %zu compiled or cached, %.0f ms\n", items.size(), ok,
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - p0).count());
    }
    const auto t0 = std::chrono::steady_clock::now();
    materials::EffectLibrary lib(*v, lo);
    lib.set_shader_files(materials::shader_files_from_vfs(*v));
    const materials::EffectAudit a = materials::audit_effects(lib, files, have_dxc ? &dxc : nullptr, nullptr);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    for (const materials::ShaderIssue& s : a.issues)
        std::printf("%-16s %-4s %-28s %s%s%s\n", materials::shader_issue_kind_name(s.kind), s.in_mod ? "mod" : "base",
                    s.effect.c_str(), s.path.c_str(), s.detail.empty() ? "" : "  ", s.detail.c_str());
    std::printf("effects %zu (from mods %zu), loaded %zu, track-B passes %zu, %s %zu, issues %zu, %.0f ms\n", a.effects,
                a.mod_effects, a.loaded, a.translated_passes, a.compiled ? "compiled" : "translated", a.translated_ok,
                a.issues.size(), ms);
    if (a.sources) std::printf("  .fx sources %zu, loaded compiled %zu\n", a.sources, a.sources_compiled);
    for (size_t k = 0; k < materials::kShaderIssueKinds; ++k) {
        const auto kind = static_cast<materials::ShaderIssueKind>(k);
        if (const size_t n = a.count(kind)) std::printf("  %s: %zu\n", materials::shader_issue_kind_name(kind), n);
    }
    if (json == "-") {
        write_json(std::cout, a, ms);
    } else if (!json.empty()) {
        std::ofstream f(json, std::ios::binary);
        write_json(f, a, ms);
        if (!f) {
            std::printf("cannot write %s\n", json.c_str());
            return 2;
        }
    }
    return 0;
}

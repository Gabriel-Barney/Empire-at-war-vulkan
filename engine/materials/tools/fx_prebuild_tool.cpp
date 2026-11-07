// alamo_fx_prebuild - compiles every effect source (`<name>.fx`) a game + mod set ships into the FxCache, so
// the loader takes it in place of the `.fxo` like the original (roadmap modc t23, docs/MODS.md 4.1).
//
//   alamo_fx_prebuild <FoC dir with Data\> [MODPATH=<dir>]... [FALLBACKPATH=<dir>]... [--fxc <fxc.exe>]
//                     [--cache <dir>] [--force] [--json <file>|-]
//
// Sources are found through the same VFS and search order as the loader (mod folders first); the cache key
// is the source plus every file it #includes, so an unchanged source is not compiled again (--force does).
// Default cache: $ALAMO_FX_CACHE, else %LOCALAPPDATA%\alamo\fx; default fxc: $ALAMO_FXC, else the toolchain's
// Windows SDK. Exit code 0 when every source was compiled or found in the cache (a source fxc rejects is a
// finding, reported per line, and still 0); 1 when fxc is missing or the cache cannot be written; 2 for bad
// arguments or a VFS that cannot be created.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/materials/effect_loader.hpp"
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

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: alamo_fx_prebuild <FoC dir> [MODPATH=<dir>]... [FALLBACKPATH=<dir>]... [--fxc <exe>] [--cache <dir>] "
                    "[--force] [--json <file>|-]\n");
        return 2;
    }
    vfs::VfsConfig cfg;
    cfg.foc_root = argv[1];
    std::vector<std::string> switches;
    std::string json, fxc_arg, cache_arg;
    bool force = false;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--force") == 0) force = true;
        else if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) json = argv[++i];
        else if (std::strcmp(argv[i], "--fxc") == 0 && i + 1 < argc) fxc_arg = argv[++i];
        else if (std::strcmp(argv[i], "--cache") == 0 && i + 1 < argc) cache_arg = argv[++i];
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
    const std::filesystem::path fxc = fxc_arg.empty() ? materials::default_fxc_path() : path_from_utf8(fxc_arg);
    const materials::FxCache cache(cache_arg.empty() ? materials::FxCache::default_directory() : path_from_utf8(cache_arg));
    const auto t0 = std::chrono::steady_clock::now();
    const materials::EffectLibrary lib(*v);
    const std::vector<materials::ShaderFileEntry> files = materials::shader_files_from_vfs(*v, true);
    const std::vector<std::string> paths = materials::fx_source_paths(lib, files);
    std::vector<materials::FxPrebuildItem> items;
    const bool all = materials::prebuild_fx_sources(*v, paths, cache, fxc, force, items, &err);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();

    size_t compiled = 0, cached = 0, failed = 0;
    for (const materials::FxPrebuildItem& it : items) {
        const char* what = it.ok ? (it.cached ? "cached" : "compiled") : (it.cached ? "failed (cached)" : "failed");
        if (it.ok) (it.cached ? cached : compiled) += 1;
        else ++failed;
        std::printf("%-16s %-48s %6.0f ms%s%s\n", what, it.path.c_str(), it.ms, it.detail.empty() ? "" : "  ", it.detail.c_str());
    }
    std::printf("sources %zu: compiled %zu, from cache %zu, failed %zu; fxc %s; cache %s; %.0f ms\n", items.size(), compiled,
                cached, failed, fxc.empty() ? "(none)" : materials::fxc_identity(fxc).c_str(),
                path_to_utf8(cache.directory()).c_str(), ms);
    if (!all) std::printf("error: %s\n", err.c_str());
    if (!json.empty()) {
        std::string o = "{\n  \"schema\": \"alamo.fx_prebuild\", \"version\": 1,\n";
        o += "  \"fxc\": \"" + json_escape(materials::fxc_identity(fxc)) + "\", \"cache\": \"" +
             json_escape(path_to_utf8(cache.directory())) + "\", \"ms\": " + std::to_string(static_cast<long long>(ms)) +
             ", \"ok\": " + (all ? "true" : "false") + ",\n  \"sources\": [";
        for (size_t i = 0; i < items.size(); ++i) {
            const materials::FxPrebuildItem& it = items[i];
            o += std::string(i ? ",\n" : "\n") + "    {\"path\": \"" + json_escape(it.path) + "\", \"key\": \"" + it.key +
                 "\", \"ok\": " + (it.ok ? "true" : "false") + ", \"cached\": " + (it.cached ? "true" : "false") +
                 ", \"ms\": " + std::to_string(static_cast<long long>(it.ms)) + ", \"detail\": \"" + json_escape(it.detail) + "\"}";
        }
        o += items.empty() ? "]\n}\n" : "\n  ]\n}\n";
        if (json == "-") {
            std::cout << o;
        } else {
            std::ofstream f(json, std::ios::binary);
            f << o;
            if (!f) {
                std::printf("cannot write %s\n", json.c_str());
                return 2;
            }
        }
    }
    return all ? 0 : 1;
}

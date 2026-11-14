// alamo_shaderxlat_tool - offline batch translation of FXO shader bytecode (build-time path of
// PORTING.md track B). For every VS/PS blob of the given effects it writes
//   <out>/<EFFECT>_<hash16>.asm    D3D9 disassembly (also written when the translation fails)
//   <out>/<EFFECT>_<hash16>.hlsl   translated HLSL (entry VSMain / PSMain)
//   <out>/<EFFECT>_<hash16>.json   interface description (ShaderInterface::to_json)
//   <out>/<EFFECT>_<hash16>.spv    SPIR-V compiled by dxc.exe (same arguments as the runtime path)
// and, with --cache, stores the results in the shader disk cache under the key the engine uses
// (the compiler identity comes from dxcompiler.dll next to dxc.exe).
//
//   alamo_shaderxlat_tool [--fxo <file.fxo>]... [--game <dir with Data\>] [--eaw <dir>] --out <dir>
//                         [--dxc <dxc.exe>] [--cache <dir>] [--no-compile]
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/shaderxlat/shaderxlat.hpp"
#include "fx2_reader.hpp"

#ifdef ALAMO_SHADERXLAT_HAVE_VFS
#include "alamo/vfs/vfs.hpp"
#endif

using namespace alamo::shaderxlat;
namespace fs = std::filesystem;

namespace {

struct Input {
    std::string name;   // effect name (upper case, no extension)
    std::vector<uint8_t> bytes;
};

bool is_shader(const std::vector<uint8_t>& d) {
    if (d.size() < 8) return false;
    uint32_t v;
    std::memcpy(&v, d.data(), 4);
    return (v >> 16) == 0xFFFEu || (v >> 16) == 0xFFFFu;
}

std::string quote(const std::string& s) { return "\"" + s + "\""; }

int run_command(const std::string& cmd) {
    return std::system(("\"" + cmd + "\"").c_str());  // cmd.exe /c strips one level of outer quotes
}

void usage() {
    std::printf("usage: alamo_shaderxlat_tool [--fxo <file>]... [--game <dir>] [--eaw <dir>] --out <dir> "
                "[--dxc <dxc.exe>] [--cache <dir>] [--no-compile]\n");
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> fxos;
    std::string game, eaw, out, dxc = ALAMO_SHADERXLAT_DXC_EXE, cache_dir;
    bool compile = true;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](std::string& v) {
            if (i + 1 >= argc) return false;
            v = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--fxo" && next(v)) fxos.push_back(v);
        else if (a == "--game" && next(v)) game = v;
        else if (a == "--eaw" && next(v)) eaw = v;
        else if (a == "--out" && next(v)) out = v;
        else if (a == "--dxc" && next(v)) dxc = v;
        else if (a == "--cache" && next(v)) cache_dir = v;
        else if (a == "--no-compile") compile = false;
        else {
            usage();
            return 2;
        }
    }
    if (out.empty() || (fxos.empty() && game.empty() && eaw.empty())) {
        usage();
        return 2;
    }
    std::vector<Input> inputs;
    for (const std::string& f : fxos) {
        Input in;
        if (!alamo::read_host_file(f, in.bytes)) {
            std::printf("cannot read %s\n", f.c_str());
            return 1;
        }
        in.name = fs::path(f).stem().string();
        inputs.push_back(std::move(in));
    }
    for (const std::string& root : {game, eaw}) {
        if (root.empty()) continue;
#ifdef ALAMO_SHADERXLAT_HAVE_VFS
        alamo::vfs::VfsConfig cfg;
        cfg.foc_root = root;
        cfg.eaw_root = std::string();
        std::string err;
        auto v = alamo::vfs::Vfs::create(cfg, &err);
        if (!v) {
            std::printf("VFS: %s\n", err.c_str());
            return 1;
        }
        std::set<std::string> files;
        for (const char* g : {"DATA\\ART\\SHADERS\\*.FXO", "DATA\\ART\\SHADERS\\**\\*.FXO"})
            for (const std::string& f : v->list(g)) files.insert(f);
        for (const std::string& f : files) {
            Input in;
            if (!v->read(f, in.bytes)) continue;
            std::string n = f.substr(f.find_last_of('\\') + 1);
            in.name = n.substr(0, n.find('.'));
            inputs.push_back(std::move(in));
        }
#else
        std::printf("built without alamo_vfs: --game/--eaw unavailable\n");
        return 1;
#endif
    }
    std::error_code ec;
    fs::create_directories(out, ec);

    RuntimeDxc rt;
    std::unique_ptr<ShaderCache> cache;
    if (!cache_dir.empty()) {
        std::string err;
        if (rt.load(fs::path(dxc).parent_path().string(), &err)) cache = std::make_unique<ShaderCache>(cache_dir);
        else std::printf("--cache ignored: %s\n", err.c_str());
    }

    std::set<uint64_t> seen;
    int blobs = 0, translated = 0, compiled = 0, failed = 0;
    for (const Input& in : inputs) {
        alamo::fx::Effect e;
        try {
            e = alamo::fx::parseEffect(in.bytes.data(), in.bytes.size());
        } catch (const std::exception& ex) {
            std::printf("%s: %s\n", in.name.c_str(), ex.what());
            ++failed;
            continue;
        }
        std::vector<std::vector<uint8_t>> shaders;
        for (const auto& kv : e.objects)
            if (is_shader(kv.second.data)) shaders.push_back(kv.second.data);
        for (const auto& t : e.techniques)
            for (const auto& p : t.passes)
                for (const auto& s : p.states)
                    if (s.kind == alamo::fx::State::Kind::Shader && is_shader(s.blob)) shaders.push_back(s.blob);
        for (std::vector<uint8_t>& b : shaders) {
            Program prog;
            std::string err;
            if (parse(b, prog, nullptr)) b.resize(static_cast<size_t>(prog.token_count) * 4);
            const uint64_t h = alamo::fnv1a64(b.data(), b.size());
            if (!seen.insert(h).second) continue;
            ++blobs;
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
            const std::string base = (fs::path(out) / (in.name + "_" + hex)).string();
            Translation t;
            if (!parse(b, prog, &err)) {
                std::printf("%s_%s: %s\n", in.name.c_str(), hex, err.c_str());
                ++failed;
                continue;
            }
            std::ofstream(base + ".asm", std::ios::binary) << disassemble(prog);
            if (!translate(prog, TranslateOptions{}, t, &err)) {
                std::printf("%s_%s: %s\n", in.name.c_str(), hex, err.c_str());
                ++failed;
                continue;
            }
            ++translated;
            std::ofstream(base + ".hlsl", std::ios::binary) << t.hlsl;
            std::ofstream(base + ".json", std::ios::binary) << t.iface.to_json() << "\n";
            if (!compile) continue;
            std::string cmd = quote(dxc) + " -nologo";
            for (const std::string& a : dxc_arguments(t)) cmd += " " + a;
            cmd += " -Fo " + quote(base + ".spv") + " " + quote(base + ".hlsl") + " > " + quote(base + ".log") + " 2>&1";
            if (run_command(cmd) != 0) {
                std::printf("%s_%s: dxc failed (see %s.log)\n", in.name.c_str(), hex, base.c_str());
                ++failed;
                continue;
            }
            fs::remove(base + ".log", ec);
            ++compiled;
            if (cache) {
                std::vector<uint8_t> spv;
                if (alamo::read_host_file(base + ".spv", spv) && spv.size() % 4 == 0) {
                    CacheEntry ce;
                    ce.spirv.resize(spv.size() / 4);
                    std::memcpy(ce.spirv.data(), spv.data(), spv.size());
                    ce.iface = t.iface;
                    ce.entry = t.entry;
                    cache->store(CacheKey::make(b, TranslateOptions{}, rt.version(), t), ce);
                }
            }
        }
    }
    std::printf("%zu effects, %d unique shader blobs, %d translated, %d compiled, %d failed\n", inputs.size(), blobs, translated,
                compiled, failed);
    return failed ? 1 : 0;
}

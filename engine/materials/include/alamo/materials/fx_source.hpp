// Effect sources (`<name>.fx`, roadmap modc t23). The original compiles a `.fx` with D3DX at run time and
// prefers it to a `.fxo` of the same name (RENDER.md 6). The port compiles sources ahead of time: the
// prebuild tool `alamo_fx_prebuild` (engine/materials/tools, fx_compile.hpp) runs the toolchain's fxc.exe
// (`/T fx_2_0`) on every reachable `.fx` and stores the result in an FxCache keyed by the source text and
// every file it #includes. EffectLibrary::load() takes a compiled source from that cache; without an entry,
// or when the compile failed, it keeps the t11 fallback (the `.fxo`, else the green default). The engine
// itself never runs a compiler and has no D3DCompiler dependency.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {
class FileProvider;
}

namespace alamo::materials {

inline constexpr const char* kFxProfile = "fx_2_0";  // fxc target of every compile (RENDER.md 6: D3DX effects)
inline constexpr uint32_t kFxKeyVersion = 1;         // bump when the key or the compile arguments change

struct FxSourceFile {
    std::string path;            // game path as found ("Data\\Art\\Shaders\\AlamoEngine.fxh")
    std::vector<uint8_t> bytes;
};

struct FxSource {
    std::string path;                  // game path of the `.fx`
    std::vector<FxSourceFile> files;   // [0] = the `.fx`, then every resolved #include, first-seen order
    std::vector<std::string> missing;  // #include names that resolved to no file (fxc fails if one is used)
    // 32 hex digits: kFxKeyVersion, kFxProfile and every file (name as written + bytes) and missing name.
    std::string key() const;
};

// Reads `path` and, recursively, every file it #includes ("x" or <x>), through `files` (so mod folders come
// first, like every other game file). An include is looked up relative to the including file's directory,
// then under Data\Art\Shaders\ (port choice; fxc gets the same two places, fx_compile.cpp). Lines inside
// /* */ or after // are ignored; #if is not evaluated, so an include in a disabled branch is still read
// (or listed in `missing`). False (and *err) when `path` cannot be read.
bool collect_fx_source(const FileProvider& files, std::string_view path, FxSource& out, std::string* err = nullptr);

struct FxCacheEntry {
    bool ok = false;            // fxc succeeded: `fxo` holds the compiled effect
    std::string compiler;       // identity of the fxc that produced it (fx_compile.hpp fxc_identity())
    std::string log;            // fxc output (warnings; the errors when !ok)
    std::vector<uint8_t> fxo;
};

// One file per key, `<key>.afx`, written to a unique temporary name and renamed into place, so processes
// and threads can share a directory (a reader sees no file or a complete one; a damaged one is a miss).
class FxCache {
public:
    FxCache();                                    // default_directory()
    explicit FxCache(std::filesystem::path directory);
    // $ALAMO_FX_CACHE if set, else %LOCALAPPDATA%\alamo\fx (never inside the game install).
    static std::filesystem::path default_directory();
    const std::filesystem::path& directory() const { return dir_; }

    bool load(const std::string& key, FxCacheEntry& out) const;   // false: no entry / damaged / other key
    bool store(const std::string& key, const FxCacheEntry& entry) const;  // creates the directory

private:
    std::filesystem::path dir_;
};

// What the loader does with an effect's `.fx` (EffectLibrary::fx_source_status()).
enum class FxSourceState : uint8_t {
    None = 0,     // no `<name>.fx` reachable
    Disabled,     // LoadOptions::use_fx_sources off: the t11 behaviour (.fxo, else green default)
    NotCompiled,  // no cache entry for this source + includes (run alamo_fx_prebuild): t11 fallback
    Failed,       // fxc failed (or its output does not parse): t11 fallback
    Compiled,     // the compiled source is loaded in place of any `.fxo` (the original's choice)
};

}  // namespace alamo::materials

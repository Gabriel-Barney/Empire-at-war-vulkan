// Disk cache of translated + compiled programs, keyed by (bytecode hash, interface key, translator
// version, compiler version). One file per entry, written to a unique temporary file and renamed
// into place, so any number of threads and processes can share a cache directory: readers see
// either no file or a complete one (a damaged or foreign file fails its checksum / key check and
// counts as a miss).
#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/shaderxlat/translate.hpp"

namespace alamo::shaderxlat {

struct CacheKey {
    uint64_t blob_fnv = 0;        // FNV-1a 64 of the bytecode
    uint32_t blob_crc = 0;        // CRC-32 of the bytecode
    uint32_t blob_size = 0;
    uint64_t options_key = 0;     // TranslateOptions::key()
    uint32_t translator_version = kTranslatorVersion;
    std::string compiler;         // RuntimeDxc::version() or the offline compiler's identity
    uint64_t source_hash = 0;     // source_hash() of the emitted HLSL + DXC arguments

    // Keys on the bytecode, the options and the translation's HLSL text + dxc_arguments(t), so a translator or
    // argument change can never return stale SPIR-V even without a kTranslatorVersion bump.
    static CacheKey make(std::span<const uint8_t> bytecode, const TranslateOptions& options, std::string_view compiler,
                         const Translation& t);
    static uint64_t source_hash_of(std::string_view hlsl, std::span<const std::string> args);
    std::string describe() const;   // full key text, stored in the file and compared on load
    std::string file_name() const;  // "<32 hex digits>.axs"
};

struct CacheEntry {
    std::vector<uint32_t> spirv;
    ShaderInterface iface;
    std::string entry;            // SPIR-V entry point ("VSMain" / "PSMain")
};

class ShaderCache {
public:
    ShaderCache();                                         // default_directory()
    explicit ShaderCache(std::filesystem::path directory);

    // $ALAMO_SHADER_CACHE if set, else %LOCALAPPDATA%\alamo\shaders (Windows) /
    // $XDG_CACHE_HOME/alamo/shaders or ~/.cache/alamo/shaders.
    static std::filesystem::path default_directory();
    const std::filesystem::path& directory() const { return dir_; }

    bool load(const CacheKey& key, CacheEntry& out) const;
    bool store(const CacheKey& key, const CacheEntry& entry) const;   // creates the directory

private:
    std::filesystem::path dir_;
};

}  // namespace alamo::shaderxlat

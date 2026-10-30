// alamo_shaderxlat - PORTING.md track B: D3D9 SM1-2 bytecode -> HLSL (SM6) -> SPIR-V.
//
//   Program prog;            parse(bytecode, prog, &err)                 bytecode.hpp
//   Translation t;           translate(prog, options, t, &err)           translate.hpp (HLSL + ShaderInterface)
//   RuntimeDxc dxc;          dxc.load(); dxc.compile(t, spirv, &diag)    dxc.hpp
//   ShaderCache cache;       cache.load(key, entry) / store(key, entry)  cache.hpp
//   Interpreter ref;         ref.init(prog, options); run_vs / run_ps    interpreter.hpp (CPU reference)
//
// translate_and_compile() chains them with the cache for the engine's load path.
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/shaderxlat/bytecode.hpp"
#include "alamo/shaderxlat/cache.hpp"
#include "alamo/shaderxlat/dxc.hpp"
#include "alamo/shaderxlat/translate.hpp"

namespace alamo::shaderxlat {

struct CompiledProgram {
    std::vector<uint32_t> spirv;
    ShaderInterface iface;
    std::string entry;            // "VSMain" / "PSMain"
    bool from_cache = false;
    std::string hlsl;             // only when translated in this call (not from the cache)
};

// bytecode -> SPIR-V + interface. Looks the cache up first (when `cache` is set), stores new
// results. Thread-safe as long as `dxc` and `cache` are shared read-only (they are).
bool translate_and_compile(std::span<const uint8_t> bytecode, const TranslateOptions& options, const RuntimeDxc& dxc,
                           const ShaderCache* cache, CompiledProgram& out, std::string* err = nullptr);

}  // namespace alamo::shaderxlat

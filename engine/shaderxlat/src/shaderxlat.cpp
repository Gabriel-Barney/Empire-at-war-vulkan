#include "alamo/shaderxlat/shaderxlat.hpp"

namespace alamo::shaderxlat {

bool translate_and_compile(std::span<const uint8_t> bytecode, const TranslateOptions& options, const RuntimeDxc& dxc,
                           const ShaderCache* cache, CompiledProgram& out, std::string* err) {
    out = CompiledProgram{};
    if (!dxc.loaded()) {
        if (err) *err = "DXC not loaded";
        return false;
    }
    // Parsing + translation are cheap (well under a millisecond) next to DXC; the key covers the
    // bytes up to END (padded FXO objects and exact blobs share entries) and the emitted HLSL + DXC
    // arguments, so a translator change can never be served stale SPIR-V.
    Program prog;
    std::string e;
    if (!parse(bytecode, prog, &e)) {
        if (err) *err = "parse: " + e;
        return false;
    }
    Translation t;
    if (!translate(prog, options, t, &e)) {
        if (err) *err = "translate: " + e;
        return false;
    }
    const std::span<const uint8_t> exact = bytecode.first(static_cast<size_t>(prog.token_count) * 4);
    const CacheKey key = CacheKey::make(exact, options, dxc.version(), t);
    if (cache) {
        CacheEntry ce;
        if (cache->load(key, ce)) {
            out.spirv = std::move(ce.spirv);
            out.iface = std::move(ce.iface);
            out.entry = std::move(ce.entry);
            out.from_cache = true;
            return true;
        }
    }
    std::string diag;
    if (!dxc.compile(t, out.spirv, &diag)) {
        if (err) *err = "dxc: " + diag;
        return false;
    }
    out.iface = t.iface;
    out.entry = t.entry;
    out.hlsl = std::move(t.hlsl);
    if (cache) cache->store(key, {out.spirv, out.iface, out.entry});
    return true;
}

}  // namespace alamo::shaderxlat

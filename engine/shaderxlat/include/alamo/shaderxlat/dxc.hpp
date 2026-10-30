// Runtime HLSL -> SPIR-V compilation through the DXC library (approved by Gabriel 2026-10-26).
// dxcompiler.dll (Windows; dxil.dll next to it) / libdxcompiler.so (Linux) is loaded dynamically
// (LoadLibrary/GetProcAddress "DxcCreateInstance", dlopen/dlsym); nothing links against it. The
// few COM interfaces used are declared in dxc.cpp from their documented IIDs/CLSIDs (no dxcapi.h).
//
// Thread safety: compile() may be called from any number of threads. DXC compiler instances are
// not thread-safe, so each call borrows an instance from a mutex-protected pool (created on demand,
// released by the destructor).
#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/shaderxlat/translate.hpp"

namespace alamo::shaderxlat {

class RuntimeDxc {
public:
    RuntimeDxc();
    ~RuntimeDxc();
    RuntimeDxc(const RuntimeDxc&) = delete;
    RuntimeDxc& operator=(const RuntimeDxc&) = delete;

    // Loads the library from `dir` when given; otherwise tries $ALAMO_DXC_DIR, then the executable's
    // folder (Linux also the dlopen search path; Windows never searches the CWD/PATH). Windows loads
    // by absolute path with LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | SYSTEM32 and pre-loads the dxil.dll
    // next to dxcompiler.dll. False (+ *err) when not found or not usable.
    bool load(const std::string& dir = {}, std::string* err = nullptr);
    bool loaded() const;
    // "dxcompiler <major>.<minor> (<commit count> <commit hash>)", part of the shader cache key;
    // " [size <n> fnv <hash>]" of the library file is appended when the version query fails.
    const std::string& version() const;
    const std::string& library_path() const;

    // HLSL text -> SPIR-V words with DXC command-line arguments (e.g. dxc_arguments()).
    // *diagnostics receives DXC's error/warning text (also on success when there are warnings).
    bool compile(std::string_view hlsl, std::span<const std::string> args, std::vector<uint32_t>& spirv,
                 std::string* diagnostics = nullptr) const;
    bool compile(const Translation& t, std::vector<uint32_t>& spirv, std::string* diagnostics = nullptr) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace alamo::shaderxlat

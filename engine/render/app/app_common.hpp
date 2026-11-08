// Shared bits of the alamo_render.exe sub-mains (CU4 t03): main.cpp, sim_main.cpp (--sim), galactic/app/galactic_main.cpp
// (--galactic) and particles/app/particles_main.cpp (--particles), which used to carry their own copies. Header-only;
// all four TUs are in one exe, so everything here is inline.
//   Offscreen        alamo/render/offscreen.hpp (PNG captures; a debug name and the swapchain format as parameters)
//   env_or_empty     an environment variable, empty when unset
//   cache_dir_of     --cache-dir, else render::default_pipeline_cache_dir() (pipeline cache)
//   shader_cache_of  --cache-dir\shaders, else empty = shaderxlat's default (track B translation cache)
// Tool code only: nothing here is read by the sim.
#pragma once
#include <cstdlib>
#include <filesystem>
#include <string>

#include "alamo/core/file_provider.hpp"
#include "alamo/render/offscreen.hpp"
#include "alamo/render/pipelines.hpp"

namespace alamo::render_app {

using render::Offscreen;
using render::OffscreenDesc;

inline std::string env_or_empty(const char* name) {
#ifdef _MSC_VER
    char* v = nullptr;
    size_t len = 0;
    std::string out;
    if (_dupenv_s(&v, &len, name) == 0 && v) {
        out = v;
        std::free(v);
    }
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
#endif
}

inline std::filesystem::path cache_dir_of(const std::string& cache_dir) {
    return cache_dir.empty() ? render::default_pipeline_cache_dir() : path_from_utf8(cache_dir);
}
inline std::filesystem::path shader_cache_of(const std::string& cache_dir) {
    return cache_dir.empty() ? std::filesystem::path() : path_from_utf8(cache_dir) / "shaders";
}

}  // namespace alamo::render_app

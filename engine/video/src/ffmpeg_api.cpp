// Runtime loading of the FFmpeg shared libraries (LGPL dynamic linking; see include/alamo/video/video.hpp).
#include "ffmpeg_api.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/video/video.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#ifndef ALAMO_VIDEO_FFMPEG_DEFAULT_DIR
#define ALAMO_VIDEO_FFMPEG_DEFAULT_DIR ""
#endif

namespace alamo::video {
namespace detail {
namespace {

#define ALAMO_STR2(x) #x
#define ALAMO_STR(x) ALAMO_STR2(x)

struct LibName {
    const char* key;
    std::string file;
};

std::vector<LibName> lib_names() {
    return {{"avutil", "avutil-" ALAMO_STR(LIBAVUTIL_VERSION_MAJOR) ".dll"},
            {"avcodec", "avcodec-" ALAMO_STR(LIBAVCODEC_VERSION_MAJOR) ".dll"},
            {"avformat", "avformat-" ALAMO_STR(LIBAVFORMAT_VERSION_MAJOR) ".dll"}};
}

using Handle = void*;

std::filesystem::path exe_dir() {
    std::wstring buf(1024, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            break;
        }
        buf.resize(buf.size() * 2);
    }
    return std::filesystem::path(buf).parent_path();
}

Handle open_lib(const std::filesystem::path& p) {
    // Altered search path: the DLL's own folder is searched for its dependencies (avcodec -> avutil).
    return reinterpret_cast<Handle>(LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH));
}

void* find_sym(Handle h, const char* name) {
    FARPROC p = GetProcAddress(reinterpret_cast<HMODULE>(h), name);
    void* out = nullptr;
    static_assert(sizeof(p) == sizeof(out));
    std::memcpy(&out, &p, sizeof(out));
    return out;
}

template <class Fn>
void assign(Fn& dst, void* sym) {
    static_assert(sizeof(Fn) == sizeof(void*));
    std::memcpy(&dst, &sym, sizeof(dst));
}

struct State {
    std::mutex mx;
    bool tried = false;
    bool ok = false;
    std::string err;
    std::string dir;  // set_ffmpeg_dir
    Api api;
};

State& state() {
    static State s;
    return s;
}

bool load(State& s) {
    const auto names = lib_names();
    std::vector<std::filesystem::path> dirs;
    const auto ed = exe_dir();
    if (!ed.empty()) dirs.push_back(ed);
    const std::string fallback = s.dir.empty() ? std::string(ALAMO_VIDEO_FFMPEG_DEFAULT_DIR) : s.dir;
    if (!fallback.empty()) dirs.push_back(alamo::path_from_utf8(fallback));

    Handle handles[3] = {nullptr, nullptr, nullptr};
    std::filesystem::path chosen;
    for (const auto& d : dirs) {
        std::error_code ec;
        if (!std::filesystem::exists(d / names[0].file, ec)) continue;
        bool all = true;
        for (size_t i = 0; i < names.size(); ++i) {
            handles[i] = open_lib(d / names[i].file);
            if (!handles[i]) {
                all = false;
                s.err = "cannot load " + alamo::path_to_utf8(d / names[i].file);
                break;
            }
        }
        if (all) {
            chosen = d;
            break;
        }
    }
    if (chosen.empty()) {
        if (s.err.empty()) {
            s.err = "FFmpeg libraries (" + names[0].file + ", " + names[1].file + ", " + names[2].file +
                    ") not found next to the executable";
            if (!fallback.empty()) s.err += " or in " + fallback;
        }
        return false;
    }
    auto handle_of = [&](const char* key) -> Handle {
        for (size_t i = 0; i < names.size(); ++i)
            if (std::strcmp(names[i].key, key) == 0) return handles[i];
        return nullptr;
    };
#define ALAMO_FF_LOAD(lib, name)                                                  \
    {                                                                             \
        void* sym = find_sym(handle_of(#lib), #name);                             \
        if (!sym) {                                                               \
            s.err = std::string("missing FFmpeg export ") + #name + " in " #lib; \
            return false;                                                         \
        }                                                                         \
        assign(s.api.name, sym);                                                  \
    }
    ALAMO_FF_FUNCS(ALAMO_FF_LOAD)
#undef ALAMO_FF_LOAD
    // The DLLs must be the major versions the headers describe (struct layouts).
    if (AV_VERSION_MAJOR(s.api.avutil_version()) != LIBAVUTIL_VERSION_MAJOR ||
        AV_VERSION_MAJOR(s.api.avcodec_version()) != LIBAVCODEC_VERSION_MAJOR ||
        AV_VERSION_MAJOR(s.api.avformat_version()) != LIBAVFORMAT_VERSION_MAJOR) {
        s.err = "FFmpeg library major versions do not match the headers";
        return false;
    }
    s.api.av_log_set_level(AV_LOG_ERROR);
    // The libraries stay loaded for the life of the process (never unloaded).
    return true;
}

}  // namespace

const Api* api(std::string* err) {
    State& s = state();
    std::lock_guard<std::mutex> lk(s.mx);
    if (!s.tried) {
        s.tried = true;
        s.ok = load(s);
    }
    if (!s.ok) {
        if (err) *err = s.err;
        return nullptr;
    }
    return &s.api;
}

std::string ff_error(int code) {
    const Api* a = api();
    char buf[256] = {};
    if (a && a->av_strerror(code, buf, sizeof(buf)) == 0) return buf;
    return "FFmpeg error " + std::to_string(code);
}

}  // namespace detail

void set_ffmpeg_dir(std::string dir_utf8) {
    auto& s = detail::state();
    std::lock_guard<std::mutex> lk(s.mx);
    if (!s.tried) s.dir = std::move(dir_utf8);
}

bool ffmpeg_available(std::string* err) { return detail::api(err) != nullptr; }

namespace {
std::string ver3(unsigned v) {
    return std::to_string(AV_VERSION_MAJOR(v)) + "." + std::to_string(AV_VERSION_MINOR(v)) + "." +
           std::to_string(AV_VERSION_MICRO(v));
}
}  // namespace

std::string ffmpeg_version_string() {
    const detail::Api* a = detail::api();
    if (!a) return {};
    return std::string("FFmpeg ") + a->av_version_info() + ", avcodec " + ver3(a->avcodec_version()) + ", avformat " +
           ver3(a->avformat_version()) + ", avutil " + ver3(a->avutil_version()) + ", " + a->avcodec_license();
}

std::string ffmpeg_license() {
    const detail::Api* a = detail::api();
    return a ? std::string(a->avcodec_license()) : std::string();
}

}  // namespace alamo::video

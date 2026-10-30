// Runtime-loaded FFmpeg entry points (avutil / avcodec / avformat). Internal to engine/video.
#pragma once

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/avutil.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
}
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#endif

#include <string>

namespace alamo::video::detail {

#define ALAMO_FF_FUNCS(X)              \
    X(avutil, av_frame_alloc)          \
    X(avutil, av_frame_free)           \
    X(avutil, av_malloc)               \
    X(avutil, av_free)                 \
    X(avutil, av_log_set_level)        \
    X(avutil, av_strerror)             \
    X(avutil, av_version_info)         \
    X(avutil, avutil_version)          \
    X(avcodec, avcodec_find_decoder)   \
    X(avcodec, avcodec_alloc_context3) \
    X(avcodec, avcodec_free_context)   \
    X(avcodec, avcodec_parameters_to_context) \
    X(avcodec, avcodec_open2)          \
    X(avcodec, avcodec_send_packet)    \
    X(avcodec, avcodec_receive_frame)  \
    X(avcodec, av_packet_alloc)        \
    X(avcodec, av_packet_free)         \
    X(avcodec, av_packet_unref)        \
    X(avcodec, avcodec_version)        \
    X(avcodec, avcodec_license)        \
    X(avformat, avformat_alloc_context) \
    X(avformat, avformat_open_input)   \
    X(avformat, avformat_close_input)  \
    X(avformat, av_read_frame)         \
    X(avformat, av_find_input_format)  \
    X(avformat, avio_alloc_context)    \
    X(avformat, avio_context_free)     \
    X(avformat, avformat_version)

struct Api {
#define ALAMO_FF_DECL(lib, name) decltype(&::name) name = nullptr;
    ALAMO_FF_FUNCS(ALAMO_FF_DECL)
#undef ALAMO_FF_DECL
};

// The loaded API, or nullptr (+ *err) when the libraries are unavailable. Loads once; thread-safe.
const Api* api(std::string* err = nullptr);

// av_strerror text for an FFmpeg error code.
std::string ff_error(int code);

}  // namespace alamo::video::detail

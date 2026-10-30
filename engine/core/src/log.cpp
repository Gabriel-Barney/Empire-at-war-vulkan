#include "alamo/core/log.hpp"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace alamo::log {
namespace {
std::mutex g_mu;
Sink g_sink;
std::atomic<int> g_min{static_cast<int>(Level::Info)};

const char* level_name(Level l) {
    switch (l) {
        case Level::Trace: return "trace";
        case Level::Debug: return "debug";
        case Level::Info: return "info";
        case Level::Warn: return "warn";
        case Level::Error: return "error";
    }
    return "?";
}

void vwrite(Level level, const char* channel, const char* fmt, va_list ap) {
    if (static_cast<int>(level) < g_min.load(std::memory_order_relaxed)) return;
    char buf[2048];
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    write(level, channel, buf);
}
}  // namespace

void set_sink(Sink sink) {
    std::lock_guard lk(g_mu);
    g_sink = std::move(sink);
}
void set_min_level(Level level) { g_min.store(static_cast<int>(level)); }
Level min_level() { return static_cast<Level>(g_min.load()); }

void write(Level level, std::string_view channel, std::string_view message) {
    if (static_cast<int>(level) < g_min.load(std::memory_order_relaxed)) return;
    std::lock_guard lk(g_mu);
    if (g_sink) {
        g_sink(level, channel, message);
        return;
    }
    std::fprintf(stderr, "[%s] %.*s: %.*s\n", level_name(level), static_cast<int>(channel.size()),
                 channel.data(), static_cast<int>(message.size()), message.data());
}

#define ALAMO_LOG_FN(fn, lvl)                         \
    void fn(const char* channel, const char* fmt, ...) { \
        va_list ap;                                   \
        va_start(ap, fmt);                            \
        vwrite(lvl, channel, fmt, ap);                \
        va_end(ap);                                   \
    }
ALAMO_LOG_FN(tracef, Level::Trace)
ALAMO_LOG_FN(debugf, Level::Debug)
ALAMO_LOG_FN(infof, Level::Info)
ALAMO_LOG_FN(warnf, Level::Warn)
ALAMO_LOG_FN(errorf, Level::Error)
#undef ALAMO_LOG_FN

}  // namespace alamo::log

// Tiny thread-safe logger. Default sink writes to stderr; tools/tests may replace it.
#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace alamo::log {

enum class Level { Trace, Debug, Info, Warn, Error };

using Sink = std::function<void(Level, std::string_view channel, std::string_view message)>;
void set_sink(Sink sink);          // nullptr restores the stderr sink
void set_min_level(Level level);   // default Info
Level min_level();
void write(Level level, std::string_view channel, std::string_view message);

// printf-style helpers.
void tracef(const char* channel, const char* fmt, ...);
void debugf(const char* channel, const char* fmt, ...);
void infof(const char* channel, const char* fmt, ...);
void warnf(const char* channel, const char* fmt, ...);
void errorf(const char* channel, const char* fmt, ...);

}  // namespace alamo::log

// Includes the vendored miniaudio header with its warnings silenced (third-party code; rule 1 covers
// our code only). The include dir is also SYSTEM in CMake. MA_* configuration macros come from the
// alamo_miniaudio target so every includer sees the same struct layouts.
#pragma once
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#elif defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wshadow"
#endif
#include "miniaudio.h"
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#elif defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

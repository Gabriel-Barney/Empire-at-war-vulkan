// Shared support for the engine test exes (CU4 t01): the CHECK harness, file and environment helpers, the
// installed-game data roots and the ctest skip code. Header-only (INTERFACE target alamo_test_support); every exe
// that includes it gets one g_fail / g_checks however many of its TUs include it (inline variables).
//
// Migrating a test file: drop its own `static int g_fail = 0;` and `#define CHECK`, include this header and put
// `using ::alamo::test::g_fail;` where the old counter was declared, so `g_fail` keeps working unqualified. main()
// keeps its own summary line and exit code (`return g_fail ? 1 : 0;`); report() is there for new tests.
//
// Test-only code: nothing here may be used by engine libraries or read by sim code.
#pragma once
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <source_location>
#include <string>
#include <string_view>

namespace alamo::test {

// Failed / evaluated CHECK()s of this exe.
inline int g_fail = 0;
inline int g_checks = 0;

// Exit code ctest reads as "skipped" for tests registered with SKIP_RETURN_CODE 77 (the automake convention).
inline constexpr int kSkipReturnCode = 77;

// The last path component of a source file name ("a/b\c.cpp" -> "c.cpp"). FAIL lines print this instead of __FILE__ so the
// compiler cache sees the same preprocessed text for a TU in any worktree (a __FILE__ in the macro text differs per
// checkout path and halved the hit rate); the line number plus the file name is enough to find the check.
inline const char* file_tail(const char* path) {
    const char* tail = path;
    for (const char* p = path; *p; ++p)
        if (*p == '/' || *p == '\\') tail = p + 1;
    return tail;
}

// Prints "== <name>" so a failure line can be placed in a long log.
inline void section(const char* name) { std::printf("== %s\n", name); }

// "<exe>: all N checks passed" or "<exe>: F of N check(s) failed"; returns the exit code (0 / 1).
inline int report(const char* exe) {
    if (g_fail) {
        std::printf("%s: %d of %d check(s) failed\n", exe, g_fail, g_checks);
        return 1;
    }
    std::printf("%s: all %d checks passed\n", exe, g_checks);
    return 0;
}

// The environment variable's value; empty when unset. (_dupenv_s on MSVC, which warns on std::getenv.)
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

// The variable's value, or `fallback` when it is unset or empty.
inline std::string env_or(const char* name, const char* fallback) {
    std::string v = env_or_empty(name);
    return v.empty() ? std::string(fallback) : v;
}

// Installed-game data roots (the FoC "corruption" folder / the EaW "GameData" folder); empty when not set. Data-driven
// tests print a SKIP line and pass (or return kSkipReturnCode) when these are empty; the build scripts set them
// (tools\test_env.cmd).
inline std::string foc_data_root() { return env_or_empty("ALAMO_FOC_DATA"); }
inline std::string eaw_data_root() { return env_or_empty("ALAMO_EAW_DATA"); }

// "SKIP <what>: <var> not set" for a data-driven part that did not run.
inline void print_skip(const char* what, const char* var = "ALAMO_FOC_DATA") {
    std::printf("SKIP %s: %s not set\n", what, var);
}

// Writes `bytes` to `p` (binary), creating the parent folders. Returns false when the file could not be written.
inline bool write_file(const std::filesystem::path& p, std::string_view bytes) {
    std::error_code ec;
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary);
    f.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(f);
}

// The whole file (binary); empty when it cannot be read.
inline std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace alamo::test

// ALAMO_TEST_FILE() is the path-independent file name for a test file's own check macros (use it instead of __FILE__).
#define ALAMO_TEST_FILE() (::alamo::test::file_tail(std::source_location::current().file_name()))

// One failed condition prints "FAIL <file>:<line>: <expr>" and counts in alamo::test::g_fail; every evaluation counts
// in g_checks. Define ALAMO_TEST_NO_CHECK_MACRO before including to get only ALAMO_CHECK (a file with its own CHECK).
#define ALAMO_CHECK(c)                                                       \
    do {                                                                     \
        ++::alamo::test::g_checks;                                           \
        if (!(c)) {                                                          \
            std::printf("FAIL %s:%d: %s\n", ALAMO_TEST_FILE(), __LINE__, #c); \
            ++::alamo::test::g_fail;                                         \
        }                                                                    \
    } while (0)
#ifndef ALAMO_TEST_NO_CHECK_MACRO
#define CHECK(c) ALAMO_CHECK(c)
#endif

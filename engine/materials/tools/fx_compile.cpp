// fxc.exe runner (fx_compile.hpp, roadmap modc t23).
#include "fx_compile.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include "alamo/core/file_provider.hpp"

#include <windows.h>

namespace alamo::materials {

namespace {

std::string getenv_str(const char* n) {
    char buf[4096];
    const DWORD r = GetEnvironmentVariableA(n, buf, sizeof(buf));
    return (r > 0 && r < sizeof(buf)) ? std::string(buf, r) : std::string();
}

std::wstring quoted(const std::filesystem::path& p) {
    return L"\"" + p.wstring() + L"\"";
}

std::string upper_ascii(std::string s) {
    for (char& c : s)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return s;
}

// Runs `cmd` with stdout + stderr going to `log`; the exit code, or -1 when it cannot start / times out.
int run_process(const std::wstring& cmd, const std::filesystem::path& cwd, const std::filesystem::path& log) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof sa;
    sa.bInheritHandle = TRUE;
    HANDLE out = CreateFileW(log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return -1;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = out;
    si.hStdError = out;
    PROCESS_INFORMATION pi{};
    std::wstring line = cmd;  // CreateProcessW may modify the buffer
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi);
    CloseHandle(out);
    if (!ok) return -1;
    int code = -1;
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) {
        DWORD c = 0;
        if (GetExitCodeProcess(pi.hProcess, &c)) code = static_cast<int>(c);
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

// Upper case, '/' -> '\\' (the same length as the input).
std::string fold_path(std::string s) {
    for (char& c : s)
        if (c == '/') c = '\\';
    return upper_ascii(std::move(s));
}

// Removes every occurrence of folder `prefix` (case and separator insensitive) from `s` (temporary folder
// names in fxc's log).
std::string strip_prefix(std::string s, const std::string& prefix) {
    if (prefix.empty()) return s;
    const std::string up = fold_path(prefix);
    for (;;) {
        const size_t at = fold_path(s).find(up);
        if (at == std::string::npos) return s;
        s.erase(at, prefix.size());
    }
}

}  // namespace

std::filesystem::path default_fxc_path() {
    const std::string over = getenv_str("ALAMO_FXC");
    if (!over.empty()) return alamo::path_from_utf8(over);
#ifdef ALAMO_FXC_PATH
    std::error_code ec;
    const std::filesystem::path p = alamo::path_from_utf8(ALAMO_FXC_PATH);
    if (std::filesystem::is_regular_file(p, ec)) return p;
#endif
    return {};
}

std::string fxc_identity(const std::filesystem::path& fxc) {
    std::error_code ec;
    if (fxc.empty() || !std::filesystem::is_regular_file(fxc, ec)) return {};
    const auto size = std::filesystem::file_size(fxc, ec);
    const auto dll = std::filesystem::file_size(fxc.parent_path() / "d3dcompiler_47.dll", ec);
    const std::string ver = alamo::path_to_utf8(fxc.parent_path().parent_path().filename());  // bin\<ver>\x64
    return "fxc " + ver + " " + std::to_string(size) + "/" + (ec ? std::string("-") : std::to_string(dll));
}

std::string fx_log_summary(const std::string& log) {
    std::string first;
    size_t i = 0;
    while (i < log.size()) {
        const size_t e = std::min(log.find('\n', i), log.size());
        std::string line = log.substr(i, e - i);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        i = e + 1;
        if (line.empty()) continue;
        if (line.find("error") != std::string::npos) return line;
        if (first.empty()) first = line;
    }
    return first;
}

bool compile_fx_source(const FxSource& src, const std::filesystem::path& fxc, FxCacheEntry& out, std::string* err) {
    out = FxCacheEntry{};
    std::error_code ec;
    if (fxc.empty() || !std::filesystem::is_regular_file(fxc, ec)) {
        if (err) *err = "fxc.exe not found (set ALAMO_FXC or install the toolchain's Windows SDK)";
        return false;
    }
    if (src.files.empty()) {
        if (err) *err = "empty source";
        return false;
    }
    static std::atomic<uint64_t> counter{0};
    char name[96];
    std::snprintf(name, sizeof name, "alamo_fxc_%lu_%llu", static_cast<unsigned long>(GetCurrentProcessId()),
                  static_cast<unsigned long long>(counter.fetch_add(1)));
    const std::filesystem::path tmp = std::filesystem::temp_directory_path(ec) / name;
    std::filesystem::remove_all(tmp, ec);
    if (!std::filesystem::create_directories(tmp, ec)) {
        if (err) *err = "cannot create " + alamo::path_to_utf8(tmp);
        return false;
    }
    auto host = [&](const std::string& game) {
        std::string rel = game;
        std::replace(rel.begin(), rel.end(), '/', '\\');
        return tmp / alamo::path_from_utf8(rel);
    };
    bool wrote = true;
    for (const FxSourceFile& f : src.files) {
        const std::filesystem::path p = host(f.path);
        std::filesystem::create_directories(p.parent_path(), ec);
        std::ofstream o(p, std::ios::binary | std::ios::trunc);
        o.write(reinterpret_cast<const char*>(f.bytes.data()), static_cast<std::streamsize>(f.bytes.size()));
        wrote = wrote && static_cast<bool>(o);
    }
    std::filesystem::create_directories(tmp / "Data" / "Art" / "Shaders", ec);
    const std::filesystem::path fxo = tmp / "out.fxo", log = tmp / "fxc.log", main = host(src.files[0].path);
    const std::wstring cmd = quoted(fxc) + L" /nologo /T " + std::filesystem::path(kFxProfile).wstring() + L" /I " +
                             quoted(tmp / "Data" / "Art" / "Shaders") + L" /Fo " + quoted(fxo) + L" " + quoted(main);
    const int code = wrote ? run_process(cmd, main.parent_path(), log) : -1;
    std::vector<uint8_t> text;
    alamo::read_host_file(log, text);
    out.log = strip_prefix(std::string(text.begin(), text.end()), alamo::path_to_utf8(tmp) + "/");
    out.compiler = fxc_identity(fxc);
    const bool ran = wrote && code >= 0;
    if (ran && code == 0 && alamo::read_host_file(fxo, out.fxo) && !out.fxo.empty()) out.ok = true;
    else out.fxo.clear();
    std::filesystem::remove_all(tmp, ec);
    if (!ran) {
        if (err) *err = wrote ? "cannot run " + alamo::path_to_utf8(fxc) : "cannot write the sources to " + alamo::path_to_utf8(tmp);
        return false;
    }
    return true;
}

std::vector<std::string> fx_source_paths(const EffectLibrary& lib, std::span<const ShaderFileEntry> shader_files) {
    std::set<std::string> names;  // upper-case base names that have some .fx
    for (const ShaderFileEntry& f : shader_files) {
        const std::string up = upper_ascii(f.path);
        if (up.size() < 4 || up.compare(up.size() - 3, 3, ".FX") != 0) continue;
        const size_t slash = up.find_last_of("\\/");
        names.insert(up.substr(slash == std::string::npos ? 0 : slash + 1));
    }
    std::map<std::string, std::string> paths;  // normalised path -> path
    for (const std::string& n : names)
        if (auto p = lib.find_effect_source(n)) paths.emplace(alamo::normalize_path(*p), *p);
    std::vector<std::string> out;
    for (auto& kv : paths) out.push_back(kv.second);
    return out;
}

bool prebuild_fx_sources(const FileProvider& files, std::span<const std::string> fx_paths, const FxCache& cache,
                         const std::filesystem::path& fxc, bool force, std::vector<FxPrebuildItem>& items, std::string* err) {
    items.clear();
    bool all = true;
    for (const std::string& path : fx_paths) {
        const auto t0 = std::chrono::steady_clock::now();
        FxPrebuildItem it;
        it.path = path;
        FxSource src;
        std::string e;
        if (!collect_fx_source(files, path, src, &e)) {
            it.detail = e;
        } else {
            it.key = src.key();
            FxCacheEntry entry;
            if (!force && cache.load(it.key, entry)) {
                it.cached = true;
                it.ok = entry.ok;
                if (!entry.ok) it.detail = fx_log_summary(entry.log);
            } else if (!compile_fx_source(src, fxc, entry, &e)) {
                it.detail = e;
                if (err && err->empty()) *err = e;
                all = false;
            } else {
                it.ok = entry.ok;
                if (!entry.ok) it.detail = fx_log_summary(entry.log);
                if (!cache.store(it.key, entry)) {
                    if (err && err->empty()) *err = "cannot write the cache in " + alamo::path_to_utf8(cache.directory());
                    all = false;
                }
            }
        }
        it.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        items.push_back(std::move(it));
    }
    return all;
}

}  // namespace alamo::materials

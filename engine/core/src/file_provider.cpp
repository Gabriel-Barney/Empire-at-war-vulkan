#include "alamo/core/file_provider.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>

#include "alamo/core/hash.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace fs = std::filesystem;

namespace alamo {

std::string normalize_path(std::string_view path) {
    std::string s = normalize_name(path);
    // collapse duplicate separators
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\\' && !out.empty() && out.back() == '\\') continue;
        out.push_back(c);
    }
    while (out.rfind(".\\", 0) == 0) out.erase(0, 2);
    while (!out.empty() && out.front() == '\\') out.erase(0, 1);
    return out;
}

std::string with_default_dir_ext(std::string_view name, std::string_view dir, std::string_view ext) {
    std::string s(name);
    const bool has_dir = s.find('\\') != std::string::npos || s.find('/') != std::string::npos;
    const auto slash = s.find_last_of("\\/");
    const auto dot = s.rfind('.');
    const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    if (!has_ext) s += ext;
    if (!has_dir) s = std::string(dir) + s;
    return normalize_path(s);
}

fs::path path_from_utf8(std::string_view utf8) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::string path_to_utf8(const fs::path& p) {
    const std::u8string s = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

namespace {
bool is_sep(wchar_t c) { return c == L'\\' || c == L'/'; }
}  // namespace

fs::path long_path(const fs::path& p) {
    const std::wstring& in = p.native();
    if (in.empty()) return p;
    // Already "\\?\..." -- possibly in generic form after a UTF-8 round trip: "//?/C:/x" (MSVC) or
    // "/?/C:/x" (libstdc++ collapses the leading separators). '/' and '?' cannot occur in Win32 file
    // names, so rewriting the separators to '\' and restoring the "\\?\" prefix is lossless.
    size_t prefixed = 0;
    if (in.size() >= 4 && is_sep(in[0]) && is_sep(in[1]) && in[2] == L'?' && is_sep(in[3]))
        prefixed = 4;
    else if (in.size() >= 3 && is_sep(in[0]) && in[1] == L'?' && is_sep(in[2]))
        prefixed = 3;
    if (prefixed) {
        std::wstring out = L"\\\\?\\" + in.substr(prefixed);
        for (wchar_t& c : out)
            if (c == L'/') c = L'\\';
        return fs::path(out);
    }
    if (in.size() >= 4 && is_sep(in[0]) && is_sep(in[1]) && in[2] == L'.' && is_sep(in[3]))
        return p;  // device path "\\.\..."
    std::wstring full(in.size() + 512, L'\0');
    for (int attempt = 0;; ++attempt) {  // bounded: the size can only change if the cwd changes meanwhile
        const DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
        if (n == 0 || attempt == 4) return p;  // cannot resolve: leave it to the caller's call to fail
        if (n < full.size()) {
            full.resize(n);
            break;
        }
        full.resize(static_cast<size_t>(n) + 1);
    }
    if (full.size() >= 2 && full[0] == L'\\' && full[1] == L'\\') return fs::path(L"\\\\?\\UNC\\" + full.substr(2));
    return fs::path(L"\\\\?\\" + full);
}

namespace {
std::error_code win_error(DWORD e) { return std::error_code(static_cast<int>(e), std::system_category()); }
bool is_dot(const wchar_t* n) { return n[0] == L'.' && (n[1] == 0 || (n[1] == L'.' && n[2] == 0)); }

// Name surrogates (IO_REPARSE_TAG_SYMLINK, IO_REPARSE_TAG_MOUNT_POINT = junctions, ...) alias another
// file system entity: they are removed as links, never followed. Other reparse points (cloud-file
// placeholders, dedup, WCI, ...) hold their own real children and are recursed into. An unknown tag
// (0: it could not be read) is treated as a link -- the safe choice.
bool is_link(DWORD attrs, DWORD tag) {
    return (attrs & FILE_ATTRIBUTE_REPARSE_POINT) && (tag == 0 || (tag & 0x20000000u) != 0);
}

// Reparse tag of `lp` itself (not of its target); 0 when it cannot be read.
DWORD reparse_tag_of(const fs::path& lp) {
    HANDLE h = CreateFileW(lp.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FILE_ATTRIBUTE_TAG_INFO info{};
    const BOOL ok = GetFileInformationByHandleEx(h, FileAttributeTagInfo, &info, sizeof info);
    CloseHandle(h);
    return ok ? info.ReparseTag : 0;
}

// Calls f(name, attributes, reparse tag) for each entry of `lp` (already in long form). False + ec on
// error. Terminates: FindNextFileW walks a finite snapshot and nothing here is retried.
template <class F>
bool for_each_entry(const fs::path& lp, std::error_code& ec, F&& f) {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = lp.native() + (lp.native().back() == L'\\' ? L"*" : L"\\*");
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) {
        ec = win_error(GetLastError());
        return false;
    }
    bool ok = true;
    do {
        const DWORD tag = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ? fd.dwReserved0 : 0;
        if (!is_dot(fd.cFileName) && !f(std::wstring(fd.cFileName), fd.dwFileAttributes, tag)) {
            ok = false;
            break;
        }
    } while (FindNextFileW(h, &fd));
    if (ok) {
        const DWORD e = GetLastError();
        if (e != ERROR_NO_MORE_FILES) {
            ec = win_error(e);
            ok = false;
        }
    }
    FindClose(h);
    return ok;
}

// Runs `op` (a Win32 call returning BOOL) and retries a bounded number of times, with exponential
// back-off, on errors another process can cause transiently: a handle open without FILE_SHARE_DELETE
// (sharing violation), a delete still pending on a child (directory not empty), an antivirus scan
// (access denied). Worst case ~0.1 s per entry, then the error is returned -- never a hang.
template <class Op>
bool with_retries(Op&& op, std::error_code& ec) {
    constexpr int kRetries = 6;  // sleeps 1, 2, 4, 8, 16, 32 ms
    for (int attempt = 0;; ++attempt) {
        if (op()) return true;
        const DWORD e = GetLastError();
        const bool transient = e == ERROR_SHARING_VIOLATION || e == ERROR_LOCK_VIOLATION || e == ERROR_ACCESS_DENIED ||
                               e == ERROR_DIR_NOT_EMPTY;
        if (!transient || attempt == kRetries) {
            ec = win_error(e);
            return false;
        }
        Sleep(1u << attempt);
    }
}

// `lp` is in long form; `attrs` / `tag` its attributes and reparse tag. Recursion only descends into
// real directories (links are not followed), so it terminates and never leaves the tree.
bool remove_entry(const fs::path& lp, DWORD attrs, DWORD tag, std::uintmax_t& count, std::error_code& ec) {
    const bool dir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (dir && !is_link(attrs, tag)) {
        const bool listed = for_each_entry(lp, ec, [&](const std::wstring& name, DWORD a, DWORD t) {
            return remove_entry(lp / name, a, t, count, ec);
        });
        if (!listed) return false;
    }
    if (attrs & FILE_ATTRIBUTE_READONLY) {  // DeleteFileW / RemoveDirectoryW refuse read-only entries
        const DWORD keep = attrs & (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                                    FILE_ATTRIBUTE_NOT_CONTENT_INDEXED | FILE_ATTRIBUTE_TEMPORARY);
        SetFileAttributesW(lp.c_str(), keep ? keep : FILE_ATTRIBUTE_NORMAL);
    }
    const wchar_t* w = lp.c_str();
    if (!with_retries([&] { return dir ? RemoveDirectoryW(w) : DeleteFileW(w); }, ec)) return false;
    ++count;
    return true;
}

// Inputs remove_all_long refuses because Win32 path resolution would widen them: "C:" / "C:x" (relative
// to the drive's current directory), "x." / "x " (trailing dots and spaces are stripped, so "cache."
// would delete "cache"), a final "." / "..", device paths "\\.\...".
bool ambiguous_for_delete(const fs::path& p) {
    const std::wstring& s = p.native();
    if (s.size() >= 4 && is_sep(s[0]) && is_sep(s[1]) && (s[2] == L'?' || s[2] == L'.') && is_sep(s[3]))
        return s[2] == L'.';  // "\\?\" paths are taken literally by Win32: no stripping
    if (s.size() >= 3 && is_sep(s[0]) && s[1] == L'?' && is_sep(s[2])) return false;  // generic "\\?\"
    const std::wstring root = p.root_name().native();
    if (root.size() == 2 && root[1] == L':' && !p.has_root_directory()) return true;  // drive-relative
    std::wstring last;
    for (const fs::path& c : p.relative_path()) {
        last = c.native();
        if (last.empty() || last == L"." || last == L"..") continue;
        if (last.back() == L'.' || last.back() == L' ') return true;
    }
    return last == L"." || last == L"..";
}

// True for "\\?\C:\", "\\?\C:", "\\?\Volume{...}\", "\\?\UNC\server\share[\]".
bool is_volume_root(const fs::path& lp) {
    const std::wstring& s = lp.native();
    const bool unc = s.rfind(L"\\\\?\\UNC\\", 0) == 0;
    std::wstring rest = s.substr(unc ? 8 : 4);
    while (!rest.empty() && rest.back() == L'\\') rest.pop_back();
    size_t parts = 0;
    for (size_t i = 0; i < rest.size();) {
        const size_t j = std::min(rest.find(L'\\', i), rest.size());
        if (j > i) ++parts;
        i = j + 1;
    }
    return parts <= (unc ? 2u : 1u);
}
}  // namespace

std::vector<fs::path> list_directory(const fs::path& dir, std::error_code& ec) {
    ec.clear();
    std::vector<fs::path> out;
    if (dir.empty()) {
        ec = std::make_error_code(std::errc::no_such_file_or_directory);
        return out;
    }
    if (!for_each_entry(long_path(dir), ec, [&](const std::wstring& name, DWORD, DWORD) {
            out.push_back(dir / name);
            return true;
        }))
        out.clear();
    return out;
}

std::uintmax_t remove_all_long(const fs::path& p, std::error_code& ec) {
    ec.clear();
    if (p.empty()) return 0;
    const fs::path lp = long_path(p);
    if (ambiguous_for_delete(p) || lp.native().rfind(L"\\\\?\\", 0) != 0 || is_volume_root(lp)) {
        ec = std::make_error_code(std::errc::invalid_argument);
        return static_cast<std::uintmax_t>(-1);
    }
    const DWORD attrs = GetFileAttributesW(lp.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return 0;
        ec = win_error(e);
        return static_cast<std::uintmax_t>(-1);
    }
    const DWORD tag = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) ? reparse_tag_of(lp) : 0;
    std::uintmax_t count = 0;
    if (!remove_entry(lp, attrs, tag, count, ec)) return static_cast<std::uintmax_t>(-1);
    return count;
}

bool read_host_file(const fs::path& path, std::vector<uint8_t>& out) {
    out.clear();
    if (path.empty()) return false;
    // Share read/write/delete: a reader never blocks a concurrent atomic replace (rename over the file).
    HANDLE h = CreateFileW(long_path(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(h, &size) && size.QuadPart >= 0 &&
              static_cast<unsigned long long>(size.QuadPart) <= static_cast<unsigned long long>(SIZE_MAX);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        size_t done = 0;
        while (ok && done < out.size()) {
            const DWORD want = static_cast<DWORD>(std::min<size_t>(out.size() - done, size_t{1} << 30));
            DWORD got = 0;
            ok = ReadFile(h, out.data() + done, want, &got, nullptr) && got == want;
            done += got;
        }
    }
    CloseHandle(h);
    if (!ok) out.clear();
    return ok;
}

bool read_host_file(const std::string& path, std::vector<uint8_t>& out) {
    return read_host_file(path_from_utf8(path), out);
}

bool read_host_file(const char* path, std::vector<uint8_t>& out) {
    return read_host_file(path_from_utf8(path ? std::string_view(path) : std::string_view()), out);
}

DirectoryProvider::DirectoryProvider(std::string root) : root_(std::move(root)) {
    std::error_code ec;
    const fs::path base = path_from_utf8(root_);
    if (!fs::is_directory(base, ec)) return;
    for (auto it = fs::recursive_directory_iterator(base, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const auto rel = fs::relative(it->path(), base, ec);
        if (ec) continue;
        index_.emplace(normalize_path(path_to_utf8(rel)), path_to_utf8(it->path()));
    }
}

bool DirectoryProvider::read(std::string_view path, std::vector<uint8_t>& out) const {
    const auto it = index_.find(normalize_path(path));
    if (it == index_.end()) {
        out.clear();
        return false;
    }
    return read_host_file(it->second, out);
}

bool DirectoryProvider::exists(std::string_view path) const { return index_.count(normalize_path(path)) != 0; }

std::string DirectoryProvider::origin(std::string_view path) const {
    const auto it = index_.find(normalize_path(path));
    return it == index_.end() ? std::string() : it->second;
}

}  // namespace alamo

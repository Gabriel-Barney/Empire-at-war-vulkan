// alamo_vfs STEAMMOD= resolver without the Steam API. See include/alamo/vfs/steam_workshop.hpp.
#include "alamo/vfs/steam_workshop.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <utility>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"

#include <windows.h>

namespace fs = std::filesystem;

namespace alamo::vfs {

namespace {

bool is_dir(const std::string& host) {
    std::error_code ec;
    return !host.empty() && fs::is_directory(long_path(path_from_utf8(host)), ec);
}

std::string read_text(const std::string& host) {
    std::ifstream f(long_path(path_from_utf8(host)), std::ios::binary);
    if (!f) return {};
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::string strip_sep(std::string s) {
    while (s.size() > 1 && (s.back() == '\\' || s.back() == '/') && !(s.size() == 3 && s[1] == ':')) s.pop_back();
    return s;
}

std::string join(std::string_view a, std::string_view b) {
    std::string out = strip_sep(std::string(a));
    if (!out.empty() && out.back() != '\\' && out.back() != '/') out.push_back('\\');
    out += b;
    return out;
}

// Comparison key: upper case ASCII, '/' -> '\\', no trailing separator.
std::string key_of(std::string_view p) {
    std::string k = strip_sep(std::string(p));
    for (char& c : k) {
        if (c == '/') c = '\\';
        else if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return k;
}

void add_unique(std::vector<std::string>& out, std::vector<std::string>& keys, std::string p) {
    p = strip_sep(std::move(p));
    if (p.empty()) return;
    std::string k = key_of(p);
    for (const std::string& e : keys)
        if (e == k) return;
    keys.push_back(std::move(k));
    out.push_back(std::move(p));
}

bool all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (c < '0' || c > '9') return false;
    return true;
}

// One KeyValues token: a quoted string (escapes decoded), an unquoted word, '{' or '}'.
struct Token {
    enum Kind { End, String, Open, Close } kind = End;
    std::string text;
};

class VdfLexer {
public:
    explicit VdfLexer(std::string_view s) : s_(s) {}
    Token next() {
        skip();
        Token t;
        if (i_ >= s_.size()) return t;
        const char c = s_[i_];
        if (c == '{' || c == '}') {
            ++i_;
            t.kind = c == '{' ? Token::Open : Token::Close;
            return t;
        }
        t.kind = Token::String;
        if (c == '"') {
            ++i_;
            while (i_ < s_.size() && s_[i_] != '"') {
                char ch = s_[i_++];
                if (ch == '\\' && i_ < s_.size()) {
                    const char e = s_[i_++];
                    ch = e == 'n' ? '\n' : e == 't' ? '\t' : e;  // \\ \" and anything else: the character itself
                }
                t.text.push_back(ch);
            }
            if (i_ < s_.size()) ++i_;  // closing quote
            return t;
        }
        while (i_ < s_.size() && !is_space(s_[i_]) && s_[i_] != '{' && s_[i_] != '}' && s_[i_] != '"') t.text.push_back(s_[i_++]);
        return t;
    }

private:
    static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
    void skip() {
        for (;;) {
            while (i_ < s_.size() && is_space(s_[i_])) ++i_;
            if (i_ + 1 < s_.size() && s_[i_] == '/' && s_[i_ + 1] == '/') {  // // comment to end of line
                while (i_ < s_.size() && s_[i_] != '\n') ++i_;
                continue;
            }
            return;
        }
    }
    std::string_view s_;
    size_t i_ = 0;
};

void add_vdf_libraries(const std::string& vdf_path, std::vector<std::string>& out, std::vector<std::string>& keys) {
    const std::string text = read_text(vdf_path);
    if (text.empty()) return;
    for (std::string& p : parse_steam_library_folders(text)) add_unique(out, keys, std::move(p));
}

std::string to_utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// A REG_SZ value, read only (RegGetValueW). "" when absent.
std::string registry_string(HKEY root, const wchar_t* subkey, const wchar_t* value) {
    DWORD bytes = 0;
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS || bytes == 0) return {};
    std::wstring w(bytes / sizeof(wchar_t) + 1, L'\0');
    if (RegGetValueW(root, subkey, value, RRF_RT_REG_SZ, nullptr, w.data(), &bytes) != ERROR_SUCCESS) return {};
    w.resize(wcsnlen(w.c_str(), w.size()));
    return to_utf8(w);
}

std::string env_utf8(const wchar_t* name) {
    const DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return {};
    std::wstring w(n, L'\0');
    const DWORD got = GetEnvironmentVariableW(name, w.data(), n);
    w.resize(got < n ? got : 0);
    return to_utf8(w);
}

struct LookupState {
    std::mutex mu;
    bool installed = false;
    bool roots_known = false;
    std::vector<std::string> roots;
};
LookupState& lookup_state() {
    static LookupState s;
    return s;
}

}  // namespace

std::vector<std::string> parse_steam_library_folders(std::string_view vdf) {
    std::vector<std::string> out;
    VdfLexer lex(vdf);
    int depth = 0;
    std::string key;  // pending key at this position ("" = expecting a key)
    bool have_key = false;
    for (Token t = lex.next(); t.kind != Token::End; t = lex.next()) {
        if (t.kind == Token::Open) {
            ++depth;
            have_key = false;
            continue;
        }
        if (t.kind == Token::Close) {
            if (--depth < 0) break;
            have_key = false;
            continue;
        }
        if (!have_key) {
            key = std::move(t.text);
            have_key = true;
            continue;
        }
        // key "value": "path" anywhere, or a numeric key directly under the root block (pre-2021 form).
        if (alamo::iequals(key, "path") || (depth == 1 && all_digits(key))) out.push_back(std::move(t.text));
        have_key = false;
    }
    return out;
}

std::vector<std::string> find_steam_libraries(std::string_view foc_root, std::span<const std::string> steam_roots) {
    std::vector<std::string> out, keys;
    if (!foc_root.empty()) {
        std::error_code ec;
        fs::path p = fs::absolute(path_from_utf8(foc_root), ec);
        if (ec) p = path_from_utf8(foc_root);
        for (fs::path cur = p; !cur.empty(); cur = cur.parent_path()) {
            if (alamo::iequals(path_to_utf8(cur.filename()), "steamapps")) {
                std::string lib = path_to_utf8(cur.parent_path().make_preferred());
                for (char& c : lib)
                    if (c == '/') c = '\\';
                add_unique(out, keys, std::move(lib));
                break;
            }
            if (cur == cur.parent_path()) break;
        }
    }
    for (const std::string& root : steam_roots) {
        if (root.empty()) continue;
        add_unique(out, keys, root);
        add_vdf_libraries(join(root, "steamapps\\libraryfolders.vdf"), out, keys);
        add_vdf_libraries(join(root, "config\\libraryfolders.vdf"), out, keys);
    }
    return out;
}

std::vector<std::string> default_steam_roots() {
    std::vector<std::string> out, keys;
    const std::string env = env_utf8(L"ALAMO_STEAM_ROOT");
    if (!env.empty()) {
        size_t b = 0;
        while (b <= env.size()) {
            const size_t e = std::min(env.find(';', b), env.size());
            std::string part = env.substr(b, e - b);
            if (is_dir(part)) add_unique(out, keys, std::move(part));
            b = e + 1;
        }
        return out;
    }
    std::vector<std::string> candidates;
    candidates.push_back(registry_string(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"));
    candidates.push_back(registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\Valve\\Steam", L"InstallPath"));
    candidates.push_back(registry_string(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath"));
    candidates.emplace_back("C:\\Program Files (x86)\\Steam");
    for (std::string& c : candidates) {
        for (char& ch : c)
            if (ch == '/') ch = '\\';  // the registry's SteamPath uses '/'
        if (is_dir(c)) add_unique(out, keys, std::move(c));
    }
    return out;
}

std::optional<std::string> find_workshop_item(std::span<const std::string> libraries, std::string_view id, std::string_view app_id) {
    if (!all_digits(id) || !all_digits(app_id)) return std::nullopt;
    std::string rel = "steamapps\\workshop\\content\\";
    rel += app_id;
    rel += '\\';
    rel += id;
    for (const std::string& lib : libraries) {
        std::string dir = join(lib, rel);
        if (is_dir(dir)) return dir;
    }
    return std::nullopt;
}

SteamModResolver make_steam_workshop_resolver(std::vector<std::string> libraries, std::string app_id) {
    return [libs = std::move(libraries), app = std::move(app_id)](std::string_view id) {
        return find_workshop_item(libs, id, app);
    };
}

void install_steam_workshop_lookup() {
    LookupState& s = lookup_state();
    const std::lock_guard<std::mutex> lock(s.mu);
    s.installed = true;
    s.roots_known = false;  // read on first use
    s.roots.clear();
}

void install_steam_workshop_lookup(std::vector<std::string> steam_roots) {
    LookupState& s = lookup_state();
    const std::lock_guard<std::mutex> lock(s.mu);
    s.installed = true;
    s.roots_known = true;
    s.roots = std::move(steam_roots);
}

void uninstall_steam_workshop_lookup() {
    LookupState& s = lookup_state();
    const std::lock_guard<std::mutex> lock(s.mu);
    s.installed = false;
    s.roots_known = false;
    s.roots.clear();
}

SteamModResolver default_steam_resolver(std::string_view foc_root) {
    LookupState& s = lookup_state();
    std::vector<std::string> roots;
    {
        const std::lock_guard<std::mutex> lock(s.mu);
        if (!s.installed) return {};
        if (!s.roots_known) {
            s.roots = default_steam_roots();
            s.roots_known = true;
        }
        roots = s.roots;
    }
    return make_steam_workshop_resolver(find_steam_libraries(foc_root, roots));
}

}  // namespace alamo::vfs

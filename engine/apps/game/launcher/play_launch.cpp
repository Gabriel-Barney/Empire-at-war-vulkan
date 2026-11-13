// Play.exe's testable half (play_launch.hpp).
#include "play_launch.hpp"

#include <cstdio>
#include <cwctype>
#include <exception>
#include <system_error>

namespace alamo::play {
namespace fs = std::filesystem;

namespace {

bool dir_exists(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

bool path_exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

std::wstring trim(std::wstring s) {
    const auto space = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n' || c == L'"'; };
    size_t b = 0;
    while (b < s.size() && space(s[b])) ++b;
    size_t e = s.size();
    while (e > b && space(s[e - 1])) --e;
    return s.substr(b, e - b);
}

bool iequals(const std::wstring& a, const wchar_t* b) {
    const std::wstring bb(b);
    if (a.size() != bb.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::towlower(a[i]) != std::towlower(bb[i])) return false;
    return true;
}

}  // namespace

bool is_game_dir(const fs::path& dir) {
    if (dir.empty() || !dir_exists(dir / L"Data")) return false;
    return path_exists(dir / L"Data" / L"MegaFiles.xml") || path_exists(dir / L"StarWarsG.exe");
}

fs::path normalize_game_dir(std::wstring text) {
    text = trim(std::move(text));
    while (text.size() > 3 && (text.back() == L'\\' || text.back() == L'/')) text.pop_back();  // keep "C:\"
    if (text.empty()) return {};
    const fs::path dir(text);
    if (is_game_dir(dir)) return dir;
    if (is_game_dir(dir / L"corruption")) return dir / L"corruption";
    return {};
}

std::wstring first_line_utf8(std::string_view bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.remove_prefix(3);
    const size_t eol = bytes.find_first_of("\r\n");
    if (eol != std::string_view::npos) bytes = bytes.substr(0, eol);
    std::u8string u8;
    u8.reserve(bytes.size());
    for (const char c : bytes) u8.push_back(static_cast<char8_t>(c));
    try {
        return fs::path(u8).wstring();
    } catch (const std::exception&) {  // not UTF-8
        return {};
    }
}

fs::path find_game(const FindInputs& in) {
    if (!in.saved_file.empty() && path_exists(in.saved_file)) {
        std::string bytes;
        if (FILE* f = nullptr; _wfopen_s(&f, in.saved_file.c_str(), L"rb") == 0 && f) {
            char buf[4096];
            size_t n = 0;
            while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.append(buf, n);
            std::fclose(f);
        }
        if (fs::path g = normalize_game_dir(first_line_utf8(bytes)); !g.empty()) return g;
    }
    if (fs::path g = normalize_game_dir(in.env); !g.empty()) return g;
    for (const fs::path& lib : in.steam)
        if (fs::path g = normalize_game_dir((lib / L"steamapps" / L"common" / L"Star Wars Empire at War").wstring()); !g.empty()) return g;
    for (const fs::path& dir : in.fixed)
        if (is_game_dir(dir)) return dir;
    return {};
}

std::vector<std::wstring> game_args(const fs::path& game, const fs::path& cache, const std::vector<std::wstring>& user) {
    std::vector<std::wstring> out{L"--game", game.wstring(), L"--cache-dir", cache.wstring()};
    for (const std::wstring& a : user) {
        fs::path p(trim(a));
        if (!a.empty() && dir_exists(p)) {
            std::error_code ec;
            fs::path abs = fs::absolute(p, ec);
            if (ec) abs = p;
            abs = abs.lexically_normal();
            if (!abs.has_filename()) abs = abs.parent_path();  // "...\Mod\" -> "...\Mod"
            if (iequals(abs.filename().wstring(), L"Data")) abs = abs.parent_path();
            if (dir_exists(abs / L"Data")) {
                out.push_back(L"MODPATH=" + abs.wstring());
                continue;
            }
        }
        out.push_back(a);
    }
    return out;
}

std::wstring quote_arg(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++slashes;
        }
        if (i == arg.size()) {  // before the closing quote: every backslash doubled
            out.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"') {
            out.append(slashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(slashes, L'\\');
            out.push_back(arg[i]);
        }
    }
    out.push_back(L'"');
    return out;
}

std::wstring command_line(const fs::path& exe, const std::vector<std::wstring>& args) {
    std::wstring line = quote_arg(exe.wstring());
    for (const std::wstring& a : args) line += L" " + quote_arg(a);
    return line;
}

}  // namespace alamo::play

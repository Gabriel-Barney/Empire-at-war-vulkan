// Static scan of Lua 5.0 sources: called / defined names, classified against the binding table.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "alamo/lua/bindings.hpp"
#include "alamo/lua/script_loader.hpp"

namespace alamo::lua {

namespace {

enum class Tok : uint8_t { Name, String, Number, Punct };

struct Token {
    Tok kind;
    std::string_view text;
};

bool is_ident_start(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool is_ident(char c) { return is_ident_start(c) || (c >= '0' && c <= '9'); }

// Lua 5.0 long bracket [[ ... ]] (nests in 5.0). `i` points at the first '['; returns the index
// after the closing "]]" (or the end).
size_t skip_long(std::string_view s, size_t i) {
    int depth = 0;
    while (i < s.size()) {
        if (s[i] == '[' && i + 1 < s.size() && s[i + 1] == '[') {
            ++depth;
            i += 2;
        } else if (s[i] == ']' && i + 1 < s.size() && s[i + 1] == ']') {
            i += 2;
            if (--depth == 0) return i;
        } else {
            ++i;
        }
    }
    return i;
}

std::vector<Token> lex(std::string_view s) {
    std::vector<Token> out;
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '-' && i + 1 < s.size() && s[i + 1] == '-') {
            if (i + 3 < s.size() && s[i + 2] == '[' && s[i + 3] == '[') {
                i = skip_long(s, i + 2);
            } else {
                while (i < s.size() && s[i] != '\n') ++i;
            }
        } else if (c == '[' && i + 1 < s.size() && s[i + 1] == '[') {
            const size_t start = i;
            i = skip_long(s, i);
            out.push_back({Tok::String, s.substr(start, i - start)});
        } else if (c == '"' || c == '\'') {
            const size_t start = i++;
            while (i < s.size() && s[i] != c && s[i] != '\n') i += (s[i] == '\\' && i + 1 < s.size()) ? 2 : 1;
            if (i < s.size()) ++i;
            out.push_back({Tok::String, s.substr(start, i - start)});
        } else if (is_ident_start(c)) {
            const size_t start = i;
            while (i < s.size() && is_ident(s[i])) ++i;
            out.push_back({Tok::Name, s.substr(start, i - start)});
        } else if (c >= '0' && c <= '9') {
            const size_t start = i;
            while (i < s.size() && (is_ident(s[i]) || s[i] == '.')) ++i;
            out.push_back({Tok::Number, s.substr(start, i - start)});
        } else if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            ++i;
        } else {
            size_t len = 1;
            if (i + 1 < s.size()) {
                const std::string_view two = s.substr(i, 2);
                if (two == "==" || two == "~=" || two == "<=" || two == ">=" || two == "..") len = 2;
                if (two == ".." && i + 2 < s.size() && s[i + 2] == '.') len = 3;
            }
            out.push_back({Tok::Punct, s.substr(i, len)});
            i += len;
        }
    }
    return out;
}

constexpr std::string_view kKeywords[] = {"and",   "break", "do",     "else", "elseif", "end",   "false",
                                          "for",   "function", "if",  "in",   "local",  "nil",   "not",
                                          "or",    "repeat", "return", "then", "true",  "until", "while"};
bool is_keyword(std::string_view s) {
    return std::find(std::begin(kKeywords), std::end(kKeywords), s) != std::end(kKeywords);
}

constexpr std::string_view kLibraries[] = {"coroutine", "debug", "io", "math", "os", "string", "table", "_G"};
bool is_library(std::string_view s) {
    return std::find(std::begin(kLibraries), std::end(kLibraries), s) != std::end(kLibraries);
}

constexpr std::string_view kBuiltins[] = {
    "assert", "collectgarbage", "dofile",   "error",  "gcinfo",   "getfenv", "getmetatable", "ipairs",
    "loadfile", "loadstring",   "newproxy", "next",   "pairs",    "pcall",   "print",        "rawequal",
    "rawget", "rawset",         "require",  "setfenv", "setmetatable", "tonumber", "tostring", "type",
    "unpack", "xpcall"};
bool is_builtin(std::string_view s) {
    return std::find(std::begin(kBuiltins), std::end(kBuiltins), s) != std::end(kBuiltins);
}

bool call_follows(const std::vector<Token>& t, size_t i) {
    if (i + 1 >= t.size()) return false;
    const Token& n = t[i + 1];
    return (n.kind == Tok::Punct && (n.text == "(" || n.text == "{")) || n.kind == Tok::String;
}

}  // namespace

void scan_script(std::string_view source, ScriptScan& scan) {
    ++scan.files;
    const std::vector<Token> t = lex(source);
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i].kind != Tok::Name || is_keyword(t[i].text)) continue;
        const Token* prev = i > 0 ? &t[i - 1] : nullptr;
        const bool after_dot = prev && prev->kind == Tok::Punct && (prev->text == "." || prev->text == ":");
        const bool after_function = prev && prev->kind == Tok::Name && prev->text == "function";
        const bool after_local = prev && prev->kind == Tok::Name && prev->text == "local";
        const std::string name(t[i].text);
        if (after_function) {
            // function Name(  (global) - `function A.B(` defines a field, not a global.
            const bool field = i + 1 < t.size() && t[i + 1].kind == Tok::Punct &&
                               (t[i + 1].text == "." || t[i + 1].text == ":");
            const bool is_local = i >= 2 && t[i - 2].kind == Tok::Name && t[i - 2].text == "local";
            if (!field && !is_local) scan.defined.insert(name);
            continue;
        }
        if (!after_dot && !after_local && i + 1 < t.size() && t[i + 1].kind == Tok::Punct && t[i + 1].text == "=")
            scan.defined.insert(name);
        if (!call_follows(t, i)) continue;
        if (after_dot) {
            const Token* recv = i >= 2 ? &t[i - 2] : nullptr;
            const bool plain_recv =
                recv && recv->kind == Tok::Name && !(i >= 3 && t[i - 3].kind == Tok::Punct && t[i - 3].text == ".");
            if (plain_recv && is_library(recv->text))
                ++scan.library_calls[std::string(recv->text) + "." + name];
            else
                ++scan.method_calls[name];
        } else {
            ++scan.global_calls[name];
        }
    }
}

std::string scan_report(const ScriptScan& scan, ScanSummary* summary) {
    ScanSummary s;
    const auto table = binding_table();
    auto global_status = [&](const std::string& n) -> const char* {
        for (const BindingInfo& b : table) {
            if (b.kind != BindingKind::Global || n != b.name) continue;
            if (is_host_value_global(b.name)) return "host-value";
            return binding_implemented(b) ? "engine-implemented" : "engine-UNBOUND";
        }
        if (is_builtin(n)) return "lua-builtin";
        if (scan.defined.count(n)) return "script-defined";
        return "unknown";
    };
    auto method_status = [&](const std::string& n) -> const char* {
        bool known = false, impl = false;
        for (const BindingInfo& b : table) {
            if (b.kind != BindingKind::Method || n != b.name) continue;
            known = true;
            impl = impl || binding_implemented(b);
        }
        if (!known) return "script/other";
        return impl ? "engine-implemented" : "engine-UNBOUND";
    };
    using Row = std::pair<std::string, uint32_t>;
    auto sorted = [](const std::map<std::string, uint32_t>& m) {
        std::vector<Row> v(m.begin(), m.end());
        std::stable_sort(v.begin(), v.end(), [](const Row& a, const Row& b) { return a.second > b.second; });
        return v;
    };
    std::string body;
    char line[256];
    body += "\n== global calls (name, call sites, status)\n";
    for (const Row& r : sorted(scan.global_calls)) {
        const std::string st = global_status(r.first);
        if (st == "engine-implemented") {
            ++s.engine_globals_implemented;
            s.implemented_call_sites += r.second;
        } else if (st == "engine-UNBOUND") {
            ++s.engine_globals_unbound;
            s.unbound_call_sites += r.second;
        } else if (st == "unknown") {
            ++s.unknown_globals;
        }
        std::snprintf(line, sizeof line, "%-44s %6u  %s\n", r.first.c_str(), r.second, st.c_str());
        body += line;
    }
    body += "\n== method calls (name, call sites on any receiver, status)\n";
    for (const Row& r : sorted(scan.method_calls)) {
        const std::string st = method_status(r.first);
        if (st == "engine-implemented") {
            ++s.engine_methods_implemented;
            s.implemented_call_sites += r.second;
        } else if (st == "engine-UNBOUND") {
            ++s.engine_methods_unbound;
            s.unbound_call_sites += r.second;
        }
        std::snprintf(line, sizeof line, "%-44s %6u  %s\n", r.first.c_str(), r.second, st.c_str());
        body += line;
    }
    body += "\n== standard library calls\n";
    for (const Row& r : sorted(scan.library_calls)) {
        std::snprintf(line, sizeof line, "%-44s %6u\n", r.first.c_str(), r.second);
        body += line;
    }
    std::snprintf(line, sizeof line,
                  "files scanned %u, names defined or assigned by scripts (approx.) %zu\n"
                  "engine globals referenced: %u implemented, %u unbound; engine methods referenced: %u implemented, "
                  "%u unbound\n"
                  "engine call sites: %llu implemented, %llu unbound; unknown globals called: %u\n",
                  scan.files, scan.defined.size(), s.engine_globals_implemented, s.engine_globals_unbound,
                  s.engine_methods_implemented, s.engine_methods_unbound,
                  static_cast<unsigned long long>(s.implemented_call_sites),
                  static_cast<unsigned long long>(s.unbound_call_sites), s.unknown_globals);
    if (summary) *summary = s;
    return std::string(line) + body;
}

}  // namespace alamo::lua

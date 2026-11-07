// modc t08: the mod picker's data side (mod_picker.hpp).
#include "alamo/game/mod_picker.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>

#include "alamo/core/ascii.hpp"
#include "alamo/game/mod_check.hpp"
#include "alamo/vfs/steam_workshop.hpp"

namespace alamo::game {
namespace {
namespace fs = std::filesystem;

// Sub-folders of `dir` that have a Data folder, by name (byte order); `digits_only`: workshop item folders only.
std::vector<fs::path> mod_folders(const fs::path& dir, bool digits_only) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return out;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code ec2;
        if (!it->is_directory(ec2)) continue;
        const std::string name = it->path().filename().string();
        if (digits_only && (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)) continue;
        if (fs::is_directory(it->path() / "Data", ec2)) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) { return a.filename().string() < b.filename().string(); });
    return out;
}

void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

uint32_t hex4(std::string_view s) {
    uint32_t v = 0;
    for (const char c : s) {
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
        else return 0xFFFFFFFFu;
    }
    return v;
}

// The JSON string starting at text[i] == '"' (decoded), i past it; nullopt-like "" + ok = false on a malformed string.
bool json_string(const std::string& text, size_t& i, std::string& out) {
    out.clear();
    if (i >= text.size() || text[i] != '"') return false;
    ++i;
    while (i < text.size()) {
        const char c = text[i++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i >= text.size()) return false;
        const char e = text[i++];
        switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'u': {
                if (i + 4 > text.size()) return false;
                uint32_t cp = hex4(std::string_view(text).substr(i, 4));
                i += 4;
                if (cp == 0xFFFFFFFFu) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= text.size() && text[i] == '\\' && text[i + 1] == 'u') {
                    const uint32_t lo = hex4(std::string_view(text).substr(i + 2, 4));
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                append_utf8(out, cp);
                break;
            }
            default: out.push_back(e); break;  // \" \\ \/
        }
    }
    return false;
}

std::string fmt(const char* f, unsigned long long a, unsigned long long b = 0, unsigned long long c = 0) {
    char buf[160];
    std::snprintf(buf, sizeof buf, f, a, b, c);
    return buf;
}

}  // namespace

std::string modinfo_name(const std::string& folder) {
    std::ifstream in(fs::path(folder) / "modinfo.json", std::ios::binary);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF && static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF)
        text.erase(0, 3);
    // The top-level object's "name" key: walk the first object's members at depth 1 (nested objects such as "steamdata"
    // may have their own keys).
    int depth = 0;
    std::string s;
    for (size_t i = 0; i < text.size();) {
        const char c = text[i];
        if (c == '"') {
            if (!json_string(text, i, s)) return {};
            if (depth != 1) continue;
            size_t j = i;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) ++j;
            if (j >= text.size() || text[j] != ':') continue;  // a value, not a key
            ++j;
            while (j < text.size() && (text[j] == ' ' || text[j] == '\t' || text[j] == '\r' || text[j] == '\n')) ++j;
            if (s != "name") {
                i = j;
                continue;
            }
            std::string v;
            if (j < text.size() && text[j] == '"' && json_string(text, j, v)) return v;
            return {};
        }
        if (c == '{' || c == '[') ++depth;
        else if (c == '}' || c == ']') --depth;
        ++i;
    }
    return {};
}

std::vector<InstalledMod> list_installed_mods(const std::string& game, std::span<const std::string> workshop_libraries) {
    std::vector<InstalledMod> out;
    std::set<std::string> seen;
    std::vector<InstalledMod> workshop;
    for (const std::string& lib : workshop_libraries)
        for (const fs::path& item : mod_folders(fs::path(lib) / "steamapps" / "workshop" / "content" / std::string(vfs::kFocSteamAppId), true)) {
            InstalledMod m;
            m.id = item.filename().string();
            if (!seen.insert(m.id).second) continue;  // the first library wins (find_workshop_item order)
            m.workshop = true;
            m.path = item.string();
            m.name = modinfo_name(m.path);
            if (m.name.empty()) m.name = m.id;
            m.arg = "STEAMMOD=" + m.id;
            workshop.push_back(std::move(m));
        }
    std::sort(workshop.begin(), workshop.end(), [](const InstalledMod& a, const InstalledMod& b) {
        if (a.id.size() != b.id.size()) return a.id.size() < b.id.size();  // numeric order of the ids
        return a.id < b.id;
    });
    out = std::move(workshop);
    for (const fs::path& local : mod_folders(fs::path(game) / "Mods", false)) {
        InstalledMod m;
        m.id = local.filename().string();
        m.path = local.string();
        m.name = modinfo_name(m.path);
        if (m.name.empty()) m.name = m.id;
        m.arg = "MODPATH=Mods\\" + m.id;
        out.push_back(std::move(m));
    }
    return out;
}

bool is_mod_arg(std::string_view arg) { return ascii_istarts_with(arg, "MODPATH=") || ascii_istarts_with(arg, "STEAMMOD="); }

std::vector<std::string> relaunch_args(std::span<const std::string> args, std::span<const std::string> mod_args) {
    std::vector<std::string> out;
    for (const std::string& a : args)
        if (!is_mod_arg(a)) out.push_back(a);
    out.insert(out.end(), mod_args.begin(), mod_args.end());
    return out;
}

std::vector<std::string> mod_launch_args(const InstalledMod& mod, const ModCheckReport* report) {
    if (report && report->submod.likely && !report->submod.stacked_args.empty()) {
        std::vector<std::string> out;
        for (const std::string& a : report->submod.stacked_args)
            if (is_mod_arg(a)) out.push_back(a);
        if (!out.empty()) return out;
    }
    return {mod.arg};
}

std::vector<std::string> mod_report_summary(const ModCheckReport& r) {
    std::vector<std::string> out;
    const size_t errors = r.error_count(), warnings = r.warning_count();
    if (!r.loaded) {
        out.push_back("Does not load: " + (r.error.empty() ? std::string("a load step failed") : r.error));
    } else {
        out.push_back(fmt(errors ? "Loads with %llu error(s), %llu warning(s)" : "Loads: OK, %llu errors, %llu warning(s)",
                          static_cast<unsigned long long>(errors), static_cast<unsigned long long>(warnings)));
    }
    const ModCheckReport::Counts& c = r.counts;
    out.push_back(fmt("%llu campaigns, %llu planets, %llu factions", c.campaigns, c.planets, c.factions));
    out.push_back(fmt("%llu object types, %llu skirmish maps (%llu custom)", c.object_types, c.official_maps + c.custom_maps, c.custom_maps));
    if (r.no_startable_campaign()) out.push_back("No campaign has planets");
    else if (!r.campaigns_without_planets.empty())
        out.push_back(fmt("%llu campaign(s) without planets", r.campaigns_without_planets.size()));
    size_t mod_missing = 0;
    for (const MissingFile& m : r.missing_files) mod_missing += m.in_mod ? 1u : 0u;
    if (!r.missing_files.empty())
        out.push_back(fmt("%llu missing file(s), %llu named by the mod", r.missing_files.size(), mod_missing));
    out.push_back(fmt("Lua: %llu scripts, %llu do not compile, %llu main-chunk errors", r.lua_scripts, r.lua_errors.size(),
                      r.lua_run_errors.size()));
    if (!r.unbound_lua_calls.empty()) out.push_back(fmt("%llu engine call(s) the port does not bind", r.unbound_lua_calls.size()));
    if (r.shaders.checked)  // report v3 (modc t21): the effect audit
        out.push_back(fmt("Shaders: %llu effects (%llu from the mod), %llu issue(s)", r.shaders.effects, r.shaders.mod_effects,
                          r.shaders.issues.size()));
    const SubmodHint& h = r.submod;
    if (h.likely && !h.parents.empty()) {
        const ModParentCandidate& p = h.parents.front();
        out.push_back("Submod of " + p.id + (p.workshop ? " (workshop)" : " (Mods folder)") +
                      fmt(": it has %llu of %llu missing files", p.provides, h.missing) +
                      (p.mentioned ? ", named in modinfo.json" : ""));
        out.push_back("Launch stacks it on " + p.arg);
    } else if (h.missing > 0) {
        out.push_back(fmt("Names %llu file(s) no installed mod has", h.missing));
    }
    for (const std::string& e : r.check_errors) out.push_back("Check failed: " + e);
    return out;
}

std::string windows_command_line(std::span<const std::string> args) {
    std::string out;
    for (const std::string& a : args) {
        if (!out.empty()) out.push_back(' ');
        if (!a.empty() && a.find_first_of(" \t\n\v\"") == std::string::npos) {
            out += a;
            continue;
        }
        out.push_back('"');
        size_t slashes = 0;
        for (const char c : a) {
            if (c == '\\') {
                ++slashes;
                continue;
            }
            if (c == '"') out.append(2 * slashes + 1, '\\');
            else out.append(slashes, '\\');
            slashes = 0;
            out.push_back(c);
        }
        out.append(2 * slashes, '\\');
        out.push_back('"');
    }
    return out;
}

}  // namespace alamo::game

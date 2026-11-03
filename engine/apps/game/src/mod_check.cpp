#include "alamo/game/mod_check.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <map>
#include <string_view>

#include "alamo/vfs/vfs.hpp"
#ifdef ALAMO_GAME_HAVE_LUA_VM
#include "alamo/lua/lua_vm.hpp"
#endif

namespace alamo::game {

namespace {

// "DATA\XML\UNITS.XML" -> "XML"; "DATA\MEGAFILES.XML" -> "".
std::string data_folder(std::string_view path) {
    constexpr std::string_view kData = "DATA\\";
    if (path.substr(0, kData.size()) != kData) return {};
    path.remove_prefix(kData.size());
    const size_t sep = path.find('\\');
    return sep == std::string_view::npos ? std::string() : std::string(path.substr(0, sep));
}

void list_mod_files(const vfs::Vfs& v, ModCheckReport& r) {
    constexpr uint32_t kLower = vfs::Vfs::kLayerBase | vfs::Vfs::kLayerMeg | vfs::Vfs::kLayerFallback;
    const std::vector<std::string> mod = v.list("Data\\**", vfs::Vfs::kLayerMod);
    const std::vector<std::string> lower = v.list("Data\\**", kLower);  // both sorted by byte order
    std::map<std::string, ModFolderStats> folders;
    for (const std::string& p : mod) {
        ModFolderStats& f = folders[data_folder(p)];
        if (std::binary_search(lower.begin(), lower.end(), p)) ++f.overrides;
        else ++f.added;
    }
    for (auto& [name, f] : folders) {
        f.folder = name;
        r.mod_folders.push_back(f);
    }
}

void count_content(const GameData& d, ModCheckReport& r) {
    r.counts.campaigns = d.db.campaigns().size();
    r.counts.planets = d.db.planets().size();
    r.counts.trade_routes = d.db.trade_routes().size();
    r.counts.object_types = d.types.size();
    r.counts.factions = d.rules.factions.size();
#ifdef ALAMO_GAME_HAVE_GALACTIC_AI
    if (d.ai) {
        r.counts.ai = true;
        r.counts.ai_plans = d.ai->plan_files.size();
#ifdef ALAMO_SIM_HAVE_TACTICAL_AI
        if (d.ai->tactical) {
            r.counts.tactical_space_plans = d.ai->tactical->space_plans.size();
            r.counts.tactical_land_plans = d.ai->tactical->land_plans.size();
        }
#endif
    }
#endif
    if (d.vfs) {
        const SkirmishMapCatalog cat = build_skirmish_map_catalog(*d.vfs);
        r.counts.official_maps = skirmish_map_rows(cat, false).size();
        r.counts.custom_maps = skirmish_map_rows(cat, true).size();
    }
}

// Mods ship one campaign per playable faction, so the same skipped route shows up in many: one line per message.
void check_campaigns(const GameData& d, ModCheckReport& r) {
    std::map<std::string, std::vector<std::string>> by_message;  // message -> campaigns, in database order
    for (const data::CampaignDef& c : d.db.campaigns()) {
        std::vector<std::string> diag;
        const auto map = data::build_galaxy_map(d.db, c.name, &diag);
        ++r.campaigns_checked;
        if (map.planets.size() == 0) r.campaigns_without_planets.push_back(c.name);
        const std::string prefix = c.name + ": ";
        for (std::string_view msg : diag) {
            if (msg.starts_with(prefix)) msg.remove_prefix(prefix.size());
            by_message[std::string(msg)].push_back(c.name);
        }
    }
    for (const auto& [msg, camps] : by_message) {
        std::string line = msg + " (" + std::to_string(camps.size()) + (camps.size() == 1 ? " campaign: " : " campaigns: ");
        constexpr size_t kNamed = 3;
        for (size_t i = 0; i < camps.size() && i < kNamed; ++i) line += (i ? ", " : "") + camps[i];
        if (camps.size() > kNamed) line += ", ...";
        r.campaign_warnings.push_back(line + ")");
        r.campaign_warning_count += camps.size();
    }
}

void check_lua(const GameData& d, ModCheckReport& r) {
#ifdef ALAMO_GAME_HAVE_LUA_VM
    if (!d.vfs) return;
    lua::LuaHost host;
    host.files = d.files.get();
    lua::LuaVm vm(host);
    std::vector<uint8_t> bytes;
    for (const std::string& path : d.vfs->list("Data\\Scripts\\**.lua")) {
        ++r.lua_scripts;
        std::string err;
        if (!d.files->read(path, bytes)) {
            r.lua_errors.push_back(path + ": unreadable");
            continue;
        }
        const std::string_view chunk(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!vm.compile(chunk, path, &err)) r.lua_errors.push_back(path + ": " + err);
    }
#else
    (void)d;
    (void)r;
#endif
}

void append(std::string& s, const char* fmt, auto... args) {
    char buf[512];
    const int n = std::snprintf(buf, sizeof buf, fmt, args...);
    if (n > 0) s.append(buf, std::min(static_cast<size_t>(n), sizeof buf - 1));
}

void append_list(std::string& s, const std::vector<std::string>& items, size_t first, size_t count, bool verbose) {
    constexpr size_t kShown = 20;
    const size_t shown = verbose ? count : std::min(count, kShown);
    for (size_t i = 0; i < shown; ++i) s += "    " + items[first + i] + "\n";
    if (shown < count) append(s, "    ... %zu more (--verbose lists all)\n", count - shown);
}

}  // namespace

size_t ModCheckReport::error_count() const {
    size_t n = loaded ? 0 : 1;
    for (const auto& s : stages) n += s.ok ? 0 : 1;
    return n + campaigns_without_planets.size() + check_errors.size();
}

size_t ModCheckReport::warning_count() const {
    return vfs_warnings.size() + diagnostics.size() + campaign_warning_count + lua_errors.size();
}

ModCheckReport check_mod_data(const std::string& root, const std::vector<std::string>& args, const ModCheckOptions& options) {
    ModCheckReport r;
    GameData d;
    std::string err;
    try {
        r.loaded = load_game_data(root, args, d, &err);
        if (!r.loaded) r.error = err;
    } catch (const std::exception& e) {
        r.error = std::string("exception in the step after '") + (d.stages.empty() ? "VFS" : d.stages.back().name) + "': " + e.what();
    } catch (...) {
        r.error = std::string("unknown exception in the step after '") + (d.stages.empty() ? "VFS" : d.stages.back().name) + "'";
    }
    r.stages = d.stages;
    r.diagnostics = d.diagnostics;
    if (d.vfs) {
        r.vfs_layers = d.vfs->describe();
        r.vfs_warnings = d.vfs->warnings();
        r.mod_dirs = d.vfs->mod_dirs();
        list_mod_files(*d.vfs, r);
    }
    if (!r.loaded) return r;
    // The checks below read what loaded; a throw in one is reported as that check's error, the others still run.
    const auto guarded = [&](const char* what, auto&& fn) {
        try {
            fn();
        } catch (const std::exception& e) {
            r.check_errors.push_back(std::string(what) + ": exception: " + e.what());
        }
    };
    guarded("content", [&] { count_content(d, r); });
    if (options.campaigns) guarded("campaigns", [&] { check_campaigns(d, r); });
    if (options.lua) guarded("lua", [&] { check_lua(d, r); });
    return r;
}

std::string format_mod_report(const ModCheckReport& r, bool verbose) {
    std::string s;
    s += "== files\n";
    s += r.vfs_layers;
    if (!r.vfs_layers.empty() && r.vfs_layers.back() != '\n') s += '\n';
    append(s, "VFS warnings: %zu\n", r.vfs_warnings.size());
    append_list(s, r.vfs_warnings, 0, r.vfs_warnings.size(), verbose);
    if (r.mod_dirs.empty()) {
        s += "mod dirs: none (base game)\n";
    } else {
        uint32_t added = 0, overrides = 0;
        for (const auto& f : r.mod_folders) added += f.added, overrides += f.overrides;
        append(s, "mod files: %u new, %u replace a base / MEG / fallback file\n", added, overrides);
        for (const auto& f : r.mod_folders)
            append(s, "    Data\\%-16s %6u new %6u replaced\n", f.folder.empty() ? "." : f.folder.c_str(), f.added, f.overrides);
    }

    s += "== loaders\n";
    for (const auto& st : r.stages) {
        append(s, "%-16s %s %8.1f ms  %zu diagnostics\n", st.name.c_str(), st.ok ? "ok    " : "FAILED", st.ms, st.diag_count);
        append_list(s, r.diagnostics, st.first_diag, st.diag_count, verbose);
    }
    if (!r.loaded) s += "LOAD FAILED: " + r.error + "\n";

    if (r.loaded) {
        const auto& c = r.counts;
        s += "== content\n";
        append(s, "campaigns %zu, planet types %zu, trade routes %zu, object types %zu, factions %zu\n", c.campaigns, c.planets,
               c.trade_routes, c.object_types, c.factions);
        if (c.ai)
            append(s, "AI: %zu galactic plans, %zu space / %zu land tactical plans\n", c.ai_plans, c.tactical_space_plans,
                   c.tactical_land_plans);
        else
            s += "AI: no AI data\n";
        append(s, "skirmish maps: %zu official, %zu custom\n", c.official_maps, c.custom_maps);

        append(s, "== campaigns\n%zu checked, %zu without planets (errors), %zu skipped rows (warnings)\n", r.campaigns_checked,
               r.campaigns_without_planets.size(), r.campaign_warning_count);
        append_list(s, r.campaigns_without_planets, 0, r.campaigns_without_planets.size(), verbose);
        append_list(s, r.campaign_warnings, 0, r.campaign_warnings.size(), verbose);
        append(s, "== lua\n%zu scripts compiled, %zu do not compile (warnings)\n", r.lua_scripts, r.lua_errors.size());
        append_list(s, r.lua_errors, 0, r.lua_errors.size(), verbose);
    }
    if (!r.check_errors.empty()) {
        append(s, "== check errors: %zu\n", r.check_errors.size());
        append_list(s, r.check_errors, 0, r.check_errors.size(), true);
    }
    append(s, "== result: %s, %zu errors, %zu warnings\n", r.error_count() ? "ERRORS" : "OK", r.error_count(), r.warning_count());
    return s;
}

}  // namespace alamo::game

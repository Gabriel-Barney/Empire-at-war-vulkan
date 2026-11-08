#include "alamo/game/mod_check.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

#include "alamo/core/hash.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/data/xml_list.hpp"
#include "alamo/objects/coverage.hpp"
#include "alamo/objects/field_schema.hpp"
#include "alamo/game/mod_picker.hpp"  // mod_folders, workshop_content_dir (cu9 t03)
#include "alamo/vfs/vfs.hpp"
#include "json_text.hpp"  // the JSON string escaper, shared with benchmark.cpp
#ifdef ALAMO_GAME_HAVE_MATERIALS
#include "alamo/assets/model.hpp"
#include "alamo/materials/effect_audit.hpp"
#include "alamo/materials/effect_loader.hpp"
#include "alamo/shaderxlat/dxc.hpp"
#endif
#ifdef ALAMO_GAME_HAVE_LUA_VM
#include <lua.h>  // Lua 5.0 is built as C++ (engine/lua/CMakeLists.txt): no extern "C"

#include "alamo/lua/bindings.hpp"
#include "alamo/lua/lua_vm.hpp"
#include "alamo/lua/script_loader.hpp"
#include "alamo/lua/script_remaining.hpp"
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

using MissingMap = std::map<std::string, MissingFile>;  // by path: sorted, deterministic

// True when `path` resolves to a file of a mod folder (the VFS mod layer).
bool in_mod_layer(const vfs::Vfs& v, std::string_view path) {
    const auto res = v.resolve(path);
    return res && res->layer == vfs::Vfs::Resolution::Mod;
}

void note_missing(MissingMap& m, std::string path, std::string_view by, bool in_mod) {
    MissingFile& f = m[path];
    if (f.references++ == 0) {
        f.path = std::move(path);
        f.referenced_by = std::string(by);
    }
    f.in_mod = f.in_mod || in_mod;
}

// Entries of every Data\XML\*Files.xml list (GameObjectFiles, FactionFiles, CampaignFiles, SFXEventFiles, ...): each
// <File> names Data\XML\<entry> (the convention of every list the loaders read: game_database.cpp read_list,
// object_types.cpp). A list that fails to parse is reported by its loader, not here.
void check_list_files(const GameData& d, MissingMap& m) {
    std::vector<uint8_t> bytes;
    for (const std::string& list : d.vfs->list("Data\\XML\\*Files.xml")) {
        if (!d.files->read(list, bytes)) continue;
        const bool in_mod = in_mod_layer(*d.vfs, list);
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, list);
        for (const data::ListedXmlEntry& en : data::listed_xml_entries(doc)) {  // cu5 t02 (none when !doc.ok())
            std::string path = normalize_name(en.path);
            if (!d.files->exists(path)) note_missing(m, std::move(path), list, in_mod);
        }
    }
}

// The model of each object type (first definitions only: a later duplicate never loads). A name without an extension
// is also tried with .ALO (PROVISIONAL: how the original completes such names is not traced; this avoids a false report).
void check_models(const GameData& d, MissingMap& m) {
    static constexpr std::string_view kTags[] = {"Space_Model_Name", "Land_Model_Name", "Galactic_Model_Name"};
    const objects::ObjectTypeDatabase& db = d.raw;
    const objects::TypeColumns& t = db.types();
    const objects::FieldColumns& f = db.fields();
    std::set<std::string> present;  // paths already found (types share models)
    for (objects::TypeIndex i = 0; i < t.size(); ++i) {
        if (t.first_definition[i] != i) continue;
        for (objects::FieldIndex fi = t.field_begin[i]; fi < t.field_end[i]; ++fi) {
            if (f.depth[fi] != 1 || f.text[fi].empty()) continue;
            const std::string_view tag = db.tag_name(fi);
            const auto* hit = std::find_if(std::begin(kTags), std::end(kTags), [&](std::string_view k) { return iequals(k, tag); });
            if (hit == std::end(kTags)) continue;
            std::string path = normalize_name(std::string("Data\\Art\\Models\\") + std::string(f.text[fi]));
            if (present.count(path)) continue;
            bool found = d.files->exists(path);
            if (!found && path.find('.', path.rfind('\\')) == std::string::npos) found = d.files->exists(path + ".ALO");
            if (found) {
                present.insert(std::move(path));
                continue;
            }
            note_missing(m, std::move(path), std::string(t.name[i]) + " (" + std::string(*hit) + ")", in_mod_layer(*d.vfs, db.file_of(i)));
        }
    }
}

void check_files(const GameData& d, ModCheckReport& r) {
    if (!d.vfs) return;
    MissingMap m;
    check_list_files(d, m);
    check_models(d, m);
    for (auto& [path, f] : m) r.missing_files.push_back(std::move(f));
}

// modc t15: the XML databases besides the object types that the port loads. Per database: where its entries are (the
// <File> entries of a Data\XML list, as the loader reads them, or one Data\XML file), the entry element below the root
// (or the root itself), and which child tags count as read: the rows of the original's descriptor table for the class
// (docs/re/xml_field_tables.md through objects::field_schema(), by segment owner) plus the tags the port's loader reads
// that no row lists (cited per database). Only direct children of an entry are checked, except for a walked file.
// modc t25 (docs/re/xml_field_tables.md "modc t25" sections): CommandBarComponents.xml, whose entries are every child of
// the root (the element name is the component name); RadarMap.xml, which the original walks as a whole (no element path
// checked, a known tag read at any depth, an element without text only a container).
struct XmlDatabase {
    std::string_view name;
    std::string_view list;     // Data\XML\<list> holds the entry files; "" = `file`
    std::string_view file;     // Data\XML\<file> holds the entries
    std::string_view element;  // entry element below the root; "" = the root element is the one entry; "*" = every child
    std::span<const std::string_view> owners;     // field_schema() segment owners
    std::span<const std::string_view> port_tags;  // read by the port's loader, no table row
    bool walk = false;  // every element with text at any depth below the root is checked; entries = its parents
};

constexpr std::string_view kFaction[] = {"Faction"};
constexpr std::string_view kCampaign[] = {"Campaign"};
constexpr std::string_view kTradeRoute[] = {"TradeRoute"};
constexpr std::string_view kHardPoint[] = {"HardPoint"};
constexpr std::string_view kHardPointPort[] = {"Type"};  // render/hardpoints.cpp (hard point type)
constexpr std::string_view kGameConstants[] = {"GameConstants"};
constexpr std::string_view kGameConstantsPort[] = {"Ships_Per_Stack"};  // apps/game app_screens.cpp (fleet stacks)
constexpr std::string_view kSfxEvent[] = {"SFXEvent"};
constexpr std::string_view kMusicEvent[] = {"MusicEvent"};
// SpeechEvent has no table in the doc: what audio_db.cpp speech_fields() reads.
constexpr std::string_view kSpeechEventPort[] = {"Files", "Text_ID", "Volume_Percent", "Overlap_Test"};
constexpr std::string_view kAudio[] = {"Audio"};
constexpr std::string_view kAudioPort[] = {"Ambient_Map_Sounds"};  // audio/ambience.hpp (map ambient beds)
constexpr std::string_view kTacticalCamera[] = {"TacticalCamera"};
constexpr std::string_view kTacticalCameraPort[] = {"MinHeightAboveTerrain"};  // ui/camera.cpp
constexpr std::string_view kMovie[] = {"Movie"};
constexpr std::string_view kDifficulty[] = {"Difficulty_Adjustment"};
constexpr std::string_view kLightning[] = {"LightningEffect"};
constexpr std::string_view kPrioritySet[] = {"Priority_Set"};
constexpr std::string_view kMousePointer[] = {"MousePointer"};
constexpr std::string_view kCommandBar[] = {"CommandBarComponent"};
constexpr std::string_view kRadarMap[] = {"RadarMapSettings", "Radar_Map_Event", "Passability_Color_Settings"};
// The walker's own names (xml_field_tables.md "Elements read outside the descriptor tables"): attributes select a block.
constexpr std::string_view kRadarMapSpecial[] = {"Passability_Color_Settings", "Color", "Radar_Map_Event"};

constexpr XmlDatabase kXmlDatabases[] = {
    {"factions", "FactionFiles.xml", "", "Faction", kFaction, {}},
    {"campaigns", "CampaignFiles.xml", "", "Campaign", kCampaign, {}},
    {"trade routes", "TradeRouteFiles.xml", "", "TradeRoute", kTradeRoute, {}},
    {"hard points", "HardPointDataFiles.xml", "", "HardPoint", kHardPoint, kHardPointPort},
    {"game constants", "", "GameConstants.xml", "", kGameConstants, kGameConstantsPort},
    {"sfx events", "SFXEventFiles.xml", "", "SFXEvent", kSfxEvent, {}},
    {"music events", "", "MusicEvents.xml", "MusicEvent", kMusicEvent, {}},
    {"speech events", "", "SpeechEvents.xml", "SpeechEvent", {}, kSpeechEventPort},
    {"audio", "", "Audio.xml", "", kAudio, kAudioPort},
    {"tactical cameras", "", "TacticalCameras.xml", "TacticalCamera", kTacticalCamera, kTacticalCameraPort},
    {"movies", "", "Movies.xml", "Movie", kMovie, {}},
    {"difficulty adjustments", "", "DifficultyAdjustments.xml", "Difficulty_Adjustment", kDifficulty, {}},
    {"lightning effects", "", "LightningEffectTypes.xml", "LightningEffect", kLightning, {}},
    {"targeting priority sets", "TargetingPrioritySetFiles.xml", "", "Priority_Set", kPrioritySet, {}},
    {"mouse pointers", "", "MousePointers.xml", "MousePointer", kMousePointer, {}},  // as app game_main.cpp reads it
    {"command bar components", "CommandBarComponentFiles.xml", "", "*", kCommandBar, {}},
    {"radar map", "", "RadarMap.xml", "", kRadarMap, kRadarMapSpecial, true},
};

// Every descendant of `n` with non-empty text, with its parent (walk databases).
void walk_text_elements(data::XmlNode n, std::vector<std::pair<data::XmlNode, data::XmlNode>>& out) {
    for (data::XmlNode c : n.children()) {
        if (!c.text_trimmed().empty()) out.emplace_back(n, c);
        walk_text_elements(c, out);
    }
}

// The Data\XML files of a database in load order, each once.
std::vector<std::string> database_files(const GameData& d, const XmlDatabase& db) {
    std::vector<std::string> out;
    if (db.list.empty()) {
        out.push_back("Data\\XML\\" + std::string(db.file));
        return out;
    }
    std::vector<uint8_t> bytes;
    if (!d.files->read("Data\\XML\\" + std::string(db.list), bytes)) return out;
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, std::string(db.list));
    std::set<std::string> seen;
    for (data::ListedXmlEntry& en : data::listed_xml_entries(doc))  // cu5 t02 (none when !doc.ok())
        if (seen.insert(normalize_name(en.path)).second) out.push_back(std::move(en.path));
    return out;
}

void check_database_tags(const GameData& d, const XmlDatabase& db, ModCheckReport& r) {
    std::set<std::string, std::less<>> known;
    for (const objects::FieldSchema& row : objects::field_schema())
        for (const std::string_view o : db.owners)
            if (iequals(row.owner, o)) known.insert(ascii_lower(row.tag));
    for (const std::string_view t : db.port_tags) known.insert(ascii_lower(t));
    struct Seen {
        UnknownXmlTag u;
        std::set<std::string> entries;
    };
    std::map<std::string, Seen> unknown;  // by lower-case tag: sorted, deterministic
    std::vector<uint8_t> bytes;
    for (const std::string& path : database_files(d, db)) {
        if (!d.files->read(path, bytes)) continue;  // a missing list entry is reported by the files check
        const data::XmlDocument doc = data::XmlDocument::parse(bytes, path);
        if (!doc.ok()) continue;  // its loader reports the parse error
        const bool in_mod = d.vfs && in_mod_layer(*d.vfs, path);
        std::vector<std::pair<data::XmlNode, data::XmlNode>> checked;  // (entry, child)
        if (db.walk) {
            walk_text_elements(doc.root(), checked);
        } else {
            std::vector<data::XmlNode> entries;
            if (db.element.empty()) entries.push_back(doc.root());
            else
                for (data::XmlNode e : doc.root().children())
                    if (db.element == "*" || iequals(e.name(), db.element)) entries.push_back(e);
            for (const data::XmlNode e : entries)
                for (data::XmlNode c : e.children()) checked.emplace_back(e, c);
        }
        {
            for (const auto& [e, c] : checked) {
                const std::string entry = path + "#" + std::to_string(e.index());
                std::string key = ascii_lower(c.name());
                if (known.count(key)) continue;
                Seen& s = unknown[std::move(key)];
                if (s.u.occurrences++ == 0) {
                    s.u.database = std::string(db.name);
                    s.u.tag = std::string(c.name());
                    s.u.first_file = path;
                }
                s.u.in_mod = s.u.in_mod || in_mod;
                s.entries.insert(entry);
            }
        }
    }
    for (auto& [key, s] : unknown) {
        s.u.types = static_cast<uint32_t>(s.entries.size());
        r.unknown_xml_tags.push_back(std::move(s.u));
    }
}

void check_xml_tags(const GameData& d, ModCheckReport& r) {
    const objects::CoverageReport cov = objects::build_coverage(d.raw, 0);
    const auto& files = d.raw.files();
    for (const objects::TagCoverage& t : cov.tags) {
        if (!t.unknown() || t.occurrences == 0) continue;
        UnknownXmlTag u;
        u.database = "object types";
        u.tag = std::string(t.name);
        u.occurrences = t.occurrences;
        u.types = t.types;
        if (!t.files.empty() && t.files.front() < files.size()) u.first_file = files[t.files.front()].path;
        for (const uint32_t fi : t.files)
            if (fi < files.size() && d.vfs && in_mod_layer(*d.vfs, files[fi].path)) {
                u.in_mod = true;
                break;
            }
        r.unknown_xml_tags.push_back(std::move(u));
    }
    if (d.files)
        for (const XmlDatabase& db : kXmlDatabases) check_database_tags(d, db, r);
    std::stable_sort(r.unknown_xml_tags.begin(), r.unknown_xml_tags.end(), [](const UnknownXmlTag& a, const UnknownXmlTag& b) {
        if (a.occurrences != b.occurrences) return a.occurrences > b.occurrences;
        if (a.database != b.database) return a.database < b.database;
        return a.tag < b.tag;
    });
}

// ---- modc t21: shaders ----
// "DATA\ART\SHADERS\TERRAIN\X.FX" -> "terrain"; ...\ENGINE\... -> "engine"; else "models".
[[maybe_unused]] std::string_view shader_drawn_by(std::string_view path) {
    const std::string p = normalize_name(path);
    if (p.find("\\SHADERS\\TERRAIN\\") != std::string::npos) return "terrain";
    if (p.find("\\SHADERS\\ENGINE\\") != std::string::npos) return "engine";
    return "models";
}

// "MeshGloss.fx" / "Data\Art\Shaders\MESHGLOSS.FXO" -> "MESHGLOSS" (the form ShaderIssue::effect has).
[[maybe_unused]] std::string effect_base(std::string_view name) {
    const size_t slash = name.find_last_of("\\/");
    if (slash != std::string_view::npos) name.remove_prefix(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string_view::npos) name = name.substr(0, dot);
    return normalize_name(name);
}

#ifdef ALAMO_GAME_HAVE_MATERIALS
// The models the object types name (first definitions, every tag ending in Model_Name), found through the files, sorted.
std::vector<std::string> object_type_models(const GameData& d) {
    const objects::ObjectTypeDatabase& db = d.raw;
    const objects::TypeColumns& t = db.types();
    const objects::FieldColumns& f = db.fields();
    std::set<std::string> paths;
    constexpr std::string_view kSuffix = "MODEL_NAME";
    for (objects::TypeIndex i = 0; i < t.size(); ++i) {
        if (t.first_definition[i] != i) continue;
        for (objects::FieldIndex fi = t.field_begin[i]; fi < t.field_end[i]; ++fi) {
            if (f.depth[fi] != 1 || f.text[fi].empty()) continue;
            const std::string tag = normalize_name(db.tag_name(fi));
            if (tag.size() < kSuffix.size() || tag.compare(tag.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) continue;
            std::string path = normalize_name(std::string("Data\\Art\\Models\\") + std::string(f.text[fi]));
            if (path.find('.', path.rfind('\\')) == std::string::npos) path += ".ALO";
            if (path.size() < 4 || path.compare(path.size() - 4, 4, ".ALO") != 0) continue;
            if (d.files->exists(path)) paths.insert(std::move(path));
        }
    }
    return {paths.begin(), paths.end()};
}
#endif

void check_shaders(const GameData& d, ModCheckReport& r, bool compile) {
#ifdef ALAMO_GAME_HAVE_MATERIALS
    if (!d.vfs) return;
    ShaderCheck& sc = r.shaders;
    shaderxlat::RuntimeDxc dxc;
    const bool have_dxc = compile && dxc.load();
    // As alamo_effect_audit (materials/tools/effect_audit_tool.cpp): the library gets the D.1 preload list, the audit
    // every effect of the shader directories including Shaders\Engine.
    materials::EffectLibrary lib(*d.files);
    lib.set_shader_files(materials::shader_files_from_vfs(*d.vfs));
    const std::vector<materials::ShaderFileEntry> files = materials::shader_files_from_vfs(*d.vfs, true);
    const materials::EffectAudit a = materials::audit_effects(lib, files, have_dxc ? &dxc : nullptr, nullptr);
    sc.checked = true;
    sc.effects = a.effects;
    sc.mod_effects = a.mod_effects;
    sc.loaded = a.loaded;
    sc.translated_passes = a.translated_passes;
    sc.translated_ok = a.translated_ok;
    sc.compiled = a.compiled;
    sc.fx_sources = a.sources;
    sc.fx_sources_compiled = a.sources_compiled;
    std::map<std::string, size_t> model_effects;  // effect base name -> index of its first issue with drawn_by "models"
    for (const materials::ShaderIssue& i : a.issues) {
        ShaderIssueEntry e;
        e.kind = materials::shader_issue_kind_name(i.kind);
        e.effect = i.effect;
        e.path = i.path;
        e.in_mod = i.in_mod;
        e.detail = i.detail;
        e.drawn_by = std::string(shader_drawn_by(i.path));
        // The device-init engine effects (GLOBAL, ALAMOENGINE, the Prim* sprites, ...) are the engine's, wherever they lie.
        const std::vector<std::string>& engine = materials::engine_effect_load_sequence();
        if (std::find(engine.begin(), engine.end(), e.effect) != engine.end()) e.drawn_by = "engine";
        if (e.drawn_by == "models") model_effects.emplace(e.effect, sc.issues.size());
        sc.issues.push_back(std::move(e));
    }
    if (model_effects.empty()) return;  // vanilla: no issue, no model read
    // Which models name the issue effects: every material of every object-type model (render submeshes and the
    // others alike: the effect is loaded for them either way).
    std::map<std::string, std::pair<uint32_t, std::string>> users;  // effect -> (models, first model)
    for (const std::string& path : object_type_models(d)) {
        assets::ModelAsset m;
        if (!assets::load_model(*d.files, path, m)) continue;  // a broken model is not this check's finding
        ++sc.models_read;
        std::set<std::string> named;
        for (const assets::Submesh& sub : m.submeshes) {
            std::string base = effect_base(sub.material.effect);
            if (model_effects.count(base)) named.insert(std::move(base));
        }
        for (const std::string& eff : named) {
            auto& u = users[eff];
            if (u.first++ == 0) u.second = path;
        }
    }
    for (ShaderIssueEntry& e : sc.issues) {
        if (e.drawn_by != "models") continue;
        const auto it = users.find(e.effect);
        if (it == users.end()) continue;
        e.models = it->second.first;
        e.first_model = it->second.second;
    }
#else
    (void)d;
    (void)r;
    (void)compile;
#endif
}

// ---- modc t16: submods ----
namespace fs = std::filesystem;

// Data\XML files the mod's own list files name that no layer has (MissingFile::in_mod; models are not counted: a full
// mod misses art too, only a mod whose own database lists another mod's XML files is a submod).
std::vector<std::string> missing_list_entries(const ModCheckReport& r) {
    std::vector<std::string> out;
    for (const MissingFile& f : r.missing_files)
        if (f.in_mod && f.path.starts_with("DATA\\XML\\")) out.push_back(f.path);
    return out;
}

bool same_folder(const fs::path& a, const std::string& b) {
    std::error_code ec;
    return fs::equivalent(a, fs::path(b), ec) && !ec;
}

// How many of `paths` the candidate's own files have: a Vfs with the candidate as its only tree (its loose Data, the
// MEGs its MegaFiles.xml / Patch*.meg name; no EaW, no FoC), so every hit is the candidate's.
uint32_t provided_by(const fs::path& candidate, const std::vector<std::string>& paths) {
    vfs::VfsConfig cfg;
    cfg.foc_root = candidate.string();
    cfg.eaw_root = std::string();
    std::string err;
    const std::unique_ptr<vfs::Vfs> v = vfs::Vfs::create(cfg, &err);
    if (!v) return 0;
    uint32_t n = 0;
    for (const std::string& p : paths) n += v->exists(p) ? 1u : 0u;
    return n;
}

// Every digit run of the mod folders' modinfo.json files (workshop ids a submod names, e.g. "Requires ... 1125571106").
std::set<std::string> modinfo_numbers(const std::vector<std::string>& mod_dirs) {
    std::set<std::string> out;
    for (const std::string& dir : mod_dirs) {
        std::ifstream in(fs::path(dir) / "modinfo.json", std::ios::binary);
        if (!in) continue;
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        for (size_t i = 0; i < text.size();) {
            if (text[i] < '0' || text[i] > '9') {
                ++i;
                continue;
            }
            const size_t b = i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9') ++i;
            out.insert(text.substr(b, i - b));
        }
    }
    return out;
}

void check_submod(const ModCheckReport& base, const ModCheckOptions& o, SubmodHint& h) {
    if (base.mod_dirs.empty()) return;
    const std::vector<std::string> missing = missing_list_entries(base);
    h.missing = static_cast<uint32_t>(missing.size());
    if (missing.empty()) return;
    const std::set<std::string> mentioned = modinfo_numbers(base.mod_dirs);
    std::vector<ModParentCandidate> cands;
    const auto consider = [&](const fs::path& dir, bool workshop) {
        for (const std::string& m : base.mod_dirs)
            if (same_folder(dir, m)) return;  // already in the stack
        ModParentCandidate c;
        c.id = dir.filename().string();
        c.workshop = workshop;
        c.path = dir.string();
        c.arg = workshop ? "STEAMMOD=" + c.id : "MODPATH=Mods\\" + c.id;
        c.mentioned = workshop && mentioned.count(c.id) != 0;
        c.provides = provided_by(dir, missing);
        if (c.provides > 0) cands.push_back(std::move(c));
    };
    std::set<std::string> seen_items;  // a workshop id found in an earlier library wins (find_workshop_item order)
    for (const std::string& lib : o.workshop_libraries)
        for (const fs::path& item : mod_folders(workshop_content_dir(lib), true))  // mod_picker.hpp (cu9 t03)
            if (seen_items.insert(item.filename().string()).second) consider(item, true);
    for (const fs::path& local : mod_folders(fs::path(base.game) / "Mods", false)) consider(local, false);
    std::sort(cands.begin(), cands.end(), [](const ModParentCandidate& a, const ModParentCandidate& b) {
        if (a.provides != b.provides) return a.provides > b.provides;
        if (a.mentioned != b.mentioned) return a.mentioned;
        if (a.workshop != b.workshop) return a.workshop;
        return a.id < b.id;
    });
    constexpr size_t kListed = 5;
    if (cands.size() > kListed) cands.resize(kListed);
    h.parents = std::move(cands);
    if (h.parents.empty()) return;
    const ModParentCandidate& top = h.parents.front();
    h.likely = top.provides >= SubmodHint::kMinProvided && 2 * top.provides >= h.missing;
    if (h.likely) {
        h.stacked_args = base.args;
        h.stacked_args.push_back(top.arg);
    }
}

#ifdef ALAMO_GAME_HAVE_LUA_VM
// Engine bindings the port registers as "unbound" stubs, by name (globals and methods separately; a method name counts
// as unbound only when no class implements it, as lua::scan_report does).
struct UnboundNames {
    std::set<std::string, std::less<>> globals, methods;
    UnboundNames() {
        std::set<std::string, std::less<>> method_impl;
        for (const lua::BindingInfo& b : lua::binding_table()) {
            if (b.kind == lua::BindingKind::Method) {
                if (lua::binding_implemented(b)) method_impl.insert(b.name);
                else methods.insert(b.name);
            } else if (!lua::binding_implemented(b) && !lua::is_host_value_global(b.name)) {
                globals.insert(b.name);
            }
        }
        for (const std::string& n : method_impl) methods.erase(n);
    }
};

// Scripts the original starts as scripts of their own: those under a script root other than Library (story plots, AI
// plans, GameObject scripts, evaluators, free stores, interventions, miscellaneous; lua/script_loader.hpp). Library
// scripts are only reached through require from those, and often rely on globals their requirer set, so running one on
// its own reports errors the game never hits.
bool is_entry_script(std::string_view path) {
    bool entry = false;
    for (const std::string_view root : lua::script_roots()) {
        if (path.size() <= root.size() || !iequals(path.substr(0, root.size()), root)) continue;
        if (iequals(root, "Data\\Scripts\\Library\\")) return false;
        entry = true;
    }
    return entry;
}

using UnboundMap = std::map<std::pair<std::string, bool>, UnboundLuaCall>;  // (name, method): sorted, deterministic

// modc t12. Run before an entry script's main chunk, in its VM: GlobalValue becomes a table whose Get / Set call the
// engine's (dot or colon call) and which records, in __alamo_mod_check_unset, every string key Get found unset.
constexpr std::string_view kGlobalValueProbe = R"lua(
local gv = GlobalValue
if gv == nil then return end
local get, set = gv.Get, gv.Set
local unset = {}
__alamo_mod_check_unset = unset
local proxy = {}
proxy.Get = function(a, b)
  local k = a
  if a == proxy then k = b end
  local v = get(k)
  if v == nil and type(k) == "string" then unset[k] = true end
  return v
end
proxy.Set = function(a, b, c)
  if a == proxy then return set(b, c) end
  return set(a, b)
end
GlobalValue = proxy
)lua";

// The keys the probe recorded, sorted.
std::vector<std::string> unset_global_values(lua::LuaVm& vm) {
    std::vector<std::string> keys;
    lua_State* L = vm.state();
    lua_pushstring(L, "__alamo_mod_check_unset");
    lua_gettable(L, LUA_GLOBALSINDEX);
    if (lua_istable(L, -1)) {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) keys.emplace_back(lua_tostring(L, -2), lua_strlen(L, -2));
            lua_pop(L, 1);
        }
    }
    lua_pop(L, 1);
    std::sort(keys.begin(), keys.end());
    return keys;
}

// Literal keys of GlobalValue.Set("KEY", ...) / GlobalValue:Set('KEY', ...) calls outside line comments.
void scan_global_value_sets(std::string_view src, const std::string& path, std::map<std::string, std::string>& set_by) {
    constexpr std::string_view kName = "GlobalValue";
    for (size_t at = src.find(kName); at != std::string_view::npos; at = src.find(kName, at + 1)) {
        const size_t nl = src.rfind('\n', at);
        const size_t bol = nl == std::string_view::npos ? 0 : nl + 1;
        if (src.substr(bol, at - bol).find("--") != std::string_view::npos) continue;
        size_t i = at + kName.size();
        const auto skip_ws = [&] {
            while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
        };
        skip_ws();
        if (i >= src.size() || (src[i] != '.' && src[i] != ':')) continue;
        ++i;
        skip_ws();
        if (src.substr(i, 3) != "Set") continue;
        i += 3;
        skip_ws();
        if (i >= src.size() || src[i] != '(') continue;
        ++i;
        skip_ws();
        if (i >= src.size() || (src[i] != '"' && src[i] != '\'')) continue;
        const char q = src[i++];
        const size_t end = src.find(q, i);
        if (end == std::string_view::npos || src.substr(i, end - i).find('\n') != std::string_view::npos) continue;
        set_by.emplace(std::string(src.substr(i, end - i)), path);  // the first script (path order) keeps it
    }
}

// Literal script names of require("X") / require "X" / require('X') calls outside line comments, by upper-case base name
// (the part after the last '/' or '\'): name -> the first script (path order) requiring it.
void scan_requires(std::string_view src, const std::string& path, std::map<std::string, std::string>& required_by) {
    constexpr std::string_view kName = "require";
    const auto ident = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; };
    for (size_t at = src.find(kName); at != std::string_view::npos; at = src.find(kName, at + 1)) {
        if ((at > 0 && ident(src[at - 1])) || (at + kName.size() < src.size() && ident(src[at + kName.size()]))) continue;
        const size_t nl = src.rfind('\n', at);
        const size_t bol = nl == std::string_view::npos ? 0 : nl + 1;
        if (src.substr(bol, at - bol).find("--") != std::string_view::npos) continue;
        size_t i = at + kName.size();
        const auto skip_ws = [&] {
            while (i < src.size() && (src[i] == ' ' || src[i] == '\t')) ++i;
        };
        skip_ws();
        if (i < src.size() && src[i] == '(') {
            ++i;
            skip_ws();
        }
        if (i >= src.size() || (src[i] != '"' && src[i] != '\'')) continue;
        const char q = src[i++];
        const size_t end = src.find(q, i);
        if (end == std::string_view::npos || end == i || src.substr(i, end - i).find('\n') != std::string_view::npos) continue;
        std::string_view name = src.substr(i, end - i);
        const size_t sep = name.find_last_of("/\\");
        if (sep != std::string_view::npos) name.remove_prefix(sep + 1);
        required_by.emplace(normalize_name(name), path);
    }
}

// The script name of the require that found no file, from the port's message (engine/lua lua_vm.cpp load_script:
// "cannot find script '<name>' in Data\Scripts"); "" for any other error.
std::string missing_require_of(std::string_view err) {
    constexpr std::string_view kPrefix = "cannot find script '";
    const size_t b = err.find(kPrefix);
    if (b == std::string_view::npos) return {};
    const size_t from = b + kPrefix.size();
    const size_t e = err.find("' in Data\\Scripts", from);
    return e == std::string_view::npos ? std::string() : std::string(err.substr(from, e - from));
}

// XML elements whose text names a script the original loads by that name (with or without .lua): an object type's or a
// story plot's Lua_Script (OBJECT_SCRIPTS.md, STORY.md), an AI player's free store scripts, GameConstants'
// Game_Scoring_Script_Name (GAME_SCORING.md). Matched case-insensitively.
constexpr std::string_view kScriptNameTags[] = {"LUA_SCRIPT", "GALACTICFREESTORESCRIPT", "SPACEFREESTORESCRIPT", "LANDFREESTORESCRIPT",
                                                "GAME_SCORING_SCRIPT_NAME"};

// For each upper-case script base name in `wanted`, the first Data\XML file (path order) with a kScriptNameTags element
// naming it: "<file> (<tag>)". A byte scan of the upper-cased text, so it also sees files no loader reads.
std::map<std::string, std::string> xml_script_names(const GameData& d, const std::set<std::string>& wanted) {
    std::map<std::string, std::string> found;
    std::vector<uint8_t> bytes;
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    for (const std::string& path : d.vfs->list("Data\\XML\\**.xml")) {
        if (found.size() == wanted.size()) break;
        if (!d.files->read(path, bytes)) continue;
        std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        ascii_upper_in_place(text);
        for (const std::string_view tag : kScriptNameTags) {
            const std::string open = "<" + std::string(tag);
            for (size_t at = text.find(open); at != std::string::npos; at = text.find(open, at + 1)) {
                const size_t after = at + open.size();
                if (after >= text.size() || (text[after] != '>' && !space(text[after]))) continue;
                const size_t gt = text.find('>', after);
                if (gt == std::string::npos) break;
                const size_t lt = text.find('<', gt + 1);
                if (lt == std::string::npos) break;
                std::string_view v = std::string_view(text).substr(gt + 1, lt - gt - 1);
                while (!v.empty() && space(v.front())) v.remove_prefix(1);
                while (!v.empty() && space(v.back())) v.remove_suffix(1);
                if (v.size() > 4 && v.substr(v.size() - 4) == ".LUA") v.remove_suffix(4);
                const std::string key(v);
                if (wanted.count(key) && !found.count(key)) found.emplace(key, path + " (" + std::string(tag) + ")");
            }
        }
    }
    return found;
}

// The script root of `path` (lua::script_roots(), longest match) or "".
std::string_view script_root_of(std::string_view path) {
    std::string_view best;
    for (const std::string_view root : lua::script_roots())
        if (path.size() > root.size() && root.size() > best.size() && iequals(path.substr(0, root.size()), root)) best = root;
    return best;
}

// modc t24. A main chunk that fails on a require no layer can satisfy fails the same way in retail: its Lua 5.0.2
// require raises "could not load package" (docs/re/MODC_LOADER.md section 6), so the script does not load. That matters
// when the data loads the script; a mod's own script that nothing names never runs (e.g. Thrawn's Revenge's
// GameScoring_old.lua beside the GameScoring.lua that GameConstants names). Fills missing_require / named_by and moves
// the unnamed ones to lua_unused_scripts. Only the roots whose scripts are loaded by a name the data gives count as
// unnamed (GameObject, Story, FreeStore, Miscellaneous); the AI folders are enumerated, Evaluators / Interventions not
// traced, Library is never an entry.
void classify_missing_requires(const GameData& d, ModCheckReport& r, const std::map<std::string, std::string>& required_by) {
    // The game without the mods (the same arguments minus MODPATH= / STEAMMOD=): a mod's MEGs mount in the MEG layer like
    // the base ones, so the base files are only told apart by a tree without the mods. Created on first need.
    std::unique_ptr<vfs::Vfs> base_tree;
    bool base_tried = false;
    const auto in_base_game = [&](const std::string& path) {
        if (!base_tried) {
            base_tried = true;
            std::vector<std::string> rest;
            for (const std::string& a : r.args)
                if (!(a.size() >= 8 && iequals(std::string_view(a).substr(0, 8), "MODPATH=")) &&
                    !(a.size() >= 9 && iequals(std::string_view(a).substr(0, 9), "STEAMMOD=")))
                    rest.push_back(a);
            vfs::VfsConfig cfg;
            cfg.foc_root = r.game;
            if (vfs::parse_command_line(rest, cfg)) base_tree = vfs::Vfs::create(cfg, nullptr);
        }
        return base_tree && base_tree->exists(path);
    };
    constexpr std::string_view kByName[] = {"Data\\Scripts\\GameObject\\", "Data\\Scripts\\Story\\", "Data\\Scripts\\FreeStore\\",
                                            "Data\\Scripts\\Miscellaneous\\"};
    std::set<std::string> unresolved;  // base names left for the XML scan
    for (LuaIssue& e : r.lua_run_errors) {
        e.missing_require = missing_require_of(e.error);
        if (e.missing_require.empty()) continue;
        const std::string_view root = script_root_of(e.path);
        const std::string base = effect_base(e.path);
        if (std::none_of(std::begin(kByName), std::end(kByName), [&](std::string_view k) { return iequals(k, root); })) {
            e.named_by = "script root " + std::string(root);
        } else if (in_base_game(e.path)) {
            e.named_by = "base script";
        } else if (const auto it = required_by.find(base); it != required_by.end() && !iequals(it->second, e.path)) {
            e.named_by = "require in " + it->second;
        } else {
            unresolved.insert(base);
        }
    }
    if (unresolved.empty()) return;
    const std::map<std::string, std::string> named = xml_script_names(d, unresolved);
    std::vector<LuaIssue> kept;
    for (LuaIssue& e : r.lua_run_errors) {
        if (!e.missing_require.empty() && e.named_by.empty()) {
            const auto it = named.find(effect_base(e.path));
            if (it != named.end()) e.named_by = it->second;
        }
        (!e.missing_require.empty() && e.named_by.empty() ? r.lua_unused_scripts : kept).push_back(std::move(e));
    }
    r.lua_run_errors = std::move(kept);
}

void add_unbound(UnboundMap& calls, const std::map<std::string, uint32_t>& found, const std::set<std::string, std::less<>>& unbound,
                 bool method) {
    for (const auto& [name, sites] : found) {
        if (!unbound.count(name)) continue;
        UnboundLuaCall& c = calls[{name, method}];
        c.name = name;
        c.method = method;
        c.call_sites += sites;
        ++c.scripts;
    }
}
#endif

void check_lua(const GameData& d, ModCheckReport& r, bool run_chunks) {
#ifdef ALAMO_GAME_HAVE_LUA_VM
    if (!d.vfs) return;
    lua::LuaHost host;
    host.files = d.files.get();
    host.types = &d.types;
    lua::LuaVm compiler(host);
    const UnboundNames unbound;
    UnboundMap calls;
    std::vector<uint8_t> bytes;
    // One GlobalValue store for every run, as the session has one: a value a main chunk sets is seen by later ones.
    lua::GlobalValueStore global_values;
    std::map<std::string, std::string> set_by;  // GlobalValue key -> first script (path order) that sets it
    std::map<std::string, std::string> required_by;  // required script base name -> first script requiring it
    struct Failed {
        LuaIssue issue;
        std::vector<std::string> unset;  // GlobalValue keys the chunk read while unset
    };
    std::vector<Failed> failed;
    for (const std::string& path : d.vfs->list("Data\\Scripts\\**.lua")) {
        ++r.lua_scripts;
        std::string err;
        const bool in_mod = in_mod_layer(*d.vfs, path);
        if (!d.files->read(path, bytes)) {
            r.lua_errors.push_back({path, "unreadable", in_mod});
            continue;
        }
        const std::string_view chunk(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        if (!compiler.compile(chunk, path, &err)) {
            r.lua_errors.push_back({path, err, in_mod});
            continue;
        }
        lua::ScriptScan scan;
        lua::scan_script(chunk, scan);
        scan_global_value_sets(chunk, path, set_by);
        scan_requires(chunk, path, required_by);
        add_unbound(calls, scan.global_calls, unbound.globals, false);
        add_unbound(calls, scan.method_calls, unbound.methods, true);
        if (!run_chunks || !is_entry_script(path)) continue;
        // What loading the script runs: its main chunk (requires, definitions, top-level calls), in a VM of its own as
        // the original gives every script (lua_vm.hpp). No objects, players or session values exist here.
        lua::LuaVmOptions opt;
        const size_t slash = path.rfind('\\');
        opt.script_name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        lua::LuaVm vm(host, opt);
        lua::attach_global_values(vm, &global_values);
        std::string probe_err;
        const bool probed = vm.run(kGlobalValueProbe, "=mod_check_global_value_probe", &probe_err);
        ++r.lua_run;
        if (!vm.run(chunk, path, &err)) {
            Failed f{LuaIssue(path, err, in_mod), {}};
            if (probed) f.unset = unset_global_values(vm);
            failed.push_back(std::move(f));
        }
    }
    // A failed chunk that read a key some script sets (in its main chunk after this one, or in a function the session
    // calls) depends on session state; the others are load warnings.
    for (Failed& f : failed) {
        for (const std::string& key : f.unset) {
            const auto it = set_by.find(key);
            if (it == set_by.end()) continue;
            f.issue.global_value = key;
            f.issue.set_by = it->second;
            break;
        }
        (f.issue.global_value.empty() ? r.lua_run_errors : r.lua_session_errors).push_back(std::move(f.issue));
    }
    classify_missing_requires(d, r, required_by);
    for (auto& [key, c] : calls) r.unbound_lua_calls.push_back(std::move(c));
    std::sort(r.unbound_lua_calls.begin(), r.unbound_lua_calls.end(), [](const UnboundLuaCall& a, const UnboundLuaCall& b) {
        if (a.call_sites != b.call_sites) return a.call_sites > b.call_sites;
        if (a.name != b.name) return a.name < b.name;
        return a.method < b.method;
    });
#else
    (void)d;
    (void)r;
    (void)run_chunks;
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
    return n + (no_startable_campaign() ? 1u : 0u) + check_errors.size();
}

size_t ModCheckReport::warning_count() const {
    const size_t menu_only = no_startable_campaign() ? 0 : campaigns_without_planets.size();
    return vfs_warnings.size() + diagnostics.size() + menu_only + campaign_warning_count + missing_files.size() +
           unknown_xml_tags.size() + lua_errors.size() + lua_run_errors.size() + unbound_lua_calls.size() + shaders.issues.size();
}

ModCheckReport check_mod_data(const std::string& root, const std::vector<std::string>& args, const ModCheckOptions& options) {
    ModCheckReport r;
    r.game = root;
    r.args = args;
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
    if (options.files) guarded("files", [&] { check_files(d, r); });
    if (options.xml_tags) guarded("xml tags", [&] { check_xml_tags(d, r); });
    if (options.lua) guarded("lua", [&] { check_lua(d, r, options.lua_run); });
    if (options.shaders) guarded("shaders", [&] { check_shaders(d, r, options.shader_compile); });
    if (options.files && options.submod) guarded("submod", [&] { check_submod(r, options, r.submod); });
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

        append(s, "== campaigns\n%zu checked, %zu without planets (%s), %zu skipped rows (warnings)\n", r.campaigns_checked,
               r.campaigns_without_planets.size(),
               r.no_startable_campaign() ? "ERROR: no campaign can be started" : "warnings: menu entries that cannot be started",
               r.campaign_warning_count);
        append_list(s, r.campaigns_without_planets, 0, r.campaigns_without_planets.size(), verbose);
        append_list(s, r.campaign_warnings, 0, r.campaign_warnings.size(), verbose);
        append(s, "== missing files\n%zu files named by the data that no layer has (warnings)\n", r.missing_files.size());
        {
            std::vector<std::string> lines;
            for (const MissingFile& f : r.missing_files)
                lines.push_back(std::string(f.in_mod ? "[mod] " : "      ") + f.path + "  (" + f.referenced_by +
                                (f.references > 1 ? ", +" + std::to_string(f.references - 1) + " more" : std::string()) + ")");
            append_list(s, lines, 0, lines.size(), verbose);
        }
        {
            std::map<std::string, size_t> per_db;
            for (const UnknownXmlTag& t : r.unknown_xml_tags) ++per_db[t.database];
            std::string dbs;
            for (const auto& [db, n] : per_db) append(dbs, "%s%s %zu", dbs.empty() ? ": " : ", ", db.c_str(), n);
            append(s, "== xml tags\n%zu tags the original does not read (warnings)%s\n", r.unknown_xml_tags.size(), dbs.c_str());
        }
        {
            std::vector<std::string> lines;
            for (const UnknownXmlTag& t : r.unknown_xml_tags) {
                std::string line;
                append(line, "%s%-14.14s %-40s %6u uses in %u entries, first in %s", t.in_mod ? "[mod] " : "      ", t.database.c_str(),
                       t.tag.c_str(), t.occurrences, t.types, t.first_file.c_str());
                lines.push_back(std::move(line));
            }
            append_list(s, lines, 0, lines.size(), verbose);
        }
        const auto issue_lines = [](const std::vector<LuaIssue>& v) {
            std::vector<std::string> lines;
            for (const LuaIssue& e : v) lines.push_back(std::string(e.in_mod ? "[mod] " : "      ") + e.text());
            return lines;
        };
        append(s, "== lua\n%zu scripts compiled, %zu do not compile (warnings)\n", r.lua_scripts, r.lua_errors.size());
        const std::vector<std::string> compile_lines = issue_lines(r.lua_errors);
        append_list(s, compile_lines, 0, compile_lines.size(), verbose);
        append(s, "%zu entry scripts' main chunks run, %zu raise an error (warnings)\n", r.lua_run, r.lua_run_errors.size());
        std::vector<std::string> run_lines = issue_lines(r.lua_run_errors);
        for (size_t i = 0; i < run_lines.size(); ++i)
            if (!r.lua_run_errors[i].missing_require.empty()) run_lines[i] += "  [missing require; loaded: " + r.lua_run_errors[i].named_by + "]";
        append_list(s, run_lines, 0, run_lines.size(), verbose);
        append(s, "%zu more fail on a require of a missing script and are the mod's own scripts nothing names: unused-script leads (info)\n",
               r.lua_unused_scripts.size());
        const std::vector<std::string> unused_lines = issue_lines(r.lua_unused_scripts);
        append_list(s, unused_lines, 0, unused_lines.size(), verbose);
        append(s, "%zu more fail only for session state: they read a GlobalValue a script sets in the session (info)\n",
               r.lua_session_errors.size());
        {
            std::map<std::pair<std::string, std::string>, size_t> by_key;  // (key, setter) -> scripts
            for (const LuaIssue& e : r.lua_session_errors) ++by_key[{e.global_value, e.set_by}];
            std::vector<std::string> lines;
            for (const auto& [k, n] : by_key)
                lines.push_back("GlobalValue \"" + k.first + "\" (set by " + k.second + "): " + std::to_string(n) + " scripts");
            if (verbose)
                for (const LuaIssue& e : r.lua_session_errors) lines.push_back(std::string(e.in_mod ? "[mod] " : "      ") + e.text());
            append_list(s, lines, 0, lines.size(), verbose);
        }
        append(s, "%zu unbound engine functions / methods called (warnings)\n", r.unbound_lua_calls.size());
        {
            std::vector<std::string> lines;
            for (const UnboundLuaCall& u : r.unbound_lua_calls) {
                std::string line;
                append(line, "%-44s %-6s %6u call sites in %u scripts", u.name.c_str(), u.method ? "method" : "global", u.call_sites,
                       u.scripts);
                lines.push_back(std::move(line));
            }
            append_list(s, lines, 0, lines.size(), verbose);
        }
    }
    if (r.loaded && r.shaders.checked) {
        const ShaderCheck& sc = r.shaders;
        append(s, "== shaders\n%zu effects (%zu from mods), %zu loaded, %zu track-B passes, %zu %s; %zu issues (warnings)\n", sc.effects,
               sc.mod_effects, sc.loaded, sc.translated_passes, sc.translated_ok, sc.compiled ? "compiled" : "translated (no DXC)",
               sc.issues.size());
        std::vector<std::string> lines;
        for (const ShaderIssueEntry& e : sc.issues) {
            std::string line;
            append(line, "%s%-17s %-28s %s", e.in_mod ? "[mod] " : "      ", e.kind.c_str(), e.effect.c_str(), e.path.c_str());
            if (e.drawn_by != "models") line += "  (" + e.drawn_by + " effect)";
            else if (e.models) line += "  (named by " + std::to_string(e.models) + " models, first " + e.first_model + ")";
            else line += "  (no object type's model names it)";
            if (!e.detail.empty()) line += "  " + e.detail;
            lines.push_back(std::move(line));
        }
        append_list(s, lines, 0, lines.size(), verbose);
        if (sc.fx_sources) append(s, "    .fx sources %zu, %zu loaded compiled (alamo_fx_prebuild cache)\n", sc.fx_sources, sc.fx_sources_compiled);
        if (sc.models_read) append(s, "    (%zu object type models read for the cross-reference)\n", sc.models_read);
    }
    if (r.submod.missing > 0) {
        const SubmodHint& h = r.submod;
        append(s, "== submod\n%u XML files the mod's own lists name are in no layer; %s\n", h.missing,
               h.likely ? "likely a submod (info)" : "no installed mod has most of them");
        for (const ModParentCandidate& c : h.parents)
            append(s, "    %-14s %5u of %u%s  %s\n", c.arg.c_str(), c.provides, h.missing, c.mentioned ? "  (named in modinfo.json)" : "",
                   c.path.c_str());
        if (h.likely) {
            std::string args;
            for (const std::string& a : h.stacked_args) args += " " + a;
            s += "    check it stacked:" + args + "\n";
        }
    }
    if (!r.check_errors.empty()) {
        append(s, "== check errors: %zu\n", r.check_errors.size());
        append_list(s, r.check_errors, 0, r.check_errors.size(), true);
    }
    append(s, "== result: %s, %zu errors, %zu warnings%s\n", r.error_count() ? "ERRORS" : "OK", r.error_count(), r.warning_count(),
           r.submod.likely ? " (likely a submod: check it stacked, see == submod)" : "");
    return s;
}

namespace {

// Minimal writer: objects and arrays with comma handling, two-space indent, keys in call order.
class Json {
public:
    std::string out;
    void open(char c) {
        sep();
        out.push_back(c);
        first_.push_back(true);
    }
    void close(char c) {
        const bool empty = first_.back();
        first_.pop_back();
        if (!empty) newline();
        out.push_back(c);
    }
    void key(std::string_view k) {
        sep();
        json_text::json_str(out, k);
        out += ": ";
        pending_value_ = true;
    }
    void str(std::string_view v) {
        sep();
        json_text::json_str(out, v);
    }
    void num(uint64_t v) {
        sep();
        out += std::to_string(v);
    }
    void real(double v) {
        sep();
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.1f", v >= 0 && v < 1e15 ? v : 0.0);
        out += buf;
    }
    void boolean(bool v) {
        sep();
        out += v ? "true" : "false";
    }
    // key: [the strings v[first, first + count)]
    void strings(std::string_view k, const std::vector<std::string>& v, size_t first = 0, size_t count = SIZE_MAX) {
        key(k);
        open('[');
        const size_t begin = std::min(first, v.size());
        const size_t end = begin + std::min(count, v.size() - begin);
        for (size_t i = begin; i < end; ++i) str(v[i]);
        close(']');
    }

private:
    void newline() {
        out.push_back('\n');
        out.append(2 * first_.size(), ' ');
    }
    void sep() {
        if (pending_value_) {
            pending_value_ = false;
            return;
        }
        if (first_.empty()) return;
        if (!first_.back()) out.push_back(',');
        first_.back() = false;
        newline();
    }
    std::vector<bool> first_;
    bool pending_value_ = false;
};

enum class LuaIssueKeys { Plain, Session, Require };
void lua_issues(Json& j, std::string_view k, const std::vector<LuaIssue>& v, LuaIssueKeys keys = LuaIssueKeys::Plain) {
    j.key(k);
    j.open('[');
    for (const LuaIssue& e : v) {
        j.open('{');
        j.key("path");
        j.str(e.path);
        j.key("error");
        j.str(e.error);
        j.key("in_mod");
        j.boolean(e.in_mod);
        if (keys == LuaIssueKeys::Require) {
            j.key("missing_require");
            j.str(e.missing_require);
            j.key("named_by");
            j.str(e.named_by);
        }
        if (keys == LuaIssueKeys::Session) {
            j.key("global_value");
            j.str(e.global_value);
            j.key("set_by");
            j.str(e.set_by);
        }
        j.close('}');
    }
    j.close(']');
}

void count(Json& j, std::string_view k, uint64_t v) {
    j.key(k);
    j.num(v);
}

}  // namespace

std::string format_mod_report_json(const ModCheckReport& r) {
    Json j;
    j.open('{');
    j.key("schema");
    j.str("alamo.mod_report");
    count(j, "version", 3);
    j.key("result");
    j.str(r.error_count() ? "errors" : "ok");
    count(j, "errors", r.error_count());
    count(j, "warnings", r.warning_count());
    j.key("game");
    j.str(r.game);
    j.strings("args", r.args);
    j.key("loaded");
    j.boolean(r.loaded);
    j.key("load_error");
    j.str(r.error);

    j.key("files");
    j.open('{');
    j.strings("mod_dirs", r.mod_dirs);
    j.strings("vfs_warnings", r.vfs_warnings);
    uint64_t added = 0, replaced = 0;
    for (const auto& f : r.mod_folders) added += f.added, replaced += f.overrides;
    count(j, "added", added);
    count(j, "replaced", replaced);
    j.key("folders");
    j.open('[');
    for (const auto& f : r.mod_folders) {
        j.open('{');
        j.key("folder");
        j.str(f.folder);
        count(j, "added", f.added);
        count(j, "replaced", f.overrides);
        j.close('}');
    }
    j.close(']');
    j.close('}');

    j.key("loaders");
    j.open('[');
    for (const auto& st : r.stages) {
        j.open('{');
        j.key("name");
        j.str(st.name);
        j.key("ok");
        j.boolean(st.ok);
        j.key("ms");
        j.real(st.ms);
        j.strings("diagnostics", r.diagnostics, st.first_diag, st.diag_count);
        j.close('}');
    }
    j.close(']');

    const auto& c = r.counts;
    j.key("content");
    j.open('{');
    count(j, "campaigns", c.campaigns);
    count(j, "planets", c.planets);
    count(j, "trade_routes", c.trade_routes);
    count(j, "object_types", c.object_types);
    count(j, "factions", c.factions);
    j.key("ai");
    j.boolean(c.ai);
    count(j, "ai_plans", c.ai_plans);
    count(j, "tactical_space_plans", c.tactical_space_plans);
    count(j, "tactical_land_plans", c.tactical_land_plans);
    count(j, "official_maps", c.official_maps);
    count(j, "custom_maps", c.custom_maps);
    j.close('}');

    j.key("campaigns");
    j.open('{');
    count(j, "checked", r.campaigns_checked);
    j.strings("without_planets", r.campaigns_without_planets);
    count(j, "skipped_rows", r.campaign_warning_count);
    j.strings("warnings", r.campaign_warnings);
    j.close('}');

    j.key("missing_files");
    j.open('[');
    for (const MissingFile& f : r.missing_files) {
        j.open('{');
        j.key("path");
        j.str(f.path);
        j.key("referenced_by");
        j.str(f.referenced_by);
        count(j, "references", f.references);
        j.key("in_mod");
        j.boolean(f.in_mod);
        j.close('}');
    }
    j.close(']');

    j.key("unknown_xml_tags");
    j.open('[');
    for (const UnknownXmlTag& t : r.unknown_xml_tags) {
        j.open('{');
        j.key("database");
        j.str(t.database);
        j.key("tag");
        j.str(t.tag);
        count(j, "occurrences", t.occurrences);
        count(j, "types", t.types);
        j.key("first_file");
        j.str(t.first_file);
        j.key("in_mod");
        j.boolean(t.in_mod);
        j.close('}');
    }
    j.close(']');

    j.key("lua");
    j.open('{');
    count(j, "scripts", r.lua_scripts);
    lua_issues(j, "compile_errors", r.lua_errors);
    count(j, "chunks_run", r.lua_run);
    lua_issues(j, "run_errors", r.lua_run_errors, LuaIssueKeys::Require);
    lua_issues(j, "session_errors", r.lua_session_errors, LuaIssueKeys::Session);
    lua_issues(j, "unused_scripts", r.lua_unused_scripts, LuaIssueKeys::Require);
    j.key("unbound_calls");
    j.open('[');
    for (const UnboundLuaCall& u : r.unbound_lua_calls) {
        j.open('{');
        j.key("name");
        j.str(u.name);
        j.key("kind");
        j.str(u.method ? "method" : "global");
        count(j, "call_sites", u.call_sites);
        count(j, "scripts", u.scripts);
        j.close('}');
    }
    j.close(']');
    j.close('}');

    const ShaderCheck& sc = r.shaders;
    j.key("shaders");
    j.open('{');
    j.key("checked");
    j.boolean(sc.checked);
    count(j, "effects", sc.effects);
    count(j, "mod_effects", sc.mod_effects);
    count(j, "loaded", sc.loaded);
    count(j, "translated_passes", sc.translated_passes);
    count(j, "translated_ok", sc.translated_ok);
    j.key("compiled");
    j.boolean(sc.compiled);
    count(j, "fx_sources", sc.fx_sources);
    count(j, "fx_sources_compiled", sc.fx_sources_compiled);
    count(j, "models_read", sc.models_read);
    j.key("issues");
    j.open('[');
    for (const ShaderIssueEntry& e : sc.issues) {
        j.open('{');
        j.key("kind");
        j.str(e.kind);
        j.key("effect");
        j.str(e.effect);
        j.key("path");
        j.str(e.path);
        j.key("in_mod");
        j.boolean(e.in_mod);
        j.key("detail");
        j.str(e.detail);
        j.key("drawn_by");
        j.str(e.drawn_by);
        count(j, "models", e.models);
        j.key("first_model");
        j.str(e.first_model);
        j.close('}');
    }
    j.close(']');
    j.close('}');

    j.key("submod");
    j.open('{');
    j.key("likely");
    j.boolean(r.submod.likely);
    count(j, "missing_list_entries", r.submod.missing);
    j.key("parents");
    j.open('[');
    for (const ModParentCandidate& p : r.submod.parents) {
        j.open('{');
        j.key("id");
        j.str(p.id);
        j.key("kind");
        j.str(p.workshop ? "workshop" : "local");
        j.key("path");
        j.str(p.path);
        j.key("arg");
        j.str(p.arg);
        count(j, "provides", p.provides);
        j.key("mentioned");
        j.boolean(p.mentioned);
        j.close('}');
    }
    j.close(']');
    j.strings("stacked_args", r.submod.stacked_args);
    j.close('}');

    j.strings("check_errors", r.check_errors);
    j.close('}');
    j.out.push_back('\n');
    return std::move(j.out);
}

}  // namespace alamo::game

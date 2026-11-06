// Story plot model (include/alamo/lua/story_model.hpp). Behaviour: docs/re/STORY.md (section numbers
// in the comments); port choices [port], the doc's own uncertainty marks [M]/[L] kept.
#include "alamo/lua/story_model.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>

#include "alamo/core/bytes.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/lua/lua_vm.hpp"

#include <lua.h>

namespace alamo::lua {

namespace {

constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr size_t kNpos = static_cast<size_t>(-1);
constexpr uint32_t kMaxFireDepth = 256;  // cascade / TRIGGER_EVENT recursion guard [port]
constexpr uint32_t kMaxResetDepth = 3;   // RESET_BRANCH (0x140b3073c, Â§4.5)
constexpr double kSpeechDoneTimeout = 60.0;

char up(char ch) { return (ch >= 'a' && ch <= 'z') ? static_cast<char>(ch - 'a' + 'A') : ch; }

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& ch : out) ch = up(ch);
    return out;
}

bool iequal(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (up(a[i]) != up(b[i])) return false;
    return true;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

// Strict decimal number (whole string). Locale: the engine runs in the "C" locale.
bool parse_number(std::string_view s, double& out) {
    s = trim(s);
    if (s.empty() || s.size() > 63) return false;
    char buf[64];
    s.copy(buf, s.size());
    buf[s.size()] = 0;
    char* end = nullptr;
    out = std::strtod(buf, &end);
    return end == buf + s.size();
}

// atoi-like (leading integer part; "2.0" -> 2, "" / junk -> fallback).
int32_t parse_int(std::string_view s, int32_t fallback) {
    s = trim(s);
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    if (i >= s.size() || s[i] < '0' || s[i] > '9') return fallback;
    int64_t v = 0;
    for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i) {
        v = v * 10 + (s[i] - '0');
        if (v > 0x7FFFFFFFLL) return neg ? -2147483647 - 1 : 2147483647;
    }
    return static_cast<int32_t>(neg ? -v : v);
}

double parse_float(std::string_view s, double fallback) {
    double d = 0;
    return parse_number(s, d) ? d : fallback;
}

// "Event_Param12" -> 12 for prefix "EVENT_PARAM"; 0 if not that form.
uint32_t param_index(std::string_view tag, std::string_view prefix) {
    if (tag.size() <= prefix.size() || !iequal(tag.substr(0, prefix.size()), prefix)) return 0;
    uint32_t n = 0;
    for (char ch : tag.substr(prefix.size())) {
        if (ch < '0' || ch > '9' || n > 1000) return 0;
        n = n * 10 + static_cast<uint32_t>(ch - '0');
    }
    return n;
}

void set_param(std::vector<StoryParam>& v, uint32_t index1, StoryParam p) {
    if (v.size() < index1) v.resize(index1);
    v[index1 - 1] = std::move(p);
}

const StoryParam& param_at(const std::vector<StoryParam>& v, size_t i) {
    static const StoryParam kEmpty;
    return i < v.size() ? v[i] : kEmpty;
}
std::string_view text_at(const std::vector<StoryParam>& v, size_t i) { return trim(param_at(v, i).text); }

std::string base_name(std::string_view path) {
    const size_t cut = path.find_last_of("\\/");
    return std::string(cut == std::string_view::npos ? path : path.substr(cut + 1));
}

// Q6 [port]: a plots / plot file name is read under Data\XML as written (Thrawn's Revenge lists
// "Conquests\Historical\...\Story_Plots_X.xml"; the original loads those), then by its base name (the earlier port rule;
// the FoC names have no folder, so both give the same path). Returns the path read, "" when neither exists.
std::string read_xml_file(const FileProvider& files, std::string_view name, std::vector<uint8_t>& bytes) {
    std::string as_written(name);
    std::replace(as_written.begin(), as_written.end(), '/', '\\');
    for (const std::string& path : {"Data\\XML\\" + as_written, "Data\\XML\\" + base_name(name)})
        if (files.read(path, bytes)) return path;
    return {};
}

bool is_sep(char ch) { return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n' || ch == ','; }

// A list parameter (the generic list parser: whitespace and commas [M]), upper-cased.
std::vector<std::string> name_list(std::string_view s) {
    std::vector<std::string> out;
    while (!s.empty()) {
        while (!s.empty() && is_sep(s.front())) s.remove_prefix(1);
        size_t n = 0;
        while (n < s.size() && !is_sep(s[n])) ++n;
        if (n) out.push_back(upper(s.substr(0, n)));
        s.remove_prefix(n);
    }
    return out;
}
bool list_has(const std::vector<std::string>& list, std::string_view name) {
    for (const std::string& s : list)
        if (iequal(s, name)) return true;
    return false;
}

// Event parameter (from XML or Lua) against a pushed parameter (generic rule).
bool param_matches(const StoryParam& want, const StoryParam& got) {
    if (want.kind != StoryParam::Kind::Text) return got.kind == want.kind && got.id == want.id;
    if (iequal(trim(want.text), trim(got.text))) return true;
    double a = 0, b = 0;
    return parse_number(want.text, a) && parse_number(got.text, b) && a == b;
}

// Shared name -> int enum table (0x45c3f0; unknown -> 0). Numbers are accepted as themselves [port].
int32_t enum_value(std::string_view s) {
    static constexpr std::pair<std::string_view, int32_t> kNames[] = {
        // FILTER_NONE ... FILTER_FRIENDLY_AND_ENEMY = 0..6 (name order [M]: as listed in Story XML docs)
        {"FILTER_NONE", 0}, {"FILTER_FRIENDLY_ONLY", 1}, {"FILTER_NEUTRAL_ONLY", 2}, {"FILTER_ENEMY_ONLY", 3},
        {"FILTER_FRIENDLY_AND_NEUTRAL", 4}, {"FILTER_ENEMY_AND_NEUTRAL", 5}, {"FILTER_FRIENDLY_AND_ENEMY", 6},
        {"GREATER_THAN", 1}, {"LESS_THAN", 2}, {"EQUAL_TO", 3}, {"NOT_EQUAL_TO", 4},
        {"GREATER_THAN_EQUAL_TO", 5}, {"LESS_THAN_EQUAL_TO", 6}};
    s = trim(s);
    for (const auto& [n, v] : kNames)
        if (iequal(n, s)) return v;
    return parse_int(s, 0);
}

// FILTER semantics (0x455ce0). L1/S4 wave 2b (SCRIPT_GAPS.md §4.5): the comparison is by FACTION - the plot player's
// faction vs the object owner's (StoryModel::faction_of); without that hook, by player.
bool filter_ok(int32_t filter, bool friendly, bool neutral) {
    switch (filter) {
        case 0: return true;
        case 1: return friendly;
        case 2: return neutral;
        case 3: return !friendly && !neutral;
        case 4: return friendly || neutral;
        case 5: return !friendly;
        case 6: return !neutral;
        default: return true;
    }
}

// Comparison switch: 1 >, 2 <, 3 ==, 5 >=, 6 <=; 4 (NOT_EQUAL_TO) and anything else: no case.
bool compare(double a, int32_t op, double b) {
    switch (op) {
        case 1: return a > b;
        case 2: return a < b;
        case 3: return a == b;
        case 5: return a >= b;
        case 6: return a <= b;
        default: return false;
    }
}

// Corruption type keyword (the CRC-keyed story keyword map 0x140b30728, SCRIPT_GAPS.md §4.7; exact, not upper-cased):
// CORRUPTION_NONE -1, INTIMIDATION 0 .. BONDED_CITIZENS 7, CORRUPTION_ANY 8; unknown -> 0. Empty -> the ctor default 8.
int32_t corruption_keyword(std::string_view s) {
    static constexpr std::string_view kNames[] = {"INTIMIDATION",    "PIRACY",  "KIDNAPPING",   "RACKETEERING",
                                                  "CORRUPT_MILITIA", "BRIBERY", "BLACK_MARKET", "BONDED_CITIZENS"};
    s = trim(s);
    if (s.empty() || s == "CORRUPTION_ANY") return 8;
    if (s == "CORRUPTION_NONE") return -1;
    for (int32_t i = 0; i < 8; ++i)
        if (s == kNames[i]) return i;
    return 0;
}

bool in(std::string_view t, std::initializer_list<std::string_view> set) {
    for (const std::string_view s : set)
        if (t == s) return true;
    return false;
}

bool is_single_object_name(std::string_view t) {
    return in(t, {"STORY_CONQUER", "STORY_CONSTRUCT", "STORY_CAPTURE_HERO", "STORY_DEFEAT_HERO", "STORY_REINFORCE",
                  "STORY_SELECT_UNIT", "STORY_UNIT_ARRIVED", "STORY_FULL_STOP", "STORY_ATTACK_HARDPOINT",
                  "STORY_GARRISON_UNIT"});
}
bool is_enter_family(std::string_view t) {
    return in(t, {"STORY_ENTER", "STORY_LAND_ON", "STORY_PLANET_DESTROYED", "STORY_FLEET_BOUNCED", "STORY_INVASION_BOUNCED"});
}
// WIN/LOSE_BATTLES Param2, the location enum ("2 = EITHER", STORY.md §5) [inferred by name - shipped data
// writes SPACE, Land, BASE_SPACE]: empty / EITHER / 2 any battle; SPACE / LAND (GROUND) that kind
// (n.location 2 / 1); BASE_SPACE / BASE_LAND (BASE_GROUND) that kind with a base taking part; anything else
// (unknown name: the enum table gives 0) any battle. A notification without a location matches only "any".
bool battle_location_ok(std::string_view p, const StoryNotification& n) {
    const std::string s = upper(trim(p));
    if (s.empty() || s == "EITHER" || s == "2") return true;
    if (s == "SPACE") return n.location == 2;
    if (s == "LAND" || s == "GROUND") return n.location == 1;
    if (s == "BASE_SPACE") return n.location == 2 && n.base;
    if (s == "BASE_LAND" || s == "BASE_GROUND") return n.location == 1 && n.base;
    return true;
}

// WIN/LOSE_BATTLES Param3 (STORY.md §5: first entry FRIENDLY/ENEMY(_ONLY)_CONTAINS, then CLASS_* names) [inferred
// semantics]: *_CONTAINS = some unit of that side has a listed class; *_ONLY_CONTAINS = the side has units and
// every one has a listed class. CLASS_<X> = GameObjectCategoryType X through `mask` (CLASS_ prefix optional).
// Any other first entry (shipped data writes FILTER_FRIENDLY_ONLY: the dispatch is already player-scoped) or
// an empty Param3: no composition test.
bool battle_composition_ok(std::string_view p, const StoryNotification& n,
                           const std::function<uint64_t(std::string_view)>& mask) {
    const std::vector<std::string> list = name_list(p);
    if (list.empty()) return true;
    const std::string& op = list[0];
    const bool friendly = op == "FRIENDLY_CONTAINS" || op == "FRIENDLY_ONLY_CONTAINS";
    const bool enemy = op == "ENEMY_CONTAINS" || op == "ENEMY_ONLY_CONTAINS";
    if (!friendly && !enemy) return true;
    const bool only = op == "FRIENDLY_ONLY_CONTAINS" || op == "ENEMY_ONLY_CONTAINS";
    uint64_t classes = 0;
    for (size_t i = 1; i < list.size(); ++i) {
        std::string_view c = list[i];
        if (c.size() > 6 && c.substr(0, 6) == "CLASS_") c.remove_prefix(6);
        if (mask) classes |= mask(c);
    }
    const std::vector<uint64_t>& side = friendly ? n.friendly : n.enemy;
    if (only) {
        if (side.empty()) return false;
        for (const uint64_t m : side)
            if ((m & classes) == 0) return false;
        return true;
    }
    for (const uint64_t m : side)
        if ((m & classes) != 0) return true;
    return false;
}

bool is_string_family(std::string_view t) {
    return in(t, {"STORY_CLICK_GUI", "STORY_SPEECH_DONE", "STORY_GENERIC", "STORY_MISSION_LOST", "STORY_BUY_BLACK_MARKET",
                  "STORY_GALACTIC_SABOTAGE", "STORY_OPEN_CORRUPTION"});
}

void hash_param(StateHasher& h, const StoryParam& p) {
    h.u8(static_cast<uint8_t>(p.kind));
    h.str(p.text);
    h.i32(p.id);
}

// ---- the original's CRC-keyed hash table order (Â§4.2) ----
// Bucket of a key: Park-Miller of (crc ^ 0xDEADBEEF) & 0x7FFFFFFF ([M] multiplier 16807, the
// "minimal standard"), ANDed with the bucket mask.
uint32_t table_hash(uint32_t crc) {
    const uint64_t x = (crc ^ 0xDEADBEEFu) & 0x7FFFFFFFu;
    return static_cast<uint32_t>((x * 16807u) % 0x7FFFFFFFu);
}

// Item indices in the list order of a table built by inserting keys[i] in index order: 8 buckets,
// max load 1.0, growth to round_pow2(max(8, n, buckets x8 below 512 / x2 above)) BEFORE the insert that
// exceeds the load, rehash = stable regroup by bucket (MSVC _Forced_rehash), insert into an empty bucket
// = append, into a non-empty bucket = ascending key order inside the bucket. Equal keys: first wins.
std::vector<uint32_t> table_order(const std::vector<uint32_t>& keys, std::vector<bool>* inserted) {
    struct Node {
        uint32_t key, item;
    };
    std::vector<Node> list;
    size_t buckets = 8;
    auto bucket_of = [&buckets](uint32_t key) { return static_cast<size_t>(table_hash(key)) & (buckets - 1); };
    if (inserted) inserted->assign(keys.size(), false);
    for (uint32_t i = 0; i < keys.size(); ++i) {
        bool dup = false;
        for (const Node& n : list) dup = dup || n.key == keys[i];
        if (dup) continue;
        if (list.size() + 1 > buckets) {
            size_t nb = list.size() + 1 > 8 ? list.size() + 1 : 8;
            const size_t grown = buckets < 512 ? buckets * 8 : buckets * 2;
            if (grown > nb) nb = grown;
            size_t pow2 = 8;
            while (pow2 < nb) pow2 *= 2;
            buckets = pow2;
            std::vector<Node> regrouped;
            for (const Node& n : list) {
                const size_t b = bucket_of(n.key);
                size_t last = kNpos;
                for (size_t k = 0; k < regrouped.size(); ++k)
                    if (bucket_of(regrouped[k].key) == b) last = k;
                if (last == kNpos) regrouped.push_back(n);
                else regrouped.insert(regrouped.begin() + static_cast<std::ptrdiff_t>(last + 1), n);
            }
            list.swap(regrouped);
        }
        const size_t b = bucket_of(keys[i]);
        size_t first = kNpos, end = kNpos, pos = kNpos;
        for (size_t k = 0; k < list.size(); ++k)
            if (bucket_of(list[k].key) == b) {
                if (first == kNpos) first = k;
                end = k + 1;
                if (pos == kNpos && list[k].key > keys[i]) pos = k;
            }
        const Node node{keys[i], i};
        if (first == kNpos) list.push_back(node);
        else list.insert(list.begin() + static_cast<std::ptrdiff_t>(pos != kNpos ? pos : end), node);
        if (inserted) (*inserted)[i] = true;
    }
    std::vector<uint32_t> out;
    out.reserve(list.size());
    for (const Node& n : list) out.push_back(n.item);
    return out;
}

}  // namespace

// ------------------------------------------------------------------------------------------ loading

uint32_t StoryModel::add_plot(std::string_view plot_name, std::string_view xml, bool active, std::string_view lua_script,
                              std::string* err) {
    const data::XmlDocument doc = data::XmlDocument::parse(xml, std::string(plot_name));
    if (!doc.ok() || !iequal(doc.root().name(), "Story")) {
        if (err) *err = std::string(plot_name) + ": not a <Story> file";
        return kNone;
    }
    StoryPlot plot;
    plot.name = std::string(plot_name);
    plot.lua_script = std::string(trim(lua_script));
    plot.active = active;
    std::vector<std::vector<std::string>> prereq_text;  // per event: one string per <Prereq>
    for (const data::XmlNode ev : doc.root().children()) {
        if (!iequal(ev.name(), "Event")) continue;
        StoryEventDef def;
        def.name = std::string(trim(ev.attribute("Name")));
        std::vector<std::string> pre;
        bool timeout_set = false;
        for (const data::XmlNode c : ev.children()) {
            const std::string_view tag = c.name();
            const std::string_view text = c.text_trimmed();
            if (iequal(tag, "Event_Type")) {
                def.type = upper(text);
            } else if (iequal(tag, "Reward_Type")) {
                def.reward_type = upper(text);
            } else if (iequal(tag, "Prereq")) {
                pre.emplace_back(text);
            } else if (iequal(tag, "Branch")) {
                def.branch = std::string(text);
            } else if (iequal(tag, "Perpetual")) {
                def.perpetual = iequal(text, "true") || iequal(text, "yes") || text == "1";
            } else if (iequal(tag, "Multiplayer")) {
                def.multiplayer = iequal(text, "true") || iequal(text, "yes") || text == "1";
            } else if (iequal(tag, "Timeout")) {
                def.timeout = parse_float(text, 0.0);
                timeout_set = true;
            } else if (iequal(tag, "Inactive_Delay")) {
                def.inactive_delay = parse_float(text, 0.0);
            } else if (iequal(tag, "Story_Dialog")) {
                def.story_dialog = std::string(text);
            } else if (iequal(tag, "Story_Tag")) {
                def.story_tag = std::string(text);
            } else if (iequal(tag, "Story_Chapter")) {
                def.story_chapter = parse_int(text, -1);
            } else if (iequal(tag, "Story_Dialog_Popup")) {
                def.story_dialog_popup = iequal(text, "true") || iequal(text, "yes") || text == "1";
            } else if (iequal(tag, "Story_Dialog_SFX")) {  // STORY.md §1.6 (+0x2B9); no shipped event sets it
                def.story_dialog_sfx = iequal(text, "true") || iequal(text, "yes") || text == "1";
            } else if (iequal(tag, "Story_Var")) {  // +0x258: the printf argument of the chapter's TEXT lines
                def.story_var = std::string(text);
            } else if (iequal(tag, "Reward_Param_List")) {
                for (std::string& n : name_list(text)) def.reward_param_list.push_back(std::move(n));
            } else if (const uint32_t i = param_index(tag, "Event_Param")) {
                set_param(def.event_params, i, StoryParam::of_text(text));
            } else if (const uint32_t r = param_index(tag, "Reward_Param")) {
                set_param(def.reward_params, r, StoryParam::of_text(text));
            }
        }
        if (def.type == "STORY_SPEECH_DONE" && !timeout_set) def.timeout = kSpeechDoneTimeout;  // 0x4562a0
        if (def.name.empty()) {
            warnings_.push_back(plot.name + "(" + std::to_string(ev.line()) + "): event without a Name");
            continue;
        }
        def.has_prereq_tags = !pre.empty();
        request_dialog_load(def.story_dialog);  // GALACTIC_DIALOGS §2.8: the dialog script loads with its plot (0x4562a0)
        const std::string key = upper(def.name);
        if (!plot.by_name.emplace(key, static_cast<uint32_t>(plot.events.size())).second) {
            warnings_.push_back(plot.name + ": duplicate event " + def.name + " (only the first is in the table)");
            def.in_table = false;
        }
        plot.events.push_back(std::move(def));
        prereq_text.push_back(std::move(pre));
    }
    // Table order of the events (key = CRC-32 of the upper-cased name).
    std::vector<uint32_t> keys;
    keys.reserve(plot.events.size());
    for (const StoryEventDef& d : plot.events) {
        const std::string k = upper(d.name);
        keys.push_back(crc32(k.data(), k.size()));
    }
    plot.walk = table_order(keys, nullptr);
    std::vector<uint32_t> walk_in_table;
    for (const uint32_t e : plot.walk)
        if (plot.events[e].in_table) walk_in_table.push_back(e);
    plot.walk.swap(walk_in_table);
    // Prereq groups (Compute_Dependants 0x452d70): names resolve inside this plot; unknown names are
    // dropped from their group; dependants are appended in table-walk order.
    for (const uint32_t e : plot.walk) {
        StoryEventDef& def = plot.events[e];
        for (const std::string& group_text : prereq_text[e]) {
            std::vector<uint32_t> group;
            for (const std::string& name : name_list(group_text)) {
                const auto it = plot.by_name.find(name);
                if (it == plot.by_name.end()) {
                    def.unresolved_prereq = true;
                    warnings_.push_back(plot.name + ": event " + def.name + " has unknown prereq " + name);
                    continue;
                }
                group.push_back(it->second);
                std::vector<uint32_t>& deps = plot.events[it->second].dependants;
                bool have = false;
                for (const uint32_t d : deps) have = have || d == e;
                if (!have) deps.push_back(e);
            }
            def.prereqs.push_back(std::move(group));
        }
    }
    plot.state.assign(plot.events.size(), StoryEventState{});
    for (size_t e = 0; e < plot.events.size(); ++e) plot.state[e].armed = !plot.events[e].has_prereq_tags;  // Â§1.1
    plots_.push_back(std::move(plot));
    script_vms_.push_back(nullptr);
    build_plot_order();
    return static_cast<uint32_t>(plots_.size() - 1);
}

// q6 t03 (STORY.md §2.1 [H]): the plot table is a unique map keyed by the CRC-32 of the file name as written; a later
// plot of the same key takes the node of the first one (its place in the walk) and the earlier plot leaves the table.
// Plots a StoryTable marked replaced (a later plot set lists the same file) leave it too.
void StoryModel::build_plot_order() {
    std::vector<uint32_t> keys;
    keys.reserve(plots_.size());
    for (const StoryPlot& p : plots_) keys.push_back(crc32(p.name.data(), p.name.size()));  // name as written
    const std::vector<uint32_t> order = table_order(keys, nullptr);
    table_replaced_.resize(plots_.size(), 0);
    for (uint32_t p = 0; p < plots_.size(); ++p) plots_[p].in_table = table_replaced_[p] == 0;
    plot_walk_.clear();
    for (const uint32_t first : order) {
        uint32_t latest = first;  // the last plot of that key (table_order keeps the first one's place)
        for (uint32_t p = first + 1; p < plots_.size(); ++p)
            if (keys[p] == keys[first]) {
                plots_[latest].in_table = false;
                latest = p;
            }
        if (plots_[latest].in_table) plot_walk_.push_back(latest);
    }
}

void StoryModel::set_table_replaced(uint32_t plot, bool replaced) {
    table_replaced_.resize(plots_.size(), 0);
    if (plot < plots_.size()) table_replaced_[plot] = replaced ? 1 : 0;
}

bool StoryModel::load_plots(const FileProvider& files, std::string_view plots_file, std::string* err) {
    std::vector<uint8_t> bytes;
    const std::string path = read_xml_file(files, plots_file, bytes);
    if (path.empty()) {
        if (err) *err = "cannot read Data\\XML\\" + std::string(plots_file);
        return false;
    }
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, path);
    if (!doc.ok() || !iequal(doc.root().name(), "Story_Mode_Plots")) {
        if (err) *err = path + ": not a <Story_Mode_Plots> file";
        return false;
    }
    std::string pending_script;
    for (const data::XmlNode c : doc.root().children()) {
        const std::string_view tag = c.name();
        const std::string_view text = c.text_trimmed();
        if (iequal(tag, "Lua_Script")) {
            pending_script = std::string(text);
            continue;
        }
        if (text.empty()) continue;  // <Suspended_Plot /> placeholder creates nothing
        const bool active = tag == "Active_Plot";  // Â§2.1: exactly this tag; any other tag loads suspended
        std::vector<uint8_t> plot_bytes;
        if (read_xml_file(files, text, plot_bytes).empty()) {
            if (err) *err = "cannot read plot Data\\XML\\" + std::string(text) + " (listed in " + path + ")";
            return false;
        }
        std::string perr;
        const std::string_view xml(reinterpret_cast<const char*>(plot_bytes.data()), plot_bytes.size());
        if (add_plot(text, xml, active, pending_script, &perr) == kNone) {
            if (err) *err = perr;
            return false;
        }
        pending_script.clear();  // a Lua_Script pairs with the next plot only
    }
    return true;
}

// ------------------------------------------------------------------------------------------ lookup

// q6 t03 (STORY.md §2.1 / §2.4 [H]): one hash lookup by the CRC-32 of the name as passed - no upper-casing, no trim, no
// ".xml" added, the path included - so the name must match the Story_Plots text byte for byte. At most one plot of the
// table has that key.
uint32_t StoryModel::find_plot(std::string_view name) const {
    const uint32_t key = crc32(name.data(), name.size());
    for (const uint32_t p : plot_walk_)
        if (crc32(plots_[p].name.data(), plots_[p].name.size()) == key) return p;
    return kNone;
}

uint32_t StoryModel::find_event(uint32_t plot, std::string_view name) const {
    if (plot >= plots_.size()) return kNone;
    const auto it = plots_[plot].by_name.find(upper(trim(name)));
    return it == plots_[plot].by_name.end() ? kNone : it->second;
}

void StoryModel::set_plot_active(uint32_t plot, bool active) {
    if (plot < plots_.size()) plots_[plot].active = active;  // Â§2.2: the flag only
}

// Â§2.3: for every event: triggered = 0, vf4 twice (events with prereqs disarm, counters restored, ELAPSED /
// OBJECTIVE_TIMEOUT timers cleared); disabled flags untouched.
void StoryModel::reset_plot(uint32_t plot) {
    if (plot >= plots_.size()) return;
    for (uint32_t e = 0; e < plots_[plot].events.size(); ++e) {
        plots_[plot].state[e].fired = false;
        reset_event(plot, e);
    }
}

void StoryModel::set_script_vm(uint32_t plot, LuaVm* vm) {
    if (plot < script_vms_.size()) script_vms_[plot] = vm;
}

// ------------------------------------------------------------------------------------------ firing

bool StoryModel::gated(uint32_t p, uint32_t e) const {
    const StoryPlot& plot = plots_[p];
    const StoryEventState& st = plot.state[e];
    // Multiplayer gate (Â§4.3): single-player sessions pass every event.
    return plot.active && st.armed && !st.fired && st.enabled && plot.events[e].in_table;
}

bool StoryModel::prereqs_hold(uint32_t p, uint32_t e) const {
    const StoryPlot& plot = plots_[p];
    for (const std::vector<uint32_t>& group : plot.events[e].prereqs) {
        bool all = true;  // an empty group counts as satisfied
        for (const uint32_t i : group) all = all && plot.state[i].fired;
        if (all) return true;
    }
    return false;
}

// vf4: an event with prereq groups disarms (timeout cancelled); counters restored. ELAPSED / OBJECTIVE_TIMEOUT override
// it (0x45e550, q6 t03, STORY.md §2.3 / §4.7 [H]): the base reset, then accumulated = 0 and initialised = 0, so the timer
// restarts at its next evaluation (vf7 re-records LastTime only when prereqs arm the event again). Shared by Fire's
// Perpetual step, Lua Reset, RESET_EVENT and RESET_BRANCH.
void StoryModel::reset_event(uint32_t p, uint32_t e) {
    StoryEventState& st = plots_[p].state[e];
    const StoryEventDef& def = plots_[p].events[e];
    if (def.has_prereq_tags) {
        st.armed = false;
        st.timeout_at = -1.0;
    }
    st.countdowns.clear();
    st.count = 0;
    st.seen_level = -1;
    if (def.type == "STORY_ELAPSED" || def.type == "STORY_OBJECTIVE_TIMEOUT") {
        st.timer_acc = 0.0;
        st.timer_init = false;
    }
}

// Â§1.2: armed with activation time = now, Timeout registered; STORY_TRIGGER fires at once, ELAPSED /
// OBJECTIVE_TIMEOUT start their timers (vf7).
void StoryModel::arm(uint32_t p, uint32_t e) {
    StoryEventState& st = plots_[p].state[e];
    const StoryEventDef& def = plots_[p].events[e];
    st.armed = true;
    st.activation_time = time_;
    st.timeout_at = def.timeout > 0.0 ? time_ + def.timeout : -1.0;
    if (def.type == "STORY_TRIGGER") {
        fire(p, e);
    } else if (def.type == "STORY_ELAPSED" || def.type == "STORY_OBJECTIVE_TIMEOUT") {
        st.timer_init = true;
        st.timer_last = st.activation_time;
    }
}

void StoryModel::call_script(uint32_t p, const std::string& name) {
    StoryPlot& plot = plots_[p];
    if (plot.lua_script.empty()) return;
    LuaVm* vm = script_vms_[p];
    if (!vm) {
        plot.pending_script_triggers.push_back(name);
        return;
    }
    // q6 t11 (LUA_API.md "Binding error model across all bindings"): a binding error set the plot script's stop flag;
    // the script has ended, so its Story_Event_Trigger is not called any more (the event itself still fires).
    if (vm->exit_requested()) return;
    // Synchronous (0x52f920). A fresh Lua thread, so the call is safe while one of the VM's script
    // threads is running (Story_Event from a script).
    lua_State* L = vm->state();
    lua_State* T = lua_newthread(L);
    lua_pushstring(T, "Story_Event_Trigger");
    lua_rawget(T, LUA_GLOBALSINDEX);
    if (lua_type(T, -1) == LUA_TFUNCTION) {
        lua_pushlstring(T, name.data(), name.size());
        if (lua_pcall(T, 1, 0, 0) != 0) {
            const char* m = lua_tostring(T, -1);
            script_errors_.push_back("Story_Event_Trigger(" + name + "): " + (m ? m : "error"));
            lua_pop(T, 1);
        }
    } else {
        lua_pop(T, 1);
    }
    lua_pop(L, 1);  // the thread
}

// Fire (0x455db0), Â§1.5.
bool StoryModel::fire(uint32_t p, uint32_t e, bool forced) {
    if (fire_depth_ >= kMaxFireDepth) return false;
    {
        StoryEventState& st = plots_[p].state[e];
        const StoryEventDef& def = plots_[p].events[e];
        if (!st.enabled) return false;
        if (def.inactive_delay > 0.0 && !forced) {
            st.inactive_start = time_;
            return false;
        }
    }
    ++fire_depth_;
    const std::string name = plots_[p].events[e].name;
    StoryFired f;
    f.sequence = static_cast<uint32_t>(fired_.size());
    f.frame = frame_;
    f.plot = p;
    f.event = e;
    f.plot_name = plots_[p].name;
    f.event_name = name;
    f.event_type = plots_[p].events[e].type;
    f.reward_type = plots_[p].events[e].reward_type;
    f.reward_params = plots_[p].events[e].reward_params;
    fired_.push_back(std::move(f));
    call_script(p, name);
    {
        StoryEventState& st = plots_[p].state[e];
        st.timeout_at = -1.0;
        st.inactive_start = -1.0;
        st.fired = true;
        ++st.fire_count;
        st.last_fire_frame = frame_;
    }
    apply_reward(p, e);
    // Step 6 (STORY.md §1.6), only with a Story_Dialog: the tag (Story_Tag, else "<dialog>_<chapter>" + the auto-tag flag),
    // a holocron record that references this event's Add_Dialog_Text list, and the presentation record for the UI.
    if (const StoryEventDef& def = plots_[p].events[e]; !def.story_dialog.empty()) {
        StoryHolocronEntry h;
        h.dialog = def.story_dialog;
        h.var = def.story_var;
        h.chapter = def.story_chapter;
        h.auto_tag = def.story_tag.empty();
        h.tag = h.auto_tag ? def.story_dialog + "_" + std::to_string(def.story_chapter) : def.story_tag;
        h.frame = frame_;
        h.model = this;
        h.plot = p;
        h.event = e;
        StoryDialogEntry d{frame_, p, e, def.story_dialog, h.tag, def.story_chapter, def.story_dialog_popup,
                           def.story_dialog_sfx, 0, plots_[p].name};
        d.holocron_id = holocron_->add(std::move(h));
        dialog_log_.push_back(d);
        holocron_->push_log(std::move(d));
    }
    // Dependants (copy: rewards never change definitions, but stay safe).
    const std::vector<uint32_t> deps = plots_[p].events[e].dependants;
    for (const uint32_t d : deps) {
        const StoryEventState& ds = plots_[p].state[d];
        if (ds.armed || ds.fired || !prereqs_hold(p, d)) continue;
        arm(p, d);
    }
    if (plots_[p].events[e].perpetual) {
        StoryEventState& st = plots_[p].state[e];
        st.fired = false;
        reset_event(p, e);
        if (plots_[p].events[e].type != "STORY_TRIGGER") {
            st.armed = true;
            st.activation_time = time_;
        }
    }
    --fire_depth_;
    return true;
}

StoryObjective* StoryModel::find_objective(std::string_view text) {
    for (StoryObjective& o : objectives_)
        if (iequal(o.text, text)) return &o;
    return nullptr;
}

void StoryModel::add_objective(std::string_view text, bool from_lua) {
    if (StoryObjective* o = find_objective(text)) {
        o->state = StoryObjectiveState::Active;  // [inferred] re-adding re-activates, no duplicate
        return;
    }
    objectives_.push_back(StoryObjective{std::string(text), StoryObjectiveState::Active, from_lua});
}

// Reward application (0x456970), Â§6.
void StoryModel::apply_reward(uint32_t p, uint32_t e) {
    const std::string type = plots_[p].events[e].reward_type;
    if (type.empty()) return;
    const std::vector<StoryParam> rp = plots_[p].events[e].reward_params;
    const std::string p1(text_at(rp, 0)), p2(text_at(rp, 1)), p3(text_at(rp, 2));
    if (type == "SET_FLAG") {
        if (!p2.empty()) (*flags_)[upper(p1)] = parse_int(p2, 0);
    } else if (type == "INCREMENT_FLAG") {
        const auto it = flags_->find(upper(p1));
        if (!p2.empty() && it != flags_->end()) it->second += parse_int(p2, 0);  // undefined: nothing
    } else if (type == "ADD_OBJECTIVE") {
        add_objective(p1, false);
    } else if (type == "OBJECTIVE_COMPLETE" || type == "OBJECTIVE_FAILED") {
        if (StoryObjective* o = find_objective(p1))
            o->state = type == "OBJECTIVE_COMPLETE" ? StoryObjectiveState::Complete : StoryObjectiveState::Failed;
    } else if (type == "UPDATE_OBJECTIVE") {
        if (StoryObjective* o = find_objective(p1)) o->text = p2;
    } else if (type == "REMOVE_OBJECTIVE") {
        for (size_t i = 0; i < objectives_.size(); ++i)
            if (iequal(objectives_[i].text, p1)) {
                objectives_.erase(objectives_.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
    } else if (type == "REMOVE_ALL_OBJECTIVES") {
        objectives_.clear();
    } else if (type == "TRIGGER_EVENT") {
        // Every plot of the mode's table, every player's, active or not (§2.4): through the StoryTable when there is one.
        if (table_) table_->trigger_event_by_name(p1);
        else trigger_event_by_name(p1);
    } else if (type == "RESET_EVENT") {
        std::vector<std::string> names{p1};
        for (const std::string& n : plots_[p].events[e].reward_param_list) names.push_back(n);
        for (const std::string& n : names) {
            const uint32_t ev = find_event(p, n);
            if (ev == kNone) continue;
            plots_[p].state[ev].fired = false;
            reset_event(p, ev);
        }
    } else if (type == "RESET_BRANCH") {
        if (reset_depth_ >= kMaxResetDepth || p1.empty()) return;  // "Event resets itself over and over forever"
        ++reset_depth_;
        const std::vector<uint32_t> walk = plots_[p].walk;
        for (const uint32_t ev : walk)
            if (plots_[p].events[ev].branch == p1) {  // exact, case-sensitive
                plots_[p].state[ev].fired = false;
                reset_event(p, ev);
            }
        for (const uint32_t ev : walk) {
            if (plots_[p].events[ev].branch != p1) continue;
            const StoryEventState& st = plots_[p].state[ev];
            if (st.armed || st.fired || !plots_[p].events[ev].has_prereq_tags || !prereqs_hold(p, ev)) continue;
            arm(p, ev);
        }
        if (!p2.empty()) {
            const uint32_t ev = find_event(p, p2);
            if (ev != kNone) fire(p, ev);
        }
        --reset_depth_;
    } else if (type == "DISABLE_BRANCH") {
        if (p1.empty() || p2.empty()) return;  // logged and ignored by the original
        const bool disable = parse_int(p2, 0) != 0;
        for (size_t i = 0; i < plots_[p].events.size(); ++i)
            if (plots_[p].events[i].branch == p1) plots_[p].state[i].enabled = !disable;  // [M] exact compare
    } else if (type == "DISABLE_STORY_EVENT") {
        if (p1.empty() || p2.empty()) return;
        const bool disable = parse_int(p2, 0) != 0;
        const bool all = parse_int(p3, 0) != 0;
        if (all && table_) {
            table_->set_event_enabled(p1, !disable);  // every plot of the table (q6 t03, §6.2)
        } else {
            for (uint32_t q = 0; q < plots_.size(); ++q) {
                if (!all && q != p) continue;
                if (all && !plots_[q].in_table) continue;
                const uint32_t ev = find_event(q, p1);
                if (ev != kNone) plots_[q].state[ev].enabled = !disable;
            }
        }
    } else if (type == "STORY_ELEMENT") {
        // Any player's plot of the mode's table, active or suspended; silent when not found (§2.4).
        if (table_) {
            const StoryTable::PlotRef q = table_->find_plot(p1 + ".xml");
            if (q.valid()) table_->model(q.set)->set_plot_active(q.plot, true);
        } else if (const uint32_t q = find_plot(p1 + ".xml"); q != kNone) {
            set_plot_active(q, true);
        }
    } else if (type == "REMOVE_STORY_GOAL") {  // 0x2C (0x47eea0): the newest holocron record with tag P1 goes
        holocron_->remove_tag(p1);
    } else if (type == "STORY_GOAL_COMPLETED") {  // 0x38 (0x47f5b0): ... is marked completed (the Completed tab)
        holocron_->complete_tag(p1);
    } else if (on_reward) {
        on_reward(p, e, type, rp);
    }
}

bool StoryModel::tick_timer(StoryEventState& st, double threshold) {
    if (!st.timer_init) {
        // First evaluation (prereq-less): LastTime = now, initialised, and return without the accumulator test (0x454980,
        // A2I_T01_FRAME_ORDER.md §6 [H]). So a prereq-less ELAPSED 0 fires at its second T9 (tick 2 for an Active_Plot),
        // not its first. A prereq-armed timer was initialised by arm() (vf7) and never takes this branch.
        st.timer_init = true;
        st.timer_last = time_;
        return false;
    } else if (time_ < st.timer_last) {
        st.timer_last = time_;  // time went backwards: reset, add nothing
    } else {
        if (!paused_) st.timer_acc += time_ - st.timer_last;
        st.timer_last = time_;
    }
    return st.timer_acc >= threshold;
}

// FILTER "friendly" side (wave 2b, SCRIPT_GAPS.md §4.5): same faction as the plot player (faction_of), else same player.
bool StoryModel::same_side(int32_t owner) const {
    if (owner < 0 || player_ < 0) return false;
    if (faction_of) {
        const int32_t a = faction_of(owner), b = faction_of(player_);
        if (a >= 0 && b >= 0) return a == b;
    }
    return owner == player_;
}

// vf3 match rules (Â§5). True = fire.
bool StoryModel::evaluate(uint32_t p, uint32_t e, std::string_view type, const StoryNotification& n) {
    const StoryEventDef& def = plots_[p].events[e];
    StoryEventState& st = plots_[p].state[e];
    const auto& ep = def.event_params;
    // Player (or host-made object) parameter values [port]: compared by id through the generic rule. Lua's
    // Set_Event_Parameter stores objects and types as their type name (bind_story.cpp, story t02), so those take the
    // type's own rule below like XML text.
    bool lua_values = false;
    for (const StoryParam& q : ep) lua_values = lua_values || q.kind != StoryParam::Kind::Text;
    if (lua_values) {
        for (size_t i = 0; i < ep.size(); ++i) {
            if (ep[i].empty()) continue;
            if (i >= n.params.size() || !param_matches(ep[i], n.params[i])) return false;
        }
        return true;
    }
    if (is_single_object_name(type)) {
        const std::vector<std::string> names = name_list(text_at(ep, 0));
        size_t idx = names.size();
        for (size_t i = 0; i < names.size(); ++i)
            if (iequal(names[i], n.name)) idx = i;
        if (idx == names.size()) return false;
        if (!n.has_object) return true;
        const std::string_view f = text_at(ep, 2);
        const int32_t filter = f.empty() ? 1 : enum_value(f);  // default FILTER_FRIENDLY_ONLY
        if (!filter_ok(filter, same_side(n.owner), n.owner_neutral)) return false;
        if (st.countdowns.size() != names.size()) {
            const std::vector<std::string> counts = name_list(text_at(ep, 1));
            st.countdowns.assign(names.size(), 1);
            for (size_t i = 0; i < counts.size() && i < names.size(); ++i) st.countdowns[i] = parse_int(counts[i], 1);
        }
        return --st.countdowns[idx] <= 0;
    }
    if (type == "STORY_CONQUER_COUNT") return ++st.count >= parse_int(text_at(ep, 0), 1);
    if (type == "STORY_CONSTRUCT_LEVEL") {
        if (!list_has(name_list(text_at(ep, 0)), n.planet)) return false;
        const std::string_view cat = text_at(ep, 2);
        if (cat == "0" && n.category != 0) return false;
        if (cat == "1" && n.category != 1) return false;
        return n.value >= static_cast<double>(parse_int(text_at(ep, 1), 0));
    }
    if (is_enter_family(type)) {
        const std::vector<std::string> planets = name_list(text_at(ep, 0));
        if (!planets.empty() && !list_has(planets, n.planet)) return false;
        const std::string_view f = text_at(ep, 1);
        if (!filter_ok(f.empty() ? 0 : enum_value(f), same_side(n.owner), n.owner_neutral)) return false;  // default NONE
        for (const std::string& u : name_list(text_at(ep, 2)))
            if (!list_has(n.units, u)) return false;  // all of Param3
        const std::vector<std::string> any = name_list(text_at(ep, 3));
        if (!any.empty()) {
            bool hit = false;
            for (const std::string& u : any) hit = hit || list_has(n.units, u);
            if (!hit) return false;
        }
        return true;  // Param5-7 [L] not modelled
    }
    if (type == "STORY_ACCUMULATE") {
        const int32_t op = enum_value(text_at(ep, 1));
        return compare(n.value, op == 0 ? 1 : op, parse_float(text_at(ep, 0), 0.0));
    }
    if (type == "STORY_WIN_BATTLES" || type == "STORY_LOSE_BATTLES") {
        // A battle matches Param2 (location) and Param3 (composition); a matching battle counts even when
        // the planet (Param4) does not match.
        if (!battle_location_ok(text_at(ep, 1), n) || !battle_composition_ok(text_at(ep, 2), n, category_mask)) return false;
        ++st.count;
        const std::vector<std::string> planets = name_list(text_at(ep, 3));
        return st.count >= parse_int(text_at(ep, 0), 1) && (planets.empty() || list_has(planets, n.planet));
    }
    // L1/S4 wave 2b: the original vf3 bodies (docs/re/SCRIPT_GAPS.md §4; replaces the wave-2a [inferred] rules).
    // [port] every evaluation fires at most once (the original calls Fire inside its loops, so a list that matches
    // twice fires twice; no shipped event has such a list).
    //   SPACE/LAND_TACTICAL (§4.1, 0x455710): Param1 = Story_Plots file loaded by the host for the plot's player;
    //     Param2 = planet names: empty, or no battle planet (n.planet "") -> fire; else fire when one names it.
    if (type == "STORY_SPACE_TACTICAL" || type == "STORY_LAND_TACTICAL") {
        const std::vector<std::string> planets = name_list(text_at(ep, 1));
        return planets.empty() || n.planet.empty() || list_has(planets, n.planet);
    }
    //   VICTORY (§4.2): Param1 = ONE faction name resolved at parse time; fire iff the winner's faction (n.name) is
    //     it. An empty Param1 is never parsed: never fires (likewise an unknown faction - it never equals a winner).
    if (type == "STORY_VICTORY") {
        const std::string_view faction = text_at(ep, 0);
        return !faction.empty() && iequal(faction, n.name);
    }
    //   COMMAND_UNIT (§4.3, 0x454190): Param1 = unit type names (one must equal n.name), then Param2 empty or
    //     Param3 == 0 -> fire; else fire when some object of a Param2 type is within (3D, strictly) Param3 of the
    //     order's destination (n.has_position; objects_within, the mode's objects). Without a destination / hook:
    //     the host-computed n.target / n.distance stand in [port].
    if (type == "STORY_COMMAND_UNIT") {
        if (!list_has(name_list(text_at(ep, 0)), n.name)) return false;
        const std::vector<std::string> targets = name_list(text_at(ep, 1));
        const double range = parse_float(text_at(ep, 2), 0.0);
        if (targets.empty() || range == 0.0) return true;
        if (n.has_position && objects_within) {
            for (const std::string& t : targets)
                if (objects_within(t, n.x, n.y, n.z, range)) return true;
            return false;
        }
        return list_has(targets, n.target) && n.distance >= 0.0 && n.distance < range;
    }
    //   GUARD_UNIT (§4.4, 0x454e70): entries pair by index - Param1[i] == the guarding type and Param2[i] == the
    //     guarded type; an empty Param2 never fires, i past the end of Param2 never matches.
    if (type == "STORY_GUARD_UNIT") {
        const std::vector<std::string> units = name_list(text_at(ep, 0)), guarded = name_list(text_at(ep, 1));
        for (size_t i = 0; i < units.size(); ++i)
            if (iequal(units[i], n.name) && i < guarded.size() && iequal(guarded[i], n.target)) return true;
        return false;
    }
    // S4 (docs/re/L1_S4_REMAINING.md §4.2, 0x4540b0): CAPTURE_STRUCTURE fires when the structure's type name is in Param1
    //   AND the new owner's faction (n.faction) is the Param2 faction (resolved at parse time; empty Param1 or Param2 ->
    //   never). No filter, no counter.
    if (type == "STORY_CAPTURE_STRUCTURE") {
        const std::string_view faction = text_at(ep, 1);
        return !faction.empty() && iequal(faction, n.faction) && list_has(name_list(text_at(ep, 0)), n.name);
    }
    //   LOAD_TACTICAL_MAP (§4.4, 0x455130): data2 (n.value: 0 land, 1 space) == the Param3 keyword (GROUND / LAND 0, SPACE
    //   1, EITHER 2 [M: never equal], numbers as themselves, unknown / absent 0) and the battle planet's type name in
    //   Param1; then Param2 empty -> fire, else entry i fires when load_map_hero(type, i) (the original indexes the
    //   instance list by the Param2 position; i past the instances never matches).
    if (type == "STORY_LOAD_TACTICAL_MAP") {
        const std::string_view k = trim(text_at(ep, 2));
        const int32_t kind = iequal(k, "SPACE") ? 1 : iequal(k, "EITHER") ? 2 : (iequal(k, "GROUND") || iequal(k, "LAND")) ? 0 : parse_int(k, 0);
        if (kind != static_cast<int32_t>(n.value) || !list_has(name_list(text_at(ep, 0)), n.planet)) return false;
        const std::vector<std::string> heroes = name_list(text_at(ep, 1));
        if (heroes.empty()) return true;
        for (size_t i = 0; i < heroes.size(); ++i)
            if (load_map_hero && load_map_hero(heroes[i], i)) return true;
        return false;
    }
    if (type == "STORY_TECH_LEVEL" || type == "STORY_BEGIN_ERA") {
        const int32_t want = parse_int(text_at(ep, 0), 0);
        const auto level = static_cast<int32_t>(n.value);
        if (want != -1) return level >= want;
        if (st.seen_level < 0) {
            st.seen_level = level;  // first observation [M]
            return false;
        }
        return level != st.seen_level;
    }
    if (type == "STORY_AI_NOTIFICATION") {
        if (!list_has(name_list(text_at(ep, 1)), n.name)) return false;
        const std::vector<std::string> objects = name_list(text_at(ep, 0));
        return n.has_object ? list_has(objects, n.params.empty() ? std::string_view() : std::string_view(n.params[0].text))
                            : objects.empty();
    }
    if (type == "STORY_DESTROY" || type == "STORY_TACTICAL_DESTROY") {
        // Wave 2b, StoryEventDestroyClass (§4.5, 0x454780): Param1 = destroyed type names, each with a counter (1, or
        // Param3[i] - atoi per entry; the counters grow to Param3's length); Param2 = a second name list; Param4 =
        // Event_Filter (default FILTER_NONE). i = the first Param1 entry equal to the dead type (none: nothing).
        //   Param2 empty: counter[i] -= 1, fire at <= 0, no filter.
        //   Param2 non-empty: the first j with Param2[j] == data2's type name and the filter passing on data2:
        //     counter[j] -= 1 (the Param2 index), fire at <= 0. data2 = the planet for DESTROY (n.planet, filter on
        //     its owner), null for TACTICAL_DESTROY - which therefore never fires with a Param2.
        const std::vector<std::string> names = name_list(text_at(ep, 0));
        size_t i = names.size();
        for (size_t k = 0; k < names.size() && i == names.size(); ++k)
            if (iequal(names[k], n.name)) i = k;
        if (i == names.size()) return false;
        if (st.countdowns.empty()) {  // initialised from the parameters (vf4 Reset restores them: cleared)
            const std::vector<std::string> counts = name_list(text_at(ep, 2));
            st.countdowns.assign(std::max(names.size(), counts.size()), 1);
            for (size_t k = 0; k < counts.size(); ++k) st.countdowns[k] = parse_int(counts[k], 0);
        }
        const std::vector<std::string> second = name_list(text_at(ep, 1));
        if (second.empty()) return --st.countdowns[i] <= 0;
        const bool data2 = type == "STORY_DESTROY" && n.has_planet;
        if (!data2) return false;
        const std::string_view f = text_at(ep, 3);
        const int32_t filter = f.empty() ? 0 : enum_value(f);
        for (size_t j = 0; j < second.size(); ++j) {
            if (!iequal(second[j], n.planet) || !filter_ok(filter, same_side(n.planet_owner), n.planet_owner_neutral)) continue;
            if (j >= st.countdowns.size()) return false;  // past the counters (undefined in the original)
            return --st.countdowns[j] <= 0;
        }
        return false;
    }
    if (type == "STORY_MOVE" || type == "STORY_DEPLOY") {  // [port] Param1 unit in the fleet, Param2 planet
        const std::vector<std::string> units = name_list(text_at(ep, 0));
        bool hit = units.empty();
        for (const std::string& u : units) hit = hit || list_has(n.units, u) || iequal(u, n.name);
        const std::vector<std::string> planets = name_list(text_at(ep, 1));
        return hit && (planets.empty() || list_has(planets, n.planet));
    }
    if (type == "STORY_CORRUPTION_CHANGED" || type == "STORY_CORRUPTION_TACTICAL_COMPLETE" ||
        type == "STORY_CORRUPTION_TACTICAL_FAILED") {
        // Wave 2b, StoryEventCorruptionLevelClass (§4.7, 0x4544e0): Param1 planet names (empty never fires), Param2
        // the corruption keyword (default 8 = any). n.value = the corruption type (-1 none).
        const int32_t want = corruption_keyword(text_at(ep, 1));
        const auto got = static_cast<int32_t>(n.value);
        if (!(got == want || (want == 8 && got != -1))) return false;
        return list_has(name_list(text_at(ep, 0)), n.planet);
    }
    if (is_string_family(type) && n.params.empty()) {  // [port] Param1 empty or names the string
        const std::vector<std::string> names = name_list(text_at(ep, 0));
        return names.empty() || list_has(names, n.name);
    }
    // Generic rule: every non-empty event parameter equals the pushed one at the same index.
    for (size_t i = 0; i < ep.size(); ++i) {
        if (ep[i].empty()) continue;
        if (i >= n.params.size() || !param_matches(ep[i], n.params[i])) return false;
    }
    return true;
}

namespace {
// The dispatched type, upper-cased; "" for the types no call site dispatches.
std::string dispatched_type(std::string_view type_in) {
    std::string type = upper(trim(type_in));
    if (type == "STORY_TRIGGER") return {};  // nobody dispatches STORY_TRIGGER (Â§1.3)
    // L1/S4 wave 2b (SCRIPT_GAPS.md §4.6): no call site dispatches STORY_DIFFICULTY_LEVEL in 1.121 - parsed, never
    // evaluated.
    if (type == "STORY_DIFFICULTY_LEVEL") return {};
    // S4 (docs/re/L1_S4_REMAINING.md §4.3): no call site dispatches STORY_CAPTURE_HERO (0x11) in 1.121 either.
    if (type == "STORY_CAPTURE_HERO") return {};
    return type;
}

StoryNotification ai_notification(std::string_view name, std::string_view object_type, bool has_object) {
    StoryNotification n;
    n.name = std::string(trim(name));
    n.has_object = has_object;
    if (has_object) n.params.push_back(StoryParam::of_text(object_type));
    return n;
}
}  // namespace

void StoryModel::dispatch_in(uint32_t p, const std::string& type, const StoryNotification& n) {
    if (p >= plots_.size() || !plots_[p].active || !plots_[p].in_table) return;
    const std::vector<uint32_t> walk = plots_[p].walk;
    for (const uint32_t e : walk) {
        if (plots_[p].events[e].type != type || !gated(p, e)) continue;
        if (evaluate(p, e, type, n)) fire(p, e);
    }
}

uint32_t StoryModel::dispatch(std::string_view type_in, const StoryNotification& n) {
    const std::string type = dispatched_type(type_in);
    if (type.empty()) return 0;
    const size_t before = fired_.size();
    for (const uint32_t p : plot_walk_) dispatch_in(p, type, n);
    return static_cast<uint32_t>(fired_.size() - before);
}

uint32_t StoryModel::dispatch_plot(uint32_t plot, std::string_view type_in, const StoryNotification& n) {
    const std::string type = dispatched_type(type_in);
    if (type.empty()) return 0;
    const size_t before = fired_.size();
    dispatch_in(plot, type, n);
    return static_cast<uint32_t>(fired_.size() - before);
}

uint32_t StoryModel::story_event(std::string_view name, std::string_view object_type, bool has_object) {
    return dispatch("STORY_AI_NOTIFICATION", ai_notification(name, object_type, has_object));
}

uint32_t StoryModel::push_trigger(std::string_view type, std::span<const StoryParam> params) {
    StoryNotification n;
    n.params.assign(params.begin(), params.end());
    if (!params.empty()) n.name = params[0].text;
    // Modelled single-name / enter types read the first parameter as the dispatched name / planet.
    if (!params.empty()) n.planet = params[0].text;
    return dispatch(type, n);
}

bool StoryModel::trigger_event(uint32_t plot, uint32_t event) {
    if (plot >= plots_.size() || event >= plots_[plot].events.size()) return false;
    return fire(plot, event);
}

uint32_t StoryModel::trigger_event_by_name(std::string_view name) {
    const size_t before = fired_.size();
    for (const uint32_t p : plot_walk_) {
        const uint32_t e = find_event(p, name);
        if (e != kNone) fire(p, e);  // active or suspended, no armed / fired / prereq check (Â§6.2)
    }
    return static_cast<uint32_t>(fired_.size() - before);
}

// ------------------------------------------------------------------------------------------ StoryTable (q6 t03)

void StoryTable::clear() {
    for (StoryModel* m : models_) {
        if (!m) continue;
        m->table_ = nullptr;
        for (uint32_t p = 0; p < m->plot_count(); ++p) m->set_table_replaced(p, false);
        m->build_plot_order();
    }
    models_.clear();
    order_.clear();
}

// One node per key (CRC-32 of the plot file name as written), in the original's table order of the keys inserted in
// load order (§4.2 emulation, the same as one model's plot_order()); the node holds the LAST plot of that key (§2.1
// unique map: the later-loaded plot replaces the earlier one, which leaves its model's walks and lookups).
void StoryTable::assign(std::vector<StoryModel*> models) {
    clear();
    models_ = std::move(models);
    std::vector<uint32_t> keys;
    std::vector<PlotRef> refs;
    for (uint32_t s = 0; s < models_.size(); ++s) {
        if (!models_[s]) continue;
        for (uint32_t p = 0; p < models_[s]->plot_count(); ++p) {
            const std::string& name = models_[s]->plot(p).name;
            keys.push_back(crc32(name.data(), name.size()));
            refs.push_back(PlotRef{s, p});
        }
    }
    std::vector<uint8_t> replaced(keys.size(), 0);
    for (const uint32_t first : table_order(keys, nullptr)) {
        uint32_t latest = first;
        for (uint32_t i = first + 1; i < keys.size(); ++i)
            if (keys[i] == keys[first]) {
                replaced[latest] = 1;
                latest = i;
            }
        order_.push_back(refs[latest]);
    }
    for (size_t i = 0; i < refs.size(); ++i) models_[refs[i].set]->set_table_replaced(refs[i].plot, replaced[i] != 0);
    for (StoryModel* m : models_)
        if (m) {
            m->table_ = this;
            m->build_plot_order();
        }
}

uint32_t StoryTable::set_of(const StoryModel* m) const {
    for (uint32_t s = 0; s < models_.size(); ++s)
        if (models_[s] == m) return s;
    return kNone;
}

StoryTable::PlotRef StoryTable::find_plot(std::string_view name) const {
    const uint32_t key = crc32(name.data(), name.size());  // as passed: case and path kept (§2.1)
    for (const PlotRef& r : order_) {
        const std::string& n = models_[r.set]->plot(r.plot).name;
        if (crc32(n.data(), n.size()) == key) return r;
    }
    return {};
}

size_t StoryTable::fired_total() const {
    size_t n = 0;
    for (const StoryModel* m : models_)
        if (m) n += m->fired().size();
    return n;
}

uint32_t StoryTable::trigger_event_by_name(std::string_view name) {
    const size_t before = fired_total();
    for (const PlotRef& r : order_) {
        StoryModel& m = *models_[r.set];
        const uint32_t e = m.find_event(r.plot, name);
        if (e != kNone) m.trigger_event(r.plot, e);  // every plot that has it: no first-match stop (§2.4)
    }
    return static_cast<uint32_t>(fired_total() - before);
}

uint32_t StoryTable::story_event(std::string_view name, std::string_view object_type, bool has_object) {
    static const std::string kType = "STORY_AI_NOTIFICATION";
    const StoryNotification n = ai_notification(name, object_type, has_object);
    const size_t before = fired_total();
    for (const PlotRef& r : order_) models_[r.set]->dispatch_in(r.plot, kType, n);  // active plots only
    return static_cast<uint32_t>(fired_total() - before);
}

void StoryTable::set_event_enabled(std::string_view name, bool enabled) {
    for (const PlotRef& r : order_) {
        StoryModel& m = *models_[r.set];
        const uint32_t e = m.find_event(r.plot, name);
        if (e != kNone) m.plot(r.plot).state[e].enabled = enabled;
    }
}

uint32_t StoryModel::poll(std::string_view type_in, const std::function<bool(const StoryEventDef&)>& match) {
    const std::string type = upper(trim(type_in));
    const size_t before = fired_.size();
    for (const uint32_t p : plot_walk_) {
        if (!plots_[p].active) continue;
        const std::vector<uint32_t> walk = plots_[p].walk;
        for (const uint32_t e : walk) {
            if (plots_[p].events[e].type != type || !gated(p, e)) continue;
            if (match(plots_[p].events[e])) fire(p, e);
        }
    }
    return static_cast<uint32_t>(fired_.size() - before);
}

// ------------------------------------------------------------------------------------------ T9

void StoryModel::set_clock(uint32_t frame, double seconds, bool paused) {
    frame_ = frame;
    time_ = seconds;
    paused_ = paused;
}

void StoryModel::service_elapsed() {
    for (const uint32_t p : plot_walk_) {
        if (!plots_[p].active) continue;
        const std::vector<uint32_t> walk = plots_[p].walk;
        for (const uint32_t e : walk) {
            if (plots_[p].events[e].type != "STORY_ELAPSED" || !gated(p, e)) continue;
            if (tick_timer(plots_[p].state[e], parse_float(text_at(plots_[p].events[e].event_params, 0), 0.0))) fire(p, e);
        }
    }
}

void StoryModel::service_plot(uint32_t p) {
    if (p >= plots_.size() || !plots_[p].active) return;
    const std::vector<uint32_t> walk = plots_[p].walk;
    // Inactivity timers (0x52e7b0, Â§4.4).
    for (const uint32_t e : walk) {
        const StoryEventDef& def = plots_[p].events[e];
        if (def.inactive_delay <= 0.0 || !gated(p, e)) continue;
        StoryEventState& st = plots_[p].state[e];
        if (st.inactive_start < 0.0) st.inactive_start = time_;
        else if (time_ - st.inactive_start >= def.inactive_delay) fire(p, e, true);
    }
    // STORY_FLAG poll (0x52e730).
    for (const uint32_t e : walk) {
        const StoryEventDef& def = plots_[p].events[e];
        if (def.type != "STORY_FLAG" || !gated(p, e)) continue;
        const std::string_view op_text = text_at(def.event_params, 2);
        const int32_t op = op_text.empty() ? 3 : enum_value(op_text);  // ctor default EQUAL_TO
        const double value = static_cast<double>(parse_int(text_at(def.event_params, 1), 0));
        for (const std::string& name : name_list(text_at(def.event_params, 0))) {
            const auto it = flags_->find(name);
            if (it == flags_->end()) continue;  // undefined flag: skipped
            if (compare(static_cast<double>(it->second), op, value)) {
                fire(p, e);
                break;
            }
        }
    }
    // Timeouts [L]: an expired Timeout fires the event (STORY.md Â§7.1 open).
    for (const uint32_t e : walk) {
        if (!gated(p, e)) continue;
        const StoryEventState& st = plots_[p].state[e];
        if (st.timeout_at >= 0.0 && time_ >= st.timeout_at) fire(p, e);
    }
}

void StoryModel::service_objective_timeouts() {
    for (const uint32_t p : plot_walk_) {
        if (!plots_[p].active) continue;
        const std::vector<uint32_t> walk = plots_[p].walk;
        for (const uint32_t e : walk) {
            if (plots_[p].events[e].type != "STORY_OBJECTIVE_TIMEOUT" || !gated(p, e)) continue;
            if (tick_timer(plots_[p].state[e], parse_float(text_at(plots_[p].events[e].event_params, 0), 0.0))) fire(p, e);
        }
    }
}

void StoryModel::update(uint32_t frame, double seconds, bool paused) {
    set_clock(frame, seconds, paused);
    service_elapsed();
    for (const uint32_t p : plot_walk_) service_plot(p);
    service_objective_timeouts();
}

// ------------------------------------------------------------------------------------------ state

int32_t StoryModel::flag(std::string_view name) const {
    const auto it = flags_->find(upper(trim(name)));
    return it == flags_->end() ? kUndefinedFlag : it->second;
}

bool StoryModel::has_flag(std::string_view name) const { return flags_->count(upper(trim(name))) != 0; }

void StoryModel::set_flag(std::string_view name, int32_t value) { (*flags_)[upper(trim(name))] = value; }

void StoryModel::add_blip(std::string_view name, int32_t object_id) {
    for (StoryBlip& b : blips_)
        if (iequal(b.name, name)) {
            b.object_id = object_id;
            return;
        }
    blips_.push_back(StoryBlip{std::string(name), object_id});
}

bool StoryModel::remove_blip(std::string_view name) {
    for (size_t i = 0; i < blips_.size(); ++i)
        if (iequal(blips_[i].name, name)) {
            blips_.erase(blips_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    return false;
}

void StoryModel::add_highlight(std::string_view name, int32_t object_id) {
    for (StoryHighlight& h : highlights_)
        if (iequal(h.name, name)) {
            h.object_id = object_id;
            return;
        }
    highlights_.push_back(StoryHighlight{std::string(name), object_id});
}

bool StoryModel::remove_highlight(std::string_view name) {
    for (size_t i = 0; i < highlights_.size(); ++i)
        if (iequal(highlights_[i].name, name)) {
            highlights_.erase(highlights_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    return false;
}

namespace {
std::string ai_flag_key(int32_t player, std::string_view name, int32_t location) {
    return std::to_string(player) + "|" + upper(trim(name)) + "|" + std::to_string(location);
}
}  // namespace

void StoryModel::raise_ai_flag(int32_t player, std::string_view name, int32_t location_id) {
    ++ai_flags_[ai_flag_key(player, name, location_id)];
}

bool StoryModel::check_ai_flag(int32_t player, std::string_view name, int32_t location_id, bool reset) {
    const auto it = ai_flags_.find(ai_flag_key(player, name, location_id));
    if (it == ai_flags_.end()) return false;
    if (reset) ai_flags_.erase(it);
    return true;
}

// ------------------------------------------------------------------------------------------ Lua / CRC

size_t StoryModel::deliver_script_triggers(uint32_t plot, LuaVm& vm, std::string* err) {
    if (plot >= plots_.size()) return 0;
    std::vector<std::string> queue;
    queue.swap(plots_[plot].pending_script_triggers);  // triggers raised while delivering wait
    lua_State* L = vm.state();
    size_t n = 0;
    for (const std::string& name : queue) {
        if (vm.exit_requested()) break;  // q6 t11: a stopped plot script gets no more triggers (as call_script)
        lua_pushstring(L, "Story_Event_Trigger");
        lua_rawget(L, LUA_GLOBALSINDEX);
        if (lua_type(L, -1) != LUA_TFUNCTION) {
            lua_pop(L, 1);
            continue;
        }
        lua_pushlstring(L, name.data(), name.size());
        if (lua_pcall(L, 1, 0, 0) != 0) {
            if (err) {
                const char* m = lua_tostring(L, -1);
                *err += std::string("Story_Event_Trigger(") + name + "): " + (m ? m : "error") + "\n";
            }
            lua_pop(L, 1);
        }
        ++n;
    }
    return n;
}

void StoryModel::hash(StateHasher& h) const {
    h.u32(frame_);
    h.f64(time_);
    h.boolean(paused_);
    h.i32(player_);
    h.u32(static_cast<uint32_t>(plots_.size()));
    for (const StoryPlot& plot : plots_) {
        h.boolean(plot.active);
        h.u32(static_cast<uint32_t>(plot.events.size()));
        for (size_t i = 0; i < plot.events.size(); ++i) {
            const StoryEventState& st = plot.state[i];
            h.boolean(st.enabled);
            h.boolean(st.fired);
            h.boolean(st.armed);
            h.u32(st.fire_count);
            h.u32(st.last_fire_frame);
            h.f64(st.activation_time);
            h.f64(st.timeout_at);
            h.f64(st.inactive_start);
            h.boolean(st.timer_init);
            h.f64(st.timer_last);
            h.f64(st.timer_acc);
            h.u32(static_cast<uint32_t>(st.countdowns.size()));
            for (const int32_t c : st.countdowns) h.i32(c);
            h.i32(st.count);
            h.i32(st.seen_level);
            h.u32(st.lua_edits);
            if (st.lua_edits) {
                const StoryEventDef& def = plot.events[i];
                h.str(def.reward_type);
                h.u32(static_cast<uint32_t>(def.event_params.size()));
                for (const StoryParam& p : def.event_params) hash_param(h, p);
                h.u32(static_cast<uint32_t>(def.reward_params.size()));
                for (const StoryParam& p : def.reward_params) hash_param(h, p);
                h.u32(static_cast<uint32_t>(def.dialog.size()));
                for (const std::string& s : def.dialog) h.str(s);
            }
        }
        h.u32(static_cast<uint32_t>(plot.pending_script_triggers.size()));
    }
    h.u32(static_cast<uint32_t>(flags_->size()));
    for (const auto& [name, value] : *flags_) {
        h.str(name);
        h.i32(value);
    }
    h.u32(static_cast<uint32_t>(objectives_.size()));
    for (const StoryObjective& o : objectives_) {
        h.str(o.text);
        h.u8(static_cast<uint8_t>(o.state));
        h.boolean(o.from_lua);
    }
    h.u32(static_cast<uint32_t>(blips_.size()));
    for (const StoryBlip& b : blips_) {
        h.str(b.name);
        h.i32(b.object_id);
    }
    h.u32(static_cast<uint32_t>(highlights_.size()));
    for (const StoryHighlight& b : highlights_) {
        h.str(b.name);
        h.i32(b.object_id);
    }
    h.u32(static_cast<uint32_t>(ai_flags_.size()));
    for (const auto& [key, count] : ai_flags_) {
        h.str(key);
        h.u32(count);
    }
    h.u32(static_cast<uint32_t>(fired_.size()));
}

// ------------------------------------------------------------------------------------------ save / load (OI lua)

namespace {
constexpr uint32_t kSaveVersion = 2;  // 2 (q6 t03): + Story_Dialog of a Lua-edited event (Set_Dialog); 1 still read
constexpr uint32_t kSaveMaxCount = 1u << 24;  // sanity bound for counts / string lengths

void put_str(ByteWriter& w, std::string_view s) {
    w.u32(static_cast<uint32_t>(s.size()));
    for (const char ch : s) w.u8(static_cast<uint8_t>(ch));
}
bool get_str(ByteReader& r, std::string& s) {
    const uint32_t n = r.u32();
    if (!r.ok() || n > kSaveMaxCount) return false;
    s.clear();
    for (uint32_t i = 0; i < n && r.ok(); ++i) s.push_back(static_cast<char>(r.u8()));
    return r.ok();
}
bool get_count(ByteReader& r, uint32_t& n) {
    n = r.u32();
    return r.ok() && n <= kSaveMaxCount;
}
void put_param(ByteWriter& w, const StoryParam& p) {
    w.u8(static_cast<uint8_t>(p.kind));
    put_str(w, p.text);
    w.i32(p.id);
}
bool get_param(ByteReader& r, StoryParam& p) {
    const uint8_t k = r.u8();
    if (!r.ok() || k > static_cast<uint8_t>(StoryParam::Kind::Player)) return false;
    p.kind = static_cast<StoryParam::Kind>(k);
    if (!get_str(r, p.text)) return false;
    p.id = r.i32();
    return r.ok();
}
void put_params(ByteWriter& w, const std::vector<StoryParam>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (const StoryParam& p : v) put_param(w, p);
}
bool get_params(ByteReader& r, std::vector<StoryParam>& v) {
    uint32_t n = 0;
    if (!get_count(r, n)) return false;
    v.assign(n, StoryParam{});
    for (StoryParam& p : v)
        if (!get_param(r, p)) return false;
    return true;
}
void put_strs(ByteWriter& w, const std::vector<std::string>& v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (const std::string& s : v) put_str(w, s);
}
bool get_strs(ByteReader& r, std::vector<std::string>& v) {
    uint32_t n = 0;
    if (!get_count(r, n)) return false;
    v.assign(n, std::string());
    for (std::string& s : v)
        if (!get_str(r, s)) return false;
    return true;
}
bool get_bool(ByteReader& r) { return r.u8() != 0; }
}  // namespace

// Layout (little endian): u32 version, clock (frame, time, paused), player; flags, objectives, blips, highlights, AI flags;
// u32 plot count, per plot (load order): name (checked), active, u32 event count, per event (index order): name (checked),
// state (the hash() fields in order), Lua-edited definition fields; pending script triggers; then the fired log.
void StoryModel::write(ByteWriter& w) const {
    w.u32(kSaveVersion);
    w.u32(frame_);
    w.f64(time_);
    w.u8(paused_ ? 1 : 0);
    w.i32(player_);
    w.u32(static_cast<uint32_t>(flags_->size()));
    for (const auto& [name, value] : *flags_) {
        put_str(w, name);
        w.i32(value);
    }
    w.u32(static_cast<uint32_t>(objectives_.size()));
    for (const StoryObjective& o : objectives_) {
        put_str(w, o.text);
        w.u8(static_cast<uint8_t>(o.state));
        w.u8(o.from_lua ? 1 : 0);
    }
    w.u32(static_cast<uint32_t>(blips_.size()));
    for (const StoryBlip& b : blips_) {
        put_str(w, b.name);
        w.i32(b.object_id);
    }
    w.u32(static_cast<uint32_t>(highlights_.size()));
    for (const StoryHighlight& b : highlights_) {
        put_str(w, b.name);
        w.i32(b.object_id);
    }
    w.u32(static_cast<uint32_t>(ai_flags_.size()));
    for (const auto& [key, count] : ai_flags_) {
        put_str(w, key);
        w.u32(count);
    }
    w.u32(static_cast<uint32_t>(plots_.size()));
    for (const StoryPlot& plot : plots_) {
        put_str(w, plot.name);
        w.u8(plot.active ? 1 : 0);
        w.u32(static_cast<uint32_t>(plot.events.size()));
        for (size_t i = 0; i < plot.events.size(); ++i) {
            const StoryEventDef& def = plot.events[i];
            const StoryEventState& st = plot.state[i];
            put_str(w, def.name);
            w.u8(st.enabled ? 1 : 0);
            w.u8(st.fired ? 1 : 0);
            w.u8(st.armed ? 1 : 0);
            w.u32(st.fire_count);
            w.u32(st.last_fire_frame);
            w.f64(st.activation_time);
            w.f64(st.timeout_at);
            w.f64(st.inactive_start);
            w.u8(st.timer_init ? 1 : 0);
            w.f64(st.timer_last);
            w.f64(st.timer_acc);
            w.u32(static_cast<uint32_t>(st.countdowns.size()));
            for (const int32_t c : st.countdowns) w.i32(c);
            w.i32(st.count);
            w.i32(st.seen_level);
            w.u32(st.lua_edits);
            if (st.lua_edits) {
                put_str(w, def.reward_type);
                put_params(w, def.event_params);
                put_params(w, def.reward_params);
                put_strs(w, def.dialog);
                put_str(w, def.story_dialog);
            }
        }
        put_strs(w, plot.pending_script_triggers);
    }
    w.u32(static_cast<uint32_t>(fired_.size()));
    for (const StoryFired& f : fired_) {
        w.u32(f.sequence);
        w.u32(f.frame);
        w.u32(f.plot);
        w.u32(f.event);
        put_str(w, f.plot_name);
        put_str(w, f.event_name);
        put_str(w, f.event_type);
        put_str(w, f.reward_type);
        put_params(w, f.reward_params);
    }
}

bool StoryModel::read(ByteReader& r) {
    const uint32_t version = r.u32();
    if (!r.ok() || version < 1 || version > kSaveVersion) return false;
    const uint32_t frame = r.u32();
    const double time = r.f64();
    const bool paused = get_bool(r);
    const int32_t player = r.i32();
    uint32_t n = 0;
    std::map<std::string, int32_t> flags;
    if (!get_count(r, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        std::string name;
        if (!get_str(r, name)) return false;
        flags[name] = r.i32();
    }
    std::vector<StoryObjective> objectives;
    if (!get_count(r, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        StoryObjective o;
        if (!get_str(r, o.text)) return false;
        const uint8_t s = r.u8();
        if (s > static_cast<uint8_t>(StoryObjectiveState::Failed)) return false;
        o.state = static_cast<StoryObjectiveState>(s);
        o.from_lua = get_bool(r);
        objectives.push_back(std::move(o));
    }
    std::vector<StoryBlip> blips;
    if (!get_count(r, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        StoryBlip b;
        if (!get_str(r, b.name)) return false;
        b.object_id = r.i32();
        blips.push_back(std::move(b));
    }
    std::vector<StoryHighlight> highlights;
    if (!get_count(r, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        StoryHighlight b;
        if (!get_str(r, b.name)) return false;
        b.object_id = r.i32();
        highlights.push_back(std::move(b));
    }
    std::map<std::string, uint32_t> ai_flags;
    if (!get_count(r, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        std::string key;
        if (!get_str(r, key)) return false;
        ai_flags[key] = r.u32();
    }
    if (!get_count(r, n) || n != plots_.size()) return false;
    std::vector<StoryPlot> plots = plots_;
    for (StoryPlot& plot : plots) {
        std::string name;
        if (!get_str(r, name) || name != plot.name) return false;
        plot.active = get_bool(r);
        uint32_t ne = 0;
        if (!get_count(r, ne) || ne != plot.events.size()) return false;
        for (size_t i = 0; i < plot.events.size(); ++i) {
            StoryEventDef& def = plot.events[i];
            StoryEventState& st = plot.state[i];
            if (!get_str(r, name) || name != def.name) return false;
            st.enabled = get_bool(r);
            st.fired = get_bool(r);
            st.armed = get_bool(r);
            st.fire_count = r.u32();
            st.last_fire_frame = r.u32();
            st.activation_time = r.f64();
            st.timeout_at = r.f64();
            st.inactive_start = r.f64();
            st.timer_init = get_bool(r);
            st.timer_last = r.f64();
            st.timer_acc = r.f64();
            uint32_t nc = 0;
            if (!get_count(r, nc)) return false;
            st.countdowns.assign(nc, 0);
            for (int32_t& c : st.countdowns) c = r.i32();
            st.count = r.i32();
            st.seen_level = r.i32();
            st.lua_edits = r.u32();
            if (st.lua_edits && (!get_str(r, def.reward_type) || !get_params(r, def.event_params) ||
                                 !get_params(r, def.reward_params) || !get_strs(r, def.dialog) ||
                                 (version >= 2 && !get_str(r, def.story_dialog))))
                return false;
        }
        if (!get_strs(r, plot.pending_script_triggers)) return false;
    }
    std::vector<StoryFired> fired;
    if (!get_count(r, n)) return false;
    for (uint32_t i = 0; i < n; ++i) {
        StoryFired f;
        f.sequence = r.u32();
        f.frame = r.u32();
        f.plot = r.u32();
        f.event = r.u32();
        if (!get_str(r, f.plot_name) || !get_str(r, f.event_name) || !get_str(r, f.event_type) || !get_str(r, f.reward_type) ||
            !get_params(r, f.reward_params))
            return false;
        fired.push_back(std::move(f));
    }
    if (!r.ok()) return false;
    frame_ = frame;
    time_ = time;
    paused_ = paused;
    player_ = player;
    *flags_ = std::move(flags);
    objectives_ = std::move(objectives);
    blips_ = std::move(blips);
    highlights_ = std::move(highlights);
    ai_flags_ = std::move(ai_flags);
    plots_ = std::move(plots);
    fired_ = std::move(fired);
    // GALACTIC_DIALOGS §2.8: the holocron's load reader loads the dialog scripts again; the plot load already asked for the
    // XML names, so only the Lua-set ones (Set_Dialog) are asked for here.
    for (const StoryPlot& plot : plots_)
        for (size_t i = 0; i < plot.events.size(); ++i)
            if (plot.state[i].lua_edits) request_dialog_load(plot.events[i].story_dialog);
    return true;
}

// ------------------------------------------------------------------------------------------ holocron (GALACTIC_DIALOGS §2.8)

uint64_t StoryHolocron::add(StoryHolocronEntry e) {
    e.id = next_id_++;
    const uint64_t id = e.id;
    entries_.insert(entries_.begin(), std::move(e));  // newest first, no de-duplication
    return id;
}

bool StoryHolocron::remove_tag(std::string_view tag) {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].tag == tag) {
            entries_.erase(entries_.begin() + static_cast<std::ptrdiff_t>(i));
            return true;
        }
    return false;
}

bool StoryHolocron::complete_tag(std::string_view tag) {
    for (StoryHolocronEntry& e : entries_)
        if (e.tag == tag) {
            e.completed = true;
            return true;
        }
    return false;
}

void StoryHolocron::mark_read(size_t index) {
    if (index < entries_.size()) entries_[index].read = true;
}

size_t StoryHolocron::index_of(uint64_t id) const {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].id == id) return i;
    return SIZE_MAX;
}

const std::vector<std::string>& StoryHolocron::lines(const StoryHolocronEntry& e) const {
    if (e.model && e.plot < e.model->plot_count() && e.event < e.model->plot(e.plot).events.size())
        return e.model->plot(e.plot).events[e.event].dialog;
    return e.detached_lines;
}

void StoryHolocron::detach(const StoryModel* model) {
    for (StoryHolocronEntry& e : entries_)
        if (e.model == model) {
            e.detached_lines = lines(e);
            e.model = nullptr;
            e.ref_set = -1;
        }
}

void StoryHolocron::clear() {
    entries_.clear();
    log_.clear();
    next_id_ = 1;
}

namespace {
constexpr uint32_t kHolocronVersion = 1;
}  // namespace

// Layout: u32 version, u64 next id, u32 count, per record (list order, newest first): id, dialog, var, tag, chapter, frame,
// flags (completed, read, auto-tag), the reference (i32 set number, -1 = none; plot, event) and, without one, the lines.
void StoryHolocron::write(ByteWriter& w, const std::function<int32_t(const StoryModel*)>& set_of) const {
    w.u32(kHolocronVersion);
    w.u64(next_id_);
    w.u32(static_cast<uint32_t>(entries_.size()));
    for (const StoryHolocronEntry& e : entries_) {
        w.u64(e.id);
        put_str(w, e.dialog);
        put_str(w, e.var);
        put_str(w, e.tag);
        w.i32(e.chapter);
        w.u32(e.frame);
        w.u8(e.completed ? 1 : 0);
        w.u8(e.read ? 1 : 0);
        w.u8(e.auto_tag ? 1 : 0);
        const int32_t set = e.model && set_of ? set_of(e.model) : (e.model ? -1 : e.ref_set);
        w.i32(set);
        w.u32(e.plot);
        w.u32(e.event);
        if (set < 0) put_strs(w, lines(e));
    }
}

bool StoryHolocron::read(ByteReader& r) {
    if (r.u32() != kHolocronVersion || !r.ok()) return false;
    const uint64_t next = r.u64();
    uint32_t n = 0;
    if (!get_count(r, n)) return false;
    std::vector<StoryHolocronEntry> entries(n);
    for (StoryHolocronEntry& e : entries) {
        e.id = r.u64();
        if (!get_str(r, e.dialog) || !get_str(r, e.var) || !get_str(r, e.tag)) return false;
        e.chapter = r.i32();
        e.frame = r.u32();
        e.completed = get_bool(r);
        e.read = get_bool(r);
        e.auto_tag = get_bool(r);
        e.ref_set = r.i32();
        e.plot = r.u32();
        e.event = r.u32();
        if (e.ref_set < 0 && !get_strs(r, e.detached_lines)) return false;
        if (!r.ok()) return false;
    }
    entries_ = std::move(entries);
    log_.clear();
    next_id_ = next;
    return true;
}

void StoryHolocron::relink(int32_t set, const StoryModel* model) {
    for (StoryHolocronEntry& e : entries_)
        if (e.ref_set == set && set >= 0 && model && e.plot < model->plot_count() && e.event < model->plot(e.plot).events.size())
            e.model = model;
}

}  // namespace alamo::lua

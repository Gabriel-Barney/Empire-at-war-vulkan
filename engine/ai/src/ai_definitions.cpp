#include "alamo/ai/ai_definitions.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "alamo/data/xml.hpp"

namespace alamo::ai {

// ================================================================ ai_types helpers

namespace {
constexpr std::string_view kPlanEventNames[kPlanEventCount] = {
    "Space_Conflict_Begin", "Space_Conflict_End", "Unit_Destroyed", "Fleet_Destoyed", "Production_Failed",
    "Original_Target_Owner_Changed", "Current_Target_Owner_Changed", "No_Units_Remaining", "Ground_Conflict_Begin",
    "Ground_Conflict_End", "Original_Target_Destroyed", "Current_Target_Destroyed", "Target_In_Range",
    "Reached_System_On_Path", "Unit_Damaged", "Special_Weapon_Online", "Hardpoint_Target_In_Range",
    "Hardpoint_Opportunity_Target_Acquired", "Unit_Move_Finished", "Unit_Ability_Finished", "Unit_Diversion_Finished",
    "Unit_Ability_Cancelled", "Unit_Ability_Ready", "Unit_No_Threat",
};
}  // namespace

std::string_view plan_event_name(PlanEvent e) {
    const auto i = static_cast<uint32_t>(e);
    return i < kPlanEventCount ? kPlanEventNames[i] : std::string_view{};
}

bool plan_event_code(std::string_view name, PlanEvent& out) {
    for (uint32_t i = 0; i < kPlanEventCount; ++i)
        if (ai_iequal(kPlanEventNames[i], name)) {
            out = static_cast<PlanEvent>(i);
            return true;
        }
    return false;
}

bool parse_ai_mode(std::string_view s, AiMode& out) {
    s = ai_trim(s);
    if (ai_iequal(s, "Galactic")) out = AiMode::Galactic;
    else if (ai_iequal(s, "Land")) out = AiMode::Land;
    else if (ai_iequal(s, "Space")) out = AiMode::Space;
    else return false;
    return true;
}

std::vector<std::string> ai_split(std::string_view s, std::string_view seps) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = i;
        while (j < s.size() && seps.find(s[j]) == std::string_view::npos) ++j;
        const std::string_view piece = ai_trim(s.substr(i, j - i));
        if (!piece.empty()) out.emplace_back(piece);
        i = j + 1;
    }
    return out;
}

bool ai_parse_float(std::string_view s, float& out) {
    s = ai_trim(s);
    if (s.empty()) return false;
    // Locale-free decimal parse: [sign] digits [. digits] [e[sign]digits].
    size_t i = 0;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') neg = s[i++] == '-';
    double v = 0.0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
        v = v * 10.0 + (s[i++] - '0');
        any = true;
    }
    if (i < s.size() && s[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
            v += (s[i++] - '0') * scale;
            scale *= 0.1;
            any = true;
        }
    }
    if (!any) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        int e = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') e = e * 10 + (s[i++] - '0');
        for (int k = 0; k < e; ++k) v = eneg ? v / 10.0 : v * 10.0;
    }
    if (i != s.size() && !(i + 1 == s.size() && (s[i] == 'f' || s[i] == 'F'))) return false;
    out = static_cast<float>(neg ? -v : v);
    return true;
}

uint32_t parse_application_flags(std::string_view s, bool* ok) {
    static constexpr std::pair<const char*, uint32_t> kNames[] = {
        {"Friendly", app::Friendly},
        {"Enemy", app::Enemy},
        {"Neutral", app::Neutral},  // [name-ok: AIGoalApplicationFlags keyword, not a faction]
        {"Contested", app::Contested},
        {"Global", app::Global},
        {"Friendly_Unit", app::FriendlyUnit},
        {"Enemy_Unit", app::EnemyUnit},
        {"Friendly_Structure", app::FriendlyStructure},
        {"Enemy_Structure", app::EnemyStructure},
        {"Tactical_Location", app::TacticalLocation},
        {"Friendly_Build_Pad", app::FriendlyBuildPad},
        {"Enemy_Build_Pad", app::EnemyBuildPad},
        {"Friendly_Cash_Point", app::FriendlyCashPoint},
        {"Enemy_Cash_Point", app::EnemyCashPoint},
        {"Friendly_Reinforcement_Point", app::FriendlyReinforcementPoint},
        {"Enemy_Reinforcement_Point", app::EnemyReinforcementPoint},
    };
    uint32_t bits = 0;
    bool good = true;
    for (const std::string& name : ai_split(s)) {
        bool found = false;
        for (const auto& [n, b] : kNames)
            if (ai_iequal(name, n)) {
                bits |= b;
                found = true;
            }
        if (!found) good = false;
    }
    if (ok) *ok = good;
    return bits;
}

bool parse_reachability(std::string_view s, Reachability& out) {
    static constexpr std::pair<const char*, Reachability> kNames[] = {
        {"Friendly_only", Reachability::FriendlyOnly},
        {"Friendly_Ignore_Threat", Reachability::FriendlyIgnoreThreat},
        {"Enemy_destination", Reachability::EnemyDestination},
        {"Enemy_undefended", Reachability::EnemyUndefended},
        {"Single_Hop_Disconnected", Reachability::SingleHopDisconnected},
        {"Any", Reachability::Any},
        {"No_Threat", Reachability::NoThreat},
        {"Low_Threat", Reachability::LowThreat},
        {"Medium_Threat", Reachability::MediumThreat},
        {"High_Threat", Reachability::HighThreat},
        {"Any_Threat", Reachability::AnyThreat},
    };
    s = ai_trim(s);
    for (const auto& [n, r] : kNames)
        if (ai_iequal(s, n)) {
            out = r;
            return true;
        }
    return false;
}

// ================================================================ AiDefinitions

namespace {

const char* const kDefaultCategories[] = {"Offensive",  "Defensive",      "Information",   "Infrastructure",
                                          "Tactical_Targeted", "Tactical_Untargeted", "NoBudget", "Hero",
                                          "Always",     "Macro_Goal",     "MajorItem",     "StoryArc",
                                          "StoryArcHuman", "Interventions", "High_Priority", "Sandbox_Events",
                                          "Med_Priority", "Map_Control"};
const int32_t kDefaultCategoryValues[] = {0, 1, 2, 3, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};

std::string diag(const std::string& file, uint32_t line, const std::string& msg) {
    return file + "(" + std::to_string(line) + "): " + msg;
}

}  // namespace

AiDefinitions::AiDefinitions() {
    for (const char* c : kDefaultCategories) categories.emplace_back(c);
    category_value.assign(std::begin(kDefaultCategoryValues), std::end(kDefaultCategoryValues));
}

void AiDefinitions::load_category_enum(std::string_view xml, const std::string& file) {
    const data::XmlDocument doc = data::XmlDocument::parse(xml, file);
    if (!doc.ok()) {
        diagnostics.push_back(file + ": unreadable category enum (defaults kept)");
        return;
    }
    std::vector<std::string> names;
    std::vector<int32_t> values;
    // Dynamic-enum rules (OBJECT_TYPES §9.1 [H], as the equation-literal table reads the same file, A2J t07): C atoi of
    // the text; an entry with empty text gets the running entry index.
    for (const data::XmlNode n : doc.root().children()) {
        const std::string_view t = n.text_trimmed();
        names.emplace_back(n.name());
        values.push_back(t.empty() ? static_cast<int32_t>(names.size() - 1) : std::atoi(std::string(t).c_str()));
    }
    if (names.empty()) return;
    categories = std::move(names);
    category_value = std::move(values);
}

int32_t AiDefinitions::find_category(std::string_view name) const {
    name = ai_trim(name);
    for (size_t i = 0; i < categories.size(); ++i)
        if (ai_iequal(categories[i], name)) return static_cast<int32_t>(i);
    return -1;
}

uint32_t AiDefinitions::find_goal(std::string_view name) const {
    const auto it = goal_index_.find(ai_upper_copy(ai_trim(name)));
    return it == goal_index_.end() ? kAiNone : it->second;
}

void AiDefinitions::index_goal(uint32_t g) { goal_index_.emplace(ai_upper_copy(goals[g].name), g); }

void AiDefinitions::add_goals(std::string_view xml, const std::string& file) {
    const data::XmlDocument doc = data::XmlDocument::parse(xml, file);
    if (!doc.ok()) {
        diagnostics.push_back(file + ": unreadable goals file");
        return;
    }
    for (const data::XmlNode n : doc.root().children()) {
        GoalTypeDef g;
        g.name = std::string(n.name());
        for (const data::XmlNode f : n.children()) {
            const std::string_view tag = f.name();
            const std::string_view v = f.text_trimmed();
            float x = 0.0f;
            if (ai_iequal(tag, "AIGoalApplicationFlags")) {
                bool ok = true;
                g.application = parse_application_flags(v, &ok);
                if (!ok) diagnostics.push_back(diag(file, f.line(), g.name + ": unknown application flag in '" + std::string(v) + "'"));
            } else if (ai_iequal(tag, "GameMode")) {
                g.mode_set = parse_ai_mode(v, g.mode);
                if (!g.mode_set) diagnostics.push_back(diag(file, f.line(), g.name + ": unknown GameMode '" + std::string(v) + "'"));
            } else if (ai_iequal(tag, "Category")) {
                g.category = find_category(v);
                if (g.category < 0) diagnostics.push_back(diag(file, f.line(), g.name + ": unknown category '" + std::string(v) + "'"));
            } else if (ai_iequal(tag, "Reachability")) {
                if (!parse_reachability(v, g.reachability))
                    diagnostics.push_back(diag(file, f.line(), g.name + ": unknown reachability '" + std::string(v) + "'"));
            } else if (ai_iequal(tag, "Time_Limit") && ai_parse_float(v, x)) {
                g.time_limit = x;
            } else if (ai_iequal(tag, "Build_Time_Delay_Tolerance") && ai_parse_float(v, x)) {
                g.build_time_delay_tolerance = x;
            } else if (ai_iequal(tag, "Is_Like")) {
                for (std::string& s : ai_split(v)) g.is_like_names.push_back(std::move(s));
            } else if (ai_iequal(tag, "Global_Exclusions")) {
                for (std::string& s : ai_split(v)) g.exclusion_names.push_back(std::move(s));
            } else if (ai_iequal(tag, "Tracking_Duration") && ai_parse_float(v, x)) {
                g.tracking_duration = x;
            } else if (ai_iequal(tag, "Per_Failure_Desire_Adjust") && ai_parse_float(v, x)) {
                g.per_failure_desire_adjust = x;
            } else if (ai_iequal(tag, "Activation_Tracking_Duration") && ai_parse_float(v, x)) {
                g.activation_tracking_duration = x;
            } else if (ai_iequal(tag, "Per_Activation_Failure_Desire_Adjust") && ai_parse_float(v, x)) {
                g.per_activation_failure_desire_adjust = x;
            }
        }
        if (find_goal(g.name) != kAiNone) {
            diagnostics.push_back(diag(file, n.line(), "duplicate goal type " + g.name + " (first kept)"));
            continue;
        }
        goals.push_back(std::move(g));
        index_goal(static_cast<uint32_t>(goals.size() - 1));
    }
}

void AiDefinitions::link() {
    for (GoalTypeDef& g : goals) {
        g.is_like.clear();
        g.exclusions.clear();
        for (const std::string& n : g.is_like_names) {
            const uint32_t i = find_goal(n);
            if (i == kAiNone) diagnostics.push_back(g.name + ": Is_Like names unknown goal " + n);
            else g.is_like.push_back(i);
        }
        for (const std::string& n : g.exclusion_names) {
            const uint32_t i = find_goal(n);
            if (i == kAiNone) diagnostics.push_back(g.name + ": Global_Exclusions names unknown goal " + n);
            else g.exclusions.push_back(i);
        }
    }
}

void AiDefinitions::add_function_set(std::string_view set_name, std::string_view xml, const std::string& file) {
    const data::XmlDocument doc = data::XmlDocument::parse(xml, file);
    if (!doc.ok()) {
        diagnostics.push_back(file + ": unreadable goal function set");
        return;
    }
    if (find_set(set_name)) {
        diagnostics.push_back(file + ": duplicate function set " + std::string(set_name) + " (first kept)");
        return;
    }
    GoalFunctionSet s;
    s.name = std::string(set_name);
    for (const data::XmlNode n : doc.root().children()) {
        GoalFunctionEntry e;
        const std::string_view goal = n.child_text("Goal");
        e.goal = find_goal(goal);
        e.function = std::string(n.child_text("Function"));
        if (e.goal == kAiNone) {
            diagnostics.push_back(diag(file, n.line(), "unknown goal '" + std::string(goal) + "' in set " + s.name));
            continue;
        }
        s.entries.push_back(std::move(e));
    }
    sets.push_back(std::move(s));
}

const GoalFunctionSet* AiDefinitions::find_set(std::string_view name) const {
    for (const GoalFunctionSet& s : sets)
        if (ai_iequal(s.name, ai_trim(name))) return &s;
    return nullptr;
}

void AiDefinitions::add_players(std::string_view xml, const std::string& file) {
    const data::XmlDocument doc = data::XmlDocument::parse(xml, file);
    if (!doc.ok()) {
        diagnostics.push_back(file + ": unreadable AI player file");
        return;
    }
    const data::XmlNode r = doc.root();
    AiPlayerTypeDef p;
    p.name = std::string(r.child_text("Name"));
    p.free_store_script[static_cast<size_t>(AiMode::Galactic)] = std::string(r.child_text("GalacticFreeStoreScript"));
    p.free_store_script[static_cast<size_t>(AiMode::Space)] = std::string(r.child_text("SpaceFreeStoreScript"));
    p.free_store_script[static_cast<size_t>(AiMode::Land)] = std::string(r.child_text("LandFreeStoreScript"));
    for (std::string& s : ai_split(r.child_text("GoalProposalFunctionSets"))) p.function_sets.push_back(std::move(s));
    if (const data::XmlNode t = r.child("Templates")) {
        for (const auto& [tag, mode] : {std::pair{"Galactic", AiMode::Galactic}, std::pair{"Land", AiMode::Land},
                                        std::pair{"Space", AiMode::Space}})
            for (std::string& s : ai_split(t.child_text(tag))) p.templates[static_cast<size_t>(mode)].push_back(std::move(s));
    }
    if (const data::XmlNode d = r.child("Difficulty_Adjustments")) {
        p.difficulty[0] = std::string(d.child_text("Easy"));
        p.difficulty[1] = std::string(d.child_text("Normal"));
        p.difficulty[2] = std::string(d.child_text("Hard"));
    }
    if (const data::XmlNode g = r.child("GoalSetExtensionSize")) {
        const auto v = static_cast<uint32_t>(std::atoi(std::string(g.text_trimmed()).c_str()));
        p.goal_set_extension_size = static_cast<int32_t>(std::min(v, 5u));
    }
    if (p.name.empty()) {
        diagnostics.push_back(file + ": AI player type without <Name>");
        return;
    }
    if (find_player(p.name)) {
        diagnostics.push_back(file + ": duplicate AI player type " + p.name + " (first kept)");
        return;
    }
    players.push_back(std::move(p));
}

const AiPlayerTypeDef* AiDefinitions::find_player(std::string_view name) const {
    for (const AiPlayerTypeDef& p : players)
        if (ai_iequal(p.name, ai_trim(name))) return &p;
    return nullptr;
}

void AiDefinitions::add_templates(std::string_view xml, const std::string& file) {
    const data::XmlDocument doc = data::XmlDocument::parse(xml, file);
    if (!doc.ok()) {
        diagnostics.push_back(file + ": unreadable templates file");
        return;
    }
    auto read_switch = [&](data::XmlNode sw, TemplateSwitch& out, const std::string& tname) {
        if (!sw) return;
        for (const data::XmlNode goals_node : sw.children("Goals"))
            for (const data::XmlNode g : goals_node.children()) {
                if (ai_iequal(g.name(), "Category")) {
                    const int32_t c = find_category(g.text_trimmed());
                    if (c < 0) diagnostics.push_back(diag(file, g.line(), tname + ": unknown category " + std::string(g.text_trimmed())));
                    else out.goal_categories.push_back(c);
                } else if (ai_iequal(g.name(), "Goal_Type")) {
                    const uint32_t t = find_goal(g.text_trimmed());
                    if (t == kAiNone) diagnostics.push_back(diag(file, g.line(), tname + ": unknown goal " + std::string(g.text_trimmed())));
                    else out.goal_types.push_back(t);
                }
            }
        for (const data::XmlNode plans : sw.children("Plans"))
            for (const data::XmlNode p : plans.children()) {
                if (ai_iequal(p.name(), "Goal_Category")) {
                    const int32_t c = find_category(p.text_trimmed());
                    if (c < 0) diagnostics.push_back(diag(file, p.line(), tname + ": unknown category " + std::string(p.text_trimmed())));
                    else out.plan_categories.push_back(c);
                } else if (ai_iequal(p.name(), "Name")) {
                    out.plan_names.emplace_back(p.text_trimmed());
                }
            }
    };
    for (const data::XmlNode n : doc.root().children()) {
        TemplateDef t;
        t.name = std::string(n.name());
        if (const data::XmlNode pr = n.child("Priority")) t.priority = std::atoi(std::string(pr.text_trimmed()).c_str());
        t.trigger = std::string(n.child_text("Trigger"));
        t.untrigger = std::string(n.child_text("Un_Trigger"));
        if (const data::XmlNode b = n.child("Budget"))
            for (const data::XmlNode e : b.children()) {
                const int32_t c = find_category(e.name());
                if (c < 0) diagnostics.push_back(diag(file, e.line(), t.name + ": unknown budget category " + std::string(e.name())));
                else t.budget.emplace_back(c, std::string(e.text_trimmed()));
            }
        if (const data::XmlNode b = n.child("Tactical_Budget_Allowance"))
            for (const data::XmlNode e : b.children()) {
                const int32_t c = find_category(e.name());
                float v = 0.0f;
                if (c >= 0 && ai_parse_float(e.text_trimmed(), v)) t.tactical_allowance.emplace_back(c, v);
            }
        read_switch(n.child("Turn_On"), t.turn_on, t.name);
        read_switch(n.child("Turn_Off"), t.turn_off, t.name);
        if (find_template(t.name)) {
            diagnostics.push_back(diag(file, n.line(), "duplicate template " + t.name + " (first kept)"));
            continue;
        }
        templates.push_back(std::move(t));
    }
}

const TemplateDef* AiDefinitions::find_template(std::string_view name) const {
    const uint32_t i = template_index(name);
    return i == kAiNone ? nullptr : &templates[i];
}

uint32_t AiDefinitions::template_index(std::string_view name) const {
    for (size_t i = 0; i < templates.size(); ++i)
        if (ai_iequal(templates[i].name, ai_trim(name))) return static_cast<uint32_t>(i);
    return kAiNone;
}

// ================================================================ loading

std::string file_base_name(std::string_view path) {
    size_t b = path.find_last_of("\\/");
    b = b == std::string_view::npos ? 0 : b + 1;
    size_t e = path.find_last_of('.');
    if (e == std::string_view::npos || e < b) e = path.size();
    return std::string(path.substr(b, e - b));
}

AiDataPaths list_ai_data(const AiFileLister& list) {
    AiDataPaths p;
    p.goals = list("Data\\XML\\AI\\Goals\\*.xml");
    p.goal_functions = list("Data\\XML\\AI\\GoalFunctions\\*.xml");
    p.players = list("Data\\XML\\AI\\Players\\*.xml");
    p.templates = list("Data\\XML\\AI\\Templates\\*.xml");
    return p;
}

bool load_ai_definitions(const FileProvider& files, const AiDataPaths& paths, AiDefinitions& out) {
    std::vector<uint8_t> bytes;
    auto text = [&]() { return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()); };
    size_t read = 0;
    if (!paths.category_enum.empty() && files.read(paths.category_enum, bytes)) out.load_category_enum(text(), paths.category_enum);
    for (const std::string& f : paths.goals)
        if (files.read(f, bytes)) {
            out.add_goals(text(), f);
            ++read;
        } else {
            out.diagnostics.push_back(f + ": cannot read");
        }
    out.link();
    for (const std::string& f : paths.goal_functions)
        if (files.read(f, bytes)) {
            out.add_function_set(file_base_name(f), text(), f);
            ++read;
        }
    for (const std::string& f : paths.players)
        if (files.read(f, bytes)) {
            out.add_players(text(), f);
            ++read;
        }
    for (const std::string& f : paths.templates)
        if (files.read(f, bytes)) {
            out.add_templates(text(), f);
            ++read;
        }
    return read > 0;
}

// ================================================================ AI assignment (AI_GALACTIC §1.2)

std::string ai_player_control_type(const std::vector<std::string>& pairs, std::string_view faction) {
    for (const std::string& p : pairs) {
        const size_t comma = p.find(',');
        if (comma == std::string::npos) continue;
        if (!ai_iequal(ai_trim(std::string_view(p).substr(0, comma)), ai_trim(faction))) continue;
        return std::string(ai_trim(std::string_view(p).substr(comma + 1)));
    }
    return {};
}

std::map<std::string, std::string> load_faction_basic_ai(const FileProvider& files) {
    std::map<std::string, std::string> out;
    std::vector<uint8_t> bytes;
    auto parse = [&bytes](const std::string& name) {
        return data::XmlDocument::parse(bytes, name);
    };
    if (!files.read("Data\\XML\\FactionFiles.xml", bytes)) return out;
    std::vector<std::string> list;
    {
        const data::XmlDocument doc = parse("FactionFiles.xml");
        if (!doc.ok()) return out;
        for (const data::XmlNode n : doc.root().children())
            if (ai_iequal(n.name(), "File")) list.emplace_back(n.text_trimmed());
    }
    for (const std::string& f : list) {
        const std::string path = "Data\\XML\\" + f;
        if (!files.read(path, bytes)) continue;
        const data::XmlDocument doc = parse(f);
        if (!doc.ok()) continue;
        for (const data::XmlNode n : doc.root().children()) {
            if (!ai_iequal(n.name(), "Faction")) continue;
            const std::string key = ai_upper_copy(ai_trim(n.attribute("Name", "")));
            if (key.empty() || out.count(key)) continue;
            const std::string_view b = ai_trim(n.child_text("Basic_AI"));
            if (!b.empty()) out.emplace(key, std::string(b));
        }
    }
    return out;
}

std::string campaign_ai_type(const std::vector<std::string>& pairs, const std::map<std::string, std::string>& basic_ai,
                             std::string_view faction, bool fill_unassigned) {
    std::string t = ai_player_control_type(pairs, faction);
    if (t.empty() && fill_unassigned) {  // [port] AI-vs-AI hosts: an unassigned (human) slot plays its Basic_AI
        const auto it = basic_ai.find(ai_upper_copy(ai_trim(faction)));
        if (it != basic_ai.end()) t = it->second;
    }
    if (ai_iequal(t, "Human")) return {};
    return t;
}

}  // namespace alamo::ai

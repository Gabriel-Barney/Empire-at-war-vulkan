// P4 sim (owner: galactic agent): loader of the special-system tables (special_data.hpp, docs/re/P4_SIM_SYSTEMS.md).
#include <algorithm>
#include <map>
#include <memory>

#include "alamo/core/hash.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::galactic {

namespace {

float to_f(std::string_view s) { return static_cast<float>(objects::c_atof(ascii_trim(s))); }

std::vector<float> float_list(std::string_view s) {
    std::vector<float> out;
    for (std::string_view t : objects::split_type_list(s)) out.push_back(static_cast<float>(objects::c_atof(t)));
    return out;
}

std::vector<std::string> name_list(std::string_view s) {
    std::vector<std::string> out;
    for (std::string_view t : objects::split_type_list(s)) out.emplace_back(t);
    return out;
}

void note(std::vector<std::string>* diag, std::string msg) {
    if (diag) diag->push_back(std::move(msg));
}

SpecialAbilityClass class_of(std::string_view tag) {
    if (ascii_iequals(tag, "Black_Market_Ability")) return SpecialAbilityClass::BlackMarket;
    if (ascii_iequals(tag, "Galactic_Sabotage_Ability")) return SpecialAbilityClass::Sabotage;
    if (ascii_iequals(tag, "Hack_Super_Weapon_Ability")) return SpecialAbilityClass::Hack;
    if (ascii_iequals(tag, "Corruption_Ability")) return SpecialAbilityClass::Corruption;
    if (ascii_iequals(tag, "Planet_Income_Bonus_Ability")) return SpecialAbilityClass::IncomeBonus;
    if (ascii_iequals(tag, "Planet_Income_Gambling_Ability")) return SpecialAbilityClass::IncomeGambling;
    if (ascii_iequals(tag, "Slicer_Ability")) return SpecialAbilityClass::Slicer;
    if (ascii_iequals(tag, "System_Spy_Ability")) return SpecialAbilityClass::SystemSpy;
    if (ascii_iequals(tag, "Reduce_Production_Price_Ability")) return SpecialAbilityClass::ReducePrice;
    if (ascii_iequals(tag, "Reduce_Production_Time_Ability")) return SpecialAbilityClass::ReduceTime;
    return SpecialAbilityClass::Other;
}

// GRC 2.3.1: System_Spy_Ability See_* flags -> VisibilityLevelType bits (0x70fd20). A spy never grants bit 0.
struct SpyFlag {
    const char* tag;
    uint32_t bits;
};
constexpr SpyFlag kSpyFlags[] = {
    {"See_Base_Levels", 0x6},           {"See_Credit_Income", 0x8},          {"See_Credit_Income_Breakdown", 0x8000},
    {"See_Num_Fleets", 0x30},           {"See_Most_Powerful_Ship", 0x10000}, {"See_Fleet_Contents", 0x40},
    {"See_Num_Ground_Companies", 0x80}, {"See_Ground_Company_Contents", 0x100}, {"See_Special_Structures", 0x200},
    {"See_Political_Control", 0x400},   {"See_Political_Control_Breakdown", 0x800}, {"See_Current_Production", 0x1000},
    {"See_Minor_Stealth_Heroes", 0x2000}, {"See_Major_Stealth_Heroes", 0x4000}, {"See_Force_Sensitive_Heroes", 0x20000},
    {"See_Super_Weapons", 0x40000},
};

// One <Abilities> child element (raw field `f`) -> definition, with the class post-load rules.
SpecialAbilityDef parse_ability(const objects::ObjectTypeDatabase& raw, objects::FieldIndex f, std::vector<std::string>* diag) {
    const objects::FieldColumns& fc = raw.fields();
    SpecialAbilityDef a;
    a.tag = std::string(raw.tag_name(f));
    a.cls = class_of(a.tag);
    for (uint32_t i = 0; i < fc.attr_count[f]; ++i) {
        const objects::XmlAttr& at = raw.attributes()[fc.attr_begin[f] + i];
        if (ascii_iequals(at.name, "Name")) a.name = std::string(ascii_trim(at.value));
    }
    a.name_crc = special_name_crc(a.name);
    bool have_style = false, have_halt = false;
    std::vector<float> bm_min, bm_max;
    for (objects::FieldIndex g = f + 1; g < fc.size() && fc.depth[g] > fc.depth[f]; ++g) {
        if (fc.parent[g] != f) continue;
        const std::string_view tag = raw.tag_name(g);
        const std::string_view v = ascii_trim(fc.text[g]);
        if (v.empty()) continue;  // empty value = no-op (OBJECT_TYPES field rule)
        if (ascii_iequals(tag, "Activation_Style")) {
            a.style = parse_activation_style(v);
            have_style = a.style != ActivationStyle::None;
        } else if (ascii_iequals(tag, "Initially_Enabled")) a.initially_enabled = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Causes_Despawn")) a.causes_despawn = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Min_Respawn_Time")) a.min_respawn = to_f(v);
        else if (ascii_iequals(tag, "Max_Respawn_Time")) a.max_respawn = to_f(v);
        else if (ascii_iequals(tag, "Must_Be_Bought_On_Black_Market")) a.must_be_bought = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Min_Respawn_Times")) bm_min = float_list(v);
        else if (ascii_iequals(tag, "Max_Respawn_Times")) bm_max = float_list(v);
        else if (ascii_iequals(tag, "Price_Modifier")) a.price_modifier = to_f(v);
        else if (ascii_iequals(tag, "Tech_Level_Adjustment")) a.tech_level_adjustment = static_cast<int32_t>(to_f(v));
        else if (ascii_iequals(tag, "Can_Reduce_Ground_Base_Level")) a.sabotage_can[0] = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Can_Reduce_Starbase_Level")) a.sabotage_can[1] = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Can_Cancel_Production")) a.sabotage_can[2] = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Can_Destroy_Special_Structure")) a.sabotage_can[3] = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Can_Halt_Credit_Production")) a.sabotage_can[4] = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Duration_Of_Credit_Halt")) {
            a.halt_duration = to_f(v);
            have_halt = true;
        } else if (ascii_iequals(tag, "Sabotage_Cost_Increase_Factor")) a.cost_increase_factor = to_f(v);
        else if (ascii_iequals(tag, "Percentage_Income_Modifier")) a.income_percentage = to_f(v);
        else if (ascii_iequals(tag, "Absolute_Income_Modifier")) a.income_absolute = to_f(v);
        else if (ascii_iequals(tag, "Additional_Multiplier_On_Mining_Colonies")) a.mining_multiplier = to_f(v);
        else if (ascii_iequals(tag, "Specific_Mod_Source_Text")) a.source_text = std::string(v);
        else if (ascii_iequals(tag, "Evaluation_Interval_In_Secs")) a.evaluation_interval = to_f(v);
        else if (ascii_iequals(tag, "Min_Income_Modifier")) a.min_income_modifier = to_f(v);
        else if (ascii_iequals(tag, "Max_Income_Modifier")) a.max_income_modifier = to_f(v);
        else if (ascii_iequals(tag, "Chance_To_Win")) a.chance_to_win = to_f(v);
        else if (ascii_iequals(tag, "Min_Respawn_Time_Per_Tech_Level")) a.slice_min_respawn = float_list(v);
        else if (ascii_iequals(tag, "Max_Respawn_Time_Per_Tech_Level")) a.slice_max_respawn = float_list(v);
        else if (ascii_iequals(tag, "Chance_Modifier_By_Base_Level")) a.chance_by_base_level = PiecewiseCurve::parse(v);
        else if (ascii_iequals(tag, "Chance_Modifier_By_Planet_Difficulty")) a.chance_by_difficulty = PiecewiseCurve::parse(v);
        else if (ascii_iequals(tag, "Cost_Multiplier_By_Planet_Tech_Availability")) a.cost_by_tech_availability = PiecewiseCurve::parse(v);
        else if (ascii_iequals(tag, "Chance_Affinity_Modifier")) a.chance_affinity = to_f(v);
        else if (ascii_iequals(tag, "Cost_Affinity_Modifier")) a.cost_affinity = to_f(v);
        else if (ascii_iequals(tag, "Duration_In_Secs")) a.spy_duration = to_f(v);
        else if (ascii_iequals(tag, "Price_Reduction_Percentage") || ascii_iequals(tag, "Time_Reduction_Percentage")) a.production_reduction = to_f(v);
        else if (ascii_iequals(tag, "Stacking_Category")) a.stacking_category = static_cast<int32_t>(to_f(v));
        else if (ascii_iequals(tag, "Galaxy_Wide")) a.galaxy_wide = objects::parse_type_bool(v);
        else if (a.cls == SpecialAbilityClass::SystemSpy) {
            for (const SpyFlag& sf : kSpyFlags)
                if (ascii_iequals(tag, sf.tag)) {
                    if (objects::parse_type_bool(v)) a.spy_mask |= sf.bits;
                    else a.spy_mask &= ~sf.bits;
                }
        }
    }
    (void)have_halt;
    switch (a.cls) {
    case SpecialAbilityClass::BlackMarket:
        // Post-load 0x61b0e0 [H]: min <= max per entry, else both vectors are cleared ([inferred]: also on a size
        // mismatch); Ground_Activated; Causes_Despawn forced Yes.
        {
            bool ok = bm_min.size() == bm_max.size();
            for (size_t i = 0; ok && i < bm_min.size(); ++i) ok = bm_min[i] <= bm_max[i];
            if (ok) {
                a.bm_min_respawn = std::move(bm_min);
                a.bm_max_respawn = std::move(bm_max);
            }
        }
        a.style = ActivationStyle::GroundActivated;
        a.causes_despawn = true;
        break;
    case SpecialAbilityClass::Sabotage:
        // Post-load 0x5b40f0 [H]. Halt duration: 10.0 when halting is on and the value is <= 0, except the -1 =
        // permanent marker ([inferred]: the spec gives both rules).
        if (a.sabotage_can[4] && a.halt_duration <= 0.0f && a.halt_duration != -1.0f) a.halt_duration = 10.0f;
        a.style = ActivationStyle::GroundActivated;
        a.causes_despawn = true;
        break;
    case SpecialAbilityClass::Hack:
        a.style = ActivationStyle::GroundActivated;  // vanilla Causes_Despawn No: as parsed [H]
        break;
    case SpecialAbilityClass::Corruption:
        a.style = ActivationStyle::GroundActivated;  // ctor 0x52b0c0: style 6, Causes_Despawn
        a.causes_despawn = true;
        break;
    case SpecialAbilityClass::IncomeBonus:
        if (!have_style) a.style = ActivationStyle::GalacticAutomatic;
        if (a.income_percentage < -1.0f) {  // 0x708150 [H]
            note(diag, "galactic: " + a.name + ": Percentage_Income_Modifier < -1.0 reset to 0");
            a.income_percentage = 0.0f;
        }
        a.source_tag = a.source_text.empty() ? 0u : special_name_crc(a.source_text);
        break;
    case SpecialAbilityClass::IncomeGambling:
        if (!have_style) a.style = ActivationStyle::GalacticAutomatic;
        // Field ranges of P4 4.2 [H]: values outside are clamped [inferred].
        if (a.evaluation_interval < 0.0f) a.evaluation_interval = 0.0f;
        if (a.min_income_modifier < -1.0f) a.min_income_modifier = -1.0f;
        if (a.min_income_modifier > 0.0f) a.min_income_modifier = 0.0f;
        if (a.max_income_modifier < 0.0f) a.max_income_modifier = 0.0f;
        if (a.chance_to_win < 0.0f) a.chance_to_win = 0.0f;
        if (a.chance_to_win > 1.0f) a.chance_to_win = 1.0f;
        break;
    case SpecialAbilityClass::Slicer: {
        // Post-load 0x619980 [H]: Ground_Activated, Causes_Despawn; respawn lists equal-length with min <= max, else
        // both cleared; each curve >= 2 points, else (0, 0)-(5, 0) (the cost curve (0, 1)-(5, 1)).
        a.style = ActivationStyle::GroundActivated;
        a.causes_despawn = true;
        bool ok = a.slice_min_respawn.size() == a.slice_max_respawn.size();
        for (size_t i = 0; ok && i < a.slice_min_respawn.size(); ++i) ok = a.slice_min_respawn[i] <= a.slice_max_respawn[i];
        if (!ok) a.slice_min_respawn.clear(), a.slice_max_respawn.clear();
        const auto fix = [](PiecewiseCurve& c, float y) {
            if (c.x.size() < 2) c = PiecewiseCurve{{0.0f, 5.0f}, {y, y}};
        };
        fix(a.chance_by_base_level, 0.0f);
        fix(a.chance_by_difficulty, 0.0f);
        fix(a.cost_by_tech_availability, 1.0f);
        break;
    }
    case SpecialAbilityClass::SystemSpy: {
        // GRC 2.3.1 load-time fix-ups (0x70fd20) [H]: dependent flags force their prerequisite on (load error logged).
        const auto need = [&](uint32_t dependents, uint32_t prereq, const char* what) {
            if ((a.spy_mask & dependents) != 0 && (a.spy_mask & prereq) != prereq) {
                note(diag, "galactic: " + a.name + ": " + what);
                a.spy_mask |= prereq;
            }
        };
        need(0x10000u | 0x40u, 0x30u, "See_Most_Powerful_Ship / See_Fleet_Contents need See_Num_Fleets");
        need(0x100u, 0x80u, "See_Ground_Company_Contents needs See_Num_Ground_Companies");
        need(0x800u, 0x400u, "See_Political_Control_Breakdown needs See_Political_Control");
        need(0x8000u, 0x8u, "See_Credit_Income_Breakdown needs See_Credit_Income");
        a.spy_mask &= ~1u;  // never Planet_Ownership
        if (a.style == ActivationStyle::GalacticAutomatic) {
            if (a.spy_duration >= 0.0f) a.spy_duration = -1.0f;
        } else if (a.style == ActivationStyle::GroundActivated) {
            if (a.spy_duration <= 0.0f) a.spy_duration = 30.0f;
        } else {
            note(diag, "galactic: " + a.name + ": System_Spy_Ability needs Galactic_Automatic or Ground_Activated");
            a.style = ActivationStyle::None;
        }
        break;
    }
    case SpecialAbilityClass::ReducePrice:
    case SpecialAbilityClass::ReduceTime:
        // A2K_T01_BUILD_COST_RE 1.3 [H] (0x70af70): the fraction must be < 1.0, else 1.0 with an error; the only allowed
        // Activation_Style is Galactic_Automatic (anything else is forced to it with an error). [port] The same rules
        // for the time class (same class family, not traced separately).
        if (a.production_reduction >= 1.0f) {
            note(diag, "galactic: " + a.name + ": production reduction >= 1.0 set to 1.0");
            a.production_reduction = 1.0f;
        }
        if (have_style && a.style != ActivationStyle::GalacticAutomatic)
            note(diag, "galactic: " + a.name + ": production ability needs Galactic_Automatic");
        a.style = ActivationStyle::GalacticAutomatic;
        break;
    case SpecialAbilityClass::Other: break;
    }
    return a;
}

// a2m r2: one Bounty_On_Faction_Ability element (BountyAbilityDef). Target names resolve through the loaded factions.
BountyAbilityDef parse_bounty_ability(const objects::ObjectTypeDatabase& raw, objects::FieldIndex f, const GalacticRules& rules) {
    const objects::FieldColumns& fc = raw.fields();
    BountyAbilityDef a;
    for (uint32_t i = 0; i < fc.attr_count[f]; ++i) {
        const objects::XmlAttr& at = raw.attributes()[fc.attr_begin[f] + i];
        if (ascii_iequals(at.name, "Name")) a.name = std::string(ascii_trim(at.value));
    }
    for (objects::FieldIndex g = f + 1; g < fc.size() && fc.depth[g] > fc.depth[f]; ++g) {
        if (fc.parent[g] != f) continue;
        const std::string_view tag = raw.tag_name(g);
        const std::string_view v = ascii_trim(fc.text[g]);
        if (v.empty()) continue;
        if (ascii_iequals(tag, "Bounty_Percentage_Amount")) a.percentage = to_f(v);
        else if (ascii_iequals(tag, "Stacking_Category")) a.stacking_category = static_cast<int32_t>(to_f(v));
        else if (ascii_iequals(tag, "Apply_To_All_Allies")) a.apply_to_all_allies = objects::parse_type_bool(v);
        else if (ascii_iequals(tag, "Target_Faction_Names")) {
            a.target_factions.clear();
            for (std::string_view n : objects::split_type_list(v)) {
                const uint32_t fi = rules.find_faction(n);
                if (fi != kNoIndex && std::find(a.target_factions.begin(), a.target_factions.end(), fi) == a.target_factions.end())
                    a.target_factions.push_back(fi);
            }
        }
    }
    return a;
}

// The applicable set of a production ability (0x48d070 inputs): Applicable_Unit_Categories (GameObjectCategoryType mask,
// "All" = every bit), Applicable_Unit_Types and Excluded_Unit_Types (resolved type names; unknown names are dropped).
void parse_production_targets(const objects::GameObjectTypes& types, objects::FieldIndex f, SpecialAbilityDef& a,
                              std::vector<std::string>* diag) {
    const objects::ObjectTypeDatabase& raw = types.raw();
    const objects::FieldColumns& fc = raw.fields();
    const auto names = [&](std::string_view v) {
        std::vector<uint32_t> out;
        for (std::string_view n : objects::split_type_list(v)) {
            const objects::TypeIndex r = types.resolve_name(ascii_trim(n));
            if (r != objects::kNone) out.push_back(r);
            else note(diag, "galactic: " + a.name + ": unknown type " + std::string(ascii_trim(n)));
        }
        return out;
    };
    for (objects::FieldIndex g = f + 1; g < fc.size() && fc.depth[g] > fc.depth[f]; ++g) {
        if (fc.parent[g] != f) continue;
        const std::string_view tag = raw.tag_name(g);
        const std::string_view v = ascii_trim(fc.text[g]);
        if (v.empty()) continue;
        if (ascii_iequals(tag, "Applicable_Unit_Categories")) {
            const auto m = types.enums().get(objects::EnumId::GameObjectCategory).convert(v);
            if (m) a.applicable_categories = static_cast<uint64_t>(*m);
            else note(diag, "galactic: " + a.name + ": unknown Applicable_Unit_Categories " + std::string(v));
        } else if (ascii_iequals(tag, "Applicable_Unit_Types")) a.applicable_types = names(v);
        else if (ascii_iequals(tag, "Excluded_Unit_Types")) a.excluded_types = names(v);
    }
}

std::vector<std::string_view> list_or_split(const objects::GameObjectTypes& types, objects::TypeIndex t, std::string_view tag) {
    const std::span<const std::string_view> l = types.get_list(t, tag);
    std::vector<std::string_view> out(l.begin(), l.end());
    if (out.empty()) out = objects::split_type_list(types.get_string(t, tag));
    return out;
}

std::vector<uint32_t> type_list(const objects::GameObjectTypes& types, objects::TypeIndex t, std::string_view tag) {
    std::vector<uint32_t> out;
    for (std::string_view n : list_or_split(types, t, tag)) {
        const objects::TypeIndex r = types.resolve_name(ascii_trim(n));
        if (r != objects::kNone) out.push_back(r);
    }
    return out;
}

void load_items(const FileProvider& provider, GalacticRules& out, std::vector<std::string>* diag) {
    const std::string path = "Data\\XML\\BlackMarketItems.xml";
    if (!provider.exists(path)) return;
    std::vector<uint8_t> bytes;
    if (!provider.read(path, bytes)) {
        note(diag, "galactic: unable to open " + path);
        return;
    }
    const data::XmlDocument doc = data::XmlDocument::parse(bytes, path);
    if (!doc.ok()) {
        note(diag, "galactic: unparsable " + path);
        return;
    }
    for (data::XmlNode n : doc.root().children()) {  // every child, name not checked (vanilla Item, TR BlackMarketItem)
        BlackMarketItemDef d;
        d.name = std::string(ascii_trim(n.attribute("Name")));
        d.name_crc = special_name_crc(d.name);
        auto f = [&](std::string_view tag, float fallback) {
            const std::string_view v = n.child_text(tag);
            return v.empty() ? fallback : to_f(v);
        };
        d.uncommon_price_multiplier = f("Uncommon_Price_Multiplier", 0.0f);
        const std::string_view fac = n.child_text("Faction");
        if (!fac.empty()) {
            d.faction = out.find_faction(ascii_trim(fac));
            if (d.faction == kNoIndex) note(diag, "galactic: black market item " + d.name + ": unknown faction " + std::string(fac));
        }
        d.tech_level = static_cast<int32_t>(f("Tech_Level", 1.0f));
        d.ability_names = name_list(n.child_text("Ability_Names"));
        for (const std::string& a : d.ability_names) d.ability_crcs.push_back(special_name_crc(a));  // no upper-casing [H]
        d.base_cost = static_cast<int32_t>(f("Base_Cost_Credits", 0.0f));
        d.tutorial_tech_level = static_cast<int32_t>(f("Tutorial_Tech_Level", 0.0f));
        d.tutorial_base_cost = static_cast<int32_t>(f("Tutorial_Base_Cost_Credits", 0.0f));
        d.icon = std::string(n.child_text("Icon_Name"));
        d.localized_name = std::string(n.child_text("Localized_Name"));
        d.localized_description = std::string(n.child_text("Localized_Description"));
        for (const std::string& u : name_list(n.child_text("Used_By_Units"))) d.used_by_units.push_back(u);
        out.special.items.push_back(std::move(d));
    }
}

}  // namespace

uint32_t special_name_crc(std::string_view name) { return crc32(name.data(), name.size()); }

ActivationStyle parse_activation_style(std::string_view text) {
    static constexpr const char* kNames[] = {"Global_Automatic", "Combat_Automatic", "Galactic_Automatic", "Space_Automatic",
                                             "Ground_Automatic", "Ground_Activated", "Hero_Detected",      "Combat_Imminent",
                                             "Special_Attack",   "Take_Damage",      "User_Input",         "Skirmish_Automatic"};
    const std::string_view t = ascii_trim(text);
    for (uint8_t i = 0; i < 12; ++i)
        if (ascii_iequals(t, kNames[i])) return static_cast<ActivationStyle>(i + 1);
    return ActivationStyle::None;
}

const TypeSpecial& SpecialRules::type(uint32_t t) const {
    static const TypeSpecial kDefault{};
    return t < types.size() ? types[t] : kDefault;
}

uint32_t SpecialRules::find_item(uint32_t crc) const {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].name_crc == crc) return static_cast<uint32_t>(i);
    return kNoIndex;
}

bool SpecialRules::production_applies(const SpecialAbilityDef& a, uint32_t t, uint64_t categories) {
    if (std::find(a.excluded_types.begin(), a.excluded_types.end(), t) != a.excluded_types.end()) return false;
    if (std::find(a.applicable_types.begin(), a.applicable_types.end(), t) != a.applicable_types.end()) return true;
    if (a.applicable_categories == ~uint64_t{0}) return true;  // "All": every type [port choice, RE open point 3]
    return (a.applicable_categories & categories) != 0;
}

uint32_t SpecialRules::find_item_by_name(std::string_view name) const {
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].name == name) return static_cast<uint32_t>(i);
    return kNoIndex;
}

void load_special_rules(const FileProvider& provider, const data::GameDatabase& db, const objects::GameObjectTypes& types,
                        GalacticRules& out, std::vector<std::string>* diagnostics) {
    SpecialRules& s = out.special;
    s = SpecialRules{};
    // GameConstants (P4 2.1, 3.2, 6.2).
    const data::GameConstants& g = db.constants();
    s.default_hero_respawn_time = g.get_float("Default_Hero_Respawn_Time", s.default_hero_respawn_time);
    s.hack_cost = g.get_float("Hack_Super_Weapon_Cost", s.hack_cost);
    if (auto t = g.get("Hack_Super_Weapon_Required_Type"); t && !ascii_trim(*t).empty()) s.hack_required_type = out.find_type(ascii_trim(*t));
    if (auto t = g.get("Hack_Super_Weapon_Particle_Effect")) s.hack_particle = std::string(ascii_trim(*t));
    if (auto t = g.get("Sabotage_Particle_Effect")) s.sabotage_particle = std::string(ascii_trim(*t));
    if (auto t = g.get("Activated_Black_Market_Ability_Names")) s.activated_black_market = name_list(*t);
    if (auto t = g.get("Activated_Sabotage_Ability_Names")) s.activated_sabotage = name_list(*t);
    if (auto t = g.get("Activated_Corrupt_Planet_Ability_Names")) s.activated_corrupt = name_list(*t);
    if (auto t = g.get("Activated_Hack_Super_Weapon_Ability_Names")) s.activated_hack = name_list(*t);
    load_items(provider, out, diagnostics);

    // Per-type fields and abilities (type index order; each sub-object element parsed once).
    const objects::ObjectTypeDatabase& raw = types.raw();
    const size_t n = types.size();
    s.types.assign(n, TypeSpecial{});
    std::map<objects::FieldIndex, uint32_t> parsed, parsed_production, parsed_bounty;  // lookup only
    for (objects::TypeIndex t = 0; t < n; ++t) {
        TypeSpecial& ts = s.types[t];
        for (objects::FieldIndex f : types.get_sub_objects(t, "Abilities")) {
            // OI galactic (STRATEGIC_GAPS 1.4 / 2.2): the galactic stealth / detection / planet destruction entries.
            const std::string_view tag = raw.tag_name(f);
            if (ascii_iequals(tag, "Galactic_Stealth_Ability")) {
                if (parse_ability(raw, f, nullptr).initially_enabled) ts.galactic_stealth = true;
            } else if (ascii_iequals(tag, "Planet_Destruction_Ability")) {
                if (t < out.types.size()) out.types[t].planet_destruction = true;
            } else if (ascii_iequals(tag, "Detect_Hero_Ability") || ascii_iequals(tag, "Hero_Detection_Ability")) {  // [M] tag name
                ts.detection_chance = 0.0f;
                const objects::FieldColumns& fc = raw.fields();
                for (objects::FieldIndex c = f + 1; c < fc.size() && fc.depth[c] > fc.depth[f]; ++c)
                    if (fc.parent[c] == f && ascii_iequals(raw.tag_name(c), "Detection_Chance")) ts.detection_chance = to_f(fc.text[c]);
            }
            if (ascii_iequals(tag, "Bounty_On_Faction_Ability")) {  // a2m r2
                auto bit = parsed_bounty.find(f);
                if (bit == parsed_bounty.end()) {
                    bit = parsed_bounty.emplace(f, static_cast<uint32_t>(s.bounty.size())).first;
                    s.bounty.push_back(parse_bounty_ability(raw, f, out));
                }
                ts.bounty.push_back(bit->second);
                continue;
            }
            const SpecialAbilityClass cls = class_of(tag);
            if (cls == SpecialAbilityClass::Other) continue;
            if (cls == SpecialAbilityClass::ReducePrice || cls == SpecialAbilityClass::ReduceTime) {
                auto pit = parsed_production.find(f);
                if (pit == parsed_production.end()) {
                    SpecialAbilityDef a = parse_ability(raw, f, diagnostics);
                    parse_production_targets(types, f, a, diagnostics);
                    if (!a.initially_enabled) continue;
                    pit = parsed_production.emplace(f, static_cast<uint32_t>(s.production.size())).first;
                    s.production.push_back(std::move(a));
                }
                ts.production.push_back(pit->second);
                continue;
            }
            auto it = parsed.find(f);
            if (it == parsed.end()) {
                it = parsed.emplace(f, static_cast<uint32_t>(s.abilities.size())).first;
                s.abilities.push_back(parse_ability(raw, f, diagnostics));
            }
            ts.abilities.push_back(it->second);
        }
        for (objects::TypeIndex m : types.get_type_refs(t, "Company_Units"))
            if (m != objects::kNone) ts.members.push_back(m);
        ts.sabotage_cost = types.get_int(t, "Sabotage_Cost_Credits");
        ts.tutorial_sabotage_cost = types.get_int(t, "Tutorial_Sabotage_Cost_Credits");
        ts.planet_vigilance = objects::parse_type_bool(types.get_string(t, "Planet_Vigilance")) || types.get_bool(t, "Planet_Vigilance");
        ts.mining_colony = types.get_bool(t, "Planet_Is_Mining_Colony");
        ts.slice_difficulty = to_f(types.get_string(t, "Planet_Slice_Difficulty_Rating"));  // REMAINING_GAPS 1.1
        ts.tech_availability = to_f(types.get_string(t, "Planet_Tech_Availability_Rating"));
        ts.slice_affinity = type_list(types, t, "Planet_Slice_Affinity_Types");
        ts.slice_cost = to_f(types.get_string(t, "Slice_Cost_Credits"));
        ts.black_market_modifier = to_f(types.get_string(t, "Planet_Black_Market_Modifier"));
        ts.destroy_when_prereq = types.get_bool(t, "Destroy_When_Used_As_Corruption_Prereq");
        ts.generic_hero = types.get_bool(t, "Is_Generic_Hero");
        ts.named_hero = types.get_bool(t, "Is_Named_Hero");
        {
            const std::string_view prev = ascii_trim(types.get_string(t, "Prev_Level_Base"));
            if (!prev.empty()) {
                const objects::TypeIndex r = types.resolve_name(prev);
                if (r != objects::kNone) ts.prev_level_base = r;
            }
        }
        {
            const std::vector<std::string_view> text = list_or_split(types, t, "Text_ID");
            if (!text.empty()) ts.text_crc = special_name_crc(text.front());
        }
        if (t < out.types.size() && out.types[t].kind == UnitKind::Planet)
            for (uint32_t slot = 1; slot <= 3; ++slot) {
                const std::string p = "Corruption_" + std::to_string(slot) + "_";
                if (ascii_trim(types.get_string(t, p + "Type")).empty()) continue;
                CorruptionChoiceDef c;
                c.slot = slot;
                c.type = static_cast<int8_t>(types.get_enum(t, p + "Type"));
                c.credit_cost = to_f(types.get_string(t, p + "Credit_Cost"));
                {  // stored as a float field: rounded half up (bkc1 t07: get_int now truncates a float field)
                    const float f = types.get_float(t, p + "Credit_Cost");
                    c.credit_cost_rounded = f > 0.0f ? static_cast<int32_t>(f + 0.5f) : types.get_int(t, p + "Credit_Cost");
                }
                c.required_unit_types = type_list(types, t, p + "Required_Unit_Type");
                c.required_unit_count = types.get_int(t, p + "Required_Unit_Count");
                if (!ascii_trim(types.get_string(t, p + "Required_Mission_Type")).empty())
                    c.required_mission = static_cast<int8_t>(types.get_enum(t, p + "Required_Mission_Type"));
                c.required_mission_count = types.get_int(t, p + "Required_Mission_Count");
                c.required_planets = type_list(types, t, p + "Required_Planets");
                if (const std::string_view u = ascii_trim(types.get_string(t, p + "Success_Unlock_Unit_Type")); !u.empty())
                    if (const objects::TypeIndex r = types.resolve_name(u); r != objects::kNone) c.success_unlock = r;
                c.tactical_map = std::string(ascii_trim(types.get_string(t, p + "Tactical_Map_Name")));
                c.tactical_plots = std::string(ascii_trim(types.get_string(t, p + "Tactical_Script_Name")));
                const bool has_mode = !ascii_trim(types.get_string(t, p + "Tactical_Game_Mode")).empty();
                const int32_t mode = types.get_enum(t, p + "Tactical_Game_Mode");  // Galactic 0, Land 1, Space 2
                c.tactical_space = mode == 2;
                c.tactical = has_mode && (mode == 1 || mode == 2) && !c.tactical_map.empty();
                ts.corruption.push_back(std::move(c));
            }
    }
    // Teams: a container without its own list uses its first member's abilities [inferred]; hero flags likewise.
    for (objects::TypeIndex t = 0; t < n; ++t) {
        TypeSpecial& ts = s.types[t];
        if (ts.members.empty()) continue;
        if (!ts.galactic_stealth) {  // OI galactic: a transport is hidden only when all its cargo is (SG 2.2)
            bool all = true;
            for (uint32_t mt : ts.members) all = all && s.types[mt].galactic_stealth;
            ts.galactic_stealth = all;
        }
        const TypeSpecial& m = s.types[ts.members.front()];
        if (ts.abilities.empty()) ts.abilities = m.abilities;
        if (ts.production.empty()) ts.production = m.production;
        ts.generic_hero = ts.generic_hero || m.generic_hero;
        ts.named_hero = ts.named_hero || m.named_hero;
        if (ts.text_crc == 0) ts.text_crc = m.text_crc;
    }
    for (TypeSpecial& ts : s.types)
        for (uint32_t a : ts.abilities) {
            const SpecialAbilityDef& d = s.abilities[a];
            if (d.cls == SpecialAbilityClass::IncomeBonus || d.cls == SpecialAbilityClass::IncomeGambling) ts.has_income_ability = true;
            if (d.cls == SpecialAbilityClass::SystemSpy && d.style == ActivationStyle::GalacticAutomatic && d.initially_enabled)
                ts.auto_spy_mask |= d.spy_mask;  // GRC 2.3 (always-on spies)
        }
}

}  // namespace alamo::galactic

#include "alamo/galactic/galactic_rules.hpp"

#include <algorithm>
#include <memory>

#include "alamo/data/game_database.hpp"
#include "alamo/core/ascii.hpp"
#include "alamo/data/xml.hpp"
#include "alamo/data/xml_list.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/enums.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::galactic {

namespace {

float atof_or(std::string_view text, float fallback) {
    for (char c : text)
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return static_cast<float>(objects::c_atof(text));
    return fallback;
}

std::array<float, 2> pair_or(std::string_view text, std::array<float, 2> fallback) {
    const std::vector<std::string_view> tok = objects::split_type_list(text);
    for (size_t i = 0; i < tok.size() && i < 2; ++i) fallback[i] = static_cast<float>(objects::c_atof(tok[i]));
    return fallback;
}

void note(std::vector<std::string>* diag, std::string msg) {
    if (diag) diag->push_back(std::move(msg));
}

std::unique_ptr<data::XmlDocument> read_xml(const FileProvider& provider, const std::string& path,
                                            std::vector<std::string>* diag) {
    std::vector<uint8_t> bytes;
    if (!provider.read(path, bytes)) {
        note(diag, "galactic: unable to open " + path);
        return nullptr;
    }
    auto doc = std::make_unique<data::XmlDocument>(data::XmlDocument::parse(bytes, path));
    if (!doc->ok()) {
        note(diag, "galactic: unparsable " + path);
        return nullptr;
    }
    return doc;
}

bool has_field(const objects::GameObjectTypes& types, objects::TypeIndex t, std::string_view tag) {
    return types.value(t, objects::find_type_field(tag)) != nullptr;
}

int32_t int_or(const objects::GameObjectTypes& types, objects::TypeIndex t, std::string_view tag, int32_t fallback) {
    return has_field(types, t, tag) ? types.get_int(t, tag) : fallback;
}

uint32_t resolve_or_none(const objects::GameObjectTypes& types, std::string_view name) {
    const objects::TypeIndex r = types.resolve_name(ascii_trim(name));
    return r == objects::kNone ? kNoIndex : static_cast<uint32_t>(r);
}

// OI galactic (STRATEGIC_GAPS 1.2 / 1.3 / 1.7): dummy behaviour, company transport, buildability fields.
void extract_oi_fields(const objects::GameObjectTypes& types, objects::TypeIndex t, TypeInfo& i) {
    i.data_rules = true;
    for (uint8_t id = kDummyGroundBase; id <= kDummyStarship; ++id)
        if (types.has_behavior(t, objects::BehaviorSet::Union, id)) {
            i.dummy = id;
            break;
        }
    i.transport_behavior = types.has_behavior(t, objects::BehaviorSet::Union, objects::beh::Transport);
    for (objects::TypeIndex r : types.get_type_refs(t, "Company_Transport_Unit")) {
        if (r != objects::kNone) i.company_transport = r;
        break;  // the first entry is used (0x400f30)
    }
    for (objects::TypeIndex m : types.get_type_refs(t, "Company_Units"))
        if (m != objects::kNone) i.company_units.push_back(m);
    i.cinematic_only = types.get_bool(t, "Cinematic_Object_Only");
    i.always_spawn_in_orbit = types.get_bool(t, "Always_Spawn_In_Orbit");
    i.super_weapon_killer = types.get_bool(t, "Is_Super_Weapon_Killer");
    i.named_hero = types.get_bool(t, "Is_Named_Hero");
    i.generic_hero = types.get_bool(t, "Is_Generic_Hero");
    i.requires_initial_placement = types.get_bool(t, "Build_Requires_Initial_Placement");
    // Required_Special_Structures: the parser keeps '|' as a token (0x53 OR markers): `A | B, C` = (A or B) and C.
    bool join = false;
    for (std::string_view tok : types.get_list(t, "Required_Special_Structures")) {
        tok = ascii_trim(tok);
        if (tok.empty()) continue;
        if (tok == "|") {
            join = true;
            continue;
        }
        const uint32_t r = resolve_or_none(types, tok);
        if (join && !i.required_special_groups.empty()) i.required_special_groups.back().push_back(r);
        else i.required_special_groups.push_back({r});
        join = false;
    }
    for (objects::TypeIndex r : types.get_type_refs(t, "Required_Orbiting_Units"))
        if (r != objects::kNone) i.required_orbiting_units.push_back(r);
    {
        const std::span<const std::string_view> lst = types.get_list(t, "Required_Planets");
        std::vector<std::string_view> names(lst.begin(), lst.end());
        if (names.empty()) names = objects::split_type_list(types.get_string(t, "Required_Planets"));
        for (std::string_view n : names)
            if (!ascii_trim(n).empty()) i.required_planets.push_back(resolve_or_none(types, n));
    }
    // Defaults when a tag is absent: -1 = unlimited [L: the type constructor values were not read].
    i.max_instances_per_planet = int_or(types, t, "Build_Max_Instances_Per_Planet", -1);
    i.limit_lifetime = int_or(types, t, "Build_Limit_Lifetime_Per_Player", -1);
    i.limit_current = int_or(types, t, "Build_Limit_Current_Per_Player", -1);
    i.max_space_base = int_or(types, t, "Max_Space_Base", i.max_space_base);
    i.ground_structure_slots = int_or(types, t, "Special_Structures_Land", -1);
    i.orbital_structure_slots = int_or(types, t, "Special_Structures_Space", -1);
}

}  // namespace

// q6 t03. The port's element-tag table (unchanged since S3).
UnitKind unit_kind_from_element(std::string_view el, bool hyperspace) {
    if (ascii_iequals(el, "StarBase")) return UnitKind::StarBase;
    if (ascii_iequals(el, "GroundStructure") || ascii_iequals(el, "GroundBase") || ascii_iequals(el, "SpecialStructure"))
        return UnitKind::GroundStructure;
    if (ascii_iequals(el, "Squadron")) return UnitKind::Squadron;
    if (ascii_iequals(el, "SpaceUnit")) return UnitKind::SpaceUnit;
    if (ascii_iequals(el, "GroundCompany") || ascii_iequals(el, "GroundInfantry") || ascii_iequals(el, "GroundVehicle") ||
        ascii_iequals(el, "HeroCompany"))
        return UnitKind::GroundUnit;
    if (ascii_iequals(el, "HeroUnit") || ascii_iequals(el, "UniqueUnit"))
        return hyperspace ? UnitKind::SpaceUnit : UnitKind::GroundUnit;  // [M]
    if (ascii_iequals(el, "Planet")) return UnitKind::Planet;
    return UnitKind::Other;
}

// The behaviour ids 0x5B..0x61 (SG 1.2). Orbital structures share GroundStructure: the port keeps one structure list
// per planet, and the vanilla SpecialStructure orbital types already mapped there through the tag table.
UnitKind unit_kind_from_dummy(uint8_t dummy) {
    switch (dummy) {
    case kDummyStarBase: return UnitKind::StarBase;
    case kDummyGroundBase:
    case kDummyGroundStructure:
    case kDummyOrbitalStructure: return UnitKind::GroundStructure;
    case kDummyGroundCompany: return UnitKind::GroundUnit;
    case kDummyFighterSquadron: return UnitKind::Squadron;
    case kDummyStarship: return UnitKind::SpaceUnit;
    default: return UnitKind::Other;
    }
}

// Vanilla FoC / EaW element tags outside the tag table whose types carry a role behaviour (FoC: 11 GroundBuildable,
// 10 SecondaryStructure, 1 SpaceBuildable, 8 SlaveCompany, 1 Marker, 38 Props_Story, 2 MOV_Cinematic, 5 CIN_SpaceUnit,
// 8 CIN_SpaceProp; EaW a subset). The port always left them Other; they stay so, so base-game behaviour does not
// change [port: the original would give them their behaviour's role; open, needs a decision].
bool is_vanilla_unclassified_element(std::string_view el) {
    static constexpr std::string_view kTags[] = {"GroundBuildable", "SecondaryStructure", "SpaceBuildable",
                                                 "SlaveCompany",    "Marker",             "Props_Story",
                                                 "MOV_Cinematic",   "CIN_SpaceUnit",      "CIN_SpaceProp"};
    for (const std::string_view t : kTags)
        if (ascii_iequals(el, t)) return true;
    return false;
}

UnitKind galactic_unit_kind(std::string_view el, bool hyperspace, uint8_t dummy) {
    const UnitKind by_tag = unit_kind_from_element(el, hyperspace);
    const UnitKind by_behavior = unit_kind_from_dummy(dummy);
    if (by_tag == UnitKind::Planet) return by_tag;
    if (by_tag != UnitKind::Other) return by_behavior != UnitKind::Other ? by_behavior : by_tag;
    return is_vanilla_unclassified_element(el) ? UnitKind::Other : by_behavior;
}

uint32_t GalacticRules::find_faction(std::string_view name) const {
    for (size_t i = 0; i < factions.size(); ++i)
        if (ascii_iequals(factions[i].name, name)) return static_cast<uint32_t>(i);
    return kNoIndex;
}

uint32_t GalacticRules::find_difficulty(std::string_view name) const {
    for (size_t i = 0; i < difficulties.size(); ++i)
        if (ascii_iequals(difficulties[i].name, name)) return static_cast<uint32_t>(i);
    return kNoIndex;
}

uint32_t GalacticRules::find_type(std::string_view name) const {
    for (size_t i = 0; i < types.size(); ++i)
        if (ascii_iequals(types[i].name, name)) return static_cast<uint32_t>(i);
    return kNoIndex;
}

uint32_t GalacticRules::neutral_faction() const {
    for (size_t i = 0; i < factions.size(); ++i)
        if (factions[i].is_neutral) return static_cast<uint32_t>(i);
    return kNoIndex;
}

uint32_t GalacticRules::native_owner_faction() const {
    for (size_t i = 0; i < factions.size(); ++i)
        if (!factions[i].is_playable && !factions[i].is_neutral && !factions[i].can_control_planets) return static_cast<uint32_t>(i);
    return kNoIndex;
}

EconomyConstants load_economy_constants(const data::GameDatabase& db) {
    EconomyConstants c;
    const data::GameConstants& g = db.constants();
    c.fiscal_cycle_seconds = g.get_float("Fiscal_Cycle_Time_In_Secs", c.fiscal_cycle_seconds);
    c.credit_cap_per_planet = g.get_float("Credit_Cap_Per_Planet", c.credit_cap_per_planet);
    c.black_market_min = g.get_float("Black_Market_Income_Mult_Min", c.black_market_min);
    c.black_market_max = g.get_float("Black_Market_Income_Mult_Max", c.black_market_max);
    if (auto t = g.get("Corruption_Choice_Income_Percentage")) {
        const std::vector<std::string_view> tok = objects::split_type_list(*t);
        c.corruption_income_pct.fill(0.0f);
        for (size_t i = 0; i < tok.size() && i < c.corruption_income_pct.size(); ++i)
            c.corruption_income_pct[i] = static_cast<float>(objects::c_atof(tok[i]));
    }
    c.credit_alignment_bonus = g.get_float("MaxCreditIncomeAlignmentBonus", c.credit_alignment_bonus);
    c.credit_alignment_penalty = g.get_float("MaxCreditIncomeAlignmentPenalty", c.credit_alignment_penalty);
    if (auto t = g.get("Political_Income_Curve")) c.political_income_curve = PiecewiseCurve::parse(*t);
    c.max_build_queue = g.get_int("Max_Build_Queue", c.max_build_queue);
    c.pay_as_you_go = g.get_bool("Pay_As_You_Go", c.pay_as_you_go);
    c.production_speed_factor = g.get_float("Production_Speed_Factor", c.production_speed_factor);
    for (int i = 1; i <= 4; ++i)
        c.base_vs_tech[static_cast<size_t>(i)] = g.get_float(
            "Production_Speed_Mod_Base_Vs_Tech_" + std::to_string(i), c.base_vs_tech[static_cast<size_t>(i)]);
    c.max_ground_forces_on_planet = g.get_int("Max_Ground_Forces_On_Planet", c.max_ground_forces_on_planet);
    c.health_low_percent_threshold = g.get_float("Health_Low_Percent_Threshold", c.health_low_percent_threshold);  // a2l t03
    {  // a2m t05: Low_ / Medium_ / High_Threat_Reachability_Tolerance (A2M_T04_RE_ANSWERS Q3)
        static constexpr const char* kTol[3] = {"Low_Threat_Reachability_Tolerance", "Medium_Threat_Reachability_Tolerance",
                                                "High_Threat_Reachability_Tolerance"};
        for (size_t i = 0; i < 3; ++i) c.threat_reachability_tolerance[i] = g.get_float(kTol[i], c.threat_reachability_tolerance[i]);
    }
    c.corruption_hyperspace_bonus = g.get_float("Corruption_Hyperspace_Bonus", c.corruption_hyperspace_bonus);
    c.political_control_change_seconds =
        g.get_float("Political_Control_Change_Time_Seconds", c.political_control_change_seconds);
    c.influence_alignment_bonus = g.get_float("MaxInfluenceTransitionAlignmentBonus", c.influence_alignment_bonus);
    c.influence_alignment_penalty =
        g.get_float("MaxInfluenceTransitionAlignmentPenalty", c.influence_alignment_penalty);
    c.ai_uses_galactic_pop_cap = g.get_bool("AI_Uses_Galactic_Pop_Cap", c.ai_uses_galactic_pop_cap);  // A2 galactic
    // OI galactic (STRATEGIC_GAPS 2.1): the galactic mode's speed multiplier M.
    c.fleet_speed_multiplier = g.get_float("Object_Max_Speed_Multiplier_Galactic", c.fleet_speed_multiplier);
    c.planet_reveal_delay_seconds = g.get_float("Planet_Reveal_Delay_Time", c.planet_reveal_delay_seconds);  // GRC 1.3
    const auto float_list = [&g](const char* tag, std::vector<float>& out) {  // GRC 4.5
        if (auto t = g.get(tag)) {
            out.clear();
            for (std::string_view tok : objects::split_type_list(*t)) out.push_back(static_cast<float>(objects::c_atof(tok)));
        }
    };
    float_list("Particle_Brightness_Per_Corruption_Level", c.particle_brightness_per_level);
    float_list("Particle_Scale_Per_Corruption_Level", c.particle_scale_per_level);
    float_list("Particle_Energy_Per_Corruption_Level", c.particle_energy_per_level);
    if (auto t = g.get("Good_Side_Name")) c.good_side_name = std::string(ascii_trim(*t));  // REMAINING_GAPS 1.5
    if (auto t = g.get("Evil_Side_Name")) c.evil_side_name = std::string(ascii_trim(*t));  // modc t20 (presentation)
    return c;
}

std::vector<FactionRules> load_factions(const FileProvider& provider, std::vector<std::string>* diagnostics) {
    std::vector<FactionRules> out;
    constexpr std::string_view kList = "Data\\XML\\FactionFiles.xml";
    data::ListedXmlOptions opt;  // every entry is opened (an empty one fails to), each failure noted
    opt.keep_empty_entries = true;
    opt.visit_missing = true;
    opt.visit_unparsable = true;
    const data::ListStatus st = data::for_each_listed_xml(provider, kList, [&](data::ListedXmlFile& file) {
        if (!file.doc || !file.doc->ok()) {
            note(diagnostics, (file.doc ? "galactic: unparsable " : "galactic: unable to open ") + file.entry.path);
            return;
        }
        for (data::XmlNode n : file.doc->root().children("Faction")) {
            FactionRules r;
            r.name = std::string(n.attribute("Name"));
            for (data::XmlNode c : n.children()) r.xml_fields.emplace_back(c.name(), c.text_trimmed());
            r.is_neutral = objects::parse_type_bool(n.child_text("Is_Neutral"));
            r.can_control_planets = objects::parse_type_bool(n.child_text("Can_Control_Planets"));
            r.is_playable = objects::parse_type_bool(n.child_text("Is_Playable"));
            r.benefits_from_corruption = objects::parse_type_bool(n.child_text("Benefits_From_Corruption"));
            r.basic_ai = std::string(ascii_trim(n.child_text("Basic_AI")));
            r.text_id = std::string(ascii_trim(n.child_text("Text_ID")));
            r.tech_tree_dialog = std::string(ascii_trim(n.child_text("Tech_Tree_Dialog_Name")));
            r.tech_level_icon = std::string(ascii_trim(n.child_text("Tech_Level_Icon_Name")));
            r.land_selection_blob = std::string(ascii_trim(n.child_text("Land_Mode_Selection_Blob_Material_Name")));
            r.space_selection_blob = std::string(ascii_trim(n.child_text("Space_Mode_Selection_Blob_Material_Name")));
            r.hyperspace_speed_factor = atof_or(n.child_text("Hyperspace_Speed_Factor"), r.hyperspace_speed_factor);
            r.maintenance_cost = atof_or(n.child_text("Maintenance_Cost"), r.maintenance_cost);
            r.force_alignment = parse_force_side(n.child_text("Force_Alignment"));
            r.corruption_time_mult = pair_or(n.child_text("Corruption_Galactic_Production_Time_Multipliers"),
                                             r.corruption_time_mult);
            r.corruption_price_mult = pair_or(n.child_text("Corruption_Galactic_Production_Price_Multipliers"),
                                              r.corruption_price_mult);
            r.base_population_capacity = static_cast<int32_t>(atof_or(n.child_text("Base_Population_Capacity"), 0.0f));
            r.displayed_tech_level_adjustment =  // P4 sim ("+1" parses as 1)
                static_cast<int32_t>(atof_or(n.child_text("Displayed_Tech_Level_Adjustment"), 0.0f));
            // OI galactic: victory monitor inputs (resolved in load_galactic_rules).
            r.home_planet = std::string(ascii_trim(n.child_text("Home_Planet")));
            r.leader_name = std::string(ascii_trim(n.child_text("Faction_Leader")));
            r.leader_company_name = std::string(ascii_trim(n.child_text("Faction_Leader_Company")));
            r.primary_enemy_name = std::string(ascii_trim(n.child_text("Primary_Enemy")));
            // S7 t30 (TACTICAL_COMBAT §2.10): the relation lists.
            const auto names = [](std::string_view s) {
                std::vector<std::string> v;
                std::string cur;
                for (const char ch : s) {
                    if (ch == ',' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                        if (!cur.empty()) v.push_back(std::move(cur));
                        cur.clear();
                    } else {
                        cur.push_back(ch);
                    }
                }
                if (!cur.empty()) v.push_back(std::move(cur));
                return v;
            };
            r.allies = names(n.child_text("Allies"));
            r.enemies = names(n.child_text("Enemies"));
            out.push_back(std::move(r));
        }
    }, opt);
    if (st == data::ListStatus::Missing) note(diagnostics, "galactic: unable to open " + std::string(kList));
    if (st == data::ListStatus::Unparsable) note(diagnostics, "galactic: unparsable " + std::string(kList));
    return out;
}

std::vector<DifficultyRules> load_difficulties(const FileProvider& provider, std::vector<std::string>* diagnostics) {
    std::vector<DifficultyRules> out;
    auto doc = read_xml(provider, "Data\\XML\\DifficultyAdjustments.xml", diagnostics);
    if (!doc) return out;
    for (data::XmlNode n : doc->root().children("Difficulty_Adjustment")) {
        DifficultyRules d;
        d.name = std::string(n.attribute("Name"));
        d.credit_multiplier = atof_or(n.child_text("Credit_Multiplier"), 1.0f);
        d.galactic_build_time_multiplier = atof_or(n.child_text("Galactic_Build_Time_Multiplier"), 1.0f);
        d.damage_multiplier = atof_or(n.child_text("Damage_Multiplier"), 1.0f);
        d.health_multiplier = atof_or(n.child_text("Health_Multiplier"), 1.0f);
        d.shield_multiplier = atof_or(n.child_text("Shield_Multiplier"), 1.0f);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<TypeInfo> extract_type_info(const objects::GameObjectTypes& types, const std::vector<FactionRules>& factions) {
    std::vector<TypeInfo> out(types.size());
    for (objects::TypeIndex t = 0; t < types.size(); ++t) {
        TypeInfo& i = out[t];
        i.name = std::string(types.raw().types().name[t]);
        i.hyperspace = types.get_bool(t, "Hyperspace");
        i.build_cost = types.get_int(t, "Build_Cost_Credits");
        i.build_time_seconds = types.get_int(t, "Build_Time_Seconds");
        i.tech_level = types.get_int(t, "Tech_Level");
        i.required_ground_base = types.get_int(t, "Required_Ground_Base_Level");
        i.required_star_base = types.get_int(t, "Required_Star_Base_Level");
        i.base_level = types.get_int(t, "Base_Level");
        i.advances_tech = types.get_int(t, "Build_Advances_Tech_Level");
        i.squadron_capacity = types.get_int(t, "Squadron_Capacity");
        i.hyperspace_speed = types.get_float(t, "Hyperspace_Speed");
        i.influence_range = types.get_float(t, "Galactic_Influence_Range");
        i.piracy_value = types.get_float(t, "Piracy_Value_Credits");
        i.maintenance_cost = types.get_float(t, "Maintenance_Cost");
        i.ai_combat_power = types.columns().ai_combat_power[t];
        i.reduced_by_factories = types.get_bool(t, "Build_Time_Reduced_By_Multiple_Factories");
        i.build_initially_locked = types.get_bool(t, "Build_Initially_Locked");
        i.sliceable = types.get_bool(t, "Build_Can_Be_Unlocked_By_Slicer");
        for (objects::TypeIndex r : types.get_type_refs(t, "Required_Special_Structures"))
            if (r != objects::kNone) i.required_special_structures.push_back(r);
        const std::span<const std::string_view> aff_list = types.get_list(t, "Affiliation");
        std::vector<std::string_view> aff(aff_list.begin(), aff_list.end());
        if (aff.empty()) aff = objects::split_type_list(types.get_string(t, "Affiliation"));
        for (std::string_view a : aff)
            for (size_t f = 0; f < factions.size(); ++f)
                if (ascii_iequals(factions[f].name, a)) {
                    i.affiliation.push_back(static_cast<uint32_t>(f));
                    break;
                }
        i.credit_value = types.get_int(t, "Planet_Credit_Value");
        i.destroyed_credit_value = types.get_int(t, "Planet_Destroyed_Credit_Value");
        i.capture_bonus = types.get_int(t, "Planet_Capture_Bonus_Reward");
        i.black_market_influences = types.get_bool(t, "Planet_Black_Market_Influences");
        i.starbase_stealth = types.get_bool(t, "Planet_Starbase_Stealth");
        i.groundbase_stealth = types.get_bool(t, "Planet_Groundbase_Stealth");
        i.surface_accessible =
            types.value(t, objects::find_type_field("Planet_Surface_Accessible")) == nullptr
                ? true  // ASSUMPTION: an unset Planet_Surface_Accessible means accessible
                : types.get_bool(t, "Planet_Surface_Accessible");
        i.the_force = parse_force_side(types.get_string(t, "The_Force"));
        i.force_strength = types.get_float(t, "Force_Strength");
        // A2 galactic (AI_GALACTIC_3 §3, §6).
        i.population = types.columns().population_value[t];
        i.population_capacity = types.get_int(t, "Additional_Population_Capacity");
        i.categories = types.get_mask(t, "CategoryMask");
        i.movement_class = types.get_enum(t, "MovementClass");
        for (std::string_view m : types.get_list(t, "Company_Units")) {  // a company: its first member's class
            const objects::TypeIndex mt = types.resolve_name(ascii_trim(m));
            if (mt != objects::kNone) i.movement_class = types.get_enum(mt, "MovementClass");
            break;
        }
        {
            const std::string terrain(types.get_string(t, "Terrain"));
            i.terrain = terrain_id(ascii_trim(terrain));
        }
        i.restricted_categories = types.get_mask(t, "Planet_Restricted_Unit_Categories");
        for (objects::TypeIndex r : types.get_type_refs(t, "Planet_Restricted_Unit_Types"))
            if (r != objects::kNone) i.restricted_types.push_back(r);
        extract_oi_fields(types, t, i);  // OI galactic (STRATEGIC_GAPS 1.2 / 1.3 / 1.7)
        i.kind = galactic_unit_kind(types.raw().element_name(t), i.hyperspace, i.dummy);  // q6 t03: needs i.dummy
    }
    // Company / squadron membership flags (type derivation 0x377500: +0x2A / +0x29 on every listed member).
    for (objects::TypeIndex t = 0; t < types.size(); ++t) {
        for (uint32_t m : out[t].company_units)
            if (m < out.size()) out[m].company_member = true;
        for (objects::TypeIndex m : types.get_type_refs(t, "Squadron_Units"))
            if (m != objects::kNone && m < out.size()) out[m].squadron_member = true;
    }
    return out;
}

int32_t terrain_id(std::string_view name) {
    for (int32_t i = 0; i < 8; ++i)
        if (ascii_iequals(name, kTerrainNames[i])) return i;
    return -1;
}

double TerrainEffectiveness::get(int32_t terrain, int32_t movement_class) const {
    if (terrain < 0 || movement_class < 0 || static_cast<size_t>(terrain) >= value.size()) return 1.0;
    const std::vector<double>& row = value[static_cast<size_t>(terrain)];
    return static_cast<size_t>(movement_class) < row.size() ? row[static_cast<size_t>(movement_class)] : 1.0;
}

TerrainEffectiveness load_terrain_effectiveness(const FileProvider& provider, const objects::GameObjectTypes& types,
                                                std::vector<std::string>* diagnostics) {
    TerrainEffectiveness out;
    if (!provider.exists("Data\\XML\\AITerrainEffectiveness.xml")) return out;
    auto doc = read_xml(provider, "Data\\XML\\AITerrainEffectiveness.xml", diagnostics);
    if (!doc) return out;
    const objects::EnumConverter& mc = types.enums().get(objects::EnumId::MovementClass);
    for (data::XmlNode terr : doc->root().children()) {
        const int32_t tid = terrain_id(terr.name());
        if (tid < 0) {
            note(diagnostics, "galactic: AITerrainEffectiveness: unknown terrain " + std::string(terr.name()));
            continue;
        }
        if (out.value.size() <= static_cast<size_t>(tid)) out.value.resize(static_cast<size_t>(tid) + 1);
        for (data::XmlNode cls : terr.children()) {
            const auto id = mc.lookup(cls.name());
            if (!id || *id < 0) {
                note(diagnostics, "galactic: AITerrainEffectiveness: unknown movement class " + std::string(cls.name()));
                continue;
            }
            std::vector<double>& row = out.value[static_cast<size_t>(tid)];
            if (row.size() <= static_cast<size_t>(*id)) row.resize(static_cast<size_t>(*id) + 1, 1.0);
            row[static_cast<size_t>(*id)] = objects::c_atof(cls.text_trimmed());
        }
    }
    return out;
}

bool load_galactic_rules(const FileProvider& provider, const data::GameDatabase& db,
                         const objects::GameObjectTypes& types, GalacticRules& out,
                         std::vector<std::string>* diagnostics) {
    out.constants = load_economy_constants(db);
    out.factions = load_factions(provider, diagnostics);
    out.difficulties = load_difficulties(provider, diagnostics);
    out.types = extract_type_info(types, out.factions);
    out.terrain_effectiveness = load_terrain_effectiveness(provider, types, diagnostics);
    load_special_rules(provider, db, types, out, diagnostics);  // P4 sim
    // OI galactic: faction victory inputs (STRATEGIC_GAPS 1.4).
    for (FactionRules& f : out.factions) {
        if (!f.home_planet.empty()) f.home_planet_type = out.find_type(f.home_planet);
        if (!f.leader_name.empty()) f.leader = out.find_type(f.leader_name);
        if (!f.leader_company_name.empty()) f.leader_company = out.find_type(f.leader_company_name);
        if (!f.primary_enemy_name.empty()) f.primary_enemy = out.find_faction(f.primary_enemy_name);
    }
    return !out.factions.empty();
}

}  // namespace alamo::galactic

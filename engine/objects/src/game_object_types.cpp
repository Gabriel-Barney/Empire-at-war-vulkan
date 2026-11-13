#include "alamo/objects/game_object_types.hpp"

#include <algorithm>
#include <cstdint>

#include "alamo/core/hash.hpp"
#include "alamo/core/log.hpp"
#include "alamo/data/values.hpp"
#include "alamo/data/xml_list.hpp"

namespace alamo::objects {

namespace {

using Def = std::pair<std::string_view, std::string_view>;
const Def kDefaults[] = {
    // §4.6 bools true by default.
    {"Is_Editor_Placed", "1"}, {"Cache_Crusher_Boxes", "1"}, {"Movie_Object", "1"}, {"Is_Homogeneous", "1"},
    {"Is_Discardable", "1"}, {"Is_Valid_Target", "1"}, {"Model_Visible_To_Enemy", "1"},
    {"Can_Participate_In_Space_Battle", "1"}, {"Can_Hyperspace_Without_Activating_Ability", "1"},
    {"Projectile_Does_Shield_Damage", "1"}, {"Projectile_Does_Hitpoint_Damage", "1"},
    {"Is_Visible_On_Enemy_Radar", "1"}, {"Radar_Show_Facing", "1"}, {"Influences_Cash_Point", "1"},
    {"Influences_Capture_Point", "1"}, {"Should_Cause_Limited_Turrets_To_Turn", "1"},
    {"Targeting_Allowed_When_Burning", "1"}, {"Targeting_Land_Model_Stay_Horiz_Flat", "1"},
    {"Can_Indigenous_Unit_Stop", "1"}, {"Is_Indigenous_Unit_Corruptible", "1"}, {"Shield_Requires_Base_Power", "1"},
    {"TSW_Start_Pos_Match_Targets_Z", "1"}, {"Death_Clone_Is_Obstacle", "1"}, {"Death_Leave_Hulk_Behind", "1"},
    {"Fine_Tune_Occupied_Passability", "1"}, {"Display_Contained_Hero_Grab_Bars", "1"},
    {"Is_Branched_Map_Discardable", "1"}, {"Should_Be_Destroyed_When_All_Hardpoints_Destroyed", "1"},
    {"Fire_Weapon_When_Deployed", "1"}, {"Fire_Weapon_When_Undeployed", "1"},
    {"Fire_Weapon_When_In_Rocket_Attack_Mode", "1"}, {"Fire_Weapon_When_In_Normal_Attack_Mode", "1"},
    {"Planet_Surface_Accessible", "1"}, {"Planet_Retains_Residual_Influence", "1"},
    {"Planet_Is_Destroyable_By_TSW", "1"}, {"Destroy_Previous_Upgrade_Level", "1"},
    {"Show_In_Sidebar_While_Building", "1"}, {"Show_In_Sidebar_When_Complete", "1"}, {"Show_In_Sidebar_Tray", "1"},
    {"Tactically_Built_Child_Object_Persists", "1"}, {"Auto_Deploys", "1"}, {"Should_Reinforcements_Move", "1"},
    {"Use_Transported_Object_Bounds_For_Landing_Z", "1"}, {"Terrain_Texture_Modifier_Square", "1"},
    {"Destroy_When_Used_As_Corruption_Prereq", "1"}, {"Respects_Skirmish_Pre_Build_Bases_Option", "1"},
    // §4.6 / §5.4 numeric and string defaults.
    {"Tactical_Health", "100.0"}, {"Max_Speed", "0.25"}, {"Max_Rate_Of_Turn", "2.3"}, {"Mass", "0.995"},
    {"Scale_Factor", "1.0"}, {"LOD_Bias", "4.0"}, {"Armor_Type", "Armor_Default"},
    {"Shield_Armor_Type", "Shield_Default"}, {"Damage_Type", "Damage_Default"}, {"Max_Squad_Size", "10"},
    {"Ranking_In_Category", "25"}, {"Build_Limit_Current_Per_Player", "-1"}, {"Build_Limit_Lifetime_Per_Player", "-1"},
    {"Build_Limit_Current_For_All_Allies", "-1"}, {"Build_Limit_Lifetime_For_All_Allies", "-1"},
    {"Build_Max_Instances_Per_Planet", "-1"}, {"Tactical_Build_Cost_Campaign", "-1"},
    {"Space_Override_Population_Value", "-1"}, {"Land_Override_Population_Value", "-1"},
    {"Select_Box_Z_Adjust", "-1.0"}, {"Tactical_Sell_Percentage", "0.5"}, {"Spawned_Squadron_Delay_Seconds", "5.0"},
    {"Projectile_Fire_Pulse_Count", "1"}, {"Projectile_Fire_Pulse_Delay_Seconds", "1.0"},
    {"Projectile_Max_Flight_Distance", "50.0"}, {"Alternate_Max_Speed", "1.0"}, {"Alternate_Max_Rate_Of_Turn", "2.0"},
    {"Max_Thrust", "0.015"}, {"Max_Rate_Of_Roll", "2.0"}, {"Bank_Turn_Angle", "70.0"}, {"Max_Lift", "0.8"},
    {"Stopping_Distance", "2.0"},
    {"Movement_Animation_Speed", "0.25"}, {"Walk_Animation_Speed", "0.25"}, {"Crouch_Animation_Speed", "0.25"},
    {"Dense_FOW_Reveal_Range_Multiplier", "0.5"},
    // Enums whose constructor value is -1 ("none"); kept as FieldValue::i = -1 with no name.
    {"MovementClass", "-1"}, {"Ship_Class", "-1"}, {"OccupationStyle", "-1"}, {"UnitCollisionClass", "-1"},
    {"Weather_Category", "-1"}, {"Tactical_Production_Queue", "-1"}, {"Projectile_Category", "-1"},
};

const FieldId kNoField = 0xFFFFFFFFu;
constexpr std::string_view kStarshipCombatant = "STARSHIP_COMBATANT";

struct Ids {
    FieldId variant, build_cost, base_level, named_hero, behavior, req_ground, req_star, weather, company_units,
        squadron_units, company_transport, category_mask, property_flags, garrison_category, ai_power,
        hyperspace_speed, max_speed, turn_rate, min_speed, min_speed_fraction, hyperspace, hardpoints, affiliation,
        min_attack, max_attack, health, shield, energy, mass, scale, build_time, tech, score, population, land_fow,
        space_fow, is_decoration, damage, galactic_behavior, land_behavior, space_behavior, create_team,
        max_squad_size, space_pop, land_pop, place_marker, priority_set;
    Ids() {
        auto id = [](std::string_view t) { return find_type_field(t); };
        variant = id("Variant_Of_Existing_Type");
        build_cost = id("Build_Cost_Credits");
        base_level = id("Base_Level");
        named_hero = id("Is_Named_Hero");
        behavior = id("Behavior");
        req_ground = id("Required_Ground_Base_Level");
        req_star = id("Required_Star_Base_Level");
        weather = id("Weather_Category");
        company_units = id("Company_Units");
        squadron_units = id("Squadron_Units");
        company_transport = id("Company_Transport_Unit");
        category_mask = id("CategoryMask");
        property_flags = id("Property_Flags");
        garrison_category = id("Garrison_Category");
        ai_power = id("AI_Combat_Power");
        hyperspace_speed = id("Hyperspace_Speed");
        max_speed = id("Max_Speed");
        turn_rate = id("Max_Rate_Of_Turn");
        min_speed = id("Min_Speed");
        min_speed_fraction = id("Min_Speed_Fraction_For_Turn");
        hyperspace = id("Hyperspace");
        hardpoints = id("HardPoints");
        affiliation = id("Affiliation");
        min_attack = id("Targeting_Min_Attack_Distance");
        max_attack = id("Targeting_Max_Attack_Distance");
        health = id("Tactical_Health");
        shield = id("Shield_Points");
        energy = id("Energy_Capacity");
        mass = id("Mass");
        scale = id("Scale_Factor");
        build_time = id("Build_Time_Seconds");
        tech = id("Tech_Level");
        score = id("Score_Cost_Credits");
        population = id("Population_Value");
        land_fow = id("Land_FOW_Reveal_Range");
        space_fow = id("Space_FOW_Reveal_Range");
        is_decoration = id("Is_Decoration");
        damage = id("Damage");
        galactic_behavior = id("GalacticBehavior");
        land_behavior = id("LandBehavior");
        space_behavior = id("SpaceBehavior");
        create_team = id("Create_Team");
        max_squad_size = id("Max_Squad_Size");
        space_pop = id("Space_Override_Population_Value");
        land_pop = id("Land_Override_Population_Value");
        place_marker = id("Place_Other_Type_At_Every_Specific_Marker_Position");
        priority_set = id("Targeting_Priority_Set");
    }
};

const Ids& ids() {
    static const Ids i;
    return i;
}

const FieldValue kZero{};

bool list_has(const std::vector<std::string_view>& l, std::string_view s) {
    for (std::string_view x : l)
        if (iequals(x, s)) return true;
    return false;
}

// Current mode type while the type database loads (§9.5): no mode exists yet -> -1.
constexpr int32_t kLoadModeType = -1;

// Squadron speed factor of §9.5: Object_Max_Speed_Multiplier_<mode> x 1.0 for modes 0..2, else
// 1.0. At load (mode -1) it is exactly 1.0, so no GameConstants value is needed.
float squadron_speed_factor(int32_t mode_type) {
    (void)mode_type;  // modes 0..2 would read GameConstants; the loader only runs at mode -1
    return 1.0f;
}

}  // namespace

std::span<const std::pair<std::string_view, std::string_view>> type_field_defaults() {
    return {kDefaults, std::size(kDefaults)};
}

void GameObjectTypes::warn(std::string message) {
    log::warnf("objects", "%s", message.c_str());
    if (diag_) diag_->push_back(std::move(message));
}

const FieldValue* GameObjectTypes::find(const State& s, FieldId f) const {
    const auto it = std::lower_bound(s.begin(), s.end(), f, [](const FieldValue& v, FieldId k) { return v.field < k; });
    return (it != s.end() && it->field == f) ? &*it : nullptr;
}

FieldValue& GameObjectTypes::slot(State& s, FieldId f) {
    auto it = std::lower_bound(s.begin(), s.end(), f, [](const FieldValue& v, FieldId k) { return v.field < k; });
    if (it != s.end() && it->field == f) return *it;
    FieldValue nv = defaults_[f];  // a field starts from its constructor default
    nv.field = f;
    return *s.insert(it, std::move(nv));
}

bool GameObjectTypes::convert_enum(State& s, FieldId f, std::string_view text) {
    const TypeField& tf = type_fields()[f];
    if (tf.code == 0x32) {  // Ship_Class: unknown -> unchanged, no log
        const std::optional<int32_t> v = ship_class_value(text);
        if (v) {
            FieldValue& fv = slot(s, f);
            fv.i = *v;
            fv.text = text;
            if (!reparse_) ++stats_.enum_converted;
        }
        return true;
    }
    if (tf.code == 0x4a) {  // Projectile_Category: unknown -> -1
        FieldValue& fv = slot(s, f);
        fv.i = projectile_category_value(text);
        fv.text = text;
        if (!reparse_) ++stats_.enum_converted;
        return true;
    }
    if (tf.code == 0x52) {  // combat-mod enum (TACTICAL_GAPS §6.5): first token; unknown -> unchanged, no log
        const std::optional<int32_t> v = combat_mod_value(text);
        if (v) {
            FieldValue& fv = slot(s, f);
            fv.i = *v;
            fv.text = text;
            if (!reparse_) ++stats_.combat_mod_converted;
        } else if (!reparse_) {
            ++stats_.combat_mod_unknown;
        }
        return true;
    }
    if (tf.code == 0x43) return false;  // plain string, resolved at use time (TACTICAL_GAPS §6.5)
    // Name-to-index converters outside the EnumRegistry (§9.1.3): one name, unknown = failure.
    const auto convert_name = [&](int32_t value, uint32_t& converted, uint32_t& failed) {
        if (value < 0) {
            if (!reparse_) {
                ++failed;
                warn("Error: Class data member <" + std::string(tf.tag) + ">: Failed to convert value (" +
                     std::string(text) + ")!");
            }
            return true;  // the field keeps its previous value
        }
        FieldValue& fv = slot(s, f);
        fv.i = value;
        if (tf.conv == Conv::Mask) fv.items = split_type_list(text, false, true);
        fv.text = text;
        if (!reparse_) ++converted;
        return true;
    };
    if (f == ids().priority_set && have_priority_sets_) {
        int32_t k = 0;
        while (static_cast<size_t>(k) < priority_set_name_.size() && !iequals(priority_set_name_[static_cast<size_t>(k)], text)) ++k;
        if (static_cast<size_t>(k) == priority_set_name_.size()) k = -1;
        return convert_name(k, stats_.priority_set_converted, stats_.priority_set_failed);
    }
    if (anim_lookup_ && tf.code == 0xe && is_anim_type_field(tf.tag))
        return convert_name(anim_lookup_(text), stats_.anim_converted, stats_.anim_failed);
    const std::optional<EnumId> id = enum_of_type_field(tf.tag);
    if (!id) return false;
    const EnumConverter& c = enums_.get(*id);
    const bool is_mask = tf.conv == Conv::Mask || c.bitfield;
    const std::optional<int64_t> r = c.convert(text);
    if (!r) {
        if (!reparse_) {
            ++(is_mask ? stats_.mask_failed : stats_.enum_failed);
            warn("Error: Class data member <" + std::string(tf.tag) + ">: Failed to convert value (" +
                 std::string(text) + ")!");
        }
        return true;  // the field keeps its previous value
    }
    FieldValue& fv = slot(s, f);
    if (tf.conv == Conv::Mask) {
        fv.mask = static_cast<uint64_t>(*r);
        fv.items = split_type_list(text, false, true);
    } else {
        const uint32_t low = static_cast<uint32_t>(static_cast<uint64_t>(*r) & 0xFFFFFFFFu);  // code 0xe: 32 bits
        fv.i = static_cast<int32_t>(low);
        if (c.bitfield) fv.mask = low;
    }
    fv.text = text;
    if (!reparse_) ++(is_mask ? stats_.mask_converted : stats_.enum_converted);
    return true;
}

void GameObjectTypes::apply(State& s, FieldId f, std::string_view text, bool variant) {
    const TypeField& tf = type_fields()[f];
    if (tf.conv == Conv::Enum || tf.conv == Conv::Mask) {
        if (convert_enum(s, f, text)) return;
        // No reproduced converter: keep the name (Enum) / the tokens (Mask) only.
        FieldValue& v = slot(s, f);
        if (tf.conv == Conv::Mask) v.items = split_type_list(text, false, true);
        v.text = text;
        return;
    }
    FieldValue& v = slot(s, f);
    switch (tf.conv) {
        case Conv::Bool: v.b = parse_type_bool(text); break;
        case Conv::Int32: v.i = c_atoi(text); break;
        case Conv::Int8: v.i = static_cast<int8_t>(static_cast<uint8_t>(static_cast<uint32_t>(c_atoi(text)) & 0xFFu)); break;
        case Conv::UIntClamp100: {
            const int32_t x = c_atoi(text);
            v.i = static_cast<uint32_t>(x) > 100u ? 100 : x;
            if (v.i != x) ++stats_.clamped;
            break;
        }
        case Conv::Float: v.f = static_cast<float>(c_atof(text)); break;
        case Conv::Float01:
        case Conv::Double01: {
            double x = c_atof(text);
            if (x < 0.0 || x > 1.0) {
                ++stats_.clamped;
                x = x < 0.0 ? 0.0 : 1.0;
            }
            v.d = x;
            v.f = static_cast<float>(x);
            break;
        }
        case Conv::Double: v.d = c_atof(text); break;
        case Conv::Float2:
        case Conv::Vec3:
        case Conv::Float4:
        case Conv::Rgba: {
            const size_t n = tf.conv == Conv::Float2 ? 2 : (tf.conv == Conv::Vec3 ? 3 : 4);
            const auto toks = split_type_list(text);
            for (size_t k = 0; k < n && k < toks.size(); ++k)
                v.v[k] = tf.conv == Conv::Rgba ? static_cast<float>(c_atoi(toks[k])) : static_cast<float>(c_atof(toks[k]));
            break;
        }
        case Conv::Enum:
        case Conv::Mask:
        case Conv::String:
        case Conv::StringUpper:
        case Conv::TypeRef:
        case Conv::SubObjects: break;
        case Conv::Tokens: {
            if (text.size() >= 8192) {
                ++stats_.long_lists_ignored;
                return;
            }
            if (tf.merge == Merge::ClearList && variant) v.items.clear();
            if (tf.merge == Merge::TypeRefList && v.inherited) {
                v.items.clear();
                v.inherited = false;
            }
            const auto toks = split_type_list(text, tf.code == 0x53);
            if (tf.code == 0x22 || tf.code == 0x3c) {
                // §9.6: one (first token, second token) pair per occurrence, appended (also in
                // variants). A missing second token is stored empty so the list stays paired.
                if (!toks.empty()) {
                    v.items.push_back(toks[0]);
                    v.items.push_back(toks.size() > 1 ? toks[1] : std::string_view{});
                }
            } else {
                for (std::string_view tok : toks) v.items.push_back(tok);
            }
            break;
        }
    }
    v.text = text;
}

void GameObjectTypes::parse(TypeIndex t, bool variant) {
    const ObjectTypeDatabase& db = *raw_;
    const FieldColumns& fc = db.fields();
    const TypeColumns& ty = db.types();
    State& s = state_[t];
    uint32_t skip_depth = 0xFFFFFFFFu;  // inside a sub-object list: skip deeper elements
    // Conversion counters / errors only on the first parse: a variant re-parse re-applies the
    // child's own tags, which pass 0 already converted (and logged) once.
    reparse_ = variant;
    for (FieldIndex f = ty.field_begin[t]; f < ty.field_end[t]; ++f) {
        if (fc.depth[f] > skip_depth) continue;
        skip_depth = 0xFFFFFFFFu;
        const std::string_view tag = db.tag_name(f);
        const std::string_view text = fc.text[f];
        bool sub_list = false;
        if (fc.child_count[f] > 0)
            for (uint32_t a = 0; a < fc.attr_count[f]; ++a) {
                const XmlAttr& at = db.attributes()[fc.attr_begin[f] + a];
                if (iequals(at.name, "SubObjectList") && at.value == "Yes") sub_list = true;
            }
        if (sub_list) {
            skip_depth = fc.depth[f];
            const FieldId id = find_type_field(tag);
            if (id == kNoField || type_fields()[id].conv != Conv::SubObjects) {
                if (variant) continue;
                ++stats_.subobject_unprocessed;
                warn("Unprocessed sub-object list '" + std::string(tag) + "' in object '" + std::string(ty.name[t]) + "'");
                continue;
            }
            FieldValue& v = slot(s, id);
            if (variant) v.sub.clear();
            for (FieldIndex c = f + 1; c < ty.field_end[t] && fc.depth[c] > fc.depth[f]; ++c)
                if (fc.parent[c] == f) v.sub.push_back(c);
            v.text = tag;
            if (!variant) ++stats_.subobject_lists;
            continue;
        }
        if (text == "TBD") {
            if (variant) continue;
            ++stats_.tbd;
            warn("Warning: GameObjectTypeClass::Parse_Database_Entry() - " + std::string(ty.name[t]) + " key " +
                 std::string(tag) + " value is TBD.");
            continue;  // children (if any) are still descended into
        }
        if (text.empty()) {
            if (!variant && fc.child_count[f] == 0) ++stats_.empty;
            continue;
        }
        const FieldId id = find_type_field(tag);
        if (id == kNoField) {
            if (!variant) ++stats_.tags_ignored;
            continue;
        }
        if (type_fields()[id].conv == Conv::SubObjects) continue;  // only reachable through SubObjectList="Yes"
        if (!variant) ++stats_.tags_applied;
        apply(s, id, text, variant);
    }
    reparse_ = false;
}

void GameObjectTypes::rebuild_bits(TypeIndex t) {
    // §9.3: all five cleared, then each list's names set their id in that list's set and in Union.
    const Ids& I = ids();
    for (auto& b : bits_) b[t] = BehaviorBits{};
    const std::pair<FieldId, BehaviorSet> lists[] = {{I.galactic_behavior, BehaviorSet::Galactic},
                                                     {I.land_behavior, BehaviorSet::Land},
                                                     {I.space_behavior, BehaviorSet::Space},
                                                     {I.behavior, BehaviorSet::Generic}};
    for (const auto& [field, set] : lists) {
        if (field == kNoField) continue;
        for (std::string_view n : effective(t, field).items) {
            const BehaviorTypeId id = behavior_type_id(n);
            if (id == kNoBehavior) continue;
            bits_[static_cast<size_t>(set)][t].set(id);
            bits_[static_cast<size_t>(BehaviorSet::Union)][t].set(id);
        }
    }
}

void GameObjectTypes::end_top_level_parse(TypeIndex t, const BehaviorBits& generic_before) {
    // §9.3 implicit STARSHIP_COMBATANT. The PROJECTILE / PARTICLE test reads the bitset from BEFORE
    // this parse (empty on a first parse, the parent's in a variant); no duplicate check.
    const Ids& I = ids();
    const auto& e = [&](FieldId f) -> const FieldValue& { return effective(t, f); };
    if (!e(I.is_decoration).b && !generic_before.test(beh::Projectile) && !generic_before.test(beh::Particle) &&
        (e(I.damage).f > 0.0f || e(I.health).f > 0.0f || e(I.shield).f > 0.0f || e(I.ai_power).f > 0.0f)) {
        slot(state_[t], I.behavior).items.push_back(kStarshipCombatant);
        ++stats_.implicit_combatant_appends;
    }
    rebuild_bits(t);
}

void GameObjectTypes::fixup(TypeIndex t) {
    // §6.1 build-level normalisation.
    const Ids& I = ids();
    const auto& e = [&](FieldId f) -> const FieldValue& { return effective(t, f); };
    const int32_t cost = e(I.build_cost).i;
    if (e(I.base_level).i == 0 && (cost > 0 || cost == -1) && !e(I.named_hero).b &&
        !list_has(e(I.behavior).items, "DUMMY_GROUND_STRUCTURE")) {
        const int32_t g = e(I.req_ground).i, st = e(I.req_star).i;
        if (g == 0 && st == 0) {
            slot(state_[t], I.req_ground).i = 1;
            ++stats_.base_level_normalised;
        } else if (g != 0 && st != 0) {
            slot(state_[t], I.req_star).i = 0;
            ++stats_.base_level_normalised;
        }
    }
}

std::string_view GameObjectTypes::variant_name(TypeIndex t) const {
    const FieldValue* v = find(state_[t], ids().variant);
    return v ? v->text : std::string_view{};
}

TypeIndex GameObjectTypes::resolve_name(std::string_view name) const {
    if (name.empty() || iequals(name, "None")) return kNone;
    return raw_->find(name);
}

TypeIndex GameObjectTypes::variant_parent(TypeIndex t) const {
    const std::string_view n = variant_name(t);
    return n.empty() ? kNone : raw_->find(n);
}

bool GameObjectTypes::try_resolve(TypeIndex t, uint8_t pass) {
    if (resolved_[t]) return true;
    const TypeIndex p = variant_parent(t);
    if (p == kNone || !resolved_[p]) return false;  // missing, cycle, self, or parent not ready
    State copy = state_[p];
    for (FieldValue& v : copy)
        if (type_fields()[v.field].merge == Merge::TypeRefList) v.inherited = true;
    state_[t] = std::move(copy);
    for (auto& b : bits_) b[t] = b[p];  // bitsets copied from the parent, rebuilt after the re-parse
    const BehaviorBits before = bits_[static_cast<size_t>(BehaviorSet::Generic)][t];
    parse(t, true);
    end_top_level_parse(t, before);
    fixup(t);
    resolved_[t] = 1;
    resolve_pass_[t] = pass;
    return true;
}

void GameObjectTypes::derive(TypeIndex t) {
    const Ids& I = ids();
    State& s = state_[t];
    // 1. Weather_Category -1 -> 0.
    if (effective(t, I.weather).i == -1 && I.weather != kNoField) slot(s, I.weather).i = 0;
    const std::vector<std::string_view> company = effective(t, I.company_units).items;
    const std::vector<std::string_view> squadron = effective(t, I.squadron_units).items;
    // 2. Negative cost = sum of members (first transport entry, every company entry, every squadron entry).
    if (effective(t, I.build_cost).i < 0) {
        uint32_t sum = 0;
        auto add = [&](std::string_view n) {
            const TypeIndex m = resolve_name(n);
            if (m != kNone) sum += static_cast<uint32_t>(effective(m, I.build_cost).i);
        };
        const auto& tr = effective(t, I.company_transport).items;
        if (!tr.empty()) add(tr.front());
        for (std::string_view n : company) add(n);
        for (std::string_view n : squadron) add(n);
        slot(s, I.build_cost).i = static_cast<int32_t>(sum);
        ++stats_.cost_derived;
    }
    // Masks replaced by the members' bit OR (items: union of the members' tokens, as written).
    auto union_masks = [&](const std::vector<TypeIndex>& members, FieldId f) {
        std::vector<std::string_view> u;
        uint64_t bits = 0;
        for (TypeIndex m : members) {
            const FieldValue& mv = effective(m, f);
            bits |= mv.mask;
            for (std::string_view tok : mv.items)
                if (!list_has(u, tok)) u.push_back(tok);
        }
        FieldValue& v = slot(s, f);
        v.items = std::move(u);
        v.mask = bits;
        v.text = {};
    };
    auto members_of = [&](const std::vector<std::string_view>& names) {
        std::vector<TypeIndex> out;
        for (std::string_view n : names) {
            const TypeIndex m = resolve_name(n);
            if (m != kNone) out.push_back(m);
        }
        return out;
    };
    // §9.5 member power: a nested member (own Company_Units, else Squadron_Units, whose first
    // entry's type has Create_Team) contributes power x clamp(Max_Squad_Size / own list count, 0, 1)
    // when the mode type != 0 (-1 at load). Ratio computed in float.
    auto member_power = [&](TypeIndex m) {
        float p = effective(m, I.ai_power).f;
        if (kLoadModeType == 0) return p;
        const std::vector<std::string_view>& cu = effective(m, I.company_units).items;
        const std::vector<std::string_view>& list = !cu.empty() ? cu : effective(m, I.squadron_units).items;
        if (list.empty()) return p;
        const TypeIndex first = resolve_name(list.front());
        if (first == kNone || !effective(first, I.create_team).b) return p;  // original: no null check
        float r = static_cast<float>(effective(m, I.max_squad_size).i) / static_cast<float>(list.size());
        r = r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
        return p * r;
    };
    // 3. Company.
    if (!company.empty()) {
        const std::vector<TypeIndex> mem = members_of(company);
        union_masks(mem, I.category_mask);
        union_masks(mem, I.property_flags);
        union_masks(mem, I.garrison_category);
        float power = 0.0f;
        for (TypeIndex m : mem) {
            power += member_power(m);
            company_member_[m] = 1;
        }
        slot(s, I.ai_power).f = power;
    }
    // 4. Squadron.
    if (!squadron.empty()) {
        const std::vector<TypeIndex> mem = members_of(squadron);
        union_masks(mem, I.category_mask);
        union_masks(mem, I.property_flags);
        float power = 0.0f;
        for (TypeIndex m : mem) {
            power += member_power(m);
            squadron_member_[m] = 1;
        }
        slot(s, I.ai_power).f = power;
        if (!mem.empty()) {
            const float factor = squadron_speed_factor(kLoadModeType);
            auto scale = [&](TypeIndex m) {
                return has_behavior(m, BehaviorSet::Generic, beh::Projectile) ? 1.0f : factor;
            };
            float hs = effective(mem[0], I.hyperspace_speed).f, sp = effective(mem[0], I.max_speed).f;
            float turn = effective(mem[0], I.turn_rate).f * scale(mem[0]);
            float mn = effective(mem[0], I.min_speed).f * scale(mem[0]);
            for (TypeIndex m : mem) {
                hs = std::min(hs, effective(m, I.hyperspace_speed).f);
                sp = std::min(sp, effective(m, I.max_speed).f);
                turn = std::min(turn, effective(m, I.turn_rate).f * scale(m));
                mn = std::max(mn, effective(m, I.min_speed).f * scale(m));
            }
            const bool hyper = effective(mem.back(), I.hyperspace).b;
            slot(s, I.hyperspace_speed).f = hs;
            slot(s, I.max_speed).f = sp;
            slot(s, I.turn_rate).f = turn;
            slot(s, I.min_speed).f = mn;
            slot(s, I.min_speed_fraction).f = mn;
            slot(s, I.hyperspace).b = hyper;
        }
    }
    // 5. Affiliation -> factions (first definition by name, unknown dropped); empty list -> the
    //    default (first Is_Neutral) faction, name written back.
    if (have_factions_) {
        std::vector<uint32_t> out;
        for (std::string_view n : effective(t, I.affiliation).items) {
            uint32_t k = 0;
            while (k < faction_name_.size() && !iequals(faction_name_[k], n)) ++k;
            if (k < faction_name_.size()) out.push_back(k);
        }
        if (effective(t, I.affiliation).items.empty() && default_faction_ != kNone) {
            slot(s, I.affiliation).items.push_back(default_faction_upper_);
            out.push_back(default_faction_);
        }
        faction_ids_[t] = std::move(out);
    }
    // 6. HardPoints -> hardpoint data (unknown dropped); min attack distance raised without WEAPON.
    if (have_hardpoints_) {
        std::vector<uint32_t> out;
        for (std::string_view n : effective(t, I.hardpoints).items) {
            uint32_t k = 0;
            while (k < hp_name_.size() && !iequals(hp_name_[k], n)) ++k;
            if (k < hp_name_.size()) out.push_back(k);
        }
        if (!list_has(effective(t, I.behavior).items, "WEAPON")) {
            float d = effective(t, I.min_attack).f;
            const float before = d;
            for (uint32_t k : out) d = std::max(d, hp_min_range_[k]);
            if (d != before) {
                slot(s, I.min_attack).f = d;
                ++stats_.min_range_raised;  // counted per raise (a pass can raise again after a variant copy)
            }
        }
        hardpoint_ids_[t] = std::move(out);
    }
    // 12. Population override (§9.4) and the marker-placement flag of the named type.
    if (has_behavior(t, BehaviorSet::Space, beh::TacticalSuperWeapon) && effective(t, I.space_pop).i != 0) {
        slot(s, I.space_pop).i = 0;
        ++stats_.population_forced;
    }
    if (has_behavior(t, BehaviorSet::Land, beh::TacticalSuperWeapon) && effective(t, I.land_pop).i != 0) {
        slot(s, I.land_pop).i = 0;
        ++stats_.population_forced;
    }
    if (I.place_marker != kNoField) {
        const TypeIndex m = resolve_name(effective(t, I.place_marker).text);
        if (m != kNone) placed_at_markers_[m] = 1;
    }
}

void GameObjectTypes::resize_squadrons() {
    // [port: Gabriel, 2026-11-13] Every fighter / bomber squadron has squadron_size_ members: a Squadron_Units list naming
    // ONE member type whose CategoryMask has Fighter or Bomber and not SpaceHero is rewritten to that many copies, once,
    // on the final state after every pass. Its prices (Build_Cost_Credits, Tactical_Build_Cost_Multiplayer /
    // _Campaign, when > 0) and AI_Combat_Power (the members' sum after derive) are scaled by size / the list's count, so a
    // TIE_Fighter_Squadron (4 members, 300) becomes 12 members for 900. Each type scales its own final values, so a
    // variant overriding a price or inheriting the list is scaled once. Mixed lists (hero squadrons such as Red_Squadron,
    // Rogue_Squadron_Space, Darth_Vader_TIE_Fighter_Squadron) keep their XML members and prices.
    if (squadron_size_ == 0) return;
    const Ids& I = ids();
    const EnumConverter& cat = enums_.get(EnumId::GameObjectCategory);
    const uint64_t fighter = static_cast<uint64_t>(cat.lookup("Fighter").value_or(0x1));
    const uint64_t bomber = static_cast<uint64_t>(cat.lookup("Bomber").value_or(0x2));
    const uint64_t hero = static_cast<uint64_t>(cat.lookup("SpaceHero").value_or(0x2000000));
    const FieldId prices[] = {I.build_cost, find_type_field("Tactical_Build_Cost_Multiplayer"),
                              find_type_field("Tactical_Build_Cost_Campaign")};
    for (TypeIndex t = 0; t < state_.size(); ++t) {
        const FieldValue* v = value(t, I.squadron_units);
        if (v == nullptr || v->items.empty() || v->items.size() == squadron_size_) continue;
        const std::string_view first = v->items.front();
        bool one_type = true;
        for (const std::string_view n : v->items) one_type = one_type && iequals(n, first);
        if (!one_type) continue;
        const TypeIndex m = resolve_name(first);
        if (m == kNone) continue;
        const uint64_t mask = effective(m, I.category_mask).mask;
        if ((mask & (fighter | bomber)) == 0 || (mask & hero) != 0) continue;
        const auto count = static_cast<int64_t>(v->items.size());
        const auto size = static_cast<int64_t>(squadron_size_);
        for (const FieldId f : prices) {
            if (f == kNoField) continue;
            const FieldValue* pv = value(t, f);
            if (pv == nullptr || pv->i <= 0) continue;
            const int64_t scaled = (static_cast<int64_t>(pv->i) * size + count / 2) / count;  // rounded
            slot(state_[t], f).i = static_cast<int32_t>(std::min<int64_t>(scaled, INT32_MAX));
        }
        if (const FieldValue* pw = value(t, I.ai_power); pw != nullptr) {
            const float power = pw->f * static_cast<float>(size) / static_cast<float>(count);
            slot(state_[t], I.ai_power).f = power;
        }
        slot(state_[t], I.squadron_units).items.assign(squadron_size_, first);
        ++stats_.squadrons_resized;
    }
}

void GameObjectTypes::after_pass() {
    // Lazy references are resolved on read (resolve_name = first definition; all types exist after
    // pass 0), so only the derivations run here, on every type in index order.
    for (TypeIndex t = 0; t < state_.size(); ++t) derive(t);
}

void GameObjectTypes::load_registries(const FileProvider& provider) {
    // cu5 t02: the shared list walk. Every child element of the list's root is an entry (not only <File>), in list
    // order; the listed documents stay alive in registry_docs_ (the tables keep views into them).
    data::ListedXmlOptions list_options;
    list_options.entry_tag = {};
    list_options.visit_missing = true;
    list_options.visit_unparsable = true;
    auto load_list = [&](std::string_view list, auto&& per_element) {
        const data::ListStatus st = data::for_each_listed_xml(provider, list, [&](data::ListedXmlFile& file) {
            if (!file.doc) {
                warn("unable to open XML file '" + file.entry.path + "' (skipped)");
                return;
            }
            const data::XmlDocument* d = file.doc.get();
            registry_docs_.push_back(std::move(file.doc));
            if (!d->ok()) return;
            for (data::XmlNode n = d->root().first_child(); n; n = n.next_sibling()) per_element(n);
        }, list_options);
        return st == data::ListStatus::Ok;
    };
    // Last text of a direct child (scalars: last occurrence wins).
    auto last_child_text = [](data::XmlNode n, std::string_view tag) {
        std::string_view out;
        bool seen = false;
        for (data::XmlNode c = n.first_child(); c; c = c.next_sibling())
            if (iequals(c.name(), tag) && !data::trim(c.text()).empty()) {
                out = data::trim(c.text());
                seen = true;
            }
        return seen ? out : std::string_view{};
    };
    // §9.6: hardpoints keyed by name, insert-or-overwrite -> the LAST definition wins.
    have_hardpoints_ = load_list("Data\\XML\\HardPointDataFiles.xml", [&](data::XmlNode n) {
        const std::string_view name = data::trim(n.attribute("Name"));
        if (name.empty()) return;
        const std::string_view rt = last_child_text(n, "Fire_Min_Range_Distance");
        const float range = rt.empty() ? 0.0f : static_cast<float>(c_atof(rt));
        for (size_t k = 0; k < hp_name_.size(); ++k)
            if (iequals(hp_name_[k], name)) {
                hp_min_range_[k] = range;
                ++stats_.hardpoints_redefined;
                return;
            }
        hp_name_.push_back(name);
        hp_min_range_.push_back(range);
    });
    // §9.2: factions appended in file order without a duplicate check (lookups: first match).
    have_factions_ = load_list("Data\\XML\\FactionFiles.xml", [&](data::XmlNode n) {
        const std::string_view name = data::trim(n.attribute("Name"));
        if (name.empty()) return;
        faction_name_.push_back(name);
        faction_neutral_.push_back(parse_type_bool(last_child_text(n, "Is_Neutral")) ? 1 : 0);
    });
    // §9.1.3 / TACTICAL_COMBAT.md §2.4: Targeting_Priority_Set names, every set kept in file order
    // (lookups first-wins, like the combat loader). Loaded before the types (MAIN_LOOP.md order).
    have_priority_sets_ = load_list("Data\\XML\\TargetingPrioritySetFiles.xml", [&](data::XmlNode n) {
        const std::string_view name = data::trim(n.attribute("Name"));
        if (!name.empty()) priority_set_name_.push_back(name);
    });
    for (uint32_t k = 0; k < faction_name_.size(); ++k)
        if (faction_neutral_[k]) {
            default_faction_ = k;
            default_faction_name_ = std::make_unique<std::string>(normalize_name(faction_name_[k]));
            default_faction_upper_ = *default_faction_name_;
            break;
        }
}

bool GameObjectTypes::build(const ObjectTypeDatabase& raw, const FileProvider* provider,
                            std::vector<std::string>* diagnostics, AnimTypeLookup anim_types) {
    const uint32_t squadron_size = squadron_size_;  // the port rule survives the reset
    *this = GameObjectTypes{};
    squadron_size_ = squadron_size;
    raw_ = &raw;
    diag_ = diagnostics;
    anim_lookup_ = anim_types;
    const size_t n = raw.type_count();
    // Constructor defaults.
    defaults_.assign(type_fields().size(), FieldValue{});
    for (FieldId f = 0; f < defaults_.size(); ++f) defaults_[f].field = f;
    for (const auto& [tag, text] : kDefaults) {
        const FieldId f = find_type_field(tag);
        if (f == kNoField) continue;
        FieldValue& v = defaults_[f];
        const Conv c = type_fields()[f].conv;
        if (c == Conv::Enum) v.i = c_atoi(text);  // name unknown: "none" = -1
        else if (c == Conv::Bool) v.b = parse_type_bool(text);
        else if (c == Conv::Int32 || c == Conv::Int8 || c == Conv::UIntClamp100) v.i = c_atoi(text);
        else if (c == Conv::Float || c == Conv::Float01) v.f = static_cast<float>(c_atof(text));
        else if (c == Conv::Double || c == Conv::Double01) v.d = c_atof(text);
        else v.text = text;
    }
    if (ids().priority_set != kNoField) defaults_[ids().priority_set].i = -1;  // null set
    state_.assign(n, {});
    resolved_.assign(n, 0);
    resolve_pass_.assign(n, 0xFF);
    company_member_.assign(n, 0);
    squadron_member_.assign(n, 0);
    placed_at_markers_.assign(n, 0);
    for (auto& b : bits_) b.assign(n, BehaviorBits{});
    hardpoint_ids_.assign(n, {});
    faction_ids_.assign(n, {});
    if (provider) {
        // Enum converters are created before the object databases (§9.1).
        std::vector<std::string> enum_diag;
        enums_.load_dynamic(*provider, &enum_diag);
        for (std::string& d : enum_diag) warn(std::move(d));
        load_registries(*provider);
    }
    stats_.types = static_cast<uint32_t>(n);

    // Pass 0: creation order.
    for (TypeIndex t = 0; t < n; ++t) {
        const BehaviorBits before = bits_[static_cast<size_t>(BehaviorSet::Generic)][t];  // empty (constructor)
        parse(t, false);
        end_top_level_parse(t, before);
        fixup(t);
        if (variant_name(t).empty()) {
            resolved_[t] = 1;
            resolve_pass_[t] = 0;
        } else {
            ++stats_.variants;
            try_resolve(t, 0);
        }
    }
    after_pass();
    stats_.passes = 1;
    auto all_resolved = [&] {
        for (TypeIndex t = 0; t < n; ++t)
            if (!resolved_[t]) return false;
        return true;
    };
    // Passes 1..9: re-walk every list walk that created an unresolved type.
    const TypeColumns& ty = raw.types();
    for (uint8_t pass = 1; pass <= 9 && !all_resolved(); ++pass) {
        ++stats_.passes;
        TypeIndex begin = 0;
        while (begin < n) {
            TypeIndex end = begin;
            uint32_t unresolved = 0;
            while (end < n && ty.walk[end] == ty.walk[begin]) unresolved += resolved_[end++] ? 0u : 1u;
            for (uint32_t rep = 0; rep < unresolved; ++rep)
                for (TypeIndex e = begin; e < end; ++e) {
                    const TypeIndex first = raw.find(ty.name[e]);
                    if (first != kNone) try_resolve(first, pass);
                }
            begin = end;
        }
        after_pass();
    }
    for (TypeIndex t = 0; t < n; ++t) {
        if (variant_name(t).empty()) continue;
        if (!resolved_[t]) {
            ++stats_.unresolved;
            const TypeIndex p = variant_parent(t);
            warn(raw.file_of(t) + "(" + std::to_string(ty.line[t]) + "): variant '" + std::string(ty.name[t]) +
                 "' unresolved: " +
                 (p == kNone ? "parent '" + std::string(variant_name(t)) + "' not found"
                             : (p == t ? std::string("refers to itself") : "parent chain does not resolve (cycle?)")));
        } else if (resolve_pass_[t] == 0) {
            ++stats_.resolved_pass0;
        } else {
            ++stats_.resolved_later;
        }
    }
    resize_squadrons();  // [port: Gabriel] fighter / bomber squadrons of squadron_size_, prices scaled
    const Ids& I = ids();
    for (TypeIndex t = 0; t < n; ++t) {
        if (!effective(t, I.company_units).items.empty()) ++stats_.companies;
        if (!effective(t, I.squadron_units).items.empty()) ++stats_.squadrons;
        if (have_hardpoints_) {
            const size_t listed = effective(t, I.hardpoints).items.size();
            stats_.hardpoints_dropped += static_cast<uint32_t>(listed - hardpoint_ids_[t].size());
        }
        if (have_factions_) {
            const auto& aff = effective(t, I.affiliation).items;
            if (aff.size() == 1 && default_faction_ != kNone && aff[0].data() == default_faction_upper_.data())
                ++stats_.affiliation_defaulted;
            stats_.factions_dropped += static_cast<uint32_t>(aff.size() - faction_ids_[t].size());
        }
        const uint32_t sc = behavior_count(t, kStarshipCombatant);
        if (sc >= 1) ++stats_.combatant_types;
        if (sc >= 2) ++stats_.combatant_twice;
    }
    // Columns.
    Columns& c = cols_;
    auto fcol = [&](std::vector<float>& col, FieldId f) {
        col.resize(n);
        for (TypeIndex t = 0; t < n; ++t) col[t] = effective(t, f).f;
    };
    auto icol = [&](std::vector<int32_t>& col, FieldId f) {
        col.resize(n);
        for (TypeIndex t = 0; t < n; ++t) col[t] = effective(t, f).i;
    };
    fcol(c.tactical_health, I.health);
    fcol(c.shield_points, I.shield);
    fcol(c.energy_capacity, I.energy);
    fcol(c.max_speed, I.max_speed);
    fcol(c.max_rate_of_turn, I.turn_rate);
    fcol(c.mass, I.mass);
    fcol(c.scale_factor, I.scale);
    fcol(c.ai_combat_power, I.ai_power);
    fcol(c.hyperspace_speed, I.hyperspace_speed);
    fcol(c.targeting_min_attack_distance, I.min_attack);
    fcol(c.targeting_max_attack_distance, I.max_attack);
    fcol(c.land_fow_reveal_range, I.land_fow);
    fcol(c.space_fow_reveal_range, I.space_fow);
    icol(c.build_cost_credits, I.build_cost);
    icol(c.build_time_seconds, I.build_time);
    icol(c.tech_level, I.tech);
    icol(c.score_cost_credits, I.score);
    icol(c.population_value, I.population);
    diag_ = nullptr;
    return true;
}

const FieldValue* GameObjectTypes::value(TypeIndex t, FieldId f) const {
    if (t >= state_.size() || f >= defaults_.size()) return nullptr;
    return find(state_[t], f);
}

const FieldValue& GameObjectTypes::effective(TypeIndex t, FieldId f) const {
    if (f >= defaults_.size()) return kZero;
    const FieldValue* v = t < state_.size() ? find(state_[t], f) : nullptr;
    return v ? *v : defaults_[f];
}

bool GameObjectTypes::get_bool(TypeIndex t, std::string_view tag) const { return effective(t, find_type_field(tag)).b; }
// bkc1 t07 (b0126): the numeric getters read the slot the field's parser fills, so a caller does not need to know
// which numeric kind the schema gives a tag. A float-parsed field (codes 0x8..0xb, e.g. Override_Bounty_SP with text
// "50") read with get_int truncates toward zero, as a C float -> int cast; an int field read with get_float converts.
// Before, get_int on a float field returned the unset int slot (0).
int32_t GameObjectTypes::get_int(TypeIndex t, std::string_view tag) const {
    const FieldId f = find_type_field(tag);
    const FieldValue& v = effective(t, f);
    if (f >= defaults_.size()) return v.i;
    switch (type_fields()[f].conv) {
        case Conv::Float:
        case Conv::Float01: return static_cast<int32_t>(v.f);
        case Conv::Double:
        case Conv::Double01: return static_cast<int32_t>(v.d);
        default: return v.i;
    }
}
float GameObjectTypes::get_float(TypeIndex t, std::string_view tag) const {
    const FieldId f = find_type_field(tag);
    const FieldValue& v = effective(t, f);
    if (f >= defaults_.size()) return v.f;
    switch (type_fields()[f].conv) {
        case Conv::Int32:
        case Conv::Int8:
        case Conv::UIntClamp100: return static_cast<float>(v.i);
        case Conv::Double: return static_cast<float>(v.d);
        default: return v.f;
    }
}
int32_t GameObjectTypes::targeting_priority_set(TypeIndex t) const {
    return have_priority_sets_ && ids().priority_set != kNoField ? effective(t, ids().priority_set).i : -1;
}
uint64_t GameObjectTypes::get_mask(TypeIndex t, std::string_view tag) const {
    return effective(t, find_type_field(tag)).mask;
}
std::string_view GameObjectTypes::get_string(TypeIndex t, std::string_view tag) const {
    return effective(t, find_type_field(tag)).text;
}
std::array<float, 4> GameObjectTypes::get_vec(TypeIndex t, std::string_view tag) const {
    return effective(t, find_type_field(tag)).v;
}
std::span<const std::string_view> GameObjectTypes::get_list(TypeIndex t, std::string_view tag) const {
    return effective(t, find_type_field(tag)).items;
}
std::span<const FieldIndex> GameObjectTypes::get_sub_objects(TypeIndex t, std::string_view tag) const {
    return effective(t, find_type_field(tag)).sub;
}
TypeIndex GameObjectTypes::get_type_ref(TypeIndex t, std::string_view tag) const {
    return resolve_name(effective(t, find_type_field(tag)).text);
}
std::vector<TypeIndex> GameObjectTypes::get_type_refs(TypeIndex t, std::string_view tag) const {
    std::vector<TypeIndex> out;
    for (std::string_view n : get_list(t, tag)) out.push_back(resolve_name(n));
    return out;
}
uint32_t GameObjectTypes::behavior_count(TypeIndex t, std::string_view name) const {
    uint32_t k = 0;
    for (std::string_view n : effective(t, ids().behavior).items)
        if (iequals(n, name)) ++k;
    return k;
}

}  // namespace alamo::objects

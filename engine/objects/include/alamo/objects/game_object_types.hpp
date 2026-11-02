// Typed, merged GameObjectType view (docs/re/OBJECT_TYPES.md) built on the raw ObjectTypeDatabase.
//
// Reproduces the original loader:
//   * pass 0: every type element in creation order gets a first parse (defaults + own tags, Â§4) and
//     the per-parse fix-up (Â§6.1); a variant whose parent (first definition of the name) is already
//     resolved resolves at once;
//   * after each pass: lazy references, then the per-type derivations of Â§6.2 on every type in
//     index order (company / squadron members, negative Build_Cost_Credits, hardpoint min range);
//   * passes 1..9: every list walk that created an unresolved type is re-walked (once per such
//     type, as the original re-reads the file), each element's FIRST definition by name retried;
//   * resolving = copy of the parent's whole state, then the child's own XML in VARIANT mode (Â§3.3).
//   Cycles, self references and missing parents stay unresolved (own tags on defaults), with a
//   diagnostic (the original is silent).
// Field rules: tags case-insensitive, values trimmed, empty value = no-op, "TBD" skipped (logged),
// tags outside the type's descriptor table ignored, nested elements flattened unless
// SubObjectList="Yes" (sub-object list). Scalars last-wins; list merge per type_fields.hpp.
//
// Value tables and load rules of OBJECT_TYPES.md §9 (2026-10-27):
//   * enum / bitmask fields convert through enums.hpp (FieldValue::i = enum value, FieldValue::mask
//     = bitfield value; text / items keep the value as written). A failed conversion (unknown
//     name, ONE unknown mask token) logs "Error: Class data member <tag>: Failed to convert value
//     (value)!" and leaves the field untouched. Code 0x43 (Projectile_Blast_Area_Immune_Faction,
//     Item.Faction) is a plain string resolved at use time; code 0x52 (Projectile_Combat_Mod) is
//     the combat-mod enum of TACTICAL_GAPS §6.5 (enums.hpp combat_mod_value; FieldValue::i = 1..11,
//     an unknown token leaves the field unchanged without a log; the text is kept as written);
//   * ModelAnimType fields (Specific_Death_Anim_Type, Shield_On_Anim, Shield_Off_Anim; §9.1.3) convert
//     through the caller's AnimTypeLookup (pass anim::find_anim_type: engine/anim links engine/objects,
//     so objects cannot call it directly). FieldValue::i = the ModelAnimType id; unknown names fail as
//     above. Without a lookup the names are kept only (no conversion, no log);
//   * Targeting_Priority_Set (code 0x54, §9.1.3): a set name from the files listed in
//     TargetingPrioritySetFiles.xml (loaded with the other registries, before the types, as in
//     MAIN_LOOP.md's database order); FieldValue::i = index into targeting_priority_set_names() (first
//     definition of a name, case-insensitive), -1 = none (the constructor's null). Unknown names fail as
//     above. Without the list file the name is kept only;
//   * implicit STARSHIP_COMBATANT appended to Behavior at the end of every top-level parse when
//     !Is_Decoration, the PRE-parse generic bitset lacks PROJECTILE/PARTICLE, and Damage,
//     Tactical_Health, Shield_Points or AI_Combat_Power > 0 (no duplicate check, §9.3);
//   * five behaviour bitsets rebuilt at the end of every top-level parse (copied from the parent
//     before a variant re-parse);
//   * empty Affiliation -> the first Is_Neutral faction (name upper-cased + index), every pass;
//   * company / squadron masks = bit OR of the members; nested-member power clamp (mode -1 at load);
//     squadron speed factor 1.0 at load;
//   * Space/Land_Override_Population_Value forced to 0 when the Space/Land bitset has
//     TACTICAL_SUPER_WEAPON (id 44);
//   * hardpoints: last definition of a name wins; factions: every definition kept, lookups first-wins.
// Not reproduced: Lua.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/enums.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/objects/type_fields.hpp"

namespace alamo::objects {

using FieldId = uint32_t;  // index into type_fields()

// The value of one field on one type. Which members are meaningful depends on the field's Conv.
struct FieldValue {
    FieldId field = 0;
    bool inherited = false;  // Merge::TypeRefList: copied from the parent, next append clears
    bool b = false;
    int32_t i = 0;           // ints; Enum: the converted value (32 bits)
    float f = 0.0f;
    double d = 0.0;
    std::array<float, 4> v{};             // Float2 / Vec3 / Float4 / Rgba
    std::string_view text;                // scalars: last value as written (trimmed)
    std::vector<std::string_view> items;  // lists: tokens; Mask: tokens of the value (company/squadron: union)
    std::vector<FieldIndex> sub;          // SubObjects: raw-store indices of the child elements
    uint64_t mask = 0;                    // Mask (0x54/0x56): 64-bit value; bitfield enums (0xe): 32-bit value
};

struct TypeLoadStats {
    uint32_t types = 0, variants = 0, resolved_pass0 = 0, resolved_later = 0, unresolved = 0, passes = 0;
    uint32_t tags_applied = 0, tags_ignored = 0, tbd = 0, empty = 0, clamped = 0, long_lists_ignored = 0;
    uint32_t subobject_lists = 0, subobject_unprocessed = 0;
    uint32_t companies = 0, squadrons = 0, cost_derived = 0, base_level_normalised = 0, min_range_raised = 0;
    uint32_t hardpoints_dropped = 0, factions_dropped = 0;
    // §9 (counted over every parse, variant re-parses included).
    uint32_t enum_converted = 0, enum_failed = 0, mask_converted = 0, mask_failed = 0;
    uint32_t implicit_combatant_appends = 0;
    uint32_t anim_converted = 0, anim_failed = 0, priority_set_converted = 0, priority_set_failed = 0;
    uint32_t combat_mod_converted = 0, combat_mod_unknown = 0;  // code 0x52 (Projectile_Combat_Mod)
    // Final state (after the last pass).
    uint32_t combatant_types = 0, combatant_twice = 0;  // STARSHIP_COMBATANT in Behavior >= 1 / >= 2 times
    uint32_t affiliation_defaulted = 0, population_forced = 0, hardpoints_redefined = 0;
};

// The five per-type behaviour bitsets (bit = runtime behaviour type id, 0x6A bits).
enum class BehaviorSet : uint8_t { Galactic = 0, Land = 1, Space = 2, Union = 3, Generic = 4 };
struct BehaviorBits {
    std::array<uint64_t, 2> w{};
    bool test(BehaviorTypeId id) const { return id < kBehaviorTypeCount && ((w[id >> 6] >> (id & 63)) & 1u) != 0; }
    void set(BehaviorTypeId id) {
        if (id < kBehaviorTypeCount) w[id >> 6] |= uint64_t{1} << (id & 63);
    }
    friend bool operator==(const BehaviorBits& a, const BehaviorBits& b) { return a.w == b.w; }
};

// ModelAnimType XML name -> id, negative when unknown. Pass anim::find_anim_type (dependency
// inversion: engine/anim depends on engine/objects).
using AnimTypeLookup = int32_t (*)(std::string_view xml_name);

class GameObjectTypes {
public:
    // `raw` must outlive this object. `provider` (optional) supplies HardPointDataFiles.xml,
    // FactionFiles.xml and TargetingPrioritySetFiles.xml for hardpoint / faction / priority-set name
    // resolution; without it those names are kept but not resolved, and Targeting_Min_Attack_Distance
    // is not raised. `anim_types` (optional) converts the ModelAnimType fields.
    bool build(const ObjectTypeDatabase& raw, const FileProvider* provider = nullptr,
               std::vector<std::string>* diagnostics = nullptr, AnimTypeLookup anim_types = nullptr);

    const ObjectTypeDatabase& raw() const { return *raw_; }
    size_t size() const { return state_.size(); }
    const TypeLoadStats& stats() const { return stats_; }

    bool is_variant(TypeIndex t) const { return !variant_name(t).empty(); }
    std::string_view variant_name(TypeIndex t) const;
    bool resolved(TypeIndex t) const { return resolved_[t] != 0; }
    // Pass in which the type resolved (0..9); 0xFF while unresolved.
    uint8_t resolve_pass(TypeIndex t) const { return resolve_pass_[t]; }
    TypeIndex variant_parent(TypeIndex t) const;  // first definition of variant_name, kNone if absent

    // Field state; nullptr = never set (the default applies).
    const FieldValue* value(TypeIndex t, FieldId f) const;
    const FieldValue& default_value(FieldId f) const { return defaults_[f]; }
    const FieldValue& effective(TypeIndex t, FieldId f) const;  // value or default
    std::span<const FieldValue> values(TypeIndex t) const { return state_[t]; }  // sorted by field

    // Convenience by tag (ASCII case-insensitive); unknown tags give the zero value.
    bool get_bool(TypeIndex t, std::string_view tag) const;
    int32_t get_int(TypeIndex t, std::string_view tag) const;
    float get_float(TypeIndex t, std::string_view tag) const;
    double get_double(TypeIndex t, std::string_view tag) const;
    std::string_view get_string(TypeIndex t, std::string_view tag) const;
    std::array<float, 4> get_vec(TypeIndex t, std::string_view tag) const;
    std::span<const std::string_view> get_list(TypeIndex t, std::string_view tag) const;
    std::span<const FieldIndex> get_sub_objects(TypeIndex t, std::string_view tag) const;
    // Lazy references (Â§4.5): "None" / unknown -> kNone, else the first definition.
    TypeIndex get_type_ref(TypeIndex t, std::string_view tag) const;
    std::vector<TypeIndex> get_type_refs(TypeIndex t, std::string_view tag) const;
    TypeIndex resolve_name(std::string_view name) const;
    // Converted values (§9.1): enum value (= get_int) and 64-bit mask; unknown tags give 0.
    int32_t get_enum(TypeIndex t, std::string_view tag) const { return get_int(t, tag); }
    uint64_t get_mask(TypeIndex t, std::string_view tag) const;
    const EnumRegistry& enums() const { return enums_; }

    // Behaviour bitsets (§9.3) and list queries.
    const BehaviorBits& behavior_bits(TypeIndex t, BehaviorSet s) const { return bits_[static_cast<size_t>(s)][t]; }
    bool has_behavior(TypeIndex t, BehaviorSet s, BehaviorTypeId id) const { return behavior_bits(t, s).test(id); }
    // Occurrences of `name` (case-insensitive) in the generic Behavior list.
    uint32_t behavior_count(TypeIndex t, std::string_view name) const;

    // Derived (Â§6.2).
    bool company_member(TypeIndex t) const { return company_member_[t] != 0; }
    bool squadron_member(TypeIndex t) const { return squadron_member_[t] != 0; }
    // Byte +0xE3 of §9.4: set on the type named by some type's
    // Place_Other_Type_At_Every_Specific_Marker_Position (tested by parent-mode death marking).
    bool placed_at_markers(TypeIndex t) const { return placed_at_markers_[t] != 0; }
    std::span<const uint32_t> hardpoints(TypeIndex t) const { return hardpoint_ids_[t]; }  // into hardpoint_names()
    std::span<const uint32_t> factions(TypeIndex t) const { return faction_ids_[t]; }      // into faction_names()
    // Unique hardpoint names (first-seen order); data from the LAST definition of each name.
    const std::vector<std::string_view>& hardpoint_names() const { return hp_name_; }
    const std::vector<float>& hardpoint_min_range() const { return hp_min_range_; }
    // Every <Faction> definition in file order (duplicates kept; lookups take the first match).
    const std::vector<std::string_view>& faction_names() const { return faction_name_; }
    const std::vector<uint8_t>& faction_is_neutral() const { return faction_neutral_; }
    // Default faction for an empty Affiliation (§9.2): first Is_Neutral faction; kNone if none.
    uint32_t default_faction() const { return default_faction_; }
    std::string_view default_faction_name() const { return default_faction_upper_; }
    // Targeting_Priority_Set registry: every <... Name=> element of the listed files, file order,
    // duplicates kept (lookups first-wins). Empty when TargetingPrioritySetFiles.xml was not loaded.
    const std::vector<std::string_view>& targeting_priority_set_names() const { return priority_set_name_; }
    bool have_priority_sets() const { return have_priority_sets_; }
    // The type's set as an index into targeting_priority_set_names(); -1 = none or not resolved.
    int32_t targeting_priority_set(TypeIndex t) const;
    // Animation / presence fields (docs/re/ANIM_PARTICLES.md §A.11, §A.12; R3f t05): effective values (own, inherited
    // or the constructor default, which lives in one place: kDefaults in game_object_types.cpp).
    //   Presence_Induced_Animations (code 0x35, affected type +0x1610): the tokens as written (anim type names, each
    //     followed by the type names it applies to). The caller converts them: objects does not link anim. Default empty.
    //   Presence_Range (presence type +0x1628): presence cube half-extent, default 0.
    //   Allow_Idle_When_Moving (+0x85): default false [inferred].
    //   Deploys (+0xd1): default false.
    //   Induced_Flinch_Range (explosion type +0x20e0): default 100. Induced_Flinch_Intensity (+0x20e4): default 1.0.
    std::span<const std::string_view> presence_induced_animations(TypeIndex t) const;
    float presence_range(TypeIndex t) const;
    bool allow_idle_when_moving(TypeIndex t) const;
    bool deploys(TypeIndex t) const;
    float induced_flinch_range(TypeIndex t) const;
    float induced_flinch_intensity(TypeIndex t) const;
    // ModelAnimType id of Specific_Death_Anim_Type / Shield_On_Anim / Shield_Off_Anim (= get_enum);
    // only meaningful when build() had an AnimTypeLookup (anim_types_converted()).
    bool anim_types_converted() const { return anim_lookup_ != nullptr; }

    // SoA columns of frequently read fields (effective values after every pass), index = TypeIndex.
    struct Columns {
        std::vector<float> tactical_health, shield_points, energy_capacity, max_speed, max_rate_of_turn, mass,
            scale_factor, ai_combat_power, hyperspace_speed, targeting_min_attack_distance,
            targeting_max_attack_distance, land_fow_reveal_range, space_fow_reveal_range;
        std::vector<int32_t> build_cost_credits, build_time_seconds, tech_level, score_cost_credits,
            population_value;
    };
    const Columns& columns() const { return cols_; }

private:
    using State = std::vector<FieldValue>;
    FieldValue& slot(State& s, FieldId f);
    const FieldValue* find(const State& s, FieldId f) const;
    void parse(TypeIndex t, bool variant);
    void apply(State& s, FieldId f, std::string_view text, bool variant);
    bool convert_enum(State& s, FieldId f, std::string_view text);  // false = not a converted field
    void end_top_level_parse(TypeIndex t, const BehaviorBits& generic_before);
    void rebuild_bits(TypeIndex t);
    void fixup(TypeIndex t);
    bool try_resolve(TypeIndex t, uint8_t pass);
    void after_pass();
    void derive(TypeIndex t);
    void load_registries(const FileProvider& provider);
    void warn(std::string message);

    const ObjectTypeDatabase* raw_ = nullptr;
    std::vector<State> state_;
    std::vector<FieldValue> defaults_;
    std::vector<uint8_t> resolved_, resolve_pass_, company_member_, squadron_member_, placed_at_markers_;
    std::array<std::vector<BehaviorBits>, 5> bits_;
    std::vector<std::vector<uint32_t>> hardpoint_ids_, faction_ids_;
    std::vector<std::string_view> hp_name_, faction_name_;
    std::vector<float> hp_min_range_;
    std::vector<uint8_t> faction_neutral_;
    std::vector<std::string_view> priority_set_name_;
    std::vector<std::unique_ptr<data::XmlDocument>> registry_docs_;
    AnimTypeLookup anim_lookup_ = nullptr;
    EnumRegistry enums_;
    uint32_t default_faction_ = kNone;
    std::unique_ptr<std::string> default_faction_name_;  // heap: stable for the written-back Affiliation views
    std::string_view default_faction_upper_;
    bool have_hardpoints_ = false, have_factions_ = false, have_priority_sets_ = false, reparse_ = false;
    Columns cols_;
    TypeLoadStats stats_;
    std::vector<std::string>* diag_ = nullptr;
};

// Constructor defaults of Â§4.6 / Â§5.4 as (tag, value text); every other field is 0 / false / empty.
std::span<const std::pair<std::string_view, std::string_view>> type_field_defaults();

}  // namespace alamo::objects

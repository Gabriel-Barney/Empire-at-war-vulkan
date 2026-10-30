#include "alamo/tactical/garrison/garrison_types.hpp"

#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/enums.hpp"

namespace alamo::tactical::garrison {

namespace {
bool valid(const objects::GameObjectTypes& types, objects::TypeIndex t) { return t != objects::kNone && t < types.size(); }

// GameObjectPropertiesType bit (OBJECT_TYPES.md 9.1.2): the enum file's value, else the vanilla constant.
uint64_t property_bit(const objects::GameObjectTypes& types, const char* name, uint64_t vanilla) {
    const auto m = types.enums().get(objects::EnumId::GameObjectProperties).convert(name);
    return (m && *m != 0) ? static_cast<uint64_t>(*m) : vanilla;
}

bool has(const objects::GameObjectTypes& types, objects::TypeIndex t, const char* behavior) {
    const objects::BehaviorTypeId b = objects::behavior_type_id(behavior);
    return b != objects::kNoBehavior && types.has_behavior(t, objects::BehaviorSet::Union, b);
}
}  // namespace

bool holder_desc_from_type(const objects::GameObjectTypes& types, objects::TypeIndex t, HolderDesc& out) {
    if (!valid(types, t)) return false;
    const uint64_t props = types.get_mask(t, "Property_Flags");
    const bool contain = (props & property_bit(types, "CanContainGarrison", 0x1000u)) != 0;
    HolderDesc d;
    if (has(types, t, "GARRISON_VEHICLE")) d.kind = HolderKind::Vehicle;
    else if (has(types, t, "GARRISON_STRUCTURE")) d.kind = HolderKind::Structure;
    else if (has(types, t, "GARRISON_HOVER")) d.kind = HolderKind::Hover;
    else if (contain) d.kind = HolderKind::Property;
    else return false;
    d.accepts = types.get_mask(t, "Garrison_Category");
    d.slots = types.get_int(t, "Num_Garrison_Slots");
    d.enter_dist = types.get_float(t, "Garrison_Enter_Dist");
    d.exit_dist = types.get_float(t, "Garrison_Exit_Dist");
    d.radius = types.get_float(t, "Garrison_Radius");
    d.can_fire = (props & property_bit(types, "GarrisonCanFire", 0x2000u)) != 0;
    out = d;
    return true;
}

bool occupant_desc_from_type(const objects::GameObjectTypes& types, objects::TypeIndex t, OccupantDesc& out) {
    if (!valid(types, t) || !has(types, t, "GARRISON_UNIT")) return false;
    out.category = types.get_mask(t, "Garrison_Category");
    out.value = types.get_int(t, "Garrison_Value");
    return true;
}

bool capture_desc_from_type(const objects::GameObjectTypes& types, objects::TypeIndex t, CapturePointDesc& out) {
    if (!valid(types, t) || !has(types, t, "CAPTURE_POINT")) return false;
    out.radius = types.get_float(t, "Capture_Point_Radius");
    out.transition_seconds = types.get_float(t, "Capture_Point_Transition_Time_Seconds");
    out.sticky = has(types, t, "REINFORCEMENT_POINT") || types.get_bool(t, "Ownership_Sticks");
    return true;
}

bool influences_capture(const objects::GameObjectTypes& types, objects::TypeIndex t) {
    return valid(types, t) && types.get_bool(t, "Influences_Capture_Point");
}

}  // namespace alamo::tactical::garrison

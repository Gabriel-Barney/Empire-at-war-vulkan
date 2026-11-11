// S4 garrison / capture descriptors from the object type data (GameObjectTypes; OBJECT_TYPES.md field table).
//   holder:   GARRISON_VEHICLE / GARRISON_STRUCTURE / GARRISON_HOVER behaviour, or Property_Flags CanContainGarrison;
//             Garrison_Category (accepted), Num_Garrison_Slots, Garrison_Enter_Dist / Exit_Dist / Radius,
//             Property_Flags GarrisonCanFire, Is_Community_Property.
//   occupant: GARRISON_UNIT behaviour; its own Garrison_Category and Garrison_Value.
//   capture:  CAPTURE_POINT behaviour; Capture_Point_Radius, Capture_Point_Transition_Time_Seconds, sticky =
//             REINFORCEMENT_POINT behaviour or Ownership_Sticks; Influences_Capture_Point (default true) on the units.
// The host fills HolderDesc::extent_len (and the bones) from its own bounds / model data.
#pragma once
#include "alamo/objects/game_object_types.hpp"
#include "alamo/tactical/garrison/capture.hpp"
#include "alamo/tactical/garrison/garrison.hpp"

namespace alamo::tactical::garrison {

bool holder_desc_from_type(const objects::GameObjectTypes& types, objects::TypeIndex t, HolderDesc& out);
bool occupant_desc_from_type(const objects::GameObjectTypes& types, objects::TypeIndex t, OccupantDesc& out);
bool capture_desc_from_type(const objects::GameObjectTypes& types, objects::TypeIndex t, CapturePointDesc& out);
bool influences_capture(const objects::GameObjectTypes& types, objects::TypeIndex t);

}  // namespace alamo::tactical::garrison

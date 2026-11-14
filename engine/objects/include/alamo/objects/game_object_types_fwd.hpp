// Forward declaration of GameObjectTypes (game_object_types.hpp, ~16 KB plus its includes) and the TypeIndex alias. Public headers
// that only take it by reference or pointer include this; the .cpp that calls its members includes game_object_types.hpp.
#pragma once
#include <cstdint>

namespace alamo::objects {
class GameObjectTypes;
using TypeIndex = uint32_t;  // same alias as object_types.hpp
}  // namespace alamo::objects

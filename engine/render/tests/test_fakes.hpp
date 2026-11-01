// Fakes shared by several render test executables (header-only; tests only).
#pragma once
#include <cstdint>
#include <string>

namespace alamo::testing {

// Request record shaped like lua::ParticleEffectRequest (snapshot_lua_effects is a template over its fields).
struct FakeLuaRequest {
    uint32_t frame = 0, seq = 0;
    int32_t object = 0, type = -1;
    std::string type_name, bone;
};

}  // namespace alamo::testing

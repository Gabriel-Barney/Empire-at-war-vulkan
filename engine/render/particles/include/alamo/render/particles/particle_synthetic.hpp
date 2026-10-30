// alamo_render_particles - data-free particle systems for tests and `alamo_render --particles --synthetic`.
#pragma once
#include <string>
#include <vector>

#include "alamo/render/particles/particle_data.hpp"

namespace alamo::render::particles {

// A plain emitter: rate mode, point shapes, white tracks (alpha 1 -> 0), scale `size`, the built-in dot texture.
EmitterDef basic_emitter(uint32_t particles_per_second, float lifetime, float size = 4.0f);

// Demo set: "SYN_FIRE" (additive burst sphere, rising), "SYN_SMOKE" (alpha, depth-sorted, rising), "SYN_SPARKS"
// (additive tails, gravity), "SYN_HEAT" (heat emitter, bucket 11), "SYN_CHAIN" (burst parent with spawn-on-death and
// spawn-during-life children).
std::vector<SystemPtr> synthetic_systems();

}  // namespace alamo::render::particles

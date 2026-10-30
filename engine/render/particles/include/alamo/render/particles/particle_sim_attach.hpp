// alamo_render_particles - proxies of the R1 sim view's objects (lib alamo_render_particles_sim, needs alamo_render_sim).
// Render-side only: reads an InterpolatedFrame and the SimInstanceBuilder's hardpoint state, never the sim (B.6).
#pragma once
#include <cstdint>
#include <functional>

#include "alamo/render/particles/particle_attach.hpp"
#include "alamo/render/sim_view.hpp"

namespace alamo::render::particles {

// Owner key of a sim object (bit 63 set; static_key values can collide only by hash chance).
inline uint64_t sim_owner_key(int64_t object_id) { return (1ull << 63) | static_cast<uint64_t>(static_cast<uint32_t>(object_id)); }
// Owner key of an effect the sim attached to an object (SnapshotEffect: damaged smoke), one per (object, percent).
inline uint64_t attached_effect_key(int64_t object_id, uint8_t percent) {
    return (1ull << 62) | (static_cast<uint64_t>(percent) << 32) | static_cast<uint64_t>(static_cast<uint32_t>(object_id));
}

// Owner key of a Lua Attach_Particle_Effect request (SnapshotEffect::lua_seq): one system per request, kept (one-shot
// effects are not recreated) while the object and the request stay listed.
inline uint64_t lua_effect_key(uint32_t lua_seq) { return (1ull << 60) | static_cast<uint64_t>(lua_seq); }

// Owner key of a projectile (SnapshotProjectile::serial): its model's proxies (missile trails), or the system itself when
// the projectile type's model is a particle file.
inline uint64_t projectile_key(uint32_t serial) { return (1ull << 59) | static_cast<uint64_t>(serial); }

struct SimAttachStats {
    uint32_t objects = 0, synced = 0, effects = 0, no_model = 0, faded_out = 0;  // effects: particle-file types (B.1.1)
    uint32_t below_detail = 0;  // skipped: Particle Detail < the type's No_Particle_Below_Detail_Level (no systems)
    uint32_t culled = 0;        // Allow_Particle_Model_Culling types the local player cannot see (systems kept, not drawn)
    uint32_t attached = 0, attached_missing = 0;  // InterpolatedFrame::effects synced / whose asset is no particle file
    uint32_t world = 0, world_missing = 0;        // R5 world effects synced / whose asset is no particle file
    uint32_t contained = 0;                       // R3: kSnapContained objects skipped
    uint32_t lua = 0, lua_bones = 0;              // R5: Lua Attach_Particle_Effect systems synced / placed at a bone
    uint32_t projectiles = 0;                     // projectiles synced (proxies or particle-file model)
};

struct SimAttachOptions {
    // Particle Detail option (ParticleWorld::detail()); types with No_Particle_Below_Detail_Level > detail get no systems.
    float detail = 1.0f;
    // Local visibility (fog of war) for Allow_Particle_Model_Culling types; unset = the snapshot's kSnapFogHidden mark (R5).
    // Independently of this, every object whose SimInstanceBuilder fog alpha is below kFogHideAlpha (HideWhenFogged) has
    // its proxies hidden (systems kept), and the proxies' fade is multiplied by that alpha.
    std::function<bool(objects::ObjectId)> locally_visible;
};

// One begin() .. end() over the frame: an object whose type model is a particle file is itself a system
// (ProxyAttachments::sync_effect); every other object with fade > 0 and a model is synced under sim_owner_key(id) with
// the same transform / fade as SimInstanceBuilder::build gives its instance, the builder's hardpoint mask and the
// object's current _ALT index (SnapshotObject::alt, damage state) for the proxy alt filter (call it after
// builder.build(frame, ...)). Objects that left the frame release their systems. `pose` (optional, R3): the object's
// current bone matrices, as given to SimInstanceBuilder::set_pose_source; it must be a pure getter here (it is called a
// second time per object). Null = bind pose. `options` null = detail 1, everything visible.
// Int-B: every frame.effects entry (sim-attached effects, e.g. damaged smoke) whose object passes the same gates is one
// system under attached_effect_key(id, percent) at the object's transform; it is released with its object.
// R5: `world_effects` (SimDecorations::effects(): footprint puffs at a world point, SurfaceFX wakes on an object) are
// synced in the same pass under world_effect_key(key): an entry with an owner follows that object's transform (skipped
// while the owner is not drawable / not in the frame), one without at its position / facing; kept while listed.
// R3: kSnapContained objects get no systems (released).
// frame.projectiles: each projectile with a model is synced under projectile_key(serial) at projectile_transform (a
// particle-file model as one system); while fogged (projectile_fogged) its systems are kept but not drawn.
inline uint64_t world_effect_key(uint64_t key) { return (1ull << 61) | (key & ((1ull << 61) - 1)); }
SimAttachStats sync_sim_frame(ProxyAttachments& attach, const InterpolatedFrame& frame, const SnapshotTypeTable& table,
                              SimModelSource& models, const SimInstanceBuilder& builder,
                              const SimInstanceBuilder::PoseSource* pose = nullptr, const SimAttachOptions* options = nullptr,
                              std::span<const SimWorldEffect> world_effects = {});

}  // namespace alamo::render::particles

// alamo_render_particles - particle systems on model proxies (R4, docs/re/ANIM_PARTICLES.md B.3).
//
// Per owner (a stable 64-bit key: sim object id, map placement) and per ALO proxy of the owner's model: the proxy
// name is cut at _ALT / _LOD and resolved through the ParticleLibrary (names that resolve to nothing load nothing);
// the system follows owner transform x the proxy's bone (Instance::bone_world, bind pose when empty) and is updated
// whether drawn or not. Drawing gates (B.3): the proxy's sub-object bit of Instance::mesh_hidden when the mask covers
// the proxies (HardpointVisibility layout: meshes, then proxies, then hardpoints), else the ALO hidden flag (0x603/07);
// the alt filter (proxy alt index >= 0 and != the model's alt -> hidden); a _LOD proxy only at the model's chosen
// LOD; and the parent fade (LIGHT_SCALE.w <= 0.01: not drawn). A system that finished by itself (one-shot effects) is
// not recreated while its owner stays. Owners not synced between begin() and end() are released (leave particles:
// their particles live out their lifetime).
#pragma once
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "alamo/render/particles/particle_data.hpp"
#include "alamo/render/particles/particle_sim.hpp"
#include "alamo/render/renderer.hpp"

namespace alamo::render::particles {

struct AttachStats {
    uint32_t owners = 0, proxies = 0, particle_proxies = 0, stripped = 0, unresolved = 0;
    uint32_t systems = 0, hidden = 0, finished = 0, released = 0;
};

class ProxyAttachments {
public:
    ProxyAttachments(ParticleWorld& world, ParticleLibrary& library) : world_(&world), lib_(&library) {}
    ~ProxyAttachments();
    ProxyAttachments(const ProxyAttachments&) = delete;
    ProxyAttachments& operator=(const ProxyAttachments&) = delete;

    void begin();
    // The proxies of `inst.model`'s asset under owner `key` (water: the system draws in bucket 7). `alt`: the owner's
    // current _ALT index (damage state, R1 SnapshotObject::alt); kModelAlt = the model's selection.alt.
    static constexpr int kModelAlt = -2;
    void sync(uint64_t key, const Instance& inst, bool water = false, int alt = kModelAlt);
    // Owners without their own identity (static placements, background objects): key = static_key(inst, salt).
    void sync_static(std::span<const Instance> instances, uint64_t salt);
    // An owner whose model IS a particle file (sim PARTICLE objects, B.1.1: death explosions, hit effects): one system at
    // `transform`, created once per key, not recreated after it finished.
    // `intensity` >= 0: the system's detail scale through the scalar setter (index 0, smoothing 0.1 s: SurfaceFX, R4
    // STRATEGIC_GAPS §6.2), set when it changes; < 0 leaves the scalars alone.
    void sync_effect(uint64_t key, SystemPtr def, const Mat3x4& transform, float fade, bool visible = true, float intensity = -1.0f);
    // The galactic planet corruption scalars (GALACTIC_REVEAL_CORRUPTION.md 4.5, snapped every frame with smoothing 0) on
    // every live system of owner `key` (after its sync_effect): colour RGBA = brightness (index 4), emission = scale
    // (index 1), detail = energy (index 0). Unknown keys are ignored.
    void set_effect_scalars(uint64_t key, float brightness, float scale, float energy);
    ParticleLibrary& library() { return *lib_; }
    void end();
    void clear();  // releases every owner

    static uint64_t static_key(const Instance& inst, uint64_t salt);
    size_t owners() const { return owners_.size(); }
    const AttachStats& stats() const { return stats_; }  // of the last begin() .. end()

private:
    struct ProxySys {
        ParticleWorld::Handle h = 0;
        uint32_t proxy = 0;
        uint32_t bone = 0;
        int alt = -1, lod = -1;
        bool finished = false;
        float intensity = -1.0f;  // last scalar-setter intensity (sync_effect)
    };
    struct Owner {
        uint64_t model_uid = 0;
        std::vector<ProxySys> systems;
        uint64_t seen = 0;
    };
    void release_owner(Owner& o);
    ParticleWorld* world_;
    ParticleLibrary* lib_;
    std::map<uint64_t, Owner> owners_;  // ordered: deterministic creation / release order
    uint64_t generation_ = 0;
    AttachStats stats_;
};

}  // namespace alamo::render::particles

// R3 render side: animated sim units for alamo_render --sim (pose upload through Instance::bone_world; the
// renderer's skinned path builds palette[i] = instance x bone_world[b] x inverse_bind_world[b] on the GPU side).
//
// FALLBACK VISUAL DRIVER. alamo_render --sim now poses units from the sim-owned world (sim::SimAnimation +
// AnimPoseCache, anim_pose_cache.hpp); this driver only serves hosts without a SimAnimation (--no-sim-anim). It keeps
// its own AnimWorld fed from the published snapshots: one row per object of
// the snapshot (model of its type, anim::type_anim_params), created with the model-created rules (§A.7), advanced
// once per new sim tick at t = tick / 30, idle fidgets through the idle behaviour rules and Move / idle switches
// from snapshot movement (a stand-in for the locomotor). It draws from its OWN SimRng, never the host's, so the
// sim and its CRC are untouched (the render-on vs render-off checks stay identical). Poses are evaluated after
// each update in parallel (per-row outputs) and kept until the next update.
//
// Thread use: update() and pose() on the frame thread (the producer of the render app's snapshots).
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/anim/anim_state.hpp"
#include "alamo/anim/type_params.hpp"
#include "alamo/assets/math.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/render/anim/anim_pose_cache.hpp"

namespace alamo::render {

struct AnimDriverObject {
    objects::ObjectId id = objects::kNoObjectId;
    objects::TypeIndex type = objects::kNone;
    Vec3 position{};
};

struct AnimDriverStats {
    uint32_t objects = 0, animated = 0, posed = 0, models = 0, models_with_clips = 0;
    uint32_t idle_plays = 0, move_plays = 0;  // cumulative
};

class SimAnimDriver {
public:
    SimAnimDriver(const FileProvider& files, const objects::GameObjectTypes& types, objects::ModeKind mode, uint32_t seed = 0x52335233u);

    // A new snapshot (`objects` ascending by id). Only acts when `tick` advanced since the last call.
    void update(uint64_t tick, std::span<const AnimDriverObject> objects, const ParallelFor& pf = serial_for);
    // Current model-space pose of `id` (per skeleton bone, assets::Mat3x4 layout) when its skeleton has `bones`
    // bones and it plays a bound clip; empty otherwise (draw the bind pose).
    std::span<const assets::Mat3x4> pose(objects::ObjectId id, size_t bones) const;

    const anim::AnimWorld& world() const { return world_; }
    const AnimDriverStats& stats() const { return stats_; }
    uint64_t tick() const { return tick_; }
    // Ordered digest of the driver's AnimWorld (determinism tests).
    uint64_t digest() const;

private:
    const anim::AnimModel* model_for(objects::TypeIndex type);

    const FileProvider* files_;
    const objects::GameObjectTypes* types_;
    objects::ModeKind mode_;
    anim::ClipCache clip_cache_;
    std::map<std::string, std::unique_ptr<anim::AnimModel>> models_;   // key: model|override (upper case)
    std::map<objects::TypeIndex, const anim::AnimModel*> by_type_;      // lookup only
    std::map<objects::TypeIndex, anim::TypeAnimParams> params_;
    std::map<objects::ObjectId, Vec3> last_pos_;
    anim::AnimWorld world_;
    SimRng rng_;
    uint64_t tick_ = 0;
    bool started_ = false;
    AnimPoseCache poses_;  // poses of the last update
    AnimDriverStats stats_;
};

}  // namespace alamo::render

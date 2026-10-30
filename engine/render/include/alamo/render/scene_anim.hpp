// alamo_render - R3 T6 scene models (docs/re/ANIM_PARTICLES.md A.5): models that are not sim objects but are registered
// for animation (here: the Model_To_Attach models of hardpoints - turrets, dishes, emitters) advance in tick step T6
// with the object time t = sim frame / 30. Each model plays its "<model stem>_IDLE_00" clip, looped with the original
// frame rule (wrap at count - 1), sampled with alamo_anim's pose math (the same as MapScene's background idles).
// [port] rate 1 and loop: an attached model has no GameObjectType for Idle_Anim_00_Rate_Mod / Loop_Idle_Anim_00.
// Visual only: nothing here is read by the sim. Frame thread only.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/render/renderer.hpp"

namespace alamo::render {

class SceneModelAnimator {
public:
    explicit SceneModelAnimator(const FileProvider* files) : files_(files) {}
    // Model-space pose (per skeleton bone) of `model`'s idle clip at sim tick `tick`; empty when the model has no idle
    // clip, the clip does not bind (bones != ALA bones + 1) or the build has no engine/anim. The span stays valid until
    // pose() is asked for the same model at another tick (instances of one model in a frame share it).
    std::span<const Mat3x4> pose(const GpuModel& model, uint64_t tick);
    size_t models() const { return entries_.size(); }
    size_t animated() const;  // models with a bound idle clip

    // Frame position of the idle clip after `seconds` (loop: wrap at count - 1; count < 2 -> 0). Exposed for tests.
    static float frame_position(double seconds, float fps, uint32_t count);

private:
    struct Entry {
        bool loaded = false;
        std::shared_ptr<const void> clip;  // alamo_anim skeleton + bound clip (scene_anim.cpp), null = none
        uint64_t tick = ~uint64_t{0};
        std::vector<Mat3x4> world;
    };
    const FileProvider* files_;
    std::map<uint64_t, Entry> entries_;  // by GpuModel::uid (lookup only)
};

}  // namespace alamo::render

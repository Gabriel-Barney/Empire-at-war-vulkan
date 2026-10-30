// R3 render side: model-space poses of an anim::AnimWorld for the renderer (Instance::bone_world) and the R4 particle
// proxies. update() evaluates every row that plays a bound clip (primary or overlay) in parallel with per-row outputs
// and keeps the result until the next update; pose() is then a PURE getter (no evaluation, no state change), so the
// instance build and the particle proxies may both call it for the same object in one frame.
//
// The world is only read (const AnimWorld&): with a sim-owned world (sim::SimAnimation, "T5.anim") update() must run
// between ticks on the thread that ticks the host (alamo_render --sim: right after the tick, before the snapshot is
// published); the cache never touches the sim, its RNG or its CRC.
#pragma once
#include <cstdint>
#include <span>
#include <vector>

#include "alamo/anim/anim_state.hpp"
#include "alamo/assets/math.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo::render {

struct AnimPoseStats {
    uint32_t rows = 0;   // rows of the world at the last update
    uint32_t posed = 0;  // rows playing a bound clip (poses kept)
};

class AnimPoseCache {
public:
    // Re-evaluates the poses of `world` (null = no world: every pose() empty).
    void update(const anim::AnimWorld* world, const ParallelFor& pf = serial_for);
    void clear();
    // Model-space pose of `id` (per skeleton bone, assets::Mat3x4 layout) when its skeleton has `bones` bones and it
    // played a bound clip at the last update; empty otherwise (draw the bind pose).
    std::span<const assets::Mat3x4> pose(objects::ObjectId id, size_t bones) const;
    // R3 per-bone "animated visible" bits of the same evaluation (ANIM_PARTICLES.md A.6: 0 = the clip hides the bone's
    // sub-objects; crossfade = either clip visible; overlay bones use the overlay clip's bit). Same span rules as pose();
    // empty when every bone is visible (nothing to hide). For SimInstanceBuilder::set_visibility_source.
    std::span<const uint8_t> visibility(objects::ObjectId id, size_t bones) const;
    const AnimPoseStats& stats() const { return stats_; }
    std::span<const objects::ObjectId> ids() const { return ids_; }  // rows of the last update, ascending

private:
    std::vector<assets::Mat3x4> data_;
    std::vector<uint8_t> visible_;      // per posed bone (same offsets as data_)
    std::vector<uint8_t> any_hidden_;   // per row: some bone hidden
    std::vector<uint32_t> offset_, count_;
    std::vector<objects::ObjectId> ids_;
    AnimPoseStats stats_;
};

}  // namespace alamo::render

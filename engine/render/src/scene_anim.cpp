// R3 T6 scene models. See scene_anim.hpp.
#include "alamo/render/scene_anim.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <string_view>

#ifdef ALAMO_RENDER_HAVE_ANIM
#include "alamo/anim/anim_state.hpp"
#include "alamo/anim/clip.hpp"
#endif

namespace alamo::render {

namespace {
#ifdef ALAMO_RENDER_HAVE_ANIM
struct BoundIdle {
    anim::Skeleton skeleton;
    anim::ClipRef clip;
};
#endif
}  // namespace

float SceneModelAnimator::frame_position(double seconds, float fps, uint32_t count) {
    if (count < 2 || !(seconds > 0.0) || !(fps > 0.0f)) return 0.0f;
    return static_cast<float>(std::fmod(seconds * static_cast<double>(fps), static_cast<double>(count - 1)));
}

size_t SceneModelAnimator::animated() const {
    size_t n = 0;
    for (const auto& [uid, e] : entries_) n += e.clip ? 1u : 0u;
    return n;
}

std::span<const Mat3x4> SceneModelAnimator::pose(const GpuModel& model, uint64_t tick) {
    if (!model.asset || model.asset->skeleton.size() == 0) return {};
    Entry& e = entries_[model.uid];
    if (!e.loaded) {
        e.loaded = true;
#ifdef ALAMO_RENDER_HAVE_ANIM
        std::string_view name = model.asset->name;
        const size_t b = name.find_last_of("\\/");
        if (b != std::string_view::npos) name = name.substr(b + 1);
        const size_t dot = name.find_last_of('.');
        if (dot != std::string_view::npos) name = name.substr(0, dot);
        std::vector<uint8_t> bytes;
        const std::string path = assets::animation_path(std::string(name) + "_IDLE_00");
        auto clip = std::make_shared<anim::Clip>();
        if (files_ && !name.empty() && files_->exists(path) && files_->read(path, bytes) && anim::parse_clip(bytes, path, *clip) &&
            clip->frame_count > 0) {
            const assets::Skeleton& s = model.asset->skeleton;
            std::vector<anim::Mat34> bind(s.size());
            static_assert(sizeof(anim::Mat34) == sizeof(Mat3x4), "pose layouts differ");
            std::memcpy(static_cast<void*>(bind.data()), static_cast<const void*>(s.bind_local.data()), s.size() * sizeof(Mat3x4));
            auto bound = std::make_shared<BoundIdle>();
            bound->skeleton = anim::skeleton_from_arrays(s.names, s.parents, s.billboard_mode, bind);
            anim::AnimModel binder;  // binding rule: model bones == ALA bones + 1
            binder.skeleton = bound->skeleton;
            binder.has_skeleton = true;
            binder.clips[0].push_back(anim::ClipRef{clip, false, {}});
            anim::bind_clips(binder);
            if (binder.clips[0][0].bound) {
                bound->clip = binder.clips[0][0];
                e.clip = std::move(bound);
            }
        }
#endif
    }
    if (!e.clip) return {};
#ifdef ALAMO_RENDER_HAVE_ANIM
    if (e.tick != tick) {
        const auto& bi = *static_cast<const BoundIdle*>(e.clip.get());
        const float frame = frame_position(static_cast<double>(tick) / 30.0, bi.clip.clip->fps, bi.clip.clip->frame_count);
        std::vector<anim::Mat34> world(bi.skeleton.size());
        anim::clip_world_pose(bi.skeleton, bi.clip, frame, world);
        e.world.resize(world.size());
        std::memcpy(static_cast<void*>(e.world.data()), world.data(), world.size() * sizeof(Mat3x4));
        e.tick = tick;
    }
#else
    (void)tick;
    (void)files_;
#endif
    return e.world;
}

}  // namespace alamo::render

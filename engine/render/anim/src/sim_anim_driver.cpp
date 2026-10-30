#include "alamo/render/anim/sim_anim_driver.hpp"

#include <algorithm>

#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::render {

namespace {
constexpr float kMoveEpsilon2 = 0.01f * 0.01f;  // snapshot displacement that counts as moving (per tick)
}

SimAnimDriver::SimAnimDriver(const FileProvider& files, const objects::GameObjectTypes& types, objects::ModeKind mode, uint32_t seed)
    : files_(&files), types_(&types), mode_(mode), rng_(seed) {}

const anim::AnimModel* SimAnimDriver::model_for(objects::TypeIndex type) {
    const auto hit = by_type_.find(type);
    if (hit != by_type_.end()) return hit->second;
    const anim::TypeAnimParams p = anim::type_anim_params(*types_, type, mode_);
    params_[type] = p;
    const anim::AnimModel* out = nullptr;
    if (!p.model.empty()) {
        const std::string key = normalize_name(p.model) + "|" + normalize_name(p.anim_override);
        auto& slot = models_[key];
        if (!slot) {
            slot = std::make_unique<anim::AnimModel>();
            anim::load_anim_model(*files_, p.model, p.anim_override, *slot, &clip_cache_);
            ++stats_.models;
            if (slot->has_any_clip()) ++stats_.models_with_clips;
        }
        out = slot.get();
    }
    by_type_[type] = out;
    return out;
}

void SimAnimDriver::update(uint64_t tick, std::span<const AnimDriverObject> objects, const ParallelFor& pf) {
    if (started_ && tick == tick_) return;
    started_ = true;
    tick_ = tick;
    stats_.objects = static_cast<uint32_t>(objects.size());

    // Rows: drop objects that left the snapshot, create the new ones (ascending id).
    {
        std::vector<anim::ObjectId> gone;
        size_t k = 0;
        for (const anim::ObjectId id : world_.ids()) {
            while (k < objects.size() && objects[k].id < id) ++k;
            if (k >= objects.size() || objects[k].id != id) gone.push_back(id);
        }
        for (const anim::ObjectId id : gone) {
            world_.remove(id);
            last_pos_.erase(id);
        }
    }
    for (const AnimDriverObject& o : objects) {
        if (world_.contains(o.id)) continue;
        const anim::AnimModel* m = model_for(o.type);
        if (!m || !m->has_any_clip()) continue;
        const anim::TypeAnimParams& p = params_[o.type];
        anim::AnimCreateParams cp;
        cp.idle_type = anim::kAnimIdle;
        cp.loop_idle = p.loop_idle;
        cp.idle_rate = p.idle_rate;
        world_.create(o.id, m, cp, &rng_, tick);
        last_pos_[o.id] = o.position;
    }

    world_.advance(tick, pf, 64);

    // Behaviour stand-ins, serial, newest first (the original's service order).
    for (auto it = objects.rbegin(); it != objects.rend(); ++it) {
        const AnimDriverObject& o = *it;
        if (!world_.contains(o.id)) continue;
        const anim::AnimModel* m = world_.model(o.id);
        Vec3& last = last_pos_[o.id];
        const float dx = o.position.x - last.x, dy = o.position.y - last.y, dz = o.position.z - last.z;
        const bool moving = dx * dx + dy * dy + dz * dz > kMoveEpsilon2;
        last = o.position;
        const int32_t cur = world_.current_type(o.id);
        if (moving && m->variant_count(anim::kAnimMove) > 0) {
            if (cur != anim::kAnimMove) {
                anim::PlayRequest req;
                req.type = anim::kAnimMove;
                const uint32_t variants = m->variant_count(anim::kAnimMove);
                req.index = static_cast<uint32_t>(rng_.range_int(0, static_cast<int32_t>(variants) - 1));
                req.loop = true;
                req.blend_seconds = world_.tunables().move_blend_seconds;
                req.speed = params_[o.type].movement_anim_speed;
                const anim::ClipRef* c = m->clip(anim::kAnimMove, req.index);
                const uint32_t count = c && c->clip ? c->clip->frame_count : 0u;
                // Random start frame [0, count - 1] (§A.7 walk row).
                if (count > 1) req.start_frame = static_cast<float>(rng_.range_int(0, static_cast<int32_t>(count) - 1));
                if (world_.play(o.id, req, nullptr, tick) == anim::PlayResult::Played) ++stats_.move_plays;
            }
            continue;
        }
        if (cur == anim::kAnimMove) {  // stopped: back to the idle loop
            anim::PlayRequest req;
            req.type = anim::kAnimIdle;
            req.loop = params_[o.type].loop_idle;
            req.blend_seconds = world_.tunables().move_blend_seconds;
            req.speed = params_[o.type].idle_rate;
            if (world_.play(o.id, req, &rng_, tick) == anim::PlayResult::Played) ++stats_.idle_plays;
            continue;
        }
        if (world_.service_idle(o.id, anim::kAnimIdle, false, rng_, tick)) ++stats_.idle_plays;
    }

    // Poses (model space) for every row that plays a bound clip; parallel with per-row outputs (AnimPoseCache).
    poses_.update(&world_, pf);
    stats_.animated = poses_.stats().rows;
    stats_.posed = poses_.stats().posed;
}

std::span<const assets::Mat3x4> SimAnimDriver::pose(objects::ObjectId id, size_t bones) const { return poses_.pose(id, bones); }

uint64_t SimAnimDriver::digest() const {
    StateHasher h;
    world_.hash_state(h);
    h.u64(rng_.state());
    return h.digest();
}

}  // namespace alamo::render

#include "alamo/anim/anim_state.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "alamo/core/hash.hpp"
#include "alamo/core/sim_crc.hpp"
#include "alamo/core/sim_rng.hpp"

namespace alamo::anim {

namespace {

double sim_time(uint64_t frame) { return static_cast<double>(frame) / kSimTicksPerSecond; }

template <class T>
void insert_at(std::vector<T>& v, size_t at, const T& value) {
    v.insert(v.begin() + static_cast<std::ptrdiff_t>(at), value);
}
template <class T>
void erase_at(std::vector<T>& v, size_t at) {
    v.erase(v.begin() + static_cast<std::ptrdiff_t>(at));
}

}  // namespace

// ---- frame rules -------------------------------------------------------------------------------------

float advance_loop(float frame, float step, uint32_t count) {
    if (count < 2) return 0.0f;
    const float n = static_cast<float>(count - 1);
    frame += step;
    while (frame >= n) frame -= n;  // repeated subtraction (§A.5), not fmod
    return frame;
}

float advance_one_shot(float frame, float step, uint32_t count, bool& finished) {
    finished = false;
    const float last = count >= 2 ? static_cast<float>(count - 2) : 0.0f;
    frame += step;
    if (frame >= last) {
        frame = last;
        finished = true;
    }
    return frame;
}

void sample_keys(float frame, uint32_t count, uint32_t& a, uint32_t& b, float& t) {
    a = b = 0;
    t = 0.0f;
    if (count < 2 || !(frame >= 0.0f)) return;  // also NaN / negative -> frame 0
    const uint32_t n = count - 1;
    const float fl = std::floor(frame);
    t = frame - fl;
    const uint64_t ia = fl < 4294967295.0f ? static_cast<uint64_t>(fl) : 0u;
    a = static_cast<uint32_t>(ia % n);
    b = static_cast<uint32_t>((ia + 1) % n);
}

// ---- membership --------------------------------------------------------------------------------------

uint32_t AnimWorld::row(ObjectId id) const {
    const auto it = std::lower_bound(id_.begin(), id_.end(), id);
    return (it != id_.end() && *it == id) ? static_cast<uint32_t>(it - id_.begin()) : kNoRow;
}

uint32_t AnimWorld::insert_row(ObjectId id) {
    const auto it = std::lower_bound(id_.begin(), id_.end(), id);
    const size_t at = static_cast<size_t>(it - id_.begin());
    if (it != id_.end() && *it == id) return static_cast<uint32_t>(at);
    insert_at(id_, at, id);
    insert_at<const AnimModel*>(model_, at, nullptr);
    insert_at<uint32_t>(model_crc_, at, 0u);
    insert_at(last_time_, at, 0.0);
    insert_at<uint8_t>(frozen_, at, 0);
    insert_at<uint8_t>(lock_, at, 0);
    insert_at<uint8_t>(frame_changed_, at, 0);
    insert_at<uint8_t>(near_end_, at, 0);
    insert_at<uint8_t>(berserk_, at, 0);
    insert_at<uint64_t>(idle_timer_, at, 0);
    for (int s = 0; s < kSlotCount; ++s) {
        insert_at<int32_t>(type_[s], at, kNoAnimType);
        insert_at<uint32_t>(variant_[s], at, 0);
        insert_at<uint8_t>(mode_[s], at, static_cast<uint8_t>(SlotMode::Finished));
        insert_at(frame_[s], at, 0.0f);
        insert_at(speed_[s], at, 1.0f);
        insert_at<uint8_t>(timed_[s], at, 0);
        insert_at(timed_left_[s], at, 0.0f);
    }
    insert_at<uint8_t>(fade_active_, at, 0);
    insert_at(fade_duration_, at, 0.0f);
    insert_at(fade_remaining_, at, 0.0f);
    insert_at(fade_weight_, at, 1.0f);
    return static_cast<uint32_t>(at);
}

bool AnimWorld::remove(ObjectId id) {
    const uint32_t r = row(id);
    if (r == kNoRow) return false;
    erase_at(id_, r);
    erase_at(model_, r);
    erase_at(model_crc_, r);
    erase_at(last_time_, r);
    erase_at(frozen_, r);
    erase_at(lock_, r);
    erase_at(frame_changed_, r);
    erase_at(near_end_, r);
    erase_at(berserk_, r);
    erase_at(idle_timer_, r);
    for (int s = 0; s < kSlotCount; ++s) {
        erase_at(type_[s], r);
        erase_at(variant_[s], r);
        erase_at(mode_[s], r);
        erase_at(frame_[s], r);
        erase_at(speed_[s], r);
        erase_at(timed_[s], r);
        erase_at(timed_left_[s], r);
    }
    erase_at(fade_active_, r);
    erase_at(fade_duration_, r);
    erase_at(fade_remaining_, r);
    erase_at(fade_weight_, r);
    return true;
}

void AnimWorld::add(ObjectId id, const AnimModel* model, uint64_t frame) {
    const uint32_t r = insert_row(id);
    model_[r] = model;
    model_crc_[r] = model ? name_crc(model->model) : 0u;  // cached for hash_state
    last_time_[r] = sim_time(frame);
    frozen_[r] = lock_[r] = frame_changed_[r] = near_end_[r] = berserk_[r] = 0;
    idle_timer_[r] = 0;
    for (int s = 0; s < kSlotCount; ++s) clear_slot(r, static_cast<Slot>(s));
    fade_active_[r] = 0;
    fade_duration_[r] = fade_remaining_[r] = 0.0f;
    fade_weight_[r] = 1.0f;
}

void AnimWorld::create(ObjectId id, const AnimModel* model, const AnimCreateParams& p, SimRng* rng, uint64_t frame) {
    add(id, model, frame);
    const uint32_t r = row(id);
    if (!model || !model->clip(p.idle_type, 0)) return;  // no clip: no play, no draw
    PlayRequest req;
    req.type = p.idle_type;
    req.index = 0;
    req.loop = p.loop_idle;
    req.blend_seconds = 0.0f;
    req.speed = p.idle_rate;
    if (play_row(r, req, nullptr, frame) != PlayResult::Played) return;
    const ClipRef* c = slot_clip(r, kPrimary);
    const uint32_t count = c && c->clip ? c->clip->frame_count : 0u;
    if (rng && count > 0) {
        // Random start frame in [0, count - 1] ([port] integer draw), applied like 0x265490.
        const int32_t f = rng->range_int(0, static_cast<int32_t>(count) - 1);
        frame_[kPrimary][r] = count < 2 ? 0.0f : static_cast<float>(f);
    }
}

const AnimModel* AnimWorld::model(ObjectId id) const {
    const uint32_t r = row(id);
    return r == kNoRow ? nullptr : model_[r];
}

void AnimWorld::clear_slot(uint32_t r, Slot s) {
    type_[s][r] = kNoAnimType;
    variant_[s][r] = 0;
    mode_[s][r] = static_cast<uint8_t>(SlotMode::Finished);
    frame_[s][r] = 0.0f;
    speed_[s][r] = 1.0f;
    timed_[s][r] = 0;
    timed_left_[s][r] = 0.0f;
}

const ClipRef* AnimWorld::slot_clip(uint32_t r, Slot s) const {
    const AnimModel* m = model_[r];
    if (!m || type_[s][r] == kNoAnimType) return nullptr;
    const ClipRef* c = m->clip(type_[s][r], variant_[s][r]);
    return c && c->clip ? c : nullptr;
}

// ---- play --------------------------------------------------------------------------------------------

PlayResult AnimWorld::play_row(uint32_t r, const PlayRequest& req_in, SimRng* rng, uint64_t frame) {
    PlayRequest req = req_in;
    // Berserker (§A.4 item 1): any request but Attack / Attack Idle / Idle (or a clear) becomes SpinMove, looped,
    // speed 1; a running spin is left alone until it is within 2 frames of its end ([port] frame + 2 >= count - 1).
    if (berserk_[r] && req.type != kNoAnimType && req.type != kAnimAttack && req.type != kAnimAttackIdle && req.type != kAnimIdle) {
        if (type_[kPrimary][r] == kAnimSpinMove) {
            const ClipRef* spin = slot_clip(r, kPrimary);
            if (spin && frame_[kPrimary][r] + 2.0f < static_cast<float>(spin->clip->frame_count) - 1.0f) return PlayResult::Locked;
        }
        req.type = kAnimSpinMove;
        req.index = 0;  // [port] variant 0
        req.loop = true;
        req.speed = 1.0f;
        req.start_frame = 0.0f;
    }
    // Lock (object +0x3a0 bit 3): while set, calls without lock are ignored; accepted calls rewrite it.
    if (lock_[r] && !req.lock) return PlayResult::Locked;
    lock_[r] = req.lock ? 1 : 0;
    const AnimModel* m = model_[r];
    uint32_t index = req.index;
    if (index == kRandomVariant) {
        const uint32_t count = m ? m->variant_count(req.type) : 0u;
        index = (count > 0 && rng) ? static_cast<uint32_t>(rng->range_int(0, static_cast<int32_t>(count) - 1)) : 0u;
    }
    if (req.type == kNoAnimType) {
        clear_slot(r, kPrimary);
        clear_slot(r, kPrevious);
        fade_active_[r] = 0;
        fade_weight_[r] = 1.0f;
        last_time_[r] = sim_time(frame);
        return PlayResult::Cleared;
    }
    const ClipRef* c = m ? m->clip(req.type, index) : nullptr;
    if (!c || !c->clip) return PlayResult::NoClip;

    // Crossfade (0x265560).
    const bool has_primary = slot_clip(r, kPrimary) != nullptr;
    if (req.blend_seconds > 0.0f && has_primary) {
        const bool running = fade_active_[r] != 0 && fade_duration_[r] > 0.0f;
        const float rem0 = fade_remaining_[r], dur0 = fade_duration_[r];
        const float w = running ? 1.0f - rem0 / dur0 : 1.0f;
        const bool keep_previous = running && w <= 0.25f;
        if (!keep_previous) {
            type_[kPrevious][r] = type_[kPrimary][r];
            variant_[kPrevious][r] = variant_[kPrimary][r];
            mode_[kPrevious][r] = mode_[kPrimary][r];
            frame_[kPrevious][r] = frame_[kPrimary][r];
            speed_[kPrevious][r] = speed_[kPrimary][r];
            timed_[kPrevious][r] = timed_[kPrimary][r];
            timed_left_[kPrevious][r] = timed_left_[kPrimary][r];
        }
        if (running && req.blend_mode == 2) {
            fade_remaining_[r] = (rem0 / dur0) * req.blend_seconds;
            fade_duration_[r] = req.blend_seconds;
        } else if (running && req.blend_mode == 3) {
            fade_remaining_[r] = (1.0f - rem0 / dur0) * req.blend_seconds;
            fade_duration_[r] = req.blend_seconds;
        } else if (!keep_previous) {
            fade_remaining_[r] = req.blend_seconds;
            fade_duration_[r] = req.blend_seconds;
        }
        fade_active_[r] = 1;
        fade_weight_[r] = 1.0f - fade_remaining_[r] / fade_duration_[r];
    } else {
        clear_slot(r, kPrevious);  // hard cut
        fade_active_[r] = 0;
        fade_duration_[r] = fade_remaining_[r] = 0.0f;
        fade_weight_[r] = 1.0f;
    }

    type_[kPrimary][r] = req.type;
    variant_[kPrimary][r] = index;
    mode_[kPrimary][r] = static_cast<uint8_t>(req.loop ? SlotMode::Loop : SlotMode::OneShot);
    frame_[kPrimary][r] = 0.0f;
    speed_[kPrimary][r] = req.speed;
    timed_[kPrimary][r] = req.timed_seconds > 0.0f ? 1 : 0;
    timed_left_[kPrimary][r] = req.timed_seconds > 0.0f ? req.timed_seconds : 0.0f;
    if (req.start_frame > 0.0f) frame_[kPrimary][r] = c->clip->frame_count < 2 ? 0.0f : req.start_frame;
    last_time_[r] = sim_time(frame);
    frame_changed_[r] = 1;
    near_end_[r] = 0;
    return PlayResult::Played;
}

PlayResult AnimWorld::play(ObjectId id, const PlayRequest& req, SimRng* rng, uint64_t frame) {
    const uint32_t r = row(id);
    return r == kNoRow ? PlayResult::NoObject : play_row(r, req, rng, frame);
}

PlayResult AnimWorld::play_overlay(ObjectId id, int32_t type, uint32_t index, float speed, float timed_seconds, SimRng* rng, uint64_t frame) {
    const uint32_t r = row(id);
    if (r == kNoRow) return PlayResult::NoObject;
    const AnimModel* m = model_[r];
    if (index == kRandomVariant) {
        const uint32_t count = m ? m->variant_count(type) : 0u;
        index = (count > 0 && rng) ? static_cast<uint32_t>(rng->range_int(0, static_cast<int32_t>(count) - 1)) : 0u;
    }
    if (type == kNoAnimType) {
        clear_slot(r, kOverlay);
        return PlayResult::Cleared;
    }
    const ClipRef* c = m ? m->clip(type, index) : nullptr;
    if (!c || !c->clip) return PlayResult::NoClip;
    type_[kOverlay][r] = type;
    variant_[kOverlay][r] = index;
    mode_[kOverlay][r] = static_cast<uint8_t>(SlotMode::OneShot);
    frame_[kOverlay][r] = 0.0f;
    speed_[kOverlay][r] = speed;
    timed_[kOverlay][r] = timed_seconds > 0.0f ? 1 : 0;
    timed_left_[kOverlay][r] = timed_seconds > 0.0f ? timed_seconds : 0.0f;
    last_time_[r] = sim_time(frame);
    return PlayResult::Played;
}

void AnimWorld::clear_overlay(ObjectId id) {
    const uint32_t r = row(id);
    if (r != kNoRow) clear_slot(r, kOverlay);
}

void AnimWorld::set_speed(ObjectId id, float speed) {
    const uint32_t r = row(id);
    if (r != kNoRow) speed_[kPrimary][r] = speed;
}

PlayResult AnimWorld::switch_preserving_phase(ObjectId id, int32_t type, uint32_t index, bool loop, float blend, float speed, uint64_t frame) {
    const uint32_t r = row(id);
    if (r == kNoRow) return PlayResult::NoObject;
    const ClipRef* old = slot_clip(r, kPrimary);
    const float old_frame = frame_[kPrimary][r];
    const uint32_t old_count = old ? old->clip->frame_count : 0u;
    PlayRequest req;
    req.type = type;
    req.index = index;
    req.loop = loop;
    req.blend_seconds = blend;
    req.speed = speed;
    const PlayResult res = play_row(r, req, nullptr, frame);
    if (res != PlayResult::Played) return res;
    const ClipRef* now = slot_clip(r, kPrimary);
    const uint32_t new_count = now ? now->clip->frame_count : 0u;
    if (old_count > 0 && new_count >= 2) frame_[kPrimary][r] = old_frame * static_cast<float>(new_count) / static_cast<float>(old_count);
    return res;
}

void AnimWorld::set_frozen(ObjectId id, bool frozen) {
    const uint32_t r = row(id);
    if (r != kNoRow) frozen_[r] = frozen ? 1 : 0;
}

bool AnimWorld::frozen(ObjectId id) const {
    const uint32_t r = row(id);
    return r != kNoRow && frozen_[r] != 0;
}

void AnimWorld::set_berserker(ObjectId id, bool active) {
    const uint32_t r = row(id);
    if (r != kNoRow) berserk_[r] = active ? 1 : 0;
}

bool AnimWorld::berserker(ObjectId id) const {
    const uint32_t r = row(id);
    return r != kNoRow && berserk_[r] != 0;
}

bool AnimWorld::bone_visibility(ObjectId id, std::span<uint8_t> visible) const {
    const uint32_t r = row(id);
    if (r == kNoRow || !model_[r] || !model_[r]->has_skeleton) return false;
    const size_t n = model_[r]->skeleton.size();
    if (visible.size() < n) return false;
    for (size_t b = 0; b < n; ++b) visible[b] = 1;
    const ClipRef* prim = slot_clip(r, kPrimary);
    const ClipRef* prev = fade_active_[r] ? slot_clip(r, kPrevious) : nullptr;
    if (prev && !prev->bound) prev = nullptr;
    if (prim && prim->bound) {
        uint32_t a = 0, b = 0, pa = 0, pb = 0;
        float t = 0.0f, pt = 0.0f;
        sample_keys(frame_[kPrimary][r], prim->clip->frame_count, a, b, t);
        if (prev) sample_keys(frame_[kPrevious][r], prev->clip->frame_count, pa, pb, pt);
        for (size_t i = 0; i < prim->track_bone.size(); ++i) {
            const int32_t bone = prim->track_bone[i];
            if (bone < 0 || static_cast<size_t>(bone) >= n) continue;
            bool vis = prim->clip->visible(static_cast<uint32_t>(i), a);
            if (prev && !vis)
                for (size_t k = 0; k < prev->track_bone.size(); ++k)
                    if (prev->track_bone[k] == bone) {
                        vis = prev->clip->visible(static_cast<uint32_t>(k), pa);
                        break;
                    }
            visible[static_cast<size_t>(bone)] = vis ? 1 : 0;
        }
    }
    const ClipRef* ov = slot_clip(r, kOverlay);
    if (ov && ov->bound) {
        uint32_t a = 0, b = 0;
        float t = 0.0f;
        sample_keys(frame_[kOverlay][r], ov->clip->frame_count, a, b, t);
        for (size_t i = 0; i < ov->track_bone.size(); ++i) {
            const int32_t bone = ov->track_bone[i];
            if (bone < 0 || static_cast<size_t>(bone) >= n || ov->clip->tracks[i].overlay_excluded) continue;
            visible[static_cast<size_t>(bone)] = ov->clip->visible(static_cast<uint32_t>(i), a) ? 1 : 0;
        }
    }
    return true;
}

// ---- advance -----------------------------------------------------------------------------------------

void AnimWorld::advance_row(uint32_t r, double t) {
    if (frozen_[r]) return;
    const float dt = static_cast<float>(t - last_time_[r]);
    last_time_[r] = t;
    frame_changed_[r] = 0;
    const ClipRef* clips[kSlotCount] = {slot_clip(r, kPrimary), slot_clip(r, kPrevious), slot_clip(r, kOverlay)};
    for (int si = 0; si < kSlotCount; ++si) {
        const ClipRef* c = clips[si];
        if (!c) continue;
        const Slot s = static_cast<Slot>(si);
        if (static_cast<SlotMode>(mode_[s][r]) == SlotMode::Finished) continue;
        float speed = speed_[s][r];
        if (s == kPrevious && speed == 0.0f) speed = clips[kPrimary] ? speed_[kPrimary][r] : 1.0f;
        const uint32_t count = c->clip->frame_count;
        const float step = c->clip->fps * dt * speed;
        const float before = frame_[s][r];
        if (static_cast<SlotMode>(mode_[s][r]) == SlotMode::Loop) {
            frame_[s][r] = advance_loop(before, step, count);
        } else {
            bool finished = false;
            frame_[s][r] = advance_one_shot(before, step, count, finished);
            if (finished) mode_[s][r] = static_cast<uint8_t>(SlotMode::Finished);
        }
        if (timed_[s][r]) {
            timed_left_[s][r] -= dt;
            if (timed_left_[s][r] <= 0.0f) {
                mode_[s][r] = static_cast<uint8_t>(SlotMode::Finished);
                speed_[s][r] = 1.0f;
                timed_[s][r] = 0;
            }
        }
        if (s == kPrimary) {
            if (frame_[s][r] != before) frame_changed_[r] = 1;
            if (count > 0 && frame_[s][r] >= static_cast<float>(count) - 2.5f) near_end_[r] = 1;
        }
    }
    if (fade_active_[r]) {
        fade_remaining_[r] -= dt;
        if (fade_remaining_[r] <= 0.0f || !(fade_duration_[r] > 0.0f)) {
            fade_active_[r] = 0;
            fade_remaining_[r] = 0.0f;
            fade_weight_[r] = 1.0f;
            clear_slot(r, kPrevious);
        } else {
            fade_weight_[r] = 1.0f - fade_remaining_[r] / fade_duration_[r];
        }
        frame_changed_[r] = 1;
    }
}

void AnimWorld::advance(uint64_t frame, const ParallelFor& pf, uint32_t grain) {
    const double t = sim_time(frame);
    const uint32_t n = static_cast<uint32_t>(id_.size());
    if (n == 0) return;
    pf(n, grain == 0 ? 1u : grain, [&](uint32_t begin, uint32_t end) {
        for (uint32_t r = begin; r < end; ++r) advance_row(r, t);
    });
}

void AnimWorld::advance_one(ObjectId id, uint64_t frame) {
    const uint32_t r = row(id);
    if (r != kNoRow) advance_row(r, sim_time(frame));
}

// ---- queries -----------------------------------------------------------------------------------------

SlotView AnimWorld::slot(ObjectId id, Slot s) const {
    SlotView v;
    const uint32_t r = row(id);
    if (r == kNoRow) return v;
    v.type = type_[s][r];
    v.variant = variant_[s][r];
    v.mode = static_cast<SlotMode>(mode_[s][r]);
    v.frame = frame_[s][r];
    v.speed = speed_[s][r];
    v.timed = timed_[s][r] != 0;
    v.timed_remaining = timed_left_[s][r];
    v.clip = slot_clip(r, s);
    return v;
}

bool AnimWorld::crossfading(ObjectId id) const {
    const uint32_t r = row(id);
    return r != kNoRow && fade_active_[r] != 0;
}

float AnimWorld::crossfade_weight(ObjectId id) const {
    const uint32_t r = row(id);
    return r == kNoRow || !fade_active_[r] ? 1.0f : fade_weight_[r];
}

bool AnimWorld::frame_changed(ObjectId id) const {
    const uint32_t r = row(id);
    return r != kNoRow && frame_changed_[r] != 0;
}

bool AnimWorld::near_end(ObjectId id) const {
    const uint32_t r = row(id);
    return r != kNoRow && near_end_[r] != 0;
}

bool AnimWorld::locked(ObjectId id) const {
    const uint32_t r = row(id);
    return r != kNoRow && lock_[r] != 0;
}

uint32_t AnimWorld::current_frame_count(ObjectId id) const {
    const uint32_t r = row(id);
    if (r == kNoRow) return 0;
    const ClipRef* c = slot_clip(r, kPrimary);
    return c ? c->clip->frame_count : 0u;
}

uint64_t AnimWorld::idle_timer(ObjectId id) const {
    const uint32_t r = row(id);
    return r == kNoRow ? 0 : idle_timer_[r];
}

void AnimWorld::set_idle_timer(ObjectId id, uint64_t frame) {
    const uint32_t r = row(id);
    if (r != kNoRow) idle_timer_[r] = frame;
}

bool AnimWorld::service_idle(ObjectId id, int32_t idle_type, bool crouch_changed, SimRng& rng, uint64_t frame) {
    const uint32_t r = row(id);
    if (r == kNoRow) return false;
    const ClipRef* cur = slot_clip(r, kPrimary);
    const bool finished = cur && static_cast<SlotMode>(mode_[kPrimary][r]) == SlotMode::Finished;
    if (cur && !finished && !crouch_changed) return false;
    const uint32_t count = model_[r] ? model_[r]->variant_count(idle_type) : 0u;
    uint32_t variant = 0;
    if (frame >= idle_timer_[r]) {
        if (count > 1) variant = static_cast<uint32_t>(rng.range_int(1, static_cast<int32_t>(count) - 1));
        idle_timer_[r] = frame + static_cast<uint64_t>(rng.range_int(210, 450));
    }
    PlayRequest req;
    req.type = idle_type;
    req.index = variant;
    req.loop = false;
    req.blend_seconds = (cur && type_[kPrimary][r] == idle_type) ? 0.0f : tunables_.idle_blend_seconds;
    req.speed = 1.0f;
    return play_row(r, req, nullptr, frame) == PlayResult::Played;
}

// ---- pose --------------------------------------------------------------------------------------------

bool AnimWorld::sample_local(uint32_t r, size_t n, std::span<Mat34> local, std::span<uint8_t> visible, int32_t chain_bone) const {
    const AnimModel* m = model_[r];
    if (!m || !m->has_skeleton || m->skeleton.size() != n || local.size() < n) return false;
    const Skeleton& sk = m->skeleton;
    for (size_t b = 0; b < n; ++b) local[b] = sk.bind_local[b];
    const bool want_vis = visible.size() >= n;
    if (want_vis)
        for (size_t b = 0; b < n; ++b) visible[b] = 1;
    // Chain mask (gameplay single-bone reads evaluate only the bone and its ancestors).
    std::vector<uint8_t> mask;
    if (chain_bone >= 0) {
        mask.assign(n, 0);
        for (int32_t b = chain_bone; b >= 0 && static_cast<size_t>(b) < n; b = sk.parents[static_cast<size_t>(b)]) mask[static_cast<size_t>(b)] = 1;
    }
    const auto wanted = [&](int32_t bone) { return bone >= 0 && static_cast<size_t>(bone) < n && (mask.empty() || mask[static_cast<size_t>(bone)]); };

    const ClipRef* prim = slot_clip(r, kPrimary);
    const ClipRef* prev = fade_active_[r] ? slot_clip(r, kPrevious) : nullptr;
    if (prev && !prev->bound) prev = nullptr;  // [port] an unbound previous clip is ignored
    if (prim && prim->bound) {
        uint32_t a = 0, b = 0, pa = 0, pb = 0;
        float t = 0.0f, pt = 0.0f;
        sample_keys(frame_[kPrimary][r], prim->clip->frame_count, a, b, t);
        std::vector<int16_t> prev_track;  // bone -> previous clip track
        float w = 1.0f;
        if (prev) {
            sample_keys(frame_[kPrevious][r], prev->clip->frame_count, pa, pb, pt);
            prev_track.assign(n, -1);
            for (size_t i = 0; i < prev->track_bone.size(); ++i)
                if (prev->track_bone[i] >= 0) prev_track[static_cast<size_t>(prev->track_bone[i])] = static_cast<int16_t>(i);
            w = fade_weight_[r];
        }
        for (size_t i = 0; i < prim->track_bone.size(); ++i) {
            const int32_t bone = prim->track_bone[i];
            if (!wanted(bone)) continue;
            Quat q;
            Float3 tr;
            prim->clip->sample(static_cast<uint32_t>(i), a, b, t, q, tr);
            bool vis = prim->clip->visible(static_cast<uint32_t>(i), a);
            if (prev && prev_track[static_cast<size_t>(bone)] >= 0) {
                const uint32_t pi = static_cast<uint32_t>(prev_track[static_cast<size_t>(bone)]);
                Quat q0;
                Float3 t0;
                prev->clip->sample(pi, pa, pb, pt, q0, t0);
                tr = lerp(t0, tr, w);
                q = nlerp_fast(q0, q, w);
                vis = vis || prev->clip->visible(pi, pa);
            }
            local[static_cast<size_t>(bone)] = mat_from_rt(q, tr);
            if (want_vis) visible[static_cast<size_t>(bone)] = vis ? 1 : 0;
        }
    }
    const ClipRef* ov = slot_clip(r, kOverlay);
    if (ov && ov->bound) {
        uint32_t a = 0, b = 0;
        float t = 0.0f;
        sample_keys(frame_[kOverlay][r], ov->clip->frame_count, a, b, t);
        for (size_t i = 0; i < ov->track_bone.size(); ++i) {
            const int32_t bone = ov->track_bone[i];
            if (!wanted(bone) || ov->clip->tracks[i].overlay_excluded) continue;
            Quat q;
            Float3 tr;
            ov->clip->sample(static_cast<uint32_t>(i), a, b, t, q, tr);
            local[static_cast<size_t>(bone)] = mat_from_rt(q, tr);
            if (want_vis) visible[static_cast<size_t>(bone)] = ov->clip->visible(static_cast<uint32_t>(i), a) ? 1 : 0;
        }
    }
    return true;
}

bool AnimWorld::local_pose(ObjectId id, std::span<Mat34> local, std::span<uint8_t> visible) const {
    const uint32_t r = row(id);
    if (r == kNoRow || !model_[r]) return false;
    return sample_local(r, model_[r]->skeleton.size(), local, visible, -1);
}

namespace {
void compose(const Skeleton& sk, std::span<const Mat34> local, std::span<Mat34> world, size_t n) {
    for (size_t b = 0; b < n; ++b) {
        const int32_t p = sk.parents[b];
        if (p < 0 || static_cast<size_t>(p) >= b) world[b] = local[b];
        else if (sk.copies_parent(b)) world[b] = world[static_cast<size_t>(p)];
        else world[b] = mul(world[static_cast<size_t>(p)], local[b]);
    }
}
}  // namespace

bool AnimWorld::world_pose(ObjectId id, std::span<Mat34> world, std::span<uint8_t> visible) const {
    const uint32_t r = row(id);
    if (r == kNoRow || !model_[r]) return false;
    const size_t n = model_[r]->skeleton.size();
    if (world.size() != n) return false;
    std::vector<Mat34> local(n);
    if (!sample_local(r, n, local, visible, -1)) return false;
    compose(model_[r]->skeleton, local, world, n);
    return true;
}

bool AnimWorld::bone_transform(ObjectId id, int32_t bone, Mat34& out) const {
    const uint32_t r = row(id);
    if (r == kNoRow || !model_[r] || !model_[r]->has_skeleton) return false;
    const Skeleton& sk = model_[r]->skeleton;
    const size_t n = sk.size();
    if (bone < 0 || static_cast<size_t>(bone) >= n) return false;
    std::vector<Mat34> local(n), world(n);
    if (!sample_local(r, n, local, {}, bone)) return false;
    // Compose the chain only (root first).
    std::vector<int32_t> chain;
    for (int32_t b = bone; b >= 0; b = sk.parents[static_cast<size_t>(b)]) chain.push_back(b);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const size_t b = static_cast<size_t>(*it);
        const int32_t p = sk.parents[b];
        if (p < 0) world[b] = local[b];
        else if (sk.copies_parent(b)) world[b] = world[static_cast<size_t>(p)];
        else world[b] = mul(world[static_cast<size_t>(p)], local[b]);
    }
    out = world[static_cast<size_t>(bone)];
    return true;
}

bool AnimWorld::bone_transform(ObjectId id, std::string_view bone_name, Mat34& out) const {
    const AnimModel* m = model(id);
    if (!m) return false;
    return bone_transform(id, m->skeleton.find(bone_name), out);
}

bool AnimWorld::bone_world_position(ObjectId id, int32_t bone, const Mat34& object_to_world, Float3& out) const {
    Mat34 m;
    if (!bone_transform(id, bone, m)) return false;
    out = transform_point(object_to_world, m.translation());
    return true;
}

void clip_world_pose(const Skeleton& sk, const ClipRef& clip, float frame, std::span<Mat34> world) {
    const size_t n = sk.size();
    if (world.size() != n) return;
    std::vector<Mat34> local(sk.bind_local.begin(), sk.bind_local.end());
    if (clip.bound && clip.clip) {
        uint32_t a = 0, b = 0;
        float t = 0.0f;
        sample_keys(frame, clip.clip->frame_count, a, b, t);
        for (size_t i = 0; i < clip.track_bone.size(); ++i) {
            const int32_t bone = clip.track_bone[i];
            if (bone < 0 || static_cast<size_t>(bone) >= n) continue;
            Quat q;
            Float3 tr;
            clip.clip->sample(static_cast<uint32_t>(i), a, b, t, q, tr);
            local[static_cast<size_t>(bone)] = mat_from_rt(q, tr);
        }
    }
    compose(sk, local, world, n);
}

// ---- CRC ---------------------------------------------------------------------------------------------

void AnimWorld::hash_state(StateHasher& h) const {
    // The same little-endian byte stream as feeding every field through StateHasher (the hash is a pure byte
    // stream), packed per row and fed in one call: identical digest, far fewer calls (R3 profile: this was the
    // anim-on hot spot with the opt-in "anim" CRC captured every tick).
    static_assert(std::endian::native == std::endian::little, "hash_state packs native little-endian fields");
    h.u32(static_cast<uint32_t>(id_.size()));
    unsigned char buf[128];
    for (size_t r = 0; r < id_.size(); ++r) {
        size_t n = 0;
        const auto put = [&buf, &n](const auto& v) {
            std::memcpy(buf + n, &v, sizeof v);
            n += sizeof v;
        };
        put(id_[r]);
        put(model_crc_[r]);
        put(last_time_[r]);
        put(frozen_[r]);
        put(lock_[r]);
        put(near_end_[r]);
        put(idle_timer_[r]);
        for (int s = 0; s < kSlotCount; ++s) {
            put(type_[s][r]);
            put(variant_[s][r]);
            put(mode_[s][r]);
            put(frame_[s][r]);
            put(speed_[s][r]);
            put(timed_[s][r]);
            put(timed_left_[s][r]);
        }
        put(fade_active_[r]);
        put(fade_duration_[r]);
        put(fade_remaining_[r]);
        put(fade_weight_[r]);
        h.bytes(buf, n);
    }
}

void AnimWorld::register_crc(CrcRegistry& registry, const std::string& name) const {
    registry.add(name, [this](StateHasher& h) { hash_state(h); });
}

}  // namespace alamo::anim

// N1 save / load of the animation state (contract: alamo/core/save_file.hpp; layouts in anim_state.hpp and
// anim_behaviors.hpp). Every read stages into copies, validates, and commits only on success.
#include <functional>
#include <utility>
#include <vector>

#include "alamo/anim/anim_behaviors.hpp"
#include "alamo/anim/anim_state.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/hash.hpp"

namespace alamo::anim {

namespace {

constexpr uint32_t kAnimWorldVersion = 1;
constexpr uint32_t kAnimBehaviorsVersion = 1;

// Lower bounds of one saved record (corrupted-count guard: a count larger than the bytes left can hold fails before
// anything is allocated).
constexpr size_t kRowBytes = 4 + 1 + 4 + 8 + 5 + 8 + 3 * (4 + 4 + 1 + 4 + 4 + 1 + 4) + 1 + 4 + 4 + 4;
constexpr size_t kEntryBytes = 4 + 4 + 8 + 8 + 4 + 4 + 1 + 4 + 8 + 4 + 1 + 1;
constexpr size_t kCorpseBytes = 4 + 1 + 4 + 1 + 1 + 1 + 1 + 8 + 8 + 8 + 8 + 4 + 4 + 12 + 12;

bool read_count(ByteReader& r, size_t record_bytes, uint32_t& n) {
    n = r.u32();
    if (!r.ok() || static_cast<uint64_t>(n) * record_bytes > r.remaining()) {
        r.fail();
        return false;
    }
    return true;
}

bool valid_type(int32_t t) { return t == kNoAnimType || (t >= 0 && t < static_cast<int32_t>(kAnimTypeCount)); }

}  // namespace

// ---- AnimWorld -----------------------------------------------------------------------------------------

void AnimWorld::write(ByteWriter& w) const {
    w.u32(kAnimWorldVersion);
    w.u32(static_cast<uint32_t>(id_.size()));
    for (size_t r = 0; r < id_.size(); ++r) {
        w.i32(id_[r]);
        w.boolean(model_[r] != nullptr);
        w.u32(model_[r] ? model_crc_[r] : 0u);
        w.f64(last_time_[r]);
        w.boolean(frozen_[r] != 0);
        w.boolean(lock_[r] != 0);
        w.boolean(frame_changed_[r] != 0);
        w.boolean(near_end_[r] != 0);
        w.boolean(berserk_[r] != 0);
        w.u64(idle_timer_[r]);
        for (int s = 0; s < kSlotCount; ++s) {
            w.i32(type_[s][r]);
            w.u32(variant_[s][r]);
            w.u8(mode_[s][r]);
            w.f32(frame_[s][r]);
            w.f32(speed_[s][r]);
            w.boolean(timed_[s][r] != 0);
            w.f32(timed_left_[s][r]);
        }
        w.boolean(fade_active_[r] != 0);
        w.f32(fade_duration_[r]);
        w.f32(fade_remaining_[r]);
        w.f32(fade_weight_[r]);
    }
}

bool AnimWorld::read(ByteReader& r, const std::function<const AnimModel*(ObjectId id)>& model_of) {
    if (r.u32() != kAnimWorldVersion || !r.ok()) {
        r.fail();
        return false;
    }
    uint32_t n = 0;
    if (!read_count(r, kRowBytes, n)) return false;

    // Staged copy of every SoA column (tunables stay the reader's own).
    AnimWorld st(tunables_);
    const size_t rows = n;
    st.id_.resize(rows);
    st.model_.resize(rows);
    st.model_crc_.resize(rows);
    st.last_time_.resize(rows);
    st.frozen_.resize(rows);
    st.lock_.resize(rows);
    st.frame_changed_.resize(rows);
    st.near_end_.resize(rows);
    st.berserk_.resize(rows);
    st.idle_timer_.resize(rows);
    for (int s = 0; s < kSlotCount; ++s) {
        st.type_[s].resize(rows);
        st.variant_[s].resize(rows);
        st.mode_[s].resize(rows);
        st.frame_[s].resize(rows);
        st.speed_[s].resize(rows);
        st.timed_[s].resize(rows);
        st.timed_left_[s].resize(rows);
    }
    st.fade_active_.resize(rows);
    st.fade_duration_.resize(rows);
    st.fade_remaining_.resize(rows);
    st.fade_weight_.resize(rows);

    const auto flag = [&r]() -> uint8_t { return r.boolean() ? 1 : 0; };
    for (size_t i = 0; i < rows; ++i) {
        const ObjectId id = r.i32();
        if (i > 0 && id <= st.id_[i - 1]) r.fail();  // strictly ascending
        st.id_[i] = id;
        const bool has_model = r.boolean();
        const uint32_t crc = r.u32();
        if (!has_model && crc != 0) r.fail();
        st.model_crc_[i] = crc;
        st.last_time_[i] = r.f64();
        st.frozen_[i] = flag();
        st.lock_[i] = flag();
        st.frame_changed_[i] = flag();
        st.near_end_[i] = flag();
        st.berserk_[i] = flag();
        st.idle_timer_[i] = r.u64();
        for (int s = 0; s < kSlotCount; ++s) {
            st.type_[s][i] = r.i32();
            st.variant_[s][i] = r.u32();
            st.mode_[s][i] = r.u8();
            st.frame_[s][i] = r.f32();
            st.speed_[s][i] = r.f32();
            st.timed_[s][i] = flag();
            st.timed_left_[s][i] = r.f32();
            if (!valid_type(st.type_[s][i]) || st.mode_[s][i] > static_cast<uint8_t>(SlotMode::Finished)) r.fail();
        }
        st.fade_active_[i] = flag();
        st.fade_duration_[i] = r.f32();
        st.fade_remaining_[i] = r.f32();
        st.fade_weight_[i] = r.f32();
        if (!r.ok()) return false;
        if (has_model) {
            const AnimModel* m = model_of ? model_of(id) : nullptr;
            if (!m || name_crc(m->model) != crc) {
                r.fail();
                return false;
            }
            st.model_[i] = m;
        }
    }
    if (!r.ok()) return false;

    // Commit (tunables unchanged).
    id_ = std::move(st.id_);
    model_ = std::move(st.model_);
    model_crc_ = std::move(st.model_crc_);
    last_time_ = std::move(st.last_time_);
    frozen_ = std::move(st.frozen_);
    lock_ = std::move(st.lock_);
    frame_changed_ = std::move(st.frame_changed_);
    near_end_ = std::move(st.near_end_);
    berserk_ = std::move(st.berserk_);
    idle_timer_ = std::move(st.idle_timer_);
    for (int s = 0; s < kSlotCount; ++s) {
        type_[s] = std::move(st.type_[s]);
        variant_[s] = std::move(st.variant_[s]);
        mode_[s] = std::move(st.mode_[s]);
        frame_[s] = std::move(st.frame_[s]);
        speed_[s] = std::move(st.speed_[s]);
        timed_[s] = std::move(st.timed_[s]);
        timed_left_[s] = std::move(st.timed_left_[s]);
    }
    fade_active_ = std::move(st.fade_active_);
    fade_duration_ = std::move(st.fade_duration_);
    fade_remaining_ = std::move(st.fade_remaining_);
    fade_weight_ = std::move(st.fade_weight_);
    return true;
}

// ---- AnimBehaviors -------------------------------------------------------------------------------------

void AnimBehaviors::write(ByteWriter& w) const {
    w.u32(kAnimBehaviorsVersion);
    w.u32(static_cast<uint32_t>(entries_.size()));
    for (const Entry& e : entries_) {
        w.i32(e.id);
        w.i32(e.flinch);
        w.u64(e.flinch_end);
        w.u64(e.flinch_next);
        w.f32(e.flinch_blend);
        w.i32(e.ret_type);
        w.boolean(e.ret_loop);
        w.f32(e.ret_speed);
        w.u64(e.combat_timer);
        w.f32(e.last_yaw);
        w.boolean(e.have_yaw);
        w.boolean(e.last_crouch);
    }
    w.u32(static_cast<uint32_t>(corpses_.size()));
    for (const Corpse& c : corpses_) {
        w.i32(c.id);
        w.u8(static_cast<uint8_t>(c.phase));
        w.i32(c.death_type);
        w.boolean(c.forever);
        w.boolean(c.spin_away);
        w.u8(c.damage_kind);
        w.boolean(c.detached);
        w.u64(c.death_frame);
        w.u64(c.phase_end);
        w.u64(c.persist_frames);
        w.u64(c.fade_frames);
        w.u32(c.info.object_type);
        w.i32(c.info.owner);
        for (float v : c.info.position) w.f32(v);
        for (float v : c.info.facing) w.f32(v);
    }
}

bool AnimBehaviors::read(ByteReader& r) {
    if (r.u32() != kAnimBehaviorsVersion || !r.ok()) {
        r.fail();
        return false;
    }
    uint32_t n = 0;
    if (!read_count(r, kEntryBytes, n)) return false;
    std::vector<Entry> entries(n);
    for (size_t i = 0; i < entries.size(); ++i) {
        Entry& e = entries[i];
        e.id = r.i32();
        if (i > 0 && e.id <= entries[i - 1].id) r.fail();
        e.flinch = r.i32();
        e.flinch_end = r.u64();
        e.flinch_next = r.u64();
        e.flinch_blend = r.f32();
        e.ret_type = r.i32();
        e.ret_loop = r.boolean();
        e.ret_speed = r.f32();
        e.combat_timer = r.u64();
        e.last_yaw = r.f32();
        e.have_yaw = r.boolean();
        e.last_crouch = r.boolean();
        if (!valid_type(e.flinch) || !valid_type(e.ret_type)) r.fail();
        if (!r.ok()) return false;
    }
    if (!read_count(r, kCorpseBytes, n)) return false;
    std::vector<Corpse> corpses(n);
    for (size_t i = 0; i < corpses.size(); ++i) {
        Corpse& c = corpses[i];
        c.id = r.i32();
        if (i > 0 && c.id <= corpses[i - 1].id) r.fail();
        const uint8_t phase = r.u8();
        if (phase > static_cast<uint8_t>(CorpsePhase::SpinAway)) r.fail();
        c.phase = static_cast<CorpsePhase>(phase);
        c.death_type = r.i32();
        c.forever = r.boolean();
        c.spin_away = r.boolean();
        c.damage_kind = r.u8();
        c.detached = r.boolean();
        c.death_frame = r.u64();
        c.phase_end = r.u64();
        c.persist_frames = r.u64();
        c.fade_frames = r.u64();
        c.info.object_type = r.u32();
        c.info.owner = r.i32();
        for (float& v : c.info.position) v = r.f32();
        for (float& v : c.info.facing) v = r.f32();
        if (!valid_type(c.death_type)) r.fail();
        if (!r.ok()) return false;
    }
    if (!r.ok()) return false;
    entries_ = std::move(entries);
    corpses_ = std::move(corpses);
    return true;
}

}  // namespace alamo::anim

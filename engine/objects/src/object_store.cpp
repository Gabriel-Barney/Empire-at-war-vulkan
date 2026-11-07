#include "alamo/objects/object_store.hpp"

#include <algorithm>
#include <string>
#include <utility>

#include "alamo/objects/game_object_types.hpp"

namespace alamo::objects {

uint32_t BehaviorTable::row(uint32_t slot) const {
    const auto it = std::lower_bound(members.begin(), members.end(), slot);
    return (it != members.end() && *it == slot) ? static_cast<uint32_t>(it - members.begin()) : kNone;
}

ObjectStore::ObjectStore(ModeKind mode, StoreKind kind, const GameObjectTypes& types, IdCounter* ids)
    : mode_(mode), kind_(kind), types_(&types), ids_(ids ? ids : &own_ids_) {
    for (uint32_t i = 0; i < kBehaviorTypeCount; ++i) tables_[i].id = static_cast<BehaviorTypeId>(i);
}

uint32_t ObjectStore::alloc_slot() {
    if (!free_.empty()) {
        const uint32_t s = free_.back();
        free_.pop_back();
        return s;
    }
    const uint32_t s = static_cast<uint32_t>(gen_.size());
    gen_.push_back(1);
    used_.push_back(0);
    id_.push_back(kNoObjectId);
    parent_mode_id_.push_back(kNoObjectId);
    type_.push_back(kNone);
    owner_.push_back(-1);
    position_.push_back({});
    facing_.push_back({});
    health_.push_back(0.0f);
    flags_.push_back(0);
    registered_serviced_.push_back(0);
    container_.push_back({});
    attach_parent_.push_back({});
    carried_type_.push_back(kNone);
    landing_persist_.push_back(0);
    behaviors_.emplace_back();
    beh_state_.emplace_back();
    std::array<uint8_t, kBehaviorTypeCount> none;
    none.fill(0xFF);
    behavior_index_.push_back(none);
    return s;
}

ObjectHandle ObjectStore::handle_of_slot(uint32_t slot) const {
    if (slot >= gen_.size() || !used_[slot]) return {};
    return ObjectHandle::make(slot, gen_[slot]);
}

bool ObjectStore::exists(ObjectHandle h) const {
    const uint32_t s = h.slot();
    return h && s < gen_.size() && used_[s] && gen_[s] == h.generation();
}

bool ObjectStore::alive(ObjectHandle h) const { return exists(h) && !(flags_[h.slot()] & kFlagDead); }

void ObjectStore::attach(uint32_t slot, BehaviorTypeId id, const BehaviorService& svc) {
    if (id == kNoBehavior || id >= kBehaviorTypeCount || behaviors_[slot].size() >= 0xFF) return;  // unknown
    const bool first = behavior_index_[slot][id] == 0xFF;
    behavior_index_[slot][id] = static_cast<uint8_t>(behaviors_[slot].size());  // later instance wins
    behaviors_[slot].push_back(id);
    // Interval cached at attach (0x4c3710); computed classes from the config (TACTICAL_GAPS §6.4).
    BehaviorState st;
    st.serviced = svc.serviced;
    st.interval = svc.computed ? computed_behavior_interval(id, interval_cfg_) : svc.interval;
    // Initialise (base vf9 0x4c2f90): first due frame, one synced draw for interval >= 2.
    if (st.interval < 2) {
        st.next = 0;
    } else {
        uint32_t offset = 0;
        if (rng_) {
            offset = static_cast<uint32_t>(rng_->range_int(0, static_cast<int32_t>(std::min<uint32_t>(st.interval, 0x7FFFFFFFu) - 1u)));
            ++attach_draws_;
        }
        st.next = mode_frame() + offset;
    }
    st.disabled = behavior_starts_disabled(id) ? 1u : 0u;
    beh_state_[slot].push_back(st);
    const ObjectHandle h = ObjectHandle::make(slot, gen_[slot]);
    if (first) {  // per-type tables hold the object once
        BehaviorTable& t = tables_[id];
        t.members.insert(std::lower_bound(t.members.begin(), t.members.end(), slot), slot);
        t.insertion.push_back(h);
    }
    if (svc.serviced && !registered_serviced_[slot]) {
        registered_serviced_[slot] = 1;
        serviced_.push_back(h);
    }
}

ObjectHandle ObjectStore::create(const SpawnParams& p) {
    const uint32_t s = alloc_slot();
    used_[s] = 1;
    const ObjectHandle h = ObjectHandle::make(s, gen_[s]);
    id_[s] = ids_->allocate();
    parent_mode_id_[s] = p.parent_mode_id;
    type_[s] = p.type;
    owner_[s] = p.owner;
    position_[s] = p.position;
    facing_[s] = p.facing;
    flags_[s] = 0;
    registered_serviced_[s] = 0;
    container_[s] = p.container;
    attach_parent_[s] = p.attach_parent;
    carried_type_[s] = kNone;
    landing_persist_[s] = 0;
    behaviors_[s].clear();
    beh_state_[s].clear();
    behavior_index_[s].fill(0xFF);
    const bool typed = p.type != kNone && p.type < types_->size();
    health_[s] = typed ? types_->columns().tactical_health[p.type] : 0.0f;
    if (typed) {
        for (std::string_view n : types_->get_list(p.type, "Behavior")) attach(s, behavior_type_id(n), behavior_service(n));
        const char* mode_list = mode_ == ModeKind::Galactic ? "GalacticBehavior"
                                : mode_ == ModeKind::Land   ? "LandBehavior"
                                                            : "SpaceBehavior";
        for (std::string_view n : types_->get_list(p.type, mode_list)) attach(s, behavior_type_id(n), behavior_service(n));
    }
    all_.push_back(h);
    by_id_[id_[s]] = s;
    const size_t o = static_cast<size_t>(static_cast<int64_t>(p.owner) + 1);
    if (p.owner >= -1) {
        if (by_owner_.size() <= o) by_owner_.resize(o + 1);
        by_owner_[o].push_back(h);
    }
    if (on_created) on_created(*this, h);  // q6 object scripts (host attach hook)
    return h;
}

void ObjectStore::queue_create(uint32_t due_frame, const SpawnParams& p) { delayed_.push_back({due_frame, p}); }

bool ObjectStore::add_behavior(ObjectHandle h, BehaviorTypeId id) {
    if (!alive(h) || id == kNoBehavior || id >= kBehaviorTypeCount || behavior_index_[h.slot()][id] != 0xFF) return false;
    attach(h.slot(), id, behavior_service(id));
    return true;
}

bool ObjectStore::remove_behavior(ObjectHandle h, uint32_t index) {
    if (!exists(h)) return false;
    const uint32_t s = h.slot();
    auto& list = behaviors_[s];
    if (index >= list.size()) return false;
    const BehaviorTypeId id = list[index];
    list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
    beh_state_[s].erase(beh_state_[s].begin() + static_cast<std::ptrdiff_t>(index));
    behavior_index_[s].fill(0xFF);
    for (size_t k = 0; k < list.size(); ++k) behavior_index_[s][list[k]] = static_cast<uint8_t>(k);  // later instance wins
    if (behavior_index_[s][id] == 0xFF) {  // last instance gone: out of the per-type table and category list
        BehaviorTable& t = tables_[id];
        const auto it = std::lower_bound(t.members.begin(), t.members.end(), s);
        if (it != t.members.end() && *it == s) t.members.erase(it);
        t.insertion.erase(std::remove(t.insertion.begin(), t.insertion.end(), h), t.insertion.end());
    }
    return true;
}

uint32_t ObjectStore::serviced_behavior_count(ObjectHandle h) const {
    if (!exists(h)) return 0;
    uint32_t n = 0;
    for (const BehaviorState& b : beh_state_[h.slot()]) n += b.serviced ? 1u : 0u;
    return n;
}

uint32_t ObjectStore::behavior_index(ObjectHandle h, BehaviorTypeId id) const {
    if (!exists(h) || id >= kBehaviorTypeCount) return kNone;
    const uint8_t k = behavior_index_[h.slot()][id];
    return k == 0xFF ? kNone : k;
}

uint32_t ObjectStore::service_behaviors(ObjectHandle h, uint32_t frame, const BehaviorServiceFn& fn) {
    uint32_t n = 0;
    const uint32_t s = h.slot();
    // Attach order (the original walks its array from NumServicedBehaviors-1 down to 0; serviced
    // instances are inserted at the front, so that is attach order). Behaviours attached during
    // the walk are not visited this frame.
    const size_t count = alive(h) ? behaviors_[s].size() : 0;
    for (size_t k = 0; k < count; ++k) {
        if (!alive(h)) break;  // killed: its serviced behaviours are detached
        BehaviorState& b = beh_state_[s][k];
        if (!b.serviced || b.next > frame || b.disabled != 0) continue;
        b.next = frame + b.interval;  // base Service 0x4c2f70: +0x30 = frame + +0x34
        ++n;
        if (fn) fn(*this, h, behaviors_[s][k], static_cast<uint32_t>(k));
    }
    return n;
}

uint32_t ObjectStore::mode_frame() const {
    return mode_frame_src_ ? static_cast<uint32_t>(*mode_frame_src_) : last_frame_;
}

ObjectStore::BehaviorState* ObjectStore::beh_state(ObjectHandle h, uint32_t index) {
    if (!exists(h) || index >= beh_state_[h.slot()].size()) return nullptr;
    return &beh_state_[h.slot()][index];
}

const ObjectStore::BehaviorState* ObjectStore::beh_state(ObjectHandle h, uint32_t index) const {
    if (!exists(h) || index >= beh_state_[h.slot()].size()) return nullptr;
    return &beh_state_[h.slot()][index];
}

void ObjectStore::set_behavior_next_service(ObjectHandle h, uint32_t index, uint32_t frame) {
    if (BehaviorState* b = beh_state(h, index)) b->next = frame;
}

void ObjectStore::set_behavior_interval(ObjectHandle h, uint32_t index, uint32_t interval) {
    if (BehaviorState* b = beh_state(h, index)) b->interval = interval;
}

uint32_t ObjectStore::enable_behavior(ObjectHandle h, uint32_t index) {
    BehaviorState* b = beh_state(h, index);
    if (!b) return 0;
    if (b->disabled > 0) --b->disabled;
    return b->disabled;
}

uint32_t ObjectStore::disable_behavior(ObjectHandle h, uint32_t index) {
    BehaviorState* b = beh_state(h, index);
    if (!b) return 0;
    if (b->disabled != 0xFFFFFFFFu) ++b->disabled;
    return b->disabled;
}

void ObjectStore::disable_all_behaviors(ObjectHandle h) {
    if (!exists(h)) return;
    for (BehaviorState& b : beh_state_[h.slot()])
        if (b.disabled != 0xFFFFFFFFu) ++b.disabled;
}

uint32_t ObjectStore::behavior_disable_count(ObjectHandle h, uint32_t index) const {
    const BehaviorState* b = beh_state(h, index);
    return b ? b->disabled : 0u;
}

bool ObjectStore::behavior_enabled_by_type(ObjectHandle h, BehaviorTypeId id) const {
    const uint32_t k = behavior_index(h, id);
    return k != kNone && behavior_enabled(h, k);
}

bool ObjectStore::has_behavior(ObjectHandle h, BehaviorTypeId id) const {
    return exists(h) && id < kBehaviorTypeCount && behavior_index_[h.slot()][id] != 0xFF;
}

bool ObjectStore::set_carried_type(ObjectHandle h, TypeIndex carried) {
    if (!alive(h) || (carried != kNone && carried >= types_->size())) return false;
    if (!has_behavior(h, beh::Transport) && !has_behavior(h, beh::TransportLanding)) return false;
    carried_type_[h.slot()] = carried;
    return true;
}

bool ObjectStore::set_landing_persist(ObjectHandle h, bool persist) {
    if (!alive(h) || (!has_behavior(h, beh::Transport) && !has_behavior(h, beh::TransportLanding))) return false;
    landing_persist_[h.slot()] = persist ? 1 : 0;
    return true;
}

void ObjectStore::set_position(ObjectHandle h, Vec3 p) {
    if (exists(h)) position_[h.slot()] = p;
}
void ObjectStore::set_facing(ObjectHandle h, Vec3 f) {
    if (exists(h)) facing_[h.slot()] = f;
}
void ObjectStore::set_health(ObjectHandle h, float health) {
    if (exists(h)) health_[h.slot()] = health;
}
void ObjectStore::set_flag(ObjectHandle h, uint8_t flag) {
    if (exists(h)) flags_[h.slot()] = static_cast<uint8_t>(flags_[h.slot()] | (flag & ~kFlagDead));
}

bool ObjectStore::set_owner(ObjectHandle h, int32_t owner) {
    if (!alive(h) || owner < -1) return false;
    const uint32_t s = h.slot();
    const int32_t old = owner_[s];
    if (old == owner) return false;
    if (old >= -1) {
        const size_t o = static_cast<size_t>(static_cast<int64_t>(old) + 1);
        if (o < by_owner_.size()) {
            auto& l = by_owner_[o];
            const auto it = std::find(l.begin(), l.end(), h);
            if (it != l.end()) l.erase(it);
        }
    }
    owner_[s] = owner;
    const size_t n = static_cast<size_t>(static_cast<int64_t>(owner) + 1);
    if (by_owner_.size() <= n) by_owner_.resize(n + 1);
    by_owner_[n].push_back(h);
    return true;
}

ObjectHandle ObjectStore::find(ObjectId id) const {
    const auto it = by_id_.find(id);
    return it == by_id_.end() ? ObjectHandle{} : ObjectHandle::make(it->second, gen_[it->second]);
}

bool ObjectStore::destroy(ObjectHandle h) {
    if (!alive(h)) return false;
    const uint32_t s = h.slot();
    if (parent_mode_id_[s] != kNoObjectId && on_parent_mode_death) on_parent_mode_death(*this, h);
    pending_.push_back(h);
    flags_[s] = static_cast<uint8_t>(flags_[s] | kFlagDead);  // Detach: serviced behaviours stop
    return true;
}

void ObjectStore::remove_object(uint32_t slot) {
    const ObjectHandle h = ObjectHandle::make(slot, gen_[slot]);
    // Scrub references in every other object, newest first; attached children die (cascade,
    // appended to the pending list and flushed in this same flush).
    if (!has_behavior(h, beh::Projectile)) {
        for (size_t i = all_.size(); i-- > 0;) {
            const ObjectHandle o = all_[i];
            if (o == h || !exists(o)) continue;
            const uint32_t os = o.slot();
            if (container_[os] == h) container_[os] = {};
            if (attach_parent_[os] == h) {
                attach_parent_[os] = {};
                destroy(o);
            }
        }
    }
    by_id_.erase(id_[slot]);
    for (BehaviorTypeId b : behaviors_[slot]) {
        auto& m = tables_[b].members;
        const auto it = std::lower_bound(m.begin(), m.end(), slot);
        if (it != m.end() && *it == slot) m.erase(it);  // duplicates: member once
    }
    used_[slot] = 0;
    carried_type_[slot] = kNone;
    landing_persist_[slot] = 0;
    gen_[slot] = gen_[slot] + 1 == 0 ? 1 : gen_[slot] + 1;  // stale handles fail from now on
    free_.push_back(slot);
}


void ObjectStore::flush() {
    if (pending_.empty()) return;
    // Index loop: cascaded kills append to pending_ and are flushed in the same pass (in order).
    for (size_t i = 0; i < pending_.size(); ++i) {
        const ObjectHandle h = pending_[i];
        if (exists(h)) remove_object(h.slot());
    }
    pending_.clear();
    // Unlink from every list in one stable pass (order of the survivors unchanged).
    auto keep = [this](ObjectHandle h) { return !exists(h); };
    all_.erase(std::remove_if(all_.begin(), all_.end(), keep), all_.end());
    serviced_.erase(std::remove_if(serviced_.begin(), serviced_.end(), keep), serviced_.end());
    for (auto& l : by_owner_) l.erase(std::remove_if(l.begin(), l.end(), keep), l.end());
    for (auto& t : tables_) t.insertion.erase(std::remove_if(t.insertion.begin(), t.insertion.end(), keep), t.insertion.end());
}

void ObjectStore::update(uint32_t frame, const ServiceFn& service, const std::function<void(ObjectStore&)>& pre_service) {
    last_frame_ = frame;
    if (pre_service) pre_service(*this);
    // Serviced objects newest-registered first; only those registered before the loop started.
    for (size_t i = serviced_.size(); i-- > 0;) {
        const ObjectHandle h = serviced_[i];
        if (alive(h) && service) service(*this, h);
    }
    // Delayed creations, last to first.
    for (size_t k = delayed_.size(); k-- > 0;) {
        if (delayed_[k].due > frame) continue;
        const SpawnParams p = delayed_[k].p;
        delayed_.erase(delayed_.begin() + static_cast<std::ptrdiff_t>(k));
        create(p);
    }
    flush();
}

std::vector<ObjectHandle> ObjectStore::objects_newest_first() const {
    return std::vector<ObjectHandle>(all_.rbegin(), all_.rend());
}

std::vector<ObjectHandle> ObjectStore::find_all_of_type(TypeIndex type) const {
    std::vector<ObjectHandle> out;
    for (size_t i = all_.size(); i-- > 0;)
        if (active(all_[i]) && type_[all_[i].slot()] == type) out.push_back(all_[i]);
    return out;
}

std::span<const ObjectHandle> ObjectStore::owned_by(int32_t owner) const {
    const int64_t o = static_cast<int64_t>(owner) + 1;
    if (o < 0 || static_cast<size_t>(o) >= by_owner_.size()) return {};
    return by_owner_[static_cast<size_t>(o)];
}

uint32_t ObjectStore::parity_crc() const {
    CrcSum sum;
    for (ObjectHandle h : all_) {
        if (!alive(h)) continue;
        const uint32_t s = h.slot();
        const float pos[3] = {position_[s].x, position_[s].y, position_[s].z};
        const float face[3] = {facing_[s].x, facing_[s].y, facing_[s].z};
        sum.add(parity_object_crc(pos, face, health_[s]));
    }
    return sum.value();
}

void ObjectStore::hash_state(StateHasher& hs) const {
    std::vector<uint32_t> slots;
    slots.reserve(all_.size());
    for (ObjectHandle h : all_) slots.push_back(h.slot());
    std::sort(slots.begin(), slots.end(), [this](uint32_t a, uint32_t b) { return id_[a] < id_[b]; });
    auto id_of = [this](ObjectHandle r) { return exists(r) ? id_[r.slot()] : kNoObjectId; };
    hs.i32(ids_->next());
    hs.u32(static_cast<uint32_t>(slots.size()));
    for (uint32_t s : slots) {
        hs.i32(id_[s]);
        hs.i32(parent_mode_id_[s]);
        hs.u32(type_[s]);
        hs.i32(owner_[s]);
        hs.f32(position_[s].x);
        hs.f32(position_[s].y);
        hs.f32(position_[s].z);
        hs.f32(facing_[s].x);
        hs.f32(facing_[s].y);
        hs.f32(facing_[s].z);
        hs.f32(health_[s]);
        hs.u8(flags_[s]);
        hs.i32(id_of(container_[s]));
        hs.i32(id_of(attach_parent_[s]));
        hs.u32(static_cast<uint32_t>(behaviors_[s].size()));
        for (BehaviorTypeId b : behaviors_[s]) hs.u8(b);
    }
    hs.u32(static_cast<uint32_t>(delayed_.size()));
    for (const Delayed& d : delayed_) {
        hs.u32(d.due);
        hs.u32(d.p.type);
        hs.i32(d.p.owner);
    }
    // a1h t08: the carried types (id order), only when an object has one.
    uint32_t carried = 0;
    for (uint32_t s : slots) carried += carried_type_[s] != kNone ? 1u : 0u;
    if (carried != 0) {
        hs.u32(carried);
        for (uint32_t s : slots)
            if (carried_type_[s] != kNone) {
                hs.i32(id_[s]);
                hs.u32(carried_type_[s]);
            }
    }
    // a1i t05: the persist flags (id order), only when an object has one.
    uint32_t persist = 0;
    for (uint32_t s : slots) persist += landing_persist_[s] != 0 ? 1u : 0u;
    if (persist != 0) {
        hs.u32(persist);
        for (uint32_t s : slots)
            if (landing_persist_[s] != 0) hs.i32(id_[s]);
    }
}

// ---- N1 save / load ----------------------------------------------------------------------------------
//
// Layout (v2, little-endian, ByteWriter; v1 = the same without the last line):
//   u32 version | u8 mode | u8 kind | u64 type count | u64 types digest | i32 id counter next()
//   u32 last update frame | u64 attach draws
//   u32 slot count, per slot (all slots, free ones too):
//     u32 generation | u8 used | i32 id | i32 parent-mode id | u32 type | i32 owner | f32 x3 position
//     | f32 x3 facing | f32 health | u8 flags | u8 registered-serviced | u64 container | u64 attach parent
//     | u32 behaviour count, per instance: u8 id | u8 serviced | u32 interval | u32 next | u32 disable count
//   u32 n + u32 slots          free list (LIFO order)
//   u32 n + u64 handles        all objects (insertion order)
//   u32 n + u64 handles        serviced objects (registration order)
//   u32 lists, per list: u32 n + u64 handles      per owner (index owner + 1)
//   kBehaviorTypeCount x (u32 n + u64 handles)    per-behaviour category lists (attach order)
//   u32 n, per entry: i32 id | u32 slot           id map, id ascending
//   u32 n, per entry: u32 due | u32 type | i32 owner | f32 x3 position | f32 x3 facing | i32 parent-mode id
//                     | u64 container | u64 attach parent                     delayed creations (queue order)
//   u32 n + u64 handles        dead objects pending the flush (destroy order)
//   u32 n, per entry: u32 slot | u32 type       carried types (a1h t08), slot ascending, objects that have one
//   u32 n + u32 slots          landing persist flags (a1i t05, version 3), slot ascending, objects that have it
namespace {

constexpr size_t kSlotMinBytes = 4 + 1 + 4 + 4 + 4 + 4 + 7 * 4 + 1 + 1 + 8 + 8 + 4;
constexpr size_t kBehaviorBytes = 1 + 1 + 4 + 4 + 4;
constexpr size_t kDelayedBytes = 4 + 4 + 4 + 6 * 4 + 4 + 8 + 8;

void put_vec3(ByteWriter& w, const Vec3& v) { w.f32(v.x).f32(v.y).f32(v.z); }
Vec3 get_vec3(ByteReader& r) {
    Vec3 v;
    v.x = r.f32();
    v.y = r.f32();
    v.z = r.f32();
    return v;
}

void put_handles(ByteWriter& w, std::span<const ObjectHandle> v) {
    w.u32(static_cast<uint32_t>(v.size()));
    for (ObjectHandle h : v) w.u64(h.value);
}

// Count prefix whose elements need at least `elem_bytes` each: guards allocations on corrupt data.
bool get_count(ByteReader& r, size_t elem_bytes, uint32_t& n) {
    n = r.u32();
    if (!r.ok() || n > r.remaining() / elem_bytes) {
        r.fail();
        return false;
    }
    return true;
}

bool get_handles(ByteReader& r, std::vector<ObjectHandle>& v) {
    uint32_t n = 0;
    if (!get_count(r, 8, n)) return false;
    v.resize(n);
    for (ObjectHandle& h : v) h.value = r.u64();
    return r.ok();
}

}  // namespace

uint64_t ObjectStore::types_digest() const {
    StateHasher h;
    const size_t n = types_->size();
    h.u64(n);
    if (n != 0) {
        const auto& crcs = types_->raw().types().name_crc;
        for (size_t i = 0; i < n && i < crcs.size(); ++i) h.u32(crcs[i]);
    }
    return h.digest();
}

void ObjectStore::write(ByteWriter& w) const {
    w.u32(kSaveVersion).u8(static_cast<uint8_t>(mode_)).u8(static_cast<uint8_t>(kind_));
    w.u64(types_->size()).u64(types_digest());
    w.i32(ids_->next()).u32(last_frame_).u64(attach_draws_);
    const uint32_t cap = capacity();
    w.u32(cap);
    for (uint32_t s = 0; s < cap; ++s) {
        w.u32(gen_[s]).boolean(used_[s] != 0).i32(id_[s]).i32(parent_mode_id_[s]).u32(type_[s]).i32(owner_[s]);
        put_vec3(w, position_[s]);
        put_vec3(w, facing_[s]);
        w.f32(health_[s]).u8(flags_[s]).boolean(registered_serviced_[s] != 0);
        w.u64(container_[s].value).u64(attach_parent_[s].value);
        w.u32(static_cast<uint32_t>(behaviors_[s].size()));
        for (size_t k = 0; k < behaviors_[s].size(); ++k) {
            const BehaviorState& b = beh_state_[s][k];
            w.u8(behaviors_[s][k]).boolean(b.serviced).u32(b.interval).u32(b.next).u32(b.disabled);
        }
    }
    w.u32(static_cast<uint32_t>(free_.size()));
    for (uint32_t s : free_) w.u32(s);
    put_handles(w, all_);
    put_handles(w, serviced_);
    w.u32(static_cast<uint32_t>(by_owner_.size()));
    for (const auto& l : by_owner_) put_handles(w, l);
    for (const BehaviorTable& t : tables_) put_handles(w, t.insertion);
    // The id map is lookup-only (never iterated by the sim); saved id-ascending.
    std::vector<std::pair<ObjectId, uint32_t>> ids(by_id_.begin(), by_id_.end());
    std::sort(ids.begin(), ids.end());
    w.u32(static_cast<uint32_t>(ids.size()));
    for (const auto& [id, slot] : ids) w.i32(id).u32(slot);
    w.u32(static_cast<uint32_t>(delayed_.size()));
    for (const Delayed& d : delayed_) {
        w.u32(d.due).u32(d.p.type).i32(d.p.owner);
        put_vec3(w, d.p.position);
        put_vec3(w, d.p.facing);
        w.i32(d.p.parent_mode_id).u64(d.p.container.value).u64(d.p.attach_parent.value);
    }
    put_handles(w, pending_);
    uint32_t carried = 0;
    for (uint32_t s = 0; s < cap; ++s) carried += carried_type_[s] != kNone ? 1u : 0u;
    w.u32(carried);
    for (uint32_t s = 0; s < cap; ++s)
        if (carried_type_[s] != kNone) w.u32(s).u32(carried_type_[s]);
    uint32_t persist = 0;
    for (uint32_t s = 0; s < cap; ++s) persist += landing_persist_[s] != 0 ? 1u : 0u;
    w.u32(persist);
    for (uint32_t s = 0; s < cap; ++s)
        if (landing_persist_[s] != 0) w.u32(s);
}

bool ObjectStore::read(ByteReader& r) {
    const uint32_t version = r.u32();
    if (version < 1 || version > kSaveVersion || !r.ok()) return false;
    const uint8_t saved_mode = r.u8(), saved_kind = r.u8();
    const uint64_t type_count = r.u64(), digest = r.u64();
    if (!r.ok() || saved_mode != static_cast<uint8_t>(mode_) || saved_kind != static_cast<uint8_t>(kind_) ||
        type_count != types_->size() || digest != types_digest())
        return false;
    const ObjectId next_id = r.i32();
    // Parse into a scratch store; *this changes only once everything parsed and checked out.
    ObjectStore t(mode_, kind_, *types_);
    t.last_frame_ = r.u32();
    t.attach_draws_ = r.u64();
    uint32_t cap = 0;
    if (!get_count(r, kSlotMinBytes, cap)) return false;
    t.gen_.resize(cap);
    t.used_.resize(cap);
    t.id_.resize(cap);
    t.parent_mode_id_.resize(cap);
    t.type_.resize(cap);
    t.owner_.resize(cap);
    t.position_.resize(cap);
    t.facing_.resize(cap);
    t.health_.resize(cap);
    t.flags_.resize(cap);
    t.registered_serviced_.resize(cap);
    t.container_.resize(cap);
    t.attach_parent_.resize(cap);
    t.carried_type_.assign(cap, kNone);
    t.landing_persist_.assign(cap, 0);
    t.behaviors_.resize(cap);
    t.beh_state_.resize(cap);
    std::array<uint8_t, kBehaviorTypeCount> none;
    none.fill(0xFF);
    t.behavior_index_.assign(cap, none);
    const size_t ntypes = types_->size();
    uint32_t used_count = 0;
    for (uint32_t s = 0; s < cap; ++s) {
        t.gen_[s] = r.u32();
        t.used_[s] = static_cast<uint8_t>(r.boolean());
        t.id_[s] = r.i32();
        t.parent_mode_id_[s] = r.i32();
        t.type_[s] = r.u32();
        t.owner_[s] = r.i32();
        t.position_[s] = get_vec3(r);
        t.facing_[s] = get_vec3(r);
        t.health_[s] = r.f32();
        t.flags_[s] = r.u8();
        t.registered_serviced_[s] = static_cast<uint8_t>(r.boolean());
        t.container_[s].value = r.u64();
        t.attach_parent_[s].value = r.u64();
        uint32_t nb = 0;
        if (!get_count(r, kBehaviorBytes, nb) || nb > 0xFF) return false;
        if (t.gen_[s] == 0 || (t.type_[s] != kNone && t.type_[s] >= ntypes)) return false;
        auto& bl = t.behaviors_[s];
        auto& st = t.beh_state_[s];
        bl.resize(nb);
        st.resize(nb);
        for (uint32_t k = 0; k < nb; ++k) {
            bl[k] = r.u8();
            st[k].serviced = r.boolean();
            st[k].interval = r.u32();
            st[k].next = r.u32();
            st[k].disabled = r.u32();
            if (bl[k] == kNoBehavior || bl[k] >= kBehaviorTypeCount) return false;
            t.behavior_index_[s][bl[k]] = static_cast<uint8_t>(k);  // later instance wins (as attach)
        }
        if (!r.ok()) return false;
        used_count += t.used_[s] ? 1u : 0u;
    }
    // Free list: exactly the unused slots, each once.
    uint32_t nfree = 0;
    if (!get_count(r, 4, nfree) || nfree != cap - used_count) return false;
    std::vector<uint32_t> seen(cap, 0);  // stamp per checked list
    uint32_t stamp = 1;
    t.free_.resize(nfree);
    for (uint32_t& s : t.free_) {
        s = r.u32();
        if (!r.ok() || s >= cap || t.used_[s] || seen[s] == stamp) return false;
        seen[s] = stamp;
    }
    // Handle lists: every handle names a registered object that `member` selects, at most once, and the
    // list has `want` entries (= every selected object is in it).
    auto check_list = [&](const std::vector<ObjectHandle>& l, uint32_t want, auto&& member) {
        ++stamp;
        if (l.size() != want) return false;
        for (ObjectHandle h : l) {
            if (!t.exists(h) || seen[h.slot()] == stamp || !member(h.slot())) return false;
            seen[h.slot()] = stamp;
        }
        return true;
    };
    uint32_t want_serviced = 0, want_dead = 0;
    std::vector<uint32_t> want_owner, want_behavior(kBehaviorTypeCount, 0);
    for (uint32_t s = 0; s < cap; ++s) {
        if (!t.used_[s]) continue;
        want_serviced += t.registered_serviced_[s] ? 1u : 0u;
        want_dead += (t.flags_[s] & kFlagDead) ? 1u : 0u;
        for (uint32_t b = 0; b < kBehaviorTypeCount; ++b) want_behavior[b] += t.behavior_index_[s][b] != 0xFF ? 1u : 0u;
    }
    if (!get_handles(r, t.all_) || !check_list(t.all_, used_count, [](uint32_t) { return true; })) return false;
    if (!get_handles(r, t.serviced_) ||
        !check_list(t.serviced_, want_serviced, [&t](uint32_t s) { return t.registered_serviced_[s] != 0; }))
        return false;
    uint32_t nowners = 0;
    if (!get_count(r, 4, nowners)) return false;
    want_owner.assign(nowners, 0);
    for (uint32_t s = 0; s < cap; ++s) {
        if (!t.used_[s] || t.owner_[s] < -1) continue;  // owner < -1: in no list
        const uint64_t o = static_cast<uint64_t>(static_cast<int64_t>(t.owner_[s]) + 1);
        if (o >= nowners) return false;  // create / set_owner grow the table to every owner they see
        ++want_owner[o];
    }
    t.by_owner_.resize(nowners);
    for (uint32_t o = 0; o < nowners; ++o) {
        const int64_t owner = static_cast<int64_t>(o) - 1;
        if (!get_handles(r, t.by_owner_[o]) ||
            !check_list(t.by_owner_[o], want_owner[o], [&t, owner](uint32_t s) { return t.owner_[s] == owner; }))
            return false;
    }
    for (uint32_t b = 0; b < kBehaviorTypeCount; ++b) {
        BehaviorTable& tb = t.tables_[b];
        if (!get_handles(r, tb.insertion) ||
            !check_list(tb.insertion, want_behavior[b], [&t, b](uint32_t s) { return t.behavior_index_[s][b] != 0xFF; }))
            return false;
        for (uint32_t s = 0; s < cap; ++s)  // slot order
            if (t.used_[s] && t.behavior_index_[s][b] != 0xFF) tb.members.push_back(s);
    }
    uint32_t nids = 0;
    if (!get_count(r, 8, nids) || nids > used_count) return false;
    ObjectId prev = 0;
    for (uint32_t i = 0; i < nids; ++i) {
        const ObjectId id = r.i32();
        const uint32_t s = r.u32();
        if (!r.ok() || (i > 0 && id <= prev) || s >= cap || !t.used_[s] || t.id_[s] != id) return false;
        t.by_id_.emplace(id, s);
        prev = id;
    }
    uint32_t ndelayed = 0;
    if (!get_count(r, kDelayedBytes, ndelayed)) return false;
    t.delayed_.resize(ndelayed);
    for (Delayed& d : t.delayed_) {
        d.due = r.u32();
        d.p.type = r.u32();
        d.p.owner = r.i32();
        d.p.position = get_vec3(r);
        d.p.facing = get_vec3(r);
        d.p.parent_mode_id = r.i32();
        d.p.container.value = r.u64();
        d.p.attach_parent.value = r.u64();
        if (!r.ok() || (d.p.type != kNone && d.p.type >= ntypes)) return false;
    }
    if (!get_handles(r, t.pending_) ||
        !check_list(t.pending_, want_dead, [&t](uint32_t s) { return (t.flags_[s] & kFlagDead) != 0; }))
        return false;
    if (version >= 2) {  // carried types: registered transports only, slot ascending, each a valid type
        uint32_t ncarried = 0;
        if (!get_count(r, 8, ncarried)) return false;
        for (uint32_t i = 0, prev_slot = 0; i < ncarried; ++i) {
            const uint32_t s = r.u32();
            const TypeIndex carried = r.u32();
            if (!r.ok() || (i > 0 && s <= prev_slot) || s >= cap || !t.used_[s] || carried >= ntypes) return false;
            if (t.behavior_index_[s][beh::Transport] == 0xFF && t.behavior_index_[s][beh::TransportLanding] == 0xFF) return false;
            t.carried_type_[s] = carried;
            prev_slot = s;
        }
    }
    if (version >= 3) {  // landing persist flags: registered transports only, slot ascending
        uint32_t npersist = 0;
        if (!get_count(r, 4, npersist)) return false;
        for (uint32_t i = 0, prev_slot = 0; i < npersist; ++i) {
            const uint32_t s = r.u32();
            if (!r.ok() || (i > 0 && s <= prev_slot) || s >= cap || !t.used_[s]) return false;
            if (t.behavior_index_[s][beh::Transport] == 0xFF && t.behavior_index_[s][beh::TransportLanding] == 0xFF) return false;
            t.landing_persist_[s] = 1;
            prev_slot = s;
        }
    }
    if (!r.ok()) return false;

    gen_ = std::move(t.gen_);
    used_ = std::move(t.used_);
    id_ = std::move(t.id_);
    parent_mode_id_ = std::move(t.parent_mode_id_);
    type_ = std::move(t.type_);
    owner_ = std::move(t.owner_);
    position_ = std::move(t.position_);
    facing_ = std::move(t.facing_);
    health_ = std::move(t.health_);
    flags_ = std::move(t.flags_);
    registered_serviced_ = std::move(t.registered_serviced_);
    container_ = std::move(t.container_);
    attach_parent_ = std::move(t.attach_parent_);
    carried_type_ = std::move(t.carried_type_);
    landing_persist_ = std::move(t.landing_persist_);
    behaviors_ = std::move(t.behaviors_);
    beh_state_ = std::move(t.beh_state_);
    behavior_index_ = std::move(t.behavior_index_);
    free_ = std::move(t.free_);
    all_ = std::move(t.all_);
    serviced_ = std::move(t.serviced_);
    by_owner_ = std::move(t.by_owner_);
    tables_ = std::move(t.tables_);
    by_id_ = std::move(t.by_id_);
    delayed_ = std::move(t.delayed_);
    pending_ = std::move(t.pending_);
    last_frame_ = t.last_frame_;
    attach_draws_ = t.attach_draws_;
    ids_->set_next(next_id);
    return true;
}

void ObjectStore::register_crc(CrcRegistry& registry, const std::string& name) const {
    registry.add(name, [this](StateHasher& h) { hash_state(h); });
    registry.add_parity(name + ".parity", [this] { return parity_crc(); });
}

ObjectHandle mark_parent_mode_object_for_death(ObjectStore& parent, const ObjectStore& child_store, ObjectHandle dying) {
    const ObjectHandle c = parent.find(child_store.parent_mode_id(dying));
    if (!parent.alive(c)) return {};
    const TypeIndex t = child_store.type(dying);
    // OBJECT_RUNTIME §2.5 exception [M]: a type placed at markers (type byte +0xE3, OBJECT_TYPES §9.4) is not marked
    // while another live object of the same type and owner is still in the tactical store.
    if (child_store.object_types().placed_at_markers(t)) {
        const int32_t owner = child_store.owner(dying);
        for (ObjectHandle h : child_store.all_objects())
            if (h != dying && child_store.alive(h) && child_store.type(h) == t && child_store.owner(h) == owner) return {};
    }
    if (parent.type(c) == t) {
        parent.set_flag(c, kFlagMarkedByChildMode);
        return c;
    }
    for (ObjectHandle h : parent.all_objects())
        if (parent.alive(h) && parent.container(h) == c && parent.type(h) == t &&
            !(parent.flags(h) & kFlagMarkedByChildMode)) {
            parent.set_flag(h, kFlagMarkedByChildMode);
            return h;
        }
    return {};
}

}  // namespace alamo::objects

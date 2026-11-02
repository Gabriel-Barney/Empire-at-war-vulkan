#include "alamo/tactical/presence/presence_system.hpp"

#include <algorithm>
#include <limits>

#include "alamo/core/sim_rng.hpp"

namespace alamo::tactical::presence {

namespace {

constexpr uint32_t kMinBuckets = 8;

template <class T>
auto lower_by_id(std::vector<T>& v, ObjectId id, ObjectId T::*key) {
    return std::lower_bound(v.begin(), v.end(), id, [key](const T& e, ObjectId k) { return e.*key < k; });
}
template <class T>
auto lower_by_id(const std::vector<T>& v, ObjectId id, ObjectId T::*key) {
    return std::lower_bound(v.begin(), v.end(), id, [key](const T& e, ObjectId k) { return e.*key < k; });
}

// The cube test of the box query, inclusive on all three axes [M].
bool in_cube(const UnitView& u, float cx, float cy, float cz, float h) {
    const float dx = u.x - cx, dy = u.y - cy, dz = u.z - cz;
    return dx >= -h && dx <= h && dy >= -h && dy <= h && dz >= -h && dz <= h;
}

}  // namespace

// ---- type table -----------------------------------------------------------------------------------------

bool PresenceTypeTable::lists(TypeId affected, TypeId presence) const {
    const PresenceTypeInfo* a = find(affected);
    if (!a || presence == kNoType) return false;
    for (const InducedAnimation& e : a->induced)
        for (const TypeId t : e.types)
            if (t == presence) return true;
    return false;
}

int32_t PresenceTypeTable::induced_anim(TypeId affected, TypeId presence) const {
    const PresenceTypeInfo* a = find(affected);
    if (!a || presence == kNoType) return kNoAnimType;
    for (const InducedAnimation& e : a->induced)
        for (const TypeId t : e.types)
            if (t == presence) return e.anim_type;
    return kNoAnimType;
}

int32_t PresenceConfig::relaxation_frames() const {
    return static_cast<int32_t>(static_cast<float>(fps) * relaxation_seconds);
}

// ---- the hash-map list order (A1_READINGS §30.1) -----------------------------------------------------------

uint32_t PresenceSystem::bucket_of(ObjectId key, uint32_t buckets) {
    // minstd step (Schrage): q = x / 127773, r = x % 127773, v = 16807 r - 2836 q, v += 0x7fffffff when negative.
    const int32_t x = static_cast<int32_t>((static_cast<uint32_t>(key) ^ 0xDEADBEEFu) & 0x7FFFFFFFu);
    const int32_t q = x / 127773, r = x % 127773;
    int32_t v = 16807 * r - 2836 * q;
    if (v < 0) v += 0x7FFFFFFF;
    return static_cast<uint32_t>(v) & (buckets - 1u);
}

namespace {

// Place `e` into `list` (bucket count `b`): bucket empty -> append; else inside the bucket's run, ascending by key
// (after the last key < e's key, else before the run's first node).
void place(std::vector<PresenceEntry>& list, const PresenceEntry& e, uint32_t b) {
    constexpr size_t kNpos = static_cast<size_t>(-1);
    const uint32_t bk = PresenceSystem::bucket_of(e.presence, b);
    size_t first = kNpos, last_less = kNpos;
    for (size_t i = 0; i < list.size(); ++i) {
        if (PresenceSystem::bucket_of(list[i].presence, b) != bk) continue;
        if (first == kNpos) first = i;
        if (list[i].presence < e.presence) last_less = i;
    }
    const size_t at = first == kNpos ? list.size() : (last_less != kNpos ? last_less + 1 : first);
    list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), e);
}

}  // namespace

void PresenceSystem::insert_or_refresh(Map& m, ObjectId key, int32_t value) {
    for (PresenceEntry& e : m.list)
        if (e.presence == key) {
            e.remaining = value;
            return;
        }
    // Grow before the insert that would exceed max load 1.0: x8 below 512 buckets, else the next power of two.
    if (static_cast<float>(m.list.size() + 1) / static_cast<float>(m.buckets) > 1.0f) {
        uint32_t nb = m.buckets < 512u ? m.buckets * 8u : m.buckets;
        const auto need = static_cast<uint32_t>(m.list.size() + 1);
        while (nb < need) nb *= 2u;
        std::vector<PresenceEntry> regrouped;
        regrouped.reserve(m.list.size() + 1);
        for (const PresenceEntry& n : m.list) place(regrouped, n, nb);
        m.list.swap(regrouped);
        m.buckets = nb;
    }
    place(m.list, PresenceEntry{key, value}, m.buckets);
}

// ---- registration ---------------------------------------------------------------------------------------

bool PresenceSystem::add_presence_unit(ObjectId id, uint32_t frame, SimRng* rng) {
    auto it = lower_by_id(units_, id, &PresenceUnit::id);
    if (it != units_.end() && it->id == id) return false;
    uint32_t due = frame;
    if (cfg_.interval >= 2 && rng) due = frame + static_cast<uint32_t>(rng->range_int(0, static_cast<int32_t>(cfg_.interval) - 1));
    units_.insert(it, PresenceUnit{id, due});
    return true;
}

bool PresenceSystem::remove_presence_unit(ObjectId id) {
    auto it = lower_by_id(units_, id, &PresenceUnit::id);
    if (it == units_.end() || it->id != id) return false;
    units_.erase(it);
    return true;
}

bool PresenceSystem::is_presence_unit(ObjectId id) const {
    auto it = lower_by_id(units_, id, &PresenceUnit::id);
    return it != units_.end() && it->id == id;
}

uint32_t PresenceSystem::next_due(ObjectId id) const {
    auto it = lower_by_id(units_, id, &PresenceUnit::id);
    return it != units_.end() && it->id == id ? it->due : 0xFFFFFFFFu;
}

void PresenceSystem::remove_object(ObjectId id) {
    remove_presence_unit(id);
    auto it = lower_by_id(maps_, id, &Map::owner);
    if (it != maps_.end() && it->owner == id) maps_.erase(it);
}

PresenceSystem::Map* PresenceSystem::find_map(ObjectId id) {
    auto it = lower_by_id(maps_, id, &Map::owner);
    return it != maps_.end() && it->owner == id ? &*it : nullptr;
}

const PresenceSystem::Map* PresenceSystem::find_map(ObjectId id) const {
    auto it = lower_by_id(maps_, id, &Map::owner);
    return it != maps_.end() && it->owner == id ? &*it : nullptr;
}

PresenceSystem::Map& PresenceSystem::map_for(ObjectId id) {
    auto it = lower_by_id(maps_, id, &Map::owner);
    if (it != maps_.end() && it->owner == id) return *it;
    Map m;
    m.owner = id;
    return *maps_.insert(it, std::move(m));
}

// A cleared map keeps its bucket count (MSVC clear()); only the default-sized empty map is dropped from the store.
void PresenceSystem::drop_map_if_empty(ObjectId id) {
    auto it = lower_by_id(maps_, id, &Map::owner);
    if (it != maps_.end() && it->owner == id && it->list.empty() && it->buckets == kMinBuckets) maps_.erase(it);
}

// ---- services -------------------------------------------------------------------------------------------

uint32_t PresenceSystem::service_presence(ObjectId id, uint32_t frame, const PresenceWorld& world) {
    auto it = lower_by_id(units_, id, &PresenceUnit::id);
    if (it == units_.end() || it->id != id || frame < it->due) return 0;
    it->due = frame + cfg_.interval;  // base BehaviorClass::Service: +0x30 := frame + interval
    ++stats_.services;
    UnitView p;
    if (!world.unit(id, p)) return 0;
    const PresenceTypeInfo* pt = types_.find(p.type);
    const float h = pt ? pt->presence_range : 0.0f;
    if (!(h >= 0.0f)) return 0;  // negative / NaN range: an empty cube [port]

    std::vector<ObjectId> found;
    world.box_query(p.x, p.y, p.z, h, found);
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());

    const int32_t value = cfg_.relaxation_frames();
    uint32_t written = 0;
    for (const ObjectId c : found) {
        UnitView u;
        if (!world.unit(c, u) || !in_cube(u, p.x, p.y, p.z, h)) continue;
        if (u.dying) continue;                          // +0x3a0 & 0x42
        if (!types_.lists(u.type, p.type)) continue;    // 0x370630
        if (!u.has_idle) continue;                      // +0x339 != -1
        // 0x62e450: gates on the affected unit.
        if (u.has_target) continue;
        const PresenceTypeInfo* ut = types_.find(u.type);
        if (u.has_locomotor && !(ut && ut->allow_idle_when_moving) && u.moving) continue;
        // The original stores int(FPS x relaxation) as is; a value < 1 would be removed by the next aging step.
        insert_or_refresh(map_for(c), id, value);
        ++written;
        ++stats_.refreshes;
    }
    return written;
}

IdleMaintenance PresenceSystem::service_idle(ObjectId id, const PresenceWorld& world) {
    Map* m = find_map(id);
    if (!m || m->list.empty()) return IdleMaintenance::None;
    UnitView u;
    if (!world.unit(id, u)) {
        remove_object(id);
        return IdleMaintenance::Dropped;
    }
    const PresenceTypeInfo* ut = types_.find(u.type);
    if (u.has_locomotor && !(ut && ut->allow_idle_when_moving) && u.moving) {
        m->list.clear();
        ++stats_.cleared;
        drop_map_if_empty(id);
        return IdleMaintenance::ClearedMoving;
    }
    if (u.has_target) {
        m->list.clear();
        ++stats_.cleared;
        drop_map_if_empty(id);
        return IdleMaintenance::ClearedTarget;
    }
    // Deploys types while deployed or (un)deploying: early return before the decrement [M for the position].
    if (ut && ut->deploys && u.deploy_busy) return IdleMaintenance::Frozen;
    size_t kept = 0;
    for (size_t i = 0; i < m->list.size(); ++i) {
        PresenceEntry e = m->list[i];
        --e.remaining;
        if (e.remaining < 1) {
            ++stats_.expired;
            continue;
        }
        m->list[kept++] = e;
    }
    m->list.resize(kept);
    drop_map_if_empty(id);
    return IdleMaintenance::Aged;
}

void PresenceSystem::update(uint32_t frame, const PresenceWorld& world) {
    // Newest first (descending id). The next object is looked up after each step, so a map created during this walk
    // for a lower id is aged in the same frame, as the original's later IDLE service of that unit would.
    int64_t cursor = static_cast<int64_t>(std::numeric_limits<ObjectId>::max()) + 1;
    for (;;) {
        int64_t next = std::numeric_limits<int64_t>::min();
        // Largest id < cursor in each sorted list.
        auto pick = [&next, cursor](const auto& vec, auto key) {
            auto i = std::lower_bound(vec.begin(), vec.end(), cursor,
                                      [key](const auto& e, int64_t k) { return static_cast<int64_t>(e.*key) < k; });
            if (i != vec.begin()) next = std::max(next, static_cast<int64_t>((*(i - 1)).*key));
        };
        pick(units_, &PresenceUnit::id);
        pick(maps_, &Map::owner);
        if (next == std::numeric_limits<int64_t>::min()) break;
        const auto id = static_cast<ObjectId>(next);
        service_idle(id, world);
        service_presence(id, frame, world);
        cursor = next;
    }
}

// ---- reads ----------------------------------------------------------------------------------------------

ObjectId PresenceSystem::winner(ObjectId id, const PresenceWorld& world) const {
    const Map* m = find_map(id);
    if (!m || m->list.empty()) return kNoObject;
    UnitView self;
    const bool have_self = world.unit(id, self);
    ObjectId best = kNoObject;
    int32_t best_remaining = 0;
    bool best_allied = false;
    for (const PresenceEntry& e : m->list) {
        UnitView c;
        if (!world.unit(e.presence, c)) continue;  // not live: skipped, the entry stays
        const bool allied = have_self && world.allied(self.owner, c.owner);
        if (best == kNoObject || e.remaining > best_remaining || (allied && !best_allied)) {
            best = e.presence;
            best_remaining = e.remaining;
            best_allied = allied;
        }
    }
    return best;
}

int32_t PresenceSystem::presence_idle(ObjectId id, const PresenceWorld& world) const {
    const ObjectId w = winner(id, world);
    if (w == kNoObject) return kNoAnimType;
    UnitView self, c;
    if (!world.unit(id, self) || !world.unit(w, c)) return kNoAnimType;
    return types_.induced_anim(self.type, c.type);
}

int32_t PresenceSystem::default_idle(ObjectId id, bool spread_out, const PresenceWorld& world) const {
    if (spread_out) return kAnimIdleCrouch;
    const Map* m = find_map(id);
    if (!m || m->list.empty()) return kAnimIdle;
    return presence_idle(id, world);
}

std::span<const PresenceEntry> PresenceSystem::entries(ObjectId id) const {
    const Map* m = find_map(id);
    return m ? std::span<const PresenceEntry>(m->list) : std::span<const PresenceEntry>();
}

int32_t PresenceSystem::remaining(ObjectId affected, ObjectId presence) const {
    for (const PresenceEntry& e : entries(affected))
        if (e.presence == presence) return e.remaining;
    return 0;
}

}  // namespace alamo::tactical::presence

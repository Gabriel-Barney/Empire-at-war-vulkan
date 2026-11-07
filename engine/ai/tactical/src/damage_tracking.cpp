#include "alamo/ai/tactical/damage_tracking.hpp"

#include "alamo/core/sim_crc.hpp"

namespace alamo::ai::tactical {

void DamageTrackingStore::enable(int32_t object, float max_health, float max_shield) {
    Tracked& t = objects_[object];
    t.max_health = max_health;
    t.max_shield = max_shield;
}

void DamageTrackingStore::disable(int32_t object) { objects_.erase(object); }

void DamageTrackingStore::record(int32_t victim, int32_t attacker, float amount) {
    const auto it = objects_.find(victim);
    if (it == objects_.end()) return;
    it->second.by_attacker[attacker] += amount;
}

void DamageTrackingStore::decay(Tracked& t, float decay_step) {
    const float step = (t.max_health + t.max_shield) * decay_step;
    for (auto it = t.by_attacker.begin(); it != t.by_attacker.end();) {
        it->second -= step;
        if (it->second <= 0.0f)
            it = t.by_attacker.erase(it);
        else
            ++it;
    }
}

void DamageTrackingStore::service(int32_t victim, float decay_step) {
    const auto it = objects_.find(victim);
    if (it != objects_.end()) decay(it->second, decay_step);
}

void DamageTrackingStore::service_all(float decay_step) {
    for (auto& kv : objects_) decay(kv.second, decay_step);
}

const std::map<int32_t, float>* DamageTrackingStore::entries(int32_t victim) const {
    const auto it = objects_.find(victim);
    return it == objects_.end() ? nullptr : &it->second.by_attacker;
}

void DamageTrackingStore::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(objects_.size()));
    for (const auto& [id, t] : objects_) {
        h.i32(id);
        h.f32(t.max_health);
        h.f32(t.max_shield);
        h.u32(static_cast<uint32_t>(t.by_attacker.size()));
        for (const auto& [a, d] : t.by_attacker) {
            h.i32(a);
            h.f32(d);
        }
    }
}

namespace {
void collect_rec(const DamageTrackingStore& store, int32_t object, const TeamMembersFn& members,
                 std::vector<int32_t>& out, int depth) {
    if (store.tracks(object)) {
        out.push_back(object);
        return;
    }
    if (!members || depth > 8) return;  // teams nest at most a couple of levels; guard cycles
    std::vector<int32_t> m;
    members(object, m);
    for (int32_t c : m) collect_rec(store, c, members, out, depth + 1);
}
}  // namespace

void collect_tracked(const DamageTrackingStore& store, int32_t object, const TeamMembersFn& members,
                     std::vector<int32_t>& out) {
    collect_rec(store, object, members, out, 0);
}

int32_t find_deadly_enemy(const DamageTrackingStore& store, std::span<const int32_t> victims,
                          const std::function<bool(int32_t attacker)>& exists) {
    std::map<int32_t, float> total;
    for (int32_t v : victims) {
        const std::map<int32_t, float>* e = store.entries(v);
        if (!e) continue;
        for (const auto& [a, d] : *e) total[a] += d;
    }
    int32_t best = kNoAttacker;
    float best_v = 0.0f;
    for (const auto& [a, d] : total) {
        if (best == kNoAttacker || d > best_v) {
            best = a;
            best_v = d;
        }
    }
    if (best != kNoAttacker && exists && !exists(best)) return kNoAttacker;
    return best;
}

}  // namespace alamo::ai::tactical

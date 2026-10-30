#include "alamo/ai/tactical/perception_cache.hpp"

#include "alamo/ai/tactical/constants.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo::ai::tactical {

bool leaf_cache_hit(int32_t lifetime_s, uint32_t stamp, uint32_t now) {
    if (lifetime_s == kCacheNone || stamp == 0) return false;
    if (lifetime_s == kCacheForever) return true;
    if (lifetime_s < 0) return false;
    const uint64_t until = static_cast<uint64_t>(stamp) +
                           static_cast<uint64_t>(lifetime_s) * static_cast<uint64_t>(kFramesPerSecond);
    return static_cast<uint64_t>(now) <= until;
}

bool LeafCache::lookup(const LeafCacheKey& key, int32_t lifetime_s, uint32_t now, double& out) const {
    if (lifetime_s == kCacheNone) return false;
    std::lock_guard<std::mutex> lock(mu_);
    const auto it = map_.find(key);
    if (it == map_.end() || !leaf_cache_hit(lifetime_s, it->second.stamp, now)) return false;
    out = it->second.value;
    ++hits_;
    return true;
}

void LeafCache::store(const LeafCacheKey& key, int32_t lifetime_s, uint32_t now, double value) {
    if (lifetime_s == kCacheNone) return;
    std::lock_guard<std::mutex> lock(mu_);
    Entry& e = map_[key];
    e.value = value;
    e.stamp = now;
}

void LeafCache::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    map_.clear();
    hits_ = 0;
}

size_t LeafCache::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return map_.size();
}

uint64_t LeafCache::hits() const {
    std::lock_guard<std::mutex> lock(mu_);
    return hits_;
}

void LeafCache::hash(StateHasher& h) const {
    std::lock_guard<std::mutex> lock(mu_);
    h.u64(static_cast<uint64_t>(map_.size()));
    for (const auto& [k, e] : map_) {
        h.u8(k.eval_kind);
        h.i32(k.eval_a);
        h.i32(k.eval_b);
        h.u16(k.leaf);
        h.i32(k.self);
        h.i32(k.target);
        h.i32(k.enemy);
        h.i32(k.human);
        h.i32(k.location);
        h.str(k.params);
        h.u32(e.stamp);
        h.f64(e.value);
    }
}

}  // namespace alamo::ai::tactical

#include "alamo/tactical/abilities/space_fields.hpp"

#include <algorithm>

#include "alamo/core/sim_crc.hpp"

namespace alamo::tactical::abilities {

bool SpaceFieldIndex::remove(combat::ObjectId id) {
    const auto it = std::find_if(fields_.begin(), fields_.end(), [id](const SpaceField& f) { return f.id == id; });
    if (it == fields_.end()) return false;
    fields_.erase(it);
    return true;
}

uint32_t SpaceFieldIndex::kinds_at(float x, float y) const {
    uint32_t k = 0;
    for (const SpaceField& f : fields_) {
        const float dx = x - f.x, dy = y - f.y;
        if (dx * dx + dy * dy <= f.radius * f.radius) k |= f.kinds;
    }
    return k;
}

int32_t SpaceFieldIndex::nearest(float x, float y, uint32_t mask) const {
    int32_t best = -1;
    float best_d2 = 0.0f;
    for (size_t i = 0; i < fields_.size(); ++i) {
        const SpaceField& f = fields_[i];
        if (!(f.kinds & mask)) continue;
        const float dx = x - f.x, dy = y - f.y, d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < best_d2) {
            best = static_cast<int32_t>(i);
            best_d2 = d2;
        }
    }
    return best;
}

void SpaceFieldIndex::hash(StateHasher& h) const {
    h.u32(static_cast<uint32_t>(fields_.size()));
    for (const SpaceField& f : fields_) {
        h.i32(f.id);
        h.u32(f.kinds);
        h.f32(f.x);
        h.f32(f.y);
        h.f32(f.radius);
    }
}

}  // namespace alamo::tactical::abilities

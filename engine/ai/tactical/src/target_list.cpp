#include "alamo/ai/tactical/target_list.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "alamo/core/sim_crc.hpp"
#include "alamo/core/text.hpp"

namespace alamo::ai::tactical {

namespace {

int32_t cell_count(float extent, float size) {
    if (!(extent > 0.0f) || !(size > 0.0f)) return 0;
    const float n = std::ceil(extent / size);
    if (!(n < 1.0e6f)) return 0;  // malformed map bounds: refuse instead of allocating millions
    return static_cast<int32_t>(n);
}

}  // namespace

std::string unit_target_name(int32_t object_id) { return "OBJECT_" + std::to_string(object_id); }

std::string cell_target_name(int32_t x, int32_t y) {
    return "CELL_" + std::to_string(x) + "_" + std::to_string(y);
}

Rect2 region_rect(const TacticalTarget& t, float region_size) {
    const float h = region_size * 0.5f;
    return {t.position.x - h, t.position.y - h, t.position.x + h, t.position.y + h};
}

bool TacticalTargetList::is_registered(int32_t object_id) const {
    return std::binary_search(registered_.begin(), registered_.end(), object_id);
}

bool TacticalTargetList::add_unit(int32_t object_id) {
    const auto it = std::lower_bound(registered_.begin(), registered_.end(), object_id);
    if (it != registered_.end() && *it == object_id) return false;
    registered_.insert(it, object_id);
    TacticalTarget t;
    t.kind = TargetKind::Unit;
    t.object_id = object_id;
    t.name = unit_target_name(object_id);
    targets_.push_back(std::move(t));
    return true;
}

uint32_t TacticalTargetList::add_existing_objects(std::span<const TargetObjectInfo> objects_in_gom_order) {
    uint32_t added = 0;
    for (const TargetObjectInfo& o : objects_in_gom_order) {
        if (o.dead_or_removing) continue;
        // [M] the space init path (0x6ba320) is assumed to apply the same dead/removing test.
        const bool flag = mode_ == TacticalMode::Land ? o.rep_has_land_evaluator : o.own_has_space_evaluator;
        if (flag && add_unit(o.id)) ++added;
    }
    return added;
}

uint32_t TacticalTargetList::add_land_regions(std::span<const HintZone> zones) {
    if (mode_ != TacticalMode::Land) return 0;
    uint32_t added = 0;
    for (size_t i = 0; i < zones.size(); ++i) {
        TacticalTarget t;
        t.kind = TargetKind::Region;
        t.name = zones[i].name;
        t.position = {zones[i].center[0], zones[i].center[1], zones[i].center[2]};
        t.zone = static_cast<int32_t>(i);
        targets_.push_back(std::move(t));
        ++added;
    }
    return added;
}

uint32_t TacticalTargetList::build_space_grid(const Rect2& bounds, float cell_size, bool threat_grid_exists) {
    if (mode_ != TacticalMode::Space || !threat_grid_exists) return 0;
    const int32_t nx = cell_count(bounds.max_x - bounds.min_x, cell_size);
    const int32_t ny = cell_count(bounds.max_y - bounds.min_y, cell_size);
    uint32_t added = 0;
    for (int32_t x = 0; x < nx; ++x) {
        for (int32_t y = 0; y < ny; ++y) {
            // [M] arithmetic form: lo = min + i * size, hi = lo + size (unclipped), centre = (lo + hi) / 2.
            const float x0 = bounds.min_x + static_cast<float>(x) * cell_size;
            const float y0 = bounds.min_y + static_cast<float>(y) * cell_size;
            const float x1 = x0 + cell_size, y1 = y0 + cell_size;
            TacticalTarget t;
            t.kind = TargetKind::Region;
            t.name = cell_target_name(x, y);
            t.position = {(x0 + x1) * 0.5f, (y0 + y1) * 0.5f, 0.0f};
            t.eval_rect = {x0, y0, std::min(x1, bounds.max_x), std::min(y1, bounds.max_y)};
            t.cell_x = x;
            t.cell_y = y;
            targets_.push_back(std::move(t));
            ++added;
        }
    }
    return added;
}

bool TacticalTargetList::on_object_added(const TargetObjectInfo& obj) {
    if (obj.dead_or_removing) return false;
    const bool flag = mode_ == TacticalMode::Land ? obj.rep_has_land_evaluator : obj.rep_has_space_evaluator;
    return flag && add_unit(obj.id);
}

bool TacticalTargetList::on_object_destroyed(int32_t object_id) {
    const int32_t i = find_object(object_id);
    if (i < 0) return false;
    targets_.erase(targets_.begin() + i);  // keeps the order of the rest [H]
    const auto it = std::lower_bound(registered_.begin(), registered_.end(), object_id);
    if (it != registered_.end() && *it == object_id) registered_.erase(it);
    return true;
}

int32_t TacticalTargetList::find_object(int32_t object_id) const {
    for (size_t i = 0; i < targets_.size(); ++i)
        if (targets_[i].kind == TargetKind::Unit && targets_[i].object_id == object_id) return static_cast<int32_t>(i);
    return -1;
}

int32_t TacticalTargetList::find_region(std::string_view name) const {
    for (size_t i = 0; i < targets_.size(); ++i)
        if (targets_[i].kind == TargetKind::Region && ascii_iequals(targets_[i].name, name)) return static_cast<int32_t>(i);
    return -1;
}

void TacticalTargetList::hash(StateHasher& h) const {
    h.u8(static_cast<uint8_t>(mode_));
    h.u32(static_cast<uint32_t>(targets_.size()));
    for (const TacticalTarget& t : targets_) {
        h.u8(static_cast<uint8_t>(t.kind));
        h.i32(t.object_id);
        h.str(t.name);
        h.f32(t.position.x);
        h.f32(t.position.y);
        h.f32(t.position.z);
        h.f32(t.eval_rect.min_x);
        h.f32(t.eval_rect.min_y);
        h.f32(t.eval_rect.max_x);
        h.f32(t.eval_rect.max_y);
        h.i32(t.zone);
        h.i32(t.cell_x);
        h.i32(t.cell_y);
    }
}

}  // namespace alamo::ai::tactical

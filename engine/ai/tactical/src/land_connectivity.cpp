#include "alamo/ai/tactical/land_connectivity.hpp"

namespace alamo::ai::tactical {

bool land_points_connected(const ::alamo::tactical::movement::RegionMap& regions, uint32_t mask, float ax, float ay, float bx,
                           float by) {
    namespace mv = ::alamo::tactical::movement;
    const int32_t slot = regions.mask_slot(mask);
    if (slot < 0) return false;
    const int32_t ca = regions.component_at(slot, mv::world_to_cell(ax), mv::world_to_cell(ay));
    const int32_t cb = regions.component_at(slot, mv::world_to_cell(bx), mv::world_to_cell(by));
    return ca >= 0 && ca == cb;
}

}  // namespace alamo::ai::tactical

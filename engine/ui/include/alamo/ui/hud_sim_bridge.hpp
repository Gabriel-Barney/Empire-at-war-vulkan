// Header-only glue from the R1 render snapshot (engine/render sim_snapshot.hpp) to HudUnit records. Include it
// only from code that links alamo_render_sim (the app); alamo_ui_hud itself does not depend on render.
//
//   render::interpolate(prev, curr, alpha, mode, frame);                 // the frame the renderer draws
//   ui::hud_units_from_frame(frame, selection, groups, hovered, radius, units);
//   hud.build(hud_frame, units, hud_types);                              // hud_types = build_hud_type_table(types)
//
// Read-only: the snapshot is an immutable copy of a finished tick; `selection` is the synced selection the host
// also gives OrderTranslator::set_selection (ascending ids), `control_group(id)` the synced group of an id (-1).
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>

#include "alamo/render/sim_snapshot.hpp"
#include "alamo/ui/hud_world.hpp"

namespace alamo::ui {

// Shield fraction source until R1 exposes shields in the snapshot (open item): -1 = no bar.
using HudShieldFn = std::function<float(const render::SnapshotObject&)>;

inline void hud_units_from_frame(const render::InterpolatedFrame& frame, std::span<const uint32_t> selection,
                                 const std::function<int8_t(uint32_t)>& control_group, uint32_t hovered,
                                 const std::function<float(objects::TypeIndex)>& radius, std::vector<HudUnit>& out,
                                 const HudShieldFn& shield = {}) {
    out.clear();
    out.reserve(frame.objects.size());
    auto to8 = [](float f) { return static_cast<uint8_t>(std::lround(std::clamp(f, 0.0f, 1.0f) * 255.0f)); };
    for (const render::SnapshotObject& o : frame.objects) {  // ascending id
        if ((o.flags & render::kSnapDying) != 0 || o.fade <= 0.0f) continue;
        HudUnit u;
        const auto oid = static_cast<uint32_t>(o.id);  // objects::ObjectId is signed; ids are >= 1
        u.id = oid;
        u.type = o.type;
        u.owner = o.owner;
        u.position = {o.position.x, o.position.y, o.position.z};
        u.radius = radius ? radius(o.type) : 0.0f;
        u.health = o.health_fraction;
        u.shield = shield ? shield(o) : -1.0f;
        u.control_group = control_group ? control_group(oid) : int8_t{-1};
        u.selected = std::binary_search(selection.begin(), selection.end(), oid);
        u.hovered = oid == hovered;
        u.team_color = {to8(o.team_color.x), to8(o.team_color.y), to8(o.team_color.z), 255};
        u.jammed = frame.jamming.is_jammed(o.id);  // R5: the radar drops hostile jammed blips (tactical_hud.cpp)
        u.stealthed = frame.jamming.is_stealthed(o.id);  // R5: ... and hostile stealthed ones
        out.push_back(u);
    }
}

}  // namespace alamo::ui

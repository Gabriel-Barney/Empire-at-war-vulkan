// HudTypeInfo table from the typed object view (engine/objects). Compiled when alamo_objects is configured.
#include <algorithm>
#include <string>

#include "alamo/core/hash.hpp"
#include "alamo/data/values.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/ui/hud_world.hpp"

namespace alamo::ui {

std::vector<HudTypeInfo> build_hud_type_table(const objects::GameObjectTypes& types) {
    std::vector<HudTypeInfo> out(types.size());
    for (size_t i = 0; i < types.size(); ++i) {
        const auto t = static_cast<objects::TypeIndex>(i);
        HudTypeInfo& h = out[i];
        h.name = std::string(types.raw().types().name[i]);
        const std::string_view bracket = data::trim(types.get_string(t, "GUI_Bracket_Size"));
        h.gui_bracket_size = bracket.empty() ? -1 : data::parse_int_or(bracket, -1);
        h.hide_health_bar = types.get_bool(t, "GUI_Hide_Health_Bar");
        const std::string_view bounds = data::trim(types.get_string(t, "GUI_Bounds_Scale"));
        h.bounds_scale = bounds.empty() ? 1.0f : data::parse_float_or(bounds, 1.0f);
        h.select_box_scale = types.get_float(t, "Select_Box_Scale");
        h.select_box_z_adjust = types.get_float(t, "Select_Box_Z_Adjust");
        const std::string_view sc = data::trim(types.get_string(t, "Scale_Factor"));
        h.scale = sc.empty() ? 1.0f : data::parse_float_or(sc, 1.0f);
        h.selection_blob_material = std::string(data::trim(types.get_string(t, "Selection_Blob_Material_Name")));
        h.has_shields = types.get_float(t, "Shield_Points") > 0.0f;
        h.is_hero = types.get_bool(t, "Is_Named_Hero");
        h.icon_name = std::string(data::trim(types.get_string(t, "Icon_Name")));
        h.radar_icon = std::string(data::trim(types.get_string(t, "Radar_Icon_Name")));
        float rs[2] = {0.0f, 0.0f};
        if (data::parse_floats(types.get_string(t, "Radar_Icon_Size"), rs, 2) == 2) h.radar_icon_size = {rs[0], rs[1]};
        auto flag = [&](std::string_view tag) {  // absent = Yes [port]
            const std::string_view v = data::trim(types.get_string(t, tag));
            if (v.empty()) return true;
            const auto b = data::parse_bool(v);
            return b ? *b : true;
        };
        h.visible_on_radar = flag("Is_Visible_On_Radar");
        h.visible_on_enemy_radar = flag("Is_Visible_On_Enemy_Radar");
        const std::span<const std::string_view> cats = types.get_list(t, "CategoryMask");
        // GUI_Bracket_Size -1: class from the category (derive_bracket_classes).
        const BracketClasses bc = derive_bracket_classes(cats);
        h.land_class = bc.land;
        h.space_class = bc.space;
    }
    return out;
}

}  // namespace alamo::ui

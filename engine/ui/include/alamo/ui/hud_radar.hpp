// P2 stage 2: the tactical radar / minimap (docs/re/UI.md §1.7, RadarMap.xml).
//   * RadarSettings: RadarMap.xml - map events (Event_Model_Name / _Scale / _Duration / Single_Instance /
//     Persistent), space / land backdrop textures, FOW colours (space 25,66,120,100; land 10,10,10,100) and the
//     per-biome passability colours (Default first, then e.g. Temperate);
//   * RadarImage: the dynamic radar picture (HUD texture page kHudPageRadar): a terrain background (host
//     RGBA, or passability classes coloured with the biome palette) with the fog-of-war mask composited over
//     it: out = lerp(background, FOW colour, FOW alpha x (1 - visibility / 255)) per pixel (visibility grid from
//     the host, nearest sample) [port compositing rule]; regenerated only when its inputs change (generation);
//   * RadarEvents: pings (Default_Click on move orders, Default_Unit_Die, Default_Control_Point_Transition, ...) with the
//     event's duration; [port] drawn as an expanding, fading ring of radar-focus quads (the original draws the
//     I_minimap_*.alo event models through an ortho camera - the ring is the stand-in until P4 loads them);
//   * RadarView: map box <-> radar rectangle mapping (world +X right, +Y up on the radar), the camera footprint
//     outline, and click handling: left press / drag on the radar = camera jump request (world x, y), right click
//     = move order for the selection at that point (MoveToPosition, [port]) + a Default_Click ping.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/ui/hud_components.hpp"
#include "alamo/ui/picking.hpp"

namespace alamo {
class FileProvider;
}

namespace alamo::ui {

struct RadarEventType {
    std::string name, model;
    float scale = 0.001f;
    float duration = 1.0f;  // seconds (<= 0 with persistent: until removed)
    bool single_instance = false, persistent = false;
};

// Passability classes in RadarMap.xml colour-name order.
enum class Passability : uint8_t { Steep = 0, Water, Wall, Obstacle, Clear, Impassable, InfantryOnly, Count };
inline constexpr size_t kPassabilityCount = static_cast<size_t>(Passability::Count);
using PassabilityPalette = std::array<Rgba8, kPassabilityCount>;

struct RadarSettings {
    std::map<std::string, RadarEventType> events;  // by upper-case name
    std::string space_backdrop = "i_radar_map_grid.tga", land_backdrop = "i_radar_map_grid.tga";
    Rgba8 space_fow{25, 66, 120, 100}, land_fow{10, 10, 10, 100};
    Rgba8 space_asteroid{103, 130, 139, 127}, space_asteroid_border{174, 171, 200, 127};
    std::map<std::string, PassabilityPalette> palettes;  // by upper-case biome name ("DEFAULT", "TEMPERATE", ...)

    bool parse(const data::XmlNode& root);
    bool load(const FileProvider& files, std::string* error = nullptr);  // Data\XML\RadarMap.xml
    const RadarEventType* event(std::string_view name) const;
    // Biome palette (missing colours / biomes fall back to Default, then to a grey ramp).
    PassabilityPalette palette(std::string_view biome) const;
};

// Space asteroid fields on the radar (RadarMap.xml Space_Asteroid_Field_Color / _Border_Color, UI.md §1.7): discs in
// world units; the host lists the map's fields. [port] shape = disc of `radius` (the field's extent), border = the
// outermost radar pixel ring, both alpha-blended over the background before the fog.
struct RadarField {
    float x = 0, y = 0, radius = 0;
};
struct RadarFieldPaint {
    std::span<const RadarField> fields;
    float min_x = -5000, min_y = -5000, max_x = 5000, max_y = 5000;  // the radar's world box (RadarView)
    Rgba8 fill{103, 130, 139, 127}, border{174, 171, 200, 127};
};

class RadarImage {
public:
    // Background: `terrain_rgba` (w x h x 4, any size: sampled nearest) or empty (transparent - space). Fog:
    // `visibility` fw x fh bytes (0 = fogged .. 255 = visible), empty = no fog. Both grids are world-aligned: row 0
    // = the map's min y (south) edge, column 0 = min x. Output `size` x `size`, row 0 = north (texture top).
    // `fields`: asteroid fields painted over the background, under the fog.
    void build(uint32_t size, std::span<const uint8_t> terrain_rgba, uint32_t tw, uint32_t th,
               std::span<const uint8_t> visibility, uint32_t fw, uint32_t fh, Rgba8 fow_color,
               const RadarFieldPaint& fields = {});
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    std::span<const uint8_t> rgba() const { return px_; }
    uint64_t generation() const { return generation_; }
    bool empty() const { return px_.empty(); }

private:
    std::vector<uint8_t> px_;
    uint32_t w_ = 0, h_ = 0;
    uint64_t generation_ = 0;
};
// Passability classes (w x h, values Passability) -> RGBA with a palette.
std::vector<uint8_t> passability_rgba(std::span<const uint8_t> classes, const PassabilityPalette& palette);

struct RadarEvent {
    std::string type;  // RadarEventType name
    float x = 0, y = 0;
    float age = 0, duration = 1.0f;
    bool persistent = false;
    bool marker = false;  // a story blip (RadarEvents::set_marker): `type` is the Lua blip name
    uint32_t id = 0;
};
class RadarEvents {
public:
    // Adds an event of a RadarMap.xml type (unknown types: 1 s); single-instance types replace their previous
    // ping. Returns its id.
    uint32_t add(const RadarSettings& s, std::string_view type, float x, float y);
    void remove(uint32_t id);
    // Lua Add_Radar_Blip(object, name) / Remove_Radar_Blip(name) (UI.md §1.7; the visual is [port], no spec): a persistent
    // named marker (a ring ping that follows the object). Upsert by case-insensitive name; the age is kept on a move.
    uint32_t set_marker(std::string_view name, float x, float y);
    bool remove_marker(std::string_view name);
    std::vector<std::string> marker_names() const;
    void update(float dt);
    std::span<const RadarEvent> items() const { return items_; }
    void clear() { items_.clear(); }

private:
    std::vector<RadarEvent> items_;
    uint32_t next_ = 1;
};

// Radar rectangle on the canvas <-> the map's world box.
struct RadarView {
    Vec2f center{}, size{171, 175};  // canvas units
    float min_x = -5000, min_y = -5000, max_x = 5000, max_y = 5000;
    bool valid() const { return max_x > min_x && max_y > min_y && size.x > 0 && size.y > 0; }
    Vec2f to_canvas(float wx, float wy) const;
    bool inside(Vec2f canvas_point) const;
    // Canvas point -> world (x, y); clamped to the map box.
    Vec2f to_world(Vec2f canvas_point) const;
};
// World ground footprint of the camera (the four screen corners projected on the plane z = ground_z; corners
// whose ray misses the plane are pushed to the far clip). Order: top-left, top-right, bottom-right, bottom-left.
std::array<Vec2f, 4> camera_footprint(const View& view, float ground_z);

}  // namespace alamo::ui

// alamo_terrain - canonical .ted map reader/writer (docs/re/TERRAIN_WATER_POST.md 1, 5; SPEC.md 1, 6).
//
// A map is one chunk file with four top-level chunks: 0 map info, 3 start positions, 2 preview
// objects, 1 the world (environments 0x100, terrain 0x101, objects 0x102, playable bounds 0x103,
// space camera 0x105, passability 0x10A, AI zones 0x10B, tactical camera 0x10C).
//
// Maps are untrusted mod files: parse_ted() never reads outside its input, never throws, caps
// every allocation by the input size, and returns false (with a message) on malformed structure.
// Unknown chunks / minis are skipped. Fields whose meaning is not established are kept raw.
//
// Environment is field-compatible with alamo::render::Environment (engine/render ted.hpp): every
// member there exists here with the same name, type, default and meaning; the extra members
// (skydome scales/angles, lightning, weather, clouds, field 13) are additions.
#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/terrain/chunk.hpp"

namespace alamo {
class FileProvider;
}

namespace alamo::terrain {

// ---------------------------------------------------------------- constants (TERRAIN_WATER_POST 1.4)
constexpr float kVertexSpacing = 20.0f;         // world units between grid vertices
constexpr float kHeightScale = 0.048828125f;    // raw s16 height -> world units (1/20.48)
constexpr uint32_t kPatchQuads = 31;            // quads per patch side
constexpr uint32_t kPatchVerts = 32;            // vertices per patch side
constexpr uint32_t kMaterialSlots = 64;
constexpr uint32_t kMaxGridVerts = 31 * 64 + 1; // reader cap (vanilla max 373); larger -> error

// Cell passability bits (CellPassabilityType, 1.5).
enum PassabilityBits : uint32_t {
    kPassClear = 0x1, kPassSteep = 0x2, kPassObstacle = 0x4, kPassRebelWall = 0x8, kPassEmpireWall = 0x10,
    kPassWater = 0x20, kPassShield = 0x40, kPassInfantryOnly = 0x80, kPassImpassable = 0x100,
};

// ---------------------------------------------------------------- top chunk 0: map info
struct MapInfo {
    bool present = false;
    uint32_t format = 0;                // 00 (513 in vanilla)
    uint32_t map_type = 0;              // 01 (1 land, 2 space in vanilla)
    std::array<uint32_t, 6> counts{};   // 02..07 u32 (player / start counts ...; [M] names)
    std::string name_key;               // 08 UTF-16 text key, e.g. TEXT_MAP_NAME_MP_LAND_10
    std::string planet_name;            // 09 UTF-16 (e.g. "Endor", "Kuat")
    std::string text_0a;                // 0A UTF-16 (e.g. "control")
    float world_width = 0, world_height = 0;  // 10, 11
    uint8_t flag_12 = 0, flag_0b = 0;   // 12, 0B
    std::vector<RawMini> raw;           // every mini as stored
};

struct Vec2f { float x = 0, y = 0; friend bool operator==(const Vec2f&, const Vec2f&) = default; };

// ---------------------------------------------------------------- top chunk 2: preview objects
struct PreviewObject {
    uint32_t object_id = 0;   // 0C (matches ObjectPlacement::object_id)
    uint32_t type_crc = 0;    // 0D
    Vec2f position;           // 0E/0F, normalised by the map info world size
    friend bool operator==(const PreviewObject&, const PreviewObject&) = default;
};

// ---------------------------------------------------------------- 0x100: environments (TEE, 5.2)
struct Environment {
    // --- members shared with alamo::render::Environment (same names/defaults) ---
    std::string name;                                             // 14
    float light_color[3][3] = {{1, 1, 1}, {1, 1, 1}, {1, 1, 1}};  // 00, 01, 02 sun, fill 1, fill 2
    float sun_specular[3] = {1, 1, 1};                            // 03 (fills get 0)
    float ambient[3] = {0, 0, 0};                                 // 04
    float intensity[3] = {0.75f, 0.5f, 0.5f};                     // 05, 06, 07
    float yaw[3] = {0.0f, 2.0943951f, 3.6651914f};                // 08..0A radians
    float elevation[3] = {0.7853982f, -0.1745329f, -0.1745329f};  // 0B..0D radians
    float fog_color[3] = {1, 1, 1};                               // 0F
    bool fog_enabled = false;                                     // 10
    float fog_start = 1000.0f, fog_end = 10000.0f;                // 11, 12 (min / max of the pair)
    float clear_color[3] = {0.3f, 0.3f, 0.7f};                    // 15 (ctor default, 5.3)
    float shadow_color[3] = {0.5f, 0.5f, 0.5f};                   // 17
    std::string skydome[2];                                       // 19, 1A (GameObjectType names)
    float bloom_strength = 1.0f, bloom_cutoff = 0.9f, bloom_size = 0.25f;  // 23, 24, 28
    float wind_direction_deg = 0.0f, wind_speed = 2.0f;           // 2B, 2C
    // --- additions ---
    uint32_t field_13 = 1;                                        // 13 weather type (>= 0; CU8F_T03_T05_RE §2.2)
    float skydome_scale[2] = {1, 1};                              // 1B, 1C (1 when the name is empty)
    float skydome_angle_deg[2][2] = {{0, 0}, {0, 0}};             // [0] = 1D,1F  [1] = 1E,20 (degrees)
    float lightning_intensity = 0.0f;                             // 21
    std::string weather_effect;                                   // 29
    float cloud_scale = 500.0f;                                   // 2D (overrides the terrain header)
    float cloud_speed = 30.0f;                                    // 2E
    std::string cloud_texture = "W_Clouds00.tga";                 // 2F
    std::vector<RawMini> raw;                                     // every mini as stored

    // Direction towards light i (0 sun, 1-2 fills): (cos e sin a, -cos e cos a, sin e).
    std::array<float, 3> light_direction(int i) const;
    // speed * (cos w, sin w, 0) with w = 2B in degrees.
    std::array<float, 3> wind_vector() const;
    std::array<float, 3> cloud_velocity() const;
};
// Decodes one TEE record (leaf 6). Unknown minis are kept in raw only. False if not a mini list.
bool decode_environment(std::span<const uint8_t> leaf, Environment& out);

struct EnvironmentSet {
    std::vector<Environment> environments;
    uint32_t start_index = 0;            // 0x100/8 mini 25
    uint32_t field_2a = 0xFFFFFFFFu;     // 0x100/8 mini 2A
    bool scene_fog_present = false;      // 0x100/2/0
    bool scene_fog_enabled = false;
    float scene_fog_start = 0, scene_fog_end = 0;
    float scene_fog_color[3] = {1, 1, 1};
    bool legacy_format = false;          // old 0x100/0 + /1 blocks (not decoded; none in vanilla FoC)
    const Environment* start() const {
        return environments.empty() ? nullptr : &environments[start_index < environments.size() ? start_index : 0];
    }
};

// ---------------------------------------------------------------- 0x101: terrain
// TEM record (SPEC 6.1, alTerrain::Mtl 1.7). Defaults are the runtime constructor's.
struct TerrainMaterial {
    bool present = false;                 // a 0x101/2/3 record existed for this slot
    std::string diffuse_texture;          // 0C
    std::string normal_texture;           // 13 (empty -> Generic_Flat_B.tga at load)
    float tile_size = 100.0f;             // 07 world units per repeat
    float rotation_z = 0.0f;              // 08 radians
    float rotation_x = 0.0f;              // 09 radians (applied first)
    float specular_power = 64.0f;         // 06
    float diffuse_color[3] = {1, 1, 1};   // 0A
    float specular_color[3] = {1, 1, 1};  // 0B
    uint32_t surface_type = 0;            // 14
    uint8_t flag_0d = 0;                  // 0D (no runtime reader)
    bool full_coverage = false;           // 12
    bool not_rendered = false;            // 23
    std::vector<RawMini> raw;

    // diffuseTexU / diffuseTexV (float4, dotted with world (x, y, z, 1)), 1.7:
    // U0 = (1/t,0,0,0), V0 = (0,1/t,0,0); rotate both by rotation_x about X, then rotation_z about Z.
    std::array<float, 4> tex_u() const;
    std::array<float, 4> tex_v() const;
    // Normal map the engine binds (13, or Generic_Flat_B.tga when empty; the low-memory option is
    // not modelled).
    std::string effective_normal_texture() const;
};
bool decode_terrain_material(std::span<const uint8_t> leaf, TerrainMaterial& out);

enum class WaterType : uint32_t { None = 0, Water = 1, Lava = 2, Ice = 3 };

// Water scroll record (0x101/7 and /8 -> child 0x0AA45BCD), 1.3 / 3.4.
struct WaterScroll {
    bool present = false;
    float tile_size = 100.0f;   // 00 world units
    float angle = 0.0f;         // 01 radians
    float speed = 0.0f;         // 02 UV units / s
    float direction = 0.0f;     // 03 radians
};

// Terrain header minis (0x101/0), 1.3. Defaults = runtime constructor values.
struct TerrainHeader {
    uint32_t width = 0, height = 0;         // 00, 01 as stored
    uint32_t vertex_count = 0;              // 04
    uint32_t material_slots = 64;           // 05
    std::string cloud_texture;              // 0E
    float cloud_scale = 500.0f;             // 0F
    float cloud_velocity[3] = {5, 5, 0};    // 10
    bool use_vertex_brightness = false;     // 11
    float water_height = 0.0f;              // 15
    WaterType water_type = WaterType::None; // 16 (values > 3 are kept in water_type_raw, type None)
    uint32_t water_type_raw = 0;
    float reflection_fraction = 0.5f;       // 17 r
    float reflection_distortion = 0.5f;     // 18 (used x0.02)
    float refraction_distortion = 0.5f;     // 19 (used x0.02)
    float water_color[3] = {1, 1, 1};       // 1A
    float unused_1b[3] = {1, 1, 1};         // 1B (not read by the engine)
    std::string water_texture[3];           // 1D, 1E, 1F (empty -> w_generic_white.tga at load)
    float water_alpha = 0.5f;               // 20 (DX8/FF WaterColor.a)
    float shore_fade_depth = 10.0f;         // 21 d
    float shore_min_alpha = 0.0f;           // 22 m
    std::vector<RawMini> raw;
};

// Map decal (0x101/6 -> leaf 0x10 per decal), DECORATIONS.md 4.1. Defaults = runtime ctor values.
// Geometry / fade / colour: decorations.hpp (build_decal_geometry, decal_alpha).
struct TerrainDecal {
    uint32_t field_00 = 0;                // 00 (always 0 in data; meaning unknown [L])
    bool permanent = false;               // 01 (ctor default not established [L]; vanilla data: 1)
    uint32_t render_mode = 3;             // 02 RenderMode (decorations.hpp), default MODULATE
    float position[3] = {0, 0, 0};        // 03 world centre
    float rotation = 0.0f;                // 04 radians about Z
    float width = 1.0f, height = 1.0f;    // 05 world units (ctor default not established [L])
    float intensity = 1.0f;               // 06 alpha
    uint32_t slot_column = 0;             // 07 atlas slot column
    uint32_t slot_row = 0;                // 08 atlas slot row
    float fade_time = 0.0f;               // 0A (or legacy f64 09) seconds; <= 0 never fades
    uint32_t slot_scale = 1;              // 0B atlas cells spanned
    // Not in the file: GameConstants Master_Decal_Slot_Count (absent in vanilla -> 16 = 4x4) [M].
    uint32_t slot_count = 16;
    std::vector<RawMini> raw;             // every mini as stored
};

// Map spline track (0x101/9 -> node 0x100 per track), DECORATIONS.md 5.1. Defaults = ctor values.
// Geometry: decorations.hpp (build_track_geometry).
struct TerrainTrack {
    float field_00 = 10.0f;               // 00 (always 10; editor default width? [L])
    uint32_t flags = 7;                   // 01 1 markers, 2 lines (editor only), 4 textured ribbon
    // 02 spline sample spacing (world units). Stored as u32 in every vanilla map (11..56; read as f32\r
    // it would be a denormal), so it is decoded as an integer [H data].\r
    uint32_t sample_spacing = 50;
    float u_speed = 0.0f, v_speed = 0.0f; // 03, 04 texture scroll (1/s)
    float opacity = 1.0f;                 // 05
    uint32_t stored_point_count = 0;      // 06 (check only; points come from child 0x102)
    float first_row_alpha = 0.0f;         // 0A alpha multiplier of the first row
    float last_row_alpha = 0.0f;          // 0B alpha multiplier of the last row
    float height_offset = 0.0f;           // 0C (height mode 2)
    float repeat_length = 0.0f;           // 0D texture repeat along the track (0 = once over the length)
    bool clamp_u = false, clamp_v = false;// 0E, 0F (0 WRAP, 1 CLAMP)
    uint32_t height_mode = 1;             // 10: 0 water plane + 1, 1 terrain height, 2 terrain + 0C
    std::vector<std::array<float, 3>> points;  // child 0x102, mini 07 each
    std::string texture;                  // child 0x103 (zero-terminated name)
    std::vector<float> widths;            // child 0x104, mini 08 per control point
    std::vector<float> profile = {1, 1, 1};    // child 0x105, mini 09 per vertex across
    bool profile_present = false;         // 0x105 existed (else the default profile is used)
    std::vector<RawMini> raw;             // child 0x101 minis as stored
    bool on_water() const { return height_mode == 0; }  // decoration type 5 (else 3)
};

struct Terrain {
    bool present = false;
    TerrainHeader header;
    // Grid actually used: header size rounded up to 31n+1 (1.3 "resizes to the next 31n+1").
    uint32_t width = 0, height = 0;
    // Row-major (index = y * width + x), width * height entries.
    std::vector<int16_t> heights;
    std::vector<uint8_t> material;          // 0..63 (larger values clamp to 63 at build)
    std::vector<uint8_t> brightness;        // used only when header.use_vertex_brightness
    bool legacy_vertices = false;           // came from child 1 (brightness forced 0xFF)
    std::array<TerrainMaterial, kMaterialSlots> materials{};
    WaterScroll water_scroll[2];            // child 7, child 8
    std::vector<TerrainDecal> decals;       // child 6 (DECORATIONS.md 4.1; only UM11 in vanilla)
    std::vector<TerrainTrack> tracks;       // child 9 (DECORATIONS.md 5.1; 23 FoC maps)

    uint32_t patches_x() const { return width > 1 ? (width - 1) / kPatchQuads : 0; }
    uint32_t patches_y() const { return height > 1 ? (height - 1) / kPatchQuads : 0; }
    // Clamped lookups (1.4: out-of-range clamps to the border).
    int16_t raw_height(int64_t x, int64_t y) const;
    float height_at(int64_t x, int64_t y) const { return static_cast<float>(raw_height(x, y)) * kHeightScale; }
    uint8_t material_at(int64_t x, int64_t y) const;
    // Height of the rendered surface at world (wx, wy): linear over the mesh triangle containing
    // the point (diagonal (0,0)-(1,1) of each quad), clamped to the map.
    float sample_height(float wx, float wy) const;
};

// ---------------------------------------------------------------- 0x102: objects (1.6)
struct ObjectPlacement {
    uint32_t list_id[2] = {0, 0};     // 0x454 minis 00, 01
    uint32_t object_id = 0;           // 0x4B0 00
    uint32_t type_crc = 0;            // 01: CRC-32 of the upper-cased GameObjectType name
    uint32_t owner = 0;               // 02: owner / player slot
    // 03: starting health as a fraction of the type's max tactical health (NOT a scale; the
    // loader sets health = this * max health). Kept as stored so writes are byte-identical;
    // resolve_placements validates it.
    float health_fraction = 1.0f;
    float position[3] = {0, 0, 0};    // 04 world units (z as stored: absolute height)
    float rotation_deg[3] = {0, 0, 0};// 05 Euler degrees, z = yaw
    float rotation_copy[3] = {0, 0, 0};  // 12
    bool has_secondary = false;
    float secondary[3] = {0, 0, 0};   // 15 rally / flag point
    std::vector<RawMini> placement_raw;  // every 0x4B0 mini as stored
    std::vector<std::string> attachments;  // 0x4BA child names that are not "NONE" (hardpoint slots)
    uint32_t attachment_slots = 0;         // number of 0x4BA children
    std::vector<RawMini> owner_raw;        // 0x476/1 minis
    // 0x482 (TERRAIN_WATER_POST 1.6): the object's hint (marker) name, leaf child 0 mini 01 (NUL-terminated);
    // empty = the object has no 0x482 chunk. Lua Find_Hint / Get_Hint read it.
    std::string hint;
};

// ---------------------------------------------------------------- 0x10B: AI zones (cheap decode)
struct AiZone {
    uint32_t id = 0;                  // 02
    std::string name;                 // 01
    uint32_t stored_cells = 0;        // 03
    float center[3] = {0, 0, 0};      // 05
    std::vector<RawMini> raw;         // the zone header minis
    struct Link { uint32_t zone = 0, kind = 0; };
    std::vector<Link> links;          // child E
    struct Cell { uint16_t x = 0, y = 0; uint8_t flag = 0; };
    std::vector<Cell> cells;          // child 7 (01 u32 = x | y << 16, 02 u8)
};

// ---------------------------------------------------------------- 0x103 / 0x105 / 0x10C
struct PlayableArea {
    bool present = false;
    bool has_box = false;
    // Land: 05/06 = playable box. Space: 07/08 = the CAMERA box (SPACE_AND_OPEN A.1); the space
    // playable box is 04/05 (outer_*). Use MapData::playable_box() / space for the meaning.
    float box_min[3] = {0, 0, 0}, box_max[3] = {0, 0, 0};
    bool has_outer_box = false;
    float outer_min[3] = {0, 0, 0}, outer_max[3] = {0, 0, 0};  // space 04/05
    std::vector<RawMini> raw;
};

// Space map description (SPACE_AND_OPEN.md A.1), filled from 0x103 when the map has no terrain.
// Defaults are the space loader's: playable box +-5000 on each axis, camera box = playable box
// shrunk by 400 in X and Y. The camera fields are overwritten by the Space_Mode constants right
// after load in the original (A.1 [M]); they are kept for tools. Read-only view derived from
// PlayableArea::raw (write_ted writes PlayableArea, not this).
struct SpaceMapDesc {
    bool present = false;                       // map is a space map (no 0x101)
    float playable_min[3] = {-5000, -5000, -5000}, playable_max[3] = {5000, 5000, 5000};  // 04 / 05
    float camera_min[3] = {-4600, -4600, -5000}, camera_max[3] = {4600, 4600, 5000};      // 07 / 08
    float camera_pitch = 0.0f, camera_yaw = 0.0f;   // 00, 01
    float camera_look_at[3] = {0, 0, 0};            // 03
    float camera_distance = -1.0f;                  // 02 (-1 = not set)
    float camera_fov = -1.0f;                       // 06 (-1 = not set)
};
struct CameraBlock {
    bool present = false;
    std::vector<RawMini> raw;          // ~40 minis; 00/01 angles, 02 position, 03/04 distances, 08/09 FOV
    float f32_or(uint8_t id, float fallback) const;
};

// ---------------------------------------------------------------- the whole map
struct MapData {
    MapInfo info;
    std::vector<Vec2f> start_positions;     // top chunk 3
    std::vector<PreviewObject> preview_objects;  // top chunk 2
    EnvironmentSet environments;            // 0x100
    Terrain terrain;                        // 0x101 (space maps: present == false)
    std::vector<ObjectPlacement> objects;   // 0x102
    PlayableArea playable;                  // 0x103
    SpaceMapDesc space;                     // 0x103 decoded for space maps (space.present)
    CameraBlock space_camera;               // 0x105
    std::vector<uint32_t> passability;      // 0x10A, converted to ROW-major (y * width + x)
    bool passability_present = false;
    std::vector<uint8_t> legacy_passability;  // 0x104 raw (not in vanilla FoC)
    std::vector<AiZone> zones;              // 0x10B
    CameraBlock tactical_camera;            // 0x10C
    std::vector<uint32_t> unknown_world_chunks;  // world child ids not handled (diagnostics)

    bool is_space() const { return !terrain.present; }
    uint32_t passability_at(uint32_t x, uint32_t y) const;
    // The playable box in world XY (what the FOW grid covers, DECORATIONS.md 2.2): land 0x103
    // 05/06, else the terrain extent; space 0x103 04/05 (default +-5000). False only for a land map
    // with neither a box nor a terrain grid.
    bool playable_box(float (&min_xy)[2], float (&max_xy)[2]) const;
};

struct ParseOptions {
    bool objects = true;      // decode 0x102 (the largest part of a land map)
    bool zones = true;        // decode 0x10B
    // Map info (top chunk 0) + the world's environment set (0x100) only. Every other chunk is
    // skipped unread (start positions, preview, terrain, objects, passability, zones, cameras), so
    // a malformed terrain chunk cannot fail the read. terrain.present is still set when a 0x101
    // chunk exists (is_space() stays meaningful) but the terrain is empty (width 0).
    bool environments_only = false;
};

// False on malformed structure (out holds what was decoded before the error).
bool parse_ted(std::span<const uint8_t> bytes, MapData& out, std::string* error = nullptr,
               const ParseOptions& options = {});
// Writes a map in the same layout (decoded fields + raw minis where kept). parse_ted(write_ted(m))
// reproduces every decoded field of m.
std::vector<uint8_t> write_ted(const MapData& map);

// "_MP_LAND_ENDOR" -> "DATA\ART\MAPS\_MP_LAND_ENDOR.TED" (a name with a separator is kept, .TED
// appended if missing). Same rule as alamo::render::map_path.
std::string map_path(std::string_view name);
bool load_map(const FileProvider& files, std::string_view name, MapData& out, std::string* error = nullptr,
              const ParseOptions& options = {});

// Effective terrain clouds after the environment override (5.3 step 4): environment 2D/2E/2B/2F
// when the map has environments, else the terrain header.
struct CloudParams {
    std::string texture;
    float scale = 500.0f;
    float velocity[3] = {5, 5, 0};
};
CloudParams effective_clouds(const MapData& map, const Environment* env);

}  // namespace alamo::terrain

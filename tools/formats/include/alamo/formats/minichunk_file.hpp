// "Chunks of mini-chunks" files: terrain material sets (.tem) and environment sets (.tee).
//
//   .tem: chunk 0x02 (children) -> 64 x chunk 0x03 (one terrain texture slot each; unused slots
//         have empty texture names)
//   .tee: sequence of leaf chunks 0x06 (one lighting environment each), terminated by an empty
//         chunk 0x05
//
// The generic Record tree is exact. Typed views (TerrainMaterial, Environment) name each field
// with a confidence level; see SPEC.md section 6.
#pragma once

#include "alamo/formats/chunk.hpp"

#include <optional>
#include <string>
#include <vector>

namespace alamo::minifile {

struct Record {
    std::uint32_t chunk_id = 0;
    std::vector<std::uint8_t> mini_ids;
    std::vector<std::vector<std::uint8_t>> mini_data;
    const std::vector<std::uint8_t>* get(std::uint8_t id) const;
    std::optional<float> f32(std::uint8_t id) const;
    std::optional<std::uint32_t> u32(std::uint8_t id) const;   // 1-, 2- or 4-byte payloads
    std::optional<Vec3> vec3(std::uint8_t id) const;
    std::optional<std::string> str(std::uint8_t id) const;
    // Mini-chunks that look like printable NUL-terminated strings (e.g. texture names).
    std::vector<std::string> strings() const;
};

struct File {
    std::vector<Record> records;   // in file order, flattened
};

File parse(Bytes data);
File load(const std::filesystem::path& path);

// ---- typed .tem record (chunk 0x03) ----
struct TerrainMaterial {
    std::string diffuse_texture;   // 0x0C (high)
    std::string normal_texture;    // 0x13 (high; runtime default Generic_Flat_B.tga)
    float tile_size = 100;         // 0x07 world units per texture repeat, UV scale = 1/x (high)
    float uv_rotation_z = 0;       // 0x08 radians, projection rotation about Z (medium)
    float uv_rotation_x = 0;       // 0x09 radians, projection rotation about X (medium)
    float specular_power = 64;     // 0x06 (low; runtime default 64)
    Vec3 diffuse_color{1, 1, 1};   // 0x0A (medium)
    Vec3 specular_color{1, 1, 1};  // 0x0B (medium)
    std::uint32_t surface_type = 0;// 0x14 SurfaceFX surface index (consumed by SurfaceFX and damage
                                   //      multipliers, high); observed 0 default, 1 grass, 2 dirt,
                                   //      3 snow, 5 rock/concrete, 6 mud, 7 ice, 8 lava (names medium)
    std::uint8_t flag_0d = 0;      // 0x0D (low; no runtime consumer found)
    bool full_coverage = false;    // 0x12 layer blend weights forced to full where present (medium)
    bool not_rendered = false;     // 0x23 layer skipped by the terrain renderer (medium)
    bool used() const { return !diffuse_texture.empty(); }
};
TerrainMaterial decode_terrain_material(const Record& r);

// ---- typed .tee record (chunk 0x06) ----
struct Environment {
    std::string name;                         // 0x14 (high)
    std::array<Vec3, 3> light_color{};        // 0x00-0x02 three directional lights (medium)
    std::array<float, 3> light_intensity{};   // 0x05-0x07 (medium)
    std::array<float, 3> light_angle_z{};     // 0x08-0x0A radians (medium)
    std::array<float, 3> light_angle_x{};     // 0x0B-0x0D radians (medium)
    Vec3 specular_color;                      // 0x03 specular colour of light 0 (high)
    Vec3 ambient_color;                       // 0x04 ambient colour (high)
    Vec3 fog_color;                           // 0x0F (high)
    bool fog_enabled = false;                 // 0x10 (high)
    float fog_start = 0, fog_end = 0;         // 0x11/0x12, sorted min/max by the loader (high)
    std::string preset_name;                  // 0x19 e.g. "DAY_SNOW" (low)
    std::string weather_effect;               // 0x29 e.g. "BLIZZARD_RANDOM" (low)
    std::string cloud_texture;                // 0x2F (medium)
};
Environment decode_environment(const Record& r);

}  // namespace alamo::minifile

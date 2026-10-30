// Particle systems stored in .alo files (top-level chunk 0x900).
//
// The emitter property block (chunk 0x700/0x02) is a mini-chunk list whose IDs share one
// "emitter property" ID space with the emitter's child chunks (0x03 texture, 0x16 name,
// 0x29 spawn shapes, 0x36 links, 0x45 bump texture). The five observed block sizes
// (239/245/248/254/260 bytes) are the same list with optional IDs 0x06, 0x47, 0x48, 0x49.
// Field semantics were established from value statistics over 891 emitters and cross-checked
// against the runtime's use of each field; every field carries a confidence level
// (see emitter_fields()).
#pragma once

#include "alamo/formats/chunk.hpp"

#include <array>
#include <bitset>
#include <optional>
#include <string>
#include <vector>

namespace alamo::particle {

enum class Confidence : std::uint8_t { High, Medium, Low };
const char* to_string(Confidence c);

enum class FieldType : std::uint8_t { Bool, U32, F32, Vec3, Vec4 };

struct EmitterFieldInfo {
    std::uint8_t id;
    const char* name;      // EmitterDesc member name
    FieldType type;
    Confidence confidence;
    const char* meaning;
};
// Catalogue of every property-block mini-chunk ID observed (plus legacy IDs the loader accepts).
std::span<const EmitterFieldInfo> emitter_fields();
const EmitterFieldInfo* emitter_field(std::uint8_t id);

// ---- spawn shapes (chunk 0x29: three 0x1100/0x1101 records, 64 bytes each) ----
enum class ShapeKind : std::uint32_t {
    Point = 0,     // constant value (point_value)
    Box = 1,       // uniform in [box_min, box_max]
    Cube = 2,      // uniform in [-cube_half_size, +cube_half_size]^3
    Sphere = 3,    // radius sphere_radius; on the surface if sphere_surface else inside
    Cylinder = 4,  // radius cylinder_radius (rim if cylinder_surface), z in [0, cylinder_height]
    Kind5 = 5,     // accepted by the runtime (spindle-like distribution); not observed in data
};
struct SpawnShape {
    ShapeKind kind = ShapeKind::Point;
    Vec3 box_min, box_max;       // +4, +16
    float cube_half_size = 0;    // +28
    float sphere_radius = 0;     // +32
    bool sphere_surface = false; // +36 (u8)
    float cylinder_radius = 0;   // +40
    bool cylinder_surface = false; // +44 (u8)
    float cylinder_height = 0;   // +48
    Vec3 point_value;            // +52
    std::array<std::uint8_t, 64> raw{};
};

// ---- tracks ----
// Mini 0x05 record, SPEC 3.3: value first, then time. Byte tracks (0..3) store the value as a u32 0..255 (returned
// here as a float 0..255, like Track::start / end); float tracks store an f32.
struct TrackKey {
    float value = 0;
    float time = 0;   // 0..1, fraction of particle life
};
// Emitters carry exactly 7 tracks, each a (0x00 header, 0x01 keys) chunk pair, in this order.
enum TrackIndex : std::size_t {
    TrackRed = 0, TrackGreen = 1, TrackBlue = 2, TrackAlpha = 3,   // byte tracks (0..255)
    TrackScale = 4,          // particle size (medium confidence)
    TrackTextureIndex = 5,   // atlas cell index (high)
    TrackRotationSpeed = 6,  // revolutions per particle life (high)
    TrackCount = 7,
};
const char* track_name(std::size_t index);
struct Track {
    bool byte_values = false;
    float start = 0;   // header mini 0x02 (byte tracks returned as 0..255)
    float end = 0;     // header mini 0x03
    std::uint32_t interpolation = 0;  // header mini 0x04
    std::vector<TrackKey> keys;       // mini 0x05 records
};

// ---- typed emitter description ----
// Particle blend mode = index of the engine "Prim" effect used to draw the emitter (runtime
// table, high confidence). Blend states from the shader catalog (docs/shaders).
// Observed in data: 1 (577 emitters), 2 (299), 4 (14), 5 (1).
enum class BlendMode : std::uint32_t {
    Opaque = 0,                 // Engine\PrimOpaque            (no blend, z-write)
    Additive = 1,               // Engine\PrimAdditive          ONE/ONE
    Alpha = 2,                  // Engine\PrimAlpha             SRCALPHA/INVSRCALPHA
    Modulate = 3,               // Engine\PrimModulate          ZERO/SRCCOLOR
    DepthSpriteAdditive = 4,    // Engine\PrimDepthSpriteAdditive  ONE/ONE, soft (depth) sprite
    DepthSpriteAlpha = 5,       // Engine\PrimDepthSpriteAlpha     SRCALPHA/INVSRCALPHA, soft sprite
    DepthSpriteModulate = 6,    // Engine\PrimDepthSpriteModulate
    DiffuseAlpha = 7,           // Engine\PrimDiffuseAlpha      SRCALPHA/INVSRCALPHA
    StencilDarken = 8,          // Engine\StencilDarken(ToAlpha)
    StencilDarkenFinalBlur = 9, // Engine\StencilDarkenFinalBlur
    Heat = 10,                  // Engine\PrimHeat (heat-distortion phase)
    ParticleBumpAlpha = 11,     // Engine\PrimParticleBumpAlpha SRCALPHA/INVSRCALPHA
    DecalBumpAlpha = 12,        // Engine\PrimDecalBumpAlpha
    AlphaScanlines = 13,        // Engine\PrimAlphaScanlines
};
const char* blend_mode_effect(std::uint32_t mode);  // e.g. "Engine\\PrimAdditive"

struct EmitterDesc {
    // Rendering
    std::uint32_t blend_mode = 0;          // 0x04
    std::uint32_t geometry = 1;            // 0x05 0 = triangles (3 verts), 1 = quads (4 verts)
    std::uint32_t texture_cells = 1;       // 0x10 atlas cell count (square; columns = sqrt)
    bool depth_sort = false;               // 0x2B
    bool alternate_vertex_layout = false;  // 0x2E selects an alternate vertex/UV table (low)
    bool is_heat_particle = false;         // 0x3B rendered in the heat-distortion pass
    bool render_flag_46 = false;           // 0x46 passed to the draw call; meaning low
    bool has_tail = false;                 // 0x41 stretch along velocity
    float tail_size = 0;                   // 0x42
    // Emission
    std::uint32_t index = 0;               // 0x14 emitter index within the system
    bool enabled = true;                   // 0x15 (always 1)
    bool use_bursts = false;               // 0x07
    std::uint32_t particles_per_second = 0;// 0x2A
    float burst_delay = 0;                 // 0x25 seconds between bursts
    std::uint32_t particles_per_burst = 0; // 0x26
    std::uint32_t burst_count = 0;         // 0x27 0 = unlimited
    float initial_delay = 0;               // 0x24 seconds before the first emission
    float stop_time = 0;                   // 0x32 stop emitting after N s (0 = never)
    float skip_time = 0;                   // 0x33 pre-simulate N s at start
    std::uint32_t spawn_source = 0;        // 0x34 0 = spawn shapes, 1/2 = parent mesh, 3 = mesh vertices
    float mesh_normal_offset = 0;          // 0x3C offset along mesh normal for spawn_source 1/2
    // Particle life
    float lifetime = 1;                    // 0x0F seconds
    float random_lifetime = 0;             // 0x13 fraction: life in [L*(1-r), L]
    // Motion
    bool object_space = false;             // 0x08 particles move with the emitter
    Vec3 acceleration;                     // 0x0A
    bool object_space_acceleration = false;// 0x35
    float gravity = 0;                     // 0x0C subtracted from acceleration.z
    float outward_speed = 0;               // 0x09 radial speed from emitter centre
    float outward_acceleration = 0;        // 0x0B radial acceleration (negative = inward)
    bool inherit_velocity = false;         // 0x43
    float inherit_velocity_factor = 0;     // 0x28
    bool affected_by_wind = false;         // 0x31
    bool affected_by_wind_disturbance = false; // 0x44
    std::uint32_t ground_behavior = 0;     // 0x2F 0 = none; 1/2 collide with terrain
    float bounciness = 1;                  // 0x30
    // Randomisation
    Vec4 random_color;                     // 0x2C per-channel variance (fraction, alpha subtracts)
    bool random_color_uniform = false;     // 0x2D same variance for R, G, B
    float random_initial_rotation = 0;     // 0x12
    float random_rotation_speed = 0;       // 0x17 fraction (0..1)
    bool random_rotation_direction = false;// 0x23 50% of particles spin the other way
    bool rotation_fixed_at_spawn = false;  // 0x48 rotation evaluated once at spawn
    // Weather
    bool is_weather_particle = false;      // 0x3D particles fill a camera-relative cube
    float weather_cube_size = 0;           // 0x3E
    float weather_fade_start = 0;          // 0x3F
    float weather_fade_length = 0;         // 0x40
    float weather_camera_distance = 0;     // 0x47
    // Tool-only fields (ignored by the game loader)
    std::optional<std::uint32_t> tool_field_06;   // 0x06 (0 or 40)
    std::optional<float> tool_field_49;           // 0x49 (often uninitialised data)

    std::bitset<256> present;              // which mini IDs were in the block
    struct UnknownField { std::uint8_t id; std::vector<std::uint8_t> data; };
    std::vector<UnknownField> unknown;     // IDs not in the catalogue
};
EmitterDesc parse_emitter_desc(Bytes property_block, std::size_t base_offset = 0);

struct Emitter {
    std::string name;            // 0x16
    std::string color_texture;   // 0x03
    std::string bump_texture;    // 0x45
    std::vector<std::uint8_t> properties;  // 0x02 raw property block
    EmitterDesc desc;                      // decoded 0x02
    // 0x29: three shapes in this order (legacy files used minis 0x19/0x1A/0x1B of chunk 0x00).
    std::vector<SpawnShape> shapes;
    const SpawnShape* velocity_shape() const { return shapes.size() > 0 ? &shapes[0] : nullptr; }
    // Lifetime shape: the runtime recomputes it from lifetime/random_lifetime on load.
    const SpawnShape* lifetime_shape() const { return shapes.size() > 1 ? &shapes[1] : nullptr; }
    const SpawnShape* position_shape() const { return shapes.size() > 2 ? &shapes[2] : nullptr; }
    std::vector<Track> tracks;             // 0x01
    // 0x36 links (emitter indices, 0xFFFFFFFF = none).
    std::uint32_t spawn_on_death = 0xFFFFFFFFu;   // mini 0x37: spawned where a particle dies
    std::uint32_t spawn_during_life = 0xFFFFFFFFu;// mini 0x39: follows each live particle
};

struct ParticleSystem {
    std::string name;                  // 0x00
    std::uint32_t editor_id = 0;       // 0x01 particle-editor system ID (always 0 in shipped data)
    std::uint8_t leave_particles = 0;  // 0x02 keep emitted particles alive when detached
    std::vector<Emitter> emitters;     // 0x800/0x700*
};

// Parse from a 0x900 chunk.
ParticleSystem parse_system(const Chunk& chunk900);

}  // namespace alamo::particle

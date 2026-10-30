// alamo_render_particles - particle system files (ALO top chunk 0x900) as CPU data (R4).
//
// Clean-room: written from docs/formats/SPEC.md section 3 and docs/re/ANIM_PARTICLES.md section B. The byte-level
// parse is tools/formats (alamo::particle::parse_system); this layer turns it into what the runtime uses:
//   * texture names without directories (64 EaW emitters store full authoring paths, B.2), upper-case;
//   * the lifetime shape recomputed from 0x0F / 0x13 (SPEC 3.2: the stored one is stale in 383 of 891 emitters);
//   * the 7 tracks resampled into 32-entry tables (SPEC 3.3), byte tracks scaled to 0..1;
//   * spawn interval / per-event count / pool size (B.4 "Pool size"), with the 5,003 cap and the link indices
//     (B.2 "Links": 0xFFFFFFFF = none -> -1).
// Render particles are purely visual (B.6): nothing here is read by the simulation.
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alamo {
class FileProvider;
}

namespace alamo::render::particles {

inline constexpr uint32_t kTrackSamples = 32;       // resampled table size (SPEC 3.3)
inline constexpr uint32_t kPoolCap = 5003;          // particles per emitter (B.4)
inline constexpr float kTickSeconds = 0.03f;        // render delta per sim tick (B.4 "Clock")
inline constexpr float kMaxUpdateSeconds = 1.0f;    // scene Update clamps d to <= 1 s
inline constexpr float kPrewarmStep = 0.1f;         // skip-time pre-simulation step
inline constexpr float kEmitterAgeWrap = 14400.0f;  // emitter age wraps (B.4 "Emitter time")

enum Track : uint32_t { kRed = 0, kGreen, kBlue, kAlpha, kScale, kTexture, kRotation, kTrackCount };

// Spawn shape (SPEC 3.2), flattened.
struct Shape {
    uint32_t kind = 0;  // 0 point, 1 box, 2 cube, 3 sphere, 4 cylinder, 5 (accepted; sampled as a point)
    float box_min[3] = {0, 0, 0}, box_max[3] = {0, 0, 0};
    float cube_half = 0;
    float sphere_radius = 0;
    bool sphere_surface = false;
    float cylinder_radius = 0;
    bool cylinder_surface = false;
    float cylinder_height = 0;
    float point[3] = {0, 0, 0};
    // Largest distance from the origin a sample can have (culling reach).
    float extent() const;
};

struct TrackTable {
    uint32_t interpolation = 0;  // header mini 0x04 (0/1/2; meaning [L], B.2 / C.2)
    std::array<float, kTrackSamples> v{};
};

struct EmitterDef {
    std::string name;           // 0x16
    std::string color_texture;  // 0x03, directory stripped, upper-case ("" = none)
    std::string bump_texture;   // 0x45, same
    uint32_t index = 0;         // file position (the 0x14 field equals it in shipped data)
    // Rendering
    uint32_t blend_mode = 0;    // 0x04 (SPEC 3.1 table)
    uint32_t geometry = 1;      // 0x05: 0 = 3 vertices, 1 = 4 vertices per particle
    uint32_t texture_cells = 1; // 0x10
    bool depth_sort = false;    // 0x2B
    bool heat = false;          // 0x3B: bucket 11
    bool has_tail = false;      // 0x41
    float tail_size = 0;        // 0x42
    bool enabled = true;        // 0x15 (the loader forces 1)
    // Emission
    bool use_bursts = false;    // 0x07
    uint32_t particles_per_second = 0;  // 0x2A
    float burst_delay = 0;      // 0x25
    uint32_t particles_per_burst = 0;   // 0x26
    uint32_t burst_count = 0;   // 0x27 (0 = unlimited)
    float initial_delay = 0;    // 0x24
    float stop_time = 0;        // 0x32
    float skip_time = 0;        // 0x33
    uint32_t spawn_source = 0;  // 0x34
    // Particle life + motion
    float lifetime = 1;         // 0x0F
    float random_lifetime = 0;  // 0x13
    bool object_space = false;  // 0x08
    float acceleration[3] = {0, 0, 0};  // 0x0A
    bool object_space_acceleration = false;  // 0x35
    float gravity = 0;          // 0x0C
    float outward_speed = 0;    // 0x09
    float outward_acceleration = 0;  // 0x0B
    bool inherit_velocity = false;   // 0x43
    float inherit_velocity_factor = 0;  // 0x28
    bool affected_by_wind = false;   // 0x31
    uint32_t ground_behavior = 0;    // 0x2F (not simulated yet, R4 open item)
    // Randomisation
    float random_color[4] = {0, 0, 0, 0};  // 0x2C
    bool random_color_uniform = false;     // 0x2D
    float random_initial_rotation = 0;     // 0x12
    float random_rotation_speed = 0;       // 0x17 (clamped 0..1)
    bool random_rotation_direction = false;// 0x23
    bool rotation_fixed_at_spawn = false;  // 0x48
    // Weather
    bool weather = false;            // 0x3D
    float weather_cube_size = 0;     // 0x3E
    float weather_fade_start = 0;    // 0x3F
    float weather_fade_length = 0;   // 0x40
    float weather_camera_distance = 0;  // 0x47
    // Shapes (lifetime recomputed: box [L*(1-r), L] on x)
    Shape velocity, life, position;
    std::array<TrackTable, kTrackCount> tracks{};
    // Links (B.2): emitter indices within the system, -1 = none.
    int32_t spawn_on_death = -1;     // mini 0x37
    int32_t spawn_during_life = -1;  // mini 0x39
    // ---- derived (B.4 "Spawning" / "Pool size")
    float interval = 0;        // seconds between spawn events (burst delay, or 1 / rate)
    uint32_t per_event = 1;    // particles per event (1 rate mode, 0x26 burst mode)
    uint32_t base_pool = 0;    // pool before link / mesh multiplication, capped
    // ---- derived from the system's links
    int32_t parent = -1;            // emitter whose particles drive this one (-1: the system's transform)
    bool parent_on_death = false;   // true: spawn-on-death child, false: spawn-during-life child
    uint32_t pool = 0;              // final pool (x parent pool for linked children), capped at kPoolCap
    // Draw rules (B.5).
    bool sorted() const;            // per-particle back-to-front: blend 0/2/5/11 and 0x2B
    // Upper bound of a particle's distance from its spawn point over its life (culling reach).
    float reach = 0;
};

struct SystemDef {
    std::string name;             // file name (upper-case, no extension)
    bool leave_particles = false; // 0x900/0x02
    std::vector<EmitterDef> emitters;
    bool has_heat() const;
    bool has_non_heat() const;
};
using SystemPtr = std::shared_ptr<const SystemDef>;

// Strips a directory ("C:\PROJECTS\...\P_PARTICLE_MASTER.TGA" -> "P_PARTICLE_MASTER.TGA") and upper-cases.
std::string texture_base_name(std::string_view name);

// Track sample at a life fraction t (0..1): linear between the 32 entries for modes 0/1, the entry below for 2.
float sample_track(const TrackTable& t, float life_fraction);

// B.4 pool size of one emitter before link multiplication: rate = 0x2A or 0x26 / 0x25; interval I = 0x25 or 1 / rate;
// pool = ceil(rate x lifetime) when I <= lifetime (weather: lifetime 1 s), else the per-event count; capped.
uint32_t emitter_pool(const EmitterDef& e);

// Derived fields of every emitter (index, lifetime shape, interval, per-event count, base pool, reach) and the links
// (parent, pool x parent pool, capped). load_system calls it; call it again after editing a system's emitters.
void finalize_system(SystemDef& s);

// Parses one particle ALO. False (with *error) for a malformed file or a model ALO.
bool load_system(std::span<const uint8_t> bytes, std::string_view name, SystemDef& out, std::string* error = nullptr);
// True if the bytes start with chunk 0x900.
bool is_particle_file(std::span<const uint8_t> bytes);

// ---- Proxy names (B.3): "_ALTn" / "_LODn" give the alt / LOD index, and the name is cut at the first of them.
struct ProxyName {
    std::string base;  // upper-case, cut at _ALT / _LOD (the file to load)
    int alt = -1;      // digits after _ALT, -1 if absent
    int lod = -1;      // digits after _LOD, -1 if absent
};
ProxyName parse_proxy_name(std::string_view name);

// Case-insensitive cache of systems by name ("P_SMOKE" / "p_smoke.alo"): one load per name, misses cached (134 shipped
// proxies name nothing, B.3). Reads Data\Art\Models\<NAME>.ALO through the FileProvider. Thread-safe.
class ParticleLibrary {
public:
    explicit ParticleLibrary(const FileProvider* files = nullptr) : files_(files) {}
    // Null when the name is missing, not a particle file or malformed.
    SystemPtr get(std::string_view name);
    // In-memory system (tests, synthetic worlds); replaces a cached entry of that name.
    void add(SystemPtr system);
    struct Stats {
        uint32_t loaded = 0, missing = 0, not_particle = 0, malformed = 0;
    };
    Stats stats() const;

private:
    const FileProvider* files_;
    mutable std::mutex mutex_;
    std::map<std::string, SystemPtr> cache_;  // upper-case name -> system (null = miss)
    Stats stats_;
};

}  // namespace alamo::render::particles

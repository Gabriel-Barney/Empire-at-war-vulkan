// alamo_render_particles - CPU side of the render particle systems (R4): emitter bookkeeping with the counts and timing
// of the original (docs/re/ANIM_PARTICLES.md B.4, B.7 first bullet), positions on the GPU.
//
// What the CPU keeps per emitter: age, the interval spawn events with back-filling (each pre-aged by now - event time,
// spawned with the emitter transform interpolated to its sub-step, events older than the lifetime skipped), the pool
// (B.4 size, 5,003 cap) with its free-slot stack (spawns dropped when it is empty), the Particle Detail slot spread,
// burst limits, pre-warm (skip time), stop time, and per slot the age / lifetime so deaths (and spawn-on-death links)
// happen at the same update as on the GPU (ages advance by the same float additions on both sides).
// What the GPU does (particle_gpu.hpp): initial position / velocity / colour / rotation from the spawn records, forces,
// integration, tracks, sorting, expansion. The CPU queues one UpdatePacket per update for it.
//
// Clock (B.4): update(d) is one scene Update; d = 0.03 s per sim tick of the loop iteration (0 otherwise), clamped
// to 1 s. update(0) does nothing (particles freeze while paused). Visibility never stops updating (B.3).
// Everything here is render-side and uses its own free RNG: no sim state is read or written (B.6).
#pragma once
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <vector>

#include "alamo/assets/math.hpp"
#include "alamo/render/particles/particle_data.hpp"

namespace alamo::render::particles {

using assets::Aabb;
using assets::Float3;
using assets::Mat3x4;

// Render-side free RNG (any RNG is allowed for render particles, B.6): 32-bit LCG.
class FreeRng {
public:
    explicit FreeRng(uint32_t seed = 0x2545F491u) : s_(seed) {}
    uint32_t next() {
        s_ = s_ * 1664525u + 1013904223u;
        return s_;
    }
    float uniform() { return static_cast<float>(next() >> 8) * (1.0f / 16777216.0f); }  // [0, 1)

private:
    uint32_t s_;
};

// One particle to initialise on the GPU. `slot` is local to the emitter's pool.
struct SpawnRecord {
    uint32_t slot = 0;
    uint32_t emitter = 0;      // index into UpdatePacket::emitters
    float pre_age = 0;         // seconds already lived at the end of the update
    float life = 0;            // seconds
    float lerp = 1;            // emitter transform: 0 = prev, 1 = current
    uint32_t seed = 0;         // GPU random stream
    int32_t parent_slot = -1;  // linked child: slot in the parent's pool whose particle drives the spawn
    uint32_t flags = 0;        // kSpawnFromDeath: parent position from the death buffer (spawn-on-death child)
};
static_assert(sizeof(SpawnRecord) == 32, "SpawnRecord is a 32-byte GPU record");
inline constexpr uint32_t kSpawnFromDeath = 1u;

// Per emitter of one update.
struct PacketEmitter {
    const EmitterDef* def = nullptr;
    uint32_t base = 0;          // first particle in the GPU buffer
    uint32_t pool = 0;
    int32_t parent_base = -1;   // linked child: the parent's base
    Mat3x4 prev, cur;           // system transform at the start / end of the update
    Float3 velocity;            // (cur - prev) / d of the system position
    float d = 0;                // this system's step (UpdatePacket::d, or the throttle's accumulated d)
};

struct ClearRange {
    uint32_t base = 0, count = 0;
};

struct UpdatePacket {
    float d = 0;
    std::vector<ClearRange> clears;      // newly allocated ranges: reset before the update
    std::vector<PacketEmitter> emitters;
    std::vector<SpawnRecord> spawns;
};

// Range allocator over the GPU particle buffer (first fit, coalescing frees).
class RangeAllocator {
public:
    void init(uint32_t capacity);
    // UINT32_MAX when no range fits.
    uint32_t allocate(uint32_t count);
    void free(uint32_t base, uint32_t count);
    uint32_t capacity() const { return capacity_; }
    uint32_t used() const { return used_; }

private:
    struct Range {
        uint32_t base, count;
    };
    std::vector<Range> free_;  // ascending base
    uint32_t capacity_ = 0, used_ = 0;
};

// Particle Detail slot spread (B.4): active = ceil(clamp(scale2, 0, 1) x detail x pool); slot i is active iff
// ((0xEDF1 + i * 0xECF3) mod pool) < active.
uint32_t detail_active_count(uint32_t pool, float detail, float scale2);
bool detail_slot_active(uint32_t slot, uint32_t pool, uint32_t active);

struct EmitterState {
    const EmitterDef* def = nullptr;
    uint32_t pool = 0, active = 0;  // pool size, Particle Detail active slots
    uint32_t base = UINT32_MAX;     // GPU range (UINT32_MAX: none, nothing spawns)
    std::vector<float> age, life;   // per slot; life < 0 = free
    std::vector<uint32_t> free;     // stack of free active slots (back = next)
    std::vector<uint32_t> pending_free;  // slots freed this update (on-death parents: reused from the next update)
    uint32_t live = 0;
    // Spawn schedules: one for a root emitter, one per parent slot for a linked child (B.2).
    struct Schedule {
        float age = 0;
        uint64_t events = 0;
        bool running = false;
        bool stopped = false;
        bool from_death = false;
    };
    std::vector<Schedule> schedules;
    bool done = false;
    // Culling: emitter world positions of the last `lifetime` seconds (sampled every 0.1 s).
    struct Sample {
        float time;
        Float3 pos;
    };
    std::deque<Sample> trail;
    uint64_t spawned = 0, dropped = 0, died = 0;  // statistics
};

struct SystemState {
    SystemPtr def;
    Mat3x4 cur, prev;
    Float3 velocity;
    bool has_transform = false;
    bool visible = true;    // drawing gate (hidden proxies, B.3); updating continues
    float fade = 1.0f;      // parent model fade (submit skipped at <= 0.01)
    bool water = false;     // system flag +0x338: bucket 7 instead of 8
    bool released = false;  // detached: no new spawns (leave particles) - removed once empty
    bool alive = false;
    float emission_scale = 1.0f;  // current emission scale (setter index 1, "scalar 0" of B.4: the spawn test)
    float detail_scale = 1.0f;    // current detail scale (setter index 0, "scalar 2" of B.4: Particle Detail slots)
    // Scalar setter state (STRATEGIC_GAPS §6.2, 0x173220): critically damped springs toward the targets, smoothed at the
    // start of every update (B.4 order); both targets default to 1.
    struct Spring {
        float target = 1.0f, velocity = 0.0f, rate = 100.0f;  // rate = 1 / smoothing seconds
    };
    Spring detail_spring, emission_spring;
    Spring color_spring[4];
    float color[4] = {1, 1, 1, 1};  // setter index 4: RGBA multiplier of every emitter's particles
    float scalar2[2] = {0, 0};      // setter index 2 (+0x2BC / +0x2C0, set immediately; no reader traced)
    // Invisible-system throttle (B.4 / STRATEGIC_GAPS §6.2): updated before (+0x31E), drawn since the last update
    // (+0xC9, ParticleWorld::mark_drawn), the gathered d (+0xCC).
    bool updated = false;
    bool drawn = false;
    float throttle_acc = 0.0f;
    float time = 0;               // system clock (culling trail)
    uint32_t generation = 0;
    std::vector<EmitterState> emitters;
};

struct WorldStats {
    uint32_t systems = 0, emitters = 0, live_particles = 0;
    uint64_t spawned = 0, dropped = 0, died = 0;
    uint32_t pool_exhausted = 0;  // emitters without a GPU range (buffer full)
    uint32_t updates = 0;
    uint64_t system_steps = 0, throttled = 0;  // system updates run / skipped by the invisible-system throttle
};

// Indices of the system scalar setter (STRATEGIC_GAPS §6.2, 0x173220). ANIM_PARTICLES B.4 calls the detail scale
// "scalar 2" and the emission scale "scalar 0" [M for that numbering].
enum class SystemScalar : uint32_t { Detail = 0, Emission = 1, Immediate = 2, Color = 4 };
inline constexpr float kThrottleSeconds = 0.1f;  // B.4: an undrawn system updates once >= 0.1 s has gathered

class ParticleWorld {
public:
    using Handle = uint64_t;  // (generation << 32) | index; 0 = none
    explicit ParticleWorld(uint32_t gpu_capacity = 1u << 19, uint32_t seed = 0x12345678u);

    Handle create(SystemPtr def, const Mat3x4& transform);
    bool valid(Handle h) const;
    // Moves the system. teleport: no velocity / interpolation from the old transform.
    void set_transform(Handle h, const Mat3x4& t, bool teleport = false);
    void set_visible(Handle h, bool visible);
    void set_fade(Handle h, float fade);
    void set_water(Handle h, bool water);
    // Both scales snapped (no smoothing): set_scalar(Emission / Detail, value, 0).
    void set_scales(Handle h, float emission, float detail);
    // The scalar setter (0x173220): `value` = 1 float (Detail, Emission), 2 (Immediate) or 4 (Color RGBA); smoothing
    // seconds s: the spring rate is 1 / s, s <= 0 -> 0.01 and the value snaps to the target. Detail re-evaluates the
    // emitters' active slots whenever their count changes. Callers (STRATEGIC_GAPS §6.2): planet corruption, weather
    // intensity, SurfaceFX (colour fade + intensity, s = 0.1), Lure (colour fade), TransportLanding, a model API.
    void set_scalar(Handle h, SystemScalar index, std::span<const float> value, float smoothing_seconds = 0.0f);
    // Invisible-system throttle (global 0x140a12648, statically 1 in the exe: on). On: a system updated before that was
    // not drawn since its last update gathers d and steps only once >= kThrottleSeconds has gathered, with that d.
    void set_throttle(bool on) { throttle_ = on; }
    bool throttle() const { return throttle_; }
    // The renderer drew system `index` (for_each index) this frame (+0xC9).
    void mark_drawn(uint32_t index) {
        if (index < systems_.size()) systems_[index].drawn = true;
    }
    // Detach (owner gone / proxy dropped): with leave particles (0x900/02) the system stops spawning and is removed when
    // its particles are dead; otherwise it is removed now.
    void release(Handle h);
    // Particle Detail option (0..1, B.4); re-evaluates every emitter's active slots.
    void set_detail(float detail);
    float detail() const { return detail_; }

    // One scene update of d seconds (clamped to kMaxUpdateSeconds; d <= 0 does nothing).
    void update(float d);
    // Seconds for a loop iteration with `ticks` sim ticks: 0.03 per tick (B.4), clamped.
    static float tick_delta(uint32_t ticks);

    // GPU consumer: packets queued since the last take (only while record_packets is on).
    void set_record_packets(bool on) { record_ = on; }
    std::vector<UpdatePacket> take_packets();

    const SystemState* system(Handle h) const;
    // Live systems in creation order (index order of the slot table).
    template <class F>
    void for_each(F&& f) const {
        for (size_t i = 0; i < systems_.size(); ++i)
            if (systems_[i].alive) f(static_cast<uint32_t>(i), systems_[i]);
    }
    // World AABB of a system (B.5 culling): per emitter the positions of its trail (live particles' spawn points) padded
    // by the emitter's reach; empty emitters a unit box at the system position.
    Aabb bounds(const SystemState& s) const;
    WorldStats stats() const;
    uint32_t gpu_capacity() const { return ranges_.capacity(); }

private:
    void init_emitters(SystemState& s);
    void reset_detail(EmitterState& e, float scale2);
    void free_system(SystemState& s);
    void step_system(SystemState& s, float d, UpdatePacket* pkt);
    bool take_slot(EmitterState& e, uint32_t& slot);
    std::vector<SystemState> systems_;
    std::vector<uint32_t> free_systems_;
    RangeAllocator ranges_;
    FreeRng rng_;
    float detail_ = 1.0f;
    bool throttle_ = true;
    void smooth_scalars(SystemState& s, float d);
    bool record_ = false;
    std::vector<UpdatePacket> packets_;
    std::vector<ClearRange> pending_clears_;
    WorldStats totals_;
};

}  // namespace alamo::render::particles

// alamo_render - R1 sim driver for the renderer (alamo_render --sim, the R1 tests). A tactical map run through
// alamo_sim's public API exactly as sim::run_map does it (session start, mode by map kind, spawn_map, the
// AiStandIn controller on every player slot, RandomOrders between ticks: once before the first tick and once
// after every tick), but one tick at a time so the caller can snapshot / render between ticks. With the same
// seed the CRC log equals run_map's.
#pragma once
#include <cstdint>
#include <memory>
#include <string>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/host_fs.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/sim/map_spawn.hpp"
#include "alamo/sim/scenario.hpp"
#include "alamo/sim/sim_host.hpp"
#include "alamo/terrain/ted.hpp"
#ifdef ALAMO_SIM_HAVE_ANIM
#include "alamo/sim/sim_anim.hpp"
#endif

namespace alamo::anim {
class AnimWorld;
}

namespace alamo::render {

struct SimSessionSpec {
    uint32_t seed = 1;
    bool ai = true;           // AiStandIn on every player slot
    bool live_orders = true;  // RandomOrders between ticks
    // R3 (sim-owned animation): with `anim_files` (ALO / ALA source, must outlive the session) and a build whose
    // alamo_sim has engine/anim (ALAMO_SIM_HAVE_ANIM), the session owns a sim::SimAnimation attached to its host
    // ("T5.anim": model creation draws on the SYNCED RNG, so the "rng" CRC differs from a session without it);
    // `anim_crc` also registers the opt-in "anim" CRC system. Null = no sim animation (render may fall back to the
    // snapshot-fed SimAnimDriver).
    const FileProvider* anim_files = nullptr;
    bool anim_crc = false;
};

class SimSession {
public:
    SimSession(const objects::GameObjectTypes& types, jobs::JobSystem& js, const terrain::MapData& map,
               const sim::TypeCrcIndex& index, const SimSessionSpec& spec = {});
    ~SimSession();
    SimSession(const SimSession&) = delete;
    SimSession& operator=(const SimSession&) = delete;

    // R3: the sim-owned animation (null without SimSessionSpec::anim_files / engine/anim) and the AnimWorld of the
    // top tactical mode (null when none or before the first tick created it). Const reads between ticks only.
    bool has_animation() const;
    const anim::AnimWorld* anim_world() const;
#ifdef ALAMO_SIM_HAVE_ANIM
    const sim::SimAnimation* animation() const { return anim_.get(); }
#endif

    // Headless step: one tick, then the between-tick orders. False when the host did not tick.
    bool tick();
    // Real-time step (FrameSynchronizer pacing at wall time now_ms): at most one tick; true if it ticked.
    bool loop_iteration(uint32_t now_ms);
    sim::SimHost& host() { return host_; }
    const sim::SimHost& host() const { return host_; }
    const sim::SpawnReport& spawn() const { return spawn_; }
    uint64_t ticks() const { return ticks_; }
    uint64_t orders_live() const { return orders_; }

private:
    void between();
#ifdef ALAMO_SIM_HAVE_ANIM
    std::unique_ptr<sim::SimAnimation> anim_;  // declared before host_: outlives the host's hooks / CRC hashers
#endif
    sim::SimHost host_;
    sim::SpawnReport spawn_;
    sim::RandomOrders orders_gen_;
    bool live_ = true;
    uint64_t ticks_ = 0, orders_ = 0;
};

// A data-free space world for tests and `alamo_render --sim-headless-check` without game data: object types
// written to `dir` (Data\XML, model name kSyntheticModel on every drawable type) and the two-fleet map of
// engine/apps/sim's tests (stations, frigates, corvettes, neutral asteroids, one unknown type).
inline constexpr const char* kSyntheticModel = "SIM_DEBUG_BOX.ALO";
struct SyntheticSimWorld {
    std::unique_ptr<DirectoryProvider> files;
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types;
    terrain::MapData map;
    std::unique_ptr<sim::TypeCrcIndex> index;
};
// Null on failure (with `error`).
std::unique_ptr<SyntheticSimWorld> make_synthetic_sim_world(const std::string& dir, std::string* error = nullptr);

}  // namespace alamo::render

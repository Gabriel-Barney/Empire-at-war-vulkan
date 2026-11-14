// alamo_render_galactic - R2 galactic campaign driver for the renderer (alamo_render --galactic, the R2 tests):
// SimHost::start_galactic on a campaign (synthetic galaxy or a real FoC campaign), one tick at a time so the caller
// can snapshot / render between ticks, plus GalacticOrders, a deterministic stand-in for the players' galactic
// orders so fleets fly and planets change hands on screen. It is a renderer test stand-in; the game's galactic AI
// (engine/ai) drives real sessions, so this is not a gap waiting for an AI.
//
// GalacticOrders [port] (R1's RandomOrders counterpart; an input source, not sim code): before the first tick and
// after every tick whose global frame is a multiple of 90 (kOrderInterval), for every non-neutral player in player
// order and its idle fleets in fleet order: a fleet orbiting a planet its owner does not hold lands its ground
// units (Invade event, empty unit list = all); otherwise, with probability 3/4, it is sent to a random graph
// neighbour of its planet (MoveObjectToObject). Orders go through SimHost::submit (scheduled, recorded in the
// "commands" CRC); randomness is a private LCG seeded from the session seed, never the synced RNG. At start every
// player's ground units on a planet where it has a fleet in orbit board that fleet (GalacticSim::launch_units,
// the AI/Lua direct path), so invasions can happen. The session is the only writer of the sim; the renderer only
// reads snapshots.
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/data/game_database.hpp"
#include "alamo/galactic/campaign.hpp"
#include "alamo/galactic/galactic_rules.hpp"
#include "alamo/jobs/job_system.hpp"
#include "alamo/objects/game_object_types.hpp"
#include "alamo/objects/object_types.hpp"
#include "alamo/render/galactic/galactic_snapshot.hpp"
#include "alamo/render/galactic/galactic_view.hpp"
#include "alamo/sim/sim_host.hpp"

namespace alamo::render {

struct GalacticSessionSpec {
    uint32_t seed = 1;
};

class GalacticSession {
public:
    GalacticSession(const objects::GameObjectTypes& types, jobs::JobSystem& js, const galactic::GalacticRules& rules,
                    const galactic::GalacticSetup& setup, const GalacticSessionSpec& spec = {},
                    std::vector<std::string>* diagnostics = nullptr);
    GalacticSession(const GalacticSession&) = delete;
    GalacticSession& operator=(const GalacticSession&) = delete;

    bool ok() const { return sim_ != nullptr; }
    // Headless step: one tick, then the between-tick orders. False when the host did not tick.
    bool tick();
    // Real-time step (FrameSynchronizer pacing at wall time now_ms): at most one tick; true if it ticked.
    bool loop_iteration(uint32_t now_ms);
    sim::SimHost& host() { return host_; }
    const sim::SimHost& host() const { return host_; }
    const galactic::GalacticSim* sim() const { return sim_; }
    uint64_t ticks() const { return ticks_; }
    uint64_t orders() const { return orders_; }
    uint64_t boarded() const { return boarded_; }
    // Galactic state digest (GalacticSim::hash_state), for the on/off comparisons.
    uint64_t state_hash() const;

private:
    void between();
    uint32_t next();
    sim::SimHost host_;
    galactic::GalacticSim* sim_ = nullptr;
    uint32_t lcg_ = 1;
    uint64_t ticks_ = 0, orders_ = 0, boarded_ = 0;
};

// Everything a galactic render session needs besides the GPU: object types, galactic rules, the campaign setup,
// faction colours and the trade-route line style. `files` is the game's file provider (real data) or the folder
// of the synthetic XML.
struct GalacticWorld {
    std::string name;
    bool synthetic = false;
    std::unique_ptr<FileProvider> files;
    std::unique_ptr<data::GameDatabase> gdb;
    objects::ObjectTypeDatabase db;
    objects::GameObjectTypes types;
    galactic::GalacticRules rules;
    galactic::GalacticSetup setup;
    std::vector<FactionColor> faction_colors;
    RouteLineStyle route_style;
    CorruptionPathSettings corruption_path;  // GameConstants (FIXPASS_GALACTIC 3.2)
    std::vector<std::string> diagnostics;
};

// A data-free 12-planet galaxy (Empire / Rebel / Underworld / Neutral, 17 trade routes, a background galaxy disc):
// object types written to `dir` (Data\XML) with primitive model names (galactic_view.hpp), rules built by hand.
std::unique_ptr<GalacticWorld> make_synthetic_galactic_world(const std::string& dir, std::string* error = nullptr);
// A campaign from game data (Data\XML through `files`): GameDatabase, object types, load_galactic_rules,
// build_campaign_setup. Empty campaign = Sandbox_Heart_Of_The_Maw_Underworld.
std::unique_ptr<GalacticWorld> load_galactic_world(std::unique_ptr<FileProvider> files, const std::string& campaign,
                                                   std::string* error = nullptr);
inline constexpr const char* kDefaultGalacticCampaign = "Sandbox_Heart_Of_The_Maw_Underworld";

// Viewpoint (route colours) and neutral-player flags of a running sim: the first human player (else 0). Route sides
// (FIXPASS_GALACTIC 3.1) by faction NAME: GameConstants Good_Side_Name (GalacticRules constants) -> kRouteSideGood,
// `evil_side_name` (default "": GameConstants Evil_Side_Name from the GalacticRules constants; modc t20: no built-in
// name) -> kRouteSideEvil, any other non-neutral faction -> kRouteSideOther (Corrupt).
GalacticViewOptions galactic_view_options(const galactic::GalacticSim& sim, std::string_view evil_side_name = {});

}  // namespace alamo::render

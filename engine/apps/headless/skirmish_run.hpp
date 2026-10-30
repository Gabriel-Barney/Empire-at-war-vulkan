// alamo_headless --skirmish-ai synthetic|foc|foc-land (A1 stage 2): an AI-vs-AI space skirmish fought by the tactical AI
// (engine/ai/tactical tactical_ai.hpp) inside an S10 TacticalBattle (engine/apps/sim battle_ai.hpp).
//   synthetic: game data, AI data (goals, a template, two AIPlayerTypes), three SpaceMode plans and a free-store
//              script are written to a temporary folder; no game files needed;
//   foc:       the FoC data (--game DIR or ALAMO_FOC_DATA; exit 0 "skipped" without it): shipped AI data, plans,
//              BusyTacticalFreeStore, DifficultyAdjustments, a space map (--map, default _MP_SPACE_KUAT) and an
//              Empire vs Rebel fleet.
//   foc-land:  (battle host) the same FoC data, a land map (--map, default _Land_Planet_Utapau_01, Utapau's campaign
//              map): Empire companies hold the planet, Rebel companies lift off from the next planet, fly in and invade
//              (S10 land conflict, fought AI vs AI). Players: Empire, Rebel, Neutral, Hostile (map owners resolve by
//              faction, TacticalBattle::bt_map_owner).
// Both: a 3-planet galaxy whose attacker fleet moves into the defender's orbit (S10 conflict, fought AI vs AI),
// every playable side a tactical AI at Normal; per-tick CRC log (--crc-out / --golden), --check-workers
// compares 0/1/4/N workers. Prints orders issued / rejected, the battle result and the Lua error count.
#pragma once
#include <string>

#include "map_run.hpp"

// `ai_log`: write every battle's AI decision log + Lua errors (first run) to this file ("" = none).
int skirmish_ai_run(const MapRunOptions& opt, const std::string& kind, const std::string& ai_log = {});

// --- N1 stage 2 (owner: n2-host): the same battle as a save session (save_run.cpp, --save-session skirmish) ---
// skirmish_save_load loads the data of `kind` (synthetic | foc | foc-land, as above; map "" = the default) once;
// skirmish_save_start sets up a fresh host (SimConfig::players 4) exactly as skirmish_ai_run's run (galaxy,
// CampaignBattles fighting AI vs AI with the tactical AI, the attacker's opening moves) and returns the battles glue;
// `after_tick` is the land invasion step skirmish_ai_run runs after every tick (its "invaded" flag saves with the session
// as the participant "skirmish"). `anim`: also the sim-owned animation (SimAnimation, attached with CRC). Without the
// tactical AI in the build: null / false with an error.
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>
namespace alamo {
class FileProvider;
namespace objects {
class GameObjectTypes;
}
namespace sim {
class SimHost;
class CampaignBattles;
}  // namespace sim
}  // namespace alamo
struct SkirmishSaveData;
struct SkirmishSaveRun {  // shared_ptr: complete types only where they are created (skirmish_run.cpp)
    std::shared_ptr<void> anim;                           // SimAnimation; before the battles glue (it points at it)
    std::shared_ptr<alamo::sim::CampaignBattles> battles;
    std::function<void()> after_tick;
};
std::shared_ptr<SkirmishSaveData> skirmish_save_load(const std::string& kind, const std::string& game_dir,
                                                     const std::vector<std::string>& game_args, const std::string& map,
                                                     bool& skipped, std::string& err);
const alamo::objects::GameObjectTypes& skirmish_save_types(const SkirmishSaveData& d);
uint64_t skirmish_save_fingerprint(const SkirmishSaveData& d);
std::string skirmish_save_map(const SkirmishSaveData& d);
bool skirmish_save_start(const SkirmishSaveData& d, alamo::sim::SimHost& host, bool anim, SkirmishSaveRun& run, std::string& err);

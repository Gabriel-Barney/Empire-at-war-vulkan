// alamo_render - deterministic fleet stress scene (C5 measurements).
//
// Model choice comes from the game XML (engine/data GameDatabase), not from model names:
//   * only <SpaceUnit> and <UniqueUnit> objects (StarBase, SpaceStructure, SpaceProp, Props_*,
//     SpaceBuildable, SecondaryStructure, Marker, TransportUnit, CIN_* / MOV_* cinematic types
//     and every other object type are out), first definition of a name only, no *_Death_Clone;
//   * a non-empty <Space_Model_Name>;
//   * class from the <CategoryMask> tokens ('|' / ',' / whitespace separated, case-insensitive):
//     any of Structure, Super, Transport, Upgrade, Wall, Infantry, Vehicle -> excluded;
//     Capital or Frigate -> capital; Corvette -> corvette; Fighter or Bomber -> fighter;
//     anything else (no mask) -> excluded;
//   * one entry per model file (upper-cased), first object in database order wins.
// Each candidate model is then loaded on the CPU in parallel and kept if it has submeshes and
// valid bounds. A seeded shuffle per class picks `capitals` / `corvettes` / `fighters` types.
// Track-B coverage: with an EffectCache, a model "has a translated pass" when one of its render
// submeshes (not ALO-hidden) resolves to an effect with a ProgramStatus::Translated draw pass
// (e.g. MeshShield). If no picked type has one, the same seeded RNG picks one such candidate
// (database order, classes with a non-zero quota) and it replaces the last pick of its class.
// Layout: instance i is a capital when i % 10 == 0, a corvette when i % 10 is 1 or 2, else a
// fighter (an empty class falls back to the next one); positions uniform in a slab of half-size
// L = 150 sqrt(N) (z range L/8), random yaw; team = quadrant of the position, with a per-team
// colour (Colorization). Same install + seed -> same scene on every machine and thread count.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "alamo/assets/math.hpp"
#include "alamo/materials/constants.hpp"

namespace alamo {
class FileProvider;
}
namespace alamo::vfs {
class Vfs;
}
namespace alamo::assets {
class AssetCache;
}
namespace alamo::jobs {
class JobSystem;
}
namespace alamo::data {
class GameDatabase;
}

namespace alamo::render {

class EffectCache;

enum class FleetClass : uint8_t { Capital = 0, Corvette = 1, Fighter = 2 };
const char* fleet_class_name(FleetClass c);

struct FleetShipType {
    std::string type_name;  // XML object name, e.g. "Calamari_Cruiser"
    std::string model;      // "Data\\Art\\Models\\" + upper-cased <Space_Model_Name>
    FleetClass cls = FleetClass::Fighter;
};
// Classification rules above, in database order (deterministic for a given install).
std::vector<FleetShipType> fleet_ship_types(const data::GameDatabase& db);

struct FleetModels {
    std::vector<std::string> capitals, corvettes, fighters;                 // picked model paths
    std::vector<std::string> capital_names, corvette_names, fighter_names;  // their XML type names
    uint32_t candidates = 0;    // distinct ship models classified from the XML
    uint32_t available[3] = {};  // loadable models per FleetClass before the pick
    uint32_t translated_candidates = 0;  // loadable models with a translated pass (0 without an EffectCache)
    uint32_t translated_picked = 0;      // picked types with a translated pass
    bool translated_forced = false;      // the coverage rule replaced a pick
};
// Loads the game database and an EffectCache through `files` itself (always applies the coverage rule).
FleetModels pick_fleet_models(const vfs::Vfs& files, assets::AssetCache& cache, jobs::JobSystem& js, uint32_t seed,
                              uint32_t capitals = 6, uint32_t fighters = 10, uint32_t corvettes = 4);
// effects == nullptr: no translated-pass coverage rule.
FleetModels pick_fleet_models(const data::GameDatabase& db, assets::AssetCache& cache, jobs::JobSystem& js, uint32_t seed,
                              uint32_t capitals = 6, uint32_t fighters = 10, uint32_t corvettes = 4,
                              EffectCache* effects = nullptr);

struct FleetInstance {
    uint32_t model = 0;  // index into capitals, then corvettes, then fighters
    assets::Mat3x4 transform;
    materials::float4 team{1, 1, 1, 1};
};
std::vector<FleetInstance> make_fleet(uint32_t count, uint32_t seed, uint32_t capital_types, uint32_t corvette_types,
                                      uint32_t fighter_types);
// No corvettes: model indices are capitals, then fighters.
std::vector<FleetInstance> make_fleet(uint32_t count, uint32_t seed, uint32_t capital_types, uint32_t fighter_types);
float fleet_radius(uint32_t count);  // bounding radius of the layout (for the camera)

// Frame ring sizing: Renderer records secondaries with frame.secondary(JobSystem::current_slot()),
// so FrameRingDesc::recording_threads must be >= JobSystem::thread_count(). Returns the requested
// value clamped up to `job_threads` (and at least 1).
uint32_t ring_recording_threads(uint32_t requested, uint32_t job_threads);

}  // namespace alamo::render

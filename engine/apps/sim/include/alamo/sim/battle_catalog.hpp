// S10 battle setup: game data -> the tables MovementSystem (S7), CombatSystem (S8) and AbilitySystem (S9) read
// for one tactical battle. Loaded once per session (BattleCatalog), then the tables are built per battle for
// exactly the types taking part (dense indices in first-use order, so a battle's tables never depend on other
// battles).
//
// Sources: GameConstants (Damage_To_Armor_Mod, health multipliers, ability / engine constants),
// TargetingPrioritySetFiles.xml (priority sets), HardPointDataFiles.xml (<HardPoint> records; last definition
// of a name wins, Variant_Of_Existing_Type copies the parent's tags first), projectile types and unit fields
// from the typed object view.
// Integration (S10-int, 2026-10-27): build_battle() adds the S9 ability data (Unit_Abilities_Data /
// Abilities sub-object lists through the type loader's variant rules, parsed by parse_unit_ability /
// parse_special_ability, linked with link_combat_abilities), the S9 unit / projectile / hardpoint fields
// (death clones, death projectiles, hit-particle list sizes, energy, ion stun, smoke, fire bones, hardpoint
// death objects) and the visual object types the host spawns as PARTICLE objects (R4).
// PROVISIONAL [port] until models are loaded by the sim: hardpoint attachment points sit at the unit centre
// (fire bones are resolved against the animated pose only when the host runs animation), combat radius =
// movement footprint radius.
#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/tactical/abilities/ability_data.hpp"
#include "alamo/tactical/abilities/ability_system.hpp"
#include "alamo/tactical/combat/combat_system.hpp"
#ifdef ALAMO_SIM_HAVE_FOG
#include "alamo/tactical/fog/fog_of_war.hpp"  // fog of war constants (owner: fog agent)
#endif

namespace alamo::data {
struct GameConstants;
class XmlNode;
}  // namespace alamo::data
namespace alamo::objects {
class GameObjectTypes;
}
namespace alamo::tactical::movement {
struct AvoidModel;
}

namespace alamo::sim {

// Per-battle tables (S10-int). Index spaces: units / abilities.objects = the build order of the unit types,
// projectiles / hardpoints = CombatData's.
struct BattleTables {
    tactical::combat::CombatData combat;
    tactical::abilities::AbilityData abilities;  // objects[i] describes the same type as combat.units[i]
    struct UnitVisuals {
        std::vector<int32_t> hit, shield_hit;  // Damage_Hit_Particles / Shield_Hit_Particles (TypeIndex, -1 unknown)
        std::vector<int32_t> death_explosions; // Death_Explosions
        std::string smoke;                     // Damaged_Smoke_Asset_Name (render attachment, not an object)
        int32_t particle_lifetime = 0;         // not used for units
    };
    std::vector<UnitVisuals> units;
    struct ProjectileVisuals {
        int32_t detonation = -1, armor_reduced = -1, absorbed = -1, lifetime = -1;  // Projectile_*_Particle types
    };
    std::vector<ProjectileVisuals> projectiles;
    std::vector<std::array<std::string, 2>> fire_bones;  // per hardpoint: Fire_Bone_A / Fire_Bone_B ("" none)
    std::vector<std::string> diagnostics;                // ability XML parse notes
};

class BattleCatalog {
public:
    // `types` must outlive the catalog. `files` may be null (no hardpoints / priority sets).
    BattleCatalog(const objects::GameObjectTypes& types, const FileProvider* files, const data::GameConstants* constants,
                  std::vector<std::string>* diagnostics = nullptr);

    const objects::GameObjectTypes& types() const { return *types_; }
    const FileProvider* files() const { return files_; }  // refixf t04: model / clip reads (null: none)
    const tactical::combat::DamageArmorTable& damage() const { return damage_; }
    const std::vector<tactical::combat::PrioritySet>& priority_sets() const { return priority_sets_; }
    size_t hardpoint_records() const { return hardpoints_.size(); }
    float space_health_multiplier() const { return space_health_mult_; }
    // GameConstants SpaceIdleMovementSpeed (STATION_HANGARS_SOUND §3: engine loops moving at >= 1.1 x this speed).
    float space_idle_movement_speed() const { return space_idle_speed_; }
    float land_health_multiplier() const { return land_health_mult_; }
    // GameConstants Land_ / Space_Capture_Allowed_Countdown_Seconds, Garrisoned_Max_Attack_Distance_Multiplier
    // (REMAINING_GAPS §3.3); < 0 = not read (the battle keeps its config value).
    float capture_countdown(bool space) const { return space ? space_capture_countdown_ : land_capture_countdown_; }
    float garrisoned_range_multiplier() const { return garrisoned_range_mult_; }
    bool use_reinforcement_points() const { return use_reinforcement_points_; }  // Use_Reinforcement_Points (GC +0x118a0; land drops)
    float threat_decay_step() const { return threat_decay_step_; }  // AI_SpaceThreatDecayStep (DAMAGE_TRACKING)
    // S10-int constants.
    float engines_disabled_speed_modifier() const { return engines_disabled_speed_; }  // Engines_Disabled_Speed_Modifier
    tactical::abilities::AbilityConfig ability_config(bool space, bool skirmish) const;
    // OI tactical: CombatConfig defaults from GameConstants (tc::apply_game_constants: low-health threshold, melee cutoff,
    // opportunity-fire cutoff, diminishing-firepower curve); the battle sets mode / health multipliers on top.
    const tactical::combat::CombatConfig& combat_config() const { return combat_config_; }
    // --- battle host (BT): GameConstants of the battle setup / transitions (TACTICAL_GAPS §4.2, §4.9) ---
    struct TransitionConstants {
        // Retreat attrition (RetreatCoordinator vf13): space = Space_Retreat_Attrition_Factor; land =
        // Land_Retreat_Attrition_Factor + Blockade_Run_Attrition_Factor (blockade term only past an enemy fleet).
        float space_retreat_attrition = 0.0f, land_retreat_attrition = 0.0f, blockade_run_attrition = 0.0f;
        // Land lead unit 0x29c0c0 rule (b): Preferred_Pathfinder_Types in order (empty in FoC / TR).
        std::vector<std::string> preferred_pathfinder_types;
        // Raid force test 0x432e20 (Raid_Force_*; TR leaves them empty: never a raid).
        std::string raid_required_faction;
        uint64_t raid_free_mask = 0, raid_limited_mask = 0;
        int32_t raid_max_limited = 0, raid_max_heroes = 0;
    };
    const TransitionConstants& transition_constants() const { return transition_; }
    // Model data (battle host, item 5): read from the type's ALO through the catalog's FileProvider when the sim is built
    // with engine/assets (ALAMO_SIM_HAVE_ASSETS); null / false otherwise or when the model does not load (headless runs
    // without art data keep the data-only fallbacks). Bind pose, model space (x forward, z up), before Scale_Factor.
    // Cached per (type, mode) behind a mutex (const, thread-safe, deterministic: same files -> same answer).
    struct ModelInfo {
        float min[3] = {0.0f, 0.0f, 0.0f}, max[3] = {0.0f, 0.0f, 0.0f};
        bool has_bounds = false;
        std::vector<std::string> bones;       // upper-cased bone names
        std::vector<std::array<float, 3>> bone_pos;  // bind-world translation per bone
        std::vector<std::array<float, 9>> bone_rot;  // refixf t06: bind-world 3x3 part per bone (row-major)
        int32_t max_alt = -1;  // S8 t10: highest _ALT<n> index of the mesh names (0x12dca0), -1 none
        int32_t find(std::string_view bone) const;  // -1 when absent
    };
    // `space`: Space_Model_Name else Land_Model_Name; `fallback`: then the other mode's field, then Galactic_Model_Name.
    const ModelInfo* model_info(uint32_t type, bool space, bool fallback = false) const;
    // MapStampHooks::model_half_extents (land placements: Land, Space, Galactic model order): half x / y extents and the
    // z extent of the bind-pose bounds; false without model data.
    bool model_half_extents(uint32_t type, float& half_x, float& half_y, float& height_z) const;
    // SPACE_AVOIDANCE §3.2 / §5: the collision data of the type's model for the mode (Space_ / Land_Model_Name, no
    // fallback) for the fighter avoidance test: collision bound, COLLISIONBOX, collision-enabled mesh triangles in model
    // space (bind pose). Null without model data. Cached per (type, mode) behind the model mutex.
    const tactical::movement::AvoidModel* avoid_model(uint32_t type, bool space) const;
    // S7 t07 (MOVEMENT §2.2 model-extent fallback, SpaceObstacleHooks): half x / y of avoid_model(type, space = true)'s
    // collision bound (1 / 1 when the model has no collision mesh); false without model data.
    bool space_model_half_extents(uint32_t type, float& half_x, float& half_y) const;
    // OPT3 t05: loads the model data of `types` that is not cached yet (avoid_model(type, space); on land also
    // model_info(type, false, true), the stamp extents) on `pf`, one job per type, so a battle's first tick does not read
    // the files one type after another. Same entries as the lazy loads (same files -> same answer): only the time moves.
    void preload_models(std::span<const uint32_t> types, bool space, const ParallelFor& pf) const;
    // S8 t06 B4 (LOB_SOLVER §5.1, WeaponBehavior data pack init 0x54cb00): the fire bones <Muzzle_Bone_Name>_00, _01, ...
    // (default MUZZLEA) of the type's model for the mode (Space_ / Land_Model_Name, no fallback) until a name is missing,
    // as bind-pose offsets in the combat unit-local frame (x forward, y left, z up) x Scale_Factor. False without model
    // data (`out` cleared); true with an empty list = the model has no such bone (the weapon never fires). build_battle
    // (with a mode) stores them in UnitDesc::muzzle_bones.
    bool muzzle_bones(uint32_t type, bool space, std::vector<tactical::Vec3>& out) const;
    // S8 t10 (LOB_SOLVER §5.1 "Garrison fire bones in detail", type model load 0x375380): the gun-bone table of a type with
    // `guns` = Num_Garrison_Guns != 0 - M = the model's highest _ALT mesh index entries (ALT a = 0..M-1), `guns` bones
    // each: FP_0<i>_ALT<a> (i <= 9) or FP_<i>_ALT<a> (i >= 10), bind pose in the unit-local frame x Scale_Factor, with the
    // yaw of the bone's x axis; a missing bone is invalid (-1). False without model data (`out` cleared).
    bool garrison_bones(uint32_t type, bool space, int32_t guns, std::vector<std::vector<tactical::combat::GarrisonBone>>& out) const;
    // --- end battle host ---
#ifdef ALAMO_SIM_HAVE_FOG
    // Fog of war GameConstants (Desired{Land,Space}FOWCellSize, {Land,Space}FOWRegrowTime, AIUsesFogOfWar{Land,Space},
    // Tactical_Overrun_Multiple, Minimum_Tactical_Overrun_Time_In_Secs). Owner: fog agent.
    const tactical::fog::FogConstants& fog_constants() const { return fog_constants_; }
#endif

    // Per-battle tables: one UnitDesc per entry of `unit_types` (same order; duplicates allowed), the
    // hardpoints and projectiles they reference. out_unit_index[i] = UnitDesc index of unit_types[i].
    // (S8/S10 view: no ability data, no S9 fields - kept for the catalog tests.)
    tactical::combat::CombatData build(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index) const;
    // S10-int: the full battle tables (combat + S9 fields + abilities + visuals).
    BattleTables build_battle(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index) const;
    // battle host: the same with model data for `space` / land battles: a unit whose model resolves a hard point's
    // Attachment_Bone / Fire_Bone_A / Fire_Bone_B gets its own copy of that HardpointDesc with attach_offset / bone_a /
    // bone_b at the bind-pose bone positions x Scale_Factor (unit-local) [port: static bind pose; a fire bone missing
    // from the unit model falls back to the attachment bone; Model_To_Attach bones are not read]. Without model data the
    // tables equal build_battle's.
    BattleTables build_battle(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index, bool space) const;

private:
    using Tags = std::vector<std::pair<std::string, std::string>>;  // (upper-cased tag, trimmed text)
    struct BuildState {
        tactical::combat::CombatData* d = nullptr;
        BattleTables* full = nullptr;  // null: plain build()
        std::map<std::string, int32_t> hp_index;
        std::map<uint32_t, int32_t> proj_index;
        int32_t model_mode = -1;  // battle host: -1 no model data, 0 land, 1 space
    };
    int32_t hardpoint_on_model(int32_t base, uint32_t unit_type, BuildState& s) const;  // battle host
    void load_hardpoints(const FileProvider& files, std::vector<std::string>* diag);
    void load_priority_sets(const FileProvider& files, std::vector<std::string>* diag);
    tactical::combat::UnitDesc unit_desc(uint32_t t, BuildState& s) const;
    int32_t projectile(uint32_t t, BuildState& s) const;
    int32_t hardpoint(std::string_view name, BuildState& s) const;
    void build_units(const std::vector<uint32_t>& unit_types, std::vector<uint32_t>& out_unit_index, BuildState& s) const;
    void s9_unit_fields(uint32_t t, tactical::combat::UnitDesc& u, BuildState& s) const;
    tactical::abilities::AbilityObjectDesc ability_desc(uint32_t t, BuildState& s) const;
    // The XML element of raw-store field `f` (through the source document), null node if not found.
    bool field_node(uint32_t f, data::XmlNode& out) const;
    int32_t type_or_none(std::string_view name) const;

    const objects::GameObjectTypes* types_;
    tactical::combat::DamageArmorTable damage_;
    std::vector<tactical::combat::PrioritySet> priority_sets_;
    std::map<std::string, Tags> hardpoints_;  // upper-cased name -> tags (lookup only)
    float space_health_mult_ = 1.5f, land_health_mult_ = 1.0f;
    float space_idle_speed_ = 0.0f;
    float land_capture_countdown_ = -1.0f, space_capture_countdown_ = -1.0f, garrisoned_range_mult_ = -1.0f;
    bool use_reinforcement_points_ = true;
    float threat_decay_step_ = 0.05f;
    float engines_disabled_speed_ = 0.4f;
    float nebula_ability_disable_ = 5.0f, force_ability_disable_ = 0.0f, ion_storm_shield_disable_ = 5.0f;
    tactical::combat::CombatConfig combat_config_;  // OI tactical
    TransitionConstants transition_;                // battle host
    const FileProvider* files_ = nullptr;           // battle host: model reads
    struct ModelCache;                              // battle host: (type, mode) -> ModelInfo, mutex-guarded
    std::shared_ptr<ModelCache> model_cache_;
#ifdef ALAMO_SIM_HAVE_FOG
    tactical::fog::FogConstants fog_constants_;
#endif
};

}  // namespace alamo::sim

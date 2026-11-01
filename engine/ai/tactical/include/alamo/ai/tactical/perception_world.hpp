// A1 stage 1b - the tactical world as tactical perception reads it (docs/re/AI_TACTICAL_PERCEPTION.md).
//
// A plain snapshot the host (stage 2 framework hookup) fills from the object store, the player list,
// the type database, the FOW system and the mode, once per AI frame (before AI evaluation, after the
// threat-grid service). Perception never writes to it. Everything is indexed in a deterministic order:
//   * players: PLAYER LIST ORDER (0x140a16fd0); every "player" in tactical perception is an index
//     into `players` (EvalContext::self / enemy / human are player indices);
//   * objects: GOM object-list order (the host's order); found by id through index();
//   * types: any order; objects refer to them by index.
// Fields carry the original's source (object / type offsets, behaviours) so the adapter knows what to
// read; [M]/[L] tags are the spec's confidence of what the field means.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/ai/tactical/constants.hpp"
#include "alamo/ai/tactical/hint_zones.hpp"
#include "alamo/ai/tactical/target_list.hpp"

namespace alamo {
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::ai::tactical {

inline constexpr int32_t kNoPlayerIndex = -1;
// "None found" distance (1e18 rounded to float, as the original returns it) [H].
inline constexpr double kNoDistance = 9.999999843067494e17;

struct PerceptionType {
    std::string name;
    uint64_t category = 0;            // GameObjectCategoryType mask (type +0x1648)
    float combat_power = 0.0f;        // AI_Combat_Power of the type (reinforcement pools, HasUnit)
    bool is_hero = false;             // type +0x13
    float bombing_run_prevention_radius = 0.0f;  // Bombing_Run_Prevention_Radius (+0x1dfc): IsValidBombingTarget
    bool community_property = false;  // Is_Community_Property (+0x27)
    int32_t builds_into = -1;         // type +0x9f0 "builds into" (index), -1 none
    // --- A2 tactical: PotentialPlan inputs (tactical_pp.hpp) ---
    int32_t tech_level = 0;                // Tech_Level (+0x89C)
    float population = 0.0f;               // pop(T, mode) 0x373500 (Land_/Space_Override_Population_Value if >= 0)
    float build_cost = 0.0f;               // Build_Cost_Credits (+0x86C)
    float tactical_cost = 0.0f;            // TacticalBuild price: pad price 0x3744e0 (UC types) / tactical price 0x496d80
    std::vector<std::string> affiliation;  // Affiliation (0x3751a0); empty = every faction
    // Team expansion filter (AI_GALACTIC_2 §1.3, STRATEGIC_GAPS §1.2 [H]): flag +0x67 Create_Team, +0x29 = listed in
    // some type's Squadron_Units, +0x21 Cinematic_Object_Only, or behaviour DUMMY_STAR_BASE / DUMMY_GROUND_BASE.
    bool pp_excluded = false;
    bool under_construction = false;       // behaviour 0x2A TACTICAL_UNDER_CONSTRUCTION (pad structures, UC_)
    bool generic_hero = false;             // +0x14
    bool special_weapon = false;           // Is_Special_Weapon_In_Space (+0x1E) or behaviour 0x37 (Hero Setup C)
    bool has_ai_script = false;            // a Lua script (Lua_Script) the Hero marshaller could load (§2.1 item 4) [M]
    // --- end A2 tactical ---
};

// One hard point of an object (S9 engine/tactical/abilities will provide them; until then the host
// fills them or installs PerceptionWorld::hard_points).
struct PerceptionHardPoint {
    int32_t type = 0;             // HardPointType value; 5..10 are the weapon types the grid uses
    bool grid_flag = true;        // hard point +0x4c (must be set for a grid entry)
    float weapon_range = 0.0f;    // hard-point type +0x238
    float power = 0.0f;           // hard-point value +0x88 [M: per-hard-point combat power]
    float health = 1.0f;          // fraction (HardPointHealth [L])
    bool destroyable = false;
    bool destroyed = false;
    bool turret = false;          // position = the hard point's world position
    Vec3f world_position;
};

struct PerceptionObject {
    int32_t id = -1;
    int32_t owner = kNoPlayerIndex;  // player list index (obj +0x58 -> player)
    int32_t type = -1;               // own / container type (0x396380) index; a TEAM: its company type (A1_READINGS §29)
    int32_t rep_type = -1;           // representative type (0x3973b0) index; -1 = same as `type`
    bool alive = true;               // obj +0x3a0 bit 0x40 clear
    bool has_unit_ai = false;        // UNIT_AI behaviour 0x12: the object is in the threat grid
    bool team_or_hide_when_fogged = false;  // TEAM 0x16 or HIDE_WHEN_FOGGED 0x19
    bool mobile = false;             // has a locomotor (look-ahead radius)
    float speed = 0.0f;              // current speed (units per second for the look-ahead) [M]
    Vec3f position;
    float transform[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};  // 3x4 (obj +0x248..+0x274): re-projection gate
    float radius = 0.0f;             // 0x397780
    float health = 1.0f;             // hull fraction (0x396df0)
    float shield = 0.0f;             // 0x3976f0 [M]
    float energy = 0.0f;             // 0x396560 [M]
    float combat_power = 0.0f;       // AI_Combat_Power (0x370e60, squad fill scaling applied)
    float max_attack_range = 0.0f;   // 0x3749d0 on the representative type
    std::vector<PerceptionHardPoint> hard_points;
    int32_t team_leader = -1;        // team: its leader object id (0x4ec820), -1 none
    // unit-target tokens
    bool has_damage_tracking = false;
    int32_t last_damage_frame = -1;  // DAMAGE_TRACKING 0x58bb40
    bool is_build_pad = false;       // TACTICAL_BUILD_OBJECTS 0x29
    int32_t built_type = -1;         // object built / under construction on the pad (type index), -1 none
    bool is_cash_point = false;      // CASH_POINT (+0x36e)
    float cash_point_value = 0.0f;   // [L]
    bool contestable = false;        // behaviour 0x3D + 0x3751a0 test [M]
    int32_t garrison_capacity = 0;   // 0x3954b0
    int32_t garrison_occupants = 0;  // obj +0x210 -> +0x34
    float additional_population_capacity = 0.0f;  // [L]
    std::vector<int32_t> contained_types;  // garrison / contained objects (+0xf8) and team members (types)
    bool valid_bombing_target = false;     // unused: IsValidBombingTarget is computed (A1_READINGS §21); kept for saves
    bool engines_online = true;            // [L]
    // player-token inputs
    bool dummy_ground_structure = false;   // behaviour 0x5F (BaseLevel, land skirmish)
    bool dummy_star_base = false;          // behaviour 0x5C (BaseLevel, space)
    int32_t starbase_level = 0;            // 0x397540
    bool built_structure = false;          // counted by TacticalBuiltStructureCount (free-store table) [M]
    bool transport_landing = false;        // TRANSPORT_LANDING 0x36: reinforcement still in the air (land)
    uint64_t visible_to = 0;               // bit p: UnitAI reports visible to player p (ForceVisibility) [M]
    int32_t zone = -1;                     // land: current hint zone (land mode slot +0x180), -1 none
};

struct PerceptionPlayerInfo {
    int32_t id = -1;                 // the player's id (+0x478 name id; informational)
    std::string faction;             // faction name
    bool neutral_faction = false;    // faction Is_Neutral (+0x108)
    bool is_human = false;
    bool attacker = false;           // campaign battle: the attacker (0x28adf0)
    std::string ai_difficulty;       // AI difficulty name ("Easy" / "Normal" / "Hard"), empty = none / human
    float credits = 0.0f;            // float +0x70
    int32_t pop_cap = 0, pop_used = 0;
    bool can_retreat = false;        // retreat coordinator reports 0 for P
    bool is_retreating = false;      // [L]
    bool bombing_run_available = false;
    bool orbital_bombardment_available = false;
    float tech_level = 0.0f;
    std::vector<int32_t> reinforcements;    // tactical reinforcement pool / units in orbit (type indices)
    std::vector<int32_t> production_queue;  // player +0x3a0 (type indices)
};

struct StartMarker {
    enum class Kind : uint8_t { PlayerPosition = 0, DefendingForces = 1, AttackerEntry = 2 };
    Kind kind = Kind::PlayerPosition;
    int32_t player_id = -1;  // PlayerPosition: Player_%d_Position_Marker
    Vec3f position;
};

struct PerceptionWorld {
    TacticalMode mode = TacticalMode::Land;
    uint32_t frame = 0;              // F, the AI context's mode frame
    bool campaign = false;           // battle has a galactic conflict parent (0x28aa00)
    bool land_control = false;       // IsLandControlGame [L]
    bool setup_phase = false;        // 0x35f790 [M]
    bool story_campaign = false;
    bool fow_active = false;         // global FOW-active flag (0x140a284c4)
    Rect2 map;                       // map extents (mode slot +0x1f8)
    int32_t conflict_ground_structures = 0;  // land campaign BaseLevel: planet ground structures w/o flag 4

    std::vector<PerceptionPlayerInfo> players;  // player list order
    std::vector<uint8_t> relation;              // players^2, [a * P + b]; 0 = allied (self included); empty => only self
    std::vector<PerceptionType> types;
    std::vector<PerceptionObject> objects;      // GOM order
    std::vector<StartMarker> markers;

    // Real fog (force = 1): is object `id` fogged for player p. Absent => never fogged.
    std::function<bool(int32_t player, int32_t object_id)> object_fogged;
    // Real fog at a point (cell centre) for player p. Absent => never fogged.
    std::function<bool(int32_t player, float x, float y)> point_fogged;
    // Land: the passability cell at a point ((int)((x + 10) / 20), (int)((y + 10) / 20)) is off the grid or its base
    // word (.ted 0x10A) has Steep | Water (0x22); TimeLastSeen skips those cells (A1_READINGS §26). Absent => none.
    std::function<bool(float x, float y)> point_blocked;
    // Base shield test (land mode 0x3b8790): point inside an enemy (true) / allied (false) shield of `self`.
    std::function<bool(const Vec3f& p, int32_t self, bool enemy)> inside_shield;
    // Hint evaluator override (Hints.<name>) of zone z. Absent => the zone's own hint map (HintZone::hints; a missing
    // name or a zone without hints fails, A1_READINGS §22).
    std::function<double(int32_t zone, std::string_view hint)> zone_hint;
    // Free-store visible open pads within `range` of object id (0x4d3770). Absent => computed from objects [port].
    std::function<uint32_t(int32_t object_id, float range)> nearby_open_pads;
    // Pad of type `pad_type` accepts building type `type` for faction (0x374330). Absent => never.
    std::function<bool(int32_t pad_type, int32_t type, std::string_view faction)> pad_accepts;
    // Hard points of an object (S9 hookup). Absent => PerceptionObject::hard_points.
    std::function<void(int32_t object_id, std::vector<PerceptionHardPoint>& out)> hard_points;

    // Builds the id lookup (call after filling `objects`).
    void index();
    const PerceptionObject* object(int32_t id) const;
    uint32_t player_count() const { return static_cast<uint32_t>(players.size()); }
    bool is_player(int32_t p) const { return p >= 0 && static_cast<uint32_t>(p) < players.size(); }
    // relation(a -> b); 0 for a == b; with an empty table every other pair is 1.
    uint8_t rel(int32_t a, int32_t b) const;
    bool allied(int32_t a, int32_t b) const { return rel(a, b) == 0; }
    const PerceptionType* type_of(const PerceptionObject& o) const;
    const PerceptionType* rep_type_of(const PerceptionObject& o) const;
    void object_hard_points(const PerceptionObject& o, std::vector<PerceptionHardPoint>& out) const;

    // N1 save / load (contract: alamo/core/save_file.hpp; part of TacticalAiWorld's chunk). Layout: u32 version (1), the
    // scalar fields, players (every field), the relation table, `types` as its count + types_digest() only (definitions:
    // the host's fill() rebuilds them), objects in GOM order (every field, hard points included), markers. Not saved:
    // the callbacks (host wiring) and `types`. read() keeps this world's callbacks and types: it fails (unchanged) when
    // this world has types that differ from the saved count / digest, and hands the saved ones back for a later check
    // (TacticalAiWorld::relink). The id index is rebuilt. Reads its own fields only (the owner checks done()).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r, uint32_t& saved_type_count, uint64_t& saved_types_digest);
    uint64_t types_digest() const;  // StateHasher digest of every field of every type

private:
    std::vector<std::pair<int32_t, uint32_t>> id_index_;  // sorted (id, index); lookup only
};

}  // namespace alamo::ai::tactical

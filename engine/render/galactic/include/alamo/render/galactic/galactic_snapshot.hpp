// alamo_render_galactic - R2 galactic sim -> render snapshot (Phase 4 roadmap R2; same contract as R1's
// sim_snapshot.hpp: render preparation consumes a COMPLETED snapshot and never touches the sim).
//
// GalacticSnapshot: an immutable copy of what the galactic map renderer needs from one finished tick, taken
// after SimHost::tick_once returned, from the galactic mode's GalacticSim (engine/galactic) through its const
// interface only (planets(), fleets(), map(), store() reads). Nothing registered with the CrcRegistry is written,
// so the per-tick CRC ("galactic.state" included) is the same with snapshots on or off
// (alamo_render_galactic_tests, alamo_render --galactic-headless-check).
//   planets: every <Locations> row in row order (scenery rows such as Galaxy_Core_Art_Model included, typed by
//            their row name): type, position, owner + colour, political control, starbase (object, type, owner,
//            level), ground-base level, orbiting fleets, flags (planet / In_Background / hidden / destroyed /
//            corrupted / in battle / drawable);
//   routes:  the campaign's trade routes (route row order: endpoints);
//   fleets:  every live fleet in ascending object id: owner + colour, position, facing, moving, orbit planet /
//            hop endpoints, its slot in the planet's orbit list, member count, the type whose model draws it
//            (Galactic_Fleet container, or a member's Galactic_Fleet_Override_Model_Name [port: first member with
//            an override, docs give no rule]).
// The planet rows fan out over a ParallelFor (per-index outputs), fleets are collected in fleet order and
// sorted by id: the result is bit-identical for every worker count.
//
// Interpolation (MAIN_LOOP.md: the original renders the latest state, no interpolation): InterpMode::Original
// (default, parity) copies the latest snapshot; InterpMode::Smooth blends fleet position / facing of fleets
// present in both snapshots by the frame's tick alpha and the idle-animation clock (planet spin); planets and
// routes are not interpolated (they do not move).
#pragma once
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/file_provider.hpp"
#include "alamo/core/galaxy.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/materials/constants.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/render/sim_snapshot.hpp"

namespace alamo::objects {
class GameObjectTypes;
}
namespace alamo::galactic {
class GalacticSim;
struct GalacticRules;
}  // namespace alamo::galactic
namespace alamo::sim {
class SimHost;
}

namespace alamo::render {

// Galactic presentation fields of one GameObjectType (typed view, variants merged by GameObjectTypes).
struct GalacticTypeInfo {
    uint32_t name_crc = 0;
    std::string name;
    std::string model;            // Galactic_Model_Name ("" = not drawn)
    std::string destroyed_model;  // Destroyed_Galactic_Model_Name (planets)
    std::string fleet_override;   // Galactic_Fleet_Override_Model_Name (units)
    float scale = 1.0f;           // Scale_Factor
    float lod_bias = 4.0f;        // LOD_Bias
    bool in_background = false;   // In_Background (galactic background scene, SPACE_AND_OPEN.md A.3 table)
    bool loop_idle = false;       // Loop_Idle_Anim_00
    float idle_rate = 1.0f;       // Idle_Anim_00_Rate_Mod
};

class GalacticTypeTable {
public:
    void build(const objects::GameObjectTypes& types);
    size_t size() const { return info_.size(); }
    bool valid(objects::TypeIndex t) const { return t < info_.size(); }
    const GalacticTypeInfo& operator[](objects::TypeIndex t) const { return info_[t]; }
    // First definition of a type name (case-insensitive), objects::kNone if unknown.
    objects::TypeIndex find(std::string_view name) const;
    // The galactic fleet container (CONTAINERS.XML "Galactic_Fleet", w_planet_fleet.alo); kNone if absent.
    objects::TypeIndex fleet_type() const { return fleet_type_; }
    // Tests / tools.
    void set_model(objects::TypeIndex t, std::string model) { info_[t].model = std::move(model); }

private:
    const objects::GameObjectTypes* types_ = nullptr;
    std::vector<GalacticTypeInfo> info_;
    objects::TypeIndex fleet_type_ = objects::kNone;
};

// Faction colours (Factions.xml <Color> / <No_Colorization_Color> / <Display_Font_Color> / <Big_Fleet_Color>,
// FactionFiles.xml order; faction record +0xa8 / +0xd8 / +0xb8 / +0xc8, xml_field_tables.md). R2 t01: `display_font` is the
// colour of the faction's text (planet labels, Planet Info name: GALACTIC_DIALOGS §9.2 [H] reads +0xb8..+0xc4); `big_fleet`
// is parsed but no reader is traced yet (needs RE).
struct FactionColor {
    std::string name;
    bool neutral = false;
    bool has_color = false;
    materials::float4 color{1, 1, 1, 1};
    materials::float4 no_colorization{1, 1, 1, 1};
    bool has_display_font = false;
    materials::float4 display_font{1, 1, 1, 1};
    bool has_big_fleet = false;
    materials::float4 big_fleet{1, 1, 1, 1};
};
std::vector<FactionColor> load_faction_colors(const FileProvider& files);
// Colours of the faction called `name` (case-insensitive; the table's first match) - what the HUD feeds from instead of
// hard-coded values. map_color = <Color> (ring, selection tint, pancake secondary colour); text_color = <Display_Font_Color>
// (<Color> when a mod's faction lacks it). false = no such faction in the table.
bool faction_map_color(std::span<const FactionColor> table, std::string_view name, materials::float4& out);
bool faction_text_color(std::span<const FactionColor> table, std::string_view name, materials::float4& out);
// "R, G, B[, A]" bytes (commas and / or blanks; TradeRouteLines.xml has "128, 0 0, 255") -> float4 / 255.
bool parse_color_bytes(std::string_view text, materials::float4& out);

// Owner -> colour of the galactic map. [port] SPACE_AND_OPEN.md B.3 gives the tactical rule (MP palette by player
// slot); for the single-player galaxy the port uses the owner FACTION's <Color> (Factions.xml), falling back to the
// MP palette entry of the player id when the faction has none. Neutral owners: the faction's No_Colorization_Color.
struct GalacticPalette {
    std::vector<materials::float4> player;  // per player id
    std::vector<uint8_t> neutral;           // per player id
    materials::float4 none{0.55f, 0.55f, 0.55f, 1.0f};  // kNoPlayer
    materials::float4 color(int32_t owner, uint8_t& index) const;
};
GalacticPalette make_galactic_palette(const galactic::GalacticSim& sim, std::span<const FactionColor> factions);

enum GalacticPlanetFlag : uint16_t {
    kGalPlanet = 0x01,       // row has a planet object (PLANET behaviour)
    kGalBackground = 0x02,   // In_Background type or row flag: background scene
    kGalHidden = 0x04,       // inaccessible (D +0x2e0)
    kGalDestroyed = 0x08,    // destroyed planet (Destroyed_Galactic_Model_Name)
    kGalCorrupted = 0x10,
    kGalInBattle = 0x20,
    kGalDrawable = 0x40,     // the row's type has a galactic model
    kGalFogged = 0x80,       // not visible to the local player (apply_galactic_fog): wireframe globe, no owner info
    kGalCorruptionParticles = 0x100,   // GRC 4.5: the corruption particle object D +0x300 exists (pairs valid)
    kGalCorruptionTransition = 0x200,  // GRC 4.1: gaining or removing (D +0x2e2 || +0x2e3)
    // FIXPASS_GALACTIC §3.2: the running transition comes from a FADING source (a gain / removal wrapper: ability,
    // corruption action, mission success, Death Star hack, story REMOVE_CORRUPTION), not from the 10 s ownership change.
    // Only fading transitions tint the planet's corruption path lines. build_galactic_snapshot infers it [port]: a gain
    // fades when the particle object exists (wrappers rebuild at begin, ownership gains do not - GRC 4.6); a removal
    // fades when the pairs ramp (start != end: the wrapper rebuilt them). Hosts that know the source override it with
    // apply_galactic_corruption_fading.
    kGalCorruptionFading = 0x400,
};

struct GalacticPlanetSnap {
    PlanetIndex row = kInvalidPlanet;
    objects::ObjectId id = objects::kNoObjectId;  // planet object (kNoObjectId for scenery rows)
    objects::TypeIndex type = objects::kNone;
    Vec3 position{};
    int32_t owner = -1;
    uint8_t team_index = kNeutralTeam;
    materials::float4 team_color{1, 1, 1, 1};
    float control = 0.0f;  // political control (D +0x68)
    int32_t starbase_level = 0;
    objects::ObjectId starbase = objects::kNoObjectId;
    objects::TypeIndex starbase_type = objects::kNone;
    int32_t starbase_owner = -1;
    materials::float4 starbase_color{1, 1, 1, 1};
    int32_t ground_level = 0;     // ground structures
    uint32_t surface_units = 0;
    uint32_t orbit_fleets = 0;
    uint16_t flags = 0;
    // GRC 4 corruption presentation (docs/re/GALACTIC_REVEAL_CORRUPTION.md; read from the sim, never fed back):
    int8_t corruption_type = -1;       // current type D +0x2f4
    int8_t corruption_effective = -1;  // 0x4b7e90: the pending type while a transition runs, else the current one
    float corruption_fraction = 1.0f;  // 0x4b7e20 at the snapshot's mode frame (the planet service's frame argument)
    // Particle pairs D +0x308..+0x31c: brightness start/end, scale start/end, energy start/end (kGalCorruptionParticles).
    float corruption_pairs[6] = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
};

enum GalacticRouteFlag : uint8_t {
    // GRC 2.4 step 4: a trade route is visible only when both endpoints' raw revealed bytes (D +0x2c9) are set (an
    // endpoint without a planet counts as set); auto-reveal is ignored. Set by apply_galactic_fog: not drawn.
    kGalRouteHidden = 0x01,
};

struct GalacticRouteSnap {
    PlanetIndex a = kInvalidPlanet, b = kInvalidPlanet;
    uint8_t flags = 0;
};

enum GalacticFleetFlag : uint8_t {
    kGalFleetMoving = 0x01,
    kGalFleetDrawable = 0x02,  // a model type was found
    // GALAXY_MAP_PRESENTATION §2.1 (fleet service 0x4352f0) [H]: every non-transport member has the galactic stealth
    // byte set -> the fleet model shows _ALT1 (gold scrolling dot), else _ALT0. Members that are company transports
    // (GalacticSim::is_company_transport) are skipped; [port] a fleet with no non-transport member keeps ALT0.
    kGalFleetStealthed = 0x04,
};

struct GalacticFleetSnap {
    objects::ObjectId id = objects::kNoObjectId;
    uint32_t index = 0xFFFFFFFFu;  // GalacticSim fleet index
    int32_t owner = -1;
    uint8_t team_index = kNeutralTeam;
    materials::float4 team_color{1, 1, 1, 1};
    Vec3 position{};
    Vec3 facing{1, 0, 0};
    PlanetIndex planet = kInvalidPlanet;  // orbiting (kInvalidPlanet in transit)
    PlanetIndex from = kInvalidPlanet, to = kInvalidPlanet;
    uint32_t orbit_slot = 0;  // index in the planet's orbit list (arrival order): only the (undrawn) 3D stand-in offset
    // The fleet's own orbit slot as the sim keeps it (FleetState::slot, GALAXY_MAP_PRESENTATION §1.3 / playtest 3 t07):
    // 0..2 while orbiting, unique among its owner's fleets at that planet; -1 in transit or without a free slot. The HUD's
    // g_planet_fleet buttons sit on these slots; the 3D map draws no orbiting fleet (§2.1), so nothing else reads it.
    int8_t slot = -1;
    uint32_t members = 0;
    objects::TypeIndex model_type = objects::kNone;
    uint8_t flags = 0;
};

struct GalacticSnapshot {
    uint64_t tick = 0;         // SimHost global frame of the finished tick
    int32_t mode_frame = 0;    // GalacticSim::frame() (frozen while paused / under a tactical battle)
    int32_t galactic_day = 0;  // fiscal cycles
    std::vector<GalacticPlanetSnap> planets;  // row order
    std::vector<GalacticRouteSnap> routes;    // route order
    std::vector<GalacticFleetSnap> fleets;    // ascending id
    const GalacticFleetSnap* find_fleet(objects::ObjectId id) const;  // binary search, null if absent
    uint64_t digest() const;  // ordered digest of every field (thread-determinism tests)
};

// Builds `out` from a GalacticSim (const access only).
void build_galactic_snapshot(const galactic::GalacticSim& sim, uint64_t tick, const GalacticTypeTable& types,
                             const GalacticPalette& palette, GalacticSnapshot& out, const ParallelFor& pf = serial_for);
// The galactic mode of a host (bottom of the mode stack); false and `out` emptied when there is none.
bool capture_galactic_snapshot(const sim::SimHost& host, const GalacticTypeTable& types, const GalacticPalette& palette,
                               GalacticSnapshot& out, const ParallelFor& pf = serial_for);
// Galactic fog for the local player (set after build / capture; the sim's state is not changed). `visible` holds one
// byte per planet row (0 = not visible -> kGalFogged); rows past its end stay visible, so an empty span clears the fog.
// Routes get kGalRouteHidden from `route_revealed` (one byte per row, the raw revealed byte; rows past its end count as
// revealed); an empty span shows every route.
void apply_galactic_fog(GalacticSnapshot& snap, std::span<const uint8_t> visible, std::span<const uint8_t> route_revealed);
inline void apply_galactic_fog(GalacticSnapshot& snap, std::span<const uint8_t> visible) { apply_galactic_fog(snap, visible, visible); }
// Which reveal state draws a planet fogged.
enum class GalacticFogRule : uint8_t {
    // GRC 1.1 / 1.3 step 5: the pre-step's presentation state and the UI read the raw byte D +0x2c9 (default; under
    // auto-reveal unrevealed planets stay fogged until the reveal spread reaches them).
    RawByte,
    // Is_Planet_Revealed 0x3fa420: hidden -> fogged, auto-reveal -> nothing fogged, else the byte.
    IsPlanetRevealed,
};
// Same from the sim's reveal state (GRC 1). The byte belongs to the sim's local player (Is_Planet_Revealed takes no
// player); nothing is fogged when `player` < 0 (no viewpoint) or the sim has no local player. Scenery rows (no planet
// object) are never fogged. Routes: raw byte on both endpoints, whatever the rule.
void apply_galactic_fog(const galactic::GalacticSim& sim, int32_t player, GalacticSnapshot& snap,
                        GalacticFogRule rule = GalacticFogRule::RawByte);
// FIXPASS_GALACTIC §3.2 host override of kGalCorruptionFading: one byte per planet row, 1 = the row's running corruption
// transition comes from a fading source (wrapper), 0 = the ownership change (no fade); rows past the span's end keep
// the inferred flag. The flag is only kept on rows with kGalCorruptionTransition.
void apply_galactic_corruption_fading(GalacticSnapshot& snap, std::span<const uint8_t> fading);
// The host's GalacticSim (const), null when the stack has no galactic mode with a sim.
const galactic::GalacticSim* find_galactic_sim(const sim::SimHost& host);

// Double buffer of immutable snapshots (R1 SnapshotBuffer semantics).
class GalacticSnapshotBuffer {
public:
    struct Pair {
        std::shared_ptr<const GalacticSnapshot> prev, curr;
    };
    void publish(std::shared_ptr<const GalacticSnapshot> s);
    Pair latest() const;
    void clear();
    uint64_t published() const;

private:
    mutable std::mutex mutex_;
    Pair pair_;
    uint64_t published_ = 0;
};

struct GalacticFrame {
    uint64_t tick = 0;
    float alpha = 1.0f;
    InterpMode mode = InterpMode::Original;
    float anim_frames = 0.0f;  // galactic mode frames for idle animations (smooth: blended)
    std::vector<GalacticPlanetSnap> planets;
    std::vector<GalacticRouteSnap> routes;
    std::vector<GalacticFleetSnap> fleets;
};
// Frame view between prev (may be null) and curr at `alpha` in [0, 1] (clamped; non-finite -> 1).
void interpolate_galactic(const GalacticSnapshot* prev, const GalacticSnapshot& curr, float alpha, InterpMode mode,
                          GalacticFrame& out);

}  // namespace alamo::render

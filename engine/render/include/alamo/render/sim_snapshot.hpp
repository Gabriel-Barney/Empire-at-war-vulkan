// alamo_render - R1 sim -> render snapshot (Phase 4 roadmap step R1, "modern-engine" rule: render
// preparation consumes a COMPLETED snapshot and never touches the sim).
//
// SimSnapshot: an immutable copy of what the renderer needs from one finished sim tick, taken at the end
// of the tick (after SimHost::tick_once returned) from the top tactical mode's PRIMARY store:
//   per object (ascending object id): type, owner + team colour (SPACE_AND_OPEN.md B.3: player slot -> the
//   9-colour MP palette, neutral owners -> No_Colorization_Color), position, facing, health, LOD_Bias,
//   fade, flags (dying / In_Background / drawable) and its hardpoint destroyed bits.
// Building reads the store through its const interface only: no ObjectStore / mode / RNG / command /
// spatial state is written and nothing registered with the CrcRegistry is touched, so the per-tick CRC
// is the same with snapshots on or off (alamo_render_sim_tests, alamo_render --sim-headless-check).
// It fans out over a ParallelFor (per-index outputs, then an ordered compaction + id sort), so the
// result is bit-identical for every worker count.
//
// Fade: 1 for live objects; 0 for objects destroyed this tick but not yet flushed (kSnapDying) - the
// smooth mode fades them out over one tick, the original mode hides them.
// Hardpoints: the sim does not spawn hardpoint sub-objects yet (engine/apps/sim map_spawn open item).
// PROVISIONAL rule: a child attached to an object (ObjectStore::attach_parent) whose type name CRC is
// one of the parent type's <HardPoints> names marks that hardpoint destroyed while it is dying or at
// health <= 0. The render side keeps destroyed bits sticky (the game never repairs hardpoint visuals,
// SPACE_AND_OPEN.md B.10).
//
// Interpolation (docs/re/MAIN_LOOP.md: the original renders once per loop iteration with NO
// interpolation): InterpMode::Original (default, parity) draws the latest snapshot as is;
// InterpMode::Smooth blends the previous and the latest snapshot by the frame's tick alpha (position
// lerp, facing nlerp, roll / pitch shorter-way angle lerp, health / fade lerp; objects new this tick appear at their current state, objects
// gone this tick are dropped).
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "alamo/assets/math.hpp"
#include "alamo/core/parallel.hpp"
#include "alamo/materials/constants.hpp"
#include "alamo/objects/object_store.hpp"
#include "alamo/render/beams.hpp"
#include "alamo/render/fog_view.hpp"

namespace alamo::objects {
class GameObjectTypes;
}
namespace alamo::sim {
class SimHost;
class TacticalBattle;
}

namespace alamo::render {

class GameTypeResolver;
class BeamStyleTable;  // sim_beams.hpp

inline constexpr uint8_t kNeutralTeam = 0xFF;  // SnapshotObject::team_index of neutral owners

// Render-relevant data of one GameObjectType, resolved once per session.
struct SnapshotTypeInfo {
    uint32_t name_crc = 0;           // alamo::name_crc of the type name (map placements / GameTypeResolver key)
    std::string name;
    std::string model;               // .ALO name for the mode (Space_ / Land_ / Galactic_Model_Name); "" = not drawn
    float scale = 1.0f;              // Scale_Factor
    float lod_bias = 4.0f;           // LOD_Bias (kDefaultLodBias when absent)
    float max_health = 0.0f;         // Tactical_Health (health_fraction denominator)
    bool in_background = false;      // In_Background (space: drawn in the background scene)
    bool has_no_colorization = false;
    materials::float4 no_colorization{1, 1, 1, 1};
    std::vector<uint32_t> hardpoint_crcs;  // name_crc of each <HardPoints> entry, HardPoints order
    // Particle gates for the render side (R4 asks; xml_field_tables: type +0x3d bool, +0x1818 float).
    bool allow_particle_model_culling = false;  // Allow_Particle_Model_Culling: the model's particles may be culled locally (fog of war)
    float no_particle_below_detail = 0.0f;      // No_Particle_Below_Detail_Level: no particles while Particle Detail < this
    // Land damage states (Land_Damage_Thresholds / Land_Damage_Alternates, TERRAIN_WATER_POST 1.6: health picks a model
    // alternate = the _ALT index of meshes / proxies). Same length (shorter list wins); empty = no damage states.
    std::vector<float> damage_thresholds;
    std::vector<int8_t> damage_alternates;
    // R5 decorations (SurfaceFX.xml names, matched case-insensitively): SurfaceFX_Name list (dynamic tracks /
    // footprints / terrain modifiers) and Explosion_Scorch_Mark (PARTICLE types: scorch decal when the object appears).
    std::vector<std::string> surface_fx;
    std::string scorch_mark;
    // R5 fogged-model manager (FOG_OF_WAR §5.4, AI_TACTICAL §1): Last_State_Visible_Under_FOW (+0x88: a fogged object
    // stays drawn as a last-seen ghost) and Initial_State_Visible_Under_FOW (+0x89: a ghost from creation).
    bool ghost_last_state = false, ghost_initial_state = false;
    // R5 SurfaceFX particles: Max_Speed (Particle_Min_Energy is compared with speed / Max_Speed [port]); 0 = unknown.
    float max_speed = 0.0f;
    // R3 spin-away deaths: Spin_Away_On_Death_Explosion (a particle type / ALO played when the spinning corpse ends).
    std::string spin_away_explosion;
    // Projectile types (xml_field_tables: Projectile_Custom_Render +0x23c int, Projectile_Width +0x464, Projectile_Length
    // +0x468, Projectile_Texture_Slot +0x46c, Projectile_Laser_Color +0x454): Projectile_Custom_Render 1 (beam) / 2 (kite)
    // are procedural lasers (lasers.hpp, PROJECTILE_NEBULA_DRAW.md 1) - their model is never loaded (`model` stays "").
    int32_t projectile_custom_render = 0;
    float projectile_width = 0.0f, projectile_length = 0.0f;
    uint8_t projectile_slot[2] = {0, 0};                  // (column, row)
    uint8_t projectile_color[4] = {255, 255, 255, 255};  // R, G, B, A
    // R5 stealth (ABILITY_VISUALS §1.1): the type's Stealth_Ability / Force_Cloak_Ability fields (nested under <Abilities>),
    // colours 0..255 in the XML -> 0..1 here (R, G, B, A); the transition time T in seconds (Stealth_Transition_Time /
    // Force_Cloak_Transition_In_Secs). has_* = the tag was found (the colour stays white, T 0 = instant, otherwise).
    bool has_stealth_color = false, has_cloak_color = false;
    float stealth_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float cloak_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float stealth_time = 0.0f, cloak_time = 0.0f;
};

// Current _ALT index for a health fraction (SnapshotObject::alt), TACTICAL_GAPS §6.3 (0x3ac290) [H]: walk EVERY
// Land_Damage_Thresholds entry in list order, each with fraction <= t_k sets alt := Land_Damage_Alternates[k] (the last
// match wins, default 0). No mode test: space types with thresholds use it too. The model applies it only while
// alt <= its highest ALT index (SimInstanceBuilder::applied_alt). Data quirk kept: thresholds "1" / alternates "3" shows
// alt 3 at full health.
int8_t damage_alt_index(const SnapshotTypeInfo& t, float health_fraction);

class SnapshotTypeTable {
public:
    // One entry per TypeIndex of `types`. With a GameTypeResolver (the same one MapScene uses) the model, scale,
    // LOD_Bias, In_Background, No_Colorization_Color, hardpoint list, MP palette and neutral factions come from it,
    // so sim objects look exactly like the map's placements; without it they come from the typed object view
    // (variants merged by GameObjectTypes), the default MP palette and "owner < 0 = neutral".
    void build(const objects::GameObjectTypes& types, objects::ModeKind mode, const GameTypeResolver* resolver = nullptr);
    size_t size() const { return info_.size(); }
    const SnapshotTypeInfo& operator[](objects::TypeIndex t) const { return info_[t]; }
    bool valid(objects::TypeIndex t) const { return t < info_.size(); }
    objects::ModeKind mode() const { return mode_; }
    bool space() const { return mode_ == objects::ModeKind::Space; }
    // B.3 team colour: non-neutral owner -> palette[owner % 9] (index = owner % 9); neutral -> the type's
    // No_Colorization_Color when set, else the owner faction's (index kNeutralTeam).
    materials::float4 team_color(int32_t owner, const SnapshotTypeInfo& t, uint8_t& index) const;
    // Tests / tools: replace the model of every type (e.g. a debug box for model-less synthetic types).
    void set_model(objects::TypeIndex t, std::string model) { info_[t].model = std::move(model); }
    // Tests / tools: replace a type's R5 decoration fields.
    void set_decorations(objects::TypeIndex t, std::vector<std::string> surface_fx, std::string scorch_mark) {
        info_[t].surface_fx = std::move(surface_fx);
        info_[t].scorch_mark = std::move(scorch_mark);
    }
    void set_ghosts(objects::TypeIndex t, bool last_state, bool initial_state) {
        info_[t].ghost_last_state = last_state;
        info_[t].ghost_initial_state = initial_state;
    }
    void set_max_speed(objects::TypeIndex t, float v) { info_[t].max_speed = v; }
    void set_spin_away_explosion(objects::TypeIndex t, std::string name) { info_[t].spin_away_explosion = std::move(name); }
    void set_projectile_render(objects::TypeIndex t, int32_t custom_render, float width, float length, uint8_t slot_col = 0,
                               uint8_t slot_row = 0) {
        info_[t].projectile_custom_render = custom_render;
        info_[t].projectile_width = width;
        info_[t].projectile_length = length;
        info_[t].projectile_slot[0] = slot_col;
        info_[t].projectile_slot[1] = slot_row;
        if (custom_render == 1 || custom_render == 2) info_[t].model.clear();
    }
    void set_projectile_color(objects::TypeIndex t, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
        info_[t].projectile_color[0] = r;
        info_[t].projectile_color[1] = g;
        info_[t].projectile_color[2] = b;
        info_[t].projectile_color[3] = a;
    }
    // Tests / tools: a type's R5 stealth look (colour R, G, B, A in 0..1, transition time in seconds), Stealth_Ability or
    // Force_Cloak_Ability.
    void set_stealth(objects::TypeIndex t, const float color[4], float seconds, bool force_cloak = false) {
        float* c = force_cloak ? info_[t].cloak_color : info_[t].stealth_color;
        for (int k = 0; k < 4; ++k) c[k] = color[k];
        (force_cloak ? info_[t].has_cloak_color : info_[t].has_stealth_color) = true;
        (force_cloak ? info_[t].cloak_time : info_[t].stealth_time) = seconds;
    }
    const std::array<materials::float4, 9>& palette() const { return palette_; }
    // Type by name (case-insensitive), objects::kNone when unknown.
    objects::TypeIndex find(std::string_view name) const;

private:
    objects::ModeKind mode_ = objects::ModeKind::Space;
    std::vector<SnapshotTypeInfo> info_;
    std::vector<std::pair<std::string, objects::TypeIndex>> by_name_;  // upper-case name, sorted (lookup only)
    std::array<materials::float4, 9> palette_{};
    std::vector<uint8_t> neutral_;                     // per owner index (faction order)
    std::vector<materials::float4> neutral_color_;     // per owner index: the faction's No_Colorization_Color
};

enum SnapshotFlag : uint8_t {
    kSnapDying = 0x01,       // destroyed this tick, flushed at the next store update (fade 0)
    kSnapBackground = 0x02,  // In_Background type on a space map (background scene)
    kSnapDrawable = 0x04,    // the type has a model
    // R5: hidden from the LOCAL player (FOG_OF_WAR §5.4 HideWhenFogged: FOW_Object_Hidden(local, obj, 0), or an enemy
    // that Is_Stealthed). Presentation only: set between ticks by apply_battle_fog / apply_fog_hidden; the instance
    // builder fades such objects out (and back in), their particles stop drawing.
    kSnapFogHidden = 0x08,
    // R3: a detached corpse (the sim removed the store object at death; anim::Corpse keeps type / owner / transform and
    // its AnimWorld row of the same id). Added by add_snapshot_corpses; fade = the corpse alpha.
    kSnapCorpse = 0x10,
    // R3: inside a garrison / transport (not drawn, no particles; the sim keeps the object). Set by apply_contained.
    kSnapContained = 0x20,
    // R3: a spin-away corpse (Spin_Away_On_Death): add_snapshot_corpses applies the render-side spin motion; when it
    // leaves the frame SimDecorations plays the type's Spin_Away_On_Death_Explosion at its last position.
    kSnapSpinAway = 0x40,
    // refix t01: a store death clone (objects::kFlagDeathClone, dying / removing): drawn while it plays its death, but
    // not a HUD unit and never hovered or picked.
    kSnapDeathClone = 0x80,
};

struct SnapshotObject {
    objects::ObjectId id = objects::kNoObjectId;
    objects::TypeIndex type = objects::kNone;
    int32_t owner = -1;
    objects::ObjectId attach_parent = objects::kNoObjectId;  // id of the attach parent (hardpoints), none otherwise
    Vec3 position{};
    Vec3 facing{1, 0, 0};
    float health = 0.0f;
    float health_fraction = 1.0f;  // health / Tactical_Health (1 when the type has none)
    float lod_bias = 4.0f;
    float fade = 1.0f;             // LIGHT_SCALE.w
    materials::float4 team_color{1, 1, 1, 1};
    uint8_t team_index = kNeutralTeam;
    uint8_t flags = 0;
    int8_t alt = 0;                // current model _ALT index (damage state, land and space - see damage_alt_index)
    float shield_fraction = -1.0f; // R5: shield points / max (CombatSystem::shield_fraction); -1 = no shields / unknown
    uint32_t hardpoint_first = 0;  // into SimSnapshot::hardpoint_destroyed (count = the type's hardpoints)
    uint32_t hardpoint_count = 0;
    // Attitude on top of the facing frame (radians; object_transform): roll right-handed about the forward axis (positive
    // = right wing down), pitch right-handed about the left axis (positive = nose down), pitch applied first. Live
    // objects: the movement attitude of fighters / flyers (apply_snapshot_attitude, apply_battle_state; the facing stays
    // the level heading); R3 spin-away corpses: the spin roll. 0 otherwise.
    float roll = 0.0f;
    float pitch = 0.0f;
    // R5: active unit-ability types (bit t of ability_bits[t / 64], AbilitySystem::is_active; apply_battle_state) and the
    // stealth mesh state (kSnapStealthMeshes). Snapshot-to-snapshot changes are this tick's ability events.
    uint64_t ability_bits[2] = {0, 0};
    bool ability_active(uint32_t t) const { return t < 128 && ((ability_bits[t / 64] >> (t % 64)) & 1u) != 0; }
    // R5 stealth fade time override (ABILITY_VISUALS §1.1/§6): seconds of the stealth spring's smooth time while the
    // fade target changes; 0 = the type's Stealth_Transition_Time / Force_Cloak_Transition_In_Secs. apply_battle_state
    // sets kStealthBreakFade (0.3 s, the firing / stun break) when the ability is still active but no longer applied.
    // Presentation only, not in digest().
    float stealth_time = 0.0f;
};
// R5 stealth (ABILITIES_HARDPOINTS §3.7, TACTICAL_GAPS §2.3 Force_Cloak): while applied, meshes named BASEMESH are hidden
// and STEALTH meshes shown. Set by apply_battle_state (AbilitySystem::is_stealthed or an active STEALTH (48) /
// FORCE_CLOAK (54) unit ability [port: the applied flag SAP +0x2a4 itself is not exposed]).
inline constexpr uint8_t kSnapStealthMeshes = 0x80;
inline constexpr uint8_t kAbilityStealth = 48, kAbilityForceCloak = 54;
inline constexpr float kStealthBreakFade = 0.3f;  // spring smooth time of a stealth break (firing / stun / bribe / earthquake)

// R5 ability events of one tick, derived from two consecutive snapshots (snapshot_ability_events): the sim's own
// AbilitySystem events are cleared inside the tick, so the render side sees the state changes.
enum class SnapshotAbilityKind : uint8_t { Activated = 0, Deactivated = 1, StealthOn = 2, StealthOff = 3 };
struct SnapshotAbilityEvent {
    objects::ObjectId id = objects::kNoObjectId;
    uint8_t type = 0;  // unit-ability type (stealth events: 0)
    SnapshotAbilityKind kind = SnapshotAbilityKind::Activated;
};

// A render attachment the sim reports for an object (Int-B: TacticalBattle::attached_effects(), Damaged_Smoke_Asset_Name
// when the hull crossed 75 % / 50 %): the particle system `asset` stays on the object while it exists. Copied into the
// snapshot as plain data (render never holds sim objects).
struct SnapshotEffect {
    objects::ObjectId id = objects::kNoObjectId;
    std::string asset;     // particle ALO name
    uint8_t percent = 75;  // threshold that attached it (75 / 50): one attachment per (object, percent);
                           // R4 ability effects: kEffectAbilityBase + unit-ability type; R5 Lua: kEffectLua
    uint32_t frame = 0;    // battle frame of the report
    // R5 Lua GameObjectWrapper.Attach_Particle_Effect(type [, bone]) (lua::ParticleEffectRequest): seq + 1 of the request
    // (0 = not a Lua effect; one system per request, keyed by it) and the bone the system follows ("" = the root).
    uint32_t lua_seq = 0;
    std::string bone{};
};
// R4: SnapshotEffect::percent of an active unit ability's Particle_Effect (128 + ability type 0..75).
inline constexpr uint8_t kEffectAbilityBase = 128;
// R5: SnapshotEffect::percent of a Lua Attach_Particle_Effect request (with lua_seq != 0).
inline constexpr uint8_t kEffectLua = 255;
// R5: SnapshotEffect::percent of a sensor jammer's Owner_Particle_Effect (ABILITY_VISUALS §3; one system per jammer object).
inline constexpr uint8_t kEffectJamming = 240;

// R5: one hardpoint destruction reported by the sim this tick (CombatSystem::hardpoint_events), in event order. The
// instance builder applies them in this order before the destroyed bits (B.10: the destruction order decides
// overlapping show / hide subtrees); bits alone fall back to HardPoints order.
struct SnapshotHardpointEvent {
    objects::ObjectId id = objects::kNoObjectId;
    uint32_t index = 0;  // HardPoints list index
};

// R3 muzzle flash (ANIM_PARTICLES.md A.7 attack row: muzzle sub-objects per fire bone are shown and re-hidden after 4
// ticks [M]): the sim's muzzle state of one object this tick. The builder shows every mesh / proxy whose own bone is
// `bone` (case-insensitive; e.g. MuzzleA_00 -> MuzzleA_00_Flash, ALO-hidden by default) on the object's model, or on
// its Model_To_Attach of hardpoint `hardpoint` (>= 0) when the fire bone lives there.
// sim::SimAnimation::Muzzle gives a skeleton bone index of the object's model (bone_index) or -1 = "the object's own
// weapon": with neither a name nor an index the builder shows every bone whose name starts with "Muzzle" (MuzzleA_00 ...).
struct SnapshotMuzzle {
    objects::ObjectId id = objects::kNoObjectId;
    std::string bone;
    int32_t hardpoint = -1;
    int32_t bone_index = -1;
};

// R5 foot plant (ANIM / AnimationSFXMaps SURFACE rows: the sim's per-tick foot-plant events). `trigger` = the SurfaceFX
// trigger name when the event carries one (AT_AT_FOOTPRINT, ATAA_FOOTPRINT_LEFT ...), else the decoration code picks
// one from the type's SurfaceFX_Name list; foot 0 = left, 1 = right.
struct SnapshotFootPlant {
    objects::ObjectId id = objects::kNoObjectId;
    uint8_t foot = 0;
    Vec3 position{};
    std::string trigger;
};

// R5 particle effect at a world point (footprint puffs) or attached to an object (SurfaceFX water wakes); keyed so the
// particle attachments keep one system per key while it is listed.
struct SimWorldEffect {
    uint64_t key = 0;
    std::string asset;            // particle ALO name
    objects::ObjectId owner = objects::kNoObjectId;  // != none: follows the object (its transform at sync time)
    Vec3 position{};
    Vec3 facing{1, 0, 0};
    // R4 scalar setter caller (STRATEGIC_GAPS §6.2, SurfaceFX 0x5ee570: index 0 = intensity, smoothing 0.1 s); < 0 = none.
    float intensity = -1.0f;
};

// A live projectile of the battle's CombatSystem (flat table, not store objects): CombatSystem::projectile(i) copied as
// plain data by apply_battle_projectiles. `type` = the GameObjectType of its ProjectileDesc (by name; kNone = unknown).
struct SnapshotProjectile {
    uint32_t serial = 0;          // CombatSystem projectile serial (stable for the projectile's life; interpolation key)
    uint32_t desc = 0;            // ProjectileDesc index
    objects::TypeIndex type = objects::kNone;
    int32_t owner = -1;
    objects::ObjectId shooter = objects::kNoObjectId;
    Vec3 position{};
    Vec3 direction{1, 0, 0};      // normalised flight direction
    float speed = 0.0f;           // units per sim tick (position advances by direction x speed)
};

// R5 sensor jamming (ABILITY_VISUALS §3, presentation only; filled by apply_battle_jamming, jam_fx.hpp draws it).
// One record per object with an applied Sensor_Jamming_Ability that has NO Owner_Particle_Effect (that one is a
// SnapshotEffect of percent kEffectJamming instead): the radius blob of side 2 x radius centred on the jammer.
struct SnapshotJammer {
    objects::ObjectId id = objects::kNoObjectId;  // the jammer object (its position comes from the frame's object)
    int32_t owner = -1;
    float radius = 0.0f;           // the radius the sim jams with (unit ability 63 Effective_Radius / Passive_Missile_Shield_Radius)
    bool allied = false;           // the jammer's owner is the local player or not its enemy (when the snapshot was filled)
    bool show_to_enemies = false;  // Show_Blob_To_Enemies
    std::string blob_material;     // Blob_Material_Name (ShadowBlobMaterials.xml; "" = Sensor_Jamming_Blob [M])
    float blob_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // Blob_Color 0..1
};
struct SnapshotJamming {
    std::vector<SnapshotJammer> jammers;       // ascending jammer id
    std::vector<objects::ObjectId> jammed;     // objects with Is_Jammed (CombatSystem::is_jammed), ascending id
    bool is_jammed(objects::ObjectId id) const { return std::binary_search(jammed.begin(), jammed.end(), id); }
    // Radar filter 0x35b4d0 (ABILITY_VISUALS 1.4): objects hostile to the local player that are Is_Stealthed (not detected),
    // ascending id. Their radar blip is dropped like a jammed one.
    std::vector<objects::ObjectId> stealthed;
    bool is_stealthed(objects::ObjectId id) const { return std::binary_search(stealthed.begin(), stealthed.end(), id); }
};

// ---- R5 ability beams (ABILITY_VISUALS §2, §4, §6 finding 1; presentation only; filled by apply_battle_beams /
// apply_battle_state, resolved to world endpoints by sim_beams.hpp, drawn by Renderer::set_beams): one record per active
// tractor beam (30), energy weapon (31), super laser (55, firing), laser-defense zap (56, this tick's) and leech shields
// (58). The ability's beam fields are already resolved to texture / width / colour / frames over the GC fallbacks (4.2).
struct SnapshotBeam {
    uint8_t kind = 0;                                       // kBeamTractor / kBeamEnergyWeapon / kBeamSuperLaser / kBeamLaserZap / kBeamLeech
    objects::ObjectId owner = objects::kNoObjectId;         // the beam's source object (start bone)
    objects::ObjectId target = objects::kNoObjectId;        // far end object (centre + Ranged_Target_Z_Adjust, or its hardpoint bone)
    bool fixed_end = false;                                 // zap / continuation: `end` is the far end, `target` is not used
    Vec3 end{};
    int32_t hardpoint = -1;                                 // energy weapon: the targeted hardpoint (HardPoints index), -1 = the centre
    float end_z_adjust = 0.0f;                              // the target type's Ranged_Target_Z_Adjust (added to the centre)
    uint32_t frame = 0;                                     // battle frame of the record (zap identity)
    BeamStyle style;
};

struct SimSnapshot {
    uint64_t tick = 0;       // SimHost global frame of the finished tick
    uint64_t mode_frame = 0;  // the tactical mode's frame
    objects::ModeKind mode = objects::ModeKind::Space;
    std::vector<SnapshotObject> objects;       // ascending id
    std::vector<uint8_t> hardpoint_destroyed;  // 1 = destroyed, ranges per object
    std::vector<SnapshotEffect> effects;       // attached effects of objects in `objects`: ascending id, report order per id
    std::vector<SnapshotHardpointEvent> hardpoint_events;  // R5: this tick's destructions, event order
    std::shared_ptr<const FogFrameInput> fog;  // R5: the local player's FOW grid (null = no fog of war)
    std::vector<SnapshotMuzzle> muzzles;       // R3: muzzles showing this tick (ascending id)
    std::vector<SnapshotFootPlant> foot_plants;  // R5: this tick's foot plants (event order)
    std::vector<SnapshotAbilityEvent> ability_events;  // R5: changes against the previous snapshot (ascending id, type)
    std::vector<SnapshotProjectile> projectiles;  // live projectiles (ascending serial; apply_battle_projectiles)
    std::vector<SnapshotBeam> beams;              // R5: ability beams (ascending owner id, then kind; apply_battle_beams)
    SnapshotJamming jamming;                      // R5: sensor jamming (apply_battle_jamming)
    uint32_t store_objects = 0;                // objects in the store (incl. not drawable / dying)
    const SnapshotObject* find(objects::ObjectId id) const;  // binary search, null if absent
    // Ordered digest of every field (thread-determinism tests).
    uint64_t digest() const;
};

struct SnapshotOptions {
    bool drawable_only = true;  // skip objects whose type has no model
    uint32_t grain = 128;       // ParallelFor grain
};

// Builds `out` from a store (const access only). `table` must have been built for the store's types.
void build_snapshot(const objects::ObjectStore& store, uint64_t tick, uint64_t mode_frame, const SnapshotTypeTable& table,
                    SimSnapshot& out, const ParallelFor& pf = serial_for, const SnapshotOptions& options = {});
// The top tactical mode of a host (false and `out` emptied when there is none).
bool capture_snapshot(const sim::SimHost& host, const SnapshotTypeTable& table, SimSnapshot& out,
                      const ParallelFor& pf = serial_for, const SnapshotOptions& options = {});

// Int-B consumers (after build / capture, before publish). Effects: entries with an empty asset or whose object is not
// in `snap.objects` are dropped; the rest are stored by ascending id (stable: report order kept per id).
void set_snapshot_effects(SimSnapshot& snap, std::span<const SnapshotEffect> effects);
// The same rule, appended to the effects already set (kept ascending by id, stable).
void append_snapshot_effects(SimSnapshot& snap, std::span<const SnapshotEffect> effects);
// R5 Lua Attach_Particle_Effect: lua::ParticleEffectRequest-like records (fields frame, seq, object, type, type_name,
// bone) made on or after `since_frame` (the battle's first global frame: earlier requests name other modes' object ids)
// -> SnapshotEffects (percent kEffectLua, lua_seq = seq + 1, bone). Asset = the type's model in `table` (by index when
// it names the same type, else by name), else the type name itself (an ALO name). Header template: render does not
// link engine/lua.
template <class RequestRange>
std::vector<SnapshotEffect> snapshot_lua_effects(const RequestRange& requests, const SnapshotTypeTable& table, uint64_t since_frame) {
    std::vector<SnapshotEffect> out;
    for (const auto& r : requests) {
        if (static_cast<uint64_t>(r.frame) < since_frame) continue;
        SnapshotEffect e;
        e.id = static_cast<objects::ObjectId>(r.object);
        e.percent = kEffectLua;
        e.frame = static_cast<uint32_t>(r.frame);
        e.lua_seq = static_cast<uint32_t>(r.seq) + 1u;
        e.bone = r.bone;
        objects::TypeIndex t = table.find(r.type_name);
        if (t == objects::kNone && r.type >= 0 && table.valid(static_cast<objects::TypeIndex>(r.type))) t = static_cast<objects::TypeIndex>(r.type);
        e.asset = t != objects::kNone && table.valid(t) && !table[t].model.empty() ? table[t].model : std::string(r.type_name);
        if (!e.asset.empty()) out.push_back(std::move(e));
    }
    return out;
}
// Destroyed hardpoints from a source that knows them per (object, HardPoints index) - the battle's CombatSystem, whose
// hardpoints are flat sub-records (no sub-objects for the PROVISIONAL rule): OR'd into snap.hardpoint_destroyed for
// every object with `count(id)` == its type's hardpoint count (other counts: a different list, left alone).
void apply_hardpoint_source(SimSnapshot& snap, const std::function<uint32_t(objects::ObjectId)>& count,
                            const std::function<bool(objects::ObjectId, uint32_t)>& destroyed);
// S7 t05: the movement attitude of live objects (FIGHTER_FLYING §1.1: fighter roll / pitch, flyer bank / pitch, bike
// lean) from a source that knows it per object id (TacticalBattle::unit_attitude: radians, SnapshotObject::roll / pitch
// conventions). Every object that is not a corpse (kSnapCorpse) and for which `source` returns true gets roll / pitch
// (non-finite values -> 0); the others keep theirs. Null source: no-op.
void apply_snapshot_attitude(SimSnapshot& snap, const std::function<bool(objects::ObjectId, float& roll, float& pitch)>& source);
// R5: this tick's hardpoint destructions (event order); entries whose object is not in `snap.objects` or whose index is
// out of its type's hardpoint range are dropped. Also ORs them into snap.hardpoint_destroyed.
void set_snapshot_hardpoint_events(SimSnapshot& snap, std::span<const SnapshotHardpointEvent> events);
// R5: marks objects hidden from the local player (kSnapFogHidden) with a host predicate (null clears every mark).
void apply_fog_hidden(SimSnapshot& snap, const std::function<bool(const SnapshotObject&)>& hidden);
// R3: marks garrisoned / transported objects (kSnapContained) with a host predicate (null clears every mark).
void apply_contained(SimSnapshot& snap, const std::function<bool(const SnapshotObject&)>& contained);
// R3: this tick's muzzle state (entries of objects not in the snapshot or with an empty bone dropped; sorted by id,
// input order kept per id).
void set_snapshot_muzzles(SimSnapshot& snap, std::span<const SnapshotMuzzle> muzzles);
// R5: this tick's foot plants (entries of objects not in the snapshot dropped; event order kept).
void set_snapshot_foot_plants(SimSnapshot& snap, std::span<const SnapshotFootPlant> plants);
// R5: snap.ability_events = the ability state changes of `snap` against `prev` (the previously published snapshot of
// the same mode; null = none): per object in both (same type) every unit-ability bit that turned on (Activated) / off
// (Deactivated) in type order, then StealthOn / StealthOff; objects new in `snap` report their active bits as
// Activated. Ascending id.
void set_snapshot_ability_events(SimSnapshot& snap, const SimSnapshot* prev);
// sim::SimAnimation::muzzles(mode) records (fields id, bone, until) still showing at `frame` (frame < until) ->
// SnapshotMuzzles (bone index, -1 = the object's own Muzzle* bones). Header template: render does not link the sim here.
template <class MuzzleRange>
std::vector<SnapshotMuzzle> snapshot_muzzles(const MuzzleRange& muzzles, uint64_t frame) {
    std::vector<SnapshotMuzzle> out;
    for (const auto& m : muzzles) {
        if (!(frame < m.until)) continue;
        SnapshotMuzzle s;
        s.id = static_cast<objects::ObjectId>(m.id);
        s.bone_index = static_cast<int32_t>(m.bone);
        // Int-final: records with a hardpoint field (SimAnimation::Muzzle): a fire bone the sim resolved in the object's
        // skeleton stays a bone-index muzzle on the object; an unresolved one of hardpoint k is placed on k's attached
        // model (Model_To_Attach: its Muzzle* bones, sim_view.cpp).
        if constexpr (requires { m.hardpoint; })
            if (m.bone < 0 && m.hardpoint >= 0) s.hardpoint = static_cast<int32_t>(m.hardpoint);
        out.push_back(s);
    }
    return out;
}
// sim::SimAnimation::events(mode) records (fields id, def -> kind / name / bone, position) -> SnapshotFootPlants: the
// SURFACE events (def->kind == 1), trigger = the SurfaceFX name, foot = 1 when the bone name ends in "R" (B_Foot_R), else 0.
template <class EventRange>
std::vector<SnapshotFootPlant> snapshot_foot_plants(const EventRange& events) {
    std::vector<SnapshotFootPlant> out;
    for (const auto& e : events) {
        if (!e.def || static_cast<int>(e.def->kind) != 1) continue;
        SnapshotFootPlant f;
        f.id = static_cast<objects::ObjectId>(e.id);
        const std::string& b = e.def->bone;
        f.foot = !b.empty() && (b.back() == 'R' || b.back() == 'r') ? uint8_t{1} : uint8_t{0};
        f.position = {e.position.x, e.position.y, e.position.z};
        f.trigger = e.def->name;
        out.push_back(std::move(f));
    }
    return out;
}
#ifdef ALAMO_SIM_HAVE_BATTLES
// Both of the above from a running battle (snapshot captured from the battle's tactical mode): attached_effects(),
// CombatSystem::hardpoint_count / hardpoint_destroyed and (R5) this tick's CombatSystem::hardpoint_events, and (S7 t05)
// the movement attitude of every mover (TacticalBattle::unit_attitude through apply_snapshot_attitude). With `table`
// (R4) also the Particle_Effect of every active unit ability (AbilitySystem::is_active + record): the name is resolved as
// a GameObjectType (its model, e.g. a Particles.xml type) and used as an ALO name otherwise; one effect per (object,
// ability type), SnapshotEffect::percent = kEffectAbilityBase + type. Const reads between ticks only.
void apply_battle_state(const sim::TacticalBattle& battle, SimSnapshot& snap, const SnapshotTypeTable* table = nullptr,
                        const BeamStyleTable* beam_styles = nullptr);
// R5 ability beams (ABILITY_VISUALS §2 / §4 / §6.1): snap.beams from the battle - one record per applied Tractor_Beam (30),
// Energy_Weapon (31; target hardpoint = the ordered one, else the best, CombatSystem::choose_target_hardpoint), Super_Laser
// (55, once its active phase runs) and Leech_Shields (58) special; a Laser_Defense (56) zap only while the sim's
// AbilityEvent::Zap is still pending (the host's tick clears it: use apply_battle_zaps). The beam fields come from `styles`
// (the ability XML over the GameConstants fallbacks; null = the GC-only defaults). Const reads only. apply_battle_state calls it.
void apply_battle_beams(const sim::TacticalBattle& battle, SimSnapshot& snap, const BeamStyleTable* styles = nullptr);
// R5 laser-defense zaps (kind 56, fixed end) of this tick from `prev` (the previously captured snapshot, null = none) and
// snap.projectiles: call AFTER apply_battle_projectiles. See apply_beam_zaps (sim_beams.hpp).
void apply_battle_zaps(const sim::TacticalBattle& battle, SimSnapshot& snap, const SimSnapshot* prev, const BeamStyleTable* styles = nullptr);
// The battle's live projectiles (CombatSystem::projectile(i), alive ones) into snap.projectiles, ascending serial. The
// type of each ProjectileDesc is the table's type of the same name (ProjectileDesc::name = the GameObjectType name the
// battle catalog built it from), resolved once per call; unknown names keep kNone (not drawn). Const reads only.
void apply_battle_projectiles(const sim::TacticalBattle& battle, SimSnapshot& snap, const SnapshotTypeTable& table);
// R5 sensor jamming (ABILITY_VISUALS §3): snap.jamming from the battle (AbilitySystem::jamming_special, CombatSystem::is_jammed;
// const reads between ticks, call after apply_battle_state / apply_battle_fog). `local_player` decides SnapshotJammer::allied
// (CombatSystem::is_enemy). A jammer with an Owner_Particle_Effect adds a SnapshotEffect (percent kEffectJamming, bone = the
// first Owner_Particle_Bone_Name, asset resolved through `table` like the R4 ability effects) and no SnapshotJammer.
void apply_battle_jamming(const sim::TacticalBattle& battle, int32_t local_player, SimSnapshot& snap, const SnapshotTypeTable* table = nullptr);
#endif
void sort_snapshot_projectiles(SimSnapshot& snap);  // ascending serial (stable)
// The same from any projectile view list (tests / other hosts): records with fields serial, desc, owner, shooter, pos,
// dir, speed, alive; `desc_types[desc]` gives the type (out of range -> kNone).
template <class ViewRange>
void set_snapshot_projectiles(SimSnapshot& snap, const ViewRange& views, std::span<const objects::TypeIndex> desc_types) {
    snap.projectiles.clear();
    for (const auto& v : views) {
        if (!v.alive) continue;
        SnapshotProjectile p;
        p.serial = v.serial;
        p.desc = v.desc;
        p.type = v.desc < desc_types.size() ? desc_types[v.desc] : objects::kNone;
        p.owner = v.owner;
        p.shooter = static_cast<objects::ObjectId>(v.shooter);
        p.position = {v.pos.x, v.pos.y, v.pos.z};
        p.direction = {v.dir.x, v.dir.y, v.dir.z};
        p.speed = v.speed;
        snap.projectiles.push_back(p);
    }
    sort_snapshot_projectiles(snap);
}
#if defined(ALAMO_SIM_HAVE_BATTLES) && defined(ALAMO_SIM_HAVE_FOG)
// R5: the battle's fog of war for `local_player` (FogOfWar::render_view copied into snap.fog; null when the battle has
// no fog or the player no grid) and kSnapFogHidden on every object TacticalBattle::fog_object_hidden(local, id, false)
// reports hidden, or that is an enemy (not FogOfWar::allied) of the local player and AbilitySystem::is_stealthed (FOG_OF_WAR
// §5.4). The local player must be marked human in the fog (FogOfWar::set_human) - non-human players are never fogged
// (§5.1 AI gate). Const reads between ticks only; nothing in the sim changes.
void apply_battle_fog(const sim::TacticalBattle& battle, int32_t local_player, SimSnapshot& snap);
// R5: the space reinforcement overlay of `local_player` (TacticalBattle::fog_reinforce_overlay, TACTICAL_GAPS §5.5) into
// snap.fog->reinforce (the fog input is copied once). No-op without a fog input or when the battle samples nothing. Call
// it after apply_battle_fog, only while the overlay is shown (host: placing a space reinforcement).
void apply_battle_fog_reinforce(const sim::TacticalBattle& battle, int32_t local_player, SimSnapshot& snap);
#endif
// R3 detached corpses (sim::SimAnimation::corpses(mode), anim::Corpse with detached = true): plain render records.
struct SnapshotCorpse {
    objects::ObjectId id = objects::kNoObjectId;
    objects::TypeIndex type = objects::kNone;
    int32_t owner = -1;
    Vec3 position{};
    Vec3 facing{1, 0, 0};
    float alpha = 1.0f;  // anim::Corpse::alpha(frame): 1 until the fade, then down to 0
    float roll = 0.0f;   // radians about the facing axis (0 when the record has none)
    bool spin_away = false;      // Corpse::spin_away
    float spin_seconds = 0.0f;   // seconds since death (spin-away motion)
};
// R3 spin-away motion [port: the original's spin is untraced (ABILITIES_HARDPOINTS §4.9 open item)]: the corpse rolls
// about its facing at kSpinAwayRollRate, yaws at kSpinAwayYawRate and drifts along its death facing at kSpinAwayDrift
// units / s from the death position. Visual only.
inline constexpr float kSpinAwayRollRate = 3.0f, kSpinAwayYawRate = 0.6f, kSpinAwayDrift = 40.0f;
// Appends each corpse as a SnapshotObject (kSnapCorpse, fade = alpha, health 0, alt = the type's damage alternate at
// health 0, team colour of the owner, no hardpoints) and re-sorts by id. Skipped: alpha <= 0, an unknown / model-less
// type (with SnapshotOptions::drawable_only semantics), or an id already in the snapshot (a corpse whose store object
// still exists is drawn through it). The instance builder then draws it with the pose source's pose of the same id
// (the AnimWorld row the sim keeps for the corpse: AnimPoseCache covers it) and the fade as LIGHT_SCALE.w.
void add_snapshot_corpses(SimSnapshot& snap, const SnapshotTypeTable& table, std::span<const SnapshotCorpse> corpses);
// Converts anim::Corpse-like records (fields id, detached, info.object_type / owner / position[3] / facing[3],
// alpha(frame)) into SnapshotCorpses: detached ones only. Header template so render does not link engine/anim here.
template <class CorpseRange>
std::vector<SnapshotCorpse> snapshot_corpses(const CorpseRange& corpses, uint64_t frame) {
    std::vector<SnapshotCorpse> out;
    for (const auto& c : corpses) {
        if (!c.detached) continue;
        SnapshotCorpse s;
        s.id = static_cast<objects::ObjectId>(c.id);
        s.type = static_cast<objects::TypeIndex>(c.info.object_type);
        s.owner = c.info.owner;
        s.position = {c.info.position[0], c.info.position[1], c.info.position[2]};
        s.facing = {c.info.facing[0], c.info.facing[1], c.info.facing[2]};
        s.alpha = c.alpha(frame);
        if constexpr (requires { c.info.roll; }) s.roll = static_cast<float>(c.info.roll);  // when the record carries one
        if constexpr (requires { c.spin_away; c.death_frame; }) {
            s.spin_away = c.spin_away;
            if (c.spin_away && frame > c.death_frame) s.spin_seconds = static_cast<float>(static_cast<double>(frame - c.death_frame) / 30.0);
        }
        out.push_back(s);
    }
    return out;
}

// Plain copy of a fog view into a FogFrameInput (any source with the FogRenderView field names).
template <class View>
std::shared_ptr<const FogFrameInput> fog_frame_input(const View& v) {
    if (!v.valid || v.width <= 0 || v.height <= 0) return nullptr;
    auto f = std::make_shared<FogFrameInput>();
    f->valid = true;
    f->player = v.player;
    f->width = static_cast<uint32_t>(v.width);
    f->height = static_cast<uint32_t>(v.height);
    f->x0 = v.box.x0;
    f->y0 = v.box.y0;
    f->x1 = v.box.x1;
    f->y1 = v.box.y1;
    f->cell_w = v.cell_w;
    f->cell_h = v.cell_h;
    f->state.assign(v.state.begin(), v.state.end());
    f->fade.assign(v.fade.begin(), v.fade.end());
    f->frame = v.frame;
    f->last_decay = v.last_decay;
    f->regrow_step = v.regrow_step;
    f->fow_active = v.fow_active;
    f->render_option = v.render_option;
    f->setup_phase = v.setup_phase;
    f->reveal_all = v.reveal_all;
    return f;
}

// Double buffer of immutable snapshots (previous + current). publish() is the producer's ordered commit
// (end of a tick); latest() hands the consumer both pointers; snapshots are never modified after publish.
class SnapshotBuffer {
public:
    struct Pair {
        std::shared_ptr<const SimSnapshot> prev, curr;
    };
    void publish(std::shared_ptr<const SimSnapshot> s);
    Pair latest() const;
    void clear();
    uint64_t published() const;

private:
    mutable std::mutex mutex_;
    Pair pair_;
    uint64_t published_ = 0;
};

enum class InterpMode : uint8_t { Original = 0, Smooth = 1 };
const char* interp_mode_name(InterpMode m);

struct InterpolatedFrame {
    uint64_t tick = 0;  // current snapshot's tick
    float alpha = 1.0f;
    InterpMode mode = InterpMode::Original;
    objects::ModeKind sim_mode = objects::ModeKind::Space;
    std::vector<SnapshotObject> objects;       // ascending id
    std::vector<uint8_t> hardpoint_destroyed;  // the current snapshot's
    std::vector<SnapshotEffect> effects;       // the current snapshot's
    std::vector<SnapshotHardpointEvent> hardpoint_events;  // the current snapshot's (R5)
    std::shared_ptr<const FogFrameInput> fog;  // the current snapshot's (R5; shared, immutable)
    std::vector<SnapshotMuzzle> muzzles;       // the current snapshot's (R3)
    std::vector<SnapshotFootPlant> foot_plants;  // the current snapshot's (R5)
    std::vector<SnapshotAbilityEvent> ability_events;  // the current snapshot's (R5)
    // Projectiles of the current snapshot (ascending serial). Smooth: a projectile also in prev is placed at
    // lerp(prev, curr, alpha); one new this tick appears at its current position (like objects).
    // Original: the current positions.
    std::vector<SnapshotProjectile> projectiles;
    std::vector<SnapshotBeam> beams;  // the current snapshot's (R5 ability beams)
    SnapshotJamming jamming;  // the current snapshot's (R5)
};

// Frame view between prev (may be null) and curr at `alpha` in [0, 1] (clamped; non-finite -> 1).
void interpolate(const SimSnapshot* prev, const SimSnapshot& curr, float alpha, InterpMode mode, InterpolatedFrame& out);

// Tick alpha of a frame at wall time now_ms when the last tick ran at last_tick_ms with ticks every
// period_ms (32-bit wrapping milliseconds like the pacing): (now - last) / period clamped to [0, 1].
float tick_alpha(uint32_t now_ms, uint32_t last_tick_ms, uint32_t period_ms);

// Interpolation primitives (tests).
float interp_lerp(float a, float b, float t);  // a + (b - a) * t
Vec3 interp_lerp(const Vec3& a, const Vec3& b, float t);
Vec3 interp_facing(const Vec3& a, const Vec3& b, float t);  // normalised lerp; degenerate -> b
// Angle lerp (radians) along the shorter way: a + wrap(b - a) x t with wrap into (-pi, pi] (exactly b - a when it is
// already in range, so small steps equal interp_lerp). Roll / pitch of SnapshotObject. Smooth mode: an object whose
// facing turns by more than 90 degrees in one tick while it carries a pitch (a fighter's pitch fold: yaw + 180, roll +
// 180) takes the current facing / roll / pitch unblended.
float interp_angle(float a, float b, float t);

// Alamo models are authored nose along model -Y, left along +X, up +Z (ALO hardpoint / muzzle / engine bones of the
// ships, vehicles, fighters and projectiles; docs/formats/SPEC.md 2.2). kIdentityFacing is the facing whose
// object_transform has the identity rotation (a model as authored).
inline constexpr Vec3 kIdentityFacing{0.0f, -1.0f, 0.0f};
// Model -> world of a sim object: the model's nose (-Y) along the facing (normalised; zero -> kIdentityFacing), its
// up (+Z) = world up made orthogonal to the facing (facing straight up -> +Y as the reference), its +X = up x facing
// (the object's left), uniform `scale`, translation = position. For the facing (sin yaw, -cos yaw, 0) this equals
// terrain::placement_transform with rotation (0, 0, yaw) - a map placement's yaw rotates the model as authored.
assets::Mat3x4 object_transform(const Vec3& position, const Vec3& facing, float scale);
// R3: the same with a roll of `roll` radians about the facing axis applied first (spin-away corpses). roll = 0 gives
// exactly object_transform.
assets::Mat3x4 object_transform(const Vec3& position, const Vec3& facing, float scale, float roll);
// S7 t05: the same with a pitch of `pitch` radians about the object's left axis (positive = nose down: forward' = f cos
// - u sin, up' = u cos + f sin) and then the roll about the pitched forward axis. With the level facing (cos Y, sin Y, 0)
// the nose points along FIGHTER_FLYING §1.1 forward (cos Y cos P, sin Y cos P, -sin P). pitch = 0 gives exactly the
// roll-only overload.
assets::Mat3x4 object_transform(const Vec3& position, const Vec3& facing, float scale, float roll, float pitch);
// Model -> world of a snapshot object: position, facing, roll and pitch with `scale` (the type's Scale_Factor).
assets::Mat3x4 object_transform(const SnapshotObject& o, float scale);

}  // namespace alamo::render

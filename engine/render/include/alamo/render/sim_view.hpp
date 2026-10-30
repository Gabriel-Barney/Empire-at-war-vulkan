// alamo_render - R1 snapshot -> render::Instance (space and land), the camera-input interface and the
// streamed models of sim object types. Render-side only: everything here reads an InterpolatedFrame
// (sim_snapshot.hpp) and never sees the sim.
//
// Per object of the frame (ascending id): model of its type (SnapshotTypeTable, the same GameTypeResolver
// models MapScene draws for placements), transform = object_transform(position, facing, Scale_Factor),
// LIGHT_SCALE.w = fade (objects at fade <= 0 are not drawn), team colour override (B.3), LOD_Bias, hidden
// sub-objects = HardpointVisibility over the type's hardpoint layout (B.9 / B.10) with the frame's destroyed
// hardpoints applied (sticky: hardpoint visuals are never repaired), Model_To_Attach instances at their
// Attachment_Bone after the owner, pose from set_pose_source (R3 animation; bind pose without one). In_Background objects of a space map go
// to the background list (draw with Renderer::render's `sky` + RecordOptions::space, like MapScene::sky()).
// Projectiles of the frame (ascending serial) follow the objects in the main list: projectile_transform, not fogged
// (projectile_fogged), no team colour / hardpoints / pose.
// Per-object render state (HardpointVisibility) is keyed by object id in an ordered map and dropped when the
// object leaves the frame; Instance::mesh_hidden spans point into it and stay valid until the next build().
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "alamo/render/camera.hpp"
#include "alamo/render/hardpoints.hpp"
#include "alamo/render/map_scene.hpp"
#include "alamo/render/renderer.hpp"
#include "alamo/render/sim_snapshot.hpp"
#include "alamo/render/stealth_fx.hpp"

namespace alamo::render {

// Where a frame's models come from (ModelStreamer in the app, fixed models in tests). Frame thread only.
class SimModelSource {
public:
    virtual ~SimModelSource() = default;
    // Drawable model of a type (null: no model, still loading or failed).
    virtual const GpuModel* type_model(objects::TypeIndex type) = 0;
    // A Model_To_Attach model by name (null while loading / failed).
    virtual const GpuModel* named_model(const std::string& name) = 0;
};

// SimModelSource over a ModelStreamer: one request per distinct model name (case-insensitive), issued the
// first time a type / name is asked for (types in frame order = ascending object id: deterministic).
class StreamedSimModels final : public SimModelSource {
public:
    StreamedSimModels(const SnapshotTypeTable& table, ModelStreamer& streamer) : table_(&table), streamer_(&streamer) {}
    const GpuModel* type_model(objects::TypeIndex type) override;
    const GpuModel* named_model(const std::string& name) override;
    // Requests the models of every type in `frame` (call once per published snapshot).
    void request(const InterpolatedFrame& frame);
    uint32_t requested() const { return static_cast<uint32_t>(handles_.size()); }
    uint32_t ready() const;
    uint32_t failed() const;
    bool all_finished() const { return ready() + failed() == requested(); }

private:
    uint32_t handle(const std::string& name);
    const SnapshotTypeTable* table_;
    ModelStreamer* streamer_;
    std::unordered_map<std::string, uint32_t> by_name_;  // upper-cased name -> streamer handle (lookup only)
    std::vector<uint32_t> handles_;                       // request order
};

struct SimInstanceStats {
    uint32_t objects = 0, drawn = 0, background = 0, attachments = 0;
    uint32_t no_model = 0, faded_out = 0, hardpoints_destroyed = 0;
    uint32_t fog_hidden = 0;        // R5: kSnapFogHidden objects of the frame
    uint32_t fog_faded_out = 0;     // R5: not drawn because their fog alpha fell below kFogHideAlpha
    uint32_t alt_nonzero = 0;       // R4: drawn with a damage alternate (Instance::alt != 0)
    uint32_t bone_hidden_meshes = 0;  // R3: meshes hidden by the clip's per-bone visibility
    uint32_t hardpoint_events = 0;  // R5: hardpoint destructions applied from the frame's events
    uint32_t contained = 0;         // R3: kSnapContained objects (not drawn)
    uint32_t ghosts = 0;            // R5: last-seen ghosts drawn this frame
    uint32_t muzzle_meshes = 0;     // R3: sub-objects shown by muzzle state
    uint32_t attachment_posed = 0;  // R3 T6: Model_To_Attach instances posed by the scene-model source
    uint32_t stealthed = 0;         // R5: drawn with the stealth mesh swap (kSnapStealthMeshes)
    uint32_t stealth_tinted = 0;    // R5: drawn with the Stealth_Color multiply (spring value > 0.01)
    uint32_t stealth_hardpoints_hidden = 0;  // R5: hardpoint meshes / attached models hidden by stealth
    uint32_t stealth_no_shadow = 0; // R5: drawn without the shadow caster (stealth shadow bit lost)
    uint32_t projectiles = 0;       // projectiles in the frame
    uint32_t projectiles_drawn = 0;  // ... drawn (model ready or laser, not fogged)
    uint32_t projectiles_lasers = 0; // ... of them procedural lasers (beam / kite)
    uint32_t projectiles_fogged = 0;  // ... hidden by the local player's fog (HIDE_WHEN_FOGGED)
};

class SimInstanceBuilder {
public:
    // `resolver` (optional) supplies the hardpoint records (Damage_Decal, Model_To_Attach, ...) of each type;
    // without it hidden meshes follow the model's default mask (name rule) and nothing is attached.
    explicit SimInstanceBuilder(const SnapshotTypeTable& table, const GameTypeResolver* resolver = nullptr)
        : table_(&table), resolver_(resolver) {}
    // Instances of `frame` (main list, then In_Background objects into `background` on space maps).
    void build(const InterpolatedFrame& frame, SimModelSource& models, std::vector<Instance>& main,
               std::vector<Instance>& background, SimInstanceStats* stats = nullptr);
    void set_show_damage(bool on) { show_damage_ = on; }  // alamo_render --show-damage
    // Laser projectiles of the last build() (projectile_laser, ascending serial, fogged ones left out): hand them to
    // Renderer::set_lasers.
    std::span<const Laser> lasers() const { return lasers_; }
    // R3 pose hook: model-space bone matrices of an object's current pose for its drawn model (per skeleton bone,
    // Instance::bone_world; empty span = bind pose). Called once per drawn object in frame order; the span must
    // stay valid until the frame's render. Model_To_Attach attachments follow the posed bone. Unset = bind pose.
    using PoseSource = std::function<std::span<const Mat3x4>(objects::ObjectId id, const GpuModel& model)>;
    void set_pose_source(PoseSource fn) { pose_ = std::move(fn); }
    // R3 per-bone visibility of the object's current clip (ANIM_PARTICLES.md A.6: every sub-object takes the "animated
    // visible" bit of its bone; in a crossfade a bone is visible if either clip says so). One byte per skeleton bone
    // (0 = hidden), empty = everything visible. A mesh is drawn only when its hardpoint bit is clear AND its bone is
    // visible. Same call rules as the pose source.
    using VisibilitySource = std::function<std::span<const uint8_t>(objects::ObjectId id, const GpuModel& model)>;
    void set_visibility_source(VisibilitySource fn) { visibility_ = std::move(fn); }
    // R5 HideWhenFogged presentation (ABILITY_VISUALS 1.4, 0x53ddc0): kSnapFogHidden objects fade out with the critically
    // damped spring of smooth time kFogSpringTime (0.25 s: omega 8 / s), stepped once per new sim tick at dt = 1/30, and
    // are not drawn once their alpha is below kFogHideAlpha (0.025). Types with Last_State_Visible_Under_FOW (type +0x88)
    // switch instantly. `seconds` 0 = instant for every type. Objects new in a frame start at their target.
    static constexpr float kFogHideAlpha = 0.025f;
    void set_fog_smooth_time(float seconds) { fog_smooth_time_ = seconds; }
    float fog_alpha(objects::ObjectId id) const;  // 1 when unknown
    // R5 stealth presentation (ABILITY_VISUALS 1.1-1.3): per object a critically damped spring 0 -> 1 over the type's
    // Stealth_Transition_Time (or SnapshotObject::stealth_time) towards the fade target (kSnapStealthMeshes: 1, else 0),
    // stepped once per new sim tick at dt = 1/30. While > 0.01 the model's LIGHT_SCALE is multiplied by lerp(1, Stealth_Color,
    // v); while the flag is set, hardpoint meshes / Model_To_Attach models are hidden and the shadow caster is dropped (a
    // Stealth_Ability object keeps that until it leaves the frame; Force_Cloak gets it back at v < 0.01). 0 when unknown.
    float stealth_value(objects::ObjectId id) const;
    bool stealth_shadow_dropped(objects::ObjectId id) const;
    // R1 (TACTICAL_GAPS §6.3, 0x3ac290): the _ALT index the model shows. SnapshotObject::alt is applied only while it is
    // <= the model's highest ALT index (GpuModel::max_alt; 0 always applies); otherwise the previous applied one stays.
    // `fallback` when the object is unknown (not built yet).
    int applied_alt(objects::ObjectId id, int fallback) const;
    // Render-side state of an object (null if it was not in the last frame).
    const HardpointVisibility* hardpoint_state(objects::ObjectId id) const;
    // Effective per-mesh hidden mask of the last build (hardpoints + bone visibility + muzzles); empty if none.
    std::span<const uint8_t> mesh_mask(objects::ObjectId id) const;
    size_t tracked_objects() const { return states_.size(); }

    // R3 T6 scene models (ANIM_PARTICLES.md A.5: non-object scene models are advanced in tick step T6 with t = frame /
    // 30): the Model_To_Attach models of hardpoints play their own idle clip. Source = model-space pose of `model` at
    // sim tick `tick` (e.g. SceneModelAnimator::pose); empty = bind pose. Unset = bind pose (as before).
    using AttachmentPoseSource = std::function<std::span<const Mat3x4>(const GpuModel& model, uint64_t tick)>;
    void set_attachment_pose_source(AttachmentPoseSource fn) { attach_pose_ = std::move(fn); }

    // R5 fogged-model manager (FOG_OF_WAR §5.4 / open item 5; rules [port], the manager 0x4ec2e0 is untraced): with a fog
    // grid in the frame, an object of a type with Last_State_Visible_Under_FOW that becomes kSnapFogHidden leaves a
    // last-seen ghost (its last visible position, facing, damage alternate, hardpoint mask, team colour; bind pose, full
    // alpha); a type with Initial_State_Visible_Under_FOW gets one when it first appears hidden. The ghost replaces the
    // object while it is hidden, stays after the object died or moved away, and is removed once the local player sees the
    // ghost's position again (fog_point_visible) or the fog leaves the frame. Ghosts are drawn after the frame's objects
    // (ascending id) into the main / background list like objects; no particles, no Model_To_Attach.
    void set_ghosts_enabled(bool on) { ghosts_enabled_ = on; }
    size_t ghost_count() const { return ghosts_.size(); }
    bool has_ghost(objects::ObjectId id) const { return ghosts_.count(id) != 0; }

private:
    struct ObjState {
        HardpointVisibility vis;
        const GpuModel* bound = nullptr;  // model the layout was built for
        uint64_t seen = 0;                // build generation
        float fog_alpha = 1.0f;           // R5
        float fog_vel = 0.0f;             // R5: velocity of the HideWhenFogged spring
        uint64_t fog_tick = 0;            // R5: frame tick of the last fog step
        StealthFx stealth;                // R5: stealth tint spring + shadow bit
        uint64_t stealth_tick = 0;        // R5: frame tick of the last stealth step
        bool stealth_init = false;
        int alt = 0;                      // R1: applied _ALT index (obj +0x300 / model +0xc4)
        std::vector<uint8_t> combined;    // R3: hardpoint mask & bone visibility & muzzles (storage for Instance::mesh_hidden)
        bool use_combined = false;
        std::vector<std::vector<uint8_t>> attach_masks;  // R3: per hardpoint, muzzle mask of its Model_To_Attach
        bool have_last = false;           // R5: last visible state (ghost source)
        SnapshotObject last;
        std::vector<uint8_t> last_mask;
    };
    struct Ghost {
        SnapshotObject obj;
        std::vector<uint8_t> mask;
    };
    const TypeRenderInfo& render_info(objects::TypeIndex type);
    const SnapshotTypeTable* table_;
    const GameTypeResolver* resolver_;
    bool show_damage_ = false;
    PoseSource pose_;  // R3
    VisibilitySource visibility_;  // R3
    float fog_smooth_time_ = kFogSpringTime;  // R5
    AttachmentPoseSource attach_pose_;  // R3 T6
    bool ghosts_enabled_ = true;        // R5
    std::map<objects::ObjectId, Ghost> ghosts_;  // R5, ordered by id
    uint64_t generation_ = 0;
    std::vector<Laser> lasers_;
    std::map<objects::ObjectId, ObjState> states_;          // ordered by id
    std::map<objects::TypeIndex, TypeRenderInfo> type_info_;  // lookup only
    std::map<std::pair<objects::TypeIndex, uint64_t>, HardpointLayoutPtr> layouts_;  // (type, GpuModel::uid)
};

// Projectiles (CombatSystem flat table, SimSnapshot::projectiles), PROJECTILE_NEBULA_DRAW.md 1:
//   * Projectile_Custom_Render 1 / 2: a procedural laser (projectile_laser -> SimInstanceBuilder::lasers(), drawn by
//     Renderer::set_lasers); the type's ALO is never loaded or drawn;
//   * otherwise the type's Space_ / Land_Model_Name drawn like an object: projectile_transform (local X = flight direction,
//     Scale_Factor). `model` is unused (kept for callers).
assets::Mat3x4 projectile_transform(const SnapshotProjectile& p, const SnapshotTypeInfo* type, const GpuModel* model);
// The laser of a custom-rendered projectile: kind = Projectile_Custom_Render, centred on the position along the flight
// direction (the render object's local +Y), length = Projectile_Length x Scale_Factor, width, texture slot, colour.
// False: not a laser type (custom render 0 / unknown) or no width / length.
bool projectile_laser(const SnapshotProjectile& p, const SnapshotTypeInfo& type, Laser& out);
// HIDE_WHEN_FOGGED for projectiles (every shipped projectile type has it): hidden while its position is fogged for the
// local player (fog_point_visible on the frame's fog input; no fog -> visible).
bool projectile_fogged(const InterpolatedFrame& frame, const SnapshotProjectile& p);

// ---- Camera input (the contract with engine/ui's camera control, P1): camera parameters in, view out.
// The UI owns input and produces CameraParams every frame; the renderer only turns them into matrices.
struct CameraParams {
    Float3 target{0, 0, 0};
    float yaw = 0.0f;         // radians about +Z (0 = camera on +X looking towards -X)
    float pitch = 0.6f;       // radians above the XY plane
    float distance = 100.0f;
    float fov_x = 0.9599311f; // horizontal FOV, radians
    float znear = 10.0f, zfar = 7000.0f;
};
CameraParams camera_params(const OrbitCamera& cam, float znear, float zfar);
OrbitCamera orbit_camera(const CameraParams& p);
ViewMatrices camera_view(const CameraParams& p, uint32_t width, uint32_t height);

// A unit box (+-0.5, effect-less: draws with the unlit stand-in, vertex colour white) for synthetic sims and
// tests; `name` is the ModelAsset name. Bounds / sphere filled.
std::shared_ptr<assets::ModelAsset> debug_box_model(const std::string& name, float half_extent = 0.5f);

}  // namespace alamo::render

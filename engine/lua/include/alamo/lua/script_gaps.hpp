// L1/S4 wave 2b (docs/re/SCRIPT_GAPS.md): the script bindings left open by wave 2a and the host contract they
// need (src/bind_script_gaps.cpp).
//
// Bound by the family (vm_internal.hpp impl_script_gaps):
//   GameObjectWrapper.Fire_Special_Weapon(target, player)        §1.1 (lobbing / special weapon / space station)
//   GameObjectWrapper.Fire_Tactical_Superweapon()                 §1.2   .Is_Tactical_Superweapon_Ready()
//   GameObjectWrapper.Get_Parent_Mode_Object()                    §5.1 (a wrapper on the parent mode's store)
//   GameObjectTypeWrapper.Is_Affected_By_Laser_Defense()          §1.7
//   Spawn_Special_Weapon(type, player)                            §1.3
//   Create_Cinematic_Transport(type, player_id, pos, z_angle, landing_mode, anim_delta, idle_time, persist [, hint])
//                                                                 §3.1 (+ CinematicTransports below: the landing machine)
//   Start_Cinematic_Space_Retreat(player_id [, delay_seconds])    §3.2
//   Apply_Markup(player, target, amount [, handle])               §5.2 (ScriptMarkupHost below; the AI wires it)
//   MarkupBlockStatus.IsFinished() (extra: the handle is a "BlockStatus"; true, nothing to wait for) [port]
// The task-force variants (§1.4 / §1.5) are bound by the tactical AI family (bind_ai_tactical.cpp, A1): its
// Land/SpaceTaskForceClass.Fire_Special_Weapon calls detail::task_force_fire_special_weapon (vm_internal.hpp)
// with the task force's unit list - see that declaration.
//
// Error discipline (SCRIPT_GAPS.md "Common conventions"): the original's script error / warning helpers do not
// unwind; the binding logs the message and returns nothing (nil), or carries on where the doc says so (argument
// defaults of Create_Cinematic_Transport). Logged lines: "SCRIPT ERROR: ..." / "SCRIPT WARNING: ..." in
// LuaVm::messages(). Argument-count errors of the dispatcher (dead receiver) still raise as before.
//
// Host contract: ObjectBridge::tactical (object_bridge.hpp) points at a TacticalScriptHooks of the mode. Every hook
// is optional; without one the binding answers as a mode without that system would (no special-weapon data pack:
// "not ready" / error per the doc, TSW state unknown: not ready / false). The battle host (engine/apps/sim
// CampaignBattles) sets the ones its systems can answer.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/objects/object_store.hpp"

namespace alamo {
class StateHasher;
}

namespace alamo::lua {

class LuaVm;
class CinematicTransports;
struct FogScriptHooks;  // fog_bridge.hpp (fog follow-up)

struct TacticalScriptHooks {
    // ---- special weapons (§1.1, §1.6) ----
    // Readiness of the object's special-weapon data pack (obj +0x158): 0..1 ("ready" = 1); < 0 = no pack.
    std::function<float(objects::ObjectHandle weapon)> weapon_readiness;
    // Begin targeting (0x491b40): toggles the targeting flag; false = failed (already targeting / another weapon of
    // the owner targeting in space).
    std::function<bool(objects::ObjectHandle weapon)> begin_targeting;
    // Fire at an object (0x491cd0, with its own checks: ready, not allied, valid category, not self). True = fired.
    std::function<bool(objects::ObjectHandle weapon, objects::ObjectHandle target)> fire_at_object;
    // Fire a lobbing superweapon at a position (0x4ea3f0: base power, recharge restart). True = fired.
    std::function<bool(objects::ObjectHandle weapon, Vec3 position)> fire_lobbing_at;
    // SpecialWeaponsAllowed (0x527620) for a player of the mode: battle planet destroyed -> false; skirmish -> the
    // Enable_Special_Weapons modifier; campaign -> true. Unset: true.
    std::function<bool(int32_t player)> special_weapons_allowed;
    // Space station (DUMMY_STAR_BASE): readiness of its special-weapon hard point (0x385c00; < 0 = no such hard
    // point) and the can-fire test + target assignment (0x386220 range / arc / fog / protection, 0x3822e0).
    std::function<float(objects::ObjectHandle station)> station_readiness;
    std::function<bool(objects::ObjectHandle station, objects::ObjectHandle target, int32_t player)> station_fire;

    // ---- tactical superweapon (§1.2) ----
    std::function<int32_t(objects::ObjectHandle tsw)> tsw_state;  // data pack +0x20; < 0 = unknown
    std::function<bool(objects::ObjectHandle tsw)> tsw_fire;       // 0x42a200 (planet / state / target checks inside)

    // ---- engines (§2.2) ----
    // The locomotor engines flag (+0xd0): 1 online, 0 disabled (destroyed engines or a timed disable); < 0 = no
    // locomotor data (the binding then answers "online" unless the S9 engine hard points say otherwise).
    std::function<int32_t(objects::ObjectHandle unit)> engines_online;

    // ---- cinematics (§3) ----
    // Object creation through the host (trees, movement, combat). Unset: ObjectStore::create on the VM's store.
    std::function<objects::ObjectHandle(const objects::SpawnParams& p)> spawn;
    // max(terrain height, water height) at x, y (Create_Cinematic_Transport). Unset: the position's own z only.
    std::function<float(float x, float y)> ground_height;
    // Cinematic landing state machines (the host services them every frame). Null: transports are created, their
    // landing parameters are not kept.
    CinematicTransports* transports = nullptr;
    // Teleport (Start_Cinematic_Space_Retreat line-up, 0x3a8f90): store + movement + trees. Unset: store only.
    std::function<void(objects::ObjectHandle unit, Vec3 position, Vec3 facing)> teleport;
    // cu8l t02 (CU8K_T02_T06_RE §3 rule 6): Lock_Current_Orders took the movement lock counter 0 -> 1 on `unit` (0x3a8e50's
    // first lock: the split from its formation or the adopt, at most one synced draw). Unset: no draw.
    std::function<void(objects::ObjectHandle unit)> movement_first_lock;
    // Locomotor vtable +0x2a8: hyperspace out after `delay_frames` (sets the in-end-cinematic flag obj +0x3c9), with
    // its keep_parent_link flag (L1 final, L1_S4_REMAINING §3.4): Hyperspace_Away([bool = true]) -> (unit, 0, bool);
    // Start_Cinematic_Space_Retreat -> (unit, delay, false). False = not such a unit. Unset: Hyperspace_Away keeps its
    // Lua order on the direct path; the space retreat line-up happens, nobody jumps (logged).
    std::function<bool(objects::ObjectHandle unit, int32_t delay_frames, bool keep_parent_link)> hyperspace_out_link;
    // story t12: Resume_Hyperspace_In() (0x543570) clears the reinforcement hyperspace hold 0x140b27d5f (LINK_TACTICAL
    // P9 == 2, L1_S4_REMAINING §3.4). Unset: only the presentation event is recorded.
    std::function<void()> resume_hyperspace_in;
    // cu8g t03: Set_New_Environment(int) (0x5448a0 -> mode vf25, CU8F_T03_T05_RE §2.2): the mode's active environment
    // index := clamp(index, 0, n - 1) (the battle's TacticalBattle::set_environment; land weather re-inits on its next
    // service, the renderer re-applies lighting / fog / clouds / sky / land wind). Unset: only the presentation event.
    std::function<void(int32_t index)> set_new_environment;
    // The player's faction Space_Retreat_Unit_Increment_Wait_Frames (+0x20c0). Unset: 0 [port].
    std::function<int32_t(int32_t player)> retreat_increment_frames;

    // ---- fog of war (fog follow-up: FogOfWar.* / Undo_Reveal, fog_bridge.hpp) ----
    FogScriptHooks* fog = nullptr;  // null: FogOfWar.* answer "Command only valid in a tactical game"
};

// ---- Create_Cinematic_Transport landing state machine (§3.1, TransportLandingBehaviorClass 0x643c30) ----
// Animation ids: Land 43, Takeoff 44 (the doc's anim numbers). Progress of a playing animation at frame f =
// (int((frames - 1) x start_fraction) + (f - started) x speed) / frames.
inline constexpr uint8_t kAnimLand = 43, kAnimTakeoff = 44;
inline constexpr uint32_t kDefaultLandingAnimFrames = 60;  // [port] when the host does not know the clip length

struct CinematicLanding {
    objects::ObjectId id = objects::kNoObjectId;
    int32_t landing_mode = 1;   // +0x50
    float anim_delta = 0.0f;    // +0xc0
    float idle_seconds = 2.0f;  // +0xc4
    bool persist = false;       // +0xc9
    bool initialised = false;   // +0xc8
    uint8_t state = 0;          // 0 none, 1 landing, 2 landed, 3 taking off
    uint8_t anim = 0;           // playing animation (kAnimLand / kAnimTakeoff), 0 none
    uint32_t anim_start = 0;    // frame the animation started
    int32_t anim_offset = 0;    // int((frames - 1) x start fraction)
    float anim_speed = 1.0f;
    uint32_t timer_end = 0;     // landed: take-off frame
    bool cargo_released = false;
};

// What the host has to do for one landing step (in service order).
struct CinematicStep {
    enum class Kind : uint8_t { PlayLand, PlayTakeoff, Landed, CargoReleased, Destroy };
    Kind kind = Kind::PlayLand;
    objects::ObjectId id = objects::kNoObjectId;
    float start_fraction = 0.0f;  // Play*: where the clip starts (0..1)
    float speed = 1.0f;           // Play*: playback speed (take-off from landed: 1.15)
};

class CinematicTransports {
public:
    // A transport created by Create_Cinematic_Transport (initialised on its first service).
    void add(objects::ObjectId id, int32_t landing_mode, float anim_delta, float idle_seconds, bool persist);
    // One service of every machine, ascending id (frame = the battle frame, 30 / s). `anim_frames(id, anim)` = the
    // clip length in frames (0 = unknown: kDefaultLandingAnimFrames). Steps for the host are appended to `out`;
    // destroyed transports leave the list. Ids the host no longer has should be removed with remove().
    void service(uint32_t frame, const std::function<uint32_t(objects::ObjectId, uint8_t anim)>& anim_frames,
                 std::vector<CinematicStep>& out);
    void remove(objects::ObjectId id);
    const std::vector<CinematicLanding>& entries() const { return list_; }
    const CinematicLanding* find(objects::ObjectId id) const;
    void hash(StateHasher& h) const;
    // N1 save / load (contract: alamo/core/save_file.hpp; saved by the tactical host that owns the machines): u32
    // version, u32 count, the machines in ascending id (every CinematicLanding field). False and unchanged on bad
    // data. Reads its own fields only (the owner checks done()).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    std::vector<CinematicLanding> list_;  // ascending id
};

// ---- Apply_Markup (§5.2): the AI side ----
// The AI runtime implements this and attaches it to the VMs that call Apply_Markup (galactic AI plans:
// PathBonusForDistantTargets.lua). Target ids are AITargetLocationWrapper ids (ai_bridge.hpp).
class ScriptMarkupHost {
public:
    virtual ~ScriptMarkupHost() = default;
    // The AI target of a game object in `player`'s AI context for the current mode (planets: the planet's target;
    // other objects: looked up by name). -1 = none.
    virtual int32_t markup_target_of_object(int32_t player, objects::ObjectHandle object) = 0;
    // `player`'s own AI target (player +0x478) in `caller`'s target registry. -1 = none.
    virtual int32_t markup_target_of_player(int32_t caller, int32_t player) = 0;
    // The AI perception "Markup" value of the target (+0x80, a double) += delta.
    virtual void add_markup(int32_t target, double delta) = 0;
};
void attach_markup_host(LuaVm& vm, ScriptMarkupHost* host);  // nullptr detaches
ScriptMarkupHost* markup_host(LuaVm& vm);

}  // namespace alamo::lua

// S10-int / R3 hookup: the sim-owned animation state (docs/re/ANIM_PARTICLES.md §A, R3 integration asks).
//
// One anim::AnimWorld per mode primary store (rows = objects whose type has a model with a skeleton or clips),
// driven from the SimHost's T5 slot (SimHost::set_object_animation_hook, step "T5.anim", right after the
// mode tick and the S10 battle hook):
//   1. sync the TOP mode's world with its primary store: objects new since the last sync get their model
//      created (§A.7 "model created": default idle, Loop_Idle_Anim_00, Idle_Anim_00_Rate_Mod, random start
//      frame on the SYNCED RNG only when the idle clip exists), in ascending object id; rows of objects that
//      left the store (deferred-delete flush) are removed;
//   2. advance(global frame) through the host's ParallelFor (per-row pure function).
// Behaviours that play clips (the S10 battle: weapon Attack, locomotor Move / Walk, idle fidgets; Lua later)
// use world(mode) directly in their own tick slot. Only the top mode is synced / advanced (only it ticks).
// Worlds of modes that left the stack are dropped at the next service.
// [port] (listed in docs/PHASE4.md): model creation happens at the sync after the spawning step, not inside
// the spawn call, so the start-frame draw sits after that tick's battle draws; hardpoints are flat combat
// sub-records (no own models / rows); the T6 scene-model advance has no sim models yet (stub). Secondary stores
// get their own world (model created with the FREE RNG, advanced, never hashed). While the top mode is paused
// the service skips behaviours and the advance ([port] frozen rule, see service()).
// Per-tick outputs for the renderer / audio (never hashed): muzzles() (fire-bone muzzle flashes, 4 ticks),
// events() (AnimationSFXMaps.txt SURFACE / SOUND rows crossed by the primary clip: foot plants etc., with the
// bone's world position), corpses(), plus AnimWorld::bone_visibility on the worlds.
// Locomotor / turn / deploy / Berserker / Space Idle / damage-kind deaths / spin-away: service_locomotor(),
// deploy(), set_berserker(), begin_death(kind) (rules in anim_behaviors.hpp, anim_state.hpp).
// R3 behaviours (anim::AnimBehaviors, one per mode next to the world; anim_behaviors.hpp has the rules):
//   * hit()          flinch on a projectile hit (the battle's HitEvents, event order);
//   * service_idle() the idle behaviour incl. idle while engaged (the battle's locomotor step);
//   * begin_death()  Die + DeathBehavior: death clip, or removal at once (Remove_Upon_Death without a clip). Called
//                    by the battle BEFORE it destroys the unit; the unit's row then lives on as a CORPSE row
//                    ([port]: the battle removes the store object at death, so the corpse is a detached row +
//                    anim::Corpse record holding the type / owner / position / facing captured at death);
//   * death_clone_spawned()  a Death_Clone object runs its own death sequence (its type's Specific_Death_Anim_*)
//                    right after its model is created at the next sync [port: clones die at once];
//   * service: before the advance, flinch holds end and the corpse countdown runs (Death_Persistence_Duration
//     after the death clip, then Death_Fade_Time); a finished corpse loses its row, and a store object that is
//     still alive (death clones) is destroyed (TacticalMode::destroy).
// Rows whose store object left are removed unless they are corpses (then marked detached).
// CRC: opt-in "anim" (attach(host, true)): per mode bottom -> top, has-world flag + AnimWorld::hash_state +
// AnimBehaviors::hash.
// Animation draws on the synced RNG, so enabling the service changes the "rng" CRC of runs that use it;
// it is off unless a host attaches it (existing goldens unchanged).
// Build hygiene (battle host): without engine/anim (ALAMO_SIM_HAVE_ANIM unset) this header only declares the class,
// so translation units that include it unconditionally still compile; every use is guarded by ALAMO_SIM_HAVE_ANIM.
#pragma once
#ifndef ALAMO_SIM_HAVE_ANIM
namespace alamo::sim {
class SimAnimation;
}  // namespace alamo::sim
#else
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "alamo/anim/anim_behaviors.hpp"
#include "alamo/anim/anim_events.hpp"
#include "alamo/anim/anim_state.hpp"
#include "alamo/anim/clip.hpp"
#include "alamo/anim/type_params.hpp"
#include "alamo/core/file_provider.hpp"
#include "alamo/sim/game_modes.hpp"

namespace alamo::sim {

class SimHost;

struct SimAnimStats {
    uint32_t models = 0, models_with_clips = 0, created = 0, removed = 0, rows = 0;
    // R3 behaviours
    uint32_t flinches = 0, deaths = 0, death_clips = 0, removed_at_death = 0, clone_deaths = 0, corpses_removed = 0, corpses = 0;
    uint32_t spin_aways = 0, kind_death_types = 0, turns = 0, deploys = 0, muzzle_flashes = 0, secondary_rows = 0;
    uint64_t events = 0, foot_plants = 0;
};

class SimAnimation {
public:
    // `types` and `files` must outlive this object.
    SimAnimation(const objects::GameObjectTypes& types, const FileProvider& files, anim::AnimTunables tunables = {});
    SimAnimation(const SimAnimation&) = delete;
    SimAnimation& operator=(const SimAnimation&) = delete;

    // Installs the T5 hook on `host` (and the "anim" CRC system when `crc`) and the "anim" save chunk. The object must
    // outlive the host's ticks / CRC captures / saves.
    void attach(SimHost& host, bool crc);
    void detach(SimHost& host);
    // N1 save chunk "anim" (owner: n2-host): u32 version 1, u32 mode depth, then per mode bottom -> top: bool has-world;
    // if so a section with AnimBehaviors::write, a section with AnimWorld::write (primary world), i32 max_seen, the
    // pending death-clone ids, the event-row ids, the muzzles (id, hardpoint, bone, until). read() validates every mode
    // on scratch copies, then reads the live worlds in place (battle hooks / script bridges hold pointers to them): rows
    // come back with the host's model choice (the store object's type, else the corpse's), nothing is re-created at
    // the next sync (max_seen restored) and no RNG is drawn. Not saved: the secondary (cosmetic) worlds - re-created at
    // the next service with the FREE RNG (max_seen_secondary reset) - per-service events and the stats.
    void write(const SimHost& host, ByteWriter& w) const;
    bool read(SimHost& host, ByteReader& r);

    // The world of `mode` (created empty when absent). Valid until the mode leaves the stack.
    anim::AnimWorld& world(const GameMode& mode);
    const anim::AnimWorld* find_world(const GameMode& mode) const;
    // Model (shared, cached) and parameters of a type in a mode; null model = no animated model.
    const anim::AnimModel* model_for(objects::TypeIndex type, objects::ModeKind mode);
    const anim::TypeAnimParams& params_for(objects::TypeIndex type, objects::ModeKind mode);
    // Test / tool hook: `model` (caller-owned, must outlive this object; null = no row) is used for `type` in
    // `mode` instead of loading the type's model. Call before the type's first use.
    void set_model(objects::TypeIndex type, objects::ModeKind mode, const anim::AnimModel* model);

    // ---- R3 behaviours (battle hookups; sim thread, the caller's service order) ----
    anim::AnimBehaviors& behaviors(const GameMode& mode);
    const anim::AnimBehaviors* find_behaviors(const GameMode& mode) const;
    // Flinch of `victim` hit by a shot travelling along `travel` (2-D; victim facing `facing`). True if it played.
    bool hit(const GameMode& mode, ObjectId victim, Vec3 facing, Vec3 travel, SimRng& rng, uint64_t frame);
    // Idle behaviour (incl. idle while engaged) of a battle unit whose clip is finished / absent.
    bool service_idle(const GameMode& mode, ObjectId id, SimRng& rng, uint64_t frame);
    // Death of a live store object (call before destroying it). RemoveNow: no corpse (the caller removes it).
    // `damage_kind` = the killing hit's DamageType (death type by kind, anim_behaviors.hpp).
    anim::DeathResult begin_death(const GameMode& mode, ObjectId id, SimRng& rng, uint64_t frame, uint8_t damage_kind = 1);
    // A Death_Clone object was spawned: its death sequence runs after its model creation at the next sync.
    void death_clone_spawned(const GameMode& mode, ObjectId clone);
    // Corpses of `mode` (ascending id). `detached` ones have no store object: the renderer draws them from
    // Corpse::info with the row's pose (AnimWorld row of the same id) and Corpse::alpha(frame); `spin_away`
    // ones spin until removal (the spin motion and Spin_Away_On_Death_Explosion are render-side).
    std::span<const anim::Corpse> corpses(const GameMode& mode) const;

    // Locomotor clip selection of a unit (AnimBehaviors::service_locomotor with the type's parameters; the
    // default idle goes through the idle-type hook). `walk_ratio` / `min_ratio`: the battle's tunables.
    uint32_t service_locomotor(const GameMode& mode, ObjectId id, anim::LocomotorInput in, float walk_ratio, float min_ratio, SimRng& rng,
                               uint64_t frame);
    // Deploy / undeploy clip (Deployment_Anim_Rate). True if a clip started.
    bool deploy(const GameMode& mode, ObjectId id, bool undeploy, SimRng& rng, uint64_t frame);
    // Berserker ability state (AnimWorld::set_berserker).
    void set_berserker(const GameMode& mode, ObjectId id, bool active);
    // Per-object frozen flag (object +0x3a7 [L]): the row's advance is skipped.
    void set_frozen(const GameMode& mode, ObjectId id, bool frozen);
    // Default-idle hook (garrison container mapping, 0x395920): returns the idle type for a garrisoned object or
    // anim::kNoAnimType. Unset = no garrison mapping. The S4 garrison system plugs in here.
    using IdleTypeHook = std::function<int32_t(const GameMode&, ObjectId)>;
    void set_idle_type_hook(IdleTypeHook hook) { idle_hook_ = std::move(hook); }

    // Muzzle flash (§A.7 attack row: muzzle sub-objects shown, re-hidden after 4 ticks [M]). hardpoint = index into
    // the unit's HardPoints list of the shooting hardpoint (-1 = the unit's own weapon); bone = that fire bone
    // (Fire_Bone_A / _B) in the object's skeleton when it resolved there, -1 otherwise (the renderer resolves it,
    // e.g. inside the hardpoint's attached model, or picks the unit's muzzle bones).
    void muzzle_flash(const GameMode& mode, ObjectId id, int32_t hardpoint, int32_t bone, uint64_t frame);
    struct Muzzle {
        ObjectId id = 0;
        int32_t hardpoint = -1;
        int32_t bone = -1;
        uint64_t until = 0;  // visible while frame < until
    };
    std::span<const Muzzle> muzzles(const GameMode& mode) const;  // ascending (id, hardpoint, bone)

    // Animation events of the last service (AnimationSFXMaps.txt SURFACE / SOUND rows crossed by the primary clip;
    // anim_events.hpp): foot plants for footprints, body impacts, animation SFX. Object order (ascending id), then
    // time order. Visual / audio output only (never hashed).
    struct FrameEvent {
        ObjectId id = 0;
        const anim::AnimEventDef* def = nullptr;  // kind, name (SurfaceFX / SFX event), bone name, time
        int32_t bone = -1;                        // skeleton bone index (-1: none / not found)
        Vec3 position{};                          // world position of the bone (object position without a bone)
        bool corpse = false;
    };
    std::span<const FrameEvent> events(const GameMode& mode) const;
    // Event table (loaded from Data\XML\AnimationSFXMaps.txt at the first service; tests may set it).
    const anim::AnimEventMap& event_map();
    void set_event_map(anim::AnimEventMap map);

    // Secondary (local, cosmetic) store animation: free RNG, never hashed (§A.8).
    const anim::AnimWorld* find_secondary_world(const GameMode& mode) const;

    // One service (what the hook runs): prune, sync the top mode, behaviours, advance it.
    void service(SimHost& host);
    void hash(const SimHost& host, StateHasher& h) const;
    const SimAnimStats& stats() const { return stats_; }

private:
    struct ModeWorld {
        const GameMode* mode = nullptr;
        std::unique_ptr<anim::AnimWorld> world;
        std::unique_ptr<anim::AnimBehaviors> behaviors;
        std::unique_ptr<anim::AnimWorld> secondary;
        std::vector<ObjectId> pending_clone_deaths;  // request order
        ObjectId max_seen = INT32_MIN;               // highest store id considered for model creation
        ObjectId max_seen_secondary = INT32_MIN;
        std::vector<ObjectId> event_rows;            // rows whose model has event clips (ascending)
        std::vector<Muzzle> muzzles;
        std::vector<FrameEvent> events;
    };
    struct EventSnap {
        ObjectId id;
        const anim::ClipRef* clip;
        int32_t type;
        uint32_t variant;
        float frame;
        bool loop;
    };
    anim::IdleParams idle_params(const GameMode& mode, ObjectId id, objects::TypeIndex t, const anim::AnimModel* m);
    void sync_secondary(ModeWorld& mw, const GameMode& mode, SimRng& rng, uint64_t frame);
    void collect_events(ModeWorld& mw, const GameMode& mode);
    bool model_has_events(const anim::AnimModel* m);
    ModeWorld& mode_world(const GameMode& mode);
    const ModeWorld* find_mode_world(const GameMode& mode) const;
    anim::DeathResult start_death(ModeWorld& mw, const GameMode& mode, ObjectId id, SimRng& rng, uint64_t frame, uint8_t damage_kind = 1);
    struct TypeEntry {
        anim::TypeAnimParams params;
        const anim::AnimModel* model = nullptr;
    };
    void prune(const SimHost& host);
    void sync(const GameMode& mode, SimRng& rng, uint64_t frame);  // appends objects to destroy to scratch_destroy_

    const objects::GameObjectTypes* types_;
    const FileProvider* files_;
    anim::AnimTunables tunables_;
    anim::ClipCache clips_;
    std::vector<ModeWorld> worlds_;                                   // lookup by mode identity only
    std::map<std::string, std::unique_ptr<anim::AnimModel>> models_;  // by normalised model|override (lookup)
    std::map<uint64_t, TypeEntry> types_cache_;                       // (mode << 32 | type) (lookup)
    std::vector<ObjectId> scratch_ids_, scratch_remove_, scratch_destroy_;
    SimAnimStats stats_;
    IdleTypeHook idle_hook_;
    anim::AnimEventMap events_map_;
    bool events_loaded_ = false;
    std::map<const anim::AnimModel*, bool> model_events_;                                // lookup only
    std::map<const anim::Clip*, const std::vector<anim::AnimEventDef>*> clip_events_;  // lookup only
    std::vector<EventSnap> snaps_;
    std::vector<uint32_t> scratch_hits_;
};

}  // namespace alamo::sim
#endif  // ALAMO_SIM_HAVE_ANIM

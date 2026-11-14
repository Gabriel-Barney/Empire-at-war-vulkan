// Runtime game objects (docs/re/OBJECT_RUNTIME.md): one object kind for everything (planets,
// fleets, heroes, units, projectiles, hardpoints, props); behaviours make the difference.
//
// Identity
//   * ObjectId (int32) = the original's gameplay-visible id (Lua Get_Object_ID, saves, ordering).
//     kNoObjectId = 0x3FFFFF. Primary stores of all modes share one IdCounter (starts at 1 per game,
//     post-incremented, never reused); a secondary store has its own counter. 0x3FFFFF is never
//     handed out (the original's latent wrap bug is not reproduced).
//   * ObjectHandle (64-bit: generation << 32 | slot) is the port's internal reference: a stale
//     handle (object flushed, slot reused) is detected by the generation. value() is what other
//     modules (engine/commands) carry; 0 = null.
//   * parent_mode_id links a tactical object to its galactic counterpart (by id).
//
// Ownership: the ObjectStore is the single mutation owner of its objects. Everything is SoA,
// indexed by slot. create() registers immediately (as Create_Object); destroy() flags the object
// dead and queues it at once; unlinking happens in flush() at the end of update() (deferred
// deletion). Children attached to a dying object (attach_parent: hardpoints...) die with it during
// the flush, and references to the dying object (container / attach parent) are cleared.
//
// Behaviours: attached at creation in order: the type's Behavior list, then the mode list
// (GalacticBehavior / LandBehavior / SpaceBehavior by mode), then runtime additions; one instance
// per listed name, duplicates included (§9.2). Per object an attach-ordered list plus the
// original's 0x6A-entry type -> index table (last instance). Only serviced behaviours (§9.1) put
// the object in the serviced list; each instance carries its service interval. Per behaviour type a
// component table (members sorted by slot, a dense row per member; the state struct is empty until
// the systems that own the data exist) for parallel per-behaviour updates, and an insertion-ordered
// member list (the original's category lists).
//
// Determinism (Â§6): store lists are insertion-ordered and the canonical walk is NEWEST FIRST
// (object service order, Lua Find_All_Objects_Of_Type). The serviced loop visits only objects that
// existed when it started; objects created during it are serviced next frame. The delayed-creation
// queue is scanned last-to-first. The id map is lookup-only. The frame parity CRC is the wrapping
// sum of per-object CRCs (position, facing, health; dead objects skipped).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "alamo/core/galaxy.hpp"
#include "alamo/core/bytes.hpp"
#include "alamo/core/sim_crc_fwd.hpp"
#include "alamo/core/sim_rng.hpp"
#include "alamo/objects/behaviors.hpp"
#include "alamo/objects/object_types.hpp"

namespace alamo::objects {

class GameObjectTypes;

using ObjectId = int32_t;
inline constexpr ObjectId kNoObjectId = 0x3FFFFF;

struct ObjectHandle {
    uint64_t value = 0;
    static constexpr ObjectHandle make(uint32_t slot, uint32_t generation) {
        return {(static_cast<uint64_t>(generation) << 32) | slot};
    }
    uint32_t slot() const { return static_cast<uint32_t>(value & 0xFFFFFFFFu); }
    uint32_t generation() const { return static_cast<uint32_t>(value >> 32); }
    explicit operator bool() const { return value != 0; }
    friend bool operator==(ObjectHandle a, ObjectHandle b) { return a.value == b.value; }
    friend bool operator!=(ObjectHandle a, ObjectHandle b) { return a.value != b.value; }
};

// Post-increment id allocator. Saved/restored with the game (next()/set_next()).
class IdCounter {
public:
    ObjectId allocate() {
        if (next_ == kNoObjectId) ++next_;
        return next_++;
    }
    ObjectId next() const { return next_; }
    void set_next(ObjectId n) { next_ = n; }
    void reset() { next_ = 1; }  // game start

private:
    ObjectId next_ = 1;
};

enum class ModeKind : uint8_t { Galactic = 0, Land = 1, Space = 2 };
enum class StoreKind : uint8_t { Primary, Secondary };

struct SpawnParams {
    TypeIndex type = kNone;
    int32_t owner = -1;  // player id
    Vec3 position{};
    Vec3 facing{};
    ObjectId parent_mode_id = kNoObjectId;
    ObjectHandle container{};      // ParentContainerObject (team / fleet / transport / planet)
    ObjectHandle attach_parent{};  // AttachParentObject (hardpoints): dies with the parent
};

// Object flags (subset of the original's +0x3A0 byte).
enum ObjectFlag : uint8_t {
    kFlagDead = 0x02,                // destroyed; unlinked at the next flush
    kFlagMarkedByChildMode = 0x04,   // a tactical counterpart died (parent-mode death marking)
    // refix t01 (S9_DEATH_CLONE_SHIELDS.md §1.6 [H], AI_TACTICAL.md §2, L1_S4_REMAINING.md): +0x3a0 bit 0x40 = dying /
    // dead state (Die sets it; death clones are created with it), bit 0x80 = removing. A death clone gets both (0xc0).
    // Such an object still exists and is alive() (it is drawn and runs its death sequence), but gameplay searches skip
    // it: use active() for "alive and not dying / removing" (the original's +0x3a0 & 0xc2 clear).
    kFlagDying = 0x40,
    kFlagRemoving = 0x80,
    kFlagDeathClone = kFlagDying | kFlagRemoving,
};

// Per-behaviour component table. members: slots ascending (row = index); row_of_slot maps back.
struct BehaviorTable {
    BehaviorTypeId id = kNoBehavior;
    std::vector<uint32_t> members;            // slot order
    std::vector<ObjectHandle> insertion;      // attach order across objects (category list)
    uint32_t row(uint32_t slot) const;        // kNone if not a member
};

class ObjectStore {
public:
    // `ids`: the shared counter for primary stores (must outlive the store); nullptr = own counter
    // (secondary stores). `types` must outlive the store.
    ObjectStore(ModeKind mode, StoreKind kind, const GameObjectTypes& types, IdCounter* ids = nullptr);

    ModeKind mode() const { return mode_; }
    StoreKind kind() const { return kind_; }
    IdCounter& id_counter() { return *ids_; }

    // ---- mutation (single owner) ----
    ObjectHandle create(const SpawnParams& p);
    void queue_create(uint32_t due_frame, const SpawnParams& p);  // delayed-creation queue
    // Destroy_Object: idempotent; flags dead, queues for the flush, applies the parent-mode death
    // hook. Returns false for a stale/null handle or an already-dead object.
    bool destroy(ObjectHandle h);
    // Runtime behaviour addition (appended after the XML lists). False if already present/unknown.
    // (The XML attach loop itself has no duplicate check: a name listed twice, or two names with
    // one id, attach two instances; the type -> index table points at the later one.)
    bool add_behavior(ObjectHandle h, BehaviorTypeId id);
    // Remove_Behavior (0x3a54c0, S9_DEATH_CLONE_SHIELDS 1.6): instance `index` of behaviors(h) leaves the attach-ordered
    // list (later instances move down one index; their state goes with them), the type -> index table is rebuilt (last
    // instance wins), and when no instance of that id is left the object leaves the id's component table and category
    // list (order of the others kept). The serviced registration stays [port: not traced]. No draws. False for a stale
    // handle or a bad index. The detach side effects (vf15) are the caller's.
    bool remove_behavior(ObjectHandle h, uint32_t index);
    void set_position(ObjectHandle h, Vec3 p);
    void set_facing(ObjectHandle h, Vec3 f);
    void set_health(ObjectHandle h, float health);
    void set_flag(ObjectHandle h, uint8_t flag);  // kFlagDead is ignored here (use destroy)
    // Owner change (Lua Change_Owner, story rewards, captures; L1/S4 2026-10-27): the object leaves the
    // old owner's owned_by() list (order of the others kept) and is APPENDED to the new owner's list, as
    // if it had been registered for that owner now [port: the original's per-owner lists are its player
    // trees, TACTICAL_COMBAT.md 1.3 "remove from the old tree, insert into the new"]. Live objects only;
    // false for a stale / dead handle, owner < -1, or no change. The owner is part of hash_state; the
    // parity CRC (position / facing / health) is unchanged. Systems that cache owners (GalacticSim data
    // packs, tactical TreeSet) are the caller's to update.
    bool set_owner(ObjectHandle h, int32_t owner);
    // a1h t08 - transport carried type (TransportDataPack +0x48, A1_READINGS §32.2 [H]): the company type a TRANSPORT
    // carries, which is its representative type (0x3973b0) when set. The original sets it in galactic company production
    // (0x4f45c0) and on a land reinforcement's landing object (0x4ee230 from 0x2b5fe0: the galactic parent's
    // representative type in campaign, the reinforced type in skirmish); Create_Cinematic_Transport leaves it unset.
    // Only live objects with TRANSPORT or TRANSPORT_LANDING (the pack at obj +0xc8) have it; kNone clears it. False and
    // nothing changed otherwise or for a type index out of range. A new object starts with none.
    bool set_carried_type(ObjectHandle h, TypeIndex carried);
    // a1i t05 - landing persist (TransportDataPack +0xc9, TACTICAL_GAPS §4.2.2 item 3 [H]): set only by
    // Create_Cinematic_Transport's `persist` argument; a persistent landing transport never takes off and does not block a
    // human drop's ignore of landing transports. Same objects as set_carried_type; false and nothing changed otherwise.
    bool set_landing_persist(ObjectHandle h, bool persist);

    // Store update (Â§5.3): pre-service hook (spatial-tree refit), serviced objects newest-first
    // (those alive when the loop started), delayed creations (last-to-first, due <= frame), flush.
    using ServiceFn = std::function<void(ObjectStore&, ObjectHandle)>;
    void update(uint32_t frame, const ServiceFn& service, const std::function<void(ObjectStore&)>& pre_service = {});
    void flush();  // deferred-delete flush (also called by update())

    // Behaviour service (object Service step 4, OBJECT_RUNTIME.md §4.4 / §9.1, TACTICAL_GAPS.md
    // §6.4 / §6.6), for use inside the ServiceFn: visits the object's SERVICED behaviour instances
    // in attach order and calls `fn` for each one whose next-service frame <= `frame` AND whose
    // disable counter is 0; before the call the next-service frame becomes frame + interval (base
    // Service 0x4c2f70; interval 0 and 1 both mean every frame). `fn` may override it
    // (set_behavior_next_service: STUNNABLE / CONFUSE set their end frame) or re-cache the interval
    // (set_behavior_interval: SHIELDED / POWERED / INDIGENOUS re-cache on every service). Stops if
    // the object dies. Returns the number serviced.
    using BehaviorServiceFn = std::function<void(ObjectStore&, ObjectHandle, BehaviorTypeId id, uint32_t index)>;
    uint32_t service_behaviors(ObjectHandle h, uint32_t frame, const BehaviorServiceFn& fn);

    // ---- behaviour scheduling (OI objects 2026-10-28, TACTICAL_GAPS.md §6.4) ----
    // Initialise of every attached behaviour (construction lists and add_behavior; the port has no
    // object load path that re-attaches, so it always runs):
    //   * interval < 2: next-service frame 0 (due at once);
    //   * interval >= 2: next = mode frame + synced Get_Int(0, interval - 1) (0x1ffb40 on the sync
    //     RNG) -- ONE draw per such behaviour, in attach order, at the moment it is attached;
    //   * disable counter := 0, then 1 for behavior_starts_disabled() ids.
    // The draw needs bind_sync_rng(); an unbound store (secondary stores, unit tests) makes no draw
    // and uses offset 0 (next = mode frame). Never bind a secondary store: it is local / cosmetic.
    void bind_sync_rng(SimRng* rng) { rng_ = rng; }
    SimRng* sync_rng() const { return rng_; }
    // Mode frame source (GameMode +0x10); without it the frame of the last update() is used.
    void bind_mode_frame(const uint64_t* mode_frame) { mode_frame_src_ = mode_frame; }
    uint32_t mode_frame() const;
    // Inputs of the computed intervals (FPS, GameConstants shield / energy recharge seconds).
    void set_interval_config(const BehaviorIntervalConfig& c) { interval_cfg_ = c; }
    const BehaviorIntervalConfig& interval_config() const { return interval_cfg_; }
    // Per-instance overrides for the behaviour systems (no-ops on stale handles / bad indices).
    void set_behavior_next_service(ObjectHandle h, uint32_t index, uint32_t frame);
    void set_behavior_interval(ObjectHandle h, uint32_t index, uint32_t interval);
    // Disable counter (+0x3c, TACTICAL_GAPS.md §6.6): Enable (vf23 0x4c30a0) = max(0, c - 1),
    // Disable (vf24 0x4c30b0) = c + 1; serviced / "behaviour active" only at 0. Return the new
    // counter (0 on a stale handle / bad index).
    uint32_t enable_behavior(ObjectHandle h, uint32_t index);
    uint32_t disable_behavior(ObjectHandle h, uint32_t index);
    // Do_End_Cinematic_Cleanup (§7.2): Disable on every attached behaviour, attach order.
    void disable_all_behaviors(ObjectHandle h);
    uint32_t behavior_disable_count(ObjectHandle h, uint32_t index) const;
    // 0x4c3700: counter == 0 (HUNT / LURE / DEPLOY_TROOPERS "active" in Is_Unit_Ability_Active).
    bool behavior_enabled(ObjectHandle h, uint32_t index) const { return behavior_disable_count(h, index) == 0; }
    // By type id: the LAST attached instance (type -> index table); false / 0 when absent.
    bool behavior_enabled_by_type(ObjectHandle h, BehaviorTypeId id) const;
    // Sync draws made by behaviour Initialise in this store (diagnostic, not hashed).
    uint64_t attach_draws() const { return attach_draws_; }

    // Called from destroy() when the dying object has a parent-mode id (Â§2.5).
    std::function<void(const ObjectStore&, ObjectHandle dying)> on_parent_mode_death;
    // q6 object scripts (docs/re/OBJECT_SCRIPTS.md §1.2 / §1.3): called at the end of create(), after the behaviour
    // lists and the store lists, so a host can attach the type's Lua_Script. Host wiring: not saved, not hashed. Unset:
    // nothing. [port] The original attaches before the object enters the store lists.
    std::function<void(ObjectStore&, ObjectHandle created)> on_created;

    // ---- queries ----
    bool exists(ObjectHandle h) const;  // live or dead-but-not-flushed
    bool alive(ObjectHandle h) const;   // exists and not dead (Lua Is_Valid)
    // refix t01: dying / removing (kFlagDying or kFlagRemoving set: death clones). False for a stale handle.
    bool dying(ObjectHandle h) const { return exists(h) && (flags_[h.slot()] & kFlagDeathClone) != 0; }
    // alive() and not dying(): what gameplay searches see (Lua Find_* walks, AI perception, counts by type / owner).
    bool active(ObjectHandle h) const { return alive(h) && (flags_[h.slot()] & kFlagDeathClone) == 0; }
    ObjectHandle find(ObjectId id) const;  // also finds dead-not-flushed objects; null if unknown
    ObjectHandle handle_of_slot(uint32_t slot) const;
    size_t live_count() const { return all_.size(); }  // registered (incl. dead pending flush)
    uint32_t capacity() const { return static_cast<uint32_t>(gen_.size()); }

    ObjectId id(ObjectHandle h) const { return id_[h.slot()]; }
    ObjectId parent_mode_id(ObjectHandle h) const { return parent_mode_id_[h.slot()]; }
    TypeIndex type(ObjectHandle h) const { return type_[h.slot()]; }
    const GameObjectTypes& object_types() const { return *types_; }
    int32_t owner(ObjectHandle h) const { return owner_[h.slot()]; }
    Vec3 position(ObjectHandle h) const { return position_[h.slot()]; }
    Vec3 facing(ObjectHandle h) const { return facing_[h.slot()]; }
    float health(ObjectHandle h) const { return health_[h.slot()]; }
    uint8_t flags(ObjectHandle h) const { return flags_[h.slot()]; }
    ObjectHandle container(ObjectHandle h) const { return container_[h.slot()]; }
    ObjectHandle attach_parent(ObjectHandle h) const { return attach_parent_[h.slot()]; }
    TypeIndex carried_type(ObjectHandle h) const { return exists(h) ? carried_type_[h.slot()] : kNone; }  // a1h t08
    bool landing_persist(ObjectHandle h) const { return exists(h) && landing_persist_[h.slot()] != 0; }     // a1i t05
    bool has_behavior(ObjectHandle h, BehaviorTypeId id) const;
    std::span<const BehaviorTypeId> behaviors(ObjectHandle h) const { return behaviors_[h.slot()]; }  // attach order
    // Per instance (index into behaviors(h)): serviced flag, interval, next-service frame.
    bool behavior_serviced(ObjectHandle h, uint32_t index) const { return beh_state_[h.slot()][index].serviced; }
    uint32_t behavior_interval(ObjectHandle h, uint32_t index) const { return beh_state_[h.slot()][index].interval; }
    uint32_t behavior_next_service(ObjectHandle h, uint32_t index) const { return beh_state_[h.slot()][index].next; }
    uint32_t serviced_behavior_count(ObjectHandle h) const;  // the original's NumServicedBehaviors
    // Index of the LAST attached instance of `id` (type -> index table), kNone if absent.
    uint32_t behavior_index(ObjectHandle h, BehaviorTypeId id) const;

    // Insertion-ordered lists; the canonical game order is newest first (reverse).
    std::span<const ObjectHandle> all_objects() const { return all_; }
    std::span<const ObjectHandle> serviced() const { return serviced_; }
    std::vector<ObjectHandle> objects_newest_first() const;
    // active() objects of `type` (dying / removing death clones skipped), newest first. Not the Lua
    // Find_All_Objects_Of_Type, which has no death-clone test and matches representative types (engine/lua bind_objects.cpp).
    std::vector<ObjectHandle> find_all_of_type(TypeIndex type) const;
    std::span<const ObjectHandle> owned_by(int32_t owner) const;  // insertion order
    const BehaviorTable& behavior_table(BehaviorTypeId id) const { return tables_[id]; }

    // ---- CRC ----
    // Original frame-CRC part (primary store): wrapping sum of parity_object_crc over live objects.
    uint32_t parity_crc() const;
    // Ordered state digest for the Q1 registry: live and dead-pending objects by id ascending. The carried types are
    // hashed after them only when an object has one, so a store without any keeps the digest it had before a1h t08.
    void hash_state(StateHasher& h) const;
    // N1 save / load (contract: alamo/core/save_file.hpp). write() stores the whole runtime state: the
    // id counter's next(), every slot (generation, used flag and all per-slot data, also of free slots,
    // so a stale handle reads exactly what it read before), the LIFO free list, every insertion-ordered
    // list (all / serviced / per owner / per-behaviour category lists), the id map, the delayed-creation
    // queue and the dead objects pending the flush. Handles are saved as their 64-bit values (slot +
    // generation): the slot table is restored as it was, so every handle held elsewhere (sim, commands,
    // Lua) stays valid / stale exactly as before. Types are saved by TypeIndex, guarded by the type count
    // and a digest of the types' name CRCs in definition order.
    // NOT saved (host wiring / definitions, re-established when the session is rebuilt): the sync RNG and
    // mode-frame bindings, interval_config(), on_parent_mode_death. The behaviour tables' slot-sorted
    // member lists and the per-object type -> index tables are rebuilt.
    // read() into a store built with the same mode, kind and GameObjectTypes; it also sets the id
    // counter (a primary store's shared counter gets the same value from every store of that game).
    // False and nothing changed (store and counter) on a version / mode / kind / types mismatch, an
    // inconsistent list or truncated data. Like SimRng / StoryModel it does not require r.done(): the
    // caller checks that at the end of its chunk. Version 2 (a1h t08) adds the carried types at the end; a version 1
    // save still loads (no carried types). Version 3 (a1i t05) adds the landing persist flags after them (v1 / v2 load
    // with none).
    static constexpr uint32_t kSaveVersion = 3;
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
    // Registers "<name>" (ordered digest) and "<name>.parity" (parity sum) with a CrcRegistry.
    void register_crc(CrcRegistry& registry, const std::string& name) const;

private:
    struct Delayed {
        uint32_t due;
        SpawnParams p;
    };
    struct BehaviorState {
        bool serviced = false;
        uint32_t interval = 0;
        uint32_t next = 0;
        uint32_t disabled = 0;  // disable counter (+0x3c)
    };
    BehaviorState* beh_state(ObjectHandle h, uint32_t index);
    const BehaviorState* beh_state(ObjectHandle h, uint32_t index) const;
    uint32_t alloc_slot();
    uint64_t types_digest() const;
    void attach(uint32_t slot, BehaviorTypeId id, const BehaviorService& svc);
    void remove_object(uint32_t slot);

    ModeKind mode_;
    StoreKind kind_;
    const GameObjectTypes* types_;
    IdCounter own_ids_;
    IdCounter* ids_;
    SimRng* rng_ = nullptr;
    const uint64_t* mode_frame_src_ = nullptr;
    uint32_t last_frame_ = 0;
    BehaviorIntervalConfig interval_cfg_{};
    uint64_t attach_draws_ = 0;

    // SoA by slot.
    std::vector<uint32_t> gen_;
    std::vector<uint8_t> used_;
    std::vector<ObjectId> id_, parent_mode_id_;
    std::vector<TypeIndex> type_;
    std::vector<int32_t> owner_;
    std::vector<Vec3> position_, facing_;
    std::vector<float> health_;
    std::vector<uint8_t> flags_, registered_serviced_;
    std::vector<ObjectHandle> container_, attach_parent_;
    std::vector<TypeIndex> carried_type_;  // kNone: none (always kNone on a free slot)
    std::vector<uint8_t> landing_persist_;  // a1i t05: 1 = persist (always 0 on a free slot)
    std::vector<std::vector<BehaviorTypeId>> behaviors_;
    std::vector<std::vector<BehaviorState>> beh_state_;  // parallel to behaviors_
    std::vector<std::array<uint8_t, kBehaviorTypeCount>> behavior_index_;  // 0xFF = absent
    std::vector<uint32_t> free_;  // LIFO free slots

    std::vector<ObjectHandle> all_, serviced_;
    std::vector<std::vector<ObjectHandle>> by_owner_;  // index owner + 1 (owner -1 -> 0)
    std::array<BehaviorTable, kBehaviorTypeCount> tables_;
    std::unordered_map<ObjectId, uint32_t> by_id_;  // lookup only, never iterated
    std::vector<Delayed> delayed_;
    std::vector<ObjectHandle> pending_;
};

// Parent-mode death marking (§2.5), for use as the tactical store's on_parent_mode_death: the
// counterpart (by parent-mode id in parent) gets kFlagMarkedByChildMode when it has the dying
// object's type; otherwise it is a container (fleet / transport) and its first not-yet-marked
// child of that type (insertion order) is marked. Exception (§2.5): a type with placed_at_markers (+0xE3) is not
// marked while another live object of that type and owner remains in child_store. Returns the marked object (null if none).
ObjectHandle mark_parent_mode_object_for_death(ObjectStore& parent, const ObjectStore& child_store, ObjectHandle dying);

}  // namespace alamo::objects

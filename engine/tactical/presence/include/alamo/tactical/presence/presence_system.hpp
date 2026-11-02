// R3 - presence-induced idle (docs/re/ANIM_PARTICLES.md §A.11, TACTICAL_GAPS.md §6.2 as corrected by §A.11):
// the IMPOSING_PRESENCE service (0x624ab0), the presence-map part of the IDLE service (0x62e570) and the default idle
// type (0x395920: 71 Idle Crouch under SPREAD_OUT, else the presence-induced idle, else 0).
//
// HOST CONTRACT
//   * Definitions: set_types() once per battle with a PresenceTypeTable indexed by the host TypeIndex
//     (presence_data.hpp builds it from GameObjectTypes). Not saved.
//   * Attach: add_presence_unit(id, frame, &synced_rng) when an object with IMPOSING_PRESENCE is created, in its
//     attach-order position (the behaviour's first-due draw: synced Get_Int(0, 29), TACTICAL_GAPS §6.4; nullptr = no
//     draw, due at once). remove_object(id) when an object leaves the mode (drops its registration and its own map;
//     entries keyed by it in other maps stay and time out, as in the original).
//   * Per frame, in the object-service phase, either
//       - update(frame, world): every presence unit and every unit holding a map, newest first (descending id,
//         OBJECT_RUNTIME §6), per object the IDLE maintenance first, then the IMPOSING_PRESENCE service; or
//       - from the host's own per-object loop: service_idle(id, world) from each IDLE unit's IDLE service and
//         service_presence(id, frame, world) from each IMPOSING_PRESENCE unit's service (both are cheap no-ops when
//         nothing is due / no map exists).
//   * Reads: default_idle(id, spread_out, world) / presence_idle(id, world) wherever 0x395920 is called (model
//     creation, SPREAD_OUT toggle, locomotor stop / turn paths, ability ends, the IDLE service).
//   * The PresenceWorld callbacks must be read-only and must not create or destroy objects.
//
// DETERMINISM: no draws except the attach draw from the passed synced SimRng. Presence units and maps are kept in
// vectors sorted by object id. Each map's entry order reproduces the original's MSVC hash-map list order exactly
// (A1_READINGS §30.1 rules: minstd((id ^ 0xDEADBEEF) & 0x7FFFFFFF) & (buckets - 1), 8 buckets, max load 1.0, x8
// growth): a pure function of the insertion history, saved verbatim. The winner rule walks that order.
#pragma once
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
class SimRng;
}  // namespace alamo

namespace alamo::tactical::presence {

using ObjectId = int32_t;  // gameplay object id (objects::ObjectId)
using TypeId = uint32_t;   // host TypeIndex (objects::TypeIndex)
inline constexpr ObjectId kNoObject = 0x3FFFFF;
inline constexpr TypeId kNoType = 0xFFFFFFFFu;

// ModelAnimType ids used here (anim_types.hpp).
inline constexpr int32_t kNoAnimType = -1;
inline constexpr int32_t kAnimIdle = 0;
inline constexpr int32_t kAnimIdleCrouch = 71;
inline constexpr int32_t kAnimAttention = 77;
inline constexpr int32_t kAnimCelebrate = 78;

inline constexpr uint32_t kDefaultFps = 30;
inline constexpr uint32_t kPresenceServiceInterval = 30;    // IMPOSING_PRESENCE behaviour interval
inline constexpr float kDefaultRelaxationSeconds = 5.0f;     // GameConstants Unit_Presence_Relaxation_Time (+0x117a8)

// One Presence_Induced_Animations entry (affected type +0x1610, 0x28-byte element): the anim type and the presence
// types that induce it (type-pointer equality: a variant is its own type).
struct InducedAnimation {
    int32_t anim_type = kNoAnimType;
    std::vector<TypeId> types;
};

// Per-type data the services read.
struct PresenceTypeInfo {
    std::vector<InducedAnimation> induced;  // Presence_Induced_Animations (affected side), list order
    float presence_range = 0.0f;            // Presence_Range (+0x1628, presence side): cube half-extent
    bool allow_idle_when_moving = false;    // Allow_Idle_When_Moving (+0x85)
    bool deploys = false;                   // Deploys (+0xd1)
};

class PresenceTypeTable {
public:
    void resize(size_t n) { types_.resize(n); }
    size_t size() const { return types_.size(); }
    PresenceTypeInfo& at(TypeId t) { return types_[t]; }
    // nullptr for kNoType / out of range.
    const PresenceTypeInfo* find(TypeId t) const { return t < types_.size() ? &types_[t] : nullptr; }
    // 0x370630: does `affected`'s list name `presence` in any entry?
    bool lists(TypeId affected, TypeId presence) const;
    // 0x373530: anim type of the first entry of `affected` whose list contains `presence`; kNoAnimType if none.
    int32_t induced_anim(TypeId affected, TypeId presence) const;

private:
    std::vector<PresenceTypeInfo> types_;
};

// What the services read about one live object (the GOM id lookup succeeded; dying objects are still live).
struct UnitView {
    TypeId type = kNoType;
    int32_t owner = -1;            // obj +0x58, or the planet owner for a planet (+0x335 != -1)
    float x = 0.0f, y = 0.0f, z = 0.0f;
    bool dying = false;            // dying or removed (obj +0x3a0 & 0x42)
    bool has_idle = false;         // IDLE behaviour attached (+0x339 != -1)
    // TARGETING (9) has a target (vf +0xe0); for a unit without its own TARGETING, its parent's (obj +0x2b0) when
    // its locomotor's vf +0x228 is true (0x62e450).
    bool has_target = false;
    bool has_locomotor = false;
    bool moving = false;           // locomotor vf +0x110
    // Deployed, deploying or undeploying (0x39a400, 0x39b2a0, 0x39a390); only read for Deploys types.
    bool deploy_busy = false;
};

class PresenceWorld {
public:
    virtual ~PresenceWorld() = default;
    // GOM id lookup: false when no live object has `id`.
    virtual bool unit(ObjectId id, UnitView& out) const = 0;
    // Ids of every object of the mode whose position lies in the cube [c - h, c + h] on all three axes. A superset is
    // fine (the system re-tests the positions, inclusive); any order, duplicates allowed.
    virtual void box_query(float cx, float cy, float cz, float h, std::vector<ObjectId>& out) const = 0;
    // Owner relation == 0 (0x2823e0, "allied"); must be true for a == b.
    virtual bool allied(int32_t a, int32_t b) const = 0;
};

struct PresenceConfig {
    uint32_t fps = kDefaultFps;
    uint32_t interval = kPresenceServiceInterval;
    float relaxation_seconds = kDefaultRelaxationSeconds;
    // int(FPS x Unit_Presence_Relaxation_Time): 150 in vanilla.
    int32_t relaxation_frames() const;
};

struct PresenceEntry {
    ObjectId presence = kNoObject;  // key: the IMPOSING_PRESENCE object
    int32_t remaining = 0;          // frames, >= 1 while stored
};

// Result of service_idle (the map part of 0x62e570).
enum class IdleMaintenance : uint8_t {
    None,           // no map (or empty)
    ClearedMoving,  // locomotor moving without Allow_Idle_When_Moving: map cleared (the host also drops the flinch hold)
    ClearedTarget,  // TARGETING has a target: map cleared (the host's IDLE service sets the combat timer)
    Frozen,         // Deploys type deployed / (un)deploying: entries kept, not aged
    Aged,           // every entry decremented, those below 1 removed
    Dropped,        // the unit is no longer live: its map was dropped [port]
};

struct PresenceStats {
    uint32_t services = 0;   // IMPOSING_PRESENCE services run
    uint32_t refreshes = 0;  // entries written (inserted or refreshed)
    uint32_t expired = 0;    // entries removed at < 1
    uint32_t cleared = 0;    // maps cleared by the move / target gates
};

class PresenceSystem {
public:
    explicit PresenceSystem(PresenceConfig cfg = {}) : cfg_(cfg) {}

    void set_config(PresenceConfig cfg) { cfg_ = cfg; }
    const PresenceConfig& config() const { return cfg_; }
    void set_types(PresenceTypeTable types) { types_ = std::move(types); }
    const PresenceTypeTable& types() const { return types_; }

    // IMPOSING_PRESENCE attach: first due = frame + rng->range_int(0, interval - 1) (interval >= 2), else frame.
    // False (no draw) when `id` is already registered.
    bool add_presence_unit(ObjectId id, uint32_t frame, SimRng* rng);
    bool remove_presence_unit(ObjectId id);
    bool is_presence_unit(ObjectId id) const;
    uint32_t next_due(ObjectId id) const;  // 0xFFFFFFFF when not registered
    // The object left the mode: presence registration and its own map dropped.
    void remove_object(ObjectId id);

    // IMPOSING_PRESENCE service 0x624ab0 for one presence unit: nothing unless due (frame >= due); then
    // due = frame + interval and every qualifying unit in the cube of half-extent Presence_Range around it gets
    // map[id] = relaxation_frames(). Returns the entries written.
    uint32_t service_presence(ObjectId id, uint32_t frame, const PresenceWorld& world);
    // Presence-map part of the IDLE service 0x62e570 for one affected unit (interval 1, every frame).
    IdleMaintenance service_idle(ObjectId id, const PresenceWorld& world);
    // Both services for the whole frame in object-service order (see the header comment).
    void update(uint32_t frame, const PresenceWorld& world);

    // 0x395920 step 2: the presence-induced idle, or kNoAnimType when the map is empty, no key is live, or no entry
    // of the unit's type lists the winner's type.
    int32_t presence_idle(ObjectId id, const PresenceWorld& world) const;
    // 0x395920: 71 when `spread_out` (unit ability 7 active), else presence_idle() when the map is not empty (may be
    // kNoAnimType, see there), else 0.
    int32_t default_idle(ObjectId id, bool spread_out, const PresenceWorld& world) const;
    // Winner of the map walk (kNoObject when the map is empty or no key is live).
    ObjectId winner(ObjectId id, const PresenceWorld& world) const;

    // Introspection.
    std::span<const PresenceEntry> entries(ObjectId id) const;  // list order
    int32_t remaining(ObjectId affected, ObjectId presence) const;  // 0 when absent
    size_t map_count() const { return maps_.size(); }
    size_t presence_unit_count() const { return units_.size(); }
    const PresenceStats& stats() const { return stats_; }

    void hash(StateHasher& h) const;
    // N1 save (save_file.hpp contract): u32 version, presence units (id, due) ascending, maps (affected id, bucket
    // count, entries in list order) ascending. Config and types are definitions (not saved).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

    // Bucket index of a key for `buckets` (a power of two): minstd((id ^ 0xDEADBEEF) & 0x7FFFFFFF) & (buckets - 1).
    static uint32_t bucket_of(ObjectId key, uint32_t buckets);

    struct Map {
        ObjectId owner = kNoObject;  // the affected unit
        uint32_t buckets = 8;
        std::vector<PresenceEntry> list;  // MSVC list order
    };
    struct PresenceUnit {
        ObjectId id = kNoObject;
        uint32_t due = 0;
    };

private:
    Map* find_map(ObjectId id);
    const Map* find_map(ObjectId id) const;
    Map& map_for(ObjectId id);
    void drop_map_if_empty(ObjectId id);
    static void insert_or_refresh(Map& m, ObjectId key, int32_t value);

    PresenceConfig cfg_;
    PresenceTypeTable types_;
    std::vector<PresenceUnit> units_;  // ascending id
    std::vector<Map> maps_;            // ascending owner id; only non-empty maps or maps that grew past 8 buckets
    PresenceStats stats_;              // diagnostics, not state
};

}  // namespace alamo::tactical::presence

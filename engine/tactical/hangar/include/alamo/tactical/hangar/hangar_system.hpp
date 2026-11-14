// Hangars - SPAWN_SQUADRON (behaviour 0x1A, SpawnSquadronBehaviorClass) for space stations and carriers, and the
// hangar / hardpoint part of the skirmish station upgrade (Starbase_Upgrade_Ability).
// Spec: docs/re/STATION_HANGARS_SOUND.md §1-§2 (with HARDPOINT_DISABLED.md §3 / §9.4, ABILITIES_HARDPOINTS.md §2.4 /
// §2.5 for DEPLOY_SQUAD). Space spawners only: the land structure path (bones, Structure Open / Hold / Close states,
// Spawn_Garrison_On_Load, companies) is not modelled here.
//
// WIRING (host = the battle, engine/apps/sim TacticalBattle)
//   * add_carrier(id, desc, frame, &rng) when the spawner object is created (its combat unit must exist): it makes the
//     behaviour's first-due draw synced Get_Int(0, 29) (TACTICAL_GAPS §6.4), in the object's attach-order position.
//   * update(frame) once per frame, serial, in the object-service phase. Carriers are serviced newest first (descending
//     id, OBJECT_RUNTIME §5.3), each every 30 frames from its own first-due frame. It never creates objects: it
//     appends LaunchRequests (one per carrier per service at most). Per launch the synced draws are, in order:
//     can-spawn entry pick (>= 2 entries), launch entry pick (>= 2 entries), bay pick (>= 2 bays).
//   * The host creates each request (squadron: members at `origin` facing `facing` with velocity `direction` x member
//     max speed, then the team container, spawned flag +0x3A2 |= 2; single ship: the ring search around `flyout`),
//     calls bind_launch(serial, object) with the container / ship id, gives the initial order (escort ? guard the
//     carrier (order kind 3) : formation move to `flyout`) and finally clear_requests() (unbound requests are rolled
//     back: the spawn failed).
//   * squadron_removed(id, frame) when a squadron container / single ship is removed (signal 0x28). Individual fighter
//     deaths do not count.
//   * Carrier death / removal: remove_carrier() (or the next update() notices the dead combat unit). Spawned units
//     survive and are no longer counted by anyone. Retreat: nothing here (the hyperspace query blocks launches; the
//     retreat coordinator treats spawned squadrons like any unit).
//
// DETERMINISM: carriers are kept sorted by id; lists in insertion order; draws only from the synced SimRng passed
// to update() / add_carrier(), in the documented order; no pointer-keyed containers. The state is hashed in
// "<prefix>.carriers" (register_crc).
#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "alamo/core/sim_rng.hpp"
#include "alamo/tactical/combat/combat_system.hpp"

namespace alamo {
class StateHasher;
class CrcRegistry;
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::tactical::abilities {
class AbilitySystem;
}

namespace alamo::tactical::hangar {

using combat::kNoObject;
using combat::ObjectId;

inline constexpr uint32_t kTechSlots = 6;  // Starting / Reserve_Spawned_Units_Tech_0..5

// One (type, count) pair of a Starting_ / Reserve_Spawned_Units_Tech_N list.
struct SpawnEntry {
    int32_t type = -1;  // host TypeIndex (-1 unknown: never spawned)
    int32_t count = 0;  // starting: alive at once; reserve: extra launches, -1 = unlimited
};
using SpawnList = std::vector<SpawnEntry>;

struct HangarDesc {
    std::string name;
    std::array<SpawnList, kTechSlots> starting;  // Starting_Spawned_Units_Tech_N, list order
    std::array<SpawnList, kTechSlots> reserve;   // Reserve_Spawned_Units_Tech_N
    float delay_seconds = 5.0f;                  // Spawned_Squadron_Delay_Seconds
    bool community_property = false;             // Is_Community_Property (skirmish owner rotation)
    bool station = false;                        // station behaviour (+0x38E): skirmish pending-delivery path
    // The type has the DEPLOY_SQUAD (75) unit ability. Used when no AbilitySystem is attached (set_abilities);
    // with one, AbilitySystem::has_ability(id, kDeploySquad) decides.
    bool deploy_squad = false;
};

// Flattened 0x46 list tokens ("Type", "2", "Type", "1", ...) or "Type, 2" pairs -> entries in list order. `resolve`
// maps a type name to the host TypeIndex (-1 unknown; the entry is kept).
SpawnList parse_spawn_list(std::span<const std::string_view> tokens, const std::function<int32_t(std::string_view)>& resolve);
// "Type, 2" or "Type 2" (one pair) -> true.
bool parse_type_count(std::string_view text, std::string& type, int32_t& count);
// Tech slot (§2.1): the highest N <= tech whose list is not empty; -1 when none (tech > 5 gives none).
int32_t tech_slot(const std::array<SpawnList, kTechSlots>& lists, int32_t tech);

struct HangarConfig {
    uint32_t fps = combat::kDefaultFps;
    uint32_t service_interval = 30;  // behaviour service interval (first due = add frame + Get_Int(0, interval - 1))
    bool skirmish = true;            // community-property rotation and the station pending-delivery path
};

// A unit the host must create after update(), request order. Each consumes object ids.
struct LaunchRequest {
    uint32_t serial = 0;
    ObjectId carrier = kNoObject;
    int32_t owner = -1;          // player that owns the spawned unit (data pack +0x60)
    int32_t type = -1;           // host TypeIndex
    int32_t entry = -1;          // index into the carrier's entry list; -1 = the player's pending delivery
    bool from_pending = false;   // skirmish pending-delivery launch (register as reinforcement; local-player SFX)
    int32_t bay = -1;            // FIGHTER_BAY hardpoint index (-1: no bay, never in space)
    Vec3 origin;                 // P0: the bay bone world position
    Vec3 flyout;                 // P1 = P0 + bone X axis x Fighter_Bay_Flyout_Distance
    Vec3 direction;              // normalize(P1 - P0); zero when the flyout distance is 0
    Vec3 facing{1.0f, 0.0f, 0.0f};  // direction, else the bone X axis [port]
    bool escort = false;         // the carrier has a locomotor: order kind 3 (guard the carrier); else move to P1
};

enum class HangarEventKind : uint8_t {
    Launched,      // a request was made (id = carrier, target = kNoObject, value = entry or -1)
    Manual,        // state 4: a human-owned DEPLOY_SQUAD spawner stopped auto-launching (id = carrier)
    SquadronLost,  // a counted squadron was removed (id = carrier, target = squadron, value = matched entry or -1)
    BaysGone,      // no fighter bay left (all erased, or none ever): this spawner never launches (id = carrier)
    CarrierLost,   // update() found the carrier dead (id = carrier); its spawned units stay, uncounted
};
struct HangarEvent {
    HangarEventKind kind = HangarEventKind::Launched;
    ObjectId id = kNoObject;
    ObjectId target = kNoObject;
    int32_t value = 0;
    uint32_t frame = 0;
};

// Host queries, all optional (unset = the documented fallback). Called from update(); they must not create objects.
struct HangarHost {
    std::function<int32_t(int32_t player)> tech_level;             // player +0x84; unset 0
    std::function<bool()> cinematic_camera;                        // §2.4 item 1; unset false
    std::function<bool(int32_t owner)> corruption_blocked;         // §2.4 item 2 (Corruption_Bribery planet and the
                                                                   // owner's faction lacks Benefits_From_Corruption)
    std::function<bool(ObjectId carrier)> in_hyperspace;           // §2.4 item 3; unset false
    std::function<bool(int32_t type)> planet_restricted;           // §2.4 item 7; unset false
    std::function<bool(int32_t a, int32_t b)> allied;              // unset: a == b || !combat.is_enemy(a, b)
    // §2.4 item 8 / §2.5: the first type of `player`'s pending space-delivery list (-1 empty); `pop` removes it.
    // Unset: always empty.
    std::function<int32_t(int32_t player, bool pop)> pending_delivery;
    // Bay bone world position and its X axis (hardpoint bone +0x2C, else +0x38). False: [port] the hardpoint's
    // attachment offset in world space and the carrier's facing.
    std::function<bool(ObjectId carrier, uint32_t hardpoint, Vec3& pos, Vec3& x_axis)> bay_bone;
    // DEFEND (1) active on the carrier; unset: the attached AbilitySystem (is_active), else false.
    std::function<bool(ObjectId carrier)> defend_active;
};

class HangarSystem {
public:
    HangarSystem(combat::CombatSystem& combat, std::vector<HangarDesc> descs, HangarConfig config = {});
    HangarSystem(const HangarSystem&) = delete;
    HangarSystem& operator=(const HangarSystem&) = delete;

    void set_host(HangarHost h) { host_ = std::move(h); }
    // DEFEND / DEPLOY_SQUAD queries (is_active / has_ability). Optional.
    void set_abilities(const abilities::AbilitySystem* a) { abilities_ = a; }
    const HangarConfig& config() const { return cfg_; }
    const HangarDesc& desc(uint32_t i) const { return descs_[i]; }

    // A spawner object was created. `rng` (synced) makes the first-due draw Get_Int(0, interval - 1); nullptr: due at
    // `frame`. The entry list and bay list are built on the first service. +0x3CB (spawn enabled) starts at 1.
    // False for an unknown unit / descriptor or a duplicate.
    bool add_carrier(ObjectId id, uint32_t hangar_desc, uint32_t frame, SimRng* rng);
    // Silent removal (death, station upgrade, retreat end): spawned units stay, uncounted. No event.
    void remove_carrier(ObjectId id);
    // Binds a request to the created squadron container / single ship. False for an unknown serial.
    bool bind_launch(uint32_t serial, ObjectId object);
    // The host removed a squadron container / single ship (§2.7). Unknown ids are ignored.
    void squadron_removed(ObjectId object, uint32_t frame);
    // Lua Set_Garrison_Spawn (obj +0x3CB).
    void set_spawn_enabled(ObjectId carrier, bool on);
    // DEPLOY_SQUAD handler (ABILITIES_HARDPOINTS §2.5, inline in 0x38a350): resets SPAWN_SQUADRON (state 0) and sets the
    // next spawn frame to frame + int((recharge_seconds - 1) x FPS). [L] the next launch after a reset is not turned
    // back into state 4 (otherwise a manual spawner could never launch); the one after it is.
    void deploy_squad(ObjectId carrier, uint32_t frame, float recharge_seconds);

    // Serial, once per frame: carrier deaths, then the due services (descending id).
    // SP5 t03 (docs/SIM_PARALLEL_DESIGN.md §4.4): with `stream_of`, each serviced carrier's draws (the entry and bay
    // picks) come from stream_of(carrier id) - a counter-based stream of its own - instead of `rng`; the order of the
    // services and every other effect are unchanged. Null: the synced draws (retail order).
    using StreamOf = std::function<SimRng(ObjectId carrier)>;
    void update(uint32_t frame, SimRng& rng, const StreamOf* stream_of = nullptr);

    // ---- queries ----
    bool has_carrier(ObjectId id) const { return find(id) >= 0; }
    bool spawn_enabled(ObjectId carrier) const;
    bool deploy_ready(ObjectId carrier) const;                 // DEPLOY_SQUAD gate: state 4
    uint8_t state(ObjectId carrier) const;                     // 0 idle, 4 manual
    uint32_t entry_count(ObjectId carrier) const;              // 0 until the first service
    int32_t live(ObjectId carrier, uint32_t entry) const;      // counted units of the entry
    int32_t max_alive(ObjectId carrier, uint32_t entry) const; // starting count
    int32_t remaining(ObjectId carrier, uint32_t entry) const; // launches left, -1 unlimited
    int32_t entry_type(ObjectId carrier, uint32_t entry) const;
    uint32_t bay_count(ObjectId carrier) const;                // cached bays (destroyed ones erased at can-spawn)
    uint32_t next_spawn_frame(ObjectId carrier) const;         // +0x64
    uint32_t next_service_frame(ObjectId carrier) const;
    int32_t spawn_player(ObjectId carrier) const;              // +0x60
    ObjectId carrier_of(ObjectId object) const;                // kNoObject when not a counted spawned unit

    std::span<const LaunchRequest> launches() const { return requests_; }
    std::span<const HangarEvent> events() const { return events_; }
    // Host: once per frame after consuming launches and events. Requests not bound are rolled back (live -1,
    // remaining +1): the spawn failed.
    void clear_requests();

    void hash(StateHasher& h) const;
    void register_crc(CrcRegistry& registry, const std::string& prefix = "hangar") const;
    // N1 stage 2 (save_file.hpp contract, hangar_save.cpp). Between ticks. Every carrier (ascending id: timers, state
    // bytes, cached bays, entries with their live / remaining counts, the counted spawned units with their bind state),
    // the pending launch requests and events, the request serial. read() checks the descriptor count, validates the
    // indices, requires r.done() and leaves the system unchanged on failure. Not saved: the combat / ability pointers,
    // HangarHost, config and descriptors (re-attached / rebuilt by the host). No host call is needed after read.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Entry {
        int32_t type = -1;
        int32_t live = 0;
        int32_t max = 0;
        int32_t remaining = 0;  // -1 unlimited
    };
    struct Spawned {
        ObjectId id = kNoObject;  // kNoObject while unbound
        uint32_t serial = 0;
        int32_t type = -1;
        int32_t entry = -1;       // -1: pending delivery (not counted in an entry)
        uint8_t spent = 0;        // remaining was decremented
    };
    struct Carrier {
        ObjectId id = kNoObject;
        uint32_t desc = 0;
        int32_t owner = -1;
        int32_t spawn_player = -1;  // +0x60
        uint32_t due = 0;           // next behaviour service
        uint32_t next = 0;          // +0x64 next spawn frame
        uint8_t state = 0;          // +0x50: 0 idle, 4 manual
        uint8_t enabled = 1;        // +0x3CB
        uint8_t deploy_latch = 0;   // [L] one launch after a DEPLOY_SQUAD reset
        uint8_t bays_gone = 0;      // BaysGone reported
        std::vector<uint32_t> bays;   // cached FIGHTER_BAY hardpoint indices
        std::vector<Entry> entries;
        std::vector<Spawned> spawned;  // counted units (launch order)
    };

    int32_t find(ObjectId id) const;
    Carrier* carrier(ObjectId id);
    const Carrier* carrier(ObjectId id) const;
    void build(Carrier& c, uint32_t frame);
    bool eligible(const Carrier& c, const Entry& e) const;
    bool can_spawn(Carrier& c, uint32_t frame, SimRng& rng);
    void launch(Carrier& c, uint32_t frame, SimRng& rng);
    void emit(Carrier& c, int32_t type, int32_t entry, bool pending, uint32_t frame, SimRng& rng);
    bool allied(int32_t a, int32_t b) const;
    bool has_deploy_squad(const Carrier& c) const;
    bool defend_on(const Carrier& c) const;

    combat::CombatSystem* combat_;
    const abilities::AbilitySystem* abilities_ = nullptr;
    std::vector<HangarDesc> descs_;
    HangarConfig cfg_;
    HangarHost host_;
    std::vector<Carrier> carriers_;  // sorted by id
    std::vector<LaunchRequest> requests_;
    std::vector<HangarEvent> events_;
    uint32_t serial_ = 1;
};

// Do_Starbase_Upgrade (0x70f070, STATION_HANGARS_SOUND.md §1.2) steps 4 and 9 plus the hangar side of step 10, for
// the host after it created `new_station` (S's type's Next_Level_Base, S's owner / position / facing; its combat unit
// exists and, when `hangars` is given and the type spawns, it is already add_carrier'ed with its own first-due draw):
//   step 4: hardpoint state copy by index (CombatSystem::copy_upgrade_hardpoints);
//   step 9: the +0x3CB spawn flag S -> S';
//   step 10 (hangar part): S is dropped from the hangar silently; its spawned units stay, uncounted, and S' launches a
//   full new complement. Nothing else is carried (hull, shields, cooldowns, hangar counts / timers).
// Steps 5-8, 10 (the silent removal) and 11 are the host's (see abilities::SpawnPurpose::StationUpgrade).
// False when either station is not a combat unit.
bool complete_station_upgrade(combat::CombatSystem& combat, HangarSystem* hangars, ObjectId old_station,
                              ObjectId new_station, uint32_t frame);

}  // namespace alamo::tactical::hangar

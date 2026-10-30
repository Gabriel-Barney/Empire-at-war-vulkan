// S4 - land garrisons (units entering structures / vehicles, garrison fire, exit): the occupancy state the battle host
// and the Lua bindings (Can_Garrison, Garrison, Leave_Garrison, Can_Garrison_Fire, Is_In_Garrison, Get_Garrisoned_Units,
// Eject_Garrison) read. Spec: docs/re/L1_S4_REMAINING.md §3.5 (GarrisonUnitBehaviorClass / host behaviours
// GARRISON_STRUCTURE 0x50, GARRISON_VEHICLE 0x4F, GARRISON_HOVER_TRANSPORT 0x56) and §4.1 (STORY_GARRISON_UNIT), plus
// OBJECT_TYPES.md (Garrison_Category, CanContainGarrison 0x1000 / GarrisonCanFire 0x2000), TACTICAL_COMBAT.md 2.2 / 2.3.
//
// Rules ([H] = the spec, [port] = this port's stand-in):
//   * masks [H]: the unit's Garrison_Category and the host's Garrison_Category intersect; a zero mask on either side
//     always matches. Free space [H] = Num_Garrison_Slots - sum of the occupants' Garrison_Value (an occupant with
//     value <= 0 counts 0 [port: the field is unset on such types]); entering needs free >= the unit's value.
//   * hostility [H]: the host's owner must be allied with the unit's owner (the host passes `allied`; default: same
//     owner only), or be unowned / the neutral player (§2.5 faction byte +0x108; see Can_Garrison below).
//   * order (Lua Garrison / command 55 / command 52 with a host) [H]: needs a GARRISON_UNIT unit, an allied host with a
//     host behaviour (or CanContainGarrison) and matching masks; the unit walks to the approach point (host position +
//     its 2D facing x Garrison_Enter_Dist if > 0, else half the host box diagonal [H: the no-bone case; Garrison_Bone_Names
//     are not resolved in the sim, port]). Approach [H]: cancelled when the host is gone / hostile or free < value;
//     2D distance - the unit's radius <= max(Garrison_Radius, reach_slack [port: the 0x140a28bcc constant]) -> entering
//     (EnterStarted: the host stops the unit); entering lasts enter_frames (the fade 1 -> 0.1 at 0.03 per frame = 30
//     [H rates, port count]); then the capacity check and Entered (STORY_GARRISON_UNIT), or OrderDropped.
//   * leaving [H]: Leave_Garrison / command 52 with unit + 0x3FFFFF -> the unit leaves at once (Exited) to the exit point
//     (host position + 2D facing x Garrison_Exit_Dist if > 0, else the full box diagonal) [port: no fade-in / walk-out,
//     no locomotor wait]; on a unit still approaching it cancels the approach. Eject_Garrison / command 52 with unit
//     0x3FFFFF: every occupant (Ejected).
//   * host death [H]: occupants are ejected (Ejected) when the host has a garrison behaviour, else killed (Killed: the
//     host kills them, credit to the host's killer). Host owner change [H]: every occupant is ejected.
//   * Can_Garrison (Lua, §2.5) [H]: the host has a host behaviour; its owner is allied, unowned or the neutral player
//     (faction byte +0x108); the unit is known (GARRISON_UNIT), not inside / entering, masks match and free - reserved
//     (units walking in) - the unit's value > 0 (stricter than the engine's entry rule free >= value). The order uses
//     the same owner test [port: doc open item 4, Can_Garrison vs Garrison for neutral hosts].
//   * Can_Garrison_Fire [port, doc open item 4: host vf +0xD8]: a host with GarrisonCanFire, or an occupant of one.
// Determinism: serial, holders / units in vectors sorted by id, occupants in entry order, events in the order they
// happen; floats only through + - * / and dmath::sqrt. hash() covers all state.
#pragma once
#include <cstdint>
#include <functional>
#include <span>
#include <utility>
#include <vector>

#include "alamo/core/galaxy.hpp"
#include "alamo/core/sim_crc.hpp"

namespace alamo {
class ByteWriter;
class ByteReader;
}  // namespace alamo

namespace alamo::tactical::garrison {

using ObjectId = int32_t;
inline constexpr ObjectId kNoObject = 0x3FFFFF;
using Vec3 = alamo::Vec3;  // the store's vector (not alamo::tactical::Vec3), whatever else the includer saw first

enum class HolderKind : uint8_t {
    Vehicle = 0,    // GARRISON_VEHICLE (0x4F)
    Structure = 1,  // GARRISON_STRUCTURE (0x50)
    Hover = 2,      // GARRISON_HOVER (0x56)
    Property = 3,   // CanContainGarrison only (no host behaviour: occupants die with the host)
};

struct HolderDesc {
    HolderKind kind = HolderKind::Structure;
    uint64_t accepts = 0;        // Garrison_Category of the host
    int32_t slots = 0;           // Num_Garrison_Slots
    float enter_dist = 0.0f;     // Garrison_Enter_Dist
    float exit_dist = 0.0f;      // Garrison_Exit_Dist
    float radius = 0.0f;         // Garrison_Radius
    bool can_fire = false;       // Property_Flags GarrisonCanFire
    float half_diagonal = 0.0f;  // half the host box diagonal (XY), from the host's bounds
};

struct OccupantDesc {
    uint64_t category = 0;  // the unit's own Garrison_Category
    int32_t value = 0;      // Garrison_Value (slots taken)
    float radius = 0.0f;    // the unit's footprint radius
};

struct GarrisonConfig {
    float reach_slack = 10.0f;  // [port] lower bound of the entering distance (the 0x140a28bcc constant is not read)
    uint32_t enter_frames = 30;  // [port] frames of the entering fade
};

enum class GarrisonEventKind : uint8_t {
    EnterStarted = 0,  // the unit reached the approach point: the host stops it (position = the approach point)
    Entered = 1,       // the unit is inside (STORY_GARRISON_UNIT); position = the host's
    Exited = 2,        // Leave_Garrison / command 52: the unit left to `position`
    Ejected = 3,       // the host was destroyed / changed owner / Eject_Garrison: the unit left to `position`
    Killed = 4,        // the host died without a garrison behaviour: the host kills the unit
    OrderDropped = 5,  // a pending approach / entering was cancelled (host gone / hostile / full / Leave_Garrison)
};

struct GarrisonEvent {
    GarrisonEventKind kind = GarrisonEventKind::Entered;
    ObjectId unit = kNoObject;
    ObjectId holder = kNoObject;
    int32_t owner = -1;  // the unit's owner
    Vec3 position{};
    uint32_t frame = 0;
};

// A live object's position and facing (2D direction in x / y; need not be normalised). False = gone.
using PoseFn = std::function<bool(ObjectId id, Vec3& position, Vec3& facing)>;

class GarrisonSystem {
public:
    explicit GarrisonSystem(GarrisonConfig cfg = {}) : cfg_(cfg) {}

    const GarrisonConfig& config() const { return cfg_; }
    // Relation test (player +0x370 relation == 0); default: same owner only.
    void set_allied(std::function<bool(int32_t a, int32_t b)> fn) { allied_ = std::move(fn); }
    bool allied(int32_t a, int32_t b) const { return a == b || (allied_ && allied_(a, b)); }
    void set_neutral_player(int32_t p) { neutral_ = p; }  // hosts of this owner are open to every player (§2.5)
    int32_t reserved(ObjectId holder) const;              // units walking in (approach in progress)

    // ---- registration (host, at spawn) ----
    bool add_holder(ObjectId id, int32_t owner, const HolderDesc& d);  // false: already known
    bool add_unit(ObjectId id, int32_t owner, const OccupantDesc& d);  // an object may be both (Juggernaut)
    // Death / retreat / removal at `pose` of the host: a host ejects (garrison behaviour) or kills (Property) its
    // occupants; approaches to it are dropped; a unit leaves its host silently. Unknown ids: no-op.
    void remove(ObjectId id, Vec3 pos, Vec3 facing, uint32_t frame);
    // Owner change: a host with occupants ejects them all (at the given pose); a unit only changes owner.
    void set_owner(ObjectId id, int32_t owner, Vec3 pos = {}, Vec3 facing = {}, uint32_t frame = 0);
    bool is_holder(ObjectId id) const { return holder_index(id) >= 0; }
    bool is_unit(ObjectId id) const { return unit_index(id) >= 0; }
    const HolderDesc* holder_desc(ObjectId id) const;
    const OccupantDesc* unit_desc(ObjectId id) const;

    // ---- queries (Lua bindings, AI, HUD) ----
    bool masks_match(ObjectId unit, ObjectId holder) const;
    bool can_garrison(ObjectId unit, ObjectId holder) const;  // order rules + free >= value, unit not inside / entering
    bool can_garrison_fire(ObjectId id) const;                // holder or occupant (see the header)
    ObjectId holder_of(ObjectId unit) const;                  // kNoObject when not inside
    bool garrisoned(ObjectId unit) const { return holder_of(unit) != kNoObject; }
    std::span<const ObjectId> occupants(ObjectId holder) const;  // entry order
    int32_t capacity(ObjectId holder) const { const HolderDesc* d = holder_desc(holder); return d ? d->slots : 0; }
    int32_t used_slots(ObjectId holder) const;
    int32_t free_slots(ObjectId holder) const;  // GarrisonSlotsAvailable (may be < 0 never: clamped at 0)
    ObjectId pending_holder(ObjectId unit) const;  // the host of an approach / entering in progress
    bool entering(ObjectId unit) const;
    Vec3 approach_point(ObjectId holder, Vec3 pos, Vec3 facing) const;
    Vec3 exit_point(ObjectId holder, Vec3 pos, Vec3 facing) const;

    // ---- orders ----
    // Approach (the caller moves the unit to approach_point): false when the unit / host is unknown, the unit is inside
    // or entering, the host is hostile or the masks do not match. Replaces an earlier approach.
    bool order_enter(ObjectId unit, ObjectId holder);
    bool cancel_enter(ObjectId unit);  // a new move / stop order replaces an approach (not an entering unit)
    // Vehicle_Thief (TACTICAL_GAPS §2.3.10, VEHICLE_THIEF 0x32): `unit` is inside `holder` at once (Entered event at
    // `at`), whatever the owners, masks and free slots [port: the original's board path is not modelled]. A pending
    // approach is replaced. False: unknown ids, the same object, or `unit` already inside / entering.
    bool board(ObjectId unit, ObjectId holder, Vec3 at, uint32_t frame);
    // Leave: inside -> Exited at exit_point(host pose); approaching / entering -> cancelled (OrderDropped). False: none.
    bool order_exit(ObjectId unit, Vec3 holder_pos, Vec3 holder_facing, uint32_t frame);
    uint32_t eject_all(ObjectId holder, Vec3 holder_pos, Vec3 holder_facing, uint32_t frame);  // Ejected events

    // ---- tick ----
    // Approaches and entering units, ascending unit id (see the header).
    void service(uint32_t frame, const PoseFn& pose);

    std::span<const GarrisonEvent> events() const { return events_; }
    void clear_events() { events_.clear(); }
    std::vector<std::pair<ObjectId, ObjectId>> occupancy() const;  // (holder, unit), ascending holder id, entry order

    void hash(StateHasher& h) const;
    // N1 stage 2 (save_file.hpp contract, garrison_save.cpp): the neutral player, every holder (owner, descriptor,
    // occupants in entry order) and unit (owner, descriptor, state, host, entering end frame), the pending events.
    // Not saved: the config and the relation callback (the host's). False (unchanged) on bad data.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    enum class State : uint8_t { None = 0, Moving = 1, Entering = 5, Inside = 7 };  // GarrisonDataPack +8 states
    struct Holder {
        ObjectId id = kNoObject;
        int32_t owner = -1;
        HolderDesc d;
        std::vector<ObjectId> inside;  // entry order
    };
    struct Unit {
        ObjectId id = kNoObject;
        int32_t owner = -1;
        OccupantDesc d;
        State state = State::None;
        ObjectId host = kNoObject;  // Moving / Entering / Inside
        uint32_t enter_at = 0;      // Entering: the frame the fade ends
    };
    int32_t holder_index(ObjectId id) const;
    int32_t unit_index(ObjectId id) const;
    static int32_t value_of(const Unit& u) { return u.d.value > 0 ? u.d.value : 0; }
    static bool mask_ok(uint64_t a, uint64_t b) { return a == 0 || b == 0 || (a & b) != 0; }
    void push(GarrisonEventKind k, const Unit& u, ObjectId holder, Vec3 p, uint32_t frame);
    void leave(Unit& u, Holder& h, GarrisonEventKind kind, Vec3 at, uint32_t frame);
    bool host_open(const Holder& h, int32_t owner) const {
        return h.owner < 0 || h.owner == neutral_ || allied(owner, h.owner);
    }

    GarrisonConfig cfg_;
    int32_t neutral_ = -2;
    std::function<bool(int32_t, int32_t)> allied_;
    std::vector<Holder> holders_;  // ascending id
    std::vector<Unit> units_;      // ascending id
    std::vector<GarrisonEvent> events_;
};

}  // namespace alamo::tactical::garrison

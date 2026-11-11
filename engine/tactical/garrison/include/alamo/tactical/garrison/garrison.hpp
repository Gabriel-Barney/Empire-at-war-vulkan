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
//     host behaviour (or CanContainGarrison) and matching masks. s78n t20 batch 2 (FIGHTER_FLYING §7.4 "Garrison approach
//     in full"; the same code for a squad's container and a single unit, the "key"): the host sends the key on an
//     ordinary checked move to approach_destination (the entry bone nearest it in 3-D whose point it can reach, else the
//     host position + L along the host's yaw, bone -1) and order_enter records the bone. The approach point A
//     (approach_point) is the bone's world point, or the host position for bone -1. Every service in states 1 - 2:
//     free < value -> cancel; 3-D |A - P| - r <= Garrison_Radius -> entering (EnterStarted: the host stops the key). The
//     key's move finishing (ApproachInfo::moving false after the order, [port] the falling edge of its goal): state 2 ->
//     entering; state 1 -> A bit-equal to the A of the previous finish counts a retry (> max_retries -> cancel), else
//     it is stored with the count 0; 3-D distance - r <= 60 -> state 2, HostStop (the host stops), 2-D e = |A - P| - r
//     <= Garrison_Radius -> entering, else CorrectiveMove (a flag-0x20 move of the whole excess toward A); farther ->
//     cancel when the host has a path, else Reissue (the host repeats the approach order). r, the move's end and the
//     host's path come from the host (ApproachFn). Entering lasts enter_frames (the fade 1 -> 0.1 at 0.03 per frame =
//     30 [H rates, port count]); then the capacity check and Entered (STORY_GARRISON_UNIT), or OrderDropped. A cancel
//     issues no stop; OrderDropped carries the state it ended (the host's hold follows only state 2).
//   * leaving [H]: Leave_Garrison / command 52 with unit + 0x3FFFFF -> the unit leaves at once (Exited) to the exit point
//     (host position + 2D facing x Garrison_Exit_Dist if > 0, else the full box diagonal) [port: no fade-in / walk-out,
//     no locomotor wait]; on a unit still approaching it cancels the approach. Eject_Garrison / command 52 with unit
//     0x3FFFFF: every occupant (Ejected).
//   * host death [H]: occupants are ejected (Ejected) when the host has a garrison behaviour, else killed (Killed: the
//     host kills them, credit to the host's killer). Host owner change: occupants not allied with the new owner are
//     ejected (Ejected, entry order); occupants allied with it stay [port: §3.5 names the mass owner change 0x4ca2f0
//     and the bribe 0x5b07c0 as ejecting all occupants - both hand the host to a side hostile to them, so the result
//     is the same; capture complete 0x500e30 (§4.2) changes the owner through 0x38e5a0 to the controller, which for an
//     occupied host is the first occupant's owner (rule b), and ejecting there would throw out the capturers; doc open
//     item 4]. A neutral / unowned new owner is not allied with anyone: every occupant is ejected.
//   * Can_Garrison (Lua, §2.5) [H]: the host has a host behaviour; its owner is allied, unowned or the neutral player
//     (faction byte +0x108); the unit is known (GARRISON_UNIT), not inside / entering, masks match and free - reserved
//     (units walking in) - the unit's value > 0 (stricter than the engine's entry rule free >= value). The order uses
//     the same owner test [port: doc open item 4, Can_Garrison vs Garrison for neutral hosts].
//   * Can_Garrison_Fire [port, doc open item 4: host vf +0xD8]: a host with GarrisonCanFire, or an occupant of one.
//   * vehicle thief (s78n t12 review; HERO_CLASH §8.1 / §8.4.3, REMAINING_GAPS §5.1): the boarding 0x63fae0 puts the
//     thief into ANY vehicle - the container pack is created on demand (0x390740), with or without a garrison
//     behaviour. The host registers such a vehicle as HolderKind::VehicleThief and boards the thief with board().
//     That holder is not a garrison: it takes nobody else (order_enter / can_garrison refuse it, an approach to it is
//     dropped, free_slots is 0), its occupant does not leave by order (order_exit / eject_all do nothing) and stays
//     through an owner change of the vehicle; when the vehicle dies or is removed the occupant is released ALIVE at the
//     vehicle's position (Ejected) - the thief leaves through the vehicle's death (the eject ability kills the vehicle).
//   * squads (s78n t18; FIGHTER_FLYING §7.4) [H]: a squad garrisons as ONE object, its team container K. The host
//     names a unit's group (set_group_fn: K's id, K's type Garrison_Value - default 1, so a squad takes one slot whatever
//     its size - and K's type Garrison_Category); the members keep their own records here ([port]), all with the same
//     state, bone and retry memo. Capacity, the reserved count and the mask tests count a group once with the group's
//     value / category. service() decides a group at its lowest pending member id with K as the key: dropped, entering
//     and Entered together (ascending member id); HostStop / CorrectiveMove / Reissue name the lowest member. There is
//     no "some members garrisoned" state.
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
    // s78n t12 review: a vehicle a vehicle thief boarded that has no holder of its own (the on-demand container pack
    // 0x390740). Entered only through board(); closed to every order; the occupant is released alive when it goes.
    VehicleThief = 4,
};

struct HolderDesc {
    HolderKind kind = HolderKind::Structure;
    uint64_t accepts = 0;        // Garrison_Category of the host
    int32_t slots = 0;           // Num_Garrison_Slots
    float enter_dist = 0.0f;     // Garrison_Enter_Dist
    float exit_dist = 0.0f;      // Garrison_Exit_Dist
    float radius = 0.0f;         // Garrison_Radius
    bool can_fire = false;       // Property_Flags GarrisonCanFire
    float half_diagonal = 0.0f;  // half the host box diagonal (XY), from the host's bounds (the exit point)
    // s78n t20 batch 2 (FIGHTER_FLYING §7.4 item 1): the entry bones - Garrison_Bone_Names resolved on the host's model,
    // names missing on it dropped - host-local (x forward, y left, z up) x Scale_Factor, list order.
    std::vector<Vec3> bones;
};

// s78n t12 review: OccupantDesc::value of a unit that is no GARRISON_UNIT and is known only because a vehicle thief's
// boarding put it into a vehicle (the boarding 0x63fae0 asks for no garrison behaviour). Such a unit is never taken by
// order_enter / can_garrison; it counts 0 slots.
inline constexpr int32_t kBoardOnlyValue = -0x7FFFFFFF - 1;

struct OccupantDesc {
    uint64_t category = 0;  // the unit's own Garrison_Category
    int32_t value = 0;      // Garrison_Value (slots taken; the type default is 1); kBoardOnlyValue: boarded only (see above)
};

struct GarrisonConfig {
    uint32_t enter_frames = 30;  // [port] frames of the entering fade
    float arrived_reach = 60.0f;  // 0x140a28bcc: the range test of a finished approach move (state 1 -> 2)
    uint32_t max_retries = 9;     // s78n t20 batch 2: a finished approach with an unchanged A cancels past this count
};

// s78n t18 (FIGHTER_FLYING §7.4): the squad a unit garrisons with - its team container's id, that container type's
// Garrison_Value (type default 1) and Garrison_Category.
struct GarrisonGroup {
    ObjectId id = kNoObject;
    int32_t value = 1;
    uint64_t category = 0;
};
// A unit's group; false = it garrisons on its own. Const, deterministic; called from the serial garrison code.
using GroupFn = std::function<bool(ObjectId unit, GarrisonGroup& out)>;
// s78n t20 batch 2 (FIGHTER_FLYING §7.4 items 3 - 5): per approaching key (a squad's container or a single unit) and
// service call - the radius r the range tests subtract (0x443ce0: the 3-D length of the half-extent vector of the key's
// world box; a squad: the members' live union box), whether the key's move still runs (its end is the "move finished"
// signal) and whether the host has a path (0x39ade0). Defaults (no hook, or false): r 0, moving, no host path.
struct ApproachInfo {
    float radius = 0.0f;
    bool moving = true;
    bool host_path = false;
};
using ApproachFn = std::function<bool(ObjectId key, ObjectId host, ApproachInfo& out)>;
// Whether the key can reach a point (0x50eeb0: both cells pass its class and lie in one connected region).
using ReachableFn = std::function<bool(Vec3 point)>;

enum class GarrisonEventKind : uint8_t {
    EnterStarted = 0,  // the unit reached the approach point: the host stops it (position = the approach point)
    Entered = 1,       // the unit is inside (STORY_GARRISON_UNIT); position = the host's
    Exited = 2,        // Leave_Garrison / command 52: the unit left to `position`
    Ejected = 3,       // the host was destroyed / changed to a hostile owner / Eject_Garrison: left to `position`
    Killed = 4,        // the host died without a garrison behaviour: the host kills the unit
    OrderDropped = 5,  // a pending approach / entering was cancelled (host gone / hostile / full / Leave_Garrison)
    // s78n t20 batch 2 (FIGHTER_FLYING §7.4 items 4 - 5), one per key, naming its lowest member:
    HostStop = 6,        // a finished approach within 60 of A (state 2, 0x443700): the host stops (0x3a9880)
    CorrectiveMove = 7,  // ... and still beyond Garrison_Radius in 2-D: a flag-0x20 move of the key to `position`
    Reissue = 8,         // a finished approach beyond 60, the host without a path: the host repeats the approach order
};

struct GarrisonEvent {
    GarrisonEventKind kind = GarrisonEventKind::Entered;
    ObjectId unit = kNoObject;
    ObjectId holder = kNoObject;
    int32_t owner = -1;  // the unit's owner
    Vec3 position{};
    uint32_t frame = 0;
    uint8_t was = 0;  // s78n t20 batch 2: OrderDropped - the state it ended (1 approaching, 2 closing in, 5 entering)
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
    void set_group_fn(GroupFn fn) { group_ = std::move(fn); }  // s78n t18: squads (not saved: the host's, like set_allied)
    int32_t reserved(ObjectId holder) const;              // units walking in (approach in progress)

    // ---- registration (host, at spawn) ----
    bool add_holder(ObjectId id, int32_t owner, const HolderDesc& d);  // false: already known
    bool add_unit(ObjectId id, int32_t owner, const OccupantDesc& d);  // an object may be both (Juggernaut)
    // Death / retreat / removal at `pose` of the host: a host ejects (garrison behaviour) or kills (Property) its
    // occupants; approaches to it are dropped; a unit leaves its host silently. Unknown ids: no-op. s78n t20 batch 2
    // (MOVEMENT §5.3 "Hover in full", death 0x391310): `kill` - the occupants are killed whatever the kind (a
    // GARRISON_HOVER host that died off the ground; the host decides, it knows the hover state).
    void remove(ObjectId id, Vec3 pos, Vec3 facing, uint32_t frame, bool kill = false);
    // Owner change: a host ejects the occupants not allied with the new owner (at the exit point of the given pose),
    // allied ones stay; approaches are re-checked at the next service; a unit only changes owner.
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
    // s78n t20 batch 2: the GarrisonDataPack +8 state (0 none, 1 approaching, 2 closing in, 5 entering, 7 inside; 0 for an
    // unknown unit), the approach's bone index (-1: none) and the retry memo's count.
    uint8_t state_of(ObjectId unit) const;
    int32_t bone_of(ObjectId unit) const;
    uint32_t retries_of(ObjectId unit) const;
    // A (0x443a90) for a host at `pos` / `facing`: the world point of entry bone `bone` (host-local, turned by the host's
    // yaw [port: no pitch / roll], + Garrison_Enter_Dist along the 2-D host -> bone direction when > 0, the bone's z kept);
    // the host position for bone < 0 or a bone the host does not have.
    Vec3 approach_point(ObjectId holder, Vec3 pos, Vec3 facing, int32_t bone = -1) const;
    // The approach move's destination (0x3ad880) for a key at `from`: the entry-bone point (approach_point) nearest `from`
    // in 3-D (strict <: the first in list order wins a tie) that `reachable` accepts, `bone` := its index; none -> the host
    // position + L (cos, sin)(host yaw), L = Garrison_Enter_Dist if > 0, else half of `extent_len` (the 3-D length of the
    // host box's half-extent vector), `bone` := -1. [port] retail's keeping of a nearer unreachable bone's point is not
    // copied.
    Vec3 approach_destination(ObjectId holder, Vec3 pos, Vec3 facing, Vec3 from, float extent_len, const ReachableFn& reachable,
                              int32_t& bone) const;
    Vec3 exit_point(ObjectId holder, Vec3 pos, Vec3 facing) const;

    // ---- orders ----
    // Approach (the caller moves the unit to approach_destination): false when the unit / host is unknown, the unit is
    // inside or entering, the host is hostile or the masks do not match. Replaces an earlier approach (state 1, `bone`, a
    // move in flight); the retry memo is kept (nothing but a finished approach writes it).
    bool order_enter(ObjectId unit, ObjectId holder, int32_t bone = -1);
    // s78n t18 review: order_enter's test without the order (the host refuses a garrison order before it sends the
    // units walking): both known, the unit not inside / entering, the host no thief's vehicle, the unit no board-only
    // one, the host open to the unit's owner and the masks matching.
    bool enter_allowed(ObjectId unit, ObjectId holder) const;
    bool cancel_enter(ObjectId unit);  // a new move / stop order replaces an approach (states 1 - 2; no event)
    // Vehicle_Thief (TACTICAL_GAPS §2.3.10, VEHICLE_THIEF 0x32): `unit` is inside `holder` at once (Entered event at
    // `at`), whatever the owners, masks and free slots [port: the original's board path is not modelled]. A pending
    // approach is replaced. False: unknown ids, the same object, or `unit` already inside / entering.
    bool board(ObjectId unit, ObjectId holder, Vec3 at, uint32_t frame);
    // Leave: inside -> Exited at exit_point(host pose); approaching / entering -> cancelled (OrderDropped). False: none.
    bool order_exit(ObjectId unit, Vec3 holder_pos, Vec3 holder_facing, uint32_t frame);
    uint32_t eject_all(ObjectId holder, Vec3 holder_pos, Vec3 holder_facing, uint32_t frame);  // Ejected events

    // ---- tick ----
    // Approaches and entering units, ascending unit id (see the header); `approach` serves the keys' range tests.
    void service(uint32_t frame, const PoseFn& pose, const ApproachFn& approach = {});

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
    enum class State : uint8_t { None = 0, Moving = 1, Closing = 2, Entering = 5, Inside = 7 };  // GarrisonDataPack +8
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
        ObjectId host = kNoObject;  // Moving / Closing / Entering / Inside
        uint32_t enter_at = 0;      // Entering: the frame the fade ends
        // s78n t20 batch 2 (FIGHTER_FLYING §7.4): the approach's bone index (pack +0x30), a move in flight whose end is
        // the "move finished" signal, and the retry memo (GarrisonUnitBehavior +0x40 / +0x44: A at the previous finish,
        // bit for bit, and the count; written only by a finished approach in state 1; [M] zero at creation, as the
        // behaviour's fields)
        int32_t bone = -1;
        bool awaiting = false;
        Vec3 memo{};
        uint32_t retries = 0;
    };
    int32_t holder_index(ObjectId id) const;
    int32_t unit_index(ObjectId id) const;
    static int32_t value_of(const Unit& u) { return u.d.value > 0 ? u.d.value : 0; }
    static bool mask_ok(uint64_t a, uint64_t b) { return a == 0 || b == 0 || (a & b) != 0; }
    static bool thief_holder(const Holder& h) { return h.d.kind == HolderKind::VehicleThief; }  // s78n t12 review
    // s78n t18: the unit's group (false: none), the slots it takes and its category - the group's when it has one
    bool group_of(const Unit& u, GarrisonGroup& g) const { return group_ && group_(u.id, g) && g.id != kNoObject; }
    int32_t slot_value(const Unit& u) const;
    uint64_t category_of(const Unit& u) const;
    static bool pending(State s) { return s == State::Moving || s == State::Closing || s == State::Entering; }
    void service_key(const std::vector<size_t>& mem, ObjectId key, int32_t value, uint32_t frame, const PoseFn& pose,
                     const ApproachFn& approach);
    void drop_key(const std::vector<size_t>& mem, ObjectId host, uint32_t frame, const PoseFn& pose);
    void enter_key(const std::vector<size_t>& mem, ObjectId host, uint32_t frame, const PoseFn& pose, Vec3 at);
    void push(GarrisonEventKind k, const Unit& u, ObjectId holder, Vec3 p, uint32_t frame);
    void leave(Unit& u, Holder& h, GarrisonEventKind kind, Vec3 at, uint32_t frame);
    bool host_open(const Holder& h, int32_t owner) const {
        return h.owner < 0 || h.owner == neutral_ || allied(owner, h.owner);
    }

    GarrisonConfig cfg_;
    int32_t neutral_ = -2;
    std::function<bool(int32_t, int32_t)> allied_;
    GroupFn group_;
    std::vector<Holder> holders_;  // ascending id
    std::vector<Unit> units_;      // ascending id
    std::vector<GarrisonEvent> events_;
};

}  // namespace alamo::tactical::garrison

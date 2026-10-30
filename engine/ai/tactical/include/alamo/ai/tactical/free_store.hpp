// A1 - tactical free store: admission filter, membership and service scheduling
// (docs/re/AI_TACTICAL.md 5.1-5.4; framework AI_GALACTIC.md 1.10). The Lua side (script start,
// FreeStoreService resume, On_Unit_Service / On_Unit_Added calls, AIBuildTask hand-off) rides on
// the A2 plan / Lua work and is stage 2.
//
// Admission (Land 0x690a10 -> base 0x4d2c90) [H unless tagged]: see FreeStoreObjectInfo; all of
//   owned by this AI player; not TRANSPORT; type not Create_Team, not Is_Dummy, not
//   Stay_In_Transport_During_Ground_Battle; no INDIGENOUS_UNIT/EXPLOSION behaviour;
//   LOBBING_SUPERWEAPON, or (locomotor and (STARSHIP_COMBATANT or TEAM) and not type bit 0x5f [M]);
//   a TEAM with a container/member type bit 0x20 refused; type bit 0x5b refused; obj +0x2b0/+0xf8
//   empty; not in the mode's +0x38 manager set [M]; base: not dead/removing, in world [M].
// Space (0x6904b0): PROVISIONAL [L] - its own rule list is not in the spec; this port applies the
// land list without the land-only Stay_In_Transport_During_Ground_Battle test.
//
// [port] ORDER: the retail map is an MSVC hash container keyed by object id (list order depends on
// insertion history and rehashes). Here the store is ordered by ASCENDING OBJECT ID (std::map), so
// On_Unit_Service visits free units lowest id first. Deviation from vanilla only in which unit acts
// first within one service pass (AI_TACTICAL 5.3 recommendation).
//
// Service (0x4d5a70, called on each Execution service = every 3 frames) [H]:
//   now = (float)F * (1/30) (mode frame F);
//   FreeStoreService runs when ServiceRate exists and now - LastService > ServiceRate (strict),
//   then LastService = now; afterwards On_Unit_Service(unit) for every entry whose order lock is
//   clear, in store order, when UnitServiceRate exists and now - LastUnitService > UnitServiceRate,
//   then LastUnitService = now. Both stamps start at 0. [M]: the stamps are Lua numbers (double);
//   the comparison is done in double with now widened from float.
#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace alamo {
class StateHasher;
class ByteWriter;
class ByteReader;
}

namespace alamo::ai::tactical {

struct FreeStoreObjectInfo {
    bool owned_by_player = false;               // owner == this AI player
    bool is_transport = false;                  // behaviour 5 TRANSPORT
    bool type_create_team = false;              // type +0x67
    bool type_is_dummy = false;                 // type +0x10
    bool type_stay_in_transport = false;        // Stay_In_Transport_During_Ground_Battle (+0x2d), land only
    bool has_indigenous_or_explosion = false;   // behaviour 0x20
    bool has_lobbing_superweapon = false;       // behaviour 0x37
    bool has_locomotor = false;                 // behaviour 0x01
    bool is_starship_combatant = false;         // STARSHIP_COMBATANT behaviour
    bool is_team = false;                       // TEAM behaviour
    bool type_category_bit_5f = false;          // type bitfield +0x790 bit 0x5f [M]
    bool team_has_bit_20 = false;               // TEAM: container or any member type has bit 0x20 [M]
    bool type_bit_5b = false;                   // type bit 0x5b [M]
    bool subobject_f8_nonempty = false;         // obj +0x2b0 -> +0xf8 not empty [M]
    bool in_mode_manager_set = false;           // in the mode's +0x38 set (0x4944e0) [M]
    bool dead_or_removing = false;              // +0x3a0 & 0x42, or bit 0x80
    bool in_world = true;                       // obj +0x3c8 set [M]
};

bool land_free_store_admits(const FreeStoreObjectInfo& o);
bool space_free_store_admits(const FreeStoreObjectInfo& o);

class TacticalFreeStore {
public:
    // Inserts an admitted object (call after the admission filter). False if already present.
    bool admit(int32_t object_id, uint32_t type_id);
    // Destroyed / owner changed / taken by a task force. False if not present.
    bool remove(int32_t object_id);
    bool contains(int32_t object_id) const { return entries_.count(object_id) != 0; }
    size_t size() const { return entries_.size(); }
    int32_t type_count(uint32_t type_id) const;
    // Lock_Current_Orders (0x57cd40): skip the unit in On_Unit_Service until cleared. The writer that
    // clears it (order finished) is open item 5. False if the object is not in the store.
    bool set_order_lock(int32_t object_id, bool locked);
    bool order_locked(int32_t object_id) const;
    // Free units in service order (ascending id), all or only the unlocked ones.
    void units(std::vector<int32_t>& out, bool unlocked_only) const;

    void hash(StateHasher& h) const;  // entries ascending (id, type, lock), type counters ascending
    // N1 save / load: u32 version (1), u32 count + entries ascending (i32 id, u32 type, bool lock); the type counters are
    // rebuilt from the entries (they always count them). False and unchanged on bad data; reads its own fields only.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Entry {
        uint32_t type_id = 0;
        bool locked = false;
    };
    std::map<int32_t, Entry> entries_;
    std::map<uint32_t, int32_t> per_type_;
};

// Script-declared rates and the Lua-side stamps.
struct FreeStoreSchedule {
    std::optional<double> service_rate;       // global ServiceRate (absent -> FreeStoreService never runs)
    std::optional<double> unit_service_rate;  // global UnitServiceRate (absent -> no per-unit service)
    double last_service = 0.0;                // LastService
    double last_unit_service = 0.0;           // LastUnitService
    void hash(StateHasher& h) const;
    // N1 save / load: u32 version (1), both optional rates (bool + f64), both stamps (f64).
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);
};
// BusyTacticalFreeStore (land and space): ServiceRate 20, UnitServiceRate 2 [H].
FreeStoreSchedule busy_tactical_schedule();

struct FreeStoreServiceStep {
    float now = 0.0f;
    bool run_service = false;          // resume the script threads (FreeStoreService) first
    bool run_unit_service = false;
    std::vector<int32_t> units;        // On_Unit_Service order (unlocked, ascending id)
};
// Mode time in seconds: (float)F * (1/30).
float free_store_mode_time(uint32_t mode_frame);
// One free-store service at mode frame F (the caller invokes it on Execution services). Updates the
// stamps. The unit list is a snapshot: stage 2 must re-check membership / lock before each call
// because a callback can release or lock units.
void free_store_service(FreeStoreSchedule& sched, const TacticalFreeStore& store, uint32_t mode_frame,
                        FreeStoreServiceStep& out);

}  // namespace alamo::ai::tactical

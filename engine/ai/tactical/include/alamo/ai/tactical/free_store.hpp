// A1 - tactical free store: admission filter, membership and service scheduling
// (docs/re/AI_TACTICAL.md 5.1-5.4; framework AI_GALACTIC.md 1.10). The Lua side (script start,
// FreeStoreService resume, On_Unit_Service / On_Unit_Added calls, AIBuildTask hand-off) rides on
// the A2 plan / Lua work and is stage 2.
//
// Admission (A1_READINGS 6 [H]; the 0x20 / 0x5B / 0x5C / 0x5F / 0x60 tests are behaviour-in-type tests 0x370320).
// Land (0x690a10), all of:
//   type not Create_Team; no TRANSPORT;
//   LOBBING_SUPERWEAPON (0x37), or all of: type lacks DUMMY_GROUND_STRUCTURE (0x5F), STARSHIP_COMBATANT or TEAM,
//   a locomotor, type not Is_Dummy (so a dummy with LOBBING_SUPERWEAPON is admitted);
//   no INDIGENOUS_UNIT (0x20): a TEAM's representative type and every member type must lack it too;
//   type lacks DUMMY_GROUND_BASE (0x5B); owned by this AI player; not Stay_In_Transport_During_Ground_Battle;
//   obj +0x2b0 and +0xf8 empty; not in the land mode's +0x38 set (the bombing-run manager, 0x4944e0);
//   base test 0x4d2c90 (not dead / removing, in world) [M: AT 5.1 places it on the land path too].
// Space (0x6904b0), a separate list (none of the TRANSPORT / Create_Team / Stay_In_Transport / INDIGENOUS_UNIT /
// DUMMY_GROUND_BASE / bombing-manager tests), all of:
//   type neither a squadron member (+0x29) nor a company member (+0x2a);
//   SPECIAL_WEAPON (0x2F, obj +0x361), or all of: type not Is_Dummy, STARSHIP_COMBATANT or TEAM, a locomotor;
//   type lacks DUMMY_ORBITAL_STRUCTURE (0x60) and DUMMY_STAR_BASE (0x5C); no TACTICAL_SUPER_WEAPON (0x2C, obj +0x35e);
//   owned by this AI player; obj +0x2b0 and +0xf8 empty; base test 0x4d2c90.
//
// [port] ORDER: the retail map is an MSVC hash container keyed by object id (list order depends on
// insertion history and rehashes). Here the store is ordered by ASCENDING OBJECT ID (std::map), so
// On_Unit_Service visits free units lowest id first. Deviation from vanilla only in which unit acts
// first within one service pass (AI_TACTICAL 5.3 recommendation).
//
// Entry +0x30 (A1_READINGS §28.1, §28.4) [H]: the owning PotentialPlan (here the PP handle, -1 = unreserved). Admission
// builds every entry unreserved; removal erases the entry, so a unit that leaves and comes back is unreserved. The
// writers are the goal system's (tactical_ai.cpp): reserve / re-assert 0x6c2b20 (overwrites any owner), unreserve
// 0x6c2320 / 0x6c27d0 (only the owner's). `Lock_Current_Orders` does not touch it (§28.2): the store keeps no order lock.
//
// Service (0x4d5a70, called on each Execution service = every 3 frames) [H] (A1_READINGS 9 [H static]):
//   now = (float)F * 0.033333335f (mode frame F, the float at 0x140b0a344);
//   FreeStoreService runs when ServiceRate exists and now - LastService > ServiceRate (strict),
//   then LastService = now; afterwards On_Unit_Service(unit) for every entry with +0x30 == 0 (no lock test,
//   A1_READINGS §28.5), in store order, when UnitServiceRate exists and now - LastUnitService > UnitServiceRate,
//   then LastUnitService = now. Both stamps start at 0. The stamps are Lua numbers (double); the comparison
//   is done in double with now widened from float [H].
//   Result with BusyTacticalFreeStore (20 / 2): FreeStoreService is first eligible at F = 600 (now =
//   20.0000019), then F = 1200, ...; On_Unit_Service at F = 61 (now(60) is exactly 2.0, which does not pass).
//   Each fires on the next Execution service at or after that frame [H]. With Execution services on
//   multiples of 3 that is F = 600 and F = 63 [M: the player's cadence phase was not traced].
//   Note: A1_READINGS 9.1 words the test "rate <= now - LastService", but its own F = 60 result (now = 2.0
//   exactly, not passed) needs the strict form kept here.
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
    bool has_indigenous_or_explosion = false;   // INDIGENOUS_UNIT 0x20 (land only)
    bool has_lobbing_superweapon = false;       // behaviour 0x37
    bool has_locomotor = false;                 // behaviour 0x01
    bool is_starship_combatant = false;         // STARSHIP_COMBATANT behaviour
    bool is_team = false;                       // TEAM behaviour
    bool type_category_bit_5f = false;          // type has DUMMY_GROUND_STRUCTURE 0x5F (land only)
    bool team_has_bit_20 = false;               // TEAM: representative or any member type has INDIGENOUS_UNIT 0x20 (land)
    bool type_bit_5b = false;                   // type has DUMMY_GROUND_BASE 0x5B (land only)
    bool subobject_f8_nonempty = false;         // obj +0x2b0 or +0xf8 not empty: contained or attached
    bool in_mode_manager_set = false;           // in the land mode's +0x38 set (bombing-run manager, 0x4944e0)
    bool dead_or_removing = false;              // +0x3a0 & 0x42, or bit 0x80
    bool in_world = true;                       // obj +0x3c8 set [M]
    // Space-only facts (A1_READINGS 6.1), appended after the 17 facts above. The defaults keep the old result
    // for hosts that do not fill them.
    bool type_squadron_member = false;          // type +0x29
    bool type_company_member = false;           // type +0x2a
    bool has_special_weapon = false;            // SPECIAL_WEAPON 0x2F (obj +0x361)
    bool type_dummy_orbital_structure = false;  // type has DUMMY_ORBITAL_STRUCTURE 0x60
    bool type_dummy_star_base = false;          // type has DUMMY_STAR_BASE 0x5C
    bool has_tactical_super_weapon = false;     // TACTICAL_SUPER_WEAPON 0x2C (obj +0x35e)
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
    // Entry +0x30: the owning PotentialPlan handle, -1 = unreserved or not in the store.
    int32_t reservation(int32_t object_id) const;
    // 0x6c2b20: +0x30 = pp (overwrites another owner, no check). False if the object is not in the store.
    bool reserve(int32_t object_id, int32_t pp);
    // 0x6c2320 / 0x6c27d0: +0x30 = 0 when it is pp. False if not in the store or owned by another PP.
    bool unreserve(int32_t object_id, int32_t pp);
    // Free units in service order (ascending id), all or only the unreserved ones (+0x30 == 0).
    void units(std::vector<int32_t>& out, bool unreserved_only) const;

    void hash(StateHasher& h) const;  // entries ascending (id, type, +0x30), type counters ascending
    // N1 save / load: u32 version (2), u32 count + entries ascending (i32 id, u32 type, i32 +0x30); the type counters are
    // rebuilt from the entries (they always count them). False and unchanged on bad data; reads its own fields only.
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    struct Entry {
        uint32_t type_id = 0;
        int32_t owner_pp = -1;  // +0x30
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
    std::vector<int32_t> units;        // On_Unit_Service order (unreserved, ascending id)
};
// Mode time in seconds: (float)F * (1/30).
float free_store_mode_time(uint32_t mode_frame);
// One free-store service at mode frame F (the caller invokes it on Execution services). Updates the
// stamps. The unit list is a snapshot: stage 2 must re-check membership / +0x30 before each call
// because a callback can release or reserve units.
void free_store_service(FreeStoreSchedule& sched, const TacticalFreeStore& store, uint32_t mode_frame,
                        FreeStoreServiceStep& out);

}  // namespace alamo::ai::tactical

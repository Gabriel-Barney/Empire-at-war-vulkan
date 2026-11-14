// Fog follow-up (L1, docs/re/FOG_OF_WAR.md §6, LUA_API.md LuaFOWRevealCommandClass / LuaFOWCellsClass): the
// FogOfWar global and the LuaFOWCellsClass handles (src/bind_fog.cpp).
//
// Bound by the family (vm_internal.hpp impl_fog):
//   FogOfWar(...)                                         the global called directly: [port] no documented form, nil
//   FogOfWar.Reveal(player, object)                       §6.2: exactly 2 args and a game object -> RevealObject
//                                                         (force 0: the object's own sight when allied to the
//                                                         player, its footprint otherwise); a LuaFOWCellsClass
//   FogOfWar.Reveal(player, target, radius [, dense])     §6.2: target = object / position / AI target with an object;
//                                                         dense defaults to radius; battle scale applies
//   FogOfWar.Reveal_All(player)                           §6.5: Reveal_All on the player's grid, returns NOTHING
//   FogOfWar.Temporary_Reveal(player, object [, radius])  §6.5: flash (radius 0 / missing -> footprint radius)
//   FogOfWar.Disable_Rendering([bool])                    §6.5: FOW-active = render option = not arg (no arg: fog
//                                                         OFF); the only one that works outside a tactical mode
//   cells.Undo_Reveal()                                   §6.3: decrement the snapshot's cells, empty it (a second
//                                                         call does nothing); garbage collection never undoes
// Error discipline (as script_gaps.hpp): the original's script error helper logs and does not unwind; a binding logs
// "SCRIPT ERROR: LuaFOWRevealCommandClass -- ..." in LuaVm::messages() and returns nothing. Outside a tactical mode
// (no hooks / no fog): "Command only valid in a tactical game".
//
// Host contract: ObjectBridge::tactical (script_gaps.hpp) -> TacticalScriptHooks::fog -> FogScriptHooks below, one
// per tactical mode (CampaignBattles sets it to the battle's TacticalBattle::fog() / fog_object()). Without the fog
// library (ALAMO_LUA_HAVE_FOG unset) the bindings answer as outside a tactical mode.
//
// Snapshots: a LuaFOWCellsClass userdata holds a handle (Ud::id) into FogScriptCells of the hooks plus the hooks'
// epoch (Ud::value): cells of another fog instance (an earlier battle) are ignored. Saving = FogScriptCells::entries()
// (player id + cell list, LuaFOWCellsClass 0x6a5bf0); the original's load adds the entries back and recounts once the
// grids exist (0x6a5120). This engine saves the fog grids themselves (FogOfWar::write / read, N1 stage 2), so a load
// reads the entries back and does NOT recount (that would count them twice). Determinism: entries in ascending handle id (creation order), no pointer-keyed container.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/objects/object_store.hpp"
#ifdef ALAMO_LUA_HAVE_FOG
#include "alamo/tactical/fog/fog_of_war.hpp"
#endif

namespace alamo {
class StateHasher;
}

namespace alamo::lua {

#ifdef ALAMO_LUA_HAVE_FOG
// The LuaFOWCellsClass snapshots of one fog instance (handle ids from 1, never reused).
class FogScriptCells {
public:
    struct Entry {
        int32_t id = 0;
        tactical::fog::FogCells cells;
    };
    int32_t add(tactical::fog::FogCells cells);
    tactical::fog::FogCells* find(int32_t id);
    void erase(int32_t id);  // Undo_Reveal empties the snapshot: the entry goes
    // Load (0x6a5120): every held snapshot counted once more on its player's grid, ascending id.
    void recount(tactical::fog::FogOfWar& fog) const;
    const std::vector<Entry>& entries() const { return list_; }
    void clear() {
        list_.clear();
        next_ = 1;
    }
    void hash(StateHasher& h) const;  // opt-in (fog is not in the frame CRC)
    // N1 save / load (contract: alamo/core/save_file.hpp): u32 version, i32 next id, u32 count, per snapshot
    // (ascending id): i32 id, i32 player, u32 cell count, u32 cells. read() reads its own fields only (the owner
    // checks done() of its chunk); false and unchanged on bad data. The fog grids are saved with FogOfWar (N1 stage 2)
    // and already hold these snapshots' counts: after a load the host must NOT call recount().
    void write(ByteWriter& w) const;
    bool read(ByteReader& r);

private:
    std::vector<Entry> list_;  // ascending id
    int32_t next_ = 1;
};

struct FogScriptHooks {
    tactical::fog::FogOfWar* fog = nullptr;  // the mode's fog; null = fog off (tactical commands then refuse)
    // The fog's view of a live object of the VM's store (TacticalBattle::fog_object). Unset / false: no position.
    std::function<bool(objects::ObjectHandle, tactical::fog::FogObject&)> object;
    // refixf t03: FOW_Object_Hidden 0x35f470 (FOG_OF_WAR.md §5) of a live object of the VM's store for player p
    // (TacticalBattle::fog_object_hidden), used by Find_Nearest with a player (force = true). Unset: `fog`'s
    // object_hidden with a query built from the store (TEAM / HIDE_WHEN_FOGGED / REVEAL behaviours, position; no
    // Multisample_FOW_Check corners [port]).
    std::function<bool(objects::ObjectHandle, int32_t player, bool force)> object_hidden;
    // Disable_Rendering while `fog` is null: the host's carried FOW-active / render option (CampaignBattles keeps them
    // between battles, FOG_OF_WAR §8 item 2). Unset: ignored.
    std::function<void(bool fow_active, bool render_option)> set_globals;
    // sp3 (AI lane): set by a host whose VMs run off the critical lane. Reveal_All (op 1, player), Temporary_Reveal (op 2,
    // player, object, seconds) and Disable_Rendering (op 3, value 1 = disable) are handed to it - the host applies them at
    // the due tick - instead of acting on `fog`; Reveal and Undo_Reveal (a cells handle used at once) are a script error
    // there. The fog reads stay `object_hidden` (the host answers from its capture).
    std::function<void(uint8_t op, int32_t player, objects::ObjectHandle object, float value)> lane_write;
    uint32_t epoch = 0;  // host-chosen id of this fog instance (e.g. the battle number)
    FogScriptCells cells;
};
#else
struct FogScriptHooks;  // only with the fog library
#endif

}  // namespace alamo::lua

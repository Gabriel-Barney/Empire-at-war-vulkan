// q6 t19: the saved state of one script owner's LuaScriptClass - the shared "script block" (docs/re/LUA_SAVE.md §1, §2;
// port notes there: "one shared script block save routine serves all owners; each owner adds only its name chunk and its
// own engine-side fields"). Used by the story plot scripts (story.lua chunk), the GameScoring script (scoring chunk) and,
// once the object-script host saves, by every game-object script (OBJECT_SCRIPTS §2.6).
//
// WHAT A BLOCK HOLDS (the port's own format, N1; it is not retail's byte layout):
//   * the script name and the VM options that change what the state means (math library, upper-cased type names): the
//     original creates the load's instance BY NAME (§4.1), so the block names what the reader must create;
//   * the thread records in id order (start function name, started / killed) and the next thread id (§2.1 / §2.2
//     micro 3 / 5): a finished thread leaves a gap in the ids, so ids never shift. The reader checks that the restored
//     VM holds exactly these records (no second `main` from a normal attach, §4.1);
//   * the LuaVm::write image (vm_persist.cpp): the whole Lua heap. It covers everything LUA_SAVE §3 lists as saved -
//     prototypes / bytecode of the save (a load runs the saved bytecode, not the file on disk), every reachable table
//     with its metatable and its exact node layout, persistable engine userdata (object / player / type / position /
//     blocking-status wrappers, by class and engine handle), each coroutine's stack and call frames with saved pcs, the
//     GetEvent queue (a registry table), LastService and every other global - and also what retail loses, which the
//     port keeps on purpose (LUA_SAVE §3.4 port notes: "keeping upvalues and identity is the recommended choice"):
//     upvalue values, closure environments, table identity across threads, coroutine.create threads, the global
//     command objects (GameRandom, GlobalValue, FogOfWar, ...) also when a local or a table field holds them. Upvalues
//     are kept: TR's deepcore closures need them, and the N1 lockstep tests (test_lua_save.cpp) rely on them.
//   * an FNV-1a 64 checksum of all of the above.
// The script's GlobalValue entries, players, objects and bridges are NOT in the block: their owners save them.
//
// LOADING (§4.1 / §4.2): the owner creates the instance by the block's name (peek_script_block) WITHOUT running anything
// - no require of the file, no Base_Definitions, no `main`, no `Object` (the from-save attach of OBJECT_SCRIPTS §1.3):
// LuaVm constructor (base globals and commands) + the owner's bridges and host attachments, then read_script_block.
// The image REPLACES the instance's whole Lua state, so a load runs nothing either: `main` and every other thread
// continue at their saved pc, OnEnter-style code is not re-run (TR's company spawner does not spawn again). Owners that
// rebuild their scripts the normal way before a load (the story session, the GameScoring host) may read into those VMs
// with ScriptLoad::Any: the rebuilt state is discarded whole; only host attachments are kept (LuaVm::read).
#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "alamo/core/bytes.hpp"
#include "alamo/lua/lua_vm.hpp"

namespace alamo::lua {

inline constexpr uint32_t kScriptBlockVersion = 1;

struct ScriptBlockHeader {
    uint32_t version = 0;
    std::string name;  // LuaVmOptions::script_name of the saved VM
    bool math_library = false;
    bool upper_type_names = false;
    std::vector<LuaVm::ThreadInfo> threads;  // id order
    int32_t next_thread = 0;
    bool saved = false;      // false: the VM could not be saved (save_error says why); read_script_block refuses it
    std::string save_error;  // LuaVm::save_error() at write time
};

// Writes the block of `vm` (between services: no script running). A VM whose state cannot be saved (LuaVm::save_error)
// still gets a block; its header says so and read_script_block refuses it.
void write_script_block(ByteWriter& w, const LuaVm& vm);

// The header of a block (exactly the bytes write_script_block wrote), without a VM. False on damaged / unknown data.
bool peek_script_block(std::span<const uint8_t> block, ScriptBlockHeader& out, std::string* err = nullptr);

enum class ScriptLoad : uint8_t {
    FromSave,  // the target must be LuaVm::fresh() (the from-save instance: nothing run, no thread made)
    Any,       // a rebuilt instance (Base_Definitions / main already ran); its Lua state is replaced whole
};

// Restores a block into `vm`, which must have the block's name and options. Reads the whole of `r` (r.done() after
// success). False + *err when the data is damaged, the name / options / ScriptLoad rule do not match, or the image is
// refused; the VM is then unchanged (every check runs before LuaVm::read commits).
bool read_script_block(ByteReader& r, LuaVm& vm, ScriptLoad mode, std::string* err = nullptr);

}  // namespace alamo::lua

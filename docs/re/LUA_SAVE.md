# Lua script state in save games (q6 t19 question 9)

Question: what does retail FoC write into a save game for a live Lua script (object script, story plot script, AI plan
script, GameScoring script and the two other AI owners), which Lua values survive, what is rebuilt instead, and how is
the state restored on load? The port refused saves while object scripts were live (OBJECT_SCRIPTS §7.2); this
document is what it needed to stop refusing (done in q6 t19, §6).

Answer in one paragraph:
- Every script owner saves the same thing: the script **file name** and one **LuaScriptClass state block**
  (save 0x247e50, load 0x246310). The block holds the thread list (by global function name), the script's `GetEvent`
  object, and one **Lua state image per live thread** (or one image of the main state when no thread is live).
- The image is written by a persistence layer built into Petroglyph's modified Lua 5.0 (persist 0x7c27e0, unpersist
  0x7c5020). It is a whole-graph image: the function prototypes (bytecode) of the state, every table reachable from
  the globals and the thread stack, the persistable userdata, and the thread's stack and call frames (with program
  counters), so a coroutine resumes exactly where it yielded.
- **Not saved:** Lua upvalue values, closure environments, C functions (they are matched by registration index),
  and every engine userdata whose class has no persistence id (the global command objects such as `GlobalValue`,
  `GameRandom`, `FindPlanet`, `FogOfWar`). Those come back from the fresh instance that the load creates first.
- **Load:** a fresh, unpooled instance is created by name (no `Base_Definitions`, no `main`, no `Object`), the base
  globals and the 119 commands are registered, the threads are re-created by name in saved order, and each image is
  then unpersisted **into** that state: saved globals overwrite the fresh ones, keys whose saved value was not
  persistable keep the fresh registration.

Source: Ghidra decompile of Steam FoC x64 1.121 (`re_reference/ghidra/StarWarsG_decompiled.zip`, `Global.c`); RVAs
relative to the image base. Lua structure offsets are those of the 64-bit build (Lua 5.0 layout plus Petroglyph's
added fields, listed in §2.1). Script data: vanilla FoC `Data\Scripts` (`extracted/foc_data_text.zip`) and Thrawn's
Revenge (workshop 1125571106).

---

## 1. Owners

Six classes own a `LuaScriptClass` that is saved. All call the same block writer 0x247e50 inside their own chunk
and the same reader 0x246310 on load. [H for the six call sites]

| owner | save | load | chunks (inside the owner's chunk) | instance used on load |
|---|---|---|---|---|
| game object (`Lua_Script`, object +0x2D8) | object save 0x3a5ad0 | object load 0x39c3a0 | 0x4B6 = name, 0x4B5 = block. Written only when the object has a script and the two save flags of 0x3a5ad0 are clear; read only when 0x140b2c37b is 0 | Attach_Object_Script 0x38cf30, from-save path (§4.1) [M] |
| GameScoring (`TheGameScoringManagerClass`, +0x10) | 0x487870 | 0x486690 | 0 = name, 1 = block (GMM chunk 3, GAME_SCORING §6) | Attach_Lua_Script 0x4854e0, from-save path (§4.1) [M] |
| story plot (plot +0x610) | 0x52fe20 | 0x52f0a0 | 0x1D = name, 0x1E = block (STRATEGIC_GAPS §4 item 8) | plain new instance 0x2440f0 + base globals 0x2567b0 + commands 0x546c70 [H] |
| AI plan (`AIPlanningSystemClass` plan record, script +0x48; written per plan by 0x725ac0, chunk 3 each) | 0x5f7f80 | 0x5f7660 | 0 = plan header (micro 4 = plan definition name, micro 5 = …), then, when a script exists: 2 / 3 / 0xD = three engine-held LuaVars (+0x50, +0x58, +0xA0), 0xC = one per entry of the plan's LuaVar list (+0x68), 8 = the +0x90 sub-object (0x6892e0), 1 = block | 0x2440f0 + base globals + commands [H] |
| AI free store (`AIFreeStoreClass` vf25 / vf26, script +0x80; `GalacticFreeStoreClass` 0x6900d0 calls it) | 0x4d5250 | 0x4d3d10 | 0xD = name, 0xE = block | the free store's own attach 0x4d31e0 [L for the path] |
| AI evaluator scripts (`TheEvaluatorScriptManagerClass`, global 0x140b310d0, one entry per script name) | 0x5e8160 | 0x5e79d0 | per entry: 0 = name, 1 = block | the manager's get-or-create by name 0x5e7cb0, then the block is read into it [M] |

The engine-held LuaVars of the AI plan (chunks 2 / 3 / 0xD / 0xC) are written with the LuaVar writer 0x24b650 (§3.3),
not inside a Lua image. [H for the writer, L for what each LuaVar is]

**Port notes.** The port has object scripts, story plot scripts and GameScoring today; the AI plan / free store /
evaluator owners matter only when the port's AI runs Lua plans. One shared "script block" save routine serves all
owners; each owner adds only its name chunk and its own engine-side fields.

---

## 2. The LuaScriptClass block (0x247e50 / 0x246310)

### 2.1 Fields involved [H]

| offset | what |
|---|---|
| +0x28 | an embedded sub-object whose address is registered for pointer fix-up (micro 8) |
| +0x40 / +0x48 | the thread record vector, 0x40 bytes per record |
| +0x58 | the main `lua_State` |
| +0x68 | the script name (std::string) |
| +0xD8 | the script's `GetEvent` object (class id 0x3D, created with the commands at 0x2567b0) |
| +0x125 | `ScriptShouldCRC` flag |

Thread record (0x40 bytes): +0x00 the coroutine `lua_State` (null when the thread has finished), +0x08 the engine
`LuaFunction` wrapper of its start function, +0x10 the start function's global name, +0x30 an int (the stack offset
of the start function in the new coroutine, 0x7b8b50; written by 0x244740), +0x38 an optional start argument. A
finished thread's record is reset in place by the thread service 0x247a90 (thread unpinned from `LuaThreadTable`
0x248d10, then thread, function and name cleared), so record indices never shift. [H]

### 2.2 Save order [H]

1. Run a full garbage collection on the main state and on every live thread (`lua_setgcthreshold(L, 0)`, 0x7b9990),
   so the images hold only reachable data.
2. Chunk 0, micro-chunks:
   - 3 = record count (4 bytes);
   - 1 = the block's own address (8 bytes; also registered for pointer fix-up, 0x1fec80);
   - 8 = the address of +0x28 (8 bytes, fix-up);
   - per record, in order: 5 = the start function name (empty for a finished thread), 7 = the +0x30 int.
3. Chunk 2: the `GetEvent` object +0xD8, through the LuaVar writer 0x24b650 (§3.3). This is the script's event
   queue object; the earlier OBJECT_SCRIPTS text called it "pending-resume values".
4. For every record with a live thread, in record order: chunk 4 = the Lua image of that thread (§3), with "last"
   set for the last live record.
5. Only when **no** record has a live thread: chunk 6 = the Lua image of the main state (with "last" set).

So the main state's own stack is not saved when threads exist. Between services it holds nothing the script needs.
The globals are carried by every thread image (§3.4). [H for the branch; M for "nothing needed"]

### 2.3 Load order [H]

The reader walks the chunks in file order:
- chunk 0: micro 1 and 8 register "old address → this block / this +0x28" for the pointer fix-up pass
  (0x1fe2d0); micro 3 is read and not used; micro 5 appends a record: empty name → an empty record, else
  0x244740(name), which looks up the global of that name in the fresh state, makes a new coroutine for it, pins it in
  the registry table `LuaThreadTable` and appends the record; micro 7 overwrites the last record's +0x30.
- chunk 2: the `GetEvent` object, through the LuaVar reader 0x24ab30, into +0xD8.
- chunk 4: unpersist (0x7b9e60) into the next record that has a thread, with "last" when no later record has one.
- chunk 6: unpersist into the main state.

After the chunks:
1. every record that has a thread but no `LuaFunction` wrapper gets one: the global of its name, read from the
   restored globals;
2. the global `ScriptShouldCRC` is read from the restored globals into +0x125 (when it is a boolean);
3. the script-manager listener 0x140b09e88 is told that the script was loaded (vf +0x30).

Because the threads are re-created **before** the images are read, thread ids inside the images (§3.2, type 8) line
up with the new coroutines only if the records are created in the same order as at save time. They are: the
records are rebuilt in saved order. [M]

**Port notes.**
- Keep finished threads as empty records so thread ids stay stable; save the record list in order.
- `LastService` is an ordinary Lua global (OBJECT_SCRIPTS §3), so it comes back with the globals; no new RNG draw
  happens on the first service after a load as long as the port restores the globals before the first service.
- The `GetEvent` queue is per script and must be saved with it.

---

## 3. The Lua state image (persist 0x7c27e0, unpersist 0x7c5020)

### 3.1 Additions Petroglyph made to Lua 5.0 [H]

| where | added field | used for |
|---|---|---|
| `Proto` +0x64 | proto id (1-based index in the global proto table) | a Lua function is saved as "proto id"; `FUN_1407ba430` creates a proto with a given id (a normal load picks the first free id) |
| `global_State` +0x38 / +0x40 / +0x44 | proto table (all protos, nested ones included) | id → proto (0x7ba200) |
| `global_State` +0x48 / +0x50 | C closure table | every C closure gets an id (`CClosure` +0x20); id → closure (0x7ba1e0) |
| `global_State` +0x58 / +0x60 | thread table | id → thread (0x7b8710) |
| `global_State` +0x68 | "protos already written" flag | the protos are dumped once per save (§3.4) |
| `lua_State` +0x28 / +0x38 / +0x48 (+count, +capacity) | per-image work lists: metatables, tables, userdata | object ids during one persist / unpersist; cleared at the start of each (0x7c3030) |

### 3.2 Value encoding [H]

Every Lua value is a type byte (Lua 5.0 tags) followed by its payload:

| tag | type | payload |
|---|---|---|
| 0 | nil | none |
| 1 | boolean | 1 byte |
| 2 | light userdata | the raw 8-byte pointer (meaningless after a load; no shipped script stores one) |
| 3 | number | 8-byte double |
| 4 | string | 8-byte length + 1, then the bytes including the terminating zero; length 0 = null string |
| 5 | table | 4-byte id in the image's table list (1-based); **id 1 is always the state's globals table** |
| 6 | function | 1 byte "is C". Lua function: 4-byte proto id + 4-byte upvalue count, **no upvalue values**. C function: 4-byte C closure id + 4-byte upvalue count + each upvalue as a value |
| 7 | userdata | 4-byte id in the image's userdata list; **0 = not persisted** (reads back as nil) |
| 8 | thread | 4-byte id in the global thread table |

Functions are written inline wherever they appear, so two references to one Lua closure come back as two closures.
On load a Lua closure is rebuilt from its proto with the state's globals as environment (0x7ba3c0); its upvalue
slots are not filled. A C closure is looked up by id in the fresh state (the same object, upvalues overwritten).
[H for the encoding; M for the uninitialised upvalue slots: 0x7ba3c0 sets only the count]

### 3.3 Userdata and engine wrappers [H]

The persist call passes a userdata hook (0x24bce0) and a writer (0x246f80, which appends raw bytes to the current
save chunk). For each userdata met while marking, the hook decides whether it is persisted: only when its payload is
a `LuaUserVar` whose **class id** (+0x20, set by the `LuaUserVar` constructor 0x249c20) is non-zero. Others are left
out and every reference to them is written as id 0.

When the image reaches the userdata section, the hook closes the current chunk, writes chunk 0x37 = the wrapper
(0x24b470), and opens chunk 0x3F in which the image continues. So an image chunk 4 / 6 is followed by pairs of
(0x37 wrapper, 0x3F continuation) chunks, one per persisted userdata.

Wrapper record (0x24b470 / 0x2491c0), chunk-coded:
- 0 = state: 3 = null or not persistable, 2 = first time this wrapper is written in the save, 1 = already written
  (only a reference);
- 4 = the wrapper's owner pointer +0x18 (fix-up id), when set;
- 3 = the wrapper's own address (fix-up id);
- first time only: 1 = class id, 5 = (empty; on load: create the wrapper from the class factory 0x24a320), 2 = the
  class's own Save (vf +0x70) / Load (vf +0x78).

So one engine wrapper referenced from several scripts is one object again after the load (fix-up table).

Persistable classes (factory registrations, class id → creator; [H]):

| id | class | id | class |
|---|---|---|---|
| 61 (0x3D) | GetEvent | 2069 | GalacticTaskForceClass |
| 64 | BlockingStatus | 2070 | SpaceTaskForceClass |
| 65 | ThreadValue | 2071 | SpaceMovementBlockStatus |
| 66 | LuaScriptWrapper | 2072 | LandTaskForceClass |
| 67 | LuaExternalFunction | 2073 | LandMovementBlockStatus |
| 2049 | TaskForceClass | 2074 | ReinforceBlockStatus |
| 2050 | GameObjectWrapper | 2075 | MarkupBlockStatus |
| 2051 | PlayerWrapper | 2076 | BombingBlockStatus |
| 2052 | GameObjectTypeWrapper | 2077 | SpaceAmbushBlockStatus |
| 2055 | FormationBlockStatus | 2078 | TacticalBuildBlockStatus |
| 2056 | MovementBlockStatus | 2079 | SpaceReinforceBlockStatus |
| 2058 | InvasionBlockStatus | 2080 | PositionWrapper |
| 2059 | ProduceForceBlockStatus | 2081 | StoryPlotWrapper |
| 2060 | LandUnitsBlockStatus | 2082 | StoryEventWrapper |
| 2061 | ForeverBlockStatus | 2083 | LuaDiscreteDistributionClass |
| 2064 | AITargetLocationWrapper | 2084 | UnitMovementBlockStatus |
| 2065 | GiveDesireBlockStatus | 2085 | AbilityBlockStatus |
| 2066 | BaseBlockStatus | 2086 | UnitAnimationBlockStatus |
| 2067 | WeightedTypeListClass | 2087 | GenericSignalBlockStatus |
| 2068 | FreeStoreMovementBlockStatus | 2088 | LuaFOWCellsClass |
| | | 2089 | LightningEffectBlockStatus |
| | | 2090 | BinkMovieBlockStatus |
| | | 2091 | ExploreAreaBlockStatus |

Class id 0 (constructed with 0x249c20(…, 0, 1); not persisted): `GlobalValue`, `LuaGameRandom`, `FindPlanetClass`,
`LuaFOWRevealCommandClass`, `LuaSFXCommandsClassClass`, `LuaConsolePrint`, and (no factory entry, [M]) `FindTargetClass`,
`LuaGetTime`, `LuaCreateThread`, `BudgetWrapper`, `AIFreeStoreClass`, `GalacticFreeStoreClass`, `LuaScriptClass`,
the `LuaMemberFunctionWrapper<…>` method objects. These are the global command objects; they come back from the
fresh registration (§4.2).

Class saves read so far [H]:
- `GameObjectWrapper` (0x57f4c0): its +0x28 address and the object pointer +0x60 as fix-up ids, then its per-wrapper
  list (+0x40, 0x28-byte entries) [L for the list];
- `PlayerWrapper` (0x604970): the player pointer (fix-up id);
- `GameObjectTypeWrapper` (0x606630): the type's id (type +0x11C), 0 for none;
- `PositionWrapper` (0x60f100): the 12-byte position.

LuaVar writer 0x24b650 (engine-side values, used for `GetEvent` and the AI plan's LuaVars): chunk 6 = {state 3 / 2 /
1 as above, address}, then by LuaVar type: 1 = map (each key and value recursively), 2 = list (count + items), 3 / 7 =
no payload, 4 and 10 = 8 bytes, 5 = boolean byte, 6 = string (length + bytes), 8 = function reference (a function id:
C closure id with the top bit set, or the Lua proto id, 0x7ba690; plus the script address), 9 = a `LuaUserVar` (the
wrapper record above). [H for the cases; M for type names 4 / 10]

### 3.4 Image layout [H]

In stream order (all ints 4 bytes, little-endian):
1. **Header:** the Lua dump header with version byte 0x51 (not 0x50), little-endian, sizes int 4 / size_t 8 /
   Instruction 4 / op 6 / A 8 / B 9 / C 9 / number 8, and the test number 31415926.5359 (0x7c1380).
2. The C closure count of the state (`global_State` +0x50); the reader does not use it [M].
3. **Protos:** a count, then each root proto (a proto that is not nested in another) dumped like `luac`, with the
   proto id added: source, line defined, proto id, upvalue count, parameter count, vararg flag, max stack, line
   info, locals (name, start pc, end pc), upvalue names, constants (number or string), nested protos, code
   (0x7c0f70). The count is 0 when an earlier image of the same save already wrote them (flag `global_State` +0x68,
   cleared again by the "last" image). The reader recreates every proto **with its saved id**, replacing the proto
   that the fresh instance compiled into that slot (0x7ba430). So a loaded game runs the bytecode of the save, not of
   the script file on disk.
4. Three counts: metatables, tables, userdata.
5. Per userdata: its old address (8 bytes, identity key: the reader keeps one new userdata per old address across
   the images of one load, 0x140b3cd78) and its payload size.
6. Per metatable: key / value pairs, ended by a nil key, then the table's flags byte.
7. Per table: "has metatable" byte (+ metatable id), key / value pairs, nil key, flags byte. Table 1 is not created:
   the reader takes the state's **existing** globals table as table 1 (0x7c4820) and writes the pairs into it.
   Pairs whose value reads back as nil are skipped (0x7c4c90).
8. Per userdata: "has metatable" byte (+ metatable id), then the hook (wrapper chunks, §3.3).
9. **The thread:** stack size, top index, base index, then the stack slots from the bottom to top as values.
10. The globals table id (always 1), the CallInfo array size, the end index, the current index, then every CallInfo
    from the base to the current one: base index, top index, state flags, and for a Lua frame (flag bit 0 clear) the
    proto id, the pc as an instruction index into that proto's code, and the tail-call count.

Identity rules that follow from this [M]:
- Within one image, table identity is kept (ids). Across the images of one script it is not: each thread image
  creates its own copies of every table except the globals table, and the last image's copies end up in the
  globals. A thread whose local still points at a sub-table of the globals sees a separate copy after a load.
- A table that is both a metatable and a plain value is in both lists and comes back as two tables.
- Userdata identity is kept across images (old-address map), wrapper identity across scripts (fix-up table).
- Threads created with `coroutine.create` have no record and are not re-created; a reference to one is lost. Vanilla
  scripts never use `coroutine` or closures (grep of `Data\Scripts`); Thrawn's Revenge's deepcore library uses both
  closures with upvalues and class metatables heavily.

**Port notes.**
- The port's Lua is vendored 5.0.2 without these additions, and the port uses its own save format (N1), so it needs
  its own persistence with the same visible result, not this byte format.
- What must survive for parity: all reachable values (numbers, strings, booleans, tables with their
  metatables, Lua functions, persistable engine wrappers), every live coroutine's stack and call frames including
  the pc and the yielded C frame, and the record list.
- Where the original loses data (upvalue values, closure environments, cross-image table identity, light userdata,
  `coroutine.create` threads) the port may keep it: a script that works after the original's load works the same
  with a more faithful restore, and TR's closures would otherwise come back broken. Keeping upvalues and identity is
  the recommended choice [port decision, not retail behaviour].
- Bytecode: the original restores the saved bytecode. The port can either save its compiled protos or recompile the
  file and map protos by (source, line defined, nesting index). The second only differs when a script file changed
  between save and load; record the script file CRC in the save and refuse (or warn) on mismatch.
- Engine userdata: save a class id plus a stable engine handle (object id, player id, type id, position, the blocking
  command's id) for the persistable classes of §3.3; write "not persisted" for the command objects and let the fresh
  registration supply them (§4.2).

---

## 4. Restore path (`from_save`)

### 4.1 Which attach path a load uses

- Story plot and AI plan loads call the plain constructor 0x2440f0 (new instance by name, not pooled; it compiles and
  runs the file's top level), then the base globals 0x2567b0 and the commands 0x546c70, then the block reader. No
  `Base_Definitions`, no `main` thread. [H]
- Object and GameScoring loads call their attach functions (0x38cf30, 0x4854e0). Both have a `from_save` argument
  whose non-zero path does exactly the same as above (0x2440f0 + base globals + commands; the object path also puts
  the object in the serviced list). Ghidra drops the third argument at both load call sites. They must take the
  from-save path: the normal path creates a `main` record and the reader appends one record per saved name, which
  would give two `main` threads. [M]
- So on load nothing runs except the file's top level (which defines functions and initialises globals, all then
  overwritten by the image). `OnEnter`-style code inside `main` is not re-run; the restored `main` continues at its
  saved pc. [M]

### 4.2 What the fresh instance supplies

The image merges into the fresh state (table 1 = existing globals, nil values skipped). So:
- every global the save did not hold, or held as a non-persistable command object, keeps the fresh value: the 119
  command objects whose class id is 0 (`GlobalValue`, `GameRandom`, `FindPlanet`, `FogOfWar`, …; which of the 119
  commands are class 0 was checked only for those listed in §3.3), the Lua base library;
- C closures are matched by registration index, which is the same because the fresh instance registers the same
  libraries and commands in the same order; [M]
- `Object` (a `GameObjectWrapper`, class 2050) is persisted and points at the loaded object after the fix-up pass;
- a non-persistable command object stored in a **local** or a table field is nil after the load (for example
  `local rnd = GameRandom` inside a running thread). [H for the mechanism; no vanilla script does this]

**Port notes.**
- Load order per script: create the state by name (no pool, no `Base_Definitions`, no `main`, no `Object`), register
  the base globals and commands, rebuild the thread records by name in saved order, restore the globals and each
  coroutine, set `ScriptShouldCRC`, then let the normal service continue (no `OnEnter` re-run: TR's company spawner
  would otherwise spawn its company again).
- Resolve engine handles after all objects are loaded (the original uses its pointer fix-up pass); a handle to an
  object that no longer exists reads back as nil.
- Determinism: a save → load must reproduce the same next ticks. The restored globals include `LastService` and the
  scripts' own state; the RNG is the engine's, saved elsewhere. Add a CRC test that runs N ticks, saves, loads in a
  new process and compares the next M tick CRCs with an uninterrupted run, on a map with object scripts and a story
  plot script.

---

## 5. Open items

1. The meaning of 0x140b2c37b (it skips the object's 0x4B5 chunk on load; OBJECT_SCRIPTS §8 item 3). [L]
2. Whether the allocator 0x7bf7e0 zeroes memory: if it does, a restored Lua closure's upvalue slots are null and the
   first access crashes; if not, they are garbage. Either way retail cannot restore closures with upvalues. A probe
   (Gabriel): TR galactic save → load and watch the deepcore plugins. [M]
3. The +0x30 record int width (the decompile drops the size argument of micro 7; the reader stores 4 bytes) and the
   unsaved +0x38 start argument (only matters for a thread created but never resumed before the save). [L]
4. What the AI plan's LuaVars +0x50 / +0x58 / +0xA0 and the +0x90 sub-object are (AI owner only). [L]

---

## 6. Port status (q6 t19, 2026-11-04)

The port's own format (N1), matching the behaviour above, not retail's bytes:

| item | port |
|---|---|
| Image | `LuaVm::write` / `read` (`engine/lua/src/vm_persist.cpp`, unchanged, version 1): the whole heap. A superset of §3: **upvalues, closure environments, identity across threads, `coroutine.create` threads and command objects in locals are kept** (the §4.2 port notes' recommended choice; TR's deepcore closures and the `test_lua_save` lockstep tests need them). A light userdata outside the registry attachments refuses the save (retail writes garbage, §3.2) |
| Block (§2) | `engine/lua/include/alamo/lua/script_save.hpp` (`kScriptBlockVersion` 1): name, the VM options, thread records (id, start function, started / killed) and the next id, the image, an FNV-1a checksum. `read_script_block` checks name / options and, for `ScriptLoad::FromSave`, that the target ran nothing (`LuaVm::fresh`); the restored thread records must equal the header's. Test: `alamo_lua_script_save_tests` |
| Story plots | `story.lua` chunk version 4 (was 3: a bare `LuaVm` image per plot): a block per plot, read with `ScriptLoad::Any` into the rebuilt plot VMs (their state is replaced whole) |
| GameScoring | new `scoring` chunk (`SimHost::write_scoring`): the block, the VM's own player world, queued mode-start events, the tapped events, the failed flag and counters. Before q6 t19 nothing was saved (a load restarted the script fresh) |
| Object scripts | new `object_scripts` chunk (`ObjectScriptHost::write` / `read`, OBJECT_SCRIPTS §2.6): per live script its object, pool name and instance; every pool with its free instances; the type instances ([port] retail rebuilds those at type setup). Load = the §4.1 from-save path: new VM by name, GlobalValue and the holder store's bridges attached, then the block (no top level, `Base_Definitions`, `main` or `Object`). `save_allowed` no longer refuses live object scripts |
| AI plans | `galactic_ai_save.cpp` keeps its own `LuaVm` images (already a superset); not moved to the block |
| `ScriptShouldCRC` (§2.1) | not modelled: the port has no per-script CRC; the global survives in the image anyway |
| Determinism | `alamo_headless_save_synthetic` (story plot + GameScoring + object scripts, save at 450, in-process and fresh-process loads and the replays compared with the uninterrupted run); `alamo_sim_tests` `object_script_test::test_save_load` (two-pass battle load, 150 ticks after the load CRC-identical) |

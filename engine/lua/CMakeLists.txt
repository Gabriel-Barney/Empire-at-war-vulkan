# L1 - Lua bridge, stage 1: the original's VM (Lua 5.0.2, approved by Gabriel 2026-10-27) plus the
# engine bindings (docs/re/LUA_API.md, docs/re/lua_api.json).
#
# Vendored VM: the pristine lua.org tarball unpacked by toolchain\7_lua.cmd into toolchain\lua-5.0.2
# (SHA-256 pinned there). It is NOT copied into engine/: at configure time the sources are copied into
# the build dir, the determinism patches below are applied to the copies (each one verified), and the
# copies are compiled AS C++ with the same compiler as the engine:
#   * one language/ABI: no C compiler needed by the build scripts, no extern "C" seams;
#   * /fp:precise (MSVC) or -ffp-contract=off (clang) like every engine target (rule 3);
#   * third-party warnings are silenced for this target only (/W0, -w); the include dir is SYSTEM.
# Built: core VM + lauxlib + base (incl. coroutine), string and table libs - what the original's
# scripts can reach. NOT built: lmathlib (the original exposes no math table; an optional dmath-backed
# one lives in src/math_lib.cpp), liolib, ldblib, loadlib (no io/os/debug/dynamic loading for scripts).
# require/dofile/loadfile/print are replaced by VFS/log bindings (src/lua_vm.cpp).
set(ALAMO_LUA_DIR "${PROJECT_SOURCE_DIR}/toolchain/lua-5.0.2" CACHE PATH "Lua 5.0.2 source tree (toolchain\\7_lua.cmd)")
if(NOT EXISTS "${ALAMO_LUA_DIR}/src/lvm.c")
  message(WARNING "engine/lua skipped: Lua 5.0.2 not found in ${ALAMO_LUA_DIR} (run toolchain\\7_lua.cmd)")
  return()
endif()

set(_lua_core lapi lcode ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate lstring
              ltable ltm lundump lvm lzio)
set(_lua_libs lauxlib lbaselib lstrlib ltablib)
set(_lua_out "${CMAKE_CURRENT_BINARY_DIR}/lua_src")
file(MAKE_DIRECTORY "${_lua_out}")

# Determinism patch: deterministic hashing of table/function/userdata/thread keys. Stock 5.0 hashes
# them by ADDRESS, so pairs()/next() order over such keys differs between machines and runs (a
# lockstep desync). Each collectable object gets a creation serial (global counter, deterministic
# for a deterministic script run) and ltable.c hashes that instead. Light userdata still hash by
# address; the engine never puts light userdata in script-visible tables.
set(_lua_patches "")
function(alamo_lua_patch file old new)
  list(LENGTH _lua_patches n)
  set(_lua_patches ${_lua_patches} ${n} PARENT_SCOPE)
  set(_lp_${n}_file "${file}" PARENT_SCOPE)
  set(_lp_${n}_old "${old}" PARENT_SCOPE)
  set(_lp_${n}_new "${new}" PARENT_SCOPE)
endfunction()
alamo_lua_patch(lobject.h
  "#define CommonHeader\tGCObject *next; lu_byte tt; lu_byte marked\n"
  "#define CommonHeader\tGCObject *next; lu_byte tt; lu_byte marked; unsigned int alamo_serial\n")
alamo_lua_patch(lstate.h
  "  GCObject *rootgc;  /* list of (almost) all collectable objects */\n"
  "  GCObject *rootgc;  /* list of (almost) all collectable objects */\n  unsigned int alamo_serial;  /* alamo: last creation serial */\n")
alamo_lua_patch(lstate.c "  g->rootgc = NULL;\n" "  g->rootgc = NULL;\n  g->alamo_serial = 0;\n")
alamo_lua_patch(lstate.c "    L->marked = 0;\n" "    L->marked = 0;\n    L->alamo_serial = 0;\n")
alamo_lua_patch(lgc.c "  o->gch.tt = tt;\n" "  o->gch.tt = tt;\n  o->gch.alamo_serial = ++G(L)->alamo_serial;\n")
alamo_lua_patch(lstring.c
  "  u->uv.tt = LUA_TUSERDATA;\n"
  "  u->uv.tt = LUA_TUSERDATA;\n  u->uv.alamo_serial = ++G(L)->alamo_serial;\n")
alamo_lua_patch(lstring.c "  ts->tsv.marked = 0;\n" "  ts->tsv.marked = 0;\n  ts->tsv.alamo_serial = 0;\n")
# Portability (same value): C++ forbids a direct pointer -> 32-bit integer cast on 64-bit targets.
alamo_lua_patch(llimits.h "#define IntPoint(p)  ((lu_hash)(p))\n" "#define IntPoint(p)  ((lu_hash)(size_t)(p))\n")
# Toolchain workaround (not a semantic change): MSVC 14.51 at /O2 miscompiles ltable.c computesizes
# (rehash sizing) - stock 5.0.2 crashes in luaopen_base; /Od, /O1, /d2SSAOptimizer- and clang /O2 are
# fine. Optimisation is switched off for that one cold function on MSVC only.
alamo_lua_patch(ltable.c
  "static void computesizes  (int nums[], int ntotal, int *narray, int *nhash) {\n"
  "#if defined(_MSC_VER) && !defined(__clang__)\n#pragma optimize(\"\", off)  /* alamo: MSVC 14.51 /O2 miscompiles this function */\n#endif\nstatic void computesizes  (int nums[], int ntotal, int *narray, int *nhash) {\n")
alamo_lua_patch(ltable.c
  "  lua_assert(na <= *narray && na >= *narray/2);\n}\n"
  "  lua_assert(na <= *narray && na >= *narray/2);\n}\n#if defined(_MSC_VER) && !defined(__clang__)\n#pragma optimize(\"\", on)\n#endif\n")
alamo_lua_patch(ltable.c
  "      return hashpointer(t, gcvalue(key));\n"
  "      return hashmod(t, cast(lu_hash, gcvalue(key)->gch.alamo_serial));  /* alamo: serial, not address */\n")
# OI lua (L1 determinism, [port]): tostring() of a table / function / full userdata / thread prints the value's
# creation serial (8 hex digits) instead of its address, so printed / concatenated / compared names are the same
# on every run, worker count and compiler. alamo_lua_serial is the C API for the serial (0 for non-collectables).
# Light userdata keeps the address (never script-visible: the engine only uses it for binding upvalues).
alamo_lua_patch(lapi.c
  "LUA_API const void *lua_topointer (lua_State *L, int idx) {\n"
  "/* alamo: creation serial of a collectable value, 0 otherwise */\nLUA_API unsigned int alamo_lua_serial (lua_State *L, int idx) {\n  StkId o = luaA_indexAcceptable(L, idx);\n  return (o == NULL || !iscollectable(o)) ? 0u : gcvalue(o)->gch.alamo_serial;\n}\n\n\nLUA_API const void *lua_topointer (lua_State *L, int idx) {\n")
alamo_lua_patch(lbaselib.c "#include \"lualib.h\"\n"
  "#include \"lualib.h\"\n\nLUA_API unsigned int alamo_lua_serial (lua_State *L, int idx);  /* alamo: lapi.c */\n")
alamo_lua_patch(lbaselib.c "      sprintf(buff, \"table: %p\", lua_topointer(L, 1));\n"
  "      sprintf(buff, \"table: %08X\", alamo_lua_serial(L, 1));  /* alamo: serial, not address */\n")
alamo_lua_patch(lbaselib.c "      sprintf(buff, \"function: %p\", lua_topointer(L, 1));\n"
  "      sprintf(buff, \"function: %08X\", alamo_lua_serial(L, 1));  /* alamo: serial, not address */\n")
alamo_lua_patch(lbaselib.c "      sprintf(buff, \"userdata: %p\", lua_touserdata(L, 1));\n"
  "      if (lua_type(L, 1) == LUA_TUSERDATA) sprintf(buff, \"userdata: %08X\", alamo_lua_serial(L, 1));  /* alamo */\n      else sprintf(buff, \"userdata: %p\", lua_touserdata(L, 1));\n")
alamo_lua_patch(lbaselib.c "      sprintf(buff, \"thread: %p\", (void *)lua_tothread(L, 1));\n"
  "      sprintf(buff, \"thread: %08X\", alamo_lua_serial(L, 1));  /* alamo: serial, not address */\n")
# gcinfo / collectgarbage / weak tables ([port], no patch needed): the collector runs when Lua's own byte count
# (requested sizes, lstate.h nblocks) reaches the threshold, so collection points - and therefore weak-table
# clearing and gcinfo() - are a function of the VM's allocation history only: identical across runs and worker
# counts, and across MSVC / clang (same x64 ABI, same struct sizes). Tested against a golden in test_lua.cpp.
# end OI lua
# OI lua (UB fixes, clang ASan/UBSan 2026-10-28; upstream 5.0.2 UB, not our patches or C API use). Each one keeps the
# value the shipped x64 code already computes, so behaviour, results and CRCs are unchanged:
#   * luaH_new reads the default metatable before f_luaopen has created it (lstate.c: tt = LUA_TTABLE, value.gc
#     uninitialised -> hvalue() member access through a garbage pointer; the result is overwritten right after). The
#     value is now NULL until set and luaH_new stores NULL for that first table (then closed to itself as before).
#   * ngcotouv(NULL) computes &NULL->uv (lfunc.c luaF_findupval / luaF_close loop ends): NULL stays NULL explicitly.
#   * lua_number2int on a double outside int range (ltable.c arrayindex / luaH_get, e.g. a 1e18 key or constant) is a
#     float-cast overflow: range-checked, out of range / NaN -> INT_MIN = x64 cvttsd2si's result; the callers then see
#     (lua_Number)k != n exactly as before.
alamo_lua_patch(lstate.c "  setnilvalue(defaultmeta(L));\n"
  "  setnilvalue(defaultmeta(L));\n  defaultmeta(L)->value.gc = NULL;  /* alamo: luaH_new reads it before it exists */\n")
alamo_lua_patch(ltable.c "  t->metatable = hvalue(defaultmeta(L));\n"
  "  t->metatable = (gcvalue(defaultmeta(L)) == NULL) ? NULL : hvalue(defaultmeta(L));  /* alamo: no UB at open */\n")
alamo_lua_patch(lstate.h
  "\tcheck_exp((o) == NULL || (o)->gch.tt == LUA_TUPVAL, &((o)->uv))\n"
  "\tcheck_exp((o) == NULL || (o)->gch.tt == LUA_TUPVAL, ((o) == NULL ? NULL : &((o)->uv)))  /* alamo: no &NULL->uv */\n")
alamo_lua_patch(ltable.c "#define lua_number2int(i,n)\t((i)=(int)(n))\n"
  "#define lua_number2int(i,n)\t((i)=(((n) >= -2147483648.0 && (n) < 2147483648.0) ? (int)(n) : (-2147483647-1)))  /* alamo: no float-cast UB */\n")
# end OI lua (UB fixes)
# PAF t03 (ASan, 2026-10-30): luaG_symbexec (luaG_checkcode, run on every function loaded from a save) indexes luaP_opmodes
# with the raw 6-bit opcode, so a corrupt save holding an opcode >= NUM_OPCODES read past the table (ASan
# global-buffer-overflow in alamo_lua_save_tests test_corruption). The check rejects it like any other malformed code;
# valid code is unchanged.
alamo_lua_patch(ldebug.c "    OpCode op = GET_OPCODE(i);\n    int a = GETARG_A(i);\n"
  "    OpCode op = GET_OPCODE(i);\n    check(op < NUM_OPCODES);  /* alamo: opcode past the opmodes table */\n    int a = GETARG_A(i);\n")
# N1 lua (memory safety, found by the save tests): a closure keeps a coroutine's local through an OPEN upvalue; when
# that coroutine becomes garbage, stock 5.0.2 marks neither (traverseclosure skips open upvalues, the dead thread is not
# traversed), sweeps the value, then frees the thread, whose luaF_close copies the now dangling value into the closed
# upvalue the live closure still uses (use after free; e.g. a coroutine's dynamic string read by a global closure). As
# in 5.1 (remarkupvals) the value of an open upvalue reached through a live closure is now marked. Only objects that
# were freed while still referenced are affected.
alamo_lua_patch(lgc.c
  "      if (!u->marked) {\n        markobject(st, &u->value);\n        u->marked = 1;\n      }\n"
  "      if (!u->marked) {\n        markobject(st, &u->value);\n        u->marked = 1;\n      }\n      else if (u->v != &u->value)  /* alamo: open upvalue - its value, even when its thread is dead */\n        markobject(st, u->v);\n")
# N1 lua (determinism, found by twin-VM save images): a table node whose value is nil keeps its key ("dead key"), and
# stock 5.0.2 compares keys by address. Once the key's object is freed, a later object allocated at the same address
# (an equal string re-created, a new table ...) MATCHES that node, so lookups, insertions and pairs() order depended on
# allocator address reuse. Each collection now retires the dead keys whose objects it is about to free (key type
# LUA_TNONE, exactly as lgc.c removekey does for weak tables) before the sweep. Keys of live objects are untouched, so
# every address-independent behaviour (and every result that did not hinge on an address coincidence) is unchanged.
alamo_lua_patch(lgc.c
  "void luaC_collectgarbage (lua_State *L) {\n  size_t deadmem = mark(L);\n  luaC_sweep(L, 0);\n"
  "static void alamo_retire_dead_keys (lua_State *L) {  /* alamo: see engine/lua/CMakeLists.txt */\n  GCObject *o;\n  for (o = G(L)->rootgc; o != NULL; o = o->gch.next) {\n    if (o->gch.tt == LUA_TTABLE && ismarked(o)) {\n      Table *h = gcotoh(o);\n      int i = sizenode(h);\n      while (i--) {\n        Node *n = gnode(h, i);\n        if (ttisnil(gval(n)) && iscollectable(gkey(n)) && !ismarked(gcvalue(gkey(n))))\n          setttype(gkey(n), LUA_TNONE);\n      }\n    }\n  }\n}\n\nvoid luaC_collectgarbage (lua_State *L) {\n  size_t deadmem = mark(L);\n  alamo_retire_dead_keys(L);\n  luaC_sweep(L, 0);\n")
# OPT2 t04 (Lua GC without spikes): the collection is split in two - alamo_luaC_collect(L, 0) marks, sweeps and sets the
# new threshold, touching nothing but L's own global_State; the finalizers (__gc, which run Lua code and so may call
# engine bindings) stay in luaC_callGCTM. luaC_collectgarbage = alamo_luaC_collect(L, 1), the stock order, so automatic
# collections are unchanged. That split is what lets collect_scheduled (src/gc_schedule.cpp) collect independent VMs on
# job workers and run any finalizers afterwards on the sim thread. alamo_lua_gc_hook (null = off) brackets the mark /
# sweep part of every collection (never the finalizers, so it cannot nest); lua_vm.cpp points it at the "lua.gc"
# profiler zone. It is written once, before the first VM exists, and only read afterwards.
alamo_lua_patch(lgc.c
  "void luaC_collectgarbage (lua_State *L) {\n  size_t deadmem = mark(L);\n  alamo_retire_dead_keys(L);\n  luaC_sweep(L, 0);\n  checkSizes(L, deadmem);\n  luaC_callGCTM(L);\n}\n"
  "void (*alamo_lua_gc_hook) (lua_State *L, int end) = NULL;  /* alamo: OPT2 t04, see engine/lua/CMakeLists.txt */\n\nvoid alamo_luaC_collect (lua_State *L, int finalize) {  /* alamo: OPT2 t04 */\n  size_t deadmem;\n  if (alamo_lua_gc_hook) alamo_lua_gc_hook(L, 0);\n  deadmem = mark(L);\n  alamo_retire_dead_keys(L);\n  luaC_sweep(L, 0);\n  checkSizes(L, deadmem);\n  if (alamo_lua_gc_hook) alamo_lua_gc_hook(L, 1);\n  if (finalize) luaC_callGCTM(L);\n}\n\nvoid luaC_collectgarbage (lua_State *L) {\n  alamo_luaC_collect(L, 1);\n}\n")
alamo_lua_patch(lgc.h "void luaC_collectgarbage (lua_State *L);\n"
  "void luaC_collectgarbage (lua_State *L);\nvoid alamo_luaC_collect (lua_State *L, int finalize);  /* alamo: OPT2 t04 */\nextern void (*alamo_lua_gc_hook) (lua_State *L, int end);  /* alamo: OPT2 t04 */\n")

file(GLOB _lua_all_src RELATIVE "${ALAMO_LUA_DIR}/src" "${ALAMO_LUA_DIR}/src/*.c" "${ALAMO_LUA_DIR}/src/*.h")
file(GLOB _lua_all_lib RELATIVE "${ALAMO_LUA_DIR}/src/lib" "${ALAMO_LUA_DIR}/src/lib/*.c")
set(_lua_sources "")
set(_lua_applied 0)
foreach(f IN LISTS _lua_all_src _lua_all_lib)
  if(EXISTS "${ALAMO_LUA_DIR}/src/${f}")
    set(_in "${ALAMO_LUA_DIR}/src/${f}")
  else()
    set(_in "${ALAMO_LUA_DIR}/src/lib/${f}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_in}")
  file(READ "${_in}" _text)
  string(REPLACE "\r\n" "\n" _text "${_text}")
  foreach(p IN LISTS _lua_patches)
    if(_lp_${p}_file STREQUAL f)
      string(FIND "${_text}" "${_lp_${p}_old}" _pos)
      if(_pos EQUAL -1)
        message(FATAL_ERROR "engine/lua: determinism patch ${p} for ${f} does not apply (unexpected Lua source?)")
      endif()
      string(REPLACE "${_lp_${p}_old}" "${_lp_${p}_new}" _text "${_text}")
      math(EXPR _lua_applied "${_lua_applied} + 1")
    endif()
  endforeach()
  set(_out "${_lua_out}/${f}")
  set(_old_text "")
  if(EXISTS "${_out}")
    file(READ "${_out}" _old_text)
  endif()
  if(NOT _old_text STREQUAL _text)
    file(WRITE "${_out}" "${_text}")
  endif()
  get_filename_component(_stem "${f}" NAME_WE)
  get_filename_component(_ext "${f}" EXT)
  if(_ext STREQUAL ".c" AND (_stem IN_LIST _lua_core OR _stem IN_LIST _lua_libs))
    list(APPEND _lua_sources "${_out}")
  endif()
endforeach()
list(LENGTH _lua_patches _lua_npatches)
if(NOT _lua_applied EQUAL _lua_npatches)
  message(FATAL_ERROR "engine/lua: ${_lua_applied} of ${_lua_npatches} determinism patches applied")
endif()
set_source_files_properties(${_lua_sources} PROPERTIES LANGUAGE CXX)

add_library(alamo_lua_vm STATIC ${_lua_sources})
target_include_directories(alamo_lua_vm SYSTEM PUBLIC "${ALAMO_LUA_DIR}/include")
target_compile_features(alamo_lua_vm PRIVATE cxx_std_20)
# 'register' is gone in C++17; Lua 5.0 still spells it.
target_compile_definitions(alamo_lua_vm PRIVATE register=)
if(MSVC)
  target_compile_options(alamo_lua_vm PRIVATE /W0 /fp:precise /TP)
else()
  target_compile_options(alamo_lua_vm PRIVATE -w -ffp-contract=off -x c++)
endif()

add_library(alamo_lua STATIC
  src/lua_vm.cpp
  src/gc_schedule.cpp
  src/bindings.cpp
  src/bind_engine.cpp
  src/bind_registry.cpp
  src/bind_objects.cpp
  src/bind_players.cpp
  src/bind_story.cpp
  src/bind_presentation.cpp
  src/math_lib.cpp
  src/script_loader.cpp
  src/static_scan.cpp
)
add_library(alamo::lua ALIAS alamo_lua)
# --- L1 stage 2, family PRESENTATION (owner: presentation agent) ------------------------------
# PresentationLog / PresentationState (include/alamo/lua/presentation.hpp): the deterministic event
# log and state model the presentation bindings (src/bind_presentation.cpp) write into.
target_sources(alamo_lua PRIVATE src/presentation.cpp)
# --- end PRESENTATION ----------------------------------------------------------------------------
# --- AI family (A2, owner: A2 agent): plan / free-store bindings, include/alamo/lua/ai_bridge.hpp ---
target_sources(alamo_lua PRIVATE src/bind_ai.cpp)
# --- end AI family ---
# --- tactical AI family (A1, owner: A1 agent): land/space task-force orders, tactical queries,
#     include/alamo/lua/ai_tactical_bridge.hpp ---
target_sources(alamo_lua PRIVATE src/bind_ai_tactical.cpp)
# --- end tactical AI family ---
# --- story family (L1 stage 2 / S4): story plot model, include/alamo/lua/story_model.hpp ---
target_sources(alamo_lua PRIVATE src/story_model.cpp)
# --- end story family ---
# --- L1/S4 wave 2b (owner: L1/S4 agent): script gaps (special weapons, TSW, cinematics, markup, parent mode),
#     include/alamo/lua/script_gaps.hpp; test exe alamo_lua_script_gaps_tests (family loop below) ---
target_sources(alamo_lua PRIVATE src/bind_script_gaps.cpp)
# --- end L1/S4 wave 2b ---
# --- fog follow-up (owner: fog follow-up agent): FogOfWar.* / LuaFOWCellsClass.Undo_Reveal, include/alamo/lua/fog_bridge.hpp
#     (docs/re/FOG_OF_WAR.md §6) on engine/tactical/fog (configured before lua); without it the bindings answer as
#     outside a tactical mode. Test exe alamo_lua_fog_tests (family loop below). ---
target_sources(alamo_lua PRIVATE src/bind_fog.cpp)
if(TARGET alamo_tactical_fog)
  target_link_libraries(alamo_lua PUBLIC alamo_tactical_fog)
  target_compile_definitions(alamo_lua PUBLIC ALAMO_LUA_HAVE_FOG=1)
endif()
# --- end fog follow-up ---
# --- L1 final (owner: L1 bindings agent): the last unbound globals / methods, include/alamo/lua/script_remaining.hpp;
#     test exe alamo_lua_script_remaining_tests (family loop below) ---
target_sources(alamo_lua PRIVATE src/bind_script_remaining.cpp)
# --- end L1 final ---
# --- L1 stage 2, family PLAYERS (owner: players agent) -------------------------------------------
# src/bind_players.cpp + include/alamo/lua/players.hpp read credits/players from the S3 galactic sim
# (alamo_galactic_sim) and evaluate perception equations with engine/ai (alamo_ai, configured after
# lua, so tested by module list, not TARGET). Without both the players family table is empty.
if(TARGET alamo_galactic_sim AND "ai" IN_LIST ALAMO_ENGINE_MODULES AND EXISTS ${PROJECT_SOURCE_DIR}/engine/ai/CMakeLists.txt)
  target_link_libraries(alamo_lua PUBLIC alamo_galactic_sim alamo_ai)
  target_compile_definitions(alamo_lua PUBLIC ALAMO_LUA_PLAYERS=1)
endif()
# --- end PLAYERS ---------------------------------------------------------------------------------
# --- N1 save / load (owner: n1-lua): LuaVm::write / read persist the whole Lua heap (src/vm_persist.cpp). That file
#     reads Lua's internal structures, so it includes the PATCHED internal headers of lua_src (SYSTEM: their macros
#     and declarations are third-party code). Test exe alamo_lua_save_tests (family loop below). ---
target_sources(alamo_lua PRIVATE src/vm_persist.cpp)
target_include_directories(alamo_lua SYSTEM PRIVATE "${_lua_out}")
# q6 t19 (docs/re/LUA_SAVE.md): the per-owner script block (include/alamo/lua/script_save.hpp) on top of LuaVm::write /
# read, shared by the story, GameScoring and object-script saves. Test exe alamo_lua_script_save_tests (family loop).
target_sources(alamo_lua PRIVATE src/script_save.cpp)
# --- end N1 save / load ---
# --- L1 stage 2, family OBJECTS (owner: objects agent) -------------------------------------------
# src/bind_objects.cpp + include/alamo/lua/object_bridge.hpp: Lua orders are EventData run through
# engine/commands' CommandPipeline::execute_direct (COMMANDS.md §5). Without engine/commands the
# object bindings still work; orders are then not issued (ALAMO_LUA_HAVE_COMMANDS unset).
if(TARGET alamo_commands)
  target_link_libraries(alamo_lua PUBLIC alamo_commands)
  target_compile_definitions(alamo_lua PUBLIC ALAMO_LUA_HAVE_COMMANDS=1)
endif()
# L1/S4 wave 2a: ability bindings on the S9 AbilitySystem (engine/tactical/abilities, also the single
# UnitAbilityType name table) and Play_Animation / Get_Bone_Position on the R3 AnimWorld (engine/anim). Both
# are configured before lua; without them the bindings keep their stage-2 behaviour (object_bridge.hpp).
if(TARGET alamo_tactical_abilities)
  target_link_libraries(alamo_lua PUBLIC alamo_tactical_abilities)
  target_compile_definitions(alamo_lua PUBLIC ALAMO_LUA_HAVE_ABILITIES=1)
endif()
if(TARGET alamo_anim)
  target_link_libraries(alamo_lua PUBLIC alamo_anim)
  target_compile_definitions(alamo_lua PUBLIC ALAMO_LUA_HAVE_ANIM=1)
endif()
# --- end OBJECTS ---------------------------------------------------------------------------------
target_include_directories(alamo_lua PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_lua PUBLIC alamo_objects alamo_core alamo_engine_flags PRIVATE alamo_lua_vm)

add_executable(alamo_lua_tests tests/test_lua.cpp)
target_link_libraries(alamo_lua_tests PRIVATE alamo_lua alamo_lua_vm Threads::Threads)
# Binding coverage / script compile reports go to <project>/logs/lua_*.txt.
target_compile_definitions(alamo_lua_tests PRIVATE "ALAMO_LUA_LOG_DIR=\"${PROJECT_SOURCE_DIR}/logs\"")
if(TARGET alamo_vfs)
  target_link_libraries(alamo_lua_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_lua_tests PRIVATE ALAMO_LUA_HAVE_VFS=1)
endif()
add_test(NAME alamo_lua_tests COMMAND alamo_lua_tests)
set_tests_properties(alamo_lua_tests PROPERTIES TIMEOUT 120)

# One test exe per binding family (L1 stage 2), so families are developed in parallel without
# touching each other's files. A family's test file is created by the agent that owns that family.
foreach(_fam objects players story presentation tactical script_gaps fog script_remaining save gc script_save)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_lua_${_fam}.cpp)
    add_executable(alamo_lua_${_fam}_tests tests/test_lua_${_fam}.cpp)
    target_link_libraries(alamo_lua_${_fam}_tests PRIVATE alamo_lua alamo_lua_vm Threads::Threads)
    target_include_directories(alamo_lua_${_fam}_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    if(TARGET alamo_vfs)
      target_link_libraries(alamo_lua_${_fam}_tests PRIVATE alamo_vfs)
      target_compile_definitions(alamo_lua_${_fam}_tests PRIVATE ALAMO_LUA_HAVE_VFS=1)
    endif()
    add_test(NAME alamo_lua_${_fam}_tests COMMAND alamo_lua_${_fam}_tests)
    set_tests_properties(alamo_lua_${_fam}_tests PROPERTIES TIMEOUT 120)
  endif()
endforeach()
# OPT2 t04: the GC schedule test checks 0 / 1 / 4 / 16 workers on engine/jobs (configured before lua) when it is built.
if(TARGET alamo_lua_gc_tests AND TARGET alamo_jobs)
  target_link_libraries(alamo_lua_gc_tests PRIVATE alamo_jobs)
  target_compile_definitions(alamo_lua_gc_tests PRIVATE ALAMO_LUA_HAVE_JOBS=1)
endif()

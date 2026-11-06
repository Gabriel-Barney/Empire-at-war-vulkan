# Phase 2 engine (headless simulation core). Clean-room: written from docs/, never from re_reference/.
#
# Modules (each a static library under engine/<module>):
#   core      - hashing, logging, file-provider interface, galaxy DOD tables, parallel_for hook
#   vfs       - virtual file system implementing the verified Data\ resolution order (needs tools/meg)
#   data      - XML reader + game-database loaders (GameConstants, Planets, TradeRoutes, Campaigns)
#   jobs      - job system / thread pool + deterministic fixed-step tick
#   galactic  - planet graph, re-entrant A* fleet pathing, AI all-pairs (FW reference + parallel)
#   apps      - alamo_headless host (+ Phase 3 viewer)
# Phase 3 (Vulkan 1.3 renderer):
#   assets    - ALO/ALA/texture loading into GPU-ready CPU data (no Vulkan)
#   rhi       - Vulkan 1.3 backend: runtime vulkan-1 loader, device, swapchain, memory, sync
#   materials - FXO -> PSO keys, constant layouts, SM6 HLSL hot-set + FF ubershader compiled by DXC
#   shaderxlat - track B: D3D9 SM1-2 bytecode -> HLSL -> SPIR-V (DXC at build time and, via dxcompiler.dll, at load time)
#   terrain   - .ted maps: terrain patches + per-material blend textures, water plane, environments, objects (CPU, no Vulkan)
#   render    - scene renderer joining assets + materials + rhi; alamo_render test app
# Phase 4 (game):
#   objects   - GameObjectType model (all XML fields, Variant_Of_Existing_Type), entity ids, SoA object store
#   anim      - sim-side animation state (clip/frame/blend, deterministic bone poses read by gameplay; R3)
#   commands  - command/event pipeline: player/AI/network orders queued and applied at a scheduled sim tick
#   lua       - vendored Lua 5.0.2 VM (approved 2026-10-27) + the engine's Lua bindings (one VM owner thread)
#   ai        - perception evaluation (Node_Weight, threat/value), later galactic and tactical AI
#   tactical  - land/space battle simulation: spatial tree (TreeCull); movement/ (S7) and combat/ (S8) subfolders
#   ui        - input, camera control, selection picking, later HUD and GUI (P1, P2, P4)
#   audio     - SFX/music/speech events on miniaudio (approved 2026-10-27), Bink video via FFmpeg later (P3)
#   video     - Bink movie playback through FFmpeg's Bink decoder (approved 2026-10-27, P3)
#
# ALAMO_ENGINE_MODULES limits which modules are configured (handy while one module is mid-edit).

set(ALAMO_ENGINE_MODULES "core;vfs;data;jobs;galactic;objects;anim;commands;tactical;lua;ai;assets;rhi;shaderxlat;materials;terrain;render;ui;audio;video;apps" CACHE STRING "Engine modules to configure")

# Phase 3 deps, project-local (toolchain\3_renderer_deps.cmd). Header-only Vulkan; vulkan-1 is loaded at runtime.
set(ALAMO_VULKAN_HEADERS "${PROJECT_SOURCE_DIR}/toolchain/vulkan-headers/include" CACHE PATH "Vulkan-Headers include dir")
if(WIN32)
  set(_alamo_dxc_default "${PROJECT_SOURCE_DIR}/toolchain/dxc/bin/x64/dxc.exe")
else()
  set(_alamo_dxc_default "dxc")
endif()
set(ALAMO_DXC "${_alamo_dxc_default}" CACHE FILEPATH "DXC executable (HLSL -> SPIR-V)")
# Runtime DXC (approved by Gabriel 2026-10-26): shaderxlat loads dxcompiler.dll dynamically to translate mod FXOs
# at load time. Apps copy it (and dxil.dll) next to the exe; nothing links against it.
if(WIN32)
  set(ALAMO_DXC_RUNTIME_DIR "${PROJECT_SOURCE_DIR}/toolchain/dxc/bin/x64" CACHE PATH "Folder with dxcompiler.dll/dxil.dll")
endif()
if(EXISTS "${ALAMO_VULKAN_HEADERS}/vulkan/vulkan.h")
  add_library(alamo_vulkan_headers INTERFACE)
  target_include_directories(alamo_vulkan_headers SYSTEM INTERFACE "${ALAMO_VULKAN_HEADERS}")
  target_compile_definitions(alamo_vulkan_headers INTERFACE VK_NO_PROTOTYPES)
endif()
# VMA v3.3.0 (header-only, MIT; approved by Gabriel 2026-10-26). Functions are fed at runtime
# (VMA_STATIC_VULKAN_FUNCTIONS=0, VMA_DYNAMIC_VULKAN_FUNCTIONS=1) since vulkan-1 is loaded dynamically.
set(ALAMO_VMA_INCLUDE "${PROJECT_SOURCE_DIR}/toolchain/vma/include" CACHE PATH "VMA include dir")
if(TARGET alamo_vulkan_headers AND EXISTS "${ALAMO_VMA_INCLUDE}/vk_mem_alloc.h")
  add_library(alamo_vma INTERFACE)
  target_include_directories(alamo_vma SYSTEM INTERFACE "${ALAMO_VMA_INCLUDE}")
  target_compile_definitions(alamo_vma INTERFACE VMA_STATIC_VULKAN_FUNCTIONS=0 VMA_DYNAMIC_VULKAN_FUNCTIONS=1)
  target_link_libraries(alamo_vma INTERFACE alamo_vulkan_headers)
endif()

# Common warning/determinism flags for every engine target.
add_library(alamo_engine_flags INTERFACE)
if(MSVC)
  # /fp:precise + no FMA contraction keeps float results identical between builds (lockstep CRC).
  target_compile_options(alamo_engine_flags INTERFACE /W4 /fp:precise /utf-8 /permissive-)
else()
  target_compile_options(alamo_engine_flags INTERFACE -Wall -Wextra -Wpedantic -ffp-contract=off)
endif()
target_compile_features(alamo_engine_flags INTERFACE cxx_std_20)

find_package(Threads REQUIRED)

# Test tiers: tests that take more than a few seconds carry the ctest label "slow". The inner loop runs the fast tier
# (ctest -LE slow, what build_agent.cmd / build_module.cmd do by default); the gate scripts run everything. Call from the
# directory that created the tests (set_property(TEST) only sees tests of the current directory). Labels a test that
# does not exist as a no-op, so a list can name tests that are only built with some modules.
function(alamo_label_slow)
  foreach(_t IN LISTS ARGN)
    if(TEST ${_t})
      set_property(TEST ${_t} APPEND PROPERTY LABELS slow)
    endif()
  endforeach()
endfunction()

# Precompiled project header (2026-11-06): a target whose sources all start with one big shared header (alamo_game: game_app.hpp, ~450
# headers per file) parses it once instead of once per file; measured on app_screens.cpp / app_input.cpp the compiler front end went
# from 5.2 s to 0.4 s per file. Only worth it where the shared set is large: most targets share almost no project headers (a std-only PCH
# saved 0.6 s of 5 s). sccache cannot be assumed to cache PCH compiles, so those files are compiled for real; at about 1 s each that is cheap.
#   alamo_target_pch(<target> <header> [SKIP <source>...])   SKIP: sources that do not include the header (they would get it force-included)
# -DALAMO_PCH=OFF turns it off (build trees keep the setting, so pass it once).
option(ALAMO_PCH "Precompile the shared header of targets that have a big one (alamo_target_pch)" ON)
function(alamo_target_pch target header)
  cmake_parse_arguments(PARSE_ARGV 2 _p "" "" "SKIP")
  if(NOT ALAMO_PCH)
    return()
  endif()
  target_precompile_headers(${target} PRIVATE ${header})
  if(_p_SKIP)
    set_source_files_properties(${_p_SKIP} TARGET_DIRECTORY ${target} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
  endif()
endfunction()

# Nightly tier (2026-11-06): tests with the ctest label "nightly" repeat coverage the per-change gate already has (today the 1- and
# 16-worker runs of the long worker-count checks, which add nothing the 0 / 4 / machine-default runs do not show on most edits). The
# gate scripts (build_msvc / build_clang / build_asan / build_gate) skip them (ctest -LE nightly); tools\gate_scheduled.cmd, the
# nightly task, and `set ALAMO_TESTS=all` run everything. Labelling a test that does not exist is a no-op.
function(alamo_label_nightly)
  foreach(_t IN LISTS ARGN)
    if(TEST ${_t})
      set_property(TEST ${_t} APPEND PROPERTY LABELS nightly)
    endif()
  endforeach()
endfunction()

# Worker-count check (OPT3 t07; docs/PHASE4.md "Worker-count check"): one scenario at 0 / 1 / 4 / 16 workers and the
# machine default (hardware threads - 1), per-tick CRCs compared. One ctest entry per count so a long run's counts go side
# by side under ctest -j: <name> runs `--workers 0 --crc-out <bin>/<name>_w0.crc` (FIXTURES_SETUP <name>_w0) and
# <name>_w<N> runs `--workers N --golden <that file>` (FIXTURES_REQUIRED <name>_w0). The command must accept
# --workers N, --crc-out FILE and --golden FILE (alamo_headless does) and fail when the golden differs.
#   alamo_add_worker_count_check(<name> COMMAND <exe> [arg...]
#       [SETUP_ARGS arg...]   extra args for the 0-worker entry only (e.g. --golden <checked-in file>, an --expect check)
#       [CHECK_ARGS arg...]   extra args for the other entries only
#       [WORKERS n...]        counts besides 0 (default 1 4 16 and the machine default; duplicates and 0 dropped)
#       [NIGHTLY_WORKERS n...] counts that also run but carry the label "nightly" (see above); default with SLOW and no WORKERS: 1 16
#                             (then WORKERS defaults to 4 and the machine default). Ignored counts already in WORKERS.
#       [TIMEOUT s] [SLOW]    TIMEOUT for every entry; SLOW labels them all "slow" (do it for runs over ~5 s)
#       [PROPERTIES p v...])  more test properties for every entry (e.g. SKIP_RETURN_CODE 77)
# More entries can compare against the same log: FIXTURES_REQUIRED <name>_w0 and --golden <bin>/<name>_w0.crc
# (the path is also in <name>_CRC in the caller's scope). For short scenarios the in-process form is
# alamo::jobs::check_worker_counts() (engine/jobs crc_recorder.hpp).
function(alamo_add_worker_count_check name)
  cmake_parse_arguments(PARSE_ARGV 1 _wc "SLOW" "TIMEOUT" "COMMAND;SETUP_ARGS;CHECK_ARGS;WORKERS;NIGHTLY_WORKERS;PROPERTIES")
  if(NOT _wc_COMMAND)
    message(FATAL_ERROR "alamo_add_worker_count_check(${name}): COMMAND is required")
  endif()
  cmake_host_system_information(RESULT _cores QUERY NUMBER_OF_LOGICAL_CORES)
  math(EXPR _default "${_cores} - 1")
  if(NOT _wc_WORKERS)
    if(_wc_SLOW AND NOT _wc_NIGHTLY_WORKERS)
      set(_wc_WORKERS 4 ${_default})
      set(_wc_NIGHTLY_WORKERS 1 16)
    else()
      set(_wc_WORKERS 1 4 16 ${_default})
    endif()
  endif()
  list(REMOVE_DUPLICATES _wc_WORKERS)
  list(REMOVE_ITEM _wc_WORKERS 0)
  list(REMOVE_DUPLICATES _wc_NIGHTLY_WORKERS)
  list(REMOVE_ITEM _wc_NIGHTLY_WORKERS 0)
  foreach(_w IN LISTS _wc_WORKERS)
    list(REMOVE_ITEM _wc_NIGHTLY_WORKERS ${_w})
  endforeach()
  set(_crc ${CMAKE_CURRENT_BINARY_DIR}/${name}_w0.crc)
  set(_props ${_wc_PROPERTIES})
  if(_wc_TIMEOUT)
    list(APPEND _props TIMEOUT ${_wc_TIMEOUT})
  endif()
  if(_wc_SLOW)
    list(APPEND _props LABELS slow)
  endif()
  add_test(NAME ${name} COMMAND ${_wc_COMMAND} --workers 0 --crc-out ${_crc} ${_wc_SETUP_ARGS})
  set_tests_properties(${name} PROPERTIES FIXTURES_SETUP ${name}_w0 ${_props})
  foreach(_w IN LISTS _wc_WORKERS _wc_NIGHTLY_WORKERS)
    add_test(NAME ${name}_w${_w} COMMAND ${_wc_COMMAND} --workers ${_w} --golden ${_crc} ${_wc_CHECK_ARGS})
    set_tests_properties(${name}_w${_w} PROPERTIES FIXTURES_REQUIRED ${name}_w0 ${_props})
  endforeach()
  foreach(_w IN LISTS _wc_NIGHTLY_WORKERS)
    alamo_label_nightly(${name}_w${_w})
  endforeach()
  set(${name}_CRC ${_crc} PARENT_SCOPE)
endfunction()

# Module labels (2026-11-06): every test gets the label of the engine module whose folder (or a subfolder) created it, so
# `ctest -L "^<module>$"` runs a module's tests (build_module.cmd does) instead of guessing from test names. Needs the DIRECTORY
# form of set_property(TEST) / the directory property TESTS (CMake 3.28+; older CMake just skips the labels).
function(alamo_label_module_tests dir module)
  get_property(_tests DIRECTORY ${dir} PROPERTY TESTS)
  foreach(_t IN LISTS _tests)
    set_property(TEST ${_t} DIRECTORY ${dir} APPEND PROPERTY LABELS ${module})
  endforeach()
  get_property(_subdirs DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
  foreach(_d IN LISTS _subdirs)
    alamo_label_module_tests(${_d} ${module})
  endforeach()
endfunction()

foreach(m core vfs data jobs galactic objects anim commands tactical lua ai assets rhi shaderxlat materials terrain render ui audio video apps)
  if(m IN_LIST ALAMO_ENGINE_MODULES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${m}/CMakeLists.txt)
    add_subdirectory(${m})
    if(NOT CMAKE_VERSION VERSION_LESS 3.28)
      alamo_label_module_tests(${CMAKE_CURRENT_SOURCE_DIR}/${m} ${m})
    endif()
  endif()
endforeach()

# Phase 2 engine (headless simulation core). Clean-room: written from docs/, never from re_reference/.
#
# Modules (each a static library under engine/<module>):
#   core      - hashing, logging, file-provider interface, galaxy DOD tables, parallel_for hook
#   vfs       - virtual file system implementing the verified Data\ resolution order (needs tools/meg)
#   data      - XML reader + game-database loaders (GameConstants, Planets, TradeRoutes, Campaigns)
#   jobs      - job system / thread pool + deterministic fixed-step tick
#   galactic  - planet graph, re-entrant A* fleet pathing, AI all-pairs (FW reference + parallel)
#   apps      - alamo_headless host (+ Phase 3 viewer)
#   test_support - header-only CHECK harness, env / data-root / file helpers and ParallelFor pool for the test exes
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

# Shared precompiled header (roadmap bo1 t02). alamo_pch is a tiny static library that owns the PCH; every engine target
# created below reuses it (CMAKE_PRECOMPILE_HEADERS_REUSE_FROM), so the standard headers are parsed once per tree instead of
# once per target (the loop at the end of this file points every target at it). -DALAMO_PCH=OFF builds without it. A target whose flags differ (vendored Lua) opts out in its own file.
# Default: ON only when no compiler cache is active. sccache cannot cache a compile that uses /Fp (measured 2026-11-13:
# 519 of ~880 compiles uncacheable), so with the cache on the PCH costs more in cross-tree cache hits than it saves.
if(CMAKE_CXX_COMPILER_LAUNCHER)
  set(_alamo_pch_default OFF)
else()
  set(_alamo_pch_default ON)
endif()
option(ALAMO_PCH "Use the shared precompiled header (engine/pch)" ${_alamo_pch_default})
if(ALAMO_PCH)
  add_library(alamo_pch STATIC pch/pch_host.cpp)
  target_include_directories(alamo_pch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/core/include)
  target_link_libraries(alamo_pch PRIVATE alamo_engine_flags Threads::Threads)
  target_precompile_headers(alamo_pch PRIVATE pch/pch.hpp)
  set(CMAKE_PRECOMPILE_HEADERS_REUSE_FROM alamo_pch)
endif()

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

# Worker-count check (OPT3 t07; docs/BUILD.md "Worker-count check"): one scenario at 0 / 1 / 4 / 16 workers and the
# machine default (hardware threads - 1), per-tick CRCs compared. One ctest entry per count so a long run's counts go side
# by side under ctest -j: <name> runs `--workers 0 --crc-out <bin>/<name>_w0.crc` (FIXTURES_SETUP <name>_w0) and
# <name>_w<N> runs `--workers N --golden <that file>` (FIXTURES_REQUIRED <name>_w0). The command must accept
# --workers N, --crc-out FILE and --golden FILE (alamo_headless does) and fail when the golden differs.
#   alamo_add_worker_count_check(<name> COMMAND <exe> [arg...]
#       [SETUP_ARGS arg...]   extra args for the 0-worker entry only (e.g. --golden <checked-in file>, an --expect check)
#       [CHECK_ARGS arg...]   extra args for the other entries only
#       [WORKERS n...]        counts besides 0 (default 1 4 16 and the machine default; duplicates and 0 dropped)
#       [TIMEOUT s] [SLOW]    TIMEOUT for every entry; SLOW labels them all "slow" (do it for runs over ~5 s)
#       [PROPERTIES p v...])  more test properties for every entry (e.g. SKIP_RETURN_CODE 77)
# More entries can compare against the same log: FIXTURES_REQUIRED <name>_w0 and --golden <bin>/<name>_w0.crc
# (the path is also in <name>_CRC in the caller's scope). For short scenarios the in-process form is
# alamo::jobs::check_worker_counts() (engine/jobs crc_recorder.hpp).
function(alamo_add_worker_count_check name)
  cmake_parse_arguments(PARSE_ARGV 1 _wc "SLOW" "TIMEOUT" "COMMAND;SETUP_ARGS;CHECK_ARGS;WORKERS;PROPERTIES")
  if(NOT _wc_COMMAND)
    message(FATAL_ERROR "alamo_add_worker_count_check(${name}): COMMAND is required")
  endif()
  if(NOT _wc_WORKERS)
    cmake_host_system_information(RESULT _cores QUERY NUMBER_OF_LOGICAL_CORES)
    math(EXPR _default "${_cores} - 1")
    set(_wc_WORKERS 1 4 16 ${_default})
  endif()
  list(REMOVE_DUPLICATES _wc_WORKERS)
  list(REMOVE_ITEM _wc_WORKERS 0)
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
  foreach(_w IN LISTS _wc_WORKERS)
    add_test(NAME ${name}_w${_w} COMMAND ${_wc_COMMAND} --workers ${_w} --golden ${_crc} ${_wc_CHECK_ARGS})
    set_tests_properties(${name}_w${_w} PROPERTIES FIXTURES_REQUIRED ${name}_w0 ${_props})
  endforeach()
  set(${name}_CRC ${_crc} PARENT_SCOPE)
endfunction()

# Header-only test support (CU4). Not in ALAMO_ENGINE_MODULES (a cached list), so existing build trees get it too.
add_subdirectory(test_support)

foreach(m core vfs data jobs galactic objects anim commands tactical lua ai assets rhi shaderxlat materials terrain render ui audio video apps)
  if(m IN_LIST ALAMO_ENGINE_MODULES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${m}/CMakeLists.txt)
    add_subdirectory(${m})
  endif()
endforeach()

# Point every engine library / executable at the shared PCH (roadmap bo1 t02). Done here, after all subdirectories, so no
# module file needs to know about it. Targets that opted out (DISABLE_PRECOMPILE_HEADERS) or already have their own PCH stay.
if(ALAMO_PCH)
  function(_alamo_collect_targets dir out)
    get_property(_t DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_s DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach(_d IN LISTS _s)
      _alamo_collect_targets(${_d} _sub)
      list(APPEND _t ${_sub})
    endforeach()
    set(${out} ${_t} PARENT_SCOPE)
  endfunction()
  _alamo_collect_targets(${CMAKE_CURRENT_SOURCE_DIR} _alamo_all_targets)
  foreach(_t IN LISTS _alamo_all_targets)
    get_target_property(_type ${_t} TYPE)
    get_target_property(_off ${_t} DISABLE_PRECOMPILE_HEADERS)
    get_target_property(_own ${_t} PRECOMPILE_HEADERS)
    if(NOT _t STREQUAL alamo_pch AND NOT _off AND NOT _own AND _type MATCHES "^(STATIC_LIBRARY|EXECUTABLE|OBJECT_LIBRARY)$")
      set_property(TARGET ${_t} PROPERTY PRECOMPILE_HEADERS_REUSE_FROM alamo_pch)
    endif()
  endforeach()
endif()

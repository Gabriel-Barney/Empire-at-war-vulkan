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

foreach(m core vfs data jobs galactic objects anim commands tactical lua ai assets rhi shaderxlat materials terrain render ui audio video apps)
  if(m IN_LIST ALAMO_ENGINE_MODULES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${m}/CMakeLists.txt)
    add_subdirectory(${m})
  endif()
endforeach()

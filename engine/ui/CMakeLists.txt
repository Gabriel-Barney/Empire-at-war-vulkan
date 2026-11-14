# P1 ui - input layer, camera controllers (galactic / Land_Mode / Space_Mode), screen-ray picking against
# the tactical spatial trees, box selection and click -> command translation (engine/commands). Headless: depends on core, data, tactical, commands only.
#   alamo_ui_rhi  rhi::Window -> InputFrame adapter (only when the rhi module is configured)
#   render_bridge.hpp  header-only CameraParams -> render::OrbitCamera / ViewMatrices (include from code
#                      that links alamo_render)
foreach(_dep alamo_core alamo_data alamo_tactical alamo_commands)
  if(NOT TARGET ${_dep})
    message(WARNING "alamo_ui skipped: needs ${_dep} (modules core;data;tactical;commands)")
    return()
  endif()
endforeach()

add_library(alamo_ui STATIC
  src/input.cpp
  src/camera.cpp
  src/picking.cpp
  src/orders.cpp
)
add_library(alamo::ui ALIAS alamo_ui)
target_include_directories(alamo_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_ui PUBLIC alamo_core alamo_engine_flags alamo_tactical alamo_commands PRIVATE alamo_data)

if(TARGET alamo_rhi)
  add_library(alamo_ui_rhi STATIC src/window_input.cpp)
  add_library(alamo::ui_rhi ALIAS alamo_ui_rhi)
  target_link_libraries(alamo_ui_rhi PUBLIC alamo_ui alamo_rhi)
endif()

# P2 HUD, CPU side (headless): canvas + ortho cameras, CommandBarComponents / MTD atlas / MasterTextFile, layout,
# 13-layer draw list, fonts (GDI on Windows), world-space brackets / bars / fly-text, selection panel, input routing.
add_library(alamo_ui_hud STATIC
  src/hud_canvas.cpp
  src/hud_components.cpp
  src/hud_font.cpp
  src/hud_draw.cpp
  src/credits_roll.cpp
  src/hud_world.cpp
  src/hud_input.cpp
  src/hud_buttons.cpp
  src/hud_radar.cpp
  src/hud_markers.cpp
  src/hotkeys.cpp
  src/tactical_hud.cpp
  # P4 GUI & menus: GUIDialogs.rc / Resource.h / GUIDialogs.xml, dialog manager, menus, load screens, galactic HUD,
  # Lua GUI control consumer.
  src/gui_resource.cpp
  src/gui_dialog.cpp
  src/gui_menus.cpp
  src/load_screen.cpp
  src/galactic_hud.cpp
  src/rename_prompt.cpp  # [port: Gabriel 2026-11-13] fleet / ship rename prompt
  src/gui_script.cpp
  # P4 completion: in-game screens, HUD panels (encyclopedia, heroes, FoC shells).
  src/gui_screens.cpp
  src/hud_panels.cpp
  # P4 completion 2: holocron / story / planet / buy-units screens, mouse cursor.
  src/gui_info_screens.cpp
  src/mouse_cursor.cpp
  # P2 completion: targeting controller (BeginAbilityTargeting / BeginReinforcement / RadarCameraJump), S9 ability feed.
  src/targeting.cpp
  src/hud_abilities.cpp
)
add_library(alamo::ui_hud ALIAS alamo_ui_hud)
target_link_libraries(alamo_ui_hud PUBLIC alamo_ui alamo_core alamo_engine_flags PRIVATE alamo_data)
if(TARGET alamo_tactical_abilities)  # hud_abilities.hpp abilities_from_s9 (read-only AbilitySystem queries)
  target_link_libraries(alamo_ui_hud PUBLIC alamo_tactical_abilities)
  target_compile_definitions(alamo_ui_hud PUBLIC ALAMO_UI_HAVE_ABILITIES=1)
endif()
if(TARGET alamo_objects)
  target_sources(alamo_ui_hud PRIVATE src/hud_types.cpp)  # build_hud_type_table (GameObjectTypes)
  target_link_libraries(alamo_ui_hud PRIVATE alamo_objects)
endif()
if(TARGET alamo_assets)  # stage 2: shell .alo bones / meshes (hud_shell.hpp)
  target_sources(alamo_ui_hud PRIVATE src/hud_shell.cpp)
  target_link_libraries(alamo_ui_hud PUBLIC alamo_assets)
endif()
if(WIN32)
  target_link_libraries(alamo_ui_hud PRIVATE gdi32)
endif()

add_executable(alamo_ui_hud_tests tests/test_hud.cpp tests/test_hud_stage2.cpp)
target_link_libraries(alamo_ui_hud_tests PRIVATE alamo_ui_hud alamo_data alamo_test_support)
target_compile_definitions(alamo_ui_hud_tests PRIVATE "ALAMO_UI_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
if(TARGET alamo_assets)
  target_compile_definitions(alamo_ui_hud_tests PRIVATE ALAMO_UI_HAVE_ASSETS=1)
endif()
if(TARGET alamo_vfs)  # real-data shell check (MEG archives) when ALAMO_FOC_DATA is set
  target_link_libraries(alamo_ui_hud_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_ui_hud_tests PRIVATE ALAMO_UI_HAVE_VFS=1)
endif()
add_test(NAME alamo_ui_hud_tests COMMAND alamo_ui_hud_tests)
set_tests_properties(alamo_ui_hud_tests PROPERTIES TIMEOUT 120)
# ui3 (UIART "Tactical HUD controls"): ability hotkeys, keyboard edit-box order, command-bar button state, tooltips.
add_executable(alamo_ui_hud_ui3_tests tests/test_hud_ui3.cpp)
target_link_libraries(alamo_ui_hud_ui3_tests PRIVATE alamo_ui_hud alamo_data alamo_test_support)
add_test(NAME alamo_ui_hud_ui3_tests COMMAND alamo_ui_hud_ui3_tests)
set_tests_properties(alamo_ui_hud_ui3_tests PROPERTIES TIMEOUT 120)

# P4: engine/lua -> UI adapters (GUI_* presentation events, story objectives); only when the lua module is configured.
if(TARGET alamo_lua)
  add_library(alamo_ui_lua STATIC src/gui_script_lua.cpp)
  add_library(alamo::ui_lua ALIAS alamo_ui_lua)
  target_link_libraries(alamo_ui_lua PUBLIC alamo_ui_hud PRIVATE alamo_lua alamo_engine_flags)
endif()

# P4 GUI tests (CPU): dialog script / ids / skin parsing, controls, focus, modal routing, menus, load screens, galactic
# HUD, Lua GUI ops; real GUIDialogs.rc / Resource.h / GUIDialogs.xml / campaigns through the VFS with ALAMO_FOC_DATA.
add_executable(alamo_ui_gui_tests tests/test_gui.cpp)
target_link_libraries(alamo_ui_gui_tests PRIVATE alamo_ui_hud alamo_data alamo_test_support)
target_compile_definitions(alamo_ui_gui_tests PRIVATE "ALAMO_UI_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
if(TARGET alamo_vfs)
  target_link_libraries(alamo_ui_gui_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_ui_gui_tests PRIVATE ALAMO_UI_HAVE_VFS=1)
endif()
if(TARGET alamo_ui_lua)
  target_link_libraries(alamo_ui_gui_tests PRIVATE alamo_ui_lua alamo_lua)
  target_compile_definitions(alamo_ui_gui_tests PRIVATE ALAMO_UI_HAVE_LUA=1)
endif()
add_test(NAME alamo_ui_gui_tests COMMAND alamo_ui_gui_tests)
set_tests_properties(alamo_ui_gui_tests PROPERTIES TIMEOUT 120)

# P4 completion tests (CPU): in-game screens, advanced video, HUD panels, camera hotkeys;
# real GUIDialogs.rc / CommandBarComponents through the VFS with ALAMO_FOC_DATA.
add_executable(alamo_ui_screens_tests tests/test_gui_screens.cpp tests/test_gui_screens_a.cpp tests/test_gui_screens_b.cpp tests/test_gui_screens_c.cpp
  tests/test_gui_skirmish_maps.cpp)
target_link_libraries(alamo_ui_screens_tests PRIVATE alamo_ui_hud alamo_data alamo_test_support)
if(TARGET alamo_vfs)
  target_link_libraries(alamo_ui_screens_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_ui_screens_tests PRIVATE ALAMO_UI_HAVE_VFS=1)
endif()
add_test(NAME alamo_ui_screens_tests COMMAND alamo_ui_screens_tests)
set_tests_properties(alamo_ui_screens_tests PROPERTIES TIMEOUT 120)

# p4ui t03 (CPU): colour combos draw item text in the item colour, tech-tree arrows / tints, battle-end arrows, fleet
# summary dividers (docs/re/UI.md §3.7).
add_executable(alamo_ui_colour_arrows_tests tests/test_gui_colour_arrows.cpp)
target_link_libraries(alamo_ui_colour_arrows_tests PRIVATE alamo_ui_hud alamo_data alamo_test_support)
add_test(NAME alamo_ui_colour_arrows_tests COMMAND alamo_ui_colour_arrows_tests)
set_tests_properties(alamo_ui_colour_arrows_tests PROPERTIES TIMEOUT 120)

# P2 HUD, GPU side (only with the renderer): the HUD Vulkan pass after the post chain (hud_renderer.hpp),
# selection circles as 3D primitives (selection_blob.hpp), header-only snapshot bridge (hud_sim_bridge.hpp).
if(TARGET alamo_render AND TARGET alamo_rhi)
  set(_ui_dxc "${ALAMO_DXC}")
  if(NOT EXISTS "${_ui_dxc}")
    find_program(_ui_dxc_found NAMES dxc)
    set(_ui_dxc "${_ui_dxc_found}")
  endif()
  set(_ui_gen ${CMAKE_CURRENT_BINARY_DIR}/generated)
  file(MAKE_DIRECTORY ${_ui_gen})
  set(_ui_shader_headers)
  foreach(spec "hud_vs:VSMain:vs_6_0" "hud_ps:PSMain:ps_6_0")
    string(REPLACE ":" ";" parts "${spec}")
    list(GET parts 0 _name)
    list(GET parts 1 _entry)
    list(GET parts 2 _profile)
    add_custom_command(
      OUTPUT ${_ui_gen}/${_name}.h
      COMMAND "${_ui_dxc}" -nologo -spirv -fvk-use-dx-layout -fspv-target-env=vulkan1.3 -WX -O3
              -T ${_profile} -E ${_entry} -Fh ${_ui_gen}/${_name}.h -Vn g_${_name} "${CMAKE_CURRENT_SOURCE_DIR}/shaders/hud.hlsl"
      DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/hud.hlsl
      COMMENT "DXC ${_name} (hud.hlsl ${_entry} ${_profile})"
      VERBATIM)
    list(APPEND _ui_shader_headers ${_ui_gen}/${_name}.h)
  endforeach()
  add_custom_target(alamo_ui_draw_shaders DEPENDS ${_ui_shader_headers})
  add_library(alamo_ui_draw STATIC src/hud_renderer.cpp src/selection_blob.cpp src/hud_shell_draw.cpp src/hud_model_draw.cpp
              ${_ui_shader_headers})
  add_library(alamo::ui_draw ALIAS alamo_ui_draw)
  add_dependencies(alamo_ui_draw alamo_ui_draw_shaders)
  target_include_directories(alamo_ui_draw PRIVATE ${_ui_gen})
  target_link_libraries(alamo_ui_draw PUBLIC alamo_ui_hud alamo_render alamo_rhi alamo_terrain PRIVATE alamo_engine_flags)

  add_executable(alamo_ui_draw_tests tests/test_hud_draw.cpp)
  # alamo_core first: tools/formats' inline ByteReader vs core's (see alamo_render_app).
  target_link_libraries(alamo_ui_draw_tests PRIVATE alamo_core alamo_ui_draw alamo_engine_flags alamo_render_test_support)
  target_compile_definitions(alamo_ui_draw_tests PRIVATE "ALAMO_UI_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
  if(TARGET alamo_objects)  # hud_sim_bridge.hpp (header-only) needs the object-store headers of sim_snapshot.hpp
    target_link_libraries(alamo_ui_draw_tests PRIVATE alamo_objects)
    target_compile_definitions(alamo_ui_draw_tests PRIVATE ALAMO_UI_HAVE_OBJECTS=1)
  endif()
  if(TARGET alamo_vfs AND TARGET alamo_jobs)  # real-data HUD frames (shell .alo, atlas, text) with ALAMO_FOC_DATA
    target_link_libraries(alamo_ui_draw_tests PRIVATE alamo_vfs alamo_jobs)
    target_compile_definitions(alamo_ui_draw_tests PRIVATE ALAMO_UI_HAVE_VFS=1)
  endif()
  if(COMMAND alamo_shaderxlat_copy_dxc)
    alamo_shaderxlat_copy_dxc(alamo_ui_draw_tests)
  endif()
  add_test(NAME alamo_ui_draw_tests COMMAND alamo_ui_draw_tests)
  set_tests_properties(alamo_ui_draw_tests PROPERTIES TIMEOUT 600)

  # P4 GUI frames on the GPU: main menu, campaign selection, load screen, galactic HUD (synthetic, and real data with
  # ALAMO_FOC_DATA) -> logs/ui_gui_*.png, 0 validation messages.
  add_executable(alamo_ui_gui_draw_tests tests/test_gui_draw.cpp)
  target_link_libraries(alamo_ui_gui_draw_tests PRIVATE alamo_core alamo_ui_draw alamo_data alamo_engine_flags alamo_render_test_support)
  target_compile_definitions(alamo_ui_gui_draw_tests PRIVATE "ALAMO_UI_SOURCE_DIR=\"${PROJECT_SOURCE_DIR}\"")
  if(TARGET alamo_vfs AND TARGET alamo_jobs)
    target_link_libraries(alamo_ui_gui_draw_tests PRIVATE alamo_vfs alamo_jobs)
    target_compile_definitions(alamo_ui_gui_draw_tests PRIVATE ALAMO_UI_HAVE_VFS=1)
  endif()
  if(COMMAND alamo_shaderxlat_copy_dxc)
    alamo_shaderxlat_copy_dxc(alamo_ui_gui_draw_tests)
  endif()
  add_test(NAME alamo_ui_gui_draw_tests COMMAND alamo_ui_gui_draw_tests)
  set_tests_properties(alamo_ui_gui_draw_tests PROPERTIES TIMEOUT 600)
endif()

add_executable(alamo_ui_tests tests/test_ui.cpp)
target_link_libraries(alamo_ui_tests PRIVATE alamo_ui alamo_data alamo_test_support)
add_test(NAME alamo_ui_tests COMMAND alamo_ui_tests)
set_tests_properties(alamo_ui_tests PROPERTIES TIMEOUT 60)

# CameraParams must give the renderer's exact view (render_bridge.hpp); checked when render is configured.
if(TARGET alamo_render)
  add_executable(alamo_ui_render_bridge_tests tests/test_render_bridge.cpp)
  target_link_libraries(alamo_ui_render_bridge_tests PRIVATE alamo_ui alamo_render alamo_test_support)
  if(TARGET alamo_ui_rhi)
    target_link_libraries(alamo_ui_render_bridge_tests PRIVATE alamo_ui_rhi)
  endif()
  add_test(NAME alamo_ui_render_bridge_tests COMMAND alamo_ui_render_bridge_tests)
  set_tests_properties(alamo_ui_render_bridge_tests PROPERTIES TIMEOUT 60)
endif()

# p4ui t04 (CPU, synthetic data): which fleet fills which of the three organiser panes (docs/re/UI.md §7).
add_executable(alamo_ui_organizer_tests tests/test_gui_organizer.cpp)
target_link_libraries(alamo_ui_organizer_tests PRIVATE alamo_ui_hud alamo_data alamo_test_support)
if(TARGET alamo_vfs)
  target_link_libraries(alamo_ui_organizer_tests PRIVATE alamo_vfs)
  target_compile_definitions(alamo_ui_organizer_tests PRIVATE ALAMO_UI_HAVE_VFS=1)
endif()
add_test(NAME alamo_ui_organizer_tests COMMAND alamo_ui_organizer_tests)
set_tests_properties(alamo_ui_organizer_tests PROPERTIES TIMEOUT 60)

# G0-3 - command/event pipeline: typed commands with a canonical binary format, a tick scheduler with a
# deterministic drain order, an event bus, a recorder/replayer and a per-tick batch CRC hook.
# Stage 2: the original's event catalogue (docs/re/COMMANDS.md). The bit-packed network wire codec and the lockstep /
# network scheduler paths were removed in CU7 (2026-11-07): multiplayer is out of scope (docs/DECISIONS.md).
add_library(alamo_commands STATIC
  src/event_types.cpp
  src/command.cpp
  src/fleet_group.cpp  # [port] fleet groups (PortExtension subtype "FG")
  src/events.cpp
  src/scheduler.cpp
  src/recording.cpp
  src/pipeline.cpp
)
add_library(alamo::commands ALIAS alamo_commands)
target_include_directories(alamo_commands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_commands PUBLIC alamo_core)

add_executable(alamo_commands_tests tests/test_commands.cpp)
target_link_libraries(alamo_commands_tests PRIVATE alamo_commands Threads::Threads alamo_test_support)
# Read-only: the existing .acmd streams (headless / sim goldens) must keep loading and re-encoding identically.
target_compile_definitions(alamo_commands_tests PRIVATE "ALAMO_COMMANDS_APPS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../apps\"")
add_test(NAME alamo_commands_tests COMMAND alamo_commands_tests)
# test_drain_order spawns ~3200 yielding threads: 4-10 s alone, past 60 s under a loaded ctest -j 8 (2026-11-01 gate).
set_tests_properties(alamo_commands_tests PROPERTIES TIMEOUT 300)
alamo_label_slow(alamo_commands_tests)

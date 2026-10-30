# G0-3 - command/event pipeline: typed commands with a canonical binary format, a tick scheduler with a
# deterministic drain order, an event bus, a recorder/replayer and a per-tick batch CRC hook.
# Stage 2: the original's event catalogue, bit-packed wire codec and lockstep scheduling (docs/re/COMMANDS.md).
add_library(alamo_commands STATIC
  src/event_types.cpp
  src/command.cpp
  src/wire_codec.cpp
  src/events.cpp
  src/scheduler.cpp
  src/recording.cpp
  src/pipeline.cpp
)
add_library(alamo::commands ALIAS alamo_commands)
target_include_directories(alamo_commands PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_commands PUBLIC alamo_core)

add_executable(alamo_commands_tests tests/test_commands.cpp)
target_link_libraries(alamo_commands_tests PRIVATE alamo_commands Threads::Threads)
# Read-only: the existing .acmd streams (headless / sim goldens) must keep loading and re-encoding identically.
target_compile_definitions(alamo_commands_tests PRIVATE "ALAMO_COMMANDS_APPS_DIR=\"${CMAKE_CURRENT_SOURCE_DIR}/../apps\"")
add_test(NAME alamo_commands_tests COMMAND alamo_commands_tests)
set_tests_properties(alamo_commands_tests PROPERTIES TIMEOUT 60)

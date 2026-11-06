# q6 t03: alamo_game_rules - the skirmish rules both alamo_game (alamo_game_core) and alamo_headless (headless/skirmish_run.cpp)
# use: AI types, default forces, the map's planet type (include/alamo/game/skirmish_rules.hpp). No UI / audio / sim deps.
# Added from engine/apps/CMakeLists.txt before alamo_headless, so both hosts can test for the target.
add_library(alamo_game_rules STATIC skirmish_rules.cpp)
target_include_directories(alamo_game_rules PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(alamo_game_rules PUBLIC alamo_galactic_sim alamo_objects alamo_data alamo_core alamo_engine_flags)
if(TARGET alamo_ai)  # resolve_ai_type (ai::AiDefinitions, ai::campaign_ai_type)
  target_link_libraries(alamo_game_rules PUBLIC alamo_ai)
  target_compile_definitions(alamo_game_rules PUBLIC ALAMO_GAME_RULES_HAVE_AI=1)
endif()

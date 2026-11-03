# S5 / G0 game host library: mode stack (galactic / land / space), main loop + pacing, command
# handlers, map spawning, per-tick CRC. Integrates objects, commands, tactical, terrain, jobs.
set(_alamo_sim OFF)
if(TARGET alamo_objects AND TARGET alamo_commands AND TARGET alamo_tactical AND TARGET alamo_terrain AND TARGET alamo_jobs
   AND TARGET alamo_galactic_sim)
  set(_alamo_sim ON)
  add_library(alamo_sim STATIC
    sim/src/pacing.cpp
    sim/src/game_modes.cpp
    sim/src/sim_host.cpp
    sim/src/sim_host_save.cpp   # N1 save / load (owner: n1-host)
    sim/src/save_session.cpp    # N1 session descriptor + data fingerprints
    sim/src/map_spawn.cpp
    sim/src/scenario.cpp
  )
  add_library(alamo::sim ALIAS alamo_sim)
  target_include_directories(alamo_sim PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/sim/include)
  target_link_libraries(alamo_sim PUBLIC alamo_objects alamo_commands alamo_tactical alamo_terrain alamo_jobs
                                         alamo_galactic_sim alamo_core alamo_engine_flags)
  # S4 story & campaign events: engine/lua's StoryModel + one LuaVm per plot script (lua.h for
  # ServiceRate / LastService). Without engine/lua the session never starts (story_session_none.cpp).
  if(TARGET alamo_lua AND TARGET alamo_lua_vm)
    target_sources(alamo_sim PRIVATE sim/src/story_session.cpp)
    target_link_libraries(alamo_sim PUBLIC alamo_lua PRIVATE alamo_lua_vm)
    target_compile_definitions(alamo_sim PUBLIC ALAMO_SIM_HAVE_STORY=1)
  else()
    target_sources(alamo_sim PRIVATE sim/src/story_session_none.cpp)
  endif()

  # S10-int / R3: sim-owned animation (SimAnimation, T5.anim) when engine/anim is configured.
  if(TARGET alamo_anim)
    target_sources(alamo_sim PRIVATE sim/src/sim_anim.cpp)
    target_link_libraries(alamo_sim PUBLIC alamo_anim)
    target_compile_definitions(alamo_sim PUBLIC ALAMO_SIM_HAVE_ANIM=1)
  endif()

  # S10 battle transitions (docs/re/BATTLE_TRANSITIONS.md): tactical battles of galactic conflicts on
  # MovementSystem (S7) + CombatSystem (S8): battle_catalog / tactical_battle / campaign_battles.
  # S10-int: the battles run combat through the S9 AbilitySystem (abilities required).
  if(TARGET alamo_tactical_movement AND TARGET alamo_tactical_combat AND TARGET alamo_tactical_abilities)
    target_sources(alamo_sim PRIVATE sim/src/battle_catalog.cpp sim/src/tactical_battle.cpp sim/src/campaign_battles.cpp)
    target_link_libraries(alamo_sim PUBLIC alamo_tactical_movement alamo_tactical_combat alamo_tactical_abilities PRIVATE alamo_data)
    target_compile_definitions(alamo_sim PUBLIC ALAMO_SIM_HAVE_BATTLES=1)
    # --- fog of war (owner: fog agent): per-battle FogOfWar (tactical_battle_fog.cpp; no-op stubs without the lib) ---
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_fog.cpp)
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_l1.cpp sim/src/tactical_battle_l1_strikes.cpp)  # L1 systems (owner: tactical-systems agent)
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_pads.cpp)  # Int-final: build pads (L1_S4_REMAINING §3.6)
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_bt.cpp)    # battle host: BT setup / transition rules, pad units
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_skirmish.cpp)  # skai: skirmish map battles (income, victory)
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_oi_abilities.cpp)  # OI abilities: ability host side (owner: abilities agent)
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_save.cpp)  # N1 stage 2 (owner: n2-host): battle save / load
    # Hangars / station upgrade / engine-loop requests (docs/re/STATION_HANGARS_SOUND.md): engine/tactical/hangar.
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_hangar.cpp)
    target_link_libraries(alamo_sim PUBLIC alamo_tactical_hangar)
    # battle host: model data (bounds for the LAND_OBSTACLE stamps, hard-point bones) from the ALO files when engine/assets
    # is configured; without it (or without the model files) the catalog keeps its data-only fallbacks.
    if(TARGET alamo_assets)
      target_link_libraries(alamo_sim PRIVATE alamo_assets)
      target_compile_definitions(alamo_sim PRIVATE ALAMO_SIM_HAVE_ASSETS=1)
    endif()
    if(TARGET alamo_tactical_fog)
      target_link_libraries(alamo_sim PUBLIC alamo_tactical_fog)
      target_compile_definitions(alamo_sim PUBLIC ALAMO_SIM_HAVE_FOG=1)
    endif()
    # --- end fog ---
    # --- S4 (owner: S4 agent): garrisons + structure capture in battles (tactical_battle_garrison.cpp; stubs without the lib) ---
    target_sources(alamo_sim PRIVATE sim/src/tactical_battle_garrison.cpp)
    if(TARGET alamo_tactical_garrison)
      target_link_libraries(alamo_sim PUBLIC alamo_tactical_garrison)
      target_compile_definitions(alamo_sim PUBLIC ALAMO_SIM_HAVE_GARRISON=1)
    endif()
    # --- end S4 ---
    # A1 hooks: DAMAGE_TRACKING store per battle, land campaign-battle credit transfer.
    if(TARGET alamo_ai_tactical)
      target_link_libraries(alamo_sim PUBLIC alamo_ai_tactical)
      target_compile_definitions(alamo_sim PUBLIC ALAMO_SIM_HAVE_AI_TACTICAL=1)
    endif()
    # --- A1 stage 2 (owner: A1 agent): opt-in tactical AI in battles (battle_ai.hpp, campaign_battles_ai.cpp) ---
    if(TARGET alamo_ai_tactical_world)
      target_sources(alamo_sim PRIVATE sim/src/battle_ai.cpp sim/src/campaign_battles_ai.cpp)
      target_link_libraries(alamo_sim PUBLIC alamo_ai_tactical_world)
      target_compile_definitions(alamo_sim PUBLIC ALAMO_SIM_HAVE_TACTICAL_AI=1)
    endif()
    # --- end A1 ---
    add_executable(alamo_sim_battle_tests sim/tests/test_battles.cpp)
    target_link_libraries(alamo_sim_battle_tests PRIVATE alamo_sim alamo_vfs)
    target_compile_definitions(alamo_sim_battle_tests PRIVATE "ALAMO_SIM_TESTDATA=\"${CMAKE_CURRENT_SOURCE_DIR}/sim/testdata\"")
    add_test(NAME alamo_sim_battle_tests COMMAND alamo_sim_battle_tests)
    set_tests_properties(alamo_sim_battle_tests PROPERTIES TIMEOUT 900)
    # --- L1 systems (owner: tactical-systems agent): special weapons / TSW / hyperspace / engines in a campaign battle ---
    add_executable(alamo_sim_l1_tests sim/tests/test_battle_l1.cpp)
    target_link_libraries(alamo_sim_l1_tests PRIVATE alamo_sim alamo_vfs)
    target_compile_definitions(alamo_sim_l1_tests PRIVATE "ALAMO_SIM_TESTDATA=\"${CMAKE_CURRENT_SOURCE_DIR}/sim/testdata\"")
    add_test(NAME alamo_sim_l1_tests COMMAND alamo_sim_l1_tests)
    set_tests_properties(alamo_sim_l1_tests PROPERTIES TIMEOUT 600)
    # --- end L1 systems ---
  endif()

  # Tests: pacing, mode stack (only the top mode ticks), per-tick step order, synthetic map with a
  # recorded command stream at 0/1/4/N workers + checked-in golden (no game data needed), and the
  # real FoC space / land maps when ALAMO_FOC_DATA is set (skipped otherwise).
  add_executable(alamo_sim_tests sim/tests/test_sim.cpp)
  target_link_libraries(alamo_sim_tests PRIVATE alamo_sim alamo_vfs)
  target_compile_definitions(alamo_sim_tests PRIVATE "ALAMO_SIM_TESTDATA=\"${CMAKE_CURRENT_SOURCE_DIR}/sim/testdata\"")
  add_test(NAME alamo_sim_tests COMMAND alamo_sim_tests)
  set_tests_properties(alamo_sim_tests PROPERTIES TIMEOUT 600)
endif()

# Headless host: load game data through the VFS, build a campaign's galaxy, and run the
# deterministic tick with per-AI all-pairs recomputes on the job system. Also the Phase 2 smoke test.
# With alamo_sim: --map NAME --ticks N runs a tactical map through the G0 host.
add_executable(alamo_headless headless/main.cpp)
# alamo_core first: tools/formats defines its own alamo::ByteReader (inline members) that clashes with
# core's out-of-line one when MSVC pulls formats' COMDATs first (LNK2005, an ODR issue outside apps).
target_link_libraries(alamo_headless PRIVATE alamo_core alamo_vfs alamo_data alamo_jobs alamo_galactic alamo_commands
                                             alamo_ai alamo_objects)
if(_alamo_sim)
  target_sources(alamo_headless PRIVATE headless/map_run.cpp headless/battle_run.cpp)
  target_link_libraries(alamo_headless PRIVATE alamo_sim)
  target_compile_definitions(alamo_headless PRIVATE ALAMO_HEADLESS_HAVE_SIM=1)
  # --- A2: --economy --galactic-ai (AI-vs-AI galactic campaign, headless/ai_run.cpp) ---
  target_sources(alamo_headless PRIVATE headless/ai_run.cpp headless/port_trace.cpp)
  if(TARGET alamo_ai_galactic)
    target_link_libraries(alamo_headless PRIVATE alamo_ai_galactic)
    target_compile_definitions(alamo_headless PRIVATE ALAMO_HEADLESS_HAVE_GALACTIC_AI=1)
    # FoC Heart of the Maw, 10 min of galactic time, every faction an AI; skipped (exit 0) without
    # ALAMO_FOC_DATA. Regenerate after an intended change: same command with --ai-log <golden path>.
    add_test(NAME alamo_headless_galactic_ai
             COMMAND alamo_headless --economy --galactic-ai --ticks 18000 --workers 4 --quiet
                     --ai-golden ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/galactic_ai_hotm_golden.txt)
    set_tests_properties(alamo_headless_galactic_ai PROPERTIES TIMEOUT 1200)
    # A2 galactic: the same campaign with the Rebel player a scripted human (fixed recorded command stream
    # headless/testdata/galactic_human_hotm.txt), so the Human-rooted conquest desires run; CRC compared across worker
    # counts (OPT3 t07 worker-count check, one entry per count) and every count's AI log against the golden.
    # Regenerate after an intended change: same command with --workers 0 --ai-log <golden path> (MSVC).
    alamo_add_worker_count_check(alamo_headless_galactic_ai_human
        COMMAND alamo_headless --economy --galactic-ai --ticks 36000 --quiet --human Rebel
                --human-script ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/galactic_human_hotm.txt
                --ai-golden ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/galactic_ai_hotm_human_golden.txt
        TIMEOUT 1800 SLOW)
    # q2f t05: --trace-out (headless/port_trace.hpp), the Q2TRACE 1 state trace read by tools/probe_mod/q2_traces/q2trace.py.
    # 300 HotM ticks across worker counts (the trace changes nothing), then q2trace.py validate (headless/check_port_trace.cmake).
    find_program(ALAMO_PYTHON NAMES python PATHS ${CMAKE_SOURCE_DIR}/toolchain/venv/Scripts NO_DEFAULT_PATH)
    find_program(ALAMO_PYTHON NAMES python python3)
    if(NOT ALAMO_PYTHON)
      set(ALAMO_PYTHON "")
    endif()
    add_test(NAME alamo_headless_galactic_trace
             COMMAND ${CMAKE_COMMAND} -DEXE=$<TARGET_FILE:alamo_headless>
                     -DTRACE=${CMAKE_CURRENT_BINARY_DIR}/q2f_port_trace/hotm_300.q2t -DPY=${ALAMO_PYTHON}
                     -DQ2TRACE=${CMAKE_SOURCE_DIR}/tools/probe_mod/q2_traces/q2trace.py
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/headless/check_port_trace.cmake)
    set_tests_properties(alamo_headless_galactic_trace PROPERTIES TIMEOUT 600)
  endif()
  # --- end A2 ---
  # --- A1 stage 2 (owner: A1 agent): --skirmish-ai synthetic|foc (headless/skirmish_run.cpp). Goldens: regenerate
  # after an intended change with the same command and --crc-out <golden path> (MSVC); clang must match. ---
  target_sources(alamo_headless PRIVATE headless/skirmish_run.cpp)
  if(TARGET alamo_ai_tactical_world)
    add_test(NAME alamo_headless_skirmish_ai_synthetic
             COMMAND alamo_headless --skirmish-ai synthetic --check-workers --quiet
                     --golden ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/skirmish_ai_synthetic_golden.crc)
    # FoC _MP_SPACE_KUAT, Empire vs Rebel; skipped (exit 0) without ALAMO_FOC_DATA.
    add_test(NAME alamo_headless_skirmish_ai_foc
             COMMAND alamo_headless --skirmish-ai foc --workers 4 --quiet
                     --golden ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/skirmish_ai_foc_golden.crc)
    set_tests_properties(alamo_headless_skirmish_ai_synthetic alamo_headless_skirmish_ai_foc PROPERTIES TIMEOUT 1200)
    # skai (q2f t04): the skirmish host on _MP_SPACE_GEONOSIS with an idle human Empire (the retail Geonosis trace's setup): stations
    # from the team markers, both slots' default squadrons, MP_Default_Credits, the per-frame station income; skipped without ALAMO_FOC_DATA.
    add_test(NAME alamo_headless_skirmish_ai_geonosis
             COMMAND alamo_headless --skirmish-ai skirmish --map _MP_SPACE_GEONOSIS --human Empire --seed 1103945700 --ticks 900 --workers 4 --quiet)
    set_tests_properties(alamo_headless_skirmish_ai_geonosis PROPERTIES TIMEOUT 1200)
    alamo_label_slow(alamo_headless_skirmish_ai_geonosis)
    # skai: foc-land is the skirmish host (the battle started on the map, the land default forces, income, skirmish victory, land
    # production) since 2026-11-02; before that a galactic invasion with campaign companies (t50).
    # t50: FoC land SKIRMISH (_MP_LAND_UTAPAU: the explicit Empire / Rebel pads, Player_N_Spawn_Point_Marker, MP_Default_Credits, the
    # pre-built Command Center + Barracks, the skirmish pad lists / prices / pop-cap curve, the shipped skirmish equations), 2 min of battle;
    # skipped (exit 0) without ALAMO_FOC_DATA.
    add_test(NAME alamo_headless_skirmish_ai_foc_land
             COMMAND alamo_headless --skirmish-ai foc-land --ticks 3600 --workers 4 --quiet
                     --golden ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/skirmish_ai_foc_land_golden.crc)
    # The old land invasion (Utapau, _Land_Planet_Utapau_01, a campaign map) under the campaign rules: its golden is the pre-t50 one.
    add_test(NAME alamo_headless_skirmish_ai_foc_land_campaign
             COMMAND alamo_headless --skirmish-ai foc-land-campaign --ticks 3600 --workers 4 --quiet
                     --golden ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata/skirmish_ai_foc_land_campaign_golden.crc)
    set_tests_properties(alamo_headless_skirmish_ai_foc_land alamo_headless_skirmish_ai_foc_land_campaign PROPERTIES TIMEOUT 1200)
    # S10 land outcome, t50: the land skirmish played on (0/1/4/16/N workers identical); skipped (exit 0) without ALAMO_FOC_DATA. What it asserts
    # is structure, not a seed's luck (--expect-skirmish-base): both sides start with their pre-built base (Command Center + Barracks of both
    # slots), the shipped skirmish AI issues orders, shots are fired, 0 Lua errors. Whether the skirmish AI takes the enemy base inside the
    # 15-minute limit is not asserted: it does not (the Destroy_Structure plan reaches the base at ~frame 11000 and fails; open item).
    # OPT3 t08: the worker-count check (alamo_add_worker_count_check, docs/PHASE4.md "Worker-count check"): the 0-worker entry writes
    # the CRC log and checks the base (--expect-skirmish-base), _w1 / _w4 / _w16 / _w<N> compare against it.
    alamo_add_worker_count_check(alamo_headless_skirmish_ai_foc_land_outcome
        COMMAND alamo_headless --skirmish-ai foc-land --ticks 7000 --quiet
        SETUP_ARGS --expect-skirmish-base
        TIMEOUT 1800 SLOW)
    # OPT1: the tick profiler is wall-clock and write-only; a run with --profile must give the same CRCs as the 0-worker run.
    add_test(NAME alamo_headless_skirmish_ai_foc_land_outcome_profiled
             COMMAND alamo_headless --skirmish-ai foc-land --ticks 7000 --workers 4 --quiet --profile
                     --profile-out ${CMAKE_CURRENT_BINARY_DIR}/land_outcome_profile.txt
                     --golden ${alamo_headless_skirmish_ai_foc_land_outcome_CRC})
    set_tests_properties(alamo_headless_skirmish_ai_foc_land_outcome_profiled PROPERTIES TIMEOUT 1800
                         FIXTURES_REQUIRED alamo_headless_skirmish_ai_foc_land_outcome_w0)
    # The campaign-rules land invasion (foc-land-campaign; before skai this was foc-land) must END with a winner before the frame
    # limit. a1g (2026-11-02): after the free-store reservations (§28) and the land Reinforce drop layout (§30.7, TACTICAL_GAPS
    # §4.2.1) no seed 1-12 ended before the 14400-frame cap. a1h t01: with the M1 WeaponBehavior fix, passive / superseded guard
    # statuses (A1_READINGS §33) and non-unit REVEAL sources (FOG_OF_WAR §3.2) seeds 5, 7 and 11 end. a1i t01 (2026-11-02):
    # reserves no longer count (BT §3.1a [H]) and the land AI loads every plan folder (AI_GALACTIC §1.9.1), so SpaceMode\RetreatPlan
    # serves Land_Retreat: seeds 1-12 all end with the Rebel attacker retreating (seed 1 at 4080; A1_READINGS §34). S7/S8 wave 5
    # (2026-11-02): main's worker-count run moved to seed 6 (ends by the Rebel retreat), so seeds 6 and 1 are checked here.
    # a1j t01 / t02 (2026-11-02): with the timed landing unload, the Reinforce in-flight wait and the giant-vehicle push
    # (A1_READINGS §36) seeds 1-12 still end (Rebel retreats, frames 2241-2991).
    # a1j t07: 0 Lua errors too (--fail-on-lua-errors; the galactic-context budget call works in the battle, A1_READINGS §38).
    # One entry per seed, so a change that stalls only some seeds is caught. The worker-count identity of this path is the
    # foc_land_campaign golden run.
    foreach(_seed 6 1)
      add_test(NAME alamo_headless_skirmish_ai_foc_land_campaign_outcome_seed${_seed}
               COMMAND alamo_headless --skirmish-ai foc-land-campaign --seed ${_seed} --ticks 20000 --workers 0 --quiet --expect-battle-end
                       --fail-on-lua-errors)
      set_tests_properties(alamo_headless_skirmish_ai_foc_land_campaign_outcome_seed${_seed} PROPERTIES TIMEOUT 1800)
    endforeach()
    # t41: FoC space, an AI defender with a Level 2 station against a far larger fleet must not retreat; skipped (exit 0) without ALAMO_FOC_DATA.
    # refixf t06/t07 (2026-10-31): the battle must end; seed 3 now kills the station (~frame 10300) and the remaining defenders
    # then retreat (victory all-retreated), which is the expected end after the station is lost. a1h (2026-11-02): with the
    # host-side object follow (A1_READINGS §33.4) and non-unit REVEAL sources (FOG_OF_WAR §3.2) together, seed 3 reaches the
    # cap; seeds 1, 2, 4-7 end. After merging main's S7/S8 work (fighter formation targets, run-time obstacles) on top of
    # a1h, seeds 2-5, 7, 8 of 1-8 end and 1, 6 reach the cap; seed 3 ends again (station lost, defenders all retreat, frame
    # 11967), so the test is back on seed 3. a1i t02 (2026-11-02): seeds 1 and 6 are slow sieges, not stalls (the station
    # launches squadrons without limit; with a 40000-frame cap they end at 16344 / 17340, A1_READINGS §34.5).
    # S8 t10-t12 (2026-11-02): the test runs seed 2 (all destroyed).
    # a1j t03 / t04 (2026-11-02): seed 2 still ends (Rebel, all retreated, 8010); seeds 2 and 4-7 of 1-8 end, 1 / 3 / 8 reach the
    # cap through the space AI's late re-plan at the station (A1_READINGS §37.5; roadmap a1k t01). a1k t01 (2026-11-02,
    # A1_READINGS §39): seeds 1-8 all end (6852-12496); seed 2 ends at 10107 (Rebel, all retreated).
    # OPT2 t12 (2026-11-03, a1k t01 on the OPT2 AI timing): seeds 1-8 all end (9480-13985); seed 2 at 10203 (Rebel, all retreated).
    add_test(NAME alamo_headless_skirmish_ai_foc_station
             COMMAND alamo_headless --skirmish-ai foc-station --seed 2 --ticks 20000 --workers 4 --quiet --expect-battle-end)
    # t48: the default seed reaches QuickReinforce's anti-idle; 0 Lua errors (it may run to the frame limit: half the seeds do, the
    # station holds). refixf t02 (2026-10-31): back on the default seed. It had stacked the rebel ships on the station's centre
    # (3300,0,0) - Project_By_Unit_Range nil, Move_To failed (also seed 4): the station is now a space static entry and a task
    # force's per-unit Lua move orders of one frame are one group move (TacticalBattle::space_obstacles_sync, BattleAi).
    add_test(NAME alamo_headless_skirmish_ai_foc_station_lua
             COMMAND alamo_headless --skirmish-ai foc-station --ticks 20000 --workers 4 --quiet)
    set_tests_properties(alamo_headless_skirmish_ai_foc_station alamo_headless_skirmish_ai_foc_station_lua PROPERTIES TIMEOUT 1800)
    # S7 t09 (2026-11-01): the FoC space skirmish (as alamo_headless_skirmish_ai_foc) must reach a decisive outcome before the
    # 14400-frame limit on every seed. It ran to the limit on ~10 of 16 seeds after S7 t03 (2026-10-29); seeds 1-64 end since
    # refixf t06 (task-force Attack_Move on an object, damage to hardpointed targets). One entry per seed so they run side by side
    # under ctest -j (each ~10 s: "slow" through the regex below); skipped (exit 0) without ALAMO_FOC_DATA. S7 t23
    # (2026-11-02): seeds 9-16 end as well (5000-8300 frames) except 13, left out: its end hangs on the Empire's BurnUnits
    # Attack_Move loop on the neutral / own Skirmish_Merchant_Dock (14400-frame cap before S7 t24, ends after it). S7 t30/t36
    # (2026-11-02, relation table with neutral = 2, merged S8 t17): seeds 1-16 all end (5355-9915 ticks), so 13 is back.
    # a1j t03 / t04 (2026-11-02: own-weapon inaccuracy table, land ground test, model-box target extents, A1_READINGS §37):
    # 14 of 16 end (5724-9163); 6 and 9 are left out: they reach the cap through the space AI's late re-plan after a plan
    # target dies (ships idle for ~4000 frames, §37.5 / §37.7; roadmap a1k t01). Put them back when that is fixed.
    # OPT2 t01 / t02 (2026-11-02, staggered AI planning and background land paths): 6 and 9 end (7180 / 5197 frames) and 2 reaches
    # the cap instead (it ends at 5724 with --no-ai-stagger; the stall is not traced, likely the same a1k t01 re-plan).
    # a1k t01 (2026-11-02): fixed (Collect_All_Free_Units' category filter / reservation / movement lock, the task force's
    # priority set reset when a unit leaves it, A1_READINGS §39): seeds 1-16 all end (5982-12149), so 6 and 9 are back.
    # OPT2 t12 (2026-11-03): seed 2's stall was the same collect rule. On main with OPT2 t10's --no-pp-streams it reaches the cap:
    # from frame 3719 the Empire's BURNUNITS MainForce (Collect_All_Free_Units() with every category) took 10 units, the reserved
    # ones and the members of other plans included, and attack-moved on one point from 4906 on, while every other plan failed its
    # PotentialPlan for want of free units; the Rebels' last frigate was never attacked. With a1k t01 merged on top of OPT2 seeds
    # 1-16 all end with the defaults (5818-9894) and with --no-pp-streams (6122-11047), so seed 2 is back.
    foreach(_seed RANGE 1 16)
      add_test(NAME alamo_headless_skirmish_ai_foc_outcome_s${_seed}
               COMMAND alamo_headless --skirmish-ai foc --seed ${_seed} --ticks 16000 --workers 0 --quiet --expect-battle-end)
      set_tests_properties(alamo_headless_skirmish_ai_foc_outcome_s${_seed} PROPERTIES TIMEOUT 1200)
    endforeach()
  endif()
  # --- end A1 ---
  # --- N1 save / load (owner: n1-host): --save-session / --load / --save-check (headless/save_run.cpp) ---
  # The N1 done-check: save at T, continue to T+K; loading the save in this process, in a fresh process and with
  # replay-from-save must give the original's per-tick CRCs. Chunks whose system write/read has not landed yet are listed
  # in _n1_pending_*: the test then verifies what loaded at the save tick, prints "PENDING: <chunks>" and exits 77
  # (ctest reports it as skipped, not passed). Empty a list (remove the --expect-pending argument) once its chunks load:
  # the check prints "note: --expect-pending lists chunks that load now" when an entry is stale.
  target_sources(alamo_headless PRIVATE headless/save_run.cpp)
  # Nothing pending since n1-lua landed ObjectBridge / LuaPlayerWorld write+read (story.bridge, story.players).
  add_test(NAME alamo_headless_save_synthetic
           COMMAND alamo_headless --save-session synthetic --save-check
                   --save-at 450 ${CMAKE_CURRENT_BINARY_DIR}/n1_save_synthetic.alsv --ticks 750 --workers 4)
  # The same session without the story: nothing pending, the full continue / fresh-process / replay-from-save check.
  add_test(NAME alamo_headless_save_synthetic_nostory
           COMMAND alamo_headless --save-session synthetic-nostory --save-check
                   --save-at 450 ${CMAKE_CURRENT_BINARY_DIR}/n1_save_synthetic_nostory.alsv --ticks 750 --workers 4)
  set_tests_properties(alamo_headless_save_synthetic_nostory PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
  # FoC Heart of the Maw, every faction a galactic AI (as alamo_headless_galactic_ai), save after 1 min of galactic time,
  # 2 min compared; skipped (exit 0) without ALAMO_FOC_DATA. Nothing pending (every chunk loads).
  add_test(NAME alamo_headless_save_hotm
           COMMAND alamo_headless --save-session hotm --save-check
                   --save-at 1800 ${CMAKE_CURRENT_BINARY_DIR}/n1_save_hotm.alsv --ticks 3600 --workers 4 --quiet)
  set_tests_properties(alamo_headless_save_synthetic PROPERTIES TIMEOUT 300 SKIP_RETURN_CODE 77)
  set_tests_properties(alamo_headless_save_hotm PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77)
  # OPT3 t04: saved mid-schedule - at the first tick >= T with a galactic AI path recompute in flight (kicked, applied 15
  # frames later) and the AI players on their staggered planning phases; the loaded runs must match the uninterrupted CRCs.
  add_test(NAME alamo_headless_save_hotm_scheduled
           COMMAND alamo_headless --save-session hotm --save-when scheduled --save-check
                   --save-at 1800 ${CMAKE_CURRENT_BINARY_DIR}/n1_save_hotm_scheduled.alsv --ticks 1800 --workers 4 --quiet)
  set_tests_properties(alamo_headless_save_hotm_scheduled PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77)
  # N1 stage 2 (owner: n2-host): saves during a tactical battle (headless/save_run.hpp "Stage 2 options"). Each saves at
  # the first tick >= T matching --save-when and runs the full check (in-process, fresh-process, both replays, synced RNG).
  # All need ALAMO_FOC_DATA (skipped, exit 0, without it).
  if(TARGET alamo_anim)
    # hotm with the sim-owned animation ("anim" chunk), conflicts auto-resolved.
    add_test(NAME alamo_headless_save_hotm_anim
             COMMAND alamo_headless --save-session hotm --battles auto+anim --save-check
                     --save-at 1800 ${CMAKE_CURRENT_BINARY_DIR}/n2_save_hotm_anim.alsv --ticks 3600 --workers 4 --quiet)
    set_tests_properties(alamo_headless_save_hotm_anim PROPERTIES TIMEOUT 1200 SKIP_RETURN_CODE 77)
  endif()
  if(TARGET alamo_ai_tactical_world)
    # The --skirmish-ai FoC battles (both sides a tactical AI), saved while the battle runs; 1.5 min of AI battle after the load.
    add_test(NAME alamo_headless_save_skirmish_foc
             COMMAND alamo_headless --save-session skirmish --campaign foc --save-when battle --save-check
                     --save-at 600 ${CMAKE_CURRENT_BINARY_DIR}/n2_save_skirmish_foc.alsv --ticks 2400 --workers 4 --quiet)
    add_test(NAME alamo_headless_save_skirmish_foc_land
             COMMAND alamo_headless --save-session skirmish --campaign foc-land --save-when battle --save-check
                     --save-at 900 ${CMAKE_CURRENT_BINARY_DIR}/n2_save_skirmish_foc_land.alsv --ticks 2700 --workers 4 --quiet)
    # hotm campaign with the story on, every conflict fought (tactical AI on all sides): saved during a running battle,
    # and a galactic save taken before the battles start (they start after the load).
    add_test(NAME alamo_headless_save_hotm_battle
             COMMAND alamo_headless --save-session hotm --battles fight-all --story --save-when battle --save-check
                     --save-at 1800 ${CMAKE_CURRENT_BINARY_DIR}/n2_save_hotm_battle.alsv --ticks 900 --workers 4 --quiet)
    add_test(NAME alamo_headless_save_hotm_before_battle
             COMMAND alamo_headless --save-session hotm --battles fight-all --story --save-when galactic --save-check
                     --save-at 600 ${CMAKE_CURRENT_BINARY_DIR}/n2_save_hotm_before_battle.alsv --ticks 1800 --workers 4 --quiet)
    # OPT3 t04: the land skirmish saved with a land path recompute in flight (tactical AI, 15-frame latency).
    add_test(NAME alamo_headless_save_skirmish_foc_land_scheduled
             COMMAND alamo_headless --save-session skirmish --campaign foc-land --save-when scheduled --save-check
                     --save-at 900 ${CMAKE_CURRENT_BINARY_DIR}/n2_save_skirmish_foc_land_scheduled.alsv --ticks 1200 --workers 4 --quiet)
    set_tests_properties(alamo_headless_save_skirmish_foc alamo_headless_save_skirmish_foc_land alamo_headless_save_hotm_battle
                         alamo_headless_save_hotm_before_battle alamo_headless_save_skirmish_foc_land_scheduled
                         PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77)
    if(TARGET alamo_anim)
      # A battle with the animation on (anim worlds of the galaxy and the battle mode).
      add_test(NAME alamo_headless_save_skirmish_anim
               COMMAND alamo_headless --save-session skirmish --campaign foc --battles +anim --save-when battle --save-check
                       --save-at 600 ${CMAKE_CURRENT_BINARY_DIR}/n2_save_skirmish_anim.alsv --ticks 600 --workers 4 --quiet)
      set_tests_properties(alamo_headless_save_skirmish_anim PROPERTIES TIMEOUT 1800 SKIP_RETURN_CODE 77)
    endif()
  endif()
  # --- end N1 ---
endif()

# Self-check on synthetic data (no game files needed): determinism across worker counts
# (final checksum + every tick's CRC record, 0/1/4/N workers).
add_test(NAME alamo_headless_selfcheck COMMAND alamo_headless --selfcheck)

# Q1 replay: a fixed recorded command stream must give identical per-tick CRC sequences for
# 0/1/4/N workers, and match the golden CRC file checked into the repo (generated by MSVC;
# the clang build must reproduce it). Regenerate after an intended sim change:
#   alamo_headless --replay engine/apps/headless/testdata/replay_stream.acmd --crc-out engine/apps/headless/testdata/replay_golden.crc
# alamo_headless_replay_workers* is the OPT3 t07 worker-count check (one process per count, the 0-worker entry also
# against the golden); alamo_headless_replay_golden runs the in-process form (--check-workers, jobs::check_worker_counts).
set(_q1_data ${CMAKE_CURRENT_SOURCE_DIR}/headless/testdata)
alamo_add_worker_count_check(alamo_headless_replay_workers
    COMMAND alamo_headless --replay ${_q1_data}/replay_stream.acmd
    SETUP_ARGS --golden ${_q1_data}/replay_golden.crc
    TIMEOUT 300)
add_test(NAME alamo_headless_replay_golden
         COMMAND alamo_headless --replay ${_q1_data}/replay_stream.acmd --check-workers
                 --crc-out ${CMAKE_CURRENT_BINARY_DIR}/replay_actual.crc --golden ${_q1_data}/replay_golden.crc)
set_tests_properties(alamo_headless_replay_golden PROPERTIES TIMEOUT 300)

# Slow tier (label "slow", see engine/CMakeLists.txt): the long headless runs; ctest -LE slow skips them. The land-outcome
# worker-count tests and the golden checks stay labelled together with the run whose CRC log they need (fixtures).
get_directory_property(_alamo_apps_tests TESTS)
list(FILTER _alamo_apps_tests INCLUDE REGEX "^alamo_headless_(skirmish_ai_foc|galactic_ai_human|save_hotm|save_skirmish)")
alamo_label_slow(${_alamo_apps_tests})

# --- P host (owner: game host agent): alamo_game, the playable game exe (menus/HUD/renderer/sim/audio per frame) ---
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/game/CMakeLists.txt)
  add_subdirectory(game)
endif()
# --- end P host ---

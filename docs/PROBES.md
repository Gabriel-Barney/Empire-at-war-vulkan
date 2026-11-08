# In-game probes: index

Where each in-game probe kit lives, what it asks, and where its findings are recorded (2026-11-06). Rule: a kit's README under
`tools/probe_mod/<kit>/` says **how to run it** (scripts, setup, cleanup); the **findings** go in the spec it names under `docs/re/`
(and the decision, if one follows, in [`DECISIONS.md`](DECISIONS.md)), never only in the kit README. Status of a probe (open or run) is on the
roadmap (`roadmap-store/`). Probes need the original game and Gabriel's go-ahead to launch it (rule 7 in `CLAUDE.md`).

| Kit (`tools/probe_mod/…`) | Question | Findings / spec |
|---|---|---|
| `README.md`, `v2/`, `probeE/` (payloads built by `build_probe.py`, not in git) | File-priority probe v2: how `Data\` paths resolve (MODPATH, loose folders, MEGs, fallback) | [`PHASE2.md`](PHASE2.md) "Data\ resolution order" |
| `a1_tactical/` | A1 t02 free-store timing and A1 t04 landing-point ownership (tactical AI) | `re/AI_TACTICAL.md`, `re/A1_READINGS.md` |
| `a2f_budget/` | a2f t07 / a2g t01: AI budget reset flag +0x74, pre-Init Infrastructure pool and template state at Heart of the Maw start; a2k t05 (`run_a2f_sab.cmd`, short run): the Underworld saboteur, DeploySaboteur stage / results, free-store return (Q5, H1 / H2) | `re/A2F_T07_BUDGET_PROBE.md`, `re/A2G_T01_RESET_FLAG_PROBE.md`, `re/A2K_T08_RE_ANSWERS.md` §5 |
| `a1h_assemble_fleet/` | a1h t09: does `Assemble_Fleet` launch ground units from a planet's surface? | `re/A1_READINGS.md` |
| `cu8g_probes/` | cu8g t07: targeting stickiness spawn default (`run_cu8g_stick.cmd`); t08: missing fallback maps, campaign without a matching map / with a faction-patched MP map (`run_cu8g_maps_miss.cmd`, `run_cu8g_maps_hit.cmd`); t09 (optional): LURE `Is_Pulsing` No and Cancel_Special_Ability restore timing (`run_cu8g_abil.cmd`). All run 2026-11-08: t07 CONFIRMED, t08 answered; t09 rerun as t10 the same day: LURE `Is_Pulsing` No still pulses (CONFIRMED), Cancel_Special_Ability blocks the target's unit ability and restores it within 3 frames of the user's death (retail crashes when the user dies while the target's tractor beam runs); env pick P2 / P3 checked by hand in the same session (P1 needs a debugger, Vader's hum a memory reader: run in cu8h) | `re/CU8F_T04_T06_RE.md` §1.5 / §3.1, `re/CU8F_T03_T05_RE.md` §1.7, `re/CU8F_T01_T02_T07_RE.md` §1, `re/CU8G_T02_ENV_PICK_RE.md` §7 |
| `cu8h_probes/` | cu8h t01: synced draw order of a map load, traced by a debugger (`cu8h_dbg.py`, hardware write watchpoint on the LCG state; `run_cu8h_trace.cmd`) on `_MP_LAND_ENDOR` skirmish and galactic loads (`run_cu8h_galendor.cmd` adds the probe campaign); t02: lightsaber hum / holster timing and Saber_Throw catch, read from memory (`run_cu8h_hum.cmd`, `run_cu8h_luke.cmd`). All run 2026-11-08: the pick order CONFIRMED (plus one Load_Map-tail formation draw `int(1, FPS)`), holster timing CONFIRMED, the hum returns one frame after a catch | `re/CU8G_T02_ENV_PICK_RE.md` §7 / §8, `re/CU8F_T01_T02_T07_RE.md` §2.6 |
| `fixpass_capture/` | Screenshot checklist for the galactic fix pass (no mod files) | `re/FIXPASS_GALACTIC.md` §4–5 |
| `fow_regrow/` | fowf t06: fog regrow window and top-edge circle shift | `re/FOG_OF_WAR.md` §9 item 8 |
| `g1_capture/` | G1 in-game render capture (DXVK, apitrace; RenderDoc is out of scope) | `re/RENDER.md` §7.2 / §7.3 |
| `g1_hops/` | STRATEGIC_GAPS probe G1: ticks per galactic hop, fleet speed multiplier | `re/STRATEGIC_GAPS.md` §2.1 |
| `modc_t09/` | modc t09: play the second workshop mods in `alamo_game` and report what the headless checks cannot see (no mod files) | [`MODS.md`](MODS.md) §9 "Second mod" |
| `modc_loader/` | modc t13 / t14: Lua script with a UTF-8 BOM, XML element names starting with a digit, campaign without planets (run 2026-11-06) | `re/MODC_LOADER.md` |
| `oi_abilities/` | OI abilities: `Destroy_When_Stunned_Over_Water` and clip timings | `re/ABILITIES_HARDPOINTS.md` §4.12 / §4.13 |
| `p1_camera_input/` | P1 camera and input checks (observations, no mod files) | `re/P1_CAMERA_INPUT.md`, `re/P1_DOUBLE_CLICK_MOVE.md` |
| `q6_t15_probes/` | Q6 t15: six retail checks of the Q6 `[port]` rules (EliminationLatch, object-script first pump, Perpetual STORY_ELAPSED, load-time story resume, TR land end time, Select All) | the specs named per question in the kit README (STRATEGIC_GAPS, OBJECT_SCRIPTS, STORY, AI_TACTICAL, COMMANDS) |
| `q2_traces/` | Q2 reference traces: tick-by-tick recordings of the original galactic and space-skirmish runs (also `-Mod tr`) | traces under the kit's `out/`; comparison in `PHASE4_HISTORY.md` and the roadmap |
| `s10_map_owners/` | S10: map owner ids vs battle players (landing / capture-point ownership) | `re/REMAINING_GAPS.md` §3.1, §3.3, §7 |
| `s7s8_probes/` | S7/S8: lob solver, deploy windows, ability blocks, squadrons, escorts | `re/TACTICAL_COMBAT.md`, `re/LOB_SOLVER.md`, `re/MOVEMENT.md` |
| `tsw_planet_destroy/` | L1 systems t04: planet destroy (TSW) frame offsets and story events | `re/TSW_PLANET_DESTROY.md` |

Other kit-like tooling: `tools/shaders/` (shader tools, see its README), `tools/re/` (field tables, locality, Ghidra helpers;
`docs/re/ghidra/README.md`), `tools/visual/` (screenshot diffs, see [`BUILD.md`](BUILD.md)).

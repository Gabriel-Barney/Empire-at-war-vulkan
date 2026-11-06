# In-game probes: index

Where each in-game probe kit lives, what it asks, and where its findings are recorded (2026-11-06). Rule: a kit's README under
`tools/probe_mod/<kit>/` says **how to run it** (scripts, setup, cleanup); the **findings** go in the spec it names under `docs/re/`
(and the decision, if one follows, in [`DECISIONS.md`](DECISIONS.md)), never only in the kit README. Status of a probe (open or run) is on the
roadmap (`roadmap-store/`). Probes need the original game and Gabriel's go-ahead to launch it (rule 7 in `CLAUDE.md`).

| Kit (`tools/probe_mod/…`) | Question | Findings / spec |
|---|---|---|
| `README.md`, `v2/`, `probeE/` | File-priority probe v2: how `Data\` paths resolve (MODPATH, loose folders, MEGs, fallback) | [`PHASE2.md`](PHASE2.md) "Data\ resolution order" |
| `a1_tactical/` | A1 t02 free-store timing and A1 t04 landing-point ownership (tactical AI) | `re/AI_TACTICAL.md`, `re/A1_READINGS.md` |
| `a1h_assemble_fleet/` | a1h t09: does `Assemble_Fleet` launch ground units from a planet's surface? | `re/A1_READINGS.md` |
| `fixpass_capture/` | Screenshot checklist for the galactic fix pass (no mod files) | `re/FIXPASS_GALACTIC.md` §4–5 |
| `fow_regrow/` | fowf t06: fog regrow window and top-edge circle shift | `re/FOG_OF_WAR.md` §9 item 8 |
| `g1_capture/` | G1 in-game render capture (DXVK, apitrace; RenderDoc is out of scope) | `re/RENDER.md` §7.2 / §7.3 |
| `g1_hops/` | STRATEGIC_GAPS probe G1: ticks per galactic hop, fleet speed multiplier | `re/STRATEGIC_GAPS.md` §2.1 |
| `modc_loader/` | modc t13 / t14: Lua script with a UTF-8 BOM, XML element names starting with a digit, campaign without planets (run 2026-11-06) | `re/MODC_LOADER.md` |
| `oi_abilities/` | OI abilities: `Destroy_When_Stunned_Over_Water` and clip timings | `re/ABILITIES_HARDPOINTS.md` §4.12 / §4.13 |
| `p1_camera_input/` | P1 camera and input checks (observations, no mod files) | `re/P1_CAMERA_INPUT.md`, `re/P1_DOUBLE_CLICK_MOVE.md` |
| `q2_traces/` | Q2 reference traces: tick-by-tick recordings of the original galactic and space-skirmish runs (also `-Mod tr`) | traces under the kit's `out/`; comparison in `PHASE4_HISTORY.md` and the roadmap |
| `s10_map_owners/` | S10: map owner ids vs battle players (landing / capture-point ownership) | `re/REMAINING_GAPS.md` §3.1, §3.3, §7 |
| `s7s8_probes/` | S7/S8: lob solver, deploy windows, ability blocks, squadrons, escorts | `re/TACTICAL_COMBAT.md`, `re/LOB_SOLVER.md`, `re/MOVEMENT.md` |
| `tsw_planet_destroy/` | L1 systems t04: planet destroy (TSW) frame offsets and story events | `re/TSW_PLANET_DESTROY.md` |

Other kit-like tooling: `tools/shaders/` (shader tools, see its README), `tools/re/` (field tables, locality, Ghidra helpers;
`docs/re/ghidra/README.md`), `tools/visual/` (screenshot diffs, see [`BUILD.md`](BUILD.md)).

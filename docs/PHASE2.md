# Phase 2 — Architecture design & headless core: results and design notes (2026-10-26)

Phase 2 is finished. Status, open work and the flowchart live only on the main roadmap (`roadmap-store/ROADMAP.html`, `docs/ROADMAP_PROTOCOL.md`); this file keeps the results, decisions, unresolved points and design notes. Code is in `engine\`. Build: `build_msvc.cmd` (logs in `logs\`).

## Done
| Item | Result | Location |
|---|---|---|
| B1 Engine CMake targets | `engine/` added to the top-level build. `alamo_engine_flags` (/W4 /fp:precise; clang `-ffp-contract=off`), module toggle `ALAMO_ENGINE_MODULES`. `alamo_core`: CRC name hash (same as MEG), logger, `FileProvider` + case-insensitive `DirectoryProvider`, `GalaxyMap` SoA tables, `ParallelFor` hook. MSVC 19.51: 7/7 new steps, 0 warnings, 3/3 tests pass (meg, formats, core) | engine/CMakeLists.txt, engine/core |
| B2 VFS | `alamo_vfs` (`Vfs : FileProvider`): loose mod dirs → `corruption\Data` → MEGs (10-level chain: FoC 64Patch > patch2 > Patch > MegaFiles.xml later-listed > … > EaW) → FALLBACKPATH dirs → GameData. Mod MegaFiles.xml / Patch.meg replace base; `parse_command_line` (MODPATH/FALLBACKPATH/STEAMMOD), `resolve()` reports layer, `list(glob)` union sorted, immutable after create (lock-free concurrent reads). Tests replay probe v2 + Probe E E1/E2; clang -Wconversion, TSan, ASan/UBSan clean | engine/vfs |
| B3 XML + loaders | `alamo_data`: tolerant flat-array XML DOM (~170 MB/s; all 627 FoC + 366 EaW files parse, 9 malformed EaW/AI-template files recovered), locale-free value parsing, `GameDatabase::load` (GameConstants, 65/47 planets, 327/209 trade routes, 33/32 campaigns, 3,345 named game objects indexed for later loaders) in 33 ms, `build_galaxy_map()`: 33/33 FoC, 32/32 EaW, 51/51 merged campaigns valid, zero unresolved references. clang -Wconversion, ASan/UBSan + mutation test clean | engine/data |
| B5 Galactic pathing | `alamo_galactic`: shared CSR `PlanetGraph` (routes + free hops, parallel build); re-entrant fleet A* with all six request flags + Edge_Cost rules, per-request `PathScratch` (8 threads × 4k queries identical to serial); `reference_all_pairs` (Floyd–Warshall) and `fast_all_pairs` (reverse Dijkstra + max-intermediate-vertex predecessor chain) — **bit-identical weight + predecessor matrices on 6,000 random graphs (0 mismatches over 10M entries)**; O(L) `get_path`; `AllPairsCache` skips recompute when inputs are unchanged. Bench (2-core box): 400 planets FW 48–64 ms → fast 23 ms (1 thread) / 14 ms (2 threads); slower than FW below ~100 planets → size switch planned | engine/galactic |
| B4 Job system | `alamo_jobs`: per-slot deques with stealing, helping waits (nested `parallel_for` safe), background priority queue, exceptions rethrown deterministically (lowest chunk); `make_parallel_for` backs core's hook; order-stable `parallel_reduce` (bit-identical floats for 0–16 workers) and `parallel_map_then_ordered_commit`; `TaskGraph`; drift-free `SimClock`; `TickScheduler` (period/phase systems); `Deferred<T>` visible exactly at T+k; per-thread `ScratchArena` (pmr). TSan (30+ runs), clang -Wconversion clean. PC (16 threads): 200×Dijkstra kernel 11.35× speedup | engine/jobs |
| B6 Headless host + PC verification | `alamo_headless`: VFS or loose-folder data → GameDatabase → GalaxyMap → PlanetGraph; per-AI all-pairs every 90 frames (vanilla timing: computed and read the same frame, parallel inside), fleet A* each frame via parallel score + ordered commit; prints a checksum of everything the AIs read. **MSVC 19.51: 29/29 steps, 0 warnings, 8/8 tests.** Real install via VFS: all 33 FoC campaigns run (DB load 21 ms). **Checksums identical between Windows/MSVC and Linux/GCC and for 0/1/4 workers.** All-pairs on the 16-thread PC: 200 planets 4.7 → 0.66 ms (7.2×), 400 planets 36.9 → 1.6 ms (23×); plus cache hits skip half the recomputes. Run `bench_msvc.cmd` → `logs\bench.txt` | engine/apps, bench_msvc.cmd |
| RE answers (Phase 2 open items) | 14 questions answered from the decompilation (`docs/re/PHASE2_ANSWERS.md`): tick rate **30 logic Hz** (galactic + tactical; SP fast mode 120); graph nodes = objects with PLANET behaviour + owner ≠ −1 (not In_Background); A* heap is f-only with lazy duplicates; FW uses wrapping unsigned 32-bit sums, diag pred −1; node weight = unsigned trunc(perception)/100, unclamped; Get_Path weights per hop; first AI recompute at creation then staggered; XML: types first-wins, TradeRoute/Campaign last-wins, those two lists parsed in CRC32 order; MODPATH/STEAMMOD interleaved. **Found 3 errors in GALACTIC.md** (fog condition reversed, flags 0/1, "+100 blockade" is a corruption surcharge) | docs/re/PHASE2_ANSWERS.md |
| B8b Data + VFS parity | Q1: `<Behavior>` parsed, `kPlanetHasPlanetBehavior` set (64/65 FoC, 46/47 EaW; Galaxy_Core_Art_Model = IDLE). Q13: every GameObjectType definition kept, lookup returns first; TradeRoute/Campaign last-wins whole replacement (warns "CONTENT DIFFERS" if not identical — vanilla duplicates are identical); GameObjectFiles in list order, TradeRouteFiles/CampaignFiles de-duplicated and parsed in ascending CRC32 of the name as written; `campaigns_by_sort_order()` for UI. Q14: one ordered `mod_search` list (MODPATH + STEAMMOD interleaved, `steam_resolver` callback), already-mounted MEG names skipped, mod Config.meg replaces base (tested). Real-data counts and every campaign's planets/routes unchanged | engine/data, engine/vfs |
| RE doc corrections | GALACTIC.md (15 spots), STRUCTS.md (6), SUBSYSTEMS.md (3) corrected in place, each marked "(corrected 2026-10-26, see PHASE2_ANSWERS Qn)" | docs/re |
| B8a Galactic parity | Q1 node set = PLANET behaviour + owned (In_Background no longer used); Q2 inclusive 3-D free hop, AI z-offset on destination; Q3/Q4 A* reproduces the original heap exactly (1-based, f-only, strict sifts, lazy duplicates, goal on pop, 3N record/heap caps — pool exhaustion returns a status instead of crashing); Q5/Q6 Edge_Cost rewritten (is-human fog, flag 0 unknown/neutral, flag 1 enemy-owned, +100.0f corruption surcharge, LOCAL-player masks marked); Q7/Q8 unclamped unsigned node cost, `(d/HS)*10` hop; Q9 wrapping uint32 FW (row-parallel per k); fast Dijkstra path only when a proven no-wrap bound holds and N ≥ 150, else reference — **0 mismatches over 8,500 graphs / 14.9M entries**; Q10 per-hop Get_Path (cyclic wrapped chains return empty instead of the original's infinite loop); Q11 `cadence.hpp` | engine/galactic |
| B4 review fixes | `ResultCell`/`CommitBuffer` (no `vector<bool>`), seq_cst wake handshake, strong exception safety on submit/spawn/parallel_for/Deferred/TaskGraph, background priority inherited by nested work (sim thread ran 0/400 background chunks), deterministic abort-after-failure, fewer wake-ups, DEADLOCK RULE doc. TSan 45 runs + reviewer stress, clang clean | engine/jobs |
| Headless update | New APIs; Q11 cadence; every PlayerView mask populated; perception stand-ins include negative totals (exercises wrap → reference path); checksum now covers weight **and predecessor** matrices; fixed unspecified argument-evaluation order in A* request draws; `--latency F` runs recomputes as background `Deferred` jobs. Selfcheck: identical checksums for 0/1/4 workers at latency 0 and 30. All 33 FoC campaigns run | engine/apps |
| Independent review | Fresh agent (did not write the code): clang `-Wconversion -Wshadow-all -Wsign-conversion` clean, TSan clean on jobs tests + 300-iteration stress + headless selfcheck. **1 confirmed defect:** `std::vector<bool>` race in `parallel_map_then_ordered_commit`/`parallel_reduce` when the result type is `bool` (reproduced). Plausible: lost wake-up on ARM (x86/MSVC safe), OOM-only exception paths, background-isolation latency leak. Headless gaps: checksum omits predecessor matrices; Deferred/background path and PlayerView masks not exercised | /home/claude review notes → fixes in progress |
| MSVC re-verification (2026-10-26) | `build_msvc.cmd` on the PC: all Phase 2 tests pass, 0 warnings (now 12/12 with the Phase 3 modules); `logs\bench.txt` selfcheck OK (identical checksums 0/1/4 workers, latency 0 and 30), all FoC campaigns run via the real VFS. Phase 3 continues in `docs/PHASE3.md` | build_msvc.cmd |

## Fixes after Phase 2
- 2026-10-26: `alamo_data_tests` crashed when `ALAMO_FOC_DATA` pointed at a real install (XML in MEGs → empty database → `front()` on an empty vector in the test). The test now mounts the VFS (or skips with a reason); the library only gained a diagnostic. Real-install numbers match the table above; headless checksums unchanged. See `docs/PHASE3.md`.

## Decisions
- Custom job system (no EnkiTS) to keep zero third-party deps; parallel work writes per-index, reductions in index order.
- Floats: IEEE single, no FMA contraction, so results match across MSVC/clang and thread counts.
- Graph nodes follow the original (PLANET behaviour + owned); vanilla quirks that can hang or crash the original (cyclic predecessor chains, record-pool exhaustion) return empty/status instead — same result whenever the original terminates.

## Unresolved points and original quirks (notes; open work is tracked on the roadmap)
- Still unresolved by RE: planet-list (+0x560) population order (fixes node indices), perception function names behind 4 tokens, `player+0x48` in the cadence stagger, whether game-mode +0x10 equals the global frame, galactic game-speed scaling.
- Original quirks to keep for MP parity: path visibility uses the LOCAL player's mask (possible vanilla desync source), A* pools capped at 3N, a real 0xFFFFFFFF edge weight reads as unlinked.
- Data assumptions: CRC32 over the list `<File>` text as written (case kept, trimmed); a replacing route/campaign keeps the replaced slot (original's map order unknown); `Variant_Of_Existing_Type` re-pass not ported yet; STEAMMOD shader search paths not handled.
- Data: real ground-slot field (`Max_Ground_Base` not in field table); `Political_Control_Gain` is int in the original but float in `TradeRouteTable`.
- VFS: missing EaW install warns instead of aborting like the original.
- Deferred all-pairs (latency k>0) is lockstep-safe but not vanilla-identical; default stays latency 0 (vanilla), `--latency` for MP-with-new-clients-only. Decide per mode later.
- Fast all-pairs only applies when perception totals keep sums from wrapping; with negative totals (wrap to huge costs) the exact reference runs (400 planets: ~29 ms on 2 cores). Measure real perception ranges in Phase 4; reference FW is row-parallel from 256 planets (threshold to tune with the real job system on the 16-thread PC).
- OOM rollback paths in jobs untested (needs fault-injecting allocator).
- Job overhead with many workers is 0.5–0.7 µs/submit on the PC — fine for ≥10 µs jobs; Chase–Lev/atomic::wait only if profiling demands.
- Headless uses placeholder perception values (deterministic LCG); real perception + Lua come in Phase 4.
- 17 localized-speech/cinematics MEGs listed in MegaFiles.xml are absent on this install (warnings only).

---

# Design notes

### Module layout (`engine/`)

| Module | Library | Depends on | Purpose |
|---|---|---|---|
| core | `alamo_core` | — | CRC name hashing (same as MEG), logger, `FileProvider` interface + `DirectoryProvider`, galaxy DOD tables (`GalaxyMap`), `ParallelFor` hook |
| vfs | `alamo_vfs` | core, `alamo_meg` | Verified Data\ resolution order |
| data | `alamo_data` | core | XML reader, GameConstants/Planets/TradeRoutes/Campaigns loaders → `GalaxyMap` |
| jobs | `alamo_jobs` | core | Thread pool, job graph, `ParallelFor` backend, deterministic tick |
| galactic | `alamo_galactic` | core | Planet graph, A* fleet paths, AI all-pairs |
| apps | `alamo_headless` | all | Headless host: loads a campaign, runs ticks, benchmarks pathing |

Rules: C++20, no third-party deps, clean-room (docs only, never `re_reference/`), `/W4` and
`-Wall -Wextra -Wpedantic` clean, `/fp:precise` / `-ffp-contract=off` for determinism.
`-DALAMO_ENGINE_MODULES="core;data"` configures a subset.

### Determinism contract
- The simulation advances in fixed ticks; every result read by game logic at tick T must be
  bit-identical across machines and thread counts.
- Parallel work writes only to per-index outputs; reductions run in index order.
- Floating point: IEEE single, no FMA contraction, `sqrt` only (correctly rounded).
- Container iteration order never depends on pointer values or hash seeds.

### Data\ resolution order (verified in Phase 1, probe v2 + Probe E)
1. `MODPATH=` dirs, command-line order (earlier wins); includes `STEAMMOD=` items.
2. FoC loose `corruption\Data`.
3. MEGs, newest-mounted first: FoC 64Patch > patch2 > Patch > FoC MegaFiles.xml (later-listed wins) > EaW 64Patch > patch2 > Patch > EaW MegaFiles.xml. A mod's own `Data\MegaFiles.xml` / `Patch*.meg` replace the base ones (they are resolved through steps 1–2).
4. Fallback chain: `FALLBACKPATH=` dirs, then auto-detected EaW `GameData\Data`.
Campaign list: one `CampaignFiles.xml`, first match, no merge (`--merge-campaign-lists` opt-in).
Directory scans are not file lookups (docs/re/UI.md §9.2, 2026-11-02): the original's find walks the FoC loose folder first, then every mounted MEG in mount order, each name once; the mod search list and the fallback folders are not part of it (the skirmish map catalog scans each mod dir on its own). `Vfs::list(glob, layers)` restricts a listing to chosen layers for this.

### Galactic pathing targets (from docs/re/GALACTIC.md)
- Fleet A*: per-request scratch (re-entrant, parallel-safe); edge cost `distance / HS_Speed_Factor`
  (plain distance for free hops ≤ `MaximumFleetMovementDistance`); heuristic `dist / max_speed_factor`.
- AI all-pairs: the original runs Floyd–Warshall per AI every 90 frames (O(N³)). Port keeps a
  reference FW and a sparse per-source Dijkstra run in parallel that must give identical results,
  recomputed only when inputs change while preserving the 90-frame read cadence.


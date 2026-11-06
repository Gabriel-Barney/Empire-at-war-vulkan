# Deferred side goals

Five roadmap side goals were removed from the roadmap on 2026-11-06 (Gabriel: "re-analyse these areas later, when the core game and the mod platform are complete"). Nothing here is cancelled for good, but no group below is scheduled, and agents must not start any of it without a new decision in `docs/DECISIONS.md`.

**Deferred:** shader precompile (SP1-SP6), loading and build speed (LP1, LP4, LP5), CPU efficiency (CP1-CP4, CP6), GPU efficiency (GP1-GP3), graphics fidelity (FX1). **Still on the roadmap:** repeatable benchmark (BM1-BM4), first-person ground unit (FP1-FP3), captain's view (CB1-CB4).

**Restart condition:** the core game (stages "Finish the core systems", "Campaigns and menus", "Parity and mods" and the full QA pass) and the mod platform (Q6 Thrawn's Revenge, MODC) are done. Then re-analyse each area from the figures at that time, starting with the measuring groups listed below, and bring back only what the numbers justify.

The full group definitions are kept below exactly as they stood on the roadmap (task text, estimates and dependencies). The roadmap-store git history also holds them (`roadmap-store/data/items/<id>.json` in commits before 2026-11-06).

## What the 2026-11-06 analysis found (starting points for the re-analysis)

- **CPU (CP1-CP4, CP6).** `docs/PERFORMANCE.md` had the worst tick p99 at 2.2 ms against a 16.7 ms budget, and the 400-planet galaxy at 1.5 ms p99 (OPT1-OPT3 done). CP2, CP3 and CP4 each already say "close this group if the profile shows it is not hot"; nothing suggested it was. CP3 (parallel AI goal scoring) is the riskiest group (determinism). CP1's topology, priority and affinity work could not be tested on other CPU types here; its t08 (7 against 15 workers) is a cheap one-off. **CP6 (sim math ban check) is a cheap guard for determinism rule 3 and is independent of the rest: bring it back first, whatever else happens.**
- **GPU (GP1-GP3).** GPU times are low where measured (TAA about 0.10 ms at 720p, SSAO about 0.4 ms at 1080p) but no worst case was measured (massed explosions, nebulas, water maps, 1440p / 4K). GP1 is the cheap measuring step (about 1 day) that decides GP2 and GP3. FX1's render scale is a better lever for weak GPUs than GP2 / GP3.
- **Shader precompile (SP1-SP6).** The cold cost looked small: 172 ms wall (1795 ms summed pipeline creation) for 38 pipelines on the whole install, 17 track-B programs translated. The real problem to measure is pipeline misses that draw unlit meshes during play (SP1 t03). SP1, SP2, SP3 and SP6 are the core; SP4 (loading screen) is needed only if SP1 shows a long pass; SP5 (mods and settings recompile) can wait for the mod platform.
- **Loading and build speed (LP1, LP4, LP5).** LP1 t01 (measure first-use hitches) decides LP1. LP4 (LTO and PGO) is not needed for speed at this headroom and adds a multi-stage build and a risk to `/fp:precise`. LP5 (regression gate) only pays off after a quiet-PC baseline exists (BM1f t01, t03, t04); timing on a shared PC is noisy.
- **Graphics fidelity (FX1).** HDR tonemap (t01-t04) and render scale (t05-t07) are the useful parts. Shadow maps (t08-t12) are the largest and riskiest part for a visual-only gain.

The benchmark groups (BM1-BM3 done) stay on the roadmap as the measuring tool for this later work.


## Side goal: shader precompile

Compile every shader and pipeline the game can need on first launch, behind a loading screen, so a fresh install or driver update does not start cold or show unlit meshes. Runs alongside the main stages; nothing on the road to playable waits for it.

Done when: From an empty cache, one launch leaves a skirmish and a galactic session with zero pipeline misses and no unlit frames, and later launches skip the pass.


### SP1 Measure and inventory (box SPM; cold cost · pipelines outside the cache)


#### sp1: SP1 Measure cold cost and list every pipeline

Status when deferred: big · preset: sonnet · module: render · apps/game · after: none · effort: ~1 day


Goal (Gabriel, 2026-10-30): precompile all shader data on first launch. The recorded 970 ms for 38 keys is from C3b, before the translator, so the real cost is unknown and decides how much loading screen is needed. Preset Sonnet 5.5 · high: measurement and reading, no behaviour change.


- t01 [todo] Time a fully cold run with both the pipeline and translated-shader caches empty, on the whole FoC install: key count, translation time, pipeline creation wall time, slowest pipeline. Record in docs/PHASE4.md
- t02 [todo] List every pipeline created outside PipelineManager (hud_renderer, taa, gamma, ssao, decorations, particle_gpu, unlit, anything else found) and, for each, when it is created and whether it reads the disk cache
- t03 [todo] Add a run counter for pipeline misses after load: keys requested during play, and frames or draws skipped or drawn unlit while waiting. SP6 uses it as the pass/fail number
- t04 [todo] Baseline that counter on a cold and a warm skirmish, then set a time budget for the pass and record whether a progress screen is needed

### SP2 Full key list (box SPK; sweep in the library · translated keys)


#### sp2: SP2 Build the full key list without playing

Status when deferred: big · preset: high · module: render · materials · render/particles · after: sp1 · effort: ~1–2 days


The --sweep path in alamo_render already derives model keys, but it lives in the app and covers models only. Translated keys depend on program options, and each key includes attachment formats and MSAA class, so the list must be built for the current settings. Preset Opus 5.5 · high: getting coverage complete is the hard part.


- t01 [todo] Move the key collection out of run_sweep (render/app/main.cpp) into the render library; --sweep calls the shared code and its numbers stay comparable
- t02 [todo] Translate effects during collection and emit translated keys, including the fade variants and mesh forms the renderer requests when a model loads
- t03 [todo] Build keys for the current render-pass class (attachment formats + MSAA), not every combination; a settings change is handled in SP5
- t04 [todo] Cover the non-model pipelines SP1 found: particles, terrain, decorations, post effects, HUD. Either list them or route them through the same pass
- t05 [todo] Test: two collections give the same sorted key list, and the list contains every key a scripted scene requests

### SP3 Precompile pass (box SPP; first-launch trigger · resumable)


#### sp3: SP3 First-launch precompile pass

Status when deferred: big · preset: high · module: render · apps/game · after: sp2 · effort: ~1–2 days


Replaces waiting until a key is first drawn. Built into renderer startup rather than a separate tool, since the game has to run it anyway. The async path stays as the fallback for anything missed. Preset Opus 5.5 · high: threading and cache lifetime in the renderer.


- t01 [todo] PipelineManager precompile call: takes a key list, compiles translated keys too instead of leaving them deferred, runs as background jobs, exposes done/total, and can be cancelled
- t02 [todo] Trigger: run when the key file is missing or was just invalidated (programs, driver, device, UUID); skip when the cache already holds the list
- t03 [todo] Save the key list and VkPipelineCache when the pass ends and at checkpoints during it, so a crash or early quit resumes instead of restarting
- t04 [todo] A key that fails to build is logged and does not stop the launch; the existing fallback still draws it
- t05 [todo] Wire the pass into alamo_game startup before the main menu
- t06 [todo] Tests: cold pass then zero misses, second launch compiles nothing, cancel and resume finishes the rest, invalidated cache reruns, 0 validation messages

### SP4 Loading screen (box SPU; progress · quit · rebuild option)


#### sp4: SP4 Shader loading screen

Status when deferred: big · preset: sonnet · module: apps/game · ui · after: sp3 · effort: ~1 day


A long silent first launch looks like a hang. A port feature with no original-game behaviour to match. Preset Sonnet 5.5 · high: UI on top of the SP3 progress counters.


- t01 [todo] Progress screen with a bar and done/total while the pass runs; the window keeps responding throughout
- t02 [todo] Quitting during the pass exits cleanly and keeps the progress saved so far
- t03 [todo] Show the screen only when the pass has real work; a warm launch goes straight to the menu with no flash
- t04 [todo] Options entry to rebuild the shader cache, which clears both caches and reruns the pass on next launch

### SP5 Mods and settings (box SPI; compile only what changed)


#### sp5: SP5 Mods and settings changes

Status when deferred: big · preset: high · module: render · materials · apps/game · after: sp3 · effort: ~1 day


The cache header covers programs, driver and device, but I saw no check for the active mod's files, and a changed MSAA or format setting needs new keys. Only the difference should compile. Feeds Q6 (Thrawn's Revenge) but does not block it. Preset Opus 5.5 · high: invalidation mistakes show up as stale or missing shaders.


- t01 [todo] Fingerprint the active mod's effect files and rescan when it changes; confirm first whether anything already does this
- t02 [todo] Compile only keys missing from the cache after a mod or settings change, with no full rebuild; switching back to earlier settings or mods is instant if their keys are still cached
- t03 [todo] Changing a graphics setting that alters the render-pass class runs the pass for the new class, with the SP4 screen if it is long enough to notice
- t04 [todo] Tests: add a mod effect and only its keys compile; change MSAA and only the new class compiles; remove the mod and nothing stale is used

### SP6 Verify from clean (box SPV; zero misses · Gabriel first-launch run)


#### sp6: SP6 Verify from an empty cache

Status when deferred: big · preset: sonnet · module: render · apps/game · docs · after: sp4, sp5 · effort: ~1 day


The goal is a measured result: no misses in real play. Any key that turns up here goes back into SP2's coverage. Preset Sonnet 5.5 · high: scripted runs and reporting.


- t01 [todo] Scripted run from an empty cache: space and land skirmish plus a galactic session; the SP1 counter must read zero misses and zero unlit frames
- t02 [todo] Add any keys the run finds to SP2 coverage and repeat until clean
- t03 [todo] Record final first-launch time, warm-launch time and cache size against the SP1 baseline in docs/PHASE4.md; update docs/PHASE3.md where the C3b notes change
- t04 [todo] Gabriel: launch a build with an empty cache, play a skirmish and report pass length and any pop-in

## Side goal: loading and build speed

Cut load times and first-use hitches, make release builds faster, and stop later changes from losing those gains. Runs alongside the main stages; nothing on the road to playable waits for it.

Done when: Load times and hitches have before and after numbers in docs/PHASE4.md, the release build uses LTO and PGO, and the performance gate runs in the bench.


### LP1 Per-map asset preload (box LPA; no hitch when a unit first appears)


#### lp1: LP1 Per-map asset preload

Status when deferred: big · preset: high · module: assets · render · apps/game · after: none · effort: ~1–2 days


Goal (Gabriel, 2026-10-30): the shader precompile idea applied to assets. Load what a map used last time on the loading screen, so a new unit does not hitch on first appearance. Presentation only: it changes when assets load, never what the sim reads. Preset Opus 5.5 · high: load threading and first-use behaviour.


- t01 [todo] Measure first-use hitches in a skirmish: which models, textures and sounds load during play, and how long the worst frames take
- t02 [todo] Record the assets each map used into a per-map list in the local cache, merged across sessions
- t03 [todo] Preload the list on the loading screen with worker jobs; a missing or stale entry is skipped and the normal on-demand load stays as the fallback
- t04 [todo] Also seed the list from the map's own starting forces and the faction's units, so a first play of a map benefits; confirm this does not load far more than is used
- t05 [todo] Tests: a second run of a map has no on-demand loads for listed assets, a corrupt list is ignored, and sim CRCs are unchanged
- t06 [todo] Record load time, memory use and worst-frame numbers before and after in docs/PHASE4.md

### LP4 LTO and PGO builds (box LPB; release only · CRC checked)


#### lp4: LP4 LTO and PGO release builds

Status when deferred: big · preset: tools · module: top-level CMake · build scripts · after: opt1, bm3 · effort: ~1 day


Lets the compiler optimize across modules, guided by real play, with almost no code changes. Waits for OPT1's timers and BM3's benchmark scenes, which are the training data. Top-level CMake changes go through the integrating agent. Preset Sonnet 5.5 · medium: build configuration, with the CRC checks as the safety net.


- t01 [todo] Confirm what the release build uses today, then enable LTO in a separate release configuration; everyday builds stay as they are so build times do not grow
- t02 [todo] Add a PGO step to the build script: instrumented build, the BM3 benchmark scenes as training, then the optimized build. Keep it project-local (rule 5)
- t03 [todo] Run the full CRC selfchecks and goldens on the LTO and PGO build, and compare per-tick CRCs and saves with the normal MSVC and clang builds
- t04 [todo] Confirm /fp:precise and no FMA contraction still hold under LTO and PGO (rule 3)
- t05 [todo] Record tick times, frame times, build time and exe size before and after in docs/PHASE4.md, and keep each step only if it pays off

### LP5 Performance gate (box LPG; fail on p99 or load-time regressions)


#### lp5: LP5 Performance regression gate

Status when deferred: next · preset: tools · module: apps/headless · build scripts · after: opt1, bm1 · effort: ~1 day


OPT1 adds profile runs to bench_msvc.cmd but no pass/fail limit, so gains can quietly erode. Timing never feeds game logic or the CRC. Preset Sonnet 5.5 · medium: scripting on top of OPT1's reports.


- t01 [todo] Read the benchmark's JSON reports: p50, p99 and max tick time, plus load time and first-launch time
- t02 [todo] Stored per-machine baselines, since timings do not compare across PCs, updated only by an explicit command
- t03 [todo] Script that repeats each benchmark scenario and compares medians to reduce noise; fails past a set limit and names the scenario and metric, treats a CRC fingerprint mismatch as not comparable, and sets limits above BM3's measured noise
- t04 [todo] Add it to bench_msvc.cmd as a separate step, not to ctest, so timing noise cannot fail ordinary test runs
- t05 [todo] Prove it works by adding a deliberate slowdown and checking that the gate fails; document it in CLAUDE.md's build section via the integrating agent

## Side goal: CPU efficiency

Get more from the CPU without changing results: place work on the right cores, reduce allocator contention, score AI goals in parallel and reduce memory stalls in hot loops. Runs alongside the main stages; nothing on the road to playable waits for it.

Done when: Each group has before and after numbers from the OPT1 profile in docs/PHASE4.md or a recorded reason it was closed, and CRCs stay identical at every worker count and on both compilers.


### CP1 CPU-aware job system (box CPT; P/E cores · cache groups · priorities)


#### cp1: CP1 CPU-aware job system

Status when deferred: big · preset: high · module: jobs · apps/game · after: opt1, cp6 · effort: ~2 days


Goal (Gabriel, 2026-10-30). Workers are logical processors minus one, with no topology or thread-priority code found. Intel reports that reducing background threads fixed a title where E-cores hurt performance. Waits for OPT1's worker-utilisation counter so the change can be measured. Preset Opus 5.5 · high: threading, needs a clang sanitizer run.


- t01 [todo] Detect topology with GetSystemCpuSetInformation or GetLogicalProcessorInformationEx: efficiency class, SMT siblings and last-level cache groups, using the two-call sizing pattern and a safe fallback if detection fails
- t02 [todo] Size the pools from it: frame-critical work on the fastest cores in one cache group, background work on the rest, with headroom for the OS. Prefer soft placement; use hard affinity only if a benchmark justifies it
- t03 [todo] Set priorities for the sim and render threads relative to workers, and mark background workers as low priority; check this cannot starve a job the sim waits on
- t04 [todo] Add a command-line override for worker count and placement so runs can be compared; defaults must work with no user setup
- t05 [todo] Determinism: the worker-count check (OPT3 t07) with placement on and off; clang ASan / UBSan runs
- t06 [todo] Record p99 tick time and utilisation before and after on this PC in docs/PHASE4.md, and note which CPU types were not tested
- t08 [todo] On this PC, compare 7 workers with the current 15 across the OPT1 scenarios to see whether SMT threads help or hurt each workload, and record the result

### CP2 Scratch arena rollout (box CPA; hot temporary allocations)


#### cp2: CP2 Scratch arena rollout

Status when deferred: big · preset: high · module: tactical · ai · objects · after: opt1, cp6 · effort: ~1–2 days


engine/jobs already has a per-thread scratch arena, and a search found no other module using it directly. docs/re/HOTSPOTS.md lists hot allocators in the original, which need checking against the port. Allocation cost in the port is unmeasured, so OPT1 comes first. Preset Opus 5.5 · high: arena pointers must not outlive their scope.


- t01 [todo] Measure allocation count and time per tick by call site, and rank the sites; if allocation is not a meaningful cost, record that and close this group
- t02 [todo] Check the HOTSPOTS list against the port: path smoothing, spatial query results, perception contexts and goal proposal
- t03 [todo] Move the top sites to ScratchScope or pmr containers, only where the data ends inside the scope; never return scratch pointers from a job
- t04 [todo] Add a debug check that catches use after a scope ends, or run the converted paths under clang ASan with the arena's memory poisoned
- t05 [todo] Determinism: the worker-count check (OPT3 t07) and goldens unchanged on MSVC and clang; no iteration order may depend on addresses
- t06 [todo] Record allocations per tick and p99 before and after in docs/PHASE4.md

### CP3 Parallel AI goal scoring (box CPS; score on workers · insert in order)


#### cp3: CP3 Parallel AI goal scoring

Status when deferred: big · preset: xhigh · module: ai · jobs · after: opt1, a1, a2, q2f, cp6, a2k, skai · effort: ~2–3 days


docs/re/HOTSPOTS.md calls goal proposal the main galactic CPU cost and nearly pure, but the port's comments say its plan solve draws random numbers at proposal time, so purity must be proven first. Narrows OPT2 t03, which does not name this pass; work with OPT2's owner. Waits for the AI groups and trace gaps to avoid clashing edits. Preset Opus 5.5 · xhigh: threading and determinism.


- t01 [todo] Confirm from OPT1 that goal proposal is a real cost in the port, and map what each scoring step reads and writes: RNG draws, evaluator caches, and allocations. Close this group if it is not hot
- t02 [todo] Split the pass: scoring without side effects runs on workers with per-index outputs; anything that draws RNG or changes state stays serial and in the original order
- t03 [todo] Make evaluators safe to run at once: per-thread contexts, and per-frame caches that are read-only or per-thread during the pass
- t04 [todo] Serial insert in index order, so tie-breaks and RNG draws are unchanged
- t05 [todo] Tests: the worker-count check (OPT3 t07) and goldens identical to the serial pass on MSVC and clang; save and load mid-pass; clang ASan / UBSan
- t06 [todo] Record galactic p99 and spike numbers before and after in docs/PHASE4.md, and update OPT2 t03 to reflect what this covered

### CP4 Prefetch in hot loops (box CPF; profile-picked · keep only proven gains)


#### cp4: CP4 Prefetch in hot loops

Status when deferred: big · preset: sonnet · module: tactical · objects · after: opt1, cp6 · effort: ~1 day


Factorio gained about 5–13% with little code, but needed many measurements to tune, and prefetching can also hurt. It cannot change results, so the risk is only wasted time. Do this after any data-layout change in OPT3 t06 for the same loop, since layout changes what to fetch. Preset Sonnet 5.5 · high: small, measurement-driven changes.


- t01 [todo] Use OPT1's profile to pick up to three per-unit loops where memory stalls are the likely cost; if none stand out, record that and close this group
- t02 [todo] Add a small prefetch helper for MSVC and clang, with no effect on results and no reads through invalid pointers
- t03 [todo] Tune the look-ahead distance and which fields to fetch per loop, over repeated runs of the OPT1 scenarios
- t04 [todo] Keep a change only if its gain is above run-to-run noise; remove the rest
- t05 [todo] Confirm the worker-count check (OPT3 t07) and goldens are unchanged on MSVC and clang, and record the kept gains in docs/PHASE4.md

### CP6 Sim math ban check (box CPB; no C runtime math in sim folders)


#### cp6: CP6 Sim math ban check

Status when deferred: next · preset: tools · module: build scripts · tests · after: none · effort: ~half a day


Microsoft documents that the C runtime's math functions pick FMA versions at startup, so results can differ between PCs. A search of tactical, ai, galactic, objects and apps/sim source found sim math going through dmath, and this check keeps it that way. Preset Sonnet 5.5 · medium: a small script and test.


- t01 [todo] List every sim folder under rule 3, and every folder or file allowed to use C runtime math (render, ui, audio and terrain's documented render-only helpers)
- t02 [todo] Add a check that fails if sim source calls sin, cos, tan, atan2, pow, exp, log or their f forms outside dmath, naming the file and line. Ignore comments and unrelated names such as a variable called log
- t03 [todo] Fix or explicitly allow anything the first run finds, and record each allowance with its reason
- t04 [todo] Register it with add_test so it runs in both the MSVC and clang builds, and prove it works with a deliberately bad sample

## Side goal: GPU efficiency

Find what the GPU really spends in the heaviest scenes, then cut particle overdraw and water reflection cost where the numbers justify it. Presentation only, so it cannot affect the CRC. Runs alongside the main stages; nothing on the road to playable waits for it.

Done when: Worst-case GPU times are recorded at 1440p and 4K, and each optimization has before and after numbers in docs/PHASE4.md or a recorded reason it was closed.


### GP1 GPU stress benchmark (box GPB; worst scenes · 1440p and 4K)


#### gp1: GP1 GPU stress benchmark

Status when deferred: big · preset: sonnet · module: render · rhi · render/particles · after: bm3 · effort: ~1 day


Goal (Gabriel, 2026-10-30). Recorded GPU times are low but mostly at 1280×720 on fleets and map scenes, with no worst-case figures found for massed explosions or nebulas. This decides whether GP2 and GP3 are worth doing. Preset Sonnet 5.5 · high: timers and repeatable scenes, no rendering change.


- t01 [todo] Add GPU timer scopes for particles, other transparency and water reflection and refraction, alongside the existing world, scene and post scopes
- t02 [todo] Use the BM3 stress scenes and add any worst case they miss: massed explosions, a nebula filling the screen, a large space battle, a large land battle and a water-heavy map
- t03 [todo] Run them at 1280×720, 2560×1440 and 3840×2160 on this PC, with validation off and repeated runs, and report average and worst GPU frame time per scope
- t04 [todo] Try a weaker GPU if one is available; otherwise test with a driver frequency cap as a rough stand-in, and record it as an estimate
- t05 [todo] Set a GPU budget from the results and record in docs/PHASE4.md whether each of GP2 and GP3 should proceed; update their status or close them with the reason

### GP2 Half-resolution particles (box GPP; depth-aware upscale · less overdraw)


#### gp2: GP2 Half-resolution particles

Status when deferred: big · preset: high · module: render · render/particles · ui · after: gp1 · effort: ~2–3 days


GPU Gems 3 describes screen-filling overdraw as almost unbounded and reports large savings from an off-screen pass, and no half-resolution path was found in the particle code. Depends on GP1 showing particles are a real cost. Preset Opus 5.5 · high: draw order and blending with the rest of the frame are easy to get wrong.


- t01 [todo] Map how particles, beams, heat distortion and other transparent effects are ordered and blended today, and decide which can safely move to the smaller pass
- t02 [todo] Render them to a half-resolution target, with quarter resolution as an option, depth-tested against downsampled scene depth
- t03 [todo] Composite with a depth-aware (nearest-depth) upscale so edges against ships and terrain stay clean; fall back to full resolution on edge pixels if blockiness remains
- t04 [todo] Check additive and alpha blending, TAA and bloom against the full-resolution result, and keep draw order correct against effects left at full resolution
- t05 [todo] Add it to Advanced video as an option the player can turn off, and mark it [port: Gabriel]
- t06 [todo] Tests: reference-image comparison within a set tolerance, 0 validation messages, sim CRCs unchanged
- t07 [todo] Record GP1 scene times before and after, including the upscale's own cost, in docs/PHASE4.md, and choose the default from those numbers

### GP3 Cheaper water reflections (box GPW; lower resolution · alternating frames)


#### gp3: GP3 Cheaper water reflections

Status when deferred: big · preset: sonnet · module: render · ui · after: gp1 · effort: ~1 day


The world prepass redraws scene parts for reflection and refraction, but its GPU share here is unmeasured, so GP1 comes first. Preset Sonnet 5.5 · high: contained change with visual checks.


- t01 [todo] Confirm from GP1 how much GPU time reflection and refraction take on water-heavy maps, and record their current target sizes
- t02 [todo] Try lower resolutions for each target and find the lowest with no visible loss in normal play
- t03 [todo] Try updating reflection on alternating frames, and check for visible lag during fast camera moves and interaction with TAA
- t04 [todo] Add the chosen settings to Advanced video or the existing quality levels, with the original look kept available
- t05 [todo] Tests: reference-image comparison within a set tolerance, 0 validation messages, sim CRCs unchanged; record before and after times in docs/PHASE4.md

## Side goal: graphics fidelity

Raise the picture quality beyond the original: HDR with a tonemap, a render scale with a sharpening upscale, and shadow maps. Presentation only, so it cannot affect the CRC. Runs alongside the main stages; nothing on the road to playable waits for it.

Done when: Each option is in Advanced video, can be turned off to the original look, and has a reference-image test, 0 validation messages, unchanged sim CRCs and recorded GPU times in docs/PHASE4.md.


### FX1 Fidelity options (box FX1; HDR tonemap · render scale · shadow maps)


#### fx1: FX1 HDR tonemap, render scale and shadow maps

Status when deferred: big · preset: high · module: render · shaders · ui · game host · after: none · effort: ~8 to 10 days


Gabriel 2026-11-01: the three fidelity options with the biggest visible effect for their cost. Port features beyond the original, each a Video / Advanced video option the player can turn off (docs/PHASE4.md “Graphics and display”). HDR first (render scale and the TAA resolve then work on the same HDR target), shadow maps last as the largest part; none is a hard prerequisite of another. Preset Opus 5.5 · high: renderer work with visual checks, and the shadow-map design needs judgement.


- t01 [todo] HDR: find where the scene is resolved to the back buffer (gamma.hlsl, post chain order), record the scene target format today, and decide where the tonemap goes relative to bloom, heat and the TAA resolve
- t02 [todo] HDR: render scene colour in a 16-bit float target and add a tonemap pass (filmic curve) with an exposure setting, so engines, lasers and explosions stop clipping to flat white; an Original mode keeps today's output
- t03 [todo] HDR: Tonemapping choice (Original / Filmic) and exposure in Advanced video, as RecordOptions fields marked [port: Gabriel]; alamo_render --tonemap
- t04 [todo] HDR: tests: Original is pixel-identical to today's frames, a Filmic reference image within tolerance, 0 validation messages, sim CRCs unchanged; GPU cost recorded in docs/PHASE4.md
- t05 [todo] Render scale: Render scale option (50 to 200%) that sizes the scene targets while the UI and back buffer stay native; check every size-dependent target (SSAO half-res, bloom, heat, fog of war, TAA history) and that mouse picking and screen-to-world stay correct
- t06 [todo] Render scale: our own edge-adaptive upscale plus sharpen pass below 100% and a good downscale filter above 100% (no third-party code, rule 2); TAA jitter and history work at render resolution
- t07 [todo] Render scale: slider in Advanced video; tests: 100% is pixel-identical, picking unchanged, 0 validation messages; GPU times at 720p, 1080p and 1440p scales recorded in docs/PHASE4.md
- t08 [todo] Shadow maps: design note in docs/PHASE4.md: cascade count and resolution, depth format, bias, how alpha-tested casters and the existing instancing / cull path are reused, how it combines with soft shadows, SSAO and TAA; land battles only, as the stencil shadows are
- t09 [todo] Shadow maps: depth-only cascade pass fitted to the camera frustum with texel snapping, recorded through the existing multithreaded draw path
- t10 [todo] Shadow maps: receivers sample the cascades with PCF filtering (soft penumbra), cascade blending and slope-scaled bias, replacing the stencil darken when the mode is on
- t11 [todo] Shadow maps: Shadow quality choice (Stencil / Map low, medium, high) in Advanced video next to Soft shadows; stencil volumes stay as the faithful mode and Shader detail 0 still disables shadows
- t12 [todo] Shadow maps: tests: reference-image comparison within tolerance, 0 validation messages, sim CRCs unchanged; GPU time per quality level recorded in docs/PHASE4.md

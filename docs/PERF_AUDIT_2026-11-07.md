# Performance audit and research — 2026-11-07

Scope: a read-only audit of CPU (sim, loading, build) and GPU (Vulkan renderer) performance, plus web research on techniques
that could help, read from `dev` at `ebcc6522`. Nothing was built, run or changed except this file. No numbers here are new
measurements: "measured" means the number is already in [`PERFORMANCE.md`](PERFORMANCE.md); everything else is an estimate
from reading the code. Each finding gives `file:line`, a fix, effort (S ≤ 1 day, M a few days, L a week or more) and its
determinism risk (rule 3).

**How this fits the roadmap.** Most performance work was deferred on 2026-11-06 (`DECISIONS.md`, "Five side goals deferred";
definitions in [`DEFERRED_SIDE_GOALS.md`](DEFERRED_SIDE_GOALS.md)) until the core game and mod platform are done, because the
budgets are met by a wide margin (p99 2.2 ms against 16.7 ms). This audit does not change that decision. It does three things:
1. lists the few findings that fit work that is still open (OPT3 t12, the BM groups) or that are bugs more than tuning;
2. maps every other finding to the deferred group it belongs to (CP, GP, LP, SP, FX), so the re-analysis starts from code
   references instead of from scratch;
3. records the research, with sources, in the same form as [`OPT_RESEARCH.md`](OPT_RESEARCH.md) (2026-10-30), which it extends.

## Summary

- **The sim is in good shape.** OPT1–OPT3 already took the per-tick cost far below budget. The one remaining budget miss is the
  first battle tick (land 55 ms, space 42 ms, land skirmish 139 ms; measured). The audit found the cause: the sim loads full
  render models (all 12 vertex attributes, 64-byte render vertices) to get collision meshes and skeletons, and parses the same
  ALO file up to four times. A sim-only model load is the main fix (finding C1).
- **The job system waits badly on a busy PC.** `parallel_for` waits for every helper job to exit rather than for the work to
  finish, wakes every idle worker for small forks, and spins with `yield()`. That is the likely reason 15 workers lost badly to
  0 on a loaded PC in BM1 (J1–J3). It needs no golden changes.
- **The renderer is well built** (bindless, async pipelines, a dedicated transfer queue, tight barriers, radix-sorted instanced
  draws, parallel secondaries). Its biggest costs are not in the main draw path but around it:
  - a full GPU drain on every new movie frame;
  - two extra full scene renders for water reflection and refraction on any map with water, even when no water is on screen;
  - the swapchain acquire blocking all offscreen work.
- **The research's biggest risk is already handled.** The Windows C runtime picks FMA3 code paths for `sin`/`cos`/`atan2` on
  newer CPUs, so results can differ between PCs. The port already uses its own `alamo::dmath` (including in the Lua `math` table,
  `engine/lua/src/math_lib.cpp`) and `alamo_sim_math_lint` rejects direct calls in sim modules. Nothing to do.
- **Measuring is the gap.** GPU timestamps run only in benchmark mode, and the renderer's own CPU breakdown (`DrawTiming`) is
  never requested by the game (`app_frame.cpp:78`, `:95`). Without these, the ~3–4 ms of render CPU in the frame profile cannot
  be split up, and the deferred GP1 stress benchmark would have no per-pass numbers.

## Fits work that is open now

| # | Finding | Fix | Effort | Risk | Where it belongs |
|---|---|---|---|---|---|
| C1 | **Sim loads render models.** `battle_catalog.cpp:1057` (also `:963`, `:999`) calls `assets::load_model`: `alo::parse` decodes all vertex attributes (`alo.cpp:127-160`), `build_submesh` packs render vertices (`model.cpp:210`). `avoid_from_asset` / `info_from_asset` only read positions, indices, mesh names / flags / bounds and the skeleton. Measured: ~570 ms thread time at skirmish start. | `alo::parse` options for positions + indices + skeleton; an `assets::build_sim_model` that skips packing; same bounds math so the floats are identical | M | goldens must stay byte-identical on MSVC and clang | OPT3 t12 (open) |
| C2 | **Same ALO parsed up to 4 times**: battle catalog; `SimAnimation::model_for` → `skeleton_from_alo` (`clip.cpp:68-70`, `:341`, a full parse for the skeleton); render `AssetCache::model` (`asset_cache.cpp:82-89`); render `SimAnimDriver` (`sim_anim_driver.cpp:28`) | skeleton-only chunk walk; per-battle cache of file bytes or parsed `AloFile`s shared by catalog and anim | M | none (same data) | OPT3 t12 |
| C3 | **Anim models load lazily inside the tick** (`sim_anim.cpp:281-292`); `discover_clips` (`clip.cpp:303-315`) probes all 119 anim types with `files.exists()` and then reads each hit again: ≥120 VFS misses per model | preload with the battle's `ParallelFor` next to `BattleCatalog::preload`; one prefix listing per model instead of 120 probes | M | low (check that `stats_` is not read by the sim) | OPT3 t12 |
| R1 | **Every new movie frame drains the GPU.** `MovieSlot::update` (`game_movies.cpp:158`) → `HudRenderer::set_image_rgba` (`hud_renderer.cpp:269-285`) creates a new `VkImage` per frame and calls `Uploader::finish`, which with a dedicated transfer queue calls `Context::immediate_submit` → `vkWaitForFences` (`context.cpp:510-533`). The advisor movies loop in the galactic and tactical HUD. | persistent image per page, frames through the existing `set_dynamic_page` in-frame copy | S | none (presentation) | a bug more than tuning; small enough to fix on its own |
| M1 | **No render breakdown in live play.** `Renderer::render` can fill `DrawTiming`, but the app passes `nullptr`; GPU timestamps (`rhi/src/timer.cpp`) only run under `--benchmark` | pass `DrawTiming` into the `--profile` table and the F11 overlay; per-pass timestamps (water, shadows, transparent, particle compute, HUD, final blit) | S | none (wall-clock, write-only) | BM groups / prerequisite of GP1 |

## Renderer findings (GPU efficiency, GP; deferred)

Everything here stays in the renderer or presentation layer. The only thing that feeds back into input is the prepared frame
(`bview_.frame()`: picks, camera focus, audio), noted where it matters.

| # | Finding | Fix | Effort | Est. impact |
|---|---|---|---|---|
| R2 | **Water reflection and refraction run on any map with water.** `world.cpp:760` checks only the map's `water_on_`; `:851-893` then run two more full `build()`s and record them inline on the primary | skip when no water patch is in the frustum; record into secondaries; optionally refresh the reflection every other frame | S / S / M | ~1.3 ms CPU at 2,000 ships (one build is 0.67 ms, measured) plus two scene draws on the GPU |
| R3 | **Acquire blocks all offscreen work.** `app_frame.cpp:307` waits on the acquire semaphore at `ALL_TRANSFER \| COLOR_ATTACHMENT_OUTPUT` for the whole submit, and the acquire (`:25`) happens before any recording | record world + HUD first, acquire late, put the final blit in a second small submit that waits on the acquire at the blit stage | S-M | up to a frame of CPU/GPU overlap with V-sync on |
| R4 | **Per-draw cost of translated `.fx` draws.** `renderer_xlat.cpp:538` copies the full 4 KB `VSConstants` (256 registers) per draw into the write-combined ring (~10 MB/frame at 2,500 draws); descriptor sets allocated and written per run (`renderer_draw.cpp:898`, `:928`) and per pass per frame (`xt_prepare`, ~`:222`) | copy only up to the highest register the plan uses; dynamic-offset sets built once per frame slot; cache texture sets across frames as terrain does | M | 0.3–1 ms CPU |
| R5 | **Too many full-screen copies**: scene → slot target (`renderer_graph.cpp:1022`), gamma copy + pass (`gamma.cpp:115-150`), blit to swapchain (`app_frame.cpp:276-291`); heat adds two more (`:876`, `:888`); bloom without heat still copies SceneTexture | heat straight into DistortionTexture; bloom samples the scene; gamma as the last pass into the swapchain view | S-M | 0.05–0.2 ms at 1080p desktop, more on iGPU / 4K |
| R6 | **Transparent bucket split into many render passes** (`renderer_graph.cpp:830-859`): each particle and mesh range opens its own `vkCmdBeginRendering`, mesh ranges may fork a `parallel_for` | record the interleaved sorted range inline in one rendering scope | S | CPU and GPU (tile flushes on iGPU) |
| R7 | **Fog of war makes a new image per update** (`decorations.cpp:325-345`): VMA allocation + transfer submit + new bindless slot each time | 2–3 persistent images, or staging + copy on the graphics queue | S | removes allocation churn |
| R8 | **Particles run over the whole pool.** `vkCmdDraw(6, d.pool)` (`particle_gpu.cpp:726`) and `CSUpdate` over the full pool (`:597-599`, `:635`), full compute barriers between every packet's clear / update / spawn (`:615-643`) | high-water mark of used slots, or compaction + `DrawIndirect`; batch barriers across packets | M | GPU, unverified; the deferred GP2 (half-res particles) should come after this |
| R9 | **Pipeline variants and first-sight pop-in.** Only 8 dynamic states (`rhi/src/pipeline.cpp:334`); cull mode for mirrored / coverage renders is baked (`renderer_draw.cpp:396-397`), doubling PSOs. Translated keys loaded from disk are not prewarmed (`pipelines.cpp:574-579`), so models are skipped until their pipeline is ready, every session | Vulkan 1.3 core dynamic cull mode / front face / depth / stencil state; prewarm saved keys on workers at map load; `VK_EXT_graphics_pipeline_library` fast-link as the fallback | M | removes pop-in; this is the deferred SP group |
| R10 | **Frame fence wait before CPU-only work.** `begin_frame` (`app_frame.cpp:22`) runs before `bview_.prepare` (`:88`) and the HUD / panel builds | move that work before `begin_frame` | S | overlap; prepare stays after input and `step_sim`, but rerun the land smoke golden |
| R11 | **Uniform ring placement unverified.** 2 × 128 MB `Dynamic` (device-local preferred) (`game_render.cpp:91`, `rhi/src/memory.cpp:21-24`); without ReBAR the BAR heap is ~256 MB, so one slot may land in system RAM | log the memory type chosen; record a high-water mark; size from it | S | consistency between the two frame slots |
| R12 | Minor: TAA velocity RGBA32F (`taa.cpp:161`), separate linear-depth pass (`:566-572`), `unordered_map` insert + erase sweep per instance per frame (`:407-416`); SSAO RG32F targets (`ssao.cpp:91-92`, `:173`); vertex / index buffers rebound every draw (`renderer_draw.cpp:1137-1140`, `:1163-1164`); colour and depth barriers not batched | RG16F targets; skip redundant binds; suballocate model buffers | S each | small (TAA 0.10 ms at 720p, SSAO 0.4 ms at 1080p, measured) |

Already good, no change suggested: CPU frustum and fade culling, terrain patch culling, LOD by screen coverage, instanced runs,
radix-sorted keys, preallocated command buffers, per-frame descriptor pool reset, async pipeline and shader compilation with
disk caches, `VkPipelineCache` persisted with its key list, a dedicated transfer queue on a timeline semaphore, tight barrier
masks. GPU-driven indirect drawing and occlusion culling are not needed at today's ~2,500 draws (an RTS camera has little
occlusion); revisit only with much bigger battles.

## CPU findings: sim (CPU efficiency, CP; deferred)

### Job system (`engine/jobs`)

The likely cause of the BM1 result where 15 workers lost badly to 0 on a loaded PC (mean 12 ms against 1.6 ms) is how forks
wait, not the work itself. All three fixes keep CRCs unchanged by construction (outputs are per index); check at 0 / 1 / 4 / 15
workers as usual. This is the deferred CP1, but J1 and J2 may justify bringing it back early, since BM1f t04 measures exactly this.

| # | Finding | Fix | Effort |
|---|---|---|---|
| J1 | **`parallel_for` waits for helper jobs, not for chunks** (`job_system.cpp:368-384`). `ForState` is on the caller's stack, so `wait(counter)` waits until every helper has exited. A worker that popped a helper and was then descheduled before claiming its first chunk holds up the sim thread for an OS time slice, though it never ran a chunk | count finished chunks and let the caller wait on that; keep `ForState` in a JobSystem-owned refcounted slab (free list, no heap allocation per call) so late helpers can still touch it | M |
| J2 | **Wake-up and spin strategy.** `notify_all` when more than one copy is pushed (`:146`), so a 3-helper fork wakes all 15 idle workers; waiters and idle workers spin 64 × `std::this_thread::yield()` (`:252-255`, `:288-295`), which on Windows gives the sim thread's time slice to other processes; one shared `done_cv_` for every counter | wake min(copies, idle) workers; spin a few µs with `_mm_pause`, then block on the counter itself (C++20 `atomic::wait`, i.e. WaitOnAddress); never `yield` on the sim thread | M |
| J3 | **Forks that always happen, regardless of size**: the CRC capture (`sim_crc.cpp:158`, grain 1, ~18 systems → up to 15 helpers every tick), `gc_schedule`, plan solves; `parallel_for_ref` has no minimum-work guard. Battles keep the job slots only ~4–5% busy (measured). Hot atomics (`fg_queued_`, `bg_*`, `idle_workers_`, `waiters_`) share cache lines; `Slot` is not cache-aligned; a steal locks each slot's mutex even when it is empty (`job_system.hpp:179-188`, `job_system.cpp:51-55`, `:171-180`) | helpers = min(workers, chunks − 1, count / min_items); `alignas(64)`; a per-slot size atomic checked before locking; consider a default of min(hardware threads − 1, 8) or a startup pick (the 400-planet galaxy still gains 3.1× at W15) | S |

### Sim hot paths and battle setup

| # | Finding | Fix | Effort | Risk |
|---|---|---|---|---|
| S1 | **Plan VMs re-parse the PG libraries in every VM** (`ai_lua_runtime.cpp:195-227`): ~250 ms of Lua thread time per battle, measured. `vm_persist` already restores each object's `alamo_serial`, `nblocks` and `GCthreshold`, and `pristine_digest()` is a hash of `write()` (`vm_persist.cpp:1500-1501`, `:1576`) | build each plan's pristine VM once per process and mode, keep its `write()` image, and per battle `read()` the images through the `ParallelFor`. Unlike the rejected shared bytecode cache, the same objects come back with the same serials | M | needs care: gate on digest equality and host-pointer re-attach (the save-load path already does both) |
| S2 | A mid-battle `fresh()` (`ai_lua_runtime.cpp:260-286`) builds a whole VM on the sim thread when a plan is used twice at once (~3 ms each, estimate); evaluator VMs (`Script_<File>`) load lazily inside goal scoring (`:14-29`), a likely part of the 8–11 ms second battle tick | restore from the S1 image or prewarm `pool_count` VMs; build evaluator VMs in the parallel load into a side cache moved into `vms_` on first use, so the saved and hashed sets stay the same | S after S1 | needs care |
| S3 | **Fine_Tune stamp rays repeat work per sample** (`tactical_battle.cpp:79-90`, `space_avoid.cpp:189-204`): per footprint cell, `avoid_model()` takes the cache mutex and does a `std::map` lookup, the placement transform is rebuilt, and each ray scans every triangle of each mesh whose box it hits. Covers the stamps (17 ms) and the serial `bt_place_forces` (~25 ms), both measured | resolve model and placement once per footprint; a per-mesh XY bin grid in model space (conservative; the result is an OR of the same per-triangle tests) | M | none if the per-triangle expressions are unchanged |
| S4 | The battle model cache fills during the battle (`battle_catalog.cpp:1021-1077`), though `GameData::catalog` lives for the whole process (`game_data.cpp:64`) and entries depend only on the files | run `preload()` as a background job at campaign start or on the loading screen for every type in the involved factions' rosters | S | none |
| S5 | **The CRC log grows every tick in `alamo_game`.** `SimHost::step` captures every tick (`sim_host.cpp:604`); `CrcRecorder::capture` (`crc_recorder.hpp:34-39`) rebuilds `parity_names()` (a `vector<string>`) every tick and appends to a log that is never trimmed, so it reallocates more as a session grows | in the game: a ring buffer, or capture only checkpoint ticks and saves (read-only, goldens unchanged); build `parity_parts` once; later, cached sub-digests for big blocks that change rarely (threat grid, `a->paths.hash(h)` at `tactical_ai.cpp:3996`), which changes goldens | S (ring) / M (sub-digests) | none / changes goldens |
| S6 | **String-keyed type lookups per object.** `BattleAi::fill` (`battle_ai.cpp:696-781`) makes ~12 `types.get_bool(ti, "Has_Land_Evaluator")`-style calls per object, each via `normalize_name` (a heap `std::string`, most tags exceed the SSO size) and an `unordered_map<string>` lookup, inside the parallel build, so workers contend on the heap; plus a `std::map` per fill (`:592`) and a linear `containers_` scan per team object (`:664`). Fill is 0.70 ms p50, measured | a per-battle `TypeFacts[type]` table or `static const` field ids | S | none |
| S7 | **Same pattern in perception and goal scoring**: `type_index` scans every type name (`tactical_perception.cpp:328-332`), parameter text re-parsed per evaluation, case-insensitive name compares per object (`:938-1010`); galactic `evaluate()` upper-cases and hashes the equation name and builds a stats key by string concatenation into a `std::map<string>` per call, for every planet in FindTarget / Best_Of (`galactic_ai.cpp:165-183`, `:1788-1795`, `:1913-1930`; tactical `tactical_ai.cpp:353-377`). OPT2 t13 fixed the same pattern elsewhere (land p99 19.7 → 2.6 ms) | resolve `Parameter_Type` lists to type-index bitsets at equation load; equation ids resolved once per loop; stats as counters indexed by (equation, outcome), names formatted only for reports | M / S | none (stats are not hashed) |
| S8 | **Decision logs always on and never trimmed**: `log_decisions = true` by default (`tactical_ai.hpp:346`, `galactic_ai.hpp:87`, `battle_ai.hpp:69`), and some call sites format the string before checking the flag (e.g. `tactical_ai.cpp:2134`) | default off in `alamo_game` (or a ring buffer); format only inside `if (cfg.log_decisions)` | S | needs care: first check that nothing reads the log for game logic (`galactic_ai_tokens.cpp:763-796`, unverified) |
| S9 | Lua 5.0.2 uses the CRT heap (`lmem.c:27-36`); 54–81 plan VMs load in parallel on one process heap, and closing a VM frees objects one at a time | see L5 below (per-`global_State` size-class pool) | M | needs care: light userdata are still hashed by address |
| S10 | `combat_pre` (`tactical_battle.cpp:4314-4351`) does ~10 `lower_bound` searches over large `Unit` structs per unit (`set_flag`, `flags`, `set_position`, ...) and copies the `UnitView` | resolve each unit's index once; a bulk sync call | S | none |

**First battle tick, in short.** Golden-neutral, before the battle on a background job: roster model data (S4), plan-VM images
(S1), evaluator VMs (S2), a per-process PG library read cache. Sim-only: the model load (C1). Cheaper: Fine_Tune rays (S3). The
galactic plan load is still serial and could use the same `ParallelFor`. Spreading spawns or plan loads over several ticks moves
goldens and is a last resort.

## CPU findings: loading, I/O and build (loading and build speed, LP; deferred)

| # | Finding | Fix | Effort | Risk |
|---|---|---|---|---|
| L1 | **Every VFS lookup walks every archive and allocates.** `vfs.cpp:296-331` tries mods, base, then each MEG newest-first; `meg::Archive::find` (`meg.cpp:265-273`) builds an upper-cased copy of the name, CRCs it, then copies each candidate's name again; `normalize_path` (`file_provider.cpp:22-34`) copies twice before that. A miss costs ~2 + 2 × archives allocations | one merged index at mount time (normalized name → winning hit), names stored pre-normalized, CRC computed once | S-M | none; `list()` keeps its sorted output |
| L2 | **Startup XML parsed twice**: `GameDatabase::load_game_objects` (`game_database.cpp:207-214`) and `ObjectTypeDatabase::load` (`object_types.cpp:137-175`) both read every `GameObjectFiles.xml` entry; `HardPointDataFiles` by `game_object_types.cpp:666` and `battle_catalog.cpp:150` | one shared cache of immutable `XmlDocument`s from `load_common` (`game_data.cpp:42`) | S-M | none |
| L3 | **Startup is serial before the first frame.** `game_main.cpp:33` runs `load_game_data` single-threaded; the `JobSystem` is created at `:38`; window, Vulkan device and swapchain (`:42-78`) wait for it | create the job system first; parse the XML lists in parallel into per-file slots, walk them in list order; load data on a worker while the main thread creates the window and device | M | low (walk order unchanged) |
| L4 | **Avoidable copies.** `Vfs::read` → `Archive::read` (`meg.cpp:275-279`) zero-fills a vector and copies out of an archive that is already memory-mapped; `XmlDocument::parse` (`xml.cpp:423`) copies again. `Archive::view()` (`meg.cpp:281`) already returns zero-copy spans, unused by the VFS | `FileProvider::view()` (copy fallback) for ALO / ALA / DDS parsing; an XML parse overload that takes ownership | S | none |
| L5 | **Lua uses the stock allocator** (`lmem.c` `l_realloc` = `realloc`): all plan VMs share one process heap during the parallel plan load (~250 ms Lua thread time, measured). Lua 5.0.2 has no `lua_Alloc`; the hook is the compile-time `l_realloc` / `l_free` macros | per-VM size-class pool through the existing `alamo_lua_patch` mechanism (`engine/lua/CMakeLists.txt:33`); GC counts requested sizes, so GC timing is unchanged | M | none; the deferred CP2 (scratch arenas) is the same family |
| L6 | **No symbols in Release** (`/O2 /Ob2 /DNDEBUG`, no `/Zi` / `/DEBUG`), so WPA, VTune, Superluminal and RGP's CPU side cannot name functions; RelWithDebInfo uses `/Ob1`, which skews profiles | `/Z7` (works with sccache) + `/DEBUG /OPT:REF /OPT:ICF` (set OPT explicitly: `/DEBUG` turns it off) | S | none (same generated code) |
| L7 | A warm shader cache still parses and translates every pass and loads `dxcompiler.dll` (`shaderxlat.cpp:21-30`, `cache.hpp:26-28`, `dxc.cpp:200-201`): the key includes the translated HLSL | an index keyed by (bytecode hash, options, translator version) so a full hit skips translation and the DLL load | S-M | none (presentation) |
| L8 | No segment-heap opt-in (no manifest anywhere); no prefetch of archive pages before the parallel preload | `<heapType>SegmentHeap</heapType>` in an embedded manifest, then benchmark; `PrefetchVirtualMemory` over the entries about to be preloaded (cold cache only). Large pages: no (needs an admin privilege, rule 5) | S | none |

Already good: MEG archives memory-mapped, XML parsed in place with string views, `std::from_chars` for numbers, no
`std::regex`, BC textures uploaded as-is, the plan-VM read cache and the per-file parallel model preload.

### Build flags

| Flag | Now | Recommendation | Determinism |
|---|---|---|---|
| `/O2 /Ob2`, clang `-O3` | CMake Release defaults | keep | — |
| `/fp:precise`, `-ffp-contract=off` | on (`engine/CMakeLists.txt:65`, `:67`) | keep; required | — |
| `/GL` + `/LTCG`, `-flto` | off (LP4 deferred) | a separate release configuration, goldens on both compilers. Reported gains: LTCG ~+10% (informal, from an MSVC developer) | low: inlining changes, FP semantics do not under `/fp:precise` |
| PGO | off (LP4 deferred) | after LTO; the headless replays and BM scenes are a ready training set. Xbox GDK docs: 5–30% | low (same reason) |
| `/arch:AVX2` | off (CP5 dropped 2026-10-30) | keep off: scalar sim, drops pre-2013 CPUs | low, but not worth it |
| `/Zc:inline`, `/Gw` | off | add (smaller objects, faster links) | none |
| `/Z7` + `/DEBUG` | off | add for profiling (L6) | none |
| `/GS`, `/EHsc`, RTTI, static `/MT`, `_ITERATOR_DEBUG_LEVEL=0`, `NDEBUG` | as they are | keep (`/GS`: mods load untrusted data) | none |

## Research

Web research on 12 topics. Evidence quality varies: vendor docs and shipped-engine talks are marked as such, forum numbers as
anecdotal. "Fit" weighs rule 3 (determinism) and rule 2 (no new `engine/` dependency without Gabriel).

### GPU and Vulkan

**Pipeline-compile hitches.** Fit: high (the deferred SP group).
- `VK_EXT_graphics_pipeline_library` pre-builds stage libraries and fast-links them at draw time: about 0.02 ms on NVIDIA,
  0.05–0.11 ms on Mesa AMD ([zmike](https://zmike.github.io/fastlink)). The RX 9070 XT's Windows driver exposes it
  ([Geeks3D capability dump](https://geeks3d.com/20250805/amd-radeon-adrenalin-25-8-graphics-driver)). DXVK uses it to compile
  when shaders load, not at draw ([DXVK](https://github.com/doitsujin/DXVK)).
- `VK_EXT_shader_object`: conformance requires draw CPU cost within 150% of static pipelines; native on NVIDIA, a layer
  elsewhere, no AMD Windows numbers found ([Khronos](https://www.khronos.org/blog/you-can-use-vulkan-without-pipelines-today)).
- Pipeline cache files: own header (hash, size, vendor / device id, driver version), write to a temp file, fall back to empty on
  any mismatch ([zeux](https://zeux.io/2019/07/17/serializing-pipeline-cache/)). Roblox found drivers that kept the UUID across
  incompatible updates ([Roblox](https://about.roblox.com/newsroom/2021/02/creating-a-robust-pipeline-cache-with-vulkan)).
- Shipped practice: Unreal 5.2+ precaches likely PSOs at load and skips drawing an object until its pipeline is ready
  ([Epic](https://www.unrealengine.com/tech-blog/game-engines-and-shader-stuttering-unreal-engines-solution-to-the-problem));
  Fossilize replays recorded state on a quarter of the cores at idle priority ([Fossilize](https://github.com/Themaister/Fossilize)).
- For this engine: the 2006 content is a finite set of `.fx` × vertex formats × states, so the full key list can be built
  offline from the asset catalog (SP2) and precompiled on workers (SP3), with GPL fast-link for anything missed (R9).

**GPU-driven rendering.** Fit: medium, later.
- Assassin's Creed Unity: 1–2 orders of magnitude fewer CPU draws, ~10× instances; GPU culling removed 20–40% of triangles
  ([SIGGRAPH 2015](https://advances.realtimerendering.com/s2015/aaltonenhaar_siggraph2015_combined_final_footer_220dpi.pdf)).
  Rainbow Six Siege: 10,537 draws batched to 412 ([GDC 2016](https://archive.org/download/GDC2016Mansouri/GDC2016-Mansouri.pdf)).
- `VK_EXT_descriptor_buffer` is being replaced by `VK_EXT_descriptor_heap` (June 2026, NVIDIA 610+, no numbers)
  ([NVIDIA](https://developer.nvidia.com/blog/streamlining-resource-binding-with-end-to-end-support-for-vulkan-descriptor-heaps/));
  stay on descriptor indexing.
- For this engine: compute frustum culling feeding draw-indirect-count helps only once battles reach many thousands of draws;
  Hi-Z occlusion is of little use in space.

**Particles and blended effects.** Fit: high.
- Half- or quarter-resolution particles against max-of-4 downsampled depth with a depth-aware upsample: 2–3× faster in
  NVIDIA's sample ([NVIDIA](https://docs.nvidia.com/gameworks/content/gameworkslibrary/graphicssamples/opengl_samples/particleupsamplingsample.htm)).
  That is the deferred GP2.
- Particle trimming (Avalanche): a tight polygon around each sprite's visible pixels leaves 60% of the quad's area with 4
  vertices, 52% with 8 ([Persson](https://www.humus.name/Articles/Persson_GraphicsGemsForGames.pdf)). An offline step with no
  runtime cost; a good first step for nebula and explosion overdraw.
- Additive effects (lasers, glows) need no sorting; weighted blended OIT looks too transparent with dense particles
  ([MJP](https://therealmjp.github.io/posts/weighted-blended-oit)).

**Frame pacing and latency.** Fit: high (extensions, not libraries, so rule 2 does not apply).
- `VK_KHR_present_wait` / `present_id` bound outstanding presents; `VK_EXT_present_timing` (Dec 2025) adds target times
  ([Khronos](https://www.khronos.org/blog/vk-ext-present-timing-the-journey-to-state-of-the-art-frame-pacing-in-vulkan)).
- NVIDIA Reflex on Vulkan is `VK_NV_low_latency2` ([spec](https://docs.vulkan.org/spec/latest/chapters/VK_NV_low_latency2/low_latency2.html));
  AMD Anti-Lag 2 on Vulkan is `VK_AMD_anti_lag` (driver 24.9.1+, RDNA1 and newer) ([GPUOpen](https://gpuopen.com/anti-lag-2/)).
- Measure with PresentMon (MIT, external tool) ([PresentMon](https://github.com/GameTechDev/PresentMon)).
- For this engine: R3 (late acquire) first, then present_wait, then the two vendor extensions behind feature checks.

**Upscaling for weak GPUs.** Fit: medium.
- FSR 2 cost (AMD's README, DX12): 1080p Quality 1.3 ms on an RX 590, 0.6 ms on a 5700 XT, 0.2 ms on a 7900 XTX; MIT, with a
  Vulkan backend ([FSR2](https://github.com/GPUOpen-Effects/FidelityFX-FSR2)). FSR 4.1 on Vulkan ships as SDK DLLs, RX 6000+
  ([FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK)).
- Dynamic resolution heuristics (Unreal, Unity HDRP): GPU-time history against a budget, a "panic" drop after consecutive
  over-budget frames, discrete steps with a minimum hold time
  ([Unreal](https://dev.epicgames.com/documentation/en-us/unreal-engine/dynamic-resolution-in-unreal-engine)).
- For this engine: render scale with TAA upsampling (the deferred FX1 t05–t07) needs no dependency and is the better lever;
  FSR source needs Gabriel's approval and its 1–2 ms on an iGPU may cost more than the 2006 scene itself.

**Async compute and transfer.** Fit: low for compute at 1–5 ms frames; high for transfer (already in place).
- AMD: overlap compute with front-end-bound work (depth, shadows, post) ([RDNA guide](https://gpuopen.com/learn/rdna-performance-guide/)).
  NVIDIA: only long async work that finishes well before it is needed; use the copy engine for uploads
  ([NVIDIA](https://developer.nvidia.com/blog/advanced-api-performance-async-compute-and-overlap)).

**GPU profiling without RenderDoc.** Fit: high (external tools).
- Radeon GPU Profiler 2.7 (Vulkan, RDNA 4 instruction timing) and Radeon Memory Visualizer are the main tools on the RX 9070 XT
  ([RGP](https://gpuopen.com/rgp/)); Nsight Graphics on NVIDIA. Tracy's Vulkan zones need calibrated timestamps; Tracy itself
  still needs approval. In-house per-pass timestamps (M1) cover the overlay.

### CPU

**Build: LTO, PGO, AVX2 and FP reproducibility.** See the build-flags table. The UCRT selects FMA3 versions of its
transcendental functions at startup, so results can differ between PCs and Windows versions
([`_set_FMA3_enable`](https://learn.microsoft.com/en-us/cpp/c-runtime-library/reference/get-fma3-enable-set-fma3-enable));
Box2D v3 replaced `atan2f` with its own for the same reason ([Box2D](https://box2d.org/posts/2024/08/determinism/)). **Already
covered here** by `alamo::dmath` and `alamo_sim_math_lint` (checked: no direct transcendental calls in sim `src` / `include`;
the Lua `math` table is `dmath`-backed). FMA contraction has been off by default in MSVC since VS 2022 17.0
([docs](https://learn.microsoft.com/en-us/cpp/preprocessor/fp-contract)).

**Allocators.** Fit: medium; profile first.
- The default Windows CRT heap contends badly with many threads: an LLVM 36-core link took 38 min 47 s on it against 2 min 22 s
  with mimalloc ([LLVM RFC](https://discourse.llvm.org/t/rfc-replacing-the-default-crt-allocator-on-windows/55795)). That is
  a very allocation-heavy load; a game frame is far lighter.
- A Lua small-block allocator gained under 1% on desktop ([Kalogirou](https://kalogirou.net/2010/11/28/optimizing-script-language-performance-with-custom-memory-allocators/)).
- For this engine: allocator changes cannot change results while rule 3 holds (no pointer-keyed iteration). In-house arenas
  (CP2) and a Lua pool (L5) need no approval; mimalloc does.

**Job systems on hybrid and multi-CCD CPUs.** Fit: high (the deferred CP1).
- Windows QoS: lowering worker priority can move workers onto E-cores; EcoQoS is for background cooking threads only
  ([QoS](https://learn.microsoft.com/en-us/windows/win32/procthread/quality-of-service)). `GetSystemCpuSetInformation` reports
  each core's `EfficiencyClass` ([CPU sets](https://learn.microsoft.com/en-us/windows/win32/procthread/cpu-sets)); Intel's
  HybridDetect sample shows a P/E pool ([HybridDetect](https://www.github.com/GameTechDev/HybridDetect)).
- Dual-CCD X3D chips park the non-cache CCD when Game Bar marks the process as a game
  ([hwbusters](https://hwbusters.com/cpu/amd-ryzen-9-7950x3d-core-parking-problem-solution/)).
- Waiting: a bounded spin with a pause hint, then block ([felixk15](https://felixk15.github.io/posts/comment_on_parallelizing_the_physics_solver)).
- For this engine: results already do not depend on worker count, so the pool can be sized freely (P-cores or one L3 domain,
  minus the main thread). The BM1 loaded-PC run, where 15 workers lost badly to 0, is the case to fix.

**Large-scale simulation.** Fit: medium.
- Box2D v3 is bit-identical across MSVC, clang and GCC with SIMD and threads: own trig, `-ffp-contract=off`, per-thread bit
  arrays merged with OR, a hash checked on every PR ([Box2D](https://box2d.org/posts/2024/08/determinism/)).
- Lane-wise SIMD add / mul / div / sqrt rounds like scalar code; horizontal reductions and FMA do not. Test SIMD against a scalar
  reference. The Recoil engine ships lockstep battles of 10,000–15,000 units
  ([PC Gamer](https://pcgamer.com/games/strategy/over-110-players-and-10-000-units-clash-as-this-free-rts-celebrates-its-growing-multiplayer-scene-with-some-of-the-biggest-multiplayer-battles-ever-fought)).

**Loading.** Fit: low for DirectStorage (small DXT assets; without D3D12 it only loads into plain memory
([docs](https://learn.microsoft.com/en-us/windows/win32/dstorage/dstorage/ns-dstorage-dstorage_queue_desc))). High for what
the audit found: sim-only model loads (C1), no duplicate parses (C2, L2), cooked binary caches of parsed XML and models, parallel
parsing, and pipeline prewarm.

## Suggested order when the deferred side goals come back

1. **Now (open work):** C1–C3, S1–S4 under OPT3 t12; R1 (movie stall) as a small standalone fix; M1 (live render breakdown and
   per-pass timestamps), since every later GPU decision needs it; J1–J2 (job waits), since BM1f t04's loaded-PC run will measure
   them anyway and they need no golden changes.
2. **Cheap measuring first** (as `DEFERRED_SIDE_GOALS.md` already says): GP1 stress benchmark with M1's per-pass numbers;
   BM1f t04 quiet-PC baseline; CP6 is already done in effect (`alamo_sim_math_lint`), so check whether the deferred group can be
   closed.
3. **GPU, by expected payoff:** R2 (visible-water test), R3 (late acquire), R9 (prewarm + dynamic state = SP), R4, R6, R8, then
   GP2 half-res particles and FX1 render scale if GP1 shows heavy scenes are GPU-bound.
4. **CPU:** S5–S8 (string lookups, CRC log, decision logs: cheap, golden-neutral), L1, L2, L3 (startup and VFS), the rest of
   CP1 (topology-aware pool sizing), then L5 / CP2 allocators if a profile shows heap time. LTO + PGO (LP4) last: least needed
   at today's headroom.

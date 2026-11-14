# Performance techniques for OPT1 / OPT2 (research, 2026-10-30)

Web research on techniques for the roadmap's performance goal (box OPT: use the cores better, keep large game /
AI updates off single ticks). Each technique is checked against this engine's constraints: 30 ticks/s (33 ms),
rule 3 determinism (identical results for any worker count, run and compiler), Lua 5.0.2 with original semantics,
Windows x64, no new `engine/` dependencies without Gabriel. Sources are at the end; most are developer blogs,
forum threads and talk abstracts, not benchmarks, so OPT1's own profile decides what is worth doing.

## Where the engine stands (from the code, 2026-10-30)
- `engine/jobs` scales well: 7.9x at 8 workers and 12x at 15 on Dijkstra, 16.9x on a 16M float reduce
  (`logs/bench.txt`, 16 logical processors), with order-stable results.
- Parallel today: tactical movement, combat, spatial tree, tree sets, AI path finder, galactic perception,
  zone reachability, galactic all-pairs / planet graph, anim. Serial: all of `lua` and `objects`, most of the
  tactical and galactic AI, story.
- No per-system tick timing anywhere in the sim.
- Galactic all-pairs already has a deferred mode (`--latency k`): computed in the background and applied k ticks
  later, lockstep-safe. It stayed off by default only because it is not vanilla-identical, which no longer
  matters now that parity is checked by behaviour (2026-10-29).

## Techniques, ranked by fit

### 1. Measure before changing anything (OPT1)
- **Frame profiler with zones.** Tracy (BSD-3, C/C++/Lua zones, lock and context-switch views, off unless
  `TRACY_ENABLE`) is what many engines use. It is a third-party dependency, so it needs Gabriel's approval
  under rule 2. The alternative is an in-house scoped timer per system writing per-tick totals into a ring buffer.
  Recommendation: build the in-house timers (OPT1 t01 to t03 need them anyway for the headless report); ask
  Gabriel about Tracy as an optional, off-by-default developer build for the timeline view.
- Timers must never feed game logic or the CRC (wall-clock time differs per run).

### 2. Time slicing by tick number: stagger AI thinking (OPT2 t01)
- Standard practice: don't run every agent's decision pass on the same frame. Round-robin a fixed number of
  agents per frame, and run decisions far less often than movement (RTS AI often every ~10 frames).
- The cost is latency: Allen Chou's numbers show main-thread time falling as slices shrink, while update
  latency grows from 1 to 10 frames. For an AI that thinks in seconds, a few ticks of latency is invisible.
- **Deterministic form for this engine:** player p runs its planning on ticks where `tick % N == p % N`; a task
  force or unit list is walked in fixed index slices per tick. The schedule depends only on the tick number and
  stable ids, never on measured time. This keeps the CRC identical across worker counts and compilers, and saves
  must store the slice cursor (N1 chunk).
- Do not use a time-based budget ("run until 2 ms are used") for sim work: it is the common game technique but
  it breaks determinism. Time budgets are only acceptable for non-sim work (rendering, asset streaming).

### 3. Background jobs with a fixed latency (OPT2 t02)
- Forum experience warns that threaded pathfinding is hard to keep deterministic. The safe pattern, already in
  the engine for all-pairs: take an immutable snapshot on tick N, compute on background workers, and apply the
  result on exactly tick N+k whether or not the job finished early (wait if it is late). Results then don't depend on
  thread timing.
- Candidates: galactic all-pairs (make `--latency` the default), AI perception refresh, task-force planning,
  tactical zone reachability, long A* requests. Save files must hold in-flight jobs' inputs, or the rule
  "no job in flight across a save point".

### 4. Read-old / write-new per pass, commit in index order (OPT2 t03)
- The common way to parallelise entity updates deterministically: each entity reads a frozen snapshot of last
  tick's state and writes only its own output or a per-index message list; cross-entity effects (damage,
  orders, events) are queued and applied afterwards in a fixed order. Double buffering alone is not enough;
  the fixed-order commit is what makes it deterministic. Knockout City's entity system (GDC 2022) is a shipped
  example of deterministic parallel entity updates.
- Run independent systems in parallel with each other when their data does not overlap, like ECS schedulers
  (Bevy) do: declared read/write sets, conflicting systems ordered explicitly, deferred command buffers applied
  at sync points. Ambiguous ordering between conflicting systems is exactly the nondeterminism to avoid.
- Factorio's experience matches: the systems it threaded were the isolated ones (electric networks, fluids,
  heat), and it shipped a desync where a result depended on the core count. Hence the OPT2 t06 rule: every
  change is checked at 0 / 1 / 4 / 16 workers.

### 5. Lua garbage collection spikes (OPT2 t04)
- Lua 5.0 has a non-incremental mark-and-sweep collector (the incremental one came in 5.1): when the
  allocation count passes `GCthreshold` it collects the whole state in one go, which shows up as a spike.
  `lua_setgcthreshold(L, 0)` forces a full collection; `INT_MAX` stops it.
- Game practice: keep GC under ~1 to 2 ms per frame, by time-budgeted incremental steps (Lua 5.1+, e.g. the
  Kalogirou patch) or by running GC in parallel with other frame work (OpenMW). Neither fits directly:
  porting 5.1's incremental collector into 5.0.2 changes original semantics (weak tables, finalizer timing),
  and time budgets break determinism.
- **Fits this engine:** each `lua_State` is independent, so (a) **stagger forced collections**: VM i runs a
  full collect on its own tick offset, with a size check, so no two big VMs collect on the same tick and no VM
  reaches a surprise threshold collection; (b) **collect VMs on workers**: separate states can be collected
  in parallel while nothing else touches them (e.g. at the end of the tick), because Lua 5.0 states share no
  data. Both depend only on the tick number and allocation counts, which N1 already saves.
- Running separate AI players' VMs in parallel is possible for the same reason, but only if every C API call
  they make reads the sim's frozen state and queues commands (technique 4); any call that writes the sim
  directly has to move behind the command bus first.
- **Done (OPT2 t04, 2026-11-02):** (a) and (b) as above, with the details and numbers in `docs/PERFORMANCE.md`
  ("Lua GC schedule"). One change to (a): a VM's slot comes from a hash of its own GC threshold, not from a host
  index, so it is saved with the VM and needs no new save data.

#### Investigation: AI players' Lua VMs in parallel (OPT2 t04, 2026-11-02)
Not implemented; left as OPT2 t08. The VMs themselves would allow it (each plan, free store and story script is
its own `lua_State`, see above), but what the scripts call does not:
- **Writes through the AI host, not only reads.** The AI binding families (`bind_ai.cpp`, `bind_ai_tactical.cpp`,
  `lua/ai_bridge.hpp` `AiScriptHost`) mix queries (`tf_valid`, `target_distance`, `free_store_count`) with calls
  that change the AI world in place: `tf_produce_force` (build tasks, budget spend), `tf_order` / `free_store_move`
  (status records plus orders), `tf_release_*`, `budget_op`, `set_global_value`, `find_target` (creates target
  records). Those maps (`plans`, `tfs`, `statuses`, `pps`, `builds`) and their id counters (`next_plan`, `next_tf`,
  ...) are shared by every player of a world, so ids and container order depend on which player ran first.
- **Orders execute at once.** Object and task-force orders go through `CommandPipeline::execute_direct`, so the
  next player's scripts already see this player's orders in the same tick. A queue applied after all players
  changes that ordering (a small AI timing shift, which is allowed), but every binding that reads back its own
  order (`Is_Moving`, status polling, `BlockOnCommand`) needs a defined answer for "queued, not applied".
- **One synced RNG stream.** `GameRandom` and the plan solver draw from the session's synchronized stream in
  player order; parallel players need per-player streams (a behaviour change, and the draw order of the original
  is one stream).
- **Shared VMs and caches.** `Script_<File>` evaluators are ONE VM per file shared by all players of a world
  (`ScriptEvaluators`), perception caches fill lazily on read, the plan VM pools (`PlanScriptRuntime::acquire`)
  and the decision / error logs are shared.
- **Little to win today.** Per-player AI time in the OPT1 galactic run (Heart of the Maw, every faction an AI):
  `T7.p0` p50 0.10 ms, p99 0.81 ms; `T7.p1` 0.07 / 0.76 ms; the other players far less. In battles the tactical
  AI runs at the end of `T5.battles` and is not split out by the profiler yet (the part of `T5.battles` outside
  the `tac.*` steps, 1.2 ms p50 on land in the OPT1 baseline, is an upper bound). So the galactic best case is
  well under 1 ms per tick; a battle zone for the AI is needed before judging the tactical side.
- **What it would take:** a frozen per-tick sim view for the read bindings, per-player id spaces (or ids assigned
  at commit in player order), a per-player command / AI-mutation queue committed in player order after the
  parallel phase, per-player RNG streams or pre-drawn numbers, per-player evaluator VMs, and a read-only
  perception cache filled before the phase. That is several days of work across `engine/ai`, `engine/lua` and the
  hosts, and it changes AI behaviour; worth it only if bigger scenes make `T7` a real cost.

### 6. Data layout for hot loops (OPT2 t03, where the profile points)
- Data-oriented design: hot per-unit data in contiguous arrays (structure of arrays), no linked lists or
  hash maps in inner loops; cache misses usually dominate. Uniform grids beat trees for unit neighbour and
  target queries when units are similar in size (one hobby RTS project chose a grid over a quadtree for 50k
  units). The port already has a spatial tree; only change it if the profile shows queries hot.

### 7. Sim and render overlap (OPT2 t05)
- Fixed-rate sim with rendering decoupled from it ("Fix Your Timestep"): render interpolates between the last two
  sim snapshots, at the cost of one tick of visual delay. With the snapshot handoff the renderer already uses, the
  sim tick can run on workers while the previous snapshot is drawn. This changes only presentation, so the CRC is
  unaffected.

### 8. Algorithmic wins over threads
- Factorio's robot check went from O(N) over 36,815 roboports to O(log N) over ~900 merged rectangles with a
  sorted, deduplicated structure, which made it "essentially free" without threads. Expect some OPT1 spikes to be
  fixed this way (caching, incremental recompute, better structures) rather than by parallelism.
- Flow fields (one integrated cost grid per goal, shared by every unit of a group) are the usual way RTS games
  move large groups cheaply. Not a first step here: the port's movement follows the original's A* and
  formation rules, and replacing them changes behaviour. Note for later only.

## Techniques for the five biggest costs (research, 2026-11-14)

Added after the 2026-11-14 measurements. The five costs, as Gabriel listed them: (1) `tac.ai.perception` (40% of a land battle
tick, 1.43 ms; 46% on water; 22% in space), (2) per-tick hashing of every system (`crc.battle.ai` etc., 16-24% of a battle tick),
(3) `ai.goals` (69% of a galactic tick, p99 4.4 ms, spikes to 22 ms), (4) single-thread spikes (`nebula_battle` movement p99 24 ms,
max 76 ms; one 190 ms AI-planning tick), (5) job-system overhead (galaxy 30% slower at 15 workers than at 0; battles gain
5-25%, all of it at 3 workers). The audit (`docs/PERF_AUDIT_2026-11-07.md`) already lists S1, S2, S5, S6, S7, S8 and J1-J3; this
section only adds to them. Evidence is thin: few sources are RTS-specific and none has benchmark numbers. Items marked
**(own reasoning)** are not from a source. Every item must still pass rule 3 and the parity tiers; "goldens" says whether it moves them.

### P1. Perception: keep a snapshot, emit changes
- After S6 (`TypeFacts[type]` table), the remaining cost is rebuilding and diffing the whole snapshot each tick. Give each object a
  change stamp (a tick number); at commit emit add / remove / change events in index order, so no sort-and-compare is needed.
- Same idea as Unreal's AI perception "sense only changes" mode (off by default there) and as fog-of-war guides: a dirty flag and
  version stamp per viewer, emit visibility changes instead of recomputing everything.
- Deterministic: stamps are tick numbers, order is index order. Goldens: none expected (same snapshot content); verify by CRC.
- The galactic perception parallel attempt failed because readers use results in the same frame ("Tried and not shipped" in
  `PERFORMANCE.md`); this change is serial and does not need that.

### P2. Hashing: do less of it in the game
- **(own reasoning)** In `alamo_game`, hash only on checkpoint ticks (every 64th, the golden v2 cadence) and at save points; keep per-tick
  hashing in `alamo_headless` and tests. Goldens: none. This extends audit S5 from the log to the hashing itself.
- Larger option: per-entity sub-hashes combined commutatively (Zobrist-style: remove the old contribution, add the new one when an
  entity changes). Mix the entity id in, or add modulo 2^64 instead of XOR, so equal entities do not cancel **(own reasoning)**;
  every field that affects behaviour must be covered; run a periodic full recompute as a drift check. Goldens: change.
- Sources agree the per-tick, per-system hash is the standard desync tool and need not be cryptographic; keep the per-system split in
  tests so a mismatch localises. No source gives overhead numbers.

### P3. Goal scoring: ids, then caching
- After S7 (equation ids once, stats as counters): general interning. Resolve names to integer ids at load time, keep text in a
  debug / report table only (Stingray resource ids, Unreal FName, Lengyel's compile-time hashing). Per-thread interners avoid heap
  contention in parallel builds.
- For the 4.4 ms p99 and 22 ms spikes **(own reasoning)**: cache each planet's score and re-score only when its inputs changed
  (dirty flags); fall back to the tick-number round robin of technique 2 for what cannot be cached.

### P4. Spikes
- 190 ms planning tick: S1 / S2 (restore plan-VM images, prewarm `pool_count` VMs) is the right fix. Prewarming pooled instances at load
  is the standard pattern; creating a Lua state is cheap, the cost is parsing the libraries, which is what S1 removes.
- `nebula_battle` movement: cap path requests per tick by a **fixed count or node budget, never wall-clock time** (a time budget breaks
  determinism), served round robin with the cursor saved in the save data. Sources suggest fixed-size batches and incremental A*.
  The cause of the 76 ms tick is **not known**; profile that tick before choosing. Shared flow fields stay a note for later
  (behaviour change, see technique 8).

### P5. Job system
- After J1-J3: Unity's scheduler batches jobs and flushes the batch once, so only the needed number of workers wake; waking workers
  that find no work is the expensive part. Blitz derives the grain size from the worker count with a floor, and runs the first
  chunk on the calling thread.
- A per-phase worker cap (battle phases at 3-4, the galaxy at fewer when the fork is small) is deterministic because outputs are per
  index **(own reasoning, from the measured 3-worker plateau)**.
- Waiting: `WaitOnAddress` / `std::atomic::wait` is fine, but Raymond Chen notes hand-built waits lack the spin counts and convoy
  resistance of Windows' own locks, so spin briefly with a pause instruction, then block (J2).

### Sources for this section
- [Unreal AI perception `bSenseOnlyChanges`](https://docs.unrealengine.com/4.27/en-US/API/Plugins/UE4ML/Sensors/UE4U4MLSensor_AIPerception/bSenseOnlyChanges/index.html);
  [fog-of-war dirty flags](https://skillselion.com/skills/pproenca/dot-skills/fog-of-war-js-ts)
- [Zobrist hashing](https://en.wikipedia.org/wiki/Zobrist_hashing);
  [lockstep determinism checker](https://bugnet.io/blog/how-to-build-a-lockstep-determinism-checker)
- [Compile-time string hashing (Lengyel)](https://gameenginegems.com/gemsdb/article.php?id=1278);
  [Stingray resource ids](https://help.autodesk.com/cloudhelp/ENU/Stingray-SDK-Help/sdk_help/extend_engine/about_resource_ids.html)
- [Object pooling against hitches](https://bugnet.io/blog/how-to-use-object-pooling-to-stop-instantiation-hitches);
  [raasta batch pathfinding](https://docs.rs/crate/raasta/1.0.0/source/src/batch.rs);
  [gamedev.net: time slicing for pathfinding](https://ehe.gamedev.net/forums/topic/627306-time-slicing-or-multithreading-for-pathfinding)
- [Unity: job system overhead (2022.2)](https://unity.com/blog/engine-platform/improving-job-system-performance-2022-2-part-2);
  [Blitz architecture](https://blitz-3nq.pages.dev/internals/architecture);
  [Raymond Chen on WaitOnAddress](https://devblogs.microsoft.com/oldnewthing/20201214-00/?p=104544)

## On the roadmap
These techniques are roadmap group **OPT3 "Techniques from the performance research"** (box OPT, 2026-10-30),
kept separate from OPT1 / OPT2. The notes below say how they narrow the OPT2 tasks.

## Suggested changes to the roadmap tasks
- OPT1 t01: in-house per-system tick timers; Tracy only as an optional developer build, if Gabriel approves the
  dependency.
- OPT2 t01: specify the schedule as tick-number round robin (`tick % N`), cursor saved in N1 chunks; never
  time-based.
- OPT2 t02: start by making galactic all-pairs `--latency` the default, then reuse the same snapshot / apply on
  N+k path for AI perception and planning.
- OPT2 t04: replace "incremental GC" with "staggered forced collections per VM plus parallel collection of
  independent VMs"; keep Lua 5.0.2's collector unchanged.

## Sources
- Allen Chou, [Game Programming: Time Slicing](https://allenchou.net/2021/05/time-slicing) and
  [Timeslicing Batched Algorithms in Games](https://allenchou.net/2017/03/timeslicing-batched-algorithms-across-multiple-frames/)
- [How to fix frame spikes when ticking thousands of sim agents](https://bugnet.io/blog/how-to-fix-frame-spikes-when-ticking-thousands-of-sim-agents);
  [How to fix AI performance with many agents](https://bugnet.io/blog/how-to-fix-ai-performance-with-many-agents)
- gamedev.net: [Time slicing or multithreading for pathfinding](https://gamedev.net/forums/topic/627306-time-slicing-or-multithreading-for-pathfinding/),
  [RTS lockstep commands synchronization](https://gamedev.net/forums/topic/419969-wargamerts-network-architecture-a-danger-of-lock-step-commands-synchronization/),
  [Immutable world state for parallelization](https://gamedev.net/forums/topic/619929-immutable-world-state-for-parallelization/),
  [Entity update determinism](https://gamedev.net/forums/topic/684448-entity-update-determinism/5322845/)
- Factorio: [Friday Facts #415](https://cf-www.factorio.com/blog/post/fff-415) (core-count desync, robot check),
  [FFF #215 multithreading issues discussion](https://forums.factorio.com/viewtopic.php?p=316253),
  [parallel processing discussion](https://forums.factorio.com/viewtopic.php?p=476526)
- [Knockout City's parallel entity system, GDC 2022](https://gdcvault.com/play/1027634/-Knockout-City-s-Parallel);
  [Parallelizing the Naughty Dog Engine Using Fibers, GDC 2015](https://gdcvault.com/play/1022186/contactUs)
- Bevy: [0.10 release notes (scheduling)](https://bevy.org/news/bevy-0-10/),
  [schedule docs](https://doc.qu1x.dev/bevy_trackball/bevy_ecs/schedule/index.html)
- Lua: [lgc.h 5.0](https://www.lua.org/source/5.0/lgc.h.html),
  [Ierusalimschy, Lua workshop 2018](https://www.lua.org/wshop18/Ierusalimschy.pdf),
  [lua-l on forcing GC per frame](https://lua-users.org/lists/lua-l/2001-09/msg00176.html),
  [Kalogirou, Predictable garbage collection with Lua](https://kalogirou.net/2011/07/22/predictable-garbage-collection-with-lua/),
  [OpenMW: run Lua GC in parallel with the frame](https://gitlab.com/OpenMW/openmw/-/merge_requests/5487),
  [Defold: avoiding GC hitches](https://forum.defold.com/t/solved-optimizing-frame-stability-avoiding-garbage-collector-related-hitches/70930)
- [Data-oriented design](https://en.wikipedia.org/wiki/Data-oriented_design);
  [Android: data-oriented game performance](https://android-developers.googleblog.com/2015/07/game-performance-data-oriented.html);
  [Unity DOTS RTS collision system (uniform grid)](https://github.com/unitycoder/Unity-DOTS-RTS-Collision-System)
- Fixed timestep: [Fix your timestep discussion](https://gamedev.net/forums/topic/691776-fix-your-timestep)
- Tracy: [wolfpld/tracy releases](https://newreleases.io/project/github/wolfpld/tracy/release/v0.14.0),
  [Flax Engine Tracy docs](https://github.com/FlaxEngine/FlaxDocs/blob/0ac60094d0312b8ef089dcbd32668cbe6cbb765c/manual/editor/profiling/tracy.md/)
- Flow fields: [Fieldrunners 2 flow field chapter](https://www.taylorfrancis.com/books/9780429055096/chapters/10.1201/9780429055096-8)

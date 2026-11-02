# Empire at War: Vulkan

A clean-room C++20 rewrite of the *Star Wars: Empire at War: Forces of Corruption* engine (Petroglyph's "Alamo").
The simulation is rebuilt from scratch as a headless, multithreaded, deterministic core. A Vulkan 1.3 renderer
sits on top of it. The goal is the game you already own, on an engine that uses a modern PC.

> **Status:** active development. The simulation, renderer, Lua layer, AI, UI and save/load are all in place and
> the game boots and plays through its own host (`alamo_game`). The remaining work is parity polish (checking
> behaviour against the original), mod loading and performance. Progress is on the project roadmap (`ROADMAP.html`)
> and in `docs/PHASE2.md`, `docs/PHASE3.md` and `docs/PHASE4.md`.

This repository contains **no game assets and no original code**. You need your own copy of the game
(Steam: *Star Wars: Empire at War - Forces of Corruption*). Behaviour was recovered by studying the shipped binary
and data, and written up as specs in `docs/re/` before any engine code was written.

---

## Goals

1. **Play the real game.** Base *Forces of Corruption* is the parity target: the same content, the same rules,
   the same feel. Parity is judged by behaviour (travel and build times, damage, income, AI choices, battle
   outcomes), not by matching the original's internal hashes.
2. **Keep the mods alive.** *Thrawn's Revenge* and other large FoC mods are the compatibility goal. The engine
   reads the original XML, Lua and asset formats unchanged, so mod content should load without conversion.
3. **Use the whole PC.** The original runs its simulation and rendering on one thread over Direct3D 9. This port
   spreads work across every core and drives the GPU through a modern, low-overhead API.
4. **Deterministic by design.** The same inputs give the same results on any number of threads, on any run, and
   on both supported compilers. That makes saves, replays and debugging trustworthy.
5. **Quality-of-life that the original never had.** Smooth motion at any refresh rate, modern anti-aliasing and
   ambient occlusion, rebindable camera and pause controls, and built-in profiling tools.

**Scope:** Windows x64 only, single player. Multiplayer, loading the original game's save files, and non-Windows
platforms are deliberately out of scope.

---

## What is new

### Renderer (Vulkan 1.3)
- Vulkan 1.3 with synchronization2, bindless textures and a persistent on-disk pipeline cache.
- The original game's shaders are translated to SPIR-V by an in-house translator plus DXC (all 291 shaders in the
  shipped data translate).
- Draw recording runs on the job system across all cores; draw lists are identical at any thread count.
- Terrain, water, decals, tracks, stencil shadows, fog of war, space backdrops, hardpoint destruction and a GPU
  particle system.
- Presentation-only options never feed the simulation, so turning them on or off cannot change a battle.

### Graphics and display options
- **Refresh-rate aware.** The sim ticks at 30 Hz; the views interpolate between ticks, so motion is smooth at 60,
  144 or 240 Hz instead of stepping.
- **V-sync modes:** FIFO, Mailbox (low latency) or off, with a frame-rate cap that also works with V-sync on.
- **Temporal anti-aliasing** with depth reprojection, per-object motion vectors and disocclusion rejection.
- **Screen-space ambient occlusion** in land battles.
- **Anisotropic filtering** up to 16x.
- **In-game frame-time overlay** (F11) showing sim ticks and frame phases against the 16.7 ms and 33.3 ms lines.

### Controls and camera
- Pause on both the galactic and tactical level, rebindable to a hotkey.
- Adjustable field of view, with separate values for the galactic and tactical cameras.
- Free tactical camera: full 360 degree rotation on all axes. W A S D moves the camera by default.
- **Shift+click order queues:** line up attacks, moves and abilities for a unit; a plain click replaces the queue.
  Queued orders are ordinary commands in the stream, so they replay deterministically.

### Simulation, AI and scripting
- Full object model (about 4,000 unit definitions, about 1,000 fields each) and the original behaviour set.
- Galactic and tactical simulation: economy, production, fleets, hyperspace, land and space combat, abilities,
  garrisons, fog of war, weather and special weapons.
- The original AI, including its Lua plans and perception, running on a vendored **Lua 5.0.2** with the original
  semantics.
- Story campaigns, including linked story battles.
- **Save and load** in the port's own compact format, with save/load menus and mid-battle saves. A Forces of
  Corruption story save is about 50-60 KB, and a mid-battle save about 145 KB.
- Skirmish, galactic conquest and the main-menu attract battle.

### Tooling
- `alamo_headless`: runs the whole simulation without a window, for testing, benchmarking and AI experiments.
- Per-tick state checksums with per-system breakdown, recorded and compared across thread counts and compilers.
- A tick profiler (`--profile`) reporting p50/p99/max per tick and per system.
- Visual regression checks (screenshot diffs against stored baselines).
- Tools for the original formats: MEG archives, ALO/ALA models and animations, shaders.
- Two compilers in the loop: MSVC builds the product, clang 20 checks it with extra warnings and sanitizers.
  The code builds with zero warnings on both, and the test suite (150+ tests) runs on every change.

---

## Performance

### What is different from the original
| | Original engine | This port |
|---|---|---|
| Simulation | One thread, run serially in the frame | Parallel jobs across all cores: movement, combat, spatial queries, pathfinding, perception, animation, galactic graph |
| Thread library | Intel TBB shipped but unused | Own work-stealing job system with order-stable results |
| Graphics API | Direct3D 9 | Vulkan 1.3, bindless, multithreaded recording |
| Lua garbage collection | Collects whenever a VM hits its threshold, mid-script | Collections scheduled by tick number and run in parallel; most moved out of script calls |
| Frame pacing | Fixed 30 Hz steps shown as-is | Interpolated to the display refresh rate |

### Measured results
All numbers are from this project's own profiler on a Ryzen 7 7800X3D (8 cores, 16 threads), MSVC `/O2`, at
30 ticks per second (one tick is 33.3 ms). They come from `docs/PHASE4.md`, which also lists the scenarios.

**Job system scaling** (`logs/bench.txt`, 16 logical processors, results identical at every worker count):
- Dijkstra over a large graph: 7.9x at 8 workers, 12x at 15
- 16-million-element float reduction: 16.9x

**Tick time on real Forces of Corruption content:**

| Scenario | Mean | p50 | p99 |
|---|---|---|---|
| Galactic conquest, Heart of the Maw, every faction an AI (18,000 ticks) | 0.21 ms | 0.19 ms | 0.52 ms |
| 400-planet synthetic galaxy, 4 AIs | 0.48 ms | 0.39 ms | 2.99 ms |
| Land battle (2 AIs) | 3.42 ms | 3.06 ms | 6.39 ms |
| Space battle with station | 1.02 ms | 0.95 ms | 2.17 ms |

The budget is a p99 below 16.7 ms (half a tick), and every scenario meets it. On the 400-planet galaxy the
parallel run is 2.4x faster on mean and 5x on p99 than the serial run.

**Optimisation pass (OPT3):** results stayed byte-identical (checked against the CRC logs at 0/1/4/16 workers).

| Measure | Before | After |
|---|---|---|
| Land battle, tick p50 | 6.83 ms | 3.85 ms |
| Land battle, end-of-tick work (checksum) p50 | 3.14 ms | 0.87 ms |
| Land battle, tactical-AI checksum p50 | 2.06 ms | 0.73 ms |
| Space battle, tick p50 | 2.41 ms | 1.59 ms |
| Space battle, end-of-tick work p50 | 0.88 ms | 0.35 ms |
| Space battle, movement p99 | 6.43 ms | 3.68 ms |
| Land battle start-up: placing map objects | 73 ms | 2.4 ms |
| Land battle start-up: obstacle stamps and placements | 23.6 ms | 1.5 ms |
| Land battle start-up: region build | 42 ms | 20 ms |

(Model loading for a battle now runs in parallel before the first tick instead of inside each spawn.)

**Lua garbage collection:** 70-85% of collections moved out of script calls to a fixed point in the tick. The
worst collection that still lands inside a script call dropped from 1.4-4.2 ms to 0.5-2.8 ms across the three
scenarios. Collection was never a large cost here (under 0.5% of tick time), so this is about removing spikes, not
raising the average. The "before" times were measured on a busy PC, so they are upper bounds.

**Renderer:** with 2,000 ships on screen (2,502 draws), draw-list build time went from 0.88 ms to 0.67 ms and
sorting from 0.19 ms to 0.09 ms. TAA costs about 0.10 ms of GPU time at 720p, and SSAO about 0.4 ms at 1080p.

### An honest caveat
There is **no side-by-side benchmark against the original executable yet**. The comparison table above describes
how the architectures differ. The measured numbers compare this port with its own earlier builds. The test battles
are two-AI skirmishes, not the largest scenes the game can produce, so the budget will be re-checked as bigger
scenes are added. A repeatable benchmark (fixed saves, recorded orders and a scripted camera) is on the roadmap
and will make a direct comparison possible.

---

## Building

Everything is built with a project-local toolchain under `toolchain\` (MSVC 14.51, Windows SDK, CMake, Ninja,
DXC). Nothing is installed system-wide.

```bat
:: one-time: set up the toolchain (see toolchain\2_setup.cmd)
build_msvc.cmd        :: configure, build and run all tests with MSVC
build_clang.cmd       :: the clang 20 check build
build_gate.cmd        :: MSVC + clang + sanitizer gate used before merging
```

Data-driven tests read `ALAMO_FOC_DATA` and `ALAMO_EAW_DATA` (folders containing `Data\XML`). The build scripts
default them to a Steam install when it exists; tests skip when the data is missing.

Useful entry points:

```bat
alamo_game.exe              :: the game
alamo_game.exe --frame-overlay
alamo_headless.exe --profile --economy --galactic-ai --ticks 18000
alamo_render.exe            :: renderer test app
```

## Repository layout

| Path | What |
|---|---|
| `engine/` | The engine, one static library per module: `core`, `vfs`, `data`, `jobs`, `galactic`, `assets`, `rhi`, `materials`, `shaderxlat`, `terrain`, `render`, `objects`, `tactical`, `ai`, `lua`, `anim`, `commands`, `audio`, `video`, `ui`, `apps` |
| `tools/` | MEG archive tool, format readers, shader tools, probes, visual diff |
| `docs/` | Phase status boards, behaviour specs recovered from the original (`docs/re/`), asset and format specs |
| `ROADMAP.html` | The project roadmap and flowchart (generated; update with `roadmap-store/roadmap_store.py`) |
| `CLAUDE.md` | Working rules for contributors and agents |

## Rules of the road
- Zero warnings at MSVC `/W4 /permissive-` and clang `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`.
- Determinism: results the game reads must be bit-identical across thread counts, runs and compilers.
- No new third-party dependencies in `engine/` without discussion. Current ones: Vulkan-Headers, DXC, VMA, Lua 5.0.2,
  miniaudio and FFmpeg's Bink decoder (LGPL, dynamically linked).
- Clean-room: engine code is written from the specs in `docs/re/`, not from decompiled output.

## Legal
*Star Wars* and *Empire at War* are trademarks of their respective owners. This is an unofficial fan project, not
affiliated with or endorsed by Lucasfilm, Disney, Petroglyph or any other rights holder. You must own a legitimate
copy of the game to use it.

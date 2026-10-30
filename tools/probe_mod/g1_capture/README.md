# G1 in-game capture kit

> **2026-10-29 (Gabriel): RenderDoc is out of scope.** Skip run 2 (`run_2_renderdoc.cmd`); G1 uses runs 1 and 3 (DXVK, apitrace). Checks listed below as RenderDoc-only are answered from the apitrace dump / d3dretrace snapshots or dropped (roadmap G1 t04); the table below says which.

> **2026-10-29: the patch chain is no longer installed.** Since some time between 2026-10-28 19:38 and 2026-10-29 22:44 (outside the kit: no kit run or cleanup ran in that window), `corruption\` holds no `d3dx9_43.dll`, `d3d9.dll`, `dxgi.dll` or `eaw_*` files. `status` then reports `"state": "no_chain"`, and the kit only adds DXVK's `d3d9.dll` (dxvk) or apitrace's wrapper (apitrace) and removes it again afterwards. Runs from that state are parity captures without any change to `d3dx9_43`.
>
> **Menus are driven by the kit** (`--drive K,E --probe-mod admonitor`): see "For an agent" below. No hands needed.

Roadmap step G1 "In-game capture", approved by Gabriel on 2026-10-27. This kit runs the capture plan in
`docs/re/RENDER.md` §7.2 against the real game. It uses three tools: DXVK HUD, RenderDoc and apitrace. The
prior patch chain (`docs/re/PRIOR_PATCH.md`) is switched off during each run, so the captures show the original game.

**What the kit changes, and how it puts it back.** The kit only touches files directly inside the game's
`corruption\` folder:

| Run | `d3dx9_43.dll` | `d3d9.dll` | `dxgi.dll` |
|---|---|---|---|
| dxvk, renderdoc | replaced by a copy of `eaw_modifier_d3dx_original.dll` (the genuine Microsoft d3dx9_43; this switches off the patch chain) | kept: it is already DXVK 3.1.1, byte-identical to the pinned copy | kept |
| apitrace | same as above | replaced by apitrace's D3D9 wrapper | moved aside |

- Before it changes anything, the kit records the SHA-256 of every file in `corruption\` and backs up each file
  it replaces. This goes into `out\state\manifest.json` and `out\state\backup\`.
- After the game exits, the kit always restores the folder, even after a crash. It then checks every file
  against the recorded hashes. The game's own log files (`_*.txt`, `*.log`, `temp.rec`) are allowed to differ.
- If something is ever left behind, `cleanup.cmd` repairs it. It is safe to run as often as you like.

## For Gabriel: step by step

Each run takes **10–20 minutes**. Steam must be running. The game starts in a 1920×1080 window.

1. Double-click **`status.cmd`**. It should say the game was found and `"state": "enabled"` (the prior patch
   chain is on) or `"no_chain"` (nothing installed; fine too). Close it.
2. In the game's Options, check that **Bloom**, **Heat Distortions** and **Soft Shadows** are on and that Shadow
   detail is high. If you change them, note what they were so you can set them back later.
3. **Run 1: DXVK (about 10 min).** Double-click **`run_1_dxvk.cmd`**. The window lists what to do. In short:
   1. Start a space skirmish on **Kuat** as the Empire with maximum credits.
   2. Build **Thrawn's Star Destroyer** (Admonitor) and keep it on screen while it's lit, with the planet behind it.
   3. Then start a land skirmish on **Endor** with units next to a building.

   The kit takes a screenshot (including the DXVK numbers) every 15 seconds. Wait at least 30 seconds in each
   scene. Once, set Shadow detail to low, wait 30 seconds, then set it back to high. Quit the game normally.
4. **Run 2: RenderDoc (about 20 min).** Double-click **`run_2_renderdoc.cmd`** and play the same scenes. Press
   **F12** once at each of these moments:
   - Thrawn's Star Destroyer close up
   - ships firing or exploding
   - the whole battle zoomed out
   - Endor with Soft Shadows on
   - Endor with Soft Shadows off
   - a scorch mark or build pad on the ground

   Quit the game normally. The kit then extracts the captures, which takes a few minutes.
5. **Run 3: apitrace (about 10 min + analysis).** Double-click **`run_3_apitrace.cmd`**. **Keep it short.** Go
   straight to the Kuat battle. As soon as Thrawn's Star Destroyer is on screen, keep it there for 10 seconds,
   then quit. The game will be slow while it is traced. The analysis afterwards can take 10–30 minutes, so leave
   the window open until it says *Finished*.
6. If a window says *Finished with exit code …*, or the game crashed, double-click **`cleanup.cmd`**.
7. Tell Claude the runs are done. You don't need to send anything: everything is in
   `tools\probe_mod\g1_capture\out\runs\`. The short results are in `analysis\report.md` in each run folder.
8. Set your graphics options back if you changed them.

**Optional (only if asked): planet spin.** This is a fourth apitrace run: 5 minutes at Kuat without touching
anything. The trace is several GB.

## What each capture answers, and how the result is read

`g1_analyze.py` writes `analysis\facts.json` and `analysis\report.md` into each run folder. An RE/doc agent reads
those files and records the results in `docs/re/` (prose only, per CLAUDE.md).

| Open question (source) | Capture / scene | Read from | How |
|---|---|---|---|
| Engine cull mode: `engine_device_defaults()` assumes CULLMODE=CW (PHASE3.md "Needs Gabriel") | apitrace, K/E | `facts.apitrace.*.cull` | `first_set` is the first `SetRenderState(D3DRS_CULLMODE)`. With `draws_before: 0` it is the engine's device default. `at_draw` gives the value in effect at every draw. |
| Per-draw cull (was RenderDoc) | apitrace, K | `focus.cull_by_shader` | **Replaced:** CULLMODE in effect at each draw, grouped by the shader pair (`--focus-frames A-B`). |
| StencilDarken variant and stencil function (RENDER.md §7.1 q2, answered statically; confirm) | apitrace / RenderDoc, E | `stencil_states_at_draw` / `stencil_states` | Expected: NOTEQUAL, ref 0, read mask 0x4F (hard shadows) or 0x3F (soft); z-fail INCR/DECR on volumes. |
| Blue cast on the lit Thrawn SD: Colorization / team-colour default (PHASE3.md) | apitrace, K | `colorization_values_at_draw`, `colorization_examples`, `texturefactor_at_ff_draw` | The value of the shader register named `Colorization` (from the CTAB in the dump's disassembly) at every draw that uses it. Compare with the MP palette blue (0.306, 0.588, 0.929, 1) and the FXO defaults (0,1,0,1) / (0,0,0,1). |
| Final pixel colour (was RenderDoc) | apitrace, K | `final_images` (`final_<call>.png`, `lit_mean_rgb`, `blue_ratio_lit`) | **Replaced:** `d3dretrace -b --snapshot` at the last draw before `EndScene` of each `--seq-frames` frame. Compare with an `alamo_render` image of the same view. |
| Heat target 0x80 clear → about a half-texel bias (PHASE3.md) | apitrace, K | `heat_passes_found`, `heat_shift_apitrace` | Finds `Clear(0x008080FF)` → StretchRect → the full-screen strip (SceneHeat). d3dretrace takes snapshots just before and after that draw, and the kit measures the image shift (0.05-px search). Expected for an untouched distortion texel: (128/255 − 0.5) × 2 × 0.25 = 0.00098 uv (≈1.9 px at 1920) plus SceneHeat's −½ texel. The `t1_ati` technique on AMD halves the V term. |
| Space frame order / pass and clear order (was RenderDoc) (SPACE_AND_OPEN.md §C) | apitrace, K | `frame_sequences`, `set_render_target`, `stretch_rect_pairs` | **Replaced:** per frame (`--seq-frames`) the Clear / StretchRect / SetRenderTarget / draw-run / Present sequence. |
| Shadow volumes cast in space (SPACE_AND_OPEN.md C.4) | DXVK, K, shadow detail high vs low | `shots\*.png` (HUD "Draw calls") | Read the draw-call difference from the screenshots. apitrace `stencil_states_at_draw` during K shows any volume draws. |
| Render targets: 4 × A8R8G8B8, two at ¼ size; bloom chain (RENDER.md §1.5) | apitrace / RenderDoc | `render_targets` | Sizes and formats of the targets created. |
| Skydome (was RenderDoc) (SPACE_AND_OPEN.md C.3) | apitrace, K | `frame_sequences` (draws before the first depth clear), `final_images` | **Replaced:** the background draws are the run between the full clear and the depth+stencil clear; the final snapshots show the result for a visual comparison with `alamo_render` Kuat images. The rotation order is not read numerically. |
| Galactic camera (B.8, answered; confirm) / no bloom or heat in galactic | RenderDoc / DXVK, G | passes, HUD | No heat clear and no ¼-size bloom passes should appear in the galactic capture. |
| Planet clip wraps at 239 or 240 frames (PHASE3.md Planet) | apitrace, P (optional) | `planet_candidates` | For shaders with `UVOffset` (Planet.fx): the `m_world` rotation angle in each frame. `steps_per_turn` = 360° ÷ the median step, i.e. sim ticks per turn. |
| Decal atlas corner (PHASE3.md assumptions) | — | — | **Dropped:** needs vertex-buffer contents per draw (Lock/Unlock blobs), which the analyser does not decode; the static answer (DECORATIONS.md) stands. |
| Legacy vertex colour (SPACE_AND_OPEN.md D.4, answered; confirm) | apitrace, K | `focus.colour_input`, `focus.ff_draws` | **Partly replaced:** which draws have COLOR0 in their layout and the FF state they run under (LIGHTING, COLORVERTEX, DIFFUSEMATERIALSOURCE). The COLOR0 values themselves are vertex-buffer contents and are not read (same reason as the decal row). |
| Fog source | — | — | Already answered (SPACE_AND_OPEN.md B.1). No capture needed. |

The apitrace dump also keeps the full call stream (`apitrace dump out\runs\<run>\eaw.trace`). Use it for the
effect-list order question (RENDER.md §7.1 q10).

## For an agent: unattended runs

Use the project venv. Every mode is non-interactive with `--non-interactive`: no prompts, exit codes, and a JSON
status file.

```
set PY=toolchain\venv\Scripts\python.exe
set G1=tools\probe_mod\g1_capture\g1.py
%PY% -I %G1% status                                   # JSON: game, patch chain, d3d9.dll, active run, steam_running
%PY% -I %G1% renderdoc --non-interactive --timeout 1500   # start in the background; returns after the game exits
%PY% -I %G1% trigger --label kuat_thrawn             # capture now (renderdoc: next frame; dxvk: screenshot; apitrace: marker)
%PY% -I %G1% quit                                     # WM_CLOSE to the game, forced kill after --grace (20 s)
%PY% -I %G1% cleanup                                  # restore; safe any number of times
%PY% -I %G1% analyze --run-dir tools\probe_mod\g1_capture\out\runs\<run>
```

Useful options:

| Option | Effect |
|---|---|
| `--dry-run` | Prints every planned change and the launch line, and changes nothing. |
| `--auto-trigger-sec 60 90 --auto-labels a b` | RenderDoc captures on a timer. |
| `--shot-interval N` | DXVK timer screenshots. |
| `--extra-args "..."` | More game arguments. |
| `--start-steam` | Starts Steam if it is not running. |
| `--keep-patch-chain` | Captures with the patch chain on (not a parity capture). |
| `--drive K,E` | Drives the menus itself (`g1_ui.py`, ctypes only): runs each scene's `auto` steps from `scenes.json` (clicks in 1920×1080 client space, `expect_like` checks against the crops in `refs\`, screenshots `shots\ui_*.png`), then quits the game. A failed step saves `FAIL_<scene>_<n>.png` and ends the run normally (restore as usual). |
| `--ui-method post` | `post` (default): PostMessage to the game window, no focus needed. `input`: SendInput. While in a battle the real cursor is parked at client (960,150), because the game edge-scrolls on the real cursor position. |
| `--drive-keep-open` | Leaves the game running after the steps. |
| `--probe-mod admonitor` | Starts the game with `MODPATH=` a mod built under `out\mods\g1_admonitor` (never in the game folder): `STARBASES.XML` from `Data\config.meg` with `Admonitor_Star_Destroyer` added to `Skirmish_Empire_Star_Base_1`, so Thrawn's SD can be built at station level 1. |
| `--game-dir` | Overrides game detection. Otherwise the order is `ALAMO_GAME_DIR`, then `F:\SteamLibrary\…`, then the Steam library folders. |

Results:

- **Status file:** `out\runs\<run>\status.json`, also copied to `out\last_status.json`. It holds `result`,
  `exit_code`, `error`, `warnings`, `actions` (the manifest entries), `launch` (`timed_out` and `forced`),
  `captures`, `screenshots`, `markers`, `cleanup` (`verified`, `mismatches`, `game_written`) and `analysis`
  (the path of `report.md`).
- **Exit codes:**
  - 0 ok
  - 2 game not found
  - 3 unexpected `d3dx9_43.dll` state
  - 4 tool missing (run `toolchain\6_capture_tools.cmd`)
  - 5 Steam not running
  - 6 the game never started
  - 7 restore not verified (manifest and backups kept)
  - 8 no capture produced
  - 9 another run is active

The kit cleans up after itself. A run that is killed leaves `out\state\manifest.json`. The next run, or
`cleanup`, restores it first.

**What is automatic, and what needs UI driving:**

- **Automatic:** game detection, patch-chain disable and restore, DLL placement, launch (windowed 1920×1080),
  DXVK HUD and log, timer or requested screenshots, RenderDoc injection and captures (no hotkey needed),
  apitrace tracing and markers, timeout and close, restore with hash verification, RenderDoc extraction, and
  the analysis. Analysis covers cull, stencil, Colorization, clears, render targets, heat shift and planet steps.
- **Needs UI driving (computer use):** getting to the scene. No documented command-line argument loads a map
  in this build. A quick-load-map path exists in the exe (0x68b20), but what triggers it is not in
  `docs/re/`. If an RE pass finds it, pass it with `--extra-args`.
  - The exact clicks per scene are in `scenes.json` under `ui_steps`: screen, button text from the English
    MasterTextFile, and the expected next screen. Logo movies are skipped with Escape.
  - Find each label on a screenshot before clicking, because positions depend on resolution.
  - Changing graphics options (Soft Shadows, Shadow detail) is also UI driving. Put them back afterwards.

## Tools (toolchain\6_capture_tools.cmd, called from 2_setup.cmd)

| Tool | Version | Source | Size | SHA-256 |
|---|---|---|---|---|
| DXVK | 3.1.1 | https://github.com/doitsujin/dxvk/releases/download/v3.1.1/dxvk-3.1.1.tar.gz | 18,041,512 | `40565B4A724AADC4433FA4E010B4B23916D9B1F1BAEEE64E17186DB94F54E608` (= GitHub's published digest) |
| RenderDoc | 1.46 portable | https://renderdoc.org/stable/1.46/RenderDoc_1.46_64.zip | 101,000,153 | `9CA4D09ECABA2CC791168660D6FC2A7E3D70FE67146E87EC05C5F8DEA70772F7` (pinned on first download; `renderdoc.dll` Authenticode signature checked = Valid) |
| apitrace | 14.0 win64 | https://github.com/apitrace/apitrace/releases/download/14.0/apitrace-14.0-win64.7z | 34,802,182 | `32C702683770BA00B06A12A95FE1AFDD7D7353CF519ADF2073E6433F78147DC3` (= GitHub's published digest) |

Notes on the tools:

- The game's existing `corruption\d3d9.dll` and `dxgi.dll` are byte-identical to DXVK 3.1.1 x64.
- RenderDoc is never registered system-wide. Its Vulkan layer is enabled per process with `VK_ADD_LAYER_PATH`
  and `VK_INSTANCE_LAYERS`.
- On its first start, qrenderdoc asks a modal analytics question. The kit answers it the privacy-preserving way
  (opt out, no update checks) in RenderDoc's own per-user `%APPDATA%\qrenderdoc\UI.config`.
- Other Vulkan implicit layers on this PC are active in the game too: RTSS, CapFrameX, OBS, Steam overlay, Epic
  overlay. If a capture misbehaves, close those programs first.

## Files

| File | What it does |
|---|---|
| `g1.py` | The kit: modes, manifest, restore and verification, launch and monitoring, status JSON. |
| `run_1_dxvk.cmd`, `run_2_renderdoc.cmd`, `run_3_apitrace.cmd`, `cleanup.cmd`, `status.cmd` | Double-click wrappers. Set `G1_NONINTERACTIVE=1` to skip the final pause. |
| `g1_ui.py` | Menu driver (ctypes only): window fit, PostMessage / SendInput clicks and keys, PrintWindow screenshots, `expect_like` checks against `refs\*.png` crops. CLI: `g1_ui.py shot|click|key|like …`. |
| `g1_options.py` | `g1_options.py --set Bloom=1 HeatDistortion=1 -- <g1.py args>`: backs up the player's FoC graphics options (HKCU `Software\Petroglyph\StarWars FOC\Profiles\Profile0\Player`, REG_DWORD) to `out\state\options_backup.json`, sets them, runs g1.py, restores them in a `finally` and reads them back (exit 7 if not verified). `--restore` repairs after an interruption. On 2026-10-29 the player's settings were Bloom 0, HeatDistortion 0, SoftShadows 1, ShadowDetail 3, ShaderDetail 3. |
| `rdoc_controller.py` | Runs inside qrenderdoc. Launches the game with RenderDoc injected, takes captures on request, timer or F12. |
| `rdoc_extract.py` | Runs inside qrenderdoc. Writes the per-capture `*.summary.json` and the PNGs. |
| `g1_analyze.py` | Analysis: apitrace dump state machine, d3dretrace snapshots, RenderDoc summaries, DXVK logs. |
| `scenes.json` | Scenes, manual steps, `ui_steps` for computer use, trigger labels. |
| `selftest\` | `run_selftest.cmd` (about 3 min) runs every mode against a fake game folder built from `fake_d3d9_game.cpp`, a small D3D9 app with the same call shapes. It writes `calibration.json`. Last result: 37/37 checks passed (dry run changes nothing; each mode restores byte-identically; a run killed mid-way is restored by `cleanup`; a second `cleanup` is a no-op). |
| `out\` | Created by runs: `runs\<mode>_<time>\`, `state\` (manifest and backups while a run is active), `control\` (request files), `last_status.json`. |
